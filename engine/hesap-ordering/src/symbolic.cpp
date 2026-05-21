#include <crd/hesap/ordering/symbolic.hpp>

#include <crd/hesap/ordering/adjacency_graph.hpp>

#include <crd/core/assert.hpp>

// -----------------------------------------------------------------------
// Symbolic Cholesky fill metrics, ported line-for-line from Tim Davis's
// CSparse (cs_etree.c / cs_post.c / cs_counts.c, "Direct Methods for Sparse
// Linear Systems", SIAM 2006). Kept in SIGNED i32 with -1 sentinels exactly as
// the reference: cs_counts' `first[j] <= maxfirst[i]` test relies on -1 being
// the MINIMUM value, which u32 0xFFFFFFFF is not. Convert to u32 only at the API
// boundary. n < 2^31 always (matrix dimension), so i32 is safe.
//
// Operates on the symmetrised AdjacencyGraph (full off-diagonal pattern, sorted
// ascending) — for a symmetric matrix the CSR/CSC patterns coincide, so the
// adjacency lists are exactly the "column j row indices" CSparse iterates.
// -----------------------------------------------------------------------

namespace crd::hesap::ordering
{
namespace
{
// cs_etree (ata = 0): elimination tree of chol(A). parent[k] = -1 for roots.
void etree_i32(const AdjacencyGraph& g, crd::i32* parent, crd::i32* ancestor)
{
    const crd::i32  n   = static_cast<crd::i32>(g.n);
    const crd::u32* xadj = g.xadj.data();
    const crd::u32* adj  = g.adjncy.data();
    for (crd::i32 k = 0; k < n; ++k)
    {
        parent[k]   = -1;
        ancestor[k] = -1;
        for (crd::u32 p = xadj[k]; p < xadj[k + 1]; ++p)
        {
            crd::i32 i = static_cast<crd::i32>(adj[p]);
            if (i >= k)
            {
                break;  // adjacency ascending → no more below-diagonal (i<k) entries
            }
            // Walk i up the partial elimination tree (path-compress via ancestor).
            for (; i != -1 && i < k;)
            {
                const crd::i32 inext = ancestor[i];
                ancestor[i]          = k;
                if (inext == -1)
                {
                    parent[i] = k;
                }
                i = inext;
            }
        }
    }
}

// cs_tdfs: iterative depth-first postorder of node j; head/next are the child
// linked lists (built so children come out ascending → deterministic).
crd::i32 tdfs_i32(crd::i32 j, crd::i32 k, crd::i32* head, const crd::i32* next, crd::i32* post, crd::i32* stack)
{
    crd::i32 top = 0;
    stack[0]     = j;
    while (top >= 0)
    {
        const crd::i32 p = stack[top];
        const crd::i32 i = head[p];
        if (i == -1)
        {
            --top;
            post[k++] = p;  // all children done → emit p
        }
        else
        {
            head[p]        = next[i];  // remove i from p's child list
            stack[++top]   = i;
        }
    }
    return k;
}

// cs_post: postorder a forest given parent[]. post[] gets the ordering.
void post_order_i32(const crd::i32* parent, crd::i32 n, crd::i32* post, crd::i32* head, crd::i32* next,
                    crd::i32* stack)
{
    for (crd::i32 j = 0; j < n; ++j)
    {
        head[j] = -1;
    }
    for (crd::i32 j = n - 1; j >= 0; --j)  // reverse → children ascending in the lists
    {
        if (parent[j] == -1)
        {
            continue;
        }
        next[j]         = head[parent[j]];
        head[parent[j]] = j;
    }
    crd::i32 k = 0;
    for (crd::i32 j = 0; j < n; ++j)
    {
        if (parent[j] != -1)
        {
            continue;  // start a DFS only at roots
        }
        k = tdfs_i32(j, k, head, next, post, stack);
    }
}

// cs_leaf: is j a leaf of the ith row subtree? Returns LCA q + jleaf flag
// (0 = not a leaf, 1 = first leaf, 2 = subsequent leaf).
crd::i32 leaf_i32(crd::i32 i, crd::i32 j, const crd::i32* first, crd::i32* maxfirst, crd::i32* prevleaf,
                  crd::i32* ancestor, crd::i32* jleaf)
{
    *jleaf = 0;
    if (i <= j || first[j] <= maxfirst[i])
    {
        return -1;  // j not a leaf of T_i (relies on -1 == minimum: the i32 reason)
    }
    maxfirst[i]          = first[j];
    const crd::i32 jprev = prevleaf[i];
    prevleaf[i]          = j;
    *jleaf               = (jprev == -1) ? 1 : 2;
    if (*jleaf == 1)
    {
        return i;  // first leaf → LCA is i itself
    }
    crd::i32 q = jprev;  // find LCA(jprev, j) via path-halving on ancestor
    for (; q != ancestor[q]; q = ancestor[q])
    {
    }
    for (crd::i32 s = jprev; s != q;)
    {
        const crd::i32 sparent = ancestor[s];
        ancestor[s]            = q;
        s                      = sparent;
    }
    return q;
}

// cs_ereach (ata = 0): nonzero pattern of ROW k of L. Returns `top`; on exit
// s[top..n-1] holds the column indices i < k with L(k,i) != 0, in topological
// (etree) order. `mark` is reusable scratch (CS_FLIP sign trick: a node is marked
// when its entry is negative); it is left fully restored to its entry values on
// return, so the SAME zero-initialised array is reused across all k.
crd::i32 ereach_i32(const AdjacencyGraph& g, crd::i32 k, const crd::i32* parent, crd::i32* s, crd::i32* mark)
{
    const crd::i32  n    = static_cast<crd::i32>(g.n);
    const crd::u32* xadj = g.xadj.data();
    const crd::u32* adj  = g.adjncy.data();
    const auto      flip = [](crd::i32 v) { return -v - 2; };  // CS_FLIP: involution, maps >=0 -> <0
    crd::i32        top  = n;

    mark[k] = flip(mark[k]);  // mark node k (its path terminates the upward walks)
    for (crd::u32 p = xadj[k]; p < xadj[k + 1]; ++p)
    {
        crd::i32 i = static_cast<crd::i32>(adj[p]);
        if (i > k)
        {
            break;  // adjacency ascending + diagonal-free -> only i < k below here
        }
        crd::i32 len = 0;
        for (; mark[i] >= 0; i = parent[i])  // walk i up the etree until a marked node
        {
            s[len++] = i;          // L(k,i) is nonzero
            mark[i]  = flip(mark[i]);
        }
        while (len > 0)
        {
            s[--top] = s[--len];  // splice this root-path onto the output stack
        }
    }
    for (crd::i32 p = top; p < n; ++p)
    {
        mark[s[p]] = flip(mark[s[p]]);  // unmark every path node
    }
    mark[k] = flip(mark[k]);  // unmark node k -> `mark` fully restored
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

// cs_counts (ata = 0): column counts of L. colcount[j] = nnz of column j of L
// (incl. diagonal). Sum == nnz(L).
void counts_i32(const AdjacencyGraph& g, const crd::i32* parent, const crd::i32* post, crd::i32* colcount,
                crd::i32* ancestor, crd::i32* maxfirst, crd::i32* prevleaf, crd::i32* first)
{
    const crd::i32  n    = static_cast<crd::i32>(g.n);
    const crd::u32* xadj = g.xadj.data();
    const crd::u32* adj  = g.adjncy.data();
    for (crd::i32 i = 0; i < n; ++i)
    {
        ancestor[i] = i;
        maxfirst[i] = -1;
        prevleaf[i] = -1;
        first[i]    = -1;
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 j = post[k];
        colcount[j]      = (first[j] == -1) ? 1 : 0;  // delta[j]
        for (crd::i32 jj = j; jj != -1 && first[jj] == -1; jj = parent[jj])
        {
            first[jj] = k;
        }
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 j = post[k];
        if (parent[j] != -1)
        {
            --colcount[parent[j]];  // j is not a root
        }
        for (crd::u32 p = xadj[j]; p < xadj[j + 1]; ++p)
        {
            const crd::i32 i = static_cast<crd::i32>(adj[p]);
            crd::i32       jleaf;
            const crd::i32 q = leaf_i32(i, j, first, maxfirst, prevleaf, ancestor, &jleaf);
            if (jleaf >= 1)
            {
                ++colcount[j];
            }
            if (jleaf == 2)
            {
                --colcount[q];
            }
        }
        if (parent[j] != -1)
        {
            ancestor[j] = parent[j];
        }
    }
    for (crd::i32 j = 0; j < n; ++j)  // sum children's counts into parents
    {
        if (parent[j] != -1)
        {
            colcount[parent[j]] += colcount[j];
        }
    }
}
} // namespace

crd::containers::Array<crd::u32> elimination_tree(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc)
{
    const AdjacencyGraph g = build_adjacency(pattern, alloc);
    const crd::u32       n = g.n;
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

crd::containers::Array<crd::u32> column_counts(const sparse::SparsePattern& pattern,
                                               crd::containers::ConstSpan<crd::u32> etree,
                                               crd::memory::IAllocator* alloc)
{
    const AdjacencyGraph g = build_adjacency(pattern, alloc);
    const crd::i32       n = static_cast<crd::i32>(g.n);
    CRD_ASSERT_MSG(static_cast<crd::u32>(etree.size()) == g.n, "column_counts: etree size mismatch");

    crd::containers::Array<crd::i32> parent(alloc);
    parent.resize(g.n);
    for (crd::i32 i = 0; i < n; ++i)
    {
        parent[i] = (etree[static_cast<crd::usize>(i)] == kNoParent) ? -1 : static_cast<crd::i32>(etree[static_cast<crd::usize>(i)]);
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
    crd::containers::Array<crd::i32> maxfirst(alloc);
    crd::containers::Array<crd::i32> prevleaf(alloc);
    crd::containers::Array<crd::i32> first(alloc);
    colcount.resize(g.n);
    ancestor.resize(g.n);
    maxfirst.resize(g.n);
    prevleaf.resize(g.n);
    first.resize(g.n);
    counts_i32(g, parent.data(), post.data(), colcount.data(), ancestor.data(), maxfirst.data(), prevleaf.data(),
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
    const crd::i32       n = static_cast<crd::i32>(g.n);
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
    crd::containers::Array<crd::i32> maxfirst(alloc);
    crd::containers::Array<crd::i32> prevleaf(alloc);
    crd::containers::Array<crd::i32> first(alloc);
    colcount.resize(g.n);
    maxfirst.resize(g.n);
    prevleaf.resize(g.n);
    first.resize(g.n);
    // counts_i32 reuses `ancestor` as scratch (overwrites the etree one — fine, etree done).
    counts_i32(g, parent.data(), post.data(), colcount.data(), ancestor.data(), maxfirst.data(), prevleaf.data(),
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
    crd::u32        bw    = 0;
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
    crd::u64        prof  = 0;
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
        parent[i]        = (p == kNoParent) ? -1 : static_cast<crd::i32>(p);
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

SymbolicFactor symbolic_factorize(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc)
{
    const AdjacencyGraph g = build_adjacency(pattern, alloc);
    const crd::i32       n = static_cast<crd::i32>(g.n);
    SymbolicFactor       out(alloc);
    out.n = g.n;
    if (n == 0)
    {
        out.lp.push_back(0U);  // empty factor: lp = {0}, nnz() == 0
        return out;
    }

    // --- etree (cs_etree) + postorder (cs_post) ---------------------------
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

    // --- column counts (cs_counts) ---------------------------------------
    crd::containers::Array<crd::i32> colcount(alloc);
    crd::containers::Array<crd::i32> maxfirst(alloc);
    crd::containers::Array<crd::i32> prevleaf(alloc);
    crd::containers::Array<crd::i32> first(alloc);
    colcount.resize(g.n);
    maxfirst.resize(g.n);
    prevleaf.resize(g.n);
    first.resize(g.n);
    // counts_i32 reuses `ancestor` as scratch (the etree walk is done with it).
    counts_i32(g, parent.data(), post.data(), colcount.data(), ancestor.data(), maxfirst.data(), prevleaf.data(),
               first.data());

    // --- L column pointers from the prefix sum of the column counts -------
    out.lp.resize(g.n + 1U);
    out.lp[0] = 0U;
    for (crd::i32 j = 0; j < n; ++j)
    {
        out.lp[static_cast<crd::usize>(j) + 1] =
            out.lp[static_cast<crd::usize>(j)] + static_cast<crd::u32>(colcount[j]);
    }
    out.li.resize(out.lp[static_cast<crd::usize>(n)]);

    // --- full L row pattern (cs_ereach per column) ------------------------
    // Reuse `first` as the per-column write cursor c[] (= a copy of lp), `stack`
    // as the cs_ereach output stack, `head` as the zero-initialised marker.
    for (crd::i32 j = 0; j < n; ++j)
    {
        first[j] = static_cast<crd::i32>(out.lp[static_cast<crd::usize>(j)]);  // cursor c[j]
        head[j]  = 0;                                                          // marker (CS_FLIP scratch)
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 top = ereach_i32(g, k, parent.data(), stack.data(), head.data());
        for (crd::i32 p = top; p < n; ++p)
        {
            const crd::i32 i                          = stack[p];  // L(k,i) != 0 -> column i, row k
            out.li[static_cast<crd::usize>(first[i]++)] = static_cast<crd::u32>(k);
        }
        out.li[static_cast<crd::usize>(first[k]++)] = static_cast<crd::u32>(k);  // diagonal L(k,k)
    }
    for (crd::i32 j = 0; j < n; ++j)  // every column filled exactly colcount[j] entries
    {
        CRD_ASSERT_MSG(first[j] == static_cast<crd::i32>(out.lp[static_cast<crd::usize>(j) + 1]),
                       "symbolic_factorize: L column cursor mismatch (pattern/count disagree)");
    }

    // --- fundamental supernodes (reuse `maxfirst` as the nchild scratch) --
    out.nsuper = fundamental_supernodes_i32(parent.data(), colcount.data(), n, out.super, maxfirst.data());

    // --- export parent / post / colcount as u32 --------------------------
    out.parent.resize(g.n);
    out.post.resize(g.n);
    out.colcount.resize(g.n);
    for (crd::i32 j = 0; j < n; ++j)
    {
        out.parent[static_cast<crd::usize>(j)]   = (parent[j] == -1) ? kNoParent : static_cast<crd::u32>(parent[j]);
        out.post[static_cast<crd::usize>(j)]     = static_cast<crd::u32>(post[j]);
        out.colcount[static_cast<crd::usize>(j)] = static_cast<crd::u32>(colcount[j]);
    }
    return out;
}

} // namespace crd::hesap::ordering
