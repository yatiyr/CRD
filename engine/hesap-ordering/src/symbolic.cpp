#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/ordering/adjacency_graph.hpp>
#include <crd/hesap/ordering/symbolic.hpp>

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
    const crd::i32 n = static_cast<crd::i32>(g.n);
    const crd::u32* xadj = g.xadj.data();
    const crd::u32* adj = g.adjncy.data();
    for (crd::i32 k = 0; k < n; ++k)
    {
        parent[k] = -1;
        ancestor[k] = -1;
        for (crd::u32 p = xadj[k]; p < xadj[k + 1]; ++p)
        {
            crd::i32 i = static_cast<crd::i32>(adj[p]);
            if (i >= k)
            {
                break; // adjacency ascending → no more below-diagonal (i<k) entries
            }
            // Walk i up the partial elimination tree (path-compress via ancestor).
            for (; i != -1 && i < k;)
            {
                const crd::i32 inext = ancestor[i];
                ancestor[i] = k;
                if (inext == -1)
                {
                    parent[i] = k;
                }
                i = inext;
            }
        }
    }
}

// cs_etree (ata = 1): column elimination tree = elimination tree of chol(AᵀA),
// computed WITHOUT forming AᵀA. A is the COMPRESSED CSC pattern (m rows × n cols):
// `ap`/`ai` are column pointers / row indices. `prev` (length m) tracks, per row, the
// last column that held a nonzero in that row; the matched entries pretend to be the
// AᵀA off-diagonals so the same path-compressed ancestor walk as the symmetric etree
// applies. parent[k] = -1 for roots. Faithful to Tim Davis's cs_etree.c ata branch.
void etree_ata_i32(const crd::u32* ap, const crd::u32* ai, crd::i32 m, crd::i32 n, crd::i32* parent, crd::i32* ancestor,
                   crd::i32* prev)
{
    for (crd::i32 i = 0; i < m; ++i)
    {
        prev[i] = -1;
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        parent[k] = -1;
        ancestor[k] = -1;
        for (crd::u32 p = ap[static_cast<crd::u32>(k)]; p < ap[static_cast<crd::u32>(k) + 1]; ++p)
        {
            crd::i32 i = prev[ai[p]]; // ata: previous column with a nonzero in row ai[p]
            for (; i != -1 && i < k;) // walk i up the partial column-etree (path-compress)
            {
                const crd::i32 inext = ancestor[i];
                ancestor[i] = k;
                if (inext == -1)
                {
                    parent[i] = k;
                }
                i = inext;
            }
            prev[ai[p]] = k;
        }
    }
}

// cs_tdfs: iterative depth-first postorder of node j; head/next are the child
// linked lists (built so children come out ascending → deterministic).
crd::i32 tdfs_i32(crd::i32 j, crd::i32 k, crd::i32* head, const crd::i32* next, crd::i32* post, crd::i32* stack)
{
    crd::i32 top = 0;
    stack[0] = j;
    while (top >= 0)
    {
        const crd::i32 p = stack[top];
        const crd::i32 i = head[p];
        if (i == -1)
        {
            --top;
            post[k++] = p; // all children done → emit p
        }
        else
        {
            head[p] = next[i]; // remove i from p's child list
            stack[++top] = i;
        }
    }
    return k;
}

