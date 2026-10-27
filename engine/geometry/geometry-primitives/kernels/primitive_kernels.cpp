// The geometry-primitives kernels of crd/geometry/primitives/primitive_kernels.hpp, compiled once per CPU level by
// crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace
// (CRD_MATH_LEVEL_NS). Each body runs one bundle of eight lanes per step on the level's Vec8f (one 256-bit register
// on avx2, two 128-bit ones on sse2, scalar lanes on portable), every product and sum rounded on its own, so every
// level gives the portable bits. Comparisons give all-ones masks; min and max use the IEEE ordering, so a lane whose
// axis has an infinite inverse direction drops that axis instead of poisoning the result.

#include <crd/geometry/primitives/detail/batch8_core.hpp>
#include <crd/geometry/primitives/primitive_kernels.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <cstring>

namespace crd::geometry::primitives::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::f32;
using crd::u32;
using crd::usize;
using crd::math::simd::Vec8f;

// b's lane where a is set, else 0 (the masks are all ones or zero).
CRD_FORCEINLINE Vec8f and_mask(Vec8f a, Vec8f b) noexcept
{
    return select(a, b, Vec8f::zero());
}

CRD_FORCEINLINE Vec8f clamp01(Vec8f x) noexcept
{
    return max(Vec8f::zero(), min(Vec8f::one(), x));
}

CRD_FORCEINLINE void store_mask(Vec8f mask, u32 (&out)[kBatchLanes]) noexcept
{
    alignas(32) f32 lanes[kBatchLanes];
    mask.store(lanes);
    std::memcpy(out, lanes, sizeof(lanes));
}

// The slab entry clamped to t0 and the hit mask of [tmin, tmax] (tmax widened by pad) against [t0, t1].
CRD_FORCEINLINE void store_slab(Vec8f tmin, Vec8f tmax, f32 pad, f32 t0, f32 t1, RayAabb8Result& out) noexcept
{
    tmax = tmax * Vec8f(pad); // Ize 2013: conservative
    const Vec8f t0v(t0);
    const Vec8f t1v(t1);
    const Vec8f hit = and_mask(cmp_le(tmin, tmax), and_mask(cmp_le(tmin, t1v), cmp_ge(tmax, t0v)));
    max(tmin, t0v).store(out.t_enter);
    store_mask(hit, out.hit_mask);
}

void ray_vs_aabb8(const RaySlab& ray, const Aabb8* boxes, usize n, f32 t0, f32 t1, RayAabb8Result* out) noexcept
{
    const Vec8f inv_x(ray.inv_dir[0]);
    const Vec8f inv_y(ray.inv_dir[1]);
    const Vec8f inv_z(ray.inv_dir[2]);
    const Vec8f ox(ray.origin[0]);
    const Vec8f oy(ray.origin[1]);
    const Vec8f oz(ray.origin[2]);
    for (usize i = 0; i < n; ++i)
    {
        const Aabb8& b = boxes[i];
        // The near and far plane of each axis by the ray's sign: one whole column.
        const f32* bnx = ray.sign[0] != 0U ? b.max_x : b.min_x;
        const f32* bfx = ray.sign[0] != 0U ? b.min_x : b.max_x;
        const f32* bny = ray.sign[1] != 0U ? b.max_y : b.min_y;
        const f32* bfy = ray.sign[1] != 0U ? b.min_y : b.max_y;
        const f32* bnz = ray.sign[2] != 0U ? b.max_z : b.min_z;
        const f32* bfz = ray.sign[2] != 0U ? b.min_z : b.max_z;

        Vec8f tmin = (Vec8f::load(bnx) - ox) * inv_x;
        Vec8f tmax = (Vec8f::load(bfx) - ox) * inv_x;
        tmin = max(tmin, (Vec8f::load(bny) - oy) * inv_y);
        tmax = min(tmax, (Vec8f::load(bfy) - oy) * inv_y);
        tmin = max(tmin, (Vec8f::load(bnz) - oz) * inv_z);
        tmax = min(tmax, (Vec8f::load(bfz) - oz) * inv_z);
        store_slab(tmin, tmax, ray.pad, t0, t1, out[i]);
    }
}

void packet8_vs_aabb(const RayPacket8& p, const Box3* boxes, usize n, f32 pad, f32 t0, f32 t1,
                     RayAabb8Result* out) noexcept
{
    const Vec8f ox = Vec8f::load(p.origin_x);
    const Vec8f oy = Vec8f::load(p.origin_y);
    const Vec8f oz = Vec8f::load(p.origin_z);
    const Vec8f ix = Vec8f::load(p.inv_dir_x);
    const Vec8f iy = Vec8f::load(p.inv_dir_y);
    const Vec8f iz = Vec8f::load(p.inv_dir_z);
    for (usize i = 0; i < n; ++i)
    {
        const Box3& box = boxes[i];
        const Vec8f t1x = (Vec8f(box.min[0]) - ox) * ix;
        const Vec8f t2x = (Vec8f(box.max[0]) - ox) * ix;
        Vec8f tmin = min(t1x, t2x);
        Vec8f tmax = max(t1x, t2x);
        const Vec8f t1y = (Vec8f(box.min[1]) - oy) * iy;
        const Vec8f t2y = (Vec8f(box.max[1]) - oy) * iy;
        tmin = max(tmin, min(t1y, t2y));
        tmax = min(tmax, max(t1y, t2y));
        const Vec8f t1z = (Vec8f(box.min[2]) - oz) * iz;
        const Vec8f t2z = (Vec8f(box.max[2]) - oz) * iz;
        tmin = max(tmin, min(t1z, t2z));
        tmax = min(tmax, max(t1z, t2z));
        store_slab(tmin, tmax, pad, t0, t1, out[i]);
    }
}

