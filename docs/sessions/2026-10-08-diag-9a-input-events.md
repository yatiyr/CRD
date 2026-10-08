# DIAG.9a input events, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a interactive host clock](2026-10-08-diag-9a-interactive-host-clock.md).

## Goal

Record the input events the contract lists ("input events") at the host input seam, so a failure that depends on
what the user did reproduces in a later process from the record alone, after the program file was edited. The earlier
batches had left them open because no op read an event and none was specified. DIAG.9a stays Partial.

## What was there (checked before coding)

- The seam (`crd/ceir/input.hpp`) carries one raw i64 per read with a kind and a channel; `InputRecorder` and
  `InputFeed` are kind-agnostic, the record stores the kind byte and refuses one past `kLastInputKind`.
- The engine already has a real event model, `platform::InputEvent` (key, button, pointer move, scroll, resize, with
  modifiers and float payloads), so the op's vocabulary follows its `Type` order rather than inventing one.
- CEIR values are integers, so an event has to arrive as integers; a host adapter quantizes the float payloads.
- `replay_needs.cpp` already lists `UIRead` under the `host-state` input; nothing could record that input, so it was
  always missing when needed. Scene ops declare `SceneRead` and `HostStateRead` and do not read through the seam.
- `UIWrite` is a write of the UI class in `hazard.hpp`, so an op declaring it is never reordered with another.
- No op declares `FileIO`, `NetworkIO`, `DeviceIO`, `ExternalCall`, `AgentAction` or a physics write, and only tests
  call `migrate_state`: external completions, stubbed network or physical effects, and cells across invokes still have
  no op or host to record (see Remaining).

## What changed

- **Op** (`input.ceirop.toml`, opgen): `input.event {queue}` takes the next event of a host queue. The seam delivers
  one raw packed i64 (`InputKind::Event`, appended; the channel is the queue) and the op unpacks it into five results:
  bits 0..7 type (0 none, 1 key_down, 2 key_up, 3 key_repeat, 4 mouse_down, 5 mouse_up, 6 mouse_move, 7 scroll,
  8 resize), 8..23 key or button, 24..31 modifiers (shift 1, ctrl 2, alt 4, super 8), 32..47 x and 48..63 y as signed
  16-bit values (pixels for a pointer or a size, hundredths of a step for a scroll). Effects `UIRead` and `UIWrite`
  (taking an event consumes it), `ExternalNondeterminism`, native provider host. A queue outside [0, 2^32) is
  `UndefinedValue` at eval and `BadConst` at plan compile.
- **Seam**: `input::EventType`, `event_type_name` / `event_type_of`, `pack_event` / `unpack_event`, `event_attrs`;
  `reads_input` knows the op. `input::HostEvents` holds up to 16 queues and 4,096 events; a read takes its queue's
  next event in push order, an open queue with none left delivers 0 (a `none` event), a queue the host lacks has no
  value; reads never allocate; `rewind` and `clear` start over.
- **Executors**: the reference installs `eval_event`; the plan compiles `Op::Event` (the queue in the immediate) and
  writes the five results through the instr's result slots. The profile's static assertion moved to `Event`. The
  parallel pre-flight and the provider's inline-launch rule key on `reads_input`, so they cover the op unchanged.
- **Records**: schema 4 stands. `is_seam_input` now includes `host-state`, so it is recorded under `random`'s rule (in
  `record_inputs` and in the inspect host's bound) and stays missing when an op declares a host-state family outside
  the seam. Its missing reason became "the host-state reads are not held; replay.record keeps only input.event reads"
  (the `replay.prepare` expectation in `test_replay_diag.cpp` was updated). The trace keeps four results per event,
  so the fifth (y) is not compared in the trace; it is a fixed function of the recorded raw read.
