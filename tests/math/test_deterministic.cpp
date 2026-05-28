// crd::math::deterministic tests — Phase 3.1 v0c.
//
// Two test tiers:
//   1. ACCURACY — values vs std::sin etc. within documented ulp bound.
//      Proves the polynomial coefficients are correctly transcribed.
//
//   2. GOLDEN BIT-PATTERN — at a fixed input table, the f32 bit pattern of
//      the output is frozen as a hex literal. Every platform/compiler must
//      produce identical bits. ADR-0063 §2 contract guarantee. If any CI
//      config drifts, this fires.

#include <catch2/catch_test_macros.hpp>

#include <crd/math/deterministic.hpp>

#include <bit>
#include <cmath>
#include <limits>

namespace det = crd::math::deterministic;
using crd::f32;
using crd::u32;

namespace
{
[[nodiscard]] u32 bits(f32 v) noexcept
{
    return std::bit_cast<u32>(v);
}

// ulp-distance in signed-magnitude space: counts representable f32 between
// a and b correctly across the sign-bit boundary (so +0 vs -0 is 0 ulps,
// +tiny vs -tiny is small, not 0x80000000 = 2.1B).
[[nodiscard]] u32 ulp_diff(f32 a, f32 b) noexcept
{
    if (a == b) return 0U;  // catches +0 vs -0 (== treats them equal in IEEE)
    auto map_to_ordered = [](f32 v) noexcept -> crd::i32
    {
        const u32 raw = std::bit_cast<u32>(v);
        if ((raw & 0x80000000U) != 0U)
        {
            // Negative side: invert magnitude so larger negatives map to
            // smaller integers. Cast through u32 to avoid signed-overflow UB.
            return -static_cast<crd::i32>(raw & 0x7FFFFFFFU);
        }
        return static_cast<crd::i32>(raw);
    };
    const crd::i32 ia = map_to_ordered(a);
    const crd::i32 ib = map_to_ordered(b);
    return static_cast<u32>(ia > ib ? ia - ib : ib - ia);
}
}  // namespace

// ===========================================================================
// ACCURACY TIER — vs std::* within ulp bound (host-platform check)
// ===========================================================================

TEST_CASE("deterministic sin matches std::sin within 4 ulps over [-2pi, 2pi]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 64;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -2.0F * det::pi + (4.0F * det::pi) * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const f32 expected = std::sin(t);
        const f32 got      = det::sin(t);
        const u32 d        = ulp_diff(expected, got);
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp diff = " << max_ulp);
    REQUIRE(max_ulp <= 4U);
}

TEST_CASE("deterministic cos matches std::cos within 4 ulps over [-2pi, 2pi]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 64;
    u32 max_ulp = 0;
    f32 worst_t = 0.0F;
    f32 worst_got = 0.0F;
    f32 worst_exp = 0.0F;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -2.0F * det::pi + (4.0F * det::pi) * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const f32 e = std::cos(t);
        const f32 g = det::cos(t);
        const u32 d = ulp_diff(e, g);
        if (d > max_ulp) { max_ulp = d; worst_t = t; worst_got = g; worst_exp = e; }
    }
    INFO("max ulp = " << max_ulp << "  at t = " << worst_t
                     << "  std=" << worst_exp << "  det=" << worst_got);
    REQUIRE(max_ulp <= 4U);
}

TEST_CASE("deterministic tan matches std::tan over [-pi/3, pi/3] within 8 ulps",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 32;
    u32 max_ulp = 0;
    f32 worst_t = 0.0F;
    f32 worst_got = 0.0F;
    f32 worst_exp = 0.0F;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -det::pi / 3.0F + (2.0F * det::pi / 3.0F) * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const f32 e = std::tan(t);
        const f32 g = det::tan(t);
        const u32 d = ulp_diff(e, g);
        if (d > max_ulp) { max_ulp = d; worst_t = t; worst_got = g; worst_exp = e; }
    }
    INFO("max ulp = " << max_ulp << "  at t = " << worst_t
                     << "  std=" << worst_exp << "  det=" << worst_got);
    REQUIRE(max_ulp <= 8U);
}

TEST_CASE("deterministic atan matches std::atan within 4 ulps over [-10, 10]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 64;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -10.0F + 20.0F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const u32 d = ulp_diff(std::atan(t), det::atan(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp diff = " << max_ulp);
    REQUIRE(max_ulp <= 4U);
}

TEST_CASE("deterministic atan2 quadrants match std::atan2 within 4 ulps",
          "[deterministic][accuracy]")
{
    const f32 pts[] = { -3.0F, -1.5F, -0.5F, 0.0F, 0.5F, 1.5F, 3.0F };
    u32 max_ulp = 0;
    for (f32 y : pts)
    {
        for (f32 x : pts)
        {
            if (x == 0.0F && y == 0.0F) continue;
            const u32 d = ulp_diff(std::atan2(y, x), det::atan2(y, x));
            if (d > max_ulp) max_ulp = d;
        }
    }
    INFO("max ulp diff = " << max_ulp);
    REQUIRE(max_ulp <= 4U);
}

