// Loss and delay of a simulated link (HESAP-NET, capability numerics.link-models, owner crd-hesap-net): what NET-SIM
// and NET-DEBUG draw per packet, on the seeded Philox stream, so a whole run is a pure function of (seed, stream).
//
// Two families, each for what it does best:
//   * Linux netem's models, in its fixed point and word for word: a chance is a u32 compared with a u32 draw, so these
//     are integer-exact on every platform and any `tc qdisc ... netem` line replays here with netem's own decisions
//     (crd-hesap-link-netem-tests runs net/sched/sch_netem.c's functions on the same words, boundaries included).
//   * DelayProcess, which keeps what netem's correlation loses. netem averages a word with the last answer, which
//     narrows the draw's law: at 25% correlation `loss 1%` loses almost nothing. Here a Gaussian AR(1) drives every
//     shape through its own quantile, so the marginal law is exact at any correlation;
//     gilbert_from_rate_and_correlation() is the same promise for loss, a two-state chain with the asked rate and
//     lag-one correlation.
//
// Nothing allocates. Word sources are anything with next_u32(); production passes stats::PhiloxRng.

#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/stats/normal.hpp>

#include <concepts>
#include <limits>

namespace crd::hesap::net
{

template <typename Words>
concept LinkWords = requires(Words& words) {
    { words.next_u32() } -> std::same_as<crd::u32>;
};

// iproute2's fixed point: rint(fraction x (2^32 - 1)). Below 0 and NaN are 0, above 1 is 1.
[[nodiscard]] crd::u32 link_chance(crd::f64 fraction) noexcept;

// netem's correlated word (get_crandom): a correlation of chance c weighs the last answer by (c + 1) / 2^32. init draws
// one word whatever the correlation, as init_crandom does.
class CorrelatedDraw
{
public:
    template <LinkWords Words> void init(crd::u32 correlation, Words& words) noexcept
    {
        m_rho = correlation;
        m_last = words.next_u32();
    }

    template <LinkWords Words> [[nodiscard]] crd::u32 next(Words& words) noexcept
    {
        const crd::u64 value = words.next_u32();
        if (m_rho == 0U)
        {
            return static_cast<crd::u32>(value);
        }
        // The two weights sum to 2^32 and each word is below it, so the sum stays within 64 bits.
        const crd::u64 rho = crd::u64{m_rho} + 1U;
        m_last = static_cast<crd::u32>(((value * ((crd::u64{1} << 32U) - rho)) + (crd::u64{m_last} * rho)) >> 32U);
        return m_last;
    }

private:
    crd::u32 m_rho = 0U;
    crd::u32 m_last = 0U;
};

// Gilbert-Elliott in netem's four numbers, each a link_chance. A packet takes the state it arrives in; the chain moves
// first. Simple Gilbert is {p, r, 0, 0}: every packet in the bad state is lost, none in the good one.
struct GilbertElliottChances
{
    crd::u32 p = 0U;           // good to bad
    crd::u32 r = 0U;           // bad to good
    crd::u32 survive_bad = 0U; // h: a packet survives the bad state
    crd::u32 lost_good = 0U;   // 1 - k: a packet is lost in the good state
};

// netem's four-state chain, the GI model of Salsano et al.: 1 transmitted in a gap, 2 transmitted in a burst, 3 lost in
// a burst, 4 lost in a gap.
struct FourStateChances
{
    crd::u32 p13 = 0U;
    crd::u32 p31 = 0U;
    crd::u32 p32 = 0U;
    crd::u32 p23 = 0U;
    crd::u32 p14 = 0U;
};

// A simple Gilbert chain by what it means: the long-run loss fraction and the mean length of a burst of losses. False,
// writing nothing, unless 0 <= loss < 1 and mean_burst >= 1 and the entry chance they imply is at most 1.
[[nodiscard]] bool gilbert_from_rate_and_burst(crd::f64 loss, crd::f64 mean_burst, GilbertElliottChances& out) noexcept;

// A simple Gilbert chain with the long-run loss fraction and the lag-one correlation of loss events. False, writing
// nothing, unless 0 <= loss < 1 and 0 <= correlation < 1.
[[nodiscard]] bool gilbert_from_rate_and_correlation(crd::f64 loss, crd::f64 correlation,
                                                     GilbertElliottChances& out) noexcept;

// One packet's fate. Default-constructed it loses nothing and draws nothing.
class LossModel
{
public:
    // netem `loss random`: the chance is compared with a correlated word; a chance of zero draws nothing.
    template <LinkWords Words> void set_random(crd::u32 chance, crd::u32 correlation, Words& words) noexcept
    {
        m_kind = Kind::Random;
        m_a1 = chance;
        m_draw.init(correlation, words);
    }

