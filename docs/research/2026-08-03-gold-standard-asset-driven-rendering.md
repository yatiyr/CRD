# QUEST — Cerid Gold-Standard Asset-Driven Rendering Foundation (the RAF band mission)

> **Status:** the authoritative mission for the **RAF band** in `docs/detours/D-007-gpu-program-system.md`
> (Rendering Asset Foundation). **RAF ✅ COMPLETE 2026-08-06 (ADR-0106 closed)** — this document remains the
> constitution the post-RAF programme extends. User-directed 2026-08-03: a foundation-cleanup quest to make Cerid's rendering
> architecture fully gold-standard and asset-driven **before** adding more rendering features. **S4 (REN-41 Nanite
> cluster-LOD) is PAUSED after S4-0** until this band lands (see the RAF band note in D-007 for the S4-0 state).
>
> This document is the CONSTITUTION for the band — it is followed verbatim. The detailed target-state **design
> spec** (diagrams, type-ownership, module deps, migration table, lifecycle diagrams, deletion list) is RAF-0's
> deliverable and lands under `docs/design/`. The band's slice rows (RAF-0 … RAF-13) map 1:1 to §19 Phases 0–13,
> each carrying that phase's gate as its DoD. The §22 Definition of Done (35 conditions) is the band's close gate.
>
> ⛔ Reading order for anyone picking this up: this doc → RAF-0's design spec → the active RAF slice row in D-007.
> Prior related work this band CONSOLIDATES (do the §3 reuse audit against all of it, do not rebuild): the REN band
> (authorable frame graphs, composition/anchors/injection, resource lifetime/aliasing/barriers — REN-1/36/37/38),
> the D-007 CKIR program system, material/technique/shader/frame cookers, the two GPU backends, and the declarative
> GPU-command refactor this quest folds in (§7). The S4-0 work already fixed three real gaps this band inherits:
> `lower_entry` must root every entry node (mesh_prim/task); `draw_mesh_storage` needed a synchronous path on both
> backends; the frame-graph WAR cycle rule must be resource-lifetime-aware.

---

## Mission

Refactor and consolidate Cerid's entire rendering foundation before adding more rendering features. The final
result must be a simple, coherent, world-class, asset-driven rendering architecture in which:

- Engine-provided renderers are ordinary assets.
- Applications can author, compose, replace and extend renderers using the same public systems.
- Shaders, materials, techniques, render passes and frame graphs have clear and non-overlapping responsibilities.
- Authoring data, cooked runtime data, per-frame commands and backend objects are strictly separated.
- Vulkan and D3D12 consume one canonical backend-neutral command model.
- No rendering feature requires another combinatorial `draw_*` method.
- No engine-default renderer uses a privileged hidden path unavailable to applications.
- All invalid combinations are rejected early with precise diagnostics.
- Hot reload, dependency tracking, capability selection and deferred GPU destruction are first-class.
- Runtime render recording performs no ordinary per-draw heap allocation or string lookup.
- The architecture remains suitable not only for games, but also simulation, scientific visualization, tools, CAD,
  cinematics and other Cerid consumers.

This is a foundation-cleanup quest. Do not add unrelated rendering features until this foundation is complete. Do
not merely add another abstraction layer over the existing system. Do not leave both the old and new architectures
active at completion.

---

## 1. Conceptual model (use these distinctions consistently)

**1.1 Authoring asset** — editable, serializable data describing intent (material surface graph; lighting/shading
technique; shader-stage declaration; render-pass declaration; frame graph; renderer configuration; capability
variants + explicit fallbacks). May use names/paths/human-readable structures. Must never contain Vulkan or D3D12
concepts.

**1.2 Cooked asset** — the validated, normalized, deterministic runtime representation. Must: contain no unresolved
textual references; use stable IDs/indices/compact tables; carry explicit schema/version; carry dependency
information; be validated before runtime installation; produce deterministic bytes from identical inputs; be
loadable without reparsing authoring text; remain backend-neutral unless it is explicitly a backend shader binary
cache.

**1.3 Runtime asset** — an immutable loaded object backed by cooked data (runtime material definition; material
instance; technique; frame-graph template; compiled frame-graph instance; runtime shader program contract). May
hold stable handles to dependencies; must not own arbitrary backend details unless it is specifically a backend
runtime object.

**1.4 Backend object** — the Vulkan/D3D12 realization (Vulkan shader object; D3D12 PSO; root signature/pipeline
layout; descriptor allocation; physical image/buffer; command-list state). Stays behind backend interfaces + caches.

**1.5 Per-frame command** — what is actually executed this frame (draw a visible mesh section; dispatch compute;
trace rays over an output extent; copy; resolve). Not an authoring asset; produced by executors from live scene +
frame data.

The complete direction (never collapse into one object):

