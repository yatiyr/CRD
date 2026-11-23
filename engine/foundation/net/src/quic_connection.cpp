// crd-net -- a QUIC connection (4.2a; contract: docs/design/network-foundation.md#quic-connection).
#include <crd/containers/wire_bytes.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/quic_connection.hpp>
#include <crd/net/wire.hpp>

#include <cstring>
#include <new>

namespace crd::net
{
namespace
{
constexpr u8 kRecordCrypto = 1U;
constexpr u8 kRecordHandshakeDone = 2U;
constexpr u8 kRecordRetire = 3U;     // a RETIRE_CONNECTION_ID frame: value is its sequence, stream_id its path ID
constexpr u8 kRecordStream = 4U;     // a frame of the stream set
constexpr u8 kRecordDatagram = 5U;   // a DATAGRAM frame whose acknowledgement or loss is passed on
constexpr u8 kRecordNewId = 6U;      // a NEW_CONNECTION_ID frame: value is its sequence, stream_id its path ID
constexpr u8 kRecordNewToken = 7U;   // a server's NEW_TOKEN frame
constexpr u8 kRecordObserved = 8U;   // an OBSERVED_ADDRESS frame: value is its sequence, length its path
constexpr u8 kRecordPathsBlocked = 9U;     // a PATHS_BLOCKED frame: value is its Maximum Path Identifier
constexpr u8 kRecordPathCidsBlocked = 10U; // a PATH_CIDS_BLOCKED frame: value is its sequence, stream_id its path ID
constexpr u8 kRecordPathAbandon = 11U;     // a PATH_ABANDON frame: stream_id is its path ID
constexpr u8 kRecordMaxPathId = 12U;       // a MAX_PATH_ID frame: value is its Maximum Path Identifier
constexpr u8 kRecordPathStatus = 13U;      // a PATH_STATUS frame: value is its sequence, stream_id its path ID
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
constexpr u64 kNoSequence = kQuicNoSequence; // a path with no peer connection ID to send to

// What a congestion controller sees of a packet counted in flight.
// The four bytes after a short header's Destination Connection ID of `dcid_bytes`: the protected packet number and
// the start of the protected payload, which no one off the path can know (QuicSentPacket::tag); never 0, and 0 for
// bytes too short to hold them.
[[nodiscard]] u32 packet_tag(cont::ConstSpan<u8> packet, usize dcid_bytes) noexcept
{
    u32 tag = 0U;
    if (packet.size() < 1U + dcid_bytes + sizeof(tag))
    {
        return 0U;
    }
    std::memcpy(&tag, packet.data() + 1U + dcid_bytes, sizeof(tag));
    return (tag == 0U) ? 1U : tag;
}

// What the handshake's path starts from.
[[nodiscard]] QuicPathControlConfig path_config(const QuicConnectionConfig& config) noexcept
{
    QuicPathControlConfig out;
    out.congestion = config.congestion;
    out.base_datagram_bytes = config.base_datagram_bytes;
    out.max_datagram_bytes = config.max_datagram_bytes;
    out.path_mtu_discovery = config.path_mtu_discovery;
    out.ecn = config.ecn;
    out.handover = config.handover;
    out.handover_learning = config.handover_learning;
    return out;
}

// What a path not yet searched is known to carry: its family's floor (RFC 8899 section 5.2), within `most`.
[[nodiscard]] u32 family_base(const QuicPath& path, u32 most) noexcept
{
    const u32 base =
        (path.peer.address.unmapped().family == AddressFamily::V6) ? kQuicIpv6BaseDatagram : kQuicPmtuFloor;
    return (base < most) ? base : most;
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

[[nodiscard]] constexpr u64 three_times(u64 period) noexcept
{
    return (period > kNeverNs / 3U) ? kNeverNs : 3U * period;
}

// A preferred address's family is given when its address and port are not all zeros (section 18.2).
[[nodiscard]] bool given(cont::ConstSpan<u8> address, u16 port) noexcept
{
    bool any = false;
    for (const u8 byte : address)
    {
        any = any || byte != 0U;
    }
    return any && port != 0U;
}

// The preferred address of `like`'s family, in its form (an IPv4 address mapped where `like` is); port 0 for none.
[[nodiscard]] Endpoint preferred_endpoint(const QuicPreferredAddress& preferred, const IpAddress& like) noexcept
{
    Endpoint out;
    if (like.unmapped().family == AddressFamily::V4)
    {
        if (given(cont::ConstSpan<u8>(preferred.ipv4, 4U), preferred.ipv4_port))
        {
            const u8* const a = preferred.ipv4;
            out.address = IpAddress::v4(a[0], a[1], a[2], a[3]);
            out.address = (like.family == AddressFamily::V6) ? out.address.mapped() : out.address;
            out.port = preferred.ipv4_port;
        }
        return out;
    }
    if (given(cont::ConstSpan<u8>(preferred.ipv6, 16U), preferred.ipv6_port))
    {
        out.address = IpAddress::v6(preferred.ipv6);
        out.port = preferred.ipv6_port;
    }
    return out;
}

[[nodiscard]] bool config_valid(const QuicConnectionConfig& config) noexcept
{
    const QuicTransportParameters& p = config.parameters;
    const QuicPreferredAddress& preferred = p.preferred_address;
    // A server's, with an address of either family, and its ID and token left to the connection.
    const bool preferred_ok =
        !p.has_preferred_address ||
        (config.side == crypto::QuicSide::Server && config.tls != nullptr && preferred.connection_id.bytes == 0U &&
         (given(cont::ConstSpan<u8>(preferred.ipv4, 4U), preferred.ipv4_port) ||
          given(cont::ConstSpan<u8>(preferred.ipv6, 16U), preferred.ipv6_port)));
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
    return keyed_ok && preferred_ok && version_ok && listed && config.versions.size() < kQuicMaxAvailableVersions &&
           p.initial_max_data == 0U && p.initial_max_stream_data_bidi_local == 0U &&
           p.initial_max_stream_data_bidi_remote == 0U && p.initial_max_stream_data_uni == 0U &&
           p.initial_max_streams_bidi == 0U && p.initial_max_streams_uni == 0U && p.max_datagram_frame_size == 0U &&
           p.max_receive_timestamps_per_ack <= kQuicMaxReceiveTimestamps &&
           p.receive_timestamps_exponent <= kQuicMaxReceiveTimestampsExponent &&
           p.address_discovery <= QuicAddressDiscovery::Both && !p.has_stateless_reset_token &&
           (!p.has_initial_max_path_id || p.initial_max_path_id < kQuicMaxPaths) &&
           config.path_choice <= QuicPathChoice::Turns &&
           QuicStreamSet::limits_valid(config.streams) && QuicDatagrams::limits_valid(config.datagrams) &&
           p.active_connection_id_limit >= kQuicDefaultActiveIdLimit &&
           p.active_connection_id_limit <= kQuicMaxPeerConnectionIds && config.sent_packets != 0U &&
           config.sent_packets <= kMaxSentPackets && config.sent_frames != 0U &&
           config.sent_frames <= kMaxSentPackets && config.crypto_bytes % kQuicStreamChunk == 0U &&
           config.crypto_bytes >= kMinCryptoBytes && config.crypto_bytes <= kMaxCryptoBytes &&
           config.max_datagram_bytes >= kQuicMinInitialDatagram &&
           config.max_datagram_bytes <= kQuicDefaultMaxUdpPayload && config.base_datagram_bytes >= kQuicPmtuFloor &&
           config.base_datagram_bytes <= config.max_datagram_bytes && congestion_policy_valid(config.congestion) &&
           handover_schedule_valid(config.handover) && handover_learning_valid(config.handover_learning) &&
           (config.handover.period_ns == 0U || config.handover_learning.period_ns == 0U) &&
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

// ---- Events ----

void QuicConnection::Events::on_acked(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Acked);
    if (!m_connection.m_recovery->on_path(space, packet))
    {
        return; // sent on another path, or on a slot now closed: it says nothing of this one (RFC 9000 section 9.4)
    }
    const u64 received_ns =
        (space == QuicSpace::Application) ? m_connection.m_peer_times.find(packet.number) : kNeverNs;
    m_connection.control(packet.path).on_acked(space, packet, received_ns, m_connection.m_event_ns);
}

void QuicConnection::Events::on_lost(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Lost);
    if (m_connection.m_recovery->on_path(space, packet))
    {
        m_connection.count(m_connection.control(packet.path).on_lost(space, packet, m_connection.m_event_ns));
    }
}

void QuicConnection::Events::on_spurious(QuicSpace space, const QuicSentPacket& packet, u64 lost_ns) noexcept
{
    if (m_connection.m_recovery->on_path(space, packet))
    {
        m_connection.control(packet.path).on_spurious(space, packet, lost_ns, m_connection.m_event_ns);
    }
}

void QuicConnection::Events::on_discarded(QuicSpace space, const QuicSentPacket& packet) noexcept
{
    m_connection.settle(space, packet, Settle::Discarded);
    if (m_connection.m_recovery->on_path(space, packet))
    {
        m_connection.control(packet.path).on_discarded(packet);
    }
}

// ---- Construction ----

QuicConnection::QuicConnection(memory::IAllocator& allocator, const QuicConnectionConfig& config) noexcept
    : m_allocator(allocator), m_config(config), m_events(*this), m_hello(*this), m_cids(config.ids)
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
    // Further path slots only for a limit above path ID 0 (RFC 10069 section 2.1).
    const bool further = config.parameters.has_initial_max_path_id && config.parameters.initial_max_path_id != 0U;
    PathSlot* const slots = further ? memory::try_construct_array<PathSlot>(allocator, kFurtherSlots) : nullptr;
    QuicSentEntry* const slot_entries =
        further ? memory::try_construct_array<QuicSentEntry>(allocator, kFurtherSlots * per_space) : nullptr;
    void* const recovery = allocator.try_allocate(sizeof(QuicRecovery), alignof(QuicRecovery));
    if (seeded != crypto::CryptoError::None || entries == nullptr || frames == nullptr || bytes == nullptr ||
        bits == nullptr || recovery == nullptr || !streams || !datagrams || (early_bytes != 0U && early == nullptr) ||
        (held_bytes != 0U && (held == nullptr || held_entries == nullptr)) ||
        (further && (slots == nullptr || slot_entries == nullptr)))
    {
        memory::destroy_array(allocator, entries, kQuicSpaces * per_space);
        memory::destroy_array(allocator, frames, config.sent_frames);
        memory::destroy_array(allocator, bytes, kQuicSpaces * 2U * crypto);
        memory::destroy_array(allocator, bits, kQuicSpaces * 3U * words);
        memory::destroy_array(allocator, static_cast<u8*>(recovery), sizeof(QuicRecovery));
        memory::destroy_array(allocator, early, early_bytes);
        memory::destroy_array(allocator, held, held_bytes);
        memory::destroy_array(allocator, held_entries, config.held_packets);
        memory::destroy_array(allocator, slots, further ? kFurtherSlots : 0U);
        memory::destroy_array(allocator, slot_entries, further ? kFurtherSlots * per_space : 0U);
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
    // The policy, the sizes and the handover were checked (config_valid).
    static_cast<void>(m_first_control.start(path_config(config), config.path, entropy(), &m_coupling));
    m_datagram_bytes = m_first_control.datagram_bytes();
    m_first_paths.paths.open(config.path, false, 0U);
    m_slots = slots;
    m_slot_entries = slot_entries;
    for (usize slot = 1U; further && slot < kQuicMaxPaths; ++slot)
    {
        m_tuples[slot] = &m_slots[slot - 1U].tuples;
    }
    m_frames = frames;
    m_crypto_bytes = bytes;
    m_crypto_bits = bits;
    m_storage = entries;
}

QuicConnection::~QuicConnection() noexcept
{
    finish();
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
    const usize further = (m_slots != nullptr) ? kFurtherSlots : 0U;
    memory::destroy_array(m_allocator, m_slots, further);
    memory::destroy_array(m_allocator, m_slot_entries, further * (usize{m_config.sent_packets} + kSentPacketReserve));
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
        m_config.tls == nullptr || initial_dcid.size() < 8U ||
        initial_dcid.size() > crypto::kQuicMaxConnectionIdBytes ||
        local_cid.size() > crypto::kQuicMaxConnectionIdBytes ||
        (m_config.parameters.has_initial_max_path_id && local_cid.empty()))
    {
        // An end offering multipath uses connection IDs of non-zero length (RFC 10069 section 2.1).
        return false;
    }
    static_cast<void>(m_local_cid.assign(local_cid)); // both bounded above
    static_cast<void>(m_initial_dcid.assign(initial_dcid));
    m_cids.first_local(m_local_cid.view(), {});
    m_original_dcid = m_initial_dcid;
    // A client's sending is not limited by its address.
    m_first_paths.paths.set_validated(m_first_paths.paths.active());
    return start(now_ns);
}

bool QuicConnection::accept(const QuicAccept& accept, u64 now_ns) noexcept
{
    const auto bounded = [](cont::ConstSpan<u8> id)
    {
        return id.size() <= crypto::kQuicMaxConnectionIdBytes;
    };
    if (!ready() || m_state != QuicConnectionState::Idle || m_config.side != crypto::QuicSide::Server ||
        m_config.tls == nullptr || accept.local_cid.empty() || accept.client_dcid.empty() ||
        !bounded(accept.local_cid) || !bounded(accept.client_dcid) || !bounded(accept.original_dcid) ||
        !bounded(accept.client_scid) ||
        (!accept.retried && !wire::wire_same_bytes(accept.original_dcid, accept.client_dcid)))
    {
        return false;
    }
    u8 token[kQuicResetTokenBytes] = {};
    if (m_config.ids != nullptr && !m_config.ids->token(accept.local_cid, cont::Span<u8>(token, sizeof(token))))
    {
        return false;
    }
    static_cast<void>(m_local_cid.assign(accept.local_cid)); // each bounded above
    m_cids.first_local(m_local_cid.view(), cont::ConstSpan<u8>(token, sizeof(token)));
    static_cast<void>(m_initial_dcid.assign(accept.client_dcid));
    static_cast<void>(m_original_dcid.assign(accept.original_dcid));
    static_cast<void>(m_peer_scid.assign(accept.client_scid));
    m_retried = accept.retried;
    if (accept.validated)
    {
        m_first_paths.paths.set_validated(m_first_paths.paths.active());
    }
    m_peer_scid_known = true;
    m_cids.first_peer(m_peer_scid.view());
    // The preferred address's ID is sequence 1 (section 5.1.1), routed as any other; the parameters carry it.
    if (m_config.parameters.has_preferred_address && m_config.path.socket != m_config.preferred_socket &&
        !m_cids.issue(0U, true))
    {
        return false;
    }
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
        const QuicLocalCid* first = m_cids.local(0U, 0U);
        m_local.has_stateless_reset_token = m_config.ids != nullptr && first != nullptr;
        if (first != nullptr)
        {
            std::memcpy(m_local.stateless_reset_token, first->reset_token, kQuicResetTokenBytes);
        }
        // Sequence 1 is the preferred address's (accept()), absent when the connection opened on its socket.
        const QuicLocalCid* preferred = m_cids.local(0U, 1U);
        m_local.has_preferred_address = preferred != nullptr;
        if (preferred != nullptr)
        {
            m_local.preferred_address.connection_id = preferred->id;
            std::memcpy(m_local.preferred_address.reset_token, preferred->reset_token, kQuicResetTokenBytes);
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
    m_cids.first_local(m_local_cid.view(), cont::ConstSpan<u8>(reset_token, kQuicResetTokenBytes));
    m_cids.first_peer(m_peer_scid.view());
    fill_local_parameters();

    u8 hello[kKeyedHelloPrefix + crypto::kQuicMaxTransportParameterBytes];
    const usize bytes = encode_parameters(cont::Span<u8>(hello + kKeyedHelloPrefix, sizeof(hello) - kKeyedHelloPrefix));
    hello[0] = static_cast<u8>(bytes >> 8U);
    hello[1] = static_cast<u8>(bytes);
    const usize whole = kKeyedHelloPrefix + bytes;
    if (bytes == 0U ||
        m_levels[quic_space_index(QuicSpace::Application)].out.write(cont::ConstSpan<u8>(hello, whole)) != whole)
    {
        return false;
    }
    m_keyed = true;
    if (side == crypto::QuicSide::Client)
    {
        // A server's limit lasts until its client acknowledges a packet.
        m_first_paths.paths.set_validated(m_first_paths.paths.active());
    }
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

u32 QuicConnection::peer_ceiling() const noexcept
{
    return static_cast<u32>(min_u64(m_peer.max_udp_payload_size, kQuicDefaultMaxUdpPayload));
}

void QuicConnection::apply_peer_limits(const QuicTransportParameters& peer) noexcept
{
    m_streams.set_peer_parameters(peer);
    if (&peer == &m_peer)
    {
        for (QuicPathControl* const control : m_controls)
        {
            if (control != nullptr)
            {
                control->limit(peer_ceiling());
            }
        }
        follow_datagram_size();
    }
    m_datagrams.set_peer(peer.max_datagram_frame_size, min_u64(m_datagram_bytes, peer.max_udp_payload_size));
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
    // A token for a validated path the client moved to replaces the one for its address before (RFC 9000 section 9.3).
    const bool replaces = m_new_token_bytes != 0U && !(m_new_token_path == path()) && address_validated();
    if (m_config.side != crypto::QuicSide::Server || !m_peer_known || token.empty() ||
        token.size() > kQuicMaxCarriedTokenBytes || (m_new_token_bytes != 0U && !replaces) ||
        (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established))
    {
        return false;
    }
    std::memcpy(m_new_token, token.data(), token.size());
    m_new_token_bytes = token.size();
    m_new_token_path = path();
    ++m_new_token_generation;
    m_new_token_owed = m_confirmed;
    m_new_token_acked = false;
    return true;
}

// ---- Receiving ----

void QuicConnection::receive(cont::Span<u8> datagram, Ecn ecn, u64 now_ns) noexcept
{
    receive(path(), datagram, ecn, now_ns);
}

void QuicConnection::receive(const QuicPath& path, cont::Span<u8> datagram, Ecn ecn, u64 now_ns) noexcept
{
    if (m_state == QuicConnectionState::Idle || m_state == QuicConnectionState::Closed)
    {
        return;
    }
    ++m_counters.datagrams_received;
    m_counters.bytes_received += datagram.size();
    // A datagram is one path slot's, its first packet's (RFC 10069 section 2), read among that slot's 4-tuples.
    m_receive_slot = datagram_slot(datagram);
    if (m_receive_slot == kQuicNoPath)
    {
        m_receive_slot = begin_path(datagram, path, now_ns); // a path ID the client opens (section 3.1)
    }
    if (m_receive_slot == kQuicNoPath)
    {
        m_receive_slot = 0U;
        ++m_counters.packets_dropped;
        return;
    }
    QuicPaths& paths = tuples(m_receive_slot);
    const usize active = paths.active();
    // An unbound path counts every datagram toward itself until the first packet that opens names it.
    const QuicPath& current = paths.path(active);
    m_receive_path = (current.peer.port == 0U) ? active : paths.find(path);
    m_candidate = path;
    // A server's address changes only to its preferred one (sections 9 and 9.6.2): its other sockets are not the
    // connection's, nor, once it moved, the one it left.
    const bool preferred_socket = handshake_slot(m_receive_slot) && m_local.has_preferred_address && !m_on_preferred &&
                                  path.socket == m_config.preferred_socket;
    if (m_config.side == crypto::QuicSide::Server && current.peer.port != 0U && path.socket != current.socket &&
        !preferred_socket)
    {
        ++m_counters.foreign_datagrams;
        return;
    }
    if (m_receive_path == kQuicNoPath && !follows_peer())
    {
        ++m_counters.foreign_datagrams; // a client's unknown server, or a server not yet confirmed (section 9)
        return;
    }
    if (m_receive_path != kQuicNoPath)
    {
        paths.on_received(m_receive_path, datagram.size(), now_ns);
    }
    if (!handshake_validated())
    {
        m_recovery->set_amplification_blocked(amplification_credit() < kMinUsefulPacket, now_ns);
    }
    if (m_state == QuicConnectionState::Draining)
    {
        return;
    }
    if (m_state == QuicConnectionState::Closing)
    {
        if (m_receive_slot != lead_slot() || m_receive_path != active)
        {
            return; // the close answers the leading slot's active path alone
        }
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
    if (m_tentative_slot != kQuicNoPath)
    {
        undo_path(m_tentative_slot); // its first packet did not authenticate: the path ID stays unopened
    }
    // The comparison is owed when the first packet cannot be associated or opened; it is skipped otherwise, and for a
    // path that no packet opened on.
    if (!first_opened && reset_sized && m_receive_path != kQuicNoPath &&
        reset_by(cont::ConstSpan<u8>(tail, kQuicResetTokenBytes), now_ns))
    {
        return;
    }
    read_held(now_ns);
}

void QuicConnection::hold(QuicPacketType type, cont::ConstSpan<u8> packet, Ecn ecn) noexcept
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

bool QuicConnection::path_too_big(cont::ConstSpan<u8> quote, u32 limit, u64 now_ns) noexcept
{
    return path_too_big(path(), quote, limit, now_ns);
}

bool QuicConnection::path_too_big(const QuicPath& from, cont::ConstSpan<u8> quote, u32 limit, u64 now_ns) noexcept
{
    const cont::ConstSpan<u8> dcid = peer_cid(lead_slot()); // path()'s
    const bool ours = m_state == QuicConnectionState::Established && from == path() &&
                      quote.size() >= 1U + dcid.size() && (quote[0] & 0x80U) == 0U &&
                      (dcid.empty() || std::memcmp(quote.data() + 1U, dcid.data(), dcid.size()) == 0);
    const QuicSentPacket* const packet =
        ours ? m_recovery->unacknowledged(QuicSpace::Application, packet_tag(quote, dcid.size()), lead_slot())
             : nullptr;
    if (packet == nullptr || !m_recovery->on_path(QuicSpace::Application, *packet))
    {
        return false;
    }
    control(packet->path).on_too_big(packet->bytes, limit, now_ns);
    follow_datagram_size();
    return true;
}

bool QuicConnection::reset_by(cont::ConstSpan<u8> tail, u64 now_ns) noexcept
{
    if (m_state != QuicConnectionState::Handshaking && m_state != QuicConnectionState::Established &&
        m_state != QuicConnectionState::Closing)
    {
        return false;
    }
    if (!m_cids.reset_by(tail))
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

bool QuicConnection::receive_packet(const QuicHeader& header, cont::Span<u8> packet, usize datagram_bytes, Ecn ecn,
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
    m_packet_local = m_cids.find(header.dcid);
    bool routed =
        m_packet_local.found() || (!client && long_header && wire::wire_same_bytes(header.dcid, m_initial_dcid.view()));
    if (long_header)
    {
        // Retry is taken above; other versions are not this connection's, and a server's Initial carries no token
        // (section 17.2.2).
        routed = routed && header.type != QuicPacketType::Retry && header.type != QuicPacketType::OtherVersion &&
                 version_routed(header) &&
                 (!m_peer_scid_known || wire::wire_same_bytes(header.scid, m_peer_scid.view())) &&
                 (!client || header.type != QuicPacketType::Initial || header.token.empty());
    }
    // A server discards a client Initial in a datagram under 1200 bytes (section 14.1); a path not known opens only a
    // 1-RTT packet (section 9).
    if (!routed || (!client && header.type == QuicPacketType::Initial && datagram_bytes < kQuicMinInitialDatagram) ||
        (m_receive_path == kQuicNoPath && header.type != QuicPacketType::OneRtt))
    {
        ++m_counters.packets_dropped;
        return false;
    }
    if (!m_protection.can_open(header.type) && m_protection.keys_pending(header.type))
    {
        hold(header.type, packet, ecn);
        return false;
    }
    // A packet decodes and opens in its datagram's path slot: a long header in the handshake's, slot 0, a 1-RTT
    // packet in its ID's path ID's (RFC 10069 section 2). One of another slot is dropped.
    if ((long_header ? 0U : slot_of(m_packet_local.path_id)) != m_receive_slot)
    {
        ++m_counters.packets_dropped;
        return false;
    }
    QuicOpened opened;
    const QuicPacketError error = m_protection.open(header, packet, opened, m_receive_slot);
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
    if (m_receive_slot == m_tentative_slot)
    {
        commit_path(m_receive_slot, now_ns);
    }
    ++m_counters.packets_opened;
    QuicPaths& paths = tuples(m_receive_slot);
    if (m_receive_path == kQuicNoPath)
    {
        admit_path(m_receive_slot, datagram_bytes, now_ns);
    }
    else if (m_receive_path == paths.active() && paths.path(m_receive_path).peer.port == 0U)
    {
        paths.bind(m_candidate);
        if (m_controls[m_receive_slot] != nullptr)
        {
            control(m_receive_slot).bind(m_candidate); // a stopped slot sends nothing more
        }
    }
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
        m_cids.first_peer(m_peer_scid.view());
    }
    if (!client && opened.type == QuicPacketType::Handshake)
    {
        // A Handshake packet proves the client's address (section 8.1).
        m_first_paths.paths.set_validated(m_first_paths.paths.active());
        m_recovery->set_amplification_blocked(false, now_ns);
    }
    if (!m_protection.can_open(QuicPacketType::Initial))
    {
        discard_space(QuicSpace::Initial, now_ns);
    }

    cont::WireReader in(opened.payload);
    bool eliciting = false;
    bool probing = true; // only PATH_CHALLENGE, PATH_RESPONSE, NEW_CONNECTION_ID and PADDING (section 9.1)
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
        probing = probing && quic_frame_probing(read.kind);
        if (!on_frame(read, space, now_ns))
        {
            return true;
        }
    }
    if (space == QuicSpace::Application && !stopped(m_receive_slot))
    {
        follow_packet(m_receive_slot, opened.number, probing, now_ns); // its path ID's 4-tuples (S75)
    }
    if (!m_space_gone[quic_space_index(space)])
    {
        static_cast<void>(m_recovery->on_received(space, opened.number, eliciting, ecn, now_ns, m_receive_slot));
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
            return ack_frame(frame, space, now_ns);
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
            tuples(m_receive_slot).on_challenge(m_receive_path, frame.data);
            // A challenge on the active path is answered with a non-probing packet as well (section 9.3.3). On a
            // further slot the response rides with that slot's PATH_ACK, which is not probing either: a PING would
            // go on the handshake slot's path, which the challenge says nothing of.
            m_ping_owed = m_ping_owed ||
                          (handshake_slot(m_receive_slot) && !stopped(0U) && m_receive_path == tuples(0U).active());
            return true;
        case QuicFrameKind::ObservedAddress:
            return observed_address_frame(frame, now_ns);
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
            return retire_local(0U, frame.sequence, now_ns);
        case QuicFrameKind::PathResponse:
        {
            // One that answers no live challenge of its path ID's is late or never asked for, and ignored (section
            // 19.18's MAY left; RFC 10069 section 3.1 sends it with an ID of the challenge's path ID).
            usize index = kQuicNoPath;
            if (tuples(m_receive_slot).on_response(frame.data, index) == QuicPathResponse::Validated)
            {
                on_validated(m_receive_slot, index, now_ns);
            }
            return true;
        }
        case QuicFrameKind::PathAck:
        case QuicFrameKind::PathAbandon:
        case QuicFrameKind::PathStatus:
        case QuicFrameKind::PathNewConnectionId:
        case QuicFrameKind::PathRetireConnectionId:
        case QuicFrameKind::MaxPathId:
        case QuicFrameKind::PathsBlocked:
        case QuicFrameKind::PathCidsBlocked:
            return multipath_frame(frame, space, now_ns);
    }
    return true;
}

bool QuicConnection::ack_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept
{
    if (frame.ack.timestamps)
    {
        // draft-ietf-quic-receive-ts-03: never asked for, the frame is of no known type (as RFC 9000 section
        // 12.4 reads an unnegotiated extension's frame); outside 1-RTT packets or past the maximum asked
        // for, it breaks the draft's sections 4 and 6.
        if (m_local.max_receive_timestamps_per_ack == 0U)
        {
            fail(QuicTransportError::FrameEncodingError, 0U, now_ns);
            return false;
        }
        if (space != QuicSpace::Application || frame.ack.timestamp_count > m_local.max_receive_timestamps_per_ack)
        {
            fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
            return false;
        }
    }
    // An ACK frame's packets are path ID 0's, a PATH_ACK's its path ID's (RFC 10069 section 2.3), received through
    // the packet's own slot. A path ID whose state is gone is silently ignored (section 4).
    const bool application = space == QuicSpace::Application;
    const usize slot = application ? slot_of(frame.kind == QuicFrameKind::PathAck ? frame.path_id : 0U) : 0U;
    if (slot != kQuicNoPath && stopped(slot))
    {
        return true; // abandoned: its packets went back to the queues as lost when it stopped (section 3.4.3)
    }
    if (slot == kQuicNoPath)
    {
        // A path ID whose ID space is open and not abandoned never had a number space here: none of its packets were
        // sent to acknowledge (RFC 9000 section 13.1). A path ID whose state is gone is ignored.
        if (frame.kind == QuicFrameKind::PathAck && m_cids.is_open(frame.path_id) && !m_cids.abandoned(frame.path_id))
        {
            fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
            return false;
        }
        return true;
    }
    if (frame.ack.timestamps)
    {
        // Path ID 0's packets, read in its own slot: never those of a later path ID slot 0 was reclaimed for.
        m_counters.receive_timestamps +=
            m_peer_times.read(frame.ack, m_local.receive_timestamps_exponent, *m_recovery, now_ns);
    }
    const usize via = application ? m_receive_slot : 0U;
    m_event_ns = now_ns;
    QuicPathControl& acknowledged = control(slot);
    acknowledged.on_ack_begin();
    const QuicAckOutcome outcome = m_recovery->on_ack(space, frame.ack, now_ns, m_events, slot, via);
    m_peer_times.clear();
    if (outcome.error == QuicRecoveryError::AckOfUnsent)
    {
        fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
        return false;
    }
    if (outcome.error == QuicRecoveryError::None)
    {
        count(acknowledged.on_ack_end(space, frame.ack, outcome, m_recovery->rtt(slot),
                                      m_recovery->bytes_in_flight(slot), now_ns));
        follow_datagram_size();
    }
    if (application)
    {
        m_protection.acknowledged(frame.ack.largest, slot);
    }
    if (m_keyed && !handshake_validated() && outcome.acked_packets != 0U)
    {
        // Only an end that read a packet sent to this address can acknowledge it.
        m_first_paths.paths.set_validated(m_first_paths.paths.active());
        m_recovery->set_amplification_blocked(false, now_ns);
    }
    return true;
}

bool QuicConnection::multipath_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept
{
    if (!m_multipath)
    {
        // RFC 10069 section 2: not offered by both ends, so of no known type (RFC 9000 section 12.4).
        fail(QuicTransportError::FrameEncodingError, 0U, now_ns);
        return false;
    }
    // This side's limit, raised as path IDs close (design S78): a path ID above it, or a PATHS_BLOCKED frame naming a
    // larger one, is PROTOCOL_VIOLATION (sections 4 and 4.7).
    const u64 local_max = m_local_max_path_id;
    bool above = frame.path_id > local_max;
    if (frame.kind == QuicFrameKind::PathsBlocked)
    {
        above = frame.value > local_max;
    }
    else if (frame.kind == QuicFrameKind::MaxPathId)
    {
        above = frame.value < m_peer.initial_max_path_id;
    }
    if (above)
    {
        fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
        return false;
    }
    switch (frame.kind)
    {
        case QuicFrameKind::PathAck:
            return ack_frame(frame, space, now_ns);
        case QuicFrameKind::PathAbandon:
            return peer_abandon(frame, now_ns);
        case QuicFrameKind::PathStatus:
        {
            // One sequence space for both frames per path ID, an older or repeated one ignored (section 4.3); any
            // path ID under the limit may be named, open or not yet, and one abandoned or closed is ignored (section
            // 4).
            if (!m_cids.is_open(frame.path_id) || m_cids.abandoned(frame.path_id))
            {
                return true;
            }
            PathStatusEntry* const entry = status_entry(m_peer_statuses, frame.path_id);
            if (entry != nullptr && (entry->status == QuicPathStatus::Unknown || frame.sequence > entry->sequence))
            {
                *entry = PathStatusEntry{frame.path_id, frame.sequence,
                                         frame.backup ? QuicPathStatus::Backup : QuicPathStatus::Available, false};
            }
            return true;
        }
        case QuicFrameKind::PathNewConnectionId:
            return new_connection_id(frame, now_ns);
        case QuicFrameKind::PathRetireConnectionId:
            return retire_local(frame.path_id, frame.sequence, now_ns);
        case QuicFrameKind::MaxPathId:
            // A frame that does not raise the limit is late or repeated, and ignored (section 4.6); one that does ends
            // a PATHS_BLOCKED owed at the old limit.
            m_paths_blocked_owed = m_paths_blocked_owed && frame.value <= m_peer_max_path_id;
            m_peer_max_path_id = max_u64(m_peer_max_path_id, frame.value);
            return true;
        case QuicFrameKind::PathsBlocked:
            ++m_counters.paths_blocked;
            return true; // informs only (section 4.7); this side's limit rises as path IDs close
        case QuicFrameKind::PathCidsBlocked:
            // Informs only, but may not name a sequence this side has not reached (section 4.7); IDs are topped up
            // to the peer's limit whenever one is retired. A path ID abandoned or closed is ignored (section 4).
            if (!m_cids.is_open(frame.path_id) || m_cids.abandoned(frame.path_id))
            {
                return true;
            }
            if (frame.sequence > m_cids.local_next(frame.path_id))
            {
                fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
                return false;
            }
            ++m_counters.paths_blocked;
            return true;
        default:
            return true; // never reached: on_frame routes RFC 10069's kinds alone here
    }
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
    Level& level = m_levels[quic_space_index(space)];
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
    if (error == QuicTransportError::NoError)
    {
        error = quic_check_multipath(m_local, m_peer, m_peer_scid.bytes == 0U || m_local_cid.bytes == 0U);
    }
    if (error != QuicTransportError::NoError)
    {
        fail(error, 0U, now_ns);
        return false;
    }
    m_peer_known = true;
    m_multipath = quic_multipath_negotiated(m_local, m_peer);
    m_peer_max_path_id = m_multipath ? m_peer.initial_max_path_id : 0U;
    m_local_max_path_id = m_multipath ? m_local.initial_max_path_id : 0U;
    open_id_spaces();
    apply_peer_limits(m_peer);
    m_recovery->set_peer_parameters(m_peer.max_ack_delay * kNsPerMs, m_peer.ack_delay_exponent, now_ns);
    // Timestamps are reported only by a side that asks for them too: the draft is behind its NET-STD flag.
    m_recovery->set_receive_timestamps(
        (m_local.max_receive_timestamps_per_ack == 0U) ? 0U : m_peer.max_receive_timestamps_per_ack,
        m_peer.receive_timestamps_exponent);
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
        m_cids.peer_reset_token(cont::ConstSpan<u8>(m_peer.stateless_reset_token, kQuicResetTokenBytes));
    }
    if (m_config.side == crypto::QuicSide::Client && m_peer.has_preferred_address)
    {
        // The preferred address's ID is the server's sequence 1 with its token (section 5.1.1), held and checked as
        // a NEW_CONNECTION_ID frame's would be.
        QuicFrame issued;
        issued.kind = QuicFrameKind::NewConnectionId;
        issued.sequence = 1U;
        issued.data = m_peer.preferred_address.connection_id.view();
        issued.reset_token = cont::ConstSpan<u8>(m_peer.preferred_address.reset_token, kQuicResetTokenBytes);
        if (!new_connection_id(issued, now_ns))
        {
            return false;
        }
    }
    restart_idle(now_ns);
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
    bool added = false;
    const QuicTransportError error = m_cids.add_peer(frame, m_local.active_connection_id_limit, added);
    if (error != QuicTransportError::NoError)
    {
        fail(error, 0U, now_ns);
        return false;
    }
    // The paths of the ID's path ID that wait for one (NEW_CONNECTION_ID's is 0).
    const usize slot = slot_of(frame.path_id);
    if (added && slot != kQuicNoPath)
    {
        address_paths(slot, now_ns);
        if (slot != m_receive_slot)
        {
            follow_waiting(slot, now_ns); // in its own slot's packet, follow_packet() decides once the frames are read
        }
    }
    return true;
}

bool QuicConnection::retire_local(u64 path_id, u64 sequence, u64 now_ns) noexcept
{
    const QuicTransportError error = m_cids.retire_local(path_id, sequence, m_packet_local);
    if (error != QuicTransportError::NoError)
    {
        fail(error, 0U, now_ns);
        return false;
    }
    return true; // send() issues the replacement (section 5.1.1)
}

void QuicConnection::issue_local_ids() noexcept
{
    if (m_confirmed && m_peer_known && m_state == QuicConnectionState::Established)
    {
        m_cids.top_up(m_peer.active_connection_id_limit, m_peer_max_path_id);
    }
}

void QuicConnection::open_id_spaces() noexcept
{
    // Rising path IDs from 1, one ID space of the pool each: within kQuicMaxPaths, never consumed before. A path ID
    // that closes hands its storage to the next one (close_abandoned).
    for (u64 path_id = 1U; m_multipath && m_slots != nullptr && path_id <= m_local.initial_max_path_id; ++path_id)
    {
        static_cast<void>(m_cids.open(path_id, m_slots[path_id - 1U].ids));
    }
}

bool QuicConnection::rotate_connection_ids() noexcept
{
    return m_confirmed && m_peer_known && m_state == QuicConnectionState::Established &&
           m_cids.rotate(0U, m_peer.active_connection_id_limit);
}

void QuicConnection::finish() noexcept
{
    m_state = QuicConnectionState::Closed;
    m_cids.finish();
}

void QuicConnection::version_negotiation(const QuicHeader& header, cont::ConstSpan<u8> packet) noexcept
{
    // Only a client before any packet of its server opened, for the IDs it sent (RFC 9000 section 6.2).
    if (m_config.side != crypto::QuicSide::Client || m_peer_scid_known ||
        !wire::wire_same_bytes(header.dcid, m_local_cid.view()) ||
        !wire::wire_same_bytes(header.scid, m_initial_dcid.view()))
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
        !wire::wire_same_bytes(header.dcid, m_local_cid.view()) ||
        wire::wire_same_bytes(header.scid, m_initial_dcid.view()) || packet.size() <= body + crypto::kQuicTagBytes ||
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
    m_first_control.reset(now_ns);
    for (Numbering& numbers : m_numbers)
    {
        numbers.probes = 0U;
    }
    QuicSendBuffer& out = m_levels[quic_space_index(QuicSpace::Initial)].out;
    if (out.sent_end() > out.acked_end())
    {
        out.on_lost(out.acked_end(), static_cast<usize>(out.sent_end() - out.acked_end()), false);
    }
    return true;
}

void QuicConnection::discard_space(QuicSpace space, u64 now_ns) noexcept
{
    const usize index = quic_space_index(space);
    if (m_space_gone[index])
    {
        return;
    }
    m_space_gone[index] = true;
    m_numbers[number_space(space, 0U)].probes = 0U; // never the Application space, which is never discarded
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
    follow_preferred(now_ns);
}

// ---- Sending ----

bool QuicConnection::can_acknowledge(QuicSpace space, usize slot) const noexcept
{
    return !m_space_gone[quic_space_index(space)] && m_protection.can_send(packet_type(space)) &&
           m_recovery->can_send(space, slot) && amplification_credit(slot) >= kMinUsefulPacket && addressed(slot);
}

bool QuicConnection::addressed(usize slot) const noexcept
{
    return handshake_slot(slot) || peer_held(slot, m_cids.current(m_protection.path_id(slot)));
}

bool QuicConnection::carries_data(usize slot, usize index) const noexcept
{
    const QuicPaths& paths = tuples(slot);
    return handshake_slot(slot) ||
           (m_config.side == crypto::QuicSide::Server ? paths.validated(index) : paths.proven(index));
}

bool QuicConnection::data_path(usize slot) const noexcept
{
    return m_controls[slot] != nullptr && m_protection.path_open(slot) && addressed(slot) &&
           carries_data(slot, tuples(slot).active());
}

bool QuicConnection::fresher_path(usize slot, u32 timeouts) const noexcept
{
    for (usize other = 0U; other < kQuicMaxPaths; ++other)
    {
        if (other != slot && data_path(other) && m_recovery->pto_count(other) < timeouts)
        {
            return true;
        }
    }
    return false;
}

bool QuicConnection::peer_backup(usize slot) const noexcept
{
    return peer_path_status(m_protection.path_id(slot)) == QuicPathStatus::Backup;
}

bool QuicConnection::backup_held(usize slot) const noexcept
{
    if (!peer_backup(slot))
    {
        return false;
    }
    // With every usable path backup, or the others dark, the peer's marks give no guidance (section 3.3).
    for (usize other = 0U; other < kQuicMaxPaths; ++other)
    {
        if (other != slot && data_path(other) && m_recovery->pto_count(other) == 0U && !peer_backup(other))
        {
            return true;
        }
    }
    return false;
}

bool QuicConnection::ranks_before(usize a, usize b) const noexcept
{
    const u64 first = m_recovery->rtt(a).smoothed_ns;
    const u64 second = m_recovery->rtt(b).smoothed_ns;
    return (first != second) ? first < second : a < b;
}

bool QuicConnection::yields_data(usize slot, usize out_bytes, u64 now_ns, u32 held_sockets) const noexcept
{
    if (backup_held(slot))
    {
        return true;
    }
    if (m_config.path_choice != QuicPathChoice::LowestRtt)
    {
        return false;
    }
    // Every term below is one that lets the other slot's data go in the same send() round, so a slot never yields
    // to one that then sends nothing: work goes on, on the next path in rank.
    for (usize other = 0U; other < kQuicMaxPaths; ++other)
    {
        if (other == slot || !data_path(other) || m_recovery->pto_count(other) != 0U || backup_held(other) ||
            !ranks_before(other, slot))
        {
            continue;
        }
        const QuicPathControl& path = *m_controls[other];
        const u64 in_flight = m_recovery->bytes_in_flight(other);
        const bool room = in_flight + datagram_limit(other, out_bytes) <= path.congestion().window();
        if (room && path.release_ns(now_ns, m_recovery->rtt(other)) <= now_ns &&
            m_recovery->can_send_data(QuicSpace::Application, other) &&
            !path_held(other, tuples(other).active(), held_sockets) && amplification_credit(other) >= kMinUsefulPacket)
        {
            return true;
        }
    }
    return false;
}

bool QuicConnection::held_dark(usize slot) const noexcept
{
    // Ranked, not live or not: a path dark for long stays held while the path carrying its data takes a timeout of
    // its own, and the path with the fewest timeouts pending sends as RFC 9002's single path does.
    const u32 timeouts = m_recovery->pto_count(slot);
    return timeouts != 0U && fresher_path(slot, timeouts);
}

usize QuicConnection::amplification_credit(usize slot) const noexcept
{
    const QuicPaths& paths = tuples(slot);
    return paths.credit(paths.active());
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
    const usize index = quic_space_index(space);
    const usize slot = (space == QuicSpace::Application) ? send_slot() : 0U;
    if (m_space_gone[index] || !m_protection.can_send(send_type(space)) || !m_recovery->can_send(space, slot))
    {
        return false;
    }
    // The active path's PATH_RESPONSE, PATH_CHALLENGE and OBSERVED_ADDRESS go as ACKs do, never held by the window or
    // the pacer. On another path an OBSERVED_ADDRESS only rides with the others: packets replayed from spoofed
    // addresses then draw no more datagrams than they did (the draft's section 6.2).
    const QuicPaths& paths = tuples(send_slot());
    const usize active = paths.active();
    const bool path_owed =
        space == QuicSpace::Application && send_type(space) == QuicPacketType::OneRtt &&
        (paths.response_owed(active) || paths.challenge_due(active, now_ns) || observation_due(active));
    if (m_recovery->ack_deadline_ns(space, slot) <= now_ns || m_numbers[number_space(space, slot)].probes > 0U ||
        path_owed || (space == QuicSpace::Application && stopped_ack_due(now_ns)))
    {
        return true;
    }
    return (data_allowed(space) && has_data(space, now_ns)) || (own_allowed(space) && own_ready(now_ns));
}

bool QuicConnection::data_allowed(QuicSpace space) const noexcept
{
    // The handshake's CRYPTO data is not held by the window or the pacer: a peer whose handshake has not completed
    // reads no 1-RTT packet, so 1-RTT data filling the window is neither acknowledged nor declared lost, and no probe
    // timer serves the Application space before confirmation (RFC 9002 appendix A.8). A lost Handshake flight held
    // behind it would wedge both ends. Its size is bounded by the crypto buffers and, on a server, by the
    // amplification limit; it still counts in bytes in flight.
    const usize slot = send_slot();
    return (space != QuicSpace::Application || (m_data_open && carries_data(slot, m_send_path) && !held_dark(slot))) &&
           m_recovery->can_send_data(space, (space == QuicSpace::Application) ? slot : 0U);
}

bool QuicConnection::own_allowed(QuicSpace space) const noexcept
{
    const usize slot = send_slot();
    return space == QuicSpace::Application && m_own_open && carries_data(slot, m_send_path) && !held_dark(slot) &&
           m_recovery->can_send_data(space, slot);
}

bool QuicConnection::own_ready(u64 now_ns) const noexcept
{
    return datagram_ready(now_ns, datagram_paths(true)) ||
           (m_frame_count < m_config.sent_frames && m_streams.has_frame(stream_route(false)));
}

QuicStreamRoute QuicConnection::stream_route(bool any) const noexcept
{
    QuicStreamRoute route;
    route.slot = send_slot();
    route.any = any;
    return route;
}

u32 QuicConnection::copy_paths() const noexcept
{
    u32 copies = 0U;
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        copies |= (data_path(slot) && !held_dark(slot) && !backup_held(slot)) ? (1U << slot) : 0U;
    }
    return copies;
}

bool QuicConnection::has_data(QuicSpace space, u64 now_ns) const noexcept
{
    const usize index = quic_space_index(space);
    if (m_levels[index].out.pending())
    {
        return true;
    }
    if (space != QuicSpace::Application)
    {
        return false;
    }
    bool abandon_owed = false;
    for (const QuicPathAbandon& entry : m_abandons)
    {
        abandon_owed = abandon_owed || (entry.owed && slot_of(entry.path_id) != send_slot());
    }
    return m_handshake_done_owed || m_ping_owed || m_probe_fill || m_cids.retirement_owed() ||
           (m_frame_count < m_config.sent_frames &&
            (m_streams.has_frame(stream_route(true)) || m_cids.local_owed() || m_new_token_owed || m_max_path_id_owed ||
             m_paths_blocked_owed || m_cids_blocked != 0U || abandon_owed || status_owed())) ||
           datagram_ready(now_ns, datagram_paths(false));
}

void QuicConnection::gate_data(usize datagram, usize out_bytes, u64 now_ns, u32 held_sockets) noexcept
{
    const usize slot = send_slot();
    QuicPathControl& path = control(slot);
    path.disarm_pace();
    m_probe_fill = false;
    m_own_open = false;
    // This slot's own probes alone: another path's owed probe opens neither this window nor this pacer.
    const auto owes = [&](QuicSpace space)
    {
        return m_numbers[number_space(space, slot)].probes > 0U;
    };
    const bool probing = owes(QuicSpace::Application) ||
                         (handshake_slot(slot) && (owes(QuicSpace::Initial) || owes(QuicSpace::Handshake)));
    if (probing)
    {
        m_data_open = true; // RFC 9002 section 7.5: probes are not held by the window or the pacer
        return;
    }
    const bool probe_wants =
        send_type(QuicSpace::Application) == QuicPacketType::OneRtt && path.congestion().probe_bytes() != 0U;
    // A whole datagram must fit: a smaller room would let only ACKs and frames too small to matter through, packet
    // after packet. The window's floor of two datagrams always holds one.
    const u64 in_flight = m_recovery->bytes_in_flight(slot);
    const bool room = in_flight + datagram <= path.congestion().window();
    const QuicSpace application = QuicSpace::Application;
    const bool sendable = !m_space_gone[quic_space_index(application)] &&
                          m_protection.can_send(send_type(application)) && m_recovery->can_send_data(application, slot);
    if (yields_data(slot, out_bytes, now_ns, held_sockets))
    {
        // Another path takes the data (S78's scheduling). Copies of redundant datagrams the other path sent and the
        // datagrams and stream data pinned here still go, in this window and pacer, which hold them back as they
        // hold data; with none waiting the window waits unused by the application's choice, so a rate sample here
        // shows the application's rate.
        m_data_open = false;
        const u64 release = path.release_ns(now_ns, m_recovery->rtt(slot));
        m_own_open = room && release <= now_ns;
        if (!sendable)
        {
            return;
        }
        if (!own_ready(now_ns))
        {
            if (room)
            {
                path.on_app_limited(in_flight);
            }
        }
        else if (!m_own_open)
        {
            path.on_limited(!room, now_ns);
            if (room)
            {
                path.arm_pace(release);
            }
        }
        return;
    }
    const u64 release = path.release_ns(now_ns, m_recovery->rtt(slot));
    m_data_open = room && release <= now_ns;
    m_probe_fill = m_data_open && probe_wants; // a probe's fill is data: the window and the pacer hold it as any
    if (m_data_open)
    {
        if (sendable && !has_data(application, now_ns) && !m_streams.has_frame(stream_route(true)))
        {
            // The window and the pacer allow sending and nothing waits: until what is in flight is delivered, a rate
            // sample shows the application's rate, not the path's (draft-ietf-ccwg-bbr-07 section 4.1.2.4).
            path.on_app_limited(in_flight);
        }
        return;
    }
    if (sendable && (probe_wants || has_data(application, now_ns)))
    {
        // Section 7.8: the window is what holds sending back, so it may grow; a window with room waits for the pacer.
        path.on_limited(!room, now_ns);
        if (room)
        {
            path.arm_pace(release);
        }
    }
}

bool QuicConnection::set_congestion(const CongestionPolicy& policy) noexcept
{
    if (!ready() || !congestion_policy_valid(policy))
    {
        return false;
    }
    for (QuicPathControl* const path : m_controls)
    {
        if (path != nullptr)
        {
            static_cast<void>(path->set_congestion(policy, entropy()));
        }
    }
    m_config.congestion = policy; // the policy paths opened from now start on
    return true;
}

bool QuicConnection::set_path_choice(QuicPathChoice choice) noexcept
{
    if (choice > QuicPathChoice::Turns)
    {
        return false;
    }
    m_config.path_choice = choice;
    return true;
}

bool QuicConnection::set_handover(const HandoverSchedule& schedule) noexcept
{
    QuicPathControl* const lead = m_controls[lead_slot()];
    return ready() && lead != nullptr && lead->set_handover(schedule);
}

bool QuicConnection::set_handover_learning(const HandoverLearning& learning) noexcept
{
    QuicPathControl* const lead = m_controls[lead_slot()];
    return ready() && lead != nullptr && lead->set_handover_learning(learning);
}

void QuicConnection::count(const QuicPathSignals& signals) noexcept
{
    m_counters.handover_losses += signals.handover_loss ? 1U : 0U;
    m_counters.handover_locks += signals.handover_lock ? 1U : 0U;
    m_counters.ecn_signals += signals.ecn_signal ? 1U : 0U;
}

QuicDatagramPaths QuicConnection::datagram_paths(bool copies_only) const noexcept
{
    QuicDatagramPaths paths;
    paths.slot = send_slot();
    paths.copies = (m_datagrams.queued() != 0U) ? copy_paths() : 0U; // nothing queued: nothing to ask
    paths.copies_only = copies_only;
    return paths;
}

bool QuicConnection::datagram_ready(u64 now_ns, const QuicDatagramPaths& paths) const noexcept
{
    return m_datagrams.has_frame(now_ns, paths) && (!m_datagrams.settles() || m_frame_count < m_config.sent_frames);
}

usize QuicConnection::write_datagrams(cont::Span<u8> frames, usize used, u64 packet, u64 now_ns,
                                      Written& written) noexcept
{
    const QuicDatagramPaths paths = datagram_paths(!m_data_open);
    while (datagram_ready(now_ns, paths))
    {
        u64 ticket = 0U;
        const usize bytes = m_datagrams.write_next(frames.subspan(used), now_ns, ticket, paths);
        if (bytes == 0U)
        {
            break;
        }
        used += bytes;
        written.eliciting = true;
        if (m_datagrams.settles())
        {
            QuicStreamRecord sent;
            sent.value = ticket;
            static_cast<void>(record(QuicSpace::Application, packet, kRecordDatagram, sent, written)); // room checked
        }
    }
    return used;
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
    sent.path = static_cast<u8>((space == QuicSpace::Application) ? send_slot() : 0U); // as write_packet's slot
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
            if (frame.kind == 0U || frame.packet != packet.number || frame.space != static_cast<u8>(space) ||
                frame.path != packet.path)
            {
                break;
            }
            settle_frame(frame, how);
        }
    }
    free_settled_frames();
}

