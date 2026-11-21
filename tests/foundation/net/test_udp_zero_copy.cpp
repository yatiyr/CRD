// crd-net -- the zero-copy send (NET-PATH; docs/design/network-foundation.md#sockets): send_stable() hands Linux's
// io_uring a message's pages where the host has its zero-copy send, the socket numbers what it takes and says what the
// host released, and the host's copy reports turn it off over loopback until a probe. On Linux a dummy device behind a
// delaying qdisc sends in place: the pages stay the host's until it is done, a full socket is Busy at once, and a
// refused pin goes copied ([.netns]); the board compares the sender's CPU a byte both ways ([.udp-board]).

#include "ingress_fixture.hpp"

#include <crd/core/environment.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/udp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>

// NOLINTBEGIN(cppcoreguidelines-macro-usage): selects the zero-copy probe, the namespace case and the board
#if CRD_OS_LINUX && !CRD_OS_ANDROID && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(IORING_SEND_ZC_REPORT_USAGE) && defined(IORING_FEAT_REG_REG_RING)
#define CRD_TEST_ZERO_COPY 1
#endif
#endif
#ifndef CRD_TEST_ZERO_COPY
#define CRD_TEST_ZERO_COPY 0
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::BatchResult;
using crd::net::Endpoint;
using crd::net::IpAddress;
using crd::net::OutgoingDatagram;
using crd::net::ReceivedDatagram;
using crd::net::SocketError;
using crd::net::UdpOptions;
using crd::net::UdpSocket;

constexpr u64 kPatience = 2'000'000'000U;

// Whether this host runs io_uring's zero-copy send for this process, read apart from crd-net: Linux 6.3 or later
// that sets a ring up here, and no CRD_NET_UDP_RING=0 in the environment.
bool host_lends()
{
#if CRD_TEST_ZERO_COPY
    char value[4] = {};
    if (crd::read_environment("CRD_NET_UDP_RING", value, sizeof(value)) == crd::EnvironmentRead::Set &&
        std::strcmp(value, "0") == 0)
    {
        return false;
    }
    utsname name{};
    if (::uname(&name) != 0)
    {
        return false;
    }
    char* minor = nullptr;
    const unsigned long major = std::strtoul(name.release, &minor, 10);
    if (major < 6U || (major == 6U && std::strtoul(minor + 1, nullptr, 10) < 3U))
    {
        return false;
    }
    io_uring_params params{};
    const long ring = ::syscall(__NR_io_uring_setup, 4U, &params);
    if (ring < 0)
    {
        return false;
    }
    ::close(static_cast<int>(ring));
    return true;
#else
    return false;
#endif
}

UdpSocket open_at(const Endpoint& bind, u32 zero_copy_bytes, u32 send_buffer_bytes = 1U << 21U)
{
    UdpOptions options;
    options.bind = bind;
    options.receive_buffer_bytes = 1U << 21U;
    options.send_buffer_bytes = send_buffer_bytes;
    options.zero_copy_bytes = zero_copy_bytes;
    SocketError error = SocketError::Failed;
    UdpSocket socket = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    return socket;
}

// A loopback socket lending messages of `zero_copy_bytes` and more, or a skip on a host without the zero-copy send.
UdpSocket open_lending(u32 zero_copy_bytes)
{
    UdpSocket socket = open_at(Endpoint{IpAddress::v4_loopback(), 0U}, zero_copy_bytes);
    REQUIRE(socket.features().lent_send == host_lends());
    if (!socket.features().lent_send)
    {
        SKIP("the host runs no io_uring zero-copy send");
    }
    return socket;
}

void pattern(Span<u8> bytes, u32 index)
{
    for (usize at = 0U; at < bytes.size(); ++at)
    {
        bytes[at] = static_cast<u8>((index * 29U) + (at * 5U));
    }
}

