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
//
// crd conventions throughout: crd::containers::Array (never std::vector),
// crd::platform::fs::read_file_text + manual parse (never std::ifstream/string),
// a named TlsfAllocator (never malloc), crd::f64/usize/i32/i64 (never raw types).
// Raw double survives ONLY at the Eigen C++ API boundary; crd::f64 IS double.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
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

static Mtx make_laplace_2d(crd::i32 g) // SPD 5-point
{
    Mtx m;
    m.n = g * g;
    m.ok = true;
    for (crd::i32 y = 0; y < g; ++y)
        for (crd::i32 x = 0; x < g; ++x)
        {
            const crd::i32 i = y * g + x;
            m.trips.push_back({i, i, 4.0});
            if (x + 1 < g)
            {
                m.trips.push_back({i, i + 1, -1.0});
            }
            if (x > 0)
            {
                m.trips.push_back({i, i - 1, -1.0});
            }
            if (y + 1 < g)
            {
                m.trips.push_back({i, i + g, -1.0});
            }
            if (y > 0)
            {
                m.trips.push_back({i, i - g, -1.0});
            }
        }
    return m;
}

static Mtx make_conv_diff_2d(crd::i32 g, crd::f64 beta) // nonsym 5-point
{
    Mtx m;
    m.n = g * g;
    m.ok = true;
    for (crd::i32 y = 0; y < g; ++y)
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
    return m;
}

// Helper: TRUE relative residual ‖b − A·x‖₂ / ‖b‖₂ via the operator (apples-to-apples with
// Eigen's (b − A·x).norm()/b.norm()). The Krylov recurrence residual can drift below the true
// residual on ill-conditioned A, so the FAIR comparison is BOTH sides' TRUE residual; Cerid is
// driven to a tight rel_tol so its true residual lands at the same accuracy Eigen reaches.
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

// SPD: FSPAI-PCG (parallel setup, no tri-solve apply) vs Eigen IncompleteCholesky-CG. Matched
// TRUE-residual accuracy: Cerid rel_tol=1e-12 (drives true residual to Eigen's regime), Eigen
// tol=1e-8; both TRUE residuals + time-per-iteration reported (the per-iter ratio is the
// structural no-tri-solve win, the total-wall ratio the matched-accuracy headline).
static void run_fspai(const char* name, const Mtx& mtx)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 ctol = 1e-12;
    const crd::f64 etol = 1e-8;
    const crd::i32 cap = 8000;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    crd::containers::Array<crd::f64> Bv(&g_alloc);
    Bv.resize(static_cast<crd::usize>(n), 1.0);

    const crd::f64 set_s = best_ms(
        [&]()
        {
            hp::FspaiPreconditioner<crd::f64> m(a, &g_alloc, hp::SpaiPattern::Static);
            (void)m.factor_nnz();
        });
    const crd::f64 set_a = best_ms(
        [&]()
        {
            hp::FspaiPreconditioner<crd::f64> m(a, &g_alloc, hp::SpaiPattern::Adaptive, 0.05);
            (void)m.factor_nnz();
        });

    hp::FspaiPreconditioner<crd::f64> fss(a, &g_alloc, hp::SpaiPattern::Static);
    hp::FspaiPreconditioner<crd::f64> fsa(a, &g_alloc, hp::SpaiPattern::Adaptive, 0.05);
    auto solve = [&](const hp::FspaiPreconditioner<crd::f64>& m, crd::usize& it, crd::f64& tr)
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
                opts.rel_tol = ctol;
                opts.max_iter = static_cast<crd::usize>(cap);
                hi::KrylovWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
                auto r = hi::pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &g_alloc);
                it = r.iterations;
                tr = true_resid(op, Bv, x, n);
            });
    };
    crd::usize its = 0;
    crd::usize ita = 0;
    crd::f64 rs = 0;
    crd::f64 ra = 0;
    const crd::f64 t_s = solve(fss, its, rs);
    const crd::f64 t_a = solve(fsa, ita, ra);

    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    const crd::f64 set_e = best_ms(
        [&]()
        {
            Eigen::IncompleteCholesky<double> ic;
            ic.compute(ea);
        });
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                             Eigen::IncompleteCholesky<double>>
        cg;
    cg.setMaxIterations(cap);
    cg.setTolerance(etol);
    cg.compute(ea);
    crd::i64 eit = 0;
    crd::f64 eres = 0;
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

    const crd::f64 mspit_c = its > 0 ? t_s / static_cast<crd::f64>(its) : 0.0;
    const crd::f64 mspit_e = eit > 0 ? t_e / static_cast<crd::f64>(eit) : 0.0;
    std::printf("  %-10s n=%-6d nnz=%-8zu  [FSPAI-PCG vs Eigen IncompleteCholesky-CG; matched TRUE residual]\n", name,
                n, a.nnz());
    std::printf("    SETUP  Cerid FSPAI(static) %7.2f ms  FSPAI(adapt) %7.2f ms   Eigen IChol %7.2f ms   "
                "[Eigen/Cerid-static = %.2fx]\n",
                set_s, set_a, set_e, set_s > 0 ? set_e / set_s : 0.0);
    std::printf(
        "    SOLVE  Cerid static %5zu it %7.2f ms (true r=%.1e, %.4f ms/it)  adapt %5zu it %7.2f ms (true r=%.1e)\n",
        its, t_s, rs, mspit_c, ita, t_a, ra);
    std::printf("    SOLVE  Eigen IChol  %5lld it %7.2f ms (true r=%.1e, %.4f ms/it)   [Eigen/Cerid-static = %.2fx "
                "wall, %.2fx per-it]\n",
                static_cast<long long>(eit), t_e, eres, mspit_e, t_s > 0 ? t_e / t_s : 0.0,
                mspit_c > 0 ? mspit_e / mspit_c : 0.0);
    std::printf("    fill   static %.2fx  adapt %.2fx  (nnz(factor)/nnz(A))\n",
                static_cast<crd::f64>(fss.factor_nnz()) / static_cast<crd::f64>(a.nnz()),
                static_cast<crd::f64>(fsa.factor_nnz()) / static_cast<crd::f64>(a.nnz()));
}

