# DIAG.9a recording at the inspect host, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a run records and replay](2026-10-08-diag-9a-run-records-and-replay.md).

## Goal

The second DIAG.9a batch: capture a run record at a production boundary, the inspect host that `ceridc inspect`,
`program.inspect` and the crd-sandbox panel run authored programs on, and put the ReloadSet asset generation that ran
into the record. DIAG.9a stays Partial.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- `plan::run` takes one `RunControl`. `inspect::Session::run` owned it (its stop logic), and `run_traced` owned
  another (the private recorder), so a run under a debug session could not be traced.
- `InspectHost` runs a ReloadSet generation's compiled entry on its own executing thread; the controller only talks
  to it through the session. A load is refused `Busy` while an execution is attached, so a run never spans a reload.
- A `Generation` holds its Context and module, not the cooked bytes (`cook_source` hands the blob to `add_impl`).
- The record had no field for where its program came from (asset id, generation).

## What changed

- **Session observer** (`crd/ceir/inspect.hpp`, `src/inspect.cpp`): `Session::run(plan, args, alloc, observer)`. The
  observer's `safe_point` runs on the executing thread at every safe point before the session decides whether to stop,
  so it sees the instr a stop holds; a `Cancel` it returns ends the run; its `cancel` flag is not read.
- **`ReplayRecorder`** (`crd/ceir/cook/replay_record.hpp`): the trace `run_traced` already took, now a public class
  (constructed, run and finished on the executing thread) whose `control()` a host passes to the session.
- **`InspectHost` recording** (`crd/ceir/cook/inspect_host.hpp`): `start(args, HostRecording{enabled, max_events})`.
  On the controller, before the executing thread starts, the host re-cooks the installed generation's module in its
  own Context (`cook_program`) into the record's blob, valid only when it cooks to that generation's own content hash,
  and classifies the inputs with the same walk as `replay.record` (`detail::record_inputs`, now shared). The executing
  thread traces into the host's execution allocator. `record(out)` gives the record once the run has ended: `running`
  while it is attached, `cancelled` for a cancelled run (a replay would not stop where it stopped), `not-recorded`
  after a start without recording, `artifact-failed` if the re-cook did not match (not reachable by a test).
- **Record schema 2**: `asset` and `generation` follow the content hash (0 and 0 when no reloading host ran the
  program; a generation without an asset is `malformed`); schema 1 is refused `unsupported-schema`. `replay.run`'s
  summary answers both. `write_record_file` / `RecordWrite` create a record exclusively.
- **ceridc**: `ceridc inspect --record <file>`. An existing file is refused before anything is read or run; the report
  gains a `record` object (`written`, `status`, and for a written record its asset, generation, content hash and event
  count); a cancelled run writes nothing.
- **crd-sandbox**: `--inspect-record <file>`. `InspectPanel::start` passes `HostRecording` to the host; the frame loop
  writes the record when the run ends (logged), refuses an existing file before the run, and "Run again" starts
  without recording.

## Tests

- `tests/execution/ceir-cook/test_host_record.cpp` (4 cases), on the committed `assets/ceir/replay_demo.ceir`:
  - `main(1)` on the host, stopped at the `func.call` and `arith.addi` breakpoints, stepped into the callee and out
    (at least five stops, one at depth 1), records exactly what `replay.record`'s unobserved run of the same file
    records: content hash, input needs and states, every event, the outcome, results and cells. The record names the
    host's asset and current generation, the host's file name and the build; its blob loads in a fresh Context, replays
    without divergence, and resolves the fault to the switch's scanned line and column; `replay.run` on the written
    file answers `reproduced` with the asset and generation; a second `write_record_file` is `exists` and the file is
    unchanged.
  - After a recorded run of generation 1, a hot reload of the edited bias (HotSwap) installs generation 2; the record
    still names generation 1, holds its content hash and failure, and replays; `replay.run` with `program=` the edited
    file names the edited constant (`value`) at its scanned line and column. A start without recording forgets it;
    a recorded run of generation 2 names generation 2, finishes, and keeps 3 of its events.
  - A run held at a breakpoint is `running`; cancelled there it is `cancelled`; schema 1 and a generation without an
    asset are refused by the decoder, and a fixed record decodes with its asset and generation.
  - An observer passed to `Session::run` sees as many safe points as the unobserved run has events, and its `Cancel`
    at the third ends the run there.
