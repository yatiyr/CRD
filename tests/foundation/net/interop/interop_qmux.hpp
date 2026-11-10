#pragma once

// crd-net interoperability tests (4.2a) -- the QMux connection crd-qmux-peer runs and reports, and the WebSocket
// tunnels (RFC 8441 in crd-qmux-peer, RFC 9220 in crd-quic-peer) that carry QMux over an HTTP connection, each running
// interop_echo.hpp's application.
//
//   options  --alpn (the QMux protocol, a tunnel's subprotocol), --idle-ms (default 30000) and --keepalive-ms (a
//            QX_PING after this long with no record)
//   tunnels  a client opens `connections` tunnels at once on its one HTTP connection; a server serves that many, in the
//            order they opened. Each tunnel's report goes out when it has ended, `connection=N` as on every other path.
//   refusal  a client's tunnel the server refused reports `close=upgrade-refused` with the status; a server never sees
//            one it refused (WebSocketTunnels answers it), so it reports nothing.

#include "interop_echo.hpp"

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/host_handle.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/udp.hpp>
#include <crd/net/websocket.hpp>
#include <crd/net/websocket_tunnel.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace crd_net_interop
{
inline constexpr char kQmuxCloseReason[] = "cerid-interop done";
inline constexpr StringView kQmuxHost = "cerid.test";
inline constexpr StringView kQmuxPath = "/interop";
inline constexpr u32 kQmuxStreamBytes = 65536U;
inline constexpr u64 kQmuxConnectionWindow = 1U << 22U;
inline constexpr u64 kQmuxConnectionMaxWindow = 1U << 24U;
inline constexpr u32 kQmuxDatagramRingBytes = 1U << 20U;
inline constexpr u32 kQmuxDatagramRingCount = 2048U;

struct QmuxOptions : EchoOptions
{
    StringView alpn;
    u64 idle_ms = 30000U;
    u64 keepalive_ms = 0U;
};

// One of the options above: false when `name` is none of them.
[[nodiscard]] inline bool parse_qmux_option(const char* name, const char* value, QmuxOptions& options) noexcept
{
    if (std::strcmp(name, "--alpn") == 0)
    {
        options.alpn = StringView(value, std::strlen(value));
        return true;
    }
    u64* const number = std::strcmp(name, "--idle-ms") == 0        ? &options.idle_ms
                        : std::strcmp(name, "--keepalive-ms") == 0 ? &options.keepalive_ms
                                                                   : nullptr;
    if (number != nullptr)
    {
        *number = std::strtoull(value, nullptr, 10);
    }
    return number != nullptr;
}

[[nodiscard]] inline ConstSpan<u8> bytes_of(StringView text) noexcept
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

// A QMux connection running the echo: its streams, datagrams and timers.
[[nodiscard]] inline crd::net::QmuxConfig qmux_config(const QmuxOptions& options) noexcept
{
    crd::net::QmuxConfig config;
    const u64 uni = options.uni + options.resets + options.stops;
    config.streams.peer_bidi = static_cast<u32>(options.max_streams);
    config.streams.local_bidi = static_cast<u32>(options.max_streams);
    config.streams.peer_uni = static_cast<u32>(uni != 0U ? uni : 1U);
    config.streams.local_uni = static_cast<u32>(uni != 0U ? uni : 1U);
    config.streams.receive_bytes = kQmuxStreamBytes;
    config.streams.send_bytes = kQmuxStreamBytes;
    config.streams.stream_window = kQmuxStreamBytes;
    config.streams.connection_window = kQmuxConnectionWindow;
    config.streams.connection_max_window = kQmuxConnectionMaxWindow;
    if (options.datagrams != 0U)
    {
        config.datagrams.max_frame_bytes = static_cast<u32>(options.datagram_limit);
        config.datagrams.receive_bytes = kQmuxDatagramRingBytes;
        config.datagrams.receive_count = kQmuxDatagramRingCount;
        config.datagrams.send_bytes = kQmuxDatagramRingBytes;
        config.datagrams.send_count = kQmuxDatagramRingCount;
    }
    config.idle_timeout_ns = options.idle_ms * 1000000U;
    config.keepalive_ns = options.keepalive_ms * 1000000U;
    return config;
}

// The WebSocket opening handshake's request; `protocols` outlive it.
[[nodiscard]] inline crd::net::WebSocketConfig websocket_config(ConstSpan<ConstSpan<u8>> protocols) noexcept
{
    crd::net::WebSocketConfig websocket;
    websocket.protocols = protocols;
    websocket.path = bytes_of(kQmuxPath);
    websocket.host = bytes_of(kQmuxHost);
    return websocket;
}

// `connections` tunnels, each a QMux connection of `options` over `websocket` (which outlives what this makes).
[[nodiscard]] inline crd::net::WsTunnelConfig tunnel_config(const QmuxOptions& options,
                                                            const crd::net::WebSocketConfig& websocket) noexcept
{
    crd::net::WsTunnelConfig tunnels;
    tunnels.connection = qmux_config(options);
    tunnels.connection.websocket = &websocket;
    tunnels.tunnels = static_cast<u32>(options.connections);
    return tunnels;
}

[[nodiscard]] inline const char* qmux_close_name(const crd::net::QmuxCloseInfo& info) noexcept
{
    using crd::net::QmuxCloseReason;
    switch (info.reason)
    {
        case QmuxCloseReason::None:
            return "none";
        case QmuxCloseReason::Local:
            return "local";
        case QmuxCloseReason::Error:
            return "transport";
        case QmuxCloseReason::Peer:
            return info.application ? "application" : "peer-transport";
        case QmuxCloseReason::PeerShutdown:
            return "shutdown";
        case QmuxCloseReason::IdleTimeout:
            return "idle";
        case QmuxCloseReason::HandshakeTimeout:
            return "handshake-timeout";
        case QmuxCloseReason::TlsFailed:
            return "tls";
        case QmuxCloseReason::Truncated:
            return "truncated";
        case QmuxCloseReason::Reset:
            return "reset";
        case QmuxCloseReason::UpgradeRefused:
            return "upgrade-refused";
        case QmuxCloseReason::WebSocketFailed:
            return "websocket-failed";
    }
    return "unknown";
}

// What a QMux connection reports after the echo's lines; `alpn` is what carries it (TLS's protocol, or the HTTP version
// of a tunnel).
inline void report_qmux(const crd::net::QmuxConnection& connection, StringView alpn,
                        crd::net::SocketError transport) noexcept
{
    const crd::net::QmuxCloseInfo& info = connection.close_info();
    const StringView tls_error = crd::crypto::tls_error_name(connection.tls_error());
    const ConstSpan<u8> reason = connection.close_reason();
    const ConstSpan<u8> subprotocol = connection.websocket_protocol();
    std::printf("websocket_protocol=%.*s\n", static_cast<int>(subprotocol.size()),
                reinterpret_cast<const char*>(subprotocol.data()));
    std::printf("alpn=%.*s\nclose=%s\nclose_code=%llu\nclose_reason=%.*s\ntls_error=%.*s\nsocket_error=%d\n",
                static_cast<int>(alpn.size()), alpn.data(), qmux_close_name(info),
                static_cast<unsigned long long>(info.error_code), static_cast<int>(reason.size()),
                reinterpret_cast<const char*>(reason.data()), static_cast<int>(tls_error.size()), tls_error.data(),
                static_cast<int>(transport));
    const crd::net::QmuxCounters& counters = connection.counters();
    std::printf("records_sent=%llu\nrecords_received=%llu\npings_sent=%llu\npongs_received=%llu\nrtt=%d\n",
                static_cast<unsigned long long>(counters.records_sent),
                static_cast<unsigned long long>(counters.records_received),
                static_cast<unsigned long long>(counters.pings_sent),
                static_cast<unsigned long long>(counters.pongs_received), connection.rtt_ns() != 0U ? 1 : 0);
    std::fflush(stdout);
}

// One HTTP connection's tunnels, either end's, each running the echo.
class TunnelPeer
{
public:
    // `options` and `version` (the HTTP version's name in the reports) outlive this.
    TunnelPeer(const EchoOptions& options, StringView version) noexcept : m_options(options), m_version(version) {}

    template <typename Http>
    [[nodiscard]] bool init(crd::memory::IAllocator& allocator, Http& http, const crd::net::WsTunnelConfig& config)
    {
        return m_tunnels.init(allocator, http, config);
    }

    // An event of the HTTP connection's poll(): true when it was a tunnel's.
    template <typename Event>
    [[nodiscard]] bool on_event(const Event& event, u64 now_ns) noexcept
    {
        return m_tunnels.on_event(event, now_ns);
    }

    // Opens a client's tunnels, reads what happened to every tunnel and serves each live one: on what arrived, and on a
    // timer for what waits on nothing the connection knows of (datagrams, a hold, resets).
    void service(u64 now_ns) noexcept
    {
        while (m_options.client && m_started < m_options.connections)
        {
            crd::net::QuicHandle handle;
            if (m_tunnels.open(now_ns, handle) != crd::net::WsTunnelError::None)
            {
                break;
            }
            start(handle);
        }
        crd::net::WsTunnelEvent event;
        while (m_tunnels.poll(event, now_ns))
        {
            handle(event, now_ns);
        }
        if (now_ns >= m_next_wake_ns)
        {
            m_next_wake_ns = now_ns + kWakeNs;
            for (u64 index = 0U; index < m_started; ++index)
            {
                crd::net::QmuxConnection* const connection =
                    m_open[index] ? m_tunnels.connection(m_handles[index]) : nullptr;
                if (connection != nullptr)
                {
                    serve(index, *connection, now_ns);
                }
            }
            m_tunnels.flush(now_ns);
        }
    }

    // Every tunnel this run expects has reported.
    [[nodiscard]] bool done() const noexcept { return m_reported >= m_options.connections; }
    [[nodiscard]] u32 live() const noexcept { return m_tunnels.live(); }
    [[nodiscard]] u64 deadline_ns() const noexcept
    {
        const u64 tunnels = m_tunnels.next_deadline_ns();
        return tunnels < m_next_wake_ns ? tunnels : m_next_wake_ns;
    }

private:
    void start(crd::net::QuicHandle handle) noexcept
    {
        m_echo[m_started].start(m_options, m_started + 1U, nullptr);
        m_handles[m_started] = handle;
        ++m_started;
    }

    [[nodiscard]] u64 index_of(crd::net::QuicHandle handle) const noexcept
    {
        for (u64 index = 0U; index < m_started; ++index)
        {
            if (m_handles[index] == handle)
            {
                return index;
            }
        }
        return kMaxConnections;
    }

    void handle(const crd::net::WsTunnelEvent& event, u64 now_ns) noexcept
    {
        using crd::net::WsTunnelEventKind;
        if (event.kind == WsTunnelEventKind::Opened && !m_options.client && m_started < m_options.connections)
        {
            start(event.tunnel);
        }
        const u64 index = index_of(event.tunnel);
        if (index == kMaxConnections)
        {
            return;
        }
        switch (event.kind)
        {
            case WsTunnelEventKind::Opened:
                m_open[index] = true;
                serve(index, *event.connection, now_ns);
                break;
            case WsTunnelEventKind::Service:
                serve(index, *event.connection, now_ns);
                break;
            case WsTunnelEventKind::Refused:
                std::printf("connection=%llu\nestablished=0\nverified=1\nclose=upgrade-refused\nclose_code=%u\n"
                            "refused_error=%llu\n",
                            static_cast<unsigned long long>(index + 1U), static_cast<unsigned>(event.status),
                            static_cast<unsigned long long>(event.error));
                std::fflush(stdout);
                finish(index);
                break;
            case WsTunnelEventKind::Closed:
                m_echo[index].report(false, crd::crypto::TlsEarlyData::None, event.connection->datagrams());
                report_qmux(*event.connection, m_version, crd::net::SocketError::None);
                finish(index);
                break;
        }
    }

    void serve(u64 index, crd::net::QmuxConnection& connection, u64 now_ns) noexcept
    {
        EchoTurn turn;
        turn.established = connection.peer_parameters_known();
        turn.can_open = connection.state() == crd::net::QmuxState::Established && turn.established;
        turn.now_ns = now_ns;
        if (m_echo[index].service(connection.streams(), connection.datagrams(), turn))
        {
            const ConstSpan<u8> reason(reinterpret_cast<const u8*>(kQmuxCloseReason), sizeof(kQmuxCloseReason) - 1U);
            connection.close(m_options.code, reason, now_ns);
        }
    }

    void finish(u64 index) noexcept
    {
        m_handles[index] = crd::net::QuicHandle{};
        m_open[index] = false;
        ++m_reported;
    }

    const EchoOptions& m_options;
    StringView m_version;
    crd::net::WebSocketTunnels m_tunnels;
    Echo m_echo[kMaxConnections];
    crd::net::QuicHandle m_handles[kMaxConnections];
    bool m_open[kMaxConnections] = {};
    u64 m_started = 0U;
    u64 m_reported = 0U;
    u64 m_next_wake_ns = 0U;
};
} // namespace crd_net_interop
