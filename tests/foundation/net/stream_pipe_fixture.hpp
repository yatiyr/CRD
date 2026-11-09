#pragma once

// What the stream path's host tests and fuzz target share (4.2a; docs/design/network-foundation.md#stream-host): byte
// pipes behind the StreamPoller seam, where NET-SIM's virtual streams will stand. Everything is level-triggered from
// the pipes' state, as the host's TCP is, and nothing waits: time is the test's.

#include <crd/containers/span.hpp>
#include <crd/net/stream_poller.hpp>

#include <cstring>
#include <memory>

namespace crd::net::testing
{
inline constexpr usize kPipeBytes = 128U * 1024U;
inline constexpr usize kPipeLinks = 64U;

// One direction of a pipe. A writer stops at `limit` bytes queued: a test's full socket.
struct PipeHalf
{
    u8 bytes[kPipeBytes] = {};
    usize head = 0U;
    usize size = 0U;
    usize limit = kPipeBytes;
    bool ended = false;
    bool reset = false;
    bool reader_gone = false; // the host answers a write to a closed peer with a reset

    [[nodiscard]] usize room() const noexcept { return (size < limit) ? limit - size : 0U; }
};

class PipeSocket final : public StreamSocket
{
public:
    void bind(PipeHalf* in, PipeHalf* out, const Endpoint& local, const Endpoint& peer) noexcept
    {
        m_in = in;
        m_out = out;
        m_local = local;
        m_peer = peer;
    }

    [[nodiscard]] Endpoint local_endpoint() const noexcept override { return m_local; }
    [[nodiscard]] Endpoint peer_endpoint() const noexcept override { return m_peer; }

    [[nodiscard]] StreamResult read(cont::Span<u8> out) noexcept override
    {
        StreamResult result;
        if (m_in->reset)
        {
            result.error = SocketError::Reset;
            return result;
        }
        usize wanted = (out.size() < m_in->size) ? out.size() : m_in->size;
        while (wanted != 0U)
        {
            const usize run = (wanted < kPipeBytes - m_in->head) ? wanted : kPipeBytes - m_in->head;
            std::memcpy(out.data() + result.bytes, m_in->bytes + m_in->head, run);
            m_in->head = (m_in->head + run) % kPipeBytes;
            m_in->size -= run;
            result.bytes += run;
            wanted -= run;
        }
        if (m_in->size == 0U && m_in->ended)
        {
            result.ended = true;
        }
        else if (result.bytes == 0U)
        {
            result.error = SocketError::Busy;
        }
        return result;
    }

    [[nodiscard]] StreamResult write(cont::ConstSpan<u8> bytes) noexcept override
    {
        StreamResult result;
        if (m_out->reset || m_out->ended || m_out->reader_gone || write_error != SocketError::None)
        {
            result.error = (write_error != SocketError::None) ? write_error : SocketError::Reset;
            return result;
        }
        ++writes;
        usize wanted = (bytes.size() < m_out->room()) ? bytes.size() : m_out->room();
        if (wanted == 0U)
        {
            result.error = SocketError::Busy;
            return result;
        }
        while (wanted != 0U)
        {
            const usize tail = (m_out->head + m_out->size) % kPipeBytes;
            const usize run = (wanted < kPipeBytes - tail) ? wanted : kPipeBytes - tail;
            std::memcpy(m_out->bytes + tail, bytes.data() + result.bytes, run);
            m_out->size += run;
            result.bytes += run;
            wanted -= run;
        }
        return result;
    }

    [[nodiscard]] SocketError shutdown_send() noexcept override
    {
        m_out->ended = true;
        return SocketError::None;
    }

    [[nodiscard]] u8 wait(u8 interests, u64) noexcept override { return interests; }

    [[nodiscard]] PipeHalf* in() noexcept { return m_in; }
    [[nodiscard]] PipeHalf* out() noexcept { return m_out; }

    SocketError write_error = SocketError::None;
    usize writes = 0U;

private:
    PipeHalf* m_in = nullptr;
    PipeHalf* m_out = nullptr;
    Endpoint m_local;
    Endpoint m_peer;
};

struct PipeLink
{
    PipeHalf to_server;
    PipeHalf to_client;
    PipeSocket client;
    PipeSocket server;
    bool client_closed = false;
    bool server_closed = false;
};

class PipePoller;

// Holds every link of a test and the one listening poller.
class PipeNetwork
{
public:
    [[nodiscard]] PipeLink* link() noexcept
    {
        if (m_links == kPipeLinks)
        {
            return nullptr;
        }
        m_storage[m_links] = std::make_unique<PipeLink>();
        return m_storage[m_links++].get();
    }

    PipePoller* listener = nullptr;
    SocketError connect_error = SocketError::None;   // connect() refuses at once
    SocketError connected_error = SocketError::None; // connect() starts, then fails
    bool connects_hang = false;                      // connect() starts and is never made
    bool connects_busy = false;                      // connect() is reported ready but is not made yet
    usize limit = kPipeBytes;                        // how many bytes a new link's halves queue

private:
    std::unique_ptr<PipeLink> m_storage[kPipeLinks];
    usize m_links = 0U;
};

class PipePoller final : public StreamPoller
{
public:
    static constexpr u32 kSlots = 32U;

