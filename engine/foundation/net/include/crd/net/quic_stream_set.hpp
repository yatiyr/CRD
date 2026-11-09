#pragma once

// crd-net -- a QUIC connection's streams (4.2a; contract: docs/design/network-foundation.md#quic-stream-set).
// Sources: RFC 9000 sections 2, 3, 4, 13.3 and 19.4 to 19.14, RFC 9218 section 4's urgencies and
// draft-ietf-quic-reliable-stream-reset.
//
//   streams    each of the four stream types (section 2.1) has a fixed pool of slots holding S38's buffers and
//              credits; its open streams are kept sorted by stream index (IDs open in rising order, so an append
//              keeps the order) and found by binary search. A peer's frame for a higher stream of its own opens every
//              lower one of that type (section 3.2); a frame for a stream already closed is ignored.
//   limits     a peer may have as many streams of a type open as this side has slots for it: MAX_STREAMS grants the
//              streams freed plus the slots, and is owed once half the slots were freed since the last grant or when
//              the peer says it is blocked (section 4.6). This side opens streams within the peer's grant and owes one
//              STREAMS_BLOCKED per limit it reached.
//   credit     every stream's data passes its own credit and the connection's, both directions: the connection's
//              sums are running totals (a stream's highest offset received, its bytes read, its bytes sent), so a
//              stream that closes changes neither window, and a reset stream counts as read to its final size (4.5).
//   states     sections 3.1 and 3.2: a STOP_SENDING resets the send part with the peer's code (3.5), a reset stream's
//              lost data is never sent again, a lost RESET_STREAM or STOP_SENDING is sent again until acknowledged and
//              lost credit frames follow section 13.3. A slot is freed once both parts are terminal and its events
//              were polled.
//   reliable   a reset may keep the stream's first bytes (RESET_STREAM_AT, only toward a peer that advertised it): they
//   resets     are sent and resent as the data before a FIN is, the frame goes once they have all been sent, so flow
//              control always covers its final size, and the part is done when the frame with the smallest size and
//              those bytes are acknowledged. The receiver reads up to that size and only then sees the reset.
//   sending    next_frame() hands the connection one frame that fits its room, with the record that settles it:
//              connection credit frames first, then each listed stream's owed control frames and data, by urgency
//              (0 first) and round robin within one, so no stream at an urgency starves another.
//   0-RTT      a client's streams run first on the parameters it remembers and then on its server's, which only raise
//              them (streams already open included); a rejection returns every stream and credit to init()'s state
//              (RFC 9001 section 4.6.2). A server marks the bytes that came in 0-RTT packets: they are replayable.
//   events     a per-slot mask of what happened (opened, readable, writable, reset, stop asked, all sent) in a queue
//              of slots: at most one entry per slot, so it never fills.
//   memory     everything comes from the injected allocator at init(); nothing allocates after it.
//   threads    a stream set belongs to its connection's thread.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_stream.hpp>
#include <crd/net/quic_transport_parameters.hpp>

namespace crd::memory
{
class IAllocator;
} // namespace crd::memory

