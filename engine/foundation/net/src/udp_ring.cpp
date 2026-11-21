// crd-net -- Linux's io_uring under a UdpSocket's receive (NET-PATH; docs/design/network-foundation.md#sockets).

#include "udp_ring.hpp"

#if CRD_NET_RING
#include <crd/vm/virtual_memory.hpp>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <new>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace crd::net
{

// An io_uring instance's descriptor and the queues it shares with the host.
struct RingQueues
{
    int fd = -1;
    void* rings = nullptr; // the submission and completion rings, one mapping where the host shares them
    usize rings_bytes = 0U;
    void* completions_map = nullptr; // the completion ring where the host maps it apart
    usize completions_bytes = 0U;
    io_uring_sqe* sqes = nullptr;
    usize sqes_bytes = 0U;
    u32* sq_tail = nullptr;
    u32* sq_array = nullptr;
    u32* sq_flags = nullptr;
    u32 sq_mask = 0U;
    u32* cq_head = nullptr;
    u32* cq_tail = nullptr;
    u32 cq_mask = 0U;
    io_uring_cqe* cqes = nullptr;
};

// The ring in the first bytes of its own pages; the buffer ring, the lent list, the buffers and the report buffers
// follow in that order, each starting on a page or a cache line.
struct UdpRing : RingQueues
{
    vm::VmRegion region;
    int socket = -1;
    io_uring_buf* buffer_ring = nullptr; // the host's ring of free buffers; its tail overlays entry 0's resv
    u16* lent = nullptr;                 // buffers completions handed out since the last recycle
    u32 lent_count = 0U;
    u8* buffers = nullptr;
    u32 buffer_count = 0U;
    u32 stride = 0U;        // a buffer's bytes rounded up to a page, where the next one starts
    u32 message_bytes = 0U; // what the host may write into one: header, name, control and the datagram
    u16 buffer_tail = 0U;
    u32 control_bytes = 0U;
    u8* reports = nullptr;
    msghdr layout{};         // the multishot recvmsg's shape: the room for a name and for control messages per buffer
    u32 generation = 0U;     // the current requests', in their user_data above the tag
    u32 requests = 0U;       // requests the host holds: the current ones and any replaced one not yet ended
    pid_t owner = 0;         // the thread that armed the current requests, the task their completions run as
    bool armed = false;      // the host holds the current receive
    bool watching = false;   // the host holds the current watch on the socket's error queue
    bool registered = false; // the buffer ring is registered with the host
};

namespace
{
// Each request's user_data: its tag in the low byte, the generation of the requests it belongs to above.
constexpr u64 kReceiveTag = 1U; // the multishot recvmsg
constexpr u64 kCancelTag = 2U;  // a cancellation
constexpr u64 kWatchTag = 3U;   // the poll for the socket's error queue
constexpr u64 kTagMask = 0xFFU;
// Two cancellations and the two requests that replace them are the most the ring submits at once.
constexpr u32 kSubmissionEntries = 4U;

[[nodiscard]] int setup(u32 entries, io_uring_params& params) noexcept
{
    return static_cast<int>(::syscall(__NR_io_uring_setup, entries, &params));
}

[[nodiscard]] int enter(int fd, u32 submit, u32 wait, u32 flags) noexcept
{
    return static_cast<int>(::syscall(__NR_io_uring_enter, fd, submit, wait, flags, nullptr, 0));
}

[[nodiscard]] int register_with(int fd, unsigned opcode, void* argument, unsigned count) noexcept
{
    return static_cast<int>(::syscall(__NR_io_uring_register, fd, opcode, argument, count));
}

// The calling thread, as the host names the task a request's completions run as.
[[nodiscard]] pid_t this_thread() noexcept
{
    thread_local const pid_t kThread = static_cast<pid_t>(::syscall(SYS_gettid));
    return kThread;
}

[[nodiscard]] constexpr usize round_up(usize value, usize to) noexcept
{
    return (value + to - 1U) / to * to;
}

[[nodiscard]] constexpr u64 request_data(u64 tag, u32 generation) noexcept
{
    return (u64{generation} << 8U) | tag;
}

// The host and this side share the rings' heads and tails: each side's stores are released and the other's acquired.
[[nodiscard]] u32 acquire(u32* at) noexcept
{
    return std::atomic_ref<u32>(*at).load(std::memory_order_acquire);
}

void release(u32* at, u32 value) noexcept
{
    std::atomic_ref<u32>(*at).store(value, std::memory_order_release);
}

[[nodiscard]] void* map_ring(int fd, usize bytes, off_t offset) noexcept
{
    void* const at = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, offset);
    return (at == MAP_FAILED) ? nullptr : at;
}

// The entry `queued` places past the submission ring's tail. The host consumes every entry each enter() submits, so
// the ring holds no more than one call's.
[[nodiscard]] io_uring_sqe& next_entry(RingQueues& ring, u32 queued) noexcept
{
    const u32 index = (*ring.sq_tail + queued) & ring.sq_mask;
    io_uring_sqe& entry = ring.sqes[index];
    std::memset(&entry, 0, sizeof(entry));
    ring.sq_array[index] = index;
    return entry;
}

// Submits the `count` entries next_entry() placed; false, taking them back, when the host took none of them now.
[[nodiscard]] bool submit(RingQueues& ring, u32 count) noexcept
{
    const u32 tail = *ring.sq_tail;
    release(ring.sq_tail, tail + count);
    int taken = -1;
    do
    {
        taken = enter(ring.fd, count, 0U, 0U);
    } while (taken < 0 && errno == EINTR);
    if (taken <= 0)
    {
        release(ring.sq_tail, tail); // the host read none of them: they go again on the next call
        return false;
    }
    return true;
}

void cancel_entry(io_uring_sqe& entry, u64 request) noexcept
{
    entry.opcode = IORING_OP_ASYNC_CANCEL;
    entry.flags = IOSQE_CQE_SKIP_SUCCESS; // a cancellation answers only when it fails
    entry.fd = -1;
    entry.addr = request;
    entry.user_data = kCancelTag;
}

void receive_entry(const UdpRing& ring, io_uring_sqe& entry, u32 generation) noexcept
{
    entry.opcode = IORING_OP_RECVMSG;
    entry.fd = ring.socket;
    entry.addr = reinterpret_cast<u64>(&ring.layout);
    entry.len = 1U;
    entry.ioprio = IORING_RECV_MULTISHOT;
    entry.flags = IOSQE_BUFFER_SELECT;
    entry.buf_group = 0U;
    entry.user_data = request_data(kReceiveTag, generation);
}

// The socket's error queue watched: one completion once it holds a report (POLLERR), at once when it does already.
// The receive alone would learn of a report only from the socket's error, which a send may take first (sendmmsg
// drops one met past its first message). One shot, armed again at the end of each receive(): a flood of reports
// completes it once a call, and reports a call leaves complete it again at once.
void watch_entry(const UdpRing& ring, io_uring_sqe& entry, u32 generation) noexcept
{
    entry.opcode = IORING_OP_POLL_ADD;
    entry.fd = ring.socket;
    entry.poll32_events = POLLERR;
    entry.user_data = request_data(kWatchTag, generation);
}

// Arms from this thread the requests named: a new generation of both after the current ones' cancellation when
// `replace`, else the receive and the watch where `receive` and `watch` ask for them.
[[nodiscard]] bool arm(UdpRing& ring, bool replace, bool receive, bool watch) noexcept
{
    const u32 generation = replace ? ring.generation + 1U : ring.generation;
    u32 queued = 0U;
    if (replace && ring.armed)
    {
        cancel_entry(next_entry(ring, queued++), request_data(kReceiveTag, ring.generation));
    }
    if (replace && ring.watching)
    {
        cancel_entry(next_entry(ring, queued++), request_data(kWatchTag, ring.generation));
    }
    if (receive)
    {
        receive_entry(ring, next_entry(ring, queued++), generation);
    }
    if (watch)
    {
        watch_entry(ring, next_entry(ring, queued++), generation);
    }
    if (queued == 0U || !submit(ring, queued))
    {
        return queued == 0U;
    }
    ring.generation = generation;
    ring.owner = (receive && watch) ? this_thread() : ring.owner;
    ring.armed = receive || (ring.armed && !replace);
    ring.watching = watch || (ring.watching && !replace);
    ring.requests += (receive ? 1U : 0U) + (watch ? 1U : 0U);
    return true;
}

void give(UdpRing& ring, u16 buffer) noexcept
{
    io_uring_buf& entry = ring.buffer_ring[ring.buffer_tail & (ring.buffer_count - 1U)];
    // Field by field: entry 0's resv is the ring's tail, which only publish_buffers() writes.
    entry.addr = reinterpret_cast<u64>(ring.buffers + (usize{buffer} * ring.stride));
    entry.len = ring.message_bytes;
    entry.bid = buffer;
    ++ring.buffer_tail;
}

void publish_buffers(UdpRing& ring) noexcept
{
    std::atomic_ref<u16>(ring.buffer_ring[0].resv).store(ring.buffer_tail, std::memory_order_release);
}

// Unmaps the queues and closes the descriptor.
void close_queues(RingQueues& ring) noexcept
{
    if (ring.sqes != nullptr)
    {
        ::munmap(ring.sqes, ring.sqes_bytes);
    }
    if (ring.completions_map != nullptr)
    {
        ::munmap(ring.completions_map, ring.completions_bytes);
    }
    if (ring.rings != nullptr)
    {
        ::munmap(ring.rings, ring.rings_bytes);
    }
    if (ring.fd >= 0)
    {
        ::close(ring.fd);
    }
}

// Releases what ring_open() made, the host holding no buffer of it any more.
void destroy(UdpRing* ring) noexcept
{
    if (ring->registered)
    {
        io_uring_buf_reg unregister{};
        unregister.bgid = 0U;
        static_cast<void>(register_with(ring->fd, IORING_UNREGISTER_PBUF_RING, &unregister, 1U));
    }
    close_queues(*ring);
    vm::VmRegion region = ring->region;
    ring->~UdpRing();
    vm::release(region);
}

// Maps the rings of a set-up ring; false where the host refuses a mapping.
[[nodiscard]] bool map_rings(RingQueues& ring, const io_uring_params& params) noexcept
{
    const usize submission_bytes = params.sq_off.array + (params.sq_entries * sizeof(u32));
    const usize completion_bytes = params.cq_off.cqes + (params.cq_entries * sizeof(io_uring_cqe));
    const bool single = (params.features & IORING_FEAT_SINGLE_MMAP) != 0U;
    ring.rings_bytes = (single && completion_bytes > submission_bytes) ? completion_bytes : submission_bytes;
    ring.rings = map_ring(ring.fd, ring.rings_bytes, IORING_OFF_SQ_RING);
    if (ring.rings == nullptr)
    {
        return false;
    }
    u8* completions = static_cast<u8*>(ring.rings);
    if (!single)
    {
        ring.completions_bytes = completion_bytes;
        ring.completions_map = map_ring(ring.fd, completion_bytes, IORING_OFF_CQ_RING);
        if (ring.completions_map == nullptr)
        {
            return false;
        }
        completions = static_cast<u8*>(ring.completions_map);
    }
    ring.sqes_bytes = params.sq_entries * sizeof(io_uring_sqe);
    ring.sqes = static_cast<io_uring_sqe*>(map_ring(ring.fd, ring.sqes_bytes, IORING_OFF_SQES));
    if (ring.sqes == nullptr)
    {
        return false;
    }
    u8* const submissions = static_cast<u8*>(ring.rings);
    ring.sq_tail = reinterpret_cast<u32*>(submissions + params.sq_off.tail);
    ring.sq_array = reinterpret_cast<u32*>(submissions + params.sq_off.array);
    ring.sq_flags = reinterpret_cast<u32*>(submissions + params.sq_off.flags);
    ring.sq_mask = *reinterpret_cast<u32*>(submissions + params.sq_off.ring_mask);
    ring.cq_head = reinterpret_cast<u32*>(completions + params.cq_off.head);
    ring.cq_tail = reinterpret_cast<u32*>(completions + params.cq_off.tail);
    ring.cq_mask = *reinterpret_cast<u32*>(completions + params.cq_off.ring_mask);
    ring.cqes = reinterpret_cast<io_uring_cqe*>(completions + params.cq_off.cqes);
    return true;
}

} // namespace

