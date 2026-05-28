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
//
// crd conventions throughout: crd::containers::Array (never std::vector), a named
// TlsfAllocator (never malloc), crd::f64/usize/i32/i64 (never raw double/int/size_t).
// Raw double/integer survive ONLY at the ILUPACK C-API boundary (the Dmat struct
// + DGNLAMG* calls demand them); crd::f64 IS double so the Array buffers pass through.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/ordering/amd.hpp>
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/hesap/preconditioners/inverse_based_ilu.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
using Clock = std::chrono::steady_clock;
using Csr = hs::SparseMatrix<crd::f64, hs::SparseFormat::Csr>;

static crd::memory::TlsfAllocator g_alloc{crd::usize{1} << 30}; // 1 GiB pool — named allocator, never malloc
static crd::usize g_restart = 30;                               // FGMRES restart (argv[4]) — v4j-3 diagnostic

// Anisotropic convection-diffusion on an N×N grid (the ILUPACK model problem,
// encyclopedia §Mathematical Background): −ε·u_xx − u_yy + β·u_x, 5-point upwind.
// Small ε makes it strongly anisotropic ⇒ ILUPACK builds several levels (semi-
// coarsening). β adds nonsymmetry. n = N².
static Csr build_cd2d(crd::i32 gridN, crd::f64 eps, crd::f64 beta)
{
    const crd::i32 n = gridN * gridN;
    hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    auto idx = [gridN](crd::i32 i, crd::i32 j)
    {
        return i * gridN + j;
    };
    for (crd::i32 i = 0; i < gridN; ++i)
    {
        for (crd::i32 j = 0; j < gridN; ++j)
        {
            const crd::i32 r = idx(i, j);
            tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(r), 2.0 * eps + 2.0);
            if (j > 0)
            {
                tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(idx(i, j - 1)), -eps - beta);
            } // west
            if (j + 1 < gridN)
            {
                tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(idx(i, j + 1)), -eps + beta);
            } // east
            if (i > 0)
            {
                tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(idx(i - 1, j)), -1.0);
            } // south
            if (i + 1 < gridN)
            {
                tb.add(static_cast<crd::u32>(r), static_cast<crd::u32>(idx(i + 1, j)), -1.0);
            } // north
        }
    }
    return tb.compress();
}

struct Row
{
    const char* who;
    crd::i32 levels;
    crd::f64 fill;
    crd::i64 iters;
    crd::f64 factor_ms;
    crd::f64 solve_ms;
    crd::f64 true_res;
    bool converged;
};

static void print_row(const Row& r)
{
    std::printf("  %-13s levels=%-3d fill=%6.2f iters=%-5lld factor=%8.2f ms  solve=%8.2f ms  ||b-Ax||/||b||=%.2e %s\n",
                r.who, r.levels, r.fill, static_cast<long long>(r.iters), r.factor_ms, r.solve_ms, r.true_res,
                r.converged ? "" : "[DIVERGED]");
}

