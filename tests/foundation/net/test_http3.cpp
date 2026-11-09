// crd-net -- an HTTP/3 connection (4.2a; docs/design/network-foundation.md#http3): SETTINGS and the critical streams,
// every frame on every stream with the error RFC 9114 names, requests and responses with bodies, trailers and interim
// responses, CONNECT and extended CONNECT, HTTP datagrams, GOAWAY both ways, QPACK blocking, sections too large,
// 0-RTT's remembered settings, and what a call out of sequence returns.

#include "http3_fixture.hpp"
#include "ingress_fixture.hpp"

#include <crd/containers/wire_bytes.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/http_capsule.hpp>

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::crypto::QuicSide;

constexpr u64 kData = 0x0U;
constexpr u64 kHeaders = 0x1U;
constexpr u64 kCancelPush = 0x3U;
constexpr u64 kSettings = 0x4U;
constexpr u64 kPushPromise = 0x5U;
constexpr u64 kGoAway = 0x7U;
constexpr u64 kMaxPushId = 0xDU;
constexpr u64 kGreaseFrame = 0x21U;
constexpr u64 kUniControl = 0x0U;
constexpr u64 kUniPush = 0x1U;
constexpr u64 kUniEncoder = 0x2U;
constexpr u64 kUniDecoder = 0x3U;

H3Wire payload(std::initializer_list<u64> values)
{
    H3Wire wire;
    for (const u64 value : values)
    {
        wire.varint(value);
    }
    return wire;
}

H3Wire settings(std::initializer_list<u64> pairs)
{
    H3Wire wire;
    wire.frame(kSettings, payload(pairs));
    return wire;
}

H3Wire frame_of(u64 type, const H3Wire& body)
{
    H3Wire wire;
    wire.frame(type, body);
    return wire;
}

// A static-table section QPACK can decode without any table.
H3Wire request_section()
{
    // A Required Insert Count and Delta Base of 0, then indexed static fields: :method GET (17), :path / (1),
    // :scheme https (23) and :authority (0) with a literal value.
    H3Wire wire;
    wire.byte(0x00U).byte(0x00U).byte(0xC0U | 17U).byte(0xC0U | 1U).byte(0xC0U | 23U);
    wire.byte(0x50U).byte(0x01U).byte('a');
    return wire;
}

H3Wire status_section()
{
    // :status 200 is static index 25.
    H3Wire wire;
    wire.byte(0x00U).byte(0x00U).byte(0xC0U | 25U);
    return wire;
}

H3Wire headers_frame()
{
    return frame_of(kHeaders, request_section());
}

H3Wire response_frame()
{
    return frame_of(kHeaders, status_section());
}

// Trailers: one literal field, x with no value, name not in a table.
H3Wire headers_frame_of_trailers()
{
    H3Wire section;
    section.byte(0x00U).byte(0x00U).byte(0x21U).byte('x').byte(0x00U);
    return frame_of(kHeaders, section);
}

H3Wire data_frame(const char* text)
{
    H3Wire body;
    body.raw(containers::ConstSpan<u8>(reinterpret_cast<const u8*>(text), std::strlen(text)));
    return frame_of(kData, body);
}

containers::ConstSpan<u8> text_span(const char* text)
{
    return containers::ConstSpan<u8>(reinterpret_cast<const u8*>(text), std::strlen(text));
}

struct Fixture
{
    CountingAllocator allocator;
    std::unique_ptr<H3Rig> rig;

    void both()
    {
        rig = std::make_unique<H3Rig>(allocator, true, true);
        REQUIRE(rig->ok);
    }
    // A server under test and a client written by hand.
    void server()
    {
        rig = std::make_unique<H3Rig>(allocator, false, true);
        REQUIRE(rig->ok);
    }
    // A client under test and a server written by hand.
    void client()
    {
        rig = std::make_unique<H3Rig>(allocator, true, false);
        REQUIRE(rig->ok);
    }
    void both(const Http3Config& client_config, const Http3Config& server_config)
    {
        rig = std::make_unique<H3Rig>(allocator, true, true, client_config, server_config);
        REQUIRE(rig->ok);
    }

    // The server's error after a hand-written client sent `wire` on a new control stream of its own.
    u64 server_control_error(const H3Wire& wire)
    {
        server();
        const u64 control = rig->open_typed(kH3Client, kUniControl);
        rig->write(kH3Client, control, wire);
        rig->settle();
        return rig->h3[kH3Server].error();
    }

    // The server's error after one request stream carries `wire`, its FIN following when `fin`.
    u64 server_request_error(const H3Wire& wire, bool fin = false)
    {
        server();
        static_cast<void>(rig->open_control(kH3Client));
        const u64 stream = rig->open(kH3Client, true);
        rig->write(kH3Client, stream, wire, fin);
        rig->settle();
        return rig->h3[kH3Server].error();
    }

    // A client under test sent a request: the hand-written server answers on the stream it returns.
    u64 client_request()
    {
        client();
        static_cast<void>(rig->open_control(kH3Server));
        u64 stream = 0U;
        REQUIRE(rig->h3[kH3Client].open_request(h3_get().span(), true, stream) == Http3Error::None);
        rig->settle();
        return stream;
    }

    u64 client_response_error(const H3Wire& wire, bool fin = false)
    {
        const u64 stream = client_request();
        rig->write(kH3Server, stream, wire, fin);
        rig->settle();
        return rig->h3[kH3Client].error();
    }
};
} // namespace

// ---- Settings and critical streams ----

TEST_CASE("http3: both sides learn each other's settings without waiting", "[http3]")
{
    Fixture f;
    f.both();
    f.rig->settle();
    REQUIRE(f.rig->find(kH3Client, Http3EventKind::Settings) != nullptr);
    REQUIRE(f.rig->find(kH3Server, Http3EventKind::Settings) != nullptr);
    for (usize side = 0U; side < 2U; ++side)
    {
        const Http3Settings& peer = f.rig->h3[side].peer_settings();
        CHECK(f.rig->h3[side].settings_known());
        CHECK(peer.qpack_max_table_capacity == 1024U);
        CHECK(peer.qpack_blocked_streams == 4U);
        CHECK(peer.max_field_section_size == 4096U);
        CHECK(peer.enable_connect_protocol);
        CHECK(peer.h3_datagram);
        CHECK(f.rig->h3[side].error() == 0U);
    }
}

TEST_CASE("http3: a configuration out of range or stream limits that cannot carry it are refused", "[http3]")
{
    CountingAllocator allocator;
    QuicStreamSet set;
    QuicDatagrams datagrams;
    REQUIRE(set.init(allocator, h3_limits(), QuicSide::Client));

    auto refused = [&](const Http3Config& config, QuicDatagrams* d = nullptr)
    {
        Http3Connection h3;
        return !h3.init(allocator, set, d, config);
    };
    Http3Config config = h3_config(QuicSide::Client);
    config.field_bytes = kHttp3MinFieldBytes - 1U;
    CHECK(refused(config, &datagrams));
    config = h3_config(QuicSide::Client);
    config.field_bytes = kHttp3MaxFieldBytes + 1U;
    CHECK(refused(config, &datagrams));
    config = h3_config(QuicSide::Client);
    config.datagram_bytes = 0U;
    CHECK(refused(config, &datagrams));
    config = h3_config(QuicSide::Client);
    config.datagram_bytes = 65536U;
    CHECK(refused(config, &datagrams));
    config = h3_config(QuicSide::Client);
    CHECK(refused(config, nullptr)); // datagrams asked for and none given
    config.field_bytes = 8192U;      // a section with its frame header passes the stream's window
    CHECK(refused(config, &datagrams));

    const auto refused_limits = [&](QuicStreamLimits limits, QuicSide side)
    {
        QuicStreamSet other;
        REQUIRE(other.init(allocator, limits, side));
        Http3Connection h3;
        Http3Config c = h3_config(side);
        c.datagrams = false;
        return !h3.init(allocator, other, nullptr, c);
    };
    QuicStreamLimits limits = h3_limits();
    limits.local_uni = 2U;
    CHECK(refused_limits(limits, QuicSide::Client));
    limits = h3_limits();
    limits.peer_uni = 2U;
    CHECK(refused_limits(limits, QuicSide::Client));
    limits = h3_limits();
    limits.local_bidi = 0U;
    CHECK(refused_limits(limits, QuicSide::Client));
    limits = h3_limits();
    limits.peer_bidi = 0U;
    CHECK(refused_limits(limits, QuicSide::Server));
    limits = h3_limits();
    limits.stream_window = 4096U;
    CHECK(refused_limits(limits, QuicSide::Client));
    limits = h3_limits();
    limits.receive_bytes = 4096U;
    limits.stream_window = 4096U;
    CHECK(refused_limits(limits, QuicSide::Client));

    allocator.refuse_after(0U);
    Http3Connection h3;
    CHECK_FALSE(h3.init(allocator, set, &datagrams, h3_config(QuicSide::Client)));
}

TEST_CASE("http3: a refused allocation at any step of init holds nothing", "[http3]")
{
    for (u32 grant = 0U; grant < 32U; ++grant)
    {
        CountingAllocator allocator;
        QuicStreamSet set;
        QuicDatagrams datagrams;
        REQUIRE(set.init(allocator, h3_limits(), QuicSide::Client));
        REQUIRE(datagrams.init(allocator, h3_datagram_limits()));
        allocator.refuse_after(grant);
        Http3Connection h3;
        const bool ready = h3.init(allocator, set, &datagrams, h3_config(QuicSide::Client));
        if (!ready)
        {
            allocator.refuse_after(~u32{0});
            Http3Event event;
            CHECK_FALSE(h3.poll(event));
        }
    }
}

