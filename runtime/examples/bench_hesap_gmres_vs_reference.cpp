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
//
// crd conventions throughout: crd::containers::Array (never std::vector),
// crd::platform::fs::read_file_text + manual parse (never std::ifstream/string),
// a named TlsfAllocator (never malloc), crd::f64/usize/i32/i64 (never raw types).
// Eigen runs in its peer library at the host's best ISA (examples/peers/eigen_peer.hpp).

#include "eigen_peer.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
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

static void run(const char* name)
{
    Mtx mtx = load_ss(name);
    if (!mtx.ok)
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 tol = 1e-8;
    const crd::i32 cap = 2000;
    const crd::i32 m = 30;

    hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.row), static_cast<crd::u32>(t.col), t.value);
    }
    auto a = tb.compress();
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    hp::JacobiPreconditioner<crd::f64> jac(a, &g_alloc);

    hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n));
    b.fill(1.0);
    hi::IterativeOptions<crd::f64> opts;
    opts.rel_tol = tol;
    opts.max_iter = static_cast<crd::usize>(cap);

    crd::usize cer_it = 0;
    bool cer_conv = false;
    const crd::f64 t_cer = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            hi::GmresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(m));
            auto res = hi::fgmres<crd::f64>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc);
            cer_it = res.iterations;
            cer_conv = res.converged;
        });

    // APPLES-TO-APPLES: Cerid BiCGSTAB vs Eigen BiCGSTAB (same algorithm + Jacobi).
    crd::usize cb_it = 0;
    bool cb_conv = false;
    const crd::f64 t_cb = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            hi::BicgstabWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
            auto res = hi::bicgstab<crd::f64>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc);
            cb_it = res.iterations;
            cb_conv = res.converged;
        });

    const peer::Owned ea{peer::csc_f64(n, n, mtx.trips.data(), static_cast<peer::Index>(mtx.trips.size()))};
    const peer::Owned eb{peer::vec(n, b.data())};
    const peer::Owned ex{peer::vec(n, static_cast<const crd::f64*>(nullptr))};
    peer::IterativeSettings settings;
    settings.max_iterations = cap;
    settings.tolerance = tol;
    settings.restart = m;
    // Each run builds its solver, as an Eigen user solving once does.
    const auto eigen_run = [&](peer::IterativeSolve method, crd::i64& iterations, bool& converged)
    {
        return best_ms(
            [&]()
            {
                const peer::Owned s{peer::iterative_solver(method, settings)};
                (void)peer::compute(s.get(), ea.get());
                const peer::IterativeResult r = peer::solve(s.get(), eb.get(), ex.get());
                iterations = r.iterations;
                converged = r.converged;
            });
    };

    crd::i64 eb_it = 0;
    bool eb_conv = false;
    const crd::f64 t_bicg = eigen_run(peer::IterativeSolve::BicgstabDiagonal, eb_it, eb_conv);

    crd::i64 eg_it = 0;
    bool eg_conv = false;
    const crd::f64 t_egm = eigen_run(peer::IterativeSolve::GmresDiagonal, eg_it, eg_conv);

    // Headline (apples-to-apples): Cerid BiCGSTAB vs Eigen BiCGSTAB.
    std::printf("  %-10s n=%-6d nnz=%-8zu [%s]\n", name, n, a.nnz(), op.is_parallel() ? "parallel" : "serial");
    std::printf("    BiCGSTAB (same algo): Cerid %4zu it %7.2f ms (%s)  vs  Eigen %4lld it %7.2f ms (%s)  "
                "Eigen/Cerid=%.2fx %s\n",
                cb_it, t_cb, cb_conv ? "conv" : "cap", static_cast<long long>(eb_it), t_bicg, eb_conv ? "conv" : "cap",
                t_bicg / t_cb, (t_bicg / t_cb >= 1.0 ? "WIN" : "loss"));
    std::printf("    GMRES    (same algo): Cerid FGMRES %4zu it %7.2f ms (%s)  vs  Eigen GMRES %4lld it %7.2f ms (%s)  "
                "Eigen/Cerid=%.2fx\n",
                cer_it, t_cer, cer_conv ? "conv" : "cap", static_cast<long long>(eg_it), t_egm,
                eg_conv ? "conv" : "fail", t_egm / t_cer);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crd::jobs::init();
    peer::set_threads(1);
    std::printf("hesap FGMRES(m=30,Jacobi) vs Eigen BiCGSTAB (main) + Eigen GMRES (unsupported) -- real nonsym\n");
    std::printf("  Eigen: %s\n", peer::isa());
    std::printf("  (apples-to-apples is GMRES-vs-GMRES; sherman3-class restarted-GMRES stagnation is a GMRES(m)\n");
    std::printf("   limitation that v4c BiCGSTAB addresses with the same operator + determinism moat.)\n");
    const char* names[] = {"gemat11", "sherman3"};
    for (const char* n : names)
    {
        run(n);
    }
    crd::jobs::shutdown();
    return 0;
}
