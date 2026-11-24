// crd-net -- Windows' Registered I/O under UdpSocket (NET-PATH; docs/design/network-foundation.md#sockets): taken
// exactly where asked, lent and copied slots, codepoints and trains through its registered slots, what its posted
// receives hold and lose, its send limits and late failures, and the event waits a SocketSet mixes with a plain
// socket's. Each case skips where the host has no Registered I/O (every host but Windows). The QUIC hosts run over it
// in test_quic_client.cpp.

#include <crd/net/udp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: psapi.h uses its types; PSAPI_VERSION 2 resolves its call to kernel32's K32GetProcessMemoryInfo.
#include <psapi.h>
#endif

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::BatchResult;
using crd::net::DatagramSocket;
using crd::net::Ecn;
using crd::net::Endpoint;
using crd::net::IpAddress;
using crd::net::OutgoingDatagram;
using crd::net::ReceivedDatagram;
using crd::net::RingRefusal;
using crd::net::SocketError;
using crd::net::SocketSet;
using crd::net::UdpOptions;
using crd::net::UdpSocket;

constexpr u64 kPatience = 2'000'000'000U;
// What arrives for each codepoint sent: Windows refuses a sender's CE, which leaves unmarked.
constexpr Ecn kSent[4] = {Ecn::NotEct, Ecn::Ect0, Ecn::Ect1, Ecn::Ce};
constexpr Ecn kArrives[4] = {Ecn::NotEct, Ecn::Ect0, Ecn::Ect1, Ecn::NotEct};

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

// Buffers of 1500 bytes by default: each one's pages stay locked, so a case asks for any datagram only where it needs
// one.
UdpOptions registered(u32 buffers = 128U, u32 datagram_bytes = 1500U)
{
    UdpOptions options;
    options.registered_io = true;
    options.ring_buffers = buffers;
    options.ring_datagram_bytes = datagram_bytes;
    return options;
}

// A Registered I/O socket, or a skip on a host without it.
UdpSocket open_rio(const UdpOptions& options = registered())
{
#if !CRD_OS_WINDOWS
    SKIP("Registered I/O is Windows'"); // elsewhere registered_io changes nothing and the ring receives
#endif
    UdpSocket socket = open_with(options);
    if (!socket.features().lent_receive)
    {
        SKIP("the host runs no Registered I/O");
    }
    return socket;
}

// Datagram i: 4 + i * 13 % 1400 bytes, its number in the first four and a pattern behind, marked kSent[i % 4].
usize size_of(u32 index)
{
    return 4U + ((index * 13U) % 1400U);
}

void pattern(Span<u8> bytes, u32 index)
{
    for (usize at = 0U; at < bytes.size(); ++at)
    {
        bytes[at] = static_cast<u8>((index * 31U) + (at * 7U));
    }
    std::memcpy(bytes.data(), &index, (bytes.size() < 4U) ? bytes.size() : 4U);
}

bool matches(const ReceivedDatagram& slot, u32 index, const Endpoint& from, bool ecn_reading)
{
    u8 want[1404];
    pattern(Span<u8>(want, size_of(index)), index);
    return slot.size == size_of(index) && !slot.truncated && slot.peer == from && slot.segment == 0U &&
           (!ecn_reading || slot.ecn == kArrives[index % 4U]) && std::memcmp(slot.storage.data(), want, slot.size) == 0;
}

