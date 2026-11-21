// crd-net -- the transport's I/O loop (4.2a; docs/design/network-foundation.md#ingress): turns over a virtual socket
// and a test clock (the seams NET-SIM drives), every action reaching the handler or the wire, back-pressure and hard
// send errors, the per-turn receive bound, no allocation in a turn, run() and its stop, the host clock, and a real
// UDP socket on the loopback answered with Version Negotiation; a loop over a socket set (each datagram naming its
// socket, answers leaving by it, no socket starved, run() waking on any, real sockets included) and the send queue
// handing each socket its own datagrams in order, a busy one holding back no other.

#include "ingress_fixture.hpp"
#include "loop_fixture.hpp"
#include "quic_connection_fixture.hpp"

#include <crd/net/io_loop.hpp>
#include <crd/net/udp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

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

class RecordingHandler final : public IngressHandler
{
public:
    explicit RecordingHandler(ConnectionIdTable& table) : m_table(table) {}

    void on_route(const IngressDecision& decision, const ReceivedDatagram& datagram, u64) noexcept override
    {
        if (routes < kKept)
        {
            route_bytes[routes] = datagram.storage.first(datagram.size);
            route_ecn[routes] = datagram.ecn;
        }
        ++routes;
        last_connection = decision.connection;
        last_size = datagram.size;
        last_socket = datagram.socket;
        per_socket[datagram.socket % 2U] += 1U;
    }

    void on_admit(const IngressDecision& decision, const ReceivedDatagram& datagram, u64) noexcept override
    {
        ++admits;
        last_peer = datagram.peer;
        (void)m_table.insert(decision.header.dcid, static_cast<u32>(100U + admits));
    }

    void on_early(const IngressDecision& decision, const ReceivedDatagram& datagram, u64) noexcept override
    {
        ++earlies;
        last_connection = decision.connection;
        last_size = datagram.size;
    }

    [[nodiscard]] bool on_unrouted(const IngressDecision& decision, const ReceivedDatagram& datagram,
                                   u64) noexcept override
    {
        ++unrouted;
        last_socket = datagram.socket;
        per_socket[datagram.socket % 2U] += 1U;
        last_answered = decision.action == IngressAction::StatelessReset;
        return claim_unrouted;
    }

    void on_too_big(const ReceivedDatagram& report, u64) noexcept override
    {
        ++too_big;
        last_peer = report.peer;
        last_size = report.size;
        last_limit = report.too_big;
    }

    [[nodiscard]] u64 on_timers(u64 now_ns) noexcept override
    {
        ++timer_calls;
        polled.fetch_add(1U, std::memory_order_release);
        if (stop_after != 0U && timer_calls >= stop_after && stop != nullptr)
        {
            stop->store(true, std::memory_order_release);
        }
        return (next_timer == kNeverNs) ? kNeverNs : now_ns + next_timer;
    }

    static constexpr usize kKept = 64U;
    ConstSpan<u8> route_bytes[kKept]; // each routed datagram's bytes where the loop held them, for the first kKept
    Ecn route_ecn[kKept] = {};
    usize routes = 0U;
    usize admits = 0U;
    usize earlies = 0U;
    usize unrouted = 0U;
    usize too_big = 0U;
    u32 last_limit = 0U;
    bool claim_unrouted = false; // on_unrouted takes every short header for no connection
    bool last_answered = false;  // the last one on_unrouted saw was to be answered with a reset
    usize timer_calls = 0U;
    u32 last_connection = kNoConnection;
    usize last_size = 0U;
    u32 last_socket = ~u32{0};
    usize per_socket[2] = {}; // routed and unrouted datagrams by the socket they arrived on
    Endpoint last_peer;
    u64 next_timer = kNeverNs;
    usize stop_after = 0U;
    std::atomic<bool>* stop = nullptr;
    std::atomic<usize> polled{0U}; // turns, read from another thread

private:
    ConnectionIdTable& m_table;
};

