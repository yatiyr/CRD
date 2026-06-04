// bench_hesap_hss_vs_strumpack -- Phase 3.1.6 v5e-1d gold-standard shootout.
//
// Cerid HSS ULV (build_hss_from_dense -> factor_hss_ulv -> solve) vs STRUMPACK's
// standalone dense HSS + ULV (strumpack::HSS::HSSMatrix<double>), on the SAME
// SPD low-off-diagonal-rank matrix, same rel-tol + leaf size. The gold-standard
// peer for the rank-structured family (Ghysels-Li, STRUMPACK).
//
// HONEST: both compress dense -> HSS -> ULV factor -> solve. We time
// construct(compress) / factor / solve separately, report ratios + residual +
// the compressed rank. STRUMPACK's dense HSS uses RANDOMIZED sampling by default
// (a fair, different compression path); we report each side's rank so the
// comparison is apples-to-apples on accuracy.
//
// WSL/Linux only; built only when CRD_BUILD_HESAP_VS_STRUMPACK=ON. Dev-only,
// NEVER shipped, NEVER in CI release. Raw double only at the STRUMPACK boundary.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/hss.hpp>
#include <crd/hesap/direct/hss_ulv.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <HSS/HSSMatrix.hpp>
#include <dense/DenseMatrix.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace dir = crd::hesap::direct;
crd::memory::GrowableTlsfAllocator g_alloc;
using Mat = crd::hesap::dense::Matrix<crd::f64>;

Mat make_spd_lowrank(crd::usize n)
{
    constexpr crd::usize terms = 4;
    const double decay = 0.05;
    const double freq[terms] = {0.021, 0.037, 0.053, 0.071};
    Mat a(&g_alloc, n, n);
    a.set_zero();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = i; j < n; ++j)
        {
            double g = 0.0;
            double wt = 1.0;
            for (crd::usize t = 0; t < terms; ++t)
            {
                const double wi = std::sin(static_cast<double>(i) * freq[t] + 0.3 * static_cast<double>(t) + 0.1);
                const double wj = std::sin(static_cast<double>(j) * freq[t] + 0.3 * static_cast<double>(t) + 0.1);
                g += wt * wi * wj;
                wt *= decay;
            }
            a.at(i, j) = g;
            a.at(j, i) = g;
        }
        a.at(i, i) += 2.0 + static_cast<double>(i) * 0.1;
    }
    return a;
}

double seconds(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

double resid(const Mat& a, const crd::f64* x, const crd::f64* b, crd::usize n)
{
    double rn = 0.0;
    double bn = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a.at(i, j) * x[j];
        }
        rn += (s - b[i]) * (s - b[i]);
        bn += b[i] * b[i];
    }
    return std::sqrt(rn) / std::sqrt(bn);
}

void run(crd::usize n, crd::usize leaf, double tol, crd::usize nrhs)
{
    const Mat a = make_spd_lowrank(n);

    crd::containers::Array<crd::f64> rhs(&g_alloc);  // column-major n × nrhs
    rhs.resize(n * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            rhs[c * n + i] = std::cos(static_cast<double>(i * 3 + c) * 0.31 + 0.2);
        }
    }

    // ---- Cerid HSS ULV ----
    auto c0 = Clock::now();
    dir::HssMatrix<crd::f64> h = dir::build_hss_from_dense<crd::f64>(&g_alloc, a, leaf, tol);
    auto c1 = Clock::now();
    auto f = dir::factor_hss_ulv<crd::f64>(&g_alloc, h);
    auto c2 = Clock::now();
    crd::containers::Array<crd::f64> xc(&g_alloc);
    xc.resize(n * nrhs);
    // Solve is a few-ms op -> single-shot timing is pure jitter. Take min over reps
    // (re-init RHS each rep); the min is the standard low-noise microbench estimator.
    constexpr int kSolveReps = 50;
    double cs = 1e30;
    for (int rep = 0; rep < kSolveReps; ++rep)
    {
        for (crd::usize i = 0; i < n * nrhs; ++i)
        {
            xc[i] = rhs[i];
        }
        auto t0 = Clock::now();
        f.solve(crd::containers::Span<crd::f64>{xc.data(), n * nrhs}, nrhs);
        auto t1 = Clock::now();
        const double dt = seconds(t0, t1);
        if (dt < cs)
        {
            cs = dt;
        }
    }
    crd::usize cmaxr = 0;
    for (crd::usize id = 0; id < h.num_nodes(); ++id)
    {
        if (h.nodes[id].rank > cmaxr)
        {
            cmaxr = h.nodes[id].rank;
        }
    }
    const double cerid_resid = resid(a, xc.data(), rhs.data(), n);

    // ---- STRUMPACK dense HSS + ULV ----
    strumpack::DenseMatrix<double> sa(n, n, a.data(), n);  // symmetric ⇒ col-major == row-major
    strumpack::HSS::HSSOptions<double> opts;
    opts.set_rel_tol(tol);
    opts.set_leaf_size(static_cast<int>(leaf));
    auto s0 = Clock::now();
    strumpack::HSS::HSSMatrix<double> sh(sa, opts);
    auto s1 = Clock::now();
    sh.factor();
    auto s2 = Clock::now();
    strumpack::DenseMatrix<double> sb(n, nrhs, rhs.data(), n);
    double ss = 1e30;
    for (int rep = 0; rep < kSolveReps; ++rep)
    {
        for (crd::usize i = 0; i < n * nrhs; ++i)
        {
            sb.data()[i] = rhs[i];  // STRUMPACK solve is in-place -> re-seed each rep
        }
        auto t0 = Clock::now();
        sh.solve(sb);
        auto t1 = Clock::now();
        const double dt = seconds(t0, t1);
        if (dt < ss)
        {
            ss = dt;
        }
    }
    const double strum_resid = resid(a, sb.data(), rhs.data(), n);
    const std::size_t smaxr = sh.rank();

    const double cb = seconds(c0, c1), cf = seconds(c1, c2);
    const double s_compress = seconds(s0, s1), s_factor = seconds(s1, s2);

    std::printf("N=%-5zu leaf=%zu  ranks: cerid=%zu strumpack=%zu   resid: cerid=%.2e strumpack=%.2e\n", n, leaf,
                cmaxr, smaxr, cerid_resid, strum_resid);
    std::printf("  compress: cerid %.4fs   strumpack %.4fs   -> %.2fx\n", cb, s_compress,
                s_compress / (cb + 1e-12));
    std::printf("  factor  : cerid %.5fs   strumpack %.5fs   -> %.2fx\n", cf, s_factor, s_factor / (cf + 1e-12));
    std::printf("  solve   : cerid %.5fs   strumpack %.5fs   -> %.2fx  (nrhs=%zu)\n", cs, ss,
                ss / (cs + 1e-12), nrhs);
}
} // namespace

int main()
{
    std::printf("=== Cerid HSS ULV vs STRUMPACK dense HSS+ULV (SPD, low off-diagonal rank) ===\n\n");
    const double tol = 1e-8;
    const crd::usize nrhs = 64;
    for (const crd::usize n : {crd::usize{512}, crd::usize{1024}, crd::usize{2048}, crd::usize{4096}})
    {
        run(n, 128, tol, nrhs);
        std::printf("\n");
    }
    return 0;
}
