// crd-net -- Windows' Registered I/O under a UdpSocket (NET-PATH; docs/design/network-foundation.md#sockets).

#include "udp_rio.hpp"

#if CRD_OS_WINDOWS
#include <crd/vm/virtual_memory.hpp>

#include <cstring>
#include <new>

namespace crd::net
{
namespace
{
// The three kinds of slot (pools), in this order in the pages and in the slot numbers.
constexpr u32 kReceives = 0U;
constexpr u32 kSends = 1U;
constexpr u32 kTrains = 2U;
constexpr u32 kPools = 3U;
} // namespace

// The queues and their bookkeeping in the first bytes of its own pages; the registrations, the lent and free lists and
// the slots of each pool follow, each slot starting on a cache line with its datagram, peer and control messages.
struct UdpRio
{
    vm::VmRegion region;
    RIO_EXTENSION_FUNCTION_TABLE table{};
    HANDLE event = nullptr;
    RIO_CQ receive_queue = RIO_INVALID_CQ;
    RIO_CQ send_queue = RIO_INVALID_CQ;
    RIO_RQ requests = RIO_INVALID_RQ;
    RIO_BUFFERID* ids = nullptr; // one registration per slot, by slot number
    u32* sent = nullptr;         // per send slot in flight: its datagrams, and kMarked where it carries a codepoint
    u32* lent = nullptr;         // receive buffers the last receive() lent
    u32* free[kPools] = {};      // each send pool's slots not in flight, a stack (kReceives' is the lent list)
    u8* base[kPools] = {};
    u32 first[kPools] = {}; // each pool's first slot number
    u32 count[kPools] = {};
    u32 stride[kPools] = {}; // a slot's bytes rounded up to a cache line, where the next one starts
    u32 bytes[kPools] = {};  // what a slot's datagram holds, its peer and control messages behind it
    u32 free_count[kPools] = {};
    u32 lent_count = 0U;
    bool deferred_receives = false;
    bool deferred_sends = false;
};

namespace
{
constexpr usize kLine = 64U;
constexpr u32 kMarked = 1U << 16U; // in UdpRio::sent, above the datagram count

[[nodiscard]] constexpr usize round_up(usize value, usize to) noexcept
{
    return (value + to - 1U) / to * to;
}

[[nodiscard]] constexpr usize peer_offset(usize datagram_bytes) noexcept
{
    return round_up(datagram_bytes, alignof(SOCKADDR_INET));
}

[[nodiscard]] constexpr usize control_offset(usize datagram_bytes) noexcept
{
    return round_up(peer_offset(datagram_bytes) + sizeof(SOCKADDR_INET), alignof(WSACMSGHDR));
}

[[nodiscard]] constexpr usize slot_stride(usize datagram_bytes) noexcept
{
    return round_up(control_offset(datagram_bytes) + kRioControlBytes, kLine);
}

[[nodiscard]] u32 pool_of(const UdpRio& rio, u32 slot) noexcept
{
    if (slot < rio.first[kSends])
    {
        return kReceives;
    }
    return (slot < rio.first[kTrains]) ? kSends : kTrains;
}

[[nodiscard]] u8* slot_at(const UdpRio& rio, u32 slot) noexcept
{
    const u32 pool = pool_of(rio, slot);
    return rio.base[pool] + usize{slot - rio.first[pool]} * rio.stride[pool];
}

// A request's context: its slot's number, carried through the host and never dereferenced.
[[nodiscard]] void* context_of(u32 slot) noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(slot));
}

// Posts receive buffer `slot`, deferred: the host starts it at the next commit.
[[nodiscard]] bool post_receive(UdpRio& rio, u32 slot) noexcept
{
    const u32 bytes = rio.bytes[kReceives];
    RIO_BUF data{rio.ids[slot], 0U, bytes};
    RIO_BUF peer{rio.ids[slot], static_cast<ULONG>(peer_offset(bytes)), sizeof(SOCKADDR_INET)};
    RIO_BUF control{rio.ids[slot], static_cast<ULONG>(control_offset(bytes)), kRioControlBytes};
    const bool posted = rio.table.RIOReceiveEx(rio.requests, &data, 1U, nullptr, &peer, &control, nullptr,
                                               RIO_MSG_DEFER, context_of(slot)) != FALSE;
    rio.deferred_receives = rio.deferred_receives || posted;
    return posted;
}

