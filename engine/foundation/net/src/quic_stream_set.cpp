// crd-net -- a QUIC connection's streams (4.2a; contract: docs/design/network-foundation.md#quic-stream-set).
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/quic_stream_set.hpp>

namespace crd::net
{
namespace
{
constexpr u32 kUniBit = 2U;        // bit 1 of a stream ID: unidirectional (section 2.1)
constexpr usize kCreditBytes = 9U; // a MAX_DATA or MAX_STREAMS frame at most: its type and an 8-byte value

[[nodiscard]] constexpr u64 min_u64(u64 a, u64 b) noexcept
{
    return (a < b) ? a : b;
}

[[nodiscard]] constexpr u64 max_u64(u64 a, u64 b) noexcept
{
    return (a > b) ? a : b;
}

[[nodiscard]] constexpr bool sending(QuicSendState state) noexcept
{
    return state == QuicSendState::Ready || state == QuicSendState::Send || state == QuicSendState::DataSent;
}

// What still goes out on a sending part: everything before its end, and a reliable reset's first bytes.
[[nodiscard]] constexpr bool delivering(QuicSendState state, bool reset_at) noexcept
{
    return sending(state) || (state == QuicSendState::ResetSent && reset_at);
}

[[nodiscard]] constexpr bool receiving(QuicRecvState state) noexcept
{
    return state == QuicRecvState::Recv || state == QuicRecvState::SizeKnown || state == QuicRecvState::DataRecvd;
}

[[nodiscard]] constexpr bool chunk_bytes_ok(u32 bytes) noexcept
{
    return bytes != 0U && bytes % kQuicStreamChunk == 0U && bytes <= kQuicMaxStreamBuffer;
}

[[nodiscard]] bool fits(const QuicFrame& frame, usize room) noexcept
{
    const usize bytes = quic_frame_bytes(frame);
    return bytes != 0U && bytes <= room;
}
} // namespace

// ---- Construction ----

QuicStreamSet::~QuicStreamSet() noexcept
{
    if (m_allocator != nullptr)
    {
        memory::destroy_array(*m_allocator, m_slots, m_total_slots);
        memory::destroy_array(*m_allocator, m_indices, 2U * usize{m_total_slots});
        memory::destroy_array(*m_allocator, m_bytes, m_byte_count);
        memory::destroy_array(*m_allocator, m_bits, m_word_count);
    }
}

bool QuicStreamSet::limits_valid(const QuicStreamLimits& limits) noexcept
{
    const QuicStreamLimits& l = limits;
    const bool counts = l.peer_bidi <= kQuicMaxStreamSlots && l.peer_uni <= kQuicMaxStreamSlots &&
                        l.local_bidi <= kQuicMaxStreamSlots && l.local_uni <= kQuicMaxStreamSlots;
    const bool receives = l.peer_bidi != 0U || l.peer_uni != 0U || l.local_bidi != 0U;
    const bool sends = l.peer_bidi != 0U || l.local_bidi != 0U || l.local_uni != 0U;
    return counts && !(l.in_order && l.reset_stream_at) &&
           (!receives ||
            (chunk_bytes_ok(l.receive_bytes) && l.stream_window != 0U && l.stream_window <= l.receive_bytes)) &&
           (!sends || chunk_bytes_ok(l.send_bytes)) && l.connection_window <= l.connection_max_window &&
           l.connection_max_window <= kQuicMaxVarint;
}

u32 QuicStreamSet::type_index(bool local, bool bidirectional) const noexcept
{
    // Bit 0 names the initiator (0: the client), bit 1 a unidirectional stream.
    const bool by_client = local == (m_side == crypto::QuicSide::Client);
    return (bidirectional ? 0U : kUniBit) | (by_client ? 0U : 1U);
}

bool QuicStreamSet::init(memory::IAllocator& allocator, const QuicStreamLimits& limits, crypto::QuicSide side) noexcept
{
    if (m_allocator != nullptr || !limits_valid(limits))
    {
        return false;
    }
    m_limits = limits;
    m_side = side;
    u64 bytes = 0U;
    u64 words = 0U;
    u32 total = 0U;
    for (u32 index = 0U; index < 4U; ++index)
    {
        Type& type = m_types[index];
        type.local = ((index & 1U) == 0U) == (side == crypto::QuicSide::Client);
        type.bidirectional = (index & kUniBit) == 0U;
        type.sends = type.bidirectional || type.local;
        type.receives = type.bidirectional || !type.local;
        if (type.local)
        {
            type.capacity = type.bidirectional ? limits.local_bidi : limits.local_uni;
        }
        else
        {
            type.capacity = type.bidirectional ? limits.peer_bidi : limits.peer_uni;
            type.limit = type.capacity; // initial_max_streams_*
        }
        type.stride = (type.receives ? usize{limits.receive_bytes} : 0U) + (type.sends ? usize{limits.send_bytes} : 0U);
        type.words = (type.receives ? quic_stream_bitmap_words(limits.receive_bytes) : 0U) +
                     (type.sends ? 2U * quic_stream_bitmap_words(limits.send_bytes) : 0U);
        type.first = total;
        total += type.capacity;
        bytes += u64{type.capacity} * type.stride;
        words += u64{type.capacity} * type.words;
    }
    m_window.reset(limits.connection_window, limits.connection_max_window);
    m_allocator = &allocator;
    if (total == 0U)
    {
        return true;
    }
    if (bytes > (~usize{0} / 2U) || words > (~usize{0} / 16U))
    {
        m_allocator = nullptr;
        return false;
    }
    m_slots = memory::try_construct_array<Slot>(allocator, total);
    m_indices = memory::try_construct_array<u32>(allocator, 2U * usize{total});
    m_bytes = memory::try_construct_array<u8>(allocator, static_cast<usize>(bytes));
    m_bits = memory::try_construct_array<u64>(allocator, static_cast<usize>(words));
    m_total_slots = total;
    m_byte_count = static_cast<usize>(bytes);
    m_word_count = static_cast<usize>(words);
    if (m_slots == nullptr || m_indices == nullptr || m_bytes == nullptr || m_bits == nullptr)
    {
        memory::destroy_array(allocator, m_slots, total);
        memory::destroy_array(allocator, m_indices, 2U * usize{total});
        memory::destroy_array(allocator, m_bytes, m_byte_count);
        memory::destroy_array(allocator, m_bits, m_word_count);
        m_slots = nullptr;
        m_indices = nullptr;
        m_bytes = nullptr;
        m_bits = nullptr;
        m_total_slots = 0U;
        m_byte_count = 0U;
        m_word_count = 0U;
        m_allocator = nullptr;
        return false;
    }
    usize byte_at = 0U;
    usize word_at = 0U;
    for (Type& type : m_types)
    {
        type.order = m_indices + (2U * usize{type.first});
        type.free = type.order + type.capacity;
        type.bytes = m_bytes + byte_at;
        type.bits = m_bits + word_at;
        byte_at += usize{type.capacity} * type.stride;
        word_at += usize{type.capacity} * type.words;
    }
    reset_state();
    return true;
}

void QuicStreamSet::reset_state() noexcept
{
    for (Type& type : m_types)
    {
        for (u32 slot = 0U; slot < type.capacity; ++slot)
        {
            m_slots[type.first + slot] = Slot{};
            type.free[slot] = type.first + (type.capacity - 1U - slot); // the lowest slot is taken first
        }
        type.free_count = type.capacity;
        type.open = 0U;
        type.next = 0U;
        type.limit = type.local ? 0U : type.capacity; // the peer's: initial_max_streams_*
        type.freed = 0U;
        type.send_credit = 0U;
        type.blocked_at = 0U;
        type.blocked_owed = false;
        type.blocked_sent = false;
        type.peer_blocked = false;
        type.grant_lost = false;
    }
    for (usize urgency = 0U; urgency < kQuicStreamUrgencies; ++urgency)
    {
        m_heads[urgency] = kNone;
        m_listed[urgency] = 0U;
    }
    m_event_head = kNone;
    m_event_tail = kNone;
    m_credit = QuicSendCredit{};
    m_window.reset(m_limits.connection_window, m_limits.connection_max_window);
    m_sent = 0U;
    m_received = 0U;
    m_consumed = 0U;
    m_peer_resets_at = false;
}

void QuicStreamSet::reset_all() noexcept
{
    if (m_allocator != nullptr)
    {
        reset_state();
    }
}

void QuicStreamSet::fill_parameters(QuicTransportParameters& local) const noexcept
{
    const u64 window = m_limits.stream_window;
    local.initial_max_data = m_limits.connection_window;
    local.initial_max_stream_data_bidi_local = (m_limits.local_bidi != 0U) ? window : 0U;
    local.initial_max_stream_data_bidi_remote = (m_limits.peer_bidi != 0U) ? window : 0U;
    local.initial_max_stream_data_uni = (m_limits.peer_uni != 0U) ? window : 0U;
    local.initial_max_streams_bidi = m_limits.peer_bidi;
    local.initial_max_streams_uni = m_limits.peer_uni;
    local.reset_stream_at = m_limits.reset_stream_at;
}

void QuicStreamSet::set_peer_parameters(const QuicTransportParameters& peer) noexcept
{
    m_credit.raise(peer.initial_max_data);
    m_peer_resets_at = m_peer_resets_at || peer.reset_stream_at;
    Type& local_bidi = m_types[type_index(true, true)];
    Type& local_uni = m_types[type_index(true, false)];
    Type& peer_bidi = m_types[type_index(false, true)];
    // The peer's "remote" grant covers the streams this side opens; its "local" one those it opens (section 18.2).
    local_bidi.send_credit = max_u64(local_bidi.send_credit, peer.initial_max_stream_data_bidi_remote);
    local_bidi.limit = max_u64(local_bidi.limit, peer.initial_max_streams_bidi);
    local_uni.send_credit = max_u64(local_uni.send_credit, peer.initial_max_stream_data_uni);
    local_uni.limit = max_u64(local_uni.limit, peer.initial_max_streams_uni);
    // An open() before the parameters met limit 0: its STREAMS_BLOCKED is stale once the grant is above it.
    local_bidi.blocked_owed = local_bidi.blocked_owed && local_bidi.limit <= local_bidi.blocked_at;
    local_uni.blocked_owed = local_uni.blocked_owed && local_uni.limit <= local_uni.blocked_at;
    peer_bidi.send_credit = max_u64(peer_bidi.send_credit, peer.initial_max_stream_data_bidi_local);
    Type* const sending_types[] = {&local_bidi, &local_uni, &peer_bidi};
    for (const Type* type : sending_types)
    {
        for (u32 at = 0U; at < type->open; ++at)
        {
            Slot& slot = m_slots[type->order[at]];
            slot.credit.raise(type->send_credit);
            relist(slot);
        }
    }
}

// ---- Slots ----

u32 QuicStreamSet::index_of(const Slot& slot) const noexcept
{
    return static_cast<u32>(&slot - m_slots);
}

u32 QuicStreamSet::locate(u64 id) const noexcept
{
    const Type& type = type_of(id);
    u32 low = 0U;
    u32 high = type.open;
    while (low < high)
    {
        const u32 middle = low + ((high - low) / 2U);
        const u64 found = m_slots[type.order[middle]].id;
        if (found == id)
        {
            return type.order[middle];
        }
        if (found < id)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    return kNone;
}

QuicStreamSet::Slot* QuicStreamSet::find(u64 id) noexcept
{
    const u32 index = locate(id);
    return (index != kNone) ? &m_slots[index] : nullptr;
}

const QuicStreamSet::Slot* QuicStreamSet::find(u64 id) const noexcept
{
    const u32 index = locate(id);
    return (index != kNone) ? &m_slots[index] : nullptr;
}

QuicStreamSet::Slot* QuicStreamSet::open_slot(Type& type) noexcept
{
    if (type.free_count == 0U)
    {
        return nullptr;
    }
    const u32 index = type.free[--type.free_count];
    Slot& slot = m_slots[index];
    slot = Slot{};
    slot.id = (type.next << 2U) | static_cast<u64>(&type - m_types);
    slot.active = true;
    const u32 at = index - type.first;
    u8* const bytes = type.bytes + (usize{at} * type.stride);
    u64* const bits = type.bits + (usize{at} * type.words);
    usize byte_at = 0U;
    usize word_at = 0U;
    if (type.receives)
    {
        const usize words = quic_stream_bitmap_words(m_limits.receive_bytes);
        static_cast<void>(slot.in.init(cont::Span<u8>(bytes, m_limits.receive_bytes), cont::Span<u64>(bits, words)));
        slot.window.reset(m_limits.stream_window, m_limits.receive_bytes);
        slot.recv = QuicRecvState::Recv;
        byte_at = m_limits.receive_bytes;
        word_at = words;
    }
    if (type.sends)
    {
        const usize words = quic_stream_bitmap_words(m_limits.send_bytes);
        static_cast<void>(slot.out.init(cont::Span<u8>(bytes + byte_at, m_limits.send_bytes),
                                        cont::Span<u64>(bits + word_at, words),
                                        cont::Span<u64>(bits + word_at + words, words)));
        slot.credit.reset(type.send_credit);
        slot.send = QuicSendState::Ready;
    }
    type.order[type.open++] = index; // IDs of a type open in rising order, so the order stays sorted
    ++type.next;
    return &slot;
}

QuicStreamSet::Lookup QuicStreamSet::reach(u64 id, Slot*& slot, QuicTransportError& error) noexcept
{
    Type& type = type_of(id);
    const u64 index = id >> 2U;
    slot = nullptr;
    if (type.local)
    {
        if (index >= type.next)
        {
            error = QuicTransportError::StreamStateError; // a stream this side never opened (sections 19.4 to 19.13)
            return Lookup::Error;
        }
        slot = find(id);
        return (slot != nullptr) ? Lookup::Found : Lookup::Closed;
    }
    if (index >= type.limit)
    {
        error = QuicTransportError::StreamLimitError; // past this side's grant (section 4.6)
        return Lookup::Error;
    }
    if (index < type.next)
    {
        slot = find(id);
        return (slot != nullptr) ? Lookup::Found : Lookup::Closed;
    }
    // A frame for a higher stream opens every lower one of its type (section 3.2); the grant leaves a slot for each.
    while (type.next <= index)
    {
        slot = open_slot(type);
        if (slot == nullptr)
        {
            error = QuicTransportError::InternalError;
            return Lookup::Error;
        }
        raise(*slot, kQuicStreamOpened);
    }
    return Lookup::Found;
}

void QuicStreamSet::raise(Slot& slot, u8 events) noexcept
{
    if (slot.events == 0U)
    {
        const u32 index = index_of(slot);
        slot.event_next = kNone;
        if (m_event_tail == kNone)
        {
            m_event_head = index;
        }
        else
        {
            m_slots[m_event_tail].event_next = index;
        }
        m_event_tail = index;
    }
    slot.events = static_cast<u8>(slot.events | events);
}

void QuicStreamSet::relist(Slot& slot) noexcept
{
    if (slot.listed || !pending(slot))
    {
        return;
    }
    const u32 index = index_of(slot);
    u32& head = m_heads[slot.urgency];
    if (head == kNone)
    {
        slot.send_prev = index;
        slot.send_next = index;
        head = index;
    }
    else
    {
        // Appended before the head: the end of the round.
        Slot& first = m_slots[head];
        Slot& last = m_slots[first.send_prev];
        slot.send_prev = first.send_prev;
        slot.send_next = head;
        last.send_next = index;
        first.send_prev = index;
    }
    slot.listed = true;
    ++m_listed[slot.urgency];
}

void QuicStreamSet::unlist(Slot& slot) noexcept
{
    if (!slot.listed)
    {
        return;
    }
    const u32 index = index_of(slot);
    u32& head = m_heads[slot.urgency];
    if (slot.send_next == index)
    {
        head = kNone;
    }
    else
    {
        m_slots[slot.send_prev].send_next = slot.send_next;
        m_slots[slot.send_next].send_prev = slot.send_prev;
        if (head == index)
        {
            head = slot.send_next;
        }
    }
    slot.send_prev = kNone;
    slot.send_next = kNone;
    slot.listed = false;
    --m_listed[slot.urgency];
}

void QuicStreamSet::try_free(Slot& slot) noexcept
{
    const bool send_done = slot.send == QuicSendState::None || slot.send == QuicSendState::DataRecvd ||
                           slot.send == QuicSendState::ResetRecvd;
    const bool recv_done = slot.recv == QuicRecvState::None || slot.recv == QuicRecvState::DataRead ||
                           slot.recv == QuicRecvState::ResetRead;
    if (!slot.active || slot.events != 0U || !send_done || !recv_done)
    {
        return;
    }
    unlist(slot);
    Type& type = type_of(slot.id);
    const u32 index = index_of(slot);
    u32 at = 0U;
    while (type.order[at] != index)
    {
        ++at;
    }
    for (; at + 1U < type.open; ++at)
    {
        type.order[at] = type.order[at + 1U];
    }
    --type.open;
    type.free[type.free_count++] = index;
    slot.active = false;
    if (!type.local)
    {
        ++type.freed;
    }
}

// ---- Receiving ----

bool QuicStreamSet::account_received(Slot& slot, u64 end) noexcept
{
    const u64 before = slot.window.received();
    if (!slot.window.on_received(end))
    {
        return false;
    }
    if (end > before)
    {
        const u64 total = m_received + (end - before);
        if (!m_window.on_received(total))
        {
            return false;
        }
        m_received = total;
    }
    return true;
}

void QuicStreamSet::account_consumed(Slot& slot, u64 consumed_to) noexcept
{
    if (consumed_to > slot.consumed)
    {
        m_consumed += consumed_to - slot.consumed;
        slot.consumed = consumed_to;
        m_window.on_consumed(m_consumed);
    }
    slot.window.on_consumed(consumed_to);
}

void QuicStreamSet::after_receive(Slot& slot, u64 contiguous_before) noexcept
{
    if (!receiving(slot.recv))
    {
        return;
    }
    const QuicRecvState before = slot.recv;
    if (slot.recv == QuicRecvState::Recv && slot.in.final_known())
    {
        slot.recv = QuicRecvState::SizeKnown;
    }
    if (slot.recv == QuicRecvState::SizeKnown &&
        slot.in.contiguous_end() >= (slot.peer_reset_at ? slot.peer_reliable : slot.in.final_size()))
    {
        slot.recv = QuicRecvState::DataRecvd;
    }
    if (slot.stopped)
    {
        drop_stopped(slot);
        return;
    }
    if (slot.in.contiguous_end() > contiguous_before || slot.recv != before)
    {
        raise(slot, kQuicStreamReadable);
    }
}

void QuicStreamSet::drop_stopped(Slot& slot) noexcept
{
    for (cont::ConstSpan<u8> readable = slot.in.readable(); !readable.empty(); readable = slot.in.readable())
    {
        slot.in.consume(readable.size());
    }
    account_consumed(slot, slot.in.read_offset());
    settle_peer_reset(slot);
    if (slot.recv == QuicRecvState::DataRecvd && slot.in.complete())
    {
        slot.recv = QuicRecvState::DataRead;
    }
}

void QuicStreamSet::settle_peer_reset(Slot& slot) noexcept
{
    if (slot.peer_reset_at && receiving(slot.recv) && (slot.stopped || slot.in.read_offset() >= slot.peer_reliable))
    {
        slot.recv = QuicRecvState::ResetRecvd;
        account_consumed(slot, slot.in.final_size());
        raise(slot, kQuicStreamReset);
    }
}

QuicTransportError QuicStreamSet::on_stream(const QuicFrame& frame, Slot& slot, bool early) noexcept
{
    if (m_limits.in_order && frame.offset != slot.in.contiguous_end())
    {
        return QuicTransportError::ProtocolViolation;
    }
    const u64 end = frame.offset + frame.data.size(); // read_quic_frame bounds it by 2^62 - 1
    if (!account_received(slot, end))
    {
        return QuicTransportError::FlowControlError;
    }
    slot.early_end = early ? max_u64(slot.early_end, end) : slot.early_end;
    const u64 contiguous = slot.in.contiguous_end();
    switch (slot.in.insert(frame.offset, frame.data, frame.fin))
    {
        case QuicReceiveResult::Accepted:
            break;
        case QuicReceiveResult::BeyondWindow:
            return QuicTransportError::FlowControlError;
        case QuicReceiveResult::FinalSize:
            return QuicTransportError::FinalSizeError;
        case QuicReceiveResult::Changed:
            return QuicTransportError::ProtocolViolation;
        case QuicReceiveResult::Unusable:
            return QuicTransportError::InternalError;
    }
    after_receive(slot, contiguous);
    return QuicTransportError::NoError;
}

QuicTransportError QuicStreamSet::take_final_size(Slot& slot, u64 final_size) noexcept
{
    if (!account_received(slot, final_size))
    {
        return QuicTransportError::FlowControlError;
    }
    switch (slot.in.insert(final_size, {}, true))
    {
        case QuicReceiveResult::Accepted:
            return QuicTransportError::NoError;
        case QuicReceiveResult::BeyondWindow:
            return QuicTransportError::FlowControlError;
        case QuicReceiveResult::FinalSize:
        case QuicReceiveResult::Changed:
            return QuicTransportError::FinalSizeError;
        case QuicReceiveResult::Unusable:
            break;
    }
    return QuicTransportError::InternalError;
}

QuicTransportError QuicStreamSet::on_reset(const QuicFrame& frame, Slot& slot) noexcept
{
    const u64 final_size = frame.value;
    if (slot.peer_reset_at && frame.error_code != slot.peer_reset_error)
    {
        return QuicTransportError::StreamStateError; // section 5.2 of the draft
    }
    const QuicTransportError error = take_final_size(slot, final_size);
    if (error != QuicTransportError::NoError)
    {
        return error;
    }
    if (receiving(slot.recv))
    {
        // Data not yet read is given up: the stream counts as read to its final size (section 4.5).
        slot.recv = QuicRecvState::ResetRecvd;
        slot.peer_reset_error = frame.error_code; // a STOP_SENDING still owed is not sent from ResetRecvd on
        account_consumed(slot, final_size);
        raise(slot, kQuicStreamReset);
    }
    return QuicTransportError::NoError;
}

QuicTransportError QuicStreamSet::on_reset_at(const QuicFrame& frame, Slot& slot) noexcept
{
    const bool reset_known =
        slot.peer_reset_at || slot.recv == QuicRecvState::ResetRecvd || slot.recv == QuicRecvState::ResetRead;
    if (reset_known && frame.error_code != slot.peer_reset_error)
    {
        return QuicTransportError::StreamStateError;
    }
    const u64 contiguous = slot.in.contiguous_end();
    const QuicTransportError error = take_final_size(slot, frame.value);
    if (error != QuicTransportError::NoError || !receiving(slot.recv))
    {
        return error;
    }
    slot.peer_reset_error = frame.error_code;
    // A reordered frame with a larger size is ignored: the smallest one given stands (section 5.2).
    slot.peer_reliable = slot.peer_reset_at ? min_u64(slot.peer_reliable, frame.offset) : frame.offset;
    slot.peer_reset_at = true;
    after_receive(slot, contiguous);
    settle_peer_reset(slot);
    return QuicTransportError::NoError;
}

void QuicStreamSet::reset_send(Slot& slot, u64 error, u64 reliable_size) noexcept
{
    slot.send = QuicSendState::ResetSent;
    slot.reset_error = error;
    slot.reset_final = slot.out.sent_end(); // the credit the stream consumed (section 4.5); slot_frame adds what follows
    slot.reset_owed = true;
    slot.want_writable = false;
    if (reliable_size != 0U)
    {
        slot.reset_at = true;
        slot.reset_reliable = reliable_size;
        slot.out.truncate(reliable_size);
    }
    relist(slot);
}

void QuicStreamSet::lower_reliable(Slot& slot, u64 reliable_size) noexcept
{
    if (reliable_size < slot.reset_reliable)
    {
        slot.reset_final = max_u64(slot.reset_final, slot.out.sent_end()); // truncate() forgets what was sent above
        slot.reset_reliable = reliable_size;
        slot.reset_at_acked = false;
        slot.reset_owed = true;
        slot.out.truncate(reliable_size);
        relist(slot);
    }
}

void QuicStreamSet::settle_reset_at(Slot& slot) noexcept
{
    if (slot.reset_at_acked && slot.out.all_acked())
    {
        slot.send = QuicSendState::ResetRecvd;
    }
}

QuicTransportError QuicStreamSet::on_frame(const QuicFrame& frame, bool early) noexcept
{
    switch (frame.kind)
    {
        case QuicFrameKind::MaxData:
            m_credit.raise(frame.value);
            return QuicTransportError::NoError;
        case QuicFrameKind::DataBlocked:
            m_window.on_peer_blocked();
            return QuicTransportError::NoError;
        case QuicFrameKind::MaxStreams:
        {
            Type& type = m_types[type_index(true, frame.bidirectional)];
            if (frame.value > type.limit)
            {
                type.limit = frame.value;
                type.blocked_owed = false;
            }
            return QuicTransportError::NoError;
        }
        case QuicFrameKind::StreamsBlocked:
            m_types[type_index(false, frame.bidirectional)].peer_blocked = true;
            return QuicTransportError::NoError;
        case QuicFrameKind::ResetStreamAt:
            if (!m_limits.reset_stream_at)
            {
                return QuicTransportError::FrameEncodingError; // never advertised: a frame of no known type
            }
            break;
        case QuicFrameKind::Stream:
        case QuicFrameKind::ResetStream:
        case QuicFrameKind::StreamDataBlocked:
        case QuicFrameKind::StopSending:
        case QuicFrameKind::MaxStreamData:
            break;
        default:
            return QuicTransportError::InternalError; // not a stream frame: the connection's
    }
    const Type& type = type_of(frame.stream_id);
    const bool to_receiver = frame.kind == QuicFrameKind::Stream || frame.kind == QuicFrameKind::ResetStream ||
                             frame.kind == QuicFrameKind::ResetStreamAt ||
                             frame.kind == QuicFrameKind::StreamDataBlocked;
    if ((to_receiver && !type.receives) || (!to_receiver && !type.sends))
    {
        return QuicTransportError::StreamStateError; // a send-only or receive-only stream (sections 19.4 to 19.13)
    }
    Slot* slot = nullptr;
    QuicTransportError error = QuicTransportError::NoError;
    const Lookup found = reach(frame.stream_id, slot, error);
    if (found != Lookup::Found)
    {
        return (found == Lookup::Error) ? error : QuicTransportError::NoError; // a closed stream's frames are ignored
    }
    switch (frame.kind)
    {
        case QuicFrameKind::Stream:
            error = on_stream(frame, *slot, early);
            break;
        case QuicFrameKind::ResetStream:
            error = on_reset(frame, *slot);
            break;
        case QuicFrameKind::ResetStreamAt:
            error = on_reset_at(frame, *slot);
            break;
        case QuicFrameKind::StreamDataBlocked:
            slot->window.on_peer_blocked();
            break;
        case QuicFrameKind::StopSending:
            if (sending(slot->send))
            {
                reset_send(*slot, frame.error_code, 0U); // the peer's code, as section 3.5 asks
                slot->peer_stop_error = frame.error_code;
                raise(*slot, kQuicStreamStopSending);
            }
            else if (slot->send == QuicSendState::ResetSent && slot->reset_at)
            {
                lower_reliable(*slot, 0U); // the peer reads no more (section 5.4 of the draft)
            }
            break;
        default: // MaxStreamData
            slot->credit.raise(frame.value);
            break;
    }
    relist(*slot);
    try_free(*slot);
    return error;
}

// ---- Sending ----

u64 QuicStreamSet::fresh_limit(const Slot& slot) const noexcept
{
    return min_u64(slot.credit.limit(), slot.out.sent_end() + m_credit.available(m_sent));
}

bool QuicStreamSet::reset_ready(const Slot& slot) const noexcept
{
    // Every reliable byte went out first, so the final size lies within both credits (section 4 of the draft).
    return slot.reset_owed && (!slot.reset_at || slot.out.sent_end() >= slot.reset_reliable);
}

bool QuicStreamSet::data_ready(const Slot& slot) const noexcept
{
    return delivering(slot.send, slot.reset_at) && !slot.out.next(1U, fresh_limit(slot)).empty();
}

bool QuicStreamSet::control_owed(const Slot& slot) const noexcept
{
    const bool stop = slot.stop_owed && (slot.recv == QuicRecvState::Recv || slot.recv == QuicRecvState::SizeKnown);
    // No MAX_STREAM_DATA once the final size is known (section 13.3), nor to a peer asked to stop.
    const bool update = slot.recv == QuicRecvState::Recv && !slot.stopped && slot.window.update_owed();
    const bool blocked = (slot.send == QuicSendState::Ready || slot.send == QuicSendState::Send) &&
                         slot.credit.blocked_owed(slot.out.sent_end(), slot.out.written_end() > slot.out.sent_end());
    return reset_ready(slot) || stop || update || blocked;
}

bool QuicStreamSet::pending(const Slot& slot) const noexcept
{
    return slot.active && (control_owed(slot) || (delivering(slot.send, slot.reset_at) && slot.out.pending()));
}

bool QuicStreamSet::data_waiting_on_connection() const noexcept
{
    if (m_credit.available(m_sent) != 0U)
    {
        return false;
    }
    for (u32 urgency = 0U; urgency < kQuicStreamUrgencies; ++urgency)
    {
        u32 at = m_heads[urgency];
        for (u32 walked = 0U; walked < m_listed[urgency]; ++walked, at = m_slots[at].send_next)
        {
            const Slot& slot = m_slots[at];
            if (delivering(slot.send, slot.reset_at) && slot.out.written_end() > slot.out.sent_end() &&
                slot.credit.limit() > slot.out.sent_end())
            {
                return true;
            }
        }
    }
    return false;
}

bool QuicStreamSet::grant_owed(const Type& type) const noexcept
{
    if (type.local || type.capacity == 0U)
    {
        return false;
    }
    if (type.grant_lost)
    {
        return true;
    }
    const u64 candidate = min_u64(type.freed + type.capacity, kQuicMaxStreams);
    if (candidate <= type.limit)
    {
        return false;
    }
    // Once half the slots were freed since the last grant, or at once to a peer that says it is blocked.
    return type.peer_blocked || (candidate - type.limit) * 2U >= type.capacity;
}

bool QuicStreamSet::has_frame() const noexcept
{
    if (m_window.update_owed())
    {
        return true;
    }
    for (const Type& type : m_types)
    {
        if (grant_owed(type) || type.blocked_owed)
        {
            return true;
        }
    }
    for (u32 urgency = 0U; urgency < kQuicStreamUrgencies; ++urgency)
    {
        u32 at = m_heads[urgency];
        for (u32 walked = 0U; walked < m_listed[urgency]; ++walked, at = m_slots[at].send_next)
        {
            if (control_owed(m_slots[at]) || data_ready(m_slots[at]))
            {
                return true;
            }
        }
    }
    return m_credit.blocked_owed(m_sent, data_waiting_on_connection());
}

bool QuicStreamSet::slot_frame(Slot& slot, usize room, u64 now_ns, u64 rtt_ns, QuicFrame& frame,
                               QuicStreamRecord& record) noexcept
{
    frame = QuicFrame{};
    frame.stream_id = slot.id;
    record = QuicStreamRecord{};
    record.stream_id = slot.id;
    if (reset_ready(slot))
    {
        frame.kind = slot.reset_at ? QuicFrameKind::ResetStreamAt : QuicFrameKind::ResetStream;
        frame.error_code = slot.reset_error;
        slot.reset_final = max_u64(slot.reset_final, slot.out.sent_end());
        frame.value = slot.reset_final;
        frame.offset = slot.reset_reliable;
        if (fits(frame, room))
        {
            slot.reset_owed = false;
            record.kind = frame.kind;
            record.value = slot.reset_reliable;
            return true;
        }
    }
    if (slot.stop_owed && (slot.recv == QuicRecvState::Recv || slot.recv == QuicRecvState::SizeKnown))
    {
        frame.kind = QuicFrameKind::StopSending;
        frame.error_code = slot.stop_error;
        if (fits(frame, room))
        {
            slot.stop_owed = false;
            record.kind = frame.kind;
            return true;
        }
    }
    if (slot.recv == QuicRecvState::Recv && !slot.stopped && slot.window.update_owed())
    {
        frame.kind = QuicFrameKind::MaxStreamData;
        frame.value = kQuicMaxVarint; // the largest the update can name, so the size check holds for it
        if (fits(frame, room))
        {
            frame.value = slot.window.take_update(now_ns, rtt_ns);
            record.kind = frame.kind;
            record.value = frame.value;
            return true;
        }
    }
    if ((slot.send == QuicSendState::Ready || slot.send == QuicSendState::Send) &&
        slot.credit.blocked_owed(slot.out.sent_end(), slot.out.written_end() > slot.out.sent_end()))
    {
        frame.kind = QuicFrameKind::StreamDataBlocked;
        frame.value = slot.credit.limit();
        if (fits(frame, room))
        {
            slot.credit.on_blocked_sent();
            record.kind = frame.kind;
            record.value = frame.value;
            return true;
        }
    }
    if (!delivering(slot.send, slot.reset_at))
    {
        return false;
    }
    const u64 limit = fresh_limit(slot);
    const QuicSendChunk probe = slot.out.next(room, limit);
    if (probe.empty())
    {
        return false;
    }
    // The type, the ID, the offset when not 0 and a Length sized for the whole room.
    const usize header = 1U + quic_varint_bytes(slot.id) +
                         ((probe.offset != 0U) ? quic_varint_bytes(probe.offset) : 0U) + quic_varint_bytes(room);
    if (room <= header)
    {
        return false;
    }
    const QuicSendChunk chunk = slot.out.next(room - header, limit);
    if (chunk.empty())
    {
        return false;
    }
    frame.kind = QuicFrameKind::Stream;
    frame.offset = chunk.offset;
    frame.data = slot.out.bytes_of(chunk);
    frame.fin = chunk.fin;
    const u64 end = chunk.offset + chunk.length;
    if (end > slot.out.sent_end())
    {
        m_sent += end - slot.out.sent_end(); // fresh bytes take the connection's credit; lost ones do not
    }
    slot.out.on_sent(chunk);
    if (chunk.fin)
    {
        slot.send = QuicSendState::DataSent;
    }
    else if (slot.send == QuicSendState::Ready)
    {
        slot.send = QuicSendState::Send;
    }
    record.kind = frame.kind;
    record.value = chunk.offset;
    record.length = static_cast<u32>(chunk.length);
    record.fin = chunk.fin;
    return true;
}

bool QuicStreamSet::next_frame(usize room, u64 now_ns, u64 rtt_ns, QuicFrame& frame, QuicStreamRecord& record) noexcept
{
    frame = QuicFrame{};
    record = QuicStreamRecord{};
    if (m_window.update_owed() && room >= kCreditBytes)
    {
        frame.kind = QuicFrameKind::MaxData;
        frame.value = m_window.take_update(now_ns, rtt_ns);
        record.kind = frame.kind;
        record.value = frame.value;
        return true;
    }
    for (Type& type : m_types)
    {
        if (grant_owed(type) && room >= kCreditBytes)
        {
            type.limit = min_u64(type.freed + type.capacity, kQuicMaxStreams);
            type.grant_lost = false;
            type.peer_blocked = false;
            frame.kind = QuicFrameKind::MaxStreams;
            frame.bidirectional = type.bidirectional;
            frame.value = type.limit;
            record.kind = frame.kind;
            record.value = frame.value;
            record.bidirectional = type.bidirectional;
            return true;
        }
        if (type.blocked_owed)
        {
            frame.kind = QuicFrameKind::StreamsBlocked;
            frame.bidirectional = type.bidirectional;
            frame.value = type.blocked_at;
            if (fits(frame, room))
            {
                type.blocked_owed = false;
                type.blocked_sent = true;
                record.kind = frame.kind;
                record.value = frame.value;
                record.bidirectional = type.bidirectional;
                return true;
            }
            frame = QuicFrame{};
        }
    }
    if (m_credit.blocked_owed(m_sent, data_waiting_on_connection()))
    {
        frame.kind = QuicFrameKind::DataBlocked;
        frame.value = m_credit.limit();
        if (fits(frame, room))
        {
            m_credit.on_blocked_sent();
            record.kind = frame.kind;
            record.value = frame.value;
            return true;
        }
        frame = QuicFrame{};
    }
    for (u32 urgency = 0U; urgency < kQuicStreamUrgencies; ++urgency)
    {
        const u32 listed = m_listed[urgency];
        u32 at = m_heads[urgency];
        for (u32 walked = 0U; walked < listed && at != kNone; ++walked)
        {
            Slot& slot = m_slots[at];
            const u32 next = slot.send_next;
            if (!pending(slot))
            {
                unlist(slot);
            }
            else if (slot_frame(slot, room, now_ns, rtt_ns, frame, record))
            {
                if (frame.kind == QuicFrameKind::Stream && slot.listed)
                {
                    m_heads[urgency] = slot.send_next; // round robin: the next stream at this urgency goes first
                }
                return true;
            }
            at = (m_heads[urgency] == kNone) ? kNone : next;
        }
    }
    frame = QuicFrame{};
    record = QuicStreamRecord{};
    return false;
}

void QuicStreamSet::on_acked(const QuicStreamRecord& record) noexcept
{
    const bool settles = record.kind == QuicFrameKind::Stream || record.kind == QuicFrameKind::ResetStream ||
                         record.kind == QuicFrameKind::ResetStreamAt;
    Slot* const slot = settles ? find(record.stream_id) : nullptr;
    if (slot == nullptr)
    {
        return; // credit frames need nothing, and a freed stream's records are dropped
    }
    if (record.kind == QuicFrameKind::ResetStream)
    {
        if (slot->send == QuicSendState::ResetSent)
        {
            slot->send = QuicSendState::ResetRecvd;
        }
    }
    else if (record.kind == QuicFrameKind::ResetStreamAt)
    {
        if (slot->send == QuicSendState::ResetSent && record.value == slot->reset_reliable)
        {
            slot->reset_at_acked = true; // only the frame with the current, smallest size counts (section 5.3)
            settle_reset_at(*slot);
        }
    }
    else if (slot->send == QuicSendState::ResetSent && slot->reset_at)
    {
        slot->out.on_acked(record.value, record.length, record.fin);
        settle_reset_at(*slot);
    }
    else if (slot->send == QuicSendState::Send || slot->send == QuicSendState::DataSent)
    {
        slot->out.on_acked(record.value, record.length, record.fin);
        if (slot->out.all_acked())
        {
            slot->send = QuicSendState::DataRecvd;
            raise(*slot, kQuicStreamFinished);
        }
        if (slot->want_writable && slot->out.room() > 0U)
        {
            slot->want_writable = false;
            raise(*slot, kQuicStreamWritable);
        }
    }
    try_free(*slot);
}

void QuicStreamSet::on_all_acked() noexcept
{
    for (const Type& type : m_types)
    {
        for (u32 at = 0U; at < type.capacity; ++at)
        {
            const Slot& slot = m_slots[type.first + at];
            if (slot.active)
            {
                QuicStreamRecord record;
                record.stream_id = slot.id;
                record.kind =
                    (slot.send == QuicSendState::ResetSent) ? QuicFrameKind::ResetStream : QuicFrameKind::Stream;
                record.value = slot.out.acked_end(); // the unacknowledged rest fits a length: at most one buffer
                record.length = static_cast<u32>(slot.out.sent_end() - slot.out.acked_end());
                record.fin = slot.out.finished();
                on_acked(record);
            }
        }
    }
}

void QuicStreamSet::on_lost(const QuicStreamRecord& record) noexcept
{
    switch (record.kind)
    {
        case QuicFrameKind::MaxData:
            m_window.on_update_lost(record.value);
            return;
        case QuicFrameKind::DataBlocked:
            m_credit.on_blocked_lost(record.value);
            return;
        case QuicFrameKind::MaxStreams:
        {
            Type& type = m_types[type_index(false, record.bidirectional)];
            type.grant_lost = type.grant_lost || record.value == type.limit;
            return;
        }
        case QuicFrameKind::StreamsBlocked:
        {
            Type& type = m_types[type_index(true, record.bidirectional)];
            if (type.blocked_sent && record.value == type.limit && type.blocked_at == type.limit)
            {
                type.blocked_owed = true; // still blocked at the limit it named
            }
            return;
        }
        default:
            break;
    }
    Slot* const slot = find(record.stream_id);
    if (slot == nullptr)
    {
        return;
    }
    switch (record.kind)
    {
        case QuicFrameKind::Stream:
            // Marked for resending; slot_frame sends data only while the part is sending, and after a reset only
            // what a RESET_STREAM_AT keeps, the rest being gone from the buffer.
            slot->out.on_lost(record.value, record.length, record.fin);
            break;
        case QuicFrameKind::ResetStream:
            slot->reset_owed = slot->reset_owed || slot->send == QuicSendState::ResetSent;
            break;
        case QuicFrameKind::ResetStreamAt:
            // A loss of a frame with a size since lowered needs nothing: the current frame went after it.
            slot->reset_owed = slot->reset_owed ||
                               (slot->send == QuicSendState::ResetSent && !slot->reset_at_acked &&
                                record.value == slot->reset_reliable);
            break;
        case QuicFrameKind::StopSending:
            slot->stop_owed =
                slot->stop_owed ||
                (slot->stopped && (slot->recv == QuicRecvState::Recv || slot->recv == QuicRecvState::SizeKnown));
            break;
        case QuicFrameKind::MaxStreamData:
            if (slot->recv == QuicRecvState::Recv)
            {
                slot->window.on_update_lost(record.value);
            }
            break;
        case QuicFrameKind::StreamDataBlocked:
            slot->credit.on_blocked_lost(record.value);
            break;
        default:
            break;
    }
    relist(*slot);
}

// ---- The application's side ----

QuicStreamError QuicStreamSet::open(bool bidirectional, u64& id) noexcept
{
    Type& type = m_types[type_index(true, bidirectional)];
    if (type.free_count == 0U)
    {
        return QuicStreamError::NoSlot;
    }
    if (type.next >= type.limit)
    {
        if (!(type.blocked_sent && type.blocked_at == type.limit))
        {
            type.blocked_owed = true; // one STREAMS_BLOCKED per limit (section 4.6)
            type.blocked_at = type.limit;
        }
        return QuicStreamError::Blocked;
    }
    id = open_slot(type)->id; // a free slot was checked
    return QuicStreamError::None;
}

QuicStreamError QuicStreamSet::write(u64 id, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept
{
    taken = 0U;
    Slot* const slot = find(id);
    if (slot == nullptr || slot->send == QuicSendState::None)
    {
        return QuicStreamError::Unknown;
    }
    if ((slot->send != QuicSendState::Ready && slot->send != QuicSendState::Send) || slot->out.finished())
    {
        return QuicStreamError::Ended;
    }
    taken = slot->out.write(data);
    if (taken < data.size())
    {
        slot->want_writable = true;
    }
    else if (fin)
    {
        slot->out.finish();
    }
    relist(*slot);
    return QuicStreamError::None;
}

QuicStreamError QuicStreamSet::reset(u64 id, u64 error, u64 reliable_size) noexcept
{
    Slot* const slot = find(id);
    if (slot == nullptr || slot->send == QuicSendState::None)
    {
        return QuicStreamError::Unknown;
    }
    if (slot->send == QuicSendState::ResetSent && slot->reset_at)
    {
        if (error != slot->reset_error || reliable_size > slot->reset_reliable)
        {
            return QuicStreamError::Invalid; // the code never changes and the size only falls (section 5.2)
        }
        lower_reliable(*slot, reliable_size);
        return QuicStreamError::None;
    }
    if (!sending(slot->send))
    {
        return QuicStreamError::Ended;
    }
    if (reliable_size != 0U && (!m_peer_resets_at || reliable_size > slot->out.written_end()))
    {
        return QuicStreamError::Invalid;
    }
    reset_send(*slot, error, reliable_size);
    return QuicStreamError::None;
}

QuicStreamError QuicStreamSet::stop_sending(u64 id, u64 error) noexcept
{
    Slot* const slot = find(id);
    if (slot == nullptr || slot->recv == QuicRecvState::None)
    {
        return QuicStreamError::Unknown;
    }
    if (!receiving(slot->recv) || slot->stopped)
    {
        return QuicStreamError::Ended;
    }
    slot->stopped = true;
    // Every byte already arrived needs no STOP_SENDING (section 3.5): only the rest is dropped.
    slot->stop_owed = slot->recv != QuicRecvState::DataRecvd;
    slot->stop_error = error;
    drop_stopped(*slot);
    relist(*slot);
    try_free(*slot);
    return QuicStreamError::None;
}

QuicStreamError QuicStreamSet::set_urgency(u64 id, u8 urgency) noexcept
{
    Slot* const slot = find(id);
    if (slot == nullptr)
    {
        return QuicStreamError::Unknown;
    }
    if (urgency >= kQuicStreamUrgencies)
    {
        return QuicStreamError::Invalid;
    }
    unlist(*slot);
    slot->urgency = urgency;
    relist(*slot);
    return QuicStreamError::None;
}

QuicStreamRead QuicStreamSet::peek(u64 id) const noexcept
{
    QuicStreamRead read;
    const Slot* const slot = find(id);
    if (slot == nullptr)
    {
        return read;
    }
    if (slot->recv == QuicRecvState::ResetRecvd || slot->recv == QuicRecvState::ResetRead)
    {
        read.reset = true;
        read.reset_error = slot->peer_reset_error;
        return read;
    }
    if (!receiving(slot->recv))
    {
        return read;
    }
    read.data = slot->in.readable();
    const u64 offset = slot->in.read_offset();
    if (slot->peer_reset_at)
    {
        read.data = read.data.first(static_cast<usize>(min_u64(read.data.size(), slot->peer_reliable - offset)));
    }
    else
    {
        read.fin = slot->in.final_known() && offset + read.data.size() == slot->in.final_size();
    }
    if (slot->early_end > offset)
    {
        read.early_bytes = static_cast<usize>(min_u64(slot->early_end - offset, read.data.size()));
    }
    return read;
}

usize QuicStreamSet::copy_readable(u64 id, usize skip, cont::Span<u8> out, bool& ends) const noexcept
{
    ends = false;
    const Slot* const slot = find(id);
    if (slot == nullptr || !receiving(slot->recv))
    {
        return 0U;
    }
    if (slot->peer_reset_at)
    {
        const u64 reliable_left = slot->peer_reliable - min_u64(slot->peer_reliable, slot->in.read_offset() + skip);
        return slot->in.copy_contiguous(skip, out.first(static_cast<usize>(min_u64(out.size(), reliable_left))));
    }
    const usize copied = slot->in.copy_contiguous(skip, out);
    ends = slot->in.final_known() && slot->in.read_offset() + skip + copied >= slot->in.final_size();
    return copied;
}

QuicStreamError QuicStreamSet::consume(u64 id, usize count, bool keep_end) noexcept
{
    Slot* const slot = find(id);
    if (slot == nullptr || slot->recv == QuicRecvState::None)
    {
        return QuicStreamError::Unknown;
    }
    if (!receiving(slot->recv))
    {
        return QuicStreamError::Ended;
    }
    const u64 readable_end =
        slot->peer_reset_at ? min_u64(slot->in.contiguous_end(), slot->peer_reliable) : slot->in.contiguous_end();
    if (count > readable_end - slot->in.read_offset())
    {
        return QuicStreamError::Invalid;
    }
    slot->in.consume(count);
    account_consumed(*slot, slot->in.read_offset());
    settle_peer_reset(*slot);
    if (slot->recv == QuicRecvState::DataRecvd && slot->in.complete() && !keep_end)
    {
        slot->recv = QuicRecvState::DataRead;
    }
    relist(*slot);
    try_free(*slot);
    return QuicStreamError::None;
}

bool QuicStreamSet::poll(QuicStreamEvent& event) noexcept
{
    if (m_event_head == kNone)
    {
        return false;
    }
    Slot& slot = m_slots[m_event_head];
    m_event_head = slot.event_next;
    if (m_event_head == kNone)
    {
        m_event_tail = kNone;
    }
    slot.event_next = kNone;
    event = QuicStreamEvent{};
    event.id = slot.id;
    event.slot = index_of(slot);
    event.events = slot.events;
    event.reset_error = slot.peer_reset_error;
    event.stop_error = slot.peer_stop_error;
    slot.events = 0U;
    if ((event.events & kQuicStreamReset) != 0U && slot.recv == QuicRecvState::ResetRecvd)
    {
        slot.recv = QuicRecvState::ResetRead; // the application was told
    }
    try_free(slot);
    return true;
}

usize QuicStreamSet::send_room(u64 id, usize wanted) noexcept
{
    Slot* const slot = find(id);
    if (slot == nullptr || (slot->send != QuicSendState::Ready && slot->send != QuicSendState::Send) ||
        slot->out.finished())
    {
        return 0U;
    }
    const usize room = slot->out.room();
    if (room < wanted)
    {
        slot->want_writable = true;
    }
    return room;
}

u32 QuicStreamSet::slot_of(u64 id) const noexcept
{
    const u32 slot = locate(id);
    return (slot == kNone) ? kQuicNoStreamSlot : slot;
}

// ---- State ----

QuicSendState QuicStreamSet::send_state(u64 id) const noexcept
{
    const Slot* const slot = find(id);
    return (slot != nullptr) ? slot->send : QuicSendState::None;
}

QuicRecvState QuicStreamSet::recv_state(u64 id) const noexcept
{
    const Slot* const slot = find(id);
    return (slot != nullptr) ? slot->recv : QuicRecvState::None;
}

u32 QuicStreamSet::open_streams() const noexcept
{
    return m_types[0].open + m_types[1].open + m_types[2].open + m_types[3].open;
}

u64 QuicStreamSet::granted(bool bidirectional) const noexcept
{
    return m_types[type_index(false, bidirectional)].limit;
}

u64 QuicStreamSet::peer_limit(bool bidirectional) const noexcept
{
    return m_types[type_index(true, bidirectional)].limit;
}

} // namespace crd::net
