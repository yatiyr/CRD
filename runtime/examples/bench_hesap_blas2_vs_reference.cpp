// bench_hesap_blas2_vs_reference — Reference-class shootout for BLAS L2
// (gemv / symv / trsv). Applies the policy from
// docs/PRINCIPLES_reference_class_benchmarking.md to the L2 surface.

#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>
#include <cblas.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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
    for (crd::u64 i = 0; i < 100000000ULL; ++i) { x += i; }
    (void)x;
    const auto t1 = std::chrono::steady_clock::now();
    return 100.0 / std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
}

template <typename T>
void fill(T* p, crd::usize n, crd::u32 seed)
{
    crd::u32 s = seed;
    for (crd::usize i = 0; i < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        p[i] = static_cast<T>(static_cast<crd::i32>(s >> 8) % 1000) * static_cast<T>(0.001);
    }
}

template <typename Op>
crd::f64 time_loop_best_of_3(Op&& op, int& iters_out)
{
    op(); op(); op(); op();
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
            if (el > 0.4 && iters >= 5)
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

void bench_gemv(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== gemv.f64: y = alpha*A*x + beta*y ====\n");
    std::fprintf(stdout, "%-6s | %-22s | %-22s | %-22s | %-10s | %-10s\n", "N",
                 "Cerid (GFLOPS,iters)", "Eigen (GFLOPS,iters)", "OBLAS (GFLOPS,iters)", "C/Eigen",
                 "C/OBLAS");
    std::fprintf(stdout, "%s\n",
                 "-----------------------------------------------------------"
                 "-------------------------------------------------------------");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512},
                         crd::usize{1024}, crd::usize{2048}, crd::usize{4096}})
    {
        using crd::hesap::dense::Matrix;
        using crd::hesap::dense::Vector;
        using crd::hesap::dense::Layout;
        Matrix<crd::f64> ca(alloc, n, n);
        Vector<crd::f64> cx(alloc, n);
        Vector<crd::f64> cy(alloc, n);
        fill(ca.data(), n * n, 11U);
        fill(cx.data(), n, 22U);
        fill(cy.data(), n, 33U);
        const crd::f64 alpha = 1.25, beta = 0.5;

        int citers = 0;
        const crd::f64 ce = time_loop_best_of_3(
            [&]() {
                crd::hesap::dense::gemv<crd::f64, Layout::RowMajor>(alpha, ca.cview(), cx.span(),
                                                                    beta, cy.span());
            },
            citers);

        Eigen::MatrixXd ea(n, n);
        Eigen::VectorXd ex(n), ey(n);
        std::memcpy(ea.data(), ca.data(), n * n * sizeof(crd::f64));
        std::memcpy(ex.data(), cx.data(), n * sizeof(crd::f64));
        std::memcpy(ey.data(), cy.data(), n * sizeof(crd::f64));
        int eiters = 0;
        const crd::f64 ee = time_loop_best_of_3(
            [&]() { ey.noalias() = alpha * ea * ex + beta * ey; }, eiters);

        std::vector<crd::f64> oa(n * n), ox(n), oy(n);
        std::memcpy(oa.data(), ca.data(), n * n * sizeof(crd::f64));
        std::memcpy(ox.data(), cx.data(), n * sizeof(crd::f64));
        std::memcpy(oy.data(), cy.data(), n * sizeof(crd::f64));
        int oiters = 0;
        const crd::f64 oe = time_loop_best_of_3(
            [&]() {
                cblas_dgemv(CblasRowMajor, CblasNoTrans, static_cast<int>(n), static_cast<int>(n),
                            alpha, oa.data(), static_cast<int>(n), ox.data(), 1, beta, oy.data(),
                            1);
            },
            oiters);

        const crd::f64 flops_per_iter = 2.0 * static_cast<crd::f64>(n) * static_cast<crd::f64>(n);
        const crd::f64 cg = flops_per_iter * citers / (ce * 1e9);
        const crd::f64 eg = flops_per_iter * eiters / (ee * 1e9);
        const crd::f64 og = flops_per_iter * oiters / (oe * 1e9);
        std::fprintf(stdout, "%-6zu | %8.2f (%6d) | %8.2f (%6d) | %8.2f (%6d) | %8.2fx | %8.2fx\n",
                     static_cast<std::size_t>(n), cg, citers, eg, eiters, og, oiters, cg / eg,
                     cg / og);
        std::fflush(stdout);
    }
}

