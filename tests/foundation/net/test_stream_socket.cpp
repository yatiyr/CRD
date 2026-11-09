// crd-net -- byte-stream sockets on the host's loopback (4.2a; docs/design/network-foundation.md#stream-sockets).

#include <crd/containers/array.hpp>
#include <crd/net/stream_socket.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::Endpoint;
using crd::net::IpAddress;
using crd::net::kStreamReadable;
using crd::net::kStreamWritable;
using crd::net::SocketError;
using crd::net::StreamResult;
using crd::net::TcpListener;
using crd::net::TcpListenOptions;
using crd::net::TcpOptions;
using crd::net::TcpSocket;

constexpr u64 kPatience = 5000000000U;

struct Connected
{
    TcpListener listener;
    TcpSocket client;
    TcpSocket server;

    explicit Connected(const IpAddress& address = IpAddress::v4_loopback(), bool dual_stack = true,
                       const Endpoint* peer = nullptr)
    {
        SocketError error = SocketError::None;
        TcpListenOptions options;
        options.bind.address = address;
        options.dual_stack = dual_stack;
        listener = TcpListener::open(options, error);
        REQUIRE(error == SocketError::None);
        Endpoint to = listener.local_endpoint();
        if (peer != nullptr)
        {
            to.address = peer->address;
        }
        client = TcpSocket::connect(to, TcpOptions{}, error);
        REQUIRE(error == SocketError::None);
        REQUIRE(listener.wait_readable(kPatience));
        server = listener.accept(error);
        REQUIRE(error == SocketError::None);
        REQUIRE(client.wait(kStreamWritable, kPatience) != 0U);
        REQUIRE(client.connected() == SocketError::None);
    }
};

StreamResult read_some(TcpSocket& socket, Span<u8> out)
{
    REQUIRE(socket.wait(kStreamReadable, kPatience) != 0U);
    return socket.read(out);
}

TEST_CASE("a TCP connection reports both ends and carries bytes each way", "[net][tcp]")
{
    Connected pair;
    CHECK(pair.client.peer_endpoint() == pair.listener.local_endpoint());
    CHECK(pair.server.peer_endpoint() == pair.client.local_endpoint());
    CHECK(pair.server.local_endpoint() == pair.listener.local_endpoint());

    const u8 ping[] = {1U, 2U, 3U};
    const StreamResult sent = pair.client.write(ConstSpan<u8>(ping));
    REQUIRE(sent.error == SocketError::None);
    REQUIRE(sent.bytes == sizeof(ping));
    u8 out[16] = {};
    const StreamResult got = read_some(pair.server, Span<u8>(out));
    REQUIRE(got.bytes == sizeof(ping));
    CHECK(out[2] == 3U);
    CHECK_FALSE(got.ended);

    const StreamResult back = pair.server.write(ConstSpan<u8>(ping, 1U));
    CHECK(back.bytes == 1U);
    CHECK(read_some(pair.client, Span<u8>(out)).bytes == 1U);
}

TEST_CASE("a TCP read with nothing waiting is Busy, and a wait for it times out", "[net][tcp]")
{
    Connected pair;
    u8 out[4];
    const StreamResult got = pair.client.read(Span<u8>(out));
    CHECK(got.bytes == 0U);
    CHECK(got.error == SocketError::Busy);
    CHECK_FALSE(got.ended);
    CHECK(pair.client.wait(kStreamReadable, 1000000U) == 0U);
    CHECK(pair.client.read(Span<u8>{}).error == SocketError::None);
    CHECK(pair.client.wait(kStreamWritable, kPatience) == kStreamWritable);
}

