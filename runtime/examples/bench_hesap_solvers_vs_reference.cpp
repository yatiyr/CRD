// bench_hesap_solvers_vs_reference — Reference-class shootout for the
// v0e dense direct solvers (LU / Cholesky / LDLT / QR) vs Eigen.
//
// Apply the policy from docs/PRINCIPLES_reference_class_benchmarking.md:
// every workhorse N gets a head-to-head GFLOPS + relative-error
// measurement. Filed follow-ons for any sub-1× ratio.
//
// Build-gated by `CRD_BUILD_HESAP_VS_REFERENCE=ON`. Eigen is fetched via
// CPM into build/_deps/ — never vendored.

#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/ldlt.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>
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
template <typename T> void fill(T* p, crd::usize n, crd::u32 seed)
{
    crd::u32 s = seed;
    for (crd::usize i = 0; i < n; ++i)
    {
        s = s * 1664525U + 1013904223U;
        p[i] = static_cast<T>(static_cast<crd::i32>(s >> 8) % 1000) * static_cast<T>(0.001);
    }
}

template <typename Op> crd::f64 time_loop(Op&& op, int& iters_out, crd::f64 budget_s = 0.4, int min_iters = 3)
{
    // Per memory/feedback_jobs_parallel_for_frame_arena_exhaustion: reset
    // the per-thread jobs frame arena between iters; otherwise tight loops
    // exhaust the 1 MB arena.
    op();
    crd::jobs::frame_reset();
    op();
    crd::jobs::frame_reset();
    op();
    crd::jobs::frame_reset();
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
            crd::jobs::frame_reset();
            ++iters;
            const auto el = std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
            if (el > budget_s && iters >= min_iters)
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

void print_header()
{
    std::fprintf(stdout, "%-12s | %-6s | %-22s | %-22s | %-10s | %-10s\n", "Solver", "N", "Cerid (GFLOPS,iters)",
                 "Eigen (GFLOPS,iters)", "C/Eigen", "max|err|");
    std::fprintf(stdout, "%s\n",
                 "----------------------------------------------------------"
                 "----------------------------------------------------------");
}

void bench_lu(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== LU partial-pivoting factor+solve (f64) ====\n");
    print_header();
    using crd::hesap::dense::factor_lu;
    using crd::hesap::dense::Layout;
    using crd::hesap::dense::LU;
    using crd::hesap::dense::Matrix;
    using crd::hesap::dense::solve_lu;
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}, crd::usize{1024}})
    {
        Matrix<crd::f64, Layout::RowMajor> a(alloc, n, n);
        // Diagonally-dominant for stable factor.
        crd::u32 s = 42U;
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                a.at(i, j) = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 1000) * 0.001;
            }
            a.at(i, i) += static_cast<crd::f64>(n);
        }
        crd::containers::Array<crd::f64> b(alloc);
        b.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            b[i] = static_cast<crd::f64>(i + 1);
        }

        int citers = 0;
        crd::f64 max_err = 0.0;
        const crd::f64 ce = time_loop(
            [&]()
            {
                LU<crd::f64, Layout::RowMajor> lu(alloc, n);
                factor_lu(lu, a);
                crd::containers::Array<crd::f64> x(alloc);
                x.resize(n);
                for (crd::usize i = 0; i < n; ++i)
                {
                    x[i] = b[i];
                }
                solve_lu(lu, crd::containers::Span<crd::f64>(x.data(), n));
            },
            citers);

        Eigen::MatrixXd ea(n, n);
        std::memcpy(ea.data(), a.data(), n * n * sizeof(crd::f64));
        // Eigen is column-major; transpose since we filled in row-major.
        ea.transposeInPlace();
        Eigen::VectorXd eb(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            eb(i) = b[i];
        }
        int eiters = 0;
        Eigen::VectorXd ex;
        const crd::f64 ee = time_loop(
            [&]()
            {
                Eigen::PartialPivLU<Eigen::MatrixXd> lu(ea);
                ex = lu.solve(eb);
            },
            eiters);

        // Compute Cerid solution once for error.
        {
            LU<crd::f64, Layout::RowMajor> lu(alloc, n);
            factor_lu(lu, a);
            crd::containers::Array<crd::f64> x(alloc);
            x.resize(n);
            for (crd::usize i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            solve_lu(lu, crd::containers::Span<crd::f64>(x.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                const crd::f64 d = x[i] - ex(i);
                const crd::f64 ad = d < 0 ? -d : d;
                if (ad > max_err)
                {
                    max_err = ad;
                }
            }
        }

        // FLOPs for LU factor: ~ (2/3) n^3; solve: ~ 2 n^2. Per-iter total:
        const crd::f64 flops =
            (2.0 / 3.0) * static_cast<crd::f64>(n) * static_cast<crd::f64>(n) * static_cast<crd::f64>(n) +
            2.0 * static_cast<crd::f64>(n) * static_cast<crd::f64>(n);
        const crd::f64 cgflops = flops * citers / ce / 1e9;
        const crd::f64 egflops = flops * eiters / ee / 1e9;
        const crd::f64 ratio = cgflops / egflops;
        std::fprintf(stdout, "%-12s | %-6zu | %8.2f (nw=1,iters=%-4d) | %8.2f (iters=%-4d)      | %.2fx     | %.2e\n",
                     "LU.f64", n, cgflops, citers, egflops, eiters, ratio, max_err);
    }
}

