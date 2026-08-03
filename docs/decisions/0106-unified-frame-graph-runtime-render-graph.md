# ADR-0106 — Unified frame-graph runtime: `crd-render-graph` is the single live runtime

**Status:** Accepted (2026-08-03) — **RAF-8 (in progress).** The `FramePassKind` switch remains as a migration
adapter until RAF-12 deletes it.
**Phase:** D-007 (RAF band) — RAF-8a/8b. Mission constitution: `docs/research/2026-08-03-gold-standard-asset-driven-rendering.md`.
**Tags:** `[renderer]` `[frame-graph]` `[gpu-context]` `[architecture]` `[render-path]`

---

## Context

RAF-0…7 built the asset-driven rendering foundation as **new additive leaf modules** (`crd-render-asset-core`,
`crd-render-program`, `crd-render-material`, `crd-render-pass` = the pass-executor registry, `crd-render-graph` =
the unified frame-graph runtime: `FrameGraphTemplate → compile → execute_frame`, records the RAF-2 canonical command
model into an `ICommandEncoder`). Each phase was "purely additive, sandbox untouched" and gated **in isolation**.

The consequence, made explicit at the start of RAF-8: **the live renderer was never wired onto them.** There are two
frame-graph execution paths over one device:

1. **Live** — `SceneRenderer` (the `IFrameGraphHost`) → `FrameRecorder::record` / `execute_frame_graph`
   (`frame-cook/src/frame_runtime.cpp`) drives the gpu-context `IFrameGraph` through a **`FramePassKind` switch of
   ~19 kinds** calling the ~57 `draw_*` verbs. Cooked frame assets are `FrameGraphDesc` (from `.crdr`).
2. **New (RAF-7)** — `crd-render-graph::execute_frame`, gated (167 assertions, both backends), **used by no live
   path** (verified: zero references under `sandbox/`, `scene-render`, or `frame-cook`).

Mission §2.1 forbids two representations of one concept at close ("no separate direct vs authored command models; no
synchronous draw verbs vs frame-graph draw vocabulary"). Per PRINCIPLES "deletion is the proof" corollary 3, an
unreachable runtime is indistinguishable from a missing one — so RAF-6/7's live-migration work (RAF-0's own RAF-6
invariant: "migrate `FramePassKind`→executors one kind at a time, both resolve") silently landed on RAF-8.

A second divergence: the RAF-0 design spec §3 type-ownership table assigned the executor **registry** and the frame-
graph **runtime template + compiled instance** to `frame-cook`. The RAF-6/7 implementation placed the registry in
`crd-render-pass` and the runtime in `crd-render-graph` (new modules `frame-cook` does not depend on).

## Decision

1. **`crd-render-graph` is THE single live frame-graph runtime.** The one runtime template is
   `crd::rendergraph::FrameGraphTemplate` (= RuntimeFrameGraphTemplate) and its `CompiledFrameGraph` (=
   CompiledFrameGraphInstance). The executor registry is `crd-render-pass`. **This corrects the RAF-0 §3 table** —
   `crd-render-pass`/`crd-render-graph` own the registry + runtime, NOT `frame-cook`. The table is struck in place;
   the code does not move (folding them into `frame-cook` would rebuild the "one giant rendering module" §18/§21
   forbid).

2. **`frame-cook` keeps the description + cooked forms and gains the load bridge.** `frame-cook` owns
   `FrameGraphDesc` (description) and the cooked `.crdr` blob (CookedFrameGraph). It gains a **Cooked →
   `FrameGraphTemplate`** load bridge via a **new `frame-cook → crd-render-graph` module edge**. This edge is
   acyclic: `crd-render-graph` depends only on `{render-asset-core, render-pass, gpu-context}` — neither `frame-cook`
   nor `crd-scene` — so no back-edge is created and the one-way graph holds. `FrameGraphDesc` vs `FrameGraphTemplate`
   is legitimate **Desc-vs-Runtime staging** (mission §6), not duplication; the duplication to delete at RAF-12 is
   the second *execution path* (the `FramePassKind` switch), not a second type.

