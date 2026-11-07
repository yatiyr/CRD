#pragma once

// crd-net -- reading frames off their carriers within the budgets of admission (NET.3; contract:
// docs/design/network-foundation.md#frames). A frame whose bytes are all in hand is delivered as a view of them,
// copying nothing. Only a frame that really arrives in pieces takes a buffer, of exactly its declared length, after S6
// has reserved that length from the principal's quota and S7 has admitted it, so a refused message never costs a byte
// of memory.

#include <crd/net/admission.hpp>

namespace crd::net
{

// One reliable stream. Feed it what the stream holds; each call reads at most one frame.
//
//   usize at = 0U;
//   while (at < bytes.size())
//   {
//       const FrameStep step = assembler.feed(bytes.subspan(at), delivery, now);
//       at += step.consumed;
//       // Deliver: decode_frame() and execute, then admission.finish(step.frame).
//       // Wait: stop reading until step.retry_at_ns or a finish(), then feed the same bytes again.
//       // Close: end the session.
//   }
//
// A delivered frame's body may view the fed bytes: handle it before giving those up.
class StreamAssembler
{
public:
    explicit StreamAssembler(Admission& admission) noexcept : m_admission(admission) {}

    // Gives back the stream and any partial frame.
    ~StreamAssembler();

    StreamAssembler(const StreamAssembler&) = delete;
    StreamAssembler& operator=(const StreamAssembler&) = delete;

    [[nodiscard]] FrameStep feed(cont::ConstSpan<u8> bytes, crypto::Delivery delivery, u64 now_ns) noexcept;

private:
    enum class State : u8
    {
        Preamble,
        Header,
        Body,
        StepOver,
        Padding,
        Closed,
    };

    [[nodiscard]] FrameStep read_head(cont::ConstSpan<u8> bytes, crypto::Delivery delivery, u64 now_ns) noexcept;
    [[nodiscard]] FrameStep read_preamble(cont::ConstSpan<u8> view, usize from_input) noexcept;
    [[nodiscard]] FrameStep read_frame(cont::ConstSpan<u8> view, cont::ConstSpan<u8> bytes, crypto::Delivery delivery,
                                       u64 now_ns) noexcept;
    [[nodiscard]] FrameStep read_body(cont::ConstSpan<u8> bytes) noexcept;
    [[nodiscard]] usize skip_bytes(cont::ConstSpan<u8> bytes) noexcept;
    [[nodiscard]] FrameStep read_padding(cont::ConstSpan<u8> bytes, FrameStep step) noexcept;
    [[nodiscard]] FrameStep conclude(FrameStep step) noexcept;

    Admission& m_admission;
    State m_state = State::Preamble;
    bool m_opened = false;
    u32 m_channel = 0U;
    AdmitVerdict m_closed_by = AdmitVerdict::Malformed;
    u8 m_carry[kMaxFrameHeaderBytes] = {}; // a header split across feeds
    usize m_carried = 0U;
    u32 m_filled = 0U; // of a frame being reassembled
    u32 m_skip = 0U;   // bytes left of a refused frame or of padding
    AdmittedFrame m_frame;
};

// One message being put together from its fragments, kept by the session's pool.
struct FragmentSlot
{
    bool used = false;
    u64 sequence = 0U;
    u64 expires_ns = 0U;
    u32 chunk = 0U;
    u32 pieces = 0U;
    u32 received = 0U;
    crypto::Delivery delivery = crypto::Delivery::Established;
    u64 seen[kMaxFragments / 64U] = {};
    AdmittedFrame frame;
};

// One unreliable channel's datagrams: whole messages, listed ones and fragments, in any order, lost or repeated.
//
//   DatagramCursor cursor;
//   if (admission.open_datagram(bytes, now, cursor) == AdmitVerdict::Admitted)
//   {
//       DatagramAssembler& assembler = assemblers[cursor.route.channel];
//       while (cursor.more)
//       {
//           const FrameStep step = assembler.next(cursor, delivery, now);
//           // Deliver: decode_frame() and execute, then admission.finish(step.frame).
//       }
//   }
class DatagramAssembler
{
public:
    // `slots` bounds how many messages may be in pieces at once. A piece left unfinished `timeout_ns` after its
    // message's first gives its message up.
    DatagramAssembler(Admission& admission, u32 channel, cont::Span<FragmentSlot> slots, u64 timeout_ns) noexcept;

    // Gives back every partial message.
    ~DatagramAssembler();

    DatagramAssembler(const DatagramAssembler&) = delete;
    DatagramAssembler& operator=(const DatagramAssembler&) = delete;

    [[nodiscard]] FrameStep next(DatagramCursor& cursor, crypto::Delivery delivery, u64 now_ns) noexcept;

    // Gives up the messages whose time has passed; next() does this too.
    void sweep(u64 now_ns) noexcept;

private:
    [[nodiscard]] FrameStep read_whole(DatagramCursor& cursor, crypto::Delivery delivery, u64 now_ns) noexcept;
    [[nodiscard]] FrameStep read_listed(DatagramCursor& cursor, crypto::Delivery delivery, u64 now_ns) noexcept;
    [[nodiscard]] FrameStep read_fragment(DatagramCursor& cursor, crypto::Delivery delivery, u64 now_ns) noexcept;
    [[nodiscard]] FragmentSlot* find_slot(u64 sequence) noexcept;
    [[nodiscard]] AdmitVerdict open_slot(const ParsedFrame& parsed, crypto::Delivery delivery, u64 now_ns,
                                         FrameStep& step, FragmentSlot*& slot) noexcept;
    void abandon(FragmentSlot& slot) noexcept;
    void mark_done(u64 sequence) noexcept;
    [[nodiscard]] bool is_done(u64 sequence) const noexcept;
    [[nodiscard]] FrameStep conclude(FrameStep step) noexcept;

    Admission& m_admission;
    u32 m_channel = 0U;
    bool m_latest_wins = false; // a State channel: a newer message makes every older one stale
    cont::Span<FragmentSlot> m_slots;
    u64 m_timeout_ns = 0U;
    // The sequences finished, by delivery or by giving up: the highest, and a window below it.
    bool m_any_done = false;
    u64 m_done_high = 0U;
    u64 m_done_window = 0U; // bit n: m_done_high - 1 - n
};

} // namespace crd::net
