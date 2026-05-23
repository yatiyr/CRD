// Phase 3.1.6 v3d-1a — Hessenberg reduction vs Eigen + LAPACK.
//
//   Cerid hessenberg (blocked dgehrd: dlahr2 panel + gemm/dlarfb BLAS-3 trailing)
//   vs Eigen HessenbergDecomposition (unblocked) AND LAPACK dgehrd (blocked BLAS-3).
//
// The blocked trailing update routes through the v0d gemm_parallel that already
// crushes — the lever for beating LAPACK's own BLAS-3 dgehrd. LAPACK rides
// OpenBLAS-generic on MSVC (fair *speed* is a Linux-CI statement; the accuracy
// oracle is the recon test). Build-gated by CRD_BUILD_HESAP_VS_REFERENCE=ON.
#include <crd/containers/array.hpp>
#include <crd/hesap/dense/eig_nonsym.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cstdio>

extern "C" void dgehrd_(const int* n, const int* ilo, const int* ihi, double* a, const int* lda,
                        double* tau, double* work, const int* lwork, int* info);
extern "C" void dhseqr_(const char* job, const char* compz, const int* n, const int* ilo, const int* ihi,
                        double* h, const int* ldh, double* wr, double* wi, double* z, const int* ldz,
                        double* work, const int* lwork, int* info);

namespace
{
using crd::hesap::dense::hessenberg;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::real_schur;
using crd::hesap::dense::RealSchur;
using crd::hesap::dense::schur_aed;

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
            const crd::f64 el =
                std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
            if (el > budget_s && iters >= min_iters)
            {
                best = std::min(best, el / iters);
                break;
            }
        }
    }
    return best;
}

// Single warmup + one timed call — for large N where each call is seconds and
// the 9x time_loop would run for minutes (the un-accelerated references in
// particular). Low relative noise since per-call time dominates.
template <typename Op>
crd::f64 time_once(Op&& op)
{
    op();
    crd::jobs::frame_reset();
    const auto t0 = std::chrono::steady_clock::now();
    op();
    crd::jobs::frame_reset();
    return std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
}

