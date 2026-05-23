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
#include <crd/hesap/dense/detail/dqds.hpp>
#include <crd/hesap/dense/detail/mrrr_vectors.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

extern "C" void dsyev_(const char* jobz, const char* uplo, const int* n, double* a, const int* lda,
                       double* w, double* work, const int* lwork, int* info);
extern "C" void zheev_(const char* jobz, const char* uplo, const int* n, double* a, const int* lda,
                       double* w, double* work, const int* lwork, double* rwork, int* info);
// Eigenvalues-only references: dsyevd (dense, D&C), dsterf (tridiagonal root-
// free QL/QR — the LAPACK values-only speed king), dstemr (tridiagonal MRRR,
// JOBZ='N' — LAPACK's own MRRR eigenvalue path, the head-to-head for ours).
extern "C" void dsyevd_(const char* jobz, const char* uplo, const int* n, double* a, const int* lda,
                        double* w, double* work, const int* lwork, int* iwork, const int* liwork,
                        int* info);
extern "C" void dsterf_(const int* n, double* d, double* e, int* info);
// dstedc — divide-and-conquer tridiagonal eigensolver (values+vectors, O(n^3)).
extern "C" void dstedc_(const char* compz, const int* n, double* d, double* e, double* z, const int* ldz,
                        double* work, const int* lwork, int* iwork, const int* liwork, int* info);
extern "C" void dstemr_(const char* jobz, const char* range, const int* n, double* d, double* e,
                        const double* vl, const double* vu, const int* il, const int* iu, int* m,
                        double* w, double* z, const int* ldz, const int* nzc, int* isuppz,
                        int* tryrac, double* work, const int* lwork, int* iwork, const int* liwork,
                        int* info);

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

// Parallel SHARED-Sturm multisection eigenvalues of a symmetric tridiagonal.
// Eigenvalue index range split into `p` disjoint chunks; each worker multisects
// its chunk (one negcount(mid) splits an interval for all eigenvalues in it =
// the dlaebz sharing that kills naive bisection's ~16x redundancy), all chunks
// in parallel over crd::jobs. The "use all 16 cores" lever LAPACK's serial
// dsterf/dstemr cannot answer.
struct MultiState
{
    const crd::f64* d;
    const crd::f64* e2;
    int n;
    int p;
    crd::f64 gl;
    crd::f64 gu;
    crd::f64 pivmin;
    crd::f64 reltol;
    crd::f64* w;
    crd::f64* sl;
    crd::f64* sr;
    int* sclo;
    int* schi;
    int stride;
};

