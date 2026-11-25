// crd-net -- the transport's I/O loop (4.2a; docs/design/network-foundation.md#ingress).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/time/clocks.hpp>

namespace crd::net
{
namespace
{
constexpr u32 kMaxBatch = 1024U;
constexpr u32 kMaxQueueSlots = 65536U;
// Datagrams one send() of a flush hands a socket at most: two of the host's longest trains, so a call carries whole
// trains however a quantum's lie in it.
constexpr usize kSendBatch = 2U * kMaxTrainDatagrams;
constexpr u64 kAnswerRetryNs = 1000000U; // an answer the host was too busy to take is offered again within 1 ms
} // namespace

void IngressHandler::on_too_big(const ReceivedDatagram& /*report*/, u64 /*now_ns*/) noexcept {}

u64 HostClock::monotonic_ns() const noexcept
{
    const i64 now = time::TickClock::now_ns();
    return (now > 0) ? static_cast<u64>(now) : 0U;
}

u64 HostClock::unix_seconds() const noexcept
{
    const i64 now = time::WallClock::now().ns_since_epoch();
    return (now > 0) ? static_cast<u64>(now) / kNanosPerSecond : 0U;
}

// ---- SendQueue ----

SendQueue::SendQueue(memory::IAllocator& allocator, u32 slots, usize slot_bytes, u32 sockets) noexcept
    : m_allocator(allocator), m_slots(slots), m_slot_bytes(slot_bytes)
{
    if (slots == 0U || slots > kMaxQueueSlots || slot_bytes == 0U || slot_bytes > kMaxUdpPayloadV6 || sockets == 0U ||
        sockets > kMaxSetSockets)
    {
        return; // not ready, with no sockets: every room() is empty and nothing is held or counted
    }
    m_sockets = sockets;
    if (sockets > 1U && slots >= sockets)
    {
        // The shares together take at most half the slots, and one each where only the slots for one each exist.
        const u32 half = slots / (2U * sockets);
        m_share = (half > 1U) ? half : 1U;
    }
    Entry* const entries = memory::try_construct_array<Entry>(allocator, slots);
    u32* const order = memory::try_construct_array<u32>(allocator, slots);
    u8* const bytes = memory::try_construct_array<u8>(allocator, usize{slots} * slot_bytes);
    if (entries == nullptr || order == nullptr || bytes == nullptr)
    {
        memory::destroy_array(allocator, entries, slots);
        memory::destroy_array(allocator, order, slots);
        memory::destroy_array(allocator, bytes, usize{slots} * slot_bytes);
        return;
    }
    for (u32 index = 0U; index < slots; ++index)
    {
        order[index] = index;
    }
    m_entries = entries;
    m_order = order;
    m_bytes = bytes;
}

SendQueue::~SendQueue()
{
    for (u32 socket = 0U; socket < kMaxSetSockets; ++socket)
    {
        if (m_lent_per_socket[socket] != 0U)
        {
            static_cast<void>(m_lenders[socket]->sends_released(true)); // then the host reads none of its slots
        }
    }
    if (m_entries != nullptr)
    {
        memory::destroy_array(m_allocator, m_entries, m_slots);
        memory::destroy_array(m_allocator, m_order, m_slots);
        memory::destroy_array(m_allocator, m_bytes, usize{m_slots} * m_slot_bytes);
    }
}

bool SendQueue::admits(u32 socket) const noexcept
{
    if (m_entries == nullptr || m_queued + m_lent == m_slots || socket >= m_sockets)
    {
        return false;
    }
    // The slots the other sockets' shares still lack stay free (S33, #sockets), lent ones counted as queued.
    u32 owed = 0U;
    for (u32 other = 0U; other < m_sockets; ++other)
    {
        const u32 holds = m_per_socket[other] + m_lent_per_socket[other];
        if (other != socket && holds < m_share)
        {
            owed += m_share - holds;
        }
    }
    return m_slots - m_queued - m_lent > owed;
}

cont::Span<u8> SendQueue::room(u32 socket) noexcept
{
    if (!admits(socket))
    {
        return {};
    }
    return cont::Span<u8>(m_bytes + (usize{m_order[m_queued + m_lent]} * m_slot_bytes), m_slot_bytes);
}

u32 SendQueue::held() const noexcept
{
    u32 held = 0U;
    for (u32 socket = 0U; socket < m_sockets; ++socket)
    {
        held |= admits(socket) ? 0U : (1U << socket);
    }
    return held;
}

bool SendQueue::push(usize bytes, const Endpoint& peer, Ecn ecn, u32 socket) noexcept
{
    const cont::Span<u8> slot = room(socket);
    if (slot.empty() || bytes == 0U || bytes > slot.size())
    {
        return false;
    }
    // The first free slot joins the queued ones, the first lent one moving behind the rest.
    const u32 free = m_order[m_queued + m_lent];
    m_order[m_queued + m_lent] = m_order[m_queued];
    m_order[m_queued] = free;
    m_entries[free] = Entry{OutgoingDatagram{slot.first(bytes), peer, ecn}, socket};
    ++m_queued;
    ++m_per_socket[socket];
    return true;
}

SendFlush SendQueue::flush(const SocketSet& sockets) noexcept
{
    SendFlush flushed;
    if (m_queued + m_lent == 0U)
    {
        return flushed;
    }
    for (u32 index = 0U; index < sockets.size() && m_queued != 0U; ++index)
    {
        send_through(sockets[index], index, flushed);
    }
    u64 released[kMaxSetSockets];
    for (u32 socket = 0U; socket < kMaxSetSockets; ++socket)
    {
        released[socket] = (m_lenders[socket] != nullptr) ? m_lenders[socket]->sends_released(false) : 0U;
        m_per_socket[socket] = 0U;
        m_lent_per_socket[socket] = 0U;
    }
    // What is left in order, then what the host still reads, recounted per socket; the rest becomes free.
    const u32 used = m_queued + m_lent;
    u32 kept = 0U;
    for (u32 position = 0U; position < used; ++position)
    {
        Entry& entry = m_entries[m_order[position]];
        if (!entry.done && entry.socket >= sockets.size())
        {
            entry.done = true; // no such socket: it can never leave
            ++flushed.dropped;
        }
        if (!entry.done)
        {
            const u32 slot = m_order[position];
            m_order[position] = m_order[kept];
            m_order[kept++] = slot;
            if (entry.socket < m_sockets)
            {
                ++m_per_socket[entry.socket];
            }
        }
    }
    m_queued = kept;
    for (u32 position = m_queued; position < used; ++position)
    {
        Entry& entry = m_entries[m_order[position]];
        entry.lent = entry.lent && entry.socket < m_sockets && entry.number >= released[entry.socket];
        if (entry.lent)
        {
            const u32 slot = m_order[position];
            m_order[position] = m_order[kept];
            m_order[kept++] = slot;
            ++m_lent_per_socket[entry.socket];
        }
    }
    m_lent = kept - m_queued;
    return flushed;
}

void SendQueue::send_through(DatagramSocket& socket, u32 index, SendFlush& flushed) noexcept
{
    // One datagram past the batch tells whether its last train goes on: a segmenting socket makes trains of each call
    // (segment_train), so a batch ends before a train it would cut, and that train leaves whole in the next call.
    OutgoingDatagram batch[kSendBatch + 1U];
    u32 slots[kSendBatch + 1U];
    u32 places[kSendBatch + 1U]; // each one's position in the order
    u32 position = 0U;
    for (;;)
    {
        usize count = 0U;
        for (; position < m_queued && count <= kSendBatch; ++position)
        {
            const Entry& entry = m_entries[m_order[position]];
            if (!entry.done && entry.socket == index)
            {
                batch[count] = entry.datagram;
                places[count] = position;
                slots[count++] = m_order[position];
            }
        }
        const SocketFeatures features = socket.features();
        m_lenders[index] = features.lent_send ? &socket : m_lenders[index];
        const bool more = count > kSendBatch;
        if (more)
        {
            usize last = 0U; // where the train holding the datagram past the batch begins
            for (usize at = 0U; features.segmented_send && at <= kSendBatch;)
            {
                last = at;
                at += segment_train(cont::ConstSpan<OutgoingDatagram>(batch + at, count - at));
            }
            count = (last != 0U) ? last : kSendBatch;
            position = places[count];
        }
        for (usize at = 0U; at < count;)
        {
            const u64 first = features.lent_send ? socket.sends_taken() : 0U;
            const BatchResult result = socket.send_stable(cont::ConstSpan<OutgoingDatagram>(batch + at, count - at));
            for (usize sent = 0U; sent < result.count; ++sent)
            {
                Entry& entry = m_entries[slots[at + sent]];
                entry.done = true;
                entry.lent = features.lent_send;
                entry.number = first + sent;
            }
            at += result.count;
            flushed.sent += result.count;
            if (result.error == SocketError::Busy || (result.error == SocketError::None && result.count == 0U))
            {
                return; // the rest of this socket's wait for the next flush
            }
            if (result.error != SocketError::None && at < count)
            {
                // That datagram alone cannot leave (an unreachable peer, a refused address): it is given up.
                m_entries[slots[at++]].done = true;
                ++flushed.dropped;
            }
        }
        if (!more)
        {
            return;
        }
    }
}

// ---- IoLoop ----

IoLoop::IoLoop(memory::IAllocator& allocator, const SocketSet& sockets, const TransportClock& clock, Ingress& ingress,
               IngressHandler& handler, const IoLoopConfig& config) noexcept
    : m_allocator(allocator), m_sockets(sockets), m_clock(clock), m_ingress(ingress), m_handler(handler),
      m_config(config), m_answers(allocator, config.queued_answers, kQuicMaxStatelessAnswerBytes, sockets.size())
{
    if (!sockets.ready() || !ingress.ready() || !m_answers.ready() || config.batch == 0U || config.batch > kMaxBatch ||
        config.slot_bytes < kQuicMinInitialDatagram || config.slot_bytes > kMaxUdpPayloadV6 ||
        config.batches_per_poll == 0U || config.max_wait_ns == 0U)
    {
        return;
    }
    u32 coalescing = 0U;
    u32 lending = 0U;
    for (u32 socket = 0U; socket < sockets.size(); ++socket)
    {
        const SocketFeatures features = sockets[socket].features();
        coalescing |= features.coalesced_receive ? (1U << socket) : 0U;
        lending |= features.lent_receive ? (1U << socket) : 0U;
        if (features.receive_bytes != 0U && features.receive_bytes < config.slot_bytes)
        {
            return; // a datagram the loop reads whole would arrive cut
        }
    }
    if (coalescing != 0U && (config.train_slots == 0U || config.train_slots > kMaxBatch))
    {
        return;
    }
    // Storage only for the sockets that read into it: a socket that lends fills empty slots with its own buffers.
    const u32 every = (1U << sockets.size()) - 1U;
    const bool plain = ((coalescing | lending) & every) != every;
    const u32 trains = ((coalescing & ~lending) != 0U) ? config.train_slots : 0U;
    u32 lent = (lending != 0U) ? config.batch : 0U;
    if ((coalescing & lending) != 0U && config.train_slots > lent)
    {
        lent = config.train_slots;
    }
    const usize buffer_bytes = plain ? usize{config.batch} * config.slot_bytes : 0U;
    const usize train_bytes = usize{trains} * kMaxTrainBytes;
    ReceivedDatagram* const slots = memory::try_construct_array<ReceivedDatagram>(allocator, config.batch);
    u8* const buffers = plain ? memory::try_construct_array<u8>(allocator, buffer_bytes) : nullptr;
    ReceivedDatagram* const train_slots =
        (trains != 0U) ? memory::try_construct_array<ReceivedDatagram>(allocator, trains) : nullptr;
    u8* const train_buffers = (trains != 0U) ? memory::try_construct_array<u8>(allocator, train_bytes) : nullptr;
    ReceivedDatagram* const lent_slots =
        (lent != 0U) ? memory::try_construct_array<ReceivedDatagram>(allocator, lent) : nullptr;
    const bool trains_held = trains == 0U || (train_slots != nullptr && train_buffers != nullptr);
    if (slots == nullptr || (plain && buffers == nullptr) || !trains_held || (lent != 0U && lent_slots == nullptr))
    {
        memory::destroy_array(allocator, slots, config.batch);
        memory::destroy_array(allocator, buffers, buffer_bytes);
        memory::destroy_array(allocator, train_slots, trains);
        memory::destroy_array(allocator, train_buffers, train_bytes);
        memory::destroy_array(allocator, lent_slots, lent);
        return;
    }
    for (u32 index = 0U; plain && index < config.batch; ++index)
    {
        slots[index].storage = cont::Span<u8>(buffers + (usize{index} * config.slot_bytes), config.slot_bytes);
    }
    for (u32 index = 0U; index < trains; ++index)
    {
        train_slots[index].storage = cont::Span<u8>(train_buffers + (usize{index} * kMaxTrainBytes), kMaxTrainBytes);
    }
    m_slots = slots;
    m_buffers = buffers;
    m_buffer_bytes = buffer_bytes;
    m_coalescing = coalescing;
    m_trains = train_slots;
    m_train_buffers = train_buffers;
    m_train_count = trains;
    m_lending = lending;
    m_lent = lent_slots;
    m_lent_count = lent;
}

IoLoop::~IoLoop()
{
    memory::destroy_array(m_allocator, m_slots, m_config.batch);
    memory::destroy_array(m_allocator, m_buffers, m_buffer_bytes);
    memory::destroy_array(m_allocator, m_trains, m_train_count);
    memory::destroy_array(m_allocator, m_train_buffers, usize{m_train_count} * kMaxTrainBytes);
    memory::destroy_array(m_allocator, m_lent, m_lent_count);
}

u64 IoLoop::poll() noexcept
{
    if (m_slots == nullptr)
    {
        return kNeverNs;
    }
    ++m_counters.polls;
    const u64 now = m_clock.monotonic_ns();
    const u64 unix_seconds = m_clock.unix_seconds();
    flush_answers();
    // Each socket is read until it has handed on batches_per_poll plain batches of datagrams this turn, however they
    // arrive (the receive that reaches the bound may end inside a train): a train slot holds one datagram from a peer
    // that does not segment, so counting receives would cut a coalescing socket's turn to train_slots datagrams each.
    const u64 budget = u64{m_config.batch} * m_config.batches_per_poll;
    for (u32 socket = 0U; socket < m_sockets.size(); ++socket)
    {
        const u64 start = m_counters.received;
        while (m_counters.received - start < budget)
        {
            BatchResult result;
            const bool full = receive_batch(socket, now, unix_seconds, result);
            if (result.error != SocketError::None)
            {
                ++m_counters.receive_errors;
                break;
            }
            if (!full)
            {
                break;
            }
        }
    }
    flush_answers();
    return m_handler.on_timers(m_clock.monotonic_ns());
}

void IoLoop::run(const std::atomic<bool>& stop) noexcept
{
    while (!stop.load(std::memory_order_acquire))
    {
        const u64 next = poll();
        const u64 now = m_clock.monotonic_ns();
        if (stop.load(std::memory_order_acquire))
        {
            return;
        }
        if (next <= now)
        {
            continue;
        }
        u64 wait = next - now;
        wait = (wait < m_config.max_wait_ns) ? wait : m_config.max_wait_ns;
        if (m_answers.queued() != 0U && wait > kAnswerRetryNs)
        {
            wait = kAnswerRetryNs;
        }
        (void)m_sockets.wait_readable(wait);
    }
}

bool IoLoop::receive_batch(u32 socket, u64 now_ns, u64 unix_seconds, BatchResult& result) noexcept
{
    const bool trains = (m_coalescing & (1U << socket)) != 0U;
    const bool lent = (m_lending & (1U << socket)) != 0U;
    ReceivedDatagram* slots = trains ? m_trains : m_slots;
    if (lent)
    {
        slots = m_lent;
    }
    const u32 count = trains ? m_config.train_slots : m_config.batch;
    for (u32 index = 0U; lent && index < count; ++index)
    {
        slots[index].storage = cont::Span<u8>(); // each slot asks for the socket's own buffer
    }
    result = m_sockets[socket].receive(cont::Span<ReceivedDatagram>(slots, count));
    for (usize index = 0U; index < result.count; ++index)
    {
        ReceivedDatagram& slot = slots[index];
        slot.socket = socket;
        // A lent datagram is cut to slot_bytes below as a slot of that size would have cut it.
        if ((!trains && !lent) || slot.too_big != 0U)
        {
            ++m_counters.received;
            deliver(slot, now_ns, unix_seconds);
            continue;
        }
        // A train slot holds a train or one datagram: each datagram in place, one past slot_bytes truncated as a slot
        // of that size would have made it, and a truncated slot's cut last datagram truncated (a shorter datagram
        // only ever ends a train).
        const usize segment = (slot.segment != 0U) ? slot.segment : slot.size;
        m_counters.trains += (slot.segment != 0U) ? 1U : 0U;
        usize at = 0U;
        do
        {
            const usize size = (slot.size - at < segment) ? slot.size - at : segment;
            ReceivedDatagram one = slot;
            one.storage = slot.storage.subspan(at, size);
            one.size = (size < m_config.slot_bytes) ? size : m_config.slot_bytes;
            one.truncated = size > m_config.slot_bytes || (slot.truncated && (slot.segment == 0U || size < segment));
            one.segment = 0U;
            ++m_counters.received;
            deliver(one, now_ns, unix_seconds);
            at += segment;
        } while (at < slot.size);
    }
    return result.count == count;
}

void IoLoop::deliver(ReceivedDatagram& datagram, u64 now_ns, u64 unix_seconds) noexcept
{
    if (datagram.too_big != 0U)
    {
        ++m_counters.too_big;
        m_handler.on_too_big(datagram, now_ns);
        return;
    }
    const IngressDecision decision = m_ingress.classify(datagram.storage.first(datagram.size), datagram.truncated,
                                                        datagram.peer, now_ns, unix_seconds);
    switch (decision.action)
    {
    case IngressAction::Route:
        m_handler.on_route(decision, datagram, now_ns);
        break;
    case IngressAction::Admit:
        m_handler.on_admit(decision, datagram, now_ns);
        break;
    case IngressAction::Early:
        m_handler.on_early(decision, datagram, now_ns);
        break;
    case IngressAction::StatelessReset:
        if (!m_handler.on_unrouted(decision, datagram, now_ns))
        {
            queue_answer(decision, datagram, unix_seconds);
        }
        break;
    case IngressAction::Retry:
    case IngressAction::NegotiateVersion:
    case IngressAction::InvalidToken:
        queue_answer(decision, datagram, unix_seconds);
        break;
    case IngressAction::Drop:
        // A stateless reset can be shorter than any packet this side would open (RFC 9000 section 10.3.1).
        if (decision.reason == DropReason::UnknownConnection ||
            (decision.reason == DropReason::TooShortToProtect && decision.header.type == QuicPacketType::OneRtt))
        {
            static_cast<void>(m_handler.on_unrouted(decision, datagram, now_ns));
        }
        break;
    }
}

void IoLoop::queue_answer(const IngressDecision& decision, const ReceivedDatagram& datagram,
                          u64 unix_seconds) noexcept
{
    const cont::Span<u8> out = m_answers.room(datagram.socket);
    if (out.empty())
    {
        ++m_counters.answers_dropped;
        return;
    }
    const usize bytes = m_ingress.answer(decision, datagram.peer, unix_seconds, out);
    // Admitted just now, so it queues; an answer not built (0 bytes) queues nothing and is the ingress's to count.
    static_cast<void>(m_answers.push(bytes, datagram.peer, Ecn::NotEct, datagram.socket));
}

void IoLoop::flush_answers() noexcept
{
    const SendFlush flushed = m_answers.flush(m_sockets);
    m_counters.answers_sent += flushed.sent;
    m_counters.answers_dropped += flushed.dropped;
}

} // namespace crd::net
