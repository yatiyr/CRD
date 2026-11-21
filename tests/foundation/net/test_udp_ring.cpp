// crd-net -- the io_uring receive under Linux's UDP sockets (NET-PATH; docs/design/network-foundation.md#sockets):
// taken exactly where the host runs it, lent and copied slots, a ring that runs out of buffers, the threads that arm,
// wait on and close it, and on Linux the ICMP reports its lent slots hold ([.netns], router_namespace_fixture.hpp).
// Every other real-socket case runs over the ring too, and again without it (the crd-net-udp-recvmmsg CTest).

#include <crd/core/environment.hpp>
#include <crd/net/udp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#if CRD_OS_LINUX && !CRD_OS_ANDROID && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#define CRD_TEST_RING 1
#else
#define CRD_TEST_RING 0
#endif
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
using crd::net::Endpoint;
using crd::net::IpAddress;
using crd::net::OutgoingDatagram;
using crd::net::ReceivedDatagram;
using crd::net::SocketError;
using crd::net::SocketSet;
using crd::net::UdpOptions;
using crd::net::UdpSocket;

constexpr u64 kPatience = 2'000'000'000U;
constexpr Ecn kCodepoints[4] = {Ecn::NotEct, Ecn::Ect0, Ecn::Ect1, Ecn::Ce};

// Whether this host runs io_uring's multishot recvmsg for this process, read apart from crd-net: Linux 6.0 or later
// that sets a ring up here, and no CRD_NET_UDP_RING=0 in the environment.
bool host_runs_ring()
{
#if CRD_TEST_RING
    char value[4] = {};
    if (crd::read_environment("CRD_NET_UDP_RING", value, sizeof(value)) == crd::EnvironmentRead::Set &&
        std::strcmp(value, "0") == 0)
    {
        return false;
    }
    utsname name{};
    if (::uname(&name) != 0 || std::strtoul(name.release, nullptr, 10) < 6U)
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

UdpSocket open_with(const UdpOptions& base)
{
    UdpOptions options = base;
    options.bind = Endpoint{IpAddress::v4_loopback(), 0U};
    options.receive_buffer_bytes = 1U << 21U;
    options.send_buffer_bytes = 1U << 21U;
    SocketError error = SocketError::Failed;
    UdpSocket socket = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    return socket;
}

// A ring socket, or a skip on a host without one.
UdpSocket open_ring(const UdpOptions& options = UdpOptions{})
{
    UdpSocket socket = open_with(options);
    if (!socket.features().lent_receive)
    {
        SKIP("the host runs no io_uring receive");
    }
    return socket;
}

void pattern(Span<u8> bytes, u32 index)
{
    for (usize at = 0U; at < bytes.size(); ++at)
    {
        bytes[at] = static_cast<u8>((index * 31U) + (at * 7U));
    }
    for (usize at = 0U; at < 4U && at < bytes.size(); ++at)
    {
        bytes[at] = static_cast<u8>(index >> (8U * at));
    }
}

bool same_pattern(ConstSpan<u8> bytes, u32 index)
{
    u8 want[2048];
    if (bytes.size() > sizeof(want))
    {
        return false;
    }
    pattern(Span<u8>(want, bytes.size()), index);
    return std::memcmp(bytes.data(), want, bytes.size()) == 0;
}

u32 index_of(ConstSpan<u8> bytes)
{
    u32 index = 0U;
    for (usize at = 0U; at < 4U && at < bytes.size(); ++at)
    {
        index |= u32{bytes[at]} << (8U * at);
    }
    return index;
}

// Datagram i is 4 + i * 13 % 1400 bytes of pattern i, marked codepoint i % 4.
constexpr usize kBurst = 100U;
usize size_of(u32 index)
{
    return 4U + ((index * 13U) % 1400U);
}

// Sends datagrams `first` to `first + count`: true when every one left. No assertion runs here, nor in take_lent, so a
// thread of its own may call either.
bool send_burst(UdpSocket& from, const Endpoint& to, usize count = kBurst, u32 first = 0U)
{
    thread_local u8 bytes[kBurst][1404];
    thread_local OutgoingDatagram burst[kBurst];
    if (count > kBurst)
    {
        return false;
    }
    for (u32 index = 0U; index < count; ++index)
    {
        const u32 number = first + index;
        pattern(Span<u8>(bytes[index], size_of(number)), number);
        burst[index] = OutgoingDatagram{ConstSpan<u8>(bytes[index], size_of(number)), to, kCodepoints[number % 4U]};
    }
    usize sent = 0U;
    while (sent < count)
    {
        const BatchResult result = from.send(ConstSpan<OutgoingDatagram>(burst, count).subspan(sent));
        if (result.error != SocketError::None && result.error != SocketError::Busy)
        {
            return false;
        }
        sent += result.count;
    }
    return true;
}

// Receives `count` datagrams numbered from `first` through lent slots, `per_call` a receive, each checked before the
// next receive takes its buffer back: how many arrived in order and exactly as sent.
usize take_lent(UdpSocket& socket, const Endpoint& from, usize count, usize per_call, u32 first = 0U)
{
    ReceivedDatagram slots[64];
    usize got = 0U;
    while (got < count && per_call <= 64U && socket.wait_readable(kPatience))
    {
        const usize want = (count - got < per_call) ? count - got : per_call;
        const BatchResult result = socket.receive(Span<ReceivedDatagram>(slots, want));
        if (result.error != SocketError::None)
        {
            return got;
        }
        for (usize index = 0U; index < result.count; ++index)
        {
            const ReceivedDatagram& slot = slots[index];
            const u32 number = first + static_cast<u32>(got);
            const bool ecn = !socket.features().ecn_reading || slot.ecn == kCodepoints[number % 4U];
            if (slot.storage.size() != size_of(number) || slot.size != size_of(number) || slot.truncated ||
                slot.peer != from || slot.too_big != 0U || !ecn || index_of(slot.storage) != number ||
                !same_pattern(slot.storage, number))
            {
                return got;
            }
            slots[index].storage = Span<u8>(); // lent again by the next receive
            ++got;
        }
    }
    return got;
}

} // namespace