// ---- v4z Step 1: factor-vs-solve split + reuse break-even --------------------
// Cerid wins by a CHEAPER FACTOR but pays a DEARER SOLVE (more iters); ILUPACK is
// the reverse. The amortised total over k re-solves of the SAME factorization is
// factor + k*solve. ILUPACK overtakes Cerid once its cumulative-cheaper solve has
// repaid Cerid's factor advantage:
//     factor_C + k*solve_C = factor_I + k*solve_I
//  => k* = (factor_I - factor_C) / (solve_C - solve_I)
// Turns the convection "crush" claim from scoped -> quantified: a single solve
// favours whoever has the lower total; many re-solves favour the cheaper solve.
static void print_breakeven(const Row& c, const Row& il)
{
    if (!c.converged || !il.converged)
    {
        std::printf("  break-even[%-13s vs ILUPACK]: n/a (a config diverged)\n", c.who);
        return;
    }
    const crd::f64 save = il.factor_ms - c.factor_ms;  // Cerid's factor advantage (>0 if Cerid cheaper to factor)
    const crd::f64 penalty = c.solve_ms - il.solve_ms; // Cerid's per-solve penalty  (>0 if Cerid dearer to solve)
    std::printf("  split[%-13s]: factor=%8.2f ms  solve=%8.2f ms   |  ILUPACK: factor=%8.2f ms  solve=%8.2f ms\n",
                c.who, c.factor_ms, c.solve_ms, il.factor_ms, il.solve_ms);
    if (save <= 0.0 && penalty <= 0.0)
    {
        std::printf("  break-even[%-13s vs ILUPACK]: ILUPACK dominates from k=1 (cheaper factor AND solve)\n", c.who);
    }
    else if (penalty <= 0.0)
    {
        std::printf("  break-even[%-13s vs ILUPACK]: Cerid dominates for all k (cheaper factor AND solve)\n", c.who);
    }
    else if (save <= 0.0)
    {
        std::printf("  break-even[%-13s vs ILUPACK]: ILUPACK dominates from k=1 (cheaper factor; Cerid dearer solve)\n",
                    c.who);
    }
    else
    {
        const crd::f64 k = save / penalty;
        std::printf("  break-even[%-13s vs ILUPACK]: ILUPACK overtakes after k* = %.1f re-solves"
                    "  (factor save %.2f ms / solve penalty %.2f ms-per-solve)\n",
                    c.who, k, save, penalty);
    }
}

static Row run_cerid(const Csr& a, const crd::containers::Array<crd::f64>& b, crd::f64 condest, crd::f64 droptol,
                     crd::f64 rel_tol, crd::f64 milu, const char* who, bool reorder); // fwd-decl (defaults below)

// ---- v4j-3 discriminating experiment: AMD-reorder then InverseBasedIlu --------
// Builds B = P A Pᵀ (AMD on A's pattern), factors B, solves the (norm-equivalent) reordered
// system. Tests whether REORDERING makes the deferred Schur stay HARD → deeper hierarchy
// (the HILUCSI/ILUPACK mechanism). Watch the per-level CRD_MLILU_DEBUG output for depth.
[[maybe_unused]] static Row run_cerid_amd(const Csr& a, const crd::containers::Array<crd::f64>& b, crd::f64 condest,
                                          crd::f64 droptol, crd::f64 rel_tol)
{
    namespace ord = crd::hesap::ordering;
    const crd::u32 n = a.rows();
    auto t0 = Clock::now(); // HONEST timing: include the AMD reorder + PAPᵀ build in the factor cost.
    auto perm = ord::amd_order(a.pattern(), &g_alloc); // perm[i] = original vertex at new slot i
    const crd::f64 amd_ms = std::chrono::duration<crd::f64, std::milli>(Clock::now() - t0).count();
    auto t_build = Clock::now();
    // B = P A Pᵀ : for each A entry (r,c,v) → B[inv[r], inv[c]] = v.
    hs::TripletBuilder<crd::f64> tb(&g_alloc, n, n);
    const auto* o = a.pattern().outer_ptr.data();
    const auto* ci = a.pattern().inner_idx.data();
    const auto* va = a.values().values.data();
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 q = o[r]; q < o[r + 1]; ++q)
        {
            tb.add(perm.inv_perm[r], perm.inv_perm[ci[q]], va[q]);
        }
    }
    Csr bmat = tb.compress();
    crd::containers::Array<crd::f64> bp(&g_alloc);
    bp.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bp[i] = b[perm.perm[i]];
    } // b' = P b
    const crd::f64 build_ms = std::chrono::duration<crd::f64, std::milli>(Clock::now() - t_build).count();
    const crd::f64 reorder_ms = amd_ms + build_ms; // amd_order now near-linear (v4j-3(b) fix); PAPᵀ build ~1ms
    Row row = run_cerid(bmat, bp, condest, droptol, rel_tol, 0.0, "Cerid-AMD", false);
    row.factor_ms += reorder_ms; // fold the reorder+build cost into factor (honest total wall-time)
    return row;
}

