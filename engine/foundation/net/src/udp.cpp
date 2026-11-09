// crd-net -- datagram sockets (4.2a; docs/design/network-foundation.md#sockets).

#include "native_socket.hpp"

#include <crd/net/udp.hpp>

#include <climits>
#include <cstring>

#if !CRD_OS_WINDOWS
#include <fcntl.h>
#endif

namespace crd::net
{
namespace
{

#if CRD_OS_LINUX
constexpr usize kBatchChunk = 32U; // datagrams per recvmmsg or sendmmsg
#endif

// An ICMP report about an earlier send, which says nothing about the datagrams still waiting.
[[nodiscard]] bool stale_report(int code) noexcept
{
#if CRD_OS_WINDOWS
    return code == WSAECONNRESET || code == WSAENETRESET;
#else
    return code == ECONNREFUSED;
#endif
}

[[nodiscard]] usize payload_limit(const Endpoint& peer) noexcept
{
    return peer.address.unmapped().family == AddressFamily::V4 ? kMaxUdpPayloadV4 : kMaxUdpPayloadV6;
}

// Everything before bind: non-blocking and not inherited, the family split, Windows' exclusive port and ICMP
// suppression, and the buffer sizes asked for.
[[nodiscard]] SocketError configure(NativeSocket handle, bool v6, const UdpOptions& options) noexcept
{
#if CRD_OS_WINDOWS
    u_long non_blocking = 1U;
    if (::ioctlsocket(handle, FIONBIO, &non_blocking) != 0 || !set_option(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1))
    {
        return map_error(last_error(), false);
    }
    const DWORD controls[] = {SIO_UDP_CONNRESET, SIO_UDP_NETRESET};
    for (const DWORD control : controls)
    {
        BOOL report = FALSE;
        DWORD returned = 0U;
        if (::WSAIoctl(handle, control, &report, sizeof(report), nullptr, 0U, &returned, nullptr, nullptr) != 0)
        {
            return map_error(last_error(), false);
        }
    }
#elif !CRD_OS_LINUX
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0 || ::fcntl(handle, F_SETFD, FD_CLOEXEC) != 0)
    {
        return map_error(last_error(), false);
    }
#endif
    if (v6 && !set_option(handle, IPPROTO_IPV6, IPV6_V6ONLY, options.dual_stack ? 0 : 1))
    {
        return map_error(last_error(), false);
    }
    if ((options.receive_buffer_bytes != 0U &&
         !set_option(handle, SOL_SOCKET, SO_RCVBUF, static_cast<int>(options.receive_buffer_bytes))) ||
        (options.send_buffer_bytes != 0U &&
         !set_option(handle, SOL_SOCKET, SO_SNDBUF, static_cast<int>(options.send_buffer_bytes))))
    {
        return map_error(last_error(), false);
    }
    return SocketError::None;
}

void fill_slot(ReceivedDatagram& slot, usize size, bool truncated, const sockaddr_storage& peer) noexcept
{
    slot.size = size < slot.storage.size() ? size : slot.storage.size();
    slot.truncated = truncated;
    slot.peer = from_native(peer);
}

} // namespace

UdpSocket::~UdpSocket()
{
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : DatagramSocket(), m_handle(other.m_handle), m_local(other.m_local), m_dual_stack(other.m_dual_stack)
{
    other.m_handle = kNoHandle;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_handle = other.m_handle;
        m_local = other.m_local;
        m_dual_stack = other.m_dual_stack;
        other.m_handle = kNoHandle;
    }
    return *this;
}

UdpSocket UdpSocket::open(const UdpOptions& options, SocketError& error) noexcept
{
    UdpSocket socket;
#if CRD_OS_WINDOWS
    // Each open socket holds one reference of Winsock, which close() gives back.
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        error = SocketError::Failed;
        return socket;
    }
#endif
    const bool v6 = options.bind.address.unmapped().family == AddressFamily::V6;
    const int family = v6 ? AF_INET6 : AF_INET;
#if CRD_OS_WINDOWS
    const NativeSocket handle =
        ::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0U, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif CRD_OS_LINUX
    const NativeSocket handle = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
