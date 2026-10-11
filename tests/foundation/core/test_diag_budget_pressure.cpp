// DIAG.10c budget pressure: the crd-core probe that judges a bounded resource's reading. Declared contract
// (docs/design/runtime-diagnostics.md, DIAG.10c): a level for one amount (Unread, Unbounded, Within, Near, Full,
// Over) with an exact integer near threshold, and a verdict for the window between two readings of one resource,
// worst first (Exhausted, Overrun, Saturated, Near, then Relaxed only when the high water proves it, else
// Undetermined; Unjudged when the readings cannot be compared). Every expected value is derived by hand below.

#include <crd/core/budget_pressure.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace
{
using crd::BudgetReading;
using crd::PressureLevel;
using crd::PressureVerdict;

BudgetReading bounded(crd::u64 capacity, crd::u64 in_use, bool tracked = true, crd::u64 high_water = 0U)
{
    BudgetReading r;
    r.read               = true;
    r.bounded            = true;
    r.capacity           = capacity;
    r.in_use             = in_use;
    r.high_water_tracked = tracked;
    if (tracked)
    {
        r.high_water = high_water > in_use ? high_water : in_use;
    }
    return r;
}

crd::PressureReport window(const BudgetReading& before, const BudgetReading& after)
{
    return crd::pressure_report(before, after);
}
} // namespace

TEST_CASE("diag10c: a pressure level names an unread or unbounded resource and never judges it",
          "[core][pressure][diag10c]")
{
    const BudgetReading unread;
    CHECK(crd::pressure_level(unread, 0U) == PressureLevel::Unread);
    CHECK(crd::pressure_level(unread, 5U) == PressureLevel::Unread);

    BudgetReading open;
    open.read   = true;
    open.in_use = 1U << 20U;
    CHECK(crd::pressure_level(open, open.in_use) == PressureLevel::Unbounded);
    CHECK(crd::pressure_level(open, UINT64_MAX) == PressureLevel::Unbounded);
}

TEST_CASE("diag10c: the near threshold is exact integer arithmetic", "[core][pressure][diag10c]")
{
    // capacity 10, 900 permille: near from 9.
    const BudgetReading ten = bounded(10U, 0U);
    CHECK(crd::pressure_level(ten, 0U) == PressureLevel::Within);
    CHECK(crd::pressure_level(ten, 8U) == PressureLevel::Within);
    CHECK(crd::pressure_level(ten, 9U) == PressureLevel::Near);
    CHECK(crd::pressure_level(ten, 10U) == PressureLevel::Full);
    CHECK(crd::pressure_level(ten, 11U) == PressureLevel::Over);

    // capacity 1001: 1001 * 0.9 = 900.9, so near from 901 (rounded up, never down).
    const BudgetReading odd = bounded(1001U, 0U);
    CHECK(crd::pressure_level(odd, 900U) == PressureLevel::Within);
    CHECK(crd::pressure_level(odd, 901U) == PressureLevel::Near);

    // The largest capacity: the product would overflow u64, the split form does not.
    // ceil((2^64 - 1) * 0.9) = 16602069666338596454 (q = 18446744073709551, r = 615: q * 900 + ceil(553.5)).
    const BudgetReading huge = bounded(UINT64_MAX, 0U);
    CHECK(crd::pressure_level(huge, 16602069666338596453ULL) == PressureLevel::Within);
    CHECK(crd::pressure_level(huge, 16602069666338596454ULL) == PressureLevel::Near);
    CHECK(crd::pressure_level(huge, UINT64_MAX) == PressureLevel::Full);

    // Other thresholds: 500 permille of 10 is 5; 1 permille of 10 rounds up to 1; 1000 makes Near unreachable.
    CHECK(crd::pressure_level(ten, 4U, 500U) == PressureLevel::Within);
    CHECK(crd::pressure_level(ten, 5U, 500U) == PressureLevel::Near);
    CHECK(crd::pressure_level(ten, 0U, 1U) == PressureLevel::Within);
    CHECK(crd::pressure_level(ten, 1U, 1U) == PressureLevel::Near);
    CHECK(crd::pressure_level(ten, 9U, 1000U) == PressureLevel::Within);
    // A threshold outside 1..1000 is taken as 1000.
    CHECK(crd::pressure_level(ten, 9U, 0U) == PressureLevel::Within);
    CHECK(crd::pressure_level(ten, 9U, 5000U) == PressureLevel::Within);
    CHECK(crd::pressure_level(ten, 10U, 0U) == PressureLevel::Full);

    // A bound of 0 is full when empty and over with anything in it.
    const BudgetReading zero = bounded(0U, 0U);
    CHECK(crd::pressure_level(zero, 0U) == PressureLevel::Full);
    CHECK(crd::pressure_level(zero, 1U) == PressureLevel::Over);
}

