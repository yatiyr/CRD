#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/rminres.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
namespace dense = crd::hesap::dense;

namespace
{
// Symmetric tridiagonal with a few small-diagonal rows (deflatable near-zero modes)
// + a uniform shift. tridiag(-1, d_i + shift, -1). For shift near 0 the spectrum
// straddles 0 ⇒ INDEFINITE (the regime CG diverges on, MINRES is built for).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> sym_indef(crd::memory::IAllocator* a, crd::u32 n, crd::u32 nsmall, double shift)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const double d = (i < nsmall ? 0.2 : 4.0) + shift;
        b.add(i, i, static_cast<T>(d));
        if (i + 1 < n)
        {
            b.add(i, i + 1, T(-1));
        }
        if (i > 0)
        {
            b.add(i, i - 1, T(-1));
        } // symmetric off-diagonals
    }
    return b.compress();
}

// Complex HERMITIAN: real diagonal + conjugate off-diagonals (A[i,i+1] = conj(A[i+1,i])).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> herm_indef(crd::memory::IAllocator* a, crd::u32 n, crd::u32 nsmall)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T{(i < nsmall ? 0.2 : 4.0), 0.0}); // real diagonal (Hermitian)
        if (i + 1 < n)
        {
            b.add(i, i + 1, T{-1.0, 0.3});
            b.add(i + 1, i, T{-1.0, -0.3}); // conjugate ⇒ Hermitian
        }
    }
    return b.compress();
}

template <typename T>
crd::hesap::dense::RealType<T> true_residual(const crd::hesap::LinearOp<T>& op, crd::containers::ConstSpan<T> x,
                                             crd::containers::ConstSpan<T> b, crd::memory::IAllocator* a)
{
    const crd::usize n = op.n_rows();
    dense::Vector<T> ax(a, n);
    (void)op.apply(x, ax.span());
    dense::Vector<T> r(a, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        r(i) = b[i] - ax(i);
    }
    return dense::nrm2<T>(r.span()) / dense::nrm2<T>(b);
}
} // namespace

TEST_CASE("RMINRES solves a symmetric INDEFINITE system (f64)", "[hesap-iterative][rminres]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 n = 200;
        auto a = sym_indef<crd::f64>(&alloc, n, /*nsmall=*/6, /*shift=*/-0.3); // indefinite
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);
        dense::Vector<crd::f64> x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 4000;
        RminresWorkspace<crd::f64> ws(&alloc, n, /*inner=*/20, /*recycle=*/10);

        auto res = rminres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("RMINRES solves a complex Hermitian indefinite system (c64)", "[hesap-iterative][rminres][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 160;
        auto a = herm_indef<C>(&alloc, n, /*nsmall=*/6);
        ParallelSparseLinearOp<C> op(a, &alloc);
        dense::Vector<C> b(&alloc, n);
        b.fill(C{1.0, 0.0});
        dense::Vector<C> x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 4000;
        RminresWorkspace<C> ws(&alloc, n, /*inner=*/20, /*recycle=*/10);

        auto res = rminres<C>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<C>(op, x.span(), b.span(), &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("RMINRES cross-solve recycling is correct across DIFFERENT symmetric operators",
          "[hesap-iterative][rminres][recycle]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32 n = 200;
        auto a1 = sym_indef<crd::f64>(&alloc, n, /*nsmall=*/6, /*shift=*/0.0);
        auto a2 = sym_indef<crd::f64>(&alloc, n, /*nsmall=*/6, /*shift=*/1.5); // different symmetric operator
        ParallelSparseLinearOp<crd::f64> op1(a1, &alloc);
        ParallelSparseLinearOp<crd::f64> op2(a2, &alloc);
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 4000;

        RecycleSpace<crd::f64> rs(&alloc, n, /*recycle=*/10);
        RminresWorkspace<crd::f64> ws(&alloc, n, /*inner=*/20, /*recycle=*/10);

        dense::Vector<crd::f64> x1(&alloc, n);
        auto r1 = rminres_recycled<crd::f64>(op1, b.span(), x1.span(), opts, ws, rs, &alloc);
        REQUIRE(r1.converged);
        REQUIRE(rs.dimension() > 0);

        dense::Vector<crd::f64> x2(&alloc, n);
        auto r2 = rminres_recycled<crd::f64>(op2, b.span(), x2.span(), opts, ws, rs, &alloc);
        REQUIRE(r2.converged);
        // C = A2·U rebuilt on entry ⇒ the A2 solution is correct despite rs filled on A1.
        REQUIRE(true_residual<crd::f64>(op2, x2.span(), b.span(), &alloc) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("RMINRES cross-solve recycling saves iterations across a symmetric shift sequence",
          "[hesap-iterative][rminres][recycle]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32 n = 220;
        const crd::u32 nsmall = 6;
        const double shifts[6] = {0.0, 0.04, 0.08, 0.12, 0.16, 0.2};
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-10;
        opts.max_iter = 6000;

        // Fresh RMINRES per system (recycle built within each solve, NOT carried over).
        crd::usize fresh_total = 0;
        for (double s : shifts)
        {
            auto a = sym_indef<crd::f64>(&alloc, n, nsmall, s);
            ParallelSparseLinearOp<crd::f64> op(a, &alloc);
            dense::Vector<crd::f64> x(&alloc, n);
            RminresWorkspace<crd::f64> ws(&alloc, n, /*inner=*/10, /*recycle=*/10);
            auto r = rminres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(r.converged);
            fresh_total += r.iterations;
        }

        // RMINRES with a PERSISTENT recycle space carried across the sequence.
        crd::usize recyc_total = 0;
        RecycleSpace<crd::f64> rs(&alloc, n, /*recycle=*/10);
        RminresWorkspace<crd::f64> ws(&alloc, n, /*inner=*/10, /*recycle=*/10);
        for (double s : shifts)
        {
            auto a = sym_indef<crd::f64>(&alloc, n, nsmall, s);
            ParallelSparseLinearOp<crd::f64> op(a, &alloc);
            dense::Vector<crd::f64> x(&alloc, n);
            auto r = rminres_recycled<crd::f64>(op, b.span(), x.span(), opts, ws, rs, &alloc);
            REQUIRE(r.converged);
            recyc_total += r.iterations;
        }

        // The carried-over deflation space accelerates solves 2..N ⇒ fewer total iters.
        REQUIRE(recyc_total < fresh_total * 7 / 10);
    }
    crd::jobs::shutdown();
}

TEST_CASE("RMINRES is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][rminres][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32 n = 300;
        auto a = sym_indef<crd::f64>(&alloc, n, /*nsmall=*/6, /*shift=*/-0.2);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, /*parallel_min_stored_bytes=*/~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-10;
            opts.max_iter = 2000;
            opts.record_residuals = true;
            RminresWorkspace<crd::f64> ws(&alloc, n, /*inner=*/20, /*recycle=*/10);
            return rminres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto rs = solve(serial_op, xs);
        auto rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        REQUIRE(rs.residual_history.size() == rp.residual_history.size());
        for (crd::usize i = 0; i < rs.residual_history.size(); ++i)
        {
            REQUIRE(rs.residual_history[i] == rp.residual_history[i]);
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(xs(i) == xp(i));
        }
    }
    crd::jobs::shutdown();
}
