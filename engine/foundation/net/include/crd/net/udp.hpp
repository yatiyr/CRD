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

struct UdpOptions
{
    Endpoint bind;                 // port 0 takes an ephemeral port; the address picks the socket's family
    bool dual_stack = true;        // an IPv6 socket also carries IPv4, its peers reported as IPv4 endpoints
    u32 receive_buffer_bytes = 0U; // 0 keeps the host's default
    u32 send_buffer_bytes = 0U;    // 0 keeps the host's default
};

// One receive slot: the caller points `storage` at its buffer before receive(), which fills the rest.
struct ReceivedDatagram
{
    cont::Span<u8> storage;
    usize size = 0U;        // bytes of the datagram in storage
    bool truncated = false; // the datagram was longer than storage and its rest is lost
    Ecn ecn = Ecn::NotEct;  // its IP header's codepoint; Not-ECT where the socket cannot read it (ecn_reading)
    Endpoint peer;
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
};

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
    // Hands datagrams to the host in order, without waiting.
    [[nodiscard]] virtual BatchResult send(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept = 0;
    // True once a datagram (or an error receive() reports) is waiting, false when `timeout_ns` passes first. A signal
    // that interrupts the wait restarts it.
    [[nodiscard]] virtual bool wait_readable(u64 timeout_ns) noexcept = 0;
    // None by default: a socket that carries only bytes.
    [[nodiscard]] virtual SocketFeatures features() const noexcept { return {}; }

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
// each enabled where the host accepts it and reported by features(); macOS carries bytes alone until crd-net's port
// there (MAC.1). Other path behaviour (PMTU, pacing, offloads) is NET-PATH's.
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
    [[nodiscard]] bool wait_readable(u64 timeout_ns) noexcept override;
    [[nodiscard]] SocketFeatures features() const noexcept override { return m_features; }

private:
    static constexpr u64 kNoHandle = ~u64{0};

    u64 m_handle = kNoHandle; // the host's descriptor or SOCKET
    Endpoint m_local;
    bool m_dual_stack = false;
    SocketFeatures m_features;
#if CRD_OS_WINDOWS
    void (*m_receive_message)() = nullptr; // WSARecvMsg, which Winsock hands out per provider, as a generic pointer
#endif
};

} // namespace crd::net
