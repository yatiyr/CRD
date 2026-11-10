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
constexpr u8 kRecordStream = 4U;     // a frame of the stream set
constexpr u8 kRecordDatagram = 5U;   // a DATAGRAM frame whose outcome is reported
constexpr u8 kRecordNewId = 6U;      // a NEW_CONNECTION_ID frame: value is its sequence
constexpr u8 kRecordNewToken = 7U;   // a server's NEW_TOKEN frame
constexpr usize kIssueAttempts = 4U; // random IDs a host draws before it gives up on a full table
constexpr u32 kNoRecord = ~u32{0};
constexpr u64 kNsPerMs = 1000000U;
constexpr u64 kCryptoFrameType = 0x06U;
constexpr usize kMinCryptoBytes = 4096U; // RFC 9000 section 7.5
constexpr usize kMaxCryptoBytes = 1U << 20U;
constexpr u32 kMaxSentPackets = 1U << 16U;
// Past sent_packets, per space: what ACK-only packets and probes have when data has filled the rest.
constexpr u32 kSentPacketReserve = 8U;
constexpr usize kMinUsefulPacket = 64U; // a server with less credit than this waits for a datagram
constexpr usize kPullChunk = 512U;
constexpr usize kKeyedHelloPrefix = 2U; // a keyed connection's parameters follow their two-byte length
constexpr u64 kSkipMask = 63U;   // one 1-RTT packet number in 64 is skipped (section 21.4)
constexpr u32 kAckOnlyRun = 16U; // ACK-only packets in a row before one carries a PING

// What a congestion controller sees of a packet counted in flight.
[[nodiscard]] CongestionPacket congestion_packet(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    CongestionPacket out;
    out.bytes = packet.bytes;
    out.number = packet.number;
    out.sent_ns = packet.sent_ns;
    out.space = space;
    return out;
}

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
    // Without a TLS context the connection can only be opened keyed: nothing of a handshake is configured.
    const bool keyed_ok =
        config.tls != nullptr || (config.tickets == nullptr && config.token.empty() && config.early_context.empty() &&
                                  config.versions.empty() && !config.after_version_negotiation);
    return keyed_ok && version_ok && listed && config.versions.size() < kQuicMaxAvailableVersions &&
           p.initial_max_data == 0U && p.initial_max_stream_data_bidi_local == 0U &&
           p.initial_max_stream_data_bidi_remote == 0U && p.initial_max_stream_data_uni == 0U &&
           p.initial_max_streams_bidi == 0U && p.initial_max_streams_uni == 0U && p.max_datagram_frame_size == 0U &&
           !p.has_stateless_reset_token && QuicStreamSet::limits_valid(config.streams) &&
           QuicDatagrams::limits_valid(config.datagrams) && p.active_connection_id_limit >= kQuicDefaultActiveIdLimit &&
           p.active_connection_id_limit <= kQuicMaxPeerConnectionIds && config.sent_packets != 0U &&
           config.sent_packets <= kMaxSentPackets && config.sent_frames != 0U &&
           config.sent_frames <= kMaxSentPackets && config.crypto_bytes % kQuicStreamChunk == 0U &&
           config.crypto_bytes >= kMinCryptoBytes && config.crypto_bytes <= kMaxCryptoBytes &&
           config.max_datagram_bytes >= kQuicMinInitialDatagram &&
           config.max_datagram_bytes <= kQuicDefaultMaxUdpPayload && congestion_policy_valid(config.congestion) &&
           (config.entropy_seed.empty() || config.entropy_seed.size() == crypto::kPrfKeyBytes) &&
           (config.token.size() <= kQuicMaxCarriedTokenBytes) && config.held_packets <= kQuicMaxHeldPackets &&
           (config.early_context.empty() || (config.side == crypto::QuicSide::Server && config.tls->early_data() &&
                                             config.early_context.size() <= kQuicMaxEarlyContextBytes));
}

[[nodiscard]] usize early_context_bytes(const QuicConnectionConfig& config) noexcept
{
    const bool accepts = config.side == crypto::QuicSide::Server && config.tls != nullptr && config.tls->early_data();
    return accepts ? kQuicEarlyParameterBytes + config.early_context.size() : 0U;
}
} // namespace

// ---- The host side of connection IDs ----

bool QuicRoutedIds::issue(cont::Span<u8> id, cont::Span<u8> token) noexcept
{
    if (id.size() < 8U || id.size() > crypto::kQuicMaxConnectionIdBytes)
    {
        return false;
    }
    for (usize attempt = 0U; attempt < kIssueAttempts; ++attempt)
    {
        if (crypto::random_bytes(id) != crypto::CryptoError::None)
        {
            return false;
        }
        const RouteError routed = m_routes.insert(id, m_connection);
        if (routed == RouteError::Exists)
        {
            continue;
        }
        if (routed != RouteError::None)
        {
            return false;
        }
        if (!this->token(id, token))
        {
            static_cast<void>(m_routes.erase(id));
            return false;
        }
        return true;
    }
    return false;
}

bool QuicRoutedIds::token(cont::ConstSpan<u8> id, cont::Span<u8> token) noexcept
{
    return m_reset.token(id, token) == crypto::CryptoError::None;
}

void QuicRoutedIds::retire(cont::ConstSpan<u8> id) noexcept
{
    static_cast<void>(m_routes.erase(id));
}

void QuicConnectionIds::watch_reset(cont::ConstSpan<u8>) noexcept {}

void QuicConnectionIds::unwatch_reset(cont::ConstSpan<u8>) noexcept {}

void QuicRoutedIds::watch_reset(cont::ConstSpan<u8> token) noexcept
{
    if (m_peer_tokens != nullptr)
    {
        static_cast<void>(m_peer_tokens->insert(token, m_connection)); // a token held already stays its first holder's
    }
}

void QuicRoutedIds::unwatch_reset(cont::ConstSpan<u8> token) noexcept
{
    if (m_peer_tokens != nullptr && m_peer_tokens->find(token) == m_connection)
    {
        static_cast<void>(m_peer_tokens->erase(token));
    }
}

// ---- Events ----

void QuicConnection::Events::on_acked(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Acked);
    if (packet.in_flight)
    {
        m_connection.m_congestion->on_acked(congestion_packet(space, packet), m_connection.m_event_ns);
    }
}

void QuicConnection::Events::on_lost(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Lost);
    if (packet.in_flight)
    {
        m_connection.m_congestion->on_lost(congestion_packet(space, packet), m_connection.m_event_ns);
    }
}

void QuicConnection::Events::on_spurious(QuicSpace space, const QuicSentPacket& packet, u64 lost_ns) noexcept
{
    if (packet.in_flight)
    {
        m_connection.m_congestion->on_spurious(congestion_packet(space, packet), lost_ns, m_connection.m_event_ns);
    }
}

void QuicConnection::Events::on_discarded(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Discarded);
}

// ---- Construction ----