    PipePoller(PipeNetwork& network, const Endpoint& self) noexcept : m_network(network), m_self(self) {}

    [[nodiscard]] StreamSocket* accept(u32 slot, SocketError& error) noexcept override
    {
        if (accept_error != SocketError::None)
        {
            error = accept_error;
            return nullptr;
        }
        if (m_waiting == 0U)
        {
            error = SocketError::Busy;
            return nullptr;
        }
        PipeLink* const link = m_queue[0];
        --m_waiting;
        for (usize at = 0U; at < m_waiting; ++at)
        {
            m_queue[at] = m_queue[at + 1U];
        }
        error = SocketError::None;
        m_slots[slot] = Slot{link, kStreamReadable, true, false};
        ++accepts;
        return &link->server;
    }

    [[nodiscard]] StreamSocket* connect(u32 slot, const Endpoint& peer, SocketError& error) noexcept override
    {
        error = m_network.connect_error;
        if (error != SocketError::None)
        {
            return nullptr;
        }
        PipeLink* const link = m_network.link();
        PipePoller* const listener = m_network.listener;
        if (link == nullptr || listener == nullptr || listener->m_waiting == kSlots)
        {
            error = SocketError::Resources;
            return nullptr;
        }
        link->to_server.limit = m_network.limit;
        link->to_client.limit = m_network.limit;
        link->client.bind(&link->to_client, &link->to_server, m_self, peer);
        link->server.bind(&link->to_server, &link->to_client, peer, m_self);
        listener->m_queue[listener->m_waiting++] = link;
        m_slots[slot] = Slot{link, kStreamWritable, false, true};
        return &link->client;
    }

    [[nodiscard]] SocketError connected(u32 slot) noexcept override
    {
        if (m_network.connects_hang || m_network.connects_busy)
        {
            return SocketError::Busy;
        }
        if (m_network.connected_error != SocketError::None)
        {
            return m_network.connected_error;
        }
        m_slots[slot].connecting = false;
        return SocketError::None;
    }

    void interest(u32 token, u8 interests) noexcept override
    {
        ++interest_calls;
        if (token == kStreamListener)
        {
            listener_interest = interests;
            return;
        }
        m_slots[token].interests = interests;
    }

    void close(u32 slot, bool reset) noexcept override
    {
        Slot& entry = m_slots[slot];
        PipeSocket& socket = entry.server ? entry.link->server : entry.link->client;
        (entry.server ? entry.link->server_closed : entry.link->client_closed) = true;
        if (reset)
        {
            socket.out()->reset = true;
            socket.in()->reset = true;
            ++resets;
        }
        else
        {
            socket.out()->ended = true;
            socket.in()->reader_gone = true;
            ++fins;
        }
        entry = Slot{};
    }

    [[nodiscard]] usize wait(cont::Span<StreamReady> out, u64 timeout_ns) noexcept override
    {
        last_timeout_ns = timeout_ns;
        usize filled = 0U;
        if (m_waiting != 0U && (listener_interest & kStreamReadable) != 0U && filled < out.size())
        {
            out[filled++] = StreamReady{kStreamListener, kStreamReadable};
        }
        for (u32 slot = 0U; slot < kSlots && filled < out.size(); ++slot)
        {
            const u8 ready = readiness(m_slots[slot]);
            if (ready != 0U)
            {
                out[filled++] = StreamReady{slot, ready};
            }
        }
        return filled;
    }

    [[nodiscard]] PipeLink* link_of(u32 slot) noexcept { return m_slots[slot].link; }
    [[nodiscard]] u8 interests_of(u32 slot) const noexcept { return m_slots[slot].interests; }
    [[nodiscard]] bool holds(u32 slot) const noexcept { return m_slots[slot].link != nullptr; }

    u8 listener_interest = kStreamReadable;
    SocketError accept_error = SocketError::None;
    usize interest_calls = 0U;
    usize accepts = 0U;
    usize resets = 0U;
    usize fins = 0U;
    u64 last_timeout_ns = 0U;

private:
    struct Slot
    {
        PipeLink* link = nullptr;
        u8 interests = 0U;
        bool server = false;
        bool connecting = false;
    };

    [[nodiscard]] u8 readiness(const Slot& slot) const noexcept
    {
        if (slot.link == nullptr)
        {
            return 0U;
        }
        const PipeHalf& in = slot.server ? slot.link->to_server : slot.link->to_client;
        const PipeHalf& out = slot.server ? slot.link->to_client : slot.link->to_server;
        if (slot.connecting)
        {
            return m_network.connects_hang ? u8{0U} : kStreamWritable;
        }
        if (in.reset || out.reset)
        {
            return slot.interests; // an error is always reported
        }
        u8 ready = 0U;
        if ((in.size != 0U || in.ended) && (slot.interests & kStreamReadable) != 0U)
        {
            ready = static_cast<u8>(ready | kStreamReadable);
        }
        if (out.room() != 0U && (slot.interests & kStreamWritable) != 0U)
        {
            ready = static_cast<u8>(ready | kStreamWritable);
        }
        return ready;
    }

    PipeNetwork& m_network;
    Endpoint m_self;
    Slot m_slots[kSlots];
    PipeLink* m_queue[kSlots] = {};
    usize m_waiting = 0U;
};

} // namespace crd::net::testing
