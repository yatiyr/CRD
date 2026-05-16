// crd-geometry-mesh — per-leaf SIMD Möller-Trumbore raycast (v4d).

#include <crd/geometry/mesh/mesh_raycast_simd.hpp>

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <limits>

namespace crd::geometry::mesh
{

using crd::geometry::bvh::BvhNode;
using crd::geometry::bvh::k_max_bvh_depth;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::intersect_ray_aabb_robust;
using crd::geometry::primitives::precompute_ray_aabb;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::RayAABBPrecompute;
using crd::math::Vec3;
using crd::math::simd::cmp_gt;
using crd::math::simd::cmp_lt;
using crd::math::simd::Vec8f;

namespace
{

// Per-axis epsilon for the det test — same value v0f's MT uses.
constexpr crd::f32 k_mt_det_eps = 1.0e-7F;

// 8-way Möller-Trumbore — SIMD over the heavy ALU (edges/det/inv_det/u/v/t),
// then scalar lane scan applies the masking decisions (cull_back / |det|>eps /
// 0<=u<=1 / 0<=v / u+v<=1 / tnear<=t<=tmax). The SIMD-mask-AND pattern is
// dropped — combining `_CMP_*` mask registers via `min`/`max` is implementation-
// defined for NaN-encoded all-bits-set lanes, which made the original draft
// silently lose the cull bit. Lane scan is 8 branches, trivial cost vs the ALU.
//
// On return: 4 Vec8f registers holding per-lane (det, u, v, t). The caller
// scans them in scalar.
struct MtResult
{
    Vec8f det;
    Vec8f u;
    Vec8f v;
    Vec8f t;
};

[[nodiscard]] inline MtResult mt8(const Vec3<crd::f32>& ro, const Vec3<crd::f32>& rd,
                                   Vec8f v0x, Vec8f v0y, Vec8f v0z,
                                   Vec8f v1x, Vec8f v1y, Vec8f v1z,
                                   Vec8f v2x, Vec8f v2y, Vec8f v2z) noexcept
{
    const Vec8f rox(ro.x), roy(ro.y), roz(ro.z);
    const Vec8f rdx(rd.x), rdy(rd.y), rdz(rd.z);

    // edge1 = v1 - v0
    const Vec8f e1x = v1x - v0x;
    const Vec8f e1y = v1y - v0y;
    const Vec8f e1z = v1z - v0z;
    // edge2 = v2 - v0
    const Vec8f e2x = v2x - v0x;
    const Vec8f e2y = v2y - v0y;
    const Vec8f e2z = v2z - v0z;
    // pvec = rd × edge2
    const Vec8f px = rdy * e2z - rdz * e2y;
    const Vec8f py = rdz * e2x - rdx * e2z;
    const Vec8f pz = rdx * e2y - rdy * e2x;
    // det = edge1 · pvec
    const Vec8f det = e1x * px + e1y * py + e1z * pz;
    const Vec8f inv_det = Vec8f(1.0F) / det;
    // tvec = ro - v0
    const Vec8f tvx = rox - v0x;
    const Vec8f tvy = roy - v0y;
    const Vec8f tvz = roz - v0z;
    // u = (tvec · pvec) * inv_det
    const Vec8f u = (tvx * px + tvy * py + tvz * pz) * inv_det;
    // qvec = tvec × edge1
    const Vec8f qx = tvy * e1z - tvz * e1y;
    const Vec8f qy = tvz * e1x - tvx * e1z;
    const Vec8f qz = tvx * e1y - tvy * e1x;
    // v = (rd · qvec) * inv_det
    const Vec8f v = (rdx * qx + rdy * qy + rdz * qz) * inv_det;
    // t = (edge2 · qvec) * inv_det
    const Vec8f t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;

    return MtResult{det, u, v, t};
}

// AoS gather → SoA columns for up to 8 triangles. Lanes beyond
// `valid_count` are replicated from lane 0 (deterministic padding).
struct TriColumns
{
    Vec8f v0x, v0y, v0z;
    Vec8f v1x, v1y, v1z;
    Vec8f v2x, v2y, v2z;
};

[[nodiscard]] inline TriColumns gather_tris(const TriangleMeshViewf& view,
                                             const crd::u32*           leaf_prim_idx,
                                             crd::u32                  valid_count) noexcept
{
    crd::f32 v0x[8]{}, v0y[8]{}, v0z[8]{};
    crd::f32 v1x[8]{}, v1y[8]{}, v1z[8]{};
    crd::f32 v2x[8]{}, v2y[8]{}, v2z[8]{};
    for (crd::u32 i = 0U; i < 8U; ++i)
    {
        const crd::u32 src_lane = i < valid_count ? i : 0U;
        const crd::u32 ti = leaf_prim_idx[src_lane];
        const crd::u32 i0 = view.indices[ti * 3U + 0U];
        const crd::u32 i1 = view.indices[ti * 3U + 1U];
        const crd::u32 i2 = view.indices[ti * 3U + 2U];
        v0x[i] = view.vertices[i0].x; v0y[i] = view.vertices[i0].y; v0z[i] = view.vertices[i0].z;
        v1x[i] = view.vertices[i1].x; v1y[i] = view.vertices[i1].y; v1z[i] = view.vertices[i1].z;
        v2x[i] = view.vertices[i2].x; v2y[i] = view.vertices[i2].y; v2z[i] = view.vertices[i2].z;
    }
    return TriColumns{
        Vec8f::load(v0x), Vec8f::load(v0y), Vec8f::load(v0z),
        Vec8f::load(v1x), Vec8f::load(v1y), Vec8f::load(v1z),
        Vec8f::load(v2x), Vec8f::load(v2y), Vec8f::load(v2z)};
}

} // namespace

std::optional<MeshRayHit>
mesh_raycast_simd(const TriangleMeshViewf& view,
                  const TriangleMeshBvh&    bvh,
                  const Ray3<crd::f32>&     ray,
                  crd::f32                  tmax,
                  bool                      cull_back) noexcept
{
    if (view.is_empty() || bvh.is_empty())
    {
        return std::nullopt;
    }

    const auto nodes    = bvh.tree.nodes();
    const auto prim_idx = bvh.tree.prim_indices();
    const RayAABBPrecompute<crd::f32> aabb_pc = precompute_ray_aabb(ray);

    crd::f32 best_t   = tmax;
    crd::u32 best_tri = 0xFFFFFFFFU;
    Vec3<crd::f32> best_bary{};

    struct Frame { crd::u32 node; crd::f32 enter_t; };
    Frame stack[k_max_bvh_depth];
    crd::usize sp = 0;

    {
        crd::f32 root_t = 0.0F;
        if (!intersect_ray_aabb_robust(ray, aabb_pc, nodes[bvh.tree.root()].bounds,
                                        0.0F, best_t, root_t))
        {
            return std::nullopt;
        }
        stack[sp++] = Frame{bvh.tree.root(), root_t};
    }

    while (sp > 0)
    {
        const Frame f = stack[--sp];
        if (f.enter_t >= best_t)
        {
            continue;
        }
        const BvhNode& node = nodes[f.node];

        if (node.is_leaf())
        {
            // Process the leaf's triangles in chunks of 8.
            const crd::u32 leaf_start = node.left_first;
            const crd::u32 leaf_end   = leaf_start + node.prim_count;
            for (crd::u32 chunk_start = leaf_start; chunk_start < leaf_end; chunk_start += 8U)
            {
                const crd::u32 remaining = leaf_end - chunk_start;
                const crd::u32 valid     = remaining < 8U ? remaining : 8U;
                const TriColumns cols = gather_tris(view, &prim_idx[chunk_start], valid);

                const MtResult r = mt8(ray.origin, ray.direction,
                                        cols.v0x, cols.v0y, cols.v0z,
                                        cols.v1x, cols.v1y, cols.v1z,
                                        cols.v2x, cols.v2y, cols.v2z);

                // Scalar lane scan applies the masking decisions. Each lane
                // is independent; branches are predictable on near-uniform
                // hit/miss patterns within a leaf.
                for (crd::u32 lane = 0U; lane < valid; ++lane)
                {
                    const crd::f32 d   = r.det.lane(lane);
                    if (cull_back)
                    {
                        if (d <= k_mt_det_eps) { continue; }
                    }
                    else
                    {
                        if (d > -k_mt_det_eps && d < k_mt_det_eps) { continue; }
                    }
                    const crd::f32 u_l = r.u.lane(lane);
                    if (u_l < 0.0F || u_l > 1.0F) { continue; }
                    const crd::f32 v_l = r.v.lane(lane);
                    if (v_l < 0.0F || (u_l + v_l) > 1.0F) { continue; }
                    const crd::f32 t   = r.t.lane(lane);
                    if (t < 0.0F || t > best_t) { continue; }

                    const crd::u32 ti = prim_idx[chunk_start + lane];
                    if (t < best_t)
                    {
                        best_t    = t;
                        best_tri  = ti;
                        best_bary = Vec3<crd::f32>{1.0F - u_l - v_l, u_l, v_l};
                    }
                    else if (t == best_t && ti < best_tri)
                    {
                        best_tri  = ti;
                        best_bary = Vec3<crd::f32>{1.0F - u_l - v_l, u_l, v_l};
                    }
                }
            }
            continue;
        }

        // Interior — slab-test both children, descend nearer-first.
        const crd::u32 left  = node.left_first;
        const crd::u32 right = node.left_first + 1U;
        crd::f32 ltl = 0.0F, ltr = 0.0F;
        const bool lhit = intersect_ray_aabb_robust(ray, aabb_pc, nodes[left].bounds,
                                                     0.0F, best_t, ltl);
        const bool rhit = intersect_ray_aabb_robust(ray, aabb_pc, nodes[right].bounds,
                                                     0.0F, best_t, ltr);
        CRD_ASSERT(sp + 2U <= k_max_bvh_depth);
        if (lhit && rhit)
        {
            if (ltl <= ltr)
            {
                stack[sp++] = Frame{right, ltr};
                stack[sp++] = Frame{left,  ltl};
            }
            else
            {
                stack[sp++] = Frame{left,  ltl};
                stack[sp++] = Frame{right, ltr};
            }
        }
        else if (lhit) { stack[sp++] = Frame{left,  ltl}; }
        else if (rhit) { stack[sp++] = Frame{right, ltr}; }
    }

    if (best_tri == 0xFFFFFFFFU)
    {
        return std::nullopt;
    }
    MeshRayHit out{};
    out.t       = best_t;
    out.payload = MeshHitPayload{best_tri, best_bary};
    return out;
}

} // namespace crd::geometry::mesh
