# DIAG.8a entry refusals, generation sites and CHIR reload provenance, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8a](../ROADMAP.md#slice-diag.8a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [host provider](2026-10-07-diag-8a-host-provider-provenance.md).

## Goal

Close three of the CPU-side DIAG.8a clauses: an entry refusal names the entry that was asked for, a fault in a
hot-reloaded program names the generation that ran it, and a CHIR-lowered program keeps its CHIR node origins through
failed reloads. The GPU/CKIR clause and the intrinsic clause stay open (see the end).

## What was there (checked before coding)

- `Interpreter::invoke`, `plan::compile` and `HostProvider::execute` (through `invoke`) returned `NoEntry` with no op
  and nothing else when the module had no symbol table or no such symbol. The only existing check
  (`test_provenance.cpp`) asserted that bare `no-operation` gap. A found entry with a wrong argument count already
  named the entry function op (`BadArity`), and `invoke_region`'s `NoEntry` means "a body with no block", not a lookup.
- `ReloadSet` kept the current generation and a one-deep zombie, but nothing tied a fault back to a generation. A plan
  compiled from generation N keeps running after N+1 is installed, so resolving its fault against whatever is
  installed now would name the wrong text. `Generation` carried no number; the slot's number lived only in handles.
- A lowered CHIR program is cooked with `cook_program` (the builder path). Its blob carries `ChirNode` origins through
  `ORIG` (proved by the CHIR lowering batch), but no test installed one in a `ReloadSet` and then failed a reload.

## What landed

- **Entry refusals** (`crd/ceir/exec.hpp`, `crd/ceir/plan.hpp`, `exec.cpp`, `plan.cpp`): `ExecResult::entry` and
  `CompileResult::entry` are owned `containers::String` copies of the requested name, set only for a lookup-failed
  `NoEntry` (no symbol table, or no such symbol). The caller's view can die before the result is read. The provider
  inherits it through `invoke`.
- **Generation sites** (`crd/ceir/cook/hot_reload.hpp`, `hot_reload.cpp`):
  - `Generation::number` is the slot generation its install minted; it is set at both install sites and carried into
    retirement.
  - `ReloadSet::locate(handle, Provenance)` (a plan's `instr_provenance`) and `locate(handle, const Operation*)` (an
    interpreter's error op) return a `GenerationSite`: asset, generation, `GenerationState` (Current, Retiring or
    Gone), that generation's content hash, and `where`, rendered in that generation's own Context.
  - The generation is matched by number, never by pointer, because a freed generation's memory can back a later one.
    A generation the set no longer holds (drained, removed, never minted, another asset) is Gone: only the asset and
    the number are named, and `where` is empty because the file names died with its Context.
  - `render_generation_site` prints `asset#<id> gen#<n> <state> <where>`.
- **Test dependency:** `crd-chir-tests` links `crd-ceir-cook`, declared as `TEST_DEPENDS ceir-cook` on the chir module
  (the module-graph check refused the link until it was declared).

## Tests

Expected positions come from scanning the text (CEIR or CHIR), never from the parser.

- `tests/execution/ceir/test_provenance.cpp`, new case (`[ceir][diag]`): the plan and the interpreter refuse `mian`
  and each result still holds `mian` after the caller's buffer is overwritten; a module with no symbols names
  `absent`. Control: a found entry leaves `entry` empty, and a bad arity is blamed on the entry function's authored
  `sym_name = "main"` line in the authored file.
- `tests/execution/ceir-host/test_host_provenance.cpp`, new case (`[ceir][host][diag]`): the crd-jobs provider names a
  misspelt entry from an overwritten buffer; its bad-arity control names the authored `pfor_fault` function line
  after CSE, serialization and a fresh-Context load.
