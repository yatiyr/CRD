// crd-net -- an HTTP/3 connection over a QUIC connection's streams and datagrams (4.2a;
// docs/design/network-foundation.md#http3 and #webtransport).

#include <crd/containers/wire_bytes.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/quic_header.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
// RFC 9114 section 7.2 and 11.2.
constexpr u64 kFrameData = 0x0U;
constexpr u64 kFrameHeaders = 0x1U;
constexpr u64 kFrameCancelPush = 0x3U;
constexpr u64 kFrameSettings = 0x4U;
constexpr u64 kFramePushPromise = 0x5U;
constexpr u64 kFrameGoAway = 0x7U;
constexpr u64 kFrameMaxPushId = 0xDU;
constexpr u64 kFrameWtStream = 0x41U; // has no length (draft-ietf-webtrans-http3-17 section 4.3)

constexpr u64 kStreamControl = 0x0U;
constexpr u64 kStreamPush = 0x1U;
constexpr u64 kStreamQpackEncoder = 0x2U;
constexpr u64 kStreamQpackDecoder = 0x3U;
constexpr u64 kStreamWebTransport = 0x54U;

constexpr u64 kSettingQpackMaxTableCapacity = 0x1U;
constexpr u64 kSettingMaxFieldSectionSize = 0x6U;
constexpr u64 kSettingQpackBlockedStreams = 0x7U;
constexpr u64 kSettingEnableConnectProtocol = 0x8U;
constexpr u64 kSettingH3Datagram = 0x33U;
constexpr u64 kSettingH3DatagramOld = 0xFFD277U; // the legacy spelling's, read as 0x33 is
constexpr u64 kSettingWtEnabled = 0x20B8933BU;
constexpr u64 kSettingWtEnabledDraft16 = 0x2C7CF000U; // draft section 7.1: sent and read beside the current one (S59)
constexpr u64 kSettingWtMaxStreamsUni = 0x2B64U;
constexpr u64 kSettingWtMaxStreamsBidi = 0x2B65U;
constexpr u64 kSettingWtMaxData = 0x2B61U;
constexpr u64 kSettingWtLegacySessions = 0xC671706AU;

constexpr u64 kShutdownRequestId = kQuicMaxVarint - 3U; // 2^62 - 4, the largest client bidirectional stream
constexpr u64 kMaxQuarterStreamId = (u64{1} << 60U) - 1U;
constexpr usize kMaxVarintBytes = 8U;

// Which known settings a SETTINGS frame has named.
constexpr u32 kSeenCapacity = 1U << 0U;
constexpr u32 kSeenBlocked = 1U << 1U;
constexpr u32 kSeenField = 1U << 2U;
constexpr u32 kSeenConnect = 1U << 3U;
constexpr u32 kSeenDatagram = 1U << 4U;
constexpr u32 kSeenDatagramOld = 1U << 5U;
constexpr u32 kSeenWtEnabled = 1U << 6U;
constexpr u32 kSeenWtUni = 1U << 7U;
constexpr u32 kSeenWtBidi = 1U << 8U;
constexpr u32 kSeenWtData = 1U << 9U;
constexpr u32 kSeenWtLegacy = 1U << 10U;
constexpr u32 kSeenWtEnabledDraft16 = 1U << 11U;

// Frame types HTTP/2 used, which HTTP/3 reserved (section 7.2.8).
[[nodiscard]] bool is_http2_frame(u64 type) noexcept
{
    return type == 0x2U || type == 0x6U || type == 0x8U || type == 0x9U;
}

[[nodiscard]] u64 grease_value(u64 seed) noexcept
{
    return (0x1FU * (seed % 0x10000U)) + 0x21U;
}

[[nodiscard]] bool is_client_bidirectional(u64 id) noexcept
{
    return (id & 3U) == 0U;
}

[[nodiscard]] u64 min_of(u64 a, u64 b) noexcept
{
    return (a < b) ? a : b;
}

[[nodiscard]] bool span_is(cont::ConstSpan<u8> bytes, const char* text, usize size) noexcept
{
    return bytes.size() == size && std::memcmp(bytes.data(), text, size) == 0;
}

// The most bytes QPACK can spend on `fields`: its prefix, and each line's longest integers.
[[nodiscard]] usize section_bound(cont::ConstSpan<HttpField> fields) noexcept
{
    usize bound = kHttp3SectionOverhead;
    for (const HttpField& field : fields)
    {
        bound += field.name.size() + field.value.size() + (2U * kHttpMaxIntegerBytes);
    }
    return bound;
}
} // namespace

bool http3_wt_token(cont::ConstSpan<u8> protocol, bool& legacy) noexcept
{
    legacy = span_is(protocol, "webtransport", 12U);
    return legacy || span_is(protocol, "webtransport-h3", 15U);
}

Http3Connection::~Http3Connection() noexcept
{
    if (m_allocator == nullptr)
    {
        return;
    }
    memory::destroy_array(*m_allocator, m_records, m_record_count);
    memory::destroy_array(*m_allocator, m_scratch, m_scratch_bytes);
    memory::destroy_array(*m_allocator, m_text, usize{m_config.field_bytes});
    memory::destroy_array(*m_allocator, m_fields, m_field_slots);
}

bool Http3Connection::init(memory::IAllocator& allocator, QuicStreamSet& streams, QuicDatagrams* datagrams,
                           const Http3Config& config) noexcept
{
    const QuicStreamLimits& limits = streams.limits();
    const u32 frame_bytes = config.field_bytes + kHttp3SectionOverhead + kHttp3FrameHeaderBytes;
    const bool requests =
        (config.side == crypto::QuicSide::Client) ? (limits.local_bidi != 0U) : (limits.peer_bidi != 0U);
    if (m_allocator != nullptr || config.field_bytes < kHttp3MinFieldBytes ||
        config.field_bytes > kHttp3MaxFieldBytes || limits.local_uni < 3U || limits.peer_uni < 3U || !requests ||
        limits.receive_bytes < frame_bytes || limits.stream_window < frame_bytes || config.datagram_bytes == 0U ||
        config.datagram_bytes > 65535U || (config.datagrams && datagrams == nullptr))
    {
        return false;
    }
    const Http3WebTransportSettings& offer = config.webtransport;
    if (offer.enabled && (!config.datagrams || !limits.reset_stream_at || offer.max_streams_uni > kWtMaxStreams ||
                          offer.max_streams_bidi > kWtMaxStreams || offer.max_data > kQuicMaxVarint ||
                          offer.legacy_sessions > kQuicMaxVarint ||
                          (config.side == crypto::QuicSide::Server && !config.connect_protocol)))
    {
        return false;
    }
    if (!m_decoder.init(allocator, config.decoder) || !m_encoder.init(allocator, config.encoder))
    {
        return false;
    }
    m_record_count = streams.slots();
    m_scratch_bytes = frame_bytes;
    if (usize{config.datagram_bytes} + kMaxVarintBytes > m_scratch_bytes)
    {
        m_scratch_bytes = usize{config.datagram_bytes} + kMaxVarintBytes;
    }
    m_field_slots = (usize{config.field_bytes} / kHttpFieldOverhead) + 1U;
    m_records = memory::try_construct_array<Record>(allocator, m_record_count);
    m_scratch = memory::try_construct_array<u8>(allocator, m_scratch_bytes);
    m_text = memory::try_construct_array<u8>(allocator, usize{config.field_bytes});
    m_fields = memory::try_construct_array<HttpField>(allocator, m_field_slots);
    m_allocator = &allocator;
    m_config = config;
    if (m_records == nullptr || m_scratch == nullptr || m_text == nullptr || m_fields == nullptr)
    {
        memory::destroy_array(allocator, m_records, m_record_count);
        memory::destroy_array(allocator, m_scratch, m_scratch_bytes);
        memory::destroy_array(allocator, m_text, usize{config.field_bytes});
        memory::destroy_array(allocator, m_fields, m_field_slots);
        m_records = nullptr;
        m_scratch = nullptr;
        m_text = nullptr;
        m_fields = nullptr;
        m_allocator = nullptr;
        return false;
    }
    m_streams = &streams;
    m_datagrams = datagrams;
    m_ready_head = m_record_count;
    m_ready_tail = m_record_count;
    if (config.remembered != nullptr)
    {
        m_remembered = *config.remembered;
        m_remembered.wt = Http3WebTransportSettings{}; // draft section 3.2: only this connection's count
        m_peer = m_remembered;
        m_has_remembered = true;
        m_encoder.peer_settings(m_peer.qpack_max_table_capacity, m_peer.qpack_blocked_streams);
    }
    return true;
}

