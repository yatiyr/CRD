#pragma once

// crd-net -- IP addresses and endpoints (4.2a; contract: docs/design/network-foundation.md#sockets). Plain values with
// one representation each, so two equal endpoints compare equal and NET-SIM, logs and captures print one text for
// them. Text follows RFC 4291 section 2.2 on input and RFC 5952 on output; no host call is made, so every platform
// reads and prints alike.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <string_view>

namespace crd::net
{
namespace cont = crd::containers;

enum class AddressFamily : u8
{
    V4,
    V6,
};

// An IPv4 address holds its four bytes first and zero in the other twelve; the scope is an IPv6 zone index
// (RFC 4007) and zero for every other address. Both rules keep one representation per address.
struct IpAddress
{
    AddressFamily family = AddressFamily::V4;
    u8 bytes[16] = {}; // network order
    u32 scope = 0U;

    [[nodiscard]] static constexpr IpAddress v4(u8 a, u8 b, u8 c, u8 d) noexcept
    {
        IpAddress address;
        address.bytes[0] = a;
        address.bytes[1] = b;
        address.bytes[2] = c;
        address.bytes[3] = d;
        return address;
    }

    [[nodiscard]] static constexpr IpAddress v6(const u8 (&bytes)[16], u32 scope = 0U) noexcept
    {
        IpAddress address;
        address.family = AddressFamily::V6;
        for (usize index = 0U; index < 16U; ++index)
        {
            address.bytes[index] = bytes[index];
        }
        address.scope = scope;
        return address;
    }

    [[nodiscard]] static constexpr IpAddress v4_any() noexcept
    {
        return IpAddress{};
    }

    [[nodiscard]] static constexpr IpAddress v4_loopback() noexcept
    {
        return v4(127U, 0U, 0U, 1U);
    }

    [[nodiscard]] static constexpr IpAddress v6_any() noexcept
    {
        IpAddress address;
        address.family = AddressFamily::V6;
        return address;
    }

    [[nodiscard]] static constexpr IpAddress v6_loopback() noexcept
    {
        IpAddress address = v6_any();
        address.bytes[15] = 1U;
        return address;
    }

    // 0.0.0.0 or ::
    [[nodiscard]] bool is_unspecified() const noexcept;
    // 127.0.0.0/8, ::1, or an IPv4-mapped loopback address
    [[nodiscard]] bool is_loopback() const noexcept;
    // ::ffff:0:0/96 (RFC 4291 section 2.5.5.2)
    [[nodiscard]] bool is_v4_mapped() const noexcept;
    // The IPv4 address an IPv4-mapped one carries; any other address unchanged.
    [[nodiscard]] IpAddress unmapped() const noexcept;
    // The IPv4-mapped form of an IPv4 address; an IPv6 address unchanged.
    [[nodiscard]] IpAddress mapped() const noexcept;
    // The first `bits` bits kept and the rest and the scope cleared: the key of admission S3's per-prefix buckets
    // (/64 for IPv6; /32 or a configured prefix for IPv4). `bits` past the family's width keeps every bit.
    [[nodiscard]] IpAddress prefix(u32 bits) const noexcept;
    // The address's own bytes: four for IPv4, sixteen for IPv6 (what a QUIC address validation token binds).
    [[nodiscard]] cont::ConstSpan<u8> view() const noexcept
    {
        return cont::ConstSpan<u8>(bytes, (family == AddressFamily::V4) ? 4U : 16U);
    }

    friend bool operator==(const IpAddress&, const IpAddress&) noexcept = default;
};

struct Endpoint
{
    IpAddress address;
    u16 port = 0U;

    friend bool operator==(const Endpoint&, const Endpoint&) noexcept = default;
};

// The longest texts format_address() and format_endpoint() write: eight four-digit groups with a ten-digit zone, then
// brackets and a five-digit port.
inline constexpr usize kMaxAddressText = 50U;  // "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295"
inline constexpr usize kMaxEndpointText = 58U; // "[" address "]:65535"

// Reads "a.b.c.d" or any RFC 4291 form of an IPv6 address (with "::", an embedded dotted IPv4 tail, upper or lower
// case hex) and an optional numeric zone "%n". Refused: anything else, including IPv4 octets with leading zeros
// (which some readers take as octal), more than four hex digits in a group, two "::" and a zone on an IPv4 address.
[[nodiscard]] bool parse_address(std::string_view text, IpAddress& out) noexcept;

// Reads "a.b.c.d:port" or "[ipv6]:port"; an IPv6 address must be bracketed. The port is decimal, 0 to 65535, without
// leading zeros.
[[nodiscard]] bool parse_endpoint(std::string_view text, Endpoint& out) noexcept;

// Writes RFC 5952's text (lower case, the longest run of two or more zero groups compressed, the first of equal
// runs, an IPv4-mapped address with a dotted tail) and returns its length; zero when `out` is shorter than the text.
[[nodiscard]] usize format_address(const IpAddress& address, cont::Span<char> out) noexcept;
[[nodiscard]] usize format_endpoint(const Endpoint& endpoint, cont::Span<char> out) noexcept;

} // namespace crd::net
