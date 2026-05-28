// bench_hesap_ilu_vs_reference.cpp -- Phase 3.1.6 v4g.
//
// IC(0)-PCG vs Eigen ConjugateGradient + IncompleteCholesky on real SuiteSparse SPD,
// and ILU(0)-BiCGSTAB vs Eigen BiCGSTAB + IncompleteLUT on real SuiteSparse nonsym.
// Cerid's operator is the parallel SELL spmv (the DRAM-bound win); the incomplete-
// factorization triangular solves are sequential on both sides. The preconditioner
// FACTORISATION (Cerid ctor / Eigen .compute()) is hoisted OUT of the timed region.
// Iteration counts WILL differ (Cerid pure level-0 vs Eigen's shifted-IC / dual-
// threshold ILUT) -- the honest comparison is wall time at equal accuracy, both
// iteration counts reported.
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).
//
// crd conventions throughout: crd::containers::Array (never std::vector),
// crd::platform::fs::read_file_text + manual parse (never std::ifstream/string),
// a named TlsfAllocator (never malloc), crd::f64/usize/i32/i64 (never raw
// double/int/size_t). Raw double survives ONLY at the Eigen C++ API boundary
// (Eigen::SparseMatrix<double>/VectorXd/Triplet<double>); crd::f64 IS double.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/ic0.hpp>
#include <crd/hesap/preconditioners/ilu0.hpp>
#include <crd/hesap/preconditioners/ilup.hpp>
#include <crd/hesap/preconditioners/ilut.hpp>
#include <crd/hesap/preconditioners/inverse_based_ilu.hpp> // v4z Step 2: reorder-default evidence
#include <crd/hesap/preconditioners/reordered.hpp>         // AMD-reordered wrapper (matches Eigen IncompleteLUT)
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib> // strtol / strtod
#include <cstring> // strncmp
#include <unsupported/Eigen/IterativeSolvers>

#ifndef CRD_SUITESPARSE_DIR
#define CRD_SUITESPARSE_DIR "."
#endif

namespace hs = crd::hesap::sparse;
namespace hi = crd::hesap::iterative;
namespace hd = crd::hesap::dense;
namespace hp = crd::hesap::preconditioners;
namespace fs = crd::platform::fs;
using Clock = std::chrono::steady_clock;

static crd::memory::TlsfAllocator g_alloc{crd::usize{1} << 30}; // 1 GiB pool — named allocator, never malloc

struct Trip
{
    crd::i32 r, c;
    crd::f64 v;
};
struct Mtx
{
    crd::i32 n = 0;
    crd::containers::Array<Trip> trips{&g_alloc};
    bool ok = false;
};

// Matrix-Market reader, crd-native: read the whole file into a crd::String via the
// platform filesystem, then hand-parse with strtol/strtod (no std::ifstream / std::string
// line buffers / std::stringstream). DATA lands in crd::containers::Array<Trip>.
static Mtx read_mtx(const char* path)
{
    Mtx m;
    crd::containers::String text(&g_alloc);
    if (!fs::read_file_text(fs::Path{path}, text))
    {
        return m;
    }
    const char* p = text.c_str();
    const char* end = p + text.size();
    bool sym = false;
    bool dims_read = false;
    crd::i32 rows = 0;
    crd::i32 nnz = 0;
    crd::i32 seen = 0;
    while (p < end)
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        {
            ++p;
        }
        if (p >= end)
        {
            break;
        }
        if (*p == '%') // comment / banner line
        {
            const char* eol = p;
            while (eol < end && *eol != '\n')
            {
                ++eol;
            }
            if (!dims_read)
            {
                for (const char* q = p; q + 9 <= eol; ++q)
                {
                    if (std::strncmp(q, "symmetric", 9) == 0)
                    {
                        sym = true;
                        break;
                    }
                }
            }
            p = eol;
            continue;
        }
        char* np = nullptr;
        if (!dims_read)
        {
            rows = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            [[maybe_unused]] const crd::i32 cols = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            nnz = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            m.n = rows;
            m.trips.reserve(static_cast<crd::usize>(nnz) * (sym ? 2 : 1));
            dims_read = true;
            continue;
        }
        const crd::i32 i = static_cast<crd::i32>(std::strtol(p, &np, 10)) - 1;
        p = np;
        const crd::i32 j = static_cast<crd::i32>(std::strtol(p, &np, 10)) - 1;
        p = np;
        const crd::f64 v = std::strtod(p, &np);
        p = np;
        m.trips.push_back({i, j, v});
        if (sym && i != j)
        {
            m.trips.push_back({j, i, v});
        }
        if (++seen >= nnz)
        {
            break;
        }
    }
    m.ok = (rows > 0);
    return m;
}

