// bench_hesap_spai_vs_reference.cpp -- Phase 3.1.6 v4i-1.
//
// SPARSE APPROXIMATE INVERSE preconditioners vs Eigen. Eigen ships NO SPAI/FSPAI, so the
// honest comparison is two-fold and split:
//   (1) SETUP wall -- SPAI/FSPAI construction is EMBARRASSINGLY PARALLEL (each column an
//       independent dense solve over crd::jobs) where Eigen's IncompleteCholesky / ILUT
//       factorization is inherently SEQUENTIAL. This is the headline win.
//   (2) APPLY+iters wall -- SPAI/FSPAI apply is one/two matrix-free spmv with NO triangular
//       solve (parallel-SELL, GPU-mappable), where IC/ILU apply is two sequential triangular
//       solves. SPAI wins per-iteration on the tri-solve-bound regime even when its iteration
//       count is higher; both iteration counts are reported (honest).
//
// SPD: FSPAI (M = L·Lᴴ, SPD-by-construction) -PCG vs Eigen IncompleteCholesky-CG.
// nonsym: classical SPAI (M ≈ A⁻¹) -FGMRES vs Eigen IncompleteLUT-GMRES.
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).

#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/block_jacobi.hpp>
#include <crd/hesap/preconditioners/chebyshev.hpp>
#include <crd/hesap/preconditioners/fspai.hpp>
#include <crd/hesap/preconditioners/schwarz.hpp>
#include <crd/hesap/preconditioners/spai.hpp>
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

static Mtx make_laplace_2d(int g) // SPD 5-point
{
    Mtx m; m.n = g * g; m.ok = true;
    for (int y = 0; y < g; ++y)
        for (int x = 0; x < g; ++x)
        {
            const int i = y * g + x;
            m.trips.push_back({i, i, 4.0});
            if (x + 1 < g) { m.trips.push_back({i, i + 1, -1.0}); }
            if (x > 0)     { m.trips.push_back({i, i - 1, -1.0}); }
            if (y + 1 < g) { m.trips.push_back({i, i + g, -1.0}); }
            if (y > 0)     { m.trips.push_back({i, i - g, -1.0}); }
        }
    return m;
}

static Mtx make_conv_diff_2d(int g, double beta) // nonsym 5-point
{
    Mtx m; m.n = g * g; m.ok = true;
    for (int y = 0; y < g; ++y)
        for (int x = 0; x < g; ++x)
        {
            const int i = y * g + x;
            m.trips.push_back({i, i, 4.0});
            if (x + 1 < g) { m.trips.push_back({i, i + 1, -1.0 + beta}); }
            if (x > 0)     { m.trips.push_back({i, i - 1, -1.0 - beta}); }
            if (y + 1 < g) { m.trips.push_back({i, i + g, -1.0 + beta}); }
            if (y > 0)     { m.trips.push_back({i, i - g, -1.0 - beta}); }
        }
    return m;
}

// Helper: TRUE relative residual ‖b − A·x‖₂ / ‖b‖₂ via the operator (apples-to-apples with
// Eigen's (b − A·x).norm()/b.norm()). The Krylov recurrence residual can drift below the true
// residual on ill-conditioned A, so the FAIR comparison is BOTH sides' TRUE residual; Cerid is
// driven to a tight rel_tol so its true residual lands at the same accuracy Eigen reaches.
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

