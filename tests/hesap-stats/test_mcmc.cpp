// v12-q MCMC diagnostics — rank-normalized split R-hat / bulk-ESS / autocorrelation / Geweke gated vs ArviZ on
// bit-reproducible AR(1) chains (integer-LCG noise → identical in python and C++).

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/stats/mcmc.hpp>
#include <crd/hesap/stats/mcmc_diagnostics.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::stats;
using crd::containers::ConstSpan;

namespace
{
[[nodiscard]] bool close(double a, double b, double tol = 1e-9)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}
[[nodiscard]] bool within(double a, double b, double abstol)
{
    return (a < b ? b - a : a - b) < abstol;
}
[[nodiscard]] crd::u64 lcg(crd::u64 n)
{
    return (1103515245ULL * n + 12345ULL) % 2147483648ULL;
}
} // namespace

TEST_CASE("v12-q: MCMC diagnostics (R-hat / ESS / autocorr / Geweke) vs ArviZ", "[v12-q][stats][mcmc]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 22);
    constexpr crd::usize kM = 4;
    constexpr crd::usize kN = 500;
    crd::containers::Array<double> ch(&alloc);
    ch.resize(kM * kN);
    for (crd::usize c = 0; c < kM; ++c)
    {
        ch[c * kN] = static_cast<double>(c);
        for (crd::usize t = 1; t < kN; ++t)
        {
            const double r = (static_cast<double>(lcg(c * 100000 + t) % 2000) - 1000.0) / 1000.0;
            ch[c * kN + t] = 0.7 * ch[c * kN + t - 1] + r;
        }
    }
    const auto chains = ConstSpan<double>{ch.data(), kM * kN};
    const auto c0 = ConstSpan<double>{ch.data(), kN};

    CHECK(close(ch[1], 0.59)); // chain reproducibility
    CHECK(close(ch[2], 0.6));

    CHECK(close(autocorr(c0, 1), 0.327611654407111));
    CHECK(close(autocorr(c0, 2), -0.0413164576362931));
    CHECK(close(autocorr(c0, 5), -0.370635386089755));
    CHECK(close(geweke(c0), -0.969097661277755));

    CHECK(close(rhat(chains, kM, kN, &alloc), 0.999780540060195, 1e-6));     // ArviZ az.rhat (rank)
    CHECK(close(ess_bulk(chains, kM, kN, &alloc), 808.485534731799, 1e-2));  // ArviZ az.ess (bulk)
}

TEST_CASE("v12-q: samplers recover N(0,1) + leapfrog + determinism moat", "[v12-q][stats][mcmc]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 25);
    const auto logp = [](ConstSpan<double> x) { return -0.5 * x[0] * x[0]; };
    const auto grad = [](ConstSpan<double> x, crd::containers::Span<double> g) { g[0] = -x[0]; };
    constexpr double x0v[] = {0.0};
    const auto x0 = ConstSpan<double>{x0v, 1};

    { // leapfrog bit-exact: x=1,p=1,step=0.1,1 step on U=x^2/2 → x=1.095, p=0.89525
        double xb[] = {1.0};
        double pb[] = {1.0};
        leapfrog(grad, crd::containers::Span<double>{xb, 1}, crd::containers::Span<double>{pb, 1}, 0.1, 1, &alloc);
        CHECK(close(xb[0], 1.095, 1e-12));
        CHECK(close(pb[0], 0.89525, 1e-12));
    }

    { // determinism moat: same seed → bit-identical chain
        const auto a = metropolis(logp, x0, 2000, 2.0, 7, &alloc);
        const auto b = metropolis(logp, x0, 2000, 2.0, 7, &alloc);
        bool same = (a.size() == b.size());
        for (crd::usize i = 0; i < a.size() && same; ++i)
        {
            if (a[i] != b[i])
            {
                same = false;
            }
        }
        CHECK(same);
    }

    // recover N(0,1): pooled mean≈0, var≈1, R-hat<1.05 (validated with our own diagnostics)
    const auto pool_check = [&](auto run_chain) {
        constexpr crd::usize kK = 4;
        constexpr crd::usize kNn = 5000;
        crd::containers::Array<double> pooled(&alloc);
        pooled.resize(kK * kNn);
        for (crd::usize c = 0; c < kK; ++c)
        {
            const auto ch = run_chain(static_cast<crd::u64>(100 + c));
            for (crd::usize i = 0; i < kNn; ++i)
            {
                pooled[c * kNn + i] = ch[i];
            }
        }
        const auto ps = ConstSpan<double>{pooled.data(), kK * kNn};
        CHECK(within(mean(ps), 0.0, 0.07));
        CHECK(within(variance(ps, 1), 1.0, 0.12));
        CHECK(rhat(ps, kK, kNn, &alloc) < 1.05);
    };
    pool_check([&](crd::u64 sd) { return metropolis(logp, x0, 5000, 2.4, sd, &alloc); });
    pool_check([&](crd::u64 sd) { return slice_sample(logp, x0, 5000, 1.0, sd, &alloc); });
    pool_check([&](crd::u64 sd) { return hmc(logp, grad, x0, 5000, 0.25, 10, sd, &alloc); });
    pool_check([&](crd::u64 sd) { return adaptive_metropolis(logp, x0, 5000, 1.0, 500, sd, &alloc); });
    pool_check([&](crd::u64 sd) { return nuts(logp, grad, x0, 5000, 0.5, sd, &alloc); });
    pool_check([&](crd::u64 sd) { return nuts_adapt(logp, grad, x0, 1000, 5000, 0.1, sd, &alloc); }); // dual-averaging
}

