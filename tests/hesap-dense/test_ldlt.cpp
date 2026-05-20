#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/ldlt.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "random_matrix.hpp"

#include <cmath>

using crd::hesap::dense::factor_ldlt;
using crd::hesap::dense::Layout;
using crd::hesap::dense::LDLT;
using crd::hesap::dense::solve_ldlt;
using crd::hesap::dense::Symmetric;
using crd_hesap_dense_tests::random_symmetric_indefinite;
using Catch::Matchers::WithinAbs;

namespace
{
// Compute A·x manually using Symmetric's full-access at().
template <typename T>
void mat_vec(const Symmetric<T>& a, const T* x, T* y)
{
    const crd::usize n = a.n();
    for (crd::usize i = 0; i < n; ++i)
    {
        T s = T{0};
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a.at(i, j) * x[j];
        }
        y[i] = s;
    }
}
} // namespace

TEST_CASE("LDLT: 2x2 indefinite matrix factor + solve",
          "[hesap][ldlt][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // A = [[1, 2], [2, 1]] — symmetric, indefinite (eigenvalues 3, -1).
    Symmetric<double> a(&alloc, 2);
    a.at(0, 0) = 1.0;
    a.at(1, 0) = 2.0;
    a.at(1, 1) = 1.0;

    // x_true = [1, 2], b = A·x = [1*1+2*2, 2*1+1*2] = [5, 4]
    crd::containers::Array<double> x(&alloc);
    x.resize(2);
    x[0] = 5.0;
    x[1] = 4.0;

    LDLT<double, Layout::RowMajor> ldlt(&alloc, 2);
    factor_ldlt(ldlt, a);
    REQUIRE(ldlt.info() == 0U);

    crd::containers::Span<double> xs(x.data(), 2);
    solve_ldlt(ldlt, xs);
    CHECK_THAT(xs[0], WithinAbs(1.0, 1e-12));
    CHECK_THAT(xs[1], WithinAbs(2.0, 1e-12));
}

TEST_CASE("LDLT: 4x4 symmetric indefinite solve",
          "[hesap][ldlt][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    Symmetric<double> a(&alloc, 4);
    // Indefinite, well-conditioned: lower triangle:
    a.at(0, 0) = -5.0;
    a.at(1, 0) =  1.0;  a.at(1, 1) =  6.0;
    a.at(2, 0) =  0.5;  a.at(2, 1) = -2.0;  a.at(2, 2) =  8.0;
    a.at(3, 0) =  1.0;  a.at(3, 1) =  0.0;  a.at(3, 2) =  3.0;  a.at(3, 3) = -10.0;

    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(4);
    x_true[0] = 1.0; x_true[1] = -2.0; x_true[2] = 3.0; x_true[3] = -4.0;
    crd::containers::Array<double> b(&alloc);
    b.resize(4);
    mat_vec<double>(a, x_true.data(), b.data());

    LDLT<double, Layout::RowMajor> ldlt(&alloc, 4);
    factor_ldlt(ldlt, a);
    REQUIRE(ldlt.info() == 0U);

    crd::containers::Span<double> xs(b.data(), 4);
    solve_ldlt(ldlt, xs);

    for (crd::usize i = 0; i < 4; ++i)
    {
        CHECK_THAT(xs[i], WithinAbs(x_true[i], 1e-10));
    }
}

TEST_CASE("LDLT: solve correctness at N=16 random indefinite",
          "[hesap][ldlt][real]")
{
    crd::memory::TlsfAllocator alloc(512U * 1024U);
    constexpr crd::usize kN = 16;
    Symmetric<double> a(&alloc, kN);
    random_symmetric_indefinite<double>(a, 271U);

    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x_true[i] = static_cast<double>(i + 1) * 0.5 - 4.0;
    }
    crd::containers::Array<double> b(&alloc);
    b.resize(kN);
    mat_vec<double>(a, x_true.data(), b.data());

    LDLT<double, Layout::RowMajor> ldlt(&alloc, kN);
    factor_ldlt(ldlt, a);
    REQUIRE(ldlt.info() == 0U);

    crd::containers::Span<double> xs(b.data(), kN);
    solve_ldlt(ldlt, xs);

    for (crd::usize i = 0; i < kN; ++i)
    {
        CHECK_THAT(xs[i], WithinAbs(x_true[i], 1e-8));
    }
}

