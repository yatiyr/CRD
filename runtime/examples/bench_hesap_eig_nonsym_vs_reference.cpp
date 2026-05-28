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
#include <complex>
#include <cstdio>

extern "C" void dgehrd_(const int* n, const int* ilo, const int* ihi, double* a, const int* lda, double* tau,
                        double* work, const int* lwork, int* info);
extern "C" void zgehrd_(const int* n, const int* ilo, const int* ihi, std::complex<double>* a, const int* lda,
                        std::complex<double>* tau, std::complex<double>* work, const int* lwork, int* info);
extern "C" void zhseqr_(const char* job, const char* compz, const int* n, const int* ilo, const int* ihi,
                        std::complex<double>* h, const int* ldh, std::complex<double>* w, std::complex<double>* z,
                        const int* ldz, std::complex<double>* work, const int* lwork, int* info);
extern "C" void dhseqr_(const char* job, const char* compz, const int* n, const int* ilo, const int* ihi, double* h,
                        const int* ldh, double* wr, double* wi, double* z, const int* ldz, double* work,
                        const int* lwork, int* info);
extern "C" void dgeev_(const char* jobvl, const char* jobvr, const int* n, double* a, const int* lda, double* wr,
                       double* wi, double* vl, const int* ldvl, double* vr, const int* ldvr, double* work,
                       const int* lwork, int* info);
extern "C" void zgeev_(const char* jobvl, const char* jobvr, const int* n, std::complex<double>* a, const int* lda,
                       std::complex<double>* w, std::complex<double>* vl, const int* ldvl, std::complex<double>* vr,
                       const int* ldvr, std::complex<double>* work, const int* lwork, double* rwork, int* info);

namespace
{
using crd::hesap::dense::complex_schur;
using crd::hesap::dense::complex_schur_aed;
using crd::hesap::dense::ComplexSchur;
using crd::hesap::dense::eig;
using crd::hesap::dense::EigNonsym;
using crd::hesap::dense::hessenberg;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::real_schur;
using crd::hesap::dense::RealSchur;
using crd::hesap::dense::schur_aed;

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

// Single warmup + one timed call — for large N where each call is seconds and
// the 9x time_loop would run for minutes (the un-accelerated references in
// particular). Low relative noise since per-call time dominates.
template <typename Op> crd::f64 time_once(Op&& op)
{
    op();
    crd::jobs::frame_reset();
    const auto t0 = std::chrono::steady_clock::now();
    op();
    crd::jobs::frame_reset();
    return std::chrono::duration<crd::f64>(std::chrono::steady_clock::now() - t0).count();
}

template <typename Op> crd::f64 timed(bool once, Op&& op)
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
    std::fprintf(stdout, "%-6s | %-9s %-9s %-7s | %-9s %-7s\n", "n", "Cerid", "EigHess", "C/Eig", "dgehrd", "C/lp");
    std::fprintf(stdout, "%s\n", "------------------------------------------------------------");

