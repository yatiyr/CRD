// bench_hesap_ldlt_vs_reference — Cerid multifrontal LDLᵀ (Bunch-Kaufman) vs Eigen SimplicialLDLT.
//
// FAIR PROTOCOL (mirrors the cholesky/QR benches): AMD-order the matrix ONCE, then feed BOTH factorizations
// the SAME permuted matrix — Cerid factors it as-given (v5d has no internal AMD; the consumer applies the
// fill order), and Eigen uses SimplicialLDLT<…, NaturalOrdering> so it does NOT reorder again. This isolates
// the KERNEL (same fill), not the ordering.
//
// HONEST PEER FRAMING (v5d-g): Eigen's SimplicialLDLT is PIVOT-FREE (no 2×2 blocks). It is NOT the same-class
// peer for indefinite — the fair same-class indefinite peer is MA57 / MUMPS-LDLᵀ / PARDISO (all do 2×2
// Bunch-Kaufman pivoting), deferred to the v5z parallel-peer sweep. Two honest findings, reported as the
// numbers show (a loss reported in the same plain ratio as a win):
//   (1) DEFINITE (SPD) + fill-regularized indefinite (saddle-points): BOTH correct. Eigen's reorder+fill
//       regularizes most practical indefinite systems before a zero pivot is reached, so it succeeds there —
//       and Eigen Simplicial* is a mature tuned scalar code, so expect Cerid (correctness-first multifrontal,
//       no perf pass, + the BK pivot search Eigen skips) to be SLOWER. That speed gap is characterized here,
//       NOT dressed as a win; the fair speed peer is MA57/MUMPS.
//   (2) The pivot-free FAILURE MODE is real but narrow: a persistently-zero pivot (e.g. [[0,1],[1,0]]) that
//       no reorder+fill rescues. Eigen returns NumericalIssue/NaN there; Cerid's BK 2×2 is correct. One
//       canonical receipt below — not a contrived corpus.
//
// Cerid's genuine v5d differentiators: correct on ALL indefinite (incl. persistently-zero pivots), the only
// cross-thread bit-deterministic LDLᵀ anywhere, and a complete family (real + complex sym/Herm).
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. crd conventions throughout; raw double only at the Eigen
// API boundary.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas3.hpp>                  // dense::gemm + MatrixView (blocked-front lever probe)
#include <crd/hesap/direct/dense_ldlt_kernels.hpp>    // factor_front_ldlt (the unblocked front kernel)
#include <crd/hesap/direct/multifrontal_ldlt.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

#ifdef CRD_HESAP_VS_MUMPS
#include <dmumps_c.h> // the REAL same-class indefinite gold standard (SYM=2 ⇒ 2×2 Bunch-Kaufman pivoting)
// LAPACK (linked transitively via MUMPS-seq's BLAS/LAPACK dep) — the per-front kernel ceiling: dpotrf =
// Cholesky (absolute BLAS-3 ceiling, no pivots), dsytrf = the same-algorithm BK LDLᵀ peer MUMPS runs per front.
extern "C"
{
    void dpotrf_(const char* uplo, const int* n, double* a, const int* lda, int* info);
    void dsytrf_(const char* uplo, const int* n, double* a, const int* lda, int* ipiv, double* work,
                 const int* lwork, int* info);
}
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;
namespace ord = crd::hesap::ordering;
namespace dir = crd::hesap::direct;

crd::memory::GrowableTlsfAllocator g_alloc;

struct Trip
{
    crd::u32 r, c;
    crd::f64 v;
};

void make_spd_laplacian(crd::u32 k, crd::f64 shift, crd::u32& n, crd::containers::Array<Trip>& t)
{
    n = k * k;
    auto id = [k](crd::u32 i, crd::u32 j) { return i * k + j; };
    for (crd::u32 i = 0; i < k; ++i)
    {
        for (crd::u32 j = 0; j < k; ++j)
        {
            const crd::u32 d = id(i, j);
            t.push_back({d, d, 4.0 + shift});
            if (i + 1 < k)
            {
                t.push_back({d, id(i + 1, j), -1.0});
                t.push_back({id(i + 1, j), d, -1.0});
            }
            if (j + 1 < k)
            {
                t.push_back({d, id(i, j + 1), -1.0});
                t.push_back({id(i, j + 1), d, -1.0});
            }
        }
    }
}

