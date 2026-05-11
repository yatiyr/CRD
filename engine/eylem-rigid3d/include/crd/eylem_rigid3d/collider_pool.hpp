#pragma once

// ColliderPool — per-kind AoSoA-8 storage for the eylem rigid-3D scene.
// Phase 3.1 v1b-b (ADR-0062 §6, §15).
//
// Architecture decisions:
//
//   1. Three kind-specific pools internally — Sphere / Box / Capsule. Each
//      lays out the kind-specific shape data + the COMMON (body_id,
//      local_pos, local_rot) columns in its own SoA tile. Per-kind iteration
//      from the broadphase (v1c+) reads only the columns it needs and
//      avoids the per-element type dispatch a single union-pool would
//      require. Mirrors the PhysX `PxgShape*` family + Bullet's per-type
//      hash-pool layout.
//
//   2. ColliderId index field encodes routing: [kind:3 | per_kind_idx:21].
//      Top 3 bits = ColliderShape value; bottom 21 bits = the per-kind
//      pool's local index. Lookup is O(1) without any global record table —
//      the ID itself dispatches to the right pool. 21 bits = 2,097,151
//      colliders per kind, ample for any practical scene.
//
//   3. Generation per slot lives IN the kind-specific pool (alongside
//      `live[Lane]`). ColliderId stores the generation in its top 8 bits;
//      contains() does live[lane] && generation[lane] == id.generation()
//      branch-free, same pattern as BodyPool.
//
//   4. ConvexHull + Plane deferred to v1d (when GJK + plane raycast
//      consume them). Pools without consumers would be dead storage —
//      "stub targets are not integration" per project quality bar.
//      `insert(body, collider)` on an unsupported kind returns
//      `ColliderId::null()` and logs an error.
//
// Determinism: per-kind insertion order preserved within a tile, lowest-
// index free slot reused first; the kind itself is a function of the input
// `Collider::shape`, so the full handle sequence is reproducible from the
// public call sequence.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/collider.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/soa.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::eylem_rigid3d
{
using crd::eylem::BodyId;
using crd::eylem::Collider;
using crd::eylem::ColliderId;
using crd::eylem::ColliderShape;

// ---- ColliderId index encoding (impl-internal) ----------------------------
//
// ColliderId.index() = [kind:4 | per_kind_idx:20]. The high 4 bits select
// the shape kind; the low 20 bits are the per-kind pool's local index.
// The kind value is the raw `ColliderShape` enum byte.
//
// 4 bits = 16 shape kinds (5 industry-standard slots used today, 8 with
// the v1d-mesh / v1d-hf / Phase-3.1.5 expansions, 8 reserved for compound
// + soft-body + fluid + future categories without an API break).
// 20 bits = 1,048,575 colliders per kind, ample for any practical scene.
inline constexpr crd::u32 kColliderKindBits   = 4U;
inline constexpr crd::u32 kColliderKindShift  = 20U;
inline constexpr crd::u32 kColliderKindMask   = (1U << kColliderKindBits) - 1U;
inline constexpr crd::u32 kColliderPerKindMax = (1U << kColliderKindShift) - 1U; // 1,048,575

[[nodiscard]] inline constexpr crd::u32 encode_collider_index(ColliderShape kind,
                                                              crd::u32      per_kind_idx) noexcept
{
    return ((static_cast<crd::u32>(kind) & kColliderKindMask) << kColliderKindShift)
         | (per_kind_idx & kColliderPerKindMax);
}

[[nodiscard]] inline constexpr ColliderShape decode_collider_kind(crd::u32 index) noexcept
{
    return static_cast<ColliderShape>((index >> kColliderKindShift) & kColliderKindMask);
}

[[nodiscard]] inline constexpr crd::u32 decode_collider_per_kind_idx(crd::u32 index) noexcept
{
    return index & kColliderPerKindMax;
}

// ---- Per-kind chunk layouts -----------------------------------------------
//
// Each chunk holds Lane colliders. Common columns (body_idx, local_pos,
// local_rot) plus kind-specific shape columns. The integer side-bands
// (body_idx, generation, live) sit alongside the SIMD float columns in
// the same chunk for cache locality.

template <crd::usize Lane> struct SphereChunkT;

template <>
struct alignas(32) SphereChunkT<8>
{
    using Col = crd::math::simd::Vec8f;
    Col lpos_x{};  Col lpos_y{};  Col lpos_z{};
    Col lrot_x{};  Col lrot_y{};  Col lrot_z{};  Col lrot_w{};
    Col radius{};
    crd::u32 body_idx[8]{};
    crd::u8  generation[8]{};
    crd::u8  live[8]{};
};

template <>
struct alignas(16) SphereChunkT<4>
{
    using Col = crd::math::simd::Vec4f;
    Col lpos_x{};  Col lpos_y{};  Col lpos_z{};
    Col lrot_x{};  Col lrot_y{};  Col lrot_z{};  Col lrot_w{};
    Col radius{};
    crd::u32 body_idx[4]{};
    crd::u8  generation[4]{};
    crd::u8  live[4]{};
};

template <crd::usize Lane> struct BoxChunkT;

template <>
struct alignas(32) BoxChunkT<8>
{
    using Col = crd::math::simd::Vec8f;
    Col lpos_x{};  Col lpos_y{};  Col lpos_z{};
    Col lrot_x{};  Col lrot_y{};  Col lrot_z{};  Col lrot_w{};
    Col half_x{};  Col half_y{};  Col half_z{};
    crd::u32 body_idx[8]{};
    crd::u8  generation[8]{};
    crd::u8  live[8]{};
};

template <>
struct alignas(16) BoxChunkT<4>
{
    using Col = crd::math::simd::Vec4f;
    Col lpos_x{};  Col lpos_y{};  Col lpos_z{};
    Col lrot_x{};  Col lrot_y{};  Col lrot_z{};  Col lrot_w{};
    Col half_x{};  Col half_y{};  Col half_z{};
    crd::u32 body_idx[4]{};
    crd::u8  generation[4]{};
    crd::u8  live[4]{};
};

template <crd::usize Lane> struct CapsuleChunkT;

template <>
struct alignas(32) CapsuleChunkT<8>
{
    using Col = crd::math::simd::Vec8f;
    Col lpos_x{};  Col lpos_y{};  Col lpos_z{};
    Col lrot_x{};  Col lrot_y{};  Col lrot_z{};  Col lrot_w{};
    Col radius{};  Col half_height{};
    crd::u32 body_idx[8]{};
    crd::u8  generation[8]{};
    crd::u8  live[8]{};
};

template <>
struct alignas(16) CapsuleChunkT<4>
{
    using Col = crd::math::simd::Vec4f;
    Col lpos_x{};  Col lpos_y{};  Col lpos_z{};
    Col lrot_x{};  Col lrot_y{};  Col lrot_z{};  Col lrot_w{};
    Col radius{};  Col half_height{};
    crd::u32 body_idx[4]{};
    crd::u8  generation[4]{};
    crd::u8  live[4]{};
};

using SphereChunk  = SphereChunkT <crd::math::simd::k_native_lane_width>;
using BoxChunk     = BoxChunkT    <crd::math::simd::k_native_lane_width>;
using CapsuleChunk = CapsuleChunkT<crd::math::simd::k_native_lane_width>;

class ColliderPool
{
public:
    static constexpr crd::usize kLane = crd::math::simd::k_native_lane_width;
    using SphereStorage  = crd::math::simd::Soa<SphereChunk,  kLane>;
    using BoxStorage     = crd::math::simd::Soa<BoxChunk,     kLane>;
    using CapsuleStorage = crd::math::simd::Soa<CapsuleChunk, kLane>;

    // `persistent_alloc` may be nullptr → fall back to default_allocator().
    // `max_per_kind` clamps to `kColliderPerKindMax`. Slot 0 in each
    // per-kind pool is reserved as the null sentinel.
    ColliderPool(crd::memory::IAllocator* persistent_alloc, crd::u32 max_per_kind);

    // Returns ColliderId::null() if:
    //   - `body` is null
    //   - `collider.shape` is not Sphere/Box/Capsule (ConvexHull/Plane deferred)
    //   - the kind-specific pool is at capacity
    [[nodiscard]] ColliderId insert(BodyId body, const Collider& collider);

    void                 remove(ColliderId id) noexcept;
    [[nodiscard]] bool   contains(ColliderId id) const noexcept;
    [[nodiscard]] Collider read(ColliderId id) const noexcept;
    // BodyId of the body this collider is attached to. null() if id is invalid.
    [[nodiscard]] BodyId   body_of(ColliderId id) const noexcept;

    // Counts.
    [[nodiscard]] crd::usize size() const noexcept
    {
        return m_spheres.live_count + m_boxes.live_count + m_capsules.live_count;
    }
    [[nodiscard]] crd::usize size_of(ColliderShape kind) const noexcept;
    [[nodiscard]] crd::u32   capacity_per_kind() const noexcept { return m_capacity_per_kind; }

    // Direct kind-specific storage — v1c+ broadphase iterates these.
    [[nodiscard]] SphereStorage&        sphere_storage()        noexcept { return m_spheres.storage; }
    [[nodiscard]] BoxStorage&           box_storage()           noexcept { return m_boxes.storage; }
    [[nodiscard]] CapsuleStorage&       capsule_storage()       noexcept { return m_capsules.storage; }
    [[nodiscard]] const SphereStorage&  sphere_storage()  const noexcept { return m_spheres.storage; }
    [[nodiscard]] const BoxStorage&     box_storage()     const noexcept { return m_boxes.storage; }
    [[nodiscard]] const CapsuleStorage& capsule_storage() const noexcept { return m_capsules.storage; }

private:
    template <typename TStorage>
    struct PerKind
    {
        TStorage                         storage;
        crd::containers::Array<crd::u32> free_next;
        crd::u32                         high_water = 1; // slot 0 reserved
        crd::u32                         free_head  = 0; // 0 == empty list
        crd::usize                       live_count = 0;

        explicit PerKind(crd::memory::IAllocator* alloc) noexcept
            : storage(alloc), free_next(alloc) {}
    };

    PerKind<SphereStorage>  m_spheres;
    PerKind<BoxStorage>     m_boxes;
    PerKind<CapsuleStorage> m_capsules;
    crd::u32                m_capacity_per_kind = 0;

    // Per-kind insert / remove primitives (defined in .cpp).
    [[nodiscard]] crd::u32 insert_sphere (BodyId body, const Collider& c);
    [[nodiscard]] crd::u32 insert_box    (BodyId body, const Collider& c);
    [[nodiscard]] crd::u32 insert_capsule(BodyId body, const Collider& c);
};

} // namespace crd::eylem_rigid3d
