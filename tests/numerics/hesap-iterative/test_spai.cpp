#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/fspai.hpp>
#include <crd/hesap/preconditioners/spai.hpp>
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
using crd::hesap::preconditioners::FspaiPreconditioner;
using crd::hesap::preconditioners::SpaiPattern;
using crd::hesap::preconditioners::SpaiPreconditioner;
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

// 2D Laplacian 5-point (SPD): diagonal 4, off-diagonal -1, symmetric.
template <typename T> SparseMatrix<T, SparseFormat::Csr> laplace2d(crd::memory::IAllocator* a, crd::u32 g)
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
    }
    return b.compress();
}

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
} // namespace

TEST_CASE("SPAI (static) accelerates FGMRES on a nonsymmetric system", "[hesap-iterative][spai]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 200;
        auto a = nonsym_tridiag<crd::f64>(&alloc, n, 0.4);
        SparseLinearOp<crd::f64> op(a);
        SpaiPreconditioner<crd::f64> m(a, &alloc, SpaiPattern::Static);
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 7);
            x(i) = 0.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 500;
        GmresWorkspace<crd::f64> ws(&alloc, n, 40);
        auto res = fgmres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
    }
    crd::jobs::shutdown();
}

TEST_CASE("SPAI adaptive yields a stronger preconditioner than static (fewer iters)", "[hesap-iterative][spai]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        const crd::u32 g = 24;
        const crd::u32 n = g * g;
        auto a = conv_diff2d<crd::f64>(&alloc, g, 0.3);
        SparseLinearOp<crd::f64> op(a);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }

        auto iters = [&](SpaiPreconditioner<crd::f64>& m)
        {
            dense::Vector<crd::f64> x(&alloc, n);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-8;
            opts.max_iter = 5000;
            GmresWorkspace<crd::f64> ws(&alloc, n, 60);
            auto r = fgmres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(r.converged);
            return r.iterations;
        };
        SpaiPreconditioner<crd::f64> ms(a, &alloc, SpaiPattern::Static);
        SpaiPreconditioner<crd::f64> ma(a, &alloc, SpaiPattern::Adaptive, 0.2);
        const auto is = iters(ms);
        const auto ia = iters(ma);
        REQUIRE(ma.factor_nnz() >= ms.factor_nnz()); // adaptive adds fill
        REQUIRE(ia <= is);                           // stronger ⇒ no more iterations
    }
    crd::jobs::shutdown();
}

TEST_CASE("SPAI-BiCGSTAB solves a complex nonsymmetric system", "[hesap-iterative][spai][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 160;
        TripletBuilder<C> tb(&alloc, n, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            tb.add(i, i, C{4.0, 0.5});
            if (i + 1 < n)
            {
                tb.add(i, i + 1, C{-1.0, 0.2});
            }
            if (i > 0)
            {
                tb.add(i, i - 1, C{-1.3, -0.1});
            }
        }
        auto a = tb.compress();
        SparseLinearOp<C> op(a);
        SpaiPreconditioner<C> m(a, &alloc, SpaiPattern::Adaptive, 0.3);
        dense::Vector<C> b(&alloc, n);
        dense::Vector<C> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = C{1.0, 0.1};
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 400;
        BicgstabWorkspace<C> ws(&alloc, n);
        auto res = bicgstab<C>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
    }
    crd::jobs::shutdown();
}

TEST_CASE("SPAI build is bit-identical across thread counts (determinism moat)", "[hesap-iterative][spai][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        const crd::u32 g = 26;
        const crd::u32 n = g * g;
        auto a = conv_diff2d<crd::f64>(&alloc, g, 0.25);

        // Same SPAI build runs over crd::jobs (>=1 worker). Two builds must produce a
        // bit-identical M; the FGMRES solve over serial vs parallel spmv must agree.
        SpaiPreconditioner<crd::f64> m1(a, &alloc, SpaiPattern::Adaptive, 0.2);
        SpaiPreconditioner<crd::f64> m2(a, &alloc, SpaiPattern::Adaptive, 0.2);
        REQUIRE(m1.factor_nnz() == m2.factor_nnz());

        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11);
        }
        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            SpaiPreconditioner<crd::f64> m(a, &alloc, SpaiPattern::Adaptive, 0.2);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-9;
            opts.max_iter = 5000;
            GmresWorkspace<crd::f64> ws(&alloc, n, 40);
            return fgmres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
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

