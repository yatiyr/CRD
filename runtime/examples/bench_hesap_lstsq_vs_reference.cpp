// Phase 3.1.6 v3c-1b — least-squares (lstsq / pinv) vs Eigen + LAPACK.
//
// Three method peerings (feedback_always_bench_both_eigen_and_lapack):
//   Cerid lstsq QR  vs  Eigen HouseholderQR::solve            + LAPACK dgels
//   Cerid lstsq COD vs  Eigen CompleteOrthogonalDecomposition + LAPACK dgelsy
//   Cerid lstsq SVD vs  Eigen BDCSVD::solve                   + LAPACK dgelsd
//   Cerid pinv      vs  Eigen completeOrthogonalDecomposition().pseudoInverse()
//
// LAPACK rides OpenBLAS generic kernels on MSVC (accuracy-faithful; fair *speed*
// vs LAPACK is a Linux-CI statement). Eigen is the speed gate. The inner
// factorizations (blocked QR / D&C SVD) already beat Eigen + LAPACK, so lstsq
// inherits the crush; COD is the new kernel measured directly here.
//
// Build-gated by CRD_BUILD_HESAP_VS_REFERENCE=ON.
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/hesap/dense/lstsq.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/qr.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

extern "C" void dgels_(const char* trans, const int* m, const int* n, const int* nrhs, double* a, const int* lda,
                       double* b, const int* ldb, double* work, const int* lwork, int* info);
extern "C" void dgelsy_(const int* m, const int* n, const int* nrhs, double* a, const int* lda, double* b,
                        const int* ldb, int* jpvt, const double* rcond, int* rank, double* work, const int* lwork,
                        int* info);
extern "C" void dgelsd_(const int* m, const int* n, const int* nrhs, double* a, const int* lda, double* b,
                        const int* ldb, double* s, const double* rcond, int* rank, double* work, const int* lwork,
                        int* iwork, int* info);

