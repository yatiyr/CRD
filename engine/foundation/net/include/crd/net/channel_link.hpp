#pragma once

// crd-net -- a session's channels over whichever path carries it (4.2a; contract:
// docs/design/network-foundation.md#channel-link). One reader and one writer of NET.3's frames for QUIC, QMux, the raw UDP
// path and a WebTransport session, so a channel means the same thing on every path.
//
//   carrier   ChannelCarrier is what a path offers: a connection's QuicStreamSet and QuicDatagrams, or one WebTransport
//             session. Those are the only two shapes, so a branch serves them and a read costs no virtual call.
//   sending   a reliable ordered channel has one unidirectional stream, opened by its first write, whose route goes
//   once.
//             A reliable unordered channel has one stream per write, so nothing waits behind another message's loss. An
//             unreliable channel goes in datagrams the caller assembled. A channel's priority is its streams' urgency.
//   padding   a path that controls its packets pads them itself. WebTransport does not (the browser's end cannot), so
//             there a channel with a block pads in band, S30's forms, which a reader takes off on every path.
//   reading   every stream the peer opened has a StreamAssembler from a fixed pool; next() takes one frame from each
//             ready stream in turn and every datagram first. A frame whose bytes are all in hand is a view of the
//             stream's buffer: its bytes are consumed, and so its credit returned, only when the next call gives it up.
//   limits    a Wait leaves the bytes unread, so flow control holds the sender; a refused stream is STOP_SENDING for
//   that
//             stream alone; a broken frame or a stream ending inside one fails the link, for the owner to end the
//             session.
//   events    a QUIC or QMux link polls its stream set and datagrams inside next(). A WebTransport session shares its
//             connection's poll, so the owner hands over each event naming the session, and drains next() before the
//             next poll, whose call ends what the event views.
//   memory    everything comes from the injected allocator at init(); nothing allocates after it but what admission
//             asks for a frame that arrives in pieces.
//   threads   the owning connection's.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/admission.hpp>
#include <crd/net/quic_datagram.hpp>
#include <crd/net/quic_stream_set.hpp>
#include <crd/net/reassembly.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
class WebTransport;
struct WtEvent;

inline constexpr u32 kChannelLinkMaxStreams = 1024U;
// The code a refused stream is stopped with, and how long a reader held back by another session's frames waits at most.
inline constexpr u32 kChannelStreamRefused = 1U;
inline constexpr u64 kChannelRoomRetryNs = 10'000'000U;

enum class ChannelError : u8
{
    None,
    Blocked,  // a limit refuses it now: a Writable event says when to go again (a datagram: just try again)
    TooLarge, // a datagram above max_datagram()
    Invalid,  // no such channel, or the wrong carrier for it
    Closed,   // the path or the session is gone, or the link failed
};

class ChannelCarrier
{
public:
    ChannelCarrier() noexcept = default;
    // QUIC, QMux and the raw UDP path.
    ChannelCarrier(QuicStreamSet& streams, QuicDatagrams& datagrams) noexcept
        : m_streams(&streams), m_datagrams(&datagrams)
    {
    }
    ChannelCarrier(WebTransport& transport, u64 session) noexcept : m_transport(&transport), m_session(session) {}

    [[nodiscard]] bool valid() const noexcept { return m_streams != nullptr || m_transport != nullptr; }
    // A link on this carrier polls for itself.
    [[nodiscard]] bool polled() const noexcept { return m_streams != nullptr; }
    [[nodiscard]] bool pads_in_band() const noexcept { return m_transport != nullptr; }