void bench_symv(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== symv.f64: y = alpha*A_sym*x + beta*y ====\n");
    std::fprintf(stdout, "%-6s | %-22s | %-22s | %-22s | %-10s | %-10s\n", "N",
                 "Cerid (GFLOPS,iters)", "Eigen (GFLOPS,iters)", "OBLAS (GFLOPS,iters)", "C/Eigen",
                 "C/OBLAS");
    std::fprintf(stdout, "%s\n",
                 "-----------------------------------------------------------"
                 "-------------------------------------------------------------");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512},
                         crd::usize{1024}, crd::usize{2048}, crd::usize{4096}})
    {
        using crd::hesap::dense::Symmetric;
        using crd::hesap::dense::Vector;
        Symmetric<crd::f64> ca(alloc, n);
        Vector<crd::f64> cx(alloc, n);
        Vector<crd::f64> cy(alloc, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                ca.at(i, j) = 0.001 * static_cast<crd::f64>((i + 1) * (j + 1));
            }
        }
        fill(cx.data(), n, 22U);
        fill(cy.data(), n, 33U);
        const crd::f64 alpha = 1.25, beta = 0.5;

        int citers = 0;
        const crd::f64 ce = time_loop_best_of_3(
            [&]() { crd::hesap::dense::symv<crd::f64>(alpha, ca, cx.span(), beta, cy.span()); },
            citers);

        // Eigen: build dense symmetric matrix, use selfadjointView.
        Eigen::MatrixXd ea(n, n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                ea(i, j) = ca.at(i, j);
        Eigen::VectorXd ex(n), ey(n);
        std::memcpy(ex.data(), cx.data(), n * sizeof(crd::f64));
        std::memcpy(ey.data(), cy.data(), n * sizeof(crd::f64));
        int eiters = 0;
        const crd::f64 ee = time_loop_best_of_3(
            [&]() { ey.noalias() = alpha * ea.selfadjointView<Eigen::Lower>() * ex + beta * ey; },
            eiters);

        std::vector<crd::f64> oa(n * n), ox(n), oy(n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                oa[i * n + j] = ca.at(i, j);
        std::memcpy(ox.data(), cx.data(), n * sizeof(crd::f64));
        std::memcpy(oy.data(), cy.data(), n * sizeof(crd::f64));
        int oiters = 0;
        const crd::f64 oe = time_loop_best_of_3(
            [&]() {
                cblas_dsymv(CblasRowMajor, CblasLower, static_cast<int>(n), alpha, oa.data(),
                            static_cast<int>(n), ox.data(), 1, beta, oy.data(), 1);
            },
            oiters);

        const crd::f64 flops_per_iter = 2.0 * static_cast<crd::f64>(n) * static_cast<crd::f64>(n);
        const crd::f64 cg = flops_per_iter * citers / (ce * 1e9);
        const crd::f64 eg = flops_per_iter * eiters / (ee * 1e9);
        const crd::f64 og = flops_per_iter * oiters / (oe * 1e9);
        std::fprintf(stdout, "%-6zu | %8.2f (%6d) | %8.2f (%6d) | %8.2f (%6d) | %8.2fx | %8.2fx\n",
                     static_cast<std::size_t>(n), cg, citers, eg, eiters, og, oiters, cg / eg,
                     cg / og);
        std::fflush(stdout);
    }
}

