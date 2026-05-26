#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/ilu0.hpp>
#include <crd/hesap/preconditioners/ilut.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triangular_solve.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::Ilu0Preconditioner;
using crd::hesap::preconditioners::IlutPreconditioner;
namespace dense = crd::hesap::dense;

namespace
{
template <typename T>
SparseMatrix<T, SparseFormat::Csr> nonsym_tridiag(crd::memory::IAllocator* a, crd::u32 n, double g)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(4));
        if (i + 1 < n) { b.add(i, i + 1, T(static_cast<crd::f64>(-1.0 + g))); }
        if (i > 0) { b.add(i, i - 1, T(static_cast<crd::f64>(-1.0 - g))); }
    }
    return b.compress();
}

// 2D convection-diffusion 5-point (n=g²): IC(0)/ILU(0) drop fill that ILUT can keep.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> conv_diff2d(crd::memory::IAllocator* a, crd::u32 g, double beta)
{
    const crd::u32    n = g * g;
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 y = 0; y < g; ++y)
    {
        for (crd::u32 x = 0; x < g; ++x)
        {
            const crd::u32 i = y * g + x;
            b.add(i, i, T(4));
            if (x + 1 < g) { b.add(i, i + 1, T(static_cast<crd::f64>(-1.0 + beta))); }
            if (x > 0) { b.add(i, i - 1, T(static_cast<crd::f64>(-1.0 - beta))); }
            if (y + 1 < g) { b.add(i, i + g, T(static_cast<crd::f64>(-1.0 + beta))); }
            if (y > 0) { b.add(i, i - g, T(static_cast<crd::f64>(-1.0 - beta))); }
        }
    }
    return b.compress();
}

template <typename T>
SparseMatrix<T, SparseFormat::Csr> nonsym_tridiag_c(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T{4.0, 0.5});
        if (i + 1 < n) { b.add(i, i + 1, T{-1.0, 0.2}); }
        if (i > 0) { b.add(i, i - 1, T{-1.3, -0.1}); }
    }
    return b.compress();
}
} // namespace

