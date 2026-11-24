#pragma once

// crd-net -- datagram sockets (4.2a; contract: docs/design/network-foundation.md#sockets). The transport's one way onto
// the host's network: datagrams move in batches, into storage the caller owns, and nothing here allocates or blocks
// except wait_readable(). Every path the transport drives is a DatagramSocket, so NET-SIM's virtual sockets and
// NET-DEBUG's link conditioner stand where a UdpSocket stands, and the transport cannot tell them apart.

#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/net/ecn.hpp>
#include <crd/net/endpoint.hpp>

namespace crd::net
{
#if CRD_OS_LINUX
struct UdpRing;
struct UdpSendRing;
#elif CRD_OS_WINDOWS
struct UdpRio;
#endif

enum class SocketError : u8
{
    None,
    Busy,               // send: the host's buffer is full; send the rest later
    FamilyUnsupported,  // the host has no such address family (an IPv6-less host)
    FamilyMismatch,     // a peer the socket cannot reach: IPv6 through IPv4, IPv4 through an IPv6-only socket
    AddressInUse,       // bind: another socket holds the port
    AddressUnavailable, // bind: not an address of this host
    Permission,         // the host refused (a privileged port, a firewall, a broadcast address)
    Resources,          // descriptors, buffers or memory ran out
    MessageTooLarge,    // send: more than a datagram of the peer's family carries, or than the host's path takes
    Unreachable,        // send: the host has no route to the peer
    Refused,            // connect: nothing listens at the peer
    Reset,              // a stream's connection was reset or aborted
    TimedOut,           // the host gave up on the peer
    Closed,             // the socket is not open
    Failed,             // anything else the host reported
};

// The largest payloads a UDP datagram carries: 65535 less the IPv4 and UDP headers, and less the UDP header for IPv6,
// whose payload length field leaves its own header out (RFC 8200; jumbograms are not used).
inline constexpr usize kMaxUdpPayloadV4 = 65507U;
inline constexpr usize kMaxUdpPayloadV6 = 65527U;

// The step at which the host refused the ring UdpOptions::ring_buffers asked for (SocketFeatures::ring_refusal). A
// refusal of its memory or resources is asked again with half the buffers, down to 4; a socket that ends with no
// ring receives through its plain path.
enum class RingRefusal : u8
{
    None,     // the socket lends all it asked for, or asked for no ring (ring_buffers 0; Windows without registered_io)
    Count,    // ring_buffers is not a power of two from 4 to 32768
    Off,      // CRD_NET_UDP_RING=0, or a host with neither io_uring nor Registered I/O
    Memory,   // the pages of its buffers
    Setup,    // the ring itself: io_uring_setup and its mappings, or Registered I/O's table and queues
    Register, // handing its buffers to the host
    Arm,      // the first receive (a kernel before 6.0 refuses the multishot recvmsg)
};

// A socket the host cannot wait on together with others (DatagramSocket::wait_handle).
inline constexpr u64 kNoWaitHandle = ~u64{0};
// The most sockets one loop and its host serve together: one per path a QUIC connection knows (kQuicMaxPaths).
inline constexpr u32 kMaxSetSockets = 4U;
// A train: datagrams to one peer with one ECN codepoint, each the first's size but the last, which may be shorter, that
// the host segments from one send (Linux's UDP_SEGMENT, Windows' USO) or joins into one receive (UDP_GRO, URO). One
// carries at most kMaxTrainDatagrams of them (every Linux kernel's UDP_MAX_SEGMENTS) and, sent, at most the peer
// family's UDP payload; a receive slot of kMaxTrainBytes holds the longest train a host joins.
inline constexpr usize kMaxTrainDatagrams = 64U;
inline constexpr usize kMaxTrainBytes = 65535U;

struct UdpOptions
{
    Endpoint bind;                 // port 0 takes an ephemeral port; the address picks the socket's family
    bool dual_stack = true;        // an IPv6 socket also carries IPv4, its peers reported as IPv4 endpoints
    u32 receive_buffer_bytes = 0U; // 0 keeps the host's default
    u32 send_buffer_bytes = 0U;    // 0 keeps the host's default
    // A send hands the host each train in one call where it segments them (SocketFeatures::segmented_send); false
    // keeps the portable path, one datagram a call.
    bool segment_send = true;
    // The host may join one sender's datagrams into one train per receive slot (SocketFeatures::coalesced_receive):
    // the caller then gives every slot kMaxTrainBytes, as IoLoop does.
    bool coalesce_receive = false;
    // Linux's io_uring receive (SocketFeatures::lent_receive) where the host has it, and with registered_io Windows'
    // Registered I/O: `ring_buffers` buffers the host fills ahead of receive(), a power of two from 4 to 32768, each
    // holding a datagram of ring_datagram_bytes. Linux backs their pages as the host first writes them, so the memory
    // a ring holds follows the datagrams that arrive. A host short of memory for them gives fewer
    // (SocketFeatures::ring_buffers). Any other count, or CRD_NET_UDP_RING=0 in the environment, keeps the recvmmsg or
    // WSARecvMsg path.
    u32 ring_buffers = 64U;
    // The longest datagram a ring buffer (and a Windows send slot) holds whole: 0, or anything over kMaxUdpPayloadV6,
    // holds any; coalesce_receive's buffers hold kMaxTrainBytes whatever it says. A host that sends no datagram
    // longer than its path's MTU sets that here (SocketFeatures::receive_bytes and send_bytes report it).
    u32 ring_datagram_bytes = 0U;
    // Windows' Registered I/O for the receives and the sends, where the host has it. Its receives are the posted ring
    // buffers alone: a datagram that arrives while all of them are out is lost, whatever receive_buffer_bytes says,
    // so a host sizes ring_buffers for its bursts and ring_datagram_bytes for its path's MTU. Their pages, and 64
    // send slots of ring_datagram_bytes beside them (and 8 of kMaxTrainBytes for trains), stay locked while the
    // socket is open. The host's request queue is unsynchronised: the socket's sends and receives never run on two
    // threads at once (an IoLoop makes both on its own thread). Off by default (S33's Registered I/O).
    bool registered_io = false;
    // A message of at least this many bytes (a train, or one large datagram) that send_stable() hands over leaves
    // through io_uring's zero-copy send where the host has it (SocketFeatures::lent_send): the host reads its bytes
    // in place until sends_released() passes them. 0, the default, or CRD_NET_UDP_RING=0 copies every send: the
    // board measured zero-copy at 1.4 to 2.9 times a copy's sender CPU at every size (S33's zero-copy send), so a
    // host turns it on where its own board shows it gains (a NIC that segments and sends in place).
    u32 zero_copy_bytes = 0U;
};

// One receive slot: the caller points `storage` at its buffer before receive(), which fills the rest. A slot left
// empty is lent instead, by a socket that lends (SocketFeatures::lent_receive): receive() points it at the socket's
// own buffer, which holds the datagram, writable, until that socket's next receive().
struct ReceivedDatagram
{
    cont::Span<u8> storage;
    usize size = 0U;        // bytes of the datagram in storage
    bool truncated = false; // the datagram was longer than storage and its rest is lost
    Ecn ecn = Ecn::NotEct;  // its IP header's codepoint; Not-ECT where the socket cannot read it (ecn_reading)
    Endpoint peer;
    // Nonzero for no datagram but the host's report, an ICMP Packet Too Big message (RFC 8899 section 4.6), that one
    // this socket sent to `peer` met a hop carrying this many bytes of UDP payload; storage then holds as much of
    // that datagram's start as the message quoted.
    u32 too_big = 0U;
    // The index of the socket it arrived on in its loop's SocketSet: the loop sets it, a socket leaves it.
    u32 socket = 0U;
    // Nonzero for a train (SocketFeatures::coalesced_receive): storage then holds `size` bytes of datagrams of this
    // many bytes each, the last one shorter or as long, all from `peer` with codepoint `ecn`; a truncated train's last
    // datagram is cut and its whole ones are intact. A coalescing socket sets it on every slot it fills.
    u32 segment = 0U;
};

struct OutgoingDatagram
{
    cont::ConstSpan<u8> bytes;
    Endpoint peer;
    Ecn ecn = Ecn::NotEct; // the codepoint its IP header carries where the socket can mark it (ecn_marking)
};

// What a socket carries beyond the bytes, as it opened. ECN marking may stop later (a host that refuses a marked
// send): the socket then sends unmarked, and the transport's ECN validation fails on the path (quic_ecn.hpp).
struct SocketFeatures
{
    bool ecn_reading = false; // received datagrams report their codepoint
    bool ecn_marking = false; // sent datagrams carry theirs
    // Sent datagrams are never fragmented, by the host or on the path (IPv4's DF bit), and the host's own path MTU
    // estimate refuses none: a datagram larger than the path carries is lost, so a PMTU probe proves its size
    // (RFC 8899 section 3), and one larger than the host's interface fails with MessageTooLarge.
    bool dont_fragment = false;
    // The host's ICMP Packet Too Big messages about sent datagrams reach receive() as reports
    // (ReceivedDatagram::too_big); its other ICMP errors are read and dropped without failing a receive or a send.
    bool too_big_reports = false;
    // Offloads, each the socket's own (a SocketSet reports none of them): a send hands the host a train in one call
    // (segment_train), and a host that later refuses one has it sent again a datagram a call, turning this off for good
    // where it cannot segment at all; a receive may fill a slot with a train (ReceivedDatagram::segment).
    bool segmented_send = false;
    bool coalesced_receive = false;
    // Receives come from buffers the host filled ahead (Linux's io_uring), copied into a slot's storage or lent to
    // an empty slot; also the socket's own. Nonzero, the longest datagram one of them holds whole, whatever the slot.
    bool lent_receive = false;
    u32 receive_bytes = 0U;
    // The buffers the ring holds: UdpOptions::ring_buffers, or fewer where the host refused that many; 0 with none.
    u32 ring_buffers = 0U;
    // Where a ring was asked for and none lends, or one with fewer buffers, the step the host refused first and its
    // error code there (errno, or Windows' error), for an operator to read why: a resource refusal (ENOMEM, EMFILE)
    // is the host's limits.
    RingRefusal ring_refusal = RingRefusal::None;
    int ring_error = 0;
    // Nonzero, the longest datagram a send takes (Windows' Registered I/O slots); a longer one fails with
    // MessageTooLarge.
    u32 send_bytes = 0U;
    // send_stable() may leave a datagram's bytes to the host after it returns (Linux's io_uring zero-copy send),
    // until sends_released() passes it; a host that turns out to take no zero-copy send turns this off for good.
    bool lent_send = false;
};

// What a socket's offloads did since it opened.
struct OffloadCounters
{
    u64 trains_sent = 0U;              // trains of two or more datagrams the host took in one call
    u64 train_datagrams_sent = 0U;     // the datagrams those carried
    u64 trains_refused = 0U;           // trains the host refused, sent again a datagram a call
    u64 trains_received = 0U;          // receive slots holding two or more datagrams
    u64 train_datagrams_received = 0U; // the datagrams those held
    u64 zero_copy_sent = 0U;           // zero-copy messages the host sent from the caller's pages
    u64 zero_copy_copied = 0U;         // zero-copy messages the host copied after all (loopback, a device without it)
    u64 zero_copy_refused = 0U;        // zero-copy messages the host failed, sent again copied
    // Datagrams a send took that the host failed only once they had left the call (Windows' Registered I/O, which
    // reports a send's error as it completes): lost, as a datagram the path drops is. Such a train also counts in
    // trains_refused, and the errors that end segmentation or marking in the call end them here too.
    u64 sends_failed = 0U;
};

// How many datagrams from the first form one train (0 for none): each to the first's peer with its codepoint and each
// as long as the first but the last, which may be shorter and then ends it, at most kMaxTrainDatagrams and together
// at most the peer family's UDP payload. A datagram longer than the first never joins, and a first datagram whose
// next is shorter goes alone, so a path MTU probe neither joins a train nor leads one.
[[nodiscard]] usize segment_train(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept;

// What one batch did: `count` datagrams in order, then the reason it stopped. A receive stops with None when the host
// holds no more; a send stops with None only when every datagram left, and otherwise at the datagram the error names,
// which did not leave (Busy: try it again later; any other error: that datagram alone).
struct BatchResult
{
    usize count = 0U;
    SocketError error = SocketError::None;
};

class DatagramSocket
{
public:
    virtual ~DatagramSocket() = default;

    [[nodiscard]] virtual Endpoint local_endpoint() const noexcept = 0;
    // Fills slots in order with what has arrived, without waiting.
    [[nodiscard]] virtual BatchResult receive(cont::Span<ReceivedDatagram> slots) noexcept = 0;
    // Hands datagrams to the host in order, without waiting; their bytes are the caller's again when it returns.
    [[nodiscard]] virtual BatchResult send(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept = 0;
    // As send(), from a caller that keeps each datagram's bytes unchanged until sends_released() passes it: a socket
    // that lends sends (SocketFeatures::lent_send) may then hand the host the pages without a copy. A socket numbers
    // the datagrams it takes through either call in order from 0, sends_taken() of them so far, and the host no
    // longer reads any numbered below sends_released(); with `wait` that returns only once the host holds none.
    [[nodiscard]] virtual BatchResult send_stable(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept
    {
        return send(datagrams);
    }
    [[nodiscard]] virtual u64 sends_taken() const noexcept { return 0U; }
    [[nodiscard]] virtual u64 sends_released(bool /*wait*/) noexcept { return ~u64{0}; }
    // True once a datagram (or an error receive() reports) is waiting, false when `timeout_ns` passes first. A signal
    // that interrupts the wait restarts it.
    [[nodiscard]] virtual bool wait_readable(u64 timeout_ns) noexcept = 0;
    // None by default: a socket that carries only bytes.
    [[nodiscard]] virtual SocketFeatures features() const noexcept { return {}; }
    // What a SocketSet waits on to wait on several sockets at once: a descriptor poll() takes or, on Windows, an event
    // object that is set while the socket has something to read; kNoWaitHandle (the default) for a socket the host
    // cannot wait on, such as a simulated one.
    [[nodiscard]] virtual u64 wait_handle() const noexcept { return kNoWaitHandle; }
    // A SocketSet's wait found this socket readable, `error` when the host also flagged an error: what
    // wait_readable() notes for the next receive().
    virtual void waited(bool /*error*/) noexcept {}

protected:
    DatagramSocket() = default;
    DatagramSocket(const DatagramSocket&) = default;
    DatagramSocket(DatagramSocket&&) noexcept = default;
    DatagramSocket& operator=(const DatagramSocket&) = default;
    DatagramSocket& operator=(DatagramSocket&&) noexcept = default;
};

// A non-blocking UDP socket of the host. On Windows its port is held exclusively and ICMP port- and TTL-unreachable
// reports do not end receives (SIO_UDP_CONNRESET, SIO_UDP_NETRESET), so one unreachable peer never stops a server;
// on Linux a batch is one recvmmsg or sendmmsg per 32 datagrams. ECN is read and marked per datagram on Linux
// (IP_TOS and IPV6_TCLASS control messages) and Windows (IP_ECN and IPV6_ECN through WSARecvMsg and WSASendMsg),
// each enabled where the host accepts it and reported by features(). Datagrams leave unfragmented on both
// (IP_MTU_DISCOVER and IPV6_MTU_DISCOVER at IP_PMTUDISC_PROBE, or Windows' IP_DONTFRAGMENT and IPV6_DONTFRAG where it
// refuses those), so the transport's PMTU search sees the path. On Linux the host queues each ICMP error about a sent
// datagram (IP_RECVERR, IPV6_RECVERR): receive() reads that queue first once a wait, a receive or a send has seen a
// report, at most 64 reports a call so a flood never starves the datagrams, hands on Packet Too Big and drops the
// rest; a send the host fails with an earlier datagram's report goes again once. Windows keeps its ICMP reports to
// itself. On Linux a send hands each train (segment_train) to the host in one message with UDP_SEGMENT, its datagrams
// one iovec each where they lie, where the host takes the option; a train it refuses for any reason but back-pressure
// goes again a datagram a message, so each meets its own error, and EIO (a device or route that cannot segment)
// also turns segmentation off for good, as features() then reports. Windows does the same through USO: one WSASendMsg
// per train, one WSABUF per datagram, with a UDP_SEND_MSG_SIZE control message, where the option reads back; a train
// it refuses goes again a datagram a call, and WSAEOPNOTSUPP (or WSAEINVAL for an unmarked train) turns segmentation
// off. With coalesce_receive a socket takes UDP_GRO on Linux or URO (UDP_RECV_MAX_COALESCED_SIZE) on Windows and
// reports each train's datagram size. On Linux 6.0 and later (not Android, whose sandbox forbids it) a socket
// receives through io_uring where the host allows it: one multishot recvmsg the host keeps armed fills a ring of the
// socket's buffers ahead of receive(), which reads the completions without a call while they wait, copies each into
// its slot or lends it to an empty one, and reads the ICMP reports its ended receives name as above; the socket then
// waits on the ring's descriptor. The host runs a receive's completions as the thread that armed it, so a receive on
// another thread moves the ring there, and the old receive's end may wake the next wait once with nothing to read.
// Anything the host refuses keeps recvmmsg, as features() reports. With zero_copy_bytes set, Linux 6.3 and later send
// each message send_stable() hands over of that size, and of no more page fragments than the host's buffer holds,
// through io_uring's SENDMSG_ZC on a ring of its own (one linked chain a send, MSG_DONTWAIT): the host reads the
// caller's pages until sends_released() passes them. A message the host fails zero-copy goes again copied; eight
// copy reports in a row (loopback) turn it off until a probe every 1024 sends; close() waits until the host holds no
// page. On Windows a socket goes through Registered I/O where the host has it (registered_io): its receives wait posted
// in registered buffers the host fills, read back without a call while they wait and copied or lent as io_uring's are,
// so a datagram that arrives while every buffer is out is lost, as one past a full receive buffer is; its sends are
// copied into registered slots (a train end to end in one of eight that hold kMaxTrainBytes, segmented through USO),
// deferred, and started with one commit a call. Such a socket takes no other Winsock call, so a send's error arrives as
// it completes, counted in sends_failed, except Busy while every slot is in flight and MessageTooLarge past a slot, and
// a CE mark, which Windows refuses, leaves unmarked at once. Every Windows socket waits on an event (its completion
// queue's, or one WSAEventSelect sets on reads, which may wake a wait once with nothing left to read), so a SocketSet
// waits on any mix of them in one host wait. macOS carries bytes alone until crd-net's port there (MAC.1).
class UdpSocket final : public DatagramSocket
{
public:
    UdpSocket() noexcept = default;
    ~UdpSocket() override;

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // An open, bound socket, or a closed one and the reason.
    [[nodiscard]] static UdpSocket open(const UdpOptions& options, SocketError& error) noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

    [[nodiscard]] Endpoint local_endpoint() const noexcept override;
    [[nodiscard]] BatchResult receive(cont::Span<ReceivedDatagram> slots) noexcept override;
    [[nodiscard]] BatchResult send(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept override;
    [[nodiscard]] BatchResult send_stable(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept override;
    [[nodiscard]] u64 sends_taken() const noexcept override { return m_sends_taken; }
    [[nodiscard]] u64 sends_released(bool wait) noexcept override;
    [[nodiscard]] bool wait_readable(u64 timeout_ns) noexcept override;
    [[nodiscard]] SocketFeatures features() const noexcept override { return m_features; }
    // The ring's descriptor where it receives; on Windows an event object (wait_handle's note).
    [[nodiscard]] u64 wait_handle() const noexcept override;
    void waited(bool error) noexcept override;
    [[nodiscard]] const OffloadCounters& offload_counters() const noexcept { return m_offloads; }

private:
    static constexpr u64 kNoHandle = kNoWaitHandle;

    // send() and, with `stable`, send_stable().
    [[nodiscard]] BatchResult transmit(cont::ConstSpan<OutgoingDatagram> datagrams, bool stable) noexcept;
    // `datagrams` handed to the host in one message, a train when more than one: counted as sent.
    void sent_train(usize datagrams, BatchResult& result) noexcept;

    u64 m_handle = kNoHandle; // the host's descriptor or SOCKET
    Endpoint m_local;
    bool m_dual_stack = false;
    SocketFeatures m_features;
    OffloadCounters m_offloads;
    u64 m_sends_taken = 0U;
#if CRD_OS_WINDOWS
    // The Registered I/O paths of receive() and send().
    void receive_rio(cont::Span<ReceivedDatagram> slots, BatchResult& result) noexcept;
    [[nodiscard]] BatchResult transmit_rio(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept;

    void (*m_receive_message)() = nullptr; // WSARecvMsg, which Winsock hands out per provider, as a generic pointer
    UdpRio* m_rio = nullptr;               // the Registered I/O queues and buffers; null for WSARecvMsg and WSASendMsg
    u64 m_read_event = 0U;                 // a plain socket's event, which WSAEventSelect sets once it is readable
#elif CRD_OS_LINUX
    // The ring's receive into `slots` from `count` on (receive's io_uring path).
    void receive_ring(cont::Span<ReceivedDatagram> slots, BatchResult& result) noexcept;

    bool m_reports_waiting = false; // the error queue may hold reports: the next receive() reads them first
    UdpRing* m_ring = nullptr;      // the io_uring receive, in pages of its own; null for recvmmsg
    UdpSendRing* m_send_ring = nullptr; // the io_uring zero-copy send; null for sendmmsg alone
    u32 m_zero_copy_bytes = 0U;
#endif
};

// The sockets one I/O loop and its host serve together, by index (RFC 9000 section 9: a client's own paths each leave
// from a socket of their own). Socket 0 is the one connections open on; a datagram names the socket it arrived on
// (ReceivedDatagram::socket) and a connection the one each of its datagrams leaves by (QuicPath::socket). The set is
// fixed when made, and its sockets outlive everything holding it.
class SocketSet
{
public:
    // A set of one: what a host of one socket passes.
    SocketSet(DatagramSocket& socket) noexcept; // NOLINT(google-explicit-constructor): every one-socket host
    // `sockets` in order, 1 to kMaxSetSockets of them and none null; otherwise an empty set, which is not ready.
    explicit SocketSet(cont::ConstSpan<DatagramSocket*> sockets) noexcept;

    [[nodiscard]] bool ready() const noexcept { return m_count != 0U; }
    [[nodiscard]] u32 size() const noexcept { return m_count; }
    [[nodiscard]] DatagramSocket& operator[](u32 index) const noexcept { return *m_sockets[index]; }
    // What every socket of the set carries: a feature only where each offers it, as a connection may move among them.
    [[nodiscard]] SocketFeatures features() const noexcept;
    // True once a socket of the set has a datagram (or an error receive() reports) waiting, false when `timeout_ns`
    // passes first. One socket waits alone; sockets that all have a wait_handle() wait in one host wait. A set holding
    // any other socket asks each without waiting, then the first for at most a millisecond and the rest again, and
    // returns false after that slice: its caller turns and waits again.
    [[nodiscard]] bool wait_readable(u64 timeout_ns) const noexcept;

private:
    DatagramSocket* m_sockets[kMaxSetSockets] = {};
    u32 m_count = 0U;
};

} // namespace crd::net