usize Http3Connection::early_context(const Http3Config& config, cont::Span<u8> out) noexcept
{
    if (out.size() < kHttp3EarlyContextBytes)
    {
        return 0U;
    }
    cont::WireWriter writer(out);
    writer.put_u8(1U); // this layout
    writer.put_u32(config.decoder.table_capacity);
    writer.put_u32(config.decoder.blocked_streams);
    writer.put_u32(config.field_bytes);
    writer.put_u8(static_cast<u8>((config.connect_protocol ? 1U : 0U) | (config.datagrams ? 2U : 0U)));
    return kHttp3EarlyContextBytes;
}

bool Http3Connection::fail(u64 code) noexcept
{
    if (m_error == 0U)
    {
        m_error = code;
    }
    return false;
}

// ---- Records ----

Http3Connection::Record* Http3Connection::find(u64 id) noexcept
{
    const u32 slot = (m_streams != nullptr) ? m_streams->slot_of(id) : kQuicNoStreamSlot;
    if (slot == kQuicNoStreamSlot || !m_records[slot].used || m_records[slot].id != id)
    {
        return nullptr;
    }
    return &m_records[slot];
}

const Http3Connection::Record* Http3Connection::find(u64 id) const noexcept
{
    const u32 slot = (m_streams != nullptr) ? m_streams->slot_of(id) : kQuicNoStreamSlot;
    if (slot == kQuicNoStreamSlot || !m_records[slot].used || m_records[slot].id != id)
    {
        return nullptr;
    }
    return &m_records[slot];
}

Http3Connection::Record& Http3Connection::claim(u32 slot, u64 id) noexcept
{
    Record& record = m_records[slot];
    const bool queued = record.queued;
    const u32 next = record.next;
    record = Record{};
    record.queued = queued; // a slot reused while still on the ready list stays linked; advance() finds the new stream
    record.next = next;
    record.used = true;
    record.id = id;
    return record;
}

u32 Http3Connection::slot_index(const Record& record) const noexcept
{
    return static_cast<u32>(&record - m_records);
}

void Http3Connection::enqueue(u32 slot) noexcept
{
    Record& record = m_records[slot];
    if (record.queued)
    {
        return;
    }
    record.queued = true;
    record.next = m_record_count;
    if (m_ready_head == m_record_count)
    {
        m_ready_head = slot;
    }
    else
    {
        m_records[m_ready_tail].next = slot;
    }
    m_ready_tail = slot;
}

// ---- Events ----

bool Http3Connection::poll(Http3Event& event) noexcept
{
    if (m_datagram_held)
    {
        m_datagrams->pop();
        m_datagram_held = false;
    }
    if (m_streams == nullptr)
    {
        return false;
    }
    bool produced = false;
    while (!produced && m_error == 0U)
    {
        if (m_current.events != 0U)
        {
            produced = take_event_bit(event);
        }
        else if (m_streams->poll(m_current))
        {
            continue;
        }
        else if (m_ready_head != m_record_count)
        {
            const u32 slot = m_ready_head;
            Record& record = m_records[slot];
            m_ready_head = record.next;
            record.queued = false;
            if (advance(record, event))
            {
                enqueue(slot); // it may have more, in turn with the others
                produced = true;
            }
        }
        else if (m_datagrams != nullptr && m_datagrams->held() != 0U)
        {
            produced = take_datagram(event);
        }
        else
        {
            break;
        }
    }
    flush_instructions();
    return produced;
}

bool Http3Connection::take_event_bit(Http3Event& event) noexcept
{
    const u8 bit = static_cast<u8>(m_current.events & static_cast<u8>(-m_current.events));
    m_current.events = static_cast<u8>(m_current.events ^ bit);
    if (bit == kQuicStreamOpened)
    {
        opened(claim(m_current.slot, m_current.id));
        return false;
    }
    Record& record = m_records[m_current.slot];
    if (!record.used || record.id != m_current.id)
    {
        m_current.events = 0U; // the slot went to another stream
        return false;
    }
    switch (bit)
    {
        case kQuicStreamReset:
            return peer_reset(record, m_current.reset_error, event);
        case kQuicStreamStopSending:
            return peer_stop(record, m_current.stop_error, event);
        case kQuicStreamWritable:
            if (record.kind == Kind::Request || record.kind == Kind::WtData)
            {
                event = Http3Event{};
                event.kind = (record.kind == Kind::Request) ? Http3EventKind::Writable : Http3EventKind::WtWritable;
                event.stream = record.id;
                event.id = record.wt_session;
                event.header = record.wt_header;
                event.slot = m_current.slot;
                return true;
            }
            return false;
        case kQuicStreamReadable:
            record.signalled = record.signalled && record.kind != Kind::WtData;
            enqueue(m_current.slot);
            return false;
        default:
            return false;
    }
}

void Http3Connection::opened(Record& record) noexcept
{
    if ((record.id & 2U) != 0U)
    {
        record.kind = Kind::UniType;
        return;
    }
    if (m_config.webtransport.enabled)
    {
        record.kind = Kind::First;
        return;
    }
    if (m_config.side == crypto::QuicSide::Client)
    {
        fail(kH3StreamCreationError); // a server opens no bidirectional stream (RFC 9114 section 6.1)
        return;
    }
    admit_request(record);
}

void Http3Connection::admit_request(Record& record) noexcept
{
    record.kind = Kind::Request;
    if (m_goaway_was_sent && record.id >= m_goaway_sent)
    {
        Http3Event ignored;
        static_cast<void>(abort_stream(record, kH3RequestRejected, false, ignored));
        return;
    }
    if (record.id + 4U > m_next_request)
    {
        m_next_request = record.id + 4U;
    }
}

bool Http3Connection::peer_reset(Record& record, u64 code, Http3Event& event) noexcept
{
    if (record.kind == Kind::Control || record.kind == Kind::QpackEncoder || record.kind == Kind::QpackDecoder)
    {
        return fail(kH3ClosedCriticalStream);
    }
    if (record.kind == Kind::WtData && record.read != Read::Over)
    {
        record.read = Read::Over;
        event = Http3Event{};
        event.kind = Http3EventKind::WtAborted;
        event.stream = record.id;
        event.id = record.wt_session;
        event.error = code;
        event.final_size = m_current.reset_final;
        event.header = record.wt_header;
        event.slot = m_current.slot;
        return true;
    }
    if ((record.kind == Kind::First || record.kind == Kind::WtHead) && record.read != Read::Over)
    {
        // Given up before it said what it is: nothing was reported, and its other half must not pin the slot.
        record.read = Read::Over;
        static_cast<void>(m_streams->reset(record.id, kH3RequestCancelled));
        return false;
    }
    if (record.kind != Kind::Request || record.read == Read::Over)
    {
        return false;
    }
    record.read = Read::Over;
    record.frame = Frame::None;
    record.blocked = false;
    m_decoder.cancel(record.id);
    event = Http3Event{};
    event.kind = Http3EventKind::Aborted;
    event.stream = record.id;
    event.error = code;
    return true;
}

