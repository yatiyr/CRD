// crd-net -- a QUIC connection's unreliable datagrams (4.2a; contract:
// docs/design/network-foundation.md#quic-datagrams).
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/quic_datagram.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
// Each Length size with the largest value it encodes (RFC 9000 section 16).
constexpr usize kLengthBytes[] = {1U, 2U, 4U, 8U};
constexpr u64 kLengthMax[] = {63U, 16383U, (u64{1} << 30U) - 1U, kQuicMaxVarint};
} // namespace

bool quic_datagram_payload_limit(u64 frame_bytes, usize& payload) noexcept
{
    // The type takes one byte; each Length size bounds the payload twice, by the room it leaves and by its range.
    bool found = false;
    u64 best = 0U;
    for (usize index = 0U; index < sizeof(kLengthBytes) / sizeof(kLengthBytes[0]); ++index)
    {
        if (frame_bytes < 1U + kLengthBytes[index])
        {
            break;
        }
        const u64 room = frame_bytes - 1U - kLengthBytes[index];
        const u64 candidate = (room < kLengthMax[index]) ? room : kLengthMax[index];
        best = (candidate > best) ? candidate : best;
        found = true;
    }
    payload = static_cast<usize>(best);
    return found;
}

// ---- Ring ----

void QuicDatagrams::Ring::bind(u8* bytes, u32 capacity, Entry* entries, u32 slots) noexcept
{
    m_bytes = bytes;
    m_capacity = capacity;
    m_entries = entries;
    m_slots = slots;
}

bool QuicDatagrams::Ring::push(cont::ConstSpan<u8> payload, u64 id, u64 now_ns, bool early) noexcept
{
    if (m_count == m_slots || payload.size() > m_capacity)
    {
        return false;
    }
    if (m_count == 0U)
    {
        m_head = 0U;
        m_tail = 0U;
    }
    u64 at = m_tail;
    const u64 offset = at % m_capacity;
    if (offset + payload.size() > m_capacity)
    {
        at += m_capacity - offset; // the payload starts the ring's next lap
    }
    if (at + payload.size() - m_head > m_capacity)
    {
        return false;
    }
    if (!payload.empty())
    {
        std::memcpy(m_bytes + (at % m_capacity), payload.data(), payload.size());
    }
    Entry& entry = m_entries[(m_first + m_count) % m_slots];
    entry.at = at;
    entry.id = id;
    entry.queued_ns = now_ns;
    entry.bytes = static_cast<u32>(payload.size());
    entry.early = early;
    m_tail = at + payload.size();
    ++m_count;
    return true;
}

cont::ConstSpan<u8> QuicDatagrams::Ring::bytes_of(const Entry& entry) const noexcept
{
    return cont::ConstSpan<u8>(m_bytes + (entry.at % m_capacity), entry.bytes);
}

void QuicDatagrams::Ring::pop() noexcept
{
    m_first = (m_first + 1U) % m_slots;
    --m_count;
    m_head = m_entries[m_first].at; // stale once empty: push() restarts an empty ring at 0
}

// ---- Construction ----

QuicDatagrams::~QuicDatagrams() noexcept
{
    if (m_allocator != nullptr)
    {
        memory::destroy_array(*m_allocator, m_bytes, usize{m_limits.receive_bytes} + m_limits.send_bytes);
        memory::destroy_array(*m_allocator, m_entries, usize{m_limits.receive_count} + m_limits.send_count);
        memory::destroy_array(*m_allocator, m_reports, usize{m_limits.send_count});
    }
}