TEST_CASE("a UDP socket receives through io_uring exactly where the host runs it, and through recvmmsg elsewhere",
          "[net][udp][net-path]")
{
    const bool ring = host_runs_ring();
    const UdpSocket socket = open_with(UdpOptions{});
    CHECK(socket.features().lent_receive == ring);
    CHECK(socket.features().receive_bytes == (ring ? crd::net::kMaxUdpPayloadV6 : 0U));

    // A power of two from 4 to 32768 buffers; any other count keeps recvmmsg.
    for (const u32 buffers : {0U, 1U, 2U, 3U, 5U, 48U, 65536U})
    {
        INFO(buffers << " buffers");
        UdpOptions options;
        options.ring_buffers = buffers;
        CHECK_FALSE(open_with(options).features().lent_receive);
    }
    for (const u32 buffers : {4U, 8U, 1024U, 32768U})
    {
        INFO(buffers << " buffers");
        UdpOptions options;
        options.ring_buffers = buffers;
        CHECK(open_with(options).features().lent_receive == ring);
    }
    UdpOptions trains; // a coalescing socket's buffers each hold a whole train
    trains.coalesce_receive = true;
    const UdpSocket coalescing = open_with(trains);
    CHECK(coalescing.features().lent_receive == (ring && coalescing.features().coalesced_receive));
    if (coalescing.features().lent_receive)
    {
        CHECK(coalescing.features().receive_bytes == crd::net::kMaxTrainBytes);
    }
}

