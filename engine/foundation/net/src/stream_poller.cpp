// crd-net -- many byte-stream connections on one thread (4.2a; docs/design/network-foundation.md#stream-host).

#include "native_socket.hpp"

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/stream_poller.hpp>

#include <utility>

#if CRD_OS_LINUX
#include <sys/epoll.h>
#endif

namespace crd::net
{

// ---- The host's set ----

#if CRD_OS_LINUX

namespace
{
[[nodiscard]] u32 epoll_events(u8 interests) noexcept
{
    return (((interests & kStreamReadable) != 0U) ? static_cast<u32>(EPOLLIN | EPOLLRDHUP) : 0U) |
           (((interests & kStreamWritable) != 0U) ? static_cast<u32>(EPOLLOUT) : 0U);
}

// The events wait() takes from the host at once, the rest being reported by the next.
constexpr int kEpollBatch = 64;
} // namespace

struct TcpPoller::Native
{
    int epoll = -1;
};

bool TcpPoller::open_set() noexcept
{
    m_native->epoll = ::epoll_create1(EPOLL_CLOEXEC);
    return m_native->epoll >= 0;
}

void TcpPoller::close_set() noexcept
{
    if (m_native->epoll >= 0)
    {
        ::close(m_native->epoll);
    }
}

bool TcpPoller::watch(u32 token, u64 handle, u8 interests) noexcept
{
    epoll_event event{};
    event.events = epoll_events(interests);
    event.data.u32 = token;
    return ::epoll_ctl(m_native->epoll, EPOLL_CTL_ADD, native(handle), &event) == 0;
}

void TcpPoller::forget(u32) noexcept {} // closing a descriptor leaves the epoll set

void TcpPoller::interest(u32 token, u8 interests) noexcept
{
    u8& current = interests_of(token);
    if (current == interests)
    {
        return;
    }
    const u64 handle = (token == kStreamListener) ? m_listener->m_handle : m_sockets[token].m_handle;
    epoll_event event{};
    event.events = epoll_events(interests);
    event.data.u32 = token;
    if (::epoll_ctl(m_native->epoll, EPOLL_CTL_MOD, native(handle), &event) == 0)
    {
        current = interests;
    }
}

usize TcpPoller::wait(cont::Span<StreamReady> out, u64 timeout_ns) noexcept
{
    epoll_event events[kEpollBatch];
    const int room = (out.size() < static_cast<usize>(kEpollBatch)) ? static_cast<int>(out.size()) : kEpollBatch;
    const int count = ::epoll_wait(m_native->epoll, events, room, wait_milliseconds(timeout_ns));
    usize filled = 0U;
    for (int at = 0; at < count; ++at)
    {
        const u32 token = events[at].data.u32;
        const u32 fired = events[at].events;
        const bool failed = (fired & static_cast<u32>(EPOLLERR | EPOLLHUP)) != 0U;
        const u8 ready = failed ? interests_of(token)
                                : static_cast<u8>((((fired & (EPOLLIN | EPOLLRDHUP)) != 0U) ? kStreamReadable : 0U) |
                                                  (((fired & EPOLLOUT) != 0U) ? kStreamWritable : 0U));
        out[filled++] = StreamReady{token, ready};
    }
    return filled;
}

#else

namespace
{
#if CRD_OS_WINDOWS
using PollEntry = WSAPOLLFD;
#else
using PollEntry = pollfd;
#endif

constexpr u32 kNoPlace = ~u32{0};

[[nodiscard]] short poll_events(u8 interests) noexcept
{
    return static_cast<short>((((interests & kStreamReadable) != 0U) ? kPollReadable : 0) |
                              (((interests & kStreamWritable) != 0U) ? kPollWritable : 0));
}
} // namespace

// The polled sockets are kept dense: a slot that is free or paused costs a wait nothing, and an empty set is a sleep
// (WSAPoll refuses one).
struct TcpPoller::Native
{
    PollEntry* entries = nullptr;
    u32* tokens = nullptr; // entry to token
    u32* places = nullptr; // slot, then the listener, to entry
    u32 count = 0U;
    u32 rotation = 0U; // where the next report starts, so a full `out` starves nobody
};

bool TcpPoller::open_set() noexcept
{
    Native& set = *m_native;
    const usize tokens = usize{m_slots} + 1U;
    set.entries = memory::try_construct_array<PollEntry>(m_allocator, tokens);
    set.tokens = memory::try_construct_array<u32>(m_allocator, tokens);
    set.places = memory::try_construct_array<u32>(m_allocator, tokens);
    if (set.entries == nullptr || set.tokens == nullptr || set.places == nullptr)
    {
        return false;
    }
    for (usize index = 0U; index < tokens; ++index)
    {
        set.places[index] = kNoPlace;
    }
    return true;
}

void TcpPoller::close_set() noexcept
{
    const usize tokens = usize{m_slots} + 1U;
    memory::destroy_array(m_allocator, m_native->entries, tokens);
    memory::destroy_array(m_allocator, m_native->tokens, tokens);
    memory::destroy_array(m_allocator, m_native->places, tokens);
}

bool TcpPoller::watch(u32 token, u64 handle, u8 interests) noexcept
{
    Native& set = *m_native;
    set.entries[set.count] = PollEntry{};
    set.entries[set.count].fd = native(handle);
    set.entries[set.count].events = poll_events(interests);
    set.tokens[set.count] = token;
    set.places[(token == kStreamListener) ? m_slots : token] = set.count;
    ++set.count;
    return true;
}

void TcpPoller::forget(u32 token) noexcept
{
    Native& set = *m_native;
    const u32 index = (token == kStreamListener) ? m_slots : token;
    const u32 place = set.places[index];
    if (place == kNoPlace)
    {
        return;
    }
    set.places[index] = kNoPlace;
    --set.count;
    if (place != set.count)
    {
        set.entries[place] = set.entries[set.count];
        set.tokens[place] = set.tokens[set.count];
        const u32 moved = set.tokens[place];
        set.places[(moved == kStreamListener) ? m_slots : moved] = place;
    }
}

void TcpPoller::interest(u32 token, u8 interests) noexcept
{
    u8& current = interests_of(token);
    if (current == interests)
    {
        return;
    }
    current = interests;
    if (interests == 0U)
    {
        forget(token);
        return;
    }
    const u32 index = (token == kStreamListener) ? m_slots : token;
    if (m_native->places[index] != kNoPlace)
    {
        m_native->entries[m_native->places[index]].events = poll_events(interests);
        return;
    }
    const u64 handle = (token == kStreamListener) ? m_listener->m_handle : m_sockets[token].m_handle;
    static_cast<void>(watch(token, handle, interests));
}

usize TcpPoller::wait(cont::Span<StreamReady> out, u64 timeout_ns) noexcept
{
    Native& set = *m_native;
#if CRD_OS_WINDOWS
    if (set.count == 0U)
    {
        ::Sleep(static_cast<DWORD>(wait_milliseconds(timeout_ns)));
        return 0U;
    }
    const int fired = ::WSAPoll(set.entries, set.count, wait_milliseconds(timeout_ns));
#else
    const int fired = ::poll(set.entries, set.count, wait_milliseconds(timeout_ns));
#endif
    if (fired <= 0 || set.count == 0U)
    {
        return 0U;
    }
    usize filled = 0U;
    const u32 start = set.rotation % set.count;
    for (u32 step = 0U; step < set.count && filled < out.size(); ++step)
    {
        const PollEntry& entry = set.entries[(start + step) % set.count];
        if (entry.revents == 0)
        {
            continue;
        }
        const u32 token = set.tokens[(start + step) % set.count];
        const bool failed = (entry.revents & ~(kPollReadable | kPollWritable)) != 0;
        const u8 ready = failed ? interests_of(token)
                                : static_cast<u8>((((entry.revents & kPollReadable) != 0) ? kStreamReadable : 0U) |
                                                  (((entry.revents & kPollWritable) != 0) ? kStreamWritable : 0U));
        out[filled++] = StreamReady{token, ready};
        set.rotation = start + step + 1U;
    }
    return filled;
}

#endif

// ---- The poller ----

TcpPoller::TcpPoller(memory::IAllocator& allocator, u32 slots, TcpListener* listener,
                     const TcpOptions& connect_options) noexcept
    : m_allocator(allocator), m_listener(listener), m_options(connect_options), m_slots(slots)
{
    if (slots == 0U || slots > kStreamMaxSlots || (listener != nullptr && !listener->is_open()))
    {
        return;
    }
    m_sockets = memory::try_construct_array<TcpSocket>(allocator, slots);
    m_interests = memory::try_construct_array<u8>(allocator, usize{slots} + 1U);
    m_native = memory::construct<Native>(allocator);
    if (m_sockets == nullptr || m_interests == nullptr || m_native == nullptr || !open_set() ||
        (listener != nullptr && !watch(kStreamListener, listener->m_handle, kStreamReadable)))
    {
        release();
        return;
    }
    if (listener != nullptr)
    {
        interests_of(kStreamListener) = kStreamReadable;
    }
}

TcpPoller::~TcpPoller()
{
    release();
}

void TcpPoller::release() noexcept
{
    if (m_native != nullptr)
    {
        close_set();
    }
    memory::destroy_array(m_allocator, m_sockets, m_slots);
    memory::destroy_array(m_allocator, m_interests, usize{m_slots} + 1U);
    memory::destroy(m_allocator, m_native);
    m_sockets = nullptr;
    m_interests = nullptr;
    m_native = nullptr;
}

u8& TcpPoller::interests_of(u32 token) noexcept
{
    return m_interests[(token == kStreamListener) ? m_slots : token];
}

StreamSocket* TcpPoller::accept(u32 slot, SocketError& error) noexcept
{
    error = SocketError::Closed;
    if (m_listener == nullptr)
    {
        return nullptr;
    }
    TcpSocket socket = m_listener->accept(error);
    if (error != SocketError::None)
    {
        return nullptr;
    }
    if (!watch(slot, socket.m_handle, kStreamReadable))
    {
        error = SocketError::Resources;
        socket.abort();
        return nullptr;
    }
    m_sockets[slot] = std::move(socket);
    interests_of(slot) = kStreamReadable;
    return &m_sockets[slot];
}

StreamSocket* TcpPoller::connect(u32 slot, const Endpoint& peer, SocketError& error) noexcept
{
    TcpSocket socket = TcpSocket::connect(peer, m_options, error);
    if (error != SocketError::None)
    {
        return nullptr;
    }
    if (!watch(slot, socket.m_handle, kStreamWritable))
    {
        error = SocketError::Resources;
        return nullptr;
    }
    m_sockets[slot] = std::move(socket);
    interests_of(slot) = kStreamWritable;
    return &m_sockets[slot];
}

SocketError TcpPoller::connected(u32 slot) noexcept
{
    return m_sockets[slot].connected();
}

void TcpPoller::close(u32 slot, bool reset) noexcept
{
    forget(slot);
    interests_of(slot) = 0U;
    if (reset)
    {
        m_sockets[slot].abort();
    }
    else
    {
        m_sockets[slot].close();
    }
}

} // namespace crd::net