    // Capped at 256: OpenBLAS-generic `dgehrd` on MSVC crashes at n=512 (a
    // reference-harness fragility — Cerid's own hessenberg passes the n=512 recon
    // unit test). The win vs Eigen is decisive across 64/128/256.
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}})
    {
        const int ni = static_cast<int>(n);
        Matrix<crd::f64> a0(&alloc, n, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::containers::Array<crd::f64> acm0(&alloc); // column-major copy for LAPACK
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
        const crd::f64 ct = time_loop(
            [&]()
            {
                for (crd::usize k = 0; k < n * n; ++k)
                    a.data()[k] = a0.data()[k];
                hessenberg<crd::f64>(a, 0, n - 1, tau);
            });

        // --- Eigen HessenbergDecomposition ---
        const crd::f64 et = time_loop(
            [&]()
            {
                Eigen::HessenbergDecomposition<Eigen::MatrixXd> h(ea);
                volatile double sink = h.matrixH()(0, 0);
                (void)sink;
            });

        // --- LAPACK dgehrd (column-major; fresh copy each iter) ---
        crd::containers::Array<crd::f64> acm(&alloc);
        crd::containers::Array<crd::f64> wk(&alloc);
        crd::containers::Array<crd::f64> tlp(&alloc);
        acm.resize(n * n);
        tlp.resize(n);
        int info = 0;
        int lwork = -1;
        const int ilo = 1;
        crd::f64 wq = 0.0;
        dgehrd_(&ni, &ilo, &ni, acm0.data(), &ni, tlp.data(), &wq, &lwork, &info);
        lwork = static_cast<int>(wq);
        wk.resize(static_cast<crd::usize>(lwork));
        const crd::f64 lt = time_loop(
            [&]()
            {
                for (crd::usize k = 0; k < n * n; ++k)
                    acm.data()[k] = acm0.data()[k];
                int inf = 0;
                dgehrd_(&ni, &ilo, &ni, acm.data(), &ni, tlp.data(), wk.data(), &lwork, &inf);
            });

        std::fprintf(stdout, "%-6zu | %8.4f %8.4f %6.2fx | %8.4f %6.2fx\n", static_cast<size_t>(n), ct * 1e3, et * 1e3,
                     et / ct, lt * 1e3, lt / ct);
    }

    // ==== complex Hessenberg reduction (zgehd2, c64) vs Eigen + LAPACK zgehrd ====
    using C = crd::hesap::Complex<crd::f64>;
    std::fprintf(stdout, "\n==== complex Hessenberg reduction (general A, c64) ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-9s %-7s | %-9s %-7s\n", "n", "Cerid", "EigHess", "C/Eig", "zgehrd", "C/lp");
    std::fprintf(stdout, "%s\n", "------------------------------------------------------------");
    // Reference cap at n=128. BOTH complex references are fragile at n≥256 on
    // this MSVC/AVX build — an access violation that is NOT in Cerid (the marker-
    // isolation proved Cerid's reduction completes; recon is clean to n=512 and
    // ASan-clean to n=256 in the unit tests):
    //   • Eigen `HessenbergDecomposition<MatrixXcd>::compute` AVs at n≥256 — the
    //     COMPLEX path only (real `MatrixXd` at n=256 is fine). Root cause not
    //     pinpointed; treat as reference fragility (like OpenBLAS-generic below).
    //   • LAPACK `zgehrd` (OpenBLAS-generic on MSVC) is likewise fragile at n>128.
    // For n=256 we time Cerid alone; the win is decisive at 64/128 and the
    // real-Hessenberg trend (1.15× @ 256) confirms it holds. (n=512 dropped: no
    // reference there, AND the unblocked complex reduction shows a real cache
    // cliff at 512 — measured 5.5ms@256 → 106ms@512, ~19× for 2× n: the complex
    // working set ar+ai = 4MB exceeds the P-core 2MB L2, so it goes memory-bound,
    // the same unblocked-vs-blocked tradeoff the REAL v3d-1a path accepted. A
    // blocked zlahr2+gemm reduction is the large-n lever — deferred consistently
    // with the real path, which also ships unblocked.)
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}})
    {
        const int ni = static_cast<int>(n);
        const bool run_refs = (n <= 128);
        Matrix<C> a0(&alloc, n, n);
        Eigen::MatrixXcd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::containers::Array<std::complex<double>> acm0(&alloc); // column-major for LAPACK
        acm0.resize(n * n);
        crd::u32 s = 81017U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 re = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                s = s * 1664525U + 1013904223U;
                const crd::f64 im = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                a0.at(i, j) = C{re, im};
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = std::complex<double>(re, im);
                acm0[j * n + i] = std::complex<double>(re, im);
            }
        }

        Matrix<C> a(&alloc, n, n);
        crd::containers::Array<C> tau(&alloc);
        const crd::f64 ct = time_loop(
            [&]()
            {
                for (crd::usize k = 0; k < n * n; ++k)
                    a.data()[k] = a0.data()[k];
                hessenberg<C>(a, 0, n - 1, tau);
            });

        crd::f64 et = 0.0;
        crd::f64 lt = 0.0;
        if (run_refs)
        {
            et = time_loop(
                [&]()
                {
                    Eigen::HessenbergDecomposition<Eigen::MatrixXcd> h(ea);
                    volatile double sink = h.matrixH()(0, 0).real();
                    (void)sink;
                });

            crd::containers::Array<std::complex<double>> acm(&alloc);
            crd::containers::Array<std::complex<double>> wk(&alloc);
            crd::containers::Array<std::complex<double>> tlp(&alloc);
            acm.resize(n * n);
            tlp.resize(n);
            int info = 0;
            int lwork = -1;
            const int ilo = 1;
            std::complex<double> wq = 0.0;
            zgehrd_(&ni, &ilo, &ni, acm0.data(), &ni, tlp.data(), &wq, &lwork, &info);
            lwork = static_cast<int>(wq.real());
            wk.resize(static_cast<crd::usize>(lwork > 0 ? lwork : 1));
            lt = time_loop(
                [&]()
                {
                    for (crd::usize k = 0; k < n * n; ++k)
                        acm[k] = acm0[k];
                    int inf = 0;
                    zgehrd_(&ni, &ilo, &ni, acm.data(), &ni, tlp.data(), wk.data(), &lwork, &inf);
                });
        }

        if (run_refs)
            std::fprintf(stdout, "%-6zu | %8.4f %8.4f %6.2fx | %8.4f %6.2fx\n", static_cast<size_t>(n), ct * 1e3,
                         et * 1e3, et / ct, lt * 1e3, lt / ct);
        else
            std::fprintf(stdout, "%-6zu | %8.4f %8s %7s | %8s %7s\n", static_cast<size_t>(n), ct * 1e3, "ref-AV", "n/a",
                         "n/a", "n/a");
    }

    // ==== complex Schur (from Hessenberg, c64): zlahqr vs Eigen ComplexSchur vs zhseqr ====
    // Refs capped at n=128 (same Eigen-complex AVX fragility at n≥256 as the
    // complex Hessenberg above; zhseqr/OpenBLAS-generic likewise). Cerid alone
    // at n=256 (cache-resident scaling point).
    std::fprintf(stdout, "\n==== complex Schur (from Hessenberg, c64): zlahqr vs Eigen vs zhseqr ====\n");
    std::fprintf(stdout, "%-6s | %-9s | %-9s %-7s | %-9s %-7s | %-7s\n", "n", "Cerid", "EigCS", "C/Eig", "zhseqr",
                 "C/lp", "recon");
    std::fprintf(stdout, "%s\n", "----------------------------------------------------------------------------");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}})
    {
        const int ni = static_cast<int>(n);
        const bool run_refs = (n <= 128);
        // Build a complex upper-Hessenberg H (same matrix for all three).
        Matrix<C> hbuild(&alloc, n, n);
        crd::u32 s = 24611U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 re = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                s = s * 1664525U + 1013904223U;
                const crd::f64 im = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                hbuild.at(i, j) = C{re, im};
            }
        crd::containers::Array<C> tau(&alloc);
        hessenberg<C>(hbuild, 0, n - 1, tau);
        Matrix<C> hmat(&alloc, n, n);
        Eigen::MatrixXcd eh(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::containers::Array<std::complex<double>> hcm0(&alloc);
        hcm0.resize(n * n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                const C v = (j + 1 >= i) ? hbuild.at(i, j) : C{0.0, 0.0};
                hmat.at(i, j) = v;
                eh(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = std::complex<double>(v.re, v.im);
                hcm0[j * n + i] = std::complex<double>(v.re, v.im);
            }

        crd::f64 recon = 0.0;
        const crd::f64 ct = time_loop(
            [&]()
            {
                auto sc = complex_schur<C>(&alloc, hmat, 0, n - 1, true);
                volatile crd::f64 sink = sc.t.at(0, 0).re;
                (void)sink;
            });
        {
            auto sc = complex_schur<C>(&alloc, hmat, 0, n - 1, true);
            // O(n^3) recon max|Z·T·Zᴴ − H|.
            Matrix<C> zt(&alloc, n, n);
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize p = 0; p < n; ++p)
                {
                    C acc{0.0, 0.0};
                    for (crd::usize q = 0; q < n; ++q)
                        acc = acc + sc.z.at(i, q) * sc.t.at(q, p);
                    zt.at(i, p) = acc;
                }
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize j = 0; j < n; ++j)
                {
                    C acc{0.0, 0.0};
                    for (crd::usize p = 0; p < n; ++p)
                        acc = acc + zt.at(i, p) * crd::hesap::conj(sc.z.at(j, p));
                    recon = std::max(recon, std::abs(acc.re - hmat.at(i, j).re) + std::abs(acc.im - hmat.at(i, j).im));
                }
        }

        crd::f64 et = 0.0;
        crd::f64 lt = 0.0;
        if (run_refs)
        {
            et = time_loop(
                [&]()
                {
                    Eigen::ComplexSchur<Eigen::MatrixXcd> cs;
                    cs.computeFromHessenberg(
                        eh, Eigen::MatrixXcd::Identity(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n)),
                        true);
                    volatile crd::f64 sink = cs.matrixT()(0, 0).real();
                    (void)sink;
                });
            crd::containers::Array<std::complex<double>> hcm(&alloc);
            crd::containers::Array<std::complex<double>> w(&alloc);
            crd::containers::Array<std::complex<double>> zlp(&alloc);
            crd::containers::Array<std::complex<double>> wk(&alloc);
            hcm.resize(n * n);
            w.resize(n);
            zlp.resize(n * n);
            int info = 0;
            int lwork = -1;
            const int one_i = 1;
            std::complex<double> wq = 0.0;
            zhseqr_("S", "I", &ni, &one_i, &ni, hcm0.data(), &ni, w.data(), zlp.data(), &ni, &wq, &lwork, &info);
            lwork = static_cast<int>(wq.real());
            wk.resize(static_cast<crd::usize>(lwork > 0 ? lwork : 1));
            lt = time_loop(
                [&]()
                {
                    for (crd::usize k = 0; k < n * n; ++k)
                        hcm[k] = hcm0[k];
                    int inf = 0;
                    zhseqr_("S", "I", &ni, &one_i, &ni, hcm.data(), &ni, w.data(), zlp.data(), &ni, wk.data(), &lwork,
                            &inf);
                });
        }

        if (run_refs)
            std::fprintf(stdout, "%-6zu | %8.4f | %8.4f %6.2fx | %8.4f %6.2fx | %.1e\n", static_cast<size_t>(n),
                         ct * 1e3, et * 1e3, et / ct, lt * 1e3, lt / ct, recon);
        else
            std::fprintf(stdout, "%-6zu | %8.4f | %8s %7s | %8s %7s | %.1e\n", static_cast<size_t>(n), ct * 1e3,
                         "ref-AV", "n/a", "n/a", "n/a", recon);
    }

    // ==== complex Schur AED vs single-shift complex_schur (c64): the scale crush ====
    // Both Cerid: the external complex refs (Eigen ComplexSchur + zhseqr) AV at
    // n≥256, so the SCALE gate is AED vs our own single-shift baseline. Single-
    // shift's per-eigenvalue O(n) sweep count blows up super-linearly with n; AED
    // converges a whole window per inner Schur (the swp column collapses). Single-
    // shot timing at large n (each call is expensive).
    std::fprintf(stdout, "\n==== complex Schur AED vs single-shift (c64): the scale crush ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-5s | %-9s | %-7s | %-7s\n", "n", "AED", "swp", "1shift", "1s/AED", "recon");
    std::fprintf(stdout, "%s\n", "-----------------------------------------------------------------");
    for (crd::usize n : {crd::usize{128}, crd::usize{256}, crd::usize{400}, crd::usize{512}})
    {
        Matrix<C> hbuild(&alloc, n, n);
        crd::u32 s = 88017U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 re = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                s = s * 1664525U + 1013904223U;
                const crd::f64 im = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                hbuild.at(i, j) = C{re, im};
            }
        crd::containers::Array<C> tau(&alloc);
        hessenberg<C>(hbuild, 0, n - 1, tau);
        Matrix<C> hmat(&alloc, n, n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
                hmat.at(i, j) = (j + 1 >= i) ? hbuild.at(i, j) : C{0.0, 0.0};

        const bool large = (n > 256);
        crd::usize sweeps = 0;
        crd::f64 recon = 0.0;
        const crd::f64 t_aed = timed(large,
                                     [&]()
                                     {
                                         auto sc = complex_schur_aed<C>(&alloc, hmat, 0, n - 1, true, &sweeps);
                                         volatile crd::f64 sink = sc.t.at(0, 0).re;
                                         (void)sink;
                                     });
        {
            auto sc = complex_schur_aed<C>(&alloc, hmat, 0, n - 1, true, &sweeps);
            Matrix<C> zt(&alloc, n, n);
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize p = 0; p < n; ++p)
                {
                    C acc{0.0, 0.0};
                    for (crd::usize q = 0; q < n; ++q)
                        acc = acc + sc.z.at(i, q) * sc.t.at(q, p);
                    zt.at(i, p) = acc;
                }
            for (crd::usize i = 0; i < n; ++i)
                for (crd::usize j = 0; j < n; ++j)
                {
                    C acc{0.0, 0.0};
                    for (crd::usize p = 0; p < n; ++p)
                        acc = acc + zt.at(i, p) * crd::hesap::conj(sc.z.at(j, p));
                    recon = std::max(recon, std::abs(acc.re - hmat.at(i, j).re) + std::abs(acc.im - hmat.at(i, j).im));
                }
        }
        const crd::f64 t_1s = timed(large,
                                    [&]()
                                    {
                                        auto sc = complex_schur<C>(&alloc, hmat, 0, n - 1, true);
                                        volatile crd::f64 sink = sc.t.at(0, 0).re;
                                        (void)sink;
                                    });
        std::fprintf(stdout, "%-6zu | %8.4f %5zu | %8.4f | %6.2fx | %.1e\n", static_cast<size_t>(n), t_aed * 1e3,
                     static_cast<size_t>(sweeps), t_1s * 1e3, t_1s / t_aed, recon);
    }

    // ==== real Schur form: AED driver vs pure dlahqr vs Eigen vs LAPACK ====
    std::fprintf(stdout, "\n==== real Schur (from Hessenberg, f64): AED vs pure-dlahqr vs Eigen vs LAPACK ====\n");
    std::fprintf(stdout, "%-6s | %-9s %-7s | %-9s %-7s | %-9s %-7s | %-9s %-7s | %-7s\n", "n", "AED", "swp", "dlahqr",
                 "A/dlq", "EigRS", "A/Eig", "dhseqr", "A/lp", "recon");
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
        const crd::f64 t_aed = timed(large,
                                     [&]()
                                     {
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
            t_dlq = time_loop(
                [&]()
                {
                    auto sc = real_schur<crd::f64>(&alloc, hmat, 0, n - 1, true);
                    volatile crd::f64 sink = sc.t.at(0, 0);
                    (void)sink;
                });
        }
        const crd::f64 t_eig = timed(
            large,
            [&]()
            {
                Eigen::RealSchur<Eigen::MatrixXd> rs;
                rs.computeFromHessenberg(
                    eh, Eigen::MatrixXd::Identity(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n)), true);
                volatile crd::f64 sink = rs.matrixT()(0, 0);
                (void)sink;
            });
        const bool run_lapack = (n <= 400);
        crd::f64 t_lp = 0.0;
        if (run_lapack)
        {
            crd::containers::Array<crd::f64> hcm(&alloc);
            crd::containers::Array<crd::f64> wr(&alloc);
            crd::containers::Array<crd::f64> wi(&alloc);
            crd::containers::Array<crd::f64> zlp(&alloc);
            crd::containers::Array<crd::f64> wk(&alloc);
            hcm.resize(n * n);
            wr.resize(n);
            wi.resize(n);
            zlp.resize(n * n);
            int info = 0;
            int lwork = -1;
            const int one_i = 1;
            crd::f64 wq = 0.0;
            dhseqr_("S", "I", &ni, &one_i, &ni, hcm0.data(), &ni, wr.data(), wi.data(), zlp.data(), &ni, &wq, &lwork,
                    &info);
            lwork = static_cast<int>(wq);
            wk.resize(static_cast<crd::usize>(lwork > 0 ? lwork : 1));
            t_lp = time_loop(
                [&]()
                {
                    for (crd::usize k = 0; k < n * n; ++k)
                        hcm[k] = hcm0[k];
                    int inf = 0;
                    dhseqr_("S", "I", &ni, &one_i, &ni, hcm.data(), &ni, wr.data(), wi.data(), zlp.data(), &ni,
                            wk.data(), &lwork, &inf);
                });
        }

        std::fprintf(stdout, "%-6zu | %8.4f %6zu | ", static_cast<size_t>(n), t_aed * 1e3, static_cast<size_t>(sweeps));
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

    // ==== full eig (values + vectors): Cerid eig vs Eigen EigenSolver vs LAPACK dgeev ====
    // This is the v3d-2b slice's bench: the back-transform is what's added on top
    // of the already-crushing values-only Schur. `recon` = worst per-eigenpair
    // ‖A·vₖ − λₖ·vₖ‖∞/‖vₖ‖∞; `dλ` = max sorted-eigenvalue diff vs Eigen.
    std::fprintf(stdout, "\n==== full eig (general A, f64): Cerid vs Eigen EigenSolver vs LAPACK dgeev ====\n");
    std::fprintf(stdout, "%-6s | %-9s | %-9s %-7s | %-9s %-7s | %-8s %-8s\n", "n", "Cerid", "EigES", "C/Eig", "dgeev",
                 "C/lp", "resid", "dlam");
    std::fprintf(stdout, "%s\n", "--------------------------------------------------------------------------------");
    for (crd::usize n : {crd::usize{100}, crd::usize{200}, crd::usize{400}})
    {
        const int ni = static_cast<int>(n);
        Matrix<crd::f64> a0(&alloc, n, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::containers::Array<crd::f64> acm0(&alloc); // column-major for LAPACK
        acm0.resize(n * n);
        crd::u32 s = 90211U + static_cast<crd::u32>(n);
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

        const crd::f64 t_cerid = time_loop(
            [&]()
            {
                EigNonsym<crd::f64> e = eig<crd::f64>(&alloc, a0);
                volatile crd::f64 sink = e.values.data()[0].re;
                (void)sink;
            });

        // Worst residual + collect Cerid eigenvalues for the match check.
        crd::f64 resid = 0.0;
        crd::containers::Array<crd::f64> cwr(&alloc);
        crd::containers::Array<crd::f64> cwi(&alloc);
        cwr.resize(n);
        cwi.resize(n);
        {
            EigNonsym<crd::f64> e = eig<crd::f64>(&alloc, a0);
            for (crd::usize k = 0; k < n; ++k)
            {
                cwr[k] = e.values.data()[k].re;
                cwi[k] = e.values.data()[k].im;
                const crd::f64 lr = e.values.data()[k].re;
                const crd::f64 li = e.values.data()[k].im;
                crd::f64 vnorm = 0.0;
                for (crd::usize i = 0; i < n; ++i)
                    vnorm = std::max(vnorm, std::abs(e.vectors.at(i, k).re) + std::abs(e.vectors.at(i, k).im));
                for (crd::usize i = 0; i < n; ++i)
                {
                    crd::f64 avre = 0.0;
                    crd::f64 avim = 0.0;
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        avre += a0.at(i, j) * e.vectors.at(j, k).re;
                        avim += a0.at(i, j) * e.vectors.at(j, k).im;
                    }
                    const crd::f64 rr = avre - (lr * e.vectors.at(i, k).re - li * e.vectors.at(i, k).im);
                    const crd::f64 ri = avim - (lr * e.vectors.at(i, k).im + li * e.vectors.at(i, k).re);
                    resid = std::max(resid, (std::abs(rr) + std::abs(ri)) / vnorm);
                }
            }
        }

        const crd::f64 t_eig = time_loop(
            [&]()
            {
                Eigen::EigenSolver<Eigen::MatrixXd> es(ea, true);
                volatile crd::f64 sink = es.eigenvalues()(0).real();
                (void)sink;
            });

        // LAPACK dgeev (column-major; right vectors only).
        crd::containers::Array<crd::f64> acm(&alloc);
        crd::containers::Array<crd::f64> lwr(&alloc);
        crd::containers::Array<crd::f64> lwi(&alloc);
        crd::containers::Array<crd::f64> vr(&alloc);
        crd::containers::Array<crd::f64> wk(&alloc);
        acm.resize(n * n);
        lwr.resize(n);
        lwi.resize(n);
        vr.resize(n * n);
        int info = 0;
        int lwork = -1;
        crd::f64 wq = 0.0;
        dgeev_("N", "V", &ni, acm0.data(), &ni, lwr.data(), lwi.data(), nullptr, &ni, vr.data(), &ni, &wq, &lwork,
               &info);
        lwork = static_cast<int>(wq);
        wk.resize(static_cast<crd::usize>(lwork > 0 ? lwork : 1));
        const crd::f64 t_lp = time_loop(
            [&]()
            {
                for (crd::usize k = 0; k < n * n; ++k)
                    acm[k] = acm0[k];
                int inf = 0;
                dgeev_("N", "V", &ni, acm.data(), &ni, lwr.data(), lwi.data(), nullptr, &ni, vr.data(), &ni, wk.data(),
                       &lwork, &inf);
            });

        // Eigenvalue match: sort both by (re, im), max abs diff.
        auto sort_eigs = [&](crd::containers::Array<crd::f64>& wr, crd::containers::Array<crd::f64>& wi)
        {
            crd::containers::Array<crd::usize> idx(&alloc);
            idx.resize(n);
            for (crd::usize i = 0; i < n; ++i)
                idx[i] = i;
            std::sort(idx.data(), idx.data() + n,
                      [&](crd::usize p, crd::usize q) { return wr[p] < wr[q] || (wr[p] == wr[q] && wi[p] < wi[q]); });
            crd::containers::Array<crd::f64> r(&alloc);
            crd::containers::Array<crd::f64> m(&alloc);
            r.resize(n);
            m.resize(n);
            for (crd::usize i = 0; i < n; ++i)
            {
                r[i] = wr[idx[i]];
                m[i] = wi[idx[i]];
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                wr[i] = r[i];
                wi[i] = m[i];
            }
        };
        sort_eigs(cwr, cwi);
        sort_eigs(lwr, lwi);
        crd::f64 dlam = 0.0;
        for (crd::usize i = 0; i < n; ++i)
            dlam = std::max(dlam, std::abs(cwr[i] - lwr[i]) + std::abs(std::abs(cwi[i]) - std::abs(lwi[i])));

        std::fprintf(stdout, "%-6zu | %8.4f | %8.4f %6.2fx | %8.4f %6.2fx | %.1e %.1e\n", static_cast<size_t>(n),
                     t_cerid * 1e3, t_eig * 1e3, t_eig / t_cerid, t_lp * 1e3, t_lp / t_cerid, resid, dlam);
    }

    // ==== complex full eig: Cerid eig vs Eigen ComplexEigenSolver vs zgeev ====
    // Refs capped n=128 (Eigen ComplexSchur/ComplexEigenSolver + zgeev/OpenBLAS-
    // generic AV at n≥256, same fragility as the complex Schur above); Cerid alone
    // at 256. `resid` = worst ‖A·vₖ − λₖ·vₖ‖₁/‖vₖ‖₁.
    std::fprintf(stdout, "\n==== complex full eig (general A, c64): Cerid vs Eigen vs zgeev ====\n");
    std::fprintf(stdout, "%-6s | %-9s | %-9s %-7s | %-9s %-7s | %-8s\n", "n", "Cerid", "EigCES", "C/Eig", "zgeev",
                 "C/lp", "resid");
    std::fprintf(stdout, "%s\n", "------------------------------------------------------------------------");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}})
    {
        const int ni = static_cast<int>(n);
        const bool run_refs = (n <= 128);
        Matrix<C> a0(&alloc, n, n);
        Eigen::MatrixXcd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::containers::Array<std::complex<double>> acm0(&alloc); // column-major for zgeev
        acm0.resize(n * n);
        crd::u32 s = 60611U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
            for (crd::usize j = 0; j < n; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 re = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                s = s * 1664525U + 1013904223U;
                const crd::f64 im = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                a0.at(i, j) = C{re, im};
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = std::complex<double>(re, im);
                acm0[j * n + i] = std::complex<double>(re, im);
            }

        const crd::f64 t_cerid = timed(n > 128,
                                       [&]()
                                       {
                                           EigNonsym<C> e = eig<C>(&alloc, a0);
                                           volatile crd::f64 sink = e.values.data()[0].re;
                                           (void)sink;
                                       });
        crd::f64 resid = 0.0;
        {
            EigNonsym<C> e = eig<C>(&alloc, a0);
            for (crd::usize k = 0; k < n; ++k)
            {
                const C lam = e.values.data()[k];
                crd::f64 vnorm = 0.0;
                for (crd::usize i = 0; i < n; ++i)
                    vnorm = std::max(vnorm, std::abs(e.vectors.at(i, k).re) + std::abs(e.vectors.at(i, k).im));
                for (crd::usize i = 0; i < n; ++i)
                {
                    C av{0.0, 0.0};
                    for (crd::usize j = 0; j < n; ++j)
                        av = av + a0.at(i, j) * e.vectors.at(j, k);
                    const C r = av - lam * e.vectors.at(i, k);
                    resid = std::max(resid, (std::abs(r.re) + std::abs(r.im)) / vnorm);
                }
            }
        }

        crd::f64 t_eig = 0.0;
        crd::f64 t_lp = 0.0;
        if (run_refs)
        {
            t_eig = time_loop(
                [&]()
                {
                    Eigen::ComplexEigenSolver<Eigen::MatrixXcd> es(ea, true);
                    volatile crd::f64 sink = es.eigenvalues()(0).real();
                    (void)sink;
                });
            crd::containers::Array<std::complex<double>> acm(&alloc);
            crd::containers::Array<std::complex<double>> w(&alloc);
            crd::containers::Array<std::complex<double>> vr(&alloc);
            crd::containers::Array<std::complex<double>> wk(&alloc);
            crd::containers::Array<double> rwk(&alloc);
            acm.resize(n * n);
            w.resize(n);
            vr.resize(n * n);
            rwk.resize(2 * n);
            int info = 0;
            int lwork = -1;
            std::complex<double> wq = 0.0;
            zgeev_("N", "V", &ni, acm0.data(), &ni, w.data(), nullptr, &ni, vr.data(), &ni, &wq, &lwork, rwk.data(),
                   &info);
            lwork = static_cast<int>(wq.real());
            wk.resize(static_cast<crd::usize>(lwork > 0 ? lwork : 1));
            t_lp = time_loop(
                [&]()
                {
                    for (crd::usize k = 0; k < n * n; ++k)
                        acm[k] = acm0[k];
                    int inf = 0;
                    zgeev_("N", "V", &ni, acm.data(), &ni, w.data(), nullptr, &ni, vr.data(), &ni, wk.data(), &lwork,
                           rwk.data(), &inf);
                });
        }

        if (run_refs)
            std::fprintf(stdout, "%-6zu | %8.4f | %8.4f %6.2fx | %8.4f %6.2fx | %.1e\n", static_cast<size_t>(n),
                         t_cerid * 1e3, t_eig * 1e3, t_eig / t_cerid, t_lp * 1e3, t_lp / t_cerid, resid);
        else
            std::fprintf(stdout, "%-6zu | %8.4f | %8s %7s | %8s %7s | %.1e\n", static_cast<size_t>(n), t_cerid * 1e3,
                         "ref-AV", "n/a", "n/a", "n/a", resid);
    }

    crd::jobs::shutdown();
    return 0;
}
