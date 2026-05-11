// ColliderPool impl. Phase 3.1 v1b-b.

#include <crd/eylem_rigid3d/collider_pool.hpp>

#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>

CRD_DEFINE_LOG_CHANNEL(g_log_collider, "EylemColliderPool", crd::log::LogLevel::Info)

namespace crd::eylem_rigid3d
{
namespace
{
// Same store/modify/load pattern as BodyPool's put_lane. Templated on
// column type so exactly one specialisation gets emitted per build
// (Vec8f on AVX2, Vec4f on scalar fallback) — no unused-function
// warnings under -Werror,-Wunused-function.
template <typename ColT>
CRD_FORCEINLINE void put_lane(ColT& col, crd::usize lane_idx, crd::f32 value) noexcept
{
    constexpr crd::usize kLanes = sizeof(ColT) / sizeof(crd::f32);
    crd::f32 buf[kLanes];
    col.store(buf);
    buf[lane_idx] = value;
    col = ColT::load(buf);
}

// Bump generation, wrapping 0xFF -> 1 (0 stays reserved as "never allocated").
[[nodiscard]] CRD_FORCEINLINE crd::u32 next_generation(crd::u8 prev) noexcept
{
    crd::u32 next = static_cast<crd::u32>(prev) + 1U;
    if (next > 0xFFu) next = 1U;
    return next;
}

// chunk index / lane index from a per-kind local index.
[[nodiscard]] CRD_FORCEINLINE crd::u32 chunk_of(crd::u32 idx, crd::u32 lane) noexcept { return idx / lane; }
[[nodiscard]] CRD_FORCEINLINE crd::u32 lane_of (crd::u32 idx, crd::u32 lane) noexcept { return idx % lane; }

// Reserve one new slot in a per-kind pool. Returns the slot's local index,
// or 0 (== reserved-null) if at capacity. Grows the SoA storage as needed.
template <typename PerKind>
[[nodiscard]] crd::u32 acquire_slot(PerKind& pk, crd::u32 capacity) noexcept
{
    crd::u32 idx = 0;
    if (pk.free_head != 0)
    {
        idx          = pk.free_head;
        pk.free_head = pk.free_next[idx];
    }
    else if (pk.high_water < capacity)
    {
        idx = pk.high_water++;
    }
    else
    {
        return 0; // capacity exhausted
    }

    const auto needed_chunks =
        (idx / static_cast<crd::u32>(ColliderPool::kLane)) + 1U;
    if (pk.storage.chunk_count() < needed_chunks)
    {
        pk.storage.resize(static_cast<crd::usize>(needed_chunks) * ColliderPool::kLane);
    }
    return idx;
}

template <typename PerKind, typename TileType>
void mark_live(PerKind& pk, crd::u32 idx, crd::u32 generation) noexcept
{
    TileType&      tile = pk.storage.chunk(chunk_of(idx, static_cast<crd::u32>(ColliderPool::kLane)));
    const crd::u32 lane = lane_of(idx, static_cast<crd::u32>(ColliderPool::kLane));
    tile.generation[lane] = static_cast<crd::u8>(generation);
    tile.live[lane]       = 1;
    ++pk.live_count;
}
} // namespace

ColliderPool::ColliderPool(crd::memory::IAllocator* persistent_alloc, crd::u32 max_per_kind)
    : m_spheres (persistent_alloc != nullptr ? persistent_alloc : crd::memory::default_allocator())
    , m_boxes   (persistent_alloc != nullptr ? persistent_alloc : crd::memory::default_allocator())
    , m_capsules(persistent_alloc != nullptr ? persistent_alloc : crd::memory::default_allocator())
    , m_capacity_per_kind(std::min(max_per_kind, kColliderPerKindMax))
{
    auto seed_per_kind = [this](auto& pk) {
        pk.free_next.resize(m_capacity_per_kind);
        if (m_capacity_per_kind > 0)
        {
            pk.storage.resize(1); // sentinel chunk for slot 0
        }
    };
    seed_per_kind(m_spheres);
    seed_per_kind(m_boxes);
    seed_per_kind(m_capsules);
}

ColliderId ColliderPool::insert(BodyId body, const Collider& collider)
{
    if (body.is_null()) return ColliderId::null();

    crd::u32 per_kind_idx = 0;
    crd::u32 generation   = 0;
    ColliderShape kind    = collider.shape;

    switch (kind)
    {
        case ColliderShape::Sphere:
        {
            per_kind_idx = insert_sphere(body, collider);
            if (per_kind_idx == 0) return ColliderId::null();
            const crd::u32 ch   = chunk_of(per_kind_idx, static_cast<crd::u32>(kLane));
            const crd::u32 lane = lane_of (per_kind_idx, static_cast<crd::u32>(kLane));
            generation = static_cast<crd::u32>(m_spheres.storage.chunk(ch).generation[lane]);
            break;
        }
        case ColliderShape::Box:
        {
            per_kind_idx = insert_box(body, collider);
            if (per_kind_idx == 0) return ColliderId::null();
            const crd::u32 ch   = chunk_of(per_kind_idx, static_cast<crd::u32>(kLane));
            const crd::u32 lane = lane_of (per_kind_idx, static_cast<crd::u32>(kLane));
            generation = static_cast<crd::u32>(m_boxes.storage.chunk(ch).generation[lane]);
            break;
        }
        case ColliderShape::Capsule:
        {
            per_kind_idx = insert_capsule(body, collider);
            if (per_kind_idx == 0) return ColliderId::null();
            const crd::u32 ch   = chunk_of(per_kind_idx, static_cast<crd::u32>(kLane));
            const crd::u32 lane = lane_of (per_kind_idx, static_cast<crd::u32>(kLane));
            generation = static_cast<crd::u32>(m_capsules.storage.chunk(ch).generation[lane]);
            break;
        }
        case ColliderShape::ConvexHull:
        case ColliderShape::Plane:
            CRD_LOG_ERROR(g_log_collider,
                          "ColliderShape {} not supported until v1d (GJK + EPA / plane raycast); "
                          "Sphere/Box/Capsule are the only v1b-b shapes",
                          static_cast<int>(kind));
            return ColliderId::null();
        case ColliderShape::TriangleMesh:
            CRD_LOG_ERROR(g_log_collider,
                          "ColliderShape::TriangleMesh not supported until v1d-mesh (BVH + per-tri SAT)");
            return ColliderId::null();
        case ColliderShape::Heightfield:
            CRD_LOG_ERROR(g_log_collider,
                          "ColliderShape::Heightfield not supported until v1d-hf (per-cell analytic)");
            return ColliderId::null();
        case ColliderShape::Sdf:
            CRD_LOG_ERROR(g_log_collider,
                          "ColliderShape::Sdf not supported until Phase 3.1.5 (crd-sdf substrate)");
            return ColliderId::null();
    }

    return ColliderId::make(encode_collider_index(kind, per_kind_idx), generation);
}

void ColliderPool::remove(ColliderId id) noexcept
{
    if (!contains(id)) return;
    const crd::u32      idx          = id.index();
    const ColliderShape kind         = decode_collider_kind(idx);
    const crd::u32      per_kind_idx = decode_collider_per_kind_idx(idx);
    const crd::u32      ch           = chunk_of(per_kind_idx, static_cast<crd::u32>(kLane));
    const crd::u32      lane         = lane_of (per_kind_idx, static_cast<crd::u32>(kLane));

    auto kill_in = [&](auto& pk, auto& tile) {
        tile.live[lane]   = 0;
        pk.free_next[per_kind_idx] = pk.free_head;
        pk.free_head      = per_kind_idx;
        --pk.live_count;
    };

    switch (kind)
    {
        case ColliderShape::Sphere:  kill_in(m_spheres,  m_spheres.storage.chunk(ch));  break;
        case ColliderShape::Box:     kill_in(m_boxes,    m_boxes.storage.chunk(ch));    break;
        case ColliderShape::Capsule: kill_in(m_capsules, m_capsules.storage.chunk(ch)); break;
        default: break; // ConvexHull/Plane never reach here (insert returns null)
    }
}

bool ColliderPool::contains(ColliderId id) const noexcept
{
    if (id.is_null()) return false;
    const crd::u32      idx          = id.index();
    const ColliderShape kind         = decode_collider_kind(idx);
    const crd::u32      per_kind_idx = decode_collider_per_kind_idx(idx);
    if (per_kind_idx == 0 || per_kind_idx > m_capacity_per_kind) return false;
    const crd::u32 ch   = chunk_of(per_kind_idx, static_cast<crd::u32>(kLane));
    const crd::u32 lane = lane_of (per_kind_idx, static_cast<crd::u32>(kLane));

    auto check = [&](const auto& pk, const auto& tile) -> bool {
        if (per_kind_idx >= pk.high_water) return false;
        if (tile.live[lane] == 0)          return false;
        return static_cast<crd::u32>(tile.generation[lane]) == id.generation();
    };

    switch (kind)
    {
        case ColliderShape::Sphere:  return check(m_spheres,  m_spheres.storage.chunk(ch));
        case ColliderShape::Box:     return check(m_boxes,    m_boxes.storage.chunk(ch));
        case ColliderShape::Capsule: return check(m_capsules, m_capsules.storage.chunk(ch));
        default: return false;
    }
}

Collider ColliderPool::read(ColliderId id) const noexcept
{
    Collider out{};
    if (!contains(id)) return out;
    const crd::u32      idx          = id.index();
    const ColliderShape kind         = decode_collider_kind(idx);
    const crd::u32      per_kind_idx = decode_collider_per_kind_idx(idx);
    const crd::u32      ch           = chunk_of(per_kind_idx, static_cast<crd::u32>(kLane));
    const crd::u32      lane         = lane_of (per_kind_idx, static_cast<crd::u32>(kLane));

    out.shape = kind;

    switch (kind)
    {
        case ColliderShape::Sphere:
        {
            const auto& tile = m_spheres.storage.chunk(ch);
            out.local_position = {tile.lpos_x.lane(lane), tile.lpos_y.lane(lane), tile.lpos_z.lane(lane)};
            out.local_rotation = {tile.lrot_x.lane(lane), tile.lrot_y.lane(lane),
                                  tile.lrot_z.lane(lane), tile.lrot_w.lane(lane)};
            out.sphere = crd::eylem::ColliderSphere{tile.radius.lane(lane)};
            break;
        }
        case ColliderShape::Box:
        {
            const auto& tile = m_boxes.storage.chunk(ch);
            out.local_position = {tile.lpos_x.lane(lane), tile.lpos_y.lane(lane), tile.lpos_z.lane(lane)};
            out.local_rotation = {tile.lrot_x.lane(lane), tile.lrot_y.lane(lane),
                                  tile.lrot_z.lane(lane), tile.lrot_w.lane(lane)};
            out.box = crd::eylem::ColliderBox{
                {tile.half_x.lane(lane), tile.half_y.lane(lane), tile.half_z.lane(lane)}};
            break;
        }
        case ColliderShape::Capsule:
        {
            const auto& tile = m_capsules.storage.chunk(ch);
            out.local_position = {tile.lpos_x.lane(lane), tile.lpos_y.lane(lane), tile.lpos_z.lane(lane)};
            out.local_rotation = {tile.lrot_x.lane(lane), tile.lrot_y.lane(lane),
                                  tile.lrot_z.lane(lane), tile.lrot_w.lane(lane)};
            out.capsule = crd::eylem::ColliderCapsule{
                tile.radius.lane(lane), tile.half_height.lane(lane)};
            break;
        }
        default: break;
    }
    return out;
}

BodyId ColliderPool::body_of(ColliderId id) const noexcept
{
    if (!contains(id)) return BodyId::null();
    const crd::u32      idx          = id.index();
    const ColliderShape kind         = decode_collider_kind(idx);
    const crd::u32      per_kind_idx = decode_collider_per_kind_idx(idx);
    const crd::u32      ch           = chunk_of(per_kind_idx, static_cast<crd::u32>(kLane));
    const crd::u32      lane         = lane_of (per_kind_idx, static_cast<crd::u32>(kLane));

    crd::u32 raw_body = 0;
    switch (kind)
    {
        case ColliderShape::Sphere:  raw_body = m_spheres.storage.chunk(ch).body_idx[lane]; break;
        case ColliderShape::Box:     raw_body = m_boxes.storage.chunk(ch).body_idx[lane];    break;
        case ColliderShape::Capsule: raw_body = m_capsules.storage.chunk(ch).body_idx[lane]; break;
        default: return BodyId::null();
    }
    // We stored BodyId::raw (full 32 bits including generation) in body_idx,
    // so we can return it as-is.
    return BodyId{raw_body};
}

crd::usize ColliderPool::size_of(ColliderShape kind) const noexcept
{
    switch (kind)
    {
        case ColliderShape::Sphere:  return m_spheres.live_count;
        case ColliderShape::Box:     return m_boxes.live_count;
        case ColliderShape::Capsule: return m_capsules.live_count;
        default: return 0;
    }
}

// ---- Per-kind insert primitives ------------------------------------------

crd::u32 ColliderPool::insert_sphere(BodyId body, const Collider& c)
{
    const crd::u32 idx = acquire_slot(m_spheres, m_capacity_per_kind);
    if (idx == 0) return 0;

    const crd::u32 ch   = chunk_of(idx, static_cast<crd::u32>(kLane));
    const crd::u32 lane = lane_of (idx, static_cast<crd::u32>(kLane));
    auto& tile          = m_spheres.storage.chunk(ch);

    const crd::u32 next_gen = next_generation(tile.generation[lane]);

    put_lane(tile.lpos_x, lane, c.local_position.x);
    put_lane(tile.lpos_y, lane, c.local_position.y);
    put_lane(tile.lpos_z, lane, c.local_position.z);
    put_lane(tile.lrot_x, lane, c.local_rotation.x);
    put_lane(tile.lrot_y, lane, c.local_rotation.y);
    put_lane(tile.lrot_z, lane, c.local_rotation.z);
    put_lane(tile.lrot_w, lane, c.local_rotation.w);
    put_lane(tile.radius, lane, c.sphere.radius);
    tile.body_idx[lane] = body.raw;

    tile.generation[lane] = static_cast<crd::u8>(next_gen);
    tile.live[lane]       = 1;
    ++m_spheres.live_count;
    return idx;
}

crd::u32 ColliderPool::insert_box(BodyId body, const Collider& c)
{
    const crd::u32 idx = acquire_slot(m_boxes, m_capacity_per_kind);
    if (idx == 0) return 0;

    const crd::u32 ch   = chunk_of(idx, static_cast<crd::u32>(kLane));
    const crd::u32 lane = lane_of (idx, static_cast<crd::u32>(kLane));
    auto& tile          = m_boxes.storage.chunk(ch);

    const crd::u32 next_gen = next_generation(tile.generation[lane]);

    put_lane(tile.lpos_x, lane, c.local_position.x);
    put_lane(tile.lpos_y, lane, c.local_position.y);
    put_lane(tile.lpos_z, lane, c.local_position.z);
    put_lane(tile.lrot_x, lane, c.local_rotation.x);
    put_lane(tile.lrot_y, lane, c.local_rotation.y);
    put_lane(tile.lrot_z, lane, c.local_rotation.z);
    put_lane(tile.lrot_w, lane, c.local_rotation.w);
    put_lane(tile.half_x, lane, c.box.half_extents.x);
    put_lane(tile.half_y, lane, c.box.half_extents.y);
    put_lane(tile.half_z, lane, c.box.half_extents.z);
    tile.body_idx[lane] = body.raw;

    tile.generation[lane] = static_cast<crd::u8>(next_gen);
    tile.live[lane]       = 1;
    ++m_boxes.live_count;
    return idx;
}

crd::u32 ColliderPool::insert_capsule(BodyId body, const Collider& c)
{
    const crd::u32 idx = acquire_slot(m_capsules, m_capacity_per_kind);
    if (idx == 0) return 0;

    const crd::u32 ch   = chunk_of(idx, static_cast<crd::u32>(kLane));
    const crd::u32 lane = lane_of (idx, static_cast<crd::u32>(kLane));
    auto& tile          = m_capsules.storage.chunk(ch);

    const crd::u32 next_gen = next_generation(tile.generation[lane]);

    put_lane(tile.lpos_x,      lane, c.local_position.x);
    put_lane(tile.lpos_y,      lane, c.local_position.y);
    put_lane(tile.lpos_z,      lane, c.local_position.z);
    put_lane(tile.lrot_x,      lane, c.local_rotation.x);
    put_lane(tile.lrot_y,      lane, c.local_rotation.y);
    put_lane(tile.lrot_z,      lane, c.local_rotation.z);
    put_lane(tile.lrot_w,      lane, c.local_rotation.w);
    put_lane(tile.radius,      lane, c.capsule.radius);
    put_lane(tile.half_height, lane, c.capsule.half_height);
    tile.body_idx[lane] = body.raw;

    tile.generation[lane] = static_cast<crd::u8>(next_gen);
    tile.live[lane]       = 1;
    ++m_capsules.live_count;
    return idx;
}

} // namespace crd::eylem_rigid3d