bool Http3Connection::peer_stop(Record& record, u64 code, Http3Event& event) noexcept
{
    if (record.id == m_control || record.id == m_encoder_stream || record.id == m_decoder_stream)
    {
        if (record.kind == Kind::LocalUni)
        {
            return fail(kH3ClosedCriticalStream);
        }
    }
    if ((record.kind != Kind::Request && record.kind != Kind::WtData) ||
        (record.kind == Kind::Request && record.send == Send::Over && record.read == Read::Over))
    {
        return false; // nothing is left that the application could stop doing
    }
    record.send = Send::Over; // QUIC reset the sending part
    event = Http3Event{};
    event.kind = (record.kind == Kind::Request) ? Http3EventKind::Stopped : Http3EventKind::WtStopped;
    event.stream = record.id;
    event.id = record.wt_session;
    event.error = code;
    event.final_size = m_current.stop_final;
    event.header = record.wt_header;
    event.slot = m_current.slot;
    return true;
}

void Http3Connection::drop_input(const Record& record) noexcept
{
    for (QuicStreamRead in = m_streams->peek(record.id); !in.data.empty() || in.fin; in = m_streams->peek(record.id))
    {
        if (m_streams->consume(record.id, in.data.size()) != QuicStreamError::None || in.fin)
        {
            return;
        }
    }
}

void Http3Connection::wake_unblocked() noexcept
{
    u64 stream = 0U;
    while (m_decoder.next_unblocked(stream))
    {
        Record* const record = find(stream);
        if (record != nullptr)
        {
            record->blocked = false;
            enqueue(slot_index(*record));
        }
    }
}

bool Http3Connection::take_datagram(Http3Event& event) noexcept
{
    cont::ConstSpan<u8> bytes;
    while (m_error == 0U && m_datagrams->peek(bytes))
    {
        cont::WireReader in(bytes);
        u64 quarter = 0U;
        if (!take_quic_varint(in, quarter) || quarter > kMaxQuarterStreamId)
        {
            return fail(kH3DatagramError);
        }
        const u64 id = quarter * 4U;
        Record* const record = find(id);
        const bool request = record != nullptr && record->kind == Kind::Request && record->read != Read::Over;
        // A server's request not yet read may still turn out to carry datagrams; a client knows what it sent. With
        // WebTransport the layer above decides what waits, as the session may not have been read yet (draft section
        // 4.6); otherwise it goes.
        const bool pending = request && !record->datagrams && record->read == Read::Head &&
                             m_config.side == crypto::QuicSide::Server;
        if (request && !record->datagrams && !pending)
        {
            m_datagrams->pop();
            return abort_stream(*record, kH3DatagramError, true, event);
        }
        const bool unread = record == nullptr || record->kind == Kind::First || pending;
        if (m_config.datagrams && ((request && record->datagrams) || (unread && m_config.webtransport.enabled)))
        {
            event = Http3Event{};
            event.kind = Http3EventKind::Datagram;
            event.stream = id;
            event.payload = in.rest();
            event.early = m_datagrams->oldest_early();
            event.unread = unread;
            m_datagram_held = true;
            return true;
        }
        m_datagrams->pop();
    }
    return false;
}

// ---- Reading ----

bool Http3Connection::advance(Record& record, Http3Event& event) noexcept
{
    if (m_streams->slot_of(record.id) == kQuicNoStreamSlot)
    {
        return false;
    }
    if (m_streams->peek(record.id).reset_coming && !keeps_reliable(record))
    {
        // RFC 9114 gives a reliable size no meaning, and bytes cut inside a frame would be waited on for ever.
        static_cast<void>(m_streams->give_up_reliable(record.id));
        return false;
    }
    switch (record.kind)
    {
        case Kind::UniType:
            read_uni_type(record);
            return (record.kind != Kind::UniType && m_error == 0U) ? advance(record, event) : false;
        case Kind::Control:
            return read_control(record, event);
        case Kind::QpackEncoder:
        case Kind::QpackDecoder:
            read_qpack(record);
            return false;
        case Kind::Request:
            return advance_request(record, event);
        case Kind::First:
            return read_first(record, event);
        case Kind::WtHead:
            return read_wt_head(record, event);
        case Kind::WtData:
            return read_wt_data(record, event);
        case Kind::Unused:
        case Kind::Discard:
        case Kind::LocalUni:
            return false;
    }
    return false;
}

bool Http3Connection::keeps_reliable(const Record& record) const noexcept
{
    return record.kind == Kind::First || record.kind == Kind::WtHead || record.kind == Kind::WtData ||
           (record.kind == Kind::UniType && m_config.webtransport.enabled);
}

bool Http3Connection::held_for_settings() const noexcept
{
    return m_config.side == crypto::QuicSide::Server && !m_peer_known; // draft section 7.1
}

bool Http3Connection::read_first(Record& record, Http3Event& event) noexcept
{
    if (record.read == Read::Over || held_for_settings())
    {
        return false;
    }
    u8 head[kMaxVarintBytes];
    bool ends = false;
    const usize bytes = m_streams->copy_readable(record.id, 0U, cont::Span<u8>(head, sizeof(head)), ends);
    cont::WireReader in(cont::ConstSpan<u8>(head, bytes));
    u64 type = 0U;
    const bool typed = take_quic_varint(in, type);
    if (!typed && !ends)
    {
        return false;
    }
    if (typed && type == kFrameWtStream)
    {
        static_cast<void>(m_streams->consume(record.id, in.consumed()));
        record.kind = Kind::WtHead;
        record.wt_header = static_cast<u32>(in.consumed());
        return read_wt_head(record, event);
    }
    if (m_config.side == crypto::QuicSide::Client)
    {
        if (typed)
        {
            return fail(kH3StreamCreationError); // only 0x41 makes a server's bidirectional stream legal (section 4.3)
        }
        return abort_stream(record, kH3RequestCancelled, false, event);
    }
    admit_request(record);
    if (record.read == Read::Over)
    {
        return false;
    }
    if (m_streams->peek(record.id).reset_coming)
    {
        static_cast<void>(m_streams->give_up_reliable(record.id));
        return false;
    }
    return advance_request(record, event);
}

bool Http3Connection::read_wt_head(Record& record, Http3Event& event) noexcept
{
    if (record.read == Read::Over || held_for_settings())
    {
        return false;
    }
    u8 head[kMaxVarintBytes];
    bool ends = false;
    const usize bytes = m_streams->copy_readable(record.id, 0U, cont::Span<u8>(head, sizeof(head)), ends);
    cont::WireReader in(cont::ConstSpan<u8>(head, bytes));
    u64 session = 0U;
    if (!take_quic_varint(in, session))
    {
        // A stream that ended, or a reliable size that cuts the header, names no session: the stream's reset.
        return ends ? abort_stream(record, kH3MessageError, false, event) : false;
    }
    if (!is_client_bidirectional(session))
    {
        return fail(kH3IdError); // section 4
    }
    if (m_streams->peek(record.id).early_bytes != 0U)
    {
        return abort_stream(record, kWtBufferedStreamRejected, false, event); // no session ever opens in 0-RTT
    }
    static_cast<void>(m_streams->consume(record.id, in.consumed(), true));
    record.wt_header += static_cast<u32>(in.consumed());
    record.wt_session = session;
    record.kind = Kind::WtData;
    event = Http3Event{};
    event.kind = Http3EventKind::WtStream;
    event.stream = record.id;
    event.id = session;
    event.header = record.wt_header;
    event.slot = slot_index(record);
    return true;
}

bool Http3Connection::read_wt_data(Record& record, Http3Event& event) noexcept
{
    const QuicStreamRead in = m_streams->peek(record.id);
    if (record.signalled || record.read == Read::Over || (in.data.empty() && !in.fin))
    {
        return false;
    }
    record.signalled = true;
    event = Http3Event{};
    event.kind = Http3EventKind::WtReadable;
    event.stream = record.id;
    event.id = record.wt_session;
    event.header = record.wt_header;
    event.slot = slot_index(record);
    return true;
}