// Sends datagrams `first` to `first + count` (at most 200), retrying while the sender is busy: true when all left.
bool send_burst(UdpSocket& from, const Endpoint& to, usize count, u32 first = 0U)
{
    static u8 bytes[200][1404];
    static OutgoingDatagram burst[200];
    for (u32 index = 0U; index < count; ++index)
    {
        const u32 number = first + index;
        pattern(Span<u8>(bytes[index], size_of(number)), number);
        burst[index] = OutgoingDatagram{ConstSpan<u8>(bytes[index], size_of(number)), to, kSent[number % 4U]};
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

// Receives `count` datagrams numbered from `first`, `per_call` slots a receive, lent where `lent` and otherwise
// copied into storage of its own: how many arrived in order and as sent.
usize take(UdpSocket& socket, const Endpoint& from, usize count, usize per_call, bool lent, u32 first = 0U)
{
    static u8 storage[64][1500];
    ReceivedDatagram slots[64];
    usize got = 0U;
    while (got < count && socket.wait_readable(kPatience))
    {
        const usize want = (count - got < per_call) ? count - got : per_call;
        for (usize index = 0U; index < want; ++index)
        {
            slots[index] = ReceivedDatagram{};
            slots[index].storage = lent ? Span<u8>() : Span<u8>(storage[index], sizeof(storage[index]));
        }
        const BatchResult result = socket.receive(Span<ReceivedDatagram>(slots, want));
        if (result.error != SocketError::None)
        {
            return got;
        }
        for (usize index = 0U; index < result.count; ++index)
        {
            if (!matches(slots[index], first + static_cast<u32>(got), from, socket.features().ecn_reading))
            {
                return got;
            }
            ++got;
        }
    }
    return got;
}

} // namespace

TEST_CASE("a UDP socket goes through Registered I/O exactly where asked, with buffers of the size it names",
          "[net][udp][net-path]")
{
    CHECK(open_with(UdpOptions{}).features().send_bytes == 0U); // off unless asked
    UdpSocket socket = open_rio(registered(16U, 0U));
    CHECK(socket.features().receive_bytes == crd::net::kMaxUdpPayloadV6);
    CHECK(socket.features().send_bytes == crd::net::kMaxUdpPayloadV6);
    CHECK(socket.features().ecn_reading);
    CHECK(socket.wait_handle() != crd::net::kNoWaitHandle);

    CHECK(socket.features().ring_refusal == RingRefusal::None);

    // A power of two from 4 to 32768 buffers; any other count keeps the plain path, open all the same, and says why.
    for (const u32 buffers : {0U, 1U, 3U, 48U, 65536U})
    {
        INFO(buffers << " buffers");
        const UdpSocket plain = open_with(registered(buffers));
        CHECK(plain.is_open());
        CHECK_FALSE(plain.features().lent_receive);
        CHECK(plain.features().send_bytes == 0U);
        CHECK(plain.features().ring_refusal == ((buffers == 0U) ? RingRefusal::None : RingRefusal::Count));
        CHECK(plain.features().ring_error == 0);
        CHECK(plain.features().ring_buffers == 0U);
    }
    // Every valid count lends; the host's limits may give the largest fewer buffers, naming why.
    for (const u32 buffers : {4U, 1024U, 32768U})
    {
        const UdpSocket ring = open_with(registered(buffers));
        const u32 granted = ring.features().ring_buffers;
        INFO(buffers << " buffers, refused at step " << static_cast<int>(ring.features().ring_refusal) << ", error "
                     << ring.features().ring_error << ", granted " << granted);
        CHECK(ring.features().lent_receive);
        CHECK((ring.features().ring_refusal == RingRefusal::None) == (granted == buffers));
        CHECK((granted == buffers || (buffers > 1024U && granted >= 4U && (granted & (granted - 1U)) == 0U)));
    }
    const UdpSocket sized = open_with(registered(64U, 1500U));
    CHECK(sized.features().receive_bytes == 1500U);
    CHECK(sized.features().send_bytes == 1500U);
    UdpOptions trains = registered(16U, 1500U); // a coalescing socket's buffers each hold a whole train
    trains.coalesce_receive = true;
    const UdpSocket coalescing = open_with(trains);
    if (coalescing.features().coalesced_receive)
    {
        CHECK(coalescing.features().receive_bytes == crd::net::kMaxTrainBytes);
    }
}

#if CRD_OS_WINDOWS
namespace
{
// This process's commit limited to its commit now and `room` past it for one scope, through a job of its own (one
// nested in any job it runs in), and unlimited again after, whatever the scope throws.
class CommitRoom
{
public:
    explicit CommitRoom(usize room)
    {
        PROCESS_MEMORY_COUNTERS counters{};
        REQUIRE(::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) != FALSE);
        m_job = ::CreateJobObjectW(nullptr, nullptr);
        REQUIRE(m_job != nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limit.ProcessMemoryLimit = counters.PagefileUsage + room;
        m_limited = ::SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limit, sizeof(limit)) &&
                    ::AssignProcessToJobObject(m_job, ::GetCurrentProcess());
    }
    ~CommitRoom()
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION none{};
        static_cast<void>(::SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &none, sizeof(none)));
        static_cast<void>(::CloseHandle(m_job)); // the process stays in it, unlimited, until it ends
    }
    CommitRoom(const CommitRoom&) = delete;
    CommitRoom& operator=(const CommitRoom&) = delete;
    CommitRoom(CommitRoom&&) = delete;
    CommitRoom& operator=(CommitRoom&&) = delete;

    [[nodiscard]] bool limited() const noexcept
    {
        return m_limited;
    }

