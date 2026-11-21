// crd-net -- datagram sockets on the host's loopback (4.2a; docs/design/network-foundation.md#sockets), the ECN
// codepoint each datagram carries (NET-PATH; #ecn), trains sent in one host call and received whole (#offloads), and
// on Linux a router's Packet Too Big reports (#pmtu) and the host refusing trains through private network namespaces
// ([.netns], router_namespace_fixture.hpp).

#include <crd/net/udp.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstring>

#if CRD_OS_LINUX
#include "router_namespace_fixture.hpp"
#endif

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::BatchResult;
using crd::net::Ecn;
using crd::net::kMaxTrainBytes;
using crd::net::kMaxTrainDatagrams;
using crd::net::OffloadCounters;
using crd::net::Endpoint;
using crd::net::IpAddress;
using crd::net::OutgoingDatagram;
using crd::net::ReceivedDatagram;
using crd::net::segment_train;
using crd::net::SocketError;
using crd::net::SocketFeatures;
using crd::net::UdpOptions;
using crd::net::UdpSocket;

constexpr u64 kPatience = 2'000'000'000U; // how long a loopback datagram may take to arrive

UdpSocket open_on(const IpAddress& address, bool dual_stack = true, u16 port = 0U)
{
    UdpOptions options;
    options.bind = Endpoint{address, port};
    options.dual_stack = dual_stack;
    options.receive_buffer_bytes = 1U << 21U; // room for every test's burst
    // and for the largest datagram: macOS sends nothing above its send buffer, 9216 bytes by default
    options.send_buffer_bytes = 1U << 21U;
    SocketError error = SocketError::Failed;
    UdpSocket socket = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    REQUIRE(socket.is_open());
    return socket;
}

// An IPv6 loopback socket, or false when the host has no IPv6.
bool open_v6(UdpSocket& out, bool dual_stack)
{
    UdpOptions options;
    options.bind = Endpoint{IpAddress::v6_loopback(), 0U};
    options.dual_stack = dual_stack;
    SocketError error = SocketError::Failed;
    out = UdpSocket::open(options, error);
    if (error == SocketError::FamilyUnsupported || error == SocketError::AddressUnavailable)
    {
        return false;
    }
    REQUIRE(error == SocketError::None);
    return true;
}

// The loopback endpoint of a socket bound to an unspecified address.
Endpoint reach(const UdpSocket& socket, const IpAddress& loopback)
{
    return Endpoint{loopback, socket.local_endpoint().port};
}

// Fills a datagram with its index and a pattern of its own.
void pattern(Span<u8> bytes, u32 index)
{
    for (usize at = 0U; at < bytes.size(); ++at)
    {
        bytes[at] = static_cast<u8>(index * 31U + at * 7U);
    }
    if (bytes.size() >= 4U)
    {
        for (usize at = 0U; at < 4U; ++at)
        {
            bytes[at] = static_cast<u8>(index >> (8U * at));
        }
    }
}

bool matches(ConstSpan<u8> bytes, u32 index)
{
    u8 want[2048];
    REQUIRE(bytes.size() <= sizeof(want));
    pattern(Span<u8>(want, bytes.size()), index);
    for (usize at = 0U; at < bytes.size(); ++at)
    {
        if (bytes[at] != want[at])
        {
            return false;
        }
    }
    return true;
}

// Receives into `slots` until `want` datagrams arrived or patience ran out.
usize receive_all(UdpSocket& socket, Span<ReceivedDatagram> slots, usize want)
{
    usize got = 0U;
    while (got < want && socket.wait_readable(kPatience))
    {
        const BatchResult result = socket.receive(slots.subspan(got));
        REQUIRE(result.error == SocketError::None);
        got += result.count;
    }
    return got;
}

struct Slots
{
    static constexpr usize kCount = 160U;
    static constexpr usize kBytes = 2048U;
    u8 storage[kCount][kBytes] = {};
    ReceivedDatagram slots[kCount];

    explicit Slots(usize bytes = kBytes)
    {
        for (usize index = 0U; index < kCount; ++index)
        {
            slots[index].storage = Span<u8>(storage[index], bytes);
        }
    }
};

// What arrives when CE is sent: Windows refuses a sender's CE, a router's mark, and sends that datagram unmarked.
#if CRD_OS_WINDOWS
constexpr Ecn kCeArrives = Ecn::NotEct;
#else
constexpr Ecn kCeArrives = Ecn::Ce;
#endif

// Each codepoint sent, and what arrives.
constexpr Ecn kCodepoints[4] = {Ecn::NotEct, Ecn::Ect1, Ecn::Ect0, Ecn::Ce};
constexpr Ecn kArrives[4] = {Ecn::NotEct, Ecn::Ect1, Ecn::Ect0, kCeArrives};

// Sends one datagram with each codepoint from `from` to `to` and checks each arrives with its own: true when every
// one did. A host that cannot carry codepoints must say so in features() instead.
bool carries_every_codepoint(UdpSocket& from, UdpSocket& to, const Endpoint& to_endpoint, const Endpoint& from_seen)
{
    static u8 payload[4][48];
    OutgoingDatagram datagrams[4];
    for (u32 index = 0U; index < 4U; ++index)
    {
        pattern(payload[index], index);
        datagrams[index] =
            OutgoingDatagram{ConstSpan<u8>(payload[index], sizeof(payload[index])), to_endpoint, kCodepoints[index]};
    }
    REQUIRE(from.send(datagrams).count == 4U);
    static Slots slots;
    REQUIRE(receive_all(to, Span<ReceivedDatagram>(slots.slots, 4U), 4U) == 4U);
    bool all = true;
    for (u32 index = 0U; index < 4U; ++index)
    {
        const ReceivedDatagram& slot = slots.slots[index];
        CHECK(slot.peer == from_seen);
        const u32 sent = slot.storage[0]; // the datagram's index (pattern)
        REQUIRE(sent < 4U);
        INFO("datagram " << sent);
        CHECK(slot.ecn == kArrives[sent]);
        all = all && slot.ecn == kArrives[sent];
    }
    return all;
}

} // namespace

TEST_CASE("a UDP socket carries each datagram's ECN codepoint over IPv4, IPv6 and dual-stack", "[net][udp][net-path]")
{
    UdpSocket a = open_on(IpAddress::v4_loopback());
    UdpSocket b = open_on(IpAddress::v4_loopback());
    const SocketFeatures features = a.features();
#if CRD_OS_LINUX || CRD_OS_WINDOWS
    REQUIRE(features.ecn_reading);
    REQUIRE(features.ecn_marking);
#else
    // macOS carries bytes alone until crd-net's port there (MAC.1): every datagram arrives Not-ECT.
    CHECK_FALSE(features.ecn_reading);
    CHECK_FALSE(features.ecn_marking);
    return;
#endif
    CHECK(carries_every_codepoint(a, b, b.local_endpoint(), a.local_endpoint()));
    CHECK(carries_every_codepoint(b, a, a.local_endpoint(), b.local_endpoint()));
    CHECK(a.features().ecn_marking); // a refused CE leaves marking on

    UdpSocket v6a;
    UdpSocket v6b;
    if (!open_v6(v6a, false) || !open_v6(v6b, false))
    {
        SKIP("the host has no IPv6 loopback");
    }
    CHECK(carries_every_codepoint(v6a, v6b, v6b.local_endpoint(), v6a.local_endpoint()));

    // A dual-stack socket marks toward an IPv4 peer with IPv4's message and reads IPv4's codepoints.
    UdpOptions options;
    options.bind = Endpoint{IpAddress::v6_any(), 0U};
    SocketError error = SocketError::Failed;
    UdpSocket dual = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    CHECK(dual.features().ecn_reading);
    const Endpoint dual_v4 = reach(dual, IpAddress::v4_loopback());
    CHECK(carries_every_codepoint(a, dual, dual_v4, a.local_endpoint()));
    CHECK(carries_every_codepoint(dual, a, a.local_endpoint(), dual_v4));
    const Endpoint dual_v6 = reach(dual, IpAddress::v6_loopback());
    CHECK(carries_every_codepoint(v6a, dual, dual_v6, v6a.local_endpoint()));
    CHECK(carries_every_codepoint(dual, v6a, v6a.local_endpoint(), dual_v6));
}