void release(UdpRio& rio) noexcept
{
    if (rio.receive_queue != RIO_INVALID_CQ)
    {
        rio.table.RIOCloseCompletionQueue(rio.receive_queue);
    }
    if (rio.send_queue != RIO_INVALID_CQ)
    {
        rio.table.RIOCloseCompletionQueue(rio.send_queue);
    }
    const u32 slots = rio.first[kTrains] + rio.count[kTrains];
    for (u32 slot = 0U; slot < slots; ++slot)
    {
        if (rio.ids[slot] != RIO_INVALID_BUFFERID)
        {
            rio.table.RIODeregisterBuffer(rio.ids[slot]);
        }
    }
    if (rio.event != nullptr)
    {
        ::CloseHandle(rio.event);
    }
    vm::VmRegion region = rio.region;
    rio.~UdpRio();
    vm::release(region);
}
} // namespace

NativeSocket rio_socket(int family) noexcept
{
    return ::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0U,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT | WSA_FLAG_REGISTERED_IO);
}

UdpRio* rio_open(NativeSocket socket, u32 buffers, u32 datagram_bytes, bool trains) noexcept
{
    if (buffers < kRioMinBuffers || buffers > kRioMaxBuffers || (buffers & (buffers - 1U)) != 0U ||
        datagram_bytes == 0U || datagram_bytes > kMaxTrainBytes)
    {
        return nullptr;
    }
    const u32 counts[kPools] = {buffers, kRioSends, trains ? kRioTrains : 0U};
    const u32 bytes[kPools] = {datagram_bytes, datagram_bytes, static_cast<u32>(kMaxTrainBytes)};
    const u32 slots = counts[kReceives] + counts[kSends] + counts[kTrains];
    const usize head = round_up(sizeof(UdpRio), kLine);
    const usize lists = round_up(usize{slots} * (sizeof(RIO_BUFFERID) + (2U * sizeof(u32))), kLine);
    usize total = head + lists;
    for (u32 pool = 0U; pool < kPools; ++pool)
    {
        total += usize{counts[pool]} * slot_stride(bytes[pool]);
    }
    vm::VmRegion region = vm::reserve(round_up(total, vm::page_size()));
    if (!region.valid())
    {
        return nullptr;
    }
    if (!vm::commit(region.base, region.size))
    {
        vm::release(region);
        return nullptr;
    }
    u8* const base = static_cast<u8*>(region.base);
    UdpRio* const rio = new (base) UdpRio{};
    rio->region = region;
    rio->ids = reinterpret_cast<RIO_BUFFERID*>(base + head);
    // Slot numbers are the receive buffers', then the send slots' and the train slots': each list in that order.
    rio->sent = reinterpret_cast<u32*>(rio->ids + slots);
    u32* list = rio->sent + slots;
    u8* at = base + head + lists;
    u32 number = 0U;
    for (u32 pool = 0U; pool < kPools; ++pool)
    {
        rio->free[pool] = list;
        rio->base[pool] = at;
        rio->first[pool] = number;
        rio->count[pool] = counts[pool];
        rio->stride[pool] = static_cast<u32>(slot_stride(bytes[pool]));
        rio->bytes[pool] = bytes[pool];
        list += counts[pool];
        at += usize{counts[pool]} * rio->stride[pool];
        number += counts[pool];
    }
    rio->lent = rio->free[kReceives];
    for (u32 slot = 0U; slot < slots; ++slot)
    {
        rio->ids[slot] = RIO_INVALID_BUFFERID;
    }

    GUID guid = WSAID_MULTIPLE_RIO;
    DWORD returned = 0U;
    rio->table.cbSize = sizeof(rio->table);
    bool ready = ::WSAIoctl(socket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &rio->table,
                            sizeof(rio->table), &returned, nullptr, nullptr) == 0;
    // Manual reset, so a set event stays set until the next rio_watch() (NotifyReset) clears and arms it again.
    rio->event = ready ? ::CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    ready = rio->event != nullptr;
    const u32 sends = counts[kSends] + counts[kTrains];
    if (ready)
    {
        RIO_NOTIFICATION_COMPLETION notify{};
        notify.Type = RIO_EVENT_COMPLETION;
        notify.Event.EventHandle = rio->event;
        notify.Event.NotifyReset = TRUE;
        rio->receive_queue = rio->table.RIOCreateCompletionQueue(buffers, &notify);
        rio->send_queue = rio->table.RIOCreateCompletionQueue(sends, nullptr); // read as slots are wanted
        ready = rio->receive_queue != RIO_INVALID_CQ && rio->send_queue != RIO_INVALID_CQ;
    }
    for (u32 slot = 0U; ready && slot < slots; ++slot)
    {
        rio->ids[slot] = rio->table.RIORegisterBuffer(reinterpret_cast<PCHAR>(slot_at(*rio, slot)),
                                                      rio->stride[pool_of(*rio, slot)]);
        ready = rio->ids[slot] != RIO_INVALID_BUFFERID;
    }
    if (ready)
    {
        rio->requests = rio->table.RIOCreateRequestQueue(socket, buffers, 1U, sends, 1U, rio->receive_queue,
                                                         rio->send_queue, nullptr);
        ready = rio->requests != RIO_INVALID_RQ;
    }
    for (u32 slot = 0U; ready && slot < buffers; ++slot)
    {
        ready = post_receive(*rio, slot);
    }
    if (!ready)
    {
        release(*rio);
        return nullptr;
    }
    rio_flush(*rio);
    for (u32 pool = kSends; pool < kPools; ++pool)
    {
        for (u32 index = rio->count[pool]; index > 0U; --index)
        {
            rio->free[pool][rio->free_count[pool]++] = rio->first[pool] + index - 1U;
        }
    }
    return rio;
}

