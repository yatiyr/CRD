#include <crd/hesap/dense/eig_sym.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/householder.hpp>
#include <crd/hesap/dense/detail/secular.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::hesap::dense
{
namespace
{
// =======================================================================
// Scalar numeric helpers (faithful ports of the LAPACK auxiliaries the
// symmetric tridiagonal QL/QR path needs). All deterministic: only
// correctly-rounded ops (+ - * / sqrt) and abs/sign — no transcendentals,
// no RNG (D(dense-eig)-1/-2).
// =======================================================================

template <typename R>
[[nodiscard]] inline R sgn(R x) noexcept
{
    return x >= R{0} ? R{1} : R{-1};
}

// dlapy2 — overflow-safe hypot.
template <typename R>
[[nodiscard]] inline R lapy2(R x, R y) noexcept
{
    return detail::hypot2(x, y);
}

// dlartg — generate a Givens rotation [c s; -s c] such that
// [c s; -s c] * [f; g] = [r; 0]. Deterministic + numerically safe.
template <typename R>
struct Givens
{
    R c;
    R s;
    R r;
};

template <typename R>
[[nodiscard]] inline Givens<R> lartg(R f, R g) noexcept
{
    if (g == R{0})
    {
        return Givens<R>{R{1}, R{0}, f};
    }
    if (f == R{0})
    {
        return Givens<R>{R{0}, R{1}, g};
    }
    if (std::abs(f) > std::abs(g))
    {
        const R t = g / f;
        const R u = sgn(f) * std::sqrt(R{1} + t * t);
        const R c = R{1} / u;
        return Givens<R>{c, t * c, f * u};
    }
    const R t = f / g;
    const R u = sgn(g) * std::sqrt(R{1} + t * t);
    const R s = R{1} / u;
    return Givens<R>{t * s, s, g * u};
}

// dlae2 — eigenvalues of the 2x2 symmetric [[a, b], [b, c]]. rt1 >= rt2.
template <typename R>
struct Eig2
{
    R rt1;
    R rt2;
    R cs1;  // eigenvector (cs1, sn1) for rt1 (only filled by laev2)
    R sn1;
};

template <typename R>
[[nodiscard]] inline Eig2<R> lae2(R a, R b, R c) noexcept
{
    const R sm = a + c;
    const R df = a - c;
    const R adf = std::abs(df);
    const R tb = b + b;
    const R ab = std::abs(tb);
    R acmx;
    R acmn;
    if (std::abs(a) > std::abs(c))
    {
        acmx = a;
        acmn = c;
    }
    else
    {
        acmx = c;
        acmn = a;
    }
    R rt;
    if (adf > ab)
    {
        const R q = ab / adf;
        rt = adf * std::sqrt(R{1} + q * q);
    }
    else if (adf < ab)
    {
        const R q = adf / ab;
        rt = ab * std::sqrt(R{1} + q * q);
    }
    else
    {
        rt = ab * std::sqrt(R{2});
    }
    Eig2<R> out{};
    if (sm < R{0})
    {
        out.rt1 = R{0.5} * (sm - rt);
        out.rt2 = (acmx / out.rt1) * acmn - (b / out.rt1) * b;
    }
    else if (sm > R{0})
    {
        out.rt1 = R{0.5} * (sm + rt);
        out.rt2 = (acmx / out.rt1) * acmn - (b / out.rt1) * b;
    }
    else
    {
        out.rt1 = R{0.5} * rt;
        out.rt2 = R{-0.5} * rt;
    }
    return out;
}

// dlaev2 — eigenvalues + eigenvector (cs1, sn1) for rt1, of [[a,b],[b,c]].
template <typename R>
[[nodiscard]] inline Eig2<R> laev2(R a, R b, R c) noexcept
{
    const R sm = a + c;
    const R df = a - c;
    const R adf = std::abs(df);
    const R tb = b + b;
    const R ab = std::abs(tb);
    R acmx;
    R acmn;
    if (std::abs(a) > std::abs(c))
    {
        acmx = a;
        acmn = c;
    }
    else
    {
        acmx = c;
        acmn = a;
    }
    R rt;
    if (adf > ab)
    {
        const R q = ab / adf;
        rt = adf * std::sqrt(R{1} + q * q);
    }
    else if (adf < ab)
    {
        const R q = adf / ab;
        rt = ab * std::sqrt(R{1} + q * q);
    }
    else
    {
        rt = ab * std::sqrt(R{2});
    }
    Eig2<R> out{};
    int sgn1;
    if (sm < R{0})
    {
        out.rt1 = R{0.5} * (sm - rt);
        sgn1 = -1;
        out.rt2 = (acmx / out.rt1) * acmn - (b / out.rt1) * b;
    }
    else if (sm > R{0})
    {
        out.rt1 = R{0.5} * (sm + rt);
        sgn1 = 1;
        out.rt2 = (acmx / out.rt1) * acmn - (b / out.rt1) * b;
    }
    else
    {
        out.rt1 = R{0.5} * rt;
        out.rt2 = R{-0.5} * rt;
        sgn1 = 1;
    }

    // Eigenvector.
    R cs;
    int sgn2;
    if (df >= R{0})
    {
        cs = df + rt;
        sgn2 = 1;
    }
    else
    {
        cs = df - rt;
        sgn2 = -1;
    }
    const R acs = std::abs(cs);
    if (acs > ab)
    {
        const R ct = -tb / cs;
        out.sn1 = R{1} / std::sqrt(R{1} + ct * ct);
        out.cs1 = ct * out.sn1;
    }
    else
    {
        if (ab == R{0})
        {
            out.cs1 = R{1};
            out.sn1 = R{0};
        }
        else
        {
            const R tn = -cs / tb;
            out.cs1 = R{1} / std::sqrt(R{1} + tn * tn);
            out.sn1 = tn * out.cs1;
        }
    }
    if (sgn1 == sgn2)
    {
        const R tn = out.cs1;
        out.cs1 = -out.sn1;
        out.sn1 = tn;
    }
    return out;
}

// Apply the plane rotation (ctemp, stemp) to columns p and q of z. Z is stored
// COLUMN-MAJOR (z[col*ldz + row]) so each column is contiguous — the Givens
// column-rotation is then a contiguous SIMD sweep (the key perf fix vs a
// row-major Z where the same rotation strides by ldz and thrashes cache; this
// is the ADR-0083 layout-fit escape hatch applied to the eigensolver). Matches
// LAPACK dlasr's 'R','V' body:  q' = c*q - s*p;  p' = s*q + c*p.
template <typename R, typename Z>
inline void rot_cols(Z* z, crd::usize ldz, crd::usize n, crd::usize p, crd::usize q, R ctemp,
                     R stemp) noexcept
{
    Z* colp = z + p * ldz;
    Z* colq = z + q * ldz;
    crd::usize r = 0;
    if constexpr (std::is_same_v<Z, crd::f64> && std::is_same_v<R, crd::f64>)
    {
        const crd::math::simd::Vec4d cc(ctemp);
        const crd::math::simd::Vec4d ss(stemp);
        for (; r + 4 <= n; r += 4)
        {
            const crd::math::simd::Vec4d tq = crd::math::simd::Vec4d::load(colq + r);
            const crd::math::simd::Vec4d tp = crd::math::simd::Vec4d::load(colp + r);
            (cc * tq - ss * tp).store(colq + r);
            (ss * tq + cc * tp).store(colp + r);
        }
    }
    else if constexpr (std::is_same_v<Z, crd::f32> && std::is_same_v<R, crd::f32>)
    {
        const crd::math::simd::Vec8f cc(ctemp);
        const crd::math::simd::Vec8f ss(stemp);
        for (; r + 8 <= n; r += 8)
        {
            const crd::math::simd::Vec8f tq = crd::math::simd::Vec8f::load(colq + r);
            const crd::math::simd::Vec8f tp = crd::math::simd::Vec8f::load(colp + r);
            (cc * tq - ss * tp).store(colq + r);
            (ss * tq + cc * tp).store(colp + r);
        }
    }
    for (; r < n; ++r)
    {
        const Z tq = colq[r];
        const Z tp = colp[r];
        colq[r] = ctemp * tq - stemp * tp;
        colp[r] = stemp * tq + ctemp * tp;
    }
}

// Contiguous SIMD dot over [0, n):  sum x[t]*y[t].
template <typename T>
inline T simd_dot(const T* x, const T* y, crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize t = 0;
    T acc{};
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        simd::Vec4d a0 = simd::Vec4d::zero();
        simd::Vec4d a1 = simd::Vec4d::zero();
        for (; t + 8 <= n; t += 8)
        {
            a0 = simd::fma(simd::Vec4d::load(x + t), simd::Vec4d::load(y + t), a0);
            a1 = simd::fma(simd::Vec4d::load(x + t + 4), simd::Vec4d::load(y + t + 4), a1);
        }
        acc = simd::horizontal_sum(a0 + a1);
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        simd::Vec8f a0 = simd::Vec8f::zero();
        for (; t + 8 <= n; t += 8)
        {
            a0 = simd::fma(simd::Vec8f::load(x + t), simd::Vec8f::load(y + t), a0);
        }
        acc = simd::horizontal_sum(a0);
    }
    for (; t < n; ++t)
    {
        acc += x[t] * y[t];
    }
    return acc;
}

// Contiguous SIMD axpy over [0, n):  y[t] += a * x[t].
template <typename T>
inline void simd_axpy(T* y, const T* x, T a, crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize t = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        const simd::Vec4d av(a);
        for (; t + 8 <= n; t += 8)
        {
            simd::fma(av, simd::Vec4d::load(x + t), simd::Vec4d::load(y + t)).store(y + t);
            simd::fma(av, simd::Vec4d::load(x + t + 4), simd::Vec4d::load(y + t + 4)).store(y + t + 4);
        }
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        const simd::Vec8f av(a);
        for (; t + 8 <= n; t += 8)
        {
            simd::fma(av, simd::Vec8f::load(x + t), simd::Vec8f::load(y + t)).store(y + t);
        }
    }
    for (; t < n; ++t)
    {
        y[t] += a * x[t];
    }
}

} // namespace

