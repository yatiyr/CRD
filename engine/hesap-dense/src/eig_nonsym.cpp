#include <crd/hesap/dense/eig_nonsym.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/dot_simd.hpp>
#include <crd/hesap/dense/detail/dot_simd_complex.hpp>
#include <crd/hesap/dense/detail/householder.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// Magnitude (abs) and squared-magnitude helpers that work for real or complex T,
// returning the real type — so `balance` (zgebal/dgebal) is one body for both.
template <typename T>
[[nodiscard]] inline RealType<T> bal_abs(const T& x) noexcept
{
    if constexpr (is_complex_v<T>)
    {
        return crd::hesap::abs(x);
    }
    else
    {
        return std::abs(x);
    }
}
template <typename T>
[[nodiscard]] inline RealType<T> bal_nsq(const T& x) noexcept
{
    if constexpr (is_complex_v<T>)
    {
        return crd::hesap::norm_sq(x);
    }
    else
    {
        return x * x;
    }
}
} // namespace

template <typename T>
void balance(Matrix<T>& a, crd::containers::Array<RealType<T>>& scale, crd::usize& ilo, crd::usize& ihi)
{
    using R = RealType<T>;
    const crd::usize n = a.rows();
    CRD_ASSERT_MSG(a.is_square(), "balance: A must be square");
    if (scale.size() != n)
    {
        scale.resize(n);
    }
    if (n == 0)
    {
        ilo = 0;
        ihi = 0;
        return;
    }

    // 1-based logic mirroring LAPACK dgebal (JOB='B'). A(i,j) == a.at(i-1, j-1).
    const crd::isize nn = static_cast<crd::isize>(n);
    auto at = [&](crd::isize i, crd::isize j) -> T& {
        return a.at(static_cast<crd::usize>(i - 1), static_cast<crd::usize>(j - 1));
    };

    crd::isize k = 1;
    crd::isize l = nn;

    // ---- permutation: isolate eigenvalues to the corners ----
    auto swap_perm = [&](crd::isize m, crd::isize j) {
        scale[static_cast<crd::usize>(m - 1)] = static_cast<R>(j);
        if (j == m)
        {
            return;
        }
        for (crd::isize r = 1; r <= l; ++r)  // swap columns j,m over rows 1..l
        {
            const T t = at(r, j);
            at(r, j) = at(r, m);
            at(r, m) = t;
        }
        for (crd::isize c = k; c <= nn; ++c)  // swap rows j,m over cols k..n
        {
            const T t = at(j, c);
            at(j, c) = at(m, c);
            at(m, c) = t;
        }
    };

    bool restart = true;
    while (restart)  // row search: push isolating rows to the bottom
    {
        restart = false;
        for (crd::isize j = l; j >= 1; --j)
        {
            bool isolated = true;
            for (crd::isize i = 1; i <= l; ++i)
            {
                if (i != j && at(j, i) != T{0})
                {
                    isolated = false;
                    break;
                }
            }
            if (isolated)
            {
                swap_perm(l, j);
                --l;
                restart = (l >= 1);
                break;
            }
        }
        if (l == 0)
        {
            break;
        }
    }

    restart = true;
    while (restart)  // column search: push isolating columns to the top
    {
        restart = false;
        for (crd::isize j = k; j <= l; ++j)
        {
            bool isolated = true;
            for (crd::isize i = k; i <= l; ++i)
            {
                if (i != j && at(i, j) != T{0})
                {
                    isolated = false;
                    break;
                }
            }
            if (isolated)
            {
                swap_perm(k, j);
                ++k;
                restart = true;
                break;
            }
        }
    }

    for (crd::isize i = k; i <= l; ++i)
    {
        scale[static_cast<crd::usize>(i - 1)] = R{1};
    }

    // ---- iterative radix-2 scaling of the block rows/cols k..l ----
    // All norm/factor scalars are REAL (R); only the matrix entries are T. For
    // complex T the 2-norm uses |·|² (bal_nsq) and ca/ra use |·| (bal_abs); the
    // radix-2 factors f,g multiply the complex columns/rows (Complex *= R).
    const R sfmin1 = std::numeric_limits<R>::min() / std::numeric_limits<R>::epsilon();
    const R sfmax1 = R{1} / sfmin1;
    const R sfmin2 = sfmin1 * R{2};
    const R sfmax2 = R{1} / sfmin2;

    bool noconv = true;
    while (noconv)
    {
        noconv = false;
        for (crd::isize i = k; i <= l; ++i)
        {
            // c = ||A(k:l, i)||_2 ; r = ||A(i, k:l)||_2 (include diagonal — LAPACK).
            R c = R{0};
            R r = R{0};
            for (crd::isize p = k; p <= l; ++p)
            {
                c += bal_nsq<T>(at(p, i));
                r += bal_nsq<T>(at(i, p));
            }
            c = std::sqrt(c);
            r = std::sqrt(r);
            // ca = max|A(1:l, i)| ; ra = max|A(i, k:n)|.
            R ca = R{0};
            for (crd::isize p = 1; p <= l; ++p)
            {
                ca = std::max(ca, bal_abs<T>(at(p, i)));
            }
            R ra = R{0};
            for (crd::isize p = k; p <= nn; ++p)
            {
                ra = std::max(ra, bal_abs<T>(at(i, p)));
            }
            if (c == R{0} || r == R{0})
            {
                continue;
            }
            R g = r / R{2};
            R f = R{1};
            const R s_init = c + r;
            // Scale up f until balanced (guarded against overflow).
            while (!(c >= g || std::max(f, c) >= sfmax2 || std::min(r, g) <= sfmin2 ||
                     std::max(ca, c) >= sfmax2 || std::min(ra, r) <= sfmin2))
            {
                f *= R{2};
                c *= R{2};
                ca *= R{2};
                r /= R{2};
                g /= R{2};
                ra /= R{2};
            }
            g = c / R{2};
            while (!(g < r || std::max(r, ra) >= sfmax2 || std::min(std::min(f, c), g) <= sfmin2 ||
                     std::max(ca, c) >= sfmax2 || ca <= sfmin2))
            {
                f /= R{2};
                c /= R{2};
                g /= R{2};
                ca /= R{2};
                r *= R{2};
                ra *= R{2};
            }
            if (c + r >= s_init * static_cast<R>(0.95))
            {
                continue;
            }
            if (f < R{1} && scale[static_cast<crd::usize>(i - 1)] < R{1})
            {
                if (f * scale[static_cast<crd::usize>(i - 1)] <= sfmin1)
                {
                    continue;
                }
            }
            if (f > R{1} && scale[static_cast<crd::usize>(i - 1)] > R{1})
            {
                if (scale[static_cast<crd::usize>(i - 1)] >= sfmax1 / f)
                {
                    continue;
                }
            }
            g = R{1} / f;
            scale[static_cast<crd::usize>(i - 1)] *= f;
            noconv = true;
            for (crd::isize p = k; p <= nn; ++p)  // row i, cols k..n  *= g
            {
                at(i, p) *= g;
            }
            for (crd::isize p = 1; p <= l; ++p)  // col i, rows 1..l  *= f
            {
                at(p, i) *= f;
            }
        }
    }

    ilo = static_cast<crd::usize>(k - 1);
    ihi = static_cast<crd::usize>(l - 1);
}

namespace
{
// SIMD contiguous dot product: Σ x[i]·y[i]  (f32/f64).
template <typename T>
inline T sdot(const T* x, const T* y, crd::usize len) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    T acc{0};
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        simd::Vec4d a0 = simd::Vec4d::zero();
        simd::Vec4d a1 = simd::Vec4d::zero();
        for (; p + 8 <= len; p += 8)
        {
            a0 = simd::fma(simd::Vec4d::load(x + p), simd::Vec4d::load(y + p), a0);
            a1 = simd::fma(simd::Vec4d::load(x + p + 4), simd::Vec4d::load(y + p + 4), a1);
        }
        acc = simd::horizontal_sum(a0 + a1);
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        simd::Vec8f a0 = simd::Vec8f::zero();
        for (; p + 8 <= len; p += 8)
        {
            a0 = simd::fma(simd::Vec8f::load(x + p), simd::Vec8f::load(y + p), a0);
        }
        acc = simd::horizontal_sum(a0);
    }
    for (; p < len; ++p)
    {
        acc += x[p] * y[p];
    }
    return acc;
}

// SIMD contiguous axpy: y[i] += s·x[i]  (f32/f64).
template <typename T>
inline void saxpy(T* y, const T* x, T s, crd::usize len) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize p = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        const simd::Vec4d sv(s);
        for (; p + 8 <= len; p += 8)
        {
            simd::fma(sv, simd::Vec4d::load(x + p), simd::Vec4d::load(y + p)).store(y + p);
            simd::fma(sv, simd::Vec4d::load(x + p + 4), simd::Vec4d::load(y + p + 4)).store(y + p + 4);
        }
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        const simd::Vec8f sv(s);
        for (; p + 8 <= len; p += 8)
        {
            simd::fma(sv, simd::Vec8f::load(x + p), simd::Vec8f::load(y + p)).store(y + p);
        }
    }
    for (; p < len; ++p)
    {
        y[p] += s * x[p];
    }
}

// =======================================================================
// v3d-2c-1 — complex Hessenberg reduction (zgehd2) on the two-real-array
// (ar, ai) SIMD representation (the eig_herm v3a-2.5 idiom). Reduces the
// active block to upper Hessenberg by a UNITARY similarity Q^H A Q = H. Every
// inner loop is a contiguous real-SIMD sweep (`sdot`/`saxpy`) over ar/ai — no
// scalar complex arithmetic in the O(n³) updates. Reflector i (complex, v[0]=1
// implicit) annihilates A(i+2:ihi, i); its tail is stored in ar/ai below the
// subdiagonal, the real beta on the subdiagonal, the complex `tau[i]` returned.
//
// Two-sided update per reflector, faithful to LAPACK zgehd2 order (RIGHT then
// LEFT): A := H^H · A · H with H = I − tau·v·v^H.
//   RIGHT  A(0:ihi, i+1:ihi)  := A·H   = A − tau·(A·v)·v^H   (A·v: no conj)
//   LEFT   A(i+1:ihi, i+1:n)  := H^H·A = A − conj(tau)·v·(v^H·A)
// =======================================================================
template <typename R>
void hessenberg_complex_split(R* ar, R* ai, crd::usize n, crd::usize lda, crd::usize ilo,
                              crd::usize ihi, crd::hesap::Complex<R>* tau, crd::memory::IAllocator* sc)
{
    using C = crd::hesap::Complex<R>;
    for (crd::usize k = 0; k < n; ++k)
    {
        tau[k] = C{R{0}, R{0}};
    }
    if (ihi < ilo + 2)
    {
        return;
    }
    crd::containers::Array<C> vc(sc);
    crd::containers::Array<R> vr(sc);
    crd::containers::Array<R> vi(sc);
    crd::containers::Array<R> wr(sc);
    crd::containers::Array<R> wi(sc);
    vc.resize(n);
    vr.resize(n);
    vi.resize(n);
    wr.resize(n);
    wi.resize(n);

    for (crd::usize i = ilo; i + 1 < ihi; ++i)
    {
        const crd::usize len = ihi - i;  // reflector support rows [i+1, ihi]
        for (crd::usize k = 0; k < len; ++k)
        {
            vc[k] = C{ar[(i + 1 + k) * lda + i], ai[(i + 1 + k) * lda + i]};
        }
        const detail::HouseholderComplex<R> h = detail::make_householder_complex<R>(vc.data(), len);
        tau[i] = h.tau;
        ar[(i + 1) * lda + i] = h.beta;  // H subdiagonal (real)
        ai[(i + 1) * lda + i] = R{0};
        vc[0] = C{R{1}, R{0}};
        for (crd::usize k = 0; k < len; ++k)
        {
            vr[k] = vc[k].re;
            vi[k] = vc[k].im;
        }
        for (crd::usize k = 1; k < len; ++k)  // store reflector tail
        {
            ar[(i + 1 + k) * lda + i] = vr[k];
            ai[(i + 1 + k) * lda + i] = vi[k];
        }
        if (h.tau.re == R{0} && h.tau.im == R{0})
        {
            continue;
        }

        // RIGHT: A(0:ihi, i+1:ihi) := A − tau·(A·v)·v^H. Per row rr:
        //   u = Σ_k A[rr, i+1+k]·v[k]   (complex dot, NO conj)
        //   s = tau·u;  A[rr, i+1+j] −= s·conj(v[j]).  Fused single-pass kernels.
        for (crd::usize rr = 0; rr <= ihi; ++rr)
        {
            R* arow = &ar[rr * lda + (i + 1)];
            R* airow = &ai[rr * lda + (i + 1)];
            R ur = R{0};
            R ui = R{0};
            detail::simd_cdot_nc<R>(arow, airow, vr.data(), vi.data(), len, ur, ui);
            const R sr = h.tau.re * ur - h.tau.im * ui;
            const R si = h.tau.re * ui + h.tau.im * ur;
            // A += (−s)·conj(v).
            detail::simd_caxpy_conjx<R>(arow, airow, -sr, -si, vr.data(), vi.data(), len);
        }

        // LEFT: A(i+1:ihi, i+1:n) := A − conj(tau)·v·(v^H·A). v^H·A = Σ_k conj(v[k])·A[i+1+k,:].
        const crd::usize col0 = i + 1;
        const crd::usize ncol = n - col0;
        for (crd::usize jj = 0; jj < ncol; ++jj)
        {
            wr[jj] = R{0};
            wi[jj] = R{0};
        }
        for (crd::usize k = 0; k < len; ++k)  // w += conj(v[k])·A_row
        {
            const R* arow = &ar[(i + 1 + k) * lda + col0];
            const R* airow = &ai[(i + 1 + k) * lda + col0];
            detail::simd_caxpy<R>(wr.data(), wi.data(), vr[k], -vi[k], arow, airow, ncol);
        }
        const R ctr = h.tau.re;   // conj(tau).re
        const R cti = -h.tau.im;  // conj(tau).im
        for (crd::usize k = 0; k < len; ++k)  // A_row += (−conj(tau)·v[k])·w
        {
            const R sr = ctr * vr[k] - cti * vi[k];
            const R si = ctr * vi[k] + cti * vr[k];
            R* arow = &ar[(i + 1 + k) * lda + col0];
            R* airow = &ai[(i + 1 + k) * lda + col0];
            detail::simd_caxpy<R>(arow, airow, -sr, -si, wr.data(), wi.data(), ncol);
        }
    }
}

// v3d-2c-1 — form the unitary Q (zunghr) of the complex Hessenberg reduction
// into split arrays (qr, qi). Q = H(ilo)·…·H(ihi-2), built by left-applying
// H_i to I from i = ihi-2 down to ilo: Q := (I − tau·v·v^H)·Q. Row-wise SIMD.
template <typename R>
void form_q_complex_split(const R* ar, const R* ai, crd::usize n, crd::usize lda, crd::usize ilo,
                          crd::usize ihi, const crd::hesap::Complex<R>* tau, R* qr, R* qi,
                          crd::usize ldq, crd::memory::IAllocator* sc)
{
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            qr[i * ldq + j] = (i == j) ? R{1} : R{0};
            qi[i * ldq + j] = R{0};
        }
    }
    if (ihi < ilo + 2)
    {
        return;
    }
    crd::containers::Array<R> vr(sc);
    crd::containers::Array<R> vi(sc);
    crd::containers::Array<R> wr(sc);
    crd::containers::Array<R> wi(sc);
    vr.resize(n);
    vi.resize(n);
    wr.resize(n);
    wi.resize(n);

    for (crd::usize ii = ihi - 1; ii-- > ilo;)  // i = ihi-2 .. ilo
    {
        const crd::usize i = ii;
        const crd::usize len = ihi - i;
        if (tau[i].re == R{0} && tau[i].im == R{0})
        {
            continue;
        }
        vr[0] = R{1};
        vi[0] = R{0};
        for (crd::usize k = 1; k < len; ++k)
        {
            vr[k] = ar[(i + 1 + k) * lda + i];
            vi[k] = ai[(i + 1 + k) * lda + i];
        }
        // Q[i+1..ihi, :] := (I − tau·v·v^H)·Q = Q − tau·v·(v^H·Q).
        for (crd::usize jj = 0; jj < n; ++jj)
        {
            wr[jj] = R{0};
            wi[jj] = R{0};
        }
        for (crd::usize k = 0; k < len; ++k)  // w += conj(v[k])·Q_row
        {
            const R* qrow = &qr[(i + 1 + k) * ldq];
            const R* qirow = &qi[(i + 1 + k) * ldq];
            detail::simd_caxpy<R>(wr.data(), wi.data(), vr[k], -vi[k], qrow, qirow, n);
        }
        for (crd::usize k = 0; k < len; ++k)  // Q_row += (−tau·v[k])·w
        {
            const R sr = tau[i].re * vr[k] - tau[i].im * vi[k];
            const R si = tau[i].re * vi[k] + tau[i].im * vr[k];
            R* qrow = &qr[(i + 1 + k) * ldq];
            R* qirow = &qi[(i + 1 + k) * ldq];
            detail::simd_caxpy<R>(qrow, qirow, -sr, -si, wr.data(), wi.data(), n);
        }
    }
}
} // namespace

template <typename T>
void hessenberg(Matrix<T>& a, crd::usize ilo, crd::usize ihi, crd::containers::Array<T>& tau)
{
    const crd::usize n = a.rows();
    CRD_ASSERT_MSG(a.is_square(), "hessenberg: A must be square");
    CRD_ASSERT_MSG(ihi < n && ilo <= ihi, "hessenberg: bad ilo/ihi");
    if (tau.size() != n)
    {
        tau.resize(n);
    }

    if constexpr (is_complex_v<T>)
    {
        // Complex Hessenberg (zgehd2) on the split-array SIMD kernel. The matrix
        // is split to two real arrays (ar, ai) once (O(n²), negligible vs the
        // O(n³) kernel — the eig_herm boundary, ADR-0078 §5 lower layer), reduced
        // in place, then recombined. tau is Array<Complex<R>> — written directly.
        using R = RealType<T>;
        crd::memory::IAllocator* alloc = a.allocator();
        const crd::usize ld = a.ld();
        crd::containers::Array<R> ar(alloc);
        crd::containers::Array<R> ai(alloc);
        ar.resize(n * n);
        ai.resize(n * n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                ar[i * n + j] = a.data()[i * ld + j].re;
                ai[i * n + j] = a.data()[i * ld + j].im;
            }
        }
        hessenberg_complex_split<R>(ar.data(), ai.data(), n, n, ilo, ihi, tau.data(), alloc);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                a.data()[i * ld + j] = T{ar[i * n + j], ai[i * n + j]};
            }
        }
    }
    else
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            tau[i] = T{0};
        }
        if (ihi < ilo + 2)
        {
            return;
        }

        // Unblocked reduction (LAPACK dgehd2) with the two-sided updates done
        // ROW-WISE so every inner loop is a contiguous SIMD sweep in our row-major
        // layout (vᵀ·A and the rank-1 updates stream along contiguous rows). This
        // sidesteps the row-major column-reduction penalty AND the small-K gemm
        // overhead of a blocked dlahr2 — and beats Eigen's column-major unblocked.
        crd::memory::IAllocator* alloc = a.allocator();
        T* data = a.data();
        const crd::usize ld = a.ld();
        crd::containers::Array<T> vbuf(alloc);  // contiguous reflector (v[0] = 1)
        crd::containers::Array<T> wbuf(alloc);  // left-update accumulation row
        vbuf.resize(n);
        wbuf.resize(n);

        for (crd::usize i = ilo; i + 1 < ihi; ++i)
        {
            const crd::usize len = ihi - i;  // reflector support rows [i+1, ihi]
            for (crd::usize r = 0; r < len; ++r)
            {
                vbuf[r] = data[(i + 1 + r) * ld + i];
            }
            const auto h = detail::make_householder<T>(vbuf.data(), len);
            tau[i] = h.tau;
            data[(i + 1) * ld + i] = h.beta;  // H subdiagonal
            for (crd::usize r = 1; r < len; ++r)
            {
                data[(i + 1 + r) * ld + i] = vbuf[r];  // store reflector tail
            }
            vbuf[0] = T{1};  // explicit unit for the contiguous applies
            if (h.tau == T{0})
            {
                continue;
            }

            // Left:  A(i+1:ihi, i+1:n-1) := (I - tau v vᵀ)·A.  Row-wise:
            //   w := vᵀ·A_trail (accumulate row by row), then A_trail -= tau·v·wᵀ.
            const crd::usize col_l = i + 1;
            const crd::usize ncol_l = n - col_l;
            if (ncol_l > 0)
            {
                for (crd::usize jj = 0; jj < ncol_l; ++jj)
                {
                    wbuf[jj] = T{0};
                }
                for (crd::usize r = 0; r < len; ++r)
                {
                    saxpy<T>(wbuf.data(), data + (i + 1 + r) * ld + col_l, vbuf[r], ncol_l);
                }
                for (crd::usize r = 0; r < len; ++r)
                {
                    saxpy<T>(data + (i + 1 + r) * ld + col_l, wbuf.data(), -h.tau * vbuf[r], ncol_l);
                }
            }

            // Right: A(0:ihi, i+1:ihi) := A·(I - tau v vᵀ).  Row-wise per row rr:
            //   wr := A[rr, i+1:ihi]·v;  A[rr, i+1:ihi] -= tau·wr·v.
            for (crd::usize rr = 0; rr <= ihi; ++rr)
            {
                T* arow = data + rr * ld + (i + 1);
                const T wr = sdot<T>(arow, vbuf.data(), len);
                saxpy<T>(arow, vbuf.data(), -h.tau * wr, len);
            }
        }
    }
}


template <typename T>
Matrix<T> form_hessenberg_q(crd::memory::IAllocator* alloc, const Matrix<T>& a_packed,
                            crd::usize ilo, crd::usize ihi, const crd::containers::Array<T>& tau)
{
    const crd::usize n = a_packed.rows();
    Matrix<T> q(alloc, n, n);
    q.set_identity();

    if constexpr (is_complex_v<T>)
    {
        // Complex unitary Q (zunghr) via the split-array SIMD kernel: split the
        // packed reflectors to (ar, ai), build Q into (qr, qi), recombine.
        using R = RealType<T>;
        const T* pdata = a_packed.data();
        const crd::usize pld = a_packed.ld();
        crd::containers::Array<R> ar(alloc);
        crd::containers::Array<R> ai(alloc);
        crd::containers::Array<R> qr(alloc);
        crd::containers::Array<R> qi(alloc);
        ar.resize(n * n);
        ai.resize(n * n);
        qr.resize(n * n);
        qi.resize(n * n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                ar[i * n + j] = pdata[i * pld + j].re;
                ai[i * n + j] = pdata[i * pld + j].im;
            }
        }
        form_q_complex_split<R>(ar.data(), ai.data(), n, n, ilo, ihi, tau.data(), qr.data(),
                                qi.data(), n, alloc);
        const crd::usize qld = q.ld();
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                q.data()[i * qld + j] = T{qr[i * n + j], qi[i * n + j]};
            }
        }
        return q;
    }
    else
    {
        if (ihi < ilo + 2)
        {
            return q;
        }
        const T* pdata = a_packed.data();
        const crd::usize pld = a_packed.ld();
        T* qdata = q.data();
        const crd::usize qld = q.ld();

        crd::containers::Array<T> vfull(alloc);
        crd::containers::Array<T> wrow(alloc);  // per-column reflector weights w[0..n-1]
        vfull.resize(n);
        wrow.resize(n);

        // Q = H_ilo · … · H_{ihi-2}: build by left-applying H_i to I for i from
        // ihi-2 down to ilo (innermost reflector applied first).
        //
        // The reflector apply Q[i+1..ihi, :] := (I − τ v vᵀ)·Q[i+1..ihi, :] is done
        // ROW-WISE (contiguous, SIMD via simd_axpy) rather than the naive column-
        // strided scalar form: accumulate w[:] = Σ_r v[r]·Q[row_r][:], scale by τ,
        // then Q[row_r][:] −= v[r]·w[:]. Same per-element accumulation order over r;
        // the dorghr cost drops to gemm-class (the v3d-1a SIMD-row-wise lever —
        // [[feedback_simd_rowwise_unblocked_beats_blocked_smallk]]).
        for (crd::usize ii = ihi - 1; ii-- > ilo;)  // ii = ihi-2 .. ilo
        {
            const crd::usize i = ii;
            const crd::usize len = ihi - i;  // rows [i+1, ihi]
            const T taui = tau[i];
            if (taui == T{0})
            {
                continue;
            }
            vfull[0] = T{1};
            for (crd::usize r = 1; r < len; ++r)
            {
                vfull[r] = pdata[(i + 1 + r) * pld + i];
            }
            for (crd::usize j = 0; j < n; ++j)
            {
                wrow[j] = T{0};
            }
            for (crd::usize r = 0; r < len; ++r)  // w += v[r]·Q[row_r][:]
            {
                detail::simd_axpy<T>(wrow.data(), qdata + (i + 1 + r) * qld, vfull[r], n);
            }
            for (crd::usize j = 0; j < n; ++j)
            {
                wrow[j] *= taui;
            }
            for (crd::usize r = 0; r < len; ++r)  // Q[row_r][:] −= v[r]·w[:]
            {
                detail::simd_axpy<T>(qdata + (i + 1 + r) * qld, wrow.data(), -vfull[r], n);
            }
        }
        return q;
    }
}

