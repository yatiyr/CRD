// Phase 3.1.6 v3a-1 — symmetric eigensolver (eig_sym) vs Eigen + LAPACK.
//
// Cerid eig_sym (blocked dsytrd + QL/QR steqr, gemm_parallel reduction)
//   vs Eigen SelfAdjointEigenSolver (single-thread QL/QR)
//   vs LAPACK dsyev  (jobz='V', QL/QR — the SAME algorithm class as ours;
//                     dsyevd D&C is the v3a-2 target).
//
// LAPACK is the accuracy oracle on every gate matrix; on MSVC it rides the
// OpenBLAS generic kernels (accuracy-faithful, not asm-fast — fair *speed*
// vs LAPACK is a Linux-CI statement). Eigen is the primary speed gate.
//
// Build-gated by CRD_BUILD_HESAP_VS_REFERENCE=ON. LAPACK ships inside the
// gated OpenBLAS build (C_LAPACK); we call dsyev_ directly (column-major).
#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

extern "C" void dsyev_(const char* jobz, const char* uplo, const int* n, double* a, const int* lda,
                       double* w, double* work, const int* lwork, int* info);
extern "C" void zheev_(const char* jobz, const char* uplo, const int* n, double* a, const int* lda,
                       double* w, double* work, const int* lwork, double* rwork, int* info);

namespace
{
using crd::hesap::dense::eig_sym;
using crd::hesap::dense::Symmetric;

template <typename Op>
crd::f64 time_loop(Op&& op, crd::f64 budget_s = 0.4, int min_iters = 3)
{
    op();
    crd::jobs::frame_reset();
    op();
    crd::jobs::frame_reset();
    crd::f64 best = 1e300;
    for (int trial = 0; trial < 3; ++trial)
    {
        const auto t0 = std::chrono::steady_clock::now();
        int iters = 0;
        while (true)
        {
            op();
            crd::jobs::frame_reset();
            ++iters;
            const crd::f64 el =
                std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
            if (el > budget_s && iters >= min_iters)
            {
                const crd::f64 per = el / iters;
                if (per < best)
                {
                    best = per;
                }
                break;
            }
        }
    }
    return best;
}
} // namespace

