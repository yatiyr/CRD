#pragma once

// crd-net -- WebSockets over an HTTP/2 connection (RFC 8441) or an HTTP/3 connection (RFC 9220), each carrying QMux
// as the HTTP/1.1 path does (4.2a; contract: docs/design/network-foundation.md#websocket-tunnels).
//
//   sans-I/O  it sits on the HTTP connection it was given. The owner polls that connection and hands every event to
//             on_event(), which keeps the tunnels' and passes the rest back (false); then poll() serves the tunnels.
//   tunnels   a fixed pool searched by stream ID, each a QmuxConnection with no TLS of its own: the application meets
//             the same QuicStreamSet and QuicDatagrams as on every other path, so a ChannelLink runs over it unchanged.
//   policy    a server answers an extended CONNECT whose :protocol is `websocket` by ws_check_connect(): the same
//             Origin, path and subprotocol rules as the HTTP/1.1 handshake, so the application never sees a refused
//             one. A request in 0-RTT is answered 425 (RFC 8470): an opening handshake that could be replayed is none.
//             Beyond the pool it is refused as a stream (REFUSED_STREAM, H3_REQUEST_REJECTED).
//   bytes     a tunnel takes every byte it is given and returns the stream's credit at once: its own streams' flow
//             control bounds what it holds. What it writes goes out as the HTTP stream's credit and room allow.
//   ends      a clean end of the stream is the end of the transport (RFC 8441 section 5); a reset is its failure.
//             STOP_SENDING ends this side's direction alone, and the peer's is read to its end (RFC 9000 section 3.5):
//             a peer may stop reading once its Close has gone, and a server may answer an opening request complete
//             after stopping it (RFC 9114 section 4.1.1), where a refusal keeps its status and a 2xx is refused, as
//             nothing could reach the server. A closed tunnel's last bytes are tried once; its stream is then
//             finished, or cancelled (CANCEL, H3_REQUEST_CANCELLED) when they could not all go or the tunnel failed.
//   memory    the pool comes from the injected allocator at init(); a tunnel's streams, datagrams and staging buffer
//             when it is opened, so the allocator bounds them.
//   threads   the owning connection's.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/host_handle.hpp>
#include <crd/net/http2.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/qmux.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
inline constexpr u32 kWsMaxTunnels = 256U;

struct WsTunnelConfig
{
    // Every tunnel's: `websocket` is what a client offers or a server accepts and must outlive this; `tls` stays null
    // and `tunnel` None, as init() sets the side from the HTTP connection's.
    QmuxConfig connection;
    u32 tunnels = 4U; // open at once, 1 to kWsMaxTunnels
};

enum class WsTunnelEventKind : u8
{
    Opened,  // a server accepted one, or a client's was answered 2xx; what arrived behind it may have ended it already,
             // and its Closed follows
    Refused, // a client's: `status` of the response (0 for a 2xx it refuses) or `error` of a reset, both 0 when no
             // answer came within the handshake timeout; its connection is Closed and says which (UpgradeRefused,
             // Reset, Truncated or HandshakeTimeout); then forgotten
    Service, // bytes arrived or a timer ran: read and write its streams
    Closed,  // the tunnel is Closed: read what arrived before the end; it is destroyed by the next poll()
};

struct WsTunnelEvent
{
    WsTunnelEventKind kind = WsTunnelEventKind::Opened;
    QuicHandle tunnel;
    u64 stream = 0U;
    QmuxConnection* connection = nullptr; // valid until the next poll()
    u32 tag = kNoConnection;              // what the owner tagged the tunnel with
    u16 status = 0U;
    u64 error = 0U;
};

enum class WsTunnelError : u8
{
    None,
    NotReady,  // the server has not allowed extended CONNECT (yet)
    NoTunnel,  // every tunnel of the pool is in use
    Refused,   // the HTTP connection refuses a request now: its error(), GOAWAY or stream limits
    Blocked,   // no room: try again after the HTTP connection's Writable
    Failed,    // the tunnel's connection could not be made
};

class WebSocketTunnels
{
public:
    WebSocketTunnels() noexcept = default;
    ~WebSocketTunnels() noexcept;
    WebSocketTunnels(const WebSocketTunnels&) = delete;
    WebSocketTunnels& operator=(const WebSocketTunnels&) = delete;
    WebSocketTunnels(WebSocketTunnels&&) = delete;
    WebSocketTunnels& operator=(WebSocketTunnels&&) = delete;

