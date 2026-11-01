// The geometry-primitives kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the
// process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/geometry/primitives/primitive_dispatch.hpp>

#include <bit>
#include <limits>

namespace crd::geometry::primitives
{
namespace
{

using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::usize;

// CRD_KERNELS_PRIMITIVES: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_PRIMITIVES & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_PRIMITIVES, PORTABLE), "the portable level is always built");

constexpr KernelLevel<PrimitiveKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kPrimitiveKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_PRIMITIVES, SSE2)
    {crd::CpuLevel::Sse2, &kPrimitiveKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_PRIMITIVES, AVX2)
    {crd::CpuLevel::Avx2, &kPrimitiveKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_PRIMITIVES, AVX512)
    {crd::CpuLevel::Avx512, &kPrimitiveKernelsAvx512, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// Each batch routine runs over three bundles (an avx512 pair, then the odd last bundle on Vec8f) and the hull scan over
// 37 vertices (two sixteen-lane steps and an eight-lane tail at avx512). The inputs are small dyadic values, so every
// product, sum and quotient the kernels form is exact; each expected result is a closed form of the lane's index g,
// evaluated at compile time, never the kernels' own arithmetic.
constexpr usize kCheckBundles = 3;
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr u32 kAll = 0xFFFFFFFFU;

constexpr f32 lane_index(usize bundle, usize lane) noexcept
{
    return static_cast<f32>(bundle * kBatchLanes + lane);
}

constexpr f32 clamp01(f32 x) noexcept
{
    if (x < 0.0F)
    {
        return 0.0F;
    }
    return x > 1.0F ? 1.0F : x;
}

// The ray (0, 0, 0) + t (1, -1/2, 1/4) against box g: x in [g, g + 2]; even lanes take y in [-20, 1] and so enter at
// g and leave at g + 2, odd lanes y in [2, 3], which the ray never reaches. With t0 = 2.5 and t1 = 20 the even lanes
// from 2 to 20 hit, every lane entering at max(g, 2.5).
struct SlabCheck
{
    Aabb8 boxes[kCheckBundles];
    RayAabb8Result want[kCheckBundles];
};

constexpr SlabCheck make_slab_check() noexcept
{
    SlabCheck c{};
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        for (usize l = 0; l < kBatchLanes; ++l)
        {
            const f32 g = lane_index(b, l);
            const bool odd = (l % 2U) != 0U;
            c.boxes[b].min_x[l] = g;
            c.boxes[b].max_x[l] = g + 2.0F;
            c.boxes[b].min_y[l] = odd ? 2.0F : -20.0F;
            c.boxes[b].max_y[l] = odd ? 3.0F : 1.0F;
            c.boxes[b].min_z[l] = -1.0F;
            c.boxes[b].max_z[l] = 8.0F;
            c.want[b].t_enter[l] = g > 2.5F ? g : 2.5F;
            c.want[b].hit_mask[l] = !odd && g >= 2.0F && g <= 20.0F ? kAll : 0U;
        }
    }
    return c;
}

constexpr RaySlab kSlabRay{{0.0F, 0.0F, 0.0F}, {1.0F, -0.5F, 0.25F}, {1.0F, -2.0F, 4.0F}, {0U, 1U, 0U}, 1.0F};

// Eight rays along x at height y = lane (even lanes from x = 0 forward, odd lanes from x = 8 backward) against box i:
// x in [i + 1, i + 3], y in [-1/2, 7/2]. The zero direction components give infinite inverses, so lanes 0 to 3 hit,
// entering at i + 1 (even) or 5 - i (odd), and lanes 4 to 7 miss.
struct PacketCheck
{
    RayPacket8 packet;
    Box3 boxes[kCheckBundles];
    RayAabb8Result want[kCheckBundles];
};

constexpr PacketCheck make_packet_check() noexcept
{
    PacketCheck c{};
    for (usize l = 0; l < kBatchLanes; ++l)
    {
        const bool odd = (l % 2U) != 0U;
        c.packet.origin_x[l] = odd ? 8.0F : 0.0F;
        c.packet.origin_y[l] = static_cast<f32>(l);
        c.packet.origin_z[l] = 0.0F;
        c.packet.inv_dir_x[l] = odd ? -1.0F : 1.0F;
        c.packet.inv_dir_y[l] = kInf;
        c.packet.inv_dir_z[l] = kInf;
    }
    for (usize i = 0; i < kCheckBundles; ++i)
    {
        const f32 fi = static_cast<f32>(i);
        c.boxes[i] = Box3{{fi + 1.0F, -0.5F, -1.0F}, {fi + 3.0F, 3.5F, 1.0F}};
        for (usize l = 0; l < kBatchLanes; ++l)
        {
            c.want[i].t_enter[l] = (l % 2U) != 0U ? 5.0F - fi : fi + 1.0F;
            c.want[i].hit_mask[l] = l <= 3U ? kAll : 0U;
        }
    }
    return c;
}

// The ray (0, 0, -4) + t (0, 0, 1) against the triangle with legs of 4 from a = (-1, -1, g/2) (lane 7: a = (1, 1,
// g/2)), wound clockwise to the ray on even lanes and counter-clockwise on odd ones: t = 4 + g/2, u = v = 1/4 (lane 7:
// -1/4). Culling back faces with tnear = 5, the odd lanes but 7 from g = 3 hit.
struct TriangleCheck
{
    Triangle38 tris[kCheckBundles];
    RayTri8Result want[kCheckBundles];
};

constexpr TriangleCheck make_triangle_check() noexcept
{
    TriangleCheck c{};
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        for (usize l = 0; l < kBatchLanes; ++l)
        {
            const f32 g = lane_index(b, l);
            const bool odd = (l % 2U) != 0U;
            const f32 a = l == 7U ? 1.0F : -1.0F;
            const f32 z = g * 0.5F;
            Triangle38& t = c.tris[b];
            t.ax[l] = a;
            t.ay[l] = a;
            t.az[l] = z;
            t.bx[l] = odd ? a : a + 4.0F;
            t.by[l] = odd ? a + 4.0F : a;
            t.bz[l] = z;
            t.cx[l] = odd ? a + 4.0F : a;
            t.cy[l] = odd ? a : a + 4.0F;
            t.cz[l] = z;
            c.want[b].t[l] = 4.0F + z;
            c.want[b].u[l] = l == 7U ? -0.25F : 0.25F;
            c.want[b].v[l] = l == 7U ? -0.25F : 0.25F;
            c.want[b].hit_mask[l] = odd && l != 7U && g >= 2.0F ? kAll : 0U;
        }
    }
    return c;
}

