// crd-net -- the host's TCP behind StreamPoller, on the loopback (4.2a; docs/design/network-foundation.md#stream-host),
// and the deadline heap every host's turn asks.

#include "ingress_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/net/deadline_heap.hpp>
#include <crd/net/stream_poller.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <random>

namespace
{
using namespace crd;
using namespace crd::net;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::testing::CountingAllocator;

constexpr u64 kWaitNs = 20'000'000U;
constexpr u32 kSlots = 4U;
constexpr usize kReports = 16U;

struct Listening
{
    CountingAllocator allocator;
    TcpListener listener;
    Endpoint address;

    Listening()
    {
        TcpListenOptions options;
        options.bind = Endpoint{IpAddress::v4_loopback(), 0U};
        SocketError error = SocketError::None;
        listener = TcpListener::open(options, error);
        REQUIRE(error == SocketError::None);
        address = listener.local_endpoint();
    }
};

// Waits until `token` is reported with one of `wanted`; the readiness, 0 when it never was.
u8 wait_for(TcpPoller& poller, u32 token, u8 wanted)
{
    for (usize turn = 0U; turn < 400U; ++turn)
    {
        StreamReady reports[kReports];
        const usize count = poller.wait(Span<StreamReady>(reports, kReports), kWaitNs);
        for (usize at = 0U; at < count; ++at)
        {
            if (reports[at].token == token && (reports[at].ready & wanted) != 0U)
            {
                return reports[at].ready;
            }
        }
    }
    return 0U;
}

struct Accepted
{
    Listening net;
    TcpPoller poller;
    SocketError error = SocketError::None;
    StreamSocket* client = nullptr;
    StreamSocket* server = nullptr;

    Accepted() : poller(net.allocator, kSlots, &net.listener)
    {
        REQUIRE(poller.ready());
        client = poller.connect(0U, net.address, error);
        REQUIRE(client != nullptr);
        REQUIRE(wait_for(poller, 0U, kStreamWritable) != 0U);
        REQUIRE(poller.connected(0U) == SocketError::None);
        poller.interest(0U, kStreamReadable);
        REQUIRE(wait_for(poller, kStreamListener, kStreamReadable) == kStreamReadable);
        server = poller.accept(1U, error);
        REQUIRE(server != nullptr);
        REQUIRE(error == SocketError::None);
    }
};

} // namespace

TEST_CASE("TcpPoller: a wait with nothing ready returns nothing, in time", "[net][tcp][poller]")
{
    Listening net;
    TcpPoller poller(net.allocator, kSlots, &net.listener);
    REQUIRE(poller.ready());
    StreamReady reports[kReports];
    CHECK(poller.wait(Span<StreamReady>(reports, kReports), 2'000'000U) == 0U);
}

TEST_CASE("TcpPoller: a host that only connects waits with an empty set", "[net][tcp][poller]")
{
    CountingAllocator allocator;
    TcpPoller poller(allocator, kSlots, nullptr);
    REQUIRE(poller.ready());
    StreamReady reports[kReports];
    CHECK(poller.wait(Span<StreamReady>(reports, kReports), 1'000'000U) == 0U);
    SocketError error = SocketError::None;
    CHECK(poller.accept(0U, error) == nullptr);
    CHECK(error == SocketError::Closed);
}

TEST_CASE("TcpPoller: slots and listeners are checked, and an allocator's refusal holds nothing", "[net][tcp][poller]")
{
    CountingAllocator allocator;
    CHECK_FALSE(TcpPoller(allocator, 0U, nullptr).ready());
    CHECK_FALSE(TcpPoller(allocator, kStreamMaxSlots + 1U, nullptr).ready());
    TcpListener closed;
    CHECK_FALSE(TcpPoller(allocator, kSlots, &closed).ready());
    bool made = false;
    for (usize refuse = 0U; refuse < 16U && !made; ++refuse)
    {
        CountingAllocator refusing;
        refusing.refuse_after(refuse);
        const TcpPoller poller(refusing, kSlots, nullptr);
        made = poller.ready();
        CHECK((made || refusing.calls() == refuse));
    }
    CHECK(made);
}

