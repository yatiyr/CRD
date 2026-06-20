# Hesap FFT — Generated Codelet / Planner Project (design)

> Status: OPEN (substrate project, multi-milestone). Opened 2026-06-16 after the AVX2 local-patch campaign closed
> with four banked archaeology wins (split, f32 scatter NT, f32 SIMD twiddle, f64 SIMD twiddle) lifting f64 8M
> 0.76→0.84× and f32 8M 0.61→0.78× MKL. The remaining sub-FFT gap is bound-analysis-proven to need genfft-class
> codegen, not a local patch. Fork A chosen by the user. Full measured history: `docs/research/fft-mkl-crush.md`.

## 1. Problem statement
Match (or beat) MKL single-thread AVX2 large-N C2C FFT. After four wins the residual is concentrated in the
**sub-FFT** (the per-block batched radix-8 inside the four-step). It is compiler-scheduled generic radix-8 and is
**3.6× above its own flop floor** — port-bound on shuffles + spills, not flops. MKL's sub-FFT is ~2.6× faster.

## 2. Current sub-FFT baseline (4096, the four-step block kernel)
| dtype | kernel | ns/el | cyc/el (~5GHz) | % of full 8M | known bottleneck |
|---|---|---|---|---|---|
| f64 | current 4096 sub-FFT | 2.71 | ~13.5 | ~46% | shuffles(160)/spills(110)/L2 |
| f32 | current 4096 sub-FFT | 1.33–1.34 | ~6.7 | largest remaining f32 phase | Vec8f but still overhead |

Floors: radix-8 flop floor ~3.75 cyc/el; split-radix ~3.0; MKL ~5.25 cyc/el. **CRD's 9.75 cyc/el over the floor is
the lever** (shuffles + spills, both at gcc's limit for hand-written/templated AoS radix-8).

## 3. Why local patches are closed (measured, not asserted)
Every bounded experiment was run and measured-negative: stage-specialized radix-8 reschedule (overhead
batch-amortized; A2 reorder made spills *worse*), split-radix-alone (~+6%, flops are only 28% of cyc/el),
SoA-split sub-FFT (de/interleave + bitrev cost > shuffle saving; shuffles are port-parallel = +10% max),
batch-width (b16≈b32 optimal). See dossier C-11..C-21.

## 4. Why a generator is justified
Closing the 9.75 cyc/el overhead requires attacking **shuffles + spills + flops simultaneously** — a SoA-friendly
schedule with optimal register allocation and split-radix flop reduction. No single source-level patch does this
(each prior patch fixed one axis and the others dominated). A code generator that searches schedules and emits
register-allocated straight-line codelets is the genfft/SPIRAL approach MKL/FFTW use. The small-N AoS lane-trick
codelets (N≤32) already CRUSH MKL — proof a generator path works in this codebase.

## 5. Target kernels
- f64 4096 sub-FFT (the four-step block kernel), engine-native element-major batched layout; AVX2 (Vec4d).
- f32 4096 sub-FFT (Vec8f) — port after f64.
- Ladder: N=64 (sanity) → 256 → 512 → 1024 → 4096.
- The current `execute_batched` remains the fallback; the generated kernel is gated/dispatched by size+dtype.

## 6. IR / schedule representation
A codelet = an op-DAG over complex values (`load`, `add`, `sub`, `mul_tw` const-twiddle, `neg/mulj/mulnj` ±1/±i),
built by recursive split-radix Cooley-Tukey, CSE'd, **numpy-self-checked at generation** (a fast wrong codelet is
worthless). The seed exists: `scripts/gen_fft_codelets.py` (split-radix DAG + CSE + numpy self-check + the
register-pressure list scheduler `_schedule` that already bought +22% on the radix-8-over-k twiddle path).

## 7. Cost model (flops are NOT the only term)
Per candidate schedule estimate: flop count, **shuffle count** (deinterleave/interleave per stage), **peak live
values** (→ predicted spills over the 16-ymm file), L1 temp stores, predicted cycles ≈ max(port pressures). Calibrate
constants against measured codelets. The current kernel's lesson: flops 28% of cyc/el → optimize shuffles+spills+flops
jointly, weighted by measured port throughput (port-5 shuffles vs port-0/1 FMA run in parallel).

