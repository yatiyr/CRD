#pragma once

// Value-domain probes: does a value lie in the domain its boundary declares? DIAG.10c.
//
// A wrong result that does not crash is often a value no correct run produces: a NaN in an audio buffer, a negative
// distance, a subnormal that a later flush-to-zero turns into a division by zero, a count past its bound. Which special
// values are wrong depends on the domain. A NaN may encode "no data" in one table and be poison in the next; +Inf is
// the right distance for a ray that hits nothing; -0 is a valid result of many functions. So a domain declares what it
// allows, class by class, plus an optional range, and the probe reports what it found without changing any value
// (docs/design/runtime-diagnostics.md, DIAG.10c).
//
// The probe reads bit patterns only. It does no floating-point arithmetic and no floating-point comparison, so the
// caller's numerical mode cannot change a verdict: under denormals-are-zero a subnormal compares equal to zero and a
// negative subnormal is not "below 0" (crd/core/fp_environment.hpp), and a classifier built on `x == 0` or
// `fabs(x) < FLT_MIN` reports such a value as a zero. A binary32 value is widened to binary64 exactly, bit by bit,
// before it is compared with the domain's bounds.

#include <crd/core/types.hpp>

#include <limits>

namespace crd
{

// The IEEE-754 class of a value, read from its bits.
// NOLINTNEXTLINE(performance-enum-size)
enum class ValueClass : crd::u8
{
    Zero,             // +0
    NegativeZero,     // -0
    Subnormal,        // nonzero with a zero exponent field (either sign)
    Normal,           // finite and normal (either sign)
    Infinity,         // +Inf
    NegativeInfinity, // -Inf
    NaN,              // any NaN, quiet or signalling, either sign, any payload
};
inline constexpr crd::u32 kValueClassCount = 7U;
[[nodiscard]] const char* value_class_name(ValueClass c) noexcept;

// What a value did that its domain does not allow, one bit each. A value can carry several (a disallowed -Inf is also
// below a finite minimum).
// NOLINTNEXTLINE(performance-enum-size)
enum class ValueViolation : crd::u32
{
    NaN              = 1U << 0U,
    Infinity         = 1U << 1U, // +Inf
    NegativeInfinity = 1U << 2U,
    Subnormal        = 1U << 3U,
    NegativeZero     = 1U << 4U,
    BelowMinimum     = 1U << 5U,
    AboveMaximum     = 1U << 6U,
};
inline constexpr crd::u32 kValueViolationCount = 7U;
[[nodiscard]] const char* value_violation_name(ValueViolation v) noexcept;

// A floating-point domain: the special classes it allows and the closed range [minimum, maximum] every non-NaN value
// must lie in. The range compares numerically, so -0 equals +0 and an infinite bound admits the infinity at that end
// (if its class is allowed). The defaults are "finite": no NaN, no infinity, subnormals and -0 allowed, no bound.
// A domain is valid when neither bound is a NaN and minimum <= maximum; the probe checks nothing against an invalid
// one.
struct FloatDomain
{
    const char* name                    = "finite";
    bool        allow_nan               = false;
    bool        allow_infinity          = false; // +Inf
    bool        allow_negative_infinity = false;
    bool        allow_subnormal         = true;
    bool        allow_negative_zero     = true;
    crd::f64    minimum                 = -std::numeric_limits<crd::f64>::infinity();
    crd::f64    maximum                 = std::numeric_limits<crd::f64>::infinity();
};

// Every IEEE value is valid (a table where NaN encodes a missing sample, a raw bit-preserving copy).
[[nodiscard]] constexpr FloatDomain any_float_domain() noexcept
{
    FloatDomain d{};
    d.name                    = "any";
    d.allow_nan               = true;
    d.allow_infinity          = true;
    d.allow_negative_infinity = true;
    return d;
}
// Finite values of either sign (the default).
[[nodiscard]] constexpr FloatDomain finite_float_domain() noexcept
{
    return FloatDomain{};
}
// [0, 1]: a weight, a coverage, a normalized parameter. -0 is 0 and allowed.
[[nodiscard]] constexpr FloatDomain unit_interval_domain() noexcept
{
    FloatDomain d{};
    d.name    = "unit-interval";
    d.minimum = 0.0;
    d.maximum = 1.0;
    return d;
}
// [0, +Inf]: a distance or a time where +Inf means "never" (a ray that hits nothing). NaN and -Inf are wrong.
[[nodiscard]] constexpr FloatDomain distance_domain() noexcept
{
    FloatDomain d{};
    d.name           = "distance";
    d.allow_infinity = true;
    d.minimum        = 0.0;
    return d;
}

// An integer domain: the closed range [minimum, maximum]. The defaults admit every i64. Valid when minimum <= maximum.
struct IntDomain
{
    const char* name    = "any-i64";
    crd::i64    minimum = std::numeric_limits<crd::i64>::min();
    crd::i64    maximum = std::numeric_limits<crd::i64>::max();
};

[[nodiscard]] ValueClass classify_value(crd::f32 v) noexcept;
[[nodiscard]] ValueClass classify_value(crd::f64 v) noexcept;

[[nodiscard]] bool domain_valid(const FloatDomain& d) noexcept;
[[nodiscard]] bool domain_valid(const IntDomain& d) noexcept;

// The ValueViolation bits of one value (0 = in the domain). Against an invalid domain only the class bits are set.
[[nodiscard]] crd::u32 value_violations(const FloatDomain& d, crd::f32 v) noexcept;
[[nodiscard]] crd::u32 value_violations(const FloatDomain& d, crd::f64 v) noexcept;
[[nodiscard]] crd::u32 value_violations(const IntDomain& d, crd::i64 v) noexcept;

inline constexpr crd::u64 kNoValueIndex = ~crd::u64{0};

// What a probe over a run of values found. `first_*` describe the first value with a violation; `first_bits` is its
// bit pattern (a binary32 zero-extended, a binary64, or an i64 as two's complement) so a report can name it exactly.
struct ValueDomainReport
{
    bool     domain_valid     = true;
    crd::u64 checked          = 0U; // values examined (0 against an invalid domain)
    crd::u64 violating        = 0U; // values with at least one violation
    crd::u32 violations       = 0U; // the union of every value's ValueViolation bits
    crd::u64 first_index      = kNoValueIndex;
    crd::u32 first_violations = 0U;
    crd::u64 first_bits       = 0U;
    [[nodiscard]] bool ok() const noexcept { return domain_valid && violating == 0U; }
};

[[nodiscard]] ValueDomainReport probe_values(const FloatDomain& d, const crd::f32* values, crd::usize count) noexcept;
[[nodiscard]] ValueDomainReport probe_values(const FloatDomain& d, const crd::f64* values, crd::usize count) noexcept;
[[nodiscard]] ValueDomainReport probe_values(const IntDomain& d, const crd::i64* values, crd::usize count) noexcept;

} // namespace crd