bool QuicDatagrams::limits_valid(const QuicDatagramLimits& limits) noexcept
{
    // The largest payload received comes in a frame without a Length: max_frame_bytes - 1 bytes.
    const bool receives = limits.max_frame_bytes != 0U;
    const bool receive_ok = receives ? (limits.max_frame_bytes <= kQuicMaxDatagramFrame &&
                                        limits.receive_bytes >= limits.max_frame_bytes - 1U &&
                                        limits.receive_bytes != 0U && limits.receive_bytes <= kQuicMaxDatagramRing &&
                                        limits.receive_count != 0U && limits.receive_count <= kQuicMaxDatagramCount)
                                     : (limits.receive_bytes == 0U && limits.receive_count == 0U);
    const bool sends = limits.send_bytes != 0U;
    const bool send_ok = sends ? (limits.send_bytes <= kQuicMaxDatagramRing && limits.send_count != 0U &&
                                  limits.send_count <= kQuicMaxDatagramCount)
                               : (limits.send_count == 0U && !limits.outcomes);
    return receive_ok && send_ok;
}

bool QuicDatagrams::init(memory::IAllocator& allocator, const QuicDatagramLimits& limits) noexcept
{
    if (m_allocator != nullptr || !limits_valid(limits))
    {
        return false;
    }
    const usize bytes = usize{limits.receive_bytes} + limits.send_bytes;
    const usize entries = usize{limits.receive_count} + limits.send_count;
    const usize reports = limits.outcomes ? limits.send_count : 0U;
    u8* const ring = memory::try_construct_array<u8>(allocator, bytes);
    Entry* const slots = memory::try_construct_array<Entry>(allocator, entries);
    QuicDatagramReport* const outcomes = memory::try_construct_array<QuicDatagramReport>(allocator, reports);
    if ((bytes != 0U && ring == nullptr) || (entries != 0U && slots == nullptr) ||
        (reports != 0U && outcomes == nullptr))
    {
        memory::destroy_array(allocator, ring, bytes);
        memory::destroy_array(allocator, slots, entries);
        memory::destroy_array(allocator, outcomes, reports);
        return false;
    }
    m_allocator = &allocator;
    m_limits = limits;
    m_bytes = ring;
    m_entries = slots;
    m_reports = outcomes;
    m_receive.bind(ring, limits.receive_bytes, slots, limits.receive_count);
    m_send.bind(ring + limits.receive_bytes, limits.send_bytes, slots + limits.receive_count, limits.send_count);
    return true;
}

void QuicDatagrams::fill_parameters(QuicTransportParameters& local) const noexcept
{
    local.max_datagram_frame_size = m_limits.max_frame_bytes;
}

void QuicDatagrams::set_peer(u64 peer_max_frame, usize udp_payload) noexcept
{
    m_peer_known = true;
    m_sendable = false;
    m_max_payload = 0U;
    const usize room = (udp_payload > kQuicShortPacketOverhead) ? udp_payload - kQuicShortPacketOverhead : 0U;
    const u64 frame_bytes = (peer_max_frame < room) ? peer_max_frame : room;
    usize payload = 0U;
    if (m_limits.send_bytes == 0U || !quic_datagram_payload_limit(frame_bytes, payload))
    {
        return;
    }
    m_max_payload = (payload < m_limits.send_bytes) ? payload : m_limits.send_bytes;
    m_sendable = true;
}

// ---- Receiving ----

QuicTransportError QuicDatagrams::on_frame(const QuicFrame& frame, bool early) noexcept
{
    if (quic_frame_bytes(frame) > m_limits.max_frame_bytes)
    {
        return QuicTransportError::ProtocolViolation; // RFC 9221 section 3 (every frame passes a size of 0)
    }
    if (m_receive.push(frame.data, m_counters.received, 0U, early))
    {
        ++m_counters.received;
    }
    else
    {
        ++m_counters.dropped;
    }
    return QuicTransportError::NoError;
}

bool QuicDatagrams::peek(cont::ConstSpan<u8>& payload) const noexcept
{
    if (m_receive.count() == 0U)
    {
        return false;
    }
    payload = m_receive.bytes_of(m_receive.front());
    return true;
}

bool QuicDatagrams::oldest_early() const noexcept
{
    return m_receive.count() != 0U && m_receive.front().early;
}

void QuicDatagrams::pop() noexcept
{
    if (m_receive.count() != 0U)
    {
        m_receive.pop();
    }
}