    // False, holding nothing, for a configuration out of range (a server's HTTP connection must take extended CONNECT)
    // or a refusal. The connection must outlive this. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, Http2Connection& http2,
                            const WsTunnelConfig& config) noexcept;
    [[nodiscard]] bool init(memory::IAllocator& allocator, Http3Connection& http3,
                            const WsTunnelConfig& config) noexcept;

    // An event of the HTTP connection's poll(): true when it was a tunnel's, which is then done with. Body payloads
    // are read here, before the next poll() ends their view.
    [[nodiscard]] bool on_event(const Http2Event& event, u64 now_ns) noexcept;
    [[nodiscard]] bool on_event(const Http3Event& event, u64 now_ns) noexcept;

    // The next thing that happened, false when nothing is left for now. Runs the tunnels' timers and sends what they
    // owe first.
    [[nodiscard]] bool poll(WsTunnelEvent& event, u64 now_ns) noexcept;
    // Sends what the tunnels owe: after the application wrote to them outside a poll().
    void flush(u64 now_ns) noexcept;
    // When poll() must run though nothing arrived; kQmuxNoTimer for never.
    [[nodiscard]] u64 next_deadline_ns() const noexcept;

    // A client's new tunnel, its extended CONNECT built from the configuration's WebSocketConfig.
    [[nodiscard]] WsTunnelError open(u64 now_ns, QuicHandle& handle) noexcept;
    // The live tunnel `handle` names; null for a stale or unknown handle.
    [[nodiscard]] QmuxConnection* connection(QuicHandle handle) noexcept;
    // Names the tunnel in its events from now on (a host's own slot for it); false for a stale or unknown handle.
    [[nodiscard]] bool tag(QuicHandle handle, u32 tag) noexcept;
    // The transport beneath the HTTP connection ended, or its owner closed it: every tunnel ends at the next poll(),
    // as on a failed HTTP connection.
    void bearer_ended() noexcept { m_bearer_ended = true; }
    [[nodiscard]] u32 live() const noexcept { return m_live; }

private:
    struct Slot
    {
        QmuxConnection* connection = nullptr; // live: in m_cells
        u64 stream = 0U;
        u64 deadline_ns = kQmuxNoTimer; // a client's: the answer is due by then
        u64 error = 0U;
        u32 generation = 0U;
        u32 tag = kNoConnection;
        u16 status = 0U;
        bool opening = false;     // a client's: no answer yet
        bool stopped = false;     // the peer reads no more of the stream (STOP_SENDING)
        bool finished = false;    // this side's end of the stream went
        bool peer_ended = false;  // the peer's did, or its reset
        bool report_open = false;
        bool report_service = false;
        bool refused = false;     // a client's that was never opened
        bool report_end = false;  // Refused or Closed: then forgotten
    };

    [[nodiscard]] bool setup(memory::IAllocator& allocator, const WsTunnelConfig& config, bool client) noexcept;
    [[nodiscard]] Slot* find(u64 stream) noexcept;
    [[nodiscard]] u32 claim(u64 stream) noexcept;
    void forget(u32 index) noexcept;
    // A client's tunnel that will not open: its connection is Closed for `reason`, and the end is reported.
    void refuse(Slot& slot, QmuxCloseReason reason, u64 code) noexcept;
    [[nodiscard]] QuicHandle handle_of(u32 index) const noexcept;
    [[nodiscard]] bool on_headers(u64 stream, cont::ConstSpan<HttpField> fields, const HttpMessageInfo& info,
                                  bool early, u64 now_ns) noexcept;
    void accept(u64 stream, cont::ConstSpan<HttpField> fields, const HttpMessageInfo& info, bool early,
                u64 now_ns) noexcept;
    void answer(Slot& slot, cont::ConstSpan<HttpField> fields, const HttpMessageInfo& info, u64 now_ns) noexcept;
    void deliver(Slot& slot, cont::ConstSpan<u8> bytes, u64 now_ns) noexcept;
    [[nodiscard]] bool ended(u64 stream, bool clean, u64 error, u64 now_ns) noexcept;
    [[nodiscard]] bool stopped(u64 stream, u64 error) noexcept;
    [[nodiscard]] bool pump(Slot& slot, u64 now_ns) noexcept;
    void settle(Slot& slot) noexcept;
    [[nodiscard]] bool fill(Slot& slot, WsTunnelEvent& event, u32 index) noexcept;

    // The HTTP connection, whichever version it is.
    [[nodiscard]] bool bearer_failed() const noexcept;
    [[nodiscard]] bool respond(u64 stream, cont::ConstSpan<HttpField> fields, bool fin) noexcept;
    [[nodiscard]] usize send(u64 stream, cont::ConstSpan<u8> bytes, bool& blocked) noexcept;
    [[nodiscard]] bool finish(u64 stream) noexcept;
    [[nodiscard]] bool cancel(u64 stream, bool clean) noexcept;
    void refuse_stream(u64 stream) noexcept;

    memory::IAllocator* m_allocator = nullptr;
    Http2Connection* m_http2 = nullptr;
    Http3Connection* m_http3 = nullptr;
    WsTunnelConfig m_config;
    Slot* m_slots = nullptr;
    QmuxConnection* m_cells = nullptr; // uninitialised storage, one per slot
    u32 m_live = 0U;
    u32 m_walk = 0U;   // where poll()'s walk stands
    u32 m_first = 0U;  // where the next flush starts, so no tunnel is always first
    u32 m_release = 0U; // 1 + the slot whose end was just reported
    bool m_bearer_ended = false;
};

} // namespace crd::net
