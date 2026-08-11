# ADR-0106 — Unified frame-graph runtime: `crd-render-graph` is the single live runtime

**Status:** Accepted (2026-08-03) — **CLOSED at RAF-12.3 (2026-08-06).** The `FramePassKind` migration adapter is
DELETED (retired to `ExecutorTypeId` + role bits); one live runtime remains. See the RAF-12 close amendment below.
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

---

## RAF-12 close amendment (2026-08-06)

The migration this ADR opened is complete. Three things landed after the original text, and two of them refine a
decision above — recorded here in place (the superseded-clause-struck-in-place rule) rather than in an index.

**1. The live unification mechanism is `AuthoredPass` + `run_authored_cb`, NOT `execute_frame(FrameGraphTemplate)`
(refines Decision #1).** Decision #1 named `crd::rendergraph::execute_frame` as "the single live runtime." RAF-12.2
wired the live `SceneRenderer` frame differently and better: `FrameRecorder::record` (`frame_runtime.cpp`) resolves
each cooked pass to a **`crd::rendergraph::AuthoredPass`** (via `to_authored_pass`) and records it through **one
generic dispatch** — `pb.execute(authored_pass_fn(), &ap)` → `run_authored_cb` → the `crd-render-pass`
`GraphExecutorTable`. `record_pass` + the 11 `record_*_via_executor` wrappers + every inline `draw_*` fallback are
DELETED. So the render-graph module exposes **two entry levels that share ONE executor registry**:

- **Live authored** — host-driven: `FrameGraphDesc` → `FrameRecorder` (for_each expansion, overlay weave, draw-list
  resolution to `DrawItem` handles) → `AuthoredPass` → `run_authored_cb`.
- **Programmatic / hand-built** — `FrameGraphTemplate` → `compile` → `execute_frame` → the same executors. This is
  the render-graph's own Gate-7 path ("hand-built == authored record the same descriptors"), exercised by
  `crd-render-graph-gpu-tests`; the `frame-cook` cooked→template load bridge (`build_frame_graph_template`) feeds it.

Both funnel through the **same** `run_authored_cb` executor dispatch — that shared funnel IS the unification. They
are two *levels* of one runtime (host-orchestrated vs. hand-built), not two representations to collapse; deleting
either removes real coverage, so neither is a §31 "parallel old path." The genuine parallel path this ADR set out to
delete — the `FramePassKind`→verb dispatch — is gone.

**2. `FramePassKind` is retired to `ExecutorTypeId` + role bits (RAF-12.3; completes Decision #4).** The pass MECHANIC
is now the cooked `crd::renderpass::ExecutorTypeId executor_id` on `FramePassDesc` (a constexpr FNV of the executor
name, proven equal to the runtime `executor_type_id` hash — gated in `test_frame_asset`), plus four role bits
(`depth_only` / `mrt` / `composite` / `indirect`) for the within-executor variants one id cannot spell (a depth-only
vs. MRT scene-raster pass; a compositing vs. plain fullscreen pass; an indirect vs. direct compute dispatch). A NEW
mechanic is a **registered executor**, never an engine-enum edit — the app extension point is `register_pass_executor`
(§22-10). The `.crdr` blob bumps **v7 → v8**: the retired kind byte becomes the executor id (u64) + a role byte, and a
custom pass' app `executor` string now rides the record (v7 silently dropped it — the field-both-sides-drop class). The
migration is **byte-identical** to the committed baseline (verified by stash-diff: identical GPU results both backends)
and removes the record-time string hash Decision #1 left behind (§22-18: no runtime string lookup at recording).

**3. The 59 combinatorial `draw_*/dispatch_*/trace_*` verbs are OFF `IRasterContext` (RAF-12.4).** The lowering moved
into the per-backend command encoders; the interface no longer carries a verb per feature-combination (§22-9). With
FramePassKind and the verbs gone, the mission §7 deletion list's primary items are grep-proven empty.

**Status closed:** one live rendering architecture; the `FramePassKind` adapter, the `record_pass` switch, the 11
wrappers and the 59 verbs are deleted; both frame-graph entry levels share the executor registry. ADR-0032's runtime
model stays superseded. Follow-ups that are DESIGN choices, not gaps, are named in the RAF-12.5 notes (the authoring
`FramePassDesc` keeps typed fields as validated authoring data per §8 — the cooked/runtime form is the payload; the
`crd://`→`engine://` alias stays as RAF-1 back-compat; `untracked_storage` stays as a scheduling hint the compute
executor reads).

---

## CEIR-15 amendment (2026-08-11) — the canonical DESCRIPTION is now `ceir.frame` (refines Decision #2)

CEIR-15 (FrameGraph unification, D-007 detour; ADR-0127) makes the frame graph's authored/cook representation a
canonical **`ceir.frame`** CEIR dialect — the §159 realization "the frame graph's barrier/lifetime passes ARE CEIR
analysis passes." This REFINES Decision #2's "`frame-cook` owns `FrameGraphDesc` (description)"; it does NOT supersede
the runtime decision — Decision #1 (`crd-render-graph` is the single live runtime) stands unchanged.

- **The canonical authored form is `ceir.frame`, not `FrameGraphDesc`.** The live cook is `parse → flatten →
  to_ceir_frame → validate_ceir_frame → from_ceir_frame → FrameGraphDesc → build_frame_graph_template → runtime`
  (CEIR-15f-2/3). `validate_ceir_frame` — the full §115 CEIR semantic verifier (structural + resource-shape + def-use
  + NEW-IN-CEIR consistency + program-contract for every executor + closed-vocab + DependencyCycle) — is the LIVE cook
  gate; the CEIR analysis passes (15d: hazards / DependencyCycle / lifetimes+first-use memory-plan) derive what the
  desc-side aliaser/validator used to. §121: no path executes without going through canonical CEIR — the pre-CEIR
  direct desc path AND its `CRD_FRAME_VIA_CEIR` opt-out flag are DELETED (@ 15f-3).

- **`FrameGraphDesc` is PRESERVED as the Desc-vs-Runtime STAGING form** — Decision #2's "legitimate staging, NOT
  duplication" STANDS. It is the intermediate between the canonical `ceir.frame` and the runtime `FrameGraphTemplate`;
  `from_ceir_frame` (ceir→desc) and `build_frame_graph_template` (desc→template) are both BLESSED lowering stages, NOT
  adapters to delete. (Reading **B** — a direct `ceir.frame → FrameGraphTemplate` re-lowering that would ELIMINATE the
  desc — was REJECTED: `build_frame_graph_template` is 796 lines of per-executor mapping; a direct version would
  DUPLICATE it, manufacturing the very duplication §126 exists to kill.)

- **The runtime, executor registry, load bridge, and `.crdr` blob format are UNCHANGED.** `crd-render-graph` +
  `crd-render-pass` + `build_frame_graph_template` (Decisions #1/#2) are preserved. The `.crdr` cooker
  (`cook_frame_graph`, asset_cooker) serializes the AUTHORED desc (composition intact — it does NOT flatten), so the
  15e flatten anchor-clear does not touch the blob: no format change, no version bump (stays v8).

**Status:** Decision #2's "description" role is amended in place (canonical = `ceir.frame`; `FrameGraphDesc` = staging);
all other decisions stand. CEIR-15 = the frame graph is a CEIR dialect gated by the CEIR verifier; one runtime, reached
only through canonical CEIR.