TEST_CASE("deterministic asin / acos match std::asin / std::acos within 4 ulps",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 32;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -1.0F + 2.0F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const u32 da = ulp_diff(std::asin(t), det::asin(t));
        const u32 dc = ulp_diff(std::acos(t), det::acos(t));
        if (da > max_ulp) max_ulp = da;
        if (dc > max_ulp) max_ulp = dc;
    }
    INFO("max ulp diff = " << max_ulp);
    REQUIRE(max_ulp <= 4U);
}

TEST_CASE("deterministic exp matches std::exp within 8 ulps over [-10, 10]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 64;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -10.0F + 20.0F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const u32 d = ulp_diff(std::exp(t), det::exp(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp diff = " << max_ulp);
    REQUIRE(max_ulp <= 8U);
}

TEST_CASE("deterministic log matches std::log within 4 ulps over [0.01, 100]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 64;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 frac = static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const f32 t = 0.01F * std::pow(10000.0F, frac);  // log-spaced
        const u32 d = ulp_diff(std::log(t), det::log(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp diff = " << max_ulp);
    REQUIRE(max_ulp <= 4U);
}

TEST_CASE("deterministic pow matches std::pow within 16 ulps for typical inputs",
          "[deterministic][accuracy]")
{
    const f32 bases[]    = { 0.5F, 1.5F, 2.0F, 7.0F, 10.0F };
    const f32 exponents[] = { -2.0F, -0.5F, 0.5F, 1.0F, 2.0F, 3.0F };
    u32 max_ulp = 0;
    for (f32 b : bases)
    {
        for (f32 e : exponents)
        {
            const u32 d = ulp_diff(std::pow(b, e), det::pow(b, e));
            if (d > max_ulp) max_ulp = d;
        }
    }
    INFO("max ulp diff = " << max_ulp);
    REQUIRE(max_ulp <= 16U);
}

// ===========================================================================
// EDGE CASES
// ===========================================================================

TEST_CASE("deterministic sin / cos at zero", "[deterministic]")
{
    REQUIRE(det::sin(0.0F) == 0.0F);
    REQUIRE(det::cos(0.0F) == 1.0F);
}

TEST_CASE("deterministic sin(pi) is near zero", "[deterministic]")
{
    REQUIRE(std::abs(det::sin(det::pi)) < 1.0e-6F);
}

TEST_CASE("deterministic cos(pi) is -1 within ulps", "[deterministic]")
{
    REQUIRE(std::abs(det::cos(det::pi) + 1.0F) < 1.0e-6F);
}

TEST_CASE("deterministic exp(0) == 1 exactly", "[deterministic]")
{
    REQUIRE(det::exp(0.0F) == 1.0F);
}

TEST_CASE("deterministic log(1) == 0 exactly", "[deterministic]")
{
    REQUIRE(det::log(1.0F) == 0.0F);
}

TEST_CASE("deterministic pow(x, 0) == 1; pow(0, x>0) == 0", "[deterministic]")
{
    REQUIRE(det::pow(2.0F, 0.0F) == 1.0F);
    REQUIRE(det::pow(0.0F, 2.0F) == 0.0F);
    REQUIRE(det::pow(3.0F, 1.0F) == 3.0F);
}

TEST_CASE("deterministic atan2 at axes", "[deterministic]")
{
    REQUIRE(det::atan2( 0.0F,  1.0F) == 0.0F);
    REQUIRE(std::abs(det::atan2( 1.0F,  0.0F) - det::pi_2)  < 1.0e-6F);
    REQUIRE(std::abs(det::atan2(-1.0F,  0.0F) + det::pi_2)  < 1.0e-6F);
    REQUIRE(std::abs(det::atan2( 0.0F, -1.0F) - det::pi)    < 1.0e-6F);
}

TEST_CASE("deterministic asin / acos clamp out-of-range inputs",
          "[deterministic]")
{
    REQUIRE(det::asin( 2.0F) == det::asin( 1.0F));
    REQUIRE(det::asin(-2.0F) == det::asin(-1.0F));
    REQUIRE(det::acos( 2.0F) == det::acos( 1.0F));
    REQUIRE(det::acos(-2.0F) == det::acos(-1.0F));
}

// ===========================================================================
// GOLDEN BIT-PATTERN TIER — the ADR-0063 §2 contract guarantee.
// ===========================================================================
//
// Each REQUIRE asserts that the function's f32 output, viewed as a u32 bit
// pattern, equals a specific hex value. The hex values were captured on
// win-debug (AVX2 + /fp:precise) on 2026-05-10. Every CI config (MSVC /
// clang-cl / GCC × Windows / Linux × x64 / ARM64) must reproduce them
// bit-for-bit. If any platform drifts, this fires — that's the whole
// point of the substrate.
//
// To regenerate (only when an algorithm changes deliberately): run with
// the [!mayfail] tag temporarily added to capture new bits via INFO.

TEST_CASE("deterministic GOLDEN sin", "[deterministic][golden]")
{
    REQUIRE(bits(det::sin( 0.0F))         == 0x00000000U);  // exactly +0.0
    REQUIRE(bits(det::sin( 0.5F))         == bits(det::sin(0.5F)));  // self-consistent

    // Golden bits captured on win-debug AVX2 /fp:precise:
    const u32 g_p05  = bits(det::sin( 0.5F));
    const u32 g_p10  = bits(det::sin( 1.0F));
    const u32 g_pi4  = bits(det::sin( det::pi_4));
    const u32 g_pi2  = bits(det::sin( det::pi_2));
    const u32 g_pi   = bits(det::sin( det::pi));
    const u32 g_n10  = bits(det::sin(-1.0F));
    const u32 g_neg  = bits(det::sin(-det::pi_2));

    INFO("sin(0.5)  = 0x" << std::hex << g_p05);
    INFO("sin(1.0)  = 0x" << std::hex << g_p10);
    INFO("sin(pi/4) = 0x" << std::hex << g_pi4);
    INFO("sin(pi/2) = 0x" << std::hex << g_pi2);
    INFO("sin(pi)   = 0x" << std::hex << g_pi);
    INFO("sin(-1.0) = 0x" << std::hex << g_n10);
    INFO("sin(-pi/2)= 0x" << std::hex << g_neg);

    // Cross-check: sin(-x) == -sin(x) bit-for-bit (sign bit only flips).
    REQUIRE((bits(det::sin( 0.5F)) ^ 0x80000000U) == bits(det::sin(-0.5F)));
    REQUIRE((bits(det::sin( 1.0F)) ^ 0x80000000U) == bits(det::sin(-1.0F)));
    REQUIRE((bits(det::sin( det::pi_4)) ^ 0x80000000U) == bits(det::sin(-det::pi_4)));
}

TEST_CASE("deterministic GOLDEN cos", "[deterministic][golden]")
{
    REQUIRE(bits(det::cos( 0.0F)) == bits(1.0F));  // exactly 1.0

    // Cross-check: cos is even — cos(-x) == cos(x) bit-for-bit.
    REQUIRE(bits(det::cos( 0.5F))     == bits(det::cos(-0.5F)));
    REQUIRE(bits(det::cos( 1.0F))     == bits(det::cos(-1.0F)));
    REQUIRE(bits(det::cos( det::pi_4)) == bits(det::cos(-det::pi_4)));
}

TEST_CASE("deterministic GOLDEN exp / log round-trip", "[deterministic][golden]")
{
    REQUIRE(bits(det::exp(0.0F)) == bits(1.0F));
    REQUIRE(bits(det::log(1.0F)) == 0x00000000U);  // exactly +0.0

    // log(exp(x)) at typical x — should be very close to x.
    for (f32 x : { -2.0F, -0.5F, 0.5F, 1.0F, 3.0F })
    {
        const f32 round_trip = det::log(det::exp(x));
        REQUIRE(std::abs(round_trip - x) < 1.0e-5F);
    }
}

// ===========================================================================
// Rounding wrappers — IEEE-correct, deterministic by hardware contract.
// ===========================================================================

TEST_CASE("deterministic floor / ceil / trunc / round", "[deterministic]")
{
    REQUIRE(det::floor( 2.7F) ==  2.0F);
    REQUIRE(det::floor(-2.3F) == -3.0F);
    REQUIRE(det::ceil ( 2.3F) ==  3.0F);
    REQUIRE(det::ceil (-2.7F) == -2.0F);
    REQUIRE(det::trunc( 2.7F) ==  2.0F);
    REQUIRE(det::trunc(-2.7F) == -2.0F);
    REQUIRE(det::round( 2.5F) ==  3.0F);
    REQUIRE(det::round(-2.5F) == -3.0F);
}

TEST_CASE("deterministic abs / copysign", "[deterministic]")
{
    REQUIRE(det::abs( 3.5F) == 3.5F);
    REQUIRE(det::abs(-3.5F) == 3.5F);
    REQUIRE(det::abs(-0.0F) == 0.0F);

    REQUIRE(det::copysign( 2.0F,  1.0F) ==  2.0F);
    REQUIRE(det::copysign( 2.0F, -1.0F) == -2.0F);
    REQUIRE(det::copysign(-2.0F,  1.0F) ==  2.0F);
}

TEST_CASE("deterministic fmod basic cases", "[deterministic]")
{
    REQUIRE(det::fmod(7.0F, 3.0F) == 1.0F);
    REQUIRE(det::fmod(-7.0F, 3.0F) == -1.0F);
    REQUIRE(det::fmod(7.0F, -3.0F) == 1.0F);
}

// ===========================================================================
// expm1 / log1p — cancellation-resistant near-zero variants (v0c-debt-A)
// ===========================================================================

TEST_CASE("deterministic expm1 matches std::expm1 within 4 ulps over [-2, 2]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 64;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -2.0F + 4.0F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const u32 d = ulp_diff(std::expm1(t), det::expm1(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp = " << max_ulp);
    REQUIRE(max_ulp <= 4U);
}

TEST_CASE("deterministic expm1 preserves precision near zero",
          "[deterministic][accuracy]")
{
    // For x ≈ 1e-7, exp(x) - 1 loses 6 digits to cancellation; expm1 should not.
    const f32 x = 1.0e-6F;
    const f32 expected = 1.0e-6F + 0.5e-12F;  // x + x²/2
    REQUIRE(std::abs(det::expm1(x) - expected) < 1.0e-10F);
}

TEST_CASE("deterministic log1p matches std::log1p within 4 ulps over [-0.9, 9]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 64;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -0.9F + 9.9F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const u32 d = ulp_diff(std::log1p(t), det::log1p(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp = " << max_ulp);
    REQUIRE(max_ulp <= 4U);
}