TEST_CASE("http3: every SETTINGS has a reserved identifier and an unknown one is ignored", "[http3]")
{
    Fixture f;
    CHECK(f.server_control_error(settings({0x21U, 7U, 0x7FFFU, 1U})) == 0U);
    REQUIRE(f.rig->find(kH3Server, Http3EventKind::Settings) != nullptr);
    CHECK_FALSE(f.rig->h3[kH3Server].peer_settings().h3_datagram);
    CHECK(f.rig->h3[kH3Server].peer_settings().max_field_section_size == kHttp3NoFieldLimit);
}

TEST_CASE("http3: control stream rules", "[http3]")
{
    Fixture f;
    SECTION("no SETTINGS first")
    {
        H3Wire wire = frame_of(kGoAway, payload({0U}));
        CHECK(f.server_control_error(wire) == kH3MissingSettings);
    }
    SECTION("an unknown frame first is also not SETTINGS")
    {
        CHECK(f.server_control_error(frame_of(kGreaseFrame, H3Wire{})) == kH3MissingSettings);
    }
    SECTION("a second SETTINGS")
    {
        H3Wire wire = settings({});
        wire.raw(settings({}));
        CHECK(f.server_control_error(wire) == kH3FrameUnexpected);
    }
    SECTION("frames of other streams")
    {
        for (const u64 type : {kData, kHeaders, kPushPromise})
        {
            H3Wire wire = settings({});
            wire.raw(frame_of(type, H3Wire{}));
            CHECK(f.server_control_error(wire) == kH3FrameUnexpected);
        }
    }
    SECTION("HTTP/2's frames")
    {
        for (const u64 type : {u64{0x2U}, u64{0x6U}, u64{0x8U}, u64{0x9U}})
        {
            H3Wire wire = settings({});
            wire.raw(frame_of(type, H3Wire{}));
            CHECK(f.server_control_error(wire) == kH3FrameUnexpected);
        }
    }
    SECTION("reserved and unknown frames are skipped, even split")
    {
        H3Wire wire = settings({});
        H3Wire bytes;
        bytes.byte(1U).byte(2U).byte(3U);
        wire.raw(frame_of(kGreaseFrame, bytes));
        wire.raw(frame_of(0xFFU, H3Wire{}));
        CHECK(f.server_control_error(wire) == 0U);
    }
    SECTION("a control frame whose payload is not one integer")
    {
        H3Wire extra = payload({0U, 0U});
        H3Wire wire = settings({});
        wire.raw(frame_of(kMaxPushId, extra));
        CHECK(f.server_control_error(wire) == kH3FrameError);
        H3Wire empty = settings({});
        empty.raw(frame_of(kMaxPushId, H3Wire{}));
        CHECK(f.server_control_error(empty) == kH3FrameError);
        H3Wire long_wire = settings({});
        H3Wire nine;
        for (u32 i = 0U; i < 9U; ++i)
        {
            nine.byte(0U);
        }
        long_wire.raw(frame_of(kGoAway, nine));
        CHECK(f.server_control_error(long_wire) == kH3FrameError);
    }
    SECTION("a control frame announcing more than one integer, refused on its length")
    {
        H3Wire wire = settings({});
        wire.varint(kGoAway).varint(u64{1} << 40U);
        CHECK(f.server_control_error(wire) == kH3FrameError);
    }
    SECTION("a SETTINGS too long to hold")
    {
        H3Wire wire;
        wire.varint(kSettings).varint(u64{1} << 40U);
        CHECK(f.server_control_error(wire) == kHttp3ExcessiveLoad);
    }
}

TEST_CASE("http3: settings refusals", "[http3]")
{
    Fixture f;
    SECTION("HTTP/2's identifiers")
    {
        for (const u64 id : {u64{2U}, u64{3U}, u64{4U}, u64{5U}})
        {
            CHECK(f.server_control_error(settings({id, 1U})) == kH3SettingsError);
        }
    }
    SECTION("identifiers given twice")
    {
        for (const u64 id : {u64{1U}, u64{6U}, u64{7U}, u64{8U}, u64{0x33U}})
        {
            CHECK(f.server_control_error(settings({id, 1U, id, 1U})) == kH3SettingsError);
        }
    }
    SECTION("yes-or-no settings above 1")
    {
        CHECK(f.server_control_error(settings({8U, 2U})) == kH3SettingsError);
        CHECK(f.server_control_error(settings({0x33U, 2U})) == kH3SettingsError);
    }
    SECTION("a payload ending inside a pair")
    {
        H3Wire wire;
        wire.frame(kSettings, payload({1U}));
        CHECK(f.server_control_error(wire) == kH3FrameError);
    }
}

TEST_CASE("http3: a second critical stream of a type is refused", "[http3]")
{
    for (const u64 type : {kUniControl, kUniEncoder, kUniDecoder})
    {
        Fixture f;
        f.server();
        static_cast<void>(f.rig->open_typed(kH3Client, type));
        static_cast<void>(f.rig->open_typed(kH3Client, type));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == kH3StreamCreationError);
    }
}

TEST_CASE("http3: a critical stream closing ends the connection", "[http3]")
{
    for (const u64 type : {kUniControl, kUniEncoder, kUniDecoder})
    {
        SECTION("FIN")
        {
            Fixture f;
            f.server();
            const u64 id = f.rig->open_typed(kH3Client, type);
            f.rig->settle();
            f.rig->write(kH3Client, id, H3Wire{}, true);
            f.rig->settle();
            CHECK(f.rig->h3[kH3Server].error() == kH3ClosedCriticalStream);
        }
        SECTION("reset")
        {
            Fixture f;
            f.server();
            const u64 id = f.rig->open_typed(kH3Client, type);
            f.rig->settle();
            REQUIRE(f.rig->streams[kH3Client].reset(id, 99U) == QuicStreamError::None);
            f.rig->settle();
            CHECK(f.rig->h3[kH3Server].error() == kH3ClosedCriticalStream);
        }
    }
}

TEST_CASE("http3: STOP_SENDING on this side's critical stream ends the connection", "[http3]")
{
    Fixture f;
    f.server();
    f.rig->settle();
    // The server opened its control, encoder and decoder streams in that order: 3, 7 and 11.
    for (const u64 id : {u64{3U}, u64{7U}, u64{11U}})
    {
        Fixture g;
        g.server();
        g.rig->settle();
        REQUIRE(g.rig->streams[kH3Client].stop_sending(id, 1U) == QuicStreamError::None);
        g.rig->settle();
        CHECK(g.rig->h3[kH3Server].error() == kH3ClosedCriticalStream);
    }
}

TEST_CASE("http3: an unknown stream type is stopped, not fatal, and read no further", "[http3]")
{
    Fixture f;
    f.server();
    const u64 id = f.rig->open_typed(kH3Client, 0x21U);
    f.rig->write(kH3Client, id, H3Wire{}.byte(1U).byte(2U));
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK((f.rig->streams[kH3Client].send_state(id) == QuicSendState::ResetSent ||
           f.rig->streams[kH3Client].send_state(id) == QuicSendState::ResetRecvd));
}

TEST_CASE("http3: a stream closed before its type causes nothing", "[http3]")
{
    Fixture f;
    f.server();
    const u64 empty = f.rig->open(kH3Client, false);
    f.rig->write(kH3Client, empty, H3Wire{}, true);
    const u64 partial = f.rig->open(kH3Client, false);
    f.rig->write(kH3Client, partial, H3Wire{}.byte(0x40U), true); // the first byte of a two-byte type
    const u64 cut = f.rig->open(kH3Client, false);
    f.rig->settle();
    REQUIRE(f.rig->streams[kH3Client].reset(cut, 5U) == QuicStreamError::None);
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == 0U);
}

TEST_CASE("http3: a stream type arrives split across frames", "[http3]")
{
    Fixture f;
    f.server();
    const u64 id = f.rig->open(kH3Client, false);
    f.rig->write(kH3Client, id, H3Wire{}.byte(0x40U)); // 0 in two bytes: the control stream
    f.rig->settle();
    CHECK_FALSE(f.rig->h3[kH3Server].settings_known());
    f.rig->write(kH3Client, id, H3Wire{}.byte(0x00U));
    f.rig->write(kH3Client, id, settings({}));
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].settings_known());
}

// ---- Push is never offered ----

TEST_CASE("http3: a push stream is refused by either side's rule", "[http3]")
{
    {
        Fixture f;
        f.server();
        static_cast<void>(f.rig->open_typed(kH3Client, kUniPush));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == kH3StreamCreationError);
    }
    {
        Fixture f;
        f.client();
        static_cast<void>(f.rig->open_typed(kH3Server, kUniPush));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == kH3IdError);
    }
}

TEST_CASE("http3: a PUSH_PROMISE is an error by side", "[http3]")
{
    {
        Fixture f;
        H3Wire wire = headers_frame();
        wire.raw(frame_of(kPushPromise, payload({0U})));
        CHECK(f.server_request_error(wire) == kH3FrameUnexpected);
    }
    {
        Fixture f;
        CHECK(f.client_response_error(frame_of(kPushPromise, payload({0U}))) == kH3IdError);
    }
}

TEST_CASE("http3: CANCEL_PUSH names a push that cannot exist", "[http3]")
{
    Fixture f;
    H3Wire wire = settings({});
    wire.raw(frame_of(kCancelPush, payload({1U})));
    CHECK(f.server_control_error(wire) == kH3IdError);
}

