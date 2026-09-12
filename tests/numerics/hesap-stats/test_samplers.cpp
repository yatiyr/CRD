// crd-hesap-stats v12-f — distribution samplers. Gated by moments (mean/variance vs theory) + empirical-CDF-at-
// fixed-points vs the true CDF (hesap-special: erfc/gammainc/betainc — no sorting, the std-sort guard forbids it) +
// determinism (same seed ⇒ same samples). Tolerances are ~9σ of the sampling error ⇒ catch wrong dists, never flaky.

#include <crd/hesap/stats/stats.hpp>

#include <crd/hesap/special/special.hpp> // erfc · gammainc_p/q · betainc (CDFs for the gates)
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

namespace st = crd::hesap::stats;
namespace sp = crd::hesap::special;
using crd::u64;

namespace
{
constexpr int kN = 400000;

struct Moments
{
    double mean;
    double var;
};

template <class Fn>
Moments moments(Fn draw)
{
    double s = 0.0;
    double s2 = 0.0;
    for (int i = 0; i < kN; ++i)
    {
        const double x = draw();
        s += x;
        s2 += x * x;
    }
    const double mean = s / kN;
    return {mean, s2 / kN - mean * mean};
}

// empirical fraction of draws ≤ t
template <class Fn>
double frac_below(Fn draw, double t)
{
    int c = 0;
    for (int i = 0; i < kN; ++i)
    {
        if (draw() <= t)
        {
            ++c;
        }
    }
    return static_cast<double>(c) / kN;
}

double normal_cdf(double x)
{
    return 0.5 * sp::erfc(-x / std::sqrt(2.0));
}
} // namespace

TEST_CASE("samplers: ziggurat standard_normal", "[v12-f][stats][sampler]")
{
    st::Pcg64Dxsm g(12345);
    const Moments m = moments([&] { return st::standard_normal(g); });
    CHECK(std::fabs(m.mean) < 0.02);
    CHECK(std::fabs(m.var - 1.0) < 0.03);
    st::Pcg64Dxsm g2(7);
    for (double t : {-2.0, -1.0, 0.0, 1.0, 2.0})
    {
        CHECK(std::fabs(frac_below([&] { return st::standard_normal(g2); }, t) - normal_cdf(t)) < 0.01);
    }
}

TEST_CASE("samplers: ziggurat standard_exponential", "[v12-f][stats][sampler]")
{
    st::Pcg64Dxsm g(999);
    const Moments m = moments([&] { return st::standard_exponential(g); });
    CHECK(std::fabs(m.mean - 1.0) < 0.02);
    CHECK(std::fabs(m.var - 1.0) < 0.04);
    st::Pcg64Dxsm g2(1);
    CHECK(std::fabs(frac_below([&] { return st::standard_exponential(g2); }, 1.0) - (1.0 - std::exp(-1.0))) < 0.01);
}

TEST_CASE("samplers: gamma (Marsaglia-Tsang)", "[v12-f][stats][sampler]")
{
    for (double shape : {0.5, 2.5, 10.0}) // <1 (boost) + ≥1
    {
        st::Pcg64Dxsm g(static_cast<u64>(shape * 100));
        const Moments m = moments([&] { return st::gamma_dist(g, shape, 2.0); });
        CHECK(m.mean == Catch::Approx(shape * 2.0).epsilon(0.03));   // E = shape·scale
        CHECK(m.var == Catch::Approx(shape * 4.0).epsilon(0.06));    // Var = shape·scale²
    }
    st::Pcg64Dxsm g(42);
    const double cdf_at_5 = sp::gammainc_p(2.5, 5.0 / 2.0); // P(2.5, x/scale)
    CHECK(std::fabs(frac_below([&] { return st::gamma_dist(g, 2.5, 2.0); }, 5.0) - cdf_at_5) < 0.01);
}

TEST_CASE("samplers: beta", "[v12-f][stats][sampler]")
{
    const double a = 2.0;
    const double b = 5.0;
    st::Pcg64Dxsm g(31);
    const Moments m = moments([&] { return st::beta_dist(g, a, b); });
    CHECK(m.mean == Catch::Approx(a / (a + b)).epsilon(0.02));
    CHECK(m.var == Catch::Approx(a * b / ((a + b) * (a + b) * (a + b + 1.0))).epsilon(0.06));
    st::Pcg64Dxsm g2(32);
    CHECK(std::fabs(frac_below([&] { return st::beta_dist(g2, a, b); }, 0.3) - sp::betainc(a, b, 0.3)) < 0.01);
}

