// crd-net -- QMux over TLS (4.2a; docs/design/network-foundation.md#qmux). Source: draft-ietf-quic-qmux-02.

#include <crd/containers/wire_bytes.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/memory/construct.hpp>
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
constexpr usize kSizeBytes = 2U; // a record's Size at its longest, as this side writes them
constexpr usize kFrameAt = kWsMaxHeaderBytes; // where a record's frames start in m_out, its head before them
constexpr char kHttp11[] = "http/1.1";
// A tunnel's staging buffer: room for what produce() may add at once, past a message waiting whole.
constexpr usize kTunnelStageBytes = 2U * (kQmuxMaxRecord + kWsMaxHeaderBytes + kWsMaxControlBytes);
// What a client's QX_TRANSPORT_PARAMETERS frame takes at most; early data with less room than this sends nothing.
constexpr usize kParametersRoom = 128U;

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

QmuxConnection::~QmuxConnection()
{
    if (m_stage != nullptr)
    {
        memory::deallocate_array(*m_allocator, m_stage);
    }
}

bool QmuxConnection::is_client() const noexcept
{
    return tunneled() ? m_config.tunnel == QmuxTunnel::Client : m_config.tls->role() == crypto::TlsRole::Client;
}

bool QmuxConnection::init(memory::IAllocator& allocator, const QmuxConfig& config) noexcept
{
    const bool tunnel = config.tunnel != QmuxTunnel::None;
    const bool accepts_early =
        config.tls != nullptr && config.tls->role() == crypto::TlsRole::Server && config.tls->early_data();
    if (m_ready || (config.tls == nullptr) != tunnel ||
        (tunnel && (config.websocket == nullptr || config.tickets != nullptr)) ||
        !QuicStreamSet::limits_valid(config.streams) ||
        !QuicDatagrams::limits_valid(config.datagrams) ||
        (!config.early_context.empty() && (!accepts_early || config.early_context.size() > kQuicMaxEarlyContextBytes)))
    {
        return false;
    }
    const bool client = tunnel ? config.tunnel == QmuxTunnel::Client : config.tls->role() == crypto::TlsRole::Client;
    if (config.websocket != nullptr &&
        (!ws_config_valid(*config.websocket, client) || (!tunnel && client && config.tls->early_data())))
    {
        return false;
    }
    const crypto::QuicSide side = client ? crypto::QuicSide::Client : crypto::QuicSide::Server;
    QuicStreamLimits streams = config.streams;
    streams.in_order = true;
    if (!m_streams.init(allocator, streams, side) || !m_datagrams.init(allocator, config.datagrams))
    {
        return false;
    }
    if (tunnel)
    {
        m_stage = memory::try_allocate_array<u8>(allocator, kTunnelStageBytes);
        if (m_stage == nullptr)
        {
            return false;
        }
        m_allocator = &allocator;
    }
    m_config = config;
    m_ready = true;
    return true;
}

bool QmuxConnection::start(u64 now_ns) noexcept
{
    if (!m_ready || m_state != QmuxState::Idle || tunneled())
    {
        return false;
    }
    const bool client = is_client();
    u8 context[crypto::kTlsMaxEarlyDataContextBytes];
    usize context_bytes = 0U;
    if (!client && m_config.tls->early_data())
    {
        // Section 8.2 holds a server to RFC 9000 section 7.4.1: any limit that changed refuses a ticket's 0-RTT.
        QuicTransportParameters local;
        fill_local(local);
        context_bytes = write_quic_early_parameters(local, cont::Span<u8>(context, kQuicEarlyParameterBytes));
        if (!m_config.early_context.empty())
        {
            std::memcpy(context + context_bytes, m_config.early_context.data(), m_config.early_context.size());
            context_bytes += m_config.early_context.size();
        }
    }
    m_tls_error = m_tls.init(*m_config.tls, m_config.tickets, {}, cont::ConstSpan<u8>(context, context_bytes));
    if (m_tls_error != crypto::TlsError::None)
    {
        m_close.reason = QmuxCloseReason::TlsFailed;
        m_state = QmuxState::Closed;
        return false;
    }
    if (client && m_config.tls->early_data())
    {
        m_tls.hold_tickets(); // what 0-RTT keeps to arrives in the server's first frame, beside its tickets
    }
    m_state = QmuxState::Handshaking;
    m_last_activity = now_ns;
    m_handshake_deadline =
        (m_config.handshake_timeout_ns != 0U) ? now_ns + m_config.handshake_timeout_ns : kQmuxNoTimer;
    begin_early();
    return true;
}

