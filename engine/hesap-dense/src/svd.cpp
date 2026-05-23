#include <crd/hesap/dense/svd.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/bdsqr.hpp>
#include <crd/hesap/dense/detail/dot_simd.hpp>
#include <crd/hesap/dense/detail/dqds.hpp>
#include <crd/hesap/dense/detail/householder.hpp>
#include <crd/hesap/dense/detail/orgbr.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// Apply H = I - tau v v^T from the LEFT to the sub-block a(row0:m, col0:n1),
// where v is implicit-unit: v[0]=1, v[k] = a[(row0+k)*lda + vcol] for k>=1
// (the reflector stored in column `vcol`, rows row0..m-1). a(row0,*) uses v[0]=1.
template <typename T>
void apply_reflector_left(T* a, crd::usize lda, crd::usize row0, crd::usize m, crd::usize col0, crd::usize ncols,
                          crd::usize vcol, T tau)
{
    if (tau == T{0})
    {
        return;
    }
    for (crd::usize c = col0; c < col0 + ncols; ++c)
    {
        T dot = a[row0 * lda + c];  // v[0]=1
        for (crd::usize k = row0 + 1; k < m; ++k)
        {
            dot += a[k * lda + vcol] * a[k * lda + c];
        }
        const T s = tau * dot;
        a[row0 * lda + c] -= s;  // v[0]=1
        for (crd::usize k = row0 + 1; k < m; ++k)
        {
            a[k * lda + c] -= a[k * lda + vcol] * s;
        }
    }
}

// Apply G = I - tau v v^T from the RIGHT to a(row0:m, col0:n), where v is
// implicit-unit in row `vrow`: v[0]=1 at column col0, v[c] = a[vrow*lda + col0+c].
template <typename T>
void apply_reflector_right(T* a, crd::usize lda, crd::usize row0, crd::usize m, crd::usize col0, crd::usize n,
                           crd::usize vrow, T tau)
{
    if (tau == T{0})
    {
        return;
    }
    for (crd::usize r = row0; r < m; ++r)
    {
        T dot = a[r * lda + col0];  // v[0]=1
        for (crd::usize c = col0 + 1; c < n; ++c)
        {
            dot += a[vrow * lda + c] * a[r * lda + c];
        }
        const T s = tau * dot;
        a[r * lda + col0] -= s;  // v[0]=1
        for (crd::usize c = col0 + 1; c < n; ++c)
        {
            a[r * lda + c] -= a[vrow * lda + c] * s;
        }
    }
}

// The orthogonal-factor formation (U = Q, VT = P^T) moved to blocked dorgbr in
// detail/orgbr.hpp (v3b-1b-perf); the serial scalar reflector-apply that lived
// here is now orgbr_{q,p}_scalar (the small-n path + the test oracle).

// Panel width for the blocked dgebrd path (matches the blocked dsytrd default).
constexpr crd::usize kBidiagBlock = 32;

