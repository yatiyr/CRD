// crd-net -- the WebSocket protocol's pieces (4.2a; docs/design/network-foundation.md#websocket). Sources: RFC 6455,
// RFC 9110 and RFC 9112.

#include <crd/containers/wire_bytes.hpp>
#include <crd/core/utf8.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/net/http_message.hpp>
#include <crd/net/websocket.hpp>

#include <algorithm>
#include <cstring>

namespace crd::net
{
namespace
{
constexpr usize kExtended16 = 126U;
constexpr usize kExtended64 = 127U;
constexpr usize kVersionBytes = 8U; // HTTP/1.1
constexpr u8 kFin = 0x80U;
constexpr u8 kReserved = 0x70U;
constexpr u8 kMaskBit = 0x80U;
constexpr u8 kLengthMask = 0x7FU;
constexpr u8 kOpcodeMask = 0x0FU;
constexpr u8 kFirstControl = 0x8U;
constexpr u16 kStatusUpgrade = 101U;
constexpr u16 kStatusBad = 400U;
constexpr u16 kStatusForbidden = 403U;
constexpr u16 kStatusNotFound = 404U;
constexpr u16 kStatusMethod = 405U;
constexpr u16 kStatusUpgradeRequired = 426U;
constexpr u16 kStatusFieldsTooLarge = 431U;
constexpr u16 kStatusVersion = 505U;
constexpr u32 kSeenHost = 1U << 0U;
constexpr u32 kSeenKey = 1U << 1U;
constexpr u32 kSeenOrigin = 1U << 2U;
constexpr u32 kSeenAccept = 1U << 3U;
constexpr u32 kSeenLength = 1U << 4U;
constexpr u32 kSeenProtocol = 1U << 5U;
constexpr u32 kSeenUpgrade = 1U << 6U;
constexpr u32 kSeenConnection = 1U << 7U;
constexpr u32 kSeenVersion = 1U << 8U;
constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

template <usize N> [[nodiscard]] cont::ConstSpan<u8> text(const char (&literal)[N]) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(literal), N - 1U);
}

[[nodiscard]] bool named(cont::ConstSpan<u8> name, cont::ConstSpan<u8> known) noexcept
{
    return http_same_ignoring_case(name, known);
}

[[nodiscard]] bool first_time(u32& seen, u32 bit) noexcept
{
    const bool first = (seen & bit) == 0U;
    seen |= bit;
    return first;
}

[[nodiscard]] bool whitespace(u8 c) noexcept
{
    return c == ' ' || c == '\t';
}

[[nodiscard]] cont::ConstSpan<u8> trimmed(cont::ConstSpan<u8> value) noexcept
{
    usize from = 0U;
    usize to = value.size();
    while (from < to && whitespace(value[from]))
    {
        ++from;
    }
    while (to > from && whitespace(value[to - 1U]))
    {
        --to;
    }
    return value.subspan(from, to - from);
}

// RFC 9110 section 5.5: visible bytes, those above ASCII, and spaces and tabs inside.
[[nodiscard]] bool value_bytes_valid(cont::ConstSpan<u8> value) noexcept
{
    return std::ranges::all_of(value, [](u8 c) { return (c >= 0x20U || c == '\t') && c != 0x7FU; });
}

// The offset just past the head's empty line, or 0 while it has not ended.
[[nodiscard]] usize head_end(cont::ConstSpan<u8> bytes, usize& scanned) noexcept
{
    usize at = (scanned > 3U) ? scanned - 3U : 0U;
    for (; at + 4U <= bytes.size(); ++at)
    {
        if (bytes[at] == '\r' && bytes[at + 1U] == '\n' && bytes[at + 2U] == '\r' && bytes[at + 3U] == '\n')
        {
            return at + 4U;
        }
    }
    scanned = bytes.size();
    return 0U;
}

// One line of a head, its CRLF consumed. False for a CR with no LF, or for a head without one.
[[nodiscard]] bool next_line(cont::ConstSpan<u8> head, usize& at, cont::ConstSpan<u8>& line) noexcept
{
    usize end = at;
    while (end < head.size() && head[end] != '\r')
    {
        ++end;
    }
    if (end + 1U >= head.size() || head[end + 1U] != '\n')
    {
        return false;
    }
    line = head.subspan(at, end - at);
    at = end + 2U;
    return true;
}

