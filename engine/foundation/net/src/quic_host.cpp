// crd-net -- what the two QUIC hosts share: the connections behind an I/O loop (4.2a;
// docs/design/network-foundation.md#quic-client).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/quic_host.hpp>

#include <cstring>
#include <new>

namespace crd::net
{
namespace
{
constexpr usize kPeerKeyBytes = 16U + 2U; // an IPv6 address and a port

// A peer's address and port as the key of the host's peer index.
usize peer_key(const Endpoint& peer, u8 (&key)[kPeerKeyBytes]) noexcept
{
    const cont::ConstSpan<u8> address = peer.address.view();
    std::memcpy(key, address.data(), address.size());
    key[address.size()] = static_cast<u8>(peer.port >> 8U);
    key[address.size() + 1U] = static_cast<u8>(peer.port & 0xFFU);
    return address.size() + 2U;
}

bool limits_valid(const QuicHostLimits& limits) noexcept
{
    return limits.connections != 0U && limits.connections <= kQuicMaxHostConnections && limits.send_retry_ns != 0U &&
           limits.datagram_bytes >= kQuicMinInitialDatagram;
}
} // namespace

QuicHost::QuicHost(memory::IAllocator& allocator, DatagramSocket& socket, ConnectionIdTable& routes,
                   const crypto::QuicResetKey& reset, QuicHostEvents& events, const QuicHostLimits& limits) noexcept
    : m_allocator(allocator), m_socket(socket), m_routes(routes), m_reset(reset), m_events(events), m_limits(limits),
      m_by_peer(allocator, limits_valid(limits) ? limits.connections : 1U),
      m_queue(allocator, limits.send_slots, limits.datagram_bytes)
{
}

bool QuicHost::open_pool() noexcept
{
    const u32 count = m_limits.connections;
    if (m_slots != nullptr || !limits_valid(m_limits) || !m_routes.ready() || !m_by_peer.ready() || !m_queue.ready())
    {
        return false;
    }
    Slot* const slots = memory::try_construct_array<Slot>(m_allocator, count);
    QuicConnection* const storage = memory::try_allocate_array<QuicConnection>(m_allocator, usize{count} + 1U);
    QuicRoutedIds* const ids = memory::try_allocate_array<QuicRoutedIds>(m_allocator, count);
    u32* const heap = memory::try_construct_array<u32>(m_allocator, count);
    u32* const due = memory::try_construct_array<u32>(m_allocator, count);
    if (slots == nullptr || storage == nullptr || ids == nullptr || heap == nullptr || due == nullptr)
    {
        memory::destroy_array(m_allocator, slots, count);
        if (storage != nullptr)
        {
            memory::deallocate_array(m_allocator, storage);
        }
        if (ids != nullptr)
        {
            memory::deallocate_array(m_allocator, ids);
        }
        memory::destroy_array(m_allocator, heap, count);
        memory::destroy_array(m_allocator, due, count);
        return false;
    }
    for (u32 index = 0U; index < count; ++index)
    {
        ::new (static_cast<void*>(ids + index)) QuicRoutedIds(m_routes, m_reset, index);
        slots[index].cell = storage + index;
        slots[index].next_free = (index + 1U < count) ? index + 1U : kNoConnection;
    }
    m_slots = slots;
    m_storage = storage;
    m_spare = storage + count;
    m_ids = ids;
    m_heap = heap;
    m_due = due;
    m_free = 0U;
    return true;
}

QuicHost::~QuicHost()
{
    if (m_slots == nullptr)
    {
        return;
    }
    const u32 count = m_limits.connections;
    for (u32 index = 0U; index < count; ++index)
    {
        if (m_slots[index].connection != nullptr)
        {
            m_slots[index].connection->~QuicConnection(); // its IDs leave the table
        }
        m_ids[index].~QuicRoutedIds();
    }
    memory::destroy_array(m_allocator, m_slots, count);
    memory::deallocate_array(m_allocator, m_storage);
    memory::deallocate_array(m_allocator, m_ids);
    memory::destroy_array(m_allocator, m_heap, count);
    memory::destroy_array(m_allocator, m_due, count);
}

// ---- Building connections ----

QuicConnection* QuicHost::build(u32 index, QuicConnectionConfig config) noexcept
{
    u8 seed[crypto::kPrfKeyBytes] = {};
    config.ids = &m_ids[index];
    if (config.entropy_seed.size() == sizeof(seed))
    {
        std::memcpy(seed, config.entropy_seed.data(), sizeof(seed));
        for (usize byte = 0U; byte < 8U; ++byte)
        {
            seed[byte] = static_cast<u8>(seed[byte] ^ static_cast<u8>(m_built >> (8U * byte)));
        }
        config.entropy_seed = cont::ConstSpan<u8>(seed, sizeof(seed)); // copied by the connection
    }
    ++m_built;
    QuicConnection* const connection = ::new (static_cast<void*>(m_spare)) QuicConnection(m_allocator, config);
    if (!connection->ready())
    {
        connection->~QuicConnection();
        return nullptr;
    }
    return connection;
}

void QuicHost::discard(QuicConnection& connection) noexcept
{
    connection.~QuicConnection(); // in the spare, which stays the spare
}

void QuicHost::enlist(u32 index, QuicConnection& connection, const Endpoint& peer, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    m_free = slot.next_free;
    slot.next_free = kNoConnection;
    m_spare = slot.cell;
    slot.cell = &connection;
    slot.connection = &connection;
    slot.peer = peer;
    index_peer(index);
    ++m_live;
    schedule(index, now_ns);
}

void QuicHost::succeed(u32 index, QuicConnection& successor, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    slot.connection->~QuicConnection(); // its IDs leave the table; the successor's stay
    m_spare = slot.cell;
    slot.cell = &successor;
    slot.connection = &successor;
    index_peer(index);
    schedule(index, now_ns);
}

// ---- The loop's events ----

void QuicHost::on_route(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept
{
    QuicConnection* const connection = live_connection(decision.connection);
    if (connection == nullptr)
    {
        return;
    }
    if (!(datagram.peer == m_slots[decision.connection].peer))
    {
        ++m_counters.foreign_path;
        return;
    }
    connection->receive(datagram.storage.first(datagram.size), QuicEcn::NotEct, now_ns);
    schedule(decision.connection, now_ns);
}

bool QuicHost::on_unrouted(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept
{
    if (m_slots == nullptr || decision.header.type != QuicPacketType::OneRtt)
    {
        return false;
    }
    u8 key[kPeerKeyBytes] = {};
    const u32 index = m_by_peer.find(cont::ConstSpan<u8>(key, peer_key(datagram.peer, key)));
    QuicConnection* const connection = live_connection(index); // its peer is the key's (index_peer)
    if (connection == nullptr || !connection->stateless_reset(datagram.storage.first(datagram.size), now_ns))
    {
        return false;
    }
    ++m_counters.peer_resets;
    schedule(index, now_ns);
    return true;
}

u64 QuicHost::on_timers(u64 now_ns) noexcept
{
    if (m_slots == nullptr)
    {
        return kNeverNs;
    }
    // The connections due now are taken first, so one served again at once waits for the next turn.
    u32 due = 0U;
    while (m_heap_size != 0U && m_slots[m_heap[0]].due_ns <= now_ns)
    {
        const u32 index = m_heap[0];
        unschedule(index);
        m_due[due++] = index;
    }
    for (u32 next = 0U; next < due; ++next)
    {
        if (m_slots[m_due[next]].connection != nullptr)
        {
            serve(m_due[next], now_ns);
        }
    }
    flush();
    after_turn(now_ns);
    u64 next = (m_heap_size != 0U) ? m_slots[m_heap[0]].due_ns : kNeverNs;
    if (m_queue.queued() != 0U && now_ns + m_limits.send_retry_ns < next)
    {
        next = now_ns + m_limits.send_retry_ns;
    }
    return next;
}

// ---- The application's side ----

QuicConnection* QuicHost::connection(QuicHandle handle) noexcept
{
    QuicConnection* const connection = live_connection(handle.slot);
    return (connection != nullptr && m_slots[handle.slot].generation == handle.generation) ? connection : nullptr;
}

void QuicHost::wake(QuicHandle handle) noexcept
{
    if (connection(handle) != nullptr)
    {
        schedule(handle.slot, 0U);
    }
}

QuicConnection* QuicHost::live_connection(u32 index) noexcept
{
    return (m_slots == nullptr || index >= m_limits.connections) ? nullptr : m_slots[index].connection;
}

QuicHandle QuicHost::handle_of(u32 index) const noexcept
{
    return QuicHandle{index, m_slots[index].generation};
}

const Endpoint& QuicHost::peer_of(u32 index) const noexcept
{
    return m_slots[index].peer;
}

cont::Span<u8> QuicHost::room() noexcept
{
    cont::Span<u8> room = m_queue.room();
    if (room.empty())
    {
        flush();
        room = m_queue.room();
    }
    return room;
}

void QuicHost::push(usize bytes, const Endpoint& peer) noexcept
{
    m_queue.push(bytes, peer);
}

// ---- Serving ----

bool QuicHost::replace(u32, QuicConnection&, u64) noexcept
{
    return false;
}

void QuicHost::release(u32, const QuicConnection&) noexcept {}

void QuicHost::after_turn(u64) noexcept {}

void QuicHost::serve(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    QuicConnection& connection = *slot.connection;
    if (connection.next_timer_ns() <= now_ns)
    {
        connection.on_timeout(now_ns);
    }
    if (connection.state() != QuicConnectionState::Closed)
    {
        service(index, connection, now_ns);
    }
    const bool drained = drain(slot, now_ns);
    if (connection.state() == QuicConnectionState::Closed)
    {
        reclaim(index, now_ns);
        return;
    }
    u64 due = connection.next_timer_ns();
    if (!drained && now_ns + m_limits.send_retry_ns < due)
    {
        due = now_ns + m_limits.send_retry_ns;
    }
    schedule(index, due);
}

bool QuicHost::drain(Slot& slot, u64 now_ns) noexcept
{
    for (;;)
    {
        const cont::Span<u8> out = room();
        if (out.empty())
        {
            return false;
        }
        const usize written = slot.connection->send(out, now_ns);
        if (written == 0U)
        {
            return true;
        }
        m_queue.push(written, slot.peer);
    }
}

void QuicHost::flush() noexcept
{
    const SendFlush flushed = m_queue.flush(m_socket);
    m_counters.datagrams_sent += flushed.sent;
    m_counters.send_dropped += flushed.dropped;
}

void QuicHost::reclaim(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    unschedule(index);
    unindex_peer(index);
    if (replace(index, *slot.connection, now_ns))
    {
        return;
    }
    m_events.on_closed(handle_of(index), *slot.connection);
    release(index, *slot.connection);
    slot.connection->~QuicConnection(); // its IDs leave the table
    slot.connection = nullptr;
    ++slot.generation;
    slot.next_free = m_free;
    m_free = index;
    --m_live;
    ++m_counters.closed;
}

void QuicHost::index_peer(u32 index) noexcept
{
    Slot& slot = m_slots[index];
    u8 key[kPeerKeyBytes] = {};
    slot.peer_indexed = m_by_peer.insert(cont::ConstSpan<u8>(key, peer_key(slot.peer, key)), index) == RouteError::None;
}

void QuicHost::unindex_peer(u32 index) noexcept
{
    Slot& slot = m_slots[index];
    if (!slot.peer_indexed)
    {
        return;
    }
    u8 key[kPeerKeyBytes] = {};
    const cont::ConstSpan<u8> peer(key, peer_key(slot.peer, key));
    if (m_by_peer.find(peer) == index)
    {
        static_cast<void>(m_by_peer.erase(peer));
    }
    slot.peer_indexed = false;
}

// ---- The heap ----

void QuicHost::schedule(u32 index, u64 due_ns) noexcept
{
    Slot& slot = m_slots[index];
    slot.due_ns = due_ns;
    if (slot.heap == kNotQueued)
    {
        put(m_heap_size, index);
        ++m_heap_size;
    }
    sift_up(slot.heap);
    sift_down(slot.heap);
}

void QuicHost::unschedule(u32 index) noexcept
{
    Slot& slot = m_slots[index];
    const u32 place = slot.heap;
    if (place == kNotQueued)
    {
        return;
    }
    slot.heap = kNotQueued;
    --m_heap_size;
    if (place == m_heap_size)
    {
        return;
    }
    const u32 moved = m_heap[m_heap_size];
    put(place, moved);
    sift_up(place);
    sift_down(m_slots[moved].heap);
}

void QuicHost::sift_up(u32 place) noexcept
{
    const u32 index = m_heap[place];
    const u64 due = m_slots[index].due_ns;
    while (place != 0U)
    {
        const u32 parent = (place - 1U) / 2U;
        if (m_slots[m_heap[parent]].due_ns <= due)
        {
            break;
        }
        put(place, m_heap[parent]);
        place = parent;
    }
    put(place, index);
}

void QuicHost::sift_down(u32 place) noexcept
{
    const u32 index = m_heap[place];
    const u64 due = m_slots[index].due_ns;
    for (;;)
    {
        const u32 left = (2U * place) + 1U;
        if (left >= m_heap_size)
        {
            break;
        }
        const u32 right = left + 1U;
        const u32 child =
            (right < m_heap_size && m_slots[m_heap[right]].due_ns < m_slots[m_heap[left]].due_ns) ? right : left;
        if (due <= m_slots[m_heap[child]].due_ns)
        {
            break;
        }
        put(place, m_heap[child]);
        place = child;
    }
    put(place, index);
}

void QuicHost::put(u32 place, u32 index) noexcept
{
    m_heap[place] = index;
    m_slots[index].heap = place;
}

} // namespace crd::net
