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
#include <crd/hesap/iterative/block_bicgstab.hpp>
#include <crd/hesap/iterative/block_gmres.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
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

// Nonsymmetric convection-diffusion (diag dd, super -1+g, sub -1-g). 3 nnz/row =
// cheap operator (per-column owns it). A larger band ⇒ expensive operator.
static Mtx make_conv_diff(crd::i32 n, crd::f64 dd, crd::f64 g, crd::i32 hb)
{
    Mtx m;
    m.n = n;
    for (crd::i32 i = 0; i < n; ++i)
    {
        m.trips.push_back({i, i, dd});
        for (crd::i32 d = 1; d <= hb; ++d)
        {
            if (i + d < n)
            {
                m.trips.push_back({i, i + d, -1.0 + g / d});
            }
            if (i - d >= 0)
            {
                m.trips.push_back({i, i - d, -1.0 - g / d});
            }
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

static crd::f64 block_worst_res(hs::ParallelSpmmLinearOp<crd::f64>& op, const hd::Vector<crd::f64>& x,
                                const crd::containers::Array<crd::f64>& B, crd::i32 n, crd::i32 s)
{
    hd::Vector<crd::f64> ax(&g_alloc, static_cast<crd::usize>(n) * s);
    (void)op.apply_block(x.span(), static_cast<crd::u32>(s), ax.span(), static_cast<crd::u32>(s),
                         static_cast<crd::u32>(s));
    crd::f64 worst = 0;
    for (crd::i32 j = 0; j < s; ++j)
    {
        crd::f64 rn = 0;
        crd::f64 bn = 0;
        for (crd::i32 k = 0; k < n; ++k)
        {
            const crd::f64 d = B[static_cast<crd::usize>(k) * s + j] - ax(static_cast<crd::usize>(k) * s + j);
            rn += d * d;
            bn += B[static_cast<crd::usize>(k) * s + j] * B[static_cast<crd::usize>(k) * s + j];
        }
        worst = std::max(worst, std::sqrt(rn) / std::sqrt(bn));
    }
    return worst;
}

enum class Kind
{
    Gmres,
    Bicgstab
};

static void run(const char* name, const Mtx& mtx, crd::i32 s, Kind kind)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 tol = 1e-8;
    const crd::i32 cap = 4000;
    const crd::i32 restart = 60;

    hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();
    hs::ParallelSpmmLinearOp<crd::f64> bop(a);
    hs::ParallelSparseLinearOp<crd::f64> colop(a, &g_alloc);

    crd::containers::Array<crd::f64> B(&g_alloc);
    B.resize(static_cast<crd::usize>(n) * s);
    for (crd::i32 k = 0; k < n; ++k)
    {
        for (crd::i32 j = 0; j < s; ++j)
        {
            B[static_cast<crd::usize>(k) * s + j] = std::sin(0.21 * k * (j + 1) + 0.4 * j) + 0.3 * ((k + 2 * j) % 5);
        }
    }

    // 1. Cerid block solver.
    crd::usize blk_it = 0;
    crd::f64 blk_res = 0;
    const crd::f64 t_blk = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n) * s);
            for (crd::usize i = 0; i < B.size(); ++i)
            {
                b(i) = B[i];
            }
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n) * s);
            hi::IterativeOptions<crd::f64> opts;
            opts.rel_tol = tol;
            opts.max_iter = static_cast<crd::usize>(cap);
            if (kind == Kind::Gmres)
            {
                hi::BlockGmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::u32>(s),
                                                     restart);
                blk_it = hi::block_gmres<crd::f64>(bop, b.span(), x.span(), opts, ws, &g_alloc).iterations;
            }
            else
            {
                hi::BlockBicgstabWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::u32>(s));
                blk_it = hi::block_bicgstab<crd::f64>(bop, b.span(), x.span(), opts, ws, &g_alloc).iterations;
            }
            blk_res = block_worst_res(bop, x, B, n, s);
        });

    // 2. Cerid per-column (FGMRES / BiCGSTAB).
    crd::i64 col_it = 0;
    crd::f64 col_res = 0;
    const crd::f64 t_col = best_ms(
        [&]()
        {
            col_it = 0;
            crd::f64 worst = 0;
            hd::Vector<crd::f64> bj(&g_alloc, static_cast<crd::usize>(n));
            hd::Vector<crd::f64> xj(&g_alloc, static_cast<crd::usize>(n));
            for (crd::i32 j = 0; j < s; ++j)
            {
                for (crd::i32 k = 0; k < n; ++k)
                {
                    bj(static_cast<crd::usize>(k)) = B[static_cast<crd::usize>(k) * s + j];
                    xj(static_cast<crd::usize>(k)) = 0.0;
                }
                hi::IterativeOptions<crd::f64> opts;
                opts.rel_tol = tol;
                opts.max_iter = static_cast<crd::usize>(cap);
                hi::IterativeResult<crd::f64> r(&g_alloc);
                if (kind == Kind::Gmres)
                {
                    hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n),
                                                    static_cast<crd::usize>(restart));
                    r = hi::gmres<crd::f64>(colop, bj.span(), xj.span(), opts, ws, &g_alloc);
                }
                else
                {
                    hi::BicgstabWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
                    r = hi::bicgstab<crd::f64>(colop, nullptr, bj.span(), xj.span(), opts, ws, &g_alloc);
                }
                col_it += static_cast<crd::i64>(r.iterations);
                crd::f64 rn = 0;
                crd::f64 bn = 0;
                hd::Vector<crd::f64> ax(&g_alloc, static_cast<crd::usize>(n));
                (void)colop.apply(xj.span(), ax.span());
                for (crd::i32 k = 0; k < n; ++k)
                {
                    crd::f64 d = bj(static_cast<crd::usize>(k)) - ax(static_cast<crd::usize>(k));
                    rn += d * d;
                    bn += bj(static_cast<crd::usize>(k)) * bj(static_cast<crd::usize>(k));
                }
                worst = std::max(worst, std::sqrt(rn) / std::sqrt(bn));
            }
            col_res = worst;
        });

    // 3. Eigen per-column (GMRES / BiCGSTAB), single-threaded; compute() hoisted out.
    crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        et.push_back(Eigen::Triplet<double>(t.r, t.c, t.v));
    }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.data(), et.data() + et.size());

    crd::i64 eig_it = 0;
    crd::f64 eig_res = 0;
    const char* eref = "";
    if (kind == Kind::Gmres)
    {
        Eigen::GMRES<Eigen::SparseMatrix<double>, Eigen::IdentityPreconditioner> g;
        g.set_restart(restart);
        g.setMaxIterations(cap);
        g.setTolerance(tol);
        g.compute(ea);
        eref = "Eigen GMRES";
        const crd::f64 t = best_ms(
            [&]()
            {
                eig_it = 0;
                crd::f64 worst = 0;
                for (crd::i32 j = 0; j < s; ++j)
                {
                    Eigen::VectorXd bj(n);
                    for (crd::i32 k = 0; k < n; ++k)
                    {
                        bj(k) = B[static_cast<crd::usize>(k) * s + j];
                    }
                    Eigen::VectorXd xj = g.solve(bj);
                    eig_it += static_cast<crd::i64>(g.iterations());
                    worst = std::max(worst, (bj - ea * xj).norm() / bj.norm());
                }
                eig_res = worst;
            });
        std::printf("  %-10s n=%-6d nnz=%-8zu s=%d  [block-GMRES]\n", name, n, a.nnz(), s);
        std::printf(
            "    Cerid block-GMRES : %5zu block-iters %8.2f ms (r=%.1e)  [Eigen/blk = %.2fx time, %.2fx A-passes]\n",
            blk_it, t_blk, blk_res, t_blk > 0 ? t / t_blk : 0.0,
            blk_it > 0 ? static_cast<crd::f64>(eig_it) / static_cast<crd::f64>(blk_it) : 0.0);
        std::printf("    Cerid GMRES(/col) :%6lld col-iters   %8.2f ms (r=%.1e)  [Eigen/Cerid-col = %.2fx time]\n",
                    static_cast<long long>(col_it), t_col, col_res, t_col > 0 ? t / t_col : 0.0);
        std::printf("    %-12s     :%6lld col-iters   %8.2f ms (r=%.1e)\n", eref, static_cast<long long>(eig_it), t,
                    eig_res);
    }
    else
    {
        Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::IdentityPreconditioner> g;
        g.setMaxIterations(cap);
        g.setTolerance(tol);
        g.compute(ea);
        eref = "Eigen BiCGSTAB";
        const crd::f64 t = best_ms(
            [&]()
            {
                eig_it = 0;
                crd::f64 worst = 0;
                for (crd::i32 j = 0; j < s; ++j)
                {
                    Eigen::VectorXd bj(n);
                    for (crd::i32 k = 0; k < n; ++k)
                    {
                        bj(k) = B[static_cast<crd::usize>(k) * s + j];
                    }
                    Eigen::VectorXd xj = g.solve(bj);
                    eig_it += static_cast<crd::i64>(g.iterations());
                    worst = std::max(worst, (bj - ea * xj).norm() / bj.norm());
                }
                eig_res = worst;
            });
        std::printf("  %-10s n=%-6d nnz=%-8zu s=%d  [block-BiCGSTAB]\n", name, n, a.nnz(), s);
        std::printf(
            "    Cerid blk-BiCGSTAB: %5zu block-iters %8.2f ms (r=%.1e)  [Eigen/blk = %.2fx time, %.2fx A-passes]\n",
            blk_it, t_blk, blk_res, t_blk > 0 ? t / t_blk : 0.0,
            blk_it > 0 ? static_cast<crd::f64>(eig_it) / static_cast<crd::f64>(blk_it) : 0.0);
        std::printf("    Cerid BiCGSTAB/col:%6lld col-iters   %8.2f ms (r=%.1e)  [Eigen/Cerid-col = %.2fx time]\n",
                    static_cast<long long>(col_it), t_col, col_res, t_col > 0 ? t / t_col : 0.0);
        std::printf("    %-12s   :%6lld col-iters   %8.2f ms (r=%.1e)\n", eref, static_cast<long long>(eig_it), t,
                    eig_res);
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf(
        "hesap block-GMRES + block-BiCGSTAB (multi-RHS) vs Eigen per-column (no block algo) -- general nonsym\n");
    std::printf("  block: one spmm/step for all s RHS + shared Krylov; A-passes = block-iters vs sum(col-iters).\n");
    for (crd::i32 s : {4, 16})
    {
        std::printf(" --- s=%d ---\n", s);
        run("convdiff3", make_conv_diff(20000, 4.0, 0.4, 1), s, Kind::Gmres);  // cheap operator
        run("convband", make_conv_diff(20000, 42.0, 0.4, 20), s, Kind::Gmres); // expensive operator
        run("convdiff3", make_conv_diff(20000, 4.0, 0.4, 1), s, Kind::Bicgstab);
        run("convband", make_conv_diff(20000, 42.0, 0.4, 20), s, Kind::Bicgstab);
        run("gemat11", load_ss("gemat11"), s, Kind::Gmres); // real nonsym
        run("gemat11", load_ss("gemat11"), s, Kind::Bicgstab);
    }
    crd::jobs::shutdown();
    return 0;
}
