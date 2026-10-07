# DIAG.8c program inspection under the Execute authority, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8c](../ROADMAP.md#slice-diag.8c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [GPU resource summaries](2026-10-07-diag-8c-gpu-resource-summaries.md).

## Goal

The fourth DIAG.8c batch closes the DIAG.8b hand-off the row lists as remaining local work: `ceridc inspect` reachable
by the agent transport under a declared authority class, through the same typed, bounded service as every other
diagnostic command rather than as a transport-side exception.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- The service had six authority classes; `read` is defined as "snapshots and inspection of evidence that already
  exists". Running an authored program under a debug session creates new execution, so it fits none of them.
- A `DiagRequest` carried only a command, a path and paging fields: no way to pass an entry, arguments, breakpoints,
  watched lines or steps.
- `ceridc inspect` held its own stop loop in `tools/ceridc/src/verbs.cpp`; the MCP server deliberately did not list it.
- `InspectHost::start` launches the executing thread, which attaches to the session only once it runs. A cancel sent
  before that is refused `NotRunning` and would be lost.

## What changed

- **Authority (`crd/perf/diag_commands.hpp`):** a seventh class, `Execute` (`execute`): running an authored program
  under a debug session and reading its values. No other class implies it; `diag.capabilities` lists it and
  `parse_authority_list` accepts it.
- **Named arguments:** `DiagArg {name, value}` and `DiagRequest::args`. A command takes them only when registered with
  a `DiagArgsCheck` (a new `register_command` overload); `diag.commands` reports `takes_args`. The service bounds the
  count (8), names (32 bytes) and values (512 bytes) in the bounds step (`oversized`); in the arguments step it refuses
  a name outside `[a-z0-9_]`, a repeated name, any argument to a command without a check, and whatever the command's
  check refuses (`bad-argument`), all before the cursor and the cancel flag. The check only parses. As with the path,
  later pages are cut from the retained snapshot.
- **One scripted-inspection engine (`crd/ceir/cook/inspect_script.hpp`, crd-ceir-cook):** `run_inspect_script` adds
  the breakpoints, starts the entry, and at every stop records the authored position and depth and each watched line's
  typed value, then applies the next scripted action, with every wait bounded and at most `max_stops` stops (the next
  cancels and marks the run truncated). Stops are waited for in 10 ms slices that check the caller's cancel flag
  first. A cancel the session refuses `NotRunning` while the execution is still alive is repeated until the session
  takes it or the execution ends. `ceridc inspect` now uses this engine and writes its report in the same shape.
- **`program.inspect` (`crd/ceir/cook/inspect_diag.hpp`):** registered by crd-ceir-cook beside `program.provenance`
  (`execute`, a path, named arguments `entry`, `args`, `breaks`, `watches`, `steps`, `max_stops` up to the host's
  limit of 256). The file's size is bounded before it is read (the shared private `bounded_file` helper, which
  `program.provenance` now also uses); the text is cooked under the request's relative path. Items: one `breakpoint`
  per breakpoint, one `stop` per stop (sequence, reason, file, line, col, depth, op, action) followed by one `value`
  per watched line, one `result` per result; the summary carries the outcome, error and fault position. A load
  failure is `failed` with the cook error and `path:line:col` or the compile error; a run that does not finish within
  the host's `wait_ms` is `failed`; a caller cancel is `cancelled`. The host may observe stops (`on_stop`).
- **ceridc:** `bind_diag_commands` registers `program.inspect`. The MCP `diag` tool takes `args`, an object whose
  members must all be strings (anything else is a protocol fault); `ceridc diag --param name=value` is the CLI form.
  The grant stays the process's start-up flag (`--grant`, `--diag-grant`). `ceridc inspect` remains a CLI-only
  convenience report over the same engine (it reads any path); the agent form is the command.

## Tests

- `tests/foundation/perf/test_diag_commands.cpp`: a new case, "named arguments are bounded and checked in the
  arguments step" (100 assertions): 12 refusals, each with the command's check and handler counters, including the
  order against authority, a stale cursor and a raised cancel flag; the check's reason in the document; a retained
  snapshot still paging after the refusals; a check run on a request without arguments; the listing's `takes_args`.
  The authority and capabilities cases cover `execute`.
- `tests/execution/ceir-cook/test_inspect_diag.cpp` (3 cases, 218 assertions), on the committed
  `assets/ceir/inspect_demo.ceir` with positions from scanning the text:
  - the stepped run (break at the call, step into and out) as 17 items: the breakpoint, three stops at their scanned
    `file:line:col` and depth, four typed values each (available with and without a unit, not yet computed, out of
    scope), and the result 36; the root never appears; pages of three walk the same items and a repeated cursor gives
    the same bytes, with one more run for the walk;
  - refusals that run, read and start nothing: `read` and every class but `execute`; 14 malformed or oversized
    argument sets; an escaping path; a raised cancel. An oversized file is refused unread; a source that does not cook
    is refused at its scanned line; a missing entry is named;
  - the stop bound (truncated, cancelled), a scripted cancel, the caller's cancel raised at a stop (by the host's stop
    observer; the next wait sees it and the run ends at its next safe point) with the next request finishing normally, and the caller's cancel raised while a 10^12-iteration loop
    runs with no breakpoint.
