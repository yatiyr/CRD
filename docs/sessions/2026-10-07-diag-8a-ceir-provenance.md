# DIAG.8a CEIR authored-source provenance, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8a](../ROADMAP.md#slice-diag.8a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.7c(h) handed to the user](2026-10-07-diag-7c-h-handed-to-user.md).

## Goal

DG13 names the gap: CEIR has `SourceLoc` and stable ids, but nothing carries an authored position from text through
optimization, binary cooking and the compiled plan to a runtime error. This batch closes that gap inside CEIR. CHIR
lowering, CKIR/GPU validation navigation, host-provider errors and reload retention are the row's remaining clauses.

## What was missing (measured before coding)

- The text parser recorded **no** op location: every parsed op had `SourceLoc{0,0,0}`, and a parse error reported only
  a byte offset.
- `Operation::loc` is part of the BODY chunk and therefore of `stable_hash`. Writing text positions there would make a
  whitespace edit change the content hash, the cook-cache key and the `text == builder` hash equality the cook tests
  assert. CSE equality and the content hash both compare attributes, so an attribute was no better.
- Passes dropped origins: the reshape fold's new op had no location and a fresh id; CSE kept one of two ops silently.
- `CompileResult` and `RunResult` carried bare enums; the plan kept no per-instr origin.
- The CHIR lowering creates CEIR ops without a location (CHIR nodes do carry spans).

## What landed

- **`crd/ceir/provenance.hpp` (new).** `Origin{loc, node, space}` with `OriginSpace` CarrierOp (the op carrying the
  record), CeirOp (another op by stable id) and ChirNode (reserved for the lowering). `ProvenanceGap` is None,
  NoOperation or NoSourceLocation. `resolve_provenance` returns the op's recorded origins, else its builder-declared
  `loc()`, else the gap; `render_provenance` prints `file:line:col op#id`, the other origins after `from`, or the gap.
- **A side table in the Context.** `set_origins`, `origins` and `derive_origins`, keyed by op. It is outside
  `stable_hash`, CSE equality and the printed text. `Operation::loc` is unchanged and remains the fallback.
- **Parser.** `parse(ctx, text, file_id)` records each op's first-token line:col as a CarrierOp origin; a parse error
  reports `error_line`, `error_col` and `error_file_id`. `parse(ctx, text)` passes file id 0.
- **Passes.** CSE records the survivor as standing for both ops, the reshape-of-reshape fold and the authored
  BuildOpFromInnerOperand rule record both reshapes. `derive_origins` resolves a CarrierOp entry to the replaced op's
  stable id before that op is erased, so the drivers (`cse_run`, `greedy_rewrite`, `greedy_rewrite_rules`) assign
  stable ids first. A no-op removal (identity reshape, ReplaceResultWithOperand) leaves no residue: nothing executes
  for it.
- **Binary 'ORIG' chunk.** Sparse: an entry per op with origins, in body pre-order, each origin
  `{u8 space, u64 node, u32 SRCM ref, u32 line, u32 col}`. It is written by `serialize` only when some op recorded an
  origin, so an origin-free blob is byte-identical to before, and never by `stable_hash`. Origin-only file names are
  interned into SRCM after every BODY file. The decoder bounds every count by the chunk bytes and rejects an
  out-of-range or non-ascending op index, an unknown space, a bad file ref and trailing bytes.
- **Plan.** Each compiled instr has an `InstrSite` (op id, origins, gap) resolved at compile and owned by the plan, so
  it outlives the Module; `Seq::sites` is parallel to `instrs` and never read by the dispatch loop. `compile` assigns
  stable ids first. `CompileResult::op` names the offending op (the innermost; a parallel body's preflight offender);
  `RunResult::fault` names the instr where the error originated (first wins, so the innermost); `instr_provenance`
  resolves either. The reference interpreter already returned `ExecResult::op`, which `resolve_provenance` reads.

## Test

`tests/execution/ceir/test_provenance.cpp` (new, `[ceir][diag]`, 6 cases). Expected positions come from scanning the
text, not from the parser.
- **Positive path.** A program is printed, parsed under `programs/diag/provenance_main.ceir`, run through CSE by the
  `PassManager`, serialized, loaded into a fresh Context (equal content hash) and compiled. The surviving addi names both
  authored addi lines (its own id first, the erased duplicate's id second); the consumer names its own. A run with step
  1 returns 12 with no fault; step 0 raises BadForStep at the `For` instr, whose provenance and rendering name the
  `core.for` line in that file. The reference interpreter's error op resolves to the same line.
- **Invalid input.** A bad op name on line 3 reports `error_line` 3, the column of its byte offset and the file. A
  `core.foreach` the plan cannot compile is blamed at its authored line; a missing entry has no op (`no-operation`).
- **Layout independence.** Doubling every line break moves every origin, while content and interface hashes stay
  equal.
- **Unknown attribution.** A builder module with no locations writes no ORIG chunk; its instrs report
  NoSourceLocation with the op id (`no-source-location op#N`); an invalid InstrRef reports NoOperation.
- **Fold.** The reshape fold carries both builder-declared locations (outer, then inner) with their stable ids, and
  declares no location of its own.
- **Corrupt ORIG.** An unknown space byte and an oversized entry count are both rejected with their messages.

Teeth (win-debug; each restored and the lane rebuilt):
- CSE without `derive_origins`: the survivor has one origin; 1 assertion fails.
- ORIG never written: 5 assertions fail (survivor, run fault, interpreter, corrupt-chunk setup).
- `raise` not latching the fault: `bad.fault.valid()` fails.

## Evidence

- **win-debug:** the whole tree builds. `crd-ceir-tests` passes 540 cases (12,649 assertions), `[diag]` 6 cases. CTest
  over every CEIR/CHIR test (`-R "ceir|chir|CEIR"`, 1,097 tests, including the cook, host, GPU-device, fuzz-corpus
  and CHIR suites) passes on the RTX 4070 Ti SUPER.
- **win-shipping, win-clang-cl-shipping, win-asan:** `crd-ceir-tests` builds and passes 540 cases on each (the clang-cl
  thin-LTO link was clean on the first attempt; no ASan report).
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build and pass `crd-ceir-tests`, 540 cases, with
  no sanitizer report. On linux-gcc-debug the dependents of the changed headers also build and pass:
  `crd-ceir-cook-tests` (37 cases), `crd-ceir-host-tests` (24) and `crd-chir-tests` (21).
- **Checks:** strict tidy is clean on the 13 touched C++ files (one nested conditional fixed). `clang-format
  --dry-run` reports only the hand-aligned declarations and one-line `case` labels the repository uses; the new
  header's wide-character comment lines were rewrapped. The Allman check, the ctest guards, check-master-plan and
  check-repository pass.

## Remaining on DIAG.8a

- CHIR lowering records ChirNode origins (node span + CHIR id) on the CEIR ops it creates.
- CKIR node identity and a GPU validation error navigate to the CEIR dispatch's origin (ceir-gpu's lowered dispatch
  keeps its `const Operation*`; DIAG.7a's identity registry names the GPU objects).
- A host-provider (native intrinsic) runtime error names its op; script/intrinsic entry points and asset generation.
- Cook and reload: `cook_program_text` and `ReloadSet::reload_source` take a file; a failed reload reports the
  offender's provenance (copied out of the transient Context) while the installed generation keeps its metadata.