TEST_CASE("a ring socket lends its own buffers to empty slots, each holding its datagram until the next receive",
          "[net][udp][net-path]")
{
    UdpSocket receiver = open_ring();
    UdpSocket sender = open_with(UdpOptions{});
    REQUIRE(send_burst(sender, receiver.local_endpoint()));
    CHECK(take_lent(receiver, sender.local_endpoint(), kBurst, 64U) == kBurst);
    CHECK_FALSE(receiver.wait_readable(0U));

    // Lent and copied slots in one receive: a slot bringing 100 bytes of storage takes a copy cut to it.
    REQUIRE(send_burst(sender, receiver.local_endpoint(), 8U));
    static u8 storage[4][100];
    ReceivedDatagram slots[8];
    for (usize index = 0U; index < 4U; ++index)
    {
        slots[2U * index].storage = Span<u8>(storage[index], sizeof(storage[index]));
    }
    usize got = 0U;
    while (got < 8U && receiver.wait_readable(kPatience))
    {
        const BatchResult result = receiver.receive(Span<ReceivedDatagram>(slots, 8U).subspan(got));
        REQUIRE(result.error == SocketError::None);
        got += result.count;
    }
    REQUIRE(got == 8U);
    for (u32 index = 0U; index < 8U; ++index)
    {
        INFO("datagram " << index);
        const bool copied = (index % 2U) == 0U;
        const bool cut = copied && size_of(index) > 100U;
        const usize size = cut ? 100U : size_of(index);
        CHECK(slots[index].size == size);
        CHECK(slots[index].truncated == cut);
        CHECK((slots[index].storage.data() == storage[index / 2U]) == copied);
        CHECK(index_of(slots[index].storage) == index);
        CHECK(same_pattern(ConstSpan<u8>(slots[index].storage.data(), size), index));
    }

    // The largest IPv4 datagram arrives whole, lent or copied, and a slot shorter than it takes it cut.
    static u8 large[crd::net::kMaxUdpPayloadV4];
    for (usize at = 0U; at < sizeof(large); ++at)
    {
        large[at] = static_cast<u8>((at * 131U) >> 3U);
    }
    const OutgoingDatagram three[] = {{ConstSpan<u8>(large, sizeof(large)), receiver.local_endpoint()},
                                      {ConstSpan<u8>(large, sizeof(large)), receiver.local_endpoint()},
                                      {ConstSpan<u8>(large, sizeof(large)), receiver.local_endpoint()}};
    REQUIRE(sender.send(three).count == 3U);
    static u8 whole[crd::net::kMaxUdpPayloadV4];
    static u8 short_slot[1500];
    ReceivedDatagram big[3];
    big[1].storage = Span<u8>(whole, sizeof(whole));
    big[2].storage = Span<u8>(short_slot, sizeof(short_slot));
    got = 0U;
    while (got < 3U && receiver.wait_readable(kPatience))
    {
        got += receiver.receive(Span<ReceivedDatagram>(big, 3U).subspan(got)).count;
    }
    REQUIRE(got == 3U);
    for (usize index = 0U; index < 3U; ++index)
    {
        INFO("slot " << index);
        const usize size = (index == 2U) ? sizeof(short_slot) : sizeof(large);
        CHECK(big[index].size == size);
        CHECK(big[index].truncated == (index == 2U));
        CHECK(std::memcmp(big[index].storage.data(), large, size) == 0);
    }
}

TEST_CASE("a ring of four buffers receives a burst whole and in order however few or many slots each receive brings",
          "[net][udp][net-path]")
{
    UdpOptions options;
    options.ring_buffers = 4U;
    UdpSocket receiver = open_ring(options);
    UdpSocket sender = open_with(UdpOptions{});
    for (const usize per_call : {usize{1U}, usize{3U}, usize{4U}, usize{16U}, usize{64U}})
    {
        INFO(per_call << " slots a receive");
        REQUIRE(send_burst(sender, receiver.local_endpoint()));
        CHECK(take_lent(receiver, sender.local_endpoint(), kBurst, per_call) == kBurst);
        CHECK_FALSE(receiver.wait_readable(0U));
    }
}

