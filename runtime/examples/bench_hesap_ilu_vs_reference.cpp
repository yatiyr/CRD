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

#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/ic0.hpp>
#include <crd/hesap/preconditioners/ilu0.hpp>
#include <crd/hesap/preconditioners/ilup.hpp>
#include <crd/hesap/preconditioners/ilut.hpp>
#include <crd/hesap/preconditioners/reordered.hpp> // AMD-reordered wrapper (matches Eigen IncompleteLUT)
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>
#include <unsupported/Eigen/IterativeSolvers>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CRD_SUITESPARSE_DIR
#define CRD_SUITESPARSE_DIR "."
#endif

namespace hs = crd::hesap::sparse;
namespace hi = crd::hesap::iterative;
namespace hd = crd::hesap::dense;
namespace hp = crd::hesap::preconditioners;
using Clock  = std::chrono::steady_clock;

static crd::memory::MallocAllocator g_alloc;

struct Trip { int r, c; double v; };
struct Mtx { int n = 0; std::vector<Trip> trips; bool ok = false; };

static Mtx read_mtx(const std::string& path)
{
    Mtx           m;
    std::ifstream f(path);
    if (!f) { return m; }
    std::string line;
    bool        header = false, sym = false;
    int         rows = 0, cols = 0, nnz = 0, seen = 0;
    while (std::getline(f, line))
    {
        if (line.empty()) { continue; }
        if (line[0] == '%')
        {
            if (!header && line.find("symmetric") != std::string::npos) { sym = true; }
            header = true;
            continue;
        }
        std::istringstream ss(line);
        if (rows == 0) { ss >> rows >> cols >> nnz; m.n = rows; m.trips.reserve(static_cast<std::size_t>(nnz) * (sym ? 2 : 1)); continue; }
        int i, j; double v; ss >> i >> j >> v; --i; --j;
        m.trips.push_back({i, j, v});
        if (sym && i != j) { m.trips.push_back({j, i, v}); }
        if (++seen >= nnz) { break; }
    }
    m.ok = (rows > 0);
    return m;
}

template <typename Fn>
static double best_ms(Fn&& fn, int reps = 3)
{
    fn();
    double best = 1e30;
    for (int r = 0; r < reps; ++r)
    {
        const auto t0 = Clock::now();
        fn();
        crd::jobs::frame_reset();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

static hs::SparseMatrix<double, hs::SparseFormat::Csr> to_csr(const Mtx& mtx)
{
    hs::TripletBuilder<double> tb(&g_alloc, static_cast<crd::u32>(mtx.n), static_cast<crd::u32>(mtx.n));
    for (const Trip& t : mtx.trips) { tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v); }
    return tb.compress();
}

static Eigen::SparseMatrix<double> to_eigen(const Mtx& mtx)
{
    std::vector<Eigen::Triplet<double>> et; et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips) { et.emplace_back(t.r, t.c, t.v); }
    Eigen::SparseMatrix<double> ea(mtx.n, mtx.n); ea.setFromTriplets(et.begin(), et.end());
    return ea;
}

// TRUE relative residual ‖b − A·x‖₂/‖b‖₂ via the operator -- apples-to-apples with Eigen's
// (b − A·x).norm()/b.norm(). The Krylov recurrence residual drifts BELOW the true residual on
// ill-conditioned A, so the FAIR comparison drives Cerid to a tight rel_tol and measures the
// TRUE residual on both sides (matched accuracy). See feedback_iterative_bench_matched_true_residual.
static double true_resid(hs::ParallelSparseLinearOp<double>& op, const std::vector<double>& Bv,
                         const hd::Vector<double>& x, int n)
{
    hd::Vector<double> ax(&g_alloc, static_cast<crd::usize>(n));
    (void)op.apply(x.span(), ax.span());
    crd::jobs::frame_reset();
    double nb = 0, nr = 0;
    for (int k = 0; k < n; ++k)
    {
        const double bk = Bv[static_cast<std::size_t>(k)];
        nb += bk * bk;
        const double rk = bk - ax(static_cast<crd::usize>(k));
        nr += rk * rk;
    }
    return nb > 0 ? std::sqrt(nr / nb) : std::sqrt(nr);
}

