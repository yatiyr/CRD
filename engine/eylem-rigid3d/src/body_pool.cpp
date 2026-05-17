// BodyPool impl. Phase 3.1 v1b-a (ADR-0062 §6, §15).

#include <crd/eylem_rigid3d/body_pool.hpp>

#include <crd/core/assert.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cstring>

namespace crd::eylem_rigid3d
{
namespace
{
// Per-column store: read existing 8/4-lane Vec, overwrite the lane, write
// back. Mirrors crd::math::simd::soa.hpp's scatter helper but localised so
// we don't pay the indirection cost in the hot insert/write path.
//
// Vec8f / Vec4f don't expose a per-lane write directly. The store/modify/
// load round-trip is the canonical pattern (also used by gather4/scatter8
// in crd-math::simd::soa).
// Per-lane scalar write into a SIMD column. Templated on column type so
// exactly one specialisation gets emitted per build (Vec8f on AVX2,
// Vec4f on scalar fallback) — no unused-function warnings under
// -Werror,-Wunused-function.
template <typename ColT>
CRD_FORCEINLINE void put_lane(ColT& col, crd::usize lane_idx, crd::f32 value) noexcept
{
    constexpr crd::usize kLanes = sizeof(ColT) / sizeof(crd::f32);
    crd::f32 buf[kLanes];
    col.store(buf);
    buf[lane_idx] = value;
    col = ColT::load(buf);
}
} // namespace

BodyPool::BodyPool(crd::memory::IAllocator* persistent_alloc, crd::u32 max_bodies)
    : m_storage(persistent_alloc != nullptr ? persistent_alloc : crd::memory::default_allocator())
    , m_capacity(std::min(max_bodies, kIndexMax))
    , m_free_next(persistent_alloc != nullptr ? persistent_alloc : crd::memory::default_allocator())
{
    // Reserve room for the side-table up front so insert() never reallocates
    // mid-frame (would invalidate any indices the broadphase has cached).
    m_free_next.resize(m_capacity);

    // Slot 0 is the reserved null sentinel — make sure the underlying SoA
    // has at least one chunk so chunk(0) lookups for slot 0 are valid.
    if (m_capacity > 0)
    {
        m_storage.resize(1);
        // Mark slot 0 explicitly NOT live so contains() on a default
        // BodyId{} (raw=0) returns false.
        m_storage.chunk(0).live[0]       = 0;
        m_storage.chunk(0).generation[0] = 0;
    }
}

BodyId BodyPool::insert(const RigidBody& body)
{
    crd::u32 idx = 0;

    if (m_free_head != 0)
    {
        // Reuse a free slot. Preserves "lowest-index-first" reuse for
        // deterministic slot allocation under the same call sequence.
        idx          = m_free_head;
        m_free_head  = m_free_next[idx];
    }
    else if (m_high_water < m_capacity)
    {
        idx = m_high_water++;
        ensure_slot(idx);
    }
    else
    {
        // Capacity exhausted. Caller checks for null.
        return BodyId::null();
    }

    BodyChunk& tile = m_storage.chunk(chunk_of(idx));
    const crd::u32 lane = lane_of(idx);

    // Bump generation FIRST, then write payload + flip live=1. A reader
    // observing live==0 sees the old generation; a reader observing
    // live==1 sees the new generation + new payload (single-threaded
    // World contract per ADR-0050; no atomics needed).
    crd::u32 next_gen = static_cast<crd::u32>(tile.generation[lane]) + 1U;
    if (next_gen > 0xFFU) next_gen = 1U; // wrap to 1; 0 stays reserved as "never allocated"
    tile.generation[lane] = static_cast<crd::u8>(next_gen);

    store_lane(idx, body);

    tile.live[lane] = 1;
    ++m_live_count;

    return BodyId::make(idx, next_gen);
}

void BodyPool::remove(BodyId id) noexcept
{
    if (!contains(id)) return;

    const crd::u32 idx  = id.index();
    BodyChunk&     tile = m_storage.chunk(chunk_of(idx));
    const crd::u32 lane = lane_of(idx);

    tile.live[lane]    = 0;
    // Generation bumped on next insert — the slot's stored generation
    // remains the LAST-issued one, so contains() on the just-removed
    // BodyId returns false (live==0 short-circuits before generation
    // compare). This avoids burning a generation on every remove.

    // Push to free list (LIFO; slot reuse is bounded by inserted slots,
    // not by remove order). Lower-numbered slots tend to come first
    // since we high-water-mark-grow upward, which is a desirable cache
    // property.
    m_free_next[idx] = m_free_head;
    m_free_head      = idx;

    --m_live_count;
}

bool BodyPool::contains(BodyId id) const noexcept
{
    if (id.is_null()) return false;
    const crd::u32 idx = id.index();
    if (idx == 0 || idx >= m_high_water) return false;
    const BodyChunk& tile = m_storage.chunk(chunk_of(idx));
    const crd::u32   lane = lane_of(idx);
    if (tile.live[lane] == 0) return false;
    return static_cast<crd::u32>(tile.generation[lane]) == id.generation();
}

RigidBody BodyPool::read(BodyId id) const noexcept
{
    RigidBody out{}; // default = effectively-static (inv_mass=0)
    if (!contains(id)) return out;
    load_lane(id.index(), out);
    return out;
}

void BodyPool::write(BodyId id, const RigidBody& state) noexcept
{
    if (!contains(id)) return;
    store_lane(id.index(), state);
}

void BodyPool::write_curr_only(BodyId id, const RigidBody& state) noexcept
{
    if (!contains(id)) return;
    store_curr_only_lane(id.index(), state);
}

BodyPool::PrevState BodyPool::read_prev(BodyId id) const noexcept
{
    PrevState out{};
    out.rotation = crd::math::Quatf{0.0F, 0.0F, 0.0F, 1.0F};
    if (!contains(id)) return out;

    const BodyChunk& tile = m_storage.chunk(chunk_of(id.index()));
    const crd::u32   lane = lane_of(id.index());

    out.position.x = crd::units::Length32{tile.prev_pos_x.lane(lane)};
    out.position.y = crd::units::Length32{tile.prev_pos_y.lane(lane)};
    out.position.z = crd::units::Length32{tile.prev_pos_z.lane(lane)};

    out.rotation.x = tile.prev_rot_x.lane(lane);
    out.rotation.y = tile.prev_rot_y.lane(lane);
    out.rotation.z = tile.prev_rot_z.lane(lane);
    out.rotation.w = tile.prev_rot_w.lane(lane);

    return out;
}

void BodyPool::snapshot_state_to_prev() noexcept
{
    // Whole-column copy across every chunk. The Soa storage hands out
    // chunks that are contiguous and ABI-aligned; SIMD `=` is one mov
    // per column on AVX2 (Vec8f) / SSE (Vec4f). Free-slot lanes get
    // copied too, but that is harmless: live[lane]==0 guards every
    // consumer that reads prev_*.
    const crd::usize n = m_storage.chunk_count();
    for (crd::usize c = 0; c < n; ++c)
    {
        BodyChunk& tile = m_storage.chunk(c);
        tile.prev_pos_x = tile.pos_x;
        tile.prev_pos_y = tile.pos_y;
        tile.prev_pos_z = tile.pos_z;
        tile.prev_rot_x = tile.rot_x;
        tile.prev_rot_y = tile.rot_y;
        tile.prev_rot_z = tile.rot_z;
        tile.prev_rot_w = tile.rot_w;
    }
}

BodyPool::Slot BodyPool::resolve(BodyId id) const noexcept
{
    if (!contains(id)) return Slot{0U, 0U};
    return Slot{chunk_of(id.index()), lane_of(id.index())};
}

void BodyPool::ensure_slot(crd::u32 idx)
{
    const crd::u32 needed_chunks = (idx / static_cast<crd::u32>(kLane)) + 1U;
    if (m_storage.chunk_count() < needed_chunks)
    {
        m_storage.resize(static_cast<crd::usize>(needed_chunks) * kLane);
    }
}

void BodyPool::store_curr_only_lane(crd::u32 idx, const RigidBody& body) noexcept
{
    BodyChunk&     tile = m_storage.chunk(chunk_of(idx));
    const crd::u32 lane = lane_of(idx);

    put_lane(tile.pos_x, lane, body.position.x.value);
    put_lane(tile.pos_y, lane, body.position.y.value);
    put_lane(tile.pos_z, lane, body.position.z.value);

    put_lane(tile.rot_x, lane, body.rotation.x);
    put_lane(tile.rot_y, lane, body.rotation.y);
    put_lane(tile.rot_z, lane, body.rotation.z);
    put_lane(tile.rot_w, lane, body.rotation.w);

    put_lane(tile.linvel_x, lane, body.linear_velocity.x.value);
    put_lane(tile.linvel_y, lane, body.linear_velocity.y.value);
    put_lane(tile.linvel_z, lane, body.linear_velocity.z.value);

    put_lane(tile.angvel_x, lane, body.angular_velocity.x.value);
    put_lane(tile.angvel_y, lane, body.angular_velocity.y.value);
    put_lane(tile.angvel_z, lane, body.angular_velocity.z.value);

    put_lane(tile.inv_mass,        lane, body.inv_mass.value);
    put_lane(tile.inv_inertia_x,   lane, body.inv_inertia.x.value);
    put_lane(tile.inv_inertia_y,   lane, body.inv_inertia.y.value);
    put_lane(tile.inv_inertia_z,   lane, body.inv_inertia.z.value);

    put_lane(tile.linear_damping,  lane, body.linear_damping);
    put_lane(tile.angular_damping, lane, body.angular_damping);

    // Bit-copy the flags struct to avoid bitfield-direct dependence (the
    // compiler may emit different load patterns across platforms).
    crd::u32 flags_raw = 0;
    std::memcpy(&flags_raw, &body.flags, sizeof(flags_raw));
    tile.flags[lane] = flags_raw;
}

void BodyPool::store_lane(crd::u32 idx, const RigidBody& body) noexcept
{
    // Curr columns + flags first.
    store_curr_only_lane(idx, body);

    // Then mirror pos/rot into prev columns. This is what gives `write()`
    // and `insert()` teleport semantics: the renderer's next interpolation
    // alpha lerp(prev, curr) returns the same point regardless of alpha,
    // so spawning / teleporting a body produces no visual jump.
    BodyChunk&     tile = m_storage.chunk(chunk_of(idx));
    const crd::u32 lane = lane_of(idx);

    put_lane(tile.prev_pos_x, lane, body.position.x.value);
    put_lane(tile.prev_pos_y, lane, body.position.y.value);
    put_lane(tile.prev_pos_z, lane, body.position.z.value);

    put_lane(tile.prev_rot_x, lane, body.rotation.x);
    put_lane(tile.prev_rot_y, lane, body.rotation.y);
    put_lane(tile.prev_rot_z, lane, body.rotation.z);
    put_lane(tile.prev_rot_w, lane, body.rotation.w);
}

void BodyPool::load_lane(crd::u32 idx, RigidBody& body) const noexcept
{
    const BodyChunk& tile = m_storage.chunk(chunk_of(idx));
    const crd::u32   lane = lane_of(idx);

    body.position.x = crd::units::Length32{tile.pos_x.lane(lane)};
    body.position.y = crd::units::Length32{tile.pos_y.lane(lane)};
    body.position.z = crd::units::Length32{tile.pos_z.lane(lane)};

    body.rotation.x = tile.rot_x.lane(lane);
    body.rotation.y = tile.rot_y.lane(lane);
    body.rotation.z = tile.rot_z.lane(lane);
    body.rotation.w = tile.rot_w.lane(lane);

    body.linear_velocity.x = crd::units::Velocity32{tile.linvel_x.lane(lane)};
    body.linear_velocity.y = crd::units::Velocity32{tile.linvel_y.lane(lane)};
    body.linear_velocity.z = crd::units::Velocity32{tile.linvel_z.lane(lane)};

    body.angular_velocity.x = crd::units::AngularVelocity32{tile.angvel_x.lane(lane)};
    body.angular_velocity.y = crd::units::AngularVelocity32{tile.angvel_y.lane(lane)};
    body.angular_velocity.z = crd::units::AngularVelocity32{tile.angvel_z.lane(lane)};

    body.inv_mass      = crd::units::InverseMass32{tile.inv_mass.lane(lane)};
    body.inv_inertia.x = crd::units::InverseMomentOfInertia32{tile.inv_inertia_x.lane(lane)};
    body.inv_inertia.y = crd::units::InverseMomentOfInertia32{tile.inv_inertia_y.lane(lane)};
    body.inv_inertia.z = crd::units::InverseMomentOfInertia32{tile.inv_inertia_z.lane(lane)};

    body.linear_damping  = tile.linear_damping.lane(lane);
    body.angular_damping = tile.angular_damping.lane(lane);

    const crd::u32 flags_raw = tile.flags[lane];
    std::memcpy(&body.flags, &flags_raw, sizeof(flags_raw));
}

} // namespace crd::eylem_rigid3d
