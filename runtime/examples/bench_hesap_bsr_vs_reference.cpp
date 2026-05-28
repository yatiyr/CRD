// bench_hesap_bsr_vs_reference -- Phase 3.1.6 v1f-1.
//
// BSR block-spmv on BSR's NATIVE pattern (synthetic FEM-like block-structured
// matrices: N block-rows, K dense b x b blocks each). Eigen has no first-class
// BSR, so the honest gate is BSR spmv vs Eigen SCALAR CSR spmv on the SAME
// matrix (the only thing an Eigen user can do), plus vs our own scalar CSR spmv:
//
//   Gate: Cerid BSR (parallel) >= Eigen scalar-CSR  AND  >= Cerid scalar-CSR.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.
//
// crd conventions throughout: crd::containers::Array (never std::vector), a named
// GrowableTlsfAllocator (never malloc / default_allocator) — the large block_rows
// cases exceed a single 4 GB TLSF pool, so the growable arena is the right choice.
// crd::f64/usize/i32 (never raw); raw double only at the Eigen C++ API boundary.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <Eigen/Sparse>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;

// Unbounded pooled arena (chain of TLSF chunks): the 200k-block-row cases have a
// working set well past a single 4 GB TLSF pool. Reused across all run() calls.
crd::memory::GrowableTlsfAllocator g_alloc;

template <typename Fn> crd::f64 best_ms(Fn&& fn)
{
    fn();
    crd::f64 best = 1e30;
    for (crd::i32 rep = 0; rep < 7; ++rep)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(crd::u32 block_rows, crd::u32 block_cols, crd::u32 b, crd::u32 blocks_per_row)
{
    std::uint64_t s = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(block_rows) << 7) ^ b;
    auto next = [&]()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };

    crd::memory::IAllocator* alloc = &g_alloc;
    sp::TripletBuilder<crd::f64> tb(alloc, block_rows * b, block_cols * b);
    crd::containers::Array<Eigen::Triplet<double>> et(alloc);
    for (crd::u32 ib = 0; ib < block_rows; ++ib)
    {
        for (crd::u32 t = 0; t < blocks_per_row; ++t)
        {
            const crd::u32 jb = next() % block_cols;
            for (crd::u32 rr = 0; rr < b; ++rr)
            {
                for (crd::u32 cc = 0; cc < b; ++cc)
                {
                    const crd::f64 v = static_cast<crd::f64>(1 + (next() % 9)) / 4.0;
                    tb.add(ib * b + rr, jb * b + cc, v);
                    et.push_back(
                        Eigen::Triplet<double>(static_cast<int>(ib * b + rr), static_cast<int>(jb * b + cc), v));
                }
            }
        }
    }
    auto csr = tb.compress();
    auto bsr = sp::to_bsr<crd::f64>(csr, b, alloc);
    const auto n = static_cast<int>(block_cols * b);
    const auto m = static_cast<int>(block_rows * b);

    crd::containers::Array<crd::f64> x(alloc);
    crd::containers::Array<crd::f64> y(alloc);
    x.resize(static_cast<crd::usize>(n));
    y.resize(static_cast<crd::usize>(m));
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        x[i] = 0.5 + 0.01 * static_cast<crd::f64>(i % 31);
    }

    const crd::f64 t_bsr_ser =
        best_ms([&]() { sp::spmv_bsr<crd::f64>(1.0, bsr, {x.data(), x.size()}, 0.0, {y.data(), y.size()}); });
    const crd::f64 t_bsr_par = best_ms(
        [&]()
        {
            sp::spmv_bsr_parallel<crd::f64>(1.0, bsr, {x.data(), x.size()}, 0.0, {y.data(), y.size()});
            crd::jobs::frame_reset();
        });
    const crd::f64 t_csr_ser = best_ms(
        [&]() { sp::spmv<crd::f64>(1.0, csr, sp::Trans::None, {x.data(), x.size()}, 0.0, {y.data(), y.size()}); });

    Eigen::SparseMatrix<double, Eigen::RowMajor> ea(m, n);
    ea.setFromTriplets(et.data(), et.data() + et.size());
    ea.makeCompressed();
    Eigen::Map<const Eigen::VectorXd> ex(x.data(), n);
    Eigen::VectorXd ey(m);
    Eigen::setNbThreads(1);
    const crd::f64 t_eig = best_ms([&]() { ey.noalias() = ea * ex; });

    const crd::f64 vs_eig = t_eig / t_bsr_par;
    const crd::f64 vs_csr = t_csr_ser / t_bsr_par;
    std::printf("  br=%-7u b=%u k=%-2u (nnz=%8u) | BSR ser=%7.3f par=%7.3f  CSR(ours)=%7.3f  Eigen-CSR=%7.3f ms  "
                "BSR/Eigen=%.2fx %s  BSR/CSR=%.2fx %s\n",
                block_rows, b, blocks_per_row, static_cast<unsigned>(csr.nnz()), t_bsr_ser, t_bsr_par, t_csr_ser, t_eig,
                vs_eig, (vs_eig >= 1.0 ? "WIN" : "loss"), vs_csr, (vs_csr >= 1.0 ? "WIN" : "loss"));
}
} // namespace

int main()
{
    crd::jobs::init();
    std::printf("[bench_hesap_bsr] block-spmv on native block patterns: Cerid BSR vs Cerid scalar-CSR vs Eigen "
                "scalar-CSR (best-of-7). Gate: BSR par >= Eigen-CSR AND >= our CSR.\n");
    // Cache-resident and DRAM-bound sizes, b in {3,6} (FEM-common).
    run(20000, 20000, 3, 8);
    run(20000, 20000, 6, 8);
    run(200000, 200000, 3, 8);
    run(200000, 200000, 6, 8);
    crd::jobs::shutdown();
    return 0;
}
