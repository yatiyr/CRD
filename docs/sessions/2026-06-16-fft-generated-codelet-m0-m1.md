# 2026-06-16 — FFT: AVX2 large-N archaeology close + Fork A generated-codelet M0/M1

> Module: `crd-hesap-fft` (v10). Continues `2026-06-15-fft-small-n-engine-crush.md`.
> Design doc: `docs/design/hesap_fft_generated_codelets.md`. Measured history: `docs/research/fft-mkl-crush.md`
> (FINAL SUMMARY + C-22/C-23). Memory: `project_v10_fft_plan` (Part 29).

## 1. AVX2 large-N C2C archaeology campaign — CLOSED with FOUR banked wins

The 2M–16M complex-FFT campaign began with a *premature* "AVX2 ceiling" conclusion. Black-box + internal
**archaeology** (study MKL's behavior → find the structural cause → build the closest legal fix → DoD) overturned
it and produced four real engine wins:

| win | result | mechanism | commit |
|---|---|---|---|
| larger-factor-first split (`m_n1 = 2^ceil`) | +9–13% large-N | fixed the odd-log2 split asymmetry (smaller factor was first) | `d5ae96d`/`d0f2484` |
| f32 non-temporal scatter store | +~5% f32 | removed the RFO / write-allocate penalty (NT path was f64-only) | committed |
| f32 bb-axis SIMD twiddle | (folded into f32 gains) | vectorize over the batch/column axis — contiguous read, transposed L2-band write, no gather | committed |
| f64 bb-axis SIMD twiddle | +5.6% f64 | the f64 mirror — proves bb-axis twiddle is a SHARED substrate, not f32-only | **UNCOMMITTED** (working tree) |

**Scoreboard:** f64 8M **0.76→0.84×**, f32 8M **0.61→0.78×** MKL (single-thread, core-pinned, MKL_NUM_THREADS=1,
AVX2 i9-14900K; all correct f64 ~1e-15 / f32 ~3e-7). Parity gap closed f64 ~33%, f32 ~44%. **Local patching is now
closed** — the residual sub-FFT gap (≈3.6× over its own flop floor, port-bound on shuffles + spills) is
bound-analysis-proven to need genfft/SPIRAL-class generated codelets, not a source tweak.

## 2. Fork A — the Generated Codelet / Planner Project (opened)

User chose Fork A: a deliberate multi-milestone generated-codelet/planner project (genfft/SPIRAL-style). Generator
`build/gen_subfft.py` = split-radix(2/4) DIT DAG + CSE + **numpy self-check at generation** + the `_schedule`
register-pressure list scheduler (loads-late / stores-early / kill-inputs-first) → straight-line **batched Vec4d AoS
codelets** over the batch axis, engine-native element-major layout, **no gather**. All probes live in gitignored
`build/`; no engine integration until a size wins ≥1.10× sub-FFT AND ≥1.05× full 8M.

## 3. M0 ✅ + M1 ✅ — the decisive crossover

Generated batched Vec4d codelets vs the engine's `execute_batched`, f64 b=32, all numpy-self-checked + machine-eps
vs MKL:

| N | engine ns/el | generated ns/el | speedup | DAG nodes | spills | compile |
|---|---|---|---|---|---|---|
| 16 | 0.221 | 0.470 | 0.47× (hand-tuned `small_n` lane-trick wins — expected) | 97 | low | fast |
| 32 | 1.066 | 0.537 | **1.98×** | 241 | low | fast |
| 64 | 1.285 | 1.033 | **1.24×** | 577 | low | fast |
| 256 | 1.726 | 1.746 | 0.99× | 3073 | 4454 | — |
| 512 | 1.948 | 1.860 | 1.05× | 6913 | 9887 | — |
| 1024 | 2.353 | 2.564 | **0.92×** | 15361 | **21996** | **574 s** |

⭐⭐ **Crossover at N≈64.** The full straight-line codelet beats the engine to N=64 (the generated scheduled
split-radix > compiler-scheduled generic radix at the leaf), then collapses: the DAG vastly exceeds the 16-ymm
register file, spills explode (21996 @1024), the codelet loses, and the compile becomes impractical (574 s). This is
the genfft lesson confirmed by measurement — **straight-line ONLY for small leaves; larger sizes need recursive
composition.**

## 4. Path decision for the 4096 sub-FFT → Path B (hierarchical 64×64)

| path | for | against | verdict |
|---|---|---|---|
| A full-4096 straight-line | wins ≤64 | ≈4× the 1024 ⇒ catastrophic spills + ~40 min compile | ✗ REJECTED |
| **B hierarchical 64×64** | **generated-64 leaf 1.24×; 4096 = 64×64; fits registers** | needs a 64×64 transpose between stages | **✓ CHOSEN** |
| 256×16 | — | 256 already neutral | ✗ |
| stage-Stockham | removes generic overhead | radix-8 near the gcc limit (prior audits) | △ |

Build 4096 = 64×64 from the generated-64 leaf (the proven 1.24× building block) + the already-vectorized bb-axis
twiddle + a 64×64 transpose. ⚠ **The N=64 leaf win does NOT automatically compose — M2 must prove it.**

## 5. NEXT — M2 (fresh-context)

Hierarchical 64×64 4096 sub-FFT POC. Standalone f64 `build/` probe; baseline = current `execute_batched(4096)`.
Acceptance: 4096 sub-FFT ≥1.10× → compose into full 8M; full 8M ≥1.05× → keep behind a gate; else diagnose/revise
(never revert to full straight-line). The 8-step plan + the "if both variants lose" diagnosis ladder + the strict
non-goals are in `docs/design/hesap_fft_generated_codelets.md` §16 and the phase doc v10 block.

## 6. Bank / clean status

- 3 wins committed (`d5ae96d`, `d0f2484`); **f64 bb-axis SIMD twiddle UNCOMMITTED** — working-tree
  `engine/hesap-fft/include/crd/hesap/fft/fft.hpp` (+65 lines). **➜ User commits before M2** (agents never commit).
- M0/M1 generator artifacts isolated in gitignored `build/` (`gen_subfft.py`, `gen_bench.cpp`, `gen_codelets.hpp`).
- Engine hot path clean (only the f64 twiddle delta). 4-win DoD (debug/asan/shipping/tidy 17/17) was green at C-22;
  re-run before any M2 integration.

## 7. Proposed commit message (for the f64 twiddle, the user runs it)

```
perf(hesap-fft): f64 bb-axis SIMD twiddle for the four-step large-N path

Vectorizes the four-step column twiddle over the batch axis (Vec4d), the f64
mirror of the shipped f32 bb-axis twiddle — proving bb-axis twiddle is a shared
substrate. +5.6% f64 8M (0.80→0.84x MKL), correct to ~1e-15, deterministic.
DoD: win-debug/asan/shipping/tidy green; [fft] gate machine-eps.
```
