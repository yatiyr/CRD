# DIAG.11b (6a): the full mode's jobs cost attributed, two shared writes repaired, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Previous batch: [jobs on the CPU per frame](2026-10-24-diag-11b-jobs-cpu-per-frame.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40006451399 (head
`6a964937`) failed only win-asan's gate, the WARP read `a832934d` repairs. 40041122184, 40039075298, 40029933433,
40029693899, 40022015924, 40020783027, 40018724265, 40014687187 and 40014471031 were cancelled with no job.
40032224637 was running (its win-asan job, the first on `a832934d`, not finished) and 40047173030 pending; not waited
on.

## Attribution

Full on jobs with a four-thread pool read +48% to +50% over off on Windows and +39% on linux-gcc-relwithdebinfo. By
experiment (each an uncommitted edit of HEAD, restored): one process per variant per round, 8 rounds alternated, 300
frames after 30 warmup, `--threads 4 --affinity 5555`, median frame of the per-process medians against HEAD's off in
the same rounds.

| Full-mode variant | win-shipping-profile | linux-gcc-relwithdebinfo |
| --- | ---: | ---: |
| HEAD | +46.6% | +37.1% |
| the adapter not installed | +25.9%, +25.8% | +17.7% |
| neither the adapter nor the job's site | -3.7%, -4.4% | -0.4% |
| no adapter; the site formats its payload, `record()` returns before the clock | +13.1% | +9.4% |
| no adapter; `record()` claims its sequence but does not publish | +16.8% | +12.6% |
| the adapter without its `register_thread` call per job | +36.4% | +30.5% |
| the adapter without its tallies | +41.7% | +34.0% |
| the harness's payload counter removed | +46.5% | |

Per point of a frame on Windows: the payload's `snprintf`, which full evaluates at every site by design, about 17;
the slot write about 9 (152 bytes per event into a 1,024-event ring that 2,048 events a frame lap twice, written by
whichever thread claimed the sequence); the recording word's claim with the clock read about 4, so the
compare-exchange the previous batch suspected is not the cost, and it is the recorder's total order. The adapter about
21: `register_thread` about 10, whose reader pin is two seq_cst read-modify-writes of one global counter
(`g_in_flight`) at every job; the five tallies on one cache line, two adds a job, about 5; the rest, two clock reads,
the token claim pair and the sample write, about 6. Padding each token slot to a cache line changed nothing (+36.1%
against +35.9%, 10 rounds), so the table stays as it is.

## Repairs

- `register_thread` (`engine/foundation/perf/src/profiler.cpp`): a thread whose cache belongs to the live lifetime
  (its generation is the latest `init()`'s stamp, compared before the state is read) and carries the same name returns
  without the reader pin. It records into that state, so the "join every recording thread before shutdown" contract
  covers it as it covers `push_region`; a stale cache, a changed name and an inactive profiler take the pinned path as
  before. A first form read the live state's generation before the match, which a thread of an earlier lifetime is not
  entitled to; reordered in a second commit (tooth 1 rerun on it, `[thread]` and `[quiescence]` pass).
- `StripedCount` (`engine/foundation/perf/src/striped_count.hpp`, new): the flight recorder's striped left-out count,
  moved out of `flight_recorder.cpp` so the jobs adapter's five tallies use it too. A new header because the two users
  are separate units of crd-perf; the recorder's copy is deleted.

## Boards

`diag-overhead.py run --workloads jobs --arms off,basic,full --processes 96 --iterations 200 --warmup 20 --threads 4
--affinity 5555`, host idle, boards alternated (HEAD, repaired, HEAD, repaired). Full over off, frame time and CPU per
frame, 95% intervals:

| Tuple | HEAD | Repaired |
| --- | --- | --- |
| win-shipping-profile | +46.79% (+45.53 .. +47.83), CPU +46.90%; +47.59%, CPU +49.11% | +31.84% (+31.04 .. +32.74), CPU +32.23%; +31.48% (+30.40 .. +32.68), CPU +32.91% |
| linux-gcc-relwithdebinfo | +40.22% (+39.26 .. +41.17), CPU +39.25%; +40.15%, CPU +39.27% | +36.47% (+35.11 .. +37.43), CPU +34.37%; +35.61% (+34.24 .. +36.72), CPU +34.87% |

Basic stays within on all eight boards (-3.31% to +0.23%; CPU -3.07% to +0.04%). Binaries (SHA-256 prefixes): Windows
HEAD `bd194fddbbcc`, repaired `9eb2c3f6ebb6`; Linux HEAD `b6a1f753d831`, repaired `0d91c1adac57`.

Two readings for (1a), the two-binary floor: the repaired Linux binary's off arm reads 5.3% to 7.5% slower than
HEAD's (365 µs against 385 to 396 µs) although off runs none of the changed code; every crd-jobs global and the
workload's arrays keep their offsets within a cache line (`nm`), and only their page offsets moved (`g_slices` at
0xd60 within its page, now 0x160). On Windows the off arms agree (363 to 371 µs). And on the same binary basic reads
1.4% to 3.3% below off on Windows (the host's watchdog thread is the only addition), not yet explained.

## Teeth

Each restored, rebuilt and rerun on win-debug (295 cases pass):

- the generation compare dropped from the fast path: the new registration case fails (the stale thread takes the
  index another thread records into, so two rings are counted, not three);
- the fast path returning without the name check: the same case fails (the thread keeps "main" after re-registering
  as "main.renamed");
- `StripedCount::total()` reading only the first stripe: 7 cases fail, the adapter's exact job counts and the flight
  recorder's left-out counts.

## Verification

win-debug: crd-perf-tests (295 cases) and the 12 `crd-diag-overhead-*` CTests; linux-gcc-debug and linux-clang-tsan
through `wsl-lane.sh`: crd-perf-tests (293 cases; under TSan exit 0 and no `ThreadSanitizer` line) and the 12 CTests; `tidy-files.py` clean on the
five files, clang-format findings per file unchanged from HEAD, `check-allman-braces.py`, the ctest guards.

## Not done in this batch

- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads; each waits for the user's own approval in the
  conversation.
- What is left of full on jobs is attributed and reported, as ID-5 sets no full budget: the payload formatting, the
  slot write, the recording word, and the adapter's clock reads, token claims and sample write.
