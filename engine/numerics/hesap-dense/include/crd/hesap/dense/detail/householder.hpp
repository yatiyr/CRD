#pragma once

#include <crd/core/platform.hpp> // CRD_NOINLINE
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>

#include <crd/math/cmath.hpp>
#include <cmath> // std::ldexp
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3a-1 — shared Householder reflector substrate.
//
// `make_householder` follows LAPACK `dlarfg` (incl. the `safmin` rescaling
// guard); where dlarfg takes the tail norm from the scaled `dnrm2`, it sums
// the squares plainly and redoes a sum it cannot trust on scaled values
// (`scaled_column_norm`, 2026-11-10) — the primitive shared by the symmetric
// tridiagonalization (dsytrd/dsytd2), and later the bidiagonalization
// (dgebrd) and Hessenberg reduction (dgehrd). It generates an elementary
// reflector
//
//     H = I - tau * v * v^T,   v[0] = 1 (implicit),
//
// such that  H * x = beta * e_0,  where x = [alpha, x_1, ..., x_{n-1}].
//
// The compact-WY QR path (`qr.cpp::panel_factor_qr_transposed`) uses the same
// beta/tau formula with its own guard (D(dense-eig)-5, revised 2026-11-10):
// a column whose plain sum of squares is non-finite or below mu / eps is
// redone scaled by an exact power of two (`reflector_rescaled`). The earlier
// claim that QR could skip a guard because the trailing update swamps any
// underflow was wrong: a column 2^-530 below its neighbours left Q 3.3e-6
// from orthogonal. The tridiagonalization's rank-2 update needs this
// primitive's guard for the further reason that a tiny `beta` would give a
// non-zero `tau` with `beta == 0`, propagating NaN.
// Follows LAPACK dlarfg, zlarfg (BSD-3-Clause-Open-MPI); notice in THIRD-PARTY-NOTICES.md.
// -----------------------------------------------------------------------

template <typename T>
struct Householder
{
    T tau;   // scalar factor; tau == 0 means H == I.
    T beta;  // the value H*x places in position 0 (= +/- ||x||).
};

// dlapy2 — overflow-safe sqrt(x^2 + y^2). Deterministic (sqrt is
// correctly-rounded; only basic ops besides).
template <typename T>
[[nodiscard]] inline T hypot2(T x, T y) noexcept
{
    const T ax = std::abs(x);
    const T ay = std::abs(y);
    const T w = ax > ay ? ax : ay;
    const T z = ax > ay ? ay : ax;
    if (z == T{0})
    {
        return w;
    }
    const T r = z / w;
    return w * crd::math::sqrt(T{1} + r * r);
}

// The norm of a column whose plain sum of squares s = |alpha|^2 + |tail|^2 cannot be trusted: s is non-finite or
// below mu / eps (mu the smallest normal, eps the epsilon). Below that floor the squares under mu carry an absolute
// error of up to delta / 2 each (delta = mu eps), an error of order one in s, so beta is wrong and H = I - tau v v^T
// is not orthogonal (eig_sym on a column tail 2^-530 below the rest left its eigenvectors 3.4e-6 from orthogonal); a
// finite s at or above the floor is accurate to n eps^2 / 2. The norm is root 2^e, root the square root of the sum of
// the squares of the values scaled by the exact power of two 2^-e that brings their largest magnitude into [1/2, 1),
// returned through ldexp (infinite only when the norm itself leaves the range); the plain root for a zero or
// non-finite column. The real form reads count values, the complex form both parts of count values. Out of line: an
// ordinary column never reaches them.
template <typename T> CRD_NOINLINE T scaled_column_norm(const T* values, crd::usize count) noexcept
{
    T peak = T{0};
    bool finite = true;
    for (crd::usize i = 0; i < count; ++i)
    {
        const T a = std::abs(values[i]);
        finite = finite && a <= std::numeric_limits<T>::max();
        peak = a > peak ? a : peak;
    }
    if (!finite || peak == T{0})
    {
        T s = T{0};
        for (crd::usize i = 0; i < count; ++i)
        {
            s += values[i] * values[i];
        }
        return crd::math::sqrt(s);
    }
    int e = 0;
    static_cast<void>(crd::math::frexp(peak, &e));
    // 2^-e as two factors, each a finite power of two even for the exponent of a subnormal peak
    const int half = e / 2;
    const T sa = std::ldexp(T{1}, -half);
    const T sb = std::ldexp(T{1}, half - e);
    T s = T{0};
    for (crd::usize i = 0; i < count; ++i)
    {
        const T t = (values[i] * sa) * sb;
        s += t * t;
    }
    return std::ldexp(crd::math::sqrt(s), e);
}

