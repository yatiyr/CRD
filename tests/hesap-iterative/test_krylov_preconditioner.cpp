#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/krylov_preconditioner.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::make_krylov_preconditioner;
namespace dense = crd::hesap::dense;

namespace
{
template <typename T>
SparseMatrix<T, SparseFormat::Csr> random_nonsym(crd::memory::IAllocator* a, crd::u32 n, crd::u64 seed)
{
    crd::u64 state = seed;
    auto     next  = [&state]() -> double {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
    };
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (dense::is_complex_v<T>) { b.add(i, i, T{5.0, 0.5}); }
        else { b.add(i, i, T(5)); }
        for (crd::u32 off = 0; off < 5; ++off)
        {
            const crd::u32 j = static_cast<crd::u32>(next() * n);
            if (j == i) { continue; }
            const double re = next() - 0.5;
            if constexpr (dense::is_complex_v<T>) { b.add(i, j, T{re, next() - 0.5}); }
            else { b.add(i, j, static_cast<T>(re)); }
        }
    }
    return b.compress();
}

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
crd::hesap::dense::RealType<T> true_residual(const crd::hesap::LinearOp<T>& op, crd::containers::ConstSpan<T> x,
                                             crd::containers::ConstSpan<T> b, crd::memory::IAllocator* a)
{
    const crd::usize n = op.n_rows();
    dense::Vector<T> ax(a, n);
    (void)op.apply(x, ax.span());
    dense::Vector<T> r(a, n);
    for (crd::usize i = 0; i < n; ++i) { r(i) = b[i] - ax(i); }
    return dense::nrm2<T>(r.span()) / dense::nrm2<T>(b);
}
} // namespace

TEST_CASE("KrylovPreconditioner: inner-GMRES preconditioner accelerates outer FGMRES",
          "[hesap-iterative][nested]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             n = 200;
        auto                       a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0xF1A7ULL);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);
        const crd::usize           m = 10; // small outer restart

        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 4000;

        // Plain FGMRES(m) (no preconditioner).
        dense::Vector<crd::f64>  xp(&alloc, n);
        GmresWorkspace<crd::f64> wp(&alloc, n, m);
        auto                     rp = gmres<crd::f64>(op, b.span(), xp.span(), opts, wp, &alloc);

        // Nested: FGMRES(m)-outer with a few-iteration inner GMRES as the preconditioner.
        GmresWorkspace<crd::f64> inner_ws(&alloc, n, /*restart=*/10);
        auto                     inner = [&](crd::containers::ConstSpan<crd::f64> r, crd::containers::Span<crd::f64> z) {
            IterativeOptions<crd::f64> io;
            io.max_iter = 8;     // a few inner iterations approximate A⁻¹
            io.rel_tol  = 1e-2;
            (void)gmres<crd::f64>(op, r, z, io, inner_ws, &alloc);
        };
        auto P = make_krylov_preconditioner<crd::f64>(n, inner);

        dense::Vector<crd::f64>  xn(&alloc, n);
        GmresWorkspace<crd::f64> wn(&alloc, n, m);
        auto                     rn = fgmres<crd::f64>(op, &P, b.span(), xn.span(), opts, wn, &alloc);

        REQUIRE(rp.converged);
        REQUIRE(rn.converged);
        REQUIRE(true_residual<crd::f64>(op, xn.span(), b.span(), &alloc) < 1e-8);
        // The nested inner solve approximates A⁻¹ ⇒ the outer converges in strictly
        // fewer OUTER iterations than unpreconditioned FGMRES(m).
        REQUIRE(rn.iterations < rp.iterations);
    }
    crd::jobs::shutdown();
}

TEST_CASE("KrylovPreconditioner: inner-CG preconditioner composes on an SPD system", "[hesap-iterative][nested]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             n = 200;
        auto                       a = spd_tridiag<crd::f64>(&alloc, n);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);

        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 2000;

        KrylovWorkspace<crd::f64> inner_ws(&alloc, n);
        auto inner = [&](crd::containers::ConstSpan<crd::f64> r, crd::containers::Span<crd::f64> z) {
            IterativeOptions<crd::f64> io;
            io.max_iter = 10;
            io.rel_tol  = 1e-3;
            (void)cg<crd::f64>(op, r, z, io, inner_ws, &alloc);
        };
        auto P = make_krylov_preconditioner<crd::f64>(n, inner);

        dense::Vector<crd::f64>  x(&alloc, n);
        GmresWorkspace<crd::f64> w(&alloc, n, /*restart=*/20);
        auto                     r = fgmres<crd::f64>(op, &P, b.span(), x.span(), opts, w, &alloc);
        REQUIRE(r.converged);
        REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("KrylovPreconditioner: nested inner-GMRES on a complex system (c64)", "[hesap-iterative][nested][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        using C        = Complex<crd::f64>;
        const crd::u32 n = 150;
        auto           a = random_nonsym<C>(&alloc, n, /*seed=*/0xC0FFEEABULL);
        ParallelSparseLinearOp<C> op(a, &alloc);
        dense::Vector<C> b(&alloc, n);
        b.fill(C{1.0, 0.0});

        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 3000;

        GmresWorkspace<C> inner_ws(&alloc, n, /*restart=*/10);
        auto inner = [&](crd::containers::ConstSpan<C> r, crd::containers::Span<C> z) {
            IterativeOptions<crd::f64> io;
            io.max_iter = 8;
            io.rel_tol  = 1e-2;
            (void)gmres<C>(op, r, z, io, inner_ws, &alloc);
        };
        auto P = make_krylov_preconditioner<C>(n, inner);

        dense::Vector<C>  x(&alloc, n);
        GmresWorkspace<C> w(&alloc, n, /*restart=*/10);
        auto              r = fgmres<C>(op, &P, b.span(), x.span(), opts, w, &alloc);
        REQUIRE(r.converged);
        REQUIRE(true_residual<C>(op, x.span(), b.span(), &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("KrylovPreconditioner: nested solve is bit-exact over serial vs parallel spmv",
          "[hesap-iterative][nested][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32             n = 300;
        auto                       a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0xD377EDULL);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](crd::usize threshold, dense::Vector<crd::f64>& x) {
            ParallelSparseLinearOp<crd::f64> op(a, &alloc, threshold);
            IterativeOptions<crd::f64>       opts;
            opts.rel_tol          = 1e-10;
            opts.max_iter         = 2000;
            opts.record_residuals = true;
            GmresWorkspace<crd::f64> inner_ws(&alloc, n, 10);
            auto inner = [&](crd::containers::ConstSpan<crd::f64> r, crd::containers::Span<crd::f64> z) {
                IterativeOptions<crd::f64> io;
                io.max_iter = 8;
                io.rel_tol  = 1e-2;
                (void)gmres<crd::f64>(op, r, z, io, inner_ws, &alloc);
            };
            auto                     P = make_krylov_preconditioner<crd::f64>(n, inner);
            GmresWorkspace<crd::f64> w(&alloc, n, 10);
            return fgmres<crd::f64>(op, &P, b.span(), x.span(), opts, w, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto                    rs = solve(~crd::usize{0}, xs); // serial spmv
        auto                    rp = solve(0, xp);              // parallel spmv
        REQUIRE(rs.iterations == rp.iterations);
        REQUIRE(rs.residual_history.size() == rp.residual_history.size());
        for (crd::usize i = 0; i < rs.residual_history.size(); ++i)
        {
            REQUIRE(rs.residual_history[i] == rp.residual_history[i]);
        }
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(xs(i) == xp(i)); }
    }
    crd::jobs::shutdown();
}