void QuicConnection::settle_frame(SentFrame& frame, Settle how) noexcept
{
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
        if (frame.frame.value == m_new_token_generation) // a replaced token's outcome settles nothing
        {
            m_new_token_acked = m_new_token_acked || how == Settle::Acked;
            m_new_token_owed = !m_new_token_acked && (m_new_token_owed || how == Settle::Lost);
        }
    }
    else if (frame.kind == kRecordMaxPathId)
    {
        // Sent again when lost unless a larger limit went since (RFC 10069 section 4.6).
        m_max_path_id_owed = m_max_path_id_owed || (how == Settle::Lost && frame.frame.value == m_local_max_path_id);
    }
    else if (frame.kind == kRecordPathStatus)
    {
        // Sent again when lost only while it carries the newest status for its path ID (RFC 10069 section 4.3).
        for (PathStatusEntry& entry : m_local_statuses)
        {
            const bool newest = entry.status != QuicPathStatus::Unknown && entry.path_id == frame.frame.stream_id &&
                                entry.sequence == frame.frame.value;
            entry.owed = entry.owed || (how == Settle::Lost && newest);
        }
    }
    else if (frame.kind == kRecordPathsBlocked)
    {
        // Sent again only while the server's limit stands (RFC 10069 section 4.7, as RFC 9000 section 13.3).
        m_paths_blocked_owed = m_paths_blocked_owed || (how == Settle::Lost && frame.frame.value == m_peer_max_path_id);
    }
    else if (frame.kind == kRecordPathCidsBlocked)
    {
        const u64 path_id = frame.frame.stream_id;
        const bool still = m_cids.is_open(path_id) && !m_cids.abandoned(path_id) &&
                           m_cids.current(path_id) == kNoSequence && m_cids.peer_next(path_id) == frame.frame.value;
        m_cids_blocked = (how == Settle::Lost && m_cids_blocked == 0U && still) ? path_id : m_cids_blocked;
    }
    else if (frame.kind == kRecordPathAbandon)
    {
        // Sent again when lost while its path ID is still being abandoned (section 4.2); acknowledged, it
        // starts the bound on its retention (section 3.4).
        QuicPathAbandon* const entry = find_abandon(frame.frame.stream_id);
        if (entry != nullptr && !entry->acked)
        {
            entry->owed = how != Settle::Acked;
            entry->acked = how == Settle::Acked;
            entry->deadline_ns = entry->acked ? retention_end_ns(entry->path_id, m_event_ns) : kNeverNs;
        }
    }
    else if (frame.kind == kRecordNewId)
    {
        if (how != Settle::Discarded)
        {
            m_cids.local_settled(frame.frame.stream_id, frame.frame.value, how == Settle::Acked);
        }
    }
    else if (frame.kind == kRecordObserved)
    {
        // Resent only on its own path (draft-ietf-quic-address-discovery-01 section 4.1).
        if (how == Settle::Lost)
        {
            tuples(frame.path).observation_lost(frame.frame.length, frame.frame.value);
        }
    }
    else
    {
        m_cids.retirement_settled(frame.frame.stream_id, frame.frame.value, how != Settle::Lost);
    }
    frame.kind = 0U;
}

