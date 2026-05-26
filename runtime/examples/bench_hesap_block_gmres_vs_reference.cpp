// bench_hesap_block_gmres_vs_reference.cpp -- Phase 3.1.6 v4f-3.
//
// Block-GMRES + block-BiCGSTAB (multi-RHS, s columns at once) vs Eigen GMRES /
// BiCGSTAB solving the s columns ONE AT A TIME (Eigen has no block algorithm), plus
// Cerid's OWN per-column FGMRES / BiCGSTAB (the algorithm-appropriate path when A is
// cache-resident — the regime owner, same honest framing as block-CG v4f-2). GENERAL
// nonsymmetric A. Reports A-passes (block: block-iters; per-column: Σ col-iters) +
// wall time + max per-column true residual.
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).

#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/block_bicgstab.hpp>
#include <crd/hesap/iterative/block_gmres.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
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

// Nonsymmetric convection-diffusion (diag dd, super -1+g, sub -1-g). 3 nnz/row =
// cheap operator (per-column owns it). A larger band ⇒ expensive operator.
static Mtx make_conv_diff(int n, double dd, double g, int hb)
{
    Mtx m; m.n = n;
    for (int i = 0; i < n; ++i)
    {
        m.trips.push_back({i, i, dd});
        for (int d = 1; d <= hb; ++d)
        {
            if (i + d < n) { m.trips.push_back({i, i + d, -1.0 + g / d}); }
            if (i - d >= 0) { m.trips.push_back({i, i - d, -1.0 - g / d}); }
        }
    }
    m.ok = true;
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

static double block_worst_res(hs::ParallelSpmmLinearOp<double>& op, const hd::Vector<double>& x,
                              const std::vector<double>& B, int n, int s)
{
    hd::Vector<double> ax(&g_alloc, static_cast<crd::usize>(n) * s);
    (void)op.apply_block(x.span(), static_cast<crd::u32>(s), ax.span(), static_cast<crd::u32>(s), static_cast<crd::u32>(s));
    double worst = 0;
    for (int j = 0; j < s; ++j)
    {
        double rn = 0, bn = 0;
        for (int k = 0; k < n; ++k)
        {
            const double d = B[static_cast<std::size_t>(k) * s + j] - ax(static_cast<std::size_t>(k) * s + j);
            rn += d * d;
            bn += B[static_cast<std::size_t>(k) * s + j] * B[static_cast<std::size_t>(k) * s + j];
        }
        worst = std::max(worst, std::sqrt(rn) / std::sqrt(bn));
    }
    return worst;
}

enum class Kind { Gmres, Bicgstab };

static void run(const char* name, const Mtx& mtx, int s, Kind kind)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int    n   = mtx.n;
    const double tol = 1e-8;
    const int    cap = 4000;
    const int    restart = 60;

    hs::TripletBuilder<double> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips) { tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v); }
    auto                               a = tb.compress();
    hs::ParallelSpmmLinearOp<double>   bop(a);
    hs::ParallelSparseLinearOp<double> colop(a, &g_alloc);

    std::vector<double> B(static_cast<std::size_t>(n) * s);
    for (int k = 0; k < n; ++k)
    {
        for (int j = 0; j < s; ++j)
        {
            B[static_cast<std::size_t>(k) * s + j] = std::sin(0.21 * k * (j + 1) + 0.4 * j) + 0.3 * ((k + 2 * j) % 5);
        }
    }

    // 1. Cerid block solver.
    crd::usize blk_it = 0; double blk_res = 0;
    const double t_blk = best_ms([&]() {
        hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n) * s);
        for (std::size_t i = 0; i < B.size(); ++i) { b(i) = B[i]; }
        hd::Vector<double> x(&g_alloc, static_cast<crd::usize>(n) * s);
        hi::IterativeOptions<double> opts; opts.rel_tol = tol; opts.max_iter = static_cast<crd::usize>(cap);
        if (kind == Kind::Gmres)
        {
            hi::BlockGmresWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::u32>(s), restart);
            blk_it = hi::block_gmres<double>(bop, b.span(), x.span(), opts, ws, &g_alloc).iterations;
        }
        else
        {
            hi::BlockBicgstabWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::u32>(s));
            blk_it = hi::block_bicgstab<double>(bop, b.span(), x.span(), opts, ws, &g_alloc).iterations;
        }
        blk_res = block_worst_res(bop, x, B, n, s);
    });

    // 2. Cerid per-column (FGMRES / BiCGSTAB).
    long col_it = 0; double col_res = 0;
    const double t_col = best_ms([&]() {
        col_it = 0; double worst = 0;
        hd::Vector<double> bj(&g_alloc, static_cast<crd::usize>(n)), xj(&g_alloc, static_cast<crd::usize>(n));
        for (int j = 0; j < s; ++j)
        {
            for (int k = 0; k < n; ++k) { bj(static_cast<crd::usize>(k)) = B[static_cast<std::size_t>(k) * s + j]; xj(static_cast<crd::usize>(k)) = 0.0; }
            hi::IterativeOptions<double> opts; opts.rel_tol = tol; opts.max_iter = static_cast<crd::usize>(cap);
            hi::IterativeResult<double> r(&g_alloc);
            if (kind == Kind::Gmres)
            {
                hi::GmresWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(restart));
                r = hi::gmres<double>(colop, bj.span(), xj.span(), opts, ws, &g_alloc);
            }
            else
            {
                hi::BicgstabWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
                r = hi::bicgstab<double>(colop, nullptr, bj.span(), xj.span(), opts, ws, &g_alloc);
            }
            col_it += static_cast<long>(r.iterations);
            double rn = 0, bn = 0;
            hd::Vector<double> ax(&g_alloc, static_cast<crd::usize>(n));
            (void)colop.apply(xj.span(), ax.span());
            for (int k = 0; k < n; ++k) { double d = bj(static_cast<crd::usize>(k)) - ax(static_cast<crd::usize>(k)); rn += d * d; bn += bj(static_cast<crd::usize>(k)) * bj(static_cast<crd::usize>(k)); }
            worst = std::max(worst, std::sqrt(rn) / std::sqrt(bn));
        }
        col_res = worst;
    });

    // 3. Eigen per-column (GMRES / BiCGSTAB), single-threaded; compute() hoisted out.
    std::vector<Eigen::Triplet<double>> et; et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips) { et.emplace_back(t.r, t.c, t.v); }
    Eigen::SparseMatrix<double> ea(n, n); ea.setFromTriplets(et.begin(), et.end());

    long eig_it = 0; double eig_res = 0; const char* eref = "";
    if (kind == Kind::Gmres)
    {
        Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::IdentityPreconditioner> g;
        g.set_restart(restart); g.setMaxIterations(cap); g.setTolerance(tol); g.compute(ea);
        eref = "Eigen GMRES";
        const double t = best_ms([&]() {
            eig_it = 0; double worst = 0;
            for (int j = 0; j < s; ++j)
            {
                Eigen::VectorXd bj(n); for (int k = 0; k < n; ++k) { bj(k) = B[static_cast<std::size_t>(k) * s + j]; }
                Eigen::VectorXd xj = g.solve(bj);
                eig_it += static_cast<long>(g.iterations());
                worst = std::max(worst, (bj - ea * xj).norm() / bj.norm());
            }
            eig_res = worst;
        });
        std::printf("  %-10s n=%-6d nnz=%-8zu s=%d  [block-GMRES]\n", name, n, a.nnz(), s);
        std::printf("    Cerid block-GMRES : %5zu block-iters %8.2f ms (r=%.1e)  [Eigen/blk = %.2fx time, %.2fx A-passes]\n",
                    blk_it, t_blk, blk_res, t_blk > 0 ? t / t_blk : 0.0, blk_it > 0 ? double(eig_it) / double(blk_it) : 0.0);
        std::printf("    Cerid GMRES(/col) :%6ld col-iters   %8.2f ms (r=%.1e)  [Eigen/Cerid-col = %.2fx time]\n",
                    col_it, t_col, col_res, t_col > 0 ? t / t_col : 0.0);
        std::printf("    %-12s     :%6ld col-iters   %8.2f ms (r=%.1e)\n", eref, eig_it, t, eig_res);
    }
    else
    {
        Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::IdentityPreconditioner> g;
        g.setMaxIterations(cap); g.setTolerance(tol); g.compute(ea);
        eref = "Eigen BiCGSTAB";
        const double t = best_ms([&]() {
            eig_it = 0; double worst = 0;
            for (int j = 0; j < s; ++j)
            {
                Eigen::VectorXd bj(n); for (int k = 0; k < n; ++k) { bj(k) = B[static_cast<std::size_t>(k) * s + j]; }
                Eigen::VectorXd xj = g.solve(bj);
                eig_it += static_cast<long>(g.iterations());
                worst = std::max(worst, (bj - ea * xj).norm() / bj.norm());
            }
            eig_res = worst;
        });
        std::printf("  %-10s n=%-6d nnz=%-8zu s=%d  [block-BiCGSTAB]\n", name, n, a.nnz(), s);
        std::printf("    Cerid blk-BiCGSTAB: %5zu block-iters %8.2f ms (r=%.1e)  [Eigen/blk = %.2fx time, %.2fx A-passes]\n",
                    blk_it, t_blk, blk_res, t_blk > 0 ? t / t_blk : 0.0, blk_it > 0 ? double(eig_it) / double(blk_it) : 0.0);
        std::printf("    Cerid BiCGSTAB/col:%6ld col-iters   %8.2f ms (r=%.1e)  [Eigen/Cerid-col = %.2fx time]\n",
                    col_it, t_col, col_res, t_col > 0 ? t / t_col : 0.0);
        std::printf("    %-12s   :%6ld col-iters   %8.2f ms (r=%.1e)\n", eref, eig_it, t, eig_res);
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap block-GMRES + block-BiCGSTAB (multi-RHS) vs Eigen per-column (no block algo) -- general nonsym\n");
    std::printf("  block: one spmm/step for all s RHS + shared Krylov; A-passes = block-iters vs sum(col-iters).\n");
    const std::string base = std::string(CRD_SUITESPARSE_DIR);
    for (int s : {4, 16})
    {
        std::printf(" --- s=%d ---\n", s);
        run("convdiff3", make_conv_diff(20000, 4.0, 0.4, 1), s, Kind::Gmres);     // cheap operator
        run("convband", make_conv_diff(20000, 42.0, 0.4, 20), s, Kind::Gmres);    // expensive operator
        run("convdiff3", make_conv_diff(20000, 4.0, 0.4, 1), s, Kind::Bicgstab);
        run("convband", make_conv_diff(20000, 42.0, 0.4, 20), s, Kind::Bicgstab);
        run("gemat11", read_mtx(base + "/gemat11/gemat11.mtx"), s, Kind::Gmres);   // real nonsym
        run("gemat11", read_mtx(base + "/gemat11/gemat11.mtx"), s, Kind::Bicgstab);
    }
    crd::jobs::shutdown();
    return 0;
}
