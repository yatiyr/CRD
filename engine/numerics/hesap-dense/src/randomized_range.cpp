#include <crd/hesap/dense/randomized_range.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/dense/svd.hpp>

namespace crd::hesap::dense
{
namespace
{
// Orthonormalize the columns of a (rows × ell) RowMajor buffer IN PLACE via
// Householder QR — form the first `ell` columns of Q by applying Q to e_c.
// (Lifted from the dense rsvd's local lambda; deterministic.)
template <typename T>
void orthonormalize_cols(crd::memory::IAllocator* alloc, crd::containers::Array<T>& mat, crd::usize rows,
                         crd::usize ell)
{
    Matrix<T> qm(alloc, rows, ell);
    for (crd::usize r = 0; r < rows; ++r)
    {
        for (crd::usize c = 0; c < ell; ++c)
        {
            qm.at(r, c) = mat[r * ell + c];
        }
    }
    QR<T> qr(alloc, rows, ell);
    factor_qr<T, Layout::RowMajor>(qr, qm);
    crd::containers::Array<T> col(alloc);
    col.resize(rows);
    for (crd::usize c = 0; c < ell; ++c)
    {
        for (crd::usize r = 0; r < rows; ++r)
        {
            col[r] = (r == c) ? T{1} : T{0};
        }
        apply_q<T, Layout::RowMajor>(qr, crd::containers::Span<T>{col.data(), rows});
        for (crd::usize r = 0; r < rows; ++r)
        {
            mat[r * ell + c] = col[r];
        }
    }
}

// out (outrows × ell, RowMajor) := op (or opᵀ) applied to each column of
// `in` (incols × ell, RowMajor). Matrix-free: gather a contiguous column, call
// the op, scatter the result back.
template <typename T>
void apply_op_columns(const LinearOp<T>& op, bool transpose, const crd::containers::Array<T>& in,
                      crd::usize incols, crd::containers::Array<T>& out, crd::usize outrows, crd::usize ell,
                      crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> xcol(alloc);
    crd::containers::Array<T> ycol(alloc);
    xcol.resize(incols);
    ycol.resize(outrows);
    for (crd::usize c = 0; c < ell; ++c)
    {
        for (crd::usize r = 0; r < incols; ++r)
        {
            xcol[r] = in[r * ell + c];
        }
        const crd::containers::ConstSpan<T> xs{xcol.data(), incols};
        const crd::containers::Span<T> ys{ycol.data(), outrows};
        // The apply runs unconditionally (it is the initializer); `ok` is only
        // consulted by the assert, which compiles out in NDEBUG.
        [[maybe_unused]] const bool ok = transpose ? op.apply_transpose(xs, ys) : op.apply(xs, ys);
        CRD_ASSERT_MSG(ok, "randomized_range: LinearOp apply failed");
        for (crd::usize r = 0; r < outrows; ++r)
        {
            out[r * ell + c] = ycol[r];
        }
    }
}
} // namespace

template <typename T>
RangeBasis<T> randomized_range(crd::memory::IAllocator* alloc, const LinearOp<T>& op, crd::usize rank,
                               crd::usize oversampling, crd::usize power_iters, crd::u64 seed)
{
    static_assert(!is_complex_v<T>, "randomized_range: real T only (v5e-1b; complex later)");
    const crd::usize m = op.n_rows();
    const crd::usize n = op.n_cols();
    const crd::usize mn = m < n ? m : n;
    const crd::usize k = rank < mn ? rank : mn;
    crd::usize ell = k + oversampling;
    if (ell > mn)
    {
        ell = mn;
    }

    RangeBasis<T> out(alloc);
    if (m == 0 || n == 0 || k == 0 || ell == 0)
    {
        out.q = Matrix<T>(alloc, m, 0);
        out.rank = 0;
        return out;
    }

    // Power iteration needs opᵀ; the basic scheme needs only op.apply.
    const crd::usize pit = op.has_transpose() ? power_iters : 0;

    // Counter-based Gaussian sketch Ω (n × ell) — pure function of (seed, idx).
    crd::containers::Array<T> omega(alloc);
    omega.resize(n * ell);
    for (crd::usize idx = 0; idx < n * ell; ++idx)
    {
        omega[idx] = counter_gaussian<T>(seed, static_cast<crd::u64>(idx));
    }

    // Y = op·Ω (m × ell).
    crd::containers::Array<T> y(alloc);
    y.resize(m * ell);
    apply_op_columns<T>(op, /*transpose*/ false, omega, n, y, m, ell, alloc);

    // Subspace (power) iteration: same structure as the dense rsvd.
    crd::containers::Array<T> z(alloc);
    z.resize(n * ell);
    for (crd::usize it = 0; it < pit; ++it)
    {
        orthonormalize_cols<T>(alloc, y, m, ell);
        apply_op_columns<T>(op, /*transpose*/ true, y, m, z, n, ell, alloc);  // Z = opᵀ·Y
        orthonormalize_cols<T>(alloc, z, n, ell);
        apply_op_columns<T>(op, /*transpose*/ false, z, n, y, m, ell, alloc);  // Y = op·Z
    }
    orthonormalize_cols<T>(alloc, y, m, ell);  // Q (m × ell)

    out.q = Matrix<T>(alloc, m, ell);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < ell; ++j)
        {
            out.q.at(i, j) = y[i * ell + j];
        }
    }
    out.rank = ell;
    return out;
}

