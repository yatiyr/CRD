// crd-net -- datagram sockets (4.2a; docs/design/network-foundation.md#sockets).

#include "native_socket.hpp"

#include <crd/net/udp.hpp>

#include <climits>
#include <cstring>

#if !CRD_OS_WINDOWS
#include <fcntl.h>
#endif
#if CRD_OS_LINUX
#include <linux/errqueue.h>
#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#endif

namespace crd::net
{
namespace
{

#if CRD_OS_LINUX
constexpr usize kBatchChunk = 32U; // datagrams per recvmmsg or sendmmsg
// Room for the one control message an ECN codepoint takes, received or sent.
constexpr usize kControlBytes = CMSG_SPACE(sizeof(int));
// Room for an ICMP report's control messages: the extended error with its sender's address, and the codepoint the
// host adds for an ECN-reading socket.
constexpr usize kReportControlBytes =
    CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_in6)) + 2U * CMSG_SPACE(sizeof(int));
// The reports one receive() reads before the datagrams behind them, so a flood of ICMP errors never starves them.
constexpr usize kReportsPerReceive = 2U * kBatchChunk;
// The IPv4 or IPv6 header and the UDP header: what a hop's MTU carries beyond the UDP payload.
constexpr u32 kV4Overhead = 28U;
constexpr u32 kV6Overhead = 48U;
#elif CRD_OS_WINDOWS
constexpr usize kControlBytes = WSA_CMSG_SPACE(sizeof(INT));
#endif

// An ICMP report about an earlier send, which says nothing about the datagrams still waiting. Linux hands the error
// of a report its error queue holds to the socket's next call: the codes its ICMP and ICMPv6 errors carry.
[[nodiscard]] bool stale_report(int code) noexcept
{
#if CRD_OS_WINDOWS
    return code == WSAECONNRESET || code == WSAENETRESET;
#elif CRD_OS_LINUX
    switch (code)
    {
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case EHOSTDOWN:
        case ENONET:
        case EMSGSIZE:
        case ENOPROTOOPT:
        case EOPNOTSUPP:
        case EACCES:
        case EPROTO:
            return true;
        default:
            return false;
    }
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
    slot.too_big = 0U;
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

#if CRD_OS_LINUX
// Has the host queue the ICMP errors about this socket's datagrams (IP_RECVERR, IPV6_RECVERR), which an unconnected
// socket otherwise never sees. An IPv6 socket that carries IPv4 asks for both families; the host reports IPv4's there
// through IPV6_RECVERR. A host that refuses leaves the socket without reports, which features() reports.
[[nodiscard]] bool queue_reports(NativeSocket handle, bool v6, bool dual_stack) noexcept
{
    const bool v4 = (v6 && !dual_stack) || set_option(handle, IPPROTO_IP, IP_RECVERR, 1);
    const bool v6_set = !v6 || set_option(handle, IPPROTO_IPV6, IPV6_RECVERR, 1);
    return v4 && v6_set;
}
#endif

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

// The UDP payload a Packet Too Big report says its hop carries (IPv4's Fragmentation Needed or ICMPv6's Packet Too
// Big), or 0 for any other report: another ICMP error, the host's own (a send over the interface's MTU, which that
// send already failed) or a hop MTU that leaves no payload.
[[nodiscard]] u32 too_big_limit(const sock_extended_err& error) noexcept
{
    u32 overhead = 0U;
    if (error.ee_origin == SO_EE_ORIGIN_ICMP && error.ee_type == ICMP_DEST_UNREACH && error.ee_code == ICMP_FRAG_NEEDED)
    {
        overhead = kV4Overhead;
    }
    else if (error.ee_origin == SO_EE_ORIGIN_ICMP6 && error.ee_type == ICMP6_PACKET_TOO_BIG)
    {
        overhead = kV6Overhead;
    }
    return (overhead != 0U && error.ee_info > overhead) ? error.ee_info - overhead : 0U;
}

enum class Report : u8
{
    None,    // the error queue is empty
    Dropped, // a report the transport has no use for
    TooBig,  // a Packet Too Big report, in the slot
};

// Reads the oldest report off the socket's error queue: the quoted start of the datagram into the slot's storage, and
// the peer it was sent to.
[[nodiscard]] Report read_report(NativeSocket handle, ReceivedDatagram& slot) noexcept
{
    sockaddr_storage peer{};
    iovec vector{slot.storage.data(), slot.storage.size()};
    alignas(cmsghdr) u8 control[kReportControlBytes];
    msghdr message{};
    message.msg_name = &peer;
    message.msg_namelen = sizeof(peer);
    message.msg_iov = &vector;
    message.msg_iovlen = 1U;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t got = -1;
    do
    {
        got = ::recvmsg(handle, &message, MSG_ERRQUEUE | MSG_DONTWAIT);
    } while (got < 0 && interrupted(last_error()));
    if (got < 0)
    {
        return Report::None;
    }
    u32 limit = 0U;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header))
    {
        const bool extended = (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_RECVERR) ||
                              (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_RECVERR);
        if (extended && header->cmsg_len >= CMSG_LEN(sizeof(sock_extended_err)))
        {
            sock_extended_err error{};
            std::memcpy(&error, CMSG_DATA(header), sizeof(error));
            limit = too_big_limit(error);
            break;
        }
    }
    if (limit == 0U)
    {
        return Report::Dropped;
    }
    fill_slot(slot, static_cast<usize>(got), (message.msg_flags & MSG_TRUNC) != 0, peer);
    slot.too_big = limit;
    return Report::TooBig;
}

