#pragma once

// crd-hesap-stats v12-l — autodiff-ready log-densities: ANALYTIC ∂logp/∂x and ∂logp/∂θ for every distribution.
// This is the HMC/NUTS (v12-q) + MLE enabler. We hand-derive the gradients rather than run a general AD engine
// (v15/v16) for two reasons: (1) the log-densities call Real-constrained special functions (gammainc/betainc/
// digamma) that a Dual<T> can't traverse; (2) the analytic forms REUSE the shipped special functions AS the
// derivative terms — d/da·lgamma(a) IS digamma(a), which hesap-special already ships (SANITY rule 8) — so they
// are both exact and faster than a tape. Every gradient is checked against central finite-difference of the SAME
// logpdf (test_log_density_grad.cpp) and against Stan-math reference values.
//
// Interface (free functions; continuous.hpp stays lean):
//   dlogpdf_dx(d, x)            -> ∂logp/∂x            (HMC over a latent variate; MLE score has no x term)
//   dlogpdf_dtheta(d, x, g)     -> fills g[0..k) with ∂logp/∂θ_i in the distribution's field order (MLE/HMC over θ)
//   theta_dim(d)                -> k, the parameter count (so a caller can size g generically)
// θ field order is documented per distribution; it matches the struct's declaration order.

#include <crd/hesap/stats/continuous.hpp>
#include <crd/hesap/stats/discrete.hpp>
#include <crd/hesap/stats/heavy_tail.hpp>

#include <crd/core/types.hpp>

#include <crd/containers/span.hpp>
#include <crd/math/simd/transcendental.hpp> // crd_log4 (AVX2 SIMD log) for the O(N) loglik_grad hot loops

#include <type_traits>

namespace crd::hesap::stats
{

// ───────────────────────────── Normal(μ, σ) ──  θ = [μ, σ] ─────────────────────────────
// logp = −½z² − log σ − ½ln2π,  z = (x−μ)/σ
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Normal<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Normal<T>& d, T x) noexcept
{
    return (d.mu - x) / (d.sigma * d.sigma); // −z/σ
}
template <Real T>
void dlogpdf_dtheta(const Normal<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = (x - d.mu) / d.sigma;
    g[0] = z / d.sigma;                                       // ∂/∂μ
    g[1] = (z * z - static_cast<T>(1)) / d.sigma;             // ∂/∂σ
}

// ───────────────────────────── Exponential(scale) ──  θ = [scale] ─────────────────────────────
// logp = −x/s − log s
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Exponential<T>&) noexcept
{
    return 1;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Exponential<T>& d, T) noexcept
{
    return -static_cast<T>(1) / d.scale;
}
template <Real T>
void dlogpdf_dtheta(const Exponential<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    g[0] = x / (d.scale * d.scale) - static_cast<T>(1) / d.scale; // ∂/∂s
}

// ───────────────────────────── Gamma(shape a, scale s) ──  θ = [a, s] ─────────────────────────────
// logp = (a−1)log x − x/s − a log s − lgamma(a)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Gamma<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Gamma<T>& d, T x) noexcept
{
    return (d.shape - static_cast<T>(1)) / x - static_cast<T>(1) / d.scale;
}
template <Real T>
void dlogpdf_dtheta(const Gamma<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    // ∂/∂a reuses digamma = d/da·lgamma(a) (SANITY rule 8 — the shipped special function IS the derivative term).
    g[0] = crd::math::log(x) - crd::math::log(d.scale) - special::digamma(d.shape);
    g[1] = x / (d.scale * d.scale) - d.shape / d.scale;          // ∂/∂s
}

// ───────────────────────────── Beta(a, b) on [0,1] ──  θ = [a, b] ─────────────────────────────
// logp = (a−1)log x + (b−1)log(1−x) − lbeta(a,b);  ∂lbeta/∂a = ψ(a)−ψ(a+b)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Beta<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Beta<T>& d, T x) noexcept
{
    return (d.a - static_cast<T>(1)) / x - (d.b - static_cast<T>(1)) / (static_cast<T>(1) - x);
}
template <Real T>
void dlogpdf_dtheta(const Beta<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T psi_ab = special::digamma(d.a + d.b);
    g[0] = crd::math::log(x) - (special::digamma(d.a) - psi_ab);          // ∂/∂a
    g[1] = crd::math::log1p(-x) - (special::digamma(d.b) - psi_ab);       // ∂/∂b
}

// ───────────────────────────── LogNormal(μ, σ) ──  θ = [μ, σ] ─────────────────────────────
// logp = −½z² − log(xσ) − ½ln2π,  z = (log x − μ)/σ
template <Real T>
[[nodiscard]] constexpr int theta_dim(const LogNormal<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const LogNormal<T>& d, T x) noexcept
{
    const T z = (crd::math::log(x) - d.mu) / d.sigma;
    return -(z / d.sigma + static_cast<T>(1)) / x;
}
template <Real T>
void dlogpdf_dtheta(const LogNormal<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = (crd::math::log(x) - d.mu) / d.sigma;
    g[0] = z / d.sigma;                                       // ∂/∂μ
    g[1] = (z * z - static_cast<T>(1)) / d.sigma;             // ∂/∂σ
}

// ───────────────────────────── ChiSquared(k) ──  θ = [k] ─────────────────────────────
// logp = (h−1)log x − ½x − h·ln2 − lgamma(h),  h = k/2
template <Real T>
[[nodiscard]] constexpr int theta_dim(const ChiSquared<T>&) noexcept
{
    return 1;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const ChiSquared<T>& d, T x) noexcept
{
    return (static_cast<T>(0.5) * d.k - static_cast<T>(1)) / x - static_cast<T>(0.5);
}
template <Real T>
void dlogpdf_dtheta(const ChiSquared<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T h = static_cast<T>(0.5) * d.k;
    g[0] = static_cast<T>(0.5) * (crd::math::log(x) - detail::kLn2<T> - special::digamma(h)); // ∂/∂k (dh/dk = ½)
}

// ───────────────────────────── Cauchy(loc, scale) ──  θ = [loc, scale] ─────────────────────────────
// logp = −log(π·s·(1+z²)),  z = (x−loc)/s
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Cauchy<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Cauchy<T>& d, T x) noexcept
{
    const T z = (x - d.loc) / d.scale;
    return static_cast<T>(-2) * z / (d.scale * (static_cast<T>(1) + z * z));
}
template <Real T>
void dlogpdf_dtheta(const Cauchy<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = (x - d.loc) / d.scale;
    const T denom = d.scale * (static_cast<T>(1) + z * z);
    g[0] = static_cast<T>(2) * z / denom;                              // ∂/∂loc
    g[1] = (z * z - static_cast<T>(1)) / denom;                        // ∂/∂scale
}

