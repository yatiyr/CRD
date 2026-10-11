#include <crd/core/lifetime_ledger.hpp>

#include <cstdint>
#include <cstdio>

namespace crd
{

namespace
{
constinit LifetimeLedger g_ledger;

constexpr crd::usize index_of(LifetimeClass c) noexcept
{
    return static_cast<crd::usize>(c);
}

// Raise `slot` to `value` if it is lower.
void raise_to(std::atomic<crd::u64>& slot, crd::u64 value) noexcept
{
    crd::u64 seen = slot.load(std::memory_order_relaxed);
    while (seen < value && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed))
    {
    }
}

// Subtract `amount` from `slot` unless that would take it below zero; false when it would.
bool take(std::atomic<crd::u64>& slot, crd::u64 amount) noexcept
{
    crd::u64 seen = slot.load(std::memory_order_relaxed);
    while (true)
    {
        if (seen < amount)
        {
            return false;
        }
        if (slot.compare_exchange_weak(seen, seen - amount, std::memory_order_relaxed))
        {
            return true;
        }
    }
}

// The signed difference after - before of two u64 counts (both well below 2^63 in practice; saturated otherwise).
crd::i64 signed_change(crd::u64 before, crd::u64 after) noexcept
{
    constexpr auto max_i64 = static_cast<crd::u64>(INT64_MAX);
    if (after >= before)
    {
        const crd::u64 up = after - before;
        return up > max_i64 ? INT64_MAX : static_cast<crd::i64>(up);
    }
    const crd::u64 down = before - after;
    return down > max_i64 ? INT64_MIN : -static_cast<crd::i64>(down);
}

// Append `text` to [out, out + capacity), keeping room for the NUL; returns the new length.
crd::usize append(char* out, crd::usize capacity, crd::usize length, const char* text) noexcept
{
    if (capacity == 0U)
    {
        return 0U;
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

crd::usize append_classes(const LifetimeReport& report, crd::u8 mask, const char* heading, bool first, char* out,
                          crd::usize capacity, crd::usize length) noexcept
{
    if (mask == 0U)
    {
        return length;
    }
    if (!first)
    {
        length = append(out, capacity, length, "; ");
    }
    length         = append(out, capacity, length, heading);
    bool separated = false;
    for (crd::usize i = 0U; i < kLifetimeClassCount; ++i)
    {
        const auto c = static_cast<LifetimeClass>(i);
        if ((mask & lifetime_class_bit(c)) == 0U)
        {
            continue;
        }
        length    = append(out, capacity, length, separated ? ", " : " ");
        separated = true;
        length    = append(out, capacity, length, lifetime_class_name(c));
        char number[64];
        if (report.live_delta[i] != 0)
        {
            (void)std::snprintf(number, sizeof(number), " %+lld", static_cast<long long>(report.live_delta[i]));
            length = append(out, capacity, length, number);
        }
        if (report.byte_delta[i] != 0)
        {
            (void)std::snprintf(number, sizeof(number), " (%+lld bytes)", static_cast<long long>(report.byte_delta[i]));
            length = append(out, capacity, length, number);
        }
    }
    return length;
}
} // namespace

const char* lifetime_class_name(LifetimeClass c) noexcept
{
    switch (c) // no default: every class is named
    {
    case LifetimeClass::FileHandle:           return "file-handle";
    case LifetimeClass::LibraryHandle:        return "library-handle";
    case LifetimeClass::MappedRegion:         return "mapped-region";
    case LifetimeClass::DeviceObject:         return "device-object";
    case LifetimeClass::CallbackRegistration: return "callback-registration";
    case LifetimeClass::Thread:               return "thread";
    }
    return "unknown";
}

const char* lifetime_verdict_name(LifetimeVerdict v) noexcept
{
    switch (v) // no default: every verdict is named
    {
    case LifetimeVerdict::Balanced:    return "balanced";
    case LifetimeVerdict::Outstanding: return "outstanding";
    case LifetimeVerdict::Drained:     return "drained";
    case LifetimeVerdict::Underflowed: return "underflowed";
    }
    return "unknown";
}

void LifetimeLedger::acquired(LifetimeClass c, crd::u64 bytes) noexcept
{
    Slot& s = m_slots[index_of(c)];
    s.acquired.fetch_add(1U, std::memory_order_relaxed);
    if (bytes != 0U)
    {
        s.live_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    const crd::u64 live = s.live.fetch_add(1U, std::memory_order_relaxed) + 1U;
    raise_to(s.high_water, live);
}

bool LifetimeLedger::released(LifetimeClass c, crd::u64 bytes) noexcept
{
    Slot& s = m_slots[index_of(c)];
    if (!take(s.live, 1U))
    {
        s.unmatched.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    s.released.fetch_add(1U, std::memory_order_relaxed);
    if (bytes != 0U && !take(s.live_bytes, bytes))
    {
        s.unmatched.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    return true;
}

LifetimeSnapshot LifetimeLedger::snapshot() const noexcept
{
    LifetimeSnapshot out;
    for (crd::usize i = 0U; i < kLifetimeClassCount; ++i)
    {
        const Slot&     s = m_slots[i];
        LifetimeCounts& o = out.classes[i];
        o.acquired           = s.acquired.load(std::memory_order_relaxed);
        o.released           = s.released.load(std::memory_order_relaxed);
        o.live               = s.live.load(std::memory_order_relaxed);
        o.high_water         = s.high_water.load(std::memory_order_relaxed);
        o.live_bytes         = s.live_bytes.load(std::memory_order_relaxed);
        o.unmatched_releases = s.unmatched.load(std::memory_order_relaxed);
    }
    return out;
}

LifetimeLedger& lifetime_ledger() noexcept
{
    return g_ledger;
}

LifetimeReport lifetime_report(const LifetimeSnapshot& before, const LifetimeSnapshot& after) noexcept
{
    LifetimeReport r;
    for (crd::usize i = 0U; i < kLifetimeClassCount; ++i)
    {
        const LifetimeCounts& b   = before.classes[i];
        const LifetimeCounts& a   = after.classes[i];
        const crd::u8         bit = lifetime_class_bit(static_cast<LifetimeClass>(i));
        r.live_delta[i]           = signed_change(b.live, a.live);
        r.byte_delta[i]           = signed_change(b.live_bytes, a.live_bytes);
        if (a.unmatched_releases != b.unmatched_releases)
        {
            r.underflowed = static_cast<crd::u8>(r.underflowed | bit);
        }
        if (r.live_delta[i] > 0 || r.byte_delta[i] > 0)
        {
            r.outstanding = static_cast<crd::u8>(r.outstanding | bit);
        }
        else if (r.live_delta[i] < 0 || r.byte_delta[i] < 0)
        {
            r.drained = static_cast<crd::u8>(r.drained | bit);
        }
    }
    if (r.underflowed != 0U)
    {
        r.verdict = LifetimeVerdict::Underflowed;
    }
    else if (r.outstanding != 0U)
    {
        r.verdict = LifetimeVerdict::Outstanding;
    }
    else if (r.drained != 0U)
    {
        r.verdict = LifetimeVerdict::Drained;
    }
    else
    {
        r.verdict = LifetimeVerdict::Balanced;
    }
    return r;
}

crd::usize format_lifetime_report(const LifetimeReport& report, char* out, crd::usize capacity) noexcept
{
    if (out == nullptr || capacity == 0U)
    {
        return 0U;
    }
    out[0]            = '\0';
    crd::usize length = 0U;
    if (report.verdict == LifetimeVerdict::Balanced)
    {
        return append(out, capacity, length, "balanced");
    }
    length = append_classes(report, report.underflowed, "underflowed:", true, out, capacity, length);
    length = append_classes(report, report.outstanding, "outstanding:", length == 0U, out, capacity, length);
    length = append_classes(report, report.drained, "drained:", length == 0U, out, capacity, length);
    return length;
}

} // namespace crd
