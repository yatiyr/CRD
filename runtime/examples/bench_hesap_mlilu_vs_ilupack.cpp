// bench_hesap_mlilu_vs_ilupack.cpp -- Phase 3.1.6 v4j-2a.
//
// Apples-to-apples head-to-head: Cerid InverseBasedIlu (inverse-based-pivoting
// multilevel ILU core, Bollhöfer-Saad SISC 2006) vs ILUPACK V2.4 (the same-
// algorithm reference) on a BYTE-IDENTICAL matrix generated in-process and fed
// to both. Matched true residual ‖b−Ax‖/‖b‖ at the same rel_tol; report factor
// + solve wall, GMRES iterations, fill nnz(L+U)/nnz(A), and number of levels.
//
// HONEST framing for v4j-2a: Cerid is SINGLE-LEVEL (accepted block + one ILUT
// leaf ⇒ ≤ 2 levels) while ILUPACK RECURSES (nlev varies). Cerid is therefore
// NOT expected to beat ILUPACK on iterations here — the criterion is matching
// fill + iteration count up to leaf depth 2; the multilevel recursion (v4j-2b)
// and the crush land later. This bench is the baseline + correctness oracle.
//
// LOCAL-ONLY: ILUPACK is non-commercial/binary-only ⇒ gated behind the dev flag
// CRD_BUILD_HESAP_VS_ILUPACK, links external/ilupack (gitignored), WSL/Linux
// only. NEVER built in CI, NEVER shipped. See scripts/setup-ilupack-ref.sh.

#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/ordering/amd.hpp>
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/hesap/preconditioners/inverse_based_ilu.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// ILUPACK is plain C (f2c-style). integer = int for the GNU64 (32-bit-int) build;
// do NOT define _LONG_INTEGER_. __UNDERSCORE__ matches the precompiled .a mangling.
extern "C"
{
#include <ilupack.h>
}

namespace hs = crd::hesap::sparse;
namespace hi = crd::hesap::iterative;
namespace hd = crd::hesap::dense;
namespace hp = crd::hesap::preconditioners;
using Clock  = std::chrono::steady_clock;
using Csr    = hs::SparseMatrix<double, hs::SparseFormat::Csr>;

static crd::memory::MallocAllocator g_alloc;
static crd::usize g_restart = 30; // FGMRES restart (argv[4]) — v4j-3 diagnostic

// Anisotropic convection-diffusion on an N×N grid (the ILUPACK model problem,
// encyclopedia §Mathematical Background): −ε·u_xx − u_yy + β·u_x, 5-point upwind.
// Small ε makes it strongly anisotropic ⇒ ILUPACK builds several levels (semi-
// coarsening). β adds nonsymmetry. n = N².
static Csr build_cd2d(int gridN, double eps, double beta)
{
    const int          n = gridN * gridN;
    hs::TripletBuilder<double> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    auto idx = [gridN](int i, int j) { return i * gridN + j; };
    for (int i = 0; i < gridN; ++i)
    {
        for (int j = 0; j < gridN; ++j)
        {
            const int r = idx(i, j);
            tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(r), 2.0 * eps + 2.0);
            if (j > 0)         { tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(idx(i, j - 1)), -eps - beta); } // west
            if (j + 1 < gridN) { tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(idx(i, j + 1)), -eps + beta); } // east
            if (i > 0)         { tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(idx(i - 1, j)), -1.0); }        // south
            if (i + 1 < gridN) { tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(idx(i + 1, j)), -1.0); }        // north
        }
    }
    return tb.compress();
}

struct Row
{
    const char* who;
    int         levels;
    double      fill;
    long        iters;
    double      factor_ms;
    double      solve_ms;
    double      true_res;
    bool        converged;
};

static void print_row(const Row& r)
{
    std::printf("  %-13s levels=%-3d fill=%6.2f iters=%-5ld factor=%8.2f ms  solve=%8.2f ms  ||b-Ax||/||b||=%.2e %s\n",
                r.who, r.levels, r.fill, r.iters, r.factor_ms, r.solve_ms, r.true_res, r.converged ? "" : "[DIVERGED]");
}