TEST_CASE("a ring socket opened on one thread is read on another, after the first ends, and by threads in turn",
          "[net][udp][net-path]")
{
    UdpSocket sender = open_with(UdpOptions{});
    {
        // Opened here and read by a loop's own thread, which takes the receive over while this one stays blocked.
        UdpSocket receiver = open_ring();
        usize got = 0U;
        std::thread loop([&] { got = take_lent(receiver, sender.local_endpoint(), kBurst, 32U); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        REQUIRE(send_burst(sender, receiver.local_endpoint()));
        loop.join();
        CHECK(got == kBurst);
    }
    {
        // The thread that opened it has ended, and its receive with it.
        UdpSocket receiver;
        UdpOptions options;
        options.bind = Endpoint{IpAddress::v4_loopback(), 0U};
        SocketError error = SocketError::Failed;
        std::thread opener([&] { receiver = UdpSocket::open(options, error); });
        opener.join();
        REQUIRE(error == SocketError::None);
        REQUIRE(receiver.features().lent_receive);
        REQUIRE(send_burst(sender, receiver.local_endpoint()));
        CHECK(take_lent(receiver, sender.local_endpoint(), kBurst, 32U) == kBurst);
    }
    {
        // A job's executor: each receive on a thread of its own, every one ending before the next begins.
        UdpSocket receiver = open_ring();
        for (u32 turn = 0U; turn < 16U; ++turn)
        {
            REQUIRE(send_burst(sender, receiver.local_endpoint(), 5U, turn * 5U));
            usize got = 0U;
            std::thread job([&] { got = take_lent(receiver, sender.local_endpoint(), 5U, 2U, turn * 5U); });
            job.join();
            CHECK(got == 5U);
        }
        CHECK_FALSE(receiver.wait_readable(0U));
    }
}

TEST_CASE("a ring socket's wait wakes for a datagram that arrives after a receive, alone and in a set of two",
          "[net][udp][net-path]")
{
    UdpSocket first = open_ring();
    UdpSocket second = open_ring();
    UdpSocket sender = open_with(UdpOptions{});
    ReceivedDatagram slot;
    CHECK(first.receive(Span<ReceivedDatagram>(&slot, 1U)).count == 0U);
    bool left = false;
    std::thread late(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            left = send_burst(sender, first.local_endpoint(), 1U);
        });
    CHECK(first.wait_readable(kPatience));
    late.join();
    REQUIRE(left);
    CHECK(take_lent(first, sender.local_endpoint(), 1U, 1U) == 1U);

    crd::net::DatagramSocket* both[] = {&first, &second};
    const SocketSet set{ConstSpan<crd::net::DatagramSocket*>(both, 2U)};
    CHECK(second.receive(Span<ReceivedDatagram>(&slot, 1U)).count == 0U);
    CHECK_FALSE(set.wait_readable(20'000'000U));
    std::thread later(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            left = send_burst(sender, second.local_endpoint(), 1U, 1U);
        });
    CHECK(set.wait_readable(kPatience));
    later.join();
    REQUIRE(left);
    CHECK(take_lent(second, sender.local_endpoint(), 1U, 1U, 1U) == 1U);
    CHECK_FALSE(set.wait_readable(0U));
}

TEST_CASE("a ring socket moves, and closes from another thread while the thread that armed it waits",
          "[net][udp][net-path]")
{
    UdpSocket sender = open_with(UdpOptions{});
    UdpSocket opened = open_ring();
    const Endpoint was = opened.local_endpoint();
    UdpSocket moved(static_cast<UdpSocket&&>(opened));
    CHECK(moved.features().lent_receive);
    REQUIRE(send_burst(sender, was));
    CHECK(take_lent(moved, sender.local_endpoint(), 10U, 4U) == 10U);
    // Ninety datagrams still wait, some of them in the ring's buffers, as another thread closes it.
    std::atomic<bool> closed{false};
    std::thread closer(
        [&]
        {
            moved.close();
            closed.store(true);
        });
    closer.join();
    CHECK(closed.load());
    CHECK_FALSE(moved.is_open());
    // The port is free again, and a ring opens on it.
    UdpOptions again;
    again.bind = was;
    SocketError error = SocketError::Failed;
    const UdpSocket reopened = UdpSocket::open(again, error);
    CHECK(error == SocketError::None);
    CHECK(reopened.features().lent_receive);
}