// bidiag_unblocked — Golub-Kahan unblocked dgebd2 (m >= n, upper bidiagonal).
// This is the original v3b-1a kernel; it now serves three roles: the small-size
// path, the panel-tail handler for the blocked driver, and the correctness
// oracle. CONVENTION: v[0]=1 implicit (never written to A), beta lives in d/e;
// the head A(i,i)/A(i,i+1) keeps its updated value (NOT the explicit-1 form the
// blocked dlabrd_upper uses internally). The reflector-tail storage layout is
// identical to dlabrd_upper, so form_q_bidiag / form_pt_bidiag / dbdsqr read
// both paths the same way (they never read A's diagonal/super-diagonal).
template <typename T>
void bidiag_unblocked(T* a, crd::usize m, crd::usize n, crd::usize lda, RealType<T>* d, RealType<T>* e, T* tauq,
                      T* taup, crd::memory::IAllocator* scratch)
{
    if (n == 0 || m == 0)
    {
        return;
    }
    crd::containers::Array<T> colbuf(scratch);
    crd::containers::Array<T> rowbuf(scratch);
    colbuf.resize(m);
    rowbuf.resize(n);

    for (crd::usize i = 0; i < n; ++i)
    {
        // --- Left reflector H(i): annihilate A(i+1:m, i) ---
        const crd::usize collen = m - i;
        for (crd::usize k = 0; k < collen; ++k)
        {
            colbuf[k] = a[(i + k) * lda + i];
        }
        const auto h = detail::make_householder<T>(colbuf.data(), collen);
        tauq[i] = h.tau;
        d[i] = h.beta;
        for (crd::usize k = 1; k < collen; ++k)
        {
            a[(i + k) * lda + i] = colbuf[k];
        }
        if (i + 1 < n)
        {
            apply_reflector_left<T>(a, lda, i, m, i + 1, n - (i + 1), i, h.tau);
        }

        // --- Right reflector G(i): annihilate A(i, i+2:n) ---
        if (i + 1 < n)
        {
            const crd::usize rowlen = n - (i + 1);
            for (crd::usize k = 0; k < rowlen; ++k)
            {
                rowbuf[k] = a[i * lda + (i + 1 + k)];
            }
            const auto g = detail::make_householder<T>(rowbuf.data(), rowlen);
            taup[i] = g.tau;
            e[i] = g.beta;
            for (crd::usize k = 1; k < rowlen; ++k)
            {
                a[i * lda + (i + 1 + k)] = rowbuf[k];
            }
            if (i + 1 < m)
            {
                apply_reflector_right<T>(a, lda, i + 1, m, i + 1, n, i, g.tau);
            }
        }
        else
        {
            taup[i] = T{0};
        }
    }
}

