#pragma once

// ---------------------------------------------------------------------------
// LbvhTree — fat-node 64-byte LBVH (Karras 2012 + KittenGpuLBVH-style layout).
// Phase 3.1.7 v9a-c-followon "Track A elite rewrite" (2026-05-18).
//
// **Why fat nodes (64 bytes) instead of compact BvhNode (32 bytes)?**
// The compact `BvhNode` layout (`engine/geometry-bvh`) stores each node's
// OWN bounds and follows the "siblings-consecutive" invariant. That's
// optimal for CPU SAH builders that emit nodes sequentially. For GPU LBVH
// it forces:
//   1. Random tree-walk reads during upsweep (children's bounds at random
//      cache lines from the parent) — ~10× slower than the fat-node pattern.
//   2. A canonical-layout reorder pass after build (D156, D163) — another
//      ~20 ms / 1M on CPU.
//
// **Fat-node layout (this slice)** stores BOTH children's AABBs INLINE in
// the parent's struct (`bounds[2]`). Each internal node is 64 bytes = one
// cache line. The KittenGpuLBVH pattern shows this design hits ~1.5 ms / 1M
// on RTX 3090 — 8-12× faster than compact-node Karras implementations.
//
// **Carry-register upsweep:** each leaf-thread starts at its leaf, carrying
// its bounds in a REGISTER variable. At each level it walks UP via
// `parent_idx`, writes its bounds into the parent's `bounds[isRight]` slot,
// then atomically signals. The thread that observes "sibling already done"
// reads the SIBLING's bounds (already written by the first arriver — same
// cache line as the parent's struct), unions with its register-carried
// bounds, and continues walking. NO random tree-walk reads. NO global-
// memory bounds-buffer (every read/write hits a single cache line).
//
// **Index encoding (MSB bit-tricks):**
//   - `parent_idx`: MSB = "I am parent's right child" (0 = left).
//   - `left_idx`, `right_idx`: MSB = "this child is a leaf" (0 = internal).
//   - Lower 31 bits hold the actual index. Cluster cap at 2^31 - 1 ≈ 2 billion
//     primitives — way beyond the v9a-c kRadixMaxItems = 1 M ceiling (D147).
//
// **`fence` field** stores the OTHER endpoint of this node's primitive range
// — used by self-intersection-aware queries for dedup. Lower 31 bits.
//
// **D165 (pinned at v9a-c-followon elite-rewrite)** — LbvhTree is its own
// type, NOT a BvhTree subset. Cerid carries both: BvhTree (compact 32 B
// nodes for CPU SAH builder; binary-traversal via siblings-consecutive
// invariant) and LbvhTree (fat 64 B nodes for GPU LBVH builder; carry-
// register upsweep, explicit child indices). A `lbvh_to_bvh_tree()`
// conversion is provided for consumers needing BvhTree compatibility —
// O(N) post-pass. Most LBVH consumers (eylem broadphase, GPU traversal
// kernels, parallel mesh-cooker bake) query LbvhTree directly.
//
// **Industry precedent:** NVIDIA OptiX, AMD Radeon Rays, KittenGpuLBVH, RTX
// hardware acceleration structures all use fat-node layouts with explicit
// child indices. Our previous compact-node v9a-c choice was a Cerid-
// specific optimization for BvhTree compatibility that paid a real perf tax.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::bvh_gpu
{

// 64-byte LBVH node. ALWAYS one per cache line on x86/ARM (64 B cache line).
//
// Layout pinned via static_asserts below — the GPU shader's
// `lbvh_fat_build.comp` reads/writes via flat u32/float arrays at the
// matching offsets. Any change here requires lockstep update in the shader.
struct alignas(64) LbvhFatNode
{
    // parent_idx: lower 31 bits = parent's array index (within nodes).
    // MSB (bit 31) = "I am parent's RIGHT child" (set) vs LEFT (clear).
    // Root has parent_idx = 0xFFFFFFFFU (sentinel).
    crd::u32 parent_idx{0xFFFFFFFFU};

    // left_idx / right_idx: lower 31 bits = child's array index.
    // MSB = "child is a LEAF" (set) vs internal (clear).
    // Leaves: left_idx/right_idx unused (set to 0).
    crd::u32 left_idx{0U};
    crd::u32 right_idx{0U};

    // fence: lower 31 bits = the OTHER endpoint of this node's primitive
    // range (used for self-intersection dedup in queries). Unused for
    // leaves (set to 0).
    crd::u32 fence{0U};

    // Children's AABBs inlined in the parent's struct. bounds[0] = left
    // child's bounds; bounds[1] = right child's bounds.
    // For LEAF nodes: bounds[0] = the leaf's own AABB; bounds[1] unused (set
    // to inverted-infinity sentinel so a union with it is a no-op).
    crd::geometry::primitives::AABB3<crd::f32> bounds[2]{};

    // ---- Convenience accessors -----------------------------------------

    [[nodiscard]] constexpr bool is_root() const noexcept
    {
        return parent_idx == 0xFFFFFFFFU;
    }
    [[nodiscard]] constexpr bool i_am_right_child() const noexcept
    {
        return (parent_idx & 0x80000000U) != 0U;
    }
    [[nodiscard]] constexpr crd::u32 parent() const noexcept
    {
        return parent_idx & 0x7FFFFFFFU;
    }
    [[nodiscard]] constexpr bool left_is_leaf() const noexcept
    {
        return (left_idx & 0x80000000U) != 0U;
    }
    [[nodiscard]] constexpr bool right_is_leaf() const noexcept
    {
        return (right_idx & 0x80000000U) != 0U;
    }
    [[nodiscard]] constexpr crd::u32 left() const noexcept
    {
        return left_idx & 0x7FFFFFFFU;
    }
    [[nodiscard]] constexpr crd::u32 right() const noexcept
    {
        return right_idx & 0x7FFFFFFFU;
    }
};

static_assert(sizeof(LbvhFatNode) == 64U,
              "LbvhFatNode must be exactly 64 bytes (one cache line on x86/ARM). "
              "Changing this requires updating the GPU shader byte offsets.");
static_assert(alignof(LbvhFatNode) == 64U,
              "LbvhFatNode must be 64-byte aligned for cache-line co-location.");

// LbvhTree carries the fat-node array + the per-leaf primitive-index
// permutation. Built by both `build_lbvh_cpu` and `LbvhGpuPipeline::
// dispatch_build_lbvh`; the two are byte-identical on topology, ULP-
// identical on bounds (D162).
//
// **Indexing convention (Karras-native, NOT canonical BvhTree layout):**
//   - Internal nodes occupy `nodes[0 .. N-2]`. Index 0 is always the root.
//   - Leaves are NOT in `nodes[]`. Leaf bounds are stored INLINE in their
//     parent's `bounds[]` slot — i.e. nodes[parent].bounds[isRight] holds
//     the leaf's AABB. Leaves are referenced only via the MSB-flagged
//     child_idx in a parent's `left_idx`/`right_idx`; their "node" doesn't
//     have a slot in `nodes[]` because all its data (bounds + prim index)
//     is fully held by the parent + prim_indices.
//   - `prim_indices[k]` = the original input primitive index of the k-th
//     leaf (in sorted-Morton order). When a parent's `left_idx` MSB is set,
//     its lower 31 bits are the prim_indices slot for that leaf.
//
// This is a major break from BvhTree (which has 2N-1 entries in `nodes`,
// one per leaf + one per internal). LbvhTree has only N-1 entries. The
// leaf is "embedded" in its parent. Saves 50% of node-array memory at
// 1M primitives.
class LbvhTree
{
public:
    explicit LbvhTree(crd::memory::IAllocator* alloc) noexcept
        : m_nodes(alloc), m_prim_indices(alloc)
    {
    }

    LbvhTree(const LbvhTree&)            = delete;
    LbvhTree& operator=(const LbvhTree&) = delete;
    LbvhTree(LbvhTree&&) noexcept        = default;
    LbvhTree& operator=(LbvhTree&&) noexcept = default;
    ~LbvhTree()                           = default;

    [[nodiscard]] bool is_empty() const noexcept { return m_prim_indices.size() == 0U; }
    [[nodiscard]] crd::usize internal_count() const noexcept { return m_nodes.size(); }
    [[nodiscard]] crd::usize prim_count() const noexcept { return m_prim_indices.size(); }
    [[nodiscard]] crd::u32 root() const noexcept { return m_root; }

    [[nodiscard]] crd::containers::ConstSpan<LbvhFatNode> nodes() const noexcept
    {
        return {m_nodes.data(), m_nodes.size()};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> prim_indices() const noexcept
    {
        return {m_prim_indices.data(), m_prim_indices.size()};
    }

    // Builder-side mutators.
    [[nodiscard]] crd::containers::Array<LbvhFatNode>& nodes_mut() noexcept { return m_nodes; }
    [[nodiscard]] crd::containers::Array<crd::u32>& prim_indices_mut() noexcept { return m_prim_indices; }
    void set_root(crd::u32 r) noexcept { m_root = r; }

private:
    crd::containers::Array<LbvhFatNode> m_nodes;          // N-1 internal nodes (plus the special singleton N=1 case below)
    crd::containers::Array<crd::u32>    m_prim_indices;   // N entries — original prim index per leaf in sorted order
    crd::u32                            m_root{0};        // always 0 except for the degenerate N=1 case
};

} // namespace crd::geometry::bvh_gpu