void Http3Connection::read_uni_type(Record& record) noexcept
{
    u8 head[kMaxVarintBytes];
    bool ends = false;
    const usize bytes = m_streams->copy_readable(record.id, 0U, cont::Span<u8>(head, sizeof(head)), ends);
    cont::WireReader in(cont::ConstSpan<u8>(head, bytes));
    u64 type = 0U;
    if (!take_quic_varint(in, type))
    {
        if (ends)
        {
            static_cast<void>(m_streams->consume(record.id, bytes)); // closed before its type (section 6.2)
        }
        return;
    }
    static_cast<void>(m_streams->consume(record.id, in.consumed()));
    if (type == kStreamControl)
    {
        record.kind = Kind::Control;
        if (m_peer_control)
        {
            fail(kH3StreamCreationError);
        }
        m_peer_control = true;
    }
    else if (type == kStreamQpackEncoder)
    {
        record.kind = Kind::QpackEncoder;
        if (m_peer_encoder)
        {
            fail(kH3StreamCreationError);
        }
        m_peer_encoder = true;
    }
    else if (type == kStreamQpackDecoder)
    {
        record.kind = Kind::QpackDecoder;
        if (m_peer_decoder)
        {
            fail(kH3StreamCreationError);
        }
        m_peer_decoder = true;
    }
    else if (type == kStreamWebTransport && m_config.webtransport.enabled)
    {
        record.kind = Kind::WtHead;
        record.wt_header = static_cast<u32>(in.consumed());
    }
    else if (type == kStreamPush)
    {
        // A client never allowed a push, and a server is never pushed to (section 6.2.2).
        fail((m_config.side == crypto::QuicSide::Client) ? kH3IdError : kH3StreamCreationError);
    }
    else
    {
        record.kind = Kind::Discard;
        static_cast<void>(m_streams->stop_sending(record.id, kH3StreamCreationError));
    }
}

void Http3Connection::read_qpack(Record& record) noexcept
{
    for (;;)
    {
        const QuicStreamRead in = m_streams->peek(record.id);
        if (in.fin || in.reset)
        {
            fail(kH3ClosedCriticalStream);
            return;
        }
        if (in.data.empty())
        {
            return;
        }
        const bool applied = (record.kind == Kind::QpackEncoder) ? m_decoder.on_encoder_stream(in.data)
                                                                 : m_encoder.on_decoder_stream(in.data);
        if (!applied)
        {
            fail((record.kind == Kind::QpackEncoder) ? m_decoder.error() : m_encoder.error());
            return;
        }
        static_cast<void>(m_streams->consume(record.id, in.data.size()));
        if (record.kind == Kind::QpackEncoder)
        {
            wake_unblocked();
        }
    }
}

bool Http3Connection::skip_frame(Record& record, bool& more) noexcept
{
    const QuicStreamRead in = m_streams->peek(record.id);
    const u64 count = min_of(record.frame_left, in.data.size());
    static_cast<void>(m_streams->consume(record.id, static_cast<usize>(count), true));
    record.frame_left -= count;
    if (record.frame_left == 0U)
    {
        record.frame = Frame::None;
        more = true;
    }
    else if (count == 0U)
    {
        if (in.fin)
        {
            fail(kH3FrameError);
        }
        more = false;
    }
    else
    {
        more = true;
    }
    return false;
}

bool Http3Connection::read_control(Record& record, Http3Event& event) noexcept
{
    for (;;)
    {
        if (m_streams->peek(record.id).fin)
        {
            return fail(kH3ClosedCriticalStream);
        }
        if (record.frame == Frame::Skip)
        {
            bool more = false;
            static_cast<void>(skip_frame(record, more));
            if (!more || m_error != 0U)
            {
                return false;
            }
            continue;
        }
        bool ends = false;
        const usize have =
            m_streams->copy_readable(record.id, 0U, cont::Span<u8>(m_scratch, kHttp3FrameHeaderBytes), ends);
        cont::WireReader in(cont::ConstSpan<u8>(m_scratch, have));
        u64 type = 0U;
        u64 length = 0U;
        const bool typed = take_quic_varint(in, type);
        if (typed && type == kFrameWtStream && m_config.webtransport.enabled)
        {
            return fail(kH3FrameUnexpected); // draft section 4.3
        }
        if (!typed || !take_quic_varint(in, length))
        {
            return false;
        }
        const usize header = in.consumed();
        if (!m_settings_seen && type != kFrameSettings)
        {
            return fail(kH3MissingSettings);
        }
        if (type == kFrameData || type == kFrameHeaders || type == kFramePushPromise || is_http2_frame(type) ||
            (type == kFrameSettings && m_settings_seen) ||
            (type == kFrameMaxPushId && m_config.side == crypto::QuicSide::Client))
        {
            return fail(kH3FrameUnexpected);
        }
        const bool one_integer = type == kFrameCancelPush || type == kFrameGoAway || type == kFrameMaxPushId;
        if (one_integer && (length == 0U || length > kMaxVarintBytes))
        {
            return fail(kH3FrameError);
        }
        if (type != kFrameSettings && !one_integer)
        {
            static_cast<void>(m_streams->consume(record.id, header));
            record.frame = Frame::Skip;
            record.frame_left = length;
            continue;
        }
        if (length > m_scratch_bytes - header)
        {
            return fail(kHttp3ExcessiveLoad);
        }
        const usize total = header + static_cast<usize>(length);
        const cont::Span<u8> whole(m_scratch, total);
        if (m_streams->copy_readable(record.id, 0U, whole, ends) < total)
        {
            return false; // the frame stays in the stream's buffer until all of it came
        }
        static_cast<void>(m_streams->consume(record.id, total));
        const cont::ConstSpan<u8> payload(m_scratch + header, static_cast<usize>(length));
        if (type == kFrameSettings)
        {
            if (!read_settings(payload))
            {
                return false;
            }
            m_settings_seen = true;
            m_peer_known = true;
            for (u32 slot = 0U; m_config.webtransport.enabled && slot < m_record_count; ++slot)
            {
                if (m_records[slot].used &&
                    (m_records[slot].kind == Kind::First || m_records[slot].kind == Kind::WtHead))
                {
                    enqueue(slot); // what held_for_settings() left unread
                }
            }
            event = Http3Event{};
            event.kind = Http3EventKind::Settings;
            return true;
        }
        cont::WireReader value_in(payload);
        u64 value = 0U;
        if (!take_quic_varint(value_in, value) || !value_in.done())
        {
            return fail(kH3FrameError);
        }
        if (type == kFrameCancelPush)
        {
            return fail(kH3IdError); // no push was ever allowed or promised (section 7.2.3)
        }
        if (type == kFrameMaxPushId)
        {
            if (m_has_max_push_id && value < m_max_push_id)
            {
                return fail(kH3IdError);
            }
            m_has_max_push_id = true;
            m_max_push_id = value;
            continue;
        }
        if ((m_config.side == crypto::QuicSide::Client && !is_client_bidirectional(value)) ||
            (m_goaway_received && value > m_goaway_peer))
        {
            return fail(kH3IdError);
        }
        apply_goaway(value);
        event = Http3Event{};
        event.kind = Http3EventKind::GoAway;
        event.id = value;
        return true;
    }
}

void Http3Connection::apply_goaway(u64 value) noexcept
{
    m_goaway_received = true;
    m_goaway_peer = value;
    if (m_config.side != crypto::QuicSide::Client)
    {
        return; // a client's value names pushes, and none exists
    }
    for (u32 slot = 0U; slot < m_record_count; ++slot)
    {
        Record& record = m_records[slot];
        if (record.used && record.kind == Kind::Request && record.read != Read::Over && record.id >= value &&
            m_streams->slot_of(record.id) == slot)
        {
            record.rejected = true;
            enqueue(slot);
        }
    }
}

