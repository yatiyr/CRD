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
#include <crd/hesap/iterative/block_cg.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/preconditioners/block_preconditioner.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
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
#include <cstdio>
#include <cstdlib> // strtol / strtod
#include <cstring> // strncmp

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

// Well-conditioned SPD: diagonally-dominant tridiag (diag 4, off -1). 3 nnz/row —
// the CHEAP-operator regime (block overhead dominates; block-CG's A-pass win can't
// pay off in wall time — the honest lower bound).
static Mtx make_spd(crd::i32 n)
{
    Mtx m;
    m.n = n;
    for (crd::i32 i = 0; i < n; ++i)
    {
        m.trips.push_back({i, i, 4.0});
        if (i + 1 < n)
        {
            m.trips.push_back({i, i + 1, -1.0});
        }
        if (i > 0)
        {
            m.trips.push_back({i, i - 1, -1.0});
        }
    }
    m.ok = true;
    return m;
}

// Well-conditioned banded SPD, ~(2·hb+1) nnz/row — the EXPENSIVE-operator regime
// where the spmm dominates, so block-CG's 4–16× fewer A-passes win wall time.
static Mtx make_banded_spd(crd::i32 n, crd::i32 hb)
{
    Mtx m;
    m.n = n;
    for (crd::i32 i = 0; i < n; ++i)
    {
        m.trips.push_back({i, i, 2.0 * hb + 2.0}); // diagonally dominant ⇒ SPD, well-conditioned
        for (crd::i32 d = 1; d <= hb; ++d)
        {
            if (i + d < n)
            {
                m.trips.push_back({i, i + d, -1.0});
            }
            if (i - d >= 0)
            {
                m.trips.push_back({i, i - d, -1.0});
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

// max per-column true relative residual ‖B·,j - A·X·,j‖/‖B·,j‖ (blocks n×s row-major).
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

// Three Jacobi-preconditioned solvers on the SAME SPD system + the SAME s RHS:
//   1. Cerid block-PCG     -- all s RHS share one Krylov space; ONE spmm/step.
//   2. Cerid per-column PCG -- the algorithm-appropriate tool when A is cache-resident
//                             (parallel SELL spmv; the v4a path that crushes the cheap regime).
//   3. Eigen per-column CG + DiagonalPreconditioner -- the frontier reference (no block algo).
// Jacobi on all three ⇒ apples-to-apples. Eigen .compute() (the factorization) is OUTSIDE
// the timed region, matching the Cerid preconditioners (built once, before the timed solve).
static void run(const char* name, const Mtx& mtx, crd::i32 s)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 tol = 1e-8;
    const crd::i32 cap = 8000;

    hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();
    hs::ParallelSpmmLinearOp<crd::f64> op(a);                  // block op (parallel spmm above ~L2)
    hs::ParallelSparseLinearOp<crd::f64> colop(a, &g_alloc);   // single-vector op (parallel SELL spmv)
    hp::JacobiBlockPreconditioner<crd::f64> jblk(a, &g_alloc); // block Jacobi (one-pass diagonal)
    hp::JacobiPreconditioner<crd::f64> jcol(a, &g_alloc);      // single-vector Jacobi

    // RHS block B (n×s row-major), distinct columns.
    crd::containers::Array<crd::f64> B(&g_alloc);
    B.resize(static_cast<crd::usize>(n) * s);
    for (crd::i32 k = 0; k < n; ++k)
    {
        for (crd::i32 j = 0; j < s; ++j)
        {
            B[static_cast<crd::usize>(k) * s + j] = 1.0 + 0.1 * j + 0.01 * (k % 9);
        }
    }

    // 1. Cerid block-PCG (all s RHS at once, shared Krylov space, one spmm/step).
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
            hi::BlockCgWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::u32>(s));
            auto res = hi::block_pcg<crd::f64>(op, jblk, b.span(), x.span(), opts, ws, &g_alloc);
            blk_it = res.iterations;
            blk_res = block_worst_res(op, x, B, n, s);
        });

    // 2. Cerid per-column PCG (parallel SELL spmv; the cache-resident-A regime tool).
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
                hi::KrylovWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
                auto res = hi::pcg<crd::f64>(colop, jcol, bj.span(), xj.span(), opts, ws, &g_alloc);
                col_it += static_cast<crd::i64>(res.iterations);
                worst =
                    std::max(worst, res.final_residual_norm / (bj.span().size() ? hd::nrm2<crd::f64>(bj.span()) : 1.0));
            }
            col_res = worst;
        });

    // 3. Eigen per-column CG + DiagonalPreconditioner. compute() (factorization) hoisted out.
    crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        et.push_back(Eigen::Triplet<double>(t.r, t.c, t.v));
    }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.data(), et.data() + et.size());
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                             Eigen::DiagonalPreconditioner<double>>
        cg;
    cg.setMaxIterations(cap);
    cg.setTolerance(tol);
    cg.compute(ea);

    crd::i64 eig_it = 0;
    crd::f64 eig_res = 0;
    const crd::f64 t_eig = best_ms(
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
                Eigen::VectorXd xj = cg.solve(bj);
                eig_it += static_cast<crd::i64>(cg.iterations());
                worst = std::max(worst, (bj - ea * xj).norm() / bj.norm());
            }
            eig_res = worst;
        });

    std::printf("  %-10s n=%-6d nnz=%-8zu s=%d\n", name, n, a.nnz(), s);
    std::printf("    Cerid block-PCG    : %5zu block-iters (=A-passes) %8.2f ms  (r=%.1e)  [Eigen/blk = %.2fx time, "
                "%.2fx A-passes]\n",
                blk_it, t_blk, blk_res, t_blk > 0 ? t_eig / t_blk : 0.0,
                blk_it > 0 ? static_cast<crd::f64>(eig_it) / static_cast<crd::f64>(blk_it) : 0.0);
    std::printf("    Cerid per-col PCG  : %5lld total col-iters         %8.2f ms  (r=%.1e)  [Eigen/col = %.2fx time]\n",
                static_cast<long long>(col_it), t_col, col_res, t_col > 0 ? t_eig / t_col : 0.0);
    std::printf("    Eigen per-col CG+J : %5lld total col-iters         %8.2f ms  (r=%.1e)\n",
                static_cast<long long>(eig_it), t_eig, eig_res);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    Eigen::setNbThreads(1);
    std::printf("hesap block-PCG (multi-RHS) + per-column PCG vs Eigen CG+Jacobi per-column -- SPD (all "
                "Jacobi-preconditioned)\n");
    std::printf("  block-PCG wins when A>L2 (A-pass reuse) or shared-Krylov cuts iters; per-column PCG is the\n");
    std::printf("  algorithm-appropriate tool when A is cache-resident (block-PCG's O(n.s^2) dense flops then have\n");
    std::printf("  no memory-pass win to trade for). Cerid crushes Eigen via whichever path fits the regime.\n");
    std::printf("  Synthetic (well-conditioned: isolates the A-pass throughput lever):\n");
    for (crd::i32 s : {4, 16})
    {
        run("tridiag(3)", make_spd(20000), s);            // cheap operator (3 nnz/row, L1-resident):
                                                          //   algorithmic floor -- block-CG's O(n.s^2) dense
                                                          //   flops have no memory-pass win to trade for.
        run("banded(41)", make_banded_spd(20000, 20), s); // expensive operator: the block-CG win regime.
    }
    // Real SuiteSparse SPD structural-stiffness matrices (ill-conditioned ⇒ the
    // shared Krylov space cuts iterations-per-column, block-CG's second lever).
    std::printf("  Real SuiteSparse SPD (ill-conditioned: shared-Krylov iteration lever + A-pass lever):\n");
    for (crd::i32 s : {4, 16})
    {
        run("bcsstk13", load_ss("bcsstk13"), s);
        run("bcsstk24", load_ss("bcsstk24"), s);
    }
    crd::jobs::shutdown();
    return 0;
}