// The next field line. RFC 9112 section 5: a folded line starts with a space and a space before the colon is in the
// name, so neither is a token; section 5.1 has both refused.
[[nodiscard]] bool next_field(cont::ConstSpan<u8> head, usize& at, cont::ConstSpan<u8>& name,
                              cont::ConstSpan<u8>& value) noexcept
{
    cont::ConstSpan<u8> line;
    if (!next_line(head, at, line))
    {
        return false;
    }
    usize colon = 0U;
    while (colon < line.size() && line[colon] != ':')
    {
        ++colon;
    }
    if (colon == line.size())
    {
        return false;
    }
    name = line.first(colon);
    value = trimmed(line.subspan(colon + 1U));
    return http_is_token(name) && value_bytes_valid(value);
}

// Calls `visit` with each trimmed element of a comma-separated list until it returns true.
template <typename Visit> [[nodiscard]] bool list_any(cont::ConstSpan<u8> value, const Visit& visit) noexcept
{
    usize from = 0U;
    while (from <= value.size())
    {
        usize to = from;
        while (to < value.size() && value[to] != ',')
        {
            ++to;
        }
        if (visit(trimmed(value.subspan(from, to - from))))
        {
            return true;
        }
        from = to + 1U;
    }
    return false;
}

[[nodiscard]] bool list_has(cont::ConstSpan<u8> value, cont::ConstSpan<u8> token) noexcept
{
    return list_any(value, [token](cont::ConstSpan<u8> element) { return named(element, token); });
}

[[nodiscard]] char base64_at(u32 group, u32 shift) noexcept
{
    return kBase64[(group >> shift) & 0x3FU];
}

// Base64 of `bytes` into `out`, which is 4 bytes per 3 and rounded up.
void base64(cont::ConstSpan<u8> bytes, cont::Span<u8> out) noexcept
{
    usize to = 0U;
    usize at = 0U;
    for (; at + 3U <= bytes.size(); at += 3U)
    {
        const u32 group = (u32{bytes[at]} << 16U) | (u32{bytes[at + 1U]} << 8U) | u32{bytes[at + 2U]};
        out[to++] = static_cast<u8>(base64_at(group, 18U));
        out[to++] = static_cast<u8>(base64_at(group, 12U));
        out[to++] = static_cast<u8>(base64_at(group, 6U));
        out[to++] = static_cast<u8>(base64_at(group, 0U));
    }
    const usize left = bytes.size() - at;
    if (left != 0U)
    {
        u32 group = u32{bytes[at]} << 16U;
        group |= (left == 2U) ? u32{bytes[at + 1U]} << 8U : 0U;
        out[to++] = static_cast<u8>(base64_at(group, 18U));
        out[to++] = static_cast<u8>(base64_at(group, 12U));
        out[to++] = (left == 2U) ? static_cast<u8>(base64_at(group, 6U)) : static_cast<u8>('=');
        out[to] = static_cast<u8>('=');
    }
}

// A value of base64's alphabet, or 64.
[[nodiscard]] u32 base64_value(u8 c) noexcept
{
    if (c >= 'A' && c <= 'Z')
    {
        return static_cast<u32>(c - 'A');
    }
    if (c >= 'a' && c <= 'z')
    {
        return static_cast<u32>(c - 'a') + 26U;
    }
    if (c >= '0' && c <= '9')
    {
        return static_cast<u32>(c - '0') + 52U;
    }
    if (c == '+')
    {
        return 62U;
    }
    return (c == '/') ? 63U : 64U;
}

// Section 4.2.1: exactly the 24 characters base64 makes of 16 bytes, its last unused bits zero.
[[nodiscard]] bool key_valid(cont::ConstSpan<u8> key) noexcept
{
    if (key.size() != kWsKeyChars || key[22U] != '=' || key[23U] != '=')
    {
        return false;
    }
    for (usize at = 0U; at < 22U; ++at)
    {
        if (base64_value(key[at]) == 64U)
        {
            return false;
        }
    }
    return (base64_value(key[21U]) & 0x0FU) == 0U;
}

struct RequestLine
{
    cont::ConstSpan<u8> method;
    cont::ConstSpan<u8> target;
    cont::ConstSpan<u8> version;
};

