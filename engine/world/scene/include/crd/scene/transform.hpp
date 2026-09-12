#pragma once

#include <crd/core/types.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::scene
{
// Transform — Phase 3.0 v1j (ADR-0054). The scene-layer rigid-body +
// scale transform component, paired with TransformPropagation
// (PreRender phase) to maintain a per-entity world matrix that
// composes through the ChildOf hierarchy.
//
// Layout: TRS (translation, quaternion rotation, per-axis scale) plus
// a cached `world` matrix. Total = 4 (Vec3) + 16 (Quat) + 12 (Vec3) +
// 64 (Mat4) = 96 bytes. Trivially-relocatable; archetype storage hint
// is the default. v1c2's per-chunk version counter + v1i's
// ChangeDetectIndex track Transform writes for free.
//
// ---- Cross-domain robustness pin ---------------------------------------
//
// Precision: f32 with 23-bit mantissa (~7 decimal digits). Workspaces
// up to ~1 km at sub-mm precision are exact. Wider scales (orbital
// mechanics, astronomy) need a custom f64 component:
//   1. Define `struct TransformF64 { math::Transformd local; ... };`
//      following the same shape.
//   2. Register it via `world.register_component<TransformF64>(...)`.
//   3. Implement `class TransformPropagationF64 : public ISystem` that
//      walks ChildOf for f64-tagged entities. v1n verifies the
//      registration grammar accepts custom types.
//
// Quaternion drift: f32 multiplication accumulates ~1 ULP per op. Deep
// chains (>30 ChildOf depth) MAY exhibit visible drift over seconds at
// 60 Hz. Mitigations:
//   - Call `renormalize_rotation()` periodically on hot-path entities.
//   - Or register `History{60}` and rebuild from a clean snapshot
//     every N frames (Phase 3.2 HistoryIndex consumer).
//   - Or use TransformF64 (above) for precision-sensitive subtrees.
//
// Determinism: TransformPropagation visits entities in a deterministic
// order (ChildOf reverse-index insertion order + DFS pre-order). With
// the same code path + same input order, world matrices are bit-exact
// across runs. Verified by `test_transform.cpp` "determinism replay
// hash" case.
//
// Hierarchy depth limit: kMaxTransformDepth = 256 (CRD_ASSERT in debug
// during dirty-subtree marking). Real-world hierarchies (humanoid robot
// 30-40, deep UI 100-150, particle attachment chains 50-100) fit
// comfortably. Larger trees should split via Owns relations or use a
// flat propagation pattern.
struct Transform
{
    // Position in SI meters, dimensional via crd::units::Length<f32> (Phase
    // 3.1.7.5 v0b-3 / ADR-0078 §2 D4). f32 precision = sub-mm @ 1 km exact;
    // CAD / aerospace / orbital domains should register a separate
    // `TransformF64` component as documented in the header comment above.
    // Bridge to SIMD / GPU / Mat4 paths via `crd::math::to_raw_vec(translation)`.
    crd::math::Vec3<crd::units::Length32> translation{};
    crd::math::Quatf rotation = crd::math::Quatf::identity();
    // Scale is a dimensionless ratio (Vec3<f32>) -- not retyped at v0b.
    crd::math::Vec3f scale{static_cast<crd::f32>(1), static_cast<crd::f32>(1), static_cast<crd::f32>(1)};

    // World matrix cache — written by TransformPropagation in PreRender.
    // Reading `world` BEFORE propagation runs returns the previous frame's
    // value (or identity for newly-spawned entities). Document call sites
    // accordingly: gameplay logic should rely on the local TRS triple;
    // rendering / culling / picking reads `world` after PreRender.
    crd::math::Mat4f world = crd::math::Mat4f::identity();

    // Compute the entity's local TRS matrix on demand. Used by
    // TransformPropagation to compose `parent.world * local`. The
    // dimensional `translation` reaches into the raw scalar Vec3<f32> at
    // the Mat4 boundary (Mat4 itself is dimensionless in conventional
    // engine math).
    [[nodiscard]] crd::math::Mat4f local() const noexcept
    {
        return crd::math::from_trs(crd::math::to_raw_vec(translation), rotation, scale);
    }

    // Renormalize the rotation quaternion to unit length. Useful as a
    // periodic guard against drift in deep chains. NOT called
    // automatically by propagation — drift is the caller's call to
    // manage (or to register HistoryIndex / TransformF64 for precision-
    // sensitive subtrees).
    void renormalize_rotation() noexcept
    {
        (void)crd::math::try_normalize(rotation);
    }
};

// TransformDirtyFlag — Phase 3.0 v1j. Empty marker component tagged onto
// entities whose Transform was written this frame (or whose ancestor
// was written, propagated by mark_subtree_dirty).
//
// Storage hint: SparseSet (frame-scoped, sparse — adds/removes every
// step). Archetype storage would archetype-explode on every mark/unmark
// cycle.
//
// TransformPropagation:
//   1. Query world.query<Transform>().with<TransformDirtyFlag>().
//   2. Find dirty roots (dirty entities whose ChildOf parent is absent
//      or NOT dirty).
//   3. DFS each dirty subtree, recompute world, queue dirty-flag removal
//      via Commands (flushed at the PreRender phase boundary).
struct TransformDirtyFlag
{
    crd::u8 unused = 0; // SparseSet pools require non-zero size; this byte is the cost.
};

// Maximum hierarchy depth checked in debug builds during dirty-subtree
// marking. Push-based marking is O(subtree-size); CRD_ASSERT(depth <
// kMaxTransformDepth) catches accidental million-entity-tree marks in
// dev. Release builds skip the check (caller's responsibility).
inline constexpr crd::u32 kMaxTransformDepth = 256;

} // namespace crd::scene
