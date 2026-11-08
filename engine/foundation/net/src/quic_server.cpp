// crd-net -- a QUIC server: the connections behind an I/O loop's ingress (4.2a;
// docs/design/network-foundation.md#quic-server).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/quic_server.hpp>

#include <cstring>
#include <new>

namespace crd::net
{
namespace
{
constexpr usize kPeerKeyBytes = 16U + 2U; // an IPv6 address and a port

// A peer's address and port as the key of the server's peer index.
usize peer_key(const Endpoint& peer, u8 (&key)[kPeerKeyBytes]) noexcept
{
    const cont::ConstSpan<u8> address = peer.address.view();
    std::memcpy(key, address.data(), address.size());
    key[address.size()] = static_cast<u8>(peer.port >> 8U);
    key[address.size() + 1U] = static_cast<u8>(peer.port & 0xFFU);
    return address.size() + 2U;
}

bool same_id(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
} // namespace

QuicServer::QuicServer(memory::IAllocator& allocator, DatagramSocket& socket, const TransportClock& clock,
                       Ingress& ingress, ConnectionIdTable& routes, const crypto::QuicResetKey& reset,
                       const crypto::QuicTokenKeys* tokens, QuicServerEvents& events,
                       const QuicServerConfig& config) noexcept
    : m_allocator(allocator), m_socket(socket), m_clock(clock), m_ingress(ingress), m_routes(routes), m_tokens(tokens),
      m_events(events), m_config(config),
      m_by_peer(allocator, (config.connections != 0U && config.connections <= kQuicMaxServerConnections)
                               ? config.connections
                               : 1U),
      m_queue(allocator, config.send_slots, config.connection.max_datagram_bytes)
{
    const QuicConnectionConfig& connection = config.connection;
    const u32 count = config.connections;
    const u32 early = config.early_datagrams;
    if (!ingress.ready() || !routes.ready() || !m_by_peer.ready() || !m_queue.ready() ||
        connection.side != crypto::QuicSide::Server || count == 0U || count > kQuicMaxServerConnections ||
        early > kQuicMaxEarlyDatagrams ||
        (early != 0U && (config.early_per_client == 0U || config.early_bytes < kQuicMinInitialDatagram ||
                         config.early_bytes > kMaxUdpPayloadV6 || config.early_lifetime_ns == 0U)) ||
        config.send_retry_ns == 0U || connection.max_datagram_bytes < kQuicMinInitialDatagram ||
        (!connection.entropy_seed.empty() && connection.entropy_seed.size() != crypto::kPrfKeyBytes))
    {
        return;
    }
    m_config.connection.parameters.disable_active_migration = true;
    {
        // One connection built from the configuration proves it before any client depends on it.
        QuicConnectionConfig probe = m_config.connection;
        probe.ids = nullptr;
        const QuicConnection proof(allocator, probe);
        if (!proof.ready())
        {
            return;
        }
    }

    Slot* const slots = memory::try_construct_array<Slot>(allocator, count);
    QuicConnection* const pool = memory::try_allocate_array<QuicConnection>(allocator, count);
    QuicRoutedIds* const ids = memory::try_allocate_array<QuicRoutedIds>(allocator, count);
    u32* const heap = memory::try_construct_array<u32>(allocator, count);
    u32* const due = memory::try_construct_array<u32>(allocator, count);
    Early* const held = (early != 0U) ? memory::try_construct_array<Early>(allocator, early) : nullptr;
    u8* const held_bytes =
        (early != 0U) ? memory::try_construct_array<u8>(allocator, usize{early} * config.early_bytes) : nullptr;
    if (slots == nullptr || pool == nullptr || ids == nullptr || heap == nullptr || due == nullptr ||
        (early != 0U && (held == nullptr || held_bytes == nullptr)))
    {
        memory::destroy_array(allocator, slots, count);
        if (pool != nullptr)
        {
            memory::deallocate_array(allocator, pool);
        }
        if (ids != nullptr)
        {
            memory::deallocate_array(allocator, ids);
        }
        memory::destroy_array(allocator, heap, count);
        memory::destroy_array(allocator, due, count);
        memory::destroy_array(allocator, held, early);
        memory::destroy_array(allocator, held_bytes, usize{early} * config.early_bytes);
        return;
    }
    for (u32 index = 0U; index < count; ++index)
    {
        ::new (static_cast<void*>(ids + index)) QuicRoutedIds(routes, reset, index);
        slots[index].next_free = (index + 1U < count) ? index + 1U : kNoConnection;
    }
    m_slots = slots;
    m_pool = pool;
    m_ids = ids;
    m_heap = heap;
    m_due = due;
    m_early = held;
    m_early_bytes = held_bytes;
    m_free = 0U;
}

QuicServer::~QuicServer()
{
    if (m_slots == nullptr)
    {
        return;
    }
    const u32 count = m_config.connections;
    for (u32 index = 0U; index < count; ++index)
    {
        Slot& slot = m_slots[index];
        if (slot.connection != nullptr)
        {
            if (slot.client_routed)
            {
                static_cast<void>(m_routes.erase(slot.client_dcid.view()));
            }
            slot.connection->~QuicConnection(); // its IDs leave the table
        }
        m_ids[index].~QuicRoutedIds();
    }
    const u32 early = m_config.early_datagrams;
    memory::destroy_array(m_allocator, m_slots, count);
    memory::deallocate_array(m_allocator, m_pool);
    memory::deallocate_array(m_allocator, m_ids);
    memory::destroy_array(m_allocator, m_heap, count);
    memory::destroy_array(m_allocator, m_due, count);
    memory::destroy_array(m_allocator, m_early, early);
    memory::destroy_array(m_allocator, m_early_bytes, usize{early} * m_config.early_bytes);
}

// ---- The loop's events ----

void QuicServer::on_admit(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept
{
    if (m_slots == nullptr)
    {
        return;
    }
    if (m_free == kNoConnection)
    {
        refuse(decision, datagram.peer, now_ns);
        return;
    }
    const QuicHeader& header = decision.header;
    const u32 index = m_free;
    Slot& slot = m_slots[index];
    QuicRoutedIds& ids = m_ids[index];
    u8 local[crypto::kQuicMaxConnectionIdBytes] = {};
    u8 token[kQuicResetTokenBytes] = {};
    const cont::Span<u8> local_cid(local, m_ingress.config().local_cid_bytes);
    if (!ids.issue(local_cid, cont::Span<u8>(token, sizeof(token))))
    {
        refuse(decision, datagram.peer, now_ns);
        return;
    }

    QuicConnectionConfig config = m_config.connection;
    config.version = static_cast<crypto::QuicVersion>(header.version); // one the ingress runs
    config.ids = &ids;
    if (!config.entropy_seed.empty())
    {
        std::memcpy(slot.seed, config.entropy_seed.data(), sizeof(slot.seed));
        for (usize byte = 0U; byte < 8U; ++byte)
        {
            slot.seed[byte] = static_cast<u8>(slot.seed[byte] ^ static_cast<u8>(m_admissions >> (8U * byte)));
        }
        config.entropy_seed = cont::ConstSpan<u8>(slot.seed, sizeof(slot.seed));
    }
    ++m_admissions;
    QuicConnection* const connection = ::new (static_cast<void*>(m_pool + index)) QuicConnection(m_allocator, config);

    const bool retried = decision.claims.kind == crypto::QuicTokenKind::Retry;
    QuicAccept accept;
    accept.local_cid = local_cid;
    accept.client_dcid = header.dcid;
    accept.client_scid = header.scid;
    accept.original_dcid = retried ? decision.claims.original_dcid_view() : header.dcid;
    accept.retried = retried;
    accept.validated = decision.validated;
    const RouteError routed = m_routes.insert(header.dcid, index);
    if (!slot.client_dcid.assign(header.dcid) || routed != RouteError::None || !connection->ready() ||
        !connection->accept(accept, now_ns))
    {
        if (routed == RouteError::None)
        {
            static_cast<void>(m_routes.erase(header.dcid));
        }
        connection->~QuicConnection();
        ids.retire(local_cid);
        refuse(decision, datagram.peer, now_ns);
        return;
    }

    m_free = slot.next_free;
    slot.next_free = kNoConnection;
    slot.connection = connection;
    slot.peer = datagram.peer;
    slot.client_routed = true;
    slot.token_given = false;
    u8 key[kPeerKeyBytes] = {};
    slot.peer_indexed = m_by_peer.insert(cont::ConstSpan<u8>(key, peer_key(slot.peer, key)), index) == RouteError::None;
    ++m_live;
    ++m_counters.admitted;
    m_events.on_accept(handle_of(index), *connection, slot.peer);
    connection->receive(datagram.storage.first(datagram.size), QuicEcn::NotEct, now_ns);
    read_early(index, slot.client_dcid.view(), now_ns);
    schedule(index, now_ns);
}

void QuicServer::on_route(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept
{
    Slot* const slot = live_slot(decision.connection);
    if (slot == nullptr)
    {
        return;
    }
    if (!(datagram.peer == slot->peer))
    {
        ++m_counters.foreign_path;
        return;
    }
    slot->connection->receive(datagram.storage.first(datagram.size), QuicEcn::NotEct, now_ns);
    schedule(decision.connection, now_ns);
}

void QuicServer::on_early(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept
{
    const u32 capacity = m_config.early_datagrams;
    if (m_early == nullptr || datagram.size > m_config.early_bytes)
    {
        ++m_counters.early_dropped;
        return;
    }
    expire_early(now_ns);
    u32 same = 0U;
    for (u32 offset = 0U; offset < m_early_count; ++offset)
    {
        const Early& entry = m_early[(m_early_head + offset) % capacity];
        same += (entry.bytes != 0U && entry.peer == datagram.peer && same_id(entry.dcid.view(), decision.header.dcid))
                    ? 1U
                    : 0U;
    }
    if (same >= m_config.early_per_client)
    {
        ++m_counters.early_dropped;
        return;
    }
    if (m_early_count == capacity)
    {
        // The oldest goes (expire_early left a held one at the head).
        m_early[m_early_head] = Early{};
        m_early_head = (m_early_head + 1U) % capacity;
        --m_early_count;
        ++m_counters.early_evicted;
        expire_early(now_ns);
    }
    const u32 place = (m_early_head + m_early_count) % capacity;
    Early& entry = m_early[place];
    entry.arrived_ns = now_ns;
    entry.peer = datagram.peer;
    static_cast<void>(entry.dcid.assign(decision.header.dcid)); // a header's ID, at most 20 bytes
    entry.bytes = static_cast<u32>(datagram.size);
    std::memcpy(m_early_bytes + (usize{place} * m_config.early_bytes), datagram.storage.data(), datagram.size);
    ++m_early_count;
    ++m_counters.early_held;
}

bool QuicServer::on_unrouted(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept
{
    if (m_slots == nullptr || decision.header.type != QuicPacketType::OneRtt)
    {
        return false;
    }
    u8 key[kPeerKeyBytes] = {};
    const u32 index = m_by_peer.find(cont::ConstSpan<u8>(key, peer_key(datagram.peer, key)));
    Slot* const slot = live_slot(index);
    if (slot == nullptr || !(slot->peer == datagram.peer) ||
        !slot->connection->stateless_reset(datagram.storage.first(datagram.size), now_ns))
    {
        return false;
    }
    ++m_counters.peer_resets;
    schedule(index, now_ns);
    return true;
}

u64 QuicServer::on_timers(u64 now_ns) noexcept
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
    if (m_early != nullptr)
    {
        expire_early(now_ns);
    }
    u64 next = (m_heap_size != 0U) ? m_slots[m_heap[0]].due_ns : kNeverNs;
    if (m_queue.queued() != 0U && now_ns + m_config.send_retry_ns < next)
    {
        next = now_ns + m_config.send_retry_ns;
    }
    return next;
}

// ---- The application's side ----

QuicConnection* QuicServer::connection(QuicServerHandle handle) noexcept
{
    Slot* const slot = live_slot(handle.slot);
    return (slot != nullptr && slot->generation == handle.generation) ? slot->connection : nullptr;
}

void QuicServer::wake(QuicServerHandle handle) noexcept
{
    if (connection(handle) != nullptr)
    {
        schedule(handle.slot, 0U);
    }
}

u32 QuicServer::early_held() const noexcept
{
    u32 held = 0U;
    for (u32 offset = 0U; offset < m_early_count; ++offset)
    {
        held += (m_early[(m_early_head + offset) % m_config.early_datagrams].bytes != 0U) ? 1U : 0U;
    }
    return held;
}

// ---- Serving ----

QuicServer::Slot* QuicServer::live_slot(u32 index) noexcept
{
    if (m_slots == nullptr || index >= m_config.connections || m_slots[index].connection == nullptr)
    {
        return nullptr;
    }
    return &m_slots[index];
}

QuicServerHandle QuicServer::handle_of(u32 index) const noexcept
{
    return QuicServerHandle{index, m_slots[index].generation};
}

void QuicServer::refuse(const IngressDecision& decision, const Endpoint& peer, u64 now_ns) noexcept
{
    ++m_counters.refused;
    cont::Span<u8> room = m_queue.room();
    if (room.empty())
    {
        flush();
        room = m_queue.room();
    }
    if (!room.empty())
    {
        m_queue.push(m_ingress.refuse(decision, now_ns, room), peer);
    }
}

void QuicServer::read_early(u32 index, cont::ConstSpan<u8> dcid, u64 now_ns) noexcept
{
    if (m_early == nullptr)
    {
        return;
    }
    expire_early(now_ns);
    Slot& slot = m_slots[index];
    const u32 capacity = m_config.early_datagrams;
    for (u32 offset = 0U; offset < m_early_count; ++offset)
    {
        const u32 place = (m_early_head + offset) % capacity;
        Early& entry = m_early[place];
        if (entry.bytes == 0U || !(entry.peer == slot.peer) || !same_id(entry.dcid.view(), dcid))
        {
            continue;
        }
        const cont::Span<u8> bytes(m_early_bytes + (usize{place} * m_config.early_bytes), entry.bytes);
        entry.bytes = 0U;
        ++m_counters.early_read;
        slot.connection->receive(bytes, QuicEcn::NotEct, now_ns);
    }
    expire_early(now_ns);
}

void QuicServer::expire_early(u64 now_ns) noexcept
{
    // Entries are in arrival order: once the head is held and young, every later one is too.
    while (m_early_count != 0U)
    {
        Early& head = m_early[m_early_head];
        if (head.bytes != 0U && now_ns - head.arrived_ns < m_config.early_lifetime_ns)
        {
            return;
        }
        if (head.bytes != 0U)
        {
            ++m_counters.early_expired;
        }
        head = Early{};
        m_early_head = (m_early_head + 1U) % m_config.early_datagrams;
        --m_early_count;
    }
}

void QuicServer::serve(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    QuicConnection& connection = *slot.connection;
    if (connection.next_timer_ns() <= now_ns)
    {
        connection.on_timeout(now_ns);
    }
    if (connection.state() != QuicConnectionState::Closed)
    {
        m_events.on_service(handle_of(index), connection, now_ns);
    }
    if (!slot.token_given && m_tokens != nullptr && m_config.new_tokens && connection.peer_parameters_known() &&
        (connection.state() == QuicConnectionState::Handshaking ||
         connection.state() == QuicConnectionState::Established))
    {
        slot.token_given = true;
        u8 token[kQuicMaxCarriedTokenBytes] = {};
        usize written = 0U;
        if (m_tokens->mint_new_token(connection.version(), quic_token_peer(slot.peer), m_clock.unix_seconds(),
                                     cont::Span<u8>(token, sizeof(token)), written) == crypto::CryptoError::None &&
            connection.give_new_token(cont::ConstSpan<u8>(token, written)))
        {
            ++m_counters.new_tokens;
        }
    }
    const bool drained = drain(slot, now_ns);
    if (connection.state() == QuicConnectionState::Closed)
    {
        reclaim(index);
        return;
    }
    u64 due = connection.next_timer_ns();
    if (!drained && now_ns + m_config.send_retry_ns < due)
    {
        due = now_ns + m_config.send_retry_ns;
    }
    schedule(index, due);
}

bool QuicServer::drain(Slot& slot, u64 now_ns) noexcept
{
    for (;;)
    {
        cont::Span<u8> room = m_queue.room();
        if (room.empty())
        {
            flush();
            room = m_queue.room();
            if (room.empty())
            {
                return false;
            }
        }
        const usize written = slot.connection->send(room, now_ns);
        if (written == 0U)
        {
            return true;
        }
        m_queue.push(written, slot.peer);
    }
}

void QuicServer::flush() noexcept
{
    const SendFlush flushed = m_queue.flush(m_socket);
    m_counters.datagrams_sent += flushed.sent;
    m_counters.send_dropped += flushed.dropped;
}

void QuicServer::reclaim(u32 index) noexcept
{
    Slot& slot = m_slots[index];
    m_events.on_closed(handle_of(index), *slot.connection);
    unschedule(index);
    if (slot.client_routed)
    {
        static_cast<void>(m_routes.erase(slot.client_dcid.view()));
        slot.client_routed = false;
    }
    if (slot.peer_indexed)
    {
        u8 key[kPeerKeyBytes] = {};
        const cont::ConstSpan<u8> peer(key, peer_key(slot.peer, key));
        if (m_by_peer.find(peer) == index)
        {
            static_cast<void>(m_by_peer.erase(peer));
        }
        slot.peer_indexed = false;
    }
    slot.connection->~QuicConnection(); // its IDs leave the table
    slot.connection = nullptr;
    ++slot.generation;
    slot.next_free = m_free;
    m_free = index;
    --m_live;
    ++m_counters.closed;
}

// ---- The heap ----

void QuicServer::schedule(u32 index, u64 due_ns) noexcept
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

void QuicServer::unschedule(u32 index) noexcept
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

void QuicServer::sift_up(u32 place) noexcept
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

void QuicServer::sift_down(u32 place) noexcept
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

void QuicServer::put(u32 place, u32 index) noexcept
{
    m_heap[place] = index;
    m_slots[index].heap = place;
}

} // namespace crd::net
