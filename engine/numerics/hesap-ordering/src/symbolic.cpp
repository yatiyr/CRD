#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/ordering/adjacency_graph.hpp>
#include <crd/hesap/ordering/symbolic.hpp>

// -----------------------------------------------------------------------
// Symbolic Cholesky fill metrics: elimination tree (Liu 1986, 1990), postorder, and column counts
// from the row-subtree skeleton (Gilbert, Ng and Peyton 1994). The algorithms are the ones presented
// in Tim Davis, "Direct Methods for Sparse Linear Systems" (SIAM 2006), chapter 4, and implemented in
// his CSparse; the routines below are our own implementation of those published methods. Everything
// runs in SIGNED i32 with -1 as the "none" marker: the leaf test compares first-descendant stamps
// against a running maximum that starts at -1, which only works when "none" is the smallest value
// (u32 0xFFFFFFFF is the largest). Values convert to u32 only at the API boundary; n < 2^31 always
// (matrix dimension).
//
// Works on the symmetrised AdjacencyGraph (full off-diagonal pattern, sorted ascending): for a
// symmetric matrix the CSR and CSC patterns coincide, so row k's neighbour list is also column k's.
// -----------------------------------------------------------------------

namespace crd::hesap::ordering
{
namespace
{
// Elimination tree of chol(A), after Liu: rows are taken in order, and for each smaller neighbour r of
// row k the tree built so far is climbed from r. `ancestor` is the path-compression shortcut (Liu 1990):
// the climb moves from node to ancestor[node] and re-aims every node it leaves at k. It stops at a node
// whose shortcut is k (already linked below this row) or whose shortcut is none; the latter is a
// current root and becomes a child of k. parent[k] = -1 marks a root.
void etree_i32(const AdjacencyGraph& g, crd::i32* parent, crd::i32* ancestor)
{
    const crd::i32 n = static_cast<crd::i32>(g.n);
    const crd::u32* row_start = g.xadj.data();
    const crd::u32* neighbour = g.adjncy.data();
    for (crd::i32 k = 0; k < n; ++k)
    {
        parent[k] = -1;
        ancestor[k] = -1;
        for (crd::u32 e = row_start[k]; e < row_start[k + 1]; ++e)
        {
            crd::i32 r = static_cast<crd::i32>(neighbour[e]);
            if (r >= k)
            {
                break; // ascending list: the rest lies on or above the diagonal
            }
            for (;;)
            {
                const crd::i32 up = ancestor[r];
                if (up == k)
                {
                    break; // already linked below this row
                }
                ancestor[r] = k;
                if (up == -1)
                {
                    parent[r] = k; // r is a root of the tree built so far: hang it under k
                    break;
                }
                r = up;
            }
        }
    }
}

// Column elimination tree = elimination tree of chol(A^T A), found without forming A^T A (Gilbert and
// Ng; Liu). A is the COMPRESSED CSC pattern (m rows x n cols): `ap`/`ai` are the column pointers and
// row indices. `prev` (length m) remembers, for each row, the latest column seen holding it; that
// earlier column stands in for the A^T A neighbour the row contributes, so the climb is the one used
// for the symmetric tree. parent[k] = -1 marks a root.
void etree_ata_i32(const crd::u32* ap, const crd::u32* ai, crd::i32 m, crd::i32 n, crd::i32* parent, crd::i32* ancestor,
                   crd::i32* prev)
{
    for (crd::i32 r = 0; r < m; ++r)
    {
        prev[r] = -1;
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        parent[k] = -1;
        ancestor[k] = -1;
        const crd::u32 col_end = ap[static_cast<crd::u32>(k) + 1];
        for (crd::u32 e = ap[static_cast<crd::u32>(k)]; e < col_end; ++e)
        {
            const crd::u32 row = ai[e];
            crd::i32 r = prev[row];
            prev[row] = k;
            if (r == -1 || r >= k)
            {
                continue; // no earlier column holds this row
            }
            for (;;)
            {
                const crd::i32 up = ancestor[r];
                if (up == k)
                {
                    break;
                }
                ancestor[r] = k;
                if (up == -1)
                {
                    parent[r] = k;
                    break;
                }
                r = up;
            }
        }
    }
}

// Depth-first postorder of the subtree under `root`, without recursion. `head`/`next` hold the child
// lists (built ascending, so the order is deterministic). Emits into post[slot...] and returns the next
// free slot. A node is written once its child list is empty; children are consumed off `head` as they
// are entered.
crd::i32 emit_subtree_post_i32(crd::i32 root, crd::i32 slot, crd::i32* head, const crd::i32* next, crd::i32* post,
                               crd::i32* stack)
{
    crd::i32 depth = 0;
    stack[0] = root;
    for (;;)
    {
        const crd::i32 node = stack[depth];
        const crd::i32 child = head[node];
        if (child != -1)
        {
            head[node] = next[child];
            stack[++depth] = child;
            continue;
        }
        post[slot++] = node;
        if (depth == 0)
        {
            return slot;
        }
        --depth;
    }
}

// Postorder of a forest given parent[]; post[] receives the ordering. Children are chained per parent
// by sweeping nodes from the top down, so each chain ascends; trees are then emitted in ascending root
// order.
void post_order_i32(const crd::i32* parent, crd::i32 n, crd::i32* post, crd::i32* head, crd::i32* next, crd::i32* stack)
{
    for (crd::i32 v = 0; v < n; ++v)
    {
        head[v] = -1;
    }
    for (crd::i32 v = n; v-- > 0;)
    {
        const crd::i32 up = parent[v];
        if (up != -1)
        {
            next[v] = head[up];
            head[up] = v;
        }
    }
    crd::i32 slot = 0;
    for (crd::i32 v = 0; v < n; ++v)
    {
        if (parent[v] == -1)
        {
            slot = emit_subtree_post_i32(v, slot, head, next, post, stack);
        }
    }
}

// Skeleton test for the column counts (Gilbert-Ng-Peyton): is j a leaf of the row-i subtree? Returns
// the least common ancestor of j and the previous leaf of that subtree when there is one. *kind is
// 0 when j is no leaf, 1 for the first leaf of row i (the LCA is then i itself), 2 for a later leaf.
// row_stamp[i] is the largest first-descendant stamp among the leaves of row i so far (-1 for none);
// row_last_leaf[i] is the most recent leaf. `ancestor` is a union-find forest over the visited nodes.
crd::i32 skeleton_leaf_i32(crd::i32 i, crd::i32 j, const crd::i32* first, crd::i32* row_stamp, crd::i32* row_last_leaf,
                           crd::i32* ancestor, crd::i32* kind)
{
    *kind = 0;
    if (i <= j)
    {
        return -1;
    }
    if (first[j] <= row_stamp[i])
    {
        return -1; // j already lies under an earlier leaf of row i (needs -1 to be the minimum)
    }
    row_stamp[i] = first[j];
    const crd::i32 earlier = row_last_leaf[i];
    row_last_leaf[i] = j;
    if (earlier == -1)
    {
        *kind = 1;
        return i;
    }
    *kind = 2;
    crd::i32 root = earlier; // union-find: locate the set representative of the earlier leaf ...
    while (ancestor[root] != root)
    {
        root = ancestor[root];
    }
    for (crd::i32 node = earlier; node != root;) // ... then compress the path onto it
    {
        const crd::i32 up = ancestor[node];
        ancestor[node] = root;
        node = up;
    }
    return root;
}

// Nonzero pattern of ROW k of L (symmetric case), after the row-subtree characterisation of Liu: the
// columns i < k with L(k,i) != 0 are the nodes on the etree paths from each neighbour of k up to k.
// Returns `top`; on exit s[top..n-1] holds those columns in topological (etree) order. A node counts as
// visited when its `stamp` entry equals k + 1. Rows come in increasing order, so no stale entry of an
// earlier row can equal a later stamp and nothing needs resetting between rows; `stamp` must be all
// zero before the first row. Each path is written downward from `top` and then reversed in place, so it
// lands in ascending climb order just below the paths already emitted.
crd::i32 row_pattern_i32(const AdjacencyGraph& g, crd::i32 k, const crd::i32* parent, crd::i32* s, crd::i32* stamp)
{
    const crd::i32 n = static_cast<crd::i32>(g.n);
    const crd::u32* row_start = g.xadj.data();
    const crd::u32* neighbour = g.adjncy.data();
    const crd::i32 mine = k + 1;
    crd::i32 top = n;

    stamp[k] = mine; // k ends every climb
    const crd::u32 row_end = row_start[k + 1];
    for (crd::u32 e = row_start[k]; e < row_end; ++e)
    {
        crd::i32 node = static_cast<crd::i32>(neighbour[e]);
        if (node > k)
        {
            break; // ascending, no diagonal: nothing but upper entries remain
        }
        const crd::i32 high = top;
        while (stamp[node] != mine)
        {
            stamp[node] = mine;
            s[--top] = node;
            node = parent[node];
        }
        for (crd::i32 lo = top, hi = high - 1; lo < hi; ++lo, --hi)
        {
            const crd::i32 held = s[lo];
            s[lo] = s[hi];
            s[hi] = held;
        }
    }
    return top;
}

// Fundamental supernode partition (Liu-Ng-Peyton; CHOLMOD super_symbolic test).
// Column j (>=1) STARTS a new supernode iff it is not the etree-parent of j-1, or
// its column count is not exactly one less than j-1's (structures don't nest), or
// it has more than one child in the etree (a merge point). Pure integer, no
// tie-breaks. Fills `super` with nsuper+1 boundaries; returns nsuper.
crd::u32 fundamental_supernodes_i32(const crd::i32* parent, const crd::i32* colcount, crd::i32 n,
                                    crd::containers::Array<crd::u32>& super, crd::i32* nchild)
{
    for (crd::i32 j = 0; j < n; ++j)
    {
        nchild[j] = 0;
    }
    for (crd::i32 j = 0; j < n; ++j)
    {
        if (parent[j] != -1)
        {
            ++nchild[parent[j]];
        }
    }
    super.clear();
    if (n == 0)
    {
        return 0;
    }
    super.push_back(0U);
    for (crd::i32 j = 1; j < n; ++j)
    {
        if (parent[j - 1] != j || colcount[j - 1] != colcount[j] + 1 || nchild[j] > 1)
        {
            super.push_back(static_cast<crd::u32>(j));
        }
    }
    const crd::u32 nsuper = static_cast<crd::u32>(super.size());
    super.push_back(static_cast<crd::u32>(n));
    return nsuper;
}

// Column counts of L (symmetric case) from the row-subtree skeleton, Gilbert-Ng-Peyton 1994.
// colcount[j] = nnz of column j of L including the diagonal; the sum is nnz(L). The weights start at 1
// for a leaf of the etree and 0 elsewhere; walking the postorder, column j then adds 1 for every row
// of which it is a skeleton leaf, subtracts 1 at the LCA of consecutive leaves of a row subtree, and
// subtracts 1 from its parent (which re-adds the child's count below). A final sweep accumulates the
// weights up the tree. `first[v]` is the postorder index of the first descendant of v.
void counts_i32(const AdjacencyGraph& g, const crd::i32* parent, const crd::i32* post, crd::i32* colcount,
                crd::i32* ancestor, crd::i32* row_stamp, crd::i32* row_last_leaf, crd::i32* first)
{
    const crd::i32 n = static_cast<crd::i32>(g.n);
    const crd::u32* row_start = g.xadj.data();
    const crd::u32* neighbour = g.adjncy.data();
    for (crd::i32 v = 0; v < n; ++v)
    {
        ancestor[v] = v;
        row_stamp[v] = -1;
        row_last_leaf[v] = -1;
        first[v] = -1;
    }
    // Postorder places every subtree in one contiguous run, so the first child met passes its own first
    // descendant up; a node reached with no stamp yet has no children.
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 j = post[k];
        const bool childless = first[j] == -1;
        colcount[j] = childless ? 1 : 0;
        if (childless)
        {
            first[j] = k;
        }
        const crd::i32 up = parent[j];
        if (up != -1 && first[up] == -1)
        {
            first[up] = first[j];
        }
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 j = post[k];
        if (parent[j] != -1)
        {
            --colcount[parent[j]];
        }
        // Only neighbours below the diagonal row j can start a row subtree. The list ascends, so read it
        // from the top and stop at the first entry not past j; each row's state is independent of the
        // others, so the order does not change any count.
        const crd::u32 col_begin = row_start[j];
        for (crd::u32 e = row_start[j + 1]; e > col_begin; --e)
        {
            const crd::i32 row = static_cast<crd::i32>(neighbour[e - 1]);
            if (row <= j)
            {
                break;
            }
            crd::i32 kind = 0;
            const crd::i32 lca = skeleton_leaf_i32(row, j, first, row_stamp, row_last_leaf, ancestor, &kind);
            colcount[j] += (kind != 0) ? 1 : 0;
            if (kind == 2)
            {
                --colcount[lca];
            }
        }
        if (parent[j] != -1)
        {
            ancestor[j] = parent[j]; // merge j into its parent's set
        }
    }
    for (crd::i32 v = 0; v < n; ++v)
    {
        if (parent[v] != -1)
        {
            colcount[parent[v]] += colcount[v];
        }
    }
}

// Column counts of chol(A^T A) without forming A^T A (Gilbert, Ng and Peyton 1994). `atp`/`ati` are A^T
// in CSR form (row i of A: its columns, ascending), `parent`/`post` the column etree and its postorder.
// The A^T A graph has one clique per row of A, so each row is filed under the column of its least
// postorder position and, when that column is reached, its members play the part of the neighbour list
// in the symmetric routine. `first` holds the inverse postorder, which is the stamp the leaf test needs
// here. Scratch lengths: colcount, ancestor, row_stamp, row_last_leaf, first = n; head = n + 1; next = m.
void counts_ata_i32(const crd::u32* atp, const crd::u32* ati, crd::i32 m, crd::i32 n, const crd::i32* parent,
                    const crd::i32* post, crd::i32* colcount, crd::i32* ancestor, crd::i32* row_stamp,
                    crd::i32* row_last_leaf, crd::i32* first, crd::i32* head, crd::i32* next)
{
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 v = post[k];
        first[v] = k;
        ancestor[v] = v;
        row_stamp[v] = -1;
        row_last_leaf[v] = -1;
        colcount[v] = 1; // weight of a leaf; every node that is some node's parent drops to 0 below
    }
    for (crd::i32 v = 0; v < n; ++v)
    {
        if (parent[v] != -1)
        {
            colcount[parent[v]] = 0;
        }
    }
    for (crd::i32 b = 0; b <= n; ++b)
    {
        head[b] = -1;
    }
    // File each row of A under the least postorder position among its columns (bucket n if it has none,
    // which the sweep below never visits).
    for (crd::i32 r = 0; r < m; ++r)
    {
        crd::i32 earliest = n;
        const crd::u32 row_end = atp[static_cast<crd::u32>(r) + 1];
        for (crd::u32 e = atp[static_cast<crd::u32>(r)]; e < row_end; ++e)
        {
            const crd::i32 pos = first[ati[e]];
            earliest = (pos < earliest) ? pos : earliest;
        }
        next[r] = head[earliest];
        head[earliest] = r;
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 j = post[k];
        if (parent[j] != -1)
        {
            --colcount[parent[j]];
        }
        for (crd::i32 r = head[k]; r != -1; r = next[r])
        {
            const crd::u32 row_end = atp[static_cast<crd::u32>(r) + 1];
            for (crd::u32 e = atp[static_cast<crd::u32>(r)]; e < row_end; ++e)
            {
                crd::i32 kind = 0;
                const crd::i32 lca = skeleton_leaf_i32(static_cast<crd::i32>(ati[e]), j, first, row_stamp,
                                                       row_last_leaf, ancestor, &kind);
                colcount[j] += (kind != 0) ? 1 : 0;
                if (kind == 2)
                {
                    --colcount[lca];
                }
            }
        }
        if (parent[j] != -1)
        {
            ancestor[j] = parent[j];
        }
    }
    for (crd::i32 v = 0; v < n; ++v)
    {
        if (parent[v] != -1)
        {
            colcount[parent[v]] += colcount[v];
        }
    }
}
} // namespace