// ───────────────────────────── Laplace(loc, scale) ──  θ = [loc, scale] ─────────────────────────────
// logp = −|x−loc|/s − log(2s).  Non-smooth at x = loc (the ∂x kink); gradients hold for x ≠ loc.
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Laplace<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Laplace<T>& d, T x) noexcept
{
    const T s = x >= d.loc ? static_cast<T>(1) : static_cast<T>(-1);
    return -s / d.scale;
}
template <Real T>
void dlogpdf_dtheta(const Laplace<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T s = x >= d.loc ? static_cast<T>(1) : static_cast<T>(-1);
    g[0] = s / d.scale;                                                // ∂/∂loc
    g[1] = crd::math::fabs(x - d.loc) / (d.scale * d.scale) - static_cast<T>(1) / d.scale; // ∂/∂scale
}

// ───────────────────────────── Logistic(loc, scale) ──  θ = [loc, scale] ─────────────────────────────
// logp = −z − log s − 2·log1p(e^{−z}),  z = (x−loc)/s;  p = 1/(1+e^{−z}) is the logistic cdf, d/dz logp = 1−2p
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Logistic<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Logistic<T>& d, T x) noexcept
{
    const T z = (x - d.loc) / d.scale;
    const T p = static_cast<T>(1) / (static_cast<T>(1) + crd::math::exp(-z));
    return (static_cast<T>(1) - static_cast<T>(2) * p) / d.scale;
}
template <Real T>
void dlogpdf_dtheta(const Logistic<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = (x - d.loc) / d.scale;
    const T p = static_cast<T>(1) / (static_cast<T>(1) + crd::math::exp(-z));
    const T dz = static_cast<T>(1) - static_cast<T>(2) * p;            // d logp / dz
    g[0] = -dz / d.scale;                                             // ∂/∂loc
    g[1] = (dz * (-z) - static_cast<T>(1)) / d.scale;                 // ∂/∂scale
}

// ───────────────────────────── Weibull(c, scale) ──  θ = [c, scale] ─────────────────────────────
// logp = log(c/s) + (c−1)log z − z^c,  z = x/s
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Weibull<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Weibull<T>& d, T x) noexcept
{
    const T zc = crd::math::pow(x / d.scale, d.c);
    return (d.c - static_cast<T>(1) - d.c * zc) / x;
}
template <Real T>
void dlogpdf_dtheta(const Weibull<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = x / d.scale;
    const T lz = crd::math::log(z);
    const T zc = crd::math::pow(z, d.c);
    g[0] = static_cast<T>(1) / d.c + lz * (static_cast<T>(1) - zc);    // ∂/∂c
    g[1] = d.c * (zc - static_cast<T>(1)) / d.scale;                  // ∂/∂scale
}

// ───────────────────────────── Gumbel(loc, scale) ──  θ = [loc, scale] ─────────────────────────────
// logp = −(z + e^{−z}) − log s,  z = (x−loc)/s
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Gumbel<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Gumbel<T>& d, T x) noexcept
{
    const T z = (x - d.loc) / d.scale;
    return (crd::math::exp(-z) - static_cast<T>(1)) / d.scale;
}
template <Real T>
void dlogpdf_dtheta(const Gumbel<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = (x - d.loc) / d.scale;
    const T em = static_cast<T>(1) - crd::math::exp(-z);              // 1 − e^{−z}
    g[0] = em / d.scale;                                              // ∂/∂loc
    g[1] = (em * z - static_cast<T>(1)) / d.scale;                    // ∂/∂scale
}

// ───────────────────────────── Pareto(b, scale) ──  θ = [b, scale] ─────────────────────────────
// logp = log b + b·log s − (b+1)log x
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Pareto<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Pareto<T>& d, T x) noexcept
{
    return -(d.b + static_cast<T>(1)) / x;
}
template <Real T>
void dlogpdf_dtheta(const Pareto<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    g[0] = static_cast<T>(1) / d.b + crd::math::log(d.scale) - crd::math::log(x); // ∂/∂b
    g[1] = d.b / d.scale;                                                         // ∂/∂scale
}

// ───────────────────────────── Rayleigh(scale) ──  θ = [scale] ─────────────────────────────
// logp = log x − 2log s − ½x²/s²
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Rayleigh<T>&) noexcept
{
    return 1;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Rayleigh<T>& d, T x) noexcept
{
    return static_cast<T>(1) / x - x / (d.scale * d.scale);
}
template <Real T>
void dlogpdf_dtheta(const Rayleigh<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    g[0] = (x * x / (d.scale * d.scale) - static_cast<T>(2)) / d.scale; // ∂/∂scale
}

// ───────────────────────────── Maxwell(scale) ──  θ = [scale] ─────────────────────────────
// logp = ½log(2/π) + 2log x − 3log s − ½x²/s²
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Maxwell<T>&) noexcept
{
    return 1;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Maxwell<T>& d, T x) noexcept
{
    return static_cast<T>(2) / x - x / (d.scale * d.scale);
}
template <Real T>
void dlogpdf_dtheta(const Maxwell<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    g[0] = (x * x / (d.scale * d.scale) - static_cast<T>(3)) / d.scale; // ∂/∂scale
}

// ───────────────────────────── Uniform(a, b) ──  θ = [a, b] ─────────────────────────────
// logp = −log(b−a), constant in x on (a,b)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Uniform<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Uniform<T>&, T) noexcept
{
    return static_cast<T>(0);
}
template <Real T>
void dlogpdf_dtheta(const Uniform<T>& d, T, crd::containers::Span<T> g) noexcept
{
    const T inv = static_cast<T>(1) / (d.b - d.a);
    g[0] = inv;                                                        // ∂/∂a
    g[1] = -inv;                                                       // ∂/∂b
}

