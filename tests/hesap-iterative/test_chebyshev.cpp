#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/preconditioners/chebyshev.hpp>
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
using crd::hesap::preconditioners::ChebyshevPreconditioner;
namespace dense = crd::hesap::dense;

namespace
{
template <typename T> SparseMatrix<T, SparseFormat::Csr> laplace2d(crd::memory::IAllocator* a, crd::u32 g)
{
    const crd::u32 n = g * g;
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 y = 0; y < g; ++y)
        for (crd::u32 x = 0; x < g; ++x)
        {
            const crd::u32 i = y * g + x;
            b.add(i, i, T(4));
            if (x + 1 < g)
            {
                b.add(i, i + 1, T(-1));
            }
            if (x > 0)
            {
                b.add(i, i - 1, T(-1));
            }
            if (y + 1 < g)
            {
                b.add(i, i + g, T(-1));
            }
            if (y > 0)
            {
                b.add(i, i - g, T(-1));
            }
        }
    return b.compress();
}
} // namespace

TEST_CASE("Chebyshev-PCG converges on an SPD system and beats unpreconditioned CG", "[hesap-iterative][chebyshev]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32 g = 28;
        const crd::u32 n = g * g;
        auto a = laplace2d<crd::f64>(&alloc, g);
        SparseLinearOp<crd::f64> op(a);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 5000;

        KrylovWorkspace<crd::f64> ws0(&alloc, n);
        dense::Vector<crd::f64> x0(&alloc, n);
        auto r0 = cg<crd::f64>(op, b.span(), x0.span(), opts, ws0, &alloc);
        REQUIRE(r0.converged);

        ChebyshevPreconditioner<crd::f64> m(a, &alloc, 4);
        KrylovWorkspace<crd::f64> ws(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations < r0.iterations); // matrix-free polynomial M⁻¹ accelerates CG
    }
    crd::jobs::shutdown();
}

TEST_CASE("Chebyshev higher degree => stronger preconditioner (fewer outer iters)", "[hesap-iterative][chebyshev]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32 g = 30;
        const crd::u32 n = g * g;
        auto a = laplace2d<crd::f64>(&alloc, g);
        SparseLinearOp<crd::f64> op(a);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 5);
        }
        auto iters = [&](crd::u32 deg)
        {
            ChebyshevPreconditioner<crd::f64> m(a, &alloc, deg);
            KrylovWorkspace<crd::f64> ws(&alloc, n);
            dense::Vector<crd::f64> x(&alloc, n);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-9;
            opts.max_iter = 5000;
            auto r = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(r.converged);
            return r.iterations;
        };
        const auto i2 = iters(2);
        const auto i6 = iters(6);
        REQUIRE(i6 <= i2); // a higher-degree polynomial is a stronger preconditioner
    }
    crd::jobs::shutdown();
}

TEST_CASE("Chebyshev-PCG solves a complex Hermitian-PD system", "[hesap-iterative][chebyshev][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{48U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 160;
        TripletBuilder<C> tb(&alloc, n, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            tb.add(i, i, C{4.0, 0.0});
            if (i + 1 < n)
            {
                tb.add(i, i + 1, C{-1.0, 0.3});
                tb.add(i + 1, i, C{-1.0, -0.3});
            }
        }
        auto a = tb.compress();
        SparseLinearOp<C> op(a);
        ChebyshevPreconditioner<C> m(a, &alloc, 4);
        dense::Vector<C> b(&alloc, n);
        dense::Vector<C> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = C{1.0, 0.1};
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 400;
        KrylovWorkspace<C> ws(&alloc, n);
        auto res = pcg<C>(op, m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Chebyshev-PCG is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][chebyshev][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32 g = 28;
        const crd::u32 n = g * g;
        auto a = laplace2d<crd::f64>(&alloc, g);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11);
        }
        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            ChebyshevPreconditioner<crd::f64> m(a, &alloc, 4);
            KrylovWorkspace<crd::f64> ws(&alloc, n);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-9;
            opts.max_iter = 5000;
            return pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
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