// Build the full SuiteSparse path "<dir>/<name>/<name>.mtx" with a crd::String (no std::string).
static Mtx load_ss(const char* name)
{
    crd::containers::String p(&g_alloc);
    p.append(CRD_SUITESPARSE_DIR);
    p.append("/");
    p.append(name);
    p.append("/");
    p.append(name);
    p.append(".mtx");
    return read_mtx(p.c_str());
}

template <typename Fn> static crd::f64 best_ms(Fn&& fn, crd::i32 reps = 3)
{
    fn();
    crd::f64 best = 1e30;
    for (crd::i32 r = 0; r < reps; ++r)
    {
        const auto t0 = Clock::now();
        fn();
        crd::jobs::frame_reset();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

static hs::SparseMatrix<crd::f64, hs::SparseFormat::Csr> to_csr(const Mtx& mtx)
{
    hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(mtx.n), static_cast<crd::u32>(mtx.n));
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    return tb.compress();
}

static Eigen::SparseMatrix<double> to_eigen(const Mtx& mtx)
{
    crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        et.push_back(Eigen::Triplet<double>(t.r, t.c, t.v));
    }
    Eigen::SparseMatrix<double> ea(mtx.n, mtx.n);
    ea.setFromTriplets(et.data(), et.data() + et.size());
    return ea;
}

// TRUE relative residual ‖b − A·x‖₂/‖b‖₂ via the operator -- apples-to-apples with Eigen's
// (b − A·x).norm()/b.norm(). The Krylov recurrence residual drifts BELOW the true residual on
// ill-conditioned A, so the FAIR comparison drives Cerid to a tight rel_tol and measures the
// TRUE residual on both sides (matched accuracy). See feedback_iterative_bench_matched_true_residual.
static crd::f64 true_resid(hs::ParallelSparseLinearOp<crd::f64>& op, const crd::containers::Array<crd::f64>& Bv,
                           const hd::Vector<crd::f64>& x, crd::i32 n)
{
    hd::Vector<crd::f64> ax(&g_alloc, static_cast<crd::usize>(n));
    (void)op.apply(x.span(), ax.span());
    crd::jobs::frame_reset();
    crd::f64 nb = 0;
    crd::f64 nr = 0;
    for (crd::i32 k = 0; k < n; ++k)
    {
        const crd::f64 bk = Bv[static_cast<crd::usize>(k)];
        nb += bk * bk;
        const crd::f64 rk = bk - ax(static_cast<crd::usize>(k));
        nr += rk * rk;
    }
    return nb > 0 ? std::sqrt(nr / nb) : std::sqrt(nr);
}

// Large 2D convection-diffusion 5-point (n = g²), nonsymmetric. The large-n + dense-ILUT-
// factor regime where the level-scheduled parallel triangular solve pays (wide wavefront
// levels, n ≥ 8192 ⇒ per-level work dominates the barrier cost).
static Mtx make_conv_diff_2d(crd::i32 g, crd::f64 beta)
{
    Mtx m;
    m.n = g * g;
    m.ok = true;
    for (crd::i32 y = 0; y < g; ++y)
    {
        for (crd::i32 x = 0; x < g; ++x)
        {
            const crd::i32 i = y * g + x;
            m.trips.push_back({i, i, 4.0});
            if (x + 1 < g)
            {
                m.trips.push_back({i, i + 1, -1.0 + beta});
            }
            if (x > 0)
            {
                m.trips.push_back({i, i - 1, -1.0 - beta});
            }
            if (y + 1 < g)
            {
                m.trips.push_back({i, i + g, -1.0 + beta});
            }
            if (y > 0)
            {
                m.trips.push_back({i, i - g, -1.0 - beta});
            }
        }
    }
    return m;
}