TEST_CASE("TcpPoller: a waiting connection is reported, accepted and carries bytes", "[net][tcp][poller]")
{
    Accepted pair;
    CHECK(pair.server->peer_endpoint().port == pair.client->local_endpoint().port);
    const u8 sent[3] = {1U, 2U, 3U};
    const StreamResult written = pair.client->write(ConstSpan<u8>(sent, 3U));
    REQUIRE(written.bytes == 3U);
    CHECK(wait_for(pair.poller, 1U, kStreamReadable) == kStreamReadable);
    u8 got[8] = {};
    const StreamResult read = pair.server->read(Span<u8>(got));
    REQUIRE(read.bytes == 3U);
    CHECK(std::memcmp(got, sent, 3U) == 0);

    // Nothing more waits on the listener or the socket.
    SocketError error = SocketError::None;
    CHECK(pair.poller.accept(2U, error) == nullptr);
    CHECK(error == SocketError::Busy);
    StreamReady reports[kReports];
    CHECK(pair.poller.wait(Span<StreamReady>(reports, kReports), 1'000'000U) == 0U);
}

TEST_CASE("TcpPoller: bytes left unread are reported again", "[net][tcp][poller]")
{
    Accepted pair;
    const u8 sent[2] = {7U, 8U};
    REQUIRE(pair.client->write(ConstSpan<u8>(sent, 2U)).bytes == 2U);
    REQUIRE(wait_for(pair.poller, 1U, kStreamReadable) != 0U);
    u8 got = 0U;
    REQUIRE(pair.server->read(Span<u8>(&got, 1U)).bytes == 1U);
    CHECK(wait_for(pair.poller, 1U, kStreamReadable) != 0U);
    REQUIRE(pair.server->read(Span<u8>(&got, 1U)).bytes == 1U);
    CHECK(got == 8U);
}

TEST_CASE("TcpPoller: a slot is reported for its interests only", "[net][tcp][poller]")
{
    Accepted pair;
    // Writable is never asked for on slot 1, and a socket with room would report it at once.
    pair.poller.interest(1U, kStreamReadable);
    StreamReady reports[kReports];
    CHECK(pair.poller.wait(Span<StreamReady>(reports, kReports), 2'000'000U) == 0U);
    pair.poller.interest(1U, static_cast<u8>(kStreamReadable | kStreamWritable));
    CHECK(wait_for(pair.poller, 1U, kStreamWritable) == kStreamWritable);
    // The same interests again change nothing, and a report stops when interest does.
    pair.poller.interest(1U, static_cast<u8>(kStreamReadable | kStreamWritable));
    pair.poller.interest(1U, kStreamReadable);
    CHECK(pair.poller.wait(Span<StreamReady>(reports, kReports), 2'000'000U) == 0U);
}

TEST_CASE("TcpPoller: the listener's interest pauses and resumes its reports", "[net][tcp][poller]")
{
    Listening net;
    TcpPoller poller(net.allocator, kSlots, &net.listener);
    REQUIRE(poller.ready());
    poller.interest(kStreamListener, 0U);
    SocketError error = SocketError::None;
    REQUIRE(poller.connect(0U, net.address, error) != nullptr);
    poller.interest(0U, kStreamReadable);
    StreamReady reports[kReports];
    CHECK(poller.wait(Span<StreamReady>(reports, kReports), 2'000'000U) == 0U);
    poller.interest(kStreamListener, kStreamReadable);
    CHECK(wait_for(poller, kStreamListener, kStreamReadable) == kStreamReadable);
}

