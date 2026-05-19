#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/complex.hpp>

#include <cmath>

using crd::hesap::abs;
using crd::hesap::arg;
using crd::hesap::Complex32;
using crd::hesap::Complex64;
using crd::hesap::conj;
using crd::hesap::imag;
using crd::hesap::norm_sq;
using crd::hesap::real;

TEST_CASE("Complex zero-init defaults to (0,0)", "[hesap][complex]")
{
    Complex64 z{};
    REQUIRE(z.re == 0.0);
    REQUIRE(z.im == 0.0);
    REQUIRE(z.is_zero());
    REQUIRE(z.is_real());
}

TEST_CASE("Complex addition, subtraction, negation", "[hesap][complex]")
{
    const Complex64 a{1.0, 2.0};
    const Complex64 b{3.0, -4.0};

    const Complex64 sum = a + b;
    REQUIRE(sum.re == 4.0);
    REQUIRE(sum.im == -2.0);

    const Complex64 diff = a - b;
    REQUIRE(diff.re == -2.0);
    REQUIRE(diff.im == 6.0);

    const Complex64 neg = -a;
    REQUIRE(neg.re == -1.0);
    REQUIRE(neg.im == -2.0);
}

TEST_CASE("Complex multiplication matches algebraic formula", "[hesap][complex]")
{
    // (1 + 2i)(3 + 4i) = (1*3 - 2*4) + (1*4 + 2*3)i = -5 + 10i
    const Complex64 a{1.0, 2.0};
    const Complex64 b{3.0, 4.0};
    const Complex64 prod = a * b;
    REQUIRE(prod.re == -5.0);
    REQUIRE(prod.im == 10.0);
}

TEST_CASE("Complex division round-trip is identity within ulp", "[hesap][complex]")
{
    using Catch::Matchers::WithinAbs;
    const Complex64 a{7.0, -3.0};
    const Complex64 b{2.0, 5.0};
    const Complex64 quot = a / b;
    const Complex64 back = quot * b;
    REQUIRE_THAT(back.re, WithinAbs(a.re, 1e-14));
    REQUIRE_THAT(back.im, WithinAbs(a.im, 1e-14));
}

TEST_CASE("Complex Smith division handles |b.im| > |b.re|", "[hesap][complex]")
{
    using Catch::Matchers::WithinAbs;
    // b chosen so the second branch of Smith division fires (|im| > |re|).
    const Complex64 a{3.0, 4.0};
    const Complex64 b{1.0, 100.0};
    const Complex64 q = a / b;
    const Complex64 back = q * b;
    REQUIRE_THAT(back.re, WithinAbs(a.re, 1e-12));
    REQUIRE_THAT(back.im, WithinAbs(a.im, 1e-12));
}

TEST_CASE("Complex conjugate is its own inverse", "[hesap][complex]")
{
    const Complex64 z{1.5, -2.25};
    const Complex64 cc = conj(conj(z));
    REQUIRE(cc.re == z.re);
    REQUIRE(cc.im == z.im);
    // z * conj(z) is a real number equal to |z|^2.
    const Complex64 mod = z * conj(z);
    REQUIRE(mod.im == 0.0);
    REQUIRE(mod.re == norm_sq(z));
}

TEST_CASE("Complex abs and arg follow standard definitions", "[hesap][complex]")
{
    using Catch::Matchers::WithinAbs;
    const Complex64 z{3.0, 4.0};
    REQUIRE_THAT(abs(z), WithinAbs(5.0, 1e-15));
    REQUIRE_THAT(arg(z), WithinAbs(std::atan2(4.0, 3.0), 1e-15));
}

TEST_CASE("Complex sizes and trivially-copyable contracts", "[hesap][complex]")
{
    STATIC_REQUIRE(sizeof(Complex32) == 8);
    STATIC_REQUIRE(sizeof(Complex64) == 16);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Complex32>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Complex64>);
}

TEST_CASE("Complex scalar overloads commute", "[hesap][complex]")
{
    const Complex64 z{2.0, 3.0};
    const Complex64 a = z * 4.0;
    const Complex64 b = 4.0 * z;
    REQUIRE(a == b);
    REQUIRE(a.re == 8.0);
    REQUIRE(a.im == 12.0);
}

TEST_CASE("Complex real and imag projections", "[hesap][complex]")
{
    const Complex64 z{7.5, -1.25};
    REQUIRE(real(z) == 7.5);
    REQUIRE(imag(z) == -1.25);
}