// =======================================================================
// steqr — implicit-shift QL/QR on the real symmetric tridiagonal (d, e).
// Faithful port of LAPACK dsteqr (COMPZ='V' when want_vectors). Rotations
// are applied to z's columns immediately as generated (mathematically
// identical to dsteqr's save-then-dlasr; avoids the 2n-2 workspace). Does
// NOT sort — the driver applies the deterministic ascending sort.
// =======================================================================
template <typename R, typename Z>
int steqr(R* d, R* e, crd::usize n_, Z* z, crd::usize ldz, bool want_vectors)
{
    if (n_ <= 1)
    {
        return 0;
    }
    const int n = static_cast<int>(n_);

    const R eps = std::numeric_limits<R>::epsilon();
    const R eps2 = eps * eps;
    const R safmin = std::numeric_limits<R>::min();
    const R safmax = R{1} / safmin;
    const R ssfmax = std::sqrt(safmax) / R{3};
    const R ssfmin = std::sqrt(safmin) / eps2;
    constexpr int kMaxit = 30;
    const int nmaxit = n * kMaxit;
    int jtot = 0;

    // 1-based logical indices (matching the Fortran); array access subtracts 1.
    int l1 = 1;

    while (true)
    {
        if (l1 > n)
        {
            break;
        }
        if (l1 > 1)
        {
            e[l1 - 2] = R{0};
        }
        // Find the next block [l, lend]: split at a negligible off-diagonal.
        int m = n;
        if (l1 <= n - 1)
        {
            for (int mm = l1; mm <= n - 1; ++mm)
            {
                const R tst = std::abs(e[mm - 1]);
                if (tst == R{0})
                {
                    m = mm;
                    break;
                }
                if (tst <= (std::sqrt(std::abs(d[mm - 1])) * std::sqrt(std::abs(d[mm]))) * eps)
                {
                    e[mm - 1] = R{0};
                    m = mm;
                    break;
                }
                m = n;  // no split found yet
            }
        }

        int l = l1;
        int lsv = l;
        int lend = m;
        int lendsv = lend;
        l1 = m + 1;
        if (lend == l)
        {
            continue;  // 1x1 block — eigenvalue already in d[l-1].
        }

        // Block norm (dlanst 'M' = max abs over d[l..lend], e[l..lend-1]).
        R anorm = R{0};
        for (int i = l; i <= lend; ++i)
        {
            anorm = anorm > std::abs(d[i - 1]) ? anorm : std::abs(d[i - 1]);
        }
        for (int i = l; i <= lend - 1; ++i)
        {
            anorm = anorm > std::abs(e[i - 1]) ? anorm : std::abs(e[i - 1]);
        }
        int iscale = 0;
        if (anorm == R{0})
        {
            continue;
        }
        if (anorm > ssfmax)
        {
            iscale = 1;
            const R mul = ssfmax / anorm;
            for (int i = l; i <= lend; ++i)
            {
                d[i - 1] *= mul;
            }
            for (int i = l; i <= lend - 1; ++i)
            {
                e[i - 1] *= mul;
            }
        }
        else if (anorm < ssfmin)
        {
            iscale = 2;
            const R mul = ssfmin / anorm;
            for (int i = l; i <= lend; ++i)
            {
                d[i - 1] *= mul;
            }
            for (int i = l; i <= lend - 1; ++i)
            {
                e[i - 1] *= mul;
            }
        }

        // Choose QL (top diagonal smaller) or QR.
        if (std::abs(d[lend - 1]) < std::abs(d[l - 1]))
        {
            lend = lsv;
            l = lendsv;
        }

        if (lend > l)
        {
            // ---- QL iteration ----
            while (true)
            {
                // Look for a small sub-diagonal element.
                int mm = lend;
                if (l != lend)
                {
                    for (int i = l; i <= lend - 1; ++i)
                    {
                        const R tst = e[i - 1] * e[i - 1];
                        if (tst <= (eps2 * std::abs(d[i - 1])) * std::abs(d[i]) + safmin)
                        {
                            mm = i;
                            break;
                        }
                        mm = lend;
                    }
                }
                if (mm < lend)
                {
                    e[mm - 1] = R{0};
                }
                R p = d[l - 1];
                if (mm == l)
                {
                    // Eigenvalue found.
                    d[l - 1] = p;
                    ++l;
                    if (l <= lend)
                    {
                        continue;
                    }
                    break;
                }
                if (mm == l + 1)
                {
                    // 2x2 block.
                    if (want_vectors)
                    {
                        const Eig2<R> ev = laev2(d[l - 1], e[l - 1], d[l]);
                        rot_cols<R, Z>(z, ldz, n_, static_cast<crd::usize>(l - 1),
                                       static_cast<crd::usize>(l), ev.cs1, ev.sn1);
                        d[l - 1] = ev.rt1;
                        d[l] = ev.rt2;
                    }
                    else
                    {
                        const Eig2<R> ev = lae2(d[l - 1], e[l - 1], d[l]);
                        d[l - 1] = ev.rt1;
                        d[l] = ev.rt2;
                    }
                    e[l - 1] = R{0};
                    l += 2;
                    if (l <= lend)
                    {
                        continue;
                    }
                    break;
                }

                if (jtot == nmaxit)
                {
                    break;
                }
                ++jtot;

                // Form shift (Wilkinson). sign(r,g) = |r| with the sign of g.
                R g = (d[l] - p) / (R{2} * e[l - 1]);
                R r = lapy2(g, R{1});
                g = d[mm - 1] - p + (e[l - 1] / (g + (g >= R{0} ? std::abs(r) : -std::abs(r))));

                R s = R{1};
                R c = R{1};
                p = R{0};

                // Inner loop: i = mm-1 down to l.
                for (int i = mm - 1; i >= l; --i)
                {
                    R f = s * e[i - 1];
                    const R b = c * e[i - 1];
                    const Givens<R> rot = lartg(g, f);
                    c = rot.c;
                    s = rot.s;
                    r = rot.r;
                    if (i != mm - 1)
                    {
                        e[i] = r;
                    }
                    g = d[i] - p;
                    r = (d[i - 1] - g) * s + R{2} * c * b;
                    p = s * r;
                    d[i] = g + p;
                    g = c * r - b;
                    if (want_vectors)
                    {
                        // QL stores sine as -s; columns (i-1, i) 0-based.
                        rot_cols<R, Z>(z, ldz, n_, static_cast<crd::usize>(i - 1),
                                       static_cast<crd::usize>(i), c, -s);
                    }
                }

                d[l - 1] -= p;
                e[l - 1] = g;
                // loop back to QL top
            }
        }
        else
        {
            // ---- QR iteration ----
            while (true)
            {
                // Look for a small super-diagonal element.
                int mm = lend;
                if (l != lend)
                {
                    for (int i = l; i >= lend + 1; --i)
                    {
                        const R tst = e[i - 2] * e[i - 2];
                        if (tst <= (eps2 * std::abs(d[i - 1])) * std::abs(d[i - 2]) + safmin)
                        {
                            mm = i;
                            break;
                        }
                        mm = lend;
                    }
                }
                if (mm > lend)
                {
                    e[mm - 2] = R{0};
                }
                R p = d[l - 1];
                if (mm == l)
                {
                    d[l - 1] = p;
                    --l;
                    if (l >= lend)
                    {
                        continue;
                    }
                    break;
                }
                if (mm == l - 1)
                {
                    if (want_vectors)
                    {
                        const Eig2<R> ev = laev2(d[l - 2], e[l - 2], d[l - 1]);
                        rot_cols<R, Z>(z, ldz, n_, static_cast<crd::usize>(l - 2),
                                       static_cast<crd::usize>(l - 1), ev.cs1, ev.sn1);
                        d[l - 2] = ev.rt1;
                        d[l - 1] = ev.rt2;
                    }
                    else
                    {
                        const Eig2<R> ev = lae2(d[l - 2], e[l - 2], d[l - 1]);
                        d[l - 2] = ev.rt1;
                        d[l - 1] = ev.rt2;
                    }
                    e[l - 2] = R{0};
                    l -= 2;
                    if (l >= lend)
                    {
                        continue;
                    }
                    break;
                }

                if (jtot == nmaxit)
                {
                    break;
                }
                ++jtot;

                // Form shift.
                R g;
                {
                    const R gg = (d[l - 2] - p) / (R{2} * e[l - 2]);
                    const R rr = lapy2(gg, R{1});
                    g = d[mm - 1] - p + (e[l - 2] / (gg + (gg >= R{0} ? std::abs(rr) : -std::abs(rr))));
                }

                R s = R{1};
                R c = R{1};
                p = R{0};

                // Inner loop: i = mm to l-1.
                for (int i = mm; i <= l - 1; ++i)
                {
                    R f = s * e[i - 1];
                    const R b = c * e[i - 1];
                    const Givens<R> rot = lartg(g, f);
                    c = rot.c;
                    s = rot.s;
                    R r = rot.r;
                    if (i != mm)
                    {
                        e[i - 2] = r;
                    }
                    g = d[i - 1] - p;
                    r = (d[i] - g) * s + R{2} * c * b;
                    p = s * r;
                    d[i - 1] = g + p;
                    g = c * r - b;
                    if (want_vectors)
                    {
                        // QR stores sine as +s; columns (i-1, i) 0-based.
                        rot_cols<R, Z>(z, ldz, n_, static_cast<crd::usize>(i - 1),
                                       static_cast<crd::usize>(i), c, s);
                    }
                }

                d[l - 1] -= p;
                e[l - 2] = g;
            }
        }

        // Undo scaling for this block.
        if (iscale == 1)
        {
            const R mul = anorm / ssfmax;
            for (int i = lendsv; i <= lsv; ++i)
            {
                d[i - 1] *= mul;
            }
            for (int i = lendsv; i <= lsv - 1; ++i)
            {
                e[i - 1] *= mul;
            }
        }
        else if (iscale == 2)
        {
            const R mul = anorm / ssfmin;
            for (int i = lendsv; i <= lsv; ++i)
            {
                d[i - 1] *= mul;
            }
            for (int i = lendsv; i <= lsv - 1; ++i)
            {
                e[i - 1] *= mul;
            }
        }

        if (jtot >= nmaxit)
        {
            break;
        }
    }

    // Count unconverged off-diagonals (INFO).
    int info = 0;
    for (int i = 0; i < n - 1; ++i)
    {
        if (e[i] != R{0})
        {
            ++info;
        }
    }
    return info;
}

