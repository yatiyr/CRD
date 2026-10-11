#include <crd/core/budget_pressure.hpp>

#include <cstdio>

namespace crd
{

namespace
{
// The smallest amount at or above the near threshold: ceil(capacity * near_permille / 1000), formed without a
// product that could overflow (capacity = q * 1000 + r, so the product is q * near * 1000 + r * near).
crd::u64 near_threshold(crd::u64 capacity, crd::u32 near_permille) noexcept
{
    const crd::u64 near = (near_permille == 0U || near_permille > 1000U) ? 1000U : near_permille;
    const crd::u64 q    = capacity / 1000U;
    const crd::u64 r    = capacity % 1000U;
    const crd::u64 rest = r * near; // < 10^6
    return (q * near) + (rest / 1000U) + ((rest % 1000U) != 0U ? 1U : 0U);
}

crd::u64 larger(crd::u64 a, crd::u64 b) noexcept
{
    return a > b ? a : b;
}

// Append `text` to [out, out + capacity), keeping room for the NUL; returns the new length.
crd::usize append(char* out, crd::usize capacity, crd::usize length, const char* text) noexcept
{
    if (capacity == 0U)
    {
        return length;
    }
    while (*text != '\0' && length + 1U < capacity)
    {
        out[length] = *text;
        ++length;
        ++text;
    }
    out[length] = '\0';
    return length;
}

crd::usize append_u64(char* out, crd::usize capacity, crd::usize length, crd::u64 value) noexcept
{
    char digits[24] = {};
    (void)std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(value));
    return append(out, capacity, length, digits);
}
} // namespace

const char* pressure_level_name(PressureLevel l) noexcept
{
    switch (l)
    {
    case PressureLevel::Unread:
        return "unread";
    case PressureLevel::Unbounded:
        return "unbounded";
    case PressureLevel::Within:
        return "within";
    case PressureLevel::Near:
        return "near";
    case PressureLevel::Full:
        return "full";
    case PressureLevel::Over:
        return "over";
    }
    return "unknown";
}

const char* pressure_verdict_name(PressureVerdict v) noexcept
{
    switch (v)
    {
    case PressureVerdict::Unjudged:
        return "unjudged";
    case PressureVerdict::Relaxed:
        return "relaxed";
    case PressureVerdict::Undetermined:
        return "undetermined";
    case PressureVerdict::Near:
        return "near";
    case PressureVerdict::Saturated:
        return "saturated";
    case PressureVerdict::Overrun:
        return "overrun";
    case PressureVerdict::Exhausted:
        return "exhausted";
    }
    return "unknown";
}

PressureLevel pressure_level(const BudgetReading& r, crd::u64 amount, crd::u32 near_permille) noexcept
{
    if (!r.read)
    {
        return PressureLevel::Unread;
    }
    if (!r.bounded)
    {
        return PressureLevel::Unbounded;
    }
    if (amount > r.capacity)
    {
        return PressureLevel::Over;
    }
    if (amount == r.capacity)
    {
        return PressureLevel::Full;
    }
    if (amount >= near_threshold(r.capacity, near_permille))
    {
        return PressureLevel::Near;
    }
    return PressureLevel::Within;
}

PressureReport pressure_report(const BudgetReading& before, const BudgetReading& after,
                               crd::u32 near_permille) noexcept
{
    PressureReport rep;
    rep.level_now = pressure_level(after, after.in_use, near_permille);

    const bool comparable = before.read && after.read && before.bounded && after.bounded
                            && before.capacity == after.capacity
                            && before.high_water_tracked == after.high_water_tracked
                            && after.refusals >= before.refusals && after.overruns >= before.overruns
                            && (!after.high_water_tracked || after.high_water >= before.high_water);
    if (!comparable)
    {
        rep.verdict = PressureVerdict::Unjudged;
        return rep;
    }

    rep.refused       = after.refusals - before.refusals;
    rep.overran       = after.overruns - before.overruns;
    rep.peak_at_least = larger(before.in_use, after.in_use);
    if (after.high_water_tracked)
    {
        if (after.high_water > before.high_water)
        {
            rep.peak_at_least = larger(rep.peak_at_least, after.high_water);
        }
        rep.peak_at_most = larger(after.high_water, rep.peak_at_least);
        rep.peak_bounded = true;
    }

    const PressureLevel low = pressure_level(after, rep.peak_at_least, near_permille);
    if (rep.refused != 0U)
    {
        rep.verdict = PressureVerdict::Exhausted;
    }
    else if (rep.overran != 0U || low == PressureLevel::Over)
    {
        rep.verdict = PressureVerdict::Overrun;
    }
    else if (low == PressureLevel::Full)
    {
        rep.verdict = PressureVerdict::Saturated;
    }
    else if (low == PressureLevel::Near)
    {
        rep.verdict = PressureVerdict::Near;
    }
    else if (rep.peak_bounded && pressure_level(after, rep.peak_at_most, near_permille) == PressureLevel::Within)
    {
        rep.verdict = PressureVerdict::Relaxed;
    }
    else
    {
        rep.verdict = PressureVerdict::Undetermined;
    }
    return rep;
}

crd::usize format_pressure_report(const char* name, const PressureReport& report, const BudgetReading& after,
                                  char* out, crd::usize capacity) noexcept
{
    if (out == nullptr || capacity == 0U)
    {
        return 0U;
    }
    out[0]            = '\0';
    crd::usize length = 0U;
    if (name != nullptr && *name != '\0')
    {
        length = append(out, capacity, length, name);
        length = append(out, capacity, length, ": ");
    }
    length = append(out, capacity, length, pressure_verdict_name(report.verdict));
    if (!after.read)
    {
        return append(out, capacity, length, " (not read)");
    }
    length = append(out, capacity, length, " (in use ");
    length = append_u64(out, capacity, length, after.in_use);
    if (after.bounded)
    {
        length = append(out, capacity, length, "/");
        length = append_u64(out, capacity, length, after.capacity);
    }
    else
    {
        length = append(out, capacity, length, ", unbounded");
    }
    if (report.verdict != PressureVerdict::Unjudged)
    {
        length = append(out, capacity, length, ", peak ");
        length = append_u64(out, capacity, length, report.peak_at_least);
        if (!report.peak_bounded)
        {
            length = append(out, capacity, length, "+");
        }
        else if (report.peak_at_most != report.peak_at_least)
        {
            length = append(out, capacity, length, "..");
            length = append_u64(out, capacity, length, report.peak_at_most);
        }
        if (report.refused != 0U)
        {
            length = append(out, capacity, length, ", refused +");
            length = append_u64(out, capacity, length, report.refused);
        }
        if (report.overran != 0U)
        {
            length = append(out, capacity, length, ", overran +");
            length = append_u64(out, capacity, length, report.overran);
        }
    }
    return append(out, capacity, length, ")");
}

} // namespace crd
