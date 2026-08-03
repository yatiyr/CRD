# RAF-0 — Gold-Standard Asset-Driven Rendering Foundation: DESIGN SPEC

> The implementation CONTRACT for the RAF band. Mission (followed verbatim):
> `docs/research/2026-08-03-gold-standard-asset-driven-rendering.md`. Band + slices: D-007 "RAF band"
> (RAF-0 … RAF-13 ↔ mission §19 Phases 0–13). This doc is RAF-0's deliverable (Gate 0) + the current→target map,
> type-ownership, module-deps, old→new migration table, lifecycle diagrams, per-increment gates, and the deletion
> list. ⛔ Reading order: mission → this spec → the active RAF slice row in D-007.
>
> **Prime directive (user, 2026-08-03): do NOT break the sandbox or anything already built.** The method that
> guarantees this is the mission's own (§19/§23): every phase keeps the tree building AND the sandbox rendering;
> old paths stay until their callers are migrated; legacy is deleted only at RAF-12. Each phase below states its
> "sandbox-safe" invariant.

---

## 1. Current-state map (the audit — file:line evidence)

Cerid's rendering is powerful but has **three structural debts** the mission names, all measured here:

### 1.1 Combinatorial GPU verb surface — `engine/gpu-context/include/crd/gpu/raster_context.hpp`
**~57 `virtual draw_*/dispatch_*/trace_*` methods** on `IRasterContext`, each a hand-cut combination of
{clear|load} × {1|MRT|depth|depth-only|stencil} × {storage-pull|textured|shadowed|bindless} × {indexed|indirect|
count|mesh|tess|mrt}. Representative: `draw_storage`, `draw_storage_depth`, `draw_storage_depth_load`,
`draw_storage_depth_only`, `draw_storage_multi_depth`, `draw_storage_multi_indexed_depth`,
`draw_storage_multi_indexed_mrt_indirect`, `draw_storage_textured_shadowed_depth`, `draw_bindless_storage`,
`draw_mesh`, `draw_mesh_storage`(+`_load`), `draw_tess_storage`(+`_load`), `draw_visbuffer`(+`_load`),
`draw_wboit`, `trace_rays`/`_anyhit`/`_full`, `dispatch_kernel`/`_rt`/`_indirect`/`_sampled`. Every new feature
this session ADDED verbs (`draw_storage_indexed_mrt`, the synchronous `draw_mesh_storage`). **This is the
combinatorial explosion DoD #9 / §7.7 eliminates.**

### 1.2 Central pass enum — `engine/frame-cook/include/crd/framecook/frame_asset.hpp:37` `FramePassKind`
**18 kinds**, every one commented "appended at the END of the enum (a renumbered kind silently reclassifies every
cooked graph)": `RasterGeometry, RasterDepthOnly, RasterFullscreen, RasterMrt, Compute, Present, Clear, Copy,
Blit, Resolve, RasterTess, RasterMesh, RasterVisbuffer, RasterComposite, RayTrace, RayTracePipeline,
ComputeIndirect, RasterMeshIndirect`. `frame_runtime.cpp` `switch`es on this enum to the §1.1 verbs (e.g.
`RasterMesh`→`draw_mesh_storage`, `RasterMrt`→`draw_storage_mrt`, `RasterFullscreen`→`draw_bindless`/`draw_textured`).
**This is the central extension point DoD #10 / §8 replaces with a registry.**

### 1.3 Giant pass struct — `frame_asset.hpp:250` `FramePassDesc`
**~40 fields**, most serving one kind: `draw_list/view` (geometry), `shader` (fullscreen), `kernel` (compute),
`raygen/miss/closest_hit/any_hit/intersection/callable` (RT), `technique/material_pass` (scene), `blend[]`,
`shading_rate/rate_combiner/conservative`, `sampler`, `filter` (blit), `state`, `load_target/load_depth/
shared_depth/depth_as_float/untracked_storage`, `params[]`. A new mechanic appends more. **This is the giant
struct DoD #11 / §8 replaces with `{common metadata} + typed executor payload`.** Note `untracked_storage`
(line 340) is exactly the §10 "generic untracked escape hatch" to represent explicitly instead.

### 1.4 Asset identity + namespace — `crd://…`
Assets are named `crd://frame/…`, `crd://scene/…`, `crd://post/…`, `crd://shadow/…` and resolved through
`SceneRenderer::program(id)` / `kernel(id)` string switches (`scene_renderer.cpp:~4632`). Frame assets load from
disk via `CRD_ASSETS_DIR` (`SceneRenderer::init`) with **implicit shadowing** (a shipped `assets/frame/*.toml`
shadows an embedded default — the embedded pack was already retired). There is **one namespace** (`crd://`),
**no `engine://` vs `app://` split**, and per-frame recording does **string** program resolution. **§2.3/§5 make
namespaces explicit + kill string lookup on the hot path.**

