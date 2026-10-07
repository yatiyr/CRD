# DIAG.9a run records of the host provider, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a recording at the inspect host](2026-10-08-diag-9a-inspect-host-recording.md).

## Goal

The third DIAG.9a batch: record and replay a run on the interpreter-based crd-jobs host provider (`HostProvider`),
including the pooled and parallel work it schedules, and store the schedule settings that schedule replay needs.
DIAG.9a stays Partial.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- `HostProvider::run` builds a fresh submitting interpreter (`proto`) for every `execute`, so no state cell survives
  one execution. No production host keeps a reference interpreter across invokes (a search of engine, sandbox and
  tools found none outside tests), so "cells across invokes" and "a run spanning a reload" have no host boundary yet.
- The provider's schedule choices a program can observe: the job split, which results never depend on (bodies are
  state-free by pre-flight, outputs are index-ordered, folds run in index order, the lowest failing index wins), and
  the per-body step budget, which decides whether a body runs out of fuel. `eval_race_pooled` answers its first
  operand; there is no completion-order choice.
- The effect walk marks every launch, await and reduction as needing the `schedule` input (Synchronization), so a
  plan record of such a program stores it `missing` and its replay is refused.
- The interpreter's step hooks fire before every dispatch (pre) and after every successful dispatch (post); they are
  not copied by the prototype constructor, so sub-interpreters never call them.
- The record format had no executor field: its trace and outcome were the compiled plan's.

## What changed

- **Record schema 3** (`crd/ceir/cook/replay_record.hpp`): `ReplayExecutorKind executor` (`plan`, `host`), the host's
  `host_jobs` and `host_sub_fuel`, and `host_error` (`exec::ExecError`). The decoder refuses a host record whose job
  split is 0 or above 256 or whose step budget is 0, a host record with a plan error, and a plan record with either
  host field or a host error (`malformed`); schema 1 and 2 are `unsupported-schema`. `first_divergence` compares the
  executor's own error and no longer assumes plan sites.
- **`InterpreterRecorder`** (same header): `attach` installs pre and post step hooks on an interpreter; the pre hook
  appends an event (op stable id, call depth), the post hook reads up to four results of the op that just ran;
  `detach` removes the hooks and keeps the state cells' current values in stable id order; `finish` takes the run
  error, blamed op and results. Plain values right after the op, where the plan trace reads them at the frame's next
  safe point: the two traces are never compared.
- **`HostObserver`** (`crd/ceir/host/host_provider.hpp`): `HostProvider::execute(ctx, m, entry, args, observer)`
  calls `attach` on the submitting interpreter before the entry runs and `detach` after every pooled launch was
  joined, while the interpreter is alive. `num_jobs()` and `sub_fuel()` expose the settings.
- **`crd/ceir/host/host_replay.hpp`** (new, crd-ceir-host; crd-ceir-host now declares and links crd-ceir-cook, which
  stays jobs-free): `run_host_traced`, `record_host_run` (a cooked blob, path, entry, arguments and `HostSchedule`;
  the inputs from `classify_replay_inputs` with the schedule `recorded`) and `replay_host_record`. The replay refuses
  `wrong-executor`, `other-build` (naming the fields; `any_build` replays anyway), `missing-inputs` (naming them),
  `content-mismatch` and `not-loaded` before anything runs, then runs the record's own blob in a fresh Context with
  the recorded job split and step budget (another program or job split only as an explicit option) and reports the
  first divergence and the fault at their authored positions (`replay_site_in_module`, by stable id in the module).
- **Shared helpers** (crd-ceir-cook): `classify_replay_inputs` (public; `detail::record_inputs` takes the executor),
  `record_missing_inputs` (now also used by `replay.run`), `replay_executor_name`.
- **`replay.run`** refuses a host record `unavailable` ("made by the host executor") before the build check.

## Tests

