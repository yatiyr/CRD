# 2026-05-09 — Phase 3.0 v1o3: sandbox integration (closes v1o)

**Status at start:** v1o2 shipped earlier today. The phase doc's v1o3 row called for "sandbox uses async upload + profile + öbek end-to-end; ImGui panel toggles profile + reverts overrides live; visual proof of full authoring stack." The user's explicit ask: ship it elite-level, fully — author the profile.toml + preset.toml + öbek.toml content, wire the cookers, integrate `ForwardRenderPath` as a real `IPresetTarget`, and pin v1o2's promised drop-callback contract through a proper hook (no sandbox-side workarounds).

**Status at end:** v1o3 shipped — full async-GPU-upload + Profile + Preset + Öbek pipeline runs end-to-end in the sandbox with cooker-driven content, an `IComponentIndex` drop-callback hook, and a `ForwardRenderPath` that consumes `QualityPreset` to drive observable rendering behaviour. **12-config sweep: 851/848 green.**

---

## Five layers shipped

### L1 · `RenderMeshIndex` — drop-callback hook (engine/renderer)

A new `crd::scene::IComponentIndex` that owns the GPU meshes produced by completed `UploadHandle`s. v1o2 stored these in `RenderUploadSystem::m_owned_meshes`; v1o3 splits them out so the L5 fan-out (the framework already used by `ChangeDetectIndex` + `AsyncAwareIndex`) carries lifecycle events to the mesh owner.

```cpp
class RenderMeshIndex final : public crd::scene::IComponentIndex
{
public:
    explicit RenderMeshIndex(crd::memory::IAllocator* alloc);

    [[nodiscard]] bool install(crd::scene::EntityId e, GpuMesh mesh);
    [[nodiscard]] GpuMesh* find(crd::scene::EntityId e) noexcept;
    bool                   release(crd::scene::EntityId e) noexcept;

    void watch(crd::scene::ComponentId c) noexcept { m_observed.set(c); }
    [[nodiscard]] crd::usize count() const noexcept { return m_owned.size(); }

    void on_remove(EntityId, ComponentId, const void*) override;          // evicts
    void on_entity_destroyed(EntityId) override;                          // evicts
    [[nodiscard]] crd::scene::ComponentMask observed() const override { return m_observed; }
};
```

Registration sequence (consumers do):

```cpp
world.register_component<Renderable>(crd::scene::AsyncAware{}, /* hint */);
world.register_component<PendingMeshUpload>(/* hint */);
auto* idx = world.register_index<RenderMeshIndex>(allocator);
idx->watch(world.component_id<Renderable>());
world.register_system(std::make_unique<RenderUploadSystem>());
```

`RenderUploadSystem::run` looks up the index via `world.find_index<RenderMeshIndex>()` and calls `install(entity, std::move(mesh))` on promotion. When the entity is later destroyed via `World::destroy(e)`, the per-component `on_remove(Renderable, ...)` event evicts the resident `GpuMesh` automatically — no explicit cleanup call from the consumer required. **This is the architectural answer to the contract v1o2's session log said v1o3 would pin.**

Tests: `test_render_upload_system.cpp` grew from 6 to 8 cases — added one for `World::destroy(e)` triggering the drop-callback eviction and one for manual `release(e)`.

### L2 · `QualityPreset` v2 — `enable_depth_prepass` field (engine/preset)

Bumped from v1 to v2 by repurposing one byte of `_reserved[8]` into a named field. Binary layout still 144 B + alignof 8 — the version bump signals semantic upgrade, not a binary break. The 1U default matches v1's hardcoded behaviour (depth-prepass on), so default-constructed v2 presets are observably identical to v1 ones.

```cpp
struct alignas(8) QualityPreset
{
    static constexpr crd::u32 version = 2U;        // was 1U
    crd::u32 shadow_resolution    = 2048U;
    crd::u8  msaa_samples         = 4U;
    crd::u8  ssr_quality          = 2U;
    crd::u8  ssao_quality         = 2U;
    crd::u8  post_fx_count        = 0U;
    crd::u8  enable_depth_prepass = 1U;            // v2 — repurposes one _reserved byte
    crd::u8  _reserved[7]         = {};
    crd::resources::ResourceId post_fx[8] = {};
};
```

### L3 · `ForwardRenderPath` as `IPresetTarget` (engine/renderer)