bool Http3Connection::read_settings(cont::ConstSpan<u8> payload) noexcept
{
    Http3Settings next;
    cont::WireReader in(payload);
    u32 seen = 0U;
    while (!in.done())
    {
        u64 id = 0U;
        u64 value = 0U;
        if (!take_quic_varint(in, id) || !take_quic_varint(in, value))
        {
            return fail(kH3FrameError);
        }
        u32 bit = 0U;
        const bool wt = m_config.webtransport.enabled; // otherwise these are identifiers like any unknown one
        switch (id)
        {
            case kSettingQpackMaxTableCapacity:
                bit = kSeenCapacity;
                next.qpack_max_table_capacity = value;
                break;
            case kSettingQpackBlockedStreams:
                bit = kSeenBlocked;
                next.qpack_blocked_streams = value;
                break;
            case kSettingMaxFieldSectionSize:
                bit = kSeenField;
                next.max_field_section_size = value;
                break;
            case kSettingEnableConnectProtocol:
                bit = kSeenConnect;
                next.enable_connect_protocol = value != 0U;
                break;
            case kSettingH3Datagram:
                bit = kSeenDatagram;
                next.h3_datagram = next.h3_datagram || value != 0U;
                break;
            case kSettingH3DatagramOld:
                bit = wt ? kSeenDatagramOld : 0U;
                next.h3_datagram = next.h3_datagram || (wt && value != 0U);
                break;
            case kSettingWtEnabled:
                bit = wt ? kSeenWtEnabled : 0U;
                next.wt.enabled = next.wt.enabled || (wt && value == 1U);
                break;
            case kSettingWtEnabledDraft16:
                bit = wt ? kSeenWtEnabledDraft16 : 0U;
                next.wt.enabled = next.wt.enabled || (wt && value == 1U);
                break;
            case kSettingWtMaxStreamsUni:
                bit = wt ? kSeenWtUni : 0U;
                next.wt.max_streams_uni = wt ? value : 0U;
                break;
            case kSettingWtMaxStreamsBidi:
                bit = wt ? kSeenWtBidi : 0U;
                next.wt.max_streams_bidi = wt ? value : 0U;
                break;
            case kSettingWtMaxData:
                bit = wt ? kSeenWtData : 0U;
                next.wt.max_data = wt ? value : 0U;
                break;
            case kSettingWtLegacySessions:
                bit = wt ? kSeenWtLegacy : 0U;
                next.wt.legacy_sessions = wt ? value : 0U;
                break;
            case 0x2U:
            case 0x3U:
            case 0x4U:
            case 0x5U:
                return fail(kH3SettingsError); // HTTP/2's, reserved (section 7.2.4.1)
            default:
                break; // unknown and reserved identifiers are ignored
        }
        // A server ignores a client's SETTINGS_WT_ENABLED, whatever its value (draft section 3.1).
        const bool boolean = id == kSettingEnableConnectProtocol || id == kSettingH3Datagram ||
                             bit == kSeenDatagramOld ||
                             ((bit == kSeenWtEnabled || bit == kSeenWtEnabledDraft16) &&
                              m_config.side == crypto::QuicSide::Client);
        if (boolean && value > 1U)
        {
            return fail(kH3SettingsError);
        }
        if ((seen & bit) != 0U)
        {
            return fail(kH3SettingsError);
        }
        seen |= bit;
    }
    if (m_has_remembered && (next.qpack_max_table_capacity < m_remembered.qpack_max_table_capacity ||
                             next.qpack_blocked_streams < m_remembered.qpack_blocked_streams ||
                             next.max_field_section_size < m_remembered.max_field_section_size ||
                             (m_remembered.enable_connect_protocol && !next.enable_connect_protocol) ||
                             (m_remembered.h3_datagram && !next.h3_datagram)))
    {
        return fail(kH3SettingsError); // a server that took 0-RTT may not lower them (section 7.2.4.2)
    }
    if (!m_has_remembered)
    {
        m_encoder.peer_settings(next.qpack_max_table_capacity, next.qpack_blocked_streams);
    }
    m_peer = next;
    return true;
}

bool Http3Connection::abort_stream(Record& record, u64 code, bool report, Http3Event& event) noexcept
{
    m_decoder.cancel(record.id);
    static_cast<void>(m_streams->stop_sending(record.id, code));
    drop_input(record);
    if (record.send != Send::Over)
    {
        static_cast<void>(m_streams->reset(record.id, code));
        record.send = Send::Over;
    }
    record.read = Read::Over;
    record.frame = Frame::None;
    record.blocked = false;
    if (report)
    {
        event = Http3Event{};
        event.kind = Http3EventKind::Aborted;
        event.stream = record.id;
        event.error = code;
    }
    return report;
}

bool Http3Connection::advance_request(Record& record, Http3Event& event) noexcept
{
    if (record.rejected)
    {
        record.rejected = false;
        const bool ok = abort_stream(record, kH3RequestCancelled, true, event);
        event.error = kH3RequestRejected;
        return ok;
    }
    while (record.read != Read::Over && m_error == 0U)
    {
        bool more = false;
        bool produced = false;
        switch (record.frame)
        {
            case Frame::None:
                produced = request_frame(record, event, more);
                break;
            case Frame::Data:
                if (record.frame_left == 0U)
                {
                    record.frame = Frame::None;
                    more = true;
                }
                else
                {
                    const QuicStreamRead in = m_streams->peek(record.id);
                    if (in.data.empty())
                    {
                        if (in.fin)
                        {
                            return fail(kH3FrameError);
                        }
                    }
                    else if (!record.signalled)
                    {
                        record.signalled = true;
                        event = Http3Event{};
                        event.kind = Http3EventKind::Body;
                        event.stream = record.id;
                        event.early = in.early_bytes != 0U;
                        produced = true;
                    }
                }
                break;
            case Frame::Skip:
                static_cast<void>(skip_frame(record, more));
                break;
            case Frame::Headers:
                produced = read_section(record, event, more);
                break;
        }
        if (produced)
        {
            return true;
        }
        if (!more)
        {
            return false;
        }
    }
    return false;
}

bool Http3Connection::request_frame(Record& record, Http3Event& event, bool& more) noexcept
{
    u8 head[kHttp3FrameHeaderBytes];
    bool ends = false;
    const usize have = m_streams->copy_readable(record.id, 0U, cont::Span<u8>(head, sizeof(head)), ends);
    cont::WireReader in(cont::ConstSpan<u8>(head, have));
    u64 type = 0U;
    u64 length = 0U;
    const bool typed = take_quic_varint(in, type);
    if (typed && type == kFrameWtStream && m_config.webtransport.enabled)
    {
        return fail(kH3FrameUnexpected); // only as a stream's very first bytes (draft section 4.3)
    }
    if (!typed || !take_quic_varint(in, length))
    {
        if (ends)
        {
            if (have == 0U)
            {
                return request_ended(record, event);
            }
            return fail(kH3FrameError);
        }
        return false;
    }
    if (type == kFramePushPromise && m_config.side == crypto::QuicSide::Client)
    {
        return fail(kH3IdError); // beyond a limit that was never raised (section 7.2.5)
    }
    if (type == kFrameCancelPush || type == kFrameSettings || type == kFramePushPromise || type == kFrameGoAway ||
        type == kFrameMaxPushId || is_http2_frame(type) || (type == kFrameData && (record.read != Read::Body)) ||
        (type == kFrameHeaders && (record.read == Read::Tail || (record.read == Read::Body && record.tunnel))))
    {
        return fail(kH3FrameUnexpected);
    }
    static_cast<void>(m_streams->consume(record.id, in.consumed(), true));
    more = true;
    if (type == kFrameData)
    {
        if (record.has_length)
        {
            if (length > record.content_left)
            {
                more = false;
                return abort_stream(record, kH3MessageError, true, event);
            }
            record.content_left -= length;
        }
        record.frame = Frame::Data;
        record.frame_left = length;
        record.signalled = false;
        return false;
    }
    if (type == kFrameHeaders)
    {
        if (length > u64{m_config.field_bytes} + kHttp3SectionOverhead)
        {
            more = false;
            if (m_config.side == crypto::QuicSide::Server && record.read == Read::Head)
            {
                m_decoder.cancel(record.id);
                return refuse_request(record);
            }
            return abort_stream(record, kHttp3ExcessiveLoad, true, event);
        }
        record.frame = Frame::Headers;
        record.section = static_cast<u32>(length);
        return false;
    }
    record.frame = Frame::Skip;
    record.frame_left = length;
    return false;
}

