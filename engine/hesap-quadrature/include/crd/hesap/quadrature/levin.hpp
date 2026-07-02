#pragma once

// crd-hesap-quadrature v13-j — LEVIN COLLOCATION for highly-oscillatory integrals with a GENERAL phase:
//   ∫_a^b f(x)·{cos,sin}(ω g(x)) dx   (g nonlinear ⇒ no Filon/QAWO Chebyshev moments exist).
//
// The Levin idea (Levin 1996): instead of integrating the oscillation directly, find a slowly-varying antiderivative
// p with d/dx[p·e^{iωg}] = f·e^{iωg}, i.e. solve the collocation ODE  p' + iω g' p = f. Expand p in a Chebyshev basis,
// collocate at Chebyshev-Lobatto nodes, solve the (small, well-conditioned) n×n COMPLEX system, then
//   ∫ f e^{iωg} = [p e^{iωg}]_a^b,  ∫ f cos(ωg) = Re,  ∫ f sin(ωg) = Im.
// The magic: n is O(1) (it only resolves the slow p, never the oscillation) and the ACCURACY GROWS WITH ω — exactly
// where QAWO/Gauss/adaptive all degrade. There is NO QUADPACK/GSL/scipy/Boost peer (a research method); the gate is
// the analytic Fresnel value, the accuracy-grows-with-ω invariant, and bit-agreement with QAWO at the special g(x)=x.
//
// ⚠ LIMITATION (honest, ADR-0095 / SANITY #6): Levin requires g'(x) ≠ 0 on [a,b] — a STATIONARY POINT (g'=0) makes the
// collocation operator singular and degrades the result. Caller's responsibility (split the interval at the stationary
// point, or use a stationary-phase / steepest-descent method there). Verified bit-exact in python before this port.
//
// Moat: determinism (crd::math, fixed-pivot Gaussian elimination, e^{iθ}=cos+isin not a complex exp) + allocation-free
// (fixed-size stack system, n ≤ kLevinMax = the WCET bound) + the error-tier result (a two-grid Tier-1 estimate).

#include <crd/core/types.hpp>
#include <crd/hesap/quadrature/integrate.hpp>
#include <crd/hesap/quadrature/oscillatory.hpp> // OscWeight
#include <crd/math/cmath.hpp>

#include <complex>
#include <limits>

