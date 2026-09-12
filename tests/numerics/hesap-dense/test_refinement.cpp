#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/refinement.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"

#include <cmath>

using crd::hesap::dense::Cholesky;
using crd::hesap::dense::factor_cholesky;
using crd::hesap::dense::factor_lu;
using crd::hesap::dense::Layout;
using crd::hesap::dense::LU;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::refine_cholesky;
using crd::hesap::dense::refine_lu;
using crd::hesap::dense::RefinementResult;
using crd::hesap::dense::solve_cholesky;
using crd::hesap::dense::solve_lu;
using crd::hesap::dense::Symmetric;
using Catch::Matchers::WithinAbs;

TEST_CASE("refine_lu: residual decreases for well-conditioned A",
          "[hesap][refinement][lu]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(2U * 1024U * 1024U));
    constexpr crd::usize k_n = 16;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j < k_n; ++j)
        {
            a.at(i, j) = std::sin(static_cast<double>(i * 5 + j) * 0.1) +
                         (i == j ? 10.0 : 0.0);
        }
    }
    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        x_true[i] = static_cast<double>(i + 1);
    }
    crd::containers::Array<double> b(&alloc);
    b.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    LU<double, Layout::RowMajor> lu(&alloc, k_n);
    factor_lu(lu, a);

    crd::containers::Array<double> x(&alloc);
    x.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        x[i] = b[i];
    }
    crd::containers::Span<double> xs(x.data(), k_n);
    solve_lu(lu, xs);  // Initial solve.

    RefinementResult<double> result = refine_lu<double, Layout::RowMajor>(
        a, lu, crd::containers::ConstSpan<double>(b.data(), k_n), xs, 5, 1e-14);

    // Initial residual already small for well-conditioned, but refinement
    // shouldn't make it worse. Final residual must be ≤ initial.
    REQUIRE(result.final_rel_residual <= result.initial_rel_residual + 1e-15);
    // Solution still recovers x_true (within tight tolerance).
    for (crd::usize i = 0; i < k_n; ++i)
    {
        CHECK_THAT(x[i], WithinAbs(x_true[i], 1e-10));
    }
}

TEST_CASE("refine_lu: trivial b=0 returns immediately",
          "[hesap][refinement][lu]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    constexpr crd::usize k_n = 3;
    Matrix<double, Layout::RowMajor> a(&alloc, k_n, k_n,
        {2.0, 1.0, 0.0, 1.0, 3.0, 1.0, 0.0, 1.0, 4.0});
    LU<double, Layout::RowMajor> lu(&alloc, k_n);
    factor_lu(lu, a);

    crd::containers::Array<double> b(&alloc);
    b.resize(k_n);
    b[0] = 0.0; b[1] = 0.0; b[2] = 0.0;
    crd::containers::Array<double> x(&alloc);
    x.resize(k_n);
    x[0] = 0.0; x[1] = 0.0; x[2] = 0.0;

    RefinementResult<double> result = refine_lu<double, Layout::RowMajor>(
        a, lu, crd::containers::ConstSpan<double>(b.data(), k_n),
        crd::containers::Span<double>(x.data(), k_n));
    REQUIRE(result.converged);
    REQUIRE(result.iterations == 0U);
}

TEST_CASE("refine_cholesky: residual decreases for SPD",
          "[hesap][refinement][cholesky]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize k_n = 12;
    Symmetric<double> a(&alloc, k_n);
    // Build SPD via A = B^T B + n·I.
    for (crd::usize i = 0; i < k_n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            double s = 0.0;
            for (crd::usize p = 0; p < k_n; ++p)
            {
                s += std::sin(static_cast<double>(p * k_n + i) * 0.07) *
                     std::sin(static_cast<double>(p * k_n + j) * 0.07);
            }
            if (i == j)
            {
                s += static_cast<double>(k_n);
            }
            a.at(i, j) = s;
        }
    }

    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        x_true[i] = static_cast<double>(i + 1) * 0.5;
    }
    crd::containers::Array<double> b(&alloc);
    b.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < k_n; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    Cholesky<double, Layout::RowMajor> chol(&alloc, k_n);
    factor_cholesky(chol, a);

    crd::containers::Array<double> x(&alloc);
    x.resize(k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        x[i] = b[i];
    }
    crd::containers::Span<double> xs(x.data(), k_n);
    solve_cholesky(chol, xs);

    RefinementResult<double> result = refine_cholesky<double, Layout::RowMajor>(
        a, chol, crd::containers::ConstSpan<double>(b.data(), k_n), xs, 5, 1e-14);
    REQUIRE(result.final_rel_residual <= result.initial_rel_residual + 1e-15);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        CHECK_THAT(x[i], WithinAbs(x_true[i], 1e-10));
    }
}

TEST_CASE("refine: converges immediately if initial solve is already exact",
          "[hesap][refinement]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    Matrix<double, Layout::RowMajor> a(&alloc, 2, 2, {2.0, 0.0, 0.0, 3.0});
    LU<double, Layout::RowMajor> lu(&alloc, 2);
    factor_lu(lu, a);
    crd::containers::Array<double> b(&alloc);
    b.resize(2);
    b[0] = 4.0; b[1] = 9.0;  // True x = [2, 3]
    crd::containers::Array<double> x(&alloc);
    x.resize(2);
    x[0] = b[0]; x[1] = b[1];
    solve_lu(lu, crd::containers::Span<double>(x.data(), 2));

    RefinementResult<double> result = refine_lu<double, Layout::RowMajor>(
        a, lu, crd::containers::ConstSpan<double>(b.data(), 2),
        crd::containers::Span<double>(x.data(), 2));
    REQUIRE(result.converged);
    CHECK_THAT(x[0], WithinAbs(2.0, 1e-14));
    CHECK_THAT(x[1], WithinAbs(3.0, 1e-14));
}
