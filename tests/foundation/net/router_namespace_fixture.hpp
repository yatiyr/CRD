#pragma once

// A router between private network namespaces, for crd-net's real-kernel path tests (NET-PATH;
// docs/design/network-foundation.md#pmtu). The cases tagged [.netns] need the right to make namespaces, and CTest's
// crd-net-router-namespace runs them under `unshare -Urn`. The test's own namespace is the near host, 10.71.1.2 and
// fd71:1::2; a second is a router, whose route toward the third, the far host at 10.71.2.2 and fd71:2::2, carries
// kHopMtu bytes. A datagram larger than that from the near host draws the router's ICMP Packet Too Big (IPv4's
// Fragmentation Needed, ICMPv6's Packet Too Big) at the router's own rate, which is unlimited here, while the far
// host's datagrams cross at the 1500 bytes of every other link. Linux only.

#include <crd/core/platform.hpp>

#if !CRD_OS_LINUX
#error "network namespaces are Linux's"
#endif

#include <crd/net/udp.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

namespace crd::net::testing
{

class RouterPath
{
public:
    static constexpr u32 kHopMtu = 1280U;
    static constexpr u32 kLinkMtu = 1500U;

    enum Host : u8
    {
        Near,
        Router,
        Far,
        kHosts,
    };

    // The one topology of the process, made on first use: a namespace's devices outlive its last reference for a
    // moment, so a second topology would meet the first one's names.
    static RouterPath& get()
    {
        static RouterPath path;
        return path;
    }

    RouterPath(const RouterPath&) = delete;
    RouterPath& operator=(const RouterPath&) = delete;

    ~RouterPath()
    {
        for (const int ns : m_ns)
        {
            ::close(ns);
        }
    }

    // Where the near host's datagrams larger than kHopMtu are dropped by the host itself, past the socket.
    [[nodiscard]] static Endpoint dropping(bool v6)
    {
        constexpr u8 kV6[16] = {0xFDU, 0x71U, 0U, 3U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U};
        return Endpoint{v6 ? IpAddress::v6(kV6) : IpAddress::v4(10U, 71U, 3U, 2U), 9U};
    }

    [[nodiscard]] static Endpoint at(Host host, bool v6, u16 port)
    {
        const u8 subnet = host == Near ? 1U : 2U;
        const u8 last = host == Router ? 1U : 2U;
        if (v6)
        {
            u8 bytes[16] = {0xFDU, 0x71U, 0U, subnet};
            bytes[15] = last;
            return Endpoint{IpAddress::v6(bytes), port};
        }
        return Endpoint{IpAddress::v4(10U, 71U, subnet, last), port};
    }