static Row run_cerid(const Csr& a, const std::vector<double>& b, double condest, double droptol, double rel_tol,
                     double milu, const char* who, bool reorder); // fwd-decl (defaults on the definition below)

// ---- v4j-3 discriminating experiment: AMD-reorder then InverseBasedIlu --------
// Builds B = P A Pᵀ (AMD on A's pattern), factors B, solves the (norm-equivalent) reordered
// system. Tests whether REORDERING makes the deferred Schur stay HARD → deeper hierarchy
// (the HILUCSI/ILUPACK mechanism). Watch the per-level CRD_MLILU_DEBUG output for depth.
[[maybe_unused]] static Row run_cerid_amd(const Csr& a, const std::vector<double>& b, double condest,
                                          double droptol, double rel_tol)
{
    namespace ord = crd::hesap::ordering;
    const crd::u32 n = a.rows();
    auto t0 = Clock::now(); // HONEST timing: include the AMD reorder + PAPᵀ build in the factor cost.
    auto perm = ord::amd_order(a.pattern(), &g_alloc); // perm[i] = original vertex at new slot i
    const double amd_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    auto t_build = Clock::now();
    // B = P A Pᵀ : for each A entry (r,c,v) → B[inv[r], inv[c]] = v.
    crd::hesap::sparse::TripletBuilder<double> tb(&g_alloc, n, n);
    const auto* o = a.pattern().outer_ptr.data();
    const auto* ci = a.pattern().inner_idx.data();
    const auto* va = a.values().values.data();
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 q = o[r]; q < o[r + 1]; ++q) { tb.add(perm.inv_perm[r], perm.inv_perm[ci[q]], va[q]); }
    }
    Csr bmat = tb.compress();
    std::vector<double> bp(n);
    for (crd::u32 i = 0; i < n; ++i) { bp[i] = b[perm.perm[i]]; } // b' = P b
    const double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_build).count();
    const double reorder_ms = amd_ms + build_ms; // amd_order now near-linear (v4j-3(b) fix); PAPᵀ build ~1ms
    Row row = run_cerid(bmat, bp, condest, droptol, rel_tol, 0.0, "Cerid-AMD", false);
    row.factor_ms += reorder_ms; // fold the reorder+build cost into factor (honest total wall-time)
    return row;
}

// ---- Cerid: InverseBasedIlu + FGMRES(30) -------------------------------------
static Row run_cerid(const Csr& a, const std::vector<double>& b, double condest, double droptol, double rel_tol,
                     double milu = 0.0, const char* who = "Cerid", bool reorder = false)
{
    const crd::u32 n = a.rows();
    // Sequential spmv — fair single-thread comparison vs ILUPACK V2.4 (serial). A
    // parallel operator over-parallelizes these sub-L3 matrices and the dispatch
    // overhead swamps the solve (feedback_krylov_operator_size_adaptive_and_frame_reset).
    hs::SparseLinearOp<double> op(a);

    auto t0 = Clock::now();
    hp::InverseBasedIlu<double> m(a, &g_alloc, condest, droptol, 0U, 50U, 64U, hp::Mc64Mode::None, milu, reorder);
    auto t1 = Clock::now();

    hd::Vector<double> bx(&g_alloc, n), x(&g_alloc, n);
    for (crd::u32 i = 0; i < n; ++i) { bx(i) = b[i]; x(i) = 0.0; }
    hi::IterativeOptions<double> opts;
    opts.rel_tol  = rel_tol;
    opts.max_iter = 2000;
    hi::GmresWorkspace<double> ws(&g_alloc, n, g_restart);
    auto t2  = Clock::now();
    auto res = hi::fgmres<double>(op, &m, bx.span(), x.span(), opts, ws, &g_alloc);
    auto t3  = Clock::now();

    // true residual ‖b − A x‖/‖b‖
    hd::Vector<double> ax(&g_alloc, n);
    (void)op.apply(x.span(), ax.span());
    double nb = 0, nr = 0;
    for (crd::u32 i = 0; i < n; ++i) { nb += b[i] * b[i]; const double d = b[i] - ax(i); nr += d * d; }

    Row row;
    row.who       = who;
    row.levels    = static_cast<int>(m.num_levels());
    row.fill      = static_cast<double>(m.factor_nnz()) / static_cast<double>(a.nnz());
    row.iters     = static_cast<long>(res.iterations);
    row.factor_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    row.solve_ms  = std::chrono::duration<double, std::milli>(t3 - t2).count();
    row.true_res  = std::sqrt(nr / nb);
    row.converged = res.converged;
    return row;
}

