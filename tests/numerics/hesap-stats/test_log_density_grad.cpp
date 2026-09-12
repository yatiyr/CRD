// v12-l — analytic log-density gradients gated against central finite-difference of the SAME logpdf. The analytic
// ∂logp/∂x and ∂logp/∂θ must match a 2nd-order central FD to ~1e-5 (FD's own floor). Param gradients reconstruct
// the distribution with the perturbed parameter (Gamma/Beta/… cache lgamma/lbeta in their ctor, so a field poke
// would leave a stale cache) via a per-distribution `make(i, δ)` lambda. Determinism: every gradient routes
// through crd::math::* (the moat holds for free).

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/stats/log_density_grad.hpp>
#include <crd/hesap/stats/multivariate.hpp>

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

namespace
{
using namespace crd::hesap::stats;

constexpr double kH = 1e-5;

[[nodiscard]] bool close(double a, double b) noexcept
{
    return crd::math::fabs(a - b) <= 1e-5 + 1e-5 * crd::math::fabs(b);
}

template <class D>
[[nodiscard]] double fd_x(const D& d, double x) noexcept
{
    return (d.logpdf(x + kH) - d.logpdf(x - kH)) / (2.0 * kH);
}

// FD-check ∂x and every ∂θ_i; `make(i, δ)` returns the distribution with parameter i shifted by δ.
template <class D, class Make>
void check(const D& d, double x, Make make)
{
    CHECK(close(dlogpdf_dx(d, x), fd_x(d, x)));
    const int k = theta_dim(d);
    double g[8] = {};
    dlogpdf_dtheta(d, x, crd::containers::Span<double>{g, static_cast<crd::usize>(k)});
    for (int i = 0; i < k; ++i)
    {
        const double fd = (make(i, kH).logpdf(x) - make(i, -kH).logpdf(x)) / (2.0 * kH);
        CHECK(close(g[i], fd));
    }
}

template <class T>
[[nodiscard]] T pin(int i, int want, T base, double d) noexcept
{
    return i == want ? base + static_cast<T>(d) : base;
}

// Discrete: FD-check every continuous ∂θ_i at a fixed integer k; `make(i, δ)` shifts continuous param i.
template <class D, class Make>
void check_discrete(const D& d, crd::i64 k, Make make)
{
    const int kk = theta_dim(d);
    double g[4] = {};
    dlogpmf_dtheta(d, k, crd::containers::Span<double>{g, static_cast<crd::usize>(kk)});
    for (int i = 0; i < kk; ++i)
    {
        const double fd = (make(i, kH).logpmf(k) - make(i, -kH).logpmf(k)) / (2.0 * kH);
        CHECK(close(g[i], fd));
    }
}
} // namespace