TEST_CASE("http3: MAX_PUSH_ID only rises and only reaches a server", "[http3]")
{
    Fixture f;
    SECTION("raising is fine")
    {
        H3Wire wire = settings({});
        wire.raw(frame_of(kMaxPushId, payload({1U})));
        wire.raw(frame_of(kMaxPushId, payload({1U})));
        wire.raw(frame_of(kMaxPushId, payload({2U})));
        CHECK(f.server_control_error(wire) == 0U);
    }
    SECTION("lowering is not")
    {
        H3Wire wire = settings({});
        wire.raw(frame_of(kMaxPushId, payload({5U})));
        wire.raw(frame_of(kMaxPushId, payload({4U})));
        CHECK(f.server_control_error(wire) == kH3IdError);
    }
    SECTION("a client has no use for it")
    {
        f.client();
        const u64 control = f.rig->open_control(kH3Server);
        f.rig->write(kH3Server, control, frame_of(kMaxPushId, payload({0U})));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == kH3FrameUnexpected);
    }
}

// ---- Request stream frames ----

TEST_CASE("http3: request streams refuse the frames of other streams", "[http3]")
{
    for (const u64 type : {kCancelPush, kSettings, kGoAway, kMaxPushId, u64{0x2U}, u64{0x6U}, u64{0x8U}, u64{0x9U}})
    {
        Fixture f;
        H3Wire wire = headers_frame();
        wire.raw(frame_of(type, payload({0U})));
        CHECK(f.server_request_error(wire) == kH3FrameUnexpected);
    }
}

TEST_CASE("http3: DATA before HEADERS and a frame after trailers are out of sequence", "[http3]")
{
    {
        Fixture f;
        CHECK(f.server_request_error(data_frame("x")) == kH3FrameUnexpected);
    }
    {
        Fixture f;
        H3Wire wire = headers_frame();
        wire.raw(headers_frame_of_trailers());
        wire.raw(data_frame("x"));
        CHECK(f.server_request_error(wire) == kH3FrameUnexpected);
    }
    {
        Fixture f;
        H3Wire wire = headers_frame();
        wire.raw(headers_frame_of_trailers());
        wire.raw(headers_frame_of_trailers());
        CHECK(f.server_request_error(wire) == kH3FrameUnexpected);
    }
}

TEST_CASE("http3: reserved frames pass anywhere on a request stream", "[http3]")
{
    Fixture f;
    H3Wire wire = frame_of(kGreaseFrame, H3Wire{}.byte(1U));
    wire.raw(headers_frame());
    wire.raw(frame_of(kGreaseFrame, H3Wire{}.byte(2U).byte(3U)));
    wire.raw(data_frame("hello"));
    wire.raw(frame_of(0x1F21U, H3Wire{}));
    CHECK(f.server_request_error(wire, true) == 0U);
    REQUIRE(f.rig->find(kH3Server, Http3EventKind::Headers) != nullptr);
    CHECK(f.rig->received[kH3Server] == "hello");
    CHECK(f.rig->find(kH3Server, Http3EventKind::Finished) != nullptr);
}

TEST_CASE("http3: a frame cut by the stream's end is H3_FRAME_ERROR", "[http3]")
{
    {
        Fixture f; // a frame header's first byte
        CHECK(f.server_request_error(H3Wire{}.byte(0x40U), true) == kH3FrameError);
    }
    {
        Fixture f; // a section shorter than its length
        H3Wire wire;
        wire.varint(kHeaders).varint(40U).byte(0U);
        CHECK(f.server_request_error(wire, true) == kH3FrameError);
    }
    {
        Fixture f; // DATA shorter than its length
        H3Wire wire = headers_frame();
        wire.varint(kData).varint(10U).byte(0x61U);
        CHECK(f.server_request_error(wire, true) == kH3FrameError);
    }
    {
        Fixture f; // an unknown frame shorter than its length
        H3Wire wire = headers_frame();
        wire.varint(0x40U).varint(10U).byte(1U);
        CHECK(f.server_request_error(wire, true) == kH3FrameError);
    }
}

TEST_CASE("http3: a request ending before its HEADERS is incomplete, and the application never hears", "[http3]")
{
    Fixture f;
    CHECK(f.server_request_error(H3Wire{}, true) == 0U);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Aborted) == 0U);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
    CHECK((f.rig->streams[kH3Client].recv_state(0U) == QuicRecvState::ResetRead ||
           f.rig->streams[kH3Client].recv_state(0U) == QuicRecvState::ResetRecvd));
}

TEST_CASE("http3: a malformed request resets with H3_MESSAGE_ERROR and is never reported", "[http3]")
{
    Fixture f;
    H3Wire section;
    section.byte(0x00U).byte(0x00U).byte(0xC0U | 17U); // :method alone
    CHECK(f.server_request_error(frame_of(kHeaders, section)) == 0U);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Aborted) == 0U);
    QuicStreamEvent event;
    bool reset_seen = false;
    while (f.rig->streams[kH3Client].poll(event))
    {
        reset_seen = reset_seen || ((event.events & kQuicStreamReset) != 0U && event.reset_error == kH3MessageError);
    }
    CHECK(reset_seen);
}

TEST_CASE("http3: a broken QPACK section is a connection error", "[http3]")
{
    Fixture f;
    H3Wire section;
    section.byte(0x00U).byte(0x00U).byte(0xFFU).byte(0xFFU).byte(0xFFU).byte(0x7FU); // a static index past the table
    CHECK(f.server_request_error(frame_of(kHeaders, section)) == kQpackDecompressionFailed);
}

TEST_CASE("http3: a broken instruction on a QPACK stream is a connection error", "[http3]")
{
    {
        Fixture f;
        f.server();
        const u64 id = f.rig->open_typed(kH3Client, kUniEncoder);
        f.rig->write(kH3Client, id, H3Wire{}.byte(0xC0U | 63U).byte(0xFFU).byte(0x7FU)); // beyond the static table
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == kQpackEncoderStreamError);
    }
    {
        Fixture f;
        f.server();
        const u64 id = f.rig->open_typed(kH3Client, kUniDecoder);
        f.rig->write(kH3Client, id, H3Wire{}.byte(0x01U)); // an increment past anything sent
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == kQpackDecoderStreamError);
    }
}

TEST_CASE("http3: a client refuses a server-initiated bidirectional stream", "[http3]")
{
    Fixture f;
    f.client();
    static_cast<void>(f.rig->open(kH3Server, true));
    f.rig->write(kH3Server, 1U, H3Wire{}.byte(0U));
    f.rig->settle();
    CHECK(f.rig->h3[kH3Client].error() == kH3StreamCreationError);
}

// ---- Messages ----

TEST_CASE("http3: a request and a response with a body and trailers", "[http3]")
{
    Fixture f;
    f.both();
    H3Fields request = h3_get();
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(request.span(), false, stream) == Http3Error::None);
    CHECK(stream == 0U);
    usize taken = 0U;
    REQUIRE(f.rig->h3[kH3Client].send_body(stream, text_span("ping"), true, taken) == Http3Error::None);
    CHECK(taken == 4U);
    f.rig->settle();
    const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::Headers);
    REQUIRE(got != nullptr);
    CHECK(got->has(":path", "/index"));
    CHECK(got->has(":scheme", "https"));
    CHECK_FALSE(got->event.early);
    CHECK(f.rig->received[kH3Server] == "ping");
    CHECK(f.rig->find(kH3Server, Http3EventKind::Finished) != nullptr);

    REQUIRE(f.rig->h3[kH3Server].send_response(stream, h3_ok().span(), false) == Http3Error::None);
    REQUIRE(f.rig->h3[kH3Server].send_body(stream, text_span("pong"), false, taken) == Http3Error::None);
    REQUIRE(f.rig->h3[kH3Server].send_trailers(stream, H3Fields{{"x-sum", "1"}}.span()) == Http3Error::None);
    f.rig->settle();
    const H3Seen* const answer = f.rig->find(kH3Client, Http3EventKind::Headers);
    REQUIRE(answer != nullptr);
    CHECK(answer->event.info.status == 200U);
    CHECK(answer->has("server", "cerid"));
    CHECK(f.rig->received[kH3Client] == "pong");
    const H3Seen* const trailers = f.rig->find(kH3Client, Http3EventKind::Trailers);
    REQUIRE(trailers != nullptr);
    CHECK(trailers->has("x-sum", "1"));
    CHECK(f.rig->find(kH3Client, Http3EventKind::Finished) != nullptr);
    CHECK(f.rig->h3[kH3Client].error() == 0U);
    CHECK(f.rig->h3[kH3Server].error() == 0U);
}

TEST_CASE("http3: a fieldless response finishing at once", "[http3]")
{
    Fixture f;
    f.both();
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), true, stream) == Http3Error::None);
    f.rig->settle();
    REQUIRE(f.rig->find(kH3Server, Http3EventKind::Finished) != nullptr);
    REQUIRE(f.rig->h3[kH3Server].send_response(stream, h3_ok().span(), true) == Http3Error::None);
    f.rig->settle();
    CHECK(f.rig->find(kH3Client, Http3EventKind::Finished) != nullptr);
    CHECK(f.rig->streams[kH3Client].slot_of(stream) == kQuicNoStreamSlot);
    CHECK(f.rig->streams[kH3Server].slot_of(stream) == kQuicNoStreamSlot);
}

TEST_CASE("http3: interim responses come before the final one", "[http3]")
{
    Fixture f;
    f.both();
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), true, stream) == Http3Error::None);
    f.rig->settle();
    REQUIRE(f.rig->h3[kH3Server].send_response(stream, H3Fields{{":status", "103"}, {"link", "</a>"}}.span(), false) ==
            Http3Error::None);
    REQUIRE(f.rig->h3[kH3Server].send_response(stream, H3Fields{{":status", "100"}}.span(), false) == Http3Error::None);
    usize taken = 0U;
    CHECK(f.rig->h3[kH3Server].send_body(stream, text_span("x"), false, taken) == Http3Error::Invalid);
    CHECK(f.rig->h3[kH3Server].send_response(stream, H3Fields{{":status", "100"}}.span(), true) == Http3Error::Invalid);
    REQUIRE(f.rig->h3[kH3Server].send_response(stream, h3_ok().span(), true) == Http3Error::None);
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Client, Http3EventKind::Interim) == 2U);
    REQUIRE(f.rig->find(kH3Client, Http3EventKind::Interim) != nullptr);
    CHECK(f.rig->find(kH3Client, Http3EventKind::Interim)->event.info.status == 103U);
    CHECK(f.rig->how_many(kH3Client, Http3EventKind::Headers) == 1U);
}