TEST_CASE("a UDP socket forbids fragmentation in every family, so PMTU probes see the path", "[net][udp][net-path]")
{
    UdpSocket v4 = open_on(IpAddress::v4_loopback());
    // Packet Too Big reports reach receive() on Linux alone: Windows keeps its ICMP reports to itself.
    CHECK(v4.features().too_big_reports == (CRD_OS_LINUX != 0));
#if CRD_OS_LINUX || CRD_OS_WINDOWS
    CHECK(v4.features().dont_fragment);
#else
    CHECK_FALSE(v4.features().dont_fragment); // macOS until crd-net's port there (MAC.1): QuicHost probes nothing
    return;
#endif
    UdpSocket v6;
    if (!open_v6(v6, false))
    {
        SKIP("the host has no IPv6 loopback");
    }
    CHECK(v6.features().dont_fragment);
    CHECK(v6.features().too_big_reports == (CRD_OS_LINUX != 0));
    UdpOptions options;
    options.bind = Endpoint{IpAddress::v6_any(), 0U};
    SocketError error = SocketError::Failed;
    UdpSocket dual = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    CHECK(dual.features().dont_fragment); // both families' options
    CHECK(dual.features().too_big_reports == (CRD_OS_LINUX != 0));
    // The loopback's MTU carries the largest datagram unfragmented (the send test below sends it whole).
    u8 bytes[1472] = {};
    const Endpoint to = reach(dual, IpAddress::v4_loopback());
    const OutgoingDatagram datagram[] = {{ConstSpan<u8>(bytes, sizeof(bytes)), to}};
    const BatchResult sent = v4.send(ConstSpan<OutgoingDatagram>(datagram, 1U));
    CHECK(sent.error == SocketError::None);
    CHECK(sent.count == 1U);
}

TEST_CASE("a UDP socket binds a loopback port of its own", "[net][udp]")
{
    UdpSocket socket = open_on(IpAddress::v4_loopback());
    CHECK(socket.local_endpoint().address == IpAddress::v4_loopback());
    CHECK(socket.local_endpoint().port != 0U);
    // A mapped bind address is the IPv4 address it carries.
    UdpSocket mapped = open_on(IpAddress::v4_loopback().mapped());
    CHECK(mapped.local_endpoint().address == IpAddress::v4_loopback());

    // The port is the socket's alone: on Windows exclusively, on every host without address reuse.
    UdpOptions again;
    again.bind = socket.local_endpoint();
    SocketError error = SocketError::None;
    const UdpSocket second = UdpSocket::open(again, error);
    CHECK(error == SocketError::AddressInUse);
    CHECK_FALSE(second.is_open());

    // 192.0.2.1 (TEST-NET-1, RFC 5737) is no address of this host.
    UdpOptions foreign;
    foreign.bind = Endpoint{IpAddress::v4(192U, 0U, 2U, 1U), 0U};
    const UdpSocket elsewhere = UdpSocket::open(foreign, error);
    CHECK(error == SocketError::AddressUnavailable);
    CHECK_FALSE(elsewhere.is_open());
}

TEST_CASE("a UDP batch carries every datagram, past one host call's chunk", "[net][udp]")
{
    UdpSocket sender = open_on(IpAddress::v4_loopback());
    UdpSocket receiver = open_on(IpAddress::v4_loopback());
    constexpr u32 sent_count = 150U; // more than four of Linux's 32-datagram chunks
    static u8 payloads[sent_count][1500];
    OutgoingDatagram outgoing[sent_count];
    for (u32 index = 0U; index < sent_count; ++index)
    {
        const usize size = (index * 97U) % 1500U; // zero-length datagrams included
        pattern(Span<u8>(payloads[index], size), index);
        outgoing[index] = OutgoingDatagram{ConstSpan<u8>(payloads[index], size), receiver.local_endpoint()};
    }
    const BatchResult sent = sender.send(outgoing);
    REQUIRE(sent.error == SocketError::None);
    REQUIRE(sent.count == sent_count);

    static Slots slots;
    REQUIRE(receive_all(receiver, slots.slots, sent_count) == sent_count);
    bool seen[sent_count] = {};
    for (u32 index = 0U; index < sent_count; ++index)
    {
        const ReceivedDatagram& datagram = slots.slots[index];
        CHECK(datagram.peer == sender.local_endpoint());
        CHECK_FALSE(datagram.truncated);
        // Zero-length and short datagrams carry no index: they are told apart by their size.
        u32 which = sent_count;
        for (u32 candidate = 0U; candidate < sent_count; ++candidate)
        {
            const usize size = (candidate * 97U) % 1500U;
            if (!seen[candidate] && size == datagram.size &&
                matches(ConstSpan<u8>(datagram.storage.data(), datagram.size), candidate))
            {
                which = candidate;
                break;
            }
        }
        REQUIRE(which != sent_count);
        seen[which] = true;
    }

    // Nothing more is waiting: a receive returns at once, empty and without an error.
    const BatchResult empty = receiver.receive(slots.slots);
    CHECK(empty.count == 0U);
    CHECK(empty.error == SocketError::None);
}

TEST_CASE("a UDP datagram longer than its slot arrives marked truncated", "[net][udp]")
{
    UdpSocket sender = open_on(IpAddress::v4_loopback());
    UdpSocket receiver = open_on(IpAddress::v4_loopback());
    static u8 long_payload[2000];
    static u8 short_payload[50];
    pattern(long_payload, 1U);
    pattern(short_payload, 2U);
    const OutgoingDatagram outgoing[] = {
        {ConstSpan<u8>(long_payload, sizeof(long_payload)), receiver.local_endpoint()},
        {ConstSpan<u8>(short_payload, sizeof(short_payload)), receiver.local_endpoint()},
    };
    REQUIRE(sender.send(outgoing).count == 2U);

    static Slots slots(100U);
    REQUIRE(receive_all(receiver, Span<ReceivedDatagram>(slots.slots, 2U), 2U) == 2U);
    CHECK(slots.slots[0].truncated);
    CHECK(slots.slots[0].size == 100U);
    CHECK(matches(ConstSpan<u8>(slots.slots[0].storage.data(), 100U), 1U));
    // The datagram after it is whole.
    CHECK_FALSE(slots.slots[1].truncated);
    CHECK(slots.slots[1].size == sizeof(short_payload));
    CHECK(matches(ConstSpan<u8>(slots.slots[1].storage.data(), sizeof(short_payload)), 2U));
}