3. **`SceneRenderer` keeps the `IFrameGraphHost` seam.** That seam is what keeps `crd-frame-cook ⊥ crd-scene`: the
   host (which owns the World) resolves draw-list queries / programs / for_each counts / overlays / external
   resources into **pre-resolved gpu-context handles** (`DrawItem`), and `crd-render-graph` consumes only those —
   it never sees a scene or ECS type. `SceneRenderer` becomes an **orchestrator** (resolve graph + views + draw
   lists, provide scene/material data, manage instances, trigger execution); the per-kind low-level draw logic moves
   into executor `PassRecordFn`s.

4. **The `FramePassKind` switch is a migration adapter, deleted at RAF-12.** Migration is **one kind at a time**:
   the old switch case and the new executor **both resolve** during migration; each migrated kind passes a
   **command-parity gate** (a command-capturing mock encoder: the old case's recorded canonical stream ≡ the new
   record fn's) **and** a **pixel-parity readback gate**; `crd-sandbox --smoke-test 2` stays green on **both
   backends** at every increment. No switch case is deleted until its executor passes both gates. RAF-8 splits into
   **RAF-8a** (wire the live runtime onto render-pass + render-graph, kind by kind) and **RAF-8b** (scene renderer →
   pure orchestration), so the absorbed migration scope is named, not hidden.

5. **Supersedes ADR-0032's runtime-ownership.** ADR-0032 (Frame graph v1) established the frame-graph design; its
   runtime execution model (the `FramePassKind`→verb dispatch) is superseded by the executor-record model here. The
   resource lifetime analysis / transient aliasing / barrier derivation / one-submission of the gpu-context
   `IFrameGraph` are PRESERVED (render-graph's `execute_frame` runs on top of them until RAF-12 unifies the two
   frame-graph objects).

## Consequences

- One live runtime; the ~57 combinatorial `draw_*` verbs + the `FramePassKind` switch + the giant `FramePassDesc`
  fields become deletable at RAF-12 (proven empty via repo-wide grep, per the mission deletion list).
- `crd-render-graph`'s executor model must GROW to the live scene's richness: a multi-item **draw list** (`DrawItem`
  with the DrawIndex / indexed-pull / depth+velocity-twin contracts), `for_each` cascade expansion (0 = the named
  failure `UnresolvedForEach`, never a silent skip; cascade caching = clear→LOAD + zero draws), and the overlay weave
  (inserted after the last geometry pass, `read_writes` its live-depth pre-tonemap target). These land in
  `crd-render-graph` (draw list) or at the host/template-build boundary (for_each → N passes; overlay injection) —
  never by leaking `FrameDrawListDesc`/ECS types into render-graph.
- Every migrated pass honors: the DrawIndex push contract, indexed-pull-vs-classic selection, the depth/velocity
  program twins (never fall back to the forward `program`), cross-backend PSO/state completeness + the
  `indirect_command_stride()` args offset (VK 20 B @0 / DX12 24 B @4 behind the DrawIndex root constant), and
  declared==recorded (RMW≠RWM, WAR-by-lifetime).
- RAF-0 §3 table + module-dependency line are corrected in place; RAF-6/7 are reclassified as **module-gated, not
  live-wired**.

## Alternatives rejected

- **Migrate the `frame_runtime.cpp` switch in place onto the registry, keep `FrameRecorder` as the runtime** —
  abandons the compile-once `CompiledFrameGraph` separation (§10) and orphans RAF-7's gated deliverable.
- **Move the registry + runtime into `frame-cook` per the original RAF-0 §3 table** — rebuilds the "one giant
  rendering module" §18/§21 forbid; discards two clean leaf modules. (The table's *diagnosis* that ownership was
  mislocated is right; the fix is to correct the table, not move the code.)