namespace
{
// Tridiagonalization block size (panel width) for the blocked dsytrd path.
constexpr crd::usize kTridiagBlock = 32;

// reduce_sub_unblocked — faithful dsytd2 'L' on the sub-matrix A(kk:kk+nn,
// kk:kk+nn). Writes d[kk..], e[kk..], tau[kk..] + reflectors into the lower
// triangle. Scratch v/x/w must each hold >= nn entries. This is the last-
// block handler for the blocked driver AND the correctness oracle.
template <typename T>
void reduce_sub_unblocked(T* a, crd::usize lda, crd::usize kk, crd::usize nn, RealType<T>* d,
                          RealType<T>* e, T* tau, T* v, T* x, T* w)
{
    if (nn == 0)
    {
        return;
    }
    if (nn == 1)
    {
        d[kk] = a[kk * lda + kk];
        return;
    }
    auto sym_at = [&](crd::usize i, crd::usize j) -> T {
        const crd::usize r = i >= j ? i : j;
        const crd::usize c = i >= j ? j : i;
        return a[(kk + r) * lda + (kk + c)];
    };
    for (crd::usize i = 0; i + 1 < nn; ++i)
    {
        const crd::usize m = nn - i - 1;
        for (crd::usize t = 0; t < m; ++t)
        {
            v[t] = a[(kk + i + 1 + t) * lda + (kk + i)];
        }
        const detail::Householder<T> h = detail::make_householder<T>(v, m);
        e[kk + i] = h.beta;
        tau[kk + i] = h.tau;
        v[0] = T{1};
        if (h.tau != T{0})
        {
            for (crd::usize r = 0; r < m; ++r)
            {
                T acc{};
                for (crd::usize c = 0; c < m; ++c)
                {
                    acc += sym_at(i + 1 + r, i + 1 + c) * v[c];
                }
                x[r] = h.tau * acc;
            }
            T xv{};
            for (crd::usize t = 0; t < m; ++t)
            {
                xv += x[t] * v[t];
            }
            const T alpha2 = T{-0.5} * h.tau * xv;
            for (crd::usize t = 0; t < m; ++t)
            {
                w[t] = x[t] + alpha2 * v[t];
            }
            for (crd::usize r = 0; r < m; ++r)
            {
                for (crd::usize c = 0; c <= r; ++c)
                {
                    a[(kk + i + 1 + r) * lda + (kk + i + 1 + c)] -= v[r] * w[c] + w[r] * v[c];
                }
            }
        }
        for (crd::usize t = 1; t < m; ++t)
        {
            a[(kk + i + 1 + t) * lda + (kk + i)] = v[t];
        }
        a[(kk + i + 1) * lda + (kk + i)] = h.beta;
        d[kk + i] = a[(kk + i) * lda + (kk + i)];
    }
    d[kk + nn - 1] = a[(kk + nn - 1) * lda + (kk + nn - 1)];
}

// dlatrd_lower — faithful LAPACK dlatrd 'L'. Reduces the first `kb` columns
// of the sub-matrix A(kk:kk+nn, kk:kk+nn) to tridiagonal form and builds the
// nn x kb matrix W (leading dimension ldw) such that the trailing block can
// be updated as  A22 -= V*W^T + W*V^T. Leaves the reflectors' explicit unit
// heads in place (the driver restores the sub-diagonals from E after the
// trailing update, exactly as dsytrd does). v/tmp scratch: v >= nn, tmp >= kb.
template <typename T>
void dlatrd_lower(T* a, crd::usize lda, crd::usize kk, crd::usize nn, crd::usize kb,
                  RealType<T>* e, T* tau, T* w, crd::usize ldw, T* v, T* tmp, T* wc)
{
    auto aref = [&](crd::usize r, crd::usize c) -> T& {  // requires r >= c (lower-stored)
        return a[(kk + r) * lda + (kk + c)];
    };

    for (crd::usize i = 0; i < kb; ++i)
    {
        // Deferred update of the current column A_sub(i:nn, i) from prior
        // panel columns 0..i-1.
        if (i > 0)
        {
            for (crd::usize r = i; r < nn; ++r)
            {
                T acc{};
                for (crd::usize c = 0; c < i; ++c)
                {
                    acc += aref(r, c) * w[i * ldw + c];
                }
                for (crd::usize c = 0; c < i; ++c)
                {
                    acc += w[r * ldw + c] * aref(i, c);
                }
                aref(r, i) -= acc;
            }
        }
        if (i + 1 >= nn)
        {
            continue;
        }
        const crd::usize m = nn - i - 1;  // reflector length / trailing sub-dim

        for (crd::usize t = 0; t < m; ++t)
        {
            v[t] = aref(i + 1 + t, i);
        }
        const detail::Householder<T> h = detail::make_householder<T>(v, m);
        e[kk + i] = h.beta;
        tau[kk + i] = h.tau;
        v[0] = T{1};
        for (crd::usize t = 1; t < m; ++t)
        {
            aref(i + 1 + t, i) = v[t];
        }
        aref(i + 1, i) = T{1};  // explicit unit head (restored to E by the driver)

        // wc (contiguous) = A_sub(i+1:nn, i+1:nn) * v  via a single-pass
        // symmetric matvec on the lower triangle (each lower element touched
        // once; contiguous SIMD dot + axpy along each lower row).
        for (crd::usize rp = 0; rp < m; ++rp)
        {
            wc[rp] = T{0};
        }
        for (crd::usize rp = 0; rp < m; ++rp)
        {
            const T* row = &a[(kk + i + 1 + rp) * lda + (kk + i + 1)];  // lower row [0..rp]
            const T vr = v[rp];
            wc[rp] += simd_dot<T>(row, v, rp) + row[rp] * vr;  // lower-dot + diagonal
            simd_axpy<T>(wc, row, vr, rp);                     // symmetric upper contributions
        }
        if (i > 0)
        {
            // wc -= A_sub(i+1:nn, 0:i) * (W(i+1:nn, 0:i)^T * v)
            for (crd::usize c = 0; c < i; ++c)
            {
                T acc{};
                for (crd::usize rp = 0; rp < m; ++rp)
                {
                    acc += w[(i + 1 + rp) * ldw + c] * v[rp];
                }
                tmp[c] = acc;
            }
            for (crd::usize rp = 0; rp < m; ++rp)
            {
                T acc{};
                for (crd::usize c = 0; c < i; ++c)
                {
                    acc += aref(i + 1 + rp, c) * tmp[c];
                }
                wc[rp] -= acc;
            }
            // wc -= W(i+1:nn, 0:i) * (A_sub(i+1:nn, 0:i)^T * v)
            for (crd::usize c = 0; c < i; ++c)
            {
                T acc{};
                for (crd::usize rp = 0; rp < m; ++rp)
                {
                    acc += aref(i + 1 + rp, c) * v[rp];
                }
                tmp[c] = acc;
            }
            for (crd::usize rp = 0; rp < m; ++rp)
            {
                T acc{};
                for (crd::usize c = 0; c < i; ++c)
                {
                    acc += w[(i + 1 + rp) * ldw + c] * tmp[c];
                }
                wc[rp] -= acc;
            }
        }
        const T tau_i = tau[kk + i];
        for (crd::usize rp = 0; rp < m; ++rp)
        {
            wc[rp] *= tau_i;
        }
        const T wv = simd_dot<T>(wc, v, m);
        const T alpha = T{-0.5} * tau_i * wv;
        simd_axpy<T>(wc, v, alpha, m);
        // Scatter the finished w-column into W (strided column i).
        for (crd::usize rp = 0; rp < m; ++rp)
        {
            w[(i + 1 + rp) * ldw + i] = wc[rp];
        }
    }
}
} // namespace

// =======================================================================
// tridiagonalize — PRODUCTION blocked dsytrd 'L' (D(dense-eig)-1). Panels
// reduced by dlatrd; the trailing block updated by ONE gemm_parallel
// P = V*W^T (the optimized BLAS3 path — our syr2k/syrk are scalar shells)
// then A_lower -= P + P^T (minimal-flop symmetric rank-2k). Last block via
// the unblocked dsytd2 oracle. Beats Eigen's single-thread BLAS2 reduction;
// the half-flop triangular-syr2k refinement (for real-BLAS LAPACK at scale)
// is filed as v3a-1-perf.
// =======================================================================
template <typename T>
void tridiagonalize(T* a, crd::usize n, crd::usize lda, RealType<T>* d, RealType<T>* e, T* tau,
                    crd::memory::IAllocator* scratch)
{
    if (n == 0)
    {
        return;
    }
    if (n == 1)
    {
        d[0] = a[0];
        return;
    }

    crd::containers::Array<T> vv(scratch);
    crd::containers::Array<T> xx(scratch);
    crd::containers::Array<T> ww(scratch);
    vv.resize(n);
    xx.resize(n);
    ww.resize(n);

    const crd::usize nb = kTridiagBlock;
    if (n <= 2 * nb)
    {
        reduce_sub_unblocked<T>(a, lda, 0, n, d, e, tau, vv.data(), xx.data(), ww.data());
        return;
    }

    crd::containers::Array<T> wbuf(scratch);   // panel W: n x nb (ld = nb)
    crd::containers::Array<T> tmpbuf(scratch);  // length nb
    crd::containers::Array<T> pbuf(scratch);    // trailing P = V*W^T: m x m
    wbuf.resize(n * nb);
    tmpbuf.resize(nb);
    const crd::usize mmax = n - nb;
    pbuf.resize(mmax * mmax);

    crd::usize k = 0;
    while (k + nb < n)
    {
        const crd::usize nn = n - k;
        const crd::usize kb = nb;
        dlatrd_lower<T>(a, lda, k, nn, kb, e, tau, wbuf.data(), nb, vv.data(), tmpbuf.data(),
                        xx.data());

        const crd::usize m = nn - kb;  // trailing dimension (>= 1 since k+nb < n)
        MatrixView<const T, Layout::RowMajor> v_view{a + (k + kb) * lda + k, m, kb, lda};
        MatrixView<const T, Layout::RowMajor> w_view{wbuf.data() + kb * nb, m, kb, nb};
        MatrixView<T, Layout::RowMajor> p_view{pbuf.data(), m, m, m};
        gemm_parallel_auto<T, Layout::RowMajor>(T{1}, v_view, w_view, T{0}, p_view, Trans::None,
                                                Trans::Transpose, scratch);
        // A_lower(k+kb+i, k+kb+j) -= P[i][j] + P[j][i]   (j <= i; symmetric rank-2k).
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                a[(k + kb + i) * lda + (k + kb + j)] -= pbuf[i * m + j] + pbuf[j * m + i];
            }
        }
        // Restore panel sub-diagonals from E; record panel diagonals.
        for (crd::usize i = 0; i < kb; ++i)
        {
            a[(k + i + 1) * lda + (k + i)] = e[k + i];
            d[k + i] = a[(k + i) * lda + (k + i)];
        }
        k += kb;
    }

    reduce_sub_unblocked<T>(a, lda, k, n - k, d, e, tau, vv.data(), xx.data(), ww.data());
}

