// The geometry-mesh kernels of crd/geometry/mesh/mesh_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS). The raycast
// walks the BVH with the scalar robust slab test (detail/slab_core.hpp) and runs Moller-Trumbore on eight leaf
// triangles a step on the level's Vec8f (detail/batch8_core.hpp), sixteen on Vec16f at avx512 while more than eight
// are left, every product and sum rounded on its own; the lane decisions are scalar, in triangle order. Every level
// gives the portable bits.

#include <crd/core/assert.hpp>
#include <crd/geometry/mesh/mesh_kernels.hpp>
#include <crd/geometry/primitives/detail/batch8_core.hpp>
#include <crd/geometry/primitives/detail/slab_core.hpp>
#include <crd/math/simd/vec8f.hpp>
#if CRD_SIMD_HAS_AVX512
#include <crd/math/simd/vec16f.hpp>
#endif

#include <cstring>

namespace crd::geometry::mesh::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::f32;
using crd::u16;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::kBatchLanes;
using crd::geometry::primitives::RaySlab;
using crd::math::simd::Vec8f;

// The determinant a hit needs, in magnitude (with cull_back: as a positive value).
constexpr f32 kMtDetEps = 1.0e-7F;
constexpr u32 kNoTriangle = 0xFFFFFFFFU;

struct Node
{
    f32 lo[3];
    f32 hi[3];
    u32 left_first;
    u16 prim_count;
};

Node load_node(const unsigned char* nodes, u32 index) noexcept
{
    const unsigned char* p = nodes + static_cast<usize>(index) * kBvhNodeBytes;
    Node n{};
    std::memcpy(n.lo, p, sizeof(n.lo));
    std::memcpy(n.hi, p + sizeof(n.lo), sizeof(n.hi));
    std::memcpy(&n.left_first, p + kBvhNodeLeftFirst, sizeof(n.left_first));
    std::memcpy(&n.prim_count, p + kBvhNodePrimCount, sizeof(n.prim_count));
    return n;
}

// The Lanes triangles from prim[0, valid) as columns; the lanes past valid repeat lane 0.
template <u32 Lanes> struct Columns
{
    alignas(64) f32 c[9][Lanes]; // v0 x y z, v1 x y z, v2 x y z
};

template <u32 Lanes> void gather(const MeshBvhView& mesh, const u32* prim, u32 valid, Columns<Lanes>& out) noexcept
{
    for (u32 lane = 0; lane < Lanes; ++lane)
    {
        const u32 tri = prim[lane < valid ? lane : 0U];
        for (u32 corner = 0; corner < 3U; ++corner)
        {
            const u32 vertex = mesh.indices[static_cast<usize>(tri) * 3U + corner];
            f32 xyz[3];
            std::memcpy(xyz, mesh.vertices + static_cast<usize>(vertex) * kVertexBytes, sizeof(xyz));
            for (u32 axis = 0; axis < 3U; ++axis)
            {
                out.c[corner * 3U + axis][lane] = xyz[axis];
            }
        }
    }
}

struct Walk
{
    const MeshBvhView& mesh;
    const RaySlab& ray;
    bool neg[3];
    bool cull_back;
    f32 best_t;
    u32 best_tri;
    f32 best_bary[3];

    bool slab(const Node& n, f32& enter) const noexcept
    {
        return crd::geometry::primitives::detail::ray_aabb_slab(ray.origin, ray.inv_dir, neg, n.lo, n.hi, ray.pad, 0.0F,
                                                                best_t, enter);
    }

    // The leaf's triangles against the best hit so far: sixteen a step at avx512 while more than eight are left, then
    // eight.
    void leaf(const Node& n) noexcept
    {
        const u32 end = n.left_first + n.prim_count;
        u32 first = n.left_first;
#if CRD_SIMD_HAS_AVX512
        for (; first + kBatchLanes < end; first += 2U * kBatchLanes)
        {
            triangles<crd::math::simd::Vec16f>(first, end);
        }
#endif
        for (; first < end; first += kBatchLanes)
        {
            triangles<Vec8f>(first, end);
        }
    }

