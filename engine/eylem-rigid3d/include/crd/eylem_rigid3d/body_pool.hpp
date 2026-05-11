#pragma once

// BodyPool — AoSoA-8 (or 4 on non-AVX2) rigid-body storage for the eylem
// rigid-3D scene. Phase 3.1 v1b-a (ADR-0062 §6, §15).
//
// Architecture decisions locked at v1b-a:
//
//   1. AoSoA-8 layout. Per ADR-0062 the broadphase + solver iterate dense
//      packed columns; AoS would force per-lane gather/scatter on every
//      column read. AoSoA pays the gather cost only on cross-chunk moves
//      (gather8 / scatter8 helpers from crd-math::simd::soa). The ports
//      from Bullet (btSoftBody-style SoA) and PhysX (PxgPostSolveContact
//      lane packing) both validate this choice; the v0b Soa<TChunk, Lane>
//      substrate is ready to consume.
//
//   2. Free-list reclaim with generation bump. BodyId is `[gen:8|idx:24]`;
//      slot index is stable for the slot's lifetime, generation is bumped
//      on remove so stale handles fail `contains()`. No tile compaction:
//      the broadphase's persistent contact cache + solver's island
//      bookkeeping both want stable indices across frames. Compaction is
//      reserved for v1l (defrag pass) IF a real workload needs it.
//
//   3. Capacity is FIXED at construction. `PhysicsConfig::max_bodies` (24-
//      bit BodyId index) caps it. A pool that exhausts capacity returns
//      `BodyId::null()` from `insert()` -- callers must check, no silent
//      growth (would invalidate solver pointers mid-step).
//
//   4. AoS RigidBody POD is the public read/write contract. `read()` packs
//      lanes back into `RigidBody`, `write()` fans the AoS into columns.
//      The hot solver loop (v1e+) bypasses this and iterates chunks
//      directly via `storage()`.
//
// Determinism: all per-lane writes go through scalar `store()`/`load()`
// per-column; insertion order is preserved within a tile; remove +
// re-insert reuses the lowest-index free slot so the slot-allocation
// sequence is deterministic given a deterministic call sequence.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/soa.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::eylem_rigid3d
{
// Surface the eylem-public POD types into eylem_rigid3d so headers stay
// readable. These are NOT `using namespace crd::eylem;` (banned in
// headers per CLAUDE.md) -- single typedefs are an explicit, scoped opt-in.
using crd::eylem::BodyId;
using crd::eylem::RigidBody;

// Pick the column type by lane width. Win + Linux production builds use
// AVX2 (Lane=8 → Vec8f columns); win-debug-scalar uses scalar fallback
// (Lane=4 → Vec4f columns). The struct shape is identical either way.
template <crd::usize Lane>
struct BodyChunkT;

template <>
struct alignas(32) BodyChunkT<8>
{
    using Col = crd::math::simd::Vec8f;
    // ── State columns (3 + 4 + 3 + 3 + 1 + 3 + 1 + 1 = 19 columns × 32 B = 608 B) ──
    Col pos_x{};         Col pos_y{};         Col pos_z{};
    Col rot_x{};         Col rot_y{};         Col rot_z{};         Col rot_w{};
    Col linvel_x{};      Col linvel_y{};      Col linvel_z{};
    Col angvel_x{};      Col angvel_y{};      Col angvel_z{};
    Col inv_mass{};
    Col inv_inertia_x{}; Col inv_inertia_y{}; Col inv_inertia_z{};
    Col linear_damping{};
    Col angular_damping{};
    // ── Per-lane integer side-bands (kept in same chunk for cache locality) ──
    crd::u32 flags[8]{};
    // generation = the BodyId generation last issued for this slot.
    // 0 = slot has never been allocated. Slot is "live" iff `live[i]` is true.
    // The two arrays could be packed together; keeping them parallel makes
    // the validity check branch-free and easy to vectorise later.
    crd::u8  generation[8]{};
    crd::u8  live[8]{}; // 0 = free slot, 1 = currently allocated
};

template <>
struct alignas(16) BodyChunkT<4>
{
    using Col = crd::math::simd::Vec4f;
    Col pos_x{};         Col pos_y{};         Col pos_z{};
    Col rot_x{};         Col rot_y{};         Col rot_z{};         Col rot_w{};
    Col linvel_x{};      Col linvel_y{};      Col linvel_z{};
    Col angvel_x{};      Col angvel_y{};      Col angvel_z{};
    Col inv_mass{};
    Col inv_inertia_x{}; Col inv_inertia_y{}; Col inv_inertia_z{};
    Col linear_damping{};
    Col angular_damping{};
    crd::u32 flags[4]{};
    crd::u8  generation[4]{};
    crd::u8  live[4]{};
};

using BodyChunk = BodyChunkT<crd::math::simd::k_native_lane_width>;

class BodyPool
{
public:
    static constexpr crd::usize kLane = crd::math::simd::k_native_lane_width;
    using Storage                     = crd::math::simd::Soa<BodyChunk, kLane>;

    // BodyId index field is 24 bits, so the absolute hard ceiling is
    // (1 << 24) - 1 = 16M - 1. Slot 0 is reserved as the null sentinel,
    // matching `BodyId::null()`.
    static constexpr crd::u32 kIndexMax = (1U << 24) - 1U;

    // `persistent_alloc` may be nullptr → falls back to default_allocator().
    // `max_bodies` clamps to `kIndexMax`. Slot 0 is consumed as the null
    // sentinel at construction; effective capacity = max_bodies - 1.
    BodyPool(crd::memory::IAllocator* persistent_alloc, crd::u32 max_bodies);

    [[nodiscard]] BodyId    insert(const RigidBody& body);
    void                    remove(BodyId id) noexcept;
    [[nodiscard]] bool      contains(BodyId id) const noexcept;
    [[nodiscard]] RigidBody read(BodyId id) const noexcept;
    void                    write(BodyId id, const RigidBody& state) noexcept;

    // Number of currently-live bodies (excluding the null sentinel).
    [[nodiscard]] crd::usize size() const noexcept { return m_live_count; }

    // Maximum bodies this pool can ever hold (capacity hint at ctor, clamped).
    [[nodiscard]] crd::u32 capacity() const noexcept { return m_capacity; }

    // Direct chunk-storage access for the v1c+ broadphase / v1e+ solver
    // hot path. Caller is expected to use `live[lane]` to skip free
    // slots when iterating partial tiles.
    [[nodiscard]] Storage&       storage()       noexcept { return m_storage; }
    [[nodiscard]] const Storage& storage() const noexcept { return m_storage; }

    // Lane-level resolver -- v1c+ broadphase uses this to translate a
    // BodyId to (chunk_idx, lane_idx) for SIMD lookups.
    struct Slot
    {
        crd::u32 chunk_idx;
        crd::u32 lane_idx;
    };
    [[nodiscard]] Slot resolve(BodyId id) const noexcept;

private:
    // Layout helpers.
    [[nodiscard]] static constexpr crd::u32 chunk_of(crd::u32 idx) noexcept { return idx / static_cast<crd::u32>(kLane); }
    [[nodiscard]] static constexpr crd::u32 lane_of (crd::u32 idx) noexcept { return idx % static_cast<crd::u32>(kLane); }

    // Bring slot `idx` into existence (grows storage if needed). idx
    // must be < m_capacity.
    void ensure_slot(crd::u32 idx);

    // Per-lane pack/unpack between AoS RigidBody and AoSoA columns.
    void store_lane (crd::u32 idx, const RigidBody& body) noexcept;
    void load_lane  (crd::u32 idx, RigidBody& body) const noexcept;

    Storage  m_storage;
    crd::u32 m_capacity     = 0;
    crd::u32 m_high_water   = 1; // slot 0 is reserved; first user slot starts at 1
    crd::u32 m_live_count   = 0;
    crd::u32 m_free_head    = 0; // 0 = empty list (slot 0 is reserved)

    // Free-list nodes live in-place: when a slot is free, its `live` is 0
    // and the next-free index is stored in a parallel side-table to keep
    // BodyChunk's cache layout clean.
    crd::containers::Array<crd::u32> m_free_next;
};

} // namespace crd::eylem_rigid3d