namespace crd::net
{
inline constexpr u32 kQuicMaxStreamSlots = 4096U;      // per stream type
inline constexpr u32 kQuicMaxStreamBuffer = 1U << 24U; // a stream buffer's bytes at most
inline constexpr u8 kQuicStreamUrgencies = 8U;         // RFC 9218's urgencies, 0 the most urgent
inline constexpr u8 kQuicDefaultUrgency = 3U;          // RFC 9218 section 4.1
inline constexpr u32 kQuicNoStreamSlot = ~u32{0};

// What a connection's streams may hold. All zero (the default) is a connection without streams.
struct QuicStreamLimits
{
    u32 peer_bidi = 0U;  // bidirectional streams the peer may have open at once: initial_max_streams_bidi
    u32 peer_uni = 0U;   // unidirectional ones: initial_max_streams_uni
    u32 local_bidi = 0U; // streams of each kind this side may have open at once (within the peer's grant)
    u32 local_uni = 0U;
    // Per stream: the receive buffer, which is its window's maximum, and the send buffer; multiples of
    // kQuicStreamChunk, at most kQuicMaxStreamBuffer, needed when a stream of a type that uses them is allowed.
    u32 receive_bytes = 0U;
    u32 send_bytes = 0U;
    u32 stream_window = 0U;         // the window first granted on each stream: 1 to receive_bytes
    u64 connection_window = 0U;     // initial_max_data
    u64 connection_max_window = 0U; // the connection window's auto-tuned maximum: at least connection_window
    // QMux: the path delivers in order, so a STREAM frame must continue its stream exactly or is PROTOCOL_VIOLATION.
    bool in_order = false;
    // Advertise reset_stream_at and take RESET_STREAM_AT (not with in_order: it is no frame of QMux).
    bool reset_stream_at = false;
};

enum class QuicStreamError : u8
{
    None,
    Blocked, // the peer's MAX_STREAMS grant is reached: a STREAMS_BLOCKED is owed
    NoSlot,  // every slot of the type is in use
    Unknown, // no open stream of that ID, or it has no part for this call (a peer's unidirectional stream to write)
    Ended,   // the part already ended: finished or reset
    Invalid, // an argument out of range: an urgency, more bytes to consume than peek() gave
};

// RFC 9000 section 3.1's sending part; None for a stream without one.
enum class QuicSendState : u8
{
    None,
    Ready,
    Send,
    DataSent,
    DataRecvd,
    ResetSent,
    ResetRecvd,
};

// Section 3.2's receiving part; None for a stream without one.
enum class QuicRecvState : u8
{
    None,
    Recv,
    SizeKnown,
    DataRecvd,
    DataRead,
    ResetRecvd,
    ResetRead,
};

// QuicStreamEvent::events bits.
inline constexpr u8 kQuicStreamOpened = 1U << 0U;      // the peer opened it
inline constexpr u8 kQuicStreamReadable = 1U << 1U;    // data or the end arrived
inline constexpr u8 kQuicStreamWritable = 1U << 2U;    // a write() that took less than offered has room again
inline constexpr u8 kQuicStreamReset = 1U << 3U;       // the peer reset its sending part: reset_error
inline constexpr u8 kQuicStreamStopSending = 1U << 4U; // the peer asked this side to stop: the send part was reset
inline constexpr u8 kQuicStreamFinished = 1U << 5U;    // every byte and the FIN were acknowledged

struct QuicStreamEvent
{
    u64 id = 0U;
    u32 slot = 0U; // QuicStreamSet::slot_of's answer, which the stream may no longer have once this event was polled
    u8 events = 0U;
    u64 reset_error = 0U; // the peer's RESET_STREAM code
    u64 stop_error = 0U;  // the peer's STOP_SENDING code
};

// What can be read from a stream now: the bytes from its read offset that arrived without a gap, up to the end of
// its storage (after consume(), a second peek gives the rest when they wrap).
struct QuicStreamRead
{
    cont::ConstSpan<u8> data;
    bool fin = false;   // data ends at the stream's final size
    bool reset = false; // the peer reset it: nothing more is readable
    u64 reset_error = 0U;
    bool reset_coming = false; // a RESET_STREAM_AT arrived: the reset shows once the bytes it keeps are read
    // A server's: the first bytes of `data` that may have come in 0-RTT packets, replayable (RFC 9001 section 9.2),
    // so the application runs on them only what its schema declares idempotent.
    usize early_bytes = 0U;
};

// A stream frame a sent packet carried, settled when the packet is acknowledged or lost. `value` is a STREAM or
// CRYPTO frame's offset, a credit frame's limit or a RETIRE_CONNECTION_ID's sequence.
struct QuicStreamRecord
{
    u64 stream_id = 0U;
    u64 value = 0U;
    u32 length = 0U;
    QuicFrameKind kind = QuicFrameKind::Padding;
    bool fin = false;
    bool bidirectional = false; // MAX_STREAMS and STREAMS_BLOCKED
};

class QuicStreamSet
{
public:
    QuicStreamSet() noexcept = default;
    ~QuicStreamSet() noexcept;
    QuicStreamSet(const QuicStreamSet&) = delete;
    QuicStreamSet& operator=(const QuicStreamSet&) = delete;
    QuicStreamSet(QuicStreamSet&&) = delete;
    QuicStreamSet& operator=(QuicStreamSet&&) = delete;

    // Whether `limits` is in range (see QuicStreamLimits).
    [[nodiscard]] static bool limits_valid(const QuicStreamLimits& limits) noexcept;

    // Takes every slot and buffer from `allocator` for `side`'s streams; false, holding nothing, for limits out of
    // range or a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, const QuicStreamLimits& limits,
                            crypto::QuicSide side) noexcept;