TEST_CASE("http3: content-length is held to the DATA that came", "[http3]")
{
    const H3Fields with_length{
        {":method", "POST"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {"content-length", "3"}};
    SECTION("exactly")
    {
        Fixture f;
        f.both();
        u64 stream = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(with_length.span(), false, stream) == Http3Error::None);
        usize taken = 0U;
        REQUIRE(f.rig->h3[kH3Client].send_body(stream, text_span("abc"), true, taken) == Http3Error::None);
        f.rig->settle();
        CHECK(f.rig->find(kH3Server, Http3EventKind::Finished) != nullptr);
    }
    SECTION("too much is caught at the frame's length")
    {
        Fixture f;
        f.both();
        u64 stream = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(with_length.span(), false, stream) == Http3Error::None);
        usize taken = 0U;
        REQUIRE(f.rig->h3[kH3Client].send_body(stream, text_span("abcd"), true, taken) == Http3Error::None);
        f.rig->settle();
        const H3Seen* const aborted = f.rig->find(kH3Server, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == kH3MessageError);
        CHECK(f.rig->received[kH3Server].empty());
    }
    SECTION("too little at the end")
    {
        Fixture f;
        f.both();
        u64 stream = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(with_length.span(), false, stream) == Http3Error::None);
        usize taken = 0U;
        REQUIRE(f.rig->h3[kH3Client].send_body(stream, text_span("ab"), true, taken) == Http3Error::None);
        f.rig->settle();
        const H3Seen* const aborted = f.rig->find(kH3Server, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == kH3MessageError);
    }
}

TEST_CASE("http3: a response to HEAD, a 204 and a 304 carry no content whatever content-length says", "[http3]")
{
    const H3Fields head{{":method", "HEAD"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}};
    SECTION("HEAD with a length and nothing after it finishes")
    {
        Fixture f;
        f.both();
        u64 stream = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(head.span(), true, stream) == Http3Error::None);
        f.rig->settle();
        REQUIRE(f.rig->h3[kH3Server].send_response(stream, H3Fields{{":status", "200"}, {"content-length", "9"}}.span(),
                                                   true) == Http3Error::None);
        f.rig->settle();
        CHECK(f.rig->find(kH3Client, Http3EventKind::Finished) != nullptr);
    }
    for (const char* status : {"204", "304"})
    {
        Fixture f;
        f.both();
        u64 stream = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), true, stream) == Http3Error::None);
        f.rig->settle();
        REQUIRE(f.rig->h3[kH3Server].send_response(stream, H3Fields{{":status", status}}.span(), false) ==
                Http3Error::None);
        usize taken = 0U;
        REQUIRE(f.rig->h3[kH3Server].send_body(stream, text_span("x"), true, taken) == Http3Error::None);
        f.rig->settle();
        const H3Seen* const aborted = f.rig->find(kH3Client, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == kH3MessageError);
    }
}

TEST_CASE("http3: a malformed response reaches the application as an abort", "[http3]")
{
    Fixture f;
    H3Wire section;
    section.byte(0x00U).byte(0x00U).byte(0xC0U | 17U); // :method on a response
    CHECK(f.client_response_error(frame_of(kHeaders, section)) == 0U);
    const H3Seen* const aborted = f.rig->find(kH3Client, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.error == kH3MessageError);
}

TEST_CASE("http3: a response ending before HEADERS is malformed", "[http3]")
{
    Fixture f;
    CHECK(f.client_response_error(H3Wire{}, true) == 0U);
    const H3Seen* const aborted = f.rig->find(kH3Client, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.error == kH3MessageError);
}

TEST_CASE("http3: malformed trailers abort", "[http3]")
{
    Fixture f;
    f.server();
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 stream = f.rig->open(kH3Client, true);
    H3Wire wire = headers_frame();
    H3Wire section;
    section.byte(0x00U).byte(0x00U).byte(0xC0U | 17U); // a pseudo-field in trailers
    wire.raw(frame_of(kHeaders, section));
    f.rig->write(kH3Client, stream, wire);
    f.rig->settle();
    const H3Seen* const aborted = f.rig->find(kH3Server, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.error == kH3MessageError);
}

TEST_CASE("http3: DATA larger than the ring passes through without being held", "[http3]")
{
    Fixture f;
    f.both();
    u64 stream = 0U;
    H3Fields request = h3_get();
    REQUIRE(f.rig->h3[kH3Client].open_request(request.span(), false, stream) == Http3Error::None);
    Text expected;
    u8 block[1000];
    usize sent = 0U;
    const usize total = 100000U;
    while (sent < total)
    {
        for (usize at = 0U; at < sizeof(block); ++at)
        {
            block[at] = static_cast<u8>((sent + at) % 251U);
        }
        usize taken = 0U;
        const Http3Error error =
            f.rig->h3[kH3Client].send_body(stream, containers::ConstSpan<u8>(block, sizeof(block)), false, taken);
        REQUIRE((error == Http3Error::None || error == Http3Error::Blocked));
        for (usize at = 0U; at < taken; ++at)
        {
            expected += static_cast<char>(block[at]);
        }
        sent += taken;
        f.rig->settle();
    }
    REQUIRE(f.rig->h3[kH3Client].finish(stream) == Http3Error::None);
    f.rig->settle();
    CHECK(f.rig->received[kH3Server].size() == total);
    CHECK((f.rig->received[kH3Server] == expected));
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK(f.rig->find(kH3Server, Http3EventKind::Finished) != nullptr);
}

TEST_CASE("http3: a section lying across the end of the ring is decoded whole", "[http3]")
{
    Fixture f;
    f.server();
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 stream = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, stream, headers_frame());
    // Almost the whole ring as DATA, consumed as it comes, leaves the read position close to the end.
    H3Wire body;
    for (u32 i = 0U; i < 8192U - 20U; ++i)
    {
        body.byte(0x61U);
    }
    H3Wire big = frame_of(kData, body);
    f.rig->write(kH3Client, stream, big);
    f.rig->settle();
    CHECK(f.rig->received[kH3Server].size() == body.size);
    // A trailer whose bytes wrap the ring.
    H3Wire section;
    section.byte(0x00U).byte(0x00U);
    for (u32 i = 0U; i < 24U; ++i)
    {
        section.byte(0x27U).byte(0x00U).raw(text_span("x-field")).byte(0x03U).raw(text_span("abc"));
    }
    f.rig->write(kH3Client, stream, frame_of(kHeaders, section), true);
    f.rig->settle();
    const H3Seen* const trailers = f.rig->find(kH3Server, Http3EventKind::Trailers);
    REQUIRE(trailers != nullptr);
    CHECK(trailers->field_count == 24U);
    CHECK(trailers->has("x-field", "abc"));
    CHECK(f.rig->h3[kH3Server].error() == 0U);
}

TEST_CASE("http3: a Body event comes again while bytes remain, and a peek stops at the frame", "[http3]")
{
    Fixture f;
    f.server();
    f.rig->drain_bodies = false;
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 stream = f.rig->open(kH3Client, true);
    H3Wire wire = headers_frame();
    wire.raw(data_frame("abc"));
    wire.raw(data_frame("de"));
    f.rig->write(kH3Client, stream, wire, true);
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Body) == 1U);

    Http3Body body = f.rig->h3[kH3Server].peek_body(stream);
    REQUIRE(body.data.size() == 3U);
    REQUIRE(f.rig->h3[kH3Server].consume_body(stream, 1U) == Http3Error::None);
    f.rig->poll(kH3Server);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Body) == 2U);
    body = f.rig->h3[kH3Server].peek_body(stream);
    REQUIRE(body.data.size() == 2U);
    CHECK(f.rig->h3[kH3Server].consume_body(stream, 3U) == Http3Error::Invalid);
    REQUIRE(f.rig->h3[kH3Server].consume_body(stream, 2U) == Http3Error::None);
    f.rig->poll(kH3Server);
    body = f.rig->h3[kH3Server].peek_body(stream);
    REQUIRE(body.data.size() == 2U);
    REQUIRE(f.rig->h3[kH3Server].consume_body(stream, 2U) == Http3Error::None);
    f.rig->poll(kH3Server);
    CHECK(f.rig->find(kH3Server, Http3EventKind::Finished) != nullptr);
    CHECK(f.rig->h3[kH3Server].peek_body(stream).data.empty());
    CHECK(f.rig->h3[kH3Server].consume_body(stream, 1U) == Http3Error::Invalid);
}

// ---- CONNECT and extended CONNECT ----

TEST_CASE("http3: extended CONNECT needs the peer's setting, then tunnels DATA", "[http3]")
{
    const H3Fields extended{{":method", "CONNECT"},
                            {":protocol", "webtransport"},
                            {":scheme", "https"},
                            {":authority", "a"},
                            {":path", "/"}};
    Fixture f;
    f.both();
    u64 stream = 0U;
    CHECK(f.rig->h3[kH3Client].open_request(extended.span(), false, stream) == Http3Error::NotReady);
    f.rig->settle();
    REQUIRE(f.rig->h3[kH3Client].open_request(extended.span(), false, stream) == Http3Error::None);
    f.rig->settle();
    const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::Headers);
    REQUIRE(got != nullptr);
    CHECK(got->event.info.extended);
    CHECK(got->event.info.connect);
    REQUIRE(f.rig->h3[kH3Server].send_response(stream, H3Fields{{":status", "200"}}.span(), false) == Http3Error::None);
    f.rig->settle();
    usize taken = 0U;
    REQUIRE(f.rig->h3[kH3Client].send_body(stream, text_span("up"), false, taken) == Http3Error::None);
    REQUIRE(f.rig->h3[kH3Server].send_body(stream, text_span("down"), false, taken) == Http3Error::None);
    f.rig->settle();
    CHECK(f.rig->received[kH3Server] == "up");
    CHECK(f.rig->received[kH3Client] == "down");
    CHECK(f.rig->h3[kH3Client].send_trailers(stream, H3Fields{{"x", "y"}}.span()) == Http3Error::Invalid);
    CHECK(f.rig->h3[kH3Server].send_trailers(stream, H3Fields{{"x", "y"}}.span()) == Http3Error::Invalid);
}