TEST_CASE("a UDP send stops at the datagram that cannot leave and names why", "[net][udp]")
{
    UdpSocket sender = open_on(IpAddress::v4_loopback());
    UdpSocket receiver = open_on(IpAddress::v4_loopback());
    static u8 largest[crd::net::kMaxUdpPayloadV6 + 1U];
    const Endpoint to = receiver.local_endpoint();
    const Endpoint v6_peer{IpAddress::v6_loopback(), to.port};

    // An IPv4 peer takes at most 65507 bytes, and an IPv4 socket reaches no IPv6 peer: refused before the host.
    const OutgoingDatagram mixed[] = {
        {ConstSpan<u8>(largest, 8U), to},
        {ConstSpan<u8>(largest, crd::net::kMaxUdpPayloadV4 + 1U), to},
        {ConstSpan<u8>(largest, 8U), to},
    };
    const BatchResult first = sender.send(mixed);
    CHECK(first.count == 1U);
    CHECK(first.error == SocketError::MessageTooLarge);
    const BatchResult rest = sender.send(ConstSpan<OutgoingDatagram>(mixed).subspan(2U));
    CHECK(rest.count == 1U);
    CHECK(rest.error == SocketError::None);
    const OutgoingDatagram crossing[] = {{ConstSpan<u8>(largest, 8U), v6_peer}};
    const BatchResult crossed = sender.send(crossing);
    CHECK(crossed.count == 0U);
    CHECK(crossed.error == SocketError::FamilyMismatch);

    // The largest IPv4 datagram leaves and arrives whole.
    pattern(Span<u8>(largest, crd::net::kMaxUdpPayloadV4), 9U);
    const OutgoingDatagram biggest[] = {{ConstSpan<u8>(largest, crd::net::kMaxUdpPayloadV4), to}};
    REQUIRE(sender.send(biggest).count == 1U);
    static u8 big_slot[crd::net::kMaxUdpPayloadV6];
    ReceivedDatagram slots[3];
    for (ReceivedDatagram& slot : slots)
    {
        slot.storage = Span<u8>(big_slot, sizeof(big_slot));
    }
    // The two small datagrams first, then the large one, into the same buffer: the last one fills it.
    usize got = 0U;
    while (got < 3U && receiver.wait_readable(kPatience))
    {
        const BatchResult result = receiver.receive(Span<ReceivedDatagram>(slots).subspan(got, 1U));
        REQUIRE(result.error == SocketError::None);
        got += result.count;
    }
    REQUIRE(got == 3U);
    CHECK(slots[2].size == crd::net::kMaxUdpPayloadV4);
    CHECK_FALSE(slots[2].truncated);
    bool whole = true;
    for (usize at = 0U; at < crd::net::kMaxUdpPayloadV4; ++at)
    {
        const u8 want = at < 4U ? static_cast<u8>(9U >> (8U * at)) : static_cast<u8>(9U * 31U + at * 7U);
        whole = whole && big_slot[at] == want;
    }
    CHECK(whole);
}

TEST_CASE("a dual-stack UDP socket speaks IPv4 to IPv4 peers as IPv4 endpoints", "[net][udp]")
{
    UdpOptions options;
    options.bind = Endpoint{IpAddress::v6_any(), 0U};
    SocketError error = SocketError::Failed;
    UdpSocket dual = UdpSocket::open(options, error);
    if (error == SocketError::FamilyUnsupported)
    {
        SKIP("the host has no IPv6");
    }
    REQUIRE(error == SocketError::None);
    UdpSocket v4 = open_on(IpAddress::v4_loopback());
    static u8 payload[64];
    pattern(payload, 5U);
    const Endpoint dual_v4 = reach(dual, IpAddress::v4_loopback());
    const OutgoingDatagram to_dual[] = {{ConstSpan<u8>(payload, sizeof(payload)), dual_v4}};
    REQUIRE(v4.send(to_dual).count == 1U);
    static Slots slots;
    REQUIRE(receive_all(dual, Span<ReceivedDatagram>(slots.slots, 1U), 1U) == 1U);
    // The peer is the IPv4 endpoint itself, never ::ffff:127.0.0.1.
    CHECK(slots.slots[0].peer == v4.local_endpoint());
    CHECK(slots.slots[0].peer.address.family == crd::net::AddressFamily::V4);

    // And the answer goes back to that IPv4 endpoint.
    const OutgoingDatagram answer[] = {{ConstSpan<u8>(payload, 16U), slots.slots[0].peer}};
    REQUIRE(dual.send(answer).count == 1U);
    REQUIRE(receive_all(v4, Span<ReceivedDatagram>(slots.slots, 1U), 1U) == 1U);
    CHECK(slots.slots[0].peer == dual_v4);
    CHECK(slots.slots[0].size == 16U);
}

TEST_CASE("an IPv6-only UDP socket refuses IPv4 peers and carries IPv6", "[net][udp]")
{
    UdpSocket only;
    UdpSocket other;
    if (!open_v6(only, false) || !open_v6(other, false))
    {
        SKIP("the host has no IPv6 loopback");
    }
    static u8 payload[32];
    pattern(payload, 6U);
    const Endpoint v4_peer{IpAddress::v4_loopback(), 9U};
    const OutgoingDatagram to_v4[] = {{ConstSpan<u8>(payload, sizeof(payload)), v4_peer}};
    const BatchResult refused = only.send(to_v4);
    CHECK(refused.count == 0U);
    CHECK(refused.error == SocketError::FamilyMismatch);

    const OutgoingDatagram to_other[] = {{ConstSpan<u8>(payload, sizeof(payload)), other.local_endpoint()}};
    REQUIRE(only.send(to_other).count == 1U);
    static Slots slots;
    REQUIRE(receive_all(other, Span<ReceivedDatagram>(slots.slots, 1U), 1U) == 1U);
    CHECK(slots.slots[0].peer == only.local_endpoint());
    CHECK(slots.slots[0].peer.address == IpAddress::v6_loopback());
    CHECK(matches(ConstSpan<u8>(slots.slots[0].storage.data(), slots.slots[0].size), 6U));
}

TEST_CASE("an unreachable peer's ICMP reports fail no send or receive and leave no wakeup", "[net][udp]")
{
    UdpSocket server = open_on(IpAddress::v4_loopback());
    UdpSocket client = open_on(IpAddress::v4_loopback());
    // A port nobody holds any more: the host answers with ICMP port unreachable.
    Endpoint gone;
    {
        const UdpSocket closed = open_on(IpAddress::v4_loopback());
        gone = closed.local_endpoint();
    }
    static u8 payload[24];
    pattern(payload, 7U);
    const OutgoingDatagram to_gone[] = {{ConstSpan<u8>(payload, sizeof(payload)), gone}};
    // Linux returns a queued report's error to the next call: each send after the first meets one and goes again.
    for (u32 round = 0U; round < 4U; ++round)
    {
        const BatchResult sent = server.send(to_gone);
        REQUIRE(sent.error == SocketError::None);
        REQUIRE(sent.count == 1U);
        static_cast<void>(server.wait_readable(50'000'000U)); // the report's arrival
    }
    const OutgoingDatagram to_server[] = {{ConstSpan<u8>(payload, sizeof(payload)), server.local_endpoint()}};
    REQUIRE(client.send(to_server).count == 1U);
    static Slots slots;
    CHECK(receive_all(server, Span<ReceivedDatagram>(slots.slots, 1U), 1U) == 1U);
    CHECK(slots.slots[0].peer == client.local_endpoint());
    CHECK(slots.slots[0].too_big == 0U); // a port unreachable report is no Packet Too Big
    const BatchResult after = server.receive(slots.slots);
    CHECK(after.count == 0U);
    CHECK(after.error == SocketError::None);
    // Every report was read and dropped: nothing wakes a wait, and a report alone is no receive.
    REQUIRE(server.send(to_gone).count == 1U);
    static_cast<void>(server.wait_readable(50'000'000U));
    const BatchResult report_only = server.receive(slots.slots);
    CHECK(report_only.count == 0U);
    CHECK(report_only.error == SocketError::None);
    CHECK_FALSE(server.wait_readable(0U));
}

TEST_CASE("a UDP socket waits until a datagram arrives or its time passes", "[net][udp]")
{
    UdpSocket sender = open_on(IpAddress::v4_loopback());
    UdpSocket receiver = open_on(IpAddress::v4_loopback());
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(receiver.wait_readable(30'000'000U));
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(25));
    CHECK_FALSE(receiver.wait_readable(0U));

    static u8 payload[8];
    const OutgoingDatagram outgoing[] = {{ConstSpan<u8>(payload, sizeof(payload)), receiver.local_endpoint()}};
    REQUIRE(sender.send(outgoing).count == 1U);
    CHECK(receiver.wait_readable(kPatience));
    CHECK(receiver.wait_readable(0U)); // still waiting: nothing has read it
}

