#include <crd/core/assert.hpp>
#include <crd/hesap/ordering/adjacency_graph.hpp>

namespace crd::hesap::ordering
{
// Build the symmetrised, sorted, dedup'd, diagonal-free adjacency A ∪ Aᵀ in O(nnz).
//
// The input SparsePattern is canonical-sorted (its invariant: USED indices ascending per inner
// vector), so A.row(v) is already sorted. We build Aᵀ via a counting sort — emitting each Aᵀ row in
// ascending source-row (i) order makes Aᵀ's rows sorted too — then each vertex's adjacency is the
// MERGE of two sorted lists (A.row(v) and Aᵀ.row(v) = A.col(v)) with dedup + diagonal skip. That
// replaces the old per-vertex introsort (O(nnz·log deg), a measured ~140-640 ms of the symbolic
// factorize at scale) with an O(nnz) counting-sort + linear merge. Assumption-free — correct for
// non-symmetric input too (this is shared with AMD), so NO symmetry shortcut. Produces the identical
// sorted/dedup'd graph as before ⇒ etree/counts/li/supernodes and the numeric factor are unchanged.
AdjacencyGraph build_adjacency(const sparse::SparsePattern& pat, crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(pat.is_compressed(), "build_adjacency requires a compressed pattern");
    CRD_ASSERT_MSG(pat.rows == pat.cols, "build_adjacency requires a square pattern");
    const crd::u32 n = pat.rows;

    AdjacencyGraph g(alloc);
    g.n = n;
    g.xadj.resize(static_cast<crd::usize>(n) + 1);
    g.xadj[0] = 0;
    if (n == 0)
    {
        return g;
    }

    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const crd::u32 nnz = outer[n];

    // --- transpose Aᵀ via counting sort (O(nnz); each Aᵀ row emitted ascending in i) ----------
    crd::containers::Array<crd::u32> tp(alloc); // Aᵀ row pointers (= A column pointers)
    tp.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 v = 0; v <= n; ++v)
    {
        tp[v] = 0;
    }
    for (crd::u32 k = 0; k < nnz; ++k)
    {
        ++tp[inner[k] + 1]; // count column degrees
    }
    for (crd::u32 v = 0; v < n; ++v)
    {
        tp[v + 1] += tp[v]; // prefix sum
    }
    crd::containers::Array<crd::u32> ti(alloc); // Aᵀ row indices (sorted within each row)
    ti.resize(nnz);
    {
        crd::containers::Array<crd::u32> pos(alloc);
        pos.resize(n);
        for (crd::u32 v = 0; v < n; ++v)
        {
            pos[v] = tp[v];
        }
        for (crd::u32 i = 0; i < n; ++i) // i ascending ⇒ each Aᵀ row comes out sorted
        {
            for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
            {
                ti[pos[inner[k]]++] = i;
            }
        }
    }

    // --- per-vertex merge of A.row(v) and Aᵀ.row(v): sorted union, dedup, skip diagonal --------
    g.adjncy.reserve(nnz); // exact for symmetric (≈ nnz − n); grows only for non-symmetric input
    for (crd::u32 v = 0; v < n; ++v)
    {
        crd::u32 a = outer[v];
        const crd::u32 ae = outer[v + 1];
        crd::u32 b = tp[v];
        const crd::u32 be = tp[v + 1];
        crd::u32 prev = 0xFFFFFFFFU;
        auto emit = [&](crd::u32 w)
        {
            if (w != v && w != prev)
            {
                g.adjncy.push_back(w);
                prev = w;
            }
        };
        while (a < ae && b < be)
        {
            const crd::u32 ja = inner[a];
            const crd::u32 jb = ti[b];
            if (ja < jb)
            {
                emit(ja);
                ++a;
            }
            else if (ja > jb)
            {
                emit(jb);
                ++b;
            }
            else
            {
                emit(ja);
                ++a;
                ++b;
            }
        }
        while (a < ae)
        {
            emit(inner[a++]);
        }
        while (b < be)
        {
            emit(ti[b++]);
        }
        g.xadj[v + 1] = static_cast<crd::u32>(g.adjncy.size());
    }
    return g;
}

} // namespace crd::hesap::ordering