### 1.5 Engine-default renderers = 15 disk frame assets, not a privileged path (partly good already)
`assets/frame/*.frame.toml`: `forward_csm`, `forward_basic`, `forward_csm_agx/srgb`, `forward_agx/srgb`,
`forward_csm_gpu/gpu_srgb`, `velocity_debug`, `scene_tess`, `scene_mesh`, `scene_visbuffer`, `scene_cull`,
`scene_rt`. **Good:** the default renderer is ALREADY authored assets (REN-36/38). **Gap:** the C++ scene renderer
still owns the low-level combination logic they drive (the §1.1 verbs), the `crd://scene/*` programs are C++
builders (`ensure_*_program`) not authored shader/technique assets, and there is no `engine://` namespace or
public "select a renderer by ID" entry beyond `set_frame_graph_toml(text)`. **RAF-8/9 close this.**

### 1.6 Desc/cooked/runtime conflation
- **Frame:** `FrameGraphDesc` (desc) → a cooked blob (`frame_emit.cpp`, versioned) → `Dx12FrameGraph`/`VulkanFrameGraph`
  (runtime+backend fused — the graph object IS the backend recorder). **No backend-neutral runtime template /
  compiled instance split** (§6/§10): topology is validated at cook but the runtime graph re-derives per build.
- **Material:** `MaterialDesc` → `cook_material` (KGraph) — no distinct `RuntimeMaterialDefinition`/`Instance`
  (§4.3/4.4/6); instances don't exist as a first-class lightweight override.
- **Technique:** `.crdt` desc + `TechniqueLibrary` — closer, but the runtime technique + program-variant cache
  live inside `SceneRenderer::Impl` as C++ state, not a `RuntimeTechnique` asset (§4.5/6).
