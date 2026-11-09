#pragma once

// crd-net -- the WebSocket protocol's pieces, sans-I/O (4.2a; contract: docs/design/network-foundation.md#websocket).
// Source: RFC 6455, with RFC 9110 and RFC 9112 for the HTTP/1.1 around its opening handshake.
//
//   frames    a header is read whole and every section 5 refusal happens there, so a caller holding a whole frame has
//             only the fragmentation rules left. Lengths use their shortest form; a client's frames are masked and a
//             server's are not.
//   upgrade   one request or response head, ended by an empty line and held to RFC 9112's grammar: CRLF only, no folded
//             lines, no space before a colon, at most kWsMaxHeadBytes and kWsMaxHeadFields. A request with a body is
//             refused, so nothing can be smuggled behind it.
//   policy    Origin is checked as WebTransport checks it (a request with none is not from a browser), the path is
//             compared exactly, and the subprotocol must be one of the server's: nothing else names what the messages
//             carry. Extensions are never negotiated; an offer is ignored.
//   memory    nothing allocates; hostile input costs time linear in its length.
//   threads   no state.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/http_fields.hpp>

namespace crd::net
{
inline constexpr usize kWsMaxHeaderBytes = 14U;     // a frame's header at its longest
inline constexpr usize kWsMaxControlBytes = 125U;   // a control frame's payload (section 5.5)
inline constexpr usize kWsMaxHeadBytes = 8192U;     // an opening handshake's request or response head
inline constexpr usize kWsMaxHeadFields = 64U;      // its field lines
inline constexpr usize kWsMaxResponseBytes = 1024U; // what ws_write_response() needs
inline constexpr usize kWsMaxProtocols = 8U;        // subprotocols a side lists
inline constexpr usize kWsMaxProtocolBytes = 255U;  // one of them
inline constexpr usize kWsMaskBytes = 4U;
inline constexpr usize kWsKeyBytes = 16U;    // a Sec-WebSocket-Key before base64
inline constexpr usize kWsKeyChars = 24U;    // and after
inline constexpr usize kWsAcceptChars = 28U; // a Sec-WebSocket-Accept

// Section 7.4's status codes this side names.
inline constexpr u16 kWsCloseNormal = 1000U;
inline constexpr u16 kWsCloseGoingAway = 1001U;
inline constexpr u16 kWsCloseProtocolError = 1002U;
inline constexpr u16 kWsCloseUnsupportedData = 1003U;
inline constexpr u16 kWsCloseNoStatus = 1005U; // never on the wire: a Close frame with no code
inline constexpr u16 kWsCloseInvalidPayload = 1007U;
inline constexpr u16 kWsCloseTooBig = 1009U;
inline constexpr u16 kWsCloseInternalError = 1011U;

enum class WsOpcode : u8
{
    Continuation = 0x0U,
    Text = 0x1U,
    Binary = 0x2U,
    Close = 0x8U,
    Ping = 0x9U,
    Pong = 0xAU,
};

struct WsFrameHeader
{
    WsOpcode opcode = WsOpcode::Continuation;
    bool fin = false;
    bool masked = false;
    u8 mask[kWsMaskBytes] = {};
    usize header_bytes = 0U;
    usize payload_bytes = 0U;
};

// Reads the header at the start of `bytes`, whose frame came from a client when `from_client`. NeedMore until the whole
// header has arrived. Invalid names the status to fail with in `failure`: kWsCloseProtocolError for anything section 5
// refuses, kWsCloseTooBig for a data frame of more than `max_payload` bytes.
[[nodiscard]] HttpDecode ws_read_frame_header(cont::ConstSpan<u8> bytes, bool from_client, usize max_payload,
                                              WsFrameHeader& out, u16& failure) noexcept;
// The bytes a frame's header takes: the frame is masked when a client sends it.
[[nodiscard]] usize ws_frame_header_bytes(usize payload_bytes, bool masked) noexcept;
// Writes a header ending right before `out`'s end; `mask` is null for a server's frame. `out` is exactly
// ws_frame_header_bytes() long.
void ws_write_frame_header(cont::Span<u8> out, WsOpcode opcode, bool fin, usize payload_bytes, const u8* mask) noexcept;
// Masks or unmasks a whole payload in place (the same operation).
void ws_mask(cont::Span<u8> payload, const u8 (&mask)[kWsMaskBytes]) noexcept;

// Whether `code` may appear in a Close frame (section 7.4 and the IANA registry).
[[nodiscard]] bool ws_close_code_valid(u16 code) noexcept;
// Reads a Close frame's payload: `code` is kWsCloseNoStatus for one with none. 0 when it is valid, or the status to
// fail with: kWsCloseProtocolError for a one-byte payload or a code never sent, kWsCloseInvalidPayload for a reason
// that is not UTF-8.
[[nodiscard]] u16 ws_read_close(cont::ConstSpan<u8> payload, u16& code, cont::ConstSpan<u8>& reason) noexcept;

// What a side lists. The views must outlive every connection using them.
struct WebSocketConfig
{
    // The subprotocols, 1 to kWsMaxProtocols tokens: a client offers them in its order, a server takes the first of its
    // own that the client offers.
    cont::ConstSpan<cont::ConstSpan<u8>> protocols;
    // The resource, beginning with '/'. A client's may hold a query; a server's is compared exactly with the request's
    // path, its query ignored.
    cont::ConstSpan<u8> path;
    // A client's: the Host value, and the Origin it claims (empty sends none).
    cont::ConstSpan<u8> host;
    cont::ConstSpan<u8> origin;
    // A server's: the Origin values a browser may come from. An empty list refuses every browser.
    cont::ConstSpan<cont::ConstSpan<u8>> origins;
    bool any_origin = false; // takes every Origin: a deliberate choice, never a default
};

// Whether a configuration can be used by a client or a server: its request fits kWsMaxHeadBytes and every part is well
// formed.
[[nodiscard]] bool ws_config_valid(const WebSocketConfig& config, bool client) noexcept;

// What one opening handshake head said.
struct WsHandshake
{
    u16 status = 0U;       // a request: what to answer. A response: the server's, 0 for a head this side refuses
    u8 protocol = 0U;      // the configured subprotocol's index, when the status is 101
    usize head_bytes = 0U; // what the head took of the bytes given
    u8 accept[kWsAcceptChars] = {}; // a request answered 101: the Sec-WebSocket-Accept to send
};

// Sec-WebSocket-Accept for `key` (section 4.2.2): base64 of the SHA-1 of the key and the protocol's GUID.
void ws_accept_of(cont::ConstSpan<u8> key, cont::Span<u8, kWsAcceptChars> out) noexcept;
// Base64 of 16 random bytes.
void ws_key_of(const u8 (&random)[kWsKeyBytes], cont::Span<u8, kWsKeyChars> out) noexcept;

// Reads a request head from the start of `bytes`, any amount of which may have arrived: NeedMore until the head has
// ended, Ok with `out.status` naming the answer (101 or the refusal) once it has. `scanned` starts at 0 and keeps how
// far the search for its end went, so bytes arriving one at a time cost linear time.
[[nodiscard]] HttpDecode ws_read_request(cont::ConstSpan<u8> bytes, const WebSocketConfig& config, usize& scanned,
                                         WsHandshake& out) noexcept;
// Writes the answer to `handshake` into `out` (kWsMaxResponseBytes): the bytes used.
[[nodiscard]] usize ws_write_response(cont::Span<u8> out, const WebSocketConfig& config,
                                      const WsHandshake& handshake) noexcept;

// Writes a client's request into `out`, at least kWsMaxHeadBytes: the bytes used. `key` is ws_key_of()'s.
[[nodiscard]] usize ws_write_request(cont::Span<u8> out, const WebSocketConfig& config,
                                     cont::ConstSpan<u8> key) noexcept;
// Reads the response to a request whose Sec-WebSocket-Accept must be `accept`. As ws_read_request(); `out.status` is
// 101 only for a response section 4.1 accepts.
[[nodiscard]] HttpDecode ws_read_response(cont::ConstSpan<u8> bytes, const WebSocketConfig& config,
                                          cont::ConstSpan<u8> accept, usize& scanned, WsHandshake& out) noexcept;

} // namespace crd::net
