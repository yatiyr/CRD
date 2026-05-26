// bench_hesap_block_cg_vs_reference.cpp -- Phase 3.1.6 v4f-2.
//
// Block-CG (multi-RHS, s columns at once) vs Eigen ConjugateGradient solving the s
// columns ONE AT A TIME (Eigen has no true block algorithm). SPD systems. The block
// win: (a) ONE spmm per step for all s RHS instead of s spmvs, (b) the s columns
// share one Krylov space ⇒ often fewer iterations per column. Reports total
// A-passes (block: block-iters; per-column: Σ column-iters) + wall time + max
// per-column true residual. Matrices: a well-conditioned synthetic SPD (both
// converge ⇒ clean throughput crush) + a real SuiteSparse SPD.
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).

#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/block_cg.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/preconditioners/block_preconditioner.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>

#include <algorithm>
#include <chrono>
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

// Well-conditioned SPD: diagonally-dominant tridiag (diag 4, off -1). 3 nnz/row —
// the CHEAP-operator regime (block overhead dominates; block-CG's A-pass win can't
// pay off in wall time — the honest lower bound).
static Mtx make_spd(int n)
{
    Mtx m; m.n = n;
    for (int i = 0; i < n; ++i)
    {
        m.trips.push_back({i, i, 4.0});
        if (i + 1 < n) { m.trips.push_back({i, i + 1, -1.0}); }
        if (i > 0) { m.trips.push_back({i, i - 1, -1.0}); }
    }
    m.ok = true;
    return m;
}