bool same_pattern(ConstSpan<u8> bytes, u32 index)
{
    for (usize at = 0U; at < bytes.size(); ++at)
    {
        if (bytes[at] != static_cast<u8>((index * 29U) + (at * 5U)))
        {
            return false;
        }
    }
    return true;
}

// Receives `count` datagrams of `size` bytes numbered from `first`: how many arrived in order and exactly as sent.
usize take(UdpSocket& socket, usize count, usize size, u32 first)
{
    static u8 storage[32][1500];
    ReceivedDatagram slots[32];
    usize got = 0U;
    while (got < count && socket.wait_readable(kPatience))
    {
        for (usize index = 0U; index < 32U; ++index)
        {
            slots[index].storage = Span<u8>(storage[index], sizeof(storage[index]));
        }
        const BatchResult result = socket.receive(slots);
        for (usize index = 0U; index < result.count; ++index)
        {
            if (slots[index].size != size ||
                !same_pattern(ConstSpan<u8>(storage[index], size), first + static_cast<u32>(got)))
            {
                return got;
            }
            ++got;
        }
    }
    return got;
}

// Datagrams of `size` bytes in `storage`, patterned from `first`, toward `peer`.
void fill(Span<OutgoingDatagram> datagrams, u8 (*storage)[1500], usize size, u32 first, const Endpoint& peer)
{
    for (usize index = 0U; index < datagrams.size(); ++index)
    {
        pattern(Span<u8>(storage[index], size), first + static_cast<u32>(index));
        datagrams[index] = OutgoingDatagram{ConstSpan<u8>(storage[index], size), peer};
    }
}

} // namespace

TEST_CASE("a UDP socket lends sends exactly where the host has io_uring's zero-copy send, and never through send() "
          "or for a message under its size",
          "[net][udp][net-path]")
{
    const UdpSocket plain = open_at(Endpoint{IpAddress::v4_loopback(), 0U}, 0U);
    CHECK_FALSE(plain.features().lent_send); // a size of 0 copies every send
    UdpSocket sender = open_lending(4096U);
    UdpSocket receiver = open_at(Endpoint{IpAddress::v4_loopback(), 0U}, 0U);
    static u8 storage[32][1500];
    OutgoingDatagram train[20];
    fill(train, storage, 1200U, 0U, receiver.local_endpoint());
    // send() leaves the caller's bytes free at once: it copies, whatever the message's size.
    REQUIRE(sender.send(train).count == 20U);
    CHECK(sender.sends_taken() == 20U);
    CHECK(sender.sends_released(false) == 20U);
    // A stable message under the size copies too: two datagrams of one train, 2400 bytes.
    fill(Span<OutgoingDatagram>(train, 2U), storage, 1200U, 20U, receiver.local_endpoint());
    REQUIRE(sender.send_stable(ConstSpan<OutgoingDatagram>(train, 2U)).count == 2U);
    CHECK(sender.sends_taken() == 22U);
    CHECK(sender.sends_released(true) == 22U);
    const crd::net::OffloadCounters& counters = sender.offload_counters();
    CHECK(counters.zero_copy_sent + counters.zero_copy_copied + counters.zero_copy_refused == 0U);
    CHECK(take(receiver, 22U, 1200U, 0U) == 22U);
}