void rio_close(UdpRio* rio) noexcept
{
    if (rio != nullptr)
    {
        release(*rio);
    }
}

u64 rio_event(const UdpRio& rio) noexcept
{
    // A HANDLE carried as the wait handle's integer (event_of() takes it back).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return static_cast<u64>(reinterpret_cast<uintptr_t>(rio.event));
}

bool rio_watch(UdpRio& rio) noexcept
{
    const INT result = rio.table.RIONotify(rio.receive_queue);
    return result == ERROR_SUCCESS || result == WSAEALREADY;
}

void rio_recycle(UdpRio& rio) noexcept
{
    for (u32 at = 0U; at < rio.lent_count; ++at)
    {
        static_cast<void>(post_receive(rio, rio.lent[at]));
    }
    rio.lent_count = 0U;
}

bool rio_next(UdpRio& rio, RioReceived& received) noexcept
{
    RIORESULT result{};
    if (rio.table.RIODequeueCompletion(rio.receive_queue, &result, 1U) != 1U)
    {
        return false;
    }
    const u32 slot = static_cast<u32>(result.RequestContext);
    const u32 bytes = rio.bytes[kReceives];
    u8* const at = slot_at(rio, slot);
    received = RioReceived{};
    received.buffer = slot;
    // A datagram longer than its buffer completes with WSAEMSGSIZE and the bytes that fit.
    received.truncated = result.Status == WSAEMSGSIZE;
    received.error = (result.Status == 0 || received.truncated) ? 0 : static_cast<int>(result.Status);
    if (received.error != 0)
    {
        return true;
    }
    received.payload = cont::Span<u8>(at, (result.BytesTransferred < bytes) ? result.BytesTransferred : bytes);
    SOCKADDR_INET peer{};
    std::memcpy(&peer, at + peer_offset(bytes), sizeof(peer));
    std::memcpy(&received.peer, &peer, (peer.si_family == AF_INET6) ? sizeof(peer.Ipv6) : sizeof(peer.Ipv4));
    RIO_CMSG_BUFFER header{};
    std::memcpy(&header, at + control_offset(bytes), sizeof(header));
    const ULONG total = (header.TotalLength <= kRioControlBytes) ? header.TotalLength : 0U;
    received.control.Control.buf = reinterpret_cast<CHAR*>(at + control_offset(bytes) + RIO_CMSG_BASE_SIZE);
    received.control.Control.len = (total > RIO_CMSG_BASE_SIZE) ? total - static_cast<ULONG>(RIO_CMSG_BASE_SIZE) : 0U;
    return true;
}

void rio_release(UdpRio& rio, u32 buffer, bool lent) noexcept
{
    if (lent)
    {
        rio.lent[rio.lent_count++] = buffer;
        return;
    }
    static_cast<void>(post_receive(rio, buffer));
}

