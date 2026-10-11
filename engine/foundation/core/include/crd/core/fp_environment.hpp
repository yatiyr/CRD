#pragma once

// The floating-point mode a thread runs in, read as data: the numerical-mode probe of DIAG.10c.
//
// Float results depend on more than the code: flush-to-zero (FTZ), denormals-are-zero (DAZ), the rounding mode and the
// exception masks are per-thread hardware state that any library, driver or job can change. A result computed under
// another mode is wrong without crashing, so this header lets a boundary compare the mode it runs in with the one it
// declares (docs/design/runtime-diagnostics.md, DIAG.10c). It reads and compares only; it never changes a computation.
//
// Only CONTROL state is a mode. The sticky exception flags (MXCSR bits 0-5, the x87 status word) record what earlier
// operations did, and the x86-64 psABI makes them caller-saved, so they are not compared.
//
// What can be read: on x86-64 every compiler reads MXCSR, which governs all SSE/AVX float and double arithmetic. The
// x87 control word governs only x87 arithmetic, which on Linux is `long double`; MSVC compiles no x87 arithmetic on
// x64 and has no inline assembly to read it, so it is read only where GNU inline assembly exists (gcc, clang,
// clang-cl). Other architectures read nothing yet: AArch64's FPCR belongs to JOBS-PORT, and an unread field is
// reported as unread, never as a match.

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <utility>

namespace crd
{

// MXCSR control bits: 6 DAZ, 7-12 exception masks, 13-14 rounding control, 15 FTZ. Bits 0-5 are status flags.
inline constexpr crd::u32 kMxcsrControlMask      = 0xFFC0U;
inline constexpr crd::u32 kMxcsrDenormalsAreZero = 1U << 6U;
inline constexpr crd::u32 kMxcsrExceptionMasks   = 0x3FU << 7U;
inline constexpr crd::u32 kMxcsrRoundingShift    = 13U;
inline constexpr crd::u32 kMxcsrRoundingMask     = 3U << kMxcsrRoundingShift;
inline constexpr crd::u32 kMxcsrFlushToZero      = 1U << 15U;
// The power-on and thread-start value on both platforms: every exception masked, round to nearest, no FTZ or DAZ.
inline constexpr crd::u32 kMxcsrDefault = 0x1F80U;

// x87 control word: 0-5 exception masks, 8-9 precision control, 10-11 rounding control, 12 infinity control (ignored
// since the 80387, kept as written). It has no status bits.
inline constexpr crd::u16 kX87ExceptionMasks = 0x003FU;
inline constexpr crd::u16 kX87PrecisionMask  = 0x0300U;
inline constexpr crd::u16 kX87RoundingShift  = 10U;
inline constexpr crd::u16 kX87RoundingMask   = 0x0C00U;
// The thread-start value differs by platform: the Windows CRT selects 53-bit precision (0x027F), glibc 64-bit (0x037F).
#if CRD_OS_WINDOWS
inline constexpr crd::u16 kX87ControlDefault = 0x027FU;
#else
inline constexpr crd::u16 kX87ControlDefault = 0x037FU;
#endif

// NOLINTNEXTLINE(performance-enum-size)
enum class FpRounding : crd::u8
{
    Nearest,    // round to nearest, ties to even
    Down,       // toward negative infinity
    Up,         // toward positive infinity
    TowardZero, // truncate
};
[[nodiscard]] const char* fp_rounding_name(FpRounding r) noexcept;

// The control state of the calling thread's floating-point units.
struct FpEnvironment
{
    bool     sse_read    = false; // MXCSR was read (x86-64)
    crd::u32 mxcsr       = 0U;    // its control bits only (kMxcsrControlMask)
    bool     x87_read    = false; // the x87 control word was read (x86-64 with GNU inline assembly)
    crd::u16 x87_control = 0U;