// =======================================================================
// form_q — build the orthogonal reduction matrix Q (dorgtr 'L') from the
// reflectors stored in the lower triangle of `a` + `tau`, into z stored
// COLUMN-MAJOR (z[c*ldz + r]) so the inner loops are contiguous. Q = H(0)
// H(1) ... H(n-2). Eigenvector back-transform seeds steqr with Q.
// =======================================================================
template <typename T>
void form_q(const T* a, crd::usize n, crd::usize lda, const T* tau, T* z, crd::usize ldz,
            crd::memory::IAllocator* scratch)
{
    // z := I (column-major)
    for (crd::usize c = 0; c < n; ++c)
    {
        T* col = z + c * ldz;
        for (crd::usize r = 0; r < n; ++r)
        {
            col[r] = (r == c) ? T{1} : T{0};
        }
    }
    if (n <= 1)
    {
        return;
    }

    crd::containers::Array<T> vv(scratch);
    crd::containers::Array<T> ww(scratch);
    vv.resize(n);
    ww.resize(n);
    T* v = vv.data();
    T* w = ww.data();

    for (crd::usize ii = n - 1; ii-- > 0;)  // ii = n-2 .. 0
    {
        const crd::usize i = ii;
        if (tau[i] == T{0})
        {
            continue;
        }
        // v: v[i+1]=1, v[i+2..n-1] from column i of the reflector store.
        v[i + 1] = T{1};
        for (crd::usize r = i + 2; r < n; ++r)
        {
            v[r] = a[r * lda + i];
        }
        const crd::usize off = i + 1;
        const crd::usize len = n - off;
        // w[c] = v[off:]^T * Q[off:, c]  (column c contiguous).
        for (crd::usize c = 0; c < n; ++c)
        {
            w[c] = simd_dot<T>(v + off, z + c * ldz + off, len);
        }
        // Q[off:, c] -= (tau[i] * w[c]) * v[off:]   (contiguous axpy per column).
        for (crd::usize c = 0; c < n; ++c)
        {
            simd_axpy<T>(z + c * ldz + off, v + off, -(tau[i] * w[c]), len);
        }
    }
}

// =======================================================================
// rank1_eigensolve — eigendecomposition of diag(d) + rho*z*z^T (D&C conquer).
// =======================================================================
template <typename T>
void rank1_eigensolve(crd::memory::IAllocator* alloc, crd::usize n, const T* d_in, const T* z_in,
                      T rho, const T* q_in, T* lambda_out, T* v_out)
{
    if (n == 0)
    {
        return;
    }
    if (n == 1)
    {
        lambda_out[0] = d_in[0] + rho * z_in[0] * z_in[0];
        v_out[0] = (q_in != nullptr) ? q_in[0] : T{1};
        return;
    }

    // rho < 0: solve M' = diag(-d) + (-rho)*z*z^T (rho'>0), whose eigenvalues are
    // -lambda(M) and eigenvectors are identical. One-level negate-and-reverse.
    if (rho < T{0})
    {
        crd::containers::Array<T> nd(alloc);
        crd::containers::Array<T> mu(alloc);
        crd::containers::Array<T> vt(alloc);
        nd.resize(n);
        mu.resize(n);
        vt.resize(n * n);
        for (crd::usize j = 0; j < n; ++j)
        {
            nd[j] = -d_in[j];
        }
        rank1_eigensolve<T>(alloc, n, nd.data(), z_in, -rho, q_in, mu.data(), vt.data());
        for (crd::usize k = 0; k < n; ++k)
        {
            lambda_out[k] = -mu[n - 1 - k];  // ascending of -mu = reverse of mu-ascending
            for (crd::usize r = 0; r < n; ++r)
            {
                v_out[r * n + k] = vt[r * n + (n - 1 - k)];
            }
        }
        return;
    }

    const T eps = std::numeric_limits<T>::epsilon();
    T znorm2 = T{0};
    for (crd::usize j = 0; j < n; ++j)
    {
        znorm2 += z_in[j] * z_in[j];
    }

    // Index sort of d ascending (perm[k] = original index of the k-th smallest).
    crd::containers::Array<crd::usize> perm(alloc);
    perm.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        perm[k] = k;
    }
    for (crd::usize a = 0; a + 1 < n; ++a)
    {
        crd::usize best = a;
        for (crd::usize b = a + 1; b < n; ++b)
        {
            if (d_in[perm[b]] < d_in[perm[best]])
            {
                best = b;
            }
        }
        const crd::usize tmp = perm[a];
        perm[a] = perm[best];
        perm[best] = tmp;
    }

    // V (n×n RowMajor): column k initialized to Q's column perm[k] (or e_{perm[k]}
    // when q_in is null). Subsequent deflation Givens + Löwner gemm then fuse the
    // Q*V back-transform into a single pass.
    if (q_in != nullptr)
    {
        for (crd::usize r = 0; r < n; ++r)
        {
            for (crd::usize k = 0; k < n; ++k)
            {
                v_out[r * n + k] = q_in[r * n + perm[k]];
            }
        }
    }
    else
    {
        for (crd::usize r = 0; r < n * n; ++r)
        {
            v_out[r] = T{0};
        }
        for (crd::usize k = 0; k < n; ++k)
        {
            v_out[perm[k] * n + k] = T{1};
        }
    }

    // Degenerate: rho==0 or z==0 ⇒ already diagonal in the sorted basis (V already
    // holds Q's columns in sorted order; just emit the sorted eigenvalues).
    if (rho == T{0} || znorm2 == T{0})
    {
        for (crd::usize k = 0; k < n; ++k)
        {
            lambda_out[k] = d_in[perm[k]];
        }
        return;
    }

    const T znorm = std::sqrt(znorm2);
    const T rhop = rho * znorm2;  // secular eqn for diag(ds) + rhop*zs*zs^T, ||zs||=1

    crd::containers::Array<T> ds(alloc);
    crd::containers::Array<T> zs(alloc);
    ds.resize(n);
    zs.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        ds[k] = d_in[perm[k]];
        zs[k] = z_in[perm[k]] / znorm;
    }

    T dmax = T{0};
    for (crd::usize k = 0; k < n; ++k)
    {
        dmax = dmax > std::abs(ds[k]) ? dmax : std::abs(ds[k]);
    }
    const T tol = T{8} * eps * (dmax > T{1} ? dmax : T{1});

    // Deflation (faithful dlaed2): negligible-weight + equal-pole Givens.
    crd::containers::Array<crd::u8> deflated(alloc);
    deflated.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        deflated[k] = 0;
    }
    long pj = -1;
    for (crd::usize k = 0; k < n; ++k)
    {
        if (rhop * std::abs(zs[k]) <= tol)
        {
            deflated[k] = 1;  // negligible weight ⇒ ds[k] is already an eigenvalue
            continue;
        }
        if (pj < 0)
        {
            pj = static_cast<long>(k);
            continue;
        }
        const crd::usize p = static_cast<crd::usize>(pj);
        const T zp = zs[p];
        const T zc = zs[k];
        const T tau = detail::hypot2(zc, zp);
        // dlaed2 convention: c = z_nj/tau, s = -z_pj/tau ⇒ the deflated column
        // Q_pj' = c*Q_pj + s*Q_nj satisfies z^T·Q_pj' = 0 (its z-weight vanishes).
        const T c = zc / tau;
        const T s = -zp / tau;
        const T t = ds[k] - ds[p];
        if (std::abs(t * c * s) <= tol)
        {
            // Equal-pole deflation: rotate V columns p,k so zs[p] → 0.
            for (crd::usize r = 0; r < n; ++r)
            {
                const T vp = v_out[r * n + p];
                const T vk = v_out[r * n + k];
                v_out[r * n + p] = c * vp + s * vk;
                v_out[r * n + k] = c * vk - s * vp;
            }
            zs[k] = tau;
            zs[p] = T{0};
            const T newdp = ds[p] * c * c + ds[k] * s * s;
            ds[k] = ds[p] * s * s + ds[k] * c * c;
            ds[p] = newdp;
            deflated[p] = 1;
            pj = static_cast<long>(k);
        }
        else
        {
            pj = static_cast<long>(k);
        }
    }

    // Collect non-deflated slots, re-sorted ascending by ds (Givens may reorder).
    crd::containers::Array<crd::usize> kept(alloc);
    for (crd::usize k = 0; k < n; ++k)
    {
        if (deflated[k] == 0)
        {
            kept.push_back(k);
        }
    }
    const crd::usize big_k = kept.size();
    for (crd::usize a = 0; a + 1 < big_k; ++a)
    {
        crd::usize best = a;
        for (crd::usize b = a + 1; b < big_k; ++b)
        {
            if (ds[kept[b]] < ds[kept[best]])
            {
                best = b;
            }
        }
        const crd::usize tmp = kept[a];
        kept[a] = kept[best];
        kept[best] = tmp;
    }

    crd::containers::Array<T> evals(alloc);
    evals.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        evals[k] = ds[k];  // deflated slots take their (possibly rotated) ds
    }

    if (big_k > 0)
    {
        crd::containers::Array<T> dnd(alloc);
        crd::containers::Array<T> znd(alloc);
        dnd.resize(big_k);
        znd.resize(big_k);
        for (crd::usize a = 0; a < big_k; ++a)
        {
            dnd[a] = ds[kept[a]];
            znd[a] = zs[kept[a]];
        }
        // Solve the big_k secular roots; store delta_mat[i*K+a] = dnd[a] - lam_i.
        crd::containers::Array<T> lam(alloc);
        crd::containers::Array<T> deltam(alloc);
        crd::containers::Array<T> dtmp(alloc);
        lam.resize(big_k);
        deltam.resize(big_k * big_k);
        dtmp.resize(big_k);
        for (crd::usize i = 0; i < big_k; ++i)
        {
            lam[i] = detail::secular_root<T>(static_cast<int>(i), static_cast<int>(big_k),
                                             dnd.data(), znd.data(), rhop, dtmp.data());
            for (crd::usize a = 0; a < big_k; ++a)
            {
                deltam[i * big_k + a] = dtmp[a];
            }
        }
        // Löwner / Gu-Eisenstat reconstruction of ŵ (orthogonality-preserving).
        crd::containers::Array<T> what(alloc);
        what.resize(big_k);
        for (crd::usize a = 0; a < big_k; ++a)
        {
            // ŵ_a^2 = (lam_a - dnd_a) * prod_{j!=a} (lam_j - dnd_a)/(dnd_j - dnd_a).
            // INTERLEAVE numerator/denominator factors so the running product stays
            // O(1) (each ratio is bounded by the interlacing property). Computing
            // the two products separately overflows/underflows for large K
            // (∏ of ~K factors) → NaN — the dlaed3 stable form avoids it.
            // deltam[i*K+a] = dnd[a] - lam_i, so (lam_i - dnd_a) = -deltam[i*K+a].
            T w2 = T{1};
            for (crd::usize j = 0; j < big_k; ++j)
            {
                if (j == a)
                {
                    w2 *= -deltam[a * big_k + a];  // (lam_a - dnd_a), no denominator
                }
                else
                {
                    w2 *= (-deltam[j * big_k + a]) / (dnd[j] - dnd[a]);
                }
            }
            const T w = std::sqrt(std::abs(w2));
            what[a] = (znd[a] < T{0}) ? -w : w;
        }
        // Secular eigenvectors: vsec[a*K+i] = what[a]/delta_i[a], column-normalized.
        crd::containers::Array<T> vsec(alloc);
        vsec.resize(big_k * big_k);
        for (crd::usize i = 0; i < big_k; ++i)
        {
            T nrm2 = T{0};
            for (crd::usize a = 0; a < big_k; ++a)
            {
                const T u = what[a] / deltam[i * big_k + a];
                vsec[a * big_k + i] = u;
                nrm2 += u * u;
            }
            const T inv = T{1} / std::sqrt(nrm2);
            for (crd::usize a = 0; a < big_k; ++a)
            {
                vsec[a * big_k + i] *= inv;
            }
        }
        // Back-transform: result(n×K) = Vnd(n×K) · vsec(K×K)  via gemm_parallel.
        crd::containers::Array<T> vnd(alloc);
        crd::containers::Array<T> res(alloc);
        vnd.resize(n * big_k);
        res.resize(n * big_k);
        for (crd::usize r = 0; r < n; ++r)
        {
            for (crd::usize a = 0; a < big_k; ++a)
            {
                vnd[r * big_k + a] = v_out[r * n + kept[a]];
            }
        }
        MatrixView<const T, Layout::RowMajor> av{vnd.data(), n, big_k, big_k};
        MatrixView<const T, Layout::RowMajor> bv{vsec.data(), big_k, big_k, big_k};
        MatrixView<T, Layout::RowMajor> cv{res.data(), n, big_k, big_k};
        gemm_parallel_auto<T, Layout::RowMajor>(T{1}, av, bv, T{0}, cv, Trans::None, Trans::None,
                                                alloc);
        for (crd::usize r = 0; r < n; ++r)
        {
            for (crd::usize i = 0; i < big_k; ++i)
            {
                v_out[r * n + kept[i]] = res[r * big_k + i];
            }
        }
        for (crd::usize i = 0; i < big_k; ++i)
        {
            evals[kept[i]] = lam[i];
        }
    }

    // Final ascending sort across all slots + sign convention (D(dense-eig)-4).
    crd::containers::Array<crd::usize> order(alloc);
    order.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        order[k] = k;
    }
    for (crd::usize a = 0; a + 1 < n; ++a)
    {
        crd::usize best = a;
        for (crd::usize b = a + 1; b < n; ++b)
        {
            if (evals[order[b]] < evals[order[best]])
            {
                best = b;
            }
        }
        const crd::usize tmp = order[a];
        order[a] = order[best];
        order[best] = tmp;
    }
    crd::containers::Array<T> vtmp(alloc);
    vtmp.resize(n * n);
    for (crd::usize newc = 0; newc < n; ++newc)
    {
        const crd::usize oldc = order[newc];
        lambda_out[newc] = evals[oldc];
        crd::usize pivot = 0;
        T bestmag = T{};
        for (crd::usize r = 0; r < n; ++r)
        {
            const T av = std::abs(v_out[r * n + oldc]);
            if (av > bestmag)
            {
                bestmag = av;
                pivot = r;
            }
        }
        const T sign = (v_out[pivot * n + oldc] < T{0}) ? T{-1} : T{1};
        for (crd::usize r = 0; r < n; ++r)
        {
            vtmp[r * n + newc] = sign * v_out[r * n + oldc];
        }
    }
    for (crd::usize r = 0; r < n * n; ++r)
    {
        v_out[r] = vtmp[r];
    }
}

