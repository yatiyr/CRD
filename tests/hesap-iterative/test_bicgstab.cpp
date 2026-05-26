#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
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
template <typename T>
SparseMatrix<T, SparseFormat::Csr> nonsym(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(4));
        if (i + 1 < n) { b.add(i, i + 1, T(-1)); }
        if (i > 0) { b.add(i, i - 1, T(-2)); }
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

TEST_CASE("BiCGSTAB solves a nonsymmetric system (f64)", "[hesap-iterative][bicgstab]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32             n = 80;
    auto                       a = nonsym<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64>   op(a);
    dense::Vector<crd::f64>    b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    BicgstabWorkspace<crd::f64> ws(&alloc, n);

    auto res = bicgstab<crd::f64>(op, nullptr, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-10);
}

TEST_CASE("Jacobi-preconditioned BiCGSTAB solves and matches (f64)", "[hesap-iterative][bicgstab][pcg]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32             n = 80;
    auto                       a = nonsym<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64>   op(a);
    JacobiPreconditioner<crd::f64> m(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, n);
    for (crd::u32 i = 0; i < n; ++i) { b(i) = static_cast<crd::f64>(i + 1); }
    dense::Vector<crd::f64> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    BicgstabWorkspace<crd::f64> ws(&alloc, n);

    auto res = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-10);
}

TEST_CASE("BiCGSTAB solves a nonsymmetric complex system (c64)", "[hesap-iterative][bicgstab][complex]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    using C        = Complex<crd::f64>;
    const crd::u32 n = 40;
    TripletBuilder<C> bld(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bld.add(i, i, C{4.0, 1.0});
        if (i + 1 < n) { bld.add(i, i + 1, C{-1.0, 0.3}); }
        if (i > 0) { bld.add(i, i - 1, C{-2.0, -0.2}); }
    }
    auto             a = bld.compress();
    SparseLinearOp<C> op(a);
    dense::Vector<C> b(&alloc, n);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    BicgstabWorkspace<C> ws(&alloc, n);

    auto res = bicgstab<C>(op, nullptr, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<C>(op, x.span(), b.span(), &alloc) < 1e-9);
}

TEST_CASE("BiCGSTAB is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][bicgstab][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        const crd::u32             n = 220;
        auto                       a = nonsym<crd::f64>(&alloc, n);
        SparseLinearOp<crd::f64>         serial_op(a);
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol          = 1e-12;
            opts.record_residuals = true;
            BicgstabWorkspace<crd::f64> ws(&alloc, n);
            return bicgstab<crd::f64>(op, nullptr, b.span(), x.span(), opts, ws, &alloc);
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