constexpr RaySlab kTriangleRay{{0.0F, 0.0F, -4.0F}, {0.0F, 0.0F, 1.0F}, {kInf, kInf, 1.0F}, {0U, 0U, 0U}, 1.0F};

// Box g, x in [g/2 - 6, g/2 - 4] (lane 3: y in [5, 6]), against [0, 4] on every axis: closed intervals, so lanes from
// g = 8 to 20 but lane 3 overlap.
struct OverlapCheck
{
    Aabb8 boxes[kCheckBundles];
    Mask8 want[kCheckBundles];
};

constexpr OverlapCheck make_overlap_check() noexcept
{
    OverlapCheck c{};
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        for (usize l = 0; l < kBatchLanes; ++l)
        {
            const f32 g = lane_index(b, l);
            c.boxes[b].min_x[l] = g * 0.5F - 6.0F;
            c.boxes[b].max_x[l] = g * 0.5F - 4.0F;
            c.boxes[b].min_y[l] = l == 3U ? 5.0F : -1.0F;
            c.boxes[b].max_y[l] = l == 3U ? 6.0F : 1.0F;
            c.boxes[b].min_z[l] = -1.0F;
            c.boxes[b].max_z[l] = 1.0F;
            c.want[b].lane[l] = g >= 8.0F && g <= 20.0F && l != 3U ? kAll : 0U;
        }
    }
    return c;
}

constexpr Box3 kOverlapBox{{0.0F, 0.0F, 0.0F}, {4.0F, 4.0F, 4.0F}};

// Unit spheres at (g/4, 0, 0) (lane 5: (g/4, 3, 0)) against the unit sphere at the origin: lanes to g = 8, which
// touches, overlap but lane 5.
struct SphereCheck
{
    Sphere8 spheres[kCheckBundles];
    Mask8 want[kCheckBundles];
};

constexpr SphereCheck make_sphere_check() noexcept
{
    SphereCheck c{};
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        for (usize l = 0; l < kBatchLanes; ++l)
        {
            const f32 g = lane_index(b, l);
            c.spheres[b].center_x[l] = g * 0.25F;
            c.spheres[b].center_y[l] = l == 5U ? 3.0F : 0.0F;
            c.spheres[b].center_z[l] = 0.0F;
            c.spheres[b].radius[l] = 1.0F;
            c.want[b].lane[l] = g <= 8.0F && l != 5U ? kAll : 0U;
        }
    }
    return c;
}

