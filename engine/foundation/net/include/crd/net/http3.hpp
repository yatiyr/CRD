#pragma once

// crd-net -- an HTTP/3 connection over a QUIC connection's streams and datagrams (4.2a; contract:
// docs/design/network-foundation.md#http3 and #webtransport). Sources: RFC 9114, RFC 9204 section 4.2, RFC 9220, RFC
// 9297 section 2 and draft-ietf-webtrans-http3-17.
//
//   sans-I/O  it reads and writes the QuicStreamSet (and QuicDatagrams) it was given and never touches a socket or a
//             clock; the owner calls poll() from its service turn and closes the QUIC connection with error() when that
//             is set. One thread at a time.
//   streams   this side opens its control and two QPACK streams at start() and sends SETTINGS without waiting (RFC 9114
//             section 7.2.4.2). The peer's are found by their type; a second one, or one closing, is a connection
//             error. Per-stream state sits beside the QUIC slot of the same index, so looking a stream up costs QUIC's
//             search.
//   memory    a frame is parsed where it lies in the QUIC receive buffer: DATA passes through peek_body() without a
//             copy, and a HEADERS frame stays unconsumed until QPACK decodes it, which is where a blocked section
//             waits, so blocking costs no extra storage. A HEADERS frame longer than the section limit is refused on
//             its length, before any of it is read; a section over the limit answers 431 to a request by itself, and
//             neither reaches the application. The fields of an event view one scratch per connection, valid until the
//             next poll().
//   errors    a connection error stays in error() and stops everything. A stream error resets the stream and is
//             reported as Aborted, except a request the application never saw.
//   push      never offered: no MAX_PUSH_ID is sent, so any promise or push stream from a server is H3_ID_ERROR.
//   0-RTT     a client resuming passes the settings it remembered and checks the server's own against them (section
//             7.2.4.2); after a rejection the owner builds a new Http3Connection, because the streams were all reset. A
//             server binds its settings into the ticket with early_context().
//   grease    every SETTINGS carries a reserved identifier, the control stream a reserved frame and, when the peer
//             allows a fourth stream, a reserved stream type goes once (section 7.2.8).
//   WebTrans- with the offer made, a stream's first bytes say whether it is HTTP or a WebTransport stream (uni type
//   port      0x54; 0x41 first on a bidirectional stream) and of which session. This class only classifies and reports:
//             the bytes are read and written through the QuicStreamSet and sessions belong to the layer above. A server
//             touches no bidirectional or WebTransport stream before the client's SETTINGS: they wait unread in QUIC's
//             buffers (draft section 7.1).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/http_fields.hpp>
#include <crd/net/http_message.hpp>
#include <crd/net/qpack.hpp>
#include <crd/net/quic_datagram.hpp>
#include <crd/net/quic_stream_set.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
// RFC 9114 section 8.1 and RFC 9297 section 5.2.
inline constexpr u64 kH3NoError = 0x100U;
inline constexpr u64 kH3GeneralProtocolError = 0x101U;
inline constexpr u64 kH3InternalError = 0x102U;
inline constexpr u64 kH3StreamCreationError = 0x103U;
inline constexpr u64 kH3ClosedCriticalStream = 0x104U;
inline constexpr u64 kH3FrameUnexpected = 0x105U;
inline constexpr u64 kH3FrameError = 0x106U;
inline constexpr u64 kH3IdError = 0x108U;
inline constexpr u64 kH3SettingsError = 0x109U;
inline constexpr u64 kH3MissingSettings = 0x10AU;
inline constexpr u64 kH3RequestRejected = 0x10BU;
inline constexpr u64 kH3RequestCancelled = 0x10CU;
inline constexpr u64 kH3RequestIncomplete = 0x10DU;
inline constexpr u64 kH3MessageError = 0x10EU;
inline constexpr u64 kH3ConnectError = 0x10FU;
inline constexpr u64 kH3DatagramError = 0x33U;