template <typename Op>
crd::f64 timed(bool once, Op&& op)
{
    return once ? time_once(std::forward<Op>(op)) : time_loop(std::forward<Op>(op));
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1024U * 1024U * 1024U));

    std::fprintf(stdout, "\n==== Hessenberg reduction (general A, f64) ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-9s %-7s | %-9s %-7s\n", "n", "Cerid", "EigHess", "C/Eig",
                 "dgehrd", "C/lp");
    std::fprintf(stdout, "%s\n", "------------------------------------------------------------");

    // Capped at 256: OpenBLAS-generic `dgehrd` on MSVC crashes at n=512 (a
    // reference-harness fragility — Cerid's own hessenberg passes the n=512 recon
    // unit test). The win vs Eigen is decisive across 64/128/256.
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}})
    {
        const int ni = static_cast<int>(n);
        Matrix<crd::f64> a0(&alloc, n, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::containers::Array<crd::f64> acm0(&alloc);  // column-major copy for LAPACK
        acm0.resize(n * n);
        crd::u32 s = 71017U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 v = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                a0.at(i, j) = v;
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = v;
                acm0[j * n + i] = v;
            }
        }

        // --- Cerid (fresh copy each iter; hessenberg overwrites) ---
        Matrix<crd::f64> a(&alloc, n, n);
        crd::containers::Array<crd::f64> tau(&alloc);
        const crd::f64 ct = time_loop([&]() {
            for (crd::usize k = 0; k < n * n; ++k) a.data()[k] = a0.data()[k];
            hessenberg<crd::f64>(a, 0, n - 1, tau);
        });

        // --- Eigen HessenbergDecomposition ---
        const crd::f64 et = time_loop([&]() {
            Eigen::HessenbergDecomposition<Eigen::MatrixXd> h(ea);
            volatile double sink = h.matrixH()(0, 0);
            (void)sink;
        });

        // --- LAPACK dgehrd (column-major; fresh copy each iter) ---
        crd::containers::Array<crd::f64> acm(&alloc), wk(&alloc), tlp(&alloc);
        acm.resize(n * n);
        tlp.resize(n);
        int info = 0, lwork = -1;
        const int ilo = 1;
        crd::f64 wq = 0.0;
        dgehrd_(&ni, &ilo, &ni, acm0.data(), &ni, tlp.data(), &wq, &lwork, &info);
        lwork = static_cast<int>(wq);
        wk.resize(static_cast<crd::usize>(lwork));
        const crd::f64 lt = time_loop([&]() {
            for (crd::usize k = 0; k < n * n; ++k) acm.data()[k] = acm0.data()[k];
            int inf = 0;
            dgehrd_(&ni, &ilo, &ni, acm.data(), &ni, tlp.data(), wk.data(), &lwork, &inf);
        });

        std::fprintf(stdout, "%-6zu | %8.4f %8.4f %6.2fx | %8.4f %6.2fx\n", static_cast<size_t>(n),
                     ct * 1e3, et * 1e3, et / ct, lt * 1e3, lt / ct);
    }

    // ==== real Schur form: AED driver vs pure dlahqr vs Eigen vs LAPACK ====
    std::fprintf(stdout, "\n==== real Schur (from Hessenberg, f64): AED vs pure-dlahqr vs Eigen vs LAPACK ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-7s | %-9s %-7s | %-9s %-7s | %-9s %-7s | %-7s\n", "n",
                 "AED", "swp", "dlahqr", "A/dlq", "EigRS", "A/Eig", "dhseqr", "A/lp", "recon");
    std::fprintf(stdout, "%s\n",
                 "------------------------------------------------------------------------------------");
    // n>400: LAPACK dhseqr (OpenBLAS-generic on MSVC) crashes >512, so the LAPACK
    // column is dropped there — the large-N point is the AED-train vs Eigen +
    // pure-dlahqr widening (v3d-1c-4 M3 proof that the train's gain grows with N).
    for (crd::usize n : {crd::usize{200}, crd::usize{400}, crd::usize{800}, crd::usize{1200}})
    {
        const int ni = static_cast<int>(n);
        // Build a clean upper-Hessenberg H (same matrix for all four).
        Matrix<crd::f64> hbuild(&alloc, n, n);
        Eigen::MatrixXd eh(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::u32 s = 13577U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                hbuild.at(i, j) = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
            }
        }
        crd::containers::Array<crd::f64> tau(&alloc);
        hessenberg<crd::f64>(hbuild, 0, n - 1, tau);
        Matrix<crd::f64> hmat(&alloc, n, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                const crd::f64 v = (j + 1 >= i) ? hbuild.at(i, j) : 0.0;
                hmat.at(i, j) = v;
                eh(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = v;
            }
        }
        // column-major copy for LAPACK dhseqr
        crd::containers::Array<crd::f64> hcm0(&alloc);
        hcm0.resize(n * n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                hcm0[j * n + i] = hmat.at(i, j);

        // n>400: single-shot timing (each call is seconds) + drop the un-accelerated
        // pure-dlahqr reference (pathologically slow at large N and NOT a production
        // path — production uses dlahqr only below NMIN=200). The large-N point is
        // the AED-train vs Eigen widening.
        const bool large = (n > 400);
        crd::usize sweeps = 0;
        crd::f64 recon = 0.0;
        const crd::f64 t_aed = timed(large, [&]() {
            auto sc = schur_aed<crd::f64>(&alloc, hmat, 0, n - 1, true, &sweeps);
            volatile crd::f64 sink = sc.t.at(0, 0);
            (void)sink;
        });
        {
            auto sc = schur_aed<crd::f64>(&alloc, hmat, 0, n - 1, true, &sweeps);
            // O(n^3) recon: ZT = Z*T, then max|ZT*Zᵀ - H| (NOT the O(n^4) naive form).
            Matrix<crd::f64> zt(&alloc, n, n);
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize p = 0; p < n; ++p)
                {
                    crd::f64 acc = 0.0;
                    for (crd::usize q = 0; q < n; ++q)
                        acc += sc.z.at(i, q) * sc.t.at(q, p);
                    zt.at(i, p) = acc;
                }
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize j = 0; j < n; ++j)
                {
                    crd::f64 acc = 0.0;
                    for (crd::usize p = 0; p < n; ++p)
                        acc += zt.at(i, p) * sc.z.at(j, p);
                    recon = std::max(recon, std::abs(acc - hmat.at(i, j)));
                }
        }
        crd::f64 t_dlq = 0.0;
        if (!large)
        {
            t_dlq = time_loop([&]() {
                auto sc = real_schur<crd::f64>(&alloc, hmat, 0, n - 1, true);
                volatile crd::f64 sink = sc.t.at(0, 0);
                (void)sink;
            });
        }
        const crd::f64 t_eig = timed(large, [&]() {
            Eigen::RealSchur<Eigen::MatrixXd> rs;
            rs.computeFromHessenberg(eh, Eigen::MatrixXd::Identity(static_cast<Eigen::Index>(n),
                                                                   static_cast<Eigen::Index>(n)),
                                     true);
            volatile crd::f64 sink = rs.matrixT()(0, 0);
            (void)sink;
        });
        const bool run_lapack = (n <= 400);
        crd::f64 t_lp = 0.0;
        if (run_lapack)
        {
            crd::containers::Array<crd::f64> hcm(&alloc), wr(&alloc), wi(&alloc), zlp(&alloc), wk(&alloc);
            hcm.resize(n * n);
            wr.resize(n);
            wi.resize(n);
            zlp.resize(n * n);
            int info = 0, lwork = -1;
            const int one_i = 1;
            crd::f64 wq = 0.0;
            dhseqr_("S", "I", &ni, &one_i, &ni, hcm0.data(), &ni, wr.data(), wi.data(), zlp.data(), &ni,
                    &wq, &lwork, &info);
            lwork = static_cast<int>(wq);
            wk.resize(static_cast<crd::usize>(lwork > 0 ? lwork : 1));
            t_lp = time_loop([&]() {
                for (crd::usize k = 0; k < n * n; ++k) hcm[k] = hcm0[k];
                int inf = 0;
                dhseqr_("S", "I", &ni, &one_i, &ni, hcm.data(), &ni, wr.data(), wi.data(), zlp.data(),
                        &ni, wk.data(), &lwork, &inf);
            });
        }

        std::fprintf(stdout, "%-6zu | %8.4f %6zu | ", static_cast<size_t>(n), t_aed * 1e3,
                     static_cast<size_t>(sweeps));
        if (!large)
            std::fprintf(stdout, "%8.4f %6.2fx | ", t_dlq * 1e3, t_dlq / t_aed);
        else
            std::fprintf(stdout, "%8s %7s | ", "n/a", "n/a");
        std::fprintf(stdout, "%8.4f %6.2fx | ", t_eig * 1e3, t_eig / t_aed);
        if (run_lapack)
            std::fprintf(stdout, "%8.4f %6.2fx | %.1e\n", t_lp * 1e3, t_lp / t_aed, recon);
        else
            std::fprintf(stdout, "%8s %7s | %.1e\n", "n/a", "n/a", recon);
    }

    crd::jobs::shutdown();
    return 0;
}