## 8. Register-allocator strategy
The `_schedule` list scheduler (greedy: kill most inputs first, defer loads, prefer short-lived results) collapses
peak live so codelets fit 16 ymm without spilling — measured 76→7 spills on the scheduled N=64 atom. Extend it to
the batched layout + emit explicit L1 temporaries only where controlled stores beat compiler stack spills.

## 9. SIMD lane mapping
Batch axis = the SIMD lanes (Vec4d f64 / Vec8f f32), element-major so adjacent transforms are contiguous (the
engine-native layout; NOT the rejected SoA-split or transform-major atom). Deinterleave once per load via the
proven `load_complex_deinterleaved` helpers.

## 10. Twiddle strategy
Combine-twiddles as compile-time constants in the codelet (split-radix internal twiddles) + the four-step column
twiddle handled by the already-shipped bb-axis Vec4d/Vec8f recurrence (C-16/C-18 wins). No factored-table gather in
the hot codelet.

## 11. Correctness strategy
Generation-time numpy self-check (rel err ≤1e-11) before emit; build-time gate vs brute DFT (small) and vs MKL
(4096+); round-trip fwd+inv; run-twice determinism (the moat). f64 ~1e-15 class, f32 ~3e-7 class.

## 12. Benchmark strategy
Standalone build/ probe: generated codelet vs `execute_batched` at each ladder size, ns/el + spills (objdump) +
shuffles + llvm-mca, under the existing MKL comparison harness (core-pinned, MKL_NUM_THREADS=1, interleaved A/B to
beat thermal drift).

## 13. Integration strategy
Generated codelets emit to `engine/hesap-fft/include/crd/hesap/fft/detail/`; the planner dispatches to them by
(N, dtype, batch) with the current path as fallback. Gated until a size wins ≥1.10× sub-FFT AND ≥1.05× full 8M.

## 14. Milestones
- **M0 ✅ (2026-06-16):** design doc + generator skeleton (`build/gen_subfft.py`) + N=16/32/64 emitted, numpy-self-checked, benchmarked vs `execute_batched`. **Generated BEATS the engine at N=32 (1.98×) and N=64 (1.24×)** — proves the pipeline. Dossier C-22.
- **M1 ✅ (2026-06-16):** N=256/512/1024 ladder with the same schedule + register model. **Found the decisive crossover (see §15).** Dossier C-23.
- **M2 ✅ (2026-06-17):** hierarchical 64×64 4096 sub-FFT POC. **Variant B (fused twiddle+transpose into stage-1
  stores) = 1.240× over `execute_batched(4096,16)`, machine-eps.** Composed into the full FFT behind `CRD_FFT_M2_HIER`
  (gated OFF): **full-16M ~1.10× (both sub-FFTs 4096, clears the 1.05× gate), full-8M ~1.04× (only n1=4096 = 27%
  accelerated)**, all machine-eps + reproducible. Variant A (explicit transpose) lost (0.740×). See §15.5 + the
  session log. The approach is VALIDATED; 8M needs P2 too (M3).
- **M3 ✅ (2026-06-17):** the 2048 = **64×32** hierarchical sub-FFT (Choice B, 1.260× isolated; Choice A 32×64 =
  1.224×). Wired alongside M2's 4096 path ⇒ at 8M BOTH sub-FFTs hierarchical (~51%). **Full-8M 1.13–1.17× engine
  (~0.94× MKL), 4M 1.13–1.24× (~0.95–0.99× MKL), 16M unchanged-by-construction, all machine-eps** — the ≥1.05× gate
  cleared. See §15.6 + the session log. Default-enable candidate (gated OFF pending the M4 DoD).
- **M4 ✅ (2026-06-17, enable done; multi-config sweep + commit pending the user):** the hier path is now the
  **default** f64 forward sub-FFT for n∈{2048,4096} (`#ifndef CRD_FFT_DISABLE_HIER`; inverse/f32 → radix-8). Verified
  4 win-configs FFT-scope + ASan-clean + LTCG + clang-tidy + gcc; **`ctest --preset win-debug` GREEN 3935/3935 incl.
  guards**; 8M ~1.13–1.21× → ~0.90–0.94× MKL, 4M near parity, machine-eps. Hit 2 pre-existing env landmines (C1853
  stale-PCH from an MSVC update; the dumpbin-guard needing vcvars). Session `2026-06-17-fft-m4-enable-default.md`.
