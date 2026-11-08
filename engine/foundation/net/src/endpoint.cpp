// crd-net -- IP addresses and endpoints (4.2a; docs/design/network-foundation.md#sockets).

#include <crd/net/endpoint.hpp>

#include <algorithm>

namespace crd::net
{
namespace
{

constexpr u8 kMappedPrefix[12] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0xffU, 0xffU};

[[nodiscard]] bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] int hex_value(char c) noexcept
{
    if (is_digit(c))
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

// A decimal number of at most `max_digits` digits and no leading zero, not above `limit`.
[[nodiscard]] bool parse_decimal(std::string_view text, usize max_digits, u64 limit, u64& out) noexcept
{
    if (text.empty() || text.size() > max_digits || (text.size() > 1U && text[0] == '0'))
    {
        return false;
    }
    u64 value = 0U;
    for (const char c : text)
    {
        if (!is_digit(c))
        {
            return false;
        }
        value = value * 10U + static_cast<u64>(c - '0');
    }
    if (value > limit)
    {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool parse_v4(std::string_view text, u8 (&out)[4]) noexcept
{
    usize part = 0U;
    usize begin = 0U;
    for (usize at = 0U; at <= text.size(); ++at)
    {
        if (at < text.size() && text[at] != '.')
        {
            continue;
        }
        u64 octet = 0U;
        if (part == 4U || !parse_decimal(text.substr(begin, at - begin), 3U, 255U, octet))
        {
            return false;
        }
        out[part++] = static_cast<u8>(octet);
        begin = at + 1U;
    }
    return part == 4U;
}

// RFC 4291 section 2.2: eight groups, one "::" standing for one or more zero groups, and a dotted IPv4 tail in place of
// the last two.
[[nodiscard]] bool parse_v6(std::string_view text, u8 (&out)[16]) noexcept
{
    u16 groups[8] = {};
    usize count = 0U;
    bool has_gap = false;
    usize gap = 0U; // the groups before "::"
    usize at = 0U;
    if (text.size() >= 2U && text[0] == ':' && text[1] == ':')
    {
        has_gap = true;
        at = 2U;
    }
    while (at < text.size())
    {
        usize end = at;
        bool dotted = false;
        while (end < text.size() && text[end] != ':')
        {
            dotted = dotted || text[end] == '.';
            ++end;
        }
        const std::string_view piece = text.substr(at, end - at);
        if (dotted)
        {
            u8 v4[4] = {};
            if (end != text.size() || count + 2U > 8U || !parse_v4(piece, v4))
            {
                return false;
            }
            groups[count++] = static_cast<u16>((v4[0] << 8U) | v4[1]);
            groups[count++] = static_cast<u16>((v4[2] << 8U) | v4[3]);
            at = end;
            break;
        }
        if (piece.empty() || piece.size() > 4U || count == 8U)
        {
            return false;
        }
        u32 value = 0U;
        for (const char c : piece)
        {
            const int digit = hex_value(c);
            if (digit < 0)
            {
                return false;
            }
            value = (value << 4U) | static_cast<u32>(digit);
        }
        groups[count++] = static_cast<u16>(value);
        at = end;
        if (at == text.size())
        {
            break;
        }
        ++at; // the ':'
        if (at < text.size() && text[at] == ':')
        {
            if (has_gap)
            {
                return false;
            }
            has_gap = true;
            gap = count;
            ++at;
        }
        else if (at == text.size())
        {
            return false; // a single trailing ':'
        }
    }
    if (has_gap ? count > 7U : count != 8U)
    {
        return false; // "::" stands for at least one group
    }
    u16 full[8] = {};
    const usize tail = has_gap ? count - gap : 0U;
    for (usize index = 0U; index < count; ++index)
    {
        const bool after_gap = has_gap && index >= gap;
        full[after_gap ? 8U - tail + (index - gap) : index] = groups[index];
    }
    for (usize index = 0U; index < 8U; ++index)
    {
        out[2U * index] = static_cast<u8>(full[index] >> 8U);
        out[2U * index + 1U] = static_cast<u8>(full[index]);
    }
    return true;
}

// Appends to a bounded buffer; `ok` turns false, and stays so, once the buffer is too short.
struct TextOut
{
    cont::Span<char> out;
    usize size = 0U;
    bool ok = true;

    void put(char c) noexcept
    {
        if (size < out.size())
        {
            out[size++] = c;
        }
        else
        {
            ok = false;
        }
    }

    void put_decimal(u64 value) noexcept
    {
        char digits[20];
        usize count = 0U;
        do
        {
            digits[count++] = static_cast<char>('0' + value % 10U);
            value /= 10U;
        } while (value != 0U);
        while (count != 0U)
        {
            put(digits[--count]);
        }
    }

    void put_hex(u32 value) noexcept
    {
        constexpr char digits[] = "0123456789abcdef";
        bool started = false;
        for (u32 shift = 12U;; shift -= 4U)
        {
            const u32 digit = (value >> shift) & 0xfU;
            started = started || digit != 0U || shift == 0U;
            if (started)
            {
                put(digits[digit]);
            }
            if (shift == 0U)
            {
                break;
            }
        }
    }

    void put_v4(const u8* bytes) noexcept
    {
        for (usize index = 0U; index < 4U; ++index)
        {
            if (index != 0U)
            {
                put('.');
            }
            put_decimal(bytes[index]);
        }
    }

    void put_address(const IpAddress& address) noexcept
    {
        if (address.family == AddressFamily::V4)
        {
            put_v4(address.bytes);
            return;
        }
        if (address.is_v4_mapped())
        {
            for (const char c : std::string_view("::ffff:"))
            {
                put(c);
            }
            put_v4(address.bytes + 12);
        }
        else
        {
            u32 groups[8];
            for (usize index = 0U; index < 8U; ++index)
            {
                groups[index] = (static_cast<u32>(address.bytes[2U * index]) << 8U) | address.bytes[2U * index + 1U];
            }
            // RFC 5952 section 4.2: the longest run of at least two zero groups, the first of equal runs.
            usize best_at = 8U;
            usize best_length = 1U;
            for (usize index = 0U; index < 8U;)
            {
                usize end = index;
                while (end < 8U && groups[end] == 0U)
                {
                    ++end;
                }
                if (end - index > best_length)
                {
                    best_at = index;
                    best_length = end - index;
                }
                index = (end == index) ? index + 1U : end;
            }
            for (usize index = 0U; index < 8U; ++index)
            {
                if (index == best_at)
                {
                    put(':');
                    put(':');
                    index += best_length - 1U;
                    continue;
                }
                if (index != 0U && index != best_at + best_length)
                {
                    put(':');
                }
                put_hex(groups[index]);
            }
        }
        if (address.scope != 0U)
        {
            put('%');
            put_decimal(address.scope);
        }
    }
};

} // namespace

bool IpAddress::is_unspecified() const noexcept
{
    return std::ranges::all_of(bytes, [](u8 byte) { return byte == 0U; });
}

bool IpAddress::is_loopback() const noexcept
{
    const IpAddress plain = unmapped();
    if (plain.family == AddressFamily::V4)
    {
        return plain.bytes[0] == 127U;
    }
    return plain == v6_loopback();
}

bool IpAddress::is_v4_mapped() const noexcept
{
    if (family != AddressFamily::V6)
    {
        return false;
    }
    for (usize index = 0U; index < 12U; ++index)
    {
        if (bytes[index] != kMappedPrefix[index])
        {
            return false;
        }
    }
    return true;
}

IpAddress IpAddress::unmapped() const noexcept
{
    if (!is_v4_mapped())
    {
        return *this;
    }
    return v4(bytes[12], bytes[13], bytes[14], bytes[15]);
}

IpAddress IpAddress::mapped() const noexcept
{
    if (family != AddressFamily::V4)
    {
        return *this;
    }
    u8 wide[16] = {};
    for (usize index = 0U; index < 12U; ++index)
    {
        wide[index] = kMappedPrefix[index];
    }
    for (usize index = 0U; index < 4U; ++index)
    {
        wide[12U + index] = bytes[index];
    }
    return v6(wide);
}

IpAddress IpAddress::prefix(u32 bits) const noexcept
{
    IpAddress kept = *this;
    kept.scope = 0U;
    const u32 width = (family == AddressFamily::V4) ? 32U : 128U;
    for (u32 index = 0U; index < 16U; ++index)
    {
        const u32 first = index * 8U;
        if (first >= width || first >= bits)
        {
            kept.bytes[index] = 0U;
        }
        else if (bits - first < 8U)
        {
            kept.bytes[index] = static_cast<u8>(kept.bytes[index] & (0xffU << (8U - (bits - first))));
        }
    }
    return kept;
}

bool parse_address(std::string_view text, IpAddress& out) noexcept
{
    if (text.find(':') == std::string_view::npos)
    {
        u8 v4[4] = {};
        if (!parse_v4(text, v4))
        {
            return false;
        }
        out = IpAddress::v4(v4[0], v4[1], v4[2], v4[3]);
        return true;
    }
    u64 scope = 0U;
    const usize percent = text.find('%');
    if (percent != std::string_view::npos)
    {
        if (!parse_decimal(text.substr(percent + 1U), 10U, 0xffffffffU, scope))
        {
            return false;
        }
        text = text.substr(0U, percent);
    }
    u8 v6[16] = {};
    if (!parse_v6(text, v6))
    {
        return false;
    }
    out = IpAddress::v6(v6, static_cast<u32>(scope));
    return true;
}

bool parse_endpoint(std::string_view text, Endpoint& out) noexcept
{
    std::string_view address_text;
    std::string_view port_text;
    if (!text.empty() && text[0] == '[')
    {
        const usize close = text.find(']');
        if (close == std::string_view::npos || close + 1U >= text.size() || text[close + 1U] != ':')
        {
            return false;
        }
        address_text = text.substr(1U, close - 1U);
        port_text = text.substr(close + 2U);
        if (address_text.find(':') == std::string_view::npos)
        {
            return false; // brackets hold an IPv6 address only
        }
    }
    else
    {
        const usize colon = text.find(':');
        if (colon == std::string_view::npos || text.find(':', colon + 1U) != std::string_view::npos)
        {
            return false;
        }
        address_text = text.substr(0U, colon);
        port_text = text.substr(colon + 1U);
    }
    Endpoint endpoint;
    u64 port = 0U;
    if (!parse_decimal(port_text, 5U, 65535U, port) || !parse_address(address_text, endpoint.address))
    {
        return false;
    }
    endpoint.port = static_cast<u16>(port);
    out = endpoint;
    return true;
}

usize format_address(const IpAddress& address, cont::Span<char> out) noexcept
{
    TextOut text{out};
    text.put_address(address);
    return text.ok ? text.size : 0U;
}

usize format_endpoint(const Endpoint& endpoint, cont::Span<char> out) noexcept
{
    TextOut text{out};
    const bool v6 = endpoint.address.family == AddressFamily::V6;
    if (v6)
    {
        text.put('[');
    }
    text.put_address(endpoint.address);
    if (v6)
    {
        text.put(']');
    }
    text.put(':');
    text.put_decimal(endpoint.port);
    return text.ok ? text.size : 0U;
}

} // namespace crd::net