template <typename R> CRD_NOINLINE R scaled_column_norm(const crd::hesap::Complex<R>* values, crd::usize count) noexcept
{
    R peak = R{0};
    bool finite = true;
    for (crd::usize i = 0; i < count; ++i)
    {
        const R a = std::abs(values[i].re);
        const R b = std::abs(values[i].im);
        finite = finite && a <= std::numeric_limits<R>::max() && b <= std::numeric_limits<R>::max();
        peak = a > peak ? a : peak;
        peak = b > peak ? b : peak;
    }
    R s = R{0};
    if (!finite || peak == R{0})
    {
        for (crd::usize i = 0; i < count; ++i)
        {
            s += values[i].re * values[i].re + values[i].im * values[i].im;
        }
        return crd::math::sqrt(s);
    }
    int e = 0;
    static_cast<void>(crd::math::frexp(peak, &e));
    const int half = e / 2;
    const R sa = std::ldexp(R{1}, -half);
    const R sb = std::ldexp(R{1}, half - e);
    for (crd::usize i = 0; i < count; ++i)
    {
        const R a = (values[i].re * sa) * sb;
        const R b = (values[i].im * sa) * sb;
        s += a * a + b * b;
    }
    return std::ldexp(crd::math::sqrt(s), e);
}

// make_householder — dlarfg-faithful. `x` is contiguous length `n` with
// x[0] = alpha and x[1..n-1] = the vector to annihilate. On exit, x[1..n-1]
// holds the Householder vector tail (v[0] == 1 is implicit and NOT written);
// x[0] is left UNCHANGED (the caller stores `beta` into the tridiagonal
// off-diagonal and sets the in-matrix v[0] slot to 1 itself). Returns
// {tau, beta}.
template <typename T>
[[nodiscard]] inline Householder<T> make_householder(T* x, crd::usize n) noexcept
{
    if (n <= 1)
    {
        return Householder<T>{T{0}, n == 1 ? x[0] : T{0}};
    }

    const T alpha0 = x[0];

    // xnorm = ||x[1..n-1]||_2
    T xnorm_sq = T{0};
    for (crd::usize i = 1; i < n; ++i)
    {
        xnorm_sq += x[i] * x[i];
    }
    // The plain sum is trusted when finite and at least mu / eps (scaled_column_norm gives the bound); one rare branch
    // carries that and the zero tail, so an ordinary column runs the plain formulas unchanged.
    const T col_sq = alpha0 * alpha0 + xnorm_sq;
    const bool trusted = col_sq >= std::numeric_limits<T>::min() / std::numeric_limits<T>::epsilon() &&
                         col_sq <= std::numeric_limits<T>::max();
    T xnorm = T{0};
    if (!trusted || xnorm_sq == T{0}) [[unlikely]]
    {
        xnorm = trusted ? T{0} : scaled_column_norm<T>(x + 1, n - 1);
        if (xnorm == T{0})
        {
            // H == I (the tail is zero, or negligible against an alpha of trusted size).
            return Householder<T>{T{0}, alpha0};
        }
    }
    else
    {
        xnorm = crd::math::sqrt(xnorm_sq);
    }

    T alpha = alpha0;
    T beta = -(alpha >= T{0} ? T{1} : T{-1}) * hypot2(alpha, xnorm);

    // safmin = smallest representable number whose reciprocal does not
    // overflow == DLAMCH('S') / DLAMCH('E') in LAPACK terms.
    const T safmin = std::numeric_limits<T>::min() / std::numeric_limits<T>::epsilon();
    int knt = 0;
    if (std::abs(beta) < safmin)
    {
        // xnorm, beta may be inaccurate; scale x and recompute (capped at 20
        // iterations, exactly as dlarfg).
        const T rsafmn = T{1} / safmin;
        do
        {
            ++knt;
            for (crd::usize i = 1; i < n; ++i)
            {
                x[i] *= rsafmn;
            }
            beta *= rsafmn;
            alpha *= rsafmn;
        } while (std::abs(beta) < safmin && knt < 20);

        // Recompute beta from the rescaled tail.
        xnorm_sq = T{0};
        for (crd::usize i = 1; i < n; ++i)
        {
            xnorm_sq += x[i] * x[i];
        }
        xnorm = crd::math::sqrt(xnorm_sq);
        beta = -(alpha >= T{0} ? T{1} : T{-1}) * hypot2(alpha, xnorm);
    }

    const T tau = (beta - alpha) / beta;
    const T inv = T{1} / (alpha - beta);
    for (crd::usize i = 1; i < n; ++i)
    {
        x[i] *= inv;
    }

    // Undo the scaling on beta (alpha is subnormal-adjusted in dlarfg; we
    // only need beta returned at the original scale).
    for (int j = 0; j < knt; ++j)
    {
        beta *= safmin;
    }

    return Householder<T>{tau, beta};
}

