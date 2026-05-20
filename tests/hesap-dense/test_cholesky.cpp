#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"
#include "random_matrix.hpp"

#include <cmath>

using crd::hesap::dense::Cholesky;
using crd::hesap::dense::factor_cholesky;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::solve_cholesky;
using crd::hesap::dense::Symmetric;
using crd_hesap_dense_tests::random_spd;
using Catch::Matchers::WithinAbs;

namespace
{

// Reconstruct A_check = L * L^T from packed cholesky storage and compare
// against the input symmetric A.
template <typename T>
void reconstruct_and_check(const Cholesky<T, Layout::RowMajor>& chol, const Symmetric<T>& a,
                           double tol)
{
    const crd::usize n = chol.n();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            T sum = T{0};
            const crd::usize klimit = (i < j ? i : j) + 1;
            for (crd::usize p = 0; p < klimit; ++p)
            {
                sum += chol.packed().at(i, p) * chol.packed().at(j, p);
            }
            CHECK_THAT(static_cast<double>(sum),
                       WithinAbs(static_cast<double>(a.at(i, j)), tol));
        }
    }
}
} // namespace

TEST_CASE("Cholesky: 2x2 textbook factor", "[hesap][cholesky][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = [[4, 12], [12, 37]]  →  L = [[2, 0], [6, 1]]  (L*L^T = A)
    Symmetric<double> a(&alloc, 2);
    a.at(0, 0) = 4.0;
    a.at(1, 0) = 12.0;
    a.at(1, 1) = 37.0;

    Cholesky<double, Layout::RowMajor> chol(&alloc, 2);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    CHECK_THAT(chol.packed().at(0, 0), WithinAbs(2.0, 1e-12));
    CHECK_THAT(chol.packed().at(1, 0), WithinAbs(6.0, 1e-12));
    CHECK_THAT(chol.packed().at(1, 1), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Cholesky: solve recovers x for A*x = b at N=4",
          "[hesap][cholesky][real][solve]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = diag-dominant SPD: rows mirror via Symmetric storage.
    Symmetric<double> a(&alloc, 4);
    // Lower triangle (incl. diagonal):
    a.at(0, 0) = 25.0;
    a.at(1, 0) =  5.0;  a.at(1, 1) = 18.0;
    a.at(2, 0) =  2.0;  a.at(2, 1) =  3.0;  a.at(2, 2) = 30.0;
    a.at(3, 0) =  1.0;  a.at(3, 1) =  2.0;  a.at(3, 2) =  4.0;  a.at(3, 3) = 40.0;

    // True x = [1, 2, 3, 4], so b = A*x  (use full symmetric access).
    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(4);
    x_true[0] = 1.0; x_true[1] = 2.0; x_true[2] = 3.0; x_true[3] = 4.0;
    crd::containers::Array<double> b(&alloc);
    b.resize(4);
    for (crd::usize i = 0; i < 4; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < 4; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    Cholesky<double, Layout::RowMajor> chol(&alloc, 4);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);

    crd::containers::Span<double> x(b.data(), 4);
    solve_cholesky(chol, x);

    for (crd::usize i = 0; i < 4; ++i)
    {
        CHECK_THAT(x[i], WithinAbs(x_true[i], 1e-12));
    }
}

TEST_CASE("Cholesky: reconstruction L*L^T == A at N=8",
          "[hesap][cholesky][real]")
{
    crd::memory::TlsfAllocator alloc(512U * 1024U);
    constexpr crd::usize kN = 8;
    Symmetric<double> a(&alloc, kN);
    random_spd<double>(a, 42U);
    Cholesky<double, Layout::RowMajor> chol(&alloc, kN);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    reconstruct_and_check<double>(chol, a, 1e-9);
}

TEST_CASE("Cholesky: reconstruction at N=64 (single trailing update)",
          "[hesap][cholesky][real]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize kN = 64;
    Symmetric<double> a(&alloc, kN);
    random_spd<double>(a, 1729U);
    Cholesky<double, Layout::RowMajor> chol(&alloc, kN);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    reconstruct_and_check<double>(chol, a, 1e-7);
}

TEST_CASE("Cholesky: reconstruction at N=128 (multi-block)",
          "[hesap][cholesky][real]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    constexpr crd::usize kN = 128;
    Symmetric<double> a(&alloc, kN);
    random_spd<double>(a, 314159U);
    Cholesky<double, Layout::RowMajor> chol(&alloc, kN);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    reconstruct_and_check<double>(chol, a, 1e-7);
}

TEST_CASE("Cholesky: detects non-positive-definite", "[hesap][cholesky][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // Indefinite matrix: diagonal has a negative entry.
    Symmetric<double> a(&alloc, 3);
    a.at(0, 0) = 4.0;
    a.at(1, 0) = 1.0; a.at(1, 1) = -2.0;
    a.at(2, 0) = 1.0; a.at(2, 1) =  1.0; a.at(2, 2) = 3.0;

    Cholesky<double, Layout::RowMajor> chol(&alloc, 3);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() != 0U);
    REQUIRE(chol.is_singular());
}