// ───────────────────────────── HalfNormal(σ) ──  θ = [σ] ─────────────────────────────
// logp = ½log(2/π) − log σ − x²/(2σ²),  x ≥ 0
template <Real T>
[[nodiscard]] constexpr int theta_dim(const HalfNormal<T>&) noexcept
{
    return 1;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const HalfNormal<T>& d, T x) noexcept
{
    return -x / (d.sigma * d.sigma);
}
template <Real T>
void dlogpdf_dtheta(const HalfNormal<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    g[0] = (x * x / (d.sigma * d.sigma) - static_cast<T>(1)) / d.sigma; // ∂/∂σ
}

// ───────────────────────────── HalfCauchy(scale) ──  θ = [scale] ─────────────────────────────
// logp = log(2/π) − log s − log(1+(x/s)²),  x ≥ 0
template <Real T>
[[nodiscard]] constexpr int theta_dim(const HalfCauchy<T>&) noexcept
{
    return 1;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const HalfCauchy<T>& d, T x) noexcept
{
    return static_cast<T>(-2) * x / (d.scale * d.scale + x * x);
}
template <Real T>
void dlogpdf_dtheta(const HalfCauchy<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    g[0] = -static_cast<T>(1) / d.scale + static_cast<T>(2) * x * x / (d.scale * (d.scale * d.scale + x * x)); // ∂/∂scale
}

// ───────────────────────────── InverseGamma(a, scale) ──  θ = [a, scale] ─────────────────────────────
// logp = a·log s − (a+1)log x − s/x − lgamma(a)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const InverseGamma<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const InverseGamma<T>& d, T x) noexcept
{
    return -(d.a + static_cast<T>(1)) / x + d.scale / (x * x);
}
template <Real T>
void dlogpdf_dtheta(const InverseGamma<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    g[0] = crd::math::log(d.scale) - crd::math::log(x) - special::digamma(d.a); // ∂/∂a
    g[1] = d.a / d.scale - static_cast<T>(1) / x;                               // ∂/∂scale
}

// ───────────────────────────── StudentT(ν) ──  θ = [ν] ─────────────────────────────
// logp = −lbeta(ν/2,½) − ½log ν − ((ν+1)/2)·log1p(x²/ν)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const StudentT<T>&) noexcept
{
    return 1;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const StudentT<T>& d, T x) noexcept
{
    return -(d.nu + static_cast<T>(1)) * x / (d.nu + x * x);
}
template <Real T>
void dlogpdf_dtheta(const StudentT<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T h = static_cast<T>(0.5) * d.nu;
    // ∂lbeta(ν/2,½)/∂ν = ½(ψ(ν/2)−ψ((ν+1)/2)); the −lbeta sign flips it. Reuses digamma.
    g[0] = static_cast<T>(0.5) * (special::digamma(h + static_cast<T>(0.5)) - special::digamma(h)) -
           static_cast<T>(0.5) / d.nu - static_cast<T>(0.5) * crd::math::log1p(x * x / d.nu) +
           (d.nu + static_cast<T>(1)) * x * x / (static_cast<T>(2) * d.nu * (d.nu + x * x));
}

// ───────────────────────────── FisherF(d1, d2) ──  θ = [d1, d2] ─────────────────────────────
// logp = a·log(d1/d2) + (a−1)log x − (a+d2/2)·log1p(d1x/d2) − lbeta(d1/2,d2/2),  a = d1/2
template <Real T>
[[nodiscard]] constexpr int theta_dim(const FisherF<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const FisherF<T>& d, T x) noexcept
{
    return (static_cast<T>(0.5) * d.d1 - static_cast<T>(1)) / x -
           static_cast<T>(0.5) * (d.d1 + d.d2) * d.d1 / (d.d2 + d.d1 * x);
}
template <Real T>
void dlogpdf_dtheta(const FisherF<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T l1u = crd::math::log1p(d.d1 * x / d.d2);
    const T sum = d.d1 + d.d2;
    const T den = d.d2 + d.d1 * x;
    const T psi_s = special::digamma(static_cast<T>(0.5) * sum);
    g[0] = static_cast<T>(0.5) * crd::math::log(d.d1 / d.d2) + static_cast<T>(0.5) +
           static_cast<T>(0.5) * crd::math::log(x) - static_cast<T>(0.5) * l1u -
           static_cast<T>(0.5) * sum * x / den -
           static_cast<T>(0.5) * (special::digamma(static_cast<T>(0.5) * d.d1) - psi_s); // ∂/∂d1
    g[1] = -d.d1 / (static_cast<T>(2) * d.d2) - static_cast<T>(0.5) * l1u +
           static_cast<T>(0.5) * sum * d.d1 * x / (d.d2 * den) -
           static_cast<T>(0.5) * (special::digamma(static_cast<T>(0.5) * d.d2) - psi_s); // ∂/∂d2
}

// ───────────────────────────── Nakagami(m, scale) ──  θ = [m, scale] ─────────────────────────────
// logp = log2 + m·log m − lgamma(m) + (2m−1)log z − m·z² − log s,  z = x/s
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Nakagami<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Nakagami<T>& d, T x) noexcept
{
    return (static_cast<T>(2) * d.m - static_cast<T>(1)) / x - static_cast<T>(2) * d.m * x / (d.scale * d.scale);
}
template <Real T>
void dlogpdf_dtheta(const Nakagami<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = x / d.scale;
    g[0] = crd::math::log(d.m) + static_cast<T>(1) - special::digamma(d.m) +
           static_cast<T>(2) * crd::math::log(z) - z * z;                            // ∂/∂m
    g[1] = static_cast<T>(2) * d.m * (z * z - static_cast<T>(1)) / d.scale;          // ∂/∂scale
}

// ───────────────────────────── Wald(μ, λ) ──  θ = [μ, λ] ─────────────────────────────
// logp = ½(log λ − ln2π − 3log x) − λ(x−μ)²/(2μ²x)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Wald<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Wald<T>& d, T x) noexcept
{
    return static_cast<T>(-1.5) / x - d.lambda / (static_cast<T>(2) * d.mu * d.mu) +
           d.lambda / (static_cast<T>(2) * x * x);
}
template <Real T>
void dlogpdf_dtheta(const Wald<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T dm = x - d.mu;
    g[0] = d.lambda * dm / (d.mu * d.mu * d.mu);                                       // ∂/∂μ
    g[1] = static_cast<T>(0.5) / d.lambda - dm * dm / (static_cast<T>(2) * d.mu * d.mu * x); // ∂/∂λ
}