// Large 2D convection-diffusion 5-point (n = g²), nonsymmetric. The large-n + dense-ILUT-
// factor regime where the level-scheduled parallel triangular solve pays (wide wavefront
// levels, n ≥ 8192 ⇒ per-level work dominates the barrier cost).
static Mtx make_conv_diff_2d(int g, double beta)
{
    Mtx m; m.n = g * g; m.ok = true;
    for (int y = 0; y < g; ++y)
    {
        for (int x = 0; x < g; ++x)
        {
            const int i = y * g + x;
            m.trips.push_back({i, i, 4.0});
            if (x + 1 < g) { m.trips.push_back({i, i + 1, -1.0 + beta}); }
            if (x > 0)     { m.trips.push_back({i, i - 1, -1.0 - beta}); }
            if (y + 1 < g) { m.trips.push_back({i, i + g, -1.0 + beta}); }
            if (y > 0)     { m.trips.push_back({i, i - g, -1.0 - beta}); }
        }
    }
    return m;
}

// IC(0)-PCG vs Eigen CG + IncompleteCholesky (SPD).
static void run_ic(const char* name, const Mtx& mtx)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int n = mtx.n; const double ctol = 1e-9; const double etol = 1e-8; const int cap = 8000;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    hp::Ic0Preconditioner<double>      ic(a, &g_alloc); // factorisation hoisted OUT of the timed solve (= Eigen .compute())
    std::vector<double> Bv(n, 1.0);

    crd::usize cit = 0; double cres = 0;
    const double t_c = best_ms([&]() {
        hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n)), x(&g_alloc, static_cast<crd::usize>(n));
        for (int k = 0; k < n; ++k) { b(static_cast<crd::usize>(k)) = Bv[static_cast<std::size_t>(k)]; }
        hi::IterativeOptions<double> opts; opts.rel_tol = ctol; opts.max_iter = static_cast<crd::usize>(cap);
        hi::KrylovWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
        auto r = hi::pcg<double>(op, ic, b.span(), x.span(), opts, ws, &g_alloc);
        cit = r.iterations; cres = true_resid(op, Bv, x, n);
    });

    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    long eit = 0; double eres = 0;
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper, Eigen::IncompleteCholesky<double>> cg;
    cg.setMaxIterations(cap); cg.setTolerance(etol); cg.compute(ea);
    const double t_e = best_ms([&]() {
        Eigen::VectorXd b(n); for (int k = 0; k < n; ++k) { b(k) = Bv[static_cast<std::size_t>(k)]; }
        Eigen::VectorXd x = cg.solve(b);
        eit = static_cast<long>(cg.iterations()); eres = (b - ea * x).norm() / b.norm();
    });

    const double mc = cit > 0 ? t_c / static_cast<double>(cit) : 0.0;
    const double me = eit > 0 ? t_e / static_cast<double>(eit) : 0.0;
    std::printf("  %-10s n=%-6d nnz=%-8zu  [IC(0)-PCG vs Eigen IncompleteCholesky-CG; matched TRUE residual]\n", name, n, a.nnz());
    std::printf("    Cerid IC(0)-PCG  : %5zu it %8.2f ms (true r=%.1e, %.4f ms/it)  [shift=%.1e, Eigen/Cerid = %.2fx wall, %.2fx per-it]\n",
                cit, t_c, cres, mc, ic.shift(), t_c > 0 ? t_e / t_c : 0.0, mc > 0 ? me / mc : 0.0);
    std::printf("    Eigen IChol-CG   : %5ld it %8.2f ms (true r=%.1e, %.4f ms/it)\n", eit, t_e, eres, me);
}

// Helper: BiCGSTAB with a Cerid preconditioner; returns iters + residual + wall time.
template <typename Prec>
static double cerid_bicg(hs::ParallelSparseLinearOp<double>& op, const Prec& m, const std::vector<double>& Bv,
                         int n, double tol, int cap, crd::usize& it_out, double& res_out)
{
    return best_ms([&]() {
        hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n)), x(&g_alloc, static_cast<crd::usize>(n));
        for (int k = 0; k < n; ++k) { b(static_cast<crd::usize>(k)) = Bv[static_cast<std::size_t>(k)]; }
        hi::IterativeOptions<double> opts; opts.rel_tol = tol; opts.max_iter = static_cast<crd::usize>(cap);
        hi::BicgstabWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
        auto r = hi::bicgstab<double>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
        it_out = r.iterations; res_out = true_resid(op, Bv, x, n);
    });
}

