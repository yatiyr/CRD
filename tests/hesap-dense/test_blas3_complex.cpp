#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::Complex64;
using crd::hesap::dense::gemm;
using crd::hesap::dense::herk;
using crd::hesap::dense::Hermitian;
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Trans;
using crd::hesap::dense::Triangular;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;
using crd::hesap::dense::trmm;
using crd::hesap::dense::trsm;
using Catch::Matchers::WithinAbs;

TEST_CASE("gemm Complex64: 2x2 textbook", "[hesap][blas3][complex][gemm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<Complex64> a(&alloc, 2, 2);
    Matrix<Complex64> b(&alloc, 2, 2);
    a(0, 0) = Complex64{1.0, 0.0};
    a(0, 1) = Complex64{0.0, 1.0};
    a(1, 0) = Complex64{0.0, 0.0};
    a(1, 1) = Complex64{1.0, 0.0};
    b(0, 0) = Complex64{2.0, 0.0};
    b(0, 1) = Complex64{0.0, 0.0};
    b(1, 0) = Complex64{0.0, 0.0};
    b(1, 1) = Complex64{0.0, 1.0};
    Matrix<Complex64> c(&alloc, 2, 2);
    gemm<Complex64, Layout::RowMajor>(Complex64{1.0, 0.0}, a, b, Complex64{0.0, 0.0}, c);
    // c = A * B
    // c[0,0] = 1*2 + i*0 = 2
    // c[0,1] = 1*0 + i*i = -1
    // c[1,0] = 0*2 + 1*0 = 0
    // c[1,1] = 0*0 + 1*i = i
    REQUIRE(c(0, 0) == Complex64{2.0, 0.0});
    REQUIRE(c(0, 1) == Complex64{-1.0, 0.0});
    REQUIRE(c(1, 0) == Complex64{0.0, 0.0});
    REQUIRE(c(1, 1) == Complex64{0.0, 1.0});
}

TEST_CASE("gemm Complex64 ConjTranspose conjugates per element", "[hesap][blas3][complex][gemm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<Complex64> a(&alloc, 1, 1);
    Matrix<Complex64> b(&alloc, 1, 1);
    a(0, 0) = Complex64{3.0, 4.0};
    b(0, 0) = Complex64{1.0, 0.0};
    Matrix<Complex64> c(&alloc, 1, 1);
    gemm<Complex64, Layout::RowMajor>(Complex64{1.0, 0.0}, a, b, Complex64{0.0, 0.0}, c,
                                       Trans::ConjTranspose, Trans::None);
    // C = conj(A) * B = (3-4i)*1 = 3-4i
    REQUIRE(c(0, 0) == Complex64{3.0, -4.0});
}

TEST_CASE("herk: A * A^H is positive-definite Hermitian", "[hesap][blas3][complex][herk]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Matrix<Complex64> a(&alloc, 2, 2);
    a(0, 0) = Complex64{1.0, 1.0};
    a(0, 1) = Complex64{2.0, 0.0};
    a(1, 0) = Complex64{0.0, -1.0};
    a(1, 1) = Complex64{1.0, 1.0};
    Hermitian<Complex64> c(&alloc, 2);
    herk<crd::f64>(1.0, a.cview(), 0.0, c, Trans::None);
    // C[i,j] = sum_k A[i,k] * conj(A[j,k])
    // C[0,0] = |1+i|^2 + |2|^2 = 2 + 4 = 6 (real)
    // C[1,0] = (-i)*(1-i) + (1+i)*2 = (-i+i^2) + (2+2i) = (-1-i) + (2+2i) = 1 + i
    // C[1,1] = |−i|^2 + |1+i|^2 = 1 + 2 = 3 (real)
    REQUIRE_THAT(c.at_lower(0, 0).re, WithinAbs(6.0, 1e-12));
    REQUIRE_THAT(c.at_lower(0, 0).im, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(c.at_lower(1, 0).re, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(c.at_lower(1, 0).im, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(c.at_lower(1, 1).re, WithinAbs(3.0, 1e-12));
    REQUIRE_THAT(c.at_lower(1, 1).im, WithinAbs(0.0, 1e-12));
}

TEST_CASE("trmm + trsm complex Lower round-trip", "[hesap][blas3][complex][trmm][trsm]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    constexpr crd::usize kN = 4;
    Triangular<Complex64, TriangularSide::Lower, TriangularDiag::Explicit> l(&alloc, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            l.at(i, j) = (i == j) ? Complex64{3.0, 0.0} : Complex64{0.5, 0.1};
        }
    }
    Matrix<Complex64> b_initial(&alloc, kN, 2);
    for (crd::usize i = 0; i < kN; ++i)
    {
        b_initial(i, 0) = Complex64{static_cast<crd::f64>(i + 1), -static_cast<crd::f64>(i)};
        b_initial(i, 1) = Complex64{static_cast<crd::f64>(i), static_cast<crd::f64>(i + 1)};
    }
    auto b = b_initial.clone();
    trmm<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(Complex64{1.0, 0.0}, l, b.view(), Trans::None);
    trsm<Complex64, TriangularSide::Lower, TriangularDiag::Explicit>(Complex64{1.0, 0.0}, l, b.view(), Trans::None);
    for (crd::usize i = 0; i < kN; ++i)
    {
        for (crd::usize j = 0; j < 2; ++j)
        {
            REQUIRE_THAT(b(i, j).re, WithinAbs(b_initial(i, j).re, 1e-11));
            REQUIRE_THAT(b(i, j).im, WithinAbs(b_initial(i, j).im, 1e-11));
        }
    }
}
