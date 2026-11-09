// crd-net-tests: HTTP/2 (4.2a; docs/design/network-foundation.md#http2): the preface and SETTINGS, every frame type
// with every refusal RFC 9113 names, stream states and identifiers, CONTINUATION, flow control both ways with padding
// and retroactive SETTINGS, messages through the checks HTTP/3 shares, GOAWAY both ways, extended CONNECT, the flood
// limits (rapid reset, provoked resets, SETTINGS, PING, PRIORITY, empty frames, tiny updates) and a peer that does not
// read.

#include "ingress_fixture.hpp"
#include "text_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/net/http2.hpp>

#include <catch2/catch_test_macros.hpp>
#include <initializer_list>

namespace
{
using namespace crd;
using namespace crd::net;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::testing::CountingAllocator;
using namespace crd::net::testing;
namespace cont = crd::containers;

constexpr u8 kData = 0U;
constexpr u8 kHeaders = 1U;
constexpr u8 kPriority = 2U;
constexpr u8 kRst = 3U;
constexpr u8 kSettings = 4U;
constexpr u8 kPush = 5U;
constexpr u8 kPing = 6U;
constexpr u8 kGoAway = 7U;
constexpr u8 kWindow = 8U;
constexpr u8 kContinuation = 9U;
constexpr u8 kEndStream = 1U;
constexpr u8 kAck = 1U;
constexpr u8 kEndHeaders = 4U;
constexpr u8 kPadded = 8U;
constexpr u8 kPrioritized = 0x20U;
constexpr char kPrefaceText[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr u64 kSecond = 1'000'000'000U;

struct Fields
{
    Fields(std::initializer_list<TextPair> lines)
    {
        for (const TextPair& line : lines)
        {
            pairs.push_back(line);
        }
        for (const TextPair& line : pairs)
        {
            fields.push_back(HttpField{view(line.first), view(line.second), false});
        }
    }

    [[nodiscard]] ConstSpan<HttpField> span() const { return ConstSpan<HttpField>(fields.data(), fields.size()); }

    cont::Array<TextPair> pairs;
    cont::Array<HttpField> fields;
};

Fields get_request()
{
    return {{":method", "GET"}, {":scheme", "https"}, {":authority", "a.example"}, {":path", "/"}};
}

Fields post_request()
{
    return {{":method", "POST"}, {":scheme", "https"}, {":authority", "a.example"}, {":path", "/up"}};
}

Fields ok_response()
{
    return {{":status", "200"}};
}

struct Seen
{
    Http2EventKind kind = Http2EventKind::Settings;
    u32 stream = 0U;
    u32 error = 0U;
    Text fields;
    Text payload;
    u16 status = 0U;
};

struct WireFrame
{
    u8 type = 0U;
    u8 flags = 0U;
    u32 stream = 0U;
    Text payload;
};

bool holds(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

Text u32_text(u32 value)
{
    const char bytes[4] = {static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
                           static_cast<char>(value >> 8U), static_cast<char>(value)};
    return Text(bytes, 4U);
}

u32 u32_of(std::string_view bytes, usize at = 0U)
{
    return (u32{static_cast<u8>(bytes[at])} << 24U) | (u32{static_cast<u8>(bytes[at + 1U])} << 16U) |
           (u32{static_cast<u8>(bytes[at + 2U])} << 8U) | u32{static_cast<u8>(bytes[at + 3U])};
}

Text frame(u8 type, u8 flags, u32 stream, std::string_view payload = {})
{
    const usize length = payload.size();
    const char head[5] = {static_cast<char>(length >> 16U), static_cast<char>(length >> 8U), static_cast<char>(length),
                          static_cast<char>(type), static_cast<char>(flags)};
    Text out(head, 5U);
    out += u32_text(stream);
    out += Text(payload);
    return out;
}

Text setting(u16 id, u32 value)
{
    const char head[2] = {static_cast<char>(id >> 8U), static_cast<char>(id)};
    Text out(head, 2U);
    out += u32_text(value);
    return out;
}

Text zeros(usize count)
{
    Text out;
    out.resize(count);
    for (usize at = 0U; at < count; ++at)
    {
        out.data()[at] = 0;
    }
    return out;
}

Http2Config test_config(Http2Side side)
{
    Http2Config config;
    config.side = side;
    config.field_bytes = 4096U;
    config.streams = 8U;
    config.stream_window = kHttp2InitialWindow;
    config.connection_window = kHttp2InitialWindow;
    config.resets = GateRule{1'000'000U, kSecond};
    config.control = GateRule{1'000'000U, kSecond};
    return config;
}

// A connection under test: what it is fed, what it reported and what it wrote.
struct Endpoint
{
    explicit Endpoint(const Http2Config& config) { REQUIRE(conn.init(allocator, config, now)); }

    void pump()
    {
        for (;;)
        {
            usize taken = 0U;
            Http2Event event;
            const bool more = conn.poll(view(in), now, taken, event);
            Seen seen;
            seen.kind = event.kind;
            seen.stream = event.stream;
            seen.error = event.error;
            seen.fields = joined(event.fields);
            seen.payload = text_of(event.payload);
            seen.status = event.info.status;
            erase_front(in, taken);
            if (!more)
            {
                return;
            }
            if (event.kind == Http2EventKind::Body && auto_consume)
            {
                CHECK(conn.consume_body(event.stream, seen.payload.size()) == Http2Error::None);
            }
            events.push_back(seen);
        }
    }

    void read(std::string_view bytes)
    {
        in += Text(bytes);
        pump();
    }

    // Moves what it wrote out and parses it as frames.
    void drain()
    {
        const ConstSpan<u8> bytes = conn.output();
        written += text_of(bytes);
        conn.output_taken(bytes.size());
        for (;;)
        {
            if (written.size() < 9U)
            {
                return;
            }
            const usize length = (usize{static_cast<u8>(written.data()[0])} << 16U) |
                                 (usize{static_cast<u8>(written.data()[1])} << 8U) | static_cast<u8>(written.data()[2]);
            if (written.size() < 9U + length)
            {
                return;
            }
            WireFrame parsed;
            parsed.type = static_cast<u8>(written.data()[3]);
            parsed.flags = static_cast<u8>(written.data()[4]);
            parsed.stream = u32_of(written, 5U);
            parsed.payload = slice(written, 9U, length);
            frames.push_back(parsed);
            erase_front(written, 9U + length);
        }
    }

    [[nodiscard]] usize count(Http2EventKind kind) const
    {
        usize total = 0U;
        for (const Seen& seen : events)
        {
            total += (seen.kind == kind) ? 1U : 0U;
        }
        return total;
    }

    [[nodiscard]] const Seen* last(Http2EventKind kind) const
    {
        const Seen* found = nullptr;
        for (const Seen& seen : events)
        {
            found = (seen.kind == kind) ? &seen : found;
        }
        return found;
    }

    [[nodiscard]] const WireFrame* last_frame(u8 type) const
    {
        const WireFrame* found = nullptr;
        for (const WireFrame& parsed : frames)
        {
            found = (parsed.type == type) ? &parsed : found;
        }
        return found;
    }

    [[nodiscard]] usize frame_count(u8 type) const
    {
        usize total = 0U;
        for (const WireFrame& parsed : frames)
        {
            total += (parsed.type == type) ? 1U : 0U;
        }
        return total;
    }

    CountingAllocator allocator;
    Http2Connection conn;
    Text in;
    Text written;
    cont::Array<Seen> events;
    cont::Array<WireFrame> frames;
    u64 now = 1U;
    bool auto_consume = true;
};

// A connection under test and a peer written by hand, past the opening of the connection.
struct Peer
{
    explicit Peer(const Http2Config& config, bool settle = true) : side(config)
    {
        REQUIRE(encoder.init(allocator, 4096U));
        if (config.side == Http2Side::Server)
        {
            side.read(kPrefaceText);
        }
        else
        {
            side.conn.output_taken(kHttp2PrefaceBytes); // drain() reads frames
        }
        side.read(frame(kSettings, 0U, 0U));
        if (settle)
        {
            side.read(frame(kSettings, kAck, 0U));
        }
        side.drain();
        side.events.clear();
        side.frames.clear();
    }

    Text block(const Fields& fields)
    {
        cont::Array<u8> out;
        out.resize(HpackEncoder::max_bytes(fields.span()));
        const usize used = encoder.encode(fields.span(), Span<u8>(out.data(), out.size()));
        return text_of(ConstSpan<u8>(out.data(), used));
    }

    void send(u8 type, u8 flags, u32 stream, std::string_view payload = {})
    {
        side.read(frame(type, flags, stream, payload));
        side.drain();
    }

    void headers(u32 stream, const Fields& fields, u8 extra = kEndStream)
    {
        const Text bytes = block(fields);
        send(kHeaders, static_cast<u8>(kEndHeaders | extra), stream, bytes);
    }

    void get(u32 stream) { headers(stream, get_request()); }

    [[nodiscard]] u32 error() const { return side.conn.error(); }

    // The GOAWAY a connection error wrote.
    [[nodiscard]] u32 goaway_code() const
    {
        const WireFrame* const found = side.last_frame(kGoAway);
        return (found == nullptr) ? ~u32{0} : u32_of(found->payload, 4U);
    }

    [[nodiscard]] u32 reset_code(u32 stream) const
    {
        u32 code = ~u32{0};
        for (const WireFrame& parsed : side.frames)
        {
            if (parsed.type == kRst && parsed.stream == stream)
            {
                code = u32_of(parsed.payload);
            }
        }
        return code;
    }

    CountingAllocator allocator;
    HpackEncoder encoder;
    Endpoint side;
};

Peer server_peer()
{
    return Peer(test_config(Http2Side::Server));
}

// Both ends, honest, joined by a wire that moves `cut` bytes at a time.
struct Pair
{
    explicit Pair(usize piece = ~usize{0}, u32 window = kHttp2InitialWindow)
        : client([&] {
              Http2Config config = test_config(Http2Side::Client);
              config.stream_window = window;
              return config;
          }()),
          server([&] {
              Http2Config config = test_config(Http2Side::Server);
              config.stream_window = window;
              return config;
          }()),
          cut(piece)
    {
        run();
    }

    static void feed(Endpoint& from, Endpoint& to, usize cut)
    {
        const ConstSpan<u8> bytes = from.conn.output();
        const usize count = std::min(bytes.size(), cut);
        to.in += text_of(bytes.first(count));
        from.conn.output_taken(count);
        to.pump();
    }

    void run()
    {
        for (u32 round = 0U; round < 1'000'000U; ++round)
        {
            if (client.conn.output().empty() && server.conn.output().empty())
            {
                client.pump();
                server.pump();
                if (client.conn.output().empty() && server.conn.output().empty())
                {
                    return;
                }
            }
            feed(client, server, cut);
            feed(server, client, cut);
        }
        FAIL("the wire never settled");
    }

    Endpoint client;
    Endpoint server;
    usize cut;
};

TEST_CASE("http2: a connection opens with the preface and SETTINGS", "[http2]")
{
    Endpoint client(test_config(Http2Side::Client));
    const Text out = text_of(client.conn.output());
    REQUIRE(out.size() > kHttp2PrefaceBytes);
    CHECK(slice(out, 0U, kHttp2PrefaceBytes) == kPrefaceText);
    CHECK(static_cast<u8>(out.data()[kHttp2PrefaceBytes + 3U]) == kSettings);

    Endpoint server(test_config(Http2Side::Server));
    server.drain();
    REQUIRE(server.frames.size() == 1U);
    CHECK(server.frames[0].type == kSettings);
    CHECK(holds(server.frames[0].payload, setting(3U, 8U)));
    CHECK(holds(server.frames[0].payload, setting(6U, 4096U)));
    CHECK(holds(server.frames[0].payload, setting(9U, 1U)));
    CHECK_FALSE(holds(server.frames[0].payload, setting(2U, 0U)));
    CHECK(holds(text_of(client.conn.output()), setting(2U, 0U)));

    Http2Config own = test_config(Http2Side::Server);
    own.stream_window = 70000U;
    own.frame_bytes = 20000U;
    own.table_bytes = 512U;
    own.connect_protocol = true;
    Endpoint custom(own);
    custom.drain();
    REQUIRE(custom.frames.size() == 1U);
    CHECK(holds(custom.frames[0].payload, setting(1U, 512U)));
    CHECK(holds(custom.frames[0].payload, setting(4U, 70000U)));
    CHECK(holds(custom.frames[0].payload, setting(5U, 20000U)));
    CHECK(holds(custom.frames[0].payload, setting(8U, 1U)));

    Pair pair;
    CHECK(pair.client.conn.settings_known());
    CHECK(pair.server.conn.settings_known());
    CHECK(pair.client.count(Http2EventKind::Settings) == 1U);
    CHECK(pair.server.conn.next_deadline_ns() == kNeverNs);
}

TEST_CASE("http2: a configuration out of range or a refused allocation holds nothing", "[http2]")
{
    const auto refused = [](auto change)
    {
        CountingAllocator allocator;
        Http2Connection connection;
        Http2Config config = test_config(Http2Side::Server);
        change(config);
        return !connection.init(allocator, config, 0U) && allocator.calls() == 0U;
    };
    CHECK(refused([](Http2Config& c) { c.field_bytes = kHttp2MinFieldBytes - 1U; }));
    CHECK(refused([](Http2Config& c) { c.field_bytes = kHttp2MaxFieldBytes + 1U; }));
    CHECK(refused([](Http2Config& c) { c.streams = 0U; }));
    CHECK(refused([](Http2Config& c) { c.streams = kHttp2MaxStreams + 1U; }));
    CHECK(refused([](Http2Config& c) { c.stream_window = 0U; }));
    CHECK(refused([](Http2Config& c) { c.stream_window = kHttp2MaxWindow + 1U; }));
    CHECK(refused([](Http2Config& c) { c.connection_window = kHttp2InitialWindow - 1U; }));
    CHECK(refused([](Http2Config& c) { c.connection_window = kHttp2MaxWindow + 1U; }));
    CHECK(refused([](Http2Config& c) { c.frame_bytes = kHttp2MinFrameBytes - 1U; }));
    CHECK(refused([](Http2Config& c) { c.frame_bytes = kHttp2MaxFrameBytes + 1U; }));
    CHECK(refused([](Http2Config& c) { c.table_bytes = kHpackMaxTableSize + 1U; }));
    CHECK(refused([](Http2Config& c) { c.encoder_table_bytes = kHpackMaxTableSize + 1U; }));
    CHECK(refused([](Http2Config& c) { c.output_bytes = kHttp2MinOutputBytes - 1U; }));
    CHECK(refused([](Http2Config& c) { c.resets.per_second = 0U; }));
    CHECK(refused([](Http2Config& c) { c.control.per_second = 0U; }));
    CHECK(refused([](Http2Config& c) { c.settings_timeout_ns = 0U; }));
    CHECK(refused(
        [](Http2Config& c)
        {
            c.side = Http2Side::Client;
            c.connect_protocol = true;
        }));
    for (usize calls = 0U; calls < 16U; ++calls)
    {
        CountingAllocator allocator;
        allocator.refuse_after(calls);
        Http2Connection connection;
        const bool done = connection.init(allocator, test_config(Http2Side::Server), 0U);
        if (done)
        {
            CHECK(calls >= 9U);
            break;
        }
    }
}

TEST_CASE("http2: a request and its response with a body and trailers", "[http2]")
{
    for (const usize piece : {usize{1}, usize{7}, ~usize{0}})
    {
        Pair pair(piece);
        u32 id = 0U;
        REQUIRE(pair.client.conn.open_request(post_request().span(), false, id) == Http2Error::None);
        CHECK(id == 1U);
        pair.run();
        const Seen* request = pair.server.last(Http2EventKind::Headers);
        REQUIRE(request != nullptr);
        CHECK(request->fields == text_of(view(":method=POST\n:scheme=https\n:authority=a.example\n:path=/up\n")));
        usize taken = 0U;
        REQUIRE(pair.client.conn.send_body(id, view("hello"), false, taken) == Http2Error::None);
        CHECK(taken == 5U);
        REQUIRE(pair.client.conn.send_trailers(id, Fields{{"x-sum", "1"}}.span()) == Http2Error::None);
        pair.run();
        CHECK(pair.server.last(Http2EventKind::Trailers)->fields == text_of(view("x-sum=1\n")));
        Text body;
        for (const Seen& seen : pair.server.events)
        {
            body += (seen.kind == Http2EventKind::Body) ? seen.payload : Text();
        }
        CHECK(body == "hello");
        CHECK(pair.server.count(Http2EventKind::Finished) == 1U);

        REQUIRE(pair.server.conn.send_response(id, Fields{{":status", "103"}}.span(), false) == Http2Error::None);
        REQUIRE(pair.server.conn.send_response(id, ok_response().span(), false) == Http2Error::None);
        REQUIRE(pair.server.conn.send_body(id, view("world"), false, taken) == Http2Error::None);
        REQUIRE(pair.server.conn.finish(id) == Http2Error::None);
        pair.run();
        CHECK(pair.client.last(Http2EventKind::Interim)->status == 103U);
        CHECK(pair.client.last(Http2EventKind::Headers)->status == 200U);
        CHECK(pair.client.count(Http2EventKind::Finished) == 1U);
        CHECK(pair.client.conn.open_streams() == 0U);
        CHECK(pair.server.conn.open_streams() == 0U);
    }
}

TEST_CASE("http2: a large field block travels in CONTINUATION frames", "[http2]")
{
    Http2Config config = test_config(Http2Side::Client);
    config.field_bytes = 1U << 20U;
    Peer peer(config);
    Text value;
    value.resize(70000U);
    for (usize at = 0U; at < value.size(); ++at)
    {
        value.data()[at] = static_cast<char>('a' + (at % 26U));
    }
    const Fields request{{":method", "GET"},
                         {":scheme", "https"},
                         {":authority", "a.example"},
                         {":path", "/"},
                         {"x-long", value.c_str()}};
    peer.side.conn.output_taken(0U);
    u32 id = 0U;
    CHECK(peer.side.conn.open_request(request.span(), true, id) == Http2Error::TooLarge);

    config.output_bytes = 1U << 20U;
    Peer roomy(config);
    REQUIRE(roomy.side.conn.open_request(request.span(), true, id) == Http2Error::None);
    roomy.side.drain();
    REQUIRE(roomy.side.frames.size() >= 3U);
    CHECK(roomy.side.frames[0].type == kHeaders);
    CHECK((roomy.side.frames[0].flags & kEndStream) != 0U);
    CHECK((roomy.side.frames[0].flags & kEndHeaders) == 0U);
    CHECK(roomy.side.frames[0].payload.size() == kHttp2MinFrameBytes);
    Text block = roomy.side.frames[0].payload;
    for (usize at = 1U; at < roomy.side.frames.size(); ++at)
    {
        CHECK(roomy.side.frames[at].type == kContinuation);
        CHECK(((roomy.side.frames[at].flags & kEndHeaders) != 0U) == (at == roomy.side.frames.size() - 1U));
        block += roomy.side.frames[at].payload;
    }
    CountingAllocator allocator;
    HpackDecoder decoder;
    REQUIRE(decoder.init(allocator, 4096U));
    cont::Array<HttpField> fields;
    fields.resize(16U);
    cont::Array<u8> text;
    text.resize(1U << 20U);
    usize count = 0U;
    REQUIRE(decoder.decode(view(block), Span<HttpField>(fields.data(), fields.size()),
                           Span<u8>(text.data(), text.size()), count) == HpackStatus::Complete);
    CHECK(count == 5U);
    CHECK(text_of(fields[4].value) == value);
}

TEST_CASE("http2: the preface and the first frame are held to section 3.4", "[http2]")
{
    {
        Endpoint server(test_config(Http2Side::Server));
        server.read("PRI * HTTP/2.0\r\n");
        CHECK(server.conn.error() == 0U);
        CHECK(server.in.empty());
        server.read("\r\nSM\r\n\r\n");
        CHECK(server.conn.error() == 0U);
    }
    {
        Endpoint server(test_config(Http2Side::Server));
        server.read("GET / HTTP/1.1\r\n");
        CHECK(server.conn.error() == kH2ProtocolError);
    }
    const Text not_settings[] = {frame(kPing, 0U, 0U, zeros(8U)), frame(kSettings, kAck, 0U), frame(kHeaders, 4U, 1U)};
    for (const Text& first : not_settings)
    {
        Endpoint server(test_config(Http2Side::Server));
        server.read(kPrefaceText);
        server.read(first);
        CHECK(server.conn.error() == kH2ProtocolError);
    }
    Endpoint client(test_config(Http2Side::Client));
    client.read(frame(kPing, 0U, 0U, zeros(8U)));
    CHECK(client.conn.error() == kH2ProtocolError);
}

TEST_CASE("http2: SETTINGS not acknowledged in time ends the connection", "[http2]")
{
    Http2Config config = test_config(Http2Side::Server);
    config.settings_timeout_ns = 5U * kSecond;
    Peer peer(config, false);
    CHECK(peer.side.conn.next_deadline_ns() == 1U + (5U * kSecond));
    peer.side.now = 5U * kSecond;
    peer.side.pump();
    CHECK(peer.error() == 0U);
    peer.side.now = (5U * kSecond) + 1U;
    peer.side.pump();
    CHECK(peer.error() == kH2SettingsTimeout);
    peer.side.drain();
    CHECK(peer.goaway_code() == kH2SettingsTimeout);
}

struct BadFrame
{
    const char* name;
    u8 type;
    u8 flags;
    u32 stream;
    usize payload;
    u32 code;
};

TEST_CASE("http2: every frame's connection errors", "[http2]")
{
    const BadFrame cases[] = {
        {"DATA on stream 0", kData, 0U, 0U, 4U, kH2ProtocolError},
        {"DATA on an idle stream", kData, 0U, 3U, 4U, kH2ProtocolError},
        {"DATA on an even stream", kData, 0U, 2U, 4U, kH2ProtocolError},
        {"HEADERS on stream 0", kHeaders, kEndHeaders, 0U, 0U, kH2ProtocolError},
        {"PRIORITY on stream 0", kPriority, 0U, 0U, 5U, kH2ProtocolError},
        {"PRIORITY of the wrong size on an idle stream", kPriority, 0U, 3U, 4U, kH2FrameSizeError},
        {"RST_STREAM on stream 0", kRst, 0U, 0U, 4U, kH2ProtocolError},
        {"RST_STREAM on an idle stream", kRst, 0U, 3U, 4U, kH2ProtocolError},
        {"RST_STREAM of the wrong size", kRst, 0U, 1U, 5U, kH2FrameSizeError},
        {"SETTINGS on a stream", kSettings, 0U, 1U, 0U, kH2ProtocolError},
        {"SETTINGS with a partial entry", kSettings, 0U, 0U, 5U, kH2FrameSizeError},
        {"SETTINGS ACK with a payload", kSettings, kAck, 0U, 6U, kH2FrameSizeError},
        {"SETTINGS ACK unasked", kSettings, kAck, 0U, 0U, kH2ProtocolError},
        {"PUSH_PROMISE", kPush, kEndHeaders, 1U, 4U, kH2ProtocolError},
        {"PING on a stream", kPing, 0U, 1U, 8U, kH2ProtocolError},
        {"PING of the wrong size", kPing, 0U, 0U, 7U, kH2FrameSizeError},
        {"GOAWAY on a stream", kGoAway, 0U, 1U, 8U, kH2ProtocolError},
        {"GOAWAY too short", kGoAway, 0U, 0U, 7U, kH2FrameSizeError},
        {"WINDOW_UPDATE of the wrong size", kWindow, 0U, 0U, 3U, kH2FrameSizeError},
        {"WINDOW_UPDATE of zero on the connection", kWindow, 0U, 0U, 4U, kH2ProtocolError},
        {"WINDOW_UPDATE on an idle stream", kWindow, 0U, 3U, 4U, kH2ProtocolError},
        {"CONTINUATION with no block", kContinuation, kEndHeaders, 1U, 0U, kH2ProtocolError},
        {"a frame over the limit", kData, 0U, 1U, kHttp2MinFrameBytes + 1U, kH2FrameSizeError},
    };
    for (const BadFrame& bad : cases)
    {
        CAPTURE(bad.name);
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        Text payload = zeros(bad.payload);
        if (bad.type == kWindow && bad.stream != 0U && bad.payload == 4U)
        {
            payload = u32_text(1U);
        }
        peer.send(bad.type, bad.flags, bad.stream, payload);
        CHECK(peer.error() == bad.code);
        CHECK(peer.goaway_code() == bad.code);
        CHECK(u32_of(peer.side.last_frame(kGoAway)->payload) == 1U);
    }
}

TEST_CASE("http2: padding is held to its bounds and counts toward flow control", "[http2]")
{
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.send(kData, kPadded, 1U);
        CHECK(peer.error() == kH2FrameSizeError);
    }
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        Text payload = zeros(1U);
        payload.data()[0] = 1;
        peer.send(kData, kPadded, 1U, payload); // 1 byte of padding in a 1-byte payload
        CHECK(peer.error() == kH2ProtocolError);
    }
    {
        Peer peer = server_peer();
        Text payload = zeros(1U);
        payload.data()[0] = 1;
        peer.send(kHeaders, static_cast<u8>(kPadded | kEndHeaders), 1U, payload);
        CHECK(peer.error() == kH2ProtocolError);
    }
    {
        Peer peer = server_peer();
        peer.send(kHeaders, static_cast<u8>(kPadded | kEndHeaders), 1U);
        CHECK(peer.error() == kH2FrameSizeError);
    }
    {
        Peer peer = server_peer();
        peer.send(kHeaders, static_cast<u8>(kPrioritized | kEndHeaders), 1U, zeros(4U));
        CHECK(peer.error() == kH2FrameSizeError);
    }
    // Content with padding delivers only the content, and all of the frame is charged and given back.
    Http2Config config = test_config(Http2Side::Server);
    config.stream_window = 100U;
    Peer peer(config);
    peer.side.auto_consume = false;
    peer.headers(1U, post_request(), 0U);
    Text payload = zeros(1U);
    payload.data()[0] = 39; // padding
    payload += Text("hello");
    payload += zeros(39U);
    peer.send(kData, kPadded, 1U, payload);
    REQUIRE(peer.side.last(Http2EventKind::Body) != nullptr);
    CHECK(peer.side.last(Http2EventKind::Body)->payload == "hello");
    REQUIRE(peer.side.frame_count(kWindow) == 0U); // 40 is under half of the window
    peer.send(kData, kPadded, 1U, payload);
    CHECK(peer.error() == 0U);
    const WireFrame* const update = peer.side.last_frame(kWindow);
    REQUIRE(update != nullptr);
    CHECK(update->stream == 1U);
    CHECK(u32_of(update->payload) == 80U);
    REQUIRE(peer.side.conn.consume_body(1U, 10U) == Http2Error::None);
    CHECK(peer.side.conn.consume_body(1U, 1U) == Http2Error::Invalid);
}

TEST_CASE("http2: a padded and prioritized HEADERS frame is read past both", "[http2]")
{
    Peer peer = server_peer();
    const Text block = peer.block(get_request());
    Text payload = zeros(1U);
    payload.data()[0] = 3;
    payload += u32_text(5U);
    payload += zeros(1U);
    payload += block;
    payload += zeros(3U);
    peer.send(kHeaders, static_cast<u8>(kPadded | kPrioritized | kEndHeaders | kEndStream), 1U, payload);
    CHECK(peer.error() == 0U);
    CHECK(peer.side.count(Http2EventKind::Headers) == 1U);
    CHECK(peer.side.count(Http2EventKind::Finished) == 1U);
    CHECK(peer.side.in.empty());
    // Padding may take everything after the pad length.
    Text padded = zeros(1U);
    padded.data()[0] = 4;
    padded += zeros(4U);
    peer.send(kHeaders, kPadded, 3U, padded);
    CHECK(peer.error() == 0U);
    CHECK(peer.side.in.empty());
}

TEST_CASE("http2: unknown frames and flags are ignored", "[http2]")
{
    Peer peer = server_peer();
    peer.send(0xFAU, 0xFFU, 0U, zeros(32U));
    peer.send(0xFAU, 0U, 5U, zeros(16384U));
    CHECK(peer.error() == 0U);
    // Undefined flags and the reserved bit change nothing.
    peer.send(kPing, 0xFEU, 0U, zeros(8U));
    CHECK(peer.side.last_frame(kPing) != nullptr);
    const Text block = peer.block(get_request());
    peer.send(kHeaders, 0x40U | kEndHeaders | kEndStream, 0x80000001U, block);
    CHECK(peer.side.last(Http2EventKind::Headers)->stream == 1U);
}

TEST_CASE("http2: CONTINUATION is contiguous and bounded", "[http2]")
{
    {
        Peer peer = server_peer();
        const Text block = peer.block(get_request());
        const Text head = slice(block, 0U, 3U);
        const Text tail = slice(block, 3U);
        peer.send(kHeaders, kEndStream, 1U, head);
        CHECK(peer.side.count(Http2EventKind::Headers) == 0U);
        peer.send(kContinuation, 0U, 1U);
        peer.send(kContinuation, kEndHeaders, 1U, tail);
        CHECK(peer.error() == 0U);
        CHECK(peer.side.count(Http2EventKind::Headers) == 1U);
        CHECK(peer.side.count(Http2EventKind::Finished) == 1U);
    }
    const Text interrupting[] = {frame(kPing, 0U, 0U, zeros(8U)), frame(kPriority, 0U, 1U, zeros(5U)),
                                 frame(0xFAU, 0U, 0U), frame(kHeaders, kEndHeaders, 3U),
                                 frame(kContinuation, kEndHeaders, 3U), frame(kContinuation, kEndHeaders, 0U)};
    for (const Text& frame_between : interrupting)
    {
        Peer peer = server_peer();
        const Text block = peer.block(get_request());
        peer.send(kHeaders, 0U, 1U, block);
        CHECK(peer.error() == 0U);
        peer.side.read(frame_between);
        CHECK(peer.error() == kH2ProtocolError);
    }
    {
        Peer peer = server_peer(); // the bound is twice field_bytes, 8192
        peer.send(kHeaders, 0U, 1U, zeros(4096U));
        peer.send(kContinuation, 0U, 1U, zeros(4096U));
        CHECK(peer.error() == 0U);
        peer.send(kContinuation, 0U, 1U, zeros(1U));
        CHECK(peer.error() == kH2EnhanceYourCalm);
    }
    {
        Peer peer = server_peer();
        peer.send(kHeaders, 0U, 1U, zeros(8193U));
        CHECK(peer.error() == kH2EnhanceYourCalm);
    }
}

TEST_CASE("http2: a field block HPACK refuses ends the connection", "[http2]")
{
    Peer peer = server_peer();
    peer.send(kHeaders, static_cast<u8>(kEndHeaders | kEndStream), 1U, hex("80"));
    CHECK(peer.error() == kH2CompressionError);
    CHECK(peer.goaway_code() == kH2CompressionError);
}

TEST_CASE("http2: stream identifiers", "[http2]")
{
    SECTION("an even one opens nothing")
    {
        Peer peer = server_peer();
        peer.get(2U);
        CHECK(peer.error() == kH2ProtocolError);
    }
    SECTION("one below those seen, never opened")
    {
        Peer peer = server_peer();
        peer.get(5U);
        CHECK(peer.error() == 0U);
        peer.get(3U);
        CHECK(peer.error() == kH2ProtocolError);
        Peer lowest = server_peer();
        lowest.get(3U);
        lowest.get(1U);
        CHECK(lowest.error() == kH2ProtocolError);
    }
    SECTION("one the peer closed by a reset is STREAM_CLOSED and the table stays in step")
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.send(kRst, 0U, 1U, u32_text(kH2Cancel));
        peer.send(kData, 0U, 1U, zeros(4U));
        CHECK(peer.error() == 0U);
        CHECK(peer.reset_code(1U) == kH2StreamClosed);
        peer.headers(1U, Fields{{"x-after", "1"}});
        CHECK(peer.side.frame_count(kRst) == 2U);
        peer.get(3U);
        CHECK(peer.side.last(Http2EventKind::Headers)->stream == 3U);
        CHECK(peer.error() == 0U);
    }
    SECTION("frames crossing this side's reset are dropped, the table and the window kept")
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        REQUIRE(peer.side.conn.cancel(1U, kH2Cancel) == Http2Error::None);
        peer.side.drain();
        CHECK(peer.reset_code(1U) == kH2Cancel);
        const usize resets = peer.side.frame_count(kRst);
        peer.send(kData, 0U, 1U, zeros(16384U));
        peer.send(kData, 0U, 1U, zeros(16384U));
        peer.headers(1U, Fields{{"x-after", "1"}});
        peer.send(kRst, 0U, 1U, u32_text(kH2Cancel));
        peer.send(kWindow, 0U, 1U, u32_text(4096U));
        CHECK(peer.error() == 0U);
        CHECK(peer.side.frame_count(kRst) == resets);
        CHECK(peer.side.frame_count(kWindow) == 1U); // the connection's credit came back
        peer.get(3U);
        CHECK(holds(peer.side.last(Http2EventKind::Headers)->fields, ":method=GET"));
    }
    SECTION("a stream that ended well is closed")
    {
        Peer peer = server_peer();
        peer.get(1U);
        REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), true) == Http2Error::None);
        peer.side.drain();
        CHECK(peer.side.conn.open_streams() == 0U);
        peer.send(kWindow, 0U, 1U, u32_text(4096U));
        peer.send(kRst, 0U, 1U, u32_text(kH2Cancel));
        peer.send(kPriority, 0U, 1U, zeros(5U));
        CHECK(peer.side.frame_count(kRst) == 0U);
        peer.send(kData, 0U, 1U, zeros(4U));
        CHECK(peer.reset_code(1U) == kH2StreamClosed);
        CHECK(peer.error() == 0U);
    }
    SECTION("past what is remembered it is taken for closed")
    {
        Http2Config config = test_config(Http2Side::Server);
        config.streams = 1U;
        Peer peer(config);
        for (u32 id = 1U; id <= 9U; id += 2U)
        {
            peer.get(id);
            REQUIRE(peer.side.conn.send_response(id, ok_response().span(), true) == Http2Error::None);
            peer.side.drain();
        }
        peer.send(kData, 0U, 1U, zeros(4U));
        CHECK(peer.error() == 0U);
        CHECK(peer.reset_code(1U) == kH2StreamClosed);
    }
}

