// crd-net -- datagram sockets (4.2a; docs/design/network-foundation.md#sockets).

#include "native_socket.hpp"
#include "udp_ring.hpp"
#include "udp_rio.hpp"

#include <crd/core/environment.hpp>
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
#include <netinet/udp.h>
#endif

namespace crd::net
{
namespace
{
constexpr u64 kSimulatedSliceNs = 1000000U; // a set the host cannot wait on whole waits a millisecond at a time

#if CRD_NET_RING || CRD_OS_WINDOWS
// Whether sockets may use io_uring or Registered I/O: not turned off for the process (CRD_NET_UDP_RING=0, the plain
// path's CTest arm and an operator's way back to it).
[[nodiscard]] bool rings_allowed() noexcept
{
    char value[4] = {};
    const bool set = read_environment("CRD_NET_UDP_RING", value, sizeof(value)) == EnvironmentRead::Set;
    return !(set && std::strcmp(value, "0") == 0);
}

// The longest datagram each ring buffer holds (UdpOptions::ring_datagram_bytes).
[[nodiscard]] u32 ring_datagram_bytes(const UdpOptions& options, bool coalescing) noexcept
{
    const u32 asked = options.ring_datagram_bytes;
    if (coalescing)
    {
        return static_cast<u32>(kMaxTrainBytes);
    }
    return (asked == 0U || asked > kMaxUdpPayloadV6) ? static_cast<u32>(kMaxUdpPayloadV6) : asked;
}
#endif

#if CRD_OS_LINUX
constexpr usize kBatchChunk = 32U; // datagrams per recvmmsg, messages per sendmmsg
// The datagrams one sendmmsg carries at most, an iovec each, however its messages group them into trains.
constexpr usize kSendVectors = 4U * kMaxTrainDatagrams;
// Room for the control messages one datagram or train takes: received, an ECN codepoint and a train's datagram size
// (UDP_GRO); sent, a codepoint and the size the host segments a train at (UDP_SEGMENT).
constexpr usize kControlBytes = 2U * CMSG_SPACE(sizeof(int));
constexpr usize kSendControlBytes = CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(u16));
// Room for an ICMP report's control messages: the extended error with its sender's address, and the codepoint the
// host adds for an ECN-reading socket.
constexpr usize kReportControlBytes =
    CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_in6)) + 2U * CMSG_SPACE(sizeof(int));
// The reports one receive() reads before the datagrams behind them, so a flood of ICMP errors never starves them.
constexpr usize kReportsPerReceive = 2U * kBatchChunk;
#if CRD_NET_RING
static_assert(kRingReports == kReportsPerReceive, "a lent slot's report takes a report buffer of the ring");
#if CRD_NET_SEND_RING
static_assert(kSendRingMessages == kBatchChunk, "the send ring takes every message of a sendmmsg's chunk");
#endif
#endif

// Whether a message fits one of the host's zero-copy buffers (send_ring_fits); none does without the send ring.
[[nodiscard]] bool lends_whole([[maybe_unused]] const msghdr& message) noexcept
{
#if CRD_NET_SEND_RING
    return send_ring_fits(message);
#else
    return false;
#endif
}

