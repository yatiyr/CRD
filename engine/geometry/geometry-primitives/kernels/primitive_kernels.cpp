// The geometry-primitives kernels of crd/geometry/primitives/primitive_kernels.hpp, compiled once per CPU level by
// crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace
// (CRD_MATH_LEVEL_NS). Each body is written once over a register of lanes: one bundle of eight on the level's Vec8f
// (one 256-bit register on avx2, two 128-bit ones on sse2, scalar lanes on portable), and at avx512 two bundles a step
// on Vec16f (bundle i in the low half, i + 1 in the high one) with an odd last bundle on Vec8f. Every product and sum
// is rounded on its own and the lanes are independent, so every level gives the portable bits. Comparisons give
// all-ones masks; min and max use the IEEE ordering, so a lane whose axis has an infinite inverse direction drops that
// axis instead of poisoning the result.

#include <crd/geometry/primitives/detail/batch8_core.hpp>
#include <crd/geometry/primitives/primitive_kernels.hpp>
#include <crd/math/simd/vec8f.hpp>
#if CRD_SIMD_HAS_AVX512
#include <crd/math/simd/vec16f.hpp>
#endif

#include <cstring>
#include <type_traits>

namespace crd::geometry::primitives::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::f32;
using crd::u32;
using crd::usize;
using crd::math::simd::Vec8f;

// A column of one step's bundles: a's eight lanes, then b's at avx512 (b is a again on a Vec8f step).
template <typename V> CRD_FORCEINLINE V load_bundles(const f32* a, const f32* b) noexcept
{
    if constexpr (std::is_same_v<V, Vec8f>)
    {
        (void)b;
        return Vec8f::load(a);
    }
#if CRD_SIMD_HAS_AVX512
    else
    {
        return crd::math::simd::Vec16f(Vec8f::load(a), Vec8f::load(b));
    }
#endif
}

CRD_FORCEINLINE void store_bundles(Vec8f v, f32* a, f32* /*b: a again*/) noexcept
{
    v.store(a);
}

#if CRD_SIMD_HAS_AVX512
CRD_FORCEINLINE void store_bundles(crd::math::simd::Vec16f v, f32* a, f32* b) noexcept
{
    low_half(v).store(a);
    high_half(v).store(b);
}
#endif

// The bundles a register of V holds.
template <typename V> constexpr usize kBundles = sizeof(V) / sizeof(Vec8f);

// run<V>(first, last) over the bundles [0, n): pairs on Vec16f at avx512, then single bundles on Vec8f. A run hoists
// its constants and steps kBundles<V> bundles at a time, bundle i and i + kBundles<V> - 1.
template <typename Run> CRD_FORCEINLINE void for_bundles(usize n, Run run) noexcept
{
    usize i = 0;
#if CRD_SIMD_HAS_AVX512
    i = n & ~usize{1};
    run.template operator()<crd::math::simd::Vec16f>(usize{0}, i);
#endif
    run.template operator()<Vec8f>(i, n);
}

// b's lane where a is set, else 0 (the masks are all ones or zero).
template <typename V> CRD_FORCEINLINE V and_mask(V a, V b) noexcept
{
    return select(a, b, V::zero());
}

template <typename V> CRD_FORCEINLINE V clamp01(V x) noexcept
{
    return max(V::zero(), min(V::one(), x));
}

template <typename V> CRD_FORCEINLINE void store_mask(V mask, u32 (&a)[kBatchLanes], u32 (&b)[kBatchLanes]) noexcept
{
    alignas(64) f32 lanes[2U * kBatchLanes];
    store_bundles(mask, lanes, lanes + kBatchLanes);
    std::memcpy(a, lanes, sizeof(a));
    if constexpr (!std::is_same_v<V, Vec8f>)
    {
        std::memcpy(b, lanes + kBatchLanes, sizeof(b));
    }
}