// ---- Cerid: MC64-fronted InverseBasedIlu + FGMRES(30) (v4j convection follow-on) ----
// mode = TopLevel (match+scale top matrix only) or EveryLevel (the full ILUPACK pipeline,
// every Schur level re-matched). `who` labels the row so the three Cerid configs are distinct.
[[maybe_unused]] static Row run_cerid_mc64(const Csr& a, const std::vector<double>& b, double condest,
                                           double droptol, double rel_tol, hp::Mc64Mode mode, const char* who)
{
    const crd::u32 n = a.rows();
    hs::SparseLinearOp<double> op(a);

    auto t0 = Clock::now();
    hp::InverseBasedIlu<double> m(a, &g_alloc, condest, droptol, /*level=*/0U, /*max_levels=*/50U,
                                  /*dense_threshold=*/64U, mode);
    auto t1 = Clock::now();

    hd::Vector<double> bx(&g_alloc, n), x(&g_alloc, n);
    for (crd::u32 i = 0; i < n; ++i) { bx(i) = b[i]; x(i) = 0.0; }
    hi::IterativeOptions<double> opts;
    opts.rel_tol  = rel_tol;
    opts.max_iter = 2000;
    hi::GmresWorkspace<double> ws(&g_alloc, n, g_restart);
    auto t2  = Clock::now();
    auto res = hi::fgmres<double>(op, &m, bx.span(), x.span(), opts, ws, &g_alloc);
    auto t3  = Clock::now();

    hd::Vector<double> ax(&g_alloc, n);
    (void)op.apply(x.span(), ax.span());
    double nb = 0, nr = 0;
    for (crd::u32 i = 0; i < n; ++i) { nb += b[i] * b[i]; const double d = b[i] - ax(i); nr += d * d; }

    Row row;
    row.who       = who;
    row.levels    = static_cast<int>(m.num_levels());
    row.fill      = static_cast<double>(m.factor_nnz()) / static_cast<double>(a.nnz());
    row.iters     = static_cast<long>(res.iterations);
    row.factor_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    row.solve_ms  = std::chrono::duration<double, std::milli>(t3 - t2).count();
    row.true_res  = std::sqrt(nr / nb);
    row.converged = res.converged;
    return row;
}

// ---- Cerid: Smoothed-Aggregation AMG (v4k-a) + FGMRES(30) -------------------
// cycle = V (robust) / W (convection lever) / K (Krylov-accelerated, v4k-b). `who` labels.
static Row run_cerid_amg(const Csr& a, const std::vector<double>& b, double rel_tol,
                         crd::hesap::amg::SaAmg<double>::Cycle cycle = crd::hesap::amg::SaAmg<double>::Cycle::V,
                         const char* who = "Cerid-AMG", bool smooth_prolongator = true, bool adaptive = false)
{
    const crd::u32 n = a.rows();
    hs::SparseLinearOp<double> op(a);

    crd::hesap::amg::SaAmg<double>::Options aopts;
    aopts.cycle              = cycle;
    aopts.smooth_prolongator = smooth_prolongator;
    aopts.adaptive_candidate = adaptive;
    auto t0 = Clock::now();
    crd::hesap::amg::SaAmg<double> m(a, &g_alloc, aopts);
    auto t1 = Clock::now();

    hd::Vector<double> bx(&g_alloc, n), x(&g_alloc, n);
    for (crd::u32 i = 0; i < n; ++i) { bx(i) = b[i]; x(i) = 0.0; }
    hi::IterativeOptions<double> opts;
    opts.rel_tol  = rel_tol;
    opts.max_iter = 2000;
    hi::GmresWorkspace<double> ws(&g_alloc, n, 30);
    auto t2  = Clock::now();
    auto res = hi::fgmres<double>(op, &m, bx.span(), x.span(), opts, ws, &g_alloc);
    auto t3  = Clock::now();

    hd::Vector<double> ax(&g_alloc, n);
    (void)op.apply(x.span(), ax.span());
    double nb = 0, nr = 0;
    for (crd::u32 i = 0; i < n; ++i) { nb += b[i] * b[i]; const double d = b[i] - ax(i); nr += d * d; }

    Row row;
    row.who       = who;
    row.levels    = static_cast<int>(m.num_levels());
    row.fill      = static_cast<double>(m.operator_complexity()) / static_cast<double>(a.nnz()); // operator complexity
    row.iters     = static_cast<long>(res.iterations);
    row.factor_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    row.solve_ms  = std::chrono::duration<double, std::milli>(t3 - t2).count();
    row.true_res  = std::sqrt(nr / nb);
    row.converged = res.converged;
    return row;
}