UdpRing* ring_open(int socket, u32 buffers, u32 datagram_bytes, u32 control_bytes) noexcept
{
    if (buffers < kRingMinBuffers || buffers > kRingMaxBuffers || (buffers & (buffers - 1U)) != 0U ||
        datagram_bytes == 0U)
    {
        return nullptr;
    }
    // Each buffer starts a page, so a short datagram backs one page of it and a long one only the pages it fills.
    const usize page = vm::page_size();
    const usize message_bytes = sizeof(io_uring_recvmsg_out) + kRingNameBytes + control_bytes + datagram_bytes;
    const usize stride = round_up(message_bytes, page);
    const usize ring_at = round_up(sizeof(UdpRing), page);
    const usize lent_at = ring_at + round_up(usize{buffers} * sizeof(io_uring_buf), page);
    const usize buffers_at = round_up(lent_at + (usize{buffers} * sizeof(u16)), page);
    const usize reports_at = buffers_at + (usize{buffers} * stride);
    const usize total = round_up(reports_at + (usize{kRingReports} * kRingReportBytes), page);
    vm::VmRegion region = vm::reserve(total);
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
    UdpRing* const ring = new (base) UdpRing{};
    ring->region = region;
    ring->socket = socket;
    ring->buffer_ring = reinterpret_cast<io_uring_buf*>(base + ring_at);
    ring->lent = reinterpret_cast<u16*>(base + lent_at);
    ring->buffers = base + buffers_at;
    ring->buffer_count = buffers;
    ring->stride = static_cast<u32>(stride);
    ring->message_bytes = static_cast<u32>(message_bytes);
    ring->control_bytes = control_bytes;
    ring->reports = base + reports_at;
    ring->layout.msg_namelen = kRingNameBytes;
    ring->layout.msg_controllen = control_bytes;

    // Room for every buffer's completion and the cancellations', so the host never holds completions back.
    io_uring_params params{};
    // Every entry of a submit is taken, one the host refuses answered by its own completion (SUBMIT_ALL).
    params.flags = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP | IORING_SETUP_SUBMIT_ALL;
    params.cq_entries = 2U * buffers;
    ring->fd = setup(kSubmissionEntries, params);
    if (ring->fd < 0 || !map_rings(*ring, params))
    {
        destroy(ring);
        return nullptr;
    }
    io_uring_buf_reg registration{};
    registration.ring_addr = reinterpret_cast<u64>(ring->buffer_ring);
    registration.ring_entries = buffers;
    registration.bgid = 0U;
    ring->registered = register_with(ring->fd, IORING_REGISTER_PBUF_RING, &registration, 1U) == 0;
    if (!ring->registered)
    {
        destroy(ring);
        return nullptr;
    }
    for (u32 buffer = 0U; buffer < buffers; ++buffer)
    {
        give(*ring, static_cast<u16>(buffer));
    }
    publish_buffers(*ring);
    // Armed before the socket binds, so nothing can have arrived: a completion already waiting is the host refusing
    // a request (a kernel before 6.0 refuses the multishot flag at once).
    if (!arm(*ring, false, true, true))
    {
        destroy(ring);
        return nullptr;
    }
    RingCompletion refused;
    if (ring_next(*ring, refused))
    {
        ring_close(ring);
        return nullptr;
    }
    return ring;
}