- `tests/tools/ceridc/test_ceridc_inspect.cpp`: the `--record` case, in process and through the real binary; each
  record is reproduced by a separate `ceridc diag --command replay.run` process; an existing file is refused before
  the run; a scripted cancel writes nothing.
- `tests/applications/sandbox-inspect/test_inspect_panel.cpp`: the frame loop holds the run (20 idle frames), steps
  into and out, continues through the later hits to the fault; the record is `running` while held, names the panel's
  generation and file, and replays in a fresh Context to the same trace and the switch's line; "Run again" without
  recording forgets it.

## Teeth (win-debug)

Each break was applied, the target rebuilt and the suite run; each was restored (copied back and touched), and the
lanes were rebuilt and rerun green.

| Break | Result |
| --- | --- |
| the session never calls the observer | 3 cases fail (no events; the replay diverges) |
| the observer called only at safe points where nothing stops | the observer-invisibility case fails (events differ, `diverged`) |
| the record's generation read at `record` time | the reload case fails (2 == 1) |
| a cancelled run recorded | the refusal case fails |
| a start without recording keeps the previous record | the reload case fails |
| the generation-without-asset check off | the refusal case fails |
| the observer's `Cancel` ignored | the observer case fails |
| ceridc ignoring `--record` | the ceridc case fails |
| ceridc's existing-file check off | the ceridc case fails (the run started) |
| the sandbox panel dropping the recording | the sandbox case fails |

The first sandbox break did not compile (an unused parameter under `/WX`); it was redone warning-clean.

## Evidence

Final sources, every lane built from them:

- **win-debug**: the whole tree builds. `crd-ceir-cook-tests` 66 cases (3,414 assertions), `crd-ceridc-tests` 12
  (488), `crd-ceir-tests` 548 (13,094), `crd-ceir-host-tests` 32 (1,026), `crd-sandbox-inspect-tests` 6 (198) and
  `crd-ceir-gpu-tests` 140 (2,661) pass; the 8 ctest repository guards pass.
- **win-shipping, win-clang-cl-shipping and win-asan**: the same six suites, `ceridc` and `crd-sandbox` build and
  pass with the same counts. The clang-cl links were clean the first time; win-asan ran with the MSVC ASan runtime on
  the path and printed no ASan report.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane
  builds the six suites, `ceridc` and `crd-sandbox`, and passes them with the same counts; no ASan, UBSan or TSan
  report.
- **crd-sandbox on this machine's GPU** (win-debug): `--smoke-test 4 --inspect ceir/replay_demo --inspect-arg 1
  --inspect-break 15 --inspect-step into,out --inspect-record sbx.crpl` stopped at `15:13`, stepped to `5:9` (depth 1),
  hit `15:13` twice more, failed `selector-out-of-range`, wrote the record (13 events, generation 1) and passed the
  smoke test (194 frames). A second run with the same `--inspect-record` path exited 1 before running. `ceridc diag
  --command replay.run` on the record answered `reproduced`, program `ceir/replay_demo`, generation 1.
- **Checks**: strict tidy is clean on every changed C++ file (after moving the ceridc test's constants to namespace
  scope and removing an unused helper). clang-format shows only the repository's hand alignment, include order,
  one-line `case` style and the sandbox's leading-operator wraps; its other line wraps were applied by hand. The
  Allman check, check-master-plan and check-repository pass.

## Hosted CI must show

- `crd-ceir-cook-tests` (66 cases), `crd-ceridc-tests` (12, with the real binary's `inspect --record` and
  cross-process `replay.run`), `crd-sandbox-inspect-tests` (6) and `crd-ceir-tests` (548) green on all six lanes, with
  the earlier DIAG.9a suites still green (`crd-perf-tests` diag cases, `crd-chir-tests`, `crd-perf-ui-tests`).
- `ceridc` and `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- Capture at the interpreter-based `HostProvider`: state cells carried across invokes, pooled and parallel work, and
  the schedule choices schedule replay needs.
- Random streams, clock and time-step inputs, input events and external I/O completions: no CEIR op reads them, so a
  host input seam and the op that consumes it must exist first.
- A run spanning a reload: only a host that reloads between invokes (the HostProvider) can produce one; on the
  inspect host a load is refused while a run is attached.
- Backend-specific numeric replay of GPU dispatches with a declared tolerance or oracle.
- Network and physical effects stubbed only in an explicit test replay.
- Not tested: `artifact-failed` (a verified module that does not re-cook to its own hash); the 64 MiB record limit.