    // One register of the triangles from first (fewer before end).
    template <typename V> void triangles(u32 first, u32 end) noexcept
    {
        constexpr u32 lanes = static_cast<u32>(sizeof(V) / sizeof(f32));
        const u32 valid = end - first < lanes ? end - first : lanes;
        Columns<lanes> cols;
        gather(mesh, mesh.prim_indices + first, valid, cols);
        const crd::geometry::primitives::detail::Mt<V> r = crd::geometry::primitives::detail::moller_trumbore<V>(
            ray.origin, ray.direction, V::load(cols.c[0]), V::load(cols.c[1]), V::load(cols.c[2]), V::load(cols.c[3]),
            V::load(cols.c[4]), V::load(cols.c[5]), V::load(cols.c[6]), V::load(cols.c[7]), V::load(cols.c[8]));
        alignas(64) f32 det[lanes];
        alignas(64) f32 u[lanes];
        alignas(64) f32 v[lanes];
        alignas(64) f32 t[lanes];
        r.det.store(det);
        r.u.store(u);
        r.v.store(v);
        r.t.store(t);
        for (u32 lane = 0; lane < valid; ++lane)
        {
            take(det[lane], u[lane], v[lane], t[lane], mesh.prim_indices[first + lane]);
        }
    }

    // One lane's hit, kept when it is nearer, or as near with a lower triangle index.
    void take(f32 d, f32 u, f32 v, f32 t, u32 tri) noexcept
    {
        const bool flat = cull_back ? d <= kMtDetEps : (d > -kMtDetEps && d < kMtDetEps);
        if (flat || u < 0.0F || u > 1.0F || v < 0.0F || (u + v) > 1.0F || t < 0.0F || t > best_t)
        {
            return;
        }
        if (t < best_t || (t == best_t && tri < best_tri))
        {
            best_t = t;
            best_tri = tri;
            best_bary[0] = 1.0F - u - v;
            best_bary[1] = u;
            best_bary[2] = v;
        }
    }
};

bool raycast_mt8(const MeshBvhView& mesh, const RaySlab& ray, f32 tmax, bool cull_back, MeshRayHitData& hit) noexcept
{
    Walk w{mesh, ray, {ray.sign[0] != 0U, ray.sign[1] != 0U, ray.sign[2] != 0U}, cull_back, tmax, kNoTriangle, {}};

    struct Frame
    {
        u32 node;
        f32 enter_t;
    };
    Frame stack[kBvhStackDepth];
    usize sp = 0;
    f32 root_t = 0.0F;
    if (!w.slab(load_node(mesh.nodes, mesh.root), root_t))
    {
        return false;
    }
    stack[sp++] = Frame{mesh.root, root_t};
    while (sp > 0U)
    {
        const Frame f = stack[--sp];
        if (f.enter_t >= w.best_t)
        {
            continue;
        }
        const Node n = load_node(mesh.nodes, f.node);
        if (n.prim_count != 0U)
        {
            w.leaf(n);
            continue;
        }
        // Both children against the best hit so far; the nearer one is walked first.
        const u32 left = n.left_first;
        const u32 right = n.left_first + 1U;
        f32 tl = 0.0F;
        f32 tr = 0.0F;
        const bool lhit = w.slab(load_node(mesh.nodes, left), tl);
        const bool rhit = w.slab(load_node(mesh.nodes, right), tr);
        CRD_ASSERT(sp + 2U <= kBvhStackDepth);
        if (lhit && rhit)
        {
            const bool left_first = tl <= tr;
            stack[sp++] = left_first ? Frame{right, tr} : Frame{left, tl};
            stack[sp++] = left_first ? Frame{left, tl} : Frame{right, tr};
        }
        else if (lhit)
        {
            stack[sp++] = Frame{left, tl};
        }
        else if (rhit)
        {
            stack[sp++] = Frame{right, tr};
        }
    }
    if (w.best_tri == kNoTriangle)
    {
        return false;
    }
    hit.t = w.best_t;
    hit.triangle = w.best_tri;
    hit.bary[0] = w.best_bary[0];
    hit.bary[1] = w.best_bary[1];
    hit.bary[2] = w.best_bary[2];
    return true;
}

} // namespace

} // namespace crd::geometry::mesh::inline CRD_MATH_LEVEL_NS

namespace crd::geometry::mesh
{

const MeshKernels CRD_KERNEL_TABLE(kMeshKernels) = {
    CRD_MATH_LEVEL_NS::raycast_mt8,
};

} // namespace crd::geometry::mesh
