#pragma once

// crd-net -- an HTTP/2 connection over a byte stream (4.2a; contract: docs/design/network-foundation.md#http2).
// Sources: RFC 9113, RFC 8441 section 3, RFC 9218 section 2.1 and RFC 7541.
//
//   sans-I/O  the owner hands poll() the bytes it read and writes what output() holds. It never touches a socket; its
//             only clock is the time poll() is given. One thread at a time.
//   input     poll() reads from the front of its input and says how much it took; the owner keeps the rest and offers
//             it again with what arrives next. It takes nothing of a frame it cannot act on yet, so the owner's buffer
//             must hold input_bytes(). DATA and field block fragments are taken as they arrive, any amount at a time.
//   memory    a DATA frame is read where it lies: a Body event's payload views the input. Streams cannot wait in the
//             input behind each other, so the application takes a payload when it is reported and gives the credit
//             back with consume_body(), which is what bounds it. One field block at a time is gathered, at most twice
//             `field_bytes`. Everything comes from the injected allocator at init().
//   output    every frame written waits in one buffer. Reading stops while the room for what a frame may answer is
//             short, so a peer that does not read cannot grow anything.
//   streams   only clients open them, in ascending order, so the open ones are a sorted array searched by halving: an
//             identifier costs no hash an attacker can aim at. Push is never offered: PUSH_PROMISE is a connection
//             error.
//   closed    the last streams closed are remembered, with how (section 5.1): a frame for one this side reset is
//             processed minimally and dropped, for one the peer reset it is a stream error STREAM_CLOSED, and after both
//             ends finished it is a connection error STREAM_CLOSED. An identifier that was never opened is a connection
//             error, also among the ones skipped. Past what is remembered it is taken for one the peer reset.
//   sections  a block is decoded whole, so the table stays in step, whatever becomes of the stream. A section over
//             `field_bytes` answers 431 to a request by itself; a block over the bound is ENHANCE_YOUR_CALM. A
//             malformed message is a stream error PROTOCOL_ERROR and a request reaches the application only when it
//             passes.
//   floods    two gates, charged per frame: `resets` for every stream reset the peer asked for or provoked, `control`
//             for SETTINGS, PING, PRIORITY, empty frames that end nothing and tiny WINDOW_UPDATEs. Past either the
//             connection ends with ENHANCE_YOUR_CALM.
//   errors    a connection error writes GOAWAY, stays in error() and stops everything; the owner sends what is left in
//             output() and closes. A stream error resets the stream and is reported as Aborted, except for a request
//             the application never saw.
//   0-RTT     an owner reading TLS early data says how much of the input came in it (early_input()); a request whose
//             HEADERS frame starts there is reported early, replayable (RFC 8470 section 3).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/hpack.hpp>
#include <crd/net/http_fields.hpp>
#include <crd/net/http_message.hpp>
#include <crd/net/quota.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
// RFC 9113 section 7.
inline constexpr u32 kH2NoError = 0x0U;
inline constexpr u32 kH2ProtocolError = 0x1U;
inline constexpr u32 kH2InternalError = 0x2U;
inline constexpr u32 kH2FlowControlError = 0x3U;
inline constexpr u32 kH2SettingsTimeout = 0x4U;
inline constexpr u32 kH2StreamClosed = 0x5U;
inline constexpr u32 kH2FrameSizeError = 0x6U;
inline constexpr u32 kH2RefusedStream = 0x7U;
inline constexpr u32 kH2Cancel = 0x8U;
inline constexpr u32 kH2CompressionError = 0x9U;
inline constexpr u32 kH2ConnectError = 0xAU;
inline constexpr u32 kH2EnhanceYourCalm = 0xBU;

inline constexpr usize kHttp2PrefaceBytes = 24U;
inline constexpr usize kHttp2FrameHeaderBytes = 9U;
inline constexpr u32 kHttp2InitialWindow = 65535U;
inline constexpr u32 kHttp2MaxWindow = 0x7FFFFFFFU;
inline constexpr u32 kHttp2MinFrameBytes = 16384U;
inline constexpr u32 kHttp2MaxFrameBytes = 16777215U;
inline constexpr u32 kHttp2MaxStreamId = 0x7FFFFFFFU;
inline constexpr u32 kHttp2MaxStreams = 4096U;
inline constexpr u32 kHttp2MinFieldBytes = 1024U;
inline constexpr u32 kHttp2MaxFieldBytes = 1U << 20U;
inline constexpr usize kHttp2MinOutputBytes = 1024U;
inline constexpr u32 kHttp2Unlimited = ~u32{0}; // a setting the peer never limited