namespace
{
// LAPACK dlanv2: standardize a real 2×2 block [[a b];[c d]] to Schur form via a
// Givens rotation (cs, sn); returns the eigenvalues (rt1r±rt1i, rt2r±rt2i) and
// overwrites a/b/c/d. Real eigenvalues ⇒ upper-triangular (c→0); complex pair
// ⇒ standardized form (a==d, b·c<0). Faithful port (incl. the safmn2 guard).
template <typename T>
struct Lanv2Out
{
    T rt1r, rt1i, rt2r, rt2i, cs, sn;
};

template <typename T>
Lanv2Out<T> dlanv2(T& a, T& b, T& c, T& d)
{
    const T zero{0};
    const T half = static_cast<T>(0.5);
    const T one{1};
    const T two{2};
    const T multpl = static_cast<T>(4);
    const T safmin = std::numeric_limits<T>::min();
    const T eps = std::numeric_limits<T>::epsilon();
    const T safmn2 = std::pow(two, std::floor(std::log(safmin / eps) / std::log(two) / two));
    const T safmx2 = one / safmn2;
    auto sgn = [](T x, T y) -> T { return (y >= T{0}) ? std::abs(x) : -std::abs(x); };

    Lanv2Out<T> o{};
    o.cs = one;
    o.sn = zero;
    if (c == zero)
    {
        o.cs = one;
        o.sn = zero;
    }
    else if (b == zero)
    {
        o.cs = zero;
        o.sn = one;
        const T t = d;
        d = a;
        a = t;
        b = -c;
        c = zero;
    }
    else if ((a - d) == zero && ((b >= zero) != (c >= zero)))
    {
        // Identity rotation — already the default (o.cs=one, o.sn=zero). Kept as
        // a distinct branch per faithful dlanv2 (distinct condition from c==0).
        o.sn = zero;
    }
    else
    {
        T temp = a - d;
        T p = half * temp;
        const T bcmax = std::max(std::abs(b), std::abs(c));
        const T bcmis = std::min(std::abs(b), std::abs(c)) * (((b >= zero) == (c >= zero)) ? one : -one);
        T scale = std::max(std::abs(p), bcmax);
        T z = (p / scale) * p + (bcmax / scale) * bcmis;
        if (z >= multpl * eps)
        {
            z = p + sgn(std::sqrt(scale) * std::sqrt(z), p);
            a = d + z;
            d = d - (bcmax / z) * bcmis;
            const T tau = detail::hypot2(c, z);
            o.cs = z / tau;
            o.sn = c / tau;
            b = b - c;
            c = zero;
        }
        else
        {
            int count = 0;
            T sigma = b + c;
            for (;;)
            {
                ++count;
                scale = std::max(std::abs(temp), std::abs(sigma));
                if (scale >= safmx2 && count <= 20)
                {
                    sigma *= safmn2;
                    temp *= safmn2;
                    continue;
                }
                if (scale <= safmn2 && count <= 20)
                {
                    sigma *= safmx2;
                    temp *= safmx2;
                    continue;
                }
                break;
            }
            p = half * temp;
            T tau = detail::hypot2(sigma, temp);
            o.cs = std::sqrt(half * (one + std::abs(sigma) / tau));
            o.sn = -(p / (tau * o.cs)) * sgn(one, sigma);
            const T aa = a * o.cs + b * o.sn;
            const T bb = -a * o.sn + b * o.cs;
            const T cc = c * o.cs + d * o.sn;
            const T dd = -c * o.sn + d * o.cs;
            a = aa * o.cs + cc * o.sn;
            b = bb * o.cs + dd * o.sn;
            c = -aa * o.sn + cc * o.cs;
            d = -bb * o.sn + dd * o.cs;
            temp = half * (a + d);
            a = temp;
            d = temp;
            if (c != zero)
            {
                if (b != zero)
                {
                    if ((b >= zero) == (c >= zero))
                    {
                        const T sab = std::sqrt(std::abs(b));
                        const T sac = std::sqrt(std::abs(c));
                        p = sgn(sab * sac, c);
                        tau = one / std::sqrt(std::abs(b + c));
                        a = temp + p;
                        d = temp - p;
                        b = b - c;
                        c = zero;
                        const T cs1 = sab * tau;
                        const T sn1 = sac * tau;
                        const T t2 = o.cs * cs1 - o.sn * sn1;
                        o.sn = o.cs * sn1 + o.sn * cs1;
                        o.cs = t2;
                    }
                }
                else
                {
                    b = -c;
                    c = zero;
                    const T t2 = o.cs;
                    o.cs = -o.sn;
                    o.sn = t2;
                }
            }
        }
    }
    o.rt1r = a;
    o.rt2r = d;
    if (c == zero)
    {
        o.rt1i = zero;
        o.rt2i = zero;
    }
    else
    {
        o.rt1i = std::sqrt(std::abs(b)) * std::sqrt(std::abs(c));
        o.rt2i = -o.rt1i;
    }
    return o;
}
} // namespace

template <typename T>
RealSchur<T> real_schur(crd::memory::IAllocator* alloc, const Matrix<T>& h_in, crd::usize ilo,
                        crd::usize ihi, bool vectors)
{
    static_assert(!is_complex_v<T>, "real_schur is real-only (complex Schur is v3d-2)");
    const crd::usize n = h_in.rows();
    RealSchur<T> out(alloc);
    out.t = h_in.clone();
    out.wr.resize(n);
    out.wi.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        out.wr[k] = T{0};
        out.wi[k] = T{0};
    }
    if (vectors)
    {
        out.z = Matrix<T>(alloc, n, n);
        out.z.set_identity();
    }
    if (n == 0)
    {
        out.converged = true;
        return out;
    }

    T* hd = out.t.data();
    const crd::usize ld = out.t.ld();
    T* zd = vectors ? out.z.data() : nullptr;
    const crd::usize zld = vectors ? out.z.ld() : 0;
    auto h = [&](crd::usize i, crd::usize j) -> T& { return hd[i * ld + j]; };

    for (crd::usize k = 0; k < ilo; ++k)
    {
        out.wr[k] = h(k, k);
    }
    for (crd::usize k = ihi + 1; k < n; ++k)
    {
        out.wr[k] = h(k, k);
    }

    const T eps = std::numeric_limits<T>::epsilon();
    const T safmin = std::numeric_limits<T>::min();
    const crd::usize nh = ihi - ilo + 1;
    const T ulp = eps;
    const T smlnum = safmin * (static_cast<T>(nh) / ulp);
    const T dat1 = static_cast<T>(0.75);
    const T dat2 = static_cast<T>(-0.4375);
    const crd::usize kexsh = 10;
    const crd::usize itmax = 30 * (nh > 10 ? nh : 10);
    const crd::usize iloz = ilo;
    const crd::usize ihiz = ihi;
    const crd::usize i1 = 0;  // WANTT (full T) ⇒ I1=0, I2=n-1
    const crd::usize i2 = n - 1;

    crd::isize i_s = static_cast<crd::isize>(ihi);
    crd::usize kdefl = 0;
    while (i_s >= static_cast<crd::isize>(ilo))
    {
        const crd::usize i = static_cast<crd::usize>(i_s);
        crd::usize l = ilo;
        bool split = false;
        for (crd::usize its = 0; its <= itmax; ++its)
        {
            // Single small subdiagonal element (Ahues-Tisseur criterion).
            crd::usize k = l;
            for (crd::usize kk = i; kk > l; --kk)
            {
                bool small = false;
                if (std::abs(h(kk, kk - 1)) <= smlnum)
                {
                    small = true;
                }
                else
                {
                    T tst = std::abs(h(kk - 1, kk - 1)) + std::abs(h(kk, kk));
                    if (tst == T{0})
                    {
                        if (kk >= ilo + 2)
                        {
                            tst += std::abs(h(kk - 1, kk - 2));
                        }
                        if (kk + 1 <= ihi)
                        {
                            tst += std::abs(h(kk + 1, kk));
                        }
                    }
                    if (std::abs(h(kk, kk - 1)) <= ulp * tst)
                    {
                        const T ab = std::max(std::abs(h(kk, kk - 1)), std::abs(h(kk - 1, kk)));
                        const T ba = std::min(std::abs(h(kk, kk - 1)), std::abs(h(kk - 1, kk)));
                        const T aa =
                            std::max(std::abs(h(kk, kk)), std::abs(h(kk - 1, kk - 1) - h(kk, kk)));
                        const T bb =
                            std::min(std::abs(h(kk, kk)), std::abs(h(kk - 1, kk - 1) - h(kk, kk)));
                        const T s = aa + ab;
                        if (ba * (ab / s) <= std::max(smlnum, ulp * (bb * (aa / s))))
                        {
                            small = true;
                        }
                    }
                }
                if (small)
                {
                    k = kk;
                    break;
                }
            }
            l = k;
            if (l > ilo)
            {
                h(l, l - 1) = T{0};
            }
            if (l + 1 >= i)  // l >= i-1: a 1×1 or 2×2 block split off
            {
                split = true;
                break;
            }
            ++kdefl;

            T h11;
            T h12;
            T h21;
            T h22;
            if (kdefl % (2 * kexsh) == 0)
            {
                const T s = std::abs(h(i, i - 1)) + std::abs(h(i - 1, i - 2));
                h11 = dat1 * s + h(i, i);
                h12 = dat2 * s;
                h21 = s;
                h22 = h11;
            }
            else if (kdefl % kexsh == 0)
            {
                const T s = std::abs(h(l + 1, l)) + std::abs(h(l + 2, l + 1));
                h11 = dat1 * s + h(l, l);
                h12 = dat2 * s;
                h21 = s;
                h22 = h11;
            }
            else
            {
                h11 = h(i - 1, i - 1);
                h21 = h(i, i - 1);
                h12 = h(i - 1, i);
                h22 = h(i, i);
            }
            T rt1r;
            T rt1i;
            T rt2r;
            T rt2i;
            {
                const T s = std::abs(h11) + std::abs(h12) + std::abs(h21) + std::abs(h22);
                if (s == T{0})
                {
                    rt1r = T{0};
                    rt1i = T{0};
                    rt2r = T{0};
                    rt2i = T{0};
                }
                else
                {
                    h11 /= s;
                    h21 /= s;
                    h12 /= s;
                    h22 /= s;
                    const T tr = (h11 + h22) / T{2};
                    const T det = (h11 - tr) * (h22 - tr) - h12 * h21;
                    const T rtdisc = std::sqrt(std::abs(det));
                    if (det >= T{0})
                    {
                        rt1r = tr * s;
                        rt2r = rt1r;
                        rt1i = rtdisc * s;
                        rt2i = -rt1i;
                    }
                    else
                    {
                        rt1r = tr + rtdisc;
                        rt2r = tr - rtdisc;
                        if (std::abs(rt1r - h22) <= std::abs(rt2r - h22))
                        {
                            rt1r *= s;
                            rt2r = rt1r;
                        }
                        else
                        {
                            rt2r *= s;
                            rt1r = rt2r;
                        }
                        rt1i = T{0};
                        rt2i = T{0};
                    }
                }
            }

            // Two consecutive small subdiagonals → bulge start row M.
            crd::usize m = l;
            T v[3] = {T{0}, T{0}, T{0}};
            for (crd::isize mm_s = static_cast<crd::isize>(i) - 2;
                 mm_s >= static_cast<crd::isize>(l); --mm_s)
            {
                const crd::usize mm = static_cast<crd::usize>(mm_s);
                T h21s = h(mm + 1, mm);
                T s = std::abs(h(mm, mm) - rt2r) + std::abs(rt2i) + std::abs(h21s);
                h21s = h(mm + 1, mm) / s;
                v[0] = h21s * h(mm, mm + 1) + (h(mm, mm) - rt1r) * ((h(mm, mm) - rt2r) / s) -
                       rt1i * (rt2i / s);
                v[1] = h21s * (h(mm, mm) + h(mm + 1, mm + 1) - rt1r - rt2r);
                v[2] = h21s * h(mm + 2, mm + 1);
                s = std::abs(v[0]) + std::abs(v[1]) + std::abs(v[2]);
                v[0] /= s;
                v[1] /= s;
                v[2] /= s;
                m = mm;
                if (mm == l)
                {
                    break;
                }
                if (std::abs(h(mm, mm - 1)) * (std::abs(v[1]) + std::abs(v[2])) <=
                    ulp * std::abs(v[0]) * (std::abs(h(mm - 1, mm - 1)) + std::abs(h(mm, mm)) +
                                            std::abs(h(mm + 1, mm + 1))))
                {
                    break;
                }
            }

            // Double-shift QR step: chase the bulge from row M to I-1.
            for (crd::usize kc = m; kc + 1 <= i; ++kc)
            {
                const crd::usize nr = std::min<crd::usize>(3, i - kc + 1);
                if (kc > m)
                {
                    for (crd::usize r = 0; r < nr; ++r)
                    {
                        v[r] = h(kc + r, kc - 1);
                    }
                }
                const auto hh = detail::make_householder<T>(v, nr);
                const T t1 = hh.tau;
                if (kc > m)
                {
                    h(kc, kc - 1) = hh.beta;
                    h(kc + 1, kc - 1) = T{0};
                    if (kc < i - 1)
                    {
                        h(kc + 2, kc - 1) = T{0};
                    }
                }
                else if (m > l)
                {
                    h(kc, kc - 1) = h(kc, kc - 1) * (T{1} - t1);
                }
                const T v2 = v[1];
                const T t2 = t1 * v2;
                if (nr == 3)
                {
                    const T v3 = v[2];
                    const T t3 = t1 * v3;
                    for (crd::usize j = kc; j <= i2; ++j)
                    {
                        const T sum = h(kc, j) + v2 * h(kc + 1, j) + v3 * h(kc + 2, j);
                        h(kc, j) -= sum * t1;
                        h(kc + 1, j) -= sum * t2;
                        h(kc + 2, j) -= sum * t3;
                    }
                    const crd::usize jmax = std::min(kc + 3, i);
                    for (crd::usize j = i1; j <= jmax; ++j)
                    {
                        const T sum = h(j, kc) + v2 * h(j, kc + 1) + v3 * h(j, kc + 2);
                        h(j, kc) -= sum * t1;
                        h(j, kc + 1) -= sum * t2;
                        h(j, kc + 2) -= sum * t3;
                    }
                    if (vectors)
                    {
                        for (crd::usize j = iloz; j <= ihiz; ++j)
                        {
                            const T sum = zd[j * zld + kc] + v2 * zd[j * zld + kc + 1] +
                                          v3 * zd[j * zld + kc + 2];
                            zd[j * zld + kc] -= sum * t1;
                            zd[j * zld + kc + 1] -= sum * t2;
                            zd[j * zld + kc + 2] -= sum * t3;
                        }
                    }
                }
                else if (nr == 2)
                {
                    for (crd::usize j = kc; j <= i2; ++j)
                    {
                        const T sum = h(kc, j) + v2 * h(kc + 1, j);
                        h(kc, j) -= sum * t1;
                        h(kc + 1, j) -= sum * t2;
                    }
                    for (crd::usize j = i1; j <= i; ++j)
                    {
                        const T sum = h(j, kc) + v2 * h(j, kc + 1);
                        h(j, kc) -= sum * t1;
                        h(j, kc + 1) -= sum * t2;
                    }
                    if (vectors)
                    {
                        for (crd::usize j = iloz; j <= ihiz; ++j)
                        {
                            const T sum = zd[j * zld + kc] + v2 * zd[j * zld + kc + 1];
                            zd[j * zld + kc] -= sum * t1;
                            zd[j * zld + kc + 1] -= sum * t2;
                        }
                    }
                }
            }
        }
        if (!split)
        {
            out.converged = false;
            return out;
        }
        if (l == i)  // 1×1: one real eigenvalue converged
        {
            out.wr[i] = h(i, i);
            out.wi[i] = T{0};
        }
        else  // l == i-1: standardize the 2×2 block
        {
            T aa = h(i - 1, i - 1);
            T bb = h(i - 1, i);
            T cc = h(i, i - 1);
            T dd = h(i, i);
            const auto o = dlanv2<T>(aa, bb, cc, dd);
            h(i - 1, i - 1) = aa;
            h(i - 1, i) = bb;
            h(i, i - 1) = cc;
            h(i, i) = dd;
            out.wr[i - 1] = o.rt1r;
            out.wi[i - 1] = o.rt1i;
            out.wr[i] = o.rt2r;
            out.wi[i] = o.rt2i;
            const T cs = o.cs;
            const T sn = o.sn;
            for (crd::usize j = i + 1; j <= i2; ++j)  // row rotation (cols i+1..i2)
            {
                const T tmp = cs * h(i - 1, j) + sn * h(i, j);
                h(i, j) = cs * h(i, j) - sn * h(i - 1, j);
                h(i - 1, j) = tmp;
            }
            for (crd::isize jj_s = static_cast<crd::isize>(i1);
                 jj_s + 2 <= static_cast<crd::isize>(i); ++jj_s)  // col rotation (rows i1..i-2)
            {
                const crd::usize jj = static_cast<crd::usize>(jj_s);
                const T tmp = cs * h(jj, i - 1) + sn * h(jj, i);
                h(jj, i) = cs * h(jj, i) - sn * h(jj, i - 1);
                h(jj, i - 1) = tmp;
            }
            if (vectors)
            {
                for (crd::usize jj = iloz; jj <= ihiz; ++jj)
                {
                    const T tmp = cs * zd[jj * zld + (i - 1)] + sn * zd[jj * zld + i];
                    zd[jj * zld + i] = cs * zd[jj * zld + i] - sn * zd[jj * zld + (i - 1)];
                    zd[jj * zld + (i - 1)] = tmp;
                }
            }
        }
        kdefl = 0;
        i_s = static_cast<crd::isize>(l) - 1;
    }
    out.converged = true;
    return out;
}

