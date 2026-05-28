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
#include <crd/hesap/iterative/idrs.hpp>
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
    const crd::i32 cap = 4000;
    const crd::i32 sdim = 4;
    // Note: bcsstk13+Jacobi converges (r≈1.6e-6 at ~4823 it) with cap=8000 — it is
    // budget-bound here, marching down steadily; Eigen IDRS diverges on it regardless.

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

    Eigen::SparseMatrix<double> ea(n, n);
    {
        crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
        et.reserve(mtx.trips.size());
        for (const Trip& t : mtx.trips)
        {
            et.push_back(Eigen::Triplet<double>(t.r, t.c, t.v));
        }
        ea.setFromTriplets(et.data(), et.data() + et.size());
    }
    Eigen::VectorXd eb = Eigen::VectorXd::Ones(n);

    // True residual ‖b−Ax‖/‖b‖ for a Cerid solution (recompute through A).
    auto cer_true = [&](const hd::Vector<crd::f64>& x) -> crd::f64
    {
        hd::Vector<crd::f64> ax(&g_alloc, static_cast<crd::usize>(n));
        (void)op.apply(x.span(), ax.span());
        hd::Vector<crd::f64> r(&g_alloc, static_cast<crd::usize>(n));
        for (crd::i32 i = 0; i < n; ++i)
        {
            r(i) = b(i) - ax(i);
        }
        return hd::nrm2<crd::f64>(r.span()) / hd::nrm2<crd::f64>(b.span());
    };
    auto eig_true = [&](const Eigen::VectorXd& x) -> crd::f64
    {
        return (eb - ea * x).norm() / eb.norm();
    };

    auto bench_pair = [&](const char* tag, bool precond)
    {
        crd::usize cer_it = 0;
        crd::f64 cer_res = 0;
        const crd::f64 t_cer = best_ms(
            [&]()
            {
                hd::Vector<crd::f64> x(&g_alloc, static_cast<crd::usize>(n));
                hi::IdrsWorkspace<crd::f64> ws(&g_alloc, static_cast<crd::usize>(n), static_cast<crd::usize>(sdim));
                auto res = precond ? hi::idrs<crd::f64>(op, &jac, b.span(), x.span(), opts, ws, &g_alloc)
                                   : hi::idrs<crd::f64>(op, b.span(), x.span(), opts, ws, &g_alloc);
                cer_it = res.iterations;
                cer_res = cer_true(x);
            });

        crd::i64 eig_it = 0;
        crd::f64 eig_res = 0;
        bool eig_ok = false;
        const crd::f64 t_eig = best_ms(
            [&]()
            {
                Eigen::VectorXd x;
                if (precond)
                {
                    Eigen::IDRS<Eigen::SparseMatrix<double>, Eigen::DiagonalPreconditioner<double>> s;
                    s.setMaxIterations(cap);
                    s.setTolerance(tol);
                    s.setS(sdim);
                    s.compute(ea);
                    x = s.solve(eb);
                    eig_it = static_cast<crd::i64>(s.iterations());
                    eig_ok = (s.info() == Eigen::Success);
                }
                else
                {
                    Eigen::IDRS<Eigen::SparseMatrix<double>, Eigen::IdentityPreconditioner> s;
                    s.setMaxIterations(cap);
                    s.setTolerance(tol);
                    s.setS(sdim);
                    s.compute(ea);
                    x = s.solve(eb);
                    eig_it = static_cast<crd::i64>(s.iterations());
                    eig_ok = (s.info() == Eigen::Success);
                }
                eig_res = eig_true(x);
            });

        const bool cer_conv = cer_res <= 1e-6;
        const bool eig_conv = eig_ok && eig_res <= 1e-6;
        std::printf("    IDR(4) %-8s : Cerid %4zu it %7.2f ms (r=%.1e %s)  vs  Eigen %4lld it %7.2f ms (r=%.1e %s)  "
                    "Eigen/Cerid=%.2fx %s\n",
                    tag, cer_it, t_cer, cer_res, cer_conv ? "conv" : "STALL", static_cast<long long>(eig_it), t_eig,
                    eig_res, eig_conv ? "conv" : "STALL", t_eig / t_cer, (t_eig / t_cer >= 1.0 ? "WIN" : "loss"));
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
    std::printf(
        "  true residual ‖b-Ax‖/‖b‖ recomputed for BOTH sides (STALL = reported converged but residual > 1e-6).\n");
    const char* names[] = {"bcsstk13", "bcsstk24", "bcsstk25", "gemat11", "sherman3"};
    for (const char* n : names)
    {
        run(n);
    }
    crd::jobs::shutdown();
    return 0;
}