TEST_CASE("TcpPoller: a socket that fills is reported writable once it drains", "[net][tcp][poller]")
{
    Accepted pair;
    const crd::containers::Array<u8> bytes = []
    {
        crd::containers::Array<u8> filled;
        filled.resize(64U * 1024U, u8{9U});
        return filled;
    }();
    usize total = 0U;
    for (usize turn = 0U; turn < 4096U; ++turn)
    {
        const StreamResult result = pair.client->write(ConstSpan<u8>(bytes.data(), bytes.size()));
        total += result.bytes;
        if (result.error == SocketError::Busy)
        {
            break;
        }
    }
    REQUIRE(total != 0U);
    pair.poller.interest(0U, kStreamWritable);
    pair.poller.interest(1U, 0U);
    StreamReady reports[kReports];
    CHECK(pair.poller.wait(Span<StreamReady>(reports, kReports), 2'000'000U) == 0U);

    crd::containers::Array<u8> sink;
    sink.resize(256U * 1024U, u8{0U});
    usize drained = 0U;
    u8 ready = 0U;
    for (usize turn = 0U; turn < 4096U && drained < total && ready == 0U; ++turn)
    {
        drained += pair.server->read(Span<u8>(sink.data(), sink.size())).bytes;
        const usize count = pair.poller.wait(Span<StreamReady>(reports, kReports), 1'000'000U);
        for (usize at = 0U; at < count; ++at)
        {
            if (reports[at].token == 0U)
            {
                ready = reports[at].ready;
            }
        }
    }
    CHECK(ready == kStreamWritable);
}

TEST_CASE("TcpPoller: the peer's end is reported, and read as the end", "[net][tcp][poller]")
{
    Accepted pair;
    pair.poller.close(0U, false);
    CHECK(wait_for(pair.poller, 1U, kStreamReadable) != 0U);
    u8 got[4] = {};
    CHECK(pair.server->read(Span<u8>(got)).ended);
}

TEST_CASE("TcpPoller: a reset is reported, and read as a reset", "[net][tcp][poller]")
{
    Accepted pair;
    pair.poller.close(0U, true);
    CHECK(wait_for(pair.poller, 1U, kStreamReadable) != 0U);
    u8 got[4] = {};
    const StreamResult result = pair.server->read(Span<u8>(got));
    CHECK_FALSE(result.ended);
    CHECK(result.error == SocketError::Reset);
}

TEST_CASE("TcpPoller: a connection refused is reported, and named by connected()", "[net][tcp][poller]")
{
    Endpoint address;
    {
        Listening gone;
        address = gone.address;
    }
    CountingAllocator allocator;
    TcpPoller poller(allocator, kSlots, nullptr);
    REQUIRE(poller.ready());
    SocketError error = SocketError::None;
    REQUIRE(poller.connect(2U, address, error) != nullptr);
    REQUIRE(wait_for(poller, 2U, kStreamWritable) != 0U);
    SocketError answer = poller.connected(2U);
    for (usize turn = 0U; turn < 200U && answer == SocketError::Busy; ++turn)
    {
        StreamReady reports[kReports];
        static_cast<void>(poller.wait(Span<StreamReady>(reports, kReports), kWaitNs));
        answer = poller.connected(2U);
    }
    CHECK(answer == SocketError::Refused);
    poller.close(2U, true);
}

TEST_CASE("TcpPoller: a closed slot is reused and only reports its new socket", "[net][tcp][poller]")
{
    Accepted pair;
    const u8 byte = 5U;
    REQUIRE(pair.client->write(ConstSpan<u8>(&byte, 1U)).bytes == 1U);
    REQUIRE(wait_for(pair.poller, 1U, kStreamReadable) != 0U);
    pair.poller.close(1U, true);
    SocketError error = SocketError::None;
    StreamSocket* const again = pair.poller.connect(1U, pair.net.address, error);
    REQUIRE(again != nullptr);
    CHECK(wait_for(pair.poller, 1U, kStreamWritable) != 0U);
    CHECK(pair.poller.connected(1U) == SocketError::None);
}

