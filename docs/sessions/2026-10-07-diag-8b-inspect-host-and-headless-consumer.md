# DIAG.8b inspect host and the headless consumer, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8b](../ROADMAP.md#slice-diag.8b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.8b GPU seam](2026-10-07-diag-8b-gpu-dispatch-non-pausable.md).

## Goal

Fourth DIAG.8b batch: the acceptance clause "inspect/step an authored program in headless and sandbox consumers",
headless half. A shared host composition that every consumer uses, a committed authored program, and the headless
consumer that runs it under a session. The sandbox half is the next batch.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- No sandbox or headless program executes a CEIR plan or interpreter program. Outside the execution modules CEIR is
  used for conversion and GPU recording only (frame-cook, scene-render, render-graph, audio).
- No committed authored program is runnable by the plan: `assets/ceir/*.ceir` are transform, tensor and audio
  fixtures, and `assets/chir/event_handler.chir` is refused by the plan compiler (`CapturedValue`) by design.
- `ceridc` is the repository's headless agent surface: JSON verbs over a CLI and an MCP stdio loop, with an
  in-process test of the real handler and a smoke of the real binary. Its module declaration did not list CEIR.
- `inspect::Session` never creates a thread; every test drove it with its own worker thread, allocator and
  backstop. A consumer would have had to repeat that, plus the ReloadSet, compile and bind sequence.

## What changed

- **`crd/ceir/cook/inspect_host.hpp` (`InspectHost`, crd-ceir-cook):** `load(id, source, file, entry)` adds the
  authored text to the host's ReloadSet entry (or reloads it through the set's lifecycle), compiles `entry` in the
  generation the set holds and binds the session to that generation number. Outcomes are typed (`HostLoad`: `Busy`,
  `CookFailed` with the cook site, `LoadFailed`, `Rejected` with the reload decision, `CompileFailed`). `start(args)`
  rebinds (resolving breakpoints added since and resetting a finished session) and runs the plan on an executing
  thread the host owns; `wait_finished`, `result`, `op_at_line`, `stop_origin` and `file_path` serve the controller.
  The constructing thread is declared the controller. The session and the executing thread allocate only from the
  host's own allocators. A load or start while an execution is attached is refused `Busy`; the destructor cancels a
  running or paused execution through the session and joins the thread.
- **`inspect::op_at_line(plan, ctx, file, line)`** (`crd/ceir/inspect.hpp`): the first compiled op whose authored
  origins carry `file:line`, resolved the way a line breakpoint is. Consumers name the values they read by line.
- **`assets/ceir/inspect_demo.ceir`:** `@scale(p) = p*p`; `@main(n)` with three constants, a length quantity, two
  adds, a call of `scale` and a loop, returning 36. Bootstrapped by printing its builder, and kept by an anti-drift
  check (`print(parse(asset)) == print(builder)`).
- **`ceridc inspect`** (`verb_inspect`, CLI `--program --entry --arg --break --watch --step --max-stops`): validates
  the whole request first (unknown step action, a zero line, an unreadable program), loads the program under the path
  it was named by, and reports the binds, every stop (sequence, reason, authored line and column, depth, op), each
  watched line's snapshot (status, type text, unit flag, value when available) and the action applied. At most 64
  stops by default; past the bound it cancels and reports `truncated`. Every wait is bounded. `ok` is true when the run
  finished or the script cancelled it; otherwise the report names the refusal, or the run error with its authored
  fault position. It is not offered in the MCP `tools/list`: typed authority for agent transports is DIAG.8c's.
  The `ceridc` module now declares `ceir` and `ceir-cook`.
- **Carried over from the GPU batch:** `execute_lowered` resets `DeviceInspect::refusal` to `None` on entry, as its
  header promises, and `inspect.hpp`'s comment no longer calls `m_exec` executing-thread-only (it is read by
  `request_pause` under the lock).

## Tests

`tests/execution/ceir-cook/test_inspect_host.cpp` (4 cases, `[ceir][inspect][diag]`, plus `[reload]`); expected lines
are scanned from the committed text:

- **Anti-drift:** the committed asset parses and prints as its builder does.
- **Stop, snapshot, step:** `start` before a load is `NotBound`. A breakpoint on the call line binds that op and stops
  there at depth 0. Snapshots: `%a` available (3, `!i32`, no unit), the length available with its unit, the call's own
  result `NotYetComputed`, the loop body's value `OutOfScope`. Step into stops at the callee's `muli`, depth 1, where
  the caller's value is `OutOfScope`; step out stops at the loop, depth 0, with the call's result available (36).
  Continue finishes with 36. During the stop, `load` and `start` are refused `Busy`. The same generation runs again
  and a cancel at its stop ends it `Cancelled`.
- **Reload:** a body edit hot-swaps generation 2; requests naming generation 1 (wait, pause, snapshot, resume, cancel)
  are refused `StaleGeneration` while generation 2 answers from its own frame (the edited constant reads 4) and returns
  100. Renaming the exported entry is a `ContractChange`: `Rejected`, the generation stays and still runs. An
  unparsable source is `CookFailed` at the authored line.
