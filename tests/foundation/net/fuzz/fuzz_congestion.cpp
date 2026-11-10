// crd-fuzz-net-congestion: congestion control and pacing of crd-net (NET-PATH;
// docs/design/network-foundation.md#congestion) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: byte 0 picks the policy (bits 0 and 7: CUBIC, NewReno, BBRv3 or Prague, SCReAM v2 with bits 3 and 7, and GCC
// with bits 2, 3 and 7, bit 1: HyStart++, bit 2: pacing, which BBR, Prague, SCReAM and GCC always have, bit 3: fast
// convergence, bits 4 to 6: the initial window's packets; for SCReAM bit 0 is L4S and bit 1 jitter reduction); byte 1
// the datagram size step, for SCReAM its link loss tolerance (bit 0) and no delay target adjustment (bit 1), and for
// SCReAM and GCC target rate bounds (bit 2).
// Then operations of two bytes, an operation and its parameter, on one controller, the delivery rate sampler and one
// pacer, driven as a connection drives them: packets sent with their stamps, acknowledged in ACK frames with any RTT
// samples and the peer's receive times (on any clock), lost, acknowledged after their loss, ECN feedback (marked
// packets and CE, a peer's inflated CE too) and a path without ECN, persistent congestion, sends held back and
// application-limited periods, time, live swaps through the six algorithms and resets, and the pacer's sends at its
// release times or before them.
//
// Oracles. The window stays between the minimum and kCongestionMaxWindow and the threshold at or above the minimum;
// avoidance holds a window at or above its threshold; reductions only rise, undos never pass them, and persistent
// congestion leaves the minimum window. A window controller nothing ever held back, with no BBR run before it, never
// grows past its initial window. Only an acknowledgement, an undo, a reset, a swap to BBR (whose floor is four
// datagrams) or BBR leaving ProbeRTT on a restart from idle raises the window, and a reset restores the initial one.
// A live swap keeps the window, threshold and recovery period. The pacing rate is 0 without pacing and never 0 for
// BBR, whose bandwidth is never above a rate the sampler gave and whose window after an ACK frame is at least four
// datagrams; Prague's alpha stays within 0 and 1, its classic share within 0 and 2^16, and a mark's reduction takes
// at most half the window. SCReAM's reference window stays at or above MIN_REF_WND, grows only to 1.5 times the
// largest flight since it was built (or its first window), and lets 1.5 to 3 of it be in flight; its target rate
// stays within the policy's bounds, its delay target within QDELAY_TARGET_LO and _HI, and it paces at least 1.5 times
// RATE_PACE_MIN. GCC's window stays at or above its 3000-byte floor and halves on persistent congestion; its target
// rate stays within the policy's bounds (its 5 kbit/s floor included) and under its delay-based estimate, and it paces
// at 1.1 or 2.5 times that target. The sampler's delivered bytes are what was acknowledged. The pacer's release time is never
// before now, its credit stays within one burst above zero and the last send's burst below, and is positive at its
// release time.
#include <crd/core/platform.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/congestion.hpp>

#include <new>