namespace
{
// Generate a Givens rotation [cs sn; -sn cs]·[f;g] = [r;0] (LAPACK dlartg, lite).
template <typename T>
void dlartg(T f, T g, T& cs, T& sn, T& r)
{
    if (g == T{0})
    {
        cs = T{1};
        sn = T{0};
        r = f;
    }
    else if (f == T{0})
    {
        cs = T{0};
        sn = T{1};
        r = g;
    }
    else
    {
        r = detail::hypot2(f, g);
        if ((std::abs(f) >= std::abs(g)) ? (f < T{0}) : (g < T{0}))
        {
            r = -r;
        }
        cs = f / r;
        sn = g / r;
    }
}

// dlarfg on a length-`len` vector with the "alpha" (and implicit 1) at index
// `ai`; the other elements are the tail to annihilate. Scales the tail in place,
// stores beta at u[ai] (the caller sets u[ai]=1 for application). Returns tau.
template <typename T>
T dlarfg_vec(T* u, crd::usize len, crd::usize ai)
{
    T xnsq = T{0};
    for (crd::usize i = 0; i < len; ++i)
    {
        if (i != ai)
        {
            xnsq += u[i] * u[i];
        }
    }
    if (xnsq == T{0})
    {
        return T{0};
    }
    const T alpha = u[ai];
    const T beta = -(alpha >= T{0} ? T{1} : T{-1}) * detail::hypot2(alpha, std::sqrt(xnsq));
    const T tau = (beta - alpha) / beta;
    const T inv = T{1} / (alpha - beta);
    for (crd::usize i = 0; i < len; ++i)
    {
        if (i != ai)
        {
            u[i] *= inv;
        }
    }
    u[ai] = beta;
    return tau;
}

// C[r0:r0+ku, c0:c0+ncol] := (I - tau·u·uᵀ)·C  (DLARFX 'L', u length ku).
template <typename T>
void apply_h_left(T* d, crd::usize ld, crd::usize r0, crd::usize c0, crd::usize ku, crd::usize ncol,
                  const T* u, T tau)
{
    if (tau == T{0})
    {
        return;
    }
    for (crd::usize j = 0; j < ncol; ++j)
    {
        T s = T{0};
        for (crd::usize i = 0; i < ku; ++i)
        {
            s += u[i] * d[(r0 + i) * ld + (c0 + j)];
        }
        s *= tau;
        for (crd::usize i = 0; i < ku; ++i)
        {
            d[(r0 + i) * ld + (c0 + j)] -= s * u[i];
        }
    }
}

// C[r0:r0+nrow, c0:c0+ku] := C·(I - tau·u·uᵀ)  (DLARFX 'R', u length ku).
template <typename T>
void apply_h_right(T* d, crd::usize ld, crd::usize r0, crd::usize c0, crd::usize nrow, crd::usize ku,
                   const T* u, T tau)
{
    if (tau == T{0})
    {
        return;
    }
    for (crd::usize i = 0; i < nrow; ++i)
    {
        T s = T{0};
        for (crd::usize j = 0; j < ku; ++j)
        {
            s += d[(r0 + i) * ld + (c0 + j)] * u[j];
        }
        s *= tau;
        for (crd::usize j = 0; j < ku; ++j)
        {
            d[(r0 + i) * ld + (c0 + j)] -= s * u[j];
        }
    }
}

// DROT on two rows a,b over columns [c0,c1): x=c·x+s·y, y=c·y−s·x.
template <typename T>
void drot_rows(T* d, crd::usize ld, crd::usize a, crd::usize b, crd::usize c0, crd::usize c1, T cs, T sn)
{
    for (crd::usize j = c0; j < c1; ++j)
    {
        const T x = d[a * ld + j];
        const T y = d[b * ld + j];
        d[a * ld + j] = cs * x + sn * y;
        d[b * ld + j] = cs * y - sn * x;
    }
}

// DROT on two columns a,b over rows [r0,r1).
template <typename T>
void drot_cols(T* d, crd::usize ld, crd::usize a, crd::usize b, crd::usize r0, crd::usize r1, T cs, T sn)
{
    for (crd::usize i = r0; i < r1; ++i)
    {
        const T x = d[i * ld + a];
        const T y = d[i * ld + b];
        d[i * ld + a] = cs * x + sn * y;
        d[i * ld + b] = cs * y - sn * x;
    }
}

// cdiv — Smith 1962 robust complex division (e+if) = (a+ib)/(c+id). Matches
// LAPACK dladiv's intent (scale by the larger denominator component → no
// spurious overflow). Used by dlaln2's complex branches.
template <typename T>
inline void cdiv(T a, T b, T c, T d, T& e, T& f) noexcept
{
    if (std::abs(d) < std::abs(c))
    {
        const T r = d / c;
        const T t = T{1} / (c + d * r);
        e = (a + b * r) * t;
        f = (b - a * r) * t;
    }
    else
    {
        const T r = c / d;
        const T t = T{1} / (c * r + d);
        e = (a * r + b) * t;
        f = (b * r - a) * t;
    }
}

// dlaln2: solve (ca·A − w·D)·X = scale·B (or transposed) for the 1×1 or 2×2 X,
// with overflow-safe scaling + a smin singular-value floor. A is na×na (na∈{1,2}),
// D = diag(d1,d2), w = wr+i·wi (real if nw=1, complex if nw=2; X/B then carry
// real part in col 0, imag in col 1). Faithful LAPACK port (Gaussian elimination
// with complete pivoting via the IPIVOT/RSWAP/ZSWAP tables on the column-major
// flattening crv/civ of the 2×2 coefficient). Returns {scale, xnorm, info, x}.
template <typename T>
struct Ln2
{
    T scale;
    T xnorm;
    int info;
    T x[2][2];  // x[row][col]; col 0 = real part, col 1 = imag part (nw=2)
};

template <typename T>
Ln2<T> dlaln2(bool ltrans, int na, int nw, T smin, T ca, const T a[2][2], T d1, T d2,
              const T b[2][2], T wr, T wi)
{
    // Column-major flattening order of a 2×2 M: v[0]=M(0,0), v[1]=M(1,0),
    // v[2]=M(0,1), v[3]=M(1,1) — matches the Fortran CRV/CIV EQUIVALENCE.
    static constexpr int kIpivot[4][4] = {
        {1, 2, 3, 4}, {2, 1, 4, 3}, {3, 4, 1, 2}, {4, 3, 2, 1}};
    static constexpr bool kRswap[4] = {false, true, false, true};
    static constexpr bool kZswap[4] = {false, false, true, true};

    Ln2<T> out{};
    out.scale = T{1};
    out.info = 0;
    out.x[0][0] = T{0};
    out.x[0][1] = T{0};
    out.x[1][0] = T{0};
    out.x[1][1] = T{0};

    const T smlnum = T{2} * std::numeric_limits<T>::min();
    const T bignum = T{1} / smlnum;
    const T smini = std::max(smin, smlnum);

    if (na == 1)
    {
        if (nw == 1)
        {
            T csr = ca * a[0][0] - wr * d1;
            T cnorm = std::abs(csr);
            if (cnorm < smini)
            {
                csr = smini;
                cnorm = smini;
                out.info = 1;
            }
            const T bnorm = std::abs(b[0][0]);
            if (cnorm < T{1} && bnorm > T{1} && bnorm > bignum * cnorm)
            {
                out.scale = T{1} / bnorm;
            }
            out.x[0][0] = (b[0][0] * out.scale) / csr;
            out.xnorm = std::abs(out.x[0][0]);
        }
        else
        {
            T csr = ca * a[0][0] - wr * d1;
            T csi = -wi * d1;
            T cnorm = std::abs(csr) + std::abs(csi);
            if (cnorm < smini)
            {
                csr = smini;
                csi = T{0};
                cnorm = smini;
                out.info = 1;
            }
            const T bnorm = std::abs(b[0][0]) + std::abs(b[0][1]);
            if (cnorm < T{1} && bnorm > T{1} && bnorm > bignum * cnorm)
            {
                out.scale = T{1} / bnorm;
            }
            cdiv<T>(out.scale * b[0][0], out.scale * b[0][1], csr, csi, out.x[0][0], out.x[0][1]);
            out.xnorm = std::abs(out.x[0][0]) + std::abs(out.x[0][1]);
        }
        return out;
    }

    // 2×2 system. Build the real part of C = ca·A − w·D, column-major flat.
    T crv[4];
    crv[0] = ca * a[0][0] - wr * d1;  // CR(1,1)
    crv[3] = ca * a[1][1] - wr * d2;  // CR(2,2)
    if (ltrans)
    {
        crv[2] = ca * a[1][0];  // CR(1,2) = ca·A(2,1)
        crv[1] = ca * a[0][1];  // CR(2,1) = ca·A(1,2)
    }
    else
    {
        crv[1] = ca * a[1][0];  // CR(2,1)
        crv[2] = ca * a[0][1];  // CR(1,2)
    }

    if (nw == 1)
    {
        T cmax = T{0};
        int icmax = 0;  // 1-based
        for (int j = 0; j < 4; ++j)
        {
            if (std::abs(crv[j]) > cmax)
            {
                cmax = std::abs(crv[j]);
                icmax = j + 1;
            }
        }
        if (cmax < smini)
        {
            const T bnorm = std::max(std::abs(b[0][0]), std::abs(b[1][0]));
            if (smini < T{1} && bnorm > T{1} && bnorm > bignum * smini)
            {
                out.scale = T{1} / bnorm;
            }
            const T temp = out.scale / smini;
            out.x[0][0] = temp * b[0][0];
            out.x[1][0] = temp * b[1][0];
            out.xnorm = temp * bnorm;
            out.info = 1;
            return out;
        }
        const int* piv = kIpivot[icmax - 1];
        const T ur11 = crv[icmax - 1];
        const T cr21 = crv[piv[1] - 1];
        const T ur12 = crv[piv[2] - 1];
        const T cr22 = crv[piv[3] - 1];
        const T ur11r = T{1} / ur11;
        const T lr21 = ur11r * cr21;
        T ur22 = cr22 - ur12 * lr21;
        if (std::abs(ur22) < smini)
        {
            ur22 = smini;
            out.info = 1;
        }
        T br1;
        T br2;
        if (kRswap[icmax - 1])
        {
            br1 = b[1][0];
            br2 = b[0][0];
        }
        else
        {
            br1 = b[0][0];
            br2 = b[1][0];
        }
        br2 = br2 - lr21 * br1;
        const T bbnd = std::max(std::abs(br1 * (ur22 * ur11r)), std::abs(br2));
        if (bbnd > T{1} && std::abs(ur22) < T{1} && bbnd >= bignum * std::abs(ur22))
        {
            out.scale = T{1} / bbnd;
        }
        const T xr2 = (br2 * out.scale) / ur22;
        const T xr1 = (out.scale * br1) * ur11r - xr2 * (ur11r * ur12);
        if (kZswap[icmax - 1])
        {
            out.x[0][0] = xr2;
            out.x[1][0] = xr1;
        }
        else
        {
            out.x[0][0] = xr1;
            out.x[1][0] = xr2;
        }
        out.xnorm = std::max(std::abs(xr1), std::abs(xr2));
        if (out.xnorm > T{1} && cmax > T{1} && out.xnorm > bignum / cmax)
        {
            const T temp = cmax / bignum;
            out.x[0][0] *= temp;
            out.x[1][0] *= temp;
            out.xnorm *= temp;
            out.scale *= temp;
        }
        return out;
    }

    // Complex 2×2 system.
    T civ[4];
    civ[0] = -wi * d1;  // CI(1,1)
    civ[1] = T{0};      // CI(2,1)
    civ[2] = T{0};      // CI(1,2)
    civ[3] = -wi * d2;  // CI(2,2)
    T cmax = T{0};
    int icmax = 0;
    for (int j = 0; j < 4; ++j)
    {
        if (std::abs(crv[j]) + std::abs(civ[j]) > cmax)
        {
            cmax = std::abs(crv[j]) + std::abs(civ[j]);
            icmax = j + 1;
        }
    }
    if (cmax < smini)
    {
        const T bnorm = std::max(std::abs(b[0][0]) + std::abs(b[0][1]),
                                 std::abs(b[1][0]) + std::abs(b[1][1]));
        if (smini < T{1} && bnorm > T{1} && bnorm > bignum * smini)
        {
            out.scale = T{1} / bnorm;
        }
        const T temp = out.scale / smini;
        out.x[0][0] = temp * b[0][0];
        out.x[1][0] = temp * b[1][0];
        out.x[0][1] = temp * b[0][1];
        out.x[1][1] = temp * b[1][1];
        out.xnorm = temp * bnorm;
        out.info = 1;
        return out;
    }
    const int* piv = kIpivot[icmax - 1];
    const T ur11 = crv[icmax - 1];
    const T ui11 = civ[icmax - 1];
    const T cr21 = crv[piv[1] - 1];
    const T ci21 = civ[piv[1] - 1];
    const T ur12 = crv[piv[2] - 1];
    const T ui12 = civ[piv[2] - 1];
    const T cr22 = crv[piv[3] - 1];
    const T ci22 = civ[piv[3] - 1];
    T ur11r;
    T ui11r;
    T lr21;
    T li21;
    T ur12s;
    T ui12s;
    T ur22;
    T ui22;
    if (icmax == 1 || icmax == 4)
    {
        // Off-diagonals of pivoted C are real.
        if (std::abs(ur11) > std::abs(ui11))
        {
            const T temp = ui11 / ur11;
            ur11r = T{1} / (ur11 * (T{1} + temp * temp));
            ui11r = -temp * ur11r;
        }
        else
        {
            const T temp = ur11 / ui11;
            ui11r = -T{1} / (ui11 * (T{1} + temp * temp));
            ur11r = -temp * ui11r;
        }
        lr21 = cr21 * ur11r;
        li21 = cr21 * ui11r;
        ur12s = ur12 * ur11r;
        ui12s = ur12 * ui11r;
        ur22 = cr22 - ur12 * lr21;
        ui22 = ci22 - ur12 * li21;
    }
    else
    {
        // Diagonals of pivoted C are real.
        ur11r = T{1} / ur11;
        ui11r = T{0};
        lr21 = cr21 * ur11r;
        li21 = ci21 * ur11r;
        ur12s = ur12 * ur11r;
        ui12s = ui12 * ur11r;
        ur22 = cr22 - ur12 * lr21 + ui12 * li21;
        ui22 = -ur12 * li21 - ui12 * lr21;
    }
    T u22abs = std::abs(ur22) + std::abs(ui22);
    if (u22abs < smini)
    {
        ur22 = smini;
        ui22 = T{0};
        out.info = 1;
        u22abs = smini;
    }
    T br1;
    T br2;
    T bi1;
    T bi2;
    if (kRswap[icmax - 1])
    {
        br2 = b[0][0];
        br1 = b[1][0];
        bi2 = b[0][1];
        bi1 = b[1][1];
    }
    else
    {
        br1 = b[0][0];
        br2 = b[1][0];
        bi1 = b[0][1];
        bi2 = b[1][1];
    }
    br2 = br2 - lr21 * br1 + li21 * bi1;
    bi2 = bi2 - li21 * br1 - lr21 * bi1;
    const T bbnd = std::max((std::abs(br1) + std::abs(bi1)) *
                                (u22abs * (std::abs(ur11r) + std::abs(ui11r))),
                            std::abs(br2) + std::abs(bi2));
    if (bbnd > T{1} && u22abs < T{1} && bbnd >= bignum * u22abs)
    {
        out.scale = T{1} / bbnd;
        br1 *= out.scale;
        bi1 *= out.scale;
        br2 *= out.scale;
        bi2 *= out.scale;
    }
    T xr2;
    T xi2;
    cdiv<T>(br2, bi2, ur22, ui22, xr2, xi2);
    const T xr1 = ur11r * br1 - ui11r * bi1 - ur12s * xr2 + ui12s * xi2;
    const T xi1 = ui11r * br1 + ur11r * bi1 - ui12s * xr2 - ur12s * xi2;
    if (kZswap[icmax - 1])
    {
        out.x[0][0] = xr2;
        out.x[1][0] = xr1;
        out.x[0][1] = xi2;
        out.x[1][1] = xi1;
    }
    else
    {
        out.x[0][0] = xr1;
        out.x[1][0] = xr2;
        out.x[0][1] = xi1;
        out.x[1][1] = xi2;
    }
    out.xnorm = std::max(std::abs(xr1) + std::abs(xi1), std::abs(xr2) + std::abs(xi2));
    if (out.xnorm > T{1} && cmax > T{1} && out.xnorm > bignum / cmax)
    {
        const T temp = cmax / bignum;
        out.x[0][0] *= temp;
        out.x[1][0] *= temp;
        out.x[0][1] *= temp;
        out.x[1][1] *= temp;
        out.xnorm *= temp;
        out.scale *= temp;
    }
    return out;
}

// dlasy2: solve op(TL)·X + isgn·X·op(TR) = scale·B for the (≤2)×(≤2) X. TL/TR/B
// are 2×2 (only the leading n1×n1 / n2×n2 / n1×n2 used). Faithful LAPACK port.
template <typename T>
struct Sy2
{
    T scale;
    T x[2][2];
};

template <typename T>
Sy2<T> dlasy2(bool ltranl, bool ltranr, int isgn, int n1, int n2, const T tl[2][2], const T tr[2][2],
              const T b[2][2])
{
    Sy2<T> out{};
    out.scale = T{1};
    const T eps = std::numeric_limits<T>::epsilon();
    const T smlnum = std::numeric_limits<T>::min() / eps;
    const T sgn = static_cast<T>(isgn);

    if (n1 == 1 && n2 == 1)
    {
        T tau1 = tl[0][0] + sgn * tr[0][0];
        T bet = std::abs(tau1);
        if (bet <= smlnum)
        {
            tau1 = smlnum;
            bet = smlnum;
        }
        T scale = T{1};
        const T gam = std::abs(b[0][0]);
        if (smlnum * gam > bet)
        {
            scale = T{1} / gam;
        }
        out.x[0][0] = (b[0][0] * scale) / tau1;
        out.scale = scale;
        return out;
    }

    // 2×2 system (n1=1,n2=2 or n1=2,n2=1).
    if ((n1 == 1 && n2 == 2) || (n1 == 2 && n2 == 1))
    {
        const crd::usize locu12[4] = {2, 3, 0, 1};
        const crd::usize locl21[4] = {1, 0, 3, 2};
        const crd::usize locu22[4] = {3, 2, 1, 0};
        const bool xswpiv[4] = {false, false, true, true};
        const bool bswpiv[4] = {false, true, false, true};
        T tmp[4];
        T btmp[2];
        T smin;
        if (n1 == 1)
        {
            smin = std::max(std::abs(tl[0][0]), std::max(std::abs(tr[0][0]), std::abs(tr[0][1])));
            smin = std::max(smin, std::max(std::abs(tr[1][0]), std::abs(tr[1][1])));
            smin = std::max(eps * smin, smlnum);
            tmp[0] = tl[0][0] + sgn * tr[0][0];
            tmp[3] = tl[0][0] + sgn * tr[1][1];
            if (ltranr)
            {
                tmp[1] = sgn * tr[1][0];
                tmp[2] = sgn * tr[0][1];
            }
            else
            {
                tmp[1] = sgn * tr[0][1];
                tmp[2] = sgn * tr[1][0];
            }
            btmp[0] = b[0][0];
            btmp[1] = b[0][1];
        }
        else
        {
            smin = std::max(std::abs(tr[0][0]), std::max(std::abs(tl[0][0]), std::abs(tl[0][1])));
            smin = std::max(smin, std::max(std::abs(tl[1][0]), std::abs(tl[1][1])));
            smin = std::max(eps * smin, smlnum);
            tmp[0] = tl[0][0] + sgn * tr[0][0];
            tmp[3] = tl[1][1] + sgn * tr[0][0];
            if (ltranl)
            {
                tmp[1] = tl[0][1];
                tmp[2] = tl[1][0];
            }
            else
            {
                tmp[1] = tl[1][0];
                tmp[2] = tl[0][1];
            }
            btmp[0] = b[0][0];
            btmp[1] = b[1][0];
        }
        // Complete-pivoted 2×2 solve.
        crd::usize ipiv = 0;
        T amax = std::abs(tmp[0]);
        for (crd::usize q = 1; q < 4; ++q)
        {
            if (std::abs(tmp[q]) > amax)
            {
                amax = std::abs(tmp[q]);
                ipiv = q;
            }
        }
        T u11 = tmp[ipiv];
        if (std::abs(u11) <= smin)
        {
            u11 = smin;
        }
        const T u12 = tmp[locu12[ipiv]];
        const T l21 = tmp[locl21[ipiv]] / u11;
        T u22 = tmp[locu22[ipiv]] - u12 * l21;
        if (std::abs(u22) <= smin)
        {
            u22 = smin;
        }
        if (bswpiv[ipiv])
        {
            const T t = btmp[1];
            btmp[1] = btmp[0] - l21 * t;
            btmp[0] = t;
        }
        else
        {
            btmp[1] = btmp[1] - l21 * btmp[0];
        }
        T scale = T{1};
        if ((T{2} * smlnum) * std::abs(btmp[1]) > std::abs(u22) ||
            (T{2} * smlnum) * std::abs(btmp[0]) > std::abs(u11))
        {
            scale = (T{1} / T{2}) / std::max(std::abs(btmp[0]), std::abs(btmp[1]));
            btmp[0] *= scale;
            btmp[1] *= scale;
        }
        T x2[2];
        x2[1] = btmp[1] / u22;
        x2[0] = btmp[0] / u11 - (u12 / u11) * x2[1];
        if (xswpiv[ipiv])
        {
            const T t = x2[1];
            x2[1] = x2[0];
            x2[0] = t;
        }
        out.scale = scale;
        out.x[0][0] = x2[0];
        if (n1 == 1)
        {
            out.x[0][1] = x2[1];
        }
        else
        {
            out.x[1][0] = x2[1];
        }
        return out;
    }

    // 2×2 block: equivalent 4×4 system with complete pivoting.
    T smin = std::max(std::abs(tr[0][0]), std::max(std::abs(tr[0][1]), std::max(std::abs(tr[1][0]),
                                                                               std::abs(tr[1][1]))));
    smin = std::max(smin, std::max(std::abs(tl[0][0]), std::max(std::abs(tl[0][1]),
                                                                std::max(std::abs(tl[1][0]),
                                                                         std::abs(tl[1][1])))));
    smin = std::max(eps * smin, smlnum);
    T t16[4][4];
    for (int p = 0; p < 4; ++p)
    {
        for (int q = 0; q < 4; ++q)
        {
            t16[p][q] = T{0};
        }
    }
    t16[0][0] = tl[0][0] + sgn * tr[0][0];
    t16[1][1] = tl[1][1] + sgn * tr[0][0];
    t16[2][2] = tl[0][0] + sgn * tr[1][1];
    t16[3][3] = tl[1][1] + sgn * tr[1][1];
    if (ltranl)
    {
        t16[0][1] = tl[1][0];
        t16[1][0] = tl[0][1];
        t16[2][3] = tl[1][0];
        t16[3][2] = tl[0][1];
    }
    else
    {
        t16[0][1] = tl[0][1];
        t16[1][0] = tl[1][0];
        t16[2][3] = tl[0][1];
        t16[3][2] = tl[1][0];
    }
    if (ltranr)
    {
        t16[0][2] = sgn * tr[0][1];
        t16[1][3] = sgn * tr[0][1];
        t16[2][0] = sgn * tr[1][0];
        t16[3][1] = sgn * tr[1][0];
    }
    else
    {
        t16[0][2] = sgn * tr[1][0];
        t16[1][3] = sgn * tr[1][0];
        t16[2][0] = sgn * tr[0][1];
        t16[3][1] = sgn * tr[0][1];
    }
    T btmp[4] = {b[0][0], b[1][0], b[0][1], b[1][1]};
    int jpiv[4] = {0, 1, 2, 3};
    for (int ii = 0; ii < 3; ++ii)
    {
        T xmax = T{0};
        int ipsv = ii;
        int jpsv = ii;
        for (int ip = ii; ip < 4; ++ip)
        {
            for (int jp = ii; jp < 4; ++jp)
            {
                if (std::abs(t16[ip][jp]) >= xmax)
                {
                    xmax = std::abs(t16[ip][jp]);
                    ipsv = ip;
                    jpsv = jp;
                }
            }
        }
        if (ipsv != ii)
        {
            for (int q = 0; q < 4; ++q)
            {
                std::swap(t16[ipsv][q], t16[ii][q]);
            }
            std::swap(btmp[ipsv], btmp[ii]);
        }
        if (jpsv != ii)
        {
            for (int q = 0; q < 4; ++q)
            {
                std::swap(t16[q][jpsv], t16[q][ii]);
            }
        }
        jpiv[ii] = jpsv;
        if (std::abs(t16[ii][ii]) < smin)
        {
            t16[ii][ii] = smin;
        }
        for (int jr = ii + 1; jr < 4; ++jr)
        {
            t16[jr][ii] /= t16[ii][ii];
            btmp[jr] -= t16[jr][ii] * btmp[ii];
            for (int kc = ii + 1; kc < 4; ++kc)
            {
                t16[jr][kc] -= t16[jr][ii] * t16[ii][kc];
            }
        }
    }
    if (std::abs(t16[3][3]) < smin)
    {
        t16[3][3] = smin;
    }
    T scale = T{1};
    const T eight = static_cast<T>(8);
    if ((eight * smlnum) * std::abs(btmp[0]) > std::abs(t16[0][0]) ||
        (eight * smlnum) * std::abs(btmp[1]) > std::abs(t16[1][1]) ||
        (eight * smlnum) * std::abs(btmp[2]) > std::abs(t16[2][2]) ||
        (eight * smlnum) * std::abs(btmp[3]) > std::abs(t16[3][3]))
    {
        scale = (T{1} / eight) /
                std::max(std::abs(btmp[0]),
                         std::max(std::abs(btmp[1]), std::max(std::abs(btmp[2]), std::abs(btmp[3]))));
        for (int q = 0; q < 4; ++q)
        {
            btmp[q] *= scale;
        }
    }
    T tmp[4];
    for (int ii = 0; ii < 4; ++ii)
    {
        const int kk = 3 - ii;
        const T temp = T{1} / t16[kk][kk];
        tmp[kk] = btmp[kk] * temp;
        for (int j = kk + 1; j < 4; ++j)
        {
            tmp[kk] -= (temp * t16[kk][j]) * tmp[j];
        }
    }
    for (int ii = 0; ii < 3; ++ii)
    {
        if (jpiv[2 - ii] != 2 - ii)
        {
            std::swap(tmp[2 - ii], tmp[jpiv[2 - ii]]);
        }
    }
    out.scale = scale;
    out.x[0][0] = tmp[0];
    out.x[1][0] = tmp[1];
    out.x[0][1] = tmp[2];
    out.x[1][1] = tmp[3];
    return out;
}

// dlaexc (LAPACK): swap the adjacent diagonal blocks of order n1,n2 ∈ {1,2}
// starting at 1-based position j1 of the quasi-triangular t (n×n); update q if
// wantq. Returns 1 if the swap was rejected (ill-conditioned), else 0.
template <typename T>
int dlaexc(T* td, crd::usize ld, T* qd, crd::usize qld, crd::usize n, crd::usize j1, int n1, int n2,
           bool wantq)
{
    auto t1 = [&](crd::usize i, crd::usize j) -> T& { return td[(i - 1) * ld + (j - 1)]; };
    const crd::usize j2 = j1 + 1;
    const crd::usize j3 = j1 + 2;
    const crd::usize j4 = j1 + 3;

    if (n1 == 1 && n2 == 1)
    {
        const T t11 = t1(j1, j1);
        const T t22 = t1(j2, j2);
        T cs;
        T sn;
        T temp;
        dlartg<T>(t1(j1, j2), t22 - t11, cs, sn, temp);
        if (j3 <= n)
        {
            drot_rows<T>(td, ld, j1 - 1, j2 - 1, j3 - 1, n, cs, sn);
        }
        drot_cols<T>(td, ld, j1 - 1, j2 - 1, 0, j1 - 1, cs, sn);
        t1(j1, j1) = t22;
        t1(j2, j2) = t11;
        if (wantq)
        {
            drot_cols<T>(qd, qld, j1 - 1, j2 - 1, 0, n, cs, sn);
        }
        return 0;
    }

    const int nd = n1 + n2;
    const crd::usize ldd = static_cast<crd::usize>(nd);
    T dloc[16];
    for (int i = 0; i < nd; ++i)
    {
        for (int j = 0; j < nd; ++j)
        {
            dloc[i * ldd + j] = t1(j1 + static_cast<crd::usize>(i), j1 + static_cast<crd::usize>(j));
        }
    }
    T dnorm = T{0};
    for (int q = 0; q < nd * nd; ++q)
    {
        dnorm = std::max(dnorm, std::abs(dloc[q]));
    }
    const T eps = std::numeric_limits<T>::epsilon();
    const T smlnum = std::numeric_limits<T>::min() / eps;
    const T thresh = std::max(static_cast<T>(10) * eps * dnorm, smlnum);

    // Solve T11·X − X·T22 = scale·T12.
    T sytl[2][2];
    T sytr[2][2];
    T syb[2][2];
    for (int i = 0; i < n1; ++i)
    {
        for (int j = 0; j < n1; ++j)
        {
            sytl[i][j] = dloc[i * ldd + j];
        }
    }
    for (int i = 0; i < n2; ++i)
    {
        for (int j = 0; j < n2; ++j)
        {
            sytr[i][j] = dloc[(n1 + i) * ldd + (n1 + j)];
        }
    }
    for (int i = 0; i < n1; ++i)
    {
        for (int j = 0; j < n2; ++j)
        {
            syb[i][j] = dloc[i * ldd + (n1 + j)];
        }
    }
    const Sy2<T> sy = dlasy2<T>(false, false, -1, n1, n2, sytl, sytr, syb);
    const T scale = sy.scale;

    if (n1 == 1 && n2 == 2)
    {
        T u[3] = {scale, sy.x[0][0], sy.x[0][1]};
        const T tau = dlarfg_vec<T>(u, 3, 2);
        u[2] = T{1};
        const T t11 = t1(j1, j1);
        apply_h_left<T>(dloc, ldd, 0, 0, 3, 3, u, tau);
        apply_h_right<T>(dloc, ldd, 0, 0, 3, 3, u, tau);
        if (std::max(std::abs(dloc[2 * ldd + 0]),
                     std::max(std::abs(dloc[2 * ldd + 1]), std::abs(dloc[2 * ldd + 2] - t11))) > thresh)
        {
            return 1;
        }
        apply_h_left<T>(td, ld, j1 - 1, j1 - 1, 3, n - j1 + 1, u, tau);
        apply_h_right<T>(td, ld, 0, j1 - 1, j2, 3, u, tau);
        t1(j3, j1) = T{0};
        t1(j3, j2) = T{0};
        t1(j3, j3) = t11;
        if (wantq)
        {
            apply_h_right<T>(qd, qld, 0, j1 - 1, n, 3, u, tau);
        }
    }
    else if (n1 == 2 && n2 == 1)
    {
        T u[3] = {-sy.x[0][0], -sy.x[1][0], scale};
        const T tau = dlarfg_vec<T>(u, 3, 0);
        u[0] = T{1};
        const T t33 = t1(j3, j3);
        apply_h_left<T>(dloc, ldd, 0, 0, 3, 3, u, tau);
        apply_h_right<T>(dloc, ldd, 0, 0, 3, 3, u, tau);
        if (std::max(std::abs(dloc[1 * ldd + 0]),
                     std::max(std::abs(dloc[2 * ldd + 0]), std::abs(dloc[0 * ldd + 0] - t33))) > thresh)
        {
            return 1;
        }
        apply_h_right<T>(td, ld, 0, j1 - 1, j3, 3, u, tau);
        apply_h_left<T>(td, ld, j1 - 1, j2 - 1, 3, n - j1, u, tau);
        t1(j1, j1) = t33;
        t1(j2, j1) = T{0};
        t1(j3, j1) = T{0};
        if (wantq)
        {
            apply_h_right<T>(qd, qld, 0, j1 - 1, n, 3, u, tau);
        }
    }
    else  // n1 == 2 && n2 == 2
    {
        T u1[3] = {-sy.x[0][0], -sy.x[1][0], scale};
        const T tau1 = dlarfg_vec<T>(u1, 3, 0);
        u1[0] = T{1};
        const T temp = -tau1 * (sy.x[0][1] + u1[1] * sy.x[1][1]);
        T u2[3] = {-temp * u1[1] - sy.x[1][1], -temp * u1[2], scale};
        const T tau2 = dlarfg_vec<T>(u2, 3, 0);
        u2[0] = T{1};
        apply_h_left<T>(dloc, ldd, 0, 0, 3, 4, u1, tau1);
        apply_h_right<T>(dloc, ldd, 0, 0, 4, 3, u1, tau1);
        apply_h_left<T>(dloc, ldd, 1, 0, 3, 4, u2, tau2);
        apply_h_right<T>(dloc, ldd, 0, 1, 4, 3, u2, tau2);
        if (std::max(std::max(std::abs(dloc[2 * ldd + 0]), std::abs(dloc[2 * ldd + 1])),
                     std::max(std::abs(dloc[3 * ldd + 0]), std::abs(dloc[3 * ldd + 1]))) > thresh)
        {
            return 1;
        }
        apply_h_left<T>(td, ld, j1 - 1, j1 - 1, 3, n - j1 + 1, u1, tau1);
        apply_h_right<T>(td, ld, 0, j1 - 1, j4, 3, u1, tau1);
        apply_h_left<T>(td, ld, j2 - 1, j1 - 1, 3, n - j1 + 1, u2, tau2);
        apply_h_right<T>(td, ld, 0, j2 - 1, j4, 3, u2, tau2);
        t1(j3, j1) = T{0};
        t1(j3, j2) = T{0};
        t1(j4, j1) = T{0};
        t1(j4, j2) = T{0};
        if (wantq)
        {
            apply_h_right<T>(qd, qld, 0, j1 - 1, n, 3, u1, tau1);
            apply_h_right<T>(qd, qld, 0, j2 - 1, n, 3, u2, tau2);
        }
    }

    // Standardize the new 2×2 block(s).
    if (n2 == 2)
    {
        T aa = t1(j1, j1);
        T bbk = t1(j1, j2);
        T cc = t1(j2, j1);
        T dd = t1(j2, j2);
        const auto o = dlanv2<T>(aa, bbk, cc, dd);
        t1(j1, j1) = aa;
        t1(j1, j2) = bbk;
        t1(j2, j1) = cc;
        t1(j2, j2) = dd;
        if (j1 + 1 < n)
        {
            drot_rows<T>(td, ld, j1 - 1, j2 - 1, j1 + 1, n, o.cs, o.sn);
        }
        drot_cols<T>(td, ld, j1 - 1, j2 - 1, 0, j1 - 1, o.cs, o.sn);
        if (wantq)
        {
            drot_cols<T>(qd, qld, j1 - 1, j2 - 1, 0, n, o.cs, o.sn);
        }
    }
    if (n1 == 2)
    {
        const crd::usize k3 = j1 + static_cast<crd::usize>(n2);
        const crd::usize k4 = k3 + 1;
        T aa = t1(k3, k3);
        T bbk = t1(k3, k4);
        T cc = t1(k4, k3);
        T dd = t1(k4, k4);
        const auto o = dlanv2<T>(aa, bbk, cc, dd);
        t1(k3, k3) = aa;
        t1(k3, k4) = bbk;
        t1(k4, k3) = cc;
        t1(k4, k4) = dd;
        if (k3 + 2 <= n)
        {
            drot_rows<T>(td, ld, k3 - 1, k4 - 1, k3 + 1, n, o.cs, o.sn);
        }
        drot_cols<T>(td, ld, k3 - 1, k4 - 1, 0, k3 - 1, o.cs, o.sn);
        if (wantq)
        {
            drot_cols<T>(qd, qld, k3 - 1, k4 - 1, 0, n, o.cs, o.sn);
        }
    }
    return 0;
}

// dtrexc (LAPACK): move the block at 1-based `ifst` to `ilst` by adjacent
// dlaexc swaps. ifst/ilst are 1-based and snapped to block tops. Returns false
// on a rejected swap. Updates t (n×n) + q (if wantq).
template <typename T>
bool dtrexc(T* td, crd::usize ld, T* qd, crd::usize qld, crd::usize n, crd::usize ifst,
            crd::usize ilst, bool wantq)
{
    auto t1 = [&](crd::usize i, crd::usize j) -> T& { return td[(i - 1) * ld + (j - 1)]; };
    if (n <= 1)
    {
        return true;
    }
    if (ifst > 1 && t1(ifst, ifst - 1) != T{0})
    {
        --ifst;
    }
    int nbf = 1;
    if (ifst < n && t1(ifst + 1, ifst) != T{0})
    {
        nbf = 2;
    }
    if (ilst > 1 && t1(ilst, ilst - 1) != T{0})
    {
        --ilst;
    }
    int nbl = 1;
    if (ilst < n && t1(ilst + 1, ilst) != T{0})
    {
        nbl = 2;
    }
    if (ifst == ilst)
    {
        return true;
    }

    if (ifst < ilst)
    {
        if (nbf == 2 && nbl == 1)
        {
            --ilst;
        }
        if (nbf == 1 && nbl == 2)
        {
            ++ilst;
        }
        crd::usize here = ifst;
        while (true)
        {
            if (nbf == 1 || nbf == 2)
            {
                int nbnext = 1;
                if (here + static_cast<crd::usize>(nbf) + 1 <= n &&
                    t1(here + static_cast<crd::usize>(nbf) + 1, here + static_cast<crd::usize>(nbf)) !=
                        T{0})
                {
                    nbnext = 2;
                }
                if (dlaexc<T>(td, ld, qd, qld, n, here, nbf, nbnext, wantq) != 0)
                {
                    return false;
                }
                here += static_cast<crd::usize>(nbnext);
                if (nbf == 2 && t1(here + 1, here) == T{0})
                {
                    nbf = 3;
                }
            }
            else  // nbf == 3: two 1×1 blocks
            {
                int nbnext = 1;
                if (here + 3 <= n && t1(here + 3, here + 2) != T{0})
                {
                    nbnext = 2;
                }
                if (dlaexc<T>(td, ld, qd, qld, n, here + 1, 1, nbnext, wantq) != 0)
                {
                    return false;
                }
                if (nbnext == 1)
                {
                    dlaexc<T>(td, ld, qd, qld, n, here, 1, nbnext, wantq);
                    ++here;
                }
                else
                {
                    if (t1(here + 2, here + 1) == T{0})
                    {
                        nbnext = 1;
                    }
                    if (nbnext == 2)
                    {
                        if (dlaexc<T>(td, ld, qd, qld, n, here, 1, nbnext, wantq) != 0)
                        {
                            return false;
                        }
                        here += 2;
                    }
                    else
                    {
                        dlaexc<T>(td, ld, qd, qld, n, here, 1, 1, wantq);
                        dlaexc<T>(td, ld, qd, qld, n, here + 1, 1, 1, wantq);
                        here += 2;
                    }
                }
            }
            if (here >= ilst)
            {
                break;
            }
        }
    }
    else  // ifst > ilst: move up
    {
        crd::usize here = ifst;
        while (true)
        {
            if (nbf == 1 || nbf == 2)
            {
                int nbnext = 1;
                if (here >= 3 && t1(here - 1, here - 2) != T{0})
                {
                    nbnext = 2;
                }
                if (dlaexc<T>(td, ld, qd, qld, n, here - static_cast<crd::usize>(nbnext), nbnext, nbf,
                              wantq) != 0)
                {
                    return false;
                }
                here -= static_cast<crd::usize>(nbnext);
                if (nbf == 2 && t1(here + 1, here) == T{0})
                {
                    nbf = 3;
                }
            }
            else  // nbf == 3
            {
                int nbnext = 1;
                if (here >= 3 && t1(here - 1, here - 2) != T{0})
                {
                    nbnext = 2;
                }
                if (dlaexc<T>(td, ld, qd, qld, n, here - static_cast<crd::usize>(nbnext), nbnext, 1,
                              wantq) != 0)
                {
                    return false;
                }
                if (nbnext == 1)
                {
                    dlaexc<T>(td, ld, qd, qld, n, here, nbnext, 1, wantq);
                    --here;
                }
                else
                {
                    if (t1(here, here - 1) == T{0})
                    {
                        nbnext = 1;
                    }
                    if (nbnext == 2)
                    {
                        if (dlaexc<T>(td, ld, qd, qld, n, here - 1, 2, 1, wantq) != 0)
                        {
                            return false;
                        }
                        here -= 2;
                    }
                    else
                    {
                        dlaexc<T>(td, ld, qd, qld, n, here, 1, 1, wantq);
                        dlaexc<T>(td, ld, qd, qld, n, here - 1, 1, 1, wantq);
                        here -= 2;
                    }
                }
            }
            if (here <= ilst)
            {
                break;
            }
        }
    }
    return true;
}
} // namespace

