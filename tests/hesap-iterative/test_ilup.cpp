#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/ilup.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::IlupPreconditioner;
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
        if (i + 1 < n)
        {
            b.add(i, i + 1, T(static_cast<crd::f64>(-1.0 + g)));
        }
        if (i > 0)
        {
            b.add(i, i - 1, T(static_cast<crd::f64>(-1.0 - g)));
        }
    }
    return b.compress();
}

// 2D convection-diffusion 5-point (n=g²): level-1 fill (the diagonal-block coupling the
// level-0 pattern misses) measurably strengthens the preconditioner.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> conv_diff2d(crd::memory::IAllocator* a, crd::u32 g, double beta)
{
    const crd::u32 n = g * g;
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 y = 0; y < g; ++y)
    {
        for (crd::u32 x = 0; x < g; ++x)
        {
            const crd::u32 i = y * g + x;
            b.add(i, i, T(4));
            if (x + 1 < g)
            {
                b.add(i, i + 1, T(static_cast<crd::f64>(-1.0 + beta)));
            }
            if (x > 0)
            {
                b.add(i, i - 1, T(static_cast<crd::f64>(-1.0 - beta)));
            }
            if (y + 1 < g)
            {
                b.add(i, i + g, T(static_cast<crd::f64>(-1.0 + beta)));
            }
            if (y > 0)
            {
                b.add(i, i - g, T(static_cast<crd::f64>(-1.0 - beta)));
            }
        }
    }
    return b.compress();
}

template <typename T> SparseMatrix<T, SparseFormat::Csr> nonsym_tridiag_c(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T{4.0, 0.5});
        if (i + 1 < n)
        {
            b.add(i, i + 1, T{-1.0, 0.2});
        }
        if (i > 0)
        {
            b.add(i, i - 1, T{-1.3, -0.1});
        }
    }
    return b.compress();
}
} // namespace

TEST_CASE("ILU(p) on a tridiagonal is exact LU at any p (FGMRES converges in 1 step)", "[hesap-iterative][ilup]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        const crd::u32 n = 200;
        auto a = nonsym_tridiag<crd::f64>(&alloc, n, 0.4);
        SparseLinearOp<crd::f64> op(a);
        for (crd::u32 p : {0U, 1U, 3U}) // tridiag has no fill ⇒ exact at every level
        {
            IlupPreconditioner<crd::f64> ilu(a, &alloc, p);
            dense::Vector<crd::f64> b(&alloc, n);
            dense::Vector<crd::f64> x(&alloc, n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 7);
                x(i) = 0.0;
            }
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-10;
            opts.max_iter = 50;
            GmresWorkspace<crd::f64> ws(&alloc, n, 30);
            auto res = fgmres<crd::f64>(op, &ilu, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(res.converged);
            REQUIRE(res.iterations <= 2);
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILU(p) fill and convergence improve monotonically with p (and large p = full LU)", "[hesap-iterative][ilup]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32 g = 24;
        const crd::u32 n = g * g;
        auto a = conv_diff2d<crd::f64>(&alloc, g, 0.3);
        SparseLinearOp<crd::f64> op(a);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }

        IlupPreconditioner<crd::f64> p0(a, &alloc, 0);
        IlupPreconditioner<crd::f64> p1(a, &alloc, 1);
        IlupPreconditioner<crd::f64> p2(a, &alloc, 2);
        // Fill grows with level (a wrong max-instead-of-min level would drop entries ⇒ break this).
        REQUIRE(p0.factor_nnz() <= p1.factor_nnz());
        REQUIRE(p1.factor_nnz() <= p2.factor_nnz());
        REQUIRE(p1.factor_nnz() > p0.factor_nnz()); // level-1 fill exists on a 2D stencil

        auto iters = [&](IlupPreconditioner<crd::f64>& m)
        {
            dense::Vector<crd::f64> x(&alloc, n);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-9;
            opts.max_iter = 5000;
            GmresWorkspace<crd::f64> ws(&alloc, n, 60);
            auto r = fgmres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(r.converged);
            return r.iterations;
        };
        const auto i0 = iters(p0);
        const auto i1 = iters(p1);
        const auto i2 = iters(p2);
        REQUIRE(i1 <= i0); // more fill ⇒ stronger preconditioner
        REQUIRE(i2 <= i1);

        // Dense matrix ⇒ ILU(0) IS the full LU (no fill possible) ⇒ exact ⇒ 1 step. Isolates
        // the numeric elimination from deep-level fill.
        const crd::u32 nd = 8;
        TripletBuilder<crd::f64> tb(&alloc, nd, nd);
        for (crd::u32 i = 0; i < nd; ++i)
        {
            for (crd::u32 j = 0; j < nd; ++j)
            {
                tb.add(i, j, (i == j) ? 10.0 + i : (0.5 + 0.1 * static_cast<crd::f64>((i + 3 * j) % 5)));
            }
        }
        auto ad = tb.compress();
        SparseLinearOp<crd::f64> opd(ad);
        IlupPreconditioner<crd::f64> pdense(ad, &alloc, 0); // dense ⇒ ILU(0) = full LU
        dense::Vector<crd::f64> bd(&alloc, nd);
        dense::Vector<crd::f64> xd(&alloc, nd);
        for (crd::u32 i = 0; i < nd; ++i)
        {
            bd(i) = 1.0 + 0.1 * static_cast<crd::f64>(i);
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 50;
        GmresWorkspace<crd::f64> ws(&alloc, nd, nd);
        auto res = fgmres<crd::f64>(opd, &pdense, bd.span(), xd.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations <= 2);
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILU(p)-BiCGSTAB solves a complex nonsymmetric system", "[hesap-iterative][ilup][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 160;
        auto a = nonsym_tridiag_c<C>(&alloc, n);
        SparseLinearOp<C> op(a);
        IlupPreconditioner<C> ilu(a, &alloc, 1);
        dense::Vector<C> b(&alloc, n);
        dense::Vector<C> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = C{1.0, 0.1};
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 200;
        BicgstabWorkspace<C> ws(&alloc, n);
        auto res = bicgstab<C>(op, &ilu, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILU(p)-FGMRES is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][ilup][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32 g = 26;
        const crd::u32 n = g * g;
        auto a = conv_diff2d<crd::f64>(&alloc, g, 0.25);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11);
        }

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            IlupPreconditioner<crd::f64> ilu(a, &alloc, 2);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-10;
            opts.max_iter = 5000;
            GmresWorkspace<crd::f64> ws(&alloc, n, 40);
            return fgmres<crd::f64>(op, &ilu, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto rs = solve(serial_op, xs);
        auto rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(xs(i) == xp(i));
        }
    }
    crd::jobs::shutdown();
}