// =======================================================================
// dc_tridiag_eig — Cuppen divide-and-conquer on a symmetric tridiagonal
// (d, e). Cut at n/2 with a rank-1 correction, recurse to a steqr base case,
// merge with rank1_eigensolve (fused Q*V back-transform via gemm_parallel).
// Serial recursion + parallel merge gemm ⇒ deterministic + bit-identical
// across worker counts (the only parallelism is the deterministic gemm).
// On exit: lambda_out ascending, z_out n*n RowMajor (col k = eigvec k).
// =======================================================================
namespace
{
constexpr crd::usize kDcSmallSize = 32;  // D&C base-case threshold (LAPACK SMLSIZ-class)
// eig_sym dispatch crossover: QL/QR (steqr) wins/ties up to ~256 (tuned on the
// vs-reference bench: QL/QR 1.2–1.48x Eigen at n<=256; D&C 1.9–1.96x at n>=512),
// D&C wins decisively beyond. So route n>256 to D&C, smaller to QL/QR.
constexpr crd::usize kEigDcThreshold = 256;

// Base case: QL/QR (steqr) on a copy of (d,e), seeded with identity, then
// sorted ascending into row-major z_out.
template <typename T>
void dc_base_steqr(crd::memory::IAllocator* alloc, crd::usize n, const T* d, const T* e,
                   T* lambda_out, T* z_out)
{
    crd::containers::Array<T> dd(alloc);
    crd::containers::Array<T> ee(alloc);
    crd::containers::Array<T> zc(alloc);  // column-major eigenvectors for steqr
    dd.resize(n);
    ee.resize(n > 0 ? n - 1 : 0);
    zc.resize(n * n);
    for (crd::usize i = 0; i < n; ++i)
    {
        dd[i] = d[i];
    }
    for (crd::usize i = 0; i + 1 < n; ++i)
    {
        ee[i] = e[i];
    }
    for (crd::usize c = 0; c < n; ++c)
    {
        for (crd::usize r = 0; r < n; ++r)
        {
            zc[c * n + r] = (r == c) ? T{1} : T{0};
        }
    }
    [[maybe_unused]] const int info = steqr<T, T>(dd.data(), ee.data(), n, zc.data(), n, true);
    CRD_ASSERT_MSG(info == 0, "dc_base_steqr: steqr did not converge");

    crd::containers::Array<crd::usize> order(alloc);
    order.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        order[i] = i;
    }
    for (crd::usize a = 0; a + 1 < n; ++a)
    {
        crd::usize best = a;
        for (crd::usize b = a + 1; b < n; ++b)
        {
            if (dd[order[b]] < dd[order[best]])
            {
                best = b;
            }
        }
        const crd::usize tmp = order[a];
        order[a] = order[best];
        order[best] = tmp;
    }
    for (crd::usize newc = 0; newc < n; ++newc)
    {
        const crd::usize oldc = order[newc];
        lambda_out[newc] = dd[oldc];
        for (crd::usize r = 0; r < n; ++r)
        {
            z_out[r * n + newc] = zc[oldc * n + r];  // colmajor col oldc → rowmajor col newc
        }
    }
}
} // namespace

