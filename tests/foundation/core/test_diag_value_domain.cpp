// DIAG.10c -- value-domain probes (crd/core/value_domain.hpp). Values are classified from their bits in both widths;
// a domain rejects exactly the special values it does not allow, so a NaN, an infinity, a subnormal or a -0 that a
// domain admits is never reported; the range is numeric (-0 equals 0, a binary32 value is compared exactly); a probe
// over a run names the first offender and the union of what it found; and no verdict depends on the caller's
// numerical mode, which a classifier or range check built on float comparisons gets wrong under denormals-are-zero.
// Every expected value is written from IEEE-754 bit patterns, not computed by the probe. ASCII test names.

#include <crd/core/fp_environment.hpp>
#include <crd/core/value_domain.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
using crd::f32;
using crd::f64;
using crd::FloatDomain;
using crd::i64;
using crd::IntDomain;
using crd::u32;
using crd::u64;
using crd::ValueClass;
using crd::ValueDomainReport;
using crd::ValueViolation;

constexpr u32 bit(ValueViolation v)
{
    return static_cast<u32>(v);
}

f32 f32_of(u32 b)
{
    f32 f = 0.0F;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

f64 f64_of(u64 b)
{
    f64 f = 0.0;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

// binary32 patterns
constexpr u32 kF32NegZero     = 0x80000000U;
constexpr u32 kF32MinSub      = 0x00000001U; // 2^-149
constexpr u32 kF32MaxSub      = 0x007FFFFFU;
constexpr u32 kF32NegSub      = 0x80000100U; // -(2^-141)
constexpr u32 kF32MinNormal   = 0x00800000U; // 2^-126
constexpr u32 kF32One         = 0x3F800000U;
constexpr u32 kF32Max         = 0x7F7FFFFFU;
constexpr u32 kF32Inf         = 0x7F800000U;
constexpr u32 kF32NegInf      = 0xFF800000U;
constexpr u32 kF32QuietNaN    = 0x7FC00000U;
constexpr u32 kF32SignalNaN   = 0x7F800001U;
constexpr u32 kF32NegNaNLoad  = 0xFFC12345U; // a negative quiet NaN with a payload
// binary64 patterns
constexpr u64 kF64NegZero     = 0x8000000000000000ULL;
constexpr u64 kF64MinSub      = 0x0000000000000001ULL; // 2^-1074
constexpr u64 kF64NegSub      = 0x800000000000ABCDULL;
constexpr u64 kF64MinNormal   = 0x0010000000000000ULL; // 2^-1022
constexpr u64 kF64Inf         = 0x7FF0000000000000ULL;
constexpr u64 kF64NegInf      = 0xFFF0000000000000ULL;
constexpr u64 kF64QuietNaN    = 0x7FF8000000000000ULL;
constexpr u64 kF64SignalNaN   = 0x7FF0000000000001ULL;
// 2^-149 and 2^-148 as binary64: biased exponents 1023-149 = 874 and 875, zero fraction
constexpr u64 kF64TwoPowM149  = 874ULL << 52U;
constexpr u64 kF64TwoPowM148  = 875ULL << 52U;
// the largest binary32 subnormal, (2^23 - 1) * 2^-149, as binary64: 1.(22 ones) * 2^-127, biased exponent 896
constexpr u64 kF64F32MaxSub   = (896ULL << 52U) | (0x3FFFFFULL << 30U);

FloatDomain no_subnormal_no_negzero()
{
    FloatDomain d{};
    d.name                = "strict-finite";
    d.allow_subnormal     = false;
    d.allow_negative_zero = false;
    return d;
}

crd::FpEnvironment flushing_mode()
{
    crd::FpEnvironment e = crd::declared_fp_environment();
    e.mxcsr |= crd::kMxcsrFlushToZero | crd::kMxcsrDenormalsAreZero;
    e.mxcsr = (e.mxcsr & ~crd::kMxcsrRoundingMask) |
              (static_cast<u32>(crd::FpRounding::Down) << crd::kMxcsrRoundingShift);
    return e;
}

bool same(const ValueDomainReport& a, const ValueDomainReport& b)
{
    return a.domain_valid == b.domain_valid && a.checked == b.checked && a.violating == b.violating &&
           a.violations == b.violations && a.first_index == b.first_index &&
           a.first_violations == b.first_violations && a.first_bits == b.first_bits;
}
} // namespace

TEST_CASE("diag10c: values are classified from their bits in both widths", "[core][value-domain][diag10c]")
{
    CHECK(crd::classify_value(f32_of(0U)) == ValueClass::Zero);
    CHECK(crd::classify_value(f32_of(kF32NegZero)) == ValueClass::NegativeZero);
    CHECK(crd::classify_value(f32_of(kF32MinSub)) == ValueClass::Subnormal);
    CHECK(crd::classify_value(f32_of(kF32MaxSub)) == ValueClass::Subnormal);
    CHECK(crd::classify_value(f32_of(kF32NegSub)) == ValueClass::Subnormal);
    CHECK(crd::classify_value(f32_of(kF32MinNormal)) == ValueClass::Normal);
    CHECK(crd::classify_value(f32_of(kF32One | 0x80000000U)) == ValueClass::Normal);
    CHECK(crd::classify_value(f32_of(kF32Max)) == ValueClass::Normal);
    CHECK(crd::classify_value(f32_of(kF32Inf)) == ValueClass::Infinity);
    CHECK(crd::classify_value(f32_of(kF32NegInf)) == ValueClass::NegativeInfinity);
    CHECK(crd::classify_value(f32_of(kF32QuietNaN)) == ValueClass::NaN);
    CHECK(crd::classify_value(f32_of(kF32SignalNaN)) == ValueClass::NaN);
    CHECK(crd::classify_value(f32_of(kF32NegNaNLoad)) == ValueClass::NaN);

    CHECK(crd::classify_value(f64_of(0U)) == ValueClass::Zero);
    CHECK(crd::classify_value(f64_of(kF64NegZero)) == ValueClass::NegativeZero);
    CHECK(crd::classify_value(f64_of(kF64MinSub)) == ValueClass::Subnormal);
    CHECK(crd::classify_value(f64_of(kF64NegSub)) == ValueClass::Subnormal);
    CHECK(crd::classify_value(f64_of(kF64MinNormal)) == ValueClass::Normal);
    // A binary32 subnormal is a normal binary64 value: the width is the value's own.
    CHECK(crd::classify_value(f64_of(kF64F32MaxSub)) == ValueClass::Normal);
    CHECK(crd::classify_value(f64_of(kF64Inf)) == ValueClass::Infinity);
    CHECK(crd::classify_value(f64_of(kF64NegInf)) == ValueClass::NegativeInfinity);
    CHECK(crd::classify_value(f64_of(kF64QuietNaN)) == ValueClass::NaN);
    CHECK(crd::classify_value(f64_of(kF64SignalNaN)) == ValueClass::NaN);

    // Every class has its own name.
    for (u32 i = 0U; i < crd::kValueClassCount; ++i)
    {
        const char* const a = crd::value_class_name(static_cast<ValueClass>(i));
        CHECK(std::strcmp(a, "unknown") != 0);
        for (u32 j = 0U; j < i; ++j)
        {
            CHECK(std::strcmp(a, crd::value_class_name(static_cast<ValueClass>(j))) != 0);
        }
    }
}

TEST_CASE("diag10c: a domain rejects exactly the special values it does not allow", "[core][value-domain][diag10c]")
{
    const FloatDomain any      = crd::any_float_domain();
    const FloatDomain finite   = crd::finite_float_domain();
    const FloatDomain distance = crd::distance_domain();
    const FloatDomain strict   = no_subnormal_no_negzero();

    SECTION("a domain that allows every special value reports none of them")
    {
        const f32 specials32[] = {f32_of(kF32QuietNaN), f32_of(kF32SignalNaN), f32_of(kF32NegNaNLoad), f32_of(kF32Inf),
                                  f32_of(kF32NegInf),   f32_of(kF32MinSub),    f32_of(kF32NegZero)};
        for (const f32 v : specials32)
        {
            CHECK(crd::value_violations(any, v) == 0U);
        }
        const f64 specials64[] = {f64_of(kF64QuietNaN), f64_of(kF64Inf), f64_of(kF64NegInf), f64_of(kF64MinSub),
                                  f64_of(kF64NegZero)};
        for (const f64 v : specials64)
        {
            CHECK(crd::value_violations(any, v) == 0U);
        }
    }

    SECTION("the default finite domain rejects NaN and both infinities and keeps subnormals and -0")
    {
        CHECK(crd::value_violations(finite, f32_of(kF32QuietNaN)) == bit(ValueViolation::NaN));
        CHECK(crd::value_violations(finite, f32_of(kF32SignalNaN)) == bit(ValueViolation::NaN));
        CHECK(crd::value_violations(finite, f32_of(kF32Inf)) == bit(ValueViolation::Infinity));
        CHECK(crd::value_violations(finite, f32_of(kF32NegInf)) == bit(ValueViolation::NegativeInfinity));
        CHECK(crd::value_violations(finite, f64_of(kF64QuietNaN)) == bit(ValueViolation::NaN));
        CHECK(crd::value_violations(finite, f64_of(kF64Inf)) == bit(ValueViolation::Infinity));
        CHECK(crd::value_violations(finite, f64_of(kF64NegInf)) == bit(ValueViolation::NegativeInfinity));
        CHECK(crd::value_violations(finite, f32_of(kF32NegSub)) == 0U);
        CHECK(crd::value_violations(finite, f32_of(kF32NegZero)) == 0U);
        CHECK(crd::value_violations(finite, f64_of(kF64MinSub)) == 0U);
        CHECK(crd::value_violations(finite, f32_of(kF32Max)) == 0U);
    }

    SECTION("the distance domain allows +Inf but neither -Inf nor NaN nor a negative value")
    {
        CHECK(crd::value_violations(distance, f32_of(kF32Inf)) == 0U);
        CHECK(crd::value_violations(distance, f64_of(kF64Inf)) == 0U);
        CHECK(crd::value_violations(distance, f32_of(0U)) == 0U);
        CHECK(crd::value_violations(distance, f32_of(kF32NegZero)) == 0U); // -0 is 0, in [0, +Inf]
        CHECK(crd::value_violations(distance, f32_of(kF32QuietNaN)) == bit(ValueViolation::NaN));
        CHECK(crd::value_violations(distance, f32_of(kF32NegInf)) ==
              (bit(ValueViolation::NegativeInfinity) | bit(ValueViolation::BelowMinimum)));
        CHECK(crd::value_violations(distance, f32_of(kF32One | 0x80000000U)) == bit(ValueViolation::BelowMinimum));
    }

    SECTION("a strict domain names a subnormal and a -0, and only those")
    {
        CHECK(crd::value_violations(strict, f32_of(kF32MinSub)) == bit(ValueViolation::Subnormal));
        CHECK(crd::value_violations(strict, f32_of(kF32NegSub)) == bit(ValueViolation::Subnormal));
        CHECK(crd::value_violations(strict, f64_of(kF64NegSub)) == bit(ValueViolation::Subnormal));
        CHECK(crd::value_violations(strict, f32_of(kF32NegZero)) == bit(ValueViolation::NegativeZero));
        CHECK(crd::value_violations(strict, f64_of(kF64NegZero)) == bit(ValueViolation::NegativeZero));
        CHECK(crd::value_violations(strict, f32_of(0U)) == 0U);
        CHECK(crd::value_violations(strict, f32_of(kF32MinNormal)) == 0U);
    }

    SECTION("every violation has its own name")
    {
        for (u32 i = 0U; i < crd::kValueViolationCount; ++i)
        {
            const char* const a = crd::value_violation_name(static_cast<ValueViolation>(1U << i));
            CHECK(std::strcmp(a, "unknown") != 0);
            for (u32 j = 0U; j < i; ++j)
            {
                CHECK(std::strcmp(a, crd::value_violation_name(static_cast<ValueViolation>(1U << j))) != 0);
            }
        }
    }
}

TEST_CASE("diag10c: the range is numeric and a binary32 value is compared exactly", "[core][value-domain][diag10c]")
{
    const FloatDomain unit = crd::unit_interval_domain();
    CHECK(crd::value_violations(unit, f32_of(0U)) == 0U);
    CHECK(crd::value_violations(unit, f32_of(kF32NegZero)) == 0U);
    CHECK(crd::value_violations(unit, f32_of(kF32One)) == 0U);
    CHECK(crd::value_violations(unit, f32_of(kF32One + 1U)) == bit(ValueViolation::AboveMaximum)); // next after 1
    CHECK(crd::value_violations(unit, f32_of(kF32NegSub)) == bit(ValueViolation::BelowMinimum));   // below 0
    CHECK(crd::value_violations(unit, f64_of(kF64NegSub)) == bit(ValueViolation::BelowMinimum));
    CHECK(crd::value_violations(unit, f32_of(kF32Inf)) ==
          (bit(ValueViolation::Infinity) | bit(ValueViolation::AboveMaximum)));
    CHECK(crd::value_violations(unit, f32_of(kF32QuietNaN)) == bit(ValueViolation::NaN)); // a NaN is not on the line

    // Bounds at binary32 subnormals, written as binary64: the widened value must be exact, not rounded.
    FloatDomain at_min_sub{};
    at_min_sub.minimum = f64_of(kF64TwoPowM149);
    CHECK(crd::value_violations(at_min_sub, f32_of(kF32MinSub)) == 0U);
    CHECK(crd::value_violations(at_min_sub, f32_of(0U)) == bit(ValueViolation::BelowMinimum));
    FloatDomain above_min_sub{};
    above_min_sub.minimum = f64_of(kF64TwoPowM148);
    CHECK(crd::value_violations(above_min_sub, f32_of(kF32MinSub)) == bit(ValueViolation::BelowMinimum));
    CHECK(crd::value_violations(above_min_sub, f32_of(kF32MinSub + 1U)) == 0U); // 2^-148 itself
    FloatDomain to_max_sub{};
    to_max_sub.maximum = f64_of(kF64F32MaxSub);
    CHECK(crd::value_violations(to_max_sub, f32_of(kF32MaxSub)) == 0U);
    CHECK(crd::value_violations(to_max_sub, f32_of(kF32MinNormal)) == bit(ValueViolation::AboveMaximum));
    CHECK(crd::value_violations(to_max_sub, f32_of(kF32NegNaNLoad)) == bit(ValueViolation::NaN));

    // An invalid domain judges no range: its probe checks nothing and says so.
    FloatDomain inverted{};
    inverted.minimum = 1.0;
    inverted.maximum = 0.0;
    FloatDomain nan_bound{};
    nan_bound.maximum = f64_of(kF64QuietNaN);
    CHECK_FALSE(crd::domain_valid(inverted));
    CHECK_FALSE(crd::domain_valid(nan_bound));
    CHECK(crd::domain_valid(unit));
    CHECK(crd::value_violations(inverted, f32_of(kF32One)) == 0U);
    CHECK(crd::value_violations(inverted, f64_of(kF64Inf)) == bit(ValueViolation::Infinity));
    const f32               one[1] = {f32_of(kF32One)};
    const ValueDomainReport bad    = crd::probe_values(inverted, one, 1U);
    CHECK_FALSE(bad.domain_valid);
    CHECK_FALSE(bad.ok());
    CHECK(bad.checked == 0U);
    const f64 one64[1] = {1.0};
    CHECK_FALSE(crd::probe_values(nan_bound, one64, 1U).domain_valid);
}

TEST_CASE("diag10c: an integer domain bounds a count and reports the first value outside it",
          "[core][value-domain][diag10c]")
{
    IntDomain d{};
    d.name    = "count";
    d.minimum = 0;
    d.maximum = 100;
    CHECK(crd::domain_valid(d));
    CHECK(crd::value_violations(d, 0) == 0U);
    CHECK(crd::value_violations(d, 100) == 0U);
    CHECK(crd::value_violations(d, -1) == bit(ValueViolation::BelowMinimum));
    CHECK(crd::value_violations(d, 101) == bit(ValueViolation::AboveMaximum));
    CHECK(crd::value_violations(IntDomain{}, std::numeric_limits<i64>::min()) == 0U);

    const i64               values[] = {3, 100, -7, 250, 5};
    const ValueDomainReport r        = crd::probe_values(d, values, 5U);
    CHECK(r.domain_valid);
    CHECK(r.checked == 5U);
    CHECK(r.violating == 2U);
    CHECK(r.violations == (bit(ValueViolation::BelowMinimum) | bit(ValueViolation::AboveMaximum)));
    CHECK(r.first_index == 2U);
    CHECK(r.first_violations == bit(ValueViolation::BelowMinimum));
    CHECK(r.first_bits == static_cast<u64>(i64{-7}));
    CHECK_FALSE(r.ok());

    IntDomain inverted{};
    inverted.minimum = 5;
    inverted.maximum = 4;
    CHECK_FALSE(crd::domain_valid(inverted));
    CHECK(crd::value_violations(inverted, 1000) == 0U);
    const ValueDomainReport none = crd::probe_values(inverted, values, 5U);
    CHECK_FALSE(none.domain_valid);
    CHECK(none.checked == 0U);
    CHECK(crd::probe_values(d, values, 2U).ok());
}

TEST_CASE("diag10c: a probe over a run names its first offender in both widths", "[core][value-domain][diag10c]")
{
    const FloatDomain       finite = crd::finite_float_domain();
    const f32               a[]    = {f32_of(kF32One), f32_of(kF32NegSub), f32_of(kF32Inf), f32_of(kF32One),
                                      f32_of(kF32NegNaNLoad)};
    const ValueDomainReport r      = crd::probe_values(finite, a, 5U);
    CHECK(r.checked == 5U);
    CHECK(r.violating == 2U);
    CHECK(r.violations == (bit(ValueViolation::Infinity) | bit(ValueViolation::NaN)));
    CHECK(r.first_index == 2U);
    CHECK(r.first_violations == bit(ValueViolation::Infinity));
    CHECK(r.first_bits == kF32Inf); // the binary32 pattern, zero-extended

    const f64               b[] = {f64_of(kF64NegZero), f64_of(kF64SignalNaN), 2.0};
    const ValueDomainReport s   = crd::probe_values(finite, b, 3U);
    CHECK(s.violating == 1U);
    CHECK(s.first_index == 1U);
    CHECK(s.first_bits == kF64SignalNaN);

    const ValueDomainReport clean = crd::probe_values(crd::any_float_domain(), a, 5U);
    CHECK(clean.ok());
    CHECK(clean.first_index == crd::kNoValueIndex);
    CHECK(crd::probe_values(finite, a, 0U).checked == 0U);
}

TEST_CASE("diag10c: no verdict depends on the caller's numerical mode", "[core][value-domain][diag10c][fp]")
{
    const FloatDomain unit   = crd::unit_interval_domain();
    const FloatDomain strict = no_subnormal_no_negzero();
    const f32 a[] = {f32_of(kF32MinSub), f32_of(kF32NegSub), f32_of(kF32MaxSub), f32_of(kF32NegZero), f32_of(kF32One)};
    const f64 b[] = {f64_of(kF64MinSub), f64_of(kF64NegSub), f64_of(kF64MinNormal)};

    const ValueDomainReport unit32   = crd::probe_values(unit, a, 5U);
    const ValueDomainReport strict32 = crd::probe_values(strict, a, 5U);
    const ValueDomainReport unit64   = crd::probe_values(unit, b, 3U);
    // Under the declared mode: the negative subnormals are below 0, and the strict domain names the subnormals and -0.
    CHECK(unit32.violating == 1U);
    CHECK(unit32.first_index == 1U);
    CHECK(unit32.first_violations == bit(ValueViolation::BelowMinimum));
    CHECK(strict32.violating == 4U);
    CHECK(strict32.violations == (bit(ValueViolation::Subnormal) | bit(ValueViolation::NegativeZero)));
    CHECK(unit64.violating == 1U);
    CHECK(unit64.first_index == 1U);

    // Under flush-to-zero, denormals-are-zero and round-down, where a float comparison sees each subnormal as 0.
    const crd::FpEnvironment mode = flushing_mode();
    crd::ScopedFpEnvironment scoped(mode);
#if CRD_ARCH_X64
    REQUIRE(crd::fp_denormals_are_zero(crd::read_fp_environment()));
#endif
    CHECK(crd::classify_value(f32_of(kF32NegSub)) == ValueClass::Subnormal);
    CHECK(crd::classify_value(f64_of(kF64MinSub)) == ValueClass::Subnormal);
    CHECK(same(crd::probe_values(unit, a, 5U), unit32));
    CHECK(same(crd::probe_values(strict, a, 5U), strict32));
    CHECK(same(crd::probe_values(unit, b, 3U), unit64));
    CHECK(crd::value_violations(unit, f32_of(kF32NegSub)) == bit(ValueViolation::BelowMinimum));
}
