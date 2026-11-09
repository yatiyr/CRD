// crd-fuzz-net-qmux-host: the TCP and TLS path's host (4.2a; docs/design/network-foundation.md#stream-host) under the
// bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md). A server and a client host over pipes, under any
// order of connects, resets, cuts, stalls, raw bytes, clock steps, closes, wakes, stale handles and accept failures.
//
// Input: operations, each an operation byte and what it reads.
//
// Oracles. Every connection a host opened is reported closed exactly once, its handle goes stale at that report and a
// stale handle finds nothing. The live count is always opened minus closed. A turn allocates nothing once its
// connections are built. After the input, with the pipes unstalled and the clock past every timeout, no connection is
// left and every byte of the hosts' memory comes back.
#include "../stream_pipe_fixture.hpp"

#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/qmux_host.hpp>

#include <cstring>
#include <new>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using namespace crd::crypto;
using namespace crd::net;
using namespace crd::net::testing;

constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm};
constexpr StringView kProtocols[] = {"cerid-game/1"};
constexpr u64 kMsNs = 1000000U;
constexpr u32 kConnections = 6U;
constexpr usize kMostHandles = 64U;

struct Identity
{
    SigningKey key;
    TlsCredential credential;
    TlsContext client;
    TlsContext server;

    Identity()
    {
        u8 seed[32] = {4U, 2U};
        CRD_FUZZ_REQUIRE(key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed)) == CryptoError::None);
        CRD_FUZZ_REQUIRE(credential.init(key, "game.cerid.test") == TlsError::None);
        const KeyId pin = credential.key_id();
        TlsConfig config;
        config.role = TlsRole::Client;
        config.groups = kGroups;
        config.signatures = kSignatures;
        config.aeads = kAeads;
        config.pinned = ConstSpan<KeyId>(&pin, 1U);
        config.server_name = "game.cerid.test";
        config.application_protocols = kProtocols;
        CRD_FUZZ_REQUIRE(client.init(config) == TlsError::None);
        TlsConfig server_config;
        server_config.role = TlsRole::Server;
        server_config.groups = kGroups;
        server_config.aeads = kAeads;
        server_config.credential = &credential;
        server_config.server_name = "game.cerid.test";
        server_config.application_protocols = kProtocols;
        CRD_FUZZ_REQUIRE(server.init(server_config) == TlsError::None);
    }
};

Identity& identity()
{
    static Identity held;
    return held;
}

QmuxConfig connection_config()
{
    QmuxConfig config;
    config.streams.peer_bidi = 2U;
    config.streams.peer_uni = 1U;
    config.streams.local_bidi = 2U;
    config.streams.local_uni = 1U;
    config.streams.receive_bytes = 8192U;
    config.streams.send_bytes = 8192U;
    config.streams.stream_window = 4096U;
    config.streams.connection_window = 16384U;
    config.streams.connection_max_window = 65536U;
    config.handshake_timeout_ns = 2000U * kMsNs;
    config.idle_timeout_ns = 3000U * kMsNs;
    config.close_wait_ns = 500U * kMsNs;
    return config;
}

struct Input
{
    const u8* data;
    usize size;
    usize at = 0U;

    [[nodiscard]] bool more() const { return at < size; }
    u8 byte() { return (at < size) ? data[at++] : u8{0}; }
};

class Clock final : public TransportClock
{
public:
    [[nodiscard]] u64 monotonic_ns() const noexcept override { return now; }
    [[nodiscard]] u64 unix_seconds() const noexcept override { return now / 1000000000U; }

    u64 now = 1000000000U;
};

// Holds every report to the contract.
class Oracle final : public QmuxHostEvents
{
public:
    void on_accept(QuicHandle handle, QmuxConnection&, const Endpoint&) noexcept override { remember(handle); }

    void on_service(QuicHandle handle, QmuxConnection& connection, u64) noexcept override
    {
        CRD_FUZZ_REQUIRE(!stale(handle));
        CRD_FUZZ_REQUIRE(connection.state() != QmuxState::Closed);
        ++serviced;
    }

    void on_closed(QuicHandle handle, const QmuxConnection& connection, SocketError) noexcept override
    {
        CRD_FUZZ_REQUIRE(connection.state() == QmuxState::Closed);
        CRD_FUZZ_REQUIRE(!stale(handle)); // reported once
        m_stale[m_stale_count++ % kMostHandles] = handle;
        ++closed;
    }

