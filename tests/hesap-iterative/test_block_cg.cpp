#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/block_cg.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/preconditioners/block_jacobi.hpp>
#include <crd/hesap/preconditioners/block_preconditioner.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
namespace dense = crd::hesap::dense;

namespace
{
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

// Complex Hermitian positive-definite tridiagonal (real positive diagonal,
// conjugate off-diagonals, diagonally dominant ⇒ HPD).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> hpd_tridiag(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T{4.0, 0.0});
        if (i + 1 < n)
        {
            b.add(i, i + 1, T{-1.0, 0.3});
            b.add(i + 1, i, T{-1.0, -0.3});
        }
    }
    return b.compress();
}

// SPD tridiagonal with a STRONGLY varying diagonal (d_i ∈ [4, 28], off-diag -1, so
// still diagonally dominant ⇒ SPD). The varying scale is what point-Jacobi corrects,
// so block-PCG converges in meaningfully fewer iterations than plain block-CG here.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> spd_varying_diag(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(4) + T(6) * T(static_cast<crd::f64>(i % 5)));
        if (i + 1 < n) { b.add(i, i + 1, T(-1)); }
        if (i > 0) { b.add(i, i - 1, T(-1)); }
    }
    return b.compress();
}

// 1D Dirichlet Laplacian (diag 2, off-diag -1): SPD but ILL-CONDITIONED
// (cond ≈ (2n/π)² — e.g. ~2.6e5 at n=800). The discriminating case for the
// breakdown-free orthonormalization: plain D-BCG (no per-step QR) STALLS here from
// loss of search-block conjugacy; the orthonormal-search variant converges.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> laplacian_1d(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(2));
        if (i + 1 < n) { b.add(i, i + 1, T(-1)); }
        if (i > 0) { b.add(i, i - 1, T(-1)); }
    }
    return b.compress();
}

// max over columns of ‖(B - A·X)·,j‖ / ‖B·,j‖ (blocks n×s row-major).
template <typename T>
crd::hesap::dense::RealType<T> block_residual(const BlockLinearOp<T>& op, crd::containers::ConstSpan<T> x,
                                              crd::containers::ConstSpan<T> b, crd::u32 s, crd::memory::IAllocator* a)
{
    using R          = crd::hesap::dense::RealType<T>;
    const crd::usize n = op.n_rows();
    dense::Vector<T> ax(a, n * s);
    (void)op.apply_block(x, s, ax.span(), s, s);
    auto mag = [](T v) -> R {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
        else { return v < R(0) ? -v : v; }
    };
    R worst = R(0);
    for (crd::u32 j = 0; j < s; ++j)
    {
        R rn = R(0), bn = R(0);
        for (crd::usize k = 0; k < n; ++k)
        {
            const T dd = b[k * s + j] - ax(k * s + j);
            rn += mag(dd) * mag(dd);
            bn += mag(b[k * s + j]) * mag(b[k * s + j]);
        }
        const R rel = std::sqrt(rn) / (std::sqrt(bn) + static_cast<R>(1e-300));
        worst       = rel > worst ? rel : worst;
    }
    return worst;
}
} // namespace