// The IPv4 or IPv6 header and the UDP header: what a hop's MTU carries beyond the UDP payload.
constexpr u32 kV4Overhead = 28U;
constexpr u32 kV6Overhead = 48U;
#elif CRD_OS_WINDOWS
// Room for the control messages one datagram or train takes: a codepoint and, received, a train's datagram size
// (UDP_COALESCED_INFO) or, sent, the size the host segments it at (UDP_SEND_MSG_SIZE).
constexpr usize kControlBytes = WSA_CMSG_SPACE(sizeof(INT)) + WSA_CMSG_SPACE(sizeof(DWORD));
static_assert(RIO_CMSG_BASE_SIZE + kControlBytes <= kRioControlBytes, "a slot holds a datagram's control messages");
#endif
#if CRD_OS_LINUX
constexpr int kTrainLevel = SOL_UDP;
constexpr int kTrainReceived = UDP_GRO;    // a received train's datagram size, an int
constexpr int kTrainSegment = UDP_SEGMENT; // the size a sent train is segmented at, a u16
using TrainSegment = u16;
#elif CRD_OS_WINDOWS
constexpr int kTrainLevel = IPPROTO_UDP;
constexpr int kTrainReceived = UDP_COALESCED_INFO; // a DWORD
constexpr int kTrainSegment = UDP_SEND_MSG_SIZE;   // a DWORD
using TrainSegment = DWORD;
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
// suppression, and the buffer sizes asked for. A Windows socket made for Registered I/O (`registered`) never blocks
// and refuses FIONBIO, and refuses SIO_UDP_NETRESET: a TTL report ends one of its receives, which then carries nothing.
[[nodiscard]] SocketError configure(NativeSocket handle, bool v6, const UdpOptions& options,
                                    [[maybe_unused]] bool registered) noexcept
{
#if CRD_OS_WINDOWS
    u_long non_blocking = 1U;
    if ((!registered && ::ioctlsocket(handle, FIONBIO, &non_blocking) != 0) ||
        !set_option(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1))
    {
        return map_error(last_error(), false);
    }
    const DWORD controls[] = {SIO_UDP_CONNRESET, SIO_UDP_NETRESET};
    for (const DWORD control : controls)
    {
        BOOL report = FALSE;
        DWORD returned = 0U;
        if (::WSAIoctl(handle, control, &report, sizeof(report), nullptr, 0U, &returned, nullptr, nullptr) != 0 &&
            !(registered && control == SIO_UDP_NETRESET))
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
    slot.segment = 0U;
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

#if CRD_OS_LINUX || CRD_OS_WINDOWS
// One received control message: the datagram's codepoint or, for a train the host joined (UDP_GRO,
// UDP_COALESCED_INFO), its datagrams' size, 32 bits on both hosts. A slot holding no more than one datagram stays a
// datagram, and a size no slot can hold (Linux's int below zero) names none.
void read_control(int level, int type, const u8* data, usize bytes, ReceivedDatagram& slot) noexcept
{
    if (level == kTrainLevel && type == kTrainReceived && bytes >= sizeof(u32))
    {
        u32 segment = 0U;
        std::memcpy(&segment, data, sizeof(segment));
        slot.segment = (segment != 0U && slot.size > segment) ? segment : 0U;
        return;
    }
    static_cast<void>(ecn_from(level, type, data, bytes, slot.ecn));
}

// A slot holding a train, counted as the socket's offload did it.
void count_train(const ReceivedDatagram& slot, OffloadCounters& offloads) noexcept
{
    if (slot.segment != 0U)
    {
        ++offloads.trains_received;
        offloads.train_datagrams_received += (slot.size + slot.segment - 1U) / slot.segment;
    }
}

// Whether the host segments trains (Linux 4.18's UDP_SEGMENT, Windows 10 2004's UDP_SEND_MSG_SIZE): the option reads
// back where it is known. It is never set on the socket, which would segment every send, PMTU probes included; each
// train names its size in its own message.
[[nodiscard]] bool segments_trains(NativeSocket handle) noexcept
{
    TrainSegment value = 0U;
    NativeLength length = static_cast<NativeLength>(sizeof(value));
    return ::getsockopt(handle, kTrainLevel, kTrainSegment, reinterpret_cast<char*>(&value), &length) == 0;
}
#endif

#if CRD_OS_LINUX
// A received datagram's control messages into its slot.
void read_controls(msghdr& message, ReceivedDatagram& slot) noexcept
{
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header))
    {
        read_control(header->cmsg_level, header->cmsg_type, CMSG_DATA(header), header->cmsg_len - CMSG_LEN(0), slot);
    }
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
// dropped; `waiting` ends once the queue is seen empty. A lent slot (empty storage) takes its report in `spare`, cut
// into kReportsPerReceive buffers, one per read of the budget.
void read_reports(NativeSocket handle, cont::Span<ReceivedDatagram> slots, usize& count, usize& budget,
                  bool& waiting, cont::Span<u8> spare = {}) noexcept
{
    const usize spare_bytes = spare.size() / kReportsPerReceive;
    while (budget != 0U && count < slots.size())
    {
        --budget;
        ReceivedDatagram& slot = slots[count];
        const bool lent = slot.storage.empty() && spare_bytes != 0U;
        if (lent)
        {
            slot.storage = spare.subspan((kReportsPerReceive - budget - 1U) * spare_bytes, spare_bytes);
        }
        const Report report = read_report(handle, slot);
        if (report == Report::TooBig)
        {
            ++count;
            continue;
        }
        if (lent)
        {
            slot.storage = cont::Span<u8>();
        }
        if (report == Report::None)
        {
            waiting = false;
            return;
        }
    }
}

// The control messages of one sent message in `control`, which `message` then points at: `ecn` toward `peer` unless
// Not-ECT, and the size the host segments a train at unless `segment` is 0. Neither leaves the message without one.
void attach(msghdr& message, u8 (&control)[kSendControlBytes], const Endpoint& peer, Ecn ecn, u16 segment) noexcept
{
    if (ecn == Ecn::NotEct && segment == 0U)
    {
        return;
    }
    std::memset(control, 0, sizeof(control));
    message.msg_control = control;
    message.msg_controllen =
        (ecn != Ecn::NotEct ? CMSG_SPACE(sizeof(int)) : 0U) + (segment != 0U ? CMSG_SPACE(sizeof(u16)) : 0U);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    if (ecn != Ecn::NotEct)
    {
        ecn_message(peer, header->cmsg_level, header->cmsg_type);
        header->cmsg_len = CMSG_LEN(sizeof(int));
        const int value = static_cast<int>(ecn);
        std::memcpy(CMSG_DATA(header), &value, sizeof(value));
        header = CMSG_NXTHDR(&message, header);
    }
    if (segment != 0U)
    {
        header->cmsg_level = kTrainLevel;
        header->cmsg_type = kTrainSegment;
        header->cmsg_len = CMSG_LEN(sizeof(u16));
        std::memcpy(CMSG_DATA(header), &segment, sizeof(segment));
    }
}
#elif CRD_OS_WINDOWS
// A received datagram's control messages into its slot.
void read_controls(WSAMSG& message, ReceivedDatagram& slot) noexcept
{
    for (WSACMSGHDR* header = WSA_CMSG_FIRSTHDR(&message); header != nullptr;
         header = WSA_CMSG_NXTHDR(&message, header))
    {
        read_control(header->cmsg_level, header->cmsg_type, WSA_CMSG_DATA(header), header->cmsg_len - WSA_CMSG_LEN(0),
                     slot);
    }
}

// The control messages of one sent message in `control`, which `message` then points at: `ecn` toward `peer` unless
// Not-ECT, and the size the host segments a train at unless `segment` is 0. Neither leaves the message without one.
void attach(WSAMSG& message, u8 (&control)[kControlBytes], const Endpoint& peer, Ecn ecn, DWORD segment) noexcept
{
    if (ecn == Ecn::NotEct && segment == 0U)
    {
        return;
    }
    std::memset(control, 0, sizeof(control));
    const usize length =
        (ecn != Ecn::NotEct ? WSA_CMSG_SPACE(sizeof(INT)) : 0U) + (segment != 0U ? WSA_CMSG_SPACE(sizeof(DWORD)) : 0U);
    message.Control = WSABUF{static_cast<ULONG>(length), reinterpret_cast<CHAR*>(control)};
    WSACMSGHDR* header = WSA_CMSG_FIRSTHDR(&message);
    if (ecn != Ecn::NotEct)
    {
        ecn_message(peer, header->cmsg_level, header->cmsg_type);
        header->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
        const INT value = static_cast<INT>(ecn);
        std::memcpy(WSA_CMSG_DATA(header), &value, sizeof(value));
        header = WSA_CMSG_NXTHDR(&message, header);
    }
    if (segment != 0U)
    {
        header->cmsg_level = kTrainLevel;
        header->cmsg_type = kTrainSegment;
        header->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
        std::memcpy(WSA_CMSG_DATA(header), &segment, sizeof(segment));
    }
}
#endif

} // namespace

