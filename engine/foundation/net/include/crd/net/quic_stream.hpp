#pragma once

// crd-net -- QUIC byte streams and flow control (4.2a; contract: docs/design/network-foundation.md#quic-streams).
// Sources: RFC 9000 sections 2.2, 4.1, 4.2, 4.5, 7.5, 13.3, 19.6 and 19.8, and RFC 9002 section 5.
//
//   receiving   QuicReceiveBuffer reassembles one stream's or one level's CRYPTO data by offset into the caller's
//               storage, a window of its capacity from the first byte not yet read, with one bit per byte saying
//               what arrived: memory is fixed and exact whatever the peer's frames overlap, leave out or repeat.
//               Data past the window, a final size contradicted (section 4.5) and different bytes at an offset
//               already held (section 2.2) are refused, changing nothing.
//   sending     QuicSendBuffer holds what the application wrote until the peer acknowledged it, with one bit per byte
//               for acknowledged and one for lost data: lost data goes again first, lowest offset first, then data
//               never sent within the flow-control limit, and the FIN rides with the last byte (or alone).
//   credit      QuicSendCredit is the limit a peer's MAX_DATA or MAX_STREAM_DATA granted, which only rises, and the
//               DATA_BLOCKED or STREAM_DATA_BLOCKED owed once per limit; QuicReceiveWindow is the limit this side
//               grants, checked on every frame (FLOW_CONTROL_ERROR), updated once half the window was read and
//               doubled up to its maximum when updates come within two round trips (the window keeps pace with the
//               path's bandwidth-delay product), the latest value resent when a frame carrying it is lost (13.3).
//   storage     nothing allocates: each buffer's capacity is a multiple of kQuicStreamChunk, and its bitmaps are
//               quic_stream_bitmap_words(capacity) words each, all the caller's.
//   threads     each object belongs to its connection's thread.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::net
{
namespace cont = crd::containers;

inline constexpr usize kQuicStreamChunk = 64U; // a buffer's capacity is a multiple of this: one bitmap word

// The bitmap words a buffer of `capacity` bytes needs: one array for QuicReceiveBuffer, two for QuicSendBuffer.
[[nodiscard]] constexpr usize quic_stream_bitmap_words(usize capacity) noexcept
{
    return capacity / kQuicStreamChunk;
}

// What QuicReceiveBuffer::insert did with a frame's data.
enum class QuicReceiveResult : u8
{
    Accepted,     // stored; data already held or already read is accepted and changes nothing
    BeyondWindow, // ends past the window: FLOW_CONTROL_ERROR for a stream, CRYPTO_BUFFER_EXCEEDED for CRYPTO data
    FinalSize,    // FINAL_SIZE_ERROR (section 4.5)
    Changed,      // other bytes at an offset already held: PROTOCOL_VIOLATION (section 2.2)
    Unusable,     // init() did not succeed
};

// One stream's received data, or one encryption level's CRYPTO data, put back in order.
class QuicReceiveBuffer
{
public:
    // Uses `bytes` (a non-zero multiple of kQuicStreamChunk) and `bits` (quic_stream_bitmap_words(bytes.size())
    // words) and forgets everything; false, leaving the buffer unusable, for other sizes.
    bool init(cont::Span<u8> bytes, cont::Span<u64> bits) noexcept;

    // The data of a STREAM or CRYPTO frame at `offset`, `fin` for a STREAM frame with FIN; a RESET_STREAM's final
    // size is insert(final_size, {}, true). Data below read_offset() is not compared. A refusal changes nothing.
    [[nodiscard]] QuicReceiveResult insert(u64 offset, cont::ConstSpan<u8> data, bool fin) noexcept;

    // The bytes from read_offset() that have arrived without a gap, up to the end of the storage: after consume(),
    // a second call gives the rest when they wrap.
    [[nodiscard]] cont::ConstSpan<u8> readable() const noexcept;

    // Gives up the next `count` bytes that arrived without a gap (at most contiguous_end() - read_offset()), which
    // frees their room.
    void consume(u64 count) noexcept;

    [[nodiscard]] usize capacity() const noexcept { return m_bytes.size(); }
    [[nodiscard]] u64   read_offset() const noexcept { return m_read; }
    [[nodiscard]] u64   contiguous_end() const noexcept { return m_contiguous; }
    // The largest offset plus length received: what flow control counts (section 4.5).
    [[nodiscard]] u64   received_end() const noexcept { return m_received; }
    [[nodiscard]] bool  final_known() const noexcept { return m_final_known; }
    [[nodiscard]] u64   final_size() const noexcept { return m_final; }
    // Every byte up to the final size has been read.
    [[nodiscard]] bool  complete() const noexcept { return m_final_known && m_read == m_final; }

private:
    cont::Span<u8>  m_bytes;
    cont::Span<u64> m_bits;
    u64             m_read       = 0U;
    u64             m_contiguous = 0U;
    u64             m_received   = 0U;
    u64             m_final      = 0U;
    bool            m_final_known = false;
};

// A run of a stream's data to send: [offset, offset + length), with the FIN when `fin`. Nothing to send is a
// zero-length chunk without FIN.
struct QuicSendChunk
{
    u64   offset = 0U;
    usize length = 0U;
    bool  fin    = false;

    [[nodiscard]] bool empty() const noexcept { return length == 0U && !fin; }
};

// One stream's data to send, or one encryption level's CRYPTO data, held until the peer acknowledged it.
class QuicSendBuffer
{
public:
    // Uses `bytes` (a non-zero multiple of kQuicStreamChunk), `acked` and `lost` (quic_stream_bitmap_words(
    // bytes.size()) words each) and forgets everything; false, leaving the buffer unusable, for other sizes.
    bool init(cont::Span<u8> bytes, cont::Span<u64> acked, cont::Span<u64> lost) noexcept;

    // Appends as much of `data` as there is room for and returns the bytes taken: none after finish().
    [[nodiscard]] usize write(cont::ConstSpan<u8> data) noexcept;
    // The stream ends after what was written (a FIN to send).
    void finish() noexcept;

    // The next data to send, at most `max_bytes` of it: the lowest lost data first, then data never sent, which stays
    // below `fresh_limit` (the stream's flow-control limit; kQuicMaxVarint for CRYPTO data). A chunk never wraps the
    // storage, and the FIN rides on a chunk ending at the final size, or alone when only it is owed.
    [[nodiscard]] QuicSendChunk next(usize max_bytes, u64 fresh_limit) const noexcept;
    // The bytes of a chunk next() gave.
    [[nodiscard]] cont::ConstSpan<u8> bytes_of(const QuicSendChunk& chunk) const noexcept;
    // The chunk went into a packet.
    void on_sent(const QuicSendChunk& chunk) noexcept;

    // A packet carrying [offset, offset + length), with the FIN when `fin`, was acknowledged or declared lost. Only
    // data that was sent and not yet acknowledged changes; a loss after an acknowledgement of the same bytes (a
    // spurious one, or another copy) changes nothing.
    void on_acked(u64 offset, usize length, bool fin) noexcept;
    void on_lost(u64 offset, usize length, bool fin) noexcept;

    [[nodiscard]] usize capacity() const noexcept { return m_bytes.size(); }
    // The room write() has: the capacity less the bytes written and not yet acknowledged without a gap.
    [[nodiscard]] usize room() const noexcept;
    [[nodiscard]] u64   written_end() const noexcept { return m_written; }
    // The end of the data ever sent: what flow control counts.
    [[nodiscard]] u64   sent_end() const noexcept { return m_sent; }
    // Everything below it is acknowledged.
    [[nodiscard]] u64   acked_end() const noexcept { return m_acked; }
    [[nodiscard]] u64   lost_bytes() const noexcept { return m_lost_bytes; }
    [[nodiscard]] bool  finished() const noexcept { return m_finished; }
    // Lost data, data never sent or a FIN is waiting (data may still wait on flow control).
    [[nodiscard]] bool  pending() const noexcept;
    // Every byte and the FIN were acknowledged.
    [[nodiscard]] bool  all_acked() const noexcept { return m_fin_acked && m_acked == m_written; }

private:
    [[nodiscard]] bool fin_owed() const noexcept { return m_finished && !m_fin_acked && (!m_fin_sent || m_fin_lost); }

    cont::Span<u8>  m_bytes;
    cont::Span<u64> m_acked_bits;
    cont::Span<u64> m_lost_bits;
    u64             m_written    = 0U;
    u64             m_sent       = 0U;
    u64             m_acked      = 0U;
    u64             m_lost_bytes = 0U;
    u64             m_lost_low   = 0U; // no lost byte lies below it
    bool            m_finished   = false;
    bool            m_fin_sent   = false;
    bool            m_fin_lost   = false;
    bool            m_fin_acked  = false;
};

// The flow-control limit a peer granted this side: a stream's (initial_max_stream_data_* then MAX_STREAM_DATA) or
// the connection's (initial_max_data then MAX_DATA).
class QuicSendCredit
{
public:
    void reset(u64 limit) noexcept;

    // A MAX_DATA or MAX_STREAM_DATA value: a smaller one than the limit changes nothing (section 4.1).
    void raise(u64 limit) noexcept;
    [[nodiscard]] u64 limit() const noexcept { return m_limit; }
    // The credit left once `used` bytes (QuicSendBuffer::sent_end, or the connection's sum) were sent.
    [[nodiscard]] u64 available(u64 used) const noexcept { return (used < m_limit) ? m_limit - used : 0U; }

    // A DATA_BLOCKED or STREAM_DATA_BLOCKED naming limit() is owed: data `waiting` and the limit reached by `used`,
    // once per limit (section 4.1).
    [[nodiscard]] bool blocked_owed(u64 used, bool waiting) const noexcept;
    void               on_blocked_sent() noexcept;
    // A packet carrying a blocked frame naming `limit` was lost: owed again while that is still the limit.
    void               on_blocked_lost(u64 limit) noexcept;

private:
    u64  m_limit        = 0U;
    u64  m_blocked_at   = 0U;
    bool m_blocked_sent = false;
};

// The flow-control limit this side grants a peer: a stream's or the connection's (sections 4.1 and 4.2).
class QuicReceiveWindow
{
public:
    // Grants `window` bytes, tuned up to `max_window` (at least `window`; a stream's receive buffer holds it).
    void reset(u64 window, u64 max_window) noexcept;

    // The peer's data reached `end` (a stream's QuicReceiveBuffer::received_end, or the connection's sum of them):
    // false past the limit, FLOW_CONTROL_ERROR.
    [[nodiscard]] bool on_received(u64 end) noexcept;
    // The application has read up to `consumed` (a stream's read_offset, or the connection's sum; a reset stream
    // counts as read to its final size).
    void on_consumed(u64 consumed) noexcept;
    // The peer said it is blocked (DATA_BLOCKED or STREAM_DATA_BLOCKED): an update is owed if it would raise the limit.
    void on_peer_blocked() noexcept;

    // An update (MAX_DATA or MAX_STREAM_DATA) is owed: less than half the window left, the peer blocked, or the
    // latest update lost.
    [[nodiscard]] bool update_owed() const noexcept;
    // Settles the owed update and returns its limit, the consumed bytes plus the window; when the last update was
    // taken less than two `rtt_ns` (the smoothed RTT) before `now_ns`, the window doubles first, up to its maximum.
    // An update owed only because the latest was lost returns that limit unchanged and tunes nothing.
    [[nodiscard]] u64  take_update(u64 now_ns, u64 rtt_ns) noexcept;
    // A packet carrying an update of `limit` was lost: owed again while that is still the latest (section 13.3).
    void               on_update_lost(u64 limit) noexcept;

    [[nodiscard]] u64 limit() const noexcept { return m_limit; }
    [[nodiscard]] u64 window() const noexcept { return m_window; }
    [[nodiscard]] u64 received() const noexcept { return m_received; }
    [[nodiscard]] u64 consumed() const noexcept { return m_consumed; }

private:
    [[nodiscard]] bool raise_owed() const noexcept; // an update that raises the limit is owed

    u64  m_window     = 0U;
    u64  m_max_window = 0U;
    u64  m_limit      = 0U;
    u64  m_received   = 0U;
    u64  m_consumed   = 0U;
    u64  m_updated_ns = 0U;
    bool m_updated    = false; // an update was taken (m_updated_ns is its time)
    bool m_blocked    = false;
    bool m_lost       = false;
};

} // namespace crd::net
