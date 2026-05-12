#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — robust slab ray↔AABB, precomputed (v0f).
//
// Williams, Barrus, Morley, Shirley 2005, "An Efficient and Robust Ray-Box
// Intersection Algorithm" (JGT 10(1)) — precompute `inv_dir` + the per-axis
// sign bits once per ray, then each box test is 6 mul + a handful of
// branch-light min/max. NaN-safe: a zero `dir` component gives `inv_dir = ±∞`
// and the IEEE-ordered `min`/`max` simply drop that axis (no special-casing).
//
// Plus Ize 2013, "Robust BVH Ray Traversal" (JCGT 2(2)) — the `tmax`
// comparison is widened by `1 + 2·γ₃` (γ₃ = 3u/(1−3u), u = ½ulp) so the
// accumulated rounding of `inv_dir` and the slab multiplications can never make
// a hit that is "really" on the box surface slip through a BVH node. This is
// the single-ray precompute the `-bvh` v1g traversal consumes for its
// "leaf-batch" mode (one ray vs N child AABBs); the 8-ray SIMD *packet* form
// (N rays vs one box per node) is `ray_packet8_vs_aabb` in `simd_batch.hpp`.
//
// `intersect_ray_aabb` (the un-precomputed slab) lives in `intersect.hpp` (v0c)
// and stays as the cross-check reference for the non-degenerate corpus.
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>

#include <limits>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::Vec3;

// The conservative `tmax` widening from Ize 2013 (γ₃ = 3u/(1−3u), u = ½ulp):
// (1 + 2γ₃) bounds the relative error of `inv_dir`·(box − origin) so the slab
// test never rejects a true surface hit.
template <MathScalar T> [[nodiscard]] constexpr T ray_aabb_robust_pad() noexcept
{
    const T u = std::numeric_limits<T>::epsilon() / static_cast<T>(2);
    const T gamma3 = (static_cast<T>(3) * u) / (static_cast<T>(1) - static_cast<T>(3) * u);
    return static_cast<T>(1) + static_cast<T>(2) * gamma3;
}

// Per-ray precompute for the slab test. `inv_dir[i] = 1/dir[i]` (±∞ for a zero
// component — fine), `sign[i] = inv_dir[i] < 0`. (The dossier's "RayPacket" for
// the single-ray case.)
template <MathScalar T> struct RayAABBPrecompute
{
    Vec3<T> inv_dir{};
    crd::usize sign[3]{0, 0, 0};
};

template <MathScalar T> [[nodiscard]] inline RayAABBPrecompute<T> precompute_ray_aabb(const Ray3<T>& ray) noexcept
{
    RayAABBPrecompute<T> p;
    p.inv_dir = Vec3<T>(static_cast<T>(1) / ray.direction.x, static_cast<T>(1) / ray.direction.y,
                        static_cast<T>(1) / ray.direction.z);
    p.sign[0] = p.inv_dir.x < static_cast<T>(0) ? 1U : 0U;
    p.sign[1] = p.inv_dir.y < static_cast<T>(0) ? 1U : 0U;
    p.sign[2] = p.inv_dir.z < static_cast<T>(0) ? 1U : 0U;
    return p;
}

// Slab test: does `ray` (precomputed) hit `box` within the parameter window
// [t0, t1]? On a hit, `out_t` is `max(t0, t_enter)`. NaN/∞-direction-safe;
// `tmax` widened per Ize 2013.
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_aabb_robust(const Ray3<T>& ray, const RayAABBPrecompute<T>& p,
                                                    const AABB3<T>& box, T t0, T t1, T& out_t) noexcept
{
    const Vec3<T> b[2] = {box.min, box.max};

    T tmin = (b[p.sign[0]].x - ray.origin.x) * p.inv_dir.x;
    T tmax = (b[1U - p.sign[0]].x - ray.origin.x) * p.inv_dir.x;
    const T tymin = (b[p.sign[1]].y - ray.origin.y) * p.inv_dir.y;
    const T tymax = (b[1U - p.sign[1]].y - ray.origin.y) * p.inv_dir.y;
    if (tmin > tymax || tymin > tmax)
    {
        return false;
    }
    if (tymin > tmin)
    {
        tmin = tymin;
    }
    if (tymax < tmax)
    {
        tmax = tymax;
    }
    const T tzmin = (b[p.sign[2]].z - ray.origin.z) * p.inv_dir.z;
    const T tzmax = (b[1U - p.sign[2]].z - ray.origin.z) * p.inv_dir.z;
    if (tmin > tzmax || tzmin > tmax)
    {
        return false;
    }
    if (tzmin > tmin)
    {
        tmin = tzmin;
    }
    if (tzmax < tmax)
    {
        tmax = tzmax;
    }
    tmax *= ray_aabb_robust_pad<T>(); // Ize 2013 — conservative
    // Overlap with [t0, t1]?
    const T lo = tmin > t0 ? tmin : t0;
    const T hi = tmax < t1 ? tmax : t1;
    if (lo > hi)
    {
        return false;
    }
    out_t = lo;
    return true;
}

// Convenience: builds the per-ray precompute inline (use the precomputed form
// when testing one ray against many boxes).
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_aabb_robust(const Ray3<T>& ray, const AABB3<T>& box, T t0, T t1,
                                                    T& out_t) noexcept
{
    return intersect_ray_aabb_robust(ray, precompute_ray_aabb(ray), box, t0, t1, out_t);
}

} // namespace crd::geometry::primitives