template <typename T>
SVD<T> rsvd_op(crd::memory::IAllocator* alloc, const LinearOp<T>& op, crd::usize rank, crd::usize oversampling,
               crd::usize power_iters, crd::u64 seed)
{
    static_assert(!is_complex_v<T>, "rsvd_op: real T only (v5e-1b)");
    using R = RealType<T>;
    const crd::usize m = op.n_rows();
    const crd::usize n = op.n_cols();
    const crd::usize mn = m < n ? m : n;
    const crd::usize k = rank < mn ? rank : mn;

    SVD<T> out(alloc);
    // B = Qᵀ·op is formed via opᵀ·Q ⇒ requires a transpose action.
    if (m == 0 || n == 0 || k == 0 || !op.has_transpose())
    {
        out.u = Matrix<T>(alloc, m, 0);
        out.s = Vector<R>(alloc, 0);
        out.v = Matrix<T>(alloc, n, 0);
        return out;
    }

    RangeBasis<T> rb = randomized_range<T>(alloc, op, rank, oversampling, power_iters, seed);
    const crd::usize ell = rb.rank;
    if (ell == 0)
    {
        out.u = Matrix<T>(alloc, m, 0);
        out.s = Vector<R>(alloc, 0);
        out.v = Matrix<T>(alloc, n, 0);
        return out;
    }

    // Q is m × ell (rb.q). Copy into a RowMajor buffer for the per-column apply.
    crd::containers::Array<T> q(alloc);
    q.resize(m * ell);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < ell; ++j)
        {
            q[i * ell + j] = rb.q.at(i, j);
        }
    }

    // Bᵀ = opᵀ·Q (n × ell), then B (ell × n) = (Bᵀ)ᵀ.
    crd::containers::Array<T> bt(alloc);
    bt.resize(n * ell);
    apply_op_columns<T>(op, /*transpose*/ true, q, m, bt, n, ell, alloc);

    Matrix<T> bm(alloc, ell, n);
    for (crd::usize i = 0; i < ell; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            bm.at(i, j) = bt[j * ell + i];
        }
    }
    SVD<T> bsvd = svd<T>(alloc, bm);  // bsvd.u: ell×ell, bsvd.v: n×ell, bsvd.s: ell

    // U = Q · bsvd.u (m × ell).
    crd::containers::Array<T> umat(alloc);
    umat.resize(m * ell);
    {
        MatrixView<const T, Layout::RowMajor> q_v{q.data(), m, ell, ell};
        MatrixView<const T, Layout::RowMajor> bu_v{bsvd.u.data(), bsvd.u.rows(), bsvd.u.cols(), bsvd.u.ld()};
        MatrixView<T, Layout::RowMajor> u_v{umat.data(), m, ell, ell};
        gemm_parallel_auto<T, Layout::RowMajor>(T{1}, q_v, bu_v, T{0}, u_v, Trans::None, Trans::None, alloc);
    }

    // Truncate to k.
    out.u = Matrix<T>(alloc, m, k);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < k; ++j)
        {
            out.u.at(i, j) = umat[i * ell + j];
        }
    }
    out.s = Vector<R>(alloc, k);
    for (crd::usize j = 0; j < k; ++j)
    {
        out.s.data()[j] = bsvd.s.data()[j];
    }
    out.v = Matrix<T>(alloc, n, k);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < k; ++j)
        {
            out.v.at(i, j) = bsvd.v.at(i, j);
        }
    }
    return out;
}

