// crd-net -- a QUIC client: its connections behind an I/O loop over one socket (4.2a;
// docs/design/network-foundation.md#quic-client).

#include <crd/crypto/provider.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/quic_client.hpp>

#include <algorithm>
#include <cstring>

namespace crd::net
{
namespace
{
QuicHostLimits limits_of(const QuicClientConfig& config) noexcept
{
    return QuicHostLimits{config.connections, config.send_slots, config.connection.max_datagram_bytes,
                          config.send_retry_ns};
}

// Whether the ingress runs the connection's version and every version of its preference.
bool versions_run(const Ingress& ingress, const QuicConnectionConfig& connection) noexcept
{
    return ingress.runs(static_cast<u32>(connection.version)) &&
           std::ranges::all_of(connection.versions, [&ingress](u32 version) { return ingress.runs(version); });
}

// The client's own store: none when its configuration names another.
TicketCacheConfig tickets_of(const QuicClientConfig& config) noexcept
{
    return (config.connection.tickets == nullptr) ? config.tickets : TicketCacheConfig{.entries = 0U};
}
} // namespace

QuicClient::QuicClient(memory::IAllocator& allocator, DatagramSocket& socket, const Ingress& ingress,
                       ConnectionIdTable& routes, const crypto::QuicResetKey& reset, QuicClientEvents& events,
                       const QuicClientConfig& config) noexcept
    : QuicHost(allocator, socket, routes, reset, events, limits_of(config)), m_ingress(ingress), m_events(events),
      m_config(config), m_tickets(allocator, tickets_of(config))
{
    const QuicConnectionConfig& connection = config.connection;
    if (!m_tickets.ready() || !ingress.ready() || ingress.server() || connection.side != crypto::QuicSide::Client ||
        config.tokens > kQuicMaxClientTokens || !versions_run(ingress, connection) ||
        (!connection.entropy_seed.empty() && connection.entropy_seed.size() != crypto::kPrfKeyBytes))
    {
        return;
    }
    {
        // One connection built from the configuration proves it before any server depends on it.
        QuicConnectionConfig probe = connection;
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
    const u32 count = config.connections;
    Kept* const kept = (config.tokens != 0U) ? memory::try_construct_array<Kept>(allocator, config.tokens) : nullptr;
    Started* const started = memory::try_construct_array<Started>(allocator, count);
    if (started == nullptr || (config.tokens != 0U && kept == nullptr))
    {
        memory::destroy_array(allocator, kept, config.tokens);
        memory::destroy_array(allocator, started, count);
        return;
    }
    m_kept = kept;
    m_started = started;
    if (connection.tickets == nullptr && config.tickets.entries != 0U)
    {
        m_config.connection.tickets = &m_tickets;
    }
}

QuicClient::~QuicClient()
{
    memory::destroy_array(allocator(), m_kept, m_config.tokens);
    memory::destroy_array(allocator(), m_started, m_config.connections);
}

QuicClientCounters QuicClient::counters() const noexcept
{
    QuicClientCounters out = m_counters;
    static_cast<QuicHostCounters&>(out) = host_counters();
    return out;
}

// ---- Connecting ----

QuicHandle QuicClient::connect(const QuicClientConnect& request, u64 now_ns) noexcept
{
    const u32 index = (m_started != nullptr) ? free_slot() : kNoConnection;
    if (index == kNoConnection || request.token.size() > kQuicMaxCarriedTokenBytes)
    {
        ++m_counters.refused;
        return QuicHandle{};
    }
    m_tickets.advance(now_ns);
    QuicConnectionConfig config = m_config.connection;
    if (request.tls != nullptr)
    {
        config.tls = request.tls;
    }
    const u32 kept =
        request.token.empty() ? find_token(request.server, static_cast<u32>(config.version)) : m_config.tokens;
    const cont::ConstSpan<u8> token =
        (kept != m_config.tokens) ? cont::ConstSpan<u8>(m_kept[kept].token, m_kept[kept].bytes) : request.token;
    QuicConnection* const connection = start(index, request.server, config, token, now_ns);
    if (connection == nullptr)
    {
        ++m_counters.refused;
        return QuicHandle{};
    }
    if (kept != m_config.tokens)
    {
        m_kept[kept] = Kept{}; // a token is carried once (section 8.1.3)
        ++m_counters.tokens_used;
    }
    m_started[index] = Started{config.tls, false, false};
    enlist(index, *connection, request.server, now_ns);
    ++m_counters.connected;
    return handle_of(index);
}

QuicHandle QuicClient::open_keyed(const Endpoint& server, QuicKeyedStart start, u64 now_ns) noexcept
{
    start.local_id_bytes = m_ingress.config().local_cid_bytes;
    const u32 index = free_slot();
    QuicHandle handle;
    if (m_started != nullptr && server.port != 0U)
    {
        handle = start_keyed(m_config.connection, server, start, now_ns);
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
    m_started[index] = Started{nullptr, false, true}; // no version to follow and no token to keep
    ++m_counters.connected;
    return handle;
}

QuicConnection* QuicClient::start(u32 index, const Endpoint& server, const QuicConnectionConfig& config,
                                  cont::ConstSpan<u8> token, u64 now_ns) noexcept
{
    if (server.port == 0U)
    {
        return nullptr;
    }
    QuicRoutedIds& routed = ids(index);
    u8 local[crypto::kQuicMaxConnectionIdBytes] = {};
    u8 reset_token[kQuicResetTokenBytes] = {};
    const cont::Span<u8> local_cid(local, m_ingress.config().local_cid_bytes);
    if (!routed.issue(local_cid, cont::Span<u8>(reset_token, sizeof(reset_token))))
    {
        return nullptr;
    }
    u8 dcid[kQuicClientInitialDcidBytes] = {};
    QuicConnectionConfig built = config;
    built.token = token;
    QuicConnection* const connection =
        (crypto::random_bytes(cont::Span<u8>(dcid, sizeof(dcid))) == crypto::CryptoError::None)
            ? build(index, built, server)
            : nullptr;
    if (connection == nullptr || !connection->connect(local_cid, cont::ConstSpan<u8>(dcid, sizeof(dcid)), now_ns))
    {
        if (connection != nullptr)
        {
            discard(*connection);
        }
        routed.retire(local_cid);
        return nullptr;
    }
    return connection;
}

// ---- Serving ----

void QuicClient::service(u32 index, QuicConnection& connection, u64 now_ns) noexcept
{
    m_events.on_service(handle_of(index), connection, now_ns);
    Started& started = m_started[index];
    if (!started.token_kept && !connection.new_token().empty())
    {
        started.token_kept = true;
        keep_token(peer_of(index), connection);
    }
}

bool QuicClient::replace(u32 index, QuicConnection& connection, u64 now_ns) noexcept
{
    // RFC 9368 section 4: a connection in a version the client prefers among those the server offered, once.
    Started& started = m_started[index];
    const QuicCloseInfo& close = connection.close_info();
    if (!close.version_negotiation || close.retry_version == 0U || started.after_version_negotiation)
    {
        return false;
    }
    const Endpoint& server = peer_of(index);
    QuicConnectionConfig config = m_config.connection;
    config.version = static_cast<crypto::QuicVersion>(close.retry_version);
    config.after_version_negotiation = true;
    config.tls = started.tls;
    const u32 kept = find_token(server, close.retry_version);
    const cont::ConstSpan<u8> token =
        (kept != m_config.tokens) ? cont::ConstSpan<u8>(m_kept[kept].token, m_kept[kept].bytes) : cont::ConstSpan<u8>{};
    QuicConnection* const successor = start(index, server, config, token, now_ns);
    if (successor == nullptr)
    {
        return false;
    }
    if (kept != m_config.tokens)
    {
        m_kept[kept] = Kept{};
        ++m_counters.tokens_used;
    }
    succeed(index, *successor, now_ns);
    started.after_version_negotiation = true;
    started.token_kept = false;
    ++m_counters.restarts;
    m_events.on_restart(handle_of(index), *successor);
    return true;
}

void QuicClient::release(u32 index, const QuicConnection& connection) noexcept
{
    // The server may have sent a later token than the one first seen.
    keep_token(peer_of(index), connection);
}

void QuicClient::after_turn(u64 now_ns) noexcept
{
    m_tickets.advance(now_ns);
}

void QuicClient::on_admit(const IngressDecision&, const ReceivedDatagram&, u64) noexcept
{
    ++m_counters.unexpected;
}

void QuicClient::on_early(const IngressDecision&, const ReceivedDatagram&, u64) noexcept
{
    ++m_counters.unexpected;
}

// ---- Tokens ----

u32 QuicClient::find_token(const Endpoint& server, u32 version) const noexcept
{
    for (u32 entry = 0U; entry < m_config.tokens; ++entry)
    {
        const Kept& kept = m_kept[entry];
        if (kept.stamp != 0U && kept.version == version && kept.server == server)
        {
            return entry;
        }
    }
    return m_config.tokens;
}

cont::ConstSpan<u8> QuicClient::token_for(const Endpoint& server, crypto::QuicVersion version) const noexcept
{
    const u32 entry = (m_kept != nullptr) ? find_token(server, static_cast<u32>(version)) : m_config.tokens;
    return (entry != m_config.tokens) ? cont::ConstSpan<u8>(m_kept[entry].token, m_kept[entry].bytes)
                                      : cont::ConstSpan<u8>{};
}

void QuicClient::keep_token(const Endpoint& server, const QuicConnection& connection) noexcept
{
    const cont::ConstSpan<u8> token = connection.new_token();
    if (m_kept == nullptr || token.empty())
    {
        return;
    }
    const auto version = static_cast<u32>(connection.version());
    u32 entry = find_token(server, version);
    if (entry != m_config.tokens && m_kept[entry].bytes == token.size() &&
        std::memcmp(m_kept[entry].token, token.data(), token.size()) == 0)
    {
        return;
    }
    if (entry == m_config.tokens)
    {
        // A free entry, else the oldest.
        entry = 0U;
        for (u32 other = 0U; other < m_config.tokens && m_kept[entry].stamp != 0U; ++other)
        {
            if (m_kept[other].stamp < m_kept[entry].stamp)
            {
                entry = other;
            }
        }
    }
    Kept& kept = m_kept[entry];
    kept.server = server;
    kept.stamp = ++m_stamp;
    kept.version = version;
    kept.bytes = static_cast<u16>(token.size()); // at most kQuicMaxCarriedTokenBytes
    std::memcpy(kept.token, token.data(), token.size());
    ++m_counters.tokens_kept;
}

} // namespace crd::net