TEST_CASE("diag10c: a pressure window is unjudged when the readings cannot be compared", "[core][pressure][diag10c]")
{
    const BudgetReading good = bounded(10U, 2U);

    const BudgetReading unread;
    CHECK(window(unread, good).verdict == PressureVerdict::Unjudged);
    CHECK(window(good, unread).verdict == PressureVerdict::Unjudged);
    CHECK(window(good, unread).level_now == PressureLevel::Unread);

    BudgetReading open = good;
    open.bounded       = false;
    CHECK(window(open, open).verdict == PressureVerdict::Unjudged);
    CHECK(window(open, open).level_now == PressureLevel::Unbounded);

    const BudgetReading other_bound = bounded(20U, 2U);
    CHECK(window(good, other_bound).verdict == PressureVerdict::Unjudged);

    BudgetReading untracked     = good;
    untracked.high_water_tracked = false;
    CHECK(window(good, untracked).verdict == PressureVerdict::Unjudged);

    BudgetReading refused = good;
    refused.refusals      = 3U;
    CHECK(window(refused, good).verdict == PressureVerdict::Unjudged); // a monotonic count went back

    BudgetReading overran = good;
    overran.overruns      = 1U;
    CHECK(window(overran, good).verdict == PressureVerdict::Unjudged);

    const BudgetReading high = bounded(10U, 2U, true, 7U);
    CHECK(window(high, good).verdict == PressureVerdict::Unjudged); // the high water went back
}

TEST_CASE("diag10c: a pressure window names the worst thing it saw", "[core][pressure][diag10c]")
{
    const BudgetReading start = bounded(10U, 0U);

    // Exhausted outranks everything: a refusal while the ends look calm.
    BudgetReading refused = bounded(10U, 1U, true, 10U);
    refused.refusals      = 1U;
    refused.overruns      = 1U;
    const crd::PressureReport ex = window(start, refused);
    CHECK(ex.verdict == PressureVerdict::Exhausted);
    CHECK(ex.refused == 1U);
    CHECK(ex.overran == 1U);
    CHECK(ex.level_now == PressureLevel::Within);

    // Overrun: an overrun counted while the ends are within, or an end above the bound.
    BudgetReading left_over = bounded(10U, 3U, true, 3U);
    left_over.overruns      = 2U;
    CHECK(window(start, left_over).verdict == PressureVerdict::Overrun);
    const crd::PressureReport above = window(start, bounded(10U, 12U));
    CHECK(above.verdict == PressureVerdict::Overrun);
    CHECK(above.level_now == PressureLevel::Over);
    CHECK(above.peak_at_least == 12U);

    // A high water that rose in the window is a peak the window reached.
    const crd::PressureReport sat = window(start, bounded(10U, 1U, true, 10U));
    CHECK(sat.verdict == PressureVerdict::Saturated);
    CHECK(sat.peak_at_least == 10U);
    CHECK(sat.peak_at_most == 10U);
    CHECK(sat.peak_bounded);

    const crd::PressureReport near = window(start, bounded(10U, 0U, true, 9U));
    CHECK(near.verdict == PressureVerdict::Near);
    CHECK(near.peak_at_least == 9U);

    // An end at the bound is saturated even with no high water kept.
    CHECK(window(bounded(10U, 0U, false), bounded(10U, 10U, false)).verdict == PressureVerdict::Saturated);
}