// -----------------------------------------------------------------------
// make_householder_complex — faithful LAPACK `zlarfg`. Generates a complex
// elementary reflector
//
//     H = I - tau * v * v^H,   v[0] = 1 (implicit),
//
// such that  H^H * x = beta * e_0,  with **beta REAL** (the documented zlarfg
// property — the reflector is chosen so the leading element is real). `tau` is
// complex. `x` is contiguous length `n` with x[0] = alpha, x[1..n-1] the tail
// to annihilate; on exit x[1..n-1] holds the v-tail (v[0]=1 NOT written), x[0]
// unchanged. Includes the `safmin` rescaling guard (matters for the two-sided
// similarity updates, same reasoning as the real `make_householder`). As in
// zlarfg, n == 1 still reflects a complex alpha to the real beta (tau != 0).
//
// Shared by the complex Hermitian reduction (`zhetd2`, v3a-2.5) and the complex
// Hessenberg reduction (`zgehd2`, v3d-2c-1).
// -----------------------------------------------------------------------
template <typename R>
struct HouseholderComplex
{
    crd::hesap::Complex<R> tau;  // complex scalar factor; tau == 0 means H == I
    R beta;                      // real value H^H*x places in position 0
};

template <typename R>
[[nodiscard]] inline HouseholderComplex<R> make_householder_complex(crd::hesap::Complex<R>* x,
                                                                    crd::usize n) noexcept
{
    using C = crd::hesap::Complex<R>;
    if (n == 0)
    {
        return HouseholderComplex<R>{C{R{0}, R{0}}, R{0}};
    }
    R alphr = x[0].re;
    R alphi = x[0].im;
    R xnorm2 = R{0};
    for (crd::usize k = 1; k < n; ++k)
    {
        xnorm2 += x[k].re * x[k].re + x[k].im * x[k].im;
    }
    // trusted as in make_householder; one rare branch carries that and the zero tail of a real alpha
    const R col_sq = alphr * alphr + alphi * alphi + xnorm2;
    const bool trusted = col_sq >= std::numeric_limits<R>::min() / std::numeric_limits<R>::epsilon() &&
                         col_sq <= std::numeric_limits<R>::max();
    R beta = R{0};
    if (!trusted || (xnorm2 == R{0} && alphi == R{0})) [[unlikely]]
    {
        const R tail_norm = trusted ? R{0} : scaled_column_norm<R>(x + 1, n - 1);
        if (tail_norm == R{0} && alphi == R{0})
        {
            // H == I (the tail is zero, or negligible against an alpha of trusted size, and alpha is real).
            return HouseholderComplex<R>{C{R{0}, R{0}}, alphr};
        }
        beta = -(alphr >= R{0} ? R{1} : R{-1}) * scaled_column_norm<R>(x, n);
    }
    else
    {
        beta = -(alphr >= R{0} ? R{1} : R{-1}) * crd::math::sqrt(col_sq);
    }
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
        beta = -(alphr >= R{0} ? R{1} : R{-1}) * crd::math::sqrt(alphr * alphr + alphi * alphi + xnorm2);
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
    return HouseholderComplex<R>{tau, beta};
}

// -----------------------------------------------------------------------
// complex_givens — faithful LAPACK `zlartg`. Generates a plane rotation
//
//     G = [  c        s ]    with  c real ≥ 0,  c² + |s|² = 1,
//         [ -conj(s)  c ]
//
// such that  G · [f; g] = [r; 0]  (r complex). Used by the complex single-shift
// QR bulge chase (`zlahqr`, v3d-2c-2) and later `ztrevc`. Overflow-safe (the
// denominator is `hypot2`). Deterministic (sqrt + basic ops only).
// -----------------------------------------------------------------------
template <typename R>
struct ComplexGivens
{
    R c;                     // real cosine ≥ 0
    crd::hesap::Complex<R> s;  // complex sine
    crd::hesap::Complex<R> r;  // G·[f;g] places this in row 0
};

template <typename R>
[[nodiscard]] inline ComplexGivens<R> complex_givens(const crd::hesap::Complex<R>& f,
                                                     const crd::hesap::Complex<R>& g) noexcept
{
    using C = crd::hesap::Complex<R>;
    if (g.re == R{0} && g.im == R{0})
    {
        return ComplexGivens<R>{R{1}, C{R{0}, R{0}}, f};
    }
    const R g1 = crd::hesap::abs(g);
    if (f.re == R{0} && f.im == R{0})
    {
        // c = 0, s = conj(g)/|g|, r = |g|.
        return ComplexGivens<R>{R{0}, crd::hesap::conj(g) * (R{1} / g1), C{g1, R{0}}};
    }
    const R f1 = crd::hesap::abs(f);
    const R d = hypot2(f1, g1);
    const C fs = f * (R{1} / f1);  // unit phase of f
    const R c = f1 / d;
    const C s = (fs * crd::hesap::conj(g)) * (R{1} / d);
    const C r = fs * C{d, R{0}};
    return ComplexGivens<R>{c, s, r};
}

} // namespace crd::hesap::dense::detail