void QuicConnection::free_settled_frames() noexcept
{
    while (m_frame_count > 0U && m_frames[m_frame_head].kind == 0U)
    {
        m_frame_head = (m_frame_head + 1U) % m_config.sent_frames;
        --m_frame_count;
    }
}

void QuicConnection::release_frames(usize slot) noexcept
{
    // Every record of the slot's packets in flight is settled as lost, its packets left in flight: a later
    // acknowledgement or loss of one finds no record (settle() stops at a settled or reused one) and moves only
    // recovery and the slot's controller, so nothing is counted twice and the ledger's head is never held.
    const auto application = static_cast<u8>(QuicSpace::Application);
    for (u32 at = m_frame_head, walked = 0U; walked < m_frame_count; at = (at + 1U) % m_config.sent_frames, ++walked)
    {
        SentFrame& frame = m_frames[at];
        if (frame.kind != 0U && frame.space == application && frame.path == slot)
        {
            settle_frame(frame, Settle::Lost);
            ++m_counters.pto_resends;
        }
    }
    free_settled_frames();
}

usize QuicConnection::write_frames(QuicSpace space, bool early, cont::Span<u8> frames, u64 packet, u64 now_ns,
                                   Written& written) noexcept
{
    const usize index = quic_space_index(space);
    const usize slot = (space == QuicSpace::Application) ? send_slot() : 0U;
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
        // Each slot acknowledges its own space, path ID 0's in an ACK frame and the others' in PATH_ACK frames
        // (RFC 10069 section 2.3), so a carried acknowledgement is always settled in the slot that sent it.
        const auto ack_form = [this](usize of)
        {
            const u64 path_id = m_protection.path_id(of); // a reclaimed slot 0's later path ID takes PATH_ACK
            return (path_id == 0U) ? kQuicAckFrame : path_id;
        };
        if (!m_recovery->received(space, slot).ranges().empty())
        {
            const QuicAckWrite ack = m_recovery->write_ack(space, frames.subspan(used), now_ns, slot, ack_form(slot));
            used += ack.bytes;
            written.carried_ack = (ack.bytes != 0U) ? ack.largest : kQuicNoPacket;
        }
        // An abandoned path's stragglers, path ID 0's too, are acknowledged at once on this one (section 3.4.3).
        for (usize other = 0U; space == QuicSpace::Application && other < kQuicMaxPaths; ++other)
        {
            if (other != slot && stopped(other) && m_recovery->ack_deadline_ns(space, other) != kNeverNs)
            {
                used += m_recovery->write_ack(space, frames.subspan(used), now_ns, other, ack_form(other)).bytes;
            }
        }
    };
    // A 0-RTT packet carries no ACK, CRYPTO, HANDSHAKE_DONE, NEW_TOKEN or PATH_RESPONSE frame (RFC 9000 section
    // 12.4, table 3), and no RETIRE_CONNECTION_ID either: nothing is retired before the server's IDs arrive.
    const bool ack_later = space == QuicSpace::Application && m_recovery->ack_deadline_ns(space, slot) > now_ns &&
                           datagram_ready(now_ns, datagram_paths(!m_data_open));
    if (!ack_later && !early)
    {
        put_ack();
    }
    // A probe owes an ack-eliciting packet, and so does a run of ACK-only packets: those are never acknowledged
    // by themselves, so a PING lets the peer's ACK free their storage (RFC 9000 section 13.2.4).
    const auto put_owed_ping = [&]
    {
        const Numbering& numbers = m_numbers[number_space(space, slot)];
        if (!written.eliciting && (numbers.probes > 0U || numbers.ack_only >= kAckOnlyRun))
        {
            QuicFrame ping;
            ping.kind = QuicFrameKind::Ping;
            static_cast<void>(put(ping));
        }
    };
    // The active path's PATH_RESPONSE and PATH_CHALLENGE, never held for data: a peer validating its path waits.
    if (space == QuicSpace::Application && !early)
    {
        used += write_path_frames(frames.subspan(used), packet, now_ns, written);
    }
    if (!data_allowed(space))
    {
        if (own_allowed(space))
        {
            used = write_datagrams(frames, used, packet, now_ns, written);
            used = write_streams(frames, used, packet, now_ns, written);
        }
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
                QuicStreamRecord generation;
                generation.value = m_new_token_generation;
                static_cast<void>(record(space, packet, kRecordNewToken, generation, written));
                m_new_token_owed = false;
            }
        }
        if (m_max_path_id_owed && m_frame_count < m_config.sent_frames)
        {
            QuicFrame raised;
            raised.kind = QuicFrameKind::MaxPathId;
            raised.value = m_local_max_path_id;
            if (put(raised))
            {
                QuicStreamRecord limit;
                limit.value = raised.value;
                static_cast<void>(record(space, packet, kRecordMaxPathId, limit, written));
                m_max_path_id_owed = false;
            }
        }
        for (PathStatusEntry& entry : m_local_statuses)
        {
            // An abandoned path ID's status is read by no one (section 4); it is forgotten as the path ID closes.
            if (!entry.owed || m_cids.abandoned(entry.path_id) || m_frame_count == m_config.sent_frames)
            {
                continue;
            }
            QuicFrame status;
            status.kind = QuicFrameKind::PathStatus;
            status.path_id = entry.path_id;
            status.sequence = entry.sequence;
            status.backup = entry.status == QuicPathStatus::Backup;
            if (!put(status))
            {
                break;
            }
            QuicStreamRecord sent;
            sent.stream_id = entry.path_id;
            sent.value = entry.sequence;
            static_cast<void>(record(space, packet, kRecordPathStatus, sent, written));
            entry.owed = false;
        }
        if (m_paths_blocked_owed && m_frame_count < m_config.sent_frames)
        {
            QuicFrame blocked;
            blocked.kind = QuicFrameKind::PathsBlocked;
            blocked.value = m_peer_max_path_id;
            if (put(blocked))
            {
                QuicStreamRecord limit;
                limit.value = blocked.value;
                static_cast<void>(record(space, packet, kRecordPathsBlocked, limit, written));
                m_paths_blocked_owed = false;
            }
        }
        for (QuicPathAbandon& entry : m_abandons)
        {
            // Never on the path it abandons, which sends nothing more (section 3.4).
            if (!entry.owed || m_frame_count == m_config.sent_frames || slot_of(entry.path_id) == slot)
            {
                continue;
            }
            QuicFrame abandon;
            abandon.kind = QuicFrameKind::PathAbandon;
            abandon.path_id = entry.path_id;
            abandon.error_code = entry.error_code;
            if (!put(abandon))
            {
                break;
            }
            QuicStreamRecord sent;
            sent.stream_id = entry.path_id;
            static_cast<void>(record(space, packet, kRecordPathAbandon, sent, written));
            entry.owed = false;
        }
        if (m_cids_blocked != 0U && m_frame_count < m_config.sent_frames)
        {
            QuicFrame blocked;
            blocked.kind = QuicFrameKind::PathCidsBlocked;
            blocked.path_id = m_cids_blocked;
            blocked.sequence = m_cids.peer_next(m_cids_blocked);
            if (put(blocked))
            {
                QuicStreamRecord next;
                next.stream_id = blocked.path_id;
                next.value = blocked.sequence;
                static_cast<void>(record(space, packet, kRecordPathCidsBlocked, next, written));
                m_cids_blocked = 0U;
            }
        }
        // Path ID 0's IDs go in NEW_CONNECTION_ID and RETIRE_CONNECTION_ID frames without multipath, every one in
        // their PATH_ forms with it (RFC 10069 section 3.2); a record names the frame's path ID and sequence.
        const bool path_forms = m_multipath;
        u64 path_id = 0U;
        u64 sequence = 0U;
        while (m_frame_count < m_config.sent_frames && m_cids.next_retirement(path_id, sequence))
        {
            QuicFrame retire;
            retire.kind = (path_id == 0U && !path_forms) ? QuicFrameKind::RetireConnectionId
                                                         : QuicFrameKind::PathRetireConnectionId;
            retire.path_id = path_id;
            retire.sequence = sequence;
            if (!put(retire))
            {
                break;
            }
            QuicStreamRecord retired;
            retired.stream_id = path_id;
            retired.value = sequence;
            static_cast<void>(record(space, packet, kRecordRetire, retired, written));
            m_cids.retirement_sent(path_id, sequence);
        }
        const QuicLocalCid* owed = nullptr;
        while (m_frame_count < m_config.sent_frames && (owed = m_cids.next_owed(path_id)) != nullptr)
        {
            // Retire Prior To never passes the frame's own sequence (section 19.15): a rotated ID carries its own.
            QuicFrame issued;
            issued.kind =
                (path_id == 0U && !path_forms) ? QuicFrameKind::NewConnectionId : QuicFrameKind::PathNewConnectionId;
            issued.path_id = path_id;
            issued.sequence = owed->sequence;
            issued.retire_prior_to = min_u64(m_cids.local_retire_prior_to(path_id), owed->sequence);
            issued.data = owed->id.view();
            issued.reset_token = cont::ConstSpan<u8>(owed->reset_token, kQuicResetTokenBytes);
            if (!put(issued))
            {
                break;
            }
            QuicStreamRecord sent;
            sent.stream_id = path_id;
            sent.value = owed->sequence;
            static_cast<void>(record(space, packet, kRecordNewId, sent, written));
            m_cids.local_sent(path_id, owed->sequence);
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
        used = write_datagrams(frames, used, packet, now_ns, written);
        if (ack_later && !early)
        {
            put_ack();
        }
        used = write_streams(frames, used, packet, now_ns, written);
    }
    if (space == QuicSpace::Application && (m_ping_owed || m_probe_fill))
    {
        m_ping_owed = false;
        if (!written.eliciting)
        {
            QuicFrame ping;
            ping.kind = QuicFrameKind::Ping;
            static_cast<void>(put(ping));
        }
        // A bandwidth probe's packet is ack-eliciting and fills its datagram: its bytes, not its frames, are the probe.
        written.fill = m_probe_fill && written.eliciting;
    }
    put_owed_ping();
    return used;
}