TEST_CASE("v12-l: Normal log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Normal<double> d(1.5, 2.0);
    for (double x : {-3.0, 0.0, 1.5, 4.0})
        check(d, x, [](int i, double e) { return Normal<double>(pin(i, 0, 1.5, e), pin(i, 1, 2.0, e)); });
}
TEST_CASE("v12-l: LogNormal log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const LogNormal<double> d(0.3, 0.8);
    for (double x : {0.3, 1.0, 3.0})
        check(d, x, [](int i, double e) { return LogNormal<double>(pin(i, 0, 0.3, e), pin(i, 1, 0.8, e)); });
}
TEST_CASE("v12-l: Exponential log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Exponential<double> d(2.5);
    for (double x : {0.3, 2.0, 6.0})
        check(d, x, [](int i, double e) { return Exponential<double>(pin(i, 0, 2.5, e)); });
}
TEST_CASE("v12-l: Gamma log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Gamma<double> d(2.7, 1.3);
    for (double x : {0.4, 2.0, 5.5})
        check(d, x, [](int i, double e) { return Gamma<double>(pin(i, 0, 2.7, e), pin(i, 1, 1.3, e)); });
}
TEST_CASE("v12-l: Beta log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Beta<double> d(2.3, 3.1);
    for (double x : {0.15, 0.5, 0.85})
        check(d, x, [](int i, double e) { return Beta<double>(pin(i, 0, 2.3, e), pin(i, 1, 3.1, e)); });
}
TEST_CASE("v12-l: ChiSquared log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const ChiSquared<double> d(4.0);
    for (double x : {0.5, 2.0, 5.0})
        check(d, x, [](int i, double e) { return ChiSquared<double>(pin(i, 0, 4.0, e)); });
}
TEST_CASE("v12-l: Cauchy log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Cauchy<double> d(0.5, 1.4);
    for (double x : {-2.0, 0.5, 3.0})
        check(d, x, [](int i, double e) { return Cauchy<double>(pin(i, 0, 0.5, e), pin(i, 1, 1.4, e)); });
}
TEST_CASE("v12-l: Laplace log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Laplace<double> d(0.5, 1.2);
    for (double x : {-1.0, 1.5, 4.0}) // away from the loc kink
        check(d, x, [](int i, double e) { return Laplace<double>(pin(i, 0, 0.5, e), pin(i, 1, 1.2, e)); });
}
TEST_CASE("v12-l: Logistic log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Logistic<double> d(0.5, 1.3);
    for (double x : {-2.0, 0.5, 3.0})
        check(d, x, [](int i, double e) { return Logistic<double>(pin(i, 0, 0.5, e), pin(i, 1, 1.3, e)); });
}
TEST_CASE("v12-l: Weibull log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Weibull<double> d(1.8, 1.5);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return Weibull<double>(pin(i, 0, 1.8, e), pin(i, 1, 1.5, e)); });
}
TEST_CASE("v12-l: Gumbel log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Gumbel<double> d(0.5, 1.3);
    for (double x : {-1.0, 0.5, 3.0})
        check(d, x, [](int i, double e) { return Gumbel<double>(pin(i, 0, 0.5, e), pin(i, 1, 1.3, e)); });
}
TEST_CASE("v12-l: Pareto log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Pareto<double> d(2.5, 1.0); // b, scale; support x >= scale
    for (double x : {1.2, 3.0, 8.0})
        check(d, x, [](int i, double e) { return Pareto<double>(pin(i, 0, 2.5, e), pin(i, 1, 1.0, e)); });
}
TEST_CASE("v12-l: Rayleigh log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Rayleigh<double> d(1.4);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return Rayleigh<double>(pin(i, 0, 1.4, e)); });
}
TEST_CASE("v12-l: Maxwell log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Maxwell<double> d(1.4);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return Maxwell<double>(pin(i, 0, 1.4, e)); });
}
TEST_CASE("v12-l: Uniform log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Uniform<double> d(0.0, 2.0);
    for (double x : {0.3, 1.0, 1.7})
        check(d, x, [](int i, double e) { return Uniform<double>(pin(i, 0, 0.0, e), pin(i, 1, 2.0, e)); });
}
TEST_CASE("v12-l: HalfNormal log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const HalfNormal<double> d(1.4);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return HalfNormal<double>(pin(i, 0, 1.4, e)); });
}
TEST_CASE("v12-l: HalfCauchy log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const HalfCauchy<double> d(1.4);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return HalfCauchy<double>(pin(i, 0, 1.4, e)); });
}
TEST_CASE("v12-l: InverseGamma log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const InverseGamma<double> d(3.0, 1.5);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return InverseGamma<double>(pin(i, 0, 3.0, e), pin(i, 1, 1.5, e)); });
}
TEST_CASE("v12-l: StudentT log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const StudentT<double> d(5.0);
    for (double x : {-2.0, 0.5, 3.0})
        check(d, x, [](int i, double e) { return StudentT<double>(pin(i, 0, 5.0, e)); });
}
TEST_CASE("v12-l: FisherF log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const FisherF<double> d(5.0, 8.0);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return FisherF<double>(pin(i, 0, 5.0, e), pin(i, 1, 8.0, e)); });
}
TEST_CASE("v12-l: Nakagami log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Nakagami<double> d(1.5, 1.3);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return Nakagami<double>(pin(i, 0, 1.5, e), pin(i, 1, 1.3, e)); });
}
TEST_CASE("v12-l: Wald log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Wald<double> d(1.2, 2.0);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return Wald<double>(pin(i, 0, 1.2, e), pin(i, 1, 2.0, e)); });
}
TEST_CASE("v12-l: VonMises log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const VonMises<double> d(0.3, 2.0);
    for (double x : {-1.0, 0.3, 2.0}) // on (μ−π, μ+π]
        check(d, x, [](int i, double e) { return VonMises<double>(pin(i, 0, 0.3, e), pin(i, 1, 2.0, e)); });
}
TEST_CASE("v12-l: Rice log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Rice<double> d(1.5, 1.2);
    for (double x : {0.4, 1.5, 4.0})
        check(d, x, [](int i, double e) { return Rice<double>(pin(i, 0, 1.5, e), pin(i, 1, 1.2, e)); });
}

