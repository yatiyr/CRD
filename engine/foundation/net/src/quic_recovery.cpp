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

// ---- QuicReceiveTimestampLog ----

void QuicReceiveTimestampLog::configure(u64 per_ack, u64 exponent) noexcept
{
    m_per_ack = static_cast<usize>(min_u64(per_ack, kQuicMaxReceiveTimestamps));
    m_exponent = min_u64(exponent, kQuicMaxReceiveTimestampsExponent);
    m_count = (m_per_ack == 0U) ? 0U : m_count;
}

void QuicReceiveTimestampLog::record(u64 number, u64 received_ns) noexcept
{
    if (m_per_ack == 0U)
    {
        return;
    }
    if (m_basis_ns == kNeverNs)
    {
        m_basis_ns = (received_ns == 0U) ? 0U : received_ns - 1U; // below every time reported (section 6.1)
    }
    m_entries[m_next] = Entry{number, received_ns};
    m_next = (m_next + 1U == kQuicMaxReceiveTimestamps) ? 0U : m_next + 1U;
    m_count += (m_count < kQuicMaxReceiveTimestamps) ? 1U : 0U;
}

usize QuicReceiveTimestampLog::take(cont::ConstSpan<QuicAckRange> ranges, cont::Span<QuicReceiveTimestamp> out) noexcept
{
    // The records newest first; each is inserted by its time, the later first (section 4.1: times descend), so out
    // holds the latest received times however the packets were processed.
    const usize cap = (m_per_ack < out.size()) ? m_per_ack : out.size();
    usize kept = 0U;
    usize at = m_next;
    for (usize left = m_count; left > 0U; --left)
    {
        at = (at == 0U) ? kQuicMaxReceiveTimestamps - 1U : at - 1U;
        const Entry& entry = m_entries[at];
        bool acknowledged = false;
        for (const QuicAckRange& range : ranges)
        {
            acknowledged = acknowledged || (entry.number >= range.smallest && entry.number <= range.largest);
        }
        if (!acknowledged || entry.received_ns <= m_basis_ns)
        {
            continue;
        }
        const u64 time = ((entry.received_ns - m_basis_ns) / kNanosPerMicro) >> m_exponent;
        usize slot = kept;
        while (slot > 0U && out[slot - 1U].time < time)
        {
            --slot;
        }
        if (slot >= cap)
        {
            continue; // no later than every time kept, and they fill the frame
        }
        kept += (kept < cap) ? 1U : 0U;
        for (usize move = kept - 1U; move > slot; --move)
        {
            out[move] = out[move - 1U];
        }
        out[slot] = QuicReceiveTimestamp{entry.number, time};
    }
    m_count = 0U;
    return kept;
}

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

