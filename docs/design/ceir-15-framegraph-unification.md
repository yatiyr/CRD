# CEIR-15 — FrameGraph unification: the `ceir.frame` dialect (15-0 decision packet)

**Status: ◧ DESIGN-LOCKED 2026-08-11 (advisor-consulted; decision packet — no code this tick).** CEIR-15 is an
ARCHITECTURE band: it makes the authorable frame graph (REN-36's `.frame.toml` asset system) END at a `ceir.frame`
dialect (§39), per §126's eight migration steps. This packet reads the canonical sources (master roadmap §39/§126,
ADR-0106, ADR-0109 §4.2), records the dialect design + the migration spine, and names the ONE genuine design fork
(history/ping-pong hazard derivation) with options. No user-blocking fork (ADR-0108-flip class) surfaced — the
standing no-name-forward verdict + the tracker's deletion/parity contract cover the band's direction.

## The sources (read this tick, per the advisor's instruction 0)

- **§39 `ceir.frame`** (master roadmap L1239): the dialect OWNS — logical resources · whole-frame/workflow topology ·
  pass/work-unit nodes · reads/writes · history · persistent/external/transient resources · subgraphs · includes ·
  injection anchors · pass replacement · capability variants · explicit fallback · output/present endpoints ·
  multi-view · multi-window · queue hints · compile policies. `.frame.toml` becomes a FRONTEND; builders EMIT it;
  ONE runtime/compiler. ⚠ several items (subgraphs, includes, injection anchors, pass replacement, multi-window) are
  FORWARD capabilities BEYOND the current `FrameGraphDesc` — the dialect must be SHAPED to admit them, but 15a mirrors
  the CURRENT desc; the extras are named-forwards, not 15a scope.
- **§126 FrameGraph Migration** (L3362): the EIGHT steps map 1:1 to the tracker's 15a–15z — (1) `.frame.toml` frontend
  emits canonical CEIR = **15a**; (2) `FrameGraphBuilder` emits it = **15b**; (3) frame validation → CEIR verifiers =
  **15c**; (4) frame compilation → CEIR scheduling/resource planning = **15d**; (5) runtime executes CEIR plans =
  **15e**; (6) old duplicate runtime structures → adapters + (7) adapters deleted = **15f**; (8) one architecture
  remains = **15z**. "Preserve asset IDs where possible." **The tracker is faithful to §126 — no hidden ordering.**
- **ADR-0106** (`0106-unified-frame-graph-runtime-render-graph.md`): "render-graph is THE single live runtime" — the
  claim CEIR-15 SUPERSEDES. Per the master-spine memory, `.frame.toml`/`.crdm`/`.crdt`/`.crdv`/`.crdl` SURVIVE as
  frontends/domain assets (§39/§125); NOTHING RAF built is discarded — the RUNTIME collapses to one, the AUTHORING
  surfaces stay. Strike-in-place @ **15f**.
- **ADR-0109 §4.2** (the acyclicity gate): `crd-ceir` is HOST-ONLY and depends on NONE of its providers, so a NEW
  `crd-frame-cook → crd-ceir` edge is acyclic by construction (frame-cook is already a host-side cook module). The
  converter's home is a Fork below (F).

## The headline recon — the frame asset is ALREADY CEIR-shaped

The RAF-12.3 §7 fold (`frame_asset.hpp`) already made a pass **common graph metadata + a typed named-param payload**:
`FramePassDesc` = `name` + `executor_id` (the MECHANIC, a stable `ExecutorTypeId` name-hash) + `reads`/`writes`
(`FrameResourceRef`) + `for_each` + `queue` + `params[]` (a typed bag: Float/Int/Bool/Vec4/Enum/U32/String, every
executor-specific value — shader/kernel/draw_list/view/technique/RT-programs/clear/blend/depth/sampler/render-state/
role-bits). **That is an op: identity + operands + attr bag.** The old central `FramePassKind` enum is ALREADY retired.
So CEIR-15 is a PROMOTION, not a rewrite (the CEIR-0a headline restated for the frame layer). The resource model
(`FrameResourceDesc`: transient/persistent/ping-pong/external images, structured/counter/indirect-args/external
buffers, accel structures — full shape) and the draw-list model (`FrameDrawListDesc`: ECS all/any/none + cull/sort/
limit) map the same way.

## The `ceir.frame` dialect (design-locked shape — FOUR ops; resources REUSE the resource dialect)

⭐ **RESOLVED (advisor, 2026-08-11): the frame graph's resources ARE `resource.declare`/`resource.import` values — NO
new `frame.resource` op.** The decisive argument: the CEIR-12b/c/d planner (`compute_block_lifetimes`,
`resources_interfere`, `plan_block_memory`) is built OVER `resource.declare`'s attrs ("the planner plans declare's
resources, NEVER import's" — resource.ceirop.toml verbatim). A parallel `frame.resource` op would ORPHAN the whole
12b/c/d stack and force 15d to re-derive it — the exact opposite of the packet's "the frame graph's hand-rolled
lifetime/barrier pass becomes a CEIR analysis pass" headline. The 11 `FrameResourceKind`s decompose onto EXISTING axes:
Transient* → `lifetime=transient` · PersistentImage → `lifetime=persistent` · **PingPongImage → `lifetime=history,
history_length=1`** (the toml literally names the TAA prev-frame case) · ExternalBuffer/ExternalTexture/
AccelerationStructure → **`resource.import`** (host-resolved name) · IndirectArgs/Structured/Counter → declare +
frame-scoped attrs (`usage`/`stride`/`count`/`counter`). The frame SIZING (width/height/scale/samples/mips/depth_buffer/
no_alias/resizable) rides as OPEN attrs on the declare — validated by `find_frame_misuse` WHEN the declare sits inside a
`frame.graph` region (the open-attr precedent is IN-dialect: `resource.declare` already carries open planner tags
`streaming_priority`/`budget_class`). So the dialect is FOUR frame-shaped ops:

- **`frame.graph`** — a REGION op (the `render.scope` precedent, `kind="graph"`), the whole-frame container. Attrs:
  schema version, output/present endpoint. Its region holds the `resource.declare`/`import`s (with frame-scoped open
  attrs), the `frame.draw_list`s, and the `frame.pass`es.
- **`frame.draw_list`** — declares an ECS query, producing a draw-list value. Attrs: all/any/none component lists ·
  cull · sort · limit. Passes reference it by OPERAND (verifier-checkable, one source), not a name string.
- **`frame.pass`** — ONE op for EVERY mechanic. ⛔ NOT one op per executor (that recreates the retired `FramePassKind`
  enum as an opgen edit). The MECHANIC is a **symbol attr** `executor` (the executor NAME); the `ExecutorTypeId` is
  DERIVED by the same fnv1a hash at the boundary (the cook==record gate in `test_frame_asset` already exists) — one
  source of truth, and a custom executor's app id folds in as the same symbol. ⛔ **operands: opgen allows only the LAST
  operand variadic, so reads/writes CANNOT be two lists** — ONE variadic `resources` tail + a required `access` string of
  per-operand `{r|w|rw}` tokens (the 13a `compute.dispatch` / 14b `render.draw` house pattern, one source); a draw-list
  operand rides the same tail with token `r` (the walk knows by TYPE CLASS it is not a memory resource). Effects =
  GPUCommand + ambient MemoryReadWrite (the `render.scope` precedent), narrowed at lowering. Attrs: `for_each`/
  `for_each_arg` · `queue` · the folded param bag (below).
- **`frame.history`** — the Fork B read op (below). A Pure value constructor; result type = the operand's UNDERLYING
  resource type (NOT a View — a View-typed result risks `resource_root` chasing it); optional `frames_back` int (default
  1, matches the `history_length` ring). Verifier: the operand must be a `lifetime=history` declare result (an
  intent-misuse-class check). ⭐ **why B1 works, precisely:** `resource_root` chases `resource.view` BY OP KIND (the 13d
  retrofit), so a DIFFERENT op's result is automatically its own root — the false-intra-frame-RAW fix is STRUCTURAL, no
  hazard-walk special-case.