    [[nodiscard]] ChannelError open(bool bidirectional, u64& stream) noexcept;
    // Whether open() of a unidirectional stream can go now, as far as the path knows.
    [[nodiscard]] bool can_open() const noexcept;
    [[nodiscard]] ChannelError write(u64 stream, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept;
    // What write() takes now; below `wanted`, a writable event follows.
    [[nodiscard]] usize send_room(u64 stream, usize wanted) noexcept;
    [[nodiscard]] ChannelError set_urgency(u64 stream, u8 urgency) noexcept;
    [[nodiscard]] QuicStreamRead peek(u64 stream) const noexcept;
    [[nodiscard]] ChannelError consume(u64 stream, usize count) noexcept;
    [[nodiscard]] ChannelError reset(u64 stream, u32 code) noexcept;
    [[nodiscard]] ChannelError stop(u64 stream, u32 code) noexcept;
    [[nodiscard]] ChannelError send_datagram(cont::ConstSpan<u8> payload, u64 now_ns) noexcept;
    [[nodiscard]] usize max_datagram() const noexcept;

    // A polled carrier's own.
    [[nodiscard]] bool poll(QuicStreamEvent& event) noexcept;
    [[nodiscard]] bool peek_datagram(cont::ConstSpan<u8>& payload, bool& early) const noexcept;
    void pop_datagram() noexcept;

private:
    QuicStreamSet* m_streams = nullptr;
    QuicDatagrams* m_datagrams = nullptr;
    WebTransport* m_transport = nullptr;
    u64 m_session = 0U;
};

struct ChannelLinkConfig
{
    u32 streams = 16U;       // of the peer's, read at once: past it a stream is refused
    u32 fragment_slots = 4U; // messages each unreliable channel holds in pieces at once
    u64 fragment_timeout_ns = 1'000'000'000U;
};

enum class ChannelEventKind : u8
{
    Frame,    // `channel`, `frame`, valid until the next call of this link
    Writable, // `channel`: a write that took less than offered can go on
    Stopped,  // `channel`: the peer's STOP_SENDING with `code`: what its stream held is lost, the next write opens
              // another
    Failed,   // `verdict`: the peer broke the protocol, so the owner ends the session (GoAway ProtocolError). Then
              // nothing.
};

struct ChannelEvent
{
    ChannelEventKind kind = ChannelEventKind::Frame;
    u32 channel = 0U;
    u32 code = 0U;
    AdmitVerdict verdict = AdmitVerdict::Admitted;
    AdmittedFrame frame;
};

struct ChannelCounters
{
    u64 streams_refused = 0U; // a route, a pool or a principal's limit refused them, or they were bidirectional
    u64 streams_aborted = 0U; // the peer reset them
    u64 datagrams_refused = 0U;
    u64 padding_bytes = 0U;
};

class ChannelLink
{
public:
    ChannelLink() noexcept = default;
    ~ChannelLink() noexcept;
    ChannelLink(const ChannelLink&) = delete;
    ChannelLink& operator=(const ChannelLink&) = delete;
    ChannelLink(ChannelLink&&) = delete;
    ChannelLink& operator=(ChannelLink&&) = delete;

    // False, holding nothing, for a configuration out of range, an unbound admission or a refusal. `admission` and what
    // `carrier` names must outlive this. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, Admission& admission, const ChannelCarrier& carrier,
                            const ChannelLinkConfig& config) noexcept;

    // A WebTransport carrier's: an event of this link's session.
    void on_webtransport(const WtEvent& event, u64 now_ns) noexcept;

    // The next thing that happened, false when nothing is left for now.
    [[nodiscard]] bool next(ChannelEvent& event, u64 now_ns) noexcept;
    // When next() has something to do that nothing else will announce; kNeverNs for no such time.
    [[nodiscard]] u64 next_timer_ns() const noexcept;

    // Frames in the order written, whole or continuing: `taken` says how many bytes went, and the caller sends the rest
    // next. Blocked with none taken: a limit stands in the way. Invalid on an unreliable channel.
    [[nodiscard]] ChannelError write(u32 channel, cont::ConstSpan<u8> frames, usize& taken) noexcept;
    // The datagram in the first `used` bytes of `buffer`, its route included; what is left of `buffer` is room to pad.
    [[nodiscard]] ChannelError send_datagram(u32 channel, cont::Span<u8> buffer, usize used, u64 now_ns) noexcept;
    [[nodiscard]] usize max_datagram() const noexcept { return m_carrier.max_datagram(); }

    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] u32 streams_held() const noexcept;
    [[nodiscard]] const ChannelCounters& counters() const noexcept { return m_counters; }