void ring_close(UdpRing* ring) noexcept
{
    if (ring == nullptr)
    {
        return;
    }
    // Every request ends before the buffers go: the current ones cancelled, then each one's last completion read,
    // after which the host writes none of them again (closing the ring alone ends them later, on the host's time).
    u32 queued = 0U;
    if (ring->armed)
    {
        cancel_entry(next_entry(*ring, queued++), request_data(kReceiveTag, ring->generation));
    }
    if (ring->watching)
    {
        cancel_entry(next_entry(*ring, queued++), request_data(kWatchTag, ring->generation));
    }
    while (queued != 0U && !submit(*ring, queued))
    {
        static_cast<void>(enter(ring->fd, 0U, 1U, IORING_ENTER_GETEVENTS)); // room once completions are read
    }
    while (ring->requests != 0U)
    {
        RingCompletion completion;
        if (!ring_next(*ring, completion) && ring->requests != 0U)
        {
            static_cast<void>(enter(ring->fd, 0U, 1U, IORING_ENTER_GETEVENTS));
        }
    }
    destroy(ring);
}

int ring_handle(const UdpRing& ring) noexcept
{
    return ring.fd;
}

bool ring_armed(const UdpRing& ring) noexcept
{
    return ring.armed;
}

bool ring_pending(const UdpRing& ring) noexcept
{
    return *ring.cq_head != acquire(ring.cq_tail);
}

