# DIAG.8b safe-point inspection on the plan and the reference interpreter, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8b](../ROADMAP.md#slice-diag.8b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.8a CHIR graph sites](2026-10-07-diag-8a-chir-graph-schema-sites.md).

## Goal

First DIAG.8b batch: line breakpoints, stepping, safe stop and typed value snapshots on the two existing CEIR
executors, the compiled plan (`plan::run`) and the reference interpreter (`exec::Interpreter`), with stale-generation
refusal and breakpoint rebinding across a hot reload. No new evaluator.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) matches the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision
recorded there stands.

## What was there (checked before coding)

- The interpreter's §112 `pre`/`post` step hooks (CEIR-11a) receive `(const Operation&, void*)`. Nothing used them
  for a debugger. A cancel flag existed (CEIR-6c), but a cancel set while a hook held an op was only observed at the
  next op, so the held op still ran. The interpreter tracked no call depth.
- The compiled plan had only `RunHooks` (CEIR-11c): observation-only, a `u8` op id, pinned by `test_plan.cpp`'s
  pre/post stream. It had no cancel at all (`RunError` had no `Cancelled`) and kept no types for its slots.
- Stable ids are assigned in pre-order (`assign_stable_ids`), so inserting an op shifts every later id: they cannot
  key a breakpoint across an edit. Authored positions come from DIAG.8a provenance.
- crd-ceir names no jobs type (I4) and `crd/platform/threading.hpp` has no wait primitive; `std::mutex` and
  `std::condition_variable` are already used by engine modules (log, jobs, resources).
- No sandbox or headless consumer executes a CEIR plan or interpreter program today: outside the execution modules,
  CEIR is used by frame-cook, scene-render, render-graph and CEIR audio for conversion and GPU recording only.

## What changed

- **Plan debug seam** (`crd/ceir/plan.hpp`): `RunControl{safe_point, user, cancel}`, a separate parameter of
  `plan::run` (null by default: one predicted branch per instr). With a control, every instr is a safe point: the
  cancel flag is checked, the callback runs on the executing thread (it may block), and the flag is checked again, so a
  cancel issued during a pause stops the run with the new `RunError::Cancelled`, blamed on the held instr, before it
  runs. While a control is attached the run keeps the path of active sequences (sequence, instr, frame base, call
  depth).
- **`plan::read_value`**: reads one op result at a safe point, only from the safe point's own call frame. It reports
  `Available` with the bits, `NotYetComputed` (on the path, not run yet, including the held instr), `OutOfScope`
  (another region, function or frame), `OptimizedAway` with the survivor (no instr defines it, and a survivor names it
  as a `CeirOp` origin, e.g. a CSE duplicate) or `NoSuchValue`. The plan now keeps each result's IR type
  (`CompiledPlan::result_types`, parallel to `result_pool`).
- **Interpreter**: `call_depth()`; a cancel set while the `pre` hook held an op fails that op with `Cancelled` before it
  dispatches.
- **`print_type(ctx, type, out)`** (`crd/ceir/print.hpp`): one type's canonical text, units included.
- **`crd/ceir/inspect.hpp` `Session`**:
  - Breakpoints are authored `file:line` positions. `bind(plan, ctx, generation)` and `bind(module, ctx, generation)`
    resolve them through provenance and report each one `Bound` (with the number of sites and the first op), or
    `NoCodeAtLine`, or `UnknownFile`. A CSE survivor carries every merged line.
  - `run(plan, ...)` and `invoke(interpreter, ...)` attach the session on the executing thread.
  - The controller calls `request_pause`, `wait_for_stop` (timed), `snapshot` (timed), `resume` (Continue, StepInto,
    StepOver, StepOut) and `cancel`. A snapshot is a request answered by the paused executing thread from its own state,
    so a request never dereferences executor memory.
  - Every request names a generation and is refused `StaleGeneration` (or `NotBound`) before any work; configuration
    while an execution is attached is `Busy`.
  - `ValueSnapshot` carries the status, bits, `TypeId`, canonical type text, the quantity dimension when the type is a
    quantity, the redaction class from the host's `RedactFn` (a restricted value reports `Redacted` with no bits and
    keeps its type) and the survivor of an optimized-away value.
  - Pause scopes: `Task` (only this execution waits), `WholeHost` (the host's freeze/thaw run around the pause; refused
    `NoHostPause` without them), `NonPausable` (hits are counted, nothing waits). A safe point on the thread declared by
    `connect_controller` is refused `SameThread` instead of blocking the only thread that can answer.
  - Native debugger coexistence: a pause is an ordinary mutex and condition-variable wait. No signal, trap instruction
    or thread-context change is used, and a controller wait on an executor held elsewhere ends in `Timeout`.

## Tests

Each program is authored as text, parsed under a named file, run through CSE by the PassManager, serialized and
loaded into a fresh Context, as in DIAG.8a. Expected lines come from scanning the text. The program runs on a second
thread with its own allocator; the test thread is the controller. A `Worker` cancels an execution a failed check left
stopped, so a failure cannot hang the suite.

