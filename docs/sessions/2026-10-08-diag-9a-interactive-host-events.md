# DIAG.9a input events at the interactive hosts, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a input events](2026-10-08-diag-9a-input-events.md).

## Goal

Give the interactive hosts (`program.inspect`, `ceridc inspect` and the crd-sandbox inspect panel) the input event
queue that `replay.record events=` already takes, and feed the sandbox panel from the window's own input events, so a
program that reads `input.event` can be debugged and its inspected run recorded with every event read. DIAG.9a stays
Partial.

## What was there (checked before coding)

- The inspect host takes any input source and records every read of a recorded run; `host-state` is recorded under
  the seam rule when every read is an `input.event` read (the previous batch's inspect-host event case).
- The three interactive consumers built only `cook::RunInputs` (a seeded source and a clock), so a program that read
  an event failed `input-unavailable` there.
- `crd::app::Application` drains the window's `platform::InputEvent` queue itself (`dispatch_platform_events`) and
  hands typed events (`KeyPressedEvent`, ..., `WindowResizeEvent`) to its layer stack; the sandbox pushed no layer,
  so its frame loop never saw a raw event.
- `input::HostEvents` reads never allocate; `open` and `push` grow on the host's allocator between runs.

## What changed

- **Input bundle** (`crd/ceir/cook/replay_diag.hpp`): `RunInputs` owns an `input::HostEvents` on its own allocator,
  routed for `InputKind::Event`. `set(seeded, seed, clock, events = {})` applies a `HostEventsSpec` through
  `apply_events`, so every call starts the queue over (no spec: no queue); `events()` lets a host queue more between
  runs.
- **`program.inspect events=`**: parsed with `parse_events_argument` in the service's argument step (a malformed
  list is bad-argument with nothing read); the summary answers `event_queue` and `input_events` after the clock
  fields, so three existing summary assertions gained them. The command header now documents the clock and event
  arguments.
- **`ceridc inspect --events <list>`**: refused before the program is read; the report names the same two fields.
- **crd-sandbox**: two exclusive sources for queue 0 (both together exit 1 before anything runs).
  - `--inspect-events <list>` (`PanelInputs::events`, copied by `set_inputs`): every run, "Run again" included, takes
    them from the first.
  - `--inspect-window-events` (`PanelInputs::window_events`): the sandbox pushes `sandbox::InspectEventLayer` on the
    application. It rebuilds each dispatched key, mouse button, pointer, scroll and resize event as the
    `platform::InputEvent` it came from and never marks it handled. `sandbox::window_event` quantizes it the way the
    design settles: the type in the platform's order (each value `static_assert`ed against `input::EventType`), the
    key or button as its platform enum value, the modifiers as shift 1, ctrl 2, alt 4, super 8, and x and y saturated
    to 16 signed bits (the pointer rounded half away from zero to whole pixels, the scroll in hundredths of a step,
    the size in pixels; a NaN is 0). The panel stages events on the frame-loop thread (at most 4,096, the queue's own
    bound; past it the newest replaces the oldest and the drops are counted). Only a successful `start` copies them,
    oldest first, into an open queue, before the run's thread exists, so a run never reads a queue the frame loop
    writes; events that arrive while a run is held feed the next run. The first run starts before the first frame,
    so its queue is open and empty. Each start logs how many window events the run took and how many were dropped.
  - The layer is detached before the panel is destroyed.

## Tests