bool ring_arm(UdpRing& ring, bool watch) noexcept
{
    // With every buffer lent the host would end the receive at once (it takes a buffer before it reads).
    const bool receive = !ring.armed && ring.lent_count < ring.buffer_count;
    return arm(ring, false, receive, watch && !ring.watching) && ring.armed;
}

bool ring_follow(UdpRing& ring) noexcept
{
    return ring.owner == this_thread() || arm(ring, true, true, true);
}

bool ring_next(UdpRing& ring, RingCompletion& completion, bool ends_only) noexcept
{
    completion = RingCompletion{};
    for (;;)
    {
        const u32 head = *ring.cq_head;
        if (head == acquire(ring.cq_tail))
        {
            return false;
        }
        const io_uring_cqe entry = ring.cqes[head & ring.cq_mask];
        if (ends_only && (entry.flags & IORING_CQE_F_BUFFER) != 0U)
        {
            return false;
        }
        release(ring.cq_head, head + 1U);
        const u64 tag = entry.user_data & kTagMask;
        const bool current = (entry.user_data >> 8U) == ring.generation;
        if (tag == kWatchTag)
        {
            --ring.requests;
            ring.watching = ring.watching && !current;
            if (current && entry.res > 0 && (static_cast<u32>(entry.res) & POLLERR) != 0U)
            {
                completion.report = true;
                return true;
            }
            continue; // a cancelled or replaced watch
        }
        if (tag != kReceiveTag)
        {
            continue; // a cancellation's own answer
        }
        if ((entry.flags & IORING_CQE_F_MORE) == 0U)
        {
            --ring.requests;
            ring.armed = ring.armed && !current;
        }
        if (entry.res < 0)
        {
            if (!current)
            {
                continue; // a replaced receive's end
            }
            completion.error = -entry.res;
            return true;
        }
        if ((entry.flags & IORING_CQE_F_BUFFER) == 0U)
        {
            if (!current)
            {
                continue;
            }
            return true; // an end that carries nothing
        }
        const u16 buffer = static_cast<u16>(entry.flags >> IORING_CQE_BUFFER_SHIFT);
        ring.lent[ring.lent_count++] = buffer;
        u8* const message = ring.buffers + (usize{buffer} * ring.stride);
        io_uring_recvmsg_out out{};
        std::memcpy(&out, message, sizeof(out));
        // The host wrote the header, the whole room for a name and for control messages, then the payload it held.
        const usize payload_at = sizeof(out) + kRingNameBytes + ring.control_bytes;
        const usize written = static_cast<usize>(entry.res);
        const usize held = (written > payload_at) ? written - payload_at : 0U;
        completion.datagram = true;
        completion.message.header.msg_name = message + sizeof(out);
        completion.message.header.msg_namelen = (out.namelen < kRingNameBytes) ? out.namelen : kRingNameBytes;
        completion.message.header.msg_control = message + sizeof(out) + kRingNameBytes;
        completion.message.header.msg_controllen =
            (out.controllen < ring.control_bytes) ? out.controllen : ring.control_bytes;
        const usize payload = (held < out.payloadlen) ? held : out.payloadlen;
        completion.message.payload = cont::Span<u8>(message + payload_at, payload);
        completion.message.truncated = (out.flags & MSG_TRUNC) != 0U || out.payloadlen > held;
        return true;
    }
}