// The slab entry clamped to t0 and the hit mask of [tmin, tmax] (tmax widened by pad) against [t0, t1].
template <typename V>
CRD_FORCEINLINE void store_slab(V tmin, V tmax, f32 pad, f32 t0, f32 t1, RayAabb8Result& a, RayAabb8Result& b) noexcept
{
    tmax = tmax * V(pad); // Ize 2013: conservative
    const V t0v(t0);
    const V t1v(t1);
    const V hit = and_mask(cmp_le(tmin, tmax), and_mask(cmp_le(tmin, t1v), cmp_ge(tmax, t0v)));
    store_bundles(max(tmin, t0v), a.t_enter, b.t_enter);
    store_mask(hit, a.hit_mask, b.hit_mask);
}

void ray_vs_aabb8(const RaySlab& ray, const Aabb8* boxes, usize n, f32 t0, f32 t1, RayAabb8Result* out) noexcept
{
    for_bundles(n,
                [&]<typename V>(usize first, usize last)
                {
                    const V inv_x(ray.inv_dir[0]);
                    const V inv_y(ray.inv_dir[1]);
                    const V inv_z(ray.inv_dir[2]);
                    const V ox(ray.origin[0]);
                    const V oy(ray.origin[1]);
                    const V oz(ray.origin[2]);
                    const bool sx = ray.sign[0] != 0U;
                    const bool sy = ray.sign[1] != 0U;
                    const bool sz = ray.sign[2] != 0U;
                    for (usize i = first; i < last; i += kBundles<V>)
                    {
                        const usize j = i + kBundles<V> - 1U;
                        const Aabb8& a = boxes[i];
                        const Aabb8& b = boxes[j];
                        // The near and far plane of each axis by the ray's sign: one whole column.
                        const V nx = sx ? load_bundles<V>(a.max_x, b.max_x) : load_bundles<V>(a.min_x, b.min_x);
                        const V fx = sx ? load_bundles<V>(a.min_x, b.min_x) : load_bundles<V>(a.max_x, b.max_x);
                        const V ny = sy ? load_bundles<V>(a.max_y, b.max_y) : load_bundles<V>(a.min_y, b.min_y);
                        const V fy = sy ? load_bundles<V>(a.min_y, b.min_y) : load_bundles<V>(a.max_y, b.max_y);
                        const V nz = sz ? load_bundles<V>(a.max_z, b.max_z) : load_bundles<V>(a.min_z, b.min_z);
                        const V fz = sz ? load_bundles<V>(a.min_z, b.min_z) : load_bundles<V>(a.max_z, b.max_z);
                        V tmin = (nx - ox) * inv_x;
                        V tmax = (fx - ox) * inv_x;
                        tmin = max(tmin, (ny - oy) * inv_y);
                        tmax = min(tmax, (fy - oy) * inv_y);
                        tmin = max(tmin, (nz - oz) * inv_z);
                        tmax = min(tmax, (fz - oz) * inv_z);
                        store_slab(tmin, tmax, ray.pad, t0, t1, out[i], out[j]);
                    }
                });
}

// A box coordinate of bundle i's box in the low half and bundle j's in the high one (one broadcast on Vec8f).
template <typename V> CRD_FORCEINLINE V planes(f32 lo, f32 hi) noexcept
{
    if constexpr (std::is_same_v<V, Vec8f>)
    {
        (void)hi;
        return Vec8f(lo);
    }
#if CRD_SIMD_HAS_AVX512
    else
    {
        return crd::math::simd::Vec16f(Vec8f(lo), Vec8f(hi));
    }
#endif
}