// ILU(0) + ILUT (Cerid) vs Eigen IncompleteLUT, all through BiCGSTAB (nonsym). ILUT is the
// apples-to-apples peer of Eigen's IncompleteLUT (both dual-threshold), at COMPARABLE fill.
static void run_ilu(const char* name, const Mtx& mtx)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int n = mtx.n; const double ctol = 1e-9; const double etol = 1e-8; const int cap = 8000;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    std::vector<double> Bv(n, 1.0);
    const crd::u32 avg     = n > 0 ? static_cast<crd::u32>(a.nnz() / static_cast<crd::usize>(n)) : 0U;
    const crd::u32 fillf   = 10U;            // Eigen-style fill factor (× avg row nnz)
    const crd::u32 lfil    = fillf * avg + 5U; // Cerid keeps lfil per L/U row ≈ Eigen fillfactor·avg
    const double   droptol = 1e-4;           // ILUT relative drop tol (row-scaled ⇒ scale-invariant)

    hp::Ilu0Preconditioner<double> ilu0(a, &g_alloc);                // level-0 (reference)
    hp::IlutPreconditioner<double> ilut(a, &g_alloc, lfil, droptol); // structure-preserving (Cerid default)
    // AMD-reordered ILUT -- matches Eigen IncompleteLUT (which AMD-reorders Aᵀ+A internally).
    // Regime-dependent: AMD shrinks fill on small/irregular matrices but scrambles the banded
    // structure the parallel level-scheduled triangular solve exploits on large structured ones.
    hp::ReorderedPreconditioner<double, hp::IlutPreconditioner<double>> ilutr(a, &g_alloc, lfil, droptol);
    crd::usize it0 = 0, itt = 0, itr = 0; double r0 = 0, rt = 0, rr = 0;
    const double t0 = cerid_bicg(op, ilu0, Bv, n, ctol, cap, it0, r0);
    const double tt = cerid_bicg(op, ilut, Bv, n, ctol, cap, itt, rt);
    const double tr = cerid_bicg(op, ilutr, Bv, n, ctol, cap, itr, rr);

    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    long eit = 0; double eres = 0;
    Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::IncompleteLUT<double>> bs;
    bs.preconditioner().setDroptol(droptol);
    bs.preconditioner().setFillfactor(static_cast<int>(fillf)); // Eigen fillfactor = multiplier on avg row nnz
    bs.setMaxIterations(cap); bs.setTolerance(etol); bs.compute(ea);
    const double t_e = best_ms([&]() {
        Eigen::VectorXd b(n); for (int k = 0; k < n; ++k) { b(k) = Bv[static_cast<std::size_t>(k)]; }
        Eigen::VectorXd x = bs.solve(b);
        eit = static_cast<long>(bs.iterations()); eres = (b - ea * x).norm() / b.norm();
    });

    std::printf("  %-10s n=%-6d nnz=%-8zu  [Cerid ILUT (default + AMD) vs Eigen IncompleteLUT (AMD always); BiCGSTAB; matched TRUE residual; lfil=%u]\n",
                name, n, a.nnz(), lfil);
    std::printf("    Cerid ILU0-BiCG    : %5zu it %8.2f ms (true r=%.1e)\n", it0, t0, r0);
    std::printf("    Cerid ILUT default : %5zu it %8.2f ms (true r=%.1e)  [Eigen/Cerid = %.2fx wall]\n", itt, tt, rt, tt > 0 ? t_e / tt : 0.0);
    std::printf("    Cerid ILUT +AMD    : %5zu it %8.2f ms (true r=%.1e)  [Eigen/Cerid = %.2fx wall]\n", itr, tr, rr, tr > 0 ? t_e / tr : 0.0);
    std::printf("    Eigen ILUT-BiCG    : %5ld it %8.2f ms (true r=%.1e)\n", eit, t_e, eres);
}