// RFC 9112 section 3: single spaces, a token for the method and a target with neither controls nor spaces.
[[nodiscard]] bool request_line(cont::ConstSpan<u8> line, RequestLine& out) noexcept
{
    usize first = 0U;
    while (first < line.size() && line[first] != ' ')
    {
        ++first;
    }
    usize second = first + 1U;
    while (second < line.size() && line[second] != ' ')
    {
        ++second;
    }
    if (second >= line.size())
    {
        return false;
    }
    out.method = line.first(first);
    out.target = line.subspan(first + 1U, second - first - 1U);
    out.version = line.subspan(second + 1U);
    return http_is_token(out.method) && !out.target.empty() && http_target_valid(out.target);
}

[[nodiscard]] bool is_http_version(cont::ConstSpan<u8> version) noexcept
{
    return version.size() == kVersionBytes && http_equal(version.first(5U), text("HTTP/")) && version[5U] >= '0' &&
           version[5U] <= '9' && version[6U] == '.' && version[7U] >= '0' && version[7U] <= '9';
}

[[nodiscard]] usize index_of(cont::ConstSpan<u8> bytes, u8 wanted, usize from) noexcept
{
    usize at = from;
    while (at < bytes.size() && bytes[at] != wanted)
    {
        ++at;
    }
    return at;
}

// The path of a request target: origin-form, or absolute-form with its authority dropped (RFC 9112 section 3.2.2 has a
// server accept it). False for any other form and for a target holding a fragment.
[[nodiscard]] bool target_path(cont::ConstSpan<u8> target, cont::ConstSpan<u8>& path) noexcept
{
    if (index_of(target, '#', 0U) != target.size())
    {
        return false;
    }
    usize start = 0U;
    if (target[0] != '/')
    {
        const usize colon = index_of(target, ':', 0U);
        if (colon + 2U >= target.size() || target[colon + 1U] != '/' || target[colon + 2U] != '/')
        {
            return false;
        }
        const cont::ConstSpan<u8> scheme = target.first(colon);
        if (!named(scheme, text("http")) && !named(scheme, text("https")) && !named(scheme, text("ws")) &&
            !named(scheme, text("wss")))
        {
            return false;
        }
        start = colon + 3U;
        while (start < target.size() && target[start] != '/' && target[start] != '?')
        {
            ++start;
        }
        if (start == colon + 3U)
        {
            return false;
        }
    }
    const usize end = index_of(target, '?', start);
    path = (start == end) ? text("/") : target.subspan(start, end - start);
    return true;
}

// What check_request() learned from the field lines.
struct RequestFields
{
    u32 seen = 0U;
    bool body = false;
    bool version = false;
    cont::ConstSpan<u8> key;
    cont::ConstSpan<u8> origin;
    usize protocol = kWsMaxProtocols; // the best one offered so far
};

// One field line; 0 when it is fine, or the refusal.
[[nodiscard]] u16 request_field(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value, const WebSocketConfig& config,
                                RequestFields& out) noexcept
{
    if (named(name, text("host")))
    {
        const bool good = first_time(out.seen, kSeenHost) && !value.empty() && http_target_valid(value);
        return good ? 0U : kStatusBad;
    }
    if (named(name, text("sec-websocket-key")))
    {
        out.key = value;
        return first_time(out.seen, kSeenKey) ? 0U : kStatusBad;
    }
    if (named(name, text("origin")))
    {
        out.origin = value;
        return first_time(out.seen, kSeenOrigin) ? 0U : kStatusBad;
    }
    if (named(name, text("sec-websocket-version")))
    {
        out.version = first_time(out.seen, kSeenVersion) && http_equal(value, text("13"));
        return 0U;
    }
    if (named(name, text("upgrade")))
    {
        out.seen |= list_has(value, text("websocket")) ? kSeenUpgrade : 0U;
        return 0U;
    }
    if (named(name, text("connection")))
    {
        out.seen |= list_has(value, text("upgrade")) ? kSeenConnection : 0U;
        return 0U;
    }
    if (named(name, text("sec-websocket-protocol")))
    {
        static_cast<void>(list_any(value,
                                   [&config, &out](cont::ConstSpan<u8> element)
                                   {
                                       for (usize index = 0U; index < out.protocol && index < config.protocols.size();
                                            ++index)
                                       {
                                           if (http_equal(element, config.protocols[index]))
                                           {
                                               out.protocol = index;
                                           }
                                       }
                                       return false;
                                   }));
        return 0U;
    }
    if (named(name, text("transfer-encoding")))
    {
        return kStatusBad;
    }
    if (named(name, text("content-length")))
    {
        return (first_time(out.seen, kSeenLength) && http_equal(value, text("0"))) ? 0U : kStatusBad;
    }
    return 0U;
}