private:
    static constexpr u32 kNone = ~u32{0};

    struct ReadStream
    {
        u64 id = 0U;
        u64 wait_until = 0U;
        u32 next = kNone; // the ready list
        bool used = false;
        bool queued = false;
        bool waiting = false;
        bool early = false; // what is carried of a frame's head came as early data
        StreamAssembler* assembler = nullptr;
        alignas(StreamAssembler) u8 storage[sizeof(StreamAssembler)];
    };

    struct SendChannel
    {
        u64 stream = 0U;
        u64 pending = 0U; // bytes since the stream was last at a padding block
        u32 stop_code = 0U;
        bool open = false;
        bool blocked = false;
        bool writable_owed = false;
        bool stopped = false;
        u8 route_bytes = 0U;
        u8 route_sent = 0U;
        u8 route[kMaxRouteBytes] = {};
    };

    struct DatagramChannel
    {
        DatagramAssembler* assembler = nullptr;
        alignas(DatagramAssembler) u8 storage[sizeof(DatagramAssembler)];
    };

    [[nodiscard]] ReadStream* find_stream(u64 id) noexcept;
    [[nodiscard]] SendChannel* find_sender(u64 stream) noexcept;
    void push_ready(u32 index) noexcept;
    [[nodiscard]] u32 pop_ready() noexcept;
    void stream_opened(u64 id) noexcept;
    void stream_readable(u64 id) noexcept;
    void stream_aborted(u64 id) noexcept;
    void stream_stopped(u64 id, u64 code) noexcept;
    void stream_writable(u64 id) noexcept;
    void wake_blocked() noexcept;
    void refuse(u64 id) noexcept;
    void release(ReadStream& stream) noexcept;
    void finish_stream(ReadStream& stream) noexcept;
    void fail(AdmitVerdict verdict) noexcept;
    void give_up_frame() noexcept;
    void wake_waiting(u64 now_ns) noexcept;
    void hold_back(ReadStream& stream, u64 retry_at_ns, u64 now_ns) noexcept;
    [[nodiscard]] bool take_report(ChannelEvent& event) noexcept;
    [[nodiscard]] bool read_stream(u32 index, ChannelEvent& event, u64 now_ns) noexcept;
    [[nodiscard]] bool read_datagram(ChannelEvent& event, u64 now_ns) noexcept;
    void open_datagram(cont::ConstSpan<u8> payload, bool early, bool held, u64 now_ns) noexcept;
    void end_datagram() noexcept;
    [[nodiscard]] bool poll_path(u64 now_ns) noexcept;
    void on_quic_event(const QuicStreamEvent& event) noexcept;
    [[nodiscard]] ChannelError open_channel(u32 channel, const wire::ChannelDesc& desc, SendChannel& send) noexcept;
    [[nodiscard]] ChannelError send_route(SendChannel& send) noexcept;
    void pad_stream(const wire::ChannelDesc& desc, SendChannel& send) noexcept;

    memory::IAllocator* m_allocator = nullptr;
    Admission* m_admission = nullptr;
    ChannelCarrier m_carrier;
    ChannelLinkConfig m_config;
    ReadStream* m_streams = nullptr;
    SendChannel* m_send = nullptr;
    DatagramChannel* m_datagrams = nullptr;
    FragmentSlot* m_slots = nullptr;
    u8* m_padding = nullptr;
    u32 m_channel_count = 0U;
    u32 m_slot_count = 0U;
    usize m_padding_bytes = 0U;
    u32 m_ready_head = kNone;
    u32 m_ready_tail = kNone;
    u32 m_waiting = 0U;
    u32 m_reports = 0U;
    u64 m_wake_at = kNeverNs;
    u64 m_sweep_at = kNeverNs;
    bool m_room_freed = false;
    bool m_open_waiting = false;
    bool m_failed = false;
    bool m_failure_told = false;
    AdmitVerdict m_verdict = AdmitVerdict::Admitted;
    // The frame the caller holds.
    bool m_frame_out = false;
    bool m_frame_ends = false;
    AdmittedFrame m_frame;
    u32 m_frame_stream = kNone;
    usize m_frame_bytes = 0U;
    // The datagram being read.
    bool m_datagram_open = false;
    bool m_datagram_held = false; // the carrier still holds it: pop it when done
    crypto::Delivery m_datagram_delivery = crypto::Delivery::Established;
    DatagramCursor m_cursor;
    ChannelCounters m_counters;
};

} // namespace crd::net
