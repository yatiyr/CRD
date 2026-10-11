#pragma once

// Queue and budget pressure: how full a bounded resource is, judged (DIAG.10c).
//
// A bounded resource (a job queue, a pool, a memory budget) fails in two ways a crash report does not explain: it
// runs full and refuses a request, or, for a soft budget, it is left above its bound. Before either, it runs near
// full. The owner of each resource reports a `BudgetReading` (its bound, what it holds now, the most it has held,
// how many requests it refused and how often it was left over its bound), and this probe judges it with integer
// arithmetic only: a level for one amount, and a verdict for the window between two readings of one resource
// (docs/design/runtime-diagnostics.md, DIAG.10c).
//
// Nothing is guessed. A reading that was not taken, or a resource with no bound, is judged as such and never as
// within its bound. A high water the owner does not keep, or one that predates the window, bounds the window's peak
// only from one side, and a verdict that would need the other side says it is undetermined.

#include <crd/core/types.hpp>

namespace crd
{

// One bounded resource at one moment.
struct BudgetReading
{
    crd::u64 capacity   = 0U; // the bound, when `bounded`
    crd::u64 in_use     = 0U; // held now
    crd::u64 high_water = 0U; // the most held at once since the resource began, when `high_water_tracked`
    crd::u64 refusals   = 0U; // requests refused because the resource was full (monotonic)
    crd::u64 overruns   = 0U; // times a soft bound was left exceeded (monotonic; a hard bound refuses instead)
    bool     read       = false; // the owner exists and was asked
    bool     bounded    = false; // a bound is set; false for a resource configured without one
    bool     high_water_tracked = false; // the owner maintains `high_water`
};

// The fraction of the bound, in thousandths, at which a resource counts as near full.
inline constexpr crd::u32 kDefaultNearPermille = 900U;

// One amount judged against a reading's bound.
// NOLINTNEXTLINE(performance-enum-size)
enum class PressureLevel : crd::u8
{
    Unread,    // no reading was taken: nothing is judged
    Unbounded, // the resource has no bound: its use is reported, never judged
    Within,    // below the near threshold
    Near,      // at or above the near threshold and below the bound
    Full,      // at the bound: the next request does not fit (a bound of 0 is full when empty)
    Over,      // above the bound (only a soft bound can be)
};
inline constexpr crd::usize kPressureLevelCount = 6U;
[[nodiscard]] const char* pressure_level_name(PressureLevel l) noexcept;

// Judge `amount` (usually `in_use` or `high_water`) against the bound of `r`. Near means
// amount * 1000 >= capacity * near_permille, computed exactly. A threshold outside 1..1000 is taken as 1000, so Near
// is then never reported.
[[nodiscard]] PressureLevel pressure_level(const BudgetReading& r, crd::u64 amount,
                                           crd::u32 near_permille = kDefaultNearPermille) noexcept;

// What a window between two readings of one resource shows, the worst finding last.
// NOLINTNEXTLINE(performance-enum-size)
enum class PressureVerdict : crd::u8
{
    Unjudged,     // a reading was not taken, the resource is unbounded, its bound changed or a count went back
    Relaxed,      // the window's peak is proven below the near threshold
    Undetermined, // the ends are below near, but the peak is not proven below it (an untracked high water, or one
                  // from before the window that is at or above near)
    Near,         // the window reached the near threshold
    Saturated,    // the window reached the bound
    Overrun,      // a soft bound was exceeded in the window, or left exceeded
    Exhausted,    // a request was refused in the window
};
inline constexpr crd::usize kPressureVerdictCount = 7U;
[[nodiscard]] const char* pressure_verdict_name(PressureVerdict v) noexcept;

struct PressureReport
{
    PressureVerdict verdict       = PressureVerdict::Unjudged;
    PressureLevel   level_now     = PressureLevel::Unread; // `after.in_use`
    crd::u64        peak_at_least = 0U;    // the window held at least this much at some moment
    crd::u64        peak_at_most  = 0U;    // ... and never more than this, when `peak_bounded`
    bool            peak_bounded  = false; // false: the high water is untracked, so the peak has no upper bound
    crd::u64        refused       = 0U;    // refusals in the window
    crd::u64        overran       = 0U;    // overruns in the window
};

// Judge the window from `before` to `after`, two readings of one resource with `before` taken first.
//
// The peak's lower bound is the larger end, raised to `after.high_water` when the high water rose in the window
// (the window must have reached it). Its upper bound is `after.high_water` when tracked (the window cannot exceed
// the all-time most); untracked, there is none. Worst first: a refusal is Exhausted; an overrun, or a lower bound
// above the bound, is Overrun; a lower bound at the bound is Saturated; at Near it is Near; an upper bound below
// Near is Relaxed; anything else is Undetermined.
[[nodiscard]] PressureReport pressure_report(const BudgetReading& before, const BudgetReading& after,
                                             crd::u32 near_permille = kDefaultNearPermille) noexcept;

// One line for a human or an agent, e.g. "injection.normal: exhausted (in use 2/2, peak 2, refused +1)". Writes at
// most `capacity` bytes including the terminating NUL (a longer line is cut) and returns the length written.
crd::usize format_pressure_report(const char* name, const PressureReport& report, const BudgetReading& after,
                                  char* out, crd::usize capacity) noexcept;

} // namespace crd
