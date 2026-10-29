# 2026-10-29 — SIMD-DISPATCH: the permute board's arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 5, the permute board against HPTT).
Before: [the SELL board driver](2026-10-29-simd-dispatch-sell-board-driver.md).

## CI first

- No completed run has a failed job that earlier batches had not traced. Push run 40869334591 (e496d16c) has one
  failure, win-shipping's TickClock case, fixed at d3b0daf1; its win-asan and linux-gcc-arm64-asan jobs started at
  12:18 and 12:22, before 5cf0e54e and a1901325, and still run. Nothing was cancelled.
- Every push run since was replaced in the push group before a job started, so a1901325 has not yet run on
  linux-gcc-arm64-asan and 5cf0e54e not on win-asan: both clauses wait on the first run that reaches those lanes.
- DIAG.11c and DIAG.12 stay Needs CI: no green complete-tier run, and none can be dispatched while a run is in progress.

## What landed

- **Kernel levers** (`tensor_kernels.hpp`, plain data): `PermutePick` carries the next-tile prefetch hint (T0, T1,
  T2, NTA), its distance (`permute_region_tiles` now finds the tile 0, 1 or 2 ahead, into the next region when past
  the last, still only inside its extents), the destination prefetch, the scalar tile in place of the register
  transposes, and the outer order. `PermuteOrder::Hptt` is HPTT's `loopCostHeuristic` (src/transpose.cpp): each
  outer dim weighs 2^(m - its source stride rank) + 1.01 * 2^(m - its destination stride rank), the heaviest
  innermost. The defaults are HEAD's choices.
- **Serial non-temporal stores**: `PermuteWalk::stream`, under the multithreaded path's alignment gate.
- **Staged strips** (`permute_tile_staged`, every element type and level): a tile's micro column groups run outer
  into the stage, so each source column is read whole, then each stage row is stored in one sweep, non-temporally
  when the region streams. The deleted f32 AVX2 kernel of 2026-07-03 kept a 64 KB stage on the stack, which is the
  whole small fiber stack. The stage now comes from the worker's frame arena inside the job (the job never waits, so
  the mark stays on its thread); a refusal runs the unstaged tiles, the same bits.
- **`detail::permute_copy_arm`** runs a `PermuteArm`: the pick, the serial tile edge, the multithreaded tiles and
  super-blocks, forced or refused (`Unsupported`) non-temporal stores, staged strips. `permute_copy` runs the rules.
  Board scaffolding: the keep-or-drop step folds the winners into the rules and deletes the arm.
- **The sse2 16-byte body** already exists (one element a register, the block and prefetch loop); the scalar-tile arm
  at the forced sse2 level is its board comparison, so no new body was written.
- **`bench_permute`** is the board's one binary: f32, f64, c32 and c64 over the three HPTT cases. `--one-thread` times
  the serial walk's arms, `--threads N` the multithreaded ones, round by round on preallocated tensors. Each arm is
  gated against a reference walk of `src.permute(order)` first; a refusal or a miss exits 1. Before, it ran f32 only,
  timed allocation with the permute, and returned 0.
- **`scripts/run_permute_bench.sh`**: the permute gates, then per level the serial phase on one P-core thread and the
  threaded phase on 8 and 16 workers, then the HPTT harness at 1, 8 and 16 threads.

## Tests

- `[kernels]`: every region case runs five picks (other hints, distances 0 and 2, no destination prefetch, the scalar
  tile), staged and not; every walk case runs them with HPTT's order and, where the extents allow, non-temporal
  stores. All are checked against the scalar oracle with sentinels past the region.
- `[permute]`: a new moat case runs six arms, serial and at 1, 3, 7, 8 and 16 workers, unscaled and scaled, for all
  four element types. The shapes are a transpose, a rotation with source columns 80 KiB or more apart (tall tiles,
  staged strips) and a reversal whose rows are no 32-byte multiple. Each arm matches `permute_copy`'s bits, and a
  forced stream is refused exactly on the reversal.
- win-debug: `[kernels]` (19 cases) and the permute program (15 cases) pass, the permute program also at forced
  portable and sse2. Strict tidy is clean on the eight changed files; the bench went through a scratch database of
  test_permute's command, and it compiles objects-only at /W4 /WX. clang-format has no finding. Allman braces,
  allocation results, plan and repository checks pass. CI's linux-gcc-debug builds the bench.

## Left

- The board itself, in the row-close pass. The HPTT harness (external/hptt, a local clone) times f32 only, so the
  f64 and complex rows need HPTT's double and complex plans from a pinned HPTT. That is a download to ask for then.