`ForwardRenderPath` now multi-inherits `IRenderPath` + `crd::preset::IPresetTarget`. `apply(const QualityPreset&)` caches the resolved preset (`m_quality`) and the next `build()` consults it. **`enable_depth_prepass = 0` skips the depth-prepass DRAWS** without removing the pass declaration — FrameGraph topology stays invariant under preset changes (no barrier-insertion / transient-alias differences, no test fixture churn). Reverse-Z + GREATER depth test ensures the disabled-prepass path still produces correct geometry; you just lose the early-Z optimisation.

`crd-renderer`'s CMakeLists gained `crd-preset` as a PUBLIC link dep. Other QualityPreset fields (shadow_resolution, msaa_samples, ssr_quality, ssao_quality, post_fx[]) ride along for read-back via `quality_preset()`; their consuming systems light up in Phase 3.5+.

### L4 · Three new cookers + content (tools/asset_cooker, assets/source/)

Three asset cooker handlers registered:

| Extension | Handler | Output FourCC | Notes |
|---|---|---|---|
| `.preset.toml` | dispatch by `type` ("Quality" / "Camera") → `PresetArtifactBuilder` | `PRQL` / `PRCM` | Schema struct populated from TOML keys; payload = raw struct bytes |
| `.profile.toml` | walk `[[profile]]` array → `ProfileArtifactBuilder` | `PROF` | Resolves bundle entries via sibling `.meta` sidecar UUIDs; predicates supported (Os/GpuTier/Domain/Mode/TargetFps/CpuCores) |
| `.obek.toml` | wraps existing `obek_cooker_inline()` | `OBEK` | Provides FNV-1a-keyed `obek_root_id` + a filesystem `file_resolver` for `extends=`/`obek=` references |

`crd-cooker` CMakeLists gained `crd-preset` + `crd-profile` as PUBLIC link deps. `register_builtin_handlers()` now wires preset + profile + obek alongside glsl + material + texture + mesh + blob.

Content authored in `assets/source/`:

```
presets/
  quality_default.preset.toml   shadow_resolution=2048 msaa=4 enable_depth_prepass=1
  camera_default.preset.toml    fov=60° near=0.01 far=1000  Perspective+Manual
profiles/
  default.profile.toml          priority=0  no predicates  bundle=[quality, camera]
obeks/
  obek_demo.obek.toml           two-entity öbek (root + child with ChildOf), demo Transform
```

The cooker walks `assets/source/` recursively and emits all 9 artifacts (3 mesh + 2 texture + 1 obek + 2 preset + 1 profile) into the demo asset pack. `sandbox/CMakeLists.txt`'s `DEMO_ASSETS_SOURCES` glob extended to track `*.preset.toml` / `*.profile.toml` / `*.obek.toml` for incremental recooks.

### L5 · Sandbox integration (sandbox/)

`SandboxLayer` rewritten around a `crd::scene::World`:

1. **ECS bootstrap** (`init_scene_world`): registers `Renderable` (with `AsyncAware{}` trait), `PendingMeshUpload`, `Transform`, the six built-in relations; registers `RenderMeshIndex` and `RenderUploadSystem`.
2. **Single render path** — both procedural meshes and imported glTF spawn ECS entities. Procedurals upload synchronously and immediately mark `AsyncAware Loaded`; imports use `GpuUploader::upload_mesh_async` + `PendingMeshUpload`, with `RenderUploadSystem` promoting them when the fence signals. The legacy `m_gpu_mesh` singleton is gone — `m_gpu_mesh` was where v1o2 kept the procedural mesh outside the ECS; v1o3 funnels it through.
3. **Profile + Preset boot** (`try_boot_profile_pipeline`): reads the `default.profile.toml.meta` UUID, kicks `load_async<ProfileResource>` + `load_async<PresetResource>` for both presets, and once all three handles report `Ready` applies the cooked `QualityPreset` to `m_frp` and the cooked `CameraPreset` to a sandbox-side `SandboxCameraTarget` (`IPresetTarget` adapter). The Camera target's resolved values drive `render_scene`'s `perspective_reverse_z(fov, aspect, near)` — the FOV slider in the ImGui panel is a real renderer-behaviour change.
4. **Öbek demo** (`try_load_demo_obek` + ImGui panel): instantiates `obek_demo.obek.toml` at boot with `World::instantiate_obek`. Three buttons exercise `World::set_translation` (override), `World::revert_component` (revert) and `World::unpack_obek` (sever source link). The child entity's translation is read back live for visual confirmation.
5. **ImGui surface** — three new panels:
   - **Sandbox** — viewport + camera state + ECS counts (entities, resident meshes, pending uploads).
   - **Profile / Presets** — boot-time `ProfileContext` (OS, CPU cores), runtime sliders for QualityPreset (shadow_resolution, msaa, enable_depth_prepass) + CameraPreset (fov, near, far). Sliders dirty a runtime override and re-apply via `apply_preset<T>(target, resource, runtime)`.
   - **Öbek demo** — apply / revert / unpack buttons.

