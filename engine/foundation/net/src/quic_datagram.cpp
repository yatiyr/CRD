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
    entry.written = 0U;
    entry.pin = kNoPin;
    entry.redundant = false;
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
        memory::destroy_array(*m_allocator, m_copies, usize{m_limits.redundant_count});
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
    const bool send_ok =
        sends ? (limits.send_bytes <= kQuicMaxDatagramRing && limits.send_count != 0U &&
                 limits.send_count <= kQuicMaxDatagramCount && limits.redundant_count <= limits.send_count)
              : (limits.send_count == 0U && !limits.outcomes && limits.redundant_count == 0U);
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
    Copy* const copies = memory::try_construct_array<Copy>(allocator, limits.redundant_count);
    if ((bytes != 0U && ring == nullptr) || (entries != 0U && slots == nullptr) ||
        (reports != 0U && outcomes == nullptr) || (limits.redundant_count != 0U && copies == nullptr))
    {
        memory::destroy_array(allocator, ring, bytes);
        memory::destroy_array(allocator, slots, entries);
        memory::destroy_array(allocator, outcomes, reports);
        memory::destroy_array(allocator, copies, usize{limits.redundant_count});
        return false;
    }
    m_allocator = &allocator;
    m_limits = limits;
    m_bytes = ring;
    m_entries = slots;
    m_reports = outcomes;
    m_copies = copies;
    for (u32 copy = 0U; copy < limits.redundant_count; ++copy)
    {
        copies[copy].next_free = copy + 1U;
    }
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
    m_peer_frame = peer_max_frame;
    set_path(udp_payload);
}

void QuicDatagrams::set_path(usize udp_payload) noexcept
{
    if (!m_peer_known)
    {
        return;
    }
    m_sendable = false;
    m_max_payload = 0U;
    const usize room = (udp_payload > kQuicShortPacketOverhead) ? udp_payload - kQuicShortPacketOverhead : 0U;
    const u64 frame_bytes = (m_peer_frame < room) ? m_peer_frame : room;
    usize payload = 0U;
    if (m_limits.send_bytes == 0U || !quic_datagram_payload_limit(frame_bytes, payload))
    {
        return;
    }
    m_max_payload = (payload < m_limits.send_bytes) ? payload : m_limits.send_bytes;
    m_sendable = true;
}

void QuicDatagrams::open_path(usize slot, u64 path_id) noexcept
{
    if (slot < kQuicMaxPaths)
    {
        m_path_ids[slot] = path_id;
        m_open = static_cast<u8>(m_open | (1U << slot));
    }
}

