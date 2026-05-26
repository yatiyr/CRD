#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/preconditioners/schwarz.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::SchwarzPartition;
using crd::hesap::preconditioners::SchwarzPreconditioner;
using crd::hesap::preconditioners::SchwarzType;
namespace dense = crd::hesap::dense;

namespace
{
template <typename T>
SparseMatrix<T, SparseFormat::Csr> conv_diff2d(crd::memory::IAllocator* a, crd::u32 g, double beta)
{
    const crd::u32    n = g * g;
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 y = 0; y < g; ++y)
        for (crd::u32 x = 0; x < g; ++x)
        {
            const crd::u32 i = y * g + x;
            b.add(i, i, T(4));
            if (x + 1 < g) { b.add(i, i + 1, T(static_cast<crd::f64>(-1.0 + beta))); }
            if (x > 0) { b.add(i, i - 1, T(static_cast<crd::f64>(-1.0 - beta))); }
            if (y + 1 < g) { b.add(i, i + g, T(static_cast<crd::f64>(-1.0 + beta))); }
            if (y > 0) { b.add(i, i - g, T(static_cast<crd::f64>(-1.0 - beta))); }
        }
    return b.compress();
}
template <typename T>
SparseMatrix<T, SparseFormat::Csr> laplace2d(crd::memory::IAllocator* a, crd::u32 g)
{
    return conv_diff2d<T>(a, g, 0.0);
}
} // namespace

TEST_CASE("Additive Schwarz is SPD and accelerates CG (exact local solves)", "[hesap-iterative][schwarz]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32             g = 28;
        const crd::u32             n = g * g;
        auto                       a = laplace2d<crd::f64>(&alloc, g);
        SparseLinearOp<crd::f64>   op(a);
        dense::Vector<crd::f64>    b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0; }
        IterativeOptions<crd::f64> opts; opts.rel_tol = 1e-9; opts.max_iter = 5000;

        KrylovWorkspace<crd::f64> ws0(&alloc, n);
        dense::Vector<crd::f64>   x0(&alloc, n);
        auto r0 = cg<crd::f64>(op, b.span(), x0.span(), opts, ws0, &alloc);
        REQUIRE(r0.converged);

        // AS is symmetric for symmetric A with exact local solves ⇒ valid for CG (pcg).
        SchwarzPreconditioner<crd::f64> m(a, &alloc, 32, 1, SchwarzType::Additive);
        KrylovWorkspace<crd::f64>       ws(&alloc, n);
        dense::Vector<crd::f64>         x(&alloc, n);
        auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations < r0.iterations);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Restricted Schwarz (RAS)-BiCGSTAB solves a nonsymmetric system", "[hesap-iterative][schwarz]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32             g = 26;
        const crd::u32             n = g * g;
        auto                       a = conv_diff2d<crd::f64>(&alloc, g, 0.4);
        SparseLinearOp<crd::f64>   op(a);
        SchwarzPreconditioner<crd::f64> m(a, &alloc, 32, 1, SchwarzType::Restricted);
        dense::Vector<crd::f64> b(&alloc, n), x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 7); }
        IterativeOptions<crd::f64> opts; opts.rel_tol = 1e-10; opts.max_iter = 5000;
        BicgstabWorkspace<crd::f64> ws(&alloc, n);
        auto res = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        dense::Vector<crd::f64> ax(&alloc, n);
        (void)op.apply(x.span(), ax.span());
        crd::f64 nb = 0, nr = 0;
        for (crd::u32 i = 0; i < n; ++i) { nb += b(i) * b(i); const crd::f64 d = b(i) - ax(i); nr += d * d; }
        REQUIRE(std::sqrt(nr / nb) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Schwarz more overlap => stronger preconditioner; ND partition works",
          "[hesap-iterative][schwarz]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{192U << 20};
        const crd::u32             g = 30;
        const crd::u32             n = g * g;
        auto                       a = conv_diff2d<crd::f64>(&alloc, g, 0.3);
        SparseLinearOp<crd::f64>   op(a);
        dense::Vector<crd::f64>    b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0; }
        auto iters = [&](crd::u32 ov, SchwarzPartition part) {
            SchwarzPreconditioner<crd::f64> m(a, &alloc, 32, ov, SchwarzType::Restricted, part);
            dense::Vector<crd::f64>         x(&alloc, n);
            IterativeOptions<crd::f64>      opts; opts.rel_tol = 1e-9; opts.max_iter = 5000;
            BicgstabWorkspace<crd::f64>     ws(&alloc, n);
            auto r = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(r.converged);
            return r.iterations;
        };
        const auto i1 = iters(1, SchwarzPartition::Contiguous);
        const auto i2 = iters(2, SchwarzPartition::Contiguous);
        REQUIRE(i2 <= i1); // more overlap ⇒ stronger
        const auto ind = iters(1, SchwarzPartition::NestedDissection); // ND partition converges
        REQUIRE(ind > 0);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Restricted Schwarz solves a complex nonsymmetric system", "[hesap-iterative][schwarz][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{48U << 20};
        using C        = Complex<crd::f64>;
        auto           a = conv_diff2d<C>(&alloc, 13, 0.3);
        const crd::u32 n = 169;
        SparseLinearOp<C> op(a);
        SchwarzPreconditioner<C> m(a, &alloc, 24, 1, SchwarzType::Restricted);
        dense::Vector<C> b(&alloc, n), x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = C{1.0, 0.05}; }
        IterativeOptions<crd::f64> opts; opts.rel_tol = 1e-9; opts.max_iter = 2000;
        BicgstabWorkspace<C>       ws(&alloc, n);
        auto res = bicgstab<C>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
    }
    crd::jobs::shutdown();
}

TEST_CASE("RAS-BiCGSTAB is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][schwarz][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32             g = 28;
        const crd::u32             n = g * g;
        auto                       a = conv_diff2d<crd::f64>(&alloc, g, 0.25);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64>          b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i) { b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11); }
        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            SchwarzPreconditioner<crd::f64> m(a, &alloc, 32, 1, SchwarzType::Restricted);
            IterativeOptions<crd::f64>      opts; opts.rel_tol = 1e-10; opts.max_iter = 5000;
            BicgstabWorkspace<crd::f64>     ws(&alloc, n);
            return bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n), xp(&alloc, n);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(xs(i) == xp(i)); }
    }
    crd::jobs::shutdown();
}
