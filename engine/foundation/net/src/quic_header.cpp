// crd-net -- QUIC headers for stage S1, Version Negotiation and Retry (4.2a; docs/design/network-foundation.md#ingress).

#include <crd/net/quic_header.hpp>

namespace crd::net
{
namespace
{
constexpr u8 kLongForm = 0x80U;
constexpr u8 kFixedBit = 0x40U;

// The long packet type of `bits` (the first byte's bits 4 and 5) in a version 1 or 2 header: version 2 rotates the
// codes by one (RFC 9369 section 3.2).
QuicPacketType long_type(u32 version, u8 bits) noexcept
{
    constexpr QuicPacketType v1_types[4] = {QuicPacketType::Initial, QuicPacketType::ZeroRtt, QuicPacketType::Handshake,
                                       QuicPacketType::Retry};
    constexpr QuicPacketType v2_types[4] = {QuicPacketType::Retry, QuicPacketType::Initial, QuicPacketType::ZeroRtt,
                                       QuicPacketType::Handshake};
    return (version == static_cast<u32>(crypto::QuicVersion::V2)) ? v2_types[bits & 3U] : v1_types[bits & 3U];
}

u8 retry_type_bits(crypto::QuicVersion version) noexcept
{
    return (version == crypto::QuicVersion::V2) ? 0U : 3U;
}

bool take_connection_id(cont::WireReader& in, usize limit, cont::ConstSpan<u8>& out, QuicHeaderError& error) noexcept
{
    u8 length = 0U;
    if (!in.take_u8(length))
    {
        error = QuicHeaderError::Truncated;
        return false;
    }
    if (length > limit)
    {
        error = QuicHeaderError::ConnectionIdTooLong;
        return false;
    }
    if (!in.take_bytes(length, out))
    {
        error = QuicHeaderError::Truncated;
        return false;
    }
    return true;
}

bool spans_equal(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
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
} // namespace

usize quic_varint_bytes(u64 value) noexcept
{
    if (value < (u64{1} << 6U))
    {
        return 1U;
    }
    if (value < (u64{1} << 14U))
    {
        return 2U;
    }
    if (value < (u64{1} << 30U))
    {
        return 4U;
    }
    return (value <= kQuicMaxVarint) ? 8U : 0U;
}

void put_quic_varint(cont::WireWriter& out, u64 value) noexcept
{
    switch (quic_varint_bytes(value))
    {
    case 1U:
        out.put_u8(static_cast<u8>(value));
        break;
    case 2U:
        out.put_u16(static_cast<u16>(value | 0x4000U));
        break;
    case 4U:
        out.put_u32(static_cast<u32>(value | 0x80000000U));
        break;
    case 8U:
        out.put_u64(value | 0xC000000000000000ULL);
        break;
    default:
        CRD_FATAL("put_quic_varint: a value above 2^62 - 1");
    }
}

bool take_quic_varint(cont::WireReader& in, u64& value) noexcept
{
    const cont::ConstSpan<u8> rest = in.rest();
    if (rest.empty())
    {
        return false;
    }
    const usize bytes = usize{1} << (rest[0] >> 6U);
    cont::ConstSpan<u8> field;
    if (!in.take_bytes(bytes, field))
    {
        return false;
    }
    u64 result = field[0] & 0x3FU;
    for (usize index = 1U; index < bytes; ++index)
    {
        result = (result << 8U) | field[index];
    }
    value = result;
    return true;
}

QuicHeaderError parse_quic_header(cont::ConstSpan<u8> datagram, usize short_dcid_bytes, QuicHeader& out) noexcept
{
    if (datagram.empty())
    {
        return QuicHeaderError::Empty;
    }
    QuicHeader header;
    header.first = datagram[0];
    cont::WireReader in(datagram.subspan(1U));
    QuicHeaderError error = QuicHeaderError::None;

    if ((header.first & kLongForm) == 0U)
    {
        // The short header: the fixed bit is not checked, since a peer that saw grease_quic_bit may clear it (RFC
        // 9287); the packet layer checks it for a peer that did not.
        header.type = QuicPacketType::OneRtt;
        if (!in.take_bytes(short_dcid_bytes, header.dcid))
        {
            return QuicHeaderError::Truncated;
        }
        header.number_offset = 1U + short_dcid_bytes;
        if (datagram.size() - header.number_offset < kQuicMinProtectedBytes)
        {
            return QuicHeaderError::TooShortToProtect;
        }
        header.packet_bytes = datagram.size();
        out = header;
        return QuicHeaderError::None;
    }

    if (!in.take_u32(header.version))
    {
        return QuicHeaderError::Truncated;
    }
    const bool readable = crypto::quic_version_supported(header.version);
    const usize id_limit = readable ? crypto::kQuicMaxConnectionIdBytes : kQuicMaxInvariantConnectionIdBytes;
    if (!take_connection_id(in, id_limit, header.dcid, error) || !take_connection_id(in, id_limit, header.scid, error))
    {
        return error;
    }
    if (!readable)
    {
        header.type = (header.version == kQuicVersionNegotiation) ? QuicPacketType::VersionNegotiation
                                                                  : QuicPacketType::OtherVersion;
        header.packet_bytes = datagram.size();
        out = header;
        return QuicHeaderError::None;
    }

    header.type = long_type(header.version, static_cast<u8>(header.first >> 4U));
    if (header.type == QuicPacketType::Retry)
    {
        header.packet_bytes = datagram.size();
        out = header;
        return QuicHeaderError::None;
    }
    if (header.type == QuicPacketType::Initial)
    {
        u64 token_bytes = 0U;
        if (!take_quic_varint(in, token_bytes) || token_bytes > in.rest().size() ||
            !in.take_bytes(static_cast<usize>(token_bytes), header.token))
        {
            return QuicHeaderError::Truncated;
        }
    }
    u64 length = 0U;
    if (!take_quic_varint(in, length))
    {
        return QuicHeaderError::Truncated;
    }
    header.number_offset = 1U + in.consumed();
    if (length > datagram.size() - header.number_offset)
    {
        return QuicHeaderError::LengthPastDatagram;
    }
    if (length < kQuicMinProtectedBytes)
    {
        return QuicHeaderError::TooShortToProtect;
    }
    header.packet_bytes = header.number_offset + static_cast<usize>(length);
    out = header;
    return QuicHeaderError::None;
}

usize write_version_negotiation(cont::ConstSpan<u8> dcid, cont::ConstSpan<u8> scid, cont::ConstSpan<u32> versions,
                                u64 entropy, cont::Span<u8> out) noexcept
{
    if (versions.empty() || versions.size() > kQuicMaxNegotiationVersions ||
        dcid.size() > kQuicMaxInvariantConnectionIdBytes || scid.size() > kQuicMaxInvariantConnectionIdBytes)
    {
        return 0U;
    }
    for (const u32 version : versions)
    {
        if (quic_version_reserved(version) || version == kQuicVersionNegotiation)
        {
            return 0U;
        }
    }
    const usize count = versions.size() + 1U;
    const usize size = 1U + 4U + 1U + scid.size() + 1U + dcid.size() + 4U * count;
    if (out.size() < size)
    {
        return 0U;
    }
    const u32 grease = quic_grease_version(static_cast<u32>(entropy >> 8U));
    const usize grease_at = static_cast<usize>((entropy >> 40U) % count);

    cont::WireWriter writer(out);
    writer.put_u8(static_cast<u8>(kLongForm | kFixedBit | (entropy & 0x3FU)));
    writer.put_u32(kQuicVersionNegotiation);
    writer.put_u8(static_cast<u8>(scid.size()));
    writer.put_bytes(scid);
    writer.put_u8(static_cast<u8>(dcid.size()));
    writer.put_bytes(dcid);
    usize listed = 0U;
    for (usize index = 0U; index < count; ++index)
    {
        writer.put_u32((index == grease_at) ? grease : versions[listed++]);
    }
    return writer.written();
}

crypto::CryptoError write_retry(crypto::QuicVersion version, cont::ConstSpan<u8> client_scid,
                                cont::ConstSpan<u8> retry_scid, cont::ConstSpan<u8> original_dcid,
                                cont::ConstSpan<u8> token, u8 entropy, cont::Span<u8> out, usize& written) noexcept
{
    if (!crypto::quic_version_supported(static_cast<u32>(version)))
    {
        return crypto::CryptoError::Unsupported;
    }
    if (client_scid.size() > crypto::kQuicMaxConnectionIdBytes || retry_scid.empty() ||
        retry_scid.size() > crypto::kQuicMaxConnectionIdBytes ||
        original_dcid.size() > crypto::kQuicMaxConnectionIdBytes)
    {
        return crypto::CryptoError::BadLength;
    }
    if (spans_equal(retry_scid, original_dcid))
    {
        return crypto::CryptoError::Overlap;
    }
    const usize size = quic_retry_bytes(client_scid.size(), retry_scid.size(), token.size());
    if (size > crypto::kQuicMaxRetryBytes || out.size() < size)
    {
        return crypto::CryptoError::BadLength;
    }
    cont::WireWriter writer(out);
    writer.put_u8(static_cast<u8>(kLongForm | kFixedBit | (retry_type_bits(version) << 4U) | (entropy & 0x0FU)));
    writer.put_u32(static_cast<u32>(version));
    writer.put_u8(static_cast<u8>(client_scid.size()));
    writer.put_bytes(client_scid);
    writer.put_u8(static_cast<u8>(retry_scid.size()));
    writer.put_bytes(retry_scid);
    writer.put_bytes(token);
    const usize body = writer.written();
    const crypto::CryptoError error =
        crypto::quic_retry_tag(version, original_dcid, out.first(body), out.subspan(body, crypto::kQuicTagBytes));
    if (error != crypto::CryptoError::None)
    {
        return error;
    }
    written = size;
    return crypto::CryptoError::None;
}

} // namespace crd::net
