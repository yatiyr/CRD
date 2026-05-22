#pragma once

#include <crd/core/types.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3a-1 — shared Householder reflector substrate.
//
// `make_householder` is a FAITHFUL port of LAPACK `dlarfg` (incl. the
// `safmin` rescaling guard) — the primitive shared by the symmetric
// tridiagonalization (dsytrd/dsytd2), and later the bidiagonalization
// (dgebrd) and Hessenberg reduction (dgehrd). It generates an elementary
// reflector
//
//     H = I - tau * v * v^T,   v[0] = 1 (implicit),
//
// such that  H * x = beta * e_0,  where x = [alpha, x_1, ..., x_{n-1}].
//
// IMPORTANT — divergence from the QR reflector (D(dense-eig)-5): the
// compact-WY QR path (`qr.cpp::panel_factor_qr_transposed`) uses the same
// beta/tau formula but OMITS the safmin rescaling branch. QR can ignore it
// because the trailing-matrix update swamps any underflow; the symmetric
// rank-2 update in tridiagonalization cannot — a tiny `beta` would give a
// non-zero `tau` with `beta == 0`, propagating NaN. So this primitive keeps
// the guard; the QR reflector stays an internal short-cut that does not.
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
    return w * std::sqrt(T{1} + r * r);
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
    if (xnorm_sq == T{0})
    {
        // H == I (the tail is already zero).
        return Householder<T>{T{0}, alpha0};
    }
    T xnorm = std::sqrt(xnorm_sq);

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
        xnorm = std::sqrt(xnorm_sq);
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

} // namespace crd::hesap::dense::detail
