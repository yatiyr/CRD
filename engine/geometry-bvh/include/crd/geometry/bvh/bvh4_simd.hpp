#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — `Vec4f` ray-vs-4-AABB kernel (v1g).
//
// One ray vs four AABBs (held SoA: `bmin_x` = the four boxes' min.x, etc.) in a
// single `Vec4f` min/max chain — the Tavianator branchless slab test broadcast
// to 4 lanes, with the Ize 2013 conservative `tmax` widening (`× (1 + 2γ₃)`)
// applied per lane. For BVH4 traversal: one node fetch, one of these per node,
// versus four sequential scalar slab tests. Bit-identical (for finite/well-
// formed inputs) to four `intersect_ray_aabb_robust` calls — the Tavianator
// `min`/`max` form picks the same near/far plane the Williams `sign`-bit form
// does. Out-of-line in `src/bvh4_simd.cpp` (a real TU) — `Vec4f` is 128-bit
// (`xmm`) at every SIMD level, so it isn't wired into the AVX2-`ymm`-expecting
// `crd-simd-emission-check` yet (a 128-bit-aware variant is a follow-up, same as
// the `simd_batch.cpp` one).
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>      // Ray3
#include <crd/geometry/primitives/robust_ray_aabb.hpp> // RayAABBPrecompute, ray_aabb_robust_pad
#include <crd/math/simd/vec4f.hpp>

namespace crd::geometry::bvh
{
using crd::geometry::primitives::Ray3;
using crd::math::simd::Vec4f;

struct Ray4AabbResult
{
    Vec4f t_enter{};  // per-lane entry parameter (clamped to t0); meaningless where hit_mask lane == 0
    Vec4f hit_mask{}; // per-lane all-bits-set on hit, all-zero on miss (consume with `select` or `store`+scan)
};

// `ray` precomputed via `precompute_ray_aabb` (`pre.inv_dir`); `t0`/`t1` = the
// traversal parameter window (BVH4 traversal passes `0` and the current best
// hit `t`). Lanes whose box you didn't fill are still computed — the caller
// just ignores lanes ≥ child_count.
[[nodiscard]] Ray4AabbResult ray_vs_4_aabb(const Ray3<crd::f32>& ray,
                                           const crd::geometry::primitives::RayAABBPrecompute<crd::f32>& pre,
                                           const Vec4f& bmin_x, const Vec4f& bmin_y, const Vec4f& bmin_z,
                                           const Vec4f& bmax_x, const Vec4f& bmax_y, const Vec4f& bmax_z, crd::f32 t0,
                                           crd::f32 t1) noexcept;

} // namespace crd::geometry::bvh
