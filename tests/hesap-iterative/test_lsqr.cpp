#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/lsmr.hpp>
#include <crd/hesap/iterative/lsqr.hpp>
#include <crd/hesap/preconditioners/column_jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::LeastSquaresColumnJacobi;
namespace dense = crd::hesap::dense;

namespace
{
// Tall, full-column-rank A (m×n, m>n): rows [0,n) = tridiag(-1,2,-1); rows [n,m)
// add extra equations A[n+k,k]=1. Overdetermined ⇒ a genuine least-squares problem.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> tall(crd::memory::IAllocator* a, crd::u32 m, crd::u32 n)
{
    TripletBuilder<T> b(a, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(2));
        if (i + 1 < n) { b.add(i, i + 1, T(-1)); }
        if (i > 0) { b.add(i, i - 1, T(-1)); }
    }
    for (crd::u32 k = 0; n + k < m && k < n; ++k)
    {
        b.add(n + k, k, T(1));
    }
    return b.compress();
}

// Normal-equations residual ‖Aᴴ(Ax−b)‖ / ‖Aᴴb‖ -- the least-squares optimality test.
template <typename T>
crd::hesap::dense::RealType<T> normal_residual(const crd::hesap::LinearOp<T>& op, crd::containers::ConstSpan<T> x,
                                               crd::containers::ConstSpan<T> b, crd::memory::IAllocator* a)
{
    const crd::usize m = op.n_rows();
    const crd::usize n = op.n_cols();
    dense::Vector<T> ax(a, m);
    (void)op.apply(x, ax.span());
    dense::Vector<T> r(a, m);
    for (crd::usize i = 0; i < m; ++i) { r(i) = ax(i) - b[i]; }
    dense::Vector<T> atr(a, n);
    (void)op.apply_adjoint(r.span(), atr.span());
    dense::Vector<T> atb(a, n);
    (void)op.apply_adjoint(b, atb.span());
    return dense::nrm2<T>(atr.span()) / dense::nrm2<T>(atb.span());
}
} // namespace

TEST_CASE("LSQR solves an overdetermined least-squares problem (f64)", "[hesap-iterative][lsqr]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    const crd::u32 m = 120;
    const crd::u32 n = 80;
    auto                       a = tall<crd::f64>(&alloc, m, n);
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, m);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    LsqrWorkspace<crd::f64> ws(&alloc, m, n);

    auto res = lsqr<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(normal_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8); // Aᴴr ≈ 0
}

TEST_CASE("LSQR solves a consistent square system (f64)", "[hesap-iterative][lsqr]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32             n = 64;
    auto                       a = tall<crd::f64>(&alloc, n, n); // square (m==n) tridiagonal SPD
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    LsqrWorkspace<crd::f64> ws(&alloc, n, n);

    auto res = lsqr<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    // consistent: residual ‖Ax-b‖/‖b‖ small.
    dense::Vector<crd::f64> ax(&alloc, n);
    (void)op.apply(x.span(), ax.span());
    dense::Vector<crd::f64> diff(&alloc, n);
    for (crd::u32 i = 0; i < n; ++i) { diff(i) = ax(i) - b(i); }
    REQUIRE(dense::nrm2<crd::f64>(diff.span()) / dense::nrm2<crd::f64>(b.span()) < 1e-8);
}

TEST_CASE("LSQR solves a complex least-squares problem (c64)", "[hesap-iterative][lsqr][complex]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    using C        = Complex<crd::f64>;
    const crd::u32 m = 100;
    const crd::u32 n = 60;
    TripletBuilder<C> bld(&alloc, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bld.add(i, i, C{2.0, 0.5});
        if (i + 1 < n) { bld.add(i, i + 1, C{-1.0, 0.2}); }
    }
    for (crd::u32 k = 0; n + k < m && k < n; ++k) { bld.add(n + k, k, C{1.0, -0.3}); }
    auto             a = bld.compress();
    ParallelSpmvLeastSquaresOp<C> op(a, &alloc);
    dense::Vector<C> b(&alloc, m);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    LsqrWorkspace<C> ws(&alloc, m, n);

    auto res = lsqr<C>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(normal_residual<C>(op, x.span(), b.span(), &alloc) < 1e-7);
}

TEST_CASE("LSMR solves an overdetermined least-squares problem (f64)", "[hesap-iterative][lsmr]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    const crd::u32 m = 120;
    const crd::u32 n = 80;
    auto                       a = tall<crd::f64>(&alloc, m, n);
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, m);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    LsmrWorkspace<crd::f64> ws(&alloc, m, n);

    auto res = lsmr<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(normal_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
}