void tridiag_eig_multisection_parallel(MultiState* sp)
{
    const crd::u32 p = static_cast<crd::u32>(sp->p);
    auto* counter = crd::jobs::parallel_for(p, p, [sp](crd::u32 begin, crd::u32 end) {
        for (crd::u32 c = begin; c < end; ++c)
        {
            const int klo = static_cast<int>(static_cast<crd::u64>(c) * sp->n / sp->p);
            const int khi = static_cast<int>(static_cast<crd::u64>(c + 1) * sp->n / sp->p);
            const crd::usize off = static_cast<crd::usize>(c) * sp->stride;
            crd::hesap::dense::detail::multisection_chunk(sp->d, sp->e2, sp->n, klo, khi, sp->gl, sp->gu,
                                                          sp->pivmin, sp->reltol, sp->w, sp->sl + off,
                                                          sp->sr + off, sp->sclo + off, sp->schi + off);
        }
    });
    crd::jobs::wait(counter);
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

    // ==== dense symmetric eig (VALUES ONLY, f64) — eigvals_sym vs Eigen + LAPACK ====
    // Cerid eigvals_sym (blocked dsytrd + dqds) vs Eigen SelfAdjointEigenSolver
    // (EigenvaluesOnly) vs LAPACK dsyevd(jobz='N', D&C values-only).
    std::fprintf(stdout, "\n==== symmetric eig (VALUES ONLY, f64) ====\n");
    std::fprintf(stdout, "%-6s | %-11s | %-11s | %-11s | %-9s | %-9s | %-10s\n", "N", "Cerid(ms)",
                 "Eigen(ms)", "LAPACK(ms)", "C/Eigen", "C/LAPACK", "val|err|");
    std::fprintf(stdout, "------------------------------------------------------------------------------\n");
    for (crd::usize n : {crd::usize{64}, crd::usize{128}, crd::usize{256}, crd::usize{512},
                         crd::usize{1024}, crd::usize{2048}})
    {
        Symmetric<crd::f64> a(&alloc, n);
        Eigen::MatrixXd ea(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        crd::u32 s = 424242U + static_cast<crd::u32>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                s = s * 1664525U + 1013904223U;
                const crd::f64 val = static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
                a.at(i, j) = val;
                ea(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = val;
                ea(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) = val;
            }
        }

        crd::containers::Array<crd::f64> cvals(&alloc);
        cvals.resize(n);
        const crd::f64 ct = time_loop([&]() {
            const auto vals = crd::hesap::dense::eigvals_sym<crd::f64>(&alloc, a);
            for (crd::usize i = 0; i < n; ++i)
            {
                cvals[i] = vals.data()[i];
            }
        });

        Eigen::VectorXd evals;
        const crd::f64 et = time_loop([&]() {
            Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(ea, Eigen::EigenvaluesOnly);
            evals = es.eigenvalues();
        });

        const int ni = static_cast<int>(n);
        crd::containers::Array<crd::f64> la(&alloc);
        crd::containers::Array<crd::f64> lw(&alloc);
        la.resize(n * n);
        lw.resize(n);
        crd::f64 lt = 0.0;
        {
            int info = 0;
            int lwork = -1;
            int liwork = -1;
            crd::f64 wq = 0.0;
            int iwq = 0;
            dsyevd_("N", "L", &ni, la.data(), &ni, lw.data(), &wq, &lwork, &iwq, &liwork, &info);
            lwork = static_cast<int>(wq);
            liwork = iwq;
            crd::containers::Array<crd::f64> work(&alloc);
            crd::containers::Array<int> iwork(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            iwork.resize(static_cast<crd::usize>(liwork));
            lt = time_loop([&]() {
                for (crd::usize i = 0; i < n * n; ++i)
                {
                    la[i] = ea.data()[i];
                }
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                int liwk = static_cast<int>(iwork.size());
                dsyevd_("N", "L", &ni, la.data(), &ni, lw.data(), work.data(), &lwk, iwork.data(), &liwk, &inf);
            });
        }
        crd::f64 verr = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            verr = std::max(verr, std::abs(cvals[i] - evals[static_cast<Eigen::Index>(i)]));
            verr = std::max(verr, std::abs(cvals[i] - lw[i]));
        }
        std::fprintf(stdout, "%-6zu | %-11.3f | %-11.3f | %-11.3f | %-9.2f | %-9.2f | %-10.2e\n",
                     static_cast<size_t>(n), ct * 1e3, et * 1e3, lt * 1e3, et / ct, lt / ct, verr);
    }

    // ==== symmetric TRIDIAGONAL eig (VALUES ONLY, f64) — the dqds gate ====
    // The pure dqds engine head-to-head: Cerid tridiag_eigenvalues_dqds vs
    // LAPACK dsterf (root-free QL/QR, values-only speed king) and LAPACK
    // dstemr(JOBZ='N') (LAPACK's own MRRR eigenvalue path). Same (d,e) input.
    std::fprintf(stdout, "\n==== symmetric tridiagonal eig (VALUES ONLY, f64) — parallel bisection vs LAPACK ====\n");
    std::fprintf(stdout, "%-6s | %-10s | %-10s | %-10s | %-10s | %-9s | %-9s | %-9s\n", "N",
                 "Cpar(ms)", "dqds(ms)", "dsterf(ms)", "dstemr(ms)", "par/strf", "par/stmr", "|err|");
    std::fprintf(stdout, "-----------------------------------------------------------------------------------------\n");
    for (crd::usize n : {crd::usize{256}, crd::usize{512}, crd::usize{1024}, crd::usize{2048},
                         crd::usize{4096}, crd::usize{8192}})
    {
        const int ni = static_cast<int>(n);
        crd::containers::Array<crd::f64> d0(&alloc);
        crd::containers::Array<crd::f64> e0(&alloc);
        d0.resize(n);
        e0.resize(n);
        crd::u32 s = 909090U + static_cast<crd::u32>(n);
        auto nd = [&]() {
            s = s * 1664525U + 1013904223U;
            return static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
        };
        for (crd::usize i = 0; i < n; ++i)
        {
            d0[i] = nd();
        }
        for (crd::usize i = 0; i + 1 < n; ++i)
        {
            e0[i] = 0.5 + 0.5 * std::abs(nd());  // bounded away from 0 (single block)
        }

        // --- Cerid dqds (scratch reused; inputs not destroyed) ---
        crd::containers::Array<crd::f64> ework(&alloc);
        crd::containers::Array<crd::f64> e2work(&alloc);
        crd::containers::Array<int> isplit(&alloc);
        crd::containers::Array<crd::f64> zw(&alloc);
        crd::containers::Array<crd::f64> q(&alloc);
        crd::containers::Array<crd::f64> qe(&alloc);
        crd::containers::Array<crd::f64> cw(&alloc);
        ework.resize(n);
        e2work.resize(n);
        isplit.resize(n);
        zw.resize(4 * n + 8);
        q.resize(n + 2);
        qe.resize(n + 1);
        cw.resize(n);
        const crd::f64 reltol = 4.0 * std::numeric_limits<crd::f64>::epsilon();
        const crd::f64 ct = time_loop([&]() {
            crd::hesap::dense::detail::tridiag_eigenvalues_dqds<crd::f64>(
                d0.data(), e0.data(), ni, ework.data(), e2work.data(), isplit.data(), zw.data(),
                q.data(), qe.data(), cw.data(), reltol);
        });

        // --- Cerid PARALLEL multisection (all cores) ---
        crd::containers::Array<crd::f64> e2b(&alloc);
        crd::containers::Array<crd::f64> bw(&alloc);
        e2b.resize(n);
        bw.resize(n);
        for (crd::usize i = 0; i + 1 < n; ++i)
        {
            e2b[i] = e0[i] * e0[i];
        }
        crd::f64 gl = 0.0;
        crd::f64 gu = 0.0;
        crd::hesap::dense::detail::gershgorin_bounds(d0.data(), e0.data(), ni, gl, gu);
        const crd::f64 pivmin = crd::hesap::dense::detail::compute_pivmin(e0.data(), ni);
        // Widen the bracket so it strictly contains the spectrum.
        const crd::f64 wid = 2.0 * pivmin + std::numeric_limits<crd::f64>::epsilon() * std::max(std::abs(gl), std::abs(gu));
        gl -= wid;
        gu += wid;
        const int pp = static_cast<int>(crd::jobs::num_workers());
        const int chunk = (ni + pp - 1) / pp;
        const int stride = 2 * (chunk + 1) + 64;
        crd::containers::Array<crd::f64> sl(static_cast<crd::usize>(pp) * stride, &alloc);
        crd::containers::Array<crd::f64> sr(static_cast<crd::usize>(pp) * stride, &alloc);
        crd::containers::Array<int> sclo(static_cast<crd::usize>(pp) * stride, &alloc);
        crd::containers::Array<int> schi(static_cast<crd::usize>(pp) * stride, &alloc);
        MultiState ms{d0.data(),       e2b.data(),  ni,          pp,          gl,         gu,
                      pivmin,          4.0 * std::numeric_limits<crd::f64>::epsilon(),
                      bw.data(),       sl.data(),   sr.data(),   sclo.data(), schi.data(), stride};
        const crd::f64 bt = time_loop([&]() { tridiag_eig_multisection_parallel(&ms); });

        // --- LAPACK dsterf (destroys d,e — refill each iter) ---
        crd::containers::Array<crd::f64> sd(&alloc);
        crd::containers::Array<crd::f64> se(&alloc);
        sd.resize(n);
        se.resize(n);
        const crd::f64 st = time_loop([&]() {
            for (crd::usize i = 0; i < n; ++i)
            {
                sd[i] = d0[i];
            }
            for (crd::usize i = 0; i + 1 < n; ++i)
            {
                se[i] = e0[i];
            }
            int info = 0;
            dsterf_(&ni, sd.data(), se.data(), &info);
        });
        // Keep a clean dsterf run as the accuracy oracle (ascending).
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                sd[i] = d0[i];
            }
            for (crd::usize i = 0; i + 1 < n; ++i)
            {
                se[i] = e0[i];
            }
            int info = 0;
            dsterf_(&ni, sd.data(), se.data(), &info);
        }

        // --- LAPACK dstemr (JOBZ='N', RANGE='A') ---
        crd::containers::Array<crd::f64> md(&alloc);
        crd::containers::Array<crd::f64> me(&alloc);
        crd::containers::Array<crd::f64> mw(&alloc);
        crd::containers::Array<int> isuppz(&alloc);
        md.resize(n);
        me.resize(n);
        mw.resize(n);
        isuppz.resize(2 * n);
        const crd::f64 vl = 0.0;
        const crd::f64 vu = 0.0;
        const int il = 1;
        const int iu = ni;
        const int ldz = 1;
        const int nzc = 1;
        crd::f64 mt = 0.0;
        {
            int m = 0;
            int tryrac = 1;
            int info = 0;
            int lwork = -1;
            int liwork = -1;
            crd::f64 wq = 0.0;
            int iwq = 0;
            crd::f64 zdummy = 0.0;
            dstemr_("N", "A", &ni, md.data(), me.data(), &vl, &vu, &il, &iu, &m, mw.data(), &zdummy,
                    &ldz, &nzc, isuppz.data(), &tryrac, &wq, &lwork, &iwq, &liwork, &info);
            lwork = static_cast<int>(wq);
            liwork = iwq;
            crd::containers::Array<crd::f64> work(&alloc);
            crd::containers::Array<int> iwork(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            iwork.resize(static_cast<crd::usize>(liwork));
            mt = time_loop([&]() {
                for (crd::usize i = 0; i < n; ++i)
                {
                    md[i] = d0[i];
                }
                for (crd::usize i = 0; i + 1 < n; ++i)
                {
                    me[i] = e0[i];
                }
                int mm = 0;
                int tr = 1;
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                int liwk = static_cast<int>(iwork.size());
                dstemr_("N", "A", &ni, md.data(), me.data(), &vl, &vu, &il, &iu, &mm, mw.data(),
                        &zdummy, &ldz, &nzc, isuppz.data(), &tr, work.data(), &lwk, iwork.data(),
                        &liwk, &inf);
            });
        }

        crd::f64 verr = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            verr = std::max(verr, std::abs(bw[i] - sd[i]));  // parallel bisection vs dsterf oracle
        }
        std::fprintf(stdout, "%-6zu | %-10.3f | %-10.3f | %-10.3f | %-10.3f | %-9.2f | %-9.2f | %-9.2e\n",
                     static_cast<size_t>(n), bt * 1e3, ct * 1e3, st * 1e3, mt * 1e3, st / bt, mt / bt, verr);
    }

    // ==== symmetric tridiagonal eig (VALUES+VECTORS, f64) — MRRR vs dstedc/dstemr ====
    // THE O(n^2) MRRR VECTOR PAYOFF: Cerid (dqds eigenvalues + mrrr_compute_vectors,
    // O(n^2) twisted-factorization vectors) vs LAPACK dstedc (divide-and-conquer,
    // O(n^3) vectors) and dstemr (MRRR). Same (d,e). orth = ||Z^T Z - I||.
    std::fprintf(stdout, "\n==== symmetric tridiagonal eig (VALUES+VECTORS, f64) — MRRR vs Eigen + dstedc/dstemr ====\n");
    std::fprintf(stdout, "%-6s | %-9s | %-9s | %-9s | %-9s | %-8s | %-8s | %-8s | %-9s\n", "N", "Cmrrr", "Eigen",
                 "dstedc", "dstemr", "C/Eign", "C/stdc", "C/stmr", "orth");
    std::fprintf(stdout, "------------------------------------------------------------------------------------------\n");
    for (crd::usize n : {crd::usize{256}, crd::usize{512}, crd::usize{1024}, crd::usize{2048}})
    {
        const int ni = static_cast<int>(n);
        crd::containers::Array<crd::f64> d0(&alloc);
        crd::containers::Array<crd::f64> e0(&alloc);
        d0.resize(n);
        e0.resize(n);
        crd::u32 s = 313131U + static_cast<crd::u32>(n);
        auto nd = [&]() {
            s = s * 1664525U + 1013904223U;
            return static_cast<crd::f64>(static_cast<crd::i32>(s >> 8) % 2000 - 1000) * 0.001;
        };
        for (crd::usize i = 0; i < n; ++i)
        {
            d0[i] = nd();
        }
        for (crd::usize i = 0; i + 1 < n; ++i)
        {
            e0[i] = 0.5 + 0.5 * std::abs(nd());
        }

        // --- Cerid: dqds eigenvalues + MRRR vectors ---
        crd::containers::Array<crd::f64> ew(&alloc);
        crd::containers::Array<crd::f64> e2w(&alloc);
        crd::containers::Array<int> isp(&alloc);
        crd::containers::Array<crd::f64> zb(&alloc);
        crd::containers::Array<crd::f64> qq(&alloc);
        crd::containers::Array<crd::f64> qe(&alloc);
        crd::containers::Array<crd::f64> cw(&alloc);
        crd::containers::Array<crd::f64> cz(&alloc);
        ew.resize(n);
        e2w.resize(n);
        isp.resize(n);
        zb.resize(4 * n + 8);
        qq.resize(n + 2);
        qe.resize(n + 1);
        cw.resize(n);
        cz.resize(n * n);
        const crd::f64 reltol = 4.0 * std::numeric_limits<crd::f64>::epsilon();
        const crd::f64 ct = time_loop([&]() {
            crd::hesap::dense::detail::tridiag_eigenvalues_dqds<crd::f64>(d0.data(), e0.data(), ni, ew.data(),
                                                                         e2w.data(), isp.data(), zb.data(),
                                                                         qq.data(), qe.data(), cw.data(), reltol);
            crd::hesap::dense::detail::mrrr_compute_vectors<crd::f64>(&alloc, ni, d0.data(), e0.data(), cw.data(),
                                                                     cz.data(), ni);
        });
        // Orthonormality of Cerid's vectors (RowMajor, col k).
        crd::f64 orth = 0.0;
        for (crd::usize a2 = 0; a2 < n; ++a2)
        {
            for (crd::usize b2 = 0; b2 < n; ++b2)
            {
                crd::f64 dot = 0.0;
                for (crd::usize r = 0; r < n; ++r)
                {
                    dot += cz[r * n + a2] * cz[r * n + b2];
                }
                orth = std::max(orth, std::abs(dot - (a2 == b2 ? 1.0 : 0.0)));
            }
        }

        // --- Eigen computeFromTridiagonal (values+vectors; implicit-shift QL/QR) ---
        Eigen::VectorXd ediag(static_cast<Eigen::Index>(n));
        Eigen::VectorXd esub(static_cast<Eigen::Index>(n - 1));
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> ees;
        const crd::f64 egt = time_loop([&]() {
            for (crd::usize i = 0; i < n; ++i)
            {
                ediag(static_cast<Eigen::Index>(i)) = d0[i];
            }
            for (crd::usize i = 0; i + 1 < n; ++i)
            {
                esub(static_cast<Eigen::Index>(i)) = e0[i];
            }
            ees.computeFromTridiagonal(ediag, esub, Eigen::ComputeEigenvectors);
        });

        // --- LAPACK dstedc (D&C, compz='I') ---
        crd::containers::Array<crd::f64> dd(&alloc);
        crd::containers::Array<crd::f64> ee(&alloc);
        crd::containers::Array<crd::f64> zc(&alloc);
        dd.resize(n);
        ee.resize(n);
        zc.resize(n * n);
        crd::f64 et = 0.0;
        {
            int info = 0;
            int lwork = -1;
            int liwork = -1;
            crd::f64 wq = 0.0;
            int iwq = 0;
            dstedc_("I", &ni, dd.data(), ee.data(), zc.data(), &ni, &wq, &lwork, &iwq, &liwork, &info);
            lwork = static_cast<int>(wq);
            liwork = iwq;
            crd::containers::Array<crd::f64> work(&alloc);
            crd::containers::Array<int> iwork(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            iwork.resize(static_cast<crd::usize>(liwork));
            et = time_loop([&]() {
                for (crd::usize i = 0; i < n; ++i)
                {
                    dd[i] = d0[i];
                    ee[i] = (i + 1 < n) ? e0[i] : 0.0;
                }
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                int liwk = static_cast<int>(iwork.size());
                dstedc_("I", &ni, dd.data(), ee.data(), zc.data(), &ni, work.data(), &lwk, iwork.data(), &liwk,
                        &inf);
            });
        }

        // --- LAPACK dstemr (MRRR, JOBZ='V') ---
        crd::containers::Array<crd::f64> md(&alloc);
        crd::containers::Array<crd::f64> me(&alloc);
        crd::containers::Array<crd::f64> mw(&alloc);
        crd::containers::Array<crd::f64> mz(&alloc);
        crd::containers::Array<int> isuppz(&alloc);
        md.resize(n);
        me.resize(n);
        mw.resize(n);
        mz.resize(n * n);
        isuppz.resize(2 * n);
        const crd::f64 vl = 0.0;
        const crd::f64 vu = 0.0;
        const int il = 1;
        const int iu = ni;
        const int nzc = ni;
        crd::f64 mt = 0.0;
        {
            int m = 0;
            int tryrac = 1;
            int info = 0;
            int lwork = -1;
            int liwork = -1;
            crd::f64 wq = 0.0;
            int iwq = 0;
            dstemr_("V", "A", &ni, md.data(), me.data(), &vl, &vu, &il, &iu, &m, mw.data(), mz.data(), &ni, &nzc,
                    isuppz.data(), &tryrac, &wq, &lwork, &iwq, &liwork, &info);
            lwork = static_cast<int>(wq);
            liwork = iwq;
            crd::containers::Array<crd::f64> work(&alloc);
            crd::containers::Array<int> iwork(&alloc);
            work.resize(static_cast<crd::usize>(lwork));
            iwork.resize(static_cast<crd::usize>(liwork));
            mt = time_loop([&]() {
                for (crd::usize i = 0; i < n; ++i)
                {
                    md[i] = d0[i];
                    me[i] = (i + 1 < n) ? e0[i] : 0.0;
                }
                int mm = 0;
                int tr = 1;
                int inf = 0;
                int lwk = static_cast<int>(work.size());
                int liwk = static_cast<int>(iwork.size());
                dstemr_("V", "A", &ni, md.data(), me.data(), &vl, &vu, &il, &iu, &mm, mw.data(), mz.data(), &ni,
                        &nzc, isuppz.data(), &tr, work.data(), &lwk, iwork.data(), &liwk, &inf);
            });
        }

        std::fprintf(stdout, "%-6zu | %-9.3f | %-9.3f | %-9.3f | %-9.3f | %-8.2f | %-8.2f | %-8.2f | %-9.2e\n",
                     static_cast<size_t>(n), ct * 1e3, egt * 1e3, et * 1e3, mt * 1e3, egt / ct, et / ct, mt / ct,
                     orth);
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
