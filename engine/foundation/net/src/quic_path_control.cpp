// crd-net -- what one QUIC path controls of its sending (NET-PATH; contract:
// docs/design/network-foundation.md#multipath).
#include <crd/hesap/net/handover_phase.hpp>
#include <crd/net/quic_path_control.hpp>

#include <new>
#include <type_traits>

namespace crd::net
{
namespace
{
using HandoverLearner = hesap::net::HandoverPhaseLearner;

// The learner built in a control's slot (its constructor builds it first).
[[nodiscard]] HandoverLearner& learner_in(u8* bytes) noexcept
{
    return *std::launder(reinterpret_cast<HandoverLearner*>(bytes));
}

// Starts `learner` on `learning` (handover_learning_valid, so it takes it), or stops it for none.
void learn(HandoverLearner& learner, const HandoverLearning& learning) noexcept
{
    if (learning.period_ns == 0U || !learner.start(learning.period_ns))
    {
        learner.stop();
    }
}

[[nodiscard]] CongestionPacket congestion_packet(QuicSpace space, const QuicSentPacket& packet,
                                                 u64 received_ns = kNeverNs) noexcept
{
    CongestionPacket out;
    out.bytes = packet.bytes;
    out.number = packet.number;
    out.sent_ns = packet.sent_ns;
    out.received_ns = received_ns;
    out.space = space;
    out.delivery = packet.delivery;
    return out;
}

// The sampler's order of packets that left at one time: a coalesced datagram's later packet is in a later space.
[[nodiscard]] constexpr u64 delivery_order(QuicSpace space, u64 number) noexcept
{
    return (u64{quic_space_index(space)} << 62U) | number;
}
} // namespace

QuicPathControl::QuicPathControl() noexcept
{
    static_assert(sizeof(HandoverLearner) <= sizeof(m_learner.bytes) && alignof(HandoverLearner) <= 8U);
    static_assert(std::is_trivially_destructible_v<HandoverLearner>); // the slot is never destroyed
    new (m_learner.bytes) HandoverLearner();
}

QuicPathControl::~QuicPathControl() noexcept
{
    if (m_controller != nullptr)
    {
        m_controller->~CongestionController();
    }
}

bool QuicPathControl::start(const QuicPathControlConfig& config, const QuicPath& path, u64 entropy,
                            CongestionCoupling* coupling) noexcept
{
    if (!congestion_policy_valid(config.congestion) || config.max_datagram_bytes < kQuicMinInitialDatagram ||
        config.base_datagram_bytes < kQuicPmtuFloor || config.base_datagram_bytes > config.max_datagram_bytes)
    {
        return false;
    }
    m_config = config;
    m_coupling = coupling;
    m_path = path;
    m_ceiling = 0U;
    m_handover = config.handover;
    m_learning = config.handover_learning;
    HandoverLearner& learner = learner_in(m_learner.bytes);
    learn(learner, m_learning);
    m_learner_revision = learner.revision();
    m_pmtu.start(config.base_datagram_bytes, config.max_datagram_bytes, config.path_mtu_discovery);
    m_datagram_bytes = m_pmtu.size();
    m_ecn.start(codepoint());
    build(entropy);
    return true;
}

Ecn QuicPathControl::codepoint() const noexcept
{
    return m_config.ecn ? congestion_codepoint(m_config.congestion) : Ecn::NotEct;
}

void QuicPathControl::stop() noexcept
{
    if (m_controller != nullptr)
    {
        m_controller->~CongestionController();
        m_controller = nullptr;
    }
    m_pace_timer_ns = kNeverNs;
}

void QuicPathControl::build(u64 entropy) noexcept
{
    if (m_controller != nullptr)
    {
        m_controller->~CongestionController();
    }
    m_delivery.reset();
    m_pacer = Pacer{};
    m_pace_timer_ns = kNeverNs;
    // The policy and the size were checked (start).
    m_controller = make_congestion(m_config.congestion, m_datagram_bytes, nullptr, m_controller_slot, m_delivery,
                                   entropy, m_coupling);
    m_ecn_unavailable = false;
    follow_ecn();
}

void QuicPathControl::restart(const QuicPath& path, u32 base_datagram_bytes, u64 entropy) noexcept
{
    m_path = path;
    m_handover = HandoverSchedule{}; // the schedule was the old link's, and so was what the learner learned
    HandoverLearner& learner = learner_in(m_learner.bytes);
    if (learner.learning())
    {
        learner.restart();
    }
    m_learner_revision = learner.revision();
    const u32 base =
        (base_datagram_bytes < m_config.max_datagram_bytes) ? base_datagram_bytes : m_config.max_datagram_bytes;
    m_pmtu.start(base, m_config.max_datagram_bytes, m_config.path_mtu_discovery);
    if (m_ceiling != 0U)
    {
        m_pmtu.limit(m_ceiling);
    }
    m_datagram_bytes = m_pmtu.size();
    m_ecn.restart(codepoint());
    build(entropy);
}

void QuicPathControl::reset(u64 now_ns) noexcept
{
    m_delivery.reset();
    m_controller->reset(now_ns); // RFC 9002 section 6.3: congestion control starts over with recovery
    m_pacer = Pacer{};
}

bool QuicPathControl::set_congestion(const CongestionPolicy& policy, u64 entropy) noexcept
{
    if (!congestion_policy_valid(policy))
    {
        return false;
    }
    const CongestionState held = m_controller->state();
    m_controller->~CongestionController();
    m_controller = make_congestion(policy, m_datagram_bytes, &held, m_controller_slot, m_delivery, entropy, m_coupling);
    m_config.congestion = policy;
    m_ecn.set_codepoint(congestion_codepoint(policy));
    m_ecn_unavailable = false;
    follow_ecn();
    return true;
}

void QuicPathControl::limit(u32 ceiling) noexcept
{
    m_ceiling = ceiling;
    m_pmtu.limit(ceiling);
    apply_size();
}

bool QuicPathControl::set_handover(const HandoverSchedule& schedule) noexcept
{
    if (!handover_schedule_valid(schedule))
    {
        return false;
    }
    m_handover = schedule;
    m_learning = HandoverLearning{}; // an authored schedule ends learning
    HandoverLearner& learner = learner_in(m_learner.bytes);
    learner.stop();
    m_learner_revision = learner.revision();
    return true;
}

bool QuicPathControl::set_handover_learning(const HandoverLearning& learning) noexcept
{
    if (!handover_learning_valid(learning))
    {
        return false;
    }
    m_learning = learning;
    m_handover = HandoverSchedule{};
    HandoverLearner& learner = learner_in(m_learner.bytes);
    learn(learner, learning);
    m_learner_revision = learner.revision();
    return true;
}

bool QuicPathControl::follow_learner() noexcept
{
    const HandoverLearner& learner = learner_in(m_learner.bytes);
    if (!learner.learning() || learner.revision() == m_learner_revision)
    {
        return false;
    }
    m_learner_revision = learner.revision();
    const hesap::net::HandoverPhase& phase = learner.phase();
    HandoverSchedule schedule;
    if (phase.locked)
    {
        schedule = HandoverSchedule{m_learning.period_ns, phase.phase_ns, m_learning.before_ns, m_learning.after_ns};
    }
    const bool locked = phase.locked && m_handover.period_ns == 0U;
    m_handover = schedule;
    return locked;
}

Ecn QuicPathControl::marking(u64 now_ns, u64 testing_ns) noexcept
{
    const Ecn marking = m_ecn.marking(now_ns, testing_ns);
    follow_ecn();
    return marking;
}

u64 QuicPathControl::release_ns(u64 now_ns, const QuicRtt& rtt) const noexcept
{
    return m_pacer.release_ns(now_ns, m_controller->pacing_rate(rtt), m_controller->burst());
}

void QuicPathControl::arm_pace(u64 release_ns) noexcept
{
    m_pace_timer_ns = (release_ns < m_pace_timer_ns) ? release_ns : m_pace_timer_ns;
}

DeliveryStamp QuicPathControl::stamp(u64 bytes, u64 in_flight, u64 now_ns) noexcept
{
    return m_delivery.on_sent(bytes, in_flight, now_ns);
}

void QuicPathControl::on_sent(QuicSpace space, const QuicSentPacket& packet, bool controlled, u64 in_flight,
                              const QuicRtt& rtt, u64 now_ns) noexcept
{
    m_ecn.on_sent(space, packet.ecn, now_ns);
    if (controlled)
    {
        m_controller->on_sent(congestion_packet(space, packet), in_flight, now_ns);
        m_pacer.on_sent(packet.bytes, now_ns, m_controller->pacing_rate(rtt), m_controller->burst());
    }
}

void QuicPathControl::on_too_big(u32 quoted, u32 limit, u64 now_ns) noexcept
{
    static_cast<void>(m_pmtu.on_too_big(quoted, limit, now_ns));
    apply_size();
}

void QuicPathControl::on_acked(QuicSpace space, const QuicSentPacket& packet, u64 received_ns, u64 now_ns) noexcept
{
    if (packet.path_probe)
    {
        m_pmtu.on_probe_acked(packet.bytes, now_ns);
    }
    else if (space == QuicSpace::Application)
    {
        m_pmtu.on_acked(packet.number, packet.bytes);
    }
    if (packet.in_flight)
    {
        m_delivery.on_acked(packet.delivery, packet.bytes, packet.sent_ns, delivery_order(space, packet.number),
                            now_ns);
        // A packet a scheduled handover may have delayed carries no queue's delay.
        const u64 received = (space == QuicSpace::Application && !packet.handover) ? received_ns : kNeverNs;
        m_controller->on_acked(congestion_packet(space, packet, received), now_ns);
    }
}

QuicPathSignals QuicPathControl::on_lost(QuicSpace space, const QuicSentPacket& packet, u64 now_ns) noexcept
{
    QuicPathSignals signals;
    m_ecn.on_gone(packet.ecn);
    if (packet.path_probe)
    {
        // RFC 9000 section 14.4: a lost probe is no congestion signal.
        m_pmtu.on_probe_lost(packet.bytes, now_ns);
        return signals;
    }
    if (packet.in_flight)
    {
        // Covered or not: a learned schedule must keep seeing what the handovers do.
        learner_in(m_learner.bytes).on_loss(packet.sent_ns);
        signals.handover_lock = follow_learner();
    }
    if (packet.handover)
    {
        // Lost where a scheduled handover may have taken it: no congestion, and no black hole (StarQUIC section 3.2).
        signals.handover_loss = true;
        return signals;
    }
    if (space == QuicSpace::Application)
    {
        m_pmtu.on_lost(packet.number, packet.bytes);
    }
    if (packet.in_flight)
    {
        m_delivery.on_lost(packet.bytes);
        m_controller->on_lost(congestion_packet(space, packet), now_ns);
    }
    return signals;
}

void QuicPathControl::on_spurious(QuicSpace space, const QuicSentPacket& packet, u64 lost_ns, u64 now_ns) noexcept
{
    if (packet.in_flight)
    {
        // Its data was delivered after all (draft-ietf-ccwg-bbr-07 section 2.2's C.delivered).
        m_delivery.on_acked(packet.delivery, packet.bytes, packet.sent_ns, delivery_order(space, packet.number),
                            now_ns);
    }
    if (packet.path_probe)
    {
        // Its loss reduced nothing; its arrival proves its size.
        m_pmtu.on_probe_acked(packet.bytes, now_ns);
        return;
    }
    if (space == QuicSpace::Application)
    {
        m_pmtu.on_acked(packet.number, packet.bytes); // a large packet arrived: no black hole behind it
    }
    // A handover's loss reduced nothing, so its arrival undoes nothing.
    if (packet.in_flight && !packet.handover)
    {
        m_controller->on_spurious(congestion_packet(space, packet), lost_ns, now_ns);
    }
}

QuicPathSignals QuicPathControl::on_ack_end(QuicSpace space, const QuicAck& ack, const QuicAckOutcome& outcome,
                                            const QuicRtt& rtt, u64 in_flight, u64 now_ns) noexcept
{
    QuicPathSignals signals;
    m_delivery.on_ack_end(rtt.min_ns);
    const u64 ce = m_ecn.on_ack(space, ack, outcome);
    const u64 marked = outcome.ect0_packets + outcome.ect1_packets;
    if (m_ecn.state() == QuicEcnState::Capable && (marked != 0U || ce != 0U))
    {
        // RFC 9002 appendix B.7: before the frame's growth, so its packets grow nothing of the period.
        m_controller->on_ecn(marked, ce, outcome.newest_sent_ns, now_ns);
    }
    signals.ecn_signal = ce != 0U;
    follow_ecn();
    if (outcome.rtt_sampled)
    {
        // The sample is the newest packet's: the learner keys it by that packet's send time.
        learner_in(m_learner.bytes).on_rtt(outcome.newest_sent_ns, rtt.adjusted_ns);
        signals.handover_lock = follow_learner();
    }
    // An RTT a scheduled handover may have lengthened is the estimate's, never a delay signal.
    m_controller->on_ack_end(rtt, outcome.rtt_sampled && !outcome.rtt_handover, in_flight, now_ns);
    end_detection(outcome.lost, now_ns);
    return signals;
}

void QuicPathControl::end_detection(const QuicLoss& lost, u64 now_ns) noexcept
{
    if (lost.persistent_congestion)
    {
        m_controller->on_persistent_congestion(now_ns);
    }
    static_cast<void>(m_pmtu.end_detection(now_ns));
    apply_size();
}

void QuicPathControl::follow_ecn() noexcept
{
    const QuicEcnState state = m_ecn.state();
    if (!m_ecn_unavailable && (state == QuicEcnState::Off || state == QuicEcnState::Failed))
    {
        m_ecn_unavailable = true;
        m_controller->on_ecn_unavailable();
    }
}

void QuicPathControl::apply_size() noexcept
{
    const u32 bytes = m_pmtu.size();
    if (bytes == m_datagram_bytes)
    {
        return;
    }
    m_datagram_bytes = bytes;
    m_controller->set_max_datagram(bytes);
}
} // namespace crd::net