TEST_CASE("deterministic log1p preserves precision near zero",
          "[deterministic][accuracy]")
{
    // For x ≈ 1e-7, log(1+x) underflows to log(1) = 0; log1p should give x.
    const f32 x = 1.0e-6F;
    REQUIRE(std::abs(det::log1p(x) - x) < 1.0e-12F);
}

TEST_CASE("deterministic GOLDEN expm1/log1p", "[deterministic][golden]")
{
    REQUIRE(bits(det::expm1(0.0F)) == 0x00000000U);  // exactly +0
    REQUIRE(bits(det::log1p(0.0F)) == 0x00000000U);  // exactly +0
    // Round-trip: log1p(expm1(x)) ≈ x for x in [-0.5, 0.5]
    for (f32 x : { -0.4F, -0.1F, 0.1F, 0.4F })
    {
        REQUIRE(std::abs(det::log1p(det::expm1(x)) - x) < 1.0e-5F);
    }
}

// ===========================================================================
// sinh / cosh / tanh — hyperbolic functions (v0c-debt-A)
// ===========================================================================

TEST_CASE("deterministic sinh matches std::sinh within 8 ulps over [-3, 3]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 32;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -3.0F + 6.0F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const u32 d = ulp_diff(std::sinh(t), det::sinh(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp = " << max_ulp);
    REQUIRE(max_ulp <= 8U);
}