// Segment 1 from the origin to (4, 0, 0); segment 2 at x0 = g/2 - 2, y = 2, from z = h = lane % 4 - 5 to h + 4 (lane
// 6: the point (x0, 2, h)). With tiny = 0 the closest parameters are s = clamp(x0 / 4) and t = clamp(-h / 4) (lane 6:
// 0), so the squared distance is (4 s - x0)^2 + 4 + (h + 4 t)^2.
struct SegmentCheck
{
    Segment38Pair pairs[kCheckBundles];
    Lanes8 want[kCheckBundles];
};

constexpr SegmentCheck make_segment_check() noexcept
{
    SegmentCheck c{};
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        for (usize l = 0; l < kBatchLanes; ++l)
        {
            const f32 x0 = lane_index(b, l) * 0.5F - 2.0F;
            const f32 h = static_cast<f32>(l % 4U) - 5.0F;
            Segment38Pair& p = c.pairs[b];
            p.p1x[l] = 0.0F;
            p.p1y[l] = 0.0F;
            p.p1z[l] = 0.0F;
            p.q1x[l] = 4.0F;
            p.q1y[l] = 0.0F;
            p.q1z[l] = 0.0F;
            p.p2x[l] = x0;
            p.p2y[l] = 2.0F;
            p.p2z[l] = h;
            p.q2x[l] = x0;
            p.q2y[l] = 2.0F;
            p.q2z[l] = l == 6U ? h : h + 4.0F;
            const f32 s = clamp01(x0 / 4.0F);
            const f32 t = l == 6U ? 0.0F : clamp01(-h / 4.0F);
            const f32 gx = 4.0F * s - x0;
            const f32 gz = h + 4.0F * t;
            c.want[b].lane[l] = gx * gx + 4.0F + gz * gz;
        }
    }
    return c;
}

// 37 vertices padded to 40 with vertex 0: grid points in [-8, 8]^3, but vertices 21 and 33 at (9, 9, -9) and vertex
// 36 at (-9, 0, 0). Along (1, 2, -3) the two equal maxima give the lower index, 21; along (-1, 0, 0) vertex 36.
constexpr usize kHullVertices = 37;
constexpr usize kHullPadded = 40;

struct HullCheck
{
    f32 x[kHullPadded];
    f32 y[kHullPadded];
    f32 z[kHullPadded];
};

constexpr HullCheck make_hull_check() noexcept
{
    HullCheck c{};
    for (usize k = 0; k < kHullPadded; ++k)
    {
        const usize v = k < kHullVertices ? k : 0U;
        c.x[k] = static_cast<f32>((7U * v) % 17U) - 8.0F;
        c.y[k] = static_cast<f32>((11U * v) % 17U) - 8.0F;
        c.z[k] = static_cast<f32>((5U * v) % 17U) - 8.0F;
        if (v == 21U || v == 33U)
        {
            c.x[k] = 9.0F;
            c.y[k] = 9.0F;
            c.z[k] = -9.0F;
        }
        else if (v == 36U)
        {
            c.x[k] = -9.0F;
            c.y[k] = 0.0F;
            c.z[k] = 0.0F;
        }
    }
    return c;
}

constexpr SlabCheck kSlab = make_slab_check();
constexpr PacketCheck kPacket = make_packet_check();
constexpr TriangleCheck kTriangle = make_triangle_check();
constexpr OverlapCheck kOverlap = make_overlap_check();
constexpr SphereCheck kSphere = make_sphere_check();
constexpr SegmentCheck kSegment = make_segment_check();
constexpr HullCheck kHull = make_hull_check();

bool same(const f32 (&got)[kBatchLanes], const f32 (&want)[kBatchLanes]) noexcept
{
    for (usize l = 0; l < kBatchLanes; ++l)
    {
        if (std::bit_cast<u32>(got[l]) != std::bit_cast<u32>(want[l]))
        {
            return false;
        }
    }
    return true;
}

bool same(const u32 (&got)[kBatchLanes], const u32 (&want)[kBatchLanes]) noexcept
{
    for (usize l = 0; l < kBatchLanes; ++l)
    {
        if (got[l] != want[l])
        {
            return false;
        }
    }
    return true;
}