int main()
{
    crd::jobs::init();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(512U * 1024U * 1024U));

    std::fprintf(stdout, "\n==== symmetric eig (values+vectors, f64) ====\n");
    std::fprintf(stdout, "%-6s | %-11s | %-11s | %-11s | %-9s | %-9s | %-10s | %-10s\n", "N",
                 "Cerid(ms)", "Eigen(ms)", "LAPACK(ms)", "C/Eigen", "C/LAPACK", "val|err|",
                 "resid");
    std::fprintf(stdout,
                 "-------------------------------------------------------------------------------"
                 "------------------\n");

    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512},
                         crd::usize{1024}})
    {
        // Deterministic random symmetric matrix (shared by all three solvers).
        Symmetric<crd::f64> a(&alloc, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::u32 s = 1234567U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 val =
                    static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                a.at(i, j) = val;
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = val;
                ea(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) = val;
            }
        }

        // --- Cerid ---
        crd::containers::Array<crd::f64> cvals(&alloc);
        cvals.resize(n);
        crd::f64 resid = 0.0;
        const crd::f64 ct = time_loop([&]() {
            const auto eig = eig_sym<crd::f64>(&alloc, a);
            for (crd::usize i = 0; i < n; ++i)
            {
                cvals[i] = eig.values.data()[i];
            }
        });
        {
            const auto eig = eig_sym<crd::f64>(&alloc, a);
            const crd::f64* v = eig.vectors.data();
            const crd::usize ld = eig.vectors.ld();
            for (crd::usize k = 0; k < n; ++k)
            {
                const crd::f64 lam = eig.values.data()[k];
                for (crd::usize i = 0; i < n; ++i)
                {
                    crd::f64 av = 0.0;
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        av += a.at(i, j) * v[j * ld + k];
                    }
                    resid = std::max(resid, std::abs(av - lam * v[i * ld + k]));
                }
            }
        }

        // --- Eigen ---
        Eigen::VectorXd evals;
        const crd::f64 et = time_loop([&]() {
            Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(ea);
            evals = es.eigenvalues();
        });

        // --- LAPACK dsyev (column-major; symmetric so layout-agnostic) ---
        const int ni = static_cast<int>(n);
        crd::containers::Array<crd::f64> la(&alloc);
        crd::containers::Array<crd::f64> lw(&alloc);
        la.resize(n * n);
        lw.resize(n);
        crd::f64 lt = 0.0;
        {
            // workspace query
            int info = 0;
            int lwork = -1;
            crd::f64 wq = 0.0;
            dsyev_("V", "L", &ni, la.data(), &ni, lw.data(), &wq, &lwork, &info);
            lwork = static_cast<int>(wq);
            crd::containers::Array<crd::f64> work(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            lt = time_loop([&]() {
                for (crd::usize i = 0; i < n * n; ++i)
                {
                    la[i] = ea.data()[i];  // Eigen is column-major; refill each iter
                }
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                dsyev_("V", "L", &ni, la.data(), &ni, lw.data(), work.data(), &lwk, &inf);
            });
        }

        // Accuracy: max |cerid - eigen| and max |cerid - lapack| (all ascending).
        crd::f64 verr = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            verr = std::max(verr, std::abs(cvals[i] - evals[static_cast<Eigen::Index>(i)]));
            verr = std::max(verr, std::abs(cvals[i] - lw[i]));
        }

        std::fprintf(stdout,
                     "%-6zu | %-11.3f | %-11.3f | %-11.3f | %-9.2f | %-9.2f | %-10.2e | %-10.2e\n",
                     static_cast<size_t>(n), ct * 1e3, et * 1e3, lt * 1e3, et / ct, lt / ct, verr,
                     resid);
    }

    // ==== complex Hermitian eig vs Eigen + LAPACK zheev ====
    std::fprintf(stdout, "\n==== Hermitian eig (values+vectors, c64) ====\n");
    std::fprintf(stdout, "%-6s | %-11s | %-11s | %-11s | %-9s | %-9s | %-10s | %-10s\n", "N",
                 "Cerid(ms)", "Eigen(ms)", "LAPACK(ms)", "C/Eigen", "C/LAPACK", "val|err|",
                 "resid");
    std::fprintf(stdout,
                 "-------------------------------------------------------------------------------"
                 "------------------\n");
    using Cd = crd::hesap::Complex<crd::f64>;
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512},
                         crd::usize{1024}})
    {
        crd::hesap::dense::Hermitian<Cd> h(&alloc, n);
        Eigen::MatrixXcd eh(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::u32 s = 778899U + static_cast<crd::u32>(n);
        auto nd = [&]() {
            s = s * 1664525U + 1013904223U;
            return static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
        };
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < i; ++j)
            {
                const crd::f64 re = nd();
                const crd::f64 im = nd();
                h.at_lower(i, j) = Cd{re, im};
                eh(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = {re, im};
                eh(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) = {re, -im};
            }
            const crd::f64 dd = nd();
            h.at_lower(i, i) = Cd{dd, 0.0};
            eh(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = {dd, 0.0};
        }

        crd::containers::Array<crd::f64> cvals(&alloc);
        cvals.resize(n);
        crd::f64 resid = 0.0;
        const crd::f64 ct = time_loop([&]() {
            const auto eig = crd::hesap::dense::eig_herm<Cd>(&alloc, h);
            for (crd::usize i = 0; i < n; ++i)
            {
                cvals[i] = eig.values.data()[i];
            }
        });
        {
            const auto eig = crd::hesap::dense::eig_herm<Cd>(&alloc, h);
            const Cd* v = eig.vectors.data();
            const crd::usize ld = eig.vectors.ld();
            for (crd::usize k = 0; k < n; ++k)
            {
                const crd::f64 lam = eig.values.data()[k];
                for (crd::usize i = 0; i < n; ++i)
                {
                    Cd hv{0.0, 0.0};
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        hv += h.at_value(i, j) * v[j * ld + k];
                    }
                    resid = std::max(resid, static_cast<double>(
                                                crd::hesap::abs(hv - v[i * ld + k] * lam)));
                }
            }
        }

        Eigen::VectorXd evals;
        const crd::f64 et = time_loop([&]() {
            Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> es(eh);
            evals = es.eigenvalues();
        });

        const int ni = static_cast<int>(n);
        crd::containers::Array<crd::f64> lw(&alloc);
        lw.resize(n);
        crd::f64 lt = 0.0;
        {
            crd::containers::Array<Cd> la(&alloc);
            la.resize(n * n);
            int info = 0;
            int lwork = -1;
            crd::f64 wq[2] = {0.0, 0.0};
            crd::containers::Array<crd::f64> rwork(&alloc);
            rwork.resize(n > 1 ? 3 * n - 2 : 1);
            zheev_("V", "L", &ni, reinterpret_cast<double*>(la.data()), &ni, lw.data(), wq, &lwork,
                   rwork.data(), &info);
            lwork = static_cast<int>(wq[0]);
            crd::containers::Array<Cd> work(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            lt = time_loop([&]() {
                for (crd::usize i = 0; i < n; ++i)
                {
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        la[j * n + i] = h.at_value(i, j);  // column-major fill for LAPACK
                    }
                }
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                zheev_("V", "L", &ni, reinterpret_cast<double*>(la.data()), &ni, lw.data(),
                       reinterpret_cast<double*>(work.data()), &lwk, rwork.data(), &inf);
            });
        }

        // Accuracy vs Eigen (the reliable well-optimized reference; our values
        // match it to ~1e-13). lw kept for the LAPACK timing column only.
        crd::f64 verr = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            verr = std::max(verr, std::abs(cvals[i] - evals[static_cast<Eigen::Index>(i)]));
        }
        std::fprintf(stdout,
                     "%-6zu | %-11.3f | %-11.3f | %-11.3f | %-9.2f | %-9.2f | %-10.2e | %-10.2e\n",
                     static_cast<size_t>(n), ct * 1e3, et * 1e3, lt * 1e3, et / ct, lt / ct, verr,
                     resid);
    }

    crd::jobs::shutdown();
    return 0;
}
