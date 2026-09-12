#pragma once

// crd-hesap-special v12-d — generalized Marcum Q-function Q_M(a,b) = ∫_b^∞ x(x/a)^{M−1} e^{−(x²+a²)/2} I_{M−1}(ax) dx.
// Poisson-weighted upper-incomplete-gamma series (REUSES this module's gammainc_q, SANITY rule 8):
//   Q_M(a,b) = Σ_{k≥0} e^{−a²/2}(a²/2)ᵏ/k! · Q(M+k, b²/2)   (Q = regularized upper incomplete gamma)
// Equals ncx2.sf(b², 2M, a²). Gated vs scipy.stats.ncx2.sf to <1e-12. M>0 real, a,b ≥ 0.

#include <crd/hesap/special/gamma.hpp>      // Real concept
#include <crd/hesap/special/incomplete.hpp> // gammainc_q

#include <crd/math/cmath.hpp>

namespace crd::hesap::special
{
namespace detail
{
[[nodiscard]] inline double marcum_q_impl(double m, double a, double b) noexcept
{
    const double aa = 0.5 * a * a;
    const double bb = 0.5 * b * b;
    double pk = crd::math::exp(-aa); // Poisson(0; a²/2)
    double sum = pk * gammainc_q(m, bb);
    for (int k = 1; k <= 100000; ++k)
    {
        pk *= aa / k;
        const double term = pk * gammainc_q(m + k, bb);
        sum += term;
        if (term <= 1e-18 * sum && static_cast<double>(k) > aa)
        {
            break;
        }
    }
    return sum;
}
} // namespace detail

template <Real T>
[[nodiscard]] T marcum_q(T m, T a, T b) noexcept
{
    return static_cast<T>(detail::marcum_q_impl(static_cast<double>(m), static_cast<double>(a), static_cast<double>(b)));
}

} // namespace crd::hesap::special
