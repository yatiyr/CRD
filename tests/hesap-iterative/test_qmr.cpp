#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/qmr.hpp>
#include <crd/hesap/preconditioners/block_jacobi.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/preconditioners/ssor.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::BlockJacobiPreconditioner;
using crd::hesap::preconditioners::JacobiPreconditioner;
using crd::hesap::preconditioners::SsorPreconditioner;
namespace dense = crd::hesap::dense;

namespace
{
// Nonsymmetric, diagonally-dominant tridiagonal (diag 4, super -1, sub -2):
// asymmetric off-diagonals ⇒ genuinely nonsymmetric, |diag| > sum|offdiag| ⇒ QMR
// converges. Square (n×n).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> nonsym_tridiag(crd::memory::IAllocator* a, crd::u32 n)
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

// Seeded-random nonsymmetric matrix (the "don't test only smooth spectra" trap):
// strong real diagonal (5) + a few random off-diagonals per row in [-0.5,0.5] ⇒
// diagonally dominant and nonsymmetric (independent draws each side).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> random_nonsym(crd::memory::IAllocator* a, crd::u32 n, crd::u64 seed)
{
    crd::u64          state = seed;
    auto              next  = [&state]() -> double {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53); // [0,1)
    };
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        if constexpr (dense::is_complex_v<T>)
        {
            b.add(i, i, T{5.0, 0.5});
        }
        else
        {
            b.add(i, i, T(5));
        }
        for (crd::u32 off = 0; off < 4; ++off)
        {
            const crd::u32 j = static_cast<crd::u32>(next() * n);
            if (j == i) { continue; }
            const double re = next() - 0.5;
            if constexpr (dense::is_complex_v<T>)
            {
                const double im = next() - 0.5;
                b.add(i, j, T{re, im});
            }
            else
            {
                b.add(i, j, static_cast<T>(re));
            }
        }
    }
    return b.compress();
}

// Symmetric, diagonally-dominant tridiagonal (diag 4, off -1 both sides) -- valid
// SSOR target (M is Hermitian ⇒ SSOR's apply_adjoint == apply is exact).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> sym_tridiag(crd::memory::IAllocator* a, crd::u32 n)
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

// TRUE residual ‖b − A·x‖ / ‖b‖ (QMR's reported rNorm is the quasi-residual,
// only an upper bound -- the actual optimality check goes through A directly).
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

TEST_CASE("QMR solves a nonsymmetric system (f64)", "[hesap-iterative][qmr]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    const crd::u32             n = 120;
    auto                       a = nonsym_tridiag<crd::f64>(&alloc, n);
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64>    x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    QmrWorkspace<crd::f64> ws(&alloc, n);

    auto res = qmr<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
}

TEST_CASE("QMR with Jacobi right preconditioner (f64)", "[hesap-iterative][qmr][precond]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    const crd::u32             n = 120;
    auto                       a = nonsym_tridiag<crd::f64>(&alloc, n);
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    JacobiPreconditioner<crd::f64>       m(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64>    x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    QmrWorkspace<crd::f64> ws(&alloc, n);

    auto res = qmr<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
}

TEST_CASE("QMR solves a seeded-random nonsymmetric system (f64)", "[hesap-iterative][qmr]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    const crd::u32             n = 100;
    auto                       a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0x51A7C0DEULL);
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    dense::Vector<crd::f64>    b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64>    x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    QmrWorkspace<crd::f64> ws(&alloc, n);

    auto res = qmr<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-7);
}

TEST_CASE("QMR solves a complex nonsymmetric system (c64)", "[hesap-iterative][qmr][complex]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    using C        = Complex<crd::f64>;
    const crd::u32 n = 100;
    auto           a = random_nonsym<C>(&alloc, n, /*seed=*/0xBADC0FFEEULL);
    ParallelSpmvLeastSquaresOp<C> op(a, &alloc);
    dense::Vector<C> b(&alloc, n);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    QmrWorkspace<C> ws(&alloc, n);

    auto res = qmr<C>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(true_residual<C>(op, x.span(), b.span(), &alloc) < 1e-7);
}

TEST_CASE("QMR is bit-exact over serial vs parallel spmv (determinism moat)",
          "[hesap-iterative][qmr][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             n = 300;
        auto                       a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0xD37E12CULL);
        ParallelSpmvLeastSquaresOp<crd::f64> serial_op(a, &alloc, /*parallel_min_stored_bytes=*/~crd::usize{0});
        ParallelSpmvLeastSquaresOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol          = 1e-12;
            opts.max_iter         = 400;
            opts.record_residuals = true;
            QmrWorkspace<crd::f64> ws(&alloc, n);
            return qmr<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
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