// The answer to a whole request head; `out` gets what a 101 needs.
[[nodiscard]] u16 check_request(cont::ConstSpan<u8> head, const WebSocketConfig& config, WsHandshake& out) noexcept
{
    usize at = 0U;
    cont::ConstSpan<u8> line;
    RequestLine request;
    if (!next_line(head, at, line) || !request_line(line, request) || !is_http_version(request.version))
    {
        return kStatusBad;
    }
    if (!http_equal(request.version, text("HTTP/1.1")))
    {
        return kStatusVersion;
    }
    if (!http_equal(request.method, text("GET")))
    {
        return kStatusMethod;
    }
    RequestFields fields;
    for (usize count = 0U; at + 2U < head.size(); ++count)
    {
        cont::ConstSpan<u8> name;
        cont::ConstSpan<u8> value;
        if (count == kWsMaxHeadFields)
        {
            return kStatusFieldsTooLarge;
        }
        if (!next_field(head, at, name, value))
        {
            return kStatusBad;
        }
        const u16 refusal = request_field(name, value, config, fields);
        if (refusal != 0U)
        {
            return refusal;
        }
    }
    if ((fields.seen & (kSeenUpgrade | kSeenConnection)) != (kSeenUpgrade | kSeenConnection) || !fields.version)
    {
        return kStatusUpgradeRequired;
    }
    cont::ConstSpan<u8> path;
    if ((fields.seen & kSeenHost) == 0U || !key_valid(fields.key) || !target_path(request.target, path))
    {
        return kStatusBad;
    }
    if (!http_equal(path, config.path))
    {
        return kStatusNotFound;
    }
    if ((fields.seen & kSeenOrigin) != 0U && !http_origin_allowed(fields.origin, config.origins, config.any_origin))
    {
        return kStatusForbidden; // section 10.2: what stands between a browser's page and this server
    }
    if (fields.protocol == kWsMaxProtocols)
    {
        return kStatusBad; // nothing else says what the messages carry, and a browser that offered one fails without it
    }
    out.protocol = static_cast<u8>(fields.protocol);
    ws_accept_of(fields.key, cont::Span<u8, kWsAcceptChars>(out.accept, kWsAcceptChars));
    return kStatusUpgrade;
}

[[nodiscard]] cont::ConstSpan<u8> reason_phrase(u16 status) noexcept
{
    switch (status)
    {
        case kStatusBad:
            return text("Bad Request");
        case kStatusForbidden:
            return text("Forbidden");
        case kStatusNotFound:
            return text("Not Found");
        case kStatusMethod:
            return text("Method Not Allowed");
        case kStatusUpgradeRequired:
            return text("Upgrade Required");
        case kStatusFieldsTooLarge:
            return text("Request Header Fields Too Large");
        default:
            return text("HTTP Version Not Supported");
    }
}

// Section 4.1's checks of a 101 response's field lines. True when every one held.
[[nodiscard]] bool response_fields(cont::ConstSpan<u8> head, usize at, const WebSocketConfig& config,
                                   cont::ConstSpan<u8> accept, WsHandshake& out) noexcept
{
    u32 seen = 0U;
    for (usize count = 0U; at + 2U < head.size(); ++count)
    {
        cont::ConstSpan<u8> name;
        cont::ConstSpan<u8> value;
        if (count == kWsMaxHeadFields || !next_field(head, at, name, value))
        {
            return false;
        }
        if (named(name, text("upgrade")))
        {
            seen |= named(value, text("websocket")) ? kSeenUpgrade : 0U;
        }
        else if (named(name, text("connection")))
        {
            seen |= list_has(value, text("upgrade")) ? kSeenConnection : 0U;
        }
        else if (named(name, text("sec-websocket-accept")))
        {
            if (!first_time(seen, kSeenAccept) || !http_equal(value, accept))
            {
                return false;
            }
        }
        else if (named(name, text("sec-websocket-extensions")))
        {
            return false; // none was offered
        }
        else if (named(name, text("sec-websocket-protocol")))
        {
            usize index = 0U;
            while (index < config.protocols.size() && !http_equal(value, config.protocols[index]))
            {
                ++index;
            }
            if (index == config.protocols.size() || !first_time(seen, kSeenProtocol))
            {
                return false;
            }
            out.protocol = static_cast<u8>(index);
        }
    }
    const u32 needed = kSeenUpgrade | kSeenConnection | kSeenAccept | kSeenProtocol;
    return (seen & needed) == needed;
}

} // namespace

