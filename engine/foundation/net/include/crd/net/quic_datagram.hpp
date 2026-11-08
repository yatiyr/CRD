#pragma once

// crd-net -- a QUIC connection's unreliable datagrams (4.2a; contract:
// docs/design/network-foundation.md#quic-datagrams). Source: RFC 9221 sections 3 to 5.
//
//   negotiation  max_datagram_frame_size advertises the largest DATAGRAM frame (type, Length and payload) this side
//                receives, 0 for none; a frame when none was advertised, or a larger one, is PROTOCOL_VIOLATION
//                (section 3). This side sends only to a peer that advertised a size, and frames no larger than it.
//   sending      queue() copies a payload into a byte ring; a payload is refused at once when the peer takes none,
//                when its frame passes the peer's size or one 1-RTT packet of the path (frames are never split), or
//                when the ring is full. write_next() writes the oldest as a frame with its Length, so padding may
//                follow it; one that does not fit the room left waits for the next packet. A DATAGRAM frame is never
//                sent again and takes no flow-control credit, only the congestion window of its packet (section 5).
//                A datagram queued longer than max_age_ns is dropped unsent.
//   outcomes     with `outcomes`, every queued datagram is reported once: Acked (its packet was acknowledged, so the
//                peer's transport has it), Lost (its packet was declared lost) or Expired (dropped unsent). A
//                datagram counts against send_count until its report is polled, so no report is ever dropped.
//   receiving    a peer's payload is copied into a byte ring the application reads in place (peek, pop); a full
//                ring drops it and counts it (a receiver may drop datagrams, section 5).
//   memory       both rings come from the injected allocator at init(); nothing allocates after it.
//   threads      the datagrams belong to their connection's thread.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_transport_parameters.hpp>

namespace crd::memory
{
class IAllocator;
} // namespace crd::memory

namespace crd::net
{
inline constexpr u32 kQuicMaxDatagramFrame = 65535U;    // the largest size advertised (RFC 9221 section 3's "any")
inline constexpr u32 kQuicMaxDatagramRing = 1U << 24U;  // the bytes of either ring at most
inline constexpr u32 kQuicMaxDatagramCount = 1U << 16U; // the datagrams of either ring at most
// What a 1-RTT packet takes besides its frames at most: the first byte, a 20-byte Destination Connection ID, a 4-byte
// packet number and the AEAD tag (RFC 9000 section 17.3.1, RFC 9001 section 5.3).
inline constexpr usize kQuicShortPacketOverhead = 1U + crypto::kQuicMaxConnectionIdBytes + 4U + crypto::kQuicTagBytes;

// The largest payload whose DATAGRAM frame with its Length takes at most `frame_bytes`; false when not even an empty
// one fits.
[[nodiscard]] bool quic_datagram_payload_limit(u64 frame_bytes, usize& payload) noexcept;

struct QuicDatagramLimits
{
    // max_datagram_frame_size: the largest frame received, at most kQuicMaxDatagramFrame (0: none, and the peer's
    // DATAGRAM frames are PROTOCOL_VIOLATION).
    u32 max_frame_bytes = 0U;
    // Payload bytes held for the application: at least one and at least the largest payload received,
    // max_frame_bytes - 1 in a frame without a Length (0 when nothing is received).
    u32 receive_bytes = 0U;
    u32 receive_count = 0U; // datagrams held
    u32 send_bytes = 0U;    // payload bytes queued (0: this side sends none)
    u32 send_count = 0U;    // datagrams queued, and with outcomes in flight or reported and not polled
    u64 max_age_ns = 0U;    // a datagram queued longer is dropped unsent (0: no limit)
    bool outcomes = false;  // report each sent datagram's acknowledgement or loss
};

enum class QuicDatagramError : u8
{
    None,
    NotReady,    // the peer's transport parameters have not arrived
    Unsupported, // the peer takes no DATAGRAM frames, or this side has no send ring
    TooLarge,    // the payload is over max_payload()
    Full,        // the ring or send_count is full: poll reports, or wait for the queue to drain
};

enum class QuicDatagramOutcome : u8
{
    Acked,
    Lost,
    Expired,
};

struct QuicDatagramReport
{
    u64 id = 0U;
    QuicDatagramOutcome outcome = QuicDatagramOutcome::Acked;
};

struct QuicDatagramCounters
{
    u64 received = 0U; // payloads held for the application
    u64 dropped = 0U;  // payloads the receive ring had no room for
    u64 sent = 0U;     // frames written
    u64 expired = 0U;  // datagrams dropped unsent
};

class QuicDatagrams
{
public:
    QuicDatagrams() noexcept = default;
    ~QuicDatagrams() noexcept;
    QuicDatagrams(const QuicDatagrams&) = delete;
    QuicDatagrams& operator=(const QuicDatagrams&) = delete;
    QuicDatagrams(QuicDatagrams&&) = delete;
    QuicDatagrams& operator=(QuicDatagrams&&) = delete;

