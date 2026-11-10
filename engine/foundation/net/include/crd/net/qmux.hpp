#pragma once

// crd-net -- QMux (draft-ietf-quic-qmux-02): QUIC's streams and datagrams over TLS 1.3 on a byte stream, the path for
// networks that block UDP (4.2a; contract: docs/design/network-foundation.md#qmux). An application meets the same
// QuicStreamSet and QuicDatagrams as on QUIC; only the transport beneath them differs.
//
//   sans-I/O  bytes the transport received go in through receive(), bytes to send come out of take(); a StreamSocket,
//             NET-SIM's virtual stream and a test's pipe drive it alike.
//   records   each record is a Size and whole QUIC version 1 frames, 16382 bytes at most either way: larger records
//             buy nothing over TLS records of 16 KiB, so this side never advertises more and sends at most that.
//   parameters QX_TRANSPORT_PARAMETERS is the first frame each side sends, the moment TLS can carry it, and the first
//             a side reads; a parameter QUIC needs but QMux forbids is refused.
//   frames    PING, ACK, CRYPTO and the path, token and connection ID frames do not exist here; a peer's are refused.
//   streams   TCP delivers in order, so a stream frame that does not continue its stream is refused, and a frame is
//             acknowledged the moment TLS takes it. Reads never wait for the application: streams are bounded by flow
//             control and datagrams the application is late for are dropped.
//   0-RTT     a resumed client sends its parameters and frames as TLS early data under the server parameters its
//             ticket remembers; a server that accepts reads them marked replayable and answers at 0.5-RTT. Frames
//             sent early count as delivered only once the server accepted; a rejection returns the streams to their
//             start, and handshake().early_data() tells the application to send again.
//   liveness  the idle timer restarts when a whole record arrives or the last byte is handed to the transport, never
//             on TCP or TLS keep-alives; QX_PING keeps intermediaries awake and measures the RTT that tunes windows.
//   close     CONNECTION_CLOSE, then close_notify and a shutdown of the sending side, then the peer's end (data
//             arriving meanwhile is discarded). A stream ending without close_notify is a truncation, never a clean
//             end.
//   WebSocket a connection whose TLS agrees on http/1.1, where a WebSocketConfig allows it, runs an opening handshake
//             and then carries one record in each Binary message, with no Size (the browser path; contract:
//             docs/design/network-foundation.md#websocket). Everything above the records is the same code.
//   tunnel    a connection over an HTTP/2 or HTTP/3 stream (RFC 8441, RFC 9220; websocket_tunnel.hpp) is that same message
//             stage with no TLS of its own: bytes in and out are the stream's, and the extended CONNECT was its opening
//             handshake.
//   HTTP/2    a connection whose TLS agrees on h2, where an Http2Config allows it, carries HTTP/2 (RFC 9113) and its
//             WebSocket tunnels: the browser's path when it already holds an HTTP/2 connection to the origin (contract:
//             docs/design/network-foundation.md#h2-bearer). The connection is their bearer and carries no records.
//   memory    records use two fixed buffers of the connection; streams and datagrams allocate at init(), never after.
//   threads   one thread at a time.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/host_handle.hpp>
#include <crd/net/quic_datagram.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_stream_set.hpp>
#include <crd/net/websocket.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
struct Http2Config;
struct Http2Event;
struct WsTunnelEvent;
enum class WsTunnelError : u8;
struct QmuxHttp2;

// The default of max_record_size, and the largest record either side sends or accepts.
inline constexpr usize kQmuxMaxRecord = 16382U;
// The bytes of a close reason phrase kept, sent or read; a longer one is cut.
inline constexpr usize kQmuxCloseReasonBytes = 128U;
inline constexpr u64 kQmuxNoTimer = ~u64{0};

// Which end of an HTTP stream a connection is, when it runs over one.
enum class QmuxTunnel : u8
{
    None, // over TLS of its own
    Client,
    Server,
};

