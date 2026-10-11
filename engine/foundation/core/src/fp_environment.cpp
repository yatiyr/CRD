#include <crd/core/fp_environment.hpp>

#if CRD_ARCH_X64
#include <xmmintrin.h> // _mm_getcsr / _mm_setcsr
#endif

// GNU inline assembly reads and writes the x87 control word; MSVC on x64 has none and compiles no x87 arithmetic.
#if CRD_ARCH_X64 && (defined(__GNUC__) || defined(__clang__))
#define CRD_FP_X87_READABLE
#endif

namespace crd
{

namespace
{
#ifdef CRD_FP_X87_READABLE
crd::u16 read_x87_control() noexcept
{
    crd::u16 cw = 0U;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw;
}

void write_x87_control(crd::u16 cw) noexcept
{
    __asm__ volatile("fldcw %0" : : "m"(cw));
}
#endif
} // namespace

const char* fp_rounding_name(FpRounding r) noexcept
{
    switch (r)
    {
    case FpRounding::Nearest:
        return "nearest";
    case FpRounding::Down:
        return "down";
    case FpRounding::Up:
        return "up";
    case FpRounding::TowardZero:
        return "toward-zero";
    }
    return "unknown";
}

FpEnvironment read_fp_environment() noexcept
{
    FpEnvironment e{};
#if CRD_ARCH_X64
    e.sse_read = true;
    e.mxcsr    = _mm_getcsr() & kMxcsrControlMask;
#endif
#ifdef CRD_FP_X87_READABLE
    e.x87_read    = true;
    e.x87_control = read_x87_control();
#endif
    return e;
}

FpEnvironment declared_fp_environment() noexcept
{
    FpEnvironment e{};
#if CRD_ARCH_X64
    e.sse_read = true;
    e.mxcsr    = kMxcsrDefault;
#endif
#ifdef CRD_FP_X87_READABLE
    e.x87_read    = true;
    e.x87_control = kX87ControlDefault;
#endif
    return e;
}

void write_fp_environment(const FpEnvironment& e) noexcept
{
#if CRD_ARCH_X64
    if (e.sse_read)
    {
        _mm_setcsr((_mm_getcsr() & ~kMxcsrControlMask) | (e.mxcsr & kMxcsrControlMask));
    }
#endif
#ifdef CRD_FP_X87_READABLE
    if (e.x87_read)
    {
        write_x87_control(e.x87_control);
    }
#endif
    static_cast<void>(e);
}

const char* fp_difference_name(FpDifference d) noexcept
{
    switch (d)
    {
    case FpDifference::FlushToZero:
        return "flush-to-zero";
    case FpDifference::DenormalsAreZero:
        return "denormals-are-zero";
    case FpDifference::Rounding:
        return "rounding";
    case FpDifference::ExceptionMasks:
        return "exception-masks";
    case FpDifference::X87Precision:
        return "x87-precision";
    case FpDifference::X87Rounding:
        return "x87-rounding";
    case FpDifference::X87ExceptionMasks:
        return "x87-exception-masks";
    case FpDifference::X87Other:
        return "x87-other";
    case FpDifference::Unread:
        return "unread";
    }
    return "unknown";
}

crd::u32 fp_differences(const FpEnvironment& actual, const FpEnvironment& expected) noexcept
{
    crd::u32 d = 0U;
    const auto add = [&d](FpDifference f) noexcept
    {
        d |= static_cast<crd::u32>(f);
    };
    if (actual.sse_read != expected.sse_read || actual.x87_read != expected.x87_read)
    {
        add(FpDifference::Unread);
    }
    if (actual.sse_read && expected.sse_read)
    {
        const crd::u32 x = (actual.mxcsr ^ expected.mxcsr) & kMxcsrControlMask;
        if ((x & kMxcsrFlushToZero) != 0U)
        {
            add(FpDifference::FlushToZero);
        }
        if ((x & kMxcsrDenormalsAreZero) != 0U)
        {
            add(FpDifference::DenormalsAreZero);
        }
        if ((x & kMxcsrRoundingMask) != 0U)
        {
            add(FpDifference::Rounding);
        }
        if ((x & kMxcsrExceptionMasks) != 0U)
        {
            add(FpDifference::ExceptionMasks);
        }
    }
    if (actual.x87_read && expected.x87_read)
    {
        const auto x = static_cast<crd::u16>(actual.x87_control ^ expected.x87_control);
        if ((x & kX87PrecisionMask) != 0U)
        {
            add(FpDifference::X87Precision);
        }
        if ((x & kX87RoundingMask) != 0U)
        {
            add(FpDifference::X87Rounding);
        }
        if ((x & kX87ExceptionMasks) != 0U)
        {
            add(FpDifference::X87ExceptionMasks);
        }
        if ((x & static_cast<crd::u16>(~(kX87PrecisionMask | kX87RoundingMask | kX87ExceptionMasks))) != 0U)
        {
            add(FpDifference::X87Other);
        }
    }
    return d;
}

} // namespace crd
