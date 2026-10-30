# 2026-10-30 — SIMD-DISPATCH: batch_gradient's and the task schedulers' crd-jobs picks

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[the sparse LinearOps' levers](2026-10-30-simd-dispatch-jobs-operator-picks.md).

## CI first

- Push run 40976392370 (c28aaa54), still running: its eight failed linux-gcc and linux-clang-tsan jobs all stop at the
  test_cholesky.cpp:340 double promotion fixed at bc2bdb83 (each log read). Every completed run since was cancelled
  before a job started; 40987827197 (1a71b8c9) is pending. Nothing cancelled or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has run. Nothing flips.
- win-asan under the three-job bound ([session](2026-10-29-simd-dispatch-win-asan-bound.md)): the code landed on
  2026-10-29; its first green hosted run, the guard included, is still to come (40976392370's win-asan job is in
  progress).

## batch_gradient (hesap-autodiff, b080c41d)

- **`detail::BatchGradientPick`** (reverse.hpp): `jobs_min_samples` (a batch with fewer samples runs on the calling
  thread on `tapes[0]`; default 1) and `fold_jobs` (the fold's components split over that many jobs; default 1, the
  serial fold). `batch_gradient` is `batch_gradient_arm` at the default pick. The job count stays the caller's: the
  tapes are one per job, so a forced count is not a lever.
- A batch now runs `min(num_jobs, S)` jobs (none sat idle on an empty sample range), and one job or a pool that is down
  runs on the calling thread without `parallel_for`. Same bits: each sample's gradient is taken on a reset tape, and
  each component is summed over the samples in ascending order from 0.0 on every path (ADR-0097). A fold split over
  sample ranges would regroup the sums and is not offered.
- **Fatal in every build** (were a stack overrun of the 32-leaf array and unchecked span writes): more than 32
  parameters, and a `grad`, `gbuf` or `tapes` span shorter than the batch needs; specimen modes
  `crd-hesap-autodiff-fatal-batch-too-many-params`, `-batch-short-gbuf`, `-batch-short-tapes`.
- **Kept serial:** `jacobian`'s m backward passes share its one tape (a pass per job needs a tape per job, a different
  signature).

## The task schedulers (hesap-sched, c1d3f0c1)

- **`detail::SchedPick`** (task_graph.hpp): `jobs_min_tasks` (a TaskGraph batch, a `parallel_tiles_for` grid or a
  DependencyGraph level with fewer tasks runs on the calling thread in order; default 1) and `chunks_per_job` (the
  contiguous ranges each job takes; default 1, as before). Each scheduler takes it as a defaulted last argument.
- One job (0 included, which asserted in `parallel_for`) or a pool that is down runs the tasks in order on the calling
  thread. DependencyGraph's `std::min` is gone with it.
- **Outside the levers:** DependencyGraph's level barrier. An executor that overlaps levels is a second scheduler, and
  nothing in the engine calls hesap-sched yet (its tests and this board only); the hesap-direct walks have their own
  level schedule ([LDLT, QR and BLR](2026-10-30-simd-dispatch-jobs-ldlt-qr-blr-picks.md)).

## Tests

- `test_batch_gradient_arms.cpp`: n 1, 3 and 32; S 0, 1, 3, 37 and 200; NaN, +-Inf, -0 and the smallest subnormal in
  the features; seven picks at 1 to 32 jobs. grad and every gbuf row against `gradient` per sample on one tape folded
  in ascending order, byte for byte, with guard words after both; the public entry against the arm; a pool that is
  down at num_jobs 16 with one tape. win-debug 0.1 s.
- `test_sched_picks.cpp`: six picks at 0 to 32 jobs. TaskGraph batches of 0 to 500 tasks and tile grids 0 x 5 to
  17 x 13: each task once, its value written, nothing past the last; a 9 x 7 wavefront with a snapshot (read) and a
  write (read-write, so write-after-read) per tile: every edge's predecessor ended before its task started, the tiles
  are the serial walk's and every snapshot saw the wavefront's value; a pool that is down runs all three in index or
  ready order.

## Boards

`bench_hesap_autodiff_parallel` (`scripts/run_bench_autodiff_parallel.sh`): n 4 and 32, depth 1 and 16, S 16 to 64K;
one job against production, the sample cuts 64 and 1K and the fold split 4 and N. `bench_hesap_sched_parallel`
(`scripts/run_bench_sched_parallel.sh`): TaskGraph batches of even and uneven tasks, 16 to 64K, and DependencyGraph
wavefronts 8^2 to 128^2; one job against production, the cuts 64 and 1K and 4 and 16 ranges per job. Every arm is
gated bit for bit; a miss exits 1. No bench ran: the boards run at row close.

## Checks

win-debug builds of crd-hesap-autodiff-tests, its fatal specimen, crd-hesap-sched-tests and both benches; the new and
the existing batch_gradient and scheduler cases pass; strict tidy and clang-format on the changed files (added lines);
Allman braces; the ctest guards; check-master-plan; check-repository; check-generated. One guard run failed
`crd-check-environment-reads` at -j 8 and its immediate rerun and a second full guard run passed (output not kept): recorded as a
non-reproducing local event, not chased.

## Left

- Dense QR, a named retune consumer without a pick yet: its panel width (`kQrBlockSize`) and unblocked cut
  (`kQrUnblockedMax`, 128) as levers; it reaches the jobs only through `gemm_parallel_auto`, whose levers landed.
  `trsm` is serial: its column split is an arm the board judges.
- The batched fft transforms judged as jobs: hesap-fft has no jobs code, so the board needs a split of a batch across
  per-worker plans as its arm.