TEST_CASE("QMR with block-Jacobi right preconditioner (f64 + c64)", "[hesap-iterative][qmr][precond]")
{
    crd::memory::TlsfAllocator alloc{32U << 20};
    const crd::u32             n = 100;

    SECTION("real")
    {
        auto                       a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0x11AA22BBULL);
        ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
        BlockJacobiPreconditioner<crd::f64>  m(a, /*block_size=*/5, &alloc);
        dense::Vector<crd::f64>    b(&alloc, n);
        b.fill(1.0);
        dense::Vector<crd::f64>    x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-12;
        opts.max_iter = 500;
        QmrWorkspace<crd::f64> ws(&alloc, n);
        auto res = qmr<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-7);
    }
    SECTION("complex")
    {
        using C = Complex<crd::f64>;
        auto                       a = random_nonsym<C>(&alloc, n, /*seed=*/0x33CC44DDULL);
        ParallelSpmvLeastSquaresOp<C> op(a, &alloc);
        BlockJacobiPreconditioner<C>  m(a, /*block_size=*/5, &alloc);
        dense::Vector<C> b(&alloc, n);
        b.fill(C{1.0, 0.0});
        dense::Vector<C> x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-12;
        opts.max_iter = 500;
        QmrWorkspace<C> ws(&alloc, n);
        auto res = qmr<C>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<C>(op, x.span(), b.span(), &alloc) < 1e-7);
    }
}

TEST_CASE("QMR with SSOR right preconditioner on a symmetric system (f64)", "[hesap-iterative][qmr][precond]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    const crd::u32             n = 120;
    auto                       a = sym_tridiag<crd::f64>(&alloc, n);
    ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc);
    SsorPreconditioner<crd::f64>         m(a, /*omega=*/1.2, &alloc);
    dense::Vector<crd::f64>    b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64>    x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol  = 1e-12;
    opts.max_iter = 500;
    QmrWorkspace<crd::f64> ws(&alloc, n);
    auto res = qmr<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
}

// Direct adjoint identity ⟨y, M x⟩ == ⟨Mᴴ y, x⟩ (= yᴴ M x) for the preconditioners
// QMR newly relies on. Catches a conj/transpose error in apply_adjoint even when a
// solver would still converge. Complex (c64) makes the conjugation observable.
TEST_CASE("Preconditioner apply_adjoint satisfies the adjoint identity (c64)", "[hesap-iterative][precond][adjoint]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    using C        = Complex<crd::f64>;
    const crd::u32 n = 40;
    auto           a = random_nonsym<C>(&alloc, n, /*seed=*/0x9E3779B9ULL);

    // Deterministic non-trivial complex x, y.
    dense::Vector<C> x(&alloc, n), y(&alloc, n), mx(&alloc, n), mhy(&alloc, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x(i) = C{0.5 + 0.1 * static_cast<crd::f64>(i), -0.3 + 0.07 * static_cast<crd::f64>(i)};
        y(i) = C{-0.2 + 0.05 * static_cast<crd::f64>(i), 0.4 - 0.02 * static_cast<crd::f64>(i)};
    }

    auto check = [&](const crd::hesap::LinearOp<C>& m) {
        (void)m.apply(x.span(), mx.span());           // M x
        (void)m.apply_adjoint(y.span(), mhy.span());  // Mᴴ y
        const C lhs = dense::dotc<crd::f64>(y.span(), mx.span());  // ⟨y, M x⟩ = yᴴ M x
        const C rhs = dense::dotc<crd::f64>(mhy.span(), x.span()); // ⟨Mᴴ y, x⟩ = (Mᴴy)ᴴ x = yᴴ M x
        REQUIRE(crd::hesap::abs(lhs - rhs) < 1e-12);
    };

    JacobiPreconditioner<C>      jac(a, &alloc);
    BlockJacobiPreconditioner<C> bj(a, /*block_size=*/4, &alloc);
    check(jac);
    check(bj);
    // SSOR now implements the TRUE adjoint (M_SSOR(A)ᴴ = M_SSOR(Aᴴ)), so the adjoint
    // identity holds even for the NON-Hermitian `a` (not just symmetric matrices).
    SsorPreconditioner<C> ss_nonsym(a, /*omega=*/1.1, &alloc);
    check(ss_nonsym);
    auto                  as = sym_tridiag<C>(&alloc, n);
    SsorPreconditioner<C> ss_herm(as, /*omega=*/1.1, &alloc);
    check(ss_herm);
}
