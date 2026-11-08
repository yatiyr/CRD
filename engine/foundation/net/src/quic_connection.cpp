// crd-net -- a QUIC connection (4.2a; contract: docs/design/network-foundation.md#quic-connection).
#include <crd/containers/wire_bytes.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/quic_connection.hpp>

#include <cstring>
#include <new>

namespace crd::net
{
namespace
{
constexpr u8 kRecordCrypto = 1U;
constexpr u8 kRecordHandshakeDone = 2U;
constexpr u8 kRecordRetire = 3U;
constexpr u8 kRecordStream = 4U; // a frame of the stream set
constexpr u32 kNoRecord = ~u32{0};
constexpr u64 kNsPerMs = 1000000U;
constexpr u64 kCryptoFrameType = 0x06U;
constexpr usize kMinCryptoBytes = 4096U; // RFC 9000 section 7.5
constexpr usize kMaxCryptoBytes = 1U << 20U;
constexpr u32 kMaxSentPackets = 1U << 16U;
constexpr usize kMinUsefulPacket = 64U; // a server with less credit than this waits for a datagram
constexpr usize kPullChunk = 512U;
constexpr u64 kSkipMask = 63U;   // one 1-RTT packet number in 64 is skipped (section 21.4)
constexpr u32 kAckOnlyRun = 16U; // ACK-only packets in a row before one carries a PING

[[nodiscard]] constexpr usize index_of(QuicSpace space) noexcept
{
    return static_cast<usize>(space);
}

[[nodiscard]] constexpr QuicPacketType packet_type(QuicSpace space) noexcept
{
    if (space == QuicSpace::Initial)
    {
        return QuicPacketType::Initial;
    }
    return (space == QuicSpace::Handshake) ? QuicPacketType::Handshake : QuicPacketType::OneRtt;
}

[[nodiscard]] constexpr crypto::QuicLevel level_of(QuicSpace space) noexcept
{
    return static_cast<crypto::QuicLevel>(static_cast<u8>(space));
}

[[nodiscard]] constexpr QuicSpace space_of(crypto::QuicLevel level) noexcept
{
    return (level == crypto::QuicLevel::Early) ? QuicSpace::Application
                                               : static_cast<QuicSpace>(static_cast<u8>(level));
}

[[nodiscard]] constexpr crypto::QuicSide other(crypto::QuicSide side) noexcept
{
    return (side == crypto::QuicSide::Client) ? crypto::QuicSide::Server : crypto::QuicSide::Client;
}

[[nodiscard]] constexpr u64 min_u64(u64 a, u64 b) noexcept
{
    return (a < b) ? a : b;
}

[[nodiscard]] constexpr u64 max_u64(u64 a, u64 b) noexcept
{
    return (a > b) ? a : b;
}

[[nodiscard]] constexpr u64 add_saturating(u64 a, u64 b) noexcept
{
    return (a > kNeverNs - b) ? kNeverNs : a + b;
}

[[nodiscard]] bool same_bytes(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

[[nodiscard]] bool config_valid(const QuicConnectionConfig& config) noexcept
{
    const QuicTransportParameters& p = config.parameters;
    const bool version_ok = config.version == crypto::QuicVersion::V1 || config.version == crypto::QuicVersion::V2;
    bool listed = config.versions.empty();
    for (const u32 version : config.versions)
    {
        listed = listed || version == static_cast<u32>(config.version);
    }
    return config.tls != nullptr && version_ok && listed && config.versions.size() < kQuicMaxAvailableVersions &&
           p.initial_max_data == 0U && p.initial_max_stream_data_bidi_local == 0U &&
           p.initial_max_stream_data_bidi_remote == 0U && p.initial_max_stream_data_uni == 0U &&
           p.initial_max_streams_bidi == 0U && p.initial_max_streams_uni == 0U && p.max_datagram_frame_size == 0U &&
           QuicStreamSet::limits_valid(config.streams) && p.active_connection_id_limit >= kQuicDefaultActiveIdLimit &&
           p.active_connection_id_limit <= kQuicMaxPeerConnectionIds && config.sent_packets != 0U &&
           config.sent_packets <= kMaxSentPackets && config.sent_frames != 0U &&
           config.sent_frames <= kMaxSentPackets && config.crypto_bytes % kQuicStreamChunk == 0U &&
           config.crypto_bytes >= kMinCryptoBytes && config.crypto_bytes <= kMaxCryptoBytes &&
           config.max_datagram_bytes >= kQuicMinInitialDatagram &&
           config.max_datagram_bytes <= kQuicDefaultMaxUdpPayload &&
           (config.entropy_seed.empty() || config.entropy_seed.size() == crypto::kPrfKeyBytes) &&
           (config.token.size() <= crypto::kQuicMaxTokenBytes);
}
} // namespace

// ---- Events ----

void QuicConnection::Events::on_acked(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Acked);
}

void QuicConnection::Events::on_lost(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Lost);
}

void QuicConnection::Events::on_discarded(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Discarded);
}

// ---- Construction ----