TEST_CASE("a closed or moved-from UDP socket refuses every call", "[net][udp]")
{
    UdpSocket socket = open_on(IpAddress::v4_loopback());
    const Endpoint was = socket.local_endpoint();
    UdpSocket moved(static_cast<UdpSocket&&>(socket));
    CHECK(moved.is_open());
    CHECK(moved.local_endpoint() == was);
    CHECK_FALSE(socket.is_open()); // NOLINT(bugprone-use-after-move): the moved-from state is the subject

    static u8 payload[4];
    const OutgoingDatagram outgoing[] = {{ConstSpan<u8>(payload, sizeof(payload)), was}};
    ReceivedDatagram slot;
    slot.storage = Span<u8>(payload, sizeof(payload));
    CHECK(socket.send(outgoing).error == SocketError::Closed);
    CHECK(socket.receive(Span<ReceivedDatagram>(&slot, 1U)).error == SocketError::Closed);
    CHECK_FALSE(socket.wait_readable(0U));

    // Assigning over an open socket closes it first: its port is free again.
    moved = open_on(IpAddress::v4_loopback());
    UdpSocket reused = open_on(IpAddress::v4_loopback(), true, was.port);
    CHECK(reused.local_endpoint() == was);
    moved.close();
    CHECK_FALSE(moved.is_open());
    CHECK(moved.local_endpoint() == Endpoint{});
    moved.close(); // twice is harmless
}

namespace
{
// The rule segment_train follows, read directly: whether the first `count` datagrams form one train.
bool forms_train(ConstSpan<OutgoingDatagram> datagrams, usize count)
{
    if (count == 0U || count > datagrams.size() || count > kMaxTrainDatagrams)
    {
        return false;
    }
    const OutgoingDatagram& first = datagrams[0];
    const usize limit = first.peer.address.unmapped().family == crd::net::AddressFamily::V4
                            ? crd::net::kMaxUdpPayloadV4
                            : crd::net::kMaxUdpPayloadV6;
    usize total = 0U;
    for (usize index = 0U; index < count; ++index)
    {
        const OutgoingDatagram& one = datagrams[index];
        total += one.bytes.size();
        const bool inner = index + 1U < count;
        if (one.peer != first.peer || one.ecn != first.ecn || (index != 0U && one.bytes.empty()) ||
            (inner && one.bytes.size() != first.bytes.size()) || one.bytes.size() > first.bytes.size())
        {
            return false;
        }
    }
    // A datagram followed by a shorter one goes alone.
    return total <= limit && (count == 1U || datagrams[1].bytes.size() == first.bytes.size());
}

// One planned datagram of a burst.
struct Planned
{
    usize size = 0U;
    Ecn ecn = Ecn::NotEct;
};

// A burst that forms trains and breaks them: an uneven tail, a probe-sized datagram, codepoint changes, a run past
// kMaxTrainDatagrams and a lone datagram followed by a shorter one.
constexpr usize kBurst = 112U;
Planned burst_plan(usize index)
{
    if (index < 20U)
    {
        return {1200U, Ecn::Ect0};
    }
    if (index == 20U)
    {
        return {700U, Ecn::Ect0}; // the train's shorter last
    }
    if (index == 21U)
    {
        return {1400U, Ecn::Ect0}; // a probe: alone
    }
    if (index < 32U)
    {
        return {1200U, Ecn::Ect1};
    }
    if (index < 37U)
    {
        return {1200U, Ecn::NotEct};
    }
    if (index == 37U)
    {
        return {900U, Ecn::Ect1}; // a datagram followed by a shorter one goes alone
    }
    if (index == 38U)
    {
        return {100U, Ecn::Ect1};
    }
    return {1000U, Ecn::Ect0}; // 73 of them: a full train and the rest
}

// Sends the burst from `from` to `to_endpoint`, each datagram its index in its pattern, until all left.
void send_burst(UdpSocket& from, const Endpoint& to_endpoint)
{
    static u8 payload[kBurst][1400];
    OutgoingDatagram datagrams[kBurst];
    for (usize index = 0U; index < kBurst; ++index)
    {
        const Planned plan = burst_plan(index);
        pattern(Span<u8>(payload[index], plan.size), static_cast<u32>(index));
        datagrams[index] = OutgoingDatagram{ConstSpan<u8>(payload[index], plan.size), to_endpoint, plan.ecn};
    }
    usize sent = 0U;
    for (u32 attempt = 0U; sent < kBurst && attempt < 1000U; ++attempt)
    {
        const BatchResult result = from.send(ConstSpan<OutgoingDatagram>(datagrams).subspan(sent));
        REQUIRE((result.error == SocketError::None || result.error == SocketError::Busy));
        sent += result.count;
    }
    REQUIRE(sent == kBurst);
}

// The trains and their datagrams segment_train makes of the burst toward `peer`.
OffloadCounters burst_trains(const Endpoint& peer)
{
    static u8 payload[1400];
    OutgoingDatagram datagrams[kBurst];
    for (usize index = 0U; index < kBurst; ++index)
    {
        const Planned plan = burst_plan(index);
        datagrams[index] = OutgoingDatagram{ConstSpan<u8>(payload, plan.size), peer, plan.ecn};
    }
    OffloadCounters trains;
    for (usize at = 0U; at < kBurst;)
    {
        const usize count = segment_train(ConstSpan<OutgoingDatagram>(datagrams).subspan(at));
        if (count > 1U)
        {
            ++trains.trains_sent;
            trains.train_datagrams_sent += count;
        }
        at += count;
    }
    return trains;
}

// Checks datagram `expected` of the burst arrived whole in `bytes` with its codepoint, or Not-ECT on a socket that
// reads none (macOS until MAC.1).
void check_burst_datagram(ConstSpan<u8> bytes, Ecn ecn, usize expected, bool ecn_reading)
{
    INFO("datagram " << expected);
    const Planned plan = burst_plan(expected);
    REQUIRE(bytes.size() == plan.size);
    CHECK(matches(bytes, static_cast<u32>(expected)));
    CHECK(ecn == (ecn_reading ? plan.ecn : Ecn::NotEct));
}

// Receives the burst on a socket that does not coalesce and checks every datagram arrived whole, in order.
void receive_burst(UdpSocket& to, const Endpoint& from_seen)
{
    static Slots slots;
    REQUIRE(receive_all(to, Span<ReceivedDatagram>(slots.slots, kBurst), kBurst) == kBurst);
    for (usize index = 0U; index < kBurst; ++index)
    {
        const ReceivedDatagram& slot = slots.slots[index];
        CHECK(slot.peer == from_seen);
        CHECK(slot.segment == 0U);
        CHECK_FALSE(slot.truncated);
        check_burst_datagram(slot.storage.first(slot.size), slot.ecn, index, to.features().ecn_reading);
    }
}

// Receives the burst on a coalescing socket, splitting each train as IoLoop does, and checks every datagram arrived
// whole, in order: the trains it received.
u64 receive_burst_coalesced(UdpSocket& to, const Endpoint& from_seen)
{
    static u8 storage[8][kMaxTrainBytes];
    ReceivedDatagram slots[8];
    for (usize index = 0U; index < 8U; ++index)
    {
        slots[index].storage = Span<u8>(storage[index], kMaxTrainBytes);
    }
    usize next = 0U;
    u64 trains = 0U;
    while (next < kBurst && to.wait_readable(kPatience))
    {
        const BatchResult result = to.receive(slots);
        REQUIRE(result.error == SocketError::None);
        for (usize slot_index = 0U; slot_index < result.count; ++slot_index)
        {
            const ReceivedDatagram& slot = slots[slot_index];
            CHECK(slot.peer == from_seen);
            CHECK_FALSE(slot.truncated);
            const usize segment = (slot.segment == 0U) ? slot.size : slot.segment;
            trains += (slot.segment != 0U) ? 1U : 0U;
            for (usize at = 0U; at < slot.size && next < kBurst; at += segment, ++next)
            {
                const usize size = (slot.size - at < segment) ? slot.size - at : segment;
                check_burst_datagram(slot.storage.subspan(at, size), slot.ecn, next, to.features().ecn_reading);
            }
        }
    }
    CHECK(next == kBurst);
    return trains;
}

// Opens a loopback socket of `address`'s family with the offloads chosen; false when the host lacks the family.
bool open_offloaded(UdpSocket& out, const IpAddress& address, bool segment_send, bool coalesce_receive)
{
    UdpOptions options;
    options.bind = Endpoint{address, 0U};
    options.receive_buffer_bytes = 1U << 22U;
    options.send_buffer_bytes = 1U << 22U;
    options.segment_send = segment_send;
    options.coalesce_receive = coalesce_receive;
    SocketError error = SocketError::Failed;
    out = UdpSocket::open(options, error);
    if (error == SocketError::FamilyUnsupported || error == SocketError::AddressUnavailable)
    {
        return false;
    }
    REQUIRE(error == SocketError::None);
    return true;
}
} // namespace

