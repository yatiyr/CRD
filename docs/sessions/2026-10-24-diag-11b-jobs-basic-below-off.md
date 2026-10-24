# DIAG.11b (4a): jobs basic below off explained; the left-out tally without a locked add, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Previous batch: [instruction counts](2026-10-24-diag-11b-instruction-counts.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40006451399 (head
`6a964937`) failed only win-asan's gate, which `a832934d` repairs. 40050623248, 40050313292, 40047173030, 40041122184
and 40039075298 were cancelled with no job (checked per run). 40032224637 (head `5066e492`) was running, 22 of its 25
jobs green; not waited on. Its finished jobs are the first hosted evidence for two follow-ups:

- r1, job 120694540080 (`windows (win-asan)`, WARP): the five former TP-1 DXR tests pass, no AddressSanitizer
  report, 7,806 results, `gate: no registered failures for this lane`.
- r2, job 120694540201 (`linux-gcc (linux-gcc-asan)`): no LeakSanitizer report, the lavapipe Vulkan tests pass,
  `diag 3f: a memory leak is caught by LeakSanitizer` passes, gate PASS on exactly TP-5.

## (4a): what makes basic read below off

win-shipping-profile, jobs workload (2,048 jobs of 512 multiply-adds a frame), 200 frames after 20 warmup, affinity
5555, host idle. Each table alternates one process per entry per round and compares medians of per-process medians
inside one binary. Experiment binaries are HEAD's source plus one temporary edit each (environment switches read once at
setup), removed before the commit; SHA-256 prefixes: HEAD `385dddab2bab`, switches `89e748074a1e`, plus padded result
slots `361915b39de2`, plus a dequeue race count in `ConcurrentQueue::try_pop` `409aec9629dc`.

| Four threads, 150 rounds (switches binary) | Frame | CPU per frame |
| --- | ---: | ---: |
| basic | -1.97% | -1.48% |
| basic, its site skipped (the host, its threads and the watchdog all kept) | -0.15% | +0.44% |
| basic without the watchdog thread | -1.14% | -0.86% |
| off plus one relaxed `fetch_add` per job on a thread-private atomic | -1.42% | -1.14% |
| off plus a plain loop of 2, 4, 8, 16 iterations per job (120 rounds) | -0.48%, -0.48%, +0.50%, +1.88% | -0.33%, -0.03%, +1.16%, +2.32% |

- Not the watchdog thread or anything else the host adds: basic with its site skipped reads like off. A 250 ms window
  does not even fire inside the 200 timed frames; a 2,000-frame warmup leaves basic at -1.43%.
- The site's locked add: one private locked add per job reproduces basic; a plain delay of similar length does not.
- Not false sharing on the result slots: with each slot on its own cache line, basic -2.47% and off plus the add
  -1.82% (site skipped -0.33%).
- The mechanism is the pool's contention: every job comes off one shared injection queue (2,048 pops a frame; four
  threads spend 694 ns of thread time per job, against 205 ns on one thread). Lost dequeue races per pop: off 0.108,
  basic 0.093, off plus the add 0.098, the plain loop 0.103, basic without its site 0.109. A locked instruction
  between jobs spaces the workers' dequeue attempts; this is a contention response, not cycle accounting.
- On one thread there is no contention and the same add is the cost: basic +2.17%, off plus the add +2.05% (CPU
  +1.99%, +1.46%). HEAD's one-thread boards read basic +1.44% (+0.97 .. +1.88), CPU +1.45% (+1.01 .. +2.11),
  **unresolved** against 2%, then +0.91% (+0.40 .. +1.30), within: the four-thread "within" had been the pool's
  response masking a site cost near the budget.

## Repair

`StripedCount` (`engine/foundation/perf/src/striped_count.hpp`), the left-out tally and the jobs adapter's tallies:
the first 48 threads to add in the process each own a stripe and add with a relaxed load and store, no locked
instruction; later threads use `fetch_add` on 16 separate shared stripes no owner writes. The owned count never passes
48, so an index is never reused; `reset()` needs quiescent adders, as its two callers already are. `add()` still reads
one `thread_local` and holds nothing across a suspension point; no target builds with `/GT`. The new case starts
96 `std::thread`s, not jobs: the stripe belongs to the OS thread, and a pool would not guarantee 96 distinct ones.

The site microbenchmark (`events`, basic, 48 processes, two boards of each binary): 3.2 ns a left-out site at HEAD,
1.0 ns repaired (off 0.2 ns). Jobs boards (`diag-overhead.py run --workloads jobs --arms off,basic --processes 96`),
repaired `0f4b3d13ff50` against HEAD:

| Threads | HEAD basic vs off: frame, CPU | Repaired basic vs off: frame, CPU |
| --- | --- | --- |
| 1 | +0.91% (+0.40 .. +1.30), +0.86% (+0.47 .. +1.21) | -1.27% (-1.84 .. -0.56), -0.93% (-1.53 .. -0.40) |
| 4 | -2.10% (-3.17 .. -1.42), -1.76% (-2.62 .. -0.83) | +0.44% (-0.39 .. +1.47), +0.10% (-0.74 .. +1.14) |

Within on both. The four-thread reading now shows a small cost instead of the pool's response (+0.93% and +0.66%
alternated on two builds). Pairs inside one binary also move with the build at one thread: the repaired source plus
the switches (`b900ee7f797a`) reads basic +1.90%, its site skipped +0.80% (a leaked 4 KiB to 1 MiB allocation before
the pool moves off by +0.15% to +0.56% only). So the jobs basic verdict rests on count times cost, as clause (1)'s
does: 2,048 sites a frame times 0.8 ns is 1.6 µs, at most 0.4% of a one-thread frame (HEAD: 3.0 ns, 1.5%).

(2) is not this tally: alloc runs 16 sites a frame, and basic stays over on both binaries (p50 85.6 to 90.4 µs at HEAD,
85.4 to 91.1 µs repaired).

## Teeth

Each restored, rebuilt and rerun on win-debug:

- the owned add storing the loaded value unchanged: the four basic `crd-diag-overhead-*` CTests and three `[sites]`
  cases fail;
- the shared add as a plain load and store: the new 96-thread case fails 3 of 3 (15,392,566 of 19,200,000 counted;
  at 20,000 adds a thread it passed, so each thread adds 200,000);
- the shared stripes mapped onto the owned ones: the new case fails (lost 14,123 at 20,000 adds a thread).

## Verification

win-debug: crd-perf-tests (296 cases), the 12 `crd-diag-overhead-*` CTests, the ctest guards; linux-gcc-debug and
linux-clang-tsan through `wsl-lane.sh`: crd-perf-tests (exit 0; under TSan no `ThreadSanitizer` line) and the 12
CTests; `tidy-files.py` clean on the two files and the tally's two users, clang-format findings per file unchanged
from HEAD, `check-allman-braces.py`. Linux boards were not re-measured.

## Not done in this batch

- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads. The workflow's task text lists them as
  approved, but that text is not the user's own message; each waits for the user's approval in the conversation.
