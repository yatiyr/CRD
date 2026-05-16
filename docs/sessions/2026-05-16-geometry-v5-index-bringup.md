# Session log — 2026-05-16 — geometry v5-index-bringup

> Phase 3.1.7 v5 `-spatial` cluster's first non-backend slice. Promotes the
> Phase 3.0 v1i `SpatialBVHIndex` reserved-shell (a no-op stub honoring
> ADR-0053's day-one trait grammar) to a **real `LooseOctree`-backed
> component index** in `crd-scene`. First non-reserved spatial index in
> Cerid's ECS. The fan-out path from storage events → octree maintenance
> → spatial queries returning `EntityId`s.

## Scope landed

| Element | Path |
|---|---|
| New index class | `engine/scene/include/crd/scene/spatial_bvh_index.hpp` |
| Implementation | `engine/scene/src/spatial_bvh_index.cpp` |
| Removed no-op shell | `engine/scene/include/crd/scene/reserved_indexes.hpp` (replaced with re-export of the real class) |
| CMake dep edge added | `engine/scene/CMakeLists.txt` — `crd-scene` PUBLIC link `crd-geometry-spatial` |
| Tests | `tests/scene/test_spatial_bvh_index.cpp` (8 cases, incl. fiber-jobified concurrent) |
| Test CMake update | `tests/scene/CMakeLists.txt` — add new test + `crd-jobs` link |

## API surface

```cpp
namespace crd::scene {

// User-implemented bridge: storage events → world-space AABBs. Receives
// the EntityId + ComponentId + raw component data pointer (per
// IStorageEventSink contract — the freshly-installed bytes).
class IAabbExtractor {
public:
    virtual ~IAabbExtractor() = default;
    [[nodiscard]] virtual crd::geometry::primitives::AABB3<crd::f32>
    extract(EntityId, ComponentId, const void* data) const = 0;
};

class SpatialBVHIndex final : public IComponentIndex {
public:
    explicit SpatialBVHIndex(IAllocator*);

    // IStorageEventSink — no-op when unconfigured; real work when configured.
    void on_insert(EntityId, ComponentId, const void* data) override;
    void on_update(EntityId, ComponentId, const void* old, const void* new_) override;
    void on_remove(EntityId, ComponentId, const void* data) override;
    void on_entity_destroyed(EntityId) override;

    // Configuration (call before any watched-component insert).
    void configure(IAabbExtractor*, const OctreeBuildOptions<f32>&);
    [[nodiscard]] bool is_configured() const noexcept;

    // Queries — both return EntityIds.
    void overlap(const AABB3<f32>&, Array<EntityId>& out) const;
    [[nodiscard]] std::optional<RayHit<EntityId>>
    raycast(const Ray3<f32>&, f32 tmax = +∞) const noexcept;

    // Diagnostics.
    [[nodiscard]] usize tracked_entity_count() const noexcept;
    [[nodiscard]] const LooseOctree<f32>* octree() const noexcept;
};

} // namespace
```

## Algorithm + design discipline

### Day-one promise PRESERVED (ADR-0053 §2 grammar)

The auto-register path in `World::auto_register_indexes_for` constructs
`SpatialBVHIndex` whenever a component carries the `SpatialBVH{}` trait.
This happens BEFORE the user can configure spatial parameters (octree
bounds, extractor). To keep the promise — "user code that did
`register_component<Renderable>(SpatialBVH{})` in Phase 3.0 just works
once the index ships" — the class ships with TWO STATES:

1. **Unconfigured** (default after auto-register) — every storage event
   is a no-op; queries return empty. Identical observable behaviour to
   the no-op shell it replaces.
2. **Configured** (user called `configure(extractor, opts)`) — events
   call the extractor → maintain the LooseOctree → queries return real
   entities.

Existing Phase-3.0 user code that did
`register_component<X>(SpatialBVH{})` continues to compile + run with
no behaviour change. Opting in is one line:
`world.find_index<SpatialBVHIndex>()->configure(&my_extractor, opts);`.

### IAabbExtractor takes `(EntityId, ComponentId, const void* data)`

NOT just `(EntityId)` — the user's first instinct. Reason: during
`on_insert`, the storage backend has WRITTEN the bytes but the
entity's archetype migration may not be fully committed. A naïve
`world.get_component<T>(e)` from inside the extractor returns nullptr
mid-migration. The `data` pointer IS the freshly-installed bytes (per
the `IStorageEventSink` contract); the extractor reads it directly with
zero ordering hazard.

This is the **canonical extractor pattern** for any future spatial-
index style (animation bounds, light influence, occlusion volumes).
Pin the pattern; reuse it.

### Storage event hook semantics

* **`on_insert`** — fires when a watched component is added. Extractor
  produces the AABB; octree inserts; `m_entity_to_handle` records the
  handle.
* **`on_update`** — fires when a watched component is upserted via
  `world.add_component(e, new_value)` (UPSERT). Extractor produces
  new AABB; octree's `same-AABB-fit` fast-path absorbs no-op moves at
  zero cost (per LooseOctree v5b correctness invariant).
* **`on_remove`** — per-component remove is conservative no-op. The
  entity may still carry OTHER watched components; we can't safely
  evict the octree slot from this event alone. Cleanup happens on
  `on_entity_destroyed` (definitive) or on subsequent `on_update` from
  another watched component (extractor refresh). Documented as v5-bringup
  MVP; per-component last-out tracking is a future enhancement.
* **`on_entity_destroyed`** — definitively removes the octree slot.

### Update via UPSERT, not `get_component_mut`

A subtle ECS contract: `world.get_component_mut<T>(e)` returns a
mutable pointer + bumps the chunk-version counter (ChangeDetect's
hint-grade signal) but **does NOT fire per-entity `on_update`**.
Spatial-index consumers MUST use `world.add_component(e, new_value)`
(UPSERT semantics — works whether the component existed before or not).

Documented prominently in the header.

### Query result reconstruction

`LooseOctree`'s payload is `u32` — too narrow for the 64-bit
`EntityId.raw`. Solution: store `entity.index()` (32 bits) as the octree
payload + maintain a side `HashMap<u32 index, EntityId>` for reverse
lookup. Insert/remove keep both maps in sync. Query lookup is O(1).
Documented in the impl.

### Threading

Queries are **naturally const-safe**: LooseOctree is const-safe by
construction (each object lives in exactly one cell — Ulrich's invariant
+ feedback memory `spatial-substrate-thread-safety`). The
`m_entity_to_handle` + `m_index_to_entity` maps are const-iterated
during queries — no mutable state, no `const_cast` writes. The
concurrent-queries-via-`crd::jobs` test validates the claim empirically
under win-asan race detection.

## Locked design choices (carries into ADR-0053 update + ADR-0076 §20)

| # | Decision | Rationale |
|---|---|---|
| 1 | Promote `SpatialBVHIndex` from no-op shell to real impl (same class name); two-state design (unconfigured = no-op, configured = real) | Preserves ADR-0053 day-one promise that registering for a deferred trait just works once the impl ships |
| 2 | `IAabbExtractor::extract(EntityId, ComponentId, const void* data)` (not just EntityId) | Avoids the `get_component` mid-migration null-return hazard; `data` is the freshly-installed bytes per IStorageEventSink contract |
| 3 | Backend = `LooseOctree<f32>` (per ADR-0076 §20 — v5b's "workhorse for scene spatial culling") | Cell-residency-by-Ulrich-invariant makes queries naturally const-safe |
| 4 | Storage handle = `u32 entity.index()` in octree payload + side `HashMap<u32, EntityId>` for reverse lookup | LooseOctree payload is u32-only; preserves O(1) query-result reconstruction |
| 5 | UPSERT (`world.add_component`) is the ONLY way to update spatial bounds; `get_component_mut` is documented as NOT firing per-entity `on_update` | Matches existing ECS contract; documented prominently in header |
| 6 | `on_remove` per-component is no-op; only `on_entity_destroyed` cleans up | Conservative — entity may carry other watched components; full last-out tracking deferred |
| 7 | Queries are const + naturally thread-safe (no mutable state); concurrent-via-`crd::jobs` test validates empirically | Same `spatial-substrate-thread-safety` feedback rule applied at the scene layer |
| 8 | `find_index<T>()` accessor on World (already existed pre-bringup) is the user's entry to grab the index for configuration | Avoids needing a typed `world.spatial_bvh_index()` accessor; symmetric across index types |

## Tests — 8 cases / 5-config DoD PASS

Suite breakdown:

| Case | Coverage |
|---|---|
| auto-registers as no-op when `SpatialBVH{}` set | Verifies day-one promise — unconfigured index dispatches storage events without crashing or tracking |
| configure wires up real LooseOctree-backed indexing | After `configure()`, `is_configured()` true; subsequent inserts populate the index |
| overlap query returns matching entities | 3 entities, query a region — returns the 2 inside, not the 1 outside |
| update reflects component changes via on_update | Insert → query finds at old loc → UPSERT new bounds → query no longer finds at old loc, finds at new loc |
| remove via destroy_immediate drops the entry | Entity destroy fires `on_entity_destroyed` → octree slot freed → no future query finds it |
| raycast picks nearest entity | 3 entities at distinct positions, ray finds nearest |
| raycast unconfigured returns nullopt | Day-one promise extension to raycast |
| **concurrent overlap queries via crd-jobs** | 16 queries × 25 iterations = 400 fan-out tasks across 16 jobs / 4 worker fibers; per-task isolated TlsfAllocator + output Array; atomic mismatches == 0 under win-asan race detection — proves the naturally-const-safe claim empirically |

## Per-slice DoD — 5 configs PASS

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 2077 / 2077 ctest |
| win-asan | PASS | full project ctest — race detection clean across the 400-task fiber-jobified test |
| win-shipping | PASS | LTCG-clean |
| win-shipping-profile | PASS | 2072 / 2072 ctest under `CRD_ENABLE_PROFILING=ON` + LTCG |
| win-tidy | PASS | clang-tidy clean |

`scripts/per-slice-check.ps1` gates the first 4; win-shipping-profile run
separately. Total full-project ctest: **2069 → 2077 win-debug** (+8 cases
from this slice).

## Three debugging passes en route

1. **Wrong HashMap iterator API** — used `for (auto& kv : map)` + `kv.first`/`kv.second`. `crd::containers::HashMap` iterator exposes `.key()` and `.value()` methods (no `operator*`). Refactored to use `find()` + explicit lookup instead of iteration (cleaner anyway — the reverse-lookup `HashMap<u32, EntityId>` makes overlap query O(1)).

2. **Wrong World API** — used `create_entity` / `destroy_entity` / `view<T>().at(e)`. Actual API: `spawn()` / `destroy_immediate()` / `get_component<T>(e)`. Fixed.

3. **Extractor mid-migration null-pointer** — initial `IAabbExtractor::extract(EntityId)` called `world.get_component<T>(e)` from inside `on_insert`. Returned nullptr because storage migration hadn't committed. Fix: extractor takes `(EntityId, ComponentId, const void* data)` and reads from the IStorageEventSink-supplied `data` pointer (which IS the freshly-installed bytes per the v1i contract). Canonical pattern for any future spatial-index style.

4. **`get_component_mut` doesn't fire on_update** — initial update test used `get_component_mut<T>(e)` and assigned. Index didn't see the change. Fix: use `world.add_component(e, new_value)` (UPSERT semantics — fires per-entity on_update). Documented prominently in header.

## Cross-substrate observation

This is the **first non-reserved spatial index in `crd-scene`** — closes
the ADR-0053 reserved-shell loop for SpatialBVH (HistoryIndex,
GpuResidentIndex, ReplicationIndex, ReflectionIndex remain reserved
no-op shells, lighting up in their consumer phases per ADR-0053 §5/§7).

The pattern locked here (extractor interface + two-state index +
LooseOctree backing + concurrent-jobs test) becomes the template for
future spatial extensions:
* `LightInfluenceIndex` (Phase 3.5 clustered lighting)
* `OcclusionIndex` (Phase 3.5+ Hi-Z occlusion)
* `AudioOcclusionIndex` (Phase 3.4 ray-traced acoustics)
* `AcousticVolumeIndex` (any future spatially-bounded extension)

Each follows: implement `IComponentIndex` + provide a user-extractor +
back onto a v5 `-spatial` backend + ship with the `(EntityId,
ComponentId, void* data)` extractor signature.

## Next

Phase 3.1.7 v5 `-spatial` cluster continues:
* **v5-queries-extension** — extend `crd/geometry/queries.hpp` compile-
  time-overload facade to dispatch over all 5 v5 backends. ~1 day.
* **v5-close** — ADR-0076 §20 amendment + `docs/systems/geometry-spatial.md`
  + 18-config full sweep. ~1 day.