// 3D 7-point Laplacian on a k×k×k grid (SPD: diag 6, 6 neighbors −1). Separators ~ n^{2/3} ⇒ BIG dense
// fronts — the regime where multifrontal+dense-front amortization crushes scalar simplicial (the v5a 3D-FEM
// regime, hood/ldoor).
void make_spd_laplacian_3d(crd::u32 k, crd::u32& n, crd::containers::Array<Trip>& t)
{
    n = k * k * k;
    auto id = [k](crd::u32 i, crd::u32 j, crd::u32 l) { return (i * k + j) * k + l; };
    for (crd::u32 i = 0; i < k; ++i)
    {
        for (crd::u32 j = 0; j < k; ++j)
        {
            for (crd::u32 l = 0; l < k; ++l)
            {
                const crd::u32 d = id(i, j, l);
                t.push_back({d, d, 6.0});
                if (i + 1 < k) { t.push_back({d, id(i + 1, j, l), -1.0}); t.push_back({id(i + 1, j, l), d, -1.0}); }
                if (j + 1 < k) { t.push_back({d, id(i, j + 1, l), -1.0}); t.push_back({id(i, j + 1, l), d, -1.0}); }
                if (l + 1 < k) { t.push_back({d, id(i, j, l + 1), -1.0}); t.push_back({id(i, j, l + 1), d, -1.0}); }
            }
        }
    }
}

// 3D shifted Laplacian A − σ·I (Helmholtz-like): SAME big dense fronts as the SPD 3D, but σ>0 pushes the
// spectrum negative ⇒ genuinely INDEFINITE on the BIG fronts. This is LDLᵀ's actual domain (SPD ⇒ Cholesky)
// and the case that exercises 2×2 Bunch-Kaufman pivots inside large dense fronts (where the blocked path bails).
void make_indefinite_laplacian_3d(crd::u32 k, double sigma, crd::u32& n, crd::containers::Array<Trip>& t)
{
    n = k * k * k;
    auto id = [k](crd::u32 i, crd::u32 j, crd::u32 l) { return (i * k + j) * k + l; };
    for (crd::u32 i = 0; i < k; ++i)
    {
        for (crd::u32 j = 0; j < k; ++j)
        {
            for (crd::u32 l = 0; l < k; ++l)
            {
                const crd::u32 d = id(i, j, l);
                t.push_back({d, d, 6.0 - sigma}); // diagonal shifted negative-ward ⇒ indefinite
                if (i + 1 < k) { t.push_back({d, id(i + 1, j, l), -1.0}); t.push_back({id(i + 1, j, l), d, -1.0}); }
                if (j + 1 < k) { t.push_back({d, id(i, j + 1, l), -1.0}); t.push_back({id(i, j + 1, l), d, -1.0}); }
                if (l + 1 < k) { t.push_back({d, id(i, j, l + 1), -1.0}); t.push_back({id(i, j, l + 1), d, -1.0}); }
            }
        }
    }
}

// KKT saddle-point [[H, Bᵀ],[B, 0]]: H = m×m SPD tridiagonal, (2,2) block = 0 ⇒ genuinely INDEFINITE.
void make_kkt_saddle(crd::u32 m, crd::u32 p, crd::u32& n, crd::containers::Array<Trip>& t)
{
    n = m + p;
    for (crd::u32 i = 0; i < m; ++i)
    {
        t.push_back({i, i, 2.0});
        if (i + 1 < m)
        {
            t.push_back({i, i + 1, -1.0});
            t.push_back({i + 1, i, -1.0});
        }
    }
    for (crd::u32 c = 0; c < p; ++c)
    {
        const crd::u32 row = m + c;
        const crd::u32 a = (2 * c) % m;
        const crd::u32 b = (2 * c + 1) % m;
        t.push_back({row, a, 1.0});
        t.push_back({a, row, 1.0});
        t.push_back({row, b, -1.0});
        t.push_back({b, row, -1.0});
    }
}

// [[0,1],[1,0]] — the canonical persistently-zero-pivot matrix; pivot-free LDLᵀ cannot factor it (no reorder
// + fill rescues a 2-variable both-zero-diagonal block), Bunch-Kaufman uses one 2×2.
void make_zero_diag_2x2(crd::u32& n, crd::containers::Array<Trip>& t)
{
    n = 2;
    t.push_back({0, 1, 1.0});
    t.push_back({1, 0, 1.0});
}

sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> to_cerid(crd::u32 n, const crd::containers::Array<Trip>& t)
{
    sp::TripletBuilder<crd::f64> tb(&g_alloc, n, n);
    tb.reserve(t.size());
    for (const auto& e : t)
    {
        tb.add(e.r, e.c, e.v);
    }
    return sp::to_csc<crd::f64>(tb.compress(), &g_alloc);
}

crd::f64 residual_inf(crd::u32 n, const crd::containers::Array<Trip>& t, const crd::containers::Array<crd::f64>& x,
                      const crd::containers::Array<crd::f64>& b)
{
    crd::containers::Array<crd::f64> y(&g_alloc);
    y.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        y[i] = 0.0;
    }
    for (const auto& e : t)
    {
        y[e.r] += e.v * x[e.c];
    }
    crd::f64 r = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::f64 d = std::abs(y[i] - b[i]);
        r = d > r ? d : r;
    }
    return r;
}

// Blocked-1×1 LDLᵀ on a DENSE col-major SPD front (the lever probe): panel the pivots, then ONE BLAS-3
// `dense::gemm` symmetric trailing update per panel. All-1×1 (SPD ⇒ no pivoting/2×2-straddle) — this is the
// cheap validation of whether blocked-BLAS-3 closes the MUMPS gap, NOT the production indefinite path.
void blocked_ldl_1x1(double* d, crd::u32 N, crd::u32 nb, crd::memory::IAllocator* scr)
{
    namespace dl = crd::hesap::dense;
    const crd::usize ld = N;
    crd::containers::Array<double> w(scr); // W = D·L21ᵀ panel scratch (tr × nb, col-major)
    for (crd::u32 p0 = 0; p0 < N; p0 += nb)
    {
        const crd::u32 p1 = (p0 + nb < N) ? p0 + nb : N;
        // Panel factor: 1×1 pivots, rank-1 updates restricted to the PANEL columns + the foot; trailing
        // [p1:,p1:] deferred to the gemm.
        for (crd::u32 k = p0; k < p1; ++k)
        {
            const double dk = d[static_cast<crd::usize>(k) * ld + k];
            const double invd = 1.0 / dk;
            for (crd::u32 j = k + 1; j < p1; ++j)
            {
                const double s = d[static_cast<crd::usize>(k) * ld + j] * invd;
                for (crd::u32 r = j; r < N; ++r)
                {
                    d[static_cast<crd::usize>(j) * ld + r] -= d[static_cast<crd::usize>(k) * ld + r] * s;
                }
            }
            for (crd::u32 r = k + 1; r < N; ++r)
            {
                d[static_cast<crd::usize>(k) * ld + r] *= invd; // normalize column k → L21
            }
        }
        const crd::u32 tr = N - p1;
        const crd::u32 bw = p1 - p0;
        if (tr == 0)
        {
            continue;
        }
        // W[i,k'] = L21[i,k']·D[k']  (tr × bw, col-major).
        w.resize(static_cast<crd::usize>(tr) * bw);
        for (crd::u32 kk = 0; kk < bw; ++kk)
        {
            const double dkk = d[static_cast<crd::usize>(p0 + kk) * ld + (p0 + kk)];
            for (crd::u32 i = 0; i < tr; ++i)
            {
                w[static_cast<crd::usize>(kk) * tr + i] = d[static_cast<crd::usize>(p0 + kk) * ld + (p1 + i)] * dkk;
            }
        }
        // trailing[p1:,p1:] -= L21 · Wᵀ  (symmetric; full gemm writes both triangles identically).
        const dl::MatrixView<const double, dl::Layout::ColMajor> l21(&d[static_cast<crd::usize>(p0) * ld + p1], tr, bw,
                                                                    N);
        const dl::MatrixView<const double, dl::Layout::ColMajor> wv(w.data(), tr, bw, tr);
        dl::MatrixView<double, dl::Layout::ColMajor> c22(&d[static_cast<crd::usize>(p1) * ld + p1], tr, tr, N);
        dl::gemm<double, dl::Layout::ColMajor>(-1.0, l21, wv, 1.0, c22, dl::Trans::None, dl::Trans::Transpose, scr);
    }
}

