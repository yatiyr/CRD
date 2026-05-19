#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::Complex64;
using crd::hesap::dense::gemv;
using crd::hesap::dense::gerc;
using crd::hesap::dense::geru;
using crd::hesap::dense::hemv;
using crd::hesap::dense::her;
using crd::hesap::dense::Hermitian;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Trans;
using crd::hesap::dense::Triangular;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;
using crd::hesap::dense::trmv;
using crd::hesap::dense::trsv;
using crd::hesap::dense::Vector;
using Catch::Matchers::WithinAbs;

TEST_CASE("gemv Complex64: standard product", "[hesap][blas2][complex][gemv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<Complex64> a(&alloc, 2, 2);
    a(0, 0) = Complex64{1.0, 0.0};
    a(0, 1) = Complex64{0.0, 1.0};  // i
    a(1, 0) = Complex64{0.0, -1.0}; // -i
    a(1, 1) = Complex64{2.0, 0.0};
    Vector<Complex64> x(&alloc, 2);
    x(0) = Complex64{1.0, 1.0};
    x(1) = Complex64{2.0, 0.0};
    Vector<Complex64> y(&alloc, 2);
    gemv<Complex64>(Complex64{1.0, 0.0}, a.cview(), x.span(),
                    Complex64{0.0, 0.0}, y.span(), Trans::None);
    // y[0] = (1)(1+i) + (i)(2) = 1+i + 2i = 1+3i
    // y[1] = (-i)(1+i) + 2(2) = -i + 1 + 4 = 5 - i
    REQUIRE(y(0) == Complex64{1.0, 3.0});
    REQUIRE(y(1) == Complex64{5.0, -1.0});
}

TEST_CASE("gemv ConjTranspose conjugates each entry", "[hesap][blas2][complex][gemv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<Complex64> a(&alloc, 2, 2);
    a(0, 0) = Complex64{1.0, 2.0};
    a(0, 1) = Complex64{3.0, 4.0};
    a(1, 0) = Complex64{0.0, 0.0};
    a(1, 1) = Complex64{0.0, 0.0};
    Vector<Complex64> x(&alloc, 2);
    x(0) = Complex64{1.0, 0.0};
    x(1) = Complex64{0.0, 0.0};
    Vector<Complex64> y(&alloc, 2);
    gemv<Complex64>(Complex64{1.0, 0.0}, a.cview(), x.span(),
                    Complex64{0.0, 0.0}, y.span(), Trans::ConjTranspose);
    // y[0] = conj(A[0,0]) * 1 + conj(A[1,0]) * 0 = (1, -2)
    // y[1] = conj(A[0,1]) * 1 + conj(A[1,1]) * 0 = (3, -4)
    REQUIRE(y(0) == Complex64{1.0, -2.0});
    REQUIRE(y(1) == Complex64{3.0, -4.0});
}

TEST_CASE("geru vs gerc differ for non-real y", "[hesap][blas2][complex][ger]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<Complex64> a_u(&alloc, 1, 1);
    Matrix<Complex64> a_c(&alloc, 1, 1);
    Vector<Complex64> x(&alloc, 1);
    Vector<Complex64> y(&alloc, 1);
    x(0) = Complex64{1.0, 2.0};
    y(0) = Complex64{3.0, 4.0};
    geru<crd::f64>(Complex64{1.0, 0.0}, x.span(), y.span(), a_u.view());
    gerc<crd::f64>(Complex64{1.0, 0.0}, x.span(), y.span(), a_c.view());
    // u = (1+2i)(3+4i) = -5 + 10i
    // c = (1+2i)(3-4i) = 11 + 2i
    REQUIRE(a_u(0, 0) == Complex64{-5.0, 10.0});
    REQUIRE(a_c(0, 0) == Complex64{11.0, 2.0});
}

TEST_CASE("hemv: Hermitian A * x", "[hesap][blas2][complex][hemv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Hermitian<Complex64> a(&alloc, 2);
    a.at_lower(0, 0) = Complex64{2.0, 0.0};  // real diag
    a.at_lower(1, 0) = Complex64{1.0, 1.0};
    a.at_lower(1, 1) = Complex64{3.0, 0.0};
    Vector<Complex64> x(&alloc, 2);
    x(0) = Complex64{1.0, 0.0};
    x(1) = Complex64{0.0, 1.0};
    Vector<Complex64> y(&alloc, 2);
    hemv<crd::f64>(Complex64{1.0, 0.0}, a, x.span(), Complex64{0.0, 0.0}, y.span());
    // y[0] = A[0,0]*x[0] + A[0,1]*x[1] = 2*1 + conj(1+i)*i = 2 + (1-i)*i = 2 + i + 1 = 3+i
    // y[1] = A[1,0]*x[0] + A[1,1]*x[1] = (1+i)*1 + 3*i = 1 + 4i
    REQUIRE(y(0) == Complex64{3.0, 1.0});
    REQUIRE(y(1) == Complex64{1.0, 4.0});
}

TEST_CASE("her: real-alpha rank-1 Hermitian update keeps diagonal real", "[hesap][blas2][complex][her]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Hermitian<Complex64> a(&alloc, 2);
    Vector<Complex64> x(&alloc, 2);
    x(0) = Complex64{1.0, 2.0};
    x(1) = Complex64{3.0, -1.0};
    her<crd::f64>(1.0, x.span(), a);
    // A[0,0] += |x[0]|^2 = 5
    // A[1,0] += x[1] * conj(x[0]) = (3-i)(1-2i) = 3 -6i -i + 2i^2 = 1 - 7i
    // A[1,1] += |x[1]|^2 = 10
    REQUIRE_THAT(a.at_lower(0, 0).re, WithinAbs(5.0, 1e-15));
    REQUIRE_THAT(a.at_lower(0, 0).im, WithinAbs(0.0, 1e-15));
    REQUIRE_THAT(a.at_lower(1, 0).re, WithinAbs(1.0, 1e-15));
    REQUIRE_THAT(a.at_lower(1, 0).im, WithinAbs(-7.0, 1e-15));
    REQUIRE_THAT(a.at_lower(1, 1).re, WithinAbs(10.0, 1e-15));
    REQUIRE_THAT(a.at_lower(1, 1).im, WithinAbs(0.0, 1e-15));
}

TEST_CASE("trmv + trsv complex Lower round-trip", "[hesap][blas2][complex][trmv][trsv]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Triangular<Complex64, TriangularSide::Lower, TriangularDiag::Explicit> l(&alloc, 3);
    l.at(0, 0) = Complex64{2.0, 0.0};
    l.at(1, 0) = Complex64{1.0, 1.0};
    l.at(1, 1) = Complex64{3.0, 0.0};
    l.at(2, 0) = Complex64{0.0, 1.0};
    l.at(2, 1) = Complex64{1.0, -1.0};
    l.at(2, 2) = Complex64{4.0, 0.0};

    Vector<Complex64> x0(&alloc, 3);
    x0(0) = Complex64{1.0, 2.0};
    x0(1) = Complex64{3.0, -1.0};
    x0(2) = Complex64{0.0, 5.0};
    auto x = x0.clone();
    trmv<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(l, x.span(), Trans::None);
    trsv<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(l, x.span(), Trans::None);
    for (crd::usize i = 0; i < 3; ++i)
    {
        REQUIRE_THAT(x(i).re, WithinAbs(x0(i).re, 1e-12));
        REQUIRE_THAT(x(i).im, WithinAbs(x0(i).im, 1e-12));
    }
}