QuicConnection::QuicConnection(memory::IAllocator& allocator, const QuicConnectionConfig& config) noexcept
    : m_allocator(allocator), m_config(config), m_events(*this)
{
    if (!config_valid(config))
    {
        return;
    }
    m_version_count = 0U;
    if (config.versions.empty())
    {
        m_versions[m_version_count++] = static_cast<u32>(config.version);
    }
    for (const u32 version : config.versions)
    {
        m_versions[m_version_count++] = version;
    }
    m_token_bytes = config.token.size();
    if (m_token_bytes != 0U)
    {
        std::memcpy(m_token, config.token.data(), m_token_bytes);
    }
    m_config.versions = {};
    m_config.token = {};
    const crypto::CryptoError seeded = config.entropy_seed.empty()
                                           ? m_entropy.generate(crypto::AlgorithmId::Siphash24)
                                           : m_entropy.init(crypto::AlgorithmId::Siphash24, config.entropy_seed);
    m_config.entropy_seed = {};

    const usize per_space = config.sent_packets;
    const usize crypto = config.crypto_bytes;
    const usize words = quic_stream_bitmap_words(crypto);
    QuicSentEntry* const entries = memory::try_construct_array<QuicSentEntry>(allocator, kQuicSpaces * per_space);
    SentFrame* const frames = memory::try_construct_array<SentFrame>(allocator, config.sent_frames);
    u8* const bytes = memory::try_construct_array<u8>(allocator, kQuicSpaces * 2U * crypto);
    u64* const bits = memory::try_construct_array<u64>(allocator, kQuicSpaces * 3U * words);
    void* const recovery = allocator.try_allocate(sizeof(QuicRecovery), alignof(QuicRecovery));
    const bool streams = m_streams.init(allocator, config.streams, config.side);
    if (seeded != crypto::CryptoError::None || entries == nullptr || frames == nullptr || bytes == nullptr ||
        bits == nullptr || recovery == nullptr || !streams)
    {
        memory::destroy_array(allocator, entries, kQuicSpaces * per_space);
        memory::destroy_array(allocator, frames, config.sent_frames);
        memory::destroy_array(allocator, bytes, kQuicSpaces * 2U * crypto);
        memory::destroy_array(allocator, bits, kQuicSpaces * 3U * words);
        memory::destroy_array(allocator, static_cast<u8*>(recovery), sizeof(QuicRecovery));
        return;
    }
    for (usize index = 0U; index < kQuicSpaces; ++index)
    {
        u8* const level_bytes = bytes + (index * 2U * crypto);
        u64* const level_bits = bits + (index * 3U * words);
        static_cast<void>(
            m_levels[index].in.init(cont::Span<u8>(level_bytes, crypto), cont::Span<u64>(level_bits, words)));
        static_cast<void>(m_levels[index].out.init(cont::Span<u8>(level_bytes + crypto, crypto),
                                                   cont::Span<u64>(level_bits + words, words),
                                                   cont::Span<u64>(level_bits + (2U * words), words)));
    }
    QuicRecoveryConfig recovery_config;
    recovery_config.side = config.side;
    recovery_config.initial_rtt_ns = config.initial_rtt_ns;
    recovery_config.local_max_ack_delay_ns = config.parameters.max_ack_delay * kNsPerMs;
    recovery_config.local_ack_delay_exponent = config.parameters.ack_delay_exponent;
    m_recovery = new (recovery) QuicRecovery(recovery_config, cont::Span<QuicSentEntry>(entries, per_space),
                                             cont::Span<QuicSentEntry>(entries + per_space, per_space),
                                             cont::Span<QuicSentEntry>(entries + (2U * per_space), per_space));
    m_frames = frames;
    m_crypto_bytes = bytes;
    m_crypto_bits = bits;
    m_storage = entries;
}

QuicConnection::~QuicConnection() noexcept
{
    if (m_recovery != nullptr)
    {
        m_recovery->~QuicRecovery();
        m_allocator.deallocate(m_recovery);
    }
    const usize crypto = m_config.crypto_bytes;
    memory::destroy_array(m_allocator, m_storage, kQuicSpaces * usize{m_config.sent_packets});
    memory::destroy_array(m_allocator, m_frames, m_config.sent_frames);
    memory::destroy_array(m_allocator, m_crypto_bytes, kQuicSpaces * 2U * crypto);
    memory::destroy_array(m_allocator, m_crypto_bits, kQuicSpaces * 3U * quic_stream_bitmap_words(crypto));
}

u64 QuicConnection::entropy() noexcept
{
    u8 counter[8];
    for (usize index = 0U; index < sizeof(counter); ++index)
    {
        counter[index] = static_cast<u8>(m_entropy_counter >> (8U * index));
    }
    ++m_entropy_counter;
    u64 out = 0U;
    static_cast<void>(m_entropy.hash(cont::ConstSpan<u8>(counter, sizeof(counter)), out)); // the key is ready
    return out;
}

bool QuicConnection::connect(cont::ConstSpan<u8> local_cid, cont::ConstSpan<u8> initial_dcid, u64 now_ns) noexcept
{
    if (!ready() || m_state != QuicConnectionState::Idle || m_config.side != crypto::QuicSide::Client ||
        initial_dcid.size() < 8U || initial_dcid.size() > crypto::kQuicMaxConnectionIdBytes ||
        local_cid.size() > crypto::kQuicMaxConnectionIdBytes)
    {
        return false;
    }
    static_cast<void>(m_local_cid.assign(local_cid)); // both bounded above
    static_cast<void>(m_initial_dcid.assign(initial_dcid));
    m_original_dcid = m_initial_dcid;
    m_validated = true; // a client's sending is not limited by its address
    return start(now_ns);
}

bool QuicConnection::accept(const QuicAccept& accept, u64 now_ns) noexcept
{
    const auto bounded = [](cont::ConstSpan<u8> id)
    {
        return id.size() <= crypto::kQuicMaxConnectionIdBytes;
    };
    if (!ready() || m_state != QuicConnectionState::Idle || m_config.side != crypto::QuicSide::Server ||
        accept.local_cid.empty() || accept.client_dcid.empty() || !bounded(accept.local_cid) ||
        !bounded(accept.client_dcid) || !bounded(accept.original_dcid) || !bounded(accept.client_scid) ||
        (!accept.retried && !same_bytes(accept.original_dcid, accept.client_dcid)))
    {
        return false;
    }
    static_cast<void>(m_local_cid.assign(accept.local_cid)); // each bounded above
    static_cast<void>(m_initial_dcid.assign(accept.client_dcid));
    static_cast<void>(m_original_dcid.assign(accept.original_dcid));
    static_cast<void>(m_peer_scid.assign(accept.client_scid));
    m_retried = accept.retried;
    m_validated = accept.validated;
    m_peer_scid_known = true;
    m_peer_cids[0].sequence = 0U;
    m_peer_cids[0].id = m_peer_scid;
    m_peer_cid_count = 1U;
    return start(now_ns);
}

bool QuicConnection::start(u64 now_ns) noexcept
{
    const crypto::QuicSide side = m_config.side;
    if (m_protection.start(side, m_config.version, m_initial_dcid.view()) != crypto::CryptoError::None)
    {
        return false;
    }
    m_protection.accept_cleared_quic_bit(m_config.grease_quic_bit);
    m_protection.set_key_update_interval(m_config.key_update_interval);
    m_local = m_config.parameters;
    m_streams.fill_parameters(m_local);
    m_local.has_initial_source_connection_id = true;
    m_local.initial_source_connection_id = m_local_cid;
    if (side == crypto::QuicSide::Server)
    {
        m_local.has_original_destination_connection_id = true;
        m_local.original_destination_connection_id = m_original_dcid;
        m_local.has_retry_source_connection_id = m_retried;
        if (m_retried)
        {
            m_local.retry_source_connection_id = m_initial_dcid;
        }
    }
    m_local.grease_quic_bit = m_config.grease_quic_bit;
    m_local.has_version_information = true;
    m_local.version_information.chosen = static_cast<u32>(m_config.version);
    m_local.version_information.available_count = m_version_count;
    for (usize index = 0U; index < m_version_count; ++index)
    {
        m_local.version_information.available[index] = m_versions[index];
    }
    u8 encoded[crypto::kQuicMaxTransportParameterBytes];
    const usize bytes = write_quic_transport_parameters(m_local, side, QuicTransportParameterGrease{entropy()},
                                                        cont::Span<u8>(encoded, sizeof(encoded)));
    if (bytes == 0U || m_handshake.init(*m_config.tls, cont::ConstSpan<u8>(encoded, bytes), m_config.tickets) !=
                           crypto::TlsError::None)
    {
        return false;
    }
    m_state = QuicConnectionState::Handshaking;
    m_idle_ns = m_local.max_idle_timeout * kNsPerMs;
    restart_idle(now_ns);
    return true;
}