template <typename T>
void dc_tridiag_eig(crd::memory::IAllocator* alloc, crd::usize n, const T* d, const T* e,
                    T* lambda_out, T* z_out)
{
    if (n == 0)
    {
        return;
    }
    if (n == 1)
    {
        lambda_out[0] = d[0];
        z_out[0] = T{1};
        return;
    }
    if (n <= kDcSmallSize)
    {
        dc_base_steqr<T>(alloc, n, d, e, lambda_out, z_out);
        return;
    }

    const crd::usize m = n / 2;
    const crd::usize n2 = n - m;
    const T rho = e[m - 1];

    // Subproblem 1: d[0..m-1] with d[m-1] -= rho; e[0..m-2].
    crd::containers::Array<T> d1(alloc);
    crd::containers::Array<T> e1(alloc);
    crd::containers::Array<T> lam1(alloc);
    crd::containers::Array<T> z1(alloc);
    d1.resize(m);
    e1.resize(m - 1);
    lam1.resize(m);
    z1.resize(m * m);
    for (crd::usize j = 0; j < m; ++j)
    {
        d1[j] = d[j];
    }
    for (crd::usize j = 0; j + 1 < m; ++j)
    {
        e1[j] = e[j];
    }
    d1[m - 1] -= rho;
    dc_tridiag_eig<T>(alloc, m, d1.data(), e1.data(), lam1.data(), z1.data());

    // Subproblem 2: d[m..n-1] with d[m] -= rho; e[m..n-2].
    crd::containers::Array<T> d2(alloc);
    crd::containers::Array<T> e2(alloc);
    crd::containers::Array<T> lam2(alloc);
    crd::containers::Array<T> z2(alloc);
    d2.resize(n2);
    e2.resize(n2 - 1);
    lam2.resize(n2);
    z2.resize(n2 * n2);
    for (crd::usize j = 0; j < n2; ++j)
    {
        d2[j] = d[m + j];
    }
    for (crd::usize j = 0; j + 1 < n2; ++j)
    {
        e2[j] = e[m + j];
    }
    d2[0] -= rho;
    dc_tridiag_eig<T>(alloc, n2, d2.data(), e2.data(), lam2.data(), z2.data());

    // Merge: D = (lam1; lam2); z = (last row of Z1; first row of Z2); rank-1 rho.
    crd::containers::Array<T> dmerge(alloc);
    crd::containers::Array<T> zvec(alloc);
    crd::containers::Array<T> qmat(alloc);
    dmerge.resize(n);
    zvec.resize(n);
    qmat.resize(n * n);
    for (crd::usize j = 0; j < m; ++j)
    {
        dmerge[j] = lam1[j];
        zvec[j] = z1[(m - 1) * m + j];  // last row of Z1
    }
    for (crd::usize j = 0; j < n2; ++j)
    {
        dmerge[m + j] = lam2[j];
        zvec[m + j] = z2[j];  // first row of Z2 (= z2[0*n2 + j])
    }
    for (crd::usize r = 0; r < n * n; ++r)
    {
        qmat[r] = T{0};
    }
    for (crd::usize r = 0; r < m; ++r)
    {
        for (crd::usize c = 0; c < m; ++c)
        {
            qmat[r * n + c] = z1[r * m + c];
        }
    }
    for (crd::usize r = 0; r < n2; ++r)
    {
        for (crd::usize c = 0; c < n2; ++c)
        {
            qmat[(m + r) * n + (m + c)] = z2[r * n2 + c];
        }
    }
    rank1_eigensolve<T>(alloc, n, dmerge.data(), zvec.data(), rho, qmat.data(), lambda_out, z_out);
}

// =======================================================================
// eig_sym — the driver.
// =======================================================================
template <typename T>
EigSym<T> eig_sym(crd::memory::IAllocator* alloc, const Symmetric<T>& a)
{
    static_assert(!is_complex_v<T>, "eig_sym: real T only in v3a-1 (Hermitian is v3a-1b)");
    const crd::usize n = a.n();
    EigSym<T> out(alloc, n);
    if (n == 0)
    {
        return out;
    }
    if (n == 1)
    {
        out.values.data()[0] = a.data()[0];
        out.vectors.at(0, 0) = T{1};
        return out;
    }

    // Working full n×n buffer (lower triangle canonical), cloned from a.
    crd::containers::Array<T> work(alloc);
    work.resize(n * n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            work[i * n + j] = a.at(i, j);
        }
    }

    crd::containers::Array<T> d(alloc);
    crd::containers::Array<T> e(alloc);
    crd::containers::Array<T> tau(alloc);
    d.resize(n);
    e.resize(n > 0 ? n - 1 : 0);
    tau.resize(n > 0 ? n - 1 : 0);

    tridiagonalize<T>(work.data(), n, n, d.data(), e.data(), tau.data(), alloc);

    // ---- Divide-and-conquer path for large n (v3a-2.4) ----------------
    // Solve the tridiagonal by D&C, then back-transform V = Q_reduction * Z_tri
    // via a single gemm_parallel. (Small n keeps the QL/QR path below — its
    // steqr-rotates-Q accumulation avoids materializing Z_tri + the extra gemm.)
    if (n > kEigDcThreshold)
    {
        crd::containers::Array<T> qred(alloc);  // column-major Q_reduction
        crd::containers::Array<T> ztri(alloc);  // row-major tridiagonal eigenvectors
        qred.resize(n * n);
        ztri.resize(n * n);
        form_q<T>(work.data(), n, n, tau.data(), qred.data(), n, alloc);
        dc_tridiag_eig<T>(alloc, n, d.data(), e.data(), out.values.data(), ztri.data());

        // qred is column-major (qred[c*n+r] = Q[r][c]); a RowMajor view of it is
        // Q^T, so gemm with trans_a=Transpose yields V = Q * Z_tri.
        MatrixView<const T, Layout::RowMajor> qt_view{qred.data(), n, n, n};
        MatrixView<const T, Layout::RowMajor> zt_view{ztri.data(), n, n, n};
        MatrixView<T, Layout::RowMajor> v_view{out.vectors.data(), n, n, out.vectors.ld()};
        gemm_parallel_auto<T, Layout::RowMajor>(T{1}, qt_view, zt_view, T{0}, v_view,
                                                Trans::Transpose, Trans::None, alloc);

        // Sign convention (D(dense-eig)-4): lowest-index largest-magnitude positive.
        T* vd = out.vectors.data();
        const crd::usize ldv = out.vectors.ld();
        for (crd::usize c = 0; c < n; ++c)
        {
            crd::usize pivot = 0;
            T bestmag = T{};
            for (crd::usize r = 0; r < n; ++r)
            {
                const T av = std::abs(vd[r * ldv + c]);
                if (av > bestmag)
                {
                    bestmag = av;
                    pivot = r;
                }
            }
            if (vd[pivot * ldv + c] < T{0})
            {
                for (crd::usize r = 0; r < n; ++r)
                {
                    vd[r * ldv + c] = -vd[r * ldv + c];
                }
            }
        }
        return out;
    }

    // ---- QL/QR path (small n) -----------------------------------------
    // Eigenvectors accumulated COLUMN-MAJOR in a scratch buffer (zcol[c*n + r])
    // so the steqr Givens column-rotations are contiguous SIMD sweeps; the
    // final sort/sign pass transposes into the RowMajor output.
    crd::containers::Array<T> zcol(alloc);
    zcol.resize(n * n);
    form_q<T>(work.data(), n, n, tau.data(), zcol.data(), n, alloc);

    [[maybe_unused]] const int steqr_info = steqr<T, T>(d.data(), e.data(), n, zcol.data(), n, true);
    CRD_ASSERT_MSG(steqr_info == 0, "eig_sym: steqr did not converge");

    // Ascending sort via a permutation array applied once (D(dense-eig)-3).
    crd::containers::Array<crd::usize> perm(alloc);
    perm.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        perm[i] = i;
    }
    // Stable insertion sort by eigenvalue (n is small-ish here; selection is
    // O(n^2) but deterministic and bit-exact — the heavy work is the reduction).
    for (crd::usize i = 1; i < n; ++i)
    {
        const crd::usize key = perm[i];
        const T kv = d[key];
        crd::usize j = i;
        while (j > 0 && d[perm[j - 1]] > kv)
        {
            perm[j] = perm[j - 1];
            --j;
        }
        perm[j] = key;
    }

    // Transpose the sorted, sign-fixed eigenvectors from the column-major
    // scratch into the RowMajor output. Sign convention (D(dense-eig)-4):
    // lowest-index largest-magnitude component positive.
    T* vd = out.vectors.data();
    const crd::usize ldout = out.vectors.ld();
    for (crd::usize newc = 0; newc < n; ++newc)
    {
        const crd::usize oldc = perm[newc];
        out.values.data()[newc] = d[oldc];
        const T* col = zcol.data() + oldc * n;
        crd::usize pivot = 0;
        T best = T{};
        for (crd::usize r = 0; r < n; ++r)
        {
            const T av = std::abs(col[r]);
            if (av > best)
            {
                best = av;
                pivot = r;
            }
        }
        const T sign = (col[pivot] < T{0}) ? T{-1} : T{1};
        for (crd::usize r = 0; r < n; ++r)
        {
            vd[r * ldout + newc] = sign * col[r];
        }
    }

    return out;
}

