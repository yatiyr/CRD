// v12-o resampling — deterministic core gated bit-for-bit vs scipy/statsmodels (jackknife, the bootstrap CI math,
// exact permutation). The RNG bootstrap convergence + determinism-moat checks live in the same tag.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/stats/resampling.hpp>
#include <crd/hesap/stats/resampling_parallel.hpp>

#include <crd/jobs/jobs.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::stats;
using crd::containers::ConstSpan;

namespace
{
constexpr double kA[] = {2.1, 3.4, 1.9, 5.2, 4.1, 2.8, 6.3, 3.0};
constexpr double kB[] = {1.0, 2.0, 1.5, 3.0, 2.5, 2.0, 3.5, 1.8};

[[nodiscard]] bool close(double a, double b, double tol = 1e-9)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}

[[nodiscard]] double mean_of(ConstSpan<double> x)
{
    double s = 0;
    for (double v : x)
    {
        s += v;
    }
    return s / static_cast<double>(x.size());
}
} // namespace

TEST_CASE("v12-o: jackknife / bootstrap-CI / exact permutation vs scipy", "[v12-o][stats][resampling]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 20);
    const auto a = ConstSpan<double>{kA, 8};
    const auto b = ConstSpan<double>{kB, 8};
    const auto meanstat = [](ConstSpan<double> x) { return mean_of(x); };

    {
        const auto r = jackknife(a, meanstat, &alloc);
        CHECK(close(r.estimate, 3.6));
        CHECK(close(r.se, 0.539179270479017));
        CHECK(close(r.bias, 0.0, 1e-9));
    }

    crd::containers::Array<double> dist(&alloc);
    dist.resize(50);
    for (crd::usize i = 0; i < 50; ++i)
    {
        dist[i] = 3.6 + 0.4 * crd::math::sin(0.7 * static_cast<double>(i) + 1.0);
    }
    const auto distspan = ConstSpan<double>{dist.data(), 50};
    const double theta_hat = 3.6;
    {
        const auto pc = bootstrap_ci_percentile(distspan, 0.05, &alloc);
        CHECK(close(pc.low, 3.20549032652525));
        CHECK(close(pc.high, 3.99551053323796));
        const auto bs = bootstrap_ci_basic(theta_hat, distspan, 0.05, &alloc);
        CHECK(close(bs.low, 3.20448946676204));
        CHECK(close(bs.high, 3.99450967347475));
        const auto jv = jackknife_values(a, meanstat, &alloc);
        const auto bca =
            bootstrap_ci_bca(theta_hat, distspan, ConstSpan<double>{jv.data(), jv.size()}, 0.05, &alloc);
        CHECK(close(bca.low, 3.20563140011066, 1e-7));
        CHECK(close(bca.high, 3.99564647113611, 1e-7));
    }

    {
        const auto diffstat = [](ConstSpan<double> x, ConstSpan<double> y) { return mean_of(x) - mean_of(y); };
        const auto r = permutation_test_ind(a, b, diffstat, &alloc); // exact (C(16,8)=12870)
        CHECK(close(r.statistic, 1.4375));
        CHECK(close(r.pvalue, 0.0321678321678322, 1e-9));
    }
}

