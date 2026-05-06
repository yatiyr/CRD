# Cerid — Live Context

> Engine'in kısa-vadeli hafızası. "Şu an neredeyiz?" sorusuna cevap verir.
> "Master plan ne?" sorusunun cevabı `docs/ROADMAP.md` ve oradan
> dallanan dosyalardadır.
>
> Her session sonu `@docs-keeper` günceller. Kısa kalır. Eski session
> detayları `docs/sessions/YYYY-MM-DD-*.md`'de yaşar, burada değil.

---

## Current focus

**Phase 3.0 — Scene/ECS foundation. v1a SHIPPED 2026-05-06 (`crd-scene` skeleton: EntityId + SlotMap + World). 13 slices remaining. Next: v1b — `ComponentRegistry` + `IStorageBackend` interface + storage-hint registration grammar (ADRs 0050, 0053, 0056).**

The architecture is the **eight-layer slot-shaped ECS** designed for million-entity scenes, agents-as-components-with-scripts, UI on the same machinery (game and editor), with every novel ECS extension as a registered slot:

```
L8 Reflection / Editor          (Phase 7)        — API reserved
L7 Scripting & Behaviors        (Phase 4.0+)     — API reserved
L6 Replication / Networking     (Phase 4.2)      — API reserved
L5 Indexes                       (Phase 3.0+)
   ChangeDetect, AsyncAware                       — ship in 3.0
   History, SpatialBVH, GpuResident               — API only in 3.0
L4 Query · System · Schedule    (Phase 3.0)
L3 Relations                     (Phase 3.0)
L2 Storage backends              (Phase 3.0)
   ArchetypeChunk + SparseSet hybrid
L1 Entity / SlotMap              (Phase 3.0)
L0 Memory · Containers · Jobs    (already shipped)
```

Cerid signature: a uniform `IComponentIndex` extension framework where every novel ECS extension (history, change detect, spatial, GPU-mirror, async, replication, scripts, reflection) is a registered slot consuming the same component-lifecycle event stream. Adding the next extension is a one-day job.

Slices: ~~v1a (Entity+SlotMap)~~ ✅ → **v1b (registry)** ← active → v1c (Archetype storage) → v1d (SparseSet storage) → v1e (mixed-backend chunk visitor) → v1f (relations) → v1g (query DSL) → v1h (system+schedule) → v1i (index framework + ChangeDetect + AsyncAware) → v1j (Transform + propagation) → v1k (SceneResource+Loader) → v1l (cook_scene cooker handler) → v1m (sandbox renderer integration) → v1n (reserved-slot freeze).

Active phase doc: `docs/phases/phase-3.0-scene-ecs.md`.

## Previous focus (closed)

**Phase 2.8 — Material GPU wiring + sandbox rendering + asset import. ALL SLICES SHIPPED 2026-05-06. Phase 2.8 COMPLETE.**

Phase 2.8 slices:
- v1a: Per-material pipeline cache in `ForwardRenderPath` (`m_mat_cache`, keyed by material pointer) ✅
- v1b: Multi-pass shader selection — `PipelineResolver::begin_pass()` default impl + `ForwardRenderPath` calls it before each pass ✅
- v1c: Depth-only prepass pipeline (vertex-only, `Format::Undefined` color, `D32Sfloat` depth); `SandboxPipelineResolver` compiles depth + color pipelines lazily; `smoke_depth_prepass.exe` GPU smoke ✅
- v1d: Default lit material shaders — `engine/renderer/shaders/surface.vert`, `surface.frag`, `assets/materials/default_lit.mat.toml`; standard 48B vertex layout in shaders ✅
- v1e: Sandbox rendering wired to `ForwardRenderPath` + `SandboxPipelineResolver`; orbit-camera view+projection; mesh upload on shape selection; blit color RT → swapchain; ImGui overlay on top ✅
- v1f: Demo glTF assets (BoxTextured CC-BY, Duck SCEA, BoomBox CC0) + checker_512/bricks_512 procedural PNGs (CC0) under `assets/source/`; `cook-demo-assets` CMake target produces `assets/cooked/demo_assets.crdr` (5 entries); `LICENSES.md` ✅
- v1g: Unified `Asset Browser` ImGui panel — replaces Meshgen Browser; "Procedural Shapes" + "Imported Assets" collapsing sections; click swaps mesh; `SandboxLayer` mounts the cooked pack via `ResourceManager` and `load_sync<MeshResource>` for imports ✅

Bug fix landed alongside v1f/v1g: device-destroy crash on application close — `Application::detach_all_layers()` was leaving layer instances alive in `m_owned_layers`, so layer destructors (which free GPU resources) ran during `~Application` *after* the `Device` local in `main` had been destroyed. Fixed by clearing `m_owned_layers` inside `detach_all_layers()`, and re-ordering `sandbox/src/main.cpp` so `device->wait_idle()` precedes `app.detach_all_layers()`. Same fix applied to `runtime/examples/smoke_imgui_overlay.cpp`.

RHI additions in this phase: `Format::R32G32B32A32Sfloat` + VkFormat mapping; `Module::code_bytes()` on shader interface + `StoredModule` impl.
Bugfixes: SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT was mapped to R8G8B8A8Unorm — corrected.

Full design packet: `docs/phases/phase-2.8-material-completion.md`.

Aktif phase dosyası: `docs/phases/phase-2.8-material-completion.md` (active)

## Active detour

_none — running on the main roadmap._

