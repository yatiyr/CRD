#pragma once

// What the ingress and I/O loop tests share (4.2a; docs/design/network-foundation.md#ingress): hex text, a counting
// allocator that can refuse, and datagrams built field by field from RFC 9000's layouts.

#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/quic_header.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>

namespace crd::net::testing
{
namespace cont = crd::containers;

// Lower- or upper-case hex into `out`; returns the bytes.
inline usize hex(const char* text, u8* out)
{
    const auto digit = [](char c) -> u8
    {
        if (c >= '0' && c <= '9')
        {
            return static_cast<u8>(c - '0');
        }
        if (c >= 'a' && c <= 'f')
        {
            return static_cast<u8>(c - 'a' + 10);
        }
        return static_cast<u8>(c - 'A' + 10);
    };
    const usize length = std::strlen(text);
    REQUIRE(length % 2U == 0U);
    for (usize i = 0U; i < length / 2U; ++i)
    {
        out[i] = static_cast<u8>((digit(text[2U * i]) << 4U) | digit(text[(2U * i) + 1U]));
    }
    return length / 2U;
}

inline bool same(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// Counts calls and refuses once `refuse_after` allocations have been made.
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    void* allocate(usize size, usize) override
    {
        if (m_calls >= m_refuse_after)
        {
            return nullptr;
        }
        ++m_calls;
        return std::malloc(size == 0U ? 1U : size);
    }

    void deallocate(void* pointer) noexcept override { std::free(pointer); }

    [[nodiscard]] bool owns(const void*) const noexcept override { return true; }

    [[nodiscard]] usize calls() const noexcept { return m_calls; }
    void refuse_after(usize calls) noexcept { m_refuse_after = calls; }

private:
    usize m_calls = 0U;
    usize m_refuse_after = ~usize{0};
};

inline constexpr usize kDatagramBytes = 1500U;

struct Datagram
{
    u8 bytes[kDatagramBytes] = {};
    usize size = 0U;

    [[nodiscard]] cont::ConstSpan<u8> view() const noexcept { return cont::ConstSpan<u8>(bytes, size); }
};

// The long packet type codes of RFC 9000 section 17.2 and RFC 9369 section 3.2.
inline u8 long_type_bits(u32 version, QuicPacketType type)
{
    const bool v2 = version == static_cast<u32>(crypto::QuicVersion::V2);
    switch (type)
    {
    case QuicPacketType::Initial:
        return v2 ? 1U : 0U;
    case QuicPacketType::ZeroRtt:
        return v2 ? 2U : 1U;
    case QuicPacketType::Handshake:
        return v2 ? 3U : 2U;
    case QuicPacketType::Retry:
        return v2 ? 0U : 3U;
    case QuicPacketType::VersionNegotiation:
    case QuicPacketType::OneRtt:
    case QuicPacketType::OtherVersion:
        break;
    }
    return 0U;
}

// A long header packet padded to `total` bytes (its Length covers the rest); a token only for an Initial.
inline Datagram long_packet(u32 version, QuicPacketType type, cont::ConstSpan<u8> dcid, cont::ConstSpan<u8> scid,
                            cont::ConstSpan<u8> token, usize total)
{
    Datagram out;
    cont::WireWriter writer(cont::Span<u8>(out.bytes, sizeof(out.bytes)));
    writer.put_u8(static_cast<u8>(0xC0U | (long_type_bits(version, type) << 4U)));
    writer.put_u32(version);
    writer.put_u8(static_cast<u8>(dcid.size()));
    writer.put_bytes(dcid);
    writer.put_u8(static_cast<u8>(scid.size()));
    writer.put_bytes(scid);
    if (type == QuicPacketType::Initial)
    {
        put_quic_varint(writer, token.size());
        writer.put_bytes(token);
    }
    const usize header = writer.written() + 2U; // a two-byte Length
    REQUIRE(total >= header + kQuicMinProtectedBytes);
    writer.put_u16(static_cast<u16>(0x4000U | (total - header)));
    while (writer.written() < total)
    {
        writer.put_u8(0xA5U);
    }
    out.size = total;
    return out;
}

inline Datagram initial(u32 version, cont::ConstSpan<u8> dcid, cont::ConstSpan<u8> scid,
                        cont::ConstSpan<u8> token = {}, usize total = kQuicMinInitialDatagram)
{
    return long_packet(version, QuicPacketType::Initial, dcid, scid, token, total);
}

inline Datagram short_packet(cont::ConstSpan<u8> dcid, usize total = 64U)
{
    Datagram out;
    out.bytes[0] = 0x41U;
    std::memcpy(out.bytes + 1U, dcid.data(), dcid.size());
    std::memset(out.bytes + 1U + dcid.size(), 0x5A, total - 1U - dcid.size());
    out.size = total;
    return out;
}

inline constexpr u32 kV1 = static_cast<u32>(crypto::QuicVersion::V1);
inline constexpr u32 kV2 = static_cast<u32>(crypto::QuicVersion::V2);

} // namespace crd::net::testing
