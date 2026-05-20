#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/condition.hpp>
#include <crd/hesap/dense/linear_op_dense.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::hesap::dense::Cholesky;
using crd::hesap::dense::compute_1norm;
using crd::hesap::dense::condition_estimate_1norm_symmetric;
using crd::hesap::dense::factor_cholesky;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::MatrixLinearOp;
using crd::hesap::dense::Symmetric;
using crd::hesap::dense::SymmetricLinearOp;
using Catch::Matchers::WithinAbs;

TEST_CASE("MatrixLinearOp: apply matches gemv", "[hesap][linear_op][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    Matrix<double, Layout::RowMajor> a(&alloc, 3, 3, {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0});
    MatrixLinearOp<double, Layout::RowMajor> op(a);
    REQUIRE(op.n_rows() == 3U);
    REQUIRE(op.n_cols() == 3U);
    REQUIRE(op.is_square());
    REQUIRE(op.has_transpose());

    crd::containers::Array<double> x(&alloc);
    x.resize(3);
    x[0] = 1.0; x[1] = 2.0; x[2] = 3.0;
    crd::containers::Array<double> y(&alloc);
    y.resize(3);
    REQUIRE(op.apply(crd::containers::ConstSpan<double>(x.data(), 3),
                     crd::containers::Span<double>(y.data(), 3)));
    // y[0] = 1*1 + 2*2 + 3*3 = 14
    // y[1] = 4*1 + 5*2 + 6*3 = 32
    // y[2] = 7*1 + 8*2 + 9*3 = 50
    CHECK_THAT(y[0], WithinAbs(14.0, 1e-12));
    CHECK_THAT(y[1], WithinAbs(32.0, 1e-12));
    CHECK_THAT(y[2], WithinAbs(50.0, 1e-12));
}

TEST_CASE("MatrixLinearOp: apply_transpose matches gemv-trans",
          "[hesap][linear_op][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    Matrix<double, Layout::RowMajor> a(&alloc, 3, 3, {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0});
    MatrixLinearOp<double, Layout::RowMajor> op(a);

    crd::containers::Array<double> x(&alloc);
    x.resize(3);
    x[0] = 1.0; x[1] = 2.0; x[2] = 3.0;
    crd::containers::Array<double> y(&alloc);
    y.resize(3);
    REQUIRE(op.apply_transpose(crd::containers::ConstSpan<double>(x.data(), 3),
                                crd::containers::Span<double>(y.data(), 3)));
    // y = A^T · x. A^T = [[1,4,7],[2,5,8],[3,6,9]]
    // y[0] = 1*1 + 4*2 + 7*3 = 30
    // y[1] = 2*1 + 5*2 + 8*3 = 36
    // y[2] = 3*1 + 6*2 + 9*3 = 42
    CHECK_THAT(y[0], WithinAbs(30.0, 1e-12));
    CHECK_THAT(y[1], WithinAbs(36.0, 1e-12));
    CHECK_THAT(y[2], WithinAbs(42.0, 1e-12));
}

TEST_CASE("SymmetricLinearOp: apply matches symv", "[hesap][linear_op][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    Symmetric<double> a(&alloc, 3);
    // Lower triangle of symmetric A = [[4, 1, 2], [1, 5, 3], [2, 3, 6]]
    a.at(0, 0) = 4.0;
    a.at(1, 0) = 1.0; a.at(1, 1) = 5.0;
    a.at(2, 0) = 2.0; a.at(2, 1) = 3.0; a.at(2, 2) = 6.0;

    SymmetricLinearOp<double> op(a);
    REQUIRE(op.n_rows() == 3U);
    REQUIRE(op.n_cols() == 3U);

    crd::containers::Array<double> x(&alloc);
    x.resize(3);
    x[0] = 1.0; x[1] = 2.0; x[2] = 3.0;
    crd::containers::Array<double> y(&alloc);
    y.resize(3);
    REQUIRE(op.apply(crd::containers::ConstSpan<double>(x.data(), 3),
                     crd::containers::Span<double>(y.data(), 3)));
    // y[0] = 4*1 + 1*2 + 2*3 = 12
    // y[1] = 1*1 + 5*2 + 3*3 = 20
    // y[2] = 2*1 + 3*2 + 6*3 = 26
    CHECK_THAT(y[0], WithinAbs(12.0, 1e-12));
    CHECK_THAT(y[1], WithinAbs(20.0, 1e-12));
    CHECK_THAT(y[2], WithinAbs(26.0, 1e-12));
}