void bench_cholesky(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== Cholesky factor+solve (f64 SPD) ====\n");
    print_header();
    using crd::hesap::dense::Cholesky;
    using crd::hesap::dense::factor_cholesky;
    using crd::hesap::dense::Layout;
    using crd::hesap::dense::solve_cholesky;
    using crd::hesap::dense::Symmetric;
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}, crd::usize{1024}})
    {
        Symmetric<crd::f64> a_sym(alloc, n);
        crd::u32 s = 99U;
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                s = s * 1664525U + 1013904223U;
                a_sym.at(i, j) = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 1000) * 0.001;
            }
            a_sym.at(i, i) += static_cast<crd::f64>(n);
        }
        crd::containers::Array<crd::f64> b(alloc);
        b.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            b[i] = static_cast<crd::f64>(i + 1);
        }

        int citers = 0;
        const crd::f64 ce = time_loop(
            [&]()
            {
                Cholesky<crd::f64, Layout::RowMajor> chol(alloc, n);
                factor_cholesky(chol, a_sym);
                crd::containers::Array<crd::f64> x(alloc);
                x.resize(n);
                for (crd::usize i = 0; i < n; ++i)
                {
                    x[i] = b[i];
                }
                solve_cholesky(chol, crd::containers::Span<crd::f64>(x.data(), n));
            },
            citers);

        Eigen::MatrixXd ea(n, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = a_sym.at(i, j);
            }
        }
        Eigen::VectorXd eb(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            eb(i) = b[i];
        }
        int eiters = 0;
        Eigen::VectorXd ex;
        const crd::f64 ee = time_loop(
            [&]()
            {
                Eigen::LLT<Eigen::MatrixXd> llt(ea);
                ex = llt.solve(eb);
            },
            eiters);

        // Cholesky FLOPs: (1/3) n^3 factor + 2 n^2 solve.
        const crd::f64 flops =
            (1.0 / 3.0) * static_cast<crd::f64>(n) * static_cast<crd::f64>(n) * static_cast<crd::f64>(n) +
            2.0 * static_cast<crd::f64>(n) * static_cast<crd::f64>(n);
        const crd::f64 cgflops = flops * citers / ce / 1e9;
        const crd::f64 egflops = flops * eiters / ee / 1e9;

        // Error
        crd::f64 max_err = 0.0;
        {
            Cholesky<crd::f64, Layout::RowMajor> chol(alloc, n);
            factor_cholesky(chol, a_sym);
            crd::containers::Array<crd::f64> x(alloc);
            x.resize(n);
            for (crd::usize i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            solve_cholesky(chol, crd::containers::Span<crd::f64>(x.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                const crd::f64 d = x[i] - ex(i);
                const crd::f64 ad = d < 0 ? -d : d;
                if (ad > max_err)
                    max_err = ad;
            }
        }
        std::fprintf(stdout, "%-12s | %-6zu | %8.2f (iters=%-4d)      | %8.2f (iters=%-4d)      | %.2fx     | %.2e\n",
                     "Chol.f64", n, cgflops, citers, egflops, eiters, cgflops / egflops, max_err);
    }
}

