// crd-net -- QUIC transport parameters and compatible version negotiation (4.2a;
// docs/design/network-foundation.md#quic-transport-parameters).

#include <crd/containers/wire_bytes.hpp>
#include <crd/net/quic_transport_parameters.hpp>
#include <crd/net/register_ids.hpp>

#include <algorithm>

namespace crd::net
{
namespace
{
using Id = QuicTransportParameterId;

constexpr u64   kGreaseBase       = 27U; // reserved identifiers are 31 * N + 27 (section 18.1)
constexpr u64   kGreaseStep       = 31U;
constexpr usize kPreferredFixed   = 4U + 2U + 16U + 2U + 1U + kQuicResetTokenBytes;
constexpr u32   kSeenDatagram     = 17U; // bits of `seen` past the section 18.2 identifiers
constexpr u32   kSeenGreaseBit    = 18U;
constexpr u32   kSeenVersionInfo  = 19U;
constexpr u32   kSeenResetStreamAt = 20U;

bool equal_bytes(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (usize index = 0U; index < a.size(); ++index)
    {
        if (a[index] != b[index])
        {
            return false;
        }
    }
    return true;
}

bool contains(cont::ConstSpan<u32> versions, u32 version) noexcept
{
    return std::ranges::any_of(versions, [version](u32 listed) { return listed == version; });
}

// ---- Writing: one pass that counts and, given a writer, writes ----

class Emitter
{
public:
    explicit Emitter(cont::WireWriter* out) noexcept : m_out(out) {}

    [[nodiscard]] usize total() const noexcept { return m_total; }

    void varint(Id id, u64 value) noexcept { varint(static_cast<u64>(id), value); }

    void varint(u64 id, u64 value) noexcept
    {
        header(id, quic_varint_bytes(value));
        put_varint(value);
    }

    void flag(u64 id) noexcept { header(id, 0U); }

    void bytes(u64 id, cont::ConstSpan<u8> value) noexcept
    {
        header(id, value.size());
        put(value);
    }

    void preferred_address(const QuicPreferredAddress& address) noexcept
    {
        header(static_cast<u64>(Id::PreferredAddress), kPreferredFixed + address.connection_id.bytes);
        put(cont::ConstSpan<u8>(address.ipv4, 4U));
        put_u16(address.ipv4_port);
        put(cont::ConstSpan<u8>(address.ipv6, 16U));
        put_u16(address.ipv6_port);
        put_u8(address.connection_id.bytes);
        put(address.connection_id.view());
        put(cont::ConstSpan<u8>(address.reset_token, kQuicResetTokenBytes));
    }

    void version_information(const QuicVersionInformation& information, u32 grease_version) noexcept
    {
        header(iana::kQuicTpVersionInformation, 4U * (information.available_count + 2U));
        put_u32(information.chosen);
        for (const u32 version : information.available_view())
        {
            put_u32(version);
        }
        put_u32(grease_version);
    }

private:
    void header(u64 id, usize length) noexcept
    {
        put_varint(id);
        put_varint(length);
    }

    void put_varint(u64 value) noexcept
    {
        m_total += quic_varint_bytes(value);
        if (m_out != nullptr)
        {
            put_quic_varint(*m_out, value);
        }
    }

    void put(cont::ConstSpan<u8> value) noexcept
    {
        m_total += value.size();
        if (m_out != nullptr)
        {
            m_out->put_bytes(value);
        }
    }

    void put_u8(u8 value) noexcept
    {
        m_total += 1U;
        if (m_out != nullptr)
        {
            m_out->put_u8(value);
        }
    }

    void put_u16(u16 value) noexcept
    {
        m_total += 2U;
        if (m_out != nullptr)
        {
            m_out->put_u16(value);
        }
    }

    void put_u32(u32 value) noexcept
    {
        m_total += 4U;
        if (m_out != nullptr)
        {
            m_out->put_u32(value);
        }
    }

