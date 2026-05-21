// bench_hesap_ell_dia_vs_reference -- Phase 3.1.6 v1f-2.
//
// ELL on regular (uniform-row) patterns and DIA on banded patterns -- each on
// its NATIVE structure -- vs Cerid scalar CSR and Eigen scalar CSR spmv.
//   ELL contract: interop/base regular format (SELL is the irregular perf path).
//   Gate: each format's parallel spmv >= our scalar CSR AND >= Eigen-CSR on its
//   native pattern.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.

#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Sparse>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;

template <typename Fn>
double best_ms(Fn&& fn)
{
    fn();
    double best = 1e30;
    for (int rep = 0; rep < 7; ++rep)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

void run_ell(crd::u32 n, crd::u32 per_row)
{
    std::uint64_t s    = 0xABCDEF ^ (static_cast<std::uint64_t>(n) << 3) ^ per_row;
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    crd::memory::IAllocator*            alloc = crd::memory::default_allocator();
    sp::TripletBuilder<crd::f64>        tb(alloc, n, n);
    std::vector<Eigen::Triplet<double>> et;
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 t = 0; t < per_row; ++t)
        {
            const crd::u32 j = next() % n;
            const double   v = static_cast<double>(1 + (next() % 9)) / 5.0;
            tb.add(i, j, v);
            et.emplace_back(static_cast<int>(i), static_cast<int>(j), v);
        }
    }
    auto                csr = tb.compress();
    auto                ell = sp::to_ell<crd::f64>(csr, alloc);
    std::vector<double> x(n), y(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = 0.5 + 0.01 * static_cast<double>(i % 31);
    }
    const double t_ell = best_ms([&]() {
        sp::spmv_ell_parallel<crd::f64>(1.0, ell, {x.data(), x.size()}, 0.0, {y.data(), y.size()});
        crd::jobs::frame_reset();
    });
    const double t_csr = best_ms([&]() {
        sp::spmv<crd::f64>(1.0, csr, sp::Trans::None, {x.data(), x.size()}, 0.0, {y.data(), y.size()});
    });
    Eigen::SparseMatrix<double, Eigen::RowMajor> ea(static_cast<int>(n), static_cast<int>(n));
    ea.setFromTriplets(et.begin(), et.end());
    ea.makeCompressed();
    Eigen::Map<const Eigen::VectorXd> ex(x.data(), n);
    Eigen::VectorXd                   ey(n);
    Eigen::setNbThreads(1);
    const double t_eig = best_ms([&]() { ey.noalias() = ea * ex; });
    std::printf("  ELL n=%-7u k=%-3u | par=%7.3f  CSR(ours)=%7.3f  Eigen-CSR=%7.3f ms  ELL/Eigen=%.2fx %s  "
                "ELL/CSR=%.2fx %s\n",
                n, per_row, t_ell, t_csr, t_eig, t_eig / t_ell, (t_eig >= t_ell ? "WIN" : "loss"), t_csr / t_ell,
                (t_csr >= t_ell ? "WIN" : "loss"));
}

void run_dia(crd::u32 n)
{
    std::uint64_t s    = 0x123456 ^ (static_cast<std::uint64_t>(n) << 5);
    auto          next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    const crd::i32                      offs[] = {-32, -8, -1, 0, 1, 8, 32};  // 7-pt stencil-ish band
    crd::memory::IAllocator*            alloc  = crd::memory::default_allocator();
    sp::TripletBuilder<crd::f64>        tb(alloc, n, n);
    std::vector<Eigen::Triplet<double>> et;
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::i32 k : offs)
        {
            const crd::i32 j = static_cast<crd::i32>(i) + k;
            if (j >= 0 && j < static_cast<crd::i32>(n))
            {
                const double v = static_cast<double>(1 + (next() % 9)) / 6.0;
                tb.add(i, static_cast<crd::u32>(j), v);
                et.emplace_back(static_cast<int>(i), j, v);
            }
        }
    }
    auto                csr = tb.compress();
    auto                dia = sp::to_dia<crd::f64>(csr, alloc);
    std::vector<double> x(n), y(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = 0.5 + 0.01 * static_cast<double>(i % 31);
    }
    const double t_dia = best_ms([&]() {
        sp::spmv_dia_parallel<crd::f64>(1.0, dia, {x.data(), x.size()}, 0.0, {y.data(), y.size()});
        crd::jobs::frame_reset();
    });
    const double t_csr = best_ms([&]() {
        sp::spmv<crd::f64>(1.0, csr, sp::Trans::None, {x.data(), x.size()}, 0.0, {y.data(), y.size()});
    });
    Eigen::SparseMatrix<double, Eigen::RowMajor> ea(static_cast<int>(n), static_cast<int>(n));
    ea.setFromTriplets(et.begin(), et.end());
    ea.makeCompressed();
    Eigen::Map<const Eigen::VectorXd> ex(x.data(), n);
    Eigen::VectorXd                   ey(n);
    Eigen::setNbThreads(1);
    const double t_eig = best_ms([&]() { ey.noalias() = ea * ex; });
    std::printf("  DIA n=%-7u nd=7  | par=%7.3f  CSR(ours)=%7.3f  Eigen-CSR=%7.3f ms  DIA/Eigen=%.2fx %s  "
                "DIA/CSR=%.2fx %s\n",
                n, t_dia, t_csr, t_eig, t_eig / t_dia, (t_eig >= t_dia ? "WIN" : "loss"), t_csr / t_dia,
                (t_csr >= t_dia ? "WIN" : "loss"));
}
} // namespace

int main()
{
    crd::jobs::init();
    std::printf("[bench_hesap_ell_dia] ELL (regular) / DIA (banded) spmv vs Cerid+Eigen scalar CSR (best-of-7).\n");
    run_ell(100000, 16);
    run_ell(1000000, 16);
    run_dia(100000);
    run_dia(1000000);
    crd::jobs::shutdown();
    return 0;
}