TEST_CASE("v12-q: Gibbs recovers a correlated 2D Gaussian", "[v12-q][stats][mcmc]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 24);
    constexpr double rho = 0.5;
    const double sd = crd::math::sqrt(1.0 - rho * rho);
    const auto cond = [sd](crd::usize i, ConstSpan<double> x, ThreefryRng& rng) {
        return rho * x[1 - i] + sd * detail::mcmc_normal<double>(rng); // x_i | x_j ~ N(rho x_j, 1-rho^2)
    };
    constexpr double x0v[] = {0.0, 0.0};
    const auto x0 = ConstSpan<double>{x0v, 2};
    constexpr crd::usize kNn = 20000;
    constexpr crd::usize kB = 1000; // burn-in
    const auto ch = gibbs(cond, x0, kNn, 42, &alloc);
    const crd::usize ns = kNn - kB;
    double m0 = 0;
    double m1 = 0;
    for (crd::usize s = kB; s < kNn; ++s)
    {
        m0 += ch[s * 2];
        m1 += ch[s * 2 + 1];
    }
    m0 /= static_cast<double>(ns);
    m1 /= static_cast<double>(ns);
    double v0 = 0;
    double v1 = 0;
    double c01 = 0;
    for (crd::usize s = kB; s < kNn; ++s)
    {
        const double d0 = ch[s * 2] - m0;
        const double d1 = ch[s * 2 + 1] - m1;
        v0 += d0 * d0;
        v1 += d1 * d1;
        c01 += d0 * d1;
    }
    v0 /= static_cast<double>(ns - 1);
    v1 /= static_cast<double>(ns - 1);
    c01 /= static_cast<double>(ns - 1);
    CHECK(within(m0, 0.0, 0.06));
    CHECK(within(m1, 0.0, 0.06));
    CHECK(within(v0, 1.0, 0.1));
    CHECK(within(v1, 1.0, 0.1));
    CHECK(within(c01, 0.5, 0.1)); // recovers the off-diagonal covariance
}

TEST_CASE("v12-q: SMC recovers a Gaussian posterior", "[v12-q][stats][mcmc]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 24);
    const auto log_prior = [](ConstSpan<double> x) { return -0.5 * (x[0] / 10.0) * (x[0] / 10.0); }; // N(0, 100)
    const auto log_lik = [](ConstSpan<double> x) {
        const double e = x[0] - 3.0;
        return -0.5 * e * e; // N(x; 3, 1)
    };
    const auto prior_sample = [](crd::containers::Span<double> out, ThreefryRng& rng) {
        out[0] = 10.0 * detail::mcmc_normal<double>(rng);
    };
    // posterior: 1/var = 1/100 + 1 → var=0.990099, mean=var*3=2.970297
    const auto r = smc(log_prior, log_lik, prior_sample, 4000U, 1U, 25U, 5U, 0.5, 7U, &alloc);
    double wm = 0;
    for (crd::usize i = 0; i < r.n_particles; ++i)
    {
        wm += r.weights[i] * r.particles[i];
    }
    double wv = 0;
    for (crd::usize i = 0; i < r.n_particles; ++i)
    {
        const double e = r.particles[i] - wm;
        wv += r.weights[i] * e * e;
    }
    CHECK(within(wm, 2.970297, 0.1));  // posterior mean
    CHECK(within(wv, 0.990099, 0.15)); // posterior variance
}