TEST_CASE("http2: the half-closed states", "[http2]")
{
    {
        Peer peer = server_peer();
        peer.get(1U);
        peer.send(kData, 0U, 1U, zeros(4U));
        CHECK(peer.reset_code(1U) == kH2StreamClosed);
        CHECK(peer.side.last(Http2EventKind::Aborted)->error == kH2StreamClosed);
    }
    {
        Peer peer = server_peer();
        peer.get(1U);
        peer.headers(1U, Fields{{"x-t", "1"}});
        CHECK(peer.reset_code(1U) == kH2StreamClosed);
    }
    {
        Peer peer = server_peer();
        peer.get(1U);
        peer.send(kPriority, 0U, 1U, zeros(5U));
        peer.send(kWindow, 0U, 1U, u32_text(4096U));
        CHECK(peer.error() == 0U);
        CHECK(peer.side.frame_count(kRst) == 0U);
        peer.send(kRst, 0U, 1U, u32_text(kH2Cancel));
        CHECK(peer.side.last(Http2EventKind::Aborted)->error == kH2Cancel);
        CHECK(peer.side.conn.open_streams() == 0U);
    }
}

TEST_CASE("http2: the stream limit refuses and keeps the table in step", "[http2]")
{
    Http2Config config = test_config(Http2Side::Server);
    config.streams = 2U;
    Peer peer(config);
    peer.get(1U);
    peer.get(3U);
    peer.headers(5U, Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a.example"}, {":path", "/x"}});
    CHECK(peer.reset_code(5U) == kH2RefusedStream);
    CHECK(peer.side.count(Http2EventKind::Headers) == 2U);
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), true) == Http2Error::None);
    peer.side.drain();
    peer.headers(7U, Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a.example"}, {":path", "/x"}});
    CHECK(holds(peer.side.last(Http2EventKind::Headers)->fields, ":path=/x"));
    // Frames crossing the refusal are dropped.
    const usize resets = peer.side.frame_count(kRst);
    peer.send(kData, 0U, 5U, zeros(4U));
    CHECK(peer.side.frame_count(kRst) == resets);
    CHECK(peer.error() == 0U);
}

