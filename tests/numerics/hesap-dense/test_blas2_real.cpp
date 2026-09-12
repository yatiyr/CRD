#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::dense::gemv;
using crd::hesap::dense::ger;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Symmetric;
using crd::hesap::dense::symv;
using crd::hesap::dense::syr;
using crd::hesap::dense::syr2;
using crd::hesap::dense::Trans;
using crd::hesap::dense::Triangular;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;
using crd::hesap::dense::trmv;
using crd::hesap::dense::trsv;
using crd::hesap::dense::Vector;
using Catch::Matchers::WithinAbs;

TEST_CASE("gemv: y = alpha*A*x + beta*y on 2x3 (RowMajor)", "[hesap][blas2][real][gemv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 2, 3, {1, 2, 3, 4, 5, 6});
    Vector<crd::f64> x(&alloc, {1.0, 1.0, 1.0});
    Vector<crd::f64> y(&alloc, {10.0, 20.0});
    gemv<crd::f64>(2.0, a.cview(), x.span(), 3.0, y.span(), Trans::None);
    // y[0] = 2*(1+2+3) + 3*10 = 12 + 30 = 42
    // y[1] = 2*(4+5+6) + 3*20 = 30 + 60 = 90
    REQUIRE(y(0) == 42.0);
    REQUIRE(y(1) == 90.0);
}

TEST_CASE("gemv Trans equals naive A^T * x", "[hesap][blas2][real][gemv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 2, 3, {1, 2, 3, 4, 5, 6});
    Vector<crd::f64> x(&alloc, {1.0, 2.0});
    Vector<crd::f64> y(&alloc, {0.0, 0.0, 0.0});
    gemv<crd::f64>(1.0, a.cview(), x.span(), 0.0, y.span(), Trans::Transpose);
    // y[0] = 1*1 + 4*2 = 9
    // y[1] = 2*1 + 5*2 = 12
    // y[2] = 3*1 + 6*2 = 15
    REQUIRE(y(0) == 9.0);
    REQUIRE(y(1) == 12.0);
    REQUIRE(y(2) == 15.0);
}

TEST_CASE("gemv ColMajor matches RowMajor on same logical data", "[hesap][blas2][real][gemv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    // Same logical matrix [[1,2,3],[4,5,6]] in both layouts.
    Matrix<crd::f64, Layout::RowMajor> ar(&alloc, 2, 3, {1, 2, 3, 4, 5, 6});
    // Col-major raw bytes for the same logical matrix: col0=[1,4], col1=[2,5], col2=[3,6]
    Matrix<crd::f64, Layout::ColMajor> ac(&alloc, 2, 3, {1, 4, 2, 5, 3, 6});
    Vector<crd::f64> x(&alloc, {1.0, 1.0, 1.0});
    Vector<crd::f64> yr(&alloc, 2);
    Vector<crd::f64> yc(&alloc, 2);
    gemv<crd::f64, Layout::RowMajor>(1.0, ar.cview(), x.span(), 0.0, yr.span());
    gemv<crd::f64, Layout::ColMajor>(1.0, ac.cview(), x.span(), 0.0, yc.span());
    REQUIRE(yr(0) == yc(0));
    REQUIRE(yr(1) == yc(1));
}

TEST_CASE("ger: A += alpha * x * y^T", "[hesap][blas2][real][ger]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<crd::f64> a(&alloc, 2, 3);  // zero
    Vector<crd::f64> x(&alloc, {2.0, 3.0});
    Vector<crd::f64> y(&alloc, {1.0, 4.0, -1.0});
    ger<crd::f64>(1.0, x.span(), y.span(), a.view());
    // A_ij = x_i * y_j
    REQUIRE(a(0, 0) == 2.0);
    REQUIRE(a(0, 1) == 8.0);
    REQUIRE(a(0, 2) == -2.0);
    REQUIRE(a(1, 0) == 3.0);
    REQUIRE(a(1, 1) == 12.0);
    REQUIRE(a(1, 2) == -3.0);
}

TEST_CASE("symv: A * x with symmetric A", "[hesap][blas2][real][symv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Symmetric<crd::f64> a(&alloc, 3);
    // A = [[2, 1, 0], [1, 2, 1], [0, 1, 2]]  (tridiag symmetric)
    a.at(0, 0) = 2.0;
    a.at(1, 0) = 1.0;
    a.at(1, 1) = 2.0;
    a.at(2, 1) = 1.0;
    a.at(2, 2) = 2.0;
    Vector<crd::f64> x(&alloc, {1.0, 1.0, 1.0});
    Vector<crd::f64> y(&alloc, 3);
    symv<crd::f64>(1.0, a, x.span(), 0.0, y.span());
    REQUIRE(y(0) == 3.0);
    REQUIRE(y(1) == 4.0);
    REQUIRE(y(2) == 3.0);
}

TEST_CASE("syr: A += alpha * x * x^T", "[hesap][blas2][real][syr]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Symmetric<crd::f64> a(&alloc, 2);
    Vector<crd::f64> x(&alloc, {1.0, 2.0});
    syr<crd::f64>(1.0, x.span(), a);
    REQUIRE(a.at(0, 0) == 1.0);
    REQUIRE(a.at(1, 0) == 2.0);
    REQUIRE(a.at(1, 1) == 4.0);
    REQUIRE(a.at(0, 1) == 2.0);  // symmetric mirror
}

