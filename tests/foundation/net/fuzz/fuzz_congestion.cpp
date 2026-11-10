// crd-fuzz-net-congestion: congestion control and pacing of crd-net (NET-PATH;
// docs/design/network-foundation.md#congestion) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: byte 0 picks the policy (bits 0 and 7: CUBIC, NewReno or BBRv3, bit 1: HyStart++, bit 2: pacing, which BBR
// always has, bit 3: fast convergence, bits 4 to 6: the initial window's packets); byte 1 the datagram size step. Then
// operations of two bytes, an operation and its parameter, on one controller, the delivery rate sampler and one pacer,
// driven as a connection drives them: packets sent with their stamps, acknowledged in ACK frames with any RTT samples,
// lost, acknowledged after their loss, ECN marks, persistent congestion, sends held back and application-limited
// periods, time, live swaps through the three algorithms and resets, and the pacer's sends at its release times or
// before them.
//
// Oracles. The window stays between the minimum and kCongestionMaxWindow and the threshold at or above the minimum;
// avoidance holds a window at or above its threshold; reductions only rise, undos never pass them, and persistent
// congestion leaves the minimum window. A window controller nothing ever held back never grows past its initial window.
// Only an acknowledgement, an undo, a reset or a swap to BBR (whose floor is four datagrams) raises the window, and a
// reset restores the initial one. A live swap keeps the window, threshold and recovery period. The pacing rate is 0
// without pacing and never 0 for BBR, whose bandwidth is never above a rate the sampler gave and whose window after an
// ACK frame is at least four datagrams. The sampler's delivered bytes are what was acknowledged. The pacer's release
// time is never before now, its credit stays within one burst either side of zero and is positive at its release
// time.
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
    bool ever_limited = false;

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

void check(const World& w)
{
    const CongestionState state = w.ctl->state();
    const u64 minimum = w.policy.minimum_packets * w.datagram;
    CRD_FUZZ_REQUIRE(state.window == w.ctl->window());
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
    else
    {
        CRD_FUZZ_REQUIRE(state.bandwidth == 0U && state.min_rtt_ns == 0U);
    }
}

// The next algorithm of a live swap: CUBIC, NewReno, BBR, and round again.
[[nodiscard]] CongestionAlgorithm next_algorithm(CongestionAlgorithm algorithm)
{
    if (algorithm == CongestionAlgorithm::Cubic)
    {
        return CongestionAlgorithm::NewReno;
    }
    return (algorithm == CongestionAlgorithm::NewReno) ? CongestionAlgorithm::Bbr : CongestionAlgorithm::Cubic;
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
            w.ever_limited = w.ever_limited || w.in_flight + w.datagram > w.ctl->window();
            p.sent.delivery = w.delivery.on_sent(p.sent.bytes, w.in_flight, w.now);
            CRD_FUZZ_REQUIRE(p.sent.delivery.in_flight == w.in_flight && p.sent.delivery.delivered == w.acked);
            w.ctl->on_sent(p.sent, w.in_flight, w.now);
            break;
        }
        case 1U: // an ACK frame of up to four packets, with or without an RTT sample
        case 2U:
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
        case 5U: // an ECN mark up to a packet sent `parameter` ms ago
            w.ctl->on_congestion(w.now - ((w.now < parameter * kMs) ? w.now : parameter * kMs), w.now);
            break;
        case 6U:
            w.ctl->on_persistent_congestion(w.now);
            CRD_FUZZ_REQUIRE(w.ctl->window() == w.policy.minimum_packets * w.datagram);
            break;
        case 7U: // held back by the window or the pacer, or nothing to send
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
            w.policy.pacing = w.policy.pacing || is_bbr(w);
            w.ctl = make_congestion(w.policy, w.datagram, &held, w.slot, w.delivery, parameter);
            CRD_FUZZ_REQUIRE(w.ctl != nullptr);
            w.max_rate = 0U;
            const CongestionState now = w.ctl->state();
            CRD_FUZZ_REQUIRE(now.recovery_start_ns == held.recovery_start_ns && now.reductions == held.reductions);
            if (is_bbr(w))
            {
                const u64 floor = 4U * w.datagram;
                CRD_FUZZ_REQUIRE(now.window == ((held.window > floor) ? held.window : floor));
                may_grow = true;
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
                CRD_FUZZ_REQUIRE(w.ctl->window() == w.initial);
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
            const i64 limit = static_cast<i64>(burst);
            const i64 credit = w.pacer.credit(w.now, rate, burst);
            CRD_FUZZ_REQUIRE(credit <= limit && credit >= -limit);
            if (rate != 0U && release != w.now)
            {
                CRD_FUZZ_REQUIRE(credit <= 0);
                CRD_FUZZ_REQUIRE(w.pacer.release_ns(release, rate, burst) == release);
            }
            // Sends at the release time, or early when the parameter's top bit says so (a probe).
            const u64 at = ((parameter & 0x80U) != 0U) ? w.now : release;
            w.pacer.on_sent(w.datagram, at, rate, burst);
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
    w.policy.algorithm = ((data[0] & 0x80U) != 0U) ? CongestionAlgorithm::Bbr : w.policy.algorithm;
    w.policy.hystart = (data[0] & 2U) != 0U;
    w.policy.pacing = (data[0] & 4U) != 0U || is_bbr(w);
    w.policy.fast_convergence = (data[0] & 8U) != 0U;
    w.policy.initial_packets = 2U + ((data[0] >> 4U) & 7U) * 4U;
    w.datagram = 1200U + u64{data[1]} * 64U;
    w.ctl = make_congestion(w.policy, w.datagram, nullptr, w.slot, w.delivery, data[1]);
    CRD_FUZZ_REQUIRE(w.ctl != nullptr);
    w.initial = w.ctl->window();
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
}
