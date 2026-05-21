#include <crd/hesap/ordering/adjacency_graph.hpp>

#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>

namespace crd::hesap::ordering
{
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

    // Upper-bound degrees: each off-diagonal entry (i,j) contributes to both i
    // and j (symmetrise A ∪ Aᵀ); duplicates removed by the sort+compact below.
    crd::containers::Array<crd::u32> cnt(alloc);
    cnt.resize(n);  // value-init 0
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const crd::u32 j = inner[k];
            if (j != i)
            {
                ++cnt[i];
                ++cnt[j];
            }
        }
    }
    crd::containers::Array<crd::u32> head(alloc);
    head.resize(static_cast<crd::usize>(n) + 1);
    head[0] = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        head[i + 1] = head[i] + cnt[i];
    }
    const crd::u32 ub = head[n];

    crd::containers::Array<crd::u32> tmp(alloc);
    tmp.resize(ub);
    crd::containers::Array<crd::u32> pos(alloc);
    pos.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        pos[i] = head[i];
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const crd::u32 j = inner[k];
            if (j != i)
            {
                tmp[pos[i]++] = j;
                tmp[pos[j]++] = i;
            }
        }
    }

    // Sort each vertex segment ascending (D(ord)-4) + dedup-compact into adjncy.
    g.adjncy.reserve(ub);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 b = head[i];
        const crd::u32 e = head[i + 1];
        crd::containers::sort(tmp.data() + b, tmp.data() + e);  // in-place introsort
        crd::u32 prev = 0xFFFFFFFFU;
        for (crd::u32 t = b; t < e; ++t)
        {
            const crd::u32 v = tmp[t];
            if (v != prev)
            {
                g.adjncy.push_back(v);
                prev = v;
            }
        }
        g.xadj[i + 1] = static_cast<crd::u32>(g.adjncy.size());
    }
    return g;
}

} // namespace crd::hesap::ordering
