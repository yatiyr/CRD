# 2026-06-17 — FFT M5: hierarchical 1024 = 32×32 sub-FFT → 2M near-parity, 1M crush

> Module: `crd-hesap-fft` (v10), Fork A. Continues `2026-06-17-fft-m4-enable-default.md` (M4 committed `251bd79`).
> Memory: `project_v10_fft_plan`. The 1024-hier is now part of the default-on hier path (disable `-DCRD_FFT_DISABLE_HIER`).

> ⚠⚠ **HONESTY CORRECTION (M6 Phase 0, 2026-06-17):** the ×MKL numbers below used the `m3_full` harness, which
> measures MKL ~20% SLOWER than the canonical `bench_fft_vs_refs` ⇒ it FLATTERED Cerid. The "2M ~0.99× near-parity"
> and "1M 0.78×" claims are WRONG. The CANONICAL board is **1M 0.60× · 2M 0.75×** (the hier is still a real win:
> 1M 0.53→0.60, 2M 0.68→0.75 vs radix-8; the KEEP stands). Read ×MKL off `bench_fft_vs_refs`, never `m3_full`.

## TL;DR — 1024-hier (32×32) improves 1M/2M (canonical 0.60×/0.75× MKL — NOT the m3_full-flattered 0.78/0.99). KEEP.
Profiled first (as instructed): the 2M four-step is n1=2048 (M3-hier) × n2=**1024** (radix-8), and 1024 is the
single largest phase (32%). Built the 1024 hier (32×32, both stages = the strongest codelet32), composed into the
real four-step. **Result beat the cycle-based ceiling**: 2M **1.12–1.23× over M4 → ~0.99× MKL** (was 0.80–0.89),
1M **~1.25× → 0.78×** (was 0.62), machine-eps, no 4M/8M/16M regression (the n==1024 gate doesn't fire there).
Enabled by default; the existing oracle test (2^20/2^21) gates it; win-debug `[fft]` green.

## Phase 1 — 2M phase profile (confirmed 1024 is the target, before building)
2M = 2^21 ⇒ n1=2048, n2=1024. `build/prof2m.cpp`, HIER(default) vs OFF(radix-8), 40-call measurement:
| phase | HIER Mcyc (%) | OFF Mcyc | note |
|---|---|---|---|
| P1 sub-FFT (n1=2048, hier) | 10.4 (23.5%) | 14.8 | M3-hier cuts it 1.42× |
| **P2 sub-FFT (n2=1024, radix-8)** | **14.3 (32.1%)** | 14.5 | the largest phase, un-accelerated ← target |
| movement (gather+twiddle+scatter) | ~19.8 (~44%) | ~19.1 | a real floor |
Answers: (1) yes, 1024 = 32% = largest; (2) ~1.07× cycle-ceiling if 1024 improves alone; (3) ~56% sub-FFT / ~44%
movement.

## The two checks that re-pointed the build (advisor-driven, before the surgery)
- **Ceiling:** 32.1% × (1−1/1.269) ⇒ ~1.073× full-2M at *perfect* transfer — under the 1.08× bar on paper.
- **Cheap proxy (2048-hier on/off at 2M, interleaved):** the shipped 2048-hier moves the 2M *wall* ~1.04× (not the
  prof2m-warm 0%, not full) ⇒ cycle→wall transfers at ~half.
- **Four-step vs direct at 2M (the Phase-7 "planner" hypothesis):** the DIRECT radix path is **2× SLOWER** at 2M
  (14.9 vs 6.9 ns/el, 0.37× vs 0.83× MKL). The four-step is correctly chosen — NOT a planner issue. The sanity
  sizes exposed the real structure (below), which made 1024-hier worth building despite the 2M-only ceiling.

## The size structure (why 1024-hier matters more than the 2M-only ceiling said)
| size | the two four-step sub-FFTs | M4 ×MKL |
|---|---|---|
| 1M (2^20) | **1024 + 1024** (both radix-8) | **0.62 — weakest** |
| 2M (2^21) | 2048-hier + **1024** | 0.80–0.89 |
| 4M (2^22) | 2048-hier + 2048-hier | **1.05–1.09 (beats MKL)** |
Sizes where BOTH sub-FFTs are hier reach parity+; the un-accelerated 1024 is the weak link, and 1M (both 1024) is
hit hardest. So 1024-hier lifts 1M most and 2M materially.

## 1024 sub-FFT isolated (f64, BB=64, 65536 el; `build/m5_1024_bench.cpp`)
| variant | ns/el | speedup | max rel err (MKL/eng) |
|---|---|---|---|
| baseline `execute_batched(1024,64)` | 2.459 | 1.000× | 5.9e-16 |
| **A 32×32** (both codelet32) | 2.007 | 1.225× | 4.7e-16 |
| B 64×16 | 2.093 | 1.175× | 4.3e-16 |
| C 16×64 | 1.937 | **1.269×** | 4.5e-16 |
C was fastest, but A (32×32, the well-characterized codelet32 both stages, no generated-16 question) was chosen for
the engine per the advisor; the 3.5% isolated edge of C is ≤1% on full-2M.

## Full board — M4 default vs +1024-hier (Choice A), interleaved, machine-eps (`build/run_m5_board.sh`)
| size | M4 ×MKL | M5 ×MKL | engine speedup | note |
|---|---|---|---|---|
| **1M** | 0.61–0.63 | **0.77–0.79** | **~1.25×** | both sub-FFTs now hier |
| **2M** | 0.80–0.89 | **0.97–1.00** | **1.12–1.23×** | ✅ ≥1.08× victory + ≥0.97× stretch (parity band) |
| 4M | ~1.05 | unchanged (byte-identical path) | — | n==1024 gate doesn't fire |
| 8M/16M | ~0.90 | unchanged-by-construction | — | sub-FFTs are 2048/4096 |
2M maxrel 1.2e-15 (vs M4 1.3e-15). The win **exceeds the ~1.073× cycle ceiling** because 2M is memory-pass-bound:
the radix-8 1024 baseline (b=64, 4 passes in-place) pays more in-situ memory traffic than its cycle count shows, so
the hier's 2 passes win more on the wall than on cycles.

## Engine wiring (now default-on; disable `-DCRD_FFT_DISABLE_HIER`)
`detail/hier_codelets.hpp` regenerated (adds `codelet16_batched` + `codelet32_stage1_fused_32x32` + the unused
64x16/16x64 1024 variants). `fft.hpp`: ctor generalized to `h_n1` (twiddle stride; 64 for 4096/2048, 32 for 1024)
+ the n==1024 case (h_n2=32,h_n1=32,h_bb=64); dispatch n==1024,b==64 → `codelet32_stage1_fused_32x32(b=2048)` +
`codelet32_batched(b=2048)`. The 4096/2048 paths are byte-identical to M4 (h_n1=64). Forward-only.

## Verification + DoD
- win-debug `[fft]`: **72/12 PASS** — the four-step-vs-oracle test (2^19..2^23) now exercises the 1024-hier at
  **2^20 (1M, both 1024) + 2^21 (2M)** against the radix-2 oracle (rel<1e-12), so CI gates it with no new test.
- FFT-isolated change (only `fft.hpp` + the generated header); M4's full win-debug `ctest --preset` was 3935/3935.
- The formal multi-config sweep (asan/shipping/tidy full ctest + Linux) bundles with M4's pending one (same
  pre-existing C1853 stale-PCH blocker; clear PCHs + `per-slice-check -Reconfigure`).

