#pragma once

#include <crd/core/types.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3a-2.1 — secular-equation root-finder for the rank-1 update
// of a diagonal:  M = diag(d) + rho * z * z^T,  rho > 0,  d ascending.
//
// The eigenvalues lambda_i of M are the roots of the secular equation
//
//     f(lambda) = 1 + rho * sum_j  z_j^2 / (d_j - lambda)  = 0,
//
// and interlace the poles:  d_i < lambda_i < d_{i+1}  (i < n-1), and
// d_{n-1} < lambda_{n-1} < d_{n-1} + rho*||z||^2.
//
// D(dense-eig)-8 (deliberate divergence from LAPACK dlaed4's Bunch-Nielsen-
// Sorensen rational interpolation, advisor-approved 2026-05-22): we use a
// BRACKET-CONFINED Newton + bisection iteration. f is monotone increasing in
// each bracket (f' = rho*sum z_j^2/(d_j-lambda)^2 > 0), so a bracketed
// iteration CANNOT converge to the wrong root — strictly safer than BNS,
// which can. Root-finding is O(n^2 log n), dominated by the O(n^3)
// eigenvector construction, so BNS's faster convergence is invisible at
// cluster scale; the BNS upgrade is filed `v3a-2.1-perf` if ever a hot spot.
//
// STABILITY (the part we keep from dlaed4): every pole evaluation works in
// the SHIFTED coordinate  delta_j = (d_j - d_anchor) - tau,  where the anchor
// is the bracketing pole NEAREST the root (chosen by the sign of f at the
// bracket midpoint) and tau = lambda - d_anchor. Then delta_anchor = -tau is
// computed without the catastrophic cancellation of a raw (d_anchor - lambda)
// subtraction. Fixed iteration cap ⇒ deterministic.
// -----------------------------------------------------------------------

// secular_root — solve for the i-th eigenvalue (0-based) of diag(d)+rho*z*z^T.
// On exit, delta[j] = d[j] - lambda_i (the gaps the caller's eigenvector
// formula consumes). Returns lambda_i. Preconditions: n >= 1, rho > 0, d
// strictly ascending, z[j] != 0 (the caller deflates zeros/duplicates first).
template <typename R>
[[nodiscard]] inline R secular_root(int i, int n, const R* d, const R* z, R rho, R* delta) noexcept
{
    const R eps = std::numeric_limits<R>::epsilon();

    if (n == 1)
    {
        const R lambda = d[0] + rho * z[0] * z[0];
        delta[0] = d[0] - lambda;
        return lambda;
    }

    // Bracket [lo, hi] for lambda_i.
    R lo;
    R hi;
    if (i < n - 1)
    {
        lo = d[i];
        hi = d[i + 1];
    }
    else
    {
        R sumz2 = R{0};
        for (int j = 0; j < n; ++j)
        {
            sumz2 += z[j] * z[j];
        }
        lo = d[n - 1];
        hi = d[n - 1] + rho * sumz2;
    }

    // Secular function at lambda (raw form — used only for the midpoint
    // anchor decision, away from poles).
    auto f_raw = [&](R lambda) noexcept {
        R f = R{1};
        for (int j = 0; j < n; ++j)
        {
            f += rho * z[j] * z[j] / (d[j] - lambda);
        }
        return f;
    };

    // Choose the anchor pole nearest the root (orgati): f is increasing, so
    // f(mid) < 0 ⇒ root is in the upper half (anchor the upper pole).
    const R mid = lo + R{0.5} * (hi - lo);
    int anchor;
    if (i < n - 1)
    {
        anchor = (f_raw(mid) < R{0}) ? (i + 1) : i;
    }
    else
    {
        anchor = n - 1;
    }
    const R danchor = d[anchor];

    // Iterate on tau = lambda - d_anchor, bracket [tlo, thi].
    R tlo = lo - danchor;
    R thi = hi - danchor;
    R tau = mid - danchor;

    constexpr int kMaxIter = 100;  // deterministic cap (D(dense-eig)-8)
    for (int iter = 0; iter < kMaxIter; ++iter)
    {
        // Shifted evaluation: delta_j = (d_j - d_anchor) - tau = d_j - lambda.
        R f = R{1};
        R fp = R{0};
        R errm = R{1};
        for (int j = 0; j < n; ++j)
        {
            const R dl = (d[j] - danchor) - tau;
            delta[j] = dl;
            const R t = z[j] / dl;
            f += rho * z[j] * t;
            fp += rho * t * t;
            errm += rho * std::abs(z[j] * t);
        }

        if (f < R{0})
        {
            tlo = tau;
        }
        else
        {
            thi = tau;
        }

        // Converged when the residual is at the rounding floor of the sum, or
        // the bracket has collapsed to ulp width.
        if (std::abs(f) <= R{8} * eps * errm ||
            (thi - tlo) <= R{4} * eps * (std::abs(tau) + std::numeric_limits<R>::min()))
        {
            break;
        }

        // Newton step (fp > 0, monotone f), confined to the bracket; bisect on
        // overshoot. f < 0 ⇒ root above tau ⇒ Newton moves up.
        R tnew = tau - f / fp;
        if (!(tnew > tlo && tnew < thi))
        {
            tnew = R{0.5} * (tlo + thi);
        }
        tau = tnew;
    }

    const R lambda = danchor + tau;
    // delta[] already holds d_j - lambda from the final evaluation.
    return lambda;
}

} // namespace crd::hesap::dense::detail