// ---- Cerid: InverseBasedIlu + FGMRES(30) -------------------------------------
static Row run_cerid(const Csr& a, const crd::containers::Array<crd::f64>& b, crd::f64 condest, crd::f64 droptol,
                     crd::f64 rel_tol, crd::f64 milu = 0.0, const char* who = "Cerid", bool reorder = false)
{
    const crd::u32 n = a.rows();
    // Sequential spmv — fair single-thread comparison vs ILUPACK V2.4 (serial). A
    // parallel operator over-parallelizes these sub-L3 matrices and the dispatch
    // overhead swamps the solve (feedback_krylov_operator_size_adaptive_and_frame_reset).
    hs::SparseLinearOp<crd::f64> op(a);

    auto t0 = Clock::now();
    hp::InverseBasedIlu<crd::f64> m(a, &g_alloc, condest, droptol, 0U, 50U, 64U, hp::Mc64Mode::None, milu, reorder);
    auto t1 = Clock::now();

    hd::Vector<crd::f64> bx(&g_alloc, n);
    hd::Vector<crd::f64> x(&g_alloc, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bx(i) = b[i];
        x(i) = 0.0;
    }
    hi::IterativeOptions<crd::f64> opts;
    opts.rel_tol = rel_tol;
    opts.max_iter = 2000;
    hi::GmresWorkspace<crd::f64> ws(&g_alloc, n, g_restart);
    auto t2 = Clock::now();
    auto res = hi::fgmres<crd::f64>(op, &m, bx.span(), x.span(), opts, ws, &g_alloc);
    auto t3 = Clock::now();

    // true residual ‖b − A x‖/‖b‖
    hd::Vector<crd::f64> ax(&g_alloc, n);
    (void)op.apply(x.span(), ax.span());
    crd::f64 nb = 0;
    crd::f64 nr = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        nb += b[i] * b[i];
        const crd::f64 d = b[i] - ax(i);
        nr += d * d;
    }

    Row row;
    row.who = who;
    row.levels = static_cast<crd::i32>(m.num_levels());
    row.fill = static_cast<crd::f64>(m.factor_nnz()) / static_cast<crd::f64>(a.nnz());
    row.iters = static_cast<crd::i64>(res.iterations);
    row.factor_ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
    row.solve_ms = std::chrono::duration<crd::f64, std::milli>(t3 - t2).count();
    row.true_res = std::sqrt(nr / nb);
    row.converged = res.converged;
    return row;
}

// ---- Cerid: MC64-fronted InverseBasedIlu + FGMRES(30) (v4j convection follow-on) ----
// mode = TopLevel (match+scale top matrix only) or EveryLevel (the full ILUPACK pipeline,
// every Schur level re-matched). `who` labels the row so the three Cerid configs are distinct.
[[maybe_unused]] static Row run_cerid_mc64(const Csr& a, const crd::containers::Array<crd::f64>& b, crd::f64 condest,
                                           crd::f64 droptol, crd::f64 rel_tol, hp::Mc64Mode mode, const char* who)
{
    const crd::u32 n = a.rows();
    hs::SparseLinearOp<crd::f64> op(a);

    auto t0 = Clock::now();
    hp::InverseBasedIlu<crd::f64> m(a, &g_alloc, condest, droptol, /*level=*/0U, /*max_levels=*/50U,
                                    /*dense_threshold=*/64U, mode);
    auto t1 = Clock::now();

    hd::Vector<crd::f64> bx(&g_alloc, n);
    hd::Vector<crd::f64> x(&g_alloc, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bx(i) = b[i];
        x(i) = 0.0;
    }
    hi::IterativeOptions<crd::f64> opts;
    opts.rel_tol = rel_tol;
    opts.max_iter = 2000;
    hi::GmresWorkspace<crd::f64> ws(&g_alloc, n, g_restart);
    auto t2 = Clock::now();
    auto res = hi::fgmres<crd::f64>(op, &m, bx.span(), x.span(), opts, ws, &g_alloc);
    auto t3 = Clock::now();

    hd::Vector<crd::f64> ax(&g_alloc, n);
    (void)op.apply(x.span(), ax.span());
    crd::f64 nb = 0;
    crd::f64 nr = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        nb += b[i] * b[i];
        const crd::f64 d = b[i] - ax(i);
        nr += d * d;
    }

    Row row;
    row.who = who;
    row.levels = static_cast<crd::i32>(m.num_levels());
    row.fill = static_cast<crd::f64>(m.factor_nnz()) / static_cast<crd::f64>(a.nnz());
    row.iters = static_cast<crd::i64>(res.iterations);
    row.factor_ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
    row.solve_ms = std::chrono::duration<crd::f64, std::milli>(t3 - t2).count();
    row.true_res = std::sqrt(nr / nb);
    row.converged = res.converged;
    return row;
}