template <typename T>
bool reorder_schur(Matrix<T>& t, Matrix<T>& z, crd::usize ifst, crd::usize ilst)
{
    static_assert(!is_complex_v<T>, "reorder_schur is real-only");
    const crd::usize n = t.rows();
    CRD_ASSERT_MSG(z.rows() == n && z.cols() == n, "reorder_schur: Z must be n×n");
    // 1-based ifst/ilst for the faithful dtrexc port.
    return dtrexc<T>(t.data(), t.ld(), z.data(), z.ld(), n, ifst + 1, ilst + 1, true);
}

namespace
{
// C(rows×jw) := C·V  (V jw×jw); gemm into scratch then copy back. r0/c0 = top-left
// of the C sub-block in `dat` (ld); v is a contiguous jw×jw matrix (ldv).
template <typename T>
void slab_right(T* dat, crd::usize ld, crd::usize r0, crd::usize c0, crd::usize rows, crd::usize jw,
                const T* v, crd::usize ldv, Matrix<T>& scratch, crd::memory::IAllocator* alloc)
{
    if (rows == 0)
    {
        return;
    }
    constexpr Layout k_l = Layout::RowMajor;
    MatrixView<const T, k_l> cv{dat + r0 * ld + c0, rows, jw, ld};
    MatrixView<const T, k_l> vv{v, jw, jw, ldv};
    MatrixView<T, k_l> ov{scratch.data(), rows, jw, scratch.ld()};
    gemm<T, k_l>(T{1}, cv, vv, T{0}, ov, Trans::None, Trans::None, alloc);
    for (crd::usize i = 0; i < rows; ++i)
    {
        for (crd::usize j = 0; j < jw; ++j)
        {
            dat[(r0 + i) * ld + (c0 + j)] = scratch.at(i, j);
        }
    }
}

// C(jw×cols) := Vᵀ·C  (V jw×jw); gemm into scratch then copy back.
template <typename T>
void slab_left_t(T* dat, crd::usize ld, crd::usize r0, crd::usize c0, crd::usize jw, crd::usize cols,
                 const T* v, crd::usize ldv, Matrix<T>& scratch, crd::memory::IAllocator* alloc)
{
    if (cols == 0)
    {
        return;
    }
    constexpr Layout k_l = Layout::RowMajor;
    MatrixView<const T, k_l> vv{v, jw, jw, ldv};
    MatrixView<const T, k_l> cv{dat + r0 * ld + c0, jw, cols, ld};
    MatrixView<T, k_l> ov{scratch.data(), jw, cols, scratch.ld()};
    gemm<T, k_l>(T{1}, vv, cv, T{0}, ov, Trans::Transpose, Trans::None, alloc);
    for (crd::usize i = 0; i < jw; ++i)
    {
        for (crd::usize j = 0; j < cols; ++j)
        {
            dat[(r0 + i) * ld + (c0 + j)] = scratch.at(i, j);
        }
    }
}
} // namespace

template <typename T>
AedResult<T> aed_deflate(crd::memory::IAllocator* alloc, Matrix<T>& h, crd::usize ktop, crd::usize kbot,
                         crd::usize nw, Matrix<T>& z, bool wantz, crd::usize iloz, crd::usize ihiz,
                         bool wantt, crd::containers::Array<T>& wr, crd::containers::Array<T>& wi)
{
    static_assert(!is_complex_v<T>, "aed_deflate is real-only");
    const crd::usize n = h.rows();
    AedResult<T> res{};
    if (wr.size() != n)
    {
        wr.resize(n);
    }
    if (wi.size() != n)
    {
        wi.resize(n);
    }
    if (ktop > kbot)
    {
        return res;
    }

    T* hd = h.data();
    const crd::usize ld = h.ld();
    auto hh = [&](crd::usize i, crd::usize j) -> T& { return hd[i * ld + j]; };

    const T eps = std::numeric_limits<T>::epsilon();
    const T safmin = std::numeric_limits<T>::min();
    const T ulp = eps;
    const T smlnum = safmin * (static_cast<T>(n) / ulp);

    const crd::usize jw = std::min(nw, kbot - ktop + 1);
    const crd::usize kwtop = kbot - jw + 1;
    T s = (kwtop == ktop) ? T{0} : hh(kwtop, kwtop - 1);

    if (kbot == kwtop)  // 1×1 window
    {
        wr[kwtop] = hh(kwtop, kwtop);
        wi[kwtop] = T{0};
        res.ns = 1;
        res.nd = 0;
        if (std::abs(s) <= std::max(smlnum, ulp * std::abs(hh(kwtop, kwtop))))
        {
            res.ns = 0;
            res.nd = 1;
            if (kwtop > ktop)
            {
                hh(kwtop, kwtop - 1) = T{0};
            }
        }
        return res;
    }

    // Window Hessenberg → real Schur (T = twin, V = v).
    Matrix<T> win(alloc, jw, jw);
    for (crd::usize i = 0; i < jw; ++i)
    {
        for (crd::usize j = 0; j < jw; ++j)
        {
            win.at(i, j) = (j + 1 >= i) ? hh(kwtop + i, kwtop + j) : T{0};
        }
    }
    RealSchur<T> sch = real_schur<T>(alloc, win, 0, jw - 1, true);
    Matrix<T>& twin = sch.t;
    Matrix<T>& v = sch.z;
    const crd::usize infqr = 0;  // window is small; real_schur converges
    auto t1 = [&](crd::usize i, crd::usize j) -> T& { return twin.at(i - 1, j - 1); };
    auto v1 = [&](crd::usize i, crd::usize j) -> T& { return v.at(i - 1, j - 1); };

    // Clean margin near the diagonal for the reorder.
    for (crd::usize j = 1; j + 3 <= jw; ++j)
    {
        t1(j + 2, j) = T{0};
        t1(j + 3, j) = T{0};
    }
    if (jw > 2)
    {
        t1(jw, jw - 2) = T{0};
    }

    // Deflation detection loop (1-based ns/ilst).
    crd::usize ns = jw;
    crd::usize ilst = infqr + 1;
    while (ilst <= ns)
    {
        const bool bulge = (ns != 1) && (t1(ns, ns - 1) != T{0});
        if (!bulge)  // real eigenvalue
        {
            T foo = std::abs(t1(ns, ns));
            if (foo == T{0})
            {
                foo = std::abs(s);
            }
            if (std::abs(s * v1(1, ns)) <= std::max(smlnum, ulp * foo))
            {
                --ns;  // deflatable
            }
            else
            {
                reorder_schur<T>(twin, v, ns - 1, ilst - 1);  // move up out of the way
                ++ilst;
            }
        }
        else  // complex-conjugate pair
        {
            T foo = std::abs(t1(ns, ns)) +
                    std::sqrt(std::abs(t1(ns, ns - 1))) * std::sqrt(std::abs(t1(ns - 1, ns)));
            if (foo == T{0})
            {
                foo = std::abs(s);
            }
            if (std::max(std::abs(s * v1(1, ns)), std::abs(s * v1(1, ns - 1))) <=
                std::max(smlnum, ulp * foo))
            {
                ns -= 2;
            }
            else
            {
                reorder_schur<T>(twin, v, ns - 1, ilst - 1);
                ilst += 2;
            }
        }
    }
    if (ns == 0)
    {
        s = T{0};
    }

    // Restore eigenvalues from T (window-local), into wr/wi at [kwtop..kbot].
    crd::usize ii = jw;
    while (ii >= infqr + 1)
    {
        if (ii == infqr + 1 || t1(ii, ii - 1) == T{0})
        {
            wr[kwtop + ii - 1] = t1(ii, ii);
            wi[kwtop + ii - 1] = T{0};
            --ii;
        }
        else
        {
            T aa = t1(ii - 1, ii - 1);
            T cc = t1(ii, ii - 1);
            T bb = t1(ii - 1, ii);
            T dd = t1(ii, ii);
            const auto o = dlanv2<T>(aa, bb, cc, dd);
            wr[kwtop + ii - 2] = o.rt1r;
            wi[kwtop + ii - 2] = o.rt1i;
            wr[kwtop + ii - 1] = o.rt2r;
            wi[kwtop + ii - 1] = o.rt2i;
            ii -= 2;
        }
        if (ii == 0)
        {
            break;
        }
    }

    if (ns < jw || s == T{0})
    {
        const crd::usize ldt = twin.ld();
        const crd::usize ldv = v.ld();
        if (ns > 1 && s != T{0})
        {
            // Reflect the spike (first row of V over the NS undeflated columns).
            crd::containers::Array<T> work(alloc);
            work.resize(jw);
            for (crd::usize k = 0; k < ns; ++k)
            {
                work[k] = v.at(0, k);
            }
            const auto refl = detail::make_householder<T>(work.data(), ns);
            work[0] = T{1};
            // Zero the strict lower-by-2 of T.
            for (crd::usize i = 2; i < jw; ++i)
            {
                for (crd::usize j = 0; j + 2 <= i && j < jw; ++j)
                {
                    twin.at(i, j) = T{0};
                }
            }
            apply_h_left<T>(twin.data(), ldt, 0, 0, ns, jw, work.data(), refl.tau);
            apply_h_right<T>(twin.data(), ldt, 0, 0, ns, ns, work.data(), refl.tau);
            apply_h_right<T>(v.data(), ldv, 0, 0, jw, ns, work.data(), refl.tau);
            // Re-Hessenbergize the leading NS block, accumulate Q into V.
            crd::containers::Array<T> tauh(alloc);
            hessenberg<T>(twin, 0, ns - 1, tauh);
            Matrix<T> qh = form_hessenberg_q<T>(alloc, twin, 0, ns - 1, tauh);
            Matrix<T> vq(alloc, jw, jw);
            {
                constexpr Layout k_l = Layout::RowMajor;
                MatrixView<const T, k_l> vv{v.data(), jw, jw, ldv};
                MatrixView<const T, k_l> qq{qh.data(), jw, jw, qh.ld()};
                MatrixView<T, k_l> ovq{vq.data(), jw, jw, vq.ld()};
                gemm<T, k_l>(T{1}, vv, qq, T{0}, ovq, Trans::None, Trans::None, alloc);
            }
            for (crd::usize i = 0; i < jw; ++i)
            {
                for (crd::usize j = 0; j < jw; ++j)
                {
                    v.at(i, j) = vq.at(i, j);
                }
            }
        }

        // Copy the updated reduced window back into H (Hessenberg part only).
        if (kwtop > 0)
        {
            hh(kwtop, kwtop - 1) = s * v.at(0, 0);
        }
        for (crd::usize i = 0; i < jw; ++i)
        {
            for (crd::usize j = (i == 0 ? 0 : i - 1); j < jw; ++j)
            {
                hh(kwtop + i, kwtop + j) = twin.at(i, j);
            }
        }

        // Global similarity updates with V (scratch n×n covers both slab shapes).
        Matrix<T> scratch(alloc, n, n);
        const crd::usize ltop = wantt ? 0 : ktop;
        slab_right<T>(hd, ld, ltop, kwtop, kwtop - ltop, jw, v.data(), v.ld(), scratch, alloc);
        if (wantt && kbot + 1 < n)
        {
            slab_left_t<T>(hd, ld, kwtop, kbot + 1, jw, n - (kbot + 1), v.data(), v.ld(), scratch,
                           alloc);
        }
        if (wantz)
        {
            slab_right<T>(z.data(), z.ld(), iloz, kwtop, ihiz - iloz + 1, jw, v.data(), v.ld(),
                          scratch, alloc);
        }
    }

    res.nd = jw - ns;
    res.ns = ns - infqr;
    return res;
}

