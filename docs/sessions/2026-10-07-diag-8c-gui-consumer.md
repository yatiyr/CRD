# DIAG.8c GUI consumer, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8c](../ROADMAP.md#slice-diag.8c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [replay preparation](2026-10-07-diag-8c-replay-preparation.md).

## Goal

The last local clause of DIAG.8c: a GUI consumer of the same typed diagnostic command service that the native caller,
`ceridc diag` and the MCP `diag` tool already use ("GUI/CLI/RPC consumers use the same typed services").

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- crd-perf-ui (`engine/ui/perf-ui`) is the ImGui frontend for crd-perf: the profiler panel, drawn by crd-sandbox. It
  had no view of the command service.
- `DiagCommandService::command_count`, `command_at` and `handler_runs` take the service's lock, and `execute` holds it
  for the whole request, including a `program.inspect` run. A GUI that called the service on its frame thread would
  stall the frame for that run and could not press a cancel button while it ran.
- `gpu.resources` may summarize a frame graph only when the service is called from the thread that drives the graph.
- A refusal's document carries `"summary":{}` and `"items":[]`.

## What changed

- **`crd/perf/ui/diag_panel.hpp`, `src/diag_panel.cpp` (crd-perf-ui):** `DiagCommandPanel`, built over a host's
  service.
  - The form is the service's own request: a command from the listing (copied when the panel is built, and on
    `refresh_commands` while idle), a path, `name=value` argument lines, page item and byte bounds. The panel's own
    checks are only its form: no selection (`NoCommand`), a field longer than the panel's buffer (512 and 8,192 bytes,
    larger than the service's bounds so an oversized path or argument still reaches the service's refusal) and an
    argument line without `=` (`BadForm`, naming the line). Everything else goes to the service.
  - A request runs on the panel's own worker thread. `tick` polls with no wait; accessors read panel state only, so a
    frame never takes the service's lock. One request at a time (`Busy`). `cancel` raises the flag the running request
    was given. The destructor cancels and joins.
  - The answer is the service's `DiagResult`, unchanged. The view splits the document's `summary` and `items`
    structurally (strings skipped with their escapes), never re-serializing anything.
  - `submit` takes a new snapshot from the form; `next_page` sends the snapshot's own kept request with the last page's
    next cursor, whatever the form holds now; `page_at` sends any cursor, so an old one shows `stale-cursor`.
  - `draw` is the ImGui window: the host's grant (read-only text), a command combo marking commands the grant does not
    cover, the fields, Run / Next page / Cancel, the typed result fields, the summary and one row per item.
  - crd-perf-ui now declares and links crd-containers.
- **crd-sandbox:** the service is built from start-up flags only, before the window (so a bad flag exits before any
  device exists): `--diag [command]`, `--diag-grant` (default `read`), `--diag-root`, `--diag-path`, `--diag-param
  name=value` (repeatable, checked for `=` with the other flags). It registers the ceridc set: the built-ins,
  `program.provenance`, `program.inspect` and `replay.prepare` (arith, core and func dialects) and `gpu.resources`, to
  which the real GPU context is added once it exists. No frame graph is registered, because the worker thread calls
  the service. The panel comes up with jobs and perf, after the device, so it is destroyed before the device on every
  return path; the frame loop ticks it and draws it beside the profiler. A started command is sent at start and every
  page of its answer is logged; an unknown command refuses to run (exit 1). The sandbox now links crd-perf-gpu-bridge.
- **A bug found while running it:** the first version validated the started command after the scene and its GPU
  programs existed, and its early `return 1` destroyed the device with live shader modules (validation errors, then a
  segmentation fault). The service and the check moved before the window; the context joins `gpu.resources` later.

## Tests

`tests/ui/perf-ui/test_diag_panel.cpp`, 5 cases tagged `[perf-ui][diag][diag-panel]` in `crd-perf-ui-tests`. They
need no device and no ImGui context (`draw` is the only ImGui call). A fake host registers `test.items` (numbered
items, with an argument check) and `test.block` (holds the service until cancelled, with a 5 s backstop, so a broken
cancel fails instead of hanging).

- **Same bytes, page by page:** the listing equals the service's, in order; `test.items` with 70 items and a label
  that is JSON structure once unescaped, 16 to a page: each of the five pages is byte-equal to a native call at the
  same cursor, its items are exactly the item objects the command made, in order, and the handler ran once; a new
  snapshot, then the old cursor through `page_at`, shows the service's `stale-cursor` refusal.
- **The grant and the form:** `NoCommand`, an unknown command and an over-long path are refused by the panel; under a
  `read` grant `capture.start` is marked not granted and answers `unauthorized`; a line without `=` is `BadForm`
  naming line 2; a malformed name, a repeated name, the command's own check, nine arguments, a path to a command that
  takes none and an argument to a command without a check are the service's refusals with reasons; no refusal ran a
  handler.
- **The frame never waits:** while `test.block` holds the service's lock, 100 ticks with listing reads each return
  under 200 ms (they return in microseconds), submit, `next_page` and `page_at` are `Busy`, the listing is not
  re-read; the panel's cancel ends the command (`cancelled`, seen by the handler, before the backstop).