```
Authoring Asset → parse → Validated Description → cook → Cooked Asset → load → Immutable Runtime Asset
  → instantiate/compile → Compiled Runtime Plan → execute each frame → Backend-Neutral GPU Commands
  → lower → Vulkan / D3D12 Commands
```

---

## 2. Non-negotiable architecture rules

**2.1 One concept, one owner, one representation.** No two material models; no two frame-graph command
representations; no two shader binding conventions; no separate direct vs authored command models; no engine-only
vs application path; no Vulkan-oriented vs D3D12-oriented asset model; no synchronous draw verbs vs frame-graph
draw vocabulary. Migration adapters allowed ONLY while moving callers; deleted before completion.

**2.2 Data describes configuration and topology** (what resources/passes exist; what depends on what; which
executor performs a pass; which shader/technique; what state/parameters; what capabilities; what fallback). No
arbitrary control flow, loops, expressions or hidden executable behavior in asset data.
`Asset data → topology/config/references/parameters · C++ → mechanics/scene-traversal/command-gen · CKIR → GPU
computation/shader math · Frame graph → scheduling/lifetime/barriers/queues`.

**2.3 Engine defaults are ordinary assets** — authored with the same public asset types + runtime systems an app
gets. Explicit namespaces `engine://…` and `app://…`; NO implicit file shadowing. An app must explicitly select an
engine asset / select its own / include-or-extend an engine graph / bind an engine alias to a replacement / choose
a declared capability variant. The only built-in emergency path is a tiny bootstrap/error renderer that reports the
asset system itself failed; normal rendering never uses it.

**2.4 Fail loudly, never plausibly wrong.** Never silently: replace a missing technique with a default; drop a
texture/sampler binding; ignore a missing resource or a graph read/write mismatch; replace comparison sampling
with ordinary sampling; substitute a texture format; convert load↔clear; remove MRT outputs; skip a pass yet
report success; change indexed↔non-indexed; replace indirect with a semantically different direct execution; choose
an undeclared backend fallback; keep using stale incompatible dependents after a reload. A failed reload keeps the
last valid runtime version installed and exposes the new error clearly.

**2.5 Simple does not mean weak.** Do NOT: a giant struct with every field; a generic `void*` payload without a
schema; a bag of booleans for command modes; a universal "execute anything" virtual; a translation layer that
merely calls old specialized methods; a backend-neutral copy of every Vulkan/D3D12 struct; dozens of low-level
convenience methods recreating the old API. The design is strongly typed, composable and small.

---

## 3. Preserve and consolidate the good existing work

Cerid already has: CKIR-authored GPU programs; backend-neutral GPU contexts; Vulkan + D3D12 backends; frame-graph
resources/scheduling/barriers/aliasing; authorable frame graphs; frame-graph composition/includes/anchors/injection;
material authoring + cooking; technique authoring + binding contracts; shader cooking + variants; scene rendering;
asset-root selection; engine-authored default frame assets; validation + round-trip emission.

Do a reuse audit before writing replacements. For every proposed new type answer: (1) Does Cerid already represent
this? (2) Is the current type wrong, or merely mislocated? (3) Can it be renamed/split/generalized? (4) Would the
new type duplicate an existing contract? (5) Which module should own the final concept? (6) What dependencies would
the change introduce? (7) Can one representation serve direct, frame-graph AND authored execution? **Prefer
consolidation over parallel replacement.**

---

## 4. Final terminology (mandatory concepts; exact C++ names may differ)

- **4.1 Shader module** — one executable GPU stage/kernel body + metadata (stage, entry, CKIR/compiled rep,
  required features, stage I/O, declared resources, specialization axes, interface signature). Does NOT own
  material values, render targets, scene objects, frame scheduling.
- **4.2 Program** — a compatible collection of shader modules forming one executable GPU program family (VS+FS;
  task+mesh+FS; VS+TCS+TES+FS; compute; RT pipeline stages). Owns stage composition, program interface contract,
  reflection metadata, compatibility requirements. Not material resources/attachments/draw args/frame resources.
- **4.3 Material definition** — reusable surface-response description (surface graph, parameter schema, texture
  declarations, defaults, surface-contract output, declared features). Not lighting loops/shadows/exposure/pass
  scheduling/frame topology. *Preserve the shipped principle: material describes surface response; technique
  describes how that surface is used by a rendering method.*
- **4.4 Material instance** — lightweight overrides referencing one definition (parameter/texture/sampler
  overrides where permitted; explicit feature selections). Does not duplicate the graph or mutate the definition.
- **4.5 Technique** — reusable shading algorithm consuming a declared surface contract + declared frequency-based
  inputs (standard forward; forward CSM; unlit; deferred G-buffer; shadow alpha test; toon; visibility-buffer
  material eval). Owns surface contract, shader body/CKIR ref, required frame/pass/material/object/draw bindings,
  bounded variant axes, supported phases, required capabilities, output contract. Not frame graph/scene
  objects/target images/windows/draw counts.
