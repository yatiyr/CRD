// bench_hesap_nested_vs_reference.cpp -- Phase 3.1.6 v4f-1.
//
// Inner-Krylov-as-preconditioner (KrylovPreconditioner): FGMRES(m)-outer with a
// few-iteration inner GMRES as the preconditioner, vs:
//   - flat Cerid GMRES(m)         (the composition this enables vs not),
//   - Eigen GMRES(m)              (frontier ref; no nested-Krylov composition).
// Reports OUTER iterations, wall time, and the TRUE residual ‖b−Ax‖/‖b‖. The value
// is compositional: a cheap inner approximate-solve cuts OUTER iterations and lets
// the outer converge where flat restarted GMRES(m) stalls (no Eigen peer for the
// composition itself; honest breadth + the convergence demonstration). Matrices:
// a synthetic small-eigenvalue-cluster nonsym (where nesting clearly helps) + real
// SuiteSparse.
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).

#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/krylov_preconditioner.hpp>
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

static Mtx make_cluster(int n, int nsmall)
{
    Mtx m;
    m.n = n;
    for (int i = 0; i < n; ++i)
    {
        m.trips.push_back({i, i, (i < nsmall) ? 0.1 : 10.0});
        if (i + 1 < n) { m.trips.push_back({i, i + 1, -1.0}); }
        if (i > 0) { m.trips.push_back({i, i - 1, -0.7}); }
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

static void run(const char* name, const Mtx& mtx, int m_outer, int m_inner, int inner_iters)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int    n   = mtx.n;
    const double tol = 1e-9;
    const int    cap = 6000;

    hs::TripletBuilder<double> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips) { tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v); }
    auto a = tb.compress();
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);

    hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n));
    b.fill(1.0);
    hi::IterativeOptions<double> opts;
    opts.rel_tol  = tol;
    opts.max_iter = static_cast<crd::usize>(cap);

    auto cer_true = [&](const hd::Vector<double>& x) -> double {
        hd::Vector<double> ax(&g_alloc, static_cast<crd::usize>(n));
        (void)op.apply(x.span(), ax.span());
        hd::Vector<double> rr(&g_alloc, static_cast<crd::usize>(n));
        for (int i = 0; i < n; ++i) { rr(i) = b(i) - ax(i); }
        return hd::nrm2<double>(rr.span()) / hd::nrm2<double>(b.span());
    };

    crd::usize flat_it = 0; double flat_res = 0;
    const double t_flat = best_ms([&]() {
        hd::Vector<double>       x(&g_alloc, static_cast<crd::usize>(n));
        hi::GmresWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(m_outer));
        auto res = hi::gmres<double>(op, b.span(), x.span(), opts, ws, &g_alloc);
        flat_it = res.iterations; flat_res = cer_true(x);
    });

    crd::usize nest_it = 0; double nest_res = 0;
    const double t_nest = best_ms([&]() {
        hd::Vector<double>       x(&g_alloc, static_cast<crd::usize>(n));
        hi::GmresWorkspace<double> inner_ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(m_inner));
        auto inner = [&](crd::containers::ConstSpan<double> r, crd::containers::Span<double> z) {
            hi::IterativeOptions<double> io;
            io.max_iter = static_cast<crd::usize>(inner_iters);
            io.rel_tol  = 1e-2;
            (void)hi::gmres<double>(op, r, z, io, inner_ws, &g_alloc);
        };
        auto P = hp::make_krylov_preconditioner<double>(static_cast<crd::usize>(n), inner);
        hi::GmresWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(m_outer));
        auto res = hi::fgmres<double>(op, &P, b.span(), x.span(), opts, ws, &g_alloc);
        nest_it = res.iterations; nest_res = cer_true(x);
    });

    std::vector<Eigen::Triplet<double>> et;
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips) { et.emplace_back(t.r, t.c, t.v); }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.begin(), et.end());
    Eigen::VectorXd eb = Eigen::VectorXd::Ones(n);

    int eg_it = 0; double eg_res = 0;
    const double t_eg = best_ms([&]() {
        Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::IdentityPreconditioner> s;
        s.setMaxIterations(cap); s.setTolerance(tol); s.set_restart(m_outer); s.compute(ea);
        Eigen::VectorXd x = s.solve(eb);
        eg_it = static_cast<int>(s.iterations()); eg_res = (eb - ea * x).norm() / eb.norm();
    });

    std::printf("  %-10s n=%-6d nnz=%-8zu  outer=%d inner=GMRES(%d,%d it)\n", name, n, a.nnz(), m_outer, m_inner,
                inner_iters);
    std::printf("    flat GMRES(m)     : %5zu outer-it %8.2f ms (r=%.1e %s)\n", flat_it, t_flat, flat_res,
                flat_res <= 1e-6 ? "conv" : "STALL");
    std::printf("    nested FGMRES     : %5zu outer-it %8.2f ms (r=%.1e %s)  -> %.2fx fewer outer-iters\n", nest_it,
                t_nest, nest_res, nest_res <= 1e-6 ? "conv" : "STALL",
                nest_it > 0 ? double(flat_it) / double(nest_it) : 0.0);
    std::printf("    Eigen GMRES(m)    : %5d outer-it %8.2f ms (r=%.1e %s)  [no nested composition]\n", eg_it, t_eg,
                eg_res, eg_res <= 1e-6 ? "conv" : "STALL");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap nested inner-Krylov-as-preconditioner (FGMRES-outer + inner-GMRES) vs flat GMRES + Eigen GMRES\n");
    std::printf("  value = OUTER iterations cut by the inner approximate-solve; converges where flat restarted GMRES stalls.\n");
    run("cluster", make_cluster(2000, 8), /*m_outer=*/10, /*m_inner=*/20, /*inner_iters=*/12);
    const char* names[] = {"gemat11", "sherman3", "bcsstk13"};
    for (const char* nm : names)
    {
        const std::string path = std::string(CRD_SUITESPARSE_DIR) + "/" + nm + "/" + nm + ".mtx";
        run(nm, read_mtx(path), /*m_outer=*/30, /*m_inner=*/30, /*inner_iters=*/15);
    }
    crd::jobs::shutdown();
    return 0;
}