template <typename T>
EigSym<T> rsyev_op(crd::memory::IAllocator* alloc, const LinearOp<T>& op, crd::usize rank, crd::usize oversampling,
                   crd::usize power_iters, crd::u64 seed)
{
    static_assert(!is_complex_v<T>, "rsyev_op: real T only (v5e-1b)");
    using R = RealType<T>;
    const crd::usize n = op.n_rows();
    const crd::usize k = rank < n ? rank : n;

    EigSym<T> out(alloc);
    if (n == 0 || k == 0)
    {
        out.values = Vector<R>(alloc, 0);
        out.vectors = Matrix<T>(alloc, n, 0);
        return out;
    }

    RangeBasis<T> rb = randomized_range<T>(alloc, op, rank, oversampling, power_iters, seed);
    const crd::usize ell = rb.rank;
    if (ell == 0)
    {
        out.values = Vector<R>(alloc, 0);
        out.vectors = Matrix<T>(alloc, n, 0);
        return out;
    }

    crd::containers::Array<T> q(alloc);
    q.resize(n * ell);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < ell; ++j)
        {
            q[i * ell + j] = rb.q.at(i, j);
        }
    }

    // AQ = op·Q (n × ell), then B = Qᵀ·AQ (ell × ell), symmetrized.
    crd::containers::Array<T> aq(alloc);
    aq.resize(n * ell);
    apply_op_columns<T>(op, /*transpose*/ false, q, n, aq, n, ell, alloc);

    crd::containers::Array<T> b(alloc);
    b.resize(ell * ell);
    {
        MatrixView<const T, Layout::RowMajor> q_v{q.data(), n, ell, ell};
        MatrixView<const T, Layout::RowMajor> aq_v{aq.data(), n, ell, ell};
        MatrixView<T, Layout::RowMajor> b_v{b.data(), ell, ell, ell};
        gemm_parallel_auto<T, Layout::RowMajor>(T{1}, q_v, aq_v, T{0}, b_v, Trans::Transpose, Trans::None, alloc);
    }
    Symmetric<T> bsym(alloc, ell);
    for (crd::usize i = 0; i < ell; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            bsym.at(i, j) = static_cast<T>(0.5) * (b[i * ell + j] + b[j * ell + i]);
        }
    }
    const EigSym<T> bei = eig_sym<T>(alloc, bsym);  // ascending values, V_b columns

    // Top-k = the k largest (last of ascending), reversed to descending;
    // lift V = Q · V_b. (Same convention as the dense rsyev.)
    out.values = Vector<R>(alloc, k);
    out.vectors = Matrix<T>(alloc, n, k);
    for (crd::usize idx = 0; idx < k; ++idx)
    {
        const crd::usize src = ell - 1 - idx;
        out.values.data()[idx] = bei.values.data()[src];
        for (crd::usize r = 0; r < n; ++r)
        {
            T acc{};
            for (crd::usize p = 0; p < ell; ++p)
            {
                acc += q[r * ell + p] * bei.vectors.at(p, src);
            }
            out.vectors.at(r, idx) = acc;
        }
    }
    return out;
}

// ---- explicit instantiations (v5e-1b: real f32/f64) -------------------
template RangeBasis<float> randomized_range<float>(crd::memory::IAllocator*, const LinearOp<float>&, crd::usize,
                                                   crd::usize, crd::usize, crd::u64);
template RangeBasis<double> randomized_range<double>(crd::memory::IAllocator*, const LinearOp<double>&, crd::usize,
                                                     crd::usize, crd::usize, crd::u64);
template SVD<float> rsvd_op<float>(crd::memory::IAllocator*, const LinearOp<float>&, crd::usize, crd::usize,
                                   crd::usize, crd::u64);
template SVD<double> rsvd_op<double>(crd::memory::IAllocator*, const LinearOp<double>&, crd::usize, crd::usize,
                                     crd::usize, crd::u64);
template EigSym<float> rsyev_op<float>(crd::memory::IAllocator*, const LinearOp<float>&, crd::usize, crd::usize,
                                       crd::usize, crd::u64);
template EigSym<double> rsyev_op<double>(crd::memory::IAllocator*, const LinearOp<double>&, crd::usize, crd::usize,
                                         crd::usize, crd::u64);

} // namespace crd::hesap::dense