TEST_CASE("http3: a server without the setting refuses extended CONNECT", "[http3]")
{
    Http3Config server_config = h3_config(QuicSide::Server);
    server_config.connect_protocol = false;
    Fixture f;
    f.rig = std::make_unique<H3Rig>(f.allocator, false, true, h3_config(QuicSide::Client), server_config);
    REQUIRE(f.rig->ok);
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 stream = f.rig->open(kH3Client, true);
    H3Wire section;
    // :method CONNECT (15), :scheme https (23), :path / (1), :authority literal, :protocol literal.
    section.byte(0x00U).byte(0x00U).byte(0xC0U | 15U).byte(0xC0U | 23U).byte(0xC0U | 1U);
    section.byte(0x50U).byte(0x01U).byte('a');
    section.byte(0x27U).byte(0x02U).raw(text_span(":protocol")).byte(0x01U).byte('x');
    f.rig->write(kH3Client, stream, frame_of(kHeaders, section));
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
}

TEST_CASE("http3: a tunnel carries only DATA", "[http3]")
{
    Fixture f;
    f.server();
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 stream = f.rig->open(kH3Client, true);
    H3Wire section;
    section.byte(0x00U).byte(0x00U).byte(0xC0U | 15U).byte(0x50U).byte(0x01U).byte('a');
    H3Wire wire = frame_of(kHeaders, section);
    wire.raw(data_frame("x"));
    wire.raw(headers_frame_of_trailers());
    f.rig->write(kH3Client, stream, wire);
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == kH3FrameUnexpected);
}

TEST_CASE("http3: a 2xx to CONNECT at a client leaves only DATA, a 4xx leaves an ordinary response", "[http3]")
{
    const H3Fields connect{{":method", "CONNECT"}, {":authority", "a:1"}};
    SECTION("2xx")
    {
        Fixture f;
        f.client();
        static_cast<void>(f.rig->open_control(kH3Server));
        u64 stream = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(connect.span(), false, stream) == Http3Error::None);
        f.rig->settle();
        H3Wire wire = response_frame();
        wire.raw(data_frame("x"));
        wire.raw(headers_frame_of_trailers());
        f.rig->write(kH3Server, stream, wire);
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == kH3FrameUnexpected);
    }
    SECTION("4xx")
    {
        Fixture f;
        f.client();
        static_cast<void>(f.rig->open_control(kH3Server));
        u64 stream = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(connect.span(), false, stream) == Http3Error::None);
        f.rig->settle();
        H3Wire section;
        section.byte(0x00U).byte(0x00U).byte(0xC0U | 27U); // :status 404
        H3Wire wire = frame_of(kHeaders, section);
        wire.raw(data_frame("no"));
        wire.raw(headers_frame_of_trailers());
        f.rig->write(kH3Server, stream, wire, true);
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == 0U);
        CHECK(f.rig->find(kH3Client, Http3EventKind::Trailers) != nullptr);
        CHECK(f.rig->find(kH3Client, Http3EventKind::Finished) != nullptr);
    }
}

// ---- HTTP datagrams ----

TEST_CASE("http3: datagrams of an extended CONNECT cross both ways", "[http3]")
{
    const H3Fields extended{{":method", "CONNECT"},
                            {":protocol", "webtransport"},
                            {":scheme", "https"},
                            {":authority", "a"},
                            {":path", "/"}};
    Fixture f;
    f.both();
    f.rig->settle();
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(extended.span(), false, stream) == Http3Error::None);
    f.rig->settle();
    REQUIRE(f.rig->h3[kH3Server].send_response(stream, H3Fields{{":status", "200"}}.span(), false) == Http3Error::None);
    f.rig->settle();
    REQUIRE(f.rig->h3[kH3Client].send_datagram(stream, text_span("up"), f.rig->now) == Http3Error::None);
    REQUIRE(f.rig->h3[kH3Server].send_datagram(stream, text_span("down"), f.rig->now) == Http3Error::None);
    f.rig->settle();
    const H3Seen* const server = f.rig->find(kH3Server, Http3EventKind::Datagram);
    REQUIRE(server != nullptr);
    CHECK(server->payload == "up");
    CHECK(server->event.stream == stream);
    const H3Seen* const client = f.rig->find(kH3Client, Http3EventKind::Datagram);
    REQUIRE(client != nullptr);
    CHECK(client->payload == "down");
}

TEST_CASE("http3: a datagram names an extended CONNECT with its send part open", "[http3]")
{
    Fixture f;
    f.both();
    f.rig->settle();
    u64 stream = 0U;
    CHECK(f.rig->h3[kH3Client].send_datagram(0U, text_span("x"), f.rig->now) == Http3Error::Unknown);
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), false, stream) == Http3Error::None);
    CHECK(f.rig->h3[kH3Client].send_datagram(stream, text_span("x"), f.rig->now) == Http3Error::Invalid);
    const H3Fields extended{
        {":method", "CONNECT"}, {":protocol", "w"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}};
    u64 tunnel = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(extended.span(), true, tunnel) == Http3Error::None);
    CHECK(f.rig->h3[kH3Client].send_datagram(tunnel, text_span("x"), f.rig->now) == Http3Error::Unknown);
    const H3Fields open_extended{
        {":method", "CONNECT"}, {":protocol", "w"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/b"}};
    u64 live = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(open_extended.span(), false, live) == Http3Error::None);
    H3Wire big;
    for (u32 i = 0U; i < 1501U; ++i)
    {
        big.byte(1U);
    }
    CHECK(f.rig->h3[kH3Client].send_datagram(live, big.span(), f.rig->now) == Http3Error::TooLarge);
    for (u32 i = 0U; i < 8U; ++i)
    {
        REQUIRE(f.rig->h3[kH3Client].send_datagram(live, text_span("q"), f.rig->now) == Http3Error::None);
    }
    CHECK(f.rig->h3[kH3Client].send_datagram(live, text_span("q"), f.rig->now) == Http3Error::Blocked);
}

TEST_CASE("http3: a datagram without the peer's setting does not go", "[http3]")
{
    Http3Config server_config = h3_config(QuicSide::Server);
    server_config.datagrams = false;
    Fixture f;
    f.both(h3_config(QuicSide::Client), server_config);
    f.rig->settle();
    const H3Fields extended{
        {":method", "CONNECT"}, {":protocol", "w"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}};
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(extended.span(), false, stream) == Http3Error::None);
    CHECK(f.rig->h3[kH3Client].send_datagram(stream, text_span("x"), f.rig->now) == Http3Error::NotReady);
}

TEST_CASE("http3: a hostile datagram's prefix", "[http3]")
{
    const auto arrive = [](Fixture& f, const H3Wire& wire)
    {
        f.server();
        QuicFrame frame;
        frame.kind = QuicFrameKind::Datagram;
        frame.data = wire.span();
        REQUIRE(f.rig->datagrams[kH3Server].on_frame(frame) == QuicTransportError::NoError);
        f.rig->settle();
    };
    SECTION("too short for its Quarter Stream ID")
    {
        Fixture f;
        arrive(f, H3Wire{}.byte(0x40U));
        CHECK(f.rig->h3[kH3Server].error() == kH3DatagramError);
    }
    SECTION("empty")
    {
        Fixture f;
        arrive(f, H3Wire{});
        CHECK(f.rig->h3[kH3Server].error() == kH3DatagramError);
    }
    SECTION("past the largest stream")
    {
        Fixture f;
        arrive(f, H3Wire{}.varint(u64{1} << 60U));
        CHECK(f.rig->h3[kH3Server].error() == kH3DatagramError);
    }
    SECTION("the largest stream, not open, is dropped")
    {
        Fixture f;
        arrive(f, H3Wire{}.varint((u64{1} << 60U) - 1U).byte(1U));
        CHECK(f.rig->h3[kH3Server].error() == 0U);
        CHECK(f.rig->datagrams[kH3Server].held() == 0U);
    }
}

TEST_CASE("http3: a datagram on a stream with no datagram semantics aborts it", "[http3]")
{
    Fixture f;
    f.server();
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 stream = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, stream, headers_frame());
    f.rig->settle();
    REQUIRE(f.rig->find(kH3Server, Http3EventKind::Headers) != nullptr);
    QuicFrame frame;
    frame.kind = QuicFrameKind::Datagram;
    const H3Wire wire = H3Wire{}.varint(stream / 4U).byte(1U);
    frame.data = wire.span();
    REQUIRE(f.rig->datagrams[kH3Server].on_frame(frame) == QuicTransportError::NoError);
    f.rig->settle();
    const H3Seen* const aborted = f.rig->find(kH3Server, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.error == kH3DatagramError);
    CHECK(f.rig->h3[kH3Server].error() == 0U);
}

// ---- GOAWAY ----