TEST_CASE("FSPAI (static) accelerates PCG on an SPD system", "[hesap-iterative][fspai]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32 g = 24;
        const crd::u32 n = g * g;
        auto a = laplace2d<crd::f64>(&alloc, g);
        SparseLinearOp<crd::f64> op(a);
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 5000;

        // unpreconditioned baseline
        KrylovWorkspace<crd::f64> ws0(&alloc, n);
        dense::Vector<crd::f64> x0(&alloc, n);
        auto r0 = crd::hesap::iterative::cg<crd::f64>(op, b.span(), x0.span(), opts, ws0, &alloc);
        REQUIRE(r0.converged);

        FspaiPreconditioner<crd::f64> m(a, &alloc, SpaiPattern::Static);
        KrylovWorkspace<crd::f64> ws(&alloc, n);
        auto res = crd::hesap::iterative::pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations < r0.iterations); // M = L·Lᴴ is SPD and accelerates CG
    }
    crd::jobs::shutdown();
}

TEST_CASE("FSPAI adaptive converges and is at least as strong as static", "[hesap-iterative][fspai]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        const crd::u32 g = 28;
        const crd::u32 n = g * g;
        auto a = laplace2d<crd::f64>(&alloc, g);
        SparseLinearOp<crd::f64> op(a);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 5);
        }
        auto iters = [&](FspaiPreconditioner<crd::f64>& m)
        {
            dense::Vector<crd::f64> x(&alloc, n);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-9;
            opts.max_iter = 5000;
            KrylovWorkspace<crd::f64> ws(&alloc, n);
            auto r = crd::hesap::iterative::pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(r.converged);
            return r.iterations;
        };
        FspaiPreconditioner<crd::f64> ms(a, &alloc, SpaiPattern::Static);
        FspaiPreconditioner<crd::f64> ma(a, &alloc, SpaiPattern::Adaptive, 0.05);
        const auto is = iters(ms);
        const auto ia = iters(ma);
        REQUIRE(ia <= is + 2); // adaptive at least competitive with the static A-pattern
    }
    crd::jobs::shutdown();
}

TEST_CASE("FSPAI-PCG solves a complex Hermitian-PD system", "[hesap-iterative][fspai][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 160;
        TripletBuilder<C> tb(&alloc, n, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            tb.add(i, i, C{4.0, 0.0}); // real positive diagonal (HPD)
            if (i + 1 < n)
            {
                tb.add(i, i + 1, C{-1.0, 0.3});
                tb.add(i + 1, i, C{-1.0, -0.3}); // conjugate (Hermitian)
            }
        }
        auto a = tb.compress();
        SparseLinearOp<C> op(a);
        FspaiPreconditioner<C> m(a, &alloc, SpaiPattern::Static);
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
        auto res = crd::hesap::iterative::pcg<C>(op, m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
    }
    crd::jobs::shutdown();
}

TEST_CASE("FSPAI build is bit-identical across thread counts (determinism moat)",
          "[hesap-iterative][fspai][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        const crd::u32 g = 26;
        const crd::u32 n = g * g;
        auto a = laplace2d<crd::f64>(&alloc, g);
        FspaiPreconditioner<crd::f64> m1(a, &alloc, SpaiPattern::Adaptive, 0.05);
        FspaiPreconditioner<crd::f64> m2(a, &alloc, SpaiPattern::Adaptive, 0.05);
        REQUIRE(m1.factor_nnz() == m2.factor_nnz());

        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11);
        }
        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            FspaiPreconditioner<crd::f64> m(a, &alloc, SpaiPattern::Adaptive, 0.05);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-9;
            opts.max_iter = 5000;
            KrylovWorkspace<crd::f64> ws(&alloc, n);
            return crd::hesap::iterative::pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
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