void ring_recycle(UdpRing& ring) noexcept
{
    if (ring.lent_count == 0U)
    {
        return;
    }
    for (u32 index = 0U; index < ring.lent_count; ++index)
    {
        give(ring, ring.lent[index]);
    }
    ring.lent_count = 0U;
    publish_buffers(ring);
}

cont::Span<u8> ring_reports(UdpRing& ring) noexcept
{
    return cont::Span<u8>(ring.reports, usize{kRingReports} * kRingReportBytes);
}

#if CRD_NET_SEND_RING
// ---- the zero-copy send ----

namespace
{
constexpr u64 kPlainSendTag = 4U; // a message sent copied, its index in the call above the tag
constexpr u64 kZeroCopyTag = 5U;  // a zero-copy message, its request's number above the tag
static_assert((kSendRingRequests & (kSendRingRequests - 1U)) == 0U, "requests are numbered around a power of two");
// Notifications in a row reporting copied pages (loopback, a veth pair, a device without scatter-gather) turn
// zero-copy off for the socket, and every kZeroCopyProbe-th send that wants it then tries it again, so a socket whose
// route moves to a device that sends in place finds out.
constexpr u32 kCopiedStreak = 8U;
constexpr u32 kZeroCopyProbe = 1024U;
// The fragments one zero-copy message may take: its pages become fragments of the host's buffer, which holds
// MAX_SKB_FRAGS of them (17 unless the kernel is built with more), and a message past that fails EMSGSIZE.
constexpr usize kZeroCopyFragments = 17U;

// A zero-copy message from its submission until its notification.
struct SendRequest
{
    u64 first = 0U;      // the number of its first datagram
    u32 message = 0U;    // its index in the send that submitted it
    bool result = false; // its first completion arrived
    bool taken = false;  // that completion said it was sent
    bool more = false;   // a notification follows
    bool notified = false;
    bool copied = false; // the notification said the host copied the pages
};
} // namespace