void packet8_vs_aabb(const RayPacket8& p, const Box3* boxes, usize n, f32 pad, f32 t0, f32 t1,
                     RayAabb8Result* out) noexcept
{
    // The packet's eight rays in both halves of a Vec16f step, against box i in the low half and box i + 1 in the high.
    for_bundles(n,
                [&]<typename V>(usize first, usize last)
                {
                    const V ox = load_bundles<V>(p.origin_x, p.origin_x);
                    const V oy = load_bundles<V>(p.origin_y, p.origin_y);
                    const V oz = load_bundles<V>(p.origin_z, p.origin_z);
                    const V ix = load_bundles<V>(p.inv_dir_x, p.inv_dir_x);
                    const V iy = load_bundles<V>(p.inv_dir_y, p.inv_dir_y);
                    const V iz = load_bundles<V>(p.inv_dir_z, p.inv_dir_z);
                    for (usize i = first; i < last; i += kBundles<V>)
                    {
                        const usize j = i + kBundles<V> - 1U;
                        const Box3& a = boxes[i];
                        const Box3& b = boxes[j];
                        const V t1x = (planes<V>(a.min[0], b.min[0]) - ox) * ix;
                        const V t2x = (planes<V>(a.max[0], b.max[0]) - ox) * ix;
                        V tmin = min(t1x, t2x);
                        V tmax = max(t1x, t2x);
                        const V t1y = (planes<V>(a.min[1], b.min[1]) - oy) * iy;
                        const V t2y = (planes<V>(a.max[1], b.max[1]) - oy) * iy;
                        tmin = max(tmin, min(t1y, t2y));
                        tmax = min(tmax, max(t1y, t2y));
                        const V t1z = (planes<V>(a.min[2], b.min[2]) - oz) * iz;
                        const V t2z = (planes<V>(a.max[2], b.max[2]) - oz) * iz;
                        tmin = max(tmin, min(t1z, t2z));
                        tmax = min(tmax, max(t1z, t2z));
                        store_slab(tmin, tmax, pad, t0, t1, out[i], out[j]);
                    }
                });
}

void ray_vs_triangle8(const RaySlab& ray, const Triangle38* tris, usize n, bool cull_back, f32 tnear, f32 eps,
                      RayTri8Result* out) noexcept
{
    for_bundles(n,
                [&]<typename V>(usize first, usize last)
                {
                    const V epsv(eps);
                    const V tnearv(tnear);
                    for (usize i = first; i < last; i += kBundles<V>)
                    {
                        const usize j = i + kBundles<V> - 1U;
                        const Triangle38& a = tris[i];
                        const Triangle38& b = tris[j];
                        const detail::Mt<V> r = detail::moller_trumbore<V>(
                            ray.origin, ray.direction, load_bundles<V>(a.ax, b.ax), load_bundles<V>(a.ay, b.ay),
                            load_bundles<V>(a.az, b.az), load_bundles<V>(a.bx, b.bx), load_bundles<V>(a.by, b.by),
                            load_bundles<V>(a.bz, b.bz), load_bundles<V>(a.cx, b.cx), load_bundles<V>(a.cy, b.cy),
                            load_bundles<V>(a.cz, b.cz));
                        V hit = and_mask(cmp_ge(r.u, V::zero()),
                                         and_mask(cmp_ge(r.v, V::zero()),
                                                  and_mask(cmp_le(r.u + r.v, V::one()),
                                                           and_mask(cmp_ge(r.t, tnearv), cmp_gt(abs(r.det), epsv)))));
                        if (cull_back)
                        {
                            hit = and_mask(hit, cmp_gt(r.det, V::zero()));
                        }
                        store_bundles(r.t, out[i].t, out[j].t);
                        store_bundles(r.u, out[i].u, out[j].u);
                        store_bundles(r.v, out[i].v, out[j].v);
                        store_mask(hit, out[i].hit_mask, out[j].hit_mask);
                    }
                });
}