crd::containers::Array<crd::u32> elimination_tree(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc)
{
    const AdjacencyGraph g = build_adjacency(pattern, alloc);
    const crd::u32 n = g.n;
    crd::containers::Array<crd::i32> parent(alloc);
    crd::containers::Array<crd::i32> ancestor(alloc);
    parent.resize(n);
    ancestor.resize(n);
    etree_i32(g, parent.data(), ancestor.data());

    crd::containers::Array<crd::u32> out(alloc);
    out.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        out[i] = (parent[i] == -1) ? kNoParent : static_cast<crd::u32>(parent[i]);
    }
    return out;
}

crd::containers::Array<crd::u32> column_elimination_tree(const sparse::SparsePattern& csc_pattern,
                                                         crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(csc_pattern.is_compressed(), "column_elimination_tree requires a compressed CSC pattern");
    CRD_ASSERT_MSG(csc_pattern.format == sparse::SparseFormat::Csc, "column_elimination_tree requires CSC");
    const crd::i32 m = static_cast<crd::i32>(csc_pattern.rows);
    const crd::i32 n = static_cast<crd::i32>(csc_pattern.cols);
    crd::containers::Array<crd::u32> out(alloc);
    out.resize(static_cast<crd::u32>(n));
    if (n == 0)
    {
        return out;
    }
    const crd::u32* ap = csc_pattern.outer_ptr.data();
    const crd::u32* ai = csc_pattern.inner_idx.data();
    crd::containers::Array<crd::i32> parent(alloc);
    crd::containers::Array<crd::i32> ancestor(alloc);
    crd::containers::Array<crd::i32> prev(alloc);
    parent.resize(static_cast<crd::u32>(n));
    ancestor.resize(static_cast<crd::u32>(n));
    prev.resize(static_cast<crd::u32>(m));
    etree_ata_i32(ap, ai, m, n, parent.data(), ancestor.data(), prev.data());
    for (crd::i32 j = 0; j < n; ++j)
    {
        out[static_cast<crd::usize>(j)] = (parent[j] == -1) ? kNoParent : static_cast<crd::u32>(parent[j]);
    }
    return out;
}