- **M5 ✅ (2026-06-17):** the hier **1024 = 32×32** sub-FFT added to the default hier path (the four-step n=1024
  sub-FFT at 1M/2M). A real KEEP: engine ~1.15–1.20× on 1M/2M vs radix-8, no 4M/8M/16M regression, machine-eps.
  ⚠ HONESTY: the M5 "2M ~0.99× / 1M 0.78×" used `m3_full` (flatters MKL ~20%); CANONICAL `bench_fft_vs_refs`:
  **1M 0.60× · 2M 0.75×** (vs radix-8 0.53/0.68). Session `2026-06-17-fft-m5-hierarchical-1024.md`.
- **M4+M5 COMMITTED** (`251bd79`,`d602c30`) + toolchain-cleanup (`aeb0086`); full 4-config DoD GREEN.
- **M6 Phase 0+1 ✅ (2026-06-17, profile-first):** canonical board 1M 0.60 / 2M 0.75 / 4M **1.04 (beats)** / 8M 0.91 /
  16M ~0.90. Residual profile: sub-FFTs are hier-optimized; **8M/16M loss is MOVEMENT — the four-step GATHER is the
  biggest lever (~34–35%)**; 1M is MKL-near-optimal (hard).
- **M6 Front C / Variant A (gather⊗stage-1 fusion) ⛔ MEASURED-REVERT (2026-06-17):** the gather-fused stage-1 reads
  din directly (index map bit-identical) and removes the scratch round-trip (+1.077× input-side), but the full FFT
  REGRESSES (8M ~30% / 16M ~20% slower, clean CRD-vs-CRD) — the dominant cost is the strided din-read, and the
  codelet's interspersed strided loads lose the dedicated gather's prefetch + are buffer-placement-sensitive.
  Reverted to M5-clean; probes in `build/m6_gather_*`. **NEXT = M7: f32 Vec8f hierarchical port** (the biggest
  untouched lever, f32 ~0.78×), per the kill condition.

## 15. M1 result — the crossover (the architecture-deciding measurement)
Generated batched Vec4d codelets vs `execute_batched` (f64, b=32), all machine-eps correct:

| N | engine ns/el | generated ns/el | speedup | spills | compile |
|---|---|---|---|---|---|
| 16 | 0.221 | 0.470 | 0.47× (the hand-tuned `small_n` lane-trick wins — expected) | low | fast |
| 32 | 1.066 | 0.537 | **1.98×** | low | fast |
| 64 | 1.285 | 1.033 | **1.24×** | low | fast |
| 256 | 1.726 | 1.746 | 0.99× | 4454 | — |
| 512 | 1.948 | 1.860 | 1.05× | 9887 | — |
| 1024 | 2.353 | 2.564 | **0.92×** | **21996** | **574 s** |

**Crossover at N≈64.** Straight-line wins to N=64 then collapses: the DAG (15361 nodes @1024) vastly exceeds the
16-ymm file ⇒ spills explode (21996) ⇒ loses + a 574 s compile (impractical). This is the genfft lesson confirmed
by measurement — **straight-line ONLY for small leaves; larger sizes need recursive composition.**

**Path decision for 4096 → Path B (hierarchical 64×64).** 4096 = 64×64 from the generated-64 leaf (1.24×) as the
radix-64 building block + the already-vectorized bb-axis twiddle + a 64×64 transpose. Rejected: Path A full-4096
straight-line (≈4× the 1024 = catastrophic spills + ~40 min compile); 256×16 (256 already neutral); stage-Stockham
△ (radix-8 near the gcc limit). ⚠ The N=64 leaf win does NOT automatically compose — M2 must prove it.

## 15.5 M2 result — the composition proved (2026-06-17)
Built both variants (`build/gen_subfft_m2.py` + `m2_4096_bench.cpp`); gated into the engine four-step behind
`CRD_FFT_M2_HIER` (OFF by default). Decomposition `n=64·n1+n2`, `k=64·k2+k1`, element-major `scratch[n·16+bb]`:
stage-1 = one `codelet64(b=1024)` (element n1) → twiddle `W_4096^{n2·k1}` → transpose k1↔n2 → stage-2 = `codelet64(b=1024)`.

**4096 sub-FFT (isolated):** Variant B (fused twiddle+transpose into stage-1 stores, 2 passes) = **1.240×** machine-eps;
Variant A (explicit transpose pass, 4 passes) = 0.740× (the separate pass eats the win — the memory-pass model held).
The generated-64 leaf survives the b=1024/1MB regime (1.115 vs engine 1.456 ns/el = 1.31×).