TEST_CASE("compute_1norm: 3x3 matrix textbook", "[hesap][linear_op][norm]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = [[1, -2, 3], [4, 5, -6], [-7, 8, 9]]
    // Column sums of abs: |1|+|4|+|7| = 12; |2|+|5|+|8| = 15; |3|+|6|+|9| = 18.
    // ||A||_1 = max = 18.
    Matrix<double, Layout::RowMajor> a(&alloc, 3, 3, {1.0, -2.0, 3.0, 4.0, 5.0, -6.0, -7.0, 8.0, 9.0});
    CHECK_THAT(compute_1norm(a), WithinAbs(18.0, 1e-12));
}

TEST_CASE("compute_1norm: Symmetric 3x3", "[hesap][linear_op][norm]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    Symmetric<double> a(&alloc, 3);
    a.at(0, 0) = 5.0;
    a.at(1, 0) = -2.0; a.at(1, 1) = 7.0;
    a.at(2, 0) = 1.0;  a.at(2, 1) = -3.0; a.at(2, 2) = 4.0;
    // Row 0: |5| + |-2| + |1| = 8
    // Row 1: |-2| + |7| + |-3| = 12
    // Row 2: |1| + |-3| + |4| = 8
    // ||A||_1 = 12.
    CHECK_THAT(compute_1norm(a), WithinAbs(12.0, 1e-12));
}

TEST_CASE("condition_estimate: SPD 3x3 Cholesky factor",
          "[hesap][condition][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = [[4, 1, 0], [1, 4, 1], [0, 1, 4]]  (tridiagonal SPD).
    Symmetric<double> a(&alloc, 3);
    a.at(0, 0) = 4.0;
    a.at(1, 0) = 1.0; a.at(1, 1) = 4.0;
    a.at(2, 0) = 0.0; a.at(2, 1) = 1.0; a.at(2, 2) = 4.0;

    Cholesky<double, Layout::RowMajor> chol(&alloc, 3);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);

    const double kappa = condition_estimate_1norm_symmetric(a, chol, &alloc);
    // True κ_1 of this tridiag is moderate (eigenvalues are ~2.59, 4.0, 5.41 — well-conditioned).
    // The estimate must be >= 1 (κ ≥ 1 always) and finite.
    REQUIRE(kappa >= 1.0);
    REQUIRE(kappa < 100.0);  // well-conditioned matrix
}

TEST_CASE("condition_estimate: identity matrix yields kappa = 1",
          "[hesap][condition][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    constexpr crd::usize kN = 5;
    Symmetric<double> a(&alloc, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        a.at(i, i) = 1.0;
    }

    Cholesky<double, Layout::RowMajor> chol(&alloc, kN);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);

    const double kappa = condition_estimate_1norm_symmetric(a, chol, &alloc);
    CHECK_THAT(kappa, WithinAbs(1.0, 1e-10));
}

TEST_CASE("condition_estimate: scaled identity yields kappa = 1",
          "[hesap][condition][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    constexpr crd::usize kN = 4;
    Symmetric<double> a(&alloc, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        a.at(i, i) = 7.5;
    }

    Cholesky<double, Layout::RowMajor> chol(&alloc, kN);
    factor_cholesky(chol, a);
    const double kappa = condition_estimate_1norm_symmetric(a, chol, &alloc);
    CHECK_THAT(kappa, WithinAbs(1.0, 1e-10));
}