TEST_CASE("syr2: A += alpha*(x*y^T + y*x^T)", "[hesap][blas2][real][syr2]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Symmetric<crd::f64> a(&alloc, 2);
    Vector<crd::f64> x(&alloc, {1.0, 0.0});
    Vector<crd::f64> y(&alloc, {0.0, 1.0});
    syr2<crd::f64>(1.0, x.span(), y.span(), a);
    // alpha*(x*y^T + y*x^T) for these x,y = [[0,1],[1,0]]
    REQUIRE(a.at(0, 0) == 0.0);
    REQUIRE(a.at(1, 0) == 1.0);
    REQUIRE(a.at(1, 1) == 0.0);
}

TEST_CASE("trmv Lower: x = L*x", "[hesap][blas2][real][trmv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> l(&alloc, 3);
    l.at(0, 0) = 1.0;
    l.at(1, 0) = 2.0;
    l.at(1, 1) = 3.0;
    l.at(2, 0) = 4.0;
    l.at(2, 1) = 5.0;
    l.at(2, 2) = 6.0;
    Vector<crd::f64> x(&alloc, {1.0, 1.0, 1.0});
    trmv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(l, x.span(), Trans::None);
    // L*x: [1*1, 2*1+3*1, 4*1+5*1+6*1] = [1, 5, 15]
    REQUIRE(x(0) == 1.0);
    REQUIRE(x(1) == 5.0);
    REQUIRE(x(2) == 15.0);
}

TEST_CASE("trsv Lower: solve L*x = b", "[hesap][blas2][real][trsv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> l(&alloc, 3);
    l.at(0, 0) = 1.0;
    l.at(1, 0) = 2.0;
    l.at(1, 1) = 3.0;
    l.at(2, 0) = 4.0;
    l.at(2, 1) = 5.0;
    l.at(2, 2) = 6.0;
    // x = [1, 1, 1] -> L*x = [1, 5, 15]. Solve L*x = [1, 5, 15] -> should recover x = [1, 1, 1].
    Vector<crd::f64> b(&alloc, {1.0, 5.0, 15.0});
    trsv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(l, b.span(), Trans::None);
    REQUIRE_THAT(b(0), WithinAbs(1.0, 1e-14));
    REQUIRE_THAT(b(1), WithinAbs(1.0, 1e-14));
    REQUIRE_THAT(b(2), WithinAbs(1.0, 1e-14));
}

TEST_CASE("trsv Upper: solve U*x = b", "[hesap][blas2][real][trsv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit> u(&alloc, 3);
    u.at(0, 0) = 1.0;
    u.at(0, 1) = 2.0;
    u.at(0, 2) = 3.0;
    u.at(1, 1) = 4.0;
    u.at(1, 2) = 5.0;
    u.at(2, 2) = 6.0;
    // x = [1, 1, 1] -> U*x = [6, 9, 6]
    Vector<crd::f64> b(&alloc, {6.0, 9.0, 6.0});
    trsv<crd::f64, TriangularSide::Upper, TriangularDiag::Explicit>(u, b.span(), Trans::None);
    REQUIRE_THAT(b(0), WithinAbs(1.0, 1e-14));
    REQUIRE_THAT(b(1), WithinAbs(1.0, 1e-14));
    REQUIRE_THAT(b(2), WithinAbs(1.0, 1e-14));
}

TEST_CASE("trmv + trsv round-trip on random Lower", "[hesap][blas2][real][trmv][trsv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit> l(&alloc, 5);
    // Construct a well-conditioned L (diagonal dominant).
    for (crd::usize i = 0; i < 5; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            l.at(i, j) = (i == j) ? 4.0 : 0.5;
        }
    }
    Vector<crd::f64> x0(&alloc, {1.0, -2.0, 3.0, -4.0, 5.0});
    auto x = x0.clone();
    trmv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(l, x.span(), Trans::None);
    trsv<crd::f64, TriangularSide::Lower, TriangularDiag::Explicit>(l, x.span(), Trans::None);
    for (crd::usize i = 0; i < 5; ++i)
    {
        REQUIRE_THAT(x(i), WithinAbs(x0(i), 1e-12));
    }
}

TEST_CASE("gemv larger N matches naive triple-loop", "[hesap][blas2][real][gemv]")
{
    crd::memory::TlsfAllocator alloc(2 * 1024 * 1024);
    constexpr crd::usize m = 50;
    constexpr crd::usize n = 30;
    Matrix<crd::f64> a(&alloc, m, n);
    Vector<crd::f64> x(&alloc, n);
    Vector<crd::f64> y_engine(&alloc, m);
    Vector<crd::f64> y_naive(&alloc, m);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a(i, j) = static_cast<crd::f64>(i + 1) / static_cast<crd::f64>(j + 2);
        }
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        x(j) = static_cast<crd::f64>(j) - 5.0;
    }
    gemv<crd::f64>(2.0, a.cview(), x.span(), 0.0, y_engine.span(), Trans::None);
    for (crd::usize i = 0; i < m; ++i)
    {
        crd::f64 s = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a(i, j) * x(j);
        }
        y_naive(i) = 2.0 * s;
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        REQUIRE_THAT(y_engine(i), WithinAbs(y_naive(i), 1e-9));
    }
}
