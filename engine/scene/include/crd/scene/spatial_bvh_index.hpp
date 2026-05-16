#pragma once

// ---------------------------------------------------------------------------
// crd-scene — `SpatialBVHIndex`, the spatial Component-Index L5 slot
// (Phase 3.1.7 v5-index-bringup; ADR-0053 §6 + ADR-0076 §20 + new ADR
// candidate "spatial index extractor pattern").
//
// This class **promotes the no-op `SpatialBVHIndex` shell** from Phase 3.0
// v1i (`reserved_indexes.hpp`) to a real implementation backed by
// `crd::geometry::spatial::LooseOctree` (Ulrich 2000 — see ADR-0076 §20).
// Choice of LooseOctree backend per Phase 3.1.7 v5 cluster — it's the
// workhorse for dynamic AABB indexing (eylem broadphase + scene cull
// share this property) and is naturally const-safe-by-construction (each
// object lives in exactly one cell — see
// `feedback_spatial_substrate_thread_safety.md` + Ulrich's invariant).
//
// ── ADR-0053 day-one promise PRESERVED ────────────────────────────────────
//
// The auto-register path in `World::auto_register_indexes_for` constructs
// `SpatialBVHIndex` whenever a component carries the `SpatialBVH{}` trait.
// That happens BEFORE the user has any chance to provide spatial config
// (octree bounds, extractor function). To keep the day-one promise — "user
// code that did `register_component<Renderable>(SpatialBVH{})` in Phase 3.0
// just works once the index ships" — the index ships with TWO STATES:
//
//   1. **Unconfigured** (auto-registered, no extractor) — every storage
//      event is a no-op; queries return empty; the index does nothing.
//      This is what existing day-one user code sees automatically.
//   2. **Configured** (user called `configure(extractor, opts)`) — every
//      `on_insert` calls the extractor → builds an AABB → inserts into
//      the LooseOctree; `on_update` updates; `on_remove` removes;
//      queries return real entities.
//
// The user opts in by (a) registering components with `SpatialBVH{}`
// (auto-creates the index slot), (b) calling
// `world.find_index<SpatialBVHIndex>()->configure(...)` once the
// extractor is ready (typically right after component registration).
//
// ── AABB extraction via `IAabbExtractor` ──────────────────────────────────
//
// The index doesn't know how to compute world-space AABBs from component
// bytes (the storage-event API hands it `(EntityId, ComponentId, void*)`
// and that's it). The user provides an `IAabbExtractor` that bridges:
//
// ```cpp
// struct MyExtractor : crd::scene::IAabbExtractor {
//     World* world;
//     AABB3<f32> extract(EntityId e) const override {
//         const Transform& t = world->view<Transform>().at(e);
//         const Bounds& b = world->view<Bounds>().at(e);
//         return crd::geometry::primitives::transform_aabb(b.local, t.world);
//     }
// };
// ```
//
// This is the **canonical extractor pattern** for any future spatial-
// index style (animation-bounds index, light-influence index, occlusion
// index). Pin the pattern; reuse it.
//
// ── How consumers UPDATE a watched component's data ──────────────────────
//
// The right API to update an entity's bounds is **UPSERT** via
// `world.add_component(e, new_value)` — this fires per-entity
// `on_update` through the storage event sink, which the index hooks to
// refresh the octree. **`world.get_component_mut<T>(e)` does NOT fire
// on_update** (it only bumps the chunk-version for ChangeDetect's
// hint-grade signal); use it only for non-spatial mutations.
//
// ── Hooks ─────────────────────────────────────────────────────────────────
//
//   * `on_insert(e, c, data)` — when a *watched* component is added: extract
//     entity AABB, insert into octree, store handle in
//     `m_entity_to_handle[e]`.
//   * `on_update(e, c, old, new_)` — same entity may carry multiple watched
//     components; an update from any of them triggers extractor +
//     octree.update() (the LooseOctree's same-AABB-fit fast-path absorbs
//     no-op moves).
//   * `on_remove(e, c, data)` — when the LAST watched component is removed,
//     remove from octree + drop handle. (If only one of N watched
//     components is removed, entity stays in octree.)
//   * `on_entity_destroyed(e)` — same as last-watched-component-removed.
//
// ── Query API ─────────────────────────────────────────────────────────────
//
//   * `overlap(box, out)` — append every entity whose AABB overlaps the
//     query box.
//   * `raycast(ray)` — nearest entity hit, or `nullopt`.
//   * `closest_point(point, out)` — wrapper around overlap(point-as-tiny-box)
//     for now; future enhancement.
//
// All queries are `const` and **naturally thread-safe** (LooseOctree is
// const-safe by construction; the entity-handle table is const-iterated;
// no `mutable` member, no per-query state). Concurrent queries via
// `crd::jobs::parallel_for` are safe with per-fiber output Arrays — same
// pattern as the v5 thread-safety validation pass.
//
// ── Two-layer typing (ADR-0078 §5) ────────────────────────────────────────
// Storage hand IDs raw `f32` AABBs (extracted via `IAabbExtractor` which
// returns raw `AABB3<f32>` per the lower-layer rule). Index queries take
// raw AABB/Ray/Vec at the API surface — typed `AABB3<Length32>` callers
// bridge via `to_raw_vec` / `from_raw_vec` at the call site.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/geometry/spatial/loose_octree.hpp>
#include <crd/scene/component_index.hpp>
#include <crd/scene/entity.hpp>

#include <memory>
#include <optional>

