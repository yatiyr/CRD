# DIAG.9a host records under an inspection session, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a host records through the replay commands](2026-10-08-diag-9a-host-record-commands.md).

## Goal

The fifth DIAG.9a batch: record a host-provider run while an inspection session stops, steps and reads values in it,
and show the record is the one an unobserved run makes. DIAG.9a stays Partial.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- `inspect::Session::invoke` installed its own pre hook on the interpreter (and no post hook) and cleared both
  afterwards; the interpreter has one hook slot. `cook::InterpreterRecorder` records through the same slot (pre and
  post hooks), attached by `HostProvider::execute(..., HostObserver)`.
- `HostProvider::run` already took both a session and an observer, but only one public overload passed each, so a
  run under a session could not be recorded: had both been passed, the session would have replaced the recorder's
  hooks and the record would have had no events while looking complete.
- The session's plan form already had an observer (`plan::RunControl`) called before it decides whether to stop.
- `record_host_run` loaded the program into a Context of its own, so a caller could not bind a session to the module
  that runs (the session's interpreter breakpoints are keyed by op).
- No production consumer inspects a host run: ceridc inspect, `program.inspect` and the sandbox panel drive the
  compiled plan through `InspectHost`.

## What changed

- **crd-ceir**: `exec::Interpreter` exposes its installed hooks (`pre_hook`, `post_hook`, `hook_user`).
  `inspect::StepObserver` (pre and post hooks and their user) and `Session::invoke(..., HostLink, StepObserver)`: the
  session's pre hook calls the observer's at every safe point before it decides whether to stop (so it sees the op a
  stop holds), and the session installs a post hook only when the observer has one, forwarding to it. Every hook is
  removed after the run. The old overloads pass an empty observer.
- **crd-ceir-host**: `HostProvider::execute(ctx, m, entry, args, session, observer)`; under a session the provider
  reads the hooks the observer's `attach` installed and hands them to the session. `host::HostProgram` loads a cooked
  program into its own Context (dialects, stable ids, a copy of the artifact and its path; `BadSchedule` past a
  record's bounds, `NotLoaded` when it does not read). `record_host_run(program, entry, args, schedule, max_events,
  session, out, missing, fault)` runs first, then builds the record; a run that ended `Cancelled` is refused
  `HostReplayStatus::Cancelled` ("cancelled") with `out` and `missing` untouched. The blob form of `record_host_run`
  is now a wrapper over it. The host executor maps `cancelled` to `failed` (the commands never run under a session).
- Comments touched in these files lost their roadmap tags.

## Tests

`tests/execution/ceir-host/test_host_session_record.cpp` (3 cases, 220 assertions, `[ceir][host][diag]`). The program
is built, printed and cooked under `programs/diag/host_session_record.ceir`; lines come from scanning the text. It
pools a launch whose body calls `@square(5)`, calls `@square(%x)` on the submitting thread, awaits the launch, folds a
map_reduce (140), keeps a state cell and ends in a loop stepped by the argument. The test thread is the pool's thread
0 and runs the record; a controller thread drives the session, with a backstop that cancels a run still going after
its script.

- **Stopped, stepped, read**: breakpoints on the launch body's call (detached), the submitting call and the state
  cell. For `main(3)` and `main(0)`: a stop at the submitting call (depth 0, its value not yet computed), step into
  `@square`'s multiply (depth 1), step out to the await (the call's value available, 9 or 0), a stop at the state cell
  (the reduction's value 140), then the end. The detached breakpoint was counted once and never paused. The record is
  byte-equal (`encode_record`) to `record_host_run`'s unobserved record of the same blob; no event is from a body run
  on a sub-interpreter; the stepped multiply is an event at depth 1 with its value. Written with
  `write_record_file`, read back and decoded, it replays in a fresh Context with no divergence; `main(0)` fails
  `bad-for-step` at the guard loop's scanned line (also as the record's fault site and the replay's recorded fault);
  `main(3)` returns 174 with the cell 174, and replaying it against the program with the launched constant edited to
  6 diverges (`value`) at the await, recorded 25, observed 36, at its scanned line.
- **Cancelled**: a cancel at the first stop ends the run; `record_host_run` answers `cancelled`, the record keeps its
  preset entry, no program, no events and the plan executor, and the missing list stays empty. The same
  `HostProgram` then records unobserved, byte-equal to the blob form.
- **Not loaded**: a path past the bound (`bad-schedule`) and a cut blob (`not-loaded`) give no module, and recording
  them answers their status with nothing recorded; a loaded program still refuses a zero job split.

## Teeth (win-debug)

Each break was applied, `crd-ceir-host-tests` rebuilt and its `[diag]` cases run (19 cases) from a scratch folder; each
source was restored by rewriting it, the whole win-debug tree rebuilt and every touched suite passed again.

| Break | Result |
| --- | --- |
| the provider hands the session an empty observer (the session replaces the recorder's hooks) | the stopped-run case fails (bytes differ, the stepped frame missing) |
| the session calls the observer's pre hook only where it does not stop | the stopped-run case fails (bytes differ, the held multiply missing) |
| the session installs no post hook (the values are never read) | the stopped-run case fails (bytes differ, the stepped value missing) |
| a cancelled run under a session is recorded | the cancel case fails (status `ok`) |
| `record_host_run` does not pass the session to the run | the stopped-run and cancel cases fail: the first wait for a stop times out |

A first attempt at the last break did not compile; it was redone with the parameter still referenced, so it compiled.

## Evidence

Final sources, every lane built from them. Suites: `crd-ceir-host-tests` 43 cases (1,915 assertions), `crd-ceir-tests`
548 (13,094), `crd-ceir-cook-tests` 66 (3,429), `crd-ceridc-tests` 13 (527, run from a scratch folder under the lane
with `CRD_CERIDC_EXE` set), `crd-sandbox-inspect-tests` 6 (198) and `crd-ceir-gpu-tests` 140 (2,661).

- **win-debug**: the whole tree builds (the crd-ceir headers are widely included); every suite passes with those
  counts. Through ctest, the 8 repository guards pass, and the 50 `diag 9a` and `diag 8b` cases pass with `-j 8`.
- **win-shipping, win-clang-cl-shipping and win-asan**: each lane reconfigured and built the six suites, `ceridc` and
  `crd-sandbox`, and passed every suite with the same counts. The clang-cl links were clean the first time; win-asan
  ran inside vcvars with no ASan report.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane
  built the same targets and passed the six suites with the same counts; no ASan, UBSan or TSan report.
- **Checks**: strict tidy (LLVM 20) is clean on the 9 changed C++ files. clang-format shows only the repository's
  hand alignment, include order and `case` style; its two line wraps in the new test were applied by hand, and no
  added line is over 120 columns. The Allman check, check-master-plan and check-repository pass.

## Hosted CI must show

- `crd-ceir-host-tests` (43 cases, with the 11 `diag 9a` host-record cases) green on all six lanes, with
  `crd-ceir-tests` (548), `crd-ceir-cook-tests` (66), `crd-ceridc-tests` (13), `crd-sandbox-inspect-tests` (6) and
  `crd-ceir-gpu-tests` (140) still green, and `ceridc` and `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- State cells across invokes and a run spanning a reload: they need a host that keeps one interpreter across invokes.
- Random streams, clock and time-step inputs, input events and external I/O completions: a host input seam and the op
  that reads it must exist first.
- Backend-specific numeric replay of GPU dispatches with a declared tolerance or oracle.
- Network and physical effects stubbed only in an explicit test replay.
- Host records from crd-sandbox's panel: the request would have to run on a thread the pool enrolled.
- Not claimed: an interactive consumer that inspects host runs (none exists; the inspection consumers drive the
  compiled plan), and a cancel raised while a host run is not paused (the cancel test cancels at a stop).