static const char* g_ilupack_ordering = "metisn"; // overridable via argv[3] (v4j-3 experiment)

// ---- ILUPACK: DGNLAMG (multilevel inverse-based ILU) + its GMRES -------------
static Row run_ilupack(const Csr& a, const std::vector<double>& b, double condest, double droptol, double rel_tol)
{
    const int n   = static_cast<int>(a.rows());
    const int nnz = static_cast<int>(a.nnz());

    // Build a 1-based CSR copy in ILUPACK's Dmat (kept alive through factor+solve).
    std::vector<integer> ia(static_cast<std::size_t>(n) + 1), ja(static_cast<std::size_t>(nnz));
    std::vector<double>  av(static_cast<std::size_t>(nnz));
    const auto* rp = a.pattern().outer_ptr.data();
    const auto* ci = a.pattern().inner_idx.data();
    const auto* vv = a.values().values.data();
    for (int i = 0; i <= n; ++i) { ia[static_cast<std::size_t>(i)] = static_cast<integer>(rp[i]) + 1; }
    for (int q = 0; q < nnz; ++q) { ja[static_cast<std::size_t>(q)] = static_cast<integer>(ci[q]) + 1; av[static_cast<std::size_t>(q)] = vv[q]; }

    Dmat A;
    A.nr = A.nc = n;
    A.nnz       = nnz;
    A.ia        = ia.data();
    A.ja        = ja.data();
    A.a         = av.data();
    A.isreal = 1; A.issingle = 0; A.issymmetric = 0; A.isdefinite = 0; A.ishermitian = 0; A.isskew = 0;

    DILUPACKparam param;
    DAMGlevelmat  PRE;
    DGNLAMGinit(&A, &param);
    param.matching = 1;
    param.ordering = const_cast<char*>(g_ilupack_ordering);
    param.droptol  = droptol;
    param.droptolS = 0.1 * droptol;
    param.condest  = condest;
    param.restol   = rel_tol;
    param.maxit    = 2000;
    param.elbow    = 10.0;
    param.nrestart = 30;

    Row row;
    row.who = "ILUPACK";

    auto t0   = Clock::now();
    int  ierr = static_cast<int>(DGNLAMGfactor(&A, &PRE, &param));
    auto t1   = Clock::now();
    if (ierr != 0)
    {
        std::printf("  ILUPACK   factor failed (ierr=%d)\n", ierr);
        row.levels = 0; row.fill = 0; row.iters = 0; row.factor_ms = 0; row.solve_ms = 0; row.true_res = 1e9; row.converged = false;
        return row;
    }

    std::vector<double> rhs(static_cast<std::size_t>(n)), sol(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) { rhs[static_cast<std::size_t>(i)] = b[static_cast<std::size_t>(i)]; }

    auto t2 = Clock::now();
    ierr    = static_cast<int>(DGNLAMGsolver(&A, &PRE, &param, rhs.data(), sol.data()));
    auto t3 = Clock::now();

    // true residual on the original A (ILUPACK solved A x = rhs)
    std::vector<double> ax(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i)
    {
        double s = 0.0;
        for (int q = static_cast<int>(rp[i]); q < static_cast<int>(rp[i + 1]); ++q) { s += vv[q] * sol[static_cast<std::size_t>(ci[q])]; }
        ax[static_cast<std::size_t>(i)] = s;
    }
    double nb = 0, nr = 0;
    for (int i = 0; i < n; ++i) { nb += b[static_cast<std::size_t>(i)] * b[static_cast<std::size_t>(i)]; const double d = b[static_cast<std::size_t>(i)] - ax[static_cast<std::size_t>(i)]; nr += d * d; }

    row.levels    = static_cast<int>(PRE.nlev);
    row.fill      = param.elbow;          // ILUPACK reports achieved fill relative to A
    row.iters     = static_cast<long>(param.niter); // V2.4 iteration count (ipar[25] is stale)
    row.factor_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    row.solve_ms  = std::chrono::duration<double, std::milli>(t3 - t2).count();
    row.true_res  = std::sqrt(nr / nb);
    row.converged = (ierr == 0);

    DGNLAMGdelete(&A, &PRE, &param);
    return row;
}