namespace
{
// v3d-2a — dtrevc (SIDE='R', HOWMNY='A', NOT back-transformed): right
// eigenvectors of the real quasi-upper-triangular Schur form T (n×n), by column
// back-substitution using dlaln2. Output vr (n×n, real-packed): a real
// eigenvalue at diagonal k → real eigenvector in column k; a complex-conjugate
// 2×2 block at (k-1,k) → the +imag eigenvalue's eigenvector as col(k-1)+i·col(k)
// (and its conjugate for the −imag eigenvalue). Each vector normalized ‖·‖∞=1.
template <typename T>
void dtrevc_right(crd::memory::IAllocator* alloc, const Matrix<T>& t_in, Matrix<T>& vr)
{
    const crd::usize n = t_in.rows();
    const T* td = t_in.data();
    const crd::usize ld = t_in.ld();
    auto t = [&](crd::usize i, crd::usize j) -> T { return td[i * ld + j]; };

    vr = Matrix<T>(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            vr.at(i, j) = T{0};
    if (n == 0)
    {
        return;
    }

    const T unfl = std::numeric_limits<T>::min();
    const T ulp = std::numeric_limits<T>::epsilon();
    const T smlnum = unfl * (static_cast<T>(n) / ulp);
    const T bignum = (T{1} - ulp) / smlnum;

    crd::containers::Array<T> cnorm(alloc);
    crd::containers::Array<T> wkr(alloc);
    crd::containers::Array<T> wki(alloc);
    cnorm.resize(n);
    wkr.resize(n);
    wki.resize(n);
    cnorm[0] = T{0};
    for (crd::usize j = 1; j < n; ++j)
    {
        T s = T{0};
        for (crd::usize i = 0; i < j; ++i)
        {
            s += std::abs(t(i, j));
        }
        cnorm[j] = s;
    }

    // Scale wkr[0..hi] (and wki if `imag`) by `sc`.
    auto dscal_work = [&](T sc, crd::isize hi, bool imag) {
        for (crd::isize k = 0; k <= hi; ++k)
        {
            wkr[static_cast<crd::usize>(k)] *= sc;
            if (imag)
            {
                wki[static_cast<crd::usize>(k)] *= sc;
            }
        }
    };

    int ip = 0;  // 0 real; -1 second-of-pair (process now); 1 skip (first-of-pair, done)
    for (crd::isize ki = static_cast<crd::isize>(n) - 1; ki >= 0; --ki)
    {
        if (ip == 1)
        {
            ip = 0;
            continue;
        }
        bool cplx = false;
        if (ki > 0 && t(static_cast<crd::usize>(ki), static_cast<crd::usize>(ki - 1)) != T{0})
        {
            ip = -1;
            cplx = true;
        }
        const crd::usize kiu = static_cast<crd::usize>(ki);

        if (!cplx)
        {
            // ---- Real right eigenvector at column ki ----
            const T wr = t(kiu, kiu);
            const T smin = std::max(ulp * std::abs(wr), smlnum);
            wkr[kiu] = T{1};
            for (crd::isize k = 0; k < ki; ++k)
            {
                wkr[static_cast<crd::usize>(k)] = -t(static_cast<crd::usize>(k), kiu);
            }
            // Solve (T(0:ki-1,0:ki-1) - wr·I)·X = scale·wkr.
            crd::isize j = ki - 1;
            while (j >= 0)
            {
                const crd::usize ju = static_cast<crd::usize>(j);
                crd::isize j1 = j;
                if (j > 0 && t(ju, ju - 1) != T{0})
                {
                    j1 = j - 1;
                }
                if (j1 == j)
                {
                    const T a[2][2] = {{t(ju, ju), T{0}}, {T{0}, T{0}}};
                    const T b[2][2] = {{wkr[ju], T{0}}, {T{0}, T{0}}};
                    const Ln2<T> r = dlaln2<T>(false, 1, 1, smin, T{1}, a, T{1}, T{1}, b, wr, T{0});
                    T x11 = r.x[0][0];
                    T scale = r.scale;
                    if (r.xnorm > T{1} && cnorm[ju] > bignum / r.xnorm)
                    {
                        x11 /= r.xnorm;
                        scale /= r.xnorm;
                    }
                    if (scale != T{1})
                    {
                        dscal_work(scale, ki, false);
                    }
                    wkr[ju] = x11;
                    for (crd::usize k = 0; k < ju; ++k)
                    {
                        wkr[k] -= x11 * t(k, ju);
                    }
                    j -= 1;
                }
                else
                {
                    const crd::usize j0 = ju - 1;
                    const T a[2][2] = {{t(j0, j0), t(j0, ju)}, {t(ju, j0), t(ju, ju)}};
                    const T b[2][2] = {{wkr[j0], T{0}}, {wkr[ju], T{0}}};
                    const Ln2<T> r = dlaln2<T>(false, 2, 1, smin, T{1}, a, T{1}, T{1}, b, wr, T{0});
                    T x11 = r.x[0][0];
                    T x21 = r.x[1][0];
                    T scale = r.scale;
                    if (r.xnorm > T{1})
                    {
                        const T beta = std::max(cnorm[j0], cnorm[ju]);
                        if (beta > bignum / r.xnorm)
                        {
                            x11 /= r.xnorm;
                            x21 /= r.xnorm;
                            scale /= r.xnorm;
                        }
                    }
                    if (scale != T{1})
                    {
                        dscal_work(scale, ki, false);
                    }
                    wkr[j0] = x11;
                    wkr[ju] = x21;
                    for (crd::usize k = 0; k < j0; ++k)
                    {
                        wkr[k] -= x11 * t(k, j0) + x21 * t(k, ju);
                    }
                    j -= 2;
                }
            }
            // Copy + normalize to ‖·‖∞ = 1; zero below ki.
            T emax = T{0};
            for (crd::isize k = 0; k <= ki; ++k)
            {
                emax = std::max(emax, std::abs(wkr[static_cast<crd::usize>(k)]));
            }
            const T remax = T{1} / emax;
            for (crd::isize k = 0; k <= ki; ++k)
            {
                vr.at(static_cast<crd::usize>(k), kiu) = wkr[static_cast<crd::usize>(k)] * remax;
            }
        }
        else
        {
            // ---- Complex right eigenvector (block (ki-1, ki)) ----
            const crd::usize k1 = kiu - 1;
            const T wr = t(kiu, kiu);
            const T wi = std::sqrt(std::abs(t(kiu, k1))) * std::sqrt(std::abs(t(k1, kiu)));
            const T smin = std::max(ulp * (std::abs(wr) + std::abs(wi)), smlnum);
            if (std::abs(t(k1, kiu)) >= std::abs(t(kiu, k1)))
            {
                wkr[k1] = T{1};
                wki[kiu] = wi / t(k1, kiu);
            }
            else
            {
                wkr[k1] = -wi / t(kiu, k1);
                wki[kiu] = T{1};
            }
            wkr[kiu] = T{0};
            wki[k1] = T{0};
            for (crd::isize k = 0; k + 1 < ki; ++k)  // k = 0 .. ki-2
            {
                const crd::usize ku = static_cast<crd::usize>(k);
                wkr[ku] = -wkr[k1] * t(ku, k1);
                wki[ku] = -wki[kiu] * t(ku, kiu);
            }
            // Solve (T(0:ki-2,0:ki-2) - (wr+i·wi))·X = scale·(wkr+i·wki).
            crd::isize j = ki - 2;
            while (j >= 0)
            {
                const crd::usize ju = static_cast<crd::usize>(j);
                crd::isize j1 = j;
                if (j > 0 && t(ju, ju - 1) != T{0})
                {
                    j1 = j - 1;
                }
                if (j1 == j)
                {
                    const T a[2][2] = {{t(ju, ju), T{0}}, {T{0}, T{0}}};
                    const T b[2][2] = {{wkr[ju], wki[ju]}, {T{0}, T{0}}};
                    const Ln2<T> r = dlaln2<T>(false, 1, 2, smin, T{1}, a, T{1}, T{1}, b, wr, wi);
                    T xr = r.x[0][0];
                    T xi = r.x[0][1];
                    T scale = r.scale;
                    if (r.xnorm > T{1} && cnorm[ju] > bignum / r.xnorm)
                    {
                        xr /= r.xnorm;
                        xi /= r.xnorm;
                        scale /= r.xnorm;
                    }
                    if (scale != T{1})
                    {
                        dscal_work(scale, ki, true);
                    }
                    wkr[ju] = xr;
                    wki[ju] = xi;
                    for (crd::usize k = 0; k < ju; ++k)
                    {
                        wkr[k] -= xr * t(k, ju);
                        wki[k] -= xi * t(k, ju);
                    }
                    j -= 1;
                }
                else
                {
                    const crd::usize j0 = ju - 1;
                    const T a[2][2] = {{t(j0, j0), t(j0, ju)}, {t(ju, j0), t(ju, ju)}};
                    const T b[2][2] = {{wkr[j0], wki[j0]}, {wkr[ju], wki[ju]}};
                    const Ln2<T> r = dlaln2<T>(false, 2, 2, smin, T{1}, a, T{1}, T{1}, b, wr, wi);
                    T x11 = r.x[0][0];
                    T x21 = r.x[1][0];
                    T x12 = r.x[0][1];
                    T x22 = r.x[1][1];
                    T scale = r.scale;
                    if (r.xnorm > T{1})
                    {
                        const T beta = std::max(cnorm[j0], cnorm[ju]);
                        if (beta > bignum / r.xnorm)
                        {
                            const T rec = T{1} / r.xnorm;
                            x11 *= rec;
                            x21 *= rec;
                            x12 *= rec;
                            x22 *= rec;
                            scale *= rec;
                        }
                    }
                    if (scale != T{1})
                    {
                        dscal_work(scale, ki, true);
                    }
                    wkr[j0] = x11;
                    wkr[ju] = x21;
                    wki[j0] = x12;
                    wki[ju] = x22;
                    for (crd::usize k = 0; k < j0; ++k)
                    {
                        wkr[k] -= x11 * t(k, j0) + x21 * t(k, ju);
                        wki[k] -= x12 * t(k, j0) + x22 * t(k, ju);
                    }
                    j -= 2;
                }
            }
            // Copy + normalize to max(|re|+|im|) = 1; zero below ki.
            T emax = T{0};
            for (crd::isize k = 0; k <= ki; ++k)
            {
                const crd::usize ku = static_cast<crd::usize>(k);
                emax = std::max(emax, std::abs(wkr[ku]) + std::abs(wki[ku]));
            }
            const T remax = T{1} / emax;
            for (crd::isize k = 0; k <= ki; ++k)
            {
                const crd::usize ku = static_cast<crd::usize>(k);
                vr.at(ku, k1) = wkr[ku] * remax;
                vr.at(ku, kiu) = wki[ku] * remax;
            }
        }

        if (ip == -1)
        {
            ip = 1;
        }
        else
        {
            ip = 0;
        }
    }
}

// v3d-2c-3 — ztrevc (SIDE='R', HOWMNY='A'): right eigenvectors of an UPPER-
// TRIANGULAR complex Schur form `t_in` (n×n), NOT back-transformed. Eigenvalue
// λ=t(ki,ki) → eigenvector in column ki, living in the leading [0..ki] subspace.
// Per-column triangular back-solve with the `smin` near-defective floor + inline
// overflow scaling (the same cnorm/bignum guard the real `dtrevc_right` uses —
// no `zlatrs`). NO normalization here (the public complex `eig` applies the
// D(non-sym)-4 ‖·‖₂=1 + phase convention once). The complex analog of
// `dtrevc_right` with NO 2×2 blocks ⇒ all scalar (no `dlaln2`). c32/c64.
template <typename T>
void ztrevc_right(crd::memory::IAllocator* alloc, const Matrix<T>& t_in, Matrix<T>& vr)
{
    static_assert(is_complex_v<T>, "ztrevc_right is complex-only (real is dtrevc_right)");
    using R = RealType<T>;
    const crd::usize n = t_in.rows();
    const T* td = t_in.data();
    const crd::usize ld = t_in.ld();
    auto t = [&](crd::usize i, crd::usize j) -> T { return td[i * ld + j]; };
    auto cabs1 = [](const T& x) -> R { return std::abs(x.re) + std::abs(x.im); };
    const T czero{R{0}, R{0}};

    vr = Matrix<T>(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
        for (crd::usize j = 0; j < n; ++j)
            vr.at(i, j) = czero;
    if (n == 0)
    {
        return;
    }

    const R unfl = std::numeric_limits<R>::min();
    const R ulp = std::numeric_limits<R>::epsilon();
    const R smlnum = unfl * (static_cast<R>(n) / ulp);
    const R bignum = (R{1} - ulp) / smlnum;

    // cnorm[j] = Σ_{i<j} cabs1(t(i,j)) — the strict-upper column 1-norms.
    crd::containers::Array<R> cnorm(alloc);
    cnorm.resize(n);
    cnorm[0] = R{0};
    for (crd::usize j = 1; j < n; ++j)
    {
        R s = R{0};
        for (crd::usize i = 0; i < j; ++i)
        {
            s += cabs1(t(i, j));
        }
        cnorm[j] = s;
    }

    crd::containers::Array<T> work(alloc);
    work.resize(n);

    for (crd::isize ki = static_cast<crd::isize>(n) - 1; ki >= 0; --ki)
    {
        const crd::usize kiu = static_cast<crd::usize>(ki);
        const T lambda = t(kiu, kiu);
        const R smin = std::max(ulp * cabs1(lambda), smlnum);

        work[kiu] = T{R{1}, R{0}};
        for (crd::usize k = 0; k < kiu; ++k)
        {
            work[k] = czero - t(k, kiu);
        }

        // Back-solve (T[0:ki,0:ki] − λ·I)·x = work over the leading triangle.
        for (crd::isize j = static_cast<crd::isize>(kiu) - 1; j >= 0; --j)
        {
            const crd::usize ju = static_cast<crd::usize>(j);
            T diag = t(ju, ju) - lambda;
            if (cabs1(diag) < smin)
            {
                diag = T{smin, R{0}};
            }
            // Scaled complex divide diag·x = work[ju], guarding overflow when the
            // perturbed diagonal is < 1 (mirrors dlaln2 + the dtrevc_right guard).
            R scale = R{1};
            const R d1 = cabs1(diag);
            const R b1 = cabs1(work[ju]);
            if (d1 < R{1} && b1 > R{1} && b1 > bignum * d1)
            {
                scale = R{1} / b1;
            }
            T x = (work[ju] * scale) / diag;
            const R xnorm = cabs1(x);
            if (xnorm > R{1} && cnorm[ju] > bignum / xnorm)
            {
                const R sc2 = R{1} / xnorm;
                x = x * sc2;
                scale = scale * sc2;
            }
            if (scale != R{1})
            {
                for (crd::usize k = 0; k <= kiu; ++k)
                {
                    work[k] = work[k] * scale;
                }
            }
            work[ju] = x;
            for (crd::usize i = 0; i < ju; ++i)
            {
                work[i] = work[i] - x * t(i, ju);
            }
        }

        for (crd::usize i = 0; i <= kiu; ++i)
        {
            vr.at(i, kiu) = work[i];
        }
    }
}

// v3d-1c-4 (multishift train) — dlaqr1: the (unnormalized) first column of the
// real shift polynomial (H-s1·I)(H-s2·I)·e1, used to seed a double-shift bulge.
// `hsub` points at the 0-based top-left of the n×n (n∈{2,3}) leading submatrix
// (row-major, leading dim `ld`); writes v[0..n-1]. Faithful LAPACK port.
template <typename T>
void dlaqr1(crd::usize n, const T* hsub, crd::usize ld, T sr1, T si1, T sr2, T si2, T* v)
{
    auto hh = [&](crd::usize i, crd::usize j) -> T { return hsub[i * ld + j]; };
    if (n == 2)
    {
        const T s = std::abs(hh(0, 0) - sr2) + std::abs(si2) + std::abs(hh(1, 0));
        if (s == T{0})
        {
            v[0] = T{0};
            v[1] = T{0};
        }
        else
        {
            const T h21s = hh(1, 0) / s;
            v[0] = h21s * hh(0, 1) + (hh(0, 0) - sr1) * ((hh(0, 0) - sr2) / s) - si1 * (si2 / s);
            v[1] = h21s * (hh(0, 0) + hh(1, 1) - sr1 - sr2);
        }
    }
    else
    {
        const T s = std::abs(hh(0, 0) - sr2) + std::abs(si2) + std::abs(hh(1, 0)) + std::abs(hh(2, 0));
        if (s == T{0})
        {
            v[0] = T{0};
            v[1] = T{0};
            v[2] = T{0};
        }
        else
        {
            const T h21s = hh(1, 0) / s;
            const T h31s = hh(2, 0) / s;
            v[0] = (hh(0, 0) - sr1) * ((hh(0, 0) - sr2) / s) - si1 * (si2 / s) + hh(0, 1) * h21s +
                   hh(0, 2) * h31s;
            v[1] = h21s * (hh(0, 0) + hh(1, 1) - sr1 - sr2) + hh(1, 2) * h31s;
            v[2] = h31s * (hh(0, 0) + hh(2, 2) - sr1 - sr2) + h21s * hh(2, 1);
        }
    }
}

// v3d-1c-4 (multishift train) — dlaqr5: a single small-bulge multi-shift QR
// sweep on the active block [ktop, kbot] (0-based inclusive) of the global
// Hessenberg `hd` (n×n, row-major). Packs `nshifts` shifts (even; conjugate
// pairs in sr/si) into `nbmps = nshifts/2` bulges, chases the chain down the
// diagonal accumulating each near-diagonal reflector into a `kdu×kdu`
// orthogonal `U`, then updates the far-from-diagonal slabs of H (and Z, if
// `wantz`) by a single `gemm` each (via `slab_left_t`/`slab_right`) — the BLAS-3
// arithmetic-intensity lever (KACC22=1). Faithful LAPACK `dlaqr5` port; written
// 1-based internally (converted at the array boundary), like the `dtrexc` port.
// With nshifts=2 this reproduces one `dshift_sweep` exactly (the M1 gate).
template <typename T>
void dlaqr5_sweep(crd::memory::IAllocator* alloc, bool wantt, bool wantz, crd::usize n, crd::usize ktop0,
                  crd::usize kbot0, const T* sr_in, const T* si_in, crd::usize nshifts, T* hd,
                  crd::usize ld, crd::usize iloz0, crd::usize ihiz0, T* zd, crd::usize zld)
{
    if (nshifts < 2 || ktop0 >= kbot0)
    {
        return;
    }
    // 1-based accessors (i,j >= 1).
    auto h = [&](crd::isize i, crd::isize j) -> T& { return hd[(i - 1) * ld + (j - 1)]; };
    // Z far-update uses slab_right(zd, ...) directly (raw pointer); no z() accessor.
    const crd::isize ktop = static_cast<crd::isize>(ktop0) + 1;
    const crd::isize kbot = static_cast<crd::isize>(kbot0) + 1;
    const crd::isize iloz = static_cast<crd::isize>(iloz0) + 1;
    const crd::isize ihiz = static_cast<crd::isize>(ihiz0) + 1;
    const crd::isize nn = static_cast<crd::isize>(n);

    // Local copies of the shifts (dlaqr5 reorders them; mirror that).
    crd::containers::Array<T> sr(alloc);
    crd::containers::Array<T> si(alloc);
    sr.resize(nshifts);
    si.resize(nshifts);
    for (crd::usize k = 0; k < nshifts; ++k)
    {
        sr[k] = sr_in[k];
        si[k] = si_in[k];
    }
    // Shuffle shifts so complex-conjugate pairs are adjacent (1-based loop).
    for (crd::usize ii = 1; ii + 2 <= nshifts; ii += 2)
    {
        if (si[ii - 1] != -si[ii])
        {
            std::swap(sr[ii - 1], sr[ii]);
            std::swap(sr[ii], sr[ii + 1]);
            std::swap(si[ii - 1], si[ii]);
            std::swap(si[ii], si[ii + 1]);
        }
    }
    const crd::isize ns = static_cast<crd::isize>(nshifts - (nshifts % 2));
    auto shift_r = [&](crd::isize m) -> T { return sr[static_cast<crd::usize>(m - 1)]; };  // 1-based
    auto shift_i = [&](crd::isize m) -> T { return si[static_cast<crd::usize>(m - 1)]; };

    const T eps = std::numeric_limits<T>::epsilon();
    const T safmin = std::numeric_limits<T>::min();
    const T ulp = eps;
    const T smlnum = safmin * (static_cast<T>(nn) / ulp);

    if (ktop + 2 <= kbot)
    {
        h(ktop + 2, ktop) = T{0};
    }

    const crd::isize nbmps = ns / 2;
    const crd::isize kdu = 4 * nbmps;

    // Per-bulge reflector workspace (3 x nbmps), via vref(row, m): row 1 = tau,
    // row 2 = v2, row 3 = v3 (the implicit v1 = 1).
    crd::containers::Array<T> vws(alloc);
    vws.resize(static_cast<crd::usize>(3 * nbmps));
    auto vref = [&](crd::isize row, crd::isize m) -> T& {
        return vws[static_cast<crd::usize>(3 * (m - 1) + (row - 1))];
    };
    T vt[3] = {T{0}, T{0}, T{0}};

    Matrix<T> umat(alloc, static_cast<crd::usize>(kdu), static_cast<crd::usize>(kdu));
    Matrix<T> scratch(alloc, n, n);
    auto u = [&](crd::isize i, crd::isize j) -> T& {
        return umat.data()[static_cast<crd::usize>(i - 1) * umat.ld() + static_cast<crd::usize>(j - 1)];
    };

    for (crd::isize incol = ktop - 2 * nbmps + 1; incol <= kbot - 2; incol += 2 * nbmps)
    {
        const crd::isize jtop = std::max(ktop, incol);
        const crd::isize ndcol = incol + kdu;
        // U := I (kdu×kdu).
        for (crd::isize a = 1; a <= kdu; ++a)
        {
            for (crd::isize b = 1; b <= kdu; ++b)
            {
                u(a, b) = (a == b) ? T{1} : T{0};
            }
        }

        const crd::isize krcol_hi = std::min(incol + 2 * nbmps - 1, kbot - 2);
        for (crd::isize krcol = incol; krcol <= krcol_hi; ++krcol)
        {
            const crd::isize mtop = std::max<crd::isize>(1, (ktop - krcol) / 2 + 1);
            const crd::isize mbot = std::min<crd::isize>(nbmps, (kbot - krcol - 1) / 2);
            const crd::isize m22 = mbot + 1;
            const bool bmp22 = (mbot < nbmps) && (krcol + 2 * (m22 - 1) == kbot - 2);

            // ---- Special 2×2 bulge at the bottom ----
            if (bmp22)
            {
                const crd::isize k = krcol + 2 * (m22 - 1);
                if (k == ktop - 1)
                {
                    dlaqr1<T>(2, &h(k + 1, k + 1), ld, shift_r(2 * m22 - 1), shift_i(2 * m22 - 1), shift_r(2 * m22),
                              shift_i(2 * m22), &vref(1, m22));
                    T beta = vref(1, m22);
                    T vv[2] = {beta, vref(2, m22)};
                    const auto hh = detail::make_householder<T>(vv, 2);
                    vref(1, m22) = hh.tau;
                    vref(2, m22) = vv[1];
                }
                else
                {
                    T beta = h(k + 1, k);
                    T vv[2] = {beta, h(k + 2, k)};
                    const auto hh = detail::make_householder<T>(vv, 2);
                    vref(1, m22) = hh.tau;
                    vref(2, m22) = vv[1];
                    h(k + 1, k) = hh.beta;
                    h(k + 2, k) = T{0};
                }
                const T t1 = vref(1, m22);
                const T t2 = t1 * vref(2, m22);
                for (crd::isize j = jtop; j <= std::min(kbot, k + 3); ++j)
                {
                    const T refsum = h(j, k + 1) + vref(2, m22) * h(j, k + 2);
                    h(j, k + 1) -= refsum * t1;
                    h(j, k + 2) -= refsum * t2;
                }
                const crd::isize jbot_c = std::min(ndcol, kbot);
                for (crd::isize j = k + 1; j <= jbot_c; ++j)
                {
                    const T refsum = h(k + 1, j) + vref(2, m22) * h(k + 2, j);
                    h(k + 1, j) -= refsum * t1;
                    h(k + 2, j) -= refsum * t2;
                }
                if (k >= ktop && h(k + 1, k) != T{0})
                {
                    T tst1 = std::abs(h(k, k)) + std::abs(h(k + 1, k + 1));
                    if (tst1 == T{0})
                    {
                        if (k >= ktop + 1) tst1 += std::abs(h(k, k - 1));
                        if (k >= ktop + 2) tst1 += std::abs(h(k, k - 2));
                        if (k >= ktop + 3) tst1 += std::abs(h(k, k - 3));
                        if (k <= kbot - 2) tst1 += std::abs(h(k + 2, k + 1));
                        if (k <= kbot - 3) tst1 += std::abs(h(k + 3, k + 1));
                        if (k <= kbot - 4) tst1 += std::abs(h(k + 4, k + 1));
                    }
                    if (std::abs(h(k + 1, k)) <= std::max(smlnum, ulp * tst1))
                    {
                        const T h12 = std::max(std::abs(h(k + 1, k)), std::abs(h(k, k + 1)));
                        const T h21 = std::min(std::abs(h(k + 1, k)), std::abs(h(k, k + 1)));
                        const T h11 = std::max(std::abs(h(k + 1, k + 1)), std::abs(h(k, k) - h(k + 1, k + 1)));
                        const T h22 = std::min(std::abs(h(k + 1, k + 1)), std::abs(h(k, k) - h(k + 1, k + 1)));
                        const T scl = h11 + h12;
                        const T tst2 = h22 * (h11 / scl);
                        if (tst2 == T{0} || h21 * (h12 / scl) <= std::max(smlnum, ulp * tst2))
                        {
                            h(k + 1, k) = T{0};
                        }
                    }
                }
                // Accumulate the 2×2 reflection into U.
                const crd::isize kms = k - incol;
                for (crd::isize j = std::max<crd::isize>(1, ktop - incol); j <= kdu; ++j)
                {
                    const T refsum = u(j, kms + 1) + vref(2, m22) * u(j, kms + 2);
                    u(j, kms + 1) -= refsum * t1;
                    u(j, kms + 2) -= refsum * t2;
                }
            }

            // ---- Normal case: chain of 3×3 reflections (m = mbot..mtop) ----
            for (crd::isize m = mbot; m >= mtop; --m)
            {
                const crd::isize k = krcol + 2 * (m - 1);
                if (k == ktop - 1)
                {
                    dlaqr1<T>(3, &h(ktop, ktop), ld, shift_r(2 * m - 1), shift_i(2 * m - 1), shift_r(2 * m), shift_i(2 * m),
                              &vref(1, m));
                    T alpha = vref(1, m);
                    T vv[3] = {alpha, vref(2, m), vref(3, m)};
                    const auto hh = detail::make_householder<T>(vv, 3);
                    vref(1, m) = hh.tau;
                    vref(2, m) = vv[1];
                    vref(3, m) = vv[2];
                }
                else
                {
                    // Delayed transformation of the row below the m-th bulge.
                    const T t1d = vref(1, m);
                    const T t2d = t1d * vref(2, m);
                    const T t3d = t1d * vref(3, m);
                    const T refsum0 = vref(3, m) * h(k + 3, k + 2);
                    h(k + 3, k) = -refsum0 * t1d;
                    h(k + 3, k + 1) = -refsum0 * t2d;
                    h(k + 3, k + 2) -= refsum0 * t3d;
                    // Reflection to move the m-th bulge one step.
                    T beta = h(k + 1, k);
                    T vv[3] = {beta, h(k + 2, k), h(k + 3, k)};
                    const auto hh = detail::make_householder<T>(vv, 3);
                    if (h(k + 3, k) != T{0} || h(k + 3, k + 1) != T{0} || h(k + 3, k + 2) == T{0})
                    {
                        vref(1, m) = hh.tau;
                        vref(2, m) = vv[1];
                        vref(3, m) = vv[2];
                        h(k + 1, k) = hh.beta;
                        h(k + 2, k) = T{0};
                        h(k + 3, k) = T{0};
                    }
                    else
                    {
                        // Bulge collapsed: try to reintroduce ignoring H(k+1,k),H(k+2,k).
                        dlaqr1<T>(3, &h(k + 1, k + 1), ld, shift_r(2 * m - 1), shift_i(2 * m - 1), shift_r(2 * m),
                                  shift_i(2 * m), vt);
                        const auto hh2 = detail::make_householder<T>(vt, 3);
                        const T tt1 = hh2.tau;
                        const T tt2 = tt1 * vt[1];
                        const T tt3 = tt1 * vt[2];
                        const T refsum = h(k + 1, k) + vt[1] * h(k + 2, k);
                        if (std::abs(h(k + 2, k) - refsum * tt2) + std::abs(refsum * tt3) >
                            ulp * (std::abs(h(k, k)) + std::abs(h(k + 1, k + 1)) + std::abs(h(k + 2, k + 2))))
                        {
                            // New bulge would create non-negligible fill; keep the old one.
                            vref(1, m) = hh.tau;
                            vref(2, m) = vv[1];
                            vref(3, m) = vv[2];
                            h(k + 1, k) = hh.beta;
                            h(k + 2, k) = T{0};
                            h(k + 3, k) = T{0};
                        }
                        else
                        {
                            h(k + 1, k) -= refsum * tt1;
                            h(k + 2, k) = T{0};
                            h(k + 3, k) = T{0};
                            vref(1, m) = tt1;
                            vref(2, m) = vt[1];
                            vref(3, m) = vt[2];
                        }
                    }
                }
                // Apply from right + first column from left (vigilant-deflation needs these).
                const T t1 = vref(1, m);
                const T t2 = t1 * vref(2, m);
                const T t3 = t1 * vref(3, m);
                for (crd::isize j = jtop; j <= std::min(kbot, k + 3); ++j)
                {
                    const T refsum = h(j, k + 1) + vref(2, m) * h(j, k + 2) + vref(3, m) * h(j, k + 3);
                    h(j, k + 1) -= refsum * t1;
                    h(j, k + 2) -= refsum * t2;
                    h(j, k + 3) -= refsum * t3;
                }
                {
                    const T refsum = h(k + 1, k + 1) + vref(2, m) * h(k + 2, k + 1) + vref(3, m) * h(k + 3, k + 1);
                    h(k + 1, k + 1) -= refsum * t1;
                    h(k + 2, k + 1) -= refsum * t2;
                    h(k + 3, k + 1) -= refsum * t3;
                }
                if (k < ktop)
                {
                    continue;
                }
                if (h(k + 1, k) != T{0})
                {
                    T tst1 = std::abs(h(k, k)) + std::abs(h(k + 1, k + 1));
                    if (tst1 == T{0})
                    {
                        if (k >= ktop + 1) tst1 += std::abs(h(k, k - 1));
                        if (k >= ktop + 2) tst1 += std::abs(h(k, k - 2));
                        if (k >= ktop + 3) tst1 += std::abs(h(k, k - 3));
                        if (k <= kbot - 2) tst1 += std::abs(h(k + 2, k + 1));
                        if (k <= kbot - 3) tst1 += std::abs(h(k + 3, k + 1));
                        if (k <= kbot - 4) tst1 += std::abs(h(k + 4, k + 1));
                    }
                    if (std::abs(h(k + 1, k)) <= std::max(smlnum, ulp * tst1))
                    {
                        const T h12 = std::max(std::abs(h(k + 1, k)), std::abs(h(k, k + 1)));
                        const T h21 = std::min(std::abs(h(k + 1, k)), std::abs(h(k, k + 1)));
                        const T h11 = std::max(std::abs(h(k + 1, k + 1)), std::abs(h(k, k) - h(k + 1, k + 1)));
                        const T h22 = std::min(std::abs(h(k + 1, k + 1)), std::abs(h(k, k) - h(k + 1, k + 1)));
                        const T scl = h11 + h12;
                        const T tst2 = h22 * (h11 / scl);
                        if (tst2 == T{0} || h21 * (h12 / scl) <= std::max(smlnum, ulp * tst2))
                        {
                            h(k + 1, k) = T{0};
                        }
                    }
                }
            }

            // ---- Delayed left updates (within the slab) for the chain ----
            // KACC22=1 (accumulate) convention: the in-slab left update runs to
            // MIN(NDCOL, KBOT); columns beyond go through the gemm far-update.
            const crd::isize jbot_acc = std::min(ndcol, kbot);
            for (crd::isize m = mbot; m >= mtop; --m)
            {
                const crd::isize k = krcol + 2 * (m - 1);
                const T t1 = vref(1, m);
                const T t2 = t1 * vref(2, m);
                const T t3 = t1 * vref(3, m);
                for (crd::isize j = std::max(ktop, krcol + 2 * m); j <= jbot_acc; ++j)
                {
                    const T refsum = h(k + 1, j) + vref(2, m) * h(k + 2, j) + vref(3, m) * h(k + 3, j);
                    h(k + 1, j) -= refsum * t1;
                    h(k + 2, j) -= refsum * t2;
                    h(k + 3, j) -= refsum * t3;
                }
            }

            // ---- Accumulate the chain reflections into U ----
            for (crd::isize m = mbot; m >= mtop; --m)
            {
                const crd::isize k = krcol + 2 * (m - 1);
                const crd::isize kms = k - incol;
                crd::isize i2 = std::max<crd::isize>(1, ktop - incol);
                i2 = std::max(i2, kms - (krcol - incol) + 1);
                const crd::isize i4 = std::min(kdu, krcol + 2 * (mbot - 1) - incol + 5);
                const T t1 = vref(1, m);
                const T t2 = t1 * vref(2, m);
                const T t3 = t1 * vref(3, m);
                for (crd::isize j = i2; j <= i4; ++j)
                {
                    const T refsum = u(j, kms + 1) + vref(2, m) * u(j, kms + 2) + vref(3, m) * u(j, kms + 3);
                    u(j, kms + 1) -= refsum * t1;
                    u(j, kms + 2) -= refsum * t2;
                    u(j, kms + 3) -= refsum * t3;
                }
            }
        }

        // ---- Far-from-diagonal updates via gemm using the accumulated U ----
        const crd::isize jtop_g = wantt ? 1 : ktop;
        const crd::isize jbot_g = wantt ? nn : kbot;
        const crd::isize k1 = std::max<crd::isize>(1, ktop - incol);
        const crd::isize nu = (kdu - std::max<crd::isize>(0, ndcol - kbot)) - k1 + 1;
        if (nu > 0)
        {
            const crd::usize nus = static_cast<crd::usize>(nu);
            const T* ublk = &u(k1, k1);
            const crd::usize uld = umat.ld();
            // Horizontal: H(incol+k1 : .., jcol0 : jbot_g) := Uᵀ · H(...).
            const crd::isize jcol0 = std::min(ndcol, kbot) + 1;
            if (jcol0 <= jbot_g)
            {
                slab_left_t<T>(hd, ld, static_cast<crd::usize>(incol + k1 - 1),
                               static_cast<crd::usize>(jcol0 - 1), nus,
                               static_cast<crd::usize>(jbot_g - jcol0 + 1), ublk, uld, scratch, alloc);
            }
            // Vertical: H(jtop_g : maxki-1, incol+k1 : ..) := H(...) · U.
            const crd::isize maxki = std::max(ktop, incol);
            if (jtop_g <= maxki - 1)
            {
                slab_right<T>(hd, ld, static_cast<crd::usize>(jtop_g - 1),
                              static_cast<crd::usize>(incol + k1 - 1),
                              static_cast<crd::usize>(maxki - jtop_g), nus, ublk, uld, scratch, alloc);
            }
            // Z multiply: Z(iloz:ihiz, incol+k1 : ..) := Z(...) · U.
            if (wantz)
            {
                slab_right<T>(zd, zld, static_cast<crd::usize>(iloz - 1),
                              static_cast<crd::usize>(incol + k1 - 1),
                              static_cast<crd::usize>(ihiz - iloz + 1), nus, ublk, uld, scratch, alloc);
            }
        }
    }
}

// One Francis double-shift QR sweep on the active block [ktop, kbot] of the
// global Hessenberg `hd` (n×n), using the EXTERNAL shift pair
// (rt1r±i·rt1i, rt2r±i·rt2i). Bulge-chase identical to `real_schur`'s inner
// step; updates rows/cols [i1,i2] of H + Z[iloz..ihiz] (if wantz). The driver
// supplies AED-derived shifts.
template <typename T>
void dshift_sweep(T* hd, crd::usize ld, crd::usize ktop, crd::usize kbot, T rt1r, T rt1i, T rt2r,
                  T rt2i, T* zd, crd::usize zld, bool wantz, crd::usize iloz, crd::usize ihiz,
                  crd::usize i1, crd::usize i2)
{
    auto h = [&](crd::usize a, crd::usize b) -> T& { return hd[a * ld + b]; };
    const crd::usize l = ktop;
    const crd::usize i = kbot;
    const T ulp = std::numeric_limits<T>::epsilon();

    crd::usize m = l;
    T v[3] = {T{0}, T{0}, T{0}};
    for (crd::isize mm_s = static_cast<crd::isize>(i) - 2; mm_s >= static_cast<crd::isize>(l); --mm_s)
    {
        const crd::usize mm = static_cast<crd::usize>(mm_s);
        T h21s = h(mm + 1, mm);
        T s = std::abs(h(mm, mm) - rt2r) + std::abs(rt2i) + std::abs(h21s);
        h21s = h(mm + 1, mm) / s;
        v[0] = h21s * h(mm, mm + 1) + (h(mm, mm) - rt1r) * ((h(mm, mm) - rt2r) / s) - rt1i * (rt2i / s);
        v[1] = h21s * (h(mm, mm) + h(mm + 1, mm + 1) - rt1r - rt2r);
        v[2] = h21s * h(mm + 2, mm + 1);
        s = std::abs(v[0]) + std::abs(v[1]) + std::abs(v[2]);
        v[0] /= s;
        v[1] /= s;
        v[2] /= s;
        m = mm;
        if (mm == l)
        {
            break;
        }
        if (std::abs(h(mm, mm - 1)) * (std::abs(v[1]) + std::abs(v[2])) <=
            ulp * std::abs(v[0]) *
                (std::abs(h(mm - 1, mm - 1)) + std::abs(h(mm, mm)) + std::abs(h(mm + 1, mm + 1))))
        {
            break;
        }
    }

    for (crd::usize kc = m; kc + 1 <= i; ++kc)
    {
        const crd::usize nr = std::min<crd::usize>(3, i - kc + 1);
        if (kc > m)
        {
            for (crd::usize r = 0; r < nr; ++r)
            {
                v[r] = h(kc + r, kc - 1);
            }
        }
        const auto hh = detail::make_householder<T>(v, nr);
        const T t1 = hh.tau;
        if (kc > m)
        {
            h(kc, kc - 1) = hh.beta;
            h(kc + 1, kc - 1) = T{0};
            if (kc < i - 1)
            {
                h(kc + 2, kc - 1) = T{0};
            }
        }
        else if (m > l)
        {
            h(kc, kc - 1) = h(kc, kc - 1) * (T{1} - t1);
        }
        const T v2 = v[1];
        const T t2 = t1 * v2;
        if (nr == 3)
        {
            const T v3 = v[2];
            const T t3 = t1 * v3;
            for (crd::usize j = kc; j <= i2; ++j)
            {
                const T sum = h(kc, j) + v2 * h(kc + 1, j) + v3 * h(kc + 2, j);
                h(kc, j) -= sum * t1;
                h(kc + 1, j) -= sum * t2;
                h(kc + 2, j) -= sum * t3;
            }
            const crd::usize jmax = std::min(kc + 3, i);
            for (crd::usize j = i1; j <= jmax; ++j)
            {
                const T sum = h(j, kc) + v2 * h(j, kc + 1) + v3 * h(j, kc + 2);
                h(j, kc) -= sum * t1;
                h(j, kc + 1) -= sum * t2;
                h(j, kc + 2) -= sum * t3;
            }
            if (wantz)
            {
                for (crd::usize j = iloz; j <= ihiz; ++j)
                {
                    const T sum = zd[j * zld + kc] + v2 * zd[j * zld + kc + 1] + v3 * zd[j * zld + kc + 2];
                    zd[j * zld + kc] -= sum * t1;
                    zd[j * zld + kc + 1] -= sum * t2;
                    zd[j * zld + kc + 2] -= sum * t3;
                }
            }
        }
        else if (nr == 2)
        {
            for (crd::usize j = kc; j <= i2; ++j)
            {
                const T sum = h(kc, j) + v2 * h(kc + 1, j);
                h(kc, j) -= sum * t1;
                h(kc + 1, j) -= sum * t2;
            }
            for (crd::usize j = i1; j <= i; ++j)
            {
                const T sum = h(j, kc) + v2 * h(j, kc + 1);
                h(j, kc) -= sum * t1;
                h(j, kc + 1) -= sum * t2;
            }
            if (wantz)
            {
                for (crd::usize j = iloz; j <= ihiz; ++j)
                {
                    const T sum = zd[j * zld + kc] + v2 * zd[j * zld + kc + 1];
                    zd[j * zld + kc] -= sum * t1;
                    zd[j * zld + kc + 1] -= sum * t2;
                }
            }
        }
    }
}

// Finish the block [ktop, kbot] (≤ NMIN) by dlahqr (`real_schur` on a copy),
// writing the Schur form back into the global H and accumulating the block's
// orthogonal factor into the global H slabs + Z. wr/wi receive the block eigs.
template <typename T>
void schur_small_block(crd::memory::IAllocator* alloc, T* hd, crd::usize ld, crd::usize n,
                       crd::usize ktop, crd::usize kbot, T* zd, crd::usize zld, bool wantz,
                       crd::usize iloz, crd::usize ihiz, crd::containers::Array<T>& wr,
                       crd::containers::Array<T>& wi)
{
    const crd::usize nh = kbot - ktop + 1;
    auto h = [&](crd::usize a, crd::usize b) -> T& { return hd[a * ld + b]; };
    Matrix<T> win(alloc, nh, nh);
    for (crd::usize a = 0; a < nh; ++a)
    {
        for (crd::usize b = 0; b < nh; ++b)
        {
            win.at(a, b) = (b + 1 >= a) ? h(ktop + a, ktop + b) : T{0};
        }
    }
    RealSchur<T> sch = real_schur<T>(alloc, win, 0, nh - 1, true);
    for (crd::usize a = 0; a < nh; ++a)
    {
        for (crd::usize b = (a == 0 ? 0 : a - 1); b < nh; ++b)
        {
            h(ktop + a, ktop + b) = sch.t.at(a, b);
        }
        wr[ktop + a] = sch.wr[a];
        wi[ktop + a] = sch.wi[a];
    }
    Matrix<T> scratch(alloc, n, n);
    slab_right<T>(hd, ld, 0, ktop, ktop, nh, sch.z.data(), sch.z.ld(), scratch, alloc);
    if (kbot + 1 < n)
    {
        slab_left_t<T>(hd, ld, ktop, kbot + 1, nh, n - (kbot + 1), sch.z.data(), sch.z.ld(), scratch,
                       alloc);
    }
    if (wantz)
    {
        slab_right<T>(zd, zld, iloz, ktop, ihiz - iloz + 1, nh, sch.z.data(), sch.z.ld(), scratch,
                      alloc);
    }
}

// gebak_right — undo the `balance` similarity on the columns of the right
// eigenvector matrix `v` (n×m, RowMajor, leading dim `ldv`). Faithful LAPACK
// dgebak (SIDE='R', JOB='B'): first the radix-2 row scaling over [ilo,ihi]
// (right eigenvectors scale by `scale[i]`), then the isolating row permutation
// at the corners (1-based logic mirroring dgebak.f). `ilo`/`ihi` are 0-based
// inclusive; `scale` is the array `balance` filled (perm index outside the
// block, scale factor inside). The v3d-2b back-transform's 3rd stage.
template <typename V, typename S>
void gebak_right(V* v, crd::usize ldv, crd::usize n, crd::usize m, crd::usize ilo, crd::usize ihi,
                 const S* scale)
{
    // Two scalar types: `v` real (f32/f64) or complex (Complex<R>); `scale` always
    // REAL (R) — `balance` stores real scale factors + perm indices for both paths.
    if (n == 0)
    {
        return;
    }
    // ---- backward balance: scale rows [ilo, ihi] ----
    for (crd::usize i = ilo; i <= ihi; ++i)
    {
        const S s = scale[i];
        for (crd::usize j = 0; j < m; ++j)
        {
            v[i * ldv + j] = v[i * ldv + j] * s;
        }
    }
    // ---- backward permutation (faithful dgebak.f, 1-based) ----
    const crd::isize ilo1 = static_cast<crd::isize>(ilo) + 1;
    const crd::isize ihi1 = static_cast<crd::isize>(ihi) + 1;
    const crd::isize nn = static_cast<crd::isize>(n);
    for (crd::isize ii = 1; ii <= nn; ++ii)
    {
        crd::isize i = ii;
        if (i >= ilo1 && i <= ihi1)
        {
            continue;  // inside the active block — no isolating permutation
        }
        if (i < ilo1)
        {
            i = ilo1 - ii;  // remap: counts down from ilo-1 as ii grows
        }
        const crd::isize k = static_cast<crd::isize>(scale[static_cast<crd::usize>(i - 1)]);
        if (k == i)
        {
            continue;
        }
        V* ri = v + static_cast<crd::usize>(i - 1) * ldv;
        V* rk = v + static_cast<crd::usize>(k - 1) * ldv;
        for (crd::usize j = 0; j < m; ++j)
        {
            const V t = ri[j];
            ri[j] = rk[j];
            rk[j] = t;
        }
    }
}
} // namespace

template <typename T>
RealSchur<T> schur_aed(crd::memory::IAllocator* alloc, const Matrix<T>& h_in, crd::usize ilo,
                       crd::usize ihi, bool vectors, crd::usize* sweeps)
{
    static_assert(!is_complex_v<T>, "schur_aed is real-only");
    const crd::usize n = h_in.rows();
    RealSchur<T> out(alloc);
    out.t = h_in.clone();
    out.wr.resize(n);
    out.wi.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        out.wr[k] = T{0};
        out.wi[k] = T{0};
    }
    if (vectors)
    {
        out.z = Matrix<T>(alloc, n, n);
        out.z.set_identity();
    }
    if (sweeps != nullptr)
    {
        *sweeps = 0;
    }
    if (n == 0)
    {
        out.converged = true;
        return out;
    }

    // dlahqr crossover: below this, pure double-shift QR (real_schur) is faster
    // than AED — even with the BLAS-3 multishift train (dlaqr5) wired into the
    // sweep, the AED per-iteration overhead (window Schur + reorder + spike test)
    // does not amortize for blocks < ~200. Confirmed by measurement (v3d-1c-4 M3:
    // at NMIN=60, n=100/200 AED+train ran 0.64×/0.92× of pure dlahqr and 0.83×
    // Eigen — a hard-gate regression). The train's payoff is the large-N regime
    // (n=400: 2.10× Eigen / 1.70× LAPACK, 3 train passes vs 184 double-shifts).
    constexpr crd::usize nmin = 200;
    constexpr crd::usize nibble = 14;
    T* hd = out.t.data();
    const crd::usize ld = out.t.ld();
    T* zd = vectors ? out.z.data() : nullptr;
    const crd::usize zld = vectors ? out.z.ld() : 0;
    auto h = [&](crd::usize a, crd::usize b) -> T& { return hd[a * ld + b]; };

    for (crd::usize k = 0; k < ilo; ++k)
    {
        out.wr[k] = h(k, k);
    }
    for (crd::usize k = ihi + 1; k < n; ++k)
    {
        out.wr[k] = h(k, k);
    }

    const T eps = std::numeric_limits<T>::epsilon();
    const T safmin = std::numeric_limits<T>::min();
    const T ulp = eps;
    const T smlnum = safmin * (static_cast<T>(ihi - ilo + 1) / ulp);
    crd::usize total_sweeps = 0;

    crd::isize kbot = static_cast<crd::isize>(ihi);
    crd::usize stall = 0;  // consecutive cycles on the same kbot without progress
    crd::usize last_kbot = ihi + 1;
    const crd::usize itmax = 30 * std::max<crd::usize>(10, ihi - ilo + 1);
    crd::usize iters = 0;

    while (kbot >= static_cast<crd::isize>(ilo))
    {
        const crd::usize kb = static_cast<crd::usize>(kbot);
        if (++iters > itmax)
        {
            out.converged = false;
            return out;
        }
        // Find the active block top: split at a negligible subdiagonal.
        crd::usize ktop = ilo;
        for (crd::usize kk = kb; kk > ilo; --kk)
        {
            const T tst = (std::abs(h(kk - 1, kk - 1)) + std::abs(h(kk, kk)));
            const T thr = (tst == T{0}) ? smlnum : ulp * tst;
            if (std::abs(h(kk, kk - 1)) <= std::max(smlnum, thr))
            {
                h(kk, kk - 1) = T{0};
                ktop = kk;
                break;
            }
        }
        const crd::usize nh = kb - ktop + 1;

        if (nh == 1)
        {
            out.wr[kb] = h(kb, kb);
            out.wi[kb] = T{0};
            kbot = static_cast<crd::isize>(ktop) - 1;
            stall = 0;
            continue;
        }
        if (nh == 2)
        {
            T aa = h(kb - 1, kb - 1);
            T bb = h(kb - 1, kb);
            T cc = h(kb, kb - 1);
            T dd = h(kb, kb);
            const auto o = dlanv2<T>(aa, bb, cc, dd);
            h(kb - 1, kb - 1) = aa;
            h(kb - 1, kb) = bb;
            h(kb, kb - 1) = cc;
            h(kb, kb) = dd;
            out.wr[kb - 1] = o.rt1r;
            out.wi[kb - 1] = o.rt1i;
            out.wr[kb] = o.rt2r;
            out.wi[kb] = o.rt2i;
            if (kb + 1 < n)
            {
                drot_rows<T>(hd, ld, kb - 1, kb, kb + 1, n, o.cs, o.sn);
            }
            drot_cols<T>(hd, ld, kb - 1, kb, 0, kb - 1, o.cs, o.sn);
            if (vectors)
            {
                drot_cols<T>(zd, zld, kb - 1, kb, ilo, ihi + 1, o.cs, o.sn);
            }
            kbot = static_cast<crd::isize>(ktop) - 1;
            stall = 0;
            continue;
        }

        if (kb != last_kbot)
        {
            stall = 0;
            last_kbot = kb;
        }

        if (nh <= nmin || stall >= 3)
        {
            // Crossover / stalled: finish the whole block with dlahqr.
            schur_small_block<T>(alloc, hd, ld, n, ktop, kb, zd, zld, vectors, ilo, ihi, out.wr,
                                 out.wi);
            kbot = static_cast<crd::isize>(ktop) - 1;
            stall = 0;
            continue;
        }

        // Aggressive Early Deflation on the trailing window.
        crd::usize nw = std::min(nh, std::max<crd::usize>(2, nh / 3));
        if (nw > nh - 1)
        {
            nw = nh - 1;
        }
        const crd::usize kwtop = kb - nw + 1;
        const AedResult<T> aed =
            aed_deflate<T>(alloc, out.t, ktop, kb, nw, out.z, vectors, ilo, ihi, true, out.wr, out.wi);
        kbot = static_cast<crd::isize>(kb) - static_cast<crd::isize>(aed.nd);

        // Nibble: if deflation was productive, skip the sweep and AED again.
        if (aed.nd > 0 && 100 * aed.nd > nibble * nw)
        {
            stall = 0;
            continue;
        }

        // One small-bulge multishift QR sweep (dlaqr5 train) using ALL undeflated
        // AED eigenvalues as shifts — a single BLAS-3 train pass replaces the
        // ns/2 separate single-bulge sweeps (the v3d-1c-4 multishift lever). The
        // undeflated shifts occupy [kwtop, kwtop+ns-1] after aed_deflate.
        const crd::usize ns = aed.ns;
        const crd::isize kbnew = kbot;
        if (ns >= 2 && kbnew >= static_cast<crd::isize>(ktop) + 2)
        {
            const crd::usize sweep_bot = static_cast<crd::usize>(kbnew);
            crd::containers::Array<T> sr(alloc);
            crd::containers::Array<T> si(alloc);
            sr.resize(ns);
            si.resize(ns);
            for (crd::usize i = 0; i < ns; ++i)
            {
                sr[i] = out.wr[kwtop + i];
                si[i] = out.wi[kwtop + i];
            }
            dlaqr5_sweep<T>(alloc, true, vectors, n, ktop, sweep_bot, sr.data(), si.data(), ns, hd, ld,
                            ilo, ihi, zd, zld);
            ++total_sweeps;  // now counts train passes, not shift-pairs
        }
        ++stall;
    }

    out.converged = true;
    if (sweeps != nullptr)
    {
        *sweeps = total_sweeps;
    }
    return out;
}

template void balance<float>(Matrix<float>&, crd::containers::Array<float>&, crd::usize&, crd::usize&);
template void balance<double>(Matrix<double>&, crd::containers::Array<double>&, crd::usize&,
                              crd::usize&);
template void balance<Complex<float>>(Matrix<Complex<float>>&, crd::containers::Array<float>&,
                                      crd::usize&, crd::usize&);
template void balance<Complex<double>>(Matrix<Complex<double>>&, crd::containers::Array<double>&,
                                       crd::usize&, crd::usize&);
template void hessenberg<float>(Matrix<float>&, crd::usize, crd::usize, crd::containers::Array<float>&);
template void hessenberg<double>(Matrix<double>&, crd::usize, crd::usize,
                                 crd::containers::Array<double>&);
template Matrix<float> form_hessenberg_q<float>(crd::memory::IAllocator*, const Matrix<float>&,
                                                crd::usize, crd::usize,
                                                const crd::containers::Array<float>&);
template Matrix<double> form_hessenberg_q<double>(crd::memory::IAllocator*, const Matrix<double>&,
                                                  crd::usize, crd::usize,
                                                  const crd::containers::Array<double>&);
// v3d-2c-1 — complex Hessenberg (zgehd2) + unitary Q (zunghr) instantiations.
template void hessenberg<Complex<float>>(Matrix<Complex<float>>&, crd::usize, crd::usize,
                                         crd::containers::Array<Complex<float>>&);
template void hessenberg<Complex<double>>(Matrix<Complex<double>>&, crd::usize, crd::usize,
                                          crd::containers::Array<Complex<double>>&);
template Matrix<Complex<float>> form_hessenberg_q<Complex<float>>(
    crd::memory::IAllocator*, const Matrix<Complex<float>>&, crd::usize, crd::usize,
    const crd::containers::Array<Complex<float>>&);
template Matrix<Complex<double>> form_hessenberg_q<Complex<double>>(
    crd::memory::IAllocator*, const Matrix<Complex<double>>&, crd::usize, crd::usize,
    const crd::containers::Array<Complex<double>>&);
template RealSchur<float> real_schur<float>(crd::memory::IAllocator*, const Matrix<float>&, crd::usize,
                                            crd::usize, bool);
template RealSchur<double> real_schur<double>(crd::memory::IAllocator*, const Matrix<double>&,
                                              crd::usize, crd::usize, bool);
template bool reorder_schur<float>(Matrix<float>&, Matrix<float>&, crd::usize, crd::usize);
template bool reorder_schur<double>(Matrix<double>&, Matrix<double>&, crd::usize, crd::usize);
template AedResult<float> aed_deflate<float>(crd::memory::IAllocator*, Matrix<float>&, crd::usize,
                                             crd::usize, crd::usize, Matrix<float>&, bool, crd::usize,
                                             crd::usize, bool, crd::containers::Array<float>&,
                                             crd::containers::Array<float>&);
template AedResult<double> aed_deflate<double>(crd::memory::IAllocator*, Matrix<double>&, crd::usize,
                                               crd::usize, crd::usize, Matrix<double>&, bool, crd::usize,
                                               crd::usize, bool, crd::containers::Array<double>&,
                                               crd::containers::Array<double>&);
template RealSchur<float> schur_aed<float>(crd::memory::IAllocator*, const Matrix<float>&, crd::usize,
                                           crd::usize, bool, crd::usize*);
template RealSchur<double> schur_aed<double>(crd::memory::IAllocator*, const Matrix<double>&, crd::usize,
                                             crd::usize, bool, crd::usize*);

// =======================================================================
// v3d-2c-2 — complex single-shift Schur (LAPACK zlahqr). Reduces a complex
// upper-Hessenberg matrix to UPPER-TRIANGULAR Schur form by a unitary
// similarity h_in = Z·T·Zᴴ. No 2×2 blocks (complex eigenvalues on the diagonal)
// ⇒ no dlanv2. Single Wilkinson-shift implicit QR + complex-Givens bulge chase +
// Ahues-Tisseur deflation + modern zlahqr exceptional shifts (KEXSH=10 continuous
// kicks, dat1=0.75; D(non-sym)-6) — consistent with real_schur's dlahqr kdefl path.
// =======================================================================
template <typename T>
ComplexSchur<T> complex_schur(crd::memory::IAllocator* alloc, const Matrix<T>& h_in, crd::usize ilo,
                              crd::usize ihi, bool vectors)
{
    static_assert(is_complex_v<T>, "complex_schur is complex-only (real is real_schur)");
    using R = RealType<T>;
    const crd::usize n = h_in.rows();
    ComplexSchur<T> out(alloc);
    out.t = h_in.clone();
    out.w.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        out.w[k] = T{R{0}, R{0}};
    }
    if (vectors)
    {
        out.z = Matrix<T>(alloc, n, n);
        out.z.set_identity();
    }
    if (n == 0)
    {
        out.converged = true;
        return out;
    }

