#pragma once

// crd-net -- byte-stream sockets (4.2a; contract: docs/design/network-foundation.md#stream-sockets). What the TCP and
// TLS path reads and writes, as DatagramSocket is for QUIC: every call is non-blocking but wait(), nothing allocates,
// and a test's in-process pipe or NET-SIM's virtual stream stands where a TcpSocket stands.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/endpoint.hpp>
#include <crd/net/udp.hpp>

namespace crd::net
{

// What one read or write did. `error` is None or names why it stopped: Busy when the host has nothing to read or no
// room to write, Reset for a connection the peer reset. `ended` is a read's clean end: the peer shut its sending side
// and every byte before it was read.
struct StreamResult
{
    usize bytes = 0U;
    SocketError error = SocketError::None;
    bool ended = false;
};

// wait()'s interests and answer.
inline constexpr u8 kStreamReadable = 1U << 0U;
inline constexpr u8 kStreamWritable = 1U << 1U;

class StreamSocket
{
public:
    virtual ~StreamSocket() = default;

    [[nodiscard]] virtual Endpoint local_endpoint() const noexcept = 0;
    [[nodiscard]] virtual Endpoint peer_endpoint() const noexcept = 0;
    // Moves what has arrived into `out`, without waiting.
    [[nodiscard]] virtual StreamResult read(cont::Span<u8> out) noexcept = 0;
    // Hands as much of `bytes` to the host as it takes now, without waiting.
    [[nodiscard]] virtual StreamResult write(cont::ConstSpan<u8> bytes) noexcept = 0;
    // Ends the sending side once everything written has left; reads go on.
    [[nodiscard]] virtual SocketError shutdown_send() noexcept = 0;
    // Waits until one of `interests` holds or `timeout_ns` passes, and returns those that hold (0 when the time
    // passed). An error or the peer's end counts as readable and writable: the next call reports it.
    [[nodiscard]] virtual u8 wait(u8 interests, u64 timeout_ns) noexcept = 0;

protected:
    StreamSocket() = default;
    StreamSocket(const StreamSocket&) = default;
    StreamSocket(StreamSocket&&) noexcept = default;
    StreamSocket& operator=(const StreamSocket&) = default;
    StreamSocket& operator=(StreamSocket&&) noexcept = default;
};

struct TcpOptions
{
    u32 receive_buffer_bytes = 0U; // 0 keeps the host's default
    u32 send_buffer_bytes = 0U;    // 0 keeps the host's default
};

// A connection of the host's TCP. Non-blocking and not inherited by child processes, TCP_NODELAY set (the transport
// assembles its own records and never waits for Nagle), and a write to a dead peer never raises SIGPIPE.
class TcpSocket final : public StreamSocket
{
public:
    TcpSocket() noexcept = default;
    ~TcpSocket() override;

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // Starts connecting to `peer` (its family picks the socket's); `error` is None once connected or while connecting,
    // and otherwise names why the socket is closed. connected() finishes the job.
    [[nodiscard]] static TcpSocket connect(const Endpoint& peer, const TcpOptions& options,
                                           SocketError& error) noexcept;

    // None once the connection stands, Busy while it is being made; any other error is the connection's end (the
    // socket stays open until close()).
    [[nodiscard]] SocketError connected() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return m_handle != kNoHandle; }
    // Closes it, sending what was written and then a FIN.
    void close() noexcept;
    // Closes it with a reset instead: no FIN and no TIME_WAIT, for a peer the transport gives up on.
    void abort() noexcept;

    [[nodiscard]] Endpoint local_endpoint() const noexcept override { return m_local; }
    [[nodiscard]] Endpoint peer_endpoint() const noexcept override { return m_peer; }
    [[nodiscard]] StreamResult read(cont::Span<u8> out) noexcept override;
    [[nodiscard]] StreamResult write(cont::ConstSpan<u8> bytes) noexcept override;
    [[nodiscard]] SocketError shutdown_send() noexcept override;
    [[nodiscard]] u8 wait(u8 interests, u64 timeout_ns) noexcept override;

private:
    friend class TcpListener;
    friend class TcpPoller;

    static constexpr u64 kNoHandle = ~u64{0};

    u64 m_handle = kNoHandle; // the host's descriptor or SOCKET
    Endpoint m_local;
    Endpoint m_peer;
    bool m_connecting = false;
};

struct TcpListenOptions
{
    Endpoint bind;          // port 0 takes an ephemeral port; the address picks the socket's family
    bool dual_stack = true; // an IPv6 socket also accepts IPv4, its peers reported as IPv4 endpoints
    u32 backlog = 1024U;    // connections the host queues before accept(), within the host's own limit
    TcpOptions connection;  // the accepted connections' options
};

// A listening socket. Its port is held exclusively on Windows; elsewhere SO_REUSEADDR lets a restarted server take its
// port back from connections in TIME_WAIT, while a live listener still refuses a second one.
class TcpListener
{
public:
    TcpListener() noexcept = default;
    ~TcpListener();

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // A listening socket, or a closed one and the reason.
    [[nodiscard]] static TcpListener open(const TcpListenOptions& options, SocketError& error) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return m_handle != kNoHandle; }
    void close() noexcept;

    [[nodiscard]] Endpoint local_endpoint() const noexcept { return m_local; }
    // The next waiting connection and None; a closed socket and Busy when none waits, or the host's reason. A
    // connection aborted before it was accepted is skipped.
    [[nodiscard]] TcpSocket accept(SocketError& error) noexcept;
    // True once a connection is waiting, false when `timeout_ns` passes first.
    [[nodiscard]] bool wait_readable(u64 timeout_ns) noexcept;

private:
    friend class TcpPoller;

    static constexpr u64 kNoHandle = ~u64{0};

    u64 m_handle = kNoHandle;
    Endpoint m_local;
    TcpOptions m_connection;
};

} // namespace crd::net
