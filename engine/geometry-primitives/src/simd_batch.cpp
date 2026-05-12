// crd-geometry-primitives — SIMD batch intersection kernels (v0f).
// See `crd/geometry/primitives/simd_batch.hpp` for the contract. Out-of-line
// (f32-only) so `crd-simd-emission-check` has a TU with real SIMD `.obj`.

#include <crd/geometry/primitives/simd_batch.hpp>
#include <crd/math/scalar.hpp>

#include <limits>

namespace crd::geometry::primitives
{
namespace
{
// Mask combinators built from `select` (the cmp_* results are all-bits-set
// masks): `and_mask(a,b)` keeps b's lane where a is set, else 0; `or_mask(a,b)`
// keeps a's lane where a is set (all-ones), else b's lane.
[[nodiscard]] inline Vec8f and_mask(Vec8f a, Vec8f b) noexcept
{
    return select(a, b, Vec8f::zero());
}
[[nodiscard]] inline Vec8f clamp01(Vec8f x) noexcept
{
    return max(Vec8f::zero(), min(Vec8f::one(), x));
}
// Smallest positive normal — "truly zero" threshold for squared lengths /
// determinants; also the denominator pad that keeps a divide finite when the
// denominator is exactly 0 (the result is then huge and clamped away by the
// surrounding mask).
[[nodiscard]] inline Vec8f tiny() noexcept
{
    return Vec8f(std::numeric_limits<crd::f32>::min());
}
} // namespace

// ---------------------------------------------------------------------------
// Ray ↔ N AABB — leaf-batch (one ray, 8 child boxes; Williams 2005 slab).
// ---------------------------------------------------------------------------

RayAabb8Result ray_vs_8_aabb(const Ray3<crd::f32>& ray, const RayAABBPrecompute<crd::f32>& pre, const Aabb8& boxes,
                             crd::f32 t0, crd::f32 t1) noexcept
{
    const Vec8f inv_x(pre.inv_dir.x);
    const Vec8f inv_y(pre.inv_dir.y);
    const Vec8f inv_z(pre.inv_dir.z);
    const Vec8f ox(ray.origin.x);
    const Vec8f oy(ray.origin.y);
    const Vec8f oz(ray.origin.z);

    // Near/far box plane per the (scalar) ray sign — picks a whole Vec8f column.
    const Vec8f& bnx = pre.sign[0] != 0U ? boxes.max_x : boxes.min_x;
    const Vec8f& bfx = pre.sign[0] != 0U ? boxes.min_x : boxes.max_x;
    const Vec8f& bny = pre.sign[1] != 0U ? boxes.max_y : boxes.min_y;
    const Vec8f& bfy = pre.sign[1] != 0U ? boxes.min_y : boxes.max_y;
    const Vec8f& bnz = pre.sign[2] != 0U ? boxes.max_z : boxes.min_z;
    const Vec8f& bfz = pre.sign[2] != 0U ? boxes.min_z : boxes.max_z;

    Vec8f tmin = (bnx - ox) * inv_x;
    Vec8f tmax = (bfx - ox) * inv_x;
    tmin = max(tmin, (bny - oy) * inv_y);
    tmax = min(tmax, (bfy - oy) * inv_y);
    tmin = max(tmin, (bnz - oz) * inv_z);
    tmax = min(tmax, (bfz - oz) * inv_z);
    tmax = tmax * Vec8f(ray_aabb_robust_pad<crd::f32>()); // Ize 2013 — conservative

    const Vec8f t0v(t0);
    const Vec8f t1v(t1);
    const Vec8f hit = and_mask(cmp_le(tmin, tmax), and_mask(cmp_le(tmin, t1v), cmp_ge(tmax, t0v)));
    return RayAabb8Result{max(tmin, t0v), hit};
}

// ---------------------------------------------------------------------------
// Ray packet ↔ AABB — packet mode (8 coherent rays, one box; per-lane signs via
// the min(t1,t2)/max(t1,t2) form, so no explicit sign bits).
// ---------------------------------------------------------------------------

RayAabb8Result ray_packet8_vs_aabb(const RayPacket8& p, const AABB3<crd::f32>& box, crd::f32 t0, crd::f32 t1) noexcept
{
    const Vec8f bminx(box.min.x);
    const Vec8f bmaxx(box.max.x);
    const Vec8f bminy(box.min.y);
    const Vec8f bmaxy(box.max.y);
    const Vec8f bminz(box.min.z);
    const Vec8f bmaxz(box.max.z);

    const Vec8f t1x = (bminx - p.origin_x) * p.inv_dir_x;
    const Vec8f t2x = (bmaxx - p.origin_x) * p.inv_dir_x;
    Vec8f tmin = min(t1x, t2x);
    Vec8f tmax = max(t1x, t2x);
    const Vec8f t1y = (bminy - p.origin_y) * p.inv_dir_y;
    const Vec8f t2y = (bmaxy - p.origin_y) * p.inv_dir_y;
    tmin = max(tmin, min(t1y, t2y));
    tmax = min(tmax, max(t1y, t2y));
    const Vec8f t1z = (bminz - p.origin_z) * p.inv_dir_z;
    const Vec8f t2z = (bmaxz - p.origin_z) * p.inv_dir_z;
    tmin = max(tmin, min(t1z, t2z));
    tmax = min(tmax, max(t1z, t2z));
    tmax = tmax * Vec8f(ray_aabb_robust_pad<crd::f32>());

    const Vec8f t0v(t0);
    const Vec8f t1v(t1);
    const Vec8f hit = and_mask(cmp_le(tmin, tmax), and_mask(cmp_le(tmin, t1v), cmp_ge(tmax, t0v)));
    return RayAabb8Result{max(tmin, t0v), hit};
}

// ---------------------------------------------------------------------------
// Ray ↔ N triangle — Möller-Trumbore × 8.
// ---------------------------------------------------------------------------

RayTri8Result ray_vs_8_triangle(const Ray3<crd::f32>& ray, const Triangle38& tris, bool cull_back,
                                crd::f32 tnear) noexcept
{
    const Vec8f ox(ray.origin.x);
    const Vec8f oy(ray.origin.y);
    const Vec8f oz(ray.origin.z);
    const Vec8f dx(ray.direction.x);
    const Vec8f dy(ray.direction.y);
    const Vec8f dz(ray.direction.z);

    const Vec8f e1x = tris.bx - tris.ax;
    const Vec8f e1y = tris.by - tris.ay;
    const Vec8f e1z = tris.bz - tris.az;
    const Vec8f e2x = tris.cx - tris.ax;
    const Vec8f e2y = tris.cy - tris.ay;
    const Vec8f e2z = tris.cz - tris.az;

    // pvec = cross(dir, e2)
    const Vec8f px = dy * e2z - dz * e2y;
    const Vec8f py = dz * e2x - dx * e2z;
    const Vec8f pz = dx * e2y - dy * e2x;
    const Vec8f det = e1x * px + e1y * py + e1z * pz;
    const Vec8f inv_det = Vec8f::one() / det; // ±∞ where det ≈ 0 → rejected by the |det| > eps check

    const Vec8f tvx = ox - tris.ax;
    const Vec8f tvy = oy - tris.ay;
    const Vec8f tvz = oz - tris.az;
    const Vec8f u = (tvx * px + tvy * py + tvz * pz) * inv_det;

    // qvec = cross(tvec, e1)
    const Vec8f qx = tvy * e1z - tvz * e1y;
    const Vec8f qy = tvz * e1x - tvx * e1z;
    const Vec8f qz = tvx * e1y - tvy * e1x;
    const Vec8f v = (dx * qx + dy * qy + dz * qz) * inv_det;
    const Vec8f t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;

    const Vec8f eps(crd::math::default_epsilon<crd::f32>());
    Vec8f hit = and_mask(
        cmp_ge(u, Vec8f::zero()),
        and_mask(cmp_ge(v, Vec8f::zero()),
                 and_mask(cmp_le(u + v, Vec8f::one()), and_mask(cmp_ge(t, Vec8f(tnear)), cmp_gt(abs(det), eps)))));
    if (cull_back)
    {
        hit = and_mask(hit, cmp_gt(det, Vec8f::zero()));
    }
    return RayTri8Result{t, u, v, hit};
}

// ---------------------------------------------------------------------------
// Trivial overlap masks.
// ---------------------------------------------------------------------------

Vec8f aabb8_vs_aabb(const Aabb8& boxes, const AABB3<crd::f32>& box) noexcept
{
    const Vec8f bminx(box.min.x);
    const Vec8f bmaxx(box.max.x);
    const Vec8f bminy(box.min.y);
    const Vec8f bmaxy(box.max.y);
    const Vec8f bminz(box.min.z);
    const Vec8f bmaxz(box.max.z);
    return and_mask(and_mask(cmp_le(boxes.min_x, bmaxx), cmp_ge(boxes.max_x, bminx)),
                    and_mask(and_mask(cmp_le(boxes.min_y, bmaxy), cmp_ge(boxes.max_y, bminy)),
                             and_mask(cmp_le(boxes.min_z, bmaxz), cmp_ge(boxes.max_z, bminz))));
}

Vec8f sphere8_vs_sphere(const Sphere8& spheres, const Sphere<crd::f32>& sphere) noexcept
{
    const Vec8f cx(sphere.center.x);
    const Vec8f cy(sphere.center.y);
    const Vec8f cz(sphere.center.z);
    const Vec8f dx = spheres.center_x - cx;
    const Vec8f dy = spheres.center_y - cy;
    const Vec8f dz = spheres.center_z - cz;
    const Vec8f d2 = dx * dx + dy * dy + dz * dz;
    const Vec8f rr = spheres.radius + Vec8f(sphere.radius);
    return cmp_le(d2, rr * rr);
}

// ---------------------------------------------------------------------------
// Segment ↔ N segment — squared closest distance (Ericson §5.1.9 in SIMD).
// ---------------------------------------------------------------------------

Vec8f segment8_vs_segment_distsq(const Segment38Pair& s) noexcept
{
    const Vec8f d1x = s.q1x - s.p1x;
    const Vec8f d1y = s.q1y - s.p1y;
    const Vec8f d1z = s.q1z - s.p1z;
    const Vec8f d2x = s.q2x - s.p2x;
    const Vec8f d2y = s.q2y - s.p2y;
    const Vec8f d2z = s.q2z - s.p2z;
    const Vec8f rx = s.p1x - s.p2x;
    const Vec8f ry = s.p1y - s.p2y;
    const Vec8f rz = s.p1z - s.p2z;

    const Vec8f a = d1x * d1x + d1y * d1y + d1z * d1z; // |d1|²
    const Vec8f e = d2x * d2x + d2y * d2y + d2z * d2z; // |d2|²
    const Vec8f f = d2x * rx + d2y * ry + d2z * rz;
    const Vec8f c = d1x * rx + d1y * ry + d1z * rz;
    const Vec8f b = d1x * d2x + d1y * d2y + d1z * d2z;

    const Vec8f eps = tiny();
    const Vec8f a_deg = cmp_le(a, eps); // segment 1 collapses to a point
    const Vec8f e_deg = cmp_le(e, eps); // segment 2 collapses to a point
    const Vec8f denom = a * e - b * b;  // ≥ 0
    const Vec8f denom_ok = cmp_gt(denom, eps);

    // General (non-degenerate) branch.
    Vec8f s_param = select(denom_ok, clamp01((b * f - c * e) / (denom + eps)), Vec8f::zero());
    Vec8f t_param = (b * s_param + f) / (e + eps);
    const Vec8f t_lt0 = cmp_lt(t_param, Vec8f::zero());
    const Vec8f t_gt1 = cmp_gt(t_param, Vec8f::one());
    const Vec8f s_at_t0 = clamp01((-c) / (a + eps));
    const Vec8f s_at_t1 = clamp01((b - c) / (a + eps));
    s_param = select(t_lt0, s_at_t0, select(t_gt1, s_at_t1, s_param));
    t_param = select(t_lt0, Vec8f::zero(), select(t_gt1, Vec8f::one(), t_param));

    // Degenerate overrides (later select wins; both-degenerate → s=t=0):
    //   seg1 a point  ⇒ s = 0, t = clamp(f/e, 0, 1)
    //   seg2 a point  ⇒ t = 0, s = clamp(-c/a, 0, 1)
    const Vec8f t_seg1pt = clamp01(f / (e + eps));
    const Vec8f s_seg2pt = clamp01((-c) / (a + eps));
    t_param = select(e_deg, Vec8f::zero(), select(a_deg, t_seg1pt, t_param));
    s_param = select(a_deg, Vec8f::zero(), select(e_deg, s_seg2pt, s_param));

    // Closest points and the squared distance between them.
    const Vec8f c1x = s.p1x + d1x * s_param;
    const Vec8f c1y = s.p1y + d1y * s_param;
    const Vec8f c1z = s.p1z + d1z * s_param;
    const Vec8f c2x = s.p2x + d2x * t_param;
    const Vec8f c2y = s.p2y + d2y * t_param;
    const Vec8f c2z = s.p2z + d2z * t_param;
    const Vec8f gx = c1x - c2x;
    const Vec8f gy = c1y - c2y;
    const Vec8f gz = c1z - c2z;
    return gx * gx + gy * gy + gz * gz;
}

} // namespace crd::geometry::primitives