- **Destruction** while a request runs cancels and joins it.
- **The splitter** keeps strings holding braces and an escaped quote inside one item, nests arrays, reads a refusal,
  and rejects truncated, unterminated and trailing-garbage documents.

## Teeth (win-debug)

Each break was applied, the suite rebuilt and the cases run; the source was then restored (rewritten, so its mtime
moved), rebuilt and the whole suite passed again (21 cases, 627 assertions).

| Break | Result |
| --- | --- |
| `next_page` sends cursor 0 | 65 assertions fail (the first page repeats; the page bound stops the loop) |
| `tick` joins without checking that the answer arrived | 6 fail (the frame waits for the backstop) |
| `cancel` does not raise the flag | 3 fail (the backstop ends the command: `failed`) |
| The destructor joins without cancelling | 2 fail |
| `command_count` asks the service (its lock) | 6 fail (the frame waits while a command runs) |
| The splitter ignores strings | 2 cases fail |
| The argument-line check skipped | 3 fail (the request reaches the command) |
| `page_at` ignores its cursor | 4 fail (no stale refusal) |

The first run of the `next_page` tooth looped forever (a cursor that never advances never reaches the end). The test
gained a page bound so the same break fails in under a second; the tooth was rerun after the change.

## Evidence

- All on the final sources (every lane was rebuilt after the last edits, which only rewrapped lines).
- **win-debug:** `crd-perf-ui-tests` 21 cases (627 assertions; the 5 `diag-panel` cases 561), `crd-sandbox`
  builds. The 8 repository guard CTests pass.
- **win-shipping, win-clang-cl-shipping** (reconfigured for the new sources, profiling compiled out): 14 cases (594
  assertions), the 5 `diag-panel` cases (561) among them; `crd-sandbox` builds. The clang-cl links were clean.
- **win-asan** (inside vcvars): 21 cases (627 assertions), no ASan report; `crd-sandbox` builds.
- **crd-sandbox on this machine's GPU** (win-debug, Vulkan, `CRD_ASSETS_DIR` set, smoke runs of 3 to 4 s, all PASS):
  - `--diag gpu.resources`: one context, `vulkan`, the adapter's name, valid, core validation active; heap usage and
    the frame graph `unavailable` with their reasons.
  - `--diag program.provenance --diag-root <assets> --diag-path ceir/inspect_demo.ceir`: 15 op items, the root never
    in the answer.
  - `--diag program.inspect ... --diag-param entry=main --diag-param args=3 --diag-param breaks=16 --diag-param
    watches=13,16 --diag-param steps=into,out`: under the default `read` grant `unauthorized` (the command needs
    execute); under `--diag-grant read,execute` stops at `16:9`, `5:9` (depth 1) and `17:9`, the length `7` with its
    unit, and the result 36, while the frame loop kept presenting.
  - `replay.prepare` (9 inputs, `replay: unavailable`), `memory.allocators`, `jobs.waits` (32 workers);
    `capture.start` `unauthorized` under `read`; an argument to `program.provenance` `bad-argument`.
  - `--backend dx12 --diag gpu.resources`: the same answer from the D3D12 context (`dx12`, the same adapter).
  - `--diag no.such` exits 1 before the window (after the fix above); a bad `--diag-grant` and a `--diag-param`
    without `=` exit 1.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (the hosted lane's `TSAN_OPTIONS`) build
  `crd-perf-ui-tests` and `crd-sandbox` and pass the suite twice each (21 cases, 627 assertions) with no ASan, UBSan or
  TSan report; TSan covers the worker handoff, the cancel flag and the destructor's join.
- **Checks:** strict tidy (LLVM 20.1.8) is clean on `diag_panel.cpp`, `diag_panel.hpp`, `test_diag_panel.cpp` and
  `sandbox/src/main.cpp`. clang-format `--dry-run` reports only the repository's hand alignment, include grouping and
  one-line `case` style; its line wraps were applied by hand. The Allman check, check-master-plan and
  check-repository pass.

## Hosted CI must show

All of DIAG.8c's local clauses are now shown; the row stays Partial only because the validator needs a published run.
Its first hosted run must show `crd-perf-tests` with the diag-command cases (10 with profiling, 9 compiled out),
`crd-ceir-cook-tests` (55 cases), `crd-chir-tests` (30), `crd-ceridc-tests` (10, with the real binary's CLI and MCP
stdio answers), `crd-perf-gpu-bridge-tests` (14 with profiling, 2 compiled out) and `crd-perf-ui-tests` (21 with
profiling, 14 compiled out, the 5 `diag-panel` cases on every lane) green on all six lanes; the `diag gpu.resources`
case in `crd-gpu-context-vulkan-tests` passing on the lavapipe Linux lanes (it skips on hosted Windows); and `ceridc`
and `crd-sandbox` building on every lane.

## Not covered

- The window's buttons are not driven by a test; they call `submit`, `next_page` and `cancel`, which the tests drive.
  The sandbox runs drew the window every frame.
- The sandbox runs are local only (no hosted lane runs the sandbox with a window).
- Not claimed: the synchronous MCP stdio loop still does not process `notifications/cancelled`; the snapshot item cap
  (`kDiagMaxSnapshotItems`) has no test.