#else
    const NativeSocket handle = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (handle == kInvalidSocket)
    {
        error = map_error(last_error(), false);
#if CRD_OS_WINDOWS
        ::WSACleanup();
#endif
        return socket;
    }
    socket.m_handle = static_cast<u64>(handle);
    socket.m_dual_stack = v6 && options.dual_stack;
    error = configure(handle, v6, options);
    if (error == SocketError::None)
    {
        NativeAddress address;
        error = to_native(options.bind, v6, socket.m_dual_stack, address);
        if (error == SocketError::None &&
            ::bind(handle, reinterpret_cast<const sockaddr*>(&address.storage), address.length) != 0)
        {
            error = map_error(last_error(), false);
        }
    }
    if (error == SocketError::None)
    {
        sockaddr_storage local{};
        NativeLength length = static_cast<NativeLength>(sizeof(local));
        if (::getsockname(handle, reinterpret_cast<sockaddr*>(&local), &length) != 0)
        {
            error = map_error(last_error(), false);
        }
        socket.m_local = from_native(local);
    }
    if (error != SocketError::None)
    {
        socket.close();
    }
    return socket;
}

bool UdpSocket::is_open() const noexcept
{
    return m_handle != kNoHandle;
}

void UdpSocket::close() noexcept
{
    if (!is_open())
    {
        return;
    }
#if CRD_OS_WINDOWS
    ::closesocket(native(m_handle));
    ::WSACleanup();
#else
    ::close(native(m_handle));
#endif
    m_handle = kNoHandle;
    m_local = Endpoint{};
}

Endpoint UdpSocket::local_endpoint() const noexcept
{
    return m_local;
}