bool QmuxConnection::start_tunnel(u8 protocol, u64 now_ns) noexcept
{
    if (!m_ready || m_state != QmuxState::Idle || !tunneled() || protocol >= m_config.websocket->protocols.size())
    {
        return false;
    }
    m_ws.protocol = protocol;
    m_ws.upgraded = true;
    m_framing = Framing::Messages;
    established(now_ns);
    return true;
}

void QmuxConnection::begin_early() noexcept
{
    if (m_tls.early_data() != crypto::TlsEarlyData::Offered || m_tls.early_data_room() < kParametersRoom ||
        m_tls.remembered_parameters().empty() ||
        read_parameters(m_tls.remembered_parameters(), m_remembered) != QuicTransportError::NoError)
    {
        return; // the frames wait for the handshake
    }
    apply_peer_limits(m_remembered);
    m_datagrams.hold_outcomes();
    m_early_sending = true;
}

void QmuxConnection::settle_early() noexcept
{
    if (!m_early_sending || m_early_settled)
    {
        return;
    }
    const crypto::TlsEarlyData early = m_tls.early_data();
    if (early == crypto::TlsEarlyData::Accepted)
    {
        m_early_settled = true;
        m_streams.on_all_acked();
        m_datagrams.accept_early();
    }
    else if (early == crypto::TlsEarlyData::Rejected)
    {
        // What went early is gone (draft section 8.2, RFC 9001 section 4.6.2): the first frame goes again, and the
        // server's parameters grant anew.
        m_early_settled = true;
        m_early_sending = false;
        m_early_full = false;
        m_parameters_sent = false;
        m_streams.reset_all();
        m_datagrams.reject_early();
    }
}

// ---- Receiving ----

void QmuxConnection::receive(cont::ConstSpan<u8> bytes, u64 now_ns) noexcept
{
    if (tunneled())
    {
        receive_tunnel(bytes, now_ns);
        return;
    }
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

void QmuxConnection::receive_tunnel(cont::ConstSpan<u8> bytes, u64 now_ns) noexcept
{
    usize at = 0U;
    while (at < bytes.size() && m_state == QmuxState::Established)
    {
        // parse_frames() leaves at most a partial frame and the message before it, which never fill m_in.
        const usize room = sizeof(m_in) - m_in_fill;
        const usize chunk = (bytes.size() - at < room) ? bytes.size() - at : room;
        std::memcpy(m_in + m_in_fill, bytes.data() + at, chunk);
        m_in_fill += chunk;
        at += chunk;
        parse_records(now_ns);
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
    settle_early();
    const bool done = m_tls.state() == crypto::TlsState::Established;
    const bool reading_early = !is_client() && m_tls.early_data() == crypto::TlsEarlyData::Accepted;
    if (m_state == QmuxState::Handshaking && (done || reading_early))
    {
        if (m_framing == Framing::Records && takes_websocket())
        {
            if (!done)
            {
                refuse_tls(crypto::TlsError::PeerRefused); // an opening handshake that could be replayed is none
                return;
            }
            begin_upgrade();
        }
        if (m_framing == Framing::Upgrading)
        {
            read_upgrade(now_ns);
        }
        else if (m_state == QmuxState::Handshaking)
        {
            established(now_ns);
        }
    }
    if (m_state == QmuxState::Established)
    {
        drain(now_ns);
    }
}

void QmuxConnection::established(u64 now_ns) noexcept
{
    if (!tunneled() && m_tls.application_protocol().empty())
    {
        // Section 3.1: without ALPN nothing tells which protocol the frames carry.
        refuse_tls(crypto::TlsError::PeerRefused);
        return;
    }
    m_state = QmuxState::Established;
    if (!tunneled() && m_tls.state() == crypto::TlsState::Established)
    {
        m_handshake_deadline = kQmuxNoTimer; // on a server reading early data it still bounds the client's Finished
    }
    m_last_activity = now_ns;
    m_idle_ns = m_config.idle_timeout_ns;
    m_ping_owed = true; // the first RTT sample, which tunes the windows
}

void QmuxConnection::drain(u64 now_ns) noexcept
{
    for (;;)
    {
        const bool early = m_tls.state() == crypto::TlsState::Handshaking;
        const cont::Span<u8> room(m_in + m_in_fill, sizeof(m_in) - m_in_fill);
        usize got = 0U;
        const crypto::TlsError error = early ? m_tls.read_early(room, got) : m_tls.read(room, got);
        m_in_fill += got;
        m_in_early += early ? got : 0U;
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
        if (m_tls.state() != crypto::TlsState::Handshaking)
        {
            m_handshake_deadline = kQmuxNoTimer;
        }
        if (got == 0U && !(early && m_tls.state() != crypto::TlsState::Handshaking))
        {
            return; // nothing more, unless the early data just ended and the handshake with it
        }
    }
}

void QmuxConnection::parse_records(u64 now_ns) noexcept
{
    if (m_framing == Framing::Messages)
    {
        parse_frames(now_ns);
        return;
    }
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
        // A record with any byte of early data in it is early: a client could cut one at the end of its 0-RTT.
        const usize bytes = head + static_cast<usize>(size);
        const cont::ConstSpan<u8> record(m_in + at + head, static_cast<usize>(size));
        if (!read_record(record, m_in_early != 0U, now_ns))
        {
            m_in_fill = 0U;
            m_in_early = 0U;
            return;
        }
        m_in_early -= (bytes < m_in_early) ? bytes : m_in_early;
        at += bytes;
    }
    if (at != 0U)
    {
        std::memmove(m_in, m_in + at, m_in_fill - at);
        m_in_fill -= at;
    }
}

bool QmuxConnection::read_record(cont::ConstSpan<u8> record, bool early, u64 now_ns) noexcept
{
    if (!on_record(record, early, now_ns) || m_state != QmuxState::Established)
    {
        return false;
    }
    ++m_counters.records_received;
    m_last_activity = now_ns; // section 7.1: a whole record received
    return true;
}

bool QmuxConnection::on_record(cont::ConstSpan<u8> record, bool early, u64 now_ns) noexcept
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
            if (!on_frame(frame, early))
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
    if (m_early_sending)
    {
        // Accepted, and now its limits are known: none may be lower than those the early frames kept to.
        m_early_sending = false;
        if (quic_check_remembered_parameters(m_remembered, m_peer) != QuicTransportError::NoError)
        {
            fail(QuicTransportError::ProtocolViolation);
            return false;
        }
    }
    apply_peer_limits(m_peer);
    if (!tunneled())
    {
        m_tls.remember_parameters(bytes);
    }
    return true;
}

