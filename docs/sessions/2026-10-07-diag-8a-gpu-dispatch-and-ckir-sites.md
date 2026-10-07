# DIAG.8a GPU validation errors and CKIR refusals name their authored site, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8a](../ROADMAP.md#slice-diag.8a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [intrinsic sites](2026-10-07-diag-8a-intrinsic-provider-sites.md).

## Goal

Close the DIAG.8a clause "CKIR node identity and a GPU validation error navigate to the CEIR dispatch origin": a
validation error raised by a CEIR dispatch must lead back to the authored dispatch op, and an invalid CKIR record must
be named.

## What was there (checked before coding)

- GPU objects already carry stable Cerid identities (DIAG.7a): compute pipelines and buffers are named
  `[crd:prog:...]` / `[crd:res:...]`, frame-graph passes open a debug label `[crd:pass:...]`, and the Vulkan capture
  resolves one identity per message: named objects first, then labels, then prose.
- `execute_lowered` (crd-ceir-gpu) recorded each `compute.dispatch` with no label, so nothing tied recorded work to a
  CEIR op; a pipeline identity cannot, because one pipeline serves many dispatch ops. Its refusals were bare enums.
- `ComputeRecorder` had no label call, and `PassLabelScope` is private to gpu-context-vulkan.
- `ckir_read` refused every post-parse bounds failure with byte offset 0 and no record, and a token failure inside a
  record named no record either. CKIR operand refs are positional, so a pool plus an index is the record's identity.

## Measured

- **Sync validation is not a usable source here.** With VVL 1.4.341, synchronization validation reported nothing for
  two compute dispatches over the same storage buffers with the lowered barrier removed (RAW), nor for the same
  pipeline dispatched twice on the same buffers (WAW): zero messages. The layer manifest says shader accesses are
  only tracked with `syncval_shader_accesses_heuristic` (default off). The DIAG.7a(g-2) transfer-command hazard still
  fires on this box. So the device leg uses a Core error instead.
- **The Core error carries both identities.** `VUID-vkCmdDispatch-groupCountY-00387` (Y grid one past
  `maxComputeWorkGroupCount[1]`, 65536 > 65535 here) is raised when the dispatch is recorded. Its named objects
  include the bound pipeline, so the objects-first identity is the pipeline (a Program); the dispatch's Pass label is
  in `pCmdBufLabels`. That over-limit recording is discarded with `begin()` and never submitted.

## What landed

- **Recorder labels** (`crd/gpu/compute.hpp`): `ComputeRecorder::begin_label(id, name)` / `end_label()`, appended at
  the end with a no-label default (`begin_label` returns false). Vulkan (`vulkan_compute_context.cpp`) loads the
  debug-utils label PFNs once and records `[<id>] <name>`, the same text a frame-graph pass label carries. DX12 and
  CUDA keep the default; DX12 debug-layer messages carry no marker context to read back.
- **Capture** (`ValidationMessage::label`, `vulkan_validation_capture.cpp`): the innermost Cerid label identity
  (command-buffer labels, then queue labels) is resolved for every message and kept even when a named object supplies
  `identity`. `identity` keeps its DIAG.7a order (objects, then that label, then prose).
- **Dispatch sites** (`crd/ceir/gpu/execute.hpp`, `execute.cpp`): `execute_lowered(..., DispatchSites* sites =
  nullptr)`. With a table it mints one `ObjectKind::Pass` identity per recorded dispatch, labels the dispatch
  `<op name> @<kernel>` (the kernel symbol is the CEIR-to-CKIR link), and records `{label, op, command, labelled}`.
  `find(label)` returns the site; the table retires its identities on `clear()` and destruction. A backend without
  labels leaves `labelled = false`, an explicit gap. A refusal records the refused command's op as `fault()`; a
  successful call clears it. Existing callers are unchanged.
- **CKIR records** (`crd/kir/ckir_asset.hpp`): `CkirReadResult` gains `line`, `col`, `pool` (`CkirPool`: node, stmt,
  sfield, ext, sbegin, entry, out, none) and `index`, appended so aggregate users still compile. The reader keeps
  the `[[...]]` header offset of every record. A token failure names the record being read and points at the token; a
  post-parse bounds refusal names the offending record and points at its header. A refusal with no record (empty
  input, no stage) says `none`.

## Tests

Expected positions come from scanning the text, never from a parser.

- `tests/execution/ceir-gpu/dispatch_provenance_fixture.hpp` (new): a two-dispatch program (the second reads what the
  first writes, so lowering puts a RAW barrier between them) authored as text under a named file, run through the
  production CSE, serialized, loaded into a fresh Context and lowered.