**Parity dashboard (f64, the M2 line is in situ behind the gate):**
| version | 8M f64 ×MKL | 16M f64 ×MKL | notes |
|---|---|---|---|
| pre-archaeology | 0.76 | — | |
| after 4 archaeology wins | ~0.84 | ~0.85 | the committed engine |
| **M2 hierarchical 64×64 (gated)** | **~0.86 (8M +1.04×)** | **~0.96 (16M +1.10×)** | machine-eps; 16M clears the 1.05× gate |

**Why 8M < 16M:** at 8M only the n1=4096 sub-FFT is hierarchical (**27%** of the four-step — the "~46%" in §2
conflated *both* sub-FFTs); at 16M both n1 and n2 are 4096 (~51%). The gain scales with the accelerated fraction;
no mechanism difference. In-situ erosion (isolated 1.24× → ~1.1–1.18× in situ) is a cold-bbuf hypothesis, not a
finding. **M3 closes 8M by making the 2048 sub-FFT hierarchical (32×64) too.** Full detail + the interleaved 16M
correctness/timing table: `docs/sessions/2026-06-17-fft-m2-hierarchical-64x64.md`.

## 15.6 M3 result — the 2048 sub-FFT closes 8M (2026-06-17)
The M2 diagnosis (gain ∝ accelerated fraction) predicted that accelerating the n2=2048 sub-FFT too would lift 8M to
the 16M class. Confirmed. 2048 = N1·N2 with **N1=64, N2=32, BB=32** (the M2 generalization, `gen_subfft_m3.py`):
stage-1 fused `codelet64_stage1_fused_64x32(b=N2·BB=1024)` (twiddle `W_2048^{n2·k1}` + transposed store) → stage-2
plain `codelet32_batched(b=N1·BB=2048)`. **Choice B (64×32) = 1.260× isolated; Choice A (32×64) = 1.224× — B chosen.**

Wired into the gate beside the 4096 path (one ctor builds bbuf=65536 complex + the twiddle for n∈{4096,2048};
dispatch on f64/Forward/b==BB). **8M now has both sub-FFTs hierarchical (~51%).** Measured (primary = `m3_full`
interleaved; ×MKL = canonical `bench_fft_vs_refs` within-run):

| n | engine speedup (M3 vs BASE) | ×MKL (M3) | note |
|---|---|---|---|
| 4M (both 2048) | 1.13–1.24× | ~0.95–0.99 | biggest gain |
| 8M (4096+2048) | 1.13–1.17× | ~0.94 | the M3 target — cleared |
| 16M (both 4096) | 1.06–1.08× | ~0.90 | unchanged-by-construction (byte-identical 4096 codelet) |

All machine-eps (1.1–1.2e-15). 8M ~0.94× MKL matches the prompt's "≥1.10× ⇒ 8M ~0.92–0.94× MKL". Three independent
measurements agree (isolated 1.26× · interleaved 1.13–1.17× · canonical 1.115×) — solid, unlike M2's wobbly 8M.
Default-enable candidate; gated OFF pending the M4 18-config DoD + the forward-bit backward-compat check.
Full detail: `docs/sessions/2026-06-17-fft-m3-hierarchical-2048.md`.

## 16. M2 plan — hierarchical 64×64 4096 sub-FFT POC
Standalone f64 `build/` probe; baseline = current `execute_batched(4096)`. **Acceptance:** 4096 sub-FFT ≥1.10× →
compose into full 8M; full 8M ≥1.05× → keep behind a gate; else diagnose/revise (never revert to full straight-line).