usize QuicConnection::write_streams(cont::Span<u8> frames, usize used, u64 packet, u64 now_ns,
                                    Written& written) noexcept
{
    // The stream set's frames after the connection's own; each fits the room it was offered. A slot yielding its
    // data writes only the stream data pinned to it (S78).
    const QuicStreamRoute route = stream_route(m_data_open);
    const u64 rtt_ns = m_recovery->rtt(route.slot).smoothed_ns;
    QuicFrame frame;
    QuicStreamRecord taken;
    while (m_frame_count < m_config.sent_frames &&
           m_streams.next_frame(frames.size() - used, now_ns, rtt_ns, frame, taken, route))
    {
        const usize bytes = write_quic_frame(frame, frames.subspan(used));
        if (bytes == 0U)
        {
            m_streams.on_lost(taken); // owed again (a frame next_frame sized never fails to fit)
            break;
        }
        used += bytes;
        written.eliciting = written.eliciting || quic_frame_ack_eliciting(frame.kind);
        static_cast<void>(record(QuicSpace::Application, packet, kRecordStream, taken, written));
    }
    return used;
}

usize QuicConnection::write_path_frames(cont::Span<u8> frames, u64 packet, u64 now_ns, Written& written) noexcept
{
    QuicPaths& paths = tuples(send_slot());
    const usize path = m_send_path;
    usize used = 0U;
    if (paths.response_owed(path))
    {
        QuicFrame response;
        response.kind = QuicFrameKind::PathResponse;
        response.data = paths.response(path);
        const usize bytes = write_quic_frame(response, frames);
        if (bytes != 0U)
        {
            used += bytes;
            paths.response_sent(path);
            written.path_response = true;
            written.eliciting = true;
        }
    }
    if (paths.challenge_due(path, now_ns))
    {
        const u64 draw = entropy(); // from the connection's PRF: unpredictable off the path (section 8.2.1)
        for (usize index = 0U; index < kQuicPathDataBytes; ++index)
        {
            m_challenge[index] = static_cast<u8>(draw >> (8U * index));
        }
        QuicFrame challenge;
        challenge.kind = QuicFrameKind::PathChallenge;
        challenge.data = cont::ConstSpan<u8>(m_challenge, kQuicPathDataBytes);
        const usize bytes = write_quic_frame(challenge, frames.subspan(used));
        if (bytes != 0U)
        {
            used += bytes;
            written.path_challenge = true;
            written.eliciting = true;
        }
    }
    if (observation_due(path))
    {
        // The address the peer's datagrams come from on this path, an IPv4-mapped one as IPv4, without its scope.
        const Endpoint& peer = paths.path(path).peer;
        const IpAddress address = peer.address.unmapped();
        QuicFrame observed;
        observed.kind = QuicFrameKind::ObservedAddress;
        observed.sequence = m_observation_next;
        observed.data = address.view();
        observed.value = peer.port;
        const usize bytes = write_quic_frame(observed, frames.subspan(used));
        if (bytes != 0U)
        {
            used += bytes;
            written.eliciting = true;
            QuicStreamRecord sent;
            sent.value = m_observation_next;
            sent.length = static_cast<u32>(path);
            static_cast<void>(record(QuicSpace::Application, packet, kRecordObserved, sent, written)); // room checked
            paths.observation_sent(path, m_observation_next);
            ++m_observation_next;
        }
    }
    return used;
}