// ---- Receiving ----

void QuicConnection::receive(cont::Span<u8> datagram, QuicEcn ecn, u64 now_ns) noexcept
{
    if (m_state == QuicConnectionState::Idle || m_state == QuicConnectionState::Closed)
    {
        return;
    }
    ++m_counters.datagrams_received;
    m_counters.bytes_received += datagram.size();
    if (!m_validated)
    {
        m_recovery->set_amplification_blocked(amplification_credit() < kMinUsefulPacket, now_ns);
    }
    if (m_state == QuicConnectionState::Draining)
    {
        return;
    }
    if (m_state == QuicConnectionState::Closing)
    {
        // Answer at a falling rate: after 1, 2, 4, ... more datagrams (section 10.2.1).
        if (++m_close_received >= m_close_owed_after)
        {
            m_close_received = 0U;
            m_close_owed_after = (m_close_owed_after < (u64{1} << 32U)) ? m_close_owed_after * 2U : m_close_owed_after;
            m_close_owed = true;
        }
        return;
    }
    QuicDatagramPackets packets(datagram, m_local_cid.bytes);
    QuicHeader header;
    cont::Span<u8> packet;
    while (packets.next(header, packet))
    {
        receive_packet(header, packet, datagram.size(), ecn, now_ns);
        if (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established)
        {
            return;
        }
    }
    if (packets.foreign() || packets.error() != QuicHeaderError::None)
    {
        ++m_counters.packets_dropped;
    }
}

void QuicConnection::receive_packet(const QuicHeader& header, cont::Span<u8> packet, usize datagram_bytes, QuicEcn ecn,
                                    u64 now_ns) noexcept
{
    const bool client = m_config.side == crypto::QuicSide::Client;
    if (header.type == QuicPacketType::VersionNegotiation)
    {
        version_negotiation(header, packet);
        return;
    }
    const bool long_header = header.type != QuicPacketType::OneRtt;
    bool routed = same_bytes(header.dcid, m_local_cid.view()) ||
                  (!client && long_header && same_bytes(header.dcid, m_initial_dcid.view()));
    if (long_header)
    {
        // Retry is accepted with address validation tokens; other versions are not this connection's.
        routed = routed && header.type != QuicPacketType::Retry && header.type != QuicPacketType::OtherVersion &&
                 header.version == static_cast<u32>(m_config.version) &&
                 (!m_peer_scid_known || same_bytes(header.scid, m_peer_scid.view()));
    }
    // A server discards a client Initial in a datagram under 1200 bytes (section 14.1).
    if (!routed || (!client && header.type == QuicPacketType::Initial && datagram_bytes < kQuicMinInitialDatagram))
    {
        ++m_counters.packets_dropped;
        return;
    }
    QuicOpened opened;
    const QuicPacketError error = m_protection.open(header, packet, opened);
    if (error != QuicPacketError::None)
    {
        const QuicTransportError close = quic_packet_close(error);
        if (close == QuicTransportError::NoError)
        {
            ++m_counters.packets_dropped;
            return;
        }
        fail(close, 0U, now_ns);
        return;
    }
    const QuicSpace space = quic_space_of(opened.type);
    if (client && long_header && !m_peer_scid_known)
    {
        // The server's first packet names the ID the client sends to from now on (section 7.2).
        static_cast<void>(m_peer_scid.assign(header.scid));
        m_peer_scid_known = true;
        m_peer_cids[0].sequence = 0U;
        m_peer_cids[0].id = m_peer_scid;
        m_peer_cid_count = 1U;
    }
    if (!client && opened.type == QuicPacketType::Handshake)
    {
        // A Handshake packet proves the client's address (section 8.1).
        m_validated = true;
        m_recovery->set_amplification_blocked(false, now_ns);
    }
    if (!m_protection.can_open(QuicPacketType::Initial))
    {
        discard_space(QuicSpace::Initial, now_ns);
    }

    cont::WireReader in(opened.payload);
    bool eliciting = false;
    while (!in.done())
    {
        QuicFrame read;
        const QuicTransportError frame_error = read_quic_frame(in, read);
        if (frame_error != QuicTransportError::NoError)
        {
            fail(frame_error, 0U, now_ns);
            return;
        }
        if (!quic_frame_allowed(read.kind, read.application, opened.type, client))
        {
            fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
            return;
        }
        eliciting = eliciting || quic_frame_ack_eliciting(read.kind);
        if (!on_frame(read, space, now_ns))
        {
            return;
        }
    }
    if (!m_space_gone[index_of(space)])
    {
        static_cast<void>(m_recovery->on_received(space, opened.number, eliciting, ecn, now_ns));
    }
    if (opened.key_update)
    {
        m_previous_key_ns = add_saturating(now_ns, three_ptos());
    }
    restart_idle(now_ns);
    m_sent_since_receive = false;
}

bool QuicConnection::on_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept
{
    switch (frame.kind)
    {
        case QuicFrameKind::Padding:
        case QuicFrameKind::Ping:
            return true;
        case QuicFrameKind::Stream:
        case QuicFrameKind::ResetStream:
        case QuicFrameKind::StopSending:
        case QuicFrameKind::MaxData:
        case QuicFrameKind::MaxStreamData:
        case QuicFrameKind::MaxStreams:
        case QuicFrameKind::DataBlocked:
        case QuicFrameKind::StreamDataBlocked:
        case QuicFrameKind::StreamsBlocked:
        {
            const QuicTransportError error = m_streams.on_frame(frame);
            if (error != QuicTransportError::NoError)
            {
                fail(error, 0U, now_ns);
                return false;
            }
            return true;
        }
        case QuicFrameKind::Ack:
        {
            const QuicAckOutcome outcome = m_recovery->on_ack(space, frame.ack, now_ns, m_events);
            if (outcome.error == QuicRecoveryError::AckOfUnsent)
            {
                fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
                return false;
            }
            if (space == QuicSpace::Application)
            {
                m_protection.acknowledged(frame.ack.largest);
            }
            return true;
        }
        case QuicFrameKind::Crypto:
            return crypto_frame(frame, space, now_ns);
        case QuicFrameKind::HandshakeDone:
            confirm(now_ns);
            return true;
        case QuicFrameKind::ConnectionClose:
            m_close = QuicCloseInfo{};
            m_close.application = frame.application;
            m_close.error_code = frame.error_code;
            m_close.frame_type = frame.frame_type;
            m_reason_bytes = (frame.data.size() < kQuicMaxReasonBytes) ? frame.data.size() : kQuicMaxReasonBytes;
            if (m_reason_bytes != 0U)
            {
                std::memcpy(m_reason, frame.data.data(), m_reason_bytes);
            }
            enter(QuicConnectionState::Draining, now_ns);
            return false;
        case QuicFrameKind::NewToken:
            if (frame.data.size() <= sizeof(m_new_token))
            {
                std::memcpy(m_new_token, frame.data.data(), frame.data.size());
                m_new_token_bytes = frame.data.size();
            }
            return true;
        case QuicFrameKind::NewConnectionId:
            return new_connection_id(frame, now_ns);
        case QuicFrameKind::PathChallenge:
            std::memcpy(m_path_response, frame.data.data(), kQuicPathDataBytes);
            m_path_response_owed = true;
            return true;
        case QuicFrameKind::RetireConnectionId: // this side issued sequence 0 alone, where the frame arrives (19.16)
        case QuicFrameKind::PathResponse:       // this side sent no PATH_CHALLENGE (section 19.18's MAY taken)
        case QuicFrameKind::Datagram:           // max_datagram_frame_size is 0 (RFC 9221 section 3)
            fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
            return false;
    }
    return true;
}