TEST_CASE("Cholesky: f32 solve at N=32", "[hesap][cholesky][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize kN = 32;
    Symmetric<float> a(&alloc, kN);
    random_spd<float>(a, 7U);
    crd::containers::Array<float> x_true(&alloc);
    x_true.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x_true[i] = static_cast<float>(i + 1);
    }
    crd::containers::Array<float> b(&alloc);
    b.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        float s = 0.0F;
        for (crd::usize j = 0; j < kN; ++j)
        {
            s += a.at(i, j) * x_true[j];
        }
        b[i] = s;
    }

    Cholesky<float, Layout::RowMajor> chol(&alloc, kN);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    crd::containers::Span<float> x(b.data(), kN);
    solve_cholesky(chol, x);
    for (crd::usize i = 0; i < kN; ++i)
    {
        CHECK_THAT(static_cast<double>(x[i]),
                   WithinAbs(static_cast<double>(x_true[i]), 1e-3));
    }
}

TEST_CASE("Cholesky: multi-RHS solve at N=16, nrhs=3",
          "[hesap][cholesky][real][solve][multi-rhs]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    constexpr crd::usize kN = 16;
    constexpr crd::usize kRhs = 3;
    Symmetric<double> a(&alloc, kN);
    random_spd<double>(a, 999U);
    Matrix<double, Layout::RowMajor> x_true(&alloc, kN, kRhs);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize r = 0; r < kRhs; ++r)
        {
            x_true.at(i, r) = static_cast<double>(i + r * kN);
        }
    }
    Matrix<double, Layout::RowMajor> b(&alloc, kN, kRhs);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize r = 0; r < kRhs; ++r)
        {
            double s = 0.0;
            for (crd::usize j = 0; j < kN; ++j)
            {
                s += a.at(i, j) * x_true.at(j, r);
            }
            b.at(i, r) = s;
        }
    }

    Cholesky<double, Layout::RowMajor> chol(&alloc, kN);
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
    solve_cholesky(chol, b.view());
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize r = 0; r < kRhs; ++r)
        {
            CHECK_THAT(b.at(i, r), WithinAbs(x_true.at(i, r), 1e-9));
        }
    }
}

TEST_CASE("Cholesky: determinism: factor is bit-identical across runs at N=128",
          "[hesap][cholesky][real][determinism]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize kN = 128;
    Symmetric<double> a1(&alloc, kN);
    Symmetric<double> a2(&alloc, kN);
    random_spd<double>(a1, 271828U);
    random_spd<double>(a2, 271828U);

    Cholesky<double, Layout::RowMajor> chol1(&alloc, kN);
    Cholesky<double, Layout::RowMajor> chol2(&alloc, kN);
    factor_cholesky(chol1, a1);
    factor_cholesky(chol2, a2);

    // Only check the LOWER triangle for bit-equality; the upper triangle
    // is GEMM-trailing-update garbage that we don't read on solve.
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            REQUIRE(chol1.packed().at(i, j) == chol2.packed().at(i, j));
        }
    }
}

TEST_CASE("Cholesky: allocator propagation: TLSF only, no malloc",
          "[hesap][cholesky][real][allocator]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    constexpr crd::usize kN = 128;
    Symmetric<double> a(&alloc, kN);
    random_spd<double>(a, 1U);
    Cholesky<double, Layout::RowMajor> chol(&alloc, kN);
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    factor_cholesky(chol, a);
    REQUIRE(chol.info() == 0U);
}