bool QuicConnection::observation_due(usize index) const noexcept
{
    return m_peer_known && quic_provides_addresses(m_local.address_discovery) &&
           quic_wants_addresses(m_peer.address_discovery) && tuples(send_slot()).observation_owed(index) &&
           tuples(send_slot()).path(index).peer.port != 0U && m_frame_count < m_config.sent_frames;
}

bool QuicConnection::observed_address_frame(const QuicFrame& frame, u64 now_ns) noexcept
{
    // Only an end that asked for reports takes them (draft-ietf-quic-address-discovery-01 section 4.1).
    if (!quic_wants_addresses(m_local.address_discovery))
    {
        fail(QuicTransportError::ProtocolViolation, 0U, now_ns);
        return false;
    }
    Endpoint observed;
    observed.address.family = (frame.data.size() == 4U) ? AddressFamily::V4 : AddressFamily::V6;
    std::memcpy(observed.address.bytes, frame.data.data(), frame.data.size());
    observed.port = static_cast<u16>(frame.value);
    if (tuples(m_receive_slot).on_observed(m_receive_path, frame.sequence, observed))
    {
        ++m_counters.address_observations;
    }
    return true;
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
                                   Carry carry, u64 now_ns) noexcept
{
    const bool closing = carry == Carry::Close;
    const usize slot = (space == QuicSpace::Application) ? send_slot() : 0U;
    Numbering& numbers = m_numbers[number_space(space, slot)];
    u64 number = numbers.next;
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
    // An ID of the slot's own path ID (RFC 10069 section 3.1): the one its 4-tuple sends to.
    const u64 path_id = m_protection.path_id(slot);
    const bool off_path = m_send_path != tuples(slot).active();
    const u64 sequence = off_path ? tuples(slot).peer_sequence(m_send_path) : m_cids.current(path_id);
    const QuicPeerCid* peer = m_cids.peer(path_id, sequence);
    spec.dcid = (off_path && peer != nullptr) ? peer->id.view() : peer_cid(slot);
    m_cids.use(path_id, sequence); // its token is checked from now on (section 10.3.1)
    spec.scid = m_local_cid.view();
    if (space == QuicSpace::Initial && m_config.side == crypto::QuicSide::Client)
    {
        spec.token = cont::ConstSpan<u8>(m_token, m_token_bytes);
    }
    spec.number = number;
    const u64 largest = m_recovery->largest_acked(space, slot);
    spec.acknowledged = largest != kQuicNoPacket;
    spec.largest_acked = spec.acknowledged ? largest : 0U;
    spec.clear_quic_bit = quic_grease_bit_clear(m_peer_greases, entropy());
    QuicPacketWriter writer;
    if (!writer.begin(spec, room))
    {
        return 0U;
    }
    Written written;
    usize used = 0U;
    if (carry == Carry::Probe)
    {
        QuicFrame ping;
        ping.kind = QuicFrameKind::Ping;
        used = write_quic_frame(ping, writer.frames());
        written.eliciting = true;
    }
    else if (carry == Carry::Path)
    {
        used = write_path_frames(writer.frames(), number, now_ns, written);
    }
    else
    {
        used = closing ? write_close(space, writer.frames())
                       : write_frames(space, early, writer.frames(), number, now_ns, written);
    }
    // A later packet that cannot fit (a 0-RTT packet behind a full Initial) leaves the padding to this one; a path
    // frame's datagram reaches 1200 bytes as far as the amplification limit lets it (section 8.2).
    const bool padded = (pad && (last || writer.frames().size() - used < kMinUsefulPacket)) || written.path_response ||
                        written.path_challenge;
    if (used == 0U && !padded)
    {
        return 0U;
    }
    usize floor = padded ? min_u64(quic_datagram_floor(true, datagram_used), room.size()) : 0U;
    floor = (carry == Carry::Probe || written.fill) ? room.size() : floor; // a probe is exactly the size it probes
    // Every datagram reaches kQuicResetPadding past this side's ID length, so a peer's stateless reset answering it
    // can be as long as a packet to this side (section 10.3).
    const usize reset_floor = m_local_cid.bytes + kQuicResetPadding;
    const usize least = (datagram_used < reset_floor) ? min_u64(reset_floor - datagram_used, room.size()) : 0U;
    usize bytes = 0U;
    const u32 block = (carry == Carry::Probe) ? 0U : m_config.padding_block;
    // In the slot's number space, under its path ID's nonce (RFC 10069 section 2.4).
    const QuicPadding padding{max_u64(floor, least), block};
    const QuicPacketError sealed = m_protection.seal(writer, used, padding, bytes, slot);
    if (sealed != QuicPacketError::None)
    {
        const QuicTransportError error = quic_packet_close(sealed);
        fail((error == QuicTransportError::NoError) ? QuicTransportError::InternalError : error, 0U, now_ns);
        return 0U;
    }
    numbers.skipped = skip ? number - 1U : numbers.skipped;
    numbers.next = number + 1U;
    if (written.path_challenge)
    {
        tuples(slot).challenge_sent(m_send_path, cont::ConstSpan<u8>(m_challenge, kQuicPathDataBytes),
                                    datagram_used + bytes >= kQuicMinInitialDatagram, now_ns,
                                    m_recovery->probe_period_ns(QuicSpace::Application, slot));
    }
    if (!closing)
    {
        QuicPathControl& path = control(slot);
        QuicSentPacket sent;
        sent.number = number;
        sent.path = static_cast<u8>(slot);
        sent.sent_ns = now_ns;
        sent.carried_ack = written.carried_ack;
        sent.token = written.token;
        sent.bytes = static_cast<u32>(bytes);
        sent.ack_eliciting = written.eliciting;
        sent.in_flight = written.eliciting || floor != 0U;
        sent.path_probe = carry == Carry::Probe;
        sent.off_path = off_path;
        sent.handover = path.covered(now_ns);
        sent.ecn = m_marking;
        // Only a datagram that starts with this short header is one an ICMP message can quote and match.
        sent.tag = (spec.type == QuicPacketType::OneRtt && datagram_used == 0U && !off_path)
                       ? packet_tag(room.first(bytes), spec.dcid.size())
                       : 0U;
        const bool controlled = sent.in_flight && !off_path;
        if (controlled)
        {
            sent.delivery = path.stamp(bytes, m_recovery->bytes_in_flight(slot) + bytes, now_ns);
        }
        static_cast<void>(m_recovery->on_sent(space, sent)); // can_send held and numbers rise
        path.on_sent(space, sent, controlled, m_recovery->bytes_in_flight(slot), m_recovery->rtt(slot), now_ns);
        m_counters.probe_fills += written.fill ? 1U : 0U;
        if (written.eliciting && numbers.probes > 0U && carry != Carry::Probe && !off_path)
        {
            --numbers.probes;
        }
        numbers.ack_only = written.eliciting ? 0U : numbers.ack_only + 1U;
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
    const usize highest = quic_space_index(space_of(m_close_level));
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
        used += write_packet(space, out.subspan(used), used, pad, !later, Carry::Close, now_ns);
    }
    return used;
}

usize QuicConnection::send(cont::Span<u8> out, u64 now_ns, u32 held_sockets) noexcept
{
    m_held_back = false;
    if (out.size() < kQuicMinInitialDatagram || m_state == QuicConnectionState::Idle ||
        m_state == QuicConnectionState::Draining || m_state == QuicConnectionState::Closed)
    {
        m_send_path = tuples(send_slot()).active();
        return 0U;
    }
    // The close goes on the leading slot alone. Otherwise the open slots take turns, starting after the one that
    // wrote the last datagram, so one slot's data never starves another's owed frames (S78's scheduling).
    if (m_state == QuicConnectionState::Closing)
    {
        m_send_slot = lead_slot();
        return send_on_slot(out, now_ns, held_sockets);
    }
    pull_crypto();
    issue_local_ids();
    // Each open slot once, counted from the last writer; the writer of this call moves only m_send_slot.
    const usize last = m_send_slot;
    for (usize turn = 1U; turn <= kQuicMaxPaths; ++turn)
    {
        const usize slot = (last + turn) % kQuicMaxPaths;
        if (!m_protection.path_open(slot) || !addressed(slot) || m_controls[slot] == nullptr)
        {
            continue;
        }
        m_send_slot = slot;
        const usize used = send_on_slot(out, now_ns, held_sockets);
        if (used != 0U || m_state != QuicConnectionState::Established)
        {
            return used; // a close a seal failure began goes on the next call, on the leading slot
        }
    }
    return 0U;
}