// ILU(p) level-of-fill, p = 0,1,2 (Cerid) vs Eigen IncompleteLUT, all through FGMRES.
// Shows the level-of-fill value-add (fill ratio + iteration count vs p) and the breadth
// (Eigen ships no ILU(p)). Comparable fill: Eigen fillfactor ≈ ILU(2)'s fill / nnz.
static void run_ilup(const char* name, const Mtx& mtx)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int n = mtx.n; const double ctol = 1e-9; const double etol = 1e-8; const int cap = 8000; const int restart = 60;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    std::vector<double> Bv(n, 1.0);

    std::printf("  %-10s n=%-6d nnz=%-8zu  [Cerid ILU(p) p=0,1,2 vs Eigen IncompleteLUT; FGMRES(%d); matched TRUE residual]\n", name, n, a.nnz(), restart);
    crd::usize fill2 = 0;
    for (crd::u32 p = 0; p <= 2; ++p)
    {
        hp::IlupPreconditioner<double> m(a, &g_alloc, p);
        crd::usize it = 0; double res = 0;
        const double t = best_ms([&]() {
            hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n)), x(&g_alloc, static_cast<crd::usize>(n));
            for (int k = 0; k < n; ++k) { b(static_cast<crd::usize>(k)) = Bv[static_cast<std::size_t>(k)]; }
            hi::IterativeOptions<double> opts; opts.rel_tol = ctol; opts.max_iter = static_cast<crd::usize>(cap);
            hi::GmresWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(restart));
            auto r = hi::fgmres<double>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
            it = r.iterations; res = true_resid(op, Bv, x, n);
        });
        if (p == 2) { fill2 = m.factor_nnz(); }
        std::printf("    Cerid ILU(%u)     : %5zu it %8.2f ms (true r=%.1e)  fill=%.2fx\n", p, it, t, res,
                    static_cast<double>(m.factor_nnz()) / static_cast<double>(a.nnz()));
    }
    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    const double fillf = a.nnz() > 0 ? static_cast<double>(fill2) / static_cast<double>(a.nnz()) : 1.0;
    Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::IncompleteLUT<double>> g;
    g.set_restart(restart); g.preconditioner().setDroptol(1e-4); g.preconditioner().setFillfactor(static_cast<int>(fillf + 1.0));
    g.setMaxIterations(cap); g.setTolerance(etol); g.compute(ea);
    long eit = 0; double eres = 0;
    const double te = best_ms([&]() {
        Eigen::VectorXd b(n); for (int k = 0; k < n; ++k) { b(k) = Bv[static_cast<std::size_t>(k)]; }
        Eigen::VectorXd x = g.solve(b);
        eit = static_cast<long>(g.iterations()); eres = (b - ea * x).norm() / b.norm();
    });
    std::printf("    Eigen ILUT-GMRES : %5ld it %8.2f ms (r=%.1e)  fill≈%.2fx (matched to ILU(2))\n", eit, te, eres, fillf);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap IC(0)/ILU(0) preconditioners vs Eigen IncompleteCholesky / IncompleteLUT (real SuiteSparse)\n");
    std::printf("  Cerid pure level-0 + parallel SELL spmv; Eigen shifted-IC / dual-threshold ILUT. Both iter counts reported.\n");
    const std::string base = std::string(CRD_SUITESPARSE_DIR);
    std::printf(" --- SPD: IC(0) ---\n");
    run_ic("bcsstk13", read_mtx(base + "/bcsstk13/bcsstk13.mtx"));
    run_ic("bcsstk24", read_mtx(base + "/bcsstk24/bcsstk24.mtx"));
    run_ic("bcsstk25", read_mtx(base + "/bcsstk25/bcsstk25.mtx"));
    std::printf(" --- nonsym: ILU(0)/ILUT (small SuiteSparse: serial tri-solve regime) ---\n");
    run_ilu("gemat11", read_mtx(base + "/gemat11/gemat11.mtx"));
    run_ilu("sherman3", read_mtx(base + "/sherman3/sherman3.mtx"));
    std::printf(" --- nonsym: large 2D conv-diff (n=40000: level-scheduled parallel tri-solve regime) ---\n");
    run_ilu("cd2d-200", make_conv_diff_2d(200, 0.3));
    std::printf(" --- nonsym: ILU(p) level-of-fill (p=0,1,2) vs Eigen IncompleteLUT ---\n");
    run_ilup("cd2d-100", make_conv_diff_2d(100, 0.3));
    run_ilup("sherman3", read_mtx(base + "/sherman3/sherman3.mtx"));
    crd::jobs::shutdown();
    return 0;
}