bool Http3Connection::refuse_request(Record& record) noexcept
{
    static constexpr u8 kStatus[] = {':', 's', 't', 'a', 't', 'u', 's'};
    static constexpr u8 kTooLarge[] = {'4', '3', '1'};
    const HttpField status{cont::ConstSpan<u8>(kStatus, sizeof(kStatus)),
                           cont::ConstSpan<u8>(kTooLarge, sizeof(kTooLarge))};
    static_cast<void>(m_streams->stop_sending(record.id, kHttp3ExcessiveLoad));
    drop_input(record);
    if (send_section(record, cont::ConstSpan<HttpField>(&status, 1U), true) == Http3Error::None)
    {
        record.send = Send::Over;
    }
    else
    {
        static_cast<void>(m_streams->reset(record.id, kHttp3ExcessiveLoad));
        record.send = Send::Over;
    }
    record.read = Read::Over;
    record.frame = Frame::None;
    return false; // the application never saw this request
}

bool Http3Connection::request_ended(Record& record, Http3Event& event) noexcept
{
    static_cast<void>(m_streams->consume(record.id, 0U));
    if (record.read == Read::Head)
    {
        const bool server = m_config.side == crypto::QuicSide::Server;
        return abort_stream(record, server ? kH3RequestIncomplete : kH3MessageError, !server, event);
    }
    if (record.has_length && record.content_left != 0U)
    {
        return abort_stream(record, kH3MessageError, true, event);
    }
    record.read = Read::Over;
    event = Http3Event{};
    event.kind = Http3EventKind::Finished;
    event.stream = record.id;
    return true;
}

bool Http3Connection::read_section(Record& record, Http3Event& event, bool& more) noexcept
{
    if (record.blocked)
    {
        return false;
    }
    const QuicStreamRead in = m_streams->peek(record.id);
    const usize want = record.section;
    cont::ConstSpan<u8> section;
    if (in.data.size() >= want)
    {
        section = in.data.subspan(0U, want);
    }
    else
    {
        bool ends = false;
        const usize copied = m_streams->copy_readable(record.id, 0U, cont::Span<u8>(m_scratch, want), ends);
        if (copied < want)
        {
            if (ends)
            {
                return fail(kH3FrameError);
            }
            return false;
        }
        section = cont::ConstSpan<u8>(m_scratch, want);
    }
    usize count = 0U;
    const QpackStatus status = m_decoder.decode(record.id, section, cont::Span<HttpField>(m_fields, m_field_slots),
                                                cont::Span<u8>(m_text, usize{m_config.field_bytes}), count);
    switch (status)
    {
        case QpackStatus::Failed:
            return fail(m_decoder.error());
        case QpackStatus::Blocked:
            record.blocked = true;
            return false;
        case QpackStatus::TooLarge:
            static_cast<void>(m_streams->consume(record.id, want, true));
            record.frame = Frame::None;
            if (m_config.side == crypto::QuicSide::Server && record.read == Read::Head)
            {
                return refuse_request(record);
            }
            return abort_stream(record, kHttp3ExcessiveLoad, true, event);
        case QpackStatus::Complete:
            break;
    }
    static_cast<void>(m_streams->consume(record.id, want, true));
    record.frame = Frame::None;
    more = true;
    return deliver_section(record, count, in.early_bytes != 0U, event);
}

bool Http3Connection::deliver_section(Record& record, usize count, bool early, Http3Event& event) noexcept
{
    const cont::ConstSpan<HttpField> fields(m_fields, count);
    const bool server = m_config.side == crypto::QuicSide::Server;
    event = Http3Event{};
    event.stream = record.id;
    event.fields = fields;
    event.early = early;
    if (record.read == Read::Body)
    {
        if (!http_check_trailers(fields))
        {
            return abort_stream(record, kH3MessageError, true, event);
        }
        record.read = Read::Tail;
        event.kind = Http3EventKind::Trailers;
        return true;
    }
    if (server)
    {
        if (!http_check_request(fields, event.info) || (event.info.extended && !m_config.connect_protocol))
        {
            return abort_stream(record, kH3MessageError, false, event);
        }
        bool legacy = false;
        if (m_config.webtransport.enabled && event.info.extended && http3_wt_token(event.info.protocol, legacy))
        {
            if (early)
            {
                return abort_stream(record, kH3RequestRejected, false, event); // CONNECT is not safe in 0-RTT
            }
            if (!wt_ready(legacy))
            {
                return abort_stream(record, kH3MessageError, false, event); // draft section 3.1
            }
        }
        record.read = Read::Body;
        record.tunnel = event.info.connect;
        record.datagrams = event.info.extended;
        record.has_length = event.info.has_content_length && !event.info.connect;
        record.content_left = event.info.content_length;
        event.kind = Http3EventKind::Headers;
        return true;
    }
    if (!http_check_response(fields, event.info))
    {
        return abort_stream(record, kH3MessageError, true, event);
    }
    if (event.info.status < 200U)
    {
        event.kind = Http3EventKind::Interim;
        return true;
    }
    record.read = Read::Body;
    const bool success = event.info.status < 300U;
    record.tunnel = record.tunnel && success;
    record.datagrams = record.datagrams && success;
    if (record.head || event.info.status == 204U || event.info.status == 304U)
    {
        record.has_length = true; // no content may follow, whatever content-length says
        record.content_left = 0U;
    }
    else
    {
        record.has_length = event.info.has_content_length && !record.tunnel;
        record.content_left = event.info.content_length;
    }
    event.kind = Http3EventKind::Headers;
    return true;
}

Http3Body Http3Connection::peek_body(u64 id) const noexcept
{
    Http3Body body;
    const Record* const record = find(id);
    if (record == nullptr || record->frame != Frame::Data || record->read == Read::Over)
    {
        return body;
    }
    const QuicStreamRead in = m_streams->peek(id);
    const usize count = static_cast<usize>(min_of(in.data.size(), record->frame_left));
    body.data = in.data.subspan(0U, count);
    body.early_bytes = (in.early_bytes < count) ? in.early_bytes : count;
    return body;
}

Http3Error Http3Connection::consume_body(u64 id, usize count) noexcept
{
    Record* const record = find(id);
    if (record == nullptr)
    {
        return Http3Error::Unknown;
    }
    if (record->frame != Frame::Data || count > record->frame_left ||
        m_streams->consume(id, count, true) != QuicStreamError::None)
    {
        return Http3Error::Invalid;
    }
    record->frame_left -= count;
    record->signalled = false;
    enqueue(slot_index(*record));
    return Http3Error::None;
}

// ---- Writing ----

bool Http3Connection::write_all(u64 id, cont::ConstSpan<u8> bytes, bool fin) noexcept
{
    usize taken = 0U;
    return m_streams->write(id, bytes, fin, taken) == QuicStreamError::None && taken == bytes.size();
}

bool Http3Connection::open_uni(u64 type, u64& id) noexcept
{
    if (m_streams->open(false, id) != QuicStreamError::None)
    {
        return false;
    }
    Record& record = claim(m_streams->slot_of(id), id);
    record.kind = Kind::LocalUni;
    u8 bytes[kMaxVarintBytes];
    cont::WireWriter out{cont::Span<u8>(bytes, sizeof(bytes))};
    put_quic_varint(out, type);
    return write_all(id, cont::ConstSpan<u8>(bytes, out.written()), false);
}

