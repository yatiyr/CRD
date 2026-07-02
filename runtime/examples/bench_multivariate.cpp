// v12-k multivariate bench — Cerid logpdf/logpmf (batch) + sampling, ns/op. Peers (scipy/NumPy/MATLAB) timed
// separately; amortised normalisers + tight kernels should crush the per-call frozen objects. Plain C arrays only.

#include <crd/hesap/stats/stats.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

namespace st = crd::hesap::stats;
using CS = crd::containers::Span<const double>;
using S = crd::containers::Span<double>;

template <class F> double time_ns(int n, const F& f)
{
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i)
    {
        f(i);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
}

int main()
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1) << 30);
    constexpr int k_n = 500000;
    constexpr int k_nm = 200000; // matrix-output samplers (more work/op)
    volatile double acc = 0.0;

    // ── MVN k=3 ───────────────────────────────────────────────────────────────
    const double mean[3] = {1.0, -2.0, 0.5};
    const double cov[9] = {2.0, 0.5, 0.3, 0.5, 1.5, -0.2, 0.3, -0.2, 1.0};
    st::MultivariateNormal<double> mvn(&alloc, CS(mean, 3), CS(cov, 9));
    crd::containers::Array<double> pts(&alloc);
    pts.resize(static_cast<crd::usize>(k_n) * 3);
    st::ThreefryRng g(1U, 0U);
    for (int i = 0; i < k_n * 3; ++i)
    {
        pts[static_cast<crd::usize>(i)] = mean[i % 3] + 6.0 * (st::next_double(g) - 0.5);
    }
    for (int i = 0; i < k_n; ++i)
    {
        acc += mvn.logpdf(CS(&pts[static_cast<crd::usize>(i) * 3], 3)); // warm
    }
    const double mvn_lp = time_ns(k_n, [&](int i) { acc += mvn.logpdf(CS(&pts[static_cast<crd::usize>(i) * 3], 3)); });
    crd::containers::Array<double> out3(&alloc);
    out3.resize(static_cast<crd::usize>(k_n) * 3);
    st::Xoshiro256ss gx(2U);
    const double mvn_rv = time_ns(k_n, [&](int i) { mvn.rvs(gx, S(&out3[static_cast<crd::usize>(i) * 3], 3)); });
    st::ThreefryRng gt(2U, 0U);
    const double mvn_rv_t = time_ns(k_n, [&](int i) { mvn.rvs(gt, S(&out3[static_cast<crd::usize>(i) * 3], 3)); });

    // ── MultivariateT k=3, df=5 (scipy shape == our shape) ──────────────────────
    st::MultivariateT<double> mvt(&alloc, CS(mean, 3), CS(cov, 9), 5.0);
    const double mvt_lp = time_ns(k_n, [&](int i) { acc += mvt.logpdf(CS(&pts[static_cast<crd::usize>(i) * 3], 3)); });

    // ── Dirichlet k=4 ───────────────────────────────────────────────────────────
    const double alpha[4] = {2.0, 1.5, 3.0, 0.8};
    st::Dirichlet<double> dir(&alloc, CS(alpha, 4));
    crd::containers::Array<double> simplex(&alloc);
    simplex.resize(static_cast<crd::usize>(k_n) * 4);
    for (int i = 0; i < k_n; ++i)
    {
        dir.rvs(g, S(&simplex[static_cast<crd::usize>(i) * 4], 4)); // valid simplex points
    }
    const double dir_lp =
        time_ns(k_n, [&](int i) { acc += dir.logpdf(CS(&simplex[static_cast<crd::usize>(i) * 4], 4)); });

    // ── Multinomial k=4, n=20 ───────────────────────────────────────────────────
    const double pvec[4] = {0.4, 0.3, 0.2, 0.1};
    st::Multinomial<double> mn(&alloc, 20, CS(pvec, 4));
    crd::containers::Array<double> counts(&alloc);
    counts.resize(static_cast<crd::usize>(k_n) * 4);
    for (int i = 0; i < k_n; ++i)
    {
        mn.rvs(g, S(&counts[static_cast<crd::usize>(i) * 4], 4)); // valid count vectors summing to n
    }
    const double mn_lp = time_ns(k_n, [&](int i) { acc += mn.logpmf(CS(&counts[static_cast<crd::usize>(i) * 4], 4)); });

    // ── Wishart / InverseWishart k=3, df=8 — sampling-bound (bench rvs) ──────────
    const double scale[9] = {2.0, 0.3, 0.1, 0.3, 1.0, 0.2, 0.1, 0.2, 1.5};
    st::Wishart<double> wis(&alloc, 8.0, CS(scale, 9));
    st::InverseWishart<double> iwis(&alloc, 8.0, CS(scale, 9));
    crd::containers::Array<double> w9(&alloc);
    w9.resize(9);
    const double wis_rv = time_ns(k_nm, [&](int) { wis.rvs(gx, S(w9.data(), 9)); });
    const double iwis_rv = time_ns(k_nm, [&](int) { iwis.rvs(gx, S(w9.data(), 9)); });

    // ── LKJ k=4, eta=2 — no standard peer (Stan only) ───────────────────────────
    st::LKJ<double> lkj(2.0, 4);
    crd::containers::Array<double> r16(&alloc);
    r16.resize(16);
    const double lkj_rv = time_ns(k_nm, [&](int) { lkj.rvs(gx, S(r16.data(), 16)); });

    (void)acc;
    std::printf("# Cerid v12-k multivariate (N=%d logpdf, %d matrix-rvs)\n", k_n, k_nm);
    std::printf("MVN_logpdf_ns %.3f\n", mvn_lp);
    std::printf("MVN_rvs_ns_xoshiro %.3f\n", mvn_rv);
    std::printf("MVN_rvs_ns_threefry_moat %.3f\n", mvn_rv_t);
    std::printf("MVt_logpdf_ns %.3f\n", mvt_lp);
    std::printf("Dirichlet_logpdf_ns %.3f\n", dir_lp);
    std::printf("Multinomial_logpmf_ns %.3f\n", mn_lp);
    std::printf("Wishart_rvs_ns_xoshiro %.3f\n", wis_rv);
    std::printf("InverseWishart_rvs_ns_xoshiro %.3f\n", iwis_rv);
    std::printf("LKJ_rvs_ns_xoshiro %.3f\n", lkj_rv);
    return 0;
}
