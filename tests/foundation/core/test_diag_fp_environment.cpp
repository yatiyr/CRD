// DIAG.10c -- the numerical-mode probe (crd/core/fp_environment.hpp). A test thread starts in the declared mode; a
// scoped mode reads back field by field and is put back; each field the probe compares really changes a float result
// (so a mode difference is a wrong result, not a cosmetic one); and the comparison names exactly the fields that
// differ, ignores the sticky status flags and never counts an unread field as a match. ASCII test names (ctest
// by-name).

#include <crd/core/fp_environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
using crd::FpDifference;
using crd::FpEnvironment;
using crd::FpRounding;
using crd::u16;
using crd::u32;

constexpr u32 bits(FpDifference d)
{
    return static_cast<u32>(d);
}

#if CRD_ARCH_X64
FpEnvironment with_rounding(FpEnvironment e, FpRounding r)
{
    e.mxcsr = (e.mxcsr & ~crd::kMxcsrRoundingMask) | (static_cast<u32>(r) << crd::kMxcsrRoundingShift);
    return e;
}

float float_of(u32 b)
{
    float f = 0.0F;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

u32 bits_of(float f)
{
    u32 b = 0U;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

// Runtime float operations the compiler cannot fold: the operands come through volatile reads.
u32 one_third_bits()
{
    volatile float one   = 1.0F;
    volatile float three = 3.0F;
    volatile float q     = one / three;
    return bits_of(q);
}

u32 underflowing_product_bits()
{
    volatile float a = 1.0e-30F;
    volatile float b = 1.0e-10F;
    volatile float p = a * b; // 1e-40: below the smallest normal float (about 1.18e-38), so a subnormal result
    return bits_of(p);
}

u32 subnormal_input_bits()
{
    volatile float sub = float_of(0x00000100U); // a subnormal operand
    volatile float one = 1.0F;
    volatile float p   = sub * one;
    return bits_of(p);
}
#endif
} // namespace

TEST_CASE("diag10c: the test thread starts in the declared floating-point mode", "[core][fp][diag10c]")
{
    const FpEnvironment now      = crd::read_fp_environment();
    const FpEnvironment declared = crd::declared_fp_environment();
    INFO("mxcsr=" << now.mxcsr << " x87=" << now.x87_control << " x87_read=" << now.x87_read);
#if CRD_ARCH_X64
    CHECK(now.sse_read);
    CHECK(declared.mxcsr == crd::kMxcsrDefault);
#endif
    CHECK(now.x87_read == declared.x87_read);
    CHECK(crd::fp_differences(now, declared) == 0U);
    CHECK(now == declared);
    CHECK_FALSE(crd::fp_flush_to_zero(declared));
    CHECK_FALSE(crd::fp_denormals_are_zero(declared));
    CHECK(crd::fp_rounding(declared) == FpRounding::Nearest);
}

#if CRD_ARCH_X64
TEST_CASE("diag10c: a scoped floating-point mode reads back field by field and is put back", "[core][fp][diag10c]")
{
    const FpEnvironment before = crd::read_fp_environment();
    FpEnvironment       m      = with_rounding(before, FpRounding::TowardZero);
    m.mxcsr |= crd::kMxcsrFlushToZero | crd::kMxcsrDenormalsAreZero;
    if (m.x87_read)
    {
        // 24-bit precision, round up; the exception masks stay set
        m.x87_control = static_cast<u16>((m.x87_control & ~(crd::kX87PrecisionMask | crd::kX87RoundingMask)) |
                                         (2U << crd::kX87RoundingShift));
    }
    {
        const crd::ScopedFpEnvironment scope(m);
        const FpEnvironment            now = crd::read_fp_environment();
        CHECK(now == m);
        CHECK(crd::fp_flush_to_zero(now));
        CHECK(crd::fp_denormals_are_zero(now));
        CHECK(crd::fp_rounding(now) == FpRounding::TowardZero);
        u32 want =
            bits(FpDifference::FlushToZero) | bits(FpDifference::DenormalsAreZero) | bits(FpDifference::Rounding);
        if (now.x87_read)
        {
            want |= bits(FpDifference::X87Precision) | bits(FpDifference::X87Rounding);
        }
        CHECK(crd::fp_differences(now, before) == want);
        CHECK(crd::fp_differences(before, now) == want);
    }
    CHECK(crd::read_fp_environment() == before);
}

TEST_CASE("diag10c: every compared MXCSR field changes a float result", "[core][fp][diag10c]")
{
    const FpEnvironment declared = crd::declared_fp_environment();
    REQUIRE(crd::read_fp_environment() == declared);

    // Rounding: 1/3 is 0x3EAAAAAB rounded to nearest and 0x3EAAAAAA truncated.
    const u32 nearest = one_third_bits();
    CHECK(nearest == 0x3EAAAAABU);
    {
        const crd::ScopedFpEnvironment scope(with_rounding(declared, FpRounding::TowardZero));
        CHECK(one_third_bits() == 0x3EAAAAAAU);
    }
    {
        const crd::ScopedFpEnvironment scope(with_rounding(declared, FpRounding::Down));
        CHECK(one_third_bits() == 0x3EAAAAAAU);
    }
    {
        const crd::ScopedFpEnvironment scope(with_rounding(declared, FpRounding::Up));
        CHECK(one_third_bits() == 0x3EAAAAABU);
    }

    // Flush-to-zero: a subnormal result becomes zero.
    const u32 subnormal = underflowing_product_bits();
    CHECK(subnormal != 0U);
    CHECK(subnormal < 0x00800000U); // a subnormal, not a normal float
    {
        FpEnvironment ftz = declared;
        ftz.mxcsr |= crd::kMxcsrFlushToZero;
        const crd::ScopedFpEnvironment scope(ftz);
        CHECK(underflowing_product_bits() == 0U);
    }

    // Denormals-are-zero: a subnormal operand reads as zero.
    CHECK(subnormal_input_bits() == 0x00000100U);
    {
        FpEnvironment daz = declared;
        daz.mxcsr |= crd::kMxcsrDenormalsAreZero;
        const crd::ScopedFpEnvironment scope(daz);
        CHECK(subnormal_input_bits() == 0U);
    }
    CHECK(crd::read_fp_environment() == declared);
}
#endif

TEST_CASE("diag10c: the mode comparison names each field, ignores status flags and never matches an unread field",
          "[core][fp][diag10c]")
{
    FpEnvironment a{};
    a.sse_read    = true;
    a.mxcsr       = crd::kMxcsrDefault;
    a.x87_read    = true;
    a.x87_control = 0x037FU;
    CHECK(crd::fp_differences(a, a) == 0U);

    FpEnvironment status = a;
    status.mxcsr |= 0x3FU; // the six sticky exception flags: results of past operations, not a mode
    CHECK(crd::fp_differences(status, a) == 0U);

    FpEnvironment masks = a;
    masks.mxcsr &= ~(1U << 9U); // divide-by-zero unmasked
    CHECK(crd::fp_differences(masks, a) == bits(FpDifference::ExceptionMasks));

    FpEnvironment x87 = a;
    x87.x87_control = 0x027FU; // 53-bit precision
    CHECK(crd::fp_differences(x87, a) == bits(FpDifference::X87Precision));
    x87.x87_control = 0x037EU; // invalid-operation unmasked
    CHECK(crd::fp_differences(x87, a) == bits(FpDifference::X87ExceptionMasks));
    x87.x87_control = 0x077FU; // round down
    CHECK(crd::fp_differences(x87, a) == bits(FpDifference::X87Rounding));
    x87.x87_control = 0x137FU; // infinity control
    CHECK(crd::fp_differences(x87, a) == bits(FpDifference::X87Other));

    FpEnvironment unread = a;
    unread.x87_read = false;
    CHECK(crd::fp_differences(unread, a) == bits(FpDifference::Unread));
    CHECK(crd::fp_differences(a, unread) == bits(FpDifference::Unread));
    FpEnvironment none{};
    CHECK(crd::fp_differences(none, a) == bits(FpDifference::Unread));

    // Every difference has its own name.
    for (u32 i = 0U; i < crd::kFpDifferenceCount; ++i)
    {
        const char* name = crd::fp_difference_name(static_cast<FpDifference>(1U << i));
        INFO("bit " << i);
        CHECK(std::strcmp(name, "unknown") != 0);
        for (u32 j = 0U; j < i; ++j)
        {
            CHECK(std::strcmp(name, crd::fp_difference_name(static_cast<FpDifference>(1U << j))) != 0);
        }
    }
    CHECK(std::strcmp(crd::fp_rounding_name(FpRounding::Nearest), "nearest") == 0);
    CHECK(std::strcmp(crd::fp_rounding_name(FpRounding::TowardZero), "toward-zero") == 0);
}
