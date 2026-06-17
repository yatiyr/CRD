# 2026-06-17 — FFT M2: hierarchical 64×64 generated 4096 sub-FFT POC

> Module: `crd-hesap-fft` (v10), Fork A (generated-codelet project). Continues
> `2026-06-16-fft-generated-codelet-m0-m1.md`. Design: `docs/design/hesap_fft_generated_codelets.md` §16.
> Memory: `project_v10_fft_plan`. All probes in gitignored `build/`; the gated engine path is OFF by default.

## TL;DR — the hierarchical 64×64 approach is VALIDATED. Sub-FFT 1.24×; full-16M ~1.10× (clears); full-8M ~1.04×.

- **4096 sub-FFT (isolated, well-controlled): Variant B fused = 1.240× over `execute_batched(4096,16)`, machine-eps
  (4.9e-16 vs MKL+engine).** The phase gate (≥1.10× sub-FFT) PASSES. Variant A (explicit transpose) = 0.740× (the
  separate transpose pass eats the win, exactly as the memory-pass model predicted).
- **Full-16M (in situ, `CRD_FFT_M2_HIER`): ~1.10× engine speedup, 3/3 interleaved passes, machine-eps (1.1e-15).
  CLEARS the 1.05× keep gate.** At 16M BOTH four-step sub-FFTs are 4096 ⇒ both hierarchical (~51% accelerated).
- **Full-8M: ~1.04× — just below the 1.05× gate.** At 8M only the n1=4096 sub-FFT is hierarchical = **27%** of the
  transform (NOT the ~46% the design doc assumed — that figure conflated BOTH sub-FFTs; at 8M the n2 one is 2048).
- **Decision:** the approach works. Keep the gated experimental path (OFF by default — every edit is inside
  `#ifdef CRD_FFT_M2_HIER`; default TU byte-identical, baseline bench confirms machine-eps + unchanged perf). 8M
  doesn't yet hit 1.05× only because the n2=2048 sub-FFT isn't hierarchical — that's the **M3 lever** (generate a
  2048 = 32×64 hier path ⇒ ~51% accelerated at 8M too). Enable-by-default after M3 closes 8M (8M today is positive
  +machine-eps with zero regression, so flipping it on is defensible now — but the strict gate argues for P2 first).

## The decomposition (correct; machine-eps verified at 4096 isolated, 8M, and 16M)
4096 = 64(n1) × 64(n2); input n = 64·n1 + n2; output k = 64·k2 + k1; element-major `scratch[n·16 + bb]`, BB=16.
- **Stage 1** = one `codelet64(b = 64·16 = 1024)`, element n1, batch (n2,bb) contiguous — *exactly* element-major,
  no gather.
- **Twiddle** `W_4096^{n2·k1}` (a new inner 64×64 table, distinct from the four-step bb-twiddle), broadcast over bb.
- **Transpose** k1↔n2 (unavoidable). **Variant A** = a separate pass (loses). **Variant B** = folded into stage-1's
  output store (twiddle + transposed store), so stage 2 is a plain `codelet64(b=1024)` with naturally-ordered output
  and NO separate transpose pass — 2 passes vs A's 4. Variant B is what's gated into the engine.

## Measurements

### 4096 sub-FFT, isolated (f64, BB=16, 65536 el; 25 trials × 60 reps, interleaved, `build/m2_4096_bench.cpp`)
| kernel | ns/el | vs baseline | max rel err (MKL / eng) |
|---|---|---|---|
| baseline `execute_batched(4096,16)` | 2.608 | 1.000× | 7.4e-16 (vs MKL) |
| Variant A (plain s1 + twiddle + transpose + plain s2) | 3.526 | **0.740×** | 4.46e-16 / 4.62e-16 |
| **Variant B (fused tw+transpose → plain s2)** | **2.103** | **1.240×** | 4.90e-16 / 4.90e-16 |

Components (diagnostic): leaf-64 generated @b1024 **1.115** vs engine @b1024 1.456 (the leaf advantage SURVIVES the
b=1024/1MB regime, 1.31×) · twiddle 0.212 · 64×64 transpose 0.433 · stage1-fused 0.893 · stage2 1.239. The generated
N=64 leaf self-checks at gen (577-node split-radix DAG, err 2.6e-16).