void aabb8_vs_aabb(const Aabb8* boxes, usize n, const Box3& box, Mask8* out) noexcept
{
    for_bundles(n,
                [&]<typename V>(usize first, usize last)
                {
                    const V bminx(box.min[0]);
                    const V bmaxx(box.max[0]);
                    const V bminy(box.min[1]);
                    const V bmaxy(box.max[1]);
                    const V bminz(box.min[2]);
                    const V bmaxz(box.max[2]);
                    for (usize i = first; i < last; i += kBundles<V>)
                    {
                        const usize j = i + kBundles<V> - 1U;
                        const Aabb8& a = boxes[i];
                        const Aabb8& b = boxes[j];
                        const V x = and_mask(cmp_le(load_bundles<V>(a.min_x, b.min_x), bmaxx),
                                             cmp_ge(load_bundles<V>(a.max_x, b.max_x), bminx));
                        const V y = and_mask(cmp_le(load_bundles<V>(a.min_y, b.min_y), bmaxy),
                                             cmp_ge(load_bundles<V>(a.max_y, b.max_y), bminy));
                        const V z = and_mask(cmp_le(load_bundles<V>(a.min_z, b.min_z), bmaxz),
                                             cmp_ge(load_bundles<V>(a.max_z, b.max_z), bminz));
                        store_mask(and_mask(x, and_mask(y, z)), out[i].lane, out[j].lane);
                    }
                });
}

void sphere8_vs_sphere(const Sphere8* spheres, usize n, const f32 (&center)[3], f32 radius, Mask8* out) noexcept
{
    for_bundles(n,
                [&]<typename V>(usize first, usize last)
                {
                    const V cx(center[0]);
                    const V cy(center[1]);
                    const V cz(center[2]);
                    const V r(radius);
                    for (usize i = first; i < last; i += kBundles<V>)
                    {
                        const usize j = i + kBundles<V> - 1U;
                        const Sphere8& a = spheres[i];
                        const Sphere8& b = spheres[j];
                        const V dx = load_bundles<V>(a.center_x, b.center_x) - cx;
                        const V dy = load_bundles<V>(a.center_y, b.center_y) - cy;
                        const V dz = load_bundles<V>(a.center_z, b.center_z) - cz;
                        const V d2 = dx * dx + dy * dy + dz * dz;
                        const V rr = load_bundles<V>(a.radius, b.radius) + r;
                        store_mask(cmp_le(d2, rr * rr), out[i].lane, out[j].lane);
                    }
                });
}

