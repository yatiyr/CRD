# CEIR-12c — resource alias/lifetime analysis over 4d effects (§78 / §26)

**Date:** 2026-08-10 · **Slice:** CEIR-12c (D-007 master spine) · **Status:** ✅ CLOSED · **ADR:** none — see below.

## Contract

D-007 CEIR-12c row: *"Alias/lifetime analysis over 4d effects — ports the frame graph's interval model; the WAR + slot-size
scars become IR-level ctest cases."* → §78 §26. This is the first real *analysis* slice of band-12: the effect-hazard model
(CEIR-4d) already yields ORDERING; 12c yields per-resource LIVE RANGES + the may-alias predicate the CEIR-12d planner will
color.

## No ADR — precedent, not omission

CEIR-4d (the nearest analysis precedent — `ops_hazard`/`collect_block_hazards`, the effect-derived hazard pass) shipped
**without a dedicated ADR** (`docs/decisions` has 0113 effect-widening + 0114 stable-identity, but nothing for the hazard
analysis itself). 12c is the same shape — an analysis reusing the pinned effect vocabulary — so it mirrors 4d: no ADR. The
one new bit of *vocabulary* (the `size_class` attr) is documented in the TOML + here; the band's ADR, if any, lands with
the planner (12d) where a real design choice (profiles, coloring strategy) is made.

## Design (advisor-reviewed at the fork; all six deltas integrated)

**Home** — mirrors `hazard.hpp`/Context: the vocabulary (`ResourceLifetimeClass` enum + `ResourceLifetime` struct) in
`context.hpp` next to `Hazard`; the analysis in `context.cpp` next to the hazard block (so it reaches the anon-namespace
`op_access_*` effect helpers — the "over 4d effects" rule needs them). No new .cpp.

**Scope** — per-Block, block list-order = the schedule-position proxy (exactly like `collect_block_hazards`; real
scheduling is §79/12d+). Cross-block / loop-carried ranges need the CFG — a named-forward.

**`Context::compute_block_lifetimes(block, out)`** — enumerates graph-owned resources (`resource.declare` results;
`resource.import` EXCLUDED — never planned) and computes each `ResourceLifetime{resource, declare, first, last, lifetime,
kind, size_class, exported}`:
- `first` = the declare's position; `last` = the last op whose operand's ROOT resource is this one.
- **View chains** — a `resource.view`'s result maps to its operand(0)'s root, so a use of a view is a use of the
  underlying resource (a Value*→root map built in pass 1).
