// bench_hesap_cg_vs_reference.cpp -- Phase 3.1.6 v4a-2.
//
// CG / Jacobi-PCG vs Eigen ConjugateGradient + DiagonalPreconditioner on REAL
// SuiteSparse SPD stiffness matrices (bcsstk*). Our operator is the row-balanced
// PARALLEL SELL-C-σ spmv over crd::jobs (the DRAM-bound regime where Cerid beats
// Eigen's single-threaded scalar spmv); the Krylov reductions are KBN-pairwise,
// so the solve is bit-deterministic across thread counts -- which Eigen does not
// offer at all. Same tolerance + iteration cap on both sides; we report
// iterations, wall time, and Eigen/Cerid speedup.
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).

#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/minres.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>
#include <unsupported/Eigen/IterativeSolvers>

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

struct Trip
{
    int    r, c;
    double v;
};
struct Mtx
{
    int               n = 0;
    std::vector<Trip> trips;
    bool              ok = false;
};

static Mtx read_mtx(const std::string& path)
{
    Mtx           m;
    std::ifstream f(path);
    if (!f)
    {
        return m;
    }
    std::string line;
    bool        header = false;
    bool        sym    = false;
    int         rows = 0, cols = 0, nnz = 0, seen = 0;
    while (std::getline(f, line))
    {
        if (line.empty())
        {
            continue;
        }
        if (line[0] == '%')
        {
            if (!header && line.find("symmetric") != std::string::npos)
            {
                sym = true;
            }
            header = true;
            continue;
        }
        std::istringstream ss(line);
        if (rows == 0)
        {
            ss >> rows >> cols >> nnz;
            m.n = rows;
            m.trips.reserve(static_cast<std::size_t>(nnz) * (sym ? 2 : 1));
            continue;
        }
        int    i, j;
        double v;
        ss >> i >> j >> v;
        --i;
        --j;
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

static void run(const char* name)
{
    const std::string path = std::string(CRD_SUITESPARSE_DIR) + "/" + name + "/" + name + ".mtx";
    Mtx               mtx  = read_mtx(path);
    if (!mtx.ok)
    {
        std::printf("  %-12s SKIP (not found)\n", name);
        return;
    }
    const int    n   = mtx.n;
    const double tol = 1e-6;
    const int    cap = 3000;

    // Cerid CSR.
    hs::TripletBuilder<double> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    hp::JacobiPreconditioner<double>   m(a, &g_alloc);

    hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n));
    b.fill(1.0);

    hi::IterativeOptions<double> opts;
    opts.rel_tol  = tol;
    opts.max_iter = static_cast<crd::usize>(cap);

    crd::usize cerid_iters = 0;
    bool       cerid_conv  = false;
    const double t_cerid = best_ms([&]() {
        hd::Vector<double>        x(&g_alloc, static_cast<crd::usize>(n));
        hi::KrylovWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
        auto res    = hi::pcg<double>(op, m, b.span(), x.span(), opts, ws, &g_alloc);
        cerid_iters = res.iterations;
        cerid_conv  = res.converged;
    });

    // Eigen CG + DiagonalPreconditioner (Jacobi), single-threaded.
    std::vector<Eigen::Triplet<double>> et;
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        et.emplace_back(t.r, t.c, t.v);
    }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.begin(), et.end());
    Eigen::VectorXd eb = Eigen::VectorXd::Ones(n);
    int             eig_iters = 0;
    bool            eig_conv  = false;
    const double    t_eig = best_ms([&]() {
        Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                                 Eigen::DiagonalPreconditioner<double>>
            cg;
        cg.setMaxIterations(cap);
        cg.setTolerance(tol);
        cg.compute(ea); // bind + factor the preconditioner before solve
        Eigen::VectorXd ex = cg.solve(eb);
        eig_iters          = static_cast<int>(cg.iterations());
        eig_conv           = (cg.info() == Eigen::Success);
    });

    const double ratio = t_eig / t_cerid;
    std::printf("  %-12s n=%-7d nnz=%-9zu [%s] | Cerid PCG %5zu it %8.2f ms (%s) | Eigen CG %5d it %8.2f ms (%s) | "
                "Eigen/Cerid=%.2fx %s\n",
                name, n, a.nnz(), op.is_parallel() ? "parallel" : "serial", cerid_iters, t_cerid,
                cerid_conv ? "conv" : "cap", eig_iters, t_eig, eig_conv ? "conv" : "cap", ratio,
                (ratio >= 1.0 ? "WIN" : "loss"));

    // APPLES-TO-APPLES: Cerid Jacobi-MINRES vs Eigen Jacobi-MINRES (same algorithm
    // + same SPD preconditioner). v4c-2a-precond: preconditioned MINRES now solves
    // ill-conditioned SPD (bcsstk13) where unpreconditioned MINRES capped.
    crd::usize cm_it = 0; bool cm_conv = false;
    const double t_cm = best_ms([&]() {
        hd::Vector<double>       x(&g_alloc, static_cast<crd::usize>(n));
        hi::MinresWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
        auto res = hi::minres<double>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
        cm_it = res.iterations; cm_conv = res.converged;
    });
    int em_it = 0; bool em_conv = false;
    const double t_em = best_ms([&]() {
        Eigen::MINRES<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper, Eigen::DiagonalPreconditioner<double>> s;
        s.setMaxIterations(cap); s.setTolerance(tol); s.compute(ea);
        Eigen::VectorXd ex = s.solve(eb);
        em_it = static_cast<int>(s.iterations()); em_conv = (s.info() == Eigen::Success);
    });
    std::printf("    MINRES (same algo): Cerid %5zu it %8.2f ms (%s)  vs  Eigen %5d it %8.2f ms (%s)  Eigen/Cerid=%.2fx %s\n",
                cm_it, t_cm, cm_conv ? "conv" : "cap", em_it, t_em, em_conv ? "conv" : "cap", t_em / t_cm,
                (t_em / t_cm >= 1.0 ? "WIN" : "loss"));
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: see output up to any crash
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap CG vs Eigen ConjugateGradient+DiagonalPreconditioner (real SuiteSparse SPD)\n");
    const char* names[] = {"bcsstk13", "bcsstk24", "bcsstk25"};
    for (const char* n : names)
    {
        run(n);
    }
    crd::jobs::shutdown();
    return 0;
}
