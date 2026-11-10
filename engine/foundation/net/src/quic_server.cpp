// crd-net -- a QUIC server: the connections behind an I/O loop's ingress (4.2a;
// docs/design/network-foundation.md#quic-server).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/quic_server.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
bool same_id(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

QuicHostLimits limits_of(const QuicServerConfig& config) noexcept
{
    return QuicHostLimits{config.connections, config.send_slots, config.connection.max_datagram_bytes,
                          config.send_retry_ns};
}
} // namespace

QuicServer::QuicServer(memory::IAllocator& allocator, DatagramSocket& socket, const TransportClock& clock,
                       Ingress& ingress, ConnectionIdTable& routes, const crypto::QuicResetKey& reset,
                       const crypto::QuicTokenKeys* tokens, QuicServerEvents& events,
                       const QuicServerConfig& config) noexcept
    : QuicHost(allocator, socket, routes, reset, events, limits_of(config)), m_clock(clock), m_ingress(ingress),
      m_routes(routes), m_tokens(tokens), m_events(events), m_config(config)
{
    const QuicConnectionConfig& connection = config.connection;
    const u32 count = config.connections;
    const u32 early = config.early_datagrams;
    if (!ingress.ready() || !ingress.server() || connection.side != crypto::QuicSide::Server ||
        early > kQuicMaxEarlyDatagrams ||
        (early != 0U && (config.early_per_client == 0U || config.early_bytes < kQuicMinInitialDatagram ||
                         config.early_bytes > kMaxUdpPayloadV6 || config.early_lifetime_ns == 0U)) ||
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
    if (!open_pool())
    {
        return;
    }
    Admitted* const admitted = memory::try_construct_array<Admitted>(allocator, count);
    Early* const held = (early != 0U) ? memory::try_construct_array<Early>(allocator, early) : nullptr;
    u8* const held_bytes =
        (early != 0U) ? memory::try_construct_array<u8>(allocator, usize{early} * config.early_bytes) : nullptr;
    if (admitted == nullptr || (early != 0U && (held == nullptr || held_bytes == nullptr)))
    {
        memory::destroy_array(allocator, admitted, count);
        memory::destroy_array(allocator, held, early);
        memory::destroy_array(allocator, held_bytes, usize{early} * config.early_bytes);
        return;
    }
    m_admitted = admitted;
    m_early = held;
    m_early_bytes = held_bytes;
}

QuicServer::~QuicServer()
{
    if (m_admitted == nullptr)
    {
        return;
    }
    const u32 count = m_config.connections;
    for (u32 index = 0U; index < count; ++index)
    {
        if (m_admitted[index].client_routed)
        {
            static_cast<void>(m_routes.erase(m_admitted[index].client_dcid.view()));
        }
    }
    const u32 early = m_config.early_datagrams;
    memory::destroy_array(allocator(), m_admitted, count);
    memory::destroy_array(allocator(), m_early, early);
    memory::destroy_array(allocator(), m_early_bytes, usize{early} * m_config.early_bytes);
}

QuicServerCounters QuicServer::counters() const noexcept
{
    QuicServerCounters out = m_counters;
    static_cast<QuicHostCounters&>(out) = host_counters();
    return out;
}

// ---- The loop's events ----

void QuicServer::on_admit(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept
{
    if (m_admitted == nullptr)
    {
        return;
    }
    const u32 index = free_slot();
    if (index == kNoConnection)
    {
        refuse(decision, datagram.peer, now_ns);
        return;
    }
    const QuicHeader& header = decision.header;
    QuicRoutedIds& routed_ids = ids(index);
    u8 local[crypto::kQuicMaxConnectionIdBytes] = {};
    u8 token[kQuicResetTokenBytes] = {};
    const cont::Span<u8> local_cid(local, m_ingress.config().local_cid_bytes);
    if (!routed_ids.issue(local_cid, cont::Span<u8>(token, sizeof(token))))
    {
        refuse(decision, datagram.peer, now_ns);
        return;
    }
    QuicConnectionConfig config = m_config.connection;
    config.version = static_cast<crypto::QuicVersion>(header.version); // one the ingress runs
    QuicConnection* const connection = build(index, config, datagram.peer);

    const bool retried = decision.claims.kind == crypto::QuicTokenKind::Retry;
    QuicAccept accept;
    accept.local_cid = local_cid;
    accept.client_dcid = header.dcid;
    accept.client_scid = header.scid;
    accept.original_dcid = retried ? decision.claims.original_dcid_view() : header.dcid;
    accept.retried = retried;
    accept.validated = decision.validated;
    Admitted& admitted = m_admitted[index];
    const RouteError routed = (connection != nullptr) ? m_routes.insert(header.dcid, index) : RouteError::Full;
    if (connection == nullptr || !admitted.client_dcid.assign(header.dcid) || routed != RouteError::None ||
        !connection->accept(accept, now_ns))
    {
        if (routed == RouteError::None)
        {
            static_cast<void>(m_routes.erase(header.dcid));
        }
        if (connection != nullptr)
        {
            discard(*connection);
        }
        routed_ids.retire(local_cid);
        refuse(decision, datagram.peer, now_ns);
        return;
    }

    admitted.client_routed = true;
    admitted.token_given = false;
    enlist(index, *connection, datagram.peer, now_ns);
    ++m_counters.admitted;
    m_events.on_accept(handle_of(index), *connection, datagram.peer);
    connection->receive(datagram.storage.first(datagram.size), datagram.ecn, now_ns);
    read_early(index, *connection, admitted.client_dcid.view(), now_ns);
}

QuicHandle QuicServer::open_keyed(const Endpoint& client, QuicKeyedStart start, u64 now_ns) noexcept
{
    start.local_id_bytes = m_ingress.config().local_cid_bytes;
    const u32 index = free_slot();
    QuicHandle handle;
    if (m_admitted != nullptr)
    {
        handle = start_keyed(m_config.connection, client, start, now_ns);
    }
    else
    {
        crypto::secure_zero(start.secret);
    }
    if (!handle.valid())
    {
        ++m_counters.refused;
        return handle;
    }
    m_admitted[index] = Admitted{};
    m_admitted[index].token_given = true; // a keyed path proves its address by the session, and needs no token
    ++m_counters.admitted;
    return handle;
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
    entry.ecn = datagram.ecn;
    static_cast<void>(entry.dcid.assign(decision.header.dcid)); // a header's ID, at most 20 bytes
    entry.bytes = static_cast<u32>(datagram.size);
    std::memcpy(m_early_bytes + (usize{place} * m_config.early_bytes), datagram.storage.data(), datagram.size);
    ++m_early_count;
    ++m_counters.early_held;
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

void QuicServer::service(u32 index, QuicConnection& connection, u64 now_ns) noexcept
{
    m_events.on_service(handle_of(index), connection, now_ns);
    Admitted& admitted = m_admitted[index];
    if (admitted.token_given || m_tokens == nullptr || !m_config.new_tokens || !connection.peer_parameters_known() ||
        (connection.state() != QuicConnectionState::Handshaking &&
         connection.state() != QuicConnectionState::Established))
    {
        return;
    }
    admitted.token_given = true;
    u8 token[kQuicMaxCarriedTokenBytes] = {};
    usize written = 0U;
    if (m_tokens->mint_new_token(connection.version(), quic_token_peer(peer_of(index)), m_clock.unix_seconds(),
                                 cont::Span<u8>(token, sizeof(token)), written) == crypto::CryptoError::None &&
        connection.give_new_token(cont::ConstSpan<u8>(token, written)))
    {
        ++m_counters.new_tokens;
    }
}

void QuicServer::release(u32 index, const QuicConnection&) noexcept
{
    Admitted& admitted = m_admitted[index];
    if (admitted.client_routed)
    {
        static_cast<void>(m_routes.erase(admitted.client_dcid.view()));
        admitted.client_routed = false;
    }
}

void QuicServer::after_turn(u64 now_ns) noexcept
{
    if (m_early != nullptr)
    {
        expire_early(now_ns);
    }
}

void QuicServer::refuse(const IngressDecision& decision, const Endpoint& peer, u64 now_ns) noexcept
{
    ++m_counters.refused;
    const cont::Span<u8> out = room();
    if (!out.empty())
    {
        push(m_ingress.refuse(decision, now_ns, out), peer);
    }
}

void QuicServer::read_early(u32 index, QuicConnection& connection, cont::ConstSpan<u8> dcid, u64 now_ns) noexcept
{
    if (m_early == nullptr)
    {
        return;
    }
    expire_early(now_ns);
    const Endpoint& peer = peer_of(index);
    const u32 capacity = m_config.early_datagrams;
    for (u32 offset = 0U; offset < m_early_count; ++offset)
    {
        const u32 place = (m_early_head + offset) % capacity;
        Early& entry = m_early[place];
        if (entry.bytes == 0U || !(entry.peer == peer) || !same_id(entry.dcid.view(), dcid))
        {
            continue;
        }
        const cont::Span<u8> bytes(m_early_bytes + (usize{place} * m_config.early_bytes), entry.bytes);
        entry.bytes = 0U;
        ++m_counters.early_read;
        connection.receive(bytes, entry.ecn, now_ns);
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

} // namespace crd::net