- `tests/execution/ceir-cook/test_reload_provenance.cpp`, new case (`[ceir][reload][diag]`): generation 1 is cooked
  from file A and a plan is compiled from it; a body edit in file B (every line moved) hot-swaps to generation 2.
  The old plan's fault is located as `gen#1 retiring` with generation 1's content hash and file A's line and column;
  the interpreter on generation 2 is located as `gen#2 current` in file B; a failed (unparsable) reload changes
  neither; after `drain()` the old plan's fault is `asset#4500 gen#1 gone` with no file; a default handle is Gone.
- `tests/execution/chir/test_chir_reload_provenance.cpp`, new (2 cases, `[chir][diag]`): the committed
  `event_handler.chir` is lowered under its file, cooked and added. Its compile refusal (`CapturedValue`, the outer
  capture) names the `parallel_for update` node: `OriginSpace::ChirNode`, its CHIR id, its line and column, and
  `gen#1 current`. Three failed reloads install nothing and change none of that: a truncated blob (`ReadFailed`), a
  renamed handler (`ContractChange`, the exported entry changed) and an unparsable source (`ParseFailed` at line 2).
  A reformat-only reload of the same CHIR from another file (every node moved) is NoChange: the handle stays current
  and the refusal now names the moved line and column in the new file, with the same CHIR id.

Measured while writing the CHIR case: removing the `await task` node is a HotSwap (the caller contract is unchanged),
so it could not serve as the rejected candidate; renaming the handler is a ContractChange.

Teeth (win-debug; each restored, the lane rebuilt and all four suites rerun green):
- interpreter entry copy removed: 3 assertions fail in `crd-ceir-tests` (named and symbol-less module) and 1 in
  `crd-ceir-host-tests`;
- plan entry copy removed: 3 assertions fail in `crd-ceir-tests`;
- the hot-swap install no longer stamps its number: the generation-2 sites are Gone (6 assertions);
- `held()` returns the installed generation regardless of the handle's number: the retiring and gone checks fail
  (9 assertions);
- origins refreshed on every rejected candidate, not only NoChange: the contract-change section names the rejected
  candidate's file and node (3 assertions);
- the refresh drops the origin space to `CeirOp`: the CHIR reformat case fails its `ChirNode` check, and the existing
  CEIR reformat case loses its node identity (3 assertions).

## Evidence

- **win-debug:** the whole tree builds (`exec.hpp`, `plan.hpp` and `hot_reload.hpp` are widely included).
  `crd-ceir-tests` 541 cases (12,680 assertions), `crd-ceir-cook-tests` 41 (1,913), `crd-chir-tests` 28 (5,226) and
  `crd-ceir-host-tests` 29 (922) pass.
- **win-shipping, win-clang-cl-shipping, win-asan:** the same four suites build and pass with the same counts. No
  ASan report; the clang-cl link was clean first time.
- **WSL:** linux-gcc-debug, linux-gcc-asan (with UBSan) and linux-clang-tsan each build the four suites on the final
  sources (the module change reconfigured each tree) and pass them with the same counts, with no sanitizer report.
- **Checks:** strict tidy is clean on the 10 changed C++ files. `clang-format --dry-run`: the line-wrap findings on new
  lines were taken by hand; what remains is the repository's hand alignment and the include grouping the sibling
  `test_chir_provenance.cpp` already uses. The Allman check, the 8 ctest guards, check-master-plan and
  check-repository pass.

## Hosted CI must later show

`crd-ceir-tests` (541 cases), `crd-ceir-cook-tests` (41), `crd-chir-tests` (28) and `crd-ceir-host-tests` (29) green on
all six lanes, including the new `diag 8a` cases. `crd-chir-tests` now links `crd-ceir-cook`; every lane already builds
that library, so the new link edge should change nothing else.

## Remaining on DIAG.8a

- CKIR node identity and a GPU validation error navigate to the CEIR dispatch origin (needs device runs).
- An error at an intrinsic (`[op.native]`) op names its authored op and its native provider. Today the reference
  interpreter's `NoSemantics` names the op but not the provider, and the host scene resolver
  (`evaluate_scene_resolve`, crd-ceir-gpu) returns `UnresolvedSceneHandle` with no op at all.