TEST_CASE("http3: a server's GOAWAY stops new requests and rejects those above it", "[http3]")
{
    Fixture f;
    f.client();
    const u64 control = f.rig->open_control(kH3Server);
    f.rig->settle();
    u64 first = 0U;
    u64 second = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), true, first) == Http3Error::None);
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), true, second) == Http3Error::None);
    REQUIRE(second == 4U);
    f.rig->settle();
    f.rig->write(kH3Server, control, frame_of(kGoAway, payload({4U})));
    f.rig->settle();
    const H3Seen* const goaway = f.rig->find(kH3Client, Http3EventKind::GoAway);
    REQUIRE(goaway != nullptr);
    CHECK(goaway->event.id == 4U);
    CHECK(f.rig->h3[kH3Client].goaway_received());
    const H3Seen* const aborted = f.rig->find(kH3Client, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.stream == 4U);
    CHECK(aborted->event.error == kH3RequestRejected);
    CHECK(f.rig->how_many(kH3Client, Http3EventKind::Aborted) == 1U);

    u64 third = 0U;
    CHECK(f.rig->h3[kH3Client].open_request(h3_get().span(), true, third) == Http3Error::GoingAway);
    CHECK(f.rig->h3[kH3Client].error() == 0U);
}

TEST_CASE("http3: a GOAWAY from a server names a client bidirectional stream and never rises", "[http3]")
{
    {
        Fixture f;
        f.client();
        const u64 control = f.rig->open_control(kH3Server);
        f.rig->write(kH3Server, control, frame_of(kGoAway, payload({1U})));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == kH3IdError);
    }
    {
        Fixture f;
        f.client();
        const u64 control = f.rig->open_control(kH3Server);
        f.rig->write(kH3Server, control, frame_of(kGoAway, payload({8U})));
        f.rig->write(kH3Server, control, frame_of(kGoAway, payload({8U})));
        f.rig->write(kH3Server, control, frame_of(kGoAway, payload({12U})));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == kH3IdError);
    }
}

TEST_CASE("http3: a client's GOAWAY names pushes and also never rises", "[http3]")
{
    Fixture f;
    H3Wire wire = settings({});
    wire.raw(frame_of(kGoAway, payload({7U})));
    CHECK(f.server_control_error(wire) == 0U);
    REQUIRE(f.rig->find(kH3Server, Http3EventKind::GoAway) != nullptr);
    Fixture g;
    H3Wire rising = settings({});
    rising.raw(frame_of(kGoAway, payload({7U})));
    rising.raw(frame_of(kGoAway, payload({8U})));
    CHECK(g.server_control_error(rising) == kH3IdError);
}

TEST_CASE("http3: a server rejects requests at or above its GOAWAY, and handles those below", "[http3]")
{
    Fixture f;
    f.server();
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 first = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, first, headers_frame());
    f.rig->settle();
    REQUIRE(f.rig->h3[kH3Server].goaway(true));
    f.rig->settle();
    const u64 second = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, second, headers_frame());
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 1U);
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    QuicStreamEvent event;
    bool rejected = false;
    while (f.rig->streams[kH3Client].poll(event))
    {
        rejected = rejected || (event.id == second && (event.events & kQuicStreamReset) != 0U &&
                                event.reset_error == kH3RequestRejected);
    }
    CHECK(rejected);
    REQUIRE(f.rig->h3[kH3Server].send_response(first, h3_ok().span(), true) == Http3Error::None);
}

TEST_CASE("http3: GOAWAY values fall from gentle to exact and never rise", "[http3]")
{
    Fixture f;
    f.both();
    f.rig->settle();
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), true, stream) == Http3Error::None);
    f.rig->settle();
    REQUIRE(f.rig->h3[kH3Server].goaway(false));
    f.rig->settle();
    const H3Seen* gentle = f.rig->find(kH3Client, Http3EventKind::GoAway);
    REQUIRE(gentle != nullptr);
    CHECK(gentle->event.id == (u64{1} << 62U) - 4U);
    CHECK(f.rig->how_many(kH3Client, Http3EventKind::Aborted) == 0U);
    REQUIRE(f.rig->h3[kH3Server].goaway(true));
    REQUIRE(f.rig->h3[kH3Server].goaway(false)); // stays at the exact value
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Client, Http3EventKind::GoAway) == 3U);
    CHECK(f.rig->find(kH3Client, Http3EventKind::GoAway, 1U)->event.id == 4U);
    CHECK(f.rig->find(kH3Client, Http3EventKind::GoAway, 2U)->event.id == 4U);
    CHECK(f.rig->h3[kH3Client].error() == 0U);
    // A client's own gentle GOAWAY.
    REQUIRE(f.rig->h3[kH3Client].goaway(false));
    REQUIRE(f.rig->h3[kH3Client].goaway(true));
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::GoAway) == 2U);
    CHECK(f.rig->find(kH3Server, Http3EventKind::GoAway, 0U)->event.id == (u64{1} << 62U) - 1U);
    CHECK(f.rig->find(kH3Server, Http3EventKind::GoAway, 1U)->event.id == 0U);
    CHECK(f.rig->h3[kH3Server].error() == 0U);
}

// ---- QPACK blocking ----

TEST_CASE("http3: a section that waits for the encoder stream stays in the QUIC buffer and goes on", "[http3]")
{
    Fixture f;
    f.both();
    f.rig->settle();
    const H3Fields request{
        {":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {"x-custom", "value-one"}};
    // The encoder inserts on second sight, so the second section references an entry the first one's stream carries.
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(request.span(), true, stream) == Http3Error::None);
    f.rig->settle();
    const u32 headers_before = static_cast<u32>(f.rig->how_many(kH3Server, Http3EventKind::Headers));
    REQUIRE(headers_before == 1U);

    f.rig->held_stream[kH3Client] = 2U + (1U * 4U); // the client's encoder stream, the second it opened
    REQUIRE(f.rig->h3[kH3Client].open_request(request.span(), true, stream) == Http3Error::None);
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == headers_before);
    CHECK(f.rig->h3[kH3Server].error() == 0U);

    f.rig->held_stream[kH3Client] = ~u64{0};
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == headers_before + 1U);
    const H3Seen* const last = f.rig->find(kH3Server, Http3EventKind::Headers, headers_before);
    REQUIRE(last != nullptr);
    CHECK(last->has("x-custom", "value-one"));
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK(f.rig->h3[kH3Client].error() == 0U);
}

TEST_CASE("http3: a blocked stream reset by its peer stops waiting", "[http3]")
{
    Fixture f;
    f.both();
    f.rig->settle();
    const H3Fields request{
        {":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {"x-custom", "value-one"}};
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(request.span(), true, stream) == Http3Error::None);
    f.rig->settle();
    f.rig->held_stream[kH3Client] = 6U;
    REQUIRE(f.rig->h3[kH3Client].open_request(request.span(), false, stream) == Http3Error::None);
    f.rig->settle();
    REQUIRE(f.rig->h3[kH3Client].cancel(stream, kH3RequestCancelled) == Http3Error::None);
    f.rig->settle();
    const H3Seen* const aborted = f.rig->find(kH3Server, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.stream == stream);
    CHECK(aborted->event.error == kH3RequestCancelled);
    f.rig->held_stream[kH3Client] = ~u64{0};
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 1U);
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK(f.rig->h3[kH3Client].error() == 0U);
}

// ---- Sections too large ----

TEST_CASE("http3: a HEADERS frame over the limit is answered 431 on its length alone", "[http3]")
{
    Fixture f;
    f.server();
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 stream = f.rig->open(kH3Client, true);
    H3Wire wire;
    wire.varint(kHeaders).varint(u64{1} << 30U).byte(0U);
    f.rig->write(kH3Client, stream, wire);
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
    QuicStreamRead read = f.rig->streams[kH3Client].peek(stream);
    REQUIRE(read.data.size() > 2U);
    CHECK(read.fin);
    CHECK(read.data[0] == 0x01U); // a HEADERS frame comes back
}

TEST_CASE("http3: a section over the text limit is answered 431", "[http3]")
{
    Fixture f;
    f.server();
    static_cast<void>(f.rig->open_control(kH3Client));
    const u64 stream = f.rig->open(kH3Client, true);
    H3Wire section;
    section.byte(0x00U).byte(0x00U).byte(0xC0U | 17U).byte(0xC0U | 1U).byte(0xC0U | 23U);
    // Many short lines: 400 fields of 10 value bytes hold more text than the frame's bytes suggest once each counts its
    // name from the static table.
    H3Wire lines = section;
    for (u32 i = 0U; i < 400U; ++i)
    {
        lines.byte(0x50U).byte(0x0AU);
        for (u32 c = 0U; c < 10U; ++c)
        {
            lines.byte('v');
        }
    }
    f.rig->write(kH3Client, stream, frame_of(kHeaders, lines));
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
    const QuicStreamRead read = f.rig->streams[kH3Client].peek(stream);
    REQUIRE(read.data.size() > 2U);
    CHECK(read.fin);
}

TEST_CASE("http3: a section too large for a client aborts the response", "[http3]")
{
    Fixture f;
    const u64 stream = f.client_request();
    H3Wire wire;
    wire.varint(kHeaders).varint(u64{1} << 30U);
    f.rig->write(kH3Server, stream, wire);
    f.rig->settle();
    CHECK(f.rig->h3[kH3Client].error() == 0U);
    const H3Seen* const aborted = f.rig->find(kH3Client, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.error == kHttp3ExcessiveLoad);
}

TEST_CASE("http3: this side's sends stay within the peer's limit and its own", "[http3]")
{
    Http3Config server_config = h3_config(QuicSide::Server);
    Http3Config client_config = h3_config(QuicSide::Client);
    server_config.field_bytes = 1024U;
    Fixture f;
    f.both(client_config, server_config);
    f.rig->settle();
    H3Fields big = h3_get();
    Text value(1100U, 'v');
    big.add("x-big", value.c_str());
    u64 stream = 0U;
    CHECK(f.rig->h3[kH3Client].open_request(big.span(), true, stream) == Http3Error::TooLarge);
    // Larger than the sender's own scratch.
    Text huge(5000U, 'v');
    H3Fields too_big = h3_get();
    too_big.add("x-huge", huge.c_str());
    Fixture g;
    g.both();
    g.rig->settle();
    CHECK(g.rig->h3[kH3Client].open_request(too_big.span(), true, stream) == Http3Error::TooLarge);
}

