#include <crd/hesap/dense/lstsq.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/cod.hpp>
#include <crd/hesap/dense/detail/apply_q_block.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/svd.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// |z| for real or complex T (the real magnitude).
template <typename T>
inline RealType<T> mag(const T& z) noexcept
{
    if constexpr (is_complex_v<T>)
    {
        return crd::hesap::abs(z);
    }
    else
    {
        return std::abs(z);
    }
}

// conj(z) for real (identity) or complex T.
template <typename T>
inline T conj_of(const T& z) noexcept
{
    if constexpr (is_complex_v<T>)
    {
        return crd::hesap::conj(z);
    }
    else
    {
        return z;
    }
}

// residual[c] = ‖A·x_c − b_c‖₂ for each RHS column c.
template <typename T>
void compute_residuals(const Matrix<T>& a, const Matrix<T>& x, const Matrix<T>& b,
                       Vector<RealType<T>>& residual)
{
    using R = RealType<T>;
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    const crd::usize nrhs = b.cols();
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        R acc = R{0};
        for (crd::usize i = 0; i < m; ++i)
        {
            T ax = T{};
            for (crd::usize j = 0; j < n; ++j)
            {
                ax = ax + a.at(i, j) * x.at(j, c);
            }
            const T r = ax - b.at(i, c);
            acc += mag<T>(r) * mag<T>(r);
        }
        residual(c) = std::sqrt(acc);
    }
}

// Default rcond = max(m,n) * eps when the caller passes a negative value.
template <typename T>
inline RealType<T> effective_rcond(RealType<T> rcond, crd::usize m, crd::usize n) noexcept
{
    using R = RealType<T>;
    if (rcond >= R{0})
    {
        return rcond;
    }
    return static_cast<R>(m > n ? m : n) * std::numeric_limits<R>::epsilon();
}

// SVD min-norm solve: X = V · Σ⁺ · Uᴴ · B. Handles every rank/shape, real
// and complex. Fills out.x (n×nrhs) and out.rank.
template <typename T>
void solve_via_svd(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b,
                   RealType<T> rcond, LstSq<T>& out)
{
    using R = RealType<T>;
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    const crd::usize nrhs = b.cols();

    SVD<T> s = svd<T>(alloc, a);
    const crd::usize k = s.s.size();  // min(m,n)

    const R rc = effective_rcond<T>(rcond, m, n);
    const R smax = (k > 0) ? s.s(0) : R{0};
    const R tol = rc * smax;
    crd::usize rank = 0;
    for (crd::usize t = 0; t < k; ++t)
    {
        if (s.s(t) > tol)
        {
            ++rank;
        }
    }
    out.rank = rank;

    out.x = Matrix<T>(alloc, n, nrhs);
    crd::containers::Array<T> w(alloc);
    w.resize(k);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        // w = Σ⁺ · (Uᴴ · b_c).
        for (crd::usize t = 0; t < k; ++t)
        {
            if (s.s(t) > tol)
            {
                T acc = T{};
                for (crd::usize i = 0; i < m; ++i)
                {
                    acc = acc + conj_of<T>(s.u.at(i, t)) * b.at(i, c);
                }
                const R inv = R{1} / s.s(t);
                w[t] = acc * inv;
            }
            else
            {
                w[t] = T{};
            }
        }
        // x_c = V · w.
        for (crd::usize j = 0; j < n; ++j)
        {
            T acc = T{};
            for (crd::usize t = 0; t < k; ++t)
            {
                acc = acc + s.v.at(j, t) * w[t];
            }
            out.x.at(j, c) = acc;
        }
    }
}

// Full-rank fast path: blocked Householder QR + per-column back-substitution
// (assumes m >= n and full column rank — LAPACK dgels). Real only.
template <typename T>
void solve_via_qr(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b,
                  RealType<T> rcond, LstSq<T>& out)
{
    using R = RealType<T>;
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    const crd::usize nrhs = b.cols();
    CRD_ASSERT_MSG(m >= n, "lstsq QR path requires m >= n; use COD/SVD for underdetermined");

    QR<T, Layout::RowMajor> qr(alloc, m, n);
    factor_qr<T, Layout::RowMajor>(qr, a);
    const Matrix<T, Layout::RowMajor>& r = qr.packed();

    // Rank report from the R diagonal (no pivoting → a reveal only when a
    // pivot is small; for guaranteed rank-revealing use COD/SVD).
    const R rc = effective_rcond<T>(rcond, m, n);
    const R r00 = (n > 0) ? mag<T>(r.at(0, 0)) : R{0};
    const R thresh = rc * r00;
    crd::usize rank = 0;
    for (crd::usize i = 0; i < n; ++i)
    {
        if (mag<T>(r.at(i, i)) > thresh)
        {
            ++rank;
        }
    }
    out.rank = rank;

    out.x = Matrix<T>(alloc, n, nrhs);
    crd::containers::Array<T> col(alloc);
    col.resize(m);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            col[i] = b.at(i, c);
        }
        apply_q_transpose<T, Layout::RowMajor>(qr, crd::containers::Span<T>{col.data(), m});
        for (crd::usize ii = n; ii-- > 0;)
        {
            T sacc = col[ii];
            for (crd::usize j = ii + 1; j < n; ++j)
            {
                sacc = sacc - r.at(ii, j) * out.x.at(j, c);
            }
            const T diag = r.at(ii, ii);
            CRD_ASSERT_MSG(mag<T>(diag) > R{0}, "lstsq QR: zero pivot (rank-deficient — use COD/SVD)");
            out.x.at(ii, c) = sacc / diag;
        }
    }
}

