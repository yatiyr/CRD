// bench_hesap_hss_vs_reference -- Phase 3.1.6 v5e-1d.
//
// HSS ULV (build_hss_from_dense -> factor_hss_ulv -> solve) vs Eigen dense
// LLT, on an SPD matrix with LOW off-diagonal rank (the regime HSS exists for).
//
// HONESTY (the build phase is itself O(N^3)): we time build / factor / solve
// SEPARATELY. The HSS win is the factor (O(r^2 N)) + the per-solve (O(rN)) under
// factor-once/solve-many; build is O(N^3) and is reported but EXCLUDED from the
// crush claim. vs-Eigen-dense is the PREMISE (an O(r^2 N) solver beating O(N^3)
// is definitional), not a gold-standard crush — that is STRUMPACK (separate).
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. Raw double only at the Eigen
// API boundary.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/hss.hpp>
#include <crd/hesap/direct/hss_ulv.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <Eigen/Cholesky>
#include <Eigen/Dense>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace dir = crd::hesap::direct;
crd::memory::GrowableTlsfAllocator g_alloc;

using Mat = crd::hesap::dense::Matrix<crd::f64>;

// SPD, low off-diagonal rank: diag(2 + i/10) + Σ_{t=0}^{3} 0.05^t w_t w_tᵀ (PSD).
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

void run(crd::usize n, crd::usize leaf, double tol, crd::usize nrhs)
{
    const Mat a = make_spd_lowrank(n);

    // ---- Cerid HSS ULV ----
    auto t0 = Clock::now();
    dir::HssMatrix<crd::f64> h = dir::build_hss_from_dense<crd::f64>(&g_alloc, a, leaf, tol);
    auto t1 = Clock::now();
    auto f = dir::factor_hss_ulv<crd::f64>(&g_alloc, h);
    auto t2 = Clock::now();

    crd::usize maxr = 0;
    for (crd::usize id = 0; id < h.num_nodes(); ++id)
    {
        if (h.nodes[id].is_leaf && h.nodes[id].rank > maxr)
        {
            maxr = h.nodes[id].rank;
        }
    }

    // RHS block (n × nrhs), column-major; same data for Eigen.
    crd::containers::Array<crd::f64> rhs(&g_alloc);
    rhs.resize(n * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            rhs[c * n + i] = std::cos(static_cast<double>(i * 3 + c) * 0.31 + 0.2);
        }
    }
    crd::containers::Array<crd::f64> xc(&g_alloc);
    xc.resize(n * nrhs);
    for (crd::usize i = 0; i < n * nrhs; ++i)
    {
        xc[i] = rhs[i];
    }
    auto t3 = Clock::now();
    const bool ok = f.solve(crd::containers::Span<crd::f64>{xc.data(), n * nrhs}, nrhs);
    auto t4 = Clock::now();

    // residual ‖A·x0 − b0‖/‖b0‖ on the first RHS.
    double rn = 0.0;
    double bn = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a.at(i, j) * xc[j];
        }
        rn += (s - rhs[i]) * (s - rhs[i]);
        bn += rhs[i] * rhs[i];
    }
    const double cerid_resid = std::sqrt(rn) / std::sqrt(bn);

    // ---- Eigen dense LLT ----
    Eigen::MatrixXd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = a.at(i, j);
        }
    }
    Eigen::MatrixXd eb(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(nrhs));
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            eb(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(c)) = rhs[c * n + i];
        }
    }
    auto e0 = Clock::now();
    Eigen::LLT<Eigen::MatrixXd> llt(ea);
    auto e1 = Clock::now();
    Eigen::MatrixXd ex = llt.solve(eb);
    auto e2 = Clock::now();

    const double cb = seconds(t0, t1);
    const double cf = seconds(t1, t2);
    const double cs = seconds(t3, t4);
    const double ef = seconds(e0, e1);
    const double es = seconds(e1, e2);

    std::printf("N=%-5zu leaf=%-3zu maxLeafRank=%-3zu  ok=%d resid=%.2e\n", n, leaf, maxr, ok ? 1 : 0,
                cerid_resid);
    std::printf("  build  : cerid %.4fs (O(N^3), EXCLUDED from crush)\n", cb);
    std::printf("  factor : cerid %.5fs   eigenLLT %.5fs   -> %.1fx\n", cf, ef, ef / (cf + 1e-12));
    std::printf("  solve  : cerid %.5fs   eigen    %.5fs   -> %.1fx  (nrhs=%zu, %.2e s/rhs cerid)\n", cs, es,
                es / (cs + 1e-12), nrhs, cs / static_cast<double>(nrhs));
}
} // namespace

int main()
{
    std::printf("=== HSS ULV vs Eigen dense LLT (SPD, low off-diagonal rank) ===\n");
    std::printf("(build is O(N^3) and EXCLUDED; the crush is factor O(r^2 N) + per-solve O(rN))\n\n");
    const double tol = 1e-8;
    const crd::usize nrhs = 64;
    for (const crd::usize n : {crd::usize{256}, crd::usize{512}, crd::usize{1024}, crd::usize{2048}})
    {
        run(n, 64, tol, nrhs);
        std::printf("\n");
    }
    return 0;
}