- **4.6 Render phase** — a stable semantic id for WHY geometry is requested (`depth, shadow, forward, gbuffer,
  velocity, selection, visibility, transparent`). A scene raster pass names a phase; the scene executor uses phase
  + material def + instance + technique + vertex/geometry variant + skinning variant + backend caps + attachment
  signature to obtain the correct runtime program variant. Do not hard-code phase behavior into growing draw
  methods; resolve names → stable IDs at cook/load time (no arbitrary phase strings in the hot path).
- **4.7 Pass executor** — C++ mechanics converting one authored pass + live data into backend-neutral commands
  (`scene.raster, fullscreen.raster, compute.dispatch, transfer.clear, transfer.copy, transfer.blit,
  transfer.resolve, raytrace.dispatch, present`). Owns query/view-resolve/packet-build/dispatch mechanics; NOT
  backend API calls, parsing, lifetime analysis, scheduling, material logic, shader math. **Apps register custom
  executors in C++ without editing a central engine enum.**
- **4.8 Frame graph asset** — a complete rendering architecture or reusable subgraph (logical resources; passes;
  declared reads/writes; attachment bindings; pass executor IDs; draw-list/view refs; parameters; subgraph
  composition; injection anchors; capability requirements + fallback selection; output contract). Not Vulkan
  images/D3D12 resources/live entities/per-frame packets/backend commands.
- **4.9 Render pipeline** — two DISTINCT concepts: the **application render pipeline** (the renderer an app
  chooses — primarily a frame graph asset + its techniques/materials/shaders/settings) vs the **GPU pipeline
  object** (a Vulkan/D3D12 execution object — a backend detail). Do NOT create a monolithic `PipelineAsset` merely
  because both contain "pipeline". Introduce an authored pipeline asset only if the reuse audit PROVES a real
  author-facing concept not already representable by program + technique + pass state + attachment signature.

---

## 5. One canonical asset identity system