// Rank-revealing fast min-norm path: complete orthogonal decomposition. Real.
template <typename T>
void solve_via_cod(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b,
                   RealType<T> rcond, LstSq<T>& out)
{
    const crd::usize n = a.cols();
    const crd::usize m = a.rows();
    const crd::usize nrhs = b.cols();

    COD<T, Layout::RowMajor> cod = factor_cod<T, Layout::RowMajor>(alloc, a, rcond);
    out.rank = cod.rank;
    out.x = Matrix<T>(alloc, n, nrhs);

    crd::containers::Array<T> bcol(alloc);
    crd::containers::Array<T> xcol(alloc);
    bcol.resize(m);
    xcol.resize(n);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            bcol[i] = b.at(i, c);
        }
        solve_cod<T, Layout::RowMajor>(cod, crd::containers::ConstSpan<T>{bcol.data(), m},
                                       crd::containers::Span<T>{xcol.data(), n});
        for (crd::usize j = 0; j < n; ++j)
        {
            out.x.at(j, c) = xcol[j];
        }
    }
}

// In-place inverse of an r×r UPPER-triangular matrix (LAPACK dtrtri, blocked
// recursive): partition T = [[A B];[0 C]] ⟹ T⁻¹ = [[A⁻¹  −A⁻¹·B·C⁻¹];[0 C⁻¹]].
// Off-diagonal coupling rides the crushing gemm; small diagonal blocks invert
// scalar. `t` is r×r RowMajor with leading dim `ld`. Real T.
template <typename T>
void invert_upper_tri_inplace(T* t, crd::usize r, crd::usize ld, crd::memory::IAllocator* alloc)
{
    constexpr crd::usize kTriBase = 48;
    if (r == 0)
    {
        return;
    }
    if (r <= kTriBase)
    {
        // Scalar upper-triangular inverse (LAPACK dtrti2 'U'): process columns
        // left to right; when at column j the leading j×j block is already
        // inverted, so x = T_inv[0:j,0:j]·T[0:j,j] (T[0:j,j] = ORIGINAL col j),
        // then T_inv[0:j,j] = -1/T[j,j] · x. A temp avoids read/write aliasing.
        crd::containers::Array<T> tmp(alloc);
        tmp.resize(r);
        for (crd::usize j = 0; j < r; ++j)
        {
            const T ajj = T{1} / t[j * ld + j];
            for (crd::usize i = 0; i < j; ++i)
            {
                T s = T{0};
                for (crd::usize kk = i; kk < j; ++kk)
                {
                    s += t[i * ld + kk] * t[kk * ld + j];
                }
                tmp[i] = s;
            }
            for (crd::usize i = 0; i < j; ++i)
            {
                t[i * ld + j] = -ajj * tmp[i];
            }
            t[j * ld + j] = ajj;
        }
        return;
    }
    const crd::usize n1 = r / 2;
    const crd::usize n2 = r - n1;
    constexpr Layout kL = Layout::RowMajor;
    T* a = t;                  // n1×n1 at (0,0)
    T* b = t + n1;             // n1×n2 at (0,n1)
    T* c = t + n1 * ld + n1;   // n2×n2 at (n1,n1)
    invert_upper_tri_inplace<T>(a, n1, ld, alloc);
    invert_upper_tri_inplace<T>(c, n2, ld, alloc);
    // B := -(A · B) · C.
    crd::containers::Array<T> tmp(alloc);
    tmp.resize(n1 * n2);
    MatrixView<const T, kL> av{a, n1, n1, ld};
    MatrixView<const T, kL> bv{b, n1, n2, ld};
    MatrixView<const T, kL> cv{c, n2, n2, ld};
    MatrixView<T, kL> tv{tmp.data(), n1, n2, n2};
    gemm<T, kL>(T{1}, av, bv, T{0}, tv, Trans::None, Trans::None, alloc);  // tmp = A·B
    MatrixView<const T, kL> tv_c{tmp.data(), n1, n2, n2};
    MatrixView<T, kL> bout{b, n1, n2, ld};
    gemm<T, kL>(T{-1}, tv_c, cv, T{0}, bout, Trans::None, Trans::None, alloc);  // B = -tmp·C
}