TEST_CASE("TcpPoller: a full report list starves nobody", "[net][tcp][poller]")
{
    Listening net;
    TcpPoller poller(net.allocator, 8U, &net.listener);
    REQUIRE(poller.ready());
    SocketError error = SocketError::None;
    StreamSocket* clients[3] = {};
    for (u32 index = 0U; index < 3U; ++index)
    {
        clients[index] = poller.connect(index, net.address, error);
        REQUIRE(clients[index] != nullptr);
        REQUIRE(wait_for(poller, index, kStreamWritable) != 0U);
        REQUIRE(poller.connected(index) == SocketError::None);
        poller.interest(index, kStreamReadable);
        REQUIRE(wait_for(poller, kStreamListener, kStreamReadable) != 0U);
        StreamSocket* const server = poller.accept(index + 4U, error);
        REQUIRE(server != nullptr);
        const u8 byte = 1U;
        REQUIRE(server->write(ConstSpan<u8>(&byte, 1U)).bytes == 1U);
    }
    bool seen[3] = {};
    for (usize turn = 0U; turn < 300U && !(seen[0] && seen[1] && seen[2]); ++turn)
    {
        StreamReady report;
        if (poller.wait(Span<StreamReady>(&report, 1U), kWaitNs) == 1U && report.token < 3U)
        {
            seen[report.token] = true;
        }
    }
    CHECK(seen[0]);
    CHECK(seen[1]);
    CHECK(seen[2]);
}

// ---- The heap ----

TEST_CASE("DeadlineHeap: holds to a model under random schedules", "[net][deadline-heap]")
{
    CountingAllocator allocator;
    constexpr u32 kCapacity = 64U;
    DeadlineHeap heap(allocator);
    REQUIRE(heap.init(kCapacity));
    CHECK_FALSE(heap.init(kCapacity));
    u64 model[kCapacity];
    for (u64& due : model)
    {
        due = kNeverNs;
    }
    std::mt19937 random(77U);
    for (usize step = 0U; step < 20000U; ++step)
    {
        const u32 index = static_cast<u32>(random() % kCapacity);
        const u32 action = random() % 8U;
        if (action < 5U)
        {
            const u64 due = random() % 1000U;
            heap.schedule(index, due);
            model[index] = due;
        }
        else if (action < 7U)
        {
            heap.unschedule(index);
            model[index] = kNeverNs;
        }
        else
        {
            const u64 now = random() % 1000U;
            u32 out[kCapacity];
            const u32 taken = heap.take_due(now, Span<u32>(out, kCapacity));
            for (u32 at = 0U; at < taken; ++at)
            {
                REQUIRE(model[out[at]] <= now);
                model[out[at]] = kNeverNs;
            }
            for (const u64 due : model)
            {
                REQUIRE((due == kNeverNs || due > now));
            }
        }
        u64 earliest = kNeverNs;
        u32 size = 0U;
        for (const u64 due : model)
        {
            earliest = (due < earliest) ? due : earliest;
            size += (due != kNeverNs) ? 1U : 0U;
        }
        REQUIRE(heap.next_due() == earliest);
        REQUIRE(heap.size() == size);
    }
}

TEST_CASE("DeadlineHeap: refusals hold nothing, and a take stops at its room", "[net][deadline-heap]")
{
    {
        CountingAllocator allocator;
        DeadlineHeap heap(allocator);
        CHECK_FALSE(heap.init(0U));
        allocator.refuse_after(1U);
        CHECK_FALSE(heap.init(4U));
    }
    CountingAllocator allocator;
    DeadlineHeap heap(allocator);
    REQUIRE(heap.init(4U));
    CHECK(heap.next_due() == kNeverNs);
    heap.schedule(0U, 5U);
    heap.schedule(1U, 3U);
    heap.schedule(2U, 4U);
    heap.schedule(1U, 9U); // moved later
    heap.unschedule(3U);   // absent
    u32 out[1];
    CHECK(heap.take_due(10U, Span<u32>(out, 1U)) == 1U);
    CHECK(out[0] == 2U);
    CHECK(heap.size() == 2U);
    CHECK(heap.take_due(1U, Span<u32>(out, 1U)) == 0U);
}
