// Phase 3.1.6 v3b-1b — SVD (svd / svdvals) vs Eigen + LAPACK.
//
// FOUR reference columns (feedback_always_bench_both_eigen_and_lapack):
//   Eigen JacobiSVD (O(n^3) one-sided Jacobi — the easy crush)
//   Eigen BDCSVD    (divide-and-conquer — the real target)
//   LAPACK dgesvd   (dbdsqr — our direct algorithmic peer)
//   LAPACK dgesdd   (divide-and-conquer — the harder target)
//
// Cerid svd = Golub-Kahan bidiagonalization + Demmel-Kahan dbdsqr (serial
// baseline; the parallel split-block crush is v3b-1b-perf). LAPACK is the
// accuracy oracle; on MSVC it rides OpenBLAS generic kernels (accuracy-faithful,
// fair *speed* vs LAPACK is a Linux-CI statement). Eigen is the speed gate.
//
// Build-gated by CRD_BUILD_HESAP_VS_REFERENCE=ON.
#include <crd/containers/array.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/svd.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

extern "C" void dgesvd_(const char* jobu, const char* jobvt, const int* m, const int* n, double* a,
                        const int* lda, double* s, double* u, const int* ldu, double* vt, const int* ldvt,
                        double* work, const int* lwork, int* info);
extern "C" void dgesdd_(const char* jobz, const int* m, const int* n, double* a, const int* lda, double* s,
                        double* u, const int* ldu, double* vt, const int* ldvt, double* work, const int* lwork,
                        int* iwork, int* info);

