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
//
// crd conventions throughout: crd::containers::Array (never std::vector),
// crd::platform::fs::read_file_text + manual parse (never std::ifstream/string),
// a named TlsfAllocator (never malloc), crd::f64/usize/i32/i64 (never raw types).
// Eigen runs in its peer library at the host's best ISA (examples/peers/eigen_peer.hpp).

#include "eigen_peer.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/krylov_preconditioner.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

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
namespace peer = crd_peer::eigen;
using Clock = std::chrono::steady_clock;

static crd::memory::TlsfAllocator g_alloc{crd::usize{1} << 30}; // 1 GiB pool — named allocator, never malloc

// The peer's coordinate, so the Eigen side reuses the parsed list.
using Trip = peer::Entry;
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

template <typename Fn> static crd::f64 best_ms(const Fn& fn, crd::i32 reps = 3)
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

static void run(const char* name, const Mtx& mtx, crd::i32 m_outer, crd::i32 m_inner, crd::i32 inner_iters)
{
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 tol = 1e-9;
    const crd::i32 cap = 6000;

    hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.row), static_cast<crd::u32>(t.col), t.value);
    }
    auto a = tb.compress();
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);

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

    crd::usize flat_it = 0;
    crd::f64 flat_res = 0;
    const crd::f64 t_flat = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(m_outer));
            auto res = hi::gmres<crd::f64>(op, b.span(), x.span(), opts, ws, &g_alloc);
            flat_it = res.iterations;
            flat_res = cer_true(x);
        });

    crd::usize nest_it = 0;
    crd::f64 nest_res = 0;
    const crd::f64 t_nest = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            hi::GmresWorkspace<crd::f64> inner_ws(&g_alloc, static_cast<crd::usize>(n),
                                                  static_cast<crd::usize>(m_inner));
            auto inner = [&](crd::containers::ConstSpan<crd::f64> r, crd::containers::Span<crd::f64> z)
            {
                hi::IterativeOptions<crd::f64> io;
                io.max_iter = static_cast<crd::usize>(inner_iters);
                io.rel_tol = 1e-2;
                (void)hi::gmres<crd::f64>(op, r, z, io, inner_ws, &g_alloc);
            };
            auto inner_solver = hp::make_krylov_preconditioner<crd::f64>(static_cast<crd::usize>(n), inner);
            hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(m_outer));
            auto res = hi::fgmres<crd::f64>(op, &inner_solver, b.span(), x.span(), opts, ws, &g_alloc);
            nest_it = res.iterations;
            nest_res = cer_true(x);
        });

    const peer::Owned ea{peer::csc_f64(n, n, mtx.trips.data(), static_cast<peer::Index>(mtx.trips.size()))};
    const peer::Owned eb{peer::vec(n, b.data())};
    const peer::Owned ex{peer::vec(n, static_cast<const crd::f64*>(nullptr))};
    peer::IterativeSettings settings;
    settings.max_iterations = cap;
    settings.tolerance = tol;
    settings.restart = m_outer;

    crd::i64 eg_it = 0;
    crd::f64 eg_res = 0;
    const crd::f64 t_eg = best_ms(
        [&]()
        {
            const peer::Owned s{peer::iterative_solver(peer::IterativeSolve::GmresIdentity, settings)};
            (void)peer::compute(s.get(), ea.get());
            eg_it = peer::solve(s.get(), eb.get(), ex.get()).iterations;
            eg_res = peer::relative_residual(ea.get(), eb.get(), ex.get());
        });

    std::printf("  %-10s n=%-6d nnz=%-8zu  outer=%d inner=GMRES(%d,%d it)\n", name, n, a.nnz(), m_outer, m_inner,
                inner_iters);
    std::printf("    flat GMRES(m)     : %5zu outer-it %8.2f ms (r=%.1e %s)\n", flat_it, t_flat, flat_res,
                flat_res <= 1e-6 ? "conv" : "STALL");
    std::printf("    nested FGMRES     : %5zu outer-it %8.2f ms (r=%.1e %s)  -> %.2fx fewer outer-iters\n", nest_it,
                t_nest, nest_res, nest_res <= 1e-6 ? "conv" : "STALL",
                nest_it > 0 ? static_cast<crd::f64>(flat_it) / static_cast<crd::f64>(nest_it) : 0.0);
    std::printf("    Eigen GMRES(m)    : %5lld outer-it %8.2f ms (r=%.1e %s)  [no nested composition]\n",
                static_cast<long long>(eg_it), t_eg, eg_res, eg_res <= 1e-6 ? "conv" : "STALL");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    peer::set_threads(1);
    std::printf(
        "hesap nested inner-Krylov-as-preconditioner (FGMRES-outer + inner-GMRES) vs flat GMRES + Eigen GMRES\n");
    std::printf("  Eigen: %s\n", peer::isa());
    std::printf("  value = OUTER iterations cut by the inner approximate-solve; converges where flat restarted GMRES "
                "stalls.\n");
    run("cluster", make_cluster(2000, 8), /*m_outer=*/10, /*m_inner=*/20, /*inner_iters=*/12);
    const char* names[] = {"gemat11", "sherman3", "bcsstk13"};
    for (const char* nm : names)
    {
        run(nm, load_ss(nm), /*m_outer=*/30, /*m_inner=*/30, /*inner_iters=*/15);
    }
    crd::jobs::shutdown();
    return 0;
}
