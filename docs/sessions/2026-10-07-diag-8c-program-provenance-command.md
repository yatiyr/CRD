# DIAG.8c program-provenance command, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8c](../ROADMAP.md#slice-diag.8c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [command service and transports](2026-10-07-diag-8c-command-service-and-transports.md).

## Goal

The second DIAG.8c batch adds the program-provenance command the row lists as remaining local work. An authored
program's ops are answered with their authored file, line, column and CHIR origin, through the same typed, bounded
service and the same transports (`ceridc diag` and the MCP `diag` tool) as the foundation commands.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- DIAG.8a's provenance (`resolve_provenance`, `render_provenance`, `native_binding`) answers one op at a time, for a
  program already loaded into a Context. No command or transport exposed it.
- The row said the command would be "registered by crd-ceir". crd-ceir cannot do that: its link edges are fixed to
  the host-only substrate (core, log, memory, containers, units; the I5 check in `scripts/check_ceir_invariants.sh`),
  and crd-perf brings crd-jobs. The precedent is crd-ceir-host, which owns the plan-to-perf calls for the same
  reason. The command therefore lives in **crd-ceir-cook**, the bridge that already loads all three program forms
  (CEIR text, raw CEIR binary, cooked CRDR program). crd-ceir-cook now declares and links crd-perf (privately; its
  public header only forward-declares the service).
- The service's `file_bytes_read` counter belongs to its built-in readers, so the command keeps its own `runs` and
  `bytes_read` counters as the evidence that refusals happen before any work.

## What changed

- **`crd/ceir/cook/program_diag.hpp` / `program_diag.cpp` (crd-ceir-cook):** `register_program_provenance(service,
  command)` registers `program.provenance` (`read`, takes a path). The host owns the `ProgramProvenanceCommand`: its
  dialect registrar, a byte limit (default 16 MiB) and the two counters.
  - The file's size is checked against the limit before a byte is read (`oversized` otherwise).
  - The form is chosen by the leading bytes: a CRDR container is read with `read_program`, the CEIR binary magic with
    `deserialize`, anything else is parsed as text under the request's relative path, so the host's root never
    appears in the answer. The binary forms carry their own authored file names in their origin chunk.
  - Each request loads into a fresh Context with the host's dialects registered first, so an op the host does not
    register reports its native binding as `unregistered` (unknown), never as non-intrinsic.
  - A load failure is refused `failed` with a precise reason: the text's `<path>:<line>:<col>` and the parser's
    message, the binary's byte offset, or the cooked read error.
  - The walk is pre-order over every region, iterative (a deep nest costs heap, not stack), checks the cancel flag
    every 256 ops, and is paginated by the service like every other command.
  - Each op is one `kind:"op"` item: index, stable id, op name, depth, closest-known file/line/col, gap, origin count,
    the first CHIR origin (`chir` id, `chir_file`, `chir_line`, `chir_col`), native kind and provider, then the
    rendered provenance last (so clipping at the 1000-byte item bound never removes a position).
  - **Origin items (found by the CHIR test):** an op merged from several origins has more than the op item can hold,
    and its rendered provenance is clipped at the 160-byte string bound. In the lowered event handler the
    `state_update commit` node is only the second origin of the `core.state` cell, and the first draft lost it to the
    clip. Now, whenever the op item does not state every origin (more than one, or a single one naming another op),
    one `kind:"origin"` item per origin follows it: op index and id, ordinal, space (`ceir-op`, `chir-node`), node id,
    file, line, column.
  - The summary gives the path, form, size, recomputed content hash, the cooked header's recorded hash, and counts:
    ops, positioned, unpositioned, ops with a CHIR origin, intrinsic, unregistered, origin items.
- **ceridc:** `bind_diag_commands(service)` registers ceridc's commands (today `program.provenance` over the dialects
  the inspect verb runs, now shared through `host_dialects.hpp`). The CLI verb's service and the MCP server's service
  both bind through it, so the two transports list and answer the same commands. The answer reflects the host's
  dialect set: a cooked CHIR-lowered program read through ceridc reports its task and async ops as `unregistered`.

## Tests

`tests/execution/ceir-cook/test_program_diag.cpp` (2 cases, `[ceir][cook][diag]`, 199 assertions):