bool same_slabs(const RayAabb8Result (&got)[kCheckBundles], const RayAabb8Result (&want)[kCheckBundles]) noexcept
{
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        if (!same(got[b].t_enter, want[b].t_enter) || !same(got[b].hit_mask, want[b].hit_mask))
        {
            return false;
        }
    }
    return true;
}

bool same_masks(const Mask8 (&got)[kCheckBundles], const Mask8 (&want)[kCheckBundles]) noexcept
{
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        if (!same(got[b].lane, want[b].lane))
        {
            return false;
        }
    }
    return true;
}

void resolve_primitives() noexcept
{
    (void)primitive_kernels();
}

constinit crd::KernelFamily g_family{"geometry.primitives", &resolve_primitives, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* primitive_self_check(const PrimitiveKernels& table) noexcept
{
    RayAabb8Result slabs[kCheckBundles];
    table.ray_vs_aabb8(kSlabRay, kSlab.boxes, kCheckBundles, 2.5F, 20.0F, slabs);
    if (!same_slabs(slabs, kSlab.want))
    {
        return "ray_vs_aabb8";
    }
    table.packet8_vs_aabb(kPacket.packet, kPacket.boxes, kCheckBundles, 1.0F, 0.0F, 100.0F, slabs);
    if (!same_slabs(slabs, kPacket.want))
    {
        return "packet8_vs_aabb";
    }
    RayTri8Result tris[kCheckBundles];
    table.ray_vs_triangle8(kTriangleRay, kTriangle.tris, kCheckBundles, true, 5.0F, 1.0e-6F, tris);
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        const RayTri8Result& want = kTriangle.want[b];
        if (!same(tris[b].t, want.t) || !same(tris[b].u, want.u) || !same(tris[b].v, want.v) ||
            !same(tris[b].hit_mask, want.hit_mask))
        {
            return "ray_vs_triangle8";
        }
    }
    Mask8 masks[kCheckBundles];
    table.aabb8_vs_aabb(kOverlap.boxes, kCheckBundles, kOverlapBox, masks);
    if (!same_masks(masks, kOverlap.want))
    {
        return "aabb8_vs_aabb";
    }
    constexpr f32 origin[3] = {0.0F, 0.0F, 0.0F};
    table.sphere8_vs_sphere(kSphere.spheres, kCheckBundles, origin, 1.0F, masks);
    if (!same_masks(masks, kSphere.want))
    {
        return "sphere8_vs_sphere";
    }
    Lanes8 distances[kCheckBundles];
    table.segment8_distsq(kSegment.pairs, kCheckBundles, 0.0F, distances);
    for (usize b = 0; b < kCheckBundles; ++b)
    {
        if (!same(distances[b].lane, kSegment.want[b].lane))
        {
            return "segment8_distsq";
        }
    }
    if (table.hull_support(kHull.x, kHull.y, kHull.z, kHullPadded, 1.0F, 2.0F, -3.0F) != 21U ||
        table.hull_support(kHull.x, kHull.y, kHull.z, kHullPadded, -1.0F, 0.0F, 0.0F) != 36U)
    {
        return "hull_support";
    }
    return nullptr;
}

std::span<const KernelLevel<PrimitiveKernels>> primitive_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<PrimitiveKernels>& primitive_kernels() noexcept
{
    static const KernelLevel<PrimitiveKernels>& selected =
        crd::process_checked_kernel_level<PrimitiveKernels>(kLevels, &primitive_self_check, g_family);
    return selected;
}

RaySlab ray_slab(const Ray3<crd::f32>& ray, const RayAABBPrecompute<crd::f32>& pre) noexcept
{
    RaySlab s{};
    s.origin[0] = ray.origin.x;
    s.origin[1] = ray.origin.y;
    s.origin[2] = ray.origin.z;
    s.direction[0] = ray.direction.x;
    s.direction[1] = ray.direction.y;
    s.direction[2] = ray.direction.z;
    s.inv_dir[0] = pre.inv_dir.x;
    s.inv_dir[1] = pre.inv_dir.y;
    s.inv_dir[2] = pre.inv_dir.z;
    for (crd::usize axis = 0; axis < 3U; ++axis)
    {
        s.sign[axis] = pre.sign[axis] != 0U ? 1U : 0U;
    }
    s.pad = ray_aabb_robust_pad<crd::f32>();
    return s;
}

} // namespace crd::geometry::primitives