bool QuicConnection::crypto_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept
{
    Level& level = m_levels[index_of(space)];
    switch (level.in.insert(frame.offset, frame.data, false))
    {
        case QuicReceiveResult::Accepted:
            break;
        case QuicReceiveResult::BeyondWindow:
            fail(QuicTransportError::CryptoBufferExceeded, kCryptoFrameType, now_ns);
            return false;
        case QuicReceiveResult::FinalSize:
        case QuicReceiveResult::Changed:
        case QuicReceiveResult::Unusable:
            fail(QuicTransportError::ProtocolViolation, kCryptoFrameType, now_ns);
            return false;
    }
    for (cont::ConstSpan<u8> readable = level.in.readable(); !readable.empty(); readable = level.in.readable())
    {
        const crypto::TlsError error = m_handshake.provide(level_of(space), readable);
        if (error == crypto::TlsError::Full)
        {
            fail(QuicTransportError::CryptoBufferExceeded, kCryptoFrameType, now_ns);
            return false;
        }
        if (error != crypto::TlsError::None)
        {
            const crypto::TlsAlert alert = m_handshake.alert_sent();
            if (alert.present)
            {
                fail_code(crypto::quic_crypto_error(alert.description), kCryptoFrameType, m_handshake.alert_level(),
                          now_ns);
            }
            else
            {
                fail(QuicTransportError::ProtocolViolation, kCryptoFrameType, now_ns);
            }
            return false;
        }
        level.in.consume(readable.size());
    }
    return advance_handshake(now_ns);
}

bool QuicConnection::advance_handshake(u64 now_ns) noexcept
{
    if (m_protection.install(m_handshake) != crypto::CryptoError::None)
    {
        fail(QuicTransportError::InternalError, 0U, now_ns);
        return false;
    }
    if (!m_handshake_keys && m_protection.can_send(QuicPacketType::Handshake))
    {
        m_handshake_keys = true;
        m_recovery->set_handshake_keys(now_ns);
    }
    if (!m_peer_known && !m_handshake.peer_transport_parameters().empty() && !read_peer_parameters(now_ns))
    {
        return false;
    }
    if (m_state == QuicConnectionState::Handshaking && m_handshake.state() == crypto::TlsState::Established)
    {
        m_state = QuicConnectionState::Established;
        if (m_config.side == crypto::QuicSide::Server)
        {
            // A server's handshake is confirmed when it completes (RFC 9001 section 4.1.2); it says so.
            m_handshake_done_owed = true;
            confirm(now_ns);
        }
    }
    return true;
}

bool QuicConnection::read_peer_parameters(u64 now_ns) noexcept
{
    const crypto::QuicSide peer = other(m_config.side);
    QuicTransportError error = read_quic_transport_parameters(m_handshake.peer_transport_parameters(), peer, m_peer);
    if (error == QuicTransportError::NoError)
    {
        QuicConnectionIdExpectation expected;
        expected.peer_source = m_peer_scid.view();
        if (m_config.side == crypto::QuicSide::Client)
        {
            expected.original_destination = m_original_dcid.view();
        }
        error = quic_check_connection_ids(m_peer, peer, expected);
    }
    if (error == QuicTransportError::NoError)
    {
        const u32 version = static_cast<u32>(m_config.version);
        error = (m_config.side == crypto::QuicSide::Server)
                    ? quic_server_check_version_information(m_peer, version)
                    : quic_client_check_version_information(m_peer, version, m_config.after_version_negotiation,
                                                            cont::ConstSpan<u32>(m_versions, m_version_count));
    }
    if (error != QuicTransportError::NoError)
    {
        fail(error, 0U, now_ns);
        return false;
    }
    m_peer_known = true;
    m_streams.set_peer_parameters(m_peer);
    m_recovery->set_peer_parameters(m_peer.max_ack_delay * kNsPerMs, m_peer.ack_delay_exponent, now_ns);
    m_peer_greases = m_peer.grease_quic_bit;
    const u64 local_idle = m_local.max_idle_timeout;
    const u64 peer_idle = m_peer.max_idle_timeout;
    const u64 idle_ms =
        (local_idle == 0U || peer_idle == 0U) ? max_u64(local_idle, peer_idle) : min_u64(local_idle, peer_idle);
    m_idle_ns = idle_ms * kNsPerMs;
    if (m_peer.has_stateless_reset_token)
    {
        std::memcpy(m_peer_cids[0].reset_token, m_peer.stateless_reset_token, kQuicResetTokenBytes);
        m_peer_cids[0].has_token = true;
    }
    restart_idle(now_ns);
    return true;
}

bool QuicConnection::owe_retire(u64 sequence, u64 now_ns) noexcept
{
    // Retirements owed and in flight stay within twice the limit (section 5.1.2).
    if (m_retire_outstanding >= 2U * m_local.active_connection_id_limit)
    {
        fail(QuicTransportError::ConnectionIdLimitError, 0U, now_ns);
        return false;
    }
    m_retire_owed[m_retire_owed_count++] = sequence;
    ++m_retire_outstanding;
    return true;
}