struct QmuxConfig
{
    const crypto::TlsContext* tls = nullptr;   // client or server, with ALPN; null, and only then, for a tunnel
    crypto::TlsTicketStore* tickets = nullptr; // a client's tickets; nullptr offers and keeps none
    // A server accepting early data: the application's part of its early data context (at most
    // kQuicMaxEarlyContextBytes, read by start()), after the limits the connection adds itself, so a ticket's 0-RTT
    // is refused once either changed. Empty where early data is not accepted.
    cont::ConstSpan<u8> early_context;
    // Takes connections whose TLS agrees on `http/1.1` as WebSocket, what ws_config_valid() accepts for this side; null
    // for none. A server's context may list QMux identifiers beside it, so one port serves both. A client's context
    // must not ask for early data: a request that could be replayed is no opening handshake.
    const WebSocketConfig* websocket = nullptr;
    // Set by the owner of a tunnel, whose `websocket` is required and whose TLS and tickets are the bearer's.
    QmuxTunnel tunnel = QmuxTunnel::None;
    // Takes connections whose TLS agrees on `h2` as HTTP/2 whose extended CONNECTs open WebSocket tunnels of `websocket`,
    // which is then required; null for none. Its side is the connection's and its frames at most kHttp2MinFrameBytes
    // (one TLS record); a server's takes extended CONNECT, and a client's context must not ask for early data. Every
    // tunnel runs this configuration's limits and timers. What it points at must outlive the connection.
    const Http2Config* http2 = nullptr;
    u32 http2_tunnels = 4U; // the tunnels one HTTP/2 connection carries at once, 1 to kWsMaxTunnels
    QuicStreamLimits streams;
    QuicDatagramLimits datagrams;
    u64 idle_timeout_ns = 30'000'000'000U;      // the idle timeout offered; 0 for none
    u64 handshake_timeout_ns = 10'000'000'000U; // from start() to the end of the TLS handshake
    u64 close_wait_ns = 1'000'000'000U;         // how long a closed side waits for the peer's end
    u64 keepalive_ns = 0U;                      // a QX_PING after this long with no record; 0 for none
    // Records pad to the next multiple of this with PADDING frames (NET.3's padded_size()); 0 pads nothing.
    u32 padding_block = 0U;
};

enum class QmuxState : u8
{
    Idle,        // before start()
    Handshaking, // the TLS handshake runs, and a WebSocket's opening handshake after it
    Established, // records flow (peer_parameters_known() tells whether the peer's arrived); on a server that accepted
                 // early data from the client's first flight, before the handshake ends
    Closing,     // this side closed: only the peer's end, or the wait for it, remains
    Closed,      // done: flush take(), shut the sending side, destroy
};

// Why a connection ended.
enum class QmuxCloseReason : u8
{
    None,
    Local,            // close() was called: `application`, error_code
    Error,            // this side closed with the transport error `error_code` (frame_type: the frame that caused it)
    Peer,             // the peer's CONNECTION_CLOSE: `application`, error_code, frame_type; on HTTP/2 its GOAWAY with an
                      // error (error_code, not `application`)
    PeerShutdown,     // the peer's close_notify with no CONNECTION_CLOSE (its idle timeout, draft section 7.1), or
                      // its WebSocket Close with none (error_code: the Close frame's status, 1005 for none)
    IdleTimeout,      // this side's idle timeout
    HandshakeTimeout, // the TLS handshake did not end in time
    TlsFailed,        // a fatal alert or a refusal; handshake() and tls_error() tell which
    Truncated,        // the transport ended without close_notify
    Reset,            // the transport was reset or failed
    UpgradeRefused,   // the opening handshake failed: error_code is the HTTP status (a server's answer, or the one a
                      // client read; 0 for a response this client refuses)
    WebSocketFailed,  // this side failed the WebSocket with the Close status error_code
    Http2Failed,      // this side's HTTP/2 connection error error_code (RFC 9113 section 7), INTERNAL_ERROR when its
                      // state could not be allocated
};

struct QmuxCloseInfo
{
    QmuxCloseReason reason = QmuxCloseReason::None;
    bool application = false;
    u64 error_code = 0U;
    u64 frame_type = 0U;
};

struct QmuxCounters
{
    u64 records_sent = 0U;
    u64 records_received = 0U;
    u64 bytes_padded = 0U;
    u64 pings_sent = 0U;
    u64 pongs_received = 0U;
};

class QmuxConnection
{
public:
    QmuxConnection() noexcept = default;
    ~QmuxConnection();
    QmuxConnection(const QmuxConnection&) = delete;
    QmuxConnection& operator=(const QmuxConnection&) = delete;
    QmuxConnection(QmuxConnection&&) = delete;
    QmuxConnection& operator=(QmuxConnection&&) = delete;

    // Takes the streams' and datagrams' storage from `allocator`; false, holding nothing, for a configuration out of
    // range or a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, const QmuxConfig& config) noexcept;

    // Starts the TLS session of the context's role: a client's first flight is ready for take(). False for a session
    // TLS refuses (tls_error()); the connection is then Closed.
    [[nodiscard]] bool start(u64 now_ns) noexcept;
    // Starts a tunnel whose extended CONNECT agreed on the configured subprotocol `protocol`: it is Established at once.
    // False for a connection that is not a tunnel, or was started.
    [[nodiscard]] bool start_tunnel(u8 protocol, u64 now_ns) noexcept;