TEST_CASE("LDLT: solve correctness at N=64 random indefinite",
          "[hesap][ldlt][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(4U * 1024U * 1024U));
    constexpr crd::usize kN = 64;
    Symmetric<double> a(&alloc, kN);
    random_symmetric_indefinite<double>(a, 3141U);

    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x_true[i] = std::sin(static_cast<double>(i) * 0.3);
    }
    crd::containers::Array<double> b(&alloc);
    b.resize(kN);
    mat_vec<double>(a, x_true.data(), b.data());

    LDLT<double, Layout::RowMajor> ldlt(&alloc, kN);
    factor_ldlt(ldlt, a);
    REQUIRE(ldlt.info() == 0U);

    crd::containers::Span<double> xs(b.data(), kN);
    solve_ldlt(ldlt, xs);

    for (crd::usize i = 0; i < kN; ++i)
    {
        CHECK_THAT(xs[i], WithinAbs(x_true[i], 1e-6));
    }
}

TEST_CASE("LDLT: f32 solve at N=32",
          "[hesap][ldlt][real][f32]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(2U * 1024U * 1024U));
    constexpr crd::usize kN = 32;
    Symmetric<float> a(&alloc, kN);
    random_symmetric_indefinite<float>(a, 2718U);

    crd::containers::Array<float> x_true(&alloc);
    x_true.resize(kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x_true[i] = static_cast<float>(i + 1) * 0.25F;
    }
    crd::containers::Array<float> b(&alloc);
    b.resize(kN);
    mat_vec<float>(a, x_true.data(), b.data());

    LDLT<float, Layout::RowMajor> ldlt(&alloc, kN);
    factor_ldlt(ldlt, a);
    REQUIRE(ldlt.info() == 0U);

    crd::containers::Span<float> xs(b.data(), kN);
    solve_ldlt(ldlt, xs);
    for (crd::usize i = 0; i < kN; ++i)
    {
        CHECK_THAT(static_cast<double>(xs[i]),
                   WithinAbs(static_cast<double>(x_true[i]), 1e-2));
    }
}

TEST_CASE("LDLT: detects singular matrix",
          "[hesap][ldlt][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // Zero matrix is symmetric (trivially) and singular.
    Symmetric<double> a(&alloc, 3);
    a.at(0, 0) = 0.0;
    a.at(1, 0) = 0.0; a.at(1, 1) = 0.0;
    a.at(2, 0) = 0.0; a.at(2, 1) = 0.0; a.at(2, 2) = 0.0;

    LDLT<double, Layout::RowMajor> ldlt(&alloc, 3);
    factor_ldlt(ldlt, a);
    REQUIRE(ldlt.info() != 0U);
    REQUIRE(ldlt.is_singular());
}

TEST_CASE("LDLT: handles SPD matrix correctly (only 1x1 pivots)",
          "[hesap][ldlt][real]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    // SPD 3x3: A = [[4, 1, 1], [1, 5, 2], [1, 2, 6]]
    Symmetric<double> a(&alloc, 3);
    a.at(0, 0) = 4.0;
    a.at(1, 0) = 1.0; a.at(1, 1) = 5.0;
    a.at(2, 0) = 1.0; a.at(2, 1) = 2.0; a.at(2, 2) = 6.0;

    crd::containers::Array<double> x_true(&alloc);
    x_true.resize(3);
    x_true[0] = 1.0; x_true[1] = 2.0; x_true[2] = 3.0;
    crd::containers::Array<double> b(&alloc);
    b.resize(3);
    mat_vec<double>(a, x_true.data(), b.data());

    LDLT<double, Layout::RowMajor> ldlt(&alloc, 3);
    factor_ldlt(ldlt, a);
    REQUIRE(ldlt.info() == 0U);
    // SPD → expect all 1x1 pivots.
    REQUIRE(ldlt.block_kind(0) == 1U);
    REQUIRE(ldlt.block_kind(1) == 1U);
    REQUIRE(ldlt.block_kind(2) == 1U);

    crd::containers::Span<double> xs(b.data(), 3);
    solve_ldlt(ldlt, xs);
    for (crd::usize i = 0; i < 3; ++i)
    {
        CHECK_THAT(xs[i], WithinAbs(x_true[i], 1e-12));
    }
}
