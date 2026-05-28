#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/block_cg.hpp> // detail::block_qr (the QR primitive under test)
#include <crd/hesap/iterative/block_gmres.hpp>
#include <crd/hesap/iterative/gmres.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
namespace dense = crd::hesap::dense;

namespace
{
// 1D convection-diffusion: diag 4, super (-1+gamma), sub (-1-gamma). NONSYMMETRIC for
// gamma != 0, well-conditioned (diagonally dominant ⇒ no restarted-GMRES stagnation, so
// the per-column scalar-GMRES baseline converges too). With FULL-RANK (independent) RHS
// columns it still needs several block steps ⇒ exercises the banded block-Hessenberg
// Givens over multiple steps (not just an m=1 happy path).
template <typename T> SparseMatrix<T, SparseFormat::Csr> conv_diff(crd::memory::IAllocator* a, crd::u32 n, double gamma)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(4));
        if (i + 1 < n)
        {
            b.add(i, i + 1, T(static_cast<crd::f64>(-1.0 + gamma)));
        }
        if (i > 0)
        {
            b.add(i, i - 1, T(static_cast<crd::f64>(-1.0 - gamma)));
        }
    }
    return b.compress();
}

template <typename T> SparseMatrix<T, SparseFormat::Csr> conv_diff_c(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T{4.0, 0.5});
        if (i + 1 < n)
        {
            b.add(i, i + 1, T{-1.0, 0.2});
        }
        if (i > 0)
        {
            b.add(i, i - 1, T{-1.3, -0.1});
        }
    }
    return b.compress();
}

template <typename T>
crd::hesap::dense::RealType<T> block_residual(const BlockLinearOp<T>& op, crd::containers::ConstSpan<T> x,
                                              crd::containers::ConstSpan<T> b, crd::u32 s, crd::memory::IAllocator* a)
{
    using R = crd::hesap::dense::RealType<T>;
    const crd::usize n = op.n_rows();
    dense::Vector<T> ax(a, n * s);
    (void)op.apply_block(x, s, ax.span(), s, s);
    auto mag = [](T v) -> R
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>)
        {
            return std::sqrt(v.re * v.re + v.im * v.im);
        }
        else
        {
            return v < R(0) ? -v : v;
        }
    };
    R worst = R(0);
    for (crd::u32 j = 0; j < s; ++j)
    {
        R rn = R(0);
        R bn = R(0);
        for (crd::usize k = 0; k < n; ++k)
        {
            const T dd = b[k * s + j] - ax(k * s + j);
            rn += mag(dd) * mag(dd);
            bn += mag(b[k * s + j]) * mag(b[k * s + j]);
        }
        worst = std::max(worst, std::sqrt(rn) / (std::sqrt(bn) + static_cast<R>(1e-300)));
    }
    return worst;
}
} // namespace

TEST_CASE("block_qr reconstructs W = Q*R (the foundational block-Arnoldi primitive)",
          "[hesap-iterative][block-gmres][block-qr]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    const crd::usize n = 130;
    const crd::u32 s = 5;
    // Deterministic pseudo-random full-rank n×s block (row-major).
    dense::Vector<crd::f64> w0(&alloc, n * s);
    dense::Vector<crd::f64> w(&alloc, n * s);
    dense::Vector<crd::f64> cm(&alloc, n * s);
    crd::containers::Array<crd::f64> r(&alloc);
    r.resize(static_cast<crd::usize>(s) * s);
    crd::u32 seed = 12345U;
    auto rng = [&]()
    {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<crd::f64>(seed >> 8) / 16777216.0 - 0.5;
    };
    for (crd::usize i = 0; i < n * s; ++i)
    {
        w0(i) = rng();
        w(i) = w0(i);
    }

    crd::hesap::iterative::detail::block_qr<crd::f64>(w.data(), n, s, cm.data(), r.data());

    // Q (in W) is orthonormal: QᴴQ = I.
    for (crd::u32 i = 0; i < s; ++i)
    {
        for (crd::u32 j = 0; j < s; ++j)
        {
            crd::f64 dot = 0.0;
            for (crd::usize k = 0; k < n; ++k)
            {
                dot += w(k * s + i) * w(k * s + j);
            }
            REQUIRE(std::abs(dot - (i == j ? 1.0 : 0.0)) < 1e-10);
        }
    }
    // W0 = Q·R reconstructs the original block to ~eps.
    crd::f64 err = 0.0;
    for (crd::usize k = 0; k < n; ++k)
    {
        for (crd::u32 j = 0; j < s; ++j)
        {
            crd::f64 acc = 0.0;
            for (crd::u32 i = 0; i < s; ++i)
            {
                acc += w(k * s + i) * r[static_cast<crd::usize>(i) * s + j];
            }
            const crd::f64 d = acc - w0(k * s + j);
            err += d * d;
        }
    }
    REQUIRE(std::sqrt(err) < 1e-10);
}