// dlabrd_upper — faithful LAPACK dlabrd (m >= n, upper-bidiagonal branch),
// RowMajor. Reduces the first `nb` rows/columns of the submatrix
// As = A(k:m, k:n) (origin (k,k), dimensions ms x ns with ms >= ns at the
// matrix level), storing the reflectors in As and building the panel update
// matrices X (ms x nb, ld = nb) and Y (ns x nb, ld = nb) so the driver can
// crush the trailing block with two GEMMs:  As22 -= V*Y^T + X*U^T.
//
// CONVENTION (load-bearing, differs from bidiag_unblocked): during this routine
// the reflector unit heads are written EXPLICITLY  a_sub(i,i)=1 (left, H(i)) and
// a_sub(i,i+1)=1 (right, G(i)) because they are READ by the same iteration's Y/X
// matvecs (LAPACK dlabrd lines 264/292). They stay set through the trailing
// GEMM (the heads at the panel/tail boundary are intentionally part of V and the
// U-block); the driver restores a_sub(i,i)=d[k+i], a_sub(i,i+1)=e[k+i] afterwards.
// `vcol` is contiguous scratch >= ms (the left reflector is a strided column).
template <typename T>
void dlabrd_upper(T* a, crd::usize lda, crd::usize k, crd::usize ms, crd::usize ns, crd::usize nb, RealType<T>* d,
                  RealType<T>* e, T* tauq, T* taup, T* xbuf, T* ybuf, T* vcol, T* yacc)
{
    const crd::usize ldx = nb;
    const crd::usize ldy = nb;
    auto a_sub = [&](crd::usize r, crd::usize c) -> T& { return a[(k + r) * lda + (k + c)]; };
    auto xmat = [&](crd::usize r, crd::usize c) -> T& { return xbuf[r * ldx + c]; };
    auto ymat = [&](crd::usize r, crd::usize c) -> T& { return ybuf[r * ldy + c]; };

    for (crd::usize i = 0; i < nb; ++i)
    {
        // --- Update a_sub(i:ms, i) from prior panel columns (ref dlabrd 253-256). ---
        for (crd::usize r = i; r < ms; ++r)
        {
            T acc{};
            for (crd::usize c = 0; c < i; ++c)  // -= A(i:m,1:i-1) * ymat(i,1:i-1)^T
            {
                acc += a_sub(r, c) * ymat(i, c);
            }
            for (crd::usize c = 0; c < i; ++c)  // -= xmat(i:m,1:i-1) * A(1:i-1,i)
            {
                acc += xmat(r, c) * a_sub(c, i);
            }
            a_sub(r, i) -= acc;
        }

        // --- Reflector Q(i): annihilate a_sub(i+1:ms, i) (strided column). ---
        const crd::usize collen = ms - i;
        for (crd::usize t = 0; t < collen; ++t)
        {
            vcol[t] = a_sub(i + t, i);
        }
        const auto hq = detail::make_householder<T>(vcol, collen);
        tauq[k + i] = hq.tau;
        d[k + i] = hq.beta;
        for (crd::usize t = 1; t < collen; ++t)
        {
            a_sub(i + t, i) = vcol[t];
        }
        a_sub(i, i) = T{1};  // explicit unit head (restored to d[k+i] by the driver)

        if (i + 1 < ns)
        {
            // --- Compute ymat(i+1:ns, i) (ref dlabrd 268-278). ---
            // Y = A(i:m, i+1:n)^T * A(i:m, i), accumulated row-outer into a
            // CONTIGUOUS scratch `yacc` so the inner op is a contiguous SIMD axpy
            // along each A-row tail (the column-strided textbook form
            // cache-thrashes at scale), then scattered into Y's strided column.
            const crd::usize ylen = ns - (i + 1);
            for (crd::usize t = 0; t < ylen; ++t)
            {
                yacc[t] = T{0};
            }
            for (crd::usize r = i; r < ms; ++r)
            {
                detail::simd_axpy<T>(yacc, &a_sub(r, i + 1), a_sub(r, i), ylen);  // contiguous A(r, i+1:ns)
            }
            for (crd::usize jj = i + 1; jj < ns; ++jj)
            {
                ymat(jj, i) = yacc[jj - (i + 1)];
            }
            for (crd::usize c = 0; c < i; ++c)  // tmp = A(i:m,1:i-1)^T * A(i:m,i)  -> ymat(0:i,i)
            {
                T acc{};
                for (crd::usize r = i; r < ms; ++r)
                {
                    acc += a_sub(r, c) * a_sub(r, i);
                }
                ymat(c, i) = acc;
            }
            for (crd::usize jj = i + 1; jj < ns; ++jj)  // Y -= ymat(i+1:n,1:i-1) * tmp
            {
                T acc{};
                for (crd::usize c = 0; c < i; ++c)
                {
                    acc += ymat(jj, c) * ymat(c, i);
                }
                ymat(jj, i) -= acc;
            }
            for (crd::usize c = 0; c < i; ++c)  // tmp = xmat(i:m,1:i-1)^T * A(i:m,i)  -> ymat(0:i,i)
            {
                T acc{};
                for (crd::usize r = i; r < ms; ++r)
                {
                    acc += xmat(r, c) * a_sub(r, i);
                }
                ymat(c, i) = acc;
            }
            for (crd::usize jj = i + 1; jj < ns; ++jj)  // Y -= A(1:i-1,i+1:n)^T * tmp
            {
                T acc{};
                for (crd::usize c = 0; c < i; ++c)
                {
                    acc += a_sub(c, jj) * ymat(c, i);
                }
                ymat(jj, i) -= acc;
            }
            const T tq = tauq[k + i];
            for (crd::usize jj = i + 1; jj < ns; ++jj)  // Y *= tauq(i)
            {
                ymat(jj, i) *= tq;
            }

            // --- Update a_sub(i, i+1:ns) (ref dlabrd 282-285). ---
            for (crd::usize jj = i + 1; jj < ns; ++jj)  // -= ymat(i+1:n,1:i) * A(i,1:i)^T
            {
                T acc{};
                for (crd::usize c = 0; c <= i; ++c)
                {
                    acc += ymat(jj, c) * a_sub(i, c);
                }
                a_sub(i, jj) -= acc;
            }
            for (crd::usize jj = i + 1; jj < ns; ++jj)  // -= A(1:i-1,i+1:n)^T * xmat(i,1:i-1)
            {
                T acc{};
                for (crd::usize c = 0; c < i; ++c)
                {
                    acc += a_sub(c, jj) * xmat(i, c);
                }
                a_sub(i, jj) -= acc;
            }

            // --- Reflector P(i): annihilate a_sub(i, i+2:ns) (contiguous row, in place). ---
            const crd::usize rowlen = ns - (i + 1);
            const auto hp = detail::make_householder<T>(&a_sub(i, i + 1), rowlen);
            taup[k + i] = hp.tau;
            e[k + i] = hp.beta;
            a_sub(i, i + 1) = T{1};  // explicit unit head (restored to e[k+i] by the driver)

            // --- Compute xmat(i+1:ms, i) (ref dlabrd 296-306). ---
            // X = A(i+1:m, i+1:n) * A(i, i+1:n)^T: each xmat(r,i) is a contiguous dot
            // of A-row r against A-row i over the tail columns (SIMD, 8-acc FMA).
            const T* irow = &a_sub(i, i + 1);  // contiguous A(i, i+1:ns)
            const crd::usize xlen = ns - (i + 1);
            for (crd::usize r = i + 1; r < ms; ++r)
            {
                xmat(r, i) = detail::simd_dot<T>(&a_sub(r, i + 1), irow, xlen);
            }
            for (crd::usize c = 0; c <= i; ++c)  // tmp = ymat(i+1:n,1:i)^T * A(i,i+1:n)^T -> xmat(0:i+1,i)
            {
                T acc{};
                for (crd::usize jj = i + 1; jj < ns; ++jj)
                {
                    acc += ymat(jj, c) * a_sub(i, jj);
                }
                xmat(c, i) = acc;
            }
            for (crd::usize r = i + 1; r < ms; ++r)  // X -= A(i+1:m,1:i) * tmp
            {
                T acc{};
                for (crd::usize c = 0; c <= i; ++c)
                {
                    acc += a_sub(r, c) * xmat(c, i);
                }
                xmat(r, i) -= acc;
            }
            for (crd::usize c = 0; c < i; ++c)  // tmp = A(1:i-1,i+1:n) * A(i,i+1:n)^T -> xmat(0:i,i)
            {
                T acc{};
                for (crd::usize jj = i + 1; jj < ns; ++jj)
                {
                    acc += a_sub(c, jj) * a_sub(i, jj);
                }
                xmat(c, i) = acc;
            }
            for (crd::usize r = i + 1; r < ms; ++r)  // X -= xmat(i+1:m,1:i-1) * tmp
            {
                T acc{};
                for (crd::usize c = 0; c < i; ++c)
                {
                    acc += xmat(r, c) * xmat(c, i);
                }
                xmat(r, i) -= acc;
            }
            const T tp = taup[k + i];
            for (crd::usize r = i + 1; r < ms; ++r)  // X *= taup(i)
            {
                xmat(r, i) *= tp;
            }
        }
        else
        {
            taup[k + i] = T{0};
        }
    }
}
} // namespace