TEST_CASE("a TCP write past the host's buffers is Busy, and every byte arrives in order", "[net][tcp]")
{
    SocketError error = SocketError::None;
    TcpListenOptions options;
    options.bind.address = IpAddress::v4_loopback();
    options.connection.receive_buffer_bytes = 16384U;
    options.connection.send_buffer_bytes = 16384U;
    TcpListener listener = TcpListener::open(options, error);
    REQUIRE(error == SocketError::None);
    TcpOptions small;
    small.send_buffer_bytes = 16384U;
    small.receive_buffer_bytes = 16384U;
    TcpSocket client = TcpSocket::connect(listener.local_endpoint(), small, error);
    REQUIRE(error == SocketError::None);
    REQUIRE(listener.wait_readable(kPatience));
    TcpSocket server = listener.accept(error);
    REQUIRE(error == SocketError::None);
    REQUIRE(client.wait(kStreamWritable, kPatience) != 0U);
    REQUIRE(client.connected() == SocketError::None);

    crd::containers::Array<u8> pattern;
    pattern.resize(64U * 1024U);
    for (usize at = 0U; at < pattern.size(); ++at)
    {
        pattern[at] = static_cast<u8>(at * 13U);
    }
    usize offered = 0U;
    bool busy = false;
    for (usize turn = 0U; turn < 2000U && !busy; ++turn)
    {
        const usize at = offered % pattern.size(); // a partial write is followed from where it stopped
        const StreamResult sent = client.write(ConstSpan<u8>(pattern.data() + at, pattern.size() - at));
        offered += sent.bytes;
        busy = sent.error == SocketError::Busy;
        REQUIRE((sent.error == SocketError::None || busy));
    }
    REQUIRE(busy);
    REQUIRE(client.shutdown_send() == SocketError::None);

    crd::containers::Array<u8> in;
    in.resize(pattern.size());
    usize received = 0U;
    for (;;)
    {
        const StreamResult got = read_some(server, Span<u8>(in.data(), in.size()));
        REQUIRE((got.error == SocketError::None || got.error == SocketError::Busy));
        for (usize at = 0U; at < got.bytes; ++at)
        {
            REQUIRE(in[at] == pattern[(received + at) % pattern.size()]);
        }
        received += got.bytes;
        if (got.ended)
        {
            break;
        }
    }
    CHECK(received == offered);
}

TEST_CASE("a TCP peer's shutdown is a clean end, and the other direction stays open", "[net][tcp]")
{
    Connected pair;
    const u8 byte = 7U;
    REQUIRE(pair.client.write(ConstSpan<u8>(&byte, 1U)).bytes == 1U);
    REQUIRE(pair.client.shutdown_send() == SocketError::None);
    u8 out[4];
    REQUIRE(read_some(pair.server, Span<u8>(out)).bytes == 1U);
    const StreamResult end = read_some(pair.server, Span<u8>(out));
    CHECK(end.ended);
    CHECK(end.bytes == 0U);
    CHECK(end.error == SocketError::None);
    REQUIRE(pair.server.write(ConstSpan<u8>(&byte, 1U)).bytes == 1U);
    CHECK(read_some(pair.client, Span<u8>(out)).bytes == 1U);
}

TEST_CASE("a TCP peer that aborts is a Reset, and no write raises a signal", "[net][tcp]")
{
    Connected pair;
    pair.client.abort();
    CHECK_FALSE(pair.client.is_open());
    u8 out[4];
    const StreamResult read = read_some(pair.server, Span<u8>(out));
    CHECK(read.error == SocketError::Reset);
    CHECK_FALSE(read.ended);
    const u8 byte = 1U;
    const StreamResult write = pair.server.write(ConstSpan<u8>(&byte, 1U));
    CHECK((write.error == SocketError::Reset));
}

TEST_CASE("a TCP connection to a closed port is Refused", "[net][tcp]")
{
    SocketError error = SocketError::None;
    TcpListenOptions options;
    options.bind.address = IpAddress::v4_loopback();
    Endpoint unused;
    {
        TcpListener listener = TcpListener::open(options, error);
        REQUIRE(error == SocketError::None);
        unused = listener.local_endpoint();
    }
    TcpSocket socket = TcpSocket::connect(unused, TcpOptions{}, error);
    if (error == SocketError::None)
    {
        CHECK(socket.is_open());
        CHECK(socket.wait(kStreamWritable, kPatience) != 0U);
        error = socket.connected();
    }
    CHECK(error == SocketError::Refused);
}

