# 2026-10-31 — SIMD-DISPATCH: the SELL residuals closed and the permute board

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (row-close boards: SELL, then permute). Before:
[the SELL driver fix](2026-10-31-simd-dispatch-sell-driver-fix.md). Evidence:
[permute board](../bench/2026-10-31-simd-dispatch-permute-board.md).

## CI first

- No completed run has a failed job left to fix: push run 41167488972 (`d9fdeae3`) passed; 41127915077's four
  failures were handled in the previous batch.
- GitHub's copy of main stands at `d9fdeae3` while GitLab's is at `8daab825`: the push mirror has not carried the
  twelve commits since 07:10 (`85c5c1bd` to `8daab825`, the SELL driver fix and the per-case kernel-level CTests
  included), so none of them has a hosted run. Reported to the operator; no complete-tier run was dispatched (it
  would test `d9fdeae3`).
- Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run. Repository variables `FTL_GCP_PROJECT`,
  `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only), no secrets: the AMD and Mac lanes wait on their
  accounts.

## SELL residuals

The SELL family is accepted under the 2026-10-30 rule (no cell behind pre-row beyond noise). Its two cells behind a
peer each had one look:

- The Auto cut between the L2 and the L3 (f64 uniform-4, Auto's grouped Delta16 build 10% behind its U32 build,
  0.959x Eigen, pre-row 0.995). `sell_auto` separates the cases by Delta16 share, not size: banded-5 at 110,000 rows,
  every slice Delta16 and in the same band, runs 1.24x faster than its U32 build, so moving the cut to the L3 trades
  one cell for the other. No lever named; recorded.
- Portable f32's U32 body at half the L2 (1.32x to 1.47x behind its own Delta16 arm). The GCC 13 `-O3` portable unit's
  two instantiations (`sell_form<ScalarBody<f32, 8>, false|true>`) run the same scalar column loops: eight x loads,
  eight `mulss` with u32-wrapped value offsets, and per-lane branches in the masked tails of both. No codegen cause
  named; portable runs only where no SIMD level exists. Recorded.

## Permute board

A probe (one source on the pre-row and HEAD trees, the binaries alternating) put five one-thread cells 15% to 27%
behind pre-row (f32 2D and 4D, c32 and c64 2D, c64 3D). `bench_permute`'s arms named the cause: the next-tile
prefetch the per-level kernels gained in the row (`08da3328`), the rules' distance 1. Without it 47 of 48 cells ran
faster (1.05x to 1.92x; one thread at avx2, sse4.2 and sse2, 8 and 16 workers at avx2). With no prefetch the
multithreaded f32 cells still trailed pre-row by 13% to 30%; the staged full-column strips beat the unstaged tiles in
all 24 multithreaded cells (1.07x to 1.85x).

- `PermutePick::distance` defaults to 0. The hint arms reach the next tile, and HPTT's order and the scalar tile
  keep the rules' distance, so each arm moves one lever.
- Every multithreaded task stages its tiles of a kernel element through its thread's frame arena. The bool
  `PermuteArm::staged` became `unstaged`, the arm; the arm tests keep a staged and an unstaged multithreaded arm.

After both, no cell trails pre-row in every round (one thread 1.00x to 2.45x, 8 workers 1.15x to 1.85x, 16 workers
1.19x to 1.67x, medians). HPTT trails at avx2 in all but two one-thread cells (f32 4D 0.97, f64 3D 0.99). At sse4.2
and sse2 on one thread the 16-byte transposes trail HPTT's AVX kernels on f32 and f64 3D, and portable's scalar tiles
on f32 and f64, recorded under the board rule. No other arm beats the rules in every cell of a level.

Checks: GCC 13 `-Werror` lane build of `bench_permute` and the probes; win-debug `crd-hesap-tensor-permute-tests`
(15 cases) and `crd-hesap-tensor-tests "[kernels]"` (19 cases) pass.

## Left

The sse2 and sse4.2 bodies' boards, the jobs-retune thread sweep, then fft f32 against MKL.