TEST_CASE("http3: a response over the peer's limit is refused before it costs anything", "[http3]")
{
    Http3Config client_config = h3_config(QuicSide::Client);
    client_config.field_bytes = 1024U;
    Fixture f;
    f.both(client_config, h3_config(QuicSide::Server));
    f.rig->settle();
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), true, stream) == Http3Error::None);
    f.rig->settle();
    Text value(1100U, 'v');
    H3Fields big = h3_ok();
    big.add("x-big", value.c_str());
    CHECK(f.rig->h3[kH3Server].send_response(stream, big.span(), true) == Http3Error::TooLarge);
    REQUIRE(f.rig->h3[kH3Server].send_response(stream, h3_ok().span(), true) == Http3Error::None);
}

TEST_CASE("http3: a reset stream gives up its place among the blocked ones", "[http3]")
{
    Http3Config server_config = h3_config(QuicSide::Server);
    server_config.decoder.blocked_streams = 1U;
    Fixture f;
    f.rig = std::make_unique<H3Rig>(f.allocator, false, true, h3_config(QuicSide::Client), server_config);
    REQUIRE(f.rig->ok);
    static_cast<void>(f.rig->open_control(kH3Client));
    H3Wire section;
    // A Required Insert Count of 1 (encoded 2 with 32 entries at most), a Base of 0 and a dynamic reference.
    section.byte(0x02U).byte(0x00U).byte(0x80U);
    const u64 first = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, first, frame_of(kHeaders, section));
    f.rig->settle();
    REQUIRE(f.rig->streams[kH3Client].reset(first, kH3RequestCancelled) == QuicStreamError::None);
    f.rig->settle();
    const u64 second = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, second, frame_of(kHeaders, section));
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == 0U); // over the limit would be QPACK_DECOMPRESSION_FAILED
    const u64 third = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, third, frame_of(kHeaders, section));
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == kQpackDecompressionFailed);
}

// ---- Capsules ----

TEST_CASE("http capsules: a stream split anywhere gives the same capsules", "[http3]")
{
    H3Wire wire;
    wire.varint(0x00U).varint(3U).raw(text_span("abc"));
    wire.varint(0x17U).varint(0U);
    wire.varint(0x12345U).varint(70U);
    for (u32 i = 0U; i < 70U; ++i)
    {
        wire.byte(static_cast<u8>(i));
    }
    wire.varint(0x00U).varint(1U).byte('z');
    for (usize piece = 1U; piece <= wire.size; ++piece)
    {
        HttpCapsuleReader reader;
        usize capsules = 0U;
        Text value;
        for (usize at = 0U; at < wire.size;)
        {
            const usize size = ((wire.size - at) < piece) ? (wire.size - at) : piece;
            containers::ConstSpan<u8> chunk_bytes(wire.bytes + at, size);
            at += size;
            for (;;)
            {
                HttpCapsuleChunk chunk;
                bool got = false;
                const usize used = reader.next(chunk_bytes, chunk, got);
                chunk_bytes = chunk_bytes.subspan(used);
                if (got)
                {
                    REQUIRE(chunk.offset == value.size());
                    for (const u8 c : chunk.value)
                    {
                        value += static_cast<char>(c);
                    }
                    if (chunk.complete)
                    {
                        ++capsules;
                        REQUIRE(value.size() == chunk.length);
                        if (capsules == 1U)
                        {
                            CHECK(chunk.type == 0U);
                            CHECK(value == "abc");
                        }
                        if (capsules == 2U)
                        {
                            CHECK(chunk.type == 0x17U);
                            CHECK(value.empty());
                        }
                        if (capsules == 3U)
                        {
                            CHECK(chunk.type == 0x12345U);
                            CHECK(value.size() == 70U);
                        }
                        value.clear();
                    }
                }
                if (used == 0U && !got)
                {
                    break;
                }
            }
        }
        CHECK(capsules == 4U);
        CHECK(reader.at_boundary());
    }
}

TEST_CASE("http capsules: a stream may end only between capsules", "[http3]")
{
    HttpCapsuleReader reader;
    CHECK(reader.at_boundary());
    HttpCapsuleChunk chunk;
    bool got = false;
    const u8 head[] = {0x40U}; // the first byte of a two-byte type
    CHECK(reader.next(containers::ConstSpan<u8>(head, 1U), chunk, got) == 1U);
    CHECK_FALSE(got);
    CHECK_FALSE(reader.at_boundary());
    HttpCapsuleReader in_value;
    const u8 partial[] = {0x00U, 0x04U, 0x61U};
    CHECK(in_value.next(containers::ConstSpan<u8>(partial, 3U), chunk, got) == 3U);
    CHECK(got);
    CHECK_FALSE(chunk.complete);
    CHECK(chunk.length == 4U);
    CHECK_FALSE(in_value.at_boundary());
}

TEST_CASE("http capsules: the header's size and bytes", "[http3]")
{
    u8 bytes[16];
    containers::WireWriter out{containers::Span<u8>(bytes, sizeof(bytes))};
    put_http_capsule_header(out, 0x40U, 300U);
    CHECK(out.written() == http_capsule_header_bytes(0x40U, 300U));
    CHECK(out.written() == 4U);
    HttpCapsuleReader reader;
    HttpCapsuleChunk chunk;
    bool got = false;
    CHECK(reader.next(containers::ConstSpan<u8>(bytes, out.written()), chunk, got) == 4U);
    CHECK_FALSE(got);
    CHECK_FALSE(reader.at_boundary());
}

TEST_CASE("http capsules: Capsule-Protocol is one Boolean true", "[http3]")
{
    CHECK(http_capsule_protocol(H3Fields{{"capsule-protocol", "?1"}}.span()));
    CHECK(http_capsule_protocol(H3Fields{{"capsule-protocol", "?1;a=b"}}.span()));
    CHECK_FALSE(http_capsule_protocol(H3Fields{}.span()));
    CHECK_FALSE(http_capsule_protocol(H3Fields{{"capsule-protocol", "?0"}}.span()));
    CHECK_FALSE(http_capsule_protocol(H3Fields{{"capsule-protocol", "1"}}.span()));
    CHECK_FALSE(http_capsule_protocol(H3Fields{{"capsule-protocol", "?"}}.span()));
    CHECK_FALSE(http_capsule_protocol(H3Fields{{"capsule-protocol", "?1x"}}.span()));
    CHECK_FALSE(http_capsule_protocol(H3Fields{{"capsule-protocol", "?1"}, {"capsule-protocol", "?1"}}.span()));
    CHECK_FALSE(http_capsule_protocol(H3Fields{{"other", "?1"}}.span()));
}

// ---- 0-RTT ----

TEST_CASE("http3: a client resuming works from what it remembers and holds the server to it", "[http3]")
{
    const auto run = [](Http3Settings remembered, Http3Config server_config, u64 expected)
    {
        Http3Config client_config = h3_config(QuicSide::Client);
        client_config.remembered = &remembered;
        Fixture f;
        f.both(client_config, server_config);
        // Before the server's SETTINGS the remembered values apply.
        CHECK(f.rig->h3[kH3Client].peer_settings().qpack_max_table_capacity == remembered.qpack_max_table_capacity);
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == expected);
    };
    Http3Settings same;
    same.qpack_max_table_capacity = 1024U;
    same.qpack_blocked_streams = 4U;
    same.max_field_section_size = 4096U;
    same.enable_connect_protocol = true;
    same.h3_datagram = true;
    run(same, h3_config(QuicSide::Server), 0U);

    Http3Settings lower = same;
    lower.qpack_max_table_capacity = 512U;
    run(lower, h3_config(QuicSide::Server), 0U);

    Http3Settings capacity = same;
    capacity.qpack_max_table_capacity = 2048U;
    run(capacity, h3_config(QuicSide::Server), kH3SettingsError);
    Http3Settings blocked = same;
    blocked.qpack_blocked_streams = 5U;
    run(blocked, h3_config(QuicSide::Server), kH3SettingsError);
    Http3Settings field = same;
    field.max_field_section_size = kHttp3NoFieldLimit;
    run(field, h3_config(QuicSide::Server), kH3SettingsError);

    Http3Config no_connect = h3_config(QuicSide::Server);
    no_connect.connect_protocol = false;
    run(same, no_connect, kH3SettingsError);
    Http3Config no_datagram = h3_config(QuicSide::Server);
    no_datagram.datagrams = false;
    run(same, no_datagram, kH3SettingsError);
}

TEST_CASE("http3: a server's early context changes with the settings a client could have used", "[http3]")
{
    u8 a[kHttp3EarlyContextBytes] = {};
    u8 b[kHttp3EarlyContextBytes] = {};
    Http3Config config = h3_config(QuicSide::Server);
    REQUIRE(Http3Connection::early_context(config, containers::Span<u8>(a, sizeof(a))) == sizeof(a));
    const auto differs = [&](const Http3Config& other)
    {
        REQUIRE(Http3Connection::early_context(other, containers::Span<u8>(b, sizeof(b))) == sizeof(b));
        return std::memcmp(a, b, sizeof(a)) != 0;
    };
    CHECK_FALSE(differs(config));
    Http3Config changed = config;
    changed.decoder.table_capacity = 2048U;
    CHECK(differs(changed));
    changed = config;
    changed.decoder.blocked_streams = 5U;
    CHECK(differs(changed));
    changed = config;
    changed.field_bytes = 8192U;
    CHECK(differs(changed));
    changed = config;
    changed.connect_protocol = false;
    CHECK(differs(changed));
    changed = config;
    changed.datagrams = false;
    CHECK(differs(changed));
    CHECK(Http3Connection::early_context(config, containers::Span<u8>(a, sizeof(a) - 1U)) == 0U);
}