// ---- Cerid: Smoothed-Aggregation AMG (v4k-a) + FGMRES(30) -------------------
// cycle = V (robust) / W (convection lever) / K (Krylov-accelerated, v4k-b). `who` labels.
static Row run_cerid_amg(const Csr& a, const crd::containers::Array<crd::f64>& b, crd::f64 rel_tol,
                         crd::hesap::amg::SaAmg<crd::f64>::Cycle cycle = crd::hesap::amg::SaAmg<crd::f64>::Cycle::V,
                         const char* who = "Cerid-AMG", bool smooth_prolongator = true, bool adaptive = false)
{
    const crd::u32 n = a.rows();
    hs::SparseLinearOp<crd::f64> op(a);

    crd::hesap::amg::SaAmg<crd::f64>::Options aopts;
    aopts.cycle = cycle;
    aopts.smooth_prolongator = smooth_prolongator;
    aopts.adaptive_candidate = adaptive;
    auto t0 = Clock::now();
    crd::hesap::amg::SaAmg<crd::f64> m(a, &g_alloc, aopts);
    auto t1 = Clock::now();

    hd::Vector<crd::f64> bx(&g_alloc, n);
    hd::Vector<crd::f64> x(&g_alloc, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bx(i) = b[i];
        x(i) = 0.0;
    }
    hi::IterativeOptions<crd::f64> opts;
    opts.rel_tol = rel_tol;
    opts.max_iter = 2000;
    hi::GmresWorkspace<crd::f64> ws(&g_alloc, n, 30);
    auto t2 = Clock::now();
    auto res = hi::fgmres<crd::f64>(op, &m, bx.span(), x.span(), opts, ws, &g_alloc);
    auto t3 = Clock::now();

    hd::Vector<crd::f64> ax(&g_alloc, n);
    (void)op.apply(x.span(), ax.span());
    crd::f64 nb = 0;
    crd::f64 nr = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        nb += b[i] * b[i];
        const crd::f64 d = b[i] - ax(i);
        nr += d * d;
    }

    Row row;
    row.who = who;
    row.levels = static_cast<crd::i32>(m.num_levels());
    row.fill = static_cast<crd::f64>(m.operator_complexity()) / static_cast<crd::f64>(a.nnz()); // operator complexity
    row.iters = static_cast<crd::i64>(res.iterations);
    row.factor_ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
    row.solve_ms = std::chrono::duration<crd::f64, std::milli>(t3 - t2).count();
    row.true_res = std::sqrt(nr / nb);
    row.converged = res.converged;
    return row;
}

static const char* g_ilupack_ordering = "metisn"; // overridable via argv[3] (v4j-3 experiment)