// A loop over `socket`, or over `socket` and `second` with `sockets` 2.
struct Rig
{
    explicit Rig(const IoLoopConfig& config = IoLoopConfig{}, const HandshakeRules& rules = HandshakeRules{},
                 const crd::crypto::QuicResetKey* reset = nullptr, usize sockets = 1U)
        : routes(allocator, 64U, ConstSpan<u8>(kKey)), buckets(allocator, rules, ConstSpan<u8>(kKey)),
          ingress(IngressConfig{}, routes, buckets, &tokens, nullptr, reset), handler(routes),
          loop(allocator, set(sockets), clock, ingress, handler, config)
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
    VirtualSocket second;
    TestClock clock;
    RecordingHandler handler;
    IoLoop loop;

private:
    SocketSet set(usize count)
    {
        DatagramSocket* const sockets[] = {&socket, &second};
        return SocketSet(ConstSpan<DatagramSocket*>(sockets, count));
    }
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

TEST_CASE("A Packet Too Big report goes to the handler and past every admission stage", "[net][io-loop][net-path]")
{
    Rig rig;
    // The quote of a datagram sent to the client: a short header whose ID routes nowhere, which S2 would drop.
    Datagram quote = initial(kV1, view_of(kDcid, 8U), view_of(kScid, 4U));
    quote.bytes[0] = 0x40U;
    quote.size = 48U;
    rig.socket.deliver(quote, kClient, Ecn::NotEct, 1280U);
    rig.socket.deliver(initial(kV1, view_of(kDcid, 8U), view_of(kScid, 4U)), kClient); // a datagram after it
    (void)rig.loop.poll();
    CHECK(rig.handler.too_big == 1U);
    CHECK(rig.handler.last_limit == 1280U);
    CHECK(rig.handler.last_size == 48U);
    CHECK(rig.handler.unrouted == 0U);
    CHECK(rig.handler.routes + rig.handler.admits + rig.socket.sent_count == 1U); // the Initial alone was admitted
    CHECK(rig.loop.counters().received == 2U);
    CHECK(rig.loop.counters().too_big == 1U);
    CHECK(rig.ingress.counters().drops[static_cast<usize>(DropReason::UnknownConnection)] == 0U);
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

TEST_CASE("A stalled socket's answers fill only its part of the queue: the other socket's still leave",
          "[net][io-loop][net-path]")
{
    IoLoopConfig config;
    config.queued_answers = 8U; // a share of 2 for each of two sockets
    Rig rig(config, HandshakeRules{}, nullptr, 2U);
    rig.socket.busy_after = 0U;
    for (usize index = 0U; index < 8U; ++index)
    {
        rig.socket.deliver(unsupported_version(), kClient);
    }
    rig.second.deliver(unsupported_version(), kClient);
    rig.second.deliver(unsupported_version(), kClient);
    (void)rig.loop.poll();
    CHECK(rig.loop.queued_answers() == 6U); // the first socket's, all but the second's share
    CHECK(rig.loop.counters().answers_dropped == 2U);
    CHECK(rig.second.sent_count == 2U);
    CHECK(rig.socket.sent_count == 0U);

    // Still stalled: the second socket keeps its share turn after turn.
    for (usize turn = 0U; turn < 3U; ++turn)
    {
        rig.socket.deliver(unsupported_version(), kClient);
        rig.second.deliver(unsupported_version(), kClient);
        (void)rig.loop.poll();
    }
    CHECK(rig.second.sent_count == 5U);
    CHECK(rig.loop.counters().answers_dropped == 5U);
    rig.socket.busy_after = ~usize{0};
    (void)rig.loop.poll();
    CHECK(rig.socket.sent_count == 6U);
    CHECK(rig.loop.queued_answers() == 0U);
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

TEST_CASE("0-RTT for no connection goes to the handler unanswered, and a short header it claims is not answered",
          "[net][io-loop]")
{
    constexpr u8 secret[crd::crypto::kQuicResetKeyBytes] = {0x5E};
    crd::crypto::QuicResetKey key;
    REQUIRE(key.init(ConstSpan<u8>(secret)) == CryptoError::None);
    Rig rig(IoLoopConfig{}, HandshakeRules{}, &key);
    const u8 unknown[16] = {3};
    rig.socket.deliver(long_packet(kV1, QuicPacketType::ZeroRtt, view_of(unknown, 16U), {}, {}, 90U), kClient);
    rig.socket.deliver(long_packet(kV2, QuicPacketType::ZeroRtt, view_of(unknown, 8U), {}, {}, 70U), kClient);
    rig.socket.deliver(long_packet(kV1, QuicPacketType::ZeroRtt, view_of(unknown, 7U), {}, {}, 70U), kClient);
    (void)rig.loop.poll();
    CHECK(rig.handler.earlies == 2U);
    CHECK(rig.handler.last_size == 70U);
    CHECK(rig.handler.last_connection == kNoConnection);
    CHECK(rig.ingress.counters().early == 2U);
    CHECK(rig.ingress.counters().drops[static_cast<usize>(DropReason::EarlyPacket)] == 1U);
    CHECK(rig.socket.sent_count == 0U);

    // A short header for no connection is offered to the handler first: claimed, it is not answered.
    rig.handler.claim_unrouted = true;
    rig.socket.deliver(short_packet(view_of(unknown, 16U)), kClient);
    (void)rig.loop.poll();
    CHECK(rig.handler.unrouted == 1U);
    CHECK(rig.handler.last_answered);
    CHECK(rig.socket.sent_count == 0U);
    rig.handler.claim_unrouted = false;
    rig.socket.deliver(short_packet(view_of(unknown, 16U)), kClient);
    (void)rig.loop.poll();
    CHECK(rig.handler.unrouted == 2U);
    CHECK(rig.socket.sent_count == 1U);
    // One too short to be a packet is offered too (a stateless reset may be 21 bytes), and never answered.
    rig.socket.deliver(short_packet(view_of(unknown, 16U), 1U + 16U + 19U), kClient);
    (void)rig.loop.poll();
    CHECK(rig.handler.unrouted == 3U);
    CHECK(rig.socket.sent_count == 1U);
    CHECK(rig.ingress.counters().drops[static_cast<usize>(DropReason::TooShortToProtect)] == 1U);
    // One the ingress would not answer (a server without a reset key) is offered too.
    Rig keyless;
    keyless.socket.deliver(short_packet(view_of(unknown, 16U)), kClient);
    (void)keyless.loop.poll();
    CHECK(keyless.handler.unrouted == 1U);
    CHECK_FALSE(keyless.handler.last_answered);
    CHECK(keyless.socket.sent_count == 0U);
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

TEST_CASE("A coalescing socket's train reaches the handler a datagram at a time, in place, beside a plain socket's",
          "[net][io-loop][net-path]")
{
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 64U, ConstSpan<u8>(kKey));
    HandshakeBuckets buckets(allocator, HandshakeRules{}, ConstSpan<u8>(kKey));
    Ingress ingress(IngressConfig{}, routes, buckets, nullptr, nullptr);
    TestClock clock;
    RecordingHandler handler(routes);
    VirtualSocket coalescing;
    coalescing.offers.coalesced_receive = true;
    VirtualSocket plain;
    DatagramSocket* const sockets[] = {&coalescing, &plain};
    IoLoopConfig config;
    config.slot_bytes = 1300U;
    config.train_slots = 2U;
    IoLoop loop(allocator, SocketSet(ConstSpan<DatagramSocket*>(sockets)), clock, ingress, handler, config);
    REQUIRE(loop.ready());
    const u8 routed[16] = {0x2A, 0x2B, 0x2C};
    REQUIRE(routes.insert(view_of(routed, 16U), 7U) == RouteError::None);

    // Five 1200-byte short packets and a 300-byte one, each its index in its last byte.
    static u8 train[kMaxTrainBytes];
    const usize sizes[] = {1200U, 1200U, 1200U, 1200U, 1200U, 300U};
    usize at = 0U;
    for (usize index = 0U; index < 6U; ++index)
    {
        const Datagram packet = short_packet(view_of(routed, 16U), sizes[index]);
        std::memcpy(train + at, packet.bytes, packet.size);
        train[at + packet.size - 1U] = static_cast<u8>(index);
        at += packet.size;
    }
    coalescing.deliver_train(ConstSpan<u8>(train, at), 1200U, kClient);
    plain.deliver(short_packet(view_of(routed, 16U), 100U), kClient);
    const usize calls = allocator.calls();
    (void)loop.poll();
    CHECK(allocator.calls() == calls);
    REQUIRE(handler.routes == 7U);
    CHECK(loop.counters().trains == 1U);
    CHECK(loop.counters().received == 7U);
    for (usize index = 0U; index < 6U; ++index)
    {
        INFO("datagram " << index);
        REQUIRE(handler.route_bytes[index].size() == sizes[index]);
        CHECK(handler.route_bytes[index].back() == index);
        CHECK(handler.route_ecn[index] == Ecn::Ect0);
        // In place: each datagram where it lies in the train's slot, never copied.
        CHECK(handler.route_bytes[index].data() == handler.route_bytes[0].data() + (1200U * index));
    }
    CHECK(handler.route_bytes[6].size() == 100U); // the plain socket's, read into a plain slot
    CHECK(handler.last_socket == 1U);

    // A truncated train's cut last datagram is truncated and dropped; its whole ones arrive.
    coalescing.deliver_train(ConstSpan<u8>(train, 2400U + 500U), 1200U, kClient, true);
    (void)loop.poll();
    CHECK(handler.routes == 9U);
    CHECK(ingress.counters().drops[static_cast<usize>(DropReason::Truncated)] == 1U);
    // A train cut exactly at a datagram's end lost only datagrams it does not hold.
    coalescing.deliver_train(ConstSpan<u8>(train, 2400U), 1200U, kClient, true);
    (void)loop.poll();
    CHECK(handler.routes == 11U);

    // Datagrams longer than slot_bytes are truncated in a train as a plain slot of that size truncates them.
    static u8 wide[3U * 1400U];
    for (usize index = 0U; index < 3U; ++index)
    {
        const Datagram packet = short_packet(view_of(routed, 16U), 1400U);
        std::memcpy(wide + (1400U * index), packet.bytes, packet.size);
    }
    coalescing.deliver_train(ConstSpan<u8>(wide), 1400U, kClient);
    (void)loop.poll();
    CHECK(handler.routes == 11U);
    CHECK(ingress.counters().drops[static_cast<usize>(DropReason::Truncated)] == 4U);
    CHECK(loop.counters().trains == 4U);

    // A slot the socket says holds one datagram stays one, whatever its size.
    plain.deliver(short_packet(view_of(routed, 16U), 1200U), kClient);
    coalescing.deliver(short_packet(view_of(routed, 16U), 1200U), kClient);
    (void)loop.poll();
    CHECK(handler.routes == 13U);
    CHECK(loop.counters().trains == 4U);

    // Train slots exist only for a set that coalesces, and then need a count in range.
    IoLoopConfig none = config;
    none.train_slots = 0U;
    CHECK_FALSE(
        IoLoop(allocator, SocketSet(ConstSpan<DatagramSocket*>(sockets)), clock, ingress, handler, none).ready());
    none.train_slots = 1025U;
    CHECK_FALSE(
        IoLoop(allocator, SocketSet(ConstSpan<DatagramSocket*>(sockets)), clock, ingress, handler, none).ready());
    none.train_slots = 0U;
    CHECK(IoLoop(allocator, plain, clock, ingress, handler, none).ready());
    for (usize allowed = 0U; allowed < 6U; ++allowed)
    {
        CountingAllocator refusing;
        refusing.refuse_after(allowed);
        CHECK_FALSE(
            IoLoop(refusing, SocketSet(ConstSpan<DatagramSocket*>(sockets)), clock, ingress, handler, config).ready());
    }
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

TEST_CASE("The loop refuses an empty socket set", "[net][io-loop]")
{
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 4U);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    Ingress ingress(IngressConfig{}, routes, buckets, nullptr, nullptr);
    TestClock clock;
    RecordingHandler handler(routes);
    VirtualSocket socket;
    DatagramSocket* const sockets[] = {&socket, &socket, &socket, &socket, &socket};
    for (const usize count : {usize{0}, usize{kMaxSetSockets + 1U}})
    {
        const SocketSet set{ConstSpan<DatagramSocket*>(sockets, count)};
        CHECK_FALSE(set.ready());
        IoLoop loop(allocator, set, clock, ingress, handler, IoLoopConfig{});
        CHECK_FALSE(loop.ready());
        CHECK(loop.poll() == kNeverNs);
    }
    DatagramSocket* const holed[] = {&socket, nullptr};
    CHECK_FALSE(SocketSet(ConstSpan<DatagramSocket*>(holed)).ready());
    const SocketSet full{ConstSpan<DatagramSocket*>(sockets, kMaxSetSockets)};
    CHECK(full.size() == kMaxSetSockets);
    CHECK(IoLoop(allocator, full, clock, ingress, handler, IoLoopConfig{}).ready());
}

TEST_CASE("A loop serves each socket of its set: a datagram names the socket it arrived on, an answer leaves by it, "
          "and a flood on one starves no other",
          "[net][io-loop][net-path]")
{
    IoLoopConfig config;
    config.batch = 8U;
    config.batches_per_poll = 2U;
    Rig rig(config, HandshakeRules{}, nullptr, 2U);
    rig.second.local = Endpoint{IpAddress::v4(10, 0, 0, 2), 443U};

    // A routed datagram on the second socket, then one on the first.
    const u8 routed[16] = {0x2A, 0x2B};
    REQUIRE(rig.routes.insert(view_of(routed, 16U), 7U) == RouteError::None);
    rig.second.deliver(short_packet(view_of(routed, 16U)), kClient);
    (void)rig.loop.poll();
    CHECK(rig.handler.routes == 1U);
    CHECK(rig.handler.last_socket == 1U);
    rig.socket.deliver(short_packet(view_of(routed, 16U)), kClient);
    (void)rig.loop.poll();
    CHECK(rig.handler.last_socket == 0U);

    // An unsupported version is answered from the socket it reached.
    rig.second.deliver(unsupported_version(), kClient);
    (void)rig.loop.poll();
    CHECK(rig.socket.sent_count == 0U);
    REQUIRE(rig.second.sent_count == 1U);
    CHECK(rig.second.sent[0].peer == kClient);
    rig.socket.deliver(unsupported_version(), kClient);
    (void)rig.loop.poll();
    CHECK(rig.socket.sent_count == 1U);
    CHECK(rig.second.sent_count == 1U);
    CHECK(rig.loop.counters().answers_sent == 2U);

    // 100 datagrams on the first socket and 3 on the second: one turn reads 16 of the first's and all three.
    const u8 unknown[16] = {1};
    for (usize index = 0U; index < 100U; ++index)
    {
        rig.socket.deliver(short_packet(view_of(unknown, 16U)), kClient);
    }
    for (usize index = 0U; index < 3U; ++index)
    {
        rig.second.deliver(short_packet(view_of(unknown, 16U)), kClient);
    }
    rig.handler.per_socket[0] = 0U;
    rig.handler.per_socket[1] = 0U;
    (void)rig.loop.poll();
    CHECK(rig.handler.per_socket[0] == 16U);
    CHECK(rig.handler.per_socket[1] == 3U);
    CHECK(rig.handler.timer_calls == 5U);

    // A failing receive on one socket is counted and the next socket is still read.
    rig.socket.fail_receive = true;
    rig.second.deliver(short_packet(view_of(unknown, 16U)), kClient);
    rig.handler.per_socket[1] = 0U;
    (void)rig.loop.poll();
    CHECK(rig.loop.counters().receive_errors == 1U);
    CHECK(rig.handler.per_socket[1] == 1U);
}

TEST_CASE("run() over a set of simulated sockets asks each and waits a slice at most", "[net][io-loop][net-path]")
{
    Rig rig(IoLoopConfig{}, HandshakeRules{}, nullptr, 2U);
    std::atomic<bool> stop{false};
    rig.handler.stop = &stop;
    rig.handler.stop_after = 3U;
    rig.handler.next_timer = 1000U * kSecond; // longer than any wait: only the slice ends one
    rig.loop.run(stop);
    CHECK(rig.handler.timer_calls == 3U);
    CHECK(rig.socket.waits == 2U * 2U); // each of two waits: asked, then the slice
    CHECK(rig.second.waits == 2U * 2U); // asked, then again after the slice

    // A datagram waiting on the second ends the wait at once, the first asked only without waiting.
    rig.socket.waits = 0U;
    rig.second.waits = 0U;
    rig.second.deliver(unsupported_version(), kClient);
    DatagramSocket* const both[] = {&rig.socket, &rig.second};
    CHECK(SocketSet(ConstSpan<DatagramSocket*>(both)).wait_readable(kSecond));
    CHECK(rig.socket.waits == 1U);
    CHECK(rig.second.waits == 1U);
}

TEST_CASE("A send queue hands each socket its own datagrams in order: a busy one holds back no other, a refusal drops "
          "one, and a socket the set lacks drops what names it",
          "[net][io-loop][net-path]")
{
    CountingAllocator allocator;
    SendQueue queue(allocator, 64U, 64U, 3U); // shared by three sockets, flushed through a set of the first two
    REQUIRE(queue.ready());
    CHECK(queue.share() == 10U);
    VirtualSocket first;
    VirtualSocket second;
    DatagramSocket* const both[] = {&first, &second};
    const SocketSet set{ConstSpan<DatagramSocket*>(both)};
    const auto push = [&](u8 tag, u32 socket)
    {
        const Span<u8> room = queue.room(socket);
        REQUIRE(room.size() == 64U);
        room[0] = tag;
        REQUIRE(queue.push(1U, kClient, Ecn::NotEct, socket));
    };
    const auto tags = [](const VirtualSocket& socket)
    {
        u64 out = 0U;
        for (usize index = 0U; index < socket.sent_count; ++index)
        {
            out = (out << 8U) | socket.sent[index].datagram.bytes[0];
        }
        return out;
    };

    push(0xA0U, 0U);
    push(0xB0U, 1U);
    push(0xA1U, 0U);
    push(0xB1U, 1U);
    push(0xC0U, 2U); // the flushed set has two sockets

    // The queue's sockets are three: a fourth is refused at once.
    CHECK(queue.room(3U).empty());
    CHECK_FALSE(queue.push(1U, kClient, Ecn::NotEct, 3U));
    first.busy_after = 0U;
    SendFlush flushed = queue.flush(set);
    CHECK(flushed.sent == 2U);
    CHECK(flushed.dropped == 1U);
    CHECK(queue.queued() == 2U);
    CHECK(tags(second) == 0xB0B1U);
    CHECK(first.sent_count == 0U);

    push(0xB2U, 1U); // the second leaves while the first is still busy
    flushed = queue.flush(set);
    CHECK(flushed.sent == 1U);
    CHECK(tags(second) == 0xB0B1B2U);
    first.busy_after = ~usize{0};
    flushed = queue.flush(set);
    CHECK(flushed.sent == 2U);
    CHECK(tags(first) == 0xA0A1U);
    CHECK(queue.queued() == 0U);

    second.sent_count = 0U;
    push(0xB3U, 1U);
    push(0xB4U, 1U);
    second.refuse_next = true;
    flushed = queue.flush(set);
    CHECK(flushed.sent == 1U);
    CHECK(flushed.dropped == 1U);
    CHECK(tags(second) == 0xB4U);

    // Interleaved up to what the third socket's share leaves: every one leaves, each socket's in order, in one call.
    first.sent_count = 0U;
    second.sent_count = 0U;
    first.send_calls = 0U;
    for (u8 index = 0U; index < 27U; ++index)
    {
        push(index, 0U);
        push(static_cast<u8>(0x80U + index), 1U);
    }
    CHECK(queue.held() == 0b011U); // the third's share stays free for it
    CHECK(queue.room(2U).size() == 64U);
    flushed = queue.flush(set);
    CHECK(flushed.sent == 54U);
    REQUIRE(first.sent_count == 27U);
    REQUIRE(second.sent_count == 27U);
    for (usize index = 0U; index < 27U; ++index)
    {
        CHECK(first.sent[index].datagram.bytes[0] == index);
        CHECK(second.sent[index].datagram.bytes[0] == 0x80U + index);
    }
    CHECK(first.send_calls == 1U);
    // One socket fills all but the others' shares.
    for (u8 index = 0U; index < 44U; ++index)
    {
        push(index, 0U);
    }
    CHECK(queue.room(0U).empty());
    CHECK(queue.held() == 0b001U);
    CHECK(queue.queued(0U) == 44U);
    first.sent_count = 0U;
    first.send_calls = 0U;
    flushed = queue.flush(set);
    CHECK(flushed.sent == 44U);
    CHECK(first.send_calls == 2U); // 32 a call
    CHECK(first.sent[43].datagram.bytes[0] == 43U);
    CHECK(queue.queued(0U) == 0U);
}

TEST_CASE("A send queue keeps every socket's order and accounts for every datagram under random busy and refusing "
          "sockets",
          "[net][io-loop][net-path]")
{
    for (u64 seed = 1U; seed <= 8U; ++seed)
    {
        INFO("seed " << seed);
        CountingAllocator allocator;
        SendQueue queue(allocator, 48U, 16U, 4U); // the fourth socket's datagrams drop at the flush of three
        REQUIRE(queue.ready());
        REQUIRE(queue.share() == 6U);
        VirtualSocket sockets[3];
        DatagramSocket* const pointers[] = {&sockets[0], &sockets[1], &sockets[2]};
        const SocketSet set{ConstSpan<DatagramSocket*>(pointers)};
        Rng rng;
        rng.state = seed;
        u32 next[4] = {};      // per socket (3: one the set lacks), the next number pushed
        usize refused[4] = {}; // per socket, the pushes admission held back
        u32 model[4] = {};     // per socket, the datagrams it has queued
        u32 expected[3] = {};  // per socket, the next number its sends may carry at least
        usize pushed = 0U;
        usize sent = 0U;
        usize dropped = 0U;
        for (usize step = 0U; step < 4000U; ++step)
        {
            if (rng.next() % 3U != 0U)
            {
                // Mostly the first socket, which then fills what the others' shares leave.
                const u64 pick = rng.next() % 8U;
                const auto socket = static_cast<u32>((pick < 4U) ? 0U : pick - 4U);
                const Span<u8> room = queue.room(socket);
                // Admission: a socket below its share always finds room; held() names exactly the refused.
                CHECK((room.empty() == (((queue.held() >> socket) & 1U) != 0U)));
                if (queue.queued(socket) < queue.share())
                {
                    CHECK_FALSE(room.empty());
                }
                if (room.empty())
                {
                    ++refused[socket];
                    continue;
                }
                std::memcpy(room.data(), &next[socket], sizeof(u32));
                ++next[socket];
                REQUIRE(queue.push(sizeof(u32), kClient, Ecn::NotEct, socket));
                ++model[socket];
                ++pushed;
                continue;
            }
            bool refusing[3] = {};
            for (u32 index = 0U; index < 3U; ++index)
            {
                VirtualSocket& socket = sockets[index];
                socket.sent_count = 0U;
                socket.busy_after = (rng.next() % 3U == 0U) ? rng.next() % 4U : ~usize{0};
                socket.refuse_next = rng.next() % 8U == 0U;
                refusing[index] = socket.refuse_next;
            }
            if (step >= 1000U && step < 2000U)
            {
                sockets[0].busy_after = 0U; // stalled: its backlog grows to its limit
                sockets[0].refuse_next = false;
                refusing[0] = false;
            }
            const SendFlush flushed = queue.flush(set);
            sent += flushed.sent;
            dropped += flushed.dropped;
            usize seen = 0U;
            for (u32 index = 0U; index < 3U; ++index)
            {
                const VirtualSocket& socket = sockets[index];
                seen += socket.sent_count;
                for (usize at = 0U; at < socket.sent_count; ++at)
                {
                    u32 number = 0U;
                    std::memcpy(&number, socket.sent[at].datagram.bytes, sizeof(u32));
                    CHECK(number >= expected[index]); // in order; a refused one is skipped
                    expected[index] = number + 1U;
                }
            }
            CHECK(seen == flushed.sent);
            // Each socket's count is what was pushed for it less what left or was refused; the fourth's all drop.
            u32 counted = 0U;
            for (u32 socket = 0U; socket < 4U; ++socket)
            {
                if (socket < 3U)
                {
                    model[socket] -= static_cast<u32>(sockets[socket].sent_count);
                    model[socket] -= (refusing[socket] && !sockets[socket].refuse_next) ? 1U : 0U;
                }
                else
                {
                    model[socket] = 0U;
                }
                CHECK(queue.queued(socket) == model[socket]);
                counted += queue.queued(socket);
            }
            CHECK(counted == queue.queued());
        }
        CHECK(pushed == sent + dropped + queue.queued());
        CHECK(refused[0] != 0U); // the stalled first socket reached its limit
        CHECK(next[3] != 0U);    // and the one the set lacks was queued, then dropped
        CHECK(dropped >= next[3]);
    }
}

TEST_CASE("A send queue refuses socket counts out of range and gives each socket a share its slots allow",
          "[net][io-loop][net-path]")
{
    CountingAllocator allocator;
    for (const u32 sockets : {0U, kMaxSetSockets + 1U})
    {
        SendQueue refused(allocator, 16U, 64U, sockets);
        CHECK_FALSE(refused.ready());
        CHECK(refused.room(0U).empty());
        CHECK(refused.held() == 0U); // a queue with no sockets holds none
        CHECK(refused.queued(kMaxSetSockets) == 0U);
        CHECK_FALSE(refused.push(1U, kClient, Ecn::NotEct, 0U));
    }
    CHECK(allocator.calls() == 0U); // refused before allocating anything

    // Fewer slots than sockets: no shares, and every slot goes to whichever asks first.
    SendQueue scarce(allocator, 3U, 64U, 4U);
    REQUIRE(scarce.ready());
    CHECK(scarce.share() == 0U);
    for (u32 socket = 0U; socket < 3U; ++socket)
    {
        CHECK(scarce.push(1U, kClient, Ecn::NotEct, 0U));
    }
    CHECK(scarce.held() == 0b1111U);

    // As many slots as sockets: one each, so a socket that takes the first holds none of the others' back.
    SendQueue even(allocator, 4U, 64U, 4U);
    REQUIRE(even.ready());
    CHECK(even.share() == 1U);
    CHECK(even.push(1U, kClient, Ecn::NotEct, 2U));
    CHECK(even.held() == 0b0100U);
    for (u32 socket : {0U, 1U, 3U})
    {
        CHECK(even.push(1U, kClient, Ecn::NotEct, socket));
    }
    CHECK(even.full());
    CHECK(even.held() == 0b1111U);

    // One socket: every slot.
    SendQueue alone(allocator, 5U, 64U);
    REQUIRE(alone.ready());
    CHECK(alone.share() == 0U);
    for (u32 index = 0U; index < 5U; ++index)
    {
        CHECK(alone.push(1U, kClient, Ecn::NotEct, 0U));
    }
    CHECK(alone.held() == 0b1U);
    CHECK_FALSE(alone.push(1U, kClient, Ecn::NotEct, 0U));
}

TEST_CASE("A loop over two real UDP sockets answers on the one a datagram reached, and run() wakes on either",
          "[net][io-loop][net-path]")
{
    UdpOptions options;
    options.bind = Endpoint{IpAddress::v4(127, 0, 0, 1), 0U};
    SocketError error = SocketError::Failed;
    UdpSocket first = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    UdpSocket second = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    UdpSocket client = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    DatagramSocket* const both[] = {&first, &second};
    const SocketSet set{ConstSpan<DatagramSocket*>(both)};

    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 16U);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    Ingress ingress(IngressConfig{}, routes, buckets, nullptr, nullptr);
    RecordingHandler handler(routes);
    const HostClock clock;
    IoLoopConfig config;
    config.max_wait_ns = 30U * kSecond; // only a datagram ends a wait in time
    IoLoop loop(allocator, set, clock, ingress, handler, config);
    REQUIRE(loop.ready());
    CHECK_FALSE(set.wait_readable(20000000U)); // nothing waits: the time passes

    // The loop runs on its own thread; its first turn finds nothing, its second the datagram and stops.
    std::atomic<bool> stop{false};
    handler.stop = &stop;
    handler.stop_after = 2U;
    std::thread runner([&] { loop.run(stop); });
    while (handler.polled.load(std::memory_order_acquire) == 0U)
    {
        std::this_thread::yield(); // the first turn is over before the datagram leaves
    }
    const Datagram probe = unsupported_version();
    const u64 sent_at = clock.monotonic_ns();
    const OutgoingDatagram outgoing{probe.view(), second.local_endpoint()};
    REQUIRE(client.send(ConstSpan<OutgoingDatagram>(&outgoing, 1U)).count == 1U);
    REQUIRE(client.wait_readable(10U * kSecond));
    runner.join();
    CHECK(clock.monotonic_ns() - sent_at < 10U * kSecond);
    u8 storage[kDatagramBytes] = {};
    ReceivedDatagram slot;
    slot.storage = Span<u8>(storage, sizeof(storage));
    REQUIRE(client.receive(Span<ReceivedDatagram>(&slot, 1U)).count == 1U);
    CHECK(slot.peer == second.local_endpoint()); // answered from the socket it reached
    CHECK(loop.counters().answers_sent == 1U);
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
