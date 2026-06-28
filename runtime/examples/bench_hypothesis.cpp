// v12-n parametric hypothesis-test crush bench — ns/call vs scipy.stats (see bench_hypothesis_refs.py).
// n=100 per group; A[0] is perturbed each call so the optimizer can't hoist the test out of the loop.

#include <crd/hesap/stats/hypothesis.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using namespace crd::hesap::stats;
using crd::containers::ConstSpan;
using crd::f64;
using crd::usize;
using clk = std::chrono::steady_clock;

int main()
{
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 24);
    const usize n = 100;
    crd::containers::Array<f64> a(&alloc);
    crd::containers::Array<f64> b(&alloc);
    crd::containers::Array<f64> c(&alloc);
    a.resize(n);
    b.resize(n);
    c.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        const f64 fi = static_cast<f64>(i);
        a[i] = 2.0 + 0.7 * crd::math::sin(0.30 * fi);
        b[i] = 1.5 + 0.5 * crd::math::sin(0.21 * fi + 1.0);
        c[i] = 3.0 + 0.9 * crd::math::sin(0.13 * fi + 2.0);
    }
    const ConstSpan<f64> grp[] = {{a.data(), n}, {b.data(), n}, {c.data(), n}};
    const ConstSpan<ConstSpan<f64>> groups{grp, 3};

    const int N = 200000;
    f64 acc = 0;
    auto bench = [&](const char* name, auto fn) {
        for (int i = 0; i < 1000; ++i)
        {
            acc += fn(i);
        }
        const auto t0 = clk::now();
        for (int i = 0; i < N; ++i)
        {
            acc += fn(i);
        }
        const auto t1 = clk::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
        std::printf("%-14s %8.1f ns/call\n", name, ns);
    };

    bench("ttest_ind", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = t_test_ind(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n}, true);
        return r.statistic + r.pvalue;
    });
    bench("ttest_welch", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = t_test_ind(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n}, false);
        return r.statistic + r.pvalue;
    });
    bench("ttest_rel", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = t_test_rel(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n});
        return r.statistic + r.pvalue;
    });
    bench("f_oneway", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = f_oneway(groups);
        return r.statistic + r.pvalue;
    });
    bench("bartlett", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = bartlett(groups);
        return r.statistic + r.pvalue;
    });
    bench("levene", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = levene(groups, &alloc, Center::Median);
        return r.statistic + r.pvalue;
    });
    bench("mannwhitneyu", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = mann_whitney_u(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("wilcoxon", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = wilcoxon(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("kruskal", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = kruskal(groups, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("friedman", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = friedman(groups, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("pearsonr", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = pearsonr(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n});
        return r.statistic + r.pvalue;
    });
    bench("spearmanr", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = spearmanr(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("kendalltau", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = kendalltau(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("jarque_bera", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = jarque_bera(ConstSpan<f64>{a.data(), n});
        return r.statistic + r.pvalue;
    });
    bench("ks_2samp", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = ks_2samp(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("shapiro", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = shapiro(ConstSpan<f64>{a.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("anderson", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = anderson_darling(ConstSpan<f64>{a.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("ztest", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = z_test_1samp(ConstSpan<f64>{a.data(), n}, 2.0);
        return r.statistic + r.pvalue;
    });
    bench("mood", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = mood_scale(ConstSpan<f64>{a.data(), n}, ConstSpan<f64>{b.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("dagostino", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = dagostino_k2(ConstSpan<f64>{a.data(), n});
        return r.statistic + r.pvalue;
    });
    bench("cramervonmises", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto ncdf = [](f64 v) { return 0.5 * crd::hesap::special::erfc(-v / 1.4142135623730951); };
        const auto r = cramervonmises(ConstSpan<f64>{a.data(), n}, ncdf, &alloc);
        return r.statistic + r.pvalue;
    });
    bench("lilliefors", [&](int i) {
        a[0] = 2.0 + static_cast<f64>(i & 31) * 0.01;
        const auto r = lilliefors(ConstSpan<f64>{a.data(), n}, &alloc);
        return r.statistic + r.pvalue;
    });

    std::printf("# acc=%g (n=%zu/group, %d calls each)\n", acc, n, N);
    return 0;
}
