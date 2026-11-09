#pragma once

// crd-net -- WebTransport sessions over an HTTP/3 connection (4.2a; contract:
// docs/design/network-foundation.md#webtransport). Source: draft-ietf-webtrans-http3-17, and its legacy spelling.
//
//   sans-I/O  it sits on the Http3Connection and QuicStreamSet it was given and takes over Http3Connection::poll():
//             the owner calls poll() from its service turn, and what is not WebTransport comes out as an Http event.
//   sessions  a fixed pool searched by CONNECT stream ID. A server sees SessionRequest and answers with accept() or
//             reject(), nothing of the session being read until a 2xx; a client's connect() may open streams and send
//             datagrams before its SessionReady.
//   policy    an Origin that is not allowed answers 403 here and never reaches the application; a session beyond what
//             the connection allows is H3_REQUEST_REJECTED.
//   waiting   a stream naming a session not known yet stays unread in QUIC's buffers, counted against a bound; past it,
//             reset and stopped with WT_BUFFERED_STREAM_REJECTED. Datagrams wait in a few fixed slots, the oldest
//             dropped.
//   credit    on when both ends sent an initial limit and the session is not legacy. Limits count stream bodies, never
//             headers, and are granted again as the application reads and closes. What a reset never sent is given
//             back, so repeated resets cannot leak the window: the count follows the final size at both ends.
//   closing   close() sends WT_CLOSE_SESSION and FIN and ends every stream of the session with WT_SESSION_GONE at once.
//             A session error is a reset of the CONNECT stream with the draft's HTTP/3 code.
//   memory    everything comes from the injected allocator at init(); nothing allocates after it.
//   threads   the owning connection's.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/http_capsule.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
inline constexpr u32 kWtMaxSessions = 256U;
inline constexpr usize kWtMaxCloseMessage = 1024U; // section 6
inline constexpr usize kWtMaxExporterInput = 255U; // a label's or a context's bytes: their lengths are 8 bits
inline constexpr usize kWtMaxExporterContext = 8U + 1U + kWtMaxExporterInput + 1U + kWtMaxExporterInput;

struct WebTransportConfig
{
    u32 sessions = 4U;          // alive at once, also bounded by what the connection's SETTINGS allow
    u32 waiting_streams = 16U;  // streams held for a session not known yet (section 4.6)
    u32 waiting_datagrams = 8U; // and datagrams
    u32 datagram_bytes = 1200U; // the largest payload that waits, or that arrives as a capsule
    // A server's: the Origin values a session may come from, compared exactly. A request carrying any other answers 403
    // (section 3.2), so an empty list refuses every browser; a request with no Origin is not from one. The views must
    // outlive this.
    cont::ConstSpan<cont::ConstSpan<u8>> origins;
    bool any_origin = false; // takes every Origin: a deliberate choice, never a default
};

enum class WtEventKind : u8
{
    Http,            // not WebTransport: `http`
    SessionRequest,  // a server's: `session`, `fields`, `info`, `legacy`, `bytes` the offered list (wt_next_protocol)
    SessionReady,    // a client's 2xx: `fields`, `info`, `bytes` the protocol the server chose, if any
    SessionRefused,  // a client's: `status` of a response other than 2xx (3xx is never followed), or `error` of a reset
    SessionClosed,   // `code` and `bytes` the peer's WT_CLOSE_SESSION, or `error` of a CONNECT stream ended abruptly
    SessionDraining, // the peer asks to finish (WT_DRAIN_SESSION)
    SessionWritable, // a call that met a session limit can go again
    StreamOpened,    // `stream`: the peer opened one; peek() it now, as no StreamReadable covers what is already there
    StreamReadable,  // `stream`: bytes or the end arrived
    StreamWritable,  // `stream`: a write that took less than offered has room again
    StreamAborted,   // `stream`: the peer reset it, every reliable byte read: `code` if `has_code`, else `error`
    StreamStopped,   // `stream`: the peer's STOP_SENDING: `code` if `has_code`, else `error`
    Datagram,        // `bytes`
};

