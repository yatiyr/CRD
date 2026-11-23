#pragma once

// What the QUIC server and client tests share (4.2a; docs/design/network-foundation.md#quic-server and #quic-client):
// the stream limits both ends run, the server's echoing application and a server behind its loop and ingress over a
// virtual socket.

#include "ingress_fixture.hpp"
#include "loop_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/net/quic_server.hpp>

#include <catch2/catch_test_macros.hpp>

namespace crd::net::testing
{
inline constexpr usize kStreams = 4U;
inline constexpr usize kMaxSlots = 32U;
inline constexpr u8 kHostPrf[crypto::kPrfKeyBytes] = {0x71, 0x72, 0x73};
inline constexpr u8 kResetSecret[crypto::kQuicResetKeyBytes] = {0x5E, 0x5F};
inline constexpr u8 kAppContext[] = {0x5C, 0x11, 0xE7, 0x02, 0x9A, 0x43, 0x0D, 0xB8};
inline constexpr u32 kBothVersions[] = {static_cast<u32>(crypto::QuicVersion::V1),
                                        static_cast<u32>(crypto::QuicVersion::V2)};
inline constexpr u32 kOnlyV2[] = {static_cast<u32>(crypto::QuicVersion::V2)};

inline QuicStreamLimits limits()
{
    QuicStreamLimits out;
    out.peer_bidi = kStreams;
    out.local_bidi = kStreams;
    out.receive_bytes = 16384U;
    out.send_bytes = 16384U;
    out.stream_window = 8192U;
    out.connection_window = 24576U;
    out.connection_max_window = 131072U;
    return out;
}

// The server's application: echoes every stream a client opens with another pattern as it reads, and counts what
// 0-RTT carried. State per slot, cleared when a connection is accepted there.
class EchoApp final : public QuicServerEvents
{
public:
    void on_accept(QuicHandle handle, QuicConnection&, const Endpoint&) noexcept override
    {
        ++accepted;
        m_state[handle.slot] = State{};
        last = handle;
    }

    void on_service(QuicHandle handle, QuicConnection& connection, u64) noexcept override
    {
        ++services;
        State& state = m_state[handle.slot];
        QuicStreamSet& set = connection.streams();
        QuicStreamEvent event;
        while (set.poll(event))
        {
        }
        for (usize index = 0U; index < kStreams; ++index)
        {
            const u64 stream = 4U * index;
            if (set.recv_state(stream) == QuicRecvState::None)
            {
                continue;
            }
            const u64 before = state.read[index];
            const bool ended = set.recv_state(stream) == QuicRecvState::DataRead ||
                               drain(set, stream, state.read[index], stream, &early_bytes);
            bytes_read += state.read[index] - before;
            read_handshaking = read_handshaking ||
                               (connection.state() == QuicConnectionState::Handshaking && state.read[index] > before);
            const QuicSendState echo = set.send_state(stream);
            if (echo == QuicSendState::Ready || echo == QuicSendState::Send)
            {
                static_cast<void>(
                    write_pattern(set, stream, state.echoed[index], state.read[index], stream + 1U, ended));
            }
        }
        state.buffers = set.buffer_allocations();
    }

    void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept override
    {
        ++closed;
        retired_buffers += connection.streams().buffer_allocations();
        m_state[handle.slot].buffers = 0U;
    }

    // The stream buffers the connections served have taken (QuicStreamSet::buffer_allocations): all serving
    // allocates.
    [[nodiscard]] usize buffer_allocations() const
    {
        usize total = retired_buffers;
        for (const State& state : m_state)
        {
            total += state.buffers;
        }
        return total;
    }