    // A socket opened inside `host`'s namespace, where it stays. Port 0 takes an ephemeral one; `any` binds the
    // unspecified address of the family (an IPv6 one carrying IPv4 too).
    [[nodiscard]] UdpSocket open(Host host, bool v6, bool any = false)
    {
        UdpOptions options;
        options.bind = any ? Endpoint{v6 ? IpAddress::v6_any() : IpAddress::v4_any(), 0U} : at(host, v6, 0U);
        options.receive_buffer_bytes = 1U << 21U;
        options.send_buffer_bytes = 1U << 21U;
        enter(host);
        SocketError error = SocketError::Failed;
        UdpSocket socket = UdpSocket::open(options, error);
        enter(Near);
        REQUIRE(error == SocketError::None);
        return socket;
    }

private:
    RouterPath()
    {
        m_ns[Near] = current();
        for (const Host host : {Router, Far})
        {
            REQUIRE(::unshare(CLONE_NEWNET) == 0);
            m_ns[host] = current();
            enter(Near);
        }
        // Addresses without duplicate detection, and ICMP errors without a rate limit, in every namespace.
        const char* const common = "ip link set lo up && sysctl -qw net.ipv6.conf.all.accept_dad=0 "
                                   "net.ipv6.conf.default.accept_dad=0 net.ipv4.icmp_ratelimit=0 "
                                   "net.ipv6.icmp.ratelimit=0";
        for (const Host host : {Near, Router, Far})
        {
            run(host, "%s", common);
        }
        run(Near,
            "ip link add a0 type veth peer name r0 netns %s && ip link set a0 up && "
            "ip addr add 10.71.1.2/24 dev a0 && ip -6 addr add fd71:1::2/64 dev a0 nodad && "
            "ip route add default via 10.71.1.1 && ip -6 route add default via fd71:1::1",
            path(Router));
        // A link into nothing: the near host reaches 10.71.3.2 and fd71:3::2 through n0, whose other end takes only
        // kHopMtu bytes, so the host itself drops a larger datagram after the socket has sent it.
        run(Near,
            "ip link add n0 type veth peer name n1 mtu %u && ip link set n0 up && ip link set n1 up && "
            "ip addr add 10.71.3.1/24 dev n0 && ip -6 addr add fd71:3::1/64 dev n0 nodad && "
            "ip neigh add 10.71.3.2 lladdr 02:00:00:00:00:01 dev n0 nud permanent && "
            "ip -6 neigh add fd71:3::2 lladdr 02:00:00:00:00:01 dev n0 nud permanent",
            kHopMtu);
        // The hop is the router's routes toward the far host, each in place of the link's own (IPv6's at its metric)
        // and locked to kHopMtu: a veth pair drops a frame larger than its receiving end's MTU, so the link itself
        // stays at 1500 and carries the far host's datagrams back.
        run(Router,
            "ip link add r1 type veth peer name b0 netns %s && ip link set r0 up && ip link set r1 up && "
            "ip addr add 10.71.1.1/24 dev r0 && ip -6 addr add fd71:1::1/64 dev r0 nodad && "
            "ip addr add 10.71.2.1/24 dev r1 && ip -6 addr add fd71:2::1/64 dev r1 nodad && "
            "ip route replace 10.71.2.0/24 dev r1 mtu lock %u && "
            "ip -6 route replace fd71:2::/64 dev r1 metric 256 mtu lock %u && "
            "sysctl -qw net.ipv4.ip_forward=1 net.ipv6.conf.all.forwarding=1",
            path(Far), kHopMtu, kHopMtu);
        run(Far, "ip link set b0 up && ip addr add 10.71.2.2/24 dev b0 && ip -6 addr add fd71:2::2/64 dev b0 nodad && "
                 "ip route add default via 10.71.2.1 && ip -6 route add default via fd71:2::1");
        // Neighbour discovery and the links' carrier take a moment: small datagrams cross both ways first.
        for (const bool v6 : {false, true})
        {
            REQUIRE(crosses(v6));
        }
    }

    [[nodiscard]] static int current()
    {
        const int ns = ::open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
        REQUIRE(ns >= 0);
        return ns;
    }

    void enter(Host host) const { REQUIRE(::setns(m_ns[host], CLONE_NEWNET) == 0); }

    // The path `ip ... netns` takes for `host`'s namespace: this process's descriptor of it.
    [[nodiscard]] const char* path(Host host)
    {
        std::snprintf(m_path, sizeof(m_path), "/proc/%d/fd/%d", static_cast<int>(::getpid()), m_ns[host]);
        return m_path;
    }

    // Runs a shell command inside `host`'s namespace, which its process inherits from the calling thread.
    __attribute__((format(printf, 3, 4))) void run(Host host, const char* format, ...)
    {
        char command[1024];
        std::va_list args;
        va_start(args, format);
        const int length = std::vsnprintf(command, sizeof(command), format, args);
        va_end(args);
        REQUIRE((length > 0 && static_cast<usize>(length) < sizeof(command)));
        enter(host);
        const int status = std::system(command);
        enter(Near);
        INFO(command);
        REQUIRE(status == 0);
    }

    [[nodiscard]] bool crosses(bool v6)
    {
        UdpSocket near = open(Near, v6);
        UdpSocket far = open(Far, v6);
        u8 byte = 7U;
        u8 slot_bytes[64];
        ReceivedDatagram slot;
        slot.storage = cont::Span<u8>(slot_bytes, sizeof(slot_bytes));
        for (u32 attempt = 0U; attempt < 100U; ++attempt)
        {
            const OutgoingDatagram there[] = {{cont::ConstSpan<u8>(&byte, 1U), far.local_endpoint()}};
            static_cast<void>(near.send(there));
            if (far.wait_readable(50'000'000U) && far.receive(cont::Span<ReceivedDatagram>(&slot, 1U)).count == 1U)
            {
                const OutgoingDatagram back[] = {{cont::ConstSpan<u8>(&byte, 1U), slot.peer}};
                static_cast<void>(far.send(back));
                if (near.wait_readable(50'000'000U) &&
                    near.receive(cont::Span<ReceivedDatagram>(&slot, 1U)).count == 1U)
                {
                    return true;
                }
            }
        }
        return false;
    }

    int m_ns[kHosts] = {-1, -1, -1};
    char m_path[64] = {};
};

} // namespace crd::net::testing
