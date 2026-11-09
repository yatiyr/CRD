#pragma once

// crd-net -- what the host's socket calls need on every platform, shared by datagram (udp.cpp) and stream
// (stream_socket.cpp) sockets (4.2a; docs/design/network-foundation.md#sockets). Private to crd-net's sources.

#include <crd/core/platform.hpp>
#include <crd/net/endpoint.hpp>
#include <crd/net/udp.hpp>

#include <climits>
#include <cstring>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace crd::net
{

#if CRD_OS_WINDOWS
using NativeSocket = SOCKET;
using NativeLength = int;
const NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
using NativeLength = socklen_t;
constexpr NativeSocket kInvalidSocket = -1;
#endif

[[nodiscard]] inline NativeSocket native(u64 handle) noexcept
{
    return static_cast<NativeSocket>(handle);
}

[[nodiscard]] inline int last_error() noexcept
{
#if CRD_OS_WINDOWS
    return WSAGetLastError();
#else
    return errno;
#endif
}

[[nodiscard]] inline bool interrupted(int code) noexcept
{
#if CRD_OS_WINDOWS
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

[[nodiscard]] inline bool would_block(int code) noexcept
{
#if CRD_OS_WINDOWS
    return code == WSAEWOULDBLOCK;
#else
    return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

// `sending`: a full device queue (ENOBUFS) is the same back-pressure as a full socket buffer.
[[nodiscard]] inline SocketError map_error(int code, bool sending) noexcept
{
    if (would_block(code))
    {
        return SocketError::Busy;
    }
#if CRD_OS_WINDOWS
    switch (code)
    {
    case WSAEAFNOSUPPORT:
    case WSAEPFNOSUPPORT:
    case WSAEPROTONOSUPPORT:
        return SocketError::FamilyUnsupported;
    case WSAEADDRINUSE:
        return SocketError::AddressInUse;
    case WSAEADDRNOTAVAIL:
        return SocketError::AddressUnavailable;
    case WSAEACCES:
        return SocketError::Permission;
    case WSAENOBUFS:
        return sending ? SocketError::Busy : SocketError::Resources;
    case WSAEMFILE:
        return SocketError::Resources;
    case WSAEMSGSIZE:
        return SocketError::MessageTooLarge;
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
    case WSAENETDOWN:
        return SocketError::Unreachable;
    case WSAENOTSOCK:
        return SocketError::Closed;
    case WSAECONNREFUSED:
        return SocketError::Refused;
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAESHUTDOWN:
    case WSAENOTCONN:
    case WSAENETRESET:
        return SocketError::Reset;
    case WSAETIMEDOUT:
        return SocketError::TimedOut;
    default:
        return SocketError::Failed;
    }
#else
    switch (code)
    {
    case EAFNOSUPPORT:
    case EPROTONOSUPPORT:
        return SocketError::FamilyUnsupported;
    case EADDRINUSE:
        return SocketError::AddressInUse;
    case EADDRNOTAVAIL:
        return SocketError::AddressUnavailable;
    case EACCES:
    case EPERM:
        return SocketError::Permission;
    case ENOBUFS:
        return sending ? SocketError::Busy : SocketError::Resources;
    case EMFILE:
    case ENFILE:
    case ENOMEM:
        return SocketError::Resources;
    case EMSGSIZE:
        return SocketError::MessageTooLarge;
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ENETDOWN:
    case EHOSTDOWN:
        return SocketError::Unreachable;
    case EBADF:
    case ENOTSOCK:
        return SocketError::Closed;
    case ECONNREFUSED:
        return SocketError::Refused;
    case ECONNRESET:
    case ECONNABORTED:
    case EPIPE:
    case ENOTCONN:
    case ENETRESET:
        return SocketError::Reset;
    case ETIMEDOUT:
        return SocketError::TimedOut;
    default:
        return SocketError::Failed;
    }
#endif
}

struct NativeAddress
{
    sockaddr_storage storage{};
    NativeLength length = 0;
};

// The host's form of `peer` for a socket of the given family: a dual-stack socket reaches an IPv4 peer as its mapped
// address, and every other crossing of families is refused before the host sees it.
[[nodiscard]] inline SocketError to_native(const Endpoint& peer, bool v6_socket, bool dual_stack,
                                           NativeAddress& out) noexcept
{
    IpAddress address = peer.address.unmapped();
    out = NativeAddress{};
    if (!v6_socket)
    {
        if (address.family != AddressFamily::V4)
        {
            return SocketError::FamilyMismatch;
        }
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port = htons(peer.port);
        std::memcpy(&in.sin_addr, address.bytes, 4U);
        std::memcpy(&out.storage, &in, sizeof(in));
        out.length = static_cast<NativeLength>(sizeof(in));
        return SocketError::None;
    }
    if (address.family == AddressFamily::V4)
    {
        if (!dual_stack)
        {
            return SocketError::FamilyMismatch;
        }
        address = address.mapped();
    }
    sockaddr_in6 in6{};
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(peer.port);
    std::memcpy(&in6.sin6_addr, address.bytes, 16U);
    in6.sin6_scope_id = address.scope;
    std::memcpy(&out.storage, &in6, sizeof(in6));
    out.length = static_cast<NativeLength>(sizeof(in6));
    return SocketError::None;
}

// A peer as an endpoint, a mapped address as the IPv4 address it carries.
[[nodiscard]] inline Endpoint from_native(const sockaddr_storage& storage) noexcept
{
    Endpoint endpoint;
    if (storage.ss_family == AF_INET)
    {
        sockaddr_in in{};
        std::memcpy(&in, &storage, sizeof(in));
        std::memcpy(endpoint.address.bytes, &in.sin_addr, 4U);
        endpoint.port = ntohs(in.sin_port);
    }
    else if (storage.ss_family == AF_INET6)
    {
        sockaddr_in6 in6{};
        std::memcpy(&in6, &storage, sizeof(in6));
        u8 bytes[16];
        std::memcpy(bytes, &in6.sin6_addr, 16U);
        endpoint.address = IpAddress::v6(bytes, static_cast<u32>(in6.sin6_scope_id)).unmapped();
        endpoint.port = ntohs(in6.sin6_port);
    }
    return endpoint;
}

[[nodiscard]] inline bool set_option(NativeSocket handle, int level, int name, int value) noexcept
{
    return ::setsockopt(handle, level, name, reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

// The poll events a socket waits for: data (or a connection to accept) and room to write.
#if CRD_OS_WINDOWS
inline constexpr short kPollReadable = POLLRDNORM;
inline constexpr short kPollWritable = POLLWRNORM;
#else
inline constexpr short kPollReadable = POLLIN;
inline constexpr short kPollWritable = POLLOUT;
#endif

// `timeout_ns` rounded up to the milliseconds a host wait takes.
[[nodiscard]] inline int wait_milliseconds(u64 timeout_ns) noexcept
{
    const u64 whole_ms = timeout_ns / 1'000'000U + ((timeout_ns % 1'000'000U) != 0U ? 1U : 0U);
    return static_cast<int>(whole_ms < static_cast<u64>(INT_MAX) ? whole_ms : INT_MAX);
}

// Waits until `events` (or an error the next call reports) or `timeout_ns`; the events that arrived, 0 when the time
// passed. A signal that interrupts the wait restarts it.
[[nodiscard]] inline short wait_native(NativeSocket handle, short events, u64 timeout_ns) noexcept
{
    const int timeout_ms = wait_milliseconds(timeout_ns);
#if CRD_OS_WINDOWS
    WSAPOLLFD wanted{};
#else
    pollfd wanted{};
#endif
    wanted.fd = handle;
    wanted.events = events;
    for (;;)
    {
#if CRD_OS_WINDOWS
        const int ready = ::WSAPoll(&wanted, 1U, timeout_ms);
#else
        const int ready = ::poll(&wanted, 1U, timeout_ms);
#endif
        if (ready >= 0)
        {
            return (ready > 0) ? wanted.revents : short{0};
        }
        if (!interrupted(last_error()))
        {
            return events; // the next call reports what went wrong
        }
    }
}

} // namespace crd::net