crd::containers::Array<crd::u32> column_counts_ata(const sparse::SparsePattern& csc_pattern,
                                                   crd::containers::ConstSpan<crd::u32> etree,
                                                   crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(csc_pattern.is_compressed(), "column_counts_ata requires a compressed CSC pattern");
    CRD_ASSERT_MSG(csc_pattern.format == sparse::SparseFormat::Csc, "column_counts_ata requires CSC");
    const crd::i32 m = static_cast<crd::i32>(csc_pattern.rows);
    const crd::i32 n = static_cast<crd::i32>(csc_pattern.cols);
    CRD_ASSERT_MSG(static_cast<crd::u32>(etree.size()) == csc_pattern.cols, "column_counts_ata: etree size mismatch");
    crd::containers::Array<crd::u32> out(alloc);
    out.resize(static_cast<crd::u32>(n));
    if (n == 0)
    {
        return out;
    }

    crd::containers::Array<crd::i32> parent(alloc);
    parent.resize(static_cast<crd::u32>(n));
    for (crd::i32 j = 0; j < n; ++j)
    {
        const crd::u32 p = etree[static_cast<crd::usize>(j)];
        parent[j] = (p == kNoParent) ? -1 : static_cast<crd::i32>(p);
    }

    // Postorder of the column etree (separate child-list scratch — head/next below are
    // repurposed by the ata count, so don't alias them).
    crd::containers::Array<crd::i32> post(alloc);
    crd::containers::Array<crd::i32> phead(alloc);
    crd::containers::Array<crd::i32> pnext(alloc);
    crd::containers::Array<crd::i32> stack(alloc);
    post.resize(static_cast<crd::u32>(n));
    phead.resize(static_cast<crd::u32>(n));
    pnext.resize(static_cast<crd::u32>(n));
    stack.resize(static_cast<crd::u32>(n));
    post_order_i32(parent.data(), n, post.data(), phead.data(), pnext.data(), stack.data());

    // Transpose B's CSC pattern → CSR (atp row pointers over A's rows, ati the columns,
    // ascending per row via counting sort). AᵀA is never formed; this is the only extra storage.
    const crd::u32* ap = csc_pattern.outer_ptr.data();
    const crd::u32* ai = csc_pattern.inner_idx.data();
    const crd::u32 nnz = ap[static_cast<crd::u32>(n)];
    crd::containers::Array<crd::u32> atp(alloc);
    crd::containers::Array<crd::u32> ati(alloc);
    crd::containers::Array<crd::u32> wp(alloc);
    atp.resize(static_cast<crd::u32>(m) + 1); // value-init 0
    for (crd::u32 p = 0; p < nnz; ++p)
    {
        ++atp[ai[p] + 1];
    }
    for (crd::i32 i = 0; i < m; ++i)
    {
        atp[static_cast<crd::u32>(i) + 1] += atp[static_cast<crd::u32>(i)];
    }
    ati.resize_uninitialized(nnz);
    wp.resize(static_cast<crd::u32>(m));
    for (crd::i32 i = 0; i < m; ++i)
    {
        wp[static_cast<crd::u32>(i)] = atp[static_cast<crd::u32>(i)];
    }
    for (crd::u32 col = 0; col < static_cast<crd::u32>(n); ++col) // ascending col ⇒ each row's list sorted
    {
        for (crd::u32 p = ap[col]; p < ap[col + 1]; ++p)
        {
            ati[wp[ai[p]]++] = col;
        }
    }

    crd::containers::Array<crd::i32> colcount(alloc);
    crd::containers::Array<crd::i32> ancestor(alloc);
    crd::containers::Array<crd::i32> row_stamp(alloc);
    crd::containers::Array<crd::i32> row_last_leaf(alloc);
    crd::containers::Array<crd::i32> first(alloc);
    crd::containers::Array<crd::i32> head(alloc);
    crd::containers::Array<crd::i32> next(alloc);
    colcount.resize(static_cast<crd::u32>(n));
    ancestor.resize(static_cast<crd::u32>(n));
    row_stamp.resize(static_cast<crd::u32>(n));
    row_last_leaf.resize(static_cast<crd::u32>(n));
    first.resize(static_cast<crd::u32>(n));
    head.resize(static_cast<crd::u32>(n) + 1);
    next.resize(static_cast<crd::u32>(m));
    counts_ata_i32(atp.data(), ati.data(), m, n, parent.data(), post.data(), colcount.data(), ancestor.data(),
                   row_stamp.data(), row_last_leaf.data(), first.data(), head.data(), next.data());

    for (crd::i32 j = 0; j < n; ++j)
    {
        out[static_cast<crd::usize>(j)] = static_cast<crd::u32>(colcount[j]);
    }
    return out;
}

