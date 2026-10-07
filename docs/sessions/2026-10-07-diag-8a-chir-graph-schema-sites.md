# DIAG.8a malformed CHIR graph documents name their record and node, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8a](../ROADMAP.md#slice-diag.8a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [GPU dispatch and CKIR sites](2026-10-07-diag-8a-gpu-dispatch-and-ckir-sites.md).

## Goal

Close the last local DIAG.8a item: "intentionally invalid text/node input navigates to the responsible source/node".
CHIR text (`parse_chir`), CEIR text and CKIR records already name their line and column. The CR-D007 graph-schema
reader did not.

## What was there (checked before coding)

- `read_schema` returned a bare `bool`. Every refusal (a bad header, a malformed field, an unknown record, an orphaned
  layout row, an edge from an in-pin, two edges into one in-pin) gave the same `false`, with no line, column or node.
- Its tokenizer was whitespace-only with no line notion, so a record with a missing field silently consumed the next
  record's keyword as its last field and failed on the next line.
- The whole-graph checks ran over `SourceModel::edges()`, which `add_edge` re-sorts by consumer pin, and over the
  layout table, which `set_layout` keeps last-write-wins. Neither keeps document order, so neither could name the
  offending record.
- Numeric fields were parsed as unbounded `u64` and narrowed to `u32` without a check, and hex fields had no digit
  limit.

## What changed

- **`SchemaReadResult`** (`crd/chir/schema.hpp`), in the `ChirParseResult` mold: `ok`, the 1-based `err_line` and
  `err_col` of the offending record and token, `node` (the CHIR node the record declares or names, `kInvalidNode` when
  it names none that exists), the node's stable `id`, `orphan` for a layout row whose id resolves to no node (then `id`
  is the unresolved id), and a static `msg`. `read_schema` returns it; there is no `bool` overload, so every caller
  was found by the compiler and now checks `.ok`.
- **Line-oriented reading.** The tokenizer tracks line and column. A record keyword may start on any later line; each
  field must be on the record's own line. A missing field is refused on the record's line, at the column just past its
  last token. A token after the last field is refused and names the record's node. Canonical documents (one record per
  line, which `print_schema` always writes) read exactly as before.
- **Which node is named.** A node record refused past its index names the node it declares (not yet in the model). A
  pin or attr record names its node. An edge from a non-out pin names the source node; an edge into a non-in pin names
  the target. A second edge into an in-pin that is already fed is refused at read time, on the later record, naming the
  consumer. An out-of-range reference, an unknown record and an orphaned layout row name no node.
- **Document order for whole-graph checks.** Edge and layout records keep their line and column in read-order side
  lists (engine `Array`, from the model's allocator), so the orphan and pin-direction checks report the first
  offending record in the document.
- **Field bounds.** Decimal fields must fit `u32`; a stable id has at most 16 hex digits and a coordinate bit pattern
  at most 8. Larger values were previously truncated silently.

The graph a valid document produces is unchanged: `print_schema(read_schema(x)) == x`, the committed asset's anti-drift
ids and the text/graph parity tests all still pass.

## Tests

- `tests/execution/chir/test_chir_provenance.cpp`, new case `diag 8a: a malformed CHIR graph document is refused at
  its record and node` (`[chir][diag]`). Every document is the committed `event_handler.chirgraph` with one authored
  mistake. Expected lines, columns and ids are found by scanning the document text, never from the reader:
  - control: the committed document reads clean with no line, column, node or message;
  - an unknown node kind, a node record cut short, a token after the last field, an out-of-order node index, a parent
    that is not an earlier node, a pin of a missing node, a pin direction that is neither in nor out, an unknown
    record, an edge from an in-pin, a second edge into a fed in-pin (appended after the layout rows, so the earlier
    writer is on another line), an orphaned layout row, a wrong header version and a non-empty model;
  - navigation: the stable id named for the unknown-kind node and for the in-pin edge source is looked up in the
    model `parse_chir` builds from the committed `.chir` text; that node's line equals the scanned line of
    `parallel_for update` and `state_update commit`. Both projections derive the same ids, so a graph refusal leads to
    the authored text line.
- `tests/execution/chir/test_source_model.cpp`: the three existing graceful-reject negatives now assert the record
  line and node (the in-pin edge names its source node 1, the orphaned layout row its id, the double writer the later
  edge's line and the consumer node 3), not only `false`.

Teeth (win-debug; each restored, the source touched, rebuilt and rerun green):

| Break | Result |
|---|---|
| The double writer blamed on the earlier edge | 2 cases fail (`9 == 10`, `18 == 27`) |
| Fields allowed to cross lines (the old tokenizer) | the cut-short case fails (`8 == 7`, column `1 == 41`) |
| Orphan row reported at the tokenizer's position; edge source blamed on the target | 2 cases fail (`4 == 3`, `27 == 25`, node `2 == 1`) |

## Evidence

- **win-debug:** the whole tree builds. `crd-chir-tests` passes 29 cases (5,355 assertions), up from 28 (5,226).
- **win-shipping, win-clang-cl-shipping, win-asan** (vcvars environment): `crd-chir-tests` passes 29 cases (5,355) on
  each, with no ASan report; the clang-cl link was clean first time.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each rebuilt `schema.cpp` and the tests and pass 29
  cases (5,355) with no sanitizer report.
- **Checks:** strict tidy is clean on the four changed C++ files. clang-format `--dry-run` shows only the repository's
  hand alignment on changed lines; its one wrap finding on new code (a comment) was applied by hand. The Allman check,
  the 8 ctest guards, check-master-plan and check-repository pass.

## DIAG.8a after this batch

Every listed local clause is shown: CEIR text and binary, cook and reload, CHIR lowering, the host provider, entries
and generations, intrinsic sites, GPU dispatch and CKIR records, and now the CHIR graph document. The row stays Partial
only because nothing of it is published yet; its first hosted run must show the suites listed in the row.

## Also recorded

The user's second opt-in DX12 hardware fault run (DIAG.7b(h2)), pasted again on 2026-10-07, matches the run already
recorded in [user items](2026-10-07-user-items-wpr-and-vulkan-loss.md): `submit=00000000 wait=00000000
reason=00000000`, an empty removal record and exit 30 (completed). The user's decision stands; the route is
explicitly unqualified and no further hardware run is planned. `context.md` no longer says DIAG.7a waits on that run,
and the DIAG.7c row no longer says the DX12 counterpart will be proven by it.
