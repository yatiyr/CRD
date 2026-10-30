# 2026-10-30 — SIMD-DISPATCH: SaAmg's sparse and CSR products as forced crd-jobs arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[the LDLT, QR and BLR walks' levers](2026-10-30-simd-dispatch-jobs-ldlt-qr-blr-picks.md).

## CI first

- Push run 40976392370 (c28aaa54), still running: linux-gcc-debug, -relwithdebinfo, -debug-scalar, -public-checks and
  -asan failed on one error, `-Werror=double-promotion` at test_cholesky.cpp:340 (a float factor promoted to match a
  double one in the lever test's backward-error product). Fixed at bc2bdb83 (both factors widened). Every other
  completed run since 40952979940 was cancelled before a job started. Nothing cancelled or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has run. Nothing flips.

## What landed

hesap-amg had no jobs consumer: every product ran serially. The retune's question for it is whether its products'
kept-serial decision holds, so the products became levers whose defaults keep today's serial build and cycle.

- **`detail::SpgemmPick`** (hesap-sparse `detail/spgemm_arms.hpp`): `parallel_min_flop` (default never) and
  `workers`. `spgemm_arm` (spgemm_parallel.hpp) runs serial `spgemm` below the cut, on one job, on a pool that is down
  or for an empty A (no flop pass on these routes), otherwise the row-parallel SPA body, or the hash body past
  `kMaxSpaCols`. `spgemm_parallel` is the arm with the cut at 0: one body. The flop prefix and the job bounds, copied in
  both bodies, are one helper each.
- **`detail::AmgPick`** (hesap-amg `detail/amg_arms.hpp`): the products' `SpgemmPick` and the CSR products' cut
  `spmv_min_nnz` (default never) and `spmv_workers`. `SaAmg`'s constructor takes it as a defaulted last argument.
  `spgemm_arm` runs the smoothing product A T (`smoothed_prolongator`) and the Galerkin R (A P) at each level;
  `csr_apply` (`detail/csr_apply.hpp`) runs every CSR product: the residuals, restriction and prolongation, the
  K-cycle's products, the candidate relaxation and the power iteration's D^-1 A x. Its rows split into ranges of
  balanced stored entries, one per job. `SaAmg`'s private spmv and the power iteration's inline copy are gone.
- `SaAmg::level(k)` and `coarse_lu()` expose the hierarchy for the gates.
- **Kept serial, no arm:** aggregation and the C/F split (order-dependent, D(amg)-1, -2), Gauss-Seidel, the K-cycle's
  dots and the power iteration's norms (fixed-order sums), the transposes, the prolongator's TripletBuilder compress and
  the coarsest LU's solve. The board times the finest level's phases so each share is on evidence. The cycle's five
  arrays per level and apply are apply-time allocations, not a jobs lever.
- Corrected: amg.hpp claimed "only spmv/spgemm parallel" and its cycle comment "the AMG apply is fully serial"; both
  now describe the levers. hesap-amg links crd-jobs and declares it (it came only through hesap-sparse).
- `spgemm_parallel` of an empty A returned a 0 x 0 matrix; it now returns serial's 0 x n.

Determinism: both levers are scheduling. Each output row is one job's, accumulated in the serial order, so every pick
gives the serial bits. A forced parallel product inside a job nests a `parallel_for` under that job's wait; the
defaults never do.

## Tests

- `spgemm_arm` (test_spgemm_parallel.cpp), f64, f32, c64, c32: square n 1, 3, 64 and 500 and a 300 x 200 by 200 x 450
  chain, skewed rows and empty rows, with and without NaN, +-Inf, -0 and subnormal values; cuts 0, the product's
  flops, flops + 1 and never, workers 0, 1, 2, 3, 4, 8, 16 and 32; shape, topology hash and every byte against serial.
  The hash path (B past `kMaxSpaCols` columns) and an empty A on the same arms.
- `sa_amg_arm` (test_amg.cpp) on a four-worker pool, f64, f32, c64, c32: 2D Poisson (n 1600, three levels) under the
  V, W, F and K cycles, Ruge-Stuben, the adaptive candidate with the ILU(0) smoother and one NaN coupling; a
  convection-diffusion on the Petrov-Galerkin K-cycle and on plain aggregation. Eleven arms (the products on 1, 3, 8,
  32 jobs and the pool, the CSR products on 7, 16, 32 and the pool, both on 16, finest levels only): every level's A, P,
  R and 1/diag(A), the coarsest LU and the apply bit for bit, guard words after the apply's span. A forced pick on a
  pool that is down runs serially.

win-debug: spgemm cases 0.9 s, `sa_amg_arm` 3.0 s.

## Board

`bench_hesap_amg_parallel` (`scripts/run_bench_amg_parallel.sh`): 2D and 3D Poisson from 64K to 1M rows, a 2D
convection-diffusion on the K-cycle and 2D Poisson under Ruge-Stuben, f32 and f64. It reports the finest level's
build phases on one job with their shares and the three products on N jobs, the build at the default pick against
four arms gated bit for bit, and the apply against the CSR products' arm. No bench ran: the board runs at row close.
Peers: the one-job control in the same binary. PyAMG, hypre BoomerAMG and AMGCL are not on this host (each would be a
download).

## Checks

win-debug builds of crd-hesap-sparse-tests, crd-hesap-amg-tests and the bench; the `[spgemm]` cases and the whole amg
binary pass; clang-format on the added lines; Allman braces; the ctest guards (include budget with the two new
entries, source routes); check-repository.

## Follow-up carried in: svdvals of a non-finite matrix

The LDLT note's follow-up. `svdvals` had no `svd` guard: with a NaN or infinite entry the complex driver asserted its
bidiagonal iteration converged, and the real dqds path returned values that were not NaN (the extended test failed
both before the fix). It now returns min(m, n) quiet NaN, sharing `svd`'s helper; tested in the `svd` case's
shapes, entries and four types.

## Left

hesap-iterative, -autodiff and -sched.