// Pseudoinverse via complete orthogonal decomposition (real, fast — the path
// Eigen's completeOrthogonalDecomposition().pseudoInverse() takes):
//   A·P = Q·[T 0; 0 0]·Z  ⟹  A⁺ = P·Zᵀ·[T⁻¹ 0; 0 0]·Qᵀ  (n×m).
template <typename T>
Matrix<T> pinv_via_cod(crd::memory::IAllocator* alloc, const Matrix<T>& a, RealType<T> rcond)
{
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    COD<T, Layout::RowMajor> cod = factor_cod<T, Layout::RowMajor>(alloc, a, rcond);
    const crd::usize r = cod.rank;

    // W (n×m) = [T⁻¹ 0; 0 0]; fill the top-left r×r with T11 then invert it in
    // place via the blocked recursive trtri (BLAS-3 — the scalar O(r³) back-sub
    // was the pinv bottleneck at large r). The zeroed strict-lower keeps the
    // block upper-triangular for the recursion's gemms.
    Matrix<T> w(alloc, n, m);
    w.set_zero();
    const Matrix<T, Layout::RowMajor>& t11 = cod.t11;
    for (crd::usize i = 0; i < r; ++i)
    {
        for (crd::usize j = i; j < r; ++j)
        {
            w.at(i, j) = t11.at(i, j);
        }
    }
    invert_upper_tri_inplace<T>(w.data(), r, w.ld(), alloc);

    // W := Zᵀ · W. Apply the RZ reflectors H(0)…H(r-1) to each column of W.
    const crd::usize lcols = (n > r) ? (n - r) : 0;
    const Matrix<T, Layout::RowMajor>& zz = cod.z;
    for (crd::usize c = 0; c < m; ++c)
    {
        for (crd::usize i = 0; i < r; ++i)
        {
            const T tau = cod.tau_z[i];
            if (tau == T{0})
            {
                continue;
            }
            T ww = w.at(i, c);
            for (crd::usize t = 0; t < lcols; ++t)
            {
                ww += zz.at(i, t) * w.at(r + t, c);
            }
            ww *= tau;
            w.at(i, c) -= ww;
            for (crd::usize t = 0; t < lcols; ++t)
            {
                w.at(r + t, c) -= ww * zz.at(i, t);
            }
        }
    }

    // W := W · Qᵀ  (BLAS-3 blocked dlarfb, Right + Transpose) — the lever that
    // makes COD-pinv crush Eigen vs the per-row scalar reflector application.
    {
        const Matrix<T, Layout::RowMajor>& qp = cod.qr.packed();
        detail::apply_q_block<T>(qp.data(), qp.ld(), m, cod.qr.num_reflectors(),
                                 cod.qr.taus().data(), w.data(), w.ld(), /*crows=*/n, /*ccols=*/m,
                                 /*right=*/true, /*transpose=*/true, alloc);
    }

    // Undo the column permutation P on the rows: A⁺.row(jpvt[k]) = W.row(k).
    const auto& jpvt = cod.qr.jpvt();
    Matrix<T> out(alloc, n, m);
    for (crd::usize k = 0; k < n; ++k)
    {
        const crd::usize dst = jpvt[k];
        for (crd::usize i = 0; i < m; ++i)
        {
            out.at(dst, i) = w.at(k, i);
        }
    }
    return out;
}

// Pseudoinverse via SVD: A⁺ = V · Σ⁺ · Uᴴ  (handles every rank/shape + complex).
template <typename T>
Matrix<T> pinv_via_svd(crd::memory::IAllocator* alloc, const Matrix<T>& a, RealType<T> rcond)
{
    using R = RealType<T>;
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    SVD<T> s = svd<T>(alloc, a);
    const crd::usize k = s.s.size();
    const R rc = effective_rcond<T>(rcond, m, n);
    const R smax = (k > 0) ? s.s(0) : R{0};
    const R tol = rc * smax;
    Matrix<T> out(alloc, n, m);
    for (crd::usize j = 0; j < n; ++j)
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            T acc = T{};
            for (crd::usize t = 0; t < k; ++t)
            {
                if (s.s(t) > tol)
                {
                    const R inv = R{1} / s.s(t);
                    acc = acc + (s.v.at(j, t) * inv) * conj_of<T>(s.u.at(i, t));
                }
            }
            out.at(j, i) = acc;
        }
    }
    return out;
}

} // namespace

