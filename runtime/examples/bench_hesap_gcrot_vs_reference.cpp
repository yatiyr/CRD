// bench_hesap_gcrot_vs_reference.cpp -- Phase 3.1.6 v4e-2.
//
// GCROT(m,k) recycling GMRES vs:
//   - Cerid GMRES(m)  (the honest recycling BASELINE: same operator + precond,
//     no recycle space — the iters-saved + time-saved comparison),
//   - Eigen GMRES(m)  (frontier reference; no recycling Krylov exists in Eigen).
// Reports iterations, wall time, and the TRUE residual ‖b−Ax‖/‖b‖ for all three.
// Recycling trades a per-cycle overhead (the recycle projection + the eig-based
// SVD truncation) for fewer iterations; this bench shows whether that pays off in
// WALL TIME on a single solve. The bigger payoff (iters-saved across a parametric
// sequence) is the v4e-3 cross-solve gate.
//
// Matrices: real SuiteSparse (gemat11, sherman3, bcsstk13/24/25) + a synthetic
// small-eigenvalue-cluster matrix where deflation/recycling provably helps.
//
// Gated behind CRD_BUILD_HESAP_VS_REFERENCE (CRD_SUITESPARSE_DIR).
//
// crd conventions throughout: crd::containers::Array (never std::vector),
// crd::platform::fs::read_file_text + manual parse (never std::ifstream/string),
// a named TlsfAllocator (never malloc), crd::f64/usize/i32/i64 (never raw types).
// Raw double survives ONLY at the Eigen C++ API boundary; crd::f64 IS double.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/gcrot.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/Sparse>
#include <algorithm>
#include <chrono>
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

// Matrix-Market reader, crd-native: read the file into a crd::String via the platform
// filesystem, hand-parse with strtol/strtod (no std::ifstream/string/sstream); DATA → crd::Array.
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
        if (*p == '%')
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

// Synthetic small-eigenvalue-cluster matrix (matches the unit-test fixture):
// nsmall small, well-separated eigenvalues that small-m GMRES re-discovers each
// restart and GCROT deflates into the recycle space.
static Mtx make_cluster(crd::i32 n, crd::i32 nsmall)
{
    Mtx m;
    m.n = n;
    for (crd::i32 i = 0; i < n; ++i)
    {
        m.trips.push_back({i, i, (i < nsmall) ? 0.1 : 10.0});
        if (i + 1 < n)
        {
            m.trips.push_back({i, i + 1, -1.0});
        }
        if (i > 0)
        {
            m.trips.push_back({i, i - 1, -0.7});
        }
    }
    m.ok = true;
    return m;
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

static void run(const char* name, const Mtx& mtx, crd::i32 inner, crd::i32 recycle)
{
    if (!mtx.ok)
    {
        std::printf("  %-12s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 tol = 1e-9;
    const crd::i32 cap = 8000;

    hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    hp::JacobiPreconditioner<crd::f64> jac(a, &g_alloc);

    hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n));
    b.fill(1.0);
    hi::IterativeOptions<crd::f64> opts;
    opts.rel_tol = tol;
    opts.max_iter = static_cast<crd::usize>(cap);

    auto cer_true = [&](const hd::Vector<crd::f64>& x) -> crd::f64
    {
        hd::Vector<crd::f64> ax(&g_alloc, static_cast<crd::usize>(n));
        (void)op.apply(x.span(), ax.span());
        hd::Vector<crd::f64> rr(&g_alloc, static_cast<crd::usize>(n));
        for (crd::i32 i = 0; i < n; ++i)
        {
            rr(i) = b(i) - ax(i);
        }
        return hd::nrm2<crd::f64>(rr.span()) / hd::nrm2<crd::f64>(b.span());
    };

    crd::usize gm_it = 0;
    crd::f64 gm_res = 0;
    const crd::f64 t_gm = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(inner));
            auto res = hi::fgmres<crd::f64>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc);
            gm_it = res.iterations;
            gm_res = cer_true(x);
        });

    crd::usize gc_it = 0;
    crd::f64 gc_res = 0;
    const crd::f64 t_gc = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            hi::GcrotWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(inner),
                                            static_cast<crd::usize>(recycle));
            // Right-precond GCROT (matches GMRES's Jacobi for an apples-to-apples baseline).
            auto res = hi::gcrot<crd::f64>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc);
            gc_it = res.iterations;
            gc_res = cer_true(x);
        });

    crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        et.push_back(Eigen::Triplet<double>(t.r, t.c, t.v));
    }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.data(), et.data() + et.size());
    Eigen::VectorXd eb = Eigen::VectorXd::Ones(n);

    crd::i64 eg_it = 0;
    crd::f64 eg_res = 0;
    const crd::f64 t_eg = best_ms(
        [&]()
        {
            Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::DiagonalPreconditioner<double>> s;
            s.setMaxIterations(cap);
            s.setTolerance(tol);
            s.set_restart(inner);
            s.compute(ea);
            Eigen::VectorXd x = s.solve(eb);
            eg_it = static_cast<crd::i64>(s.iterations());
            eg_res = (eb - ea * x).norm() / eb.norm();
        });

    std::printf("  %-12s n=%-6d nnz=%-8zu  inner=%d recycle=%d\n", name, n, a.nnz(), inner, recycle);
    std::printf("    Cerid GMRES(m)   : %5zu it  %8.2f ms  (r=%.1e)\n", gm_it, t_gm, gm_res);
    std::printf("    Cerid GCROT(m,k) : %5zu it  %8.2f ms  (r=%.1e)  vs our GMRES: %.2fx iters, %.2fx time %s\n", gc_it,
                t_gc, gc_res, gm_it > 0 ? static_cast<crd::f64>(gm_it) / static_cast<crd::f64>(gc_it) : 0.0,
                t_gc > 0 ? t_gm / t_gc : 0.0, (t_gc <= t_gm ? "WIN" : "(overhead)"));
    std::printf("    Eigen GMRES(m)   : %5lld it  %8.2f ms  (r=%.1e)  [frontier ref; no recycling]\n",
                static_cast<long long>(eg_it), t_eg, eg_res);
}