bool QuicConnection::new_connection_id(const QuicFrame& frame, u64 now_ns) noexcept
{
    if (m_peer_scid.bytes == 0U)
    {
        // A peer sending to zero-length IDs cannot issue others (section 19.15).
        fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
        return false;
    }
    for (usize index = 0U; index < m_peer_cid_count; ++index)
    {
        const PeerCid& held = m_peer_cids[index];
        const bool same_id = same_bytes(held.id.view(), frame.data);
        if (held.sequence == frame.sequence)
        {
            // A repeat changes nothing; another ID or token under a sequence held is PROTOCOL_VIOLATION.
            if (!same_id ||
                (held.has_token && std::memcmp(held.reset_token, frame.reset_token.data(), kQuicResetTokenBytes) != 0))
            {
                fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
                return false;
            }
            return true;
        }
        if (same_id)
        {
            fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
            return false;
        }
    }
    if (frame.sequence < m_retire_prior_to)
    {
        return owe_retire(frame.sequence, now_ns); // already retired by an earlier Retire Prior To
    }
    if (frame.retire_prior_to > m_retire_prior_to)
    {
        m_retire_prior_to = frame.retire_prior_to;
        const u64 current = m_peer_cids[m_peer_cid_current].sequence;
        usize kept = 0U;
        for (usize index = 0U; index < m_peer_cid_count; ++index)
        {
            if (m_peer_cids[index].sequence < m_retire_prior_to)
            {
                if (!owe_retire(m_peer_cids[index].sequence, now_ns))
                {
                    return false;
                }
                continue;
            }
            m_peer_cids[kept++] = m_peer_cids[index];
        }
        m_peer_cid_count = kept;
        m_peer_cid_current = kQuicMaxPeerConnectionIds;
        for (usize index = 0U; index < m_peer_cid_count; ++index)
        {
            if (m_peer_cids[index].sequence == current)
            {
                m_peer_cid_current = index;
            }
        }
    }
    if (m_peer_cid_count >= m_local.active_connection_id_limit)
    {
        fail(QuicTransportError::ConnectionIdLimitError, 0U, now_ns);
        return false;
    }
    PeerCid& added = m_peer_cids[m_peer_cid_count];
    added.sequence = frame.sequence;
    static_cast<void>(added.id.assign(frame.data)); // read_quic_frame bounds it to 20 bytes
    std::memcpy(added.reset_token, frame.reset_token.data(), kQuicResetTokenBytes);
    added.has_token = true;
    if (m_peer_cid_current == kQuicMaxPeerConnectionIds)
    {
        m_peer_cid_current = m_peer_cid_count; // the ID in use was retired: the new one replaces it
    }
    ++m_peer_cid_count;
    return true;
}

void QuicConnection::version_negotiation(const QuicHeader& header, cont::ConstSpan<u8> packet) noexcept
{
    // Only a client before any packet of its server opened, for the IDs it sent (RFC 9000 section 6.2).
    if (m_config.side != crypto::QuicSide::Client || m_peer_scid_known ||
        !same_bytes(header.dcid, m_local_cid.view()) || !same_bytes(header.scid, m_initial_dcid.view()))
    {
        ++m_counters.packets_dropped;
        return;
    }
    const usize list = 1U + 4U + 1U + header.dcid.size() + 1U + header.scid.size();
    if (packet.size() < list + 4U || (packet.size() - list) % 4U != 0U)
    {
        ++m_counters.packets_dropped;
        return;
    }
    u32 offered[kQuicMaxNegotiationVersions + 1U];
    usize count = 0U;
    for (usize at = list; at < packet.size(); at += 4U)
    {
        const u32 version = (u32{packet[at]} << 24U) | (u32{packet[at + 1U]} << 16U) | (u32{packet[at + 2U]} << 8U) |
                            u32{packet[at + 3U]};
        if (version == static_cast<u32>(m_config.version))
        {
            ++m_counters.packets_dropped; // it lists the version this connection runs: not a real answer
            return;
        }
        if (count < sizeof(offered) / sizeof(offered[0]))
        {
            offered[count++] = version;
        }
    }
    m_close = QuicCloseInfo{};
    m_close.version_negotiation = true;
    m_close.retry_version =
        quic_select_version(cont::ConstSpan<u32>(m_versions, m_version_count), cont::ConstSpan<u32>(offered, count));
    m_state = QuicConnectionState::Closed;
}

void QuicConnection::discard_space(QuicSpace space, u64 now_ns) noexcept
{
    const usize index = index_of(space);
    if (m_space_gone[index])
    {
        return;
    }
    m_space_gone[index] = true;
    m_probes[index] = 0U;
    static_cast<void>(m_recovery->discard(space, now_ns, m_events));
}

void QuicConnection::confirm(u64 now_ns) noexcept
{
    if (m_confirmed)
    {
        return;
    }
    m_confirmed = true;
    m_protection.confirm();
    m_recovery->confirm_handshake(now_ns);
    discard_space(QuicSpace::Handshake, now_ns);
}

// ---- Sending ----

bool QuicConnection::can_acknowledge(QuicSpace space) const noexcept
{
    return !m_space_gone[index_of(space)] && m_protection.can_send(packet_type(space)) && m_recovery->can_send(space);
}

usize QuicConnection::amplification_credit() const noexcept
{
    if (m_validated)
    {
        return ~usize{0};
    }
    const u64 allowed = 3U * m_counters.bytes_received;
    return (allowed > m_counters.bytes_sent) ? static_cast<usize>(allowed - m_counters.bytes_sent) : 0U;
}

void QuicConnection::pull_crypto() noexcept
{
    for (usize index = 0U; index < kQuicSpaces; ++index)
    {
        const crypto::QuicLevel level = level_of(static_cast<QuicSpace>(index));
        QuicSendBuffer& out = m_levels[index].out;
        while (m_handshake.pending(level) > 0U && out.room() > 0U)
        {
            u8 chunk[kPullChunk];
            const usize want = min_u64(min_u64(m_handshake.pending(level), out.room()), sizeof(chunk));
            usize taken = 0U;
            if (m_handshake.take(level, cont::Span<u8>(chunk, want), taken) != crypto::TlsError::None || taken == 0U)
            {
                break;
            }
            static_cast<void>(out.write(cont::ConstSpan<u8>(chunk, taken))); // room() held the bytes
        }
    }
}

bool QuicConnection::has_content(QuicSpace space, u64 now_ns) const noexcept
{
    const usize index = index_of(space);
    if (m_space_gone[index] || !m_protection.can_send(packet_type(space)) || !m_recovery->can_send(space))
    {
        return false;
    }
    if (m_recovery->ack_deadline_ns(space) <= now_ns || m_levels[index].out.pending() || m_probes[index] > 0U)
    {
        return true;
    }
    return space == QuicSpace::Application &&
           (m_handshake_done_owed || m_path_response_owed || m_ping_owed || m_retire_owed_count > 0U ||
            (m_frame_count < m_config.sent_frames && m_streams.has_frame()));
}