// Well-conditioned banded SPD, ~(2·hb+1) nnz/row — the EXPENSIVE-operator regime
// where the spmm dominates, so block-CG's 4–16× fewer A-passes win wall time.
static Mtx make_banded_spd(int n, int hb)
{
    Mtx m; m.n = n;
    for (int i = 0; i < n; ++i)
    {
        m.trips.push_back({i, i, 2.0 * hb + 2.0}); // diagonally dominant ⇒ SPD, well-conditioned
        for (int d = 1; d <= hb; ++d)
        {
            if (i + d < n) { m.trips.push_back({i, i + d, -1.0}); }
            if (i - d >= 0) { m.trips.push_back({i, i - d, -1.0}); }
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

// max per-column true relative residual ‖B·,j - A·X·,j‖/‖B·,j‖ (blocks n×s row-major).
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

// Three Jacobi-preconditioned solvers on the SAME SPD system + the SAME s RHS:
//   1. Cerid block-PCG     -- all s RHS share one Krylov space; ONE spmm/step.
//   2. Cerid per-column PCG -- the algorithm-appropriate tool when A is cache-resident
//                             (parallel SELL spmv; the v4a path that crushes the cheap regime).
//   3. Eigen per-column CG + DiagonalPreconditioner -- the frontier reference (no block algo).
// Jacobi on all three ⇒ apples-to-apples. Eigen .compute() (the factorization) is OUTSIDE
// the timed region, matching the Cerid preconditioners (built once, before the timed solve).
static void run(const char* name, const Mtx& mtx, int s)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int    n   = mtx.n;
    const double tol = 1e-8;
    const int    cap = 8000;

    hs::TripletBuilder<double> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips) { tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v); }
    auto                             a = tb.compress();
    hs::ParallelSpmmLinearOp<double> op(a);                 // block op (parallel spmm above ~L2)
    hs::ParallelSparseLinearOp<double> colop(a, &g_alloc);  // single-vector op (parallel SELL spmv)
    hp::JacobiBlockPreconditioner<double> jblk(a, &g_alloc);// block Jacobi (one-pass diagonal)
    hp::JacobiPreconditioner<double>      jcol(a, &g_alloc);// single-vector Jacobi

    // RHS block B (n×s row-major), distinct columns.
    std::vector<double> B(static_cast<std::size_t>(n) * s);
    for (int k = 0; k < n; ++k)
    {
        for (int j = 0; j < s; ++j) { B[static_cast<std::size_t>(k) * s + j] = 1.0 + 0.1 * j + 0.01 * (k % 9); }
    }

    // 1. Cerid block-PCG (all s RHS at once, shared Krylov space, one spmm/step).
    crd::usize   blk_it = 0; double blk_res = 0;
    const double t_blk = best_ms([&]() {
        hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n) * s);
        for (std::size_t i = 0; i < B.size(); ++i) { b(i) = B[i]; }
        hd::Vector<double>           x(&g_alloc, static_cast<crd::usize>(n) * s);
        hi::IterativeOptions<double> opts; opts.rel_tol = tol; opts.max_iter = static_cast<crd::usize>(cap);
        hi::BlockCgWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::u32>(s));
        auto res = hi::block_pcg<double>(op, jblk, b.span(), x.span(), opts, ws, &g_alloc);
        blk_it   = res.iterations;
        blk_res  = block_worst_res(op, x, B, n, s);
    });

    // 2. Cerid per-column PCG (parallel SELL spmv; the cache-resident-A regime tool).
    long col_it = 0; double col_res = 0;
    const double t_col = best_ms([&]() {
        col_it = 0; double worst = 0;
        hd::Vector<double> bj(&g_alloc, static_cast<crd::usize>(n));
        hd::Vector<double> xj(&g_alloc, static_cast<crd::usize>(n));
        for (int j = 0; j < s; ++j)
        {
            for (int k = 0; k < n; ++k) { bj(static_cast<crd::usize>(k)) = B[static_cast<std::size_t>(k) * s + j]; xj(static_cast<crd::usize>(k)) = 0.0; }
            hi::IterativeOptions<double> opts; opts.rel_tol = tol; opts.max_iter = static_cast<crd::usize>(cap);
            hi::KrylovWorkspace<double>  ws(&g_alloc, static_cast<crd::usize>(n));
            auto res = hi::pcg<double>(colop, jcol, bj.span(), xj.span(), opts, ws, &g_alloc);
            col_it += static_cast<long>(res.iterations);
            worst = std::max(worst, res.final_residual_norm / (bj.span().size() ? hd::nrm2<double>(bj.span()) : 1.0));
        }
        col_res = worst;
    });

    // 3. Eigen per-column CG + DiagonalPreconditioner. compute() (factorization) hoisted out.
    std::vector<Eigen::Triplet<double>> et; et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips) { et.emplace_back(t.r, t.c, t.v); }
    Eigen::SparseMatrix<double> ea(n, n); ea.setFromTriplets(et.begin(), et.end());
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper, Eigen::DiagonalPreconditioner<double>> cg;
    cg.setMaxIterations(cap); cg.setTolerance(tol); cg.compute(ea);

    long eig_it = 0; double eig_res = 0;
    const double t_eig = best_ms([&]() {
        eig_it = 0; double worst = 0;
        for (int j = 0; j < s; ++j)
        {
            Eigen::VectorXd bj(n);
            for (int k = 0; k < n; ++k) { bj(k) = B[static_cast<std::size_t>(k) * s + j]; }
            Eigen::VectorXd xj = cg.solve(bj);
            eig_it += static_cast<long>(cg.iterations());
            worst = std::max(worst, (bj - ea * xj).norm() / bj.norm());
        }
        eig_res = worst;
    });

    std::printf("  %-10s n=%-6d nnz=%-8zu s=%d\n", name, n, a.nnz(), s);
    std::printf("    Cerid block-PCG    : %5zu block-iters (=A-passes) %8.2f ms  (r=%.1e)  [Eigen/blk = %.2fx time, %.2fx A-passes]\n",
                blk_it, t_blk, blk_res, t_blk > 0 ? t_eig / t_blk : 0.0, blk_it > 0 ? double(eig_it) / double(blk_it) : 0.0);
    std::printf("    Cerid per-col PCG  : %5ld total col-iters         %8.2f ms  (r=%.1e)  [Eigen/col = %.2fx time]\n",
                col_it, t_col, col_res, t_col > 0 ? t_eig / t_col : 0.0);
    std::printf("    Eigen per-col CG+J : %5ld total col-iters         %8.2f ms  (r=%.1e)\n", eig_it, t_eig, eig_res);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap block-PCG (multi-RHS) + per-column PCG vs Eigen CG+Jacobi per-column -- SPD (all Jacobi-preconditioned)\n");
    std::printf("  block-PCG wins when A>L2 (A-pass reuse) or shared-Krylov cuts iters; per-column PCG is the\n");
    std::printf("  algorithm-appropriate tool when A is cache-resident (block-PCG's O(n.s^2) dense flops then have\n");
    std::printf("  no memory-pass win to trade for). Cerid crushes Eigen via whichever path fits the regime.\n");
    std::printf("  Synthetic (well-conditioned: isolates the A-pass throughput lever):\n");
    for (int s : {4, 16})
    {
        run("tridiag(3)", make_spd(20000), s);            // cheap operator (3 nnz/row, L1-resident):
                                                          //   algorithmic floor -- block-CG's O(n.s^2) dense
                                                          //   flops have no memory-pass win to trade for.
        run("banded(41)", make_banded_spd(20000, 20), s); // expensive operator: the block-CG win regime.
    }
    // Real SuiteSparse SPD structural-stiffness matrices (ill-conditioned ⇒ the
    // shared Krylov space cuts iterations-per-column, block-CG's second lever).
    const std::string base = std::string(CRD_SUITESPARSE_DIR);
    std::printf("  Real SuiteSparse SPD (ill-conditioned: shared-Krylov iteration lever + A-pass lever):\n");
    for (int s : {4, 16})
    {
        run("bcsstk13", read_mtx(base + "/bcsstk13/bcsstk13.mtx"), s);
        run("bcsstk24", read_mtx(base + "/bcsstk24/bcsstk24.mtx"), s);
    }
    crd::jobs::shutdown();
    return 0;
}