TEST_CASE("segment_train takes exactly the longest run the rule allows, against a direct reading of it",
          "[net][udp][net-path]")
{
    static u8 bytes[crd::net::kMaxUdpPayloadV6];
    const Endpoint peers[] = {
        {IpAddress::v4_loopback(), 1U}, {IpAddress::v4_loopback(), 2U}, {IpAddress::v6_loopback(), 1U}};
    const usize bases[] = {0U, 1U, 1200U, 1472U, 1000U, 2000U, 30000U, 32763U};
    constexpr Ecn codes[] = {Ecn::Ect0, Ecn::Ect1, Ecn::NotEct, Ecn::Ce};
    u64 state = 0x9E3779B97F4A7C15ULL;
    const auto next = [&state]()
    {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };
    CHECK(segment_train({}) == 0U);
    for (u32 round = 0U; round < 20000U; ++round)
    {
        OutgoingDatagram datagrams[80];
        const usize count = next() % 81U;
        const usize base = bases[next() % 8U];
        const Endpoint peer = peers[(next() % 4U == 0U) ? 2U : 0U];
        for (usize index = 0U; index < count; ++index)
        {
            const u64 pick = next() % 16U;
            usize size = base;
            if (pick == 12U)
            {
                size = (base > 0U) ? base - 1U : 0U;
            }
            else if (pick == 13U)
            {
                size = base + 1U;
            }
            else if (pick == 14U)
            {
                size = next() % 3000U;
            }
            else if (pick == 15U)
            {
                size = 0U;
            }
            const bool stray = next() % 40U == 0U;
            datagrams[index] = OutgoingDatagram{ConstSpan<u8>(bytes, size), stray ? peers[1] : peer,
                                                codes[(next() % 30U == 0U) ? 1U + (next() % 3U) : 0U]};
        }
        const ConstSpan<OutgoingDatagram> run(datagrams, count);
        usize want = 0U;
        for (usize length = 1U; length <= count; ++length)
        {
            want = forms_train(run, length) ? length : want;
        }
        INFO("round " << round);
        REQUIRE(segment_train(run) == want);
    }
}

TEST_CASE("a burst leaves in trains where the host segments them and arrives as the same datagrams as without",
          "[net][udp][net-path]")
{
    const IpAddress families[] = {IpAddress::v4_loopback(), IpAddress::v6_loopback()};
    for (const IpAddress& family : families)
    {
        for (const bool segment : {true, false})
        {
            UdpSocket from;
            UdpSocket to;
            if (!open_offloaded(from, family, segment, false) || !open_offloaded(to, family, true, false))
            {
                SKIP("the host has no IPv6 loopback");
            }
            INFO("IPv" << (family.family == crd::net::AddressFamily::V4 ? 4 : 6) << ", segmenting " << segment);
#if CRD_OS_LINUX || CRD_OS_WINDOWS
            REQUIRE(from.features().segmented_send == segment);
#else
            CHECK_FALSE(from.features().segmented_send); // macOS's offloads are not taken yet
#endif
            CHECK_FALSE(to.features().coalesced_receive); // only on request
            send_burst(from, to.local_endpoint());
            receive_burst(to, from.local_endpoint());
            const OffloadCounters& sent = from.offload_counters();
            const OffloadCounters planned = burst_trains(to.local_endpoint());
            const bool trains = from.features().segmented_send;
            CHECK(sent.trains_sent == (trains ? planned.trains_sent : 0U));
            CHECK(sent.train_datagrams_sent == (trains ? planned.train_datagrams_sent : 0U));
            CHECK(sent.trains_refused == 0U);
            CHECK(to.offload_counters().trains_received == 0U);
        }
    }
    CHECK(burst_trains(Endpoint{IpAddress::v4_loopback(), 9U}).trains_sent == 5U);

    // A dual-stack socket's trains toward an IPv4 peer.
    UdpOptions options;
    options.bind = Endpoint{IpAddress::v6_any(), 0U};
    options.send_buffer_bytes = 1U << 22U;
    SocketError error = SocketError::Failed;
    UdpSocket dual = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    UdpSocket v4 = open_on(IpAddress::v4_loopback());
    send_burst(dual, v4.local_endpoint());
    receive_burst(v4, reach(dual, IpAddress::v4_loopback()));
#if CRD_OS_LINUX || CRD_OS_WINDOWS
    CHECK(dual.offload_counters().trains_sent == 5U);
#endif
}

TEST_CASE("a coalescing socket receives a sender's trains whole and each datagram as it was sent",
          "[net][udp][net-path]")
{
    const IpAddress families[] = {IpAddress::v4_loopback(), IpAddress::v6_loopback()};
    for (const IpAddress& family : families)
    {
        for (const bool segment : {true, false})
        {
            UdpSocket from;
            UdpSocket to;
            if (!open_offloaded(from, family, segment, false) || !open_offloaded(to, family, true, true))
            {
                SKIP("the host has no IPv6 loopback");
            }
            INFO("IPv" << (family.family == crd::net::AddressFamily::V4 ? 4 : 6) << ", segmenting " << segment);
#if CRD_OS_LINUX || CRD_OS_WINDOWS
            REQUIRE(to.features().coalesced_receive);
#else
            CHECK_FALSE(to.features().coalesced_receive); // macOS's offloads are not taken yet
#endif
            send_burst(from, to.local_endpoint());
            const u64 trains = receive_burst_coalesced(to, from.local_endpoint());
            CHECK(to.offload_counters().trains_received == trains);
#if CRD_OS_LINUX
            // On the loopback a segmented send reaches a coalescing socket whole; single datagrams stay single.
            // Windows' loopback hands even a segmented send on a datagram at a time (no URO there).
            CHECK(trains == (segment ? 5U : 0U));
            CHECK(to.offload_counters().train_datagrams_received ==
                  (segment ? burst_trains(to.local_endpoint()).train_datagrams_sent : 0U));
#endif
        }
    }
}

