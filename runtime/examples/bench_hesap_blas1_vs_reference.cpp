// bench_hesap_blas1_vs_reference — Reference-class shootout for BLAS L1
// (axpy / dot / nrm2). Applies the policy from
// docs/PRINCIPLES_reference_class_benchmarking.md to the L1 surface.
//
// Build only when -DCRD_BUILD_HESAP_VS_REFERENCE=ON.

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>
#include <cblas.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace
{
crd::f64 measure_clock_ghz()
{
    const auto t0 = std::chrono::steady_clock::now();
    volatile crd::u64 x = 0;
    for (crd::u64 i = 0; i < 100000000ULL; ++i)
    {
        x += i;
    }
    (void)x;
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
    return 100.0 / ms;
}

template <typename T> void fill(T* p, crd::usize n, crd::u32 seed)
{
    crd::u32 s = seed;
    for (crd::usize i = 0; i < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        p[i] = static_cast<T>(static_cast<crd::i32>(s >> 8) % 1000) * static_cast<T>(0.001);
    }
}

template <typename Op> crd::f64 time_loop_best_of_3(Op&& op, int& iters_out)
{
    // Warm-up x4.
    op();
    op();
    op();
    op();
    crd::f64 best_per_iter = 1e300;
    int best_iters = 0;
    crd::f64 best_elapsed = 0.0;
    for (int trial = 0; trial < 3; ++trial)
    {
        const auto t0 = std::chrono::steady_clock::now();
        int iters = 0;
        while (true)
        {
            op();
            ++iters;
            const auto el = std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
            if (el > 0.4 && iters >= 10)
            {
                break;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const crd::f64 elapsed = std::chrono::duration<crd::f64>(t1 - t0).count();
        const crd::f64 per_iter = elapsed / iters;
        if (per_iter < best_per_iter)
        {
            best_per_iter = per_iter;
            best_iters = iters;
            best_elapsed = elapsed;
        }
    }
    iters_out = best_iters;
    return best_elapsed;
}

// ---- axpy ------------------------------------------------------------

void bench_axpy(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== axpy.f64: y = alpha*x + y ====\n");
    std::fprintf(stdout, "%-8s | %-22s | %-22s | %-22s | %-10s | %-10s\n", "N", "Cerid (GFLOPS,iters)",
                 "Eigen (GFLOPS,iters)", "OBLAS (GFLOPS,iters)", "C/Eigen", "C/OBLAS");
    std::fprintf(stdout, "%s\n",
                 "----------------------------------------------------------------"
                 "----------------------------------------------------");
    for (crd::usize n : {crd::usize{1024}, crd::usize{4096}, crd::usize{16384}, crd::usize{65536}, crd::usize{262144}})
    {
        using crd::hesap::dense::Vector;
        Vector<crd::f64> cx(alloc, n);
        Vector<crd::f64> cy(alloc, n);
        fill(cx.data(), n, 11U);
        fill(cy.data(), n, 22U);

        const crd::f64 alpha = 1.25;
        // Cerid axpy
        int citers = 0;
        const crd::f64 ce =
            time_loop_best_of_3([&]() { crd::hesap::dense::axpy<crd::f64>(alpha, cx.span(), cy.span()); }, citers);

        // Eigen: y.noalias() += alpha * x
        Eigen::VectorXd ex(n);
        Eigen::VectorXd ey(n);
        std::memcpy(ex.data(), cx.data(), n * sizeof(crd::f64));
        std::memcpy(ey.data(), cy.data(), n * sizeof(crd::f64));
        int eiters = 0;
        const crd::f64 ee = time_loop_best_of_3([&]() { ey.noalias() += alpha * ex; }, eiters);

        // OpenBLAS daxpy
        crd::containers::Array<crd::f64> ox(alloc);
        crd::containers::Array<crd::f64> oy(alloc);
        ox.resize(n);
        oy.resize(n);
        std::memcpy(ox.data(), cx.data(), n * sizeof(crd::f64));
        std::memcpy(oy.data(), cy.data(), n * sizeof(crd::f64));
        int oiters = 0;
        const crd::f64 oe =
            time_loop_best_of_3([&]() { cblas_daxpy(static_cast<int>(n), alpha, ox.data(), 1, oy.data(), 1); }, oiters);

        const crd::f64 flop_per_iter = 2.0 * static_cast<crd::f64>(n);
        const crd::f64 cg = (flop_per_iter * citers) / (ce * 1e9);
        const crd::f64 eg = (flop_per_iter * eiters) / (ee * 1e9);
        const crd::f64 og = (flop_per_iter * oiters) / (oe * 1e9);
        std::fprintf(stdout, "%-8zu | %8.2f (%6d) | %8.2f (%6d) | %8.2f (%6d) | %8.2fx | %8.2fx\n",
                     static_cast<std::size_t>(n), cg, citers, eg, eiters, og, oiters, cg / eg, cg / og);
        std::fflush(stdout);
    }
}

// ---- dot -------------------------------------------------------------

void bench_dot(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== dot.f64: sum_i x[i]*y[i] ====\n");
    std::fprintf(stdout, "%-8s | %-22s | %-22s | %-22s | %-10s | %-10s\n", "N", "Cerid (GFLOPS,iters)",
                 "Eigen (GFLOPS,iters)", "OBLAS (GFLOPS,iters)", "C/Eigen", "C/OBLAS");
    std::fprintf(stdout, "%s\n",
                 "----------------------------------------------------------------"
                 "----------------------------------------------------");
    for (crd::usize n : {crd::usize{1024}, crd::usize{4096}, crd::usize{16384}, crd::usize{65536}, crd::usize{262144}})
    {
        using crd::hesap::dense::Vector;
        Vector<crd::f64> cx(alloc, n);
        Vector<crd::f64> cy(alloc, n);
        fill(cx.data(), n, 11U);
        fill(cy.data(), n, 22U);
        volatile crd::f64 sink = 0;

        int citers = 0;
        const crd::f64 ce =
            time_loop_best_of_3([&]() { sink = crd::hesap::dense::dot<crd::f64>(cx.span(), cy.span()); }, citers);

        Eigen::VectorXd ex(n);
        Eigen::VectorXd ey(n);
        std::memcpy(ex.data(), cx.data(), n * sizeof(crd::f64));
        std::memcpy(ey.data(), cy.data(), n * sizeof(crd::f64));
        int eiters = 0;
        const crd::f64 ee = time_loop_best_of_3([&]() { sink = ex.dot(ey); }, eiters);

        crd::containers::Array<crd::f64> ox(alloc);
        crd::containers::Array<crd::f64> oy(alloc);
        ox.resize(n);
        oy.resize(n);
        std::memcpy(ox.data(), cx.data(), n * sizeof(crd::f64));
        std::memcpy(oy.data(), cy.data(), n * sizeof(crd::f64));
        int oiters = 0;
        const crd::f64 oe =
            time_loop_best_of_3([&]() { sink = cblas_ddot(static_cast<int>(n), ox.data(), 1, oy.data(), 1); }, oiters);
        (void)sink;

        const crd::f64 flop_per_iter = 2.0 * static_cast<crd::f64>(n);
        const crd::f64 cg = (flop_per_iter * citers) / (ce * 1e9);
        const crd::f64 eg = (flop_per_iter * eiters) / (ee * 1e9);
        const crd::f64 og = (flop_per_iter * oiters) / (oe * 1e9);
        std::fprintf(stdout, "%-8zu | %8.2f (%6d) | %8.2f (%6d) | %8.2f (%6d) | %8.2fx | %8.2fx\n",
                     static_cast<std::size_t>(n), cg, citers, eg, eiters, og, oiters, cg / eg, cg / og);
        std::fflush(stdout);
    }
}

// ---- nrm2 ------------------------------------------------------------

void bench_nrm2(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== nrm2.f64: sqrt(sum_i x[i]^2) ====\n");
    std::fprintf(stdout, "%-8s | %-22s | %-22s | %-22s | %-10s | %-10s\n", "N", "Cerid (GFLOPS,iters)",
                 "Eigen (GFLOPS,iters)", "OBLAS (GFLOPS,iters)", "C/Eigen", "C/OBLAS");
    std::fprintf(stdout, "%s\n",
                 "----------------------------------------------------------------"
                 "----------------------------------------------------");
    for (crd::usize n : {crd::usize{1024}, crd::usize{4096}, crd::usize{16384}, crd::usize{65536}, crd::usize{262144}})
    {
        using crd::hesap::dense::Vector;
        Vector<crd::f64> cx(alloc, n);
        fill(cx.data(), n, 11U);
        volatile crd::f64 sink = 0;

        int citers = 0;
        const crd::f64 ce = time_loop_best_of_3([&]() { sink = crd::hesap::dense::nrm2<crd::f64>(cx.span()); }, citers);

        Eigen::VectorXd ex(n);
        std::memcpy(ex.data(), cx.data(), n * sizeof(crd::f64));
        int eiters = 0;
        const crd::f64 ee = time_loop_best_of_3([&]() { sink = ex.norm(); }, eiters);

        crd::containers::Array<crd::f64> ox(alloc);
        ox.resize(n);
        std::memcpy(ox.data(), cx.data(), n * sizeof(crd::f64));
        int oiters = 0;
        const crd::f64 oe =
            time_loop_best_of_3([&]() { sink = cblas_dnrm2(static_cast<int>(n), ox.data(), 1); }, oiters);
        (void)sink;

        // nrm2 does ~2 flops per element (mul + add); sqrt is a single op.
        const crd::f64 flop_per_iter = 2.0 * static_cast<crd::f64>(n);
        const crd::f64 cg = (flop_per_iter * citers) / (ce * 1e9);
        const crd::f64 eg = (flop_per_iter * eiters) / (ee * 1e9);
        const crd::f64 og = (flop_per_iter * oiters) / (oe * 1e9);
        std::fprintf(stdout, "%-8zu | %8.2f (%6d) | %8.2f (%6d) | %8.2f (%6d) | %8.2fx | %8.2fx\n",
                     static_cast<std::size_t>(n), cg, citers, eg, eiters, og, oiters, cg / eg, cg / og);
        std::fflush(stdout);
    }
}
} // namespace

int main(int argc, char** argv)
{
    bool p_cores_only = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--all-cores") == 0)
        {
            p_cores_only = false;
        }
    }
#ifdef _WIN32
    if (p_cores_only)
    {
        SetProcessAffinityMask(GetCurrentProcess(), 0xFFFFULL);
    }
#endif
    // Eigen + OpenBLAS run single-threaded for L1 (vector ops scale only at
    // very large N where memory bandwidth dominates). Use 1 thread for fair
    // comparison; Cerid L1 is also serial today.
    Eigen::setNbThreads(1);
    openblas_set_num_threads(1);

    const crd::f64 ghz = measure_clock_ghz();
    std::fprintf(stdout,
                 "==== bench_hesap_blas1_vs_reference ====\n"
                 "  SIMD backend : %s\n"
                 "  Clock        : %.2f GHz (P-cores)\n"
                 "  Threads      : 1 each (BLAS L1 is memory-bandwidth-bound)\n"
                 "  Goal         : Cerid >= Eigen-MT AND Cerid >= OpenBLAS\n",
                 crd::math::simd::backend_name(), ghz);

    crd::memory::TlsfAllocator alloc(64ULL * 1024ULL * 1024ULL);
    bench_axpy(&alloc);
    bench_dot(&alloc);
    bench_nrm2(&alloc);

    std::fprintf(stdout, "\nDone.\n");
    return 0;
}