- **Shader/Program:** CKIR `KGraph`+`KEntry` (desc-ish) → backend `IGpuProgram` (backend). The **program contract**
  (stage I/O, binding frequencies, variant key) is implicit / scattered (`ckir.hpp` KEntry + reflection + the
  scene renderer's variant logic). §4.1/4.2/RAF-4 make it a first-class `Program`/`ShaderModule` contract.

### 1.7 What is already GOLD (preserve, do not rebuild — §3 reuse audit result)
Authorable + composable frame graphs with includes/anchors/injection/namespacing (REN-36/37); resource lifetime
analysis + transient aliasing + barrier derivation + persistent/history/ping-pong + layered resources + one-submit
(REN-1/2/3); the **lifetime-aware WAR cycle rule** (fixed this session, both backends); CKIR authored programs +
the 5 emitters + oracle; material=surface / technique=algorithm SEPARATION (ADR-0102, REN-37); the 5 cookers +
round-trip + determinism gates; capability queries (`mesh_shader()`, `supports_rt_pipeline()`, …); the disk
asset-root convention. **RAF consolidates onto these; it does not replace them.**

---

## 2. Target-state map (the mission, condensed to Cerid types)

```
Authoring (engine:// | app://)         Cook (deterministic, versioned)        Runtime (immutable, generational)
  ShaderModuleDesc  ────────────────►  CookedShaderModule ──────────────────► RuntimeShaderModule
  ProgramDesc       ────────────────►  CookedProgram (interface hash) ───────► RuntimeProgram ──┐
  MaterialDesc      ────────────────►  CookedMaterial ──────────────────────► RuntimeMaterialDefinition
                                                                               RuntimeMaterialInstance (overrides)
  TechniqueDesc     ────────────────►  CookedTechnique (surface+pass contract)► RuntimeTechnique                 │
  FrameGraphDesc    ────────────────►  CookedFrameGraph (executor IDs+slots) ─► RuntimeFrameGraphTemplate         │
                                                                               └─ compile(size,fmt,caps) ─────►  │
                                                                                  CompiledFrameGraphInstance      │
   each frame:  CompiledFrameGraphInstance.execute(scene, views) ── per pass ──► PassExecutor(payload, live data) │
   produces ►  canonical backend-neutral commands (RenderingDesc + RasterDrawPacket + Dispatch/Transfer/Trace) ◄──┘
   lower    ►  Vulkan cmd buffer  /  D3D12 command list      (ONE lowering per backend, no hidden behavior)
```

Program-variant key (deterministic, §9): `{technique id+gen, material def id+gen, material feature key, render
phase id, vertex/geometry variant, skinning variant, program stage family, attachment/output signature,
capability tier, technique options}`. Binding frequencies (§9): `Frame / Pass / Material / Object / Draw`.

---

## 3. Type-ownership table (final owning module per concept)

| Concept | Owning module | Desc / Cooked / Runtime types | Notes |
|---|---|---|---|
| Asset identity, ID, dependency, diagnostics | **new `render-asset-core`** (tiny) | `AssetId`, `AssetRef`, `DependencyRecord`, `Diagnostic` | §5/§15; only if it removes real dup (it does — 5 cookers duplicate ID/diag) |
| Shader module | `shader-cook` + `kir` | `ShaderModuleDesc`/`CookedShaderModule`/`RuntimeShaderModule` | wraps CKIR `KGraph`/`KEntry` + reflection |
| Program | `shader-cook` | `ProgramDesc`/`CookedProgram`/`RuntimeProgram` | stage composition + interface hash + variant axes |
| Material definition/instance | `material-cook` | `MaterialDesc`/`CookedMaterial`/`RuntimeMaterialDefinition`+`Instance` | surface only; NO lighting |
| Technique | `technique-cook` | `TechniqueDesc`/`CookedTechnique`/`RuntimeTechnique` | surface+pass contract, phases, options, caps |
| Render phase | `render-asset-core` | `RenderPhaseId` (stable enum+registry) | authored name → id at cook |
| Pass executor | ~~`frame-cook` (registry)~~ **`crd-render-pass`** (registry) + consumers register | `ExecutorTypeId` + per-executor schema + payload | ⛔ CORRECTED (ADR-0106, RAF-8): the registry is its own leaf module `crd-render-pass`, NOT frame-cook — folding it in rebuilds the giant module §18/§21 forbid. built-ins in engine; apps register in C++ |
| Frame graph | ~~`frame-cook`~~ **desc/cooked in `frame-cook`; runtime in `crd-render-graph`** | `FrameGraphDesc`/`CookedFrameGraph` (frame-cook) · `FrameGraphTemplate`(=RuntimeFrameGraphTemplate)/`CompiledFrameGraph`(=CompiledFrameGraphInstance) (**`crd-render-graph`**) | ⛔ CORRECTED (ADR-0106, RAF-8): the RUNTIME template + compiled instance live in `crd-render-graph` (the single live runtime); frame-cook keeps the desc + cooked blob + the Cooked→Template **load bridge** (a new acyclic `frame-cook → render-graph` edge). topology only |
| Canonical GPU commands | `gpu-context` | `RenderingDesc`, `RasterDrawPacket`, `Dispatch/Transfer/Trace*Desc`, `CommandEncoder` | backend-neutral |
| Backend objects | `gpu-context-vulkan`/`-dx12` | PSO/shader-object/root-sig/descriptors/images | lower the canonical model |
| Capability + fallback | `gpu-context` + `frame-cook` | `Capability`, `CapabilityExpr`, fallback selection | §12 |
| Scene orchestration | `scene-render` | (no new asset types) | resolves graph+views+draw-lists, executes |

**One-way module dependency (target; ADR-0106):**
`render-asset-core` ↑ `{render-pass, render-graph, gpu-context}`; `render-graph` ↑ `{render-asset-core, render-pass,
gpu-context}`; `frame-cook` ↑ `{render-graph, gpu-context, ...}` (the RAF-8 load bridge — acyclic: render-graph
depends on neither frame-cook nor scene) ↑ `scene-render` ↑ `gpu-context` ↑ `{vulkan,dx12}` (+ `kir` sideways under
the cookers). ⛔ Forbidden edges to prove absent at RAF-12: frame-cook→scene ECS;
backend→upward; material-cook→renderer runtime; resource registry→Vulkan/DX12; cyclic cooker deps.

---

## 4. Old → new migration table (the core of the refactor)

| Current | Target | Phase |
|---|---|---|
| ~57 `IRasterContext::draw_*/dispatch_*/trace_*` verbs | `CommandEncoder::{begin_rendering, draw, end_rendering, dispatch, trace_rays, copy, blit, resolve, clear}` consuming `RenderingDesc` + typed packets | RAF-2 (add) → RAF-12 (delete verbs) |
| `{clear|load}`, `{1|MRT}` as separate verbs | `ColorAttachmentDesc{load,store,clear,resolve,blend}` + `DepthStencilAttachmentDesc` in `RenderingDesc` | RAF-2 |
| `indexed/indirect/count/mesh/tess` as verb suffixes | `RasterCommand` strong variants `{Draw,DrawIndexed,DrawIndirect,DrawIndexedIndirect,DrawIndexedIndirectCount,DispatchMesh,DispatchMeshIndirect,DrawPatches}` | RAF-2 |
| `IStorageBuffer&` reused for pull-vertex/index/args/bounds | `GeometrySource` variant {storage, vertex-buf, index-buf, meshlet, patches, draw-args} | RAF-2 |
| hard-coded bindings (`binding 1=base colour`, `16=bindless`, `s6=shadow`) | `ResourceBindingTable` resolved from `CookedProgram` (names+frequency → compact slot) | RAF-4 |
| `FramePassKind` 18-kind enum + `frame_runtime` switch | `ExecutorTypeId` + `PassExecutor` registry; built-ins `scene.raster/fullscreen.raster/compute.dispatch/transfer.{clear,copy,blit,resolve}/raytrace.dispatch/present` | RAF-6/7 |
| `FramePassDesc` ~40 fields | `FramePassDesc{name, executor, resources[], attachments[], parameters[], queue, requirements}` + typed cooked executor payload | RAF-6/7 |
| `untracked_storage=true` boolean | explicit "external immutable resource, no frame-local edge" resource kind | RAF-7 |
| `crd://…` single namespace + implicit shadowing | `engine://` + `app://` + explicit select/include/inject/alias | RAF-1/9 |
| `SceneRenderer::program(id)`/`kernel(id)` string switch on hot path | `RuntimeProgram` handles resolved at compile; compact indices at record | RAF-4/8 |
| `ensure_*_program` C++ CKIR builders for `crd://scene/*` | authored `engine://shader/…` + `engine://technique/…` (or kept as engine-internal `RuntimeProgram` where a shader has no authored variant — e.g. cluster mesh, like gpu_skin) | RAF-9 |
| runtime graph = backend recorder fused | `RuntimeFrameGraphTemplate` (neutral) + `CompiledFrameGraphInstance` (per size/fmt/caps) + backend lowering | RAF-7 |
| bool-returning `verify_*` compat checks | structured `Diagnostic` (code, asset, field, expected/actual, capability, message) | RAF-1/5 |

---

## 5. Lifecycle diagrams

**Asset:** `source (engine://|app://) → parse → Desc (validated) → cook (deterministic bytes, dep+iface hash) →
Cooked (loadable, no parser) → load → Runtime (immutable, generation N) → instantiate/compile → Compiled plan`.

**Per-frame (hot path — no alloc/strings):** `CompiledFrameGraphInstance.begin(frame) → for pass in schedule:
executor.record(payload, live-scene/views, encoder) → encoder emits RenderingDesc + packets → backend lowers →
submit`. Program variant: resolved-at-compile handle → compact index at record (variant CACHE keyed by §2 key).

**Hot reload (§13):** `watch fires → reparse+validate+recook changed asset → walk dependency graph → rebuild
dependents in order → validate the FULL replacement set → atomically install at frame boundary (gen N→N+1) →
enqueue old GPU objects on the deferred-deletion queue (freed when no in-flight frame ≥ their submit fence) →
report`. On failure: keep gen N, render continues, report failing asset + chain + exact mismatch.

---

## 6. Phased increments (refined) + gate + sandbox-safe invariant + deletion

Each maps to a D-007 RAF row; DoD = the mission phase gate. **"Sandbox-safe" = after this phase the sandbox still
builds + renders identically (verify with `crd-sandbox --smoke-test` both backends).**

- **RAF-1 — identity/namespaces/diagnostics.** Add `render-asset-core` (AssetId, AssetRef, DependencyRecord,
  Diagnostic); map render-asset families onto it; add `engine://`/`app://` as ALIASES of `crd://` first (no
  behavior change), migrate references incrementally. *Sandbox-safe:* `crd://` keeps resolving (alias). *Gate 1*
  as mission. *Delete later (RAF-12):* the ad-hoc per-cooker ID/diag duplication.
- **RAF-2 — canonical command model (ADD alongside).** New `gpu-context` types `RenderingDesc`,
  `ColorAttachmentDesc`, `DepthStencilAttachmentDesc`, `ResourceBindingTable`, `GeometrySource`, `RasterCommand`,
  `RasterState`, `RasterDrawPacket`, `Dispatch/Transfer/Trace*Desc`, `CommandEncoder`; implement `CommandEncoder`
  on BOTH backends lowering directly. ⛔ The old verbs stay and KEEP working (the encoder is additive). *Sandbox-
  safe:* nothing calls the encoder yet. *Gate 2* as mission (a NEW `crd-gpu-context` test suite proves each
  attachment/command/binding case + cross-backend parity + no per-draw alloc). *Delete later:* nothing yet.
- **RAF-3 — split desc/cooked/runtime forms** for shader/material/technique/frame (add the missing forms +
  schema versions + dep tables + iface hashes + generational handles). *Sandbox-safe:* existing cook paths keep
  producing today's runtime objects via thin adapters. *Gate 3* as mission.
- **RAF-4 — shader+program contract.** `ShaderModule`/`Program` with stage I/O + binding frequencies + variant
  keys; deterministic cooked binding layouts; a `RuntimeProgram` handle. Migrate the scene renderer's variant
  logic onto it. *Sandbox-safe:* variant results identical (gate: same DXIL/SPIR-V for the same key). *Gate 4.*
  *Delete later:* hard-coded binding constants (`binding 1/16`, `s6`) once techniques carry conventions.
- **RAF-5 — material+technique contract.** `RuntimeMaterialDefinition`/`Instance`, surface/pass/phase contracts,
  structured compat diagnostics. *Sandbox-safe:* the scene material renders identically. *Gate 5.*
- **RAF-6 — pass-executor registry.** `ExecutorTypeId` + registry + the 8 built-ins with schema; cook converts
  `executor name + params + slots → typed payload`. Migrate `FramePassKind`→executors ONE kind at a time (both
  the old switch and the new registry resolve during migration). *Sandbox-safe:* each migrated kind produces the
  same commands (gate compares). *Gate 6.* *Delete later:* `FramePassKind` + the switch (RAF-12).
- **RAF-7 — frame-graph + authored unification.** Authored execution records the canonical model via executors;
  `RuntimeFrameGraphTemplate`/`CompiledFrameGraphInstance` (compile-once); replace `untracked_storage` with an
  explicit external-resource kind. *Sandbox-safe:* hand-built == authored == today's output (gate). *Gate 7.*
- **RAF-8 — scene renderer → orchestration.** Move combination logic out; scene renderer resolves graph+views+
  draw-lists + provides scene/material data + manages instances + executes. *Sandbox-safe:* identical frames.
  *Gate 8.*
- **RAF-9 — engine defaults → `engine://` assets.** The 15 frames + their programs become `engine://` assets;
  add a public "select renderer by asset ID" entry; keep engine-internal `RuntimeProgram` builders (gpu_skin,
  cluster mesh, cull) as legitimate non-authored programs (they have no author-facing variant). *Sandbox-safe:*
  the sandbox selects `engine://frame/...` and renders identically. *Gate 9.* *Delete later:* implicit shadowing.
- **RAF-10 — app-custom renderer proof** (a `tests/` app package doing the 10 workflows, both backends). *Gate 10.*
- **RAF-11 — hot reload** (dependency-aware, atomic generations, deferred destruction). *Gate 11.*
- **RAF-12 — DELETE legacy.** The ~57 verbs + backend overrides, `FramePassKind` + switch, giant-struct fields,
  adapters, embedded/implicit-shadow paths, hard-coded bindings, parallel runtime models, dead compat fields,
  stale append-only comments. Prove absence via repo-wide grep. *Sandbox-safe:* everything already runs on the
  new path before deletion (RAF-8/9 moved it). *Gate 12.*
- **RAF-13 — docs + final verification** (guides + diagrams + the §22 35-condition DoD). *Gate 13.*

## 7. Deletion list (proven empty at RAF-12 via repo-wide grep)

`IRasterContext` combination verbs (§1.1, ~57) + their Vulkan/DX12 overrides · `FramePassKind` + `frame_runtime`
kind-switch · the single-purpose `FramePassDesc` fields folded into executor payloads · `untracked_storage` ·
hard-coded binding constants in high-level rendering · `crd://` implicit shadowing + embedded-default remnants ·
migration adapters · bool-only `verify_*` compat APIs · duplicate per-cooker ID/diagnostic code · stale
"appended at the END" comments · tests that only exercise obsolete wrappers.

## 8. Named risks + non-goals

**Risks:** (1) the command model must serve scene + authored + hand-built + tests with ONE packet — validated by
the RAF-7 "hand-built == authored" gate before any deletion. (2) both backends must lower identically — RAF-2
cross-backend parity gate. (3) determinism of cooked bytes + variant keys — RAF-3/4 gates (no pointer/map-order
leakage; POD field-by-field serialization per the padding scars). (4) the sandbox must never regress — every phase
runs `crd-sandbox --smoke-test` both backends. **Non-goals:** no new rendering FEATURES (S4/Nanite resumes AFTER
RAF); no authored `PipelineAsset` unless RAF-4 proves a real author-facing need (§4.9); no scripting in assets.