// ---- ILUPACK: DGNLAMG (multilevel inverse-based ILU) + its GMRES -------------
// The Dmat struct + DGNLAMG* C API demand raw `integer`/`double*`; the crd Array
// buffers feed them via .data() (crd::f64 == double, integer == ILUPACK's index type).
static Row run_ilupack(const Csr& a, const crd::containers::Array<crd::f64>& b, crd::f64 condest, crd::f64 droptol,
                       crd::f64 rel_tol)
{
    const crd::i32 n = static_cast<crd::i32>(a.rows());
    const crd::i32 nnz = static_cast<crd::i32>(a.nnz());

    // Build a 1-based CSR copy in ILUPACK's Dmat (kept alive through factor+solve).
    crd::containers::Array<integer> ia(&g_alloc), ja(&g_alloc); // `integer` = ILUPACK's index type (boundary)
    crd::containers::Array<crd::f64> av(&g_alloc);
    ia.resize(static_cast<crd::usize>(n) + 1);
    ja.resize(static_cast<crd::usize>(nnz));
    av.resize(static_cast<crd::usize>(nnz));
    const auto* rp = a.pattern().outer_ptr.data();
    const auto* ci = a.pattern().inner_idx.data();
    const auto* vv = a.values().values.data();
    for (crd::i32 i = 0; i <= n; ++i)
    {
        ia[static_cast<crd::usize>(i)] = static_cast<integer>(rp[i]) + 1;
    }
    for (crd::i32 q = 0; q < nnz; ++q)
    {
        ja[static_cast<crd::usize>(q)] = static_cast<integer>(ci[q]) + 1;
        av[static_cast<crd::usize>(q)] = vv[q];
    }

    Dmat A;
    A.nr = A.nc = n;
    A.nnz = nnz;
    A.ia = ia.data();
    A.ja = ja.data();
    A.a = av.data();
    A.isreal = 1;
    A.issingle = 0;
    A.issymmetric = 0;
    A.isdefinite = 0;
    A.ishermitian = 0;
    A.isskew = 0;

    DILUPACKparam param;
    DAMGlevelmat PRE;
    DGNLAMGinit(&A, &param);
    param.matching = 1;
    param.ordering = const_cast<char*>(g_ilupack_ordering);
    param.droptol = droptol;
    param.droptolS = 0.1 * droptol;
    param.condest = condest;
    param.restol = rel_tol;
    param.maxit = 2000;
    param.elbow = 10.0;
    param.nrestart = 30;

    Row row;
    row.who = "ILUPACK";

    auto t0 = Clock::now();
    crd::i32 ierr = static_cast<crd::i32>(DGNLAMGfactor(&A, &PRE, &param));
    auto t1 = Clock::now();
    if (ierr != 0)
    {
        std::printf("  ILUPACK   factor failed (ierr=%d)\n", ierr);
        row.levels = 0;
        row.fill = 0;
        row.iters = 0;
        row.factor_ms = 0;
        row.solve_ms = 0;
        row.true_res = 1e9;
        row.converged = false;
        return row;
    }

    crd::containers::Array<crd::f64> rhs(&g_alloc);
    crd::containers::Array<crd::f64> sol(&g_alloc);
    rhs.resize(static_cast<crd::usize>(n));
    sol.resize(static_cast<crd::usize>(n), 0.0);
    for (crd::i32 i = 0; i < n; ++i)
    {
        rhs[static_cast<crd::usize>(i)] = b[static_cast<crd::usize>(i)];
    }

    auto t2 = Clock::now();
    ierr = static_cast<crd::i32>(DGNLAMGsolver(&A, &PRE, &param, rhs.data(), sol.data()));
    auto t3 = Clock::now();

    // true residual on the original A (ILUPACK solved A x = rhs)
    crd::containers::Array<crd::f64> ax(&g_alloc);
    ax.resize(static_cast<crd::usize>(n), 0.0);
    for (crd::i32 i = 0; i < n; ++i)
    {
        crd::f64 s = 0.0;
        for (crd::i32 q = static_cast<crd::i32>(rp[i]); q < static_cast<crd::i32>(rp[i + 1]); ++q)
        {
            s += vv[q] * sol[static_cast<crd::usize>(ci[q])];
        }
        ax[static_cast<crd::usize>(i)] = s;
    }
    crd::f64 nb = 0;
    crd::f64 nr = 0;
    for (crd::i32 i = 0; i < n; ++i)
    {
        const crd::usize u = static_cast<crd::usize>(i);
        nb += b[u] * b[u];
        const crd::f64 d = b[u] - ax[u];
        nr += d * d;
    }

    row.levels = static_cast<crd::i32>(PRE.nlev);
    row.fill = param.elbow;                         // ILUPACK reports achieved fill relative to A
    row.iters = static_cast<crd::i64>(param.niter); // V2.4 iteration count (ipar[25] is stale)
    row.factor_ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();
    row.solve_ms = std::chrono::duration<crd::f64, std::milli>(t3 - t2).count();
    row.true_res = std::sqrt(nr / nb);
    row.converged = (ierr == 0);

    DGNLAMGdelete(&A, &PRE, &param);
    return row;
}