// IC(0)-PCG vs Eigen CG + IncompleteCholesky (SPD).
static void run_ic(const char* name, const Mtx& mtx)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 ctol = 1e-9;
    const crd::f64 etol = 1e-8;
    const crd::i32 cap = 8000;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    hp::Ic0Preconditioner<crd::f64> ic(a,
                                       &g_alloc); // factorisation hoisted OUT of the timed solve (= Eigen .compute())
    crd::containers::Array<crd::f64> Bv(&g_alloc);
    Bv.resize(static_cast<crd::usize>(n), 1.0);

    crd::usize cit = 0;
    crd::f64 cres = 0;
    const crd::f64 t_c = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n));
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            for (crd::i32 k = 0; k < n; ++k)
            {
                b(static_cast<crd::usize>(k)) = Bv[static_cast<crd::usize>(k)];
            }
            hi::IterativeOptions<crd::f64> opts;
            opts.rel_tol = ctol;
            opts.max_iter = static_cast<crd::usize>(cap);
            hi::KrylovWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
            auto r = hi::pcg<crd::f64>(op, ic, b.span(), x.span(), opts, ws, &g_alloc);
            cit = r.iterations;
            cres = true_resid(op, Bv, x, n);
        });

    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    crd::i64 eit = 0;
    crd::f64 eres = 0;
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                             Eigen::IncompleteCholesky<double>>
        cg;
    cg.setMaxIterations(cap);
    cg.setTolerance(etol);
    cg.compute(ea);
    const crd::f64 t_e = best_ms(
        [&]()
        {
            Eigen::VectorXd b(n);
            for (crd::i32 k = 0; k < n; ++k)
            {
                b(k) = Bv[static_cast<crd::usize>(k)];
            }
            Eigen::VectorXd x = cg.solve(b);
            eit = static_cast<crd::i64>(cg.iterations());
            eres = (b - ea * x).norm() / b.norm();
        });

    const crd::f64 mc = cit > 0 ? t_c / static_cast<crd::f64>(cit) : 0.0;
    const crd::f64 me = eit > 0 ? t_e / static_cast<crd::f64>(eit) : 0.0;
    std::printf("  %-10s n=%-6d nnz=%-8zu  [IC(0)-PCG vs Eigen IncompleteCholesky-CG; matched TRUE residual]\n", name,
                n, a.nnz());
    std::printf("    Cerid IC(0)-PCG  : %5zu it %8.2f ms (true r=%.1e, %.4f ms/it)  [shift=%.1e, Eigen/Cerid = %.2fx "
                "wall, %.2fx per-it]\n",
                cit, t_c, cres, mc, ic.shift(), t_c > 0 ? t_e / t_c : 0.0, mc > 0 ? me / mc : 0.0);
    std::printf("    Eigen IChol-CG   : %5lld it %8.2f ms (true r=%.1e, %.4f ms/it)\n", static_cast<long long>(eit),
                t_e, eres, me);
}

// Helper: BiCGSTAB with a Cerid preconditioner; returns iters + residual + wall time.
template <typename Prec>
static crd::f64 cerid_bicg(hs::ParallelSparseLinearOp<crd::f64>& op, const Prec& m,
                           const crd::containers::Array<crd::f64>& Bv, crd::i32 n, crd::f64 tol, crd::i32 cap,
                           crd::usize& it_out, crd::f64& res_out)
{
    return best_ms(
        [&]()
        {
            hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n));
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            for (crd::i32 k = 0; k < n; ++k)
            {
                b(static_cast<crd::usize>(k)) = Bv[static_cast<crd::usize>(k)];
            }
            hi::IterativeOptions<crd::f64> opts;
            opts.rel_tol = tol;
            opts.max_iter = static_cast<crd::usize>(cap);
            hi::BicgstabWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
            auto r = hi::bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
            it_out = r.iterations;
            res_out = true_resid(op, Bv, x, n);
        });
}