crd::containers::Array<crd::u32> column_counts(const sparse::SparsePattern& pattern,
                                               crd::containers::ConstSpan<crd::u32> etree,
                                               crd::memory::IAllocator* alloc)
{
    const AdjacencyGraph g = build_adjacency(pattern, alloc);
    const crd::i32 n = static_cast<crd::i32>(g.n);
    CRD_ASSERT_MSG(static_cast<crd::u32>(etree.size()) == g.n, "column_counts: etree size mismatch");

    crd::containers::Array<crd::i32> parent(alloc);
    parent.resize(g.n);
    for (crd::i32 i = 0; i < n; ++i)
    {
        parent[i] = (etree[static_cast<crd::usize>(i)] == kNoParent)
                        ? -1
                        : static_cast<crd::i32>(etree[static_cast<crd::usize>(i)]);
    }

    crd::containers::Array<crd::i32> post(alloc);
    crd::containers::Array<crd::i32> head(alloc);
    crd::containers::Array<crd::i32> next(alloc);
    crd::containers::Array<crd::i32> stack(alloc);
    post.resize(g.n);
    head.resize(g.n);
    next.resize(g.n);
    stack.resize(g.n);
    post_order_i32(parent.data(), n, post.data(), head.data(), next.data(), stack.data());

    crd::containers::Array<crd::i32> colcount(alloc);
    crd::containers::Array<crd::i32> ancestor(alloc);
    crd::containers::Array<crd::i32> row_stamp(alloc);
    crd::containers::Array<crd::i32> row_last_leaf(alloc);
    crd::containers::Array<crd::i32> first(alloc);
    colcount.resize(g.n);
    ancestor.resize(g.n);
    row_stamp.resize(g.n);
    row_last_leaf.resize(g.n);
    first.resize(g.n);
    counts_i32(g, parent.data(), post.data(), colcount.data(), ancestor.data(), row_stamp.data(), row_last_leaf.data(),
               first.data());

    crd::containers::Array<crd::u32> out(alloc);
    out.resize(g.n);
    for (crd::i32 i = 0; i < n; ++i)
    {
        out[static_cast<crd::usize>(i)] = static_cast<crd::u32>(colcount[i]);
    }
    return out;
}

