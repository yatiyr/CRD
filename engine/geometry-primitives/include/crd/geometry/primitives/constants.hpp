#pragma once

// ---------------------------------------------------------------------------
// crd-geometry — the geometry-wide epsilon / tolerance policy (Phase 3.1.7 v1h,
// ADR-0076 §15).
//
// One place that names every tolerance the geometry substrate uses, *by intent*
// rather than by magnitude — so a call site reads `k_parallel_epsilon<T>()`
// (this dot product is "near zero" in the are-these-directions-parallel sense),
// not a bare `1e-6F` whose meaning has to be reconstructed. The numeric values
// are deliberately the same magic numbers the substrate already used; v1h is a
// rename, not a retune. (`crd-math`'s `default_epsilon<T>()` stays the generic
// "is this scalar approximately zero" floor and is what the existing primitive
// helpers keep as their default argument — migrating those onto these named
// constants is a later ergonomic pass, not part of v1h.)
//
// Convention: templated `constexpr T k_<name>() noexcept` (the type-dependent
// constexpr form already established by `bvh_build_internal.hpp`'s
// `k_sah_cost_epsilon` etc.). `f32` returns `1e-6F`-class values; `f64` tightens
// by ~1e6 (matching `default_epsilon`'s 1e-6 → 1e-12 step).
// ---------------------------------------------------------------------------

#include <crd/math/scalar.hpp>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;

// Generic distance / position tolerance — "two points are the same place",
// "this segment has length ~0". The geometry analog of `default_epsilon`.
template <MathScalar T> [[nodiscard]] constexpr T k_distance_epsilon() noexcept
{
    return static_cast<T>(1e-6);
}
template <> [[nodiscard]] constexpr double k_distance_epsilon<double>() noexcept
{
    return 1e-12;
}

// Area tolerance — "this triangle is degenerate (collinear vertices)",
// "this face has ~0 area". Squared-distance-ish scale, hence one ulp-decade
// looser than the distance epsilon would suggest at the small end.
template <MathScalar T> [[nodiscard]] constexpr T k_area_epsilon() noexcept
{
    return static_cast<T>(1e-6);
}
template <> [[nodiscard]] constexpr double k_area_epsilon<double>() noexcept
{
    return 1e-12;
}

// Parallelism tolerance — a dot / cross product is "near zero" in the sense
// "these two directions are (anti)parallel", "this ray grazes this plane".
// Applied to *normalised* directions, so it lives on the [-1, 1] scale.
template <MathScalar T> [[nodiscard]] constexpr T k_parallel_epsilon() noexcept
{
    return static_cast<T>(1e-6);
}
template <> [[nodiscard]] constexpr double k_parallel_epsilon<double>() noexcept
{
    return 1e-12;
}

// Zero-extent tolerance — "this AABB / OBB has ~0 width along some axis",
// "this box is really a slab / line / point". Distinct from `k_distance_epsilon`
// only by name (same magnitude) — kept separate so a future retune of one does
// not silently move the other.
template <MathScalar T> [[nodiscard]] constexpr T k_degenerate_extent_epsilon() noexcept
{
    return static_cast<T>(1e-6);
}
template <> [[nodiscard]] constexpr double k_degenerate_extent_epsilon<double>() noexcept
{
    return 1e-12;
}

// SAH-cost tie tolerance — two BVH split-plane costs within this are "equal" and
// the deterministic tiebreak (lower bin index, X→Y→Z) decides. Single source of
// truth for `crd-geometry-bvh`'s builder (`bvh_build_internal.hpp` re-exports
// this so there is exactly one value).
template <MathScalar T> [[nodiscard]] constexpr T k_sah_cost_epsilon() noexcept
{
    return static_cast<T>(1e-6);
}
template <> [[nodiscard]] constexpr double k_sah_cost_epsilon<double>() noexcept
{
    return 1e-12;
}

// Default "fat margin" for a `DynamicBvh` leaf's enlarged AABB — how far a
// proxy may move before its node needs reinsertion (matches
// `DynamicBvhConfig::fat_margin`'s default of 0.1). This one is a *world-unit*
// length, not an epsilon — it scales with the scene, not with float precision.
template <MathScalar T> [[nodiscard]] constexpr T k_default_fat_margin() noexcept
{
    return static_cast<T>(0.1);
}

// Conservative ULP padding count for the robust ray-AABB slab test (Ize 2013):
// the `tmax` comparison is widened by `1 + 2·γ₃` with `γ₃ = 3u/(1−3u)`,
// `u = ½ ulp` — i.e. 3 ulps of slack, doubled. `robust_ray_aabb.hpp`'s
// `ray_aabb_robust_pad<T>()` computes the multiplier from this count.
template <MathScalar T> [[nodiscard]] constexpr unsigned k_robust_aabb_pad_ulps() noexcept
{
    return 3U;
}

} // namespace crd::geometry::primitives