Positions come from scanning the program text (`assets/ceir/event_demo.ceir` or the test's own app program); packed
events are spelled out in the tests from the layout, independently of `input::pack_event`.

- `tests/execution/ceir-cook/test_replay_events.cpp` (1 new case, 9 `[event]` cases): `RunInputs` with no spec has
  no queue; an open empty spec reads none events on queue 0 only; two events read in order twice over two `set`s
  (a draw between them does not move the queue); a later `set` without a spec forgets the queue; `events()` queues
  more between runs.
- `tests/execution/ceir-cook/test_inspect_diag.cpp` (1 new case, 5 new refusals): an unhandled resize held at the
  switch answers the resize type at the second read's line and fails there; handled events finish on 65 + 5 + 9 + 7;
  an empty list is an open queue (result 7); no list fails `input-unavailable` at the first read; an unknown event,
  a code over 16 bits, a field left over, a position over 16 bits and 33 events are bad-argument with nothing run.
- `tests/tools/ceridc/test_ceridc_inspect.cpp` (1 new case): the verb's held switch, its record (2 reads), handled,
  empty and absent lists, five malformed lists refused before running; the real binary records the inspected run,
  and `replay.record events=` records the same run unobserved: the two records hold the same reads, trace, outcome,
  fault op and input states. The program file is then edited (the second read takes from queue 1); other `ceridc`
  processes reproduce all three records with no queue, and against the edited file the second read is an `input`
  divergence at its line.
- `tests/applications/sandbox-inspect/test_inspect_panel.cpp` (3 new cases, `[event]`): the adapter table (every
  type, the modifier bits, rounding, saturation at both ends, NaN); listed events reach two runs from the first and
  survive the caller changing its list, and a non-window panel stages nothing; a start the host refuses (nothing
  loaded) takes none of the staged events, and a None event is never staged; the layer stages a key and a resize but
  not a close event and handles neither, the run takes both and fails at the switch reading the resize, two events
  arriving while it is held are kept through a `Busy` start and feed "Run again", which finishes on them, and a run
  with nothing staged reads none events; every input event type through the layer reaches a run in order with its
  exact packing; past 4,096 events the newest are kept, the 3 dropped are counted and the run reads them oldest first,
  then a none event. Every recorded run stores `host-state` recorded, and the held failing run replays from its reads
  alone.

## Teeth (win-debug)

Each break was applied by a driver that rebuilt the touched targets (and `ceridc` where the binary runs it), ran the
relevant cases from a scratch folder under the lane and restored the source by rewriting it (mtime bumped). After the
last tooth every touched target was rebuilt and every suite passed again.

| Break | Result |
| --- | --- |
| `RunInputs::set` applies no events | 2 cook `[event]` cases fail |
| `RunInputs` routes no Event | 2 cook `[event]` cases fail |
| `program.inspect` drops the events | the `program.inspect` event case fails |
| `program.inspect` answers no event fields | 3 cook `[diag]` cases fail |
| `program.inspect` accepts any events value | the refusal case fails |
| the ceridc verb drops the events | the ceridc event case fails |
| ceridc's `main` passes no `--events` (binary rebuilt) | the ceridc event case fails |
| the adapter truncates instead of rounding | 2 panel `[event]` cases fail |
| the adapter swaps a pointer's x and y | 2 panel `[event]` cases fail |
| the adapter maps ctrl to the alt bit | 3 panel `[event]` cases fail |
| `start` queues no staged event | 2 panel `[event]` cases fail |
| `start` leaves the staged events for the next run | 2 panel `[event]` cases fail |
| a refused start takes the staged events | 1 panel case fails (no bite at first: `Busy` returns early) |
| past the bound the oldest are kept | the bound case fails |
| the layer marks events handled | 1 panel case fails |
| the layer reads a key repeat as a key down | the layer case fails |
| `set_inputs` keeps the caller's list | the listed-events case fails |

## Evidence

Final sources, every lane built from them. Suites: `crd-ceir-cook-tests` 94 cases (4,884 assertions),
`crd-ceir-host-tests` 47 (2,078), `crd-ceridc-tests` 19 (836; run from a scratch folder under the lane with
`CRD_CERIDC_EXE` set) and `crd-sandbox-inspect-tests` 11 (438); `ceridc` and `crd-sandbox` built on every lane.

- **win-debug**: the whole tree builds; every suite passes with those counts; through ctest the 8 repository guards
  pass. The module declarations gained `app` as a test dependency of scene-render (the owner of the panel tests,
  which now link crd-app for the layer), and `crd-ceridc-tests` links crd-ceir-cook to decode records.
- **win-shipping, win-clang-cl-shipping and win-asan**: each lane was reconfigured, built the four suites, `ceridc`
  and `crd-sandbox`, and passed every suite with the same counts; win-asan ran inside vcvars with no ASan report. On
  win-clang-cl-shipping the first `ceridc` link crashed inside lld-link's LTO codegen (the known flaky toolchain
  crash), so the real-binary ceridc cases first ran against the previous binary and 2 of 19 failed; the link was
  retried once, succeeded, and the suite then passed 19 of 19.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane built
  the same targets and passed the four suites with the same counts; no ASan, UBSan or TSan report. On the TSan lane
  the panel `[event]` cases (3) and the cook `[event]` cases (9) passed three more runs each with no report.
- **crd-sandbox on this machine's GPU** (win-debug, `CRD_ASSETS_DIR` set, run from a scratch folder under the lane
  that was then deleted): `--smoke-test 4 --inspect ceir/event_demo --inspect-arg 7 --inspect-events
  key_down:65:3,resize:1280:720 --inspect-break 9 --inspect-watch 6 --inspect-step continue --inspect-record
  sbx_ev.crpl` took 2 listed events, stopped at `9:9` with the second read's type 8 (a resize), failed
  `selector-out-of-range` and wrote the record (5 events, 2 input reads); `ceridc diag --command replay.run` on it
  answered `reproduced` with 2 recorded and 2 replayed reads. `--inspect-window-events --inspect-record sbx_win.crpl`
  logged that the first run took 0 window events (it starts before the first frame), finished with 7 (two none
  events) and its record (2 reads) answered `reproduced`; no user input reached the window during these unattended
  runs, so a run reading real window events on hardware is not claimed here (the layer and staging are covered by
  the panel tests with application events). `--inspect-events jump:1`, `--inspect-events key_down:65536` and
  `--inspect-events key_down:1 --inspect-window-events` each exited 1 before running. Both smoke runs passed
  (209 and 222 frames).
- **Checks**: strict tidy (LLVM 20) is clean on 13 of the 14 changed C++ files after one fix (an integer division in
  a float context in the panel test). The 14th, `sandbox/src/inspect_panel.hpp`, is reported ungated, as in the
  previous batches: the tool's standalone pass does not resolve its includes (now `crd/app/event.hpp`); its TU and
  the test TU are clean and it builds under `/W4 /WX` on every Windows lane. clang-format shows only the
  repository's hand alignment, its one-level `case` indentation and the operator-first wrap the surrounding sandbox
  code uses; its other wraps were applied by hand, and no added line is over 120 columns. The Allman check,
  check-master-plan and check-repository pass.

## Hosted CI must show

- `crd-ceir-cook-tests` (94, with the 9 `[event]` cases), `crd-ceridc-tests` (19, with the real binary's
  `inspect --events` and its records reproduced by other processes) and `crd-sandbox-inspect-tests` (11, with the 3
  `[event]` panel cases) green on all six lanes, with `crd-ceir-host-tests` (47) still green and `ceridc` and
  `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- Backend-specific numeric replay of GPU dispatches with a declared tolerance or oracle (dispatch results must first
  be read back into the trace).
- Host records from crd-sandbox's panel: the request would have to run on a thread the pool enrolled.
- Waiting on a consumer or an op: state cells across invokes and a run spanning a reload (a host that keeps one
  interpreter across invokes); external I/O completions and network or physical effects stubbed only in explicit
  test replay (an op that declares such an effect).