bool QuicAckTracker::on_received(u64 number, bool ack_eliciting, Ecn ecn, u64 received_ns,
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

    if (ecn != Ecn::NotEct)
    {
        m_ecn_seen = true;
        if (ecn == Ecn::Ect0)
        {
            ++m_ecn.ect0;
        }
        else if (ecn == Ecn::Ect1)
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

    bool immediate = m_space != QuicSpace::Application || ecn == Ecn::Ce;
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

QuicAckWrite QuicAckTracker::write(cont::Span<u8> out, u64 now_ns, u64 exponent, QuicReceiveTimestampLog* log) noexcept
{
    const u64 delay_us = (now_ns > m_largest_ns) ? (now_ns - m_largest_ns) / kNanosPerMicro : 0U;
    const u64 delay    = (exponent >= 64U) ? 0U : (delay_us >> exponent);
    const EcnCounts* const ecn = m_ecn_seen ? &m_ecn : nullptr;
    for (usize count = m_count; count > 0U; --count)
    {
        const cont::ConstSpan<QuicAckRange> ranges(m_ranges, count);
        usize bytes = quic_ack_bytes(ranges, delay, ecn);
        if (bytes != 0U && bytes <= out.size())
        {
            // The ranges first; the timestamps take the room they leave, the oldest given up first.
            QuicReceiveTimestamp stamps[kQuicMaxReceiveTimestamps];
            usize reported = 0U;
            if (log != nullptr && log->enabled())
            {
                reported = log->take(ranges, cont::Span<QuicReceiveTimestamp>(stamps, kQuicMaxReceiveTimestamps));
            }
            for (; reported > 0U; --reported)
            {
                const usize with =
                    quic_ack_bytes(ranges, delay, ecn, cont::ConstSpan<QuicReceiveTimestamp>(stamps, reported));
                if (with != 0U && with <= out.size())
                {
                    bytes = with;
                    break;
                }
            }
            if (write_quic_ack(ranges, delay, ecn, out, cont::ConstSpan<QuicReceiveTimestamp>(stamps, reported)) !=
                bytes)
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
        adjusted_ns     = latest_ns;
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
    adjusted_ns = adjusted;
    const u64 deviation = (smoothed_ns > adjusted) ? smoothed_ns - adjusted : adjusted - smoothed_ns;
    variance_ns = (3U * variance_ns + deviation) / 4U;
    smoothed_ns = (7U * smoothed_ns + adjusted) / 8U;
}

// ---- QuicRecovery ----

// The path slot fits the padding before the tag: per-packet storage stays as it was.
static_assert(sizeof(QuicSentPacket) == (6U * sizeof(u64)) + sizeof(DeliveryStamp));

QuicRecovery::QuicRecovery(const QuicRecoveryConfig& config, cont::Span<QuicSentEntry> initial,
                           cont::Span<QuicSentEntry> handshake, cont::Span<QuicSentEntry> application) noexcept
    : m_side(config.side),
      m_local_max_ack_delay_ns(config.local_max_ack_delay_ns),
      m_local_ack_delay_exponent(min_u64(config.local_ack_delay_exponent, kQuicMaxAckDelayExponent)),
      m_initial_rtt_ns(min_u64(config.initial_rtt_ns, kMaxRttNs)),
      m_reserved_entries(config.reserved_entries)
{
    m_spaces[quic_space_index(QuicSpace::Initial)].entries   = initial;
    m_spaces[quic_space_index(QuicSpace::Handshake)].entries = handshake;
    m_spaces[quic_space_index(QuicSpace::Initial)].received.reset(QuicSpace::Initial);
    m_spaces[quic_space_index(QuicSpace::Handshake)].received.reset(QuicSpace::Handshake);
    static_cast<void>(open_path(0U, m_first, application));
}

const QuicRecovery::Path& QuicRecovery::view(usize path) const noexcept
{
    static const Path kOutOfRange{};
    return path_open(path) ? *m_paths[path] : kOutOfRange;
}

bool QuicRecovery::usable(QuicSpace space, usize path) const noexcept
{
    return (space == QuicSpace::Application) ? path_open(path) : path == 0U;
}

QuicRecovery::Space& QuicRecovery::space_of(QuicSpace space, usize path) noexcept
{
    return (space == QuicSpace::Application) ? m_paths[path]->application : m_spaces[quic_space_index(space)];
}

const QuicRecovery::Space& QuicRecovery::space_of(QuicSpace space, usize path) const noexcept
{
    if (space == QuicSpace::Application)
    {
        return view(path).application;
    }
    return (path == 0U) ? m_spaces[quic_space_index(space)] : view(kQuicNoPath).application;
}

QuicRecovery::Path& QuicRecovery::owner(QuicSpace space, usize path) noexcept
{
    return *m_paths[(space == QuicSpace::Application) ? path : 0U];
}

const QuicRecovery::Path& QuicRecovery::owner(QuicSpace space, usize path) const noexcept
{
    return view((space == QuicSpace::Application || path == 0U) ? path : kQuicNoPath);
}

void QuicRecovery::reset_estimate(Path& path) const noexcept
{
    path.rtt = QuicRtt{};
    path.rtt.smoothed_ns = m_initial_rtt_ns;
    path.rtt.variance_ns = m_initial_rtt_ns / 2U;
    path.packet_threshold = kQuicPacketThreshold;
    path.reorder_steps = 0U;
    path.reorder_persist = 0U;
    path.pto_count = 0U;
}

bool QuicRecovery::open_path(usize path, QuicPathRecovery& state, cont::Span<QuicSentEntry> storage) noexcept
{
    if (path >= kQuicMaxPaths || m_paths[path] != nullptr || storage.empty())
    {
        return false;
    }
    Path& slot = state.m_state;
    for (const Path* held : m_paths)
    {
        if (held == &slot)
        {
            return false;
        }
    }
    slot = Path{};
    reset_estimate(slot);
    slot.application.entries = storage;
    slot.application.received.reset(QuicSpace::Application);
    m_paths[path] = &slot;
    return true;
}

bool QuicRecovery::close_path(usize path, u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    usize open = 0U;
    for (const Path* each : m_paths)
    {
        open += (each != nullptr) ? 1U : 0U;
    }
    const bool handshake_done = m_confirmed && m_spaces[quic_space_index(QuicSpace::Initial)].discarded &&
                                m_spaces[quic_space_index(QuicSpace::Handshake)].discarded;
    if (!path_open(path) || open == 1U || (path == 0U && !handshake_done))
    {
        return false;
    }
    // The slot's counts end with it: its packets are reported once it is closed, so on_path() is false for them.
    Space& space = m_paths[path]->application;
    m_paths[path] = nullptr;
    for (usize i = 0U; i < space.count; ++i)
    {
        QuicSentEntry& sent = entry(space, i);
        if (sent.state == kOutstanding)
        {
            sent.state   = kLost;
            sent.lost_ns = now_ns;
            events.on_lost(QuicSpace::Application, sent.packet);
        }
    }
    for (Path* each : m_paths)
    {
        if (each != nullptr && each->min_via == path)
        {
            each->min_via = kQuicNoPath;
        }
    }
    if (path == 0U)
    {
        m_receive_log.configure(0U, 0U);
    }
    arm(now_ns);
    return true;
}

bool QuicRecovery::on_path(QuicSpace space, const QuicSentPacket& packet) const noexcept
{
    if (packet.off_path || !path_open(packet.path))
    {
        return false;
    }
    return space != QuicSpace::Application || packet.number >= m_paths[packet.path]->floor;
}

void QuicRecovery::restart_path(u64 now_ns, usize path) noexcept
{
    if (!path_open(path))
    {
        return;
    }
    Path& slot = *m_paths[path];
    const Space& space = slot.application;
    for (usize i = 0U; i < space.count; ++i)
    {
        const QuicSentEntry& sent = entry(space, i);
        if (sent.state == kOutstanding && sent.packet.in_flight && on_path(QuicSpace::Application, sent.packet))
        {
            slot.bytes_in_flight -= sent.packet.bytes;
        }
    }
    slot.floor = space.next_number;
    reset_estimate(slot);
    arm(now_ns);
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

const QuicSentEntry& QuicRecovery::entry(const Space& space, usize index) noexcept
{
    usize at = space.head + index;
    if (at >= space.entries.size())
    {
        at -= space.entries.size();
    }
    return space.entries[at];
}

usize QuicRecovery::lower_bound(const Space& space, u64 number) noexcept
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

bool QuicRecovery::acknowledges_unsent(const Space& space, const QuicAck& ack) noexcept
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

void QuicRecovery::settle(QuicSpace id, Space& space, QuicSentEntry& slot) noexcept
{
    if (slot.packet.in_flight)
    {
        owner(id, slot.packet.path).bytes_in_flight -= on_path(id, slot.packet) ? slot.packet.bytes : 0U;
        if (slot.packet.ack_eliciting)
        {
            --space.eliciting_in_flight;
        }
    }
}

bool QuicRecovery::held_lost(QuicSpace id, usize path, const QuicSentEntry& slot, u64 now_ns) const noexcept
{
    return lost(slot.state) && now_ns - slot.lost_ns <= probe_period_ns(id, path); // times only rise
}

void QuicRecovery::compact(QuicSpace id, usize path, u64 now_ns, bool room) noexcept
{
    Space& space = space_of(id, path);
    while (space.count > 0U && settled(entry(space, 0U).state) &&
           ((room && space.count >= space.entries.size()) || !held_lost(id, path, entry(space, 0U), now_ns)))
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
    for (const Path* path : m_paths)
    {
        eliciting += (path != nullptr) ? path->application.eliciting_in_flight : 0U;
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

u64 QuicRecovery::probe_period_ns(QuicSpace space, usize path) const noexcept
{
    const Path& estimate = owner(space, path);
    const u64 base = shl_sat(estimate.rtt.probe_base_ns(), estimate.pto_count);
    return (space == QuicSpace::Application) ? add_sat(base, shl_sat(m_peer_max_ack_delay_ns, estimate.pto_count))
                                             : base;
}

u64 QuicRecovery::largest_probe_period_ns() const noexcept
{
    u64 largest = 0U;
    for (usize path = 0U; path < kQuicMaxPaths; ++path)
    {
        largest = path_open(path) ? max_u64(largest, probe_period_ns(QuicSpace::Application, path)) : largest;
    }
    return largest;
}

u64 QuicRecovery::loss_delay_ns(usize path) const noexcept
{
    const Path& estimate = view(path);
    const u64 rtt    = max_u64(estimate.rtt.latest_ns, estimate.rtt.smoothed_ns);
    const u64 window = min_u64(mul_sat(estimate.reorder_steps, estimate.rtt.min_ns / 4U), estimate.rtt.smoothed_ns);
    return max_u64(add_sat(rtt * kQuicTimeThresholdNumerator / kQuicTimeThresholdDivisor, window),
                   kQuicGranularityNs);
}

u64 QuicRecovery::persistent_congestion_ns(usize path) const noexcept
{
    return mul_sat(add_sat(view(path).rtt.probe_base_ns(), m_peer_max_ack_delay_ns),
                   kQuicPersistentCongestionThreshold);
}

bool QuicRecovery::earliest_loss(u64& time_ns, QuicSpace& id, usize& path) const noexcept
{
    time_ns = kNeverNs;
    for (usize i = 0U; i < 2U; ++i)
    {
        if (m_spaces[i].loss_time_ns < time_ns)
        {
            time_ns = m_spaces[i].loss_time_ns;
            id      = static_cast<QuicSpace>(i);
            path    = 0U;
        }
    }
    for (usize i = 0U; i < kQuicMaxPaths; ++i)
    {
        if (m_paths[i] != nullptr && m_paths[i]->application.loss_time_ns < time_ns)
        {
            time_ns = m_paths[i]->application.loss_time_ns;
            id      = QuicSpace::Application;
            path    = i;
        }
    }
    return time_ns != kNeverNs;
}

u64 QuicRecovery::pto_time(u64 now_ns, QuicSpace& id, usize& path) const noexcept
{
    path = 0U;
    if (!any_eliciting_in_flight())
    {
        id = m_handshake_keys ? QuicSpace::Handshake : QuicSpace::Initial;
        return add_sat(now_ns, probe_period_ns(id));
    }
    u64 time = kNeverNs;
    for (usize i = 0U; i < 2U; ++i)
    {
        const QuicSpace candidate = static_cast<QuicSpace>(i);
        const u64 at = add_sat(m_spaces[i].last_eliciting_ns, probe_period_ns(candidate));
        if (m_spaces[i].eliciting_in_flight != 0U && at < time)
        {
            time = at;
            id   = candidate;
        }
    }
    // The Application spaces' timeouts wait for confirmation (section 6.2.1).
    for (usize i = 0U; i < kQuicMaxPaths && m_confirmed; ++i)
    {
        const Space& space = view(i).application;
        const u64 at = add_sat(space.last_eliciting_ns, probe_period_ns(QuicSpace::Application, i));
        if (space.eliciting_in_flight != 0U && at < time)
        {
            time = at;
            id   = QuicSpace::Application;
            path = i;
        }
    }
    return time;
}

void QuicRecovery::arm(u64 now_ns) noexcept
{
    u64       time = kNeverNs;
    QuicSpace id   = QuicSpace::Initial;
    usize     path = 0U;
    if (earliest_loss(time, id, path))
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
    m_timer_ns = pto_time(now_ns, id, path);
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
    Space& space = m_spaces[quic_space_index(id)];
    if (space.discarded)
    {
        return true;
    }
    drop_outstanding(id, 0U, events);
    space.discarded = true;
    space.received.reset(id);
    if (m_paths[0] != nullptr)
    {
        m_paths[0]->pto_count = 0U;
    }
    arm(now_ns);
    return true;
}

void QuicRecovery::retry(u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    if (m_paths[0] == nullptr)
    {
        return; // no Retry comes once slot 0 can close: the handshake is confirmed by then
    }
    // Only the first flight measures the path: after a probe the Retry may answer a later packet.
    Path& first = *m_paths[0];
    const Space& initial = m_spaces[quic_space_index(QuicSpace::Initial)];
    const u64 first_ns = (initial.count != 0U && first.pto_count == 0U) ? entry(initial, 0U).packet.sent_ns : now_ns;
    drop_outstanding(QuicSpace::Initial, 0U, events);
    drop_outstanding(QuicSpace::Handshake, 0U, events);
    for (usize path = 0U; path < kQuicMaxPaths; ++path)
    {
        if (path_open(path))
        {
            drop_outstanding(QuicSpace::Application, path, events);
        }
    }
    if (!first.rtt.sampled() && now_ns > first_ns)
    {
        first.rtt.smoothed_ns = min_u64(now_ns - first_ns, kMaxRttNs);
        first.rtt.variance_ns = first.rtt.smoothed_ns / 2U;
    }
    first.pto_count = 0U;
    arm(now_ns);
}

void QuicRecovery::reject_early(u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    if (m_paths[0] != nullptr)
    {
        drop_outstanding(QuicSpace::Application, 0U, events);
        m_paths[0]->pto_count = 0U;
    }
    arm(now_ns);
}

void QuicRecovery::drop_outstanding(QuicSpace id, usize path, QuicRecoveryEvents& events) noexcept
{
    Space& space = space_of(id, path);
    for (usize i = 0U; i < space.count; ++i)
    {
        QuicSentEntry& slot = entry(space, i);
        if (slot.state == kOutstanding)
        {
            settle(id, space, slot);
            slot.state = kLost;
            events.on_discarded(id, slot.packet);
        }
    }
    space.head = 0U;
    space.count = 0U;
    space.loss_time_ns = kNeverNs;
    space.last_eliciting_ns = 0U;
}

bool QuicRecovery::can_send(QuicSpace id, usize path) const noexcept
{
    if (!usable(id, path))
    {
        return false;
    }
    const Space& space = space_of(id, path);
    return !space.discarded && (space.count < space.entries.size() || settled(space.entries[space.head].state));
}

bool QuicRecovery::can_send_data(QuicSpace id, usize path) const noexcept
{
    if (!usable(id, path))
    {
        return false;
    }
    const Space& space = space_of(id, path);
    return !space.discarded &&
           (space.count + m_reserved_entries < space.entries.size() || settled(space.entries[space.head].state));
}

QuicRecoveryError QuicRecovery::on_sent(QuicSpace id, const QuicSentPacket& packet) noexcept
{
    const usize path = packet.path;
    if (!usable(id, path))
    {
        return QuicRecoveryError::NoPath;
    }
    Space& space = space_of(id, path);
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
        compact(id, path, packet.sent_ns, true); // a lost packet held for a late acknowledgement gives up its slot
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
        owner(id, path).bytes_in_flight += on_path(id, slot.packet) ? packet.bytes : 0U;
        if (packet.ack_eliciting)
        {
            ++space.eliciting_in_flight;
            space.last_eliciting_ns = packet.sent_ns;
        }
        arm(packet.sent_ns);
    }
    return QuicRecoveryError::None;
}

QuicAckWrite QuicRecovery::write_ack(QuicSpace id, cont::Span<u8> out, u64 now_ns, usize path) noexcept
{
    if (!usable(id, path))
    {
        return {};
    }
    Space& space = space_of(id, path);
    if (space.discarded)
    {
        return {};
    }
    if (id != QuicSpace::Application)
    {
        return space.received.write(out, now_ns, kQuicDefaultAckDelayExponent);
    }
    return space.received.write(out, now_ns, m_local_ack_delay_exponent, (path == 0U) ? &m_receive_log : nullptr);
}

void QuicRecovery::on_ack_acknowledged(usize path, u64 largest) noexcept
{
    if (path_open(path))
    {
        m_paths[path]->application.received.on_ack_acknowledged(largest);
    }
}

bool QuicRecovery::on_received(QuicSpace id, u64 number, bool ack_eliciting, Ecn ecn, u64 received_ns,
                               usize path) noexcept
{
    if (!usable(id, path))
    {
        return false;
    }
    Space& space = space_of(id, path);
    if (space.discarded)
    {
        return false;
    }
    const u64 max_delay = (id == QuicSpace::Application) ? m_local_max_ack_delay_ns : 0U;
    if (!space.received.on_received(number, ack_eliciting, ecn, received_ns, max_delay))
    {
        return false;
    }
    if (id == QuicSpace::Application && path == 0U)
    {
        m_receive_log.record(number, received_ns);
    }
    return true;
}

QuicLoss QuicRecovery::detect_lost(QuicSpace id, usize path, u64 now_ns, QuicRecoveryEvents& events) noexcept
{
    Space&   space = space_of(id, path);
    Path&    estimate = owner(id, path);
    QuicLoss loss;
    space.loss_time_ns = kNeverNs;
    if (space.largest_acked == kQuicNoPacket)
    {
        return loss;
    }
    const u64 loss_delay = loss_delay_ns(path);
    const u64 congestion = persistent_congestion_ns(path);

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
        const bool by_number = space.largest_acked - packet.number >= estimate.packet_threshold;
        if (!by_time && !by_number)
        {
            space.loss_time_ns = min_u64(space.loss_time_ns, add_sat(packet.sent_ns, loss_delay));
            continue;
        }
        settle(id, space, slot);
        slot.state   = by_time ? kLostByTime : kLost;
        slot.lost_ns = now_ns;
        ++loss.packets;
        const bool counted = on_path(id, packet);
        if (packet.in_flight && counted)
        {
            loss.bytes += packet.bytes;
        }
        loss.newest_sent_ns = max_u64(loss.newest_sent_ns, packet.sent_ns);
        // A lost PMTU probe says nothing of congestion (RFC 9000 section 14.4), nor does a packet a scheduled handover
        // may have taken (handover.hpp): neither starts nor ends a run.
        if (packet.ack_eliciting && !packet.path_probe && !packet.handover && counted && estimate.rtt.sampled() &&
            packet.sent_ns >= estimate.rtt.first_sample_ns)
        {
            run_first = min_u64(run_first, packet.sent_ns);
            run_last  = max_u64(run_last, packet.sent_ns);
        }
        events.on_lost(id, packet);
    }
    close_run();
    // RFC 8985 section 6.2, step 4: the raised thresholds persist for 16 loss detections with no spurious loss.
    if (loss.packets != 0U && estimate.reorder_persist != 0U)
    {
        --estimate.reorder_persist;
        if (estimate.reorder_persist == 0U)
        {
            estimate.packet_threshold = kQuicPacketThreshold;
            estimate.reorder_steps    = 0U;
        }
    }
    return loss;
}

usize QuicRecovery::room(const Space& space) noexcept
{
    usize free = space.entries.size() - space.count;
    for (usize i = 0U; i < space.count && settled(entry(space, i).state); ++i)
    {
        ++free;
    }
    return free;
}

QuicLoss QuicRecovery::make_room(QuicSpace id, usize path, usize entries, u64 now_ns,
                                 QuicRecoveryEvents& events) noexcept
{
    Space&   space = space_of(id, path);
    QuicLoss loss;
    for (usize i = 0U; i < space.count && room(space) < entries; ++i)
    {
        QuicSentEntry& slot = entry(space, i);
        if (slot.state != kOutstanding)
        {
            continue;
        }
        settle(id, space, slot);
        slot.state   = kLost;
        slot.lost_ns = now_ns;
        ++loss.packets;
        loss.bytes += (slot.packet.in_flight && on_path(id, slot.packet)) ? slot.packet.bytes : 0U;
        loss.newest_sent_ns = max_u64(loss.newest_sent_ns, slot.packet.sent_ns);
        events.on_lost(id, slot.packet);
    }
    return loss;
}

QuicAckOutcome QuicRecovery::on_ack(QuicSpace id, const QuicAck& ack, u64 now_ns, QuicRecoveryEvents& events,
                                    usize path, usize via) noexcept
{
    QuicAckOutcome out;
    if (!usable(id, path))
    {
        out.error = QuicRecoveryError::NoPath;
        return out;
    }
    Space& space = space_of(id, path);
    if (space.discarded)
    {
        out.error = QuicRecoveryError::Discarded;
        return out;
    }
    if (!path_open(via))
    {
        out.error = QuicRecoveryError::NoPath;
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
    Path& estimate = owner(id, path);
    const u64 prior_largest = space.largest_acked;
    out.advanced = prior_largest == kQuicNoPacket || ack.largest > prior_largest;
    space.largest_acked =
        (space.largest_acked == kQuicNoPacket) ? ack.largest : max_u64(space.largest_acked, ack.largest);

    // Mark what this frame newly acknowledges; the events follow the losses (appendix A.7). A packet held after it
    // was declared lost is acknowledged without an event and measures the reordering (section 6.1).
    u64  largest_newly = kQuicNoPacket;
    u64  largest_sent  = 0U;
    bool largest_on_path = false; // the largest newly acknowledged was sent where the estimate follows
    bool largest_handover = false;
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
                const bool counted = on_path(id, slot.packet);
                if (held_lost(id, path, slot, now_ns))
                {
                    reordering = max_u64(reordering, prior_largest - slot.packet.number + 1U);
                    by_time    = by_time || slot.state == kLostByTime;
                    slot.state = kAcked;
                    ++out.spurious_packets;
                    out.spurious_bytes += (slot.packet.in_flight && counted) ? slot.packet.bytes : 0U;
                    events.on_spurious(id, slot.packet, slot.lost_ns);
                }
                else if (slot.state == kOutstanding)
                {
                    settle(id, space, slot);
                    slot.state = kAckedPending;
                    ++out.acked_packets;
                    if (slot.packet.in_flight && counted)
                    {
                        out.acked_bytes += slot.packet.bytes;
                    }
                }
                else
                {
                    continue;
                }
                eliciting = eliciting || slot.packet.ack_eliciting;
                out.ect0_packets += (counted && slot.packet.ecn == Ecn::Ect0) ? 1U : 0U;
                out.ect1_packets += (counted && slot.packet.ecn == Ecn::Ect1) ? 1U : 0U;
                if (largest_newly == kQuicNoPacket || slot.packet.number > largest_newly)
                {
                    largest_newly = slot.packet.number;
                    largest_sent  = slot.packet.sent_ns;
                    largest_on_path = counted;
                    largest_handover = slot.packet.handover;
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
    out.newest_sent_ns = largest_sent;
    if (out.spurious_packets != 0U)
    {
        // RFC 8985 section 6.2, step 4, before this frame's detection: the packet threshold to the reordering seen,
        // the time threshold's window one step a round trip for a loss the time threshold declared.
        estimate.packet_threshold =
            min_u64(max_u64(estimate.packet_threshold, reordering), kQuicMaxPacketThreshold);
        if (by_time && space.largest_acked >= space.reorder_round && estimate.reorder_steps < kQuicMaxReorderSteps)
        {
            ++estimate.reorder_steps;
            space.reorder_round = space.next_number;
        }
        estimate.reorder_persist = kQuicReorderPersist;
    }
    if (largest_newly == ack.largest && eliciting && largest_on_path)
    {
        // RFC 10069 section 5.4: a sample through another path than the minimum's starts the minimum again.
        if (estimate.rtt.sampled() && estimate.min_via != via)
        {
            estimate.rtt.min_ns = kMaxRttNs;
        }
        const u64 latest = (now_ns > largest_sent) ? now_ns - largest_sent : 0U;
        estimate.rtt.sample(latest, decoded_ack_delay_ns(id, ack.delay), m_confirmed, m_peer_max_ack_delay_ns,
                            now_ns);
        estimate.min_via = via;
        out.rtt_sampled = true;
        out.rtt_handover = largest_handover;
    }
    out.lost = detect_lost(id, path, now_ns, events);

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
    compact(id, path, now_ns, false);
    if (peer_validated())
    {
        estimate.pto_count = 0U;
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
    for (const Path* path : m_paths)
    {
        if (path != nullptr)
        {
            time = min_u64(time, path->application.received.ack_deadline_ns());
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
    usize     path = 0U;
    if (earliest_loss(time, id, path))
    {
        out.kind  = QuicTimeoutKind::Loss;
        out.space = id;
        out.path  = path;
        out.lost  = detect_lost(id, path, now_ns, events);
        compact(id, path, now_ns, false);
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
        (void)pto_time(now_ns, id, path);
        out.kind   = QuicTimeoutKind::Probe;
        out.space  = id;
        out.path   = path;
        out.probes = 2U;
    }
    // The probes must leave (section 6.2.4: a sender MUST send one): a storage full of packets that nothing
    // acknowledges, as behind a path that drops every one of them, or of ACK-only packets, would hold them back for
    // good. Its oldest are declared lost instead, and their entries go to the probes.
    out.lost = make_room(out.space, out.path, out.probes, now_ns, events);
    Path& estimate = owner(out.space, out.path);
    if (estimate.pto_count < 64U)
    {
        ++estimate.pto_count; // the backoff saturates long before: an idle timeout ends the connection
    }
    arm(now_ns);
    return out;
}

u64 QuicRecovery::largest_acked(QuicSpace space, usize path) const noexcept
{
    return space_of(space, path).largest_acked;
}

const QuicSentPacket* QuicRecovery::unacknowledged(QuicSpace space, u32 tag, usize path) const noexcept
{
    const Space& held = space_of(space, path);
    for (usize index = held.count; tag != 0U && index > 0U; --index)
    {
        const QuicSentEntry& slot = entry(held, index - 1U);
        if ((slot.state == kOutstanding || lost(slot.state)) && slot.packet.tag == tag)
        {
            return &slot.packet;
        }
    }
    return nullptr;
}

u64 QuicRecovery::unacknowledged_sent_ns(QuicSpace space, u64 number, usize path) const noexcept
{
    const Space& held = space_of(space, path);
    const usize at = lower_bound(held, number);
    if (at == held.count)
    {
        return kNeverNs;
    }
    const QuicSentEntry& slot = entry(held, at);
    return (slot.packet.number == number && (slot.state == kOutstanding || lost(slot.state))) ? slot.packet.sent_ns
                                                                                              : kNeverNs;
}

u64 QuicRecovery::ack_deadline_ns(QuicSpace space, usize path) const noexcept
{
    const Space& held = space_of(space, path);
    return held.discarded ? kNeverNs : held.received.ack_deadline_ns();
}

const QuicAckTracker& QuicRecovery::received(QuicSpace space, usize path) const noexcept
{
    return space_of(space, path).received;
}

// ---- QuicPeerReceiveTimes ----

usize QuicPeerReceiveTimes::read(const QuicAck& ack, u64 exponent, const QuicRecovery& recovery, u64 now_ns) noexcept
{
    m_count = 0U;
    // A time in nanoseconds stays below 2^62, so a one-way delay (received less sent) fits a signed difference.
    const u64 unit = kNanosPerMicro << min_u64(exponent, kQuicMaxReceiveTimestampsExponent);
    const u64 max_units = (u64{1} << 62U) / unit;
    i64 least = 0;
    i64 most = 0;
    u64 oldest_ns = now_ns;
    QuicAckTimestamps walk(ack);
    QuicReceiveTimestamp stamp;
    while (m_count < kQuicMaxReceiveTimestamps && walk.next(stamp))
    {
        const u64 sent_ns = recovery.unacknowledged_sent_ns(QuicSpace::Application, stamp.number);
        if (sent_ns == kNeverNs || sent_ns > now_ns)
        {
            continue; // acknowledged before, never sent or no longer held: nothing to time
        }
        if (stamp.time > max_units)
        {
            m_count = 0U;
            return 0U;
        }
        const u64 received_ns = stamp.time * unit;
        usize at = m_count;
        while (at > 0U && m_entries[at - 1U].number > stamp.number)
        {
            --at;
        }
        if (at > 0U && m_entries[at - 1U].number == stamp.number)
        {
            continue; // reported twice: the first report stands
        }
        for (usize move = m_count; move > at; --move)
        {
            m_entries[move] = m_entries[move - 1U];
        }
        m_entries[at] = Entry{stamp.number, received_ns};
        const i64 one_way = static_cast<i64>(received_ns) - static_cast<i64>(sent_ns);
        least = (m_count == 0U || one_way < least) ? one_way : least;
        most = (m_count == 0U || one_way > most) ? one_way : most;
        oldest_ns = min_u64(oldest_ns, sent_ns);
        ++m_count;
    }
    if (m_count != 0U && static_cast<u64>(most - least) > now_ns - oldest_ns)
    {
        m_count = 0U; // implausible (section 9)
    }
    return m_count;
}

u64 QuicPeerReceiveTimes::find(u64 number) const noexcept
{
    usize low = 0U;
    usize high = m_count;
    while (low < high)
    {
        const usize mid = low + ((high - low) / 2U);
        if (m_entries[mid].number < number)
        {
            low = mid + 1U;
        }
        else
        {
            high = mid;
        }
    }
    return (low < m_count && m_entries[low].number == number) ? m_entries[low].received_ns : kNeverNs;
}

} // namespace crd::net