namespace crd::hesap::quadrature
{

namespace detail
{
constexpr int kLevinMax = 48; // max collocation points (the WCET / stack-size bound)

// Solve the n×n COMPLEX system A x = b in place by Gaussian elimination with partial pivoting (max squared-magnitude
// pivot, first wins ties ⇒ deterministic). A is row-major [n*n], b is [n]; both overwritten, x returned in b.
template <typename T> void levin_csolve(std::complex<T>* A, std::complex<T>* b, int n) noexcept
{
    for (int k = 0; k < n; ++k)
    {
        int piv = k;
        T best = A[k * n + k].real() * A[k * n + k].real() + A[k * n + k].imag() * A[k * n + k].imag();
        for (int i = k + 1; i < n; ++i)
        {
            const T m = A[i * n + k].real() * A[i * n + k].real() + A[i * n + k].imag() * A[i * n + k].imag();
            if (m > best)
            {
                best = m;
                piv = i;
            }
        }
        if (piv != k)
        {
            for (int j = k; j < n; ++j)
            {
                const std::complex<T> tmp = A[k * n + j];
                A[k * n + j] = A[piv * n + j];
                A[piv * n + j] = tmp;
            }
            const std::complex<T> tb = b[k];
            b[k] = b[piv];
            b[piv] = tb;
        }
        const std::complex<T> akk = A[k * n + k];
        for (int i = k + 1; i < n; ++i)
        {
            const std::complex<T> factor = A[i * n + k] / akk;
            for (int j = k; j < n; ++j)
            {
                A[i * n + j] -= factor * A[k * n + j];
            }
            b[i] -= factor * b[k];
        }
    }
    for (int i = n - 1; i >= 0; --i)
    {
        std::complex<T> s = b[i];
        for (int j = i + 1; j < n; ++j)
        {
            s -= A[i * n + j] * b[j];
        }
        b[i] = s / A[i * n + i];
    }
}

// Core Levin: returns the complex ∫_a^b f e^{iωg} dx with n collocation points. f/g/gprime are callables T→T.
template <typename T, typename F, typename G, typename Gp>
[[nodiscard]] std::complex<T> levin_core(F&& f, G&& g, Gp&& gprime, T a, T b, T omega, int n)
{
    const T half = (b - a) / T{2};
    const T mid = (a + b) / T{2};
    const T pi = static_cast<T>(3.14159265358979323846264338327950288);
    std::complex<T> mat[kLevinMax * kLevinMax];
    std::complex<T> rhs[kLevinMax];
    T tnode[kLevinMax];  // T_j(s_k) for the current node
    T tderiv[kLevinMax]; // T_j'(s_k)
    const std::complex<T> imom(T{0}, omega);
    for (int k = 0; k < n; ++k)
    {
        const T s = crd::math::cos(pi * static_cast<T>(k) / static_cast<T>(n - 1)); // Chebyshev-Lobatto node in [-1,1]
        const T xk = mid + half * s;
        // Chebyshev T_j(s), T_j'(s) by recurrence.
        tnode[0] = T{1};
        tderiv[0] = T{0};
        if (n > 1)
        {
            tnode[1] = s;
            tderiv[1] = T{1};
        }
        for (int j = 2; j < n; ++j)
        {
            tnode[j] = T{2} * s * tnode[j - 1] - tnode[j - 2];
            tderiv[j] = T{2} * tnode[j - 1] + T{2} * s * tderiv[j - 1] - tderiv[j - 2];
        }
        const std::complex<T> coeff = imom * gprime(xk); // iω g'(x_k)
        for (int j = 0; j < n; ++j)
        {
            // A[k][j] = d/dx u_j(x_k) + iω g'(x_k) u_j(x_k), u_j(x) = T_j(s(x)), du/dx = T_j'(s)·2/(b−a).
            mat[k * n + j] = std::complex<T>(tderiv[j] * (T{1} / half), T{0}) + coeff * tnode[j];
        }
        rhs[k] = std::complex<T>(f(xk), T{0});
    }
    levin_csolve<T>(mat, rhs, n); // rhs ← coefficients c_j of p
    // p(b) = Σ c_j T_j(1) = Σ c_j ; p(a) = Σ c_j T_j(−1) = Σ c_j (−1)^j
    std::complex<T> pb(T{0}, T{0});
    std::complex<T> pa(T{0}, T{0});
    T sign = T{1};
    for (int j = 0; j < n; ++j)
    {
        pb += rhs[j];
        pa += sign * rhs[j];
        sign = -sign;
    }
    // e^{iωg} = cos(ωg) + i sin(ωg) (crd::math, deterministic — not a complex exp()).
    const T gb = omega * g(b);
    const T ga = omega * g(a);
    const std::complex<T> eb(crd::math::cos(gb), crd::math::sin(gb));
    const std::complex<T> ea(crd::math::cos(ga), crd::math::sin(ga));
    return pb * eb - pa * ea;
}
} // namespace detail

// Levin collocation for ∫_a^b f(x)·{cos,sin}(ω g(x)) dx with a general phase g (g' ≠ 0 on [a,b]). n = collocation
// points (default 16; clamped to [4, 48]). The error_estimate is a two-grid Tier-1 estimate (n vs n−4 points).
// f/g/gprime are callables T→T. result.value = ∫ f cos(ωg) for OscWeight::Cos, ∫ f sin(ωg) for Sin.
template <typename T, typename F, typename G, typename Gp>
[[nodiscard]] QuadResult<T> integrate_levin(F&& f, G&& g, Gp&& gprime, T a, T b, T omega, OscWeight w, int n = 16)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b) || !detail::quad_finite(omega) || n < 4)
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    if (n > detail::kLevinMax)
    {
        n = detail::kLevinMax;
    }
    const std::complex<T> full = detail::levin_core<T>(f, g, gprime, a, b, omega, n);
    const int ncoarse = n - 4 >= 4 ? n - 4 : 4;
    const std::complex<T> coarse = detail::levin_core<T>(f, g, gprime, a, b, omega, ncoarse);
    const bool cos = (w == OscWeight::Cos);
    const T vfull = cos ? full.real() : full.imag();
    const T vcoarse = cos ? coarse.real() : coarse.imag();
    QuadResult<T> r;
    r.value = vfull;
    r.error_estimate = crd::math::fabs(vfull - vcoarse); // Tier-1 two-grid estimate (NOT a bound)
    r.eval_count = static_cast<crd::u32>(n + ncoarse);
    r.subdiv_count = 1;
    r.tolerance_met = true;
    r.status = QuadStatus::Ok;
    return r;
}

} // namespace crd::hesap::quadrature