    // Bytes the transport received, any amount, split anywhere.
    void receive(cont::ConstSpan<u8> bytes, u64 now_ns) noexcept;
    // The transport ended: `clean` for its orderly end (a FIN), false for a reset or a failure. Nothing more can be
    // sent, and take() has nothing.
    void transport_ended(bool clean, u64 now_ns) noexcept;

    // Whether take() would give bytes now.
    [[nodiscard]] bool wants_write(u64 now_ns) const noexcept;
    // Assembles what is due and moves up to out.size() bytes to send into `out`; the bytes it wrote.
    [[nodiscard]] usize take(cont::Span<u8> out, u64 now_ns) noexcept;
    // Assembles what is due and shows the bytes to send in place, valid until the next call: a host writes them to its
    // transport and take()s what the transport accepted, so a partial write needs no second buffer.
    [[nodiscard]] cont::ConstSpan<u8> outgoing(u64 now_ns) noexcept;
    // A tunnel's: `count` of the bytes outgoing() showed went out, without take() having to copy them.
    void taken(usize count, u64 now_ns) noexcept;
    // True once this side has closed and every byte has been taken: time to shut the transport's sending side.
    [[nodiscard]] bool send_finished() const noexcept;

    [[nodiscard]] u64 next_timer_ns() const noexcept;
    void on_timeout(u64 now_ns) noexcept;

    // Closes with `application_error` (CONNECTION_CLOSE 0x1d) and the first kQmuxCloseReasonBytes of `reason`. During
    // the handshake the session is abandoned, as TLS allows no application close there.
    void close(u64 application_error, cont::ConstSpan<u8> reason, u64 now_ns) noexcept;
    // Sends a QX_PING request.
    void ping() noexcept { m_ping_owed = true; }
    // Ends an Established connection with CONNECTION_REFUSED (RFC 9000 section 20.1): its host has no room for it.
    void refuse(u64 now_ns) noexcept;
    // A tunnel that never opened (WebSocketTunnels' refusals): Closed for `reason` with `error_code`.
    void abandon(QmuxCloseReason reason, u64 error_code) noexcept;

    // ---- HTTP/2 (http2()) ----
    // The next thing that happened to a tunnel, false when nothing is left for now (websocket_tunnel.hpp). Once this
    // connection closes, every tunnel ends and is reported.
    [[nodiscard]] bool poll_tunnel(WsTunnelEvent& event, u64 now_ns) noexcept;
    // A client's new tunnel: NotReady until the server's SETTINGS allow extended CONNECT, Refused on a connection that
    // is not HTTP/2 or no longer Established.
    [[nodiscard]] WsTunnelError open_tunnel(u64 now_ns, QuicHandle& handle) noexcept;
    // Names a tunnel in its events (WebSocketTunnels::tag()); false for a stale handle.
    [[nodiscard]] bool tag_tunnel(QuicHandle handle, u32 tag) noexcept;
    // Its tunnels live, opening ones included.
    [[nodiscard]] u32 tunnels_live() const noexcept;

    [[nodiscard]] QmuxState state() const noexcept { return m_state; }
    [[nodiscard]] const QmuxCloseInfo& close_info() const noexcept { return m_close; }
    [[nodiscard]] cont::ConstSpan<u8> close_reason() const noexcept
    {
        return cont::ConstSpan<u8>(m_reason, m_reason_bytes);
    }
    [[nodiscard]] crypto::TlsError tls_error() const noexcept { return m_tls_error; }
    [[nodiscard]] const crypto::TlsSession& handshake() const noexcept { return m_tls; }
    [[nodiscard]] bool peer_parameters_known() const noexcept { return m_peer_known; }
    // Whether this connection is a WebSocket, from the moment its TLS agreed on http/1.1.
    [[nodiscard]] bool websocket() const noexcept
    {
        return m_framing == Framing::Upgrading || m_framing == Framing::Messages;
    }
    // Whether this connection is HTTP/2, from the moment its TLS agreed on h2.
    [[nodiscard]] bool http2() const noexcept { return m_framing == Framing::Http2; }
    // The subprotocol its opening handshake agreed on; empty until then.
    [[nodiscard]] cont::ConstSpan<u8> websocket_protocol() const noexcept;
    [[nodiscard]] const QuicTransportParameters& peer_parameters() const noexcept { return m_peer; }
    // The smoothed RTT from QX_PING exchanges; 0 before the first answer.
    [[nodiscard]] u64 rtt_ns() const noexcept { return m_rtt_ns; }
    [[nodiscard]] const QmuxCounters& counters() const noexcept { return m_counters; }