void rio_flush(UdpRio& rio) noexcept
{
    if (rio.deferred_receives)
    {
        static_cast<void>(rio.table.RIOReceiveEx(rio.requests, nullptr, 0U, nullptr, nullptr, nullptr, nullptr,
                                                 RIO_MSG_COMMIT_ONLY, nullptr));
        rio.deferred_receives = false;
    }
}

void rio_reap(UdpRio& rio, RioFailures& failures) noexcept
{
    RIORESULT results[kRioSends + kRioTrains];
    for (;;)
    {
        const ULONG count = rio.table.RIODequeueCompletion(rio.send_queue, results, kRioSends + kRioTrains);
        if (count == 0U || count == RIO_CORRUPT_CQ)
        {
            return;
        }
        for (ULONG at = 0U; at < count; ++at)
        {
            const u32 slot = static_cast<u32>(results[at].RequestContext);
            const u32 pool = pool_of(rio, slot);
            const LONG status = results[at].Status;
            const bool marked = (rio.sent[slot] & kMarked) != 0U;
            if (status != 0)
            {
                const bool train = pool == kTrains;
                failures.datagrams += rio.sent[slot] & (kMarked - 1U);
                failures.trains += train ? 1U : 0U;
                failures.unsegmented =
                    failures.unsegmented || (train && (status == WSAEOPNOTSUPP || (status == WSAEINVAL && !marked)));
                failures.unmarked = failures.unmarked || (status == WSAEINVAL && marked);
            }
            rio.free[pool][rio.free_count[pool]++] = slot;
        }
    }
}

bool rio_send_slot(UdpRio& rio, bool train, RioSendSlot& slot, RioFailures& failures) noexcept
{
    const u32 pool = train ? kTrains : kSends;
    if (rio.free_count[pool] == 0U)
    {
        rio_commit(rio); // the deferred sends start, so their slots come back
        rio_reap(rio, failures);
        if (rio.free_count[pool] == 0U)
        {
            return false;
        }
    }
    const u32 index = rio.free[pool][--rio.free_count[pool]];
    const u32 bytes = rio.bytes[pool];
    u8* const at = slot_at(rio, index);
    slot.index = index;
    slot.bytes = cont::Span<u8>(at, bytes);
    slot.peer = reinterpret_cast<SOCKADDR_INET*>(at + peer_offset(bytes));
    slot.control =
        cont::Span<u8>(at + control_offset(bytes) + RIO_CMSG_BASE_SIZE, kRioControlBytes - RIO_CMSG_BASE_SIZE);
    return true;
}

bool rio_send(UdpRio& rio, const RioSendSlot& slot, usize bytes, usize control_bytes, u32 datagrams, bool marked,
              int& error) noexcept
{
    const u32 pool = pool_of(rio, slot.index);
    const usize control_at = control_offset(rio.bytes[pool]);
    // The host reads a send's control messages behind a RIO_CMSG_BUFFER header, as it writes a receive's, and
    // ignores them without it.
    const RIO_CMSG_BUFFER header{static_cast<ULONG>(RIO_CMSG_BASE_SIZE + control_bytes)};
    std::memcpy(slot_at(rio, slot.index) + control_at, &header, sizeof(header));
    RIO_BUF data{rio.ids[slot.index], 0U, static_cast<ULONG>(bytes)};
    RIO_BUF peer{rio.ids[slot.index], static_cast<ULONG>(peer_offset(rio.bytes[pool])), sizeof(SOCKADDR_INET)};
    RIO_BUF control{rio.ids[slot.index], static_cast<ULONG>(control_at), header.TotalLength};
    if (rio.table.RIOSendEx(rio.requests, &data, 1U, nullptr, &peer, (control_bytes != 0U) ? &control : nullptr,
                            nullptr, RIO_MSG_DEFER, context_of(slot.index)) == FALSE)
    {
        error = ::WSAGetLastError();
        rio.free[pool][rio.free_count[pool]++] = slot.index;
        return false;
    }
    rio.sent[slot.index] = datagrams | (marked ? kMarked : 0U);
    rio.deferred_sends = true;
    return true;
}

void rio_commit(UdpRio& rio) noexcept
{
    if (rio.deferred_sends)
    {
        static_cast<void>(rio.table.RIOSendEx(rio.requests, nullptr, 0U, nullptr, nullptr, nullptr, nullptr,
                                              RIO_MSG_COMMIT_ONLY, nullptr));
        rio.deferred_sends = false;
    }
}

} // namespace crd::net
#endif
