// bench_hesap_spgemm_stress_vs_reference -- Phase 3.1.6 v1g-2.
//
// Adversarial spgemm corpus vs Eigen-ST (Eigen sparse*sparse is single-threaded)
// to find any regime where the dense-SPA Gustavson LOSES or is infeasible:
//   (a) near the kMaxSpaCols=4M boundary (large but in-cap dimension),
//   (b) power-law row lengths (a few very long rows),
//   (c) huge dimension where the SPA exceeds L2+L3,
//   (d) extra-sparse (huge dim, ~2 nnz/row).
// Pinned trigger: implement a hash/merge accumulator iff Eigen >= 1.00x on ANY
// in-cap case. (cols > 4M is a separate CAPABILITY ceiling -- the dense SPA
// asserts; bench stays <= cap and the ceiling is reported in docs.)
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.
//
// crd conventions throughout: crd::containers::Array (never std::vector), a named
// GrowableTlsfAllocator (never malloc / default_allocator); crd::f64/u32/u64
// (never raw). Raw double only at the Eigen C++ API boundary.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <Eigen/Sparse>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;

// Unbounded pooled arena (dims up to 3.5M ⇒ working set ≫ one 4 GB TLSF pool).
crd::memory::GrowableTlsfAllocator g_alloc;

template <typename Fn> crd::f64 best_ms(Fn&& fn, crd::i32 reps = 3)
{
    fn();
    crd::f64 best = 1e30;
    for (crd::i32 r = 0; r < reps; ++r)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

// Build A (n x n) with the given per-row length generator g(i)->len, columns
// uniform-random. Returns both Cerid CSR and Eigen RowMajor.
template <typename Gen> void run(const char* name, crd::u32 n, Gen&& g)
{
    std::uint64_t s = 0xD1CE05 ^ (static_cast<std::uint64_t>(n) << 1);
    auto next = [&]()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    crd::memory::IAllocator* alloc = &g_alloc;
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    crd::containers::Array<Eigen::Triplet<double>> et(alloc);
    crd::u64 nnz = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 len = g(i, next);
        for (crd::u32 t = 0; t < len; ++t)
        {
            const crd::u32 j = next() % n;
            const crd::f64 v = 0.5 + 0.001 * static_cast<crd::f64>(next() % 1000);
            tb.add(i, j, v);
            et.push_back(Eigen::Triplet<double>(static_cast<int>(i), static_cast<int>(j), v));
            ++nnz;
        }
    }
    auto a = tb.compress();

    const crd::f64 t_cerid = best_ms(
        [&]()
        {
            auto c = sp::spgemm_parallel<crd::f64>(a, a, alloc);
            crd::jobs::frame_reset();
            (void)c;
        });

    Eigen::SparseMatrix<double, Eigen::RowMajor> ea(static_cast<int>(n), static_cast<int>(n));
    ea.setFromTriplets(et.data(), et.data() + et.size());
    ea.makeCompressed();
    Eigen::setNbThreads(1);
    Eigen::SparseMatrix<double, Eigen::RowMajor> ec;
    const crd::f64 t_eig = best_ms([&]() { ec = ea * ea; }); // NOT .pruned() (v1d: prune unfairly handicaps Eigen)

    const crd::f64 ratio = t_eig / t_cerid;
    std::printf("  %-22s n=%-8u nnz(A)=%9llu | Cerid=%9.2f  Eigen-ST=%9.2f ms  Eigen/Cerid=%.2fx %s\n", name, n,
                static_cast<unsigned long long>(nnz), t_cerid, t_eig, ratio, (ratio >= 1.0 ? "WIN" : "LOSS<-trigger"));
}
} // namespace

int main()
{
    crd::jobs::init();
    std::printf("[bench_hesap_spgemm_stress] adversarial C=A*A vs Eigen-ST (best-of-3). "
                "Trigger: implement hash accumulator iff Eigen>=1.00x on any in-cap case.\n");

    // (a) near-boundary dimension (in-cap), modest nnz/row.
    run("boundary-3.5M", 3500000U, [](crd::u32, auto&) { return 3U; });
    // (b) power-law: most rows ~4, every 1000th row very long (1000).
    run("power-law", 400000U, [](crd::u32 i, auto&) { return (i % 1000U == 0U) ? 1000U : 4U; });
    // (c) huge-dim, SPA (n*12B) ~ 24MB > L2/L3, moderate nnz/row.
    run("huge-dim-cache-2M", 2000000U, [](crd::u32, auto&) { return 8U; });
    // (d) extra-sparse: huge dim, ~2 nnz/row.
    run("extra-sparse-3M", 3000000U, [](crd::u32, auto&) { return 2U; });

    crd::jobs::shutdown();
    return 0;
}