// LEVER PROBE: factor a dense N×N SPD front (the n=64k 3D root front is ~1300 wide) with the UNBLOCKED kernel
// vs the blocked-1×1 prototype. Same matrix, verified identical L; reports GFLOP/s + speedup. If blocked is
// dramatically faster, blocked-BLAS-3 closes the MUMPS 3D gap ⇒ full xLASYF is the justified build.
void front_lever_probe()
{
    namespace dir = crd::hesap::direct;
    std::printf("\n-- LEVER PROBE: dense SPD front, UNBLOCKED rank-1 vs blocked-1×1 BLAS-3 (validates the "
                "blocked-front lever vs MUMPS) --\n");
    for (crd::u32 N : {256U, 512U, 1024U, 1536U})
    {
        crd::containers::Array<double> base(&g_alloc);
        base.resize(static_cast<crd::usize>(N) * N);
        for (crd::u32 c = 0; c < N; ++c)
        {
            for (crd::u32 r = 0; r < N; ++r)
            {
                base[static_cast<crd::usize>(c) * N + r] = (r == c) ? static_cast<double>(N + 1) : 1.0; // SPD
            }
        }
        const double gflop = (static_cast<double>(N) * N * N) / 3.0 / 1e9; // ~LDLᵀ factor flops

        crd::containers::Array<double> du(&g_alloc);
        du = base;
        crd::containers::Array<crd::u8> bk(&g_alloc);
        bk.resize(N);
        crd::containers::Array<crd::u32> pv(&g_alloc);
        pv.resize(N);
        const auto u0 = Clock::now();
        const crd::u32 ru = dir::factor_front_ldlt<double, false>(du.data(), N, N, N, bk.data(), pv.data());
        const auto u1 = Clock::now();
        const double ums = std::chrono::duration<double, std::milli>(u1 - u0).count();

        crd::containers::Array<double> db(&g_alloc);
        db = base;
        const auto b0 = Clock::now();
        blocked_ldl_1x1(db.data(), N, 64, &g_alloc);
        const auto b1 = Clock::now();
        const double bms = std::chrono::duration<double, std::milli>(b1 - b0).count();

        double diff = 0.0;
        for (crd::u32 c = 0; c < N; ++c)
        {
            for (crd::u32 r = c; r < N; ++r)
            {
                const double e = std::abs(du[static_cast<crd::usize>(c) * N + r] - db[static_cast<crd::usize>(c) * N + r]);
                diff = e > diff ? e : diff;
            }
        }
        std::printf("front N=%-5u | unblocked %8.3f ms (%5.1f GF/s)  blocked-1×1 %8.3f ms (%5.1f GF/s)  speedup "
                    "%.1fx | (ru=%u, |L_u-L_b|=%.1e)\n",
                    N, ums, gflop / (ums / 1e3), bms, gflop / (bms / 1e3), (bms > 0 ? ums / bms : 0.0), ru, diff);
    }
}