    void set_gilbert_elliott(const GilbertElliottChances& chances) noexcept
    {
        m_kind = Kind::GilbertElliott;
        m_a1 = chances.p;
        m_a2 = chances.r;
        m_a3 = chances.survive_bad;
        m_a4 = chances.lost_good;
        m_state = kGood;
    }

    // False, changing nothing, when a state's outgoing chances sum past one. Two chances of a half round to 2^32
    // between them, which is exactly one and allowed; netem's u32 sum wraps there, the one setting where this differs
    // from it.
    [[nodiscard]] bool set_four_state(const FourStateChances& chances) noexcept
    {
        constexpr crd::u64 one = crd::u64{1} << 32U;
        if (crd::u64{chances.p13} + chances.p14 > one || crd::u64{chances.p31} + chances.p32 > one)
        {
            return false;
        }
        m_kind = Kind::FourState;
        m_a1 = chances.p13;
        m_a2 = chances.p31;
        m_a3 = chances.p32;
        m_a4 = chances.p14;
        m_a5 = chances.p23;
        m_state = kTxInGap;
        return true;
    }

    // True when this packet is lost.
    template <LinkWords Words> [[nodiscard]] bool next(Words& words) noexcept
    {
        switch (m_kind)
        {
            case Kind::Random:
                return m_a1 != 0U && m_a1 >= m_draw.next(words);
            case Kind::GilbertElliott:
                return next_gilbert_elliott(words);
            case Kind::FourState:
                return next_four_state(words.next_u32());
            case Kind::None:
                break;
        }
        return false;
    }

private:
    enum class Kind : crd::u8
    {
        None,
        Random,
        GilbertElliott,
        FourState
    };

    static constexpr crd::u8 kGood = 1U;
    static constexpr crd::u8 kBad = 2U;
    static constexpr crd::u8 kTxInGap = 1U;
    static constexpr crd::u8 kTxInBurst = 2U;
    static constexpr crd::u8 kLostInBurst = 3U;
    static constexpr crd::u8 kLostInGap = 4U;

    template <LinkWords Words> [[nodiscard]] bool next_gilbert_elliott(Words& words) noexcept
    {
        if (m_state == kGood)
        {
            if (words.next_u32() < m_a1)
            {
                m_state = kBad;
            }
            return words.next_u32() < m_a4;
        }
        if (words.next_u32() < m_a2)
        {
            m_state = kGood;
        }
        return words.next_u32() > m_a3;
    }

    // A word exactly on a threshold moves nothing and loses nothing, in every state, as in netem.
    [[nodiscard]] bool next_four_state(crd::u32 word) noexcept
    {
        switch (m_state)
        {
            case kTxInGap:
                if (word < m_a4)
                {
                    m_state = kLostInGap;
                    return true;
                }
                if (m_a4 < word && word < crd::u64{m_a1} + m_a4)
                {
                    m_state = kLostInBurst;
                    return true;
                }
                return false;
            case kTxInBurst:
                if (word < m_a5)
                {
                    m_state = kLostInBurst;
                    return true;
                }
                return false;
            case kLostInBurst:
                if (word < m_a3)
                {
                    m_state = kTxInBurst;
                    return false;
                }
                if (m_a3 < word && word < crd::u64{m_a2} + m_a3)
                {
                    m_state = kTxInGap;
                    return false;
                }
                return crd::u64{m_a2} + m_a3 < word;
            default:
                m_state = kTxInGap;
                return false;
        }
    }

