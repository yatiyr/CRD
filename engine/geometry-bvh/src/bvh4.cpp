#include "aabb_ops.hpp"

#include <crd/core/assert.hpp>
#include <crd/geometry/bvh/bvh4.hpp>
#include <crd/geometry/bvh/bvh4_simd.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/math/vec.hpp>

#include <limits>

namespace crd::geometry::bvh
{
namespace
{
using crd::f32;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::math::Vec3;
using crd::math::simd::Vec4f;
using detail::aabb_empty;
using detail::aabb_half_area;
using detail::aabb_merge;

// A pending bvh4 node: the binary interior node it represents, and the bvh4
// slot to fill.
struct CollapseJob
{
    u32 binary_node;
    u32 bvh4_node;
};

} // namespace

AABB3<f32> Bvh4Tree::bounds() const noexcept
{
    if (is_empty())
    {
        constexpr f32 inf = std::numeric_limits<f32>::infinity();
        return AABB3<f32>(Vec3<f32>(inf, inf, inf), Vec3<f32>(-inf, -inf, -inf));
    }
    return m_nodes[m_root].bounds;
}

Bvh4Tree bvh4_collapse(const BvhTree& binary, crd::memory::IAllocator* alloc)
{
    CRD_ASSERT(alloc != nullptr);
    Bvh4Tree out(alloc);
    if (binary.is_empty())
    {
        return out;
    }
    CRD_ASSERT(crd::geometry::primitives::is_finite(binary.bounds())); // NaN/Inf contract — ADR-0076 §15

    // The Bvh4Tree is self-contained re: leaf order — copy the permutation in.
    const crd::containers::ConstSpan<u32> src_prims = binary.prim_indices();
    crd::containers::Array<u32>& dst_prims = out.prim_indices_mut();
    dst_prims.resize(src_prims.size());
    for (usize i = 0; i < src_prims.size(); ++i)
    {
        dst_prims[i] = src_prims[i];
    }

    const crd::containers::ConstSpan<BvhNode> bnodes = binary.nodes();
    crd::containers::Array<Bvh4Node>& nodes = out.nodes_mut();
    nodes.reserve(binary.node_count()); // ≥ the number of bvh4 nodes; no realloc during the collapse

    // Single-leaf binary tree ⇒ one bvh4 node with one (leaf) child.
    if (bnodes[binary.root()].prim_count != 0)
    {
        Bvh4Node n{};
        n.child_count = 1;
        n.children[0].bounds = bnodes[binary.root()].bounds;
        n.children[0].first = bnodes[binary.root()].left_first;
        n.children[0].count = bnodes[binary.root()].prim_count;
        n.bounds = n.children[0].bounds;
        nodes.push_back(n);
        out.set_root(0);
        return out;
    }

    nodes.push_back(Bvh4Node{}); // bvh4 node 0 ↔ binary root
    out.set_root(0);
    crd::containers::Array<CollapseJob> jobs(alloc);
    jobs.push_back(CollapseJob{binary.root(), 0});

    while (jobs.size() > 0)
    {
        const CollapseJob job = jobs[jobs.size() - 1];
        jobs.resize(jobs.size() - 1);

        // Worklist: the binary children of `job.binary_node`, then "open" the
        // largest interior member repeatedly until 4 children or no opener.
        u32 wl[4];
        u8 wc = 0;
        wl[wc++] = bnodes[job.binary_node].left_first;      // left child
        wl[wc++] = bnodes[job.binary_node].left_first + 1U; // right child
        while (wc < 4)
        {
            bool has_best = false;
            u8 best_k = 0;
            for (u8 k = 0; k < wc; ++k)
            {
                if (bnodes[wl[k]].prim_count != 0)
                {
                    continue; // leaf — cannot be opened
                }
                if (!has_best)
                {
                    has_best = true;
                    best_k = k;
                    continue;
                }
                const f32 ha_k = aabb_half_area(bnodes[wl[k]].bounds);
                const f32 ha_b = aabb_half_area(bnodes[wl[best_k]].bounds);
                if (ha_k > ha_b || (ha_k == ha_b && wl[k] < wl[best_k]))
                {
                    best_k = k;
                }
            }
            if (!has_best)
            {
                break; // all worklist members are leaves
            }
            const u32 opened = wl[best_k];
            wl[best_k] = bnodes[opened].left_first;
            wl[wc++] = bnodes[opened].left_first + 1U; // wc was < 4 ⇒ now ≤ 4
        }

        // Compute the children, allocating + queueing the node-children first so
        // no reference into `nodes` is held across a push_back (the reserve makes
        // the pushes realloc-free anyway — belt and braces).
        Bvh4Child kids[4]{};
        AABB3<f32> ub = aabb_empty();
        for (u8 k = 0; k < wc; ++k)
        {
            const BvhNode& wn = bnodes[wl[k]];
            kids[k].bounds = wn.bounds;
            aabb_merge(ub, wn.bounds);
            if (wn.prim_count != 0) // leaf child
            {
                kids[k].first = wn.left_first;
                kids[k].count = wn.prim_count;
            }
            else // node child — allocate a bvh4 node and queue it
            {
                const u32 nn = static_cast<u32>(nodes.size());
                CRD_ASSERT(nn < binary.node_count()); // reserve guarantee — no realloc
                nodes.push_back(Bvh4Node{});
                kids[k].first = nn;
                kids[k].count = 0;
                jobs.push_back(CollapseJob{wl[k], nn});
            }
        }
        Bvh4Node& dst = nodes[job.bvh4_node];
        dst.child_count = wc;
        dst.bounds = ub;
        for (u8 k = 0; k < wc; ++k)
        {
            dst.children[k] = kids[k];
        }
    }
    return out;
}

std::optional<BvhRayHit> bvh4_raycast(const Bvh4Tree& tree, crd::containers::ConstSpan<AABB3<f32>> prims,
                                      const Ray3<f32>& ray, f32 tmax)
{
    if (tree.is_empty())
    {
        return std::nullopt;
    }
    const crd::containers::ConstSpan<Bvh4Node> nodes = tree.nodes();
    const crd::containers::ConstSpan<u32> prim_idx = tree.prim_indices();
    const crd::geometry::primitives::RayAABBPrecompute<f32> pre = crd::geometry::primitives::precompute_ray_aabb(ray);

    u32 stack[k_max_bvh4_stack];
    usize sp = 0;
    stack[sp++] = tree.root();
    f32 best_t = tmax;
    u32 best_prim = 0;
    bool hit = false;
    while (sp > 0)
    {
        const Bvh4Node& node = nodes[stack[--sp]];

        // Transpose the ≤4 children's bounds into SoA Vec4f columns (unused
        // lanes duplicate child 0 — harmless, the loop below only reads lanes
        // [0, child_count)), then one Vec4f ray-vs-4-AABB instead of four scalar
        // slab tests. The result is bit-identical (finite/well-formed inputs) to
        // calling `intersect_ray_aabb_robust` per child.
        f32 minx[4];
        f32 miny[4];
        f32 minz[4];
        f32 maxx[4];
        f32 maxy[4];
        f32 maxz[4];
        for (u8 c = 0; c < 4U; ++c)
        {
            const Bvh4Child& src = (c < node.child_count) ? node.children[c] : node.children[0];
            minx[c] = src.bounds.min.x;
            miny[c] = src.bounds.min.y;
            minz[c] = src.bounds.min.z;
            maxx[c] = src.bounds.max.x;
            maxy[c] = src.bounds.max.y;
            maxz[c] = src.bounds.max.z;
        }
        const Ray4AabbResult r = ray_vs_4_aabb(ray, pre, Vec4f::load(minx), Vec4f::load(miny), Vec4f::load(minz),
                                               Vec4f::load(maxx), Vec4f::load(maxy), Vec4f::load(maxz), 0.0F, best_t);
        f32 hit4[4];
        f32 t4[4];
        r.hit_mask.store(hit4);
        r.t_enter.store(t4);

        for (u8 c = 0; c < node.child_count; ++c)
        {
            // Skip a missed lane, or one entered no earlier than the current
            // best (its box — hence every prim/subtree below it — can't beat it).
            if (hit4[c] == 0.0F || t4[c] >= best_t)
            {
                continue;
            }
            const Bvh4Child& ch = node.children[c];
            if (ch.is_leaf())
            {
                for (u32 i = ch.first; i < ch.first + ch.count; ++i)
                {
                    const u32 p = prim_idx[i];
                    f32 t = 0.0F;
                    if (crd::geometry::primitives::intersect_ray_aabb_robust(ray, pre, prims[p], 0.0F, best_t, t) &&
                        t < best_t)
                    {
                        best_t = t;
                        best_prim = p;
                        hit = true;
                    }
                }
            }
            else
            {
                CRD_ASSERT(sp + 1 <= k_max_bvh4_stack);
                stack[sp++] = ch.first;
            }
        }
    }
    if (!hit)
    {
        return std::nullopt;
    }
    return BvhRayHit{best_prim, best_t};
}

void bvh4_overlap(const Bvh4Tree& tree, crd::containers::ConstSpan<AABB3<f32>> prims, const AABB3<f32>& box,
                  crd::containers::Array<u32>& out)
{
    bvh4_overlap(tree, prims, box, [&out](u32 p) { out.push_back(p); });
}

} // namespace crd::geometry::bvh