TEST_CASE("http2: PRIORITY is read and ignored", "[http2]")
{
    Peer peer = server_peer();
    peer.send(kPriority, 0U, 3U, u32_text(1U) + zeros(1U));
    CHECK(peer.error() == 0U);
    peer.send(kPriority, 0U, 3U, u32_text(3U) + zeros(1U));
    CHECK(peer.error() == kH2ProtocolError);

    Peer open = server_peer();
    open.headers(1U, post_request(), 0U);
    open.send(kPriority, 0U, 1U, u32_text(1U) + zeros(1U));
    CHECK(open.reset_code(1U) == kH2ProtocolError);
    CHECK(open.error() == 0U);

    Peer sized = server_peer();
    sized.headers(1U, post_request(), 0U);
    sized.send(kPriority, 0U, 1U, zeros(4U));
    CHECK(sized.reset_code(1U) == kH2FrameSizeError);
    CHECK(sized.error() == 0U);

    Peer self = server_peer();
    const Text block = self.block(get_request());
    self.send(kHeaders, static_cast<u8>(kPrioritized | kEndHeaders | kEndStream), 1U, u32_text(1U) + zeros(1U) + block);
    CHECK(self.reset_code(1U) == kH2ProtocolError);
    CHECK(self.side.count(Http2EventKind::Headers) == 0U);
    self.send(kData, 0U, 1U, zeros(1U));
    CHECK(self.side.frame_count(kRst) == 1U);
    CHECK(self.error() == 0U);

    Peer trailing = server_peer();
    trailing.headers(1U, post_request(), 0U);
    const Text trailers = trailing.block(Fields{{"x-t", "1"}});
    trailing.send(kHeaders, static_cast<u8>(kPrioritized | kEndHeaders | kEndStream), 1U,
                  u32_text(1U) + zeros(1U) + trailers);
    CHECK(trailing.reset_code(1U) == kH2ProtocolError);
    CHECK(trailing.side.count(Http2EventKind::Trailers) == 0U);
}