// SPD: FSPAI-PCG (parallel setup, no tri-solve apply) vs Eigen IncompleteCholesky-CG. Matched
// TRUE-residual accuracy: Cerid rel_tol=1e-12 (drives true residual to Eigen's regime), Eigen
// tol=1e-8; both TRUE residuals + time-per-iteration reported (the per-iter ratio is the
// structural no-tri-solve win, the total-wall ratio the matched-accuracy headline).
static void run_fspai(const char* name, const Mtx& mtx)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int n = mtx.n; const double ctol = 1e-12; const double etol = 1e-8; const int cap = 8000;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    std::vector<double> Bv(n, 1.0);

    const double set_s = best_ms([&]() { hp::FspaiPreconditioner<double> m(a, &g_alloc, hp::SpaiPattern::Static); (void)m.factor_nnz(); });
    const double set_a = best_ms([&]() { hp::FspaiPreconditioner<double> m(a, &g_alloc, hp::SpaiPattern::Adaptive, 0.05); (void)m.factor_nnz(); });

    hp::FspaiPreconditioner<double> fss(a, &g_alloc, hp::SpaiPattern::Static);
    hp::FspaiPreconditioner<double> fsa(a, &g_alloc, hp::SpaiPattern::Adaptive, 0.05);
    auto solve = [&](const hp::FspaiPreconditioner<double>& m, crd::usize& it, double& tr) {
        return best_ms([&]() {
            hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n)), x(&g_alloc, static_cast<crd::usize>(n));
            for (int k = 0; k < n; ++k) { b(static_cast<crd::usize>(k)) = Bv[static_cast<std::size_t>(k)]; }
            hi::IterativeOptions<double> opts; opts.rel_tol = ctol; opts.max_iter = static_cast<crd::usize>(cap);
            hi::KrylovWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
            auto r = hi::pcg<double>(op, m, b.span(), x.span(), opts, ws, &g_alloc);
            it = r.iterations; tr = true_resid(op, Bv, x, n);
        });
    };
    crd::usize its = 0, ita = 0; double rs = 0, ra = 0;
    const double t_s = solve(fss, its, rs);
    const double t_a = solve(fsa, ita, ra);

    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    const double set_e = best_ms([&]() { Eigen::IncompleteCholesky<double> ic; ic.compute(ea); });
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper, Eigen::IncompleteCholesky<double>> cg;
    cg.setMaxIterations(cap); cg.setTolerance(etol); cg.compute(ea);
    long eit = 0; double eres = 0;
    const double t_e = best_ms([&]() {
        Eigen::VectorXd b(n); for (int k = 0; k < n; ++k) { b(k) = Bv[static_cast<std::size_t>(k)]; }
        Eigen::VectorXd x = cg.solve(b);
        eit = static_cast<long>(cg.iterations()); eres = (b - ea * x).norm() / b.norm();
    });

    const double mspit_c = its > 0 ? t_s / static_cast<double>(its) : 0.0;
    const double mspit_e = eit > 0 ? t_e / static_cast<double>(eit) : 0.0;
    std::printf("  %-10s n=%-6d nnz=%-8zu  [FSPAI-PCG vs Eigen IncompleteCholesky-CG; matched TRUE residual]\n", name, n, a.nnz());
    std::printf("    SETUP  Cerid FSPAI(static) %7.2f ms  FSPAI(adapt) %7.2f ms   Eigen IChol %7.2f ms   [Eigen/Cerid-static = %.2fx]\n",
                set_s, set_a, set_e, set_s > 0 ? set_e / set_s : 0.0);
    std::printf("    SOLVE  Cerid static %5zu it %7.2f ms (true r=%.1e, %.4f ms/it)  adapt %5zu it %7.2f ms (true r=%.1e)\n",
                its, t_s, rs, mspit_c, ita, t_a, ra);
    std::printf("    SOLVE  Eigen IChol  %5ld it %7.2f ms (true r=%.1e, %.4f ms/it)   [Eigen/Cerid-static = %.2fx wall, %.2fx per-it]\n",
                eit, t_e, eres, mspit_e, t_s > 0 ? t_e / t_s : 0.0, mspit_c > 0 ? mspit_e / mspit_c : 0.0);
    std::printf("    fill   static %.2fx  adapt %.2fx  (nnz(factor)/nnz(A))\n",
                static_cast<double>(fss.factor_nnz()) / static_cast<double>(a.nnz()),
                static_cast<double>(fsa.factor_nnz()) / static_cast<double>(a.nnz()));
}