## Parity dashboard (f64)
| version | 1M | 2M | 4M | 8M | 16M |
|---|---|---|---|---|---|
| after 4 archaeology wins | — | 0.76 | ~0.84 | ~0.84 | ~0.85 |
| M4 (2048+4096 hier) | 0.62 | ~0.86 | ~0.99 | ~0.90 | ~0.90 |
| **M5 (+1024 hier)** | **~0.78** | **~0.99** | ~0.99–1.05 | ~0.90 | ~0.90 |

## Decision & commit
**KEEP — M5 clears the victory (≥1.08×) + stretch (≥0.97× MKL).** Enabled by default. Proposed commit:
```
perf(hesap-fft): hierarchical f64 1024 subFFT (32x32) — 2M near-parity, 1M +25%

Add the generated hierarchical 1024 = 32x32 sub-FFT to the default f64 forward
hier path (disable with -DCRD_FFT_DISABLE_HIER). It accelerates the four-step
n=1024 sub-FFT (block_width 64), the weak link at 1M (both sub-FFTs 1024) and 2M
(one). Measured: 2M 1.12-1.23x faster -> ~0.99x MKL (near parity), 1M ~1.25x ->
0.78x MKL; 4M/8M/16M unchanged (the n==1024 gate doesn't fire there). Correctness
machine-eps vs MKL (~1.2e-15); the four-step-vs-oracle test already gates 2^20/2^21.
Chose 32x32 (codelet32 both stages) over 16x64/64x16. Inverse + f32 stay on the
radix-8 fallback.
```

## Next (M6 — overtake campaign)
2M/1M handled. M6 targets per the user's plan: 4M already >MKL; push 8M/16M ~0.90→0.97→>1.0 (profile the largest
remaining phase — likely the four-step movement floor at 8M/16M); then f32 (Vec8f) hier port + inverse hier codelets.
⚠ the advisor's cycle-ceiling under-predicted the 2M wall win — at memory-bound sizes, trust the in-situ composition
measurement over the cycle arithmetic.

## Files
- `build/gen_subfft_m3.py` (+1024 emits) · `build/m5_1024_bench.cpp` + `run_m5.sh` · `build/prof2m.cpp` +
  `run_m5_prof2m.sh` · `build/run_m5_proxy.sh` · `build/run_m5_fourstep.sh` · `build/run_m5_board.sh`.
- `engine/.../detail/hier_codelets.hpp` (regenerated) + `engine/.../fft.hpp` (ctor h_n1 + 1024 dispatch).