// SPD: Chebyshev-PCG (matrix-free polynomial, NO triangular solve, deg spmv/apply) vs Eigen
// IncompleteCholesky-CG. Eigen ships NO polynomial preconditioner -> breadth; Chebyshev is a
// weaker per-apply preconditioner (more outer iters) but each apply is parallel + matrix-free +
// GPU-mappable, and it is the AMG smoother (v4k). Honest: iters + wall both reported.
static void run_chebyshev(const char* name, const Mtx& mtx)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 ctol = 1e-9;
    const crd::f64 etol = 1e-8;
    const crd::i32 cap = 3000;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    crd::containers::Array<crd::f64> Bv(&g_alloc);
    Bv.resize(static_cast<crd::usize>(n), 1.0);

    std::printf("  %-10s n=%-6d nnz=%-8zu  [Chebyshev-PCG (matrix-free) vs Eigen IncompleteCholesky-CG; matched TRUE "
                "residual]\n",
                name, n, a.nnz());
    for (crd::u32 deg : {4U, 8U, 16U})
    {
        hp::ChebyshevPreconditioner<crd::f64> m(a, &g_alloc, deg);
        crd::usize it = 0;
        crd::f64 tr = 0;
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
                hi::KrylovWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
                auto r = hi::pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &g_alloc);
                it = r.iterations;
                tr = true_resid(op, Bv, x, n);
            });
        std::printf("    Cerid Chebyshev(%2u)-PCG : %5zu it %8.2f ms (true r=%.1e)  [λ=%.2e..%.2e]\n", deg, it, t, tr,
                    m.lambda_min(), m.lambda_max());
    }
    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                             Eigen::IncompleteCholesky<double>>
        cg;
    cg.setMaxIterations(cap);
    cg.setTolerance(etol);
    cg.compute(ea);
    crd::i64 eit = 0;
    crd::f64 eres = 0;
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
    std::printf("    Eigen IChol-CG          : %5lld it %8.2f ms (true r=%.1e)\n", static_cast<long long>(eit), t_e,
                eres);
}