usize QuicConnection::send_on_slot(cont::Span<u8> out, u64 now_ns, u32 held_sockets) noexcept
{
    const usize slot = send_slot();
    m_send_path = tuples(slot).active();
    const bool validated = tuples(slot).validated(m_send_path);
    usize limit = datagram_limit(slot, out.size());
    const usize full = limit; // a datagram's size before the amplification limit: what the window must have room for
    const usize credit = amplification_credit(slot);
    limit = min_u64(limit, credit);
    usize used = 0U;
    bool initial_held = false; // the amplification limit holds back a padded Initial
    if (m_state == QuicConnectionState::Closing)
    {
        if (path_held(slot, m_send_path, held_sockets))
        {
            m_held_back = true;
            return 0U;
        }
        m_marking = Ecn::NotEct; // nothing of a closing connection is acknowledged
        used = (limit >= kMinUsefulPacket) ? send_closing(out.first(limit), now_ns) : 0U;
    }
    else
    {
        used = send_path_frames(out, now_ns, held_sockets);
        if (used != 0U)
        {
            return used;
        }
        if (path_held(slot, m_send_path, held_sockets))
        {
            m_held_back = true; // nothing below runs: no number, window, probe or amplification state moves
            return 0U;
        }
        m_marking = control(slot).marking(now_ns, m_recovery->persistent_congestion_ns(slot));
        gate_data(full, out.size(), now_ns, held_sockets);
        used = send_path_probe(out, now_ns);
        if (used != 0U)
        {
            ++m_counters.datagrams_sent;
            m_counters.bytes_sent += used;
            ++m_counters.path_probes;
            tuples(slot).on_sent(m_send_path, used);
            return used;
        }
        bool content[kQuicSpaces] = {};
        QuicSpace last = QuicSpace::Initial;
        bool any = false;
        for (usize index = 0U; index < kQuicSpaces; ++index)
        {
            // Only the handshake slot carries the Initial and Handshake spaces.
            const auto space = static_cast<QuicSpace>(index);
            content[index] = (handshake_slot(slot) || space == QuicSpace::Application) && has_content(space, now_ns);
            if (content[index])
            {
                last = static_cast<QuicSpace>(index);
                any = true;
            }
        }
        const usize initial = quic_space_index(QuicSpace::Initial);
        // A client pads every datagram with an Initial, a server those with an ack-eliciting one (section 14.1).
        bool pad = content[initial] && (m_config.side == crypto::QuicSide::Client || m_levels[initial].out.pending() ||
                                        m_numbers[number_space(QuicSpace::Initial, 0U)].probes > 0U);
        // A padded Initial the amplification limit cannot carry waits for the peer's next datagram (section 8.1); what
        // a later space owes (an ACK of 0-RTT, Handshake data) still goes, unpadded, within the credit.
        if (pad && limit < kQuicMinInitialDatagram && last != QuicSpace::Initial)
        {
            content[initial] = false;
            pad = false;
            initial_held = true;
        }
        const cont::Span<u8> room = out.first(limit);
        if (!any || limit < kMinUsefulPacket || (pad && limit < kQuicMinInitialDatagram))
        {
            if (any && handshake_slot(slot) && !validated)
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
                used += write_packet(space, room.subspan(used), used, pad, space == last, Carry::Frames, now_ns);
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
        tuples(slot).on_sent(m_send_path, used);
        if (handshake_slot(slot) && !validated)
        {
            // The handshake's amplification deadlock guard (RFC 9002 section 6.2.2.1) is the handshake slot's.
            m_recovery->set_amplification_blocked(initial_held || amplification_credit() < kMinUsefulPacket, now_ns);
        }
    }
    return used;
}

usize QuicConnection::datagram_limit(usize slot, usize out_bytes) const noexcept
{
    const QuicPathControl& path = *m_controls[slot];
    usize limit = min_u64(out_bytes, path.datagram_bytes());
    if (m_peer_known || m_early_sending)
    {
        limit = min_u64(limit, (m_peer_known ? m_peer : m_remembered).max_udp_payload_size);
    }
    const QuicPaths& paths = tuples(slot);
    const usize active = paths.active();
    if (m_recovery->pto_count(slot) != 0U || (!paths.validated(active) && !(path.path() == paths.path(active))))
    {
        // A probe timeout is pending: datagrams of the floor get an acknowledgement through a black hole that drops
        // larger ones (quic_pmtu.hpp). Or the peer moved to a path not yet confirmed: the floor is all it is known
        // to carry.
        limit = min_u64(limit, kQuicPmtuFloor);
    }
    return limit;
}

usize QuicConnection::send_path_frames(cont::Span<u8> out, u64 now_ns, u32 held_sockets) noexcept
{
    const usize slot = send_slot();
    QuicPaths& paths = tuples(slot);
    const usize active = paths.active();
    for (usize index = 0U; index < kQuicMaxPaths; ++index)
    {
        const bool owed =
            index != active && paths.used(index) && (paths.response_owed(index) || paths.challenge_due(index, now_ns));
        if (!owed || !path_sendable(slot, index))
        {
            continue;
        }
        if (path_held(slot, index, held_sockets))
        {
            m_held_back = true;
            continue;
        }
        // A probing datagram of 1200 bytes, the floor every path carries, unmarked: ECN is validated per path.
        m_send_path = index;
        const Ecn marking = m_marking;
        m_marking = Ecn::NotEct;
        const usize room = min_u64(min_u64(out.size(), kQuicMinInitialDatagram), paths.credit(index));
        const usize bytes = write_packet(QuicSpace::Application, out.first(room), 0U, true, true, Carry::Path, now_ns);
        if (bytes != 0U)
        {
            ++m_counters.datagrams_sent;
            m_counters.bytes_sent += bytes;
            paths.on_sent(index, bytes);
            return bytes;
        }
        m_marking = marking;
        m_send_path = active;
    }
    return 0U;
}

usize QuicConnection::send_path_probe(cont::Span<u8> out, u64 now_ns) noexcept
{
    const QuicSpace application = QuicSpace::Application;
    const usize slot = send_slot();
    QuicPathControl& path = control(slot);
    // A further slot's path is searched once its 4-tuple validates (RFC 10069 section 3.1).
    const QuicPaths& paths = tuples(slot);
    if (m_state != QuicConnectionState::Established || !m_confirmed || !m_peer_known ||
        m_recovery->pto_count(slot) != 0U || m_space_gone[quic_space_index(application)] ||
        !m_protection.can_send(QuicPacketType::OneRtt) || !m_recovery->can_send_data(application, slot) ||
        !carries_data(slot, paths.active()))
    {
        return 0U;
    }
    for (usize index = 0U; index < kQuicSpaces; ++index)
    {
        const QuicSpace space = static_cast<QuicSpace>(index);
        const usize owner = (space == application) ? slot : 0U;
        if (can_acknowledge(space, owner) && m_recovery->ack_deadline_ns(space, owner) != kNeverNs)
        {
            return 0U; // an ACK the probe would not carry goes first; the probe takes the next datagram
        }
    }
    // A probe a scheduled handover took would read as a size the path cannot carry: it waits for the handover to pass.
    if (path.covered(now_ns))
    {
        return 0U;
    }
    const usize probe = path.probe(now_ns, has_data(application, now_ns));
    if (probe == 0U || probe > out.size() || probe > amplification_credit(slot))
    {
        return 0U;
    }
    // RFC 8899 section 3: probes are congestion controlled, though their loss is no signal.
    if (m_recovery->bytes_in_flight(slot) + probe > path.congestion().window())
    {
        return 0U;
    }
    const u64 release = path.release_ns(now_ns, m_recovery->rtt(slot));
    if (release > now_ns)
    {
        path.arm_pace(release);
        return 0U;
    }
    const usize bytes = write_packet(application, out.first(probe), 0U, false, true, Carry::Probe, now_ns);
    if (bytes != 0U)
    {
        path.on_probe_sent(static_cast<u32>(bytes));
    }
    return bytes;
}

void QuicConnection::follow_datagram_size() noexcept
{
    u32 bytes = ~u32{0};
    for (const QuicPathControl* const path : m_controls)
    {
        if (path != nullptr && path->datagram_bytes() < bytes)
        {
            bytes = path->datagram_bytes();
        }
    }
    if (bytes == m_datagram_bytes)
    {
        return;
    }
    m_datagram_bytes = bytes;
    m_datagrams.set_path(m_peer_known ? min_u64(bytes, m_peer.max_udp_payload_size) : bytes);
}

// ---- Timers and close ----

u64 QuicConnection::three_ptos() const noexcept
{
    return three_times(m_recovery->largest_probe_period_ns());
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
    next = min_u64(next, path_timer_ns());
    for (const QuicPathControl* const path : m_controls)
    {
        if (path != nullptr)
        {
            next = min_u64(next, path->pace_timer_ns());
        }
    }
    for (usize index = 0U; index < kQuicSpaces; ++index)
    {
        const QuicSpace space = static_cast<QuicSpace>(index);
        // The Application space's owed ACKs are each open slot's.
        const usize slots = (space == QuicSpace::Application) ? kQuicMaxPaths : 1U;
        for (usize slot = 0U; slot < slots; ++slot)
        {
            if (m_protection.path_open(slot) && can_acknowledge(space, slot))
            {
                next = min_u64(next, m_recovery->ack_deadline_ns(space, slot));
            }
        }
    }
    // A stopped slot's acknowledgement rides the leading slot's packets (or any other open one's), due when that one
    // can carry it; its state closes at its retention's end.
    for (usize slot = 0U; slot < kQuicMaxPaths && can_acknowledge(QuicSpace::Application, lead_slot()); ++slot)
    {
        if (stopped(slot))
        {
            next = min_u64(next, m_recovery->ack_deadline_ns(QuicSpace::Application, slot));
        }
    }
    for (const QuicPathAbandon& entry : m_abandons)
    {
        next = entry.used ? min_u64(next, entry.deadline_ns) : next;
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
    for (QuicPathControl* const path : m_controls)
    {
        if (path != nullptr && now_ns >= path->pace_timer_ns())
        {
            path->disarm_pace(); // send() writes what the pacer held, or arms it again
        }
    }
    expire_paths(now_ns);
    if (m_state == QuicConnectionState::Closed)
    {
        return;
    }
    close_abandoned(now_ns);
    if (now_ns < m_recovery->loss_timer_ns())
    {
        return;
    }
    m_event_ns = now_ns;
    const QuicTimeout timeout = m_recovery->on_timeout(now_ns, m_events);
    control(timeout.path).end_detection(timeout.lost, now_ns);
    follow_datagram_size();
    if (timeout.kind != QuicTimeoutKind::Probe && timeout.kind != QuicTimeoutKind::AntiDeadlock)
    {
        return;
    }
    const usize index = quic_space_index(timeout.space);
    m_numbers[number_space(timeout.space, timeout.path)].probes =
        (timeout.kind == QuicTimeoutKind::Probe) ? timeout.probes : u8{1};
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
    // RFC 10069 section 5.7: no packet of the path is declared lost before an acknowledgement on it or its end, so
    // what it holds goes again on the live paths, within their windows, while its probes carry none (held_dark()).
    if (timeout.kind == QuicTimeoutKind::Probe && timeout.space == QuicSpace::Application &&
        fresher_path(timeout.path, 1U))
    {
        release_frames(timeout.path);
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

// ---- Paths ----

bool QuicConnection::follows_peer() const noexcept
{
    return m_config.side == crypto::QuicSide::Server && m_state == QuicConnectionState::Established && m_confirmed;
}

void QuicConnection::admit_path(usize slot, usize datagram_bytes, u64 now_ns) noexcept
{
    QuicPaths& paths = tuples(slot);
    const usize index = take_path(slot, m_candidate, now_ns);
    m_receive_path = index;
    paths.on_received(index, datagram_bytes, now_ns);
    const usize active = paths.active();
    const u64 local = m_packet_local.found() ? m_packet_local.sequence : paths.local_sequence(active);
    paths.set_local_sequence(index, local);
    // A peer that kept its connection ID did not choose to move (a NAT rebinding): this side may keep its own. One
    // that changed it gets an ID this side has not used, never one another path sends to (section 9.5).
    const bool kept = local == paths.local_sequence(active);
    paths.set_peer_sequence(index, kept ? path_sequence(slot, active) : kNoSequence);
    address_paths(slot, now_ns);
}

void QuicConnection::address_paths(usize slot, u64 now_ns) noexcept
{
    QuicPaths& paths = tuples(slot);
    for (usize index = 0U; index < kQuicMaxPaths; ++index)
    {
        if (!paths.used(index) || index == paths.active() || peer_held(slot, paths.peer_sequence(index)))
        {
            continue;
        }
        paths.set_peer_sequence(index, unused_peer_sequence(slot, index));
        if (peer_held(slot, paths.peer_sequence(index)))
        {
            continue; // the peer's next non-probing packet there moves the connection
        }
        // None is unused: the paths left behind on one ID give it up (forgotten, the ID retired), so the peer issues
        // the one this path waits for (section 9.5). The active path's ID stays, the fallback goes only once the
        // active path is proven (no revert is left to need it), and a path under validation stays.
        const bool fallback_spare = paths.proven(paths.active());
        const auto spare = [&](usize other)
        {
            return other != index && other != paths.active() && (other != paths.fallback() || fallback_spare) &&
                   !paths.validating(other);
        };
        usize chosen = kQuicNoPath;
        for (usize other = 0U; other < kQuicMaxPaths; ++other)
        {
            const u64 sequence = path_sequence(slot, other);
            bool frees = paths.used(other) && spare(other) && peer_held(slot, sequence) &&
                         sequence != path_sequence(slot, paths.active());
            for (usize holder = 0U; holder < kQuicMaxPaths && frees; ++holder)
            {
                frees = !paths.used(holder) || path_sequence(slot, holder) != sequence || spare(holder);
            }
            if (frees && (chosen == kQuicNoPath || paths.last_ns(other) < paths.last_ns(chosen)))
            {
                chosen = other;
            }
        }
        const u64 given = (chosen == kQuicNoPath) ? kNoSequence : path_sequence(slot, chosen);
        for (usize holder = 0U; holder < kQuicMaxPaths && given != kNoSequence; ++holder)
        {
            if (paths.used(holder) && holder != paths.active() && path_sequence(slot, holder) == given)
            {
                drop_path(slot, holder, now_ns); // the last one retires the ID
            }
        }
    }
}

usize QuicConnection::take_path(usize slot, const QuicPath& path, u64 now_ns) noexcept
{
    QuicPaths& paths = tuples(slot);
    usize index = paths.admit(path, now_ns);
    if (index == kQuicNoPath)
    {
        drop_path(slot, paths.least_recent(), now_ns);
        index = paths.admit(path, now_ns);
    }
    return index;
}

u64 QuicConnection::unused_peer_sequence(usize slot, usize index) const noexcept
{
    const QuicPaths& paths = tuples(slot);
    const u64 path_id = m_protection.path_id(slot);
    usize held = 0U;
    for (const QuicPeerCid* id = m_cids.peer_at(path_id, held); id != nullptr; id = m_cids.peer_at(path_id, ++held))
    {
        const u64 sequence = id->sequence;
        bool taken = id->used;
        for (usize other = 0U; other < kQuicMaxPaths; ++other)
        {
            taken = taken || (other != index && paths.used(other) && path_sequence(slot, other) == sequence);
        }
        if (!taken)
        {
            return sequence;
        }
    }
    return kNoSequence;
}

QuicMigration QuicConnection::probe_path(const QuicPath& path, u64 now_ns) noexcept
{
    usize index = kQuicNoPath;
    return open_own_path(path, now_ns, index);
}

QuicMigration QuicConnection::migrate(const QuicPath& path, u64 now_ns) noexcept
{
    const QuicPaths& paths = m_first_paths.paths;
    usize index = kQuicNoPath;
    const QuicMigration result = open_own_path(path, now_ns, index);
    if (result != QuicMigration::Started)
    {
        return result;
    }
    if (toward_preferred(path.peer))
    {
        // Validated before datagrams move there (section 9.6.2).
        if (paths.proven(index))
        {
            take_preferred(index, now_ns);
        }
        else
        {
            m_deferred_move = index;
        }
        return result;
    }
    move_to(0U, index, now_ns);
    reannounce_local_ids();
    // Moved before reaching the preferred address: it is validated from here too (section 9.6.3).
    if (!m_on_preferred)
    {
        follow_preferred(now_ns);
    }
    return result;
}

void QuicConnection::reannounce_local_ids() noexcept
{
    // The server answers on the new path only with an ID of this side's it holds unused (section 9.5).
    m_cids.reannounce();
}

QuicMigration QuicConnection::open_own_path(const QuicPath& path, u64 now_ns, usize& index) noexcept
{
    const QuicPaths& paths = m_first_paths.paths;
    const QuicPath& active = paths.path(paths.active());
    if (m_config.side != crypto::QuicSide::Client)
    {
        return QuicMigration::NotClient;
    }
    if (m_state != QuicConnectionState::Established || !m_confirmed)
    {
        return QuicMigration::NotConfirmed;
    }
    if (!zero_sending())
    {
        return QuicMigration::Abandoned; // path ID 0's 4-tuples are the ones these move (RFC 10069 section 3.1.2)
    }
    const bool preferred = toward_preferred(path.peer);
    // disable_active_migration covers the handshake's address alone (section 18.2).
    if (m_peer.disable_active_migration && !m_on_preferred && !preferred)
    {
        return QuicMigration::Disabled;
    }
    if ((!(path.peer == active.peer) && !preferred) || active.peer.port == 0U)
    {
        return QuicMigration::OtherServer; // or the active path names no server address to keep
    }
    if (path == active)
    {
        return QuicMigration::ActivePath;
    }
    return open_path(path, now_ns, index);
}

QuicMigration QuicConnection::open_path(const QuicPath& path, u64 now_ns, usize& index, bool wait_for_id) noexcept
{
    QuicPaths& paths = m_first_paths.paths;
    index = paths.find(path);
    const bool addressed = index != kQuicNoPath && peer_held(0U, paths.peer_sequence(index));
    if (!addressed && !wait_for_id && unused_peer_sequence(0U, index) == kNoSequence)
    {
        index = kQuicNoPath;
        return QuicMigration::NoConnectionId;
    }
    if (index == kQuicNoPath)
    {
        index = take_path(0U, path, now_ns);
        // The server sends to the ID it last used here until it moves; the client's own address is not linked.
        paths.set_local_sequence(index, paths.local_sequence(paths.active()));
        paths.trust(index);
    }
    if (!addressed)
    {
        paths.set_peer_sequence(index, unused_peer_sequence(0U, index));
    }
    if (!paths.proven(index))
    {
        validate_path(0U, index, now_ns, validation_timeout_ns());
    }
    return QuicMigration::Started;
}

void QuicConnection::follow_packet(usize slot, u64 number, bool probing, u64 now_ns) noexcept
{
    QuicPaths& paths = tuples(slot);
    const usize path = m_receive_path;
    if (path == kQuicNoPath ||
        (m_state != QuicConnectionState::Established && m_state != QuicConnectionState::Handshaking))
    {
        return;
    }
    if (m_packet_local.found())
    {
        paths.set_local_sequence(path, m_packet_local.sequence);
    }
    const bool sendable = peer_held(slot, path_sequence(slot, path));
    u64& largest = m_tuples[slot]->largest_non_probing;
    usize& waiting = m_tuples[slot]->waiting;
    if (!probing && (largest == kQuicNoPacket || number > largest))
    {
        // Only the highest-numbered non-probing packet moves the path, so a reordered one never moves it back.
        largest = number;
        waiting = kQuicNoPath; // this packet chose its path
        if (m_config.side == crypto::QuicSide::Client && path == paths.active() && !m_tuples[slot]->peer_followed)
        {
            m_tuples[slot]->peer_followed = true; // the server's datagrams reach the path this side moved to
            leave_fallback(slot, now_ns);
        }
        if (m_config.side == crypto::QuicSide::Server && handshake_slot(slot))
        {
            m_deferred_move = kQuicNoPath; // a later packet elsewhere chose another path
        }
        if (handshake_slot(slot) && path != paths.active() && follows_peer() &&
            paths.path(path).socket != paths.path(paths.active()).socket)
        {
            // The client moved to the preferred socket: datagrams follow once this side validated the path there
            // (section 9.6.2), never on the packet alone.
            if (paths.validated(path) && sendable)
            {
                take_preferred(path, now_ns);
                return;
            }
            m_deferred_move = path;
            validate_path(slot, path, now_ns, validation_timeout_ns());
            return;
        }
        if (path != paths.active() && follows_peer())
        {
            if (sendable)
            {
                follow_peer(slot, path, now_ns);
                return;
            }
            // No ID of the peer's is unused for it (section 9.5): the move waits for the peer's next one, and the
            // path validates meanwhile, its challenge waiting for the ID too. address_paths() retires an ID left on
            // other paths to draw one; with none to retire, the validation times out and the path is forgotten.
            waiting = path;
            if (!paths.validated(path))
            {
                validate_path(slot, path, now_ns, validation_timeout_ns());
            }
        }
        return;
    }
    follow_waiting(slot, now_ns); // the ID may have come in this packet
    // A probe from a path not validated is answered with a challenge of this side's (section 9.6.3), which leaves
    // once the path has a peer ID to send to: one that waits for it is validating, never a path left behind.
    if (probing && path != paths.active() && follows_peer() && !paths.validated(path))
    {
        validate_path(slot, path, now_ns, validation_timeout_ns());
    }
}

void QuicConnection::follow_waiting(usize slot, u64 now_ns) noexcept
{
    usize& waiting = m_tuples[slot]->waiting;
    if (waiting == kQuicNoPath || waiting == tuples(slot).active() || !follows_peer() || stopped(slot) ||
        !peer_held(slot, path_sequence(slot, waiting)))
    {
        return;
    }
    const usize to = waiting;
    waiting = kQuicNoPath;
    follow_peer(slot, to, now_ns);
}

void QuicConnection::follow_peer(usize slot, usize index, u64 now_ns) noexcept
{
    const QuicPaths& paths = tuples(slot);
    const usize from = paths.active();
    const u64 timeout = validation_timeout_ns();
    move_to(slot, index, now_ns);
    if (!paths.validated(index))
    {
        validate_path(slot, index, now_ns, timeout);
    }
    if (paths.validated(from))
    {
        // An off-path attacker's copy looks like a rebinding: the path before it is challenged, and a peer still
        // there answers and sends on it, which moves the connection back (section 9.3.3). Its address is validated,
        // so the check goes at once.
        validate_path(slot, from, now_ns, timeout);
    }
    else
    {
        // A move away from a path never validated abandons it: a spurious migration (section 9.3.2).
        drop_path(slot, from, now_ns);
    }
}

void QuicConnection::move_to(usize slot, usize index, u64 now_ns) noexcept
{
    QuicPaths& paths = tuples(slot);
    const usize from = paths.active();
    paths.set_peer_sequence(from, path_sequence(slot, from));
    paths.activate(index);
    if (paths.proven(index))
    {
        // Its check of section 9.3.3, if one runs, is answered: the peer is there. A path whose address an
        // unexpanded challenge validated still owes its expanded one, which goes on.
        paths.abandon(index);
    }
    m_cids.set_current(m_protection.path_id(slot), paths.peer_sequence(index));
    ++m_counters.migrations;
    m_tuples[slot]->peer_followed = false;
    follow_congestion(slot, now_ns);
    m_recovery->set_amplification_blocked(!handshake_validated() && amplification_credit() < kMinUsefulPacket,
                                          now_ns);
}

void QuicConnection::leave_fallback(usize slot, u64 now_ns) noexcept
{
    const QuicPaths& paths = tuples(slot);
    // A client proven on the path it moved to, whose server sends there too, forgets the path it left and retires
    // that path's peer ID, so the server issues the ID the next move takes (section 9.5). Until then the server's
    // datagrams to the old path still arrive.
    if (m_config.side == crypto::QuicSide::Client && paths.proven(paths.active()) && m_tuples[slot]->peer_followed)
    {
        drop_path(slot, paths.fallback(), now_ns);
    }
}

void QuicConnection::follow_congestion(usize slot, u64 now_ns) noexcept
{
    const QuicPaths& paths = tuples(slot);
    const QuicPath& active = paths.path(paths.active());
    QuicPathControl& sending = control(slot);
    if (!paths.validated(paths.active()) || sending.path() == active)
    {
        return;
    }
    // Section 9.4: on confirming the peer's address, initial values unless only its port changed. Until then the
    // credit and 1200-byte datagrams hold the path, and a spurious move never confirmed costs the state nothing.
    if (quic_port_only_change(sending.path(), active))
    {
        sending.bind(active);
        return;
    }
    // A path of another family is the only thing known of it: its floor, then the search (RFC 8899 section 5.2).
    sending.restart(active, family_base(active, m_config.max_datagram_bytes), entropy());
    m_recovery->restart_path(now_ns, slot);
    follow_datagram_size();
}

Endpoint QuicConnection::preferred_for(const Endpoint& peer) const noexcept
{
    return (m_peer_known && m_peer.has_preferred_address) ? preferred_endpoint(m_peer.preferred_address, peer.address)
                                                          : Endpoint{};
}

bool QuicConnection::toward_preferred(const Endpoint& peer) const noexcept
{
    // A path that names no server address (port 0) has none to move from.
    const Endpoint& server = server_address();
    return m_config.side == crypto::QuicSide::Client && !m_on_preferred && peer.port != 0U && server.port != 0U &&
           peer == preferred_for(peer) && !(peer == server);
}

void QuicConnection::follow_preferred(u64 now_ns) noexcept
{
    const QuicPaths& paths = m_first_paths.paths;
    const QuicPath& active = paths.path(paths.active());
    const QuicPath target{active.socket, preferred_for(active.peer)};
    // By itself, or for a move migrate() asked for that waits for its validation (a socket left behind never draws
    // the connection back).
    const bool wanted = m_config.follow_preferred_address || m_deferred_move != kQuicNoPath;
    if (!wanted || m_state != QuicConnectionState::Established || !m_confirmed || !zero_sending() ||
        !toward_preferred(target.peer))
    {
        return; // the preferred address is path ID 0's (RFC 10069 section 2.2): nothing once that is abandoned
    }
    if (m_deferred_move != kQuicNoPath && paths.path(m_deferred_move).socket != active.socket)
    {
        // The validation from a socket this side left ends; its ID retires, and the peer's replacement reaches the
        // path from here (address_paths()), which waits for one when none is unused.
        drop_path(0U, m_deferred_move, now_ns);
    }
    usize index = kQuicNoPath;
    if (open_path(target, now_ns, index, true) == QuicMigration::Started)
    {
        m_deferred_move = index;
    }
}

void QuicConnection::take_preferred(usize index, u64 now_ns) noexcept
{
    const QuicPaths& paths = m_first_paths.paths;
    const bool server = m_config.side == crypto::QuicSide::Server;
    m_deferred_move = kQuicNoPath;
    m_on_preferred = true;
    move_to(0U, index, now_ns);
    const QuicPath& active = paths.path(index);
    for (usize other = 0U; other < kQuicMaxPaths; ++other)
    {
        // A server's other sockets are no longer the connection's (section 9.6.2). A client stops using the
        // server's other address (section 9.6.1), abandoning any validation there (section 9.6.3); the path it moved
        // from stays until the server follows (leave_fallback()).
        const QuicPath& known = paths.path(other);
        const bool left =
            server ? known.socket != active.socket : !(known.peer == active.peer) && other != paths.fallback();
        if (paths.used(other) && other != index && left)
        {
            drop_path(0U, other, now_ns);
        }
    }
    if (!server)
    {
        reannounce_local_ids();
    }
}

void QuicConnection::on_validated(usize slot, usize index, u64 now_ns) noexcept
{
    const QuicPaths& paths = tuples(slot);
    if (stopped(slot))
    {
        return; // abandoned: nothing goes there again
    }
    ++m_counters.path_validations;
    if (handshake_slot(slot) && index == m_deferred_move && path_sendable(slot, index))
    {
        take_preferred(index, now_ns); // section 9.6.2: validated, so datagrams move there
    }
    if (index == paths.active())
    {
        if (handshake_slot(slot))
        {
            m_recovery->set_amplification_blocked(false, now_ns); // the handshake's path, which the limit covers
        }
        follow_congestion(slot, now_ns);
        leave_fallback(slot, now_ns);
    }
    address_paths(slot, now_ns); // a path whose check ended may now give its ID to one that waits
}

void QuicConnection::expire_paths(u64 now_ns) noexcept
{
    // A sending slot's 4-tuples that failed validation are forgotten. When the active one fails, the handshake slot
    // falls back to its last validated one (RFC 9000 section 9.3.2); with none, or in a further slot, its path ID is
    // abandoned (RFC 10069 section 3.1) with the code of a path that does not work, and the last path still sending
    // ends the connection silently (section 9.3.2: no validated path is left).
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (!m_protection.path_open(slot) || m_controls[slot] == nullptr)
        {
            continue;
        }
        QuicPaths& paths = tuples(slot);
        for (usize index = paths.expired(now_ns); index != kQuicNoPath; index = paths.expired(now_ns))
        {
            ++m_counters.path_failures;
            const usize back = handshake_slot(slot) ? paths.fallback() : kQuicNoPath;
            if (index != paths.active() || back != kQuicNoPath)
            {
                if (index == paths.active())
                {
                    move_to(slot, back, now_ns);
                    if (follows_peer())
                    {
                        // A server keeps the address it reverted from, with the ID it sent there: the client's next
                        // packet from there moves it back at once (section 9.3), linking nothing new (section 9.5).
                        // Not validating, it gives the ID up when another path needs one (address_paths()).
                        continue;
                    }
                }
                drop_path(slot, index, now_ns);
                continue;
            }
            if (sending_slots() == 1U)
            {
                m_close = QuicCloseInfo{};
                m_close.path_lost = true;
                finish();
                return;
            }
            static_cast<void>(begin_abandon(m_protection.path_id(slot),
                                            static_cast<u64>(QuicTransportError::PathUnstableOrPoor), now_ns));
            break;
        }
    }
}

void QuicConnection::drop_path(usize slot, usize index, u64 now_ns) noexcept
{
    QuicPaths& paths = tuples(slot);
    if (!paths.used(index) || index == paths.active())
    {
        return;
    }
    m_deferred_move = (handshake_slot(slot) && m_deferred_move == index) ? kQuicNoPath : m_deferred_move;
    m_tuples[slot]->waiting = (m_tuples[slot]->waiting == index) ? kQuicNoPath : m_tuples[slot]->waiting;
    const u64 sequence = paths.peer_sequence(index);
    bool shared = sequence == kNoSequence;
    for (usize other = 0U; other < kQuicMaxPaths; ++other)
    {
        shared = shared || (other != index && paths.used(other) && path_sequence(slot, other) == sequence);
    }
    paths.drop(index);
    const u64 path_id = m_protection.path_id(slot);
    const QuicPeerCid* held = m_cids.peer(path_id, sequence);
    if (shared || held == nullptr || !held->used)
    {
        return;
    }
    // An ID sent to on the forgotten path alone is retired, so that no later path is linked to it (section 9.5).
    if (!m_cids.retire_peer(path_id, sequence, m_local.active_connection_id_limit))
    {
        fail(QuicTransportError::ConnectionIdLimitError, 0U, now_ns);
    }
}

u64 QuicConnection::path_sequence(usize slot, usize index) const noexcept
{
    const QuicPaths& paths = tuples(slot);
    if (index != paths.active())
    {
        return paths.peer_sequence(index);
    }
    return m_cids.current(m_protection.path_id(slot));
}

bool QuicConnection::path_sendable(usize slot, usize index) const noexcept
{
    const QuicPaths& paths = tuples(slot);
    const usize application = quic_space_index(QuicSpace::Application);
    return m_state == QuicConnectionState::Established && m_confirmed && !m_space_gone[application] &&
           m_protection.can_send(QuicPacketType::OneRtt) && m_recovery->can_send(QuicSpace::Application, slot) &&
           paths.credit(index) >= kMinUsefulPacket && peer_held(slot, path_sequence(slot, index));
}

bool QuicConnection::path_held(usize slot, usize index, u32 held_sockets) const noexcept
{
    const QuicPaths& paths = tuples(slot);
    const u32 socket = paths.path(index).socket;
    return socket < 32U && ((held_sockets >> socket) & 1U) != 0U;
}

u64 QuicConnection::validation_timeout_ns() const noexcept
{
    // Three times the larger of the leading path's probe period and a new path's, from the initial RTT (8.2.4):
    // smoothed plus four variances is three initial RTTs, then the peer's max_ack_delay.
    const u64 fresh = add_saturating(m_config.initial_rtt_ns > kNeverNs / 3U ? kNeverNs : 3U * m_config.initial_rtt_ns,
                                     m_peer.max_ack_delay * kNsPerMs);
    const u64 period = max_u64(m_recovery->probe_period_ns(QuicSpace::Application, lead_slot()), fresh);
    return (period > kNeverNs / 3U) ? kNeverNs : 3U * period;
}

void QuicConnection::validate_path(usize slot, usize index, u64 now_ns, u64 timeout_ns) noexcept
{
    QuicPaths& paths = tuples(slot);
    if (paths.validating(index))
    {
        return; // a repeat takes no turn
    }
    paths.validate(index, address_proven(paths.path(index)) ? now_ns : validation_turn(now_ns, timeout_ns), timeout_ns);
}

bool QuicConnection::address_proven(const QuicPath& path) const noexcept
{
    // Validated on any path ID's 4-tuples: the peer's own address, never a target of its choosing.
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        const usize index = m_protection.path_open(slot) ? tuples(slot).find(path) : kQuicNoPath;
        if (index != kQuicNoPath && tuples(slot).validated(index))
        {
            return true;
        }
    }
    return false;
}

u64 QuicConnection::validation_turn(u64 now_ns, u64 timeout_ns) noexcept
{
    // A peer may make this side challenge an address on each of its 4-tuples of every path ID at once, each a target
    // it chose (RFC 10069 section 7.2). Each validation of an address not validated holds a turn until its deadline,
    // which it cannot outlive, so at most kQuicMaxPaths run at once: every further path ID and a rebinding together.
    // An address validated on any path ID is the peer's, and its checks (section 9.3.3) wait for no turn.
    usize earliest = 0U;
    for (usize turn = 1U; turn < kQuicMaxPaths; ++turn)
    {
        earliest = (m_turn_ends_ns[turn] < m_turn_ends_ns[earliest]) ? turn : earliest;
    }
    const u64 start = max_u64(now_ns, m_turn_ends_ns[earliest]);
    m_turn_ends_ns[earliest] = quic_validation_deadline(start, timeout_ns);
    return start;
}

u64 QuicConnection::path_timer_ns() const noexcept
{
    // A challenge is due only where it can go: one the amplification limit holds waits for a datagram instead. Every
    // open slot's, which send() and expire_paths() serve.
    u64 next = kNeverNs;
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (!m_protection.path_open(slot) || stopped(slot))
        {
            continue;
        }
        const QuicPaths& paths = tuples(slot);
        for (usize index = 0U; index < kQuicMaxPaths; ++index)
        {
            if (paths.used(index))
            {
                next = min_u64(next, paths.deadline_ns(index));
                next = path_sendable(slot, index) ? min_u64(next, paths.due_ns(index)) : next;
            }
        }
    }
    return next;
}

// ---- State ----

cont::ConstSpan<u8> QuicConnection::peer_cid(usize slot) const noexcept
{
    // A client before its server's first packet sends to its first Destination Connection ID.
    const u64 path_id = m_protection.path_id(slot);
    const QuicPeerCid* current = m_cids.peer(path_id, m_cids.current(path_id));
    return (current == nullptr) ? m_initial_dcid.view() : current->id.view();
}

usize QuicConnection::datagram_slot(cont::ConstSpan<u8> datagram) const noexcept
{
    // A long header is the handshake's; a short header's Destination Connection ID is as long as this side's IDs. An
    // ID not this side's leaves the datagram to the leading slot, whose stateless reset check it may be.
    const usize bytes = m_local_cid.bytes;
    if (bytes == 0U || datagram.size() <= bytes || (datagram[0] & 0x80U) != 0U)
    {
        return lead_slot();
    }
    const QuicCidRef local = m_cids.find(cont::ConstSpan<u8>(datagram.data() + 1U, bytes));
    return local.found() ? slot_of(local.path_id) : lead_slot();
}

usize QuicConnection::begin_path(cont::ConstSpan<u8> datagram, const QuicPath& from, u64 now_ns) noexcept
{
    // Only a client opens a path, after the handshake (RFC 10069 section 3), to an ID this side issued for a path ID
    // still open for it; the slot's state is the pool's, so nothing is allocated on the peer's behalf.
    const usize bytes = m_local_cid.bytes;
    if (m_config.side != crypto::QuicSide::Server || !m_multipath || !m_confirmed ||
        m_state != QuicConnectionState::Established || m_slots == nullptr || datagram.size() <= bytes ||
        (datagram[0] & 0x80U) != 0U)
    {
        return kQuicNoPath;
    }
    const QuicCidRef local = m_cids.find(cont::ConstSpan<u8>(datagram.data() + 1U, bytes));
    if (!local.found() || m_cids.abandoned(local.path_id))
    {
        return kQuicNoPath;
    }
    // With every slot taken, the path waits for one to close (section 3.1).
    const usize slot = free_slot();
    if (slot == kQuicNoPath || !claim_slot(slot, local.path_id, from, now_ns))
    {
        return kQuicNoPath;
    }
    m_tentative_slot = slot;
    return slot;
}

usize QuicConnection::free_slot() const noexcept
{
    // Slot 0 is free once path ID 0 closed (close_abandoned): reclaimed like any other.
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (!m_protection.path_open(slot) && !m_recovery->path_open(slot))
        {
            return slot;
        }
    }
    return kQuicNoPath;
}