    // This side's stream and flow-control parameters, written into `local`.
    void fill_parameters(QuicTransportParameters& local) const noexcept;
    // The peer's: its grants of streams and credit to this side. A client calls it with the parameters it remembers
    // for 0-RTT and again with its server's, so a call only raises what an earlier one granted, on the streams already
    // open too (a server accepting 0-RTT lowers none of them, RFC 9000 section 7.4.1).
    void set_peer_parameters(const QuicTransportParameters& peer) noexcept;
    // A client's server rejected 0-RTT: every stream, its data and both connection credits return to their state
    // after init(), with no peer parameters and no events (RFC 9001 section 4.6.2: the client resets them all).
    void reset_all() noexcept;

    // A stream or flow-control frame from the peer (STREAM, RESET_STREAM, RESET_STREAM_AT, STOP_SENDING, MAX_DATA,
    // MAX_STREAM_DATA, MAX_STREAMS, DATA_BLOCKED, STREAM_DATA_BLOCKED, STREAMS_BLOCKED): NoError, or the error to close
    // with. `early` for a frame of a 0-RTT packet: its STREAM data is marked replayable.
    [[nodiscard]] QuicTransportError on_frame(const QuicFrame& frame, bool early = false) noexcept;

    // Whether next_frame() would give a frame now: a credit or control frame owed, or data within both credits.
    [[nodiscard]] bool has_frame() const noexcept;
    // The next frame, at most `room` bytes, and its record; false when none fits. `rtt_ns` (the smoothed RTT) tunes
    // the windows. The frame is taken as sent: the caller writes it (it fits) and keeps the record.
    [[nodiscard]] bool next_frame(usize room, u64 now_ns, u64 rtt_ns, QuicFrame& frame,
                                  QuicStreamRecord& record) noexcept;
    // A packet carrying a recorded frame was acknowledged or declared lost.
    void on_acked(const QuicStreamRecord& record) noexcept;
    void on_lost(const QuicStreamRecord& record) noexcept;
    // Everything sent so far was delivered: for a transport that acknowledges by taking a frame (QMux), whose 0-RTT
    // frames count only once its server accepted them.
    void on_all_acked() noexcept;

