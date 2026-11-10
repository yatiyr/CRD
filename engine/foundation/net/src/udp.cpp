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
// Room for the one control message an ECN codepoint takes, received or sent.
constexpr usize kControlBytes = CMSG_SPACE(sizeof(int));
#elif CRD_OS_WINDOWS
constexpr usize kControlBytes = WSA_CMSG_SPACE(sizeof(INT));
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
    slot.ecn = Ecn::NotEct;
    slot.peer = from_native(peer);
}

#if CRD_OS_LINUX || CRD_OS_WINDOWS
// Asks the host to report each datagram's ECN codepoint; an IPv6 socket that carries IPv4 asks for both families.
// A host that refuses leaves the socket without ECN, which features() reports; it is never an open() failure.
[[nodiscard]] SocketFeatures enable_ecn(NativeSocket handle, bool v6, bool dual_stack) noexcept
{
#if CRD_OS_LINUX
    const int v4_option = IP_RECVTOS;
    const int v6_option = IPV6_RECVTCLASS;
#else
    const int v4_option = IP_RECVECN;
    const int v6_option = IPV6_RECVECN;
#endif
    const bool v4_read = (v6 && !dual_stack) || set_option(handle, IPPROTO_IP, v4_option, 1);
    const bool v6_read = !v6 || set_option(handle, IPPROTO_IPV6, v6_option, 1);
    SocketFeatures features;
    features.ecn_reading = v4_read && v6_read;
    features.ecn_marking = true; // per datagram; a host that refuses a marked send turns it off (send)
    return features;
}

// Forbids fragmentation of every datagram sent, ignoring the host's path MTU estimate (IP_PMTUDISC_PROBE): the
// transport's PMTU search probes past it, and an ICMP report forged to lower it changes nothing. An IPv6 socket that
// carries IPv4 sets both families. Windows before IP_MTU_DISCOVER takes the plain DF options, which do the same
// there. A host that refuses leaves fragmentation on, which features() reports.
[[nodiscard]] bool forbid_fragmentation(NativeSocket handle, bool v6, bool dual_stack) noexcept
{
    const bool v4_family = !v6 || dual_stack;
#if CRD_OS_LINUX
    const bool v4 = !v4_family || set_option(handle, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_PROBE);
    const bool v6_set = !v6 || set_option(handle, IPPROTO_IPV6, IPV6_MTU_DISCOVER, IPV6_PMTUDISC_PROBE);
#else
    const bool v4 = !v4_family || set_option(handle, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_PROBE) ||
                    set_option(handle, IPPROTO_IP, IP_DONTFRAGMENT, 1);
    const bool v6_set = !v6 || set_option(handle, IPPROTO_IPV6, IPV6_MTU_DISCOVER, IP_PMTUDISC_PROBE) ||
                        set_option(handle, IPPROTO_IPV6, IPV6_DONTFRAG, 1);
#endif
    return v4 && v6_set;
}

// The control message level and type that carry a codepoint toward `peer`, by its own family (an IPv4 peer of a
// dual-stack socket takes IPv4's, which the host applies to the mapped address).
void ecn_message(const Endpoint& peer, int& level, int& type) noexcept
{
    const bool v4 = peer.address.unmapped().family == AddressFamily::V4;
    level = v4 ? IPPROTO_IP : IPPROTO_IPV6;
#if CRD_OS_LINUX
    type = v4 ? IP_TOS : IPV6_TCLASS;
#else
    type = IP_ECN; // IPV6_ECN is the same value: Winsock tells the families apart by the level alone
#endif
}

// The codepoint one received control message carries, or false for a message that carries none. Linux reports
// IPv4's TOS as one byte and IPv6's Traffic Class as an int; Windows reports both as an INT.
[[nodiscard]] bool ecn_from(int level, int type, const u8* data, usize bytes, Ecn& ecn) noexcept
{
#if CRD_OS_LINUX
    const bool carries = (level == IPPROTO_IP && type == IP_TOS) || (level == IPPROTO_IPV6 && type == IPV6_TCLASS);
#else
    const bool carries = (level == IPPROTO_IP && type == IP_ECN) || (level == IPPROTO_IPV6 && type == IPV6_ECN);
#endif
    if (!carries || bytes == 0U)
    {
        return false;
    }
    int value = 0;
    if (bytes >= sizeof(value))
    {
        std::memcpy(&value, data, sizeof(value));
    }
    else
    {
        value = data[0];
    }
    ecn = static_cast<Ecn>(static_cast<unsigned>(value) & 3U);
    return true;
}
#endif