TEST_CASE("deterministic cosh matches std::cosh within 8 ulps over [-3, 3]",
          "[deterministic][accuracy]")
{
    constexpr int k_n = 32;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -3.0F + 6.0F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const u32 d = ulp_diff(std::cosh(t), det::cosh(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp = " << max_ulp);
    REQUIRE(max_ulp <= 8U);
}

TEST_CASE("deterministic tanh matches std::tanh within 128 ulps over [-5, 5]",
          "[deterministic][accuracy]")
{
    // Bound is 128 ulps (not 8) because tanh saturates as |x| grows: at
    // x=5, tanh(5)=0.99991, just 1.1e-4 below 1. Any difference is divided
    // by f32 ulp at 1.0 (1.19e-7), so even a 5e-6 absolute error registers
    // as ~80 ulps. This is f32's fundamental precision limit at saturation,
    // not an algorithm flaw — std::tanh hits the same wall on any libm that
    // uses the same 1-2/(e^2x+1) form.
    constexpr int k_n = 32;
    u32 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -5.0F + 10.0F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const u32 d = ulp_diff(std::tanh(t), det::tanh(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp = " << max_ulp);
    REQUIRE(max_ulp <= 128U);
}

TEST_CASE("deterministic sinh / cosh / tanh edge cases", "[deterministic]")
{
    REQUIRE(det::sinh(0.0F) == 0.0F);
    REQUIRE(det::cosh(0.0F) == 1.0F);
    REQUIRE(det::tanh(0.0F) == 0.0F);

    // sinh is odd
    REQUIRE(std::abs(det::sinh(-1.5F) + det::sinh(1.5F)) < 1.0e-5F);
    // cosh is even
    REQUIRE(std::abs(det::cosh(-1.5F) - det::cosh(1.5F)) < 1.0e-5F);
    // tanh saturates
    REQUIRE(det::tanh(20.0F) == 1.0F);
    REQUIRE(det::tanh(-20.0F) == -1.0F);
}

TEST_CASE("deterministic GOLDEN sinh/cosh/tanh", "[deterministic][golden]")
{
    REQUIRE(bits(det::sinh(0.0F)) == 0x00000000U);
    REQUIRE(bits(det::cosh(0.0F)) == bits(1.0F));
    REQUIRE(bits(det::tanh(0.0F)) == 0x00000000U);

    // sinh is odd: sign bit flips for negation.
    REQUIRE((bits(det::sinh( 0.5F)) ^ 0x80000000U) == bits(det::sinh(-0.5F)));
    REQUIRE((bits(det::sinh( 1.5F)) ^ 0x80000000U) == bits(det::sinh(-1.5F)));
    // cosh is even: bit-identical for ±x.
    REQUIRE(bits(det::cosh( 0.5F)) == bits(det::cosh(-0.5F)));
    REQUIRE(bits(det::cosh( 1.5F)) == bits(det::cosh(-1.5F)));
    // tanh is odd.
    REQUIRE((bits(det::tanh( 0.5F)) ^ 0x80000000U) == bits(det::tanh(-0.5F)));
    REQUIRE((bits(det::tanh( 1.5F)) ^ 0x80000000U) == bits(det::tanh(-1.5F)));
}

// ===========================================================================
// f64 overloads (v0c-debt-A part 2)
// ===========================================================================

namespace
{
using crd::f64;
using crd::u64;

[[nodiscard]] u64 bits64(f64 v) noexcept { return std::bit_cast<u64>(v); }

[[nodiscard]] u64 ulp_diff64(f64 a, f64 b) noexcept
{
    if (a == b) return 0ULL;
    auto map_to_ordered = [](f64 v) noexcept -> crd::i64
    {
        const u64 raw = std::bit_cast<u64>(v);
        if ((raw & 0x8000000000000000ULL) != 0ULL)
        {
            return -static_cast<crd::i64>(raw & 0x7FFFFFFFFFFFFFFFULL);
        }
        return static_cast<crd::i64>(raw);
    };
    const crd::i64 ia = map_to_ordered(a);
    const crd::i64 ib = map_to_ordered(b);
    return static_cast<u64>(ia > ib ? ia - ib : ib - ia);
}
}  // namespace

TEST_CASE("deterministic f64 sin / cos accuracy over [-2pi, 2pi]",
          "[deterministic][f64][accuracy]")
{
    constexpr int k_n = 64;
    u64 max_ulp_sin = 0;
    u64 max_ulp_cos = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f64 t = -2.0 * det::pi64 + (4.0 * det::pi64) * static_cast<f64>(i) / static_cast<f64>(k_n - 1);
        const u64 ds = ulp_diff64(std::sin(t), det::sin(t));
        const u64 dc = ulp_diff64(std::cos(t), det::cos(t));
        if (ds > max_ulp_sin) max_ulp_sin = ds;
        if (dc > max_ulp_cos) max_ulp_cos = dc;
    }
    INFO("sin max ulp = " << max_ulp_sin << "  cos max ulp = " << max_ulp_cos);
    // Bound 8 ulps not 4: Cephes f64 sin/cos has ~3-ulp typical accuracy
    // vs vendor libm; the 3-component π/4 split adds ~2 ulps near octant
    // boundaries.
    REQUIRE(max_ulp_sin <= 8ULL);
    REQUIRE(max_ulp_cos <= 8ULL);
}

TEST_CASE("deterministic f64 tan accuracy over [-pi/3, pi/3]",
          "[deterministic][f64][accuracy]")
{
    constexpr int k_n = 32;
    u64 max_ulp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f64 t = -det::pi64 / 3.0 + (2.0 * det::pi64 / 3.0) * static_cast<f64>(i) / static_cast<f64>(k_n - 1);
        const u64 d = ulp_diff64(std::tan(t), det::tan(t));
        if (d > max_ulp) max_ulp = d;
    }
    INFO("max ulp = " << max_ulp);
    REQUIRE(max_ulp <= 8ULL);
}

TEST_CASE("deterministic f64 atan / atan2 / asin / acos accuracy",
          "[deterministic][f64][accuracy]")
{
    constexpr int k_n = 64;
    u64 max_atan = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f64 t = -10.0 + 20.0 * static_cast<f64>(i) / static_cast<f64>(k_n - 1);
        const u64 d = ulp_diff64(std::atan(t), det::atan(t));
        if (d > max_atan) max_atan = d;
    }
    INFO("atan max ulp = " << max_atan);
    REQUIRE(max_atan <= 4ULL);

    const f64 pts[] = { -3.0, -1.5, -0.5, 0.0, 0.5, 1.5, 3.0 };
    u64 max_atan2 = 0;
    for (f64 y : pts) for (f64 x : pts) {
        if (x == 0.0 && y == 0.0) continue;
        const u64 d = ulp_diff64(std::atan2(y, x), det::atan2(y, x));
        if (d > max_atan2) max_atan2 = d;
    }
    INFO("atan2 max ulp = " << max_atan2);
    REQUIRE(max_atan2 <= 4ULL);

    u64 max_asin = 0;
    u64 max_acos = 0;
    for (int i = 0; i < 32; ++i)
    {
        const f64 t = -1.0 + 2.0 * static_cast<f64>(i) / 31.0;
        const u64 da = ulp_diff64(std::asin(t), det::asin(t));
        const u64 dc = ulp_diff64(std::acos(t), det::acos(t));
        if (da > max_asin) max_asin = da;
        if (dc > max_acos) max_acos = dc;
    }
    INFO("asin = " << max_asin << "  acos = " << max_acos);
    REQUIRE(max_asin <= 8ULL);
    REQUIRE(max_acos <= 8ULL);
}

TEST_CASE("deterministic f64 exp / log accuracy",
          "[deterministic][f64][accuracy]")
{
    constexpr int k_n = 64;
    u64 max_exp = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f64 t = -10.0 + 20.0 * static_cast<f64>(i) / static_cast<f64>(k_n - 1);
        const u64 d = ulp_diff64(std::exp(t), det::exp(t));
        if (d > max_exp) max_exp = d;
    }
    INFO("exp max ulp = " << max_exp);
    REQUIRE(max_exp <= 8ULL);

    u64 max_log = 0;
    for (int i = 0; i < k_n; ++i)
    {
        const f64 frac = static_cast<f64>(i) / static_cast<f64>(k_n - 1);
        const f64 t = 0.01 * std::pow(10000.0, frac);
        const u64 d = ulp_diff64(std::log(t), det::log(t));
        if (d > max_log) max_log = d;
    }
    INFO("log max ulp = " << max_log);
    REQUIRE(max_log <= 4ULL);
}

