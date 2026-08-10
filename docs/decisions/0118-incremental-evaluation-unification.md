# ADR-0118 — Incremental-evaluation unification: one dependency/dirty engine (crd-containers::IncrementalDag)

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8h**. ⛔ ENGINE-FIRST UNIFICATION (U-§125 applied to Cerid
itself): the tree held ≥2 dependency/dirty models (`render-asset-core::DependencyGraph`; the cook `content_hash`/
`interface_hash` invalidation semantics; the 8g `AnalysisManager` memo). 8h defines ONE generic dependency/dirty
engine and ABSORBS `DependencyGraph` into it THIS slice; a third graph is the failure mode.
**Phase:** D-007. Law: `docs/research/2026-08-09-ceir-universality-review.md` §B (row "Incremental evaluation");
mission §15/§71.
**Tags:** `[ceir]` `[incremental]` `[dependency-graph]` `[containers]` `[unification]` `[foundation]`

---

## 1. Context

Three pieces of an incremental-evaluation model existed, scattered + unlinked: (1) `render-asset-core::DependencyGraph`
— a generic `AssetId` DAG with a DETERMINISTIC `topo_order` (deps-first, ties by ascending id) + `affected_by(changed)`
(the transitive-dependents dirty/rebuild set — RAF-11 hot-reload's spine) + `validate_against`, but NO memoization;
(2) the cook's `content_hash` (the cook-cache key) + `interface_hash` (§107: an *interface* change invalidates
dependents, a *content-only* change hot-swaps) — the cache KEYS + invalidation semantics, but no engine; (3) the 8g
`AnalysisManager` — a per-Module memo with preserved-set invalidation. The ENGINE tying content-addressed memoization
to dependency-driven dirty propagation was missing.

(⛔ `hesap-sched::DependencyGraph` (task_graph.hpp) is OUT OF SCOPE: it is a task-SCHEDULING precedence graph with no
revision/dirty model — a different problem than content-addressed incremental evaluation; the "≥2 graphs" this slice
unifies are the render-asset + cook + analysis-memo trio, not the scheduler's.)

⛔ **The layering wall (why the home is crd-containers).** `crd-ceir` is asset-free (I4/I5) and `render-asset-core`
does NOT link `crd-ceir` — they are independent SIBLING modules, both over crd-core/containers/memory. So a unified
model in crd-ceir is a graph render-asset-core can NEVER adopt without inverting the module DAG — a permanent third
graph by construction. The ONLY home both can converge on with zero new link edges is a LOW shared module: the engine
is a generic data structure, so it lives in **crd-containers**, the natural sibling of the DAG it generalizes.

## 2. Decision

### 2.1 `crd::containers::IncrementalDag` — the one engine

A generic dependency DAG keyed by `NodeId = u64` (a content hash, an asset id value, any stable id; `0` = invalid).
It carries, per node, TWO revision hashes (the content/interface pair) and provides:

- **Structure + determinism (ABSORBED from DependencyGraph, byte-identical):** `add_node`/`add_edge` (nodes sorted
  ascending by id, deps sorted+deduped); `topo_order` (Kahn, tie-break = the minimum-id ready node — the SAME
  determinism contract, because that is why RAF-11 rebuilds are reproducible); `affected_by(changed)` (frontier
  reverse-reachability → the transitive-dependent set, then emitted in `topo_order` filtered to that set). Both return
  `false` on a cycle (⛔ NO diagnostics — crd-containers cannot depend on a diagnostic type; the caller reports the
  cycle). Node/dep iteration (`node_count`/`node_id_at`/`deps_at`) is exposed for a caller's own validation.
- **The incremental layer (NEW — the §107 rule GENERALIZED):** `set_revision(id, content, interface)`; and the
  headline `recompute_after_change(changed, new_content, new_interface, out)` — updates the node's revisions and fills
  `out` (topo-ordered) with the nodes that must RECOMPUTE: `changed` itself iff its content **OR** interface changed,
  PLUS its transitive dependents (`affected_by`) **iff its interface changed** — a CONTENT-ONLY change hot-swaps
  (dependents stay valid). ⛔ Self recomputes on EITHER change (not content-only): an interface change without a
  content change is incoherent under content-addressing (interface is a projection of content), so the engine
  over-recomputes self conservatively rather than let a dependent recompute against a node that never did (the
  EMPTY≠UNKNOWN direction — never stale). This is the cook's §107 semantics expressed as a generic engine rule.

⛔ **The engine holds the STRUCTURE + REVISIONS + dirty propagation, NOT the results.** A node's content hash is the
memoization KEY; the cached RESULT (an analysis result, a cooked blob) is CONSUMER-owned, keyed by that hash — so the
engine is one shared model without owning every consumer's payload type.

### 2.2 The absorb is REAL: `DependencyGraph` becomes a thin wrapper THIS slice

`render-asset-core::DependencyGraph` keeps its four-method public API **byte-stable** (`add_edge`/`add_node`/
`topo_order`/`affected_by`/`validate_against`) and swaps its internals to an owned `IncrementalDag` (mapping
`AssetId`↔`NodeId` losslessly — `AssetId` is a `u64` value, `valid() == value != 0`, value-ordered — so the emitted
order is IDENTICAL). `topo_order`/`affected_by` delegate + add the `CyclicDependency` diagnostic on `false`;
`validate_against` (which needs `AssetRegistry`, a render-asset-core type that cannot move down) stays in the wrapper,
iterating the engine's exposed nodes/deps. ⛔ A canonical model with ZERO real consumers is how third graphs are born
(the 7a capability field sat ownerless six slices); the absorb is done NOW, and **the existing RAF/render-asset/
frame-cook suites are the regression harness** — byte-identical order is the pin.

### 2.3 Named-forward (deliberately NOT wired this slice)

- **The `AnalysisManager`** is a DIFFERENT invalidation paradigm (pass-declared PRESERVATION vs
  dependency-PROPAGATION); forcing it onto the engine now means inventing the inter-analysis dependency edges 8g
  explicitly refused. Analyses become `IncrementalDag` nodes when analysis dependencies become EXPLICIT — the CEIR-26+
  pass-ecosystem's call, not 8h's. One honest sentence, no code.
- **The cook / CookDb** already PRODUCES the content/interface pair the engine consumes as its revision model — the
  unification is SEMANTIC (same key vocabulary), no code motion; the plan-cache wiring is CEIR-10b.

## 3. The recook story — ZERO motion

The engine is runtime state; `DependencyGraph`'s serialized §106 `CDEP` records are unchanged in shape (the wrapper's
API is byte-stable). No `kBinaryVersion`, no `kCeirCookSchema`, no hash change, no recook. (The gate still spans
crd-containers + render-asset-core + its dependents because the DependencyGraph internals changed.)