// ───────────────────────────── VonMises(μ, κ) ──  θ = [μ, κ] ─────────────────────────────
// logp = κ·cos(x−μ) − ln2π − log I₀(κ)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const VonMises<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const VonMises<T>& d, T x) noexcept
{
    return -d.kappa * crd::math::sin(x - d.mu);
}
template <Real T>
void dlogpdf_dtheta(const VonMises<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    g[0] = d.kappa * crd::math::sin(x - d.mu);                                         // ∂/∂μ
    // ∂/∂κ = cos(x−μ) − I₁(κ)/I₀(κ)  (d/dκ log I₀(κ) = I₁(κ)/I₀(κ); reuses cyl_bessel_i, SANITY 8)
    g[1] = crd::math::cos(x - d.mu) - special::cyl_bessel_i(static_cast<T>(1), d.kappa) /
                                          special::cyl_bessel_i(static_cast<T>(0), d.kappa);
}

// ───────────────────────────── Rice(ν, σ) ──  θ = [ν, σ] ─────────────────────────────
// logp = log x − 2log σ − (x²+ν²)/(2σ²) + log I₀(b),  b = xν/σ²;  r = I₁(b)/I₀(b)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Rice<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T rice_bessel_ratio(T b) noexcept
{
    return special::cyl_bessel_i(static_cast<T>(1), b) / special::cyl_bessel_i(static_cast<T>(0), b);
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Rice<T>& d, T x) noexcept
{
    const T s2 = d.sigma * d.sigma;
    const T r = rice_bessel_ratio(x * d.nu / s2);
    return static_cast<T>(1) / x - x / s2 + (d.nu / s2) * r;
}
template <Real T>
void dlogpdf_dtheta(const Rice<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T s2 = d.sigma * d.sigma;
    const T b = x * d.nu / s2;
    const T r = rice_bessel_ratio(b);
    g[0] = -d.nu / s2 + (x / s2) * r;                                                  // ∂/∂ν
    g[1] = -static_cast<T>(2) / d.sigma + (x * x + d.nu * d.nu) / (s2 * d.sigma) -
           static_cast<T>(2) * b / d.sigma * r;                                        // ∂/∂σ
}

// Zipf(a) ──  θ = [a];  logpmf = −a·log k − log ζ(a);  ∂/∂a = −log k − ζ'(a)/ζ(a)  (reuses riemann_zeta_prime)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Zipf<T>&) noexcept
{
    return 1;
}
template <Real T>
void dlogpmf_dtheta(const Zipf<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    g[0] = -crd::math::log(static_cast<T>(k)) - special::riemann_zeta_prime(d.a) / special::riemann_zeta(d.a);
}

// ───────────────────────────── HEAVY-TAIL ─────────────────────────────

// GEV(ξ, μ, σ) ──  θ = [ξ, μ, σ];  s = 1+ξz, z=(x−μ)/σ;  logp = −log σ − ((ξ+1)/ξ)log s − s^{−1/ξ}  (ξ≠0)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const GEV<T>&) noexcept
{
    return 3;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const GEV<T>& d, T x) noexcept
{
    const T z = (x - d.mu) / d.sigma;
    const T s = static_cast<T>(1) + d.xi * z;
    const T sp = crd::math::pow(s, -static_cast<T>(1) / d.xi); // s^{−1/ξ}
    return (sp - (d.xi + static_cast<T>(1))) / (d.sigma * s);
}
template <Real T>
void dlogpdf_dtheta(const GEV<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = (x - d.mu) / d.sigma;
    const T s = static_cast<T>(1) + d.xi * z;
    const T ls = crd::math::log(s);
    const T sp = crd::math::pow(s, -static_cast<T>(1) / d.xi);
    const T inv_xi = static_cast<T>(1) / d.xi;
    g[0] = ls * inv_xi * inv_xi - (static_cast<T>(1) + inv_xi) * z / s -
           sp * (ls * inv_xi * inv_xi - z / (d.xi * s));              // ∂/∂ξ
    g[1] = ((d.xi + static_cast<T>(1)) - sp) / (d.sigma * s);         // ∂/∂μ = −∂/∂x
    g[2] = -static_cast<T>(1) / d.sigma + z * ((d.xi + static_cast<T>(1)) - sp) / (d.sigma * s); // ∂/∂σ
}

// GPD(ξ, μ, σ) ──  θ = [ξ, μ, σ];  s = 1+ξz;  logp = −log σ − (1/ξ+1)log s  (ξ≠0)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const GPD<T>&) noexcept
{
    return 3;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const GPD<T>& d, T x) noexcept
{
    const T s = static_cast<T>(1) + d.xi * (x - d.mu) / d.sigma;
    return -(static_cast<T>(1) + d.xi) / (d.sigma * s);
}
template <Real T>
void dlogpdf_dtheta(const GPD<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = (x - d.mu) / d.sigma;
    const T s = static_cast<T>(1) + d.xi * z;
    const T inv_xi = static_cast<T>(1) / d.xi;
    g[0] = crd::math::log(s) * inv_xi * inv_xi - (static_cast<T>(1) + d.xi) * z / (d.xi * s); // ∂/∂ξ
    g[1] = (static_cast<T>(1) + d.xi) / (d.sigma * s);                                        // ∂/∂μ = −∂/∂x
    g[2] = -static_cast<T>(1) / d.sigma + (static_cast<T>(1) + d.xi) * z / (d.sigma * s);     // ∂/∂σ
}

// Levy(μ, σ) ──  θ = [μ, σ];  y=x−μ;  logp = ½log(σ/2π) − σ/(2y) − 1.5·log y
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Levy<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Levy<T>& d, T x) noexcept
{
    const T y = x - d.mu;
    return d.sigma / (static_cast<T>(2) * y * y) - static_cast<T>(1.5) / y;
}
template <Real T>
void dlogpdf_dtheta(const Levy<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T y = x - d.mu;
    g[0] = -d.sigma / (static_cast<T>(2) * y * y) + static_cast<T>(1.5) / y;             // ∂/∂μ = −∂/∂x
    g[1] = static_cast<T>(0.5) / d.sigma - static_cast<T>(1) / (static_cast<T>(2) * y);  // ∂/∂σ
}

// BetaPrime(a, b) ──  θ = [a, b];  logp = (a−1)log x − (a+b)log1p(x) − lbeta(a,b)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const BetaPrime<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const BetaPrime<T>& d, T x) noexcept
{
    return (d.a - static_cast<T>(1)) / x - (d.a + d.b) / (static_cast<T>(1) + x);
}
template <Real T>
void dlogpdf_dtheta(const BetaPrime<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T psi_ab = special::digamma(d.a + d.b);
    g[0] = crd::math::log(x) - crd::math::log1p(x) - (special::digamma(d.a) - psi_ab); // ∂/∂a
    g[1] = -crd::math::log1p(x) - (special::digamma(d.b) - psi_ab);                    // ∂/∂b
}