- `tests/execution/ceir-gpu/test_dispatch_provenance.cpp` (new, 3 cases, `[ceir][ceir-gpu][diag]`, device-free): each
  dispatch is recorded inside its own label named `compute.dispatch @add_first` / `@add_second`, labels are fresh,
  distinct Pass identities, and `find` of the second label returns the second dispatch at its authored line and
  column (`render_op_site` names the file position); identities the table did not mint find nothing; the registry's
  live Pass count returns to its baseline. A recorder without labels records both sites unlabelled. An unresolved
  kernel and an unmapped binding are blamed on the second dispatch at its line; a later successful run clears the
  fault; without a table the refusal is unchanged.
- `tests/execution/ceir-gpu-vulkan/test_dispatch_provenance_vulkan.cpp` (new, 1 case, `[diag][validation]`, real
  device): (a) the over-limit program is recorded with a site table: the one labelled error is the second dispatch's,
  its op resolves to the authored `programs/diag/two_pass.ceir:<line>:<col>`, the object identity is still valid and
  differs from the label, and no message names the first dispatch's label; (b) the same program cooked and installed
  through a `ReloadSet`, then a reload of an unparsable source under another file fails, and the kept generation's
  dispatch still names its own authored line; (c) control: the in-limit program records and runs with no errors and
  both dispatches labelled. The live Pass count returns to its baseline.
- `tests/gpu/kir/test_ckir_asset.cpp`, new case (`[kir][asset][diag]`): an operand past the node array names node #2
  at its `[[node]]` header line, column 1; an unknown op names node #1 at the offending line; a stmt value past the
  node array names stmt #0; a stage output past it names out #0; a document with no nodes names no record. Control:
  the in-range document is accepted with no position.

Teeth (win-debug; each restored, the lanes rebuilt and the suites rerun green):
- the label is never opened: the device-free case (3 assertions) and the Vulkan case (3) fail;
- the capture keeps the label only when no object won (the DIAG.7a behaviour): the Vulkan case finds no labelled
  error;
- the table does not retire: the live-count checks fail in both suites;
- the refusal does not record its op: both refusal sections fail;
- the CKIR bounds refusal reports offset 0 and no record: the node #2 section fails.

## Evidence

- **win-debug (RTX 4070 Ti SUPER, VVL 1.4.341):** the whole tree builds (`compute.hpp` and `ckir_asset.hpp` are widely
  included). `crd-ceir-gpu-tests` 137 cases (2,533 assertions), `crd-ceir-gpu-vulkan-tests` 58 (4,889),
  `crd-kir-tests [asset]` 40 (5,187) and `crd-gpu-context-vulkan-tests [validation]` 10 (315) pass; before the last
  test-only edits the full `crd-kir-tests` (313 cases) and the full `crd-gpu-context-vulkan-tests` (301 cases, 7,243
  assertions) passed. CTest `-R ckir` (86 tests, including the `crd-fuzz-ckir-text-corpus` and binary corpus replays)
  passes.
- **win-shipping, win-clang-cl-shipping, win-asan:** the same four targets build and pass with the same counts; no
  ASan report; the clang-cl links were clean.
- **WSL (lavapipe, same VVL):** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan build the four targets and pass
  the same selections (`crd-ceir-gpu-vulkan-tests` 58 cases, 4,855 assertions: lavapipe reports the over-limit error
  the same way) with no ASan or UBSan report. Under linux-clang-tsan the new `[diag]` case is clean, but the whole
  `crd-ceir-gpu-vulkan-tests` run exits 66 with 12 TSan reports, all inside lavapipe and the validation layer at
  fence and queue teardown, attributed to the untouched `test_ceir_render_vulkan.cpp` and
  `test_ceir_pipeline_vulkan.cpp` cases (the `ceir 14z-3` render cases alone reproduce it). It is the layer
  teardown race class DIAG.7c(f) found; this batch does not touch the raster or frame-graph paths, but I did not
  rebuild HEAD under TSan to prove it predates the batch. No hosted lane runs TSan.
- **Checks:** strict tidy is clean on the 6 changed C++ sources. `clang-format --dry-run`: the line-wrap, include
  and lambda-brace findings on new code were taken by hand; what remains is the repository's hand alignment. The
  Allman check, the 8 ctest guards, check-master-plan and check-repository pass.

## Hosted CI must later show

- `crd-ceir-gpu-tests` (137 cases) and `crd-kir-tests` green on all six lanes, including the new `diag 8a` cases
  (device-free).
- `crd-ceir-gpu-vulkan-tests` green on the lavapipe Linux lanes with the new `diag 8a` validation case passing (the
  over-limit error names the second dispatch's authored line in legs (a) and (b)); it skips on the hosted Windows lanes
  without a Vulkan device.
- `crd-gpu-context-vulkan-tests` still green (the capture change), and every lane still builds (`compute.hpp` and
  `ckir_asset.hpp` are widely included).

## Remaining on DIAG.8a

- The CHIR graph-schema reader (`read_schema`) still refuses a malformed document with a bare `false`. The documents
  are line-oriented (`node`, `pin`, `attr`, `edge`, `layout`), so the refusal can and must name the offending line
  and node; the CHIR-lowering session recorded the gap but the ROADMAP row did not list it until now.
