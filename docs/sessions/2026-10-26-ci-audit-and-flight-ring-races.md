# CI read 2026-10-26: two flight-ring test races, and the DIAG.11c/DIAG.12 hosted audit

<!-- doc-role: historical -->
> Dated evidence. Live owners: [DIAG.11b](../ROADMAP.md#slice-diag.11b) (q1),
> [DIAG.11c](../ROADMAP.md#slice-diag.11c), [DIAG.12](../ROADMAP.md#slice-diag.12). Previous:
> [C2](2026-10-26-flight-recorder-c2.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- Complete-tier [40307186916](https://github.com/yatiyr/CRD/actions/runs/40307186916) (workflow_dispatch, head
  `ae7bdc23`): 26 of 26 jobs green. Scheduled 40292959559 green.
- [40274818980](https://github.com/yatiyr/CRD/actions/runs/40274818980) (head `1cb54d07`) failed
  `TickClock: no thread's reads step back while any thread re-anchors` on win-shipping and `fiber keep: frames of
  short jobs reach the pool's shared free list only as threads start and stop` on win-debug. `6b8cee0e` and
  `2463dc52` repaired them ([tick clock](2026-10-26-tick-clock-cross-thread.md),
  [fiber keep](2026-10-26-jobs-fiber-keep-oversubscription.md)); both pass on every lane of 40307186916. Closed.
- The cancelled push runs 40306951942 to 40346278476 have no failed job.
- Push run [40340291698](https://github.com/yatiyr/CRD/actions/runs/40340291698) (head `4e0a0298`, the C2 code)
  failed seven jobs on two ring cases, below, and 40351178841 (head `e4c56fb8`, running) win-clang-cl on the first.

## The two ring-case failures were the tests' races

- `the post window holds exactly post_events and void claims are given back` failed `REQUIRE(r.take_capture(c))` on
  win-debug, win-release, win-clang-cl and win-clang-cl-shipping. Each worker recorded a fixed 20,000 events, and the
  main thread triggered once 1,000 were kept. When the workers finished first, nothing was recorded after the
  trigger, so the post window never filled. Reproduced on win-debug with eight copies pinned to four CPUs: 5 of 200 runs. A probe printed
  `at_trigger 80001 frozen 80001` in each failure (every event before the trigger).
- `the trigger boundary is exact in the trigger's own thread while other threads record` failed
  `CHECK(r.ring_stats().shared_events > 0U)` on linux-gcc-release, linux-gcc-shipping and linux-gcc-asan. The
  workers had exited and given their rings back before the read. `1df6bdd1` already reads the stats while every thread is alive.
- The fix: each worker of the post-window case records until it has seen the trigger, then 2,000 more. The
  accounting check sums the counts the workers report. The causal-order case had the same race (a fixed hop count),
  so its hops go on until the capture is taken.
- The assertions are unchanged. Tooth: `dropped_while_complete` one short fails the post-window case (`9087 ==
  9086`). After the restore and rebuild, 0 of 200 runs failed for each case on the same four-CPU stress, and the 31
  `[flight]` cases and all 312 crd-perf-tests cases passed on win-debug. Their CTests pass on linux-gcc-asan,
  linux-gcc-debug and linux-clang-tsan, and crd-perf-tests passed whole three times on linux-gcc-asan beside eight
  busy loops ([Catch2 batch](2026-10-26-catch2-fault-address.md)).

## Needs CI audit on 40307186916

Read from the 22 job logs of the run.

- **DIAG.11c.** Every clause the row names is shown:
  - crd-perf-tests' seven `doctor:` cases pass on all 20 test lanes;
  - `diag commands: a command declaring a second class needs both grants` passes on every lane;
  - `crash contract: dump paths are UTF-8 both ways, a non-ASCII directory included` passes on the 11 Windows lanes;
  - the `diag-workflow` label counts 6 on each Windows lane (`DIAG.7c(g)` skipped: no Vulkan device) and 4 on each
    Linux lane, with every lane at 100% apart from linux-gcc-asan's registered third-party TP-5 (the mikktspace
    reference oracle; gate "exactly the registered third-party failures").
  - The head contains `32adc973` and `85fee881`.
  - The row's named open qualifications are unchanged. It stays Needs CI because the first unfinished row
    (SIMD-DISPATCH) comes before it.
- **DIAG.12, the published-CI half of the re-read.** A script matched every backtick reference of the DIAG.0 to
  DIAG.12 rows (2,161, of which 243 name a hosted test) against each lane's CTest results. None failed. The skips
  are the ones the rows name: the Vulkan cases on hosted Windows (no Vulkan device) and the three `diag 11a`
  Application window cases (headless runners).
  - Seeds DETECTED: win-asan `specimens-lose-sanitizer`, `asan-report-to-log-file`, `asan-continues-after-report`,
    `redzone-rear-check-removed`, `symbol-identity-half-compared` and `asan-symbolizer-off`;
  - linux-gcc-asan the same, with `lane-loses-sanitizer` and `lsan-suppression-names-cerid-source`;
  - linux-clang-tsan both sanitizer-activation seeds;
  - linux-gcc-debug `linux-crash-record-truncated`.
- **Left for DIAG.12.** `ae7bdc23` predates C2 (`249d00ca` to `3f15be99`). The flight-recorder clauses (the
  restricted-record case, `crd-fuzz-perf-flight-corpus`, the ring cases, `flight.status`) need the final complete-tier
  run after C2, as the user ordered.
- No run was dispatched: 40340291698 is in progress.
