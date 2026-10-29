# 2026-10-30 — SIMD-DISPATCH: the dense factors' crd-jobs levers as forced arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[gemm_parallel's levers](2026-10-29-simd-dispatch-jobs-gemm-picks.md).

## CI first

- No completed run has a failed job. Push runs 40934084386 to 40949527792 were cancelled; none of their jobs failed. Push
  run 40929108987 (1ebe9dc4) still has four jobs running (linux-clang-tsan, win-debug, win-asan, linux-gcc-asan);
  every finished job passed. 40952979940 (6b9b7b2e), the first run under the win-asan bound, is pending.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has completed. Nothing
  cancelled or dispatched.

## What landed

The retune's second consumer, the dense factors. `crd/hesap/dense/detail/factor_arms.hpp` holds `LuPick` and
`CholeskyPick`; `factor_lu` and `factor_cholesky` are `factor_lu_arm(LuPick{})` and `factor_cholesky_arm(CholeskyPick{})`,
so production and the arms share one body. Every default is today's behaviour.

- **LU:** `block` (64), `trailing_min` (the trailing `gemm_parallel` took the whole pool at every panel, even a
  tiny trailing block; it can now run serial below an m n k), `trsm_min` (U12 = L11^-1 A12 was always serial; it can
  now split whole columns across the jobs) and `workers`.
- **Cholesky:** `unblocked_max` (256), `block` (64), `trsm_min` (64K rows times nb), `syrk_mmk` (256K) and `workers`.
- **syrk:** `SyrkPick {parallel_mmk, workers}` replaces `syrk_lower_minus`'s `allow_parallel` flag (its callers pass
  `SyrkPick{.workers = 1}`: the supernodal cmod's two sites, the dense kernel test and the dense fatal specimen).
- Every internal `jobs::num_workers()` read now goes through `workers`, so one test pool runs 1 to 32 jobs.

Determinism: the worker count and the split thresholds decide which job computes a row or column, never its value,
so for one block and cut every arm gives the one-job factor's bits. The block and the unblocked cut regroup the
trailing sums: they are held to the elementwise backward-error bound (Higham 9.3 and 10.3, 4 n eps |L||U|), not bits.

- **Tests:** `factor_lu_arm` (f32, f64, c64) and `factor_cholesky_arm` (f32, f64) at n 1, 63, 64, 100 and 300; blocks
  64, 16 and 100 (Cholesky: blocked at 64 and 16, unblocked throughout). Each lever arm runs at 4, 8, 16 and 32 jobs
  and is compared bit for bit (factor, pivots, info) with one job. The inputs are random, IEEE specials (NaN,
  infinity, -0, a subnormal) and a zero column or negative diagonal at 70 (info 71 at every pick). `syrk_lower_minus`
  splits at 4 to 32 jobs against one job, over a padded C with guard words in its padding and after it.
- **Board:** `bench_hesap_factor_parallel`, one process per `--threads N`, times the one-job factor and each arm round
  by round at n 64 to 2048 for f32 and f64. Lever arms are gated bit for bit and block and cut arms by the bound;
  either miss exits 1. `scripts/run_bench_factor_parallel.sh` runs it at 4, 8, 16 and 32 workers at each level.

## Checks

win-debug builds of crd-hesap-dense-tests, crd-hesap-direct and the bench; the factor and syrk cases (31 cases
pass; the three new ones take 16 s, 12 s and 0.8 s); strict tidy (LLVM 20) clean on all ten changed C++ files;
clang-format on the added lines; Allman braces; the ctest guards (the include budget gains `factor_arms.hpp`);
`bash -n` on the driver. No bench ran: the board runs at row close.

## Left

The retune's other consumers, one batch each: eig_sym's multisection and mrrr's chunking (first prove whether
multisection is bit-exact across its chunk count p or declare its tolerance), hesap-direct (supernodal, multifrontal,
dense LU kernels), hesap-amg's serial Galerkin product, hesap-iterative's operator threshold, hesap-autodiff's batch
gradient and hesap-sched (a measured consumer or the unused link removed).
