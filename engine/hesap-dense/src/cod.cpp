#include <crd/hesap/dense/cod.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/detail/householder.hpp>

#include <type_traits>

namespace crd::hesap::dense
{

template <typename T, Layout L>
COD<T, L> factor_cod(crd::memory::IAllocator* alloc, const Matrix<T, L>& a, RealType<T> rcond)
{
    static_assert(L == Layout::RowMajor, "factor_cod currently supports RowMajor only");
    static_assert(!is_complex_v<T>, "factor_cod is real-only (complex LS routes through SVD)");

    const crd::usize m = a.rows();
    const crd::usize n = a.cols();

    COD<T, L> cod(alloc);
    cod.m = m;
    cod.n = n;

    // 1. Column-pivoting QR: A*P = Q*R, reveals rank r.
    cod.qr = QRColPiv<T, L>(alloc, m, n);
    factor_qr_colpiv<T, L>(cod.qr, a, rcond);
    const crd::usize r = cod.qr.rank();
    cod.rank = r;

    cod.t11 = Matrix<T, L>(alloc, r, r);
    cod.z = Matrix<T, L>(alloc, r, n > r ? n - r : 0);
    cod.tau_z.resize(r);
    if (r == 0)
    {
        return cod;
    }

    const Matrix<T, L>& packed = cod.qr.packed();
    const T* pdata = packed.data();
    const crd::usize pld = packed.ld();

    // 2. Extract the r×n upper-trapezoidal R block [R11 R12] (R is in the
    //    upper triangle of the col-piv-QR packed matrix). rblock(i,j) = R(i,j)
    //    for j >= i, else 0.
    const crd::usize lcols = n - r;  // trailing columns to annihilate (L in dtzrzf)
    Matrix<T, L> rblock(alloc, r, n);
    for (crd::usize i = 0; i < r; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            rblock.at(i, j) = (j >= i) ? pdata[i * pld + j] : T{0};
        }
    }

    if (lcols == 0)
    {
        // Already triangular; T11 = R11, Z = I.
        for (crd::usize i = 0; i < r; ++i)
        {
            for (crd::usize j = 0; j < r; ++j)
            {
                cod.t11.at(i, j) = rblock.at(i, j);
            }
            cod.tau_z[i] = T{0};
        }
        return cod;
    }

    // 3. DLATRZ: reduce [R11 R12] to [T11 0]·Z by RZ reflectors, bottom-up.
    //    For row i the reflector acts on [rblock(i,i), rblock(i, r:n)] (length
    //    lcols+1, the "1" implicit at position i), annihilating the trailing.
    crd::containers::Array<T> wbuf(alloc);
    wbuf.resize(lcols + 1);
    T* w = wbuf.data();

    for (crd::usize ip = r; ip-- > 0;)
    {
        // Gather [diag ; trailing] into a contiguous reflector vector.
        w[0] = rblock.at(ip, ip);
        for (crd::usize t = 0; t < lcols; ++t)
        {
            w[1 + t] = rblock.at(ip, r + t);
        }
        const auto h = detail::make_householder<T>(w, lcols + 1);
        cod.tau_z[ip] = h.tau;
        rblock.at(ip, ip) = h.beta;  // T11 diagonal
        for (crd::usize t = 0; t < lcols; ++t)
        {
            rblock.at(ip, r + t) = w[1 + t];  // z_i tail (scaled, implicit 1)
        }

        // Apply H(ip) from the right to rows above (rr < ip): acts on column ip
        // (implicit 1) + the trailing columns r:n.
        if (h.tau != T{0})
        {
            for (crd::usize rr = 0; rr < ip; ++rr)
            {
                T ww = rblock.at(rr, ip);
                for (crd::usize t = 0; t < lcols; ++t)
                {
                    ww += w[1 + t] * rblock.at(rr, r + t);
                }
                ww *= h.tau;
                rblock.at(rr, ip) -= ww;
                for (crd::usize t = 0; t < lcols; ++t)
                {
                    rblock.at(rr, r + t) -= ww * w[1 + t];
                }
            }
        }
    }