// ILU(0) + ILUT (Cerid) vs Eigen IncompleteLUT, all through BiCGSTAB (nonsym). ILUT is the
// apples-to-apples peer of Eigen's IncompleteLUT (both dual-threshold), at COMPARABLE fill.
static void run_ilu(const char* name, const Mtx& mtx)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 ctol = 1e-9;
    const crd::f64 etol = 1e-8;
    const crd::i32 cap = 8000;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    crd::containers::Array<crd::f64> Bv(&g_alloc);
    Bv.resize(static_cast<crd::usize>(n), 1.0);
    const crd::u32 avg = n > 0 ? static_cast<crd::u32>(a.nnz() / static_cast<crd::usize>(n)) : 0U;
    const crd::u32 fillf = 10U;             // Eigen-style fill factor (× avg row nnz)
    const crd::u32 lfil = fillf * avg + 5U; // Cerid keeps lfil per L/U row ≈ Eigen fillfactor·avg
    const crd::f64 droptol = 1e-4;          // ILUT relative drop tol (row-scaled ⇒ scale-invariant)

    hp::Ilu0Preconditioner<crd::f64> ilu0(a, &g_alloc);                // level-0 (reference)
    hp::IlutPreconditioner<crd::f64> ilut(a, &g_alloc, lfil, droptol); // structure-preserving (Cerid default)
    // AMD-reordered ILUT -- matches Eigen IncompleteLUT (which AMD-reorders Aᵀ+A internally).
    // Regime-dependent: AMD shrinks fill on small/irregular matrices but scrambles the banded
    // structure the parallel level-scheduled triangular solve exploits on large structured ones.
    hp::ReorderedPreconditioner<crd::f64, hp::IlutPreconditioner<crd::f64>> ilutr(a, &g_alloc, lfil, droptol);
    crd::usize it0 = 0;
    crd::usize itt = 0;
    crd::usize itr = 0;
    crd::f64 r0 = 0;
    crd::f64 rt = 0;
    crd::f64 rr = 0;
    const crd::f64 t0 = cerid_bicg(op, ilu0, Bv, n, ctol, cap, it0, r0);
    const crd::f64 tt = cerid_bicg(op, ilut, Bv, n, ctol, cap, itt, rt);
    const crd::f64 tr = cerid_bicg(op, ilutr, Bv, n, ctol, cap, itr, rr);

    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    crd::i64 eit = 0;
    crd::f64 eres = 0;
    Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::IncompleteLUT<double>> bs;
    bs.preconditioner().setDroptol(droptol);
    bs.preconditioner().setFillfactor(static_cast<int>(fillf)); // Eigen fillfactor = multiplier on avg row nnz
    bs.setMaxIterations(cap);
    bs.setTolerance(etol);
    bs.compute(ea);
    const crd::f64 t_e = best_ms(
        [&]()
        {
            Eigen::VectorXd b(n);
            for (crd::i32 k = 0; k < n; ++k)
            {
                b(k) = Bv[static_cast<crd::usize>(k)];
            }
            Eigen::VectorXd x = bs.solve(b);
            eit = static_cast<crd::i64>(bs.iterations());
            eres = (b - ea * x).norm() / b.norm();
        });

    std::printf("  %-10s n=%-6d nnz=%-8zu  [Cerid ILUT (default + AMD) vs Eigen IncompleteLUT (AMD always); BiCGSTAB; "
                "matched TRUE residual; lfil=%u]\n",
                name, n, a.nnz(), lfil);
    std::printf("    Cerid ILU0-BiCG    : %5zu it %8.2f ms (true r=%.1e)\n", it0, t0, r0);
    std::printf("    Cerid ILUT default : %5zu it %8.2f ms (true r=%.1e)  [Eigen/Cerid = %.2fx wall]\n", itt, tt, rt,
                tt > 0 ? t_e / tt : 0.0);
    std::printf("    Cerid ILUT +AMD    : %5zu it %8.2f ms (true r=%.1e)  [Eigen/Cerid = %.2fx wall]\n", itr, tr, rr,
                tr > 0 ? t_e / tr : 0.0);
    std::printf("    Eigen ILUT-BiCG    : %5lld it %8.2f ms (true r=%.1e)\n", static_cast<long long>(eit), t_e, eres);
}