// The ring in the first bytes of its own pages.
struct UdpSendRing : RingQueues
{
    vm::VmRegion region;
    int socket = -1;
    SendRequest requests[kSendRingRequests]; // numbered in submission order, `head` the oldest not yet done
    u32 head = 0U;
    u32 tail = 0U;
    u32 streak = 0U; // notifications in a row that reported copied pages
    u32 probe = 0U;  // wanted sends left until the next probe while zero-copy is off
    bool zero_copy = true;
};

namespace
{
[[nodiscard]] bool request_done(const SendRequest& request) noexcept
{
    return request.result && (!request.more || request.notified);
}

// A request done: a sent message's notification moves the copy policy.
void settle_request(UdpSendRing& ring, const SendRequest& request, OffloadCounters& counters) noexcept
{
    if (!request.taken || !request.more)
    {
        return; // a failed message sent nothing, so its notification reports nothing
    }
    if (request.copied)
    {
        ++counters.zero_copy_copied;
        ring.streak = (ring.streak < kCopiedStreak) ? ring.streak + 1U : kCopiedStreak;
        if (ring.streak == kCopiedStreak && ring.zero_copy)
        {
            ring.zero_copy = false;
            ring.probe = kZeroCopyProbe;
        }
        return;
    }
    ++counters.zero_copy_sent;
    ring.streak = 0U;
    ring.zero_copy = true;
}

// Reads every completion waiting: a result of the send in flight into `results` (one per message, `seen` counting
// them), a notification into its request; the requests done at the oldest end are given back.
void reap(UdpSendRing& ring, int* results, u32& seen, OffloadCounters& counters) noexcept
{
    for (;;)
    {
        const u32 head = *ring.cq_head;
        if (head == acquire(ring.cq_tail))
        {
            break;
        }
        const io_uring_cqe entry = ring.cqes[head & ring.cq_mask];
        release(ring.cq_head, head + 1U);
        const u64 tag = entry.user_data & kTagMask;
        const u32 number = static_cast<u32>(entry.user_data >> 8U);
        if (tag == kPlainSendTag)
        {
            results[number] = entry.res;
            ++seen;
            continue;
        }
        SendRequest& request = ring.requests[number & (kSendRingRequests - 1U)];
        if ((entry.flags & IORING_CQE_F_NOTIF) != 0U)
        {
            request.notified = true;
            request.copied = (static_cast<u32>(entry.res) & IORING_NOTIF_USAGE_ZC_COPIED) != 0U;
        }
        else
        {
            request.result = true;
            request.taken = entry.res >= 0;
            request.more = (entry.flags & IORING_CQE_F_MORE) != 0U;
            results[request.message] = entry.res;
            ++seen;
        }
        if (request_done(request))
        {
            settle_request(ring, request, counters);
        }
    }
    while (ring.head != ring.tail && request_done(ring.requests[ring.head & (kSendRingRequests - 1U)]))
    {
        ++ring.head;
    }
}

// The fragments a message's bytes become: a page each, where an iovec that goes on where the last ended, inside its
// page, joins that page's fragment as the host joins it.
[[nodiscard]] usize fragments_of(const msghdr& message) noexcept
{
    const usize page = vm::page_size();
    usize fragments = 0U;
    usize end = 0U;
    for (usize index = 0U; index < message.msg_iovlen; ++index)
    {
        const iovec& vector = message.msg_iov[index];
        const usize from = reinterpret_cast<usize>(vector.iov_base);
        if (vector.iov_len == 0U)
        {
            continue;
        }
        fragments += ((from + vector.iov_len - 1U) / page) - (from / page) + 1U;
        fragments -= (from == end && from % page != 0U) ? 1U : 0U;
        end = from + vector.iov_len;
    }
    return fragments;
}

// Runs the task work that posts notifications where the host flagged some waiting (TASKRUN_FLAG), then reads them.
void collect(UdpSendRing& ring, OffloadCounters& counters) noexcept
{
    if ((acquire(ring.sq_flags) & IORING_SQ_TASKRUN) != 0U)
    {
        static_cast<void>(enter(ring.fd, 0U, 0U, IORING_ENTER_GETEVENTS));
    }
    int results[kSendRingMessages];
    u32 seen = 0U;
    reap(ring, results, seen, counters);
}

void destroy(UdpSendRing* ring) noexcept
{
    close_queues(*ring);
    vm::VmRegion region = ring->region;
    ring->~UdpSendRing();
    vm::release(region);
}
} // namespace

