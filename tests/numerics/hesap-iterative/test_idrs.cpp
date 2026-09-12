#include <crd/containers/span.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/idrs.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::preconditioners::JacobiPreconditioner;
namespace dense = crd::hesap::dense;

namespace
{
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
SparseMatrix<T, SparseFormat::Csr> random_nonsym(crd::memory::IAllocator* a, crd::u32 n, crd::u64 seed)
{
    crd::u64 state = seed;
    auto next = [&state]() -> double
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
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
            if (j == i)
            {
                continue;
            }
            const double re = next() - 0.5;
            if constexpr (dense::is_complex_v<T>)
            {
                b.add(i, j, T{re, next() - 0.5});
            }
            else
            {
                b.add(i, j, static_cast<T>(re));
            }
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

TEST_CASE("idrs_ltri_solve solves the lower-triangular trailing block", "[hesap-iterative][idrs][unit]")
{
    // M (row-major 3x3, only lower used): [[2,0,0],[1,3,0],[1,1,4]].
    const crd::f64 m[9] = {2, 0, 0, 1, 3, 0, 1, 1, 4};
    const crd::f64 f[3] = {2, 5, 7};
    crd::f64 c[3] = {0, 0, 0};

    crd::hesap::iterative::detail::idrs_ltri_solve<crd::f64>(m, 3, /*k=*/0, f, c);
    REQUIRE(c[0] == 1.0);                        // 2/2
    REQUIRE(std::abs(c[1] - 4.0 / 3.0) < 1e-15); // (5-1)/3
    REQUIRE(std::abs(c[2] - 7.0 / 6.0) < 1e-15); // (7-1-4/3)/4

    // Trailing 2x2 block (k=1): [[3,0],[1,4]] · c = f[1:3] = [5,7].
    crd::f64 c2[3] = {0, 0, 0};
    crd::hesap::iterative::detail::idrs_ltri_solve<crd::f64>(m, 3, /*k=*/1, f, c2);
    REQUIRE(std::abs(c2[0] - 5.0 / 3.0) < 1e-15);
    REQUIRE(std::abs(c2[1] - 4.0 / 3.0) < 1e-15); // (7-5/3)/4
}

TEST_CASE("IDR(s) solves a nonsymmetric system (f64)", "[hesap-iterative][idrs]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        const crd::u32 n = 120;
        auto a = nonsym_tridiag<crd::f64>(&alloc, n);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);
        dense::Vector<crd::f64> x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-12;
        opts.max_iter = 2000;
        IdrsWorkspace<crd::f64> ws(&alloc, n, /*s=*/4);

        auto res = idrs<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-8);
    }
    crd::jobs::shutdown();
}

TEST_CASE("IDR(s) solves a seeded-random nonsymmetric system, s in {1,2,8} (f64)", "[hesap-iterative][idrs]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{48U << 20};
        const crd::u32 n = 100;
        auto a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0x1D125ULL);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);
        for (const crd::usize s : {crd::usize{1}, crd::usize{2}, crd::usize{8}})
        {
            dense::Vector<crd::f64> x(&alloc, n);
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-12;
            opts.max_iter = 3000;
            IdrsWorkspace<crd::f64> ws(&alloc, n, s);
            auto res = idrs<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(res.converged);
            REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-7);
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("IDR(s) with Jacobi left preconditioner (f64)", "[hesap-iterative][idrs][precond]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        const crd::u32 n = 120;
        auto a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0xABCDEFULL);
        ParallelSparseLinearOp<crd::f64> op(a, &alloc);
        JacobiPreconditioner<crd::f64> m(a, &alloc);
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);
        dense::Vector<crd::f64> x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-12;
        opts.max_iter = 2000;
        IdrsWorkspace<crd::f64> ws(&alloc, n, /*s=*/4);

        auto res = idrs<crd::f64>(op, &m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("IDR(s) solves a complex nonsymmetric system (c64)", "[hesap-iterative][idrs][complex]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{32U << 20};
        using C = Complex<crd::f64>;
        const crd::u32 n = 100;
        auto a = random_nonsym<C>(&alloc, n, /*seed=*/0xC0FFEEULL);
        ParallelSparseLinearOp<C> op(a, &alloc);
        dense::Vector<C> b(&alloc, n);
        b.fill(C{1.0, 0.0});
        dense::Vector<C> x(&alloc, n);
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-12;
        opts.max_iter = 3000;
        IdrsWorkspace<C> ws(&alloc, n, /*s=*/4);

        auto res = idrs<C>(op, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(true_residual<C>(op, x.span(), b.span(), &alloc) < 1e-7);
    }
    crd::jobs::shutdown();
}

TEST_CASE("IDR(s) is bit-exact over serial vs parallel spmv (determinism moat)", "[hesap-iterative][idrs][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{96U << 20};
        const crd::u32 n = 300;
        auto a = random_nonsym<crd::f64>(&alloc, n, /*seed=*/0x5EED5EEDULL);
        ParallelSparseLinearOp<crd::f64> serial_op(a, &alloc, /*parallel_min_stored_bytes=*/~crd::usize{0});
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE_FALSE(serial_op.is_parallel());
        REQUIRE(parallel_op.is_parallel());
        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-12;
            opts.max_iter = 1500;
            opts.record_residuals = true;
            // Two workspaces, same default seed + n + s ⇒ identical shadow space P.
            IdrsWorkspace<crd::f64> ws(&alloc, n, /*s=*/4);
            return idrs<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
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