    // 4. Copy out T11 (upper-tri r×r) and Z reflector tails (row i = z_i).
    for (crd::usize i = 0; i < r; ++i)
    {
        for (crd::usize j = 0; j < r; ++j)
        {
            cod.t11.at(i, j) = (j >= i) ? rblock.at(i, j) : T{0};
        }
        for (crd::usize t = 0; t < lcols; ++t)
        {
            cod.z.at(i, t) = rblock.at(i, r + t);
        }
    }

    return cod;
}

template <typename T, Layout L>
void solve_cod(const COD<T, L>& cod, crd::containers::ConstSpan<T> b, crd::containers::Span<T> x)
{
    static_assert(L == Layout::RowMajor, "solve_cod currently supports RowMajor only");
    const crd::usize m = cod.m;
    const crd::usize n = cod.n;
    const crd::usize r = cod.rank;
    CRD_ASSERT_MSG(b.size() == m, "solve_cod: b size != m");
    CRD_ASSERT_MSG(x.size() == n, "solve_cod: x size != n");

    for (crd::usize j = 0; j < n; ++j)
    {
        x[j] = T{0};
    }
    if (r == 0)
    {
        return;
    }

    // 1. c = Q^T · b. Apply on a length-m working copy.
    crd::memory::IAllocator* alloc = cod.qr.allocator();
    crd::containers::Array<T> cbuf(alloc);
    cbuf.resize(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        cbuf[i] = b[i];
    }
    apply_q_transpose<T, L>(cod.qr, crd::containers::Span<T>{cbuf.data(), m});

    // 2. Solve T11 · y = c[0:r] (upper-triangular back-substitution).
    crd::containers::Array<T> ybuf(alloc);
    ybuf.resize(n);  // n-long: [y; 0] for the Z^T apply.
    for (crd::usize j = 0; j < n; ++j)
    {
        ybuf[j] = (j < r) ? cbuf[j] : T{0};
    }
    const Matrix<T, L>& t11 = cod.t11;
    for (crd::usize ii = r; ii-- > 0;)
    {
        T s = ybuf[ii];
        for (crd::usize j = ii + 1; j < r; ++j)
        {
            s -= t11.at(ii, j) * ybuf[j];
        }
        const T diag = t11.at(ii, ii);
        CRD_ASSERT_MSG(diag != T{0}, "solve_cod: T11 zero diagonal");
        ybuf[ii] = s / diag;
    }

    // 3. x_perm = Z^T · [y; 0]. Z = H(0)···H(r-1); Z^T applies H(0) first.
    const crd::usize lcols = (n > r) ? (n - r) : 0;
    const Matrix<T, L>& zz = cod.z;
    for (crd::usize i = 0; i < r; ++i)
    {
        const T tau = cod.tau_z[i];
        if (tau == T{0})
        {
            continue;
        }
        T ww = ybuf[i];
        for (crd::usize t = 0; t < lcols; ++t)
        {
            ww += zz.at(i, t) * ybuf[r + t];
        }
        ww *= tau;
        ybuf[i] -= ww;
        for (crd::usize t = 0; t < lcols; ++t)
        {
            ybuf[r + t] -= ww * zz.at(i, t);
        }
    }

    // 4. Undo the column permutation: x[jpvt[k]] = x_perm[k].
    const auto& jpvt = cod.qr.jpvt();
    for (crd::usize k = 0; k < n; ++k)
    {
        x[jpvt[k]] = ybuf[k];
    }
}

template COD<float, Layout::RowMajor> factor_cod<float, Layout::RowMajor>(
    crd::memory::IAllocator*, const Matrix<float, Layout::RowMajor>&, float);
template COD<double, Layout::RowMajor> factor_cod<double, Layout::RowMajor>(
    crd::memory::IAllocator*, const Matrix<double, Layout::RowMajor>&, double);
template void solve_cod<float, Layout::RowMajor>(const COD<float, Layout::RowMajor>&,
                                                 crd::containers::ConstSpan<float>,
                                                 crd::containers::Span<float>);
template void solve_cod<double, Layout::RowMajor>(const COD<double, Layout::RowMajor>&,
                                                  crd::containers::ConstSpan<double>,
                                                  crd::containers::Span<double>);

} // namespace crd::hesap::dense