// SPD: Chebyshev-PCG (matrix-free polynomial, NO triangular solve, deg spmv/apply) vs Eigen
// IncompleteCholesky-CG. Eigen ships NO polynomial preconditioner -> breadth; Chebyshev is a
// weaker per-apply preconditioner (more outer iters) but each apply is parallel + matrix-free +
// GPU-mappable, and it is the AMG smoother (v4k). Honest: iters + wall both reported.
static void run_chebyshev(const char* name, const Mtx& mtx)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int n = mtx.n; const double ctol = 1e-9; const double etol = 1e-8; const int cap = 3000;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    std::vector<double> Bv(n, 1.0);

    std::printf("  %-10s n=%-6d nnz=%-8zu  [Chebyshev-PCG (matrix-free) vs Eigen IncompleteCholesky-CG; matched TRUE residual]\n",
                name, n, a.nnz());
    for (crd::u32 deg : {4U, 8U, 16U})
    {
        hp::ChebyshevPreconditioner<double> m(a, &g_alloc, deg);
        crd::usize it = 0; double tr = 0;
        const double t = best_ms([&]() {
            hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n)), x(&g_alloc, static_cast<crd::usize>(n));
            for (int k = 0; k < n; ++k) { b(static_cast<crd::usize>(k)) = Bv[static_cast<std::size_t>(k)]; }
            hi::IterativeOptions<double> opts; opts.rel_tol = ctol; opts.max_iter = static_cast<crd::usize>(cap);
            hi::KrylovWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
            auto r = hi::pcg<double>(op, m, b.span(), x.span(), opts, ws, &g_alloc);
            it = r.iterations; tr = true_resid(op, Bv, x, n);
        });
        std::printf("    Cerid Chebyshev(%2u)-PCG : %5zu it %8.2f ms (true r=%.1e)  [λ=%.2e..%.2e]\n",
                    deg, it, t, tr, m.lambda_min(), m.lambda_max());
    }
    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper, Eigen::IncompleteCholesky<double>> cg;
    cg.setMaxIterations(cap); cg.setTolerance(etol); cg.compute(ea);
    long eit = 0; double eres = 0;
    const double t_e = best_ms([&]() {
        Eigen::VectorXd b(n); for (int k = 0; k < n; ++k) { b(k) = Bv[static_cast<std::size_t>(k)]; }
        Eigen::VectorXd x = cg.solve(b);
        eit = static_cast<long>(cg.iterations()); eres = (b - ea * x).norm() / b.norm();
    });
    std::printf("    Eigen IChol-CG          : %5ld it %8.2f ms (true r=%.1e)\n", eit, t_e, eres);
}