void ray_vs_triangle8(const RaySlab& ray, const Triangle38* tris, usize n, bool cull_back, f32 tnear, f32 eps,
                      RayTri8Result* out) noexcept
{
    const Vec8f epsv(eps);
    const Vec8f tnearv(tnear);
    for (usize i = 0; i < n; ++i)
    {
        const Triangle38& tr = tris[i];
        const detail::Mt8 r = detail::moller_trumbore8(
            ray.origin, ray.direction, Vec8f::load(tr.ax), Vec8f::load(tr.ay), Vec8f::load(tr.az), Vec8f::load(tr.bx),
            Vec8f::load(tr.by), Vec8f::load(tr.bz), Vec8f::load(tr.cx), Vec8f::load(tr.cy), Vec8f::load(tr.cz));
        Vec8f hit = and_mask(
            cmp_ge(r.u, Vec8f::zero()),
            and_mask(cmp_ge(r.v, Vec8f::zero()), and_mask(cmp_le(r.u + r.v, Vec8f::one()),
                                                          and_mask(cmp_ge(r.t, tnearv), cmp_gt(abs(r.det), epsv)))));
        if (cull_back)
        {
            hit = and_mask(hit, cmp_gt(r.det, Vec8f::zero()));
        }
        r.t.store(out[i].t);
        r.u.store(out[i].u);
        r.v.store(out[i].v);
        store_mask(hit, out[i].hit_mask);
    }
}

void aabb8_vs_aabb(const Aabb8* boxes, usize n, const Box3& box, Mask8* out) noexcept
{
    const Vec8f bminx(box.min[0]);
    const Vec8f bmaxx(box.max[0]);
    const Vec8f bminy(box.min[1]);
    const Vec8f bmaxy(box.max[1]);
    const Vec8f bminz(box.min[2]);
    const Vec8f bmaxz(box.max[2]);
    for (usize i = 0; i < n; ++i)
    {
        const Aabb8& b = boxes[i];
        const Vec8f x = and_mask(cmp_le(Vec8f::load(b.min_x), bmaxx), cmp_ge(Vec8f::load(b.max_x), bminx));
        const Vec8f y = and_mask(cmp_le(Vec8f::load(b.min_y), bmaxy), cmp_ge(Vec8f::load(b.max_y), bminy));
        const Vec8f z = and_mask(cmp_le(Vec8f::load(b.min_z), bmaxz), cmp_ge(Vec8f::load(b.max_z), bminz));
        store_mask(and_mask(x, and_mask(y, z)), out[i].lane);
    }
}

void sphere8_vs_sphere(const Sphere8* spheres, usize n, const f32 (&center)[3], f32 radius, Mask8* out) noexcept
{
    const Vec8f cx(center[0]);
    const Vec8f cy(center[1]);
    const Vec8f cz(center[2]);
    const Vec8f r(radius);
    for (usize i = 0; i < n; ++i)
    {
        const Sphere8& s = spheres[i];
        const Vec8f dx = Vec8f::load(s.center_x) - cx;
        const Vec8f dy = Vec8f::load(s.center_y) - cy;
        const Vec8f dz = Vec8f::load(s.center_z) - cz;
        const Vec8f d2 = dx * dx + dy * dy + dz * dz;
        const Vec8f rr = Vec8f::load(s.radius) + r;
        store_mask(cmp_le(d2, rr * rr), out[i].lane);
    }
}