- **`replay.record events=`** on both executors (`HostEventsSpec` on `HostRecordRequest`, `parse_events_argument`,
  `apply_events`): at most 32 comma-separated events on queue 0 (`key_down|key_up|key_repeat|mouse_down|mouse_up:
  <code>[:<mods>]`, `mouse_move|scroll|resize:<x>:<y>`), each field range-checked and none left over; an empty value
  is an open, empty queue; without it the host has no queue. The cap is 32 because the service bounds an argument
  value to 512 bytes, which 64 short events would already exceed. The summary answers `event_queue` and
  `input_events`.
- **Demo program**: `assets/ceir/event_demo.ceir` takes two events from queue 0, fails `selector-out-of-range` when
  the second is a resize, and otherwise returns the first event's code plus the second's x and y plus its argument.

## Tests

Packed events are spelled out in each test from the layout, independently of `pack_event`; lines from scanning text.

- `tests/execution/ceir/test_input_seam.cpp` (5 new cases): the layout against bits computed in the test and the
  type names; `HostEvents` order per queue, independence, `none` on an empty open queue, no value for a missing queue
  or another kind, `rewind`, `clear` and both bounds; both executors returning the same fifteen results and the same
  three reads in program order (with and without a second event); a missing queue failing `InputUnavailable` at its
  op in both; four bad queues refused like a bad constant and a parallel body refused.
- `tests/execution/ceir-cook/test_replay_events.cpp` (new, 7 cases): an unhandled resize recorded with both reads,
  deterministic bytes, reproduced by a fresh service after the file was edited, and against the edit (the second read
  takes queue 1) an `input` divergence at that read's line and column; a passing run's events fed back with no queue
  and a tampered x named as a value divergence at the second read; no queue (the failed read recorded and reproduced)
  and an empty queue (two `none` events); 65,537 reads storing `host-state` missing and the replay refused, and a
  scene read beside an event read storing it missing; 17 malformed `events` values refused with nothing run or
  written, and the parser's eight forms and bounds; the inspect host given a `HostEvents` recording the unobserved
  run's reads; a `NetworkIO` op beside an event read keeping `external-results` missing, and a record forged to need
  them refused before anything runs.
- `tests/execution/ceir-host/test_host_inputs.cpp` (1 new case): the host record equal to the host executor's record
  from the same `events` argument, replayed on the recorded split and on 1 and 16 jobs, the edited queue named at its
  line, and a tampered x named at the second read.
- `tests/tools/ceridc/test_ceridc_diag.cpp` (1 new case, real binary): process 1 records the unhandled resize (equal
  to the native record), the file is edited, process 2 reproduces with no queue, process 3 names the edited read
  (`recorded 0, observed 1`, both `event`), process 4 records the same run on the host executor and process 5
  reproduces it; each answer equals the native call, and so does an MCP stdio process's tool text.

## Teeth (win-debug)

Each break was applied, its suites rebuilt, the `[event]` cases run from a scratch folder under the lane, and the
source restored by rewriting it; after the last tooth the three suites were rebuilt and their `[input]` cases passed
again (19, 26 and 4 cases), and the final sources then passed every lane below.

| Break | Result |
| --- | --- |
| `reads_input` without `input.event` | 3 of 5 seam and 5 of 6 cook `[event]` cases fail |
| `is_seam_input` without host-state | 5 of 6 cook cases fail |
| the reference swaps x and y | the two-executor case fails |
| the plan writes only the first result | the two-executor case fails |
| the plan reads queue 0 whatever the attribute | 2 seam cases fail |
| an empty open queue answers no value | 3 seam and 2 cook cases fail |
| a read takes the next event of any queue | 2 seam cases fail |
| `replay.record` routes no queue | 5 of 7 cook cases fail |
| the host executor ignores the events | the host-provider event case fails |
| the parser accepts a field left over | the malformed-argument case fails |
| an empty `events` argument opens no queue | 2 cook cases fail |
| the packing puts the modifiers at bit 16 | 1 seam and 3 cook cases fail |
| a negative queue is accepted | the bad-queue case fails |
| a replay ignores missing external results | the external-results case fails |
| external results counted as a seam input | did not bite: the uncaptured rule (a `NetworkIO` op is outside the seam) still keeps them missing, so the case tests that rule, not this one |

