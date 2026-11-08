// crd-net -- the transport's I/O loop (4.2a; docs/design/network-foundation.md#ingress): turns over a virtual socket
// and a test clock (the seams NET-SIM drives), every action reaching the handler or the wire, back-pressure and hard
// send errors, the per-turn receive bound, no allocation in a turn, run() and its stop, the host clock, and a real
// UDP socket on the loopback answered with Version Negotiation.

#include "ingress_fixture.hpp"

#include <crd/net/io_loop.hpp>
#include <crd/net/udp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <memory>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::crypto::CryptoError;
using crd::crypto::QuicTokenKeys;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u8 kKey[crd::crypto::kPrfKeyBytes] = {7, 6, 5, 4, 3, 2, 1, 0, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr u8 kTokenKey[crd::crypto::kQuicTokenKeyBytes] = {
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F};
constexpr u64 kSecond = 1000000000U;
constexpr u8 kDcid[8] = {0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08};
constexpr u8 kScid[4] = {0xC1, 0xC2, 0xC3, 0xC4};

ConstSpan<u8> view_of(const u8* bytes, usize size)
{
    return ConstSpan<u8>(bytes, size);
}

class TestClock final : public TransportClock
{
public:
    [[nodiscard]] u64 monotonic_ns() const noexcept override { return now; }
    [[nodiscard]] u64 unix_seconds() const noexcept override { return 1793577600U + (now / kSecond); }

    u64 now = kSecond;
};

// A socket of fixed queues (on the heap: a test's stack holds the socket): what a test puts in arrives, what the loop
// sends is kept; `busy_after` stops sends with Busy after that many, `refuse_next` fails the next send for good.
class VirtualSocket final : public DatagramSocket
{
public:
    static constexpr usize kQueue = 256U;

    void deliver(const Datagram& datagram, const Endpoint& peer)
    {
        REQUIRE(m_inbound_count < kQueue);
        m_inbound[m_inbound_count++] = Inbound{datagram, peer};
    }

    [[nodiscard]] Endpoint local_endpoint() const noexcept override { return Endpoint{IpAddress::v4(10, 0, 0, 1), 443U}; }

    [[nodiscard]] BatchResult receive(Span<ReceivedDatagram> slots) noexcept override
    {
        BatchResult result;
        while (result.count < slots.size() && m_inbound_next < m_inbound_count)
        {
            const Inbound& inbound = m_inbound[m_inbound_next++];
            ReceivedDatagram& slot = slots[result.count++];
            const usize size = inbound.datagram.size;
            slot.truncated = size > slot.storage.size();
            slot.size = slot.truncated ? slot.storage.size() : size;
            std::memcpy(slot.storage.data(), inbound.datagram.bytes, slot.size);
            slot.peer = inbound.peer;
        }
        if (fail_receive)
        {
            result.error = SocketError::Failed;
        }
        return result;
    }

    [[nodiscard]] BatchResult send(ConstSpan<OutgoingDatagram> datagrams) noexcept override
    {
        ++send_calls;
        BatchResult result;
        for (const OutgoingDatagram& datagram : datagrams)
        {
            if (refuse_next)
            {
                refuse_next = false;
                result.error = SocketError::Unreachable;
                return result;
            }
            if (sent_count == busy_after || sent_count == kQueue)
            {
                result.error = SocketError::Busy;
                return result;
            }
            Outbound& out = sent[sent_count++];
            std::memcpy(out.datagram.bytes, datagram.bytes.data(), datagram.bytes.size());
            out.datagram.size = datagram.bytes.size();
            out.peer = datagram.peer;
            ++result.count;
        }
        return result;
    }

    [[nodiscard]] bool wait_readable(u64) noexcept override
    {
        ++waits;
        return m_inbound_next < m_inbound_count;
    }

    struct Outbound
    {
        Datagram datagram;
        Endpoint peer;
    };

    std::unique_ptr<Outbound[]> sent = std::make_unique<Outbound[]>(kQueue);
    usize sent_count = 0U;
    usize busy_after = ~usize{0};
    bool refuse_next = false;
    bool fail_receive = false;
    usize send_calls = 0U;
    usize waits = 0U;

private:
    struct Inbound
    {
        Datagram datagram;
        Endpoint peer;
    };

    std::unique_ptr<Inbound[]> m_inbound = std::make_unique<Inbound[]>(kQueue);
    usize m_inbound_count = 0U;
    usize m_inbound_next = 0U;
};

class RecordingHandler final : public IngressHandler
{
public:
    explicit RecordingHandler(ConnectionIdTable& table) : m_table(table) {}

    void on_route(const IngressDecision& decision, const ReceivedDatagram& datagram, u64) noexcept override
    {
        ++routes;
        last_connection = decision.connection;
        last_size = datagram.size;
    }

    void on_admit(const IngressDecision& decision, const ReceivedDatagram& datagram, u64) noexcept override
    {
        ++admits;
        last_peer = datagram.peer;
        (void)m_table.insert(decision.header.dcid, static_cast<u32>(100U + admits));
    }

    [[nodiscard]] u64 on_timers(u64 now_ns) noexcept override
    {
        ++timer_calls;
        if (stop_after != 0U && timer_calls >= stop_after && stop != nullptr)
        {
            stop->store(true, std::memory_order_release);
        }
        return (next_timer == kNeverNs) ? kNeverNs : now_ns + next_timer;
    }

    usize routes = 0U;
    usize admits = 0U;
    usize timer_calls = 0U;
    u32 last_connection = kNoConnection;
    usize last_size = 0U;
    Endpoint last_peer;
    u64 next_timer = kNeverNs;
    usize stop_after = 0U;
    std::atomic<bool>* stop = nullptr;

private:
    ConnectionIdTable& m_table;
};

struct Rig
{
    explicit Rig(const IoLoopConfig& config = IoLoopConfig{}, const HandshakeRules& rules = HandshakeRules{},
                 const crd::crypto::QuicResetKey* reset = nullptr)
        : routes(allocator, 64U, ConstSpan<u8>(kKey)), buckets(allocator, rules, ConstSpan<u8>(kKey)),
          ingress(IngressConfig{}, routes, buckets, &tokens, nullptr, reset), handler(routes),
          loop(allocator, socket, clock, ingress, handler, config)
    {
        REQUIRE(tokens.init(ConstSpan<u8>(kTokenKey), {}) == CryptoError::None);
        REQUIRE(loop.ready());
    }

    CountingAllocator allocator;
    ConnectionIdTable routes;
    HandshakeBuckets buckets;
    QuicTokenKeys tokens;
    Ingress ingress;
    VirtualSocket socket;
    TestClock clock;
    RecordingHandler handler;
    IoLoop loop;
};

Datagram unsupported_version()
{
    Datagram datagram = initial(kV1, view_of(kDcid, 8U), view_of(kScid, 4U));
    crd::containers::wire_store<u32>(Span<u8>(datagram.bytes), 1U, 0x1A2A3A4AU);
    return datagram;
}

const Endpoint kClient{IpAddress::v4(192, 0, 2, 9), 40000U};

TEST_CASE("A turn hands routed and admitted datagrams on and answers what S1 to S3 answer", "[net][io-loop]")
{
    HandshakeRules rules;
    rules.always_retry = false;
    Rig rig(IoLoopConfig{}, rules);
    rig.handler.next_timer = 5000U;
    const Datagram first = initial(kV1, view_of(kDcid, 8U), view_of(kScid, 4U));
    rig.socket.deliver(first, kClient);
    rig.socket.deliver(unsupported_version(), kClient);
    rig.socket.deliver(first, kClient); // routed now: the handler routed the client's ID
    const usize calls = rig.allocator.calls();

    CHECK(rig.loop.poll() == rig.clock.now + 5000U);
    CHECK(rig.allocator.calls() == calls);
    CHECK(rig.handler.admits == 1U);
    CHECK(rig.handler.last_peer == kClient);
    CHECK(rig.handler.routes == 1U);
    CHECK(rig.handler.last_connection == 101U);
    CHECK(rig.handler.last_size == kQuicMinInitialDatagram);
    REQUIRE(rig.socket.sent_count == 1U);
    CHECK(rig.socket.sent[0].peer == kClient);
    QuicHeader read;
    REQUIRE(parse_quic_header(rig.socket.sent[0].datagram.view(), 16U, read) == QuicHeaderError::None);
    CHECK(read.type == QuicPacketType::VersionNegotiation);
    CHECK(rig.loop.counters().received == 3U);
    CHECK(rig.loop.counters().answers_sent == 1U);
    CHECK(rig.loop.counters().polls == 1U);

    // A datagram longer than its slot is dropped, never read short.
    Datagram huge = initial(kV1, view_of(kDcid, 8U), view_of(kScid, 4U), {}, kDatagramBytes);
    IoLoopConfig small;
    small.slot_bytes = 1300U;
    Rig narrow(small);
    narrow.socket.deliver(huge, kClient);
    (void)narrow.loop.poll();
    CHECK(narrow.ingress.counters().drops[static_cast<usize>(DropReason::Truncated)] == 1U);
    CHECK(narrow.handler.admits == 0U);
}

TEST_CASE("A turn sends a Retry the client can verify, and a hard send error drops only that answer",
          "[net][io-loop]")
{
    HandshakeRules rules;
    rules.always_retry = true;
    Rig rig(IoLoopConfig{}, rules);
    rig.socket.refuse_next = true;
    rig.socket.deliver(initial(kV1, view_of(kDcid, 8U), view_of(kScid, 4U)), kClient);
    rig.socket.deliver(initial(kV1, view_of(kDcid, 8U), view_of(kScid, 4U)), kClient);
    (void)rig.loop.poll();
    CHECK(rig.loop.counters().answers_dropped == 1U);
    REQUIRE(rig.socket.sent_count == 1U);
    CHECK(rig.loop.queued_answers() == 0U);
    CHECK(crd::crypto::quic_retry_verify(crd::crypto::QuicVersion::V1, view_of(kDcid, 8U),
                                         rig.socket.sent[0].datagram.view()) == CryptoError::None);
}

TEST_CASE("Answers the host cannot take yet wait in the queue, and a full queue drops new ones", "[net][io-loop]")
{
    IoLoopConfig config;
    config.queued_answers = 4U;
    Rig rig(config);
    rig.socket.busy_after = 0U;
    for (usize index = 0U; index < 6U; ++index)
    {
        rig.socket.deliver(unsupported_version(), kClient);
    }
    (void)rig.loop.poll();
    CHECK(rig.loop.queued_answers() == 4U);
    CHECK(rig.loop.counters().answers_dropped == 2U);
    CHECK(rig.socket.sent_count == 0U);

    // The host takes two; two new answers then wrap to the ring's start while it is busy; then it takes everything.
    rig.socket.busy_after = 2U;
    rig.clock.now += kSecond;
    (void)rig.loop.poll();
    CHECK(rig.socket.sent_count == 2U);
    CHECK(rig.loop.queued_answers() == 2U);
    rig.socket.deliver(unsupported_version(), kClient);
    rig.socket.deliver(unsupported_version(), kClient);
    (void)rig.loop.poll();
    CHECK(rig.loop.queued_answers() == 4U);
    CHECK(rig.socket.sent_count == 2U);
    rig.socket.busy_after = ~usize{0};
    (void)rig.loop.poll();
    CHECK(rig.socket.sent_count == 6U);
    CHECK(rig.loop.queued_answers() == 0U);
    CHECK(rig.loop.counters().answers_sent == 6U);
}

TEST_CASE("A turn reads at most batches_per_poll batches, so timers run under a flood", "[net][io-loop]")
{
    IoLoopConfig config;
    config.batch = 8U;
    config.batches_per_poll = 4U;
    Rig rig(config);
    const u8 unknown[16] = {1};
    for (usize index = 0U; index < 100U; ++index)
    {
        rig.socket.deliver(short_packet(view_of(unknown, 16U)), kClient);
    }
    (void)rig.loop.poll();
    CHECK(rig.loop.counters().received == 32U);
    CHECK(rig.handler.timer_calls == 1U);
    (void)rig.loop.poll();
    (void)rig.loop.poll();
    (void)rig.loop.poll();
    CHECK(rig.loop.counters().received == 100U);
    CHECK(rig.ingress.counters().drops[static_cast<usize>(DropReason::UnknownConnection)] == 100U);

    rig.socket.fail_receive = true;
    (void)rig.loop.poll();
    CHECK(rig.loop.counters().receive_errors == 1U);
}

TEST_CASE("A Retry token that does not validate is answered with an INVALID_TOKEN close", "[net][io-loop]")
{
    Rig rig;
    u8 forged[40];
    std::memset(forged, 0x5C, sizeof(forged));
    forged[0] = static_cast<u8>(crd::crypto::QuicTokenKind::Retry);
    rig.socket.deliver(initial(kV1, view_of(kDcid, 8U), view_of(kScid, 4U), view_of(forged, sizeof(forged))), kClient);
    (void)rig.loop.poll();
    REQUIRE(rig.socket.sent_count == 1U);
    CHECK(rig.socket.sent[0].peer == kClient);
    CHECK(rig.ingress.counters().token_closes == 1U);
    CHECK(rig.handler.admits == 0U);
    QuicHeader read;
    REQUIRE(parse_quic_header(rig.socket.sent[0].datagram.view(), 16U, read) == QuicHeaderError::None);
    CHECK(read.type == QuicPacketType::Initial);
    CHECK(same(read.dcid, view_of(kScid, 4U)));
    CHECK(rig.socket.sent[0].datagram.size < kQuicMinInitialDatagram);
}

TEST_CASE("A short header for no connection is answered with a stateless reset when the server holds a key",
          "[net][io-loop]")
{
    constexpr u8 secret[crd::crypto::kQuicResetKeyBytes] = {0x5E};
    crd::crypto::QuicResetKey key;
    REQUIRE(key.init(ConstSpan<u8>(secret)) == CryptoError::None);
    Rig rig(IoLoopConfig{}, HandshakeRules{}, &key);
    const u8 unknown[16] = {2};
    u8 token[crd::crypto::kQuicResetTokenBytes];
    REQUIRE(key.token(view_of(unknown, 16U), Span<u8>(token)) == CryptoError::None);
    for (usize index = 0U; index < 10U; ++index)
    {
        rig.socket.deliver(short_packet(view_of(unknown, 16U)), kClient);
    }
    (void)rig.loop.poll();
    REQUIRE(rig.socket.sent_count == 10U);
    CHECK(rig.loop.counters().answers_sent == 10U);
    CHECK(rig.ingress.counters().resets == 10U);
    CHECK(rig.ingress.counters().drops[static_cast<usize>(DropReason::UnknownConnection)] == 0U);
    for (usize index = 0U; index < rig.socket.sent_count; ++index)
    {
        const Datagram& reset = rig.socket.sent[index].datagram;
        CHECK(rig.socket.sent[index].peer == kClient);
        CHECK(reset.size < 64U);
        CHECK(reset.size >= kQuicMinResetBytes);
        CHECK(std::memcmp(reset.bytes + reset.size - sizeof(token), token, sizeof(token)) == 0);
    }
}

TEST_CASE("run() turns until stopped and waits between turns", "[net][io-loop]")
{
    Rig rig;
    std::atomic<bool> stop{false};
    rig.handler.stop = &stop;
    rig.handler.stop_after = 3U;
    rig.handler.next_timer = 1000U;
    rig.loop.run(stop);
    CHECK(rig.handler.timer_calls == 3U);
    CHECK(rig.socket.waits == 2U); // none after the turn that stopped it
}

TEST_CASE("The loop refuses configurations and allocations it cannot run on", "[net][io-loop]")
{
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 4U);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    Ingress ingress(IngressConfig{}, routes, buckets, nullptr, nullptr);
    VirtualSocket socket;
    TestClock clock;
    RecordingHandler handler(routes);
    const auto ready = [&](const IoLoopConfig& config, crd::memory::IAllocator& from)
    {
        IoLoop loop(from, socket, clock, ingress, handler, config);
        return loop.ready();
    };
    CHECK(ready(IoLoopConfig{}, allocator));
    IoLoopConfig config;
    config.batch = 0U;
    CHECK_FALSE(ready(config, allocator));
    config = IoLoopConfig{};
    config.slot_bytes = 1199U;
    CHECK_FALSE(ready(config, allocator));
    config = IoLoopConfig{};
    config.slot_bytes = static_cast<u32>(kMaxUdpPayloadV6 + 1U);
    CHECK_FALSE(ready(config, allocator));
    config = IoLoopConfig{};
    config.batches_per_poll = 0U;
    CHECK_FALSE(ready(config, allocator));
    config = IoLoopConfig{};
    config.queued_answers = 0U;
    CHECK_FALSE(ready(config, allocator));
    config = IoLoopConfig{};
    config.max_wait_ns = 0U;
    CHECK_FALSE(ready(config, allocator));
    for (usize allowed = 0U; allowed < 4U; ++allowed)
    {
        CountingAllocator refusing;
        refusing.refuse_after(allowed);
        CHECK_FALSE(ready(IoLoopConfig{}, refusing));
    }
    IoLoop dead(allocator, socket, clock, ingress, handler, config);
    CHECK(dead.poll() == kNeverNs);
}

