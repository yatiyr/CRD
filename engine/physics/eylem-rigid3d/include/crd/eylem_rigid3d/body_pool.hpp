#pragma once

// BodyPool — AoSoA-8 rigid-body storage for the eylem
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
//      The integrator (rigid3d_kernels.hpp) and the v1e+ solver bypass this
//      and iterate chunks directly via `storage()`.
//
// Determinism: per-lane reads and writes index the plain columns; insertion order is preserved within a tile; remove +
// re-insert reuses the lowest-index free slot so the slot-allocation
// sequence is deterministic given a deterministic call sequence.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem/types.hpp>
#include <crd/eylem_rigid3d/rigid3d_kernels.hpp>
#include <crd/math/simd/soa.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::eylem_rigid3d
{
// Surface the eylem-public POD types into eylem_rigid3d so headers stay
// readable. These are NOT `using namespace crd::eylem;` (banned in
// headers per CLAUDE.md) -- single typedefs are an explicit, scoped opt-in.
using crd::eylem::BodyId;
using crd::eylem::RigidBody;

// BodyChunk (rigid3d_kernels.hpp): kChunkLanes bodies as plain f32 columns, the layout every CPU level's kernel
// reads.

class BodyPool
{
public:
    static constexpr crd::usize kLane = kChunkLanes;
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
    // TELEPORT semantics: also resets prev = curr so the renderer does
    // not visually lerp from the old pose to the new one. Use this for
    // user-level "set state" calls (spawning, level resets, network
    // snap corrections).
    void                    write(BodyId id, const RigidBody& state) noexcept;
    // INTEGRATOR semantics: writes only the curr columns and leaves
    // prev untouched. Use this from a physics step where the prev
    // snapshot was already captured earlier in the substep (the body
    // step kernel does) — overwriting prev here would destroy the
    // interpolation source.
    void                    write_curr_only(BodyId id, const RigidBody& state) noexcept;

    // Read just the current pos+rot (`read_pose`) or the previous-frame
    // snapshot pos+rot (`read_prev`) for `id`, one chunk lookup each.
    // EylemSystem syncs Transform from the first; RigidBodyInterpolationSystem
    // lerps between the two at alpha = world.fixed_step_alpha(fixed_dt).
    // Return (pos=zero, rot=identity) if `id` is invalid — callers normally
    // guarantee validity via component-storage iteration.
    struct Pose
    {
        crd::math::Vec3<crd::units::Length32> position;
        crd::math::Quatf                       rotation;
    };
    [[nodiscard]] Pose read_pose(BodyId id) const noexcept;
    [[nodiscard]] Pose read_prev(BodyId id) const noexcept;

    // Number of currently-live bodies (excluding the null sentinel).
    [[nodiscard]] crd::usize size() const noexcept { return m_live_count; }

    // Maximum bodies this pool can ever hold (capacity hint at ctor, clamped).
    [[nodiscard]] crd::u32 capacity() const noexcept { return m_capacity; }

    // Direct chunk-storage access for the body step kernel and the v1c+
    // broadphase / v1e+ solver hot path. Caller is expected to use `live[lane]` to skip free
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
    // `store_lane` writes curr cols AND mirrors pos/rot into prev cols
    // (teleport semantics). `store_curr_only_lane` writes curr cols and
    // leaves prev untouched (integrator semantics — prev was snapshotted
    // earlier in the substep).
    void store_lane           (crd::u32 idx, const RigidBody& body) noexcept;
    void store_curr_only_lane (crd::u32 idx, const RigidBody& body) noexcept;
    void load_lane            (crd::u32 idx, RigidBody& body) const noexcept;

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
