// Loss and delay of a simulated link (HESAP-NET, capability numerics.link-models, owner crd-hesap-net): what NET-SIM
// and NET-DEBUG draw per packet, on the seeded Philox stream, so a whole run is a pure function of (seed, stream).
//
// Written from published models, not from any implementation of them:
//   * loss is one hidden Markov chain in three settings: independent loss, Gilbert-Elliott (E. N. Gilbert, Capacity of
//     a burst-noise channel, BSTJ 39, 1960; E. O. Elliott, Estimates of error rates for codes on burst-noise channels,
//     BSTJ 42, 1963) and the general and intuitive four-state model (S. Salsano, F. Ludovici, A. Ordine, Definition of
//     a general and intuitive loss model for packet networks, 2010; four states after A. D. Clark, Modeling the effects
//     of burst packet loss and recency on subjective voice quality, IPtel 2001);
//   * delay is a Gaussian AR(1) that drives every shape through its own quantile function, so the marginal law is exact
//     at any correlation (a Gaussian copula); correlated loss is the two-state chain of
//     gilbert_from_rate_and_correlation(), with the asked rate and lag-one correlation;
//   * a shared medium (Wi-Fi, LTE, cable) is a series of grants, each opened by a drawn gap and closed by its budget.
//
// Nothing allocates. Word sources are anything with next_u32(); production passes stats::PhiloxRng.

#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/stats/normal.hpp>

#include <concepts>

namespace crd::hesap::net
{

template <typename Words>
concept LinkWords = requires(Words& words) {
    { words.next_u32() } -> std::same_as<crd::u32>;
};

// A chance in units of 2^-32: a uniform 32-bit word below it hits, so zero never hits, kLinkCertain always does, and
// every value between is that probability exactly on every platform.
using LinkChance = crd::u64;
inline constexpr LinkChance kLinkCertain = LinkChance{1} << 32U;

// The chance nearest a fraction. Below 0 and NaN are never, above 1 is certain.
[[nodiscard]] LinkChance link_chance(crd::f64 fraction) noexcept;

// Gilbert-Elliott: a good and a bad state, each with its own chance of losing a packet.
struct GilbertElliottChances
{
    LinkChance to_bad = 0U;
    LinkChance to_good = 0U;
    LinkChance lost_in_good = 0U;
    LinkChance lost_in_bad = kLinkCertain;
};

// The general and intuitive model, by its paper's transition names: state 1 is a packet received in a gap, 2 received
// in a burst, 3 lost in a burst, 4 an isolated loss in a gap, and state 4 always returns to 1.
struct FourStateChances
{
    LinkChance p13 = 0U;
    LinkChance p14 = 0U;
    LinkChance p23 = 0U;
    LinkChance p31 = 0U;
    LinkChance p32 = 0U;
};

// A simple Gilbert chain (no loss in good, all lost in bad) by what it means: the long-run loss fraction and the mean
// length of a burst of losses. False, writing nothing, unless 0 <= loss < 1 and mean_burst >= 1 and the entry chance
// they imply is at most 1.
[[nodiscard]] bool gilbert_from_rate_and_burst(crd::f64 loss, crd::f64 mean_burst, GilbertElliottChances& out) noexcept;

// A simple Gilbert chain with the long-run loss fraction and the lag-one correlation of loss events. False, writing
// nothing, unless 0 <= loss < 1 and 0 <= correlation < 1.
[[nodiscard]] bool gilbert_from_rate_and_correlation(crd::f64 loss, crd::f64 correlation,
                                                     GilbertElliottChances& out) noexcept;

// One packet's fate: the chain moves on one word, then the packet takes the state it is in, on a second word. Every
// packet draws exactly two words whatever the setting, so a stream's position depends only on the packet count. Every
// setting starts in its first state (good, or received in a gap). Default-constructed it loses nothing.
class LossChain
{
public:
    void set_independent(LinkChance lost) noexcept
    {
        m_states[0] = State{.lost = lost};
        m_at = 0U;
    }

    void set_gilbert_elliott(const GilbertElliottChances& chances) noexcept
    {
        m_states[0] = State{.leave_below = chances.to_bad, .lost = chances.lost_in_good, .leave_to = 1U};
        m_states[1] = State{.leave_below = chances.to_good, .lost = chances.lost_in_bad, .leave_to = 0U};
        m_at = 0U;
    }

    // False, changing nothing, when a chance or a state's outgoing sum passes certainty.
    [[nodiscard]] bool set_four_state(const FourStateChances& chances) noexcept;

    // True when this packet is lost.
    template <LinkWords Words> [[nodiscard]] bool next(Words& words) noexcept
    {
        const LinkChance move = words.next_u32();
        const LinkChance fate = words.next_u32();
        const State& state = m_states[m_at];
        if (move < state.leave_below)
        {
            m_at = state.leave_to;
        }
        else if (move < state.other_below)
        {
            m_at = state.other_to;
        }
        return fate < m_states[m_at].lost;
    }

private:
    // A move word below leave_below goes to leave_to, else below other_below goes to other_to, else the chain stays.
    struct State
    {
        LinkChance leave_below = 0U;
        LinkChance other_below = 0U;
        LinkChance lost = 0U;
        crd::u8 leave_to = 0U;
        crd::u8 other_to = 0U;
    };

    State m_states[4] = {};
    crd::u8 m_at = 0U;
};

// Means beyond this are refused, which keeps every sum below within an i64 whatever a profile asks: 146 years.
inline constexpr crd::i64 kMaxLinkDelayNs = crd::i64{1} << 62U;

enum class DelayShape : crd::u8
{
    Uniform,
    Normal,
    LogNormal,
    Pareto,
    ParetoNormal, // A normal and a Pareto of the same rank, in standard units, weighed by pareto_share.
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
    // within kMaxLinkDelayNs. Restarts the correlation.
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
