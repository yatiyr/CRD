#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using namespace crd::hesap::preconditioners;
using crd::hesap::Complex;
namespace dense = crd::hesap::dense;

namespace
{
// Nonsymmetric, diagonally dominant tridiagonal: diag 4, super -1, sub -2.
template <typename T> SparseMatrix<T, SparseFormat::Csr> nonsym_tridiag(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(4));
        if (i + 1 < n)
        {
            b.add(i, i + 1, T(-1));
        }
        if (i > 0)
        {
            b.add(i, i - 1, T(-2));
        }
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
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        diff(i) = ax(i) - b[i];
    }
    return dense::nrm2<T>(diff.span()) / dense::nrm2<T>(b);
}

// Preconditioner whose action CHANGES every iteration (Jacobi on even calls,
// identity on odd) -- exercises FGMRES's flexible Z-based update, which plain
// preconditioned GMRES cannot represent.
template <typename T> class AlternatingPrecond final : public crd::hesap::LinearOp<T>
{
public:
    AlternatingPrecond(const SparseMatrix<T, SparseFormat::Csr>& a, crd::memory::IAllocator* alloc)
        : crd::hesap::LinearOp<T>(false, false), m_jac(a, alloc), m_n(a.rows())
    {
    }
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        if (m_count++ % 2 == 0)
        {
            return m_jac.apply(x, y);
        }
        for (crd::usize i = 0; i < x.size(); ++i) // identity
        {
            y[i] = x[i];
        }
        return true;
    }
    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    JacobiPreconditioner<T> m_jac;
    mutable int m_count = 0;
    crd::u32 m_n;
};
} // namespace

TEST_CASE("GMRES solves a nonsymmetric system (f64)", "[hesap-iterative][gmres]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32 n = 80;
    auto a = nonsym_tridiag<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64> op(a);
    dense::Vector<crd::f64> b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    GmresWorkspace<crd::f64> ws(&alloc, n, /*restart=*/30);

    auto res = gmres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-9);
}

TEST_CASE("GMRES restart length does not change the solution (f64)", "[hesap-iterative][gmres][restart]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    const crd::u32 n = 80;
    auto a = nonsym_tridiag<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64> op(a);
    dense::Vector<crd::f64> b(&alloc, n);
    b.fill(1.0);

    auto solve = [&](crd::usize restart, dense::Vector<crd::f64>& x)
    {
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-12;
        opts.max_iter = 2000;
        GmresWorkspace<crd::f64> ws(&alloc, n, restart);
        return gmres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    };
    dense::Vector<crd::f64> x_small(&alloc, n);
    dense::Vector<crd::f64> x_big(&alloc, n);
    REQUIRE(solve(10, x_small).converged);
    REQUIRE(solve(200, x_big).converged); // effectively non-restarted (n=80)
    for (crd::u32 i = 0; i < n; ++i)
    {
        REQUIRE(std::abs(x_small(i) - x_big(i)) < 1e-6);
    }
}

TEST_CASE("FGMRES converges with a per-iteration VARYING preconditioner (f64)", "[hesap-iterative][fgmres][flexible]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32 n = 80;
    auto a = nonsym_tridiag<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64> op(a);
    AlternatingPrecond<crd::f64> m(a, &alloc); // M changes every iteration
    dense::Vector<crd::f64> b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-10;
    GmresWorkspace<crd::f64> ws(&alloc, n, 30);

    auto res = fgmres<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
}

TEST_CASE("GMRES solves a nonsymmetric complex system (c64)", "[hesap-iterative][gmres][complex]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    using C = Complex<crd::f64>;
    const crd::u32 n = 40;
    // diag (4,1), super (-1,0.3), sub (-2,-0.2) -> nonsym, non-Hermitian, diag-dominant.
    TripletBuilder<C> bld(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bld.add(i, i, C{4.0, 1.0});
        if (i + 1 < n)
        {
            bld.add(i, i + 1, C{-1.0, 0.3});
        }
        if (i > 0)
        {
            bld.add(i, i - 1, C{-2.0, -0.2});
        }
    }
    auto a = bld.compress();
    SparseLinearOp<C> op(a);
    dense::Vector<C> b(&alloc, n);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    GmresWorkspace<C> ws(&alloc, n, 40);

    auto res = gmres<C>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<C>(op, x.span(), b.span(), &alloc) < 1e-9);
}

TEST_CASE("GMRES is bit-exact over serial vs parallel spmv (determinism moat)", "[hesap-iterative][gmres][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        const crd::u32 n = 220;
        auto a = nonsym_tridiag<crd::f64>(&alloc, n);
        SparseLinearOp<crd::f64> serial_op(a);
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE(parallel_op.is_parallel());

        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-12;
            opts.record_residuals = true;
            GmresWorkspace<crd::f64> ws(&alloc, n, 30);
            return gmres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
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