private:
    HANDLE m_job = nullptr;
    bool m_limited = false;
};

// A socket asking for 32768 Registered I/O buffers of a whole datagram, over 2 GiB with its 72 send slots, opened
// with `room` bytes of commit left.
UdpSocket open_with_commit_room(usize room)
{
    UdpSocket socket;
    bool limited = false;
    {
        const CommitRoom limit(room);
        limited = limit.limited();
        socket = limited ? open_with(registered(32768U, 0U)) : UdpSocket{};
    }
    REQUIRE(limited);
    return socket;
}
} // namespace
#endif

TEST_CASE("a Registered I/O ring past the commit left is asked again with half its buffers, down to 4, or refused",
          "[net][udp][net-path]")
{
    static_cast<void>(open_rio());
#if CRD_OS_WINDOWS
    UdpSocket sender = open_with(UdpOptions{});
    SECTION("256 MiB holds 2048 buffers of a whole datagram: Memory, then half the buffers until they fit")
    {
        UdpSocket socket = open_with_commit_room(usize{256U} << 20U);
        const u32 granted = socket.features().ring_buffers;
        INFO("refused at step " << static_cast<int>(socket.features().ring_refusal) << ", error "
                                << socket.features().ring_error << ", granted " << granted);
        CHECK(socket.features().lent_receive);
        CHECK(socket.features().ring_refusal == RingRefusal::Memory);
        CHECK(socket.features().ring_error != 0);
        CHECK((granted >= 4U && granted <= 2048U && (granted & (granted - 1U)) == 0U));
        REQUIRE(send_burst(sender, socket.local_endpoint(), 8U));
        CHECK(take(socket, sender.local_endpoint(), 8U, 8U, true) == 8U);
    }
    SECTION("1 MiB holds not even 4 buffers and the send slots: the first refusal, Memory, and the plain path")
    {
        UdpSocket socket = open_with_commit_room(usize{1U} << 20U);
        INFO("refused at step " << static_cast<int>(socket.features().ring_refusal) << ", error "
                                << socket.features().ring_error);
        CHECK_FALSE(socket.features().lent_receive);
        CHECK(socket.features().ring_buffers == 0U);
        CHECK(socket.features().send_bytes == 0U);
        CHECK(socket.features().ring_refusal == RingRefusal::Memory);
        CHECK(socket.features().ring_error != 0);
        REQUIRE(send_burst(sender, socket.local_endpoint(), 8U));
        CHECK(take(socket, sender.local_endpoint(), 8U, 8U, false) == 8U);
    }
    // The limit gone, the same count lends whole.
    const UdpSocket again = open_with(registered(1024U));
    CHECK(again.features().ring_buffers == 1024U);
    CHECK(again.features().ring_refusal == RingRefusal::None);
#endif
}

TEST_CASE("a Registered I/O socket lends its buffers to empty slots and copies into full ones, in order and whole",
          "[net][udp][net-path]")
{
    // Room for a burst beside the buffers 64 lent slots hold: a datagram that finds none posted is lost.
    UdpSocket receiver = open_rio(registered(256U));
    UdpSocket sender = open_rio();
    UdpSocket plain = open_with(UdpOptions{});
    for (const bool lent : {true, false})
    {
        for (const usize per_call : {usize{1U}, usize{7U}, usize{64U}})
        {
            INFO((lent ? "lent, " : "copied, ") << per_call << " slots a receive");
            // From a Registered I/O sender and from a plain one, each codepoint carried both ways.
            REQUIRE(send_burst(sender, receiver.local_endpoint(), 100U));
            CHECK(take(receiver, sender.local_endpoint(), 100U, per_call, lent) == 100U);
            REQUIRE(send_burst(plain, receiver.local_endpoint(), 100U));
            CHECK(take(receiver, plain.local_endpoint(), 100U, per_call, lent) == 100U);
            REQUIRE(send_burst(sender, plain.local_endpoint(), 100U));
            CHECK(take(plain, sender.local_endpoint(), 100U, per_call, false) == 100U);
            CHECK_FALSE(receiver.wait_readable(0U));
        }
    }
    CHECK(sender.offload_counters().sends_failed == 0U);
}