Each asset: canonical virtual path; stable asset ID; asset type; schema version; content hash; dependency list;
source location for diagnostics; cooked artifact identity; runtime generation. Canonical explicit namespaces;
deterministic normalization; reject ambiguous/invalid paths; DETECT stable-ID hash collisions (don't assume they
can't happen). No raw filesystem paths as runtime identity; no strings in per-frame recording.

**Mounts** — explicit `engine:// app:// plugin-name:// test://`. Mount precedence must not silently replace an
asset of the same virtual identity; overrides are explicit (selection/alias/inheritance/include/injection/config
binding).

**Asset dependency graph** — frame graph → techniques → shader programs → CKIR modules; technique → surface
contract + shader body; material instance → material definition + textures; runtime program variant → material def
+ technique + vertex variant + pass signature. This graph drives cook invalidation, hot reload, runtime
replacement, error propagation, cache invalidation, deferred destruction.

---

## 6. Separate description / cooked / runtime forms (no one struct for all stages)

Each family needs clearly named forms (exact names may differ; responsibilities must not):
`FrameGraphDesc / CookedFrameGraph / RuntimeFrameGraphTemplate / CompiledFrameGraphInstance`;
`MaterialDesc / CookedMaterial / RuntimeMaterialDefinition / RuntimeMaterialInstance`;
`TechniqueDesc / CookedTechnique / RuntimeTechnique`; `ShaderModuleDesc / CookedShaderModule / RuntimeShaderModule`;
`ProgramDesc / CookedProgram / RuntimeProgram`.

- **Description** — may contain strings/source names/author-friendly arrays; validated; emit-able back to authoring
  text; NOT on the hot path.
- **Cooked** — stable IDs/compact indices; immutable; deterministic serialization; versioned; dependency + interface
  hashes; no unresolved references; loadable without authoring parsers.
- **Runtime** — stable handles to dependencies; generation info; may cache validated lookups; immutable after
  install; replaced atomically at safe boundaries; do not expose backend-native handles upward.

---

## 7. Replace the combinatorial GPU interface (fold the declarative-command refactor in)

Use: (1) explicit rendering scopes; (2) explicit attachment descriptions; (3) typed resource bindings; (4) typed
geometry descriptions; (5) strong draw/dispatch command variants; (6) explicit raster/depth/stencil/blend state;
(7) explicit capability validation; (8) ONE backend-neutral command encoder; (9) direct backend lowering in Vulkan
and D3D12.

- **7.1 Rendering scope** — `RenderingDesc { Span<ColorAttachmentDesc>; Optional<DepthStencilAttachmentDesc>;
  Extent2D; LayerRange; ViewMask }`. Each color attachment: target/view, load op, store op, clear value, resolve
  target when applicable, per-attachment blend. Depth/stencil: target/view, depth load/store, stencil load/store,
  clear values, read-only/write. **Clear vs load is never separate draw functions; one target vs MRT is never
  separate draw functions.**
- **7.2 Resource bindings** — typed model: uniform/storage buffers, sampled/storage textures, samplers, comparison
  samplers, acceleration structures, buffer ranges, texture views, mip/layer selection, access intent. Identified
  through the cooked program contract (authoring = names + frequencies; cook = stable layout slots; hot path =
  compact indices). No hard-coded meanings ("binding 1 = base colour"); standard techniques may define conventions,
  the low-level system stays general.
- **7.3 Geometry source** — separate shader-visible storage data / HW vertex buffers / HW index buffers / meshlet
  data / tessellation patches / draw args. No duplicated storage buffer across unrelated fields without explicit
  semantics. Support existing Cerid geometry paths through strong variants.
- **7.4 Commands** — strong variants `Draw, DrawIndexed, DrawIndirect, DrawIndexedIndirect, DrawIndexedIndirectCount,
  DispatchMesh, DispatchMeshIndirect, DrawPatches`. Keep families distinct: raster / compute / ray tracing /
  transfer / acceleration-structure / presentation. No booleans-bag for indexed/indirect/mesh/tess.
- **7.5 Draw packet** — `RasterDrawPacket { RuntimeProgramHandle; GeometrySource; ResourceBindingTable;
  RasterCommand; RasterState; DynamicDrawValues }`. The SAME canonical packet is used by scene renderer, authored
  frame-graph execution, hand-written graphs, direct tests, Vulkan backend AND D3D12 backend.
- **7.6 Command encoder** — compact interface `begin_rendering/draw/end_rendering; dispatch/trace_rays;
  copy/blit/resolve/clear`. No large family of feature-combination methods.
- **7.7 Delete old verbs** — after migration, remove combination-specific raster methods + backend overrides +
  adapters + obsolete-wrapper-only tests + append-only-growth docs. Do not leave old specialized methods as the
  real implementation beneath the new API.

---

## 8. Replace the giant pass-description pattern

A pass = common graph metadata + a typed executor payload:
`FramePassDesc { String name; String executor; Array<ResourceUseDesc>; Array<AttachmentBindingDesc>;
Array<PassParameterDesc>; QueuePreference; CapabilityExpr requirements }`. Executor-specific authoring data is
validated against the selected executor's schema. At cook: `executor name + named parameters + named resource slots
→ ExecutorTypeId + schema version + compact typed payload + resolved resource indices`. No arbitrary strings in
runtime execution.

Built-in executors (≥): `scene.raster` (draw list/query, view, phase, technique, attachments, pass resources +
params, raster state → resolve visible items, select variants, produce raster packets, record via the canonical
encoder); `fullscreen.raster`; `compute.dispatch`; `transfer.clear/copy/blit/resolve`; `raytrace.dispatch`;
`present`. Custom executor registry (C++, no central enum edit): stable executor ID, schema version, allowed queue
families, named resource-slot schema, parameter schema, required capabilities, cook-time validation, runtime
payload creation, runtime recording callback, diagnostic name. Custom payloads use a schema + cooked binary, never
untyped `void*`. Frame-cook core must not depend on application scene types.

---

## 9. Cleanly connect frame graph, technique and material

**Scene raster pass declares:** draw-list/query ID; view ID; render phase ID; technique asset; attachments; pass
resources; pass parameters; raster state. **Each resolved scene item provides:** geometry; material-instance handle;
object data; skinning/animation data; draw args; optional instance data.

**Program variant resolution** — from an explicit key: technique ID + generation; material definition ID +
generation; material feature key; render phase ID; vertex/geometry variant; skinning variant; program stage family;
attachment/output signature; capability tier; declared technique options. No irrelevant runtime values; no pointer
addresses as identity; no string lookup during draw recording.

**Binding frequencies** — formalize `Frame / Pass / Material / Object / Draw`, each with one owner and one update
cadence (Frame: global values/time/environment; Pass: camera/view, cascade, post input; Material: textures +
params; Object: transform, skin table, object ID; Draw: draw index, offsets/push). Backend-specific layout,
backend-neutral semantic contract. Cooking maps names+frequencies → compact runtime slots deterministically.

**Compatibility validation** (reject at cook / runtime install): technique requiring a pass binding the pass does
not provide; surface contract incompatible with material output; shader output count/type incompatible with
attachments; comparison sampler bound to an ordinary texture contract; missing material resource; duplicate
resource name; phase unsupported by a technique; geometry source incompatible with program stages; material feature
variant out of declared bounds.

---

## 10. Frame graphs as complete renderer assets

Support: logical transient images/buffers; imported external resources; persistent/history/ping-pong resources;
extents relative to output; formats; layers/mips/samples; reads/writes/read-writes; color/depth/stencil attachments;
load/store; queue preference; subgraphs; namespaced includes; explicit bindings; injection anchors;
conditional/capability variants; explicit fallback graph; output contract; pass parameters; view + draw-list refs;
pass executor refs. Preserve: resource lifetime analysis; transient aliasing; barrier derivation; dependency
sorting; layered resources; composition; anchors/injection; round-trip authoring; programmatic construction;
deterministic validation.

**Compile once, execute repeatedly** — separate `FrameGraphAsset / FrameGraphTemplate / FrameGraphInstance /
CompiledFrameGraph / FrameExecution`. An instance may depend on output size/format, MSAA, HDR, capability tier,
selected optional modules, external bindings. Do not parse+rebuild topology every frame; recompile only when a
topology-affecting property changes.

**Declared usage is authoritative** — in dev builds validate during recording that bound resources were declared,
access mode matches, attachment use matches, transfer src/dst matches, undeclared use is diagnosed. No generic
"untracked" escape hatch as the normal solution; if an external immutable resource needs no frame-local edge,
represent THAT explicitly rather than bypassing all tracking via a boolean.

---

## 11. Explicit renderer selection and composition

An app chooses `engine://frame/default-forward`, `engine://frame/default-deferred`, or `app://frame/my-renderer`
through one public renderer/viewport configuration path. Three workflows: (11.1) use an engine renderer unchanged;
(11.2) compose/extend — include an engine subgraph, bind output/resources, inject a pass at a declared anchor,
replace a referenced technique explicitly, disable/select an optional module, override declared graph parameters
(without copying the whole engine graph); (11.3) supply a complete custom renderer (own frame graph, techniques,
materials, shader modules, pass executors) with NO engine-private type required.

---

## 12. Capability and fallback system

Backend-neutral capabilities: mesh/task shaders; tessellation; RT pipeline; inline ray query; indirect count; VRS;
conservative raster; fragment interlock; required format; required sample count; required attachment count;
bindless/resource-indexing tier; async compute; comparison sampling; stencil format. A graph/technique/shader
variant may declare `requires = [...]` / `fallback = asset-id`. Selection is deterministic, inspectable, reported,
testable; NO silent degradation inside a backend method (`mesh-shader graph unavailable → select declared
tessellation or classic-raster fallback asset → report selected variant`, NOT `mesh command → backend silently does
an ordinary draw`). Evaluate capabilities before frame recording where possible.

---

## 13. Hot reload and live replacement

On source change: detect the canonical asset → reparse → validate → re-cook → determine affected dependents →
rebuild dependents in dependency order → validate the complete replacement set → install atomically at a safe frame
boundary → increment generations → defer destruction of old GPU objects until no in-flight frame references them →
report success or exact failure. On failure: keep the previous valid generation, preserve rendering, report the
failing asset + dependency chain + exact contract mismatch, never partially install a mixed generation. Interface
change: revalidate dependent materials/passes, rebuild affected program variants, reject the set if any mandatory
dependent is incompatible. GPU lifetime: frame fences / timeline values / deferred deletion queues / generation-
aware caches.

---

## 14. Runtime handles and generations

Stable typed handles `AssetHandle<MaterialDefinition | MaterialInstance | Technique | ShaderModule | Program |
FrameGraphTemplate>` supporting type safety, stable logical identity, generation detection, invalid/missing state,
cheap hot-path access after resolution. No raw pointers across replacement without a lifetime mechanism; no
per-draw registry lookup — resolve handles into immutable frame-safe views / generation-pinned references before
hot-path recording.

---

## 15. Validation and diagnostics (one coherent model)

Used by parsers, cookers, asset registry, frame-graph compiler, technique/material compatibility, program creation,
command validation, backend capability validation, hot reload. A diagnostic carries (where applicable): error code;
severity; asset ID/path; source line/field; referenced dependency; pass name; resource name; binding name; executor
ID; expected type; actual type; capability name; human-readable explanation. Not just `false / invalid asset /
failed to build pipeline`. Public bool-returning compatibility APIs may remain temporarily, but the canonical
implementation exposes structured diagnostics.

Validation stages (each rule at the EARLIEST stage where all info is available; do not duplicate a rule in five
places): parse validation; description validation; cross-asset cook validation; graph validation; runtime install
validation; development command validation.

---

## 16. Determinism and serialization

For identical source + config + dependency content: identical cooked bytes; identical stable IDs; identical variant
keys; deterministic graph ordering; deterministic binding-layout assignment; deterministic diagnostic ordering;
cache keys independent of pointer addresses; map iteration order must not leak into output; no uninitialized padding
in serialized/hashed data. Version every cooked schema; explicit migration/rejection/recook, never silent
reinterpretation. Keep round-trip gates (`parse → emit → parse`) and cooked-equivalence gates.

---

## 17. Performance requirements

Ordinary draw recording: no heap allocation; no string lookup; no asset/shader-source parsing; no full dependency
traversal; no reflection reconstruction; no dynamic map lookup when a compact index exists; no re-validation of
immutable cooked contracts; no unnecessary ownership changes. Use compact handles, stable indices, spans/views,
frame arenas, precompiled binding tables, cached variant lookup, encoder-side state caches, precomputed
PSO/shader-object keys, bounded arrays / explicit capacity policies. Expensive structural validation at
cook/install; only truly dynamic checks per frame (dev builds may keep stronger command validation; release may use
prevalidated immutable packet templates + compact safety checks — correctness must not disappear). D3D12 PSO
identity includes EVERY correctness-relevant field (stages, attachment formats + count, sample count, DS format,
blend, raster, DS state, topology, conservative raster, …). Vulkan emits/caches every required dynamic state
deterministically.

---

## 18. Module and dependency cleanup

Audit dependencies among `gpu-context, gpu-context-vulkan, gpu-context-dx12, shader-cook, material-cook,
technique-cook, frame-cook, scene-render, resources, asset-io, CKIR modules`. Clear one-way graph, likely: core
asset identity/diagnostics ↑ shader/material/technique/frame descriptions + cookers ↑ runtime render assets + scene
rendering ↑ gpu-context command interfaces ↑ backend implementations. Avoid: frame-cook depending on scene ECS;
backends leaking upward; material cooker depending on renderer runtime; resource registry depending on
Vulkan/D3D12; cyclic cooker deps; one giant rendering module owning everything. A small shared render-asset-core
module (asset IDs, schema headers, diagnostics, dependency records, common resource/binding contracts) only if it
removes real duplication — never a "miscellaneous" dumping ground.

---

## 19. Migration plan (ordered phases; repo buildable at every boundary; each has an explicit gate)

The RAF band's slices RAF-0 … RAF-13 map 1:1 to these phases. Each slice's DoD is its phase's gate.

- **Phase 0 / RAF-0 — Repository audit + architecture map.** Read the rulebook + all rendering docs + every
  graphics asset header/impl + assets + tests. Produce a migration inventory (current asset/runtime/backend types;
  every specialized draw/RT verb; every frame pass kind; every pass field + owner; every hard-coded binding
  convention; every asset-loading path; every engine-default-only path; every hot-reload path; every cache/variant
  key; every test protecting behavior). **Gate 0:** a concise DESIGN SPEC under `docs/design/` (current-state
  diagram; target-state diagram; type-ownership table; module-dependency diagram; old→new migration table; asset
  lifecycle diagram; per-frame execution diagram; hot-reload lifecycle diagram; phased increments; a test gate per
  increment; explicit deletion list). Then proceed to implement.
- **Phase 1 / RAF-1 — Canonical terminology, IDs, diagnostics.** Canonical virtual paths; typed stable asset IDs;
  asset type IDs; schema/version headers; content hashes; dependency records; structured diagnostics; explicit
  namespaces/mounts; migrate render asset families to this identity. **Gate 1:** path normalization; namespace
  separation; collision detection; stable deterministic IDs; structured diagnostics; deterministic dependency
  ordering; no implicit engine/app shadowing.
- **Phase 2 / RAF-2 — Canonical declarative GPU command model.** Rendering-scope + attachment descriptors; resource
  binding tables; geometry variants; raster command variants; compute + RT descriptors; transfer commands;
  capability model; command validator; command encoder; direct Vulkan + D3D12 lowering (old verbs adapted through
  the new model only during migration). **Gate 2:** color-only/depth-only/color+depth/MRT/depth-stencil; load-store;
  per-attachment blend; indexed; direct + indirect; indirect count; mesh/tess where supported; fullscreen raster;
  binding validation; comparison-sampler correctness; cross-backend representative parity; no ordinary per-draw
  allocation.
- **Phase 3 / RAF-3 — Split description/cooked/runtime forms** (shader, material, technique, frame graph): schema
  versions; dependency tables; interface hashes; serialization gates; generation-aware handles. **Gate 3:**
  parse/emit round trip; deterministic cook bytes; load without authoring parser; missing-dependency rejection;
  generation replacement; typed handle safety; old-schema handling.
- **Phase 4 / RAF-4 — Shader + program contract cleanup.** Consolidate stage metadata, stage I/O, resource
  declarations, binding frequencies, technique options, capability requirements, program composition, interface
  signatures, variant keys; remove hard-coded resource-slot assumptions; deterministic cooked binding layouts.
  **Gate 4:** valid/invalid stage composition; duplicate binding rejection; frequency mapping; stable layout; I/O
  compat; attachment output compat; bounded variant enumeration; interface-change invalidation.
- **Phase 5 / RAF-5 — Material + technique contract cleanup** (preserve material=surface / technique=algorithm /
  pass=when+where): material definition; material instance; texture/sampler params; surface contracts; technique
  input contracts; supported phases; technique/material compatibility; program-variant creation. **Gate 5:** many
  instances share one definition; invalid overrides rejected; missing texture/resource diagnosed; material cannot
  access forbidden lighting state; technique cannot consume incompatible surface; technique pass bindings verified;
  phase incompatibility rejected; program variants cached + deterministic.
- **Phase 6 / RAF-6 — Pass executor registry.** Replace central pass-kind growth with a registered executor model;
  built-in executors + schema validation; migrate existing pass kinds; strong typed runtime payloads. **Gate 6:**
  built-in registration; duplicate-ID rejection; schema-version mismatch rejection; parameter/resource-slot/queue
  validation; app-defined executor registration; custom executor usable from an app asset WITHOUT editing the
  engine enum; no authoring `void*`; no runtime string lookup during pass recording.
- **Phase 7 / RAF-7 — Frame graph + authored runtime unification.** Authored execution: resolve the cooked executor,
  resolve compact resource indices, create/reuse compiled instances, invoke executors, record canonical command
  descriptors (no large pass-enum switch calling specialized verbs). Preserve DAG scheduling/barriers/aliasing/
  persistent+history/subgraphs/namespaces/anchors/injection/queue scheduling/presentation/round-trip editing.
  **Gate 7:** hand-built == authored commands/output; multiple packets in one scope; declared use matches recorded;
  undeclared diagnosed; load-from-prior; transient aliasing correct; history survives; resize recompiles only what's
  needed; one submission where expected.
- **Phase 8 / RAF-8 — Scene renderer conversion to orchestration.** It resolves the selected frame graph, provides
  host resources, resolves draw lists/views, provides scene/material data, manages graph instances, triggers
  execution, exposes diagnostics/profiling; low-level combination logic moves out; it is never where each new
  feature is manually wired. **Gate 8:** scene.raster executor resolves real items; instances + techniques produce
  correct variants; rigid + skinned; indexed + indirect; shadow/depth/forward/velocity through common mechanisms;
  no new specialized backend draw path in scene renderer.
- **Phase 9 / RAF-9 — Engine default assets migration.** Every shipped engine path becomes ordinary assets (forward,
  shadow, GPU-cull, mesh, tessellation, visibility-buffer, RT, velocity/debug where supported); no hard-coded hidden
  fallback; clear default renderer selection. **Gate 9:** engine default loads by canonical asset ID; no caller
  needs embedded TOML; same public registry as app assets; missing default reports a clear error; representative
  output unchanged; default renderer inspectable/emittable/composable like an app graph.
- **Phase 10 / RAF-10 — Application-custom renderer proof.** A small example/test app package that, WITHOUT modifying
  engine rendering code: (1) uses an engine default graph unchanged; (2) includes an engine graph as a subgraph;
  (3) injects a custom pass at a declared anchor; (4) replaces the tonemap/post technique; (5) adds an app material;
  (6) adds an app technique; (7) selects a fully app-authored frame graph; (8) registers + uses one small custom C++
  pass executor; (9) uses explicit capability fallback; (10) runs on both backends where available. **Gate 10:** the
  app must not call an engine-private renderer method, add a backend virtual, modify a central pass enum, hard-code
  backend slots, embed its frame asset as a C++ string, bypass validation, or use a privileged engine-only path.
- **Phase 11 / RAF-11 — Hot reload + dependency replacement.** **Gate 11:** reload material param/default; material
  graph; technique; shader body; frame graph; dependency-chain invalidation; interface-change rejection; last-good
  preservation; atomic generation installation; deferred destruction after in-flight frames; no stale mixed-
  generation program variant.
- **Phase 12 / RAF-12 — Delete legacy architecture.** Delete specialized draw-method public surface + backend
  overrides; old pass-kind switch machinery; adapters; duplicate asset identities; embedded default frame
  definitions; obsolete hard-coded binding conventions; parallel runtime asset models; dead compat fields; stale
  append-only comments; old-architecture tests/docs. Prove absence via repo-wide search. **Gate 12:** one active
  rendering architecture; no "temporary" layer without a specific unavoidable external-ABI reason documented in an
  ADR.
- **Phase 13 / RAF-13 — Documentation + final verification.** Update ADRs; system overview; asset authoring guide;
  frame graph guide; material guide; technique guide; custom renderer guide; custom executor guide; hot-reload
  guide; backend implementation guide; migration notes; context/roadmap. Diagrams for authoring/cooking/loading/
  variant-resolution/graph-compilation/pass-execution/command-lowering/hot-reload. **Gate 13:** a new engineer can
  answer from docs: what is a material / technique / render phase / pass executor / frame graph / application render
  pipeline / backend pipeline object; how an app replaces the renderer; how a custom pass mechanic is added; how
  invalid bindings are diagnosed; what is rebuilt after a shader reload.

---

## 20. Mandatory test matrix

Add focused tests for: **Asset system** (canonical IDs; mounts/namespaces; dependency graph; deterministic cooking;
schema versions; round trips; invalid references; reload generations). **Material + technique** (surface-graph
validation; instances; texture/sampler overrides; binding frequencies; surface compatibility; phase compatibility;
variant enumeration; option bounds). **Frame graph** (dependency ordering; cycles; resource lifetimes; aliasing;
layered resources; persistent/history; subgraphs; anchors/injection; executor schema; capability fallback;
resize/recompile). **Command model** (attachments; load/store; depth/stencil; MRT; blend; direct/indexed;
indirect/count; mesh/tess; compute; ray tracing; copy/blit/resolve; validation failures). **Backend** (Vulkan
validation-layer clean; D3D12 debug-layer clean where available; PSO cache identity; dynamic state reset; binding
correctness; comparison samplers; cross-backend representative parity). **Application extensibility** (engine graph
selection; app graph selection; composition; pass injection; technique replacement; custom executor; no engine-
private path). **Performance** (no ordinary per-draw allocation; no render-thread string lookup; stable variant-
cache behavior; no per-frame graph parse; no redundant backend object recreation; no accidental pipeline-cache
collisions).

---

## 21. Architecture traps to reject during review

New asset system beside the old one; a "declarative" wrapper calling old verbs; a giant pass struct; a central enum
as the extension point; everything-becomes-a-custom-executor; material owns lighting; technique owns frame topology;
frame graph owns live scene entities; asset contains backend state; runtime performs authoring work; hidden
fallback; overengineering (five layers where two suffice — every class/module has a concrete independent
responsibility); ABI superstition (determine whether a real external binary ABI exists; don't preserve a bad
internal vtable for a hypothetical consumer); feature development during foundation work (only minimal example
assets to prove extensibility).

---

## 22. Definition of Done (band close gate — all 35 true)

1. Engine rendering defaults are ordinary assets. 2. Apps can choose/compose/extend/replace renderers through public
asset systems. 3. Shaders, programs, materials, techniques, render phases, executors and frame graphs have distinct
documented responsibilities. 4. Authoring descriptions, cooked assets, runtime assets, compiled plans and backend
objects are separate. 5. One canonical asset identity/dependency/diagnostics model. 6. One canonical backend-neutral
GPU command model. 7. Authored and hand-built frame graphs record the same command descriptors. 8. Vulkan and D3D12
directly lower the canonical model. 9. Combination-specific `draw_*` growth eliminated. 10. Pass kinds no longer
grow through a central engine enum for app extension. 11. Executor-specific pass data no longer accumulates in one
giant struct. 12. Material describes surface response, no lighting. 13. Technique describes shading, no frame
scheduling. 14. Frame graph describes topology, no arbitrary logic. 15. Pass executors provide mechanics, no backend
API calls. 16. Scene renderer is orchestration, not a feature monolith. 17. Binding names + frequencies resolved
during cooking. 18. Render recording performs no ordinary string lookup. 19. Ordinary draw recording performs no
heap allocation. 20. Variant keys deterministic + complete. 21. D3D12 PSO cache keys contain every correctness-
relevant property. 22. Vulkan state established deterministically at rendering-scope boundaries. 23. All invalid
cross-asset contracts produce precise diagnostics. 24. Capability fallback explicit + inspectable. 25. Hot reload
installs complete compatible generations atomically. 26. Failed reload keeps the last valid generation. 27. Old GPU
objects destroyed only after in-flight use completes. 28. Cooked output deterministic. 29. Asset schemas versioned.
30. An application example proves a fully custom renderer without engine-private access. 31. Temporary adapters and
parallel old paths deleted. 32. Relevant builds/tests/validation-layers/performance gates pass. 33. Documentation
describes only the final architecture. 34. Adding a texture/attachment/material/shader/post-process pass/graph
composition does not require changing a backend interface. 35. Adding an ordinary new renderer requires assets, not
engine renderer surgery.

---

## 23. Work conduct

Work autonomously; do not ask for superficial naming choices; follow repo conventions + make the strongest design
choice the existing code supports. Keep changes incremental, tree green at phase gates. Do not stop at
planning/scaffolding/adapters. Do not claim tests or backend execution not actually performed — where a backend
can't run here: compile where possible, add backend-neutral coverage, state exactly what was not run, don't call it
validated. No broad TODOs; any deferral names a concrete reason + blocked dependency + safety of the temporary state
+ an explicit follow-up gate. At completion report: final architecture; module dependency graph; asset lifecycle;
command lifecycle; major types added/split/renamed/removed; old APIs deleted; engine defaults migrated; app
extensibility proof; validation/diagnostic model; hot-reload behavior; tests executed; performance/allocation
results; backend verification; unresolved blockers; deviations + why.

**The final result must feel smaller and easier to reason about than the current system despite being more capable.
The standard:** an application author can describe a renderer using clear assets; the cooker can prove those assets
fit together; the runtime can execute them without interpretation overhead; and each backend can lower the same
commands without hidden behavior.