HttpDecode ws_read_frame_header(cont::ConstSpan<u8> bytes, bool from_client, usize max_payload, WsFrameHeader& out,
                                u16& failure) noexcept
{
    if (bytes.size() < 2U)
    {
        return HttpDecode::NeedMore;
    }
    const u8 first = bytes[0];
    const u8 second = bytes[1];
    const u8 code = first & kOpcodeMask;
    const bool known = code <= static_cast<u8>(WsOpcode::Binary) || (code >= kFirstControl && code <= 0xAU);
    const bool control = code >= kFirstControl;
    const bool masked = (second & kMaskBit) != 0U;
    const usize short_length = second & kLengthMask;
    const bool fin = (first & kFin) != 0U;
    failure = kWsCloseProtocolError;
    if ((first & kReserved) != 0U || !known || masked != from_client ||
        (control && (!fin || short_length > kWsMaxControlBytes)))
    {
        return HttpDecode::Invalid;
    }
    usize extended = 0U;
    if (short_length >= kExtended16)
    {
        extended = (short_length == kExtended16) ? 2U : 8U;
    }
    const usize header = 2U + extended + (masked ? kWsMaskBytes : 0U);
    if (bytes.size() < header)
    {
        return HttpDecode::NeedMore;
    }
    u64 length = short_length;
    if (extended != 0U)
    {
        length = 0U;
        for (usize at = 0U; at < extended; ++at)
        {
            length = (length << 8U) | bytes[2U + at];
        }
        const u64 smallest = (extended == 2U) ? kExtended16 : 65536U;
        if (length < smallest || (length >> 63U) != 0U)
        {
            return HttpDecode::Invalid; // section 5.2: the minimal number of bytes, and the top bit clear
        }
    }
    if (!control && length > max_payload)
    {
        failure = kWsCloseTooBig;
        return HttpDecode::Invalid;
    }
    out = WsFrameHeader{};
    out.opcode = static_cast<WsOpcode>(code);
    out.fin = fin;
    out.masked = masked;
    out.header_bytes = header;
    out.payload_bytes = static_cast<usize>(length);
    if (masked)
    {
        std::memcpy(out.mask, bytes.data() + header - kWsMaskBytes, kWsMaskBytes);
    }
    failure = 0U;
    return HttpDecode::Ok;
}

usize ws_frame_header_bytes(usize payload_bytes, bool masked) noexcept
{
    usize extended = 0U;
    if (payload_bytes >= kExtended16)
    {
        extended = (payload_bytes <= 0xFFFFU) ? 2U : 8U;
    }
    return 2U + extended + (masked ? kWsMaskBytes : 0U);
}

void ws_write_frame_header(cont::Span<u8> out, WsOpcode opcode, bool fin, usize payload_bytes, const u8* mask) noexcept
{
    cont::WireWriter writer(out);
    const u8 mask_bit = (mask != nullptr) ? kMaskBit : u8{0U};
    writer.put_u8(static_cast<u8>((fin ? kFin : u8{0U}) | static_cast<u8>(opcode)));
    if (payload_bytes < kExtended16)
    {
        writer.put_u8(static_cast<u8>(mask_bit | payload_bytes));
    }
    else if (payload_bytes <= 0xFFFFU)
    {
        writer.put_u8(static_cast<u8>(mask_bit | kExtended16));
        writer.put_u16(static_cast<u16>(payload_bytes));
    }
    else
    {
        writer.put_u8(static_cast<u8>(mask_bit | kExtended64));
        writer.put_u64(payload_bytes);
    }
    if (mask != nullptr)
    {
        writer.put_bytes(cont::ConstSpan<u8>(mask, kWsMaskBytes));
    }
}