    T* hd = out.t.data();
    const crd::usize ld = out.t.ld();
    T* zd = vectors ? out.z.data() : nullptr;
    const crd::usize zld = vectors ? out.z.ld() : 0;
    auto h = [&](crd::usize i, crd::usize j) -> T& { return hd[i * ld + j]; };
    auto z = [&](crd::usize i, crd::usize j) -> T& { return zd[i * zld + j]; };
    auto cabs1 = [](const T& v) -> R { return std::abs(v.re) + std::abs(v.im); };

    for (crd::usize k = 0; k < ilo; ++k)
    {
        out.w[k] = h(k, k);
    }
    for (crd::usize k = ihi + 1; k < n; ++k)
    {
        out.w[k] = h(k, k);
    }

    const R ulp = std::numeric_limits<R>::epsilon();
    const R safmin = std::numeric_limits<R>::min();
    const crd::usize nh = ihi - ilo + 1;
    const R smlnum = safmin * (static_cast<R>(nh) / ulp);
    const R dat1 = static_cast<R>(0.75);  // D(non-sym)-6 exceptional-shift constant
    const crd::usize kexsh = 10;          // modern zlahqr KEXSH: kick every 10 iters
    const crd::usize itmax = 30 * std::max<crd::usize>(10, nh);
    const crd::usize iloz = ilo;
    const crd::usize ihiz = ihi;
    const crd::usize i2 = n - 1;  // WANTT ⇒ full T

    crd::isize i_s = static_cast<crd::isize>(ihi);
    while (i_s >= static_cast<crd::isize>(ilo))
    {
        const crd::usize i = static_cast<crd::usize>(i_s);
        crd::usize l = ilo;
        bool conv = false;
        for (crd::usize its = 0; its <= itmax; ++its)
        {
            // Find the largest l so that h(l,l-1) is negligible (Ahues-Tisseur).
            crd::usize k = l;
            for (crd::usize kk = i; kk > l; --kk)
            {
                if (cabs1(h(kk, kk - 1)) <= smlnum)
                {
                    k = kk;
                    break;
                }
                R tst = cabs1(h(kk - 1, kk - 1)) + cabs1(h(kk, kk));
                if (tst == R{0})
                {
                    if (kk >= ilo + 2)
                    {
                        tst += std::abs(h(kk - 1, kk - 2).re);
                    }
                    if (kk + 1 <= ihi)
                    {
                        tst += std::abs(h(kk + 1, kk).re);
                    }
                }
                if (cabs1(h(kk, kk - 1)) <= ulp * tst)
                {
                    const R ab = std::max(cabs1(h(kk, kk - 1)), cabs1(h(kk - 1, kk)));
                    const R ba = std::min(cabs1(h(kk, kk - 1)), cabs1(h(kk - 1, kk)));
                    const R aa = std::max(cabs1(h(kk, kk)), cabs1(h(kk - 1, kk - 1) - h(kk, kk)));
                    const R bb = std::min(cabs1(h(kk, kk)), cabs1(h(kk - 1, kk - 1) - h(kk, kk)));
                    const R s = aa + ab;
                    if (ba * (ab / s) <= std::max(smlnum, ulp * (bb * (aa / s))))
                    {
                        k = kk;
                        break;
                    }
                }
            }
            l = k;
            if (l > ilo)
            {
                h(l, l - 1) = T{R{0}, R{0}};
            }
            if (l == i)
            {
                conv = true;
                break;
            }

            // ---- shift ----
            // Exceptional shift schedule = modern zlahqr (KEXSH=10): kick every 10
            // iterations, alternating bottom (kdefl%2*KEXSH) / top (kdefl%KEXSH),
            // scalar T = s + diag, s = dat1·|Re(subdiag)|. `its` (per-eigenvalue,
            // reset on deflation) plays kdefl here; convergent blocks never reach
            // its=10 so the Wilkinson path is unchanged for all converging spectra.
            T t_shift;
            if (its > 0 && its % (2 * kexsh) == 0)
            {
                const R s = dat1 * std::abs(h(i, i - 1).re);
                t_shift = h(i, i) + T{s, R{0}};
            }
            else if (its > 0 && its % kexsh == 0)
            {
                const R s = dat1 * std::abs(h(l + 1, l).re);
                t_shift = h(l, l) + T{s, R{0}};
            }
            else
            {
                // Wilkinson shift (faithful zlahqr): U = sqrt(h(i-1,i))·sqrt(h(i,i-1)),
                // Y = S·sqrt((X/S)²+(U/S)²), T = h(i,i) − (U/(X+Y))·U. Scaled by S.
                t_shift = h(i, i);
                const T u = crd::hesap::sqrt(h(i - 1, i)) * crd::hesap::sqrt(h(i, i - 1));
                R s = cabs1(u);
                if (s != R{0})
                {
                    const T x = (h(i - 1, i - 1) - h(i, i)) * static_cast<R>(0.5);
                    const R sx = cabs1(x);
                    s = std::max(s, sx);
                    const T xs = x * (R{1} / s);
                    const T us = u * (R{1} / s);
                    T y = crd::hesap::sqrt(xs * xs + us * us) * s;
                    if (sx > R{0})
                    {
                        const T xsx = x * (R{1} / sx);
                        if (xsx.re * y.re + xsx.im * y.im < R{0})
                        {
                            y = -y;
                        }
                    }
                    t_shift = h(i, i) - (u / (x + y)) * u;
                }
            }

            // ---- single-shift QR sweep: chase one bulge from l to i ----
            for (crd::usize m = l; m < i; ++m)
            {
                T v0;
                T v1;
                if (m == l)
                {
                    v0 = h(l, l) - t_shift;
                    v1 = h(l + 1, l);
                }
                else
                {
                    v0 = h(m, m - 1);
                    v1 = h(m + 1, m - 1);
                }
                const auto g = detail::complex_givens<R>(v0, v1);
                const R c = g.c;
                const T s = g.s;
                if (m > l)
                {
                    h(m, m - 1) = g.r;
                    h(m + 1, m - 1) = T{R{0}, R{0}};
                }
                // Left: rows (m, m+1) over columns [m, i2].
                for (crd::usize j = m; j <= i2; ++j)
                {
                    const T tmp = h(m, j) * c + h(m + 1, j) * s;
                    h(m + 1, j) = h(m + 1, j) * c - h(m, j) * crd::hesap::conj(s);
                    h(m, j) = tmp;
                }
                // Right: columns (m, m+1) over rows [0, min(m+2, i)] (incl. the bulge).
                const crd::usize jhi = std::min(m + 2, i);
                for (crd::usize j = 0; j <= jhi; ++j)
                {
                    const T tmp = h(j, m) * c + h(j, m + 1) * crd::hesap::conj(s);
                    h(j, m + 1) = h(j, m + 1) * c - h(j, m) * s;
                    h(j, m) = tmp;
                }
                // Z := Z·Gᴴ over rows [iloz, ihiz].
                if (vectors)
                {
                    for (crd::usize j = iloz; j <= ihiz; ++j)
                    {
                        const T tmp = z(j, m) * c + z(j, m + 1) * crd::hesap::conj(s);
                        z(j, m + 1) = z(j, m + 1) * c - z(j, m) * s;
                        z(j, m) = tmp;
                    }
                }
            }
        }
        if (!conv)
        {
            out.converged = false;
            return out;
        }
        out.w[i] = h(i, i);
        i_s = static_cast<crd::isize>(l) - 1;
    }
    out.converged = true;
    return out;
}

template ComplexSchur<Complex<float>> complex_schur<Complex<float>>(crd::memory::IAllocator*,
                                                                    const Matrix<Complex<float>>&,
                                                                    crd::usize, crd::usize, bool);
template ComplexSchur<Complex<double>> complex_schur<Complex<double>>(crd::memory::IAllocator*,
                                                                      const Matrix<Complex<double>>&,
                                                                      crd::usize, crd::usize, bool);

// =======================================================================
// v3d-2c-2b-1 — reorder_complex_schur (LAPACK ztrexc). Move the diagonal
// eigenvalue at `ifst` to `ilst` by adjacent 1×1 swaps. Each swap of positions
// (p, p+1) of the upper-triangular T is a unitary similarity G·T·Gᴴ where G is
// the complex Givens (zlartg) from (t(p,p+1), t(p+1,p+1)−t(p,p)). Applying the
// rotation over the full block region ([p,n-1] left, [0,p+1] right) then forcing
// the (p+1,p) entry to 0 is a provably-unitary swap with the diagonal exchanged
// — the same Givens application form as `complex_schur`'s bulge chase.
// =======================================================================
template <typename T>
bool reorder_complex_schur(Matrix<T>& t, Matrix<T>& z, crd::usize ifst, crd::usize ilst)
{
    static_assert(is_complex_v<T>, "reorder_complex_schur is complex-only (real is reorder_schur)");
    using R = RealType<T>;
    const crd::usize n = t.rows();
    CRD_ASSERT_MSG(z.rows() == n && z.cols() == n, "reorder_complex_schur: Z must be n×n");
    if (n <= 1 || ifst == ilst)
    {
        return true;
    }

    T* td = t.data();
    const crd::usize ld = t.ld();
    T* zd = z.data();
    const crd::usize zld = z.ld();
    auto tat = [&](crd::usize i, crd::usize j) -> T& { return td[i * ld + j]; };
    auto zat = [&](crd::usize i, crd::usize j) -> T& { return zd[i * zld + j]; };

    // Swap the adjacent diagonal eigenvalues at positions (p, p+1).
    auto swap_adjacent = [&](crd::usize p)
    {
        const T t11 = tat(p, p);
        const T t22 = tat(p + 1, p + 1);
        const auto g = detail::complex_givens<R>(tat(p, p + 1), t22 - t11);
        const R c = g.c;
        const T s = g.s;
        // Left: G·T over rows (p, p+1), columns [p, n-1] (cols < p are zero in
        // both rows for upper-triangular T).
        for (crd::usize j = p; j < n; ++j)
        {
            const T tmp = tat(p, j) * c + tat(p + 1, j) * s;
            tat(p + 1, j) = tat(p + 1, j) * c - tat(p, j) * crd::hesap::conj(s);
            tat(p, j) = tmp;
        }
        // Right: T·Gᴴ over columns (p, p+1), rows [0, p+1] (rows > p+1 are zero
        // in both columns). Reads the just-left-updated block — correct G·B·Gᴴ.
        for (crd::usize i = 0; i <= p + 1; ++i)
        {
            const T tmp = tat(i, p) * c + tat(i, p + 1) * crd::hesap::conj(s);
            tat(i, p + 1) = tat(i, p + 1) * c - tat(i, p) * s;
            tat(i, p) = tmp;
        }
        tat(p + 1, p) = T{R{0}, R{0}};  // kill the rotated subdiagonal roundoff
        // Z := Z·Gᴴ over columns (p, p+1), all rows.
        for (crd::usize i = 0; i < n; ++i)
        {
            const T tmp = zat(i, p) * c + zat(i, p + 1) * crd::hesap::conj(s);
            zat(i, p + 1) = zat(i, p + 1) * c - zat(i, p) * s;
            zat(i, p) = tmp;
        }
    };

    if (ifst < ilst)
    {
        for (crd::usize here = ifst; here < ilst; ++here)
        {
            swap_adjacent(here);  // moves the eigenvalue down to here+1
        }
    }
    else  // ifst > ilst — move up; cursor avoids the usize underflow at ilst==0.
    {
        crd::usize here = ifst;
        while (here > ilst)
        {
            swap_adjacent(here - 1);  // moves the eigenvalue up to here-1
            --here;
        }
    }
    return true;
}

template bool reorder_complex_schur<Complex<float>>(Matrix<Complex<float>>&, Matrix<Complex<float>>&,
                                                    crd::usize, crd::usize);
template bool reorder_complex_schur<Complex<double>>(Matrix<Complex<double>>&,
                                                     Matrix<Complex<double>>&, crd::usize, crd::usize);