1. **Lock baseline** — current `execute_batched(4096)` (f64 b16; f32 b32/b16) + the generated-64 leaf (ns/el, cyc/el, max_rel_err).
2. **64×64 decomposition map** — answer explicitly: does stage-1 output feed stage-2 directly · is a 64×64 transpose required · can transpose+twiddle fuse · can it stay L1/L2-resident · does it preserve the engine-native batched contract · is the extra movement < the saved generic-kernel overhead.
3. **Component benchmarks** — stage-1 leaf · stage-2 leaf · 64×64 transpose · twiddle · fused twiddle+transpose · predicted hierarchical total. (The leaf is 1.24× but 4096 only wins if transpose+twiddle don't eat it.)
4. **Build at most TWO variants** — A simple (leaf · explicit twiddle · explicit 64×64 transpose · leaf); B fused (twiddle while writing the transposed tile). No more variants this cycle.
5. **Assembly / pressure check** — spills · shuffles · loads/stores · code size · L1/L2 traffic · llvm-mca. Expected mechanism: avoid the full-4096 spill explosion, reuse the fast leaf, keep the 64×64 intermediate cache-resident. If the mechanism is not observed, do NOT compose into 8M.
6. **Correctness** — vs current CRD + MKL + impulse + sine/bin (f64 ~1e-15 class).
7. **Full-8M composition** — only if the sub-FFT ≥1.10×; keep/revert table at 2M/4M/8M/16M, no regression, determinism preserved.
8. **Parity dashboard** — update (pre-archaeology 0.76/0.61 → 4 wins 0.84/0.78 → hierarchical POC).

**If both variants lose:** diagnose (transpose cost / leaf-call overhead / layout mismatch / icache / twiddle /
shuffles) → choose one of {M2b fused layout · generated stage-Stockham · generated leaves inside the current radix
tree · close the generator path}. **STRICT non-goals (unchanged from Fork A):** no full-4096 straight-line DAG · no
atom transform-major detour · no global SoA rewrite · no engine integration before the 4096 phase win · no
report-only predictions.

## 17. Bank / clean status (2026-06-16)
The 4 archaeology wins — split / f32 NT scatter / f32 SIMD twiddle = **committed** (`d5ae96d`, `d0f2484`); **f64
bb-axis SIMD twiddle = UNCOMMITTED** (working-tree `fft.hpp`, +65 lines; the user commits). M0/M1 generator
artifacts isolated in gitignored `build/`. Engine hot path clean. The 4-win DoD (debug/asan/shipping/tidy 17/17)
was green at C-22. **➜ Commit the f64 twiddle before M2.**

## Parity arithmetic (target)
f64 8M 0.84× → parity needs ~1.19× full; sub-FFT is ~46% → if only sub-FFT improves, parity needs ~1.5× sub-FFT.
Sub-FFT headroom (13.5 → ~5.25 cyc/el = MKL) is 2.6×, so ~1.5× is within the generator's reach if it closes
half the shuffle+spill overhead. Hard but not absurd.

## 18. M16–M19 contract-breaking fused-codelet campaign (2026-06-19) — status

All gated, **default-OFF**; the banked **gather fusion remains the shipped default for every size**. Honest
**same-harness** f32 1M (MIN metric — medians were host-contention-noisy): MKL 1.71 / gather 2.49 / M17 2.08 /
**M18 2.01 ms = 0.85× MKL, 19% faster than gather**, all maxrel vs MKL ~2.6e-7.

| milestone | gate | what | verdict |
|---|---|---|---|
| M16-B | `CRD_FFT_M16B_FUSED_BRIDGE_POC` | native tiled producer + in-register 8×8 transpose + recurrence twiddle (reordered base) | required by M17/M18 |
| M17 | `CRD_FFT_M17_SCATTER_FUSION_POC` | fuse final NT-scatter into P2 stage-2 store (no scratch/scatter pass) | win, fallback |
| **M18** | `CRD_FFT_M18_P2_FUSED_POC` | **fuse P2 leaf+stage2+final over a 64KB tile (no 1MB bbuf)** | **best CRD path, gated candidate** |
| M19-A | `CRD_FFT_M19_P1_FUSED_POC` | P1 gather+producer fusion | **REJECTED by measurement** (correct, but +0.8 Mcyc: 64KB tile + br_ transpose double-buffer spills) |
| M19-B | — | register-streaming P1 fusion | **structurally blocked** (proof: P1 stage-2 needs all 32 n2v ⇒ full tile; transpose needs all 8 kl ⇒ br_ buffer — both mandatory) |

**Scope: M18 = f32, N=1M, forward ONLY.** 2M's four-step is n1=2048/n2=1024 — the M16-B→M18 chain hardcodes the
32×32 (1024) decomposition, so 2M falls back to gather (correct, ~0.65× MKL). Extending to 2M = a new 64×32 fused
chain. **The remaining ~15% to MKL is genuine kernel/bandwidth, not a removable stage boundary.** No engine
default change; not committed.