// SkewNormal(α, ξ, ω) ──  θ = [α, ξ, ω];  z=(x−ξ)/ω;  logp = log2 − log ω − ½z² − ½ln2π + log Φ(αz);  r=φ(αz)/Φ(αz)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const SkewNormal<T>&) noexcept
{
    return 3;
}
template <Real T>
[[nodiscard]] T skewnormal_ratio(const SkewNormal<T>& d, T z) noexcept
{
    return detail::std_npdf(d.alpha * z) / detail::std_phi(d.alpha * z); // φ(αz)/Φ(αz)
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const SkewNormal<T>& d, T x) noexcept
{
    const T z = (x - d.xi) / d.omega;
    return (-z + d.alpha * skewnormal_ratio(d, z)) / d.omega;
}
template <Real T>
void dlogpdf_dtheta(const SkewNormal<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T z = (x - d.xi) / d.omega;
    const T r = skewnormal_ratio(d, z);
    g[0] = z * r;                                                            // ∂/∂α
    g[1] = (z - d.alpha * r) / d.omega;                                     // ∂/∂ξ = −∂/∂x
    g[2] = (z * z - d.alpha * r * z - static_cast<T>(1)) / d.omega;         // ∂/∂ω
}

// NoncentralChiSquared(k, λ) ──  θ = [k, λ]. ncx2 = Poisson(λ/2)-mixture of central χ²_{k+2j}. ∂x via the Bessel
// form (b=√(λx), R=I_{ν+1}/I_ν+ν/b, ν=k/2−1); ∂k & ∂λ via the mixture SERIES — the Bessel-ORDER derivative (∂k)
// has no closed form, but the equivalent Poisson-mixture makes ∂k digamma-based (SANITY 8, identical distribution).
template <Real T>
[[nodiscard]] constexpr int theta_dim(const NoncentralChiSquared<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const NoncentralChiSquared<T>& d, T x) noexcept
{
    const T nu = static_cast<T>(0.5) * d.k - static_cast<T>(1);
    const T b = crd::math::sqrt(d.lambda * x);
    const T r = special::cyl_bessel_i(nu + static_cast<T>(1), b) / special::cyl_bessel_i(nu, b) + nu / b;
    return -static_cast<T>(0.5) + (static_cast<T>(0.25) * d.k - static_cast<T>(0.5)) / x +
           r * b / (static_cast<T>(2) * x);
}
template <Real T>
void dlogpdf_dtheta(const NoncentralChiSquared<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const double xd = static_cast<double>(x);
    const double kd = static_cast<double>(d.k);
    const double ld = static_cast<double>(d.lambda);
    const double lx = crd::math::log(xd);
    const double ln2 = 0.69314718055994530942;
    const double log_half_l = crd::math::log(0.5 * ld);
    double s = 0.0;  // Σ p_j  (= pdf)
    double sk = 0.0; // Σ p_j · ∂_k log c_j
    double sl = 0.0; // Σ p_j · ∂_λ log w_j
    double logw = -0.5 * ld; // log w_0
    for (int j = 0; j < 1000; ++j)
    {
        const double m = kd + 2.0 * static_cast<double>(j); // df of the j-th central χ²
        const double logc = (0.5 * m - 1.0) * lx - 0.5 * xd - 0.5 * m * ln2 - special::lgamma(0.5 * m);
        const double pj = crd::math::exp(logw + logc);
        s += pj;
        sk += pj * (0.5 * lx - 0.5 * ln2 - 0.5 * special::digamma(0.5 * m)); // ∂/∂k log χ²_m
        sl += pj * (static_cast<double>(j) / ld - 0.5);                      // ∂/∂λ log Poisson(j; λ/2)
        if (static_cast<double>(j) > 0.5 * ld && pj < 1e-17 * s)
        {
            break;
        }
        logw += log_half_l - crd::math::log(static_cast<double>(j) + 1.0);
    }
    g[0] = static_cast<T>(sk / s); // ∂/∂k
    g[1] = static_cast<T>(sl / s); // ∂/∂λ
}

// NoncentralF(d1, d2, λ) ──  θ = [d1, d2, λ]. The SAME Poisson-mixture-of-central-betas series the shipped pdf
// sums (term_j = w_j·betadens(a=d1/2+j, b=d2/2; y)·dy/dx), differentiated per term: ∂logpdf/∂φ = Σ_j term_j·
// ∂log term_j/∂φ ÷ Σ_j term_j. d1/d2 thread through a/b, y, and the jacobian; ∂λ is the Poisson weight. SANITY 8.
template <Real T>
void ncf_grad_all(const NoncentralF<T>& d, T x, double out[4]) noexcept
{
    const double d1 = static_cast<double>(d.d1);
    const double d2 = static_cast<double>(d.d2);
    const double ld = static_cast<double>(d.lambda);
    const double xd = static_cast<double>(x);
    const double l2 = 0.5 * ld;
    const double log_l2 = crd::math::log(l2);
    const double dpx = d1 * xd + d2; // d1·x + d2
    const double y = d1 * xd / dpx;
    const double ly = crd::math::log(y);
    const double l1my = crd::math::log1p(-y);
    const double dydx = d1 * d2 / (dpx * dpx); // dy/dx
    const double dyd1 = xd * d2 / (dpx * dpx); // dy/dd1
    const double dyd2 = -d1 * xd / (dpx * dpx); // dy/dd2
    const double b = 0.5 * d2;
    const double psi_b = special::digamma(b);
    double s = 0.0;
    double sx = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    double sl = 0.0;
    double logw = -l2;
    for (int j = 0; j < 1000; ++j)
    {
        const double a = 0.5 * d1 + static_cast<double>(j);
        const double term = crd::math::exp(logw + (a - 1.0) * ly + (b - 1.0) * l1my - special::lbeta(a, b)) * dydx;
        const double br = (a - 1.0) / y - (b - 1.0) / (1.0 - y); // ∂log betadens/∂y
        const double psi_ab = special::digamma(a + b);
        s += term;
        sx += term * (br * dydx - 2.0 * d1 / dpx);
        s1 += term * (0.5 * ly + br * dyd1 - 0.5 * (special::digamma(a) - psi_ab) + 1.0 / d1 - 2.0 * xd / dpx);
        s2 += term * (br * dyd2 + 0.5 * l1my - 0.5 * (psi_b - psi_ab) + 1.0 / d2 - 2.0 / dpx);
        sl += term * (static_cast<double>(j) / ld - 0.5);
        if (j > static_cast<int>(l2) + 5 && term < 1e-17 * s)
        {
            break;
        }
        logw += log_l2 - crd::math::log(static_cast<double>(j) + 1.0);
    }
    out[0] = sx / s;
    out[1] = s1 / s;
    out[2] = s2 / s;
    out[3] = sl / s;
}
template <Real T>
[[nodiscard]] constexpr int theta_dim(const NoncentralF<T>&) noexcept
{
    return 3;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const NoncentralF<T>& d, T x) noexcept
{
    double o[4];
    ncf_grad_all(d, x, o);
    return static_cast<T>(o[0]);
}
template <Real T>
void dlogpdf_dtheta(const NoncentralF<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    double o[4];
    ncf_grad_all(d, x, o);
    g[0] = static_cast<T>(o[1]); // ∂/∂d1
    g[1] = static_cast<T>(o[2]); // ∂/∂d2
    g[2] = static_cast<T>(o[3]); // ∂/∂λ
}

