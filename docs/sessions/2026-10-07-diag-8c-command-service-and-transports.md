# DIAG.8c typed diagnostic command service and its transports, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8c](../ROADMAP.md#slice-diag.8c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.8b sandbox consumer](2026-10-07-diag-8b-sandbox-consumer.md).

## Goal

First DIAG.8c batch: one typed, bounded command service that every consumer calls, the built-in foundation commands
(capabilities, capture start/stop, bundle inspection, jobs and waits, allocator summaries), and its binding to the
existing CLI and agent transport (`ceridc` and its MCP stdio server). It covers the acceptance clauses "a native
caller and the existing CLI/agent transport obtain the same bounded result", "unauthorized, stale and oversized
requests fail before expensive work" and "process-local operation without a network or MCP dependency".

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- The foundation evidence already existed as separate library calls with no common request shape: the doctor
  (`run_doctor`: modes, dependencies), the bounded non-executing bundle importer (`import_bundle`), the job wait graph
  and cooperative worker snapshot (`wait_graph_snapshot`, `worker_snapshot`), the profiler's allocator registry
  (`allocator_info`, `allocator_snapshot`) and the CPROF capture writer (`save_capture_to_buffer`, `clear_samples`).
- The existing CLI/agent transport is `ceridc`: JSON verbs over a CLI and an MCP (JSON-RPC 2.0) stdio loop that share
  one implementation. It had no notion of authority: every tool in `tools/list` was callable. The `inspect` verb was
  deliberately CLI-only, waiting for this row.
- The engine's other command registry (hesap's CLI registry) sits above foundation in numerics and is register-only;
  a foundation service cannot depend on it.
- A header for the service (`crd/perf/diag_commands.hpp`) had been drafted, untracked, by an interrupted earlier
  attempt at this step, with no source or test. It was treated as a draft: reviewed, corrected (item clipping that
  could cut JSON mid-token, a dangling root view, roadmap tags in comments) and kept.

## What changed

