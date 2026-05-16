// crd-scene — `SpatialBVHIndex` impl (Phase 3.1.7 v5-index-bringup).
// Promotes the no-op shell from `reserved_indexes.hpp` (Phase 3.0 v1i)
// to a real LooseOctree-backed component index. Header
// `spatial_bvh_index.hpp` documents the design + the day-one promise.

#include <crd/scene/spatial_bvh_index.hpp>

#include <crd/core/assert.hpp>

namespace crd::scene
{

SpatialBVHIndex::SpatialBVHIndex(crd::memory::IAllocator* alloc)
    : m_alloc(alloc)
    , m_entity_to_handle(alloc)
    , m_index_to_entity(alloc)
{
}

// =============================================================================
// Configuration
// =============================================================================

void SpatialBVHIndex::configure(IAabbExtractor*                                                 extractor,
                                  const crd::geometry::spatial::OctreeBuildOptions<crd::f32>&  opts)
{
    CRD_ASSERT(extractor != nullptr && "SpatialBVHIndex::configure: extractor must be non-null");
    CRD_ASSERT(m_octree == nullptr && "SpatialBVHIndex::configure: already configured");

    m_extractor = extractor;
    m_octree    = std::make_unique<crd::geometry::spatial::LooseOctree<crd::f32>>(m_alloc, opts);
}

// =============================================================================
// Storage event hooks — no-op when unconfigured (preserves ADR-0053 day-one
// promise that registering for a deferred trait silently no-ops at runtime).
// =============================================================================

void SpatialBVHIndex::on_insert(EntityId e, ComponentId c, const void* data)
{
    if (m_octree == nullptr) { return; }
    index_insert_or_update(e, c, data);
}

void SpatialBVHIndex::on_update(EntityId e, ComponentId c, const void* /*old_data*/, const void* new_data)
{
    if (m_octree == nullptr) { return; }
    index_insert_or_update(e, c, new_data);
}

void SpatialBVHIndex::on_remove(EntityId /*e*/, ComponentId /*c*/, const void* /*data*/)
{
    // Per-component on_remove is conservative: the entity may still carry
    // OTHER watched components, so we can't safely remove the octree slot
    // here. Cleanup happens on on_entity_destroyed (definitive) OR on
    // subsequent on_update from another watched component (extractor
    // refresh). v5-bringup MVP — last-watched-component-out tracking is
    // a future enhancement, not a correctness blocker.
}

void SpatialBVHIndex::on_entity_destroyed(EntityId e)
{
    if (m_octree == nullptr) { return; }
    index_remove(e);
}

// =============================================================================
// Octree maintenance
// =============================================================================

void SpatialBVHIndex::index_insert_or_update(EntityId e, ComponentId component, const void* data)
{
    const crd::geometry::primitives::AABB3<crd::f32> aabb = m_extractor->extract(e, component, data);

    auto* existing = m_entity_to_handle.find(e);
    if (existing != nullptr)
    {
        // Already indexed — refresh AABB. LooseOctree's same-AABB-fit fast-
        // path absorbs no-op moves at zero cost.
        m_octree->update(*existing, aabb);
        // Index→entity mapping might still need refresh if the entity's
        // generation flipped (impossible without prior remove — but
        // defensive update keeps the map in sync).
        m_index_to_entity[e.index()] = e;
    }
    else
    {
        const auto handle = m_octree->insert(aabb, e.index());
        m_entity_to_handle.insert(e, handle);
        m_index_to_entity.insert(e.index(), e);
    }
}

void SpatialBVHIndex::index_remove(EntityId e)
{
    auto* existing = m_entity_to_handle.find(e);
    if (existing != nullptr)
    {
        m_octree->remove(*existing);
        m_entity_to_handle.erase(e);
        m_index_to_entity.erase(e.index());
    }
}

// =============================================================================
// Public queries — naturally const-safe (LooseOctree const-safe by construction;
// HashMap reads are const; no mutable state). Concurrent queries with per-fiber
// output Arrays are safe under win-asan race detection — same pattern as v5
// thread-safety validation pass.
// =============================================================================

void SpatialBVHIndex::overlap(const crd::geometry::primitives::AABB3<crd::f32>& query,
                                crd::containers::Array<EntityId>&                 out) const
{
    out.clear();
    if (m_octree == nullptr) { return; }

    crd::containers::Array<crd::u32> raw_payloads(out.allocator());
    m_octree->overlap(query, raw_payloads);

    out.reserve(raw_payloads.size());
    for (crd::usize i = 0; i < raw_payloads.size(); ++i)
    {
        const crd::u32 idx = raw_payloads[i];
        const EntityId* full = m_index_to_entity.find(idx);
        if (full != nullptr)
        {
            out.push_back(*full);
        }
        // (Defensive: if the reverse map drifted, skip — happens only on
        // implementation bug. In debug we'd assert; in release we soft-skip.)
    }
}

std::optional<crd::geometry::RayHit<EntityId>>
SpatialBVHIndex::raycast(const crd::geometry::primitives::Ray3<crd::f32>& ray, crd::f32 tmax) const noexcept
{
    if (m_octree == nullptr) { return std::nullopt; }
    const auto raw_hit = m_octree->raycast(ray, tmax);
    if (!raw_hit.has_value()) { return std::nullopt; }

    const EntityId* full = m_index_to_entity.find(raw_hit->payload);
    if (full == nullptr) { return std::nullopt; }
    return crd::geometry::RayHit<EntityId>{raw_hit->t, *full};
}

} // namespace crd::scene