    void remember(QuicHandle handle) noexcept
    {
        CRD_FUZZ_REQUIRE(!stale(handle));
        if (handle.valid())
        {
            m_handles[m_handle_count++ % kMostHandles] = handle;
        }
    }

    [[nodiscard]] bool stale(QuicHandle handle) const noexcept
    {
        for (usize at = 0U; at < kMostHandles; ++at)
        {
            if (m_stale[at] == handle && handle.valid())
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] QuicHandle pick(u8 choice) const noexcept { return m_handles[choice % kMostHandles]; }

    usize serviced = 0U;
    u64 closed = 0U;

private:
    QuicHandle m_handles[kMostHandles];
    QuicHandle m_stale[kMostHandles];
    usize m_handle_count = 0U;
    usize m_stale_count = 0U;
};

class Run
{
public:
    Run()
        : m_buckets(m_allocator, HandshakeRules{}), m_server_poller(m_network, server_address()),
          m_client_poller(m_network, Endpoint{IpAddress::v4_loopback(), 50000U})
    {
        CRD_FUZZ_REQUIRE(m_buckets.ready());
        m_network.limit = 4096U;
        m_network.listener = &m_server_poller;
    }

    void go(Input input)
    {
        QmuxHostConfig server;
        server.connection = connection_config();
        server.connection.tls = &identity().server;
        server.connections = kConnections;
        server.accepts_per_turn = 2U;
        QmuxHostConfig client;
        client.connection = connection_config();
        client.connections = kConnections;
        client.ready_per_turn = 4U;
        const crd::usize before = m_allocator.live_bytes();
        {
            alignas(QmuxHost) unsigned char server_cell[sizeof(QmuxHost)];
            alignas(QmuxHost) unsigned char client_cell[sizeof(QmuxHost)];
            QmuxHost* const server_host =
                new (server_cell) QmuxHost(m_allocator, m_server_poller, m_clock, &m_buckets, m_server_events, server);
            QmuxHost* const client_host =
                new (client_cell) QmuxHost(m_allocator, m_client_poller, m_clock, nullptr, m_client_events, client);
            CRD_FUZZ_REQUIRE(server_host->ready() && client_host->ready());
            while (input.more())
            {
                step(*server_host, *client_host, input);
                check(*server_host);
                check(*client_host);
            }
            settle(*server_host, *client_host);
            CRD_FUZZ_REQUIRE(server_host->live() == 0U);
            CRD_FUZZ_REQUIRE(client_host->live() == 0U);
            server_host->~QmuxHost();
            client_host->~QmuxHost();
        }
        CRD_FUZZ_REQUIRE(m_allocator.live_bytes() == before);
    }

private:
    [[nodiscard]] static Endpoint server_address() noexcept { return Endpoint{IpAddress::v4_loopback(), 4433U}; }

    void check(const QmuxHost& host) const
    {
        const QmuxHostCounters& counters = host.counters();
        CRD_FUZZ_REQUIRE(host.live() <= kConnections);
        CRD_FUZZ_REQUIRE(counters.accepted + counters.connected == u64{host.live()} + counters.closed);
    }

    void turn(QmuxHost& server, QmuxHost& client)
    {
        static_cast<void>(server.poll(0U));
        static_cast<void>(client.poll(0U));
    }

    void connect(QmuxHost& client)
    {
        SocketError error = SocketError::None;
        const QuicHandle handle = client.connect(QmuxConnect{server_address(), &identity().client}, m_clock.now, error);
        m_client_events.remember(handle);
    }

    // The links either host holds (its slot's), a link for an operation to act on.
    [[nodiscard]] PipeLink* link(PipePoller& poller, u8 choice) noexcept
    {
        return poller.link_of(choice % kConnections);
    }