crd::u64 nnz_l(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc)
{
    const AdjacencyGraph g = build_adjacency(pattern, alloc);
    const crd::i32 n = static_cast<crd::i32>(g.n);
    if (n == 0)
    {
        return 0;
    }
    crd::containers::Array<crd::i32> parent(alloc);
    crd::containers::Array<crd::i32> ancestor(alloc);
    parent.resize(g.n);
    ancestor.resize(g.n);
    etree_i32(g, parent.data(), ancestor.data());

    crd::containers::Array<crd::i32> post(alloc);
    crd::containers::Array<crd::i32> head(alloc);
    crd::containers::Array<crd::i32> next(alloc);
    crd::containers::Array<crd::i32> stack(alloc);
    post.resize(g.n);
    head.resize(g.n);
    next.resize(g.n);
    stack.resize(g.n);
    post_order_i32(parent.data(), n, post.data(), head.data(), next.data(), stack.data());

    crd::containers::Array<crd::i32> colcount(alloc);
    crd::containers::Array<crd::i32> row_stamp(alloc);
    crd::containers::Array<crd::i32> row_last_leaf(alloc);
    crd::containers::Array<crd::i32> first(alloc);
    colcount.resize(g.n);
    row_stamp.resize(g.n);
    row_last_leaf.resize(g.n);
    first.resize(g.n);
    // counts_i32 reuses `ancestor` as scratch (overwrites the etree one — fine, etree done).
    counts_i32(g, parent.data(), post.data(), colcount.data(), ancestor.data(), row_stamp.data(), row_last_leaf.data(),
               first.data());

    crd::u64 total = 0;
    for (crd::i32 i = 0; i < n; ++i)
    {
        total += static_cast<crd::u64>(colcount[i]);
    }
    return total;
}

crd::u32 bandwidth(const sparse::SparsePattern& pattern) noexcept
{
    const crd::u32* outer = pattern.outer_ptr.data();
    const crd::u32* inner = pattern.inner_idx.data();
    crd::u32 bw = 0;
    for (crd::u32 i = 0; i < pattern.rows; ++i)
    {
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const crd::u32 j = inner[k];
            const crd::u32 d = (i > j) ? (i - j) : (j - i);
            if (d > bw)
            {
                bw = d;
            }
        }
    }
    return bw;
}

crd::u64 profile(const sparse::SparsePattern& pattern) noexcept
{
    const crd::u32* outer = pattern.outer_ptr.data();
    const crd::u32* inner = pattern.inner_idx.data();
    crd::u64 prof = 0;
    for (crd::u32 i = 0; i < pattern.rows; ++i)
    {
        crd::u32 minj = i;
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const crd::u32 j = inner[k];
            if (j < minj)
            {
                minj = j;
            }
        }
        prof += (i - minj);
    }
    return prof;
}

crd::containers::Array<crd::u32> postorder(crd::containers::ConstSpan<crd::u32> etree, crd::memory::IAllocator* alloc)
{
    const crd::i32 n = static_cast<crd::i32>(etree.size());
    crd::containers::Array<crd::i32> parent(alloc);
    parent.resize(static_cast<crd::u32>(n));
    for (crd::i32 i = 0; i < n; ++i)
    {
        const crd::u32 p = etree[static_cast<crd::usize>(i)];
        parent[i] = (p == kNoParent) ? -1 : static_cast<crd::i32>(p);
    }

    crd::containers::Array<crd::i32> post(alloc);
    crd::containers::Array<crd::i32> head(alloc);
    crd::containers::Array<crd::i32> next(alloc);
    crd::containers::Array<crd::i32> stack(alloc);
    post.resize(static_cast<crd::u32>(n));
    head.resize(static_cast<crd::u32>(n));
    next.resize(static_cast<crd::u32>(n));
    stack.resize(static_cast<crd::u32>(n));
    post_order_i32(parent.data(), n, post.data(), head.data(), next.data(), stack.data());

    crd::containers::Array<crd::u32> out(alloc);
    out.resize(static_cast<crd::u32>(n));
    for (crd::i32 i = 0; i < n; ++i)
    {
        out[static_cast<crd::usize>(i)] = static_cast<crd::u32>(post[i]);
    }
    return out;
}