// Cross-solve recycling (v4e-3): a shift sequence A_i = A_base + α_i·I. Fresh
// GMRES(m) per solve vs GCROT(m,k) with a PERSISTENT recycle space carried across
// the whole sequence. This is where the recycle setup cost amortizes — the de
// Sturler payoff and the honest performance headline for recycling.
static void run_sequence(const Mtx& base, crd::i32 nsmall, crd::i32 inner, crd::i32 recycle, crd::i32 nsolves)
{
    const crd::i32 n = base.n;
    const crd::f64 tol = 1e-9;
    const crd::i32 cap = 8000;

    hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n));
    b.fill(1.0);
    hi::IterativeOptions<crd::f64> opts;
    opts.rel_tol = tol;
    opts.max_iter = static_cast<crd::usize>(cap);

    auto build = [&](crd::f64 shift)
    {
        hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
        for (const Trip& t : base.trips)
        {
            const crd::f64 v = t.v + (t.r == t.c ? shift : 0.0);
            tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), v);
        }
        return tb.compress();
    };
    (void)nsmall;

    // Fresh GMRES(m) per system (no memory across solves).
    crd::usize gmres_it = 0;
    const crd::f64 t_gmres = best_ms(
        [&]()
        {
            gmres_it = 0;
            for (crd::i32 s = 0; s < nsolves; ++s)
            {
                auto a = build(0.04 * s);
                hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
                hp::JacobiPreconditioner<crd::f64> jac(a, &g_alloc);
                hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
                hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(inner));
                auto r = hi::fgmres<crd::f64>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc);
                gmres_it += r.iterations;
            }
        },
        1);

    // GCROT(m,k) with a PERSISTENT recycle space across the sequence.
    crd::usize gcrot_it = 0;
    const crd::f64 t_gcrot = best_ms(
        [&]()
        {
            gcrot_it = 0;
            hi::RecycleSpace<crd::f64> rs(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(recycle));
            hi::GcrotWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(inner),
                                            static_cast<crd::usize>(recycle));
            for (crd::i32 s = 0; s < nsolves; ++s)
            {
                auto a = build(0.04 * s);
                hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
                hp::JacobiPreconditioner<crd::f64> jac(a, &g_alloc);
                hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
                auto r = hi::gcrot_recycled<crd::f64>(op, &jac, b.span(), x.span(), opts, ws, rs, &g_alloc);
                gcrot_it += r.iterations;
            }
        },
        1);

    std::printf("  SEQUENCE (%d shifted solves, A_i = A_base + 0.04i*I)  inner=%d recycle=%d\n", nsolves, inner,
                recycle);
    std::printf("    fresh GMRES(m) per solve  : %6zu total it  %8.2f ms\n", gmres_it, t_gmres);
    std::printf("    GCROT(m,k) recycled       : %6zu total it  %8.2f ms  ->  %.2fx fewer iters, %.2fx faster %s\n",
                gcrot_it, t_gcrot,
                gcrot_it > 0 ? static_cast<crd::f64>(gmres_it) / static_cast<crd::f64>(gcrot_it) : 0.0,
                t_gcrot > 0 ? t_gmres / t_gcrot : 0.0, (t_gcrot <= t_gmres ? "WIN" : "(overhead)"));
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap GCROT(m,k) vs Cerid GMRES(m) [recycling baseline] + Eigen GMRES(m) [frontier ref]\n");
    std::printf("  Jacobi-preconditioned; true residual ‖b-Ax‖/‖b‖ recomputed; iters-saved = recycling payoff.\n");
    std::printf(
        "  (Eigen has NO recycling Krylov; the recycling crush is iters-saved, compounding cross-solve at v4e-3.)\n");

    // Synthetic cluster: recycling's mechanism in isolation (small m forces restarts
    // that re-discover the small modes; GCROT deflates them).
    run("cluster", make_cluster(2000, 8), /*inner=*/8, /*recycle=*/8);

    // Real SuiteSparse, small inner m so restarted GMRES has room to stagnate.
    const char* names[] = {"gemat11", "sherman3", "bcsstk13", "bcsstk24"};
    for (const char* nm : names)
    {
        run(nm, load_ss(nm), /*inner=*/30, /*recycle=*/30);
    }

    // v4e-3 headline: cross-solve recycling across a parametric shift sequence.
    std::printf("\n-- cross-solve recycling (v4e-3): the recycle setup amortizes across the sequence --\n");
    run_sequence(make_cluster(2000, 8), /*nsmall=*/8, /*inner=*/8, /*recycle=*/8, /*nsolves=*/6);

    crd::jobs::shutdown();
    return 0;
}