    // The application's side. IDs are RFC 9000 section 2.1's.
    [[nodiscard]] QuicStreamError open(bool bidirectional, u64& id) noexcept;
    // Appends as much of `data` as the stream's buffer has room for (`taken`), and its end after it when `fin` and
    // every byte was taken.
    [[nodiscard]] QuicStreamError write(u64 id, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept;
    // Resets the sending part with `error`: RESET_STREAM at the bytes sent, or with a `reliable_size` (at most the
    // bytes written, and only toward a peer that advertised it: Invalid otherwise) RESET_STREAM_AT, delivering those
    // first bytes reliably. Called again on such a stream it lowers the size, with the same code.
    [[nodiscard]] QuicStreamError reset(u64 id, u64 error, u64 reliable_size = 0U) noexcept;
    // Asks the peer to stop with `error` (STOP_SENDING; data arriving after it is dropped and its credit returned).
    [[nodiscard]] QuicStreamError stop_sending(u64 id, u64 error) noexcept;
    // The stream's urgency, 0 (most urgent) to kQuicStreamUrgencies - 1; new streams take kQuicDefaultUrgency.
    [[nodiscard]] QuicStreamError set_urgency(u64 id, u8 urgency) noexcept;
    [[nodiscard]] QuicStreamRead peek(u64 id) const noexcept;
    // Copies into `out` the bytes that arrived without a gap, from `skip` bytes past the read offset, across the wrap
    // peek() stops at; `ends` is true when they reach the stream's final size. Nothing is consumed: a layer holding
    // whole frames keeps them in the stream's own buffer. 0 for a stream not receiving.
    [[nodiscard]] usize copy_readable(u64 id, usize skip, cont::Span<u8> out, bool& ends) const noexcept;
    // Gives up the next `count` bytes peek() gave; after a peek() with `fin`, consuming its data (even none) ends the
    // receiving part. `keep_end`: it stays open at its end, peek() still reporting `fin`, until a call without it, for a
    // layer that must first act on the end and would otherwise lose the slot to it.
    [[nodiscard]] QuicStreamError consume(u64 id, usize count, bool keep_end = false) noexcept;
    // Lets the reset of a RESET_STREAM_AT show now, for a layer that cannot use the bytes it keeps (they may stop inside
    // a frame, and waiting for the rest would hold the stream forever). Ended without one.
    [[nodiscard]] QuicStreamError give_up_reliable(u64 id) noexcept;
    // The next stream with events, its mask cleared; false when none has any.
    [[nodiscard]] bool poll(QuicStreamEvent& event) noexcept;
    // The bytes write() takes now; below `wanted`, a Writable event is owed once more room comes. 0 for a stream that
    // cannot send.
    [[nodiscard]] usize send_room(u64 id, usize wanted) noexcept;
    // An index below slots() that stays the stream's while it is open, for a layer keeping per-stream state beside the
    // set's; kQuicNoStreamSlot for a stream not open.
    [[nodiscard]] u32 slot_of(u64 id) const noexcept;
    [[nodiscard]] u32 slots() const noexcept { return m_total_slots; }
    [[nodiscard]] const QuicStreamLimits& limits() const noexcept { return m_limits; }

    // The parts' states of a stream (None for a stream not open).
    [[nodiscard]] QuicSendState send_state(u64 id) const noexcept;
    [[nodiscard]] QuicRecvState recv_state(u64 id) const noexcept;
    // Streams open, of every type.
    [[nodiscard]] u32 open_streams() const noexcept;
    // This side's grant of streams of a type to the peer, and the peer's grant to this side.
    [[nodiscard]] u64 granted(bool bidirectional) const noexcept;
    [[nodiscard]] u64 peer_limit(bool bidirectional) const noexcept;
    [[nodiscard]] const QuicReceiveWindow& connection_window() const noexcept { return m_window; }
    [[nodiscard]] const QuicSendCredit& connection_credit() const noexcept { return m_credit; }
    // The peer advertised reset_stream_at (remembered for 0-RTT).
    [[nodiscard]] bool peer_resets_at() const noexcept { return m_peer_resets_at; }
    [[nodiscard]] u64 connection_sent() const noexcept { return m_sent; }

private:
    static constexpr u32 kNone = ~u32{0};

    struct Slot
    {
        u64 id = 0U;
        QuicSendBuffer out;
        QuicReceiveBuffer in;
        QuicSendCredit credit;    // the peer's grant on this stream
        QuicReceiveWindow window; // this side's
        u64 consumed = 0U;        // the bytes counted as read into the connection's total
        u64 reset_error = 0U;     // this side's RESET_STREAM
        u64 reset_final = 0U;     // the final size it names: every byte ever sent
        u64 reset_reliable = 0U;  // its reliable size: the smallest one given
        u64 peer_reliable = 0U;   // the smallest reliable size the peer named
        u64 stop_error = 0U;      // this side's STOP_SENDING
        u64 peer_reset_error = 0U;
        u64 peer_stop_error = 0U;
        u64 early_end = 0U;    // the end of the data that came in 0-RTT packets
        u32 send_prev = kNone; // the urgency's send list
        u32 send_next = kNone;
        u32 event_next = kNone;
        QuicSendState send = QuicSendState::None;
        QuicRecvState recv = QuicRecvState::None;
        u8 events = 0U;
        u8 urgency = kQuicDefaultUrgency;
        bool active = false;
        bool listed = false;
        bool reset_owed = false;
        bool reset_at = false;       // this side's reset is a RESET_STREAM_AT
        bool reset_at_acked = false; // and the one carrying the current size was acknowledged
        bool peer_reset_at = false;  // the peer's RESET_STREAM_AT arrived
        bool stop_owed = false;
        bool stopped = false; // this side asked the peer to stop: arriving data is dropped
        bool want_writable = false;
    };

    // One of the four stream types (section 2.1): its slots, their open order and its limits.
    struct Type
    {
        u32 first = 0U;       // its slots: [first, first + capacity) of m_slots
        u32* order = nullptr; // open slots, ascending by stream ID
        u32* free = nullptr;  // a stack of free slots
        u8* bytes = nullptr;  // its slots' buffers, `stride` bytes and `words` bitmap words each
        u64* bits = nullptr;
        usize stride = 0U;
        usize words = 0U;
        u32 capacity = 0U;
        u32 open = 0U;
        u32 free_count = 0U;
        u64 next = 0U;        // this side's: the next index to open; the peer's: the indices opened so far
        u64 limit = 0U;       // this side's: the peer's grant; the peer's: this side's grant
        u64 freed = 0U;       // the peer's: streams freed
        u64 send_credit = 0U; // the peer's initial grant on each stream of the type
        u64 blocked_at = 0U;  // this side's: the limit a STREAMS_BLOCKED named
        bool blocked_owed = false;
        bool blocked_sent = false;
        bool peer_blocked = false; // the peer's: it sent STREAMS_BLOCKED
        bool grant_lost = false;   // the peer's: the latest MAX_STREAMS was lost
        bool local = false;
        bool bidirectional = false;
        bool sends = false;
        bool receives = false;
    };

    enum class Lookup : u8
    {
        Found,
        Closed, // a stream that existed and was freed: its frames are ignored
        Error,
    };

    [[nodiscard]] Type& type_of(u64 id) noexcept { return m_types[id & 3U]; }
    [[nodiscard]] const Type& type_of(u64 id) const noexcept { return m_types[id & 3U]; }
    [[nodiscard]] u32 type_index(bool local, bool bidirectional) const noexcept;
    [[nodiscard]] u32 locate(u64 id) const noexcept; // the open slot of a stream, kNone for none
    [[nodiscard]] Slot* find(u64 id) noexcept;
    [[nodiscard]] const Slot* find(u64 id) const noexcept;
    [[nodiscard]] Lookup reach(u64 id, Slot*& slot, QuicTransportError& error) noexcept;
    [[nodiscard]] Slot* open_slot(Type& type) noexcept;
    // Every slot free, every type's counters and both credits as init() leaves them.
    void reset_state() noexcept;
    void try_free(Slot& slot) noexcept;
    void raise(Slot& slot, u8 events) noexcept;
    void relist(Slot& slot) noexcept;
    void unlist(Slot& slot) noexcept;
    [[nodiscard]] u32 index_of(const Slot& slot) const noexcept;

    // Receiving.
    [[nodiscard]] QuicTransportError on_stream(const QuicFrame& frame, Slot& slot, bool early) noexcept;
    [[nodiscard]] QuicTransportError on_reset(const QuicFrame& frame, Slot& slot) noexcept;
    [[nodiscard]] QuicTransportError on_reset_at(const QuicFrame& frame, Slot& slot) noexcept;
    [[nodiscard]] QuicTransportError take_final_size(Slot& slot, u64 final_size) noexcept;
    // The reliable bytes of a RESET_STREAM_AT were read (or a STOP_SENDING gave them up): the reset shows.
    void settle_peer_reset(Slot& slot) noexcept;
    [[nodiscard]] bool account_received(Slot& slot, u64 end) noexcept;
    void account_consumed(Slot& slot, u64 consumed_to) noexcept;
    void after_receive(Slot& slot, u64 contiguous_before) noexcept;
    void reset_send(Slot& slot, u64 error, u64 reliable_size) noexcept;
    void lower_reliable(Slot& slot, u64 reliable_size) noexcept;
    void settle_reset_at(Slot& slot) noexcept;
    void drop_stopped(Slot& slot) noexcept;

    // Sending.
    [[nodiscard]] u64 fresh_limit(const Slot& slot) const noexcept;
    [[nodiscard]] bool reset_ready(const Slot& slot) const noexcept;
    [[nodiscard]] bool data_ready(const Slot& slot) const noexcept;
    [[nodiscard]] bool control_owed(const Slot& slot) const noexcept;
    [[nodiscard]] bool pending(const Slot& slot) const noexcept;
    [[nodiscard]] bool data_waiting_on_connection() const noexcept;
    [[nodiscard]] bool grant_owed(const Type& type) const noexcept;
    [[nodiscard]] bool slot_frame(Slot& slot, usize room, u64 now_ns, u64 rtt_ns, QuicFrame& frame,
                                  QuicStreamRecord& record) noexcept;

    memory::IAllocator* m_allocator = nullptr;
    Slot* m_slots = nullptr;
    u32* m_indices = nullptr; // each type's order and free stack
    u8* m_bytes = nullptr;
    u64* m_bits = nullptr;
    usize m_byte_count = 0U;
    usize m_word_count = 0U;
    QuicStreamLimits m_limits;
    crypto::QuicSide m_side = crypto::QuicSide::Client;
    Type m_types[4];
    u32 m_total_slots = 0U;
    u32 m_heads[kQuicStreamUrgencies] = {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    u32 m_listed[kQuicStreamUrgencies] = {};
    u32 m_event_head = kNone;
    u32 m_event_tail = kNone;
    QuicSendCredit m_credit;    // the peer's connection grant: initial_max_data, MAX_DATA
    QuicReceiveWindow m_window; // this side's
    u64 m_sent = 0U;            // every stream's fresh bytes sent
    u64 m_received = 0U;        // every stream's highest offset received (a reset's final size)
    u64 m_consumed = 0U;        // every stream's bytes read, dropped or reset away
    bool m_peer_resets_at = false;
};

} // namespace crd::net
