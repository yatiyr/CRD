#pragma once

// crd-net -- the capsule protocol, for any HTTP data stream (4.2a; contract: docs/design/network-foundation.md#http3).
// Source: RFC 9297 section 3.
//
//   reading   capsules come from a stream split anywhere, the value handed on as it arrives and in place, never
//             gathered (section 3.2: holding a whole value can exhaust flow control and deadlock). A long header waits
//             in the reader.
//   ends      a stream may end only between capsules (section 3.3); at_boundary() says whether it is at one.
//   memory    nothing allocates. threads: one at a time.

#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/net/http_fields.hpp>

namespace crd::net
{
inline constexpr u64 kHttpCapsuleDatagram = 0x00U;

// A piece of one capsule's value, or the whole of an empty one. `value` views the bytes given to next().
struct HttpCapsuleChunk
{
    u64 type = 0U;
    u64 length = 0U; // the capsule's whole value
    u64 offset = 0U; // where `value` starts in it
    cont::ConstSpan<u8> value;
    bool complete = false; // this piece ends the capsule
};

class HttpCapsuleReader
{
public:
    // Takes bytes of the stream and returns how many it used; `got` is true when `chunk` holds a piece. Call again with
    // the rest until it uses nothing and gives nothing.
    [[nodiscard]] usize next(cont::ConstSpan<u8> bytes, HttpCapsuleChunk& chunk, bool& got) noexcept;
    [[nodiscard]] bool at_boundary() const noexcept { return !m_in_value && m_head_bytes == 0U; }

private:
    u8 m_head[16] = {};
    usize m_head_bytes = 0U;
    u64 m_type = 0U;
    u64 m_length = 0U;
    u64 m_offset = 0U;
    bool m_in_value = false;
};

// The bytes a capsule's header takes.
[[nodiscard]] usize http_capsule_header_bytes(u64 type, u64 length) noexcept;
void put_http_capsule_header(cont::WireWriter& out, u64 type, u64 length) noexcept;

// Whether the fields say the capsule protocol is in use: one Capsule-Protocol field holding the Boolean true,
// parameters ignored. Several instances form a list, so anything but one Boolean counts as absent (section 3.4).
[[nodiscard]] bool http_capsule_protocol(cont::ConstSpan<HttpField> fields) noexcept;

} // namespace crd::net