// ---- Sending ----

QuicDatagramError QuicDatagrams::queue(cont::ConstSpan<u8> payload, u64 now_ns, u64& id) noexcept
{
    if (!m_peer_known)
    {
        return QuicDatagramError::NotReady;
    }
    if (!m_sendable)
    {
        return QuicDatagramError::Unsupported;
    }
    if (payload.size() > m_max_payload)
    {
        return QuicDatagramError::TooLarge;
    }
    expire(now_ns);
    if (m_unreported == m_limits.send_count || !m_send.push(payload, m_next_id, now_ns))
    {
        return QuicDatagramError::Full;
    }
    id = m_next_id++;
    ++m_unreported;
    return QuicDatagramError::None;
}

bool QuicDatagrams::has_frame(u64 now_ns) const noexcept
{
    if (m_send.count() == 0U)
    {
        return false;
    }
    const u64 queued_ns = m_send.back().queued_ns; // the newest expires last
    return m_limits.max_age_ns == 0U || now_ns < queued_ns || now_ns - queued_ns <= m_limits.max_age_ns;
}

void QuicDatagrams::expire(u64 now_ns) noexcept
{
    if (m_limits.max_age_ns == 0U)
    {
        return;
    }
    while (m_send.count() != 0U)
    {
        const Entry& oldest = m_send.front();
        if (now_ns < oldest.queued_ns || now_ns - oldest.queued_ns <= m_limits.max_age_ns)
        {
            return;
        }
        const u64 id = oldest.id;
        m_send.pop();
        ++m_counters.expired;
        if (m_limits.outcomes)
        {
            report(id, QuicDatagramOutcome::Expired);
        }
        else
        {
            --m_unreported;
        }
    }
}

usize QuicDatagrams::write_next(cont::Span<u8> out, u64 now_ns, u64& id) noexcept
{
    expire(now_ns);
    if (m_send.count() == 0U)
    {
        return 0U;
    }
    const Entry& oldest = m_send.front();
    QuicFrame frame;
    frame.kind = QuicFrameKind::Datagram;
    frame.data = m_send.bytes_of(oldest); // with its Length, so padding may follow (to_end false)
    const usize bytes = write_quic_frame(frame, out);
    if (bytes == 0U)
    {
        return 0U;
    }
    id = oldest.id;
    m_send.pop();
    ++m_counters.sent;
    if (!m_limits.outcomes)
    {
        --m_unreported;
    }
    return bytes;
}

// ---- Outcomes ----

void QuicDatagrams::report(u64 id, QuicDatagramOutcome outcome) noexcept
{
    // Every report belongs to a datagram counted in m_unreported, at most send_count of them: it always fits.
    QuicDatagramReport& slot = m_reports[(m_report_first + m_report_count) % m_limits.send_count];
    slot.id = id;
    slot.outcome = outcome;
    ++m_report_count;
}

void QuicDatagrams::on_acked(u64 id) noexcept
{
    if (m_limits.outcomes)
    {
        report(id, QuicDatagramOutcome::Acked);
    }
}

void QuicDatagrams::on_lost(u64 id) noexcept
{
    if (m_limits.outcomes)
    {
        report(id, QuicDatagramOutcome::Lost);
    }
}

void QuicDatagrams::reject_early() noexcept
{
    while (m_send.count() != 0U)
    {
        const u64 id = m_send.front().id;
        m_send.pop();
        if (m_limits.outcomes)
        {
            report(id, QuicDatagramOutcome::Lost);
        }
        else
        {
            --m_unreported;
        }
    }
    m_peer_known = false;
    m_sendable = false;
    m_max_payload = 0U;
}

bool QuicDatagrams::poll(QuicDatagramReport& out) noexcept
{
    if (m_report_count == 0U)
    {
        return false;
    }
    out = m_reports[m_report_first];
    m_report_first = (m_report_first + 1U) % m_limits.send_count;
    --m_report_count;
    --m_unreported;
    return true;
}

} // namespace crd::net