    // Whether `limits` is in range: sizes within the bounds above, a count with every ring, a receive ring with a
    // size advertised that holds its largest payload, none without, and outcomes only with a send ring.
    [[nodiscard]] static bool limits_valid(const QuicDatagramLimits& limits) noexcept;

    // Takes both rings from `allocator`; false, holding nothing, for limits out of range or a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, const QuicDatagramLimits& limits) noexcept;

    // This side's max_datagram_frame_size, written into `local`.
    void fill_parameters(QuicTransportParameters& local) const noexcept;
    // The peer's max_datagram_frame_size, and the largest UDP payload a packet to it may take.
    void set_peer(u64 peer_max_frame, usize udp_payload) noexcept;

    // A DATAGRAM frame from the peer: NoError (held, or dropped when the ring is full), or PROTOCOL_VIOLATION.
    [[nodiscard]] QuicTransportError on_frame(const QuicFrame& frame) noexcept;

    // The largest payload queue() takes now; 0 before set_peer() or while sending is unsupported.
    [[nodiscard]] usize max_payload() const noexcept { return m_max_payload; }
    [[nodiscard]] bool sendable() const noexcept { return m_sendable; }
    // Queues a copy of `payload`, naming it `id` (rising from 0): drops what expired at `now_ns` first.
    [[nodiscard]] QuicDatagramError queue(cont::ConstSpan<u8> payload, u64 now_ns, u64& id) noexcept;

    // Whether write_next() would write a frame into an empty packet at `now_ns`.
    [[nodiscard]] bool has_frame(u64 now_ns) const noexcept;
    // Drops what expired at `now_ns`, then writes the oldest datagram at the start of `out` and returns its bytes,
    // naming it `id`; 0 when none is queued or it does not fit (it stays first).
    [[nodiscard]] usize write_next(cont::Span<u8> out, u64 now_ns, u64& id) noexcept;
    // A packet carrying datagram `id` was acknowledged or declared lost (only with outcomes).
    void on_acked(u64 id) noexcept;
    void on_lost(u64 id) noexcept;
    // The next report, false when none is waiting.
    [[nodiscard]] bool poll(QuicDatagramReport& report) noexcept;
    [[nodiscard]] bool outcomes() const noexcept { return m_limits.outcomes; }

    // The oldest payload received, in place: false when none is held. pop() gives it up.
    [[nodiscard]] bool peek(cont::ConstSpan<u8>& payload) const noexcept;
    void pop() noexcept;

    [[nodiscard]] u32 queued() const noexcept { return m_send.count(); }
    [[nodiscard]] u32 held() const noexcept { return m_receive.count(); }
    [[nodiscard]] u32 unreported() const noexcept { return m_unreported; }
    [[nodiscard]] const QuicDatagramCounters& counters() const noexcept { return m_counters; }

private:
    struct Entry
    {
        u64 at = 0U; // the ring position of the first byte
        u64 id = 0U;
        u64 queued_ns = 0U;
        u32 bytes = 0U;
    };

    // Payloads in arrival order, each contiguous in the byte ring: one that would pass its end starts again at its
    // start, and an empty ring starts again at 0, so a payload of the ring's size always fits an empty one.
    class Ring
    {
    public:
        void bind(u8* bytes, u32 capacity, Entry* entries, u32 slots) noexcept;
        [[nodiscard]] bool push(cont::ConstSpan<u8> payload, u64 id, u64 now_ns) noexcept;
        [[nodiscard]] const Entry& front() const noexcept { return m_entries[m_first]; }
        [[nodiscard]] const Entry& back() const noexcept { return m_entries[(m_first + m_count - 1U) % m_slots]; }
        [[nodiscard]] cont::ConstSpan<u8> bytes_of(const Entry& entry) const noexcept;
        void pop() noexcept;
        [[nodiscard]] u32 count() const noexcept { return m_count; }

    private:
        u8* m_bytes = nullptr;
        Entry* m_entries = nullptr;
        u64 m_head = 0U; // the ring positions of the oldest byte and of the next
        u64 m_tail = 0U;
        u32 m_capacity = 0U;
        u32 m_slots = 0U;
        u32 m_first = 0U;
        u32 m_count = 0U;
    };

    void report(u64 id, QuicDatagramOutcome outcome) noexcept;
    void expire(u64 now_ns) noexcept;

    memory::IAllocator* m_allocator = nullptr;
    QuicDatagramLimits m_limits;
    u8* m_bytes = nullptr;
    Entry* m_entries = nullptr;
    QuicDatagramReport* m_reports = nullptr;
    Ring m_receive;
    Ring m_send;
    u32 m_report_first = 0U;
    u32 m_report_count = 0U;
    u32 m_unreported = 0U; // queued, and with outcomes in flight or reported and not polled
    u64 m_next_id = 0U;
    usize m_max_payload = 0U;
    bool m_peer_known = false;
    bool m_sendable = false;
    QuicDatagramCounters m_counters;
};

} // namespace crd::net
