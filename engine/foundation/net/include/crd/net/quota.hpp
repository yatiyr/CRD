#pragma once

// crd-net -- what a principal may spend (NET.3, stage S6; threats T09 and T11; contract:
// docs/design/network-foundation.md#quotas). Rates are virtual-scheduling gates (the generic cell rate algorithm of
// ITU-T I.371, Turner's leaky bucket as a meter): one integer in nanoseconds, no timer and no floating point, so a
// simulated clock reproduces them exactly. Time arrives as an argument; nothing here reads a clock.

#include <crd/core/types.hpp>

#include <atomic>

namespace crd::net
{

inline constexpr u64 kNanosPerSecond = 1000000000ULL;
inline constexpr u64 kNeverNs = ~u64{0};

// What one gate allows: `per_second` units on average, zero being none at all, and `tolerance_ns` of credit, which lets
// an idle gate take a second's worth at once by default. A message larger than the credit still passes when the gate is
// clear and is then paid for by the wait that follows, so a large message is never refused for ever and the long-run
// rate holds.
struct GateRule
{
    u64 per_second = 0U;
    u64 tolerance_ns = kNanosPerSecond;
};

// The time `units` cost at `per_second` (not zero), rounded up: a budget is a ceiling.
[[nodiscard]] constexpr u64 gate_cost_ns(u32 units, u64 per_second) noexcept
{
    const u64 scaled = u64{units} * kNanosPerSecond; // below 2^62
    return (scaled / per_second) + (((scaled % per_second) != 0U) ? 1U : 0U);
}

// The first time a gate whose next arrival is `next_ns` conforms.
[[nodiscard]] constexpr u64 gate_open_at(u64 next_ns, const GateRule& rule) noexcept
{
    if (rule.per_second == 0U)
    {
        return kNeverNs;
    }
    return (next_ns > rule.tolerance_ns) ? (next_ns - rule.tolerance_ns) : 0U;
}

// The next arrival after spending `units` at `now_ns`.
[[nodiscard]] constexpr u64 gate_after(u64 next_ns, u32 units, const GateRule& rule, u64 now_ns) noexcept
{
    const u64 base = (next_ns > now_ns) ? next_ns : now_ns;
    if (rule.per_second == 0U)
    {
        return base;
    }
    const u64 cost = gate_cost_ns(units, rule.per_second);
    return (cost > kNeverNs - base) ? kNeverNs : (base + cost);
}

// A gate one session's I/O loop owns.
class RateGate
{
public:
    [[nodiscard]] u64 open_at(const GateRule& rule) const noexcept { return gate_open_at(m_next_ns, rule); }

    void charge(u32 units, const GateRule& rule, u64 now_ns) noexcept
    {
        m_next_ns = gate_after(m_next_ns, units, rule, now_ns);
    }

private:
    u64 m_next_ns = 0U;
};

// A gate several sessions of one principal share, possibly from several loops. Concurrent checks may let one message
// each past a closing gate; what they spend is still charged, so the debt is paid and the long-run rate holds.
class SharedRateGate
{
public:
    [[nodiscard]] u64 open_at(const GateRule& rule) const noexcept
    {
        return gate_open_at(m_next_ns.load(std::memory_order_relaxed), rule);
    }

    void charge(u32 units, const GateRule& rule, u64 now_ns) noexcept
    {
        u64 next = m_next_ns.load(std::memory_order_relaxed);
        while (!m_next_ns.compare_exchange_weak(next, gate_after(next, units, rule, now_ns), std::memory_order_relaxed))
        {
        }
    }

private:
    std::atomic<u64> m_next_ns{0U};
};

// A held amount with a ceiling.
class Gauge
{
public:
    // Adds `amount` when the total stays at or below `limit`; false, changing nothing, when it would not.
    [[nodiscard]] bool try_add(u64 amount, u64 limit) noexcept
    {
        u64 held = m_held.load(std::memory_order_relaxed);
        do
        {
            if (amount > limit || held > limit - amount)
            {
                return false;
            }
        } while (!m_held.compare_exchange_weak(held, held + amount, std::memory_order_relaxed));
        return true;
    }

    void release(u64 amount) noexcept { m_held.fetch_sub(amount, std::memory_order_relaxed); }

    [[nodiscard]] u64 held() const noexcept { return m_held.load(std::memory_order_relaxed); }

private:
    std::atomic<u64> m_held{0U};
};

// What a limit leaves a channel of `priority`: the whole of it at priority 0, an eighth less for each step down to one
// eighth at 7 and below. The headroom above a class is what lets a higher class through when the lower ones have filled
// their share: overload sheds lower priority classes first (network foundation, Bounded admission).
[[nodiscard]] constexpr u64 priority_limit(u64 limit, u8 priority) noexcept
{
    const u64 step = (priority < 7U) ? u64{priority} : 7U;
    return limit - ((limit / 8U) * step);
}

// A principal's ceilings. NET.4 measures and publishes the defaults.
struct QuotaLimits
{
    u32 streams = 256U;
    u64 datagrams_per_s = 16384U;
    u64 messages_per_s = 16384U;
    u64 bytes_per_s = 64ULL * 1024ULL * 1024ULL;
    u64 reassembly_bytes = 8ULL * 1024ULL * 1024ULL;
    u32 pending_messages = 4096U;
    u64 tolerance_ns = kNanosPerSecond;
};

// One principal's spending across all its sessions. The service keeps one per principal; any of its loops may charge
// it.
struct PrincipalQuota
{
    explicit PrincipalQuota(const QuotaLimits& quota_limits) noexcept : limits(quota_limits) {}

    QuotaLimits limits;

    SharedRateGate datagrams;
    SharedRateGate messages;
    SharedRateGate bytes;
    // The capability's own budget, read from the capability the session presents.
    SharedRateGate budget_operations;
    SharedRateGate budget_bytes;

    Gauge streams;
    Gauge reassembly;
    Gauge pending;
    Gauge objects; // what the capability's `objects` budget bounds: an operation that creates or holds one takes it
};

// What a session may hold queued to send: a sender's backpressure. A reliable channel over its share is told to wait
// until sent() has made room; an unreliable one sheds the message, since a later one will replace it.
class SendBudget
{
public:
    enum class Verdict : u8
    {
        Queued,
        Full, // reliable: wait for room
        Shed, // unreliable: dropped
    };

    explicit SendBudget(u64 limit_bytes) noexcept : m_limit(limit_bytes) {}

    [[nodiscard]] Verdict try_queue(u32 bytes, u8 priority, bool reliable) noexcept
    {
        const u64 limit = priority_limit(m_limit, priority);
        if (bytes > limit || m_queued > limit - bytes)
        {
            return reliable ? Verdict::Full : Verdict::Shed;
        }
        m_queued += bytes;
        return Verdict::Queued;
    }

    void sent(u32 bytes) noexcept { m_queued -= bytes; }

    [[nodiscard]] u64 queued() const noexcept { return m_queued; }

private:
    u64 m_limit = 0U;
    u64 m_queued = 0U;
};

} // namespace crd::net