TEST_CASE("LSMR solves a complex least-squares problem (c64)", "[hesap-iterative][lsmr][complex]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    using C        = Complex<crd::f64>;
    const crd::u32 m = 100;
    const crd::u32 n = 60;
    TripletBuilder<C> bld(&alloc, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bld.add(i, i, C{2.0, 0.5});
        if (i + 1 < n) { bld.add(i, i + 1, C{-1.0, 0.2}); }
    }
    for (crd::u32 k = 0; n + k < m && k < n; ++k) { bld.add(n + k, k, C{1.0, -0.3}); }
    auto             a = bld.compress();
    ParallelSpmvLeastSquaresOp<C> op(a, &alloc);
    dense::Vector<C> b(&alloc, m);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    LsmrWorkspace<C> ws(&alloc, m, n);

    auto res = lsmr<C>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(normal_residual<C>(op, x.span(), b.span(), &alloc) < 1e-7);
}

TEST_CASE("LSMR is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][lsmr][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 m = 260;
        const crd::u32 n = 180;
        auto                       a = tall<crd::f64>(&alloc, m, n);
        ParallelSpmvLeastSquaresOp<crd::f64> serial_op(a, &alloc, /*parallel_min_stored_bytes=*/~crd::usize{0});
        ParallelSpmvLeastSquaresOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        dense::Vector<crd::f64> b(&alloc, m);
        b.fill(1.0);
        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol          = 1e-12;
            opts.max_iter         = 400;
            opts.record_residuals = true;
            LsmrWorkspace<crd::f64> ws(&alloc, m, n);
            return lsmr<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::usize i = 0; i < rs.residual_history.size(); ++i)
        {
            REQUIRE(rs.residual_history[i] == rp.residual_history[i]);
        }
        for (crd::u32 i = 0; i < n; ++i) { REQUIRE(xs(i) == xp(i)); }
    }
    crd::jobs::shutdown();
}

namespace
{
// Tall full-rank A whose column j is scaled by 10^(decades·j/(n-1)): columns then
// span many orders of magnitude ⇒ huge cond(AᴴA). Column-Jacobi (M=diag(AᴴA)⁻¹)
// undoes the scaling; plain LSQR/LSMR crawls. The unscaled column norms come from
// the tridiag stencil so the matrix stays full column rank.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> column_scaled_tall(crd::memory::IAllocator* a, crd::u32 m, crd::u32 n, double decades)
{
    TripletBuilder<T> b(a, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const T s = static_cast<T>(std::pow(10.0, decades * static_cast<double>(i) / static_cast<double>(n - 1)));
        b.add(i, i, T(2) * s);
        if (i + 1 < n)
        {
            const T s1 = static_cast<T>(std::pow(10.0, decades * static_cast<double>(i + 1) / static_cast<double>(n - 1)));
            b.add(i, i + 1, T(-1) * s1);
        }
        if (i > 0)
        {
            const T s0 = static_cast<T>(std::pow(10.0, decades * static_cast<double>(i - 1) / static_cast<double>(n - 1)));
            b.add(i, i - 1, T(-1) * s0);
        }
    }
    for (crd::u32 k = 0; n + k < m && k < n; ++k)
    {
        const T s = static_cast<T>(std::pow(10.0, decades * static_cast<double>(k) / static_cast<double>(n - 1)));
        b.add(n + k, k, T(1) * s);
    }
    return b.compress();
}
} // namespace

