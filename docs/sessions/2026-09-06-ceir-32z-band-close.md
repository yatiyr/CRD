# 2026-09-06 — CEIR-32 band close (CHIR + language prototype — the §143 event-handler proof)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**CEIR-32 is CLOSED.** CHIR — the Cerid **High-level IR / language layer** (ADR-0108/0109/0128) — reached its CHIR-0 runnable
proof: the §143 event handler `on event: query entities / parallel update / await async task / update state`, authored TWO ways
(CHIR **text** + a **CR-D007 graph schema**), both lowering through the SAME CHIR→CEIR path to the byte-identical CEIR module,
and now **surviving a hot reload**. No new CEIR ops (the zero-new-ops discipline): the five §143 constructs lower onto
`func.func`+domain / func block-args / `task.parallel_for` / `async.launch`+`await` / `core.state`. Host-only module
`engine/chir` (`crd-chir`), one-way into `crd-ceir`.

32e (this session's slice) landed the load-bearing correctness decision — ADR-0128 **D3**: CHIR pins each `state` cell's CEIR
stable id from its SOURCE identity so a re-lower after an edit reproduces the state schema the CEIR-10a supervisor keys on, and
the runtime (exec `restore_state_by_id`, which matches by stable-id VALUE) **MIGRATES** the cell's value instead of losing it.
32z resolves the band and flips the row.

Detail → `docs/detours/D-007-ceir-tracker.md` (CEIR-32 row) + `docs/systems/chir.md`. No measured board this band —
CHIR-0 is a host-only compiler frontend (parse / lower / reload-schema) with no runtime performance surface to time;
there is nothing measurable, not a claimed census exemption (the 32-0 census row records no bench ruling). `[bench close]`
(`feedback_benchmarks_mandatory_at_slice_close`) applies to perf-relevant slices.

## Slices (row per slice)

| slice | what | record |
|---|---|---|
| 32-0 | band-open census (advisor grep-verified): §143 = five constructs, ZERO new ops; the graph is a SCHEMA lowering through the same path | tracker (docs-only) |
| 32a | [ADR-0128](../decisions/0128-chir-0-language-binding-decisions.md) — the CHIR-0 binding decisions against the corpus (scope, ownership composite, D1 map, D3 reload rule, D5 two-projections, D6 placement) | ADR accepted |
| 32b | the source model (`node.hpp/cpp`) + the CR-D007 graph-schema projection (`schema.hpp/cpp`) + committed `event_handler.chirgraph` + the reading gate | device-free (2 Win + 2 Linux) |
| 32c | the CHIR TEXT projection (`text.hpp/cpp`: `print_chir`/`parse_chir`) + committed `event_handler.chir` + the four-tooth parity anchor + spans-survive + `line:col` reject | device-free |
| 32d | the CHIR→CEIR lowering (`lower.hpp/cpp`) + the text/graph PARITY gate (both → byte-identical CEIR) + structure-verifier-clean + round-trip + D1 map + D2 read-path identity + edge-threading falsifier | device-free |
| 32e | HOT RELOAD + STATE MIGRATION (ADR-0128 D3): the source-derived, watermark-safe stable-id pin + the StateDecl/StateUpdate collapse + the CEIR-10a decision-table proof | device-free (2 Win + 2 Linux + tidy + guards) |
| 32z | band-row flip ✅ + this record | this record |

## The 32e mechanism (ADR-0128 D3, the load-bearing correctness decision)

- **The pin.** Each program-scope `StateDecl`'s `core.state` cell is pinned to `id = 1 + (chir_node_id % kChirStateIdReserve)`,
  `kChirStateIdReserve = 1<<24` — a reserved LOW band. The seed is the decl's CHIR `StableId` (itself `fnv1a(scope ‖ name)`,
  **position-independent**), so a body edit / decl reorder / cell insert reproduces the SAME id (→ the value migrates), while a
  **rename** yields a DIFFERENT id (→ a fresh cell identity). Cells are processed in CHIR-id order and linear-probed within the
  band → a (negligible) hash collision resolves deterministically + position-independently, never a silent alias.
- **Watermark-safe (D3 constraint b).** `lower_chir` floors the module watermark to the reserve (`reserve_stable_id_floor`), so
  `assign_stable_ids` gives every OTHER (sequential, reload-invisible) op an id strictly ABOVE the band — the pinned ids never
  track the sequential space (the raw-hash watermark-jump the ADR forbids). The falsifier IS band-membership: a pinned cell sits
  in `[1, reserve]`, a `func.func` above it — unpin the cell and it would land above the reserve too.
- **Two NEW lowering-time `Context` APIs (D3 constraint a).** `pin_stable_id(op, id)` + `reserve_stable_id_floor(m, floor)`,
  DISTINCT from the deserialization-only `set_stable_id`/`set_stable_id_watermark` the ADR pinned (not repurposed).
- **The collapse (D1/D3).** 32d emitted TWO placeholder `core.state` (decl + update); 32e unifies to ONE cell per DECLARATION —
  the `update state` verb folds into the cell's `%next` feedback (resolved through the writing `StateUpdate`'s `updated` in-pin;
  a parallel_for source has no CEIR result → `konst(1)`, distinct from `%init` so deleting the verb/edge stays observable). This
  is the "real feedback wiring 32d flagged for 32e", and it makes the reload schema exactly one cell per state variable.

**The proof (6 `[chir]` reload cases on the §143 program built with edits), the CEIR-10a decision table:**