enum class Http2Side : u8
{
    Client,
    Server,
};

// What the peer said in SETTINGS, with RFC 9113's defaults.
struct Http2Settings
{
    u32 header_table_size = kHpackDefaultTableSize;
    u32 max_concurrent_streams = kHttp2Unlimited;
    u32 initial_window = kHttp2InitialWindow;
    u32 max_frame_bytes = kHttp2MinFrameBytes;
    u32 max_field_section_size = kHttp2Unlimited;
    bool enable_connect_protocol = false;
};

struct Http2Config
{
    Http2Side side = Http2Side::Client;
    u32 field_bytes = 16384U;  // the largest field section this side takes or sends, as section 6.5.2 counts it
    u32 streams = 100U;        // open streams at most, sent as SETTINGS_MAX_CONCURRENT_STREAMS
    u32 stream_window = 1U << 20U;      // SETTINGS_INITIAL_WINDOW_SIZE: what the application can hold per stream
    u32 connection_window = 1U << 24U;  // at least kHttp2InitialWindow
    u32 frame_bytes = kHttp2MinFrameBytes; // the largest frame this side takes
    u32 table_bytes = kHpackDefaultTableSize;   // the decoder's table, sent as SETTINGS_HEADER_TABLE_SIZE
    u32 encoder_table_bytes = kHpackDefaultTableSize; // the most the encoder uses, whatever the peer allows
    usize output_bytes = 64U * 1024U;
    bool connect_protocol = false;  // a server's SETTINGS_ENABLE_CONNECT_PROTOCOL: takes extended CONNECT
    u64 settings_timeout_ns = 10'000'000'000U; // SETTINGS_TIMEOUT when the peer has not acknowledged by then
    GateRule resets{64U, 4'000'000'000U};
    GateRule control{256U, 4'000'000'000U};
};

enum class Http2EventKind : u8
{
    Settings, // the peer's SETTINGS arrived: peer_settings()
    GoAway,   // `stream` is the last one it says it handled, `error` its code
    Headers,  // a request (server) or a final response (client): `fields`, `info`
    Interim,  // a client's 1xx response: `fields`, `info`
    Body,     // `payload`, valid until the next poll(): hand it back with consume_body()
    Trailers, // `fields`
    Finished, // the peer's message ended well
    Aborted,  // the stream ended with `error`: the peer's reset, a malformed message or a request cut by GOAWAY
    Writable, // a call that returned Blocked can go again
};

// The views live until the next poll().
struct Http2Event
{
    Http2EventKind kind = Http2EventKind::Settings;
    u32 stream = 0U;
    u32 error = 0U;
    cont::ConstSpan<HttpField> fields;
    HttpMessageInfo info;
    cont::ConstSpan<u8> payload;
    bool early = false; // a request whose HEADERS frame started in TLS early data: replayable
};

enum class Http2Error : u8
{
    None,
    Closed,    // a connection error happened: error()
    NotReady,  // extended CONNECT before the peer's SETTINGS allow it
    GoingAway, // the peer's GOAWAY forbids a new request
    NoStream,  // the peer's limit, this side's or the identifiers are spent
    Blocked,   // no credit or no room: wait for Writable
    TooLarge,  // above the peer's field section limit, this side's or the output buffer
    Invalid,   // a malformed message, or a call out of sequence for the stream
    Unknown,   // no such stream, or a stream ended
};

class Http2Connection
{
public:
    Http2Connection() noexcept = default;
    ~Http2Connection() noexcept;
    Http2Connection(const Http2Connection&) = delete;
    Http2Connection& operator=(const Http2Connection&) = delete;
    Http2Connection(Http2Connection&&) = delete;
    Http2Connection& operator=(Http2Connection&&) = delete;

    // Whether init() would take `config`, before it allocates anything.
    [[nodiscard]] static bool config_valid(const Http2Config& config) noexcept;
    // Writes this side's preface and SETTINGS to output(). False, holding nothing, for a configuration out of range or
    // a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, const Http2Config& config, u64 now_ns) noexcept;

    // The next thing that happened, false when nothing is left for now: more input, or room in output(). `taken` is how
    // much of the front of `input` is done with, also when false, and a Body payload lies inside it.
    [[nodiscard]] bool poll(cont::ConstSpan<u8> input, u64 now_ns, usize& taken, Http2Event& event) noexcept;
    // The most poll() may need to see at once, the frame header included.
    [[nodiscard]] usize input_bytes() const noexcept { return kHttp2FrameHeaderBytes + m_config.frame_bytes; }
    // The next `bytes` of input, behind what poll() has taken so far, came in TLS early data (RFC 8470).
    void early_input(usize bytes) noexcept { m_early_left += bytes; }

    // The bytes to send, valid until the next call that writes.
    [[nodiscard]] cont::ConstSpan<u8> output() const noexcept
    {
        return cont::ConstSpan<u8>(m_out + m_out_head, m_out_tail - m_out_head);
    }
    void output_taken(usize count) noexcept;

    // When poll() must run though nothing arrived; kNeverNs for never.
    [[nodiscard]] u64 next_deadline_ns() const noexcept { return m_settings_deadline_ns; }

    // A client's new request; `id` is its stream.
    [[nodiscard]] Http2Error open_request(cont::ConstSpan<HttpField> fields, bool fin, u32& id) noexcept;
    // A server's response (a 1xx any number of times first).
    [[nodiscard]] Http2Error send_response(u32 id, cont::ConstSpan<HttpField> fields, bool fin) noexcept;
    // One DATA frame of as much of `data` as fits (`taken`); `fin` ends the message when all of it went. Call again for
    // the rest until it is done or Blocked.
    [[nodiscard]] Http2Error send_body(u32 id, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept;
    // Trailers end the message.
    [[nodiscard]] Http2Error send_trailers(u32 id, cont::ConstSpan<HttpField> fields) noexcept;
    // Ends this side's message.
    [[nodiscard]] Http2Error finish(u32 id) noexcept;
    // Abandons both directions of a stream with `error` (kH2NoError after a complete response: section 8.1).
    [[nodiscard]] Http2Error cancel(u32 id, u32 error) noexcept;
    // Gives `count` bytes of Body payloads back to the peer's credit. Unknown once the stream ended, whose bytes went
    // back by themselves.
    [[nodiscard]] Http2Error consume_body(u32 id, usize count) noexcept;

    // Tells the peer no new streams will be handled. `immediate` false allows any it is opening (a first, gentle
    // GOAWAY); true names the last one handled. False when there was no room, retry after output_taken().
    [[nodiscard]] bool goaway(bool immediate) noexcept;

    [[nodiscard]] const Http2Config& config() const noexcept { return m_config; }
    [[nodiscard]] u32 error() const noexcept { return m_error; } // 0 for none
    [[nodiscard]] bool settings_known() const noexcept { return m_peer_known; }
    [[nodiscard]] const Http2Settings& peer_settings() const noexcept { return m_peer; }
    [[nodiscard]] bool goaway_received() const noexcept { return m_goaway_received; }
    [[nodiscard]] u32 open_streams() const noexcept { return m_open; }
    [[nodiscard]] const HpackDecoder& hpack_decoder() const noexcept { return m_decoder; }
    [[nodiscard]] const HpackEncoder& hpack_encoder() const noexcept { return m_encoder; }

private:
    enum class Stage : u8
    {
        Preface,     // a server's: the client's fixed bytes
        FrameHeader, // the next frame
        Body,        // DATA's content
        Fragment,    // a field block's bytes
        Padding,
        Skip,
    };

    enum class Step : u8
    {
        Wait,  // more input, or the connection failed
        Again, // something changed: look again
        Event, // `event` is filled
    };

    enum class Read : u8
    {
        Head, // no final HEADERS yet
        Body,
        Over,
    };

    enum class Send : u8
    {
        Idle,
        Message,
        Over,
    };

    enum class Closed : u8
    {
        Discard, // this side reset it: the peer's frame crossed that
        Reset,   // the peer reset it: STREAM_CLOSED for that stream
        Ended,   // both ends finished: STREAM_CLOSED for the connection
        Failed,  // closed_kind() failed the connection
    };

    struct RecentClose
    {
        u32 id = 0U;
        Closed how = Closed::Reset;
    };

    struct Record
    {
        i64 send_window = 0;
        i64 recv_window = 0;
        u64 content_left = 0U;
        u32 id = 0U;
        u32 next_free = 0U;
        u32 unconsumed = 0U; // delivered and not given back yet
        u32 credit = 0U;     // given back and not told to the peer yet
        Read read = Read::Head;
        Send send = Send::Idle;
        bool has_length = false;
        bool head = false;       // a client's HEAD
        bool connect = false;    // a CONNECT
        bool tunnel = false;     // a CONNECT answered 2xx: only DATA from here
        bool want_write = false; // a call came back Blocked
    };

    struct IndexEntry
    {
        u32 id = 0U;
        u32 slot = 0U;
    };

    void destroy() noexcept;

    // Streams.
    [[nodiscard]] usize position_of(u32 id) const noexcept;
    [[nodiscard]] Record* find(u32 id) noexcept;
    [[nodiscard]] Record& claim(u32 id) noexcept;
    void release(Record& record, Closed how) noexcept;
    void remember_closed(u32 id, Closed how) noexcept;
    [[nodiscard]] bool idle(u32 id) const noexcept;
    [[nodiscard]] Closed closed_kind(u32 id) noexcept;
    [[nodiscard]] u32 open_limit() const noexcept;

    // Reading.
    void fail(u32 code) noexcept;
    [[nodiscard]] bool charge(RateGate& gate, const GateRule& rule) noexcept;
    [[nodiscard]] bool take_owed(Http2Event& event) noexcept;
    [[nodiscard]] Step step(cont::ConstSpan<u8> view, usize& used, Http2Event& event) noexcept;
    [[nodiscard]] Step read_preface(cont::ConstSpan<u8> view, usize& used) noexcept;
    [[nodiscard]] Step read_frame_header(cont::ConstSpan<u8> view, usize& used, Http2Event& event) noexcept;
    [[nodiscard]] Step read_data(cont::ConstSpan<u8> view, u32 length, u8 flags, u32 id, usize& used,
                                 Http2Event& event) noexcept;
    [[nodiscard]] Step refuse_data(Record& record, u32 code, u32 length, u32 prefix, Http2Event& event) noexcept;
    [[nodiscard]] Step read_headers(cont::ConstSpan<u8> view, u32 length, u8 flags, u32 id, usize& used) noexcept;
    [[nodiscard]] Step read_continuation(u32 length, u8 flags, usize& used) noexcept;
    [[nodiscard]] Step read_priority(cont::ConstSpan<u8> payload, u32 id, Http2Event& event) noexcept;
    [[nodiscard]] Step read_rst_stream(cont::ConstSpan<u8> payload, u32 id, Http2Event& event) noexcept;
    [[nodiscard]] Step read_settings(cont::ConstSpan<u8> payload, u8 flags, u32 id, Http2Event& event) noexcept;
    [[nodiscard]] bool apply_settings(cont::ConstSpan<u8> payload) noexcept;
    void settings_acknowledged() noexcept;
    [[nodiscard]] Step read_ping(cont::ConstSpan<u8> payload, u8 flags, u32 id) noexcept;
    [[nodiscard]] Step read_goaway(cont::ConstSpan<u8> payload, u32 id, Http2Event& event) noexcept;
    [[nodiscard]] Step read_window_update(cont::ConstSpan<u8> payload, u32 id, Http2Event& event) noexcept;
    [[nodiscard]] Step read_body(cont::ConstSpan<u8> view, usize& used, Http2Event& event) noexcept;
    [[nodiscard]] Step frame_done(Http2Event& event) noexcept;
    [[nodiscard]] Step section_done(Http2Event& event) noexcept;
    [[nodiscard]] Step new_request(u32 id, usize count, bool too_large, Http2Event& event) noexcept;
    [[nodiscard]] Step on_section(Record& record, usize count, bool too_large, Http2Event& event) noexcept;
    [[nodiscard]] Step on_response(Record& record, cont::ConstSpan<HttpField> fields, Http2Event& event) noexcept;
    [[nodiscard]] Step on_trailers(Record& record, cont::ConstSpan<HttpField> fields, Http2Event& event) noexcept;
    [[nodiscard]] Step message_end(Record& record, Http2Event& event) noexcept;
    [[nodiscard]] Step stream_error(Record& record, u32 code, Http2Event& event) noexcept;
    void refuse_stream(u32 id, u32 code) noexcept;
    void refuse_too_large(u32 id) noexcept;
    void settle(Record& record) noexcept;
    void wake_writers() noexcept;

    // Flow control.
    void credit_connection(u32 bytes) noexcept;
    void credit_stream(Record& record, u32 bytes) noexcept;
    void flush_credit(Record* record) noexcept;

    // Writing.
    [[nodiscard]] usize output_room() const noexcept { return m_out_bytes - (m_out_tail - m_out_head); }
    [[nodiscard]] u8* output_tail(usize bytes) noexcept;
    void put_frame(u8 type, u8 flags, u32 stream, cont::ConstSpan<u8> payload) noexcept;
    void put_rst_stream(u32 id, u32 code) noexcept;
    void put_goaway(u32 last, u32 code) noexcept;
    [[nodiscard]] u32 frame_limit() const noexcept;
    [[nodiscard]] bool write_section(u32 id, cont::ConstSpan<HttpField> fields, bool fin) noexcept;
    [[nodiscard]] Http2Error send_section(Record& record, cont::ConstSpan<HttpField> fields, bool fin) noexcept;
    [[nodiscard]] Http2Error section_room(cont::ConstSpan<HttpField> fields) const noexcept;
    void write_settings() noexcept;
    void block(Record& record) noexcept;

    memory::IAllocator* m_allocator = nullptr;
    Http2Config m_config;
    Http2Settings m_peer;
    HpackDecoder m_decoder;
    HpackEncoder m_encoder;
    Record* m_records = nullptr;
    IndexEntry* m_index = nullptr;
    u32 m_open = 0U;
    u32 m_free = 0U;
    RecentClose* m_recent = nullptr; // the last streams closed
    u32 m_recent_slots = 0U;
    u32 m_recent_at = 0U;
    u32 m_recent_floor = 0U; // the highest identifier that fell out of m_recent
    u8* m_out = nullptr;
    usize m_out_bytes = 0U;
    usize m_out_head = 0U;
    usize m_out_tail = 0U;
    u8* m_block = nullptr;
    u32 m_block_bytes = 0U;
    u32 m_block_used = 0U;
    u32 m_block_id = 0U;
    u8* m_text = nullptr;
    HttpField* m_fields = nullptr;
    usize m_field_slots = 0U;
    RateGate m_reset_gate;
    RateGate m_control_gate;
    u64 m_now_ns = 0U;
    u64 m_settings_deadline_ns = kNeverNs;
    usize m_early_left = 0U; // of the input not taken yet, the bytes that came in early data
    i64 m_send_window = kHttp2InitialWindow;
    i64 m_recv_window = kHttp2InitialWindow;
    u32 m_conn_credit = 0U;
    u32 m_local_frame_bytes = kHttp2MinFrameBytes;
    u32 m_local_window = kHttp2InitialWindow;
    u32 m_highest = 0U;    // a server's: the highest stream the peer opened
    u32 m_next_local = 1U; // a client's: the next it opens
    u32 m_error = 0U;
    u32 m_goaway_sent = 0U;
    u32 m_goaway_peer = 0U;
    u32 m_wants = 0U;      // streams waiting for Writable
    u32 m_wake_at = 0U;    // where the walk for them stands
    u32 m_woken = 0U;      // the stream the walk told last: blocked again, it waits for the next wake
    u32 m_left = 0U;       // the current frame's content still to come
    u32 m_pad_left = 0U;
    u32 m_frame_id = 0U;
    u32 m_owed_id = 0U;    // a Finished is owed for it
    u8 m_frame_flags = 0U;
    Stage m_stage = Stage::FrameHeader;
    u8 m_preface_at = 0U;
    bool m_need_settings = true;  // the peer's first frame has not come
    bool m_block_open = false;    // CONTINUATION frames must follow
    bool m_block_end = false;     // its HEADERS had END_STREAM
    bool m_block_early = false;   // its HEADERS started in early data
    bool m_self_dependent = false;
    bool m_frame_is_block = false;
    bool m_settings_acked = false;
    bool m_peer_known = false;
    bool m_goaway_was_sent = false;
    bool m_goaway_received = false;
    bool m_sweep = false;         // streams above a GOAWAY's last are still to be cut
    bool m_wake = false;          // room or credit came: tell the writers
    bool m_want_open = false;     // open_request() came back Blocked
    bool m_credit_owed = false;   // a WINDOW_UPDATE had no room
};

} // namespace crd::net