template <typename T>
LstSq<T> lstsq(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b,
               LstSqMethod method, RealType<T> rcond, bool with_residual)
{
    CRD_ASSERT_MSG(a.rows() == b.rows(), "lstsq: A and B row count mismatch");
    LstSq<T> out(alloc);

    if constexpr (is_complex_v<T>)
    {
        // Complex routes through the complex SVD regardless of the request
        // (the QR/COD fast paths are real).
        solve_via_svd<T>(alloc, a, b, rcond, out);
    }
    else
    {
        LstSqMethod m = method;
        if (m == LstSqMethod::Auto)
        {
            m = LstSqMethod::COD;  // rank-revealing + fast: the robust default
        }
        switch (m)
        {
        case LstSqMethod::QR:
            solve_via_qr<T>(alloc, a, b, rcond, out);
            break;
        case LstSqMethod::SVD:
            solve_via_svd<T>(alloc, a, b, rcond, out);
            break;
        case LstSqMethod::COD:
        case LstSqMethod::Auto:
        default:
            solve_via_cod<T>(alloc, a, b, rcond, out);
            break;
        }
    }

    out.residual = Vector<RealType<T>>(alloc, b.cols());
    if (with_residual)
    {
        compute_residuals<T>(a, out.x, b, out.residual);
    }
    return out;
}

template <typename T>
LstSq<T> lstsq(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Vector<T>& b,
               LstSqMethod method, RealType<T> rcond, bool with_residual)
{
    CRD_ASSERT_MSG(a.rows() == b.size(), "lstsq: A rows != b size");
    Matrix<T> bm(alloc, b.size(), 1);
    for (crd::usize i = 0; i < b.size(); ++i)
    {
        bm.at(i, 0) = b(i);
    }
    return lstsq<T>(alloc, a, bm, method, rcond, with_residual);
}

template <typename T>
Matrix<T> pinv(crd::memory::IAllocator* alloc, const Matrix<T>& a, PinvMethod method, RealType<T> rcond)
{
    if constexpr (is_complex_v<T>)
    {
        // Complex routes through the SVD (the COD fast path is real).
        return pinv_via_svd<T>(alloc, a, rcond);
    }
    else
    {
        PinvMethod m = method;
        if (m == PinvMethod::Auto)
        {
            m = PinvMethod::COD;  // matches Eigen pseudoInverse(); ~6× the SVD path
        }
        if (m == PinvMethod::SVD)
        {
            return pinv_via_svd<T>(alloc, a, rcond);
        }
        return pinv_via_cod<T>(alloc, a, rcond);
    }
}

template LstSq<float> lstsq<float>(crd::memory::IAllocator*, const Matrix<float>&,
                                   const Matrix<float>&, LstSqMethod, float, bool);
template LstSq<double> lstsq<double>(crd::memory::IAllocator*, const Matrix<double>&,
                                     const Matrix<double>&, LstSqMethod, double, bool);
template LstSq<Complex<float>> lstsq<Complex<float>>(crd::memory::IAllocator*,
                                                     const Matrix<Complex<float>>&,
                                                     const Matrix<Complex<float>>&, LstSqMethod, float,
                                                     bool);
template LstSq<Complex<double>> lstsq<Complex<double>>(crd::memory::IAllocator*,
                                                       const Matrix<Complex<double>>&,
                                                       const Matrix<Complex<double>>&, LstSqMethod,
                                                       double, bool);

template LstSq<float> lstsq<float>(crd::memory::IAllocator*, const Matrix<float>&,
                                   const Vector<float>&, LstSqMethod, float, bool);
template LstSq<double> lstsq<double>(crd::memory::IAllocator*, const Matrix<double>&,
                                     const Vector<double>&, LstSqMethod, double, bool);
template LstSq<Complex<float>> lstsq<Complex<float>>(crd::memory::IAllocator*,
                                                     const Matrix<Complex<float>>&,
                                                     const Vector<Complex<float>>&, LstSqMethod, float,
                                                     bool);
template LstSq<Complex<double>> lstsq<Complex<double>>(crd::memory::IAllocator*,
                                                       const Matrix<Complex<double>>&,
                                                       const Vector<Complex<double>>&, LstSqMethod,
                                                       double, bool);

template Matrix<float> pinv<float>(crd::memory::IAllocator*, const Matrix<float>&, PinvMethod, float);
template Matrix<double> pinv<double>(crd::memory::IAllocator*, const Matrix<double>&, PinvMethod, double);
template Matrix<Complex<float>> pinv<Complex<float>>(crd::memory::IAllocator*,
                                                     const Matrix<Complex<float>>&, PinvMethod, float);
template Matrix<Complex<double>> pinv<Complex<double>>(crd::memory::IAllocator*,
                                                       const Matrix<Complex<double>>&, PinvMethod, double);

} // namespace crd::hesap::dense