TEST_CASE("diag10c: a pressure window is relaxed only when the high water proves it", "[core][pressure][diag10c]")
{
    // Tracked and low throughout: the window's peak is at most 4 of 10.
    const crd::PressureReport relaxed = window(bounded(10U, 0U, true, 2U), bounded(10U, 1U, true, 4U));
    CHECK(relaxed.verdict == PressureVerdict::Relaxed);
    CHECK(relaxed.peak_at_least == 4U);
    CHECK(relaxed.peak_at_most == 4U);

    // A high water from before the window that is near: the window may or may not have gone there.
    const crd::PressureReport before_window = window(bounded(10U, 1U, true, 9U), bounded(10U, 2U, true, 9U));
    CHECK(before_window.verdict == PressureVerdict::Undetermined);
    CHECK(before_window.peak_at_least == 2U);
    CHECK(before_window.peak_at_most == 9U);

    // No high water kept: low ends prove nothing about the middle.
    const crd::PressureReport untracked = window(bounded(10U, 0U, false), bounded(10U, 1U, false));
    CHECK(untracked.verdict == PressureVerdict::Undetermined);
    CHECK_FALSE(untracked.peak_bounded);
    CHECK(untracked.peak_at_least == 1U);
}

TEST_CASE("diag10c: a pressure report renders one line", "[core][pressure][diag10c]")
{
    char line[128] = {};

    BudgetReading after = bounded(4U, 4U, false);
    after.refusals      = 1U;
    const crd::PressureReport ex = window(bounded(4U, 0U, false), after);
    const crd::usize written = crd::format_pressure_report("injection.high", ex, after, line, sizeof(line));
    CHECK(written == std::strlen(line));
    CHECK(std::strcmp(line, "injection.high: exhausted (in use 4/4, peak 4+, refused +1)") == 0);

    const BudgetReading       mid   = bounded(10U, 2U, true, 9U);
    const crd::PressureReport range = window(bounded(10U, 1U, true, 9U), mid);
    (void)crd::format_pressure_report("counters", range, mid, line, sizeof(line));
    CHECK(std::strcmp(line, "counters: undetermined (in use 2/10, peak 2..9)") == 0);

    BudgetReading over = bounded(100U, 150U);
    over.overruns      = 1U;
    const crd::PressureReport ov = window(bounded(100U, 0U), over);
    (void)crd::format_pressure_report("", ov, over, line, sizeof(line));
    CHECK(std::strcmp(line, "overrun (in use 150/100, peak 150, overran +1)") == 0);

    BudgetReading open;
    open.read   = true;
    open.in_use = 7U;
    (void)crd::format_pressure_report("memory", window(open, open), open, line, sizeof(line));
    CHECK(std::strcmp(line, "memory: unjudged (in use 7, unbounded)") == 0);

    const BudgetReading unread;
    (void)crd::format_pressure_report(nullptr, window(unread, unread), unread, line, sizeof(line));
    CHECK(std::strcmp(line, "unjudged (not read)") == 0);

    // A short buffer is cut, NUL-terminated, and the length is what was written.
    char small[12] = {};
    CHECK(crd::format_pressure_report("counters", range, mid, small, sizeof(small)) == 11U);
    CHECK(std::strcmp(small, "counters: u") == 0);
    CHECK(crd::format_pressure_report("counters", range, mid, nullptr, 8U) == 0U);
}

TEST_CASE("diag10c: pressure names are stable", "[core][pressure][diag10c]")
{
    CHECK(std::strcmp(crd::pressure_level_name(PressureLevel::Unread), "unread") == 0);
    CHECK(std::strcmp(crd::pressure_level_name(PressureLevel::Unbounded), "unbounded") == 0);
    CHECK(std::strcmp(crd::pressure_level_name(PressureLevel::Within), "within") == 0);
    CHECK(std::strcmp(crd::pressure_level_name(PressureLevel::Near), "near") == 0);
    CHECK(std::strcmp(crd::pressure_level_name(PressureLevel::Full), "full") == 0);
    CHECK(std::strcmp(crd::pressure_level_name(PressureLevel::Over), "over") == 0);
    CHECK(std::strcmp(crd::pressure_verdict_name(PressureVerdict::Unjudged), "unjudged") == 0);
    CHECK(std::strcmp(crd::pressure_verdict_name(PressureVerdict::Relaxed), "relaxed") == 0);
    CHECK(std::strcmp(crd::pressure_verdict_name(PressureVerdict::Undetermined), "undetermined") == 0);
    CHECK(std::strcmp(crd::pressure_verdict_name(PressureVerdict::Near), "near") == 0);
    CHECK(std::strcmp(crd::pressure_verdict_name(PressureVerdict::Saturated), "saturated") == 0);
    CHECK(std::strcmp(crd::pressure_verdict_name(PressureVerdict::Overrun), "overrun") == 0);
    CHECK(std::strcmp(crd::pressure_verdict_name(PressureVerdict::Exhausted), "exhausted") == 0);
}
