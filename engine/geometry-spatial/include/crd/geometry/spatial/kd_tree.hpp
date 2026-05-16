#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — KdTree<T> storage + builder (Phase 3.1.7 v5a).
//
// Static balanced KD-tree over a point set. Built once over a `ConstSpan<Vec3<T>>`,
// then queried via `kd_nearest_n` / `kd_radius` / `kd_range_aabb`. The tree is
// **non-owning**: it stores a permutation `point_indices` of `[0, n)` and the
// caller keeps the point array alive (mirrors the `TriangleMeshView` + `BvhTree`
// split — build once, query thousands of times).
//
// ── Algorithm (Friedman / Bentley / Finkel 1977 + nanoflann/PCL refinements) ──
//
//   * **Split axis = widest extent of the current node's AABB**, NOT canonical
//     round-robin (X, Y, Z, X, …). Better query depth on skewed inputs (which
//     scene + lidar + particle clouds typically are). Tie-break on equal extent
//     by axis index (X<Y<Z) — keeps the tree topology canonical.
//
//   * **Leaf bucket size = `k_kd_leaf_threshold` (default 8).** When a node's
//     point count drops to ≤ threshold, it becomes a leaf and queries scan it
//     linearly. nanoflann ≈ 10, PCL ≈ 15. Cuts node count ~8×, kills per-node
//     prune overhead. Retunable at v5-close benchmark.
//
//   * **Median pick via `crd::containers::nth_element` + lex-tuple comparator
//     `(coord_value, original_index)`.** No two elements compare equal under the
//     tuple comparator, so the partition is fully determined; the resulting
//     tree topology + leaf order is byte-identical across MSVC / GCC / clang
//     (the standard `std::nth_element` partitions equal-keyed elements
//     implementation-definedly — that's the determinism trap on KD-trees).
//
//   * **Builder rejects non-finite input** in debug (`CRD_ASSERT(all_finite(points))`).
//     Queries TOLERATE non-finite query points (return std::nullopt / no hits)
//     — symmetric with the ADR-0076 §15 builder-reject / query-tolerate pin
//     `crd-geometry-bvh` already enforces.
//
//   * **Determinism tiebreak** (ADR-0076 §4 pin #11): on equal squared distance,
//     lowest payload (original-input) index wins. Same rule as `bvh_closest_point`,
//     `mesh_closest_point`, GJK / Quickhull.
//
// ── Two-layer typing (ADR-0078 §5 D32-D36) ───────────────────────────────────
//
// `KdTree<T>` template parameter is `MathScalar T` (raw f32 / f64). Typed
// `Vec3<Length32>` consumers go through `kd_queries_typed.hpp` strip-compute-
// retag wrappers (zero overhead — `to_raw_vec` / `from_raw_vec` are constexpr).
// Inner traversal stays raw — same pattern as `crd-geometry-mesh` v4.
//
// ── Node layout (16 bytes — four per cache line) ─────────────────────────────
//
//   * `interior` ⇒ `prim_count == 0`; `split_axis` ∈ {0,1,2}; `split_value`
//                  is the splitting coordinate; left child at `child_first`,
//                  right child at `child_first + 1`. Points with
//                  `coord[split_axis] < split_value` go left (lex-tuple tie-
//                  break decides equal-coord ownership at build time).
//   * `leaf`     ⇒ `prim_count != 0`; the leaf owns `point_indices` slots
//                  `[child_first, child_first + prim_count)`.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::spatial
{
using crd::geometry::primitives::AABB3;
using crd::math::MathScalar;
using crd::math::Vec3;

// Default leaf bucket size — points per leaf at which build stops splitting.
// Retunable per call via `KdBuildOptions::leaf_threshold`.
inline constexpr crd::u32 k_kd_leaf_threshold = 8;

// Maximum traversal depth. Balanced-median build on N points yields depth
// `ceil(log2(N / leaf_threshold))`; even at 2^31 points / 8-leaf that is 28.
// Stack-32 covers all realistic inputs with margin.
inline constexpr crd::usize k_max_kd_depth = 64;

// Compact node — 12 bytes for `f32` (5 per cache line), 16 bytes for `f64`
// (4 per cache line). Layout pinned (ADR-0076 §16 pin #2 style — query
// helpers depend on field offsets) so accidental field bloat fails CI.
template <MathScalar T> struct KdNode
{
    T          split_value{};   // interior: splitting coord; leaf: 0
    crd::u32   child_first{0};  // interior: left child node idx; leaf: first slot in point_indices
    crd::u16   prim_count{0};   // 0 ⇒ interior; >0 ⇒ leaf
    crd::u8    split_axis{0};   // interior: 0..2; leaf: 0
    crd::u8    pad_{0};

    [[nodiscard]] constexpr bool is_leaf() const noexcept { return prim_count != 0; }
};
static_assert(sizeof(KdNode<crd::f32>) == 12,
              "KdNode<f32> sizing pinned — 12 B (5 per 64 B cache line); accidental field bloat is a CI fail");
static_assert(sizeof(KdNode<crd::f64>) == 16,
              "KdNode<f64> sizing pinned — 16 B (4 per 64 B cache line); accidental field bloat is a CI fail");

// Build options. Default leaf_threshold = `k_kd_leaf_threshold`.
struct KdBuildOptions
{
    crd::u32 leaf_threshold{k_kd_leaf_threshold};
};

// The tree. Non-owning — caller keeps the point span alive across the tree's
// lifetime. Move-only (the underlying Arrays own their buffers).
template <MathScalar T>
class KdTree
{
public:
    explicit KdTree(crd::memory::IAllocator* alloc) noexcept
        : m_nodes(alloc), m_point_indices(alloc)
    {
    }

    KdTree(const KdTree&) = delete;
    KdTree& operator=(const KdTree&) = delete;
    KdTree(KdTree&&) noexcept = default;
    KdTree& operator=(KdTree&&) noexcept = default;
    ~KdTree() = default;

    [[nodiscard]] bool is_empty() const noexcept { return m_nodes.size() == 0U; }
    [[nodiscard]] crd::usize node_count() const noexcept { return m_nodes.size(); }
    [[nodiscard]] crd::usize point_count() const noexcept { return m_point_indices.size(); }
    [[nodiscard]] crd::u32 root() const noexcept { return m_root; }

    [[nodiscard]] crd::containers::ConstSpan<KdNode<T>> nodes() const noexcept
    {
        return crd::containers::ConstSpan<KdNode<T>>(m_nodes.data(), m_nodes.size());
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> point_indices() const noexcept
    {
        return crd::containers::ConstSpan<crd::u32>(m_point_indices.data(), m_point_indices.size());
    }

    // Root bounds — AABB enclosing every point. Identity-empty (min = +∞,
    // max = −∞) for an empty tree. Pinned at build time.
    [[nodiscard]] AABB3<T> bounds() const noexcept { return m_root_bounds; }

    // ---- builder-side mutators (kd_tree.cpp only; not query-surface) ----
    [[nodiscard]] crd::containers::Array<KdNode<T>>& nodes_mut() noexcept { return m_nodes; }
    [[nodiscard]] crd::containers::Array<crd::u32>& point_indices_mut() noexcept { return m_point_indices; }
    void set_root(crd::u32 r) noexcept { m_root = r; }
    void set_root_bounds(const AABB3<T>& b) noexcept { m_root_bounds = b; }

private:
    crd::containers::Array<KdNode<T>> m_nodes;
    crd::containers::Array<crd::u32>  m_point_indices;
    AABB3<T>                          m_root_bounds{};
    crd::u32                          m_root{0};
};

using KdTreef = KdTree<crd::f32>;
using KdTreed = KdTree<crd::f64>;

// Build a balanced KD-tree over `points`. Returns the tree by value (move).
// Empty input returns an empty tree (`is_empty() == true`).
//
// In debug builds, asserts every point is finite (no NaN, no ±∞) — builder-
// reject contract per ADR-0076 §15. Queries on a built tree tolerate non-
// finite query points (return std::nullopt / no hits).
template <MathScalar T>
[[nodiscard]] KdTree<T> kd_build(crd::containers::ConstSpan<Vec3<T>> points,
                                  crd::memory::IAllocator* alloc,
                                  KdBuildOptions opts = {});

// Explicit-instantiation declarations — the templates' bodies live in the .cpp
// file and are instantiated for `f32` + `f64` there.
extern template KdTree<crd::f32> kd_build<crd::f32>(
    crd::containers::ConstSpan<Vec3<crd::f32>>, crd::memory::IAllocator*, KdBuildOptions);
extern template KdTree<crd::f64> kd_build<crd::f64>(
    crd::containers::ConstSpan<Vec3<crd::f64>>, crd::memory::IAllocator*, KdBuildOptions);

} // namespace crd::geometry::spatial