void QmuxConnection::apply_peer_limits(const QuicTransportParameters& peer) noexcept
{
    m_streams.set_peer_parameters(peer);
    m_datagrams.set_peer(peer.max_datagram_frame_size, kQmuxMaxRecord + kQuicShortPacketOverhead);
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

bool QmuxConnection::on_frame(const QuicFrame& frame, bool early) noexcept
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
            error = m_streams.on_frame(frame, early);
            break;
        case QuicFrameKind::Datagram:
            error = m_datagrams.on_frame(frame, early);
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
    m_ws.close_code = kWsCloseProtocolError;
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
        if (m_framing == Framing::Messages && !m_ws.close_sent)
        {
            u8 status[2] = {static_cast<u8>(m_ws.close_code >> 8U), static_cast<u8>(m_ws.close_code & 0xFFU)};
            static_cast<void>(send_control(WsOpcode::Close, cont::ConstSpan<u8>(status, sizeof(status))));
            m_ws.close_sent = true;
        }
        if (!tunneled())
        {
            static_cast<void>(m_tls.close()); // close_notify; a session past that has nothing to add
        }
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
    return !m_parameters_sent || m_ping_owed || m_pong_owed || m_ws.pong_owed ||
           ((m_peer_known || m_early_sending) && (m_streams.has_frame() || m_datagrams.has_frame(now_ns)));
}

bool QmuxConnection::early_open() const noexcept
{
    return m_state == QmuxState::Handshaking && m_early_sending && !m_early_full &&
           m_tls.early_data() == crypto::TlsEarlyData::Offered;
}

usize QmuxConnection::record_room() const noexcept
{
    if (m_state != QmuxState::Handshaking)
    {
        return kQmuxMaxRecord;
    }
    const usize room = m_tls.early_data_room();
    const usize left = (room > kSizeBytes) ? room - kSizeBytes : 0U;
    return (left < kQmuxMaxRecord) ? left : kQmuxMaxRecord;
}