    usize accepted = 0U;
    usize services = 0U;
    usize closed = 0U;
    u64 early_bytes = 0U;
    u64 bytes_read = 0U;
    bool read_handshaking = false;
    QuicHandle last;
    usize retired_buffers = 0U; // those of the connections closed

private:
    struct State
    {
        u64 read[kStreams] = {};
        u64 echoed[kStreams] = {};
        usize buffers = 0U; // its connection's stream buffers when last served
    };
    State m_state[kMaxSlots];
};

struct Setup
{
    u32 connections = 24U;
    u32 early_datagrams = 16U;
    u32 early_per_client = 4U;
    u32 receive_bytes = 1500U; // the loop's slot_bytes, which the server advertises
    bool always_retry = false;
    bool only_v2 = false; // the server runs version 2 alone
    bool raw_only = false; // no TLS context: the raw UDP path alone
    u32 sockets = 1U;      // 2: the server also serves `second`, the next port of its address
    bool multipath = false; // offers multipath with path IDs up to 2 (RFC 10069)
    const u8* early_context = nullptr;
    usize early_context_bytes = 0U;
};

// A server behind its loop and ingress over a virtual socket at `local`, on the stack's test clock.
class ServerStack
{
public:
    ServerStack(const Identity& identity, const Setup& setup,
                const Endpoint& local = Endpoint{IpAddress::v4(10, 0, 0, 1), 443U})
        : routes(allocator, 256U, cont::ConstSpan<u8>(kHostPrf)),
          buckets(allocator, rules(setup), cont::ConstSpan<u8>(kHostPrf)),
          ingress(ingress_config(setup), routes, buckets, &tokens, &new_tokens, &reset),
          server(allocator, sockets_of(setup), clock, ingress, routes, reset, &tokens, app,
                 server_config(identity, setup)),
          loop(allocator, sockets_of(setup), clock, ingress, server, IoLoopConfig{.slot_bytes = setup.receive_bytes})
    {
        REQUIRE(tokens.init(cont::ConstSpan<u8>(kTokenKey), {}) == crypto::CryptoError::None);
        REQUIRE(new_tokens.init(1024U, crypto::kQuicNewTokenLifetime, kUnix) == crypto::CryptoError::None);
        REQUIRE(reset.init(cont::ConstSpan<u8>(kResetSecret)) == crypto::CryptoError::None);
        REQUIRE(server.ready());
        REQUIRE(loop.ready());
        socket.local = local;
        second.local = Endpoint{local.address, static_cast<u16>(local.port + 1U)};
        clock.now = kStartNs;
    }

    CountingAllocator allocator;
    crypto::ReplayFilter new_tokens; // NEW_TOKEN tokens are single use
    ConnectionIdTable routes;
    HandshakeBuckets buckets;
    crypto::QuicTokenKeys tokens;
    crypto::QuicResetKey reset;
    Ingress ingress;
    VirtualSocket socket;
    VirtualSocket second;
    TestClock clock;
    EchoApp app;
    QuicServer server;
    IoLoop loop;

private:
    SocketSet sockets_of(const Setup& setup)
    {
        DatagramSocket* const both[] = {&socket, &second};
        return SocketSet(cont::ConstSpan<DatagramSocket*>(both, (setup.sockets == 2U) ? 2U : 1U));
    }

    static HandshakeRules rules(const Setup& setup)
    {
        HandshakeRules out;
        out.always_retry = setup.always_retry;
        return out;
    }

    static IngressConfig ingress_config(const Setup& setup)
    {
        IngressConfig out;
        if (setup.only_v2)
        {
            out.versions[0] = kOnlyV2[0];
            out.version_count = 1U;
        }
        return out;
    }

    static QuicServerConfig server_config(const Identity& identity, const Setup& setup)
    {
        QuicServerConfig config;
        config.connection = config_for(crypto::QuicSide::Server, identity,
                                       setup.only_v2 ? crypto::QuicVersion::V2 : crypto::QuicVersion::V1, 1U);
        config.connection.streams = limits();
        config.connection.versions =
            setup.only_v2 ? cont::ConstSpan<u32>(kOnlyV2) : cont::ConstSpan<u32>(kBothVersions);
        if (setup.raw_only)
        {
            config.connection.tls = nullptr;
            config.connection.versions = {};
        }
        config.connection.early_context = cont::ConstSpan<u8>(setup.early_context, setup.early_context_bytes);
        config.connection.parameters.has_initial_max_path_id = setup.multipath;
        config.connection.parameters.initial_max_path_id = setup.multipath ? 2U : 0U;
        config.connections = setup.connections;
        config.early_datagrams = setup.early_datagrams;
        config.early_per_client = setup.early_per_client;
        config.receive_bytes = setup.receive_bytes;
        return config;
    }
};

} // namespace crd::net::testing