    Kind m_kind = Kind::None;
    crd::u8 m_state = 0U;
    crd::u32 m_a1 = 0U;
    crd::u32 m_a2 = 0U;
    crd::u32 m_a3 = 0U;
    crd::u32 m_a4 = 0U;
    crd::u32 m_a5 = 0U;
    CorrelatedDraw m_draw;
};

// netem's scale of a distribution table: entries are standard scores times 8192.
inline constexpr crd::i32 kNetemTableScale = 8192;

// Means beyond this are held to it, which keeps every sum below within an i64 whatever a profile asks: 146 years.
inline constexpr crd::i64 kMaxLinkDelayNs = crd::i64{1} << 62U;

// netem's tabledist on chance arithmetic: an empty table is uniform in [mean - deviation, mean + deviation), otherwise
// the entry a draw picks scales the deviation. A deviation of zero or below is the mean and draws nothing. The answer
// can be below zero, as in netem: the caller's clock treats that as now. A table has fewer than 2^32 entries (netem's
// 16,384 at most).
template <LinkWords Words>
[[nodiscard]] crd::i64 netem_delay(crd::i64 mean_ns, crd::i32 deviation_ns, CorrelatedDraw& draw, Words& words,
                                   crd::containers::ConstSpan<crd::i16> table) noexcept
{
    const crd::i64 lower = mean_ns < -kMaxLinkDelayNs ? -kMaxLinkDelayNs : mean_ns;
    const crd::i64 mean = lower > kMaxLinkDelayNs ? kMaxLinkDelayNs : lower;
    if (deviation_ns <= 0)
    {
        return mean;
    }
    const crd::u32 word = draw.next(words);
    if (table.empty())
    {
        return static_cast<crd::i64>(word % (2U * static_cast<crd::u32>(deviation_ns))) + mean - deviation_ns;
    }
    const crd::i64 entry = table[word % static_cast<crd::u32>(table.size())];
    crd::i64 scaled = (deviation_ns % kNetemTableScale) * entry;
    // Half the scale away from zero, without a branch: a table's signs are as random as its words.
    const crd::i64 sign = scaled >> 63U;
    scaled += (crd::i64{kNetemTableScale / 2} ^ sign) - sign;
    return (scaled / kNetemTableScale) + ((deviation_ns / kNetemTableScale) * entry) + mean;
}

enum class DelayShape : crd::u8
{
    Uniform,
    Normal,
    LogNormal,
    Pareto,
    ParetoNormal // netem's mix: a quarter of a normal and three quarters of a Pareto, both from one rank.
};

struct DelayOptions
{
    DelayShape shape = DelayShape::Normal;
    crd::i64 mean_ns = 0;
    crd::i64 deviation_ns = 0;  // The standard deviation, in every shape.
    crd::f64 correlation = 0.0; // Lag-one, of the Gaussian that drives the shape: [0, 1).
    crd::f64 pareto_tail = 3.0; // The Pareto shape's exponent: above 2, where the deviation exists.
    crd::i64 floor_ns = 0;      // Every answer is held between these.
    crd::i64 ceiling_ns = kMaxLinkDelayNs;
};

// Delay as an exact marginal law with a chosen correlation. Each draw takes one or two Box-Muller normals of the
// caller's Philox stream, through stats::NormalSampler.
class DelayProcess
{
public:
    explicit DelayProcess(crd::hesap::stats::PhiloxRng& rng) noexcept : m_normal(rng) {}

    // False, changing nothing, unless |mean| and deviation are within kMaxLinkDelayNs, deviation >= 0, correlation is
    // in [0, 1), floor <= ceiling, a log-normal with a deviation has a mean above zero, and a Pareto's tail is above 2
    // and at most 1e6, past which it is an exponential. Restarts the correlation.
    [[nodiscard]] bool init(const DelayOptions& options) noexcept;