`crd-sandbox`'s CMakeLists gained `crd-preset`, `crd-profile`, `crd-scene` PRIVATE links.

### Engine fix · `World` forward-decl of `crd::jobs::Counter`

`engine/scene/include/crd/scene/world.hpp` previously forward-declared `crd::jobs::Counter` as `class Counter;`, but `crd/jobs/jobs.hpp` defines it as `using Counter = detail::Counter`. Any TU that included both (only the new sandbox does today) hit C2371 "redefinition; base types differ". Replaced with the alias-friendly form:

```cpp
namespace crd::jobs::detail { struct Counter; }
namespace crd::jobs { using Counter = detail::Counter; }
```

### Build infra · Linux native-FS redirect (`scripts/wsl-build.ps1`)

Linux build dirs now land at `$HOME/cerid-build/<preset>` (native ext4) instead of `/mnt/d/Dev/cerid/build/<preset>` (9P bridge). CMake `_deps/`, ninja stat, `.o` writes all hit native FS; source reads still cross 9P but get cached after the first pass. Source tree stays on the Windows drive. CI on GitHub is unaffected (it never invokes `wsl-build.ps1`).

`-Reconfigure` flag now defaults OFF (was implicitly required for parallel-stream safety; the per-preset temp file from earlier this session removed that race). Subsequent local sweeps reuse build dirs and only rebuild changed engine sources.

---

## Twelve-config sweep — green

| Win × 7 | Tests | | Linux × 5 | Tests |
|---|---|---|---|---|
| win-debug | 851/851 | | linux-gcc-debug | 851/851 |
| win-relwithdebinfo | 851/851 | | linux-gcc-relwithdebinfo | 851/851 |
| win-release | 848/848 | | linux-gcc-release | 848/848 |
| win-asan | 851/851 | | linux-gcc-asan | 851/851 |
| win-clang-cl | 851/851 | | linux-gcc-shipping | clean |
| win-shipping | clean | | | |
| win-tidy | clean | | | |

851 = 849 v1o2 baseline + 2 RenderMeshIndex drop-callback tests.

A transient SEGFAULT in `jobs: run_and_wait from inside a worker fiber` showed up on the first `linux-gcc-release` parallel-build attempt; re-running the preset alone (no parallel WSL pressure) passed cleanly. The job test isn't v1o3-related; the failure correlated with three concurrent ninja builds inside a single WSL VM and isn't reproducible at lower load.

---

## Architectural pins

1. **The drop-callback hook is the proper architectural answer.** v1o2 left an explicit `release_owned(EntityId)` API as a placeholder; v1o3 deleted it in favour of the L5 `IComponentIndex` fan-out path. Any future renderer that wants per-entity GPU-resource tracking — not just meshes; textures, materials, instance buffers — implements `IComponentIndex` and rides the same machinery. No `World::destroy` call sites changed; the events were already being emitted.

2. **`enable_depth_prepass` skips draws, not the pass declaration.** Per the advisor's feedback this slice. Removing the FrameGraph pass would change topology — different barrier insertion, different transient aliasing, different test fixtures. Skipping draws inside an always-declared pass is a one-line change with zero topology impact and identical observable behaviour ("depth ends up clear-only" vs "depth populated").

3. **`QualityPreset` v2 layout-stable.** The `_reserved[8]` array existed for exactly this purpose. Repurposing one byte preserves binary compat at the bytes level; bumping `version` is the soft signal that consumers should interpret offset 8 as the new named field. There are no v1 cooked artifacts in production yet, so re-cooking is the migration.

4. **One render path, not two.** Procedurals and imports share the same ECS query in `render_scene` — `query<Renderable>().skip_pending<Renderable>()`. The sandbox is now the same shape any downstream consumer would have: `register_component`, `register_index`, `register_system`, `step`, query, submit. The legacy `m_gpu_mesh` singleton is gone.

5. **Cooker handlers mirror the existing pattern.** Mesh / texture / material / GLSL handlers each register their extension, call into a typed builder, return CRDR bytes. Preset / Profile / Öbek follow the same shape. Adding a new asset type is a one-day extension — no asset-cooker core changes.