// =======================================================================
// v3a-2.5 — complex Hermitian eigensolver (eig_herm).
// =======================================================================
namespace
{
template <typename R>
struct HouseholderC
{
    crd::hesap::Complex<R> tau;
    R beta;  // real (zlarfg makes beta real for the Hermitian reduction)
};

// Complex Householder (faithful zlarfg): H = I - tau*v*v^H, H*x = beta*e_0,
// beta real. On exit x[1..n-1] holds the v-tail (v[0]=1 implicit); x[0]
// unchanged. Includes the safmin rescaling guard.
template <typename R>
[[nodiscard]] HouseholderC<R> make_householder_herm(crd::hesap::Complex<R>* x,
                                                    crd::usize n) noexcept
{
    using C = crd::hesap::Complex<R>;
    if (n <= 1)
    {
        return HouseholderC<R>{C{R{0}, R{0}}, n == 1 ? x[0].re : R{0}};
    }
    R alphr = x[0].re;
    R alphi = x[0].im;
    R xnorm2 = R{0};
    for (crd::usize k = 1; k < n; ++k)
    {
        xnorm2 += x[k].re * x[k].re + x[k].im * x[k].im;
    }
    if (xnorm2 == R{0} && alphi == R{0})
    {
        return HouseholderC<R>{C{R{0}, R{0}}, alphr};
    }
    R beta = -(alphr >= R{0} ? R{1} : R{-1}) * std::sqrt(alphr * alphr + alphi * alphi + xnorm2);
    const R safmin = std::numeric_limits<R>::min() / std::numeric_limits<R>::epsilon();
    int knt = 0;
    if (std::abs(beta) < safmin)
    {
        const R rsafmn = R{1} / safmin;
        do
        {
            ++knt;
            for (crd::usize k = 1; k < n; ++k)
            {
                x[k].re *= rsafmn;
                x[k].im *= rsafmn;
            }
            beta *= rsafmn;
            alphi *= rsafmn;
            alphr *= rsafmn;
        } while (std::abs(beta) < safmin && knt < 20);
        xnorm2 = R{0};
        for (crd::usize k = 1; k < n; ++k)
        {
            xnorm2 += x[k].re * x[k].re + x[k].im * x[k].im;
        }
        beta = -(alphr >= R{0} ? R{1} : R{-1}) * std::sqrt(alphr * alphr + alphi * alphi + xnorm2);
    }
    const C tau{(beta - alphr) / beta, -alphi / beta};
    const C denom{alphr - beta, alphi};  // alpha - beta (beta real)
    for (crd::usize k = 1; k < n; ++k)
    {
        x[k] = x[k] / denom;
    }
    for (int j = 0; j < knt; ++j)
    {
        beta *= safmin;
    }
    return HouseholderC<R>{tau, beta};
}

// SIMD complex Hermitian tridiagonalization (zhetd2): reduce the Hermitian
// matrix in the lower triangle to a REAL symmetric tridiagonal (d, e) by
// Q^H A Q = T. The matrix is carried as two REAL arrays ar, ai (lower triangle,
// n×n, ld) so the Hermitian matvec and rank-2 update are contiguous-row real
// SIMD (simd_dot / simd_axpy) — no scalar complex arithmetic. Outputs real d, e
// + complex reflectors (stored back into ar/ai) + complex tau + the last-
// subdiagonal phase. `dphase` (length n, the diagonal-unitary phase) makes the
// last sub-diagonal real: the final m=1 step has no reflector, so its complex
// off-diagonal g is realized as |g| with phase g/|g| folded into Q (V=Q*D*Z).
template <typename R>
void tridiagonalize_hermitian_simd(R* ar, R* ai, crd::usize n, crd::usize lda, R* d, R* e,
                                   crd::hesap::Complex<R>* tau, crd::hesap::Complex<R>* dphase,
                                   crd::memory::IAllocator* sc)
{
    using C = crd::hesap::Complex<R>;
    for (crd::usize k = 0; k < n; ++k)
    {
        dphase[k] = C{R{1}, R{0}};
    }
    if (n <= 1)
    {
        if (n == 1)
        {
            d[0] = ar[0];
        }
        return;
    }
    crd::containers::Array<C> vc(sc);
    crd::containers::Array<R> vr(sc);
    crd::containers::Array<R> vi(sc);
    crd::containers::Array<R> xr(sc);
    crd::containers::Array<R> xi(sc);
    crd::containers::Array<R> wr(sc);
    crd::containers::Array<R> wi(sc);
    vc.resize(n);
    vr.resize(n);
    vi.resize(n);
    xr.resize(n);
    xi.resize(n);
    wr.resize(n);
    wi.resize(n);

    for (crd::usize i = 0; i + 1 < n; ++i)
    {
        const crd::usize m = n - i - 1;
        if (m == 1)
        {
            const C g{ar[(i + 1) * lda + i], ai[(i + 1) * lda + i]};
            const R mag = crd::hesap::abs(g);
            e[i] = mag;
            tau[i] = C{R{0}, R{0}};
            dphase[i + 1] = (mag > R{0}) ? g * (R{1} / mag) : C{R{1}, R{0}};
            ar[(i + 1) * lda + i] = mag;
            ai[(i + 1) * lda + i] = R{0};
            d[i] = ar[i * lda + i];
            continue;
        }
        for (crd::usize k = 0; k < m; ++k)
        {
            vc[k] = C{ar[(i + 1 + k) * lda + i], ai[(i + 1 + k) * lda + i]};
        }
        const HouseholderC<R> h = make_householder_herm<R>(vc.data(), m);
        e[i] = h.beta;
        tau[i] = h.tau;
        vc[0] = C{R{1}, R{0}};
        for (crd::usize k = 0; k < m; ++k)
        {
            vr[k] = vc[k].re;
            vi[k] = vc[k].im;
        }
        if (!(h.tau.re == R{0} && h.tau.im == R{0}))
        {
            for (crd::usize r = 0; r < m; ++r)
            {
                xr[r] = R{0};
                xi[r] = R{0};
            }
            // Av (single-pass Hermitian matvec on the lower triangle, SIMD rows).
            for (crd::usize r = 0; r < m; ++r)
            {
                const R* arow = &ar[(i + 1 + r) * lda + (i + 1)];
                const R* airow = &ai[(i + 1 + r) * lda + (i + 1)];
                const R diag = arow[r];  // real diagonal
                xr[r] += simd_dot<R>(arow, vr.data(), r) - simd_dot<R>(airow, vi.data(), r) +
                         diag * vr[r];
                xi[r] += simd_dot<R>(arow, vi.data(), r) + simd_dot<R>(airow, vr.data(), r) +
                         diag * vi[r];
                simd_axpy<R>(xr.data(), arow, vr[r], r);
                simd_axpy<R>(xr.data(), airow, vi[r], r);
                simd_axpy<R>(xi.data(), arow, vi[r], r);
                simd_axpy<R>(xi.data(), airow, -vr[r], r);
            }
            // x := tau * (A v).
            for (crd::usize r = 0; r < m; ++r)
            {
                const R xr0 = xr[r];
                const R xi0 = xi[r];
                xr[r] = h.tau.re * xr0 - h.tau.im * xi0;
                xi[r] = h.tau.re * xi0 + h.tau.im * xr0;
            }
            // xv = x^H v = sum conj(x) v.
            R xvr = R{0};
            R xvi = R{0};
            for (crd::usize k = 0; k < m; ++k)
            {
                xvr += xr[k] * vr[k] + xi[k] * vi[k];
                xvi += xr[k] * vi[k] - xi[k] * vr[k];
            }
            const C alpha = (h.tau * C{xvr, xvi}) * R{-0.5};
            for (crd::usize k = 0; k < m; ++k)
            {
                wr[k] = xr[k] + alpha.re * vr[k] - alpha.im * vi[k];
                wi[k] = xi[k] + alpha.re * vi[k] + alpha.im * vr[k];
            }
            // Hermitian rank-2: A -= v w^H + w v^H (lower rows, SIMD).
            for (crd::usize r = 0; r < m; ++r)
            {
                R* arow = &ar[(i + 1 + r) * lda + (i + 1)];
                R* airow = &ai[(i + 1 + r) * lda + (i + 1)];
                const crd::usize len = r + 1;
                simd_axpy<R>(arow, wr.data(), -vr[r], len);
                simd_axpy<R>(arow, wi.data(), -vi[r], len);
                simd_axpy<R>(arow, vr.data(), -wr[r], len);
                simd_axpy<R>(arow, vi.data(), -wi[r], len);
                simd_axpy<R>(airow, wr.data(), -vi[r], len);
                simd_axpy<R>(airow, wi.data(), vr[r], len);
                simd_axpy<R>(airow, vr.data(), -wi[r], len);
                simd_axpy<R>(airow, vi.data(), wr[r], len);
                ai[(i + 1 + r) * lda + (i + 1 + r)] = R{0};  // keep diagonal real
            }
        }
        for (crd::usize k = 1; k < m; ++k)
        {
            ar[(i + 1 + k) * lda + i] = vr[k];
            ai[(i + 1 + k) * lda + i] = vi[k];
        }
        ar[(i + 1) * lda + i] = h.beta;
        ai[(i + 1) * lda + i] = R{0};
        d[i] = ar[i * lda + i];
    }
    d[n - 1] = ar[(n - 1) * lda + (n - 1)];
}

// apply_q_zsplit — applies the Hermitian reduction Q to the eigenvectors. The
// transformed matrix is
// carried as two PERSISTENT real arrays (zr, zi) so every gemm is a pure real
// SIMD gemm with NO per-call complex split/recombine (the materialization that
// made cgemm_split a wash). V is split once per block; T is small/scalar.
// Reflectors are read directly from the SPLIT real arrays (ar, ai) the SIMD
// reduction produced — no complex materialization of the working matrix.
template <typename R>
void apply_q_zsplit(const R* ar, const R* ai, crd::usize n, crd::usize lda,
                    const crd::hesap::Complex<R>* tau, R* zr, R* zi, crd::usize ldz,
                    crd::memory::IAllocator* sc)
{
    using C = crd::hesap::Complex<R>;
    if (n <= 1)
    {
        return;
    }
    const crd::usize nb = 32;
    const crd::usize nrefl = n - 1;
    crd::containers::Array<R> vr(sc);
    crd::containers::Array<R> vi(sc);
    crd::containers::Array<R> vtvr(sc);
    crd::containers::Array<R> vtvi(sc);
    crd::containers::Array<R> trr(sc);
    crd::containers::Array<R> tii(sc);
    crd::containers::Array<R> wr(sc);
    crd::containers::Array<R> wi(sc);
    crd::containers::Array<R> w2r(sc);
    crd::containers::Array<R> w2i(sc);
    vr.resize(n * nb);
    vi.resize(n * nb);
    vtvr.resize(nb * nb);
    vtvi.resize(nb * nb);
    trr.resize(nb * nb);
    tii.resize(nb * nb);
    wr.resize(nb * n);
    wi.resize(nb * n);
    w2r.resize(nb * n);
    w2i.resize(nb * n);
    using MV = MatrixView<const R, Layout::RowMajor>;
    using MVm = MatrixView<R, Layout::RowMajor>;
    const Trans op_t = Trans::Transpose;
    const Trans op_n = Trans::None;

    const crd::usize first = ((nrefl - 1) / nb) * nb;
    for (long kbl = static_cast<long>(first); kbl >= 0; kbl -= static_cast<long>(nb))
    {
        const crd::usize kb = static_cast<crd::usize>(kbl);
        const crd::usize blk = (nb < nrefl - kb) ? nb : (nrefl - kb);
        for (crd::usize j = 0; j < blk; ++j)
        {
            const crd::usize i = kb + j;
            for (crd::usize r = 0; r < n; ++r)
            {
                if (r == i + 1)
                {
                    vr[r * nb + j] = R{1};
                    vi[r * nb + j] = R{0};
                }
                else if (r > i + 1)
                {
                    vr[r * nb + j] = ar[r * lda + i];
                    vi[r * nb + j] = ai[r * lda + i];
                }
                else
                {
                    vr[r * nb + j] = R{0};
                    vi[r * nb + j] = R{0};
                }
            }
        }
        MV vrv{vr.data(), n, blk, nb};
        MV viv{vi.data(), n, blk, nb};
        // vtv = V^H V:  re = Vr^T Vr + Vi^T Vi,  im = Vr^T Vi - Vi^T Vr.
        MVm vtvrv{vtvr.data(), blk, blk, nb};
        MVm vtviv{vtvi.data(), blk, blk, nb};
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, vrv, vrv, R{0}, vtvrv, op_t, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, viv, viv, R{1}, vtvrv, op_t, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, vrv, viv, R{0}, vtviv, op_t, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{-1}, viv, vrv, R{1}, vtviv, op_t, op_n, sc);
        // Compact-WY T (complex, scalar) from vtv + tau.
        for (crd::usize j = 0; j < blk; ++j)
        {
            const C tauj = tau[kb + j];
            for (crd::usize i = 0; i < j; ++i)
            {
                const C t = (C{R{0}, R{0}} - tauj) * C{vtvr[i * nb + j], vtvi[i * nb + j]};
                trr[i * nb + j] = t.re;
                tii[i * nb + j] = t.im;
            }
            trr[j * nb + j] = tauj.re;
            tii[j * nb + j] = tauj.im;
            for (crd::usize ii = 0; ii < j; ++ii)
            {
                C s{R{0}, R{0}};
                for (crd::usize l = ii; l < j; ++l)
                {
                    s += C{trr[ii * nb + l], tii[ii * nb + l]} * C{trr[l * nb + j], tii[l * nb + j]};
                }
                trr[ii * nb + j] = s.re;
                tii[ii * nb + j] = s.im;
            }
        }
        // W = V^H Z:  Wr = Vr^T Zr + Vi^T Zi,  Wi = Vr^T Zi - Vi^T Zr.
        MV zrv{zr, n, n, ldz};
        MV ziv{zi, n, n, ldz};
        MVm wrv{wr.data(), blk, n, n};
        MVm wiv{wi.data(), blk, n, n};
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, vrv, zrv, R{0}, wrv, op_t, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, viv, ziv, R{1}, wrv, op_t, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, vrv, ziv, R{0}, wiv, op_t, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{-1}, viv, zrv, R{1}, wiv, op_t, op_n, sc);
        // W2 = T W:  W2r = Tr Wr - Ti Wi,  W2i = Tr Wi + Ti Wr.
        MV trv{trr.data(), blk, blk, nb};
        MV tiv{tii.data(), blk, blk, nb};
        MV wrc{wr.data(), blk, n, n};
        MV wic{wi.data(), blk, n, n};
        MVm w2rv{w2r.data(), blk, n, n};
        MVm w2iv{w2i.data(), blk, n, n};
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, trv, wrc, R{0}, w2rv, op_n, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{-1}, tiv, wic, R{1}, w2rv, op_n, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, trv, wic, R{0}, w2iv, op_n, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, tiv, wrc, R{1}, w2iv, op_n, op_n, sc);
        // Z -= V W2:  Zr -= Vr W2r - Vi W2i,  Zi -= Vr W2i + Vi W2r.
        MV w2rc{w2r.data(), blk, n, n};
        MV w2ic{w2i.data(), blk, n, n};
        MVm zro{zr, n, n, ldz};
        MVm zio{zi, n, n, ldz};
        gemm_parallel_auto<R, Layout::RowMajor>(R{-1}, vrv, w2rc, R{1}, zro, op_n, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{1}, viv, w2ic, R{1}, zro, op_n, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{-1}, vrv, w2ic, R{1}, zio, op_n, op_n, sc);
        gemm_parallel_auto<R, Layout::RowMajor>(R{-1}, viv, w2rc, R{1}, zio, op_n, op_n, sc);
    }
}

} // namespace