- `tests/tools/ceridc/test_ceridc_diag.cpp` (2 new cases): under `read,execute`, a run, its second page, a refused step
  and arguments to `diag.commands` are byte-equal natively, through the verb and through the in-process MCP tool, with
  one handler run; under `read` all are refused alike; `args` that are not an object of strings are protocol faults.
  The real binary's `diag --param ... --grant read,execute` and `mcp --diag-grant read,execute` answers equal the
  native document; without the grant the CLI is refused `unauthorized`.
- `tests/tools/ceridc/test_ceridc_inspect.cpp` is unchanged apart from its comments and still passes: the fragments it
  checks (stops, typed values, actions, outcomes, refusals) are unchanged on the shared engine. It does not compare
  whole documents, so the report's exact bytes before and after were not compared.

## Teeth (win-debug)

Each break was applied, the target rebuilt and the named cases run; then the source was restored (mtime bumped), the
targets rebuilt and the suites passed again (`crd-perf-tests [diag]` 128 cases, `crd-ceir-cook-tests [diag]` 14,
`crd-ceridc-tests` 10).

| Break | Result |
| --- | --- |
| `program.inspect` declared `read` | cook: 3 of 3 cases |
| Arguments accepted by a command without a check | perf: the arguments case |
| The argument step after the cursor check | perf: the arguments case |
| No cancel poll while waiting for a stop | cook: the bounds case |
| A cancel refused `NotRunning` not repeated | cook: the bounds case |
| The stop bound ignored | cook: the bounds case |
| The program cooked under the joined root path | cook: the stepped-run case |
| The MCP tool dropping `args` | ceridc: the parity case |
| The CLI dropping `--param` | ceridc: the real-binary case |
| ceridc not registering `program.inspect` | ceridc: the parity case |

Two findings from the teeth: a separate cancel check after each stop was redundant (removing it changed nothing,
because the next wait checks the flag first), so it was removed; and with the authority broken the first version of
the while-running case hung, because its helper thread waited for a run that never started. The helper now also stops
waiting once the request is answered, and the authority refusals pass the program's argument so a broken grant fails
quickly.

## Evidence

All on the final sources, except that the last edit renamed one SECTION title in `test_inspect_diag.cpp`; after it
only win-debug was rebuilt and rerun (`crd-ceir-cook-tests` 51 cases, 2,511 assertions).

- **win-debug:** the whole tree builds. `crd-perf-tests [diag]` 128 cases (1,554 assertions; the 10 diag-command cases
  512), the full `crd-ceir-cook-tests` 51 cases (2,511), `crd-ceridc-tests` 10 (413, with `CRD_CERIDC_EXE`). The
  three new program cases passed eight times in a row. CTest runs the new perf, ceir-cook and ceridc cases in parallel
  (43 tests, `-j 8`), and the 8 repository guard CTests pass.
- **win-shipping and win-clang-cl-shipping** (profiling compiled out): reconfigured (new sources), built the four
  targets with clean links on the first attempt; `crd-perf-tests [diag]` 73 cases (1,048), `crd-ceir-cook-tests
  [diag]` 14 (785), `crd-ceridc-tests` 10 (413).
- **win-asan** (inside vcvars): `crd-perf-tests [commands]` 10 cases (512), `crd-ceir-cook-tests [diag]` 14 (785),
  `crd-ceridc-tests` 10 (413), no ASan report. A first attempt that ran the whole `crd-perf-tests [diag]` selection
  under ASan was stopped by the machine's 20-minute test-process watchdog before it printed; it was not a failure
  signal, and the narrower run above replaced it.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (with the hosted lane's `TSAN_OPTIONS`) each pass
  `crd-perf-tests [diag]` 127 cases (1,478), the full `crd-ceir-cook-tests` 51 (2,511) and `crd-ceridc-tests` 10
  (413). The only sanitizer reports are the four the DIAG.3 negative-control specimens raise on purpose under
  linux-gcc-asan (heap overflow, use after free, use after return, a leak); none in Cerid code, and TSan is clean on the
  inspection's controller and executing threads.
- **Checks:** strict tidy is clean on the 13 changed C++ sources (the new headers through their sources; it first
  found two local-constant names, an unforwarded forwarding reference, a loop for `std::ranges::all_of` and a nested
  conditional, all fixed). clang-format `--dry-run` reports only the repository's hand alignment, include grouping,
  one-line `case` and lambda style; its line-wrap findings were applied by hand. The Allman check, check-master-plan
  and check-repository pass.

## Hosted CI must later show

This batch does not move the row to Needs CI (local clauses remain). When the row is complete, its hosted run must
also show `crd-perf-tests` with the diag-command cases (10 with profiling, 9 with it compiled out),
`crd-ceir-cook-tests` (51 cases) and `crd-ceridc-tests` (10, including the real binary's `program.inspect` answers)
green on all six lanes.

## Remaining (local) on DIAG.8c

- Replay preparation: what a replay of a bundle or run needs and which inputs are missing; until DIAG.9a records
  inputs it can only state the precise reason.
- A GUI consumer of the same service.
- Not tested: the snapshot item cap (`kDiagMaxSnapshotItems`). Not claimed: the synchronous MCP stdio loop does not
  process `notifications/cancelled`; a `program.inspect` request holds the service for the length of its run.