// The amortized batch loglik_grad (the HMC hot path) must equal the sum of the FD-gated per-point ∂θ — same math,
// the parameter-only terms (digamma) just hoisted out of the loop. Verifies the crush path is correct, not only fast.
TEST_CASE("v12-l: batched loglik_grad matches summed per-point", "[v12-l][stats][grad]")
{
    double xs[64];
    for (int i = 0; i < 64; ++i)
    {
        xs[i] = 0.5 + 0.02 * static_cast<double>(i); // (0.5, 1.78): in-support for Normal/Gamma/StudentT
    }
    const auto check_batch = [&](const auto& d) {
        const int k = theta_dim(d);
        double gb[4] = {};
        double gs[4] = {};
        loglik_grad(d, xs, 64, crd::containers::Span<double>{gb, static_cast<crd::usize>(k)});
        for (int i = 0; i < 64; ++i)
        {
            double g[4];
            dlogpdf_dtheta(d, xs[i], crd::containers::Span<double>{g, static_cast<crd::usize>(k)});
            for (int j = 0; j < k; ++j)
            {
                gs[j] += g[j];
            }
        }
        for (int j = 0; j < k; ++j)
        {
            CHECK(close(gb[j], gs[j]));
        }
    };
    check_batch(Normal<double>(1.0, 2.0));
    check_batch(Gamma<double>(2.5, 1.3));
    check_batch(StudentT<double>(5.0));
}

// The O(1) sufficient-statistics gradient (HMC hot path) must equal the per-point batch — same score, the
// data-only stat just precomputed once.
TEST_CASE("v12-l: sufficient-statistics loglik_grad == per-point batch", "[v12-l][stats][grad]")
{
    double xs[64];
    for (int i = 0; i < 64; ++i)
    {
        xs[i] = 0.5 + 0.02 * static_cast<double>(i);
    }
    {
        const Normal<double> d(1.0, 2.0);
        double gb[2];
        double gs[2];
        loglik_grad(d, xs, 64, crd::containers::Span<double>{gb, 2});
        loglik_grad(d, suffstats(d, xs, 64), crd::containers::Span<double>{gs, 2});
        CHECK(close(gb[0], gs[0]));
        CHECK(close(gb[1], gs[1]));
    }
    {
        const Gamma<double> d(2.5, 1.3);
        double gb[2];
        double gs[2];
        loglik_grad(d, xs, 64, crd::containers::Span<double>{gb, 2});
        loglik_grad(d, suffstats(d, xs, 64), crd::containers::Span<double>{gs, 2});
        CHECK(close(gb[0], gs[0]));
        CHECK(close(gb[1], gs[1]));
    }
}

