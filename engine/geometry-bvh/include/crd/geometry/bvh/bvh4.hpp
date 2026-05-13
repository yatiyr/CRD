#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — quad-BVH (4-wide) topology variant (v1d).
//
// `Bvh4Tree` is a 4-wide BVH: each interior node holds 2–4 children (leaf or
// node). It is produced by *collapsing* a built binary `BvhTree` —
// `bvh4_collapse(binary)` — bottom-up: each interior node "opens" the largest
// interior subtree among its children, repeatedly, until it has 4 children or
// can't widen without overshooting. Wider nodes ⇒ fewer node fetches per
// traversal step and a layout that the v1g `Vec4f` ray-vs-4-AABB kernel will
// fill in lockstep. v1d ships the *scalar* path (four sequential robust slab
// tests per node); v1g promotes BVH4 to the default and adds the SIMD kernel.
//
// The collapse keeps the source tree's leaf-order permutation (copied into the
// `Bvh4Tree`); query helpers take the same `ConstSpan<AABB3<f32>>` prims back,
// exactly like the binary side. Deterministic (the "open the biggest interior
// child" rule breaks ties on the lower source-node index).
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh/bvh_query.hpp> // BvhRayHit (shared with the binary raycast)
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>

#include <limits>
#include <optional>

namespace crd::geometry::bvh
{
// One slot of a `Bvh4Node`. Leaf slot: `count > 0`, `first` = start in
// `Bvh4Tree::prim_indices()`. Node slot: `count == 0`, `first` = child node index.
struct Bvh4Child
{
    AABB3<crd::f32> bounds{};
    crd::u32 first{0};
    crd::u16 count{0};

    [[nodiscard]] bool is_leaf() const noexcept { return count != 0; }
};

struct Bvh4Node
{
    AABB3<crd::f32> bounds{}; // union of the live children's bounds
    Bvh4Child children[4]{};
    crd::u8 child_count{0}; // 2–4 normally; 1 only for the synthetic single-leaf root
    crd::u8 pad_[3]{0, 0, 0};
};

// Query-traversal stack size. The collapse roughly halves the binary tree's
// depth, but a 4-ary DFS that pushes all children at once needs ~3·depth slots
// — 256 covers any practical scene; the helpers assert it.
inline constexpr crd::usize k_max_bvh4_stack = 256;

class Bvh4Tree
{
public:
    explicit Bvh4Tree(crd::memory::IAllocator* alloc) noexcept : m_nodes(alloc), m_prim_indices(alloc) {}

    Bvh4Tree(const Bvh4Tree&) = delete;
    Bvh4Tree& operator=(const Bvh4Tree&) = delete;
    Bvh4Tree(Bvh4Tree&&) noexcept = default;
    Bvh4Tree& operator=(Bvh4Tree&&) noexcept = default;
    ~Bvh4Tree() = default;

    [[nodiscard]] bool is_empty() const noexcept { return m_nodes.size() == 0; }
    [[nodiscard]] crd::usize node_count() const noexcept { return m_nodes.size(); }
    [[nodiscard]] crd::usize prim_count() const noexcept { return m_prim_indices.size(); }
    [[nodiscard]] crd::u32 root() const noexcept { return m_root; }
    [[nodiscard]] crd::containers::ConstSpan<Bvh4Node> nodes() const noexcept
    {
        return crd::containers::ConstSpan<Bvh4Node>(m_nodes.data(), m_nodes.size());
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> prim_indices() const noexcept
    {
        return crd::containers::ConstSpan<crd::u32>(m_prim_indices.data(), m_prim_indices.size());
    }
    [[nodiscard]] AABB3<crd::f32> bounds() const noexcept;

    // ---- builder-side mutators (used by `bvh4_collapse`) -------------------
    [[nodiscard]] crd::containers::Array<Bvh4Node>& nodes_mut() noexcept { return m_nodes; }
    [[nodiscard]] crd::containers::Array<crd::u32>& prim_indices_mut() noexcept { return m_prim_indices; }
    void set_root(crd::u32 r) noexcept { m_root = r; }

private:
    crd::containers::Array<Bvh4Node> m_nodes;
    crd::containers::Array<crd::u32> m_prim_indices;
    crd::u32 m_root{0};
};

// Collapse a built binary `BvhTree` into a 4-wide `Bvh4Tree`. The result keeps
// `binary`'s leaf-order permutation (copied in); query helpers take the same
// prims span. An empty source ⇒ an empty result. A single-leaf source ⇒ a tree
// with one node (`child_count == 1`).
[[nodiscard]] Bvh4Tree bvh4_collapse(const BvhTree& binary, crd::memory::IAllocator* alloc);

// Nearest-hit raycast over a `Bvh4Tree` — same semantics & result as the binary
// `bvh_raycast` over the source tree (the collapse changes only the fan-out).
// Four sequential robust slab tests per node (the "scalar ray-vs-4-AABB").
[[nodiscard]] std::optional<BvhRayHit> bvh4_raycast(const Bvh4Tree& tree,
                                                    crd::containers::ConstSpan<AABB3<crd::f32>> prims,
                                                    const Ray3<crd::f32>& ray,
                                                    crd::f32 tmax = std::numeric_limits<crd::f32>::infinity());

// AABB-overlap over a `Bvh4Tree` — same result set as the binary `bvh_overlap`.
template <typename Fn>
inline void bvh4_overlap(const Bvh4Tree& tree, crd::containers::ConstSpan<AABB3<crd::f32>> prims,
                         const AABB3<crd::f32>& box, Fn&& on_prim)
{
    if (tree.is_empty())
    {
        return;
    }
    const crd::containers::ConstSpan<Bvh4Node> nodes = tree.nodes();
    const crd::containers::ConstSpan<crd::u32> prim_idx = tree.prim_indices();

    crd::u32 stack[k_max_bvh4_stack];
    crd::usize sp = 0;
    stack[sp++] = tree.root();
    while (sp > 0)
    {
        const Bvh4Node& node = nodes[stack[--sp]];
        for (crd::u8 c = 0; c < node.child_count; ++c)
        {
            const Bvh4Child& ch = node.children[c];
            if (!crd::geometry::primitives::intersects(ch.bounds, box))
            {
                continue;
            }
            if (ch.is_leaf())
            {
                for (crd::u32 i = ch.first; i < ch.first + ch.count; ++i)
                {
                    const crd::u32 p = prim_idx[i];
                    if (crd::geometry::primitives::intersects(prims[p], box))
                    {
                        on_prim(p);
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
}

void bvh4_overlap(const Bvh4Tree& tree, crd::containers::ConstSpan<AABB3<crd::f32>> prims, const AABB3<crd::f32>& box,
                  crd::containers::Array<crd::u32>& out);

// ---- closest point (v1i-a) -------------------------------------------------

// The primitive (and the point on its AABB) closest to `query`, considering
// only primitives within `max_dist`. Same semantics as `bvh_closest_point` over
// the source binary tree — the collapse changes only fan-out. Branch-and-bound:
// each child's AABB squared distance is a lower bound on every leaf below it;
// children sorted near-first per popped node so `best_d2` tightens before far
// subtrees are reached. Squared throughout; `max_dist²` stored as the cutoff.
[[nodiscard]] std::optional<BvhClosestPoint> bvh4_closest_point(
    const Bvh4Tree& tree, crd::containers::ConstSpan<AABB3<crd::f32>> prims, const crd::math::Vec3<crd::f32>& query,
    crd::f32 max_dist = std::numeric_limits<crd::f32>::infinity());

} // namespace crd::geometry::bvh