crypto::TlsError QmuxConnection::send_record(cont::ConstSpan<u8> record) noexcept
{
    if (m_tls.state() == crypto::TlsState::Established)
    {
        return m_tls.send(record);
    }
    return is_client() ? m_tls.send_early(record) : m_tls.send_half_rtt(record);
}

bool QmuxConnection::wants_write(u64 now_ns) const noexcept
{
    return !m_transport_over &&
           (stage_pending() != 0U || m_close_owed ||
            ((m_state == QmuxState::Established || early_open()) && frames_due(now_ns)));
}

usize QmuxConnection::take(cont::Span<u8> out, u64 now_ns) noexcept
{
    if (m_transport_over || m_state == QmuxState::Idle)
    {
        return 0U;
    }
    produce(now_ns);
    usize written = 0U;
    if (tunneled())
    {
        written = (out.size() < stage_pending()) ? out.size() : stage_pending();
        if (written != 0U)
        {
            std::memcpy(out.data(), m_stage + m_stage_head, written);
        }
        m_stage_head += written;
    }
    else if (m_tls.take(out, written) != crypto::TlsError::None)
    {
        return 0U;
    }
    if (written != 0U && stage_pending() == 0U && m_state == QmuxState::Established)
    {
        m_last_activity = now_ns; // section 7.1: every record handed on; a peer that stops reading times out
    }
    return written;
}

void QmuxConnection::taken(usize count, u64 now_ns) noexcept
{
    m_stage_head += count;
    if (count != 0U && stage_pending() == 0U && m_state == QmuxState::Established)
    {
        m_last_activity = now_ns;
    }
}

cont::ConstSpan<u8> QmuxConnection::outgoing(u64 now_ns) noexcept
{
    if (m_transport_over || m_state == QmuxState::Idle)
    {
        return {};
    }
    produce(now_ns);
    return tunneled() ? cont::ConstSpan<u8>(m_stage + m_stage_head, stage_pending()) : m_tls.outgoing();
}

bool QmuxConnection::send_finished() const noexcept
{
    return m_send_shut && stage_pending() == 0U;
}

usize QmuxConnection::stage_pending() const noexcept
{
    return tunneled() ? m_stage_tail - m_stage_head : m_tls.pending();
}

usize QmuxConnection::stage_capacity() const noexcept
{
    return tunneled() ? kTunnelStageBytes : crypto::kTlsMaxBufferedBytes;
}

crypto::TlsError QmuxConnection::stage_send(cont::ConstSpan<u8> bytes) noexcept
{
    if (!tunneled())
    {
        return m_tls.send(bytes);
    }
    if (m_stage_head != 0U)
    {
        std::memmove(m_stage, m_stage + m_stage_head, stage_pending());
        m_stage_tail -= m_stage_head;
        m_stage_head = 0U;
    }
    if (bytes.size() > kTunnelStageBytes - m_stage_tail)
    {
        return crypto::TlsError::Full;
    }
    std::memcpy(m_stage + m_stage_tail, bytes.data(), bytes.size());
    m_stage_tail += bytes.size();
    return crypto::TlsError::None;
}

void QmuxConnection::produce(u64 now_ns) noexcept
{
    while (m_state == QmuxState::Established || m_close_owed || early_open())
    {
        if (stage_pending() + sizeof(m_out) + kWsMaxControlBytes + kTlsRecordOverhead > stage_capacity())
        {
            return; // the transport is behind: the records wait, unassembled
        }
        if (m_ws.pong_owed && !m_close_owed)
        {
            m_ws.pong_owed = false; // section 5.5.3: only the latest Ping is owed an answer
            const crypto::TlsError sent = send_control(WsOpcode::Pong, cont::ConstSpan<u8>(m_ws.pong, m_ws.pong_bytes));
            if (sent != crypto::TlsError::None)
            {
                tls_failed(sent);
                return;
            }
        }
        const bool closing = m_close_owed;
        const usize used = closing ? build_close() : build_record(now_ns);
        if (used == 0U)
        {
            m_early_full = early_open() && frames_due(now_ns); // what did not fit waits for the handshake
            return;
        }
        crypto::TlsError error = crypto::TlsError::None;
        if (m_framing == Framing::Messages)
        {
            error = send_frame(WsOpcode::Binary, m_out + kFrameAt, used);
        }
        else
        {
            // Size is in its shortest encoding, directly in front of the frames.
            const usize head = quic_varint_bytes(used);
            cont::WireWriter size(cont::Span<u8>(m_out + kFrameAt - head, head));
            put_quic_varint(size, used);
            error = send_record(cont::ConstSpan<u8>(m_out + kFrameAt - head, head + used));
        }
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
        used = write_parameters(cont::Span<u8>(m_out + kFrameAt, kQmuxMaxRecord));
        m_parameters_sent = true;
    }
    return used + write_quic_frame(frame, cont::Span<u8>(m_out + kFrameAt + used, kQmuxMaxRecord - used));
}