// ───────────────────────────── discrete ∂logpmf/∂θ ─────────────────────────────
TEST_CASE("v12-l: Bernoulli logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const Bernoulli<double> d(0.35);
    for (crd::i64 k : {0, 1})
        check_discrete(d, k, [](int i, double e) { return Bernoulli<double>(pin(i, 0, 0.35, e)); });
}
TEST_CASE("v12-l: Binomial logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const Binomial<double> d(10, 0.4);
    for (crd::i64 k : {2, 5, 8})
        check_discrete(d, k, [](int i, double e) { return Binomial<double>(10, pin(i, 0, 0.4, e)); });
}
TEST_CASE("v12-l: Poisson logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const Poisson<double> d(3.0);
    for (crd::i64 k : {1, 3, 6})
        check_discrete(d, k, [](int i, double e) { return Poisson<double>(pin(i, 0, 3.0, e)); });
}
TEST_CASE("v12-l: Geometric logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const Geometric<double> d(0.3);
    for (crd::i64 k : {1, 3, 7})
        check_discrete(d, k, [](int i, double e) { return Geometric<double>(pin(i, 0, 0.3, e)); });
}
TEST_CASE("v12-l: NegativeBinomial logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const NegativeBinomial<double> d(4.0, 0.4);
    for (crd::i64 k : {1, 4, 9})
        check_discrete(d, k, [](int i, double e) { return NegativeBinomial<double>(pin(i, 0, 4.0, e), pin(i, 1, 0.4, e)); });
}
TEST_CASE("v12-l: YuleSimon logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const YuleSimon<double> d(2.5);
    for (crd::i64 k : {1, 3, 6})
        check_discrete(d, k, [](int i, double e) { return YuleSimon<double>(pin(i, 0, 2.5, e)); });
}
TEST_CASE("v12-l: BetaBinomial logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const BetaBinomial<double> d(10, 2.0, 3.0);
    for (crd::i64 k : {2, 5, 8})
        check_discrete(d, k, [](int i, double e) { return BetaBinomial<double>(10, pin(i, 0, 2.0, e), pin(i, 1, 3.0, e)); });
}
TEST_CASE("v12-l: Logarithmic logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const Logarithmic<double> d(0.5);
    for (crd::i64 k : {1, 2, 5})
        check_discrete(d, k, [](int i, double e) { return Logarithmic<double>(pin(i, 0, 0.5, e)); });
}

TEST_CASE("v12-l: Triangular log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Triangular<double> d(0.0, 0.5, 1.5);
    for (double x : {0.2, 0.8, 1.2}) // away from the mode kink at 0.5 (both branches covered)
        check(d, x,
              [](int i, double e) { return Triangular<double>(pin(i, 0, 0.0, e), pin(i, 1, 0.5, e), pin(i, 2, 1.5, e)); });
}
TEST_CASE("v12-l: Skellam logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const Skellam<double> d(2.5, 1.8);
    for (crd::i64 k : {-2, 0, 3})
        check_discrete(d, k, [](int i, double e) { return Skellam<double>(pin(i, 0, 2.5, e), pin(i, 1, 1.8, e)); });
}
TEST_CASE("v12-l: Zipf logpmf gradient vs FD", "[v12-l][stats][grad]")
{
    const Zipf<double> d(2.5);
    for (crd::i64 k : {1, 2, 5})
        check_discrete(d, k, [](int i, double e) { return Zipf<double>(pin(i, 0, 2.5, e)); });
}
TEST_CASE("v12-l: integer-only discretes have empty gradient space", "[v12-l][stats][grad]")
{
    CHECK(theta_dim(DiscreteUniform<double>{}) == 0);
    CHECK(theta_dim(Hypergeometric<double>{}) == 0);
}

// ───────────────────────────── heavy-tail ∂x + ∂θ ─────────────────────────────
TEST_CASE("v12-l: GEV log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const GEV<double> d(0.2, 0.5, 1.3);
    for (double x : {0.0, 2.0, 5.0})
        check(d, x, [](int i, double e) { return GEV<double>(pin(i, 0, 0.2, e), pin(i, 1, 0.5, e), pin(i, 2, 1.3, e)); });
}
TEST_CASE("v12-l: GPD log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const GPD<double> d(0.2, 0.5, 1.3);
    for (double x : {1.0, 3.0, 6.0})
        check(d, x, [](int i, double e) { return GPD<double>(pin(i, 0, 0.2, e), pin(i, 1, 0.5, e), pin(i, 2, 1.3, e)); });
}
TEST_CASE("v12-l: Levy log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const Levy<double> d(0.5, 1.3);
    for (double x : {1.0, 2.0, 5.0})
        check(d, x, [](int i, double e) { return Levy<double>(pin(i, 0, 0.5, e), pin(i, 1, 1.3, e)); });
}
TEST_CASE("v12-l: BetaPrime log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const BetaPrime<double> d(2.5, 3.5);
    for (double x : {0.3, 1.5, 4.0})
        check(d, x, [](int i, double e) { return BetaPrime<double>(pin(i, 0, 2.5, e), pin(i, 1, 3.5, e)); });
}
TEST_CASE("v12-l: SkewNormal log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const SkewNormal<double> d(2.0, 0.5, 1.3);
    for (double x : {-1.0, 0.5, 3.0})
        check(d, x,
              [](int i, double e) { return SkewNormal<double>(pin(i, 0, 2.0, e), pin(i, 1, 0.5, e), pin(i, 2, 1.3, e)); });
}

