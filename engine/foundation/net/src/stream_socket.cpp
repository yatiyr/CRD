// crd-net -- byte-stream sockets (4.2a; docs/design/network-foundation.md#stream-sockets).

#include "native_socket.hpp"

#include <crd/net/stream_socket.hpp>

#include <climits>

#if !CRD_OS_WINDOWS
#include <fcntl.h>
#include <netinet/tcp.h>
#endif

namespace crd::net
{
namespace
{

#if CRD_OS_LINUX
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0; // SO_NOSIGPIPE on Apple's hosts; Windows raises no signal
#endif

#if CRD_OS_WINDOWS
constexpr int kShutdownSend = SD_SEND;
#else
constexpr int kShutdownSend = SHUT_WR;
#endif

[[nodiscard]] NativeSocket open_stream(int family) noexcept
{
#if CRD_OS_WINDOWS
    return ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0U,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif CRD_OS_LINUX
    return ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    return ::socket(family, SOCK_STREAM, IPPROTO_TCP);
#endif
}

void close_native(u64 handle) noexcept
{
#if CRD_OS_WINDOWS
    ::closesocket(native(handle));
    ::WSACleanup();
#else
    ::close(native(handle));
#endif
}

// Each open socket holds one reference of Winsock, which close_native gives back.
[[nodiscard]] bool start_host() noexcept
{
#if CRD_OS_WINDOWS
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void stop_host() noexcept
{
#if CRD_OS_WINDOWS
    ::WSACleanup();
#endif
}

[[nodiscard]] SocketError make_nonblocking(NativeSocket handle) noexcept
{
#if CRD_OS_WINDOWS
    u_long non_blocking = 1U;
    if (::ioctlsocket(handle, FIONBIO, &non_blocking) != 0)
    {
        return map_error(last_error(), false);
    }
#elif !CRD_OS_LINUX
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0 || ::fcntl(handle, F_SETFD, FD_CLOEXEC) != 0 ||
        !set_option(handle, SOL_SOCKET, SO_NOSIGPIPE, 1))
    {
        return map_error(last_error(), false);
    }
#else
    static_cast<void>(handle);
#endif
    return SocketError::None;
}

[[nodiscard]] SocketError apply_options(NativeSocket handle, const TcpOptions& options) noexcept
{
    if ((options.receive_buffer_bytes != 0U &&
         !set_option(handle, SOL_SOCKET, SO_RCVBUF, static_cast<int>(options.receive_buffer_bytes))) ||
        (options.send_buffer_bytes != 0U &&
         !set_option(handle, SOL_SOCKET, SO_SNDBUF, static_cast<int>(options.send_buffer_bytes))))
    {
        return map_error(last_error(), false);
    }
    return SocketError::None;
}

// The transport assembles its own records and never waits for Nagle.
[[nodiscard]] SocketError disable_nagle(NativeSocket handle) noexcept
{
    return set_option(handle, IPPROTO_TCP, TCP_NODELAY, 1) ? SocketError::None : map_error(last_error(), false);
}

[[nodiscard]] Endpoint name_of(NativeSocket handle, bool peer) noexcept
{
    sockaddr_storage name{};
    NativeLength length = static_cast<NativeLength>(sizeof(name));
    const int result = peer ? ::getpeername(handle, reinterpret_cast<sockaddr*>(&name), &length)
                            : ::getsockname(handle, reinterpret_cast<sockaddr*>(&name), &length);
    return (result == 0) ? from_native(name) : Endpoint{};
}

#if CRD_OS_WINDOWS
// A host call takes an int on Windows.
[[nodiscard]] int call_size(usize bytes) noexcept
{
    return static_cast<int>(bytes < static_cast<usize>(INT_MAX) ? bytes : static_cast<usize>(INT_MAX));
}
#endif

} // namespace

// ---- TcpSocket ----

TcpSocket::~TcpSocket()
{
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : StreamSocket(), m_handle(other.m_handle), m_local(other.m_local), m_peer(other.m_peer),
      m_connecting(other.m_connecting)
{
    other.m_handle = kNoHandle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_handle = other.m_handle;
        m_local = other.m_local;
        m_peer = other.m_peer;
        m_connecting = other.m_connecting;
        other.m_handle = kNoHandle;
    }
    return *this;
}

TcpSocket TcpSocket::connect(const Endpoint& peer, const TcpOptions& options, SocketError& error) noexcept
{
    TcpSocket socket;
    if (!start_host())
    {
        error = SocketError::Failed;
        return socket;
    }
    const bool v6 = peer.address.unmapped().family == AddressFamily::V6;
    const NativeSocket handle = open_stream(v6 ? AF_INET6 : AF_INET);
    if (handle == kInvalidSocket)
    {
        error = map_error(last_error(), false);
        stop_host();
        return socket;
    }
    socket.m_handle = static_cast<u64>(handle);
    socket.m_peer = Endpoint{peer.address.unmapped(), peer.port};
    error = make_nonblocking(handle);
    if (error == SocketError::None)
    {
        error = apply_options(handle, options);
    }
    if (error == SocketError::None)
    {
        NativeAddress address;
        error = to_native(peer, v6, false, address);
        if (error == SocketError::None &&
            ::connect(handle, reinterpret_cast<const sockaddr*>(&address.storage), address.length) != 0)
        {
            const int code = last_error();
#if CRD_OS_WINDOWS
            socket.m_connecting = would_block(code);
#else
            socket.m_connecting = code == EINPROGRESS || code == EINTR;
#endif
            error = socket.m_connecting ? SocketError::None : map_error(code, false);
        }
    }
    if (error == SocketError::None && !socket.m_connecting)
    {
        error = disable_nagle(handle); // the host connected at once (a loopback peer can)
        socket.m_local = name_of(handle, false);
    }
    if (error != SocketError::None)
    {
        socket.close();
    }
    return socket;
}

SocketError TcpSocket::connected() noexcept
{
    if (!is_open())
    {
        return SocketError::Closed;
    }
    if (!m_connecting)
    {
        return SocketError::None;
    }
    if (wait_native(native(m_handle), kPollWritable, 0U) == 0)
    {
        return SocketError::Busy;
    }
    int code = 0;
    NativeLength length = static_cast<NativeLength>(sizeof(code));
    if (::getsockopt(native(m_handle), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) != 0)
    {
        return map_error(last_error(), false);
    }
    if (code != 0)
    {
        return map_error(code, false);
    }
    m_connecting = false;
    m_local = name_of(native(m_handle), false);
    return disable_nagle(native(m_handle));
}

void TcpSocket::close() noexcept
{
    if (is_open())
    {
        close_native(m_handle);
        m_handle = kNoHandle;
        m_local = Endpoint{};
        m_peer = Endpoint{};
        m_connecting = false;
    }
}

void TcpSocket::abort() noexcept
{
    if (is_open())
    {
        linger reset{};
        reset.l_onoff = 1;
        reset.l_linger = 0;
        // A socket that cannot set it closes gracefully, which still ends the connection.
        static_cast<void>(::setsockopt(native(m_handle), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&reset),
                                       sizeof(reset)));
        close();
    }
}

