# DIAG.11b clause (6): the jobs adapter's token table without a lock, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b). Rules: [AGENTS](../../AGENTS.md). Previous batch:
> [sampling attribution](2026-10-24-diag-11b-sampling-attribution.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40029693899,
40022015924, 40020783027, 40018724265, 40014687187 and 40014471031 were cancelled with no job. 40006451399 was
running, every finished job green; not waited on. The red nightlies 39923363836 and 39746853196 are the three repairs
of 2026-10-23.

## The change

The previous batch's re-read of the 2026-10-19 trace put 359 of 398 callback samples of jobs full in system frames
under `cb_on_job_begin`/`cb_on_job_end`: the global `g_tokens_mutex` of `park_token`/`take_token`, taken at every
job's begin and end. The open-addressed table already had atomic keys and a full-table probe, so the mutex is deleted
and nothing else is added:

- `park_token` claims an empty slot with a sentinel key (the address of a static byte), writes the token, then
  publishes the fiber's key with a release store.
- `take_token` claims the slot back from the key (acquire), reads the token and frees the slot (release).
- The uninstall drain clears only published keys, so it never frees a slot whose token a park or take is using.

## Evidence

New case "concurrent job begins and ends never lose a parked token" (`tests/foundation/perf/test_jobs_adapter.cpp`):
12 threads keep 32 jobs each begun at once (384 of 512 slots) and end them in reverse, 200 rounds, through the
installed observer. Teeth, each restored, rebuilt and rerun green:

- A check-then-store claim in place of the CAS: win-debug lost 6, 10 and 14 of 76,800 tokens in three runs.
- The slot freed with a relaxed store: linux-clang-tsan reports a data race between `park_token`'s token write and
  `take_token`'s read.

crd-perf-tests passes on win-debug and linux-gcc-debug; its adapter cases pass on linux-clang-tsan with no report.

Cost, `diag-overhead.py run --workloads jobs --arms off,full` on win-shipping-profile with the 2026-10-16 method (16
processes, 200 frames after 20, 8 threads, affinity FFFF), host idle. Each source was built and measured twice,
alternating (before, after, before, after); the off arm does not install the adapter, so full over off compares
across the builds:

| Build | off p50 µs | full p50 µs | full over off (95% interval) |
| --- | ---: | ---: | --- |
| with the mutex | 401.3 | 723.8 | +83.47% (+75.35% .. +85.71%) |
| without | 414.4 | 537.5 | +30.03% (+26.12% .. +34.55%) |
| with the mutex, rebuilt | 397.7 | 722.1 | +81.78% (+71.29% .. +86.74%) |
| without, rebuilt | 400.0 | 529.7 | +31.70% (+21.65% .. +39.19%) |

The full frame drops by about 190 µs, 93 ns per job of its 2,048. Full is a reported budget; the remaining cost is the
recorder's ring mutex per kept event (one per job) and the profiler's per-job work, both still owned by clause (6).
Showing that the callbacks' samples leave the system lock frames needs a new external recording, the user's elevated
`scripts\elevated-captures.bat sampling` that the alloc cells already wait for.

## Not done in this batch

- Clause (6) next: the recorder-wide ring mutex per kept event, then the CEIR bridge's per-instruction timing.
- (l) zstd 1.5.7 and (o) clang-tidy 20 are downloads; each waits for the user's own approval in the conversation.