TEST_CASE("every train shape segment_train makes leaves in one call and arrives as the datagrams sent",
          "[net][udp][net-path]")
{
    struct Shape
    {
        usize size;  // every datagram's but the last
        usize count; // datagrams
        usize last;  // the last datagram's
    };
    // One-byte and ACK-sized segments, a short last one, a run past one train's payload, one train at the payload
    // limit, segments past any Ethernet MTU, and two datagrams together one short of the IPv4 payload.
    constexpr Shape shapes[] = {{1U, 64U, 1U},       {40U, 64U, 40U},  {40U, 3U, 17U},      {1200U, 64U, 1200U},
                                {1472U, 44U, 1472U}, {8000U, 16U, 3U}, {32753U, 2U, 32753U}};
    UdpSocket from;
    UdpSocket to;
    REQUIRE(open_offloaded(from, IpAddress::v4_loopback(), true, false));
    REQUIRE(open_offloaded(to, IpAddress::v4_loopback(), true, false));
    static u8 out[1U << 17U];
    static u8 in[1U << 17U];
    OffloadCounters planned;
    for (u32 shape_index = 0U; shape_index < sizeof(shapes) / sizeof(shapes[0]); ++shape_index)
    {
        const Shape& shape = shapes[shape_index];
        INFO(shape.count << " datagrams of " << shape.size << " bytes, the last " << shape.last);
        OutgoingDatagram datagrams[kMaxTrainDatagrams];
        ReceivedDatagram slots[kMaxTrainDatagrams];
        usize at = 0U;
        for (usize index = 0U; index < shape.count; ++index)
        {
            const usize size = (index + 1U == shape.count) ? shape.last : shape.size;
            pattern(Span<u8>(out + at, size), (shape_index << 8U) | static_cast<u32>(index));
            datagrams[index] = OutgoingDatagram{ConstSpan<u8>(out + at, size), to.local_endpoint(), Ecn::NotEct};
            slots[index].storage = Span<u8>(in + at, size);
            at += size;
        }
        const ConstSpan<OutgoingDatagram> run(datagrams, shape.count);
        for (usize next = 0U; next < shape.count;)
        {
            const usize train = segment_train(run.subspan(next));
            planned.trains_sent += (train > 1U) ? 1U : 0U;
            planned.train_datagrams_sent += (train > 1U) ? train : 0U;
            next += train;
        }
        REQUIRE(from.send(run).count == shape.count);
        REQUIRE(receive_all(to, Span<ReceivedDatagram>(slots, shape.count), shape.count) == shape.count);
        for (usize index = 0U; index < shape.count; ++index)
        {
            INFO("datagram " << index);
            CHECK_FALSE(slots[index].truncated);
            CHECK(slots[index].size == datagrams[index].bytes.size());
            const bool same = std::memcmp(slots[index].storage.data(), datagrams[index].bytes.data(),
                                          datagrams[index].bytes.size()) == 0;
            CHECK(same);
        }
    }
    const OffloadCounters& sent = from.offload_counters();
    CHECK(planned.trains_sent == 9U); // 54 and 10 of the 1200-byte run, 8 and 8 of the 8000-byte one
    CHECK(sent.trains_refused == 0U);
#if CRD_OS_LINUX || CRD_OS_WINDOWS
    CHECK(from.features().segmented_send);
    CHECK(sent.trains_sent == planned.trains_sent);
    CHECK(sent.train_datagrams_sent == planned.train_datagrams_sent);
#endif
}

TEST_CASE("a CE train goes as the host takes CE, and segmenting and marking stay on", "[net][udp][net-path]")
{
    UdpSocket from;
    UdpSocket to;
    REQUIRE(open_offloaded(from, IpAddress::v4_loopback(), true, false));
    REQUIRE(open_offloaded(to, IpAddress::v4_loopback(), true, false));
    static u8 payload[4][64];
    OutgoingDatagram datagrams[4];
    for (u32 index = 0U; index < 4U; ++index)
    {
        pattern(payload[index], index);
        datagrams[index] = OutgoingDatagram{ConstSpan<u8>(payload[index]), to.local_endpoint(), Ecn::Ce};
    }
    REQUIRE(segment_train(datagrams) == 4U);
    REQUIRE(from.send(datagrams).count == 4U);
    static Slots slots;
    REQUIRE(receive_all(to, Span<ReceivedDatagram>(slots.slots, 4U), 4U) == 4U);
    for (u32 index = 0U; index < 4U; ++index)
    {
        const ReceivedDatagram& slot = slots.slots[index];
        INFO("datagram " << index);
        CHECK(slot.size == sizeof(payload[index]));
        CHECK(matches(slot.storage.first(slot.size), index));
        CHECK(slot.ecn == (to.features().ecn_reading ? kCeArrives : Ecn::NotEct));
    }
    const OffloadCounters& sent = from.offload_counters();
#if CRD_OS_WINDOWS
    // Windows refuses a sender's CE: the train goes again a datagram a call, each then sent unmarked.
    CHECK(sent.trains_refused == 1U);
    CHECK(sent.trains_sent == 0U);
#elif CRD_OS_LINUX
    CHECK(sent.trains_sent == 1U);
    CHECK(sent.train_datagrams_sent == 4U);
    CHECK(sent.trains_refused == 0U);
#endif
    CHECK(from.features().segmented_send == (CRD_OS_LINUX != 0 || CRD_OS_WINDOWS != 0));
    CHECK(from.features().ecn_marking == (CRD_OS_LINUX != 0 || CRD_OS_WINDOWS != 0));
}

TEST_CASE("a train past the route's MTU is refused as each datagram would be, and segmenting stays on",
          "[net][udp][net-path]")
{
#if CRD_OS_LINUX || CRD_OS_WINDOWS
    UdpSocket from;
    REQUIRE(open_offloaded(from, IpAddress::v4_any(), true, false));
    // Past any link's MTU but a loopback's, jumbo frames and tunnels included, so nothing leaves the host.
    static u8 payload[2][32000];
    const Endpoint documentation{IpAddress::v4(192, 0, 2, 1), 9U}; // TEST-NET-1 (RFC 5737)
    const OutgoingDatagram datagrams[2] = {{ConstSpan<u8>(payload[0]), documentation, Ecn::NotEct},
                                           {ConstSpan<u8>(payload[1]), documentation, Ecn::NotEct}};
    REQUIRE(segment_train(datagrams) == 2U);
    const BatchResult result = from.send(datagrams);
    if (result.error == SocketError::Unreachable)
    {
        SKIP("the host has no route off the machine");
    }
    CHECK(result.error == SocketError::MessageTooLarge);
    CHECK(result.count == 0U);
    CHECK(from.offload_counters().trains_refused == 1U);
    CHECK(from.offload_counters().trains_sent == 0U);
    CHECK(from.features().segmented_send); // the train's own refusal, not the host's
#else
    SKIP("macOS lets the host fragment until crd-net's port there (MAC.1)");
#endif
}

#if CRD_OS_LINUX
namespace
{
using crd::net::testing::RouterPath;

// The UDP payload the router's hop carries toward the far host in a family.
u32 hop_payload(bool v6)
{
    return RouterPath::kHopMtu - (v6 ? 48U : 28U);
}

// The largest UDP payload of a 1500-byte link.
usize link_payload(bool v6)
{
    return RouterPath::kLinkMtu - (v6 ? 48U : 28U);
}

// Waits for and reads one report into the first slot, `slot_bytes` long.
const ReceivedDatagram& one_report(UdpSocket& socket, Slots& slots, usize slot_bytes)
{
    ReceivedDatagram& slot = slots.slots[0];
    slot.storage = Span<u8>(slots.storage[0], slot_bytes);
    REQUIRE(receive_all(socket, Span<ReceivedDatagram>(&slot, 1U), 1U) == 1U);
    return slot;
}

void spin_for(std::chrono::milliseconds span)
{
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until)
    {
    }
}
} // namespace