TEST_CASE("http2: SETTINGS values", "[http2]")
{
    struct BadSetting
    {
        u16 id;
        u32 value;
        u32 code;
        Http2Side side;
    };
    const BadSetting cases[] = {
        {2U, 2U, kH2ProtocolError, Http2Side::Server},
        {2U, 1U, kH2ProtocolError, Http2Side::Client},
        {4U, 0x80000000U, kH2FlowControlError, Http2Side::Server},
        {5U, 16383U, kH2ProtocolError, Http2Side::Server},
        {5U, 16777216U, kH2ProtocolError, Http2Side::Server},
        {8U, 2U, kH2ProtocolError, Http2Side::Server},
        {9U, 2U, kH2ProtocolError, Http2Side::Server},
    };
    for (const BadSetting& bad : cases)
    {
        Peer peer(test_config(bad.side));
        peer.send(kSettings, 0U, 0U, setting(bad.id, bad.value));
        CHECK(peer.error() == bad.code);
    }
    Peer peer = server_peer();
    peer.send(kSettings, 0U, 0U, setting(2U, 1U) + setting(0x7777U, 9U) + setting(9U, 1U) + setting(6U, 2048U));
    CHECK(peer.error() == 0U);
    CHECK(peer.side.conn.peer_settings().max_field_section_size == 2048U);
    REQUIRE(peer.side.last_frame(kSettings) != nullptr);
    CHECK(peer.side.last_frame(kSettings)->flags == kAck);
    CHECK(peer.side.last(Http2EventKind::Settings) != nullptr);

    Peer connect(test_config(Http2Side::Client));
    connect.send(kSettings, 0U, 0U, setting(8U, 1U));
    CHECK(connect.side.conn.peer_settings().enable_connect_protocol);
    connect.send(kSettings, 0U, 0U, setting(8U, 0U));
    CHECK(connect.error() == kH2ProtocolError);
}

TEST_CASE("http2: the HPACK table sizes follow SETTINGS and its acknowledgement", "[http2]")
{
    Http2Config config = test_config(Http2Side::Server);
    config.table_bytes = 256U;
    Peer peer(config, false);
    // Before the ACK the peer may use 4096 and owes no update.
    peer.get(1U);
    CHECK(peer.error() == 0U);
    CHECK(peer.side.conn.hpack_decoder().limit() == 4096U);
    peer.send(kSettings, kAck, 0U);
    CHECK(peer.side.conn.hpack_decoder().limit() == 256U);
    // The encoder's next block says the peer's new size.
    peer.send(kSettings, 0U, 0U, setting(1U, 128U));
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), true) == Http2Error::None);
    peer.side.drain();
    const WireFrame* const response = peer.side.last_frame(kHeaders);
    REQUIRE(response != nullptr);
    CHECK((static_cast<u8>(response->payload.data()[0]) & 0xE0U) == 0x20U);
    CHECK(peer.side.conn.hpack_encoder().table().capacity() == 128U);
}

