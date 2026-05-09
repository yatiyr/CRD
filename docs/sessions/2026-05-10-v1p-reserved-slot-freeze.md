# 2026-05-10 — Phase 3.0 v1p: Reserved-slot freeze (closes Phase 3.0)

**Status at start:** v1o3 shipped late 2026-05-09 (sandbox integration with the full Öbek + Preset + Profile + async-upload stack). 16 of 17 Phase 3.0 slices complete; v1p is the final close — every reserved L6/L7/L8 trait must round-trip through `register_component`, the reserved spatial DSL operators must compile and chain, the Öbek + Preset + Profile API surface must be formally pinned.

**Status at end:** v1p shipped — `ScriptComponent` + `ScriptHandle` types added, reserved spatial query operators (`.in_aabb` / `.within_radius`) shipped as passthrough, comprehensive freeze test suite (5 cases + 50+ static_asserts) ratifies the API surface. **Phase 3.0 CLOSED.** 856 unit tests across 12 build configs — all green.

---

## What shipped

### `ScriptComponent` + `ScriptHandle` (engine/scene)

ADR-0056 §2 reserved L7 (Scripts as components) for Phase 4.0 hot-reload + behaviour dispatch. v1p locks the on-entity payload shape so consumer code written today compiles and registers cleanly even though the runtime backend (ScriptSystem + ScriptRegistry) only lights up in Phase 4.0.

```cpp
// engine/scene/include/crd/scene/script_component.hpp
struct ScriptHandle
{
    crd::u64 raw = 0U;                      // 0 = "no script" sentinel
    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0U; }
    [[nodiscard]] constexpr bool operator==(const ScriptHandle&) const noexcept = default;
};
static_assert(sizeof(ScriptHandle)  == 8U);
static_assert(alignof(ScriptHandle) == 8U);

struct ScriptComponent
{
    ScriptHandle script;
    crd::u32     state_size;
    crd::u32     _reserved;        // pad to 16 B; Phase 4.0 may repurpose
};
static_assert(sizeof(ScriptComponent)  == 16U);
static_assert(alignof(ScriptComponent) == 8U);
```

**Crucial classification fix surfaced by ADR-0056 read** — `ScriptComponent` is **not a trait**. It is a *component type* users attach to entities (`world.register_component<ScriptComponent>(StorageHint::SparseSet)`). The advisor flagged this; the original v1p plan would have wrongly added it to `apply_trait` dispatch and the Phase 4.0 ScriptSystem would have had to re-design around the mistake. Caught before code went in.

### Reserved spatial DSL operators (engine/scene/query)

ADR-0053 §6 reserves `.in_aabb()` and `.within_radius()` on `Query<>`; the backing `SpatialBVHIndex` is a no-op shell in Phase 3.0 and the actual BVH lights up in Phase 3.5 (frustum culling at scale). v1p ships the operators as passthrough — they compile, chain, and yield every entity that matches the required components — so caller code written today continues to compile + iterate unchanged once Phase 3.5 wires the BVH.

```cpp
// engine/scene/include/crd/scene/query.hpp
Query& in_aabb(const crd::math::AABB<crd::f32>& box) &;
Query  in_aabb(const crd::math::AABB<crd::f32>& box) &&;
Query& within_radius(const crd::math::Vec3<crd::f32>& center, crd::f32 radius) &;
Query  within_radius(const crd::math::Vec3<crd::f32>& center, crd::f32 radius) &&;

// Inline bodies in world.hpp's templated section, mirroring .skip_pending<>.
```

Bounding-shape types are **frozen** as `crd::math::AABB<f32>` and `Vec3<f32>` + `f32` radius. A different bounding shape (OBB, frustum, capsule) ships as a *new* operator — never as a signature change to these.

`.group_by<T>()` was deliberately deferred (advisor pushback) — its return shape would have to be a group-iterator wrapper, which is a non-trivial design commitment, and ADR-0056 reserves it for Phase 4.0 ScriptSystem (its only consumer today). Reserving the *call site* without freezing the return shape would have been worse than not reserving it.

### `test_phase_3_0_freeze.cpp` (tests/scene)

Five test cases + 50+ compile-time `static_assert`s in one file:

1. **`v1p: every reserved trait is accepted by register_component`** — exercises `History{60}`, `SpatialBVH{}`, `GpuResident{}`, `Replication::ServerAuthoritative`, `Reflection{...}` each on its own component; verifies `ComponentInfo` carries the right flag/value.
2. **`v1p: registering all reserved traits at once on a single component fires every auto-register shell index`** — variadic `register_component<T>(StorageHint, AsyncAware{}, History{8}, SpatialBVH{}, GpuResident{}, Replication::ClientPredicted, Reflection{...})`; verifies `find_index<>` returns non-null for every shell index (HistoryIndex, SpatialBVHIndex, GpuResidentIndex, ReplicationIndex, ReflectionIndex) plus the always-on ChangeDetectIndex + AsyncAwareIndex.
3. **`v1p: ScriptComponent is a regular registrable component, not a trait`** — registers `ScriptComponent` with `StorageHint::SparseSet`; verifies `ComponentInfo.size == 16`, `alignment == 8`; spawns an entity, attaches an inert `ScriptComponent{ScriptHandle{}, 0, 0}`, reads it back.
4. **`v1p: .in_aabb / .within_radius compile, chain, and pass through every entity in Phase 3.0`** — registers `CompSpatial` with `SpatialBVH{}`, spawns 3 entities, asserts each operator yields all 3 (passthrough), asserts the chain `.in_aabb(box).within_radius(center, r)` composes to passthrough.
5. **`v1p: API surface freeze static-asserts all hold (compile-time)`** — placeholder runtime case so the freeze appears in `ctest --list-tests` and a green run logs the intent on every CI sweep. The actual checks are file-level `static_assert` (next section).

The compile-time pins:

```cpp
// Öbek (ADR-0058)
static_assert(sizeof(ObekInfo)                == 24U);
static_assert(sizeof(ObekComponentDescriptor) == 32U);
static_assert(sizeof(ObekEntityRecord)        == 16U);
static_assert(sizeof(ObekRelationRecord)      == 16U);
static_assert(sizeof(ObekChainEntryRecord)    == 24U);
static_assert(kObekSchemaVersion              == 1U);

// Preset (ADR-0059)
static_assert(sizeof(QualityPreset) == 144U && alignof(QualityPreset) == 8U
              && QualityPreset::version == 2U);   // v2 = enable_depth_prepass repurposed reserved byte
static_assert(sizeof(CameraPreset)  == 40U  && alignof(CameraPreset)  == 4U
              && CameraPreset::version  == 1U);

// Profile (ADR-0060)
static_assert(sizeof(ProfileFileInfo) == 16U);
static_assert(sizeof(PredicateRecord) == 8U && alignof(PredicateRecord) == 4U);

// Closed-enum value pinning (changing any of these invalidates every cooked artifact)
static_assert(static_cast<u8>(InheritPolicy::Override)    == 0U);
static_assert(static_cast<u8>(InheritPolicy::Inherit)     == 1U);
static_assert(static_cast<u8>(InheritPolicy::DontInherit) == 2U);
static_assert(static_cast<u8>(Replication::Local)               == 0U);
// ... full Replication / PredicateField / PredicateOp value pins ...

// Script (ADR-0056)
static_assert(sizeof(ScriptHandle)    == 8U);
static_assert(sizeof(ScriptComponent) == 16U);
```

Editing any of these pins is a deliberate schema break — the cooker's payload-size check turns a mismatch into `LoadState::Failed`, every cooked artifact in the field becomes invalid, and downstream consumers need explicit migration. v1p says: **don't.** New fields land via version bumps (the QualityPreset v1→v2 in v1o3 is the canonical example — one byte of `_reserved` repurposed, total size unchanged, version constant moved).

### Engine-level fix carried in: `tests/scene/CMakeLists.txt` PUBLIC link extension

Test target `crd-scene-tests` gained `crd-math + crd-preset + crd-profile` PRIVATE links so the new freeze test can include the schema headers. No runtime impact — tests-only.

### One Windows-specific finding

The em-dash character `—` in a TEST_CASE title trips Windows cmd's argument quoting when `ctest -R` shells out to Catch2 with the full title as a filter (Catch2 sees a mangled UTF-8 sequence and reports "no tests matched" → ctest marks the test failed even though the binary itself runs the case fine). Fix: use ASCII hyphens `-` in TEST_CASE titles. Documented inline; will hold for all future tests.

---

## Phase 3.0 — closed

**17 of 17 slices shipped** (2026-05-06 → 2026-05-10):

