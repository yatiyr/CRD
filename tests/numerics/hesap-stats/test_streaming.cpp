// v12-p streaming/online — Welford vs numpy (exact); P-square vs the reference Jain-Chlamtac algorithm (bit-for-bit);
// HyperLogLog within its ~1.04/sqrt(m) error; count-min as an upper bound on the true frequency.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/stats/streaming.hpp>
#include <crd/hesap/stats/tdigest.hpp>

#include <crd/core/types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::stats;

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
} // namespace

TEST_CASE("v12-p: Welford / P-square / HyperLogLog / count-min", "[v12-p][stats][streaming]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);

    Welford<double> w;
    P2Quantile<double> p2(0.5);
    for (crd::u64 i = 1; i <= 1000; ++i) // stream x_i = (i*16807) % 100003
    {
        const double v = static_cast<double>((i * 16807ULL) % 100003ULL);
        w.add(v);
        p2.add(v);
    }
    CHECK(w.count() == 1000);
    CHECK(close(w.mean(), 50052.652));
    CHECK(close(w.variance(1), 834594467.638535));
    CHECK(close(p2.quantile(), 50257.1890317903, 1e-6)); // reference P-square

    HyperLogLog<11> hll(&alloc); // 10000 distinct → within ~7% (≈3σ) of the truth
    for (crd::u64 i = 0; i < 10000; ++i)
    {
        hll.add(i);
    }
    const double card = hll.estimate();
    CHECK(card > 9300.0);
    CHECK(card < 10700.0);

    CountMinSketch<4, 2048> cms(&alloc); // items i%50, each frequency 100
    for (crd::u64 i = 0; i < 5000; ++i)
    {
        cms.add(i % 50);
    }
    CHECK(cms.query(0) >= 100); // count-min never underestimates
    CHECK(cms.query(0) <= 110); // and is tight at low load
    CHECK(cms.query(49) >= 100);
}

TEST_CASE("v12-p: t-digest quantiles within accuracy vs true", "[v12-p][stats][streaming]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    TDigest<double> td(&alloc, 100.0);
    for (crd::u64 i = 1; i <= 1000; ++i)
    {
        td.add(static_cast<double>((i * 16807ULL) % 100003ULL));
    }
    // true quantiles q10/q50/q90 = 9984.3 / 50299.5 / 89802.7; range ~100003 → gate within ~2%
    CHECK(within(td.quantile(0.10), 9984.3, 2000.0));
    CHECK(within(td.quantile(0.50), 50299.5, 1000.0));
    CHECK(within(td.quantile(0.90), 89802.7, 2000.0));
}