void QuicDatagrams::stop_path(usize slot) noexcept
{
    if (slot >= kQuicMaxPaths)
    {
        return;
    }
    m_open = static_cast<u8>(m_open & ~(1U << slot));
    for (u64 at = m_send_base; m_pinned[slot] != 0U && at < end(); ++at)
    {
        Entry& held = entry(at);
        if (held.written == 0U && held.pin == slot)
        {
            ++m_counters.unrouted;
            drop_unsent(held, QuicDatagramOutcome::Lost);
        }
    }
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

QuicDatagramError QuicDatagrams::queue(cont::ConstSpan<u8> payload, u64 now_ns, u64& id,
                                       const QuicDatagramSend& how) noexcept
{
    if (!m_peer_known)
    {
        return QuicDatagramError::NotReady;
    }
    if (!m_sendable || (how.redundant && (m_limits.redundant_count == 0U || how.pinned)))
    {
        return QuicDatagramError::Unsupported;
    }
    u8 pin = kNoPin;
    for (usize slot = 0U; how.pinned && pin == kNoPin && slot < kQuicMaxPaths; ++slot)
    {
        pin = ((m_open & (1U << slot)) != 0U && m_path_ids[slot] == how.path_id) ? static_cast<u8>(slot) : kNoPin;
    }
    if (how.pinned && pin == kNoPin)
    {
        return QuicDatagramError::NoPath;
    }
    if (payload.size() > m_max_payload)
    {
        return QuicDatagramError::TooLarge;
    }
    expire(now_ns);
    if (how.redundant && m_free_copy == m_limits.redundant_count)
    {
        return QuicDatagramError::Full;
    }
    // Owed copies never hold a datagram back: the oldest datagram's give way first. Without outcomes a redundant
    // datagram counts until it leaves the ring, so that makes room; with them it counts until its report is polled,
    // and giving its copies up lets that report come once its copies in flight settle.
    while (m_unreported == m_limits.send_count && give_up_copies())
    {
    }
    if (m_unreported == m_limits.send_count)
    {
        return QuicDatagramError::Full;
    }
    while (!m_send.push(payload, m_next_id, now_ns))
    {
        if (!give_up_copies())
        {
            return QuicDatagramError::Full;
        }
    }
    if (pin != kNoPin)
    {
        entry(end() - 1U).pin = pin;
        ++m_pinned[pin];
    }
    if (how.redundant)
    {
        const u32 copy = m_free_copy;
        Copy& record = m_copies[copy];
        m_free_copy = record.next_free;
        record = Copy{};
        record.id = m_next_id;
        record.ordinal = end() - 1U;
        record.queued = true;
        Entry& added = entry(record.ordinal);
        added.redundant = true;
        added.copy = copy;
    }
    id = m_next_id++;
    ++m_unreported;
    return QuicDatagramError::None;
}

bool QuicDatagrams::expired(const Entry& entry, u64 now_ns) const noexcept
{
    return m_limits.max_age_ns != 0U && now_ns >= entry.queued_ns && now_ns - entry.queued_ns > m_limits.max_age_ns;
}

bool QuicDatagrams::writable(const Entry& entry, const QuicDatagramPaths& paths) noexcept
{
    if (entry.written == 0U)
    {
        return (entry.pin == kNoPin) ? !paths.copies_only : entry.pin == paths.slot;
    }
    const u32 bit = 1U << paths.slot;
    return entry.redundant && (entry.written & bit) == 0U && (paths.copies & bit) != 0U;
}

void QuicDatagrams::advance(usize slot) const noexcept
{
    // Past every entry the slot can never write again: sent by it, sent once and owing no copies, or pinned to
    // another slot (which stop_path() drops rather than unpins).
    const u32 bit = 1U << slot;
    u64& cursor = m_cursors[slot];
    cursor = (cursor > m_send_base) ? cursor : m_send_base;
    for (; cursor < end(); ++cursor)
    {
        const Entry& passed = m_send.at(cursor - m_send_base);
        const bool open = (passed.written == 0U) ? (passed.pin == kNoPin || passed.pin == slot)
                                                 : (passed.redundant && (passed.written & bit) == 0U);
        if (open)
        {
            return;
        }
    }
}

u64 QuicDatagrams::next_for(const QuicDatagramPaths& paths, u64 now_ns) const noexcept
{
    advance(paths.slot);
    const u64 last = end();
    for (u64 at = m_cursors[paths.slot]; at < last; ++at)
    {
        const Entry& next = m_send.at(at - m_send_base);
        if (next.written == 0U && next.pin == kNoPin && paths.copies_only && m_pinned[paths.slot] == 0U)
        {
            return last; // what was sent is a prefix of the datagrams pinned to none: nothing after it owes a copy
        }
        if (writable(next, paths) && !expired(next, now_ns))
        {
            return at;
        }
    }
    return last;
}

bool QuicDatagrams::has_frame(u64 now_ns, const QuicDatagramPaths& paths) const noexcept
{
    return next_for(paths, now_ns) != end();
}

void QuicDatagrams::expire(u64 now_ns) noexcept
{
    while (m_send.count() != 0U && expired(m_send.front(), now_ns))
    {
        m_counters.expired += (m_send.front().written == 0U) ? 1U : 0U; // a sent one's copies just stop
        pop_oldest(QuicDatagramOutcome::Expired);
    }
}

void QuicDatagrams::pop_oldest(QuicDatagramOutcome unsent) noexcept
{
    const Entry& oldest = m_send.front();
    const u64 id = oldest.id;
    const bool sent = oldest.written != 0U;
    const bool redundant = oldest.redundant;
    const u32 copy = oldest.copy;
    if (!sent && oldest.pin != kNoPin)
    {
        --m_pinned[oldest.pin];
    }
    m_send.pop();
    ++m_send_base;
    if (!sent && m_limits.outcomes)
    {
        report(id, unsent);
    }
    else if (!sent || (redundant && !m_limits.outcomes))
    {
        --m_unreported;
    }
    if (redundant)
    {
        Copy& record = m_copies[copy];
        record.queued = false;
        record.settled = record.settled || !sent; // reported here
        finish(copy);
    }
}

void QuicDatagrams::pop_done(u32 copies) noexcept
{
    while (m_send.count() != 0U)
    {
        const Entry& oldest = m_send.front();
        if (oldest.written == 0U || (oldest.redundant && (copies & ~u32{oldest.written} & kEverySlot) != 0U))
        {
            return;
        }
        pop_oldest(QuicDatagramOutcome::Lost); // sent: nothing is reported here
    }
}

void QuicDatagrams::drop_unsent(Entry& entry, QuicDatagramOutcome outcome) noexcept
{
    if (entry.pin != kNoPin)
    {
        --m_pinned[entry.pin];
    }
    entry.written = kEverySlot;
    if (m_limits.outcomes)
    {
        report(entry.id, outcome);
    }
    else if (!entry.redundant)
    {
        --m_unreported; // a redundant one counts until it leaves the ring, as pop_oldest() finds it sent
    }
    if (entry.redundant)
    {
        m_copies[entry.copy].settled = true; // reported here: its record is freed as it leaves the ring
    }
}

bool QuicDatagrams::give_up_copies() noexcept
{
    if (m_send.count() == 0U || m_send.front().written == 0U)
    {
        return false;
    }
    pop_oldest(QuicDatagramOutcome::Lost); // sent: its copies in flight report it
    return true;
}

usize QuicDatagrams::write_next(cont::Span<u8> out, u64 now_ns, u64& ticket, const QuicDatagramPaths& paths) noexcept
{
    expire(now_ns);
    pop_done(paths.copies);
    const u32 bit = 1U << paths.slot;
    for (;;)
    {
        const u64 at = next_for(paths, now_ns);
        if (at == end())
        {
            return 0U;
        }
        Entry& next = entry(at);
        if (next.bytes > m_max_payload)
        {
            // Queued before the path's size fell: it fits no packet now. A copy is skipped on this slot; a datagram
            // never sent is dropped where it stands, so what waits before it (pinned elsewhere, or owing copies) waits
            // on.
            if (next.written != 0U)
            {
                next.written = static_cast<u8>(next.written | bit);
            }
            else
            {
                ++m_counters.oversized;
                drop_unsent(next, QuicDatagramOutcome::Lost);
            }
            pop_done(paths.copies);
            continue;
        }
        QuicFrame frame;
        frame.kind = QuicFrameKind::Datagram;
        frame.data = m_send.bytes_of(next); // with its Length, so padding may follow (to_end false)
        const usize bytes = write_quic_frame(frame, out);
        if (bytes == 0U)
        {
            return 0U;
        }
        m_counters.copies += (next.written != 0U) ? 1U : 0U;
        if (next.written == 0U && next.pin != kNoPin)
        {
            --m_pinned[next.pin];
        }
        next.written = static_cast<u8>(next.written | bit);
        if (next.redundant)
        {
            ++m_copies[next.copy].in_flight;
            ticket = kCopyTicket | next.copy;
        }
        else
        {
            ticket = next.id;
            m_unreported -= m_limits.outcomes ? 0U : 1U;
        }
        ++m_counters.sent;
        pop_done(paths.copies);
        return bytes;
    }
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

void QuicDatagrams::on_acked(u64 ticket) noexcept
{
    if ((ticket & kCopyTicket) == 0U)
    {
        if (m_limits.outcomes)
        {
            report(ticket, QuicDatagramOutcome::Acked);
        }
        return;
    }
    const auto copy = static_cast<u32>(ticket & ~kCopyTicket);
    Copy& record = m_copies[copy];
    --record.in_flight;
    if (!record.settled)
    {
        record.settled = true;
        if (m_limits.outcomes)
        {
            report(record.id, QuicDatagramOutcome::Acked);
        }
        if (record.queued)
        {
            entry(record.ordinal).written = kEverySlot; // the peer has it: no more copies
            pop_done(kEverySlot);                       // which finishes the record once it leaves the ring
            return;
        }
    }
    finish(copy);
}

void QuicDatagrams::on_lost(u64 ticket) noexcept
{
    if ((ticket & kCopyTicket) == 0U)
    {
        if (m_limits.outcomes)
        {
            report(ticket, QuicDatagramOutcome::Lost);
        }
        return;
    }
    const auto copy = static_cast<u32>(ticket & ~kCopyTicket);
    --m_copies[copy].in_flight;
    finish(copy);
}

void QuicDatagrams::finish(u32 copy) noexcept
{
    Copy& record = m_copies[copy];
    if (record.queued || record.in_flight != 0U)
    {
        return;
    }
    if (!record.settled && m_limits.outcomes)
    {
        report(record.id, QuicDatagramOutcome::Lost); // every copy sent was lost
    }
    record.next_free = m_free_copy;
    m_free_copy = copy;
}

void QuicDatagrams::reject_early() noexcept
{
    while (m_send.count() != 0U)
    {
        pop_oldest(QuicDatagramOutcome::Lost);
    }
    if (m_outcomes_held)
    {
        for (u32 at = 0U; at < m_report_count; ++at)
        {
            QuicDatagramReport& held = m_reports[(m_report_first + at) % m_limits.send_count];
            held.outcome = (held.outcome == QuicDatagramOutcome::Acked) ? QuicDatagramOutcome::Lost : held.outcome;
        }
        m_outcomes_held = false;
    }
    m_peer_known = false;
    m_sendable = false;
    m_max_payload = 0U;
}

bool QuicDatagrams::poll(QuicDatagramReport& out) noexcept
{
    if (m_report_count == 0U || m_outcomes_held)
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