The first run of the "routes no queue" tooth hung: the inspect-host case indexed the plain record's second read
without first requiring it, and the debug array assertion stopped the process. The process was killed, the case now
requires both reads first, and the tooth was rerun cleanly (above). The runner's own decoding error on that crash
stopped the cycle after tooth 7; teeth 7 to 14 were rerun.

## Evidence

Final sources, every lane built from them. All seven lanes passed the same seven suites with the same counts:

| Suite | Cases | Assertions |
| --- | --- | --- |
| `crd-ceir-tests` | 567 | 17,865 |
| `crd-ceir-cook-tests` | 92 | 4,799 |
| `crd-ceir-host-tests` | 47 | 2,078 |
| `crd-ceridc-tests` | 18 | 756 |
| `crd-sandbox-inspect-tests` | 8 | 317 |
| `crd-ceir-gpu-tests` | 140 | 2,661 |
| `crd-chir-tests` | 30 | 5,382 |

(`crd-ceir-tests` gained 4,400 assertions mostly from the bound loop that pushes 4,096 events.)

- **win-debug**: reconfigured; the whole tree builds (`input.hpp` and `plan.hpp` are included across the CEIR
  modules); through ctest the 8 repository guards and the opgen tests pass (10 tests). `crd-ceridc-tests` ran from a
  scratch folder under the lane with `CRD_CERIDC_EXE` set, after `ceridc` itself was rebuilt.
- **win-shipping, win-clang-cl-shipping and win-asan**: each lane reconfigured and built the seven suites, `ceridc`
  and `crd-sandbox`; the clang-cl links were clean the first time; win-asan ran inside vcvars with no ASan report.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`), one lane at a
  time: every lane built the same targets and passed the seven suites with the same counts; no ASan, UBSan or TSan
  report.
- **Generated code**: opgen regenerated the `input` dialect (`--check` clean) and `check-generated.py --refresh`
  recorded the four tracked files' new bytes in `scripts/generated-sources.json` (`check-generated` passes).
- **Checks**: strict tidy (LLVM 20) is clean on all 20 changed C++ files. clang-format shows only the repository's
  hand alignment and its lambda and `case` styles on the added lines; its other wraps were applied by hand and no
  added line is over 120 columns. The Allman check, check-master-plan, check-repository and test-repository-tools
  pass.

## Hosted CI must show

- `crd-ceir-cook-tests` (92, with the 7 `[event]` cases), `crd-ceir-host-tests` (47), `crd-ceridc-tests` (18, with
  the five-process event case) and `crd-ceir-tests` (567, with the 5 new seam cases and the regenerated input smoke
  cases) green on all six lanes, with `crd-sandbox-inspect-tests` (8), `crd-ceir-gpu-tests` (140) and
  `crd-chir-tests` (30) still green; the `crd-ceir-opgen-drift` ctest green; `ceridc` and `crd-sandbox` building on
  every lane.

## Remaining for DIAG.9a

- Can be done now: input events at the interactive hosts (`program.inspect`, `ceridc inspect`, crd-sandbox's panel fed
  from the window's `platform::InputEvent` stream at a run's start); backend-specific numeric replay of GPU dispatches
  with a declared tolerance or oracle (dispatch results must first be read back into the trace); host records from
  crd-sandbox's panel (an enrolled thread).
- Waiting on a consumer or an op: state cells across invokes and a run spanning a reload (no host keeps one
  interpreter across invokes); external I/O completions and network or physical effects stubbed only in explicit test
  replay (no op declares those effects; until one does, a program needing external results keeps them missing and its
  record is refused before anything runs).
