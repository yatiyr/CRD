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
bool limits_valid(const QuicHostLimits& limits) noexcept
{
    return limits.connections != 0U && limits.connections <= kQuicMaxHostConnections && limits.send_retry_ns != 0U &&
           limits.datagram_bytes >= kQuicMinInitialDatagram && limits.receive_bytes >= kQuicMinInitialDatagram &&
           limits.receive_bytes <= kMaxUdpPayloadV6;
}
} // namespace

QuicHost::QuicHost(memory::IAllocator& allocator, DatagramSocket& socket, ConnectionIdTable& routes,
                   const crypto::QuicResetKey& reset, QuicHostEvents& events, const QuicHostLimits& limits) noexcept
    : m_allocator(allocator), m_socket(socket), m_routes(routes), m_reset(reset), m_events(events), m_limits(limits),
      m_peer_tokens(allocator, limits_valid(limits) ? limits.connections * kQuicHostTokensPerConnection : 1U),
      m_queue(allocator, limits.send_slots, limits.datagram_bytes), m_heap(allocator)
{
}

bool QuicHost::open_pool() noexcept
{
    const u32 count = m_limits.connections;
    if (m_slots != nullptr || !limits_valid(m_limits) || !m_routes.ready() || !m_peer_tokens.ready() ||
        !m_queue.ready())
    {
        return false;
    }
    Slot* const slots = memory::try_construct_array<Slot>(m_allocator, count);
    QuicConnection* const storage = memory::try_allocate_array<QuicConnection>(m_allocator, usize{count} + 1U);
    QuicRoutedIds* const ids = memory::try_allocate_array<QuicRoutedIds>(m_allocator, count);
    u32* const due = memory::try_construct_array<u32>(m_allocator, count);
    if (slots == nullptr || storage == nullptr || ids == nullptr || due == nullptr || !m_heap.init(count))
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
        memory::destroy_array(m_allocator, due, count);
        return false;
    }
    for (u32 index = 0U; index < count; ++index)
    {
        ::new (static_cast<void*>(ids + index)) QuicRoutedIds(m_routes, m_reset, index, &m_peer_tokens);
        slots[index].cell = storage + index;
        slots[index].next_free = (index + 1U < count) ? index + 1U : kNoConnection;
    }
    m_slots = slots;
    m_storage = storage;
    m_spare = storage + count;
    m_ids = ids;
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
    memory::destroy_array(m_allocator, m_due, count);
}

// ---- Building connections ----

QuicConnection* QuicHost::build(u32 index, QuicConnectionConfig config, const Endpoint& peer) noexcept
{
    u8 seed[crypto::kPrfKeyBytes] = {};
    config.ids = &m_ids[index];
    if (config.parameters.max_udp_payload_size > m_limits.receive_bytes)
    {
        config.parameters.max_udp_payload_size = m_limits.receive_bytes; // what the loop reads whole
    }
    const SocketFeatures features = m_socket.features();
    if (!features.ecn_marking)
    {
        config.ecn = Ecn::NotEct; // marks the socket would drop would only fail the path's validation
    }
    // Probes the host may fragment prove nothing (RFC 8899 section 3); an IPv6 path carries its 1280-byte floor.
    config.path_mtu_discovery = config.path_mtu_discovery && features.dont_fragment;
    if (peer.address.unmapped().family == AddressFamily::V6 && config.base_datagram_bytes < kQuicIpv6BaseDatagram)
    {
        config.base_datagram_bytes =
            (config.max_datagram_bytes < kQuicIpv6BaseDatagram) ? config.max_datagram_bytes : kQuicIpv6BaseDatagram;
    }
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

QuicHandle QuicHost::start_keyed(QuicConnectionConfig config, const Endpoint& peer, const QuicKeyedStart& start,
                                 u64 now_ns) noexcept
{
    const u32 index = (m_slots != nullptr) ? m_free : kNoConnection;
    u8 local[crypto::kQuicMaxConnectionIdBytes] = {};
    const usize id_bytes = start.local_id_bytes;
    const cont::Span<u8> local_id(local, (id_bytes <= sizeof(local)) ? id_bytes : 0U);
    if (index == kNoConnection || local_id.empty() ||
        crypto::quic_keyed_connection_id(start.secret, config.side, local_id) != crypto::CryptoError::None ||
        m_routes.insert(local_id, index) != RouteError::None)
    {
        crypto::secure_zero(start.secret);
        return QuicHandle{};
    }
    config.tls = nullptr;
    config.tickets = nullptr;
    config.token = {};
    config.early_context = {};
    config.versions = {};
    config.after_version_negotiation = false;
    config.held_packets = 0U;
    QuicConnection* const connection = build(index, config, peer);
    if (connection == nullptr || !connection->open_keyed(start, now_ns))
    {
        crypto::secure_zero(start.secret);
        if (connection != nullptr)
        {
            discard(*connection);
        }
        m_ids[index].retire(local_id);
        return QuicHandle{};
    }
    enlist(index, *connection, peer, now_ns);
    return handle_of(index);
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
    Slot& slot = m_slots[decision.connection];
    const bool unbound = slot.peer.port == 0U;
    if (!unbound && !(datagram.peer == slot.peer))
    {
        ++m_counters.foreign_path;
        return;
    }
    const u64 opened = connection->counters().packets_opened;
    connection->receive(datagram.storage.first(datagram.size), datagram.ecn, now_ns);
    if (unbound && connection->counters().packets_opened != opened)
    {
        slot.peer = datagram.peer;
    }
    schedule(decision.connection, now_ns);
}

bool QuicHost::on_unrouted(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept
{
    if (m_slots == nullptr || decision.header.type != QuicPacketType::OneRtt || datagram.size < kQuicMinResetBytes)
    {
        return false;
    }
    const cont::ConstSpan<u8> whole = datagram.storage.first(datagram.size);
    const u32 index = m_peer_tokens.find(whole.last(kQuicResetTokenBytes));
    QuicConnection* const connection = live_connection(index);
    if (connection == nullptr || !(datagram.peer == m_slots[index].peer) || !connection->stateless_reset(whole, now_ns))
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
    const u32 due = m_heap.take_due(now_ns, cont::Span<u32>(m_due, m_limits.connections));
    for (u32 next = 0U; next < due; ++next)
    {
        if (m_slots[m_due[next]].connection != nullptr)
        {
            serve(m_due[next], now_ns);
        }
    }
    flush();
    after_turn(now_ns);
    u64 next = m_heap.next_due();
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
    // A slot with no peer yet has no one to send to: what a stranger's datagrams credited waits for the binding packet.
    const bool drained = slot.peer.port == 0U || drain(slot, now_ns);
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
        m_queue.push(written, slot.peer, slot.connection->marking());
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
    m_heap.unschedule(index);
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

void QuicHost::schedule(u32 index, u64 due_ns) noexcept
{
    m_heap.schedule(index, due_ns);
}

} // namespace crd::net