usize Http3Connection::write_settings(cont::Span<u8> out) const noexcept
{
    const u64 grease = grease_value(m_config.grease);
    const Http3WebTransportSettings& offer = m_config.webtransport;
    const u64 pairs[][2] = {
        {kSettingQpackMaxTableCapacity, m_config.decoder.table_capacity},
        {kSettingQpackBlockedStreams, m_config.decoder.blocked_streams},
        {kSettingMaxFieldSectionSize, m_config.field_bytes},
        {kSettingEnableConnectProtocol, m_config.connect_protocol ? 1U : 0U},
        {kSettingH3Datagram, m_config.datagrams ? 1U : 0U},
        {grease, m_config.grease & 0xFFU},
        // These go only when they say something.
        {kSettingWtEnabled, offer.enabled ? 1U : 0U},
        {kSettingWtEnabledDraft16, offer.enabled ? 1U : 0U},
        {kSettingWtMaxStreamsUni, offer.enabled ? offer.max_streams_uni : 0U},
        {kSettingWtMaxStreamsBidi, offer.enabled ? offer.max_streams_bidi : 0U},
        {kSettingWtMaxData, offer.enabled ? offer.max_data : 0U},
        {kSettingWtLegacySessions, offer.enabled ? offer.legacy_sessions : 0U},
    };
    constexpr usize always_sent = 6U;
    usize payload = 0U;
    usize at = 0U;
    for (const auto& pair : pairs)
    {
        payload += (at < always_sent || pair[1] != 0U) ? quic_varint_bytes(pair[0]) + quic_varint_bytes(pair[1]) : 0U;
        ++at;
    }
    cont::WireWriter writer(out);
    put_quic_varint(writer, kFrameSettings);
    put_quic_varint(writer, payload);
    at = 0U;
    for (const auto& pair : pairs)
    {
        if (at < always_sent || pair[1] != 0U)
        {
            put_quic_varint(writer, pair[0]);
            put_quic_varint(writer, pair[1]);
        }
        ++at;
    }
    return writer.written();
}

bool Http3Connection::start() noexcept
{
    if (m_streams == nullptr || m_started || m_error != 0U)
    {
        return false;
    }
    if (!open_uni(kStreamControl, m_control) || !open_uni(kStreamQpackEncoder, m_encoder_stream) ||
        !open_uni(kStreamQpackDecoder, m_decoder_stream))
    {
        return fail(kH3GeneralProtocolError);
    }
    u8 bytes[256];
    usize used = write_settings(cont::Span<u8>(bytes, sizeof(bytes)));
    cont::WireWriter grease{cont::Span<u8>(bytes + used, sizeof(bytes) - used)};
    put_quic_varint(grease, grease_value(m_config.grease >> 16U));
    put_quic_varint(grease, 1U);
    grease.put_u8(static_cast<u8>(m_config.grease));
    used += grease.written();
    if (!write_all(m_control, cont::ConstSpan<u8>(bytes, used), false))
    {
        return fail(kH3InternalError);
    }
    if (m_streams->peer_limit(false) > 3U && m_streams->limits().local_uni > 3U)
    {
        u64 id = 0U;
        if (open_uni(grease_value(m_config.grease >> 32U), id))
        {
            const u8 byte = static_cast<u8>(m_config.grease >> 8U);
            static_cast<void>(write_all(id, cont::ConstSpan<u8>(&byte, 1U), true));
        }
    }
    m_started = true;
    return true;
}

void Http3Connection::flush_instructions() noexcept
{
    if (!m_started || m_error != 0U)
    {
        return;
    }
    u8 bytes[512];
    for (bool encoder : {true, false})
    {
        const u64 stream = encoder ? m_encoder_stream : m_decoder_stream;
        for (;;)
        {
            const usize room = m_streams->send_room(stream, 1U);
            if (room == 0U)
            {
                break;
            }
            const cont::Span<u8> out(bytes, (room < sizeof(bytes)) ? room : sizeof(bytes));
            const usize moved = encoder ? m_encoder.take_instructions(out) : m_decoder.take_instructions(out);
            if (moved == 0U || !write_all(stream, cont::ConstSpan<u8>(bytes, moved), false))
            {
                break;
            }
        }
    }
}

Http3Error Http3Connection::send_section(Record& record, cont::ConstSpan<HttpField> fields, bool fin) noexcept
{
    if (http_section_size(fields) > m_peer.max_field_section_size)
    {
        return Http3Error::TooLarge;
    }
    const usize bound = section_bound(fields);
    const usize frame_room = bound + kHttp3FrameHeaderBytes;
    if (frame_room > m_scratch_bytes)
    {
        return Http3Error::TooLarge;
    }
    if (m_streams->send_room(record.id, frame_room) < frame_room)
    {
        return Http3Error::Blocked;
    }
    const usize bytes = m_encoder.encode(record.id, fields, cont::Span<u8>(m_scratch + kHttp3FrameHeaderBytes, bound));
    if (bytes == 0U)
    {
        return Http3Error::TooLarge;
    }
    const usize header = 1U + quic_varint_bytes(bytes);
    cont::WireWriter out{cont::Span<u8>(m_scratch + kHttp3FrameHeaderBytes - header, header)};
    put_quic_varint(out, kFrameHeaders);
    put_quic_varint(out, bytes);
    if (!write_all(record.id, cont::ConstSpan<u8>(m_scratch + kHttp3FrameHeaderBytes - header, header + bytes), fin))
    {
        return Http3Error::Unknown;
    }
    flush_instructions();
    return Http3Error::None;
}

Http3Error Http3Connection::open_request(cont::ConstSpan<HttpField> fields, bool fin, u64& id) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    if (!m_started || m_config.side != crypto::QuicSide::Client)
    {
        return Http3Error::NotReady;
    }
    if (m_goaway_received)
    {
        return Http3Error::GoingAway;
    }
    HttpMessageInfo info;
    if (!http_check_request(fields, info))
    {
        return Http3Error::Invalid;
    }
    bool legacy = false;
    const bool wt_session = m_config.webtransport.enabled && http3_wt_token(info.protocol, legacy);
    if (info.extended && (!m_peer.enable_connect_protocol || (wt_session && !wt_ready(legacy))))
    {
        return Http3Error::NotReady;
    }
    const usize frame_room = section_bound(fields) + kHttp3FrameHeaderBytes;
    if (frame_room > m_scratch_bytes || frame_room > m_streams->limits().send_bytes ||
        http_section_size(fields) > m_peer.max_field_section_size)
    {
        return Http3Error::TooLarge;
    }
    if (m_streams->open(true, id) != QuicStreamError::None)
    {
        return Http3Error::NoStream;
    }
    Record& record = claim(m_streams->slot_of(id), id);
    record.kind = Kind::Request;
    record.send = fin ? Send::Over : Send::Message;
    record.head = info.head;
    record.tunnel = info.connect;
    record.datagrams = info.extended;
    return send_section(record, fields, fin);
}

Http3Error Http3Connection::send_response(u64 id, cont::ConstSpan<HttpField> fields, bool fin) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->kind != Kind::Request || record->send == Send::Over)
    {
        return Http3Error::Unknown;
    }
    HttpMessageInfo info;
    if (m_config.side != crypto::QuicSide::Server || record->send != Send::Idle || !http_check_response(fields, info) ||
        (info.status < 200U && fin))
    {
        return Http3Error::Invalid;
    }
    const Http3Error sent = send_section(*record, fields, fin);
    if (sent == Http3Error::None && info.status >= 200U)
    {
        record->send = fin ? Send::Over : Send::Message;
    }
    return sent;
}

Http3Error Http3Connection::send_body(u64 id, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept
{
    taken = 0U;
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->send == Send::Over)
    {
        return Http3Error::Unknown;
    }
    if (record->send != Send::Message)
    {
        return Http3Error::Invalid;
    }
    if (data.empty())
    {
        return fin ? finish(id) : Http3Error::None;
    }
    const usize room = m_streams->send_room(id, data.size() + 9U);
    const usize header_most = 1U + quic_varint_bytes(static_cast<u64>(min_of(data.size(), room)));
    if (room <= header_most)
    {
        return Http3Error::Blocked;
    }
    const usize count = static_cast<usize>(min_of(data.size(), room - header_most));
    u8 header[9];
    cont::WireWriter out{cont::Span<u8>(header, sizeof(header))};
    put_quic_varint(out, kFrameData);
    put_quic_varint(out, count);
    const bool ends = fin && count == data.size();
    if (!write_all(id, cont::ConstSpan<u8>(header, out.written()), false) ||
        !write_all(id, data.subspan(0U, count), ends))
    {
        return Http3Error::Unknown;
    }
    taken = count;
    if (ends)
    {
        record->send = Send::Over;
    }
    return Http3Error::None;
}

