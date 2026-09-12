#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — the BVH container (v1a).
//
// `BvhTree` is a flat array of 32-byte nodes plus a primitive-index array (the
// leaf order — a permutation of `[0, n)`). It is a *binary* tree; `bvh_build`
// always builds binary, and `bvh4_collapse` (`bvh4.hpp`) widens a built tree to
// the 4-wide `Bvh4Tree` for SIMD-friendly traversal. The tree does NOT own the
// input AABBs: the builder is handed a `ConstSpan<AABB3<f32>>`, records each
// leaf node's union bounds, and reorders the index array; query helpers take
// the same span back (ADR-0076 §11 — consumers stash trees in their own
// allocators alongside the primitive data).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>

namespace crd::geometry::bvh
{
using crd::geometry::primitives::AABB3;

// 32-byte node — two per 64-byte cache line.
//   * interior  ⇒ `prim_count == 0`; the two children are nodes
//     `left_first` and `left_first + 1`; `split_axis` ∈ {0,1,2} is the axis the
//     SAH partition cut on (recorded so traversal can visit the near child
//     first).
//   * leaf      ⇒ `prim_count != 0`; the leaf owns prim-index slots
//     `[left_first, left_first + prim_count)` in `BvhTree::prim_indices()`.
struct BvhNode
{
    AABB3<crd::f32> bounds{};
    crd::u32 left_first{0};
    crd::u16 prim_count{0};
    crd::u8 split_axis{0};
    crd::u8 pad_{0};

    [[nodiscard]] constexpr bool is_leaf() const noexcept { return prim_count != 0; }
};
static_assert(sizeof(BvhNode) == 32, "BvhNode must stay 32 bytes (two per cache line)");

// Maximum traversal depth. The builder asserts the recursion never exceeds this
// (binned SAH on non-pathological input stays well under ~40 even for millions
// of primitives); query helpers size their explicit stack to match.
inline constexpr crd::usize k_max_bvh_depth = 64;

class BvhTree
{
public:
    explicit BvhTree(crd::memory::IAllocator* alloc) noexcept : m_nodes(alloc), m_prim_indices(alloc) {}

    BvhTree(const BvhTree&) = delete;
    BvhTree& operator=(const BvhTree&) = delete;
    BvhTree(BvhTree&&) noexcept = default;
    BvhTree& operator=(BvhTree&&) noexcept = default;
    ~BvhTree() = default;

    [[nodiscard]] bool is_empty() const noexcept { return m_nodes.size() == 0; }
    [[nodiscard]] crd::usize node_count() const noexcept { return m_nodes.size(); }
    [[nodiscard]] crd::usize prim_count() const noexcept { return m_prim_indices.size(); }
    [[nodiscard]] crd::u32 root() const noexcept { return m_root; }
    [[nodiscard]] crd::containers::ConstSpan<BvhNode> nodes() const noexcept
    {
        return crd::containers::ConstSpan<BvhNode>(m_nodes.data(), m_nodes.size());
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> prim_indices() const noexcept
    {
        return crd::containers::ConstSpan<crd::u32>(m_prim_indices.data(), m_prim_indices.size());
    }
    // Root bounds — the AABB enclosing every primitive. Identity-empty
    // (min = +∞, max = −∞) for an empty tree.
    [[nodiscard]] AABB3<crd::f32> bounds() const noexcept;

    // ---- builder-side mutators (used by `bvh_build`; not part of the public
    //      query surface) -------------------------------------------------
    [[nodiscard]] crd::containers::Array<BvhNode>& nodes_mut() noexcept { return m_nodes; }
    [[nodiscard]] crd::containers::Array<crd::u32>& prim_indices_mut() noexcept { return m_prim_indices; }
    void set_root(crd::u32 r) noexcept { m_root = r; }

private:
    crd::containers::Array<BvhNode> m_nodes;
    crd::containers::Array<crd::u32> m_prim_indices;
    crd::u32 m_root{0};
};

} // namespace crd::geometry::bvh