void bench_trsv(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== trsv.f64.lower: solve L * x = b in-place ====\n");
    std::fprintf(stdout, "%-6s | %-22s | %-22s | %-22s | %-10s | %-10s\n", "N",
                 "Cerid (GFLOPS,iters)", "Eigen (GFLOPS,iters)", "OBLAS (GFLOPS,iters)", "C/Eigen",
                 "C/OBLAS");
    std::fprintf(stdout, "%s\n",
                 "-----------------------------------------------------------"
                 "-------------------------------------------------------------");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512},
                         crd::usize{1024}, crd::usize{2048}, crd::usize{4096}})
    {
        using Tri = crd::hesap::dense::Triangular<crd::f64, crd::hesap::dense::TriangularSide::Lower,
                                                  crd::hesap::dense::TriangularDiag::Explicit>;
        using crd::hesap::dense::Vector;
        Tri ca(alloc, n);
        // diagonal-dominant lower triangular so the solve is well-conditioned.
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < i; ++j)
            {
                ca.at(i, j) = 0.001 * static_cast<crd::f64>(j + 1);
            }
            ca.at(i, i) = 10.0 + static_cast<crd::f64>(i);
        }
        Vector<crd::f64> cb0(alloc, n);
        fill(cb0.data(), n, 41U);
        Vector<crd::f64> cb(alloc, n);

        int citers = 0;
        const crd::f64 ce = time_loop_best_of_3(
            [&]() {
                std::memcpy(cb.data(), cb0.data(), n * sizeof(crd::f64));
                crd::hesap::dense::trsv<crd::f64, crd::hesap::dense::TriangularSide::Lower,
                                        crd::hesap::dense::TriangularDiag::Explicit>(ca, cb.span());
            },
            citers);

        Eigen::MatrixXd ea(n, n);
        ea.setZero();
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j <= i; ++j)
                ea(i, j) = ca.at(i, j);
        Eigen::VectorXd eb0(n), eb(n);
        std::memcpy(eb0.data(), cb0.data(), n * sizeof(crd::f64));
        int eiters = 0;
        const crd::f64 ee = time_loop_best_of_3(
            [&]() {
                eb = eb0;
                ea.triangularView<Eigen::Lower>().solveInPlace(eb);
            },
            eiters);

        std::vector<crd::f64> oa(n * n, 0.0);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j <= i; ++j)
                oa[i * n + j] = ca.at(i, j);
        std::vector<crd::f64> ob0(n), ob(n);
        std::memcpy(ob0.data(), cb0.data(), n * sizeof(crd::f64));
        int oiters = 0;
        const crd::f64 oe = time_loop_best_of_3(
            [&]() {
                std::memcpy(ob.data(), ob0.data(), n * sizeof(crd::f64));
                cblas_dtrsv(CblasRowMajor, CblasLower, CblasNoTrans, CblasNonUnit,
                            static_cast<int>(n), oa.data(), static_cast<int>(n), ob.data(), 1);
            },
            oiters);

        // trsv flops ~= n^2 (forward substitution).
        const crd::f64 flops_per_iter = static_cast<crd::f64>(n) * static_cast<crd::f64>(n);
        const crd::f64 cg = flops_per_iter * citers / (ce * 1e9);
        const crd::f64 eg = flops_per_iter * eiters / (ee * 1e9);
        const crd::f64 og = flops_per_iter * oiters / (oe * 1e9);
        std::fprintf(stdout, "%-6zu | %8.2f (%6d) | %8.2f (%6d) | %8.2f (%6d) | %8.2fx | %8.2fx\n",
                     static_cast<std::size_t>(n), cg, citers, eg, eiters, og, oiters, cg / eg,
                     cg / og);
        std::fflush(stdout);
    }
}
} // namespace

int main(int argc, char** argv)
{
    bool p_cores_only = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--all-cores") == 0) { p_cores_only = false; }
    }
#ifdef _WIN32
    if (p_cores_only) { SetProcessAffinityMask(GetCurrentProcess(), 0xFFFFULL); }
#endif
    Eigen::setNbThreads(1);
    openblas_set_num_threads(1);

    const crd::f64 ghz = measure_clock_ghz();
    std::fprintf(stdout,
                 "==== bench_hesap_blas2_vs_reference ====\n"
                 "  SIMD backend : %s\n"
                 "  Clock        : %.2f GHz (P-cores)\n"
                 "  Threads      : 1 each (BLAS L2 is memory-bandwidth-bound)\n"
                 "  Goal         : Cerid >= Eigen AND Cerid >= OpenBLAS\n",
                 crd::math::simd::backend_name(), ghz);

    crd::memory::TlsfAllocator alloc(256ULL * 1024ULL * 1024ULL);
    bench_gemv(&alloc);
    bench_symv(&alloc);
    bench_trsv(&alloc);

    std::fprintf(stdout, "\nDone.\n");
    return 0;
}