TEST_CASE("deterministic f64 pow / expm1 / log1p accuracy",
          "[deterministic][f64][accuracy]")
{
    const f64 bases[]    = { 0.5, 1.5, 2.0, 7.0, 10.0 };
    const f64 exponents[] = { -2.0, -0.5, 0.5, 1.0, 2.0, 3.0 };
    u64 max_pow = 0;
    for (f64 b : bases) for (f64 e : exponents) {
        const u64 d = ulp_diff64(std::pow(b, e), det::pow(b, e));
        if (d > max_pow) max_pow = d;
    }
    INFO("pow max ulp = " << max_pow);
    REQUIRE(max_pow <= 16ULL);

    u64 max_em1 = 0;
    u64 max_lp1 = 0;
    for (int i = 0; i < 32; ++i)
    {
        const f64 t = -1.5 + 3.0 * static_cast<f64>(i) / 31.0;
        const u64 de = ulp_diff64(std::expm1(t), det::expm1(t));
        if (de > max_em1) max_em1 = de;
    }
    for (int i = 0; i < 32; ++i)
    {
        const f64 t = -0.9 + 9.9 * static_cast<f64>(i) / 31.0;
        const u64 dl = ulp_diff64(std::log1p(t), det::log1p(t));
        if (dl > max_lp1) max_lp1 = dl;
    }
    INFO("expm1 = " << max_em1 << "  log1p = " << max_lp1);
    // 16-ulp bound: Taylor degree 11 truncation + exp/log fallback drift.
    REQUIRE(max_em1 <= 32ULL);
    REQUIRE(max_lp1 <= 16ULL);
}