// draft-ietf-webtrans-http3-17 section 9.5.
inline constexpr u64 kWtBufferedStreamRejected = 0x3994BD84U;
inline constexpr u64 kWtSessionGone = 0x170D7B68U;
inline constexpr u64 kWtFlowControlError = 0x045D4487U;
inline constexpr u64 kWtAlpnError = 0x0817B3DDU;
inline constexpr u64 kWtRequirementsNotMet = 0x212C0D48U;
inline constexpr u64 kWtApplicationErrorFirst = 0x52E4A40FA8DBULL;
inline constexpr u64 kWtApplicationErrorLast = 0x52E5AC983162ULL;
inline constexpr u64 kWtMaxStreams = u64{1} << 60U; // a stream limit's largest value (section 5.6.2)

// Section 4.4: a WebTransport application's 32-bit code as the HTTP/3 code on the wire, skipping the reserved points.
[[nodiscard]] constexpr u64 wt_http_code(u32 application) noexcept
{
    return kWtApplicationErrorFirst + u64{application} + (u64{application} / 0x1EU);
}

// The reverse: false for a code outside the range or on a reserved point, a reset with no application code.
[[nodiscard]] constexpr bool wt_application_code(u64 http, u32& application) noexcept
{
    if (http < kWtApplicationErrorFirst || http > kWtApplicationErrorLast || ((http - 0x21U) % 0x1FU) == 0U)
    {
        return false;
    }
    const u64 shifted = http - kWtApplicationErrorFirst;
    application = static_cast<u32>(shifted - (shifted / 0x1FU));
    return true;
}

// Whether an extended CONNECT's :protocol names WebTransport, and in which revision.
[[nodiscard]] bool http3_wt_token(cont::ConstSpan<u8> protocol, bool& legacy) noexcept;

inline constexpr u32 kHttp3MinFieldBytes = 1024U;
inline constexpr u32 kHttp3MaxFieldBytes = 1U << 20U;
// A HEADERS frame's largest payload for a section of `field_bytes`, and the frame header before it at most.
inline constexpr u32 kHttp3SectionOverhead = 2U * static_cast<u32>(kHttpMaxIntegerBytes);
inline constexpr u32 kHttp3FrameHeaderBytes = 16U;
inline constexpr usize kHttp3EarlyContextBytes = 14U;
inline constexpr u64 kHttp3NoFieldLimit = ~u64{0}; // no SETTINGS_MAX_FIELD_SECTION_SIZE was sent

// A side's WebTransport settings (draft-ietf-webtrans-http3-17 section 9.2), and in Http3Config its offer. Both
// revisions go in one SETTINGS and each CONNECT says which it uses (docs/design/network-foundation.md#webtransport).
// Never remembered for 0-RTT.
struct Http3WebTransportSettings
{
    bool enabled = false; // SETTINGS_WT_ENABLED; in an offer, this connection takes WebTransport at all
    u64 max_streams_uni = 0U;
    u64 max_streams_bidi = 0U;
    u64 max_data = 0U;
    u64 legacy_sessions = 0U; // the legacy `webtransport` spelling's session count: 0 does not offer it
};

// What one side tells the other in SETTINGS, with RFC 9114's defaults.
struct Http3Settings
{
    u64 qpack_max_table_capacity = 0U;
    u64 qpack_blocked_streams = 0U;
    u64 max_field_section_size = kHttp3NoFieldLimit;
    bool enable_connect_protocol = false;
    bool h3_datagram = false;
    Http3WebTransportSettings wt;
};

