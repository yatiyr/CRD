#pragma once

// crd-net -- what makes an HTTP message well formed on the framed versions, HTTP/2 and HTTP/3 (4.2a; contract:
// docs/design/network-foundation.md#http3). Sources: RFC 9110 sections 5.1, 5.5, 8.6, 9.3.6 and 15.2, RFC 9113 section
// 8, RFC 9114 sections 4.1.2, 4.2, 4.3, 4.4 and 10.3, RFC 8441 section 4 and RFC 9220.
//
//   fields    names are RFC 9110 tokens in lower case, values hold no NUL, CR or LF and do not begin or end with a
//             space or tab; a field that would let an intermediary split or smuggle a message is refused here, on both
//             directions (what this side sends passes the same checks).
//   messages  pseudo-fields come first and are known, single and of the right kind; the fields HTTP/1.1 used to manage
//             a connection are refused; content-length is one number. CONNECT carries only :authority, and an extended
//             CONNECT (:protocol) the three a request has.
//   memory    nothing allocates; hostile input costs time linear in its length.
//   threads   no state.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/http_fields.hpp>

namespace crd::net
{
// What the checks learned of a message. The spans view the fields given.
struct HttpMessageInfo
{
    cont::ConstSpan<u8> method;
    cont::ConstSpan<u8> scheme;
    cont::ConstSpan<u8> authority;
    cont::ConstSpan<u8> path;
    cont::ConstSpan<u8> protocol; // an extended CONNECT's
    u16 status = 0U;
    u64 content_length = 0U;
    bool has_content_length = false;
    bool connect = false;  // :method is CONNECT
    bool extended = false; // and it has :protocol
    bool head = false;     // :method is HEAD
};

// Whether a field section is a well-formed request, response or set of trailers. `info` is filled when true.
[[nodiscard]] bool http_check_request(cont::ConstSpan<HttpField> fields, HttpMessageInfo& info) noexcept;
[[nodiscard]] bool http_check_response(cont::ConstSpan<HttpField> fields, HttpMessageInfo& info) noexcept;
[[nodiscard]] bool http_check_trailers(cont::ConstSpan<HttpField> fields) noexcept;

// A field section's size as SETTINGS_MAX_FIELD_SECTION_SIZE counts it: names, values and 32 bytes per field.
[[nodiscard]] u64 http_section_size(cont::ConstSpan<HttpField> fields) noexcept;

// RFC 9110 section 5.6.2's token: one or more of its characters.
[[nodiscard]] bool http_is_token(cont::ConstSpan<u8> text) noexcept;
// ASCII case-insensitive equality.
[[nodiscard]] bool http_same_ignoring_case(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept;
// Visible bytes and those above ASCII: neither a space nor a control can hide in a request target or an authority.
[[nodiscard]] bool http_target_valid(cont::ConstSpan<u8> text) noexcept;
// A browser's Origin value against a server's list, compared exactly (`any` takes every value, a deliberate choice).
[[nodiscard]] bool http_origin_allowed(cont::ConstSpan<u8> value, cont::ConstSpan<cont::ConstSpan<u8>> origins,
                                       bool any) noexcept;

} // namespace crd::net