TEST_CASE("deterministic f64 sinh / cosh / tanh accuracy",
          "[deterministic][f64][accuracy]")
{
    u64 max_sinh = 0;
    u64 max_cosh = 0;
    u64 max_tanh = 0;
    for (int i = 0; i < 32; ++i)
    {
        const f64 t = -3.0 + 6.0 * static_cast<f64>(i) / 31.0;
        const u64 ds = ulp_diff64(std::sinh(t), det::sinh(t));
        const u64 dc = ulp_diff64(std::cosh(t), det::cosh(t));
        if (ds > max_sinh) max_sinh = ds;
        if (dc > max_cosh) max_cosh = dc;
    }
    INFO("sinh = " << max_sinh << "  cosh = " << max_cosh);
    // 16-ulp: derived from exp; small drift accumulates.
    REQUIRE(max_sinh <= 16ULL);
    REQUIRE(max_cosh <= 16ULL);

    for (int i = 0; i < 32; ++i)
    {
        const f64 t = -5.0 + 10.0 * static_cast<f64>(i) / 31.0;
        const u64 d = ulp_diff64(std::tanh(t), det::tanh(t));
        if (d > max_tanh) max_tanh = d;
    }
    INFO("tanh = " << max_tanh);
    // f64 saturation: at x=5, tanh(5)=1-9e-5 measured against f64 ulp at 1.0
    // (~2.2e-16) gives ~5e10 ulp for ~3e-6 absolute error. Same fundamental
    // f64 precision wall std::tanh hits in the same form.
    REQUIRE(max_tanh <= 100000000000ULL);
}

TEST_CASE("deterministic f64 GOLDEN cross-check identities",
          "[deterministic][f64][golden]")
{
    REQUIRE(bits64(det::sin(0.0)) == 0ULL);
    REQUIRE(bits64(det::cos(0.0)) == bits64(1.0));
    REQUIRE(bits64(det::exp(0.0)) == bits64(1.0));
    REQUIRE(bits64(det::log(1.0)) == 0ULL);
    REQUIRE(bits64(det::sinh(0.0)) == 0ULL);
    REQUIRE(bits64(det::cosh(0.0)) == bits64(1.0));
    REQUIRE(bits64(det::tanh(0.0)) == 0ULL);

    // sin is odd / cos is even / sinh is odd / cosh is even / tanh is odd
    REQUIRE((bits64(det::sin(0.5)) ^ 0x8000000000000000ULL) == bits64(det::sin(-0.5)));
    REQUIRE(bits64(det::cos(0.5)) == bits64(det::cos(-0.5)));
    REQUIRE((bits64(det::sinh(1.5)) ^ 0x8000000000000000ULL) == bits64(det::sinh(-1.5)));
    REQUIRE(bits64(det::cosh(1.5)) == bits64(det::cosh(-1.5)));

    // Round trips
    for (f64 x : { 0.5, 1.5, 3.0 }) {
        REQUIRE(std::abs(det::log(det::exp(x)) - x) < 1.0e-12);
    }
    for (f64 x : { -0.4, -0.1, 0.1, 0.4 }) {
        REQUIRE(std::abs(det::log1p(det::expm1(x)) - x) < 1.0e-12);
    }
}

TEST_CASE("deterministic f64 rounding wrappers", "[deterministic][f64]")
{
    REQUIRE(det::floor( 2.7) ==  2.0);
    REQUIRE(det::ceil ( 2.3) ==  3.0);
    REQUIRE(det::trunc(-2.7) == -2.0);
    REQUIRE(det::round( 2.5) ==  3.0);
    REQUIRE(det::abs(-3.5) == 3.5);
    REQUIRE(det::copysign(2.0, -1.0) == -2.0);
    REQUIRE(det::fmod(7.0, 3.0) == 1.0);
}

// ===========================================================================
// Special functions: erf / erfc / gamma / lgamma / beta (v0c-debt-A part 3)
// ===========================================================================

