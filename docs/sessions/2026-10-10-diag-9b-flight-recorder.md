# DIAG.9b triggerable flight recorder, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a host records at crd-sandbox](2026-10-09-diag-9a-sandbox-host-records.md).

## Goal

The first half of DIAG.9b: bounded pre/post-trigger recording for assertions, allocation failures, frame hitches,
queue starvation and resource pressure, with loss metadata, crash truncation and a replay recipe in every capture.
Fault injection, the shipping capability checks and host composition are the rest of the row (see the end). This
batch is committed locally and not pushed.

## What was there (checked before coding)

- The DIAG.2b `DiagnosticRecorder` (`crd/perf/diagnostic_recorder.hpp`) is a ring of `DiagnosticEvent`, whose message
  is an owned `cont::String`: copying an event into the ring can allocate. It has no trigger, no post window and no
  production consumer. Its emergency record is the separate fixed-size fatal channel.
- Existing failure seams, none of which reported to a recorder: `crd::set_assert_handler` (single slot; crd-log
  installs one, `get_assert_handler` exists), `set_fatal_invariant_handler` (a plain global, no getter, and
  `check_invariant`'s default path inline), the jobs watchdog's `set_starvation_handler` / `set_hang_handler`
  (`void*` user), `IAllocator::try_allocate` (the non-fatal path; `allocate` is fatal on out-of-memory for TLSF,
  malloc and virtual-memory allocators, non-fatal for bump/stack/pool) and `StreamingAllocator`'s injected
  `IResidencyPolicy::evict`, called (without its lock) whenever a category is over budget.
- `CRD_FATAL` goes through `report_assert_failure`, so it reaches the assert handler. `check_invariant`'s default
  path is `CRD_FATAL`: one failed invariant reaches both handlers.
- The bundle format (DIAG.5d) reserves foundation section tags; 6 was free. `bundle.inspect` names sections with a
  switch over `BundleSectionTag`.

## What changed

- **`crd/perf/flight_recorder.hpp` / `flight_recorder.cpp`** (crd-perf): `FlightRecorder`, `FlightPolicy`,
  `FlightCapture`, the FlightRecord encoder/decoder, `write_flight_bundle` and `flight_capture_summary_json`.
  - Fixed 152-byte `FlightEvent`s (code, severity, two scalars, sequence, time, a message clipped to 111 bytes and
    flagged when clipped) in a ring preallocated at init. `record()` copies into a slot under the ring mutex.
  - `trigger()` takes only its own mutex. Armed → Triggered → Complete → collected (re-armed). A trigger while a
    capture is pending is suppressed and counted with its kind. A capture that ended Fatal is terminal.
  - The post window is anchored under the ring mutex at the first event recorded after the trigger became visible
    (see the race below), and closes at `post_events`, at `post_window_ns` (via `poll()`), at shutdown or on the
    fatal path. Events between closing and collection are counted, not recorded.
  - `poll()` delivers to the host's sink; `take_capture()` without one; `finish(Shutdown|Fatal)` closes early;
    `shutdown()` finishes first. The fatal path takes the ring with a bounded try-lock (2000 attempts with yields);
    when it stays held, the capture carries the trigger alone and says `ring_unavailable`.
  - Policy validation: capacity 2..65536, pre and post at least 1, `pre + post <= capacity`, post window at most
    60 s, a non-zero hitch threshold, a non-empty mask of known triggers, version 1. A rejected candidate, a reload
    while a capture is pending, or a generation that would wrap changes nothing.
  - The record payload ('CFR1', version 1, little-endian) is decoded with every length bounded by its fixed limit and
    the payload, the loss metadata checked against the event list, event order and side of the trigger checked, and
    trailing bytes refused.
- **`crd/perf/flight_triggers.hpp` / `flight_triggers.cpp`**: the trigger sources. Assertions and fatal invariants
  chain to the handler that was installed before (one owner at a time; uninstall restores it). The assert source
  closes the window as Fatal first when the assert path will terminate (headless, no platform handler). The jobs
  sources are handler functions; `FlightFrameWatch`, `FlightAllocatorWatch` and `FlightPressurePolicy` decorate the
  host's frame boundaries, an `IAllocator` and an `IResidencyPolicy`.
- **`diagnostics.hpp` / `.cpp`**: `get_fatal_invariant_handler()` and `default_fatal_invariant()` (the default path,
  now shared by `check_invariant`); the handler slot is atomic.
- **`bundle.hpp`**: `BundleSectionTag::FlightRecord = 6`. `bundle.inspect` names it `flight-record` (it would have
  said `reserved`; clang-cl's `-Wswitch` found the missing case). The writer's and reader's status enums are now
  `u8` and the two pinned on-disk names carry reasoned NOLINTs, so the touched header is tidy-clean.
- **Specimen** `crd-diag-flight-fatal-specimen` (tests/support/diag): records 50 events, installs both fatal-path
  triggers with a sink that writes the bundle with `write_bundle_atomic`, then fails a `CRD_DIAG_INVARIANT` (mode
  `invariant`) or reports an assertion headless (mode `assert`).

## Tests

`tests/foundation/perf/test_diag_flight_recorder.cpp`, 14 cases tagged `[diag9b]` in crd-perf-tests:

- Policy: out-of-bounds candidates refused; init, reload and generation; a pending capture holds the policy.
- Windows and loss: 30 events, a trigger, 10 more on a 16/8/4 policy give pre 22..29, post 30..33,
  `PostEventsFull`, 22 outside the pre window, 6 dropped while waiting; the next capture starts clean.
- Suppression and masking: a second and third trigger are suppressed with their kinds; a masked kind is not counted;
  calls before init are no-ops.
- Time bound and shutdown: `poll()` closes a quiet window as `PostWindowElapsed`; shutdown delivers a truncated one;
  without a recipe the capture carries the unavailable reason.
- Round trip: through `write_flight_bundle`, `import_bundle` and `decode_flight_record`, field by field including a
  clipped message; every strict prefix of the payload, a wrong magic, an inflated count and a trailing byte are
  refused; the JSON summary names the trigger and end.
- `bundle.inspect` through a real `DiagCommandService` names the section `flight-record`.
- Assertion: crd's own `report_assert_failure` triggers (expression, message, file:line), the previous handler still
  runs, a second owner is refused, uninstall restores the previous handler.
- Fatal invariant: `CRD_DIAG_INVARIANT` triggers with its own code, closes Fatal, delivers, then chains; afterwards
  the recorder is terminal.
- Fatal path in a child: for both modes the specimen dies (exit 3 from `abort()` on Windows, SIGABRT on Linux; on ASan
  lanes only a non-zero, non-timeout end is asserted) and its bundle holds the trigger, `Fatal`, a truncated post
  window, events 18..49 and the specimen's replay recipe.
- Jobs watchdog: a real Low-lane starvation under a Normal flood (workers recording) and a real executor-starved hang
  each trigger with their scalars (lane 2; `HangKind::ExecutorStarved`).
- Frame hitch: a 46.6 ms interval over a 20 ms threshold triggers; the hitch frame is the last pre-window event.
- Allocation failure and pressure: a `LinearAllocator` that runs out under `FlightAllocatorWatch`; a real
  `StreamingAllocator` category over its 8 KiB budget with `FlightPressurePolicy`.
- Concurrency: 4 recording threads, a triggering thread and a collector (600 rounds); every capture must be
  consistent and hold exactly `post_events` post events.

### A race the concurrency case found

The first version took the trigger's boundary from a `total` it read without the ring mutex. Events recorded between
that read and the state store then counted as post-window, so a capture could hold more than `post_events` events
after the trigger (and, at larger counts, a post window could overwrite the pre window it promised to keep). The
concurrency case failed once in about seven runs. The boundary is now fixed under the ring mutex by the first event
that observes the trigger; a window that closes with no such event anchors at the total when it closes. With the old
anchoring restored the case failed 2 of 15 runs on win-debug; with the fix, 0 of 15 (and see the TSan repeats below).
This tooth is statistical, and is recorded as such.

### Teeth

Each break was built on win-debug and run against `[diag9b]`, then restored (by rewriting the file) and rebuilt:

| Break | Result |
| --- | --- |
| no post-window event bound | 9 of 12 cases failed |
| re-arm after a Fatal capture | 2 failed (the in-process terminal check; the invariant child's bundle became the assert one) |
| assert trigger does not chain | 1 failed (previous handler not called) |
| a trigger replaces a pending one | 4 failed |
| decoder accepts trailing bytes | 1 failed |
| hang report dropped | the hang case failed |
| `bundle.inspect` without the new name | the inspect case failed (`reserved`) |
| post window anchored at the trigger's unlocked read | the concurrency case failed 2 of 15 runs |

## Verification

| Lane | crd-perf-tests | `[diag9b]` | crd-ceridc-tests |
| --- | --- | --- | --- |
| win-debug (whole tree built) | 230 cases, 2,187 assertions | 14, 304 | its 15 ctest cases, serially |
| win-shipping | 97, 1,418 | 14, 304 | 20, 882 |
| win-clang-cl-shipping | 97, 1,418 | 14, 304 | 20, 882 |
| win-asan (inside vcvars) | 230, 2,112 | 14, 302 | 20, 882 |
| linux-gcc-debug (WSL) | 229, 2,111 | 3 more runs, 3 passed | 20, 882 |
| linux-gcc-asan (WSL) | 229, 2,109 | 3 more runs, 3 passed | 20, 882 |
| linux-clang-tsan (WSL) | 229, 2,111 | 20 more runs, 20 passed | 20, 882 |

- The WSL lanes ran one at a time; ceridc from a scratch folder with `CRD_CERIDC_EXE` set. The only sanitizer
  reports on the ASan lane are the DIAG.3f specimen children's intended ones (heap overflow, use after free, use
  after return, leak), which their cases expect. No TSan report.
- `crd-fuzz-perf-bundle` builds on every Windows lane and its corpus replay passes on win-debug.
- Strict tidy (LLVM 20.1.8) is clean on all 10 changed C++ files. clang-format shows only the repository's hand
  alignment, its single-line case labels and include grouping; I applied its wrap of one specimen line. The Allman
  check passes, and the 8 ctest repository guards pass on win-debug.

## Hosted CI must later show

On all six lanes: crd-perf-tests green with the 14 `[diag9b]` cases (the child-process case asserts the exact exit
only off ASan), crd-ceridc-tests still green, `crd-diag-flight-fatal-specimen` building as a crd-perf-tests
dependency, and `crd-fuzz-perf-bundle`'s corpus replay green.

## Remaining for DIAG.9b

- Fault injection, test-only and seeded: allocator failure, delayed or cancelled completion, partial I/O, device-loss
  simulation and reload interruption, through existing seams.
- Injection during load, cook, install, execute, reload and teardown, with the previous working assets surviving a
  recoverable failure.
- Shipping capability checks: injection needs the `Inject` authority and a build that has it; arbitrary external I/O
  replay is refused.
- The flight policy as an authored asset (loaded through the existing config path, last known good kept),
  `flight.*` diag commands, and the doctor reporting the basic flight-recorder mode.
- A real host (crd-sandbox) installing the recorder with its triggers, its frame watch and its sink.