bool QuicConnection::record(QuicSpace space, u64 packet, u8 kind, const QuicStreamRecord& frame,
                            Written& written) noexcept
{
    if (m_frame_count == m_config.sent_frames)
    {
        return false;
    }
    const u32 at = (m_frame_head + m_frame_count) % m_config.sent_frames;
    SentFrame& sent = m_frames[at];
    sent.packet = packet;
    sent.frame = frame;
    sent.space = static_cast<u8>(space);
    sent.kind = kind;
    written.token = (written.token == kNoRecord) ? at : written.token;
    ++m_frame_count;
    return true;
}

void QuicConnection::settle(QuicSpace space, const QuicSentPacket& packet, Settle how) noexcept
{
    if (packet.token != kNoRecord)
    {
        for (u32 at = packet.token, walked = 0U; walked < m_config.sent_frames;
             at = (at + 1U) % m_config.sent_frames, ++walked)
        {
            SentFrame& frame = m_frames[at];
            if (frame.kind == 0U || frame.packet != packet.number || frame.space != static_cast<u8>(space))
            {
                break;
            }
            if (frame.kind == kRecordCrypto)
            {
                QuicSendBuffer& out = m_levels[frame.space].out;
                if (how == Settle::Acked)
                {
                    out.on_acked(frame.frame.value, frame.frame.length, false);
                }
                else if (how == Settle::Lost)
                {
                    out.on_lost(frame.frame.value, frame.frame.length, false);
                }
            }
            else if (frame.kind == kRecordStream)
            {
                if (how == Settle::Acked)
                {
                    m_streams.on_acked(frame.frame);
                }
                else if (how == Settle::Lost)
                {
                    m_streams.on_lost(frame.frame);
                }
            }
            else if (frame.kind == kRecordHandshakeDone)
            {
                m_handshake_done_acked = m_handshake_done_acked || how == Settle::Acked;
                m_handshake_done_owed = !m_handshake_done_acked && (m_handshake_done_owed || how == Settle::Lost);
            }
            else
            {
                if (how == Settle::Lost)
                {
                    m_retire_owed[m_retire_owed_count++] = frame.frame.value; // outstanding, so it fits
                }
                else
                {
                    --m_retire_outstanding;
                }
            }
            frame.kind = 0U;
        }
    }
    while (m_frame_count > 0U && m_frames[m_frame_head].kind == 0U)
    {
        m_frame_head = (m_frame_head + 1U) % m_config.sent_frames;
        --m_frame_count;
    }
}

usize QuicConnection::write_frames(QuicSpace space, cont::Span<u8> frames, u64 packet, u64 now_ns,
                                   Written& written) noexcept
{
    const usize index = index_of(space);
    usize used = 0U;
    const auto put = [&](const QuicFrame& frame) -> bool
    {
        const usize bytes = write_quic_frame(frame, frames.subspan(used));
        used += bytes;
        written.eliciting = written.eliciting || (bytes != 0U && quic_frame_ack_eliciting(frame.kind));
        return bytes != 0U;
    };
    if (!m_recovery->received(space).ranges().empty())
    {
        const QuicAckWrite ack = m_recovery->write_ack(space, frames, now_ns);
        used += ack.bytes;
        written.carried_ack = (ack.bytes != 0U) ? ack.largest : kQuicNoPacket;
    }
    if (space == QuicSpace::Application)
    {
        if (m_handshake_done_owed && m_frame_count < m_config.sent_frames)
        {
            QuicFrame done;
            done.kind = QuicFrameKind::HandshakeDone;
            if (put(done))
            {
                static_cast<void>(record(space, packet, kRecordHandshakeDone, QuicStreamRecord{}, written));
                m_handshake_done_owed = false;
            }
        }
        if (m_path_response_owed)
        {
            QuicFrame response;
            response.kind = QuicFrameKind::PathResponse;
            response.data = cont::ConstSpan<u8>(m_path_response, kQuicPathDataBytes);
            if (put(response))
            {
                m_path_response_owed = false;
                written.path_response = true;
            }
        }
        while (m_retire_owed_count > 0U && m_frame_count < m_config.sent_frames)
        {
            QuicFrame retire;
            retire.kind = QuicFrameKind::RetireConnectionId;
            retire.sequence = m_retire_owed[m_retire_owed_count - 1U];
            if (!put(retire))
            {
                break;
            }
            QuicStreamRecord retired;
            retired.value = retire.sequence;
            static_cast<void>(record(space, packet, kRecordRetire, retired, written));
            --m_retire_owed_count;
        }
    }
    QuicSendBuffer& out = m_levels[index].out;
    while (m_frame_count < m_config.sent_frames)
    {
        // CRYPTO's type, offset and length take at most 1 + 8 + 4 bytes of a datagram-sized packet.
        const usize left = frames.size() - used;
        const QuicSendChunk probe = out.next(left, kQuicMaxVarint);
        if (probe.length == 0U)
        {
            break;
        }
        const usize header = 1U + quic_varint_bytes(probe.offset) + quic_varint_bytes(left);
        if (left <= header)
        {
            break;
        }
        const QuicSendChunk chunk = out.next(left - header, kQuicMaxVarint);
        QuicFrame data;
        data.kind = QuicFrameKind::Crypto;
        data.offset = chunk.offset;
        data.data = out.bytes_of(chunk);
        if (!put(data))
        {
            break;
        }
        QuicStreamRecord sent;
        sent.value = chunk.offset;
        sent.length = static_cast<u32>(chunk.length);
        static_cast<void>(record(space, packet, kRecordCrypto, sent, written));
        out.on_sent(chunk);
    }
    if (space == QuicSpace::Application)
    {
        // The stream set's frames after the connection's own; each fits the room it was offered.
        const u64 rtt_ns = m_recovery->rtt().smoothed_ns;
        QuicFrame frame;
        QuicStreamRecord taken;
        while (m_frame_count < m_config.sent_frames &&
               m_streams.next_frame(frames.size() - used, now_ns, rtt_ns, frame, taken))
        {
            if (!put(frame))
            {
                m_streams.on_lost(taken); // owed again (a frame next_frame sized never fails to fit)
                break;
            }
            static_cast<void>(record(space, packet, kRecordStream, taken, written));
        }
    }
    if (space == QuicSpace::Application && m_ping_owed)
    {
        m_ping_owed = false;
        if (!written.eliciting)
        {
            QuicFrame ping;
            ping.kind = QuicFrameKind::Ping;
            static_cast<void>(put(ping));
        }
    }
    // A probe owes an ack-eliciting packet, and so does a run of ACK-only packets: those are never acknowledged
    // by themselves, so a PING lets the peer's ACK free their storage (RFC 9000 section 13.2.4).
    if (!written.eliciting && (m_probes[index] > 0U || m_ack_only[index] >= kAckOnlyRun))
    {
        QuicFrame ping;
        ping.kind = QuicFrameKind::Ping;
        static_cast<void>(put(ping));
    }
    return used;
}