#if CRD_OS_LINUX
TEST_CASE("a lent slot takes a router's Packet Too Big report in a buffer of the socket's own", "[.netns]")
{
    using crd::net::testing::RouterPath;
    RouterPath& path = RouterPath::get();
    UdpSocket near = path.open(RouterPath::Near, false);
    UdpSocket far = path.open(RouterPath::Far, false);
    if (!near.features().lent_receive)
    {
        SKIP("the host runs no io_uring receive");
    }
    static u8 payload[1400];
    pattern(Span<u8>(payload, sizeof(payload)), 11U);
    const OutgoingDatagram large[] = {{ConstSpan<u8>(payload, sizeof(payload)), far.local_endpoint()}};
    for (u32 round = 0U; round < 3U; ++round)
    {
        INFO("report " << round);
        REQUIRE(near.send(large).count == 1U);
        ReceivedDatagram slot;
        usize got = 0U;
        while (got == 0U && near.wait_readable(kPatience))
        {
            got = near.receive(Span<ReceivedDatagram>(&slot, 1U)).count;
        }
        REQUIRE(got == 1U);
        CHECK(slot.too_big == RouterPath::kHopMtu - 28U);
        CHECK(slot.peer == far.local_endpoint());
        CHECK(slot.size == 520U); // what the router quotes in 576 bytes of IPv4 (RFC 1812)
        CHECK_FALSE(slot.truncated);
        CHECK(same_pattern(ConstSpan<u8>(slot.storage.data(), slot.size), 11U));
    }
    CHECK_FALSE(near.wait_readable(100'000'000U));
}
#endif

#if CRD_TEST_RING
#include <ctime>
#include <cstdio>

namespace
{
// The receiving thread's CPU time, in nanoseconds.
u64 thread_cpu_ns()
{
    timespec now{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (static_cast<u64>(now.tv_sec) * 1'000'000'000U) + static_cast<u64>(now.tv_nsec);
}

// A sender thread blasts `count` datagrams of 1200 bytes; this thread receives them 32 a call, lent from a ring or
// into storage of its own: the receiver's CPU per datagram that arrived.
void board_round(bool ring, u32 count)
{
    UdpOptions options;
    options.ring_buffers = ring ? 256U : 0U;
    options.bind = Endpoint{IpAddress::v4_loopback(), 0U};
    options.receive_buffer_bytes = 8U << 20U;
    SocketError error = SocketError::Failed;
    UdpSocket receiver = UdpSocket::open(options, error);
    REQUIRE(receiver.features().lent_receive == ring);
    UdpSocket sender = open_with(UdpOptions{});
    static u8 storage[32][1200];
    ReceivedDatagram slots[32];
    std::atomic<bool> done{false};
    std::thread blaster(
        [&]
        {
            static u8 payload[1200];
            OutgoingDatagram burst[32];
            for (OutgoingDatagram& datagram : burst)
            {
                datagram = OutgoingDatagram{ConstSpan<u8>(payload, sizeof(payload)), receiver.local_endpoint()};
            }
            for (u32 sent = 0U; sent < count;)
            {
                sent += static_cast<u32>(sender.send(burst).count);
            }
            done.store(true);
        });
    const auto start = std::chrono::steady_clock::now();
    const u64 cpu_start = thread_cpu_ns();
    u64 received = 0U;
    while (received < count && (receiver.wait_readable(200'000'000U) || !done.load()))
    {
        for (usize index = 0U; index < 32U; ++index)
        {
            slots[index].storage = ring ? Span<u8>() : Span<u8>(storage[index], sizeof(storage[index]));
        }
        received += receiver.receive(slots).count;
    }
    const u64 cpu = thread_cpu_ns() - cpu_start;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    blaster.join();
    std::printf("%-9s received %7llu of %u in %.3f s, receiver %.0f ns CPU a datagram\n",
                ring ? "io_uring" : "recvmmsg", static_cast<unsigned long long>(received), count, wall,
                (received != 0U) ? static_cast<double>(cpu) / static_cast<double>(received) : 0.0);
}
} // namespace

TEST_CASE("board: the receiver's CPU a datagram through io_uring and through recvmmsg", "[.udp-board]")
{
    if (!host_runs_ring())
    {
        SKIP("the host runs no io_uring receive");
    }
    for (u32 round = 0U; round < 3U; ++round)
    {
        board_round(true, 400000U);
        board_round(false, 400000U);
    }
}
#endif