// Overlapping Schwarz vs its TRUE peer, block-Jacobi (Schwarz = block-Jacobi + overlap + exact
// local solves). Eigen ships NO domain-decomposition preconditioner of any kind, so the honest
// comparison is the OVERLAP value-add over block-Jacobi (both Cerid; breadth Eigen entirely
// lacks). AS (symmetric ⇒ SPD/PCG) / RAS (general ⇒ nonsym/BiCGSTAB). One-level (no coarse
// space ⇒ 1/H² degradation; the two-level coarse correction lives with AMG at v4k). The local
// dense-LU solves are parallel across subdomains (the distributed-memory design point).
static void run_schwarz(const char* name, const Mtx& mtx, bool spd)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int n = mtx.n; const double ctol = 1e-9; const int cap = 8000; const crd::u32 bsz = 64;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    std::vector<double> Bv(n, 1.0);

    auto solve = [&](const crd::hesap::LinearOp<double>& m, crd::usize& it, double& tr) {
        return best_ms([&]() {
            hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n)), x(&g_alloc, static_cast<crd::usize>(n));
            for (int k = 0; k < n; ++k) { b(static_cast<crd::usize>(k)) = Bv[static_cast<std::size_t>(k)]; }
            hi::IterativeOptions<double> opts; opts.rel_tol = ctol; opts.max_iter = static_cast<crd::usize>(cap);
            if (spd)
            {
                hi::KrylovWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
                auto r = hi::pcg<double>(op, m, b.span(), x.span(), opts, ws, &g_alloc);
                it = r.iterations; tr = true_resid(op, Bv, x, n);
            }
            else
            {
                hi::BicgstabWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n));
                auto r = hi::bicgstab<double>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
                it = r.iterations; tr = true_resid(op, Bv, x, n);
            }
        });
    };

    std::printf("  %-10s n=%-6d nnz=%-8zu  [Schwarz vs its peer block-Jacobi (Eigen has NO DD); %s; matched TRUE residual]\n",
                name, n, a.nnz(), spd ? "AS-PCG" : "RAS-BiCGSTAB");
    {
        hp::BlockJacobiPreconditioner<double> m(a, bsz, &g_alloc);
        crd::usize it = 0; double tr = 0; const double t = solve(m, it, tr);
        std::printf("    block-Jacobi (ov=0): %5zu it %8.2f ms (true r=%.1e)\n", it, t, tr);
    }
    const hp::SchwarzType ty = spd ? hp::SchwarzType::Additive : hp::SchwarzType::Restricted;
    for (crd::u32 ov : {1U, 2U})
    {
        hp::SchwarzPreconditioner<double> m(a, &g_alloc, bsz, ov, ty);
        crd::usize it = 0; double tr = 0; const double t = solve(m, it, tr);
        std::printf("    Schwarz overlap=%u : %5zu it %8.2f ms (true r=%.1e)  [%u subdomains, max|Ω|=%u]\n",
                    ov, it, t, tr, m.num_subdomains(), m.max_subdomain());
    }
}