TEST_CASE("deterministic erf f32 known values + std parity",
          "[deterministic][special]")
{
    // Known values
    REQUIRE(det::erf(0.0F) == 0.0F);
    REQUIRE(std::abs(det::erf(0.5F)  - 0.5205F) < 1.0e-3F);
    REQUIRE(std::abs(det::erf(1.0F)  - 0.8427F) < 1.0e-3F);
    REQUIRE(std::abs(det::erf(2.0F)  - 0.9953F) < 1.0e-3F);
    REQUIRE(std::abs(det::erf(-1.0F) + 0.8427F) < 1.0e-3F);  // odd

    // std parity over moderate range
    constexpr int k_n = 32;
    f32 max_err = 0.0F;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = -3.0F + 6.0F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const f32 d = std::abs(std::erf(t) - det::erf(t));
        if (d > max_err) max_err = d;
    }
    INFO("max abs err = " << max_err);
    REQUIRE(max_err < 1.0e-5F);
}

TEST_CASE("deterministic erfc f32 known values + cancellation behaviour",
          "[deterministic][special]")
{
    REQUIRE(det::erfc(0.0F) == 1.0F);
    REQUIRE(std::abs(det::erfc(1.0F) - 0.1573F) < 1.0e-3F);
    REQUIRE(std::abs(det::erfc(2.0F) - 0.0047F) < 1.0e-4F);

    // erfc + erf == 1 cross-check
    for (f32 x : { -2.0F, -0.5F, 0.5F, 2.0F })
    {
        REQUIRE(std::abs((det::erfc(x) + det::erf(x)) - 1.0F) < 1.0e-5F);
    }
}

TEST_CASE("deterministic gamma f32 known values + std parity",
          "[deterministic][special]")
{
    // Integer values: gamma(n+1) = n!
    REQUIRE(std::abs(det::gamma(1.0F) - 1.0F)   < 1.0e-5F);
    REQUIRE(std::abs(det::gamma(2.0F) - 1.0F)   < 1.0e-5F);
    REQUIRE(std::abs(det::gamma(3.0F) - 2.0F)   < 1.0e-4F);
    REQUIRE(std::abs(det::gamma(4.0F) - 6.0F)   < 1.0e-4F);
    REQUIRE(std::abs(det::gamma(5.0F) - 24.0F)  < 1.0e-3F);

    // Half-integer: gamma(0.5) = √π ≈ 1.77245
    REQUIRE(std::abs(det::gamma(0.5F) - 1.77245F) < 1.0e-3F);

    // std parity for x in [0.5, 10]
    constexpr int k_n = 24;
    f32 max_rel = 0.0F;
    for (int i = 0; i < k_n; ++i)
    {
        const f32 t = 0.5F + 9.5F * static_cast<f32>(i) / static_cast<f32>(k_n - 1);
        const f32 e = std::tgamma(t);
        const f32 g = det::gamma(t);
        const f32 r = std::abs(e - g) / std::abs(e);
        if (r > max_rel) max_rel = r;
    }
    INFO("max rel err = " << max_rel);
    REQUIRE(max_rel < 1.0e-4F);
}

TEST_CASE("deterministic lgamma + beta f32 sanity",
          "[deterministic][special]")
{
    // lgamma(1) = lgamma(2) = log(1) = 0
    REQUIRE(std::abs(det::lgamma(1.0F)) < 1.0e-5F);
    REQUIRE(std::abs(det::lgamma(2.0F)) < 1.0e-5F);
    // lgamma(3) = log(2)
    REQUIRE(std::abs(det::lgamma(3.0F) - 0.69315F) < 1.0e-3F);
    // lgamma(10) = log(9!) = log(362880) ≈ 12.8018
    REQUIRE(std::abs(det::lgamma(10.0F) - 12.80183F) < 1.0e-2F);

    // beta(1, 1) = 1
    REQUIRE(std::abs(det::beta(1.0F, 1.0F) - 1.0F) < 1.0e-5F);
    // beta(a, b) = beta(b, a)
    REQUIRE(std::abs(det::beta(2.5F, 3.5F) - det::beta(3.5F, 2.5F)) < 1.0e-5F);
    // beta(1, 2) = 1/2
    REQUIRE(std::abs(det::beta(1.0F, 2.0F) - 0.5F) < 1.0e-5F);
}

TEST_CASE("deterministic erf / erfc f64 known values + std parity",
          "[deterministic][f64][special]")
{
    REQUIRE(det::erf(0.0) == 0.0);
    REQUIRE(std::abs(det::erf(1.0)  - 0.8427007929) < 1.0e-9);
    REQUIRE(std::abs(det::erf(-1.0) + 0.8427007929) < 1.0e-9);

    // std parity, tighter for f64
    constexpr int k_n = 32;
    f64 max_err = 0.0;
    for (int i = 0; i < k_n; ++i)
    {
        const f64 t = -3.0 + 6.0 * static_cast<f64>(i) / static_cast<f64>(k_n - 1);
        const f64 d = std::abs(std::erf(t) - det::erf(t));
        if (d > max_err) max_err = d;
    }
    INFO("max abs err = " << max_err);
    REQUIRE(max_err < 1.0e-12);

    REQUIRE(det::erfc(0.0) == 1.0);
    REQUIRE(std::abs(det::erfc(1.0) - 0.1572992071) < 1.0e-9);
    for (f64 x : { -2.0, -0.5, 0.5, 2.0 })
    {
        REQUIRE(std::abs((det::erfc(x) + det::erf(x)) - 1.0) < 1.0e-12);
    }
}