| edit | interface_hash | contract_hash | verdict |
|---|---|---|---|
| add a body statement (2nd `await`) | equal | equal | CompatibleReuse |
| reorder two `state` decls | equal (source-derived ids) | equal | CompatibleReuse |
| add a `state` cell (declared BEFORE the existing one) | differ (schema grew) | equal (`core.state` = zero effects) | Migrate — existing cell keeps its id |
| rename a `state` cell | differ (id changes) | equal | Migrate |
| signature (new query component → new func param) | differ | differ | Reject |
| text vs graph projection | — | — | same cell ids (one reload schema) |

## Gate (2 Windows + 2 Linux + tidy, scoped to crd-ceir + crd-chir)

| config | crd-chir-tests `[chir]` | crd-ceir-tests `[reload],[stable-id]` (blast radius of context.cpp) |
|---|---|---|
| win-debug | 138 assertions / 19 cases | 79 / 18 |
| win-asan | 138 / 19 | 79 / 18 |
| linux-gcc-debug | 138 / 19 | 79 / 18 |
| linux-gcc-asan | 138 / 19 | 79 / 18 |

ctest by-name (win-debug): `chir 32` 19/19; `crd-ceir-opgen-drift`/`-validator` pass (no `.ceirop.toml` change → the all-dialects
opgen scar stays asleep); `crd-simd-emission-check` green under vcvars. tidy (LLVM-20): 5/5 files clean (`context.hpp/cpp`,
`lower.hpp/cpp`, `test_chir_lower.cpp`).

## Deferral ledger (each with a trigger — no speculative build)

- **value-feedback into `%next`** (a real per-frame updated value fed to the state cell) → blocked by `task.parallel_for` having
  NO CEIR result (a task-dialect fact); the seam is wired (the write folds into `%next`), the value is a placeholder const.
  Trigger: `task.parallel_for` gains a reduction/result.
- **multi-handler SHARED program state** → v1 emits the program-scope cells in the FIRST handler's func only (the duplicate-pin
  guard); a program with >1 handler sharing a cell needs module-level state CEIR does not model yet. Trigger: a multi-handler CHIR
  program sharing a `state`.
- the full ADR-0128 §7 deferral ledger (generics/traits/ADTs/pattern-match SEMANTICS/modules/reflection/error-model/stdlib
  boundary/full-async/the visual EDITOR → CEIR-33/D7E) carried forward, each to its first consumer.

## Scars (memory)

- `scars_ceir_ir.md` — the 32c/32d clauses stand. NEW (from 32e, added to the build/workflow home): **a non-ASCII TEST NAME
  passes a tag-filtered run but FAILS ctest-by-name** — ctest invokes each case by passing its NAME back as a Catch2 filter, and
  the Windows console codepage mangles an em-dash → "No tests ran" → ctest reports the test failed while `exe [chir]` was green.
  The [ascii-only-test-names] rule is enforced BY ctest-by-name, not by the compiler or a tag run. Reinforces "run ctest, never
  the bare binary" (a bare `[chir]` run would have shipped the broken name).

## Uncommitted batch (the user commits — NO AI co-author trailer)

CEIR-32 rides the ongoing CEIR-26→32 working-tree batch (last commit `c35548a "working on CEIR-25"`). The 32e/32z
additions/mods: `engine/ceir/include/crd/ceir/context.hpp` + `engine/ceir/src/context.cpp` (+`pin_stable_id`,
+`reserve_stable_id_floor` — lowering-time, distinct from the deserialization pins) + `engine/chir/include/crd/chir/lower.hpp`
(+`kChirStateIdReserve` + the D3 doc) + `engine/chir/src/lower.cpp` (the state-pin plan + the StateDecl/StateUpdate collapse +
the watermark floor + the duplicate-pin guard) + `tests/chir/test_chir_lower.cpp` (+6 32e reload cases + builders) + the tracker
(the 32e/32z rows + the band-row ✅) + `docs/systems/chir.md` (the 32e section + the struck 32d two-cell prose + status) + this
session log + `context.md`. `git status` is authoritative; the tracker CEIR-32 row carries the definitive per-slice files.

## Proposed commit message (Conventional Commits — NO AI co-author trailer, per CLAUDE.md/AGENTS.md)

```
feat(chir): CEIR-32 CHIR-0 language prototype — the §143 event-handler proof

Close the CEIR-32 band. CHIR (the Cerid high-level language layer, ADR-0128)
reaches its CHIR-0 runnable proof: the §143 event handler authored two ways
(CHIR text + a CR-D007 graph schema) lowers through the same CHIR->CEIR path
to the byte-identical CEIR module, then survives a hot reload — no new CEIR
ops (func.func+domain / block-args / task.parallel_for / async.launch+await /
core.state).

32e lands the load-bearing reload rule (ADR-0128 D3): CHIR pins each state
cell's CEIR stable id from its source identity (a position-independent
fnv(scope||name) mapped into a reserved low band, with a watermark floor so
sequential ids sit above it), so a re-lower after a body/reorder/insert edit
reproduces the state schema CEIR-10a keys on and the runtime migrates the
cell's value by id instead of losing it. Two new lowering-time Context pins
(pin_stable_id / reserve_stable_id_floor), distinct from the deserialization-
only set_stable_id. The StateDecl/StateUpdate lowering collapses to one
core.state per declaration (the update folds into the cell's %next). Six
[chir] reload cases prove the CEIR-10a decision table: body-edit/reorder ->
CompatibleReuse; add-cell/rename -> Migrate (the existing cell keeps its id);
signature change -> Reject; text and graph pin the same cell ids.
```