    [[nodiscard]] QuicStreamSet& streams() noexcept { return m_streams; }
    [[nodiscard]] const QuicStreamSet& streams() const noexcept { return m_streams; }
    [[nodiscard]] QuicDatagrams& datagrams() noexcept { return m_datagrams; }
    [[nodiscard]] const QuicDatagrams& datagrams() const noexcept { return m_datagrams; }

private:
    enum class Framing : u8
    {
        Records,   // a Size before each record, QMux's own
        Upgrading, // a WebSocket's opening handshake is running
        Messages,  // a record per WebSocket message
        Http2,     // HTTP/2 carrying WebSocket tunnels: no records
    };

    // What an opening handshake or a message in pieces has to remember.
    struct WebSocketState
    {
        usize scanned = 0U;              // how far the search for the end of the head went
        usize message_bytes = 0U;        // of m_in's first bytes, a message still arriving in fragments
        u16 close_code = kWsCloseNormal; // what this side's Close frame says
        u8 protocol = 0U;                // the configured subprotocol's index
        bool upgraded = false;
        bool fragmented = false; // a fragmented message has begun
        bool close_sent = false;
        bool pong_owed = false;
        u8 pong_bytes = 0U;
        u8 pong[kWsMaxControlBytes] = {};
        u8 accept[kWsAcceptChars] = {}; // a client's: what the server must answer
    };

    [[nodiscard]] bool tunneled() const noexcept { return m_config.tunnel != QmuxTunnel::None; }
    [[nodiscard]] bool is_client() const noexcept;
    // What the frames go through: this connection's TLS, or a tunnel's staging buffer.
    [[nodiscard]] usize stage_pending() const noexcept;
    [[nodiscard]] usize stage_capacity() const noexcept;
    [[nodiscard]] crypto::TlsError stage_send(cont::ConstSpan<u8> bytes) noexcept;
    void receive_tunnel(cont::ConstSpan<u8> bytes, u64 now_ns) noexcept;
    void advance(u64 now_ns) noexcept;
    void begin_early() noexcept;
    void settle_early() noexcept;
    void established(u64 now_ns) noexcept;
    void drain(u64 now_ns) noexcept;
    void parse_records(u64 now_ns) noexcept;
    [[nodiscard]] bool read_record(cont::ConstSpan<u8> record, bool early, u64 now_ns) noexcept;
    // ---- WebSocket ----
    [[nodiscard]] bool takes_websocket() const noexcept;
    void refuse_tls(crypto::TlsError error) noexcept;
    void begin_upgrade() noexcept;
    void read_upgrade(u64 now_ns) noexcept;
    void refuse_upgrade(u16 status) noexcept;
    void parse_frames(u64 now_ns) noexcept;
    [[nodiscard]] bool on_data_frame(const WsFrameHeader& header, usize payload_at, u64 now_ns) noexcept;
    void on_control_frame(WsOpcode opcode, cont::ConstSpan<u8> payload) noexcept;
    void fail_websocket(u16 status) noexcept;
    // Sends `payload_bytes` at `payload` as one frame, masked on a client; the kWsMaxHeaderBytes before `payload` are
    // its to write.
    [[nodiscard]] crypto::TlsError send_frame(WsOpcode opcode, u8* payload, usize payload_bytes) noexcept;
    [[nodiscard]] crypto::TlsError send_control(WsOpcode opcode, cont::ConstSpan<u8> payload) noexcept;
    // ---- HTTP/2 ----
    [[nodiscard]] static bool http2_config_valid(const QmuxConfig& config, bool client) noexcept;
    [[nodiscard]] bool takes_http2() const noexcept;
    [[nodiscard]] bool http2_pending() const noexcept; // output waits in HTTP/2
    [[nodiscard]] u64 http2_deadline() const noexcept; // HTTP/2's and its tunnels' next timer
    void release_http2() noexcept;
    void begin_http2(u64 now_ns) noexcept;
    void parse_http2(u64 now_ns) noexcept;
    void on_http2_event(const Http2Event& event, u64 now_ns) noexcept;
    void produce_http2(u64 now_ns) noexcept;
    // Moves HTTP/2's output into TLS as far as it has room; what TLS refused, None when it only had no room.
    [[nodiscard]] crypto::TlsError send_http2() noexcept;
    // send_http2(), a refusal failing the connection; false then.
    [[nodiscard]] bool flush_http2() noexcept;
    // Ends the tunnels and queues GOAWAY: the close every path of an HTTP/2 connection's end takes.
    void wind_down_http2() noexcept;
    void close_http2(QmuxCloseReason reason, u64 error_code) noexcept;
    void apply_peer_limits(const QuicTransportParameters& peer) noexcept;
    void fill_local(QuicTransportParameters& local) const noexcept;
    [[nodiscard]] bool early_open() const noexcept;
    [[nodiscard]] usize record_room() const noexcept;
    [[nodiscard]] crypto::TlsError send_record(cont::ConstSpan<u8> record) noexcept;
    [[nodiscard]] bool on_record(cont::ConstSpan<u8> record, bool early, u64 now_ns) noexcept;
    [[nodiscard]] bool on_parameters(cont::ConstSpan<u8> bytes) noexcept;
    [[nodiscard]] bool on_ping(u64 type, u64 sequence, u64 now_ns) noexcept;
    [[nodiscard]] bool on_frame(const QuicFrame& frame, bool early) noexcept;
    void produce(u64 now_ns) noexcept;
    [[nodiscard]] usize build_record(u64 now_ns) noexcept;
    [[nodiscard]] usize build_close() noexcept;
    [[nodiscard]] usize write_parameters(cont::Span<u8> out) noexcept;
    [[nodiscard]] usize write_ping(u64 type, u64 sequence, cont::Span<u8> out) noexcept;
    [[nodiscard]] bool frames_due(u64 now_ns) const noexcept;
    [[nodiscard]] u64 idle_deadline() const noexcept;
    [[nodiscard]] bool keepalive_on() const noexcept; // QX_PING keeps it alive, and none is owed
    void fail(QuicTransportError error) noexcept;
    void tls_failed(crypto::TlsError error) noexcept;
    void peer_shutdown() noexcept;
    void finish_sending() noexcept;
    void keep_reason(cont::ConstSpan<u8> reason) noexcept;