TEST_CASE("The host clock runs forward and reads the Unix time", "[net][io-loop]")
{
    const HostClock clock;
    const u64 first = clock.monotonic_ns();
    const u64 second = clock.monotonic_ns();
    CHECK(second >= first);
    CHECK(clock.unix_seconds() > 1735689600U); // after 2025-01-01
}

TEST_CASE("A loop on a real UDP socket answers an unsupported version on the loopback", "[net][io-loop]")
{
    UdpOptions options;
    options.bind = Endpoint{IpAddress::v4(127, 0, 0, 1), 0U};
    options.receive_buffer_bytes = 1U << 20U;
    options.send_buffer_bytes = 1U << 20U;
    SocketError error = SocketError::Failed;
    UdpSocket server = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    UdpSocket client = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);

    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 16U);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    Ingress ingress(IngressConfig{}, routes, buckets, nullptr, nullptr);
    RecordingHandler handler(routes);
    const HostClock clock;
    IoLoop loop(allocator, server, clock, ingress, handler, IoLoopConfig{});
    REQUIRE(loop.ready());

    const Datagram probe = unsupported_version();
    const OutgoingDatagram outgoing{probe.view(), server.local_endpoint()};
    REQUIRE(client.send(ConstSpan<OutgoingDatagram>(&outgoing, 1U)).count == 1U);
    REQUIRE(server.wait_readable(2U * kSecond));
    (void)loop.poll();
    CHECK(loop.counters().answers_sent == 1U);

    REQUIRE(client.wait_readable(2U * kSecond));
    u8 storage[kDatagramBytes] = {};
    ReceivedDatagram slot;
    slot.storage = Span<u8>(storage);
    REQUIRE(client.receive(Span<ReceivedDatagram>(&slot, 1U)).count == 1U);
    CHECK(slot.peer == server.local_endpoint());
    QuicHeader read;
    REQUIRE(parse_quic_header(view_of(storage, slot.size), 16U, read) == QuicHeaderError::None);
    CHECK(read.type == QuicPacketType::VersionNegotiation);
    CHECK(same(read.dcid, view_of(kScid, 4U)));
    CHECK(same(read.scid, view_of(kDcid, 8U)));
}

} // namespace
