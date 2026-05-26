#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/minres.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using namespace crd::hesap::preconditioners;
using crd::hesap::Complex;
namespace dense = crd::hesap::dense;

namespace
{
// Symmetric 1D Laplacian with a diagonal SHIFT. shift=0 -> SPD (spectrum (0,4));
// shift=2 -> INDEFINITE (spectrum (-2,2), ~half negative) where CG would diverge.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> shifted_laplacian(crd::memory::IAllocator* a, crd::u32 n, double shift)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, static_cast<T>(2.0 - shift));
        if (i + 1 < n) { b.add(i, i + 1, T(-1)); }
        if (i > 0) { b.add(i, i - 1, T(-1)); }
    }
    return b.compress();
}

template <typename T>
crd::hesap::dense::RealType<T> rel_residual(const crd::hesap::LinearOp<T>& op, crd::containers::ConstSpan<T> x,
                                            crd::containers::ConstSpan<T> b, crd::memory::IAllocator* a)
{
    dense::Vector<T> ax(a, x.size());
    (void)op.apply(x, ax.span());
    dense::Vector<T> diff(a, x.size());
    for (crd::usize i = 0; i < x.size(); ++i) { diff(i) = ax(i) - b[i]; }
    return dense::nrm2<T>(diff.span()) / dense::nrm2<T>(b);
}
} // namespace

TEST_CASE("MINRES solves an SPD system (f64)", "[hesap-iterative][minres]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32             n = 80;
    auto                       a = shifted_laplacian<crd::f64>(&alloc, n, 0.0); // SPD
    SparseLinearOp<crd::f64>   op(a);
    dense::Vector<crd::f64>    b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    MinresWorkspace<crd::f64> ws(&alloc, n);

    auto res = minres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-9);
}

TEST_CASE("MINRES solves a symmetric INDEFINITE system where CG would diverge (f64)", "[hesap-iterative][minres][indefinite]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32             n = 80;
    auto                       a = shifted_laplacian<crd::f64>(&alloc, n, 2.0); // spectrum (-2,2): indefinite
    SparseLinearOp<crd::f64>   op(a);
    dense::Vector<crd::f64>    b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-10;
    opts.max_iter = 500;
    MinresWorkspace<crd::f64> ws(&alloc, n);

    // CG (Lanczos with the CG short recurrence) is undefined on an indefinite A;
    // MINRES minimizes the residual and converges. This is the regime it owns.
    auto res = minres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
}

TEST_CASE("Jacobi-preconditioned MINRES solves an SPD system (f64)", "[hesap-iterative][minres][precond]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32             n = 80;
    auto                       a = shifted_laplacian<crd::f64>(&alloc, n, 0.0); // SPD -> Jacobi is SPD
    SparseLinearOp<crd::f64>   op(a);
    JacobiPreconditioner<crd::f64> m(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, n);
    for (crd::u32 i = 0; i < n; ++i) { b(i) = static_cast<crd::f64>(i + 1); }
    dense::Vector<crd::f64> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    MinresWorkspace<crd::f64> ws(&alloc, n);

    auto res = minres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-9);
}

TEST_CASE("MINRES solves a Hermitian indefinite system (c64)", "[hesap-iterative][minres][complex]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    using C        = Complex<crd::f64>;
    const crd::u32 n = 40;
    // Hermitian indefinite: real diagonal 0, Hermitian off-diagonal (1, 0.5)/(1,-0.5).
    TripletBuilder<C> bld(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bld.add(i, i, C{0.0, 0.0});
        if (i + 1 < n)
        {
            bld.add(i, i + 1, C{1.0, 0.5});
            bld.add(i + 1, i, C{1.0, -0.5}); // conjugate -> Hermitian
        }
    }
    auto             a = bld.compress();
    SparseLinearOp<C> op(a);
    dense::Vector<C> b(&alloc, n);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-10;
    opts.max_iter = 500;
    MinresWorkspace<C> ws(&alloc, n);

    auto res = minres<C>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<C>(op, x.span(), b.span(), &alloc) < 1e-8);
}

TEST_CASE("MINRES is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][minres][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        const crd::u32             n = 220;
        auto                       a = shifted_laplacian<crd::f64>(&alloc, n, 2.0);
        SparseLinearOp<crd::f64>         serial_op(a);
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol          = 1e-10;
            opts.max_iter         = 600;
            opts.record_residuals = true;
            MinresWorkspace<crd::f64> ws(&alloc, n);
            return minres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
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