namespace
{
using crd::i64;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::net;

constexpr usize kPackets = 256U;
constexpr u64 kMs = 1000000U;

struct Packet
{
    CongestionPacket sent;
    u64 lost_ns = 0U;
    u8 state = 0U; // 0 outstanding, 1 acknowledged, 2 lost, 3 lost then acknowledged, 4 discarded
};

// MSVC C4324 (structure padded due to alignment specifier): the slot's 16-byte alignment rounds the world up, and
// that padding is the point. ARM64 MSVC reports it here whatever the member order; the x64 compiler does not.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct World
{
    CongestionSlot slot;
    DeliveryRateSampler delivery;
    CongestionPolicy policy;
    u64 datagram = 1200U;
    CongestionController* ctl = nullptr;
    Pacer pacer;
    Packet packets[kPackets];
    usize count = 0U;
    u64 number = 0U;
    u64 now = 0U;
    u64 in_flight = 0U;
    u64 initial = 0U;
    u64 reductions = 0U;
    u64 acked = 0U;    // bytes acknowledged, spurious ones too: the sampler's delivered bytes
    u64 max_rate = 0U; // the highest rate the sampler gave since the controller was built
    u64 paced_burst = 0U; // the burst of the pacer's last send
    bool ever_limited = false; // held back, or a BBR, SCReAM or GCC ran: a window may pass the initial one
    u64 grow_base = 0U;        // SCReAM: its reference window when built, swapped in or reset
    u64 max_in_flight = 0U;    // SCReAM: the largest flight since then

    ~World()
    {
        if (ctl != nullptr)
        {
            ctl->~CongestionController();
        }
    }
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

[[nodiscard]] bool is_bbr(const World& w)
{
    return w.policy.algorithm == CongestionAlgorithm::Bbr;
}

[[nodiscard]] bool is_prague(const World& w)
{
    return w.policy.algorithm == CongestionAlgorithm::Prague;
}

[[nodiscard]] bool is_scream(const World& w)
{
    return w.policy.algorithm == CongestionAlgorithm::Scream;
}

[[nodiscard]] bool is_gcc(const World& w)
{
    return w.policy.algorithm == CongestionAlgorithm::Gcc;
}

// GCC's least window: 3000 bytes (libwebrtc's two 1500-byte packets), or the policy's minimum window when larger.
[[nodiscard]] u64 gcc_floor(const World& w)
{
    const u64 minimum = w.policy.minimum_packets * w.datagram;
    return (minimum > 3000U) ? minimum : 3000U;
}

// SCReAM's least reference window: MIN_REF_WND, or the policy's minimum window when larger.
[[nodiscard]] u64 scream_floor(const World& w)
{
    const u64 minimum = w.policy.minimum_packets * w.datagram;
    return (minimum > 3000U) ? minimum : 3000U;
}

// The window a controller starts with: RFC 9002's, or SCReAM's reference window or GCC's window at least its floor.
[[nodiscard]] u64 initial_state_window(const World& w)
{
    const u64 initial = congestion_initial_window(w.policy, w.datagram);
    if (is_gcc(w))
    {
        return (gcc_floor(w) > initial) ? gcc_floor(w) : initial;
    }
    return (is_scream(w) && scream_floor(w) > initial) ? scream_floor(w) : initial;
}

// SCReAM was built, swapped in or reset: its growth is bounded from here.
void scream_started(World& w)
{
    w.grow_base = w.ctl->state().window;
    w.max_in_flight = w.in_flight;
    w.ever_limited = w.ever_limited || is_scream(w) || is_gcc(w);
}

// SCReAM's own bounds: its reference window, the room in flight it gives, its target rate and delay target.
void check_scream(const World& w, const CongestionState& state)
{
    const u64 allowed = w.datagram + w.max_in_flight * 3U / 2U;
    CRD_FUZZ_REQUIRE(state.window >= scream_floor(w));
    CRD_FUZZ_REQUIRE(state.window <= ((w.grow_base > allowed) ? w.grow_base : allowed));
    CRD_FUZZ_REQUIRE(state.slow_start_threshold == state.window);
    const u64 room = w.ctl->window();
    const u64 least = state.window * 3U / 2U;
    CRD_FUZZ_REQUIRE(room >= ((least < kCongestionMaxWindow) ? least : kCongestionMaxWindow));
    CRD_FUZZ_REQUIRE(room >= 2U * w.datagram);
    CRD_FUZZ_REQUIRE(room <= state.window * 3U || room == 2U * w.datagram);
    CRD_FUZZ_REQUIRE(state.target_rate >= w.policy.target_rate_min);
    CRD_FUZZ_REQUIRE(w.policy.target_rate_max == 0U || state.target_rate <= w.policy.target_rate_max);
    CRD_FUZZ_REQUIRE(state.queue_delay_target_ns >= w.policy.queue_delay_target_ns &&
                     state.queue_delay_target_ns <= kScreamQueueDelayTargetMaxNs);
    CRD_FUZZ_REQUIRE(state.alpha <= kCongestionAlphaOne && state.classic == 0U);
    QuicRtt rtt;
    rtt.smoothed_ns = 1U + (w.now % (500U * kMs));
    CRD_FUZZ_REQUIRE(w.ctl->pacing_rate(rtt) >= kScreamMinPacingRate * 3U / 2U && w.ctl->burst() >= w.datagram);
}

// GCC's own bounds: its window, its target rate under its estimate and within the policy's bounds, and its pacing.
void check_gcc(const World& w, const CongestionState& state)
{
    CRD_FUZZ_REQUIRE(state.window == w.ctl->window() && state.window >= gcc_floor(w));
    CRD_FUZZ_REQUIRE(state.slow_start_threshold == state.window);
    const u64 floor = (w.policy.target_rate_min > 625U) ? w.policy.target_rate_min : 625U;
    const u64 most = (w.policy.target_rate_max != 0U) ? w.policy.target_rate_max : ~u64{0};
    const u64 least = (floor < most) ? floor : most; // a lower maximum wins over the floor
    CRD_FUZZ_REQUIRE(state.target_rate >= least && state.target_rate <= state.bandwidth);
    CRD_FUZZ_REQUIRE(w.policy.target_rate_max == 0U || state.target_rate <= w.policy.target_rate_max);
    CRD_FUZZ_REQUIRE(state.bandwidth >= least && state.bandwidth <= 12500000000U);
    CRD_FUZZ_REQUIRE(state.queue_delay_ns == 0U && state.queue_delay_target_ns == 0U && state.min_rtt_ns == 0U);
    const u64 pace = w.ctl->pacing_rate(QuicRtt{});
    CRD_FUZZ_REQUIRE(pace == state.target_rate * 110U / 100U || pace == state.target_rate * 250U / 100U);
    CRD_FUZZ_REQUIRE(w.ctl->burst() >= 2U * w.datagram);
}

void check(const World& w)
{
    const CongestionState state = w.ctl->state();
    const u64 minimum = w.policy.minimum_packets * w.datagram;
    if (is_scream(w))
    {
        check_scream(w, state);
    }
    else if (is_gcc(w))
    {
        check_gcc(w, state);
    }
    else
    {
        CRD_FUZZ_REQUIRE(state.window == w.ctl->window());
        CRD_FUZZ_REQUIRE(state.target_rate == 0U && state.queue_delay_ns == 0U && state.queue_delay_target_ns == 0U);
    }
    CRD_FUZZ_REQUIRE(state.window >= minimum && state.window <= kCongestionMaxWindow);
    CRD_FUZZ_REQUIRE(state.slow_start_threshold >= minimum);
    CRD_FUZZ_REQUIRE(state.phase != CongestionPhase::Avoidance || state.window >= state.slow_start_threshold);
    CRD_FUZZ_REQUIRE(state.reductions >= w.reductions && state.undone <= state.reductions);
    CRD_FUZZ_REQUIRE(is_bbr(w) || w.ever_limited || state.window <= w.initial);
    CRD_FUZZ_REQUIRE(w.delivery.delivered() == w.acked);
    QuicRtt rtt;
    rtt.smoothed_ns = 1U + (w.now % (500U * kMs));
    CRD_FUZZ_REQUIRE(w.policy.pacing || w.ctl->pacing_rate(rtt) == 0U);
    if (is_bbr(w))
    {
        CRD_FUZZ_REQUIRE(w.ctl->pacing_rate(rtt) != 0U && w.ctl->burst() >= 2U * w.datagram);
        CRD_FUZZ_REQUIRE(state.bandwidth <= w.max_rate);
    }
    else if (!is_gcc(w))
    {
        CRD_FUZZ_REQUIRE(state.bandwidth == 0U && state.min_rtt_ns == 0U);
    }
    if (is_prague(w))
    {
        CRD_FUZZ_REQUIRE(w.ctl->pacing_rate(rtt) != 0U && w.ctl->burst() >= w.datagram);
        CRD_FUZZ_REQUIRE(state.alpha <= kCongestionAlphaOne && state.classic <= (crd::u32{1} << 16U));
    }
    else if (!is_scream(w))
    {
        CRD_FUZZ_REQUIRE(state.alpha == 0U && state.classic == 0U);
    }
}

// The next algorithm of a live swap: CUBIC, NewReno, BBR, Prague, SCReAM, GCC, and round again.
[[nodiscard]] CongestionAlgorithm next_algorithm(CongestionAlgorithm algorithm)
{
    switch (algorithm)
    {
        case CongestionAlgorithm::Cubic:
            return CongestionAlgorithm::NewReno;
        case CongestionAlgorithm::NewReno:
            return CongestionAlgorithm::Bbr;
        case CongestionAlgorithm::Bbr:
            return CongestionAlgorithm::Prague;
        case CongestionAlgorithm::Prague:
            return CongestionAlgorithm::Scream;
        case CongestionAlgorithm::Scream:
            return CongestionAlgorithm::Gcc;
        default:
            return CongestionAlgorithm::Cubic;
    }
}

Packet* pick(World& w, u8 parameter, u8 state)
{
    for (usize i = 0U; i < w.count; ++i)
    {
        Packet& candidate = w.packets[(parameter + i) % w.count];
        if (candidate.state == state)
        {
            return &candidate;
        }
    }
    return nullptr;
}

void run(World& w, u8 op, u8 parameter)
{
    const u64 before = w.ctl->window();
    bool may_grow = false;
    switch (op % 12U)
    {
        case 0U: // send
        {
            if (w.count == kPackets)
            {
                break;
            }
            Packet& p = w.packets[w.count++];
            p.sent.bytes = w.datagram - (parameter % 64U);
            p.sent.number = w.number++;
            p.sent.sent_ns = w.now;
            p.sent.space = (parameter & 0x80U) != 0U ? QuicSpace::Handshake : QuicSpace::Application;
            w.in_flight += p.sent.bytes;
            w.max_in_flight = (w.in_flight > w.max_in_flight) ? w.in_flight : w.max_in_flight;
            w.ever_limited = w.ever_limited || w.in_flight + w.datagram > w.ctl->window();
            p.sent.delivery = w.delivery.on_sent(p.sent.bytes, w.in_flight, w.now);
            CRD_FUZZ_REQUIRE(p.sent.delivery.in_flight == w.in_flight && p.sent.delivery.delivered == w.acked);
            const CongestionPhase phase = w.ctl->state().phase;
            w.ctl->on_sent(p.sent, w.in_flight, w.now);
            // BBR restarting from idle may end ProbeRTT and restore its window (draft section 5.4).
            may_grow = is_bbr(w) && (phase == CongestionPhase::ProbeRtt || phase == CongestionPhase::Recovery) &&
                       w.ctl->state().phase != CongestionPhase::ProbeRtt;
            break;
        }
        case 1U: // an ACK frame of up to four packets, with or without an RTT sample, with receive times when bit 5 is
        case 2U: // set: the RTT bits' milliseconds after sending each later packet, on a clock bit 4 puts 2^62 ns ahead
        {
            w.delivery.on_ack_begin();
            for (u8 k = 0U; k <= (parameter & 3U); ++k)
            {
                Packet* p = pick(w, static_cast<u8>(parameter + k), 0U);
                if (p != nullptr)
                {
                    p->state = 1U;
                    w.in_flight -= p->sent.bytes;
                    w.acked += p->sent.bytes;
                    w.delivery.on_acked(p->sent.delivery, p->sent.bytes, p->sent.sent_ns, p->sent.number, w.now);
                    const u64 offset = ((parameter & 0x10U) != 0U) ? u64{1} << 62U : 0U;
                    p->sent.received_ns = ((parameter & 0x20U) != 0U)
                                              ? p->sent.sent_ns + offset + (k + 1U) * (parameter >> 2U) * kMs
                                              : kNeverNs;
                    w.ctl->on_acked(p->sent, w.now);
                }
            }
            QuicRtt rtt;
            rtt.latest_ns = (parameter >> 2U) * kMs;
            rtt.min_ns = rtt.latest_ns;
            rtt.smoothed_ns = rtt.latest_ns;
            const bool sampled = (parameter & 0x40U) != 0U;
            w.delivery.on_ack_end(sampled ? rtt.min_ns : 0U);
            const DeliverySample& sample = w.delivery.sample();
            CRD_FUZZ_REQUIRE(sample.rate == 0U || (sample.acked && sample.interval_ns >= (sampled ? rtt.min_ns : 0U)));
            w.max_rate = (sample.rate > w.max_rate) ? sample.rate : w.max_rate;
            w.ctl->on_ack_end(rtt, sampled, w.in_flight, w.now);
            CRD_FUZZ_REQUIRE(!is_bbr(w) || !sample.acked || w.ctl->window() >= 4U * w.datagram);
            may_grow = true;
            break;
        }
        case 3U: // lost
        {
            Packet* p = pick(w, parameter, 0U);
            if (p != nullptr)
            {
                p->state = 2U;
                p->lost_ns = w.now;
                w.in_flight -= p->sent.bytes;
                w.delivery.on_lost(p->sent.bytes);
                w.ctl->on_lost(p->sent, w.now);
            }
            break;
        }
        case 4U: // acknowledged after its loss
        {
            Packet* p = pick(w, parameter, 2U);
            if (p != nullptr)
            {
                p->state = 3U;
                w.acked += p->sent.bytes;
                w.delivery.on_acked(p->sent.delivery, p->sent.bytes, p->sent.sent_ns, p->sent.number, w.now);
                w.ctl->on_spurious(p->sent, p->lost_ns, w.now);
                may_grow = true; // an undo
            }
            break;
        }
        case 5U: // ECN feedback: CE on the top two bits' packets of those plus the low three bits' marked ones (CE
                 // above the marked count when they are 0: an inflating peer), up to a packet sent the low six bits'
                 // ms ago
        {
            const u64 ce = parameter >> 6U;
            const u64 marked = (parameter & 7U) == 0U ? 0U : ce + (parameter & 7U);
            const u64 ago = (parameter & 0x3FU) * kMs;
            if (marked != 0U || ce != 0U)
            {
                const u64 window = w.ctl->window();
                w.ctl->on_ecn(marked, ce, w.now - ((w.now < ago) ? w.now : ago), w.now);
                const u64 floor = w.policy.minimum_packets * w.datagram;
                CRD_FUZZ_REQUIRE(!is_prague(w) || w.ctl->window() >= ((window / 2U > floor) ? window / 2U : floor) ||
                                 w.ctl->window() == window);
            }
            break;
        }
        case 6U:
        {
            const u64 halved = w.ctl->window() / 2U;
            w.ctl->on_persistent_congestion(w.now);
            if (is_gcc(w))
            {
                CRD_FUZZ_REQUIRE(w.ctl->window() == ((halved > gcc_floor(w)) ? halved : gcc_floor(w)));
                break;
            }
            CRD_FUZZ_REQUIRE(is_scream(w) ? w.ctl->state().window == scream_floor(w)
                                          : w.ctl->window() == w.policy.minimum_packets * w.datagram);
            break;
        }
        case 7U: // held back by the window or the pacer, or nothing to send; or the path carries no ECN
            if ((parameter & 4U) != 0U)
            {
                w.ctl->on_ecn_unavailable();
            }
            if ((parameter & 1U) != 0U)
            {
                w.delivery.on_app_limited(w.in_flight);
                CRD_FUZZ_REQUIRE(w.delivery.app_limited());
                break;
            }
            w.ever_limited = true;
            w.ctl->on_limited((parameter & 2U) != 0U, w.now);
            break;
        case 8U:
            w.now += (u64{parameter} * parameter) * kMs / 4U;
            break;
        case 9U: // a live swap to the next algorithm
        {
            const CongestionState held = w.ctl->state();
            w.ctl->~CongestionController();
            w.policy.algorithm = next_algorithm(w.policy.algorithm);
            w.policy.pacing = w.policy.pacing || is_bbr(w) || is_prague(w) || is_scream(w) || is_gcc(w);
            w.ctl = make_congestion(w.policy, w.datagram, &held, w.slot, w.delivery, parameter);
            CRD_FUZZ_REQUIRE(w.ctl != nullptr);
            w.max_rate = 0U;
            const CongestionState now = w.ctl->state();
            CRD_FUZZ_REQUIRE(now.recovery_start_ns == held.recovery_start_ns && now.reductions == held.reductions);
            if (is_scream(w))
            {
                CRD_FUZZ_REQUIRE(now.window == ((held.window > scream_floor(w)) ? held.window : scream_floor(w)));
                scream_started(w);
                may_grow = true; // what may be in flight is 1.5 to 3 reference windows
                break;
            }
            if (is_gcc(w))
            {
                CRD_FUZZ_REQUIRE(now.window == ((held.window > gcc_floor(w)) ? held.window : gcc_floor(w)));
                scream_started(w);
                may_grow = true;
                break;
            }
            if (is_bbr(w))
            {
                const u64 floor = 4U * w.datagram;
                CRD_FUZZ_REQUIRE(now.window == ((held.window > floor) ? held.window : floor));
                may_grow = true;
                w.ever_limited = true; // BBR grows its window by the model, held back or not (section 5.6.4.6)
                break;
            }
            CRD_FUZZ_REQUIRE(now.window == held.window && now.slow_start_threshold == held.slow_start_threshold);
            break;
        }
        case 10U:
            if (parameter == 0U)
            {
                // A Retry: recovery discards every packet, and delivery and the controller start over.
                for (usize i = 0U; i < w.count; ++i)
                {
                    w.packets[i].state = 4U;
                }
                w.in_flight = 0U;
                w.acked = 0U;
                w.max_rate = 0U;
                w.delivery.reset();
                w.ctl->reset(w.now);
                CRD_FUZZ_REQUIRE(w.ctl->state().window == initial_state_window(w));
                scream_started(w);
                may_grow = true;
            }
            break;
        default: // the pacer
        {
            QuicRtt rtt;
            rtt.smoothed_ns = (1U + parameter) * kMs;
            const u64 rate = w.ctl->pacing_rate(rtt);
            const u64 burst = w.ctl->burst();
            const u64 release = w.pacer.release_ns(w.now, rate, burst);
            CRD_FUZZ_REQUIRE(release >= w.now);
            // Debt is bounded by the burst of the send that ran it up: BBR's send quantum moves with its rate.
            const i64 limit = static_cast<i64>(burst);
            const i64 debt_limit = static_cast<i64>((w.paced_burst > burst) ? w.paced_burst : burst);
            const i64 credit = w.pacer.credit(w.now, rate, burst);
            CRD_FUZZ_REQUIRE(credit <= limit && credit >= -debt_limit);
            if (rate != 0U && release != w.now)
            {
                CRD_FUZZ_REQUIRE(credit <= 0);
                CRD_FUZZ_REQUIRE(w.pacer.release_ns(release, rate, burst) == release);
            }
            // Sends at the release time, or early when the parameter's top bit says so (a probe).
            const u64 at = ((parameter & 0x80U) != 0U) ? w.now : release;
            w.pacer.on_sent(w.datagram, at, rate, burst);
            w.paced_burst = burst;
            w.now = (at > w.now) ? at : w.now;
            break;
        }
    }
    CRD_FUZZ_REQUIRE(may_grow || w.ctl->window() <= before);
    w.reductions = w.ctl->state().reductions;
    check(w);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    alignas(World) static unsigned char storage[sizeof(World)];
    static World* held = nullptr;
    if (held != nullptr)
    {
        held->~World();
    }
    held = new (storage) World();
    World& w = *held;
    w.policy.algorithm = ((data[0] & 1U) != 0U) ? CongestionAlgorithm::NewReno : CongestionAlgorithm::Cubic;
    if ((data[0] & 0x80U) != 0U)
    {
        w.policy.algorithm = ((data[0] & 1U) != 0U) ? CongestionAlgorithm::Prague : CongestionAlgorithm::Bbr;
    }
    if ((data[0] & 0x8CU) == 0x8CU)
    {
        w.policy.algorithm = CongestionAlgorithm::Gcc;
        if ((data[1] & 4U) != 0U)
        {
            w.policy.target_rate_min = 50000U;
            w.policy.target_rate_max = 400000U;
        }
    }
    else if ((data[0] & 0x88U) == 0x88U)
    {
        w.policy.algorithm = CongestionAlgorithm::Scream;
        w.policy.scalable_ecn = (data[0] & 1U) != 0U;
        w.policy.reduce_jitter = (data[0] & 2U) != 0U;
        w.policy.tolerate_link_loss = (data[1] & 1U) != 0U;
        w.policy.compete = (data[1] & 2U) == 0U;
        if ((data[1] & 4U) != 0U)
        {
            w.policy.target_rate_min = 50000U;
            w.policy.target_rate_max = 400000U;
        }
    }
    w.policy.hystart = (data[0] & 2U) != 0U;
    w.policy.pacing = (data[0] & 4U) != 0U || is_bbr(w) || is_prague(w) || is_scream(w) || is_gcc(w);
    w.ever_limited = is_bbr(w);
    w.policy.fast_convergence = (data[0] & 8U) != 0U;
    w.policy.initial_packets = 2U + ((data[0] >> 4U) & 7U) * 4U;
    w.datagram = 1200U + u64{data[1]} * 64U;
    w.ctl = make_congestion(w.policy, w.datagram, nullptr, w.slot, w.delivery, data[1]);
    CRD_FUZZ_REQUIRE(w.ctl != nullptr);
    w.initial = w.ctl->window();
    scream_started(w);
    check(w);
    for (usize at = 2U; at + 1U < size; at += 2U)
    {
        run(w, data[at], data[at + 1U]);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A CUBIC flight with HyStart++ and pacing: ten sends, acknowledgements with rising RTTs, a loss, its spurious
    // acknowledgement, a swap, persistent congestion and the pacer at its release times.
    u8 cubic[2U + 2U * 48U] = {0x06U, 0x00U};
    usize at = 2U;
    const auto put = [&](u8* into, u8 op, u8 p)
    {
        into[at++] = op;
        into[at++] = p;
    };
    for (u8 i = 0U; i < 10U; ++i)
    {
        put(cubic, 0x00U, i);
    }
    for (u8 i = 0U; i < 8U; ++i)
    {
        put(cubic, 0x08U, 0x04U);
        put(cubic, 0x01U, static_cast<u8>(0x40U | ((100U + 4U * i) & 0x3CU)));
    }
    put(cubic, 0x03U, 0x00U);
    put(cubic, 0x04U, 0x00U);
    put(cubic, 0x09U, 0x00U);
    put(cubic, 0x06U, 0x00U);
    for (u8 i = 0U; i < 10U; ++i)
    {
        put(cubic, 0x0BU, static_cast<u8>(20U + i));
    }
    put(cubic, 0x0AU, 0x00U);
    sink.add("cubic-flight", cubic, at);
    // NewReno without pacing on a 1500-byte path: sends held back, an ECN mark and avoidance.
    u8 reno[2U + 2U * 40U] = {0x31U, 0x05U};
    at = 2U;
    for (u8 i = 0U; i < 12U; ++i)
    {
        put(reno, 0x00U, static_cast<u8>(i * 3U));
    }
    put(reno, 0x07U, 0x00U);
    for (u8 i = 0U; i < 12U; ++i)
    {
        put(reno, 0x02U, static_cast<u8>(0x43U + i));
    }
    put(reno, 0x05U, 0x10U);
    put(reno, 0x08U, 0x20U);
    for (u8 i = 0U; i < 13U; ++i)
    {
        put(reno, 0x00U, i);
    }
    sink.add("reno-avoidance", reno, at);
    // BBRv3 on a steady path: a window sent and acknowledged each 20 ms round with RTT samples through Startup and
    // Drain into ProbeBW, a loss and its spurious acknowledgement, an application-limited period, an ECN mark, time
    // past the ten-second min RTT filter (ProbeRTT) and a swap.
    u8 bbr[2U + 2U * 120U] = {0x86U, 0x00U};
    at = 2U;
    for (u8 round = 0U; round < 12U; ++round)
    {
        for (u8 i = 0U; i < 4U; ++i)
        {
            put(bbr, 0x00U, 0x00U);
        }
        put(bbr, 0x08U, 0x09U); // 20 ms
        put(bbr, 0x01U, static_cast<u8>(0x40U | 0x50U | 3U));
        put(bbr, 0x02U, static_cast<u8>(0x40U | 0x50U | 3U));
    }
    put(bbr, 0x00U, 0x00U);
    put(bbr, 0x03U, 0x00U);
    put(bbr, 0x04U, 0x00U);
    put(bbr, 0x07U, 0x01U);
    put(bbr, 0x05U, 0x01U);
    put(bbr, 0x08U, 0xFFU); // 16 s
    for (u8 i = 0U; i < 6U; ++i)
    {
        put(bbr, 0x00U, 0x00U);
        put(bbr, 0x01U, static_cast<u8>(0x40U | 0x50U));
    }
    put(bbr, 0x09U, 0x00U);
    sink.add("bbr-cycle", bbr, at);
    // Prague over 5 ms rounds: slow start, marked feedback each round (one CE in the first, then none and some), a
    // loss within the mark's round trip, a path without ECN and a swap.
    u8 prague[2U + 2U * 80U] = {0x87U, 0x00U};
    at = 2U;
    for (u8 round = 0U; round < 10U; ++round)
    {
        for (u8 i = 0U; i < 4U; ++i)
        {
            put(prague, 0x00U, 0x00U);
        }
        put(prague, 0x08U, 0x04U); // 4 ms
        put(prague, 0x05U, static_cast<u8>(((round % 3U == 0U) ? 0x40U : 0x00U) | 0x04U));
        put(prague, 0x01U, static_cast<u8>(0x40U | 0x14U | 3U));
    }
    put(prague, 0x03U, 0x00U);
    put(prague, 0x07U, 0x04U);
    put(prague, 0x09U, 0x00U);
    sink.add("prague-marks", prague, at);
    // SCReAM v2 in L4S mode with jitter reduction, link loss tolerance and target rate bounds on 1520-byte datagrams:
    // 20 ms rounds acknowledged with 20 ms samples, then samples queued to 63 ms (past half the delay target), marks,
    // a loss, persistent congestion, a Retry and a swap.
    u8 scream[2U + 2U * 200U] = {0xABU, 0x05U};
    at = 2U;
    for (u8 round = 0U; round < 24U; ++round)
    {
        for (u8 i = 0U; i < 4U; ++i)
        {
            put(scream, 0x00U, 0x00U);
        }
        put(scream, 0x08U, 0x09U); // 20 ms
        put(scream, 0x05U, static_cast<u8>(((round % 4U == 3U) ? 0x40U : 0x00U) | 0x04U));
        put(scream, 0x01U, (round < 12U) ? u8{0x53U} : u8{0xFFU});
        put(scream, 0x02U, (round < 12U) ? u8{0x53U} : u8{0xFFU});
    }
    put(scream, 0x03U, 0x00U);
    put(scream, 0x01U, 0x53U);
    put(scream, 0x06U, 0x00U);
    put(scream, 0x0AU, 0x00U);
    put(scream, 0x09U, 0x00U);
    sink.add("scream-delay", scream, at);
    // GCC with target rate bounds: 4 ms rounds of four packets acknowledged with receive times spread by the RTT bits,
    // wider in the later rounds, frames without receive times, losses past 10% and CE, the receive clock
    // 2^62 ns ahead, persistent congestion, the pacer, a Retry and a swap.
    u8 gcc[2U + 2U * 300U] = {0x8EU, 0x04U};
    at = 2U;
    for (u8 round = 0U; round < 40U; ++round)
    {
        for (u8 i = 0U; i < 4U; ++i)
        {
            put(gcc, 0x00U, 0x00U);
        }
        put(gcc, 0x08U, 0x04U); // 4 ms
        const u8 spread = (round < 20U) ? u8{0x00U} : static_cast<u8>(((round - 19U) & 3U) << 2U);
        put(gcc, 0x01U, static_cast<u8>(0x60U | spread | 3U));
    }
    for (u8 round = 0U; round < 6U; ++round)
    {
        for (u8 i = 0U; i < 4U; ++i)
        {
            put(gcc, 0x00U, 0x00U);
        }
        put(gcc, 0x03U, 0x00U);
        put(gcc, 0x05U, 0x41U);
        put(gcc, 0x01U, ((round % 2U) == 0U) ? u8{0x43U} : u8{0x73U});
    }
    put(gcc, 0x06U, 0x00U);
    put(gcc, 0x0BU, 0x10U);
    put(gcc, 0x0AU, 0x00U);
    put(gcc, 0x09U, 0x00U);
    sink.add("gcc-delay", gcc, at);
}