TEST_CASE("Block-GMRES solves a nonsymmetric multi-RHS system + shares the Krylov space",
          "[hesap-iterative][block-gmres]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{160U << 20};
        const crd::u32 n = 200;
        const crd::u32 s = 4;
        auto mat = conv_diff<crd::f64>(&alloc, n, 0.4);
        ParallelSpmmLinearOp<crd::f64> bop(mat);
        SparseLinearOp<crd::f64> sop(mat); // single-vector op for per-column comparison

        dense::Vector<crd::f64> b(&alloc, n * s);
        for (crd::u32 k = 0; k < n; ++k)
        {
            // Genuinely INDEPENDENT columns (distinct frequency/phase per RHS), so the
            // block is full rank — not the degenerate {base, ones} span.
            for (crd::u32 j = 0; j < s; ++j)
            {
                b(k * s + j) = std::sin(0.21 * static_cast<crd::f64>(k) * static_cast<crd::f64>(j + 1) +
                                        0.4 * static_cast<crd::f64>(j)) +
                               0.3 * static_cast<crd::f64>((k + 2 * j) % 5);
            }
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 400;

        // Block-GMRES (all s RHS, shared block Krylov space). Large restart so the
        // comparison to per-column GMRES is apples-to-apples (no restart stagnation on
        // either side — restarted GMRES(m) is known to stagnate on conv-diff).
        const crd::usize restart = 50; // well-conditioned ⇒ both converge well within one cycle
        dense::Vector<crd::f64> x(&alloc, n * s);
        BlockGmresWorkspace<crd::f64> ws(&alloc, n, s, restart);
        auto res = block_gmres<crd::f64>(bop, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations >= 3); // multiple block steps ⇒ exercises the banded Givens (not an m=1 happy path)
        REQUIRE(block_residual<crd::f64>(bop, x.span(), b.span(), s, &alloc) < 1e-7);

        // Per-column scalar GMRES with the SAME RHS columns; block steps ≤ Σ column iters.
        crd::usize col_iter_sum = 0;
        for (crd::u32 j = 0; j < s; ++j)
        {
            dense::Vector<crd::f64> bj(&alloc, n);
            dense::Vector<crd::f64> xj(&alloc, n);
            for (crd::u32 k = 0; k < n; ++k)
            {
                bj(k) = b(k * s + j);
                xj(k) = 0.0;
            }
            GmresWorkspace<crd::f64> wsc(&alloc, n, restart);
            auto rc = gmres<crd::f64>(sop, bj.span(), xj.span(), opts, wsc, &alloc);
            REQUIRE(rc.converged);
            col_iter_sum += rc.iterations;
        }
        REQUIRE(res.iterations <= col_iter_sum); // shared Krylov space ⇒ no worse than summed independent work
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-GMRES solves a complex nonsymmetric multi-RHS system (c64)", "[hesap-iterative][block-gmres][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 160;
        const crd::u32 s = 3;
        auto mat = conv_diff_c<C>(&alloc, n);
        ParallelSpmmLinearOp<C> bop(mat);
        dense::Vector<C> b(&alloc, n * s);
        for (crd::u32 k = 0; k < n; ++k)
        {
            for (crd::u32 j = 0; j < s; ++j)
            {
                b(k * s + j) = C{1.0 + 0.1 * static_cast<crd::f64>(j), 0.05 * static_cast<crd::f64>(j)};
            }
        }
        dense::Vector<C> x(&alloc, n * s);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-9;
        opts.max_iter = 400;
        BlockGmresWorkspace<C> ws(&alloc, n, s, 50);
        auto res = block_gmres<C>(bop, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(block_residual<C>(bop, x.span(), b.span(), s, &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-GMRES is bit-exact over serial vs parallel spmm (determinism moat)",
          "[hesap-iterative][block-gmres][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{128U << 20};
        const crd::u32 n = 400;
        const crd::u32 s = 4;
        auto mat = conv_diff<crd::f64>(&alloc, n, 0.3);
        ParallelSpmmLinearOp<crd::f64> serial_op(mat, ~crd::usize{0});
        ParallelSpmmLinearOp<crd::f64> parallel_op(mat, 0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n * s);
        for (crd::usize i = 0; i < n * s; ++i)
        {
            b(i) = 1.0 + 0.001 * static_cast<crd::f64>(i % 11);
        }

        auto solve = [&](const BlockLinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-10;
            opts.max_iter = 400;
            BlockGmresWorkspace<crd::f64> ws(&alloc, n, s, 50);
            return block_gmres<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n * s);
        dense::Vector<crd::f64> xp(&alloc, n * s);
        auto rs = solve(serial_op, xs);
        auto rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::usize i = 0; i < n * s; ++i)
        {
            REQUIRE(xs(i) == xp(i));
        }
    }
    crd::jobs::shutdown();
}
