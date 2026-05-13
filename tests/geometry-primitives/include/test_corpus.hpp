#pragma once

// ---------------------------------------------------------------------------
// crd-geometry test corpus — shared degenerate-input + large-coordinate
// helpers (Phase 3.1.7 v1i-c, ADR-0076 §15 validation discipline pin).
//
// Header-only, no link dependency — included from both
// `crd-geometry-primitives-tests` and `crd-geometry-bvh-tests`.
//
// The NaN/Inf contract (ADR-0076 §16 pin #3) says queries tolerate garbage:
// a `raycast` / `overlap` / `closest_point` against a NaN/∞ primitive must
// silently produce "no hit", not UB. The corpora here exercise that contract
// systematically. The large-coordinate sweep documents the f32-precision
// envelope — a scene shifted to a +1e6 / +1e7 origin should still answer
// queries to within an f32 tolerance.
//
// Builder behaviour under garbage is NOT exercised here — ADR-0076 §15 says
// builders *reject in debug* (`CRD_ASSERT(all_finite(prims))`) and produce
// defined-but-degenerate output in release. Those asserts are pinned in
// `engine/geometry-bvh/src/bvh_build.cpp` etc., tested elsewhere.
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace crd::geometry::test_corpus
{
using crd::f32;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::Sphere;
using crd::geometry::primitives::Triangle3;
using crd::math::Vec3;

inline constexpr f32 k_nan = std::numeric_limits<f32>::quiet_NaN();
inline constexpr f32 k_inf = std::numeric_limits<f32>::infinity();

// ---- Degenerate AABBs -----------------------------------------------------
//
// Each entry is something a *query* must tolerate (silently never-hit; never
// UB). Each leans on a specific failure mode in the slab / intersect-AABB
// kernels — point box (min == max), inverted box (min > max — the empty
// sentinel that `aabb_empty()` produces), NaN-component, ±∞-component.

[[nodiscard]] inline std::vector<AABB3<f32>> degenerate_aabbs()
{
    return {
        // Point box (min == max) — zero-volume but otherwise well-formed.
        AABB3<f32>(Vec3<f32>(1, 1, 1), Vec3<f32>(1, 1, 1)),
        // Empty / inverted (min > max) — `aabb_empty()` sentinel.
        AABB3<f32>(Vec3<f32>(+k_inf, +k_inf, +k_inf), Vec3<f32>(-k_inf, -k_inf, -k_inf)),
        // NaN min — never-hit semantics.
        AABB3<f32>(Vec3<f32>(k_nan, 0, 0), Vec3<f32>(1, 1, 1)),
        // NaN max.
        AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, k_nan)),
        // Half-infinite — slab needs to handle ∞ in one axis without poisoning.
        AABB3<f32>(Vec3<f32>(-k_inf, 0, 0), Vec3<f32>(1, 1, 1)),
        AABB3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(+k_inf, 1, 1)),
    };
}

// ---- Degenerate triangles -------------------------------------------------

[[nodiscard]] inline std::vector<Triangle3<f32>> degenerate_triangles()
{
    return {
        // Coincident vertices (zero area, zero "edge1").
        Triangle3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(0, 0, 0), Vec3<f32>(0, 0, 0)),
        // Collinear vertices (zero area, non-coincident).
        Triangle3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0), Vec3<f32>(2, 0, 0)),
        // Two coincident vertices (degenerate "needle").
        Triangle3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 0, 0), Vec3<f32>(1, 0, 0)),
        // NaN vertex.
        Triangle3<f32>(Vec3<f32>(k_nan, 0, 0), Vec3<f32>(1, 0, 0), Vec3<f32>(0, 1, 0)),
        // ∞ vertex.
        Triangle3<f32>(Vec3<f32>(+k_inf, 0, 0), Vec3<f32>(1, 0, 0), Vec3<f32>(0, 1, 0)),
    };
}