#ifdef CRD_HESAP_VS_MUMPS
// THE DISCRIMINATING MEASUREMENT (advisor): factor one dense front (≈ the 3D root-separator size) three ways —
// Cerid's SHIPPING blocked kernel, LAPACK dpotrf (Cholesky = absolute BLAS-3 ceiling), LAPACK dsytrf (the same
// BK LDLᵀ MUMPS runs per front). Partitions the multifrontal-vs-MUMPS gap: Cerid≈dsytrf ⇒ kernel is at par,
// the gap is multifrontal overhead/amalgamation (panel-TRSM is wasted); Cerid≈½·dpotrf ⇒ kernel gap (grind it).
void front_kernel_vs_lapack()
{
    namespace dir = crd::hesap::direct;
    std::printf("\n-- FRONT-KERNEL CEILING: Cerid blocked vs LAPACK dpotrf (Cholesky ceiling) vs dsytrf (BK peer) --\n");
    for (crd::u32 N : {1024U, 1600U, 2048U})
    {
        crd::containers::Array<double> base(&g_alloc);
        base.resize(static_cast<crd::usize>(N) * N);
        for (crd::u32 c = 0; c < N; ++c)
        {
            for (crd::u32 r = 0; r < N; ++r)
            {
                base[static_cast<crd::usize>(c) * N + r] = (r == c) ? static_cast<double>(N + 1) : 1.0; // SPD
            }
        }
        const double gflop = (static_cast<double>(N) * N * N) / 3.0 / 1e9;
        const int n = static_cast<int>(N);
        const int lda = n;

        // Cerid blocked (the shipping kernel, nb=128 — matches the driver)
        crd::containers::Array<double> dc(&g_alloc);
        dc = base;
        crd::containers::Array<crd::u8> bk(&g_alloc);
        bk.resize(N);
        crd::containers::Array<crd::u32> pv(&g_alloc);
        pv.resize(N);
        const auto c0 = Clock::now();
        (void)dir::factor_front_ldlt_blocked<double>(dc.data(), N, N, N, bk.data(), pv.data(), 128, &g_alloc);
        const double cms = std::chrono::duration<double, std::milli>(Clock::now() - c0).count();

        // LAPACK dpotrf (Cholesky, lower) — the absolute BLAS-3 ceiling
        crd::containers::Array<double> dp(&g_alloc);
        dp = base;
        int info = 0;
        const char uplo = 'L';
        const auto p0 = Clock::now();
        dpotrf_(&uplo, &n, dp.data(), &lda, &info);
        const double pms = std::chrono::duration<double, std::milli>(Clock::now() - p0).count();

        // LAPACK dsytrf (Bunch-Kaufman LDLᵀ, lower) — the same-algorithm peer MUMPS runs per front
        crd::containers::Array<double> ds(&g_alloc);
        ds = base;
        crd::containers::Array<int> ipiv(&g_alloc);
        ipiv.resize(N);
        int lwork = -1;
        double wq = 0.0;
        int info2 = 0;
        dsytrf_(&uplo, &n, ds.data(), &lda, ipiv.data(), &wq, &lwork, &info2); // workspace query
        lwork = static_cast<int>(wq);
        crd::containers::Array<double> work(&g_alloc);
        work.resize(static_cast<crd::usize>(lwork > 0 ? lwork : 1));
        const auto s0 = Clock::now();
        dsytrf_(&uplo, &n, ds.data(), &lda, ipiv.data(), work.data(), &lwork, &info2);
        const double sms = std::chrono::duration<double, std::milli>(Clock::now() - s0).count();

        std::printf("N=%-5u | Cerid-blocked %7.2f ms (%5.1f GF/s) | dpotrf %7.2f ms (%5.1f GF/s) | dsytrf %7.2f "
                    "ms (%5.1f GF/s) | Cerid/dsytrf %.2fx  Cerid/dpotrf %.2fx\n",
                    N, cms, gflop / (cms / 1e3), pms, gflop / (pms / 1e3), sms, gflop / (sms / 1e3),
                    (sms > 0 ? cms / sms : 0.0), (pms > 0 ? cms / pms : 0.0));
    }
}
#endif

// v5d-h indefinite-perf lever: sweep the Bunch-Kaufman pivot threshold on the indefinite 3D Laplacian.
// Lower alpha ⇒ fewer Duff-Reid delays ⇒ smaller fronts (maxf) ⇒ faster — but looser element growth ⇒ watch
// the residual (the win is only honest if it stays competitive with MUMPS ~6e-11). Reports ALL together.
void threshold_sweep()
{
    std::printf("\n-- PIVOT-THRESHOLD SWEEP (indef 3D A−3I): alpha ↓ ⇒ delays ↓ ⇒ maxf ↓ ⇒ time ↓; residual is "
                "the guardrail (IR-recovered; FACTOR FAILED = IR didn't converge) --\n");
    for (crd::u32 k : {24U, 32U})
    {
        crd::containers::Array<Trip> t(&g_alloc);
        crd::u32 n = 0;
        make_indefinite_laplacian_3d(k, 3.0, n, t);
        auto a0 = to_cerid(n, t);
        auto perm = ord::amd_order(a0.pattern(), &g_alloc);
        crd::containers::Array<Trip> pt(&g_alloc);
        pt.reserve(t.size());
        for (const auto& e : t)
        {
            pt.push_back({perm.inv_perm[e.r], perm.inv_perm[e.c], e.v});
        }
        crd::containers::Array<crd::f64> b(&g_alloc);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b[i] = 0.0;
        }
        for (const auto& e : pt)
        {
            b[e.r] += e.v; // b = (permuted A)·ones ⇒ x_true = ones
        }
        for (double alpha : {0.01, 0.003, 0.001, 0.0003, 0.0001})
        {
            auto a = to_cerid(n, pt);
            dir::MultifrontalLDLT<crd::f64> f(&g_alloc);
            f.set_pivot_threshold(alpha);
            const auto t0 = Clock::now();
            f.factorize(a, 1);
            crd::containers::Array<crd::f64> x(&g_alloc);
            x.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            const bool ok = (f.info() == 0) && f.solve({x.data(), n});
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            const double res = ok ? residual_inf(n, pt, x, b) : -1.0;
            std::printf("  k=%-2u n=%-6u alpha=%.2f | %9.2f ms | del=%-6u maxf=%-6u | resid=%.1e %s\n", k, n, alpha,
                        ms, f.delayed_count(), f.max_front_dim(), res, ok ? "" : "(FACTOR FAILED)");
        }
    }
}