    [[nodiscard]] crd::i64 next() noexcept;

private:
    crd::hesap::stats::NormalSampler m_normal;
    DelayOptions m_options;
    crd::f64 m_gaussian = 0.0;
    crd::f64 m_innovation = 1.0;
    crd::f64 m_shape_a = 0.0;
    crd::f64 m_shape_b = 0.0;
    crd::f64 m_shape_c = 0.0;
    bool m_started = false;
};

struct SlotOptions
{
    crd::i64 min_delay_ns = 0;
    crd::i64 max_delay_ns = 0;
    crd::i32 max_packets = 0; // Per slot; 0 is no limit.
    crd::i32 max_bytes = 0;   // Per slot; 0 is no limit.
    // A non-empty table draws the gap from netem_delay(dist_mean_ns, dist_deviation_ns) instead of the two delays.
    crd::containers::ConstSpan<crd::i16> table{};
    crd::i64 dist_mean_ns = 0;
    crd::i32 dist_deviation_ns = 0;
};

// netem's slot model, the bursts of a shared medium (Wi-Fi, LTE, cable): packets leave in slots, a slot closes on its
// packet or byte budget, and the next opens after a drawn gap.
class SlotGate
{
public:
    // False, changing nothing, for a negative delay, a minimum past the maximum, a negative budget or deviation. Delays
    // of zero and no table leave the gate open for good.
    [[nodiscard]] bool init(const SlotOptions& options, crd::i64 now_ns) noexcept
    {
        if (options.min_delay_ns < 0 || options.max_delay_ns < options.min_delay_ns || options.max_packets < 0 ||
            options.max_bytes < 0 || options.dist_deviation_ns < 0)
        {
            return false;
        }
        m_options = options;
        m_options.max_packets = options.max_packets == 0 ? std::numeric_limits<crd::i32>::max() : options.max_packets;
        m_options.max_bytes = options.max_bytes == 0 ? std::numeric_limits<crd::i32>::max() : options.max_bytes;
        m_packets_left = m_options.max_packets;
        m_bytes_left = m_options.max_bytes;
        m_active = options.min_delay_ns != 0 || options.max_delay_ns != 0 || options.dist_deviation_ns != 0;
        m_next_ns = now_ns;
        return true;
    }

    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] crd::i64 next_open_ns() const noexcept { return m_next_ns; }

    // Whether the head packet, due at time_to_send_ns, leaves at now_ns. A slot already over by its due time gives way
    // to a new one; a packet that leaves spends the slot's budgets, and an empty budget ends it.
    template <LinkWords Words>
    [[nodiscard]] bool admit(crd::i64 now_ns, crd::i64 time_to_send_ns, crd::u32 bytes, Words& words) noexcept
    {
        if (!m_active)
        {
            return time_to_send_ns <= now_ns;
        }
        if (m_next_ns < time_to_send_ns)
        {
            open_next(now_ns, words);
        }
        if (time_to_send_ns > now_ns || m_next_ns > now_ns)
        {
            return false;
        }
        --m_packets_left;
        m_bytes_left -= bytes;
        if (m_packets_left <= 0 || m_bytes_left <= 0)
        {
            open_next(now_ns, words);
        }
        return true;
    }

private:
    template <LinkWords Words> void open_next(crd::i64 now_ns, Words& words) noexcept
    {
        crd::i64 gap = 0;
        if (m_options.table.empty())
        {
            // The word times the span, over 2^32, split so a span past 32 bits cannot wrap.
            const crd::u64 span = static_cast<crd::u64>(m_options.max_delay_ns - m_options.min_delay_ns);
            const crd::u64 word = words.next_u32();
            gap = m_options.min_delay_ns +
                  static_cast<crd::i64>(((span >> 32U) * word) + (((span & 0xFFFFFFFFU) * word) >> 32U));
        }
        else
        {
            CorrelatedDraw uncorrelated;
            gap =
                netem_delay(m_options.dist_mean_ns, m_options.dist_deviation_ns, uncorrelated, words, m_options.table);
        }
        constexpr crd::i64 far = std::numeric_limits<crd::i64>::max();
        if (gap > 0 && now_ns > far - gap)
        {
            m_next_ns = far;
        }
        else if (gap < 0 && now_ns < -far - gap)
        {
            m_next_ns = -far;
        }
        else
        {
            m_next_ns = now_ns + gap;
        }
        m_packets_left = m_options.max_packets;
        m_bytes_left = m_options.max_bytes;
    }

    SlotOptions m_options;
    crd::i64 m_next_ns = 0;
    crd::i32 m_packets_left = 0;
    crd::i64 m_bytes_left = 0;
    bool m_active = false;
};

} // namespace crd::hesap::net