// Overlapping Schwarz vs its TRUE peer, block-Jacobi (Schwarz = block-Jacobi + overlap + exact
// local solves). Eigen ships NO domain-decomposition preconditioner of any kind, so the honest
// comparison is the OVERLAP value-add over block-Jacobi (both Cerid; breadth Eigen entirely
// lacks). AS (symmetric ⇒ SPD/PCG) / RAS (general ⇒ nonsym/BiCGSTAB). One-level (no coarse
// space ⇒ 1/H² degradation; the two-level coarse correction lives with AMG at v4k). The local
// dense-LU solves are parallel across subdomains (the distributed-memory design point).
static void run_schwarz(const char* name, const Mtx& mtx, bool spd)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 ctol = 1e-9;
    const crd::i32 cap = 8000;
    const crd::u32 bsz = 64;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    crd::containers::Array<crd::f64> Bv(&g_alloc);
    Bv.resize(static_cast<crd::usize>(n), 1.0);

    auto solve = [&](const crd::hesap::LinearOp<crd::f64>& m, crd::usize& it, crd::f64& tr)
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
                opts.rel_tol = ctol;
                opts.max_iter = static_cast<crd::usize>(cap);
                if (spd)
                {
                    hi::KrylovWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
                    auto r = hi::pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &g_alloc);
                    it = r.iterations;
                    tr = true_resid(op, Bv, x, n);
                }
                else
                {
                    hi::BicgstabWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
                    auto r = hi::bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
                    it = r.iterations;
                    tr = true_resid(op, Bv, x, n);
                }
            });
    };

    std::printf(
        "  %-10s n=%-6d nnz=%-8zu  [Schwarz vs its peer block-Jacobi (Eigen has NO DD); %s; matched TRUE residual]\n",
        name, n, a.nnz(), spd ? "AS-PCG" : "RAS-BiCGSTAB");
    {
        hp::BlockJacobiPreconditioner<crd::f64> m(a, bsz, &g_alloc);
        crd::usize it = 0;
        crd::f64 tr = 0;
        const crd::f64 t = solve(m, it, tr);
        std::printf("    block-Jacobi (ov=0): %5zu it %8.2f ms (true r=%.1e)\n", it, t, tr);
    }
    const hp::SchwarzType ty = spd ? hp::SchwarzType::Additive : hp::SchwarzType::Restricted;
    for (crd::u32 ov : {1U, 2U})
    {
        hp::SchwarzPreconditioner<crd::f64> m(a, &g_alloc, bsz, ov, ty);
        crd::usize it = 0;
        crd::f64 tr = 0;
        const crd::f64 t = solve(m, it, tr);
        std::printf("    Schwarz overlap=%u : %5zu it %8.2f ms (true r=%.1e)  [%u subdomains, max|Ω|=%u]\n", ov, it, t,
                    tr, m.num_subdomains(), m.max_subdomain());
    }
}