// ILU(p) level-of-fill, p = 0,1,2 (Cerid) vs Eigen IncompleteLUT, all through FGMRES.
// Shows the level-of-fill value-add (fill ratio + iteration count vs p) and the breadth
// (Eigen ships no ILU(p)). Comparable fill: Eigen fillfactor ≈ ILU(2)'s fill / nnz.
static void run_ilup(const char* name, const Mtx& mtx)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 ctol = 1e-9;
    const crd::f64 etol = 1e-8;
    const crd::i32 cap = 8000;
    const crd::i32 restart = 60;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    crd::containers::Array<crd::f64> Bv(&g_alloc);
    Bv.resize(static_cast<crd::usize>(n), 1.0);

    std::printf(
        "  %-10s n=%-6d nnz=%-8zu  [Cerid ILU(p) p=0,1,2 vs Eigen IncompleteLUT; FGMRES(%d); matched TRUE residual]\n",
        name, n, a.nnz(), restart);
    crd::usize fill2 = 0;
    for (crd::u32 p = 0; p <= 2; ++p)
    {
        hp::IlupPreconditioner<crd::f64> m(a, &g_alloc, p);
        crd::usize it = 0;
        crd::f64 res = 0;
        const crd::f64 t = best_ms(
            [&]()
            {
                hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n));
                hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
                for (crd::i32 k = 0; k < n; ++k)
                {
                    b(static_cast<crd::usize>(k)) = Bv[static_cast<crd::usize>(k)];
                }
                hi::IterativeOptions<crd::f64> opts;
                opts.rel_tol = ctol;
                opts.max_iter = static_cast<crd::usize>(cap);
                hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(restart));
                auto r = hi::fgmres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
                it = r.iterations;
                res = true_resid(op, Bv, x, n);
            });
        if (p == 2)
        {
            fill2 = m.factor_nnz();
        }
        std::printf("    Cerid ILU(%u)     : %5zu it %8.2f ms (true r=%.1e)  fill=%.2fx\n", p, it, t, res,
                    static_cast<crd::f64>(m.factor_nnz()) / static_cast<crd::f64>(a.nnz()));
    }
    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    const crd::f64 fillf = a.nnz() > 0 ? static_cast<crd::f64>(fill2) / static_cast<crd::f64>(a.nnz()) : 1.0;
    Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::IncompleteLUT<double>> g;
    g.set_restart(restart);
    g.preconditioner().setDroptol(1e-4);
    g.preconditioner().setFillfactor(static_cast<int>(fillf + 1.0));
    g.setMaxIterations(cap);
    g.setTolerance(etol);
    g.compute(ea);
    crd::i64 eit = 0;
    crd::f64 eres = 0;
    const crd::f64 te = best_ms(
        [&]()
        {
            Eigen::VectorXd b(n);
            for (crd::i32 k = 0; k < n; ++k)
            {
                b(k) = Bv[static_cast<crd::usize>(k)];
            }
            Eigen::VectorXd x = g.solve(b);
            eit = static_cast<crd::i64>(g.iterations());
            eres = (b - ea * x).norm() / b.norm();
        });
    std::printf("    Eigen ILUT-GMRES : %5lld it %8.2f ms (r=%.1e)  fill≈%.2fx (matched to ILU(2))\n",
                static_cast<long long>(eit), te, eres, fillf);
}