TEST_CASE("a zero-copy message reaches its peer whole, copies the host reports over loopback turn zero-copy off, and "
          "a probe tries it again",
          "[net][udp][net-path]")
{
    UdpSocket sender = open_lending(1024U);
    UdpSocket receiver = open_at(Endpoint{IpAddress::v4_loopback(), 0U}, 0U);
    const crd::net::OffloadCounters& counters = sender.offload_counters();
    static u8 storage[32][1500];
    OutgoingDatagram datagram[1];
    // Each message is the host's until its notification, which says loopback copied it: eight in a row turn the
    // zero-copy send off.
    for (u32 round = 0U; round < 8U; ++round)
    {
        INFO("message " << round);
        fill(datagram, storage, 1100U, round, receiver.local_endpoint());
        const BatchResult sent = sender.send_stable(datagram);
        INFO("error " << static_cast<int>(sent.error) << ", refused " << counters.zero_copy_refused);
        REQUIRE(sent.count == 1U);
        CHECK(sender.sends_released(true) == round + 1U);
        CHECK(counters.zero_copy_copied == round + 1U);
        CHECK(take(receiver, 1U, 1100U, round) == 1U);
    }
    CHECK(counters.zero_copy_sent == 0U);
    // Off: the next 1023 messages that want it copy, and the 1024th probes it again. Each arrives whole, read 32 at
    // a time so the receiver's buffer never overflows.
    for (u32 round = 8U; round < 8U + 1023U; ++round)
    {
        fill(datagram, storage, 1100U, round, receiver.local_endpoint());
        REQUIRE(sender.send_stable(datagram).count == 1U);
        if ((round - 8U) % 32U == 31U)
        {
            REQUIRE(take(receiver, 32U, 1100U, round - 31U) == 32U);
        }
    }
    CHECK(sender.sends_released(true) == 1031U);
    CHECK(counters.zero_copy_copied == 8U);
    fill(datagram, storage, 1100U, 1031U, receiver.local_endpoint());
    REQUIRE(sender.send_stable(datagram).count == 1U);
    CHECK(sender.sends_released(true) == 1032U);
    CHECK(counters.zero_copy_copied == 9U);
    CHECK(counters.zero_copy_refused == 0U);
    CHECK(take(receiver, 32U, 1100U, 1000U) == 32U); // 1000 to 1031
    // Moved, the socket keeps its numbering and its ring.
    UdpSocket moved = static_cast<UdpSocket&&>(sender);
    CHECK(moved.features().lent_send);
    CHECK(moved.sends_taken() == 1032U);
    CHECK(moved.sends_released(false) == 1032U);
}