- **`crd/perf/diag_commands.hpp` / `diag_commands.cpp` (`DiagCommandService`, crd-perf):**
  - The host constructs the service with its grant (a set of authority classes) and a config (file root, input and
    output byte limits, the worker poll bound). The grant never travels in a request. Classes are distinct bits:
    `read`, `record`, `inject`, `remote-enable`, `upload`, `process-memory`; no class implies another.
  - `execute(request, cancel)` refuses in a fixed order, before any work: schema version, unknown command, authority,
    request bounds (`Oversized`: page items, page bytes, path length), arguments (`BadArgument`: a page bound below the
    minimum, a path on a command that takes none, a missing path, a path that is not relative plain names;
    `Unavailable` when the host granted no file root), stale cursor, cancellation. A refusal leaves the retained
    snapshot alone.
  - Pagination: a cursor-0 request runs the handler once and retains its snapshot under a new generation; the cursor
    is `generation * 2^20 + offset` (exact as a JSON number). Every page is cut from the retained snapshot, so the same
    cursor and bounds give the same bytes; any new snapshot makes every older cursor `StaleCursor`. A page is bounded
    by item count and serialized bytes; an item stops taking fields at 1000 bytes and a string value is clipped at 160
    bytes (not inside a UTF-8 sequence), and a clipped object says `"clipped":true` while staying valid JSON.
  - Every response is one JSON document with a fixed key order (`schema`, `command`, `ok`, `status`, `reason`,
    `grant`, `generation`, `cursor`, `next_cursor`, `total`, `page_items`, `dropped`, `complete`, `byte_bounded`,
    `summary`, `items`). Locale-independent number formatting (the existing perf JSON helpers).
  - Built-in commands: `diag.commands` (every command with owner, authority, whether granted, path argument),
    `diag.capabilities` (the doctor's modes and dependencies, every authority class and whether granted, profiling
    compiled/active, job pool, file root), `jobs.waits` (parked fibers with their wait edges, each worker's
    responsiveness, truncation-honest), `memory.allocators` (each allocator registered with the profiler and its live
    statistics), `bundle.inspect` (a bundle under the root through `import_bundle`; the file's size is checked against
    the host's limit before a byte is read; the root never appears in the answer), `capture.start` and
    `capture.stop` (`record`: a capture window; stop writes a CPROF file under the root, created exclusively so it
    never overwrites, and reports contended threads instead of hiding them). Evidence that cannot exist answers
    `Unavailable` with the reason (profiling compiled out, profiler not initialised, no job pool).
  - Upper modules add commands with `register_command(spec, handler, context)` (name `[a-z0-9._-]`, exactly one
    authority class, bounded table); they get the same refusal order.
- **`ceridc diag` and the MCP `diag` tool:** `verb_diag(service, request)` returns the service's document unchanged.
  The CLI (`ceridc diag --command --path --cursor --page-items --page-bytes --schema --grant --root`) builds a service
  from its start-up flags (grant default `read`); `ceridc mcp --diag-grant --diag-root` binds one service for the
  life of the server. `mcp_handle(request, alloc, service)` lists the `diag` tool only when a service is bound; its
  arguments are the request fields only, so a smuggled `grant` is ignored. Negative, fractional or non-numeric
  counts are JSON-RPC invalid-params faults; a count above `u32` reaches the service and is refused `oversized`.
  The `ceridc` module declares `perf`.

## Tests

`tests/foundation/perf/test_diag_commands.cpp` (9 cases on profiling builds, `[perf][diag][commands]`). The executable
links no ceridc, MCP or network code, so it is the process-local proof:

- **Refusal order:** 22 requests, one per refusal class and one per ordering pair (schema before unknown, unknown before
  authority, authority before bounds, bounds before arguments, arguments before the cursor, the cursor before the
  cancel flag); each answers its status with `ok:false`, generation 0 and no items, and neither the handler count nor
  the file byte count moves. A cancelled request is refused before its handler. A host without a root answers path
  commands `Unavailable` without opening anything. After all refusals the retained snapshot still serves its next page
  from the same handler run.
- **Pagination:** 100 items in pages of 7 concatenate to exactly the expected items from one handler run; the same
  cursor twice gives identical bytes; another command's snapshot makes every old cursor stale, and so does a new
  snapshot of the same command.
- **Bounds:** a 1024-byte page holds five 177-byte items and says `byte_bounded`; an item with a 2000-byte string and
  200 fields stays under 1000 bytes, clips the string at 160 and ends `"clipped":true`.
- **Authority:** an `inject` command under every other class is refused and never runs; `upload`, `remote-enable` and
  `process-memory` are refused under read and record; with the class granted the command runs. Registration refuses
  duplicates, bad names, `none`, multi-bit authorities, null handlers and a full table. The listing reports each
  command's authority and whether it is granted.
- **Capabilities, bundle, capture, allocators, jobs:** the doctor's schema and the authority rows; a real bundle
  through the importer (three sections, the absent one marked) and non-bundle bytes reported rejected; a missing file
  fails reading nothing; a bundle one byte over the limit is `Oversized` with zero bytes read. The capture window is
  refused under read, unavailable before the profiler exists, opens once, writes a valid CPROF file, and refuses to
  overwrite it on the next window. Profiling compiled out (win-clang-cl-shipping), both capture commands and
  `memory.allocators` answer `Unavailable` with the reason. A registered allocator is listed with its counts. A fiber
  parked on a gated child is reported with the child's task id as its wait edge.

`tests/tools/ceridc/test_ceridc_diag.cpp` (3 cases, `[ceridc][diag]`):

- **Parity:** 13 requests (pages walked by cursor, a stale cursor, capabilities, and every refusal class) through three
  identically configured services: native `execute`, `verb_diag` and the in-process MCP tool. The verb's report and
  the MCP tool text (parsed from the JSON-RPC reply) equal the native document byte for byte, `isError` matches, and
  the three handler and file counters agree.
- **Authority and arguments over MCP:** `capture.start` with `grant`, `authority` and `granted` arguments is refused
  `unauthorized` under the read grant and runs nothing; a `1e12` page count is `oversized` at the service; five
  malformed argument sets are protocol faults that reach nothing. `tools/list` lists `diag` only with a bound service
  and its schema has no authority field.
- **Real binary:** `ceridc diag --command bundle.inspect ... --root .` prints the native document byte for byte;
  `capture.start` is refused without `--grant record` and reaches the command with it; a bad grant exits nonzero.
  Over MCP stdio (`ceridc mcp --diag-root .`) the tool text equals the native document and a smuggled `grant` is
  refused, writing no file.

## Teeth (win-debug)

Each break was applied to the source, the target rebuilt and the tests run. Every one failed; then the source was
restored (mtime bumped), both targets rebuilt and both suites passed again (perf 9 cases, 407 assertions; ceridc
`[diag]` 6 cases, 227 assertions):

| Break | Result |
| --- | --- |
| Authority check disabled | 3 of 9 cases fail |
| Bundle size checked only after the read | 1 case fails (bytes read on an oversized file) |
| Cursor accepted from any generation or command | 2 cases fail |
| Schema version not checked | 1 case fails |
| Page byte bound four times too large | 1 case fails |
| A refusal discards the retained snapshot | 2 cases fail |
| MCP tool builds its service from a `grant` argument | 2 of 6 `[diag]` ceridc cases fail |
| Profiling compiled out, `capture.start` answers `ok` (win-shipping) | 1 of 8 cases fails (2 assertions) |

The last tooth ran on win-shipping, where profiling is compiled out; that lane was rebuilt from the restored source
and passed its 8 cases again.

## Evidence

- **win-debug:** the whole tree builds. `crd-perf-tests [commands]` 9 cases (407 assertions); `crd-ceridc-tests`
  8 cases (280 assertions), run from its build directory with `CRD_CERIDC_EXE` set as CTest sets it. Each
  `[ceridc][diag]` case owns its files, since CTest registers every case and may run them in parallel in one
  directory; `ctest -R "diag:" -j 8` passes the three together.
- **win-asan** (inside vcvars): the same counts, no ASan report.
- **win-shipping and win-clang-cl-shipping** (profiling compiled out on both): `crd-perf-tests [commands]` 8 cases
  (389 assertions; the allocator case is compiled only with profiling, and the capture case takes its
  compiled-out branch); `crd-ceridc-tests` 8 cases (280). The clang-cl links were clean on the first attempt.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build the three targets and pass
  `crd-perf-tests [commands]` (9 cases, 405 assertions) and `crd-ceridc-tests` (8 cases, 280), with no ASan, UBSan
  or TSan report.
- **Checks:** strict tidy is clean on the 8 changed C++ files (after replacing escaped literals with raw strings,
  a loop with `std::ranges::all_of`, two local constant names and the authority enum's base type). clang-format
  `--dry-run` reports only the repository's hand alignment and include grouping on new code; the over-long lines and
  case-label indentation it found were fixed by hand. The Allman check, the 8 repository guard CTests,
  check-master-plan and check-repository pass.

## Remaining (local) on DIAG.8c

- **Program provenance command:** crd-ceir registers a command (through `register_command`) that answers an authored
  program's ops with their authored file, line, column and CHIR origin from DIAG.8a's provenance.
- **Resource summaries:** the GPU context registers a summary of live device resources and budgets alongside
  `memory.allocators`.
- **Replay preparation:** a command that states what a replay of a bundle or run needs and which inputs are missing.
  Today's bundles carry no replay section, so until DIAG.9a records inputs it can only answer the precise reason.
- **Inspection authority for agent transports:** `ceridc inspect` becomes an MCP tool under a declared authority
  class (the DIAG.8b hand-off; `test_ceridc_inspect.cpp` asserts its absence today).
- **GUI consumer:** a GUI (perf-ui or the sandbox) calls the same service.
- **Not tested:** the snapshot item cap (`kDiagMaxSnapshotItems`, 2^20 items, counted in `dropped`) has no case;
  exercising it needs a million-item snapshot.
- **Transport cancellation (gap, not claimed):** the service honours a cancel flag before and inside its handlers,
  but the MCP stdio loop is synchronous and does not process `notifications/cancelled`.

## Hosted CI must later show

This batch does not move the row to Needs CI (local clauses remain). When the row is complete, its hosted run must
show `crd-perf-tests` green on all six lanes with the `[commands]` cases passing (9 where profiling is compiled in, 8
where it is compiled out), `crd-ceridc-tests` green with its three `[ceridc][diag]` cases (they run the real `ceridc`
binary), `ceridc` building on every lane, and the 8 repository guard CTests passing.