    void step(QmuxHost& server, QmuxHost& client, Input& input)
    {
        switch (input.byte() % 14U)
        {
            case 0U:
                connect(client);
                break;
            case 1U:
                m_clock.now += (u64{input.byte()} + 1U) * kMsNs;
                break;
            case 2U:
                turn(server, client);
                break;
            case 3U:
                if (PipeLink* const pipe = link(m_client_poller, input.byte()))
                {
                    pipe->to_client.reset = true;
                }
                break;
            case 4U:
                if (PipeLink* const pipe = link(m_server_poller, input.byte()))
                {
                    pipe->to_client.ended = true;
                }
                break;
            case 5U:
                if (PipeLink* const pipe = link(m_client_poller, input.byte()))
                {
                    pipe->to_server.limit = usize{input.byte()} * 32U;
                    pipe->to_client.limit = usize{input.byte()} * 32U;
                }
                break;
            case 6U:
            {
                const QuicHandle handle = m_client_events.pick(input.byte());
                if (QmuxConnection* const connection = client.connection(handle))
                {
                    connection->close(input.byte(), ConstSpan<u8>(), m_clock.now);
                    client.wake(handle);
                }
                break;
            }
            case 7U:
            {
                const QuicHandle handle = m_server_events.pick(input.byte());
                if (QmuxConnection* const connection = server.connection(handle))
                {
                    connection->ping();
                    server.wake(handle);
                }
                break;
            }
            case 8U:
            {
                // Bytes from nowhere, in the middle of whatever the TLS sessions are doing.
                if (PipeLink* const pipe = link(m_server_poller, input.byte()))
                {
                    const usize wanted = input.byte();
                    const usize length = (wanted < input.size - input.at) ? wanted : input.size - input.at;
                    static_cast<void>(pipe->client.write(ConstSpan<u8>(input.data + input.at, length)));
                    input.at += length;
                }
                break;
            }
            case 9U:
                m_server_poller.accept_error = ((input.byte() & 1U) != 0U) ? SocketError::Resources : SocketError::None;
                break;
            case 10U:
            {
                const u8 mode = input.byte();
                m_network.connected_error = ((mode & 1U) != 0U) ? SocketError::Refused : SocketError::None;
                m_network.connect_error = ((mode & 2U) != 0U) ? SocketError::Unreachable : SocketError::None;
                m_network.connects_hang = (mode & 4U) != 0U;
                break;
            }
            case 11U:
                client.wake(QuicHandle{input.byte(), input.byte()}); // any handle, stale or not
                CRD_FUZZ_REQUIRE(client.connection(QuicHandle{}) == nullptr);
                break;
            case 12U:
                if (PipeLink* const pipe = link(m_client_poller, input.byte()))
                {
                    pipe->client.write_error = ((input.byte() & 1U) != 0U) ? SocketError::Reset : SocketError::None;
                }
                break;
            default:
                for (usize at = 0U; at < 8U; ++at)
                {
                    turn(server, client);
                }
                break;
        }
    }

    // Everything stuck is let go and the clock runs out every timeout.
    void settle(QmuxHost& server, QmuxHost& client)
    {
        m_server_poller.accept_error = SocketError::None;
        m_network.connects_hang = false;
        m_network.connect_error = SocketError::None;
        m_network.connected_error = SocketError::None;
        for (usize round = 0U; round < 64U && (server.live() != 0U || client.live() != 0U); ++round)
        {
            for (usize at = 0U; at < 32U; ++at)
            {
                turn(server, client);
            }
            m_clock.now += 4000U * kMsNs;
        }
    }

    crd::fuzz::BudgetAllocator m_allocator;
    PipeNetwork m_network;
    HandshakeBuckets m_buckets;
    PipePoller m_server_poller;
    PipePoller m_client_poller;
    Oracle m_server_events;
    Oracle m_client_events;
    Clock m_clock;
};
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A connection, its handshake's turns, a close.
    const u8 opened[] = {0x00U, 0x0DU, 0x0DU, 0x06U, 0x00U, 0x07U, 0x0DU};
    sink.add("opened", opened, sizeof(opened));
    // A connect the host cannot make, then a pause in accepting.
    const u8 refused[] = {0x0AU, 0x02U, 0x00U, 0x09U, 0x01U, 0x00U, 0x0DU};
    sink.add("refused", refused, sizeof(refused));
    // A stalled pipe, then a reset.
    const u8 stalled[] = {0x00U, 0x0DU, 0x05U, 0x00U, 0x00U, 0x00U, 0x0DU, 0x03U, 0x00U, 0x0DU};
    sink.add("stalled", stalled, sizeof(stalled));
    // Raw bytes on an accepted connection.
    const u8 raw[] = {0x00U, 0x02U, 0x08U, 0x00U, 0x04U, 0x16U, 0x03U, 0x03U, 0xFFU, 0x0DU};
    sink.add("raw", raw, sizeof(raw));
    // Time out a connection that hangs.
    const u8 hang[] = {0x0AU, 0x04U, 0x00U, 0x02U, 0x01U, 0xFFU, 0x02U};
    sink.add("hang", hang, sizeof(hang));
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    // Hosts, pollers and pipes are too large for a fuzzer's stack: one static run, rebuilt per input.
    alignas(Run) static unsigned char storage[sizeof(Run)];
    Run* const run = new (storage) Run();
    run->go(Input{data, size});
    run->~Run();
    return 0;
}