void bench_qr(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== QR Householder factor+solve (f64 square) ====\n");
    print_header();
    using crd::hesap::dense::factor_qr;
    using crd::hesap::dense::Layout;
    using crd::hesap::dense::Matrix;
    using crd::hesap::dense::QR;
    using crd::hesap::dense::solve_qr;
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}})
    {
        Matrix<crd::f64, Layout::RowMajor> a(alloc, n, n);
        crd::u32 s = 13U;
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                a.at(i, j) = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 1000) * 0.001;
            }
            a.at(i, i) += static_cast<crd::f64>(n);
        }
        crd::containers::Array<crd::f64> b(alloc);
        b.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            b[i] = static_cast<crd::f64>(i + 1);
        }

        int citers = 0;
        const crd::f64 ce = time_loop(
            [&]()
            {
                QR<crd::f64, Layout::RowMajor> qr(alloc, n, n);
                factor_qr(qr, a);
                crd::containers::Array<crd::f64> bx(alloc);
                bx.resize(n);
                for (crd::usize i = 0; i < n; ++i)
                {
                    bx[i] = b[i];
                }
                solve_qr(qr, crd::containers::Span<crd::f64>(bx.data(), n));
            },
            citers);

        Eigen::MatrixXd ea(n, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = a.at(i, j);
            }
        }
        Eigen::VectorXd eb(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            eb(i) = b[i];
        }
        int eiters = 0;
        Eigen::VectorXd ex;
        const crd::f64 ee = time_loop(
            [&]()
            {
                Eigen::HouseholderQR<Eigen::MatrixXd> qr(ea);
                ex = qr.solve(eb);
            },
            eiters);

        // QR FLOPs: ~2 n^3 factor + 2 n^2 solve. (Heuristic; Householder is ~(4/3) n^3.)
        const crd::f64 flops =
            (4.0 / 3.0) * static_cast<crd::f64>(n) * static_cast<crd::f64>(n) * static_cast<crd::f64>(n) +
            2.0 * static_cast<crd::f64>(n) * static_cast<crd::f64>(n);
        const crd::f64 cgflops = flops * citers / ce / 1e9;
        const crd::f64 egflops = flops * eiters / ee / 1e9;

        crd::f64 max_err = 0.0;
        {
            QR<crd::f64, Layout::RowMajor> qr(alloc, n, n);
            factor_qr(qr, a);
            crd::containers::Array<crd::f64> bx(alloc);
            bx.resize(n);
            for (crd::usize i = 0; i < n; ++i)
            {
                bx[i] = b[i];
            }
            solve_qr(qr, crd::containers::Span<crd::f64>(bx.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                const crd::f64 d = bx[i] - ex(i);
                const crd::f64 ad = d < 0 ? -d : d;
                if (ad > max_err)
                    max_err = ad;
            }
        }
        std::fprintf(stdout, "%-12s | %-6zu | %8.2f (iters=%-4d)      | %8.2f (iters=%-4d)      | %.2fx     | %.2e\n",
                     "QR.f64", n, cgflops, citers, egflops, eiters, cgflops / egflops, max_err);
    }
}