> When a detour opens, this section names it (e.g. "D-001: investigate
> shader-cache corruption") and the main roadmap pauses until it closes.
> Detour file: `docs/detours/D-NNN-<slug>.md`. Queue rules:
> `docs/detours/README.md`.

## Last shipped milestone

**2026-05-06 — Phase 3.0 v1a SHIPPED: `crd-scene` module bootstrapped (EntityId + SlotMap + World shell).**

New module `engine/scene/` (`crd-scene`), depends on `crd-core` + `crd-containers` + `crd-memory`.

API surface (per ADR-0049):
- `crd::scene::EntityId` — 64-bit `[generation:32 | index:32]`, trivially copyable, default-zero is `null()`. `index()`, `generation()`, `is_null()`, `make(idx, gen)`, `null()`. `static_assert(sizeof == 8)`.
- `crd::scene::Slot` — `{ u32 generation; u32 next_free; bool alive; }`. Slot 0 reserved permanently as null sentinel.
- `crd::scene::SlotMap` — owns `Array<Slot>` + free-list head + alive count. `allocate()` (O(1)), `free()` (O(1), bumps generation), `is_alive()`, `alive_count()`, `slot_count()`, `begin()`/`end()` iterator that yields alive entities only and skips holes.
- `crd::scene::World` — wraps `SlotMap` + `Array<EntityId> m_pending_destroy`. `spawn()`, `destroy(e)` (deferred), `destroy_immediate(e)` (synchronous), `flush_destroys()` (drain queue, skip stale handles), `is_alive()`, `entity_count()`, `pending_destroy_count()`, range-for over alive entities.

Two minor divergences from ADR-0049 (defensible, called out for the next reader):
- §5 says `CRD_VERIFY` traps on generation overflow; impl instead silently bumps `0 → 1` in `SlotMap::free` to keep generation 0 reserved as the dead-slot sentinel value. The alive bit prevents handle resurrection within the same frame anyway.
- §4 commentary says `actually_free_slot` asserts on stale handle; impl makes `World::destroy_immediate(e)` lenient (no-op when `!is_alive(e)`) so double-destroy across the deferred queue + immediate path is safe. Pinned by test `destroy_immediate of stale handle is a no-op`.

Tests added (`tests/scene/`, +22 cases, +3448 assertions):
- `test_entity.cpp` (5): default null, `make` round-trip across u32 boundaries, equality/inequality, `is_null` semantics, trivial-copy + 8-byte size invariants.
- `test_slot_map.cpp` (9): fresh map empty, allocate-never-zero, generation collision after free, free-list LIFO reuse, multi-step alloc/free order, mixed alloc/free stress (1000 ops, deterministic seed), iterator skips holes, slot-0 sentinel never alive, out-of-range index dead.
- `test_world.cpp` (8): empty world, spawn/alive, destroy-is-deferred, flush drains all queued, `destroy_immediate` synchronous, lenient stale `destroy_immediate`, double-destroy across flush, iteration after destroy/flush.

Six-configuration green:
- win-debug:          503/503  (was 481, +22 new)
- win-relwithdebinfo: 503/503
- win-release:        500/500  (was 478, +22 new)
- win-asan:           503/503  (DLL PATH fix applied as documented)
- win-clang-cl:       503/503
- win-tidy:           ✅ build clean (no clang-tidy warnings or errors)

All 17 headless smokes pass on every non-tidy config.

## Previous shipped milestone

**2026-05-06 — Phase 3.0 architecture locked: nine ADRs accepted, 14 slices planned.**

ADRs accepted (in `Accepted` status under `docs/decisions/`):
- ADR-0049 — L1 Entity identity & SlotMap (32:32 EntityId, dense slot map, deferred destroy)
- ADR-0050 — L2 Storage backends (ArchetypeChunk + SparseSet hybrid behind `IStorageBackend`)
- ADR-0051 — L3 Relations as first-class (`Relation<Tag>` with ChildOf/AttachedTo/generic)
- ADR-0052 — L4 Query · System · Schedule (composable DSL, ISystem with Reads/Writes, phase scheduler, command buffers)
- ADR-0053 — L5 Component index slot framework (`IComponentIndex`, ChangeDetect+AsyncAware ship; History/SpatialBVH/GpuResident API only)
- ADR-0054 — Transform hierarchy update (TRS authored, world cached, push dirty propagation, single-thread serial v1)
- ADR-0055 — Scene serialization (TOML authoring → SCEN CRDR cooked; closes ADR-0020's FlatBuffers vs Cap'n Proto deferral with "neither — CRDR")
- ADR-0056 — L6–L8 reserved slots (Replication, Scripts, Reflection — registration grammar accepts traits, impls defer to 4.2 / 4.0 / 7)
- ADR-0057 — UI in scene tree boundary (`ControlNodeTag` reserved, all UI components live in `crd-ui`)

Also updated:
- `docs/phases/phase-3.0-scene-ecs.md` — rewritten around the 8-layer architecture and 14 slices, with explicit "ships now / API only" matrix per layer
- `docs/decisions/README.md` — ADR index extended with 9 new entries
- `docs/ROADMAP.md` — Phase 2.8 marked shipped; Phase 3.0 row added with link to architecture

## Previous shipped milestone

**2026-05-06 — `crd-math` interpolation primitives + Penner easing curves shipped.**

`crd-math/scalar.hpp` extended with the standard interpolation family:
- `lerp(a, b, t)`, `mix(a, b, t)` (GLSL alias), `saturate(x)`, `step(edge, x)`
- `smoothstep(e0, e1, x)` (Hermite C¹), `smootherstep(e0, e1, x)` (Perlin C²)
- `inverse_lerp(a, b, x)`, `remap(x, ia, ib, oa, ob)`
- `damp(a, b, lambda, dt)` — frame-rate-independent exponential approach

`crd-math/vec.hpp` extended:
- `lerp` for `Vec2/3/4` already existed; added `mix` aliases and `damp` componentwise overloads.

New header `crd-math/easing.hpp` — full Penner easing family (31 functions), all `T t ∈ [0,1] → T`:
- `ease_linear`
- `ease_in_*` / `ease_out_*` / `ease_in_out_*` for: Sine, Quad, Cubic, Quart, Quint, Expo, Circ, Back, Elastic, Bounce.
- Polynomial families (Quad/Cubic/Quart/Quint, Back, Bounce, Linear) are `constexpr noexcept`. Trig/exp families (Sine, Expo, Circ, Elastic) are `inline noexcept` (C++20 doesn't make `<cmath>` `constexpr` yet — moves to constexpr automatically when we adopt C++26).
- Curves are decoupled from value type: combine with `lerp` at the call site, e.g. `lerp(a, b, ease_out_cubic(t))`. No `Tween` class, no `EasingChannel`, no animation runtime.

`crd-math/math.hpp` umbrella includes `easing.hpp`.

Sandbox migration:
- Removed file-local `exp_lerp` / `exp_lerp3` helpers from `sandbox_layer.cpp`.
- `OrbitCamera` smoothing now uses `crd::math::damp` for both scalar (`s_dist`) and vector (`s_target`) channels.

Tests added (`tests/math/test_math.cpp`, +8 cases):
- scalar lerp/mix/saturate/step/inverse_lerp/remap with extrapolation past `t > 1`.
- smoothstep / smootherstep boundary saturation, midpoint exactness, monotonicity over 33 sample points.
- `damp` identity at `dt = 0`, convergence at large `dt`, frame-rate-stability cross-check (60 ticks at `dt = 1/60` ≈ 1 tick at `dt = 1`).
- Vec3 lerp / mix / damp componentwise.
- All 31 easings: `f(0) ≈ 0` and `f(1) ≈ 1` boundary anchors.
- In/Out reflection identity for the strictly monotone families.
- Monotone non-decreasing on [0,1] for Sine/Quad/Cubic/Quart/Quint/Circ/Expo (sampling 65 points each).
- Back undershoot, Elastic overshoot, Bounce stays in [0, 1].

Six-configuration green:
- win-debug:          481/481
- win-relwithdebinfo: 481/481
- win-release:        478/478
- win-asan:           481/481
- win-clang-cl:       481/481
- win-tidy:           ✅ (build clean)

## Previous shipped milestone

**2026-05-06 — Phase 2.8 v1f + v1g SHIPPED: glTF demo asset bundle + cook-demo-assets target + unified Asset Browser panel; device-destroy crash fix. Phase 2.8 COMPLETE.**

Source asset bundle (`assets/source/`):
- `BoxTextured.glb` (5 KB, CC-BY 4.0, Cesium/Khronos),
- `Duck.glb` (118 KB, SCEA Shared Source 1.0, Sony/Khronos),
- `BoomBox.glb` (10 MB, CC0, UX3D/Khronos),
- `checker_512.png` + `bricks_512.png` (procedural CC0, generated by `generate_textures.ps1`),
- `LICENSES.md` documenting per-file license terms.

`cook-demo-assets` CMake target (`sandbox/CMakeLists.txt`):
- `add_custom_command(OUTPUT demo_assets.crdr COMMAND $<TARGET_FILE:asset_cooker> cook ...)` with explicit DEPENDS on each source file — recooks on source change.
- `crd-sandbox` adds `add_dependencies(crd-sandbox cook-demo-assets)` so building sandbox triggers a cook; `CRD_DEMO_ASSETS_PACK` compile def points sandbox to the cooked pack.
- Verified output: 5 manifest entries (3 MESH + 2 TXTR), stable UUIDs across re-cooks.

`Asset Browser` panel (`sandbox/src/sandbox_layer.cpp`):
- Replaces former "Meshgen Browser". Two collapsing sections: **Procedural Shapes** (8 meshgen entries) and **Imported Assets** (3 glTF entries when the pack is mounted).
- `SandboxLayer` owns a `ResourceManager`; mounts `CRD_DEMO_ASSETS_PACK` and registers `MeshResourceLoader`. Reads each `<file>.glb.meta` sidecar to recover the cooker-minted UUID.
- Click an imported entry → `load_sync<MeshResource>` → `GpuUploader::upload_mesh()` → swap. Per-selection metadata: vertex count, index count, triangle count, source ("Procedural" or "glTF").
- Graceful fallback: missing pack / missing meta sidecar / UUID-not-in-pack each log a warning and hide the affected entry rather than aborting.

Crash fix:
- `Application::detach_all_layers()` now clears `m_owned_layers` — previously it cleared the stack but kept the unique_ptrs alive. Their destructors ran during `~Application` *after* the `Device` local in `main` had been destroyed, producing a use-after-free during VK destroy calls in layer dtors.
- `sandbox/src/main.cpp` reordered: `device->wait_idle()` is now called *before* `app.detach_all_layers()` (was the other way around). `smoke_imgui_overlay.cpp` reordered identically.

Other API changes:
- `GpuUploader::upload_texture` and `upload_mesh` now take `const&` (was non-const&). Required so `load_sync<T>::handle.get()` (returns `const T*`) can be passed directly. No behavioural change — both functions only read the CPU resource.

Six-configuration green:
- win-debug:          473/473
- win-relwithdebinfo: 473/473
- win-release:        470/470
- win-asan:           473/473
- win-clang-cl:       473/473
- win-tidy:           ✅ (build clean)

Headless smokes (17/17 across all five non-tidy configs): `smoke_config`, `smoke_containers`, `smoke_filesystem`, `smoke_frame_clock`, `smoke_jobs`, `smoke_log`, `smoke_math`, `smoke_memory`, `smoke_shader`, `smoke_resources`, `smoke_resources_async`, `smoke_resources_reload`, `smoke_resources_stream`, `smoke_resources_render`, `smoke_texture`, `smoke_mesh`, `smoke_material`. `crd-sandbox --headless` exits 0 with the demo pack mounted (5 entries, mount_id=1).

## Previous shipped milestone

**2026-05-05 — Phase 2.8 v1a–v1e SHIPPED: per-material pipeline cache + multi-pass + depth prepass + default lit shaders + sandbox 3D rendering.**

Key changes across v1a–v1e:

**ForwardRenderPath (`engine/renderer/src/forward_render_path.cpp`):**
- `get_or_compile_mat_pipelines()` — lazy pipeline cache keyed by `MaterialTemplate*`; compiles depth-only (vertex-only, `Undefined` color, `D32Sfloat` depth) and color (vert+frag, `B8G8R8A8Unorm`) pipelines from `ShaderResource::spirv` directly.
- `PipelineResolver::begin_pass(PassType)` default no-op added to interface; `ForwardRenderPath` calls `m_resolver->begin_pass()` before each draw loop to inform resolvers of the current pass.
- Material-path branching: `DrawItem::material != nullptr` uses the compiled cache; legacy items (null material) continue to use the `PipelineResolver`.

**Renderer (`engine/renderer/`):**
- `DrawItem::material` + `Renderable::material` fields added (`const MaterialInstance* material = nullptr`).
- `build_frame()` validation relaxed: accepts `material != nullptr` even when `variant` is invalid; copies material pointer into `DrawItem`.

**Shader (`engine/shader/`):**
- `Module::code_bytes()` pure virtual added to interface; `StoredModule` implements it via word-buffer reinterpret. Required so `SandboxPipelineResolver` can extract SPIR-V bytes from a compiled module without going through the resource system.

**RHI (`engine/rhi/`, `engine/rhi-vulkan/`):**
- `Format::R32G32B32A32Sfloat` added + VkFormat mapping.
- SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT was wrongly returning `R8G8B8A8Unorm` — corrected to `R32G32B32A32Sfloat`.

**Default lit shaders (`engine/renderer/shaders/`):**
- `surface.vert`: reads 48B interleaved vertex (pos/normal/uv0/tangent at locations 0–3); writes `VertexAttrs` (position_ws, normal_ws, uv0, tangent_ws) to fragment.
- `surface.frag`: implements `crd_evaluate_surface()` with default-lit values; Lambertian diffuse with hardcoded directional light.
- `assets/materials/default_lit.mat.toml`: domain=Surface, alpha_mode=Opaque.

**Sandbox (`sandbox/src/`):**
- `SandboxPipelineResolver`: compiles depth + color pipelines lazily from `Module::code_bytes()` on first `resolve_pipeline` call; `begin_pass()` tracks current pass.
- `SandboxLayer`: holds `ForwardRenderPath`, `GpuMesh`, `Renderer`; `render_scene()` builds camera matrices (local `look_at` + reverse-Z `perspective`), submits legacy-path renderable with `m_surface_variant`, runs frame graph, blits color RT → swapchain (ColorWrite→TransferSrc→blit→TransferDst→ColorWrite).
- `smoke_depth_prepass.exe`: GPU/window smoke — creates FRP, submits empty draw list, executes one frame, exits 0.

Six-configuration green:
- win-debug:          471/471
- win-relwithdebinfo: 471/471
- win-release:        468/468
- win-asan:           471/471
- win-clang-cl:       471/471
- win-tidy:           ✅ (build clean)

## Previous shipped milestone (-1)

**2026-05-05 — Phase 2.7 v1e SHIPPED: `crd-meshgen` + sandbox Meshgen Browser. Phase 2.7 COMPLETE.**

New module `engine/meshgen/`: 8 CPU-side procedural geometry generators. `smoke_meshgen.exe` headless. 11 unit tests. `crd-sandbox` Meshgen Browser ImGui panel. Six-configuration green (468/468 win-debug).

## Previous shipped milestone (–2)

**2026-05-05 — Phase 2.7 v1c SHIPPED: Full material system foundation (ADR-0048).**

New headers: `engine/renderer/include/crd/renderer/material_domain.hpp` (`MaterialDomain` enum: Surface/PostProcess/Compute/Decal/UI), `engine/renderer/include/crd/renderer/pass_type.hpp` (`PassType` enum: DepthPrepass=0/Shadow=1/Forward=2; `AlphaMode`, `CullMode`, `FillMode`, `BlendMode`, `RasterState` — 8 bytes per state), `engine/renderer/include/crd/renderer/material_template.hpp` (`ParameterType` enum, `CookedParameter` 24-byte struct, `ShaderOptionDecl` 16-byte struct, `PassShaderPair` {vert + frag `ResourceHandle<ShaderResource>`}, `MaterialTemplate` with `Array<CookedParameter> parameters`, `Array<u8> defaults_blob`, `PassShaderPair pass_shaders[3]`, `RasterState pso_states[3]`, `Array<ShaderOptionDecl> options`, `MaterialInstance` with `set_float`/`set_vec4` binary-search parameter write + `variant_for_pass` fallback).

Renames: `MaterialLayout` → `MaterialBindLayout`, `MaterialInstance` (transient GPU binding) → `MaterialBindGroup` (both in `engine/renderer/include/crd/renderer/material.hpp`). `MaterialResource` struct removed from `material_resource_loader.hpp` — replaced by `MaterialTemplate` in new header.

New MATR v2 artifact format: INFO chunk (4 bytes: loader_version, domain, flags, pad) + PASS chunk (4-byte count + N × 36-byte entries: pass_type u8, pad[3], vert_id u8[16], frag_id u8[16]). PRMS/DFLT/PSOS/OPTS chunks defined but not yet emitted by cooker (reserved for Phase 2.8). CRDR FourCCs added: `kFourCC_INFO`, `kFourCC_PRMS`, `kFourCC_DFLT`, `kFourCC_PASS`, `kFourCC_PSOS`, `kFourCC_OPTS`.

`MaterialResourceLoader` rewritten (version → 2): reads INFO → domain, PASS → `pass_shaders[]` indexed by `PassType` ordinal (load_sync<ShaderResource> for each vert+frag pair), falls back to legacy META chunk (synthesizes Forward entry) for backward-compat. Cooker rewritten (version → 2): emits INFO + PASS chunks; parses `[passes.forward]` and `[passes.depth_prepass]` TOML sections; legacy flat `vertex_shader`/`fragment_shader` keys treated as Forward pass.

5 new tests in `tests/resources/test_shader_material_loaders.cpp` tagged `[v1c]`: v2 PASS chunk round-trip, two-pass material (fwd + depth), missing PASS+META → Failed, MaterialInstance set_float round-trip, MaterialInstance variant_for_pass fallback. `smoke_material.exe` (headless, 2 scenarios: v2 + MaterialInstance + legacy META, exit 0).

Six-configuration green:
- win-debug:          457/457
- win-relwithdebinfo: 457/457
- win-release:        454/454
- win-asan:           457/457
- win-clang-cl:       457/457
- win-tidy:           ✅ (build clean)

## Previous shipped milestone (–1)

**2026-05-05 — Phase 2.7 v1b SHIPPED: `MeshResource` + cgltf glTF import + MikkTSpace tangent generation.**

`MeshPrimitive` + `MeshResource` in `engine/renderer/include/crd/renderer/mesh_resource.hpp`. Interleaved 48-byte vertex layout: float3 pos (0–11) + float3 normal (12–23) + float2 uv0 (24–31) + float4 tangent (32–47, w = bitangent sign). `MeshResourceLoader` (`engine/renderer/src/mesh_resource_loader.cpp`): reads `type='MESH'` CRDR artifact, parses VERT + INDX + PRIM chunks. PRIM chunk is 4-byte count + N × 32-byte entries (vertex_count, index_count, vertex_byte_offset, index_byte_offset, u8[16] material_id). Registered via `crd::renderer::register_mesh_loader(rm)`.

glTF cooker handler in `tools/asset_cooker/src/cook_handlers/mesh.cpp`: cgltf for parsing (CPM DOWNLOAD_ONLY + INTERFACE; header + impl in same TU), MikkTSpace for tangent generation (CPM DOWNLOAD_ONLY + INTERFACE; `mikktspace.c` included inline within `extern "C"` block — avoids C compiler detection in a LANGUAGES CXX-only project). Static meshes only (no skinning/morph targets). Reads POSITION/NORMAL/TEXCOORD_0/TANGENT accessors; generates tangents via `genTangSpaceDefault()` if TANGENT absent. Indices always upcast to u32. First glTF mesh → main `CookResult`; additional meshes → `ExtraArtifact` array (new `CookResult::extra_artifacts` field, backward-compatible; each extra artifact gets a `.mesh.<name>.meta` sidecar). Registers `.glb` + `.gltf`. `cook_command.cpp` extended to process extra_artifacts loop.

CRDR FourCCs added: `kFourCC_MESH`, `kFourCC_VERT`, `kFourCC_INDX`, `kFourCC_PRIM` (in `crdr.hpp`).

4 new tests in `tests/resources/test_mesh_loader.cpp`: MESH artifact round-trip, multi-primitive count, missing VERT → Failed, GLB cook + load round-trip (hand-assembled 152-byte binary GLB). `smoke_mesh.exe` (headless, 2-primitive MESH artifact, mounts, loads, verifies primitive sizes + null material UUIDs, exit 0).

Six-configuration green:
- win-debug:          452/452
- win-relwithdebinfo: 452/452
- win-release:        449/449
- win-asan:           452/452
- win-clang-cl:       452/452
- win-tidy:           ✅ (build clean)

## Previous shipped milestone (–2)

**2026-05-04 — Phase 2.7 v1a SHIPPED: `TextureResource` + stb_image texture cooker.**

`TextureResource` + `MipLevel` in `engine/renderer/include/crd/renderer/texture_resource.hpp`. `TextureFormat` enum (RGBA8Unorm/BC7Unorm/BC7UnormSrgb — on-disk byte values, never reorder). `TextureResourceLoader` (in `engine/renderer/src/texture_resource_loader.cpp`): reads `type='TXTR'` CRDR artifact, parses 16-byte `HEAD` chunk (width u32, height u32, mip_count u32, format u8, padding[3]), validates dims/format/mip count (max 16), reads and validates per-mip pixel size for RGBA8, copies mip pixel data into `MipLevel::pixels`. Registered via `crd::renderer::register_texture_loader(rm)`.

Texture cook handler in `tools/asset_cooker/src/cook_handlers/texture.cpp`: stb_image for decode (STBI_rgb_alpha → 4 channels, TGA BGRA→RGBA swap handled by stb), box-filter mip chain generation to 1×1 with ping-pong scratch buffers (O(W×H) memory), writes HEAD chunk + MIP0..MIPn chunks. Registers `.png`/`.jpg`/`.jpeg`/`.tga`/`.bmp` via `register_texture_handler()`, called from `register_builtin_handlers()`.

CRDR FourCCs added to `crdr.hpp`: `kFourCC_TXTR`, `kFourCC_HEAD`, `kFourCC_MIP0`–`kFourCC_MIP15` (via `make_mip_fourcc()`). 4 new tests; `smoke_texture.exe`. Six-configuration green (448/448 win-debug).

## Previous shipped milestone (–2)

**2026-05-04 — Phase 2.6 v1g SHIPPED: load_streamed + 2Q LRU eviction + memory budget + pinning. Phase 2.6 COMPLETE.**

5 new tests in `tests/resources/test_eviction.cpp`. `smoke_resources_stream.exe`. Six-configuration green (444/444 win-debug).

## Previous shipped milestone (–1)

**2026-05-04 — Phase 2.6 v1f shipped: hot-reload — mtime polling, atomic payload swap, callbacks.**

`ResourceControlBlock::payload` made `std::atomic<void*>`. `poll_hot_reload(debounce_ms)` polls mounted PACK files. `reload_mount_now(MountId)` forces reload bypassing mtime. `subscribe_reload` / `unsubscribe_reload`. Deferred-free grace period. 4 new unit tests in `test_hot_reload.cpp`. `smoke_resources_reload.exe`.

Six-configuration green:
- win-debug:          439/439
- win-relwithdebinfo: 439/439
- win-release:        436/436
- win-asan:           439/439
- win-clang-cl:       439/439
- win-tidy:           439/439

## Previous shipped milestone (–1)

**2026-05-04 — Phase 2.6 v1e shipped: ShaderResourceLoader + MaterialResourceLoader + end-to-end cooked render smoke.**

`ShaderResourceLoader` (`engine/shader/src/shader_resource_loader.cpp`, registered via `crd::shader::register_shader_loader(rm)`): reads SPVV/SPVF/SPVC chunk from a `type='SHDR'` artifact to determine stage, copies SPIRV bytes into `ShaderResource::spirv`, then drives spirv-reflect to populate `descriptor_bindings`, `push_constants`, and (for vertex stage) `vertex_attributes`. Version 1. Clang-cl fix: removed dead `to_parameter_class_local` helper (caught by `-Werror,-Wunused-function`; MSVC `/W4 /WX` doesn't flag unused statics).

`MaterialResourceLoader` (`engine/renderer/src/material_resource_loader.cpp`, registered via `crd::renderer::register_material_loader(rm)`): reads 32-byte META chunk from a `type='MATR'` artifact, extracts vert/frag `ResourceId` pairs, calls `ctx.manager->load_sync<ShaderResource>(id)` transitively for each, builds a `MaterialResource` holding both handles. Version 1.

`compile_glsl()` free function (`engine/shader/src/compile.cpp`): shaderc-backed GLSL→SPIRV helper usable in tests and the cooker without pulling in the full shader runtime. `.glsl` cooker handler: emits `type='SHDR'` CRDR with a SPVV/SPVF/SPVC chunk. `.mat.toml` cooker handler: parses TOML vert/frag source-path references, looks up UUIDs from adjacent `.meta` sidecars, emits `type='MATR'` CRDR with 32-byte META chunk.

`smoke_resources_render.exe`: cooks one `.vert.glsl` + one `.frag.glsl` inline, assembles them into a PACK with a MATR artifact, mounts, calls `load_sync<MaterialResource>`, asserts both shader handles are Ready, prints SPIRV sizes, exits 0. Output: `smoke_resources_render: OK — MaterialResource loaded with vert+frag SPIRV (vert=1040 bytes, frag=572 bytes)`.

Six new tests in `tests/resources/test_shader_material_loaders.cpp`: vertex SHDR round-trip, fragment SHDR round-trip, missing SPIRV chunk → Failed, material loads + resolves deps (verifies transitive cache and `handle_count() == 3`), missing META → Failed, real SPIRV round-trip via `compile_glsl()` (shaderc-dependent, skips gracefully if unavailable).

Six-configuration green:
- win-debug:          435/435
- win-relwithdebinfo: 435/435
- win-release:        432/432
- win-asan:           435/435
- win-clang-cl:       435/435
- win-tidy:           435/435

## Previous shipped milestone

**2026-05-04 — Phase 2.6 v1d shipped: AsyncFile + load_async<T> + fiber-cooperative wait_ready().**

`crd::platform::AsyncFile` (`engine/platform/`): job-pool async file reads. `open()` returns an AsyncFile with `is_open()`/`size()`. `read_async(offset, span)` submits a `crd-jobs` job and returns a `Counter*`; returns `nullptr` if `offset + size > file_size`. Windows backend uses `ReadFile` inside a SBO-compatible `ReadJob` (40 bytes). `crd-platform` gains a PRIVATE link dep on `crd-jobs`.

`ResourceManager::load_async<T>`: heap-allocates `AsyncLoadCtx`, submits via 8-byte `LoadJobFn` closure (within SBO limit). `m_in_flight` HashMap (keyed by ResourceId) prevents duplicate I/O when concurrent calls race for the same id. `m_mutex` released before all I/O and loader dispatch — enables recursive `load_sync` transitive dep resolution without deadlock. `run_load_job` made `public` in `ResourceManager` so the anonymous-namespace closure can call it. Counter leak fix: after storing counter in `block->load_counter`, if state is already terminal, immediately reclaim+wait.

`ResourceHandleBase::wait_ready()`: atomically exchanges `block->load_counter` (first caller claims it), calls `crd::jobs::wait()` for fiber-cooperative suspension. Terminal-state fast path also attempts exchange before returning (covers job-completes-before-store race). Spin+yield fallback for non-fiber callers. Moved to `resource_handle.cpp` (with `release_block()`) so headers don't pull in `jobs.hpp` or `loader.hpp`.

`smoke_resources_async.exe`: end-to-end async round-trip (assemble PACK, mount, `load_async<BlobResource>`, `wait_ready()`, verify 5 bytes, exit 0). Nine new tests: 4 `[platform][async_file]` in `tests/platform/test_async_file.cpp`, 5 `[resources]` load_async tests in `test_resource_manager.cpp`.

Six-configuration green:
- win-debug:          429/429
- win-relwithdebinfo: 429/429
- win-release:        426/426
- win-asan:           429/429
- win-clang-cl:       429/429
- win-tidy:           429/429

## Previous shipped milestone (–1)

**2026-05-03 — Phase 2.6 v1c shipped: RefCounted<T> + ResourceHandle<T> + load_sync<T> + cycle detection + smoke_resources.**

`crd::memory::RefCounted<T>` CRTP intrusive refcount. `ResourceControlBlock`, `ResourceHandleBase`, `ResourceHandle<T>`, thread-local cycle detection, `load_sync_impl`, `make_failed_block()`, `read_file_range()`, `smoke_resources.exe`. 20 new tests.

Six-configuration green:
- win-debug:          420/420
- win-relwithdebinfo: 420/420
- win-release:        417/417
- win-asan:           420/420
- win-clang-cl:       420/420
- win-tidy:           420/420

## Previous shipped milestone (–1)

**2026-05-03 — Phase 2.6 v1b shipped: cooker CLI + zstd compression.**

zstd v1.5.5 wired as per-chunk opt-in in `CrdrWriter::add_chunk_compressed()` (level 3 default; falls back to uncompressed if compression doesn't help). Two-pass reader in `crdr_read()` pre-allocates `decompressed_backing` before decompression loop (no span invalidation). `CrdrError::DecompressFailed` added; `main.cpp` switch updated.

`crd-cooker` static library split from `asset_cooker` executable (tests can link it directly). New headers: `cook_handler.hpp` (CookContext, CookResult, CookHandlerFn), `cook_command.hpp` (cmd_cook). `cmd_cook()`: recursive directory scan (excludes .meta + .cook_cache/), sorted for determinism, .meta sidecar mint/read, FNV1a-64 source hash, cook_key = source_hash ^ handler_version stored in `.cook_cache/<uuid>.key`, artifact stored in `.cook_cache/<uuid>.crdr`, two-pass PACK assembly (pass 1 measures CRDR size, pass 2 fills real blob_offsets), `cook.log.toml` written adjacent to the pack. `blob_passthrough_handler` for `.bin` files. Optional CMake `cook` target (CRD_COOK_ROOT + CRD_COOK_OUT). 4 new tests: registry, .bin round-trip, zstd round-trip, integration (10 files → 10 entries, byte-identical second run, "skipped" log entries).

Six-configuration green:
- win-debug:          408/408
- win-relwithdebinfo: 408/408
- win-release:        405/405
- win-asan:           408/408
- win-clang-cl:       408/408
- win-tidy:           408/408

## Previous shipped milestone

**2026-05-03 — Phase 2.6 v1a shipped: `crd-resources` + `asset_cooker` manifest_dump.**

`ResourceId` (UUID v4 via mt19937_64, UUID v5 via SHA-1 SHA-1 + Cerid namespace, parse/to_string,
36-char hyphenated format). CRDR chunked binary container (reader + writer, chunk sort, 16-byte
padding, LE serialization). `ManifestEntry` 48-byte disk format (MFST/STRP/DEPS chunks).
`ResourceManager` shell: `register_loader`, `mount_manifest` (reads CRDR PACK, populates live
index, newest-mount-wins collision), `unmount` (by MountId). `asset_cooker manifest_dump` CLI
sub-command. 38 new tests across three test files.

Also fixed: `crd-containers String` SSO encoding changed to remaining-capacity (`size_or_flag =
kSsoCapacity - size`) to eliminate `buf[kSsoCapacity]` UB exposed by new MSVC 14.50.35717
optimizer. A 23-char SSO string now has `size_or_flag = 0 = '\\0'` which doubles as the null
terminator, so `c_str()` is always correct and no out-of-bounds array access occurs.

Six-configuration green:
- win-debug:          393/393
- win-relwithdebinfo: 393/393
- win-release:        390/390
- win-asan:           393/393
- win-clang-cl:       393/393
- win-tidy:           393/393

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1k integration smoke + crd-app wiring shipped. Phase 2.5 COMPLETE.**

`smoke_jobs.cpp` rewritten from raw fiber demo (v1a) to full public API exercise: `init/shutdown`,
`run+wait`, `parallel_for` (1 000-element sum), H/N/L priority all-ran (10/20/40 jobs),
`frame_alloc/frame_reset`. All sections PASS, exit 0.

`Application::run()` now calls `crd::jobs::init(m_desc.jobs_config)` before the tick loop and
`crd::jobs::shutdown()` after, guarded by `if (!m_valid) return`. `ApplicationDesc` gained
`crd::jobs::Config jobs_config{}`. `crd-app` CMakeLists links `crd-jobs` PUBLIC.
`smoke_renderer` verified clean (exit 0) with the wired Application.

Six-configuration green:
- win-debug:          355/355
- win-relwithdebinfo: 355/355
- win-release:        352/352
- win-asan:           355/355
- win-clang-cl:       355/355
- win-tidy:           355/355 (exit 0)

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1h Public API layer shipped.**

`engine/jobs/include/crd/jobs/jobs.hpp` — full public API: `Config`, `init()`, `shutdown()`,
`run(span)` / `run(single)`, `wait()`, `run_and_wait(span)` / `run_and_wait(single)`,
`is_worker_fiber()`, `worker_index()`, `num_workers()`.

Key design decision: counter pointer stored in `Fiber::job_counter` (not TLS) so it survives
fiber suspension. 5 public-API tests. Total at v1h: 346 tests.

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1g Worker thread pool + main-thread fiber shipped.**

`WorkerPool` class in `engine/jobs/src/worker_pool.hpp/.cpp`: owns `Scheduler`, `FiberPool`, and
`CounterPool`; spawns N-1 background worker threads (indices 1..N-1) each running `worker_loop`;
thread 0 driven by `pump()` for main-thread use. All jobs run inside fiber context switches via
`job_fiber_trampoline` (looping entry fn burned into every fiber stack at pool init).

Key fix: `fiber_switch_win64.asm` now saves/restores GS:[8] (StackBase) and GS:[16] (StackLimit)
alongside RSP, so `__chkstk` and guard pages work correctly when switching between fiber stacks.
`FiberContext` extended with `tib_stack_base` / `tib_stack_limit` on Windows. `fiber_context.hpp`
now explicitly includes `platform.hpp` (was relying on PCH ordering).

Key fix: fiber reuse was broken — snapshot copy `target->context = target->initial_ctx` was
corrupted because `fiber_switch`'s register saves (push r14...) overwrite the initial frame data
on the fiber stack. Fix: call `fiber_init_stack` on completion to rebuild a fresh initial frame.
`Fiber` struct: `initial_ctx` field removed; `usable_base`, `usable_size`, `trampoline` fields
added so `WorkerPool` can re-initialize without pool context.

10 unit tests in `tests/jobs/test_jobs.cpp`: init/shutdown, re-init, default thread count,
single worker job, pump on thread 0, pinned job via pump, multiple jobs, fiber stack isolation,
pump empty probe, 1000-job concurrent stress. Also fixed pre-existing clang-tidy warnings in
`test_scheduler.cpp` (u→U suffix, member m_ prefix, braces-around-statements).

Six-configuration green:
- win-debug:          341/341
- win-relwithdebinfo: 341/341
- win-release:        338/338
- win-asan:           341/341
- win-clang-cl:       341/341
- win-tidy:           341/341

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1f Counter + wait mechanism shipped.**

`Counter` (alignas(64), one cache line: `atomic<u32> value`, `atomic<Waiter*> waiters`, pool
metadata) + `CounterPool` (generation-tagged 64-bit Treiber free list `[gen:32|idx:32]`, same
pattern as FiberPool). `Waiter` struct: `Fiber* fiber`, `u32 target`, `atomic<bool> canceled`,
`atomic<Waiter*> next`. `counter_decrement` steals waiter list atomically; `counter_wait` is a
6-step ABA-safe protocol (fast path, Treiber push, double-check, fiber_switch suspend). 14 unit
tests incl. real fiber_switch suspension + 16-thread concurrent stress.

Six-configuration green:
- win-debug:          331/331
- win-relwithdebinfo: 331/331
- win-release:        328/328
- win-asan:           331/331
- win-clang-cl:       331/331
- win-tidy:           331/331

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1e Priority Scheduler shipped.**

`Scheduler` class: three global Vyukov MPMC injection queues (High/Normal/Low), per-thread three
Chase-Lev local deques, and a single-slot pinned-job lane per thread. Drain order: pinned →
H-inject → H-local → H-steal → N-inject → N-local → N-steal → L-inject → L-local → L-steal.
`std::counting_semaphore<>` sleep/wake. 16 unit tests.

Detail: `docs/sessions/2026-05-02-jobs-v1e-scheduler.md`.

Six-configuration green:
- win-debug:          317/317  
- win-relwithdebinfo: 317/317  
- win-release:        314/314  
- win-asan:           317/317  
- win-clang-cl:       317/317  
- win-tidy:           317/317  

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1d Vyukov MPMC injection queue shipped.**

`MpmcQueue<T>` header-only template implementing the Vyukov bounded MPMC queue algorithm
(Dmitry Vyukov, 1024cores.net). Producers and consumers each have their own `alignas(64)`
atomic position counter (separate cache lines) to prevent false sharing. Each ring-buffer
cell holds an `atomic<u64>` sequence and one T. The sequence handshake uses `acquire`/`release`
orderings: producers read sequence with acquire and publish with release; consumers mirror
this. enqueue() returns false (non-blocking) when full; dequeue() returns false when empty.
Capacity must be a power of two.

10 unit tests: construction, single-item round-trip, empty returns false, full returns false,
FIFO ordering, wrap-around across 3 full laps, SPSC/MPSC/SPMC/MPMC concurrent stress
(each verifying all items consumed exactly once).

Six-configuration green:
- win-debug:          301/301
- win-relwithdebinfo: 301/301
- win-release:        298/298
- win-asan:           301/301
- win-clang-cl:       301/301
- win-tidy:           301/301

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1c Chase-Lev work-stealing deque shipped.**

`WorkStealingDeque<T>` header-only template implementing the Lê et al. 2013
algorithm ("Correct and Efficient Work-Stealing for Weak Memory Models").
Owner thread uses push() (LIFO via `m_bottom`) and pop(); any thread calls
steal() (FIFO via `m_top`). Fixed power-of-two capacity; indices are `i64`
monotonically increasing counters masked with `& (capacity-1)`.

Memory ordering: push uses `release` on `m_bottom`; pop uses `seq_cst` on
`m_bottom`-store + `seq_cst` on `m_top`-load + `seq_cst` CAS for the last-
element race; steal uses `acquire` load of `m_top`, `seq_cst` fence (required
for correctness on weak memory models per Lê et al. Thm 1), `acquire` load
of `m_bottom`, then `seq_cst` CAS.

`m_bottom` and `m_top` on separate `alignas(64)` cache lines to prevent
false sharing between owner and thieves. `CRD_COMPILER_MSVC` pragma suppresses
C4324 (structure padded due to alignment specifier).

12 tests: LIFO/FIFO ordering, exhaustion assertion, last-element race
(4 000 trials, each must give exactly one winner), two concurrent stress tests
(pre-fill + concurrent drain; concurrent push + pop + steal with back-pressure).

Six-configuration green:
- win-debug:          291/291
- win-relwithdebinfo: 291/291
- win-release:        288/288
- win-asan:           291/291
- win-clang-cl:       291/291
- win-tidy:           291/291

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1b fiber pool shipped.**

Three-tier fiber pool (Small 64 KB × 128, Medium 512 KB × 64, Large 2 MB × 16).
Platform stack allocation: VirtualAlloc (Windows) / mmap+mprotect (Linux) with
an uncommitted guard page below each stack — overflow crashes immediately.
Lock-free Treiber free list per tier using a tagged 64-bit head
`[gen:32 | idx:32]`; generation is bumped on every pop, making ABA structurally
impossible without CMPXCHG16B. `alignas(64)` on `Tier` struct prevents inter-tier
false sharing. Debug-only explicit state machine: `Idle / Active / Waiting / Ready`
with asserted transitions at acquire/release (Waiting/Ready stubs ready for v1f).
Peak-usage watermark tracked per tier for profiling. Trampoline is injected at
pool creation (looping, zero re-init cost on the hot acquire path).
13 new tests; concurrent 4-thread × 8 000-iteration ABA stress test included.

Also fixed in this session: renderer LTO miscompilation under win-relwithdebinfo
(`FrameGraph::build()` local-array tracking moved to `ImageResource` members;
`ForwardRenderPath` lambda captures changed from `&draw_list` to `m_draw_list`
via new member pointer — both were MSVC `/GL` miscompile sites).

Six-configuration green for the first time:
- win-debug:          279/279
- win-relwithdebinfo: 279/279
- win-release:        276/276
- win-asan:           279/279
- win-clang-cl:       279/279
- win-tidy:           279/279

## Previous shipped milestone

**2026-05-01 — `crd-jobs` v1a hand-rolled asm context switch shipped.**

`fiber_switch` in Windows x64 MASM and Linux x86-64 AT&T assembly: saves/restores
all callee-saved registers mandated by each ABI (Windows: RBX RBP RDI RSI R12–R15
XMM6–XMM15 MXCSR FCW; Linux: RBX RBP R12–R15). `fiber_init_stack` in C++ sets up
the initial stack frame so the first `fiber_switch` to a fresh fiber jumps to the
entry function; a sentinel `fiber_abort` return-address catches runaway fibers.
5 unit tests: round-trip, multiple re-entries, stack-local data survives,
callee-saved registers verified, two independent fibers.

Detail: (session combined with v1b above).

## Previous shipped milestone (–2)

**2026-05-01 — `crd-renderer` v1i swapchain blit + first full frame loop shipped.**

`CommandBuffer::blit_image(src, dst, src_extent, dst_extent)` added to RHI interface
and implemented in `VulkanCommandBuffer` via `vkCmdBlitImage` with `VK_FILTER_LINEAR`.
Swapchain creation now sets `VK_IMAGE_USAGE_TRANSFER_DST_BIT`. `ForwardRenderPath`
color image adds `TransferSrc` usage. New free function `add_swapchain_blit_pass`
(in `crd/renderer/swapchain_blit.hpp`) adds two frame graph passes per frame:
"swapchain-blit" (ColorWrite→TransferSrc, Undefined→TransferDst, blit_image call) and
"present-barrier" (TransferDst→Present, empty execute). All fake `CommandBuffer`
implementations updated. 4 new unit tests. Smoke updated with end-to-end blit path.

Three-flavour green:
- win-debug:    261/261
- win-release:  260/260
- win-asan:     261/261

Detail: `docs/sessions/2026-05-01-renderer-v1i-swapchain-blit.md` (to be written).

## Previous shipped milestone (–3)

**2026-05-01 — `crd-renderer` v1g `ForwardRenderPath` shipped.**

First concrete `IRenderPath`: depth prepass (opaque items, depth-only) + main color pass
(opaque + masked, full shading). `PerFrameUbo` (288 bytes) at set 0, `PerDrawPush`
(model matrix, 64 bytes) as push constants. `ForwardRenderPath::create()` allocates
ring UBO + descriptor set per frame-in-flight. Render targets (B8G8R8A8Unorm color,
D32Sfloat depth) owned by path, recreated on `resize()`.

Vulkan backend hardened: `begin_rendering` now caller-managed transitions (no implicit
layout changes), color attachment optional (null = depth-only), null fragment shader
supported in `create_graphics_pipeline` (`colorAttachmentCount = 0`). Added `inverse(Mat4f)`
via Laplace cofactor expansion. 5 new unit tests.

Three-flavour green:
- win-debug:    253/253
- win-release:  252/252
- win-asan:     253/253

Smokes: `smoke_rhi_vulkan_bootstrap` (120 frames, clean), `smoke_renderer` (frame graph
transitions verified).

Detail: `docs/sessions/2026-05-01-renderer-v1g-forward-render-path.md`.

## Previous shipped milestone (–4)

**2026-05-01 — `crd-renderer` v1e+f merged: push constants + descriptor system + material binding shipped.**

Merged v1e + v1f into one slice. RHI surface: `push_constants()`, `bind_descriptor_sets()`,
`create_descriptor_set_layout()`, `create_pipeline_layout()`, `create_descriptor_allocator()`.
Vulkan backend: `VulkanDescriptorSetLayout`, `VulkanPipelineLayout`, `VulkanDescriptorSet`,
`VulkanDescriptorAllocator` (ring-buffer, `frames_in_flight` pools — see session doc).
`ShaderStage` promoted to bitmask (Vertex=1, Fragment=2, Compute=4). Explicit `PipelineLayout`
added to `GraphicsPipelineDesc` (optional, at end — no positional-init breakage). Renderer
material system: `MaterialLayout` + `MaterialInstance` wrapping the allocator-backed
descriptor set lifecycle. 10 new unit tests, 4 new material tests.

Three-flavour green:
- win-debug:    248/248
- win-release:  247/247
- win-asan:     248/248

Detail: `docs/sessions/2026-05-01-renderer-v1ef-descriptors.md`.

## Previous shipped milestone (–5)

**2026-05-01 — `crd-renderer` v1b real draw execution shipped.**

`crd-renderer` now has a real execution layer over the prepared draw items:
minimal pass orchestration, command buffer recording, pipeline resolution, and
draw-call submission into one rendering pass without taking ownership of native
pipeline objects.

Three-flavour green:
- Debug: 228/228
- Release: 227/227
- ASan: 228/228

Detail: `docs/sessions/2026-05-01-renderer-v1b-draw-execution.md`.

## Previous shipped milestone (–6)

**2026-05-01 — `crd-renderer` v1a explicit renderables shipped.**

The engine now has its first high-level renderer consumer over the completed
shader packet: camera, explicit renderable list, draw-item preparation, and a
clean frame-plan handoff without scene/ECS commitments.

Three-flavour green:
- Debug: 227/227
- Release: 226/226
- ASan: 227/227

Detail: `docs/sessions/2026-05-01-renderer-v1a-explicit-renderables.md`.

## Previous shipped milestone (–7+)

**2026-05-01 — `crd-shader` 2.3g pipeline handoff / descriptor growth shipped.**

`crd-shader` now produces a backend-neutral handoff surface describing compiled
module usage, normalized descriptor bindings, push-constant visibility, and
vertex-input requirements for a variant. This cleanly separates shader-owned
metadata from backend-owned pipeline objects.

Three-flavour green:
- Debug: 226/226
- Release: 225/225 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 226/226

Detail: `docs/sessions/2026-05-01-shader-2.3g-pipeline-handoff.md`.

## Previous shipped milestone

**2026-04-30 — `crd-shader` 2.3f hot reload shipped.**

Successful reload now compiles and swaps atomically, failed reload keeps the
last-good live state, and reload observability is exposed through `ReloadEvent`
without crashing consumers or invalidating effect/variant identity.

Three-flavour green:
- Debug: 225/225
- Release: 224/224 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 225/225

Detail: `docs/sessions/2026-04-30-shader-2.3f-hot-reload.md`.

## Previous shipped milestone

**2026-04-30 — `crd-shader` 2.3e cache hierarchy shipped.**

Source/preprocessed/SPIR-V cache keys are now explicit, local include graphs
participate in the key path, and the runtime now has both in-memory and on-disk
SPIR-V cache reuse. Compile diagnostics expose cache hit/miss behavior without
leaking backend types.

Three-flavour green:
- Debug: 223/223
- Release: 222/222 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 223/223

Detail: `docs/sessions/2026-04-30-shader-2.3e-cache-hierarchy.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3d variant key + mechanism policy shipped.**

Structural variant identity is now deterministic and typed. `VariantKey`
generation uses only structural axes, specialization values are excluded from
the structural key by design, and the hybrid mechanism policy is now encoded
as public helper decisions (`Permutation`, `SpecializationConstant`,
`ResourceBinding`, `DynamicBranch`).

Three-flavour green:
- Debug: 220/220
- Release: 219/219 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 220/220

Detail: `docs/sessions/2026-04-29-shader-2.3d-variant-key.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3c reflection consumption shipped.**

`spirv-reflect` now drives descriptor bindings, push-constant layout, vertex
input metadata, and material-parameter discovery from the canonical internal
SPIR-V modules. Reflection data is consumed into Cerid-owned effect/module
metadata with no public GLSL/SPIR-V/Vulkan leakage.

Three-flavour green:
- Debug: 216/216
- Release: 215/215 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 216/216

Detail: `docs/sessions/2026-04-29-shader-2.3c-reflection.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3b frontend → IR seam + GLSL ingest shipped.**

GLSL source file ingestion now compiles through a runtime-loaded `shaderc`
frontend into canonical internal SPIR-V modules, without leaking GLSL/SPIR-V/
Vulkan through the public API. Successful and failing compile paths are both
covered in tests.

Three-flavour green:
- Debug: 215/215
- Release: 214/214 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 215/215

Detail: `docs/sessions/2026-04-29-shader-2.3b-glsl-ingest.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3a public envelope shipped.**

Opaque handles, backend-neutral metadata types, minimal `Effect` / `Runtime`
interfaces, and an in-memory runtime seam proving effect/variant/reload
observability without leaking GLSL/SPIR-V/Vulkan through the public API.

Three-flavour green:
- Debug: 214/214
- Release: 213/213 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 214/214

Detail: `docs/sessions/2026-04-29-shader-2.3a-envelope.md`.

## Previous shipped milestone

**2026-04-29 — GPU memory + streaming foundation shipped.**

Centralized Vulkan allocator helper, backend-owned buffer/image allocation,
real image creation path, and allocator-backed smoke/test coverage. This is not
the final allocator architecture, but it stabilizes resource ownership before
shader/renderer growth.

Three-flavour green:
- Debug: 210/210
- Release: 209/209 (Debug-only stats test correctly skipped)
- ASan: 210/210

Detail: `docs/sessions/2026-04-29-gpu-memory-streaming-foundation.md`.

## Previous shipped milestone

**2026-04-28 — ImGui debug overlay shipped.**

Debug-only Dear ImGui layer over `crd-app` + `crd-rhi-vulkan`, configured via
`crd-config`. Docking on by default, multi-viewport off by default, theme and
panel visibility from `runtime/configs/imgui_layer.toml`, and a real
triangle+overlay smoke.

Three-flavour green:
- Debug: 210/210
- Release: 209/209 (Debug-only stats test correctly skipped)
- ASan: 210/210

Detail: `docs/sessions/2026-04-28-imgui-debug-overlay.md`.

## Previous shipped milestone

**2026-04-28 — `crd-config` core shipped.**

Typed TOML wrapper over `toml++` with non-fatal schema-with-defaults behavior:
typed `get<T>(key, fallback)`, typed `set<T>(key, value)`, parse errors via
`g_log_config`, dot-path nested lookup, sample TOML, and `smoke_config`.

Three-flavour green:
- Debug: 209/209
- Release: 208/208 (Debug-only stats test correctly skipped)
- ASan: 209/209

Detail: `docs/sessions/2026-04-28-config-core.md`.

## Previous shipped milestone

**2026-04-28 — First triangle on screen.**

Full RHI/Vulkan path real: instance → device → swapchain → command buffer →
shader modules → graphics pipeline → vertex buffer → draw → present.
Dynamic rendering chosen as the minimal path. Triangle stayed narrow: no
descriptors, materials, scene graph, camera system, or allocator policy
creep.

Three-flavour green:
- Debug: 203/203
- Release: 202/202 (Debug-only stats test correctly skipped)
- ASan: 203/203

Detail: `docs/sessions/2026-04-28-rhi-vulkan-first-triangle.md`.

## Next up (next 1–5 sessions)

1. **Phase 3.0 v1a** — `EntityId` + `SlotMap` + `World` shell. Locked design (ADR-0049). ~250 LOC + tests.
2. **Phase 3.0 v1b** — `ComponentRegistry` + `register_component` variadic-trait grammar. ~200 LOC + tests.
3. **Phase 3.0 v1c** — `ArchetypeChunkStorage`. ~600 LOC + tests. Largest slice; may sub-divide into v1c1 (chunk allocator + SoA layout) and v1c2 (archetype graph + entity move).
4. **Phase 3.0 v1d** — `SparseSetStorage`. ~250 LOC + tests.
5. **Phase 3.0 v1e** — Mixed-backend chunk visitor. ~200 LOC + tests.

After v1e the foundation is in place; v1f (Relations) → v1g (Query DSL) → v1h (System+Schedule) → v1i (Index framework + ChangeDetect + AsyncAware) → v1j (Transform + propagation) → v1k (SceneResource + Loader) → v1l (cook_scene handler) → v1m (sandbox renderer integration) → v1n (reserved-slot freeze) follow in order.

## Roadmap ordering

- **Phase 2.6** — `crd-resources` + asset cooker. ✅ COMPLETE (v1a–v1g, 2026-05-04). ADRs 0036–0041.
- **Phase 2.7** — Asset import bootstrap: `TextureResource` + `MeshResource` + glTF (cgltf) + material params + GPU upload + `crd-meshgen` + `crd-sandbox`. ADRs 0042–0043, 0045. ✅ COMPLETE.
- **Phase 2.8** — Material GPU wiring + sandbox rendering + asset import: per-material pipeline cache, multi-pass shader selection, depth prepass, default lit shaders, sandbox 3D rendering, glTF demo assets, cook-demo-assets target, unified Asset Browser. ADRs 0044, 0046. ✅ COMPLETE (v1a–v1g, 2026-05-06).
- **Phase 3.0** — Scene/ECS foundation: 8-layer slot-shaped architecture (Entity → Storage hybrid → Relations → Query/System/Schedule → Index framework → reserved L6–L8). 14 slices. Cerid signature: every novel ECS extension rides the L5 index framework. ADRs 0049–0057 locked 2026-05-06. **Architecture ready, awaiting user start command.**
- **Phase 3.0** — `crd-scene` / ECS (hybrid hierarchy + SoA components + TOML → binary serialization + renderer integration). Seven sub-ADRs needed before coding. Plugs into `crd-resources` as `SceneLoader`.
- **Phase 3.1** — Physics (PhysX 5 backend + scene integration + fixed-step + deterministic mode).
- **Phase 3.2** — Animation (skeletal, blend trees, IK). First rig + skin data in `MeshResource`.
- **Phase 3.3** — `crd-font` (MTSDF atlas, FreeType+msdfgen cooker, HarfBuzz shaping, billboard text, dynamic atlas, extruded text mesh). ADR-0047.
- **Phase 3.4** — Audio (spatialized, DAW plugin host scaffold).
- **Phase 3.5–3.7** — PBR + post-FX + culling (IBL, CSM, ACES, bloom, TAA, BVH, GPU-driven). Closes debt items 4–5.
- **Phase 4.0** — C++ hot-reload DLL scripting (ADR-0034).
- **Phase 4.2** — Networking: transport layer → deterministic simulation → client-server sync (ADR-0035).
- **Phase 8** — Domain modules: robotics, aerospace, cinematic, procedural generation — after Phase 4 + editor foundations.

Full plan: `docs/ROADMAP.md` → `docs/phases/`.

## Open questions

- `crd-config` hot-reload remains 1.6b unless ImGui integration proves it
  should move earlier.
- Runtime scene binary format — FlatBuffers vs Cap'n Proto? Park for
  Phase 3.1c.

## Test counts (last quality pass)

- win-debug:          481/481
- win-relwithdebinfo: 481/481
- win-release:        478/478
- win-asan:           481/481
- win-clang-cl:       481/481
- win-tidy:           ✅ build clean

(win-release is 3 fewer than debug: debug-only `FiberState` tests excluded by `#if CRD_ENABLE_ASSERTS`)

## Pointers (lazy-load reference)

Agents: don't read everything. Use these breadcrumbs.

- **Hub:** `docs/ROADMAP.md` (small navigation page; safe to read fully)
- **Principles:** `docs/PRINCIPLES.md` (read every session, short)
- **Active phase only:** `docs/phases/phase-2.8-material-completion.md` (v1a–v1e complete; active); `docs/phases/phase-2.7-asset-import.md` (reference; complete); `docs/phases/phase-2.6-resources.md` (reference; complete)
- **Other phases:** `docs/phases/phase-<X>.md` (read ONLY when relevant)
- **Specific decision:** `docs/decisions/<NNNN>-<slug>.md` (find via
  `docs/decisions/README.md` tag index)
- **Last session detail:** the single file linked above, not the whole
  `docs/sessions/` folder
- **Module overview:** `docs/systems/<module>.md` (when working on that
  module)
- **Module deep-dive:** `docs/<module>/<MODULE>_FILE.md` (only when doing
  surgery)
- **Open debt:** `docs/debt.md`
- **Detour queue + rules:** `docs/detours/README.md`

When in doubt, ASK before reading large files.

## Session log (rolling, last 5)

- **2026-05-06** — Phase 3.0 architecture locked. Nine ADRs accepted (0049–0057): Entity & SlotMap, Storage backends (Archetype + SparseSet hybrid), Relations as first-class, Query/System/Schedule, Component index slot framework, Transform hierarchy update, Scene serialization (TOML + SCEN CRDR — closes ADR-0020's FlatBuffers/Cap'n Proto deferral), Reserved L6–L8 slots, UI-in-tree boundary. Phase 3.0 doc rewritten around 8-layer architecture and 14 slices. ADR index + ROADMAP updated. Ready to start v1a on user command.
- **2026-05-06** — Sandbox async asset load: `load_sync<MeshResource>` swapped for `load_async` + per-frame `is_ready()` polling on `SandboxLayer`; pending-handle state (`m_pending_load`, `m_pending_index`) added; selection re-click is a no-op while same id is in flight; "Status: loading…" appears in the metadata pane until Ready; clicking a procedural shape mid-load drops the pending import. Shutdown order in sandbox/main.cpp updated to drain `jobs::shutdown()` *before* `app.detach_all_layers()` (ResourceManager outlives in-flight load jobs). GPU upload (`GpuUploader::upload_mesh`) still synchronous — the remaining hitch — formally tracked in `docs/debt.md` and pulled forward as a Phase 3.0 prerequisite (`docs/phases/phase-3.0-scene-ecs.md`). All 6 configs green (481/481 win-debug, 478/478 release).
- **2026-05-06** — `crd-math` interpolation primitives + Penner easings shipped: scalar lerp/mix/saturate/step/smoothstep/smootherstep/inverse_lerp/remap/damp added to scalar.hpp; mix + damp componentwise added to vec.hpp; new easing.hpp with 31 Penner curves (Linear + In/Out/InOut for Sine/Quad/Cubic/Quart/Quint/Expo/Circ/Back/Elastic/Bounce); sandbox `OrbitCamera` migrated to `crd::math::damp`; 8 new tests; all 6 configs green (481/481 win-debug, 478/478 release).
- **2026-05-06** — Phase 2.8 v1f+v1g SHIPPED: glTF demo asset bundle (BoxTextured CC-BY, Duck SCEA, BoomBox CC0) + procedural CC0 PNGs (checker_512, bricks_512); `cook-demo-assets` CMake target → `assets/cooked/demo_assets.crdr` (5 entries); unified Asset Browser ImGui panel (replaces Meshgen Browser, two collapsing sections, click-to-load via `ResourceManager::load_sync<MeshResource>`); device-destroy crash fix in `Application::detach_all_layers()` + sandbox/main.cpp shutdown reorder; `GpuUploader` taken from non-const& to const&; cooker `.meta.meta` chain-growth bug fixed (`scan_recursive` now filters via `name.ends_with(".meta")`, not first-dot extension); cooker no longer writes `.meta` sidecars for handler-less files (handler lookup moved before sidecar resolution); all 6 configs green (473/473 win-debug, 470/470 release). Phase 2.8 COMPLETE.
- **2026-05-05** — Phase 2.8 v1a–v1e SHIPPED: per-material pipeline cache + multi-pass begin_pass() + depth-only prepass pipeline + surface.vert/frag default lit shaders + SandboxPipelineResolver + sandbox render_scene() wired to ForwardRenderPath; Module::code_bytes() + Format::R32G32B32A32Sfloat added; SPV_REFLECT bug fixed; smoke_depth_prepass.exe; [[maybe_unused]] release fixes; all 6 configs green (471/471 win-debug). v1f+v1g deferred.
- **2026-05-05** — Phase 2.7 trailing items closed: surface_data.glsl.inc (GLSL contract for material shaders); docs/systems/texture_resource.md + mesh_resource.md + meshgen.md; phase-2.7 DoD marked complete; sandbox GPU rendering + demo assets formally deferred to Phase 2.8 (added to phase-2.8 doc).
- **2026-05-05** — Phase 2.7 v1e SHIPPED: crd-meshgen module (8 generators: plane/box/sphere/icosphere/cylinder/cone/capsule/torus); smoke_meshgen.exe headless; 11 unit tests; crd-sandbox Meshgen Browser ImGui panel (8 shapes, vertex/index/tri counts); clang-cl normalize3 unused-function fix; all 6 configs green (468/468 win-debug). Phase 2.7 COMPLETE.
- **2026-05-05** — Phase 2.7 v1d SHIPPED: GpuUploader (upload_texture→GpuTexture, upload_mesh→GpuMesh, staging-buffer pattern); RHI additions (copy_buffer, copy_buffer_to_image, submit_and_wait, VK_REMAINING_MIP_LEVELS fix); smoke_asset_import.exe (GPU smoke, graceful skip); crd-sandbox (OrbitCamera exponential-lerp, ImGui panel, --headless); all 6 configs green (457/457 win-debug).
- **2026-05-05** — Phase 2.7 v1c SHIPPED: full material system foundation (ADR-0048): MaterialTemplate + MaterialInstance, MaterialDomain, PassType, RasterState, CookedParameter, ShaderOptionDecl, MATR v2 artifact (INFO+PASS), legacy META backward-compat, cooker rewrite, renames; 5 new tests; smoke_material.exe; all 6 configs green (457/457 win-debug).
- **2026-05-05** — Phase 2.7 v1b SHIPPED: MeshResource + MeshResourceLoader + glTF cooker handler (cgltf, MikkTSpace, multi-mesh via ExtraArtifact); 4 new tests in test_mesh_loader.cpp; smoke_mesh.exe; all 6 configs green (452/452 win-debug).
- **2026-05-04** — Phase 2.7 v1a SHIPPED: TextureResource + MipLevel + TextureFormat + TextureResourceLoader + texture cook handler (stb_image, box-filter mip gen); 4 new tests; smoke_texture.exe; all 6 configs green (448/448 win-debug).
- **2026-05-04** — Phase 2.6 v1g SHIPPED: load_streamed + 2Q LRU eviction + memory budget + pinning; 5 new tests in test_eviction.cpp; smoke_resources_stream.exe; all 6 configs green (444/444 win-debug). Phase 2.6 COMPLETE.
- **2026-05-04** — Phase 2.6 v1e shipped: ShaderResourceLoader + MaterialResourceLoader + compile_glsl() + GLSL/material cooker handlers + smoke_resources_render; 6 new tests; all 6 configs green (435/435 win-debug). Clang-cl fix: removed dead `to_parameter_class_local`.
- **2026-05-04** — Phase 2.6 v1d shipped: AsyncFile + load_async<T> + fiber-cooperative wait_ready() + load coalescing; 9 new tests; all 6 configs green (429/429 win-debug).
- **2026-05-03** — Phase 2.6 v1c shipped: RefCounted<T> + ResourceHandle<T> + load_sync<T> + cycle detection; all 6 configs green (420/420 win-debug).
- **2026-05-03** — Phase 2.6 v1b shipped: zstd compression + cooker CLI + .bin handler + 4 tests; all 6 configs green (408/408 win-debug).
- **2026-05-03** — Debt cleared: SpscQueue<T> lock-free SPSC queue (+7 tests), FileWatcher polling mtime watcher (+4 tests), Doxygen per-symbol docs in crd-core, runtime-disabled log benchmark fix, multi-viewport ImGui moved to long-term deferred. 404/404 win-debug.
- **2026-05-03** — Phase 2.6 v1a shipped: `crd-resources` (ResourceId, CRDR, ResourceManager shell) + `asset_cooker manifest_dump`; String SSO remaining-capacity fix; all 6 configs green (393/393).
- **2026-05-02** — `crd-jobs` v1k integration smoke + crd-app wiring shipped; Phase 2.5 COMPLETE; all 6 configs green (355/355 win-debug).
- **2026-05-02** — `crd-jobs` v1j per-thread frame allocator shipped; 4 new tests; all 6 configs green (355/355 win-debug).
- **2026-05-02** — `crd-jobs` v1i SBO lambda helpers shipped; `make_job<F>()` + `parallel_for()`; 5 new tests; all 6 configs green (351/351 win-debug).
- **2026-05-02** — `crd-jobs` v1h Public API shipped; `jobs.cpp` + 5 new public-API tests; `Fiber::job_counter` field (fiber-survives-suspension counter fix); all 6 configs green (346/346 win-debug).
- **2026-05-02** — `crd-jobs` v1g Worker thread pool + main-thread fiber shipped; 10 new tests; TIB save/restore fix; fiber re-init fix; all 6 configs green (341/341 win-debug).
- **2026-05-02** — `crd-jobs` v1f Counter + wait mechanism shipped; 14 new tests; NDEBUG fix for Release; all 6 configs green (331/331 win-debug).
- **2026-05-02** — `crd-jobs` v1e Priority Scheduler shipped; 16 new tests; /EHsc fix; all 6 configs green (317/317 win-debug).
- **2026-05-02** — `crd-jobs` v1d Vyukov MPMC injection queue shipped; all 6 configs green (301/301 win-debug).
- **2026-05-02** — `crd-jobs` v1c Chase-Lev work-stealing deque shipped; all 6 configs green (291/291 win-debug).
- **2026-05-02** — `crd-jobs` v1b fiber pool shipped; renderer LTO fix; all 6 configs green (279/279 win-debug).
- **2026-05-01** — `crd-jobs` v1a hand-rolled asm context switch shipped (266/266 win-debug).
- **2026-05-01** — `crd-renderer` v1i swapchain blit + first full frame loop shipped (261/261 win-debug).
- **2026-05-01** — `crd-renderer` v1h index buffer + `draw_indexed` shipped (257/257 win-debug).
- **2026-05-01** — `crd-renderer` v1g `ForwardRenderPath` shipped (253/253 win-debug).
- **2026-05-01** — `crd-renderer` v1c frame graph v1 shipped (233/233 win-debug).
- **2026-05-01** — `crd-renderer` v1b real draw execution shipped.
- **2026-05-01** — `crd-renderer` v1a explicit renderables shipped.
- **2026-05-01** — `crd-shader` 2.3g pipeline handoff shipped.
- **2026-04-29** — `crd-shader` 2.3c reflection consumption shipped.
- **2026-04-29** — `crd-shader` 2.3b frontend → IR seam + GLSL ingest shipped.
- **2026-04-29** — `crd-shader` 2.3a public envelope shipped.
- **2026-04-29** — GPU memory + streaming foundation shipped.
- **2026-04-28** — ImGui debug overlay shipped.
- **2026-04-28** — `crd-config` core shipped.
- **2026-04-28** — First triangle through full RHI/Vulkan path.
- **2026-04-27** — `crd-rhi-vulkan` bootstrap (instance/device/surface/swapchain).
- **2026-04-26** — `crd-rhi` v1a scaffold with fake-backend tests.
- **2026-04-26** — `crd-app` Phase 1.5 shipped (LayerStack + propagated
  events + sync EventBus).
- **2026-04-25** — Platform v1c (input) shipped.

> Older entries: `docs/sessions/`.