QuicConnection::QuicConnection(memory::IAllocator& allocator, const QuicConnectionConfig& config) noexcept
    : m_allocator(allocator), m_config(config), m_events(*this), m_hello(*this)
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

    const usize per_space = usize{config.sent_packets} + kSentPacketReserve;
    const usize crypto = config.crypto_bytes;
    const usize words = quic_stream_bitmap_words(crypto);
    QuicSentEntry* const entries = memory::try_construct_array<QuicSentEntry>(allocator, kQuicSpaces * per_space);
    SentFrame* const frames = memory::try_construct_array<SentFrame>(allocator, config.sent_frames);
    u8* const bytes = memory::try_construct_array<u8>(allocator, kQuicSpaces * 2U * crypto);
    u64* const bits = memory::try_construct_array<u64>(allocator, kQuicSpaces * 3U * words);
    const bool streams = m_streams.init(allocator, config.streams, config.side);
    const bool datagrams = m_datagrams.init(allocator, config.datagrams);
    const usize early_bytes = early_context_bytes(config);
    const usize held_bytes = usize{config.held_packets} * config.max_datagram_bytes;
    u8* const early = (early_bytes != 0U) ? memory::try_construct_array<u8>(allocator, early_bytes) : nullptr;
    u8* const held = (held_bytes != 0U) ? memory::try_construct_array<u8>(allocator, held_bytes) : nullptr;
    Held* const held_entries =
        (held_bytes != 0U) ? memory::try_construct_array<Held>(allocator, config.held_packets) : nullptr;
    void* const recovery = allocator.try_allocate(sizeof(QuicRecovery), alignof(QuicRecovery));
    if (seeded != crypto::CryptoError::None || entries == nullptr || frames == nullptr || bytes == nullptr ||
        bits == nullptr || recovery == nullptr || !streams || !datagrams || (early_bytes != 0U && early == nullptr) ||
        (held_bytes != 0U && (held == nullptr || held_entries == nullptr)))
    {
        memory::destroy_array(allocator, entries, kQuicSpaces * per_space);
        memory::destroy_array(allocator, frames, config.sent_frames);
        memory::destroy_array(allocator, bytes, kQuicSpaces * 2U * crypto);
        memory::destroy_array(allocator, bits, kQuicSpaces * 3U * words);
        memory::destroy_array(allocator, static_cast<u8*>(recovery), sizeof(QuicRecovery));
        memory::destroy_array(allocator, early, early_bytes);
        memory::destroy_array(allocator, held, held_bytes);
        memory::destroy_array(allocator, held_entries, config.held_packets);
        return;
    }
    if (early_bytes != 0U && !config.early_context.empty())
    {
        std::memcpy(early + kQuicEarlyParameterBytes, config.early_context.data(), config.early_context.size());
    }
    m_config.early_context = {};
    m_early_context = early;
    m_early_context_bytes = early_bytes;
    m_held_bytes = held;
    m_held = held_entries;
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
    recovery_config.reserved_entries = kSentPacketReserve;
    m_recovery = new (recovery) QuicRecovery(recovery_config, cont::Span<QuicSentEntry>(entries, per_space),
                                             cont::Span<QuicSentEntry>(entries + per_space, per_space),
                                             cont::Span<QuicSentEntry>(entries + (2U * per_space), per_space));
    // The policy and the datagram size were checked (config_valid).
    m_congestion = make_congestion(config.congestion, config.max_datagram_bytes, nullptr, m_congestion_slot);
    m_frames = frames;
    m_crypto_bytes = bytes;
    m_crypto_bits = bits;
    m_storage = entries;
}

QuicConnection::~QuicConnection() noexcept
{
    finish();
    if (m_congestion != nullptr)
    {
        m_congestion->~CongestionController();
    }
    if (m_recovery != nullptr)
    {
        m_recovery->~QuicRecovery();
        m_allocator.deallocate(m_recovery);
    }
    const usize crypto = m_config.crypto_bytes;
    memory::destroy_array(m_allocator, m_storage, kQuicSpaces * (usize{m_config.sent_packets} + kSentPacketReserve));
    memory::destroy_array(m_allocator, m_frames, m_config.sent_frames);
    memory::destroy_array(m_allocator, m_crypto_bytes, kQuicSpaces * 2U * crypto);
    memory::destroy_array(m_allocator, m_crypto_bits, kQuicSpaces * 3U * quic_stream_bitmap_words(crypto));
    memory::destroy_array(m_allocator, m_early_context, m_early_context_bytes);
    memory::destroy_array(m_allocator, m_held_bytes, usize{m_config.held_packets} * m_config.max_datagram_bytes);
    memory::destroy_array(m_allocator, m_held, m_config.held_packets);
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
        m_config.tls == nullptr || initial_dcid.size() < 8U || initial_dcid.size() > crypto::kQuicMaxConnectionIdBytes ||
        local_cid.size() > crypto::kQuicMaxConnectionIdBytes)
    {
        return false;
    }
    static_cast<void>(m_local_cid.assign(local_cid)); // both bounded above
    static_cast<void>(m_initial_dcid.assign(initial_dcid));
    m_local_ids[0] = LocalCid{};
    m_local_ids[0].id = m_local_cid;
    m_local_ids[0].acked = true;
    m_local_id_count = 1U;
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
        m_config.tls == nullptr || accept.local_cid.empty() || accept.client_dcid.empty() || !bounded(accept.local_cid) ||
        !bounded(accept.client_dcid) || !bounded(accept.original_dcid) || !bounded(accept.client_scid) ||
        (!accept.retried && !same_bytes(accept.original_dcid, accept.client_dcid)))
    {
        return false;
    }
    m_local_ids[0] = LocalCid{};
    if (m_config.ids != nullptr &&
        !m_config.ids->token(accept.local_cid, cont::Span<u8>(m_local_ids[0].reset_token, kQuicResetTokenBytes)))
    {
        return false;
    }
    static_cast<void>(m_local_cid.assign(accept.local_cid)); // each bounded above
    m_local_ids[0].id = m_local_cid;
    m_local_ids[0].acked = true;
    m_local_id_count = 1U;
    static_cast<void>(m_initial_dcid.assign(accept.client_dcid));
    static_cast<void>(m_original_dcid.assign(accept.original_dcid));
    static_cast<void>(m_peer_scid.assign(accept.client_scid));
    m_retried = accept.retried;
    m_validated = accept.validated;
    m_peer_scid_known = true;
    m_peer_cids[0] = PeerCid{};
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
    if (side == crypto::QuicSide::Client)
    {
        // The first other version its first flight converts to: the server may switch to it (RFC 9368 section 2.3).
        const u32 original = static_cast<u32>(m_config.version);
        for (usize index = 0U; index < m_version_count; ++index)
        {
            const u32 version = m_versions[index];
            if (version != original && quic_versions_compatible(original, version))
            {
                if (m_protection.expect_version(static_cast<crypto::QuicVersion>(version), m_initial_dcid.view()) !=
                    crypto::CryptoError::None)
                {
                    return false;
                }
                break;
            }
        }
    }
    fill_local_parameters();
    u8 encoded[crypto::kQuicMaxTransportParameterBytes];
    const usize bytes = encode_parameters(cont::Span<u8>(encoded, sizeof(encoded)));
    m_handshake.set_hello_answer(side == crypto::QuicSide::Server ? &m_hello : nullptr);
    // A server's early data context: the parameters its 0-RTT depends on, then the application's part.
    if (m_early_context != nullptr)
    {
        static_cast<void>(
            write_quic_early_parameters(m_local, cont::Span<u8>(m_early_context, kQuicEarlyParameterBytes)));
    }
    if (bytes == 0U || m_handshake.set_version(m_config.version) != crypto::TlsError::None ||
        m_handshake.init(*m_config.tls, cont::ConstSpan<u8>(encoded, bytes), m_config.tickets, {},
                         cont::ConstSpan<u8>(m_early_context, m_early_context_bytes)) != crypto::TlsError::None ||
        !begin_early())
    {
        return false;
    }
    m_state = QuicConnectionState::Handshaking;
    m_idle_ns = m_local.max_idle_timeout * kNsPerMs;
    restart_idle(now_ns);
    return true;
}

void QuicConnection::fill_local_parameters() noexcept
{
    m_protection.accept_cleared_quic_bit(m_config.grease_quic_bit);
    m_protection.set_key_update_interval(m_config.key_update_interval);
    m_local = m_config.parameters;
    m_streams.fill_parameters(m_local);
    m_datagrams.fill_parameters(m_local);
    m_local.has_initial_source_connection_id = true;
    m_local.initial_source_connection_id = m_local_cid;
    if (m_config.side == crypto::QuicSide::Server)
    {
        m_local.has_original_destination_connection_id = true;
        m_local.original_destination_connection_id = m_original_dcid;
        m_local.has_retry_source_connection_id = m_retried;
        if (m_retried)
        {
            m_local.retry_source_connection_id = m_initial_dcid;
        }
        // Sequence 0's token: a client's transport parameters are not confidential, so only a server sends one.
        m_local.has_stateless_reset_token = m_config.ids != nullptr;
        std::memcpy(m_local.stateless_reset_token, m_local_ids[0].reset_token, kQuicResetTokenBytes);
    }
    m_local.grease_quic_bit = m_config.grease_quic_bit;
    m_local.has_version_information = true;
    m_local.version_information.chosen = static_cast<u32>(m_config.version);
    m_local.version_information.available_count = m_version_count;
    for (usize index = 0U; index < m_version_count; ++index)
    {
        m_local.version_information.available[index] = m_versions[index];
    }
}