// nonsym: SPAI-FGMRES (parallel setup, single-spmv apply) vs Eigen IncompleteLUT-GMRES.
static void run_spai(const char* name, const Mtx& mtx)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 ctol = 1e-10;
    const crd::f64 etol = 1e-8;
    const crd::i32 cap = 8000;
    const crd::i32 restart = 60;
    auto a = to_csr(mtx);
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    crd::containers::Array<crd::f64> Bv(&g_alloc);
    Bv.resize(static_cast<crd::usize>(n), 1.0);

    const crd::f64 set_s = best_ms(
        [&]()
        {
            hp::SpaiPreconditioner<crd::f64> m(a, &g_alloc, hp::SpaiPattern::Static);
            (void)m.factor_nnz();
        });
    const crd::f64 set_a = best_ms(
        [&]()
        {
            hp::SpaiPreconditioner<crd::f64> m(a, &g_alloc, hp::SpaiPattern::Adaptive, 0.3);
            (void)m.factor_nnz();
        });

    hp::SpaiPreconditioner<crd::f64> sps(a, &g_alloc, hp::SpaiPattern::Static);
    hp::SpaiPreconditioner<crd::f64> spa(a, &g_alloc, hp::SpaiPattern::Adaptive, 0.3);
    auto solve = [&](const hp::SpaiPreconditioner<crd::f64>& m, crd::usize& it, crd::f64& tr)
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
                opts.rel_tol = ctol;
                opts.max_iter = static_cast<crd::usize>(cap);
                hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(restart));
                auto r = hi::fgmres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
                it = r.iterations;
                tr = true_resid(op, Bv, x, n);
            });
    };
    crd::usize its = 0;
    crd::usize ita = 0;
    crd::f64 rs = 0;
    crd::f64 ra = 0;
    const crd::f64 t_s = solve(sps, its, rs);
    const crd::f64 t_a = solve(spa, ita, ra);

    Eigen::SparseMatrix<double> ea = to_eigen(mtx);
    const crd::f64 set_e = best_ms(
        [&]()
        {
            Eigen::IncompleteLUT<double> lu;
            lu.setDroptol(1e-4);
            lu.setFillfactor(10);
            lu.compute(ea);
        });
    Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::IncompleteLUT<double>> g;
    g.set_restart(restart);
    g.preconditioner().setDroptol(1e-4);
    g.preconditioner().setFillfactor(10);
    g.setMaxIterations(cap);
    g.setTolerance(etol);
    g.compute(ea);
    crd::i64 eit = 0;
    crd::f64 eres = 0;
    const crd::f64 t_e = best_ms(
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

    std::printf("  %-10s n=%-6d nnz=%-8zu  [SPAI-FGMRES(%d) vs Eigen IncompleteLUT-GMRES]\n", name, n, a.nnz(),
                restart);
    std::printf("    SETUP  Cerid SPAI(static) %7.2f ms   SPAI(adapt) %7.2f ms   Eigen ILUT %7.2f ms   "
                "[Eigen/Cerid-static = %.2fx]\n",
                set_s, set_a, set_e, set_s > 0 ? set_e / set_s : 0.0);
    std::printf("    SOLVE  Cerid static %5zu it %7.2f ms (r=%.1e)  adapt %5zu it %7.2f ms (r=%.1e)\n", its, t_s, rs,
                ita, t_a, ra);
    std::printf("    SOLVE  Eigen ILUT   %5lld it %7.2f ms (r=%.1e)\n", static_cast<long long>(eit), t_e, eres);
    std::printf("    fill   static %.2fx  adapt %.2fx  (nnz(M)/nnz(A))\n",
                static_cast<crd::f64>(sps.factor_nnz()) / static_cast<crd::f64>(a.nnz()),
                static_cast<crd::f64>(spa.factor_nnz()) / static_cast<crd::f64>(a.nnz()));
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap SPAI / FSPAI vs Eigen IncompleteCholesky / IncompleteLUT (real SuiteSparse + synthetic)\n");
    std::printf(
        "  Cerid setup is PARALLEL (per-column); apply is matrix-free spmv (no triangular solve). Both honest.\n");
    std::printf(" --- SPD: FSPAI (factored, M=L·Lᴴ) ---\n");
    run_fspai("bcsstk13", load_ss("bcsstk13"));
    run_fspai("bcsstk24", load_ss("bcsstk24"));
    run_fspai("bcsstk25", load_ss("bcsstk25"));
    run_fspai("lap2d-160", make_laplace_2d(160));
    std::printf(" --- SPD: Chebyshev polynomial (matrix-free, no tri-solve; Eigen ships none) ---\n");
    run_chebyshev("bcsstk13", load_ss("bcsstk13"));
    run_chebyshev("lap2d-160", make_laplace_2d(160));
    std::printf(" --- Schwarz domain decomposition (parallel local solves; AS=SPD, RAS=nonsym) ---\n");
    run_schwarz("lap2d-200", make_laplace_2d(200), /*spd=*/true);
    run_schwarz("cd2d-200", make_conv_diff_2d(200, 0.3), /*spd=*/false);
    std::printf(" --- nonsym: classical SPAI (M ≈ A⁻¹) ---\n");
    run_spai("gemat11", load_ss("gemat11"));
    run_spai("sherman3", load_ss("sherman3"));
    run_spai("cd2d-150", make_conv_diff_2d(150, 0.3));
    crd::jobs::shutdown();
    return 0;
}