void run_case(const char* name, crd::u32 n, const crd::containers::Array<Trip>& t)
{
    // AMD-order once (on the symmetric pattern), permute BOTH factorizations' input identically:
    // new(i,j) = A(perm[i], perm[j]) ⇒ original (r,c) → (inv_perm[r], inv_perm[c]).
    auto a0 = to_cerid(n, t);
    auto perm = ord::amd_order(a0.pattern(), &g_alloc);
    crd::containers::Array<Trip> pt(&g_alloc);
    pt.reserve(t.size());
    for (const auto& e : t)
    {
        pt.push_back({perm.inv_perm[e.r], perm.inv_perm[e.c], e.v});
    }
    // Consistent RHS b = (permuted A)·ones (x_true = ones; a permutation of ones is ones, so the residual is
    // the same as in the original ordering).
    crd::containers::Array<crd::f64> b(&g_alloc);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 0.0;
    }
    for (const auto& e : pt)
    {
        b[e.r] += e.v;
    }

    // --- Cerid multifrontal LDLᵀ (Bunch-Kaufman) on the AMD-permuted matrix ---
    crd::f64 cerid_res = -1.0;
    double cerid_ms = 0.0;
    bool cerid_ok = false;
    crd::u32 cerid_ndelay = 0;
    crd::u32 cerid_nfront = 0;
    crd::u32 cerid_maxfront = 0;
    {
        auto a = to_cerid(n, pt);
        const auto t0 = Clock::now();
        auto f = dir::factor_multifrontal_ldlt<crd::f64>(a, &g_alloc, 1);
        crd::containers::Array<crd::f64> x(&g_alloc);
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = b[i];
        }
        const bool solved = (f.info() == 0) && f.solve({x.data(), n});
        const auto t1 = Clock::now();
        cerid_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        cerid_ok = solved;
        cerid_ndelay = f.delayed_count(); // v5d-h diagnostic: how many pivots delayed (front-blowup indicator)
        cerid_nfront = f.front_count();
        cerid_maxfront = f.max_front_dim();
        if (solved)
        {
            cerid_res = residual_inf(n, pt, x, b);
        }
    }

    // --- Eigen SimplicialLDLT<…, NaturalOrdering> on the SAME permuted matrix (pivot-free; no re-reorder) ---
    crd::f64 eigen_res = -1.0;
    double eigen_ms = 0.0;
    const char* eigen_status = "ok";
    {
        crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
        et.reserve(pt.size());
        for (const auto& e : pt)
        {
            if (e.r >= e.c) // lower triangle
            {
                et.push_back(Eigen::Triplet<double>(static_cast<int>(e.r), static_cast<int>(e.c), e.v));
            }
        }
        Eigen::SparseMatrix<double> ea(static_cast<int>(n), static_cast<int>(n));
        ea.setFromTriplets(et.begin(), et.end());
        Eigen::VectorXd eb(static_cast<int>(n));
        for (crd::u32 i = 0; i < n; ++i)
        {
            eb[static_cast<int>(i)] = b[i];
        }
        const auto t0 = Clock::now();
        Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::NaturalOrdering<int>> ldlt;
        ldlt.compute(ea);
        Eigen::VectorXd ex = ldlt.solve(eb);
        const auto t1 = Clock::now();
        eigen_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ldlt.info() != Eigen::Success)
        {
            eigen_status = "NumericalIssue (pivot-free LDLᵀ cannot factor this)";
        }
        else
        {
            bool finite = true;
            crd::containers::Array<crd::f64> x(&g_alloc);
            x.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                const double xv = ex[static_cast<int>(i)];
                finite = finite && std::isfinite(xv);
                x[i] = xv;
            }
            eigen_status = finite ? "ok" : "NaN/Inf solution";
            if (finite)
            {
                eigen_res = residual_inf(n, pt, x, b);
            }
        }
    }

    const double ratio = (cerid_ms > 0.0 && eigen_ms > 0.0) ? cerid_ms / eigen_ms : 0.0;
    std::printf("%-22s n=%-6u | Cerid %s %8.3f ms resid=%.1e (nf=%u del=%u maxf=%u) | Eigen %8.3f ms resid=%.1e "
                "[%s] | Cerid/Eigen %.1fx\n",
                name, n, cerid_ok ? "OK" : "FAIL", cerid_ms, cerid_res, cerid_nfront, cerid_ndelay, cerid_maxfront,
                eigen_ms, eigen_res, eigen_status, ratio);

