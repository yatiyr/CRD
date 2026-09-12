#include <crd/hesap/dense/interp_decomp.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/qr_colpiv.hpp>

namespace crd::hesap::dense
{
template <typename T, Layout L>
InterpDecomp<T, L> interp_decomp(crd::memory::IAllocator* alloc, const Matrix<T, L>& a, RealType<T> rcond,
                                 crd::usize max_rank)
{
    static_assert(L == Layout::RowMajor, "interp_decomp currently supports RowMajor only");
    static_assert(!is_complex_v<T>, "interp_decomp is real-only (v5e-1a; complex routes via SVD later)");

    const crd::usize m = a.rows();
    const crd::usize n = a.cols();

    InterpDecomp<T, L> id(alloc);
    id.m = m;
    id.n = n;

    // Column-pivoting QR reveals the numerical rank. A·P = Q·R with
    // |R[0,0]| >= |R[1,1]| >= ... and `jpvt[k]` the original column behind
    // permuted column k.
    QRColPiv<T, L> qr(alloc, m, n);
    factor_qr_colpiv<T, L>(qr, a, rcond);

    crd::usize r = qr.rank();
    if (max_rank > 0 && r > max_rank)
    {
        r = max_rank;  // HSS adaptive-rank / block-size cap.
    }
    id.rank = r;

    const auto& jpvt = qr.jpvt();
    const Matrix<T, L>& packed = qr.packed();  // R in the upper triangle, permuted column order.

    // Skeleton J = the first `r` pivot columns.
    id.skeleton.resize(r);
    for (crd::usize s = 0; s < r; ++s)
    {
        id.skeleton[s] = jpvt[s];
    }

    // cols (m × r) = A[:, J] gathered from the ORIGINAL A (self-contained
    // reconstruction: A ≈ cols·proj without re-reading A).
    id.cols = Matrix<T, L>(alloc, m, r);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize s = 0; s < r; ++s)
        {
            id.cols.at(i, s) = a.at(i, jpvt[s]);
        }
    }

    // proj (r × n), zero-initialised. Column `jpvt[j]` of proj is column j of
    // the permuted interpolation matrix Z_perm = [ I_r | T ], where
    // T = R11^{-1}·R12 (r × (n-r)). For j < r the permuted column is the unit
    // vector e_j; for j >= r it is T[:, j-r].
    id.proj = Matrix<T, L>(alloc, r, n);  // r==0 ⇒ 0×n empty (no scatter below runs).

    // j < r: the identity block.
    for (crd::usize j = 0; j < r; ++j)
    {
        id.proj.at(j, jpvt[j]) = T{1};
    }

    // j >= r: solve R11·t = R12[:, j-r] by back-substitution (R11 upper-tri,
    // diagonal nonzero for i < r by the rank threshold), scatter into proj.
    // packed.at(i, jj) is R11 for jj < r, R12 for jj >= r (permuted columns).
    crd::containers::Array<T> tcol(alloc);
    tcol.resize(r);
    for (crd::usize j = r; j < n; ++j)
    {
        for (crd::usize ii = r; ii-- > 0;)
        {
            T s = packed.at(ii, j);  // R12[ii, j-r]
            for (crd::usize jj = ii + 1; jj < r; ++jj)
            {
                s -= packed.at(ii, jj) * tcol[jj];
            }
            tcol[ii] = s / packed.at(ii, ii);
        }
        const crd::usize orig = jpvt[j];
        for (crd::usize ii = 0; ii < r; ++ii)
        {
            id.proj.at(ii, orig) = tcol[ii];
        }
    }

    return id;
}

template InterpDecomp<float, Layout::RowMajor> interp_decomp<float, Layout::RowMajor>(
    crd::memory::IAllocator*, const Matrix<float, Layout::RowMajor>&, float, crd::usize);
template InterpDecomp<double, Layout::RowMajor> interp_decomp<double, Layout::RowMajor>(
    crd::memory::IAllocator*, const Matrix<double, Layout::RowMajor>&, double, crd::usize);

} // namespace crd::hesap::dense