## 4. Consequences

- ONE dependency/dirty engine; `DependencyGraph` is now a view of it (not a rival); the cook + AnalysisManager
  converge on it by a documented, named-forward path (not a rewrite, not a permanent fork).
- crd-containers gains a foundational primitive (a DAG with revisions + the §107 dirty rule) that CEIR-10b's plan
  cache, CEIR-9's proofs, and every future DCC/CAD/notebook incremental workflow build on.
- The RAF-11 hot-reload rebuild order is preserved BYTE-IDENTICAL (the regression pin).

## 5. Alternatives rejected

- **(C) reference impl in crd-ceir** — render-asset-core cannot link crd-ceir; a permanent third graph by the layering
  wall, not "a third graph until adoption."
- **(A) a full cross-module rewrite** onto a new module + refactoring `affected_by` from scratch — steals RAF-11's
  shipped-band scope; the wrapper preserves its spine.
- **Wiring the AnalysisManager now** — invents the inter-analysis edges 8g refused; named-forward to the pass ecosystem.
- **A new engine that reimplements topo/affected_by** beside `DependencyGraph` — the exact third graph the mandate
  forbids; the absorb makes DependencyGraph a wrapper instead.

## 6. Test matrix

`crd-containers` (`test_incremental_dag.cpp`, `[containers][incremental]`): topo determinism (deps-first, ascending-id
tie-break) on a fixed fixture; `affected_by` transitive dependents in topo order; cycle ⇒ `false`; ⛔ **the §107
generic rule** — `recompute_after_change` with an INTERFACE change propagates to dependents, a CONTENT-ONLY change
does NOT (the test distinguishing this engine from a naive dirty-flag graph); `set_revision` round-trip. render-asset-core
(`test_dependency*`, existing): the FULL suite green as the wrapper's regression net — plus a determinism-parity pin
(the wrapper's `topo_order`/`affected_by` output on a fixed graph is exactly the pre-refactor order). The 4-config
CEIR gate + the render-asset-core + frame-cook test targets all green (the cross-module blast radius).