// What a WtEvent's spans view is valid until the next poll().
struct WtEvent
{
    WtEventKind kind = WtEventKind::Http;
    u64 session = 0U;
    u64 stream = 0U;
    u64 error = 0U; // an HTTP/3 code; 0 for none
    u32 code = 0U;  // the application's
    u16 status = 0U;
    bool has_code = false;
    bool legacy = false; // the session uses the legacy `webtransport` spelling, with no flow control
    cont::ConstSpan<u8> bytes;
    cont::ConstSpan<HttpField> fields;
    HttpMessageInfo info;
    Http3Event http;
};

// Walks a WT-Available-Protocols value from `at` (start at 0), an RFC 9651 List of Strings with parameters ignored:
// the next string, unescaped, in the first bytes of `out`, with its whole length in `bytes`. False at its end or at
// anything that is not such a list, for which the whole field counts as absent (section 3.3).
[[nodiscard]] bool wt_next_protocol(cont::ConstSpan<u8> list, usize& at, cont::Span<u8> out, usize& bytes) noexcept;

// Writes figure 6's context into `out` and returns its bytes, 0 for a label or context over kWtMaxExporterInput or an
// `out` too small.
[[nodiscard]] usize wt_exporter_context(u64 session, cont::ConstSpan<u8> label, cont::ConstSpan<u8> context,
                                        cont::Span<u8> out) noexcept;

class WebTransport
{
public:
    WebTransport() noexcept = default;
    ~WebTransport() noexcept;
    WebTransport(const WebTransport&) = delete;
    WebTransport& operator=(const WebTransport&) = delete;
    WebTransport(WebTransport&&) = delete;
    WebTransport& operator=(WebTransport&&) = delete;

