# 2026-10-31 — SIMD-DISPATCH: the SELL driver's narrow-row cost fixed at its cause

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (row-close boards, the SELL family). Before:
[the SELL board](2026-10-31-simd-dispatch-sell-board.md). Evidence:
[record](../bench/2026-10-31-simd-dispatch-sell-driver-fix.md).

## CI first

Push run 41127915077 (`3db4573f`) completed with four failed jobs:

- win-debug and win-debug-scalar: `crd-ci-orchestrator-e2e` (`test_1_webhook_build_runs_end_to_end`, a build `passed`
  with only its `plan` job, still `scheduled`). A `ci/` defect, reported to the operator, not edited here:
  `Coordinator.create_build` indexes the build active before `upload` adds the bootstrap job, and `settle` computes
  `state.build_outcome` from a read of the build with no jobs (an empty job list returns `passed`), then writes that
  outcome through a compare-and-swap whose change function does not recompute it. A scratch script that lets the
  upload land between `settle`'s read and its write reproduces `passed [('plan', 'scheduled')]`, off the active
  index. Also: `LocalStore.get` reads without the object lock and raises `PermissionError` on about 1.4% of reads on
  Windows while another process replaces the object.
- linux-gcc-public-checks: exit 143, "The runner has received a shutdown signal", 40 minutes in, during
  `crd-hesap-direct-kernels-at-sse42`, the rest of the run continuing. That program peaks at 2.6 GiB (measured here at
  sse4.2) on a 16 GiB runner: a hosted runner shutdown, nothing to carry.
- win-asan: `crd-hesap-fft-kernels-at-sse2` reached the 3,600 s test timeout, as in run 41112483075. The fft
  `[kernels]` arms grew 2.6x on win-debug (sse2 363 s to 958 s) since `338d0c4d` ran the forced-forward and own-plan
  cases for f64 too. Here the sse2 arm took 1,015 s in 17 cases, the largest 293 s (f64 forced forwards at 2M and
  4M), none dominating. `crd_add_kernel_level_tests` now registers each case of an arm as its own CTest,
  `<suite>-kernels-at-<level>: <case>` (934 on win-debug, from 85 arms), so no arm's sum meets the timeout and win-asan's
  three jobs spread the cases; the `-at-sse2` passes still leave them out by name, the SDE arms (at most 335 s) stay
  one process. Catch2 names a discovery's file by a hash without the prefix, so each level passes its own unused
  `OUTPUT_SUFFIX`.

Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run. Repository variables `FTL_GCP_PROJECT`,
`FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only); no secrets, so the AMD and Mac lanes wait on their
accounts.

## Cause and fix

`sell_bisect` copied production's lone-slice path and changed one thing at a time: writing a whole slice's rows from
the register, or handing the body a view of `vals` alone, changed nothing; carrying each slice's value and column
offsets on from the previous slice with the view's arrays in locals took 15% to 20% off, and dropping the pair test
another 6% to 12% (the record's table). Both landed (`85c5c1bd`):

- `SellCursor` carries the values offset, the U32 column pointer and the Delta16 gap pointer from slice to slice (each
  slice's elements follow the previous slice's in its form's array), so no slice reads `slice_ptr` or `delta_ptr`.
- `SellMatrix::max_width` (in the kernel view): a call whose pair width exceeds it runs every slice alone without the
  pair test. The layout check holds it to the widest slice (its tooth: `max_width` left 0 fails it).

The board pass then showed f64 uniform-4 between the L2 and the L3 still 0.76x of pre-row. `sell_auto` found the
Auto build there holding 45% of its slices Delta16, interleaved with U32 ones, 0.74x of its own U32 build: the
driver's per-slice form branch. `to_sell` now orders a Delta16 build's rows by the form they allow first (rows longer
than 2 with every gap within 16 bits after the others) in each σ window, so a window runs one U32 run and one Delta16
run (`05dbbc1c`); that build went to 0.90x of U32 and 0.995x of pre-row. A new case builds alternating rows and holds
one form change and every Delta16-able row in Delta16 slices (its tooth: the ordering off gives 0 of 512); the layout
check bounds the changes per window. Same bits as the CSR spmv on every level; the sparse `[kernels]` arms pass at
portable, sse2, sse4.2, avx2 and avx512 (under SDE 10.13.1, `-spr`).

## Result

Same matrix, same process, eight layouts: f64 uniform-4 1.017x of pre-row's loop (was 0.782), banded-5 1.087x (0.802),
f32 uniform-4 1.158x (0.976). Pass 7 of the board (both fixes) puts every narrow cell the five passes held slower at or
above their noise but the probe binary's f32 uniform-4 half L2 cell (a layout, per the record), and Eigen's lead shrinks to one avx2 cell (f64 uniform-4 between the L2 and the L3, 0.959). Left
for the SELL family: that cell's Auto cut (U32 runs 10% ahead of the grouped Delta16 build) and portable f32's U32 body
at half the L2, 1.32x to 1.47x behind its own Delta16 arm (pre-existing, 1.41x to 1.51x in the five passes).