TEST_CASE("http3: a request whose section came in 0-RTT is marked replayable", "[http3]")
{
    Fixture f;
    f.server();
    static_cast<void>(f.rig->open_control(kH3Client));
    f.rig->settle();
    const u64 stream = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, stream, headers_frame());
    QuicFrame frame;
    QuicStreamRecord record;
    while (f.rig->streams[kH3Client].next_frame(1200U, f.rig->now, kH3Rtt, frame, record))
    {
        REQUIRE(f.rig->streams[kH3Server].on_frame(frame, true) == QuicTransportError::NoError);
        f.rig->streams[kH3Client].on_acked(record);
    }
    f.rig->settle();
    const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::Headers);
    REQUIRE(got != nullptr);
    CHECK(got->event.early);
}

// ---- Stream ends ----

TEST_CASE("http3: a peer's reset and a STOP_SENDING reach the application", "[http3]")
{
    Fixture f;
    f.both();
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), false, stream) == Http3Error::None);
    f.rig->settle();
    REQUIRE(f.rig->streams[kH3Client].stop_sending(stream, kH3NoError) == QuicStreamError::None);
    f.rig->settle();
    const H3Seen* const stopped = f.rig->find(kH3Server, Http3EventKind::Stopped);
    REQUIRE(stopped != nullptr);
    CHECK(stopped->event.error == kH3NoError);
    CHECK(f.rig->h3[kH3Server].send_response(stream, h3_ok().span(), true) == Http3Error::Unknown);

    REQUIRE(f.rig->streams[kH3Client].reset(stream, kH3RequestCancelled) == QuicStreamError::None);
    f.rig->settle();
    const H3Seen* const aborted = f.rig->find(kH3Server, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.error == kH3RequestCancelled);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Aborted) == 1U);
}

TEST_CASE("http3: a Writable event follows a Blocked send", "[http3]")
{
    Fixture f;
    f.both();
    u64 stream = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), false, stream) == Http3Error::None);
    Http3Error error = Http3Error::None;
    H3Wire block;
    for (u32 i = 0U; i < 8000U; ++i)
    {
        block.byte('z');
    }
    usize taken = 0U;
    while (error == Http3Error::None)
    {
        error = f.rig->h3[kH3Client].send_body(stream, block.span(), false, taken);
        if (error != Http3Error::None || taken == 0U)
        {
            break;
        }
    }
    CHECK(error == Http3Error::Blocked);
    H3Fields fields = h3_get();
    u64 other = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(fields.span(), false, other) == Http3Error::None);
    f.rig->settle();
    CHECK(f.rig->find(kH3Client, Http3EventKind::Writable) != nullptr);
    CHECK(f.rig->find(kH3Client, Http3EventKind::Writable)->event.stream == stream);
}

TEST_CASE("http3: a call out of sequence or on a stream gone returns its error", "[http3]")
{
    Fixture f;
    f.both();
    u64 stream = 0U;
    usize taken = 0U;
    CHECK(f.rig->h3[kH3Client].send_response(0U, h3_ok().span(), true) == Http3Error::Unknown);
    CHECK(f.rig->h3[kH3Client].send_body(0U, text_span("x"), false, taken) == Http3Error::Unknown);
    CHECK(f.rig->h3[kH3Client].send_trailers(0U, H3Fields{{"x", "y"}}.span()) == Http3Error::Unknown);
    CHECK(f.rig->h3[kH3Client].finish(0U) == Http3Error::Unknown);
    CHECK(f.rig->h3[kH3Client].cancel(0U, 1U) == Http3Error::Unknown);
    CHECK(f.rig->h3[kH3Client].consume_body(0U, 1U) == Http3Error::Unknown);

    CHECK(f.rig->h3[kH3Client].open_request(H3Fields{{":method", "GET"}}.span(), true, stream) == Http3Error::Invalid);
    CHECK(f.rig->h3[kH3Server].open_request(h3_get().span(), true, stream) == Http3Error::NotReady);
    REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), false, stream) == Http3Error::None);
    CHECK(f.rig->h3[kH3Client].send_response(stream, h3_ok().span(), false) == Http3Error::Invalid);
    CHECK(f.rig->h3[kH3Client].send_trailers(stream, H3Fields{{":x", "y"}}.span()) == Http3Error::Invalid);
    REQUIRE(f.rig->h3[kH3Client].send_body(stream, {}, false, taken) == Http3Error::None);
    REQUIRE(f.rig->h3[kH3Client].send_body(stream, {}, true, taken) == Http3Error::None);
    CHECK(f.rig->h3[kH3Client].finish(stream) == Http3Error::Unknown);
    CHECK(f.rig->h3[kH3Client].send_body(stream, text_span("x"), false, taken) == Http3Error::Unknown);
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].send_body(stream, text_span("x"), false, taken) == Http3Error::Invalid);
    CHECK(f.rig->h3[kH3Server].finish(stream) == Http3Error::Invalid);
    CHECK(f.rig->h3[kH3Server].send_trailers(stream, H3Fields{{"x", "y"}}.span()) == Http3Error::Invalid);
    CHECK(f.rig->h3[kH3Server].send_response(stream, H3Fields{{"x", "y"}}.span(), true) == Http3Error::Invalid);
}

TEST_CASE("http3: once a connection error is set everything answers Closed", "[http3]")
{
    Fixture f;
    CHECK(f.server_control_error(settings({2U, 1U})) == kH3SettingsError);
    Http3Connection& h3 = f.rig->h3[kH3Server];
    usize taken = 0U;
    u64 stream = 0U;
    CHECK(h3.open_request(h3_get().span(), true, stream) == Http3Error::Closed);
    CHECK(h3.send_response(0U, h3_ok().span(), true) == Http3Error::Closed);
    CHECK(h3.send_body(0U, text_span("x"), false, taken) == Http3Error::Closed);
    CHECK(h3.send_trailers(0U, H3Fields{{"x", "y"}}.span()) == Http3Error::Closed);
    CHECK(h3.finish(0U) == Http3Error::Closed);
    CHECK(h3.cancel(0U, 1U) == Http3Error::Closed);
    CHECK(h3.send_datagram(0U, text_span("x"), 0U) == Http3Error::Closed);
    CHECK_FALSE(h3.goaway(true));
    CHECK_FALSE(h3.start());
    Http3Event event;
    CHECK_FALSE(h3.poll(event));
    CHECK(h3.error() == kH3SettingsError); // the first error stays
}

TEST_CASE("http3: a start that cannot open its streams fails with the protocol error", "[http3]")
{
    CountingAllocator allocator;
    QuicStreamSet set;
    REQUIRE(set.init(allocator, h3_limits(), QuicSide::Client));
    Http3Connection h3;
    Http3Config config = h3_config(QuicSide::Client);
    config.datagrams = false;
    REQUIRE(h3.init(allocator, set, nullptr, config));
    CHECK_FALSE(h3.start()); // the peer's parameters have not granted any stream
    CHECK(h3.error() == kH3GeneralProtocolError);
    CHECK_FALSE(h3.started());
}

TEST_CASE("http3: a grease stream goes only when the peer leaves room", "[http3]")
{
    Fixture f;
    f.both();
    // Slots: control, encoder, decoder and the reserved one, on each side.
    CHECK(f.rig->streams[kH3Client].open_streams() == 4U);
    f.rig->settle();
    QuicStreamLimits exact = h3_limits();
    exact.peer_uni = 3U;
    exact.local_uni = 3U;
    CountingAllocator allocator;
    H3Rig rig(allocator, true, true, h3_config(QuicSide::Client), h3_config(QuicSide::Server), exact);
    REQUIRE(rig.ok);
    CHECK(rig.streams[kH3Client].open_streams() == 3U);
    rig.settle();
    CHECK(rig.h3[kH3Client].error() == 0U);
    CHECK(rig.h3[kH3Server].error() == 0U);
}

TEST_CASE("http3: a RESET_STREAM_AT that cuts a frame is the stream's reset, never a wait", "[http3]")
{
    Fixture f;
    f.server();
    QuicStreamSet& client = f.rig->streams[kH3Client];
    QuicStreamSet& server = f.rig->streams[kH3Server];
    f.rig->settle();
    const u32 open_before = server.open_streams();
    usize taken = 0U;

    SECTION("a request stream stopped inside its first frame header")
    {
        u64 id = 0U;
        REQUIRE(client.open(true, id) == QuicStreamError::None);
        const u8 half[] = {0x01U, 0x40U}; // HEADERS, then one byte of a two-byte length
        REQUIRE(client.write(id, containers::ConstSpan<u8>(half, sizeof(half)), false, taken) == QuicStreamError::None);
        REQUIRE(client.reset(id, 99U, sizeof(half)) == QuicStreamError::None);
        f.rig->settle();
        const H3Seen* const aborted = f.rig->find(kH3Server, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == 99U);
        REQUIRE(client.stop_sending(id, 1U) == QuicStreamError::None); // ends the other half, freeing the stream
        f.rig->settle();
    }
    SECTION("a unidirectional stream stopped inside its type")
    {
        u64 id = 0U;
        REQUIRE(client.open(false, id) == QuicStreamError::None);
        const u8 half = 0x40U;
        REQUIRE(client.write(id, containers::ConstSpan<u8>(&half, 1U), false, taken) == QuicStreamError::None);
        REQUIRE(client.reset(id, 99U, 1U) == QuicStreamError::None);
        f.rig->settle();
    }
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK(server.open_streams() == open_before);
}
