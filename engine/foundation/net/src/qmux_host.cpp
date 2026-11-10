// crd-net -- the TCP and TLS path's host (4.2a; docs/design/network-foundation.md#stream-host).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/qmux_host.hpp>
#include <crd/net/websocket_tunnel.hpp>

#include <new>

namespace crd::net
{
namespace
{
constexpr u8 kReading = kStreamReadable;
constexpr u8 kReadingAndWriting = kStreamReadable | kStreamWritable;

bool config_valid(const QmuxHostConfig& config, const HandshakeBuckets* buckets) noexcept
{
    const bool server = config.connection.tls != nullptr;
    return config.connections != 0U && config.connections <= kQuicMaxHostConnections && config.ready_per_turn != 0U &&
           config.accepts_per_turn != 0U && config.accept_retry_ns != 0U && config.max_wait_ns != 0U &&
           config.write_bytes_per_turn >= kQmuxHostChunkBytes &&
           (!server || (buckets != nullptr && config.connection.tls->role() == crypto::TlsRole::Server));
}
} // namespace

QmuxHost::QmuxHost(memory::IAllocator& allocator, StreamPoller& poller, const TransportClock& clock,
                   HandshakeBuckets* buckets, QmuxHostEvents& events, const QmuxHostConfig& config) noexcept
    : m_allocator(allocator), m_poller(poller), m_clock(clock), m_buckets(buckets), m_events(events), m_config(config),
      m_heap(allocator)
{
    if (!config_valid(config, buckets))
    {
        return;
    }
    const u32 count = config.connections;
    Slot* const slots = memory::try_construct_array<Slot>(allocator, count);
    QmuxConnection* const cells = memory::try_allocate_array<QmuxConnection>(allocator, count);
    u32* const due = memory::try_construct_array<u32>(allocator, count);
    StreamReady* const ready = memory::try_construct_array<StreamReady>(allocator, config.ready_per_turn);
    u8* const chunk = memory::try_construct_array<u8>(allocator, kQmuxHostChunkBytes);
    if (slots == nullptr || cells == nullptr || due == nullptr || ready == nullptr || chunk == nullptr ||
        !m_heap.init(count))
    {
        memory::destroy_array(allocator, slots, count);
        if (cells != nullptr)
        {
            memory::deallocate_array(allocator, cells);
        }
        memory::destroy_array(allocator, due, count);
        memory::destroy_array(allocator, ready, config.ready_per_turn);
        memory::destroy_array(allocator, chunk, kQmuxHostChunkBytes);
        return;
    }
    for (u32 index = 0U; index < count; ++index)
    {
        slots[index].next_free = (index + 1U < count) ? index + 1U : kNoConnection;
    }
    m_slots = slots;
    m_cells = cells;
    m_due = due;
    m_ready = ready;
    m_chunk = chunk;
    m_free = 0U;
}

QmuxHost::~QmuxHost()
{
    if (m_slots == nullptr)
    {
        return;
    }
    const u32 count = m_config.connections;
    for (u32 index = 0U; index < count; ++index)
    {
        if (m_slots[index].connection != nullptr && m_slots[index].bearer == kNoConnection)
        {
            m_slots[index].connection->~QmuxConnection(); // a bearer's tunnels go with it
            m_poller.close(index, true);
        }
    }
    memory::destroy_array(m_allocator, m_slots, count);
    memory::deallocate_array(m_allocator, m_cells);
    memory::destroy_array(m_allocator, m_due, count);
    memory::destroy_array(m_allocator, m_ready, m_config.ready_per_turn);
    memory::destroy_array(m_allocator, m_chunk, kQmuxHostChunkBytes);
}

// ---- A turn ----

u64 QmuxHost::poll(u64 wait_ns) noexcept
{
    if (m_slots == nullptr)
    {
        return kNeverNs;
    }
    const u64 before = m_clock.monotonic_ns();
    const u64 due = (m_heap.next_due() < m_accept_resume_ns) ? m_heap.next_due() : m_accept_resume_ns;
    const u64 until_due = (due > before) ? due - before : 0U;
    const u64 waited_ns = (until_due < wait_ns) ? until_due : wait_ns;
    const usize count = m_poller.wait(cont::Span<StreamReady>(m_ready, m_config.ready_per_turn), waited_ns);
    const u64 now_ns = m_clock.monotonic_ns();
    if (m_accept_resume_ns <= now_ns)
    {
        m_accept_resume_ns = kNeverNs;
        m_poller.interest(kStreamListener, kStreamReadable);
    }
    for (usize at = 0U; at < count; ++at)
    {
        on_ready(m_ready[at], now_ns);
    }
    const u32 served = m_heap.take_due(now_ns, cont::Span<u32>(m_due, m_config.connections));
    for (u32 at = 0U; at < served; ++at)
    {
        if (m_slots[m_due[at]].connection != nullptr)
        {
            serve(m_due[at], now_ns);
        }
    }
    return (m_heap.next_due() < m_accept_resume_ns) ? m_heap.next_due() : m_accept_resume_ns;
}

void QmuxHost::run(const std::atomic<bool>& stop) noexcept
{
    while (!stop.load(std::memory_order_acquire))
    {
        static_cast<void>(poll(m_config.max_wait_ns));
    }
}

void QmuxHost::on_ready(const StreamReady& ready, u64 now_ns) noexcept
{
    if (ready.token == kStreamListener)
    {
        accept_some(now_ns);
        return;
    }
    if (ready.token >= m_config.connections || m_slots[ready.token].connection == nullptr)
    {
        return;
    }
    Slot& slot = m_slots[ready.token];
    if (slot.connecting)
    {
        finish_connect(ready.token, now_ns);
        return;
    }
    if ((ready.ready & kStreamWritable) != 0U && slot.write_blocked)
    {
        slot.write_blocked = false;
        m_heap.schedule(ready.token, now_ns);
    }
    if ((ready.ready & kStreamReadable) != 0U)
    {
        read_some(ready.token, now_ns);
    }
}

// ---- Accepting and connecting ----

QmuxConnection* QmuxHost::open(u32 index, StreamSocket& socket, const crypto::TlsContext& tls, bool connecting,
                               u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    QmuxConfig config = m_config.connection;
    config.tls = &tls;
    QmuxConnection* const connection = ::new (static_cast<void*>(m_cells + index)) QmuxConnection();
    if (!connection->init(m_allocator, config) || !connection->start(now_ns))
    {
        connection->~QmuxConnection();
        return nullptr;
    }
    m_free = slot.next_free;
    slot.next_free = kNoConnection;
    slot.connection = connection;
    slot.socket = &socket;
    slot.transport = SocketError::None;
    slot.interests = connecting ? kStreamWritable : kReading;
    slot.connecting = connecting;
    slot.write_blocked = false;
    slot.send_shut = false;
    ++m_live;
    m_heap.schedule(index, now_ns);
    return connection;
}

void QmuxHost::accept_some(u64 now_ns) noexcept
{
    const crypto::TlsContext* const tls = m_config.connection.tls;
    if (tls == nullptr)
    {
        m_poller.interest(kStreamListener, 0U); // a host that only connects
        return;
    }
    for (u32 turn = 0U; turn < m_config.accepts_per_turn; ++turn)
    {
        const u32 index = (m_free != kNoConnection) ? m_free : m_config.connections;
        SocketError error = SocketError::None;
        StreamSocket* const socket = m_poller.accept(index, error);
        if (socket == nullptr)
        {
            if (error != SocketError::Busy)
            {
                ++m_counters.accept_pauses;
                m_accept_resume_ns = now_ns + m_config.accept_retry_ns;
                m_poller.interest(kStreamListener, 0U);
            }
            return;
        }
        const Endpoint peer = socket->peer_endpoint();
        if (index == m_config.connections)
        {
            ++m_counters.refused_full;
            m_poller.close(index, true);
            continue;
        }
        if (m_buckets->admit(peer.address, true, now_ns) != HandshakeVerdict::Admit)
        {
            ++m_counters.refused_rate;
            m_poller.close(index, true);
            continue;
        }
        QmuxConnection* const connection = open(index, *socket, *tls, false, now_ns);
        if (connection == nullptr)
        {
            ++m_counters.refused_start;
            m_poller.close(index, true);
            continue;
        }
        ++m_counters.accepted;
        m_events.on_accept(handle_of(index), *connection, peer);
    }
}

QuicHandle QmuxHost::connect(const QmuxConnect& request, u64 now_ns, SocketError& error) noexcept
{
    error = SocketError::Failed;
    if (m_slots == nullptr || request.tls == nullptr || request.server.port == 0U || m_free == kNoConnection)
    {
        ++m_counters.connect_refused;
        return QuicHandle{};
    }
    const u32 index = m_free;
    StreamSocket* const socket = m_poller.connect(index, request.server, error);
    if (socket == nullptr)
    {
        ++m_counters.connect_refused;
        return QuicHandle{};
    }
    if (open(index, *socket, *request.tls, true, now_ns) == nullptr)
    {
        m_poller.close(index, true);
        ++m_counters.connect_refused;
        error = SocketError::Failed;
        return QuicHandle{};
    }
    ++m_counters.connected;
    error = SocketError::None;
    return handle_of(index);
}

void QmuxHost::finish_connect(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    const SocketError error = m_poller.connected(index);
    if (error == SocketError::Busy)
    {
        return;
    }
    slot.connecting = false;
    if (error != SocketError::None)
    {
        transport_failed(slot, error, now_ns);
    }
    m_heap.schedule(index, now_ns); // the first write that finishes sets the interests a made connection keeps
}

// ---- Reading and writing ----

void QmuxHost::read_some(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    const StreamResult result = slot.socket->read(cont::Span<u8>(m_chunk, kQmuxHostChunkBytes));
    if (result.bytes != 0U)
    {
        m_counters.bytes_read += result.bytes;
        slot.connection->receive(cont::ConstSpan<u8>(m_chunk, result.bytes), now_ns);
    }
    if (result.ended)
    {
        slot.connection->transport_ended(true, now_ns);
    }
    else if (result.error != SocketError::None && result.error != SocketError::Busy)
    {
        transport_failed(slot, result.error, now_ns);
    }
    else if (result.bytes == 0U)
    {
        return; // a report with nothing behind it
    }
    m_heap.schedule(index, now_ns);
}

void QmuxHost::read_left(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    for (u32 read = 0U; read < kReadsAfterFailure && slot.connection->state() != QmuxState::Closed; ++read)
    {
        const StreamResult result = slot.socket->read(cont::Span<u8>(m_chunk, kQmuxHostChunkBytes));
        if (result.bytes == 0U)
        {
            return;
        }
        m_counters.bytes_read += result.bytes;
        slot.connection->receive(cont::ConstSpan<u8>(m_chunk, result.bytes), now_ns);
    }
}

void QmuxHost::transport_failed(Slot& slot, SocketError error, u64 now_ns) noexcept
{
    slot.transport = error;
    slot.connection->transport_ended(false, now_ns);
}

bool QmuxHost::write_some(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    if (slot.connecting || slot.write_blocked)
    {
        return false;
    }
    usize budget = m_config.write_bytes_per_turn;
    for (;;)
    {
        const cont::ConstSpan<u8> bytes = slot.connection->outgoing(now_ns);
        if (bytes.empty())
        {
            break;
        }
        const usize most = (budget < kQmuxHostChunkBytes) ? budget : kQmuxHostChunkBytes;
        const usize offered = (bytes.size() < most) ? bytes.size() : most;
        const StreamResult result = slot.socket->write(bytes.first(offered));
        if (result.bytes != 0U)
        {
            m_counters.bytes_written += result.bytes;
            static_cast<void>(slot.connection->take(cont::Span<u8>(m_chunk, result.bytes), now_ns));
        }
        if (result.error != SocketError::None && result.error != SocketError::Busy)
        {
            read_left(index, now_ns);
            transport_failed(slot, result.error, now_ns);
            return false;
        }
        if (result.error == SocketError::Busy || result.bytes < offered)
        {
            ++m_counters.write_blocks;
            slot.write_blocked = true;
            set_interest(index, kReadingAndWriting);
            return false;
        }
        budget -= result.bytes;
        if (budget == 0U)
        {
            return true;
        }
    }
    set_interest(index, kReading);
    if (slot.connection->send_finished() && !slot.send_shut)
    {
        slot.send_shut = true;
        static_cast<void>(slot.socket->shutdown_send()); // a failure here is the connection's end, read next
    }
    return false;
}

void QmuxHost::set_interest(u32 index, u8 interests) noexcept
{
    Slot& slot = m_slots[index];
    if (slot.interests != interests)
    {
        slot.interests = interests;
        m_poller.interest(index, interests);
    }
}

// ---- Serving ----

void QmuxHost::serve(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    QmuxConnection& connection = *slot.connection;
    if (connection.next_timer_ns() <= now_ns)
    {
        connection.on_timeout(now_ns);
    }
    serve_tunnels(index, now_ns); // a bearer that ended reports its tunnels before its own last service
    m_events.on_service(handle_of(index), connection, now_ns);
    const bool more = write_some(index, now_ns);
    if (connection.state() == QmuxState::Closed)
    {
        serve_tunnels(index, now_ns); // a bearer that ended as it wrote: its tunnels end with it, reported first
        reclaim(index, now_ns);
        return;
    }
    m_heap.schedule(index, more ? now_ns : connection.next_timer_ns());
}

void QmuxHost::reclaim(u32 index, u64 now_ns) noexcept
{
    Slot& slot = m_slots[index];
    QmuxConnection& connection = *slot.connection;
    const bool flushed = !slot.connecting && !slot.write_blocked && connection.outgoing(now_ns).empty();
    const bool reset = !flushed || slot.transport != SocketError::None ||
                       connection.close_info().reason == QmuxCloseReason::HandshakeTimeout;
    m_heap.unschedule(index);
    m_events.on_closed(handle_of(index), connection, slot.transport);
    connection.~QmuxConnection();
    m_poller.close(index, reset);
    slot.connection = nullptr;
    slot.socket = nullptr;
    ++slot.generation;
    slot.next_free = m_free;
    m_free = index;
    --m_live;
    ++m_counters.closed;
    if (reset)
    {
        ++m_counters.resets_sent;
    }
}

// ---- Tunnels ----

void QmuxHost::serve_tunnels(u32 index, u64 now_ns) noexcept
{
    QmuxConnection& bearer = *m_slots[index].connection;
    WsTunnelEvent event;
    while (bearer.poll_tunnel(event, now_ns))
    {
        const u32 tunnel = event.tag;
        if (tunnel == kNoConnection)
        {
            if (event.kind == WsTunnelEventKind::Opened)
            {
                adopt_tunnel(index, event, now_ns);
            }
            continue; // a tunnel refused for want of a slot ends unreported
        }
        m_slots[tunnel].connection = event.connection;
        const QuicHandle handle = handle_of(tunnel);
        // A tunnel that ended is served once more, Closed, by its Closed event; one refused never opened.
        const bool closed = event.kind == WsTunnelEventKind::Closed;
        if (closed || (event.kind != WsTunnelEventKind::Refused && event.connection->state() != QmuxState::Closed))
        {
            m_events.on_service(handle, *event.connection, now_ns);
        }
        if (closed || event.kind == WsTunnelEventKind::Refused)
        {
            m_events.on_closed(handle, *event.connection, SocketError::None);
            release_tunnel(tunnel);
        }
    }
}

void QmuxHost::adopt_tunnel(u32 bearer, const WsTunnelEvent& event, u64 now_ns) noexcept
{
    const u32 tunnel = claim_tunnel(bearer, event.tunnel);
    if (tunnel == kNoConnection)
    {
        ++m_counters.tunnels_refused;
        event.connection->refuse(now_ns);
        return;
    }
    m_slots[tunnel].connection = event.connection;
    const QuicHandle handle = handle_of(tunnel);
    m_events.on_accept(handle, *event.connection, m_slots[bearer].socket->peer_endpoint());
    if (event.connection->state() != QmuxState::Closed) // else its Closed event follows
    {
        m_events.on_service(handle, *event.connection, now_ns);
    }
}

u32 QmuxHost::claim_tunnel(u32 bearer, QuicHandle tunnel) noexcept
{
    const u32 index = m_free;
    if (index == kNoConnection || !m_slots[bearer].connection->tag_tunnel(tunnel, index))
    {
        return kNoConnection;
    }
    Slot& slot = m_slots[index];
    m_free = slot.next_free;
    slot.next_free = kNoConnection;
    slot.bearer = bearer;
    slot.tunnel = tunnel;
    slot.transport = SocketError::None;
    ++m_live;
    ++m_counters.tunnels;
    return index;
}

void QmuxHost::release_tunnel(u32 index) noexcept
{
    Slot& slot = m_slots[index];
    slot.connection = nullptr;
    slot.bearer = kNoConnection;
    slot.tunnel = QuicHandle{};
    ++slot.generation;
    slot.next_free = m_free;
    m_free = index;
    --m_live;
    ++m_counters.closed;
}

QuicHandle QmuxHost::open_tunnel(QuicHandle bearer, u64 now_ns, WsTunnelError& error) noexcept
{
    QmuxConnection* const connection = this->connection(bearer);
    if (connection == nullptr || m_slots[bearer.slot].bearer != kNoConnection)
    {
        error = WsTunnelError::Refused; // no such connection, or a tunnel, which carries none
        return QuicHandle{};
    }
    if (m_free == kNoConnection)
    {
        error = WsTunnelError::NoTunnel;
        return QuicHandle{};
    }
    QuicHandle tunnel;
    error = connection->open_tunnel(now_ns, tunnel);
    if (error != WsTunnelError::None)
    {
        return QuicHandle{};
    }
    const u32 index = claim_tunnel(bearer.slot, tunnel); // a slot is free and the handle fresh, so it cannot fail
    m_heap.schedule(bearer.slot, now_ns);                // its request goes out in the next turn
    return handle_of(index);
}

// ---- The application's side ----

QmuxConnection* QmuxHost::connection(QuicHandle handle) noexcept
{
    if (m_slots == nullptr || handle.slot >= m_config.connections ||
        m_slots[handle.slot].generation != handle.generation)
    {
        return nullptr;
    }
    return m_slots[handle.slot].connection;
}

void QmuxHost::wake(QuicHandle handle) noexcept
{
    if (connection(handle) != nullptr)
    {
        const u32 bearer = m_slots[handle.slot].bearer; // a tunnel is served with the connection it runs on
        m_heap.schedule((bearer != kNoConnection) ? bearer : handle.slot, 0U);
    }
}

QuicHandle QmuxHost::handle_of(u32 index) const noexcept
{
    return QuicHandle{index, m_slots[index].generation};
}

} // namespace crd::net