UdpSendRing* send_ring_open(int socket) noexcept
{
    vm::VmRegion region = vm::reserve(round_up(sizeof(UdpSendRing), vm::page_size()));
    if (!region.valid())
    {
        return nullptr;
    }
    if (!vm::commit(region.base, region.size))
    {
        vm::release(region);
        return nullptr;
    }
    UdpSendRing* const ring = new (region.base) UdpSendRing{};
    ring->region = region;
    ring->socket = socket;
    // Room for a send's results and every request's two completions, so the host never holds one back. Task work
    // (what posts a notification) runs at the next kernel transition and is flagged meanwhile, never an interrupt.
    io_uring_params params{};
    params.flags = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN |
                   IORING_SETUP_TASKRUN_FLAG;
    params.cq_entries = 2U * (kSendRingRequests + kSendRingMessages);
    ring->fd = setup(kSendRingMessages, params);
    // IORING_FEAT_REG_REG_RING marks Linux 6.3, which has every flag the send asks for (copy reports from 6.2).
    if (ring->fd < 0 || (params.features & IORING_FEAT_REG_REG_RING) == 0U || !map_rings(*ring, params))
    {
        destroy(ring);
        return nullptr;
    }
    return ring;
}

void send_ring_close(UdpSendRing* ring) noexcept
{
    if (ring == nullptr)
    {
        return;
    }
    OffloadCounters ignored;
    static_cast<void>(send_ring_released(*ring, 0U, true, ignored));
    destroy(ring);
}