// ---- Degenerate spheres ---------------------------------------------------

[[nodiscard]] inline std::vector<Sphere<f32>> degenerate_spheres()
{
    return {
        Sphere<f32>(Vec3<f32>(0, 0, 0), 0.0F),                  // point sphere
        Sphere<f32>(Vec3<f32>(0, 0, 0), -1.0F),                 // negative radius — should never-hit
        Sphere<f32>(Vec3<f32>(k_nan, 0, 0), 1.0F),              // NaN center
        Sphere<f32>(Vec3<f32>(0, 0, 0), k_nan),                 // NaN radius
        Sphere<f32>(Vec3<f32>(0, 0, 0), +k_inf),                // ∞ radius
        Sphere<f32>(Vec3<f32>(+k_inf, 0, 0), 1.0F),             // ∞ center
    };
}

// ---- Degenerate rays ------------------------------------------------------

[[nodiscard]] inline std::vector<Ray3<f32>> degenerate_rays()
{
    return {
        Ray3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(0, 0, 0)),           // zero direction
        Ray3<f32>(Vec3<f32>(k_nan, 0, 0), Vec3<f32>(1, 0, 0)),       // NaN origin
        Ray3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(k_nan, 0, 0)),       // NaN direction
        Ray3<f32>(Vec3<f32>(0, 0, 0), Vec3<f32>(+k_inf, 0, 0)),      // ∞ direction
        Ray3<f32>(Vec3<f32>(+k_inf, 0, 0), Vec3<f32>(1, 0, 0)),      // ∞ origin
    };
}

// ---- Large-coordinate sweep -----------------------------------------------
//
// Shift the geometry to a far origin (+1e6 / +1e7) and verify queries still
// answer correctly within an f32-precision tolerance. The use case is large
// game worlds / robotics scenes where coordinates routinely live far from
// zero — and the regression case is a hot-path subtraction `(a - b)` that
// loses precision because both `a` and `b` are huge but their difference is
// small.

inline constexpr f32 k_far_origin_modest = 1.0e6F;  // safe — every f32 op stays in the integer-exact regime for ints up to ~16M
inline constexpr f32 k_far_origin_stress = 1.0e7F;  // stress — single-precision ULP at this magnitude is ~1.0

[[nodiscard]] inline Vec3<f32> shift(const Vec3<f32>& p, const Vec3<f32>& offset) noexcept
{
    return Vec3<f32>(p.x + offset.x, p.y + offset.y, p.z + offset.z);
}

[[nodiscard]] inline AABB3<f32> shift(const AABB3<f32>& a, const Vec3<f32>& offset) noexcept
{
    return AABB3<f32>(shift(a.min, offset), shift(a.max, offset));
}

[[nodiscard]] inline Sphere<f32> shift(const Sphere<f32>& s, const Vec3<f32>& offset) noexcept
{
    return Sphere<f32>(shift(s.center, offset), s.radius);
}

[[nodiscard]] inline Ray3<f32> shift(const Ray3<f32>& r, const Vec3<f32>& offset) noexcept
{
    return Ray3<f32>(shift(r.origin, offset), r.direction); // direction unchanged
}

// `f32` ULP-tolerance for a coordinate value `c` at the given origin offset
// — when the origin is `O(1e6)`, ULP is `O(0.06)`; when `O(1e7)`, ULP is
// `O(1.0)`. Callers comparing query outputs across a shifted scene should
// use this as the comparison slack, not `1e-6`.
[[nodiscard]] inline f32 ulp_tolerance_for(f32 magnitude) noexcept
{
    // ULP(x) ≈ 2^(floor(log2(|x|)) - 23). Conservative upper bound = |x| / 2^22.
    const f32 m = magnitude < 0.0F ? -magnitude : magnitude;
    return m < 1.0F ? 1.0e-6F : m / static_cast<f32>(1U << 22);
}

} // namespace crd::geometry::test_corpus
