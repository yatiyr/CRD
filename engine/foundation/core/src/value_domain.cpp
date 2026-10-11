#include <crd/core/value_domain.hpp>

#include <cstring>

// Integer operations on bit patterns only: no floating-point instruction reads a value, so flush-to-zero,
// denormals-are-zero and the rounding mode cannot change a verdict (see the header).

namespace crd
{

namespace
{
constexpr crd::u64 kF64Sign     = 1ULL << 63U;
constexpr crd::u64 kF64ExpMask  = 0x7FFULL << 52U;
constexpr crd::u64 kF64ManMask  = (1ULL << 52U) - 1ULL;
constexpr crd::u32 kF32Sign     = 1U << 31U;
constexpr crd::u32 kF32ExpMask  = 0xFFU << 23U;
constexpr crd::u32 kF32ManMask  = (1U << 23U) - 1U;
constexpr crd::u32 kF32Hidden   = 1U << 23U;
constexpr crd::u32 kF32ManShift = 52U - 23U; // binary32 fraction bits move up to the top of the binary64 fraction

crd::u64 bits_of(crd::f64 v) noexcept
{
    crd::u64 b = 0U;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

crd::u32 bits_of(crd::f32 v) noexcept
{
    crd::u32 b = 0U;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

constexpr crd::u32 bit(ValueViolation v) noexcept
{
    return static_cast<crd::u32>(v);
}

ValueClass classify_bits(crd::u64 b) noexcept
{
    const bool     negative = (b & kF64Sign) != 0U;
    const crd::u64 exponent = b & kF64ExpMask;
    const crd::u64 mantissa = b & kF64ManMask;
    if (exponent == kF64ExpMask)
    {
        if (mantissa != 0U)
        {
            return ValueClass::NaN;
        }
        return negative ? ValueClass::NegativeInfinity : ValueClass::Infinity;
    }
    if (exponent == 0U)
    {
        if (mantissa != 0U)
        {
            return ValueClass::Subnormal;
        }
        return negative ? ValueClass::NegativeZero : ValueClass::Zero;
    }
    return ValueClass::Normal;
}

// The class of a binary32 value from its own bits: a binary32 subnormal is a normal binary64 value once widened.
ValueClass classify_bits32(crd::u32 b) noexcept
{
    const bool     negative = (b & kF32Sign) != 0U;
    const crd::u32 exponent = b & kF32ExpMask;
    const crd::u32 mantissa = b & kF32ManMask;
    if (exponent == kF32ExpMask)
    {
        if (mantissa != 0U)
        {
            return ValueClass::NaN;
        }
        return negative ? ValueClass::NegativeInfinity : ValueClass::Infinity;
    }
    if (exponent == 0U)
    {
        if (mantissa != 0U)
        {
            return ValueClass::Subnormal;
        }
        return negative ? ValueClass::NegativeZero : ValueClass::Zero;
    }
    return ValueClass::Normal;
}

// The exact binary64 bit pattern of a binary32 value. Every binary32 value is a binary64 value; a binary32 subnormal
// is normal in binary64, so its fraction is shifted up to the hidden bit and the exponent lowered by the shift.
crd::u64 widen_bits(crd::u32 b) noexcept
{
    const crd::u64 sign     = (b & kF32Sign) != 0U ? kF64Sign : 0U;
    const crd::u32 exponent = (b & kF32ExpMask) >> 23U;
    crd::u32       mantissa = b & kF32ManMask;
    if (exponent == 0xFFU)
    {
        return sign | kF64ExpMask | (static_cast<crd::u64>(mantissa) << kF32ManShift);
    }
    if (exponent == 0U)
    {
        if (mantissa == 0U)
        {
            return sign;
        }
        crd::i32 e = -126;
        while ((mantissa & kF32Hidden) == 0U)
        {
            mantissa <<= 1U;
            --e;
        }
        mantissa &= kF32ManMask;
        return sign | (static_cast<crd::u64>(e + 1023) << 52U) | (static_cast<crd::u64>(mantissa) << kF32ManShift);
    }
    const crd::u64 rebased = static_cast<crd::u64>(exponent) - 127U + 1023U;
    return sign | (rebased << 52U) | (static_cast<crd::u64>(mantissa) << kF32ManShift);
}

// A key whose unsigned order is the numeric order of non-NaN binary64 values, with -0 and +0 equal.
crd::u64 order_key(crd::u64 b) noexcept
{
    if (b == kF64Sign)
    {
        b = 0U; // -0 compares as +0
    }
    if ((b & kF64Sign) != 0U)
    {
        return ~b;
    }
    return b | kF64Sign;
}

bool is_nan_bits(crd::u64 b) noexcept
{
    return (b & kF64ExpMask) == kF64ExpMask && (b & kF64ManMask) != 0U;
}

bool float_domain_valid(const FloatDomain& d) noexcept
{
    const crd::u64 lo = bits_of(d.minimum);
    const crd::u64 hi = bits_of(d.maximum);
    if (is_nan_bits(lo) || is_nan_bits(hi))
    {
        return false;
    }
    return order_key(lo) <= order_key(hi);
}

// The violations of a value of class `cls` whose binary64 bit pattern is `b` (a binary32 value already widened, its
// class read from its own bits). `range` is false for an invalid domain.
crd::u32 float_violations(const FloatDomain& d, ValueClass cls, crd::u64 b, bool range) noexcept
{
    crd::u32 out = 0U;
    switch (cls)
    {
    case ValueClass::NaN:
        // A NaN has no place on the line: only its class is judged.
        return d.allow_nan ? 0U : bit(ValueViolation::NaN);
    case ValueClass::Infinity:
        if (!d.allow_infinity)
        {
            out |= bit(ValueViolation::Infinity);
        }
        break;
    case ValueClass::NegativeInfinity:
        if (!d.allow_negative_infinity)
        {
            out |= bit(ValueViolation::NegativeInfinity);
        }
        break;
    case ValueClass::Subnormal:
        if (!d.allow_subnormal)
        {
            out |= bit(ValueViolation::Subnormal);
        }
        break;
    case ValueClass::NegativeZero:
        if (!d.allow_negative_zero)
        {
            out |= bit(ValueViolation::NegativeZero);
        }
        break;
    case ValueClass::Zero:
    case ValueClass::Normal:
        break;
    }
    if (range)
    {
        const crd::u64 key = order_key(b);
        if (key < order_key(bits_of(d.minimum)))
        {
            out |= bit(ValueViolation::BelowMinimum);
        }
        if (key > order_key(bits_of(d.maximum)))
        {
            out |= bit(ValueViolation::AboveMaximum);
        }
    }
    return out;
}

void note(ValueDomainReport& r, crd::u64 index, crd::u32 violations, crd::u64 bits) noexcept
{
    ++r.checked;
    if (violations == 0U)
    {
        return;
    }
    if (r.violating == 0U)
    {
        r.first_index      = index;
        r.first_violations = violations;
        r.first_bits       = bits;
    }
    ++r.violating;
    r.violations |= violations;
}
} // namespace

const char* value_class_name(ValueClass c) noexcept
{
    switch (c)
    {
    case ValueClass::Zero:
        return "zero";
    case ValueClass::NegativeZero:
        return "negative-zero";
    case ValueClass::Subnormal:
        return "subnormal";
    case ValueClass::Normal:
        return "normal";
    case ValueClass::Infinity:
        return "infinity";
    case ValueClass::NegativeInfinity:
        return "negative-infinity";
    case ValueClass::NaN:
        return "nan";
    }
    return "unknown";
}

const char* value_violation_name(ValueViolation v) noexcept
{
    switch (v)
    {
    case ValueViolation::NaN:
        return "nan";
    case ValueViolation::Infinity:
        return "infinity";
    case ValueViolation::NegativeInfinity:
        return "negative-infinity";
    case ValueViolation::Subnormal:
        return "subnormal";
    case ValueViolation::NegativeZero:
        return "negative-zero";
    case ValueViolation::BelowMinimum:
        return "below-minimum";
    case ValueViolation::AboveMaximum:
        return "above-maximum";
    }
    return "unknown";
}

ValueClass classify_value(crd::f32 v) noexcept
{
    return classify_bits32(bits_of(v));
}

ValueClass classify_value(crd::f64 v) noexcept
{
    return classify_bits(bits_of(v));
}

bool domain_valid(const FloatDomain& d) noexcept
{
    return float_domain_valid(d);
}

bool domain_valid(const IntDomain& d) noexcept
{
    return d.minimum <= d.maximum;
}

crd::u32 value_violations(const FloatDomain& d, crd::f32 v) noexcept
{
    const crd::u32 raw = bits_of(v);
    return float_violations(d, classify_bits32(raw), widen_bits(raw), float_domain_valid(d));
}

crd::u32 value_violations(const FloatDomain& d, crd::f64 v) noexcept
{
    const crd::u64 raw = bits_of(v);
    return float_violations(d, classify_bits(raw), raw, float_domain_valid(d));
}

crd::u32 value_violations(const IntDomain& d, crd::i64 v) noexcept
{
    if (!domain_valid(d))
    {
        return 0U;
    }
    if (v < d.minimum)
    {
        return bit(ValueViolation::BelowMinimum);
    }
    if (v > d.maximum)
    {
        return bit(ValueViolation::AboveMaximum);
    }
    return 0U;
}

ValueDomainReport probe_values(const FloatDomain& d, const crd::f32* values, crd::usize count) noexcept
{
    ValueDomainReport r{};
    r.domain_valid = float_domain_valid(d);
    if (!r.domain_valid)
    {
        return r;
    }
    for (crd::usize i = 0U; i < count; ++i)
    {
        const crd::u32 raw = bits_of(values[i]);
        note(r, i, float_violations(d, classify_bits32(raw), widen_bits(raw), true), raw);
    }
    return r;
}

ValueDomainReport probe_values(const FloatDomain& d, const crd::f64* values, crd::usize count) noexcept
{
    ValueDomainReport r{};
    r.domain_valid = float_domain_valid(d);
    if (!r.domain_valid)
    {
        return r;
    }
    for (crd::usize i = 0U; i < count; ++i)
    {
        const crd::u64 raw = bits_of(values[i]);
        note(r, i, float_violations(d, classify_bits(raw), raw, true), raw);
    }
    return r;
}

ValueDomainReport probe_values(const IntDomain& d, const crd::i64* values, crd::usize count) noexcept
{
    ValueDomainReport r{};
    r.domain_valid = domain_valid(d);
    if (!r.domain_valid)
    {
        return r;
    }
    for (crd::usize i = 0U; i < count; ++i)
    {
        note(r, i, value_violations(d, values[i]), static_cast<crd::u64>(values[i]));
    }
    return r;
}

} // namespace crd