bool QuicConnection::open_keyed(const QuicKeyedStart& start, u64 now_ns) noexcept
{
    const bool opened = start_keyed(start, now_ns);
    crypto::secure_zero(start.secret);
    return opened;
}

bool QuicConnection::start_keyed(const QuicKeyedStart& start, u64 now_ns) noexcept
{
    const auto id_in_range = [](usize bytes)
    {
        return bytes >= kQuicMinKeyedIdBytes && bytes <= crypto::kQuicMaxConnectionIdBytes;
    };
    if (!ready() || m_state != QuicConnectionState::Idle || m_config.tls != nullptr ||
        !id_in_range(start.local_id_bytes) || !id_in_range(start.peer_id_bytes))
    {
        return false;
    }
    if (start.policy != nullptr &&
        start.policy->snapshot().status(start.aead, start.unix_seconds, start.policy_scope) ==
            crypto::AlgorithmStatus::Refused)
    {
        return false;
    }
    const crypto::QuicSide side = m_config.side;
    const cont::ConstSpan<u8> secret = start.secret;
    u8 local[crypto::kQuicMaxConnectionIdBytes];
    u8 peer[crypto::kQuicMaxConnectionIdBytes];
    const cont::Span<u8> local_id(local, start.local_id_bytes);
    const cont::Span<u8> peer_id(peer, start.peer_id_bytes);
    u8 reset_token[kQuicResetTokenBytes] = {};
    if (crypto::quic_keyed_connection_id(secret, side, local_id) != crypto::CryptoError::None ||
        crypto::quic_keyed_connection_id(secret, other(side), peer_id) != crypto::CryptoError::None ||
        (side == crypto::QuicSide::Server && m_config.ids != nullptr &&
         !m_config.ids->token(local_id, cont::Span<u8>(reset_token, sizeof(reset_token)))))
    {
        return false;
    }
    m_protection.start_keyed(side, m_config.version);
    constexpr crypto::QuicSide directions[] = {crypto::QuicSide::Client, crypto::QuicSide::Server};
    for (const crypto::QuicSide direction : directions)
    {
        u8 traffic[crypto::kQuicMaxSecretBytes];
        usize bytes = 0U;
        crypto::CryptoError error = crypto::quic_keyed_traffic_secret(start.aead, secret, direction,
                                                                      cont::Span<u8>(traffic, sizeof(traffic)), bytes);
        if (error == crypto::CryptoError::None)
        {
            error = m_protection.install(crypto::QuicLevel::Application, direction == side, start.aead,
                                         cont::ConstSpan<u8>(traffic, bytes));
        }
        crypto::secure_zero(cont::Span<u8>(traffic, sizeof(traffic)));
        if (error != crypto::CryptoError::None)
        {
            return false;
        }
    }
    static_cast<void>(m_local_cid.assign(local_id)); // both bounded above
    static_cast<void>(m_peer_scid.assign(peer_id));
    m_initial_dcid = m_peer_scid;
    m_original_dcid = (side == crypto::QuicSide::Server) ? m_local_cid : m_peer_scid;
    m_peer_scid_known = true;
    m_local_ids[0] = LocalCid{};
    m_local_ids[0].id = m_local_cid;
    m_local_ids[0].acked = true;
    std::memcpy(m_local_ids[0].reset_token, reset_token, kQuicResetTokenBytes);
    m_local_id_count = 1U;
    m_peer_cids[0] = PeerCid{};
    m_peer_cids[0].id = m_peer_scid;
    m_peer_cid_count = 1U;
    fill_local_parameters();

    u8 hello[kKeyedHelloPrefix + crypto::kQuicMaxTransportParameterBytes];
    const usize bytes = encode_parameters(cont::Span<u8>(hello + kKeyedHelloPrefix, sizeof(hello) - kKeyedHelloPrefix));
    hello[0] = static_cast<u8>(bytes >> 8U);
    hello[1] = static_cast<u8>(bytes);
    const usize whole = kKeyedHelloPrefix + bytes;
    if (bytes == 0U || m_levels[index_of(QuicSpace::Application)].out.write(cont::ConstSpan<u8>(hello, whole)) != whole)
    {
        return false;
    }
    m_keyed = true;
    m_validated = side == crypto::QuicSide::Client; // a server's limit lasts until its client acknowledges a packet
    m_state = QuicConnectionState::Established;
    m_idle_ns = m_local.max_idle_timeout * kNsPerMs;
    discard_space(QuicSpace::Initial, now_ns);
    confirm(now_ns);
    restart_idle(now_ns);
    return true;
}

bool QuicConnection::begin_early() noexcept
{
    if (m_handshake.early_data() != crypto::TlsEarlyData::Offered)
    {
        return true;
    }
    // Parameters that no longer parse send nothing early: the data waits for the 1-RTT keys.
    if (read_quic_transport_parameters(m_handshake.remembered_transport_parameters(), crypto::QuicSide::Server,
                                       m_remembered) != QuicTransportError::NoError)
    {
        return true;
    }
    apply_peer_limits(m_remembered);
    m_early_sending = true;
    return m_protection.install(m_handshake) == crypto::CryptoError::None;
}

void QuicConnection::apply_peer_limits(const QuicTransportParameters& peer) noexcept
{
    m_streams.set_peer_parameters(peer);
    m_datagrams.set_peer(peer.max_datagram_frame_size, min_u64(m_config.max_datagram_bytes, peer.max_udp_payload_size));
}

bool QuicConnection::settle_early(u64 now_ns) noexcept
{
    const crypto::TlsEarlyData early = m_handshake.early_data();
    if (m_config.side == crypto::QuicSide::Server)
    {
        // Once the ClientHello is read, a server that did not accept 0-RTT processes none of it (RFC 9001 section
        // 4.6.2), held packets included.
        if (m_peer_known && !m_early_settled)
        {
            m_early_settled = true;
            if (early != crypto::TlsEarlyData::Accepted)
            {
                m_protection.discard(crypto::QuicLevel::Early);
            }
        }
        return true;
    }
    if (!m_early_sending)
    {
        return true;
    }
    if (early == crypto::TlsEarlyData::Rejected)
    {
        reject_early(now_ns);
        return true;
    }
    if (early == crypto::TlsEarlyData::Accepted && m_peer_known)
    {
        // The server's parameters apply from now on (RFC 9000 section 7.4.1); a limit the 0-RTT packets kept to that
        // they lower closes the connection (RFC 9221 section 3 names PROTOCOL_VIOLATION for its size).
        m_early_sending = false;
        if (quic_check_remembered_parameters(m_remembered, m_peer) != QuicTransportError::NoError)
        {
            fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
            return false;
        }
    }
    return true;
}

void QuicConnection::reject_early(u64 now_ns) noexcept
{
    // The 0-RTT packets' recovery state goes (RFC 9002 section 6.4), and with them every stream and the queued
    // datagrams (RFC 9001 section 4.6.2); the server's parameters, once known, grant this side anew.
    m_early_sending = false;
    m_protection.discard(crypto::QuicLevel::Early);
    m_recovery->reject_early(now_ns, m_events);
    m_streams.reset_all();
    m_datagrams.reject_early();
    if (m_peer_known)
    {
        apply_peer_limits(m_peer);
    }
}

usize QuicConnection::encode_parameters(cont::Span<u8> out) noexcept
{
    return write_quic_transport_parameters(m_local, m_config.side, QuicTransportParameterGrease{entropy()}, out);
}

