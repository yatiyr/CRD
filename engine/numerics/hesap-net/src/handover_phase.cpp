#include <crd/hesap/net/handover_phase.hpp>

#include <crd/core/wide_multiply.hpp>

#include <bit>

namespace crd::hesap::net
{

namespace
{

constexpr crd::u64 kAll = ~crd::u64{0};
constexpr crd::u32 kSub = 256U; // a bin's first disturbance is kept in 256ths of it

// floor(a b / 2^62) and its ceiling, for a and b at most 2^62.
[[nodiscard]] crd::u64 multiply_down_q62(crd::u64 a, crd::u64 b) noexcept
{
    const WideProduct p = multiply_wide(a, b);
    return (p.hi << 2U) | (p.lo >> 62U);
}

[[nodiscard]] crd::u64 multiply_up_q62(crd::u64 a, crd::u64 b) noexcept
{
    const WideProduct p = multiply_wide(a, b);
    return ((p.hi << 2U) | (p.lo >> 62U)) + (((p.lo & (HandoverPhaseLearner::kOneQ62 - 1U)) != 0U) ? 1U : 0U);
}

[[nodiscard]] crd::u64 power_q62(crd::u64 base, crd::u32 exponent, bool up) noexcept
{
    crd::u64 result = HandoverPhaseLearner::kOneQ62;
    for (crd::u32 i = 0U; i < exponent; ++i)
    {
        result = up ? multiply_up_q62(result, base) : multiply_down_q62(result, base);
    }
    return result;
}

[[nodiscard]] crd::u64 rotate_left(crd::u64 bits, crd::u32 by) noexcept
{
    return std::rotl(bits, static_cast<int>(by % HandoverPhaseLearner::kBins));
}

// Sorts `values[0, count)` ascending (insertion: at most kHistory * (kHistory - 1) / 2 of them).
void sort_values(crd::i64* values, crd::u32 count) noexcept
{
    for (crd::u32 i = 1U; i < count; ++i)
    {
        const crd::i64 value = values[i];
        crd::u32 at = i;
        for (; at > 0U && values[at - 1U] > value; --at)
        {
            values[at] = values[at - 1U];
        }
        values[at] = value;
    }
}

// The median of sorted `values[0, count)`, count > 0: the mean of the middle two, rounded down, for an even count.
[[nodiscard]] crd::i64 median_of(const crd::i64* values, crd::u32 count) noexcept
{
    const crd::i64 low = values[(count - 1U) / 2U];
    const crd::i64 high = values[count / 2U];
    return low + ((high - low) >> 1U);
}

} // namespace

bool HandoverPhaseLearner::start(crd::u64 period_ns) noexcept
{
    if (period_ns < kMinPeriodNs || period_ns > kMaxPeriodNs)
    {
        return false;
    }
    const crd::u32 revision = m_revision;
    *this = HandoverPhaseLearner{};
    m_period_ns = period_ns;
    m_revision = revision + 1U;
    return true;
}

void HandoverPhaseLearner::stop() noexcept
{
    const crd::u32 revision = m_revision;
    *this = HandoverPhaseLearner{};
    m_revision = revision + 1U;
}

void HandoverPhaseLearner::restart() noexcept
{
    const crd::u64 period = m_period_ns;
    const crd::u32 revision = m_revision;
    *this = HandoverPhaseLearner{};
    m_period_ns = period;
    m_revision = revision + 1U;
}

void HandoverPhaseLearner::on_rtt(crd::u64 send_ns, crd::u64 rtt_ns) noexcept
{
    if (m_period_ns == 0U)
    {
        return;
    }
    constexpr crd::u64 max_rtt_us = crd::u64{1} << 31U; // InterarrivalJitter's signed 32-bit differences
    const crd::u64 rtt_us = (rtt_ns / 1000U < max_rtt_us) ? rtt_ns / 1000U : max_rtt_us;
    // A rise only: a handover adds delay, and its end, the fall, marks no event.
    const crd::u64 rise = (rtt_us > m_previous_rtt_us) ? rtt_us - m_previous_rtt_us : 0U;
    const crd::u64 noise = kStepJitters * crd::u64{m_jitter.value()};
    const bool stepped = m_has_rtt && rise >= ((noise > kMinStepUs) ? noise : kMinStepUs);
    m_jitter.update(static_cast<crd::u32>(rtt_us), 0U); // the transit of each sample is its RTT
    m_previous_rtt_us = rtt_us;
    m_has_rtt = true;
    record(send_ns, stepped);
}

void HandoverPhaseLearner::on_loss(crd::u64 send_ns) noexcept
{
    if (m_period_ns != 0U)
    {
        record(send_ns, true);
    }
}

void HandoverPhaseLearner::record(crd::u64 send_ns, bool disturbed) noexcept
{
    const crd::u64 index = send_ns / m_period_ns;
    if (m_newest == kAll || index > m_newest)
    {
        const bool fresh = m_newest == kAll || index - m_newest >= kHistory;
        if (fresh)
        {
            for (Period& period : m_history)
            {
                period = Period{};
            }
        }
        for (crd::u64 i = fresh ? index : m_newest + 1U; i <= index; ++i)
        {
            m_history[i % kHistory] = Period{};
            m_history[i % kHistory].index = i;
        }
        const bool had = m_newest != kAll;
        m_newest = index;
        if (had)
        {
            evaluate();
        }
    }
    else if (m_newest - index >= kHistory)
    {
        return; // older than the history
    }
    Period& period = m_history[index % kHistory];
    // The bin and the 256th of it the send time falls in: phase * kBins * 256 stays below 2^60 for any period.
    const crd::u64 scaled = (send_ns % m_period_ns) * kBins;
    const crd::u32 bin = static_cast<crd::u32>(scaled / m_period_ns);
    const crd::u8 sub = static_cast<crd::u8>((scaled % m_period_ns) * kSub / m_period_ns);
    const crd::u64 bit = crd::u64{1} << bin;
    period.observed |= bit;
    if (!disturbed)
    {
        return;
    }
    if ((period.disturbed & bit) == 0U || sub < period.first[bin])
    {
        period.first[bin] = sub;
    }
    period.disturbed |= bit;
}

crd::u64 HandoverPhaseLearner::window_mask(crd::u32 start, crd::u32 bins) const noexcept
{
    return rotate_left((bins >= kBins) ? kAll : (crd::u64{1} << bins) - 1U, start);
}

HandoverPhaseLearner::Window HandoverPhaseLearner::window(crd::u32 start) const noexcept
{
    const crd::u64 mask = window_mask(start, kWindow);
    Window out;
    out.start = start;
    for (const Period& period : m_history)
    {
        if (completed(period))
        {
            out.periods += ((period.observed & mask) != 0U) ? 1U : 0U;
            out.hits += ((period.disturbed & mask) != 0U) ? 1U : 0U;
        }
    }
    return out;
}

// The chance that a window of the background disturbs a period: 1 - (1 - q)^bins for the background's rate q, the
// disturbed share of the bins observed outside this window and a window either side, by the rule of succession.
crd::u64 HandoverPhaseLearner::window_q62(crd::u32 start) const noexcept
{
    const crd::u64 background = ~window_mask(start + kBins - kWindow, 3U * kWindow);
    crd::u64 disturbed = 0U;
    crd::u64 observed = 0U;
    for (const Period& period : m_history)
    {
        if (completed(period))
        {
            disturbed += static_cast<crd::u64>(std::popcount(period.disturbed & background));
            observed += static_cast<crd::u64>(std::popcount(period.observed & background));
        }
    }
    const crd::u64 trials = observed + 2U;
    const crd::u64 rate = (disturbed + 1U) * ((kOneQ62 + trials - 1U) / trials);
    const crd::u64 q = (rate < kOneQ62) ? rate : kOneQ62;
    return kOneQ62 - power_q62(kOneQ62 - q, kWindow, false);
}

crd::u64 HandoverPhaseLearner::binomial_tail_q62(crd::u32 periods, crd::u32 hits, crd::u64 q_q62) noexcept
{
    if (hits == 0U)
    {
        return kOneQ62;
    }
    if (hits > periods)
    {
        return 0U;
    }
    const crd::u64 q = (q_q62 < kOneQ62) ? q_q62 : kOneQ62;
    // C(periods, hits), then each later coefficient from the one before: exact for the kHistory periods held.
    crd::u64 choose = 1U;
    for (crd::u32 k = 1U; k <= hits; ++k)
    {
        choose = choose * (periods - k + 1U) / k;
    }
    crd::u64 tail = 0U;
    for (crd::u32 x = hits; x <= periods; ++x)
    {
        tail += choose * multiply_up_q62(power_q62(q, x, true), power_q62(kOneQ62 - q, periods - x, true));
        choose = choose * (periods - x) / (x + 1U);
    }
    return (tail < kOneQ62) ? tail : kOneQ62;
}

// Where the event falls in the period being sent now: each disturbing period's first disturbance from the window's
// onset bin, as a line through the periods by Theil and Sen's estimator (the median of the pairwise slopes, then of
// the intercepts; Sen, JASA 63:1379, 1968), so a skewed clock's drift is followed without lag and a period whose
// first disturbance was background moves neither.
crd::u64 HandoverPhaseLearner::onset_ns(const Window& w) const noexcept
{
    const crd::u64 mask = window_mask(w.start, kWindow);
    crd::u32 onset = 0U;
    for (crd::u32 k = 0U; k < kWindow; ++k)
    {
        const crd::u64 bit = rotate_left(1U, w.start + k);
        crd::u32 count = 0U;
        for (const Period& period : m_history)
        {
            count += (completed(period) && (period.disturbed & bit) != 0U) ? 1U : 0U;
        }
        if (3U * count >= w.hits)
        {
            onset = k;
            break;
        }
    }
    // The points: a period's age from the one being sent (1 to kHistory) and its offset in the window in 256ths of a
    // bin, scaled by 2^16 so the slopes keep their fractions.
    constexpr crd::u32 scale = 16U;
    crd::i64 age[kHistory] = {};
    crd::i64 offset[kHistory] = {};
    crd::u32 points = 0U;
    for (const Period& period : m_history)
    {
        if (!completed(period) || (period.disturbed & mask) == 0U)
        {
            continue;
        }
        for (crd::u32 k = onset; k < kWindow; ++k)
        {
            const crd::u32 bin = (w.start + k) % kBins;
            if (((period.disturbed >> bin) & 1U) != 0U)
            {
                age[points] = static_cast<crd::i64>(m_newest - period.index);
                offset[points] = static_cast<crd::i64>(k * kSub + period.first[bin]) << scale;
                ++points;
                break;
            }
        }
    }
    crd::i64 at = static_cast<crd::i64>(onset * kSub) << scale;
    if (points != 0U)
    {
        crd::i64 slopes[kHistory * (kHistory - 1U) / 2U] = {};
        crd::u32 pairs = 0U;
        for (crd::u32 i = 0U; i < points; ++i)
        {
            for (crd::u32 j = i + 1U; j < points; ++j)
            {
                // Ages differ (one point a period); the slope is the offset's change per period of age.
                slopes[pairs++] = (offset[j] - offset[i]) / (age[j] - age[i]);
            }
        }
        crd::i64 slope = 0;
        if (pairs != 0U)
        {
            sort_values(slopes, pairs);
            slope = median_of(slopes, pairs);
        }
        crd::i64 intercepts[kHistory] = {};
        for (crd::u32 i = 0U; i < points; ++i)
        {
            intercepts[i] = offset[i] - slope * age[i];
        }
        sort_values(intercepts, points);
        at = median_of(intercepts, points);
        // A line that leaves the window and a window either side is no drift a clock makes: the plain median then.
        const crd::i64 reach = static_cast<crd::i64>(kWindow * kSub) << scale;
        if (at < -reach || at >= 2 * reach)
        {
            sort_values(offset, points);
            at = median_of(offset, points);
        }
    }
    // The window's start plus the offset, around the period's kBins * 256 steps.
    constexpr crd::i64 whole = static_cast<crd::i64>(kBins) * kSub;
    const crd::i64 steps = ((static_cast<crd::i64>(w.start * kSub) + (at >> scale)) % whole + whole) % whole;
    return static_cast<crd::u64>(steps) * m_period_ns / static_cast<crd::u64>(whole);
}

void HandoverPhaseLearner::evaluate() noexcept
{
    Window windows[kBins];
    for (crd::u32 c = 0U; c < kBins; ++c)
    {
        windows[c] = window(c);
    }
    // The window most periods disturbed among `count` from `from`; of equals, the first that starts a run of them.
    const auto best = [&windows](crd::u32 from, crd::u32 count)
    {
        Window out = windows[from % kBins];
        bool starts = windows[(from + kBins - 1U) % kBins].hits < out.hits;
        for (crd::u32 k = 1U; k < count; ++k)
        {
            const Window& w = windows[(from + k) % kBins];
            const bool run = windows[(from + k + kBins - 1U) % kBins].hits < w.hits;
            if (w.hits > out.hits || (w.hits == out.hits && run && !starts))
            {
                out = w;
                starts = run;
            }
        }
        return out;
    };
    HandoverPhase next;
    if (m_phase.locked)
    {
        const Window w = best(m_lock_start + kBins - (kWindow - 1U), 2U * kWindow - 1U);
        if (w.hits >= kMinKeep && 3U * w.hits >= w.periods &&
            binomial_tail_q62(w.periods, w.hits, window_q62(w.start)) <= kKeepAlphaQ62)
        {
            next = HandoverPhase{true, onset_ns(w), w.hits, w.periods};
            m_lock_start = w.start;
        }
    }
    if (!next.locked)
    {
        const Window w = best(0U, kBins);
        if (w.hits >= kMinLock && 2U * w.hits >= w.periods &&
            binomial_tail_q62(w.periods, w.hits, window_q62(w.start)) <= kLockAlphaQ62)
        {
            next = HandoverPhase{true, onset_ns(w), w.hits, w.periods};
            m_lock_start = w.start;
        }
    }
    if (!(next == m_phase))
    {
        m_phase = next;
        ++m_revision;
    }
}

} // namespace crd::hesap::net