Vec8f segment_distsq(const Segment38Pair& s, Vec8f eps) noexcept
{
    const Vec8f p1x = Vec8f::load(s.p1x);
    const Vec8f p1y = Vec8f::load(s.p1y);
    const Vec8f p1z = Vec8f::load(s.p1z);
    const Vec8f p2x = Vec8f::load(s.p2x);
    const Vec8f p2y = Vec8f::load(s.p2y);
    const Vec8f p2z = Vec8f::load(s.p2z);
    const Vec8f d1x = Vec8f::load(s.q1x) - p1x;
    const Vec8f d1y = Vec8f::load(s.q1y) - p1y;
    const Vec8f d1z = Vec8f::load(s.q1z) - p1z;
    const Vec8f d2x = Vec8f::load(s.q2x) - p2x;
    const Vec8f d2y = Vec8f::load(s.q2y) - p2y;
    const Vec8f d2z = Vec8f::load(s.q2z) - p2z;
    const Vec8f rx = p1x - p2x;
    const Vec8f ry = p1y - p2y;
    const Vec8f rz = p1z - p2z;

    const Vec8f a = d1x * d1x + d1y * d1y + d1z * d1z; // |d1|^2
    const Vec8f e = d2x * d2x + d2y * d2y + d2z * d2z; // |d2|^2
    const Vec8f f = d2x * rx + d2y * ry + d2z * rz;
    const Vec8f c = d1x * rx + d1y * ry + d1z * rz;
    const Vec8f b = d1x * d2x + d1y * d2y + d1z * d2z;

    const Vec8f a_deg = cmp_le(a, eps); // segment 1 is a point
    const Vec8f e_deg = cmp_le(e, eps); // segment 2 is a point
    const Vec8f denom = a * e - b * b;  // >= 0
    const Vec8f denom_ok = cmp_gt(denom, eps);

    // The general case.
    Vec8f s_param = select(denom_ok, clamp01((b * f - c * e) / (denom + eps)), Vec8f::zero());
    Vec8f t_param = (b * s_param + f) / (e + eps);
    const Vec8f t_lt0 = cmp_lt(t_param, Vec8f::zero());
    const Vec8f t_gt1 = cmp_gt(t_param, Vec8f::one());
    const Vec8f s_at_t0 = clamp01((-c) / (a + eps));
    const Vec8f s_at_t1 = clamp01((b - c) / (a + eps));
    s_param = select(t_lt0, s_at_t0, select(t_gt1, s_at_t1, s_param));
    t_param = select(t_lt0, Vec8f::zero(), select(t_gt1, Vec8f::one(), t_param));

    // The degenerate overrides (the later select wins; both points give s = t = 0): segment 1 a point gives s = 0,
    // t = clamp(f/e); segment 2 a point gives t = 0, s = clamp(-c/a).
    const Vec8f t_seg1pt = clamp01(f / (e + eps));
    const Vec8f s_seg2pt = clamp01((-c) / (a + eps));
    t_param = select(e_deg, Vec8f::zero(), select(a_deg, t_seg1pt, t_param));
    s_param = select(a_deg, Vec8f::zero(), select(e_deg, s_seg2pt, s_param));

    const Vec8f gx = (p1x + d1x * s_param) - (p2x + d2x * t_param);
    const Vec8f gy = (p1y + d1y * s_param) - (p2y + d2y * t_param);
    const Vec8f gz = (p1z + d1z * s_param) - (p2z + d2z * t_param);
    return gx * gx + gy * gy + gz * gz;
}

void segment8_distsq(const Segment38Pair* pairs, usize n, f32 tiny, Lanes8* out) noexcept
{
    const Vec8f eps(tiny);
    for (usize i = 0; i < n; ++i)
    {
        segment_distsq(pairs[i], eps).store(out[i].lane);
    }
}

u32 hull_support(const f32* vx, const f32* vy, const f32* vz, usize padded, f32 dx, f32 dy, f32 dz) noexcept
{
    const Vec8f dxv(dx);
    const Vec8f dyv(dy);
    const Vec8f dzv(dz);
    // From -inf, as the linear scan starts: vertex 0 stays chosen when every projection is -inf or NaN.
    constexpr u32 negative_infinity = 0xFF800000U;
    f32 best_proj = 0.0F;
    std::memcpy(&best_proj, &negative_infinity, sizeof(best_proj));
    u32 best_idx = 0U;
    alignas(32) f32 proj_lanes[kBatchLanes];
    for (usize base = 0; base < padded; base += kBatchLanes)
    {
        const Vec8f proj = Vec8f::load(vx + base) * dxv + Vec8f::load(vy + base) * dyv + Vec8f::load(vz + base) * dzv;
        proj.store(proj_lanes);
        // Strictly greater wins, so an exact tie keeps the lower index; the padded lanes repeat vertex 0 and lose.
        for (u32 lane = 0; lane < kBatchLanes; ++lane)
        {
            if (proj_lanes[lane] > best_proj)
            {
                best_proj = proj_lanes[lane];
                best_idx = static_cast<u32>(base) + lane;
            }
        }
    }
    return best_idx;
}

} // namespace

} // namespace crd::geometry::primitives::inline CRD_MATH_LEVEL_NS

namespace crd::geometry::primitives
{

const PrimitiveKernels CRD_KERNEL_TABLE(kPrimitiveKernels) = {
    CRD_MATH_LEVEL_NS::ray_vs_aabb8,  CRD_MATH_LEVEL_NS::packet8_vs_aabb,   CRD_MATH_LEVEL_NS::ray_vs_triangle8,
    CRD_MATH_LEVEL_NS::aabb8_vs_aabb, CRD_MATH_LEVEL_NS::sphere8_vs_sphere, CRD_MATH_LEVEL_NS::segment8_distsq,
    CRD_MATH_LEVEL_NS::hull_support,
};

} // namespace crd::geometry::primitives
