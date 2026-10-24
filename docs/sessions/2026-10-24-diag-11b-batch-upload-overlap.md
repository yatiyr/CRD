# DIAG.11b: the overlapping uploads in one Vulkan batch, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [hosted flaky readings](2026-10-24-diag-11b-hosted-flaky-readings.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failed job that main does not repair. 40067359705's
four failed lane jobs and the scheduled 40075929537's one (job 120819184913, `test_diag_queue_pressure.cpp:290`, the
high water below its count) are the previous batch's repairs; every later completed run was cancelled with no failed
job.

## Which two uploads overlapped

The previous batch's follow-up, taken before the target clear values because it is the smaller slice. The stub
raster of `test_scene_render.cpp` records every upload; moving one entity on two consecutive syncs uploaded its
run's four sections twice in the second sync: instances `[12800, +8800)`, bounds `[56640, +2640)`, LOD override
`[66880, +880)` and previous world `[80688, +7040)` (110 slots), 24,640 bytes instead of 12,320. A run that moved
last sync is queued once to advance its `prev_world` and again by `pass_update` when it moves again; both copies
carry the same bytes. The render workload moves the same 64 instances every frame, so every frame repeated them, and
in one Vulkan batch those were the overlapping copies synchronization validation reported.

## Change

- `SceneRenderer::sync` queues a run once per sync: `RunEntry::queued` holds the `sync_serial` that last queued it
  (u64, so it never wraps). `pass_update` records this sync's movers in `prev_dirty_runs` whether or not they were
  queued already, so a run that moves and then stops still advances once.
- The batch barrier stays (any caller may upload a range twice in a batch) and gets a test of its own beside
  DIAG.7a(g-2): three overlapping `upload_storage` calls in one batch under synchronization validation, no
  `WRITE_AFTER_WRITE` and no other message.
- No new file outside this note.

## Teeth

- HEAD: the new scene-render case fails (24,640 == 12,320 and the four overlapping pairs above).
- `prev_dirty_runs` filled only for newly queued runs: the sync after the last move uploads 0 instead of 12,320.
- The batch barrier removed: the new Vulkan case reports `WRITE_AFTER_WRITE` (2 messages) on this host's GPU
  (win-debug) and on lavapipe (linux-gcc-debug). With the dedupe in and the barrier still removed,
  `crd-diag-overhead-render-validation-sync` and `-core` pass on linux-gcc-debug, so the render workload has no other
  overlapping copy.

Each restored, rebuilt and rerun green.

## Verification

win-debug: crd-scene-render-tests in full (115 cases), the crd-diag-overhead CTests, the DIAG.7a(g*) and new
Vulkan cases. linux-gcc-debug (mirror): crd-scene-render-tests in full (82 cases), the same CTests (30). The ctest
guards, `tidy-files.py` (LLVM 20) clean on the four files, `clang-format --dry-run` at HEAD's finding count on each,
`check-allman-braces.py`, `check-diag-runbooks.py`, `check-master-plan.py`, `check-repository.py`.

The [device validation board](../bench/2026-10-24-diag-11b-device-validation.md) was measured with the repeated copies;
the change only drops the moved runs' second copy each frame, so the board is not re-run.

Hosted once pushed: the scene-render case passes on every lane, the Vulkan case on every lane that has a Vulkan
device (lavapipe on Linux), and the Linux lanes' `crd-diag-overhead-render-validation-sync` stays clean.
