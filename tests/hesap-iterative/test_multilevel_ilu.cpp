#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/preconditioners/ilut.hpp>
#include <crd/hesap/preconditioners/multilevel_ilu.hpp>
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
using crd::hesap::preconditioners::IlutPreconditioner;
using crd::hesap::preconditioners::MultilevelIlu;
namespace dense = crd::hesap::dense;

namespace
{
// A diagonally-dominant tridiagonal with its columns cyclically shifted by +1, so each row's
// LARGEST entry sits OFF the diagonal — plain ILUT factors the small shifted diagonal, MC64
// recovers the matching and factors the well-conditioned core.
template <typename T> SparseMatrix<T, SparseFormat::Csr> shifted_tridiag(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, (i + 1) % n, T(4)); // big entry → off-diagonal (matched by MC64)
        b.add(i, (i + 2) % n, T(static_cast<crd::f64>(-1.2)));
        b.add(i, i, T(static_cast<crd::f64>(-0.9))); // shifted small diagonal
    }
    return b.compress();
}

template <typename T>
double true_resid(SparseLinearOp<T>& op, const dense::Vector<T>& b, const dense::Vector<T>& x,
                  crd::memory::IAllocator* alloc, crd::u32 n)
{
    dense::Vector<T> ax(alloc, n);
    (void)op.apply(x.span(), ax.span());
    double nb = 0;
    double nr = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const auto bi = b(i);
        if constexpr (crd::hesap::dense::is_complex_v<T>)
        {
            nb += double(bi.re) * bi.re + double(bi.im) * bi.im;
        }
        else
        {
            nb += double(bi) * bi;
        }
        const auto d = b(i) - ax(i);
        if constexpr (crd::hesap::dense::is_complex_v<T>)
        {
            nr += double(d.re) * d.re + double(d.im) * d.im;
        }
        else
        {
            nr += double(d) * d;
        }
    }
    return std::sqrt(nr / nb);
}
} // namespace

TEST_CASE("MultilevelIlu (MC64+ILUT) solves the original system with off-diagonal-dominant entries",
          "[hesap-iterative][multilevel_ilu]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 300;
        auto a = shifted_tridiag<crd::f64>(&alloc, n);
        SparseLinearOp<crd::f64> op(a);
        MultilevelIlu<crd::f64> m(a, &alloc, 20U, 1e-4);
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.01 * static_cast<crd::f64>(i % 7);
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 2000;
        BicgstabWorkspace<crd::f64> ws(&alloc, n);
        auto res = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_resid<crd::f64>(op, b, x, &alloc, n) < 1e-8); // validates the MC64 transform
    }
    crd::jobs::shutdown();
}

TEST_CASE("MultilevelIlu beats plain ILUT on the off-diagonal-dominant matrix (the MC64 payoff)",
          "[hesap-iterative][multilevel_ilu]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 300;
        auto a = shifted_tridiag<crd::f64>(&alloc, n);
        SparseLinearOp<crd::f64> op(a);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
        }
        auto iters = [&](const crd::hesap::LinearOp<crd::f64>& m, bool& conv)
        {
            dense::Vector<crd::f64> x(&alloc, n);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-9;
            opts.max_iter = 4000;
            BicgstabWorkspace<crd::f64> ws(&alloc, n);
            auto r = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
            conv = r.converged;
            return r.iterations;
        };
        IlutPreconditioner<crd::f64> plain(a, &alloc, 20U, 1e-4);
        MultilevelIlu<crd::f64> mc64(a, &alloc, 20U, 1e-4);
        bool pc = false;
        bool mc = false;
        const auto ip = iters(plain, pc);
        const auto im = iters(mc64, mc);
        REQUIRE(mc);                // MC64+ILUT converges
        REQUIRE((!pc || im <= ip)); // and beats plain ILUT (fewer iters) — or plain fails entirely
    }
    crd::jobs::shutdown();
}

TEST_CASE("MultilevelIlu solves a complex off-diagonal-dominant system", "[hesap-iterative][multilevel_ilu][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 160;
        auto a = shifted_tridiag<C>(&alloc, n);
        SparseLinearOp<C> op(a);
        MultilevelIlu<C> m(a, &alloc, 20U, 1e-4);
        dense::Vector<C> b(&alloc, n);
        dense::Vector<C> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = C{1.0, 0.1};
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 2000;
        BicgstabWorkspace<C> ws(&alloc, n);
        auto res = bicgstab<C>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
    }
    crd::jobs::shutdown();
}

TEST_CASE("MultilevelIlu is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][multilevel_ilu][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32 n = 400;
        auto a = shifted_tridiag<crd::f64>(&alloc, n);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, ~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, 0);
        dense::Vector<crd::f64> b(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11);
        }
        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            MultilevelIlu<crd::f64> m(a, &alloc, 20U, 1e-4);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-10;
            opts.max_iter = 4000;
            BicgstabWorkspace<crd::f64> ws(&alloc, n);
            return bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
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