TEST_CASE("a TCP listener holds its port and accepts Busy with none waiting", "[net][tcp]")
{
    SocketError error = SocketError::None;
    TcpListenOptions options;
    options.bind.address = IpAddress::v4_loopback();
    TcpListener listener = TcpListener::open(options, error);
    REQUIRE(error == SocketError::None);
    CHECK(listener.local_endpoint().port != 0U);

    TcpListenOptions same;
    same.bind = listener.local_endpoint();
    TcpListener second = TcpListener::open(same, error);
    CHECK(error == SocketError::AddressInUse);
    CHECK_FALSE(second.is_open());

    TcpSocket none = listener.accept(error);
    CHECK(error == SocketError::Busy);
    CHECK_FALSE(none.is_open());
    CHECK_FALSE(listener.wait_readable(1000000U));
}

TEST_CASE("a TCP listener's port comes back after its connections close", "[net][tcp]")
{
    Endpoint port;
    {
        Connected pair;
        port = pair.listener.local_endpoint();
        pair.server.close(); // the server closes first: its side holds TIME_WAIT
        u8 out[4];
        CHECK(read_some(pair.client, Span<u8>(out)).ended);
        pair.client.close();
    }
    SocketError error = SocketError::None;
    TcpListenOptions options;
    options.bind = port;
    TcpListener again = TcpListener::open(options, error);
    CHECK(error == SocketError::None);
    CHECK(again.is_open());
}

TEST_CASE("a dual-stack TCP listener reports an IPv4 peer as IPv4", "[net][tcp]")
{
    SocketError error = SocketError::None;
    TcpListenOptions probe;
    probe.bind.address = IpAddress::v6_loopback();
    const TcpListener usable = TcpListener::open(probe, error);
    if (error == SocketError::FamilyUnsupported || error == SocketError::AddressUnavailable)
    {
        SKIP("this host has no IPv6");
    }
    REQUIRE(error == SocketError::None);

    const Endpoint v4{IpAddress::v4_loopback(), 0U};
    Connected pair(IpAddress::v6_any(), true, &v4);
    CHECK(pair.server.peer_endpoint().address == IpAddress::v4_loopback());
    CHECK(pair.server.local_endpoint().address == IpAddress::v4_loopback());
    CHECK(pair.client.peer_endpoint().address == IpAddress::v4_loopback());
}

TEST_CASE("a closed or moved-from TCP socket refuses every call", "[net][tcp]")
{
    Connected pair;
    TcpSocket moved(static_cast<TcpSocket&&>(pair.client));
    CHECK(moved.is_open());
    CHECK_FALSE(pair.client.is_open());
    u8 out[4];
    CHECK(pair.client.read(Span<u8>(out)).error == SocketError::Closed);
    CHECK(pair.client.write(ConstSpan<u8>(out)).error == SocketError::Closed);
    CHECK(pair.client.shutdown_send() == SocketError::Closed);
    CHECK(pair.client.connected() == SocketError::Closed);
    CHECK(pair.client.wait(kStreamReadable, 0U) == kStreamReadable);
    TcpListener listener(static_cast<TcpListener&&>(pair.listener));
    CHECK_FALSE(pair.listener.is_open());
    CHECK_FALSE(pair.listener.wait_readable(0U));
    SocketError error = SocketError::None;
    const TcpSocket none = pair.listener.accept(error);
    CHECK(error == SocketError::Closed);
    CHECK_FALSE(none.is_open());
    moved.close();
    moved.close();
    moved.abort();
    CHECK_FALSE(moved.is_open());
}

} // namespace
