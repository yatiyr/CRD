#include <crd/hesap/ordering/rcm.hpp>

#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>

#include <utility>  // std::swap

// -----------------------------------------------------------------------
// Reverse Cuthill-McKee. Pseudo-peripheral start node via George-Liu (1979):
// BFS from the lowest-index unvisited vertex (D(ord)-3 structural seed), take a
// deepest-level vertex of minimum degree (tie → ascending index, D(ord)-1), BFS
// again from it, repeat until the rooted level structure stops deepening; capped
// at kMaxGeorgeLiuIters. Then Cuthill-McKee BFS (neighbours enqueued by ascending
// (degree, index), D(ord)-1), reversed → RCM. Disconnected graphs: components in
// ascending lowest-unvisited order, each with its own pseudo-peripheral root.
// -----------------------------------------------------------------------

namespace crd::hesap::ordering
{
namespace
{
constexpr crd::u32 kMaxGeorgeLiuIters = 5;

// Level-set BFS over the component of `root` (vertices not in `gvisited`), using
// a stamp marker (no per-call reset). Returns the depth (#levels - 1); `frontier`
// is left holding the deepest level's vertices.
crd::u32 bfs_depth(const AdjacencyGraph& g, crd::u32 root, const crd::containers::Array<crd::u8>& gvisited,
                   crd::containers::Array<crd::u32>& seen, crd::u32 stamp, crd::containers::Array<crd::u32>& frontier,
                   crd::containers::Array<crd::u32>& nextf)
{
    const crd::u32* xadj = g.xadj.data();
    const crd::u32* adj  = g.adjncy.data();
    frontier.clear();
    frontier.push_back(root);
    seen[root]     = stamp;
    crd::u32 depth = 0;
    for (;;)
    {
        nextf.clear();
        for (crd::usize t = 0; t < frontier.size(); ++t)
        {
            const crd::u32 u = frontier[t];
            for (crd::u32 p = xadj[u]; p < xadj[u + 1]; ++p)
            {
                const crd::u32 v = adj[p];
                if (gvisited[v] == 0 && seen[v] != stamp)
                {
                    seen[v] = stamp;
                    nextf.push_back(v);
                }
            }
        }
        if (nextf.empty())
        {
            break;  // `frontier` is the deepest level
        }
        std::swap(frontier, nextf);
        ++depth;
    }
    return depth;
}

crd::u32 pseudo_peripheral(const AdjacencyGraph& g, crd::u32 seed, const crd::containers::Array<crd::u8>& gvisited,
                           crd::containers::Array<crd::u32>& seen, crd::u32& stamp,
                           crd::containers::Array<crd::u32>& frontier, crd::containers::Array<crd::u32>& nextf)
{
    crd::u32 root      = seed;
    crd::u32 depth     = bfs_depth(g, root, gvisited, seen, ++stamp, frontier, nextf);
    for (crd::u32 iter = 0; iter < kMaxGeorgeLiuIters; ++iter)
    {
        // Min-degree vertex of the deepest level (tie → ascending index).
        crd::u32 cand     = frontier[0];
        crd::u32 cand_deg = g.degree(cand);
        for (crd::usize t = 1; t < frontier.size(); ++t)
        {
            const crd::u32 v   = frontier[t];
            const crd::u32 deg = g.degree(v);
            if (deg < cand_deg || (deg == cand_deg && v < cand))
            {
                cand     = v;
                cand_deg = deg;
            }
        }
        const crd::u32 new_depth = bfs_depth(g, cand, gvisited, seen, ++stamp, frontier, nextf);
        if (new_depth <= depth)
        {
            return root;  // level structure stopped deepening
        }
        depth = new_depth;
        root  = cand;
    }
    return root;
}
} // namespace

Permutation rcm_order(const AdjacencyGraph& g, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    Permutation    p(alloc);
    p.perm.resize(n);
    if (n == 0)
    {
        p.rebuild_inverse();
        return p;
    }

    const crd::u32* xadj = g.xadj.data();
    const crd::u32* adj  = g.adjncy.data();

    crd::containers::Array<crd::u8>  gvisited(alloc);
    gvisited.resize(n);  // 0 = unvisited
    crd::containers::Array<crd::u32> seen(alloc);
    seen.resize(n);  // stamp marker for exploratory BFS (0 = never)
    crd::u32 stamp = 0;
    crd::containers::Array<crd::u32> frontier(alloc);
    crd::containers::Array<crd::u32> nextf(alloc);
    crd::containers::Array<crd::u32> cm(alloc);
    crd::containers::Array<crd::u32> queue(alloc);
    crd::containers::Array<crd::u32> nbrs(alloc);
    cm.reserve(n);

    auto degree_index_less = [&g](crd::u32 a, crd::u32 b) {
        const crd::u32 da = g.degree(a);
        const crd::u32 db = g.degree(b);
        return da < db || (da == db && a < b);
    };

    for (crd::u32 seed = 0; seed < n; ++seed)  // ascending → deterministic component order
    {
        if (gvisited[seed] != 0)
        {
            continue;
        }
        const crd::u32 root = pseudo_peripheral(g, seed, gvisited, seen, stamp, frontier, nextf);

        // Cuthill-McKee BFS from `root`: enqueue each node's unvisited neighbours
        // in ascending (degree, index) order; `cm` is the dequeue sequence.
        queue.clear();
        queue.push_back(root);
        gvisited[root] = 1;
        crd::usize head = 0;
        while (head < queue.size())
        {
            const crd::u32 u = queue[head++];
            cm.push_back(u);
            nbrs.clear();
            for (crd::u32 q = xadj[u]; q < xadj[u + 1]; ++q)
            {
                const crd::u32 v = adj[q];
                if (gvisited[v] == 0)
                {
                    gvisited[v] = 1;
                    nbrs.push_back(v);
                }
            }
            crd::containers::sort(nbrs.data(), nbrs.data() + nbrs.size(), degree_index_less);
            for (crd::usize t = 0; t < nbrs.size(); ++t)
            {
                queue.push_back(nbrs[t]);
            }
        }
    }

    CRD_ASSERT_MSG(cm.size() == n, "rcm_order: every vertex must be ordered exactly once");
    for (crd::u32 i = 0; i < n; ++i)
    {
        p.perm[i] = cm[n - 1 - i];  // reverse → RCM
    }
    p.rebuild_inverse();
    return p;
}

Permutation rcm_order(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc)
{
    return rcm_order(build_adjacency(pattern, alloc), alloc);
}

} // namespace crd::hesap::ordering