int main(int argc, char** argv)
{
    crd::jobs::init();

    const crd::f64 condest = (argc > 1) ? std::atof(argv[1]) : 5.0; // κ — inverse-factor bound
    if (argc > 3)
    {
        g_ilupack_ordering = argv[3];
    } // ILUPACK ordering (v4j-3 experiment)
    if (argc > 4)
    {
        g_restart = static_cast<crd::usize>(std::atoi(argv[4]));
    }                                                                // FGMRES restart
    const crd::f64 droptol = (argc > 2) ? std::atof(argv[2]) : 1e-2; // ε — inverse-based drop tolerance
    const crd::f64 rel_tol = 1e-8;                                   // matched stopping tolerance (≈ √eps)
    const crd::f64 eps = (argc > 5) ? std::atof(argv[5]) : 1e-2;     // anisotropy (argv[5])
    const crd::f64 beta = (argc > 6) ? std::atof(argv[6]) : 0.3;     // convection / nonsymmetry (argv[6])

    std::printf("hesap v4: SA-AMG / InverseBasedIlu vs ILUPACK V2.4  (anisotropic conv-diff, eps=%.0e beta=%.1f)\n",
                eps, beta);
    std::printf("  params: condest(kappa)=%.1f droptol=%.0e rel_tol=%.0e, FGMRES(30) / ILUPACK-GMRES(30)\n\n", condest,
                droptol, rel_tol);

    const crd::i32 grids[] = {50, 100, 150};
    for (crd::i32 gN : grids)
    {
        Csr a = build_cd2d(gN, eps, beta);
        const crd::u32 n = a.rows();
        crd::containers::Array<crd::f64> b(&g_alloc);
        b.resize(n, 1.0);
        std::printf("cd2d %dx%d  (n=%u, nnz=%llu):\n", gN, gN, n, static_cast<unsigned long long>(a.nnz()));
        using AmgCycle = crd::hesap::amg::SaAmg<crd::f64>::Cycle;
        const Row r_plain = run_cerid(a, b, condest, droptol, rel_tol);
        print_row(r_plain);
        // v4j-3(a): PER-LEVEL AMD reorder inside InverseBasedIlu (each Schur reorders ⇒ deepest hierarchy).
        // This is the apples-to-apples nonsym config — ILUPACK always reorders too.
        const Row r_rord = run_cerid(a, b, condest, droptol, rel_tol, 0.0, "Cerid-IB-rord", /*reorder=*/true);
        print_row(r_rord);
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::V, "Cerid-AMG-V"));
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::W, "Cerid-AMG-W"));
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::K, "Cerid-AMG-K"));
        // AGMG-style: plain (unsmoothed) aggregation + K-cycle (Notay's convection recipe).
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::K, "Cerid-AGMG-K", /*smooth=*/false));
        // αSA: adaptive near-nullspace candidate + smoothed-K (v4k-e, the β=0.3 attempt).
        print_row(run_cerid_amg(a, b, rel_tol, AmgCycle::K, "Cerid-aSA-K", /*smooth=*/true, /*adaptive=*/true));
        const Row r_ilu = run_ilupack(a, b, condest, droptol, rel_tol);
        print_row(r_ilu);
        // v4z Step 1: quantify the factor-vs-solve trade against ILUPACK (the apples-to-apples ref).
        print_breakeven(r_rord, r_ilu);  // reorder config = the recommended nonsym path
        print_breakeven(r_plain, r_ilu); // plain config (no reorder) for contrast
        std::printf("\n");
    }

    crd::jobs::shutdown();
    return 0;
}