usize QuicConnection::HelloAnswer::answer(cont::ConstSpan<u8> client, crypto::QuicVersion& version,
                                          cont::Span<u8> out) noexcept
{
    // Parameters that do not parse or name another chosen version keep the original version: read_peer_parameters()
    // closes with their error once provide() returns (RFC 9368 section 4).
    QuicConnection& connection = m_connection;
    const u32 original = static_cast<u32>(connection.m_config.version);
    QuicTransportParameters parameters;
    if (read_quic_transport_parameters(client, crypto::QuicSide::Client, parameters) != QuicTransportError::NoError ||
        quic_server_check_version_information(parameters, original) != QuicTransportError::NoError)
    {
        return 0U;
    }
    const u32 chosen = quic_server_select_version(
        parameters, original, cont::ConstSpan<u32>(connection.m_versions, connection.m_version_count));
    if (chosen == 0U || chosen == original)
    {
        return 0U;
    }
    // Nothing is sent before the switch but acknowledgements in the original version (RFC 9369 section 4.1). A key
    // the provider cannot derive, or parameters that do not fit, fail the handshake (an answer past its room).
    connection.m_local.version_information.chosen = chosen;
    const usize bytes = connection.encode_parameters(out);
    if (bytes == 0U ||
        connection.m_protection.change_version(static_cast<crypto::QuicVersion>(chosen),
                                               connection.m_initial_dcid.view()) != crypto::CryptoError::None)
    {
        connection.m_local.version_information.chosen = original;
        return out.size() + 1U;
    }
    version = static_cast<crypto::QuicVersion>(chosen);
    return bytes;
}

bool QuicConnection::give_new_token(cont::ConstSpan<u8> token) noexcept
{
    if (m_config.side != crypto::QuicSide::Server || !m_peer_known || token.empty() ||
        token.size() > kQuicMaxCarriedTokenBytes || m_new_token_bytes != 0U ||
        (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established))
    {
        return false;
    }
    std::memcpy(m_new_token, token.data(), token.size());
    m_new_token_bytes = token.size();
    m_new_token_owed = m_confirmed;
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
        if (stateless_reset(datagram, now_ns))
        {
            return;
        }
        // Answer at a falling rate: after 1, 2, 4, ... more datagrams (section 10.2.1).
        if (++m_close_received >= m_close_owed_after)
        {
            m_close_received = 0U;
            m_close_owed_after = (m_close_owed_after < (u64{1} << 32U)) ? m_close_owed_after * 2U : m_close_owed_after;
            m_close_owed = true;
        }
        return;
    }
    // Opening runs in place, so a stateless reset's token is read before it (section 10.3.1).
    u8 tail[kQuicResetTokenBytes] = {};
    const bool reset_sized = datagram.size() >= kQuicMinResetBytes;
    if (reset_sized)
    {
        std::memcpy(tail, datagram.data() + datagram.size() - kQuicResetTokenBytes, kQuicResetTokenBytes);
    }
    QuicDatagramPackets packets(datagram, m_local_cid.bytes);
    QuicHeader header;
    cont::Span<u8> packet;
    bool first = true;
    bool first_opened = false;
    bool live = true;
    while (live && packets.next(header, packet))
    {
        const bool opened = receive_packet(header, packet, datagram.size(), ecn, now_ns);
        first_opened = first ? opened : first_opened;
        first = false;
        live = m_state == QuicConnectionState::Handshaking || m_state == QuicConnectionState::Established;
    }
    if (live && (packets.foreign() || packets.error() != QuicHeaderError::None))
    {
        ++m_counters.packets_dropped;
    }
    // The comparison is owed when the first packet cannot be associated or opened; it is skipped otherwise.
    if (!first_opened && reset_sized && reset_by(cont::ConstSpan<u8>(tail, kQuicResetTokenBytes), now_ns))
    {
        return;
    }
    read_held(now_ns);
}

void QuicConnection::hold(QuicPacketType type, cont::ConstSpan<u8> packet, QuicEcn ecn) noexcept
{
    const usize slot = m_config.max_datagram_bytes;
    if (m_held_count == m_config.held_packets || packet.size() > slot)
    {
        ++m_counters.packets_dropped; // the peer sends it again
        return;
    }
    std::memcpy(m_held_bytes + (usize{m_held_count} * slot), packet.data(), packet.size());
    Held& held = m_held[m_held_count++];
    held.bytes = static_cast<u32>(packet.size());
    held.type = type;
    held.ecn = ecn;
    ++m_counters.packets_held;
}

void QuicConnection::read_held(u64 now_ns) noexcept
{
    const usize slot = m_config.max_datagram_bytes;
    // A packet read may install the keys of another held one: the ring is walked again until none opens.
    for (bool progress = true; progress && m_held_count != 0U;)
    {
        progress = false;
        u32 kept = 0U;
        for (u32 index = 0U; index < m_held_count; ++index)
        {
            const Held held = m_held[index];
            u8* const bytes = m_held_bytes + (usize{index} * slot);
            const bool live =
                m_state == QuicConnectionState::Handshaking || m_state == QuicConnectionState::Established;
            if (live && m_protection.can_open(held.type))
            {
                QuicDatagramPackets packets(cont::Span<u8>(bytes, held.bytes), m_local_cid.bytes);
                QuicHeader header;
                cont::Span<u8> packet;
                if (packets.next(header, packet))
                {
                    static_cast<void>(receive_packet(header, packet, held.bytes, held.ecn, now_ns));
                }
                progress = true;
                continue;
            }
            if (live && m_protection.keys_pending(held.type))
            {
                if (kept != index)
                {
                    std::memcpy(m_held_bytes + (usize{kept} * slot), bytes, held.bytes);
                }
                m_held[kept++] = held;
                continue;
            }
            ++m_counters.packets_dropped; // its keys went, or the connection ended
        }
        m_held_count = kept;
    }
}

bool QuicConnection::stateless_reset(cont::ConstSpan<u8> datagram, u64 now_ns) noexcept
{
    return datagram.size() >= kQuicMinResetBytes && reset_by(datagram.last(kQuicResetTokenBytes), now_ns);
}

bool QuicConnection::reset_by(cont::ConstSpan<u8> tail, u64 now_ns) noexcept
{
    if (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established &&
        m_state != QuicConnectionState::Closing)
    {
        return false;
    }
    // Every token of a used, unretired ID is compared in constant time, none skipped once one matches.
    bool matched = false;
    for (usize index = 0U; index < m_peer_cid_count; ++index)
    {
        const PeerCid& held = m_peer_cids[index];
        if (held.has_token && held.used)
        {
            const bool equal =
                crypto::constant_time_equal(tail, cont::ConstSpan<u8>(held.reset_token, kQuicResetTokenBytes));
            matched = matched || equal;
        }
    }
    if (!matched)
    {
        return false;
    }
    if (m_state != QuicConnectionState::Closing)
    {
        m_close = QuicCloseInfo{};
        m_reason_bytes = 0U;
    }
    m_close.stateless_reset = true;
    enter(QuicConnectionState::Draining, now_ns);
    return true;
}