usize QuicConnection::write_close(QuicSpace space, cont::Span<u8> frames) const noexcept
{
    QuicFrame close;
    close.kind = QuicFrameKind::ConnectionClose;
    close.error_code = m_close.error_code;
    close.frame_type = m_close.frame_type;
    close.data = cont::ConstSpan<u8>(m_reason, m_reason_bytes);
    close.application = m_close.application;
    if (m_close.application && space != QuicSpace::Application)
    {
        // An application close outside 1-RTT packets would reveal application state (section 10.2.3).
        close.application = false;
        close.error_code = static_cast<u64>(QuicTransportError::ApplicationError);
        close.frame_type = 0U;
        close.data = {};
    }
    return write_quic_frame(close, frames);
}

usize QuicConnection::write_packet(QuicSpace space, cont::Span<u8> room, usize datagram_used, bool pad, bool closing,
                                   u64 now_ns) noexcept
{
    const usize index = index_of(space);
    u64 number = m_next_number[index];
    bool skip = false;
    if (space == QuicSpace::Application && !closing && (entropy() & kSkipMask) == 0U && number < kQuicMaxVarint)
    {
        skip = true;
        ++number;
    }
    QuicPacketSpec spec;
    spec.type = packet_type(space);
    spec.version = static_cast<u32>(m_config.version);
    spec.dcid = peer_cid();
    spec.scid = m_local_cid.view();
    if (space == QuicSpace::Initial && m_config.side == crypto::QuicSide::Client)
    {
        spec.token = cont::ConstSpan<u8>(m_token, m_token_bytes);
    }
    spec.number = number;
    const u64 largest = m_recovery->largest_acked(space);
    spec.acknowledged = largest != kQuicNoPacket;
    spec.largest_acked = spec.acknowledged ? largest : 0U;
    spec.clear_quic_bit = quic_grease_bit_clear(m_peer_greases, entropy());
    QuicPacketWriter writer;
    if (!writer.begin(spec, room))
    {
        return 0U;
    }
    Written written;
    const usize used =
        closing ? write_close(space, writer.frames()) : write_frames(space, writer.frames(), number, now_ns, written);
    const bool padded = pad || written.path_response;
    if (used == 0U && !padded)
    {
        return 0U;
    }
    const usize floor = padded ? min_u64(quic_datagram_floor(true, datagram_used), room.size()) : 0U;
    usize bytes = 0U;
    const QuicPacketError sealed = m_protection.seal(writer, used, QuicPadding{floor, 0U}, bytes);
    if (sealed != QuicPacketError::None)
    {
        const QuicTransportError error = quic_packet_close(sealed);
        fail((error == QuicTransportError::NoError) ? QuicTransportError::InternalError : error, 0U, now_ns);
        return 0U;
    }
    m_skipped[index] = skip ? number - 1U : m_skipped[index];
    m_next_number[index] = number + 1U;
    if (!closing)
    {
        QuicSentPacket sent;
        sent.number = number;
        sent.sent_ns = now_ns;
        sent.carried_ack = written.carried_ack;
        sent.token = written.token;
        sent.bytes = static_cast<u32>(bytes);
        sent.ack_eliciting = written.eliciting;
        sent.in_flight = written.eliciting || floor != 0U;
        static_cast<void>(m_recovery->on_sent(space, sent)); // can_send held and numbers rise
        if (written.eliciting && m_probes[index] > 0U)
        {
            --m_probes[index];
        }
        m_ack_only[index] = written.eliciting ? 0U : m_ack_only[index] + 1U;
        if (written.eliciting && !m_sent_since_receive)
        {
            m_sent_since_receive = true;
            restart_idle(now_ns);
        }
    }
    if (space == QuicSpace::Application && written.carried_ack != kQuicNoPacket)
    {
        m_protection.sent_acknowledgement();
    }
    if (!m_protection.can_send(QuicPacketType::Initial))
    {
        discard_space(QuicSpace::Initial, now_ns); // a client's first Handshake packet ends its Initial keys
    }
    return bytes;
}

usize QuicConnection::send_closing(cont::Span<u8> out, u64 now_ns) noexcept
{
    if (!m_close_owed)
    {
        return 0U;
    }
    m_close_owed = false;
    const usize highest = index_of(space_of(m_close_level));
    usize used = 0U;
    for (usize index = 0U; index <= highest; ++index)
    {
        const QuicSpace space = static_cast<QuicSpace>(index);
        // Before confirmation every level the peer may still read; after it, 1-RTT alone (section 10.2.3).
        if ((m_confirmed && space != QuicSpace::Application) || m_space_gone[index] ||
            !m_protection.can_send(packet_type(space)))
        {
            continue;
        }
        const bool pad = m_config.side == crypto::QuicSide::Client && space == QuicSpace::Initial;
        bool later = false;
        for (usize next = index + 1U; next <= highest; ++next)
        {
            later = later || (!m_space_gone[next] && m_protection.can_send(packet_type(static_cast<QuicSpace>(next))));
        }
        used += write_packet(space, out.subspan(used), used, pad && !later, true, now_ns);
    }
    return used;
}

usize QuicConnection::send(cont::Span<u8> out, u64 now_ns) noexcept
{
    if (out.size() < kQuicMinInitialDatagram || m_state == QuicConnectionState::Idle ||
        m_state == QuicConnectionState::Draining || m_state == QuicConnectionState::Closed)
    {
        return 0U;
    }
    usize limit = min_u64(out.size(), m_config.max_datagram_bytes);
    if (m_peer_known)
    {
        limit = min_u64(limit, m_peer.max_udp_payload_size);
    }
    const usize credit = amplification_credit();
    limit = min_u64(limit, credit);
    const cont::Span<u8> room = out.first(limit);
    usize used = 0U;
    if (m_state == QuicConnectionState::Closing)
    {
        used = (limit >= kMinUsefulPacket) ? send_closing(room, now_ns) : 0U;
    }
    else
    {
        pull_crypto();
        bool content[kQuicSpaces] = {};
        QuicSpace last = QuicSpace::Initial;
        bool any = false;
        for (usize index = 0U; index < kQuicSpaces; ++index)
        {
            content[index] = has_content(static_cast<QuicSpace>(index), now_ns);
            if (content[index])
            {
                last = static_cast<QuicSpace>(index);
                any = true;
            }
        }
        const usize initial = index_of(QuicSpace::Initial);
        // A client pads every datagram with an Initial, a server those with an ack-eliciting one (section 14.1).
        const bool pad = content[initial] && (m_config.side == crypto::QuicSide::Client ||
                                              m_levels[initial].out.pending() || m_probes[initial] > 0U);
        if (!any || limit < kMinUsefulPacket || (pad && limit < kQuicMinInitialDatagram))
        {
            if (any && !m_validated)
            {
                m_recovery->set_amplification_blocked(true, now_ns);
            }
            return 0U;
        }
        for (usize index = 0U; index < kQuicSpaces && m_state != QuicConnectionState::Closing; ++index)
        {
            const QuicSpace space = static_cast<QuicSpace>(index);
            if (content[index])
            {
                used += write_packet(space, room.subspan(used), used, pad && space == last, false, now_ns);
            }
        }
        if (m_state == QuicConnectionState::Closing)
        {
            return 0U; // a seal failed: the close goes out on the next call
        }
    }
    if (used != 0U)
    {
        ++m_counters.datagrams_sent;
        m_counters.bytes_sent += used;
        if (!m_validated)
        {
            m_recovery->set_amplification_blocked(amplification_credit() < kMinUsefulPacket, now_ns);
        }
    }
    return used;
}

