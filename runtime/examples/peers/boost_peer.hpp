#pragma once

// The Boost peer of the special-function, distribution, interpolation and odeint boards (ADR-0134 §8). The pinned
// Boost release (cmake/pins.json boost) is compiled in a shared library of its own at the measuring host's best ISA
// (crd_add_bench_peer), apart from the bench units, which build at the baseline like all Cerid code. Boost runs under
// its default policy, as its own build would: a double is promoted to long double, which is x87's 64-bit significand
// with GCC and Clang on x64, software binary128 on AArch64 Linux and double itself on MSVC, so the toolchains' numbers
// differ by design (isa() names the significand). One exported call evaluates a whole array; no call per element is
// timed.

#include "peer_api.hpp"

#include <cstdint>

namespace crd_peer::boost_math
{
using Index = std::int64_t;

// The timed functions under their Boost names, each with its arguments in Boost's order.
enum class Special : std::uint8_t
{
    Erf,        // erf(x)
    Erfc,       // erfc(x)
    ErfInv,     // erf_inv(x)
    Lgamma,     // lgamma(x)
    Tgamma,     // tgamma(x)
    Digamma,    // digamma(x)
    GammaP,     // gamma_p(a, x)
    CylBesselJ, // cyl_bessel_j(nu, x)
    CylNeumann, // cyl_neumann(nu, x)
    CylBesselI, // cyl_bessel_i(nu, x)
    CylBesselK, // cyl_bessel_k(nu, x)
    AiryAi,     // airy_ai(x)
    AiryBi,     // airy_bi(x)
    Ei,         // expint(x)
    E1,         // expint(1, x)
    Zeta,       // zeta(s)
    LambertW0,  // lambert_w0(x)
    Ellint1,    // ellint_1(k): the complete integral of the first kind of modulus k
    Ellint2,    // ellint_2(k): the complete integral of the second kind of modulus k
    EllintRf,   // ellint_rf(x, y, z)
};

// The arguments of n evaluations: the i-th call takes first[i], then second[i] and third[i] when the function has
// them (unused ones may be null).
struct Arguments
{
    const double* first;
    const double* second;
    const double* third;
};

// The build: the pinned Boost.Math version, the evaluation type of its double policy and the library's ISA flags.
CRD_BENCH_PEER_API const char* isa() noexcept;

// out[i] = f(arguments at i) for i < n; out does not overlap the arguments.
CRD_BENCH_PEER_API void eval(Special f, const Arguments& arguments, double* out, Index n);

// The timed distributions under their Boost names (boost/math/distributions).
enum class Distribution : std::uint8_t
{
    Normal,           // normal(mean, sd)
    LogNormal,        // lognormal(location, scale)
    Gamma,            // gamma_distribution(shape, scale)
    Beta,             // beta_distribution(alpha, beta)
    Exponential,      // exponential(lambda)
    ChiSquared,       // chi_squared(df)
    StudentsT,        // students_t(df)
    FisherF,          // fisher_f(df1, df2)
    Cauchy,           // cauchy(location, scale)
    Weibull,          // weibull(shape, scale)
    ExtremeValue,     // extreme_value(location, scale)
    Pareto,           // pareto(scale, shape)
    Binomial,         // binomial(trials, p)
    Poisson,          // poisson(mean)
    Geometric,        // geometric(p): failures before the first success
    NegativeBinomial, // negative_binomial(successes, p)
};

// The distribution's constructor arguments in Boost's order; a one-parameter distribution reads only the first.
struct Parameters
{
    double first;
    double second;
};

enum class Density : std::uint8_t
{
    Pdf, // the density, or the mass of a discrete distribution
    Cdf,
};

// out[i] = the density or distribution function of the distribution at x[i] for i < n.
CRD_BENCH_PEER_API void eval(Distribution distribution, Parameters parameters, Density density, const double* x,
                             double* out, Index n);
} // namespace crd_peer::boost_math

namespace crd_peer::boost_odeint
{
// The Van der Pol oscillator y0' = y1, y1' = mu (1 - y0^2) y1 - y0 from (y0, y1) at t = 0 to t1. Its right-hand side
// is compiled into the peer, as an odeint user's own system is.
struct VanDerPol
{
    double mu;
    double t1;
    double y0[2];
};

struct Tolerances
{
    double absolute;
    double relative;
    double first_step;
};

struct Integration
{
    double y[2];                  // the state at t1
    std::int64_t rhs_evaluations; // calls of the right-hand side
    std::int64_t steps;           // accepted steps
};

// One integrate_adaptive run of the controlled runge_kutta_dopri5 stepper over the whole interval.
CRD_BENCH_PEER_API Integration dopri5(const VanDerPol& problem, const Tolerances& tolerances);
} // namespace crd_peer::boost_odeint