void ws_mask(cont::Span<u8> payload, const u8 (&mask)[kWsMaskBytes]) noexcept
{
    u32 half = 0U;
    std::memcpy(&half, mask, sizeof(half));
    const u64 word = (u64{half} << 32U) | half; // the mask's bytes in memory order, on either endianness
    u8* at = payload.data();
    usize left = payload.size();
    while (left >= sizeof(word))
    {
        u64 value = 0U;
        std::memcpy(&value, at, sizeof(value));
        value ^= word;
        std::memcpy(at, &value, sizeof(value));
        at += sizeof(word);
        left -= sizeof(word);
    }
    for (usize index = 0U; index < left; ++index)
    {
        at[index] = static_cast<u8>(at[index] ^ mask[index % kWsMaskBytes]);
    }
}

bool ws_close_code_valid(u16 code) noexcept
{
    return (code >= 1000U && code <= 1003U) || (code >= 1007U && code <= 1014U) || (code >= 3000U && code <= 4999U);
}

u16 ws_read_close(cont::ConstSpan<u8> payload, u16& code, cont::ConstSpan<u8>& reason) noexcept
{
    code = kWsCloseNoStatus;
    reason = {};
    if (payload.empty())
    {
        return 0U;
    }
    if (payload.size() == 1U)
    {
        return kWsCloseProtocolError;
    }
    code = static_cast<u16>((u16{payload[0]} << 8U) | payload[1]);
    if (!ws_close_code_valid(code))
    {
        return kWsCloseProtocolError;
    }
    reason = payload.subspan(2U);
    for (usize at = 0U; at < reason.size();)
    {
        const usize length = utf8_sequence_length(reason.data() + at, reason.size() - at);
        if (length == 0U)
        {
            return kWsCloseInvalidPayload;
        }
        at += length;
    }
    return 0U;
}

bool ws_config_valid(const WebSocketConfig& config, bool client) noexcept
{
    usize request = 256U; // the fixed fields and the key
    if (config.protocols.empty() || config.protocols.size() > kWsMaxProtocols || config.path.empty() ||
        config.path[0] != '/' || !http_target_valid(config.path))
    {
        return false;
    }
    for (const cont::ConstSpan<u8> protocol : config.protocols)
    {
        if (!http_is_token(protocol) || protocol.size() > kWsMaxProtocolBytes)
        {
            return false;
        }
        request += protocol.size() + 2U;
    }
    if (!client)
    {
        return index_of(config.path, '?', 0U) == config.path.size();
    }
    request += config.path.size() + config.host.size() + config.origin.size();
    return !config.host.empty() && http_target_valid(config.host) && value_bytes_valid(config.origin) &&
           request <= kWsMaxHeadBytes;
}

void ws_accept_of(cont::ConstSpan<u8> key, cont::Span<u8, kWsAcceptChars> out) noexcept
{
    u8 joined[kWsKeyChars + sizeof(kGuid) - 1U];
    u8 digest[crypto::kSha1Bytes];
    std::memcpy(joined, key.data(), kWsKeyChars);
    std::memcpy(joined + kWsKeyChars, kGuid, sizeof(kGuid) - 1U);
    crypto::legacy_sha1(cont::ConstSpan<u8>(joined, sizeof(joined)),
                        cont::Span<u8, crypto::kSha1Bytes>(digest, crypto::kSha1Bytes));
    base64(cont::ConstSpan<u8>(digest, sizeof(digest)), out);
}

void ws_key_of(const u8 (&random)[kWsKeyBytes], cont::Span<u8, kWsKeyChars> out) noexcept
{
    base64(cont::ConstSpan<u8>(random, kWsKeyBytes), out);
}

HttpDecode ws_read_request(cont::ConstSpan<u8> bytes, const WebSocketConfig& config, usize& scanned,
                           WsHandshake& out) noexcept
{
    out = WsHandshake{};
    const usize end = head_end(bytes, scanned);
    if (end == 0U && bytes.size() < kWsMaxHeadBytes)
    {
        return HttpDecode::NeedMore;
    }
    if (end == 0U || end > kWsMaxHeadBytes)
    {
        out.status = kStatusFieldsTooLarge;
        return HttpDecode::Ok;
    }
    out.head_bytes = end;
    out.status = check_request(bytes.first(end), config, out);
    return HttpDecode::Ok;
}