#if CRD_OS_LINUX
[[nodiscard]] Ecn received_ecn(msghdr& message) noexcept
{
    Ecn ecn = Ecn::NotEct;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header))
    {
        const usize bytes = header->cmsg_len - CMSG_LEN(0);
        if (ecn_from(header->cmsg_level, header->cmsg_type, CMSG_DATA(header), bytes, ecn))
        {
            break;
        }
    }
    return ecn;
}

// One control message carrying `ecn` toward `peer` in `control`, which `message` then points at.
void mark(msghdr& message, u8 (&control)[kControlBytes], const Endpoint& peer, Ecn ecn) noexcept
{
    std::memset(control, 0, sizeof(control));
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* const header = CMSG_FIRSTHDR(&message);
    ecn_message(peer, header->cmsg_level, header->cmsg_type);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    const int value = static_cast<int>(ecn);
    std::memcpy(CMSG_DATA(header), &value, sizeof(value));
}
#elif CRD_OS_WINDOWS
[[nodiscard]] Ecn received_ecn(WSAMSG& message) noexcept
{
    Ecn ecn = Ecn::NotEct;
    for (WSACMSGHDR* header = WSA_CMSG_FIRSTHDR(&message); header != nullptr;
         header = WSA_CMSG_NXTHDR(&message, header))
    {
        const usize bytes = header->cmsg_len - WSA_CMSG_LEN(0);
        if (ecn_from(header->cmsg_level, header->cmsg_type, WSA_CMSG_DATA(header), bytes, ecn))
        {
            break;
        }
    }
    return ecn;
}

// One control message carrying `ecn` toward `peer` in `control`, which `message` then points at.
void mark(WSAMSG& message, u8 (&control)[kControlBytes], const Endpoint& peer, Ecn ecn) noexcept
{
    std::memset(control, 0, sizeof(control));
    message.Control = WSABUF{static_cast<ULONG>(sizeof(control)), reinterpret_cast<CHAR*>(control)};
    WSACMSGHDR* const header = WSA_CMSG_FIRSTHDR(&message);
    ecn_message(peer, header->cmsg_level, header->cmsg_type);
    header->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
    const INT value = static_cast<INT>(ecn);
    std::memcpy(WSA_CMSG_DATA(header), &value, sizeof(value));
}
#endif

} // namespace