TEST_CASE("Level-scheduled triangular solve is bit-exact vs sequential (with real parallelism)",
          "[hesap-iterative][ilut][tri-solve]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        // 2D-grid lower-triangular off-diagonal factor (row i = y*g+x depends on i-1 and i-g):
        // diagonal wavefront levels of width ~g ⇒ parallelism triggers (n ≥ 8192, width ≥ 64).
        const crd::u32 g = 300;
        const crd::u32 n = g * g; // 90000 — wavefront width ~g=300 ≥ 256 ⇒ parallel engages
        crd::containers::Array<crd::u32> ptr(&alloc), col(&alloc);
        crd::containers::Array<crd::f64> val(&alloc), inv_diag(&alloc), b(&alloc), ys(&alloc), yp(&alloc);
        ptr.push_back(0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::u32 x = i % g, y = i / g;
            if (y > 0) { col.push_back(i - g); val.push_back(-0.3); }
            if (x > 0) { col.push_back(i - 1); val.push_back(-0.25); }
            ptr.push_back(static_cast<crd::u32>(col.size()));
        }
        inv_diag.resize(n); b.resize(n); ys.resize(n); yp.resize(n);
        for (crd::u32 i = 0; i < n; ++i) { inv_diag[i] = 1.0 / (2.0 + 0.01 * (i % 7)); b[i] = 1.0 + 0.001 * (i % 13); }

        auto sched = crd::hesap::sparse::build_lower_tri_schedule(ptr.data(), col.data(), n, &alloc);
        REQUIRE(sched.max_width >= 256U); // parallel path actually engaged
        REQUIRE(sched.worth_parallel());

        // Sequential reference.
        for (crd::u32 i = 0; i < n; ++i)
        {
            crd::f64 acc = b[i];
            for (crd::u32 p = ptr[i]; p < ptr[i + 1]; ++p) { acc -= val[p] * ys[col[p]]; }
            ys[i] = acc * inv_diag[i];
        }
        // Level-scheduled (parallel) solve.
        crd::hesap::sparse::tri_solve_lower_levelsched<crd::f64>(ptr.data(), col.data(), val.data(), inv_diag.data(),
                                                                sched, b.data(), yp.data());
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(ys[i] == yp[i]); } // bit-exact
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILUT with no dropping on a tridiagonal is exact LU (FGMRES converges in 1 step)",
          "[hesap-iterative][ilut]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator   alloc{32U << 20};
        const crd::u32               n = 200;
        auto                         a = nonsym_tridiag<crd::f64>(&alloc, n, 0.4);
        SparseLinearOp<crd::f64>     op(a);
        IlutPreconditioner<crd::f64> ilut(a, &alloc, /*lfil=*/16, /*droptol=*/0.0); // no dropping ⇒ exact on tridiag
        dense::Vector<crd::f64>      b(&alloc, n), x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 7); x(i) = 0.0; }
        IterativeOptions<crd::f64>   opts; opts.rel_tol = 1e-10; opts.max_iter = 50;
        GmresWorkspace<crd::f64>     ws(&alloc, n, 30);
        auto                         res = fgmres<crd::f64>(op, &ilut, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations <= 2);
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILUT with droptol=0 and unbounded lfil is the FULL LU (FGMRES converges in 1 step)",
          "[hesap-iterative][ilut]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator   alloc{64U << 20};
        const crd::u32               g = 6;
        const crd::u32               n = g * g; // 36 — full LU has fill; lfil=n keeps all of it
        auto                         a = conv_diff2d<crd::f64>(&alloc, g, 0.3);
        SparseLinearOp<crd::f64>     op(a);
        IlutPreconditioner<crd::f64> ilut(a, &alloc, /*lfil=*/n, /*droptol=*/0.0); // = exact LU
        dense::Vector<crd::f64>      b(&alloc, n), x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 5); x(i) = 0.0; }
        IterativeOptions<crd::f64>   opts; opts.rel_tol = 1e-10; opts.max_iter = 50;
        GmresWorkspace<crd::f64>     ws(&alloc, n, n);
        auto                         res = fgmres<crd::f64>(op, &ilut, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations <= 2); // exact LU ⇒ M⁻¹A = I
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILUT is a stronger preconditioner than ILU(0) (fewer FGMRES iters on 2D conv-diff)",
          "[hesap-iterative][ilut]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32             g = 32;
        const crd::u32             n = g * g;
        auto                       a = conv_diff2d<crd::f64>(&alloc, g, 0.3);
        SparseLinearOp<crd::f64>   op(a);
        dense::Vector<crd::f64>    b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0; }
        IterativeOptions<crd::f64> opts; opts.rel_tol = 1e-9; opts.max_iter = 5000;

        Ilu0Preconditioner<crd::f64> ilu0(a, &alloc);
        dense::Vector<crd::f64>      x0(&alloc, n);
        GmresWorkspace<crd::f64>     ws0(&alloc, n, 50);
        auto                         r0 = fgmres<crd::f64>(op, &ilu0, b.span(), x0.span(), opts, ws0, &alloc);
        REQUIRE(r0.converged);

        IlutPreconditioner<crd::f64> ilut(a, &alloc, /*lfil=*/12, /*droptol=*/1e-4);
        dense::Vector<crd::f64>      xt(&alloc, n);
        GmresWorkspace<crd::f64>     wst(&alloc, n, 50);
        auto                         rt = fgmres<crd::f64>(op, &ilut, b.span(), xt.span(), opts, wst, &alloc);
        REQUIRE(rt.converged);
        REQUIRE(rt.iterations < r0.iterations); // extra fill ⇒ stronger preconditioner
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(std::abs(x0(i) - xt(i)) < 1e-5); }
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILUT-BiCGSTAB solves a complex nonsymmetric system", "[hesap-iterative][ilut][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        using C        = Complex<crd::f64>;
        const crd::u32 n = 160;
        auto           a = nonsym_tridiag_c<C>(&alloc, n);
        SparseLinearOp<C>    op(a);
        IlutPreconditioner<C> ilut(a, &alloc, 16, 1e-5);
        dense::Vector<C> b(&alloc, n), x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = C{1.0, 0.1}; }
        IterativeOptions<crd::f64>  opts; opts.rel_tol = 1e-10; opts.max_iter = 200;
        BicgstabWorkspace<C>        ws(&alloc, n);
        auto                        res = bicgstab<C>(op, &ilut, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILUT-FGMRES is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][ilut][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32             g = 26;
        const crd::u32             n = g * g;
        auto                       a = conv_diff2d<crd::f64>(&alloc, g, 0.25);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64>          b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11); }

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IlutPreconditioner<crd::f64> ilut(a, &alloc, 12, 1e-4);
            IterativeOptions<crd::f64>   opts; opts.rel_tol = 1e-10; opts.max_iter = 5000;
            GmresWorkspace<crd::f64>     ws(&alloc, n, 40);
            return fgmres<crd::f64>(op, &ilut, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n), xp(&alloc, n);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(xs(i) == xp(i)); }
    }
    crd::jobs::shutdown();
}