TEST_CASE("http2: PING is answered with its payload", "[http2]")
{
    Peer peer = server_peer();
    peer.send(kPing, 0U, 0U, Text("abcdefgh"));
    REQUIRE(peer.side.frame_count(kPing) == 1U);
    CHECK(peer.side.frames[0].flags == kAck);
    CHECK(peer.side.frames[0].payload == "abcdefgh");
    peer.send(kPing, kAck, 0U, Text("abcdefgh"));
    CHECK(peer.side.frame_count(kPing) == 1U);
}

TEST_CASE("http2: GOAWAY both ways", "[http2]")
{
    SECTION("a client cuts what the server will not handle")
    {
        Peer peer(test_config(Http2Side::Client));
        u32 ids[3] = {};
        for (u32& id : ids)
        {
            REQUIRE(peer.side.conn.open_request(get_request().span(), true, id) == Http2Error::None);
        }
        peer.side.drain();
        peer.send(kGoAway, 0U, 0U, u32_text(1U) + u32_text(kH2NoError));
        REQUIRE(peer.side.last(Http2EventKind::GoAway) != nullptr);
        CHECK(peer.side.last(Http2EventKind::GoAway)->stream == 1U);
        CHECK(peer.side.count(Http2EventKind::Aborted) == 2U);
        CHECK(peer.side.last(Http2EventKind::Aborted)->error == kH2RefusedStream);
        CHECK(peer.side.conn.open_streams() == 1U);
        u32 id = 0U;
        CHECK(peer.side.conn.open_request(get_request().span(), true, id) == Http2Error::GoingAway);
        peer.send(kGoAway, 0U, 0U, u32_text(1U) + u32_text(kH2NoError));
        CHECK(peer.error() == 0U);
        peer.send(kGoAway, 0U, 0U, u32_text(3U) + u32_text(kH2NoError));
        CHECK(peer.error() == kH2ProtocolError);
    }
    SECTION("a server's gentle one, then the exact one")
    {
        Peer peer = server_peer();
        peer.get(1U);
        REQUIRE(peer.side.conn.goaway(false));
        peer.side.drain();
        CHECK(u32_of(peer.side.last_frame(kGoAway)->payload) == kHttp2MaxStreamId);
        peer.get(3U);
        CHECK(peer.side.count(Http2EventKind::Headers) == 2U);
        REQUIRE(peer.side.conn.goaway(true));
        peer.side.drain();
        CHECK(u32_of(peer.side.last_frame(kGoAway)->payload) == 3U);
        REQUIRE(peer.side.conn.goaway(true));
        peer.side.drain();
        CHECK(peer.side.frame_count(kGoAway) == 2U);
        // Above the last one: decoded and dropped.
        peer.get(5U);
        CHECK(peer.side.count(Http2EventKind::Headers) == 2U);
        CHECK(peer.reset_code(5U) == ~u32{0});
        CHECK(peer.error() == 0U);
    }
    SECTION("a server reads a client's")
    {
        Peer peer = server_peer();
        peer.send(kGoAway, 0U, 0U, u32_text(0U) + u32_text(kH2NoError) + Text("debug"));
        CHECK(peer.side.conn.goaway_received());
        CHECK(peer.side.last(Http2EventKind::GoAway)->error == kH2NoError);
    }
}

TEST_CASE("http2: WINDOW_UPDATE errors of a stream", "[http2]")
{
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.send(kWindow, 0U, 1U, u32_text(0U));
        CHECK(peer.reset_code(1U) == kH2ProtocolError);
        CHECK(peer.error() == 0U);
    }
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.send(kWindow, 0U, 1U, u32_text(kHttp2MaxWindow));
        CHECK(peer.reset_code(1U) == kH2FlowControlError);
        CHECK(peer.side.last(Http2EventKind::Aborted) != nullptr);
    }
    {
        Peer peer = server_peer();
        peer.send(kWindow, 0U, 0U, u32_text(kHttp2MaxWindow));
        CHECK(peer.error() == kH2FlowControlError);
    }
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.send(kWindow, 0U, 1U, u32_text(0x80000000U | 4096U)); // the reserved bit is not part of it
        CHECK(peer.error() == 0U);
        CHECK(peer.side.frame_count(kRst) == 0U);
    }
}

TEST_CASE("http2: a window reaches the maximum and no further", "[http2]")
{
    {
        Peer peer = server_peer();
        peer.send(kWindow, 0U, 0U, u32_text(kHttp2MaxWindow - kHttp2InitialWindow));
        CHECK(peer.error() == 0U);
        peer.send(kWindow, 0U, 0U, u32_text(1U));
        CHECK(peer.error() == kH2FlowControlError);
    }
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.send(kWindow, 0U, 1U, u32_text(kHttp2MaxWindow - kHttp2InitialWindow));
        CHECK(peer.side.frame_count(kRst) == 0U);
        peer.send(kWindow, 0U, 1U, u32_text(1U));
        CHECK(peer.reset_code(1U) == kH2FlowControlError);
    }
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.send(kWindow, 0U, 1U, u32_text(kHttp2MaxWindow - kHttp2InitialWindow));
        peer.send(kSettings, 0U, 0U, setting(4U, kHttp2InitialWindow + 1U));
        CHECK(peer.error() == kH2FlowControlError);
        Peer exact = server_peer();
        exact.headers(1U, post_request(), 0U);
        exact.send(kWindow, 0U, 1U, u32_text(kHttp2MaxWindow - kHttp2InitialWindow - 1U));
        exact.send(kSettings, 0U, 0U, setting(4U, kHttp2InitialWindow + 1U));
        CHECK(exact.error() == 0U);
    }
}

TEST_CASE("http2: a frame is as large as both sides allow", "[http2]")
{
    Http2Config config = test_config(Http2Side::Server);
    config.frame_bytes = 20000U;
    config.stream_window = 1U << 20U;
    Peer peer(config);
    peer.send(kWindow, 0U, 0U, u32_text(1U << 20U));
    peer.get(1U);
    peer.send(kWindow, 0U, 1U, u32_text(1U << 20U));
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    const Text body = zeros(40000U);
    usize taken = 0U;
    REQUIRE(peer.side.conn.send_body(1U, view(body), false, taken) == Http2Error::None);
    CHECK(taken == kHttp2MinFrameBytes);
    peer.send(kSettings, 0U, 0U, setting(5U, 18000U));
    REQUIRE(peer.side.conn.send_body(1U, view(body), false, taken) == Http2Error::None);
    CHECK(taken == 18000U);
    peer.send(kSettings, 0U, 0U, setting(5U, 1U << 20U));
    REQUIRE(peer.side.conn.send_body(1U, view(body), false, taken) == Http2Error::None);
    CHECK(taken == 20000U);
}

TEST_CASE("http2: a rise of the peer's initial window wakes writers", "[http2]")
{
    Peer peer = server_peer();
    peer.get(1U);
    peer.send(kSettings, 0U, 0U, setting(4U, 0U));
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    usize taken = 0U;
    CHECK(peer.side.conn.send_body(1U, view("x"), false, taken) == Http2Error::Blocked);
    peer.send(kSettings, 0U, 0U, setting(4U, 10U));
    CHECK(peer.side.last(Http2EventKind::Writable) != nullptr);
}

TEST_CASE("http2: Writable reaches every blocked stream, a closing one aside", "[http2]")
{
    Peer peer = server_peer();
    for (u32 id = 1U; id <= 5U; id += 2U)
    {
        peer.get(id);
    }
    peer.send(kSettings, 0U, 0U, setting(4U, 0U));
    usize taken = 0U;
    for (u32 id = 1U; id <= 5U; id += 2U)
    {
        REQUIRE(peer.side.conn.send_response(id, ok_response().span(), false) == Http2Error::None);
        CHECK(peer.side.conn.send_body(id, view("x"), false, taken) == Http2Error::Blocked);
    }
    peer.side.events.clear();
    peer.side.in += frame(kSettings, 0U, 0U, setting(4U, 1U));
    Http2Event event;
    const auto next = [&peer, &event]
    {
        usize used = 0U;
        const bool more = peer.side.conn.poll(view(peer.side.in), peer.side.now, used, event);
        erase_front(peer.side.in, used);
        return more;
    };
    REQUIRE(next());
    CHECK(event.kind == Http2EventKind::Settings);
    REQUIRE(next());
    CHECK(event.stream == 1U);
    REQUIRE(next());
    CHECK(event.stream == 3U);
    REQUIRE(peer.side.conn.cancel(1U, kH2Cancel) == Http2Error::None); // the walk's positions move under it
    REQUIRE(next());
    CHECK(event.stream == 5U);
    CHECK_FALSE(next());
}

TEST_CASE("http2: sending follows both windows and the frame limit", "[http2]")
{
    Peer peer = server_peer();
    peer.get(1U);
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    peer.side.drain();
    Text body = zeros(100000U);
    usize sent = 0U;
    for (;;)
    {
        usize taken = 0U;
        const Http2Error error = peer.side.conn.send_body(1U, view(body).subspan(sent), true, taken);
        peer.side.drain();
        sent += taken;
        if (error == Http2Error::Blocked)
        {
            break;
        }
        REQUIRE(error == Http2Error::None);
        CHECK(taken <= kHttp2MinFrameBytes);
    }
    CHECK(sent == kHttp2InitialWindow);
    CHECK((peer.side.last_frame(kData)->flags & kEndStream) == 0U);
    // Stream credit alone is not enough: the connection's is spent too.
    peer.send(kWindow, 0U, 1U, u32_text(4096U));
    CHECK(peer.side.count(Http2EventKind::Writable) == 1U);
    usize taken = 0U;
    CHECK(peer.side.conn.send_body(1U, view(body).subspan(sent), true, taken) == Http2Error::Blocked);
    peer.send(kWindow, 0U, 0U, u32_text(2048U));
    CHECK(peer.side.count(Http2EventKind::Writable) == 2U);
    REQUIRE(peer.side.conn.send_body(1U, view(body).subspan(sent), true, taken) == Http2Error::None);
    CHECK(taken == 2048U);
}