namespace
{
using crd::hesap::dense::factor_qr;
using crd::hesap::dense::factor_qr_unblocked;
using crd::hesap::dense::Layout;
using crd::hesap::dense::lstsq;
using crd::hesap::dense::LstSqMethod;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::pinv;
using crd::hesap::dense::QR;
using crd::hesap::dense::Vector;

template <typename Op> crd::f64 time_loop(Op&& op, crd::f64 budget_s = 0.4, int min_iters = 3)
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1024U * 1024U * 1024U));

    std::fprintf(stdout, "\n==== least-squares: overdetermined full-rank m=2n, single RHS, f64 ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-9s %-7s | %-9s %-9s %-7s | %-9s %-9s %-7s\n", "n", "C-QR", "EigQR", "C/Eig",
                 "C-COD", "EigCOD", "C/Eig", "C-SVD", "EigBDC", "C/Eig");
    std::fprintf(stdout, "%s\n",
                 "--------------------------------------------------------------------------------------");

    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}})
    {
        const crd::usize m = 2 * n;
        const int mi = static_cast<int>(m);
        const int ni = static_cast<int>(n);

        Matrix<crd::f64> a(&alloc, m, n);
        Vector<crd::f64> b(&alloc, m);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(n));
        Eigen::VectorXd eb(static_cast<Eigen::Index>(m));
        crd::u32 s = 90113U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 v =
                    static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001 + (i == j ? 4.0 : 0.0);
                a.at(i, j) = v;
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = v;
            }
            s = s * 1664525U + 1013904223U;
            const crd::f64 bv = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
            b(i) = bv;
            eb(static_cast<Eigen::Index>(i)) = bv;
        }

        const crd::f64 c_qr = time_loop(
            [&]()
            {
                auto r = lstsq<crd::f64>(&alloc, a, b, LstSqMethod::QR, -1.0, false);
                volatile crd::f64 sink = r.x.at(0, 0);
                (void)sink;
            });
        const crd::f64 c_cod = time_loop(
            [&]()
            {
                auto r = lstsq<crd::f64>(&alloc, a, b, LstSqMethod::COD, -1.0, false);
                volatile crd::f64 sink = r.x.at(0, 0);
                (void)sink;
            });
        const crd::f64 c_svd = time_loop(
            [&]()
            {
                auto r = lstsq<crd::f64>(&alloc, a, b, LstSqMethod::SVD, -1.0, false);
                volatile crd::f64 sink = r.x.at(0, 0);
                (void)sink;
            });

        const crd::f64 e_qr = time_loop(
            [&]()
            {
                Eigen::VectorXd x = ea.householderQr().solve(eb);
                (void)x.sum();
            });
        const crd::f64 e_cod = time_loop(
            [&]()
            {
                Eigen::VectorXd x = ea.completeOrthogonalDecomposition().solve(eb);
                (void)x.sum();
            });
        const crd::f64 e_bdc = time_loop(
            [&]()
            {
                Eigen::VectorXd x = ea.bdcSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(eb);
                (void)x.sum();
            });

        std::fprintf(stdout, "%-6zu | %8.5f %8.5f %6.2fx | %8.5f %8.5f %6.2fx | %8.5f %8.5f %6.2fx\n",
                     static_cast<size_t>(n), c_qr * 1e3, e_qr * 1e3, e_qr / c_qr, c_cod * 1e3, e_cod * 1e3,
                     e_cod / c_cod, c_svd * 1e3, e_bdc * 1e3, e_bdc / c_svd);
        (void)mi;
        (void)ni;
    }

    // ==== LAPACK column (dgels / dgelsy / dgelsd), accuracy oracle + fair-on-Linux speed ====
    std::fprintf(stdout, "\n==== same systems vs LAPACK (column-major; OpenBLAS-generic on MSVC) ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-9s %-7s | %-9s %-9s %-7s | %-9s %-9s %-7s\n", "n", "C-QR", "dgels", "C/lp",
                 "C-COD", "dgelsy", "C/lp", "C-SVD", "dgelsd", "C/lp");
    std::fprintf(stdout, "%s\n",
                 "--------------------------------------------------------------------------------------");

    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}})
    {
        const crd::usize m = 2 * n;
        const int mi = static_cast<int>(m);
        const int ni = static_cast<int>(n);
        const int nrhs = 1;

        Matrix<crd::f64> a(&alloc, m, n);
        Vector<crd::f64> b(&alloc, m);
        // Column-major reference copy of A (lda=m) and b.
        crd::containers::Array<crd::f64> acm(&alloc);
        crd::containers::Array<crd::f64> bcm(&alloc);
        acm.resize(m * n);
        bcm.resize(m);
        crd::u32 s = 90113U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 v =
                    static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001 + (i == j ? 4.0 : 0.0);
                a.at(i, j) = v;
                acm[j * m + i] = v;
            }
            s = s * 1664525U + 1013904223U;
            const crd::f64 bv = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
            b(i) = bv;
            bcm[i] = bv;
        }

        const crd::f64 c_qr = time_loop(
            [&]()
            {
                auto r = lstsq<crd::f64>(&alloc, a, b, LstSqMethod::QR, -1.0, false);
                volatile crd::f64 sink = r.x.at(0, 0);
                (void)sink;
            });
        const crd::f64 c_cod = time_loop(
            [&]()
            {
                auto r = lstsq<crd::f64>(&alloc, a, b, LstSqMethod::COD, -1.0, false);
                volatile crd::f64 sink = r.x.at(0, 0);
                (void)sink;
            });
        const crd::f64 c_svd = time_loop(
            [&]()
            {
                auto r = lstsq<crd::f64>(&alloc, a, b, LstSqMethod::SVD, -1.0, false);
                volatile crd::f64 sink = r.x.at(0, 0);
                (void)sink;
            });

        // dgels (full-rank QR). Workspace query then solve; fresh A,b copy each iter.
        crd::containers::Array<crd::f64> aw(&alloc);
        crd::containers::Array<crd::f64> bw(&alloc);
        crd::containers::Array<crd::f64> work(&alloc);
        aw.resize(m * n);
        bw.resize(m);
        const crd::f64 rcond = -1.0;
        int info = 0;
        int lwork = -1;
        int rank = 0;
        crd::f64 wq = 0.0;
        dgels_("N", &mi, &ni, &nrhs, acm.data(), &mi, bcm.data(), &mi, &wq, &lwork, &info);
        lwork = static_cast<int>(wq);
        work.resize(static_cast<crd::usize>(lwork));
        const crd::f64 lp_qr = time_loop(
            [&]()
            {
                for (crd::usize k = 0; k < m * n; ++k)
                    aw[k] = acm[k];
                for (crd::usize k = 0; k < m; ++k)
                    bw[k] = bcm[k];
                int inf = 0;
                dgels_("N", &mi, &ni, &nrhs, aw.data(), &mi, bw.data(), &mi, work.data(), &lwork, &inf);
            });

        // dgelsy (COD). jpvt + workspace query.
        crd::containers::Array<int> jpvt(&alloc);
        jpvt.resize(n);
        int lwork_y = -1;
        dgelsy_(&mi, &ni, &nrhs, acm.data(), &mi, bcm.data(), &mi, jpvt.data(), &rcond, &rank, &wq, &lwork_y, &info);
        lwork_y = static_cast<int>(wq);
        crd::containers::Array<crd::f64> work_y(&alloc);
        work_y.resize(static_cast<crd::usize>(lwork_y));
        const crd::f64 lp_cod = time_loop(
            [&]()
            {
                for (crd::usize k = 0; k < m * n; ++k)
                    aw[k] = acm[k];
                for (crd::usize k = 0; k < m; ++k)
                    bw[k] = bcm[k];
                for (crd::usize k = 0; k < n; ++k)
                    jpvt[k] = 0;
                int inf = 0;
                int rk = 0;
                dgelsy_(&mi, &ni, &nrhs, aw.data(), &mi, bw.data(), &mi, jpvt.data(), &rcond, &rk, work_y.data(),
                        &lwork_y, &inf);
            });

        // dgelsd (SVD D&C). s + iwork + workspace query.
        crd::containers::Array<crd::f64> sv(&alloc);
        sv.resize(n < m ? n : m);
        int lwork_d = -1;
        crd::containers::Array<int> iwork(&alloc);
        // LIWORK >= 3*MINMN*NLVL + 11*MINMN; NLVL grows like log2(MINMN). Use a
        // generous NLVL cap of 24 so dgelsd never writes past the buffer.
        const crd::usize minmn = (n < m ? n : m);
        iwork.resize(3 * minmn * 24 + 11 * minmn + 64);
        dgelsd_(&mi, &ni, &nrhs, acm.data(), &mi, bcm.data(), &mi, sv.data(), &rcond, &rank, &wq, &lwork_d,
                iwork.data(), &info);
        lwork_d = static_cast<int>(wq);
        crd::containers::Array<crd::f64> work_d(&alloc);
        work_d.resize(static_cast<crd::usize>(lwork_d));
        const crd::f64 lp_svd = time_loop(
            [&]()
            {
                for (crd::usize k = 0; k < m * n; ++k)
                    aw[k] = acm[k];
                for (crd::usize k = 0; k < m; ++k)
                    bw[k] = bcm[k];
                int inf = 0;
                int rk = 0;
                dgelsd_(&mi, &ni, &nrhs, aw.data(), &mi, bw.data(), &mi, sv.data(), &rcond, &rk, work_d.data(),
                        &lwork_d, iwork.data(), &inf);
            });

        std::fprintf(stdout, "%-6zu | %8.5f %8.5f %6.2fx | %8.5f %8.5f %6.2fx | %8.5f %8.5f %6.2fx\n",
                     static_cast<size_t>(n), c_qr * 1e3, lp_qr * 1e3, lp_qr / c_qr, c_cod * 1e3, lp_cod * 1e3,
                     lp_cod / c_cod, c_svd * 1e3, lp_svd * 1e3, lp_svd / c_svd);
    }

    // ==== pinv vs Eigen completeOrthogonalDecomposition().pseudoInverse() ====
    std::fprintf(stdout, "\n==== pinv (rectangular m=2n, f64) ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-9s %-7s\n", "n", "C-pinv", "EigCOD", "C/Eig");
    std::fprintf(stdout, "%s\n", "----------------------------------------");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512}})
    {
        const crd::usize m = 2 * n;
        Matrix<crd::f64> a(&alloc, m, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(n));
        crd::u32 s = 55501U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 v =
                    static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001 + (i == j ? 4.0 : 0.0);
                a.at(i, j) = v;
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = v;
            }
        }
        const crd::f64 c_pinv = time_loop(
            [&]()
            {
                auto p = pinv<crd::f64>(&alloc, a);
                volatile crd::f64 sink = p.at(0, 0);
                (void)sink;
            });
        const crd::f64 e_pinv = time_loop(
            [&]()
            {
                Eigen::MatrixXd p = ea.completeOrthogonalDecomposition().pseudoInverse();
                (void)p.sum();
            });
        std::fprintf(stdout, "%-6zu | %8.5f %8.5f %6.2fx\n", static_cast<size_t>(n), c_pinv * 1e3, e_pinv * 1e3,
                     e_pinv / c_pinv);
    }

    // ==== v3c-1c crossover map: QR factor blocked vs unblocked vs Eigen ====
    // Factor-only (no solve), tall m=2n, fine n grid → pick the unblocked/blocked
    // crossover and confirm the unblocked path ties/beats Eigen at small/mid n.
    std::fprintf(stdout, "\n==== QR factor (tall m=2n, f64): blocked vs unblocked vs Eigen ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-9s %-9s | %-7s %-7s %-7s\n", "n", "blocked", "unblock", "EigQR", "ub/blk",
                 "blk/Eig", "ub/Eig");
    std::fprintf(stdout, "%s\n", "--------------------------------------------------------------------");
    for (crd::usize n : {crd::usize{64}, crd::usize{96}, crd::usize{128}, crd::usize{192}, crd::usize{256},
                         crd::usize{384}, crd::usize{512}, crd::usize{768}})
    {
        const crd::usize m = 2 * n;
        Matrix<crd::f64> a(&alloc, m, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(n));
        crd::u32 s = 90011U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 v =
                    static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001 + (i == j ? 4.0 : 0.0);
                a.at(i, j) = v;
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = v;
            }
        }
        QR<crd::f64> qr_b(&alloc, m, n);
        QR<crd::f64> qr_u(&alloc, m, n);
        auto copy_in = [&](QR<crd::f64>& q)
        {
            auto& p = q.packed();
            for (crd::usize i = 0; i < m; ++i)
                for (crd::usize j = 0; j < n; ++j)
                    p.at(i, j) = a.at(i, j);
        };
        const crd::f64 t_blk = time_loop(
            [&]()
            {
                copy_in(qr_b);
                factor_qr<crd::f64, Layout::RowMajor>(qr_b);
                volatile crd::f64 sink = qr_b.packed().at(0, 0);
                (void)sink;
            });
        const crd::f64 t_ub = time_loop(
            [&]()
            {
                copy_in(qr_u);
                factor_qr_unblocked<crd::f64, Layout::RowMajor>(qr_u);
                volatile crd::f64 sink = qr_u.packed().at(0, 0);
                (void)sink;
            });
        const crd::f64 t_eig = time_loop(
            [&]()
            {
                Eigen::HouseholderQR<Eigen::MatrixXd> h(ea);
                volatile crd::f64 sink = h.matrixQR()(0, 0);
                (void)sink;
            });
        std::fprintf(stdout, "%-6zu | %8.4f %8.4f %8.4f | %6.2fx %6.2fx %6.2fx\n", static_cast<size_t>(n), t_blk * 1e3,
                     t_ub * 1e3, t_eig * 1e3, t_blk / t_ub, t_eig / t_blk, t_eig / t_ub);
    }

    crd::jobs::shutdown();
    return 0;
}