TEST_CASE("a Registered I/O socket's posted buffers are all it holds: a burst past them is lost, the rest is whole",
          "[net][udp][net-path]")
{
    UdpSocket receiver = open_rio(registered(4U));
    UdpSocket sender = open_with(UdpOptions{});
    REQUIRE(send_burst(sender, receiver.local_endpoint(), 20U));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(take(receiver, sender.local_endpoint(), 4U, 64U, false) == 4U);
    CHECK_FALSE(receiver.wait_readable(20'000'000U)); // the other sixteen arrived while no receive was posted
    // Each buffer went back as its datagram was read: the next ones arrive.
    REQUIRE(send_burst(sender, receiver.local_endpoint(), 4U, 20U));
    CHECK(take(receiver, sender.local_endpoint(), 4U, 2U, true, 20U) == 4U);
}

TEST_CASE("a Registered I/O socket cuts a datagram past its buffers and refuses to send one past its slots",
          "[net][udp][net-path]")
{
    UdpSocket receiver = open_rio(registered(8U, 1200U));
    UdpSocket sender = open_with(UdpOptions{});
    static u8 large[1300];
    pattern(Span<u8>(large, sizeof(large)), 7U);
    const OutgoingDatagram datagram{ConstSpan<u8>(large, sizeof(large)), receiver.local_endpoint()};
    REQUIRE(sender.send(ConstSpan<OutgoingDatagram>(&datagram, 1U)).count == 1U);
    REQUIRE(receiver.wait_readable(kPatience));
    ReceivedDatagram slot;
    REQUIRE(receiver.receive(Span<ReceivedDatagram>(&slot, 1U)).count == 1U);
    CHECK(slot.truncated);
    CHECK(slot.size == 1200U);
    CHECK(std::memcmp(slot.storage.data(), large, 1200U) == 0);
    // Copied into a shorter slot, it is cut there.
    REQUIRE(sender.send(ConstSpan<OutgoingDatagram>(&datagram, 1U)).count == 1U);
    REQUIRE(receiver.wait_readable(kPatience));
    u8 short_slot[100];
    slot = ReceivedDatagram{};
    slot.storage = Span<u8>(short_slot, sizeof(short_slot));
    REQUIRE(receiver.receive(Span<ReceivedDatagram>(&slot, 1U)).count == 1U);
    CHECK(slot.truncated);
    CHECK(slot.size == sizeof(short_slot));

    // A send slot holds 1200 bytes: a longer datagram fails at once, and those before it left.
    const OutgoingDatagram fits{ConstSpan<u8>(large, 1200U), sender.local_endpoint()};
    const OutgoingDatagram both[2] = {fits, datagram};
    const BatchResult result = receiver.send(ConstSpan<OutgoingDatagram>(both, 2U));
    CHECK(result.count == 1U);
    CHECK(result.error == SocketError::MessageTooLarge);
    REQUIRE(sender.wait_readable(kPatience));
}