void bench_ldlt(crd::memory::IAllocator* alloc)
{
    std::fprintf(stdout, "\n==== LDLT Bunch-Kaufman factor+solve (f64 symmetric) ====\n");
    print_header();
    using crd::hesap::dense::factor_ldlt;
    using crd::hesap::dense::Layout;
    using crd::hesap::dense::LDLT;
    using crd::hesap::dense::solve_ldlt;
    using crd::hesap::dense::Symmetric;
    for (crd::usize n :
         {crd::usize{32}, crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}, crd::usize{1024}})
    {
        Symmetric<crd::f64> a_sym(alloc, n);
        crd::u32 s = 333U;
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                s = s * 1664525U + 1013904223U;
                a_sym.at(i, j) = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 1000) * 0.001;
            }
            a_sym.at(i, i) += static_cast<crd::f64>(n);
        }
        crd::containers::Array<crd::f64> b(alloc);
        b.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            b[i] = static_cast<crd::f64>(i + 1);
        }

        int citers = 0;
        const crd::f64 ce = time_loop(
            [&]()
            {
                LDLT<crd::f64, Layout::RowMajor> ldlt(alloc, n);
                factor_ldlt(ldlt, a_sym);
                crd::containers::Array<crd::f64> x(alloc);
                x.resize(n);
                for (crd::usize i = 0; i < n; ++i)
                {
                    x[i] = b[i];
                }
                solve_ldlt(ldlt, crd::containers::Span<crd::f64>(x.data(), n));
            },
            citers);

        Eigen::MatrixXd ea(n, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = a_sym.at(i, j);
            }
        }
        Eigen::VectorXd eb(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            eb(i) = b[i];
        }
        int eiters = 0;
        Eigen::VectorXd ex;
        const crd::f64 ee = time_loop(
            [&]()
            {
                Eigen::LDLT<Eigen::MatrixXd> ldlt(ea);
                ex = ldlt.solve(eb);
            },
            eiters);

        const crd::f64 flops =
            (1.0 / 3.0) * static_cast<crd::f64>(n) * static_cast<crd::f64>(n) * static_cast<crd::f64>(n) +
            2.0 * static_cast<crd::f64>(n) * static_cast<crd::f64>(n);
        const crd::f64 cgflops = flops * citers / ce / 1e9;
        const crd::f64 egflops = flops * eiters / ee / 1e9;

        crd::f64 max_err = 0.0;
        {
            LDLT<crd::f64, Layout::RowMajor> ldlt(alloc, n);
            factor_ldlt(ldlt, a_sym);
            crd::containers::Array<crd::f64> x(alloc);
            x.resize(n);
            for (crd::usize i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            solve_ldlt(ldlt, crd::containers::Span<crd::f64>(x.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                const crd::f64 d = x[i] - ex(i);
                const crd::f64 ad = d < 0 ? -d : d;
                if (ad > max_err)
                    max_err = ad;
            }
        }
        std::fprintf(stdout, "%-12s | %-6zu | %8.2f (iters=%-4d)      | %8.2f (iters=%-4d)      | %.2fx     | %.2e\n",
                     "LDLT.f64", n, cgflops, citers, egflops, eiters, cgflops / egflops, max_err);
    }
}

} // namespace

int main()
{
#ifdef _WIN32
    // Pin to P-cores for the i9-14900K (first 16 logical = 8 P-cores * 2 HT).
    SetProcessAffinityMask(GetCurrentProcess(), 0xFFFFULL);
#endif
    crd::jobs::init();
    // Enable Eigen multi-threading for apples-to-apples comparison vs Cerid's
    // gemm_parallel. Eigen 3.4's MT requires OpenMP in the caller TU
    // (`#pragma omp parallel` expanded inline). Without setNbThreads, Eigen
    // runs serial regardless of the OpenMP linker setup.
    Eigen::initParallel();
    Eigen::setNbThreads(16);
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256ULL * 1024ULL * 1024ULL));

    std::fprintf(stdout,
                 "=== hesap dense direct solvers: head-to-head vs Eigen 3.4 ===\n"
                 "Per docs/PRINCIPLES_reference_class_benchmarking.md.\n"
                 "P-core affinity pinned (0xFFFF mask on 14900K).\n"
                 "Eigen::nbThreads() = %d (after setNbThreads(16))\n"
                 "Best-of-3 measurement with 3 warmup iters.\n",
                 Eigen::nbThreads());

    bench_lu(&alloc);
    bench_cholesky(&alloc);
    bench_qr(&alloc);
    bench_ldlt(&alloc);

    std::fprintf(stdout, "\n=== shootout complete ===\n");
    crd::jobs::shutdown();
    return 0;
}