// ---- Timers and close ----

u64 QuicConnection::three_ptos() const noexcept
{
    const u64 period = m_recovery->probe_period_ns(QuicSpace::Application);
    return (period > kNeverNs / 3U) ? kNeverNs : 3U * period;
}

void QuicConnection::restart_idle(u64 now_ns) noexcept
{
    m_idle_deadline_ns = (m_idle_ns == 0U) ? kNeverNs : add_saturating(now_ns, max_u64(m_idle_ns, three_ptos()));
}

u64 QuicConnection::next_timer_ns() const noexcept
{
    if (m_state == QuicConnectionState::Closing || m_state == QuicConnectionState::Draining)
    {
        return m_close_deadline_ns;
    }
    if (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established)
    {
        return kNeverNs;
    }
    // An owed ACK counts only where send() can write it: never one held back by the amplification limit, the
    // space's full storage or keys not yet installed, which would report a timer due that nothing serves.
    u64 next = min_u64(min_u64(m_recovery->loss_timer_ns(), m_idle_deadline_ns), m_previous_key_ns);
    for (usize index = 0U; index < kQuicSpaces; ++index)
    {
        const QuicSpace space = static_cast<QuicSpace>(index);
        if (can_acknowledge(space))
        {
            next = min_u64(next, m_recovery->ack_deadline_ns(space));
        }
    }
    return next;
}

void QuicConnection::on_timeout(u64 now_ns) noexcept
{
    if (m_state == QuicConnectionState::Closing || m_state == QuicConnectionState::Draining)
    {
        if (now_ns >= m_close_deadline_ns)
        {
            m_state = QuicConnectionState::Closed;
        }
        return;
    }
    if (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established)
    {
        return;
    }
    if (now_ns >= m_idle_deadline_ns)
    {
        m_close = QuicCloseInfo{};
        m_close.idle = true;
        m_state = QuicConnectionState::Closed;
        return;
    }
    if (now_ns >= m_previous_key_ns)
    {
        m_protection.discard_previous();
        m_previous_key_ns = kNeverNs;
    }
    if (now_ns < m_recovery->loss_timer_ns())
    {
        return;
    }
    const QuicTimeout timeout = m_recovery->on_timeout(now_ns, m_events);
    if (timeout.kind != QuicTimeoutKind::Probe && timeout.kind != QuicTimeoutKind::AntiDeadlock)
    {
        return;
    }
    const usize index = index_of(timeout.space);
    m_probes[index] = (timeout.kind == QuicTimeoutKind::Probe) ? timeout.probes : u8{1};
    // A probe carries the data still unacknowledged (RFC 9002 section 6.2.4): it goes again as if lost.
    QuicSendBuffer& out = m_levels[index].out;
    if (out.sent_end() > out.acked_end())
    {
        out.on_lost(out.acked_end(), static_cast<usize>(out.sent_end() - out.acked_end()), false);
    }
    if (timeout.space == QuicSpace::Application && m_config.side == crypto::QuicSide::Server && m_confirmed &&
        !m_handshake_done_acked)
    {
        m_handshake_done_owed = true;
    }
}

void QuicConnection::enter(QuicConnectionState state, u64 now_ns) noexcept
{
    m_state = state;
    m_close_deadline_ns = add_saturating(now_ns, three_ptos());
    m_close_owed = state == QuicConnectionState::Closing;
    m_close_owed_after = 1U;
    m_close_received = 0U;
}

void QuicConnection::fail(QuicTransportError error, u64 frame_type, u64 now_ns) noexcept
{
    crypto::QuicLevel level = crypto::QuicLevel::Initial;
    if (m_protection.can_send(QuicPacketType::OneRtt))
    {
        level = crypto::QuicLevel::Application;
    }
    else if (m_protection.can_send(QuicPacketType::Handshake))
    {
        level = crypto::QuicLevel::Handshake;
    }
    fail_code(static_cast<u64>(error), frame_type, level, now_ns);
}

void QuicConnection::fail_code(u64 error, u64 frame_type, crypto::QuicLevel level, u64 now_ns) noexcept
{
    if (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established)
    {
        return;
    }
    m_close = QuicCloseInfo{};
    m_close.local = true;
    m_close.error_code = error;
    m_close.frame_type = frame_type;
    m_reason_bytes = 0U;
    m_close_level = level;
    enter(QuicConnectionState::Closing, now_ns);
}

void QuicConnection::close(u64 application_error, cont::ConstSpan<u8> reason, u64 now_ns) noexcept
{
    if (m_state == QuicConnectionState::Idle)
    {
        m_state = QuicConnectionState::Closed;
        return;
    }
    if (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established)
    {
        return;
    }
    fail(QuicTransportError::NoError, 0U, now_ns);
    m_close.application = true;
    m_close.error_code = application_error;
    m_reason_bytes = (reason.size() < kQuicMaxReasonBytes) ? reason.size() : kQuicMaxReasonBytes;
    if (m_reason_bytes != 0U)
    {
        std::memcpy(m_reason, reason.data(), m_reason_bytes);
    }
}

// ---- State ----

cont::ConstSpan<u8> QuicConnection::peer_cid() const noexcept
{
    if (m_peer_cid_count == 0U)
    {
        return m_initial_dcid.view(); // a client before its server's first packet
    }
    return m_peer_cids[m_peer_cid_current].id.view();
}

u64 QuicConnection::next_packet_number(QuicSpace space) const noexcept
{
    return m_next_number[index_of(space)];
}

bool QuicConnection::skipped(QuicSpace space, u64 number) const noexcept
{
    return m_skipped[index_of(space)] == number;
}

} // namespace crd::net