TEST_CASE("v12-l: NoncentralChiSquared log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const NoncentralChiSquared<double> d(3.0, 2.0);
    for (double x : {1.0, 3.0, 6.0})
        check(d, x, [](int i, double e) { return NoncentralChiSquared<double>(pin(i, 0, 3.0, e), pin(i, 1, 2.0, e)); });
}
TEST_CASE("v12-l: NoncentralF log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const NoncentralF<double> d(5.0, 8.0, 2.0);
    for (double x : {0.5, 1.5, 4.0})
        check(d, x,
              [](int i, double e) { return NoncentralF<double>(pin(i, 0, 5.0, e), pin(i, 1, 8.0, e), pin(i, 2, 2.0, e)); });
}
TEST_CASE("v12-l: NoncentralT log-density gradients vs FD", "[v12-l][stats][grad]")
{
    const NoncentralT<double> d(4.0, 1.5);
    for (double x : {0.5, 1.5, 3.0}) // t>0 ⇒ base>0 (no series sign-cancellation)
        check(d, x, [](int i, double e) { return NoncentralT<double>(pin(i, 0, 4.0, e), pin(i, 1, 1.5, e)); });
}

// ───────────────────────────── multivariate ∇_x logpdf (the HMC gradient) ─────────────────────────────
TEST_CASE("v12-l: MultivariateNormal dlogpdf_dx vs FD", "[v12-l][stats][grad]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    const double mean[3] = {0.5, -1.0, 2.0};
    const double cov[9] = {2.0, 0.3, 0.1, 0.3, 1.5, 0.2, 0.1, 0.2, 1.0}; // SPD
    const MultivariateNormal<double> d(&alloc, crd::containers::Span<const double>{mean, 3},
                                       crd::containers::Span<const double>{cov, 9});
    const double x[3] = {1.0, 0.0, 1.5};
    double grad[3];
    d.dlogpdf_dx(crd::containers::Span<const double>{x, 3}, crd::containers::Span<double>{grad, 3});
    for (int j = 0; j < 3; ++j)
    {
        double xp[3];
        double xm[3];
        for (int i = 0; i < 3; ++i)
        {
            xp[i] = x[i];
            xm[i] = x[i];
        }
        xp[j] += kH;
        xm[j] -= kH;
        const double fd = (d.logpdf(crd::containers::Span<const double>{xp, 3}) -
                           d.logpdf(crd::containers::Span<const double>{xm, 3})) /
                          (2.0 * kH);
        CHECK(close(grad[j], fd));
    }
}
TEST_CASE("v12-l: MultivariateT dlogpdf_dx vs FD", "[v12-l][stats][grad]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    const double loc[2] = {0.5, -1.0};
    const double shape[4] = {1.5, 0.2, 0.2, 1.0}; // SPD shape matrix
    const MultivariateT<double> d(&alloc, crd::containers::Span<const double>{loc, 2},
                                  crd::containers::Span<const double>{shape, 4}, 5.0);
    const double x[2] = {1.2, -0.3};
    double grad[2];
    d.dlogpdf_dx(crd::containers::Span<const double>{x, 2}, crd::containers::Span<double>{grad, 2});
    for (int j = 0; j < 2; ++j)
    {
        double xp[2];
        double xm[2];
        for (int i = 0; i < 2; ++i)
        {
            xp[i] = x[i];
            xm[i] = x[i];
        }
        xp[j] += kH;
        xm[j] -= kH;
        const double fd = (d.logpdf(crd::containers::Span<const double>{xp, 2}) -
                           d.logpdf(crd::containers::Span<const double>{xm, 2})) /
                          (2.0 * kH);
        CHECK(close(grad[j], fd));
    }
}