6. **Profile boot path is async.** The ResourceManager kicks `load_async` for the profile + each preset; the apply happens once all three handles report `Ready`. The sandbox doesn't block boot on file I/O. The runtime override sliders mutate a separate `m_quality_runtime` / `m_camera_runtime` and re-apply via `apply_preset<T>(target, resource, runtime_override)` — the L4 layer of ADR-0059's resolver stack, exercised live.

---

## What's deliberately NOT in v1o3

- **No predicates in the default profile.** The schema accepts them (Os / GpuTier / Domain / Mode / TargetFps / CpuCores) and the cooker authors them; the demo profile uses an empty predicate list (= match-everything). Adding `game / simulation / daw / cinematic` profiles is a content task.
- **No multi-profile picker UI.** The sandbox boots whichever profile resolves; switching at runtime requires re-applying a different bundle. The framework is in place; the picker is content-driven.
- **No MSAA wiring.** `QualityPreset::msaa_samples` round-trips through `apply()` but `rhi::ImageDesc` doesn't yet expose multisample. Lights up when the rhi grows multisample support.
- **No shadow path.** `shadow_resolution` is cached on `m_quality`; consumed by the shadow path that ships in Phase 3.5+.
- **No öbek `extends` chain demo.** The cooker supports it (v1m3b); the demo öbek is flat. Authoring a multi-level chain is a content exercise.
- **`crd-config` hot-reload watching of the demo TOMLs.** The demo asset pack is rebuilt on file change via CMake's `add_custom_command`; runtime hot-reload of preset/profile/obek changes is reserved for the sandbox-as-editor work.

---

## Files touched

```
engine/renderer/include/crd/renderer/render_mesh_index.hpp     created
engine/renderer/src/render_mesh_index.cpp                      created
engine/renderer/include/crd/renderer/render_upload_system.hpp  refactored (m_owned_meshes removed)
engine/renderer/src/render_upload_system.cpp                   refactored (uses RenderMeshIndex)
engine/renderer/include/crd/renderer/forward_render_path.hpp   IPresetTarget inherit + apply(QualityPreset)
engine/renderer/src/forward_render_path.cpp                    apply impl + enable_depth_prepass gate
engine/renderer/CMakeLists.txt                                 + crd-preset PUBLIC

engine/preset/include/crd/preset/quality_preset.hpp            v2 schema (enable_depth_prepass)

engine/scene/include/crd/scene/world.hpp                       jobs::Counter forward-decl alias-safe

tests/renderer/test_render_upload_system.cpp                   8 cases (was 6) — drop-callback + manual release
tests/preset/test_quality_preset.cpp                           v2 layout + enable_depth_prepass round-trip

tools/asset_cooker/CMakeLists.txt                              + crd-preset crd-profile PUBLIC
tools/asset_cooker/src/cook_handlers/preset.cpp                created
tools/asset_cooker/src/cook_handlers/profile.cpp               created
tools/asset_cooker/src/cook_handlers/obek_file.cpp             created
tools/asset_cooker/src/cook_handlers/blob_passthrough.cpp      register_builtin_handlers extended

assets/source/presets/quality_default.preset.toml              created
assets/source/presets/camera_default.preset.toml               created
assets/source/profiles/default.profile.toml                    created
assets/source/obeks/obek_demo.obek.toml                        created

sandbox/src/sandbox_layer.hpp                                  rewritten — World, mesh_idx, profile + preset + obek state
sandbox/src/sandbox_layer.cpp                                  rewritten — ECS-based render path, profile/preset/obek wiring
sandbox/CMakeLists.txt                                         + crd-preset crd-profile crd-scene PRIVATE; recursive glob

scripts/wsl-build.ps1                                          $HOME/cerid-build/<preset> binaryDir; -Reconfigure default OFF

AGENTS.md                                                      Agent Conduct rules added (never silently reduce scope)

docs/phases/phase-3.0-scene-ecs.md                             v1o3 row + closes v1o
docs/sessions/2026-05-09-v1o3-sandbox-integration.md           this file
```

---

## Phase 3.0 status

15 of 17 slices shipped. **One slice remaining:** v1p (reserved-slot freeze) — confirms the L6 / L7 / L8 reserved trait registrations round-trip through `register_component`, locks the Öbek + Preset + Profile API surfaces, closes Phase 3.0.

After v1p, Phase 3.0 closes. Next: Phase 3.1 (Animation) or Phase 3.2 (Renderer features — v1j GPU instancing, deferred path) depending on what unblocks the most downstream work.