#ifdef CRD_HESAP_VS_MUMPS
    // MUMPS LDLᵀ (SYM=2, 2×2 Bunch-Kaufman — the REAL same-class indefinite gold standard). Fed the SAME
    // AMD-permuted matrix (lower triangle, 1-based) + ICNTL(7)=1 PERM_IN=identity ⇒ identical fill to Cerid
    // (no re-reorder) = a pure kernel comparison, matching the Eigen NaturalOrdering protocol. Run MUMPS with
    // OMP_NUM_THREADS=1 for the serial-vs-serial headline (Cerid here is num_workers=1).
    {
        crd::containers::Array<MUMPS_INT> mirn(&g_alloc);
        crd::containers::Array<MUMPS_INT> mjcn(&g_alloc);
        crd::containers::Array<double> ma(&g_alloc);
        for (const auto& e : pt)
        {
            if (e.r >= e.c) // lower triangle (SYM=2)
            {
                mirn.push_back(static_cast<MUMPS_INT>(e.r + 1));
                mjcn.push_back(static_cast<MUMPS_INT>(e.c + 1));
                ma.push_back(e.v);
            }
        }
        crd::containers::Array<MUMPS_INT> mperm(&g_alloc);
        mperm.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            mperm[i] = static_cast<MUMPS_INT>(i + 1); // identity (1-based): eliminate in the given AMD order
        }
        DMUMPS_STRUC_C id;
        id.comm_fortran = -987654; // USE_COMM_WORLD (sequential/OpenMP build)
        id.par = 1;
        id.sym = 2; // general symmetric ⇒ LDLᵀ with 2×2 Bunch-Kaufman pivoting
        id.job = -1;
        dmumps_c(&id);
        id.icntl[0] = -1;
        id.icntl[1] = -1;
        id.icntl[2] = -1;
        id.icntl[3] = 0;          // silence
        id.icntl[6] = 1;          // ICNTL(7)=1 ⇒ user pivot order in PERM_IN (the AMD order, no re-reorder)
        // v5e-3 PREMISE CHECK: CRD_MUMPS_BLR=<k> activates MUMPS-BLR ICNTL(35)=k
        // (1/2/3 = the FSCU/UFSC variants) with CNTL(7)=CRD_MUMPS_BLR_EPS (the BLR
        // dropping threshold). Unset ⇒ full (ICNTL(35)=0). Lets one bench binary
        // measure MUMPS-BLR vs MUMPS-full on the SAME corpus.
        if (const char* blr = std::getenv("CRD_MUMPS_BLR"))
        {
            id.icntl[34] = std::atoi(blr);  // ICNTL(35), 0-based index 34
            const char* eps = std::getenv("CRD_MUMPS_BLR_EPS");
            id.cntl[6] = eps ? std::atof(eps) : 1e-8;  // CNTL(7), 0-based index 6
        }
        id.perm_in = mperm.data();
        id.n = static_cast<MUMPS_INT>(n);
        id.nnz = static_cast<MUMPS_INT8>(mirn.size());
        id.irn = mirn.data();
        id.jcn = mjcn.data();
        id.a = ma.data();
        const auto m0 = Clock::now();
        id.job = 4; // analyze + factorize
        dmumps_c(&id);
        const auto m1 = Clock::now();
        const double mumps_ms = std::chrono::duration<double, std::milli>(m1 - m0).count();
        crd::containers::Array<double> mx(&g_alloc);
        mx.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            mx[i] = b[i];
        }
        id.rhs = mx.data();
        id.job = 3; // solve
        dmumps_c(&id);
        crd::containers::Array<crd::f64> mxt(&g_alloc);
        mxt.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            mxt[i] = mx[i];
        }
        const double mumps_res = (id.infog[0] < 0) ? 1e30 : residual_inf(n, pt, mxt, b);
        id.job = -2;
        dmumps_c(&id); // finalize
        const double mratio = (cerid_ms > 0.0 && mumps_ms > 0.0) ? cerid_ms / mumps_ms : 0.0;
        std::printf("%-22s         | MUMPS LDLᵀ(SYM=2) %8.3f ms resid=%.1e infog1=%d del=%d | Cerid/MUMPS %.2fx%s\n",
                    name, mumps_ms, mumps_res, id.infog[0], id.infog[12], mratio,
                    mratio < 1.0 ? "  <<< CERID CRUSHES MUMPS" : "");
    }