| Slice | Date | Substance |
|---|---|---|
| v1a | 2026-05-06 | EntityId + SlotMap + World shell |
| v1b | 2026-05-07 | ComponentRegistry + IStorageBackend + storage-hint trait grammar |
| v1c1 | 2026-05-07 | Chunk allocator + SoA layout + per-chunk version counter |
| v1c2 | 2026-05-07 | Archetype + ArchetypeGraph + ArchetypeChunkStorage + IStorageEventSink + typed `World::add_component<T>` |
| v1d | 2026-05-07 | SparseSetStorage + World dispatch by StorageHint |
| v1e | 2026-05-07 | Mixed-backend chunk visitor |
| v1f | 2026-05-07 | Relations + 6 built-ins (ChildOf / AttachedTo / Owns / Targets / DependsOn / PossessedBy) |
| v1g | 2026-05-07 | Query DSL — `world.query<Cs...>().with/without/with_relation/filter` chain + range-for + chunk visitor |
| v1h | 2026-05-07 | System + Schedule + Commands — ISystem virtual class + 7-phase fixed schedule + deferred-mutation buffer |
| v1i | 2026-05-07 | Index framework (IComponentIndex) + ChangeDetect + AsyncAware + 5 reserved no-op shells |
| v1j | 2026-05-07 | Transform + propagation system in PreRender; six rotation-set APIs |
| v1k | 2026-05-07 | SceneResource + SceneLoader (FourCC SCEN) + SceneArtifactBuilder + `World::instantiate_scene` |
| v1l | 2026-05-08 | `cook_scene` cooker handler + built-in TOML readers |
| v1m | 2026-05-08 | **Öbek system** (12 sub-slices, ~2700 LOC, 58 öbek tests) — full ADR-0058 surface |
| v1n | 2026-05-09 | **Preset + Profile** (6 sub-slices, 24 tests) — ADR-0059 + ADR-0060 |
| v1o | 2026-05-09 | **Async GPU upload + sandbox integration** (3 sub-slices) — ADR-0061; full end-to-end demo: cooked profile → resolved preset bundle → ForwardRenderPath as IPresetTarget; cooker handlers for `.preset.toml`/`.profile.toml`/`.obek.toml`; `RenderMeshIndex` drop-callback hook; one ECS render path for procedurals + imports |
| v1p | 2026-05-10 | **Reserved-slot freeze — closes Phase 3.0** (this slice) |

**ADRs realised:** 0049 (entity / SlotMap) · 0050 (storage backends Archetype + SparseSet) · 0051 (relations as first-class) · 0052 (Query · System · Schedule) · 0053 (Component index slot framework) · 0054 (Transform hierarchy update model) · 0055 (Scene serialization TOML + SCEN CRDR) · 0056 (Reserved L6–L8 slots) · 0057 (UI in scene tree boundary) · 0058 (Öbek system) · 0059 (Preset system) · 0060 (Profile system) · 0061 (Async GPU upload contract). **All 13 in `Accepted` status.**

**Test counts (post-v1p):** 856 / 853 in optimised configs. Twelve-config sweep all green:

| Win × 7 | Tests | Linux × 5 | Tests |
|---|---|---|---|
| win-debug | 856/856 | linux-gcc-debug | 856/856 |
| win-relwithdebinfo | 856/856 | linux-gcc-relwithdebinfo | 856/856 |
| win-release | 853/853 | linux-gcc-release | 853/853 |
| win-asan | 856/856 | linux-gcc-asan | 856/856 |
| win-clang-cl | 856/856 | linux-gcc-shipping | clean |
| win-shipping | clean | | |
| win-tidy | clean | | |

(853 = 856 − 3 debug-only `FiberState` tests gated by `#if CRD_ENABLE_ASSERTS`.)

---

## What's deliberately NOT in v1p

- **`.group_by<T>()` query operator** — deferred to its consumer phase (4.0 ScriptSystem). Reserving a passthrough shape was rejected per advisor: the operator's return type must be a group-iterator wrapper (different *shape* from the flat range-for that v1p ships), and pinning that shape now without a real consumer would lock in the wrong design.
- **`world.view<T>().at(entity, frame_offset)` history accessor** — ADR-0053 §5 reserves this on `world.view<T>()`, not on Query. The view-form was never implemented in v1i and is not part of the freeze; lights up in Phase 3.2 (animation interpolation, rollback).
- **Replication packet format / event-log binary layout** — not part of the registration grammar; lives in Phase 4.2 ADR.
- **ScriptVTable / ScriptRegistry** — Phase 4.0 ADR-0034 + ADR-0056 §2.

These are explicit deferrals — listed here so the gap is visible to future agents and so the freeze test's coverage scope is clear.

---

## Files touched

