// bench_hesap_cg_vs_reference.cpp -- Phase 3.1.6 v4a-2.
//
// CG / Jacobi-PCG vs Eigen ConjugateGradient + DiagonalPreconditioner on REAL
// SuiteSparse SPD stiffness matrices (bcsstk*). Our operator is the row-balanced
// PARALLEL SELL-C-σ spmv over crd::jobs (the DRAM-bound regime where Cerid beats
// Eigen's single-threaded scalar spmv); the Krylov reductions are KBN-pairwise,
// so the solve is bit-deterministic across thread counts -- which Eigen does not
// offer at all. Same tolerance + iteration cap on both sides; we report
// iterations, wall time, and Eigen/Cerid speedup.
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
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/minres.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
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

// Matrix-Market reader, crd-native: read the whole file into a crd::String via the
// platform filesystem, then hand-parse with strtol/strtod (no std::ifstream / std::string
// line buffers / std::stringstream). DATA lands in crd::containers::Array<Trip>.
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

// Build "<dir>/<name>/<name>.mtx" with a crd::String (no std::string) and read it.
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
        std::printf("  %-12s SKIP (not found)\n", name);
        return;
    }
    const crd::i32 n = mtx.n;
    const crd::f64 tol = 1e-6;
    const crd::i32 cap = 3000;

    // Cerid CSR.
    hs::TripletBuilder<crd::f64> tb(&g_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.row), static_cast<crd::u32>(t.col), t.value);
    }
    auto a = tb.compress();
    hs::ParallelSparseLinearOp<crd::f64> op(a, &g_alloc);
    hp::JacobiPreconditioner<crd::f64> m(a, &g_alloc);

    hd::Vector<crd::f64> b(&g_alloc, static_cast<crd::usize>(n));
    b.fill(1.0);

    hi::IterativeOptions<crd::f64> opts;
    opts.rel_tol = tol;
    opts.max_iter = static_cast<crd::usize>(cap);

    crd::usize cerid_iters = 0;
    bool cerid_conv = false;
    const crd::f64 t_cerid = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            hi::KrylovWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
            auto res = hi::pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &g_alloc);
            cerid_iters = res.iterations;
            cerid_conv = res.converged;
        });

    // Eigen CG + DiagonalPreconditioner (Jacobi), single-threaded; each run builds its solver.
    const peer::Owned ea{peer::csc_f64(n, n, mtx.trips.data(), static_cast<peer::Index>(mtx.trips.size()))};
    const peer::Owned eb{peer::vec(n, b.data())};
    const peer::Owned ex{peer::vec(n, static_cast<const crd::f64*>(nullptr))};
    peer::IterativeSettings settings;
    settings.max_iterations = cap;
    settings.tolerance = tol;
    crd::i64 eig_iters = 0;
    bool eig_conv = false;
    const crd::f64 t_eig = best_ms(
        [&]()
        {
            const peer::Owned cg{peer::iterative_solver(peer::IterativeSolve::CgDiagonal, settings)};
            (void)peer::compute(cg.get(), ea.get()); // bind + build the preconditioner before solve
            const peer::IterativeResult r = peer::solve(cg.get(), eb.get(), ex.get());
            eig_iters = r.iterations;
            eig_conv = r.converged;
        });

    const crd::f64 ratio = t_eig / t_cerid;
    std::printf("  %-12s n=%-7d nnz=%-9zu [%s] | Cerid PCG %5zu it %8.2f ms (%s) | Eigen CG %5lld it %8.2f ms (%s) | "
                "Eigen/Cerid=%.2fx %s\n",
                name, n, a.nnz(), op.is_parallel() ? "parallel" : "serial", cerid_iters, t_cerid,
                cerid_conv ? "conv" : "cap", static_cast<long long>(eig_iters), t_eig, eig_conv ? "conv" : "cap", ratio,
                (ratio >= 1.0 ? "WIN" : "loss"));

    // APPLES-TO-APPLES: Cerid Jacobi-MINRES vs Eigen Jacobi-MINRES (same algorithm
    // + same SPD preconditioner). v4c-2a-precond: preconditioned MINRES now solves
    // ill-conditioned SPD (bcsstk13) where unpreconditioned MINRES capped.
    crd::usize cm_it = 0;
    bool cm_conv = false;
    const crd::f64 t_cm = best_ms(
        [&]()
        {
            hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
            hi::MinresWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n));
            auto res = hi::minres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &g_alloc);
            cm_it = res.iterations;
            cm_conv = res.converged;
        });
    crd::i64 em_it = 0;
    bool em_conv = false;
    const crd::f64 t_em = best_ms(
        [&]()
        {
            const peer::Owned s{peer::iterative_solver(peer::IterativeSolve::MinresDiagonal, settings)};
            (void)peer::compute(s.get(), ea.get());
            const peer::IterativeResult r = peer::solve(s.get(), eb.get(), ex.get());
            em_it = r.iterations;
            em_conv = r.converged;
        });
    std::printf(
        "    MINRES (same algo): Cerid %5zu it %8.2f ms (%s)  vs  Eigen %5lld it %8.2f ms (%s)  Eigen/Cerid=%.2fx %s\n",
        cm_it, t_cm, cm_conv ? "conv" : "cap", static_cast<long long>(em_it), t_em, em_conv ? "conv" : "cap",
        t_em / t_cm, (t_em / t_cm >= 1.0 ? "WIN" : "loss"));
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: see output up to any crash
    crd::jobs::init();
    peer::set_threads(1);
    std::printf("hesap CG vs Eigen ConjugateGradient+DiagonalPreconditioner (real SuiteSparse SPD)\n");
    std::printf("  Eigen: %s\n", peer::isa());
    const char* names[] = {"bcsstk13", "bcsstk24", "bcsstk25"};
    for (const char* n : names)
    {
        run(n);
    }
    crd::jobs::shutdown();
    return 0;
}
