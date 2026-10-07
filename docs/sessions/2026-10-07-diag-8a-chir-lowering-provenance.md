# DIAG.8a CHIR lowering provenance, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8a](../ROADMAP.md#slice-diag.8a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [cook and reload](2026-10-07-diag-8a-cook-and-reload-provenance.md).

## Goal

The CEIR core and cook batches made op provenance survive passes, the binary, the compiled plan and hot reload. This
batch makes the CHIR to CEIR lowering record where each CEIR op came from, so a diagnostic on a lowered program names
the authored CHIR node and line.

## What was there (checked before coding)

- `ChirNode::loc` already holds a real line:col. `parse_chir(text, file_id, model)` computes it from the text position
  of the node keyword, and the file id is whatever the parse caller passes. That numbering is unrelated to the CEIR
  Context's `register_file` ids, so copying it would alias another file.
- Invalid CHIR text already reports `ChirParseResult::err_line/err_col`; that was not re-implemented.
- `lower_chir` recorded nothing: no op had a CHIR origin, and `OriginSpace::ChirNode` (added by the CEIR core batch) had
  no producer.
- The committed `event_handler.chirgraph` has no positions (every node loc is 0).

## What landed

- **`lower_chir(m, ctx, file = {})`** (`crd/chir/lower.hpp`, `lower.cpp`): every op the lowering creates gets an
  `OriginSpace::ChirNode` origin in the Context's provenance side table, never in `set_loc` (content). The origin is the
  node's CHIR StableId and its line:col.
  - `file` is registered in `ctx` and stamped on every positioned node. Without a file the id is 0 (file unknown) and
    line:col are kept. The model's own file id is never copied.
  - EventHandler: `func.func` and `func.return`. ParallelFor: the three bound constants, `task.parallel_for` and its
    body `core.yield`. Await: `async.launch`, its body constant and yield, and `async.await`. StateDecl: the init
    constant.
  - Many-to-many: the `core.state` cell, and its `next` fallback constant, name both the StateDecl and the StateUpdate
    folded into it.
  - A Query lowers to func parameters and creates no op, so it has nothing to attribute.
  - A graph-authored node has no position; its origin carries the CHIR id only.
- **`render_provenance`** (`context.cpp`): when no origin has a position, the gap text is now followed by
  ` from <origin>, ...` listing those position-less origins, so a graph-authored op renders
  `no-source-location op#N from <no-source-location> chir#M`. Before, the CHIR id was dropped.
- The printed module, `stable_hash` and the text/graph parity are unchanged: origins are not content.

## What the lowered program reports

The committed program's parallel body yields the query view, an outer value (ADR-0128 D2). The plan compiler refuses
it with `CompileError::CapturedValue`, and the reference interpreter fails with `ExecError::UndefinedValue`. Both now
name `assets/chir/event_handler.chir:5:5`, the authored `parallel_for update` line, after CSE, serialization and a load
into a fresh Context. Removing the `= q.entities` binding gives a self-contained program that compiles.

## Test

`tests/execution/chir/test_chir_provenance.cpp` (new, 5 cases, `[chir][diag]`). Expected positions come from scanning
the text for each node's keyword line, never from the parser. The Context registers other files under ids up to the
model's file id first, so a copied id would point at the wrong file.
- **Lowered ops:** every op of the lowered module resolves with no gap to a ChirNode origin. The handler, parallel-for,
  await and yield ops name their node's line, column, id and the authored file. The cell names the StateDecl and the
  StateUpdate, and renders with the file and ` from `.
- **Capture refusal:** after CSE through the PassManager, `serialize`, `deserialize` into a fresh Context (content hash
  equal) and `plan::compile`, the `CapturedValue` offender and the interpreter's `UndefinedValue` op both name the
  `parallel_for` node and line.
- **Self-contained control:** the same pipeline compiles. Every compiled instr names a ChirNode origin in the authored
  file; the `State` instr names both nodes; the `ParallelFor` and `Await` instrs name theirs.
- **Graph projection:** the text and graph lowerings print identically and have equal `stable_hash`. Op for op, the
  graph origins carry the same CHIR ids with no position, and the gap renders the cell's `chir#<world_state id>`.
- **No file:** line:col are kept, the file id is 0 and the render says `<unknown>:`, even though file id 1 exists.

Teeth (win-debug; each restored, touched and the lane rebuilt green, 26 cases):
- The cell attributed to the StateDecl only: the two cell checks fail (2 assertions, 2 cases).
- The file not re-stamped (the model's id 7 kept): 25 assertions fail across 4 cases.
- `render_provenance` not listing position-less origins: the graph case's `chir#` check fails (1 assertion).

## Evidence

- **win-debug:** the whole tree builds (provenance.hpp is included through context.hpp). `crd-chir-tests` passes 26
  cases (5,069 assertions), `crd-ceir-tests` 540 cases (12,649) and `crd-ceir-cook-tests` 40 cases (1,864). CTest
  `diag 8a` runs the 14 cases by name, all passing.
- **win-shipping, win-clang-cl-shipping, win-asan:** `crd-chir-tests` (26 cases) and `crd-ceir-tests [diag]` (6
  cases) build and pass on each; no ASan report; the clang-cl link was clean. `crd-chir-tests` was rebuilt and rerun
  after the last comment rewrap.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build and pass `crd-chir-tests` (26 cases) and
  `crd-ceir-tests [diag]` (6 cases) with no sanitizer report. These ran before two comment rewraps and one initializer
  re-spacing in the test, which change no behaviour.
- **Checks:** strict tidy is clean on the five changed C++ files. `clang-format --dry-run` on the new and changed lines
  reports only the repository's hand-aligned declarations. The Allman check, the 8 ctest guards, check-master-plan and
  check-repository pass.

## Not covered here

- A failed reload of a CHIR-lowered blob was not tested separately. The installed generation keeps the blob's `ORIG`
  origins through a failed reload (proved for text cooks in the previous batch), and ChirNode origins ride the same
  chunk, but no test reloads a CHIR blob.
- The graph schema reader (`read_schema`) still rejects a malformed document with a bare `false`, with no node or
  position. Graph documents have no text positions; naming the offending node is left to the CR-D007 editor work
  (I2D-9) that owns graph diagnostics.

## Remaining on DIAG.8a

- CKIR node identity and a GPU validation error navigate to the CEIR dispatch's origin.
- A host-provider (native intrinsic) runtime error names its op; script/intrinsic entry points and asset generation.