// Reads reports into the slots from `count` on while `budget` lasts, Packet Too Big reports taking slots and the rest
// dropped; `waiting` ends once the queue is seen empty.
void read_reports(NativeSocket handle, cont::Span<ReceivedDatagram> slots, usize& count, usize& budget,
                  bool& waiting) noexcept
{
    while (budget != 0U && count < slots.size())
    {
        --budget;
        const Report report = read_report(handle, slots[count]);
        if (report == Report::None)
        {
            waiting = false;
            return;
        }
        if (report == Report::TooBig)
        {
            ++count;
        }
    }
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
#elif CRD_OS_LINUX
      ,
      m_reports_waiting(other.m_reports_waiting)
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
#elif CRD_OS_LINUX
        m_reports_waiting = other.m_reports_waiting;
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
#if CRD_OS_LINUX
        socket.m_features.too_big_reports = queue_reports(handle, v6, socket.m_dual_stack);
#endif
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
#if CRD_OS_LINUX
    m_reports_waiting = false;
#endif
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
    usize report_budget = kReportsPerReceive;
    usize stale_left = kReportsPerReceive;
    if (m_reports_waiting)
    {
        read_reports(handle, slots, result.count, report_budget, m_reports_waiting);
    }
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
            if (interrupted(code))
            {
                continue;
            }
            if (stale_report(code))
            {
                // A report's error, which the call returned in place of a datagram: the report is queued. Past the
                // budget the reports wait for the next call while the datagrams are read; errors that keep coming
                // (each needs a new report) end the call.
                m_reports_waiting = true;
                if (stale_left == 0U)
                {
                    return result;
                }
                --stale_left;
                read_reports(handle, slots, result.count, report_budget, m_reports_waiting);
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
    bool resent = false; // the datagram at result.count went again after a report's error
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
                if (sent < 0 && code == ENOBUFS)
                {
                    // The host took the datagram and dropped it past the socket (a full device queue, a link that
                    // refuses its size, no memory): lost on the way, as a network loses it. The host says so only to
                    // a socket that asked for reports, and would send it the same way again.
                    ++result.count;
                    resent = false;
                    continue;
                }
                if (sent < 0 && stale_report(code))
                {
                    // Either an earlier datagram's report, which the host returns to the next call and which leaves
                    // this datagram unsent, or this datagram's own error; both leave a report to read. It goes again
                    // once: a second failure is its own.
                    m_reports_waiting = true;
                    if (!resent)
                    {
                        resent = true;
                        continue;
                    }
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
            resent = false;
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
    if (!is_open())
    {
        return false;
    }
    const short events = wait_native(native(m_handle), kPollReadable, timeout_ns);
#if CRD_OS_LINUX
    if ((events & POLLERR) != 0)
    {
        m_reports_waiting = true; // the error queue holds a report, or the socket an error one left
    }
#endif
    return events != 0;
}

} // namespace crd::net