TEST_CASE("a router's Packet Too Big reaches a UDP socket as a report quoting the datagram, in every family",
          "[.netns]")
{
    RouterPath& path = RouterPath::get();
    // 0: IPv4; 1: IPv6; 2: an IPv6 socket carrying IPv4, whose reports come through IPv6's error queue.
    const u32 family = GENERATE(0U, 1U, 2U);
    const bool v6 = family == 1U;
    INFO("family " << family);
    UdpSocket near = path.open(RouterPath::Near, family != 0U, family == 2U);
    UdpSocket far = path.open(RouterPath::Far, v6);
    REQUIRE(near.features().too_big_reports);
    const Endpoint to = far.local_endpoint();
    const u32 limit = hop_payload(v6);
    // The router quotes what fits in 576 bytes of IPv4 (RFC 1812) or in IPv6's 1280 (RFC 4443).
    const usize quoted = v6 ? 1184U : 520U;
    static u8 payload[1500];
    pattern(Span<u8>(payload, sizeof(payload)), 11U);
    static Slots slots;

    const OutgoingDatagram large[] = {{ConstSpan<u8>(payload, 1400U), to}};
    REQUIRE(near.send(large).count == 1U);
    const ReceivedDatagram& report = one_report(near, slots, Slots::kBytes);
    CHECK(report.too_big == limit);
    CHECK(report.peer == to);
    CHECK(report.size == quoted);
    CHECK_FALSE(report.truncated);
    CHECK(matches(ConstSpan<u8>(report.storage.data(), report.size), 11U));
    CHECK_FALSE(far.wait_readable(100'000'000U));

    // The limit itself crosses whole with no report; a byte more draws another, cut to a short slot.
    const OutgoingDatagram fits[] = {{ConstSpan<u8>(payload, limit), to}};
    REQUIRE(near.send(fits).count == 1U);
    REQUIRE(receive_all(far, Span<ReceivedDatagram>(slots.slots + 1U, 1U), 1U) == 1U);
    CHECK(slots.slots[1].size == limit);
    CHECK(slots.slots[1].too_big == 0U);
    CHECK(matches(ConstSpan<u8>(slots.slots[1].storage.data(), limit), 11U));
    CHECK_FALSE(near.wait_readable(100'000'000U));
    const Endpoint near_seen = slots.slots[1].peer;
    const OutgoingDatagram over[] = {{ConstSpan<u8>(payload, limit + 1U), to}};
    REQUIRE(near.send(over).count == 1U);
    const ReceivedDatagram& cut = one_report(near, slots, 64U);
    CHECK(cut.too_big == limit);
    CHECK(cut.size == 64U);
    CHECK(cut.truncated);
    CHECK(matches(ConstSpan<u8>(cut.storage.data(), cut.size), 11U));

    // The far host's datagrams meet no small hop: the link's largest crosses toward the near host.
    const OutgoingDatagram back[] = {{ConstSpan<u8>(payload, link_payload(v6)), near_seen}};
    const BatchResult returned = far.send(back);
    CHECK(returned.error == SocketError::None);
    REQUIRE(returned.count == 1U);
    ReceivedDatagram& arrived = slots.slots[2];
    arrived.storage = Span<u8>(slots.storage[2], Slots::kBytes);
    REQUIRE(receive_all(near, Span<ReceivedDatagram>(&arrived, 1U), 1U) == 1U);
    CHECK(arrived.size == link_payload(v6));
    CHECK(arrived.too_big == 0U);
    CHECK_FALSE(far.wait_readable(100'000'000U));
}

TEST_CASE("a UDP socket drops the host's other reports: an unreachable port past the router and its own oversized "
          "send",
          "[.netns]")
{
    RouterPath& path = RouterPath::get();
    const bool v6 = GENERATE(false, true);
    INFO("IPv6 " << v6);
    UdpSocket near = path.open(RouterPath::Near, v6);
    Endpoint gone;
    {
        const UdpSocket closed = path.open(RouterPath::Far, v6);
        gone = closed.local_endpoint();
    }
    static u8 payload[1500];
    pattern(Span<u8>(payload, sizeof(payload)), 12U);
    static Slots slots;
    // The far host's port unreachable, three times: every send leaves, nothing is received, no wakeup is left.
    const OutgoingDatagram to_gone[] = {{ConstSpan<u8>(payload, 32U), gone}};
    for (u32 round = 0U; round < 3U; ++round)
    {
        const BatchResult sent = near.send(to_gone);
        REQUIRE(sent.error == SocketError::None);
        REQUIRE(sent.count == 1U);
        CHECK(near.wait_readable(kPatience));
        const BatchResult received = near.receive(slots.slots);
        CHECK(received.count == 0U);
        CHECK(received.error == SocketError::None);
        CHECK_FALSE(near.wait_readable(0U));
    }
    // With an unreachable report's error waiting, a datagram over the near link's MTU still fails as its own, and the
    // host's own report of it is dropped too.
    REQUIRE(near.send(to_gone).count == 1U);
    spin_for(std::chrono::milliseconds(50));
    const OutgoingDatagram oversized[] = {{ConstSpan<u8>(payload, link_payload(v6) + 1U), gone}};
    const BatchResult refused = near.send(oversized);
    CHECK(refused.count == 0U);
    CHECK(refused.error == SocketError::MessageTooLarge);
    CHECK(near.wait_readable(0U));
    const BatchResult nothing = near.receive(slots.slots);
    CHECK(nothing.count == 0U);
    CHECK(nothing.error == SocketError::None);
    CHECK_FALSE(near.wait_readable(0U));
    // A Packet Too Big after them is still reported.
    UdpSocket far = path.open(RouterPath::Far, v6);
    const OutgoingDatagram large[] = {{ConstSpan<u8>(payload, 1400U), far.local_endpoint()}};
    REQUIRE(near.send(large).count == 1U);
    CHECK(one_report(near, slots, Slots::kBytes).too_big == hop_payload(v6));
}

TEST_CASE("a datagram the host drops past the socket is lost on the way, and the batch goes on", "[.netns]")
{
    RouterPath& path = RouterPath::get();
    const bool v6 = GENERATE(false, true);
    INFO("IPv6 " << v6);
    UdpSocket near = path.open(RouterPath::Near, v6);
    static u8 payload[1400];
    pattern(Span<u8>(payload, sizeof(payload)), 14U);
    const Endpoint nowhere = RouterPath::dropping(v6);
    // A socket that asked for reports hears of the host's drop (ENOBUFS); the datagram is gone, not waiting.
    const OutgoingDatagram batch[] = {
        {ConstSpan<u8>(payload, 64U), nowhere},
        {ConstSpan<u8>(payload, sizeof(payload)), nowhere},
        {ConstSpan<u8>(payload, sizeof(payload)), nowhere},
        {ConstSpan<u8>(payload, 64U), nowhere},
    };
    const BatchResult sent = near.send(batch);
    CHECK(sent.error == SocketError::None);
    CHECK(sent.count == 4U);
    // The two long datagrams and the short one behind them went as one train, lost whole and counted once.
    CHECK(near.offload_counters().trains_sent == 1U);
    CHECK(near.offload_counters().train_datagrams_sent == 3U);
}