    // False, holding nothing, for a configuration out of range, a connection not offering WebTransport or a refusal.
    // Both must outlive this. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, Http3Connection& http3, QuicStreamSet& streams,
                            const WebTransportConfig& config) noexcept;

    // The next thing that happened, false when nothing is left for now. Also sends what the sessions owe.
    [[nodiscard]] bool poll(WtEvent& event) noexcept;

    // A client's new session: `fields` is the whole extended CONNECT, whose :protocol picks the revision, with
    // any wt-available-protocols. `require_protocol`: a 2xx naming none of them ends the session with WT_ALPN_ERROR.
    [[nodiscard]] Http3Error connect(cont::ConstSpan<HttpField> fields, u64& session,
                                     bool require_protocol = false) noexcept;
    // A server's answers to a SessionRequest. `protocol`, if any, is one of the offered list and goes in WT-Protocol;
    // `fields` are the response's own beyond that and :status 200. Blocked: nothing happened, call again.
    [[nodiscard]] Http3Error accept(u64 session, cont::ConstSpan<HttpField> fields = {},
                                    cont::ConstSpan<u8> protocol = {}) noexcept;
    // `status` 300 to 599: 403 for a policy, 405 for a resource without WebTransport, 429 for a rate.
    [[nodiscard]] Http3Error reject(u64 session, u16 status) noexcept;

    // Ends the session: WT_CLOSE_SESSION (`message` cut at a UTF-8 boundary to kWtMaxCloseMessage bytes; Invalid when
    // it is not UTF-8) and a FIN. Its streams go at once and no event of it follows.
    [[nodiscard]] Http3Error close(u64 session, u32 code, cont::ConstSpan<u8> message = {}) noexcept;
    // Asks the peer to finish (WT_DRAIN_SESSION).
    [[nodiscard]] Http3Error drain(u64 session) noexcept;

    // NoStream: a limit refuses it now, SessionWritable or a retry tells when it no longer does.
    [[nodiscard]] Http3Error open_stream(u64 session, bool bidirectional, u64& stream) noexcept;
    // As much of `data` as the session's credit and the stream's buffer take (`taken`); `fin` ends it once all went.
    // Blocked with none taken: only the session's credit stands in the way.
    [[nodiscard]] Http3Error write(u64 stream, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept;
    // Sends nothing more, keeping the first `offset` bytes of the body reliably when the peer allows it.
    [[nodiscard]] Http3Error reset(u64 stream, u32 code, u64 offset = 0U) noexcept;
    [[nodiscard]] Http3Error stop(u64 stream, u32 code) noexcept;
    [[nodiscard]] QuicStreamRead peek(u64 stream) const noexcept;
    // Gives up the next `count` bytes peek() gave, and the session's credit for them.
    [[nodiscard]] Http3Error consume(u64 stream, usize count) noexcept;
    [[nodiscard]] Http3Error send_datagram(u64 session, cont::ConstSpan<u8> payload, u64 now_ns) noexcept;

    // Section 4.8: `out.size()` bytes from the connection's exporter, keyed to this session. NotReady for no such
    // session.
    [[nodiscard]] crypto::TlsError export_keying_material(const crypto::QuicHandshake& handshake, u64 session,
                                                          cont::ConstSpan<u8> label, cont::ConstSpan<u8> context,
                                                          cont::Span<u8> out) const noexcept;

    [[nodiscard]] u32 sessions_alive() const noexcept;
    [[nodiscard]] u32 streams_waiting() const noexcept { return m_waiting; }

private:
    static constexpr u32 kNone = ~u32{0};

    enum class State : u8
    {
        Free,
        Requested,  // a server's: the application has not answered
        Connecting, // a client's: no response yet
        Open,
        Closing, // this side's WT_CLOSE_SESSION has not found room yet
        Ending,  // the peer's WT_CLOSE_SESSION or FIN arrived: waiting for its FIN, or for room for this side's
    };

    // 0 unidirectional, 1 bidirectional, indexes the pairs below.
    struct Session
    {
        u64 id = 0U;
        u8* text = nullptr; // the offered list, a capsule being gathered, this side's close capsule
        u32 text_bytes = 0U;
        State state = State::Free;
        bool legacy = false;
        bool flow = false;
        bool require_protocol = false;
        bool pump = false;           // capsules may be readable
        bool announce = false;       // streams and datagrams wait to be reported
        bool peer_ended = false;     // Ending: the CONNECT stream's reading part ended
        bool close_seen = false;     // a WT_CLOSE_SESSION arrived
        bool credit_wanted = false;  // a call met a limit
        bool credit_arrived = false; // SessionWritable is owed
        u8 owed = 0U;
        HttpCapsuleReader reader;
        u64 peer_data = 0U; // what the peer lets this side send, and what it has
        u64 sent = 0U;
        u64 peer_streams[2] = {};
        u64 opened[2] = {};
        u64 local_data = 0U; // what this side lets the peer send, and what arrived and was read of it
        u64 seen = 0U;
        u64 read = 0U;
        u64 local_streams[2] = {};
        u64 peer_opened[2] = {};
        u64 peer_closed[2] = {};
        u64 data_blocked = ~u64{0}; // the limits a BLOCKED capsule went for: once per limit
        u64 streams_blocked[2] = {~u64{0}, ~u64{0}};
    };

    struct Stream
    {
        u64 id = 0U;
        u64 session_id = 0U;
        u64 seen = 0U;    // the body bytes that arrived
        u64 read = 0U;    // and were read
        u64 written = 0U; // the body bytes the session's credit paid for
        u32 session = kNone;
        u32 header = 0U;
        bool used = false;
        bool waiting = false;
        bool by_peer = false;
        bool read_over = false;
        bool send_over = false;
        bool closed = false; // counted as closed: one credit back
    };

    struct HeldDatagram
    {
        u64 session = 0U;
        u64 order = 0U;
        u32 bytes = 0U;
        bool used = false;
    };

    [[nodiscard]] u32 session_slot(u64 id) const noexcept;
    [[nodiscard]] u32 stream_slot(u64 id) const noexcept;
    [[nodiscard]] Session* find_session(u64 id) noexcept;
    [[nodiscard]] const Session* find_session(u64 id) const noexcept;
    [[nodiscard]] Stream* find_stream(u64 id) noexcept;
    [[nodiscard]] const Stream* find_stream(u64 id) const noexcept;
    [[nodiscard]] Stream* event_stream(const Http3Event& in) noexcept;
    [[nodiscard]] u32 session_index(const Session& session) const noexcept;
    [[nodiscard]] bool admits(bool legacy) const noexcept;
    [[nodiscard]] static bool session_unanswered(const Session& session) noexcept;
    [[nodiscard]] Session* claim_session(u64 id, bool legacy) noexcept;
    void free_session(Session& session) noexcept;

    // Events from below.
    [[nodiscard]] bool on_http3(const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool on_session_event(Session& session, const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool on_request(const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool on_response(Session& session, const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool on_datagram(const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool on_stream_opened(const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool on_stream_readable(const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool on_stream_aborted(const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool on_stream_stopped(const Http3Event& in, WtEvent& event) noexcept;
    [[nodiscard]] bool origin_allowed(cont::ConstSpan<HttpField> fields) const noexcept;
    [[nodiscard]] bool retain_offer(Session& session, cont::ConstSpan<HttpField> fields) noexcept;
    [[nodiscard]] bool read_choice(Session& session, cont::ConstSpan<HttpField> fields, usize& chosen) noexcept;

    // Sessions and their streams.
    [[nodiscard]] bool service(WtEvent& event) noexcept;
    [[nodiscard]] bool read_capsules(Session& session, WtEvent& event) noexcept;
    [[nodiscard]] bool on_capsule(Session& session, const HttpCapsuleChunk& chunk, WtEvent& event) noexcept;
    [[nodiscard]] bool on_complete_capsule(Session& session, u64 type, usize size, WtEvent& event) noexcept;
    [[nodiscard]] bool on_flow_capsule(Session& session, u64 type, u64 value, WtEvent& event) noexcept;
    [[nodiscard]] bool on_close_capsule(Session& session, usize size, WtEvent& event) noexcept;
    [[nodiscard]] bool announce(Session& session, WtEvent& event) noexcept;
    [[nodiscard]] bool attach(Session& session, Stream& stream, WtEvent& event) noexcept;
    // A session that ended abruptly: `error` reported, the CONNECT stream reset with `cancel_code`, all of it gone.
    [[nodiscard]] bool end_session(Session& session, u64 error, u64 cancel_code, WtEvent& event) noexcept;
    // Answers a request with only a status and reads no more of it.
    [[nodiscard]] Http3Error refuse(u64 id, u16 status) noexcept;
    void hold_stream(Stream& stream) noexcept;
    void release_waiting(u64 session) noexcept;
    void end_streams(Session& session) noexcept;
    void hold_datagram(u64 session, cont::ConstSpan<u8> payload) noexcept;
    void drop_held(u64 session) noexcept;
    [[nodiscard]] bool account(Session& session, Stream& stream, u64 end) noexcept;
    void give_back(Session& session, Stream& stream, u64 final_size) noexcept;
    void settle(Session& session, Stream& stream) noexcept;
    void refill(Session& session) noexcept;
    void flush(Session& session) noexcept;
    [[nodiscard]] bool write_capsule(Session& session, cont::ConstSpan<u8> bytes, bool fin) noexcept;
    void finish_close(Session& session) noexcept;
    void settle_ending(Session& session) noexcept;

    memory::IAllocator* m_allocator = nullptr;
    Http3Connection* m_http3 = nullptr;
    QuicStreamSet* m_streams = nullptr;
    WebTransportConfig m_config;
    Http3WebTransportSettings m_offer;
    Session* m_sessions = nullptr;
    Stream* m_records = nullptr;
    u32 m_record_count = 0U;
    HeldDatagram* m_held = nullptr;
    u8* m_held_bytes = nullptr; // datagram_bytes per held slot
    u8* m_text = nullptr;       // every session's text
    usize m_text_each = 0U;
    u32 m_waiting = 0U;
    u32 m_reported = kNone; // the held slot the last event viewed
    u64 m_held_order = 0U;
    crypto::QuicSide m_side = crypto::QuicSide::Client;
};

} // namespace crd::net