// nonsym: SPAI-FGMRES (parallel setup, single-spmv apply) vs Eigen IncompleteLUT-GMRES.
static void run_spai(const char* name, const Mtx& mtx)
{
    if (!mtx.ok) { std::printf("  %-10s SKIP (not found)\n", name); return; }
    const int n = mtx.n; const double ctol = 1e-10; const double etol = 1e-8; const int cap = 8000; const int restart = 60;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<double> op(a, &g_alloc);
    std::vector<double> Bv(n, 1.0);

    const double set_s = best_ms([&]() { hp::SpaiPreconditioner<double> m(a, &g_alloc, hp::SpaiPattern::Static); (void)m.factor_nnz(); });
    const double set_a = best_ms([&]() { hp::SpaiPreconditioner<double> m(a, &g_alloc, hp::SpaiPattern::Adaptive, 0.3); (void)m.factor_nnz(); });

    hp::SpaiPreconditioner<double> sps(a, &g_alloc, hp::SpaiPattern::Static);
    hp::SpaiPreconditioner<double> spa(a, &g_alloc, hp::SpaiPattern::Adaptive, 0.3);
    auto solve = [&](const hp::SpaiPreconditioner<double>& m, crd::usize& it, double& tr) {
        return best_ms([&]() {
            hd::Vector<double> b(&g_alloc, static_cast<crd::usize>(n)), x(&g_alloc, static_cast<crd::usize>(n));
            for (int k = 0; k < n; ++k) { b(static_cast<crd::usize>(k)) = Bv[static_cast<std::size_t>(k)]; }
            hi::IterativeOptions<double> opts; opts.rel_tol = ctol; opts.max_iter = static_cast<crd::usize>(cap);
            hi::GmresWorkspace<double> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(restart));
            auto r = hi::fgmres<double>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
            it = r.iterations; tr = true_resid(op, Bv, x, n);
        });
    };
    crd::usize its = 0, ita = 0; double rs = 0, ra = 0;
    const double t_s = solve(sps, its, rs);
    const double t_a = solve(spa, ita, ra);

    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    const double set_e = best_ms([&]() { Eigen::IncompleteLUT<double> lu; lu.setDroptol(1e-4); lu.setFillfactor(10); lu.compute(ea); });
    Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::IncompleteLUT<double>> g;
    g.set_restart(restart); g.preconditioner().setDroptol(1e-4); g.preconditioner().setFillfactor(10);
    g.setMaxIterations(cap); g.setTolerance(etol); g.compute(ea);
    long eit = 0; double eres = 0;
    const double t_e = best_ms([&]() {
        Eigen::VectorXd b(n); for (int k = 0; k < n; ++k) { b(k) = Bv[static_cast<std::size_t>(k)]; }
        Eigen::VectorXd x = g.solve(b);
        eit = static_cast<long>(g.iterations()); eres = (b - ea * x).norm() / b.norm();
    });

    std::printf("  %-10s n=%-6d nnz=%-8zu  [SPAI-FGMRES(%d) vs Eigen IncompleteLUT-GMRES]\n", name, n, a.nnz(), restart);
    std::printf("    SETUP  Cerid SPAI(static) %7.2f ms   SPAI(adapt) %7.2f ms   Eigen ILUT %7.2f ms   [Eigen/Cerid-static = %.2fx]\n",
                set_s, set_a, set_e, set_s > 0 ? set_e / set_s : 0.0);
    std::printf("    SOLVE  Cerid static %5zu it %7.2f ms (r=%.1e)  adapt %5zu it %7.2f ms (r=%.1e)\n", its, t_s, rs, ita, t_a, ra);
    std::printf("    SOLVE  Eigen ILUT   %5ld it %7.2f ms (r=%.1e)\n", eit, t_e, eres);
    std::printf("    fill   static %.2fx  adapt %.2fx  (nnz(M)/nnz(A))\n",
                static_cast<double>(sps.factor_nnz()) / static_cast<double>(a.nnz()),
                static_cast<double>(spa.factor_nnz()) / static_cast<double>(a.nnz()));
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap SPAI / FSPAI vs Eigen IncompleteCholesky / IncompleteLUT (real SuiteSparse + synthetic)\n");
    std::printf("  Cerid setup is PARALLEL (per-column); apply is matrix-free spmv (no triangular solve). Both honest.\n");
    const std::string base = std::string(CRD_SUITESPARSE_DIR);
    std::printf(" --- SPD: FSPAI (factored, M=L·Lᴴ) ---\n");
    run_fspai("bcsstk13", read_mtx(base + "/bcsstk13/bcsstk13.mtx"));
    run_fspai("bcsstk24", read_mtx(base + "/bcsstk24/bcsstk24.mtx"));
    run_fspai("bcsstk25", read_mtx(base + "/bcsstk25/bcsstk25.mtx"));
    run_fspai("lap2d-160", make_laplace_2d(160));
    std::printf(" --- SPD: Chebyshev polynomial (matrix-free, no tri-solve; Eigen ships none) ---\n");
    run_chebyshev("bcsstk13", read_mtx(base + "/bcsstk13/bcsstk13.mtx"));
    run_chebyshev("lap2d-160", make_laplace_2d(160));
    std::printf(" --- Schwarz domain decomposition (parallel local solves; AS=SPD, RAS=nonsym) ---\n");
    run_schwarz("lap2d-200", make_laplace_2d(200), /*spd=*/true);
    run_schwarz("cd2d-200", make_conv_diff_2d(200, 0.3), /*spd=*/false);
    std::printf(" --- nonsym: classical SPAI (M ≈ A⁻¹) ---\n");
    run_spai("gemat11", read_mtx(base + "/gemat11/gemat11.mtx"));
    run_spai("sherman3", read_mtx(base + "/sherman3/sherman3.mtx"));
    run_spai("cd2d-150", make_conv_diff_2d(150, 0.3));
    crd::jobs::shutdown();
    return 0;
}