// NoncentralT(ν, λ) ──  θ = [ν, λ]. logpdf = lpre(ν,λ,t) + log Σ_k g_k, g_k = Γ((ν+k+1)/2)/k!·base^k,
// base = λt√2/√(ν+t²). λ & t enter only base (∂log base^k = k·∂log base) ⇒ those grads are ⟨k⟩-weighted; ν enters
// lpre, Γ((ν+k+1)/2) (⟨digamma⟩) and base. out = ∂logpdf/∂{t, ν, λ}. Series differentiated term-by-term (SANITY 8).
template <Real T>
void nct_grad_all(const NoncentralT<T>& d, T x, double out[3]) noexcept
{
    const double nu = static_cast<double>(d.nu);
    const double lam = static_cast<double>(d.lambda);
    const double t = static_cast<double>(x);
    const double c = nu + t * t; // ν+t²
    const double base = lam * t * 1.4142135623730951 / crd::math::sqrt(c);
    const double lab = crd::math::log(crd::math::fabs(base) + 1e-300);
    const bool bneg = base < 0.0;
    double sg = 0.0;
    double sk = 0.0; // Σ k g_k
    double sd = 0.0; // Σ digamma((ν+k+1)/2) g_k
    for (int k = 0; k < 200; ++k)
    {
        const double half = 0.5 * (nu + static_cast<double>(k) + 1.0);
        const double dig = special::digamma(half);
        double term = crd::math::exp(special::lgamma(half) - special::lgamma(static_cast<double>(k) + 1.0) +
                                     static_cast<double>(k) * lab);
        if (bneg && (k & 1))
        {
            term = -term;
        }
        sg += term;
        sk += static_cast<double>(k) * term;
        sd += dig * term;
        if (k > 10 && crd::math::fabs(term) < 1e-16 * crd::math::fabs(sg))
        {
            break;
        }
    }
    const double kmean = sk / sg;
    const double dmean = sd / sg;
    out[0] = -(nu + 1.0) * t / c + (nu / (t * c)) * kmean;                              // ∂/∂t  (t≠0)
    out[1] = 0.5 * crd::math::log(nu) + 0.5 - 0.5 * special::digamma(0.5 * nu) - 0.5 * crd::math::log(c) -
             0.5 * (nu + 1.0) / c + 0.5 * dmean - 0.5 * kmean / c;                      // ∂/∂ν
    out[2] = -lam + kmean / lam;                                                        // ∂/∂λ
}
template <Real T>
[[nodiscard]] constexpr int theta_dim(const NoncentralT<T>&) noexcept
{
    return 2;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const NoncentralT<T>& d, T x) noexcept
{
    double o[3];
    nct_grad_all(d, x, o);
    return static_cast<T>(o[0]);
}
template <Real T>
void dlogpdf_dtheta(const NoncentralT<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    double o[3];
    nct_grad_all(d, x, o);
    g[0] = static_cast<T>(o[1]); // ∂/∂ν
    g[1] = static_cast<T>(o[2]); // ∂/∂λ
}

