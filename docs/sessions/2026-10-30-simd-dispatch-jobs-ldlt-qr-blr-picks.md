# 2026-10-30 — SIMD-DISPATCH: the LDLT, QR and BLR level walks' crd-jobs levers as forced arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[the sparse LU factors' levers](2026-10-30-simd-dispatch-jobs-lu-picks.md).

## CI first

- No completed run has a failed job. Push runs 40974794900 and earlier were cancelled before any job started (the
  jobs API lists none); push run 40976392370 (c28aaa54) was pending. Push run 40929108987 (1ebe9dc4) still runs
  win-asan in its serial Test step; its other 27 jobs passed. Nothing cancelled or dispatched.
- The row's "first win-asan run under the bound" (c364ab0d's three-at-a-time preset) has still not run: every push
  run after c364ab0d was replaced while queued.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has run.

## What landed (8a9351a0)

The last three hesap-direct consumers. `MultifrontalLDLT`, `MultifrontalQR` and `MultifrontalCholeskyBlr` run
`factorize_arm(pick)` with production's `factorize` on the default pick; the job count stays `factorize`'s parameter.

- **`FrontWalkPick`** (`direct_picks.hpp`), shared by the three: a level of at least `level_min` fronts runs
  front-parallel, each front on one job; a narrower level runs front by front on the main thread, where a front's
  gemms of at least `front.gemm_min_mnk` split across `front.workers` (`gemm_parallel`, gemm's bits). No setting nests
  a `parallel_for`. Defaults: every level front-parallel (LDLT, QR: today's walk), the serial walk (BLR: today's).
- **`MultifrontalLdltPick`**: the blocked Bunch-Kaufman's cut (128 fully-summed columns) and panel (128), both
  blocking, and its flush tile (192, scheduling); the main-thread fronts' flush gemms take `walk.front`. The panel must
  be at least 2: a 2x2 pivot at a panel start restarts at the same column on a one-wide panel and never advances (now
  asserted). Complex fronts keep the unblocked kernel at every setting.
- **`MultifrontalQrPick`**: the compact-WY panel (48) and cut (64 rows), blocking; the per-worker V, T and W scratch
  is sized from the panel; the main-thread fronts' three larfb gemms take `walk.front`.
- **`MultifrontalBlrPick`**: the BLR factor ignored `num_workers` (`(void)num_workers`) and walked serially. Its walk
  now runs a level front-parallel when every front of it is dense (a BLR front's interpolative decomposition, QR, SVD
  and Cholesky reach hesap-dense's own jobs); a front-parallel front is serial on its job. The BLR front's gemms take
  `walk.front` (default: `gemm_parallel_auto`'s 256K cut on the pool), a dense front's trsm and Schur gemms
  `dense_gemm_min_mnk` (default never, today's serial), its dpotrf Cholesky's own cuts. A failure now marks its
  front and skips only the fronts above it, so `info()` is the first failing front on every walk, as the serial walk
  reported it.
- One `detail::front_levels` (`front_levels.hpp`) replaces the four inline copies of the level schedule (LU too).
- **Kept serial (no arm):** the three solves (LDLT's refinement, QR's Q apply and back substitution, BLR's refinement)
  run on the calling thread.

## Corrected claims

- blr.cpp said the serial gemm and the deferred level-scheduled walk were due to the open debt
  `gemm-parallel-frame-arena-leak`; that debt was resolved on 2026-06-05. The walk is now an arm for the board.
- multifrontal_cholesky_blr.cpp said its fronts' gemms ran `gemm_parallel_auto` on the workers: only the BLR fronts'
  did; a dense front's trsm and syrk ran serial gemm. The moat's comment said the same; both corrected.

## Two defects found

- **Hermitian LDLH swapped without conjugation** (93de81ad). `ldlt_swap_sym` swaps the lower-triangle cells of a
  symmetric row and column exchange; for a Hermitian front the band between the indices and (k2, k1) stand for upper
  entries and must be conjugated. The arms' Hermitian stencil with delays solved at 4.2e12 times the bound (c64; 7.8e3
  in c32). The Bunch-Kaufman swaps and the delayed-column sort now swap with the Hermitian flag; a unit test compares
  the swap with P A P cell by cell.
- **svd asserted on a non-finite matrix** (f427f616). A NaN reaching a BLR front's recompression made the bidiagonal
  iteration fail, and every path asserted it converged. `svd` now returns quiet-NaN factors in their thin shapes;
  tested for NaN and +-Inf, tall and wide, both sides of the divide-and-conquer crossover, all four types.

## Tests

`test_front_walk_arms.cpp` on a four-worker pool, shared fixture `stencil_3d.hpp` (also the LDLT and BLR tests'):

- `ldlt_arms`: f64, f32, c64, c32 (LDLT and LDLH), an indefinite 4^3 stencil at Bunch-Kaufman's threshold on
  fundamental fronts (delays) and a 6^3 one at the production defaults; five scheduling arms at 4 to 32 jobs bit for
  bit (L, D, pivot kinds, permutation, delays); five blocking arms (cut 0 with panels 2, 3, 16, 128; cut 12 panel 8)
  against their own one-job bits under three flush tiles and the main-thread arms at 8 and 32 jobs, and the raw
  factor's bound 4 n eps; -0 and subnormal, NaN and a negative pivot.
- `qr_arms`: a 162 x 81 least-squares matrix, the same scheduling arms; six blocking arms (panels 1, 2, 7, 48, 256 at
  cut 0, and unblocked); R, rank, dead columns and the least-squares solution bit for bit, the bound on the solve;
  a zero column (rank deficiency).
- `blr_arms`: f64, f32 on an SPD 6^3 stencil with BLR fronts from 16 and all dense; six arms at 4 to 32 jobs bit for
  bit (L, fill, info); a non-SPD diagonal fails on every arm alike.

win-debug: ldlt_arms 4.6 s, qr_arms 2.6 s, blr_arms 15.6 s. The LDLT and QR moats run 1 to 32 workers, the BLR moat
1 to 32 pool workers.

## Board

`bench_hesap_direct_parallel` generalizes the LU board into rigs: LDLT (Laplacians shifted indefinite, blocking arms
gated by the refined solve: the relaxed default pivot threshold trades growth that refinement recovers), QR (the
convection stencils stacked over a coupled identity) and BLR (the SPD Laplacians at the production 256, 1e-6, 512).
`run_bench_direct_parallel.sh` runs the three arm tests first. No bench ran: the board runs at row close.

## Checks

win-debug builds of crd-hesap-direct, its tests, crd-hesap-dense-tests and the bench; the arm, `[ldlt]`, `[qr]`,
`[mfblr]`, `[blr]`, `[lu]` and `[svd]` cases pass; strict tidy clean on the changed C++; clang-format on the added
lines; Allman braces; the ctest guards.

## Left

hesap-amg, -iterative, -autodiff and -sched.