- **Text:** the committed `assets/ceir/inspect_demo.ceir` read under root `assets` and path
  `ceir/inspect_demo.ceir`. The oracle scans the text for the 15 op lines (name, line, the first non-blank column,
  the depth from the printer's indent); every item matches in order, every op has a stable id, nothing is clipped,
  and the repository path never appears. A walk in pages of four returns the same items in the same order.
- **No registrar:** the same 15 ops report `unregistered`, and the summary counts 15.
- **Binary and cooked:** the same program serialized and cooked to files; both answers equal the text answer item for
  item, and the recorded hash, the recomputed hashes and the cook's content hash agree.
- **Refusals:** under a `record`-only grant, without a root, with `../` in the path and with a raised cancel flag the
  command never runs (`runs` and `bytes_read` stay 0). A program over a 64-byte limit is refused `oversized` with
  zero bytes read. A text with a stray `)` is refused `failed` naming `diag8c_program_broken.ceir:<line>:5`, the line
  found by scanning.

`tests/execution/chir/test_chir_reload_provenance.cpp`, new case (`[chir][diag]`): the committed
`assets/chir/event_handler.chir` is lowered and cooked to a file; the lowering Context is gone, so everything comes
through the blob. The answer is `cooked` with no unregistered op; `event_handler on_tick`, `parallel_for update` and
`await task` are each named by an op item with their CHIR id (from the parsed source model) at the scanned position,
`state_update commit` by an origin item; the summary's CHIR count equals the op items naming a CHIR node.

`tests/tools/ceridc/test_ceridc_diag.cpp` (the three existing cases, extended; every service binds ceridc's commands):
the parity script gains a `program.provenance` snapshot, its next page and a missing file, all byte-equal across the
native call, the verb and the in-process MCP tool (5 handler runs on each). The real binary's CLI answers
`bundle.inspect`, `diag.commands` and `program.provenance` byte-equal to a native service, and over MCP stdio a third
request returns the native program answer.

## Teeth (win-debug)

Each break was applied, the targets rebuilt and the cases run. Every one failed; then the source was restored (mtime
bumped), every touched target rebuilt and the full suites passed again.

| Break | Result |
| --- | --- |
| Size bound checked as never exceeded (the file is read) | ceir-cook: 1 case, 3 assertions |
| Text parsed under the root-joined path | ceir-cook: 1 case, 61 assertions |
| Host registrar not called | ceir-cook: 16 assertions; chir: 1 |
| Nested regions not walked | ceir-cook: 6 assertions; chir: 4 |
| Origin items never emitted | chir: 2 assertions (the commit node is gone) |
| `ceridc mcp` does not bind the commands | ceridc: 1 case, 2 assertions |
| `ceridc diag` does not bind the commands | ceridc: the `diag.commands` and `program.provenance` CLI checks |

## Evidence

All on the final sources (the last edits before the lanes were a test-comment rewrap and two tidy fixes in tests):

- **win-debug:** the whole tree builds. `crd-ceir-cook-tests` 48 cases (2,293 assertions), `crd-chir-tests` 30
  (5,382), `crd-ceridc-tests` 8 (317, run with `CRD_CERIDC_EXE` as CTest sets it). The 8 repository guard CTests and
  `ctest -R diag -j 8` (222 tests, the ceridc cases in parallel in one directory) pass.
- **win-shipping, win-clang-cl-shipping, win-asan:** `ceridc` and the three suites build and pass with the same
  counts. The clang-cl links were clean on the first attempt; win-asan ran inside vcvars with no ASan report.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build the four targets and pass the three suites
  with the same counts and no ASan, UBSan or TSan report.
- **Checks:** strict tidy is clean on the 10 changed C++ files (after raw-string literals and moving a test constant
  to namespace scope). clang-format `--dry-run` reports only the repository's hand alignment and one-line `case`
  style on changed lines; the one over-long comment line was rewrapped by hand. The Allman check, check-master-plan
  and check-repository pass.

## Hosted CI must later show

This batch does not move the row to Needs CI (local clauses remain). When the row is complete, its hosted run must
also show `crd-ceir-cook-tests` (48 cases), `crd-chir-tests` (30) and `crd-ceridc-tests` (8, including the
real-binary CLI and MCP stdio `program.provenance` checks) green on all six lanes, and `ceridc` building on every lane
with crd-ceir-cook's new crd-perf link.

## Remaining (local) on DIAG.8c

- GPU resource summaries registered by the GPU context.
- Replay preparation: what a replay of a bundle or run needs and which inputs are missing; until DIAG.9a records
  inputs it can only state the precise reason.
- `ceridc inspect` as an MCP tool under a declared authority class (the DIAG.8b hand-off).
- A GUI consumer of the same service.
- Not tested: the snapshot item cap (`kDiagMaxSnapshotItems`); a program large enough to reach it is not built.
- Not claimed: the synchronous MCP stdio loop does not process `notifications/cancelled`.