BatchResult UdpSocket::receive(cont::Span<ReceivedDatagram> slots) noexcept
{
    BatchResult result;
    if (!is_open())
    {
        result.error = SocketError::Closed;
        return result;
    }
    const NativeSocket handle = native(m_handle);
#if CRD_OS_LINUX
    while (result.count < slots.size())
    {
        const usize chunk = (slots.size() - result.count < kBatchChunk) ? slots.size() - result.count : kBatchChunk;
        mmsghdr messages[kBatchChunk];
        iovec vectors[kBatchChunk];
        sockaddr_storage peers[kBatchChunk];
        for (usize index = 0U; index < chunk; ++index)
        {
            const ReceivedDatagram& slot = slots[result.count + index];
            vectors[index] = iovec{slot.storage.data(), slot.storage.size()};
            messages[index] = mmsghdr{};
            messages[index].msg_hdr.msg_name = &peers[index];
            messages[index].msg_hdr.msg_namelen = sizeof(peers[index]);
            messages[index].msg_hdr.msg_iov = &vectors[index];
            messages[index].msg_hdr.msg_iovlen = 1U;
        }
        const int got = ::recvmmsg(handle, messages, static_cast<unsigned int>(chunk), MSG_DONTWAIT, nullptr);
        if (got < 0)
        {
            const int code = last_error();
            if (interrupted(code) || stale_report(code))
            {
                continue;
            }
            if (!would_block(code))
            {
                result.error = map_error(code, false);
            }
            return result;
        }
        for (usize index = 0U; index < static_cast<usize>(got); ++index)
        {
            fill_slot(slots[result.count + index], messages[index].msg_len,
                      (messages[index].msg_hdr.msg_flags & MSG_TRUNC) != 0, peers[index]);
        }
        result.count += static_cast<usize>(got);
        if (static_cast<usize>(got) < chunk)
        {
            return result;
        }
    }
#else
    while (result.count < slots.size())
    {
        ReceivedDatagram& slot = slots[result.count];
        sockaddr_storage peer{};
#if CRD_OS_WINDOWS
        NativeLength peer_length = static_cast<NativeLength>(sizeof(peer));
        const int capacity = static_cast<int>(slot.storage.size() < INT_MAX ? slot.storage.size() : INT_MAX);
        const int got = ::recvfrom(handle, reinterpret_cast<char*>(slot.storage.data()), capacity, 0,
                                   reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (got >= 0)
        {
            fill_slot(slot, static_cast<usize>(got), false, peer);
            ++result.count;
            continue;
        }
        const int code = last_error();
        if (code == WSAEMSGSIZE)
        {
            fill_slot(slot, slot.storage.size(), true, peer);
            ++result.count;
            continue;
        }
#else
        iovec vector{slot.storage.data(), slot.storage.size()};
        msghdr message{};
        message.msg_name = &peer;
        message.msg_namelen = sizeof(peer);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const ssize_t got = ::recvmsg(handle, &message, 0);
        if (got >= 0)
        {
            fill_slot(slot, static_cast<usize>(got), (message.msg_flags & MSG_TRUNC) != 0, peer);
            ++result.count;
            continue;
        }
        const int code = last_error();
#endif
        if (interrupted(code) || stale_report(code))
        {
            continue;
        }
        if (!would_block(code))
        {
            result.error = map_error(code, false);
        }
        return result;
    }
#endif
    return result;
}

BatchResult UdpSocket::send(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept
{
    BatchResult result;
    if (!is_open())
    {
        result.error = SocketError::Closed;
        return result;
    }
    const NativeSocket handle = native(m_handle);
    const bool v6 = m_local.address.family == AddressFamily::V6;
#if CRD_OS_LINUX
    while (result.count < datagrams.size())
    {
        mmsghdr messages[kBatchChunk];
        iovec vectors[kBatchChunk];
        NativeAddress peers[kBatchChunk];
        usize chunk = 0U;
        SocketError refused = SocketError::None;
        while (chunk < kBatchChunk && result.count + chunk < datagrams.size())
        {
            const OutgoingDatagram& datagram = datagrams[result.count + chunk];
            refused = datagram.bytes.size() > payload_limit(datagram.peer)
                          ? SocketError::MessageTooLarge
                          : to_native(datagram.peer, v6, m_dual_stack, peers[chunk]);
            if (refused != SocketError::None)
            {
                break;
            }
            // sendmmsg only reads the bytes; iovec has no const form.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            vectors[chunk] = iovec{const_cast<u8*>(datagram.bytes.data()), datagram.bytes.size()};
            messages[chunk] = mmsghdr{};
            messages[chunk].msg_hdr.msg_name = &peers[chunk].storage;
            messages[chunk].msg_hdr.msg_namelen = peers[chunk].length;
            messages[chunk].msg_hdr.msg_iov = &vectors[chunk];
            messages[chunk].msg_hdr.msg_iovlen = 1U;
            ++chunk;
        }
        if (chunk != 0U)
        {
            const int sent =
                ::sendmmsg(handle, messages, static_cast<unsigned int>(chunk), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent <= 0)
            {
                const int code = last_error();
                if (sent < 0 && interrupted(code))
                {
                    continue;
                }
                result.error = sent < 0 ? map_error(code, true) : SocketError::Failed;
                return result;
            }
            result.count += static_cast<usize>(sent);
            if (static_cast<usize>(sent) < chunk)
            {
                continue; // the next call starts at the datagram that failed and reports why
            }
        }
        if (refused != SocketError::None)
        {
            result.error = refused;
            return result;
        }
    }
#else
    while (result.count < datagrams.size())
    {
        const OutgoingDatagram& datagram = datagrams[result.count];
        NativeAddress peer;
        const SocketError refused = datagram.bytes.size() > payload_limit(datagram.peer)
                                        ? SocketError::MessageTooLarge
                                        : to_native(datagram.peer, v6, m_dual_stack, peer);
        if (refused != SocketError::None)
        {
            result.error = refused;
            return result;
        }
#if CRD_OS_WINDOWS
        const int sent = ::sendto(handle, reinterpret_cast<const char*>(datagram.bytes.data()),
                                  static_cast<int>(datagram.bytes.size()), 0,
                                  reinterpret_cast<const sockaddr*>(&peer.storage), peer.length);
#else
        const ssize_t sent = ::sendto(handle, datagram.bytes.data(), datagram.bytes.size(), 0,
                                      reinterpret_cast<const sockaddr*>(&peer.storage), peer.length);
#endif
        if (sent < 0)
        {
            const int code = last_error();
            if (interrupted(code))
            {
                continue;
            }
            result.error = map_error(code, true);
            return result;
        }
        ++result.count;
    }
#endif
    return result;
}

bool UdpSocket::wait_readable(u64 timeout_ns) noexcept
{
    return is_open() && wait_native(native(m_handle), kPollReadable, timeout_ns) != 0;
}

} // namespace crd::net