usize ws_write_response(cont::Span<u8> out, const WebSocketConfig& config, const WsHandshake& handshake) noexcept
{
    cont::WireWriter writer(out);
    const auto put = [&writer](cont::ConstSpan<u8> bytes)
    {
        writer.put_bytes(bytes);
    };
    if (handshake.status == kStatusUpgrade)
    {
        put(text("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: "));
        put(cont::ConstSpan<u8>(handshake.accept, kWsAcceptChars));
        put(text("\r\nSec-WebSocket-Protocol: "));
        put(config.protocols[handshake.protocol]);
        put(text("\r\n\r\n"));
        return writer.written();
    }
    const u8 digits[3] = {static_cast<u8>('0' + handshake.status / 100U),
                          static_cast<u8>('0' + (handshake.status / 10U) % 10U),
                          static_cast<u8>('0' + handshake.status % 10U)};
    put(text("HTTP/1.1 "));
    put(cont::ConstSpan<u8>(digits, sizeof(digits)));
    put(text(" "));
    put(reason_phrase(handshake.status));
    if (handshake.status == kStatusUpgradeRequired)
    {
        // RFC 9110 sections 7.8 and 15.5.22, and RFC 6455 section 4.2.2.
        put(text("\r\nUpgrade: websocket\r\nConnection: Upgrade, close\r\nSec-WebSocket-Version: 13"));
    }
    else
    {
        put(text("\r\nConnection: close"));
    }
    if (handshake.status == kStatusMethod)
    {
        put(text("\r\nAllow: GET"));
    }
    put(text("\r\nContent-Length: 0\r\n\r\n"));
    return writer.written();
}

usize ws_write_request(cont::Span<u8> out, const WebSocketConfig& config, cont::ConstSpan<u8> key) noexcept
{
    cont::WireWriter writer(out);
    writer.put_bytes(text("GET "));
    writer.put_bytes(config.path);
    writer.put_bytes(text(" HTTP/1.1\r\nHost: "));
    writer.put_bytes(config.host);
    writer.put_bytes(text("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "));
    writer.put_bytes(key);
    writer.put_bytes(text("\r\nSec-WebSocket-Version: 13\r\n"));
    if (!config.origin.empty())
    {
        writer.put_bytes(text("Origin: "));
        writer.put_bytes(config.origin);
        writer.put_bytes(text("\r\n"));
    }
    writer.put_bytes(text("Sec-WebSocket-Protocol: "));
    for (usize index = 0U; index < config.protocols.size(); ++index)
    {
        writer.put_bytes((index == 0U) ? text("") : text(", "));
        writer.put_bytes(config.protocols[index]);
    }
    writer.put_bytes(text("\r\n\r\n"));
    return writer.written();
}

HttpDecode ws_read_response(cont::ConstSpan<u8> bytes, const WebSocketConfig& config, cont::ConstSpan<u8> accept,
                            usize& scanned, WsHandshake& out) noexcept
{
    out = WsHandshake{};
    const usize end = head_end(bytes, scanned);
    if (end == 0U && bytes.size() < kWsMaxHeadBytes)
    {
        return HttpDecode::NeedMore;
    }
    if (end == 0U || end > kWsMaxHeadBytes)
    {
        return HttpDecode::Ok; // status 0: no head this side reads
    }
    out.head_bytes = end;
    const cont::ConstSpan<u8> head = bytes.first(end);
    usize at = 0U;
    cont::ConstSpan<u8> line;
    // RFC 9112 section 4: HTTP-version SP 3DIGIT SP [reason].
    if (!next_line(head, at, line) || line.size() < kVersionBytes + 5U || !is_http_version(line.first(kVersionBytes)) ||
        line[kVersionBytes] != ' ' || line[kVersionBytes + 4U] != ' ')
    {
        return HttpDecode::Ok;
    }
    u16 status = 0U;
    for (usize index = 0U; index < 3U; ++index)
    {
        const u8 digit = line[kVersionBytes + 1U + index];
        if (digit < '0' || digit > '9')
        {
            return HttpDecode::Ok;
        }
        status = static_cast<u16>(status * 10U + (digit - '0'));
    }
    if (status != kStatusUpgrade || response_fields(head, at, config, accept, out))
    {
        out.status = status;
    }
    return HttpDecode::Ok;
}

} // namespace crd::net