template <typename C>
EigSym<C> eig_herm(crd::memory::IAllocator* alloc, const Hermitian<C>& a)
{
    static_assert(is_complex_v<C>, "eig_herm: complex C only");
    using R = RealType<C>;
    const crd::usize n = a.n();
    EigSym<C> out(alloc, n);
    if (n == 0)
    {
        return out;
    }
    if (n == 1)
    {
        out.values.data()[0] = a.data()[0].re;
        out.vectors.at(0, 0) = C{R{1}, R{0}};
        return out;
    }

    // Working buffer carried as TWO REAL arrays (ar, ai) for the lower triangle
    // so the reduction's Hermitian matvec + rank-2 update are contiguous-row real
    // SIMD (tridiagonalize_hermitian_simd). The reflectors land in ar/ai and the
    // back-transform reads them directly (no complex materialization).
    crd::containers::Array<R> ar(alloc);
    crd::containers::Array<R> ai(alloc);
    ar.resize(n * n);
    ai.resize(n * n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            const C v = a.at_value(i, j);  // const; lower triangle (i>=j)
            ar[i * n + j] = v.re;
            ai[i * n + j] = v.im;
        }
    }

    crd::containers::Array<R> d(alloc);
    crd::containers::Array<R> e(alloc);
    crd::containers::Array<C> tau(alloc);
    crd::containers::Array<C> dphase(alloc);
    d.resize(n);
    e.resize(n - 1);
    tau.resize(n - 1);
    dphase.resize(n);
    tridiagonalize_hermitian_simd<R>(ar.data(), ai.data(), n, n, d.data(), e.data(), tau.data(),
                                     dphase.data(), alloc);

    // Real solver on the tridiagonal (d, e): steqr (no gemm) for small n avoids
    // the D&C merge-gemm overhead (matches eig_sym's crossover); D&C for large n.
    crd::containers::Array<R> lam(alloc);
    crd::containers::Array<R> zr(alloc);  // real tridiagonal eigenvectors (RowMajor)
    lam.resize(n);
    zr.resize(n * n);
    if (n <= kEigDcThreshold)
    {
        dc_base_steqr<R>(alloc, n, d.data(), e.data(), lam.data(), zr.data());
    }
    else
    {
        dc_tridiag_eig<R>(alloc, n, d.data(), e.data(), lam.data(), zr.data());
    }

    // Complex back-transform V = Q*D*Z: form (D Z) (scale Z's row r by dphase[r]),
    // then apply the reduction reflectors Q via blocked compact-WY routed through
    // gemm_parallel — the fast path that replaces the O(n^3) scalar form_q.
    // (D Z) carried as two real arrays: zir = Re(dphase[r])*Z, zii = Im(dphase[r])*Z.
    crd::containers::Array<R> zir(alloc);
    crd::containers::Array<R> zii(alloc);
    zir.resize(n * n);
    zii.resize(n * n);
    for (crd::usize r = 0; r < n; ++r)
    {
        const R dr = dphase[r].re;
        const R di = dphase[r].im;
        for (crd::usize c = 0; c < n; ++c)
        {
            const R z = zr[r * n + c];
            zir[r * n + c] = dr * z;
            zii[r * n + c] = di * z;
        }
    }
    apply_q_zsplit<R>(ar.data(), ai.data(), n, n, tau.data(), zir.data(), zii.data(), n, alloc);

    // Assemble eigenvalues (ascending) + phase-normalized complex eigenvectors.
    const crd::usize ldout = out.vectors.ld();
    for (crd::usize c = 0; c < n; ++c)
    {
        out.values.data()[c] = lam[c];
        // Phase convention: lowest-index largest-magnitude component made real +.
        // Compare squared magnitudes (no sqrt); one sqrt for the winning pivot.
        crd::usize pivot = 0;
        R bestmag2 = R{};
        for (crd::usize r = 0; r < n; ++r)
        {
            const R m2 = zir[r * n + c] * zir[r * n + c] + zii[r * n + c] * zii[r * n + c];
            if (m2 > bestmag2)
            {
                bestmag2 = m2;
                pivot = r;
            }
        }
        C phase{R{1}, R{0}};
        if (bestmag2 > R{0})
        {
            const R bestmag = std::sqrt(bestmag2);
            phase = crd::hesap::conj(C{zir[pivot * n + c], zii[pivot * n + c]}) * (R{1} / bestmag);
        }
        for (crd::usize r = 0; r < n; ++r)
        {
            out.vectors.data()[r * ldout + c] = C{zir[r * n + c], zii[r * n + c]} * phase;
        }
    }
    return out;
}

// ---- explicit instantiations (v3a-1: real f32/f64) --------------------
template int steqr<float, float>(float*, float*, crd::usize, float*, crd::usize, bool);
template int steqr<double, double>(double*, double*, crd::usize, double*, crd::usize, bool);
template void tridiagonalize<float>(float*, crd::usize, crd::usize, float*, float*, float*,
                                    crd::memory::IAllocator*);
template void tridiagonalize<double>(double*, crd::usize, crd::usize, double*, double*, double*,
                                     crd::memory::IAllocator*);
template EigSym<float> eig_sym<float>(crd::memory::IAllocator*, const Symmetric<float>&);
template EigSym<double> eig_sym<double>(crd::memory::IAllocator*, const Symmetric<double>&);
template void rank1_eigensolve<float>(crd::memory::IAllocator*, crd::usize, const float*,
                                      const float*, float, const float*, float*, float*);
template void rank1_eigensolve<double>(crd::memory::IAllocator*, crd::usize, const double*,
                                       const double*, double, const double*, double*, double*);
template void dc_tridiag_eig<float>(crd::memory::IAllocator*, crd::usize, const float*, const float*,
                                    float*, float*);
template void dc_tridiag_eig<double>(crd::memory::IAllocator*, crd::usize, const double*,
                                     const double*, double*, double*);
template EigSym<crd::hesap::Complex<float>> eig_herm<crd::hesap::Complex<float>>(
    crd::memory::IAllocator*, const Hermitian<crd::hesap::Complex<float>>&);
template EigSym<crd::hesap::Complex<double>> eig_herm<crd::hesap::Complex<double>>(
    crd::memory::IAllocator*, const Hermitian<crd::hesap::Complex<double>>&);

} // namespace crd::hesap::dense