template <typename V> V segment_distsq(const Segment38Pair& sa, const Segment38Pair& sb, V eps) noexcept
{
    const V p1x = load_bundles<V>(sa.p1x, sb.p1x);
    const V p1y = load_bundles<V>(sa.p1y, sb.p1y);
    const V p1z = load_bundles<V>(sa.p1z, sb.p1z);
    const V p2x = load_bundles<V>(sa.p2x, sb.p2x);
    const V p2y = load_bundles<V>(sa.p2y, sb.p2y);
    const V p2z = load_bundles<V>(sa.p2z, sb.p2z);
    const V d1x = load_bundles<V>(sa.q1x, sb.q1x) - p1x;
    const V d1y = load_bundles<V>(sa.q1y, sb.q1y) - p1y;
    const V d1z = load_bundles<V>(sa.q1z, sb.q1z) - p1z;
    const V d2x = load_bundles<V>(sa.q2x, sb.q2x) - p2x;
    const V d2y = load_bundles<V>(sa.q2y, sb.q2y) - p2y;
    const V d2z = load_bundles<V>(sa.q2z, sb.q2z) - p2z;
    const V rx = p1x - p2x;
    const V ry = p1y - p2y;
    const V rz = p1z - p2z;

    const V a = d1x * d1x + d1y * d1y + d1z * d1z; // |d1|^2
    const V e = d2x * d2x + d2y * d2y + d2z * d2z; // |d2|^2
    const V f = d2x * rx + d2y * ry + d2z * rz;
    const V c = d1x * rx + d1y * ry + d1z * rz;
    const V b = d1x * d2x + d1y * d2y + d1z * d2z;

    const V a_deg = cmp_le(a, eps); // segment 1 is a point
    const V e_deg = cmp_le(e, eps); // segment 2 is a point
    const V denom = a * e - b * b;  // >= 0
    const V denom_ok = cmp_gt(denom, eps);

    // The general case.
    V s_param = select(denom_ok, clamp01((b * f - c * e) / (denom + eps)), V::zero());
    V t_param = (b * s_param + f) / (e + eps);
    const V t_lt0 = cmp_lt(t_param, V::zero());
    const V t_gt1 = cmp_gt(t_param, V::one());
    const V s_at_t0 = clamp01((-c) / (a + eps));
    const V s_at_t1 = clamp01((b - c) / (a + eps));
    s_param = select(t_lt0, s_at_t0, select(t_gt1, s_at_t1, s_param));
    t_param = select(t_lt0, V::zero(), select(t_gt1, V::one(), t_param));

    // The degenerate overrides (the later select wins; both points give s = t = 0): segment 1 a point gives s = 0,
    // t = clamp(f/e); segment 2 a point gives t = 0, s = clamp(-c/a).
    const V t_seg1pt = clamp01(f / (e + eps));
    const V s_seg2pt = clamp01((-c) / (a + eps));
    t_param = select(e_deg, V::zero(), select(a_deg, t_seg1pt, t_param));
    s_param = select(a_deg, V::zero(), select(e_deg, s_seg2pt, s_param));

    const V gx = (p1x + d1x * s_param) - (p2x + d2x * t_param);
    const V gy = (p1y + d1y * s_param) - (p2y + d2y * t_param);
    const V gz = (p1z + d1z * s_param) - (p2z + d2z * t_param);
    return gx * gx + gy * gy + gz * gz;
}

void segment8_distsq(const Segment38Pair* pairs, usize n, f32 tiny, Lanes8* out) noexcept
{
    for_bundles(n,
                [&]<typename V>(usize first, usize last)
                {
                    const V eps(tiny);
                    for (usize i = first; i < last; i += kBundles<V>)
                    {
                        const usize j = i + kBundles<V> - 1U;
                        store_bundles(segment_distsq(pairs[i], pairs[j], eps), out[i].lane, out[j].lane);
                    }
                });
}

u32 hull_support(const f32* vx, const f32* vy, const f32* vz, usize padded, f32 dx, f32 dy, f32 dz) noexcept
{
    // From -inf, as the linear scan starts: vertex 0 stays chosen when every projection is -inf or NaN.
    constexpr u32 negative_infinity = 0xFF800000U;
    f32 best_proj = 0.0F;
    std::memcpy(&best_proj, &negative_infinity, sizeof(best_proj));
    u32 best_idx = 0U;
    // The projections of one step's vertices (sixteen at avx512 while they last, then eight), scanned in index order:
    // strictly greater wins, so an exact tie keeps the lower index; the padded lanes repeat vertex 0 and lose.
    const auto scan = [&]<typename V>(usize base)
    {
        constexpr usize count = sizeof(V) / sizeof(f32);
        const V proj = V::load(vx + base) * V(dx) + V::load(vy + base) * V(dy) + V::load(vz + base) * V(dz);
        alignas(64) f32 proj_lanes[count];
        proj.store(proj_lanes);
        for (u32 lane = 0; lane < count; ++lane)
        {
            if (proj_lanes[lane] > best_proj)
            {
                best_proj = proj_lanes[lane];
                best_idx = static_cast<u32>(base) + lane;
            }
        }
    };
    usize base = 0;
#if CRD_SIMD_HAS_AVX512
    for (; base + 2U * kBatchLanes <= padded; base += 2U * kBatchLanes)
    {
        scan.template operator()<crd::math::simd::Vec16f>(base);
    }
#endif
    for (; base < padded; base += kBatchLanes)
    {
        scan.template operator()<Vec8f>(base);
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