### Full-16M, interleaved BASE/M2, correctness-gated (`build/m2_16m.cpp`, `run_m2_16m2.sh`) — the ≈51% test
| pass | BASE engine ns/el | M2 engine ns/el | speedup | BASE Cerid/MKL | M2 Cerid/MKL | M2 maxrel |
|---|---|---|---|---|---|---|
| 1 | 10.228 | 9.616 | 1.064× | 0.845 | 0.947 | 1.1e-15 |
| 2 | 10.505 | 9.388 | 1.119× | 0.868 | 0.984 | 1.1e-15 |
| 3 | 10.457 | 9.490 | 1.102× | 0.848 | 0.957 | 1.1e-15 |

Reproducible ~1.10× (Cerid/MKL 0.85→0.96), correct to 1.1e-15. This SUPERSEDES an earlier single `prof16m` sample
that showed no win — it had unverified correctness and sat inside the ±7% cycle-noise band (the advisor's catch).

### Full-8M, in situ (`CRD_FFT_PROFILE`, cyc counts; `build/prof8m.cpp` + the real bench `run_m2_8m.sh`) — the 27% case
P1 = the n1=4096 sub-FFT (gated); P2 = the n2=2048 sub-FFT (NOT gated). Two profiler samples:
| sample | BASE P1 sub-FFT (Mcyc, %) | M2 P1 sub-FFT | in-situ cut | engine ns/el BASE→M2 |
|---|---|---|---|---|
| 1 | 68.5 (27.2%) | 58.2 | 1.177× | 9.473→9.119 (1.039×) |
| 2 | 69.1 (26.9%) | 63.1 | 1.095× | 9.673→9.215 (1.050×) |
P2 sub-FFT unchanged (60–62 Mcyc) — correct, the gate fires only on 4096. Real-bench maxrel @8M M2 = 1.2e-15.
8M = ~1.04× (≈half the 16M gain — consistent with 27% vs 51% accelerated). The single noise-independent fact: 27%
accelerated × even the clean isolated 1.24× caps 8M at ~1.04–1.05×, so 8M alone cannot be a robust ≥1.05× — settled
by arithmetic, not the (noisy) wall.

## Diagnosis
1. **The gain scales with the accelerated fraction.** 8M (n1 only, 27%) → ~1.04×; 16M (both, 51%) → ~1.10×. This is
   the whole story for the 8M-vs-16M difference — no mechanism difference needed.
2. **In-situ erosion (hypothesis, not finding).** The isolated 1.24× drops to ~1.1–1.18× in situ. Most defensible
   cause: the transposed `bbuf` is L2-hot across the isolated reps but cold per-call inside the four-step. NOT a
   working-set-pressure effect — the per-call set is identical at 8M and 16M (`block_width(4096)=16` ⇒ 1MB scratch +
   1MB bbuf both), and the isolated Variant B touches a *larger* footprint yet still won 1.24×. (Earlier draft's
   "bbuf round-trip erased at 16M" claim was wrong and is retracted.)

## Decision & next action (M3)
- **Phase win achieved (sub-FFT 1.24×); composition validated (16M ~1.10×, machine-eps, reproducible).** 8M is the
  only sub-gate case, purely because P2 (2048) isn't hierarchical yet.
- **M3 lever (advisor-pointed, the direct route):** generate the **2048 = 32×64 hierarchical sub-FFT** (the engine
  already has codelet32/codelet64). That accelerates P2 ⇒ ~51% at 8M ⇒ 8M should reach ~1.08–1.10× like 16M. Then
  flip the dispatch on by default (size+dtype gated) and run the 18-config DoD + full parity dashboard.
- **Then** M3-rest: f32 port (Vec8f), inverse codelets (forward-only today), planner dispatch.
- **STRICT non-goals held:** no full-4096 straight-line DAG; no transform-major atom; no global SoA rewrite; the
  composition was BUILT and MEASURED (not predicted); correctness machine-eps throughout.

## Files
- `build/gen_subfft_m2.py` — M2 generator (N=64 plain + fused-stage1, numpy-self-checked).
- `build/m2_4096_bench.cpp` + `build/run_m2.sh` — the isolated 4096 sub-FFT POC.
- `build/run_m2_8m.sh` / `build/run_m2_prof.sh` (8M) · `build/m2_16m.cpp` + `build/run_m2_16m2.sh` (16M corr+timing).
- `engine/hesap-fft/include/crd/hesap/fft/detail/m2_hier_codelets.hpp` (generated) + the `#ifdef CRD_FFT_M2_HIER`
  edits in `fft.hpp` (include / ctor twiddle+bbuf / dtor / members / `execute_batched` dispatch). All gated OFF.