- `tests/execution/ceir-host/test_host_replay.cpp` (4 cases, `[ceir][host][diag]`). The program is built, printed,
  scanned for its line numbers and cooked from that text under `programs/diag/host_replay.ceir`: a pooled
  `async.launch` whose body calls `@square`, an `async.await`, a `task.map_reduce` whose map body loops 32 times, a
  state cell and a final loop whose step is the argument.
  - `main(1)` on 8 jobs: result 166, the cell 166, the schedule input `recorded` (needed), random, clock, host state,
    external results and device tolerance not needed; the await event reads 25 and the reduction event 140; no event
    names an op of the launch body, the map or the combine, and every event is at depth 0. Records on 1, 2, 3 and 16
    jobs have the same events, result and cell.
  - `main(1)` and `main(0)` encode, decode to the same fields and re-encode to the same bytes; each replays from the
    decoded record in a fresh Context with no divergence on the recorded 8 jobs and, asked explicitly, on 1 job.
    `main(0)` fails `BadForStep` at the guarding loop, found by its stable id at the scanned line in the replay.
  - A step budget of 16 makes the map body run out of fuel (`FuelExhausted`, blamed on an op of the map body, inside
    the reduction's lines); the replay reproduces it only because it runs the recorded budget. The same inputs against
    the program with the launched constant edited from 5 to 6 diverge (`value`) at the await: recorded 25, observed
    36, at the await's scanned line.
  - Refusals with nothing run (no event traced): a plan record, another build (and `any_build` reproducing it), a
    missing input, a content hash that is not the blob's, a cut blob, a job split above 256, and two out-of-range
    schedules for `record_host_run`. The decoder refuses the six mixed-executor records and schema 2.
- `test_replay_record.cpp`: `replay.run` refuses a host record `unavailable`, naming the executor.
- `test_host_record.cpp`: the schema refusal now covers schema 1 and 2.

## Teeth (win-debug)

Each break was applied, `crd-ceir-host-tests` (or `crd-ceir-cook-tests`) rebuilt and the cases run; each source was
restored by rewriting it, both suites rebuilt and the cases passed again (282 and 1,703 assertions).

| Break | Result |
| --- | --- |
| the provider never attaches the observer | 2 cases fail (8 assertions) |
| the post hook keeps no values | 2 cases fail (9) |
| `detach` keeps no cells | 1 case fails |
| the replay runs the default step budget, not the recorded one | 1 case fails (4) |
| the host schedule input left `missing` | all 4 cases fail (24) |
| the host replay accepts a plan record | 1 case fails (3) |
| `replay.run` accepts a host record | the cook refusal case fails (3) |
| the decoder's schedule-field check off | 1 case fails (4) |
| the replay ignores the other program | 1 case fails (8) |
| the provider never detaches the observer | 1 case fails |

## Evidence

Final sources, every lane built from them:

- **win-debug**: the whole tree builds. `crd-ceir-host-tests` 36 cases (1,308 assertions; the 4 new cases 282),
  `crd-ceir-cook-tests` 66 (3,429), `crd-ceridc-tests` 12 (488, run with `CRD_CERIDC_EXE` set as ctest sets it) and
  `crd-sandbox-inspect-tests` 6 (198) pass; the 8 ctest repository guards pass.
- **win-shipping, win-clang-cl-shipping and win-asan**: the four suites, `ceridc` and `crd-sandbox` build and pass
  with the same counts. The clang-cl links were clean the first time; win-asan ran inside vcvars with no ASan report.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane
  builds the four suites, `ceridc` and `crd-sandbox`, and passes them with the same counts; no ASan, UBSan or TSan
  report (the recorder's hooks run only on the submitting thread while pool workers run the bodies).
- **Checks**: strict tidy is clean on the 13 changed C++ files (one loop rewritten as `std::ranges::any_of`).
  clang-format shows only the repository's hand alignment, include order and one-line `case` style; its line wraps
  were applied by hand. The Allman check, check-master-plan and check-repository pass.
- A first run of `crd-ceridc-tests` from the repository root without `CRD_CERIDC_EXE` failed its real-binary cases
  (the variable is unset outside ctest) and left scratch files at the root; they were deleted and the suite rerun
  from a build folder with the variable set.

## Hosted CI must show

- `crd-ceir-host-tests` (36 cases, with the 4 `diag 9a` host-record cases), `crd-ceir-cook-tests` (66),
  `crd-ceridc-tests` (12) and `crd-sandbox-inspect-tests` (6) green on all six lanes.
- `ceridc` and `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- A diagnostic command and ceridc consumer for host records: today `record_host_run` and `replay_host_record` are
  library calls, so a host record reproduced in another process is not shown.
- Recording a host-provider run under an inspection session (the session installs its own step hooks).
- State cells across invokes and a run spanning a reload: they need a host that keeps one interpreter across invokes
  (a multi-invoke record with reload steps and the migrated cells as its host-state input). None exists yet.
- Random streams, clock and time-step inputs, input events and external I/O completions: no CEIR op reads them, so a
  host input seam and the op that consumes it must exist first.
- Backend-specific numeric replay of GPU dispatches with a declared tolerance or oracle.
- Network and physical effects stubbed only in an explicit test replay.
- Not tested: the divergence blame for `length` and `outcome` kinds of a host replay (only `value` and the fault
  position are checked).