TEST_CASE("Column-Jacobi LSQR converges on a badly column-scaled problem where plain LSQR stalls",
          "[hesap-iterative][lsqr][precond]")
{
    crd::memory::TlsfAllocator alloc{32U << 20};
    const crd::u32             m = 120;
    const crd::u32             n = 80;
    auto                       a = column_scaled_tall<crd::f64>(&alloc, m, n, /*decades=*/6.0);
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, m);
    b.fill(1.0);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-10;
    // Column-Jacobi cuts cond(AᴴA) from ~1e12 (decades=6 ⇒ scaling spans 1e6,
    // squared) down to the base column-normalized tridiag (~n²), so it converges
    // in a few hundred iters; plain LSQR would need ~sqrt(1e12)=1e6. This budget
    // separates the two cleanly.
    opts.max_iter = 400;

    dense::Vector<crd::f64> x_plain(&alloc, n);
    LsqrWorkspace<crd::f64> ws_plain(&alloc, m, n);
    auto                    res_plain = lsqr<crd::f64>(op, b.span(), x_plain.span(), opts, ws_plain, &alloc);

    LeastSquaresColumnJacobi<crd::f64> nprec(a, &alloc);
    dense::Vector<crd::f64>            x_jac(&alloc, n);
    LsqrWorkspace<crd::f64>            ws_jac(&alloc, m, n);
    auto res_jac = lsqr<crd::f64>(op, &nprec, b.span(), x_jac.span(), opts, ws_jac, &alloc);

    const auto nr_plain = normal_residual<crd::f64>(op, x_plain.span(), b.span(), &alloc);
    const auto nr_jac   = normal_residual<crd::f64>(op, x_jac.span(), b.span(), &alloc);

    REQUIRE(res_jac.converged);
    REQUIRE(nr_jac < 1e-7);          // column-Jacobi solves the LS problem
    REQUIRE_FALSE(res_plain.converged); // plain LSQR stalls in the same budget
    REQUIRE(nr_jac < nr_plain);      // preconditioner strictly improves optimality
}

TEST_CASE("Column-Jacobi LSMR converges on a badly column-scaled problem where plain LSMR stalls",
          "[hesap-iterative][lsmr][precond]")
{
    crd::memory::TlsfAllocator alloc{32U << 20};
    const crd::u32             m = 120;
    const crd::u32             n = 80;
    auto                       a = column_scaled_tall<crd::f64>(&alloc, m, n, /*decades=*/6.0);
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, m);
    b.fill(1.0);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-10;
    opts.max_iter = 400; // see LSQR counterpart: separates Jacobi (~hundreds) from plain (~1e6).

    dense::Vector<crd::f64> x_plain(&alloc, n);
    LsmrWorkspace<crd::f64> ws_plain(&alloc, m, n);
    auto                    res_plain = lsmr<crd::f64>(op, b.span(), x_plain.span(), opts, ws_plain, &alloc);

    LeastSquaresColumnJacobi<crd::f64> nprec(a, &alloc);
    dense::Vector<crd::f64>            x_jac(&alloc, n);
    LsmrWorkspace<crd::f64>            ws_jac(&alloc, m, n);
    auto res_jac = lsmr<crd::f64>(op, &nprec, b.span(), x_jac.span(), opts, ws_jac, &alloc);

    const auto nr_plain = normal_residual<crd::f64>(op, x_plain.span(), b.span(), &alloc);
    const auto nr_jac   = normal_residual<crd::f64>(op, x_jac.span(), b.span(), &alloc);

    REQUIRE(res_jac.converged);
    REQUIRE(nr_jac < 1e-7);
    REQUIRE_FALSE(res_plain.converged);
    REQUIRE(nr_jac < nr_plain);
}

TEST_CASE("Column-Jacobi LSQR is bit-exact over serial vs parallel spmv (determinism moat holds under precond)",
          "[hesap-iterative][lsqr][precond][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             m = 260;
        const crd::u32             n = 180;
        auto                       a = column_scaled_tall<crd::f64>(&alloc, m, n, /*decades=*/4.0);
        // The N preconditioner is shared (a pure per-element multiply, no reduction
        // ⇒ thread-count-independent by construction); only the spmv parallelism flips.
        LeastSquaresColumnJacobi<crd::f64>   nprec(a, &alloc);
        ParallelSpmvLeastSquaresOp<crd::f64> serial_op(a, &alloc, /*parallel_min_stored_bytes=*/~crd::usize{0});
        ParallelSpmvLeastSquaresOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, m);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol          = 1e-12;
            opts.max_iter         = 400;
            opts.record_residuals = true;
            LsqrWorkspace<crd::f64> ws(&alloc, m, n);
            return lsqr<crd::f64>(op, &nprec, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
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

TEST_CASE("LSQR is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][lsqr][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32 m = 260;
        const crd::u32 n = 180;
        auto                       a = tall<crd::f64>(&alloc, m, n);
        ParallelSpmvLeastSquaresOp<crd::f64> serial_op(a, &alloc, /*parallel_min_stored_bytes=*/~crd::usize{0});
        ParallelSpmvLeastSquaresOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, m);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol          = 1e-12;
            opts.max_iter         = 400;
            opts.record_residuals = true;
            LsqrWorkspace<crd::f64> ws(&alloc, m, n);
            return lsqr<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
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
