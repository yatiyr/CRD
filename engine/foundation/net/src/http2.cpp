// crd-net -- an HTTP/2 connection over a byte stream (4.2a; docs/design/network-foundation.md#http2).

#include <crd/containers/wire_bytes.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/http2.hpp>

#include <algorithm>
#include <cstring>

namespace crd::net
{
namespace
{
// RFC 9113 section 6.
constexpr u8 kFrameData = 0x0U;
constexpr u8 kFrameHeaders = 0x1U;
constexpr u8 kFramePriority = 0x2U;
constexpr u8 kFrameRstStream = 0x3U;
constexpr u8 kFrameSettings = 0x4U;
constexpr u8 kFramePushPromise = 0x5U;
constexpr u8 kFramePing = 0x6U;
constexpr u8 kFrameGoAway = 0x7U;
constexpr u8 kFrameWindowUpdate = 0x8U;
constexpr u8 kFrameContinuation = 0x9U;

constexpr u8 kFlagEndStream = 0x1U;
constexpr u8 kFlagAck = 0x1U;
constexpr u8 kFlagEndHeaders = 0x4U;
constexpr u8 kFlagPadded = 0x8U;
constexpr u8 kFlagPriority = 0x20U;

constexpr u16 kSettingHeaderTableSize = 0x1U;
constexpr u16 kSettingEnablePush = 0x2U;
constexpr u16 kSettingMaxConcurrentStreams = 0x3U;
constexpr u16 kSettingInitialWindowSize = 0x4U;
constexpr u16 kSettingMaxFrameSize = 0x5U;
constexpr u16 kSettingMaxHeaderListSize = 0x6U;
constexpr u16 kSettingEnableConnectProtocol = 0x8U; // RFC 8441 section 3
constexpr u16 kSettingNoRfc7540Priorities = 0x9U;   // RFC 9218 section 2.1

constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr usize kSettingBytes = 6U;
constexpr usize kPrioritySpecBytes = 5U;
constexpr usize kRstStreamBytes = 4U;
constexpr usize kPingBytes = 8U;
constexpr usize kGoAwayMinBytes = 8U;
constexpr usize kWindowUpdateBytes = 4U;
constexpr usize kMaxOwnSettings = 8U;
constexpr u32 kResetFlag = 0x80000000U; // on a remembered stream: this side reset it

// What one frame read can write at most: a RST_STREAM, two WINDOW_UPDATEs and a GOAWAY. Every writer leaves this much
// and reading stops below it, so a connection error always finds the room for its GOAWAY.
constexpr usize kControlRoom = 64U;
// A WINDOW_UPDATE that small is charged to the control gate: it asks for data a few bytes at a time.
constexpr u32 kTinyWindowUpdate = 1024U;

[[nodiscard]] constexpr u32 frames_for(usize block, u32 frame_bytes) noexcept
{
    return (block == 0U) ? 1U : static_cast<u32>(((block - 1U) / frame_bytes) + 1U);
}

void put_header(u8* at, usize length, u8 type, u8 flags, u32 stream) noexcept
{
    at[0] = static_cast<u8>(length >> 16U);
    at[1] = static_cast<u8>(length >> 8U);
    at[2] = static_cast<u8>(length);
    at[3] = type;
    at[4] = flags;
    cont::wire_store<u32>(cont::Span<u8>(at, kHttp2FrameHeaderBytes), 5U, stream);
}
} // namespace

Http2Connection::~Http2Connection() noexcept
{
    destroy();
}

void Http2Connection::destroy() noexcept
{
    if (m_allocator == nullptr)
    {
        return;
    }
    memory::destroy_array(*m_allocator, m_records, m_config.streams);
    memory::destroy_array(*m_allocator, m_index, m_config.streams);
    memory::destroy_array(*m_allocator, m_recent, m_recent_slots);
    memory::destroy_array(*m_allocator, m_out, m_out_bytes);
    memory::destroy_array(*m_allocator, m_block, m_block_bytes);
    memory::destroy_array(*m_allocator, m_text, m_config.field_bytes);
    memory::destroy_array(*m_allocator, m_fields, m_field_slots);
    m_allocator = nullptr;
}

bool Http2Connection::init(memory::IAllocator& allocator, const Http2Config& config, u64 now_ns) noexcept
{
    const bool valid = m_out == nullptr && config.field_bytes >= kHttp2MinFieldBytes &&
                       config.field_bytes <= kHttp2MaxFieldBytes && config.streams != 0U &&
                       config.streams <= kHttp2MaxStreams && config.stream_window != 0U &&
                       config.stream_window <= kHttp2MaxWindow && config.connection_window >= kHttp2InitialWindow &&
                       config.connection_window <= kHttp2MaxWindow && config.frame_bytes >= kHttp2MinFrameBytes &&
                       config.frame_bytes <= kHttp2MaxFrameBytes && config.table_bytes <= kHpackMaxTableSize &&
                       config.encoder_table_bytes <= kHpackMaxTableSize &&
                       config.output_bytes >= kHttp2MinOutputBytes && config.resets.per_second != 0U &&
                       config.control.per_second != 0U && config.settings_timeout_ns != 0U &&
                       (!config.connect_protocol || config.side == Http2Side::Server);
    if (!valid || !m_decoder.init(allocator, config.table_bytes) ||
        !m_encoder.init(allocator, config.encoder_table_bytes))
    {
        return false;
    }
    m_allocator = &allocator;
    m_config = config;
    m_recent_slots = config.streams * 2U;
    m_out_bytes = config.output_bytes;
    m_block_bytes = config.field_bytes * 2U;
    m_field_slots = (config.field_bytes / kHttpFieldOverhead) + 1U;
    m_records = memory::try_construct_array<Record>(allocator, config.streams);
    m_index = memory::try_construct_array<IndexEntry>(allocator, config.streams);
    m_recent = memory::try_construct_array<u32>(allocator, m_recent_slots);
    m_out = memory::try_construct_array<u8>(allocator, m_out_bytes);
    m_block = memory::try_construct_array<u8>(allocator, m_block_bytes);
    m_text = memory::try_construct_array<u8>(allocator, config.field_bytes);
    m_fields = memory::try_construct_array<HttpField>(allocator, m_field_slots);
    if (m_records == nullptr || m_index == nullptr || m_recent == nullptr || m_out == nullptr || m_block == nullptr ||
        m_text == nullptr || m_fields == nullptr)
    {
        destroy();
        m_out = nullptr;
        return false;
    }
    for (u32 slot = 0U; slot < config.streams; ++slot)
    {
        m_records[slot].next_free = slot + 1U;
    }
    m_stage = (config.side == Http2Side::Server) ? Stage::Preface : Stage::FrameHeader;
    if (config.side == Http2Side::Client)
    {
        std::memcpy(output_tail(kHttp2PrefaceBytes), kPreface, kHttp2PrefaceBytes);
    }
    write_settings();
    if (config.connection_window > kHttp2InitialWindow)
    {
        u8 payload[kWindowUpdateBytes] = {};
        cont::wire_store<u32>(payload, 0U, config.connection_window - kHttp2InitialWindow);
        put_frame(kFrameWindowUpdate, 0U, 0U, payload);
        m_recv_window = config.connection_window;
    }
    m_settings_deadline_ns =
        (config.settings_timeout_ns > kNeverNs - now_ns) ? kNeverNs : (now_ns + config.settings_timeout_ns);
    return true;
}

// --- Streams -------------------------------------------------------------------------------------------------------

usize Http2Connection::position_of(u32 id) const noexcept
{
    const IndexEntry* const first = m_index;
    const IndexEntry* const at = std::lower_bound(first, first + m_open, id, [](const IndexEntry& entry, u32 value)
                                                  { return entry.id < value; });
    return static_cast<usize>(at - first);
}

Http2Connection::Record* Http2Connection::find(u32 id) noexcept
{
    const usize at = position_of(id);
    return (at < m_open && m_index[at].id == id) ? &m_records[m_index[at].slot] : nullptr;
}

Http2Connection::Record& Http2Connection::claim(u32 id) noexcept
{
    const u32 slot = m_free;
    Record& record = m_records[slot];
    m_free = record.next_free;
    record = Record{};
    record.id = id;
    record.send_window = m_peer.initial_window;
    record.recv_window = m_local_window;
    m_index[m_open] = IndexEntry{id, slot};
    ++m_open;
    return record;
}

void Http2Connection::release(Record& record, bool reset) noexcept
{
    if (record.unconsumed != 0U)
    {
        credit_connection(record.unconsumed);
    }
    if (record.want_write)
    {
        --m_wants;
    }
    remember_closed(record.id, reset);
    const usize at = position_of(record.id);
    std::memmove(m_index + at, m_index + at + 1U, (m_open - at - 1U) * sizeof(IndexEntry));
    --m_open;
    record.next_free = m_free;
    m_free = static_cast<u32>(&record - m_records);
}

void Http2Connection::remember_closed(u32 id, bool reset) noexcept
{
    m_recent_floor = std::max(m_recent_floor, m_recent[m_recent_at] & kHttp2MaxStreamId);
    m_recent[m_recent_at] = id | (reset ? kResetFlag : 0U);
    m_recent_at = (m_recent_at + 1U) % m_recent_slots;
}

bool Http2Connection::idle(u32 id) const noexcept
{
    const bool past = (m_config.side == Http2Side::Server) ? (id > m_highest) : (id >= m_next_local);
    return past || (id & 1U) == 0U; // nothing opens even streams: push is never offered
}

Http2Connection::Closed Http2Connection::closed_kind(u32 id) noexcept
{
    for (u32 slot = 0U; slot < m_recent_slots; ++slot)
    {
        if ((m_recent[slot] & kHttp2MaxStreamId) == id)
        {
            return ((m_recent[slot] & kResetFlag) != 0U) ? Closed::Discard : Closed::Reset;
        }
    }
    if (id > m_recent_floor)
    {
        fail(kH2ProtocolError); // skipped over and never opened: section 5.1.1
        return Closed::Failed;
    }
    return Closed::Reset;
}

u32 Http2Connection::open_limit() const noexcept
{
    return std::min(m_config.streams, m_peer.max_concurrent_streams);
}

// --- Reading -------------------------------------------------------------------------------------------------------

void Http2Connection::fail(u32 code) noexcept
{
    if (m_error != 0U)
    {
        return;
    }
    m_error = code;
    put_goaway((m_config.side == Http2Side::Server) ? m_highest : 0U, code);
}

bool Http2Connection::charge(RateGate& gate, const GateRule& rule) noexcept
{
    if (gate.open_at(rule) > m_now_ns)
    {
        fail(kH2EnhanceYourCalm);
        return false;
    }
    gate.charge(1U, rule, m_now_ns);
    return true;
}

bool Http2Connection::poll(cont::ConstSpan<u8> input, u64 now_ns, usize& taken, Http2Event& event) noexcept
{
    taken = 0U;
    m_now_ns = now_ns;
    if (m_error != 0U)
    {
        return false;
    }
    if (now_ns >= m_settings_deadline_ns)
    {
        fail(kH2SettingsTimeout);
        return false;
    }
    if (m_credit_owed && output_room() >= kControlRoom)
    {
        m_credit_owed = false;
        flush_credit(nullptr);
        for (u32 at = 0U; at < m_open; ++at)
        {
            flush_credit(&m_records[m_index[at].slot]);
        }
    }
    for (;;)
    {
        if (take_owed(event))
        {
            return true;
        }
        if (m_error != 0U || output_room() < kControlRoom)
        {
            return false;
        }
        usize used = 0U;
        const Step result = step(input.subspan(taken), used, event);
        taken += used;
        if (result != Step::Again)
        {
            return result == Step::Event;
        }
    }
}

bool Http2Connection::take_owed(Http2Event& event) noexcept
{
    if (m_owed_id != 0U)
    {
        const u32 id = m_owed_id;
        m_owed_id = 0U;
        Record* const record = find(id);
        if (record != nullptr)
        {
            return message_end(*record, event) == Step::Event;
        }
    }
    m_sweep = m_sweep && m_open != 0U && m_index[m_open - 1U].id > m_goaway_peer;
    if (m_sweep)
    {
        Record& record = m_records[m_index[m_open - 1U].slot];
        event = Http2Event{};
        event.kind = Http2EventKind::Aborted;
        event.stream = record.id;
        event.error = kH2RefusedStream;
        release(record, true);
        return true;
    }
    if (!m_wake)
    {
        return false;
    }
    for (; m_wake_at < m_open && m_wants != 0U; ++m_wake_at)
    {
        Record& record = m_records[m_index[m_wake_at].slot];
        if (record.want_write)
        {
            record.want_write = false;
            --m_wants;
            event = Http2Event{};
            event.kind = Http2EventKind::Writable;
            event.stream = record.id;
            return true;
        }
    }
    m_wake = false;
    m_wake_at = 0U;
    if (m_want_open)
    {
        m_want_open = false;
        event = Http2Event{};
        event.kind = Http2EventKind::Writable;
        return true;
    }
    return false;
}

Http2Connection::Step Http2Connection::step(cont::ConstSpan<u8> view, usize& used, Http2Event& event) noexcept
{
    switch (m_stage)
    {
        case Stage::Preface:
            return read_preface(view, used);
        case Stage::FrameHeader:
            return read_frame_header(view, used, event);
        case Stage::Body:
            return read_body(view, used, event);
        case Stage::Fragment:
            if (m_left == 0U)
            {
                m_stage = Stage::Padding;
                return Step::Again;
            }
            if (view.empty())
            {
                return Step::Wait;
            }
            used = std::min<usize>(m_left, view.size());
            std::memcpy(m_block + m_block_used, view.data(), used);
            m_block_used += static_cast<u32>(used);
            m_left -= static_cast<u32>(used);
            return Step::Again;
        case Stage::Padding:
            if (m_pad_left == 0U)
            {
                return frame_done(event);
            }
            if (view.empty())
            {
                return Step::Wait;
            }
            used = std::min<usize>(m_pad_left, view.size());
            m_pad_left -= static_cast<u32>(used);
            return Step::Again;
        case Stage::Skip:
            break;
    }
    if (m_left == 0U)
    {
        m_stage = Stage::FrameHeader;
        return Step::Again;
    }
    if (view.empty())
    {
        return Step::Wait;
    }
    used = std::min<usize>(m_left, view.size());
    m_left -= static_cast<u32>(used);
    return Step::Again;
}

Http2Connection::Step Http2Connection::read_preface(cont::ConstSpan<u8> view, usize& used) noexcept
{
    const usize count = std::min<usize>(view.size(), kHttp2PrefaceBytes - m_preface_at);
    if (count == 0U)
    {
        return Step::Wait;
    }
    if (std::memcmp(view.data(), kPreface + m_preface_at, count) != 0)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    used = count;
    m_preface_at = static_cast<u8>(m_preface_at + count);
    if (m_preface_at == kHttp2PrefaceBytes)
    {
        m_stage = Stage::FrameHeader;
    }
    return Step::Again;
}

Http2Connection::Step Http2Connection::read_frame_header(cont::ConstSpan<u8> view, usize& used,
                                                         Http2Event& event) noexcept
{
    if (view.size() < kHttp2FrameHeaderBytes)
    {
        return Step::Wait;
    }
    const u32 length = (u32{view[0]} << 16U) | (u32{view[1]} << 8U) | view[2];
    const u8 type = view[3];
    const u8 flags = view[4];
    const u32 id = cont::wire_load<u32>(view, 5U) & kHttp2MaxStreamId;
    if (length > m_local_frame_bytes)
    {
        fail(kH2FrameSizeError);
        return Step::Wait;
    }
    const bool settings_first = type == kFrameSettings && (flags & kFlagAck) == 0U;
    if ((m_need_settings && !settings_first) || (m_block_open && (type != kFrameContinuation || id != m_block_id)) ||
        type == kFramePushPromise)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    m_frame_flags = flags;
    m_frame_id = id;
    switch (type)
    {
        case kFrameData:
            return read_data(view, length, flags, id, used, event);
        case kFrameHeaders:
            return read_headers(view, length, flags, id, used);
        case kFrameContinuation:
            return read_continuation(length, flags, used);
        case kFramePriority:
        case kFrameRstStream:
        case kFrameSettings:
        case kFramePing:
        case kFrameGoAway:
        case kFrameWindowUpdate:
            break;
        default: // section 5.5: ignored
            used = kHttp2FrameHeaderBytes;
            m_left = length;
            m_stage = Stage::Skip;
            return Step::Again;
    }
    if (view.size() - kHttp2FrameHeaderBytes < length)
    {
        return Step::Wait;
    }
    used = kHttp2FrameHeaderBytes + length;
    const cont::ConstSpan<u8> payload = view.subspan(kHttp2FrameHeaderBytes, length);
    switch (type)
    {
        case kFramePriority:
            return read_priority(payload, id, event);
        case kFrameRstStream:
            return read_rst_stream(payload, id, event);
        case kFrameSettings:
            return read_settings(payload, flags, id, event);
        case kFramePing:
            return read_ping(payload, flags, id);
        case kFrameGoAway:
            return read_goaway(payload, id, event);
        default:
            return read_window_update(payload, id, event);
    }
}

Http2Connection::Step Http2Connection::read_data(cont::ConstSpan<u8> view, u32 length, u8 flags, u32 id, usize& used,
                                                 Http2Event& event) noexcept
{
    const u32 prefix = ((flags & kFlagPadded) != 0U) ? 1U : 0U;
    if (id == 0U || idle(id))
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    if (length < prefix)
    {
        fail(kH2FrameSizeError);
        return Step::Wait;
    }
    if (view.size() - kHttp2FrameHeaderBytes < prefix)
    {
        return Step::Wait;
    }
    const u32 pad = (prefix != 0U) ? view[kHttp2FrameHeaderBytes] : 0U;
    if (prefix != 0U && pad >= length)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    if (length > m_recv_window)
    {
        fail(kH2FlowControlError);
        return Step::Wait;
    }
    m_recv_window -= length;
    used = kHttp2FrameHeaderBytes + prefix;
    Record* const record = find(id);
    if (record == nullptr)
    {
        const Closed kind = closed_kind(id);
        if (kind == Closed::Failed)
        {
            return Step::Wait;
        }
        m_left = length - prefix;
        m_stage = Stage::Skip;
        credit_connection(length);
        if (kind == Closed::Reset)
        {
            refuse_stream(id, kH2StreamClosed);
        }
        return Step::Again;
    }
    if (record->read == Read::Over)
    {
        return refuse_data(*record, kH2StreamClosed, length, prefix, event);
    }
    if (record->read == Read::Head)
    {
        return refuse_data(*record, kH2ProtocolError, length, prefix, event);
    }
    if (length > record->recv_window)
    {
        return refuse_data(*record, kH2FlowControlError, length, prefix, event);
    }
    const u32 content = length - prefix - pad;
    if (record->has_length && content > record->content_left)
    {
        return refuse_data(*record, kH2ProtocolError, length, prefix, event);
    }
    if (content == 0U && (flags & kFlagEndStream) == 0U && !charge(m_control_gate, m_config.control))
    {
        return Step::Wait;
    }
    record->recv_window -= length;
    record->content_left -= record->has_length ? content : 0U;
    if (prefix + pad != 0U)
    {
        credit_connection(prefix + pad);
        credit_stream(*record, prefix + pad);
    }
    m_frame_is_block = false;
    m_left = content;
    m_pad_left = pad;
    m_stage = Stage::Body;
    return Step::Again;
}

Http2Connection::Step Http2Connection::refuse_data(Record& record, u32 code, u32 length, u32 prefix,
                                                   Http2Event& event) noexcept
{
    m_left = length - prefix;
    m_stage = Stage::Skip;
    credit_connection(length);
    return stream_error(record, code, event);
}

Http2Connection::Step Http2Connection::read_headers(cont::ConstSpan<u8> view, u32 length, u8 flags, u32 id,
                                                    usize& used) noexcept
{
    const u32 pad_field = ((flags & kFlagPadded) != 0U) ? 1U : 0U;
    const u32 spec = ((flags & kFlagPriority) != 0U) ? kPrioritySpecBytes : 0U;
    const u32 prefix = pad_field + spec;
    if (id == 0U)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    if (length < prefix)
    {
        fail(kH2FrameSizeError);
        return Step::Wait;
    }
    if (view.size() - kHttp2FrameHeaderBytes < prefix)
    {
        return Step::Wait;
    }
    const u32 pad = (pad_field != 0U) ? view[kHttp2FrameHeaderBytes] : 0U;
    if (pad > length - prefix)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    const u32 content = length - prefix - pad;
    if (content > m_block_bytes)
    {
        fail(kH2EnhanceYourCalm);
        return Step::Wait;
    }
    if (content == 0U && (flags & kFlagEndHeaders) == 0U && !charge(m_control_gate, m_config.control))
    {
        return Step::Wait;
    }
    m_self_dependent =
        spec != 0U && (cont::wire_load<u32>(view, kHttp2FrameHeaderBytes + pad_field) & kHttp2MaxStreamId) == id;
    m_block_id = id;
    m_block_used = 0U;
    m_block_end = (flags & kFlagEndStream) != 0U;
    m_frame_is_block = true;
    m_left = content;
    m_pad_left = pad;
    m_stage = Stage::Fragment;
    used = kHttp2FrameHeaderBytes + prefix;
    return Step::Again;
}

Http2Connection::Step Http2Connection::read_continuation(u32 length, u8 flags, usize& used) noexcept
{
    if (!m_block_open)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    if (length > m_block_bytes - m_block_used)
    {
        fail(kH2EnhanceYourCalm);
        return Step::Wait;
    }
    if (length == 0U && (flags & kFlagEndHeaders) == 0U && !charge(m_control_gate, m_config.control))
    {
        return Step::Wait;
    }
    m_frame_is_block = true;
    m_left = length;
    m_pad_left = 0U;
    m_stage = Stage::Fragment;
    used = kHttp2FrameHeaderBytes;
    return Step::Again;
}

Http2Connection::Step Http2Connection::read_priority(cont::ConstSpan<u8> payload, u32 id, Http2Event& event) noexcept
{
    if (id == 0U)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    if (!charge(m_control_gate, m_config.control))
    {
        return Step::Wait;
    }
    const bool sized = payload.size() == kPrioritySpecBytes;
    if (sized && (cont::wire_load<u32>(payload, 0U) & kHttp2MaxStreamId) != id)
    {
        return Step::Again; // RFC 9218 replaced the signal: read and ignored
    }
    const u32 code = sized ? kH2ProtocolError : kH2FrameSizeError;
    if (idle(id))
    {
        fail(code); // a stream error cannot be sent on an idle stream (section 6.4)
        return Step::Wait;
    }
    Record* const record = find(id);
    return (record != nullptr) ? stream_error(*record, code, event) : Step::Again;
}

Http2Connection::Step Http2Connection::read_rst_stream(cont::ConstSpan<u8> payload, u32 id, Http2Event& event) noexcept
{
    if (payload.size() != kRstStreamBytes)
    {
        fail(kH2FrameSizeError);
        return Step::Wait;
    }
    if (id == 0U || idle(id))
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    Record* const record = find(id);
    if (record == nullptr)
    {
        return Step::Again; // it crossed this side's closing the stream
    }
    if (!charge(m_reset_gate, m_config.resets))
    {
        return Step::Wait;
    }
    event = Http2Event{};
    event.kind = Http2EventKind::Aborted;
    event.stream = id;
    event.error = cont::wire_load<u32>(payload, 0U);
    release(*record, false);
    return Step::Event;
}

Http2Connection::Step Http2Connection::read_settings(cont::ConstSpan<u8> payload, u8 flags, u32 id,
                                                     Http2Event& event) noexcept
{
    if (id != 0U)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    if ((flags & kFlagAck) != 0U)
    {
        if (!payload.empty())
        {
            fail(kH2FrameSizeError);
            return Step::Wait;
        }
        if (m_settings_acked)
        {
            fail(kH2ProtocolError); // this side sends one SETTINGS
            return Step::Wait;
        }
        settings_acknowledged();
        return Step::Again;
    }
    if ((payload.size() % kSettingBytes) != 0U)
    {
        fail(kH2FrameSizeError);
        return Step::Wait;
    }
    if (!m_need_settings && !charge(m_control_gate, m_config.control))
    {
        return Step::Wait;
    }
    m_need_settings = false;
    if (!apply_settings(payload))
    {
        return Step::Wait;
    }
    put_frame(kFrameSettings, kFlagAck, 0U, {});
    m_peer_known = true;
    event = Http2Event{};
    event.kind = Http2EventKind::Settings;
    return Step::Event;
}

bool Http2Connection::apply_settings(cont::ConstSpan<u8> payload) noexcept
{
    u32 window = m_peer.initial_window;
    for (usize at = 0U; at < payload.size(); at += kSettingBytes)
    {
        const u16 setting = cont::wire_load<u16>(payload, at);
        const u32 value = cont::wire_load<u32>(payload, at + 2U);
        u32 refused = kH2NoError;
        switch (setting)
        {
            case kSettingHeaderTableSize:
                m_peer.header_table_size = value;
                m_encoder.set_peer_limit(value);
                break;
            case kSettingEnablePush:
                if (value > 1U || (value == 1U && m_config.side == Http2Side::Client))
                {
                    refused = kH2ProtocolError;
                }
                break;
            case kSettingMaxConcurrentStreams:
                m_peer.max_concurrent_streams = value;
                break;
            case kSettingInitialWindowSize:
                refused = (value > kHttp2MaxWindow) ? kH2FlowControlError : kH2NoError;
                window = value;
                break;
            case kSettingMaxFrameSize:
                refused = (value < kHttp2MinFrameBytes || value > kHttp2MaxFrameBytes) ? kH2ProtocolError : kH2NoError;
                m_peer.max_frame_bytes = value;
                break;
            case kSettingMaxHeaderListSize:
                m_peer.max_field_section_size = value;
                break;
            case kSettingEnableConnectProtocol:
                refused = (value > 1U || (value == 0U && m_peer.enable_connect_protocol)) ? kH2ProtocolError
                                                                                          : kH2NoError;
                m_peer.enable_connect_protocol = value == 1U;
                break;
            case kSettingNoRfc7540Priorities:
                refused = (value > 1U) ? kH2ProtocolError : kH2NoError;
                break;
            default: // section 6.5.2: ignored
                break;
        }
        if (refused != kH2NoError)
        {
            fail(refused);
            return false;
        }
    }
    const i64 delta = i64{window} - i64{m_peer.initial_window};
    m_peer.initial_window = window;
    for (u32 at = 0U; at < m_open && delta != 0; ++at)
    {
        Record& record = m_records[m_index[at].slot];
        record.send_window += delta;
        if (record.send_window > kHttp2MaxWindow)
        {
            fail(kH2FlowControlError); // section 6.9.2
            return false;
        }
    }
    if (delta > 0)
    {
        wake_writers();
    }
    return true;
}

void Http2Connection::settings_acknowledged() noexcept
{
    m_settings_acked = true;
    m_settings_deadline_ns = kNeverNs;
    m_decoder.set_limit(m_config.table_bytes);
    m_local_frame_bytes = m_config.frame_bytes;
    const i64 delta = i64{m_config.stream_window} - i64{m_local_window};
    m_local_window = m_config.stream_window;
    for (u32 at = 0U; at < m_open; ++at)
    {
        m_records[m_index[at].slot].recv_window += delta;
    }
}

Http2Connection::Step Http2Connection::read_ping(cont::ConstSpan<u8> payload, u8 flags, u32 id) noexcept
{
    if (id != 0U)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    if (payload.size() != kPingBytes)
    {
        fail(kH2FrameSizeError);
        return Step::Wait;
    }
    if ((flags & kFlagAck) != 0U)
    {
        return Step::Again;
    }
    if (!charge(m_control_gate, m_config.control))
    {
        return Step::Wait;
    }
    put_frame(kFramePing, kFlagAck, 0U, payload);
    return Step::Again;
}

Http2Connection::Step Http2Connection::read_goaway(cont::ConstSpan<u8> payload, u32 id, Http2Event& event) noexcept
{
    if (id != 0U)
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    if (payload.size() < kGoAwayMinBytes)
    {
        fail(kH2FrameSizeError);
        return Step::Wait;
    }
    const u32 last = cont::wire_load<u32>(payload, 0U) & kHttp2MaxStreamId;
    if (m_goaway_received && last > m_goaway_peer)
    {
        fail(kH2ProtocolError); // section 6.8: the value never rises
        return Step::Wait;
    }
    m_goaway_received = true;
    m_goaway_peer = last;
    m_sweep = m_config.side == Http2Side::Client;
    event = Http2Event{};
    event.kind = Http2EventKind::GoAway;
    event.stream = last;
    event.error = cont::wire_load<u32>(payload, 4U);
    return Step::Event;
}

Http2Connection::Step Http2Connection::read_window_update(cont::ConstSpan<u8> payload, u32 id,
                                                          Http2Event& event) noexcept
{
    if (payload.size() != kWindowUpdateBytes)
    {
        fail(kH2FrameSizeError);
        return Step::Wait;
    }
    const u32 increment = cont::wire_load<u32>(payload, 0U) & kHttp2MaxWindow;
    if (increment < kTinyWindowUpdate && !charge(m_control_gate, m_config.control))
    {
        return Step::Wait;
    }
    if (id == 0U)
    {
        if (increment == 0U || m_send_window + increment > kHttp2MaxWindow)
        {
            fail((increment == 0U) ? kH2ProtocolError : kH2FlowControlError);
            return Step::Wait;
        }
        m_send_window += increment;
        if (m_send_window > 0)
        {
            wake_writers();
        }
        return Step::Again;
    }
    if (idle(id))
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    Record* const record = find(id);
    if (record == nullptr)
    {
        return Step::Again; // it crossed this side's closing the stream
    }
    if (increment == 0U)
    {
        return stream_error(*record, kH2ProtocolError, event);
    }
    if (record->send_window + increment > kHttp2MaxWindow)
    {
        return stream_error(*record, kH2FlowControlError, event);
    }
    record->send_window += increment;
    if (record->send_window > 0)
    {
        wake_writers();
    }
    return Step::Again;
}

Http2Connection::Step Http2Connection::read_body(cont::ConstSpan<u8> view, usize& used, Http2Event& event) noexcept
{
    if (m_left == 0U)
    {
        m_stage = Stage::Padding;
        return Step::Again;
    }
    if (view.empty())
    {
        return Step::Wait;
    }
    const usize count = std::min<usize>(m_left, view.size());
    used = count;
    m_left -= static_cast<u32>(count);
    Record* const record = find(m_frame_id);
    if (record == nullptr)
    {
        credit_connection(static_cast<u32>(count)); // cancelled while the frame was arriving
        return Step::Again;
    }
    record->unconsumed += static_cast<u32>(count);
    event = Http2Event{};
    event.kind = Http2EventKind::Body;
    event.stream = m_frame_id;
    event.payload = view.first(count);
    return Step::Event;
}

Http2Connection::Step Http2Connection::frame_done(Http2Event& event) noexcept
{
    m_stage = Stage::FrameHeader;
    if (m_frame_is_block)
    {
        if ((m_frame_flags & kFlagEndHeaders) != 0U)
        {
            m_block_open = false;
            return section_done(event);
        }
        m_block_open = true;
        return Step::Again;
    }
    if ((m_frame_flags & kFlagEndStream) != 0U)
    {
        Record* const record = find(m_frame_id);
        if (record != nullptr)
        {
            return message_end(*record, event);
        }
    }
    return Step::Again;
}

Http2Connection::Step Http2Connection::section_done(Http2Event& event) noexcept
{
    usize count = 0U;
    const HpackStatus status = m_decoder.decode(cont::ConstSpan<u8>(m_block, m_block_used),
                                                cont::Span<HttpField>(m_fields, m_field_slots),
                                                cont::Span<u8>(m_text, m_config.field_bytes), count);
    if (status == HpackStatus::Failed)
    {
        fail(kH2CompressionError);
        return Step::Wait;
    }
    const bool too_large =
        status == HpackStatus::TooLarge ||
        http_section_size(cont::ConstSpan<HttpField>(m_fields, count)) > m_config.field_bytes;
    const u32 id = m_block_id;
    Record* const record = find(id);
    if (record != nullptr)
    {
        return m_self_dependent ? stream_error(*record, kH2ProtocolError, event)
                                : on_section(*record, count, too_large, event);
    }
    if (m_config.side == Http2Side::Server && (id & 1U) != 0U && id > m_highest)
    {
        return new_request(id, count, too_large, event);
    }
    if (idle(id))
    {
        fail(kH2ProtocolError);
        return Step::Wait;
    }
    const Closed kind = closed_kind(id);
    if (kind == Closed::Reset)
    {
        refuse_stream(id, kH2StreamClosed);
    }
    return (kind == Closed::Failed) ? Step::Wait : Step::Again;
}

Http2Connection::Step Http2Connection::new_request(u32 id, usize count, bool too_large, Http2Event& event) noexcept
{
    m_highest = id;
    if (m_goaway_was_sent && id > m_goaway_sent)
    {
        remember_closed(id, true); // section 6.8: what follows on it is dropped too
        return Step::Again;        // minimally processed, and that was the decoding
    }
    if (m_self_dependent || m_open >= m_config.streams)
    {
        remember_closed(id, true);
        refuse_stream(id, m_self_dependent ? kH2ProtocolError : kH2RefusedStream);
        return Step::Again;
    }
    if (too_large)
    {
        remember_closed(id, true);
        refuse_too_large(id);
        return Step::Again;
    }
    const cont::ConstSpan<HttpField> fields(m_fields, count);
    event = Http2Event{};
    if (!http_check_request(fields, event.info) || (event.info.extended && !m_config.connect_protocol) ||
        (m_block_end && event.info.has_content_length && event.info.content_length != 0U))
    {
        remember_closed(id, true);
        refuse_stream(id, kH2ProtocolError);
        return Step::Again;
    }
    Record& record = claim(id);
    record.read = Read::Body;
    record.connect = event.info.connect;
    record.has_length = event.info.has_content_length;
    record.content_left = event.info.content_length;
    if (m_block_end)
    {
        m_owed_id = id;
    }
    event.kind = Http2EventKind::Headers;
    event.stream = id;
    event.fields = fields;
    return Step::Event;
}

Http2Connection::Step Http2Connection::on_section(Record& record, usize count, bool too_large,
                                                  Http2Event& event) noexcept
{
    if (record.read == Read::Over)
    {
        return stream_error(record, kH2StreamClosed, event);
    }
    if (too_large)
    {
        return stream_error(record, kH2EnhanceYourCalm, event);
    }
    const cont::ConstSpan<HttpField> fields(m_fields, count);
    return (record.read == Read::Head) ? on_response(record, fields, event) : on_trailers(record, fields, event);
}

Http2Connection::Step Http2Connection::on_response(Record& record, cont::ConstSpan<HttpField> fields,
                                                   Http2Event& event) noexcept
{
    HttpMessageInfo info;
    if (!http_check_response(fields, info) || (info.status < 200U && m_block_end))
    {
        return stream_error(record, kH2ProtocolError, event);
    }
    Http2EventKind kind = Http2EventKind::Interim;
    if (info.status >= 200U)
    {
        record.tunnel = record.connect && info.status < 300U;
        const bool no_content = record.head || info.status == 204U || info.status == 304U;
        const bool has_length = no_content || (info.has_content_length && !record.tunnel);
        if (m_block_end && has_length && !no_content && info.content_length != 0U)
        {
            return stream_error(record, kH2ProtocolError, event);
        }
        record.read = Read::Body;
        record.has_length = has_length;
        record.content_left = no_content ? 0U : info.content_length;
        if (m_block_end)
        {
            m_owed_id = record.id;
        }
        kind = Http2EventKind::Headers;
    }
    event = Http2Event{};
    event.kind = kind;
    event.stream = record.id;
    event.fields = fields;
    event.info = info;
    return Step::Event;
}

Http2Connection::Step Http2Connection::on_trailers(Record& record, cont::ConstSpan<HttpField> fields,
                                                   Http2Event& event) noexcept
{
    if (!m_block_end || record.tunnel || !http_check_trailers(fields) ||
        (record.has_length && record.content_left != 0U))
    {
        return stream_error(record, kH2ProtocolError, event);
    }
    m_owed_id = record.id;
    event = Http2Event{};
    event.kind = Http2EventKind::Trailers;
    event.stream = record.id;
    event.fields = fields;
    return Step::Event;
}

Http2Connection::Step Http2Connection::message_end(Record& record, Http2Event& event) noexcept
{
    if (record.has_length && record.content_left != 0U)
    {
        return stream_error(record, kH2ProtocolError, event);
    }
    event = Http2Event{};
    event.kind = Http2EventKind::Finished;
    event.stream = record.id;
    record.read = Read::Over;
    settle(record);
    return Step::Event;
}

Http2Connection::Step Http2Connection::stream_error(Record& record, u32 code, Http2Event& event) noexcept
{
    if (!charge(m_reset_gate, m_config.resets))
    {
        return Step::Wait;
    }
    const u32 id = record.id;
    put_rst_stream(id, code);
    release(record, true);
    event = Http2Event{};
    event.kind = Http2EventKind::Aborted;
    event.stream = id;
    event.error = code;
    return Step::Event;
}

void Http2Connection::refuse_stream(u32 id, u32 code) noexcept
{
    if (charge(m_reset_gate, m_config.resets))
    {
        put_rst_stream(id, code);
    }
}

void Http2Connection::refuse_too_large(u32 id) noexcept
{
    if (!charge(m_reset_gate, m_config.resets))
    {
        return;
    }
    static constexpr char kStatus[] = ":status";
    static constexpr char kTooLarge[] = "431";
    const HttpField status{http_text(kStatus), http_text(kTooLarge)};
    if (write_section(id, cont::ConstSpan<HttpField>(&status, 1U), true))
    {
        if (!m_block_end)
        {
            put_rst_stream(id, kH2NoError); // section 8.1: the rest of the request is not wanted
        }
    }
    else
    {
        put_rst_stream(id, kH2RefusedStream);
    }
}

void Http2Connection::settle(Record& record) noexcept
{
    if (record.read == Read::Over && record.send == Send::Over)
    {
        release(record, false);
    }
}

void Http2Connection::wake_writers() noexcept
{
    if (m_wants != 0U || m_want_open)
    {
        m_wake = true;
        m_wake_at = 0U;
    }
}

// --- Flow control --------------------------------------------------------------------------------------------------

void Http2Connection::credit_connection(u32 bytes) noexcept
{
    m_conn_credit += bytes;
    flush_credit(nullptr);
}

void Http2Connection::credit_stream(Record& record, u32 bytes) noexcept
{
    record.credit += bytes;
    flush_credit(&record);
}

void Http2Connection::flush_credit(Record* record) noexcept
{
    u8 payload[kWindowUpdateBytes] = {};
    if (record == nullptr)
    {
        if (m_conn_credit == 0U || m_conn_credit < (m_config.connection_window / 2U))
        {
            return;
        }
        if (output_room() < kControlRoom)
        {
            m_credit_owed = true;
            return;
        }
        cont::wire_store<u32>(payload, 0U, m_conn_credit);
        put_frame(kFrameWindowUpdate, 0U, 0U, payload);
        m_recv_window += m_conn_credit;
        m_conn_credit = 0U;
        return;
    }
    if (record->credit == 0U || record->credit < (m_local_window / 2U) || record->read == Read::Over)
    {
        return;
    }
    if (output_room() < kControlRoom)
    {
        m_credit_owed = true;
        return;
    }
    cont::wire_store<u32>(payload, 0U, record->credit);
    put_frame(kFrameWindowUpdate, 0U, record->id, payload);
    record->recv_window += record->credit;
    record->credit = 0U;
}

Http2Error Http2Connection::consume_body(u32 id, usize count) noexcept
{
    if (m_error != 0U)
    {
        return Http2Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr)
    {
        return Http2Error::Unknown;
    }
    if (count > record->unconsumed)
    {
        return Http2Error::Invalid;
    }
    record->unconsumed -= static_cast<u32>(count);
    credit_connection(static_cast<u32>(count));
    credit_stream(*record, static_cast<u32>(count));
    return Http2Error::None;
}

// --- Writing -------------------------------------------------------------------------------------------------------

u8* Http2Connection::output_tail(usize bytes) noexcept
{
    if (bytes > m_out_bytes - m_out_tail)
    {
        std::memmove(m_out, m_out + m_out_head, m_out_tail - m_out_head);
        m_out_tail -= m_out_head;
        m_out_head = 0U;
    }
    u8* const at = m_out + m_out_tail;
    m_out_tail += bytes;
    return at;
}

void Http2Connection::output_taken(usize count) noexcept
{
    m_out_head += std::min(count, m_out_tail - m_out_head);
    if (m_out_head == m_out_tail)
    {
        m_out_head = 0U;
        m_out_tail = 0U;
    }
    if (output_room() >= (m_out_bytes / 2U))
    {
        wake_writers();
    }
}

void Http2Connection::put_frame(u8 type, u8 flags, u32 stream, cont::ConstSpan<u8> payload) noexcept
{
    u8* const at = output_tail(kHttp2FrameHeaderBytes + payload.size());
    put_header(at, payload.size(), type, flags, stream);
    if (!payload.empty())
    {
        std::memcpy(at + kHttp2FrameHeaderBytes, payload.data(), payload.size());
    }
}

void Http2Connection::put_rst_stream(u32 id, u32 code) noexcept
{
    u8 payload[kRstStreamBytes] = {};
    cont::wire_store<u32>(payload, 0U, code);
    put_frame(kFrameRstStream, 0U, id, payload);
}

void Http2Connection::put_goaway(u32 last, u32 code) noexcept
{
    u8 payload[kGoAwayMinBytes] = {};
    cont::wire_store<u32>(payload, 0U, last);
    cont::wire_store<u32>(payload, 4U, code);
    put_frame(kFrameGoAway, 0U, 0U, payload);
    m_goaway_was_sent = true;
    m_goaway_sent = last;
}

u32 Http2Connection::frame_limit() const noexcept
{
    return std::min(m_peer.max_frame_bytes, m_config.frame_bytes);
}

void Http2Connection::write_settings() noexcept
{
    u8 payload[kMaxOwnSettings * kSettingBytes] = {};
    usize used = 0U;
    const auto put = [&payload, &used](u16 setting, u32 value)
    {
        cont::wire_store<u16>(payload, used, setting);
        cont::wire_store<u32>(payload, used + 2U, value);
        used += kSettingBytes;
    };
    if (m_config.table_bytes != kHpackDefaultTableSize)
    {
        put(kSettingHeaderTableSize, m_config.table_bytes);
    }
    if (m_config.side == Http2Side::Client)
    {
        put(kSettingEnablePush, 0U);
    }
    put(kSettingMaxConcurrentStreams, m_config.streams);
    if (m_config.stream_window != kHttp2InitialWindow)
    {
        put(kSettingInitialWindowSize, m_config.stream_window);
    }
    if (m_config.frame_bytes != kHttp2MinFrameBytes)
    {
        put(kSettingMaxFrameSize, m_config.frame_bytes);
    }
    put(kSettingMaxHeaderListSize, m_config.field_bytes);
    if (m_config.connect_protocol)
    {
        put(kSettingEnableConnectProtocol, 1U);
    }
    put(kSettingNoRfc7540Priorities, 1U);
    put_frame(kFrameSettings, 0U, 0U, cont::ConstSpan<u8>(payload, used));
}

bool Http2Connection::write_section(u32 id, cont::ConstSpan<HttpField> fields, bool fin) noexcept
{
    const usize most = HpackEncoder::max_bytes(fields);
    const u32 limit = frame_limit();
    const usize total = most + (usize{frames_for(most, limit)} * kHttp2FrameHeaderBytes);
    if (total + kControlRoom > output_room())
    {
        return false;
    }
    u8* const at = output_tail(total);
    const usize block = m_encoder.encode(fields, cont::Span<u8>(at + kHttp2FrameHeaderBytes, most));
    const u32 frames = frames_for(block, limit);
    // The block was written whole behind the first header: the later pieces move up from the last to the first.
    for (u32 frame = frames - 1U; frame != 0U; --frame)
    {
        const usize start = usize{frame} * limit;
        const usize piece = std::min<usize>(block - start, limit);
        u8* const header = at + (usize{frame} * (usize{limit} + kHttp2FrameHeaderBytes));
        std::memmove(header + kHttp2FrameHeaderBytes, at + kHttp2FrameHeaderBytes + start, piece);
        put_header(header, piece, kFrameContinuation, (frame == frames - 1U) ? kFlagEndHeaders : u8{0U}, id);
    }
    const u8 flags = static_cast<u8>((fin ? kFlagEndStream : 0U) | ((frames == 1U) ? kFlagEndHeaders : 0U));
    put_header(at, std::min<usize>(block, limit), kFrameHeaders, flags, id);
    m_out_tail -= total - (block + (usize{frames} * kHttp2FrameHeaderBytes));
    return true;
}

Http2Error Http2Connection::send_section(Record& record, cont::ConstSpan<HttpField> fields, bool fin) noexcept
{
    const Http2Error checked = section_room(fields);
    if (checked != Http2Error::None)
    {
        return checked;
    }
    if (!write_section(record.id, fields, fin))
    {
        block(record);
        return Http2Error::Blocked;
    }
    return Http2Error::None;
}

Http2Error Http2Connection::section_room(cont::ConstSpan<HttpField> fields) const noexcept
{
    const u64 size = http_section_size(fields);
    const usize most = HpackEncoder::max_bytes(fields);
    const usize total = most + (usize{frames_for(most, frame_limit())} * kHttp2FrameHeaderBytes);
    if (size > m_config.field_bytes || size > m_peer.max_field_section_size || total + kControlRoom > m_out_bytes)
    {
        return Http2Error::TooLarge;
    }
    return Http2Error::None;
}

void Http2Connection::block(Record& record) noexcept
{
    if (!record.want_write)
    {
        record.want_write = true;
        ++m_wants;
    }
}

Http2Error Http2Connection::open_request(cont::ConstSpan<HttpField> fields, bool fin, u32& id) noexcept
{
    id = 0U;
    if (m_error != 0U)
    {
        return Http2Error::Closed;
    }
    HttpMessageInfo info;
    if (m_config.side != Http2Side::Client || !http_check_request(fields, info) ||
        (fin && info.has_content_length && info.content_length != 0U))
    {
        return Http2Error::Invalid;
    }
    if (m_goaway_received)
    {
        return Http2Error::GoingAway;
    }
    if (m_open >= open_limit() || m_next_local > kHttp2MaxStreamId)
    {
        return Http2Error::NoStream;
    }
    if (info.extended && !m_peer.enable_connect_protocol)
    {
        return Http2Error::NotReady;
    }
    const Http2Error checked = section_room(fields);
    if (checked != Http2Error::None)
    {
        return checked;
    }
    if (!write_section(m_next_local, fields, fin))
    {
        m_want_open = true;
        return Http2Error::Blocked;
    }
    Record& record = claim(m_next_local);
    id = m_next_local;
    m_next_local += 2U;
    record.head = info.head;
    record.connect = info.connect;
    record.send = fin ? Send::Over : Send::Message;
    return Http2Error::None;
}

Http2Error Http2Connection::send_response(u32 id, cont::ConstSpan<HttpField> fields, bool fin) noexcept
{
    if (m_error != 0U)
    {
        return Http2Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->send == Send::Over)
    {
        return Http2Error::Unknown;
    }
    HttpMessageInfo info;
    if (m_config.side != Http2Side::Server || record->send != Send::Idle || !http_check_response(fields, info) ||
        (info.status < 200U && fin))
    {
        return Http2Error::Invalid;
    }
    const Http2Error sent = send_section(*record, fields, fin);
    if (sent == Http2Error::None && info.status >= 200U)
    {
        record->send = fin ? Send::Over : Send::Message;
        record->tunnel = record->connect && info.status < 300U;
        settle(*record);
    }
    return sent;
}

Http2Error Http2Connection::send_body(u32 id, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept
{
    taken = 0U;
    if (m_error != 0U)
    {
        return Http2Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->send == Send::Over)
    {
        return Http2Error::Unknown;
    }
    if (record->send != Send::Message || (data.empty() && !fin))
    {
        return Http2Error::Invalid;
    }
    const usize room = output_room();
    if (room < kControlRoom + kHttp2FrameHeaderBytes + (data.empty() ? 0U : 1U))
    {
        block(*record);
        return Http2Error::Blocked;
    }
    const i64 credit = std::min(record->send_window, m_send_window);
    const usize allowed = (credit > 0) ? static_cast<usize>(credit) : 0U;
    const usize count =
        std::min({data.size(), allowed, usize{frame_limit()}, room - kControlRoom - kHttp2FrameHeaderBytes});
    if (count == 0U && !data.empty())
    {
        block(*record);
        return Http2Error::Blocked;
    }
    const bool ends = fin && count == data.size();
    put_frame(kFrameData, ends ? kFlagEndStream : u8{0U}, id, data.first(count));
    record->send_window -= static_cast<i64>(count);
    m_send_window -= static_cast<i64>(count);
    taken = count;
    if (ends)
    {
        record->send = Send::Over;
        settle(*record);
    }
    return Http2Error::None;
}

Http2Error Http2Connection::send_trailers(u32 id, cont::ConstSpan<HttpField> fields) noexcept
{
    if (m_error != 0U)
    {
        return Http2Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr || record->send == Send::Over)
    {
        return Http2Error::Unknown;
    }
    if (record->send != Send::Message || record->tunnel || !http_check_trailers(fields))
    {
        return Http2Error::Invalid;
    }
    const Http2Error sent = send_section(*record, fields, true);
    if (sent == Http2Error::None)
    {
        record->send = Send::Over;
        settle(*record);
    }
    return sent;
}

Http2Error Http2Connection::finish(u32 id) noexcept
{
    usize taken = 0U;
    return send_body(id, {}, true, taken);
}

Http2Error Http2Connection::cancel(u32 id, u32 error) noexcept
{
    if (m_error != 0U)
    {
        return Http2Error::Closed;
    }
    Record* const record = find(id);
    if (record == nullptr)
    {
        return Http2Error::Unknown;
    }
    if (output_room() < kControlRoom)
    {
        block(*record);
        return Http2Error::Blocked;
    }
    put_rst_stream(id, error);
    release(*record, true);
    return Http2Error::None;
}

bool Http2Connection::goaway(bool immediate) noexcept
{
    if (m_error != 0U || output_room() < kControlRoom)
    {
        return false;
    }
    u32 last = 0U;
    if (m_config.side == Http2Side::Server)
    {
        last = immediate ? m_highest : kHttp2MaxStreamId;
    }
    if (!m_goaway_was_sent || last < m_goaway_sent)
    {
        put_goaway(last, kH2NoError);
    }
    return true;
}

} // namespace crd::net
