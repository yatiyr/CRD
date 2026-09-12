#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/ilu0.hpp>
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
using crd::hesap::preconditioners::Ilu0Preconditioner;
namespace dense = crd::hesap::dense;

namespace
{
// Nonsymmetric tridiagonal -- ILU(0) has NO fill ⇒ ILU(0) == exact LU.
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

// Nonsymmetric 2D convection-diffusion 5-point (n=g²) -- ILU(0) has fill it drops.
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

TEST_CASE("ILU(0) on a tridiagonal nonsym system is exact LU (FGMRES converges in 1 step)", "[hesap-iterative][ilu0]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        const crd::u32 n = 200;
        auto a = nonsym_tridiag<crd::f64>(&alloc, n, 0.4);
        SparseLinearOp<crd::f64> op(a);
        Ilu0Preconditioner<crd::f64> ilu(a, &alloc);
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
        REQUIRE(res.iterations <= 2); // tridiag ⇒ no fill ⇒ M == A exactly
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILU(0) accelerates FGMRES + BiCGSTAB on 2D convection-diffusion", "[hesap-iterative][ilu0]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32 g = 28;
        const crd::u32 n = g * g;
        auto a = conv_diff2d<crd::f64>(&alloc, g, 0.3);
        SparseLinearOp<crd::f64> op(a);
        Ilu0Preconditioner<crd::f64> ilu(a, &alloc);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 5000;

        // FGMRES: plain vs ILU(0)-preconditioned.
        dense::Vector<crd::f64> xp(&alloc, n);
        dense::Vector<crd::f64> xi(&alloc, n);
        GmresWorkspace<crd::f64> wsp(&alloc, n, 40);
        GmresWorkspace<crd::f64> wsi(&alloc, n, 40);
        auto plain = fgmres<crd::f64>(op, nullptr, b.span(), xp.span(), opts, wsp, &alloc);
        auto pre = fgmres<crd::f64>(op, &ilu, b.span(), xi.span(), opts, wsi, &alloc);
        REQUIRE(pre.converged);
        if (plain.converged)
        {
            REQUIRE(pre.iterations < plain.iterations);
        }

        // BiCGSTAB + ILU(0).
        dense::Vector<crd::f64> xb(&alloc, n);
        BicgstabWorkspace<crd::f64> wsb(&alloc, n);
        auto rb = bicgstab<crd::f64>(op, &ilu, b.span(), xb.span(), opts, wsb, &alloc);
        REQUIRE(rb.converged);
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(std::abs(xi(i) - xb(i)) < 1e-5);
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILU(0)-FGMRES solves a complex nonsymmetric system", "[hesap-iterative][ilu0][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 160;
        auto a = nonsym_tridiag_c<C>(&alloc, n);
        SparseLinearOp<C> op(a);
        Ilu0Preconditioner<C> ilu(a, &alloc);
        dense::Vector<C> b(&alloc, n);
        dense::Vector<C> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = C{1.0, 0.1};
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 50;
        GmresWorkspace<C> ws(&alloc, n, 30);
        auto res = fgmres<C>(op, &ilu, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations <= 2); // tridiag ⇒ ILU(0) exact
    }
    crd::jobs::shutdown();
}

TEST_CASE("ILU(0)-FGMRES is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][ilu0][determinism]")
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
            Ilu0Preconditioner<crd::f64> ilu(a, &alloc);
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