void QmuxConnection::fill_local(QuicTransportParameters& local) const noexcept
{
    m_streams.fill_parameters(local);
    m_datagrams.fill_parameters(local);
    local.max_idle_timeout = m_config.idle_timeout_ns / kMsNs;
}

usize QmuxConnection::write_parameters(cont::Span<u8> out) noexcept
{
    QuicTransportParameters local;
    fill_local(local);
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
    u8* const frames = m_out + kFrameAt;
    const bool early = m_state == QmuxState::Handshaking; // only a client's 0-RTT is built before the handshake ends
    const usize room = record_room();
    usize used = 0U;
    if (!m_parameters_sent)
    {
        used += write_parameters(cont::Span<u8>(frames, room));
        m_parameters_sent = true;
    }
    if (m_pong_owed)
    {
        used += write_ping(iana::kQuicFrameQxPingResponse, m_pong_sequence, cont::Span<u8>(frames + used, room - used));
        m_pong_owed = false;
    }
    if (m_ping_owed)
    {
        used += write_ping(iana::kQuicFrameQxPingRequest, m_ping_next, cont::Span<u8>(frames + used, room - used));
        ++m_ping_next;
        m_ping_sent_ns = now_ns;
        m_ping_owed = false;
        ++m_counters.pings_sent;
    }
    if (m_peer_known || m_early_sending)
    {
        while (m_datagrams.has_frame(now_ns))
        {
            u64 id = 0U;
            const usize bytes = m_datagrams.write_next(cont::Span<u8>(frames + used, room - used), now_ns, id);
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
        while (m_streams.next_frame(room - used, now_ns, m_rtt_ns, frame, record))
        {
            const usize bytes = write_quic_frame(frame, cont::Span<u8>(frames + used, room - used));
            if (bytes == 0U)
            {
                m_streams.on_lost(record); // owed again (a frame next_frame sized never fails to fit)
                break;
            }
            used += bytes;
            if (!early)
            {
                m_streams.on_acked(record); // section 4.1: acknowledged once the transport has it
            }
        }
    }
    if (used != 0U)
    {
        const usize padded = padded_size(used, m_config.padding_block, room);
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
            const u64 first = (idle < keepalive) ? idle : keepalive;
            return (first < m_handshake_deadline) ? first : m_handshake_deadline;
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
    if ((m_state == QmuxState::Handshaking || m_state == QmuxState::Established) && now_ns >= m_handshake_deadline)
    {
        m_close.reason = QmuxCloseReason::HandshakeTimeout;
        m_send_shut = true;
        m_state = QmuxState::Closed;
    }
    else if (m_state == QmuxState::Established && now_ns >= idle_deadline())
    {
        m_close.reason = QmuxCloseReason::IdleTimeout;
        m_ws.close_code = kWsCloseGoingAway;
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

// ---- WebSocket: the opening handshake, then a record per message (RFC 6455) ----

bool QmuxConnection::takes_websocket() const noexcept
{
    return m_config.websocket != nullptr && m_tls.application_protocol() == cont::StringView(kHttp11);
}

cont::ConstSpan<u8> QmuxConnection::websocket_protocol() const noexcept
{
    return m_ws.upgraded ? m_config.websocket->protocols[m_ws.protocol] : cont::ConstSpan<u8>();
}

void QmuxConnection::refuse_tls(crypto::TlsError error) noexcept
{
    m_tls_error = error;
    m_close.reason = QmuxCloseReason::TlsFailed;
    finish_sending();
    m_state = QmuxState::Closed;
}

void QmuxConnection::refuse_upgrade(u16 status) noexcept
{
    m_close.reason = QmuxCloseReason::UpgradeRefused;
    m_close.error_code = status;
    m_in_fill = 0U;
    finish_sending();
    m_state = QmuxState::Closed;
}

void QmuxConnection::begin_upgrade() noexcept
{
    m_framing = Framing::Upgrading;
    if (!is_client())
    {
        return; // the client speaks first
    }
    u8 random[kWsKeyBytes];
    if (crypto::random_bytes(cont::Span<u8>(random, sizeof(random))) != crypto::CryptoError::None)
    {
        refuse_tls(crypto::TlsError::ProviderFailure);
        return;
    }
    u8 key[kWsKeyChars];
    ws_key_of(random, cont::Span<u8, kWsKeyChars>(key, kWsKeyChars));
    ws_accept_of(cont::ConstSpan<u8>(key, sizeof(key)), cont::Span<u8, kWsAcceptChars>(m_ws.accept, kWsAcceptChars));
    const usize used = ws_write_request(cont::Span<u8>(m_out, kWsMaxHeadBytes), *m_config.websocket,
                                        cont::ConstSpan<u8>(key, sizeof(key)));
    const crypto::TlsError error = m_tls.send(cont::ConstSpan<u8>(m_out, used));
    if (error != crypto::TlsError::None)
    {
        tls_failed(error);
    }
}

void QmuxConnection::read_upgrade(u64 now_ns) noexcept
{
    const bool client = is_client();
    usize got = 0U;
    const crypto::TlsError error = m_tls.read(cont::Span<u8>(m_in + m_in_fill, sizeof(m_in) - m_in_fill), got);
    m_in_fill += got;
    const cont::ConstSpan<u8> bytes(m_in, m_in_fill);
    WsHandshake handshake;
    const HttpDecode decode =
        client ? ws_read_response(bytes, *m_config.websocket, cont::ConstSpan<u8>(m_ws.accept, kWsAcceptChars),
                                  m_ws.scanned, handshake)
               : ws_read_request(bytes, *m_config.websocket, m_ws.scanned, handshake);
    if (decode == HttpDecode::NeedMore)
    {
        if (error == crypto::TlsError::Closed)
        {
            peer_shutdown();
        }
        else if (error != crypto::TlsError::None)
        {
            tls_failed(error);
        }
        return;
    }
    if (!client)
    {
        const usize used =
            ws_write_response(cont::Span<u8>(m_out, kWsMaxResponseBytes), *m_config.websocket, handshake);
        const crypto::TlsError sent = m_tls.send(cont::ConstSpan<u8>(m_out, used));
        if (sent != crypto::TlsError::None)
        {
            tls_failed(sent);
            return;
        }
    }
    if (handshake.status != 101U)
    {
        refuse_upgrade(handshake.status);
        return;
    }
    // What follows the head is the first frames, read as soon as the connection stands.
    m_in_fill -= handshake.head_bytes;
    std::memmove(m_in, m_in + handshake.head_bytes, m_in_fill);
    m_ws.protocol = handshake.protocol;
    m_ws.upgraded = true;
    m_framing = Framing::Messages;
    established(now_ns);
}

void QmuxConnection::fail_websocket(u16 status) noexcept
{
    m_close.reason = QmuxCloseReason::WebSocketFailed;
    m_close.error_code = status;
    m_ws.close_code = status;
    m_in_fill = 0U;
    m_ws.message_bytes = 0U;
    finish_sending();
    m_state = QmuxState::Closed;
}

void QmuxConnection::parse_frames(u64 now_ns) noexcept
{
    const bool from_client = !is_client();
    usize at = m_ws.message_bytes; // whole frames are consumed; what precedes `at` is the message so far
    while (at < m_in_fill)
    {
        WsFrameHeader header;
        u16 failure = 0U;
        const HttpDecode decode = ws_read_frame_header(cont::ConstSpan<u8>(m_in + at, m_in_fill - at), from_client,
                                                       kQmuxMaxRecord - m_ws.message_bytes, header, failure);
        if (decode == HttpDecode::Invalid)
        {
            fail_websocket(failure);
            return;
        }
        if (decode == HttpDecode::NeedMore || m_in_fill - at - header.header_bytes < header.payload_bytes)
        {
            break;
        }
        const usize payload_at = at + header.header_bytes;
        if (header.masked)
        {
            ws_mask(cont::Span<u8>(m_in + payload_at, header.payload_bytes), header.mask);
        }
        at = payload_at + header.payload_bytes;
        if (static_cast<u8>(header.opcode) >= static_cast<u8>(WsOpcode::Close))
        {
            on_control_frame(header.opcode, cont::ConstSpan<u8>(m_in + payload_at, header.payload_bytes));
        }
        else if (!on_data_frame(header, payload_at, now_ns))
        {
            m_in_fill = 0U;
            m_ws.message_bytes = 0U;
            return;
        }
        if (m_state != QmuxState::Established)
        {
            return;
        }
    }
    const usize left = m_in_fill - at;
    std::memmove(m_in + m_ws.message_bytes, m_in + at, left);
    m_in_fill = m_ws.message_bytes + left;
}

bool QmuxConnection::on_data_frame(const WsFrameHeader& header, usize payload_at, u64 now_ns) noexcept
{
    if (header.opcode == WsOpcode::Text)
    {
        fail_websocket(kWsCloseUnsupportedData); // records are binary, and a Text message would have to be UTF-8
        return false;
    }
    if ((header.opcode == WsOpcode::Continuation) != m_ws.fragmented)
    {
        fail_websocket(kWsCloseProtocolError); // section 5.4: a fragment with no message, or a message inside one
        return false;
    }
    const usize bytes = header.payload_bytes;
    m_ws.fragmented = !header.fin;
    cont::ConstSpan<u8> record(m_in + payload_at, bytes);
    if (m_ws.message_bytes != 0U || !header.fin)
    {
        std::memmove(m_in + m_ws.message_bytes, m_in + payload_at, bytes);
        m_ws.message_bytes += bytes;
        if (!header.fin)
        {
            return true;
        }
        record = cont::ConstSpan<u8>(m_in, m_ws.message_bytes);
    }
    if (record.empty())
    {
        m_frame_type = 0U;
        fail(QuicTransportError::ProtocolViolation); // as a Size of zero on the record path
        return false;
    }
    m_ws.message_bytes = 0U;
    return read_record(record, false, now_ns);
}

void QmuxConnection::on_control_frame(WsOpcode opcode, cont::ConstSpan<u8> payload) noexcept
{
    if (opcode == WsOpcode::Ping)
    {
        m_ws.pong_owed = true;
        m_ws.pong_bytes = static_cast<u8>(payload.size());
        if (!payload.empty())
        {
            std::memcpy(m_ws.pong, payload.data(), payload.size());
        }
        return;
    }
    if (opcode == WsOpcode::Close)
    {
        u16 code = kWsCloseNoStatus;
        cont::ConstSpan<u8> reason;
        const u16 failure = ws_read_close(payload, code, reason);
        if (failure != 0U)
        {
            fail_websocket(failure);
            return;
        }
        m_close.reason = QmuxCloseReason::PeerShutdown;
        m_close.error_code = code;
        keep_reason(reason);
        m_ws.close_code = (code != kWsCloseNoStatus) ? code : kWsCloseNormal; // section 5.5.1: echoes what it read
        finish_sending();
        m_state = QmuxState::Closed;
    }
    // A Pong answers nothing: this side sends no Pings, and section 5.5.3 allows one unasked.
}

crypto::TlsError QmuxConnection::send_frame(WsOpcode opcode, u8* payload, usize payload_bytes) noexcept
{
    const bool client = is_client();
    u8 mask[kWsMaskBytes];
    if (client)
    {
        if (crypto::random_bytes(cont::Span<u8>(mask, sizeof(mask))) != crypto::CryptoError::None)
        {
            return crypto::TlsError::ProviderFailure; // section 10.3: a predictable mask must never go out
        }
        ws_mask(cont::Span<u8>(payload, payload_bytes), mask);
    }
    const usize head = ws_frame_header_bytes(payload_bytes, client);
    ws_write_frame_header(cont::Span<u8>(payload - head, head), opcode, true, payload_bytes, client ? mask : nullptr);
    return stage_send(cont::ConstSpan<u8>(payload - head, head + payload_bytes));
}

crypto::TlsError QmuxConnection::send_control(WsOpcode opcode, cont::ConstSpan<u8> payload) noexcept
{
    u8 frame[kWsMaxHeaderBytes + kWsMaxControlBytes];
    if (!payload.empty())
    {
        std::memcpy(frame + kWsMaxHeaderBytes, payload.data(), payload.size());
    }
    return send_frame(opcode, frame + kWsMaxHeaderBytes, payload.size());
}

} // namespace crd::net