```
engine/scene/include/crd/scene/script_component.hpp           created (~70 lines)
engine/scene/include/crd/scene/query.hpp                      +20 lines (in_aabb / within_radius declarations + math include)
engine/scene/include/crd/scene/world.hpp                      +50 lines (in_aabb / within_radius inline bodies)

tests/scene/test_phase_3_0_freeze.cpp                         created (~280 lines, 5 cases + 50+ static_asserts)
tests/scene/CMakeLists.txt                                    +1 source file + 3 PRIVATE link libs (crd-math + crd-preset + crd-profile)

docs/phases/phase-3.0-scene-ecs.md                            v1p row → ✅ shipped; phase status → CLOSED 2026-05-10
docs/sessions/2026-05-10-v1p-reserved-slot-freeze.md          this file
context.md                                                    dashboard updated
```

---

## Phase 3.0 retrospective — the whole foundation in one pass

Five intentionally-distinct authoring substrates landed in this phase, all riding the **eight-layer slot-shaped architecture** that's Cerid's signature:

1. **The ECS core (v1a–v1j)** — SlotMap entities + Archetype/SparseSet hybrid storage + Relations + Query DSL + 7-phase Schedule + Commands + Index framework + Transform propagation. The "extensible-from-day-one" property comes from the IComponentIndex slot framework (ADR-0053): adding a new ECS extension (history, spatial, GPU-mirror, replication, scripts, reflection) is a one-day plug-in, not a refactor.

2. **The serialization layer (v1k–v1l)** — SceneResource + SceneLoader + cook_scene cooker handler + built-in TOML readers for Transform + the six built-in relations. Authoring text, runtime binary; deterministic bit-exact cook → SCEN CRDR round-trip; cooker shares the World instance shape so component readers are reusable across scenes and öbeks.

3. **The Öbek system (v1m, 12 sub-slices)** — cooked entity-graph templates with extends chains + cycle detection, nested öbek references, runtime override patches with stable file_idx + symbolic name fallback, cook-time `overrides=[...]` baked into OOVR chunks, three InheritPolicy values including transparent CoW with content-hash dedup, revert at four granularities (field/component/entity/all), unpack semantics (with and without keep-overrides), AAAA-tier batch reservations + GUID stability. **No mainstream engine has this combination today** — Unreal prefab variants and Unity prefabs don't reach the CoW + 4-granularity-revert tier.

4. **The Preset + Profile stack (v1n, 6 sub-slices)** — typed `PresetResource` + per-type `PresetLoader` + closed-by-types `PresetRegistry` for QualityPreset + CameraPreset; five-layer resolver (default → extends → preset → instance → runtime); `ProfileResolver` with closed predicates (Os / GpuTier / Domain / Mode / TargetFps / CpuCores) and **additive composition** (priority-sorted stack — Cerid-distinct vs Unreal first-match-wins) + boot-time `ProfileContext` detection. Designed cross-domain from day one (games / sim / DAW / cinematic each have different quality + perf profiles).

5. **Async GPU upload + sandbox integration (v1o, 3 sub-slices)** — ADR-0061's three-layer contract: `crd-rhi::Fence` + non-waiting submit (v1o1) → `UploadHandle` + `RenderUploadSystem` polling in RenderExtract phase (v1o2) → sandbox integration with `RenderMeshIndex` drop-callback hook + ForwardRenderPath as `IPresetTarget` + three new cookers (`.preset.toml` / `.profile.toml` / `.obek.toml`) + demo content + single ECS render path for procedurals + imports (v1o3). The sandbox is now the same shape any downstream consumer would have: `register_component → register_index → register_system → step → query → submit`.

6. **The reserved-slot freeze (v1p)** — every L6/L7/L8 surface that consumer phases will need is shipped today as a no-op stub or compile-time pin, so caller code written for Phase 4.0 / 4.2 / 7 compiles + registers cleanly today. **No registration-grammar changes will be required when those phases ship.** This is what "extensible from day one" means in practice.

---

## Next

The strategic comparison + plan from earlier today (industry research + Cerid capability audit) recommends:

> **Phase 3.0 v1p (this) → Phase 3.5 (PBR + IBL + cascaded shadow maps + punctual lights, ~3–4 weeks) → Phase 3.1 (PhysX backend, ~3–4 weeks) → robotics-sim vertical-slice demo (~2–3 weeks).**

Phase 3.5 has the highest demo-leverage per LOC — the renderer is where Cerid most lags consumer engines today, and the leap from "lit cube" to "PBR scene with shadows" makes the rest of the engine immediately demoable. The cooker / scene / öbek / preset stack are *already* what's needed to drive a demo scene; the renderer just needs to grow into them.

After PBR + physics, the first cross-domain vertical slice (a tabletop robotics simulation with profile selection between "high-fidelity desktop" and "headless training" mode) becomes a credible thing to build, and exposes the integration debts the abstract roadmap can't predict.

Phase 3.0 is closed. The substrate is real. Time to start using it.
