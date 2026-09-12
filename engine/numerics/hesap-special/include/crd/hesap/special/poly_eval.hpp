#pragma once

// crd-hesap-special — shared Horner evaluator for the iteration-free v12-d fast paths (minimax polynomials/rationals
// emitted by the gen_*_poly.py generators). Coefficients are monomial in t = the mapped argument.

namespace crd::hesap::special::detail
{
template <int N>
[[nodiscard]] inline double horner_t(const double (&c)[N], double t) noexcept
{
    double r = c[N - 1];
    for (int i = N - 2; i >= 0; --i)
    {
        r = r * t + c[i];
    }
    return r;
}
} // namespace crd::hesap::special::detail