// =======================================================================
// bidiagonalize (v3b-1a / v3b-1a-perf) — Golub-Kahan reduction A = Q B P^T to
// upper bidiagonal form (m >= n). Blocked dgebrd (`dlabrd_upper` panels + one
// trailing BLAS-3 rank-2k update per block) for large n, falling through to the
// unblocked dgebd2 (`bidiag_unblocked`) for small n and the final tail. The
// reflector storage layout is identical on both paths, so the downstream
// consumers (form_q_bidiag / form_pt_bidiag / dbdsqr / dlasq2) are unchanged.
// =======================================================================
template <typename T>
void bidiagonalize(T* a, crd::usize m, crd::usize n, crd::usize lda, RealType<T>* d, RealType<T>* e, T* tauq,
                   T* taup, crd::memory::IAllocator* scratch)
{
    static_assert(!is_complex_v<T>, "bidiagonalize: real T only (v3b-1a; complex is v3b-1c)");
    if (n == 0 || m == 0)
    {
        return;
    }

    const crd::usize nb = kBidiagBlock;
    if (n <= 2 * nb)
    {
        bidiag_unblocked<T>(a, m, n, lda, d, e, tauq, taup, scratch);
        return;
    }

    crd::containers::Array<T> xbuf(scratch);  // panel X: ms x nb (ld = nb)
    crd::containers::Array<T> ybuf(scratch);  // panel Y: ns x nb (ld = nb)
    crd::containers::Array<T> tmp(scratch);   // trailing GEMM accumulator
    crd::containers::Array<T> vcol(scratch);  // contiguous left-reflector column
    crd::containers::Array<T> yacc(scratch);  // contiguous Y-column accumulator
    xbuf.resize(m * nb);
    ybuf.resize(n * nb);
    tmp.resize((m - nb) * (n - nb));
    vcol.resize(m);
    yacc.resize(n);

    crd::usize k = 0;
    while (k + nb < n)
    {
        const crd::usize ms = m - k;
        const crd::usize ns = n - k;
        dlabrd_upper<T>(a, lda, k, ms, ns, nb, d, e, tauq, taup, xbuf.data(), ybuf.data(), vcol.data(),
                        yacc.data());

        // Trailing update  As22 -= V*Y_tr^T + X_tr*U  (As22 = A(k+nb:m, k+nb:n)).
        // Two GEMMs accumulate V*Y_tr^T + X_tr*U into `tmp`, then one subtract —
        // the proven blocked-dsytrd pattern (sidesteps strided-output GEMM risk).
        const crd::usize mtr = ms - nb;  // trailing rows
        const crd::usize ntr = ns - nb;  // trailing cols
        MatrixView<const T, Layout::RowMajor> v_view{a + (k + nb) * lda + k, mtr, nb, lda};
        MatrixView<const T, Layout::RowMajor> ytr_view{ybuf.data() + nb * nb, ntr, nb, nb};
        MatrixView<T, Layout::RowMajor> tmp_view{tmp.data(), mtr, ntr, ntr};
        gemm_parallel_auto<T, Layout::RowMajor>(T{1}, v_view, ytr_view, T{0}, tmp_view, Trans::None,
                                                Trans::Transpose, scratch);
        MatrixView<const T, Layout::RowMajor> xtr_view{xbuf.data() + nb * nb, mtr, nb, nb};
        MatrixView<const T, Layout::RowMajor> u_view{a + k * lda + (k + nb), nb, ntr, lda};
        gemm_parallel_auto<T, Layout::RowMajor>(T{1}, xtr_view, u_view, T{1}, tmp_view, Trans::None,
                                                Trans::None, scratch);
        for (crd::usize i = 0; i < mtr; ++i)
        {
            for (crd::usize j = 0; j < ntr; ++j)
            {
                a[(k + nb + i) * lda + (k + nb + j)] -= tmp[i * ntr + j];
            }
        }

        // Restore the panel diagonal/super-diagonal from the explicit-1 form.
        for (crd::usize i = 0; i < nb; ++i)
        {
            a[(k + i) * lda + (k + i)] = d[k + i];
            a[(k + i) * lda + (k + i + 1)] = e[k + i];
        }
        k += nb;
    }

    // Unblocked reduction of the remaining tail A(k:m, k:n).
    bidiag_unblocked<T>(a + k * lda + k, m - k, n - k, lda, d + k, e + k, tauq + k, taup + k, scratch);
}