    QmuxConfig m_config;
    memory::IAllocator* m_allocator = nullptr;
    QmuxHttp2* m_h2 = nullptr; // an HTTP/2 connection's state, from the moment its TLS agreed on h2
    u8* m_stage = nullptr; // a tunnel's: frames not yet handed to the stream
    usize m_stage_head = 0U;
    usize m_stage_tail = 0U;
    QmuxState m_state = QmuxState::Idle;
    Framing m_framing = Framing::Records;
    bool m_ready = false;
    crypto::TlsSession m_tls;
    crypto::TlsError m_tls_error = crypto::TlsError::None;
    QuicStreamSet m_streams;
    QuicDatagrams m_datagrams;
    QuicTransportParameters m_peer;
    QuicTransportParameters m_remembered; // a client's: what its early frames keep to
    bool m_peer_known = false;
    bool m_early_sending = false; // a client's: frames run on m_remembered until its server's parameters or a refusal
    bool m_early_full = false;    // the ticket's allowance has no room for the next frame: the rest waits
    bool m_early_settled = false;
    bool m_parameters_sent = false;
    bool m_close_owed = false;
    bool m_send_shut = false;      // close_notify is queued (or the alert of a failed handshake)
    bool m_transport_over = false; // the transport ended: nothing is sent any more
    bool m_ping_owed = false;
    bool m_pong_owed = false;
    bool m_peer_pinged = false;
    u64 m_pong_sequence = 0U;
    u64 m_ping_next = 0U;
    u64 m_ping_sent_ns = 0U;
    u64 m_rtt_ns = 0U;
    u64 m_idle_ns = 0U;
    u64 m_handshake_deadline = kQmuxNoTimer;
    u64 m_close_deadline = kQmuxNoTimer;
    u64 m_last_activity = 0U;
    u64 m_frame_type = 0U;
    QmuxCloseInfo m_close;
    u8 m_reason[kQmuxCloseReasonBytes] = {};
    usize m_reason_bytes = 0U;
    QmuxCounters m_counters;
    usize m_in_fill = 0U;
    usize m_in_early = 0U; // of m_in's first bytes, those that came as early data
    WebSocketState m_ws;
    // Received plaintext: at most one partial record and its Size, or on a WebSocket a message in fragments and one
    // partial frame, which may be a control frame between them.
    u8 m_in[kQmuxMaxRecord + kWsMaxHeaderBytes + kWsMaxControlBytes] = {};
    u8 m_out[kQmuxMaxRecord + kWsMaxHeaderBytes] = {}; // the record being assembled, room for its head before it
};

} // namespace crd::net
