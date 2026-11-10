// crd-net -- QUIC acknowledgements and loss recovery (4.2a; contract: docs/design/network-foundation.md#quic-recovery).
#include <crd/net/quic_recovery.hpp>

namespace crd::net
{
void QuicRecoveryEvents::on_spurious(QuicSpace /*space*/, const QuicSentPacket& /*packet*/, u64 /*lost_ns*/) noexcept
{
}

namespace
{
// QuicSentEntry::state.
constexpr u8 kOutstanding  = 1U;
constexpr u8 kAckedPending = 2U; // acknowledged by the ACK frame being processed, its event not yet sent
constexpr u8 kAcked        = 3U;
constexpr u8 kLost         = 4U; // by the packet threshold alone
constexpr u8 kLostByTime   = 5U; // the time threshold held too

constexpr u64 kMaxU64 = ~u64{0};
// An RTT or ACK delay past 2^60 ns (36 years) counts as that, so the estimator's sums cannot wrap.
constexpr u64 kMaxRttNs = u64{1} << 60U;
constexpr u64 kNanosPerMicro = 1000U;

[[nodiscard]] constexpr u64 min_u64(u64 a, u64 b) noexcept
{
    return (a < b) ? a : b;
}

[[nodiscard]] constexpr u64 max_u64(u64 a, u64 b) noexcept
{
    return (a > b) ? a : b;
}

[[nodiscard]] constexpr u64 add_sat(u64 a, u64 b) noexcept
{
    return (a > kMaxU64 - b) ? kMaxU64 : a + b;
}

[[nodiscard]] constexpr u64 shl_sat(u64 value, u64 shift) noexcept
{
    if (value == 0U)
    {
        return 0U;
    }
    return (shift >= 64U || value > (kMaxU64 >> shift)) ? kMaxU64 : value << shift;
}

[[nodiscard]] constexpr u64 mul_sat(u64 a, u64 b) noexcept
{
    return (a != 0U && b > kMaxU64 / a) ? kMaxU64 : a * b;
}

[[nodiscard]] constexpr usize index_of(QuicSpace space) noexcept
{
    return static_cast<usize>(space);
}

[[nodiscard]] constexpr bool lost(u8 state) noexcept
{
    return state == kLost || state == kLostByTime;
}

[[nodiscard]] constexpr bool settled(u8 state) noexcept
{
    return state == kAcked || lost(state);
}

[[nodiscard]] constexpr bool acknowledged(u8 state) noexcept
{
    return state == kAcked || state == kAckedPending;
}
} // namespace

// ---- QuicAckTracker ----

void QuicAckTracker::reset(QuicSpace space) noexcept
{
    *this   = QuicAckTracker{};
    m_space = space;
}

void QuicAckTracker::insert(u64 number) noexcept
{
    usize at = m_count;
    for (usize i = 0U; i < m_count; ++i)
    {
        QuicAckRange& range = m_ranges[i];
        if (number == range.largest + 1U)
        {
            range.largest = number;
            if (i > 0U && m_ranges[i - 1U].smallest == number + 1U)
            {
                m_ranges[i - 1U].smallest = range.smallest;
                for (usize j = i; j + 1U < m_count; ++j)
                {
                    m_ranges[j] = m_ranges[j + 1U];
                }
                --m_count;
            }
            return;
        }
        if (number + 1U == range.smallest)
        {
            range.smallest = number;
            if (i + 1U < m_count && m_ranges[i + 1U].largest + 1U == number)
            {
                range.smallest = m_ranges[i + 1U].smallest;
                for (usize j = i + 1U; j + 1U < m_count; ++j)
                {
                    m_ranges[j] = m_ranges[j + 1U];
                }
                --m_count;
            }
            return;
        }
        if (number > range.largest)
        {
            at = i;
            break;
        }
    }
    if (m_count == kQuicAckRanges)
    {
        if (at == m_count)
        {
            return; // older than every range held: left out, as the smallest range is when the ranges are full
        }
        --m_count;
    }
    for (usize j = m_count; j > at; --j)
    {
        m_ranges[j] = m_ranges[j - 1U];
    }
    m_ranges[at] = QuicAckRange{number, number};
    ++m_count;
}

bool QuicAckTracker::on_received(u64 number, bool ack_eliciting, QuicEcn ecn, u64 received_ns,
                                 u64 max_ack_delay_ns) noexcept
{
    for (usize i = 0U; i < m_count; ++i)
    {
        if (number >= m_ranges[i].smallest && number <= m_ranges[i].largest)
        {
            return false;
        }
    }
    if (m_count == 0U || number > m_ranges[0].largest)
    {
        m_largest_ns = received_ns;
    }
    insert(number);

    if (ecn != QuicEcn::NotEct)
    {
        m_ecn_seen = true;
        if (ecn == QuicEcn::Ect0)
        {
            ++m_ecn.ect0;
        }
        else if (ecn == QuicEcn::Ect1)
        {
            ++m_ecn.ect1;
        }
        else
        {
            ++m_ecn.ce;
        }
    }
    if (!ack_eliciting)
    {
        return true;
    }

    bool immediate = m_space != QuicSpace::Application || ecn == QuicEcn::Ce;
    if (m_largest_eliciting != kQuicNoPacket && number < m_largest_eliciting)
    {
        immediate = true; // out of order
    }
    else
    {
        // A gap between the largest ack-eliciting packet and this one: the range holding it starts above the floor
        // (or it was left out of full ranges).
        const u64 floor = (m_largest_eliciting == kQuicNoPacket) ? 0U : m_largest_eliciting + 1U;
        bool      gap   = true;
        for (usize i = 0U; i < m_count; ++i)
        {
            if (number >= m_ranges[i].smallest && number <= m_ranges[i].largest)
            {
                gap = m_ranges[i].smallest > floor;
                break;
            }
        }
        immediate = immediate || gap;
        m_largest_eliciting = number;
    }
    ++m_unacked_eliciting;
    immediate = immediate || m_unacked_eliciting >= kQuicAckElicitingThreshold;
    const u64 due = immediate ? received_ns : add_sat(received_ns, max_ack_delay_ns);
    m_deadline_ns = min_u64(m_deadline_ns, due);
    return true;
}

QuicAckWrite QuicAckTracker::write(cont::Span<u8> out, u64 now_ns, u64 exponent) noexcept
{
    const u64 delay_us = (now_ns > m_largest_ns) ? (now_ns - m_largest_ns) / kNanosPerMicro : 0U;
    const u64 delay    = (exponent >= 64U) ? 0U : (delay_us >> exponent);
    const QuicEcnCounts* const ecn = m_ecn_seen ? &m_ecn : nullptr;
    for (usize count = m_count; count > 0U; --count)
    {
        const cont::ConstSpan<QuicAckRange> ranges(m_ranges, count);
        const usize                         bytes = quic_ack_bytes(ranges, delay, ecn);
        if (bytes != 0U && bytes <= out.size())
        {
            if (write_quic_ack(ranges, delay, ecn, out) != bytes)
            {
                return {};
            }
            m_unacked_eliciting = 0U;
            m_deadline_ns       = kNeverNs;
            return QuicAckWrite{bytes, m_ranges[0].largest};
        }
    }
    return {};
}

void QuicAckTracker::on_ack_acknowledged(u64 largest) noexcept
{
    if (m_count == 0U || m_ranges[0].largest == 0U)
    {
        return;
    }
    const u64 cut = min_u64(largest, m_ranges[0].largest - 1U);
    while (m_count > 0U && m_ranges[m_count - 1U].largest <= cut)
    {
        --m_count;
    }
    if (m_count > 0U && m_ranges[m_count - 1U].smallest <= cut)
    {
        m_ranges[m_count - 1U].smallest = cut + 1U;
    }
}

// ---- QuicRtt ----

u64 QuicRtt::probe_base_ns() const noexcept
{
    return smoothed_ns + max_u64(4U * variance_ns, kQuicGranularityNs);
}

void QuicRtt::sample(u64 latest, u64 ack_delay_ns, bool confirmed, u64 max_ack_delay_ns, u64 now_ns) noexcept
{
    latest_ns = min_u64(latest, kMaxRttNs);
    if (!sampled())
    {
        min_ns          = latest_ns;
        smoothed_ns     = latest_ns;
        variance_ns     = latest_ns / 2U;
        first_sample_ns = now_ns;
        return;
    }
    min_ns = min_u64(min_ns, latest_ns);
    u64 delay = min_u64(ack_delay_ns, kMaxRttNs);
    if (confirmed)
    {
        delay = min_u64(delay, max_ack_delay_ns);
    }
    // The delay is taken off only while the sample stays at or above min_rtt (section 5.3).
    const u64 adjusted = (latest_ns - min_ns >= delay) ? latest_ns - delay : latest_ns;
    const u64 deviation = (smoothed_ns > adjusted) ? smoothed_ns - adjusted : adjusted - smoothed_ns;
    variance_ns = (3U * variance_ns + deviation) / 4U;
    smoothed_ns = (7U * smoothed_ns + adjusted) / 8U;
}

// ---- QuicRecovery ----

QuicRecovery::QuicRecovery(const QuicRecoveryConfig& config, cont::Span<QuicSentEntry> initial,
                           cont::Span<QuicSentEntry> handshake, cont::Span<QuicSentEntry> application) noexcept
    : m_side(config.side),
      m_local_max_ack_delay_ns(config.local_max_ack_delay_ns),
      m_local_ack_delay_exponent(min_u64(config.local_ack_delay_exponent, kQuicMaxAckDelayExponent)),
      m_reserved_entries(config.reserved_entries)
{
    m_spaces[index_of(QuicSpace::Initial)].entries     = initial;
    m_spaces[index_of(QuicSpace::Handshake)].entries   = handshake;
    m_spaces[index_of(QuicSpace::Application)].entries = application;
    for (usize i = 0U; i < kQuicSpaces; ++i)
    {
        m_spaces[i].received.reset(static_cast<QuicSpace>(i));
    }
    m_rtt.smoothed_ns = min_u64(config.initial_rtt_ns, kMaxRttNs);
    m_rtt.variance_ns = m_rtt.smoothed_ns / 2U;
}

QuicSentEntry& QuicRecovery::entry(Space& space, usize index) noexcept
{
    usize at = space.head + index;
    if (at >= space.entries.size())
    {
        at -= space.entries.size();
    }
    return space.entries[at];
}

usize QuicRecovery::lower_bound(Space& space, u64 number) noexcept
{
    usize low  = 0U;
    usize high = space.count;
    while (low < high)
    {
        const usize mid = low + (high - low) / 2U;
        if (entry(space, mid).packet.number < number)
        {
            low = mid + 1U;
        }
        else
        {
            high = mid;
        }
    }
    return low;
}

bool QuicRecovery::acknowledges_unsent(Space& space, const QuicAck& ack) noexcept
{
    // Every number from the oldest entry up to the last sent is an entry unless this side never sent it, and a range
    // reaching the next number counts numbers no entry holds.
    const u64     oldest = (space.count > 0U) ? entry(space, 0U).packet.number : space.next_number;
    QuicAckRanges walk(ack);
    QuicAckRange  range;
    while (walk.next(range) && range.largest >= oldest)
    {
        const u64   from = max_u64(range.smallest, oldest);
        const usize held = lower_bound(space, range.largest + 1U) - lower_bound(space, from);
        if (static_cast<u64>(held) != range.largest - from + 1U)
        {
            return true;
        }
    }
    return false;
}

void QuicRecovery::settle(Space& space, QuicSentEntry& slot) noexcept
{
    if (slot.packet.in_flight)
    {
        m_bytes_in_flight -= slot.packet.bytes;
        if (slot.packet.ack_eliciting)
        {
            --space.eliciting_in_flight;
        }
    }
}

bool QuicRecovery::held_lost(QuicSpace id, const QuicSentEntry& slot, u64 now_ns) const noexcept
{
    return lost(slot.state) && now_ns - slot.lost_ns <= probe_period_ns(id); // times only rise
}

void QuicRecovery::compact(QuicSpace id, u64 now_ns, bool room) noexcept
{
    Space& space = m_spaces[index_of(id)];
    while (space.count > 0U && settled(entry(space, 0U).state) &&
           ((room && space.count >= space.entries.size()) || !held_lost(id, entry(space, 0U), now_ns)))
    {
        space.head = (space.head + 1U == space.entries.size()) ? 0U : space.head + 1U;
        --space.count;
    }
    if (space.count == 0U)
    {
        space.head = 0U;
    }
}

bool QuicRecovery::peer_validated() const noexcept
{
    return m_side == crypto::QuicSide::Server || m_confirmed || m_handshake_acked;
}

bool QuicRecovery::any_eliciting_in_flight() const noexcept
{
    usize eliciting = 0U;
    for (const Space& space : m_spaces)
    {
        eliciting += space.eliciting_in_flight;
    }
    return eliciting != 0U;
}

u64 QuicRecovery::decoded_ack_delay_ns(QuicSpace id, u64 raw) const noexcept
{
    if (id == QuicSpace::Initial)
    {
        return 0U; // an Initial ACK is never delayed on purpose (RFC 9002 section 5.3)
    }
    const u64 exponent = (id == QuicSpace::Application) ? m_peer_ack_delay_exponent : kQuicDefaultAckDelayExponent;
    return mul_sat(shl_sat(raw, exponent), kNanosPerMicro);
}

u64 QuicRecovery::probe_period_ns(QuicSpace space) const noexcept
{
    const u64 base = shl_sat(m_rtt.probe_base_ns(), m_pto_count);
    return (space == QuicSpace::Application) ? add_sat(base, shl_sat(m_peer_max_ack_delay_ns, m_pto_count)) : base;
}

u64 QuicRecovery::loss_delay_ns() const noexcept
{
    const u64 rtt    = max_u64(m_rtt.latest_ns, m_rtt.smoothed_ns);
    const u64 window = min_u64(mul_sat(m_reorder_steps, m_rtt.min_ns / 4U), m_rtt.smoothed_ns);
    return max_u64(add_sat(rtt * kQuicTimeThresholdNumerator / kQuicTimeThresholdDivisor, window),
                   kQuicGranularityNs);
}

u64 QuicRecovery::persistent_congestion_ns() const noexcept
{
    return mul_sat(add_sat(m_rtt.probe_base_ns(), m_peer_max_ack_delay_ns), kQuicPersistentCongestionThreshold);
}

bool QuicRecovery::earliest_loss(u64& time_ns, QuicSpace& id) const noexcept
{
    time_ns = kNeverNs;
    for (usize i = 0U; i < kQuicSpaces; ++i)
    {
        if (m_spaces[i].loss_time_ns < time_ns)
        {
            time_ns = m_spaces[i].loss_time_ns;
            id      = static_cast<QuicSpace>(i);
        }
    }
    return time_ns != kNeverNs;
}

u64 QuicRecovery::pto_time(u64 now_ns, QuicSpace& id) const noexcept
{
    if (!any_eliciting_in_flight())
    {
        id = m_handshake_keys ? QuicSpace::Handshake : QuicSpace::Initial;
        return add_sat(now_ns, probe_period_ns(id));
    }
    u64 time = kNeverNs;
    for (usize i = 0U; i < kQuicSpaces; ++i)
    {
        const QuicSpace candidate = static_cast<QuicSpace>(i);
        if (m_spaces[i].eliciting_in_flight == 0U)
        {
            continue;
        }
        if (candidate == QuicSpace::Application && !m_confirmed)
        {
            break; // the Application space's timeout waits for confirmation (section 6.2.1)
        }
        const u64 at = add_sat(m_spaces[i].last_eliciting_ns, probe_period_ns(candidate));
        if (at < time)
        {
            time = at;
            id   = candidate;
        }
    }
    return time;
}

void QuicRecovery::arm(u64 now_ns) noexcept
{
    u64       time = kNeverNs;
    QuicSpace id   = QuicSpace::Initial;
    if (earliest_loss(time, id))
    {
        m_timer_ns = time;
        return;
    }
    if (m_side == crypto::QuicSide::Server && m_amplification_blocked)
    {
        m_timer_ns = kNeverNs; // a datagram from the client unblocks it
        return;
    }
    if (!any_eliciting_in_flight() && peer_validated())
    {
        m_timer_ns = kNeverNs;
        return;
    }
    m_timer_ns = pto_time(now_ns, id);
}

void QuicRecovery::set_peer_parameters(u64 max_ack_delay_ns, u64 ack_delay_exponent, u64 now_ns) noexcept
{
    m_peer_max_ack_delay_ns   = max_ack_delay_ns;
    m_peer_ack_delay_exponent = min_u64(ack_delay_exponent, kQuicMaxAckDelayExponent);
    arm(now_ns);
}

void QuicRecovery::confirm_handshake(u64 now_ns) noexcept
{
    m_confirmed = true;
    arm(now_ns);
}

void QuicRecovery::set_handshake_keys(u64 now_ns) noexcept
{
    m_handshake_keys = true;
    arm(now_ns);
}

void QuicRecovery::set_amplification_blocked(bool blocked, u64 now_ns) noexcept
{
    m_amplification_blocked = blocked;
    arm(now_ns);
}

bool QuicRecovery::discard(QuicSpace id, u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    if (id == QuicSpace::Application)
    {
        return false;
    }
    Space& space = m_spaces[index_of(id)];
    if (space.discarded)
    {
        return true;
    }
    drop_outstanding(id, events);
    space.discarded = true;
    space.received.reset(id);
    m_pto_count = 0U;
    arm(now_ns);
    return true;
}

void QuicRecovery::retry(u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    // Only the first flight measures the path: after a probe the Retry may answer a later packet.
    Space& initial = m_spaces[index_of(QuicSpace::Initial)];
    const u64 first_ns = (initial.count != 0U && m_pto_count == 0U) ? entry(initial, 0U).packet.sent_ns : now_ns;
    for (usize id = 0U; id < kQuicSpaces; ++id)
    {
        drop_outstanding(static_cast<QuicSpace>(id), events);
    }
    if (!m_rtt.sampled() && now_ns > first_ns)
    {
        m_rtt.smoothed_ns = min_u64(now_ns - first_ns, kMaxRttNs);
        m_rtt.variance_ns = m_rtt.smoothed_ns / 2U;
    }
    m_pto_count = 0U;
    arm(now_ns);
}

void QuicRecovery::reject_early(u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    drop_outstanding(QuicSpace::Application, events);
    m_pto_count = 0U;
    arm(now_ns);
}

void QuicRecovery::drop_outstanding(QuicSpace id, QuicRecoveryEvents& events) noexcept
{
    Space& space = m_spaces[index_of(id)];
    for (usize i = 0U; i < space.count; ++i)
    {
        QuicSentEntry& slot = entry(space, i);
        if (slot.state == kOutstanding)
        {
            settle(space, slot);
            slot.state = kLost;
            events.on_discarded(id, slot.packet);
        }
    }
    space.head = 0U;
    space.count = 0U;
    space.loss_time_ns = kNeverNs;
    space.last_eliciting_ns = 0U;
}

bool QuicRecovery::can_send(QuicSpace id) const noexcept
{
    const Space& space = m_spaces[index_of(id)];
    return !space.discarded && (space.count < space.entries.size() || settled(space.entries[space.head].state));
}

bool QuicRecovery::can_send_data(QuicSpace id) const noexcept
{
    const Space& space = m_spaces[index_of(id)];
    return !space.discarded &&
           (space.count + m_reserved_entries < space.entries.size() || settled(space.entries[space.head].state));
}

QuicRecoveryError QuicRecovery::on_sent(QuicSpace id, const QuicSentPacket& packet) noexcept
{
    Space& space = m_spaces[index_of(id)];
    if (space.discarded)
    {
        return QuicRecoveryError::Discarded;
    }
    if (packet.number < space.next_number || packet.number > crypto::kQuicMaxPacketNumber)
    {
        return QuicRecoveryError::BadNumber;
    }
    if (space.count >= space.entries.size())
    {
        compact(id, packet.sent_ns, true); // a lost packet held for a late acknowledgement gives up its slot
        if (space.count >= space.entries.size())
        {
            return QuicRecoveryError::Full;
        }
    }
    QuicSentEntry& slot = entry(space, space.count);
    slot.packet         = packet;
    slot.packet.in_flight = packet.in_flight || packet.ack_eliciting; // an ack-eliciting packet is in flight
    slot.state          = kOutstanding;
    ++space.count;
    space.next_number = packet.number + 1U;
    if (slot.packet.in_flight)
    {
        m_bytes_in_flight += packet.bytes;
        if (packet.ack_eliciting)
        {
            ++space.eliciting_in_flight;
            space.last_eliciting_ns = packet.sent_ns;
        }
        arm(packet.sent_ns);
    }
    return QuicRecoveryError::None;
}

QuicAckWrite QuicRecovery::write_ack(QuicSpace id, cont::Span<u8> out, u64 now_ns) noexcept
{
    Space& space = m_spaces[index_of(id)];
    if (space.discarded)
    {
        return {};
    }
    const u64 exponent = (id == QuicSpace::Application) ? m_local_ack_delay_exponent : kQuicDefaultAckDelayExponent;
    return space.received.write(out, now_ns, exponent);
}

bool QuicRecovery::on_received(QuicSpace id, u64 number, bool ack_eliciting, QuicEcn ecn, u64 received_ns) noexcept
{
    Space& space = m_spaces[index_of(id)];
    if (space.discarded)
    {
        return false;
    }
    const u64 max_delay = (id == QuicSpace::Application) ? m_local_max_ack_delay_ns : 0U;
    return space.received.on_received(number, ack_eliciting, ecn, received_ns, max_delay);
}

QuicLoss QuicRecovery::detect_lost(QuicSpace id, u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    Space&   space = m_spaces[index_of(id)];
    QuicLoss loss;
    space.loss_time_ns = kNeverNs;
    if (space.largest_acked == kQuicNoPacket)
    {
        return loss;
    }
    const u64 loss_delay = loss_delay_ns();
    const u64 congestion = persistent_congestion_ns();

    // A run of lost ack-eliciting packets sent after the first RTT sample with no acknowledged packet between
    // establishes persistent congestion when its send times span more than the duration (section 7.6.2).
    u64 run_first = kNeverNs;
    u64 run_last  = 0U;
    const auto close_run = [&]()
    {
        if (run_first != kNeverNs && run_last - run_first > congestion)
        {
            loss.persistent_congestion = true;
        }
        run_first = kNeverNs;
    };
    for (usize i = 0U; i < space.count; ++i)
    {
        QuicSentEntry&        slot   = entry(space, i);
        const QuicSentPacket& packet = slot.packet;
        if (packet.number > space.largest_acked)
        {
            break;
        }
        if (acknowledged(slot.state))
        {
            close_run();
            continue;
        }
        if (slot.state != kOutstanding)
        {
            continue;
        }
        const bool by_time   = now_ns >= packet.sent_ns && now_ns - packet.sent_ns >= loss_delay;
        const bool by_number = space.largest_acked - packet.number >= m_packet_threshold;
        if (!by_time && !by_number)
        {
            space.loss_time_ns = min_u64(space.loss_time_ns, add_sat(packet.sent_ns, loss_delay));
            continue;
        }
        settle(space, slot);
        slot.state   = by_time ? kLostByTime : kLost;
        slot.lost_ns = now_ns;
        ++loss.packets;
        if (packet.in_flight)
        {
            loss.bytes += packet.bytes;
        }
        loss.newest_sent_ns = max_u64(loss.newest_sent_ns, packet.sent_ns);
        if (packet.ack_eliciting && m_rtt.sampled() && packet.sent_ns >= m_rtt.first_sample_ns)
        {
            run_first = min_u64(run_first, packet.sent_ns);
            run_last  = max_u64(run_last, packet.sent_ns);
        }
        events.on_lost(id, packet);
    }
    close_run();
    // RFC 8985 section 6.2, step 4: the raised thresholds persist for 16 loss detections with no spurious loss.
    if (loss.packets != 0U && m_reorder_persist != 0U)
    {
        --m_reorder_persist;
        if (m_reorder_persist == 0U)
        {
            m_packet_threshold = kQuicPacketThreshold;
            m_reorder_steps    = 0U;
        }
    }
    return loss;
}

QuicAckOutcome QuicRecovery::on_ack(QuicSpace id, const QuicAck& ack, u64 now_ns,
                                    QuicRecoveryEvents& events) noexcept
{
    QuicAckOutcome out;
    Space&         space = m_spaces[index_of(id)];
    if (space.discarded)
    {
        out.error = QuicRecoveryError::Discarded;
        return out;
    }
    if (acknowledges_unsent(space, ack))
    {
        out.error = QuicRecoveryError::AckOfUnsent;
        return out;
    }
    if (id == QuicSpace::Handshake)
    {
        m_handshake_acked = true;
    }
    const u64 prior_largest = space.largest_acked;
    space.largest_acked =
        (space.largest_acked == kQuicNoPacket) ? ack.largest : max_u64(space.largest_acked, ack.largest);

    // Mark what this frame newly acknowledges; the events follow the losses (appendix A.7). A packet held after it
    // was declared lost is acknowledged without an event and measures the reordering (section 6.1).
    u64  largest_newly = kQuicNoPacket;
    u64  largest_sent  = 0U;
    u64  reordering    = 0U;
    u64  carried       = kQuicNoPacket; // the largest of the ACK frames those packets carried
    bool by_time       = false;
    bool eliciting     = false;
    {
        QuicAckRanges walk(ack);
        QuicAckRange  range;
        while (walk.next(range))
        {
            const usize end = lower_bound(space, range.largest + 1U);
            for (usize i = lower_bound(space, range.smallest); i < end; ++i)
            {
                QuicSentEntry& slot = entry(space, i);
                if (held_lost(id, slot, now_ns))
                {
                    reordering = max_u64(reordering, prior_largest - slot.packet.number + 1U);
                    by_time    = by_time || slot.state == kLostByTime;
                    slot.state = kAcked;
                    ++out.spurious_packets;
                    out.spurious_bytes += slot.packet.in_flight ? slot.packet.bytes : 0U;
                    events.on_spurious(id, slot.packet, slot.lost_ns);
                }
                else if (slot.state == kOutstanding)
                {
                    settle(space, slot);
                    slot.state = kAckedPending;
                    ++out.acked_packets;
                    if (slot.packet.in_flight)
                    {
                        out.acked_bytes += slot.packet.bytes;
                    }
                }
                else
                {
                    continue;
                }
                eliciting = eliciting || slot.packet.ack_eliciting;
                if (largest_newly == kQuicNoPacket || slot.packet.number > largest_newly)
                {
                    largest_newly = slot.packet.number;
                    largest_sent  = slot.packet.sent_ns;
                }
                if (slot.packet.carried_ack != kQuicNoPacket &&
                    (carried == kQuicNoPacket || slot.packet.carried_ack > carried))
                {
                    carried = slot.packet.carried_ack;
                }
            }
        }
    }
    if (out.acked_packets == 0U && out.spurious_packets == 0U)
    {
        return out;
    }
    if (out.spurious_packets != 0U)
    {
        // RFC 8985 section 6.2, step 4, before this frame's detection: the packet threshold to the reordering seen,
        // the time threshold's window one step a round trip for a loss the time threshold declared.
        m_packet_threshold = min_u64(max_u64(m_packet_threshold, reordering), kQuicMaxPacketThreshold);
        if (by_time && space.largest_acked >= space.reorder_round && m_reorder_steps < kQuicMaxReorderSteps)
        {
            ++m_reorder_steps;
            space.reorder_round = space.next_number;
        }
        m_reorder_persist = kQuicReorderPersist;
    }
    if (largest_newly == ack.largest && eliciting)
    {
        const u64 latest = (now_ns > largest_sent) ? now_ns - largest_sent : 0U;
        m_rtt.sample(latest, decoded_ack_delay_ns(id, ack.delay), m_confirmed, m_peer_max_ack_delay_ns, now_ns);
        out.rtt_sampled = true;
    }
    out.lost = detect_lost(id, now_ns, events);

    QuicAckRanges walk(ack);
    QuicAckRange  range;
    while (walk.next(range))
    {
        const usize end = lower_bound(space, range.largest + 1U);
        for (usize i = lower_bound(space, range.smallest); i < end; ++i)
        {
            QuicSentEntry& slot = entry(space, i);
            if (slot.state != kAckedPending)
            {
                continue;
            }
            slot.state = kAcked;
            events.on_acked(id, slot.packet);
        }
    }
    if (carried != kQuicNoPacket)
    {
        space.received.on_ack_acknowledged(carried);
    }
    compact(id, now_ns, false);
    if (peer_validated())
    {
        m_pto_count = 0U;
    }
    arm(now_ns);
    return out;
}

u64 QuicRecovery::next_timer_ns() const noexcept
{
    u64 time = m_timer_ns;
    for (const Space& space : m_spaces)
    {
        if (!space.discarded)
        {
            time = min_u64(time, space.received.ack_deadline_ns());
        }
    }
    return time;
}

QuicTimeout QuicRecovery::on_timeout(u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    QuicTimeout out;
    if (m_timer_ns == kNeverNs || now_ns < m_timer_ns)
    {
        return out;
    }
    u64       time = kNeverNs;
    QuicSpace id   = QuicSpace::Initial;
    if (earliest_loss(time, id))
    {
        out.kind  = QuicTimeoutKind::Loss;
        out.space = id;
        out.lost  = detect_lost(id, now_ns, events);
        compact(id, now_ns, false);
        arm(now_ns);
        return out;
    }
    if (!any_eliciting_in_flight())
    {
        out.kind   = QuicTimeoutKind::AntiDeadlock;
        out.space  = m_handshake_keys ? QuicSpace::Handshake : QuicSpace::Initial;
        out.probes = 1U;
    }
    else
    {
        (void)pto_time(now_ns, id);
        out.kind   = QuicTimeoutKind::Probe;
        out.space  = id;
        out.probes = 2U;
    }
    if (m_pto_count < 64U)
    {
        ++m_pto_count; // the backoff saturates long before: an idle timeout ends the connection
    }
    arm(now_ns);
    return out;
}

u64 QuicRecovery::largest_acked(QuicSpace space) const noexcept
{
    return m_spaces[index_of(space)].largest_acked;
}

u64 QuicRecovery::ack_deadline_ns(QuicSpace space) const noexcept
{
    const Space& held = m_spaces[index_of(space)];
    return held.discarded ? kNeverNs : held.received.ack_deadline_ns();
}

const QuicAckTracker& QuicRecovery::received(QuicSpace space) const noexcept
{
    return m_spaces[index_of(space)].received;
}

} // namespace crd::net