TEST_CASE("a Registered I/O socket sends a train through USO from one slot and counts a send the host fails late",
          "[net][udp][net-path]")
{
    UdpSocket sender = open_rio(registered(16U, 0U));
    UdpSocket receiver = open_with(UdpOptions{});
    if (!sender.features().segmented_send)
    {
        SKIP("the host segments no train");
    }
    static u8 bytes[40][1000];
    OutgoingDatagram train[40];
    for (u32 index = 0U; index < 40U; ++index)
    {
        pattern(Span<u8>(bytes[index], (index == 39U) ? 700U : 1000U), index);
        train[index] = OutgoingDatagram{ConstSpan<u8>(bytes[index], (index == 39U) ? 700U : 1000U),
                                        receiver.local_endpoint(), Ecn::Ect1};
    }
    REQUIRE(sender.send(ConstSpan<OutgoingDatagram>(train, 40U)).count == 40U);
    CHECK(sender.offload_counters().trains_sent == 1U);
    CHECK(sender.offload_counters().train_datagrams_sent == 40U);
    static u8 storage[40][1500];
    ReceivedDatagram slots[40];
    for (usize index = 0U; index < 40U; ++index)
    {
        slots[index].storage = Span<u8>(storage[index], sizeof(storage[index]));
    }
    usize got = 0U;
    while (got < 40U && receiver.wait_readable(kPatience))
    {
        got += receiver.receive(Span<ReceivedDatagram>(slots, 40U).subspan(got)).count;
    }
    REQUIRE(got == 40U);
    for (u32 index = 0U; index < 40U; ++index)
    {
        INFO("datagram " << index);
        CHECK(slots[index].size == ((index == 39U) ? 700U : 1000U));
        CHECK(slots[index].ecn == Ecn::Ect1);
        CHECK(std::memcmp(slots[index].storage.data(), bytes[index], slots[index].size) == 0);
    }

    // A datagram the host cannot send (off a loopback-bound socket, and past any link's MTU) fails only as it
    // completes: the call took it, and a later one counts it.
    static u8 large[32000];
    const OutgoingDatagram away{ConstSpan<u8>(large), Endpoint{IpAddress::v4(192, 0, 2, 1), 9U}}; // TEST-NET-1
    REQUIRE(sender.send(ConstSpan<OutgoingDatagram>(&away, 1U)).count == 1U);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sender.offload_counters().sends_failed == 0U && std::chrono::steady_clock::now() < deadline)
    {
        CHECK(sender.send(ConstSpan<OutgoingDatagram>()).error == SocketError::None);
        std::this_thread::yield();
    }
    CHECK(sender.offload_counters().sends_failed == 1U);
    CHECK(sender.offload_counters().trains_refused == 0U);

    // A train past any link's MTU, from a socket on every address, fails as it completes as each datagram would:
    // both datagrams counted, the train refused, and segmenting stays on (the train's own refusal, not the host's).
    UdpOptions any = registered(16U, 0U);
    any.bind = Endpoint{IpAddress::v4_any(), 0U};
    SocketError error = SocketError::Failed;
    UdpSocket wide = UdpSocket::open(any, error);
    REQUIRE(error == SocketError::None);
    REQUIRE(wide.features().segmented_send);
    const OutgoingDatagram both[2] = {away, away};
    REQUIRE(wide.send(ConstSpan<OutgoingDatagram>(both, 2U)).count == 2U);
    CHECK(wide.offload_counters().trains_sent == 1U);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (wide.offload_counters().trains_refused == 0U && std::chrono::steady_clock::now() < until)
    {
        CHECK(wide.send(ConstSpan<OutgoingDatagram>()).error == SocketError::None);
        std::this_thread::yield();
    }
    CHECK(wide.offload_counters().trains_refused == 1U);
    CHECK(wide.offload_counters().sends_failed == 2U);
    CHECK(wide.features().segmented_send);
}

TEST_CASE("Registered I/O and plain sockets wait on events, alone and mixed in one set, and wake for what arrives",
          "[net][udp][net-path]")
{
    UdpSocket rio = open_rio();
    UdpSocket plain = open_with(UdpOptions{});
    UdpSocket sender = open_with(UdpOptions{});
    CHECK_FALSE(rio.wait_readable(20'000'000U));
    CHECK_FALSE(plain.wait_readable(20'000'000U));
    DatagramSocket* both[] = {&rio, &plain};
    const SocketSet set{ConstSpan<DatagramSocket*>(both, 2U)};
    CHECK_FALSE(set.wait_readable(20'000'000U));
    u32 next = 0U;
    for (UdpSocket* target : {&rio, &plain, &rio})
    {
        bool left = false;
        std::thread late(
            [&, target, next]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                left = send_burst(sender, target->local_endpoint(), 3U, next);
            });
        CHECK(set.wait_readable(kPatience));
        late.join();
        REQUIRE(left);
        // One receive takes one of three: what is left wakes the next wait at once. Once all are taken, a receive
        // that finds nothing clears a plain socket's event, which its last read may have set again (udp.hpp).
        CHECK(take(*target, sender.local_endpoint(), 1U, 1U, target == &rio, next) == 1U);
        CHECK(set.wait_readable(0U));
        // A receive of no slot reads nothing and leaves the wait as it was.
        CHECK(target->receive(Span<ReceivedDatagram>()).count == 0U);
        CHECK(target->wait_readable(0U));
        CHECK(take(*target, sender.local_endpoint(), 2U, 2U, false, next + 1U) == 2U);
        u8 spare[64];
        ReceivedDatagram empty;
        empty.storage = Span<u8>(spare, sizeof(spare));
        CHECK(target->receive(Span<ReceivedDatagram>(&empty, 1U)).count == 0U);
        CHECK_FALSE(set.wait_readable(20'000'000U));
        next += 3U;
    }
}