bool send_ring_wanted(UdpSendRing& ring) noexcept
{
    if (ring.zero_copy)
    {
        return true;
    }
    if (--ring.probe != 0U)
    {
        return false;
    }
    ring.probe = kZeroCopyProbe;
    return true;
}

bool send_ring_fits(const msghdr& message) noexcept
{
    return fragments_of(message) <= kZeroCopyFragments;
}

int send_ring_send(UdpSendRing& ring, mmsghdr* messages, u32 count, u32 zero_copy, const u64* first,
                   bool& first_zero_copy, OffloadCounters& counters) noexcept
{
    first_zero_copy = false;
    count = (count < kSendRingMessages) ? count : kSendRingMessages;
    collect(ring, counters);      // requests the host has given back since
    u32 tails[kSendRingMessages]; // the request tail before each message, to give back what the host did not take
    for (u32 index = 0U; index < count; ++index)
    {
        io_uring_sqe& entry = next_entry(ring, index);
        tails[index] = ring.tail;
        const bool lend = ((zero_copy >> index) & 1U) != 0U && ring.tail - ring.head < kSendRingRequests;
        if (lend)
        {
            ring.requests[ring.tail & (kSendRingRequests - 1U)] = SendRequest{first[index], index};
            entry.opcode = IORING_OP_SENDMSG_ZC;
            entry.ioprio = IORING_SEND_ZC_REPORT_USAGE;
            entry.user_data = (u64{ring.tail++} << 8U) | kZeroCopyTag;
        }
        else
        {
            entry.opcode = IORING_OP_SENDMSG;
            entry.user_data = (u64{index} << 8U) | kPlainSendTag;
        }
        first_zero_copy = first_zero_copy || (index == 0U && lend);
        entry.fd = ring.socket;
        entry.addr = reinterpret_cast<u64>(&messages[index].msg_hdr);
        entry.len = 1U;
        // MSG_DONTWAIT has the host fail a send it cannot take now (EAGAIN) rather than wait for room.
        entry.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
        entry.flags = (index + 1U < count) ? IOSQE_IO_LINK : 0U;
    }
    const u32 tail = *ring.sq_tail;
    release(ring.sq_tail, tail + count);
    int submitted = -1;
    do
    {
        submitted = enter(ring.fd, count, 0U, 0U);
    } while (submitted < 0 && errno == EINTR);
    const u32 taken = (submitted > 0) ? static_cast<u32>(submitted) : 0U;
    if (taken < count)
    {
        // What the host did not read is taken back before it can: the next enter() would read stale messages.
        release(ring.sq_tail, tail + taken);
        ring.tail = tails[taken];
    }
    if (taken == 0U)
    {
        errno = EAGAIN;
        return -1;
    }
    int results[kSendRingMessages];
    u32 seen = 0U;
    for (;;)
    {
        reap(ring, results, seen, counters);
        if (seen >= taken)
        {
            break;
        }
        // Each result comes within the call (MSG_DONTWAIT): the chain's later messages run as task work.
        static_cast<void>(enter(ring.fd, 0U, 1U, IORING_ENTER_GETEVENTS));
    }
    for (u32 index = 0U; index < taken; ++index)
    {
        if (results[index] < 0)
        {
            if (index == 0U)
            {
                errno = -results[0];
                return -1;
            }
            return static_cast<int>(index);
        }
    }
    return static_cast<int>(taken);
}

u64 send_ring_released(UdpSendRing& ring, u64 taken, bool wait, OffloadCounters& counters) noexcept
{
    collect(ring, counters);
    while (wait && ring.head != ring.tail)
    {
        static_cast<void>(enter(ring.fd, 0U, 1U, IORING_ENTER_GETEVENTS));
        collect(ring, counters);
    }
    // Sent messages are numbered in submission order, so the oldest one still lent holds the lowest number.
    for (u32 at = ring.head; at != ring.tail; ++at)
    {
        const SendRequest& request = ring.requests[at & (kSendRingRequests - 1U)];
        if (request.taken && !request_done(request))
        {
            return request.first;
        }
    }
    return taken;
}
#endif

} // namespace crd::net
#endif