// =======================================================================
// svd (v3b-1b) — full SVD A = U diag(S) V^T (real f32/f64).
// =======================================================================
template <typename T>
SVD<T> svd(crd::memory::IAllocator* alloc, const Matrix<T>& a_in)
{
    static_assert(!is_complex_v<T>, "svd: real T only (v3b-1b; complex is v3b-1c)");
    using R = RealType<T>;
    const crd::usize m_in = a_in.rows();
    const crd::usize n_in = a_in.cols();

    SVD<T> out(alloc);
    if (m_in == 0 || n_in == 0)
    {
        out.u = Matrix<T>(alloc, m_in, 0);
        out.v = Matrix<T>(alloc, n_in, 0);
        out.s = Vector<R>(alloc, 0);
        return out;
    }

    // Reduce to the m >= n orientation; transpose when wide (D(svd)-3).
    const bool transposed = m_in < n_in;
    const crd::usize m = transposed ? n_in : m_in;  // rows of working W (>= n)
    const crd::usize n = transposed ? m_in : n_in;  // cols of working W

    // Clone (possibly transposed) A into a RowMajor m x n work buffer.
    crd::containers::Array<T> wmat(alloc);
    wmat.resize(m * n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            wmat[i * n + j] = transposed ? a_in.at(j, i) : a_in.at(i, j);
        }
    }

    crd::containers::Array<R> d(alloc);
    crd::containers::Array<R> e(alloc);
    crd::containers::Array<T> tauq(alloc);
    crd::containers::Array<T> taup(alloc);
    d.resize(n);
    e.resize(n);  // e[n-1] unused by dbdsqr (length n-1); kept n for safety
    tauq.resize(n);
    taup.resize(n);

    bidiagonalize<T>(wmat.data(), m, n, n, d.data(), e.data(), tauq.data(), taup.data(), alloc);

    // U_init (m x n) = Q ; VT (n x n) = P^T. dbdsqr then forms U := U*Qb (left
    // singular vectors as columns) and VT := Pb^T * VT (= V^T, right vectors as
    // rows).
    crd::containers::Array<T> uw(alloc);   // m x n
    crd::containers::Array<T> vt(alloc);   // n x n
    crd::containers::Array<T> work(alloc); // 4n
    uw.resize(m * n);
    vt.resize(n * n);
    work.resize(4 * n);
    detail::orgbr_q<T>(wmat.data(), m, n, n, tauq.data(), uw.data(), alloc);
    detail::orgbr_p<T>(wmat.data(), n, n, taup.data(), vt.data(), alloc);

    [[maybe_unused]] const int info =
        detail::dbdsqr<R>(true, static_cast<int>(n), static_cast<int>(n), static_cast<int>(m), 0, d.data(),
                          e.data(), vt.data(), static_cast<int>(n), uw.data(), static_cast<int>(n), nullptr, 1,
                          work.data());
    CRD_ASSERT_MSG(info == 0, "svd: dbdsqr did not converge");

    // Vw = (V^T)^T (n x n): Vw[i][j] = vt[j*n + i].
    crd::containers::Array<T> vw(alloc);
    vw.resize(n * n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            vw[i * n + j] = vt[j * n + i];
        }
    }

    // Sign pin (D(svd)-2): largest-magnitude entry of each V column positive;
    // flip the matching U column to preserve A = U S V^T.
    for (crd::usize k = 0; k < n; ++k)
    {
        crd::usize pivot = 0;
        R best = R{0};
        for (crd::usize r = 0; r < n; ++r)
        {
            const R av = std::abs(vw[r * n + k]);
            if (av > best)
            {
                best = av;
                pivot = r;
            }
        }
        if (vw[pivot * n + k] < R{0})
        {
            for (crd::usize r = 0; r < n; ++r)
            {
                vw[r * n + k] = -vw[r * n + k];
            }
            for (crd::usize r = 0; r < m; ++r)
            {
                uw[r * n + k] = -uw[r * n + k];
            }
        }
    }

    // Assemble the result. min(m,n) = n (working orientation). When transposed,
    // A = Vw S Uw^T, so U_A = Vw (m_in x n), V_A = Uw (n_in x n).
    out.s = Vector<R>(alloc, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        out.s.data()[i] = d[i];
    }
    if (!transposed)
    {
        out.u = Matrix<T>(alloc, m, n);
        for (crd::usize i = 0; i < m * n; ++i)
        {
            out.u.data()[i] = uw[i];
        }
        out.v = Matrix<T>(alloc, n, n);
        for (crd::usize i = 0; i < n * n; ++i)
        {
            out.v.data()[i] = vw[i];
        }
    }
    else
    {
        // U_A (m_in x n) = Vw (which is n x n; m_in == n here).
        out.u = Matrix<T>(alloc, m_in, n);
        for (crd::usize i = 0; i < m_in * n; ++i)
        {
            out.u.data()[i] = vw[i];
        }
        // V_A (n_in x n) = Uw (m x n with m == n_in).
        out.v = Matrix<T>(alloc, n_in, n);
        for (crd::usize i = 0; i < n_in * n; ++i)
        {
            out.v.data()[i] = uw[i];
        }
    }
    return out;
}