StreamResult TcpSocket::read(cont::Span<u8> out) noexcept
{
    StreamResult result;
    if (!is_open())
    {
        result.error = SocketError::Closed;
        return result;
    }
    if (out.empty())
    {
        return result;
    }
    for (;;)
    {
#if CRD_OS_WINDOWS
        const int got = ::recv(native(m_handle), reinterpret_cast<char*>(out.data()), call_size(out.size()), 0);
#else
        const ssize_t got = ::recv(native(m_handle), out.data(), out.size(), 0);
#endif
        if (got > 0)
        {
            result.bytes = static_cast<usize>(got);
            return result;
        }
        if (got == 0)
        {
            result.ended = true;
            return result;
        }
        const int code = last_error();
        if (!interrupted(code))
        {
            result.error = map_error(code, false);
            return result;
        }
    }
}

StreamResult TcpSocket::write(cont::ConstSpan<u8> bytes) noexcept
{
    StreamResult result;
    if (!is_open())
    {
        result.error = SocketError::Closed;
        return result;
    }
    if (bytes.empty())
    {
        return result;
    }
    for (;;)
    {
#if CRD_OS_WINDOWS
        const int sent =
            ::send(native(m_handle), reinterpret_cast<const char*>(bytes.data()), call_size(bytes.size()), kSendFlags);
#else
        const ssize_t sent = ::send(native(m_handle), bytes.data(), bytes.size(), kSendFlags);
#endif
        if (sent >= 0)
        {
            result.bytes = static_cast<usize>(sent);
            return result;
        }
        const int code = last_error();
        if (!interrupted(code))
        {
            result.error = map_error(code, true);
            return result;
        }
    }
}

SocketError TcpSocket::shutdown_send() noexcept
{
    if (!is_open())
    {
        return SocketError::Closed;
    }
    return (::shutdown(native(m_handle), kShutdownSend) == 0) ? SocketError::None : map_error(last_error(), false);
}