usize segment_train(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept
{
    if (datagrams.empty())
    {
        return 0U;
    }
    const OutgoingDatagram& first = datagrams[0];
    const usize size = first.bytes.size();
    const usize limit = payload_limit(first.peer);
    usize total = size;
    usize count = 1U;
    // A shorter datagram ends the train, as the host segments every datagram but the last at the first's size; one
    // directly behind the first leaves it alone, so a lone longer datagram (a PMTU probe) never takes the next one's
    // fate on a path that drops a train whole (a forwarding hop checks the train's datagram size).
    if (datagrams.size() > 1U && datagrams[1].bytes.size() != size)
    {
        return 1U;
    }
    while (count < datagrams.size() && count < kMaxTrainDatagrams && datagrams[count - 1U].bytes.size() == size)
    {
        const OutgoingDatagram& next = datagrams[count];
        if (next.peer != first.peer || next.ecn != first.ecn || next.bytes.empty() || next.bytes.size() > size ||
            total + next.bytes.size() > limit)
        {
            break;
        }
        total += next.bytes.size();
        ++count;
    }
    return count;
}

UdpSocket::~UdpSocket()
{
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : DatagramSocket(), m_handle(other.m_handle), m_local(other.m_local), m_dual_stack(other.m_dual_stack),
      m_features(other.m_features), m_offloads(other.m_offloads), m_sends_taken(other.m_sends_taken)
#if CRD_OS_WINDOWS
      ,
      m_receive_message(other.m_receive_message), m_rio(other.m_rio), m_read_event(other.m_read_event)
#elif CRD_OS_LINUX
      ,
      m_reports_waiting(other.m_reports_waiting), m_ring(other.m_ring), m_send_ring(other.m_send_ring),
      m_zero_copy_bytes(other.m_zero_copy_bytes)
#endif
{
    other.m_handle = kNoHandle;
#if CRD_OS_WINDOWS
    other.m_rio = nullptr;
    other.m_read_event = 0U;
#elif CRD_OS_LINUX
    other.m_ring = nullptr;
    other.m_send_ring = nullptr;
#endif
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
        m_offloads = other.m_offloads;
        m_sends_taken = other.m_sends_taken;
#if CRD_OS_WINDOWS
        m_receive_message = other.m_receive_message;
        m_rio = other.m_rio;
        m_read_event = other.m_read_event;
        other.m_rio = nullptr;
        other.m_read_event = 0U;
#elif CRD_OS_LINUX
        m_reports_waiting = other.m_reports_waiting;
        m_ring = other.m_ring;
        m_send_ring = other.m_send_ring;
        m_zero_copy_bytes = other.m_zero_copy_bytes;
        other.m_ring = nullptr;
        other.m_send_ring = nullptr;
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
    // Registered I/O first where it may serve. A socket made for it takes no other call, so one the host refuses any
    // part of before the bind is closed and a plain one opened in its place.
    const NativeSocket registered =
        (options.registered_io && options.ring_buffers != 0U && rings_allowed()) ? rio_socket(family) : kInvalidSocket;
    const NativeSocket handle = (registered != kInvalidSocket)
                                    ? registered
                                    : ::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0U,
                                                   WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
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
#if CRD_OS_WINDOWS
    error = configure(handle, v6, options, registered != kInvalidSocket);
#else
    error = configure(handle, v6, options, false);
#endif
#if CRD_OS_WINDOWS
    if (error == SocketError::None && registered == kInvalidSocket)
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
        // Every wait of a plain socket is on this event, which WSAEventSelect sets while a datagram waits, so a
        // SocketSet waits on it beside Registered I/O's.
        const WSAEVENT event = (error == SocketError::None) ? ::WSACreateEvent() : WSA_INVALID_EVENT;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        socket.m_read_event = (event != WSA_INVALID_EVENT) ? static_cast<u64>(reinterpret_cast<uintptr_t>(event)) : 0U;
        if (error == SocketError::None && (event == WSA_INVALID_EVENT || ::WSAEventSelect(handle, event, FD_READ) != 0))
        {
            error = (event == WSA_INVALID_EVENT) ? SocketError::Resources : map_error(last_error(), false);
        }
    }
#endif
#if CRD_OS_LINUX || CRD_OS_WINDOWS
    if (error == SocketError::None)
    {
        socket.m_features = enable_ecn(handle, v6, socket.m_dual_stack);
        socket.m_features.dont_fragment = forbid_fragmentation(handle, v6, socket.m_dual_stack);
#if CRD_OS_LINUX
        socket.m_features.too_big_reports = queue_reports(handle, v6, socket.m_dual_stack);
        socket.m_features.coalesced_receive = options.coalesce_receive && set_option(handle, SOL_UDP, UDP_GRO, 1);
#else
        // The host joins trains of at most one IPv6 UDP payload (msquic's bound), so a kMaxTrainBytes slot holds any.
        socket.m_features.coalesced_receive =
            options.coalesce_receive &&
            set_option(handle, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE, static_cast<int>(kMaxUdpPayloadV6));
#endif
        socket.m_features.segmented_send = options.segment_send && segments_trains(handle);
    }
#endif
#if CRD_OS_WINDOWS
    if (registered != kInvalidSocket)
    {
        const u32 datagram_bytes = ring_datagram_bytes(options, socket.m_features.coalesced_receive);
        socket.m_rio = (error == SocketError::None)
                           ? rio_open(handle, options.ring_buffers, datagram_bytes, socket.m_features.segmented_send)
                           : nullptr;
        if (socket.m_rio == nullptr)
        {
            socket.close();
            UdpOptions plain = options;
            plain.registered_io = false;
            return open(plain, error);
        }
        static_cast<void>(rio_watch(*socket.m_rio));
        socket.m_features.lent_receive = true;
        socket.m_features.receive_bytes = datagram_bytes;
        socket.m_features.send_bytes = datagram_bytes;
    }
#endif
#if CRD_NET_RING
    if (error == SocketError::None && options.ring_buffers != 0U && rings_allowed())
    {
        const u32 datagram_bytes = ring_datagram_bytes(options, socket.m_features.coalesced_receive);
        socket.m_ring = ring_open(handle, options.ring_buffers, datagram_bytes, static_cast<u32>(kControlBytes));
        socket.m_features.lent_receive = socket.m_ring != nullptr;
        socket.m_features.receive_bytes = (socket.m_ring != nullptr) ? datagram_bytes : 0U;
    }
#endif
#if CRD_NET_SEND_RING
    if (error == SocketError::None && options.zero_copy_bytes != 0U && rings_allowed())
    {
        socket.m_send_ring = send_ring_open(handle);
        socket.m_features.lent_send = socket.m_send_ring != nullptr;
        socket.m_zero_copy_bytes = options.zero_copy_bytes;
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
    rio_close(m_rio); // after the socket, whose close ends every request on its buffers
    if (m_read_event != 0U)
    {
        ::WSACloseEvent(event_of(m_read_event));
    }
    ::WSACleanup();
#else
#if CRD_NET_RING
    ring_close(m_ring); // before the socket: the host stops writing the ring's buffers first
#endif
#if CRD_NET_SEND_RING
    send_ring_close(m_send_ring); // the host holds no page of the caller's once it returns
#endif
    ::close(native(m_handle));
#endif
    m_handle = kNoHandle;
    m_local = Endpoint{};
    m_features = SocketFeatures{};
    m_offloads = OffloadCounters{};
    m_sends_taken = 0U;
#if CRD_OS_WINDOWS
    m_rio = nullptr;
    m_read_event = 0U;
#elif CRD_OS_LINUX
    m_reports_waiting = false;
    m_ring = nullptr;
    m_send_ring = nullptr;
    m_zero_copy_bytes = 0U;
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
#if CRD_NET_RING
    if (m_ring != nullptr)
    {
        receive_ring(slots, result);
        return result;
    }
#elif CRD_OS_WINDOWS
    if (m_rio != nullptr)
    {
        receive_rio(slots, result);
        return result;
    }
    // The event is set again by the first read that leaves a datagram waiting, or by one that arrives after the
    // last; left set, it would wake a wait with nothing to read. A receive of no slot makes no read, which alone
    // would set it again, so it leaves the event as it is.
    if (!slots.empty())
    {
        static_cast<void>(::WSAResetEvent(event_of(m_read_event)));
    }
#endif
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
            read_controls(messages[index].msg_hdr, slot);
            count_train(slot, m_offloads);
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
            read_controls(message, slot);
            count_train(slot, m_offloads);
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
    return transmit(datagrams, false);
}

BatchResult UdpSocket::send_stable(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept
{
    return transmit(datagrams, true);
}

u64 UdpSocket::sends_released([[maybe_unused]] bool wait) noexcept
{
#if CRD_NET_SEND_RING
    if (m_send_ring != nullptr)
    {
        return send_ring_released(*m_send_ring, m_sends_taken, wait, m_offloads);
    }
#endif
    return m_sends_taken;
}

BatchResult UdpSocket::transmit(cont::ConstSpan<OutgoingDatagram> datagrams, [[maybe_unused]] bool stable) noexcept
{
    BatchResult result;
    if (!is_open())
    {
        result.error = SocketError::Closed;
        return result;
    }
#if CRD_OS_WINDOWS
    if (m_rio != nullptr)
    {
        return transmit_rio(datagrams);
    }
#endif
    const NativeSocket handle = native(m_handle);
    const bool v6 = m_local.address.family == AddressFamily::V6;
#if CRD_OS_LINUX
    bool resent = false;    // the message at result.count went again after a report's error
    usize plain_until = 0U; // the datagrams before it go a message each: a train the host refused
    usize copied_at = ~usize{0}; // a message starting at this datagram goes copied: the host failed it zero-copy
    while (result.count < datagrams.size())
    {
        mmsghdr messages[kBatchChunk];
        iovec vectors[kSendVectors];
        usize carried[kBatchChunk]; // datagrams per message
        NativeAddress peers[kBatchChunk];
        alignas(cmsghdr) u8 controls[kBatchChunk][kSendControlBytes];
        u64 first[kBatchChunk]; // the number of each message's first datagram
        u32 zero_copy = 0U;     // bit i: message i may go zero-copy
        bool marked = false;
        usize chunk = 0U;
        usize vector = 0U;
        usize next = result.count;
        SocketError refused = SocketError::None;
        while (chunk < kBatchChunk && next < datagrams.size() && vector < kSendVectors)
        {
            const OutgoingDatagram& datagram = datagrams[next];
            refused = datagram.bytes.size() > payload_limit(datagram.peer)
                          ? SocketError::MessageTooLarge
                          : to_native(datagram.peer, v6, m_dual_stack, peers[chunk]);
            if (refused != SocketError::None)
            {
                break;
            }
            // Every datagram of a train shares the first's peer, so the first's checks hold for all of them.
            usize count =
                (m_features.segmented_send && next >= plain_until) ? segment_train(datagrams.subspan(next)) : 1U;
            count = (count < kSendVectors - vector) ? count : kSendVectors - vector;
            usize message_bytes = 0U;
            for (usize index = 0U; index < count; ++index)
            {
                const cont::ConstSpan<u8> bytes = datagrams[next + index].bytes;
                // sendmmsg only reads the bytes; iovec has no const form.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                vectors[vector + index] = iovec{const_cast<u8*>(bytes.data()), bytes.size()};
                message_bytes += bytes.size();
            }
            messages[chunk] = mmsghdr{};
            messages[chunk].msg_hdr.msg_name = &peers[chunk].storage;
            messages[chunk].msg_hdr.msg_namelen = peers[chunk].length;
            messages[chunk].msg_hdr.msg_iov = &vectors[vector];
            messages[chunk].msg_hdr.msg_iovlen = count;
            // Numbered as the datagrams before it in this call will be once they leave.
            first[chunk] = m_sends_taken + (next - result.count);
            const bool lend = stable && m_features.lent_send && message_bytes >= m_zero_copy_bytes &&
                              next != copied_at && lends_whole(messages[chunk].msg_hdr);
            zero_copy |= lend ? (1U << chunk) : 0U;
            const Ecn ecn = m_features.ecn_marking ? datagram.ecn : Ecn::NotEct;
            marked = marked || ecn != Ecn::NotEct;
            attach(messages[chunk].msg_hdr, controls[chunk], datagram.peer, ecn,
                   count > 1U ? static_cast<u16>(datagram.bytes.size()) : u16{0U});
            carried[chunk++] = count;
            vector += count;
            next += count;
        }
        if (chunk != 0U)
        {
            bool lent_first = false; // message 0 went zero-copy
#if CRD_NET_SEND_RING
            const int sent = (zero_copy != 0U && send_ring_wanted(*m_send_ring))
                                 ? send_ring_send(*m_send_ring, messages, static_cast<u32>(chunk), zero_copy, first,
                                                  lent_first, m_offloads)
                                 : ::sendmmsg(handle, messages, static_cast<unsigned int>(chunk),
                                              MSG_DONTWAIT | MSG_NOSIGNAL);
#else
            static_cast<void>(zero_copy); // no socket lends without the ring
            const int sent =
                ::sendmmsg(handle, messages, static_cast<unsigned int>(chunk), MSG_DONTWAIT | MSG_NOSIGNAL);
#endif
            if (sent <= 0)
            {
                const int code = last_error();
                if (sent < 0 && interrupted(code))
                {
                    continue;
                }
                if (sent < 0 && lent_first && !would_block(code))
                {
                    // The host failed the message zero-copy (pinning past RLIMIT_MEMLOCK, a socket that takes no
                    // zero-copy send, more fragments than its buffer holds): it goes again copied, which meets any
                    // error of its own as every copied send does, and a socket that takes none stops lending.
                    ++m_offloads.zero_copy_refused;
                    m_features.lent_send = m_features.lent_send && code != EOPNOTSUPP;
                    copied_at = result.count;
                    continue;
                }
                if (sent < 0 && code == ENOBUFS)
                {
                    // The host took the datagram and dropped it past the socket (a full device queue, a link that
                    // refuses its size, no memory): lost on the way, as a network loses it. The host says so only to
                    // a socket that asked for reports, and would send it the same way again. A train is lost whole.
                    sent_train(carried[0], result);
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
                if (sent < 0 && carried[0] > 1U && !would_block(code))
                {
                    // A train the host refused (a segment past the device's MTU, a route through IPsec, a device
                    // that cannot checksum it): its datagrams go a message each and meet their own errors. EIO is the
                    // host refusing to segment at all, so this socket stops asking.
                    ++m_offloads.trains_refused;
                    m_features.segmented_send = m_features.segmented_send && code != EIO;
                    plain_until = result.count + carried[0];
                    resent = false;
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
            for (usize index = 0U; index < static_cast<usize>(sent); ++index)
            {
                sent_train(carried[index], result);
            }
            resent = false;
            if (static_cast<usize>(sent) < chunk)
            {
                continue; // the next call starts at the message that failed and reports why
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
    bool plain = false;     // the datagram the host refused marked goes again unmarked
    usize plain_until = 0U; // the datagrams before it go a call each: a train the host refused
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
        // A train goes as one message gathering its datagrams in place, the host segmenting it at the first's size
        // (USO); every datagram of it shares the first's peer, so the first's checks hold for all of them.
        const usize count = (m_features.segmented_send && result.count >= plain_until)
                                ? segment_train(datagrams.subspan(result.count))
                                : 1U;
        WSABUF buffers[kMaxTrainDatagrams];
        for (usize index = 0U; index < count; ++index)
        {
            const cont::ConstSpan<u8> bytes = datagrams[result.count + index].bytes;
            // WSASendMsg only reads the bytes; WSABUF has no const form.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            CHAR* const data = reinterpret_cast<CHAR*>(const_cast<u8*>(bytes.data()));
            buffers[index] = WSABUF{static_cast<ULONG>(bytes.size()), data};
        }
        WSAMSG message{};
        message.name = reinterpret_cast<sockaddr*>(&peer.storage);
        message.namelen = peer.length;
        message.lpBuffers = buffers;
        message.dwBufferCount = static_cast<DWORD>(count);
        alignas(WSACMSGHDR) u8 control[kControlBytes];
        const bool marked = !plain && datagram.ecn != Ecn::NotEct && m_features.ecn_marking;
        attach(message, control, datagram.peer, marked ? datagram.ecn : Ecn::NotEct,
               count > 1U ? static_cast<DWORD>(datagram.bytes.size()) : 0U);
        DWORD bytes_sent = 0U;
        const int sent = ::WSASendMsg(handle, &message, 0U, &bytes_sent, nullptr, nullptr);
#else
        const ssize_t sent = ::sendto(handle, datagram.bytes.data(), datagram.bytes.size(), 0,
                                      reinterpret_cast<const sockaddr*>(&peer.storage), peer.length);
        const usize count = 1U;
#endif
        if (sent < 0)
        {
            const int code = last_error();
            if (interrupted(code))
            {
                continue;
            }
#if CRD_OS_WINDOWS
            if (count > 1U && !would_block(code))
            {
                // A train the host refused (a segment past the interface's MTU, a codepoint it refuses): its
                // datagrams go a call each and meet their own errors. A host that refuses to segment at all, an
                // unmarked train refused as invalid included, has this socket stop asking.
                ++m_offloads.trains_refused;
                const bool unsegmented = code == WSAEOPNOTSUPP || (code == WSAEINVAL && !marked);
                m_features.segmented_send = m_features.segmented_send && !unsegmented;
                plain_until = result.count + count;
                continue;
            }
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
        sent_train(count, result);
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
#if CRD_NET_RING
    if (m_ring != nullptr && !ring_armed(*m_ring))
    {
        static_cast<void>(ring_arm(*m_ring, true)); // what waits in the socket then completes into the ring
    }
#endif
#if CRD_OS_WINDOWS
    if (m_rio != nullptr)
    {
        static_cast<void>(rio_watch(*m_rio)); // set at once if a completion waits
    }
    const HANDLE event = event_of(wait_handle());
    return wait_events(&event, 1U, timeout_ns);
#else
    const short events = wait_native(native(wait_handle()), kPollReadable, timeout_ns);
    if (events != 0)
    {
        waited((events & POLLERR) != 0);
    }
    return events != 0;
#endif
}

u64 UdpSocket::wait_handle() const noexcept
{
#if CRD_OS_WINDOWS
    if (m_rio != nullptr)
    {
        return rio_event(*m_rio);
    }
    return is_open() ? m_read_event : kNoWaitHandle;
#else
#if CRD_NET_RING
    // The ring while it receives or holds completions; the socket while no receive is armed, so a datagram that
    // arrives then is seen where it waits.
    if (m_ring != nullptr && (ring_armed(*m_ring) || ring_pending(*m_ring)))
    {
        return static_cast<u64>(ring_handle(*m_ring));
    }
#endif
    return m_handle;
#endif
}

#if CRD_OS_LINUX
void UdpSocket::receive_ring([[maybe_unused]] cont::Span<ReceivedDatagram> slots,
                             [[maybe_unused]] BatchResult& result) noexcept
{
#if CRD_NET_RING
    UdpRing& ring = *m_ring;
    const NativeSocket handle = native(m_handle);
    const cont::Span<u8> reports = ring_reports(ring);
    ring_recycle(ring); // the buffers the last call lent
    static_cast<void>(ring_follow(ring));
    usize report_budget = kReportsPerReceive;
    if (m_reports_waiting)
    {
        read_reports(handle, slots, result.count, report_budget, m_reports_waiting, reports);
    }
    // An ended receive is armed again at most twice a call, so a host that keeps ending it cannot hold the call.
    u32 arms_left = 2U;
    while (result.count < slots.size())
    {
        RingCompletion completion;
        if (!ring_next(ring, completion))
        {
            if (ring_armed(ring) || arms_left == 0U)
            {
                break;
            }
            --arms_left;
            if (!ring_arm(ring, false))
            {
                break;
            }
            continue; // what waits in the socket completes as the receive arms
        }
        if (completion.report)
        {
            m_reports_waiting = true;
            read_reports(handle, slots, result.count, report_budget, m_reports_waiting, reports);
            continue;
        }
        if (completion.datagram)
        {
            ReceivedDatagram& slot = slots[result.count++];
            const cont::Span<u8> payload = completion.message.payload;
            bool truncated = completion.message.truncated;
            if (slot.storage.empty())
            {
                slot.storage = payload;
            }
            else
            {
                const usize copied = (payload.size() < slot.storage.size()) ? payload.size() : slot.storage.size();
                std::memcpy(slot.storage.data(), payload.data(), copied);
                truncated = truncated || copied < payload.size();
            }
            sockaddr_storage peer{};
            std::memcpy(&peer, completion.message.header.msg_name, completion.message.header.msg_namelen);
            fill_slot(slot, payload.size(), truncated, peer);
            read_controls(completion.message.header, slot);
            count_train(slot, m_offloads);
        }
        if (completion.error == 0 || completion.error == ENOBUFS || completion.error == ECANCELED)
        {
            // No end, an end without an error, every buffer lent, or the thread that armed it gone: armed again
            // here when it ended.
            continue;
        }
        if (stale_report(completion.error))
        {
            // A report's error, which ended the receive in place of a datagram: the report is queued.
            m_reports_waiting = true;
            read_reports(handle, slots, result.count, report_budget, m_reports_waiting, reports);
            continue;
        }
        result.error = map_error(completion.error, false);
        break;
    }
    // An end the host posted right behind the last datagram taken, which carries nothing: read now, so it wakes no
    // wait (the host ends a receive once it has filled every free buffer, whether more is coming or not).
    RingCompletion end;
    while (result.error == SocketError::None && ring_next(ring, end, true))
    {
        m_reports_waiting = m_reports_waiting || end.report || stale_report(end.error);
    }
    // Armed for the next wait unless every buffer is lent to these slots: the wait then watches the socket itself
    // (wait_handle), and the next receive arms it once it has the buffers back. The error queue's watch, armed here
    // alone, completes at once when reports are left past this call.
    static_cast<void>(ring_arm(ring, true));
#endif
}
#endif

#if CRD_OS_WINDOWS
void UdpSocket::receive_rio(cont::Span<ReceivedDatagram> slots, BatchResult& result) noexcept
{
    UdpRio& rio = *m_rio;
    rio_recycle(rio); // the buffers the last call lent
    while (result.count < slots.size())
    {
        RioReceived received;
        if (!rio_next(rio, received))
        {
            break;
        }
        if (received.error != 0)
        {
            rio_release(rio, received.buffer, false);
            // An ICMP report's error (the socket asks for none) or a receive the host cancelled carries nothing.
            if (stale_report(received.error) || received.error == WSA_OPERATION_ABORTED)
            {
                continue;
            }
            result.error = map_error(received.error, false);
            break;
        }
        ReceivedDatagram& slot = slots[result.count++];
        const cont::Span<u8> payload = received.payload;
        const bool lend = slot.storage.empty();
        bool truncated = received.truncated;
        if (lend)
        {
            slot.storage = payload;
        }
        else
        {
            const usize copied = (payload.size() < slot.storage.size()) ? payload.size() : slot.storage.size();
            std::memcpy(slot.storage.data(), payload.data(), copied);
            truncated = truncated || copied < payload.size();
        }
        fill_slot(slot, payload.size(), truncated, received.peer);
        read_controls(received.control, slot);
        count_train(slot, m_offloads);
        rio_release(rio, received.buffer, lend); // a copied datagram's buffer is posted again at once
    }
    rio_flush(rio);
    static_cast<void>(rio_watch(rio)); // the event for the next wait, set at once if completions are left
}

BatchResult UdpSocket::transmit_rio(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept
{
    BatchResult result;
    UdpRio& rio = *m_rio;
    const bool v6 = m_local.address.family == AddressFamily::V6;
    RioFailures failures;
    rio_reap(rio, failures); // the sends since the last call that completed, and those that failed
    while (result.count < datagrams.size())
    {
        const OutgoingDatagram& datagram = datagrams[result.count];
        NativeAddress peer;
        const usize bytes = datagram.bytes.size();
        result.error = (bytes > payload_limit(datagram.peer) || bytes > m_features.send_bytes)
                           ? SocketError::MessageTooLarge
                           : to_native(datagram.peer, v6, m_dual_stack, peer);
        // A train goes end to end in a train slot, which the host segments at the first's size (USO); every
        // datagram of it shares the first's peer and is no longer, so the first's checks hold for all of them. One
        // with no train slot free leaves its first datagram alone and tries again with the next.
        usize count = m_features.segmented_send ? segment_train(datagrams.subspan(result.count)) : 1U;
        RioSendSlot slot;
        bool held = result.error == SocketError::None && rio_send_slot(rio, count > 1U, slot, failures);
        if (!held && result.error == SocketError::None && count > 1U)
        {
            count = 1U;
            held = rio_send_slot(rio, false, slot, failures);
        }
        if (result.error == SocketError::None && !held)
        {
            result.error = SocketError::Busy;
        }
        if (result.error != SocketError::None)
        {
            break;
        }
        usize filled = 0U;
        for (usize index = 0U; index < count; ++index)
        {
            const cont::ConstSpan<u8> part = datagrams[result.count + index].bytes;
            std::memcpy(slot.bytes.data() + filled, part.data(), part.size());
            filled += part.size();
        }
        *slot.peer = SOCKADDR_INET{};
        std::memcpy(slot.peer, &peer.storage, static_cast<usize>(peer.length));
        // Windows refuses a sender's CE, a router's mark (the WSASendMsg path learns it from WSAEINVAL); here it
        // would fail only as the send completes, so that datagram leaves unmarked.
        const Ecn ecn = (m_features.ecn_marking && datagram.ecn != Ecn::Ce) ? datagram.ecn : Ecn::NotEct;
        WSAMSG message{};
        alignas(WSACMSGHDR) u8 control[kControlBytes];
        attach(message, control, datagram.peer, ecn, count > 1U ? static_cast<DWORD>(bytes) : 0U);
        if (message.Control.len != 0U)
        {
            std::memcpy(slot.control.data(), control, message.Control.len);
        }
        int code = 0;
        if (!rio_send(rio, slot, filled, message.Control.len, static_cast<u32>(count), ecn != Ecn::NotEct, code))
        {
            // The request queue full is the slots all in flight; anything else is this datagram's alone.
            result.error = (code == WSAENOBUFS) ? SocketError::Busy : map_error(code, true);
            break;
        }
        sent_train(count, result);
    }
    rio_commit(rio);
    // What failed late changes what the next sends ask, as the same errors in the call do on the WSASendMsg path.
    m_offloads.sends_failed += failures.datagrams;
    m_offloads.trains_refused += failures.trains;
    m_features.segmented_send = m_features.segmented_send && !failures.unsegmented;
    m_features.ecn_marking = m_features.ecn_marking && !failures.unmarked;
    return result;
}
#endif

void UdpSocket::sent_train(usize datagrams, BatchResult& result) noexcept
{
    result.count += datagrams;
    m_sends_taken += datagrams;
    if (datagrams > 1U)
    {
        ++m_offloads.trains_sent;
        m_offloads.train_datagrams_sent += datagrams;
    }
}

void UdpSocket::waited([[maybe_unused]] bool error) noexcept
{
#if CRD_OS_LINUX
    if (error)
    {
        m_reports_waiting = true; // the error queue holds a report, or the socket an error one left
    }
#endif
}

// ---- SocketSet ----

SocketSet::SocketSet(DatagramSocket& socket) noexcept : m_count(1U)
{
    m_sockets[0] = &socket;
}

SocketSet::SocketSet(cont::ConstSpan<DatagramSocket*> sockets) noexcept
{
    if (sockets.empty() || sockets.size() > kMaxSetSockets)
    {
        return;
    }
    for (DatagramSocket* const socket : sockets)
    {
        if (socket == nullptr)
        {
            return;
        }
    }
    for (usize index = 0U; index < sockets.size(); ++index)
    {
        m_sockets[index] = sockets[index];
    }
    m_count = static_cast<u32>(sockets.size());
}

SocketFeatures SocketSet::features() const noexcept
{
    SocketFeatures all;
    all.ecn_reading = m_count != 0U;
    all.ecn_marking = all.ecn_reading;
    all.dont_fragment = all.ecn_reading;
    all.too_big_reports = all.ecn_reading;
    for (u32 index = 0U; index < m_count; ++index)
    {
        const SocketFeatures one = m_sockets[index]->features();
        all.ecn_reading = all.ecn_reading && one.ecn_reading;
        all.ecn_marking = all.ecn_marking && one.ecn_marking;
        all.dont_fragment = all.dont_fragment && one.dont_fragment;
        all.too_big_reports = all.too_big_reports && one.too_big_reports;
    }
    return all;
}

bool SocketSet::wait_readable(u64 timeout_ns) const noexcept
{
    if (m_count <= 1U)
    {
        return m_count == 1U && m_sockets[0]->wait_readable(timeout_ns);
    }
#if CRD_OS_WINDOWS
    HANDLE events[kMaxSetSockets] = {};
    bool host = true;
    for (u32 index = 0U; index < m_count; ++index)
    {
        const u64 handle = m_sockets[index]->wait_handle();
        host = host && handle != kNoWaitHandle;
        events[index] = event_of(handle);
    }
    if (host)
    {
        return wait_events(events, m_count, timeout_ns);
    }
#else
    NativePoll entries[kMaxSetSockets] = {};
    bool host = true;
    for (u32 index = 0U; index < m_count; ++index)
    {
        const u64 handle = m_sockets[index]->wait_handle();
        host = host && handle != kNoWaitHandle;
        entries[index].fd = native(handle);
        entries[index].events = kPollReadable;
    }
    if (host)
    {
        if (wait_native(entries, m_count, timeout_ns) == 0U)
        {
            return false;
        }
        for (u32 index = 0U; index < m_count; ++index)
        {
            if (entries[index].revents != 0)
            {
                m_sockets[index]->waited((entries[index].revents & POLLERR) != 0);
            }
        }
        return true;
    }
#endif
    for (u32 index = 0U; index < m_count; ++index)
    {
        if (m_sockets[index]->wait_readable(0U))
        {
            return true;
        }
    }
    if (m_sockets[0]->wait_readable((timeout_ns < kSimulatedSliceNs) ? timeout_ns : kSimulatedSliceNs))
    {
        return true;
    }
    for (u32 index = 1U; index < m_count; ++index)
    {
        if (m_sockets[index]->wait_readable(0U))
        {
            return true;
        }
    }
    return false;
}

} // namespace crd::net