#if CRD_TEST_ZERO_COPY
namespace
{
using crd::net::SendQueue;
using crd::net::SocketSet;

// A dummy device at 10.72.0.1/24 with a 1500-byte MTU, which sends every datagram in place and drops it: behind a
// token bucket of 500 kbit/s when `slow` (a qdisc that keeps each buffer charged to its socket until it leaves),
// behind a plain queue otherwise. False where this process may not make devices (outside a namespace of its own).
bool dummy_route(bool slow)
{
    static int made = -1;
    if (made < 0)
    {
        // NOLINTNEXTLINE(bugprone-command-processor): the test's own fixed ip commands, in its own namespace
        made = std::system("ip link add zc0 type dummy 2>/dev/null && ip link set zc0 mtu 1500 up && "
                           "ip addr add 10.72.0.1/24 dev zc0") == 0
                   ? 1
                   : 0;
    }
    // NOLINTNEXTLINE(bugprone-command-processor): the test's own fixed tc commands, in its own namespace
    return made == 1 && std::system(slow ? "tc qdisc replace dev zc0 root tbf rate 500kbit burst 1600 limit 8mb"
                                         : "tc qdisc replace dev zc0 root pfifo limit 100000") == 0;
}

u64 now_ns()
{
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const Endpoint kDummyPeer{IpAddress::v4(10, 72, 0, 2), 9U};
} // namespace

TEST_CASE("a device that sends in place holds a zero-copy train's pages until it is done, a full socket is Busy at "
          "once, and a message the host fails zero-copy goes again copied",
          "[.netns]")
{
    if (!dummy_route(true))
    {
        SKIP("this kernel makes no dummy device or token bucket in a namespace of the test's own");
    }
    UdpSocket sender = open_at(Endpoint{IpAddress::v4(10, 72, 0, 1), 0U}, 1024U, 1U << 16U);
    if (!sender.features().lent_send)
    {
        SKIP("the host runs no io_uring zero-copy send");
    }
    REQUIRE(sender.features().segmented_send);
    const crd::net::OffloadCounters& counters = sender.offload_counters();
    // Twelve datagrams end to end in pages of their own: one train of four pages.
    alignas(4096) static u8 whole[12U * 1200U];
    OutgoingDatagram train[12];
    for (u32 index = 0U; index < 12U; ++index)
    {
        pattern(Span<u8>(whole + (index * 1200U), 1200U), index);
        train[index] = OutgoingDatagram{ConstSpan<u8>(whole + (index * 1200U), 1200U), kDummyPeer};
    }

    // Held while the bucket sends it (14400 bytes at 500 kbit/s, about 230 ms), then given back with no copy.
    const u64 start = now_ns();
    REQUIRE(sender.send_stable(train).count == 12U);
    CHECK(counters.trains_sent == 1U);
    CHECK(sender.sends_taken() == 12U);
    CHECK(sender.sends_released(false) == 0U);
    CHECK(sender.sends_released(true) == 12U);
    CHECK(now_ns() - start >= 150'000'000U);
    CHECK(counters.zero_copy_sent == 1U);
    CHECK(counters.zero_copy_copied == 0U);

    // The held buffers fill the socket's: a send it cannot take is Busy within the call, every datagram before it
    // taken and numbered.
    u64 taken = sender.sends_taken();
    bool busy = false;
    for (u32 round = 0U; round < 200U && !busy; ++round)
    {
        const u64 before = now_ns();
        const BatchResult result = sender.send_stable(train);
        CHECK(now_ns() - before < 50'000'000U);
        taken += result.count;
        busy = result.error == SocketError::Busy;
        REQUIRE((result.error == SocketError::None || busy));
    }
    CHECK(busy);
    CHECK(sender.sends_taken() == taken);
    CHECK(sender.sends_released(false) < taken);
    CHECK(sender.sends_released(true) == taken);

    // A send queue over the socket keeps the slots the host holds, writing none of them, until the bucket has sent
    // them, and its end waits for the host.
    const u64 trains_held = counters.trains_sent;
    const u64 lent_held = counters.zero_copy_sent;
    {
        crd::net::testing::CountingAllocator allocator;
        SendQueue queue(allocator, 16U, 1200U);
        for (u32 index = 0U; index < 12U; ++index)
        {
            const Span<u8> room = queue.room(0U);
            REQUIRE(room.size() == 1200U);
            pattern(room, index);
            REQUIRE(queue.push(1200U, kDummyPeer));
        }
        CHECK(queue.flush(SocketSet(sender)).sent == 12U);
        CHECK(queue.lent() == 12U);
        CHECK(counters.trains_sent == trains_held + 1U); // the twelve slots lie end to end: one train
        for (u32 index = 0U; index < 4U; ++index)
        {
            REQUIRE(queue.push(1U, kDummyPeer));
        }
        CHECK(queue.full());
        CHECK(queue.room(0U).empty());
        static_cast<void>(queue.flush(SocketSet(sender)));
        CHECK(queue.lent() >= 12U);
    }
    CHECK(sender.sends_released(false) == sender.sends_taken());
    CHECK(counters.zero_copy_sent == lent_held + 1U); // sent in place; the four short ones were copied
    REQUIRE(dummy_route(false));

    // A train in slots 1500 bytes apart spans more pages than one of the host's buffers holds: it goes copied, as
    // one train still.
    static u8 storage[32][1500];
    OutgoingDatagram slots[16];
    fill(slots, storage, 1200U, 0U, kDummyPeer);
    const u64 lent_before = counters.zero_copy_sent;
    const u64 trains_before = counters.trains_sent;
    REQUIRE(sender.send_stable(slots).count == 16U);
    CHECK(sender.sends_released(true) == sender.sends_taken());
    CHECK(counters.zero_copy_sent == lent_before);
    CHECK(counters.trains_sent == trains_before + 1U);
    CHECK(counters.zero_copy_refused == 0U);

    // A datagram past the device's MTU fails zero-copy, goes again copied and fails there on its own terms.
    static u8 large[2000];
    const OutgoingDatagram oversized[] = {{ConstSpan<u8>(large, sizeof(large)), kDummyPeer}};
    const BatchResult refused = sender.send_stable(oversized);
    CHECK(refused.count == 0U);
    CHECK(refused.error == SocketError::MessageTooLarge);
    CHECK(counters.zero_copy_refused == 1U);
    CHECK(sender.features().lent_send);
    CHECK(sender.sends_released(true) == sender.sends_taken());
}

#include <cstdio>
#include <ctime>

namespace
{
u64 thread_cpu_ns()
{
    timespec now{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (static_cast<u64>(now.tv_sec) * 1'000'000'000U) + static_cast<u64>(now.tv_nsec);
}

// `messages` sends of `datagrams` datagrams of `size` bytes to the dummy device, lent at `zero_copy_bytes` and more
// (0: copied): the sender's CPU a kilobyte.
// `contiguous` lays the datagrams end to end in pages of their own, as a train written whole would lie; otherwise each
// starts 1500 bytes after the last, as a send queue's slots do.
void board_round(u32 zero_copy_bytes, usize datagrams, usize size, u32 messages, bool contiguous)
{
    UdpSocket sender = open_at(Endpoint{IpAddress::v4(10, 72, 0, 1), 0U}, zero_copy_bytes, 8U << 20U);
    REQUIRE(sender.features().lent_send == (zero_copy_bytes != 0U));
    static u8 storage[64][1500];
    alignas(4096) static u8 whole[64U * 1200U];
    OutgoingDatagram train[64];
    fill(Span<OutgoingDatagram>(train, datagrams), storage, size, 0U, kDummyPeer);
    for (usize index = 0U; contiguous && index < datagrams; ++index)
    {
        train[index].bytes = ConstSpan<u8>(whole + (index * size), size);
    }
    const u64 cpu_start = thread_cpu_ns();
    u64 bytes = 0U;
    for (u32 sent = 0U; sent < messages;)
    {
        const BatchResult result = sender.send_stable(ConstSpan<OutgoingDatagram>(train, datagrams));
        bytes += result.count * size;
        sent += (result.count == datagrams) ? 1U : 0U;
        static_cast<void>(sender.sends_released(result.error == SocketError::Busy));
    }
    static_cast<void>(sender.sends_released(true));
    const u64 cpu = thread_cpu_ns() - cpu_start;
    const crd::net::OffloadCounters& counters = sender.offload_counters();
    std::printf("%-9s %2zu x %4zu B %-10s: %7.1f ns CPU a KiB (%llu in place, %llu copied)\n",
                zero_copy_bytes != 0U ? "zero-copy" : "copied", datagrams, size, contiguous ? "contiguous" : "slots",
                static_cast<double>(cpu) * 1024.0 / static_cast<double>(bytes),
                static_cast<unsigned long long>(counters.zero_copy_sent),
                static_cast<unsigned long long>(counters.zero_copy_copied));
}
} // namespace

TEST_CASE("board: the sender's CPU a kilobyte through zero-copy and copied sends", "[.udp-board]")
{
    if (!host_lends() || !dummy_route(false))
    {
        SKIP("needs the zero-copy send and a namespace of its own (unshare -Urn)");
    }
    for (u32 round = 0U; round < 3U; ++round)
    {
        for (const usize datagrams : {usize{1U}, usize{8U}, usize{12U}, usize{54U}})
        {
            for (const bool contiguous : {false, true})
            {
                board_round(1U, datagrams, 1200U, 20000U, contiguous);
                board_round(0U, datagrams, 1200U, 20000U, contiguous);
            }
        }
    }
}
#endif