u8 TcpSocket::wait(u8 interests, u64 timeout_ns) noexcept
{
    if (!is_open())
    {
        return interests;
    }
    const short events = static_cast<short>((((interests & kStreamReadable) != 0U) ? kPollReadable : 0) |
                                            (((interests & kStreamWritable) != 0U) ? kPollWritable : 0));
    const short ready = wait_native(native(m_handle), events, timeout_ns);
    if ((ready & ~(kPollReadable | kPollWritable)) != 0)
    {
        return interests; // POLLHUP or POLLERR: the next call reports them
    }
    return static_cast<u8>((((ready & kPollReadable) != 0) ? kStreamReadable : 0U) |
                           (((ready & kPollWritable) != 0) ? kStreamWritable : 0U));
}

// ---- TcpListener ----

TcpListener::~TcpListener()
{
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : m_handle(other.m_handle), m_local(other.m_local), m_connection(other.m_connection)
{
    other.m_handle = kNoHandle;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_handle = other.m_handle;
        m_local = other.m_local;
        m_connection = other.m_connection;
        other.m_handle = kNoHandle;
    }
    return *this;
}

TcpListener TcpListener::open(const TcpListenOptions& options, SocketError& error) noexcept
{
    TcpListener listener;
    if (!start_host())
    {
        error = SocketError::Failed;
        return listener;
    }
    const bool v6 = options.bind.address.unmapped().family == AddressFamily::V6;
    const NativeSocket handle = open_stream(v6 ? AF_INET6 : AF_INET);
    if (handle == kInvalidSocket)
    {
        error = map_error(last_error(), false);
        stop_host();
        return listener;
    }
    listener.m_handle = static_cast<u64>(handle);
    listener.m_connection = options.connection;
    error = make_nonblocking(handle);
#if CRD_OS_WINDOWS
    const bool port_held = set_option(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    const bool port_held = set_option(handle, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    if (error == SocketError::None && !port_held)
    {
        error = map_error(last_error(), false);
    }
    if (error == SocketError::None && v6 && !set_option(handle, IPPROTO_IPV6, IPV6_V6ONLY, options.dual_stack ? 0 : 1))
    {
        error = map_error(last_error(), false);
    }
    if (error == SocketError::None)
    {
        error = apply_options(handle, options.connection); // accepted connections take the listener's buffer sizes
    }
    if (error == SocketError::None)
    {
        NativeAddress address;
        error = to_native(options.bind, v6, v6 && options.dual_stack, address);
        const int backlog = static_cast<int>(options.backlog < static_cast<u32>(INT_MAX) ? options.backlog : INT_MAX);
        if (error == SocketError::None &&
            (::bind(handle, reinterpret_cast<const sockaddr*>(&address.storage), address.length) != 0 ||
             ::listen(handle, backlog) != 0))
        {
            error = map_error(last_error(), false);
        }
    }
    if (error == SocketError::None)
    {
        listener.m_local = name_of(handle, false);
    }
    else
    {
        listener.close();
    }
    return listener;
}

void TcpListener::close() noexcept
{
    if (is_open())
    {
        close_native(m_handle);
        m_handle = kNoHandle;
        m_local = Endpoint{};
    }
}

TcpSocket TcpListener::accept(SocketError& error) noexcept
{
    TcpSocket socket;
    if (!is_open())
    {
        error = SocketError::Closed;
        return socket;
    }
    for (;;)
    {
#if CRD_OS_LINUX
        const NativeSocket handle = ::accept4(native(m_handle), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        const NativeSocket handle = ::accept(native(m_handle), nullptr, nullptr);
#endif
        if (handle != kInvalidSocket)
        {
#if CRD_OS_WINDOWS
            // Winsock counts each socket's reference; the accepted socket inherits the rest from the listener.
            if (!start_host())
            {
                ::closesocket(handle);
                error = SocketError::Failed;
                return socket;
            }
#endif
            socket.m_handle = static_cast<u64>(handle);
            error = make_nonblocking(handle);
            if (error == SocketError::None)
            {
                error = disable_nagle(handle);
            }
            if (error != SocketError::None)
            {
                socket.close();
                return socket;
            }
            socket.m_local = name_of(handle, false);
            socket.m_peer = name_of(handle, true);
            return socket;
        }
        const int code = last_error();
#if CRD_OS_WINDOWS
        const bool skipped = code == WSAECONNRESET;
#else
        const bool skipped = code == ECONNABORTED;
#endif
        if (!interrupted(code) && !skipped)
        {
            error = map_error(code, false);
            return socket;
        }
    }
}

bool TcpListener::wait_readable(u64 timeout_ns) noexcept
{
    return is_open() && wait_native(native(m_handle), kPollReadable, timeout_ns) != 0;
}

} // namespace crd::net