namespace
{
using crd::hesap::dense::Matrix;
using crd::hesap::dense::svd;
using crd::hesap::dense::svdvals;

template <typename Op>
crd::f64 time_loop(Op&& op, crd::f64 budget_s = 0.4, int min_iters = 3)
{
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
            const crd::f64 el = std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
            if (el > budget_s && iters >= min_iters)
            {
                best = std::min(best, el / iters);
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
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(768U * 1024U * 1024U));

    // ==== full SVD (values + thin vectors, square A, f64) ====
    std::fprintf(stdout, "\n==== SVD (values + thin U,V; square A, f64) ====\n");
    std::fprintf(stdout, "%-6s | %-9s | %-9s | %-9s | %-9s | %-9s | %-7s | %-7s | %-7s | %-7s | %-9s | %-9s\n",
                 "N", "Cerid", "Jacobi", "BDC", "dgesvd", "dgesdd", "C/Jac", "C/BDC", "C/svd", "C/sdd",
                 "val|err|", "recon");
    std::fprintf(stdout, "%s\n",
                 "----------------------------------------------------------------------------------------"
                 "--------------------------------");

    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}})
    {
        const int ni = static_cast<int>(n);
        Matrix<crd::f64> a(&alloc, n, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::u32 s = 777013U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 val = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                a.at(i, j) = val;
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = val;
            }
        }

        // --- Cerid ---
        crd::containers::Array<crd::f64> cs(&alloc);
        cs.resize(n);
        crd::f64 recon = 0.0;
        const crd::f64 ct = time_loop([&]() {
            const auto r = svd<crd::f64>(&alloc, a);
            for (crd::usize i = 0; i < n; ++i)
            {
                cs[i] = r.s.data()[i];
            }
        });
        {
            const auto r = svd<crd::f64>(&alloc, a);
            for (crd::usize i = 0; i < n; ++i)
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    crd::f64 acc = 0.0;
                    for (crd::usize k = 0; k < n; ++k)
                    {
                        acc += r.u.at(i, k) * r.s.data()[k] * r.v.at(j, k);
                    }
                    recon = std::max(recon, std::abs(acc - a.at(i, j)));
                }
            }
        }

        // --- Eigen JacobiSVD ---
        const crd::f64 jt = time_loop([&]() {
            Eigen::JacobiSVD<Eigen::MatrixXd> sv(ea, Eigen::ComputeThinU | Eigen::ComputeThinV);
            (void)sv.singularValues();
        });
        // --- Eigen BDCSVD ---
        Eigen::VectorXd bvals;
        const crd::f64 bt = time_loop([&]() {
            Eigen::BDCSVD<Eigen::MatrixXd> sv(ea, Eigen::ComputeThinU | Eigen::ComputeThinV);
            bvals = sv.singularValues();
        });

        // --- LAPACK dgesvd / dgesdd (column-major; ea.data() is col-major) ---
        crd::containers::Array<crd::f64> la(&alloc);
        crd::containers::Array<crd::f64> ls(&alloc);
        crd::containers::Array<crd::f64> lu(&alloc);
        crd::containers::Array<crd::f64> lvt(&alloc);
        la.resize(n * n);
        ls.resize(n);
        lu.resize(n * n);
        lvt.resize(n * n);

        crd::f64 svdt = 0.0;
        {
            int info = 0;
            int lwork = -1;
            crd::f64 wq = 0.0;
            dgesvd_("S", "S", &ni, &ni, la.data(), &ni, ls.data(), lu.data(), &ni, lvt.data(), &ni, &wq, &lwork,
                    &info);
            lwork = static_cast<int>(wq);
            crd::containers::Array<crd::f64> work(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            svdt = time_loop([&]() {
                for (crd::usize i = 0; i < n * n; ++i)
                {
                    la[i] = ea.data()[i];
                }
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                dgesvd_("S", "S", &ni, &ni, la.data(), &ni, ls.data(), lu.data(), &ni, lvt.data(), &ni,
                        work.data(), &lwk, &inf);
            });
        }
        crd::f64 sddt = 0.0;
        {
            crd::containers::Array<int> iwork(&alloc);
            iwork.resize(8 * n);
            int info = 0;
            int lwork = -1;
            crd::f64 wq = 0.0;
            dgesdd_("S", &ni, &ni, la.data(), &ni, ls.data(), lu.data(), &ni, lvt.data(), &ni, &wq, &lwork,
                    iwork.data(), &info);
            lwork = static_cast<int>(wq);
            crd::containers::Array<crd::f64> work(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            sddt = time_loop([&]() {
                for (crd::usize i = 0; i < n * n; ++i)
                {
                    la[i] = ea.data()[i];
                }
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                dgesdd_("S", &ni, &ni, la.data(), &ni, ls.data(), lu.data(), &ni, lvt.data(), &ni, work.data(),
                        &lwk, iwork.data(), &inf);
            });
        }

        // Accuracy: max |cerid_sv - lapack_sv| (both descending).
        crd::f64 verr = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            verr = std::max(verr, std::abs(cs[i] - ls[i]));
        }

        std::fprintf(stdout,
                     "%-6zu | %-9.3f | %-9.3f | %-9.3f | %-9.3f | %-9.3f | %-7.2f | %-7.2f | %-7.2f | %-7.2f | "
                     "%-9.2e | %-9.2e\n",
                     static_cast<size_t>(n), ct * 1e3, jt * 1e3, bt * 1e3, svdt * 1e3, sddt * 1e3, jt / ct,
                     bt / ct, svdt / ct, sddt / ct, verr, recon);
    }

    // ==== singular values only (square A, f64) — svdvals vs 4 references ====
    std::fprintf(stdout, "\n==== SVD (values only; square A, f64) ====\n");
    std::fprintf(stdout, "%-6s | %-9s | %-9s | %-9s | %-9s | %-9s | %-7s | %-7s | %-7s | %-7s | %-9s\n", "N",
                 "Cerid", "Jacobi", "BDC", "dgesvd", "dgesdd", "C/Jac", "C/BDC", "C/svd", "C/sdd", "val|err|");
    std::fprintf(stdout, "%s\n",
                 "----------------------------------------------------------------------------------------"
                 "--------------------");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}, crd::usize{1024}})
    {
        const int ni = static_cast<int>(n);
        Matrix<crd::f64> a(&alloc, n, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::u32 s = 9090909U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 val = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                a.at(i, j) = val;
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = val;
            }
        }

        crd::containers::Array<crd::f64> cs(&alloc);
        cs.resize(n);
        const crd::f64 ct = time_loop([&]() {
            const auto v = svdvals<crd::f64>(&alloc, a);
            for (crd::usize i = 0; i < n; ++i)
            {
                cs[i] = v.data()[i];
            }
        });

        const crd::f64 jt = time_loop([&]() {
            Eigen::JacobiSVD<Eigen::MatrixXd> sv(ea);
            (void)sv.singularValues();
        });
        const crd::f64 bt = time_loop([&]() {
            Eigen::BDCSVD<Eigen::MatrixXd> sv(ea);
            (void)sv.singularValues();
        });

        crd::containers::Array<crd::f64> la(&alloc);
        crd::containers::Array<crd::f64> ls(&alloc);
        la.resize(n * n);
        ls.resize(n);
        crd::f64 svdt = 0.0;
        {
            int info = 0;
            int lwork = -1;
            crd::f64 wq = 0.0;
            dgesvd_("N", "N", &ni, &ni, la.data(), &ni, ls.data(), nullptr, &ni, nullptr, &ni, &wq, &lwork,
                    &info);
            lwork = static_cast<int>(wq);
            crd::containers::Array<crd::f64> work(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            svdt = time_loop([&]() {
                for (crd::usize i = 0; i < n * n; ++i)
                {
                    la[i] = ea.data()[i];
                }
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                dgesvd_("N", "N", &ni, &ni, la.data(), &ni, ls.data(), nullptr, &ni, nullptr, &ni, work.data(),
                        &lwk, &inf);
            });
        }
        crd::f64 sddt = 0.0;
        {
            crd::containers::Array<int> iwork(&alloc);
            iwork.resize(8 * n);
            int info = 0;
            int lwork = -1;
            crd::f64 wq = 0.0;
            dgesdd_("N", &ni, &ni, la.data(), &ni, ls.data(), nullptr, &ni, nullptr, &ni, &wq, &lwork,
                    iwork.data(), &info);
            lwork = static_cast<int>(wq);
            crd::containers::Array<crd::f64> work(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            sddt = time_loop([&]() {
                for (crd::usize i = 0; i < n * n; ++i)
                {
                    la[i] = ea.data()[i];
                }
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                dgesdd_("N", &ni, &ni, la.data(), &ni, ls.data(), nullptr, &ni, nullptr, &ni, work.data(),
                        &lwk, iwork.data(), &inf);
            });
        }

        crd::f64 verr = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            verr = std::max(verr, std::abs(cs[i] - ls[i]));
        }

        std::fprintf(stdout,
                     "%-6zu | %-9.3f | %-9.3f | %-9.3f | %-9.3f | %-9.3f | %-7.2f | %-7.2f | %-7.2f | %-7.2f | "
                     "%-9.2e\n",
                     static_cast<size_t>(n), ct * 1e3, jt * 1e3, bt * 1e3, svdt * 1e3, sddt * 1e3, jt / ct,
                     bt / ct, svdt / ct, sddt / ct, verr);
    }

    crd::jobs::shutdown();
    return 0;
}