    friend bool operator==(const FpEnvironment&, const FpEnvironment&) = default;
};

[[nodiscard]] constexpr bool fp_flush_to_zero(const FpEnvironment& e) noexcept
{
    return (e.mxcsr & kMxcsrFlushToZero) != 0U;
}
[[nodiscard]] constexpr bool fp_denormals_are_zero(const FpEnvironment& e) noexcept
{
    return (e.mxcsr & kMxcsrDenormalsAreZero) != 0U;
}
[[nodiscard]] constexpr FpRounding fp_rounding(const FpEnvironment& e) noexcept
{
    return static_cast<FpRounding>((e.mxcsr & kMxcsrRoundingMask) >> kMxcsrRoundingShift);
}

// The calling thread's mode now.
[[nodiscard]] FpEnvironment read_fp_environment() noexcept;

// The declared mode (DIAG.10c): what a new thread starts in and what every crd-jobs job starts in, whatever mode the
// thread that submitted or runs it is in. Its read flags are the ones read_fp_environment() sets on this build, so a
// field this build cannot read is unread on both sides.
[[nodiscard]] FpEnvironment declared_fp_environment() noexcept;

// Set the calling thread's mode to `e`'s read fields (the status flags stay as they are). An unread field is left
// alone. For code that needs a mode for a bounded region: prefer ScopedFpEnvironment, which puts the old one back.
void write_fp_environment(const FpEnvironment& e) noexcept;

// How two modes differ, one bit per field.
// NOLINTNEXTLINE(performance-enum-size)
enum class FpDifference : crd::u32
{
    FlushToZero       = 1U << 0U,
    DenormalsAreZero  = 1U << 1U,
    Rounding          = 1U << 2U,
    ExceptionMasks    = 1U << 3U,
    X87Precision      = 1U << 4U,
    X87Rounding       = 1U << 5U,
    X87ExceptionMasks = 1U << 6U,
    X87Other          = 1U << 7U, // the reserved and infinity-control bits
    Unread            = 1U << 8U, // a field read on one side only: never counted as a match
};
inline constexpr crd::u32 kFpDifferenceCount = 9U;
[[nodiscard]] const char* fp_difference_name(FpDifference d) noexcept;

// The fields in which `actual` differs from `expected`, as FpDifference bits (0 = the same mode).
[[nodiscard]] crd::u32 fp_differences(const FpEnvironment& actual, const FpEnvironment& expected) noexcept;

// Sets a mode for a scope and puts the previous one back when it ends, also when a test assertion leaves early.
class ScopedFpEnvironment
{
public:
    explicit ScopedFpEnvironment(const FpEnvironment& e) noexcept : m_saved(read_fp_environment())
    {
        write_fp_environment(e);
    }
    ~ScopedFpEnvironment() { write_fp_environment(m_saved); }
    ScopedFpEnvironment(const ScopedFpEnvironment&)            = delete;
    ScopedFpEnvironment& operator=(const ScopedFpEnvironment&) = delete;
    ScopedFpEnvironment(ScopedFpEnvironment&&)                 = delete;
    ScopedFpEnvironment& operator=(ScopedFpEnvironment&&)      = delete;

private:
    FpEnvironment m_saved;
};

// Call code the engine does not own (a framework's DLPack deleter, a driver, a user callback) and keep a mode it sets
// from reaching the caller. Returns the FpDifference bits between the mode the call left and the mode before it
// (0 = untouched) and, when they differ, puts the mode from before the call back. A foreign call that changes the
// thread's mode is a defect in that code; the caller decides how to report it.
template <typename Call> [[nodiscard]] crd::u32 call_keeping_fp_environment(Call&& call)
{
    const FpEnvironment before = read_fp_environment();
    std::forward<Call>(call)();
    const crd::u32 changed = fp_differences(read_fp_environment(), before);
    if (changed != 0U)
    {
        write_fp_environment(before);
    }
    return changed;
}

} // namespace crd