// ───────── batched log-likelihood gradients: ∇_θ Σ_i logpdf(x_i | θ) — the HMC/MLE hot path ─────────
// Summing the per-point dlogpdf_dtheta recomputes the parameter-only terms (digamma(a), …) N times. These
// depend on θ, NOT x, so they amortise to a single evaluation × N — the leapfrog/Newton step's real cost. This
// is what makes the gradient CRUSH a tape-based autograd peer (PyTorch) which can only broadcast the scalar
// param's special-fn once. (Normal has no parameter-only special-fn term, so its batch is just the sum loop.)
template <Real T>
void loglik_grad(const Normal<T>& d, const T* xs, crd::usize n, crd::containers::Span<T> grad) noexcept
{
    const T inv_s2 = static_cast<T>(1) / (d.sigma * d.sigma);
    T s_dmu = static_cast<T>(0);
    T s_z2 = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T dx = xs[i] - d.mu;
        s_dmu += dx;
        s_z2 += dx * dx;
    }
    grad[0] = s_dmu * inv_s2;                                                 // Σ ∂/∂μ
    grad[1] = (s_z2 * inv_s2 - static_cast<T>(n)) / d.sigma;                  // Σ ∂/∂σ
}
template <Real T>
void loglik_grad(const Gamma<T>& d, const T* xs, crd::usize n, crd::containers::Span<T> grad) noexcept
{
    T s_logx = static_cast<T>(0);
    T s_x = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        s_logx += crd::math::log(xs[i]);
        s_x += xs[i];
    }
    const T nn = static_cast<T>(n);
    // digamma(shape) + log(scale) computed ONCE, not per point.
    grad[0] = s_logx - nn * (crd::math::log(d.scale) + special::digamma(d.shape)); // Σ ∂/∂a
    grad[1] = s_x / (d.scale * d.scale) - nn * d.shape / d.scale;                  // Σ ∂/∂s
}
template <Real T>
void loglik_grad(const StudentT<T>& d, const T* xs, crd::usize n, crd::containers::Span<T> grad) noexcept
{
    const T h = static_cast<T>(0.5) * d.nu;
    // the two digammas + 1/(2ν) are parameter-only ⇒ one evaluation × N.
    const T cst = static_cast<T>(0.5) * (special::digamma(h + static_cast<T>(0.5)) - special::digamma(h)) -
                  static_cast<T>(0.5) / d.nu;
    T s = static_cast<T>(0);
    crd::usize i = 0;
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
    if constexpr (std::is_same_v<T, double>)
    {
        // SIMD the ν-dependent log1p reduction (4 doubles/iter) — matches/beats XLA's vectorized gradient.
        const double nu = d.nu;
        const __m256d vnu = _mm256_set1_pd(nu);
        const __m256d vone = _mm256_set1_pd(1.0);
        const __m256d vinv_nu = _mm256_set1_pd(1.0 / nu);          // x²/ν via reciprocal-multiply (no per-iter div)
        const __m256d vneg_half = _mm256_set1_pd(-0.5);
        const __m256d vk = _mm256_set1_pd((nu + 1.0) / (2.0 * nu)); // (ν+1)/(2ν), folded constant
        __m256d acc = _mm256_setzero_pd();
        for (; i + 4 <= n; i += 4)
        {
            const __m256d vx = _mm256_loadu_pd(xs + i);
            const __m256d vx2 = _mm256_mul_pd(vx, vx);
            const __m256d vlog = crd::math::crd_log4(_mm256_fmadd_pd(vx2, vinv_nu, vone));        // log(1 + x²/ν)
            const __m256d vrat = _mm256_div_pd(_mm256_mul_pd(vk, vx2), _mm256_add_pd(vnu, vx2)); // (ν+1)x²/(2ν(ν+x²))
            acc = _mm256_add_pd(acc, _mm256_fmadd_pd(vneg_half, vlog, vrat));
        }
        alignas(32) double tmp[4];
        _mm256_store_pd(tmp, acc);
        s += (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
    }
#endif
    for (; i < n; ++i) // scalar tail (and the whole loop on non-AVX2 / f32)
    {
        const T x2 = xs[i] * xs[i];
        s += -static_cast<T>(0.5) * crd::math::log1p(x2 / d.nu) +
             (d.nu + static_cast<T>(1)) * x2 / (static_cast<T>(2) * d.nu * (d.nu + x2));
    }
    grad[0] = static_cast<T>(n) * cst + s;
}

// ───────── sufficient statistics: O(1) per-leapfrog gradient over FIXED data (the real HMC hot path) ─────────
// For the exponential family, ∇_θ Σ logpdf = (data-only sufficient statistic) + (O(1) param term). HMC holds the
// data fixed across leapfrog steps, so the statistic is computed ONCE via suffstats(), then EVERY gradient is O(1)
// — exactly what JAX/XLA gets by constant-folding the data into the compiled gradient. This is THE hot path; the
// per-point loglik_grad above is the general (data-may-change) fallback.
template <Real T>
struct NormalStats
{
    T sum_x;
    T sum_xx;
    T n;
};
template <Real T>
[[nodiscard]] NormalStats<T> suffstats(const Normal<T>&, const T* xs, crd::usize n) noexcept
{
    T sx = static_cast<T>(0);
    T sxx = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        sx += xs[i];
        sxx += xs[i] * xs[i];
    }
    return {sx, sxx, static_cast<T>(n)};
}
template <Real T>
void loglik_grad(const Normal<T>& d, const NormalStats<T>& s, crd::containers::Span<T> g) noexcept
{
    const T inv_s2 = static_cast<T>(1) / (d.sigma * d.sigma);
    g[0] = (s.sum_x - s.n * d.mu) * inv_s2;                                          // ∂μ
    const T sse = s.sum_xx - static_cast<T>(2) * d.mu * s.sum_x + s.n * d.mu * d.mu; // Σ(x−μ)²
    g[1] = (sse * inv_s2 - s.n) / d.sigma;                                           // ∂σ
}
template <Real T>
struct GammaStats
{
    T sum_logx;
    T sum_x;
    T n;
};
template <Real T>
[[nodiscard]] GammaStats<T> suffstats(const Gamma<T>&, const T* xs, crd::usize n) noexcept
{
    T sl = static_cast<T>(0);
    T sx = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        sl += crd::math::log(xs[i]);
        sx += xs[i];
    }
    return {sl, sx, static_cast<T>(n)};
}
template <Real T>
void loglik_grad(const Gamma<T>& d, const GammaStats<T>& s, crd::containers::Span<T> g) noexcept
{
    g[0] = s.sum_logx - s.n * (crd::math::log(d.scale) + special::digamma(d.shape)); // ∂a
    g[1] = s.sum_x / (d.scale * d.scale) - s.n * d.shape / d.scale;                  // ∂s
}

// ───────────── DISCRETE: ∂logpmf/∂θ over the CONTINUOUS parameters (the MLE score / HMC-over-θ for a
// discrete likelihood). Integer params (Binomial/BetaBinomial n, DiscreteUniform/Hypergeometric bounds) are not
// differentiated; θ lists only the continuous params, in struct order. No ∂k (the variate is discrete). ─────────────

// Bernoulli(p) ──  θ = [p];  logpmf = k·log p + (1−k)·log(1−p)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Bernoulli<T>&) noexcept
{
    return 1;
}
template <Real T>
void dlogpmf_dtheta(const Bernoulli<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    const T kk = static_cast<T>(k);
    g[0] = kk / d.p - (static_cast<T>(1) - kk) / (static_cast<T>(1) - d.p);
}

// Binomial(n, p) ──  θ = [p] (n integer)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Binomial<T>&) noexcept
{
    return 1;
}
template <Real T>
void dlogpmf_dtheta(const Binomial<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    g[0] = static_cast<T>(k) / d.p - static_cast<T>(d.n - k) / (static_cast<T>(1) - d.p);
}

// Poisson(λ) ──  θ = [λ];  logpmf = k·log λ − λ − lgamma(k+1)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Poisson<T>&) noexcept
{
    return 1;
}
template <Real T>
void dlogpmf_dtheta(const Poisson<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    g[0] = static_cast<T>(k) / d.lambda - static_cast<T>(1);
}

