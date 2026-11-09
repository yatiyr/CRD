// crd-net -- what makes an HTTP message well formed (4.2a; docs/design/network-foundation.md#http3).

#include <crd/net/http_message.hpp>

#include <algorithm>
#include <cstring>

namespace crd::net
{
namespace
{
constexpr u64 kMaxContentLength = (u64{1} << 62U) - 1U;

template <usize N> [[nodiscard]] bool same(cont::ConstSpan<u8> text, const char (&literal)[N]) noexcept
{
    return text.size() == N - 1U && std::memcmp(text.data(), literal, N - 1U) == 0;
}

[[nodiscard]] u8 lower(u8 c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<u8>(c + ('a' - 'A')) : c;
}

[[nodiscard]] bool same_ignoring_case(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (usize at = 0U; at < a.size(); ++at)
    {
        if (lower(a[at]) != lower(b[at]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_alpha(u8 c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] bool is_digit(u8 c) noexcept
{
    return c >= '0' && c <= '9';
}

// RFC 9110 section 5.6.2's tchar.
[[nodiscard]] bool is_token_char(u8 c) noexcept
{
    if (is_alpha(c) || is_digit(c))
    {
        return true;
    }
    switch (c)
    {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool is_token(cont::ConstSpan<u8> text) noexcept
{
    if (text.empty())
    {
        return false;
    }
    return std::ranges::all_of(text, is_token_char);
}

// A lower-case token, from `from` on (a pseudo-field's colon is not one).
[[nodiscard]] bool name_valid(cont::ConstSpan<u8> name, usize from) noexcept
{
    if (name.size() <= from)
    {
        return false;
    }
    for (usize at = from; at < name.size(); ++at)
    {
        if (!is_token_char(name[at]) || (name[at] >= 'A' && name[at] <= 'Z'))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool value_valid(cont::ConstSpan<u8> value) noexcept
{
    if (!value.empty())
    {
        const u8 first = value[0];
        const u8 last = value[value.size() - 1U];
        if (first == ' ' || first == '\t' || last == ' ' || last == '\t')
        {
            return false;
        }
    }
    return std::ranges::none_of(value, [](u8 c) { return c == 0U || c == '\r' || c == '\n'; });
}

// Visible bytes and those above ASCII: neither a space nor a control can hide in a request target.
[[nodiscard]] bool target_valid(cont::ConstSpan<u8> text) noexcept
{
    return std::ranges::none_of(text, [](u8 c) { return c <= 0x20U || c == 0x7FU; });
}

[[nodiscard]] bool scheme_valid(cont::ConstSpan<u8> scheme) noexcept
{
    if (scheme.empty() || !is_alpha(scheme[0]))
    {
        return false;
    }
    return std::ranges::all_of(scheme,
                               [](u8 c) { return is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.'; });
}

// One decimal number, none longer than the largest length a stream can carry.
[[nodiscard]] bool parse_length(cont::ConstSpan<u8> value, u64& length) noexcept
{
    if (value.empty())
    {
        return false;
    }
    u64 total = 0U;
    for (const u8 c : value)
    {
        if (!is_digit(c) || total > (kMaxContentLength - (c - u64{'0'})) / 10U)
        {
            return false;
        }
        total = (total * 10U) + (c - u64{'0'});
    }
    length = total;
    return true;
}

// What a regular field adds to a message; `host` is read by a request only.
[[nodiscard]] bool regular_field(const HttpField& field, HttpMessageInfo& info, cont::ConstSpan<u8>& host,
                                 bool& has_host) noexcept
{
    if (!name_valid(field.name, 0U) || !value_valid(field.value))
    {
        return false;
    }
    if (same(field.name, "connection") || same(field.name, "keep-alive") || same(field.name, "proxy-connection") ||
        same(field.name, "transfer-encoding") || same(field.name, "upgrade"))
    {
        return false;
    }
    if (same(field.name, "te"))
    {
        return same(field.value, "trailers");
    }
    if (same(field.name, "content-length"))
    {
        u64 length = 0U;
        if (!parse_length(field.value, length) || (info.has_content_length && info.content_length != length))
        {
            return false;
        }
        info.has_content_length = true;
        info.content_length = length;
        return true;
    }
    if (same(field.name, "host"))
    {
        if (has_host)
        {
            return false;
        }
        has_host = true;
        host = field.value;
    }
    return true;
}

// The one field `seen` flag of a pseudo-field, set; false when it was set before.
[[nodiscard]] bool first_time(u32& seen, u32 bit) noexcept
{
    const bool first = (seen & bit) == 0U;
    seen |= bit;
    return first;
}

constexpr u32 kSeenMethod = 1U << 0U;
constexpr u32 kSeenScheme = 1U << 1U;
constexpr u32 kSeenAuthority = 1U << 2U;
constexpr u32 kSeenPath = 1U << 3U;
constexpr u32 kSeenProtocol = 1U << 4U;

[[nodiscard]] bool pseudo_field(const HttpField& field, HttpMessageInfo& info, u32& seen) noexcept
{
    if (!name_valid(field.name, 1U) || !value_valid(field.value))
    {
        return false;
    }
    if (same(field.name, ":method"))
    {
        info.method = field.value;
        return first_time(seen, kSeenMethod) && is_token(field.value);
    }
    if (same(field.name, ":scheme"))
    {
        info.scheme = field.value;
        return first_time(seen, kSeenScheme) && scheme_valid(field.value);
    }
    if (same(field.name, ":authority"))
    {
        info.authority = field.value;
        if (!first_time(seen, kSeenAuthority) || !target_valid(field.value))
        {
            return false;
        }
        return std::ranges::none_of(field.value, [](u8 c) { return c == '@'; }); // no userinfo (RFC 9114 section 4.3.1)
    }
    if (same(field.name, ":path"))
    {
        info.path = field.value;
        return first_time(seen, kSeenPath) && target_valid(field.value);
    }
    if (same(field.name, ":protocol"))
    {
        info.protocol = field.value;
        return first_time(seen, kSeenProtocol) && is_token(field.value);
    }
    return false;
}

// :path of an http or https request: the origin form, or * for OPTIONS.
[[nodiscard]] bool http_path_valid(const HttpMessageInfo& info) noexcept
{
    if (info.path.empty())
    {
        return false;
    }
    if (info.path[0] == '/')
    {
        return true;
    }
    return same(info.path, "*") && same(info.method, "OPTIONS");
}
} // namespace

bool http_check_request(cont::ConstSpan<HttpField> fields, HttpMessageInfo& info) noexcept
{
    info = HttpMessageInfo{};
    u32 seen = 0U;
    bool regular = false;
    bool has_host = false;
    cont::ConstSpan<u8> host;
    for (const HttpField& field : fields)
    {
        if (!field.name.empty() && field.name[0] == ':')
        {
            if (regular || !pseudo_field(field, info, seen))
            {
                return false;
            }
        }
        else
        {
            regular = true;
            if (!regular_field(field, info, host, has_host))
            {
                return false;
            }
        }
    }
    if ((seen & kSeenMethod) == 0U)
    {
        return false;
    }
    info.connect = same(info.method, "CONNECT");
    info.head = same(info.method, "HEAD");
    info.extended = (seen & kSeenProtocol) != 0U;
    if (info.extended && !info.connect)
    {
        return false;
    }
    if (info.connect && !info.extended)
    {
        // RFC 9114 section 4.4: :authority alone.
        return (seen & (kSeenScheme | kSeenPath)) == 0U && (seen & kSeenAuthority) != 0U && !info.authority.empty();
    }
    if ((seen & kSeenScheme) == 0U || (seen & kSeenPath) == 0U)
    {
        return false;
    }
    if (same(info.scheme, "http") || same(info.scheme, "https"))
    {
        if (!http_path_valid(info))
        {
            return false;
        }
        const bool has_authority = (seen & kSeenAuthority) != 0U;
        if (!has_authority && !has_host)
        {
            return false;
        }
        if ((has_authority && info.authority.empty()) || (has_host && host.empty()))
        {
            return false;
        }
        if (has_authority && has_host && !same_ignoring_case(info.authority, host))
        {
            return false;
        }
    }
    return !info.extended || (seen & kSeenAuthority) != 0U;
}

bool http_check_response(cont::ConstSpan<HttpField> fields, HttpMessageInfo& info) noexcept
{
    info = HttpMessageInfo{};
    bool has_status = false;
    bool regular = false;
    bool has_host = false;
    cont::ConstSpan<u8> host;
    for (const HttpField& field : fields)
    {
        if (!field.name.empty() && field.name[0] == ':')
        {
            if (regular || has_status || !same(field.name, ":status") || field.value.size() != 3U)
            {
                return false;
            }
            if (!is_digit(field.value[0]) || !is_digit(field.value[1]) || !is_digit(field.value[2]) ||
                field.value[0] == '0')
            {
                return false;
            }
            has_status = true;
            info.status = static_cast<u16>(((field.value[0] - u32{'0'}) * 100U) + ((field.value[1] - u32{'0'}) * 10U) +
                                           (field.value[2] - u32{'0'}));
        }
        else
        {
            regular = true;
            if (!regular_field(field, info, host, has_host))
            {
                return false;
            }
        }
    }
    return has_status && info.status != 101U; // no Upgrade on these versions (RFC 9114 section 4.5)
}

bool http_check_trailers(cont::ConstSpan<HttpField> fields) noexcept
{
    HttpMessageInfo info;
    bool has_host = false;
    cont::ConstSpan<u8> host;
    for (const HttpField& field : fields)
    {
        if (!regular_field(field, info, host, has_host))
        {
            return false; // a pseudo-field's colon is no token either
        }
    }
    return true;
}

u64 http_section_size(cont::ConstSpan<HttpField> fields) noexcept
{
    u64 size = 0U;
    for (const HttpField& field : fields)
    {
        size += field.name.size() + field.value.size() + kHttpFieldOverhead;
    }
    return size;
}

} // namespace crd::net