TEST_CASE("samplers: Poisson (Knuth + PTRS)", "[v12-f][stats][sampler]")
{
    for (double lam : {4.0, 30.0, 200.0}) // small (Knuth) + large (PTRS)
    {
        st::Pcg64Dxsm g(static_cast<u64>(lam));
        const Moments m = moments([&] { return static_cast<double>(st::poisson(g, lam)); });
        CHECK(m.mean == Catch::Approx(lam).epsilon(0.02));
        CHECK(m.var == Catch::Approx(lam).epsilon(0.05));
    }
    st::Pcg64Dxsm g(5);
    // P(X ≤ 4) for λ=4 is Q(5, 4) = gammainc_q(5,4).
    CHECK(std::fabs(frac_below([&] { return static_cast<double>(st::poisson(g, 4.0)); }, 4.0) - sp::gammainc_q(5.0, 4.0)) <
          0.01);
}

TEST_CASE("samplers: binomial (BINV + BTPE)", "[v12-f][stats][sampler]")
{
    struct Case
    {
        crd::i64 n;
        double p;
    };
    for (Case c : {Case{20, 0.3}, Case{200, 0.4}, Case{1000, 0.5}, Case{500, 0.95}}) // small + large + symmetry
    {
        st::Pcg64Dxsm g(static_cast<u64>(c.n) * 7 + 1);
        const Moments m = moments([&] { return static_cast<double>(st::binomial(g, c.n, c.p)); });
        const double mean = static_cast<double>(c.n) * c.p;
        CHECK(m.mean == Catch::Approx(mean).epsilon(0.02));
        CHECK(m.var == Catch::Approx(mean * (1.0 - c.p)).epsilon(0.06));
    }
}

TEST_CASE("samplers: Vose alias categorical", "[v12-f][stats][sampler]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    const double w[5] = {1.0, 2.0, 3.0, 0.0, 4.0};
    const double total = 10.0;
    st::AliasTable table;
    table.build(crd::containers::Span<const double>(w, 5), &alloc);
    crd::u32 counts[5] = {0, 0, 0, 0, 0};
    st::Pcg64Dxsm g(77);
    for (int i = 0; i < kN; ++i)
    {
        ++counts[table.sample(g)];
    }
    for (int k = 0; k < 5; ++k)
    {
        CHECK(std::fabs(static_cast<double>(counts[k]) / kN - w[k] / total) < 0.01);
    }
}

TEST_CASE("samplers: determinism - same seed gives same samples", "[v12-f][stats][sampler][moat]")
{
    st::Xoshiro256ss a(2024);
    st::Xoshiro256ss b(2024);
    for (int i = 0; i < 5000; ++i)
    {
        REQUIRE(st::standard_normal(a) == st::standard_normal(b));
    }
    st::Pcg64Dxsm c(5);
    st::Pcg64Dxsm d(5);
    for (int i = 0; i < 5000; ++i)
    {
        REQUIRE(st::poisson(c, 25.0) == st::poisson(d, 25.0));
        REQUIRE(st::binomial(c, 300, 0.4) == st::binomial(d, 300, 0.4));
    }
}

TEST_CASE("samplers: BinomialSampler matches free binomial bit-for-bit", "[v12-f][stats][sampler][moat]")
{
    // The cached sampler precomputes the (n,p) setup once but must consume the RNG identically to the one-off free
    // function (same next_double order) ⇒ identical draws. Covers BINV (np<30), BTPE (np>=30), and reflection (p>0.5).
    struct NP
    {
        crd::i64 n;
        double p;
    };
    for (NP c : {NP{20, 0.3}, NP{200, 0.4}, NP{1000, 0.5}, NP{500, 0.95}})
    {
        st::Pcg64Dxsm a(2026);
        st::Pcg64Dxsm b(2026);
        const st::BinomialSampler s(c.n, c.p);
        for (int i = 0; i < 5000; ++i)
        {
            REQUIRE(s.sample(a) == st::binomial(b, c.n, c.p));
        }
    }
}