// Geometric(p) ──  θ = [p];  logpmf = (k−1)·log(1−p) + log p
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Geometric<T>&) noexcept
{
    return 1;
}
template <Real T>
void dlogpmf_dtheta(const Geometric<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    g[0] = static_cast<T>(1) / d.p - static_cast<T>(k - 1) / (static_cast<T>(1) - d.p);
}

// NegativeBinomial(n, p) ──  θ = [n, p] (n real-valued);  ∂/∂n reuses digamma
template <Real T>
[[nodiscard]] constexpr int theta_dim(const NegativeBinomial<T>&) noexcept
{
    return 2;
}
template <Real T>
void dlogpmf_dtheta(const NegativeBinomial<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    g[0] = special::digamma(static_cast<T>(k) + d.n) - special::digamma(d.n) + crd::math::log(d.p); // ∂/∂n
    g[1] = d.n / d.p - static_cast<T>(k) / (static_cast<T>(1) - d.p);                                // ∂/∂p
}

// YuleSimon(α) ──  θ = [α];  logpmf = log α + lbeta(k, α+1)
template <Real T>
[[nodiscard]] constexpr int theta_dim(const YuleSimon<T>&) noexcept
{
    return 1;
}
template <Real T>
void dlogpmf_dtheta(const YuleSimon<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    g[0] = static_cast<T>(1) / d.alpha + special::digamma(d.alpha + static_cast<T>(1)) -
           special::digamma(static_cast<T>(k) + d.alpha + static_cast<T>(1));
}

// BetaBinomial(n, a, b) ──  θ = [a, b] (n integer);  ∂lbeta terms reuse digamma
template <Real T>
[[nodiscard]] constexpr int theta_dim(const BetaBinomial<T>&) noexcept
{
    return 2;
}
template <Real T>
void dlogpmf_dtheta(const BetaBinomial<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    const T kk = static_cast<T>(k);
    const T nn = static_cast<T>(d.n);
    const T psi_nab = special::digamma(nn + d.a + d.b);
    const T psi_ab = special::digamma(d.a + d.b);
    g[0] = special::digamma(kk + d.a) - psi_nab - special::digamma(d.a) + psi_ab;     // ∂/∂a
    g[1] = special::digamma(nn - kk + d.b) - psi_nab - special::digamma(d.b) + psi_ab; // ∂/∂b
}

// Logarithmic(p) ──  θ = [p];  logpmf = k·log p − log k − log(−log(1−p))
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Logarithmic<T>&) noexcept
{
    return 1;
}
template <Real T>
void dlogpmf_dtheta(const Logarithmic<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    const T l = -crd::math::log1p(-d.p); // −log(1−p) > 0
    g[0] = static_cast<T>(k) / d.p - static_cast<T>(1) / ((static_cast<T>(1) - d.p) * l);
}

// Skellam(μ1, μ2) ──  θ = [μ1, μ2];  logpmf = −(μ1+μ2) + (k/2)log(μ1/μ2) + log I_ν(b), ν=|k|, b=2√(μ1μ2).
// d/db log I_ν(b) = I_{ν+1}(b)/I_ν(b) + ν/b (the modified-Bessel log-derivative; reuses cyl_bessel_i, SANITY 8).
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Skellam<T>&) noexcept
{
    return 2;
}
template <Real T>
void dlogpmf_dtheta(const Skellam<T>& d, crd::i64 k, crd::containers::Span<T> g) noexcept
{
    const T nu = static_cast<T>(k < 0 ? -k : k);
    const T b = static_cast<T>(2) * crd::math::sqrt(d.mu1 * d.mu2);
    const T r = special::cyl_bessel_i(nu + static_cast<T>(1), b) / special::cyl_bessel_i(nu, b) + nu / b;
    const T kk = static_cast<T>(k);
    g[0] = -static_cast<T>(1) + kk / (static_cast<T>(2) * d.mu1) + r * crd::math::sqrt(d.mu2 / d.mu1); // ∂/∂μ1
    g[1] = -static_cast<T>(1) - kk / (static_cast<T>(2) * d.mu2) + r * crd::math::sqrt(d.mu1 / d.mu2); // ∂/∂μ2
}

// DiscreteUniform(low, high) and Hypergeometric: ALL parameters are integers (counts/bounds) — there is no
// continuous parameter to differentiate, so theta_dim == 0 (not a deferral: the gradient space is empty).
template <Real T>
[[nodiscard]] constexpr int theta_dim(const DiscreteUniform<T>&) noexcept
{
    return 0;
}
template <Real T>
void dlogpmf_dtheta(const DiscreteUniform<T>&, crd::i64, crd::containers::Span<T>) noexcept
{
}
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Hypergeometric<T>&) noexcept
{
    return 0;
}
template <Real T>
void dlogpmf_dtheta(const Hypergeometric<T>&, crd::i64, crd::containers::Span<T>) noexcept
{
}

// Triangular(a, mode, b) ──  θ = [a, mode, b]. Piecewise (logpdf has a kink AT x==mode — the one measure-zero
// point where ∂x is undefined; everywhere else the gradient is exact). x<mode: logp = log2+log(x−a)−log(b−a)−
// log(mode−a); x>mode: logp = log2+log(b−x)−log(b−a)−log(b−mode).
template <Real T>
[[nodiscard]] constexpr int theta_dim(const Triangular<T>&) noexcept
{
    return 3;
}
template <Real T>
[[nodiscard]] T dlogpdf_dx(const Triangular<T>& d, T x) noexcept
{
    return x < d.cmode ? static_cast<T>(1) / (x - d.a) : -static_cast<T>(1) / (d.b - x);
}
template <Real T>
void dlogpdf_dtheta(const Triangular<T>& d, T x, crd::containers::Span<T> g) noexcept
{
    const T iba = static_cast<T>(1) / (d.b - d.a);
    if (x < d.cmode)
    {
        g[0] = -static_cast<T>(1) / (x - d.a) + iba + static_cast<T>(1) / (d.cmode - d.a); // ∂/∂a
        g[1] = -static_cast<T>(1) / (d.cmode - d.a);                                       // ∂/∂mode
        g[2] = -iba;                                                                       // ∂/∂b
    }
    else
    {
        g[0] = iba;                                                                        // ∂/∂a
        g[1] = static_cast<T>(1) / (d.b - d.cmode);                                        // ∂/∂mode
        g[2] = static_cast<T>(1) / (d.b - x) - iba - static_cast<T>(1) / (d.b - d.cmode);  // ∂/∂b
    }
}

} // namespace crd::hesap::stats