bool QuicConnection::receive_packet(const QuicHeader& header, cont::Span<u8> packet, usize datagram_bytes, QuicEcn ecn,
                                    u64 now_ns) noexcept
{
    const bool client = m_config.side == crypto::QuicSide::Client;
    if (header.type == QuicPacketType::VersionNegotiation)
    {
        version_negotiation(header, packet);
        return false;
    }
    if (client && header.type == QuicPacketType::Retry)
    {
        return retry(header, packet, now_ns);
    }
    const bool long_header = header.type != QuicPacketType::OneRtt;
    m_packet_local = local_index(header.dcid);
    bool routed = m_packet_local != m_local_id_count ||
                  (!client && long_header && same_bytes(header.dcid, m_initial_dcid.view()));
    if (long_header)
    {
        // Retry is taken above; other versions are not this connection's, and a server's Initial carries no token
        // (section 17.2.2).
        routed = routed && header.type != QuicPacketType::Retry && header.type != QuicPacketType::OtherVersion &&
                 version_routed(header) && (!m_peer_scid_known || same_bytes(header.scid, m_peer_scid.view())) &&
                 (!client || header.type != QuicPacketType::Initial || header.token.empty());
    }
    // A server discards a client Initial in a datagram under 1200 bytes (section 14.1).
    if (!routed || (!client && header.type == QuicPacketType::Initial && datagram_bytes < kQuicMinInitialDatagram))
    {
        ++m_counters.packets_dropped;
        return false;
    }
    if (!m_protection.can_open(header.type) && m_protection.keys_pending(header.type))
    {
        hold(header.type, packet, ecn);
        return false;
    }
    QuicOpened opened;
    const QuicPacketError error = m_protection.open(header, packet, opened);
    if (error != QuicPacketError::None)
    {
        const QuicTransportError close = quic_packet_close(error);
        if (close == QuicTransportError::NoError)
        {
            ++m_counters.packets_dropped;
            return false;
        }
        fail(close, 0U, now_ns);
        return false;
    }
    ++m_counters.packets_opened;
    const QuicSpace space = quic_space_of(opened.type);
    m_packet_version = long_header ? header.version : 0U;
    m_packet_early = opened.type == QuicPacketType::ZeroRtt;
    if (!client && opened.type == QuicPacketType::OneRtt && m_early_key_ns == kNeverNs &&
        m_protection.can_open(QuicPacketType::ZeroRtt))
    {
        // 0-RTT packets still reordered behind the first 1-RTT one are read for three PTOs (RFC 9001 section 4.9.3).
        m_early_key_ns = add_saturating(now_ns, three_ptos());
    }
    if (client && long_header && header.version != static_cast<u32>(m_protection.version()) && !m_version_settled)
    {
        // The server switched compatibly: its first Initial packet in a version this client offered opened, so the
        // connection runs in it from now on, its tickets too (RFC 9369 sections 4.1 and 5).
        const auto negotiated = static_cast<crypto::QuicVersion>(header.version);
        if (m_protection.change_version(negotiated, m_initial_dcid.view()) != crypto::CryptoError::None ||
            m_handshake.set_version(negotiated) != crypto::TlsError::None)
        {
            fail(QuicTransportError::InternalError, 0U, now_ns);
            return true;
        }
        m_version_settled = true;
    }
    if (client && long_header && !m_peer_scid_known)
    {
        // The server's first packet names the ID the client sends to from now on (section 7.2).
        static_cast<void>(m_peer_scid.assign(header.scid));
        m_peer_scid_known = true;
        m_peer_cids[0] = PeerCid{};
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
            return true;
        }
        if (!quic_frame_allowed(read.kind, read.application, opened.type, client))
        {
            fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
            return true;
        }
        eliciting = eliciting || quic_frame_ack_eliciting(read.kind);
        if (!on_frame(read, space, now_ns))
        {
            return true;
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
    return true;
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
        case QuicFrameKind::ResetStreamAt:
        case QuicFrameKind::StopSending:
        case QuicFrameKind::MaxData:
        case QuicFrameKind::MaxStreamData:
        case QuicFrameKind::MaxStreams:
        case QuicFrameKind::DataBlocked:
        case QuicFrameKind::StreamDataBlocked:
        case QuicFrameKind::StreamsBlocked:
        {
            const QuicTransportError error = m_streams.on_frame(frame, m_packet_early);
            if (error != QuicTransportError::NoError)
            {
                fail(error, 0U, now_ns);
                return false;
            }
            return true;
        }
        case QuicFrameKind::Ack:
        {
            m_event_ns = now_ns;
            const QuicAckOutcome outcome = m_recovery->on_ack(space, frame.ack, now_ns, m_events);
            if (outcome.error == QuicRecoveryError::AckOfUnsent)
            {
                fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
                return false;
            }
            if (outcome.error == QuicRecoveryError::None)
            {
                m_congestion->on_ack_end(m_recovery->rtt(), outcome.rtt_sampled, now_ns);
                if (outcome.lost.persistent_congestion)
                {
                    m_congestion->on_persistent_congestion(now_ns);
                }
            }
            if (space == QuicSpace::Application)
            {
                m_protection.acknowledged(frame.ack.largest);
            }
            if (m_keyed && !m_validated && outcome.acked_packets != 0U)
            {
                // Only an end that read a packet sent to this address can acknowledge it.
                m_validated = true;
                m_recovery->set_amplification_blocked(false, now_ns);
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
        case QuicFrameKind::Datagram:
        {
            const QuicTransportError error = m_datagrams.on_frame(frame, m_packet_early);
            if (error != QuicTransportError::NoError)
            {
                fail(error, 0U, now_ns);
                return false;
            }
            return true;
        }
        case QuicFrameKind::RetireConnectionId:
            return retire_local(frame.sequence, now_ns);
        case QuicFrameKind::PathResponse: // this side sent no PATH_CHALLENGE (section 19.18's MAY taken)
            fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
            return false;
    }
    return true;
}

bool QuicConnection::version_routed(const QuicHeader& header) const noexcept
{
    const crypto::QuicVersion running = m_protection.version();
    if (header.version == static_cast<u32>(running))
    {
        return true;
    }
    // An Initial packet of the other version whose key is held: from a server's client that has not learned the
    // switch, and on a client from its server switching (until the version is settled) or acknowledging in the
    // original version before it switched (RFC 9369 section 4.1). Handshake and 1-RTT packets come in one version.
    return header.type == QuicPacketType::Initial && m_protection.opens_initial(header.version) &&
           (m_config.side == crypto::QuicSide::Server || !m_version_settled ||
            header.version == static_cast<u32>(m_protection.original_version()));
}

bool QuicConnection::crypto_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept
{
    if (m_config.side == crypto::QuicSide::Client && m_packet_version != 0U)
    {
        // A server sends every CRYPTO frame in the version it runs (RFC 9369 section 4.1): one in the original
        // version settles it, one in the original version after the switch is a violation.
        if (m_packet_version != static_cast<u32>(m_protection.version()))
        {
            fail(QuicTransportError::ProtocolViolation, kCryptoFrameType, now_ns);
            return false;
        }
        m_version_settled = true;
    }
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
    if (m_keyed)
    {
        return read_keyed_hello(level, now_ns);
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

bool QuicConnection::read_keyed_hello(Level& level, u64 now_ns) noexcept
{
    const cont::ConstSpan<u8> readable = level.in.readable();
    if (m_peer_known && readable.empty())
    {
        return true; // a retransmission of what was read
    }
    if (m_peer_known)
    {
        fail(QuicTransportError::ProtocolViolation, kCryptoFrameType, now_ns);
        return false;
    }
    if (readable.size() < kKeyedHelloPrefix)
    {
        return true;
    }
    const usize bytes = (usize{readable[0]} << 8U) | usize{readable[1]};
    if (bytes > crypto::kQuicMaxTransportParameterBytes)
    {
        fail(QuicTransportError::TransportParameterError, kCryptoFrameType, now_ns);
        return false;
    }
    const usize whole = kKeyedHelloPrefix + bytes;
    if (readable.size() < whole)
    {
        return true;
    }
    if (readable.size() > whole)
    {
        fail(QuicTransportError::ProtocolViolation, kCryptoFrameType, now_ns);
        return false;
    }
    if (!read_peer_parameters(readable.subspan(kKeyedHelloPrefix, bytes), now_ns))
    {
        return false;
    }
    level.in.consume(whole);
    return true;
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
    if (!m_peer_known && !m_handshake.peer_transport_parameters().empty() &&
        !read_peer_parameters(m_handshake.peer_transport_parameters(), now_ns))
    {
        return false;
    }
    if (!settle_early(now_ns))
    {
        return false;
    }
    if (m_state == QuicConnectionState::Handshaking && m_handshake.state() == crypto::TlsState::Established)
    {
        m_state = QuicConnectionState::Established;
        if (m_config.side == crypto::QuicSide::Server)
        {
            // A server's handshake is confirmed when it completes (RFC 9001 section 4.1.2); it says so, then hands
            // out its token for the next connection (section 8.1.3).
            m_handshake_done_owed = true;
            m_new_token_owed = m_new_token_bytes != 0U;
            confirm(now_ns);
        }
    }
    return true;
}

bool QuicConnection::read_peer_parameters(cont::ConstSpan<u8> encoded, u64 now_ns) noexcept
{
    const crypto::QuicSide peer = other(m_config.side);
    QuicTransportError error = read_quic_transport_parameters(encoded, peer, m_peer);
    if (error == QuicTransportError::NoError)
    {
        QuicConnectionIdExpectation expected;
        expected.peer_source = m_peer_scid.view();
        if (m_config.side == crypto::QuicSide::Client)
        {
            expected.original_destination = m_original_dcid.view();
            expected.retried = m_retried;
            expected.retry_source = m_initial_dcid.view(); // the Retry's Source Connection ID after one
        }
        error = quic_check_connection_ids(m_peer, peer, expected);
    }
    if (error == QuicTransportError::NoError)
    {
        const u32 original = static_cast<u32>(m_config.version);
        error = (m_config.side == crypto::QuicSide::Server)
                    ? quic_server_check_version_information(m_peer, original)
                    : quic_client_check_version_information(m_peer, static_cast<u32>(m_protection.version()), original,
                                                            m_config.after_version_negotiation,
                                                            cont::ConstSpan<u32>(m_versions, m_version_count));
    }
    if (error != QuicTransportError::NoError)
    {
        fail(error, 0U, now_ns);
        return false;
    }
    m_peer_known = true;
    apply_peer_limits(m_peer);
    m_recovery->set_peer_parameters(m_peer.max_ack_delay * kNsPerMs, m_peer.ack_delay_exponent, now_ns);
    m_peer_greases = m_peer.grease_quic_bit;
    const u64 local_idle = m_local.max_idle_timeout;
    const u64 peer_idle = m_peer.max_idle_timeout;
    const u64 idle_ms =
        (local_idle == 0U || peer_idle == 0U) ? max_u64(local_idle, peer_idle) : min_u64(local_idle, peer_idle);
    m_idle_ns = idle_ms * kNsPerMs;
    if (m_peer.has_stateless_reset_token)
    {
        // Sequence 0's token. A keyed connection's parameters are a packet like any other, and a reordered flight can
        // bring NEW_CONNECTION_ID first and retire sequence 0 before them.
        for (usize index = 0U; index < m_peer_cid_count; ++index)
        {
            PeerCid& held = m_peer_cids[index];
            if (held.sequence == 0U)
            {
                std::memcpy(held.reset_token, m_peer.stateless_reset_token, kQuicResetTokenBytes);
                held.has_token = true;
                watch(held);
            }
        }
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

void QuicConnection::watch(const PeerCid& held) noexcept
{
    if (m_config.ids != nullptr && held.has_token && held.used)
    {
        m_config.ids->watch_reset(cont::ConstSpan<u8>(held.reset_token, kQuicResetTokenBytes));
    }
}

void QuicConnection::unwatch(const PeerCid& held) noexcept
{
    if (m_config.ids != nullptr && held.has_token && held.used)
    {
        m_config.ids->unwatch_reset(cont::ConstSpan<u8>(held.reset_token, kQuicResetTokenBytes));
    }
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
        // One ID under two sequences, or one token under two IDs (section 10.3.2).
        if (same_id ||
            (held.has_token && std::memcmp(held.reset_token, frame.reset_token.data(), kQuicResetTokenBytes) == 0))
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
                unwatch(m_peer_cids[index]);
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
    added = PeerCid{};
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

usize QuicConnection::local_index(cont::ConstSpan<u8> id) const noexcept
{
    for (usize index = 0U; index < m_local_id_count; ++index)
    {
        if (same_bytes(m_local_ids[index].id.view(), id))
        {
            return index;
        }
    }
    return m_local_id_count;
}

bool QuicConnection::retire_local(u64 sequence, u64 now_ns) noexcept
{
    // Never toward zero-length IDs, never past the IDs issued, never the one the packet was sent to (section 19.16).
    if (m_local_cid.bytes == 0U || sequence >= m_local_next_sequence)
    {
        fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
        return false;
    }
    usize index = 0U;
    while (index < m_local_id_count && m_local_ids[index].sequence != sequence)
    {
        ++index;
    }
    if (index == m_local_id_count)
    {
        return true; // retired already
    }
    if (index == m_packet_local)
    {
        fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
        return false;
    }
    if (m_config.ids != nullptr)
    {
        m_config.ids->retire(m_local_ids[index].id.view());
    }
    for (usize at = index + 1U; at < m_local_id_count; ++at)
    {
        m_local_ids[at - 1U] = m_local_ids[at];
    }
    --m_local_id_count;
    m_packet_local = (m_packet_local > index) ? m_packet_local - 1U : m_packet_local;
    return true; // send() issues the replacement (section 5.1.1)
}

void QuicConnection::issue_local_ids() noexcept
{
    if (m_config.ids == nullptr || !m_confirmed || !m_peer_known || m_local_cid.bytes == 0U ||
        m_state != QuicConnectionState::Established)
    {
        return;
    }
    const usize target = static_cast<usize>(min_u64(m_peer.active_connection_id_limit, kQuicMaxLocalConnectionIds));
    usize active = 0U;
    for (usize index = 0U; index < m_local_id_count; ++index)
    {
        active += (m_local_ids[index].sequence >= m_local_retire_prior_to) ? 1U : 0U;
    }
    constexpr usize capacity = sizeof(m_local_ids) / sizeof(m_local_ids[0]);
    while (active < target && m_local_id_count < capacity && m_local_next_sequence < kQuicMaxVarint)
    {
        LocalCid& added = m_local_ids[m_local_id_count];
        added = LocalCid{};
        added.id.bytes = m_local_cid.bytes;
        if (!m_config.ids->issue(cont::Span<u8>(added.id.id, added.id.bytes),
                                 cont::Span<u8>(added.reset_token, kQuicResetTokenBytes)))
        {
            return;
        }
        added.sequence = m_local_next_sequence++;
        added.owed = true;
        ++m_local_id_count;
        ++active;
    }
}

bool QuicConnection::rotate_connection_ids() noexcept
{
    if (m_config.ids == nullptr || !m_confirmed || !m_peer_known || m_local_cid.bytes == 0U ||
        m_state != QuicConnectionState::Established)
    {
        return false;
    }
    // An earlier Retire Prior To is retired in full first (section 5.1.2's SHOULD NOT).
    for (usize index = 0U; index < m_local_id_count; ++index)
    {
        if (m_local_ids[index].sequence < m_local_retire_prior_to)
        {
            return false;
        }
    }
    const u64 earlier = m_local_retire_prior_to;
    const usize before = m_local_id_count;
    m_local_retire_prior_to = m_local_next_sequence;
    issue_local_ids();
    if (m_local_id_count == before)
    {
        m_local_retire_prior_to = earlier;
        return false;
    }
    return true;
}

void QuicConnection::finish() noexcept
{
    m_state = QuicConnectionState::Closed;
    if (m_config.ids != nullptr)
    {
        for (usize index = 0U; index < m_local_id_count; ++index)
        {
            if (m_local_ids[index].id.bytes != 0U)
            {
                m_config.ids->retire(m_local_ids[index].id.view());
            }
        }
        for (usize index = 0U; index < m_peer_cid_count; ++index)
        {
            unwatch(m_peer_cids[index]);
            m_peer_cids[index].used = false;
        }
    }
    m_local_id_count = 0U;
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
    finish();
}

bool QuicConnection::retry(const QuicHeader& header, cont::ConstSpan<u8> packet, u64 now_ns) noexcept
{
    // One Retry, before any packet of the server opened, to the ID this client sent, naming a new ID, with a token and
    // a tag that verifies against the first Destination Connection ID (RFC 9000 sections 7.2 and 17.2.5.2).
    const usize body = 1U + 4U + 1U + header.dcid.size() + 1U + header.scid.size();
    if (m_retried || m_peer_scid_known || header.version != static_cast<u32>(m_config.version) ||
        !same_bytes(header.dcid, m_local_cid.view()) || same_bytes(header.scid, m_initial_dcid.view()) ||
        packet.size() <= body + crypto::kQuicTagBytes ||
        packet.size() - body - crypto::kQuicTagBytes > sizeof(m_token) ||
        crypto::quic_retry_verify(m_config.version, m_original_dcid.view(), packet) != crypto::CryptoError::None)
    {
        ++m_counters.packets_dropped;
        return false;
    }
    if (m_protection.rekey_initial(header.scid) != crypto::CryptoError::None)
    {
        fail(QuicTransportError::InternalError, 0U, now_ns);
        return true;
    }
    m_token_bytes = packet.size() - body - crypto::kQuicTagBytes;
    std::memcpy(m_token, packet.data() + body, m_token_bytes);
    static_cast<void>(m_initial_dcid.assign(header.scid)); // at most kQuicMaxConnectionIdBytes, as parsed
    m_retried = true;
    // Recovery starts over and the Initial's CRYPTO data goes again under the new keys; packet numbers continue
    // (section 17.2.5.3).
    m_recovery->retry(now_ns, m_events);
    m_congestion->reset(now_ns); // RFC 9002 section 6.3: congestion control starts over with recovery
    m_pacer = Pacer{};
    for (u8& probes : m_probes)
    {
        probes = 0U;
    }
    QuicSendBuffer& out = m_levels[index_of(QuicSpace::Initial)].out;
    if (out.sent_end() > out.acked_end())
    {
        out.on_lost(out.acked_end(), static_cast<usize>(out.sent_end() - out.acked_end()), false);
    }
    return true;
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

QuicPacketType QuicConnection::send_type(QuicSpace space) const noexcept
{
    // A client's application data goes in 0-RTT packets until it has 1-RTT keys, and only while it knows no more than
    // the parameters it remembers (RFC 9000 section 7.4.1); RFC 9000 section 17.2.3's limit follows, since 1-RTT keys
    // come before any 1-RTT packet opens.
    if (space == QuicSpace::Application && m_early_sending && !m_peer_known &&
        !m_protection.can_send(QuicPacketType::OneRtt) && m_protection.can_send(QuicPacketType::ZeroRtt))
    {
        return QuicPacketType::ZeroRtt;
    }
    return packet_type(space);
}

bool QuicConnection::has_content(QuicSpace space, u64 now_ns) const noexcept
{
    const usize index = index_of(space);
    if (m_space_gone[index] || !m_protection.can_send(send_type(space)) || !m_recovery->can_send(space))
    {
        return false;
    }
    if (m_recovery->ack_deadline_ns(space) <= now_ns || m_probes[index] > 0U)
    {
        return true;
    }
    return data_allowed(space) && has_data(space, now_ns);
}

bool QuicConnection::data_allowed(QuicSpace space) const noexcept
{
    // The handshake's CRYPTO data is not held by the window or the pacer: a peer whose handshake has not completed
    // reads no 1-RTT packet, so 1-RTT data filling the window is neither acknowledged nor declared lost, and no probe
    // timer serves the Application space before confirmation (RFC 9002 appendix A.8). A lost Handshake flight held
    // behind it would wedge both ends. Its size is bounded by the crypto buffers and, on a server, by the
    // amplification limit; it still counts in bytes in flight.
    return (space != QuicSpace::Application || m_data_open) && m_recovery->can_send_data(space);
}

bool QuicConnection::has_data(QuicSpace space, u64 now_ns) const noexcept
{
    const usize index = index_of(space);
    if (m_levels[index].out.pending())
    {
        return true;
    }
    if (space != QuicSpace::Application)
    {
        return false;
    }
    bool ids_owed = false;
    for (usize local = 0U; local < m_local_id_count; ++local)
    {
        ids_owed = ids_owed || m_local_ids[local].owed;
    }
    return m_handshake_done_owed || m_path_response_owed || m_ping_owed || m_retire_owed_count > 0U ||
           (m_frame_count < m_config.sent_frames && (m_streams.has_frame() || ids_owed || m_new_token_owed)) ||
           datagram_ready(now_ns);
}

void QuicConnection::gate_data(usize datagram, u64 now_ns) noexcept
{
    m_pace_release_ns = kNeverNs;
    bool probing = false;
    for (const u8 probes : m_probes)
    {
        probing = probing || probes > 0U;
    }
    if (probing)
    {
        m_data_open = true; // RFC 9002 section 7.5: probes are not held by the window or the pacer
        return;
    }
    // A whole datagram must fit: a smaller room would let only ACKs and frames too small to matter through, packet
    // after packet. The window's floor of two datagrams always holds one.
    const u64 in_flight = m_recovery->bytes_in_flight();
    const u64 window = m_congestion->window();
    const bool room = in_flight + datagram <= window;
    const u64 release =
        m_pacer.release_ns(now_ns, m_congestion->pacing_rate(m_recovery->rtt()), m_congestion->burst());
    m_data_open = room && release <= now_ns;
    if (m_data_open)
    {
        return;
    }
    const QuicSpace application = QuicSpace::Application;
    const bool waiting = !m_space_gone[index_of(application)] && m_protection.can_send(send_type(application)) &&
                         m_recovery->can_send_data(application) && has_data(application, now_ns);
    if (waiting)
    {
        // Section 7.8: the window is what holds sending back, so it may grow; a window with room waits for the pacer.
        m_congestion->on_limited(now_ns);
        m_pace_release_ns = room ? release : kNeverNs;
    }
}

bool QuicConnection::set_congestion(const CongestionPolicy& policy) noexcept
{
    if (!ready() || !congestion_policy_valid(policy))
    {
        return false;
    }
    const CongestionState held = m_congestion->state();
    m_congestion->~CongestionController();
    m_congestion = make_congestion(policy, m_config.max_datagram_bytes, &held, m_congestion_slot);
    m_config.congestion = policy;
    return true;
}

bool QuicConnection::datagram_ready(u64 now_ns) const noexcept
{
    return m_datagrams.has_frame(now_ns) && (!m_datagrams.outcomes() || m_frame_count < m_config.sent_frames);
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
                else
                {
                    m_streams.on_lost(frame.frame); // a Retry discards 0-RTT packets: their data goes again
                }
            }
            else if (frame.kind == kRecordDatagram)
            {
                if (how == Settle::Acked)
                {
                    m_datagrams.on_acked(frame.frame.value);
                }
                else
                {
                    m_datagrams.on_lost(frame.frame.value); // discarded with its space: never delivered
                }
            }
            else if (frame.kind == kRecordHandshakeDone)
            {
                m_handshake_done_acked = m_handshake_done_acked || how == Settle::Acked;
                m_handshake_done_owed = !m_handshake_done_acked && (m_handshake_done_owed || how == Settle::Lost);
            }
            else if (frame.kind == kRecordNewToken)
            {
                m_new_token_acked = m_new_token_acked || how == Settle::Acked;
                m_new_token_owed = !m_new_token_acked && (m_new_token_owed || how == Settle::Lost);
            }
            else if (frame.kind == kRecordNewId)
            {
                // Resent until acknowledged while held (section 13.3), a rotated one too: it asks the peer to retire.
                for (usize local = 0U; local < m_local_id_count; ++local)
                {
                    LocalCid& held = m_local_ids[local];
                    if (held.sequence == frame.frame.value)
                    {
                        held.acked = held.acked || how == Settle::Acked;
                        held.owed = !held.acked && (held.owed || how == Settle::Lost);
                    }
                }
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

usize QuicConnection::write_frames(QuicSpace space, bool early, cont::Span<u8> frames, u64 packet, u64 now_ns,
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
    // An ACK that is owed goes first; one only carried along follows waiting datagrams, so that a datagram of the
    // largest size fits the next packet while every packet carries an ACK.
    const auto put_ack = [&]
    {
        if (!m_recovery->received(space).ranges().empty())
        {
            const QuicAckWrite ack = m_recovery->write_ack(space, frames.subspan(used), now_ns);
            used += ack.bytes;
            written.carried_ack = (ack.bytes != 0U) ? ack.largest : kQuicNoPacket;
        }
    };
    // A 0-RTT packet carries no ACK, CRYPTO, HANDSHAKE_DONE, NEW_TOKEN or PATH_RESPONSE frame (RFC 9000 section
    // 12.4, table 3), and no RETIRE_CONNECTION_ID either: nothing is retired before the server's IDs arrive.
    const bool ack_later =
        space == QuicSpace::Application && m_recovery->ack_deadline_ns(space) > now_ns && datagram_ready(now_ns);
    if (!ack_later && !early)
    {
        put_ack();
    }
    // A probe owes an ack-eliciting packet, and so does a run of ACK-only packets: those are never acknowledged
    // by themselves, so a PING lets the peer's ACK free their storage (RFC 9000 section 13.2.4).
    const auto put_owed_ping = [&]
    {
        if (!written.eliciting && (m_probes[index] > 0U || m_ack_only[index] >= kAckOnlyRun))
        {
            QuicFrame ping;
            ping.kind = QuicFrameKind::Ping;
            static_cast<void>(put(ping));
        }
    };
    if (!data_allowed(space))
    {
        if (ack_later && !early)
        {
            put_ack();
        }
        put_owed_ping();
        return used;
    }
    if (space == QuicSpace::Application && !early)
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
        if (m_new_token_owed && m_frame_count < m_config.sent_frames)
        {
            QuicFrame token;
            token.kind = QuicFrameKind::NewToken;
            token.data = cont::ConstSpan<u8>(m_new_token, m_new_token_bytes);
            if (put(token))
            {
                static_cast<void>(record(space, packet, kRecordNewToken, QuicStreamRecord{}, written));
                m_new_token_owed = false;
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
        for (usize local = 0U; local < m_local_id_count && m_frame_count < m_config.sent_frames; ++local)
        {
            LocalCid& held = m_local_ids[local];
            if (!held.owed)
            {
                continue;
            }
            // Retire Prior To never passes the frame's own sequence (section 19.15): a rotated ID carries its own.
            QuicFrame issued;
            issued.kind = QuicFrameKind::NewConnectionId;
            issued.sequence = held.sequence;
            issued.retire_prior_to = min_u64(m_local_retire_prior_to, held.sequence);
            issued.data = held.id.view();
            issued.reset_token = cont::ConstSpan<u8>(held.reset_token, kQuicResetTokenBytes);
            if (!put(issued))
            {
                break;
            }
            QuicStreamRecord sent;
            sent.value = held.sequence;
            static_cast<void>(record(space, packet, kRecordNewId, sent, written));
            held.owed = false;
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
        // Datagrams after the connection's own frames and before stream data; one that does not fit waits.
        while (datagram_ready(now_ns))
        {
            u64 id = 0U;
            const usize bytes = m_datagrams.write_next(frames.subspan(used), now_ns, id);
            if (bytes == 0U)
            {
                break;
            }
            used += bytes;
            written.eliciting = true;
            if (m_datagrams.outcomes())
            {
                QuicStreamRecord sent;
                sent.value = id;
                static_cast<void>(record(space, packet, kRecordDatagram, sent, written)); // room was checked
            }
        }
        if (ack_later && !early)
        {
            put_ack();
        }
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
    put_owed_ping();
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

usize QuicConnection::write_packet(QuicSpace space, cont::Span<u8> room, usize datagram_used, bool pad, bool last,
                                   bool closing, u64 now_ns) noexcept
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
    spec.type = closing ? packet_type(space) : send_type(space);
    const bool early = spec.type == QuicPacketType::ZeroRtt;
    // 0-RTT packets keep the original version (RFC 9369 section 4.1).
    spec.version = static_cast<u32>(early ? m_protection.original_version() : m_protection.version());
    spec.dcid = peer_cid();
    if (m_peer_cid_current < m_peer_cid_count && !m_peer_cids[m_peer_cid_current].used)
    {
        m_peer_cids[m_peer_cid_current].used = true; // its token is checked from now on (section 10.3.1)
        watch(m_peer_cids[m_peer_cid_current]);
    }
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
    const usize used = closing ? write_close(space, writer.frames())
                               : write_frames(space, early, writer.frames(), number, now_ns, written);
    // A later packet that cannot fit (a 0-RTT packet behind a full Initial) leaves the padding to this one.
    const bool padded = (pad && (last || writer.frames().size() - used < kMinUsefulPacket)) || written.path_response;
    if (used == 0U && !padded)
    {
        return 0U;
    }
    const usize floor = padded ? min_u64(quic_datagram_floor(true, datagram_used), room.size()) : 0U;
    // Every datagram reaches kQuicResetPadding past this side's ID length, so a peer's stateless reset answering it
    // can be as long as a packet to this side (section 10.3).
    const usize reset_floor = m_local_cid.bytes + kQuicResetPadding;
    const usize least = (datagram_used < reset_floor) ? min_u64(reset_floor - datagram_used, room.size()) : 0U;
    usize bytes = 0U;
    const QuicPacketError sealed = m_protection.seal(writer, used, QuicPadding{max_u64(floor, least), m_config.padding_block}, bytes);
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
        if (sent.in_flight)
        {
            m_congestion->on_sent(congestion_packet(space, sent), m_recovery->bytes_in_flight(), now_ns);
            m_pacer.on_sent(bytes, now_ns, m_congestion->pacing_rate(m_recovery->rtt()), m_congestion->burst());
        }
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
        used += write_packet(space, out.subspan(used), used, pad, !later, true, now_ns);
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
    if (m_peer_known || m_early_sending)
    {
        limit = min_u64(limit, (m_peer_known ? m_peer : m_remembered).max_udp_payload_size);
    }
    const usize full = limit; // a datagram's size before the amplification limit: what the window must have room for
    const usize credit = amplification_credit();
    limit = min_u64(limit, credit);
    usize used = 0U;
    if (m_state == QuicConnectionState::Closing)
    {
        used = (limit >= kMinUsefulPacket) ? send_closing(out.first(limit), now_ns) : 0U;
    }
    else
    {
        pull_crypto();
        issue_local_ids();
        gate_data(full, now_ns);
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
        const cont::Span<u8> room = out.first(limit);
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
                used += write_packet(space, room.subspan(used), used, pad, space == last, false, now_ns);
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
    u64 next =
        min_u64(min_u64(m_recovery->loss_timer_ns(), m_idle_deadline_ns), min_u64(m_previous_key_ns, m_early_key_ns));
    next = min_u64(next, m_pace_release_ns);
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
            finish();
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
        finish();
        return;
    }
    if (now_ns >= m_previous_key_ns)
    {
        m_protection.discard_previous();
        m_previous_key_ns = kNeverNs;
    }
    if (now_ns >= m_early_key_ns)
    {
        m_protection.discard(crypto::QuicLevel::Early);
        m_early_key_ns = kNeverNs;
    }
    if (now_ns >= m_pace_release_ns)
    {
        m_pace_release_ns = kNeverNs; // send() writes what the pacer held, or arms it again
    }
    if (now_ns < m_recovery->loss_timer_ns())
    {
        return;
    }
    m_event_ns = now_ns;
    const QuicTimeout timeout = m_recovery->on_timeout(now_ns, m_events);
    if (timeout.lost.persistent_congestion)
    {
        m_congestion->on_persistent_congestion(now_ns);
    }
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
    if (timeout.space == QuicSpace::Application && m_config.side == crypto::QuicSide::Server && m_confirmed)
    {
        m_handshake_done_owed = m_handshake_done_owed || !m_handshake_done_acked;
        m_new_token_owed = m_new_token_owed || (m_new_token_bytes != 0U && !m_new_token_acked);
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
        finish();
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