- **One recursive pass-2 walker `visit_uses_and_effects` (the advisor's correctness fix, generalized twice)** — operand
  uses, `resource.export` marking, AND ambient-effect detection all run per op, **top-level AND nested**, attributed to the
  CONTAINING top-level op's position (the "pass"). ⛔ the invariant: *wrapping an op in a region must never weaken the
  analysis* — the pre-close review caught that the first cut recursed for uses but left export/ambient top-level-only, so a
  `scope{ export(%a) }` or `scope{ gwrite }` was invisible (a published resource read as aliasable). Fixed + both tested.
- **Over-4d-effects teeth** — any op (nested included) with an ambient Memory/Universe access (resolved
  `resource == nullptr`, reads‖writes — which includes every unregistered op via the synthetic Universe rw) extends the
  `last` of every resource declared at-or-before it. This is what makes the analysis genuinely consume the effect model.
- **Exported pin** — a resource reaching `resource.export` (at any nesting) is marked `exported` and its `last` pinned to
  block-END (external code may touch a published resource past any position — the WAR-scar IR edition).

**Predicates** (static): `resources_interfere(a,b)` = `[first,last]` intervals overlap; `resources_may_alias(a,b)` = BOTH
explicitly `Transient` (⛔ Unspecified/Persistent/History never pool — the conservative direction: a wrong alias is a
correctness bug, a refused one only a pessimization 12d relaxes per profile) ∧ neither `exported` ∧ same `kind` ∧ same
**NON-ZERO** `size_class` bucket (the slot-SIZE scar — `size_class` 0/unspecified never pools, same reasoning as
Unspecified lifetime) ∧ non-interfering.

**Doc-truth strike (SUPERSEDED-in-place)** — the CEIR-4d hazard-block comment claimed "distinct SSA Values assumed
non-aliasing (view-creation ops don't exist yet, so vacuously safe)". False since 12a minted `resource.view` and 12c built
the view→root map: `write(%buf)` vs `read(view(%buf))` is now a REAL `ops_hazard` false-negative. Struck in place — the
hazard walk does not yet consult the 12c root map (retrofitting it is 12d+ scheduler work, named-forward in the tracker
row); a view laundered through a region yield / call result escapes BOTH analyses (an unbound alias-model refinement, noted
in the 12c header comment too).

**The `size_class` fork** — CEIR resource types carry no byte size (`type_buffer.count` = BufferMode, `type_image.count`
= ImageDim). The frame graph aliases on `(size_class, kind)` buckets, not bytes (frame_graph.cpp:1224), so 12c mints an
OPEN `int size_class` attr on `resource.declare` — the analysis's own input. Consequences handled: it joined
`kIntentAttrNames` (presence on `resource.import` fires `IntentAttrOnImport` — the widen-vocabulary/audit-consumers scar);
the declare docs + dialect summary were amended in place; opgen regenerated (builder sigs unchanged — optional attr).

## Tests (`tests/ceir/test_lifetime.cpp`, +4 TEST_CASEs, ASCII names — the advisor's full checklist)

- transient disjoint control → **may_alias true** (the aliasable positive control 12z needs) + the **WAR-lifetime scar**
  (a late read extends the range to overlap → false).
- lifetime-class gate: transient+transient true; persistent / history / **unspecified** / mixed → false.
- **slot-SIZE scar** (disjoint + differing `size_class` → false; same → true), kind mismatch (buffer vs image → false),
  unspecified `size_class` 0 → false, **exported** (never aliasable + range pinned to block-end), the **ambient gwrite** +
  the **unregistered op** between two disjoint transients (each extends the earlier range → interference → false).
- **nested-region** use (extends to the containing op's position), **nested export** (marks exported through a region),
  **nested ambient** (a `scope{ gwrite }` still fires the rule), **view-chain** use (extends the root), import excluded.
- `size_class`-on-`import` → `IntentAttrOnImport` (in test_resource.cpp — pins `size_class` in `kIntentAttrNames`).

Effect ops hand-registered in a `u` dialect (the `test_hazard.cpp` pattern: `read`=MemoryRead operand0, `gwrite`=ambient,
`scope`=effect-free region-bearer) — no dialect ops minted for tests.

## Gate (GREEN)

- **win-debug 455/455 · win-asan 455/455 · linux-gcc-debug 455/455 · linux-gcc-asan 455/455** (was 451; +4).
- opgen regen + `--check` drift-clean + `test_opgen.py` validator OK. GCC clean (`-Werror=switch`).
- LLVM-20 tidy clean: `context.cpp`, `context.hpp`, `test_lifetime.cpp`. `crd-ceir-invariants` OK (I3/I5/I6/U-116 — the
  analysis reads op identity via `op_name`, no switch on op.kind). No recook/fuzz/version-bump.

## Named-forwards still open (for the band)

- **12d** — the memory planner: the interval-coloring port that CONSUMES `compute_block_lifetimes` + `resources_may_alias`
  (greedy slot reuse within a `(size_class, kind)` bucket; the §78 profiles latency/balanced/memory/deterministic; plan
  inspectable per §162). Cross-block / loop-carried ranges (the CFG) land with or before it.
- **13+ / §150** — providers lower `memory_domain` intent; bind import/export handles.

## Proposed commit (the USER commits — no AI co-author trailer)

```
feat(ceir): CEIR-12c resource alias/lifetime analysis over 4d effects (§78)

Add Context::compute_block_lifetimes — per graph-owned resource (resource.declare;
import excluded), a live range over the block's op order following resource.view
chains and nested-region uses, extended by ambient Memory/Universe effects and
pinned to block-end when exported. resources_interfere / resources_may_alias are
the predicates the 12d planner colors: two transients may share a slot only within
one non-zero (size_class, kind) bucket with disjoint ranges. Uses, exports, and
ambient effects are detected at any nesting (wrapping a region never weakens the
analysis). Mints the open size_class attr on resource.declare (the frame graph's
slot bucket; joins the import-rejection set). Ports the frame graph's greedy
interval model; the WAR-lifetime and slot-size scars become IR-level ctests.

Gate: 455/455 across win-debug/win-asan/linux-gcc-debug/linux-gcc-asan; opgen
drift/validator, LLVM-20 tidy, crd-ceir-invariants all clean.
```