- **Destructor:** destroying the host while its execution is paused returns.

`tests/tools/ceridc/test_ceridc_inspect.cpp` (3 cases, `[ceridc][inspect][diag]`); the expected JSON fragments are
built from the scanned lines:

- **Stepped report:** break on the call, watch four lines, steps `into, out`: three stops with the reasons, lines and
  depths above, the four typed values at each, the actions, `"outcome":"finished","results":[36]` and `ok`.
- **Cancel, bound, no code, refusals, cook failure:** a scripted cancel (`cancelled`, no results, `ok`); a loop-body
  breakpoint with `--max-stops 2` reports two stops, then `truncated` and `cancelled`; a breakpoint on the closing
  brace binds `no-code-at-line` and the run finishes without stops; an unknown action, a zero line, a missing file and
  a missing entry (`compile-failed`, `no-entry`) are rejected before running; a source broken at `arith.muli` reports
  `cook-failed` at that line.
- **Real binary:** `ceridc inspect --program ... --arg 3 --break <call> --watch <a> --step into` exits 0 with the
  breakpoint stop, the step into the callee and the result; MCP `tools/list` does not list the verb.

`tests/execution/ceir-gpu/test_dispatch_inspect.cpp`: a `DeviceInspect` without a session whose out field held `Busy`
reports `None` after recording.

## Teeth (win-debug)

Each break was applied, the target rebuilt and the cases run; the source was then written back (mtime moved),
rebuilt and rerun green.

| Break | Result |
|---|---|
| `load` does not bind the installed generation | reload case: the two generation-1 refusals fail |
| `start` does not rebind | 3 cases fail (no bind reports, no stop after a reload, no stop in the destructor case) |
| the destructor does not cancel | the destructor case never returns (killed by `timeout` after 30 s, exit 124) |
| `ceridc` ignores `into` | 3 cases fail, 12 assertions (stops 2 and 3 missing) |
| `ceridc` ignores the stop bound | the truncation section fails (5 stops reported) |
| `execute_lowered` keeps a stale refusal | the new `DeviceInspect` check fails |

## Evidence

- **win-debug:** the whole tree builds (`inspect.hpp`'s private layout changed). `crd-ceir-cook-tests` 46 cases
  (2,094 assertions), `crd-ceridc-tests` 5 (162), `crd-ceir-tests` 548 (13,094), `crd-ceir-host-tests` 32 (1,026),
  `crd-ceir-gpu-tests` 140 (2,661) and `crd-ceir-gpu-vulkan-tests` 59 (4,930, this machine's GPU) pass.
- **win-shipping, win-clang-cl-shipping (clean thin-LTO links), win-asan (inside vcvars, no ASan report):** `ceridc`,
  `crd-ceridc-tests`, `crd-ceir-cook-tests`, `crd-ceir-tests`, `crd-ceir-host-tests` and `crd-ceir-gpu-tests` build and
  pass with the same counts.
- **WSL:** see the Linux paragraph below.
- **Checks:** strict tidy is clean on the changed C++ files; clang-format `--dry-run` shows only the repository's hand
  alignment, one-line `case` style and include grouping on new code (its line-wrap findings were applied by hand); the
  Allman check, the 8 ctest guards, check-master-plan and check-repository pass.

Linux: linux-gcc-debug first failed to build `test_ceridc_inspect.cpp` (`-Werror=format-nonliteral` on a helper that
passed a pattern to `snprintf`); the helper now substitutes the line itself. On the final sources linux-gcc-debug,
linux-gcc-asan and linux-clang-tsan each build and pass `crd-ceir-cook-tests` (46 cases), `crd-ceir-tests` (548),
`crd-ceir-host-tests` (32), `crd-ceir-gpu-tests` (140), `crd-ceridc-tests` (5, including the real-binary case) and the
`[diag]` cases of `crd-ceir-gpu-vulkan-tests` on lavapipe (2 cases, 128 assertions), with no ASan, UBSan or TSan
report. TSan covers the host's executing thread against the controller and the destructor's cancel and join.

Hosted CI must show `crd-ceir-cook-tests` (46 cases) and `crd-ceridc-tests` (5 cases) green on all six lanes with
their `diag 8b` cases passing (the ceridc real-binary case runs the built `ceridc`), `crd-ceir-gpu-tests` (140 cases,
2,661 assertions) green, and `crd-ceir-tests` and `crd-ceir-host-tests` still green.

## Remaining on DIAG.8b (local)

- **Sandbox consumer:** crd-sandbox runs an authored program through the same `InspectHost`, its frame loop polling
  the session without blocking, loaded through the resolver's app-first program convention (the `ceir` folder is not
  yet a registered program folder in `render-asset-core/identity.cpp`). The showcase-test precedent (compile the
  sandbox TU into a test exe) gives it hosted evidence without a window.