#endif
}
} // namespace

int main()
{
    crd::jobs::init();
    std::printf("=== hesap multifrontal LDLᵀ (Bunch-Kaufman) vs Eigen SimplicialLDLT (pivot-free) ===\n");
    std::printf("FAIR: AMD-ordered once, BOTH fed the permuted matrix (Eigen NaturalOrdering). Honest peer\n");
    std::printf("note: Eigen SimplicialLDLT is pivot-free ⇒ NOT same-class on indefinite; fair indefinite\n");
    std::printf("peer = MA57/MUMPS-LDLᵀ/PARDISO (deferred to v5z). Eigen Simplicial* is a tuned scalar code;\n");
    std::printf("Cerid v5d is correctness-first multifrontal (no perf pass) ⇒ a characterized speed gap, not a\n");
    std::printf("win — the win is correctness-on-all-indefinite + the cross-thread determinism moat.\n\n");

    front_lever_probe();
#ifdef CRD_HESAP_VS_MUMPS
    front_kernel_vs_lapack();
#endif
    threshold_sweep();

    std::printf("\n-- DEFINITE (SPD 2D Laplacian) — both correct; the dense-front amortization regime: small n\n");
    std::printf("   is the multifrontal-overhead regime (simplicial wins), large n is its home turf. --\n");
    for (crd::u32 k : {32U, 64U, 96U, 128U, 160U, 224U})
    {
        crd::containers::Array<Trip> t(&g_alloc);
        crd::u32 n = 0;
        make_spd_laplacian(k, 0.0, n, t);
        run_case("spd_laplacian", n, t);
    }

    std::printf("\n-- DEFINITE (SPD 3D Laplacian) — BIG dense fronts (separator ~n^2/3): multifrontal's home turf --\n");
    for (crd::u32 k : {16U, 24U, 32U, 40U, 48U, 56U, 64U})  // k=64 ⇒ n=262144, root front ~4096 (BLR premise)
    {
        crd::containers::Array<Trip> t(&g_alloc);
        crd::u32 n = 0;
        make_spd_laplacian_3d(k, n, t);
        run_case("spd_laplacian_3d", n, t);
    }

    std::printf("\n-- INDEFINITE (3D shifted Laplacian A−3I) — BIG dense INDEFINITE fronts: LDLᵀ's actual domain\n");
    std::printf("   (SPD ⇒ Cholesky). Exercises 2×2 BK pivots inside large fronts vs MUMPS SYM=2. --\n");
    for (crd::u32 k : {16U, 24U, 32U})
    {
        crd::containers::Array<Trip> t(&g_alloc);
        crd::u32 n = 0;
        make_indefinite_laplacian_3d(k, 3.0, n, t);
        run_case("indef_laplacian_3d", n, t);
    }

    std::printf("\n-- INDEFINITE (KKT saddle-point) — Eigen's reorder+fill regularizes these ⇒ both correct --\n");
    for (crd::u32 mm : {200U, 800U, 1800U})
    {
        crd::containers::Array<Trip> t(&g_alloc);
        crd::u32 n = 0;
        make_kkt_saddle(mm, mm / 4, n, t);
        run_case("kkt_saddle_point", n, t);
    }

    std::printf("\n-- PERSISTENTLY-ZERO PIVOT [[0,1],[1,0]] — the narrow-but-real pivot-free failure (receipt) --\n");
    {
        crd::containers::Array<Trip> t(&g_alloc);
        crd::u32 n = 0;
        make_zero_diag_2x2(n, t);
        run_case("zero_diag_2x2", n, t);
    }

    crd::jobs::shutdown();
    return 0;
}