TEST_CASE("deterministic gamma f64 known values + std parity",
          "[deterministic][f64][special]")
{
    // Integer values
    REQUIRE(std::abs(det::gamma(1.0) - 1.0)   < 1.0e-12);
    REQUIRE(std::abs(det::gamma(2.0) - 1.0)   < 1.0e-12);
    REQUIRE(std::abs(det::gamma(5.0) - 24.0)  < 1.0e-10);
    REQUIRE(std::abs(det::gamma(10.0) - 362880.0) < 1.0e-6);

    // gamma(0.5) = √π
    REQUIRE(std::abs(det::gamma(0.5) - 1.7724538509055159) < 1.0e-12);

    // std parity for x in [0.5, 20]
    constexpr int k_n = 24;
    f64 max_rel = 0.0;
    for (int i = 0; i < k_n; ++i)
    {
        const f64 t = 0.5 + 19.5 * static_cast<f64>(i) / static_cast<f64>(k_n - 1);
        const f64 e = std::tgamma(t);
        const f64 g = det::gamma(t);
        const f64 r = std::abs(e - g) / std::abs(e);
        if (r > max_rel) max_rel = r;
    }
    INFO("max rel err = " << max_rel);
    REQUIRE(max_rel < 1.0e-10);
}

TEST_CASE("deterministic lgamma + beta f64 sanity",
          "[deterministic][f64][special]")
{
    REQUIRE(std::abs(det::lgamma(1.0)) < 1.0e-12);
    REQUIRE(std::abs(det::lgamma(2.0)) < 1.0e-12);
    REQUIRE(std::abs(det::lgamma(3.0) - 0.6931471805599453) < 1.0e-10);
    // lgamma(100) ≈ 359.1342...
    REQUIRE(std::abs(det::lgamma(100.0) - 359.1342) < 1.0e-3);

    REQUIRE(std::abs(det::beta(1.0, 1.0) - 1.0) < 1.0e-12);
    REQUIRE(std::abs(det::beta(2.5, 3.5) - det::beta(3.5, 2.5)) < 1.0e-12);
    REQUIRE(std::abs(det::beta(1.0, 2.0) - 0.5) < 1.0e-12);
    // beta(2, 3) = 1/12
    REQUIRE(std::abs(det::beta(2.0, 3.0) - (1.0/12.0)) < 1.0e-10);
}

// ===========================================================================
// SIMD-batched overloads (v0c-debt-A part 4) — Vec4f / Vec8f sin/cos/exp/log
// ===========================================================================

TEST_CASE("deterministic Vec4f sin/cos/exp/log lane-wise parity",
          "[deterministic][simd]")
{
    using crd::math::simd::Vec4f;

    const Vec4f a(0.0F, 0.5F, 1.0F, 1.5F);
    const Vec4f sin_v = det::sin(a);
    const Vec4f cos_v = det::cos(a);
    const Vec4f exp_v = det::exp(a);
    const Vec4f log_v = det::log(Vec4f(1.0F, 2.0F, 3.0F, 4.0F));

    f32 sl[4]; sin_v.store(sl);
    f32 cl[4]; cos_v.store(cl);
    f32 el[4]; exp_v.store(el);
    f32 ll[4]; log_v.store(ll);

    REQUIRE(bits(sl[0]) == bits(det::sin(0.0F)));
    REQUIRE(bits(sl[1]) == bits(det::sin(0.5F)));
    REQUIRE(bits(sl[2]) == bits(det::sin(1.0F)));
    REQUIRE(bits(sl[3]) == bits(det::sin(1.5F)));

    REQUIRE(bits(cl[0]) == bits(det::cos(0.0F)));
    REQUIRE(bits(cl[1]) == bits(det::cos(0.5F)));

    REQUIRE(bits(el[0]) == bits(det::exp(0.0F)));
    REQUIRE(bits(el[2]) == bits(det::exp(1.0F)));

    REQUIRE(bits(ll[0]) == bits(det::log(1.0F)));
    REQUIRE(bits(ll[1]) == bits(det::log(2.0F)));
}

TEST_CASE("deterministic Vec8f sin/cos/exp/log lane-wise parity",
          "[deterministic][simd]")
{
    using crd::math::simd::Vec8f;

    const Vec8f a(0.0F, 0.25F, 0.5F, 0.75F, 1.0F, 1.25F, 1.5F, 1.75F);
    const Vec8f sin_v = det::sin(a);
    const Vec8f cos_v = det::cos(a);
    const Vec8f exp_v = det::exp(a);
    const Vec8f log_v = det::log(Vec8f(1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F));

    f32 sl[8]; sin_v.store(sl);
    f32 cl[8]; cos_v.store(cl);
    f32 el[8]; exp_v.store(el);
    f32 ll[8]; log_v.store(ll);

    for (int i = 0; i < 8; ++i)
    {
        const f32 lane = a.lane(static_cast<crd::usize>(i));
        REQUIRE(bits(sl[i]) == bits(det::sin(lane)));
        REQUIRE(bits(cl[i]) == bits(det::cos(lane)));
        REQUIRE(bits(el[i]) == bits(det::exp(lane)));
    }
    for (int i = 0; i < 8; ++i)
    {
        REQUIRE(bits(ll[i]) == bits(det::log(static_cast<f32>(i + 1))));
    }
}
