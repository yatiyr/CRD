#pragma once

// What the I/O loop and server tests share (4.2a; docs/design/network-foundation.md#ingress): a test clock and a
// virtual socket, the two seams NET-SIM drives.

#include "ingress_fixture.hpp"

#include <crd/net/io_loop.hpp>
#include <crd/net/udp.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>

namespace crd::net::testing
{
class TestClock final : public TransportClock
{
public:
    [[nodiscard]] u64 monotonic_ns() const noexcept override { return now; }
    [[nodiscard]] u64 unix_seconds() const noexcept override { return 1793577600U + (now / kNanosPerSecond); }

    u64 now = kNanosPerSecond;
};

// A socket of fixed queues (on the heap: a test's stack holds the socket): what a test puts in arrives, what the loop
// sends is kept with its codepoint; `busy_after` stops sends with Busy after that many, `refuse_next` fails the next
// send for good, `offers` is what features() reports.
class VirtualSocket final : public DatagramSocket
{
public:
    static constexpr usize kQueue = 256U;

    void deliver(const Datagram& datagram, const Endpoint& peer, Ecn ecn = Ecn::NotEct)
    {
        if (m_inbound_count == kQueue && m_inbound_next != 0U)
        {
            // What was read leaves the queue; what waits moves to its front.
            for (usize index = m_inbound_next; index < m_inbound_count; ++index)
            {
                m_inbound[index - m_inbound_next] = m_inbound[index];
            }
            m_inbound_count -= m_inbound_next;
            m_inbound_next = 0U;
        }
        REQUIRE(m_inbound_count < kQueue);
        m_inbound[m_inbound_count++] = Inbound{datagram, peer, ecn};
    }

    // Datagrams delivered and not yet received.
    [[nodiscard]] usize pending() const noexcept { return m_inbound_count - m_inbound_next; }

    [[nodiscard]] Endpoint local_endpoint() const noexcept override { return local; }

    [[nodiscard]] BatchResult receive(cont::Span<ReceivedDatagram> slots) noexcept override
    {
        BatchResult result;
        while (result.count < slots.size() && m_inbound_next < m_inbound_count)
        {
            const Inbound& inbound = m_inbound[m_inbound_next++];
            ReceivedDatagram& slot = slots[result.count++];
            const usize size = inbound.datagram.size;
            slot.truncated = size > slot.storage.size();
            slot.size = slot.truncated ? slot.storage.size() : size;
            std::memcpy(slot.storage.data(), inbound.datagram.bytes, slot.size);
            slot.peer = inbound.peer;
            slot.ecn = inbound.ecn;
        }
        if (fail_receive)
        {
            result.error = SocketError::Failed;
        }
        return result;
    }

    [[nodiscard]] BatchResult send(cont::ConstSpan<OutgoingDatagram> datagrams) noexcept override
    {
        ++send_calls;
        BatchResult result;
        for (const OutgoingDatagram& datagram : datagrams)
        {
            if (refuse_next)
            {
                refuse_next = false;
                result.error = SocketError::Unreachable;
                return result;
            }
            if (sent_count == busy_after || sent_count == kQueue)
            {
                result.error = SocketError::Busy;
                return result;
            }
            Outbound& out = sent[sent_count++];
            std::memcpy(out.datagram.bytes, datagram.bytes.data(), datagram.bytes.size());
            out.datagram.size = datagram.bytes.size();
            out.peer = datagram.peer;
            out.ecn = datagram.ecn;
            ++result.count;
        }
        return result;
    }

    [[nodiscard]] bool wait_readable(u64) noexcept override
    {
        ++waits;
        return m_inbound_next < m_inbound_count;
    }

    [[nodiscard]] SocketFeatures features() const noexcept override { return offers; }

    struct Outbound
    {
        Datagram datagram;
        Endpoint peer;
        Ecn ecn = Ecn::NotEct;
    };

    // A busy host's turn: no send on one turn, one send on the next; unbounded sends when it is not busy.
    void throttle(bool busy, bool turn) noexcept
    {
        busy_after = ~usize{0};
        if (busy)
        {
            busy_after = turn ? 0U : 1U;
        }
    }

    Endpoint local{IpAddress::v4(10, 0, 0, 1), 443U};
    std::unique_ptr<Outbound[]> sent = std::make_unique<Outbound[]>(kQueue);
    usize sent_count = 0U;
    usize busy_after = ~usize{0};
    bool refuse_next = false;
    bool fail_receive = false;
    SocketFeatures offers;
    usize send_calls = 0U;
    usize waits = 0U;

private:
    struct Inbound
    {
        Datagram datagram;
        Endpoint peer;
        Ecn ecn = Ecn::NotEct;
    };

    std::unique_ptr<Inbound[]> m_inbound = std::make_unique<Inbound[]>(kQueue);
    usize m_inbound_count = 0U;
    usize m_inbound_next = 0U;
};

} // namespace crd::net::testing
