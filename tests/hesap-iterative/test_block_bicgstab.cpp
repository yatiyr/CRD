#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/block_bicgstab.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
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
// 1D convection-diffusion, diag 4 (well-conditioned nonsymmetric).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> conv_diff(crd::memory::IAllocator* a, crd::u32 n, double gamma)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(4));
        if (i + 1 < n) { b.add(i, i + 1, T(static_cast<crd::f64>(-1.0 + gamma))); }
        if (i > 0) { b.add(i, i - 1, T(static_cast<crd::f64>(-1.0 - gamma))); }
    }
    return b.compress();
}

template <typename T>
SparseMatrix<T, SparseFormat::Csr> conv_diff_c(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T{4.0, 0.5});
        if (i + 1 < n) { b.add(i, i + 1, T{-1.0, 0.2}); }
        if (i > 0) { b.add(i, i - 1, T{-1.3, -0.1}); }
    }
    return b.compress();
}

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
        worst = std::max(worst, std::sqrt(rn) / (std::sqrt(bn) + static_cast<R>(1e-300)));
    }
    return worst;
}

template <typename T>
void fill_independent_rhs(dense::Vector<T>& b, crd::u32 n, crd::u32 s)
{
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 j = 0; j < s; ++j)
        {
            const crd::f64 v = std::sin(0.21 * static_cast<crd::f64>(k) * static_cast<crd::f64>(j + 1) + 0.4 * static_cast<crd::f64>(j))
                               + 0.3 * static_cast<crd::f64>((k + 2 * j) % 5);
            if constexpr (crd::hesap::dense::is_complex_v<T>) { b(k * s + j) = T{v, 0.2 * v}; }
            else { b(k * s + j) = T(v); }
        }
    }
}
} // namespace

TEST_CASE("Block-BiCGSTAB solves a nonsymmetric multi-RHS system + matches per-column convergence",
          "[hesap-iterative][block-bicgstab]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator     alloc{96U << 20};
        const crd::u32                 n = 220;
        const crd::u32                 s = 4;
        auto                           mat = conv_diff<crd::f64>(&alloc, n, 0.4);
        ParallelSpmmLinearOp<crd::f64> bop(mat);
        SparseLinearOp<crd::f64>       sop(mat);

        dense::Vector<crd::f64> b(&alloc, n * s);
        fill_independent_rhs<crd::f64>(b, n, s);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-9;
        opts.max_iter = 1000;

        dense::Vector<crd::f64>           x(&alloc, n * s);
        BlockBicgstabWorkspace<crd::f64>  ws(&alloc, n, s);
        auto                              res = block_bicgstab<crd::f64>(bop, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(block_residual<crd::f64>(bop, x.span(), b.span(), s, &alloc) < 1e-7);

        // Per-column scalar BiCGSTAB converges on the same columns (sanity peer).
        for (crd::u32 j = 0; j < s; ++j)
        {
            dense::Vector<crd::f64> bj(&alloc, n), xj(&alloc, n);
            for (crd::u32 k = 0; k < n; ++k) { bj(k) = b(k * s + j); xj(k) = 0.0; }
            BicgstabWorkspace<crd::f64> wsc(&alloc, n);
            auto                        rc = bicgstab<crd::f64>(sop, nullptr, bj.span(), xj.span(), opts, wsc, &alloc);
            REQUIRE(rc.converged);
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-BiCGSTAB solves a complex nonsymmetric multi-RHS system (c64)",
          "[hesap-iterative][block-bicgstab][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        using C        = Complex<crd::f64>;
        const crd::u32 n = 160;
        const crd::u32 s = 3;
        auto           mat = conv_diff_c<C>(&alloc, n);
        ParallelSpmmLinearOp<C> bop(mat);
        dense::Vector<C>        b(&alloc, n * s);
        fill_independent_rhs<C>(b, n, s);
        dense::Vector<C>           x(&alloc, n * s);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol  = 1e-9;
        opts.max_iter = 1000;
        BlockBicgstabWorkspace<C>  ws(&alloc, n, s);
        auto                       res = block_bicgstab<C>(bop, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(block_residual<C>(bop, x.span(), b.span(), s, &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("Block-BiCGSTAB is bit-exact over serial vs parallel spmm (determinism moat)",
          "[hesap-iterative][block-bicgstab][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator     alloc{96U << 20};
        const crd::u32                 n = 300;
        const crd::u32                 s = 4;
        auto                           mat = conv_diff<crd::f64>(&alloc, n, 0.3);
        ParallelSpmmLinearOp<crd::f64> serial_op(mat, ~crd::usize{0});
        ParallelSpmmLinearOp<crd::f64> parallel_op(mat, 0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n * s);
        fill_independent_rhs<crd::f64>(b, n, s);

        auto solve = [&](const BlockLinearOp<crd::f64>& op, dense::Vector<crd::f64>& x) {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol  = 1e-10;
            opts.max_iter = 1000;
            BlockBicgstabWorkspace<crd::f64> ws(&alloc, n, s);
            return block_bicgstab<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        };
        dense::Vector<crd::f64> xs(&alloc, n * s), xp(&alloc, n * s);
        auto                    rs = solve(serial_op, xs);
        auto                    rp = solve(parallel_op, xp);
        REQUIRE(rs.iterations == rp.iterations);
        for (crd::usize i = 0; i < n * s; ++i) { REQUIRE(xs(i) == xp(i)); }
    }
    crd::jobs::shutdown();
}