struct Http3Config
{
    crypto::QuicSide side = crypto::QuicSide::Client;
    // The largest field section this side takes, as RFC 9114 section 4.2.2 counts it, and sends in SETTINGS; the QUIC
    // receive buffer and the first stream window hold such a section with its frame header. Also the largest it sends.
    u32 field_bytes = 16384U;
    QpackDecoderConfig decoder;
    QpackEncoderConfig encoder;
    u32 datagram_bytes = 1500U; // the largest HTTP datagram sent
    bool connect_protocol = false;
    bool datagrams = false;
    // Needs datagrams and QuicStreamLimits::reset_stream_at, and on a server connect_protocol too.
    Http3WebTransportSettings webtransport;
    // A client resuming: the server's settings it stored with the ticket, in force until the server's arrive.
    const Http3Settings* remembered = nullptr;
    u64 grease = 0U; // varies the reserved values sent
};

enum class Http3EventKind : u8
{
    Settings, // the peer's SETTINGS arrived: peer_settings()
    GoAway,   // `id` is the GOAWAY's
    Headers,  // a request (server) or a final response (client): `fields`, `info`
    Interim,  // a client's 1xx response: `fields`, `info`
    Body,     // DATA is readable: peek_body() until it is empty
    Trailers, // `fields`
    Finished, // the peer's message ended well
    Aborted,  // the stream ended with `error`: the peer's reset, a malformed message or a rejected request
    Stopped,  // the peer's STOP_SENDING, `error`: its response part or request part stops
    Writable, // a call that returned Blocked can go again
    Datagram, // `payload`, valid until the next poll(); with WebTransport offered also for a CONNECT not yet read
    // WebTransport streams: `id` is the session, `stream` the stream, `header` the bytes its header took. Each goes
    // once per arrival: read with QuicStreamSet::peek() until it is empty.
    WtStream,   // the peer opened one
    WtReadable, // bytes or the end arrived
    WtWritable, // a write that took less than offered has room again
    WtAborted,  // the peer reset it, after every reliable byte was read: `error` (wt_application_code), `final_size`
    WtStopped,  // the peer's STOP_SENDING: `error` (wt_application_code)
};

struct Http3Event
{
    Http3EventKind kind = Http3EventKind::Settings;
    u64 stream = 0U;
    u64 id = 0U;
    u64 error = 0U;
    cont::ConstSpan<HttpField> fields;
    HttpMessageInfo info;
    cont::ConstSpan<u8> payload;
    u64 final_size = 0U; // WtAborted and WtStopped: of the direction's whole stream, its header included
    u32 header = 0U;
    u32 slot = 0U;       // WebTransport streams: QuicStreamSet::slot_of's answer when the event happened
    bool early = false;  // the section or datagram came in 0-RTT: replayable
    bool unread = false; // a Datagram for a stream whose request has not been read
};

enum class Http3Error : u8
{
    None,
    Closed,    // a connection error happened: error()
    NotReady,  // before start(), or Extended CONNECT before the peer's SETTINGS allow it, or datagrams not negotiated
    GoingAway, // the peer's GOAWAY forbids a new request
    NoStream,  // the QUIC limits refuse a stream now
    Blocked,   // no room: wait for Writable
    TooLarge,  // above the peer's field section limit or this side's
    Invalid,   // a malformed message, or a call out of sequence for the stream
    Unknown,   // no such stream, or a stream ended
};

struct Http3Body
{
    cont::ConstSpan<u8> data;
    usize early_bytes = 0U; // how many of them came in 0-RTT
};

class Http3Connection
{
public:
    Http3Connection() noexcept = default;
    ~Http3Connection() noexcept;
    Http3Connection(const Http3Connection&) = delete;
    Http3Connection& operator=(const Http3Connection&) = delete;
    Http3Connection(Http3Connection&&) = delete;
    Http3Connection& operator=(Http3Connection&&) = delete;