SymbolicFactor symbolic_factorize_ata(const sparse::SparsePattern& a, crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(a.is_compressed(), "symbolic_factorize_ata requires a compressed CSC pattern");
    CRD_ASSERT_MSG(a.format == sparse::SparseFormat::Csc, "symbolic_factorize_ata requires CSC");
    const crd::i32 m = static_cast<crd::i32>(a.rows);
    const crd::i32 n = static_cast<crd::i32>(a.cols);
    SymbolicFactor out(alloc);
    out.n = static_cast<crd::u32>(n);
    if (n == 0)
    {
        out.lp.push_back(0U); // empty factor: lp = {0}, nnz() == 0
        return out;
    }

    // --- etree + column counts of chol(AᵀA) WITHOUT forming AᵀA (reuse the proven implicit
    // column functions: column_elimination_tree == symbolic_factorize(AᵀA).parent and
    // column_counts_ata == its colcount, both validated bit-identical at v5c-1a). ---------------
    crd::containers::Array<crd::u32> etree_u = column_elimination_tree(a, alloc);
    crd::containers::Array<crd::u32> colcount_u = column_counts_ata(a, {etree_u.data(), etree_u.size()}, alloc);

    crd::containers::Array<crd::i32> parent(alloc);
    crd::containers::Array<crd::i32> colcount(alloc);
    parent.resize(static_cast<crd::u32>(n));
    colcount.resize(static_cast<crd::u32>(n));
    for (crd::i32 j = 0; j < n; ++j)
    {
        const crd::u32 p = etree_u[static_cast<crd::usize>(j)];
        parent[j] = (p == kNoParent) ? -1 : static_cast<crd::i32>(p);
        colcount[j] = static_cast<crd::i32>(colcount_u[static_cast<crd::usize>(j)]);
    }

    // postorder (post_order_i32 — the SAME helper symbolic_factorize uses ⇒ bit-identical `post`).
    crd::containers::Array<crd::i32> post(alloc);
    crd::containers::Array<crd::i32> head(alloc);
    crd::containers::Array<crd::i32> next(alloc);
    crd::containers::Array<crd::i32> stack(alloc);
    post.resize(static_cast<crd::u32>(n));
    head.resize(static_cast<crd::u32>(n));
    next.resize(static_cast<crd::u32>(n));
    stack.resize(static_cast<crd::u32>(n));
    post_order_i32(parent.data(), n, post.data(), head.data(), next.data(), stack.data());

    // L column pointers from the prefix sum of the column counts.
    out.lp.resize(static_cast<crd::u32>(n) + 1U);
    out.lp[0] = 0U;
    for (crd::i32 j = 0; j < n; ++j)
    {
        out.lp[static_cast<crd::usize>(j) + 1] =
            out.lp[static_cast<crd::usize>(j)] + static_cast<crd::u32>(colcount[j]);
    }

    // fundamental supernodes (graph-agnostic — a pure function of parent + colcount).
    crd::containers::Array<crd::i32> nchild(alloc);
    nchild.resize(static_cast<crd::u32>(n));
    out.nsuper = fundamental_supernodes_i32(parent.data(), colcount.data(), n, out.super, nchild.data());
    const crd::u32 nf = out.nsuper;

    // === Supernodal leading-column patterns (slead) — the QR-front column structure. Same recurrence
    // symbolic_factorize uses: R(g) = {fc} ∪ adj_AᵀA(fc)[>fc] ∪ (∪ child fund-supernode h: R(h)[>fc]).
    // adj_AᵀA(fc) — the columns sharing a row of A with fc — is gathered IMPLICITLY from A (CSC column
    // fc gives the rows i; CSR row i gives the sharing columns), never forming AᵀA. The gathered set
    // (sorted ascending) is bit-identical to build_adjacency(ata_pattern(A))'s adjacency of fc, so the
    // emitted slead is bit-for-bit the supernodal_patterns=true output of the explicit path (oracle). ===

    // CSR of A (row -> columns ascending) via counting sort (rows accessed by the adj gather).
    const crd::u32* ap = a.outer_ptr.data();
    const crd::u32* ai = a.inner_idx.data();
    const crd::u32 nnz = ap[static_cast<crd::u32>(n)];
    crd::containers::Array<crd::u32> arp(alloc);
    arp.resize(static_cast<crd::u32>(m) + 1); // value-init 0
    for (crd::u32 p = 0; p < nnz; ++p)
    {
        ++arp[ai[p] + 1];
    }
    for (crd::i32 i = 0; i < m; ++i)
    {
        arp[static_cast<crd::u32>(i) + 1] += arp[static_cast<crd::u32>(i)];
    }
    crd::containers::Array<crd::u32> aci(alloc);
    aci.resize(nnz);
    {
        crd::containers::Array<crd::u32> rcur(alloc);
        rcur.resize(static_cast<crd::u32>(m));
        for (crd::i32 i = 0; i < m; ++i)
        {
            rcur[static_cast<crd::u32>(i)] = arp[static_cast<crd::u32>(i)];
        }
        for (crd::u32 col = 0; col < static_cast<crd::u32>(n); ++col) // ascending col ⇒ each row sorted
        {
            for (crd::u32 p = ap[col]; p < ap[col + 1]; ++p)
            {
                aci[rcur[ai[p]]++] = col;
            }
        }
    }

    // col -> fundamental supernode + per-supernode child list (parent[last_col(h)] is h's merge point).
    crd::containers::Array<crd::i32> col_fs(alloc);
    crd::containers::Array<crd::i32> chead(alloc);
    crd::containers::Array<crd::i32> cnext(alloc);
    col_fs.resize(static_cast<crd::u32>(n));
    chead.resize(nf);
    cnext.resize(nf);
    for (crd::u32 ff = 0; ff < nf; ++ff)
    {
        for (crd::u32 c = out.super[ff]; c < out.super[ff + 1]; ++c)
        {
            col_fs[c] = static_cast<crd::i32>(ff);
        }
        chead[ff] = -1;
    }
    for (crd::u32 h = 0; h < nf; ++h)
    {
        const crd::u32 last_h = out.super[h + 1] - 1;
        const crd::i32 p = parent[last_h];
        if (p != -1)
        {
            const crd::i32 pf = col_fs[p];
            cnext[h] = chead[pf];
            chead[pf] = static_cast<crd::i32>(h);
        }
    }

    crd::u64 lead_total = 0;
    for (crd::u32 ff = 0; ff < nf; ++ff)
    {
        const crd::u32 fc = out.super[ff];
        lead_total += static_cast<crd::u64>(out.lp[fc + 1] - out.lp[fc]);
    }
    out.slead_ptr.resize(static_cast<crd::usize>(nf) + 1);
    out.slead_ptr[0] = 0;
    out.slead_idx.reserve(lead_total);

    crd::containers::Array<crd::u32> mark(alloc); // adj-gather marker; stamp = fc (distinct per supernode)
    mark.resize(static_cast<crd::u32>(n));
    for (crd::i32 j = 0; j < n; ++j)
    {
        mark[static_cast<crd::u32>(j)] = static_cast<crd::u32>(n); // n: impossible column id
    }
    crd::containers::Array<crd::u32> acc(alloc);
    crd::containers::Array<crd::u32> tmp(alloc);
    crd::containers::Array<crd::u32> adjbuf(alloc);
    for (crd::u32 gg = 0; gg < nf; ++gg)
    {
        const crd::u32 fc = out.super[gg];
        // implicit adj_AᵀA(fc)[>fc]: columns > fc sharing a row of A with fc.
        adjbuf.clear();
        for (crd::u32 p = ap[fc]; p < ap[fc + 1]; ++p)
        {
            const crd::u32 i = ai[p]; // a row of A in which column fc is nonzero
            for (crd::u32 q = arp[i]; q < arp[i + 1]; ++q)
            {
                const crd::u32 k = aci[q]; // a column sharing row i with fc
                if (k > fc && mark[k] != fc)
                {
                    mark[k] = fc;
                    adjbuf.push_back(k);
                }
            }
        }
        crd::containers::sort(adjbuf.data(), adjbuf.data() + adjbuf.size());

        acc.clear();
        acc.push_back(fc); // diagonal first
        for (crd::u32 t = 0; t < adjbuf.size(); ++t)
        {
            acc.push_back(adjbuf[t]);
        }
        for (crd::i32 h = chead[gg]; h != -1; h = cnext[h]) // merge each child's R(h)[>fc]
        {
            const crd::u32 re = out.slead_ptr[static_cast<crd::usize>(h) + 1];
            crd::u32 s = out.slead_ptr[static_cast<crd::usize>(h)];
            while (s < re && out.slead_idx[s] <= fc) // skip to the suffix > fc
            {
                ++s;
            }
            tmp.clear(); // 2-way sorted merge with dedup: acc ∪ slead_idx[s..re)
            crd::u32 i = 0;
            const crd::u32 na = static_cast<crd::u32>(acc.size());
            while (i < na && s < re)
            {
                const crd::u32 va = acc[i];
                const crd::u32 vb = out.slead_idx[s];
                if (va < vb)
                {
                    tmp.push_back(va);
                    ++i;
                }
                else if (va > vb)
                {
                    tmp.push_back(vb);
                    ++s;
                }
                else
                {
                    tmp.push_back(va);
                    ++i;
                    ++s;
                }
            }
            while (i < na)
            {
                tmp.push_back(acc[i++]);
            }
            while (s < re)
            {
                tmp.push_back(out.slead_idx[s++]);
            }
            acc.clear(); // copy-back (Array has no swap; child counts are small)
            for (crd::u32 t = 0; t < tmp.size(); ++t)
            {
                acc.push_back(tmp[t]);
            }
        }
        CRD_ASSERT_MSG(static_cast<crd::u32>(acc.size()) == out.lp[fc + 1] - out.lp[fc],
                       "symbolic_factorize_ata: supernodal leading pattern size != colcount");
        for (crd::u32 t = 0; t < acc.size(); ++t)
        {
            out.slead_idx.push_back(acc[t]);
        }
        out.slead_ptr[static_cast<crd::usize>(gg) + 1] = static_cast<crd::u32>(out.slead_idx.size());
    }

    // --- export parent / post / colcount as u32 (li stays empty: supernodal-only) --------------
    out.parent.resize(static_cast<crd::u32>(n));
    out.post.resize(static_cast<crd::u32>(n));
    out.colcount.resize(static_cast<crd::u32>(n));
    for (crd::i32 j = 0; j < n; ++j)
    {
        out.parent[static_cast<crd::usize>(j)] = (parent[j] == -1) ? kNoParent : static_cast<crd::u32>(parent[j]);
        out.post[static_cast<crd::usize>(j)] = static_cast<crd::u32>(post[j]);
        out.colcount[static_cast<crd::usize>(j)] = static_cast<crd::u32>(colcount[j]);
    }
    return out;
}