Http3Error Http3Connection::send_trailers(u64 id, cont::ConstSpan<HttpField> fields) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->send == Send::Over)
    {
        return Http3Error::Unknown;
    }
    if (record->send != Send::Message || record->tunnel || !http_check_trailers(fields))
    {
        return Http3Error::Invalid;
    }
    const Http3Error sent = send_section(*record, fields, true);
    if (sent == Http3Error::None)
    {
        record->send = Send::Over;
    }
    return sent;
}

Http3Error Http3Connection::finish(u64 id) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->send == Send::Over)
    {
        return Http3Error::Unknown;
    }
    if (record->send != Send::Message)
    {
        return Http3Error::Invalid;
    }
    if (!write_all(id, {}, true))
    {
        return Http3Error::Unknown;
    }
    record->send = Send::Over;
    return Http3Error::None;
}

Http3Error Http3Connection::cancel(u64 id, u64 error) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->kind != Kind::Request)
    {
        return Http3Error::Unknown;
    }
    Http3Event ignored;
    static_cast<void>(abort_stream(*record, error, false, ignored));
    return Http3Error::None;
}

Http3Error Http3Connection::send_datagram(u64 id, cont::ConstSpan<u8> payload, u64 now_ns) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    if (!m_config.datagrams || !m_peer.h3_datagram || !m_datagrams->sendable())
    {
        return Http3Error::NotReady;
    }
    const Record* const record = find(id);
    if (record == nullptr || record->kind != Kind::Request || record->send == Send::Over)
    {
        return Http3Error::Unknown;
    }
    if (!record->datagrams)
    {
        return Http3Error::Invalid;
    }
    if (payload.size() > m_config.datagram_bytes)
    {
        return Http3Error::TooLarge;
    }
    cont::WireWriter out{cont::Span<u8>(m_scratch, m_scratch_bytes)};
    put_quic_varint(out, id / 4U);
    const usize prefix = out.written();
    std::memcpy(m_scratch + prefix, payload.data(), payload.size());
    u64 datagram = 0U;
    switch (m_datagrams->queue(cont::ConstSpan<u8>(m_scratch, prefix + payload.size()), now_ns, datagram))
    {
        case QuicDatagramError::None:
            return Http3Error::None;
        case QuicDatagramError::TooLarge:
            return Http3Error::TooLarge;
        case QuicDatagramError::Full:
            return Http3Error::Blocked;
        case QuicDatagramError::NotReady:
        case QuicDatagramError::Unsupported:
            return Http3Error::NotReady;
    }
    return Http3Error::NotReady;
}

bool Http3Connection::wt_ready(bool legacy) const noexcept
{
    if (!m_config.webtransport.enabled || !m_peer_known || !m_peer.h3_datagram || !m_datagrams->sendable())
    {
        return false;
    }
    if (m_config.side == crypto::QuicSide::Server)
    {
        return legacy || m_streams->peer_resets_at();
    }
    return legacy ? m_peer.wt.legacy_sessions != 0U
                  : m_peer.wt.enabled && m_peer.enable_connect_protocol && m_streams->peer_resets_at();
}

Http3Error Http3Connection::open_wt_stream(u64 session, bool bidirectional, u64& id) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    if (!m_started || !m_config.webtransport.enabled)
    {
        return Http3Error::NotReady;
    }
    const Record* const connect = find(session);
    if (connect == nullptr || connect->kind != Kind::Request || !connect->datagrams || connect->read == Read::Over)
    {
        return Http3Error::Unknown;
    }
    if (m_streams->open(bidirectional, id) != QuicStreamError::None)
    {
        return Http3Error::NoStream;
    }
    u8 bytes[2U * kMaxVarintBytes];
    cont::WireWriter out{cont::Span<u8>(bytes, sizeof(bytes))};
    put_quic_varint(out, bidirectional ? kFrameWtStream : kStreamWebTransport);
    put_quic_varint(out, session);
    Record& record = claim(m_streams->slot_of(id), id);
    record.kind = Kind::WtData;
    record.wt_session = session;
    record.wt_header = static_cast<u32>(out.written());
    record.read = bidirectional ? Read::Body : Read::Over;
    return write_all(id, cont::ConstSpan<u8>(bytes, out.written()), false) ? Http3Error::None : Http3Error::Unknown;
}

u64 Http3Connection::sent_header(const Record& record) const noexcept
{
    const bool local = ((record.id & 1U) != 0U) == (m_config.side == crypto::QuicSide::Server);
    return local ? u64{record.wt_header} : 0U; // a header goes only in the direction of the stream's opener
}

Http3Error Http3Connection::reset_wt(u64 id, u32 code, u64 offset) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    const Record* const record = find(id);
    if (record == nullptr || record->kind != Kind::WtData)
    {
        return Http3Error::Unknown;
    }
    const bool reliable = m_streams->peer_resets_at();
    if (!reliable && offset != 0U)
    {
        return Http3Error::Invalid;
    }
    switch (m_streams->reset(id, wt_http_code(code), reliable ? sent_header(*record) + offset : 0U))
    {
        case QuicStreamError::None:
            return Http3Error::None;
        case QuicStreamError::Invalid:
            return Http3Error::Invalid;
        default:
            return Http3Error::Unknown;
    }
}

Http3Error Http3Connection::stop_wt(u64 id, u32 code) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    const Record* const record = find(id);
    if (record == nullptr || record->kind != Kind::WtData ||
        m_streams->stop_sending(id, wt_http_code(code)) != QuicStreamError::None)
    {
        return Http3Error::Unknown;
    }
    return Http3Error::None;
}

Http3Error Http3Connection::abort_wt(u64 id, u64 http_code) noexcept
{
    if (m_error != 0U)
    {
        return Http3Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->kind != Kind::WtData)
    {
        return Http3Error::Unknown;
    }
    static_cast<void>(m_streams->stop_sending(id, http_code));
    drop_input(*record);
    static_cast<void>(m_streams->reset(id, http_code, m_streams->peer_resets_at() ? sent_header(*record) : 0U));
    record->read = Read::Over;
    return Http3Error::None;
}

u32 Http3Connection::wt_header(u64 id) const noexcept
{
    const Record* const record = find(id);
    return (record != nullptr && record->kind == Kind::WtData) ? record->wt_header : 0U;
}

bool Http3Connection::wt_session_pending(u64 id) const noexcept
{
    if (m_config.side != crypto::QuicSide::Server)
    {
        return false;
    }
    const Record* const record = find(id);
    if (record == nullptr)
    {
        return !m_streams->stream_closed(id);
    }
    return (record->kind == Kind::First && record->read != Read::Over) ||
           (record->kind == Kind::Request && record->read == Read::Head);
}

bool Http3Connection::goaway(bool immediate) noexcept
{
    if (!m_started || m_error != 0U)
    {
        return false;
    }
    const bool server = m_config.side == crypto::QuicSide::Server;
    u64 value = immediate ? 0U : kQuicMaxVarint;
    if (server)
    {
        value = immediate ? m_next_request : kShutdownRequestId;
    }
    if (m_goaway_was_sent && value > m_goaway_sent)
    {
        value = m_goaway_sent; // identifiers must not increase (RFC 9114 section 5.2)
    }
    u8 bytes[kMaxVarintBytes + 2U];
    cont::WireWriter out{cont::Span<u8>(bytes, sizeof(bytes))};
    put_quic_varint(out, kFrameGoAway);
    put_quic_varint(out, quic_varint_bytes(value));
    put_quic_varint(out, value);
    if (m_streams->send_room(m_control, out.written()) < out.written() ||
        !write_all(m_control, cont::ConstSpan<u8>(bytes, out.written()), false))
    {
        return false;
    }
    m_goaway_sent = value;
    m_goaway_was_sent = true;
    return true;
}

} // namespace crd::net