namespace
{
// Complex reflector applies for the v3d-2c-2b-2 spike reduction. H = I − tau·v·vᴴ
// (v[0]=1 implicit, supplied explicitly here), faithful to zlarf with the zlaqr2
// scalar convention: LEFT uses conj(tau) (Hᴴ·C), RIGHT uses tau (C·H).

// C[r0:r0+ku, c0:c0+ncol] := (I − tauL·v·vᴴ)·C   (vᴴ·C uses conj(v)).
template <typename T>
void apply_hc_left(T* d, crd::usize ld, crd::usize r0, crd::usize c0, crd::usize ku, crd::usize ncol,
                   const T* v, T tauL)
{
    using R = RealType<T>;
    if (tauL.re == R{0} && tauL.im == R{0})
    {
        return;
    }
    for (crd::usize j = 0; j < ncol; ++j)
    {
        T s{R{0}, R{0}};
        for (crd::usize i = 0; i < ku; ++i)
        {
            s = s + crd::hesap::conj(v[i]) * d[(r0 + i) * ld + (c0 + j)];
        }
        s = tauL * s;
        for (crd::usize i = 0; i < ku; ++i)
        {
            d[(r0 + i) * ld + (c0 + j)] = d[(r0 + i) * ld + (c0 + j)] - v[i] * s;
        }
    }
}

// C[r0:r0+nrow, c0:c0+ku] := C·(I − tauR·v·vᴴ)   (C·v: no conj; outer uses conj(v)).
template <typename T>
void apply_hc_right(T* d, crd::usize ld, crd::usize r0, crd::usize c0, crd::usize nrow, crd::usize ku,
                    const T* v, T tauR)
{
    using R = RealType<T>;
    if (tauR.re == R{0} && tauR.im == R{0})
    {
        return;
    }
    for (crd::usize i = 0; i < nrow; ++i)
    {
        T s{R{0}, R{0}};
        for (crd::usize j = 0; j < ku; ++j)
        {
            s = s + d[(r0 + i) * ld + (c0 + j)] * v[j];
        }
        s = tauR * s;
        for (crd::usize j = 0; j < ku; ++j)
        {
            d[(r0 + i) * ld + (c0 + j)] = d[(r0 + i) * ld + (c0 + j)] - s * crd::hesap::conj(v[j]);
        }
    }
}

// C(jw×cols) := Vᴴ·C  (complex sibling of slab_left_t; gemm ConjTranspose).
template <typename T>
void slab_left_h(T* dat, crd::usize ld, crd::usize r0, crd::usize c0, crd::usize jw, crd::usize cols,
                 const T* v, crd::usize ldv, Matrix<T>& scratch, crd::memory::IAllocator* alloc)
{
    if (cols == 0)
    {
        return;
    }
    constexpr Layout k_l = Layout::RowMajor;
    MatrixView<const T, k_l> vv{v, jw, jw, ldv};
    MatrixView<const T, k_l> cv{dat + r0 * ld + c0, jw, cols, ld};
    MatrixView<T, k_l> ov{scratch.data(), jw, cols, scratch.ld()};
    gemm<T, k_l>(T{RealType<T>{1}, RealType<T>{0}}, vv, cv, T{RealType<T>{0}, RealType<T>{0}}, ov,
                Trans::ConjTranspose, Trans::None, alloc);
    for (crd::usize i = 0; i < jw; ++i)
    {
        for (crd::usize j = 0; j < cols; ++j)
        {
            dat[(r0 + i) * ld + (c0 + j)] = scratch.at(i, j);
        }
    }
}
} // namespace

template <typename T>
AedResult<T> complex_aed_deflate(crd::memory::IAllocator* alloc, Matrix<T>& h, crd::usize ktop,
                                 crd::usize kbot, crd::usize nw, Matrix<T>& z, bool wantz,
                                 crd::usize iloz, crd::usize ihiz, bool wantt,
                                 crd::containers::Array<T>& w)
{
    static_assert(is_complex_v<T>, "complex_aed_deflate is complex-only (real is aed_deflate)");
    using R = RealType<T>;
    const crd::usize n = h.rows();
    AedResult<T> res{};
    if (w.size() != n)
    {
        w.resize(n);
    }
    if (ktop > kbot)
    {
        return res;
    }

    T* hd = h.data();
    const crd::usize ld = h.ld();
    auto hh = [&](crd::usize i, crd::usize j) -> T& { return hd[i * ld + j]; };
    auto cabs1 = [](const T& x) -> R { return std::abs(x.re) + std::abs(x.im); };
    const T czero{R{0}, R{0}};

    const R eps = std::numeric_limits<R>::epsilon();
    const R safmin = std::numeric_limits<R>::min();
    const R ulp = eps;
    const R smlnum = safmin * (static_cast<R>(n) / ulp);

    const crd::usize jw = std::min(nw, kbot - ktop + 1);
    const crd::usize kwtop = kbot - jw + 1;
    T s = (kwtop == ktop) ? czero : hh(kwtop, kwtop - 1);

    if (kbot == kwtop)  // 1×1 window
    {
        w[kwtop] = hh(kwtop, kwtop);
        res.ns = 1;
        res.nd = 0;
        if (cabs1(s) <= std::max(smlnum, ulp * cabs1(hh(kwtop, kwtop))))
        {
            res.ns = 0;
            res.nd = 1;
            if (kwtop > ktop)
            {
                hh(kwtop, kwtop - 1) = czero;
            }
        }
        return res;
    }

    // Window Hessenberg → complex (upper-triangular) Schur: T = twin, V = v.
    Matrix<T> win(alloc, jw, jw);
    for (crd::usize i = 0; i < jw; ++i)
    {
        for (crd::usize j = 0; j < jw; ++j)
        {
            win.at(i, j) = (j + 1 >= i) ? hh(kwtop + i, kwtop + j) : czero;
        }
    }
    ComplexSchur<T> sch = complex_schur<T>(alloc, win, 0, jw - 1, true);
    Matrix<T>& twin = sch.t;
    Matrix<T>& v = sch.z;
    const crd::usize infqr = 0;  // window is small; complex_schur converges
    auto t1 = [&](crd::usize i, crd::usize j) -> T& { return twin.at(i - 1, j - 1); };
    auto v1 = [&](crd::usize i, crd::usize j) -> T& { return v.at(i - 1, j - 1); };

    // Deflation detection loop (1-based ns/ilst). No bulge branch: every diagonal
    // entry is a 1×1 eigenvalue (complex Schur form has no 2×2 blocks).
    crd::usize ns = jw;
    crd::usize ilst = infqr + 1;
    while (ilst <= ns)
    {
        R foo = cabs1(t1(ns, ns));
        if (foo == R{0})
        {
            foo = cabs1(s);
        }
        if (cabs1(s) * cabs1(v1(1, ns)) <= std::max(smlnum, ulp * foo))
        {
            --ns;  // deflatable
        }
        else
        {
            reorder_complex_schur<T>(twin, v, ns - 1, ilst - 1);  // move up out of the way
            ++ilst;
        }
    }
    if (ns == 0)
    {
        s = czero;
    }

    // Restore eigenvalues from the window Schur diagonal into w[kwtop..kbot].
    for (crd::usize i = 0; i < jw; ++i)
    {
        w[kwtop + i] = twin.at(i, i);
    }

    if (ns < jw || (s.re == R{0} && s.im == R{0}))
    {
        const crd::usize ldt = twin.ld();
        const crd::usize ldv = v.ld();
        if (ns > 1 && !(s.re == R{0} && s.im == R{0}))
        {
            // Reflect the spike (first row of V over the NS undeflated columns).
            crd::containers::Array<T> work(alloc);
            work.resize(jw);
            for (crd::usize k = 0; k < ns; ++k)
            {
                work[k] = crd::hesap::conj(v.at(0, k));  // zlaqr2 conjugates the spike row
            }
            const auto refl = detail::make_householder_complex<R>(work.data(), ns);
            work[0] = T{R{1}, R{0}};
            // Zero the strict lower-by-2 of T (cleared by the spike reflection).
            for (crd::usize i = 2; i < jw; ++i)
            {
                for (crd::usize j = 0; j + 2 <= i && j < jw; ++j)
                {
                    twin.at(i, j) = czero;
                }
            }
            apply_hc_left<T>(twin.data(), ldt, 0, 0, ns, jw, work.data(), crd::hesap::conj(refl.tau));
            apply_hc_right<T>(twin.data(), ldt, 0, 0, ns, ns, work.data(), refl.tau);
            apply_hc_right<T>(v.data(), ldv, 0, 0, jw, ns, work.data(), refl.tau);
            // Re-Hessenbergize the leading NS block, accumulate Q into V.
            crd::containers::Array<T> tauh(alloc);
            hessenberg<T>(twin, 0, ns - 1, tauh);
            Matrix<T> qh = form_hessenberg_q<T>(alloc, twin, 0, ns - 1, tauh);
            Matrix<T> vq(alloc, jw, jw);
            {
                constexpr Layout k_l = Layout::RowMajor;
                MatrixView<const T, k_l> vv{v.data(), jw, jw, ldv};
                MatrixView<const T, k_l> qq{qh.data(), jw, jw, qh.ld()};
                MatrixView<T, k_l> ovq{vq.data(), jw, jw, vq.ld()};
                gemm<T, k_l>(T{R{1}, R{0}}, vv, qq, T{R{0}, R{0}}, ovq, Trans::None, Trans::None, alloc);
            }
            for (crd::usize i = 0; i < jw; ++i)
            {
                for (crd::usize j = 0; j < jw; ++j)
                {
                    v.at(i, j) = vq.at(i, j);
                }
            }
        }

        // Copy the updated reduced window back into H (Hessenberg part only).
        // zlaqr2 conjugates here (the real path's V(1,1) is real): the coupling
        // above the window must survive the unitary similarity.
        if (kwtop > 0)
        {
            hh(kwtop, kwtop - 1) = s * crd::hesap::conj(v.at(0, 0));
        }
        for (crd::usize i = 0; i < jw; ++i)
        {
            for (crd::usize j = (i == 0 ? 0 : i - 1); j < jw; ++j)
            {
                hh(kwtop + i, kwtop + j) = twin.at(i, j);
            }
        }

        // Global similarity updates with V (scratch n×n covers both slab shapes).
        // H := Vᴴ·H·V over the window region: right multiply C·V (slab_right), left
        // multiply Vᴴ·C (slab_left_h — ConjTranspose, the complex divergence).
        Matrix<T> scratch(alloc, n, n);
        const crd::usize ltop = wantt ? 0 : ktop;
        slab_right<T>(hd, ld, ltop, kwtop, kwtop - ltop, jw, v.data(), v.ld(), scratch, alloc);
        if (wantt && kbot + 1 < n)
        {
            slab_left_h<T>(hd, ld, kwtop, kbot + 1, jw, n - (kbot + 1), v.data(), v.ld(), scratch,
                           alloc);
        }
        if (wantz)
        {
            slab_right<T>(z.data(), z.ld(), iloz, kwtop, ihiz - iloz + 1, jw, v.data(), v.ld(),
                          scratch, alloc);
        }
    }

    res.nd = jw - ns;
    res.ns = ns - infqr;
    return res;
}

template AedResult<Complex<float>> complex_aed_deflate<Complex<float>>(
    crd::memory::IAllocator*, Matrix<Complex<float>>&, crd::usize, crd::usize, crd::usize,
    Matrix<Complex<float>>&, bool, crd::usize, crd::usize, bool, crd::containers::Array<Complex<float>>&);
template AedResult<Complex<double>> complex_aed_deflate<Complex<double>>(
    crd::memory::IAllocator*, Matrix<Complex<double>>&, crd::usize, crd::usize, crd::usize,
    Matrix<Complex<double>>&, bool, crd::usize, crd::usize, bool,
    crd::containers::Array<Complex<double>>&);

namespace
{
// v3d-2c-2b-3 — complex zlaqr1: first column of (H − s1·I)(H − s2·I) for a
// 2×2 or 3×3 leading block, the shift-polynomial seed for a multishift bulge.
// Full complex shifts s1/s2 (no real-arithmetic si1·si2 term). `hsub` 0-based.
template <typename T>
void complex_dlaqr1(crd::usize na, const T* hsub, crd::usize ld, T s1, T s2, T* v)
{
    using R = RealType<T>;
    auto hh = [&](crd::usize i, crd::usize j) -> T { return hsub[i * ld + j]; };
    auto cabs1 = [](const T& x) -> R { return std::abs(x.re) + std::abs(x.im); };
    if (na == 2)
    {
        const R s = cabs1(hh(0, 0) - s2) + cabs1(hh(1, 0));
        if (s == R{0})
        {
            v[0] = T{R{0}, R{0}};
            v[1] = T{R{0}, R{0}};
        }
        else
        {
            const T h21s = hh(1, 0) * (R{1} / s);
            v[0] = h21s * hh(0, 1) + (hh(0, 0) - s1) * ((hh(0, 0) - s2) * (R{1} / s));
            v[1] = h21s * (hh(0, 0) + hh(1, 1) - s1 - s2);
        }
    }
    else
    {
        const R s = cabs1(hh(0, 0) - s2) + cabs1(hh(1, 0)) + cabs1(hh(2, 0));
        if (s == R{0})
        {
            v[0] = T{R{0}, R{0}};
            v[1] = T{R{0}, R{0}};
            v[2] = T{R{0}, R{0}};
        }
        else
        {
            const T h21s = hh(1, 0) * (R{1} / s);
            const T h31s = hh(2, 0) * (R{1} / s);
            v[0] = (hh(0, 0) - s1) * ((hh(0, 0) - s2) * (R{1} / s)) + hh(0, 1) * h21s + hh(0, 2) * h31s;
            v[1] = h21s * (hh(0, 0) + hh(1, 1) - s1 - s2) + hh(1, 2) * h31s;
            v[2] = h31s * (hh(0, 0) + hh(2, 2) - s1 - s2) + h21s * hh(2, 1);
        }
    }
}

// v3d-2c-2b-3 — complex zlaqr5: one small-bulge multishift QR sweep on [ktop,
// kbot] of the global complex Hessenberg `hd` (n×n, row-major). Faithful port of
// the real `dlaqr5_sweep`; the divergences are the complex reflector applies
// (zlaqr5 convention: RIGHT uses T={tau, tau·conj(v2), tau·conj(v3)} + plain-v
// gather; LEFT uses T={conj(tau), conj(tau)·v2, conj(tau)·v3} + conj(v) gather;
// the similarity is Hᴴ·A·H), `complex_dlaqr1`/`make_householder_complex` for the
// reflectors, `cabs1` deflation tests, NO conjugate-pair shift shuffle, and the
// far-update horizontal slab is Vᴴ (`slab_left_h`). KACC22=1.
template <typename T>
void complex_dlaqr5_sweep(crd::memory::IAllocator* alloc, bool wantt, bool wantz, crd::usize n,
                          crd::usize ktop0, crd::usize kbot0, const T* shifts_in, crd::usize nshifts,
                          T* hd, crd::usize ld, crd::usize iloz0, crd::usize ihiz0, T* zd, crd::usize zld)
{
    using R = RealType<T>;
    if (nshifts < 2 || ktop0 >= kbot0)
    {
        return;
    }
    auto h = [&](crd::isize i, crd::isize j) -> T& { return hd[(i - 1) * ld + (j - 1)]; };
    // Z far-update uses slab_right(zd, ...) directly (raw pointer); no z() accessor.
    auto cabs1 = [](const T& x) -> R { return std::abs(x.re) + std::abs(x.im); };
    const T czero{R{0}, R{0}};
    auto iszero = [&](const T& x) -> bool { return x.re == R{0} && x.im == R{0}; };
    const crd::isize ktop = static_cast<crd::isize>(ktop0) + 1;
    const crd::isize kbot = static_cast<crd::isize>(kbot0) + 1;
    const crd::isize iloz = static_cast<crd::isize>(iloz0) + 1;
    const crd::isize ihiz = static_cast<crd::isize>(ihiz0) + 1;
    const crd::isize nn = static_cast<crd::isize>(n);

    // No conjugate-pair shuffle (complex shifts used in pairs as-is).
    crd::containers::Array<T> shf(alloc);
    shf.resize(nshifts);
    for (crd::usize k = 0; k < nshifts; ++k)
    {
        shf[k] = shifts_in[k];
    }
    const crd::isize ns = static_cast<crd::isize>(nshifts - (nshifts % 2));
    auto shift = [&](crd::isize m) -> T { return shf[static_cast<crd::usize>(m - 1)]; };  // 1-based

    const R eps = std::numeric_limits<R>::epsilon();
    const R safmin = std::numeric_limits<R>::min();
    const R ulp = eps;
    const R smlnum = safmin * (static_cast<R>(nn) / ulp);

    if (ktop + 2 <= kbot)
    {
        h(ktop + 2, ktop) = czero;
    }

    const crd::isize nbmps = ns / 2;
    const crd::isize kdu = 4 * nbmps;

    crd::containers::Array<T> vws(alloc);
    vws.resize(static_cast<crd::usize>(3 * nbmps));
    auto vref = [&](crd::isize row, crd::isize m) -> T& {
        return vws[static_cast<crd::usize>(3 * (m - 1) + (row - 1))];
    };
    T vt[3] = {czero, czero, czero};

    Matrix<T> umat(alloc, static_cast<crd::usize>(kdu), static_cast<crd::usize>(kdu));
    Matrix<T> scratch(alloc, n, n);
    auto u = [&](crd::isize i, crd::isize j) -> T& {
        return umat.data()[static_cast<crd::usize>(i - 1) * umat.ld() + static_cast<crd::usize>(j - 1)];
    };

    for (crd::isize incol = ktop - 2 * nbmps + 1; incol <= kbot - 2; incol += 2 * nbmps)
    {
        const crd::isize jtop = std::max(ktop, incol);
        const crd::isize ndcol = incol + kdu;
        for (crd::isize a = 1; a <= kdu; ++a)
        {
            for (crd::isize b = 1; b <= kdu; ++b)
            {
                u(a, b) = (a == b) ? T{R{1}, R{0}} : czero;
            }
        }

        const crd::isize krcol_hi = std::min(incol + 2 * nbmps - 1, kbot - 2);
        for (crd::isize krcol = incol; krcol <= krcol_hi; ++krcol)
        {
            const crd::isize mtop = std::max<crd::isize>(1, (ktop - krcol) / 2 + 1);
            const crd::isize mbot = std::min<crd::isize>(nbmps, (kbot - krcol - 1) / 2);
            const crd::isize m22 = mbot + 1;
            const bool bmp22 = (mbot < nbmps) && (krcol + 2 * (m22 - 1) == kbot - 2);

            // ---- Special 2×2 bulge at the bottom ----
            if (bmp22)
            {
                const crd::isize k = krcol + 2 * (m22 - 1);
                if (k == ktop - 1)
                {
                    complex_dlaqr1<T>(2, &h(k + 1, k + 1), ld, shift(2 * m22 - 1), shift(2 * m22),
                                      &vref(1, m22));
                    T vv[2] = {vref(1, m22), vref(2, m22)};
                    const auto hh = detail::make_householder_complex<R>(vv, 2);
                    vref(1, m22) = hh.tau;
                    vref(2, m22) = vv[1];
                }
                else
                {
                    T vv[2] = {h(k + 1, k), h(k + 2, k)};
                    const auto hh = detail::make_householder_complex<R>(vv, 2);
                    vref(1, m22) = hh.tau;
                    vref(2, m22) = vv[1];
                    h(k + 1, k) = T{hh.beta, R{0}};
                    h(k + 2, k) = czero;
                }
                const T tau = vref(1, m22);
                const T v2 = vref(2, m22);
                const T t1r = tau;
                const T t2r = tau * crd::hesap::conj(v2);
                const T t1l = crd::hesap::conj(tau);
                const T t2l = crd::hesap::conj(tau) * v2;
                for (crd::isize j = jtop; j <= std::min(kbot, k + 3); ++j)  // RIGHT
                {
                    const T refsum = h(j, k + 1) + v2 * h(j, k + 2);
                    h(j, k + 1) = h(j, k + 1) - refsum * t1r;
                    h(j, k + 2) = h(j, k + 2) - refsum * t2r;
                }
                const crd::isize jbot_c = std::min(ndcol, kbot);
                for (crd::isize j = k + 1; j <= jbot_c; ++j)  // LEFT
                {
                    const T refsum = h(k + 1, j) + crd::hesap::conj(v2) * h(k + 2, j);
                    h(k + 1, j) = h(k + 1, j) - refsum * t1l;
                    h(k + 2, j) = h(k + 2, j) - refsum * t2l;
                }
                if (k >= ktop && !iszero(h(k + 1, k)))
                {
                    R tst1 = cabs1(h(k, k)) + cabs1(h(k + 1, k + 1));
                    if (tst1 == R{0})
                    {
                        if (k >= ktop + 1) tst1 += cabs1(h(k, k - 1));
                        if (k >= ktop + 2) tst1 += cabs1(h(k, k - 2));
                        if (k >= ktop + 3) tst1 += cabs1(h(k, k - 3));
                        if (k <= kbot - 2) tst1 += cabs1(h(k + 2, k + 1));
                        if (k <= kbot - 3) tst1 += cabs1(h(k + 3, k + 1));
                        if (k <= kbot - 4) tst1 += cabs1(h(k + 4, k + 1));
                    }
                    if (cabs1(h(k + 1, k)) <= std::max(smlnum, ulp * tst1))
                    {
                        const R h12 = std::max(cabs1(h(k + 1, k)), cabs1(h(k, k + 1)));
                        const R h21 = std::min(cabs1(h(k + 1, k)), cabs1(h(k, k + 1)));
                        const R h11 = std::max(cabs1(h(k + 1, k + 1)), cabs1(h(k, k) - h(k + 1, k + 1)));
                        const R h22 = std::min(cabs1(h(k + 1, k + 1)), cabs1(h(k, k) - h(k + 1, k + 1)));
                        const R scl = h11 + h12;
                        const R tst2 = h22 * (h11 / scl);
                        if (tst2 == R{0} || h21 * (h12 / scl) <= std::max(smlnum, ulp * tst2))
                        {
                            h(k + 1, k) = czero;
                        }
                    }
                }
                const crd::isize kms = k - incol;  // U accumulate (RIGHT form)
                for (crd::isize j = std::max<crd::isize>(1, ktop - incol); j <= kdu; ++j)
                {
                    const T refsum = u(j, kms + 1) + v2 * u(j, kms + 2);
                    u(j, kms + 1) = u(j, kms + 1) - refsum * t1r;
                    u(j, kms + 2) = u(j, kms + 2) - refsum * t2r;
                }
            }

            // ---- Normal case: chain of 3×3 reflections (m = mbot..mtop) ----
            for (crd::isize m = mbot; m >= mtop; --m)
            {
                const crd::isize k = krcol + 2 * (m - 1);
                if (k == ktop - 1)
                {
                    complex_dlaqr1<T>(3, &h(ktop, ktop), ld, shift(2 * m - 1), shift(2 * m), &vref(1, m));
                    T vv[3] = {vref(1, m), vref(2, m), vref(3, m)};
                    const auto hh = detail::make_householder_complex<R>(vv, 3);
                    vref(1, m) = hh.tau;
                    vref(2, m) = vv[1];
                    vref(3, m) = vv[2];
                }
                else
                {
                    // Delayed transformation of the row below the m-th bulge (RIGHT form,
                    // OLD reflector m from the previous krcol step).
                    const T t1d = vref(1, m);
                    const T t2d = t1d * crd::hesap::conj(vref(2, m));
                    const T t3d = t1d * crd::hesap::conj(vref(3, m));
                    const T refsum0 = vref(3, m) * h(k + 3, k + 2);
                    h(k + 3, k) = czero - refsum0 * t1d;
                    h(k + 3, k + 1) = czero - refsum0 * t2d;
                    h(k + 3, k + 2) = h(k + 3, k + 2) - refsum0 * t3d;
                    // Reflection to move the m-th bulge one step.
                    T vv[3] = {h(k + 1, k), h(k + 2, k), h(k + 3, k)};
                    const auto hh = detail::make_householder_complex<R>(vv, 3);
                    if (!iszero(h(k + 3, k)) || !iszero(h(k + 3, k + 1)) || iszero(h(k + 3, k + 2)))
                    {
                        vref(1, m) = hh.tau;
                        vref(2, m) = vv[1];
                        vref(3, m) = vv[2];
                        h(k + 1, k) = T{hh.beta, R{0}};
                        h(k + 2, k) = czero;
                        h(k + 3, k) = czero;
                    }
                    else
                    {
                        // Bulge collapsed: try to reintroduce ignoring H(k+1,k),H(k+2,k).
                        complex_dlaqr1<T>(3, &h(k + 1, k + 1), ld, shift(2 * m - 1), shift(2 * m), vt);
                        T vvt[3] = {vt[0], vt[1], vt[2]};
                        const auto hh2 = detail::make_householder_complex<R>(vvt, 3);
                        const T refsum = crd::hesap::conj(hh2.tau) *
                                         (h(k + 1, k) + crd::hesap::conj(vvt[1]) * h(k + 2, k));
                        if (cabs1(h(k + 2, k) - refsum * vvt[1]) + cabs1(refsum * vvt[2]) >
                            ulp * (cabs1(h(k, k)) + cabs1(h(k + 1, k + 1)) + cabs1(h(k + 2, k + 2))))
                        {
                            vref(1, m) = hh.tau;
                            vref(2, m) = vv[1];
                            vref(3, m) = vv[2];
                            h(k + 1, k) = T{hh.beta, R{0}};
                            h(k + 2, k) = czero;
                            h(k + 3, k) = czero;
                        }
                        else
                        {
                            h(k + 1, k) = h(k + 1, k) - refsum;
                            h(k + 2, k) = czero;
                            h(k + 3, k) = czero;
                            vref(1, m) = hh2.tau;
                            vref(2, m) = vvt[1];
                            vref(3, m) = vvt[2];
                        }
                    }
                }
                // Apply from right + first column from left.
                const T tau = vref(1, m);
                const T v2 = vref(2, m);
                const T v3 = vref(3, m);
                const T t1r = tau;
                const T t2r = tau * crd::hesap::conj(v2);
                const T t3r = tau * crd::hesap::conj(v3);
                for (crd::isize j = jtop; j <= std::min(kbot, k + 3); ++j)  // RIGHT
                {
                    const T refsum = h(j, k + 1) + v2 * h(j, k + 2) + v3 * h(j, k + 3);
                    h(j, k + 1) = h(j, k + 1) - refsum * t1r;
                    h(j, k + 2) = h(j, k + 2) - refsum * t2r;
                    h(j, k + 3) = h(j, k + 3) - refsum * t3r;
                }
                {
                    // First column from left.
                    const T t1l = crd::hesap::conj(tau);
                    const T t2l = t1l * v2;
                    const T t3l = t1l * v3;
                    const T refsum = h(k + 1, k + 1) + crd::hesap::conj(v2) * h(k + 2, k + 1) +
                                     crd::hesap::conj(v3) * h(k + 3, k + 1);
                    h(k + 1, k + 1) = h(k + 1, k + 1) - refsum * t1l;
                    h(k + 2, k + 1) = h(k + 2, k + 1) - refsum * t2l;
                    h(k + 3, k + 1) = h(k + 3, k + 1) - refsum * t3l;
                }
                if (k < ktop)
                {
                    continue;
                }
                if (!iszero(h(k + 1, k)))
                {
                    R tst1 = cabs1(h(k, k)) + cabs1(h(k + 1, k + 1));
                    if (tst1 == R{0})
                    {
                        if (k >= ktop + 1) tst1 += cabs1(h(k, k - 1));
                        if (k >= ktop + 2) tst1 += cabs1(h(k, k - 2));
                        if (k >= ktop + 3) tst1 += cabs1(h(k, k - 3));
                        if (k <= kbot - 2) tst1 += cabs1(h(k + 2, k + 1));
                        if (k <= kbot - 3) tst1 += cabs1(h(k + 3, k + 1));
                        if (k <= kbot - 4) tst1 += cabs1(h(k + 4, k + 1));
                    }
                    if (cabs1(h(k + 1, k)) <= std::max(smlnum, ulp * tst1))
                    {
                        const R h12 = std::max(cabs1(h(k + 1, k)), cabs1(h(k, k + 1)));
                        const R h21 = std::min(cabs1(h(k + 1, k)), cabs1(h(k, k + 1)));
                        const R h11 = std::max(cabs1(h(k + 1, k + 1)), cabs1(h(k, k) - h(k + 1, k + 1)));
                        const R h22 = std::min(cabs1(h(k + 1, k + 1)), cabs1(h(k, k) - h(k + 1, k + 1)));
                        const R scl = h11 + h12;
                        const R tst2 = h22 * (h11 / scl);
                        if (tst2 == R{0} || h21 * (h12 / scl) <= std::max(smlnum, ulp * tst2))
                        {
                            h(k + 1, k) = czero;
                        }
                    }
                }
            }

            // ---- Delayed left updates (within the slab) for the chain ----
            const crd::isize jbot_acc = std::min(ndcol, kbot);
            for (crd::isize m = mbot; m >= mtop; --m)
            {
                const crd::isize k = krcol + 2 * (m - 1);
                const T tau = vref(1, m);
                const T v2 = vref(2, m);
                const T v3 = vref(3, m);
                const T t1l = crd::hesap::conj(tau);
                const T t2l = t1l * v2;
                const T t3l = t1l * v3;
                for (crd::isize j = std::max(ktop, krcol + 2 * m); j <= jbot_acc; ++j)
                {
                    const T refsum = h(k + 1, j) + crd::hesap::conj(v2) * h(k + 2, j) +
                                     crd::hesap::conj(v3) * h(k + 3, j);
                    h(k + 1, j) = h(k + 1, j) - refsum * t1l;
                    h(k + 2, j) = h(k + 2, j) - refsum * t2l;
                    h(k + 3, j) = h(k + 3, j) - refsum * t3l;
                }
            }

            // ---- Accumulate the chain reflections into U (RIGHT form) ----
            for (crd::isize m = mbot; m >= mtop; --m)
            {
                const crd::isize k = krcol + 2 * (m - 1);
                const crd::isize kms = k - incol;
                crd::isize i2 = std::max<crd::isize>(1, ktop - incol);
                i2 = std::max(i2, kms - (krcol - incol) + 1);
                const crd::isize i4 = std::min(kdu, krcol + 2 * (mbot - 1) - incol + 5);
                const T tau = vref(1, m);
                const T v2 = vref(2, m);
                const T v3 = vref(3, m);
                const T t1r = tau;
                const T t2r = tau * crd::hesap::conj(v2);
                const T t3r = tau * crd::hesap::conj(v3);
                for (crd::isize j = i2; j <= i4; ++j)
                {
                    const T refsum = u(j, kms + 1) + v2 * u(j, kms + 2) + v3 * u(j, kms + 3);
                    u(j, kms + 1) = u(j, kms + 1) - refsum * t1r;
                    u(j, kms + 2) = u(j, kms + 2) - refsum * t2r;
                    u(j, kms + 3) = u(j, kms + 3) - refsum * t3r;
                }
            }
        }

        // ---- Far-from-diagonal updates via gemm using the accumulated U ----
        const crd::isize jtop_g = wantt ? 1 : ktop;
        const crd::isize jbot_g = wantt ? nn : kbot;
        const crd::isize k1 = std::max<crd::isize>(1, ktop - incol);
        const crd::isize nu = (kdu - std::max<crd::isize>(0, ndcol - kbot)) - k1 + 1;
        if (nu > 0)
        {
            const crd::usize nus = static_cast<crd::usize>(nu);
            const T* ublk = &u(k1, k1);
            const crd::usize uld = umat.ld();
            const crd::isize jcol0 = std::min(ndcol, kbot) + 1;
            if (jcol0 <= jbot_g)  // Horizontal: H := Uᴴ·H
            {
                slab_left_h<T>(hd, ld, static_cast<crd::usize>(incol + k1 - 1),
                               static_cast<crd::usize>(jcol0 - 1), nus,
                               static_cast<crd::usize>(jbot_g - jcol0 + 1), ublk, uld, scratch, alloc);
            }
            const crd::isize maxki = std::max(ktop, incol);
            if (jtop_g <= maxki - 1)  // Vertical: H := H·U
            {
                slab_right<T>(hd, ld, static_cast<crd::usize>(jtop_g - 1),
                              static_cast<crd::usize>(incol + k1 - 1),
                              static_cast<crd::usize>(maxki - jtop_g), nus, ublk, uld, scratch, alloc);
            }
            if (wantz)  // Z := Z·U
            {
                slab_right<T>(zd, zld, static_cast<crd::usize>(iloz - 1),
                              static_cast<crd::usize>(incol + k1 - 1),
                              static_cast<crd::usize>(ihiz - iloz + 1), nus, ublk, uld, scratch, alloc);
            }
        }
    }
}

// v3d-2c-2b-3 — crossover: finish a small active block [ktop, kbot] with
// single-shift `complex_schur` (the NMIN tail of the AED driver), writing the
// upper-triangular block back into H + the global slab updates (complex sibling
// of `schur_small_block`).
template <typename T>
void complex_schur_small_block(crd::memory::IAllocator* alloc, T* hd, crd::usize ld, crd::usize n,
                               crd::usize ktop, crd::usize kbot, T* zd, crd::usize zld, bool wantz,
                               crd::usize iloz, crd::usize ihiz, crd::containers::Array<T>& w)
{
    using R = RealType<T>;
    const crd::usize nh = kbot - ktop + 1;
    auto h = [&](crd::usize a, crd::usize b) -> T& { return hd[a * ld + b]; };
    Matrix<T> win(alloc, nh, nh);
    for (crd::usize a = 0; a < nh; ++a)
    {
        for (crd::usize b = 0; b < nh; ++b)
        {
            win.at(a, b) = (b + 1 >= a) ? h(ktop + a, ktop + b) : T{R{0}, R{0}};
        }
    }
    ComplexSchur<T> sch = complex_schur<T>(alloc, win, 0, nh - 1, true);
    for (crd::usize a = 0; a < nh; ++a)
    {
        for (crd::usize b = (a == 0 ? 0 : a - 1); b < nh; ++b)
        {
            h(ktop + a, ktop + b) = sch.t.at(a, b);
        }
        w[ktop + a] = sch.w.data()[a];
    }
    Matrix<T> scratch(alloc, n, n);
    slab_right<T>(hd, ld, 0, ktop, ktop, nh, sch.z.data(), sch.z.ld(), scratch, alloc);
    if (kbot + 1 < n)
    {
        slab_left_h<T>(hd, ld, ktop, kbot + 1, nh, n - (kbot + 1), sch.z.data(), sch.z.ld(), scratch,
                       alloc);
    }
    if (wantz)
    {
        slab_right<T>(zd, zld, iloz, ktop, ihiz - iloz + 1, nh, sch.z.data(), sch.z.ld(), scratch,
                      alloc);
    }
}
} // namespace