bool QuicConnection::claim_slot(usize slot, u64 path_id, const QuicPath& from, u64 now_ns) noexcept
{
    // The path ID is at most this side's limit, at most 2^32 - 1: it fits the nonce's 32 bits (section 2.4).
    if (!m_protection.open_path(slot, static_cast<u32>(path_id)))
    {
        return false;
    }
    QuicPaths& paths = m_tuples[slot]->paths;
    *m_tuples[slot] = Tuples{};
    paths.open(from, false, now_ns);
    if (m_config.side == crypto::QuicSide::Client)
    {
        paths.trust(paths.active());
    }
    m_numbers[number_space(QuicSpace::Application, slot)] = Numbering{};
    return true;
}

QuicMigration QuicConnection::add_path(const QuicPath& path, u64 now_ns, u64& path_id) noexcept
{
    path_id = 0U;
    const Endpoint& server = server_address();
    if (m_config.side != crypto::QuicSide::Client)
    {
        return QuicMigration::NotClient;
    }
    if (m_state != QuicConnectionState::Established || !m_confirmed)
    {
        return QuicMigration::NotConfirmed;
    }
    if (!m_multipath || m_slots == nullptr)
    {
        return QuicMigration::NoMultipath;
    }
    const bool preferred = toward_preferred(path.peer);
    // A path to the handshake address is a new path there (RFC 10069 section 2.2); the preferred one stays open.
    if (m_peer.disable_active_migration && !m_on_preferred && !preferred)
    {
        return QuicMigration::Disabled;
    }
    if ((!(path.peer == server) && !preferred) || server.port == 0U)
    {
        return QuicMigration::OtherServer;
    }
    // Path IDs are taken in rising order (section 3.2.1); one unused is open in no slot, its ID space open and not
    // abandoned (a closed or abandoned one is consumed). The first with IDs both ways is opened.
    // The open ID spaces are walked, at most kQuicMaxPaths however far the limits have risen.
    const u64 limit = min_u64(m_local_max_path_id, m_peer_max_path_id);
    u64 unaddressed = 0U;  // the smallest unused one without an ID of the server's
    bool unissued = false; // an unused one without an ID of this side's
    u64 chosen = 0U;
    for (u64 candidate = m_cids.open_after(0U); candidate <= limit && chosen == 0U;
         candidate = m_cids.open_after(candidate))
    {
        if (slot_of(candidate) != kQuicNoPath || m_cids.abandoned(candidate))
        {
            continue;
        }
        const bool peer_ids = m_cids.current(candidate) != kNoSequence;
        const bool own_ids = m_cids.local_count(candidate) != 0U;
        unaddressed = (!peer_ids && unaddressed == 0U) ? candidate : unaddressed;
        unissued = unissued || !own_ids;
        chosen = (peer_ids && own_ids) ? candidate : 0U;
    }
    if (chosen == 0U)
    {
        // The server is told what holds the path back (section 4.7): no ID of its own for the smallest path ID, or
        // its limit, never this side's own (which this side would raise itself, section 3.2.1).
        if (unaddressed != 0U || unissued)
        {
            m_cids_blocked = (unaddressed != 0U) ? unaddressed : m_cids_blocked;
            return QuicMigration::NoConnectionId;
        }
        m_paths_blocked_owed = m_paths_blocked_owed || m_peer_max_path_id < m_local_max_path_id;
        return QuicMigration::NoPathId;
    }
    const usize slot = free_slot();
    if (slot == kQuicNoPath || !claim_slot(slot, chosen, path, now_ns))
    {
        return QuicMigration::NoPathId;
    }
    commit_path(slot, now_ns);
    path_id = chosen;
    return QuicMigration::Started;
}

