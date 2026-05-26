// bench_hesap_idrs_vs_reference.cpp -- Phase 3.1.6 v4d-2b.
//
// APPLES-TO-APPLES: Cerid IDR(s) vs Eigen IDRS (unsupported/Eigen/IterativeSolvers),
// SAME algorithm + SAME s, on REAL nonsymmetric SuiteSparse matrices (gemat11,
// sherman3). Both unpreconditioned, then both Jacobi-preconditioned. s = 4 pinned
// on both sides. The comparison is wall time on the same problem; the TRUE residual
// ‖b−Ax‖/‖b‖ is recomputed for BOTH sides (a solver reporting "converged" while
// having stagnated would otherwise inflate the crush -- see
// feedback_iterative_crush_claim_same_algorithm).
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).

#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/idrs.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <Eigen/Sparse>
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
    const int    cap = 4000;
    const int    sdim = 4;
    // Note: bcsstk13+Jacobi converges (r≈1.6e-6 at ~4823 it) with cap=8000 — it is
    // budget-bound here, marching down steadily; Eigen IDRS diverges on it regardless.

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

    Eigen::SparseMatrix<double> ea(n, n);
    {
        std::vector<Eigen::Triplet<double>> et;
        et.reserve(mtx.trips.size());
        for (const Trip& t : mtx.trips) { et.emplace_back(t.r, t.c, t.v); }
        ea.setFromTriplets(et.begin(), et.end());
    }
    Eigen::VectorXd eb = Eigen::VectorXd::Ones(n);

    // True residual ‖b−Ax‖/‖b‖ for a Cerid solution (recompute through A).
    auto cer_true = [&](const hd::Vector<double>& x) -> double {
        hd::Vector<double> ax(&g_alloc, static_cast<crd::usize>(n));
        (void)op.apply(x.span(), ax.span());
        hd::Vector<double> r(&g_alloc, static_cast<crd::usize>(n));
        for (int i = 0; i < n; ++i) { r(i) = b(i) - ax(i); }
        return hd::nrm2<double>(r.span()) / hd::nrm2<double>(b.span());
    };
    auto eig_true = [&](const Eigen::VectorXd& x) -> double { return (eb - ea * x).norm() / eb.norm(); };

    auto bench_pair = [&](const char* tag, bool precond) {
        crd::usize cer_it = 0; double cer_res = 0;
        const double t_cer = best_ms([&]() {
            hd::Vector<double>      x(&g_alloc, static_cast<crd::usize>(n));
            hi::IdrsWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(sdim));
            auto res = precond ? hi::idrs<double>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc)
                               : hi::idrs<double>(op, b.span(), x.span(), opts, ws, &g_alloc);
            cer_it = res.iterations; cer_res = cer_true(x);
        });

        int eig_it = 0; double eig_res = 0; bool eig_ok = false;
        const double t_eig = best_ms([&]() {
            Eigen::VectorXd x;
            if (precond)
            {
                Eigen::IDRS<Eigen::SparseMatrix<double>, Eigen::DiagonalPreconditioner<double>> s;
                s.setMaxIterations(cap); s.setTolerance(tol); s.setS(sdim); s.compute(ea);
                x = s.solve(eb);
                eig_it = static_cast<int>(s.iterations()); eig_ok = (s.info() == Eigen::Success);
            }
            else
            {
                Eigen::IDRS<Eigen::SparseMatrix<double>, Eigen::IdentityPreconditioner> s;
                s.setMaxIterations(cap); s.setTolerance(tol); s.setS(sdim); s.compute(ea);
                x = s.solve(eb);
                eig_it = static_cast<int>(s.iterations()); eig_ok = (s.info() == Eigen::Success);
            }
            eig_res = eig_true(x);
        });

        const bool cer_conv = cer_res <= 1e-6;
        const bool eig_conv = eig_ok && eig_res <= 1e-6;
        std::printf("    IDR(4) %-8s : Cerid %4zu it %7.2f ms (r=%.1e %s)  vs  Eigen %4d it %7.2f ms (r=%.1e %s)  "
                    "Eigen/Cerid=%.2fx %s\n",
                    tag, cer_it, t_cer, cer_res, cer_conv ? "conv" : "STALL", eig_it, t_eig, eig_res,
                    eig_conv ? "conv" : "STALL", t_eig / t_cer, (t_eig / t_cer >= 1.0 ? "WIN" : "loss"));
    };

    std::printf("  %-10s n=%-6d nnz=%-8zu [%s]\n", name, n, a.nnz(), op.is_parallel() ? "parallel" : "serial");
    bench_pair("(none)", false);
    bench_pair("(jacobi)", true);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap IDR(s=4) vs Eigen IDRS (unsupported) -- real nonsym, APPLES-TO-APPLES (same algo + s);\n");
    std::printf("  true residual ‖b-Ax‖/‖b‖ recomputed for BOTH sides (STALL = reported converged but residual > 1e-6).\n");
    const char* names[] = {"bcsstk13", "bcsstk24", "bcsstk25", "gemat11", "sherman3"};
    for (const char* n : names) { run(n); }
    crd::jobs::shutdown();
    return 0;
}
