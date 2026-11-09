// crd-net -- QMux over TLS (4.2a; docs/design/network-foundation.md#qmux). Source: draft-ietf-quic-qmux-02.

#include <crd/containers/wire_bytes.hpp>
#include <crd/net/frame.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/register_ids.hpp>

#include <cstring>

namespace crd::net
{
namespace
{

// The input one TlsSession::receive takes: its plaintext is read out before the next, so its bound is never reached.
constexpr usize kFeedBytes = 16384U;
// What a record costs in TLS: a record header, a tag and a content type for each of up to two TLS records.
constexpr usize kTlsRecordOverhead = 64U;
constexpr u64 kMsNs = 1000000U;
constexpr u32 kRttWeight = 8U; // RFC 9002 section 5.3's gain, one eighth

// QUIC's transport parameters QMux forbids (section 5.1): all that speak of connection IDs, paths or acknowledgements.
[[nodiscard]] bool forbidden_parameter(u64 id) noexcept
{
    switch (id)
    {
        case 0x00U: // original_destination_connection_id
        case 0x02U: // stateless_reset_token
        case 0x03U: // max_udp_payload_size
        case 0x0AU: // ack_delay_exponent
        case 0x0BU: // max_ack_delay
        case 0x0CU: // disable_active_migration
        case 0x0DU: // preferred_address
        case 0x0EU: // active_connection_id_limit
        case 0x0FU: // initial_source_connection_id
        case 0x10U: // retry_source_connection_id
            return true;
        default:
            return false;
    }
}

// The bit of a parameter's "seen" mask, or 64 for one this side ignores.
[[nodiscard]] u32 parameter_bit(u64 id) noexcept
{
    switch (id)
    {
        case 0x01U:
        case 0x04U:
        case 0x05U:
        case 0x06U:
        case 0x07U:
        case 0x08U:
        case 0x09U:
            return static_cast<u32>(id);
        case iana::kQuicTpMaxDatagramFrameSize:
            return 16U;
        case iana::kQuicTpMaxRecordSize:
            return 17U;
        default:
            return 64U;
    }
}

[[nodiscard]] QuicTransportError read_parameters(cont::ConstSpan<u8> bytes, QuicTransportParameters& out) noexcept
{
    cont::WireReader in(bytes);
    QuicTransportParameters parameters;
    u64 seen = 0U;
    while (!in.done())
    {
        u64 id = 0U;
        u64 length = 0U;
        cont::ConstSpan<u8> value;
        if (!take_quic_varint(in, id) || !take_quic_varint(in, length) || length > in.rest().size() ||
            !in.take_bytes(static_cast<usize>(length), value))
        {
            return QuicTransportError::TransportParameterError;
        }
        if (forbidden_parameter(id))
        {
            return QuicTransportError::TransportParameterError;
        }
        const u32 bit = parameter_bit(id);
        if (bit == 64U)
        {
            continue; // section 5.1: another document's parameter, not specified for QMux
        }
        cont::WireReader field(value);
        u64 number = 0U;
        if ((seen & (u64{1} << bit)) != 0U || !take_quic_varint(field, number) || !field.done())
        {
            return QuicTransportError::TransportParameterError;
        }
        seen |= u64{1} << bit;
        switch (id)
        {
            case 0x01U:
                parameters.max_idle_timeout = number;
                break;
            case 0x04U:
                parameters.initial_max_data = number;
                break;
            case 0x05U:
                parameters.initial_max_stream_data_bidi_local = number;
                break;
            case 0x06U:
                parameters.initial_max_stream_data_bidi_remote = number;
                break;
            case 0x07U:
                parameters.initial_max_stream_data_uni = number;
                break;
            case 0x08U:
                parameters.initial_max_streams_bidi = number;
                break;
            case 0x09U:
                parameters.initial_max_streams_uni = number;
                break;
            case iana::kQuicTpMaxDatagramFrameSize:
                parameters.max_datagram_frame_size = number;
                break;
            default:
                break; // max_record_size: this side never sends more than the default
        }
        if (((id == 0x08U || id == 0x09U) && number > kQuicMaxStreams) ||
            (id == iana::kQuicTpMaxRecordSize && number < kQmuxMaxRecord))
        {
            return QuicTransportError::TransportParameterError;
        }
    }
    out = parameters;
    return QuicTransportError::NoError;
}

void put_parameter(cont::WireWriter& out, u64 id, u64 value) noexcept
{
    put_quic_varint(out, id);
    put_quic_varint(out, quic_varint_bytes(value));
    put_quic_varint(out, value);
}

[[nodiscard]] usize parameter_bytes(u64 id, u64 value) noexcept
{
    return quic_varint_bytes(id) + quic_varint_bytes(quic_varint_bytes(value)) + quic_varint_bytes(value);
}

[[nodiscard]] u64 min_nonzero(u64 a, u64 b) noexcept
{
    if (a == 0U)
    {
        return b;
    }
    if (b == 0U)
    {
        return a;
    }
    return (a < b) ? a : b;
}

} // namespace

bool QmuxConnection::init(memory::IAllocator& allocator, const QmuxConfig& config) noexcept
{
    if (m_ready || config.tls == nullptr || !QuicStreamSet::limits_valid(config.streams) ||
        !QuicDatagrams::limits_valid(config.datagrams))
    {
        return false;
    }
    const crypto::QuicSide side =
        (config.tls->role() == crypto::TlsRole::Client) ? crypto::QuicSide::Client : crypto::QuicSide::Server;
    QuicStreamLimits streams = config.streams;
    streams.in_order = true;
    if (!m_streams.init(allocator, streams, side) || !m_datagrams.init(allocator, config.datagrams))
    {
        return false;
    }
    m_config = config;
    m_ready = true;
    return true;
}

bool QmuxConnection::start(u64 now_ns) noexcept
{
    if (!m_ready || m_state != QmuxState::Idle)
    {
        return false;
    }
    m_tls_error = m_tls.init(*m_config.tls, m_config.tickets);
    if (m_tls_error != crypto::TlsError::None)
    {
        m_close.reason = QmuxCloseReason::TlsFailed;
        m_state = QmuxState::Closed;
        return false;
    }
    m_state = QmuxState::Handshaking;
    m_last_activity = now_ns;
    m_handshake_deadline =
        (m_config.handshake_timeout_ns != 0U) ? now_ns + m_config.handshake_timeout_ns : kQmuxNoTimer;
    return true;
}

// ---- Receiving ----

void QmuxConnection::receive(cont::ConstSpan<u8> bytes, u64 now_ns) noexcept
{
    // A side that has closed has nothing to read: TLS is shut, and section 7.2 discards what still arrives.
    usize at = 0U;
    while (at < bytes.size() && (m_state == QmuxState::Handshaking || m_state == QmuxState::Established))
    {
        const usize chunk = (bytes.size() - at < kFeedBytes) ? bytes.size() - at : kFeedBytes;
        const crypto::TlsError error = m_tls.receive(bytes.subspan(at, chunk));
        if (error == crypto::TlsError::Closed)
        {
            peer_shutdown();
            return;
        }
        if (error != crypto::TlsError::None)
        {
            tls_failed(error);
            return;
        }
        at += chunk;
        advance(now_ns);
    }
}

void QmuxConnection::transport_ended(bool clean, u64 now_ns) noexcept
{
    static_cast<void>(now_ns);
    m_transport_over = true;
    if (m_state == QmuxState::Idle || m_state == QmuxState::Closed)
    {
        return;
    }
    if (m_state != QmuxState::Closing)
    {
        m_close.reason = clean ? QmuxCloseReason::Truncated : QmuxCloseReason::Reset;
    }
    m_state = QmuxState::Closed;
}

void QmuxConnection::advance(u64 now_ns) noexcept
{
    if (m_state == QmuxState::Handshaking && m_tls.state() == crypto::TlsState::Established)
    {
        established(now_ns);
    }
    if (m_state == QmuxState::Established)
    {
        drain(now_ns);
    }
}

void QmuxConnection::established(u64 now_ns) noexcept
{
    if (m_tls.application_protocol().empty())
    {
        // Section 3.1: without ALPN nothing tells which protocol the frames carry.
        m_tls_error = crypto::TlsError::PeerRefused;
        m_close.reason = QmuxCloseReason::TlsFailed;
        finish_sending();
        m_state = QmuxState::Closed;
        return;
    }
    m_state = QmuxState::Established;
    m_handshake_deadline = kQmuxNoTimer;
    m_last_activity = now_ns;
    m_idle_ns = m_config.idle_timeout_ns;
    m_ping_owed = true; // the first RTT sample, which tunes the windows
}

void QmuxConnection::drain(u64 now_ns) noexcept
{
    for (;;)
    {
        usize got = 0U;
        const crypto::TlsError error = m_tls.read(cont::Span<u8>(m_in + m_in_fill, sizeof(m_in) - m_in_fill), got);
        m_in_fill += got;
        parse_records(now_ns);
        if (m_state != QmuxState::Established)
        {
            return;
        }
        if (error == crypto::TlsError::Closed)
        {
            peer_shutdown();
            return;
        }
        if (error != crypto::TlsError::None)
        {
            tls_failed(error);
            return;
        }
        if (got == 0U)
        {
            return;
        }
    }
}

void QmuxConnection::parse_records(u64 now_ns) noexcept
{
    usize at = 0U;
    while (at < m_in_fill)
    {
        cont::WireReader header(cont::ConstSpan<u8>(m_in + at, m_in_fill - at));
        u64 size = 0U;
        if (!take_quic_varint(header, size))
        {
            break; // a Size still arriving
        }
        const usize head = header.consumed();
        if (size > kQmuxMaxRecord)
        {
            m_frame_type = 0U;
            fail(QuicTransportError::FrameEncodingError); // section 5.2
            return;
        }
        if (size == 0U)
        {
            // Section 3.2 has a record hold frames; RFC 9000 section 12.4 refuses a packet with none.
            m_frame_type = 0U;
            fail(QuicTransportError::ProtocolViolation);
            return;
        }
        if (m_in_fill - at - head < size)
        {
            break;
        }
        const bool good = on_record(cont::ConstSpan<u8>(m_in + at + head, static_cast<usize>(size)), now_ns);
        if (!good || m_state != QmuxState::Established)
        {
            m_in_fill = 0U;
            return;
        }
        ++m_counters.records_received;
        m_last_activity = now_ns; // section 7.1: a whole record received
        at += head + static_cast<usize>(size);
    }
    if (at != 0U)
    {
        std::memmove(m_in, m_in + at, m_in_fill - at);
        m_in_fill -= at;
    }
}

bool QmuxConnection::on_record(cont::ConstSpan<u8> record, u64 now_ns) noexcept
{
    cont::WireReader in(record);
    while (!in.done())
    {
        cont::WireReader probe(in.rest());
        u64 type = 0U;
        if (!take_quic_varint(probe, type))
        {
            m_frame_type = 0U;
            fail(QuicTransportError::FrameEncodingError);
            return false;
        }
        m_frame_type = type;
        if (type == iana::kQuicFrameQxTransportParameters)
        {
            u64 length = 0U;
            cont::ConstSpan<u8> parameters;
            if (m_peer_known)
            {
                fail(QuicTransportError::ProtocolViolation); // section 5: once
                return false;
            }
            if (!take_quic_varint(probe, length) || length > probe.rest().size() ||
                !probe.take_bytes(static_cast<usize>(length), parameters))
            {
                fail(QuicTransportError::FrameEncodingError);
                return false;
            }
            if (!on_parameters(parameters))
            {
                return false;
            }
        }
        else if (!m_peer_known)
        {
            fail(QuicTransportError::ProtocolViolation); // section 5: the first frame
            return false;
        }
        else if (type == iana::kQuicFrameQxPingRequest || type == iana::kQuicFrameQxPingResponse)
        {
            u64 sequence = 0U;
            if (!take_quic_varint(probe, sequence))
            {
                fail(QuicTransportError::FrameEncodingError);
                return false;
            }
            if (!on_ping(type, sequence, now_ns))
            {
                return false;
            }
        }
        else
        {
            QuicFrame frame;
            const QuicTransportError error = read_quic_frame(in, frame);
            if (error != QuicTransportError::NoError)
            {
                fail(error);
                return false;
            }
            if (!on_frame(frame))
            {
                return false;
            }
            continue;
        }
        cont::ConstSpan<u8> skipped;
        static_cast<void>(in.take_bytes(probe.consumed(), skipped));
    }
    return true;
}

bool QmuxConnection::on_parameters(cont::ConstSpan<u8> bytes) noexcept
{
    const QuicTransportError error = read_parameters(bytes, m_peer);
    if (error != QuicTransportError::NoError)
    {
        fail(error);
        return false;
    }
    m_peer_known = true;
    m_idle_ns = min_nonzero(m_config.idle_timeout_ns, m_peer.max_idle_timeout * kMsNs);
    m_streams.set_peer_parameters(m_peer);
    m_datagrams.set_peer(m_peer.max_datagram_frame_size, kQmuxMaxRecord + kQuicShortPacketOverhead);
    return true;
}

bool QmuxConnection::on_ping(u64 type, u64 sequence, u64 now_ns) noexcept
{
    if (type == iana::kQuicFrameQxPingRequest)
    {
        if (m_peer_pinged && sequence <= m_pong_sequence)
        {
            fail(QuicTransportError::ProtocolViolation); // section 4.3: requests increase strictly
            return false;
        }
        m_peer_pinged = true;
        m_pong_sequence = sequence;
        m_pong_owed = true;
        return true;
    }
    if (sequence >= m_ping_next)
    {
        fail(QuicTransportError::ProtocolViolation); // a response to a request never sent
        return false;
    }
    ++m_counters.pongs_received;
    if (sequence + 1U == m_ping_next)
    {
        const u64 sample = (now_ns >= m_ping_sent_ns) ? now_ns - m_ping_sent_ns : 0U;
        m_rtt_ns = (m_rtt_ns == 0U) ? sample : m_rtt_ns - (m_rtt_ns / kRttWeight) + (sample / kRttWeight);
    }
    return true;
}

bool QmuxConnection::on_frame(const QuicFrame& frame) noexcept
{
    QuicTransportError error = QuicTransportError::NoError;
    switch (frame.kind)
    {
        case QuicFrameKind::Padding:
            break;
        case QuicFrameKind::Stream:
        case QuicFrameKind::ResetStream:
        case QuicFrameKind::StopSending:
        case QuicFrameKind::MaxData:
        case QuicFrameKind::MaxStreamData:
        case QuicFrameKind::MaxStreams:
        case QuicFrameKind::DataBlocked:
        case QuicFrameKind::StreamDataBlocked:
        case QuicFrameKind::StreamsBlocked:
            error = m_streams.on_frame(frame);
            break;
        case QuicFrameKind::Datagram:
            error = m_datagrams.on_frame(frame);
            break;
        case QuicFrameKind::ConnectionClose:
            m_close.reason = QmuxCloseReason::Peer;
            m_close.application = frame.application;
            m_close.error_code = frame.error_code;
            m_close.frame_type = frame.frame_type;
            keep_reason(frame.data);
            finish_sending(); // section 7.3: the sending side shuts down too, with no frame
            m_state = QmuxState::Closed;
            return false;
        default:
            error = QuicTransportError::FrameEncodingError; // section 4.2: not a frame of QMux
            break;
    }
    if (error != QuicTransportError::NoError)
    {
        fail(error);
        return false;
    }
    return true;
}

void QmuxConnection::fail(QuicTransportError error) noexcept
{
    m_close.reason = QmuxCloseReason::Error;
    m_close.application = false;
    m_close.error_code = static_cast<u64>(error);
    m_close.frame_type = m_frame_type;
    m_reason_bytes = 0U;
    m_close_owed = true;
    m_close_deadline = kQmuxNoTimer; // set when the close is written
    m_state = QmuxState::Closing;
}

void QmuxConnection::tls_failed(crypto::TlsError error) noexcept
{
    m_tls_error = error;
    m_close.reason = QmuxCloseReason::TlsFailed;
    m_send_shut = true; // the alert, if any, is what remains to send
    m_close_owed = false;
    m_state = QmuxState::Closed;
}

void QmuxConnection::peer_shutdown() noexcept
{
    if (m_state == QmuxState::Established || m_state == QmuxState::Handshaking)
    {
        m_close.reason = QmuxCloseReason::PeerShutdown;
    }
    finish_sending();
    m_state = QmuxState::Closed;
}

void QmuxConnection::finish_sending() noexcept
{
    if (!m_send_shut)
    {
        static_cast<void>(m_tls.close()); // close_notify; a session past that has nothing to add
        m_send_shut = true;
    }
    m_close_owed = false;
}

void QmuxConnection::keep_reason(cont::ConstSpan<u8> reason) noexcept
{
    m_reason_bytes = (reason.size() < sizeof(m_reason)) ? reason.size() : sizeof(m_reason);
    if (m_reason_bytes != 0U)
    {
        std::memcpy(m_reason, reason.data(), m_reason_bytes);
    }
}

// ---- Sending ----

bool QmuxConnection::frames_due(u64 now_ns) const noexcept
{
    return !m_parameters_sent || m_ping_owed || m_pong_owed ||
           (m_peer_known && (m_streams.has_frame() || m_datagrams.has_frame(now_ns)));
}

bool QmuxConnection::wants_write(u64 now_ns) const noexcept
{
    return !m_transport_over &&
           (m_tls.pending() != 0U || m_close_owed || (m_state == QmuxState::Established && frames_due(now_ns)));
}

usize QmuxConnection::take(cont::Span<u8> out, u64 now_ns) noexcept
{
    if (m_transport_over || m_state == QmuxState::Idle)
    {
        return 0U;
    }
    produce(now_ns);
    usize written = 0U;
    if (m_tls.take(out, written) != crypto::TlsError::None)
    {
        return 0U;
    }
    if (written != 0U && m_tls.pending() == 0U && m_state == QmuxState::Established)
    {
        m_last_activity = now_ns; // section 7.1: every record handed on; a peer that stops reading times out
    }
    return written;
}

cont::ConstSpan<u8> QmuxConnection::outgoing(u64 now_ns) noexcept
{
    if (m_transport_over || m_state == QmuxState::Idle)
    {
        return {};
    }
    produce(now_ns);
    return m_tls.outgoing();
}

bool QmuxConnection::send_finished() const noexcept
{
    return m_send_shut && m_tls.pending() == 0U;
}

void QmuxConnection::produce(u64 now_ns) noexcept
{
    while (m_state == QmuxState::Established || m_close_owed)
    {
        if (m_tls.pending() + kQmuxMaxRecord + 2U + kTlsRecordOverhead > crypto::kTlsMaxBufferedBytes)
        {
            return; // the transport is behind: the records wait, unassembled
        }
        const bool closing = m_close_owed;
        const usize used = closing ? build_close() : build_record(now_ns);
        if (used == 0U)
        {
            return;
        }
        // Size is in its shortest encoding, directly in front of the frames.
        const usize head = quic_varint_bytes(used);
        cont::WireWriter size(cont::Span<u8>(m_out + 2U - head, head));
        put_quic_varint(size, used);
        const crypto::TlsError error = m_tls.send(cont::ConstSpan<u8>(m_out + 2U - head, head + used));
        if (error != crypto::TlsError::None)
        {
            tls_failed(error);
            return;
        }
        ++m_counters.records_sent;
        if (closing)
        {
            m_close_owed = false;
            finish_sending();
            m_close_deadline = now_ns + m_config.close_wait_ns;
        }
    }
}

usize QmuxConnection::build_close() noexcept
{
    QuicFrame frame;
    frame.kind = QuicFrameKind::ConnectionClose;
    frame.application = m_close.application;
    frame.error_code = m_close.error_code;
    frame.frame_type = m_close.frame_type;
    frame.data = close_reason();
    usize used = 0U;
    if (!m_parameters_sent)
    {
        used = write_parameters(cont::Span<u8>(m_out + 2U, kQmuxMaxRecord));
        m_parameters_sent = true;
    }
    return used + write_quic_frame(frame, cont::Span<u8>(m_out + 2U + used, kQmuxMaxRecord - used));
}

usize QmuxConnection::write_parameters(cont::Span<u8> out) noexcept
{
    QuicTransportParameters local;
    m_streams.fill_parameters(local);
    m_datagrams.fill_parameters(local);
    local.max_idle_timeout = m_config.idle_timeout_ns / kMsNs;
    const struct
    {
        u64 id;
        u64 value;
    } table[] = {{0x01U, local.max_idle_timeout},
                 {0x04U, local.initial_max_data},
                 {0x05U, local.initial_max_stream_data_bidi_local},
                 {0x06U, local.initial_max_stream_data_bidi_remote},
                 {0x07U, local.initial_max_stream_data_uni},
                 {0x08U, local.initial_max_streams_bidi},
                 {0x09U, local.initial_max_streams_uni},
                 {iana::kQuicTpMaxDatagramFrameSize, local.max_datagram_frame_size}};
    usize body = 0U;
    for (const auto& parameter : table)
    {
        body += (parameter.value != 0U) ? parameter_bytes(parameter.id, parameter.value) : 0U;
    }
    const usize bytes = quic_varint_bytes(iana::kQuicFrameQxTransportParameters) + quic_varint_bytes(body) + body;
    cont::WireWriter writer(out.first(bytes));
    put_quic_varint(writer, iana::kQuicFrameQxTransportParameters);
    put_quic_varint(writer, body);
    for (const auto& parameter : table)
    {
        if (parameter.value != 0U)
        {
            put_parameter(writer, parameter.id, parameter.value);
        }
    }
    return bytes;
}

usize QmuxConnection::write_ping(u64 type, u64 sequence, cont::Span<u8> out) noexcept
{
    const usize bytes = quic_varint_bytes(type) + quic_varint_bytes(sequence);
    cont::WireWriter writer(out.first(bytes));
    put_quic_varint(writer, type);
    put_quic_varint(writer, sequence);
    return bytes;
}

usize QmuxConnection::build_record(u64 now_ns) noexcept
{
    u8* const frames = m_out + 2U;
    usize used = 0U;
    if (!m_parameters_sent)
    {
        used += write_parameters(cont::Span<u8>(frames, kQmuxMaxRecord));
        m_parameters_sent = true;
    }
    if (m_pong_owed)
    {
        used += write_ping(iana::kQuicFrameQxPingResponse, m_pong_sequence,
                           cont::Span<u8>(frames + used, kQmuxMaxRecord - used));
        m_pong_owed = false;
    }
    if (m_ping_owed)
    {
        used += write_ping(iana::kQuicFrameQxPingRequest, m_ping_next,
                           cont::Span<u8>(frames + used, kQmuxMaxRecord - used));
        ++m_ping_next;
        m_ping_sent_ns = now_ns;
        m_ping_owed = false;
        ++m_counters.pings_sent;
    }
    if (m_peer_known)
    {
        while (m_datagrams.has_frame(now_ns))
        {
            u64 id = 0U;
            const usize bytes =
                m_datagrams.write_next(cont::Span<u8>(frames + used, kQmuxMaxRecord - used), now_ns, id);
            if (bytes == 0U)
            {
                break; // the frame goes alone in the next record: a datagram never exceeds one
            }
            used += bytes;
            if (m_datagrams.outcomes())
            {
                m_datagrams.on_acked(id); // handed to the transport, which never loses it
            }
        }
        QuicFrame frame;
        QuicStreamRecord record;
        while (m_streams.next_frame(kQmuxMaxRecord - used, now_ns, m_rtt_ns, frame, record))
        {
            const usize bytes = write_quic_frame(frame, cont::Span<u8>(frames + used, kQmuxMaxRecord - used));
            if (bytes == 0U)
            {
                m_streams.on_lost(record); // owed again (a frame next_frame sized never fails to fit)
                break;
            }
            used += bytes;
            m_streams.on_acked(record); // section 4.1: acknowledged once the transport has it
        }
    }
    if (used != 0U)
    {
        const usize padded = padded_size(used, m_config.padding_block, kQmuxMaxRecord);
        std::memset(frames + used, 0, padded - used);
        m_counters.bytes_padded += padded - used;
        used = padded;
    }
    return used;
}

// ---- Time and closing ----

u64 QmuxConnection::idle_deadline() const noexcept
{
    return (m_idle_ns != 0U) ? m_last_activity + m_idle_ns : kQmuxNoTimer;
}

u64 QmuxConnection::next_timer_ns() const noexcept
{
    switch (m_state)
    {
        case QmuxState::Handshaking:
            return m_handshake_deadline;
        case QmuxState::Established:
        {
            const u64 idle = idle_deadline();
            const u64 keepalive =
                (m_config.keepalive_ns != 0U && !m_ping_owed) ? m_last_activity + m_config.keepalive_ns : kQmuxNoTimer;
            return (idle < keepalive) ? idle : keepalive;
        }
        case QmuxState::Closing:
            return m_close_deadline;
        case QmuxState::Idle:
        case QmuxState::Closed:
            break;
    }
    return kQmuxNoTimer;
}

void QmuxConnection::on_timeout(u64 now_ns) noexcept
{
    if (m_state == QmuxState::Handshaking && now_ns >= m_handshake_deadline)
    {
        m_close.reason = QmuxCloseReason::HandshakeTimeout;
        m_send_shut = true;
        m_state = QmuxState::Closed;
    }
    else if (m_state == QmuxState::Established && now_ns >= idle_deadline())
    {
        m_close.reason = QmuxCloseReason::IdleTimeout;
        finish_sending(); // section 7.1: without a frame
        m_state = QmuxState::Closed;
    }
    else if (m_state == QmuxState::Established && m_config.keepalive_ns != 0U && !m_ping_owed &&
             now_ns >= m_last_activity + m_config.keepalive_ns)
    {
        m_ping_owed = true;
    }
    else if (m_state == QmuxState::Closing && now_ns >= m_close_deadline)
    {
        m_state = QmuxState::Closed;
    }
}

void QmuxConnection::close(u64 application_error, cont::ConstSpan<u8> reason, u64 now_ns) noexcept
{
    static_cast<void>(now_ns);
    if (m_state == QmuxState::Handshaking)
    {
        m_close.reason = QmuxCloseReason::Local;
        m_close.application = true;
        m_close.error_code = application_error;
        m_send_shut = true;
        m_state = QmuxState::Closed;
        return;
    }
    if (m_state != QmuxState::Established)
    {
        return;
    }
    m_close.reason = QmuxCloseReason::Local;
    m_close.application = true;
    m_close.error_code = application_error;
    m_close.frame_type = 0U;
    keep_reason(reason);
    m_close_owed = true;
    m_state = QmuxState::Closing;
}

} // namespace crd::net