- `tests/execution/ceir/test_inspect.cpp` (6 cases, 289 assertions, `[ceir][diag]`):
  - A breakpoint pauses at the authored `addi` line. Snapshots read `%a == 3` as `!i32`, a `qty<...>` constant with its
    length dimension, the held op as not yet computed, the loop body and the callee as out of scope, the CSE duplicate
    as optimized away naming the survivor, and an unknown id and a past-the-end result as no such value. Bind reports
    a terminator line and the `module` line as having no code and another file as unknown. Stale snapshot, resume and
    cancel requests and configuration while paused are refused.
  - Stepping: step into the call stops in `@helper` at depth 1, where the caller's value is out of scope; step out
    returns to the `core.for` with the call's result (36) available; step over stays in the frame and stops in the loop
    body; step over the call never stops in the callee.
  - A pause request on a fuel-bound run (2^40 iterations) stops it, and a cancel at the pause ends it with `Cancelled`
    blamed on the held instr; a cancel while running also stops it; without a debugger the same run still ends at its
    fuel budget.
  - Scopes: a non-pausable session counts two hits in the loop body and completes; a safe point on the controller's
    thread is refused `SameThread`; a whole-host session without hooks is refused, and with hooks freezes once and
    thaws once; a wait with nothing running times out.
  - A redaction policy withholds `%a`'s bits but keeps its type.
  - The reference interpreter pauses at the same authored line with the same snapshots, steps into the callee and is
    cancelled at the held `muli`, which is the op the `Cancelled` result names.
- `tests/execution/ceir-cook/test_reload_inspect.cpp` (1 case, 51 assertions, `[ceir][reload][diag]`): a ReloadSet
  installs generation 1; a session bound with its generation number stops at the `addi` line and the `core.for` line.
  A hot swap installs generation 2 (a changed constant and a blank line above the `addi`, so the `addi` keeps its
  stable id and compiled position but moves down one line, onto generation 1's `core.for` line). Requests for
  generation 2 are refused until the rebind; after it, the old line reports no code, the moved `addi` is found on its
  new line, the run stops there with generation 2's constant (4), and requests for generation 1 are refused.

## Teeth (win-debug)

Each break was applied, both suites rebuilt and run, then the source restored (mtime bumped), rebuilt and rerun green
(`crd-ceir-tests` 289 assertions in 6 cases, `crd-ceir-cook-tests` 51 in 1). The teeth ran before the last edits, which only split
declarations, renamed one local constant and rewrapped comments; no assertion changed.

| Break | Result |
|---|---|
| Breakpoint sites ignored | ceir 4 of 6 cases fail; cook fails |
| Frame boundary not enforced in `read_value` | the caller value is read from the callee: 1 assertion fails |
| Generation not checked | ceir 7 and cook 6 assertions fail |
| Cancel at a pause not honoured (plan callback, plan re-check, interpreter re-check) | 2 cases fail: the held instr/op runs |
| Rebind keeps the previous generation's site table | cook: 3 assertions fail (stops at the wrong breakpoint) |
| Call depth not counted | step over and step out stop in the wrong frame: 3 assertions fail |
| Redaction ignored | 2 assertions fail |
| Held instr reported available (`<=` for `<`) | 3 cases fail |

A first frame-boundary break (`continue` for `break`) skipped the outer frame's entries exactly as the fix does and
proved nothing; it was replaced by removing the check. The first rebind test also proved nothing against a stale table:
an inserted op shifted lines, stable ids and compiled positions by the same amount, so a stale binding stopped at the
right op by coincidence. The test now moves the line without moving the op.

## Evidence

All on the final sources (the last edits were declaration splits and comment rewraps; every lane below was built
after them).

- **win-debug:** the whole tree builds (`plan.hpp`, `exec.hpp` and `print.hpp` are widely included).
  `crd-ceir-tests` 548 cases (13,094 assertions), `crd-ceir-cook-tests` 42 (1,964), `crd-ceir-host-tests` 29 (922),
  `crd-chir-tests` 29 (5,355) and `crd-ceir-gpu-tests` 137 (2,533) pass. The 8 ctest guards pass.
- **win-shipping, win-clang-cl-shipping (clean thin-LTO link), win-asan (inside vcvars, no ASan report):**
  `crd-ceir-tests` 548 cases and `crd-ceir-cook-tests` 42 cases pass.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build and pass `crd-ceir-tests` (548) and
  `crd-ceir-cook-tests` (42) with no sanitizer report. TSan covers the pause handshake between the two threads.
- **Checks:** strict tidy is clean on the 6 changed C++ sources; clang-format `--dry-run` shows only the repository's
  hand alignment and one-line `case` style on new code; the Allman check, check-master-plan and check-repository pass.

Hosted CI must show `crd-ceir-tests` (548 cases) and `crd-ceir-cook-tests` (42 cases) green on all six lanes with
their `diag 8b` cases passing, and every lane still building (the `RunControl` parameter and `RunError::Cancelled`
are additive).

## Remaining on DIAG.8b (local)

- **Pause with pending jobs** through the crd-jobs `HostProvider`: its pooled launches and parallel ranges keep running
  while the submitting thread is paused, and resume or cancel must complete or cancel them. The provider threads its own
  cancel flag into sub-interpreters, which `Session::invoke` would replace, so the two must compose. Bodies that run on
  separate sub-interpreters have no safe points today; that must be stated as a typed gap or given safe points.
- **GPU work is non-pausable at the real seam**: a dispatch already submitted by `execute_lowered` cannot stop, so the
  GPU seam must classify its dispatches as non-pausable instead of leaving it to the session's declared scope.
- **Consumers**: no sandbox or headless consumer executes a plan or interpreter program today, so the acceptance
  clause "inspect/step an authored program in headless and sandbox consumers" needs a consumer that runs one (DIAG.8c's
  typed commands then expose the same session to CLI and agent callers).