namespace detail
{
template <typename T>
void complex_multishift_sweep(crd::memory::IAllocator* alloc, crd::usize n, crd::usize ktop,
                              crd::usize kbot, const T* shifts, crd::usize nshifts, T* hd,
                              crd::usize ld, crd::usize iloz, crd::usize ihiz, T* zd, crd::usize zld,
                              bool wantz)
{
    complex_dlaqr5_sweep<T>(alloc, true, wantz, n, ktop, kbot, shifts, nshifts, hd, ld, iloz, ihiz, zd,
                            zld);
}
template void complex_multishift_sweep<Complex<float>>(crd::memory::IAllocator*, crd::usize, crd::usize,
                                                       crd::usize, const Complex<float>*, crd::usize,
                                                       Complex<float>*, crd::usize, crd::usize,
                                                       crd::usize, Complex<float>*, crd::usize, bool);
template void complex_multishift_sweep<Complex<double>>(
    crd::memory::IAllocator*, crd::usize, crd::usize, crd::usize, const Complex<double>*, crd::usize,
    Complex<double>*, crd::usize, crd::usize, crd::usize, Complex<double>*, crd::usize, bool);
} // namespace detail

template <typename T>
ComplexSchur<T> complex_schur_aed(crd::memory::IAllocator* alloc, const Matrix<T>& h_in, crd::usize ilo,
                                  crd::usize ihi, bool vectors, crd::usize* sweeps)
{
    static_assert(is_complex_v<T>, "complex_schur_aed is complex-only");
    using R = RealType<T>;
    const crd::usize n = h_in.rows();
    ComplexSchur<T> out(alloc);
    out.t = h_in.clone();
    out.w.resize(n);
    const T czero{R{0}, R{0}};
    for (crd::usize k = 0; k < n; ++k)
    {
        out.w[k] = czero;
    }
    if (vectors)
    {
        out.z = Matrix<T>(alloc, n, n);
        out.z.set_identity();
    }
    if (sweeps != nullptr)
    {
        *sweeps = 0;
    }
    if (n == 0)
    {
        out.converged = true;
        return out;
    }

    // Crossover below which single-shift complex_schur is faster than AED.
    // NMIN measured (NOT borrowed from the real path's 200): complex single-shift
    // is cheaper per sweep but runs more sweeps; tuned in the perf pass.
    // NMIN measured (NOT borrowed from the real path's 200): the AED+train win
    // over single-shift complex_schur crosses over at n≈200 (n=128 loses 0.73×,
    // n=256 wins 1.16×, n=512 wins 2.01×), so blocks ≤150 are finished by
    // single-shift; AED engages above and the win widens with N.
    constexpr crd::usize nmin = 150;
    constexpr crd::usize nibble = 14;
    T* hd = out.t.data();
    const crd::usize ld = out.t.ld();
    T* zd = vectors ? out.z.data() : nullptr;
    const crd::usize zld = vectors ? out.z.ld() : 0;
    auto h = [&](crd::usize a, crd::usize b) -> T& { return hd[a * ld + b]; };
    auto cabs1 = [](const T& x) -> R { return std::abs(x.re) + std::abs(x.im); };

    for (crd::usize k = 0; k < ilo; ++k)
    {
        out.w[k] = h(k, k);
    }
    for (crd::usize k = ihi + 1; k < n; ++k)
    {
        out.w[k] = h(k, k);
    }

    const R eps = std::numeric_limits<R>::epsilon();
    const R safmin = std::numeric_limits<R>::min();
    const R ulp = eps;
    const R smlnum = safmin * (static_cast<R>(ihi - ilo + 1) / ulp);
    crd::usize total_sweeps = 0;

    crd::isize kbot = static_cast<crd::isize>(ihi);
    crd::usize stall = 0;
    crd::usize last_kbot = ihi + 1;
    const crd::usize itmax = 30 * std::max<crd::usize>(10, ihi - ilo + 1);
    crd::usize iters = 0;

    while (kbot >= static_cast<crd::isize>(ilo))
    {
        const crd::usize kb = static_cast<crd::usize>(kbot);
        if (++iters > itmax)
        {
            out.converged = false;
            return out;
        }
        // Find the active block top: split at a negligible subdiagonal (cabs1).
        crd::usize ktop = ilo;
        for (crd::usize kk = kb; kk > ilo; --kk)
        {
            const R tst = cabs1(h(kk - 1, kk - 1)) + cabs1(h(kk, kk));
            const R thr = (tst == R{0}) ? smlnum : ulp * tst;
            if (cabs1(h(kk, kk - 1)) <= std::max(smlnum, thr))
            {
                h(kk, kk - 1) = czero;
                ktop = kk;
                break;
            }
        }
        const crd::usize nh = kb - ktop + 1;

        if (nh == 1)
        {
            out.w[kb] = h(kb, kb);
            kbot = static_cast<crd::isize>(ktop) - 1;
            stall = 0;
            continue;
        }

        if (kb != last_kbot)
        {
            stall = 0;
            last_kbot = kb;
        }

        if (nh <= nmin || stall >= 3)
        {
            // Crossover / stalled: finish the whole block with single-shift zlahqr.
            complex_schur_small_block<T>(alloc, hd, ld, n, ktop, kb, zd, zld, vectors, ilo, ihi, out.w);
            kbot = static_cast<crd::isize>(ktop) - 1;
            stall = 0;
            continue;
        }

        // Aggressive Early Deflation on the trailing window.
        crd::usize nw = std::min(nh, std::max<crd::usize>(2, nh / 3));
        if (nw > nh - 1)
        {
            nw = nh - 1;
        }
        const crd::usize kwtop = kb - nw + 1;
        const AedResult<T> aed = complex_aed_deflate<T>(alloc, out.t, ktop, kb, nw, out.z, vectors, ilo,
                                                        ihi, true, out.w);
        kbot = static_cast<crd::isize>(kb) - static_cast<crd::isize>(aed.nd);

        // Nibble: if deflation was productive, skip the sweep and AED again.
        if (aed.nd > 0 && 100 * aed.nd > nibble * nw)
        {
            stall = 0;
            continue;
        }

        // One small-bulge multishift QR sweep using the undeflated AED eigenvalues
        // as shifts (they occupy [kwtop, kwtop+ns-1] after complex_aed_deflate).
        const crd::usize ns = aed.ns;
        const crd::isize kbnew = kbot;
        if (ns >= 2 && kbnew >= static_cast<crd::isize>(ktop) + 2)
        {
            const crd::usize sweep_bot = static_cast<crd::usize>(kbnew);
            crd::containers::Array<T> shf(alloc);
            shf.resize(ns);
            for (crd::usize i = 0; i < ns; ++i)
            {
                shf[i] = out.w[kwtop + i];
            }
            complex_dlaqr5_sweep<T>(alloc, true, vectors, n, ktop, sweep_bot, shf.data(), ns, hd, ld,
                                    ilo, ihi, zd, zld);
            ++total_sweeps;
        }
        ++stall;
    }

    out.converged = true;
    if (sweeps != nullptr)
    {
        *sweeps = total_sweeps;
    }
    return out;
}

template ComplexSchur<Complex<float>> complex_schur_aed<Complex<float>>(
    crd::memory::IAllocator*, const Matrix<Complex<float>>&, crd::usize, crd::usize, bool, crd::usize*);
template ComplexSchur<Complex<double>> complex_schur_aed<Complex<double>>(
    crd::memory::IAllocator*, const Matrix<Complex<double>>&, crd::usize, crd::usize, bool, crd::usize*);

// =======================================================================
// v3d-2b — public non-symmetric eigensolver (real input). Assembles the
// shipped pipeline: balance → hessenberg + form Q → schur_aed → dtrevc →
// 3-stage back-transform → complex assembly + normalization.
// =======================================================================
template <typename T>
EigNonsym<T> eig_real_impl(crd::memory::IAllocator* alloc, const Matrix<T>& a)
{
    static_assert(!is_complex_v<T>, "eig_real_impl is real-only (complex is eig_complex_impl)");
    const crd::usize n = a.rows();
    CRD_ASSERT_MSG(a.is_square(), "eig: A must be square");
    EigNonsym<T> out(alloc, n);
    if (n == 0)
    {
        return out;
    }
    if (n == 1) // a 1×1 matrix: its single entry is the (real) eigenvalue, eigenvector [1]. The balance/Hessenberg
    {           // pipeline below assumes n >= 2 (ihi > ilo) — guard here so degree-1 `roots` / 1×1 eig is safe.
        out.values.data()[0] = Complex<T>{a.data()[0], T{0}};
        out.vectors.at(0, 0) = Complex<T>{T{1}, T{0}};
        return out;
    }

    // 1. balance — isolating permutation + radix-2 diagonal scaling.
    Matrix<T> work = a.clone();
    crd::containers::Array<T> scale(alloc);
    crd::usize ilo = 0;
    crd::usize ihi = 0;
    balance<T>(work, scale, ilo, ihi);

    // 2. Hessenberg reduction of the active block + explicit Q (dorghr).
    crd::containers::Array<T> tau(alloc);
    hessenberg<T>(work, ilo, ihi, tau);
    Matrix<T> q = form_hessenberg_q<T>(alloc, work, ilo, ihi, tau);

    // 3. Extract the full Hessenberg H: upper triangle + first subdiagonal
    //    (j+1 >= i). The corners are already block-upper-triangular after
    //    balance; the masked strict-lower of the active block holds reflectors.
    Matrix<T> hmat(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            hmat.at(i, j) = (j + 1 >= i) ? work.at(i, j) : T{0};
        }
    }

    // 4. Real Schur via AED multishift train: A_bal = (Q·Z)·T·(Q·Z)ᵀ.
    RealSchur<T> sch = schur_aed<T>(alloc, hmat, ilo, ihi, true);

    // 5. Right eigenvectors of the quasi-triangular T (real-packed columns).
    Matrix<T> vschur = detail::schur_right_eigvecs<T>(alloc, sch.t);

    // 6. Back-transform stages (i)+(ii): QZ = Q·Z, then V = QZ·V_schur. Real
    //    linear maps preserve the dtrevc re/im column packing.
    Matrix<T> qz(alloc, n, n);
    gemm<T, Layout::RowMajor>(T{1}, q, sch.z, T{0}, qz);
    Matrix<T> vfull(alloc, n, n);
    gemm<T, Layout::RowMajor>(T{1}, qz, vschur, T{0}, vfull);

    // 7. Stage (iii): undo balance (dgebak) on the rows of V.
    gebak_right(vfull.data(), vfull.ld(), n, n, ilo, ihi, scale.data());

    // 8. Assemble complex eigenpairs (Schur order, D(non-sym)-5) + normalize
    //    each column to ‖·‖₂=1 with the lowest-index largest-magnitude
    //    component rotated real-positive (D(non-sym)-4).
    auto vat = [&](crd::usize i, crd::usize j) -> T { return vfull.data()[i * vfull.ld() + j]; };
    crd::usize k = 0;
    while (k < n)
    {
        const T li = sch.wi[k];
        if (li == T{0})
        {
            // Real eigenvalue → real eigenvector in column k.
            T norm2 = T{0};
            crd::usize istar = 0;
            T best = T{-1};
            for (crd::usize i = 0; i < n; ++i)
            {
                const T c = vat(i, k);
                norm2 += c * c;
                const T mag = std::abs(c);
                if (mag > best)
                {
                    best = mag;
                    istar = i;
                }
            }
            norm2 = std::sqrt(norm2);
            const T sgn = (vat(istar, k) < T{0}) ? T{-1} : T{1};
            const T s = (norm2 > T{0}) ? sgn / norm2 : T{1};
            for (crd::usize i = 0; i < n; ++i)
            {
                out.vectors.at(i, k) = Complex<T>{vat(i, k) * s, T{0}};
            }
            out.values.data()[k] = Complex<T>{sch.wr[k], T{0}};
            k += 1;
        }
        else
        {
            // Complex-conjugate pair: columns k (re), k+1 (im); wi[k] > 0.
            CRD_ASSERT_MSG(li > T{0}, "eig: complex pair must lead with +imag (Schur convention)");
            T norm2 = T{0};
            crd::usize istar = 0;
            T best = T{-1};
            for (crd::usize i = 0; i < n; ++i)
            {
                const T re = vat(i, k);
                const T im = vat(i, k + 1);
                const T mag2 = re * re + im * im;
                norm2 += mag2;
                if (mag2 > best)
                {
                    best = mag2;
                    istar = i;
                }
            }
            norm2 = std::sqrt(norm2);
            const T rr = vat(istar, k);
            const T ri = vat(istar, k + 1);
            const T m = std::sqrt(rr * rr + ri * ri);
            const T inv = (m > T{0} && norm2 > T{0}) ? T{1} / (m * norm2) : T{1};
            for (crd::usize i = 0; i < n; ++i)
            {
                const T re = vat(i, k);
                const T im = vat(i, k + 1);
                // Phase-rotate by conj(rr+i·ri)/|·| then scale to unit 2-norm.
                const T nre = (rr * re + ri * im) * inv;
                const T nim = (rr * im - ri * re) * inv;
                out.vectors.at(i, k) = Complex<T>{nre, nim};
                out.vectors.at(i, k + 1) = Complex<T>{nre, -nim};  // conjugate column
            }
            out.values.data()[k] = Complex<T>{sch.wr[k], li};
            out.values.data()[k + 1] = Complex<T>{sch.wr[k + 1], sch.wi[k + 1]};
            k += 2;
        }
    }
    return out;
}

// v3d-2c-3 — complex non-symmetric eigensolver. Pipeline mirrors the real
// eig_real_impl but UNITARY (Q, Z) and all-complex eigenpairs (no real-packed
// columns, no conjugate-pair assembly): balance → hessenberg + form Q →
// complex_schur_aed → ztrevc → V = D⁻¹P · Q · Z · V_schur (two gemms + complex
// gebak) → normalize each column to ‖·‖₂=1 with the largest-magnitude component
// phase-rotated real-positive (D(non-sym)-4). Eigenpairs in Schur order
// (D(non-sym)-5). T = Complex<f32|f64>.
template <typename T>
EigNonsym<T> eig_complex_impl(crd::memory::IAllocator* alloc, const Matrix<T>& a)
{
    static_assert(is_complex_v<T>, "eig_complex_impl is complex-only");
    using R = RealType<T>;
    const crd::usize n = a.rows();
    CRD_ASSERT_MSG(a.is_square(), "eig: A must be square");
    EigNonsym<T> out(alloc, n);
    if (n == 0)
    {
        return out;
    }
    if (n == 1) // 1×1: the single entry is the eigenvalue, eigenvector [1] (the pipeline below assumes n >= 2).
    {
        out.values.data()[0] = a.data()[0];
        out.vectors.at(0, 0) = T{R{1}, R{0}};
        return out;
    }
    const T czero{R{0}, R{0}};

    // 1. balance (zgebal) — real scale array (perm indices + radix-2 factors).
    Matrix<T> work = a.clone();
    crd::containers::Array<R> scale(alloc);
    crd::usize ilo = 0;
    crd::usize ihi = 0;
    balance<T>(work, scale, ilo, ihi);

    // 2. complex Hessenberg (zgehd2) + explicit unitary Q (zunghr).
    crd::containers::Array<T> tau(alloc);
    hessenberg<T>(work, ilo, ihi, tau);
    Matrix<T> q = form_hessenberg_q<T>(alloc, work, ilo, ihi, tau);

    // 3. Extract the full upper-Hessenberg H (j+1 >= i).
    Matrix<T> hmat(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            hmat.at(i, j) = (j + 1 >= i) ? work.at(i, j) : czero;
        }
    }

    // 4. Complex Schur via AED multishift: A_bal = (Q·Z)·T·(Q·Z)ᴴ.
    ComplexSchur<T> sch = complex_schur_aed<T>(alloc, hmat, ilo, ihi, true);

    // 5. Right eigenvectors of the upper-triangular T (un-normalized).
    Matrix<T> vschur(alloc);
    ztrevc_right<T>(alloc, sch.t, vschur);

    // 6. Back-transform (i)+(ii): QZ = Q·Z, then V = QZ·V_schur.
    Matrix<T> qz(alloc, n, n);
    gemm<T, Layout::RowMajor>(T{R{1}, R{0}}, q, sch.z, czero, qz);
    Matrix<T> vfull(alloc, n, n);
    gemm<T, Layout::RowMajor>(T{R{1}, R{0}}, qz, vschur, czero, vfull);

    // 7. Stage (iii): undo balance (zgebak) on the rows of V (real scale).
    gebak_right(vfull.data(), vfull.ld(), n, n, ilo, ihi, scale.data());

    // 8. Normalize each column once (D(non-sym)-4): ‖·‖₂=1, then phase-rotate so
    //    the lowest-index largest-magnitude component is real-positive.
    auto vat = [&](crd::usize i, crd::usize j) -> T { return vfull.data()[i * vfull.ld() + j]; };
    for (crd::usize k = 0; k < n; ++k)
    {
        R norm2 = R{0};
        crd::usize istar = 0;
        R best = R{-1};
        for (crd::usize i = 0; i < n; ++i)
        {
            const T c = vat(i, k);
            const R mag2 = c.re * c.re + c.im * c.im;
            norm2 += mag2;
            if (mag2 > best)
            {
                best = mag2;
                istar = i;
            }
        }
        norm2 = std::sqrt(norm2);
        const T piv = vat(istar, k);
        const R pm = std::sqrt(piv.re * piv.re + piv.im * piv.im);
        const R inv = (pm > R{0} && norm2 > R{0}) ? R{1} / (pm * norm2) : R{1};
        for (crd::usize i = 0; i < n; ++i)
        {
            const T c = vat(i, k);
            // Rotate by conj(piv)/pm (makes piv real-positive) then scale to ‖·‖₂=1.
            const R nre = (c.re * piv.re + c.im * piv.im) * inv;
            const R nim = (c.im * piv.re - c.re * piv.im) * inv;
            out.vectors.at(i, k) = T{nre, nim};
        }
        out.values.data()[k] = sch.w.data()[k];
    }
    return out;
}

// Public dispatcher (v3d-2b real + v3d-2c-3 complex).
template <typename T>
EigNonsym<T> eig(crd::memory::IAllocator* alloc, const Matrix<T>& a)
{
    if constexpr (is_complex_v<T>)
    {
        return eig_complex_impl<T>(alloc, a);
    }
    else
    {
        return eig_real_impl<T>(alloc, a);
    }
}

template EigNonsym<float> eig<float>(crd::memory::IAllocator*, const Matrix<float>&);
template EigNonsym<double> eig<double>(crd::memory::IAllocator*, const Matrix<double>&);
template EigNonsym<Complex<float>> eig<Complex<float>>(crd::memory::IAllocator*,
                                                       const Matrix<Complex<float>>&);
template EigNonsym<Complex<double>> eig<Complex<double>>(crd::memory::IAllocator*,
                                                         const Matrix<Complex<double>>&);

// v3d-1c-4 (multishift train) — thin wrapper exposing the anonymous-namespace
// `dlaqr5_sweep` (the train) for the M1/M2 correctness gate. Not yet on the
// driver path (wired at M3).
namespace detail
{
template <typename T>
void multishift_sweep(crd::memory::IAllocator* alloc, crd::usize n, crd::usize ktop, crd::usize kbot,
                      const T* sr, const T* si, crd::usize nshifts, T* hd, crd::usize ld,
                      crd::usize iloz, crd::usize ihiz, T* zd, crd::usize zld, bool wantz)
{
    dlaqr5_sweep<T>(alloc, true, wantz, n, ktop, kbot, sr, si, nshifts, hd, ld, iloz, ihiz, zd, zld);
}

template void multishift_sweep<float>(crd::memory::IAllocator*, crd::usize, crd::usize, crd::usize,
                                      const float*, const float*, crd::usize, float*, crd::usize,
                                      crd::usize, crd::usize, float*, crd::usize, bool);
template void multishift_sweep<double>(crd::memory::IAllocator*, crd::usize, crd::usize, crd::usize,
                                       const double*, const double*, crd::usize, double*, crd::usize,
                                       crd::usize, crd::usize, double*, crd::usize, bool);

template <typename T>
void lin_solve_2x2(bool ltrans, int na, int nw, T smin, T ca, const T* a2, T d1, T d2, const T* b2,
                   T wr, T wi, T* x2, T& scale, T& xnorm, int& info)
{
    const T a[2][2] = {{a2[0], a2[1]}, {a2[2], a2[3]}};
    const T b[2][2] = {{b2[0], b2[1]}, {b2[2], b2[3]}};
    const Ln2<T> r = dlaln2<T>(ltrans, na, nw, smin, ca, a, d1, d2, b, wr, wi);
    x2[0] = r.x[0][0];
    x2[1] = r.x[0][1];
    x2[2] = r.x[1][0];
    x2[3] = r.x[1][1];
    scale = r.scale;
    xnorm = r.xnorm;
    info = r.info;
}

template void lin_solve_2x2<float>(bool, int, int, float, float, const float*, float, float,
                                   const float*, float, float, float*, float&, float&, int&);
template void lin_solve_2x2<double>(bool, int, int, double, double, const double*, double, double,
                                    const double*, double, double, double*, double&, double&, int&);

template <typename T>
Matrix<T> schur_right_eigvecs(crd::memory::IAllocator* alloc, const Matrix<T>& t)
{
    Matrix<T> vr(alloc);
    dtrevc_right<T>(alloc, t, vr);
    return vr;
}

template Matrix<float> schur_right_eigvecs<float>(crd::memory::IAllocator*, const Matrix<float>&);
template Matrix<double> schur_right_eigvecs<double>(crd::memory::IAllocator*, const Matrix<double>&);
} // namespace detail

} // namespace crd::hesap::dense