// cs_post: postorder a forest given parent[]. post[] gets the ordering.
void post_order_i32(const crd::i32* parent, crd::i32 n, crd::i32* post, crd::i32* head, crd::i32* next, crd::i32* stack)
{
    for (crd::i32 j = 0; j < n; ++j)
    {
        head[j] = -1;
    }
    for (crd::i32 j = n - 1; j >= 0; --j) // reverse → children ascending in the lists
    {
        if (parent[j] == -1)
        {
            continue;
        }
        next[j] = head[parent[j]];
        head[parent[j]] = j;
    }
    crd::i32 k = 0;
    for (crd::i32 j = 0; j < n; ++j)
    {
        if (parent[j] != -1)
        {
            continue; // start a DFS only at roots
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
        return -1; // j not a leaf of T_i (relies on -1 == minimum: the i32 reason)
    }
    maxfirst[i] = first[j];
    const crd::i32 jprev = prevleaf[i];
    prevleaf[i] = j;
    *jleaf = (jprev == -1) ? 1 : 2;
    if (*jleaf == 1)
    {
        return i; // first leaf → LCA is i itself
    }
    crd::i32 q = jprev; // find LCA(jprev, j) via path-halving on ancestor
    for (; q != ancestor[q]; q = ancestor[q])
    {
    }
    for (crd::i32 s = jprev; s != q;)
    {
        const crd::i32 sparent = ancestor[s];
        ancestor[s] = q;
        s = sparent;
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
    const crd::i32 n = static_cast<crd::i32>(g.n);
    const crd::u32* xadj = g.xadj.data();
    const crd::u32* adj = g.adjncy.data();
    const auto flip = [](crd::i32 v)
    {
        return -v - 2;
    }; // CS_FLIP: involution, maps >=0 -> <0
    crd::i32 top = n;

    mark[k] = flip(mark[k]); // mark node k (its path terminates the upward walks)
    for (crd::u32 p = xadj[k]; p < xadj[k + 1]; ++p)
    {
        crd::i32 i = static_cast<crd::i32>(adj[p]);
        if (i > k)
        {
            break; // adjacency ascending + diagonal-free -> only i < k below here
        }
        crd::i32 len = 0;
        for (; mark[i] >= 0; i = parent[i]) // walk i up the etree until a marked node
        {
            s[len++] = i; // L(k,i) is nonzero
            mark[i] = flip(mark[i]);
        }
        while (len > 0)
        {
            s[--top] = s[--len]; // splice this root-path onto the output stack
        }
    }
    for (crd::i32 p = top; p < n; ++p)
    {
        mark[s[p]] = flip(mark[s[p]]); // unmark every path node
    }
    mark[k] = flip(mark[k]); // unmark node k -> `mark` fully restored
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
    const crd::i32 n = static_cast<crd::i32>(g.n);
    const crd::u32* xadj = g.xadj.data();
    const crd::u32* adj = g.adjncy.data();
    for (crd::i32 i = 0; i < n; ++i)
    {
        ancestor[i] = i;
        maxfirst[i] = -1;
        prevleaf[i] = -1;
        first[i] = -1;
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 j = post[k];
        colcount[j] = (first[j] == -1) ? 1 : 0; // delta[j]
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
            --colcount[parent[j]]; // j is not a root
        }
        for (crd::u32 p = xadj[j]; p < xadj[j + 1]; ++p)
        {
            const crd::i32 i = static_cast<crd::i32>(adj[p]);
            crd::i32 jleaf;
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
    for (crd::i32 j = 0; j < n; ++j) // sum children's counts into parents
    {
        if (parent[j] != -1)
        {
            colcount[parent[j]] += colcount[j];
        }
    }
}

// cs_counts (ata = 1) + init_ata: column counts of chol(AᵀA) WITHOUT forming AᵀA.
// `AT` is A transposed (CSR over A's rows: `atp` row pointers, `ati` the columns in each
// row, ascending), `parent`/`post` the column etree + its postorder. Faithful to Davis
// cs_counts.c ata branch: a row J of A contributes its column set as a clique into the
// count via the head/next row-merge (rows bucketed by their least-postorder column).
// `first` is the first-descendant marker in the first pass, then REPURPOSED as the
// inverse postorder (invpost) by init_ata, exactly as the reference. Scratch lengths:
// colcount/ancestor/maxfirst/prevleaf/first = n, head = n+1, next = m.
void counts_ata_i32(const crd::u32* atp, const crd::u32* ati, crd::i32 m, crd::i32 n, const crd::i32* parent,
                    const crd::i32* post, crd::i32* colcount, crd::i32* ancestor, crd::i32* maxfirst,
                    crd::i32* prevleaf, crd::i32* first, crd::i32* head, crd::i32* next)
{
    for (crd::i32 k = 0; k < n; ++k)
    {
        maxfirst[k] = -1;
        prevleaf[k] = -1;
        first[k] = -1;
    }
    for (crd::i32 k = 0; k < n; ++k) // delta[j] (leaf flag → colcount) + first-descendant first[]
    {
        const crd::i32 j = post[k];
        colcount[j] = (first[j] == -1) ? 1 : 0;
        for (crd::i32 jj = j; jj != -1 && first[jj] == -1; jj = parent[jj])
        {
            first[jj] = k;
        }
    }
    // init_ata: head[k] = linked list of A-rows whose least-postorder column is k. `first` is
    // REPURPOSED as invpost (post position of each column), the cs_leaf marker in the ata case.
    for (crd::i32 k = 0; k <= n; ++k)
    {
        head[k] = -1;
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        first[post[k]] = k; // invert post (overwrites first → invpost; matches the reference)
    }
    for (crd::i32 i = 0; i < m; ++i)
    {
        crd::i32 kmin = n; // empty row → bucket n (never processed by the k<n loop)
        for (crd::u32 p = atp[static_cast<crd::u32>(i)]; p < atp[static_cast<crd::u32>(i) + 1]; ++p)
        {
            const crd::i32 kk = first[ati[p]];
            if (kk < kmin)
            {
                kmin = kk;
            }
        }
        next[i] = head[kmin];
        head[kmin] = i;
    }
    for (crd::i32 i = 0; i < n; ++i)
    {
        ancestor[i] = i;
    }
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::i32 j = post[k];
        if (parent[j] != -1)
        {
            --colcount[parent[j]];
        }
        for (crd::i32 jrow = head[k]; jrow != -1; jrow = next[jrow]) // ata: rows in bucket k
        {
            for (crd::u32 p = atp[static_cast<crd::u32>(jrow)]; p < atp[static_cast<crd::u32>(jrow) + 1]; ++p)
            {
                const crd::i32 i = static_cast<crd::i32>(ati[p]);
                crd::i32 jleaf = 0;
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
        }
        if (parent[j] != -1)
        {
            ancestor[j] = parent[j];
        }
    }
    for (crd::i32 j = 0; j < n; ++j)
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
    crd::containers::Array<crd::i32> maxfirst(alloc);
    crd::containers::Array<crd::i32> prevleaf(alloc);
    crd::containers::Array<crd::i32> first(alloc);
    crd::containers::Array<crd::i32> head(alloc);
    crd::containers::Array<crd::i32> next(alloc);
    colcount.resize(static_cast<crd::u32>(n));
    ancestor.resize(static_cast<crd::u32>(n));
    maxfirst.resize(static_cast<crd::u32>(n));
    prevleaf.resize(static_cast<crd::u32>(n));
    first.resize(static_cast<crd::u32>(n));
    head.resize(static_cast<crd::u32>(n) + 1);
    next.resize(static_cast<crd::u32>(m));
    counts_ata_i32(atp.data(), ati.data(), m, n, parent.data(), post.data(), colcount.data(), ancestor.data(),
                   maxfirst.data(), prevleaf.data(), first.data(), head.data(), next.data());

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
    // CSparse-port functions: column_elimination_tree == symbolic_factorize(AᵀA).parent and
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

    // --- fundamental supernodes (reuse `maxfirst` as the nchild scratch) --
    // Computed BEFORE the row-pattern build so the supernodal path can build per-supernode leading
    // patterns directly. `maxfirst` is free here (counts_i32 done).
    out.nsuper = fundamental_supernodes_i32(parent.data(), colcount.data(), n, out.super, maxfirst.data());

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
        // === General path: full L row pattern (cs_ereach per column) into `li`. ====================
        // Reuse `first` as the per-column write cursor c[] (= a copy of lp), `stack` as the cs_ereach
        // output stack, `head` as the zero-initialised marker.
        out.li.resize(out.lp[static_cast<crd::usize>(n)]);
        for (crd::i32 j = 0; j < n; ++j)
        {
            first[j] = static_cast<crd::i32>(out.lp[static_cast<crd::usize>(j)]); // cursor c[j]
            head[j] = 0;                                                          // marker (CS_FLIP scratch)
        }
        for (crd::i32 k = 0; k < n; ++k)
        {
            const crd::i32 top = ereach_i32(g, k, parent.data(), stack.data(), head.data());
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
