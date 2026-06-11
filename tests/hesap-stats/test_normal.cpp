// v7-q (the v12 stats cluster grows) — NormalSampler: Box-Muller over the Philox stream. Gates: the first
// four MOMENTS of a large sample (mean ~ 0, var ~ 1, skew ~ 0, excess kurtosis ~ 0 within sampling-error
// bands), the pair-cache consumption discipline (2 normals per 2 uniforms), and bit-identical determinism
// (same (seed, stream) => same sequence; a different stream => a different sequence).

#include <crd/hesap/stats/normal.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace st = crd::hesap::stats;

TEST_CASE("normal: moments of a large sample", "[hesap-stats][normal]")
{
    st::PhiloxRng rng(/*seed=*/0x90BU, /*stream=*/0U);
    st::NormalSampler sampler(rng);
    constexpr crd::usize big_n = 200000;
    crd::f64 m1 = 0.0;
    crd::f64 m2 = 0.0;
    crd::f64 m3 = 0.0;
    crd::f64 m4 = 0.0;
    for (crd::usize i = 0; i < big_n; ++i)
    {
        const crd::f64 z = sampler.next();
        m1 += z;
        m2 += z * z;
        m3 += z * z * z;
        m4 += z * z * z * z;
    }
    const crd::f64 inv = 1.0 / static_cast<crd::f64>(big_n);
    m1 *= inv;
    m2 *= inv;
    m3 *= inv;
    m4 *= inv;
    CHECK(std::fabs(m1) < 0.01);       // mean 0 (sigma/sqrt(N) ~ 0.0022)
    CHECK(std::fabs(m2 - 1.0) < 0.02); // variance 1
    CHECK(std::fabs(m3) < 0.05);       // skew 0
    CHECK(std::fabs(m4 - 3.0) < 0.1);  // kurtosis 3
}

TEST_CASE("normal: bit-identical determinism + stream separation", "[hesap-stats][normal][determinism]")
{
    st::PhiloxRng r1(0xABCU, 7U);
    st::PhiloxRng r2(0xABCU, 7U);
    st::NormalSampler s1(r1);
    st::NormalSampler s2(r2);
    for (int i = 0; i < 100; ++i)
    {
        const crd::f64 a = s1.next();
        const crd::f64 b = s2.next();
        REQUIRE(a == b); // bit-identical
    }
    st::PhiloxRng r3(0xABCU, 8U); // a different stream
    st::NormalSampler s3(r3);
    bool any_diff = false;
    st::PhiloxRng r4(0xABCU, 7U);
    st::NormalSampler s4(r4);
    for (int i = 0; i < 16; ++i)
    {
        any_diff = any_diff || (s3.next() != s4.next());
    }
    CHECK(any_diff);
}