TEST_CASE("v12-o: delete-d / studentized / RNG bootstrap / block / CV + determinism moat", "[v12-o][stats][resampling]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 24);
    const auto a = ConstSpan<double>{kA, 8};
    const auto meanstat = [](ConstSpan<double> x) { return mean_of(x); };

    { // delete-d jackknife (d=2); for the mean it equals the delete-1 SE
        const auto r = jackknife_delete_d(a, meanstat, 2, &alloc);
        CHECK(close(r.estimate, 3.6));
        CHECK(close(r.se, 0.539179270479018, 1e-9));
        CHECK(close(r.bias, 0.0, 1e-9));
    }

    { // studentized (bootstrap-t) CI formula on fixed boot_stat/boot_se arrays
        crd::containers::Array<double> bs(&alloc);
        crd::containers::Array<double> bse(&alloc);
        bs.resize(50);
        bse.resize(50);
        for (crd::usize i = 0; i < 50; ++i)
        {
            const auto fi = static_cast<double>(i);
            bs[i] = 3.6 + 0.4 * crd::math::sin(0.7 * fi + 1.0);
            bse[i] = 0.5 + 0.1 * crd::math::cos(0.5 * fi);
        }
        const auto ci = bootstrap_ci_studentized(3.6, 0.539179270479017, ConstSpan<double>{bs.data(), 50},
                                                 ConstSpan<double>{bse.data(), 50}, 0.05, &alloc);
        CHECK(close(ci.low, 3.13037801523382, 1e-7));
        CHECK(close(ci.high, 4.09603849136996, 1e-7));
    }

    { // RNG bootstrap converges to scipy.stats.bootstrap within Monte-Carlo tolerance (B=300000)
        const auto pc = bootstrap_ci(a, meanstat, 300000, BootMethod::Percentile, 0.05, 12345, &alloc);
        CHECK(close(pc.low, 2.675, 0.05));
        CHECK(close(pc.high, 4.6375, 0.05));
        const auto bca = bootstrap_ci(a, meanstat, 300000, BootMethod::Bca, 0.05, 12345, &alloc);
        CHECK(close(bca.low, 2.75, 0.06));
        CHECK(close(bca.high, 4.7625, 0.06));
    }

    { // determinism moat: same seed → bit-identical, and resample r is independent of B (stream = (seed, r))
        const auto d1 = bootstrap_distribution(a, meanstat, 1000, 99, &alloc);
        const auto d2 = bootstrap_distribution(a, meanstat, 1000, 99, &alloc);
        const auto d3 = bootstrap_distribution(a, meanstat, 1500, 99, &alloc);
        bool identical = true;
        bool prefix_invariant = true;
        for (crd::usize i = 0; i < 1000; ++i)
        {
            if (d1[i] != d2[i])
            {
                identical = false;
            }
            if (d1[i] != d3[i])
            {
                prefix_invariant = false;
            }
        }
        CHECK(identical);
        CHECK(prefix_invariant);
    }

    { // block bootstrap — every block-mean stays inside [min,max] of the data; correct size
        const auto bd = block_bootstrap_distribution(a, meanstat, 1000, 3, 7, &alloc);
        CHECK(bd.size() == 1000);
        bool inrange = true;
        for (double v : bd)
        {
            if (v < 1.9 || v > 6.3)
            {
                inrange = false;
            }
        }
        CHECK(inrange);
    }

    { // 5-fold CV over 20 — balanced, full coverage, reproducible; k=n → leave-one-out
        const auto f = kfold_indices(20, 5, 2024, &alloc);
        crd::usize counts[5] = {0, 0, 0, 0, 0};
        for (crd::usize i = 0; i < 20; ++i)
        {
            CHECK(f[i] < 5);
            counts[f[i]]++;
        }
        for (crd::usize c = 0; c < 5; ++c)
        {
            CHECK(counts[c] == 4);
        }
        const auto f2 = kfold_indices(20, 5, 2024, &alloc);
        bool same = true;
        for (crd::usize i = 0; i < 20; ++i)
        {
            if (f[i] != f2[i])
            {
                same = false;
            }
        }
        CHECK(same);
        const auto loo = kfold_indices(8, 8, 1, &alloc);
        crd::usize seen[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (crd::usize i = 0; i < 8; ++i)
        {
            seen[loo[i]]++;
        }
        bool loo_ok = true;
        for (crd::usize c = 0; c < 8; ++c)
        {
            if (seen[c] != 1)
            {
                loo_ok = false;
            }
        }
        CHECK(loo_ok);
    }
}

TEST_CASE("v12-o: parallel bootstrap is bit-identical to serial (moat under threading)", "[v12-o][stats][resampling]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 24);
    const auto a = ConstSpan<double>{kA, 8};
    const auto meanstat = [](ConstSpan<double> x) { return mean_of(x); };
    crd::jobs::init();
    const auto serial = bootstrap_distribution(a, meanstat, 40000, 777, &alloc);
    const auto par = bootstrap_distribution_parallel(a, meanstat, 40000, 777, &alloc);
    crd::jobs::shutdown();
    bool identical = (serial.size() == par.size());
    for (crd::usize i = 0; i < serial.size() && identical; ++i)
    {
        if (serial[i] != par[i])
        {
            identical = false;
        }
    }
    CHECK(identical);
}
