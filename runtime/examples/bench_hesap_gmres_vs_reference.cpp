// bench_hesap_gmres_vs_reference.cpp -- Phase 3.1.6 v4b.
//
// FGMRES (Jacobi-preconditioned, restart 30) on REAL nonsymmetric SuiteSparse
// matrices (gemat11, sherman3) vs:
//   - Eigen BiCGSTAB + DiagonalPreconditioner  (HEADLINE: Eigen's maintained
//     main-module nonsymmetric Krylov solver),
//   - Eigen GMRES (unsupported/Eigen/IterativeSolvers; labeled, NOT main module).
// Iteration counts differ across algorithms; the comparison is wall time on the
// same problem class. Each matrix is tagged [serial]/[parallel] (the operator
// auto-selects by working set; these matrices are cache-resident ⇒ serial SELL).
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).

#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
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
        if (rows == 0)
        {
            ss >> rows >> cols >> nnz;
            m.n = rows;
            m.trips.reserve(static_cast<std::size_t>(nnz) * (sym ? 2 : 1));
            continue;
        }
        int i, j; double v;
        ss >> i >> j >> v;
        --i; --j;
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

static void run(const char* name)
{
    const std::string path = std::string(CRD_SUITESPARSE_DIR) + "/" + name + "/" + name + ".mtx";
    Mtx               mtx  = read_mtx(path);
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int    n   = mtx.n;
    const double tol = 1e-8;
    const int    cap = 2000;
    const int    m   = 30;

    hs::TripletBuilder<double> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips) { tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v); }
    auto a = tb.compress();
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    hp::JacobiPreconditioner<double>   jac(a, &g_alloc);

    hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n));
    b.fill(1.0);
    hi::IterativeOptions<double> opts;
    opts.rel_tol  = tol;
    opts.max_iter = static_cast<crd::usize>(cap);

    crd::usize cer_it = 0; bool cer_conv = false;
    const double t_cer = best_ms([&]() {
        hd::Vector<double>       x(&g_alloc, static_cast<crd::usize>(n));
        hi::GmresWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(m));
        auto res = hi::fgmres<double>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc);
        cer_it = res.iterations; cer_conv = res.converged;
    });

    // APPLES-TO-APPLES: Cerid BiCGSTAB vs Eigen BiCGSTAB (same algorithm + Jacobi).
    crd::usize cb_it = 0; bool cb_conv = false;
    const double t_cb = best_ms([&]() {
        hd::Vector<double>           x(&g_alloc, static_cast<crd::usize>(n));
        hi::BicgstabWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
        auto res = hi::bicgstab<double>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc);
        cb_it = res.iterations; cb_conv = res.converged;
    });

    std::vector<Eigen::Triplet<double>> et;
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips) { et.emplace_back(t.r, t.c, t.v); }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.begin(), et.end());
    Eigen::VectorXd eb = Eigen::VectorXd::Ones(n);

    int eb_it = 0; bool eb_conv = false;
    const double t_bicg = best_ms([&]() {
        Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::DiagonalPreconditioner<double>> s;
        s.setMaxIterations(cap); s.setTolerance(tol); s.compute(ea);
        Eigen::VectorXd x = s.solve(eb);
        eb_it = static_cast<int>(s.iterations()); eb_conv = (s.info() == Eigen::Success);
    });

    int eg_it = 0; bool eg_conv = false;
    const double t_egm = best_ms([&]() {
        Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::DiagonalPreconditioner<double>> s;
        s.setMaxIterations(cap); s.setTolerance(tol); s.set_restart(m); s.compute(ea);
        Eigen::VectorXd x = s.solve(eb);
        eg_it = static_cast<int>(s.iterations()); eg_conv = (s.info() == Eigen::Success);
    });

    // Headline (apples-to-apples): Cerid BiCGSTAB vs Eigen BiCGSTAB.
    std::printf("  %-10s n=%-6d nnz=%-8zu [%s]\n", name, n, a.nnz(), op.is_parallel() ? "parallel" : "serial");
    std::printf("    BiCGSTAB (same algo): Cerid %4zu it %7.2f ms (%s)  vs  Eigen %4d it %7.2f ms (%s)  Eigen/Cerid=%.2fx %s\n",
                cb_it, t_cb, cb_conv ? "conv" : "cap", eb_it, t_bicg, eb_conv ? "conv" : "cap", t_bicg / t_cb,
                (t_bicg / t_cb >= 1.0 ? "WIN" : "loss"));
    std::printf("    GMRES    (same algo): Cerid FGMRES %4zu it %7.2f ms (%s)  vs  Eigen GMRES %4d it %7.2f ms (%s)  Eigen/Cerid=%.2fx\n",
                cer_it, t_cer, cer_conv ? "conv" : "cap", eg_it, t_egm, eg_conv ? "conv" : "fail", t_egm / t_cer);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap FGMRES(m=30,Jacobi) vs Eigen BiCGSTAB (main) + Eigen GMRES (unsupported) -- real nonsym\n");
    std::printf("  (apples-to-apples is GMRES-vs-GMRES; sherman3-class restarted-GMRES stagnation is a GMRES(m)\n");
    std::printf("   limitation that v4c BiCGSTAB addresses with the same operator + determinism moat.)\n");
    const char* names[] = {"gemat11", "sherman3"};
    for (const char* n : names) { run(n); }
    crd::jobs::shutdown();
    return 0;
}