// =======================================================================
// svdvals (v3b-1b) — singular values only (descending) via dqds (dlasq2),
// dlasq1-style smax scaling (D(svd)-5).
// =======================================================================
template <typename T>
Vector<RealType<T>> svdvals(crd::memory::IAllocator* alloc, const Matrix<T>& a_in)
{
    static_assert(!is_complex_v<T>, "svdvals: real T only (v3b-1b; complex is v3b-1c)");
    using R = RealType<T>;
    const crd::usize m_in = a_in.rows();
    const crd::usize n_in = a_in.cols();
    const crd::usize nmin = m_in < n_in ? m_in : n_in;

    Vector<R> out(alloc, nmin);
    if (nmin == 0)
    {
        return out;
    }

    const bool transposed = m_in < n_in;
    const crd::usize m = transposed ? n_in : m_in;
    const crd::usize n = transposed ? m_in : n_in;

    crd::containers::Array<T> wmat(alloc);
    wmat.resize(m * n);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            wmat[i * n + j] = transposed ? a_in.at(j, i) : a_in.at(i, j);
        }
    }

    crd::containers::Array<R> d(alloc);
    crd::containers::Array<R> e(alloc);
    crd::containers::Array<T> tauq(alloc);
    crd::containers::Array<T> taup(alloc);
    d.resize(n);
    e.resize(n);
    tauq.resize(n);
    taup.resize(n);
    bidiagonalize<T>(wmat.data(), m, n, n, d.data(), e.data(), tauq.data(), taup.data(), alloc);

    if (n == 1)
    {
        out.data()[0] = std::abs(d[0]);
        return out;
    }

    // dlasq1 scaling: smax over |d|,|e|; scale = sqrt(eps/safmin).
    R smax = R{0};
    for (crd::usize i = 0; i < n; ++i)
    {
        smax = std::max(smax, std::abs(d[i]));
    }
    for (crd::usize i = 0; i + 1 < n; ++i)
    {
        smax = std::max(smax, std::abs(e[i]));
    }
    if (smax == R{0})
    {
        // Already diagonal: |d| sorted descending.
        crd::containers::Array<R> tmp(alloc);
        tmp.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            tmp[i] = std::abs(d[i]);
        }
        std::sort(tmp.data(), tmp.data() + n, [](R x, R y) { return x > y; });
        for (crd::usize i = 0; i < nmin; ++i)
        {
            out.data()[i] = tmp[i];
        }
        return out;
    }

    const R eps = std::numeric_limits<R>::epsilon();
    const R safmin = std::numeric_limits<R>::min();
    const R scale = std::sqrt(eps / safmin);
    const R factor = scale / smax;

    // Compact qd array Z[1..2n-1] = (q1,e1,q2,e2,...,qn), squared + scaled.
    crd::containers::Array<R> z(alloc);
    z.resize(4 * n + 8);
    for (crd::usize i = 0; i < z.size(); ++i)
    {
        z[i] = R{0};
    }
    detail::Z1<R> zz{z.data()};
    for (crd::usize k = 1; k <= n; ++k)
    {
        const R dv = d[k - 1] * factor;
        zz[2 * static_cast<int>(k) - 1] = dv * dv;
    }
    for (crd::usize k = 1; k + 1 <= n; ++k)
    {
        const R ev = e[k - 1] * factor;
        zz[2 * static_cast<int>(k)] = ev * ev;
    }

    const int rc = detail::dlasq2<R>(static_cast<int>(n), zz);
    if (rc == 0)
    {
        // Z[1..n] hold squared singular values descending; unscale + sqrt.
        const R unscale = smax / scale;
        for (crd::usize i = 0; i < nmin; ++i)
        {
            const R q = zz[static_cast<int>(i) + 1];
            out.data()[i] = std::sqrt(q > R{0} ? q : R{0}) * unscale;
        }
    }
    else
    {
        // dqds did not converge: fall back to the full SVD (vectors path is
        // robust via dbdsqr). Rare; keeps svdvals total.
        const SVD<T> full = svd<T>(alloc, a_in);
        for (crd::usize i = 0; i < nmin; ++i)
        {
            out.data()[i] = full.s.data()[i];
        }
    }
    return out;
}

// ---- explicit instantiations (v3b-1a/1b: real f32/f64) ----------------
template void bidiagonalize<float>(float*, crd::usize, crd::usize, crd::usize, float*, float*, float*, float*,
                                   crd::memory::IAllocator*);
template void bidiagonalize<double>(double*, crd::usize, crd::usize, crd::usize, double*, double*, double*,
                                    double*, crd::memory::IAllocator*);
template SVD<float> svd<float>(crd::memory::IAllocator*, const Matrix<float>&);
template SVD<double> svd<double>(crd::memory::IAllocator*, const Matrix<double>&);
template Vector<float> svdvals<float>(crd::memory::IAllocator*, const Matrix<float>&);
template Vector<double> svdvals<double>(crd::memory::IAllocator*, const Matrix<double>&);

} // namespace crd::hesap::dense