UdpSocket::~UdpSocket()
{
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : DatagramSocket(), m_handle(other.m_handle), m_local(other.m_local), m_dual_stack(other.m_dual_stack),
      m_features(other.m_features)
#if CRD_OS_WINDOWS
      ,
      m_receive_message(other.m_receive_message)
#endif
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
        m_features = other.m_features;
#if CRD_OS_WINDOWS
        m_receive_message = other.m_receive_message;
#endif
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
#if CRD_OS_WINDOWS
    if (error == SocketError::None)
    {
        GUID guid = WSAID_WSARECVMSG;
        LPFN_WSARECVMSG receive_message = nullptr;
        DWORD returned = 0U;
        const bool found = ::WSAIoctl(handle, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &receive_message,
                                      sizeof(receive_message), &returned, nullptr, nullptr) == 0;
        error = (found && receive_message != nullptr) ? SocketError::None : SocketError::Failed;
        // A function pointer kept as the generic one and cast back to call: a defined round trip.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        socket.m_receive_message = reinterpret_cast<void (*)()>(receive_message);
    }
#endif
#if CRD_OS_LINUX || CRD_OS_WINDOWS
    if (error == SocketError::None)
    {
        socket.m_features = enable_ecn(handle, v6, socket.m_dual_stack);
        socket.m_features.dont_fragment = forbid_fragmentation(handle, v6, socket.m_dual_stack);
    }
#endif
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
    m_features = SocketFeatures{};
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
        alignas(cmsghdr) u8 controls[kBatchChunk][kControlBytes];
        for (usize index = 0U; index < chunk; ++index)
        {
            const ReceivedDatagram& slot = slots[result.count + index];
            vectors[index] = iovec{slot.storage.data(), slot.storage.size()};
            messages[index] = mmsghdr{};
            messages[index].msg_hdr.msg_name = &peers[index];
            messages[index].msg_hdr.msg_namelen = sizeof(peers[index]);
            messages[index].msg_hdr.msg_iov = &vectors[index];
            messages[index].msg_hdr.msg_iovlen = 1U;
            messages[index].msg_hdr.msg_control = controls[index];
            messages[index].msg_hdr.msg_controllen = sizeof(controls[index]);
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
            ReceivedDatagram& slot = slots[result.count + index];
            fill_slot(slot, messages[index].msg_len, (messages[index].msg_hdr.msg_flags & MSG_TRUNC) != 0,
                      peers[index]);
            slot.ecn = received_ecn(messages[index].msg_hdr);
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
        alignas(WSACMSGHDR) u8 control[kControlBytes];
        WSABUF buffer{static_cast<ULONG>(slot.storage.size() < INT_MAX ? slot.storage.size() : INT_MAX),
                      reinterpret_cast<CHAR*>(slot.storage.data())};
        WSAMSG message{};
        message.name = reinterpret_cast<sockaddr*>(&peer);
        message.namelen = static_cast<INT>(sizeof(peer));
        message.lpBuffers = &buffer;
        message.dwBufferCount = 1U;
        message.Control = WSABUF{static_cast<ULONG>(sizeof(control)), reinterpret_cast<CHAR*>(control)};
        DWORD got = 0U;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto receive_message = reinterpret_cast<LPFN_WSARECVMSG>(m_receive_message);
        const bool received = receive_message(handle, &message, &got, nullptr, nullptr) == 0;
        const int code = received ? 0 : last_error();
        if (received || code == WSAEMSGSIZE)
        {
            fill_slot(slot, received ? static_cast<usize>(got) : slot.storage.size(), !received, peer);
            slot.ecn = received_ecn(message);
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
        alignas(cmsghdr) u8 controls[kBatchChunk][kControlBytes];
        bool marked = false;
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
            if (datagram.ecn != Ecn::NotEct && m_features.ecn_marking)
            {
                mark(messages[chunk].msg_hdr, controls[chunk], datagram.peer, datagram.ecn);
                marked = true;
            }
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
                if (sent < 0 && code == EINVAL && marked)
                {
                    m_features.ecn_marking = false; // a host that takes no codepoint: the batch goes unmarked
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
#if CRD_OS_WINDOWS
    bool plain = false; // the datagram the host refused marked goes again unmarked
#endif
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
        // WSASendMsg only reads the bytes; WSABUF has no const form.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        CHAR* const bytes = reinterpret_cast<CHAR*>(const_cast<u8*>(datagram.bytes.data()));
        WSABUF buffer{static_cast<ULONG>(datagram.bytes.size()), bytes};
        WSAMSG message{};
        message.name = reinterpret_cast<sockaddr*>(&peer.storage);
        message.namelen = peer.length;
        message.lpBuffers = &buffer;
        message.dwBufferCount = 1U;
        alignas(WSACMSGHDR) u8 control[kControlBytes];
        const bool marked = !plain && datagram.ecn != Ecn::NotEct && m_features.ecn_marking;
        if (marked)
        {
            mark(message, control, datagram.peer, datagram.ecn);
        }
        DWORD bytes_sent = 0U;
        const int sent = ::WSASendMsg(handle, &message, 0U, &bytes_sent, nullptr, nullptr);
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
#if CRD_OS_WINDOWS
            if (code == WSAEINVAL && marked)
            {
                // Windows takes ECT(0) and ECT(1) but refuses CE, a router's mark, so that datagram goes unmarked;
                // a host that refuses an ECT mark takes no codepoint at all.
                if (datagram.ecn != Ecn::Ce)
                {
                    m_features.ecn_marking = false;
                }
                plain = true;
                continue;
            }
#endif
            result.error = map_error(code, true);
            return result;
        }
#if CRD_OS_WINDOWS
        plain = false;
#endif
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