TEST_CASE("http2: SETTINGS_INITIAL_WINDOW_SIZE moves every open stream's window", "[http2]")
{
    Peer peer = server_peer();
    peer.get(1U);
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    peer.side.drain();
    usize taken = 0U;
    REQUIRE(peer.side.conn.send_body(1U, view(zeros(1000U)), false, taken) == Http2Error::None);
    REQUIRE(taken == 1000U);
    peer.send(kSettings, 0U, 0U, setting(4U, 500U)); // the window is now 500 - 1000
    CHECK(peer.error() == 0U);
    CHECK(peer.side.conn.send_body(1U, view(zeros(1U)), false, taken) == Http2Error::Blocked);
    peer.send(kWindow, 0U, 1U, u32_text(2000U)); // and then 1500, the update allowed while negative
    CHECK(peer.side.count(Http2EventKind::Writable) == 1U);
    REQUIRE(peer.side.conn.send_body(1U, view(zeros(4000U)), false, taken) == Http2Error::None);
    CHECK(taken == 1500U);
    // A rise that takes a window past the maximum.
    peer.send(kWindow, 0U, 1U, u32_text(kHttp2MaxWindow - 1U));
    CHECK(peer.error() == 0U);
    peer.send(kSettings, 0U, 0U, setting(4U, kHttp2MaxWindow));
    CHECK(peer.error() == kH2FlowControlError);

    Peer multiple = server_peer();
    multiple.get(1U);
    multiple.send(kSettings, 0U, 0U, setting(4U, 1U) + setting(4U, 2U) + setting(4U, 3U));
    REQUIRE(multiple.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    REQUIRE(multiple.side.conn.send_body(1U, view(zeros(100U)), false, taken) == Http2Error::None);
    CHECK(taken == 3U);
}

TEST_CASE("http2: the receive windows are enforced and given back", "[http2]")
{
    {
        Http2Config config = test_config(Http2Side::Server);
        config.stream_window = 32U;
        Peer peer(config);
        peer.headers(1U, post_request(), 0U);
        peer.send(kData, 0U, 1U, zeros(33U));
        CHECK(peer.reset_code(1U) == kH2FlowControlError);
        CHECK(peer.error() == 0U);
        CHECK(peer.side.frame_count(kWindow) == 0U); // 33 is far from half the connection's
    }
    {
        Peer peer = server_peer();
        peer.side.auto_consume = false;
        for (u32 id = 1U; id <= 9U; id += 2U)
        {
            peer.headers(id, post_request(), 0U);
            peer.send(kData, 0U, id, zeros(16384U));
        }
        CHECK(peer.error() == kH2FlowControlError);
    }
    {
        // A window below the default binds only from the ACK.
        Http2Config config = test_config(Http2Side::Server);
        config.stream_window = 32U;
        Peer peer(config, false);
        peer.headers(1U, post_request(), 0U);
        peer.send(kData, 0U, 1U, zeros(1000U));
        CHECK(peer.side.frame_count(kRst) == 0U);
        peer.send(kSettings, kAck, 0U);
        peer.send(kData, 0U, 1U, zeros(1U));
        CHECK(peer.reset_code(1U) == kH2FlowControlError);
    }
    {
        // Credit is told in half-window steps.
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.send(kData, 0U, 1U, zeros(16384U));
        CHECK(peer.side.frame_count(kWindow) == 0U);
        peer.send(kData, 0U, 1U, zeros(16384U));
        CHECK(peer.side.frame_count(kWindow) == 2U);
    }
    {
        // A larger frame limit binds only from the ACK.
        Http2Config config = test_config(Http2Side::Server);
        config.frame_bytes = 20000U;
        config.stream_window = 1U << 20U;
        config.connection_window = 1U << 20U;
        Peer peer(config, false);
        peer.headers(1U, post_request(), 0U);
        peer.send(kData, 0U, 1U, zeros(16385U));
        CHECK(peer.error() == kH2FrameSizeError);
        Peer acked(config);
        acked.headers(1U, post_request(), 0U);
        acked.send(kData, 0U, 1U, zeros(20000U));
        CHECK(acked.error() == 0U);
        acked.send(kData, 0U, 1U, zeros(20001U));
        CHECK(acked.error() == kH2FrameSizeError);
    }
}

TEST_CASE("http2: what never reached the application goes back to the connection", "[http2]")
{
    {
        // Padding alone.
        Peer peer = server_peer();
        peer.side.auto_consume = false;
        peer.headers(1U, post_request(), 0U);
        Text payload = zeros(256U);
        payload.data()[0] = static_cast<char>(255);
        for (u32 round = 0U; round < 127U; ++round)
        {
            peer.send(kData, kPadded, 1U, payload);
        }
        CHECK(peer.side.conn.consume_body(1U, 1U) == Http2Error::Invalid);
        CHECK(peer.side.frame_count(kWindow) == 0U);
        peer.send(kData, kPadded, 1U, payload);
        const WireFrame* const update = peer.side.last_frame(kWindow);
        CHECK(peer.side.frame_count(kWindow) == 2U);
        REQUIRE(update != nullptr);
        CHECK(u32_of(update->payload) == 32768U);
    }
    {
        // A frame refused for its stream.
        Peer peer = server_peer();
        peer.get(1U);
        peer.get(3U);
        peer.send(kData, 0U, 1U, zeros(16384U));
        peer.send(kData, 0U, 3U, zeros(16384U));
        REQUIRE(peer.side.last_frame(kWindow) != nullptr);
        CHECK(peer.side.last_frame(kWindow)->stream == 0U);
    }
    {
        // A stream whose peer has finished sending needs no more credit.
        Http2Config config = test_config(Http2Side::Server);
        config.stream_window = 64U;
        Peer peer(config);
        peer.side.auto_consume = false;
        peer.headers(1U, post_request(), 0U);
        peer.send(kData, kEndStream, 1U, zeros(64U));
        REQUIRE(peer.side.conn.consume_body(1U, 64U) == Http2Error::None);
        peer.side.drain();
        CHECK(peer.side.frame_count(kWindow) == 0U);
    }
}

TEST_CASE("http2: a large connection window takes what it offered", "[http2]")
{
    Http2Config config = test_config(Http2Side::Server);
    config.connection_window = 1U << 20U;
    config.stream_window = 1U << 20U;
    Peer peer(config);
    peer.side.auto_consume = false;
    peer.headers(1U, post_request(), 0U);
    for (u32 round = 0U; round < 8U; ++round)
    {
        peer.send(kData, 0U, 1U, zeros(16384U));
    }
    CHECK(peer.error() == 0U);
}

TEST_CASE("http2: a body's credit returns when the stream goes", "[http2]")
{
    Peer peer = server_peer();
    peer.side.auto_consume = false;
    peer.headers(1U, post_request(), 0U);
    peer.send(kData, 0U, 1U, zeros(16384U));
    peer.send(kData, 0U, 1U, zeros(16384U));
    CHECK(peer.side.frame_count(kWindow) == 0U);
    peer.send(kRst, 0U, 1U, u32_text(kH2Cancel));
    REQUIRE(peer.side.frame_count(kWindow) == 1U);
    CHECK(peer.side.last_frame(kWindow)->stream == 0U);
    CHECK(u32_of(peer.side.last_frame(kWindow)->payload) == 32768U);
    CHECK(peer.side.conn.consume_body(1U, 1U) == Http2Error::Unknown);
}

TEST_CASE("http2: a frame cut anywhere keeps its order", "[http2]")
{
    Peer peer = server_peer();
    peer.side.auto_consume = false;
    peer.headers(1U, post_request(), 0U);
    Text payload = zeros(1U);
    payload.data()[0] = 2;
    payload += Text("abcdef");
    payload += zeros(2U);
    const Text whole = frame(kData, static_cast<u8>(kPadded | kEndStream), 1U, payload);
    for (usize at = 0U; at < whole.size(); ++at)
    {
        peer.side.read(slice(whole, at, 1U));
    }
    Text body;
    for (const Seen& seen : peer.side.events)
    {
        body += (seen.kind == Http2EventKind::Body) ? seen.payload : Text();
    }
    CHECK(body == "abcdef");
    CHECK(peer.side.count(Http2EventKind::Finished) == 1U);
    CHECK(peer.side.in.empty());
    // A control frame is taken only whole.
    peer.side.read(slice(frame(kPing, 0U, 0U, zeros(8U)), 0U, 16U));
    CHECK(peer.side.in.size() == 16U);
}

TEST_CASE("http2: messages reach the application only when well formed", "[http2]")
{
    const auto refused = [](const Fields& fields, u8 extra = kEndStream)
    {
        Peer peer = server_peer();
        peer.headers(1U, fields, extra);
        CHECK(peer.error() == 0U);
        CHECK(peer.side.count(Http2EventKind::Headers) == 0U);
        CHECK(peer.reset_code(1U) == kH2ProtocolError);
        const usize resets = peer.side.frame_count(kRst);
        peer.send(kData, 0U, 1U, zeros(1U)); // it crossed the refusal
        CHECK(peer.error() == 0U);
        CHECK(peer.side.frame_count(kRst) == resets);
    };
    refused(Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}});
    refused(Fields{{":method", "GET"}, {":path", "/"}, {":authority", "a"}});
    refused(Fields{{":scheme", "https"}, {":path", "/"}, {":authority", "a"}});
    refused(Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", ""}});
    refused(Fields{{":method", "GET"}, {":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}});
    refused(Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {":x", "1"}});
    refused(Fields{{":status", "200"}, {":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}});
    refused(Fields{{":method", "GET"}, {":scheme", "https"}, {"x-a", "1"}, {":authority", "a"}, {":path", "/"}});
    refused(Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {"X-Up", "1"}});
    refused(Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {"connection", "a"}});
    refused(Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {"te", "gzip"}});
    refused(Fields{{":method", "GET"},
                   {":scheme", "https"},
                   {":authority", "a"},
                   {":path", "/"},
                   {"content-length", "4"}});

    SECTION("DATA past or short of content-length")
    {
        const Fields sized{{":method", "POST"},
                           {":scheme", "https"},
                           {":authority", "a"},
                           {":path", "/"},
                           {"content-length", "4"}};
        Peer peer = server_peer();
        peer.headers(1U, sized, 0U);
        peer.send(kData, 0U, 1U, zeros(5U));
        CHECK(peer.reset_code(1U) == kH2ProtocolError);
        Peer split = server_peer();
        split.headers(1U, sized, 0U);
        split.send(kData, 0U, 1U, zeros(3U));
        split.send(kData, 0U, 1U, zeros(2U));
        CHECK(split.reset_code(1U) == kH2ProtocolError);
        Peer shorter = server_peer();
        shorter.headers(1U, sized, 0U);
        shorter.send(kData, kEndStream, 1U, zeros(3U));
        CHECK(shorter.reset_code(1U) == kH2ProtocolError);
        CHECK(shorter.side.count(Http2EventKind::Finished) == 0U);
        Peer exact = server_peer();
        exact.headers(1U, sized, 0U);
        exact.send(kData, kEndStream, 1U, zeros(4U));
        CHECK(exact.side.count(Http2EventKind::Finished) == 1U);
        Peer trailed = server_peer();
        trailed.headers(1U, sized, 0U);
        trailed.send(kData, 0U, 1U, zeros(3U));
        trailed.headers(1U, Fields{{"x-t", "1"}});
        CHECK(trailed.reset_code(1U) == kH2ProtocolError);
        CHECK(trailed.side.count(Http2EventKind::Trailers) == 0U);
    }
    SECTION("trailers end the message and hold no pseudo-fields")
    {
        Peer peer = server_peer();
        peer.headers(1U, post_request(), 0U);
        peer.headers(1U, Fields{{"x-t", "1"}}, 0U);
        CHECK(peer.reset_code(1U) == kH2ProtocolError);
        Peer pseudo = server_peer();
        pseudo.headers(1U, post_request(), 0U);
        pseudo.headers(1U, Fields{{":path", "/"}});
        CHECK(pseudo.reset_code(1U) == kH2ProtocolError);
    }
    SECTION("responses")
    {
        const auto opened = [](Peer& peer, const Fields& request)
        {
            u32 id = 0U;
            REQUIRE(peer.side.conn.open_request(request.span(), true, id) == Http2Error::None);
            peer.side.drain();
        };
        const Fields head_request{{":method", "HEAD"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}};
        const Http2Config config = test_config(Http2Side::Client);
        Peer interim_end(config);
        opened(interim_end, get_request());
        interim_end.headers(1U, Fields{{":status", "100"}});
        CHECK(interim_end.reset_code(1U) == kH2ProtocolError);
        CHECK(interim_end.side.last(Http2EventKind::Aborted) != nullptr);

        Peer early_data(config);
        opened(early_data, get_request());
        early_data.send(kData, 0U, 1U, zeros(1U));
        CHECK(early_data.reset_code(1U) == kH2ProtocolError);

        Peer no_status(config);
        opened(no_status, get_request());
        no_status.headers(1U, Fields{{"x-a", "1"}});
        CHECK(no_status.reset_code(1U) == kH2ProtocolError);

        Peer head(config);
        opened(head, head_request);
        head.headers(1U, Fields{{":status", "200"}, {"content-length", "10"}});
        CHECK(head.side.count(Http2EventKind::Finished) == 1U);
        CHECK(head.side.frame_count(kRst) == 0U);

        Peer head_body(config);
        opened(head_body, head_request);
        head_body.headers(1U, ok_response(), 0U);
        head_body.send(kData, 0U, 1U, zeros(1U));
        CHECK(head_body.reset_code(1U) == kH2ProtocolError);

        Peer no_content(config);
        opened(no_content, get_request());
        no_content.headers(1U, Fields{{":status", "204"}}, 0U);
        no_content.send(kData, 0U, 1U, zeros(1U));
        CHECK(no_content.reset_code(1U) == kH2ProtocolError);

        Peer short_end(config);
        opened(short_end, get_request());
        short_end.headers(1U, Fields{{":status", "200"}, {"content-length", "4"}});
        CHECK(short_end.reset_code(1U) == kH2ProtocolError);

        Peer pushed(config);
        opened(pushed, get_request());
        pushed.get(3U);
        CHECK(pushed.error() == kH2ProtocolError);
    }
}

TEST_CASE("http2: a section over the limit is answered 431 and keeps the table in step", "[http2]")
{
    Text value;
    value.resize(5000U);
    for (usize at = 0U; at < value.size(); ++at)
    {
        value.data()[at] = 'v';
    }
    const Fields large{{":method", "GET"},
                       {":scheme", "https"},
                       {":authority", "a"},
                       {":path", "/"},
                       {"x-long", value.c_str()}};
    {
        Peer peer = server_peer();
        peer.headers(1U, large);
        CHECK(peer.error() == 0U);
        CHECK(peer.side.count(Http2EventKind::Headers) == 0U);
        REQUIRE(peer.side.last_frame(kHeaders) != nullptr);
        CHECK(peer.side.frame_count(kRst) == 0U);
        peer.get(3U);
        CHECK(peer.side.count(Http2EventKind::Headers) == 1U);
        peer.send(kData, 0U, 1U, zeros(1U));
        CHECK(peer.side.frame_count(kRst) == 0U);
        CHECK(peer.error() == 0U);
    }
    {
        Peer peer = server_peer();
        peer.headers(1U, large, 0U);
        CHECK(peer.reset_code(1U) == kH2NoError);
    }
    {
        // Room for a refusal but not for the answer: REFUSED_STREAM.
        Http2Config config = test_config(Http2Side::Server);
        config.output_bytes = kHttp2MinOutputBytes;
        Peer peer(config);
        peer.headers(1U, post_request(), 0U);
        REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
        usize taken = 0U;
        REQUIRE(peer.side.conn.send_body(1U, view(zeros(900U)), false, taken) == Http2Error::None);
        REQUIRE(taken == 900U);
        peer.side.read(frame(kHeaders, static_cast<u8>(kEndHeaders | kEndStream), 3U, peer.block(large)));
        peer.side.drain();
        CHECK(peer.reset_code(3U) == kH2RefusedStream);
    }
    {
        // A response too large for a client is the stream's end.
        Peer peer(test_config(Http2Side::Client));
        u32 id = 0U;
        REQUIRE(peer.side.conn.open_request(get_request().span(), true, id) == Http2Error::None);
        peer.side.drain();
        peer.headers(1U, Fields{{":status", "200"}, {"x-long", value.c_str()}});
        CHECK(peer.reset_code(1U) == kH2EnhanceYourCalm);
        CHECK(peer.side.last(Http2EventKind::Aborted)->error == kH2EnhanceYourCalm);
    }
}

TEST_CASE("http2: extended CONNECT", "[http2]")
{
    const Fields extended{{":method", "CONNECT"},
                          {":protocol", "websocket"},
                          {":scheme", "https"},
                          {":authority", "a"},
                          {":path", "/chat"}};
    {
        Peer peer = server_peer();
        peer.headers(1U, extended, 0U);
        CHECK(peer.reset_code(1U) == kH2ProtocolError);
    }
    Http2Config config = test_config(Http2Side::Server);
    config.connect_protocol = true;
    Peer peer(config);
    peer.headers(1U, extended, 0U);
    REQUIRE(peer.side.last(Http2EventKind::Headers) != nullptr);
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    CHECK(peer.side.conn.send_trailers(1U, Fields{{"x-t", "1"}}.span()) == Http2Error::Invalid);
    peer.send(kData, 0U, 1U, Text("tunnel"));
    CHECK(peer.side.last(Http2EventKind::Body)->payload == "tunnel");
    peer.headers(1U, Fields{{"x-t", "1"}});
    CHECK(peer.reset_code(1U) == kH2ProtocolError);

    Peer client(test_config(Http2Side::Client));
    u32 id = 0U;
    CHECK(client.side.conn.open_request(extended.span(), false, id) == Http2Error::NotReady);
    client.send(kSettings, 0U, 0U, setting(8U, 1U));
    REQUIRE(client.side.conn.open_request(extended.span(), false, id) == Http2Error::None);
    client.side.drain();
    client.headers(id, ok_response(), 0U);
    client.send(kData, 0U, id, Text("tunnel"));
    CHECK(client.side.last(Http2EventKind::Body)->payload == "tunnel");
    client.headers(id, Fields{{"x-t", "1"}});
    CHECK(client.reset_code(id) == kH2ProtocolError);
}

TEST_CASE("http2: the calls refuse what is out of sequence", "[http2]")
{
    Peer server = server_peer();
    u32 id = 0U;
    usize taken = 0U;
    CHECK(server.side.conn.open_request(get_request().span(), true, id) == Http2Error::Invalid);
    CHECK(server.side.conn.send_response(1U, ok_response().span(), true) == Http2Error::Unknown);
    server.headers(1U, post_request(), 0U);
    CHECK(server.side.conn.send_body(1U, view("x"), false, taken) == Http2Error::Invalid);
    CHECK(server.side.conn.send_trailers(1U, Fields{{"x", "1"}}.span()) == Http2Error::Invalid);
    CHECK(server.side.conn.send_response(1U, get_request().span(), true) == Http2Error::Invalid);
    CHECK(server.side.conn.send_response(1U, Fields{{":status", "100"}}.span(), true) == Http2Error::Invalid);
    REQUIRE(server.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    CHECK(server.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::Invalid);
    CHECK(server.side.conn.send_body(1U, {}, false, taken) == Http2Error::Invalid);
    REQUIRE(server.side.conn.finish(1U) == Http2Error::None);
    CHECK(server.side.conn.finish(1U) == Http2Error::Unknown);
    CHECK(server.side.conn.send_trailers(1U, Fields{{"x", "1"}}.span()) == Http2Error::Unknown);
    CHECK(server.side.conn.cancel(9U, kH2Cancel) == Http2Error::Unknown);
    CHECK(server.side.conn.consume_body(9U, 0U) == Http2Error::Unknown);
    server.send(kPing, 0U, 0U, zeros(1U));
    CHECK(server.side.conn.send_response(1U, ok_response().span(), true) == Http2Error::Closed);
    CHECK(server.side.conn.send_body(1U, view("x"), false, taken) == Http2Error::Closed);
    CHECK(server.side.conn.send_trailers(1U, Fields{{"x", "1"}}.span()) == Http2Error::Closed);
    CHECK(server.side.conn.cancel(1U, kH2Cancel) == Http2Error::Closed);
    CHECK(server.side.conn.consume_body(1U, 0U) == Http2Error::Closed);
    CHECK_FALSE(server.side.conn.goaway(true));
    CHECK(server.side.conn.open_request(get_request().span(), true, id) == Http2Error::Closed);

    Http2Config config = test_config(Http2Side::Client);
    config.streams = 1U;
    Peer client(config);
    CHECK(client.side.conn.open_request(Fields{{":method", "GET"}}.span(), true, id) == Http2Error::Invalid);
    CHECK(client.side.conn.open_request(Fields{{":method", "POST"},
                                               {":scheme", "https"},
                                               {":authority", "a"},
                                               {":path", "/"},
                                               {"content-length", "1"}}
                                            .span(),
                                        true, id) == Http2Error::Invalid);
    REQUIRE(client.side.conn.open_request(get_request().span(), true, id) == Http2Error::None);
    CHECK(client.side.conn.open_request(get_request().span(), true, id) == Http2Error::NoStream);

    Peer limited(test_config(Http2Side::Client));
    limited.send(kSettings, 0U, 0U, setting(3U, 0U) + setting(6U, 10U));
    CHECK(limited.side.conn.open_request(get_request().span(), true, id) == Http2Error::NoStream);
    limited.send(kSettings, 0U, 0U, setting(3U, 8U));
    CHECK(limited.side.conn.open_request(get_request().span(), true, id) == Http2Error::TooLarge);
}

TEST_CASE("http2: a cancelled stream's frame still counts as it arrives", "[http2]")
{
    Peer peer = server_peer();
    peer.headers(1U, post_request(), 0U);
    const Text whole = frame(kData, 0U, 1U, zeros(16384U));
    peer.side.read(slice(whole, 0U, 100U));
    REQUIRE(peer.side.count(Http2EventKind::Body) == 1U);
    REQUIRE(peer.side.conn.cancel(1U, kH2Cancel) == Http2Error::None);
    peer.side.read(slice(whole, 100U));
    CHECK(peer.side.count(Http2EventKind::Body) == 1U);
    CHECK(peer.error() == 0U);
    CHECK(peer.side.in.empty());
}

Http2Config flood_config()
{
    Http2Config config = test_config(Http2Side::Server);
    config.resets = GateRule{4U, kSecond};
    config.control = GateRule{4U, kSecond};
    return config;
}

// Six goes of `frames` at one moment end the connection, and the same pace a second apart does not.
template <typename Send> void floods(Send send)
{
    {
        Peer peer(flood_config());
        peer.side.now = 10U * kSecond;
        peer.side.pump();
        send(peer, 0U);
        for (u32 round = 1U; round < 5U; ++round)
        {
            send(peer, round);
            CHECK(peer.error() == 0U);
        }
        send(peer, 5U);
        CHECK(peer.error() == kH2EnhanceYourCalm);
        CHECK(peer.goaway_code() == kH2EnhanceYourCalm);
    }
    Peer steady(flood_config());
    steady.side.now = 10U * kSecond;
    for (u32 round = 0U; round < 32U; ++round)
    {
        steady.side.now += kSecond;
        send(steady, round);
        CHECK(steady.error() == 0U);
    }
}

TEST_CASE("http2: the flood limits", "[http2]")
{
    SECTION("PING")
    {
        floods([](Peer& peer, u32) { peer.send(kPing, 0U, 0U, zeros(8U)); });
    }
    SECTION("SETTINGS")
    {
        floods([](Peer& peer, u32) { peer.send(kSettings, 0U, 0U, setting(4U, 65535U)); });
    }
    SECTION("PRIORITY")
    {
        floods([](Peer& peer, u32) { peer.send(kPriority, 0U, 3U, zeros(5U)); });
    }
    SECTION("tiny WINDOW_UPDATEs")
    {
        floods([](Peer& peer, u32) { peer.send(kWindow, 0U, 0U, u32_text(1U)); });
    }
    SECTION("rapid reset")
    {
        floods(
            [](Peer& peer, u32 round)
            {
                const u32 id = (round * 2U) + 1U;
                peer.headers(id, post_request(), 0U);
                peer.send(kRst, 0U, id, u32_text(kH2Cancel));
            });
    }
    SECTION("resets this side is provoked into")
    {
        floods(
            [](Peer& peer, u32 round)
            {
                const u32 id = (round * 2U) + 1U;
                peer.headers(id, post_request(), 0U);
                peer.send(kWindow, 0U, id, u32_text(0U));
            });
    }
    SECTION("frames for a stream the peer already finished")
    {
        floods(
            [](Peer& peer, u32 round)
            {
                const u32 id = (round * 2U) + 1U;
                peer.get(id);
                peer.send(kData, 0U, id, zeros(1U));
            });
    }
    SECTION("sections over the limit")
    {
        Text value = zeros(5000U);
        for (usize at = 0U; at < value.size(); ++at)
        {
            value.data()[at] = 'v';
        }
        const Fields large{{":method", "GET"},
                           {":scheme", "https"},
                           {":authority", "a"},
                           {":path", "/"},
                           {"x-long", value.c_str()}};
        floods([&large](Peer& peer, u32 round) { peer.headers((round * 2U) + 1U, large); });
    }
    SECTION("the smallest WINDOW_UPDATE not charged")
    {
        Peer peer(flood_config());
        peer.side.now = 10U * kSecond;
        for (u32 round = 0U; round < 32U; ++round)
        {
            peer.send(kWindow, 0U, 0U, u32_text(1024U));
        }
        CHECK(peer.error() == 0U);
    }
    SECTION("malformed requests")
    {
        floods([](Peer& peer, u32 round) { peer.headers((round * 2U) + 1U, Fields{{"x-a", "1"}}); });
    }
    SECTION("empty HEADERS fragments")
    {
        floods(
            [](Peer& peer, u32 round)
            {
                const u32 id = (round * 2U) + 1U;
                peer.send(kHeaders, 0U, id);
                if (peer.error() == 0U)
                {
                    const Text block = peer.block(get_request());
                    peer.send(kContinuation, static_cast<u8>(kEndHeaders | kEndStream), id, block);
                    if (peer.error() == 0U)
                    {
                        REQUIRE(peer.side.conn.cancel(id, kH2Cancel) == Http2Error::None);
                    }
                }
            });
    }
    SECTION("empty CONTINUATION frames")
    {
        Peer peer(flood_config());
        peer.side.now = 10U * kSecond;
        peer.send(kHeaders, kEndStream, 1U, peer.block(get_request()));
        for (u32 round = 0U; round < 5U; ++round)
        {
            peer.send(kContinuation, 0U, 1U);
            CHECK(peer.error() == 0U);
        }
        peer.send(kContinuation, 0U, 1U);
        CHECK(peer.error() == kH2EnhanceYourCalm);
    }
    SECTION("empty DATA frames")
    {
        Peer peer(flood_config());
        peer.side.now = 10U * kSecond;
        peer.headers(1U, post_request(), 0U);
        for (u32 round = 0U; round < 5U; ++round)
        {
            peer.send(kData, 0U, 1U);
            CHECK(peer.error() == 0U);
        }
        peer.send(kData, 0U, 1U);
        CHECK(peer.error() == kH2EnhanceYourCalm);
    }
    SECTION("what ends something is not charged")
    {
        Peer peer(flood_config());
        peer.side.now = 10U * kSecond;
        for (u32 round = 0U; round < 32U; ++round)
        {
            const u32 id = (round * 2U) + 1U;
            peer.headers(id, post_request(), 0U);
            peer.send(kData, kEndStream, id);
            REQUIRE(peer.side.conn.send_response(id, ok_response().span(), true) == Http2Error::None);
            peer.side.drain();
        }
        CHECK(peer.error() == 0U);
    }
}

TEST_CASE("http2: a peer that does not read stops being read", "[http2]")
{
    Http2Config config = test_config(Http2Side::Server);
    config.output_bytes = kHttp2MinOutputBytes;
    Peer peer(config);
    Text flood;
    for (u32 count = 0U; count < 200U; ++count)
    {
        flood += frame(kPing, 0U, 0U, zeros(8U));
    }
    peer.side.read(flood);
    CHECK(peer.error() == 0U);
    CHECK(peer.side.in.size() > 0U);
    CHECK(peer.side.conn.output().size() <= kHttp2MinOutputBytes);
    usize answered = 0U;
    for (u32 round = 0U; round < 16U && !peer.side.in.empty(); ++round)
    {
        peer.side.drain();
        answered = peer.side.frame_count(kPing);
        peer.side.pump();
    }
    peer.side.drain();
    CHECK(peer.side.in.empty());
    CHECK(peer.side.frame_count(kPing) == 200U);
    CHECK(answered != 0U);
}

TEST_CASE("http2: an output buffer with no room blocks writers and wakes them", "[http2]")
{
    Http2Config config = test_config(Http2Side::Server);
    config.output_bytes = kHttp2MinOutputBytes;
    Peer peer(config);
    peer.get(1U);
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    const Text body = zeros(4096U);
    usize taken = 0U;
    REQUIRE(peer.side.conn.send_body(1U, view(body), false, taken) == Http2Error::None);
    CHECK(taken < 1024U);
    CHECK(peer.side.conn.send_body(1U, view(body), false, taken) == Http2Error::Blocked);
    CHECK(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::Invalid);
    peer.side.drain();
    peer.side.pump();
    CHECK(peer.side.count(Http2EventKind::Writable) == 1U);
    REQUIRE(peer.side.conn.send_body(1U, view(body), false, taken) == Http2Error::None);
    CHECK(taken != 0U);

    // A client's new request, blocked by the same buffer, gets a Writable naming no stream.
    Http2Config tight = test_config(Http2Side::Client);
    tight.output_bytes = kHttp2MinOutputBytes;
    Peer client(tight);
    u32 id = 0U;
    REQUIRE(client.side.conn.open_request(post_request().span(), false, id) == Http2Error::None);
    REQUIRE(client.side.conn.send_body(id, view(body), false, taken) == Http2Error::None);
    u32 blocked = 0U;
    CHECK(client.side.conn.open_request(get_request().span(), true, blocked) == Http2Error::Blocked);
    client.side.drain();
    client.side.pump();
    REQUIRE(client.side.last(Http2EventKind::Writable) != nullptr);
    CHECK(client.side.last(Http2EventKind::Writable)->stream == 0U);
    CHECK(client.side.conn.open_request(get_request().span(), true, blocked) == Http2Error::None);
}

TEST_CASE("http2: a connection error finds room for GOAWAY however full the output is", "[http2]")
{
    Http2Config config = test_config(Http2Side::Server);
    config.output_bytes = kHttp2MinOutputBytes;
    config.settings_timeout_ns = kSecond;
    Peer peer(config, false);
    peer.get(1U);
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::None);
    const Text body = zeros(4096U);
    usize taken = 0U;
    REQUIRE(peer.side.conn.send_body(1U, view(body), false, taken) == Http2Error::None);
    REQUIRE(peer.side.conn.send_response(1U, ok_response().span(), false) == Http2Error::Invalid);
    peer.side.now = 3U * kSecond;
    peer.side.pump();
    CHECK(peer.error() == kH2SettingsTimeout);
    CHECK(peer.side.conn.output().size() <= kHttp2MinOutputBytes);
    peer.side.drain();
    CHECK(peer.goaway_code() == kH2SettingsTimeout);
}
} // namespace