int main(int argc, char** argv)
{
    crd::jobs::init();

    const double condest = (argc > 1) ? std::atof(argv[1]) : 5.0;   // κ — inverse-factor bound
    if (argc > 3) { g_ilupack_ordering = argv[3]; }                 // ILUPACK ordering (v4j-3 experiment)
    if (argc > 4) { g_restart = static_cast<crd::usize>(std::atoi(argv[4])); } // FGMRES restart
    const double droptol = (argc > 2) ? std::atof(argv[2]) : 1e-2; // ε — inverse-based drop tolerance
    const double rel_tol = 1e-8;  // matched stopping tolerance (≈ √eps)
    const double eps     = (argc > 5) ? std::atof(argv[5]) : 1e-2;  // anisotropy (argv[5])
    const double beta    = (argc > 6) ? std::atof(argv[6]) : 0.3;   // convection / nonsymmetry (argv[6])

    std::printf("hesap v4: SA-AMG / InverseBasedIlu vs ILUPACK V2.4  (anisotropic conv-diff, eps=%.0e beta=%.1f)\n", eps, beta);
    std::printf("  params: condest(kappa)=%.1f droptol=%.0e rel_tol=%.0e, FGMRES(30) / ILUPACK-GMRES(30)\n\n", condest, droptol, rel_tol);

    const int grids[] = {50, 100, 150};
    for (int gN : grids)
    {
        Csr a = build_cd2d(gN, eps, beta);
        const crd::u32 n = a.rows();
        std::vector<double> b(n, 1.0);
        std::printf("cd2d %dx%d  (n=%u, nnz=%u):\n", gN, gN, n, a.nnz());
        using AmgCycle = crd::hesap::amg::SaAmg<double>::Cycle;
        print_row(run_cerid(a, b, condest, droptol, rel_tol));
        // v4j-3(a): PER-LEVEL AMD reorder inside InverseBasedIlu (each Schur reorders ⇒ deepest hierarchy).
        print_row(run_cerid(a, b, condest, droptol, rel_tol, 0.0, "Cerid-IB-rord", /*reorder=*/true));
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::V, "Cerid-AMG-V"));
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::W, "Cerid-AMG-W"));
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::K, "Cerid-AMG-K"));
        // AGMG-style: plain (unsmoothed) aggregation + K-cycle (Notay's convection recipe).
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::K, "Cerid-AGMG-K", /*smooth=*/false));
        // αSA: adaptive near-nullspace candidate + smoothed-K (v4k-e, the β=0.3 attempt).
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::K, "Cerid-aSA-K", /*smooth=*/true, /*adaptive=*/true));
        print_row(run_ilupack(a, b, condest, droptol, rel_tol));
        std::printf("\n");
    }

    crd::jobs::shutdown();
    return 0;
}
