// Loss and delay of a simulated link (HESAP-NET, capability numerics.link-models, owner crd-hesap-net): what NET-SIM
// and NET-DEBUG draw per packet, on the seeded Philox stream, so a whole run is a pure function of (seed, stream).
//
// Three families, each for what it does best:
//   * The `tc netem` settings, in the fixed point they are given in: a chance is a u32 compared with a u32 word, so
//     these are integer-exact on every platform and a `tc qdisc ... netem` line replays here with the same decisions
//     (the options are described in tc-netem(8) and in S. Hemminger, Network Emulation with NetEm, linux.conf.au 2005;
//     the loss chains are E. N. Gilbert, Capacity of a burst-noise channel, BSTJ 39, 1960, E. O. Elliott, Estimates of
//     error rates for codes on burst-noise channels, BSTJ 42, 1963, and the four-state model of S. Salsano,
//     F. Ludovici, A. Ordine, Definition of a general and intuitive loss model for packet networks, 2010, after
//     A. D. Clark, 2001). crd-hesap-link-netem-tests holds them against the running kernel's netem, by `tc` in a
//     private namespace.
//   * DelayProcess, which keeps what a correlated word loses. Averaging a word with the last answer narrows the draw's
//     law: at 25% correlation `loss 1%` loses almost nothing. Here a Gaussian AR(1) drives every shape through its own
//     quantile, so the marginal law is exact at any correlation (a Gaussian copula);
//     gilbert_from_rate_and_correlation() is the same promise for loss, a two-state chain with the asked rate and
//     lag-one correlation. A Table shape reads a measured distribution the same way.
//   * The bursts of a shared medium (Wi-Fi, LTE, cable), twice: SlotGate with the slot options of tc, and MediumGrants,
//     a pure departure-time function on a DelayProcess gap.
//
// Nothing allocates. Word sources are anything with next_u32(); production passes stats::PhiloxRng.

#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
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

// The fixed point of a chance: rint(fraction x (2^32 - 1)). Below 0 and NaN are 0, above 1 is 1.
[[nodiscard]] crd::u32 link_chance(crd::f64 fraction) noexcept;

// A word stream whose draws are pulled toward the last answer, the `correlation` option of tc-netem: with a
// correlation of chance c the answer is (word x (2^32 - w) + last x w) / 2^32, w = c + 1, rounded down, so the full
// chance repeats the first word for ever. init draws one word whatever the correlation, to start the memory.
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
        const crd::u32 fresh = words.next_u32();
        if (m_rho == 0U)
        {
            // No correlation: the word passes through and the memory stays as it was.
            return fresh;
        }
        // A weighted mean of the new word and the memory, in 32.32 fixed point. The weights add up to 2^32 and a word
        // is below 2^32, so the products and their sum fit in 64 bits.
        const crd::u64 weight_old = crd::u64{m_rho} + 1U;
        const crd::u64 weight_new = (crd::u64{1} << 32U) - weight_old;
        const crd::u64 mixed = (weight_new * fresh) + (weight_old * m_last);
        m_last = static_cast<crd::u32>(mixed >> 32U);
        return m_last;
    }

private:
    crd::u32 m_rho = 0U;
    crd::u32 m_last = 0U;
};

// Gilbert-Elliott in the four numbers `tc` takes, each a link_chance. A packet takes the state it arrives in; the chain
// moves first. Simple Gilbert is {p, r, 0, 0}: every packet in the bad state is lost, none in the good one.
struct GilbertElliottChances
{
    crd::u32 p = 0U;           // good to bad
    crd::u32 r = 0U;           // bad to good
    crd::u32 survive_bad = 0U; // h: a packet survives the bad state
    crd::u32 lost_good = 0U;   // 1 - k: a packet is lost in the good state
};

