#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::dense::Banded;
using crd::hesap::dense::gbmv;
using crd::hesap::dense::sbmv;
using crd::hesap::dense::tbmv;
using crd::hesap::dense::tbsv;
using crd::hesap::dense::Trans;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;
using crd::hesap::dense::Vector;
using Catch::Matchers::WithinAbs;

TEST_CASE("gbmv: tridiagonal A*x", "[hesap][blas2][banded][gbmv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Banded<crd::f64> a(&alloc, 4, 4, 1, 1);
    // A = tridiag(-1, 2, -1)  (Poisson 1D 4-point stencil)
    for (crd::usize i = 0; i < 4; ++i)
    {
        a.at(i, i) = 2.0;
        if (i > 0) a.at(i, i - 1) = -1.0;
        if (i + 1 < 4) a.at(i, i + 1) = -1.0;
    }
    Vector<crd::f64> x(&alloc, {1.0, 1.0, 1.0, 1.0});
    Vector<crd::f64> y(&alloc, 4);
    gbmv<crd::f64>(1.0, a, x.span(), 0.0, y.span(), Trans::None);
    // y[0] = 2*1 + (-1)*1 = 1
    // y[1] = -1 + 2 + -1 = 0
    // y[2] = -1 + 2 + -1 = 0
    // y[3] = -1 + 2*1 = 1
    REQUIRE(y(0) == 1.0);
    REQUIRE(y(1) == 0.0);
    REQUIRE(y(2) == 0.0);
    REQUIRE(y(3) == 1.0);
}

TEST_CASE("sbmv: symmetric banded matches symv on a tridiag", "[hesap][blas2][banded][sbmv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Banded<crd::f64> a(&alloc, 3, 3, 1, 1);
    a.at(0, 0) = 2.0;
    a.at(1, 0) = 1.0;
    a.at(0, 1) = 1.0;  // symmetric
    a.at(1, 1) = 2.0;
    a.at(2, 1) = 1.0;
    a.at(1, 2) = 1.0;
    a.at(2, 2) = 2.0;
    Vector<crd::f64> x(&alloc, {1.0, 1.0, 1.0});
    Vector<crd::f64> y(&alloc, 3);
    sbmv<crd::f64>(1.0, a, x.span(), 0.0, y.span());
    REQUIRE(y(0) == 3.0);  // 2 + 1
    REQUIRE(y(1) == 4.0);  // 1 + 2 + 1
    REQUIRE(y(2) == 3.0);  // 1 + 2
}

TEST_CASE("tbmv Lower: L*x with bidiagonal storage", "[hesap][blas2][banded][tbmv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    // Bidiagonal lower (kl=1, ku=0): main diag + 1 sub-diag.
    Banded<crd::f64> l(&alloc, 3, 3, 1, 0);
    l.at(0, 0) = 1.0;
    l.at(1, 0) = 2.0;
    l.at(1, 1) = 3.0;
    l.at(2, 1) = 4.0;
    l.at(2, 2) = 5.0;
    Vector<crd::f64> x(&alloc, {1.0, 1.0, 1.0});
    tbmv<crd::f64>(l, TriangularSide::Lower, TriangularDiag::Explicit, x.span(), Trans::None);
    // x[0] = 1*1 = 1
    // x[1] = 2*1 + 3*1 = 5
    // x[2] = 4*1 + 5*1 = 9
    REQUIRE(x(0) == 1.0);
    REQUIRE(x(1) == 5.0);
    REQUIRE(x(2) == 9.0);
}

TEST_CASE("tbsv Lower: solve L*x = b", "[hesap][blas2][banded][tbsv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Banded<crd::f64> l(&alloc, 3, 3, 1, 0);
    l.at(0, 0) = 1.0;
    l.at(1, 0) = 2.0;
    l.at(1, 1) = 3.0;
    l.at(2, 1) = 4.0;
    l.at(2, 2) = 5.0;
    // L * [1, 1, 1] = [1, 5, 9]; solve L*x = [1, 5, 9] -> [1, 1, 1].
    Vector<crd::f64> b(&alloc, {1.0, 5.0, 9.0});
    tbsv<crd::f64>(l, TriangularSide::Lower, TriangularDiag::Explicit, b.span(), Trans::None);
    REQUIRE_THAT(b(0), WithinAbs(1.0, 1e-14));
    REQUIRE_THAT(b(1), WithinAbs(1.0, 1e-14));
    REQUIRE_THAT(b(2), WithinAbs(1.0, 1e-14));
}

TEST_CASE("tbmv + tbsv round-trip Upper bidiagonal", "[hesap][blas2][banded][tbmv][tbsv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Banded<crd::f64> u(&alloc, 4, 4, 0, 1);  // upper bidiagonal: ku=1, kl=0
    u.at(0, 0) = 1.0;
    u.at(0, 1) = 0.5;
    u.at(1, 1) = 2.0;
    u.at(1, 2) = 0.5;
    u.at(2, 2) = 3.0;
    u.at(2, 3) = 0.5;
    u.at(3, 3) = 4.0;
    Vector<crd::f64> x0(&alloc, {1.0, -2.0, 3.0, -4.0});
    auto x = x0.clone();
    tbmv<crd::f64>(u, TriangularSide::Upper, TriangularDiag::Explicit, x.span(), Trans::None);
    tbsv<crd::f64>(u, TriangularSide::Upper, TriangularDiag::Explicit, x.span(), Trans::None);
    for (crd::usize i = 0; i < 4; ++i)
    {
        REQUIRE_THAT(x(i), WithinAbs(x0(i), 1e-12));
    }
}