TEST_CASE("a flood of Packet Too Big reports is read 64 a call, and the datagrams behind it are not held back",
          "[.netns]")
{
    RouterPath& path = RouterPath::get();
    // A datagram a send: a forwarding hop answers a train it drops with one report.
    UdpSocket near = path.open(RouterPath::Near, false, false, false);
    UdpSocket far = path.open(RouterPath::Far, false);
    static u8 payload[1400];
    pattern(Span<u8>(payload, sizeof(payload)), 13U);
    constexpr usize kFlood = 80U;
    OutgoingDatagram flood[kFlood];
    for (OutgoingDatagram& datagram : flood)
    {
        datagram = OutgoingDatagram{ConstSpan<u8>(payload, sizeof(payload)), far.local_endpoint()};
    }
    usize sent = 0U;
    while (sent < kFlood)
    {
        const BatchResult result = near.send(ConstSpan<OutgoingDatagram>(flood).subspan(sent));
        REQUIRE(result.error == SocketError::None); // a report's error met by a send goes again once
        sent += result.count;
    }
    // Every report arrives, then a datagram from the far host behind them.
    spin_for(std::chrono::milliseconds(200));
    const OutgoingDatagram hello[] = {{ConstSpan<u8>(payload, 16U), near.local_endpoint()}};
    REQUIRE(far.send(hello).count == 1U);
    spin_for(std::chrono::milliseconds(50));
    REQUIRE(near.wait_readable(kPatience));
    static Slots slots;
    const BatchResult first = near.receive(slots.slots);
    REQUIRE(first.error == SocketError::None);
    usize reports = 0U;
    usize datagrams = 0U;
    for (usize index = 0U; index < first.count; ++index)
    {
        if (slots.slots[index].too_big != 0U)
        {
            ++reports;
        }
        else
        {
            ++datagrams;
            CHECK(slots.slots[index].size == 16U);
        }
    }
    CHECK(reports == 64U);
    CHECK(datagrams == 1U);
    // The rest come with the next call, and then nothing is left.
    const BatchResult rest = near.receive(slots.slots);
    CHECK(rest.error == SocketError::None);
    CHECK(reports + rest.count == kFlood);
    CHECK_FALSE(near.wait_readable(0U));
}

TEST_CASE("a train the host cannot segment goes a datagram a send, and the socket stops segmenting", "[.netns]")
{
    RouterPath& path = RouterPath::get();
    for (const bool v6 : {false, true})
    {
        INFO("IPv" << (v6 ? 6 : 4));
        UdpSocket near = path.open(RouterPath::Near, v6);
        UdpSocket far = path.open(RouterPath::Far, v6);
        REQUIRE(near.features().segmented_send);
        // Without transmit checksum offload on the near link the host refuses every segmented send with EIO.
        path.checksum_offload(RouterPath::Near, "a0", false);
        static u8 payload[10][1000];
        OutgoingDatagram datagrams[10];
        for (usize index = 0U; index < 10U; ++index)
        {
            pattern(Span<u8>(payload[index], sizeof(payload[index])), static_cast<u32>(index));
            datagrams[index] = OutgoingDatagram{ConstSpan<u8>(payload[index], sizeof(payload[index])),
                                                far.local_endpoint(), Ecn::Ect0};
        }
        const BatchResult sent = near.send(datagrams);
        path.checksum_offload(RouterPath::Near, "a0", true);
        CHECK(sent.error == SocketError::None);
        CHECK(sent.count == 10U);
        CHECK(near.offload_counters().trains_refused == 1U);
        CHECK(near.offload_counters().trains_sent == 0U);
        CHECK_FALSE(near.features().segmented_send); // for good: later sends go a datagram a call
        CHECK(near.features().ecn_marking);          // the refusal was the train's, not the codepoint's
        static Slots slots;
        REQUIRE(receive_all(far, Span<ReceivedDatagram>(slots.slots, 10U), 10U) == 10U);
        for (usize index = 0U; index < 10U; ++index)
        {
            CHECK(slots.slots[index].size == 1000U);
            CHECK(matches(slots.slots[index].storage.first(1000U), static_cast<u32>(index)));
            CHECK(slots.slots[index].ecn == Ecn::Ect0);
        }
        CHECK(near.send(datagrams).count == 10U);
        CHECK(near.offload_counters().trains_sent == 0U);
        REQUIRE(receive_all(far, Span<ReceivedDatagram>(slots.slots, 10U), 10U) == 10U);
    }
}

TEST_CASE("a train the host drops past the socket is lost whole and counted once, and the batch goes on", "[.netns]")
{
    RouterPath& path = RouterPath::get();
    for (const bool v6 : {false, true})
    {
        INFO("IPv" << (v6 ? 6 : 4));
        UdpSocket near = path.open(RouterPath::Near, v6);
        static u8 payload[1000];
        const Endpoint nowhere = RouterPath::dropping(v6);
        OutgoingDatagram batch[11];
        for (usize index = 0U; index < 11U; ++index)
        {
            // Two trains marked apart and a lone datagram.
            batch[index] = OutgoingDatagram{ConstSpan<u8>(payload, (index == 10U) ? 64U : sizeof(payload)), nowhere,
                                            (index < 5U) ? Ecn::Ect0 : Ecn::Ect1};
        }
        batch[10].ecn = Ecn::NotEct;
        path.full_queue(true);
        const BatchResult sent = near.send(batch);
        path.full_queue(false);
        CHECK(sent.error == SocketError::None);
        CHECK(sent.count == 11U);
        CHECK(near.offload_counters().trains_sent == 2U);
        CHECK(near.offload_counters().train_datagrams_sent == 10U);
        CHECK(near.features().segmented_send);
    }
}

TEST_CASE("a train with datagrams past the link's MTU is refused as each would be, and segmenting stays on", "[.netns]")
{
    RouterPath& path = RouterPath::get();
    for (const bool v6 : {false, true})
    {
        INFO("IPv" << (v6 ? 6 : 4));
        for (const bool segment : {true, false})
        {
            UdpSocket near = path.open(RouterPath::Near, v6, false, segment);
            UdpSocket far = path.open(RouterPath::Far, v6);
            static u8 payload[1600];
            const OutgoingDatagram datagrams[] = {
                {ConstSpan<u8>(payload, link_payload(v6) + 1U), far.local_endpoint(), Ecn::Ect0},
                {ConstSpan<u8>(payload, link_payload(v6) + 1U), far.local_endpoint(), Ecn::Ect0},
            };
            const BatchResult sent = near.send(datagrams);
            CHECK(sent.count == 0U);
            CHECK(sent.error == SocketError::MessageTooLarge);
            CHECK(near.offload_counters().trains_refused == (segment ? 1U : 0U));
            CHECK(near.features().segmented_send == segment); // a train's own refusal, not the host's
            CHECK(near.features().ecn_marking);               // nor the codepoint's (the host's EINVAL)
        }
    }
}

TEST_CASE("a train across the router's narrow hop loses its longer datagrams there, never fragmented", "[.netns]")
{
    RouterPath& path = RouterPath::get();
    for (const bool v6 : {false, true})
    {
        INFO("IPv" << (v6 ? 6 : 4));
        UdpSocket near = path.open(RouterPath::Near, v6);
        UdpSocket far = path.open(RouterPath::Far, v6);
        static u8 payload[8][1400];
        OutgoingDatagram datagrams[8];
        for (usize index = 0U; index < 8U; ++index)
        {
            // Four datagrams the hop cannot carry, then four it can, marked apart: two trains.
            const usize size = (index < 4U) ? hop_payload(v6) + 1U : hop_payload(v6);
            pattern(Span<u8>(payload[index], size), static_cast<u32>(index));
            datagrams[index] = OutgoingDatagram{ConstSpan<u8>(payload[index], size), far.local_endpoint(),
                                                (index < 4U) ? Ecn::NotEct : Ecn::Ect0};
        }
        REQUIRE(near.send(datagrams).count == 8U);
        CHECK(near.offload_counters().trains_sent == 2U);
        static Slots slots;
        REQUIRE(receive_all(far, Span<ReceivedDatagram>(slots.slots, 4U), 4U) == 4U);
        for (usize index = 0U; index < 4U; ++index)
        {
            CHECK(slots.slots[index].size == hop_payload(v6));
            CHECK(matches(slots.slots[index].storage.first(hop_payload(v6)), static_cast<u32>(4U + index)));
        }
        spin_for(std::chrono::milliseconds(50));
        CHECK_FALSE(far.wait_readable(0U)); // no fragment of a longer datagram arrived reassembled
        // The router reported the hop for the train it dropped.
        const ReceivedDatagram& report = one_report(near, slots, 2048U);
        CHECK(report.too_big == hop_payload(v6));
    }
}
#endif
