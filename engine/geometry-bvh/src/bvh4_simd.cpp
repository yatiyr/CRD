#include <crd/geometry/bvh/bvh4_simd.hpp>

namespace crd::geometry::bvh
{
using crd::f32;
using crd::math::simd::cmp_le;
using crd::math::simd::max;
using crd::math::simd::min;

Ray4AabbResult ray_vs_4_aabb(const Ray3<f32>& ray, const crd::geometry::primitives::RayAABBPrecompute<f32>& pre,
                             const Vec4f& bmin_x, const Vec4f& bmin_y, const Vec4f& bmin_z, const Vec4f& bmax_x,
                             const Vec4f& bmax_y, const Vec4f& bmax_z, f32 t0, f32 t1) noexcept
{
    const Vec4f ox(ray.origin.x);
    const Vec4f oy(ray.origin.y);
    const Vec4f oz(ray.origin.z);
    const Vec4f ix(pre.inv_dir.x);
    const Vec4f iy(pre.inv_dir.y);
    const Vec4f iz(pre.inv_dir.z);

    // Per-axis slab entry/exit, Tavianator form (NaN/∞-direction-safe via min/max
    // ordering — a zero direction component gives ±∞ and that axis drops out).
    const Vec4f tx0 = (bmin_x - ox) * ix;
    const Vec4f tx1 = (bmax_x - ox) * ix;
    const Vec4f ty0 = (bmin_y - oy) * iy;
    const Vec4f ty1 = (bmax_y - oy) * iy;
    const Vec4f tz0 = (bmin_z - oz) * iz;
    const Vec4f tz1 = (bmax_z - oz) * iz;

    const Vec4f tmin = max(max(min(tx0, tx1), min(ty0, ty1)), min(tz0, tz1));
    Vec4f tmax = min(min(max(tx0, tx1), max(ty0, ty1)), max(tz0, tz1));
    tmax = tmax * Vec4f(crd::geometry::primitives::ray_aabb_robust_pad<f32>()); // Ize 2013 — conservative

    const Vec4f lo = max(tmin, Vec4f(t0));
    const Vec4f hi = min(tmax, Vec4f(t1));
    return Ray4AabbResult{lo, cmp_le(lo, hi)};
}

Ray4AabbResult ray_vs_4_aabb_inflated(const Ray3<f32>& ray,
                                      const crd::geometry::primitives::RayAABBPrecompute<f32>& pre, const Vec4f& bmin_x,
                                      const Vec4f& bmin_y, const Vec4f& bmin_z, const Vec4f& bmax_x,
                                      const Vec4f& bmax_y, const Vec4f& bmax_z, f32 pad_x, f32 pad_y, f32 pad_z, f32 t0,
                                      f32 t1) noexcept
{
    // Inflate the input columns by (pad_x, pad_y, pad_z) — broadcast to all
    // 4 lanes — then run the same Tavianator slab chain as `ray_vs_4_aabb`.
    // The pad goes outward (subtract from min, add to max).
    const Vec4f px(pad_x);
    const Vec4f py(pad_y);
    const Vec4f pz(pad_z);
    const Vec4f imin_x = bmin_x - px;
    const Vec4f imin_y = bmin_y - py;
    const Vec4f imin_z = bmin_z - pz;
    const Vec4f imax_x = bmax_x + px;
    const Vec4f imax_y = bmax_y + py;
    const Vec4f imax_z = bmax_z + pz;
    return ray_vs_4_aabb(ray, pre, imin_x, imin_y, imin_z, imax_x, imax_y, imax_z, t0, t1);
}

} // namespace crd::geometry::bvh