// v4z Step 2 evidence: InverseBasedIlu reorder OFF vs ON on real non-CFD nonsym
// matrices. The reorder-default decision hinges on this NOT regressing AWAY from
// convection (where per-level AMD reorder clearly wins — cd2d β=0.3 iters halve,
// break-even 1.1→3.6 re-solves, single-shot still wins). reorder is a Cerid knob
// with no Eigen peer ⇒ pure Cerid-vs-Cerid. κ=5 / droptol=1e-2 matches the cd2d
// apples-to-apples run; the OP is identical both ways so the comparison is fair.
static void run_mlilu_reorder(const char* name, const Mtx& mtx, crd::f64 kappa = 5.0)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 rel_tol = 1e-8;
    const crd::i32 cap = 2000;
    const crd::i32 restart = 30;
    const crd::f64 droptol = 1e-2;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    crd::containers::Array<crd::f64> Bv(&g_alloc);
    Bv.resize(static_cast<crd::usize>(n), 1.0);

    std::printf("  %-10s n=%-6d nnz=%-8zu  [InverseBasedIlu reorder OFF vs ON; FGMRES(%d); kappa=%.1f droptol=%.0e]\n",
                name, n, a.nnz(), restart, kappa, droptol);
    auto run = [&](bool reorder, const char* tag)
    {
        const auto tf0 = Clock::now();
        hp::InverseBasedIlu<crd::f64> m(a, &g_alloc, kappa, droptol, 0U, 50U, 64U, hp::Mc64Mode::None, 0.0, reorder);
        const auto tf1 = Clock::now();
        const crd::f64 tf = std::chrono::duration<crd::f64, std::milli>(tf1 - tf0).count();
        crd::usize it = 0;
        crd::f64 res = 0;
        const crd::f64 ts = best_ms(
            [&]()
            {
                hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n));
                hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
                for (crd::i32 k = 0; k < n; ++k)
                {
                    b(static_cast<crd::usize>(k)) = Bv[static_cast<crd::usize>(k)];
                }
                hi::IterativeOptions<crd::f64> opts;
                opts.rel_tol = rel_tol;
                opts.max_iter = static_cast<crd::usize>(cap);
                hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(restart));
                auto r = hi::fgmres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
                it = r.iterations;
                res = true_resid(op, Bv, x, n);
            });
        std::printf("    reorder=%-3s : %5zu it  factor %8.2f ms  solve %8.2f ms  levels=%d fill=%.2fx (true r=%.1e)\n",
                    tag, it, tf, ts, static_cast<crd::i32>(m.num_levels()),
                    static_cast<crd::f64>(m.factor_nnz()) / static_cast<crd::f64>(a.nnz()), res);
    };
    run(false, "off");
    run(true, "on");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap IC(0)/ILU(0) preconditioners vs Eigen IncompleteCholesky / IncompleteLUT (real SuiteSparse)\n");
    std::printf("  Cerid pure level-0 + parallel SELL spmv; Eigen shifted-IC / dual-threshold ILUT. Both iter counts "
                "reported.\n");
    std::printf(" --- SPD: IC(0) ---\n");
    run_ic("bcsstk13", load_ss("bcsstk13"));
    run_ic("bcsstk24", load_ss("bcsstk24"));
    run_ic("bcsstk25", load_ss("bcsstk25"));
    std::printf(" --- nonsym: ILU(0)/ILUT (small SuiteSparse: serial tri-solve regime) ---\n");
    run_ilu("gemat11", load_ss("gemat11"));
    run_ilu("sherman3", load_ss("sherman3"));
    std::printf(" --- nonsym: large 2D conv-diff (n=40000: level-scheduled parallel tri-solve regime) ---\n");
    run_ilu("cd2d-200", make_conv_diff_2d(200, 0.3));
    std::printf(" --- nonsym: ILU(p) level-of-fill (p=0,1,2) vs Eigen IncompleteLUT ---\n");
    run_ilup("cd2d-100", make_conv_diff_2d(100, 0.3));
    run_ilup("sherman3", load_ss("sherman3"));
    std::printf(" --- v4z Step 2: InverseBasedIlu reorder OFF vs ON (reorder-default regression check) ---\n");
    // sherman3 + cd2d-150 first (clean operating-regime signal); gemat11 last — it is a
    // power-circuit matrix (no PDE structure) and can run away / hit a singular dense leaf.
    run_mlilu_reorder("sherman3", load_ss("sherman3"));
    run_mlilu_reorder("cd2d-150", make_conv_diff_2d(150, 0.3));   // convection control (reorder should win here)
    run_mlilu_reorder("gemat11-k100", load_ss("gemat11"), 100.0); // κ=100 wrong-tool diagnostic
    run_mlilu_reorder("gemat11", load_ss("gemat11"));             // κ=5 (degrades gracefully now — last)
    crd::jobs::shutdown();
    return 0;
}