    cont::WireWriter* m_out   = nullptr;
    usize             m_total = 0U;
};

bool varints_in_range(const QuicTransportParameters& p) noexcept
{
    const u64 values[] = {p.max_idle_timeout,
                          p.max_udp_payload_size,
                          p.initial_max_data,
                          p.initial_max_stream_data_bidi_local,
                          p.initial_max_stream_data_bidi_remote,
                          p.initial_max_stream_data_uni,
                          p.active_connection_id_limit,
                          p.max_datagram_frame_size};
    return std::ranges::all_of(values, [](u64 value) { return value <= kQuicMaxVarint; });
}

bool version_information_valid(const QuicVersionInformation& information, crypto::QuicSide side) noexcept
{
    if (information.chosen == 0U || quic_version_reserved(information.chosen) || information.available_count == 0U ||
        information.available_count >= kQuicMaxAvailableVersions)
    {
        return false;
    }
    for (const u32 version : information.available_view())
    {
        if (version == 0U || quic_version_reserved(version))
        {
            return false;
        }
    }
    return side == crypto::QuicSide::Server || contains(information.available_view(), information.chosen);
}

// Whether `p` is a set write_quic_transport_parameters writes for `side`.
bool writable(const QuicTransportParameters& p, crypto::QuicSide side) noexcept
{
    const bool server_only = p.has_original_destination_connection_id || p.has_stateless_reset_token ||
                             p.has_preferred_address || p.has_retry_source_connection_id;
    if (side == crypto::QuicSide::Client && server_only)
    {
        return false;
    }
    if (!varints_in_range(p) || p.max_udp_payload_size < kQuicMinMaxUdpPayload ||
        p.ack_delay_exponent > kQuicMaxAckDelayExponent || p.max_ack_delay >= kQuicMaxAckDelayLimit ||
        p.active_connection_id_limit < kQuicDefaultActiveIdLimit || p.initial_max_streams_bidi > kQuicMaxStreams ||
        p.initial_max_streams_uni > kQuicMaxStreams)
    {
        return false;
    }
    const QuicConnectionId* ids[] = {&p.original_destination_connection_id, &p.initial_source_connection_id,
                                     &p.retry_source_connection_id, &p.preferred_address.connection_id};
    for (const QuicConnectionId* id : ids)
    {
        if (id->bytes > crypto::kQuicMaxConnectionIdBytes)
        {
            return false;
        }
    }
    if (p.has_preferred_address && p.preferred_address.connection_id.bytes == 0U)
    {
        return false;
    }
    return !p.has_version_information || version_information_valid(p.version_information, side);
}

void emit(const QuicTransportParameters& p, QuicTransportParameterGrease grease, Emitter& out) noexcept
{
    if (p.has_original_destination_connection_id)
    {
        out.bytes(static_cast<u64>(Id::OriginalDestinationConnectionId), p.original_destination_connection_id.view());
    }
    if (p.max_idle_timeout != 0U)
    {
        out.varint(Id::MaxIdleTimeout, p.max_idle_timeout);
    }
    if (p.has_stateless_reset_token)
    {
        out.bytes(static_cast<u64>(Id::StatelessResetToken),
                  cont::ConstSpan<u8>(p.stateless_reset_token, kQuicResetTokenBytes));
    }
    if (p.max_udp_payload_size != kQuicDefaultMaxUdpPayload)
    {
        out.varint(Id::MaxUdpPayloadSize, p.max_udp_payload_size);
    }
    const struct
    {
        Id  id;
        u64 value;
    } limits[] = {{Id::InitialMaxData, p.initial_max_data},
                  {Id::InitialMaxStreamDataBidiLocal, p.initial_max_stream_data_bidi_local},
                  {Id::InitialMaxStreamDataBidiRemote, p.initial_max_stream_data_bidi_remote},
                  {Id::InitialMaxStreamDataUni, p.initial_max_stream_data_uni},
                  {Id::InitialMaxStreamsBidi, p.initial_max_streams_bidi},
                  {Id::InitialMaxStreamsUni, p.initial_max_streams_uni}};
    for (const auto& limit : limits)
    {
        if (limit.value != 0U)
        {
            out.varint(limit.id, limit.value);
        }
    }
    if (p.ack_delay_exponent != kQuicDefaultAckDelayExponent)
    {
        out.varint(Id::AckDelayExponent, p.ack_delay_exponent);
    }
    if (p.max_ack_delay != kQuicDefaultMaxAckDelay)
    {
        out.varint(Id::MaxAckDelay, p.max_ack_delay);
    }
    if (p.disable_active_migration)
    {
        out.flag(static_cast<u64>(Id::DisableActiveMigration));
    }
    if (p.has_preferred_address)
    {
        out.preferred_address(p.preferred_address);
    }
    if (p.active_connection_id_limit != kQuicDefaultActiveIdLimit)
    {
        out.varint(Id::ActiveConnectionIdLimit, p.active_connection_id_limit);
    }
    if (p.has_initial_source_connection_id)
    {
        out.bytes(static_cast<u64>(Id::InitialSourceConnectionId), p.initial_source_connection_id.view());
    }
    if (p.has_retry_source_connection_id)
    {
        out.bytes(static_cast<u64>(Id::RetrySourceConnectionId), p.retry_source_connection_id.view());
    }
    if (p.has_version_information)
    {
        out.version_information(p.version_information, quic_grease_version(static_cast<u32>(grease.entropy >> 32U)));
    }
    if (p.max_datagram_frame_size != 0U)
    {
        out.varint(iana::kQuicTpMaxDatagramFrameSize, p.max_datagram_frame_size);
    }
    if (p.grease_quic_bit)
    {
        out.flag(iana::kQuicTpGreaseQuicBit);
    }
    if (p.reset_stream_at)
    {
        out.flag(iana::kQuicTpResetStreamAt);
    }

    // The reserved parameter: N below (2^62 - 1 - 27) / 31, so the identifier is a varint; its value is up to eight
    // bytes of the entropy itself, which no reader interprets.
    constexpr u64 grease_slots = (kQuicMaxVarint - kGreaseBase) / kGreaseStep + 1U;
    const u64 id = kGreaseStep * ((grease.entropy >> 4U) % grease_slots) + kGreaseBase;
    u8 value[kQuicMaxGreaseParameterBytes] = {};
    for (usize index = 0U; index < kQuicMaxGreaseParameterBytes; ++index)
    {
        value[index] = static_cast<u8>(grease.entropy >> (8U * index));
    }
    const usize value_bytes = static_cast<usize>(grease.entropy % (kQuicMaxGreaseParameterBytes + 1U));
    out.bytes(id, cont::ConstSpan<u8>(value, value_bytes));
}

// ---- Reading ----

bool read_exact_varint(cont::ConstSpan<u8> value, u64& out) noexcept
{
    cont::WireReader in(value);
    return take_quic_varint(in, out) && in.done();
}

bool read_preferred_address(cont::ConstSpan<u8> value, QuicPreferredAddress& out) noexcept
{
    cont::WireReader in(value);
    cont::ConstSpan<u8> ipv4;
    cont::ConstSpan<u8> ipv6;
    cont::ConstSpan<u8> id;
    cont::ConstSpan<u8> token;
    u8 id_bytes = 0U;
    QuicPreferredAddress address;
    if (!in.take_bytes(4U, ipv4) || !in.take_u16(address.ipv4_port) || !in.take_bytes(16U, ipv6) ||
        !in.take_u16(address.ipv6_port) || !in.take_u8(id_bytes) || id_bytes == 0U || !in.take_bytes(id_bytes, id) ||
        !in.take_bytes(kQuicResetTokenBytes, token) || !in.done() || !address.connection_id.assign(id))
    {
        return false; // section 18.2: a zero-length connection ID here is an error too
    }
    for (usize index = 0U; index < 4U; ++index)
    {
        address.ipv4[index] = ipv4[index];
    }
    for (usize index = 0U; index < 16U; ++index)
    {
        address.ipv6[index] = ipv6[index];
    }
    for (usize index = 0U; index < kQuicResetTokenBytes; ++index)
    {
        address.reset_token[index] = token[index];
    }
    out = address;
    return true;
}

bool read_version_information(cont::ConstSpan<u8> value, crypto::QuicSide side, QuicVersionInformation& out) noexcept
{
    // RFC 9368 section 4: too short, not a multiple of four, or a zero version is a parsing failure; so is a client's
    // chosen version missing from its own list.
    if (value.size() < 4U || value.size() % 4U != 0U || value.size() / 4U - 1U > kQuicMaxAvailableVersions)
    {
        return false;
    }
    cont::WireReader in(value);
    QuicVersionInformation information;
    if (!in.take_u32(information.chosen) || information.chosen == 0U)
    {
        return false;
    }
    while (!in.done())
    {
        u32 version = 0U;
        if (!in.take_u32(version) || version == 0U)
        {
            return false;
        }
        information.available[information.available_count++] = version;
    }
    if (side == crypto::QuicSide::Client && !contains(information.available_view(), information.chosen))
    {
        return false;
    }
    out = information;
    return true;
}

// The bit of `seen` for identifier `id`, or 64 for an identifier this module does not read.
u32 seen_bit(u64 id) noexcept
{
    if (id <= static_cast<u64>(Id::RetrySourceConnectionId))
    {
        return static_cast<u32>(id);
    }
    if (id == iana::kQuicTpMaxDatagramFrameSize)
    {
        return kSeenDatagram;
    }
    if (id == iana::kQuicTpGreaseQuicBit)
    {
        return kSeenGreaseBit;
    }
    if (id == iana::kQuicTpResetStreamAt)
    {
        return kSeenResetStreamAt;
    }
    if (id == iana::kQuicTpVersionInformation)
    {
        return kSeenVersionInfo;
    }
    return 64U;
}

bool server_only(u64 id) noexcept
{
    return id == static_cast<u64>(Id::OriginalDestinationConnectionId) ||
           id == static_cast<u64>(Id::StatelessResetToken) || id == static_cast<u64>(Id::PreferredAddress) ||
           id == static_cast<u64>(Id::RetrySourceConnectionId);
}

// Reads one parameter this module knows into `p`; false for a malformed value or one outside its bounds.
bool read_known(u64 id, cont::ConstSpan<u8> value, crypto::QuicSide side, QuicTransportParameters& p) noexcept
{
    if (id == iana::kQuicTpMaxDatagramFrameSize)
    {
        return read_exact_varint(value, p.max_datagram_frame_size);
    }
    if (id == iana::kQuicTpGreaseQuicBit)
    {
        p.grease_quic_bit = true;
        return value.empty(); // RFC 9287 section 3
    }
    if (id == iana::kQuicTpResetStreamAt)
    {
        p.reset_stream_at = true;
        return value.empty(); // section 3 of the draft
    }
    if (id == iana::kQuicTpVersionInformation)
    {
        p.has_version_information = true;
        return read_version_information(value, side, p.version_information);
    }
    switch (static_cast<Id>(id))
    {
    case Id::OriginalDestinationConnectionId:
        p.has_original_destination_connection_id = true;
        return p.original_destination_connection_id.assign(value);
    case Id::MaxIdleTimeout:
        return read_exact_varint(value, p.max_idle_timeout);
    case Id::StatelessResetToken:
        if (value.size() != kQuicResetTokenBytes)
        {
            return false;
        }
        p.has_stateless_reset_token = true;
        for (usize index = 0U; index < kQuicResetTokenBytes; ++index)
        {
            p.stateless_reset_token[index] = value[index];
        }
        return true;
    case Id::MaxUdpPayloadSize:
        return read_exact_varint(value, p.max_udp_payload_size) && p.max_udp_payload_size >= kQuicMinMaxUdpPayload;
    case Id::InitialMaxData:
        return read_exact_varint(value, p.initial_max_data);
    case Id::InitialMaxStreamDataBidiLocal:
        return read_exact_varint(value, p.initial_max_stream_data_bidi_local);
    case Id::InitialMaxStreamDataBidiRemote:
        return read_exact_varint(value, p.initial_max_stream_data_bidi_remote);
    case Id::InitialMaxStreamDataUni:
        return read_exact_varint(value, p.initial_max_stream_data_uni);
    case Id::InitialMaxStreamsBidi:
        return read_exact_varint(value, p.initial_max_streams_bidi) && p.initial_max_streams_bidi <= kQuicMaxStreams;
    case Id::InitialMaxStreamsUni:
        return read_exact_varint(value, p.initial_max_streams_uni) && p.initial_max_streams_uni <= kQuicMaxStreams;
    case Id::AckDelayExponent:
        return read_exact_varint(value, p.ack_delay_exponent) && p.ack_delay_exponent <= kQuicMaxAckDelayExponent;
    case Id::MaxAckDelay:
        return read_exact_varint(value, p.max_ack_delay) && p.max_ack_delay < kQuicMaxAckDelayLimit;
    case Id::DisableActiveMigration:
        p.disable_active_migration = true;
        return value.empty();
    case Id::PreferredAddress:
        p.has_preferred_address = true;
        return read_preferred_address(value, p.preferred_address);
    case Id::ActiveConnectionIdLimit:
        return read_exact_varint(value, p.active_connection_id_limit) &&
               p.active_connection_id_limit >= kQuicDefaultActiveIdLimit;
    case Id::InitialSourceConnectionId:
        p.has_initial_source_connection_id = true;
        return p.initial_source_connection_id.assign(value);
    case Id::RetrySourceConnectionId:
        p.has_retry_source_connection_id = true;
        return p.retry_source_connection_id.assign(value);
    }
    return false;
}
} // namespace

bool QuicConnectionId::assign(cont::ConstSpan<u8> source) noexcept
{
    if (source.size() > crypto::kQuicMaxConnectionIdBytes)
    {
        return false;
    }
    bytes = static_cast<u8>(source.size());
    for (usize index = 0U; index < crypto::kQuicMaxConnectionIdBytes; ++index)
    {
        id[index] = (index < source.size()) ? source[index] : u8{0U};
    }
    return true;
}

usize quic_transport_parameters_bytes(const QuicTransportParameters& parameters, crypto::QuicSide side,
                                      QuicTransportParameterGrease grease) noexcept
{
    if (!writable(parameters, side))
    {
        return 0U;
    }
    Emitter counter(nullptr);
    emit(parameters, grease, counter);
    return counter.total();
}

usize write_quic_transport_parameters(const QuicTransportParameters& parameters, crypto::QuicSide side,
                                      QuicTransportParameterGrease grease, cont::Span<u8> out) noexcept
{
    const usize bytes = quic_transport_parameters_bytes(parameters, side, grease);
    if (bytes == 0U || bytes > out.size())
    {
        return 0U;
    }
    cont::WireWriter writer(out.first(bytes));
    Emitter emitter(&writer);
    emit(parameters, grease, emitter);
    return bytes;
}

QuicTransportError read_quic_transport_parameters(cont::ConstSpan<u8> bytes, crypto::QuicSide side,
                                                  QuicTransportParameters& out) noexcept
{
    cont::WireReader in(bytes);
    QuicTransportParameters parameters;
    u64 seen = 0U;
    while (!in.done())
    {
        u64 id = 0U;
        cont::ConstSpan<u8> value;
        u64 length = 0U;
        if (!take_quic_varint(in, id) || !take_quic_varint(in, length) || length > in.rest().size() ||
            !in.take_bytes(static_cast<usize>(length), value))
        {
            return QuicTransportError::TransportParameterError;
        }
        const u32 bit = seen_bit(id);
        if (bit == 64U)
        {
            continue; // section 7.4.2: unknown and reserved parameters are ignored
        }
        // Section 7.4: a parameter sent twice; section 18.2: a server-only parameter from a client.
        if ((seen & (u64{1} << bit)) != 0U || (side == crypto::QuicSide::Client && server_only(id)) ||
            !read_known(id, value, side, parameters))
        {
            return QuicTransportError::TransportParameterError;
        }
        seen |= u64{1} << bit;
    }
    out = parameters;
    return QuicTransportError::NoError;
}

QuicTransportError quic_check_connection_ids(const QuicTransportParameters& parameters, crypto::QuicSide peer,
                                             const QuicConnectionIdExpectation& expected) noexcept
{
    const bool from_server = peer == crypto::QuicSide::Server;
    if (!parameters.has_initial_source_connection_id ||
        (from_server && !parameters.has_original_destination_connection_id) ||
        (from_server && parameters.has_preferred_address && expected.peer_source.empty()))
    {
        return QuicTransportError::TransportParameterError;
    }
    if (!equal_bytes(parameters.initial_source_connection_id.view(), expected.peer_source))
    {
        return QuicTransportError::ProtocolViolation;
    }
    if (!from_server)
    {
        return QuicTransportError::NoError;
    }
    if (!equal_bytes(parameters.original_destination_connection_id.view(), expected.original_destination) ||
        parameters.has_retry_source_connection_id != expected.retried ||
        (expected.retried && !equal_bytes(parameters.retry_source_connection_id.view(), expected.retry_source)))
    {
        return QuicTransportError::ProtocolViolation;
    }
    return QuicTransportError::NoError;
}

bool quic_versions_compatible(u32 from, u32 to) noexcept
{
    return from == to || (crypto::quic_version_supported(from) && crypto::quic_version_supported(to));
}

u32 quic_select_version(cont::ConstSpan<u32> preference, cont::ConstSpan<u32> offered) noexcept
{
    for (const u32 version : preference)
    {
        if (!quic_version_reserved(version) && version != 0U && contains(offered, version))
        {
            return version;
        }
    }
    return 0U;
}

u32 quic_server_select_version(const QuicTransportParameters& client, u32 original,
                               cont::ConstSpan<u32> preference) noexcept
{
    if (client.has_version_information)
    {
        for (const u32 version : preference)
        {
            if (!quic_version_reserved(version) && contains(client.version_information.available_view(), version) &&
                quic_versions_compatible(original, version))
            {
                return version;
            }
        }
    }
    return contains(preference, original) ? original : 0U;
}

usize write_quic_early_parameters(const QuicTransportParameters& server, cont::Span<u8> out) noexcept
{
    static constexpr u8 kFormat = 2U; // this encoding's revision: a ticket of another one meets another context
    if (out.size() < kQuicEarlyParameterBytes)
    {
        return 0U;
    }
    const u64 values[] = {server.initial_max_data,
                          server.initial_max_stream_data_bidi_local,
                          server.initial_max_stream_data_bidi_remote,
                          server.initial_max_stream_data_uni,
                          server.initial_max_streams_bidi,
                          server.initial_max_streams_uni,
                          server.active_connection_id_limit,
                          server.max_datagram_frame_size,
                          server.max_idle_timeout,
                          server.max_udp_payload_size,
                          server.disable_active_migration ? 1U : 0U,
                          server.reset_stream_at ? 1U : 0U};
    static_assert(1U + (sizeof(values) / sizeof(values[0])) * 8U == kQuicEarlyParameterBytes);
    cont::WireWriter writer(out.first(kQuicEarlyParameterBytes));
    writer.put_u8(kFormat);
    for (const u64 value : values)
    {
        writer.put_u64(value);
    }
    return writer.written();
}

QuicTransportError quic_check_remembered_parameters(const QuicTransportParameters& remembered,
                                                    const QuicTransportParameters& accepted) noexcept
{
    const bool kept = accepted.initial_max_data >= remembered.initial_max_data &&
                      accepted.initial_max_stream_data_bidi_local >= remembered.initial_max_stream_data_bidi_local &&
                      accepted.initial_max_stream_data_bidi_remote >= remembered.initial_max_stream_data_bidi_remote &&
                      accepted.initial_max_stream_data_uni >= remembered.initial_max_stream_data_uni &&
                      accepted.initial_max_streams_bidi >= remembered.initial_max_streams_bidi &&
                      accepted.initial_max_streams_uni >= remembered.initial_max_streams_uni &&
                      accepted.active_connection_id_limit >= remembered.active_connection_id_limit &&
                      accepted.max_datagram_frame_size >= remembered.max_datagram_frame_size &&
                      (accepted.reset_stream_at || !remembered.reset_stream_at);
    return kept ? QuicTransportError::NoError : QuicTransportError::ProtocolViolation;
}

QuicTransportError quic_server_check_version_information(const QuicTransportParameters& client, u32 original) noexcept
{
    if (client.has_version_information && client.version_information.chosen != original)
    {
        return QuicTransportError::VersionNegotiationError;
    }
    return QuicTransportError::NoError;
}

QuicTransportError quic_client_check_version_information(const QuicTransportParameters& server, u32 negotiated,
                                                         u32 attempted, bool after_version_negotiation,
                                                         cont::ConstSpan<u32> preference) noexcept
{
    QuicVersionInformation information = server.version_information;
    if (!server.has_version_information)
    {
        if (!after_version_negotiation)
        {
            return QuicTransportError::NoError;
        }
        if (negotiated != iana::kQuicVersion1)
        {
            return QuicTransportError::VersionNegotiationError; // section 4: never complete without it after VN
        }
        // Section 8: a version 1 server is read as having chosen and listed version 1 alone.
        information.chosen = iana::kQuicVersion1;
        information.available[0] = iana::kQuicVersion1;
        information.available_count = 1U;
    }
    if (information.chosen != negotiated)
    {
        return QuicTransportError::VersionNegotiationError;
    }
    if (!after_version_negotiation)
    {
        return QuicTransportError::NoError;
    }
    if (information.available_count == 0U)
    {
        return QuicTransportError::VersionNegotiationError;
    }
    // Section 4: the client would have attempted the same version had the Version Negotiation packet listed the
    // server's versions and the negotiated one.
    for (const u32 version : preference)
    {
        if (version == negotiated ||
            (!quic_version_reserved(version) && contains(information.available_view(), version)))
        {
            return (version == attempted) ? QuicTransportError::NoError : QuicTransportError::VersionNegotiationError;
        }
    }
    return QuicTransportError::VersionNegotiationError;
}

} // namespace crd::net
