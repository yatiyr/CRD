# 2026-10-30 — SIMD-DISPATCH: the supernodal Cholesky's crd-jobs levers as forced arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[the eigensolvers' levers](2026-10-30-simd-dispatch-jobs-eig-picks.md).

## CI first

- No completed run has a failed job. Every cancelled push run from 40894140703 to 40967252654 had no job started
  (the jobs API lists none). Push run 40929108987 (1ebe9dc4) still runs win-asan and linux-gcc-asan; its other 26 jobs
  passed.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has completed. Nothing
  cancelled or dispatched.

## What landed

The retune's fourth consumer, hesap-direct's `SupernodalCholesky`, the first of its sparse factors.
`crd/hesap/direct/detail/direct_picks.hpp` holds `SupernodalPick`; `factorize` is `factorize_arm(SupernodalPick{})`
and `solve` is `solve_arm(SupernodalPick{})`, so production and the arms share one body. `solve_arm`'s
`solve_workers` replaces the `solve_with_workers` hook (its six test calls moved).

- **Scheduling levers, bit for bit:** `node_min_cols` (512: a thin etree level runs node-parallel only with a front
  this wide), `gemm_min_flop` (1M: a node-parallel front's gemms and subtracts split), `cmod_syrk_min` (48: f64 serial
  cmod's syrk), and the solve's `solve_min_work`, `solve_max_workers` and forced `solve_workers`.
- **Blocking levers, by the bound:** `cdiv_scalar_max` (32), `cdiv_block` (256) and `cdiv_inner` (64), which replace
  the profile build's `CRD_CDIV_BLOCK` knob, and `solve_batch_min_nc` (48).
- **A claim corrected:** the batched and per-RHS diagonal solves were documented as bit-identical. They are not: the
  per-RHS back-solve subtracts each row's dot product once, the batched form subtracts term by term. The test
  measured it on every type, so the batch width is a blocking lever and the comments say so.
- **A scratch defect, fixed (read from the code):** the blocked cdiv's (B) walk keeps a 96-row block of the outer
  panel and an MR x 64 snapshot at `96 x 256` in the per-worker buffer, sized `max_nr x max_nc`. A matrix whose widest
  fronts are short (the new 40-column fronts over 112 rows: 8,064 elements against 24,576 + MR x 64) wrote past it.
  The buffer now covers the walk, the snapshot sits after the block actually used, and the pack offsets come from one
  helper (`cdiv_pack_next`) instead of a fixed four-entry table, so any panel pair fits.
- `panels()` and `panel_offsets()` expose the factor read-only; the tests and the board compare its lower trapezoid.

## Tests

`supernodal_cholesky_arm` (`test_supernodal.cpp`), f64, f32, c64 and c32, on a bordered block matrix (40-column
fronts under a 72-column root) and a 24 x 24 grid, on a four-worker pool on every host (a hosted runner's), so the 8 to
32 forced jobs outnumber the workers and the pool-sized scratch is exercised as CI sees it:

- 14 scheduling arms at 1 to 32 jobs (every front node-parallel and split, never split, tree only, syrk always and
  never), bit for bit against one job;
- 7 blocking arms (all blocked, all scalar, panels 64/16, 40/24, 8/8, 256/256 and 1/1), each bit for bit at 8 and 32
  jobs against its own one job, and within the normwise backward error 4 n eps of a solve;
- specials: a negative pivot and a NaN off-diagonal give `info()` k + 1 on every arm, -0 and subnormal entries;
- the solve on 1 and 5 right-hand sides: jobs 4 to 32 and the gate bit for bit, both batch widths by the bound and
  bit for bit across jobs, guard words after the right-hand sides, a failed factor refused on every arm.

## Board

`bench_hesap_direct_parallel`, one process per `--threads N`: the numeric refactor of 3D Laplacians (16³, 24³, 32³),
a 256² Laplacian and a bordered matrix with a 640-column root, all under AMD, f32 and f64, at one job and down 14
factor arms, then the solve on 1 and 16 right-hand sides down 6 arms. Bit gates, the bound for the blocking arms, a
miss exits 1. `scripts/run_bench_direct_parallel.sh` runs it at 4, 8, 16 and 32 workers per level. Each arm keeps its
factor resident (14 and the control), a few GB at 32³ f64 from the growable allocator.

## Checks

win-debug builds of crd-hesap-direct-tests and the bench; the new case (3,700 assertions, 9.6 s; it carries
`[kernels]`, so the forced-level and SDE arms rerun it) and every "supernodal Cholesky" case pass; strict tidy clean
on the five changed C++ files; clang-format on the added lines; Allman braces; the ctest guards (the include budget
gains `direct_picks.hpp`). The ISA guard failed once on its first run after the build, then passed four times running
alone and in the set; its output was not kept, so the cause is not known. No bench ran: the board runs at row close.

## Left

hesap-direct's supernodal LU, multifrontal LU, LDLt, QR and BLR and the dense LU kernels, then hesap-amg,
-iterative, -autodiff and -sched.
