// crd-geometry-spatial — kd_nearest_n impl (Phase 3.1.7 v5a).
//
// Branch-and-bound k-NN. The classic algorithm:
//   * Maintain a max-heap of size ≤ k over (distance², payload) pairs.
//     The heap top is the WORST distance still in the candidate set.
//   * At interior nodes, compute `dq = query[axis] - split_value`. The
//     near child is determined by `sign(dq)`. Visit near first.
//   * Prune the far child when `dq² >= heap_top.distance_squared` AND
//     the heap is full (k results already collected).
//   * At leaves, scan every point. If d² < heap_top: pop top, push new.
//     Tie at d² == heap_top: lower payload index wins (ADR-0076 §4 pin #11).
//
// Heap: caller-owned via `out` (capacity == k). We push directly onto its
// raw storage with `crd::containers::push_heap` / `pop_heap` — same algorithms
// `crd-eylem` will use, deterministic across MSVC / GCC / clang. No std heap.
//
// Final sort: ascending by (distance², payload) using
// `crd::containers::sort` so the result is stable + reproducible.

#include <crd/geometry/spatial/kd_nearest_n.hpp>

#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>

#include <limits>

namespace crd::geometry::spatial
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::math::Vec3;

namespace
{

// Max-heap order — top of heap = WORST (largest distance²). On distance tie,
// HIGHER payload index is "worse" (so it gets evicted first by the lowest-
// payload-wins rule on ties, leaving the lowest-index in the result).
template <MathScalar T>
struct MaxByDistance
{
    [[nodiscard]] bool operator()(const KdNeighbor<T>& a, const KdNeighbor<T>& b) const noexcept
    {
        if (a.distance_squared < b.distance_squared) return true;
        if (a.distance_squared > b.distance_squared) return false;
        return a.payload < b.payload;
    }
};

// Final ascending sort — reverse of the heap's "less".
template <MathScalar T>
struct AscendByDistance
{
    [[nodiscard]] bool operator()(const KdNeighbor<T>& a, const KdNeighbor<T>& b) const noexcept
    {
        if (a.distance_squared < b.distance_squared) return true;
        if (a.distance_squared > b.distance_squared) return false;
        return a.payload < b.payload;
    }
};

template <MathScalar T>
inline T length_sq(const Vec3<T>& v) noexcept
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

template <MathScalar T>
void kd_nearest_n_impl(const KdTree<T>&                       tree,
                       crd::containers::ConstSpan<Vec3<T>>    points,
                       const Vec3<T>&                          query,
                       usize                                   k,
                       crd::containers::Array<KdNeighbor<T>>&  out) noexcept
{
    out.clear();
    if (tree.is_empty() || k == 0U)
    {
        return;
    }
    out.reserve(k);

    const auto nodes  = tree.nodes();
    const auto pt_idx = tree.point_indices();

    // Stack frames carry the lower-bound dq² already accumulated for this
    // subtree (it's the running squared distance from `query` to the
    // *splitting hyperplane* of the closest enclosing ancestor we descended
    // away from). We prune when worst_in_heap < lower_bound² and heap is full.
    struct Frame { u32 node; T lower_dsq; };
    Frame stack[k_max_kd_depth * 2U];
    usize sp = 0;
    stack[sp++] = Frame{tree.root(), T{0}};

    const T inf = std::numeric_limits<T>::infinity();
    T worst = inf; // worst distance² currently in the heap; +inf until heap fills

    MaxByDistance<T> max_cmp{};

    while (sp > 0U)
    {
        const Frame f = stack[--sp];
        // Strict `>` (not `>=`): a subtree whose lower bound equals `worst`
        // could still contain a tied-distance point with a lower payload index
        // that would win the ADR-0076 §4 pin #11 tiebreak — we must descend.
        if (out.size() == k && f.lower_dsq > worst) { continue; }

        const KdNode<T>& n = nodes[f.node];

        if (n.is_leaf())
        {
            for (u32 i = 0; i < n.prim_count; ++i)
            {
                const u32 pidx = pt_idx[n.child_first + i];
                const T d2 = length_sq<T>(points[pidx] - query);
                const KdNeighbor<T> cand{pidx, d2};

                if (out.size() < k)
                {
                    out.push_back(cand);
                    crd::containers::push_heap(out.data(), out.data() + out.size(), max_cmp);
                    worst = (out.size() == k) ? out[0].distance_squared : inf;
                    // Heap top *might* hold a tied-distance entry with a higher
                    // payload than `cand`; that's fine — final ascending sort
                    // resolves the tie via AscendByDistance's secondary key.
                }
                else
                {
                    // Heap is full. Replace top only if cand is strictly better
                    // OR ties on distance with strictly lower payload (pin #11).
                    const KdNeighbor<T>& top = out[0];
                    const bool better = (d2 < top.distance_squared)
                                     || (d2 == top.distance_squared && pidx < top.payload);
                    if (better)
                    {
                        crd::containers::pop_heap(out.data(), out.data() + out.size(), max_cmp);
                        out[k - 1U] = cand;
                        crd::containers::push_heap(out.data(), out.data() + out.size(), max_cmp);
                        worst = out[0].distance_squared;
                    }
                }
            }
            continue;
        }

        // Interior — visit near subtree first.
        const T qa = query[n.split_axis];
        const T dq = qa - n.split_value;
        const u32 left  = n.child_first;
        const u32 right = n.child_first + 1U;

        u32 near_node, far_node;
        if (dq < T{0}) { near_node = left;  far_node = right; }
        else            { near_node = right; far_node = left;  }

        const T far_lower = f.lower_dsq + dq * dq;

        CRD_ASSERT(sp + 2U <= k_max_kd_depth * 2U);
        // Push far first so near pops first. Far frame carries its inflated
        // lower-bound; near frame inherits the parent lower bound (it doesn't
        // cross the split plane).
        // `<= worst` (not `<`): tied lower-bound subtree may contain a lower-
        // payload tie winner — descend.
        if (out.size() < k || far_lower <= worst) { stack[sp++] = Frame{far_node,  far_lower}; }
        stack[sp++] = Frame{near_node, f.lower_dsq};
    }

    // Final ascending sort by (distance², payload) — stable + reproducible.
    crd::containers::sort(out.data(), out.data() + out.size(), AscendByDistance<T>{});
}

} // namespace

template <MathScalar T>
void kd_nearest_n(const KdTree<T>&                       tree,
                   crd::containers::ConstSpan<Vec3<T>>    points,
                   const Vec3<T>&                          query,
                   usize                                   k,
                   crd::containers::Array<KdNeighbor<T>>&  out) noexcept
{
    kd_nearest_n_impl<T>(tree, points, query, k, out);
}

template void kd_nearest_n<f32>(const KdTree<f32>&,
                                  crd::containers::ConstSpan<Vec3<f32>>,
                                  const Vec3<f32>&, usize,
                                  crd::containers::Array<KdNeighbor<f32>>&) noexcept;
template void kd_nearest_n<f64>(const KdTree<f64>&,
                                  crd::containers::ConstSpan<Vec3<f64>>,
                                  const Vec3<f64>&, usize,
                                  crd::containers::Array<KdNeighbor<f64>>&) noexcept;

} // namespace crd::geometry::spatial