    // False, holding nothing, for a configuration out of range, stream limits that cannot carry it (three
    // unidirectional streams each way, a receive window that holds a section) or a refusal. Both must outlive this.
    // `datagrams` may be null when config.datagrams is false. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, QuicStreamSet& streams, QuicDatagrams* datagrams,
                            const Http3Config& config) noexcept;

    // Opens this side's streams and sends SETTINGS (once the peer's transport parameters allow the streams: a client
    // after the handshake or on 0-RTT, a server when its connection is established). False sets error().
    [[nodiscard]] bool start() noexcept;

    // The next thing that happened, false when nothing is left for now. Also moves QPACK's instructions out.
    [[nodiscard]] bool poll(Http3Event& event) noexcept;

    // A client's new request; `id` is its stream.
    [[nodiscard]] Http3Error open_request(cont::ConstSpan<HttpField> fields, bool fin, u64& id) noexcept;
    // A server's response (a 1xx any number of times first).
    [[nodiscard]] Http3Error send_response(u64 id, cont::ConstSpan<HttpField> fields, bool fin) noexcept;
    // One DATA frame of as much of `data` as fits (`taken`); `fin` ends the message when all of it went.
    [[nodiscard]] Http3Error send_body(u64 id, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept;
    // Trailers end the message.
    [[nodiscard]] Http3Error send_trailers(u64 id, cont::ConstSpan<HttpField> fields) noexcept;
    // Ends this side's message.
    [[nodiscard]] Http3Error finish(u64 id) noexcept;
    // Abandons both directions of a stream with `error`.
    [[nodiscard]] Http3Error cancel(u64 id, u64 error) noexcept;

    // The DATA bytes readable on `id` (Body events say when): at most one frame's, up to the end of the ring.
    [[nodiscard]] Http3Body peek_body(u64 id) const noexcept;
    [[nodiscard]] Http3Error consume_body(u64 id, usize count) noexcept;

    // An HTTP datagram of `id`, an extended CONNECT's stream, with the send part open: RFC 9297 section 2.
    [[nodiscard]] Http3Error send_datagram(u64 id, cont::ConstSpan<u8> payload, u64 now_ns) noexcept;

    // Whether a session of that revision can be established here now: draft section 3.1's settings and transport
    // parameters from the peer, on this connection.
    [[nodiscard]] bool wt_ready(bool legacy) const noexcept;
    // Opens a WebTransport stream of `session` (a CONNECT stream carrying extended CONNECT semantics) and writes its
    // header. The body then goes through QuicStreamSet::write().
    [[nodiscard]] Http3Error open_wt_stream(u64 session, bool bidirectional, u64& id) noexcept;
    // Resets the sending part with the application's `code`, keeping `offset` bytes of body and the header if this side
    // sent one (section 4.4); a peer without RESET_STREAM_AT gets a plain reset, and a non-zero `offset` is Invalid.
    // Again to lower the offset.
    [[nodiscard]] Http3Error reset_wt(u64 id, u32 code, u64 offset = 0U) noexcept;
    // Asks the peer to stop sending, with the application's `code`.
    [[nodiscard]] Http3Error stop_wt(u64 id, u32 code) noexcept;
    // Ends both directions with a protocol code: kWtSessionGone or kWtBufferedStreamRejected.
    [[nodiscard]] Http3Error abort_wt(u64 id, u64 http_code) noexcept;
    // The bytes the stream's header took; 0 for no such WebTransport stream.
    [[nodiscard]] u32 wt_header(u64 id) const noexcept;
    // A server's: whether `id` can still turn out to be a session: its stream has not come, or its first bytes or its
    // request have not been read. False once it is anything else, and always for a client, which opened its own.
    [[nodiscard]] bool wt_session_pending(u64 id) const noexcept;

    // Tells the peer no new requests will be handled. `immediate` false allows any it has sent or is sending (a first,
    // gentle GOAWAY); true names the first request not handled. A client's value is a push ID. False when there was no
    // room, retry after a poll().
    [[nodiscard]] bool goaway(bool immediate) noexcept;

    [[nodiscard]] const Http3Config& config() const noexcept { return m_config; }
    [[nodiscard]] u64 error() const noexcept { return m_error; } // 0 for none
    [[nodiscard]] bool started() const noexcept { return m_started; }
    [[nodiscard]] bool settings_known() const noexcept { return m_peer_known; }
    [[nodiscard]] const Http3Settings& peer_settings() const noexcept { return m_peer; }
    [[nodiscard]] bool goaway_received() const noexcept { return m_goaway_received; }
    [[nodiscard]] const QpackDecoder& qpack_decoder() const noexcept { return m_decoder; }
    [[nodiscard]] const QpackEncoder& qpack_encoder() const noexcept { return m_encoder; }

    // A server's: the bytes for QuicConnectionConfig::early_context, kHttp3EarlyContextBytes at most, so a ticket from
    // a server with other settings cannot carry 0-RTT here (RFC 9114 section 7.2.4.2). The bytes written.
    [[nodiscard]] static usize early_context(const Http3Config& config, cont::Span<u8> out) noexcept;

private:
    enum class Kind : u8
    {
        Unused,
        UniType, // the peer's: its type has not arrived
        Control,
        QpackEncoder, // the peer's, carrying instructions for this side's decoder
        QpackDecoder,
        Discard, // of a reserved or unknown type
        Request,
        LocalUni, // this side's
        First,    // WebTransport offered: a bidirectional stream the peer opened, its first varint not read
        WtHead,   // the type or signal read, the session ID not yet
        WtData,
    };

    enum class Read : u8
    {
        Head, // no final HEADERS yet
        Body, // DATA and possibly trailers
        Tail, // after trailers
        Over, // nothing more is reported
    };

    enum class Frame : u8
    {
        None,
        Data,
        Skip,
        Headers,
    };

    enum class Send : u8
    {
        Idle,
        Message,
        Over,
    };

    // The state of the stream of one QUIC slot. `used` and `id` tell whether it still is that stream's.
    struct Record
    {
        u64 id = 0U;
        u64 frame_left = 0U;   // DATA or a skipped frame's bytes left
        u64 content_left = 0U; // what content-length says is still to come
        u32 next = 0U;         // the ready list
        u32 section = 0U;      // a HEADERS frame's payload bytes
        u64 wt_session = 0U;
        u32 wt_header = 0U; // the WebTransport header's bytes so far
        Kind kind = Kind::Unused;
        Read read = Read::Head;
        Send send = Send::Idle;
        Frame frame = Frame::None;
        bool used = false;
        bool queued = false;
        bool blocked = false;   // QPACK holds its section
        bool signalled = false; // a Body or WtReadable event went and nothing was consumed or arrived since
        bool has_length = false;
        bool head = false;      // a client's HEAD
        bool tunnel = false;    // a CONNECT accepted: only DATA from here
        bool datagrams = false; // an extended CONNECT: HTTP datagrams have meaning
        bool rejected = false;  // Aborted owed: after a GOAWAY
    };

    [[nodiscard]] Record* find(u64 id) noexcept;
    [[nodiscard]] const Record* find(u64 id) const noexcept;
    [[nodiscard]] Record& claim(u32 slot, u64 id) noexcept;
    void enqueue(u32 slot) noexcept;
    [[nodiscard]] u32 slot_index(const Record& record) const noexcept;
    bool fail(u64 code) noexcept;

    // Transport events.
    [[nodiscard]] bool take_event_bit(Http3Event& event) noexcept;
    void opened(Record& record) noexcept;
    // A server's bidirectional stream is a request: GOAWAY's refusal, and the count a GOAWAY names.
    void admit_request(Record& record) noexcept;
    [[nodiscard]] bool peer_reset(Record& record, u64 code, Http3Event& event) noexcept;
    [[nodiscard]] bool peer_stop(Record& record, u64 code, Http3Event& event) noexcept;

    // Reading.
    [[nodiscard]] bool advance(Record& record, Http3Event& event) noexcept;
    [[nodiscard]] bool advance_request(Record& record, Http3Event& event) noexcept;
    [[nodiscard]] bool request_frame(Record& record, Http3Event& event, bool& more) noexcept;
    [[nodiscard]] bool request_ended(Record& record, Http3Event& event) noexcept;
    [[nodiscard]] bool read_section(Record& record, Http3Event& event, bool& more) noexcept;
    // A section too large for this side: a request the application never saw is answered 431 from here.
    [[nodiscard]] bool refuse_request(Record& record) noexcept;
    [[nodiscard]] bool deliver_section(Record& record, usize count, bool early, Http3Event& event) noexcept;
    [[nodiscard]] bool skip_frame(Record& record, bool& more) noexcept;
    void read_uni_type(Record& record) noexcept;
    [[nodiscard]] bool read_first(Record& record, Http3Event& event) noexcept;
    [[nodiscard]] bool read_wt_head(Record& record, Http3Event& event) noexcept;
    [[nodiscard]] bool read_wt_data(Record& record, Http3Event& event) noexcept;
    // Whether a RESET_STREAM_AT's reliable bytes mean something on this stream (only WebTransport gives them a
    // meaning).
    [[nodiscard]] bool keeps_reliable(const Record& record) const noexcept;
    [[nodiscard]] bool held_for_settings() const noexcept;
    [[nodiscard]] u64 sent_header(const Record& record) const noexcept;
    void read_qpack(Record& record) noexcept;
    [[nodiscard]] bool read_control(Record& record, Http3Event& event) noexcept;
    [[nodiscard]] bool read_settings(cont::ConstSpan<u8> payload) noexcept;
    void apply_goaway(u64 value) noexcept;
    [[nodiscard]] bool abort_stream(Record& record, u64 code, bool report, Http3Event& event) noexcept;
    // Gives up what the stream still holds, so its slot can go.
    void drop_input(const Record& record) noexcept;
    void wake_unblocked() noexcept;
    [[nodiscard]] bool take_datagram(Http3Event& event) noexcept;

    // Writing.
    [[nodiscard]] bool write_all(u64 id, cont::ConstSpan<u8> bytes, bool fin) noexcept;
    [[nodiscard]] Http3Error send_section(Record& record, cont::ConstSpan<HttpField> fields, bool fin) noexcept;
    [[nodiscard]] bool open_uni(u64 type, u64& id) noexcept;
    [[nodiscard]] usize write_settings(cont::Span<u8> out) const noexcept;
    void flush_instructions() noexcept;

    memory::IAllocator* m_allocator = nullptr;
    QuicStreamSet* m_streams = nullptr;
    QuicDatagrams* m_datagrams = nullptr;
    Http3Config m_config;
    Http3Settings m_peer;
    Http3Settings m_remembered;
    QpackDecoder m_decoder;
    QpackEncoder m_encoder;
    Record* m_records = nullptr;
    u32 m_record_count = 0U;
    u32 m_ready_head = 0U; // m_record_count for none
    u32 m_ready_tail = 0U;
    u8* m_scratch = nullptr; // a frame made contiguous, or one being written
    usize m_scratch_bytes = 0U;
    u8* m_text = nullptr; // decoded field text
    HttpField* m_fields = nullptr;
    usize m_field_slots = 0U;
    QuicStreamEvent m_current;
    u64 m_control = 0U;
    u64 m_encoder_stream = 0U;
    u64 m_decoder_stream = 0U;
    u64 m_error = 0U;
    u64 m_goaway_sent = 0U;
    u64 m_goaway_peer = 0U;
    u64 m_max_push_id = 0U;
    u64 m_next_request = 0U; // a server's: the lowest stream it has not seen a request on
    bool m_started = false;
    bool m_peer_known = false;
    bool m_has_remembered = false;
    bool m_goaway_was_sent = false;
    bool m_goaway_received = false;
    bool m_has_max_push_id = false;
    bool m_peer_control = false;
    bool m_peer_encoder = false;
    bool m_peer_decoder = false;
    bool m_settings_seen = false;
    bool m_datagram_held = false;
};

} // namespace crd::net