TEST_CASE("Block-CG solves a multi-RHS SPD system (f64, s=4)", "[hesap-iterative][block-cg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             n = 200;
        const crd::u32             s = 4;
        auto                       a = spd_tridiag<crd::f64>(&alloc, n);
        ParallelSpmmLinearOp<crd::f64> op(a);
        // B: distinct columns.
        dense::Vector<crd::f64> b(&alloc, n * s);
        for (crd::u32 k = 0; k < n; ++k)
        {
            for (crd::u32 j = 0; j < s; ++j) { b(k * s + j) = 1.0 + 0.1 * static_cast<crd::f64>(j) + 0.01 * static_cast<crd::f64>(k % 7); }
        }
        dense::Vector<crd::f64> x(&alloc, n * s);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 2000;
        BlockCgWorkspace<crd::f64> ws(&alloc, n, s);

        auto res = block_cg<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(block_residual<crd::f64>(op, x.span(), b.span(), s, &alloc) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-CG is breakdown-free on a rank-deficient block (duplicate RHS columns)",
          "[hesap-iterative][block-cg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        const crd::u32             n = 150;
        const crd::u32             s = 3;
        auto                       a = spd_tridiag<crd::f64>(&alloc, n);
        ParallelSpmmLinearOp<crd::f64> op(a);
        // Columns 0 and 2 IDENTICAL ⇒ the residual block is rank-deficient throughout;
        // plain block-CG breaks (singular PᴴAP). The regularized-Cholesky fallback keeps it going.
        dense::Vector<crd::f64> b(&alloc, n * s);
        for (crd::u32 k = 0; k < n; ++k)
        {
            b(k * s + 0) = 1.0;
            b(k * s + 1) = 2.0 + 0.01 * static_cast<crd::f64>(k % 5);
            b(k * s + 2) = 1.0; // duplicate of column 0
        }
        dense::Vector<crd::f64> x(&alloc, n * s);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 2000;
        BlockCgWorkspace<crd::f64> ws(&alloc, n, s);

        auto res = block_cg<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(block_residual<crd::f64>(op, x.span(), b.span(), s, &alloc) < 1e-8);
        // The two identical RHS must yield identical solution columns.
        for (crd::u32 k = 0; k < n; ++k)
        {
            REQUIRE(std::abs(x(k * s + 0) - x(k * s + 2)) < 1e-9);
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-CG solves a multi-RHS complex HPD system (c64, s=4)", "[hesap-iterative][block-cg][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        using C        = Complex<crd::f64>;
        const crd::u32 n = 160;
        const crd::u32 s = 4;
        auto           a = hpd_tridiag<C>(&alloc, n);
        ParallelSpmmLinearOp<C> op(a);
        dense::Vector<C> b(&alloc, n * s);
        for (crd::u32 k = 0; k < n; ++k)
        {
            for (crd::u32 j = 0; j < s; ++j) { b(k * s + j) = C{1.0 + 0.1 * static_cast<crd::f64>(j), 0.05 * static_cast<crd::f64>(j)}; }
        }
        dense::Vector<C> x(&alloc, n * s);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 2000;
        BlockCgWorkspace<C> ws(&alloc, n, s);

        auto res = block_cg<C>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(block_residual<C>(op, x.span(), b.span(), s, &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-CG is bit-exact over serial vs parallel spmm (determinism moat)",
          "[hesap-iterative][block-cg][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32             n = 400;
        const crd::u32             s = 4;
        auto                       a = spd_tridiag<crd::f64>(&alloc, n);
        ParallelSpmmLinearOp<crd::f64> serial_op(a, ~crd::usize{0});
        ParallelSpmmLinearOp<crd::f64> parallel_op(a, 0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n * s);
        for (crd::usize i = 0; i < n * s; ++i) { b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11); }

        auto solve = [&](const BlockLinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol  = 1e-12;
            opts.max_iter = 1000;
            BlockCgWorkspace<crd::f64> ws(&alloc, n, s);
            return block_cg<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n * s);
        dense::Vector<crd::f64> xp(&alloc, n * s);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::usize i = 0; i < n * s; ++i) { REQUIRE(xs(i) == xp(i)); }

        // The PRECONDITIONED path must hold the moat too (the shipping block-PCG path —
        // the unpreconditioned solve above never touches the preconditioner apply).
        crd::hesap::preconditioners::JacobiBlockPreconditioner<crd::f64> jac(a, &alloc);
        auto solve_pc = [&](const BlockLinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol  = 1e-12;
            opts.max_iter = 1000;
            BlockCgWorkspace<crd::f64> ws(&alloc, n, s);
            return block_pcg<crd::f64>(op, jac, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xsp(&alloc, n * s);
        dense::Vector<crd::f64> xpp(&alloc, n * s);
        auto                    rsp = solve_pc(serial_op, xsp);
        auto                    rpp = solve_pc(parallel_op, xpp);
        REQUIRE(rsp.iterations == rpp.iterations);
        for (crd::usize i = 0; i < n * s; ++i) { REQUIRE(xsp(i) == xpp(i)); }
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-CG converges on an ill-conditioned SPD (1D Laplacian) -- breakdown-free orthonormalization",
          "[hesap-iterative][block-cg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator     alloc{128U << 20};
        const crd::u32                 n = 800; // cond ≈ 2.6e5
        const crd::u32                 s = 8;
        auto                           a = laplacian_1d<crd::f64>(&alloc, n);
        ParallelSpmmLinearOp<crd::f64> op(a);
        dense::Vector<crd::f64>        b(&alloc, n * s);
        for (crd::u32 k = 0; k < n; ++k)
        {
            for (crd::u32 j = 0; j < s; ++j) { b(k * s + j) = 1.0 + 0.1 * static_cast<crd::f64>(j) + 0.01 * static_cast<crd::f64>(k % 7); }
        }
        dense::Vector<crd::f64>    x(&alloc, n * s);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-9;
        opts.max_iter = 4000;
        BlockCgWorkspace<crd::f64> ws(&alloc, n, s);

        auto res = block_cg<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged); // plain D-BCG (no QR) stalls here ⇒ this is the regression guard
        REQUIRE(block_residual<crd::f64>(op, x.span(), b.span(), s, &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-PCG with Jacobi block preconditioner converges and cuts iterations",
          "[hesap-iterative][block-cg][block-pcg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator     alloc{64U << 20};
        const crd::u32                 n = 300;
        const crd::u32                 s = 4;
        auto                           a = spd_varying_diag<crd::f64>(&alloc, n);
        ParallelSpmmLinearOp<crd::f64> op(a);
        crd::hesap::preconditioners::JacobiBlockPreconditioner<crd::f64> jac(a, &alloc);

        dense::Vector<crd::f64> b(&alloc, n * s);
        for (crd::u32 k = 0; k < n; ++k)
        {
            for (crd::u32 j = 0; j < s; ++j) { b(k * s + j) = 1.0 + 0.1 * static_cast<crd::f64>(j) + 0.01 * static_cast<crd::f64>(k % 7); }
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-10;
        opts.max_iter = 4000;

        // Plain block-CG for the iteration baseline.
        dense::Vector<crd::f64>    x_cg(&alloc, n * s);
        BlockCgWorkspace<crd::f64> ws_cg(&alloc, n, s);
        auto                       r_cg = block_cg<crd::f64>(op, b.span(), x_cg.span(), opts, ws_cg, &alloc);
        REQUIRE(r_cg.converged);

        // Block-PCG with the same operator + Jacobi.
        dense::Vector<crd::f64>    x_pcg(&alloc, n * s);
        BlockCgWorkspace<crd::f64> ws_pcg(&alloc, n, s);
        auto                       r_pcg = block_pcg<crd::f64>(op, jac, b.span(), x_pcg.span(), opts, ws_pcg, &alloc);
        REQUIRE(r_pcg.converged);
        REQUIRE(block_residual<crd::f64>(op, x_pcg.span(), b.span(), s, &alloc) < 1e-8);
        // Jacobi corrects the strongly varying diagonal ⇒ strictly fewer iterations.
        REQUIRE(r_pcg.iterations < r_cg.iterations);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-PCG: native Jacobi block-precond == adapter-wrapped point-Jacobi (bit-exact)",
          "[hesap-iterative][block-cg][block-pcg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator     alloc{96U << 20};
        const crd::u32                 n = 220;
        const crd::u32                 s = 3;
        auto                           a = spd_varying_diag<crd::f64>(&alloc, n);
        ParallelSpmmLinearOp<crd::f64> op(a);
        // Native one-pass diagonal block preconditioner.
        crd::hesap::preconditioners::JacobiBlockPreconditioner<crd::f64> native(a, &alloc);
        // Same math via the general adapter wrapping a block-size-1 (= point) block-Jacobi LinearOp.
        crd::hesap::preconditioners::BlockJacobiPreconditioner<crd::f64> point(a, 1, &alloc);
        crd::hesap::preconditioners::BlockPreconditionerAdapter<crd::f64> adapted(point, &alloc);

        dense::Vector<crd::f64> b(&alloc, n * s);
        for (crd::usize i = 0; i < n * s; ++i) { b(i) = 1.0 + 0.003 * static_cast<crd::f64>(i % 13); }

        auto solve = [&](const BlockLinearOp<crd::f64>& m, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol  = 1e-11;
            opts.max_iter = 4000;
            BlockCgWorkspace<crd::f64> ws(&alloc, n, s);
            return block_pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> x_native(&alloc, n * s);
        dense::Vector<crd::f64> x_adapted(&alloc, n * s);
        auto                    r_native  = solve(native, x_native);
        auto                    r_adapted = solve(adapted, x_adapted);
        REQUIRE(r_native.converged);
        REQUIRE(r_adapted.converged);
        REQUIRE(r_native.iterations == r_adapted.iterations);
        for (crd::usize i = 0; i < n * s; ++i) { REQUIRE(x_native(i) == x_adapted(i)); }
    }
    crd::jobs::shutdown();
}
