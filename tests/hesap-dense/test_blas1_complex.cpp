#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::hesap::Complex32;
using crd::hesap::Complex64;
using crd::hesap::dense::asum;
using crd::hesap::dense::axpy;
using crd::hesap::dense::dotc;
using crd::hesap::dense::dotu;
using crd::hesap::dense::iamax;
using crd::hesap::dense::nrm2;
using crd::hesap::dense::scal;
using crd::hesap::dense::Vector;
using Catch::Matchers::WithinAbs;

TEST_CASE("axpy complex: alpha=i scales by i", "[hesap][blas1][complex][axpy]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 3);
    Vector<Complex64> y(&alloc, 3);
    x(0) = Complex64{1.0, 0.0};
    x(1) = Complex64{0.0, 1.0};
    x(2) = Complex64{1.0, 1.0};
    // y = i * x = (i, -1, -1+i)
    axpy<Complex64>(Complex64{0.0, 1.0}, x, y);
    REQUIRE(y(0) == Complex64{0.0, 1.0});
    REQUIRE(y(1) == Complex64{-1.0, 0.0});
    REQUIRE(y(2) == Complex64{-1.0, 1.0});
}

TEST_CASE("dotu complex: matches algebraic formula", "[hesap][blas1][complex][dotu]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 2);
    Vector<Complex64> y(&alloc, 2);
    x(0) = Complex64{1.0, 2.0};
    x(1) = Complex64{3.0, 4.0};
    y(0) = Complex64{5.0, 6.0};
    y(1) = Complex64{7.0, 8.0};
    // x0*y0 = (1+2i)(5+6i) = 5-12 + (6+10)i = -7 + 16i
    // x1*y1 = (3+4i)(7+8i) = 21-32 + (24+28)i = -11 + 52i
    // sum   = -18 + 68i
    const auto d = dotu<crd::f64>(x, y);
    REQUIRE(d == Complex64{-18.0, 68.0});
}

TEST_CASE("dotc complex: Hermitian conjugation of x", "[hesap][blas1][complex][dotc]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 2);
    Vector<Complex64> y(&alloc, 2);
    x(0) = Complex64{1.0, 2.0};
    x(1) = Complex64{3.0, 4.0};
    y(0) = Complex64{5.0, 6.0};
    y(1) = Complex64{7.0, 8.0};
    // conj(x0)*y0 = (1-2i)(5+6i) = 5+12 + (6-10)i = 17 - 4i
    // conj(x1)*y1 = (3-4i)(7+8i) = 21+32 + (24-28)i = 53 - 4i
    // sum         = 70 - 8i
    const auto d = dotc<crd::f64>(x, y);
    REQUIRE(d == Complex64{70.0, -8.0});
}

TEST_CASE("dotc(x, x) is real and equals nrm2(x)^2", "[hesap][blas1][complex][dotc][nrm2]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 4);
    x(0) = Complex64{1.0, 1.0};
    x(1) = Complex64{2.0, -1.0};
    x(2) = Complex64{0.0, 3.0};
    x(3) = Complex64{-1.0, 1.0};
    const auto d = dotc<crd::f64>(x, x);
    // |1+i|^2 + |2-i|^2 + |3i|^2 + |-1+i|^2 = 2 + 5 + 9 + 2 = 18
    REQUIRE(d.im == 0.0);
    REQUIRE(d.re == 18.0);
    const auto n = nrm2<Complex64>(x);
    REQUIRE_THAT(n * n, WithinAbs(d.re, 1e-14));
}

TEST_CASE("dotu vs dotc: differ for non-real x", "[hesap][blas1][complex][dotu][dotc]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 1);
    Vector<Complex64> y(&alloc, 1);
    x(0) = Complex64{1.0, 2.0};
    y(0) = Complex64{3.0, 4.0};
    const auto a = dotu<crd::f64>(x, y);  // (1+2i)(3+4i) = -5 + 10i
    const auto b = dotc<crd::f64>(x, y);  // (1-2i)(3+4i) = 11 - 2i
    REQUIRE(a == Complex64{-5.0, 10.0});
    REQUIRE(b == Complex64{11.0, -2.0});
    REQUIRE_FALSE(a == b);
}

TEST_CASE("nrm2 complex returns real magnitude", "[hesap][blas1][complex][nrm2]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 2);
    x(0) = Complex64{3.0, 4.0};   // |x0| = 5
    x(1) = Complex64{0.0, 12.0};  // |x1| = 12
    // sqrt(25 + 144) = sqrt(169) = 13
    REQUIRE(nrm2<Complex64>(x) == 13.0);
}

TEST_CASE("scal complex: multiply by i rotates 90 degrees", "[hesap][blas1][complex][scal]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 2);
    x(0) = Complex64{1.0, 0.0};
    x(1) = Complex64{0.0, 1.0};
    scal<Complex64>(Complex64{0.0, 1.0}, x);
    REQUIRE(x(0) == Complex64{0.0, 1.0});
    REQUIRE(x(1) == Complex64{-1.0, 0.0});
}

TEST_CASE("asum complex: BLAS componentwise sum |re|+|im|", "[hesap][blas1][complex][asum]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 2);
    x(0) = Complex64{-1.0, 2.0};
    x(1) = Complex64{3.0, -4.0};
    // |−1|+|2| + |3|+|−4| = 3 + 7 = 10
    REQUIRE(asum<Complex64>(x) == 10.0);
}

TEST_CASE("iamax complex: argmax of |re|+|im|; tie-break first", "[hesap][blas1][complex][iamax]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex64> x(&alloc, 3);
    x(0) = Complex64{1.0, 1.0};   // |1|+|1| = 2
    x(1) = Complex64{-2.0, 0.0};  // 2
    x(2) = Complex64{0.0, -1.5};  // 1.5
    REQUIRE(iamax<Complex64>(x) == 0);  // ties: first index wins
}

TEST_CASE("Complex32 axpy + dotu round-trip", "[hesap][blas1][complex][c32]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    Vector<Complex32> x(&alloc, 4);
    Vector<Complex32> y(&alloc, 4);
    for (crd::usize i = 0; i < 4; ++i)
    {
        x(i) = Complex32{static_cast<crd::f32>(i + 1), 0.0F};
        y(i) = Complex32{0.0F, 0.0F};
    }
    axpy<Complex32>(Complex32{2.0F, 0.0F}, x, y);
    REQUIRE(y(3) == Complex32{8.0F, 0.0F});
    const auto d = dotu<crd::f32>(x, y);
    // sum (i+1) * 2*(i+1) = 2 * sum (i+1)^2 = 2 * 30 = 60
    REQUIRE(d == Complex32{60.0F, 0.0F});
}

TEST_CASE("complex determinism: same input -> same output", "[hesap][blas1][complex][det]")
{
    crd::memory::TlsfAllocator alloc(256 * 1024);
    constexpr crd::usize kN = 2000;
    Vector<Complex64> x(&alloc, kN);
    Vector<Complex64> y(&alloc, kN);
    for (crd::usize i = 0; i < kN; ++i)
    {
        x(i) = Complex64{std::sin(static_cast<crd::f64>(i)), std::cos(static_cast<crd::f64>(i))};
        y(i) = Complex64{std::cos(static_cast<crd::f64>(i)), std::sin(static_cast<crd::f64>(i))};
    }
    const auto d1 = dotc<crd::f64>(x, y);
    const auto d2 = dotc<crd::f64>(x, y);
    REQUIRE(d1 == d2);
}