void QuicConnection::commit_path(usize slot, u64 now_ns) noexcept
{
    // A reclaimed slot 0's state is path ID 0's: the connection's own controller and the recovery's own, over the
    // Application packets' first storage; a further slot's is the pool's.
    const usize per_space = usize{m_config.sent_packets} + kSentPacketReserve;
    const bool first = slot == 0U;
    QuicPathControl& control = first ? m_first_control : m_slots[slot - 1U].control;
    QuicPathRecovery& recovery = first ? m_recovery->first_path() : m_slots[slot - 1U].recovery;
    QuicSentEntry* const entries = first ? m_storage + (quic_space_index(QuicSpace::Application) * per_space)
                                         : m_slot_entries + ((slot - 1U) * per_space);
    QuicPaths& paths = tuples(slot);
    const QuicPath& from = paths.path(paths.active());
    // The handshake link's handover schedule says nothing of this one; learning may still find its own.
    QuicPathControlConfig config = path_config(m_config);
    config.handover = HandoverSchedule{};
    config.base_datagram_bytes = family_base(from, m_config.max_datagram_bytes);
    // The policy and sizes were checked (config_valid, family_base); the controller is built in place.
    static_cast<void>(control.start(config, from, entropy(), &m_coupling));
    // Free in the recovery (free_slot), with storage of its own.
    static_cast<void>(m_recovery->open_path(slot, recovery, cont::Span<QuicSentEntry>(entries, per_space)));
    control.limit(peer_ceiling());
    m_controls[slot] = &control;
    m_datagrams.open_path(slot, m_protection.path_id(slot));
    m_streams.open_path(slot, m_protection.path_id(slot));
    follow_datagram_size();
    // A server validates the client's address before any data goes there, a client its server's (section 3.1).
    validate_path(slot, paths.active(), now_ns, validation_timeout_ns());
    m_tentative_slot = kQuicNoPath;
    ++m_counters.paths_opened;
}

void QuicConnection::undo_path(usize slot) noexcept
{
    static_cast<void>(m_protection.close_path(slot)); // opened by begin_path, never the last one open
    *m_tuples[slot] = Tuples{};
    m_tentative_slot = kQuicNoPath;
}

QuicAbandon QuicConnection::abandon_path(u64 path_id, u64 error_code, u64 now_ns) noexcept
{
    if (m_state != QuicConnectionState::Established)
    {
        return QuicAbandon::NotEstablished;
    }
    if (!m_multipath)
    {
        return QuicAbandon::NoMultipath;
    }
    if (error_code > kQuicMaxVarint)
    {
        return QuicAbandon::BadError;
    }
    // A frame naming a path ID above the peer's limit is PROTOCOL_VIOLATION there (section 4), and no path of it can
    // have opened.
    if (path_id > m_peer_max_path_id)
    {
        return QuicAbandon::NotOpen;
    }
    // The last path still sending would leave none to carry the frame: closing the connection ends it (section 3.4).
    const usize slot = slot_of(path_id);
    if (slot != kQuicNoPath && m_controls[slot] != nullptr && sending_slots() == 1U)
    {
        return QuicAbandon::LastPath;
    }
    return (begin_abandon(path_id, error_code, now_ns) != nullptr) ? QuicAbandon::Abandoned : QuicAbandon::NotOpen;
}

QuicPathAbandon* QuicConnection::begin_abandon(u64 path_id, u64 error_code, u64 now_ns) noexcept
{
    // Each entry is an abandoned ID space still open, so one is free while a space is open and not abandoned.
    QuicPathAbandon* entry = nullptr;
    for (QuicPathAbandon& held : m_abandons)
    {
        entry = (entry == nullptr && !held.used) ? &held : entry;
    }
    // A slot still sending is never the last one here (abandon_path() and peer_abandon() close or refuse that), and
    // a second one opens only after confirmation, so path ID 0's handshake spaces are gone by then.
    const usize slot = slot_of(path_id);
    const bool sending = slot != kQuicNoPath && m_controls[slot] != nullptr;
    if (entry == nullptr || !m_cids.abandon(path_id))
    {
        return nullptr;
    }
    *entry = QuicPathAbandon{};
    entry->path_id = path_id;
    entry->error_code = error_code;
    entry->used = true;
    entry->owed = true;
    m_cids_blocked = (m_cids_blocked == path_id) ? 0U : m_cids_blocked;
    ++m_counters.paths_abandoned;
    if (sending)
    {
        m_event_ns = now_ns;
        static_cast<void>(m_recovery->stop_path(slot, now_ns, m_events));
        control(slot).stop();
        m_controls[slot] = nullptr;
        m_datagrams.stop_path(slot);
        m_streams.stop_path(slot); // pinned streams' data, lost or unsent, goes on any path
        follow_datagram_size();
        // Path ID 0's own moves end with it: a move it deferred and its pending ones (S75) wait for nothing more.
        m_deferred_move = handshake_slot(slot) ? kQuicNoPath : m_deferred_move;
    }
    return entry;
}

bool QuicConnection::peer_abandon(const QuicFrame& frame, u64 now_ns) noexcept
{
    // The last path still sending (path ID 0 before confirmation, when no other can have opened): the connection
    // closes, the close going on that path (section 3.4's SHOULD; its MAYs need a path validated first).
    const usize slot = slot_of(frame.path_id);
    if (slot != kQuicNoPath && m_controls[slot] != nullptr && sending_slots() == 1U)
    {
        fail(QuicTransportError::NoError, 0U, now_ns);
        return false;
    }
    // Answered whether its path was used or not (section 3.4); a path ID closed already is ignored (section 4).
    QuicPathAbandon* entry = find_abandon(frame.path_id);
    if (entry == nullptr)
    {
        entry = begin_abandon(frame.path_id, static_cast<u64>(QuicTransportError::NoError), now_ns);
        if (entry != nullptr && frame.path_id > m_peer_max_path_id)
        {
            // Above the peer's own limit, which no frame of this side may name (section 4): nothing to answer.
            entry->owed = false;
            entry->acked = true;
        }
    }
    if (entry == nullptr || entry->answered)
    {
        return true; // a repeat: the retention runs from the first, which no flood of copies extends
    }
    entry->answered = true;
    entry->peer_error_code = frame.error_code;
    if (entry->acked)
    {
        // Three PTOs from this frame, or from this side's acknowledgement when that ends later.
        const u64 end = retention_end_ns(entry->path_id, now_ns);
        entry->deadline_ns = (entry->deadline_ns == kNeverNs) ? end : max_u64(entry->deadline_ns, end);
    }
    return true;
}

usize QuicConnection::abandon_index(u64 path_id) const noexcept
{
    usize index = 0U;
    while (index < kQuicMaxPaths && !(m_abandons[index].used && m_abandons[index].path_id == path_id))
    {
        ++index;
    }
    return index;
}

QuicPathAbandon* QuicConnection::find_abandon(u64 path_id) noexcept
{
    const usize index = abandon_index(path_id);
    return (index < kQuicMaxPaths) ? &m_abandons[index] : nullptr;
}

const QuicPathAbandon* QuicConnection::abandon_of(u64 path_id) const noexcept
{
    const usize index = abandon_index(path_id);
    return (index < kQuicMaxPaths) ? &m_abandons[index] : nullptr;
}

u64 QuicConnection::retention_end_ns(u64 path_id, u64 now_ns) const noexcept
{
    // Its own path's stragglers take its round trip, which the paths still sending may not cover (section 3.4).
    const usize slot = slot_of(path_id);
    u64 period = m_recovery->largest_probe_period_ns();
    if (slot != kQuicNoPath)
    {
        period = max_u64(period, m_recovery->probe_period_ns(QuicSpace::Application, slot));
    }
    return add_saturating(now_ns, three_times(period));
}

void QuicConnection::close_abandoned(u64 now_ns) noexcept
{
    for (QuicPathAbandon& entry : m_abandons)
    {
        if (!entry.used || now_ns < entry.deadline_ns)
        {
            continue;
        }
        const usize slot = slot_of(entry.path_id);
        if (slot != kQuicNoPath)
        {
            // Stopped: nothing of it is outstanding, so closing reports nothing (section 3.4.3).
            m_event_ns = now_ns;
            static_cast<void>(m_recovery->close_path(slot, now_ns, m_events));
            static_cast<void>(m_protection.close_path(slot));
            if (entry.path_id == 0U)
            {
                // The handshake slot is reclaimed for later path IDs (free_slot); add_path() still opens paths to
                // the server's address it sent to.
                m_zero_server = server_address();
                m_zero_closed = true;
            }
            *m_tuples[slot] = Tuples{};
            m_numbers[number_space(QuicSpace::Application, slot)] = Numbering{};
        }
        // Its IDs unrouted, its storage opens the path ID past this side's limit, which MAX_PATH_ID raises (sections
        // 3.2.1 and 4.6), as its slot is free for one: 1 + initial_max_path_id path IDs stay open.
        QuicCidSpace* const storage = m_cids.close(entry.path_id);
        if (storage != nullptr && m_local_max_path_id < kQuicMaxPathId &&
            m_cids.open(m_local_max_path_id + 1U, *storage))
        {
            ++m_local_max_path_id;
            m_max_path_id_owed = true;
        }
        for (usize index = 0U; index < kQuicMaxPaths; ++index)
        {
            for (PathStatusEntry* const held : {&m_peer_statuses[index], &m_local_statuses[index]})
            {
                if (held->status != QuicPathStatus::Unknown && held->path_id == entry.path_id)
                {
                    *held = PathStatusEntry{};
                }
            }
        }
        entry = QuicPathAbandon{};
        ++m_counters.paths_closed;
    }
}

bool QuicConnection::stopped_ack_due(u64 now_ns) const noexcept
{
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (stopped(slot) && m_recovery->ack_deadline_ns(QuicSpace::Application, slot) <= now_ns)
        {
            return true;
        }
    }
    return false;
}

usize QuicConnection::lead_slot() const noexcept
{
    // A path still validating leads only while no other can carry data: a path ID opened in a lower slot (a
    // reclaimed slot 0 among them) takes over the close and path() once validated, never before.
    usize first = kQuicNoPath;
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (m_controls[slot] == nullptr)
        {
            continue;
        }
        if (carries_data(slot, tuples(slot).active()))
        {
            return slot;
        }
        first = (first == kQuicNoPath) ? slot : first;
    }
    return (first != kQuicNoPath) ? first : 0U;
}

const Endpoint& QuicConnection::server_address() const noexcept
{
    const QuicPaths& paths = m_first_paths.paths;
    return m_zero_closed ? m_zero_server : paths.path(paths.active()).peer;
}

usize QuicConnection::sending_slots() const noexcept
{
    usize count = 0U;
    for (const QuicPathControl* control : m_controls)
    {
        count += (control != nullptr) ? 1U : 0U;
    }
    return count;
}

usize QuicConnection::slot_of(u64 path_id) const noexcept
{
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (m_protection.path_open(slot) && m_protection.path_id(slot) == path_id)
        {
            return slot;
        }
    }
    return kQuicNoPath;
}

QuicConnection::PathStatusEntry* QuicConnection::status_entry(PathStatusEntry (&table)[kQuicMaxPaths],
                                                              u64 path_id) noexcept
{
    PathStatusEntry* entry = nullptr;
    for (PathStatusEntry& held : table)
    {
        const bool known = held.status != QuicPathStatus::Unknown;
        if (known && held.path_id == path_id)
        {
            return &held;
        }
        entry = (!known && entry == nullptr) ? &held : entry;
    }
    return entry;
}

QuicPathStatus QuicConnection::peer_path_status(u64 path_id) const noexcept
{
    for (const PathStatusEntry& held : m_peer_statuses)
    {
        if (held.status != QuicPathStatus::Unknown && held.path_id == path_id)
        {
            return held.status;
        }
    }
    return QuicPathStatus::Unknown;
}

QuicPathStatus QuicConnection::path_status(u64 path_id) const noexcept
{
    for (const PathStatusEntry& held : m_local_statuses)
    {
        if (held.status != QuicPathStatus::Unknown && held.path_id == path_id)
        {
            return held.status;
        }
    }
    return QuicPathStatus::Unknown;
}

QuicPathStatusChange QuicConnection::set_path_status(u64 path_id, bool backup) noexcept
{
    if (m_state != QuicConnectionState::Established)
    {
        return QuicPathStatusChange::NotEstablished;
    }
    if (!m_multipath)
    {
        return QuicPathStatusChange::NoMultipath;
    }
    // A frame naming a path ID above the peer's limit is PROTOCOL_VIOLATION there (section 4); one this side has not
    // opened, or abandoned, names no path it may use. At most kQuicMaxPaths ID spaces are open, so an entry is free
    // for every path ID that passes.
    PathStatusEntry* const held = status_entry(m_local_statuses, path_id);
    if (path_id > m_peer_max_path_id || !m_cids.is_open(path_id) || m_cids.abandoned(path_id) || held == nullptr)
    {
        return QuicPathStatusChange::NotOpen;
    }
    PathStatusEntry& entry = *held;
    const QuicPathStatus status = backup ? QuicPathStatus::Backup : QuicPathStatus::Available;
    if (entry.status == status)
    {
        return QuicPathStatusChange::Set;
    }
    // Numbers rise per path ID, from 0 (section 4.3).
    const u64 sequence = (entry.status == QuicPathStatus::Unknown) ? 0U : entry.sequence + 1U;
    entry = PathStatusEntry{path_id, sequence, status, true};
    return QuicPathStatusChange::Set;
}

bool QuicConnection::status_owed() const noexcept
{
    bool owed = false;
    for (const PathStatusEntry& entry : m_local_statuses)
    {
        owed = owed || (entry.owed && !m_cids.abandoned(entry.path_id));
    }
    return owed;
}

u64 QuicConnection::next_packet_number(QuicSpace space, usize slot) const noexcept
{
    return (slot < kQuicMaxPaths) ? m_numbers[number_space(space, slot)].next : 0U;
}

bool QuicConnection::skipped(QuicSpace space, u64 number, usize slot) const noexcept
{
    return slot < kQuicMaxPaths && m_numbers[number_space(space, slot)].skipped == number;
}

} // namespace crd::net
