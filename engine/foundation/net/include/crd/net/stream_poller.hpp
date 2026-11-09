#pragma once

// crd-net -- many byte-stream connections on one thread (4.2a; contract:
// docs/design/network-foundation.md#stream-host). StreamSocket is one connection's seam; StreamPoller is the set's: it
// makes and ends the connections of numbered slots and says which are ready. TcpPoller is the host's TCP; NET-SIM's
// virtual streams and a test's pipes stand in its place.
//
//   level     readiness is level-triggered: a socket with bytes left, or a listener with connections left, is reported
//             again by the next wait(), so a host takes a bounded share per turn and forgets the rest.
//   interest  a slot is reported for its interests only, but an error or the peer's end always is. A host asks for
//             kStreamWritable only while a write is blocked, so an ordinary turn makes no system call to change it.
//   memory    a TcpPoller's tables come from the injected allocator at construction; nothing is allocated after.
//   cost      epoll on Linux; a dense poll set elsewhere, O(connections) per wait, the live ones only.
//   threads   one thread at a time.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/endpoint.hpp>
#include <crd/net/stream_socket.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
// The token wait() reports a listener's waiting connections under, and interest() takes for them.
inline constexpr u32 kStreamListener = ~u32{0};
// The most slots a TcpPoller holds.
inline constexpr u32 kStreamMaxSlots = (u32{1} << 20U) + 1U;

struct StreamReady
{
    u32 token = 0U; // a slot or kStreamListener
    u8 ready = 0U;  // kStreamReadable and kStreamWritable
};

class StreamPoller
{
public:
    virtual ~StreamPoller() = default;

    // Moves the next waiting connection into the free `slot`, reported when readable. Null when none was accepted, with
    // `error` Busy for none waiting or the host's reason.
    [[nodiscard]] virtual StreamSocket* accept(u32 slot, SocketError& error) noexcept = 0;
    // Starts a connection to `peer` in the free `slot`, reported when writable (made or failed). Null, the slot free,
    // when it could not start, with `error` naming why.
    [[nodiscard]] virtual StreamSocket* connect(u32 slot, const Endpoint& peer, SocketError& error) noexcept = 0;
    // None once a connect() stands, Busy while it is being made; any other error is why it failed.
    [[nodiscard]] virtual SocketError connected(u32 slot) noexcept = 0;
    // What the next wait() reports for `token`.
    virtual void interest(u32 token, u8 interests) noexcept = 0;
    // Ends the connection of `slot` and frees the slot: a FIN after what was written, or with `reset` no FIN and no
    // TIME_WAIT, for a peer the transport gives up on.
    virtual void close(u32 slot, bool reset) noexcept = 0;
    // Waits until something is ready or `timeout_ns` passes and fills `out`; the count, 0 when the time passed or a
    // signal interrupted the wait. A socket's bytes left for later are reported again.
    [[nodiscard]] virtual usize wait(cont::Span<StreamReady> out, u64 timeout_ns) noexcept = 0;

protected:
    StreamPoller() = default;
    StreamPoller(const StreamPoller&) = default;
    StreamPoller(StreamPoller&&) noexcept = default;
    StreamPoller& operator=(const StreamPoller&) = default;
    StreamPoller& operator=(StreamPoller&&) noexcept = default;
};

// The host's TCP.
class TcpPoller final : public StreamPoller
{
public:
    // `slots` connections at once; `listener` (open, or null for a host that only connects) outlives the poller, and
    // `connect_options` shape the connections connect() makes.
    TcpPoller(memory::IAllocator& allocator, u32 slots, TcpListener* listener,
              const TcpOptions& connect_options = {}) noexcept;
    ~TcpPoller() override;
    TcpPoller(const TcpPoller&) = delete;
    TcpPoller& operator=(const TcpPoller&) = delete;
    TcpPoller(TcpPoller&&) = delete;
    TcpPoller& operator=(TcpPoller&&) = delete;

    // False when the slots are out of range (0 or over kStreamMaxSlots), the listener is closed, or the host or the
    // allocator refused.
    [[nodiscard]] bool ready() const noexcept { return m_sockets != nullptr; }

    [[nodiscard]] StreamSocket* accept(u32 slot, SocketError& error) noexcept override;
    [[nodiscard]] StreamSocket* connect(u32 slot, const Endpoint& peer, SocketError& error) noexcept override;
    [[nodiscard]] SocketError connected(u32 slot) noexcept override;
    void interest(u32 token, u8 interests) noexcept override;
    void close(u32 slot, bool reset) noexcept override;
    [[nodiscard]] usize wait(cont::Span<StreamReady> out, u64 timeout_ns) noexcept override;

private:
    struct Native; // the host's set

    [[nodiscard]] bool open_set() noexcept;
    void close_set() noexcept;
    void release() noexcept;
    [[nodiscard]] bool watch(u32 token, u64 handle, u8 interests) noexcept;
    void forget(u32 token) noexcept;
    [[nodiscard]] u8& interests_of(u32 token) noexcept;

    memory::IAllocator& m_allocator;
    TcpListener* m_listener = nullptr;
    TcpOptions m_options;
    u32 m_slots = 0U;
    TcpSocket* m_sockets = nullptr;
    u8* m_interests = nullptr; // per slot, then the listener's
    Native* m_native = nullptr;
};

} // namespace crd::net