// The four-state chain, the GI model of Salsano et al.: 1 received in a gap, 2 received in a burst, 3 lost in a burst,
// 4 lost in a gap.
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
    // `loss random`: the chance is compared with a correlated word; a chance of zero draws nothing.
    template <LinkWords Words> void set_random(crd::u32 chance, crd::u32 correlation, Words& words) noexcept
    {
        m_chance = chance;
        m_draw.init(correlation, words);
        m_kind = Kind::Random;
    }

    void set_gilbert_elliott(const GilbertElliottChances& chances) noexcept
    {
        m_kind = Kind::GilbertElliott;
        m_gilbert = chances;
        m_bad = false;
    }

    // False, changing nothing, when a state's outgoing chances sum past one. Two chances of a half round to 2^32
    // between them, which is exactly one and allowed; a u32 sum would wrap there, the one setting where this differs
    // from tc.
    [[nodiscard]] bool set_four_state(const FourStateChances& chances) noexcept
    {
        constexpr crd::u64 one = crd::u64{1} << 32U;
        if (crd::u64{chances.p13} + chances.p14 > one || crd::u64{chances.p31} + chances.p32 > one)
        {
            return false;
        }
        m_kind = Kind::FourState;
        m_four = chances;
        m_at = Four::ReceivedInGap;
        return true;
    }

    // True when this packet is lost.
    template <LinkWords Words> [[nodiscard]] bool next(Words& words) noexcept
    {
        switch (m_kind)
        {
            case Kind::Random:
            {
                if (m_chance == 0U)
                {
                    return false;
                }
                const crd::u32 sample = m_draw.next(words);
                return sample <= m_chance;
            }
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

    enum class Four : crd::u8
    {
        ReceivedInGap,
        ReceivedInBurst,
        LostInBurst,
        LostInGap
    };

    // Where a word falls against two adjacent windows, [0, first) and (first, first + second): a word exactly on an
    // edge is in neither, and above the second is its own place.
    enum class Window : crd::u8
    {
        First,
        Second,
        Edge,
        Above
    };

    [[nodiscard]] static Window locate(crd::u32 word, crd::u32 first, crd::u32 second) noexcept
    {
        const crd::u64 upper = crd::u64{first} + second;
        if (word < first)
        {
            return Window::First;
        }
        if (word > first && word < upper)
        {
            return Window::Second;
        }
        return word > upper ? Window::Above : Window::Edge;
    }

    // The chain moves on the first word and the fate is read on the second, from the state the packet arrived in.
    template <LinkWords Words> [[nodiscard]] bool next_gilbert_elliott(Words& words) noexcept
    {
        if (m_bad)
        {
            const bool recovers = words.next_u32() < m_gilbert.r;
            const bool lost = words.next_u32() > m_gilbert.survive_bad;
            m_bad = !recovers;
            return lost;
        }
        const bool falls = words.next_u32() < m_gilbert.p;
        const bool lost = words.next_u32() < m_gilbert.lost_good;
        m_bad = falls;
        return lost;
    }

    // One word picks the move and the fate together. A word exactly on a window's edge moves nothing and loses nothing,
    // in every state; a lost-in-gap packet is followed by a return to a gap, which still takes its word.
    [[nodiscard]] bool next_four_state(crd::u32 word) noexcept
    {
        switch (m_at)
        {
            case Four::ReceivedInGap:
                switch (locate(word, m_four.p14, m_four.p13))
                {
                    case Window::First:
                        m_at = Four::LostInGap;
                        return true;
                    case Window::Second:
                        m_at = Four::LostInBurst;
                        return true;
                    case Window::Edge:
                    case Window::Above:
                        break;
                }
                return false;
            case Four::ReceivedInBurst:
                if (word < m_four.p23)
                {
                    m_at = Four::LostInBurst;
                    return true;
                }
                return false;
            case Four::LostInBurst:
                switch (locate(word, m_four.p32, m_four.p31))
                {
                    case Window::First:
                        m_at = Four::ReceivedInBurst;
                        return false;
                    case Window::Second:
                        m_at = Four::ReceivedInGap;
                        return false;
                    case Window::Above:
                        return true;
                    case Window::Edge:
                        break;
                }
                return false;
            case Four::LostInGap:
                break;
        }
        m_at = Four::ReceivedInGap;
        return false;
    }

    Kind m_kind = Kind::None;
    bool m_bad = false;
    Four m_at = Four::ReceivedInGap;
    crd::u32 m_chance = 0U;
    GilbertElliottChances m_gilbert;
    FourStateChances m_four;
    CorrelatedDraw m_draw;
};

// The scale of a distribution table: entries are standard scores times 8192, as in tc's .dist files.
inline constexpr crd::i32 kNetemTableScale = 8192;

// Means beyond this are held to it, which keeps every sum below within an i64 whatever a profile asks: 146 years.
inline constexpr crd::i64 kMaxLinkDelayNs = crd::i64{1} << 62U;

// The delay `tc` draws for a mean and a deviation, on chance arithmetic: an empty table is uniform in
// [mean - deviation, mean + deviation), otherwise the entry a word picks scales the deviation (entry / 8192 standard
// scores, the fraction rounded half away from zero). A deviation of zero or below is the mean and draws nothing. The
// answer can be below zero: the caller's clock treats that as now. A table has fewer than 2^32 entries (tc's 16,384 at
// most).
template <LinkWords Words>
[[nodiscard]] crd::i64 netem_delay(crd::i64 mean_ns, crd::i32 deviation_ns, CorrelatedDraw& draw, Words& words,
                                   crd::containers::ConstSpan<crd::i16> table) noexcept
{
    crd::i64 centre = mean_ns;
    if (centre < -kMaxLinkDelayNs)
    {
        centre = -kMaxLinkDelayNs;
    }
    else if (centre > kMaxLinkDelayNs)
    {
        centre = kMaxLinkDelayNs;
    }
    if (deviation_ns <= 0)
    {
        return centre;
    }
    const crd::u32 word = draw.next(words);
    const crd::i64 spread = deviation_ns;
    if (table.empty())
    {
        return centre - spread + static_cast<crd::i64>(word % (2U * static_cast<crd::u32>(deviation_ns)));
    }
    const crd::i64 entry = table[word % static_cast<crd::u32>(table.size())];
    // The spread is split into whole scales and a remainder so the remainder's product with an entry stays small; the
    // remainder's share is rounded to the nearest, ties away from zero, without a branch (a table's signs are as
    // random as its words).
    crd::i64 share = (spread % kNetemTableScale) * entry;
    constexpr crd::i64 half = kNetemTableScale / 2;
    share += half - ((share >> 63U) & (2 * half));
    return centre + ((spread / kNetemTableScale) * entry) + (share / kNetemTableScale);
}

enum class DelayShape : crd::u8
{
    Uniform,
    Normal,
    LogNormal,
    Pareto,
    ParetoNormal, // The tc mix: a normal and a Pareto of one rank, weighed by pareto_share (tc: 1/4 and 3/4).
    Table         // A measured distribution: delays at equally spaced ranks, read with linear interpolation.
};

struct DelayOptions
{
    DelayShape shape = DelayShape::Normal;
    crd::i64 mean_ns = 0;
    crd::i64 deviation_ns = 0;    // The standard deviation, in every analytic shape.
    crd::f64 correlation = 0.0;   // Lag-one, of the Gaussian that drives the shape: [0, 1).
    crd::f64 pareto_tail = 3.0;   // The Pareto shapes' exponent: above 2, where the deviation exists.
    crd::f64 pareto_share = 0.75; // ParetoNormal's weight of the Pareto: [0, 1]; the rest is the normal's.
    crd::i64 floor_ns = 0;        // Every answer is held between these.
    crd::i64 ceiling_ns = kMaxLinkDelayNs;
    // The Table shape's delays, ascending, ranks 0 to 1 at equal steps; read while the process lives. Mean and
    // deviation do not apply to it.
    crd::containers::ConstSpan<crd::i64> table{};
};

// Delay as an exact marginal law with a chosen correlation. Each draw takes one or two Box-Muller normals of the
// caller's Philox stream, through stats::NormalSampler.
class DelayProcess
{
public:
    explicit DelayProcess(crd::hesap::stats::PhiloxRng& rng) noexcept : m_normal(rng) {}

    // False, changing nothing, unless |mean| and deviation are within kMaxLinkDelayNs, deviation >= 0, correlation is
    // in [0, 1), floor <= ceiling, a log-normal with a deviation has a mean above zero, a Pareto's tail is above 2 and
    // at most 1e6 (past which it is an exponential), pareto_share is in [0, 1], and a table is non-empty, ascending and
    // within kMaxLinkDelayNs.
    // Restarts the correlation.
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
    crd::f64 m_mix_normal = 0.0;
    crd::f64 m_mix_pareto = 0.0;
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

// A model of the `slot` option of tc-netem(8), the bursts of a shared medium (Wi-Fi, LTE, cable): packets leave in
// slots, a slot closes on its packet or byte budget, and the next opens after a drawn gap. The decisions match
// netem's, as the header says: tc-netem(8) does not specify when a slot is stale or which words a slot draws, so those
// follow netem's behaviour.
class SlotGate
{
public:
    // False, changing nothing, for a negative delay, a minimum past the maximum, a negative budget or deviation. Delays
    // of zero and no table leave the gate open for good.
    [[nodiscard]] bool init(const SlotOptions& options, crd::i64 now_ns) noexcept
    {
        const bool delays_valid = options.min_delay_ns >= 0 && options.min_delay_ns <= options.max_delay_ns;
        const bool budgets_valid = options.max_packets >= 0 && options.max_bytes >= 0;
        if (!delays_valid || !budgets_valid || options.dist_deviation_ns < 0)
        {
            return false;
        }
        constexpr crd::i32 unbounded = std::numeric_limits<crd::i32>::max();
        m_options = options;
        if (m_options.max_packets == 0)
        {
            m_options.max_packets = unbounded;
        }
        if (m_options.max_bytes == 0)
        {
            m_options.max_bytes = unbounded;
        }
        // Without a delay or a distribution there is never a gap to wait for.
        m_active = (options.max_delay_ns != 0) || (options.min_delay_ns != 0) || (options.dist_deviation_ns != 0);
        m_next_ns = now_ns;
        m_packets_left = m_options.max_packets;
        m_bytes_left = m_options.max_bytes;
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
        const bool slot_stale = m_next_ns < time_to_send_ns;
        if (slot_stale)
        {
            open_next(now_ns, words);
        }
        const bool may_leave = time_to_send_ns <= now_ns && m_next_ns <= now_ns;
        if (!may_leave)
        {
            return false;
        }
        m_packets_left -= 1;
        m_bytes_left -= static_cast<crd::i64>(bytes);
        const bool slot_spent = m_packets_left <= 0 || m_bytes_left <= 0;
        if (slot_spent)
        {
            open_next(now_ns, words);
        }
        return true;
    }

private:
    // A gap from the distribution table, out of line: it is the long path, and keeping it out of admit leaves the
    // per-packet path as small as with the uniform gap.
    template <LinkWords Words> CRD_NOINLINE crd::i64 draw_table_gap(Words& words) noexcept
    {
        CorrelatedDraw uncorrelated;
        return netem_delay(m_options.dist_mean_ns, m_options.dist_deviation_ns, uncorrelated, words, m_options.table);
    }

    // The next slot opens a drawn gap after now: uniform on the two delays, or from the distribution table.
    template <LinkWords Words> void open_next(crd::i64 now_ns, Words& words) noexcept
    {
        crd::i64 gap = 0;
        if (m_options.table.empty())
        {
            // floor(word * width / 2^32) as the sum of the product's two halves: a width above 32 bits would overflow a
            // single 64-bit multiply.
            const crd::u64 width = static_cast<crd::u64>(m_options.max_delay_ns - m_options.min_delay_ns);
            const crd::u64 draw = words.next_u32();
            const crd::u64 scaled = (draw * (width >> 32U)) + ((draw * (width & 0xFFFFFFFFU)) >> 32U);
            gap = m_options.min_delay_ns + static_cast<crd::i64>(scaled);
        }
        else
        {
            gap = draw_table_gap(words);
        }
        // Adding the gap stops one short of each end of the clock instead of wrapping.
        constexpr crd::i64 latest = std::numeric_limits<crd::i64>::max();
        if (gap >= 0)
        {
            m_next_ns = now_ns > latest - gap ? latest : now_ns + gap;
        }
        else
        {
            m_next_ns = now_ns < -latest - gap ? -latest : now_ns + gap;
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

struct MediumGrantOptions
{
    DelayOptions gap;      // What separates two grants; its floor is at least zero.
    crd::i64 hold_ns = 0;  // A grant unused this long lapses; 0 is no limit.
    crd::u32 packets = 0U; // Per grant; 0 is no limit.
    crd::u64 bytes = 0U;   // Per grant; 0 is no limit. A packet that overruns it still leaves, and ends the grant.
};

// The bursts of a shared medium: packets leave only inside a grant. A grant closes when a budget is spent, and the next
// opens a drawn gap later; one that lapses unused makes the next packet wait a fresh gap from when it is ready, as a
// station that must contend again.
class MediumGrants
{
public:
    explicit MediumGrants(crd::hesap::stats::PhiloxRng& rng) noexcept : m_gap(rng) {}

    // False, changing nothing, for a negative hold, a gap's floor below zero or a gap DelayProcess refuses. The first
    // grant is open at now_ns.
    [[nodiscard]] bool init(const MediumGrantOptions& options, crd::i64 now_ns) noexcept;

    // When a packet ready at ready_ns leaves, for packets asked in queue order: never before it is ready, never earlier
    // than the packet before it.
    [[nodiscard]] crd::i64 depart(crd::i64 ready_ns, crd::u32 bytes) noexcept;

    [[nodiscard]] crd::i64 grant_ns() const noexcept { return m_grant_ns; }

private:
    void open_after(crd::i64 from_ns) noexcept;

    DelayProcess m_gap;
    crd::i64 m_hold_ns = 0;
    crd::u64 m_packet_budget = 0U;
    crd::u64 m_byte_budget = 0U;
    crd::u64 m_packets_left = 0U;
    crd::u64 m_bytes_left = 0U;
    crd::i64 m_grant_ns = 0;
};

} // namespace crd::hesap::net