TEST_CASE("a Registered I/O socket moves with its queues, and its port is free once it closes", "[net][udp][net-path]")
{
    UdpSocket sender = open_with(UdpOptions{});
    UdpSocket opened = open_rio();
    const Endpoint was = opened.local_endpoint();
    UdpSocket moved(static_cast<UdpSocket&&>(opened));
    CHECK_FALSE(opened.is_open());
    CHECK(moved.features().lent_receive);
    REQUIRE(send_burst(sender, was, 50U));
    CHECK(take(moved, sender.local_endpoint(), 10U, 4U, true) == 10U);
    UdpSocket assigned;
    assigned = static_cast<UdpSocket&&>(moved);
    CHECK(take(assigned, sender.local_endpoint(), 10U, 4U, false, 10U) == 10U);
    assigned.close(); // thirty datagrams still wait in its buffers
    CHECK_FALSE(assigned.is_open());
    UdpOptions again = registered();
    again.bind = was;
    SocketError error = SocketError::Failed;
    const UdpSocket reopened = UdpSocket::open(again, error);
    CHECK(error == SocketError::None);
    CHECK(reopened.features().lent_receive);
}

#if CRD_OS_WINDOWS
namespace
{
// The calling thread's CPU cycles: a quantum-free count, where its CPU time moves a scheduler tick at a time.
u64 thread_cycles()
{
    ULONG64 cycles = 0U;
    static_cast<void>(::QueryThreadCycleTime(::GetCurrentThread(), &cycles));
    return cycles;
}

// A sender thread blasts `count` datagrams of 1200 bytes, 32 a call; this thread receives them 32 a call, lent from
// posted buffers or into storage of its own: each side's cycles per datagram, both through Registered I/O or both
// through WSASendMsg and WSARecvMsg.
void board_round(bool rio, u32 count)
{
    UdpOptions options = rio ? registered(4096U, 1500U) : UdpOptions{};
    options.receive_buffer_bytes = 8U << 20U;
    UdpSocket receiver = open_with(options);
    UdpSocket sender = open_with(rio ? registered(64U, 1500U) : UdpOptions{});
    REQUIRE(receiver.features().lent_receive == rio);
    static u8 storage[32][1200];
    ReceivedDatagram slots[32];
    std::atomic<bool> done{false};
    u64 sender_cycles = 0U;
    std::thread blaster(
        [&]
        {
            static u8 payload[1200];
            OutgoingDatagram burst[32];
            for (OutgoingDatagram& datagram : burst)
            {
                datagram = OutgoingDatagram{ConstSpan<u8>(payload, sizeof(payload)), receiver.local_endpoint()};
            }
            const u64 begin = thread_cycles();
            for (u32 sent = 0U; sent < count;)
            {
                sent += static_cast<u32>(sender.send(burst).count);
            }
            sender_cycles = thread_cycles() - begin;
            done.store(true);
        });
    const auto start = std::chrono::steady_clock::now();
    const u64 begin = thread_cycles();
    u64 received = 0U;
    while (received < count && (receiver.wait_readable(200'000'000U) || !done.load()))
    {
        for (usize index = 0U; index < 32U; ++index)
        {
            slots[index].storage = rio ? Span<u8>() : Span<u8>(storage[index], sizeof(storage[index]));
        }
        received += receiver.receive(slots).count;
    }
    const u64 cycles = thread_cycles() - begin;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    blaster.join();
    std::printf("%-10s received %7llu of %u in %.3f s; cycles a datagram: receiver %.0f, sender %.0f\n",
                rio ? "RIO" : "WSA*Msg", static_cast<unsigned long long>(received), count, wall,
                (received != 0U) ? static_cast<double>(cycles) / static_cast<double>(received) : 0.0,
                static_cast<double>(sender_cycles) / static_cast<double>(count));
}
} // namespace

TEST_CASE("board: each side's cycles a datagram through Registered I/O and through WSASendMsg and WSARecvMsg",
          "[.udp-board]")
{
    for (u32 round = 0U; round < 3U; ++round)
    {
        board_round(true, 400000U);
        board_round(false, 400000U);
    }
}
#endif
