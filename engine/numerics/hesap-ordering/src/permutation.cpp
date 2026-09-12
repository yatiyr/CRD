#include <crd/hesap/ordering/permutation.hpp>

#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>

#include <utility>

namespace crd::hesap::ordering
{
void Permutation::rebuild_inverse()
{
    const crd::u32 n = static_cast<crd::u32>(perm.size());
    inv_perm.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        inv_perm[perm[i]] = i;
    }
}

sparse::SparsePattern apply_symmetric(const sparse::SparsePattern& pat, const Permutation& p,
                                      crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(pat.is_compressed(), "apply_symmetric requires a compressed pattern");
    CRD_ASSERT_MSG(pat.rows == pat.cols, "apply_symmetric requires a square pattern");
    const crd::u32 n = pat.rows;
    CRD_ASSERT_MSG(p.size() == n && p.inv_perm.size() == n, "apply_symmetric: permutation size mismatch");

    sparse::SparsePattern out(alloc);
    out.rows       = n;
    out.cols       = n;
    out.format     = pat.format;
    out.block_size = 1;
    out.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    out.outer_ptr[0] = 0;
    out.inner_idx.reserve(pat.inner_idx.size());

    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const crd::u32* perm  = p.perm.data();
    const crd::u32* invp  = p.inv_perm.data();

    crd::containers::Array<crd::u32> rowbuf(alloc);
    for (crd::u32 ni = 0; ni < n; ++ni)
    {
        const crd::u32 r = perm[ni];  // original row placed at new slot ni
        rowbuf.clear();
        for (crd::u32 k = outer[r]; k < outer[r + 1]; ++k)
        {
            rowbuf.push_back(invp[inner[k]]);  // original col -> new col
        }
        crd::containers::sort(rowbuf.data(), rowbuf.data() + rowbuf.size());
        for (crd::usize t = 0; t < rowbuf.size(); ++t)
        {
            out.inner_idx.push_back(rowbuf[t]);
        }
        out.outer_ptr[ni + 1] = static_cast<crd::u32>(out.inner_idx.size());
    }
    out.recompute_topology_hash();
    return out;
}

} // namespace crd::hesap::ordering