SymbolicFactor symbolic_factorize(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc,
                                  bool supernodal_patterns)
{
    const AdjacencyGraph g = build_adjacency(pattern, alloc);
    const crd::i32 n = static_cast<crd::i32>(g.n);
    SymbolicFactor out(alloc);
    out.n = g.n;
    if (n == 0)
    {
        out.lp.push_back(0U); // empty factor: lp = {0}, nnz() == 0
        return out;
    }

    // --- etree + postorder ---------------------------------------------
    crd::containers::Array<crd::i32> parent(alloc);
    crd::containers::Array<crd::i32> ancestor(alloc);
    parent.resize(g.n);
    ancestor.resize(g.n);
    etree_i32(g, parent.data(), ancestor.data());

    crd::containers::Array<crd::i32> post(alloc);
    crd::containers::Array<crd::i32> head(alloc);
    crd::containers::Array<crd::i32> next(alloc);
    crd::containers::Array<crd::i32> stack(alloc);
    post.resize(g.n);
    head.resize(g.n);
    next.resize(g.n);
    stack.resize(g.n);
    post_order_i32(parent.data(), n, post.data(), head.data(), next.data(), stack.data());

    // --- column counts ---------------------------------------------------
    crd::containers::Array<crd::i32> colcount(alloc);
    crd::containers::Array<crd::i32> row_stamp(alloc);
    crd::containers::Array<crd::i32> row_last_leaf(alloc);
    crd::containers::Array<crd::i32> first(alloc);
    colcount.resize(g.n);
    row_stamp.resize(g.n);
    row_last_leaf.resize(g.n);
    first.resize(g.n);
    // counts_i32 reuses `ancestor` as scratch (the etree walk is done with it).
    counts_i32(g, parent.data(), post.data(), colcount.data(), ancestor.data(), row_stamp.data(), row_last_leaf.data(),
               first.data());

    // --- L column pointers from the prefix sum of the column counts -------
    out.lp.resize(g.n + 1U);
    out.lp[0] = 0U;
    for (crd::i32 j = 0; j < n; ++j)
    {
        out.lp[static_cast<crd::usize>(j) + 1] =
            out.lp[static_cast<crd::usize>(j)] + static_cast<crd::u32>(colcount[j]);
    }

    // --- fundamental supernodes (reuse `row_stamp` as the nchild scratch) --
    // Computed BEFORE the row-pattern build so the supernodal path can build per-supernode leading
    // patterns directly. `row_stamp` is free here (counts_i32 done).
    out.nsuper = fundamental_supernodes_i32(parent.data(), colcount.data(), n, out.super, row_stamp.data());

    if (supernodal_patterns)
    {
        // === Supernodal path: per-fundamental-supernode LEADING-column patterns via the assembly-tree
        // union, skipping the O(nnz(L)) full `li`. For fundamental supernode g (leading column fc):
        //   R(g) = {fc} ∪ adj(fc)[>fc] ∪ (∪ child fund supernode h: R(h) entries > fc).
        // A fundamental supernode's interior columns have a single etree child (the chain predecessor),
        // so EVERY external child supernode connects at the LEADING column fc (the merge point) — thus
        // children of g = { h : parent[last_col(h)] == fc }, and the child's contribution is pattern(
        // last_col(h))[>fc] = R(h)[>fc]. Processed g=0..nf-1 (column order == postorder ⇒ children
        // done first). R(g) is bit-identical to li[lp[fc]..lp[fc+1]] (ascending, diagonal first) ⇒ the
        // supernodal symbolic + numeric factor are unchanged. Reuses free i32 scratch (the li build is
        // skipped): `first`=col→fund-supernode, `head`=child-list head, `stack`=child-list next.
        const crd::u32 nf = out.nsuper;
        crd::i32* col_fs = first.data(); // column -> fundamental supernode
        for (crd::u32 ff = 0; ff < nf; ++ff)
        {
            for (crd::u32 c = out.super[ff]; c < out.super[ff + 1]; ++c)
            {
                col_fs[c] = static_cast<crd::i32>(ff);
            }
        }
        crd::i32* chead = head.data();  // per-supernode child-list head
        crd::i32* cnext = stack.data(); // per-supernode child-list next
        for (crd::u32 ff = 0; ff < nf; ++ff)
        {
            chead[ff] = -1;
        }
        for (crd::u32 h = 0; h < nf; ++h)
        {
            const crd::u32 last_h = out.super[h + 1] - 1;
            const crd::i32 p = parent[last_h]; // etree parent of h's top column (== leading col of pf)
            if (p != -1)
            {
                const crd::i32 pf = col_fs[p];
                cnext[h] = chead[pf];
                chead[pf] = static_cast<crd::i32>(h);
            }
        }
        // Exact total = Σ leading colcounts (each R(g) has colcount(fc) entries) ⇒ reserve, no realloc.
        crd::u64 lead_total = 0;
        for (crd::u32 ff = 0; ff < nf; ++ff)
        {
            const crd::u32 fc = out.super[ff];
            lead_total += static_cast<crd::u64>(out.lp[fc + 1] - out.lp[fc]);
        }
        out.slead_ptr.resize(static_cast<crd::usize>(nf) + 1);
        out.slead_ptr[0] = 0;
        out.slead_idx.reserve(lead_total);
        crd::containers::Array<crd::u32> acc(alloc);
        crd::containers::Array<crd::u32> tmp(alloc);
        for (crd::u32 gg = 0; gg < nf; ++gg)
        {
            const crd::u32 fc = out.super[gg];
            acc.clear();
            acc.push_back(fc);                                     // diagonal first
            for (crd::u32 p = g.xadj[fc]; p < g.xadj[fc + 1]; ++p) // adj(fc) entries > fc (already sorted)
            {
                if (g.adjncy[p] > fc)
                {
                    acc.push_back(g.adjncy[p]);
                }
            }
            for (crd::i32 h = chead[gg]; h != -1; h = cnext[h]) // merge each child's R(h)[>fc]
            {
                const crd::u32 re = out.slead_ptr[static_cast<crd::usize>(h) + 1];
                crd::u32 s = out.slead_ptr[static_cast<crd::usize>(h)];
                while (s < re && out.slead_idx[s] <= fc) // skip to the suffix > fc
                {
                    ++s;
                }
                tmp.clear(); // 2-way sorted merge with dedup: acc ∪ slead_idx[s..re)
                crd::u32 i = 0;
                const crd::u32 na = static_cast<crd::u32>(acc.size());
                while (i < na && s < re)
                {
                    const crd::u32 va = acc[i];
                    const crd::u32 vb = out.slead_idx[s];
                    if (va < vb)
                    {
                        tmp.push_back(va);
                        ++i;
                    }
                    else if (va > vb)
                    {
                        tmp.push_back(vb);
                        ++s;
                    }
                    else
                    {
                        tmp.push_back(va);
                        ++i;
                        ++s;
                    }
                }
                while (i < na)
                {
                    tmp.push_back(acc[i++]);
                }
                while (s < re)
                {
                    tmp.push_back(out.slead_idx[s++]);
                }
                acc.clear(); // copy-back (Array has no swap; child counts are small)
                for (crd::u32 t = 0; t < tmp.size(); ++t)
                {
                    acc.push_back(tmp[t]);
                }
            }
            CRD_ASSERT_MSG(static_cast<crd::u32>(acc.size()) == out.lp[fc + 1] - out.lp[fc],
                           "symbolic_factorize: supernodal leading pattern size != colcount");
            for (crd::u32 t = 0; t < acc.size(); ++t)
            {
                out.slead_idx.push_back(acc[t]);
            }
            out.slead_ptr[static_cast<crd::usize>(gg) + 1] = static_cast<crd::u32>(out.slead_idx.size());
        }
    }
    else
    {
        // === General path: full L row pattern, one row per step, into `li`. ====================
        // Reuse `first` as the per-column write cursor c[] (= a copy of lp), `stack` as the row-pattern
        // output stack, `head` as the zero-initialised row stamps.
        out.li.resize(out.lp[static_cast<crd::usize>(n)]);
        for (crd::i32 j = 0; j < n; ++j)
        {
            first[j] = static_cast<crd::i32>(out.lp[static_cast<crd::usize>(j)]); // cursor c[j]
            head[j] = 0;                                                          // no row stamped yet
        }
        for (crd::i32 k = 0; k < n; ++k)
        {
            const crd::i32 top = row_pattern_i32(g, k, parent.data(), stack.data(), head.data());
            for (crd::i32 p = top; p < n; ++p)
            {
                const crd::i32 i = stack[p]; // L(k,i) != 0 -> column i, row k
                out.li[static_cast<crd::usize>(first[i]++)] = static_cast<crd::u32>(k);
            }
            out.li[static_cast<crd::usize>(first[k]++)] = static_cast<crd::u32>(k); // diagonal L(k,k)
        }
        for (crd::i32 j = 0; j < n; ++j) // every column filled exactly colcount[j] entries
        {
            CRD_ASSERT_MSG(first[j] == static_cast<crd::i32>(out.lp[static_cast<crd::usize>(j) + 1]),
                           "symbolic_factorize: L column cursor mismatch (pattern/count disagree)");
        }
    }

    // --- export parent / post / colcount as u32 --------------------------
    out.parent.resize(g.n);
    out.post.resize(g.n);
    out.colcount.resize(g.n);
    for (crd::i32 j = 0; j < n; ++j)
    {
        out.parent[static_cast<crd::usize>(j)] = (parent[j] == -1) ? kNoParent : static_cast<crd::u32>(parent[j]);
        out.post[static_cast<crd::usize>(j)] = static_cast<crd::u32>(post[j]);
        out.colcount[static_cast<crd::usize>(j)] = static_cast<crd::u32>(colcount[j]);
    }
    return out;
}

} // namespace crd::hesap::ordering
