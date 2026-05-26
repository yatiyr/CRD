#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/preconditioners/ic0.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::Ic0Preconditioner;
namespace dense = crd::hesap::dense;

namespace
{
// SPD: 2D 5-point Laplacian on a g×g grid (n=g², ill-conditioned ⇒ IC(0) helps a lot).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> laplacian2d(crd::memory::IAllocator* a, crd::u32 g)
{
    const crd::u32    n = g * g;
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 y = 0; y < g; ++y)
    {
        for (crd::u32 x = 0; x < g; ++x)
        {
            const crd::u32 i = y * g + x;
            b.add(i, i, T(4));
            if (x + 1 < g) { b.add(i, i + 1, T(-1)); b.add(i + 1, i, T(-1)); }
            if (y + 1 < g) { b.add(i, i + g, T(-1)); b.add(i + g, i, T(-1)); }
        }
    }
    return b.compress();
}

// SPD tridiagonal (diag 4, off -1) -- IC(0) has NO fill here ⇒ IC(0) == exact Cholesky.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> spd_tridiag(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(4));
        if (i + 1 < n) { b.add(i, i + 1, T(-1)); }
        if (i > 0) { b.add(i, i - 1, T(-1)); }
    }
    return b.compress();
}

template <typename T>
SparseMatrix<T, SparseFormat::Csr> hpd_tridiag(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T{4.0, 0.0});
        if (i + 1 < n) { b.add(i, i + 1, T{-1.0, 0.3}); b.add(i + 1, i, T{-1.0, -0.3}); }
    }
    return b.compress();
}
} // namespace

TEST_CASE("IC(0) on a tridiagonal SPD system is exact Cholesky (PCG converges in 1 step)",
          "[hesap-iterative][ic0]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator     alloc{32U << 20};
        const crd::u32                 n = 200;
        auto                           a = spd_tridiag<crd::f64>(&alloc, n);
        SparseLinearOp<crd::f64>       op(a);
        Ic0Preconditioner<crd::f64>    ic(a, &alloc);
        dense::Vector<crd::f64>        b(&alloc, n), x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 7); x(i) = 0.0; }
        IterativeOptions<crd::f64> opts; opts.rel_tol = 1e-10; opts.max_iter = 50;
        KrylovWorkspace<crd::f64>  ws(&alloc, n);
        auto                       res = pcg<crd::f64>(op, ic, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations <= 2); // tridiag ⇒ no fill ⇒ M == A exactly
    }
    crd::jobs::shutdown();
}

TEST_CASE("IC(0)-PCG converges on the 2D Laplacian and cuts iterations vs plain CG",
          "[hesap-iterative][ic0]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             g = 30; // n = 900
        const crd::u32             n = g * g;
        auto                       a = laplacian2d<crd::f64>(&alloc, g);
        SparseLinearOp<crd::f64>   op(a);
        dense::Vector<crd::f64>    b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0; }

        IterativeOptions<crd::f64> opts; opts.rel_tol = 1e-9; opts.max_iter = 5000;
        dense::Vector<crd::f64>    x0(&alloc, n);
        KrylovWorkspace<crd::f64>  ws0(&alloc, n);
        auto                       plain = cg<crd::f64>(op, b.span(), x0.span(), opts, ws0, &alloc);
        REQUIRE(plain.converged);

        Ic0Preconditioner<crd::f64> ic(a, &alloc);
        dense::Vector<crd::f64>     x1(&alloc, n);
        KrylovWorkspace<crd::f64>   ws1(&alloc, n);
        auto                        pre = pcg<crd::f64>(op, ic, b.span(), x1.span(), opts, ws1, &alloc);
        REQUIRE(pre.converged);
        REQUIRE(pre.iterations < plain.iterations); // IC(0) is a real preconditioner
        // both solve the same system
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(std::abs(x0(i) - x1(i)) < 1e-5); }
    }
    crd::jobs::shutdown();
}

TEST_CASE("IC(0)-PCG solves a complex HPD system", "[hesap-iterative][ic0][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        using C        = Complex<crd::f64>;
        const crd::u32 n = 160;
        auto           a = hpd_tridiag<C>(&alloc, n);
        SparseLinearOp<C> op(a);
        Ic0Preconditioner<C> ic(a, &alloc);
        dense::Vector<C> b(&alloc, n), x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = C{1.0 + 0.01 * static_cast<crd::f64>(i % 5), 0.1}; }
        IterativeOptions<crd::f64> opts; opts.rel_tol = 1e-10; opts.max_iter = 50;
        KrylovWorkspace<C>         ws(&alloc, n);
        auto                       res = pcg<C>(op, ic, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations <= 2); // HPD tridiag ⇒ IC(0) exact
    }
    crd::jobs::shutdown();
}

TEST_CASE("IC(0)-PCG is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][ic0][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32             g = 30;
        const crd::u32             n = g * g;
        auto                       a = laplacian2d<crd::f64>(&alloc, g);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64>          b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11); }

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            Ic0Preconditioner<crd::f64> ic(a, &alloc);
            IterativeOptions<crd::f64>  opts; opts.rel_tol = 1e-11; opts.max_iter = 5000;
            KrylovWorkspace<crd::f64>   ws(&alloc, n);
            return pcg<crd::f64>(op, ic, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n), xp(&alloc, n);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(xs(i) == xp(i)); }
    }
    crd::jobs::shutdown();
}