namespace crd::scene
{

// IAabbExtractor — the canonical "compute world-space AABB for entity"
// interface. SpatialBVHIndex calls this on every observed storage event
// to map (entity, component_id, data) → AABB3<f32>. Reusable for future
// spatial indexes (animation bounds, light influence, occlusion volumes).
//
// **Why `(EntityId, ComponentId, const void* data)` not just `(EntityId)`?**
// During `on_insert` the storage backend has WRITTEN the bytes but the
// entity's archetype migration may still be in progress — `world.get_component<T>(e)`
// can return nullptr until the migration commits. The `data` pointer IS
// the freshly-installed bytes (per `IStorageEventSink` contract), so the
// extractor reads them directly with zero ordering hazard. For multi-
// component scenarios (extract uses Transform + Bounds), the extractor
// implementation can read OTHER components via the World — the
// just-installed one's data is the trigger.
class IAabbExtractor
{
public:
    virtual ~IAabbExtractor() = default;

    // Compute the world-space AABB for `entity`. `component` + `data`
    // identify the just-installed/updated component that triggered this
    // call (per IStorageEventSink contract — `data` points to the live
    // component bytes for the duration of this call). Return value is in
    // world space, raw `f32` per ADR-0078 §5 (typed callers bridge at
    // their entry point).
    [[nodiscard]] virtual crd::geometry::primitives::AABB3<crd::f32>
    extract(EntityId entity, ComponentId component, const void* data) const = 0;
};

class SpatialBVHIndex final : public IComponentIndex
{
public:
    explicit SpatialBVHIndex(crd::memory::IAllocator* alloc);

    // ---- IStorageEventSink (via IComponentIndex) -----------------------

    void on_insert(EntityId e, ComponentId c, const void* data) override;
    void on_update(EntityId e, ComponentId c, const void* old_data, const void* new_data) override;
    void on_remove(EntityId e, ComponentId c, const void* data) override;
    void on_entity_destroyed(EntityId e) override;

    // ---- IComponentIndex -----------------------------------------------

    [[nodiscard]] ComponentMask observed() const override { return m_observed; }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"SpatialBVHIndex"};
    }

    // Add `c` to the observed mask. Auto-called by World on every component
    // registration that carries the `SpatialBVH{}` trait.
    void watch(ComponentId c) noexcept { m_observed.set(c); }

    // ---- Configuration -------------------------------------------------

    // Configure the index with a backing LooseOctree + AABB extractor.
    // Must be called before any watched-component insert event for the
    // index to do real work; until then, all hooks are silent no-ops
    // (preserving the ADR-0053 day-one promise that registering for a
    // deferred trait silently no-ops at runtime).
    //
    // `extractor` lifetime: caller-owned, must outlive this index. Pass
    // a long-lived instance — typically constructed alongside the World.
    void configure(IAabbExtractor*                                                  extractor,
                    const crd::geometry::spatial::OctreeBuildOptions<crd::f32>&     opts);

    // Whether configure() has been called.
    [[nodiscard]] bool is_configured() const noexcept { return m_octree != nullptr; }

    // Diagnostics.
    [[nodiscard]] crd::usize tracked_entity_count() const noexcept { return m_entity_to_handle.size(); }
    [[nodiscard]] const crd::geometry::spatial::LooseOctree<crd::f32>* octree() const noexcept
    {
        return m_octree.get();
    }

    // ---- Queries (raw f32 AABBs / rays / points) -----------------------

    // Append every entity whose stored AABB overlaps `query`. Output cleared
    // at start of call. Returns silently if unconfigured.
    void overlap(const crd::geometry::primitives::AABB3<crd::f32>& query,
                  crd::containers::Array<EntityId>&                  out) const;

    // Nearest-hit raycast against entity AABBs. Lowest-payload (entity) tiebreak
    // on equal `t` per ADR-0076 §4 pin #11. Returns `nullopt` if unconfigured
    // or no hit.
    [[nodiscard]] std::optional<crd::geometry::RayHit<EntityId>>
    raycast(const crd::geometry::primitives::Ray3<crd::f32>& ray,
            crd::f32 tmax = std::numeric_limits<crd::f32>::infinity()) const noexcept;

private:
    // Index storage. Allocator captured at construction (passed by World's
    // auto-register path). LooseOctree + lookup table allocated in
    // configure().
    crd::memory::IAllocator*                                              m_alloc{nullptr};
    ComponentMask                                                          m_observed{};

    // Configured state — null until configure() is called.
    std::unique_ptr<crd::geometry::spatial::LooseOctree<crd::f32>>         m_octree;
    IAabbExtractor*                                                        m_extractor{nullptr};

    // Entity-id → octree handle. Used by update + remove to look up the
    // octree slot for a given entity. Keyed by EntityId.raw (the 64-bit
    // packed representation; HashMap<EntityId,...> works via the
    // DefaultHash specialisation in entity.hpp).
    crd::containers::HashMap<EntityId, crd::geometry::spatial::OctreeObjectId> m_entity_to_handle;

    // Reverse lookup for query results: octree stores `entity.index()` as
    // its u32 payload (octree payload is u32-only). This map reconstructs
    // the full EntityId (with generation) from the index. Maintained in
    // lockstep with m_entity_to_handle.
    crd::containers::HashMap<crd::u32, EntityId> m_index_to_entity;

    // Internal: insert / refresh / remove this entity's octree slot. Idempotent.
    // The `component` + `data` triple plumbs through from on_insert/on_update
    // and is forwarded to the extractor.
    void index_insert_or_update(EntityId e, ComponentId component, const void* data);
    void index_remove(EntityId e);
};

} // namespace crd::scene