**Reads/writes as OPERANDS (resource-typed values)** is the 14a targets-are-operands convention — and it is what lets
CEIR's EXISTING hazard/lifetime machinery (`resource_root` + `ops_hazard`, bands 12/13) DERIVE the barriers + transient
lifetimes from the graph, which IS the §159 realization ("topology/lifetime/history become a CEIR dialect"). The
frame graph's hand-rolled lifetime/barrier pass becomes a CEIR analysis pass.

## Resolved forks

- **Fork A — dialect granularity: ONE `frame.pass`, executor as a symbol attr.** ✅ (above). Per-executor ops =
  re-inventing the central enum RAF-12.3 retired; rejected.
- **Fork C — the param bag maps to CEIR attrs DIRECTLY.** ✅ Two checks ran: (a) **AttrKind = {Int, Float, Bool,
  String, SymbolRef, Type} — NO Vec4**, so a Vec4 param DECOMPOSES to 4 float attrs (the 14a clear_r/g/b/a precedent).
  (b) **CEIR attrs are an OPEN set** — the opgen-generated verifier checks only REQUIRED attrs by kind (`ceir_opgen.py:505`),
  never rejecting extras. So `frame.pass` declares the COMMON params (executor/for_each/queue) as its schema, and every
  folded/app param rides as an ADDITIONAL attr, validated AT COOK against the EXECUTOR's schema (a cook verifier =
  15c), exactly as `frame_asset.cpp` validates params against the executor schema today. Enum params → the
  string-token precedent (render's load/blend/compare vocab) or a u32 attr.
- **Fork D — references as SYMBOL attrs; the CDEP claim SOFTENED (advisor).** shader/kernel/technique/RT-program
  params → OPTIONAL SYMBOL attrs on `frame.pass` (typed, findable — like render's `@program`). ⛔ **NOT a kernel_ref**:
  the `executor` attr names a MECHANIC (registered code), not a cooked asset — no interface hash. And `[op.kernel_ref]`
  is ONE per op (verify at 13c) while a pass references shader + kernel + technique + six RT programs — so the multi-ref
  CDEP dependency extraction is NAMED to **15c** (generalize the schema-driven `collect_dependencies` walk), NOT the
  15a toml. Draw-lists → OPERAND references to `frame.draw_list` results (verifier-checkable). Views / external buffers /
  accel structures → `resource.import` with a host-resolved NAME (the `IFrameGraphHost` seam = the proven 14z resolver
  pattern; CEIR never sees a scene type — the frame-cook ⊥ crd-scene invariant preserved).
- **Fork E — migration is a CONVERTER PAIR with round-trip identity.** ✅ Build `FrameGraphDesc ↔ ceir.frame` BOTH
  directions. The **round-trip-identity gate** (`toml → desc → ceir → desc′`, assert `desc == desc′`) is the 15a/15b
  semantic-equality instrument BEFORE pixels exist (the §121 builder≡text precedent, frame edition). The BACKWARD
  converter (`ceir → desc`) lets the existing runtime consume `ceir.frame` early (ceir→desc→old runtime) — so 15e is
  INCREMENTAL, not a cliff. `for_each` stays an AUTHORED attr (must round-trip); its build-time expansion is the 15d
  lowering's job (as today). Blob compat: the 13c CDEP v4→v5 append precedent + the 0h deletion table.

## THE genuine design fork — Fork B: history / ping-pong hazard derivation

⛔ **The §159 realization has ONE trap.** A `PingPongImage`/`PersistentImage` READ resolves to the PREVIOUS frame's
image and a WRITE to THIS frame's — TWO physical images behind ONE authored name. If `ceir.frame` wires both to the
SAME resource Value, the hazard walk derives a FALSE intra-frame RAW, and the ⛔⛔ RMW-scar verifier (`a pass must not
read what it writes`, `feedback_frame_graph_war_needs_resource_lifetime`) FALSE-POSITIVES on every TAA/SSR/DDGI/ReSTIR
pass. The dialect MUST make history VISIBLE so the hazard walk sees two identities. Options:

- **B1 — a distinct `frame.history(R) → view` READ op** (⭐ recommended). The read of a history resource goes through
  an op producing a DISTINCT "previous-frame view of R" value; the write targets R directly. The hazard walk sees
  read(history-view) vs write(R) as different `resource_root` identities → no false RAW. This is the **CEIR-12a
  `resource.view` precedent** (a view is a distinct Value with its own identity) applied to the time axis — minimal
  new surface, reuses the proven hazard machinery, and the read/write asymmetry is STRUCTURAL (an author cannot hold
  the parity bit wrong — REN-38-B1's whole point).
- **B2 — a distinct history TYPE CLASS.** The resource's type marks it temporal; the hazard walk special-cases a read
  of a temporal type as a different identity. More pervasive (every hazard site learns about temporality); rejected
  vs B1's op-local shape unless B1 hits a wall.
- **B3 — an attr/flag + a hazard-walk special-case.** Rejected: a flag the hazard walk must remember is exactly the
  implicit-parity-bit REN-38-B1 designed OUT; a structural op (B1) is gold-standard.

**✅ RESOLVED: B1** (advisor 2026-08-11). `frame.history(R)→(underlying type)` is a Pure read op; the false-RAW fix is
STRUCTURAL because `resource_root` chases only `resource.view` by op kind — a different op's result is its own root, no
hazard special-case. See the `frame.history` bullet above for the exact shape (frames_back attr, lifetime=history operand).

## Fork F — module placement (the ADR this band births)

The converter needs `FrameGraphDesc` (in `crd-frame-cook`) AND `crd-ceir`. Per ADR-0109 §4.2, `crd-ceir` is host-only
and acyclic, so **`crd-frame-cook → crd-ceir` is a legal edge** (frame-cook is a host cook module; no device, no
shading language crosses I3/I5). Options: (F1 ⭐) the converter lives IN `crd-frame-cook` (a new `crd-ceir` edge — the
smallest change; frame-cook already owns the desc + the cook); (F2) a thin `crd-frame-cook-ceir` bridge (the 13d
`crd-ceir-gpu` precedent) if the edge turns cyclic or the converter grows device-facing. **Recommendation: F1**
(host-to-host, acyclic, no bridge needed) — confirm the acyclic gate at implementation. This band BIRTHS an ADR
(the 0125/0126 precedent): **ADR-0127 — `ceir.frame` dialect + the frame-cook↔CEIR converter placement**, and
STRIKES ADR-0106 in place @ 15f.

## Per-slice plan (the §126 steps, technical)

- **15a — the `ceir.frame` dialect + the frontend converter.** Author `frame.ceirop.toml` — FOUR ops (`frame.graph`/
  `frame.draw_list`/`frame.pass`/`frame.history`); resources REUSE `resource.declare`/`import`. opgen; the type class
  (draw-list value; `frame.history` needs none — its result is the underlying resource type); `find_frame_misuse` (the
  `find_render_misuse` mirror — includes the frame-scoped-attr-on-declare-in-graph checks + the history-operand check).
  `.frame.toml → FrameGraphDesc → ceir.frame` (the forward converter) + `ceir.frame → FrameGraphDesc` (backward) + the
  ROUND-TRIP-IDENTITY gate. DEVICE-FREE. ADR-0127. **Sub-decompose: 15a-1 = the dialect (toml+opgen+type-class+register,
  compiles) → 15a-2 = find_frame_misuse + structural/§121-text tests → 15a-3 = the converter pair + round-trip gate.**
- **15b — `FrameGraphBuilder` emits `ceir.frame`** + the builder≡toml semantic-equality ctest (§121, frame edition).
- **15c — cook validations → CEIR verifiers.** The 38-D4-class validations (varying-contract, header-word, shape) +
  the executor-schema param validation become CEIR verifiers/cook-verifiers, each a POINTING diagnostic.
- **15d — frame compile → CEIR scheduling + the CEIR-12 planner.** `for_each` expansion, lifetime/aliasing (the CEIR
  hazard analysis), one-submission execution preserved (Gate-7 semantics). `FrameGraphTemplate`/`CompiledFrameGraph`
  (ADR-0106) become the lowering/plan layer.
- **15e — runtime executes CEIR plans;** the A/B harness runs old + new PER ASSET with a DETERMINISTIC CLOCK (the
  ⛔⛔ `feedback_ab_pixel_compare_needs_a_deterministic_clock` scar). Every shipped built-in frame asset (`forward_csm`,
  deferred, the sandbox frames) renders PIXEL-IDENTICAL through CEIR before its old path dies. **✅ 15e-a DEVICE-FREE PER-ASSET
  FIDELITY LANDED 2026-08-11 (4-config, test_frame_template_bridge.cpp):** all 15 shipped STANDALONE frame assets — every
  `forward_*` variant, `scene_cull`/`scene_mesh`/`scene_tess`/`scene_visbuffer`/`scene_rt`, `velocity_debug` (RT, mesh, tess,
  visbuffer, GPU-cull-consumer, velocity — the full topology diversity) — lower to a BYTE-EQUIVALENT `FrameGraphTemplate`
  through the CEIR path (`parse → to_ceir_frame → from_ceir_frame → build_frame_graph_template`), pass-for-pass + schedule,
  read from the REAL `.frame.toml` files (CMake `CRD_FRAME_ASSETS_DIR`). Since the CEIR path REUSES the existing runtime
  (Fork E), a byte-equivalent template + the same deterministic runtime ⇒ identical pixels — so pixel A/B (15e-b, below) is
  belt-and-suspenders, catching only execution-state a template comparison cannot. `scene_gpu_cull` EXCLUDED: a compute-only
  cull FRAGMENT (no `@output`), covered by the frames that consume its buffers. ⚠️ the CEIR model requires DECLARED draw lists
  (15d-5 finding) — every shipped asset already declares them (only the test FIXTURE had taken the shortcut).
  **✅ 15e-b ON-DEVICE A/B PARITY LANDED 2026-08-11 (4-config):** the live renderer's single cook path now optionally routes
  every frame through the CEIR round-trip (`scene_renderer.cpp` `route_frame_through_ceir`; env `CRD_FRAME_VIA_CEIR`, default
  OFF ⇒ byte-unchanged — the flag-OFF path is verified identical). The FULL `crd-scene-render-tests` suite runs flag-OFF then
  flag-ON on a REAL Vulkan **and** DX12 device (this 14900K host, `CRD_ASSETS_DIR` set) with an IDENTICAL **71/71** pass set —
  every shipped asset (REN-41), the RAF-10 app-COMPOSED graph with an injected custom C++ pass + custom executor, RAF-11 hot
  reload, and the REN-36/RAF-10 PIXEL assertions (`geometry_pixels`/`diff_pixels`) all render identically through CEIR. This is
  STRONGER than the originally-planned llvmpipe smoke (real HW, both backends, composition + reload + app-custom surfaces the
  cook round-trip never touched). **FIX — the composition contract:** the flag-ON RAF-10 composed graph exposed that
  `flatten_frame_graph` left a residual `[[anchor]]` (spent scaffolding) which `to_ceir_frame` CORRECTLY rejects as
  un-flattened composition; flatten now CONSUMES anchors (clears them after its own validation), so its output is a truly
  ordinary graph (Option A — it honours the §115 `parse → flatten → to_ceir_frame` contract; the alternatives — a silent
  converter drop, or modelling anchors in the execution IR — were rejected). Durable device-free pin: `test_frame_ceir.cpp`
  *"ceir 15e"* (compose → flatten → anchor-free → converter ACCEPTS → round-trip byte-equal, + the guard: an UN-flattened
  composed desc is REJECTED). Scar: `feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip`.
  **15e is CLOSED; next is 15f.**
- **15f — adapters + the duplicate frame path DELETED; ADR-0106 superseded-in-place; blob compat per the 0h table.**
  The-deletion-is-the-proof (`feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders`).
  **SCOPE — ⛔ CORRECTED 2026-08-11 (advisor; the earlier "reading B — from_ceir_frame retreats to a test instrument" framing
  is STRUCK):** 15f is NOT a direct `ceir.frame → FrameGraphTemplate` re-lowering — `build_frame_graph_template` is 796 lines
  of per-executor mapping; a direct version would DUPLICATE it (the very duplicate §126 exists to kill). Reading **A-refined**:
  0h-F2 verbatim = "no path executes without going through canonical CEIR" — that kills the flag-off BYPASS, not the desc. The
  desc is a BLESSED lowering intermediate (the 15d bullet: "FrameGraphTemplate/CompiledFrameGraph BECOME the lowering/plan
  layer"); `from_ceir_frame` STAYS as the ceir→desc lowering stage. The scene_renderer C++ render path (0h E1–E5) is a SEPARATE
  track (RAF-12), NOT 15f. **The real content of 15f (advisor finding):** `route_frame_through_ceir` ran `to_ceir → from_ceir →
  validate_FRAME_GRAPH` — it NEVER called validate_CEIR_frame, so ALL of 15c was TEST-ONLY. "One architecture" = the CEIR
  verifier is the LIVE gate. **Decomposition (each cut separately gated):** 15f-0 pre-gates — **✅ (i) flag-ON sandbox smoke
  GREEN** (app boots+renders through CEIR, 182 frames, exit 0); (ii) RAF-12 coverage inventory. **✅ 15f-1 DONE + gated
  2026-08-11:** flipped `route_frame_through_ceir` to DEFAULT-ON (`CRD_FRAME_VIA_CEIR=0` opts out) — CEIR is the live default
  path; scene 71/71 default-on + 71/71 `=0`, sandbox smoke default-on, 4-config. **✅ 15f-2 DONE + gated 2026-08-11:** wired
  `validate_ceir_frame` (the full §115 verifier) into the route as the LIVE gate; removed the standalone pre-route + post-round-
  trip `validate_frame_graph` (it survives as flatten's composition-time check + the `=0` gate + the differential-oracle
  reference). Error surface: `FrameSemanticDiag.contract` for contract/vocab/cycle rows, else ParseFailed + `frame_semantic_kind_name`.
  New negative test (`test_scene_render.cpp` "REN-41/CEIR-15f-2"): a no-`@output` frame is rejected VIA validate_ceir_frame
  (to_ceir accepts it; the control `@output`-frame cooks); scene 72/72 + asan + linux 25/25 + tidy. **✅ 15f-3 DONE + gated
  2026-08-11:** DELETED the `=0` bypass + the `CRD_FRAME_VIA_CEIR` flag/getenv/pragma from `route_frame_through_ceir` — the CEIR
  route is now UNCONDITIONAL (§121: no path executes without going through canonical CEIR). `validate_frame_graph` stays OFF the
  live path (flatten's composition-time check + the differential-oracle reference). scene 72/72 + sandbox smoke exit 0 +
  win-asan 5/73 + linux 25/25 + tidy. The L331 tension DISSOLVED under A (validate_ceir_frame materializes via from_ceir_frame
  BY DESIGN — nothing deleted). **✅ 15f-4 DONE 2026-08-11:** read ADR-0106 verbatim — it does NOT mandate desc elimination;
  Decision #2 BLESSES `FrameGraphDesc` as legitimate Desc-vs-Runtime STAGING (contingency resolved, reading A-refined
  confirmed). Amended ADR-0106 Decision #2 IN-PLACE (canonical description = `ceir.frame`; desc = staging; runtime + registry
  + `build_frame_graph_template` load bridge PRESERVED; reading B rejected in the ADR — 796-line bridge). Blob-compat verified:
  the `.crdr` cooker (`cook_frame_graph`, asset_cooker) serializes the AUTHORED desc (NO flatten), so the 15e anchor-clear
  doesn't touch the blob — no format change, stays v8. Docs/verification only, no code change. **15f COMPLETE (15f-0…15f-4).**
- **15z — GATE:** every shipped frame asset runs through CEIR pixel-identically BOTH backends; the old path is gone;
  `crd-sandbox --smoke-test` green, validation on.

## Invariants preserved (the band contract)

`frame-cook ⊥ crd-scene` via the `IFrameGraphHost` seam (host resolves ECS→pre-resolved `DrawItem`; CEIR never sees a
scene type). Asset IDs preserved. The FrameGraph is NOT deprecated syntax — its semantics (topology/lifetime/history)
BECOME a CEIR dialect (§159). Per-asset pixel parity with a deterministic clock is the gate before any old path dies.

⛔ **PIPELINE ORDER (contract): `parse → flatten_frame_graph → to_ceir_frame`.** REN-37.6 composition (includes/anchors/
injects) is a §39 NAMED-FORWARD subgraph capability RESOLVED into plain passes by `flatten_frame_graph` BEFORE conversion —
the §115 table routes composition to PARSER ("the converter sees a FLATTENED graph"), so the 15d analyses (hazards/cycle/
lifetimes) never need composition special-cases (the whole point of REN-37.6). `to_ceir_frame` therefore REJECTS (nullptr) a
desc still carrying composition — the PERMANENT contract, a caller that skipped the flatten. ⚠️ **15e trap:** whoever wires the
runtime must FLATTEN before converting, never feed an authored (composed) desc into `to_ceir_frame`. (Contrast `for_each`,
which round-trips as an authored attr because its expansion needs RUNTIME counts — runtime-deferred ≠ frontend-resolvable.)

## 15c — the validation-layer classification (§115): every `FrameCookError`, routed

**Status: 15c-0 DONE (the `frame.history` routing fix + the ping-pong round-trip lock, 2026-08-11); this table is the
15c-1 driver.** §115's taxonomy (Syntax/structure · Type · Ownership/lifetime · Resource/effects · Domain · Capability ·
Kernel-contract · RT · Distributed) says WHERE each check belongs; below, every `FrameCookError`
(`frame_asset.hpp:504`) is routed to ONE of five destinations, so 15c-1 knows what to BUILD, what the CEIR
representation already makes IMPOSSIBLE, and what the CEIR-12/13 analysis DERIVES for free:

- **PARSER** — caught before a `ceir.frame` exists: the `.frame.toml`/CEIR-text parse, or the REN-37.6 flatten pass
  (composition is a §39 named-forward FRONTEND — the converter sees a FLATTENED graph). No 15c verifier.
- **STRUCTURAL-IMPOSSIBLE** — the SSA-operand representation cannot express the error. The proof is the representation
  itself (the §159 headline realized). No verifier.
- **CEIR-ANALYSIS (15d)** — bands 12/13 (`resource_root` + `ops_hazard` + def-use) DERIVE it. A 15c differential-oracle
  test asserts `validate_frame_graph(from_ceir_frame(m))` agrees; the real check lands at 15d lowering.
- **FRAME-VERIFIER** — a graph-semantic check needing NO executor knowledge: either `Context::find_frame_misuse`
  (crd-ceir CORE, per-op, executor-agnostic — the 15a home) or a new `validate_ceir_frame` (crd-frame-cook, cross-op
  sweep + the @output-endpoint convention). Split noted per row.
- **PROGRAM-CONTRACT** — needs the EXECUTOR REGISTRY (a per-executor operand/attachment/param contract, §115
  Kernel-contract + RT layers). Lives in `validate_ceir_frame` (crd-frame-cook, executor-aware) — NOT core.

| Destination | `FrameCookError`s | §115 layer | Note |
|---|---|---|---|
| **PARSER** | `ParseFailed` · `BadSchema` · `MissingName` · `UnknownPassKind` · `IndexWithoutForEach` · `SubscriptOnNonLayered` · `IncludeMissingName` · `DuplicateInclude` · `UnknownAnchor` · `InjectUnknownPass` · `AnchorUnknownPass` · `UnresolvedInclude` · `IncludeCycle` | Syntax/structure | toml/CEIR-text lex + the flatten frontend; `$index` is toml sugar, expanded pre-converter. Composition = named-forward. |
| **STRUCTURAL-IMPOSSIBLE** | `UnknownResource` | Ownership | ⭐ a read/write is an OPERAND (an SSA edge to a defining op) — there is no dangling name to leave undeclared. The error class is DELETED by the representation. (Caveat: an operand defined outside the graph — NEW-IN-CEIR §6 below.) |
| **CEIR-ANALYSIS** — ✅ 15c-1c-2 (def-use) | `ResourceNeverWritten` (a transient declare with no `w`-token use — def-use) · `PingPongNeedsBothWays` (a `lifetime=history` declare whose uses are read-only or write-only — def-use over `frame.history` vs direct write) · `DependencyCycle` (a cycle in the derived access-token hazard DAG) | Resource/effects | ✅ `ResourceNeverWritten` + `PingPongNeedsBothWays` implemented in `validate_ceir_frame` (one graph walk builds per-declare read/wrote flags, chasing `frame.history` to the declare; order mirrors `validate_frame_graph`: PingPong→NoOutput→NeverWritten; both oracle-agree). `DependencyCycle` genuinely needs the 15d derived hazard graph ⇒ **15d**, with a 15c differential-oracle test meanwhile. |
| **FRAME-VERIFIER · find_frame_misuse (core, per-op)** | `LayersOutOfRange` · `BadMipCount` · `CubeNeedsSquare` · `VolumeNeedsDepth` · `UnknownDimension` · `BadResourceSize` · `PersistentNeedsSize` · `StructuredNeedsStride` · `StrideNotAligned` · `AccelIsExternal` · `ExternalTextureIsReadOnly` · (`UnknownCull`/`UnknownSort`/`UnknownForEach`/`UnknownQueue` ✅ DONE 15a) | Resource/Domain | resource SHAPE on the `declare`'s frame-scoped attrs — NO executor knowledge, executor-agnostic ⇒ core-legal (the 15a "frame-scoped attrs validated when the declare sits in a frame.graph region" contract; NOT YET coded — 15c-1). `ExternalTextureIsReadOnly` reads the access token. |
| **FRAME-VERIFIER · validate_ceir_frame (frame-cook, cross-op)** — ✅ 15c-1c-1 | `NoOutputPass` (some pass must write the `@output` import) · `DuplicateName` (a per-category name-uniqueness sweep over declare+import/draw_list/pass name attrs, points at the 2nd collider) | Ownership/Domain | ✅ implemented as `FrameSemanticDiag{op, FrameSemanticKind}` (CEIR-native pointing, NOT FrameCookError/core-FrameMisuseKind). Differential oracle = the desc-side `emit→parse→validate` cook pipeline (DuplicateName is parse-time, NoOutputPass is validate-time — a single `validate_frame_graph` call would miss the former). |
| **PROGRAM-CONTRACT · validate_ceir_frame (executor-aware)** | `MissingShader` · `MissingDrawList` · `AmplifyNeedsCount` · `RayTraceNeedsAccel` · `IndirectNeedsArgs` · `IndirectArgsNotArgs` · `VisbufferNeedsUintTarget` · `CompositeNeedsBlend` · `RtPipelineNeedsThree` · `PresentNeedsOneRead` · `PresentWritesNothing` · `PresentSourceInternal` · `TransferNeedsOneRead` · `TransferNeedsOneWrite` · `ClearReadsNothing` · `AsyncQueueNeedsCompute` · `LoadNeedsGeometry` · `UnknownMaterialPass` · the executor-param vocabs (`UnknownFormat`/`UnknownCompare`/`UnknownBlend`/`UnknownFilter`/`UnknownSamplerFilter`/`UnknownSamplerAddress`/`UnknownShadingRate`/`UnknownRateCombiner`/`UnknownConservative`/`UnknownFaceCull`/`UnknownFrontFace`/`UnknownStencilOp`/`BadStencilValue`) | Kernel-contract · RT · Capability | each is a PER-EXECUTOR contract (a raytrace pass needs an accel operand; a present pass reads exactly one; a visbuffer writes R32Uint) — the executor schema is the source, consulted from the registry (Fork D's CDEP generalization lands here too). |

### NEW-IN-CEIR — expressible in `ceir.frame`, inexpressible in `FrameGraphDesc` (⛔ `from_ceir_frame` silently mangles today)

A round-trip-identity gate proves the FORWARD converter lossless, but `ceir.frame` is a STRICT SUPERSET of the desc:
a hand-authored (or a future-pass-produced) module can hold shapes the desc cannot name, and `from_ceir_frame` drops
them WITHOUT ERROR. Each needs a verifier — these are the checks with NO `FrameCookError` precedent, the real new
surface. **✅ 15c-1a (2026-08-11) implemented §1, §2, §3, §7 as `find_frame_misuse` kinds (all executor-agnostic, so
they fit crd-ceir CORE): `OperandOutsideGraph`, `MultipleGraphs`, `HistoryReadNotThroughFrameHistory`,
`HistoryWriteThroughHistory`. ✅ 15c-1b implemented §4 (the backward `rw` decode).** §6 (and the §8/§9 park items) ride 15c-1c (`validate_ceir_frame`):

1. **A pass/`frame.history` operand defined OUTSIDE its `frame.graph` region** (a value from the enclosing func or a
   sibling region). `from_ceir_frame` reads the operand's `name` attr — absent or foreign ⇒ a garbage/empty
   `FrameResourceRef`, silently. ⭐ VERIFIER: every `frame.pass`/`frame.history` operand's defining op must live in the
   SAME `frame.graph` region. (The advisor's exemplar of the class.)
2. **More than one `frame.graph` per module.** The desc is ONE graph; `find_graph` takes the FIRST and drops the rest.
   VERIFIER: exactly one `frame.graph` per module THIS slice (multi-graph = the subgraph §39 named-forward).
3. **A plain read of a `lifetime=history` declare (not through `frame.history`).** The forward ALWAYS routes through
   `frame.history` (15c-0); a hand-authored direct read reintroduces the false intra-frame RAW at 15d. ⛔ VERIFIER: a
   `lifetime=history` resource is READ only via `frame.history` (a write targets it directly). This is the STRUCTURAL
   guard that turns Fork-B B1 from a converter CONVENTION into an ENFORCED invariant — without it, B1 holds only for
   converter-produced modules.
4. **An `access` token `rw`** (✅ 15c-1b — `from_ceir_frame` structural decode). `ceir_parse_access` ACCEPTS `{r,w,rw}`
   and `find_frame_misuse` passes it, but the forward never EMITS `rw` (a read-modify-write resource rides two operands,
   `w`+`r`). ⛔ The OLD `from_ceir_frame` token scanner wrote each non-comma char to `tk[nt]` (last char WINS), so `rw`
   collapsed to `'w'` and the operand became a WRITE-ONLY ref — the READ half was silently DROPPED. FIXED: the backward
   converter decodes each token STRUCTURALLY (first char `r`⟺read, last `w`⟺write) and maps an `rw` operand to BOTH a
   read and a write ref (the desc expresses RMW as reads∋X + writes∋X; the forward re-canonicalizes to two operands),
   locked by the `#3951` differential-oracle round-trip test.
5. **A `frame.pass` executor symbol no registry entry backs** (`UnknownPassKind`'s CEIR edition). VERIFIER
   (program-contract): the `executor` symbol resolves to a registered `ExecutorTypeId` or a declared app-custom id.
6. **`frame_kind` ⟺ `lifetime` desync on a graph declare** (e.g. `frame_kind=PersistentImage` under
   `lifetime=transient`). The converter keeps them consistent; a desynced module makes `emit_frame_toml` (trusts
   `frame_kind`) and the CEIR-12 planner (trusts `lifetime`) diverge. VERIFIER: the two attrs must agree
   (`kind_lifetime(frame_kind) == lifetime`).
7. **A WRITE through a `frame.history` result** (✅ 15c-1a — `HistoryWriteThroughHistory`). The inverse of §3: a
   `frame.pass` operand DEFINED BY `frame.history` with a `w`/`rw` token = "write the previous frame" — semantically
   nonsense, resource-typed so every prior check passes, and `from_ceir_frame`'s 15c-0 def-redirect would silently
   rewrite it into a CURRENT-frame write. Guarded pass-side (last char `w` ⟺ write component + operand def kind
   `frame.history`), keeping the table's "every expressible mangle has a guard" claim honest.
8. **A `frame.history`/`frame.draw_list` OUTSIDE any `frame.graph`** (→ 15c-1c). Only `frame.pass` has an outside-graph
   kind (`PassOutsideGraph`); a `frame.history`/`frame.draw_list` in the func body is unflagged (guard 1 checks the
   operand's region relative to the op's own, which agree when both sit in the func body). Pre-existing 15a scope, not a
   15c-1a regression — route the outside-graph check for the other frame ops to a CORE `find_frame_misuse` kind (structural,
   executor-agnostic, mirroring `PassOutsideGraph`): name them **`HistoryOutsideGraph`** / **`DrawListOutsideGraph`**. §9's
   draw-list access-token check joins the same core tick. (§6 + `UnknownDimension` ✅ 15c-1c-2, frame-cook — they need the
   `FrameResourceKind`/`FgImageKind` enums.)
9. **An access token on a `frame.draw_list` operand** (→ 15c-1c). A `w`/`rw` token on a draw-list operand is
   vocabulary-clean but semantically meaningless (a draw list is queried, not written); the backward converter IGNORES
   the token (the draw-list branch fires before the read/write split), silently normalizing it. Route an
   access-token-sensibility-per-type-class check to `validate_ceir_frame`. **15c-1b ✅ handled §4** — the backward
   converter now decodes `rw` structurally (first char `r`⟺read, last `w`⟺write) and maps it to both lists.

**15c-1 build order:** the NEW-IN-CEIR structural guards (§1–4, they protect every downstream band) → the
`find_frame_misuse` resource-shape rows (core, executor-agnostic) → `validate_ceir_frame` (frame-verifier rows +
program-contract rows, executor-aware) → the CEIR-ANALYSIS differential oracle. `validate_frame_graph(from_ceir_frame(m))`
is a TRANSITIONAL oracle in tests only — the real checks are CEIR-native, POINTING diagnostics (the §115 "each a pointing
diagnostic" contract).

## Fork C-2 — the param-bag WIRE FORMAT (15c-1c-2 · task #21 2c) — DESIGN-LOCKED 2026-08-11 (advisor)

⛔ **The converter is LOSSY for the entire RAF-12.3 typed param bag** — `to_ceir_frame` carries only shader/kernel/
technique/draw_list; `emit_frame_toml` serializes the WHOLE bag (view · raygen/miss/closest_hit/any_hit/intersection/
callable · blend[] · shading_rate/rate_combiner/conservative · clear_color/clear_depth · depth_compare · sampler · the
pass-state stencil/face_cull/front_face/depth_bias vocab · material_pass · load/load_depth · and the AUTHORED
`[pass.params]`). The round-trip-identity gate false-greened because `build_scene`/`build_taa` set none. **This BLOCKS
15c-1d** (the program-contract verifiers read `composite`/`load`/`blend`/`count`/`material_pass`/RT-programs) **AND
15d/15e runtime fidelity** — so 2c lands BEFORE 15c-1d. (15c-1d's ONLY param-independent row is `AsyncQueueNeedsCompute`
— queue+executor, both carried; do NOT cherry-pick it, keep 15c-1d one coherent layer after 2c.)

**The accessors are TYPE-BLIND** (`pass_u32`/`pass_f32`/`pass_flag` all read `v[0]`; `pass_str` reads `str`; `pass_has`
= presence) and `emit_frame_toml` picks the accessor by param NAME — so folded config params need only the right FIELD.
But the AUTHORED `[pass.params]` block emits BY TYPE (Vec4→array, Bool→true/false, else→`app_f64(v[0])`), and
`cook_frame_graph` serializes `type` as a BYTE — so for exact fidelity on BOTH the TOML and the blob path, round-trip
the exact `FrameParamType`. ⛔ `v[4]` is `double[4]` and `app_f64` emits the f64 — the value channel MUST be
`attr_float` (which stores the f64 bit pattern, the `attr_f` memcpy), never a float32 detour.

**Wire format** — each param `<name>` in `frame.pass.params` becomes attrs on the `frame.pass` op, `:`-prefixed (cannot
collide with a structural attr; `is_folded_pass_param` never sees them):

| FrameParamType | value attr(s) | type attr |
|---|---|---|
| String | `p:<name>` = **string** (the `str` field) | `pt:<name>` = `(int)String` |
| Bool | `p:<name>` = **bool** (`v[0]!=0`) | `pt:<name>` = `(int)Bool` |
| Float / Int / Enum / U32 | `p:<name>` = **float** (`v[0]`, f64 bit pattern) | `pt:<name>` = `(int)type` (the ONLY discriminator among the v[0] family — accessors are type-blind, so this exists solely for blob-byte fidelity) |
| Vec4 | `p:<name>:0..3` = **four float** attrs (`v[0..3]`) | `pt:<name>` = `(int)Vec4` |

Carry `pt:<name>` for EVERY param (robust: the backward reads `pt` → the exact type, then `p:<name>` in the matching
kind — no fragile inference). The `pt:` attrs are INTERNAL (from_ceir reads them; they never reach emit_frame_toml).

**Forward:** iterate `p.params` and emit each as `p:`/`pt:` — INSTEAD OF (not in addition to) the shader/kernel/
technique special-casing. ⛔ KEEP the three top-level symbol attrs (load-bearing: the toml docs name them, CDEP
extraction is specced against them) and SKIP those three names in the generic loop, or the round-trip double-emits.
**Backward:** enumerate the op's attrs (`num_attrs()`/`attr_name(i)`), collect the `pt:<name>` set, and for each
reconstruct via `set_pass_*` (or push a FrameParam directly). ⛔ Iterate-WHAT-EXISTS — do NOT add a `!= 0` filter on a
Bool param (a param exists in the bag iff it was set; `set_pass_flag` adds only-if-true, so a present Bool is faithful).

**Fixture** (the full-surface input that would have caught this at 15a): one pass carrying EVERY type — a String
(shader), a Bool (`load`? verify it survives), a Float (clear_depth), a U32/Enum (blend), a Vec4 (clear_color), and one
authored `[pass.params]` entry — then assert `emit_frame_toml` round-trip identity + a direct param-set check.

## 15c-1d — the PROGRAM-CONTRACT layer (architecture DESIGN-LOCKED 2026-08-11, advisor)

The per-executor contract checks (~18: MissingShader/MissingDrawList/LoadNeedsGeometry/VisbufferNeedsUintTarget/
CompositeNeedsBlend/RtPipelineNeedsThree/RayTraceNeedsAccel/IndirectNeedsArgs/IndirectArgsNotArgs/AmplifyNeedsCount/
Transfer*/ClearReadsNothing/Present*/AsyncQueueNeedsCompute/UnknownResource/SubscriptOnNonLayered/IndexWithoutForEach)
live in `validate_frame_graph`'s pass loop (`frame_asset.cpp` ~1163–1487) — ~324 lines, six REN-38 revisions. All of them
now have their inputs in the ceir.frame (2c carried the param bag; `pass_is_*` derive from `executor_id` which round-trips
via the executor symbol). ⭐ **RESOLVED: Option B — extract ONE shared `pass_contract_diag`, called by BOTH validators**
(A duplicates the churniest validation → guaranteed desync; C drags parse+resource-loops in per call + non-pointing; D
makes program-contract a permanent §115 exception and blocks the 15f deletion of `validate_frame_graph`).

**Signature:** `FrameCookError pass_contract_diag(const FramePassDesc& p, ConstSpan<FrameResourceDesc> resources, String* where)`
in `frame_asset.cpp` — PURE per-pass verdict. Several checks consult the resource table (PresentSourceInternal / Visbuffer
target format / Indirect args kind / RtAccel), so it takes the resource span and defines `find_resource`/`is_sentinel`/
`is_output` internally. ⛔ `wrote_output` accumulation is NOT per-pass — it STAYS in `validate_frame_graph`'s loop (inline
the `@output`-write check there).

**Steps (refactor-then-extend, never both in one diff):**
- **15c-1d-0 = PURE REFACTOR** — extract `pass_contract_diag` from the pass-loop body; `validate_frame_graph`'s loop
  becomes `pe = pass_contract_diag(p, resources, where); if (pe) return pe;` + the `@output` accumulation. ZERO behavior
  change, gated by the full desc suite (`test_frame_asset.cpp`, ~144 frame tests). Land ALONE.
- **15c-1d-1 = the CEIR wiring** — (a) extract `from_ceir_frame`'s per-pass reconstruction into a file-local
  `rebuild_pass(ctx, op, alloc, FramePassDesc&)` used by both from_ceir_frame AND validate_ceir_frame (do NOT copy it);
  (b) ⛔ **change the signature to `validate_ceir_frame(ctx, m, alloc)`** — materializing FramePassDescs needs an allocator;
  the only breaking change, all call sites are the tests, do it in this same tick; (c) for each frame.pass, `rebuild_pass`
  + build the resource list + call `pass_contract_diag`, map its FrameCookError → `{op, FrameSemanticKind::X}` via a
  small table (that table INVERTED is the oracle: each test asserts both sides return the same row). Wire family 1
  (MissingDrawList/MissingShader/LoadNeedsGeometry) with oracle tests. Families 2–5 then land ~one tick each (mostly
  test-writing — the helper already checks everything); AsyncQueueNeedsCompute/CompositeNeedsBlend/UnknownMaterialPass
  come FREE with their families.
- **15c-1d-6 (NEW-IN-CEIR, no oracle)** — the param closed-vocabs (UnknownSamplerFilter/Address/FaceCull/FrontFace/
  StencilOp/ShadingRate/RateCombiner/Conservative/Filter, BadStencilValue): `validate_frame_graph` does NOT range-check
  enum params (they're parse-time `to_*` rejects), so a garbage enum int NORMALIZES through the ceir round-trip (the
  UnknownDimension shape) → injection tests, not oracle tests. A vocab-range check in `pass_contract_diag` if the desc
  gains one; else a dedicated NEW-IN-CEIR group.

## What CEIR-15 does NOT do

It does not migrate the EXECUTORS (scene.raster orchestration etc. → CEIR programs) — that is **CEIR-16** (§127/§128,
the `scene.raster`-as-ordinary-CEIR proof), where the RAF-14z pause finally lifts. 15 unifies the GRAPH (topology);
16 unifies the MECHANICS (executors).

## 15d — the hazard-derivation slice

**⭐⭐ 15d-1 ✅ DONE + gated 2026-08-11** (win-debug full crd-ceir 432 cases + win-asan + linux + tidy; the CONSUMER-RELINK
regression proof: the full crd-ceir suite is byte-identical, so the op-name-scoped narrowing left every non-frame op alone).
Resolution: **Fork H-A, scoped to frame.pass by op NAME**, at the access layer (`op_access_count`/`op_access_at` in
context.cpp) — NOT editing the declared effects (they still feed the 4c domain-legality + effect-safety layers). frame.pass
contributes ONE Memory access per operand, tokened by `access` (structural decode); a draw-list operand is inert; **GPUCommand
is SUPPRESSED by omission** (the frame.graph CONTAINER keeps the ambient `[GPUCommand, MemoryReadWrite]` for external
ordering — primary-source: "the passes inside carry their own precise effects"). `resource_root` runs, so a `frame.history`
operand is its own root. Gated by four tests: build_scene → one precise RAW geometry→forward (was WAW); **memory-disjoint
passes sharing only a draw list → ZERO hazards (the GPUCommand-suppression + draw-list-skip proof, NOW isolatable)**;
build_taa → the scene RAW with a history resource present; and **Fork B at the hazard level, isolated inter-op: a writer of
`history` and a separate reader (routed through frame.history) derive NO hazard** — the false-intra-frame-RAW is fixed
STRUCTURALLY, proven at the hazard level for the first time. Remaining: 15d-2..5 (below). The SCOUT record follows.

### Scout record (2026-08-11)

**The discriminating experiment (ran, definitive).** `collect_block_hazards` on build_scene's `frame.graph` block returns
ONE hazard (geometry↔forward) of kind **WAW**, not RAW. geometry writes `scene`, forward reads it — a TRUE RAW — but both
passes carry **ambient `MemoryReadWrite`** (`frame.pass` effects `[GPUCommand, MemoryReadWrite]`, the op def says verbatim
"the 15d lowering narrows per-operand from `access`"), so each reads+writes `Universe` and the strongest pair is WAW.
Locked as the characterization test `ceir 15d PRE: frame.pass hazards are the ambient all-pairs baseline` (flips WAW→RAW
when 15d-1 lands). So the §159 headline ("the frame graph's barrier pass becomes a CEIR analysis pass") is NOT yet real:
the analysis runs but derives the over-conservative baseline.

**Recon result: NO op narrows at the `ops_hazard` level today.** `op_access_at`/`gather_accesses` resolve only STATICALLY
declared effects (`{family, operand=i}`, e.g. dispatch_indirect's `MemoryRead` on operand 0) — there is no `access`-STRING-
driven per-operand path, and a variadic access-tokened tail cannot be expressed as static effect declarations. `compute.
dispatch` is ALSO ambient at this level BY DESIGN — its toml calls the ambient a "CONSERVATIVE BASELINE … a dispatch may
touch any bound memory, so it MUST hazard against exports/transfers of its own output (the more-hazards-never-fewer rule)";
its narrowing is CEIR-13d (the crd-ceir-gpu **lowering/bridge**), a DIFFERENT layer than `ops_hazard`.

**⛔ THE 15d-1 DESIGN FORK (needs resolution before coding — a load-bearing core invariant is in play):**
- **Fork H-A — make the core effect model `access`-aware.** Extend `op_access_count`/`op_access_at`/`gather_accesses` so an
  op carrying an `access` attr + a resource tail contributes PER-OPERAND read/write accesses (target = operand Value; let
  ops_hazard chase view→root on demand — do NOT pre-chase, that re-implements 12c). Cleanest, data-driven, unifies
  frame.pass + compute.dispatch + render.draw, and is the literal §159 realization. ⚠️ **but it changes the hazard verdict
  for EVERY access-tokened op**, including compute.dispatch — whose conservative baseline was a DELIBERATE safety choice
  (a narrowed dispatch might no longer hazard against the export of its output). Narrowing must PRESERVE "more-hazards-
  never-fewer" for those, or the change is a silent correctness regression in the compute path. Scope carefully: narrow
  ONLY frame.pass first (gated by op kind), or prove the narrowing is safe for dispatch before touching it.
- **Fork H-B — a frame-cook-LOCAL hazard pass.** Derive frame barriers in crd-frame-cook by walking the `frame.graph`
  block, reading `access` + `resource_root(operand i)` per pass, and pairing with `accesses_conflict`. No core change, no
  conservative-baseline risk — but RE-implements the pairing the core already has, working AGAINST the "becomes a CEIR
  analysis pass" headline. Acceptable only if H-A's dispatch-safety proof is genuinely hard.
  → **Recommendation pending advisor/user: H-A scoped to frame.pass by op kind** (data-driven but gated), preserving
  dispatch's baseline untouched. Resolve before 15d-1 coding.

**⛔⛔ DEEPER FINDING (2026-08-11, from the effect model — the plan above is INCOMPLETE):** `frame.pass` declares TWO
effects, `[GPUCommand, MemoryReadWrite]`, and `effect_access(GPUCommand) = {write, ResourceClass::Gpu}` resolved as a
WHOLE-CLASS (ambient, resource=nullptr) write. Two whole-class Gpu writes ALWAYS conflict (`accesses_conflict`: same class,
a nullptr resource = whole class, full range) → **WAW between EVERY pair of frame.pass ops, regardless of memory sharing**.
So narrowing `MemoryReadWrite` per-operand is NOT ENOUGH — the `GPUCommand` write re-introduces the all-pairs baseline by
itself (this is why the char-test's WAW can't be attributed to memory alone). 15d-1 MUST also handle `GPUCommand`: either
DROP it for a narrowed frame.pass (safe iff the `access` tail is a COMPLETE declaration of what the pass touches — which the
frame.pass contract guarantees, UNLIKE compute.dispatch's "may touch any bound memory", the exact reason dispatch keeps its
conservative baseline). ✅ RESOLVED at 15d-1: DROP it (the frame.graph container keeps it for external ordering). ✅ NOW
EMPIRICALLY ISOLATED: the "memory-disjoint passes sharing only a draw list → ZERO hazards" test — impossible under the
ambient baseline (GPUCommand whole-class Gpu write + whole-class Memory both WAW'd), so a green result there is the direct
proof that GPUCommand is suppressed AND the memory narrowing is per-operand. The elevated 15d-1 design ("suppress GPUCommand
+ narrow MemoryReadWrite per-operand for frame.pass") landed as such — a genuine effect-semantics decision, not a mechanical add.

**Decomposition (advisor 2026-08-11; non-for_each fixtures so expansion doesn't block the front):**
- **15d-1** — effect narrowing (Fork H above) + the hazard-derivation gate. Headline tests (fixtures exist): build_scene →
  EXACTLY one RAW, geometry→forward, root=`scene`, NO hazard involving the draw list; **build_taa → `read(frame.history
  (history))` vs `write(history)` derives NO hazard while `scene` still RAWs — the Fork B payoff proven at the HAZARD level
  for the first time** (B1's structural no-false-RAW has only been shown at the converter level).
- **15d-2** — `DependencyCycle`. ✅ SCHEDULING FORK RESOLVED (2026-08-11): the old path DOES topo-sort — `validate_frame_graph`
  builds a dependency graph (frame_asset.cpp) with **REN-41 authored-order-aware** WAR/RAW disambiguation (a later writer is a
  legit WAR only if the read is SATISFIABLE — a frame-start value [external/persistent/ping-pong] or an earlier producer;
  otherwise it's a forward-RAW, the cycle-surfacing case) + WAW by declaration order, then runs **Kahn** (`visited != np` ⇒
  cycle). CEIR must PRESERVE this. Gold-standard realization = the 15c-1d **extract-and-share** discipline (ONE source, parity
  by construction — NOT a native re-implementation that would re-derive REN-41 and risk divergence; and NOT collect_block_hazards,
  which is list-order and can't reveal a backward-edge cycle). **15d-2a ✅ DONE + gated 2026-08-11 (win-debug/linux 1099
  assertions/87 cases + win-asan + tidy):** extracted `dependency_cycle_diag(const FrameGraphDesc&)` from validate_frame_graph
  (verified script; removed the now-dead `alloc` local; header-declared), PURE refactor, landed ALONE. **15d-2b ✅ DONE + gated 2026-08-11 (67/67 win-debug/asan + linux + tidy) → 15d-2 CLOSED:** validate_ceir_frame calls the
  shared `dependency_cycle_diag` on the materialized desc, LAST (matching its END position in validate_frame_graph so the
  oracle agrees on which error wins), returning `{nullptr, FrameSemanticKind::DependencyCycle, FrameCookError::DependencyCycle}`
  (graph-level, no single offending op — like NoOutputPass). Oracle test: passA reads y+writes x+@output, passB reads x+writes
  y (single-frame) → DependencyCycle == cook_verdict; find_frame_misuse clean (a cycle is SEMANTIC, not structural). CONTROL:
  build_taa does NOT false-cycle — the prev-frame `history` read is a satisfiable frame-start value (PingPong), so the ping-
  pong feedback never surfaces as a cycle.
- **15d-3** — lifetimes/aliasing. **15d-3a ✅ DONE + gated 2026-08-11 (65/65 win-debug/asan + linux + tidy):**
  `compute_block_lifetimes` runs on the frame.graph block FOR FREE via 15d-1 — `op_has_ambient_mem_or_universe` routes
  through `op_access_at`, so a narrowed frame.pass is not flagged ambient and each transient's `last` is its ACTUAL last use.
  Validated: build_scene → `scene` has a precise transient range (@output excluded as an import); two memory-disjoint
  transients keep DISTINCT last-uses (`a.last < b.last`) where the ambient baseline would tie them at the final pass.
  **✅ 15d-3b DONE + gated 2026-08-11 (4-config; decided WITH the advisor, NOT a user-blocked memo).** THE FORK RESOLVED:
  **core-planner**, not converter-interleave. `compute_block_lifetimes` now makes `ResourceLifetime` the MEMORY-LIVE range
  `[first-use, last-use]` — `first` is pulled to the first ACCESS in Pass 2 (was the DECLARE position; since the converter
  emits every `resource.declare` UP-FRONT, that tied every transient to ~0 → all overlapped → `plan_block_memory` pooled
  NOTHING). This is parity with the render-graph aliaser's `first_use` model (frame_graph.cpp L1200-1225 — verified same
  `(kind,size_class)` keys + `last<first` condition; the "borrowed-bundle format" folds into `size_class`). ⛔ CORRECTNESS
  (advisor-caught): the naive first-use change opened a slot-sharing CLOBBER — a not-yet-first-used resource could pool into
  a slot an ambient MemoryWrite op clobbers under a live tenant. Fixed by a SYMMETRIC ambient extension keyed on a LOCAL
  `declare_pos` array (NOT a `ResourceLifetime` field — that layout change = the stale-.obj scar): at an ambient op, every
  resource ALLOCATED at-or-before it spans it in BOTH directions. Elegantly, converted frames have ZERO ambient ops (the
  15d-1 narrowing), so full first-use pooling precision holds exactly where it matters — 15d-1 is what makes 15d-3b safe.
  Gate: crd-ceir 433/433 (win-debug/asan + linux-gcc) + crd-frame-cook 92/92 consumer-relink + tidy `context.cpp` clean; the
  band-12/lifetime/planner/compute deltas each individually justified (more pooling; the co-slot⇒aliasable invariant holds);
  new `test_planner.cpp` "ceir 15d-3b" (declared-early/used-late pools; unused-degenerate) + `test_lifetime.cpp` (3) pins the
  ambient clobber-guard. NOTE (15d-4): the `op_access_at` frame.pass `mask=0` (whole-range) hardcode is still correct for
  every operand today; the per-layer view `range_mask` refinement arrives with for_each VIEW operands at the direct lowering.
- **15d-4** — `for_each` expansion (build-time; StereoViews→2, CubeFaces→6, …). Each `[$index]` read/write of a LAYERED
  resource expands to a per-slice operand — a `resource.view(R, layer=i)`. ⚠️ **15d-1 narrowing dependency (found 2026-08-11):**
  `op_access_at`'s frame.pass branch HARDCODES `mask = 0` (whole range), which is correct for every operand that exists TODAY
  (declares/imports/history = whole resource) but would make layer-i and layer-j views CONFLICT (over-conservative hazards +
  lifetimes) once for_each creates view operands. Fix WITH 15d-4: read the operand VIEW's `range_mask` (as the non-frame
  `op_access_at` path already does via `e.range_mask`, L1509) instead of the hardcoded 0, so per-layer slices don't
  over-conflict. Not a current bug (no view operands in frame.pass yet); an efficiency gap that arrives with for_each.
  ⛔ **RUNTIME-ENTANGLED (confirmed 2026-08-11, frame_runtime.cpp L756):** the expansion COUNT is `host.for_each_count(gen,
  arg)` — HOST-provided at RUNTIME for EVERY generator (not just ShadowCastingLights). So for_each cannot expand device-free
  at cook/ceir time; it is a runtime-lowering step (part of 15d-5), not a standalone CEIR pass.
  **15d-5** — `CompiledFrameGraph` (ADR-0106) is the lowering/plan layer; Gate-7 one-submission preserved. **✅ RUNTIME-
  FIDELITY GATE LANDED 2026-08-11 (4-config, test_frame_template_bridge.cpp):** the CEIR path executes via Fork E
  (`desc → to_ceir_frame → from_ceir_frame → desc' → build_frame_graph_template`), and a forward_csm graph (for_each cascades,
  INDEXED layered-atlas write, external buffers, sampled depth, present) lowers to a BYTE-EQUIVALENT `FrameGraphTemplate` —
  identical pass count, per-pass name_hash + executor + every ResourceRef (slot/kind/access/resource_id) + the compiled
  SCHEDULE. A STRONGER gate than the emit_frame_toml round-trip (it runs the ACTUAL runtime lowering, catching any bridge-
  consumed field the toml serializer misses). Also proves 15d-4 (for_each rides the bridge's host `for_each_count` — expands
  identically). ⚠️ **FINDING (15e parity requirement):** the CEIR path requires DECLARED draw lists (Fork A: a pass references
  a draw list by OPERAND → must be a `frame.draw_list`); a desc using UNDECLARED host-resolved-by-name draw lists (a bridge-
  tolerated shortcut) makes to_ceir_frame return nullptr. Shipped assets/fixtures must declare their draw lists — verify at 15e.

### ⭐ CEIR-15 STATUS (driving; 2026-08-11)

⛔ **CORRECTION — STRUCK IN PLACE (SUPERSEDED doctrine):** the "PHASE BOUNDARY — the REMAINDER warrants FOCUSED,
ideally-supervised effort, not unsupervised loop grinding" framing that stood here was a MISTAKE and is repudiated. The loop
NEVER idles; CEIR-15 is DRIVEN autonomously to completion WITH the advisor, gold-standard, no shortcuts, no phase-boundary
stops (`feedback_autonomous_ceir_loop_never_idles_drive_through_every_blocker`).

DEVICE-FREE cook-time work COMPLETE + gated: **15a** (dialect + converter pair + round-trip gate), **15b** (builder emits
ceir.frame), **15c** (`validate_ceir_frame` = the full semantic verifier: structural + resource-shape + def-use + NEW-IN-CEIR
consistency + program-contract for every executor + closed-vocab for every int/string/format param + DependencyCycle;
converter LOSSLESS over its input domain = FLATTENED descs). **15d ANALYSES:** 15d-1 hazards (per-operand effect narrowing),
15d-2 DependencyCycle, 15d-3a lifetimes (for-free via the narrowing), 15d-4 for_each (rides 15d-5's host `for_each_count`),
15d-5 runtime-fidelity gate — all ✅; **15d-3b** memory-pooling first-use parity being solved (the last 15d item — core-planner
fork resolved WITH the advisor: `ResourceLifetime` becomes the memory-live range `[first-use, last-use]`, with a symmetric
ambient extension so a slot-sharing clobber is impossible; converted frames have ZERO ambient ops via 15d-1 so full pooling
precision survives). The §159 headline is realized — the frame graph's barrier/lifetime passes ARE CEIR analysis passes.

RUNTIME phase: **15e** ✅ LANDED (on-device A/B parity on REAL Vulkan+DX12 — the LIVE renderer routes every frame through CEIR;
see the 15e-b entry). NEXT: **15f** (delete the frame-cook adapter — `ceir.frame` lowers DIRECTLY to `FrameGraphTemplate`, the
backward converter retreats to a test/emit instrument; ADR-0106 superseded-in-place — "deletion is the proof") → **15z** GATE.
Fork E (`ceir → desc → existing runtime`) was the incremental on-ramp; the scene_renderer C++ render path is a SEPARATE track
(RAF-12, not 15f).

**Constraints (advisor):** NO CFG for 15d-1 (the converter appends to ONE block `rb`; per-block scope is exactly this
case — the design note's "CFG for cross-block ranges" is a LATER refinement, don't pull forward). Draw-list operands: skip
by TYPE CLASS per the op def (an `r`-token on a Pure-produced draw list stays OUT of the memory-effect set). Touching
crd-ceir re-activates the CONSUMER-RELINK scar after many frame-cook-only ticks — rebuild BOTH crd-ceir-tests AND
crd-frame-cook-tests per config.
