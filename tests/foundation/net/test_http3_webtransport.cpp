// crd-net -- what HTTP/3 does for WebTransport (4.2a; docs/design/network-foundation.md#webtransport;
// draft-ietf-webtrans-http3-17): the offer in SETTINGS and what each side requires of the other, the wait for a
// client's SETTINGS, the stream headers split anywhere, the reliable resets and their application codes, and streams
// given up before they named a session.

#include "http3_fixture.hpp"
#include "ingress_fixture.hpp"

#include <crd/containers/wire_bytes.hpp>
#include <crd/net/http3.hpp>

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::crypto::QuicSide;

constexpr u64 kHeaders = 0x1U;
constexpr u64 kSettings = 0x4U;
constexpr u64 kGoAway = 0x7U;
constexpr u64 kUniControl = 0x0U;
constexpr u64 kWtUni = 0x54U;
constexpr u64 kWtBidi = 0x41U;
constexpr u64 kEnableConnect = 0x8U;
constexpr u64 kDatagram = 0x33U;
constexpr u64 kDatagramOld = 0xFFD277U;
constexpr u64 kWtEnabled = 0x20B8933BU;
constexpr u64 kWtUniLimit = 0x2B64U;
constexpr u64 kWtBidiLimit = 0x2B65U;
constexpr u64 kWtDataLimit = 0x2B61U;
constexpr u64 kWtLegacy = 0xC671706AU;
constexpr u64 kEightByteSession = 0x3FFFFFFFFFFFFFFCULL;

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

// A static-table GET QPACK decodes without a table.
H3Wire get_section()
{
    H3Wire wire;
    wire.byte(0x00U).byte(0x00U).byte(0xC0U | 17U).byte(0xC0U | 1U).byte(0xC0U | 23U);
    wire.byte(0x50U).byte(0x01U).byte('a');
    return wire;
}

H3Wire get_frame()
{
    H3Wire wire;
    wire.frame(kHeaders, get_section());
    return wire;
}

containers::ConstSpan<u8> text_span(const char* text)
{
    return containers::ConstSpan<u8>(reinterpret_cast<const u8*>(text), std::strlen(text));
}

const H3Fields& wt_legacy_connect()
{
    static const H3Fields kFields{{":method", "CONNECT"},
                                  {":protocol", "webtransport"},
                                  {":scheme", "https"},
                                  {":authority", "a"},
                                  {":path", "/wt"}};
    return kFields;
}

const H3Fields& ok_200()
{
    static const H3Fields kFields{{":status", "200"}};
    return kFields;
}

struct Fixture
{
    CountingAllocator allocator;
    std::unique_ptr<H3Rig> rig;

    void make(bool client_h3, bool server_h3, const Http3Config& client = h3_wt_config(QuicSide::Client),
              const Http3Config& server = h3_wt_config(QuicSide::Server), const QuicStreamLimits& limits = h3_limits(),
              const QuicStreamLimits* server_limits = nullptr)
    {
        rig = std::make_unique<H3Rig>(allocator, client_h3, server_h3, client, server, limits, true, server_limits);
        REQUIRE(rig->ok);
    }
    void both() { make(true, true); }
    // A server under test and a client written by hand, its SETTINGS those a WebTransport client sends.
    void server()
    {
        make(false, true);
        hand_settings(kH3Client, settings({kDatagram, 1U, kWtEnabled, 1U}));
        rig->settle();
    }
    // A client under test and a server written by hand, with a WebTransport server's SETTINGS.
    void client()
    {
        make(true, false);
        hand_settings(kH3Server, settings({kEnableConnect, 1U, kDatagram, 1U, kWtEnabled, 1U}));
        rig->settle();
    }
    void hand_settings(usize side, const H3Wire& wire)
    {
        const u64 control = rig->open_typed(side, kUniControl);
        rig->write(side, control, wire);
    }

    // An established session on two real ends: its CONNECT stream.
    u64 session()
    {
        rig->settle();
        u64 stream = 0U;
        REQUIRE(rig->h3[kH3Client].open_request(h3_wt_connect().span(), false, stream) == Http3Error::None);
        rig->settle();
        REQUIRE(rig->find(kH3Server, Http3EventKind::Headers) != nullptr);
        REQUIRE(rig->h3[kH3Server].send_response(stream, ok_200().span(), false) == Http3Error::None);
        rig->settle();
        REQUIRE(rig->h3[kH3Client].error() == 0U);
        REQUIRE(rig->h3[kH3Server].error() == 0U);
        return stream;
    }

    // Everything readable on a stream, given up.
    Text drain(usize side, u64 stream)
    {
        Text got;
        for (QuicStreamRead in = rig->streams[side].peek(stream); !in.data.empty();
             in = rig->streams[side].peek(stream))
        {
            got += Text(reinterpret_cast<const char*>(in.data.data()), in.data.size());
            REQUIRE(rig->streams[side].consume(stream, in.data.size(), true) == QuicStreamError::None);
        }
        return got;
    }

    // Hands a hand-written side's events away, so its finished streams free their slots.
    void forget_events(usize side)
    {
        QuicStreamEvent event;
        while (rig->streams[side].poll(event))
        {
        }
    }
};

// ---- Figure 4 ----

TEST_CASE("webtransport: application codes follow figure 4 around every reserved point", "[http3][webtransport]")
{
    CHECK(wt_http_code(0U) == kWtApplicationErrorFirst);
    CHECK(wt_http_code(0x1DU) == kWtApplicationErrorFirst + 0x1DU);
    CHECK(wt_http_code(0x1EU) == kWtApplicationErrorFirst + 0x1FU); // the point between them is reserved
    CHECK(wt_http_code(0xFFFFFFFFU) == kWtApplicationErrorLast);

    u32 back = 0U;
    for (const u32 n : {0U, 1U, 0x1CU, 0x1DU, 0x1EU, 0x1FU, 0x3BU, 0x3CU, 0x3DU, 0x7FFFFFFFU, 0xFFFFFFFEU, 0xFFFFFFFFU})
    {
        const u64 code = wt_http_code(n);
        CHECK((code - 0x21U) % 0x1FU != 0U);
        REQUIRE(wt_application_code(code, back));
        CHECK(back == n);
    }
    for (u32 n = 0U; n < 4096U; ++n)
    {
        REQUIRE(wt_application_code(wt_http_code(n), back));
        REQUIRE(back == n);
    }

    CHECK_FALSE(wt_application_code(kWtApplicationErrorFirst + 0x1EU, back)); // reserved
    CHECK_FALSE(wt_application_code(kWtApplicationErrorFirst - 1U, back));
    CHECK_FALSE(wt_application_code(kWtApplicationErrorLast + 1U, back));
    CHECK_FALSE(wt_application_code(0U, back));
    CHECK_FALSE(wt_application_code(kWtSessionGone, back));
}

// ---- The offer ----

TEST_CASE("webtransport: SETTINGS carry the offer in both directions", "[http3][webtransport]")
{
    Fixture f;
    f.both();
    f.rig->settle();
    for (const usize side : {kH3Client, kH3Server})
    {
        const Http3Settings& peer = f.rig->h3[side].peer_settings();
        CHECK(peer.wt.enabled);
        CHECK(peer.wt.max_streams_uni == 16U);
        CHECK(peer.wt.max_streams_bidi == 17U);
        CHECK(peer.wt.max_data == (u64{1} << 20U));
        CHECK(peer.wt.legacy_sessions == 1U);
        CHECK(f.rig->h3[side].wt_ready(false));
        CHECK(f.rig->h3[side].wt_ready(true));
    }
}

TEST_CASE("webtransport: nothing is offered, or read, unless configured", "[http3][webtransport]")
{
    Fixture f;
    f.make(false, true, h3_config(QuicSide::Client), h3_config(QuicSide::Server));
    f.hand_settings(kH3Client, settings({kWtEnabled, 7U, kWtUniLimit, 3U, kDatagramOld, 9U, kWtLegacy, 5U}));
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    const Http3Settings& peer = f.rig->h3[kH3Server].peer_settings();
    CHECK_FALSE(peer.wt.enabled);
    CHECK(peer.wt.max_streams_uni == 0U);
    CHECK(peer.wt.legacy_sessions == 0U);
    CHECK_FALSE(peer.h3_datagram);
    CHECK_FALSE(f.rig->h3[kH3Server].wt_ready(false));
}

TEST_CASE("webtransport: SETTINGS are held to section 3.1 and 9.2", "[http3][webtransport]")
{
    SECTION("a server's SETTINGS_WT_ENABLED above 1 is an error at a client")
    {
        Fixture f;
        f.make(true, false);
        f.hand_settings(kH3Server, settings({kWtEnabled, 2U}));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == kH3SettingsError);
    }
    SECTION("a client's is ignored by a server, whatever it says")
    {
        Fixture f;
        f.make(false, true);
        f.hand_settings(kH3Client, settings({kDatagram, 1U, kWtEnabled, 2U}));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == 0U);
    }
    SECTION("the older datagram setting is read as 0x33 is")
    {
        Fixture f;
        f.make(false, true);
        f.hand_settings(kH3Client, settings({kDatagramOld, 1U}));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].peer_settings().h3_datagram);

        Fixture g;
        g.make(false, true);
        g.hand_settings(kH3Client, settings({kDatagramOld, 2U}));
        g.rig->settle();
        CHECK(g.rig->h3[kH3Server].error() == kH3SettingsError);
    }
    SECTION("each may appear only once")
    {
        for (const u64 id : {kWtEnabled, kWtUniLimit, kWtBidiLimit, kWtDataLimit, kWtLegacy, kDatagramOld})
        {
            Fixture f;
            f.make(false, true);
            f.hand_settings(kH3Client, settings({id, 1U, id, 1U}));
            f.rig->settle();
            CHECK(f.rig->h3[kH3Server].error() == kH3SettingsError);
        }
    }
}

TEST_CASE("webtransport: an offer needs what the draft requires of this side", "[http3][webtransport]")
{
    CountingAllocator allocator;
    const auto accepted = [&](Http3Config config, QuicStreamLimits limits)
    {
        QuicStreamSet streams;
        QuicDatagrams datagrams;
        REQUIRE(streams.init(allocator, limits, config.side));
        REQUIRE(datagrams.init(allocator, h3_datagram_limits()));
        Http3Connection h3;
        return h3.init(allocator, streams, &datagrams, config);
    };
    QuicStreamLimits without_reset = h3_limits();
    without_reset.reset_stream_at = false;
    CHECK(accepted(h3_wt_config(QuicSide::Server), h3_limits()));
    CHECK_FALSE(accepted(h3_wt_config(QuicSide::Server), without_reset));
    CHECK_FALSE(accepted(h3_wt_config(QuicSide::Client), without_reset));

    Http3Config config = h3_wt_config(QuicSide::Server);
    config.datagrams = false;
    CHECK_FALSE(accepted(config, h3_limits()));

    config = h3_wt_config(QuicSide::Server);
    config.connect_protocol = false;
    CHECK_FALSE(accepted(config, h3_limits()));
    config.side = QuicSide::Client;
    CHECK(accepted(config, h3_limits())); // a client's own setting says nothing about what it can open

    config = h3_wt_config(QuicSide::Server);
    config.webtransport.max_streams_uni = kWtMaxStreams + 1U;
    CHECK_FALSE(accepted(config, h3_limits()));
    config.webtransport.max_streams_uni = kWtMaxStreams;
    CHECK(accepted(config, h3_limits()));
    config.webtransport.max_streams_bidi = kWtMaxStreams + 1U;
    CHECK_FALSE(accepted(config, h3_limits()));
}

// ---- 0-RTT ----

TEST_CASE("webtransport: settings are not remembered, nor bound to tickets", "[http3][webtransport]")
{
    Http3Settings remembered;
    remembered.enable_connect_protocol = true;
    remembered.h3_datagram = true;
    remembered.wt.enabled = true;
    remembered.wt.max_data = 1U << 20U;
    remembered.wt.legacy_sessions = 9U;

    SECTION("a client may not use a remembered offer")
    {
        Http3Config client = h3_wt_config(QuicSide::Client);
        client.remembered = &remembered;
        Fixture f;
        f.make(true, false, client);
        u64 stream = 0U;
        CHECK(f.rig->h3[kH3Client].open_request(h3_wt_connect().span(), false, stream) == Http3Error::NotReady);
        CHECK(f.rig->h3[kH3Client].open_request(wt_legacy_connect().span(), false, stream) == Http3Error::NotReady);
    }
    SECTION("a server that no longer offers it breaks no 0-RTT promise")
    {
        Http3Config client = h3_wt_config(QuicSide::Client);
        client.remembered = &remembered;
        Fixture f;
        f.make(true, false, client);
        f.hand_settings(kH3Server, settings({kEnableConnect, 1U, kDatagram, 1U}));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == 0U);
        CHECK_FALSE(f.rig->h3[kH3Client].wt_ready(false));
    }
    SECTION("the early context ignores the offer")
    {
        u8 with[kHttp3EarlyContextBytes] = {};
        u8 without[kHttp3EarlyContextBytes] = {};
        Http3Config plain = h3_wt_config(QuicSide::Server);
        plain.webtransport = Http3WebTransportSettings{};
        const Http3Config offering = h3_wt_config(QuicSide::Server);
        REQUIRE(Http3Connection::early_context(offering, containers::Span<u8>(with, sizeof(with))) == sizeof(with));
        REQUIRE(Http3Connection::early_context(plain, containers::Span<u8>(without, sizeof(without))) ==
                sizeof(without));
        CHECK(std::memcmp(with, without, sizeof(with)) == 0);
    }
}

TEST_CASE("webtransport: a CONNECT or a stream header in 0-RTT is refused", "[http3][webtransport]")
{
    const auto deliver_early = [](H3Rig& rig)
    {
        QuicFrame frame;
        QuicStreamRecord record;
        while (rig.streams[kH3Client].next_frame(1200U, rig.now, kH3Rtt, frame, record))
        {
            REQUIRE(rig.streams[kH3Server].on_frame(frame, true) == QuicTransportError::NoError);
            rig.streams[kH3Client].on_acked(record);
        }
    };

    SECTION("the CONNECT")
    {
        Http3Config client = h3_config(QuicSide::Client); // sends the token unchecked, as a hostile client does
        Fixture f;
        f.make(true, true, client);
        f.rig->settle();
        u64 stream = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(h3_wt_connect().span(), false, stream) == Http3Error::None);
        deliver_early(*f.rig);
        f.rig->settle();
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
        const H3Seen* const got = f.rig->find(kH3Client, Http3EventKind::Aborted);
        REQUIRE(got != nullptr);
        CHECK(got->event.error == kH3RequestRejected);
    }
    SECTION("the header")
    {
        Fixture f;
        f.server();
        const u64 id = f.rig->open(kH3Client, true);
        f.rig->write(kH3Client, id, payload({kWtBidi, 0U}));
        f.rig->write(kH3Client, id, text_span("x"));
        deliver_early(*f.rig);
        f.rig->settle();
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtStream) == 0U);
        CHECK(f.rig->streams[kH3Client].peek(id).reset);
        CHECK(f.rig->streams[kH3Client].peek(id).reset_error == kWtBufferedStreamRejected);
    }
}

// ---- What each side requires ----

TEST_CASE("webtransport: a client opens a session only on what section 3.1 lists", "[http3][webtransport]")
{
    const auto opens = [](std::initializer_list<u64> pairs, const QuicStreamLimits* server_limits,
                          bool datagram_parameter, bool legacy)
    {
        Fixture f;
        f.make(true, false, h3_wt_config(QuicSide::Client), h3_wt_config(QuicSide::Server), h3_limits(), server_limits);
        if (!datagram_parameter)
        {
            f.rig->datagrams[kH3Client].set_peer(0U, kH3FrameRoom);
        }
        u64 stream = 0U;
        const H3Fields& fields = legacy ? wt_legacy_connect() : h3_wt_connect();
        REQUIRE(f.rig->h3[kH3Client].open_request(fields.span(), false, stream) == Http3Error::NotReady); // no SETTINGS
        f.hand_settings(kH3Server, settings(pairs));
        f.rig->settle();
        REQUIRE(f.rig->h3[kH3Client].error() == 0U);
        return f.rig->h3[kH3Client].open_request(fields.span(), false, stream);
    };
    QuicStreamLimits no_reset = h3_limits();
    no_reset.reset_stream_at = false;
    const auto all = {kEnableConnect, u64{1}, kDatagram, u64{1}, kWtEnabled, u64{1}};
    CHECK(opens(all, nullptr, true, false) == Http3Error::None);
    CHECK(opens({kDatagram, 1U, kWtEnabled, 1U}, nullptr, true, false) == Http3Error::NotReady);
    CHECK(opens({kEnableConnect, 1U, kWtEnabled, 1U}, nullptr, true, false) == Http3Error::NotReady);
    CHECK(opens({kEnableConnect, 1U, kDatagram, 1U}, nullptr, true, false) == Http3Error::NotReady);
    CHECK(opens(all, &no_reset, true, false) == Http3Error::NotReady);
    CHECK(opens(all, nullptr, false, false) == Http3Error::NotReady);

    const auto legacy = {kEnableConnect, u64{1}, kDatagram, u64{1}, kWtLegacy, u64{1}};
    CHECK(opens(legacy, &no_reset, true, true) == Http3Error::None); // a plain RESET_STREAM is all it relies on
    CHECK(opens({kEnableConnect, 1U, kDatagramOld, 1U, kWtLegacy, 1U}, &no_reset, true, true) == Http3Error::None);
    CHECK(opens(all, nullptr, true, true) == Http3Error::NotReady); // not offered
    CHECK(opens(legacy, nullptr, false, true) == Http3Error::NotReady);
}

TEST_CASE("webtransport: a server treats a session from a client without the requirements as malformed",
          "[http3][webtransport]")
{
    const auto attempt = [](bool client_datagrams, bool client_reset_at, const H3Fields& fields)
    {
        Http3Config client = h3_config(QuicSide::Client); // a client that does not check, as a hostile one does not
        client.datagrams = client_datagrams;
        QuicStreamLimits client_limits = h3_limits();
        client_limits.reset_stream_at = client_reset_at;
        const QuicStreamLimits server_limits = h3_limits();
        auto f = std::make_unique<Fixture>();
        f->make(true, true, client, h3_wt_config(QuicSide::Server), client_limits, &server_limits);
        f->rig->settle();
        u64 stream = 0U;
        REQUIRE(f->rig->h3[kH3Client].open_request(fields.span(), false, stream) == Http3Error::None);
        f->rig->settle();
        return f;
    };

    SECTION("the current revision")
    {
        const auto no_datagrams = attempt(false, true, h3_wt_connect());
        CHECK(no_datagrams->rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
        const H3Seen* got = no_datagrams->rig->find(kH3Client, Http3EventKind::Aborted);
        REQUIRE(got != nullptr);
        CHECK(got->event.error == kH3MessageError);

        const auto no_reset = attempt(true, false, h3_wt_connect());
        CHECK(no_reset->rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);

        const auto fine = attempt(true, true, h3_wt_connect());
        CHECK(fine->rig->how_many(kH3Server, Http3EventKind::Headers) == 1U);
    }
    SECTION("the legacy spelling needs no reset_stream_at")
    {
        const auto no_datagrams = attempt(false, false, wt_legacy_connect());
        CHECK(no_datagrams->rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
        const auto no_reset = attempt(true, false, wt_legacy_connect());
        CHECK(no_reset->rig->how_many(kH3Server, Http3EventKind::Headers) == 1U);
    }
    SECTION("another extended CONNECT is no concern of this")
    {
        const H3Fields other{{":method", "CONNECT"},
                             {":protocol", "websocket"},
                             {":scheme", "https"},
                             {":authority", "a"},
                             {":path", "/"}};
        const auto no_datagrams = attempt(false, false, other);
        CHECK(no_datagrams->rig->how_many(kH3Server, Http3EventKind::Headers) == 1U);
    }
}

// ---- A client's SETTINGS come first ----

TEST_CASE("webtransport: a server reads nothing a client's SETTINGS could change until they arrive",
          "[http3][webtransport]")
{
    constexpr u64 client_control = 2U;

    SECTION("a CONNECT and an ordinary request wait, and both go on when they come")
    {
        Fixture f;
        f.both();
        f.rig->held_stream[kH3Client] = client_control;
        f.rig->settle();
        u64 connect = 0U;
        u64 get = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(h3_wt_connect().span(), false, connect) == Http3Error::None);
        REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), true, get) == Http3Error::None);
        f.rig->settle();
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
        CHECK_FALSE(f.rig->h3[kH3Server].settings_known());

        f.rig->held_stream[kH3Client] = ~u64{0};
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].settings_known());
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 2U);
        CHECK(f.rig->h3[kH3Server].error() == 0U);
    }
    SECTION("the streams of a session wait too")
    {
        Fixture f;
        f.make(false, true);
        const u64 uni = f.rig->open(kH3Client, false);
        f.rig->write(kH3Client, uni, payload({kWtUni, 0U}));
        const u64 bidi = f.rig->open(kH3Client, true);
        f.rig->write(kH3Client, bidi, payload({kWtBidi, 0U}));
        f.rig->settle();
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtStream) == 0U);

        f.hand_settings(kH3Client, settings({kDatagram, 1U}));
        f.rig->settle();
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtStream) == 2U);
    }
    SECTION("a client does not wait for a server's")
    {
        Fixture f;
        f.make(true, false);
        const u64 id = f.rig->open(kH3Server, true);
        f.rig->write(kH3Server, id, payload({kWtBidi, 0U}));
        f.rig->settle();
        CHECK(f.rig->how_many(kH3Client, Http3EventKind::WtStream) == 1U);
    }
}

// ---- Stream headers ----

TEST_CASE("webtransport: streams carry their session and body end to end", "[http3][webtransport]")
{
    Fixture f;
    f.both();
    const u64 session = f.session();

    SECTION("unidirectional")
    {
        u64 id = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_wt_stream(session, false, id) == Http3Error::None);
        usize taken = 0U;
        REQUIRE(f.rig->streams[kH3Client].write(id, text_span("hello"), true, taken) == QuicStreamError::None);
        f.rig->settle();
        const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::WtStream);
        REQUIRE(got != nullptr);
        CHECK(got->event.stream == id);
        CHECK(got->event.id == session);
        CHECK(got->event.header == 3U); // 0x54 takes two bytes as a varint
        CHECK(f.rig->h3[kH3Server].wt_header(id) == 3U);
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtReadable) == 1U);
        CHECK(f.drain(kH3Server, id) == "hello");
        CHECK(f.rig->streams[kH3Server].peek(id).fin);
    }
    SECTION("bidirectional, from either side")
    {
        for (const usize side : {kH3Client, kH3Server})
        {
            u64 id = 0U;
            REQUIRE(f.rig->h3[side].open_wt_stream(session, true, id) == Http3Error::None);
            usize taken = 0U;
            REQUIRE(f.rig->streams[side].write(id, text_span("ping"), false, taken) == QuicStreamError::None);
            f.rig->settle();
            const usize peer = 1U - side;
            const H3Seen* const got = f.rig->find(peer, Http3EventKind::WtStream);
            REQUIRE(got != nullptr);
            CHECK(got->event.stream == id);
            CHECK(got->event.id == session);
            CHECK(f.drain(peer, id) == "ping");
            REQUIRE(f.rig->streams[peer].write(id, text_span("pong"), false, taken) == QuicStreamError::None);
            f.rig->settle();
            CHECK(f.drain(side, id) == "pong");
            CHECK(f.rig->h3[kH3Client].error() == 0U);
            CHECK(f.rig->h3[kH3Server].error() == 0U);
            f.rig->count[peer] = 0U;
        }
    }
}

TEST_CASE("webtransport: a stream reports its first bytes once per arrival", "[http3][webtransport]")
{
    Fixture f;
    f.both();
    const u64 session = f.session();
    u64 id = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_wt_stream(session, false, id) == Http3Error::None);
    usize taken = 0U;
    REQUIRE(f.rig->streams[kH3Client].write(id, text_span("ab"), false, taken) == QuicStreamError::None);
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtReadable) == 1U);
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtReadable) == 1U); // nothing new came
    CHECK(f.drain(kH3Server, id) == "ab");
    REQUIRE(f.rig->streams[kH3Client].write(id, text_span("cd"), false, taken) == QuicStreamError::None);
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtReadable) == 2U);
    CHECK(f.drain(kH3Server, id) == "cd");
}

TEST_CASE("webtransport: a header can be split anywhere", "[http3][webtransport]")
{
    const auto split = [](u64 type, bool bidirectional)
    {
        Fixture f;
        f.server();
        const u64 id = f.rig->open(kH3Client, bidirectional);
        const H3Wire wire = payload({type, kEightByteSession});
        for (usize at = 0U; at < wire.size; ++at)
        {
            CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtStream) == 0U);
            f.rig->write(kH3Client, id, wire.span().subspan(at, 1U));
            f.rig->settle();
        }
        const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::WtStream);
        REQUIRE(got != nullptr);
        CHECK(got->event.id == kEightByteSession);
        CHECK(got->event.header == wire.size);
        CHECK(f.rig->h3[kH3Server].error() == 0U);
    };
    split(kWtUni, false);
    split(kWtBidi, true);
}

TEST_CASE("webtransport: a session ID must be a client's bidirectional stream", "[http3][webtransport]")
{
    for (const u64 session : {u64{1}, u64{2}, u64{3}, u64{5}, u64{6}, u64{7}})
    {
        for (const bool bidirectional : {false, true})
        {
            Fixture f;
            f.server();
            const u64 id = f.rig->open(kH3Client, bidirectional);
            f.rig->write(kH3Client, id, payload({bidirectional ? kWtBidi : kWtUni, session}));
            f.rig->settle();
            CHECK(f.rig->h3[kH3Server].error() == kH3IdError);
        }
    }
}

TEST_CASE("webtransport: 0x41 is a stream's very first bytes or an error", "[http3][webtransport]")
{
    SECTION("after a frame on a request stream")
    {
        Fixture f;
        f.server();
        const u64 id = f.rig->open(kH3Client, true);
        f.rig->write(kH3Client, id, get_frame());
        f.rig->write(kH3Client, id, payload({kWtBidi}));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == kH3FrameUnexpected);
    }
    SECTION("on a control stream, where it has no length to wait for")
    {
        Fixture f;
        f.make(false, true);
        f.hand_settings(kH3Client,
                        []
                        {
                            H3Wire wire = settings({kDatagram, 1U});
                            wire.varint(kWtBidi);
                            return wire;
                        }());
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == kH3FrameUnexpected);
    }
    SECTION("a connection without the offer skips it as any unknown frame")
    {
        Fixture f;
        f.make(false, true, h3_config(QuicSide::Client), h3_config(QuicSide::Server));
        f.hand_settings(kH3Client, settings({}));
        const u64 id = f.rig->open(kH3Client, true);
        H3Wire wire = payload({kWtBidi, 1U});
        wire.byte(0xAAU);
        wire.raw(get_frame());
        f.rig->write(kH3Client, id, wire);
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == 0U);
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 1U);
    }
}

TEST_CASE("webtransport: a server's bidirectional stream is legal only as one", "[http3][webtransport]")
{
    SECTION("0x41 first")
    {
        Fixture f;
        f.client();
        const u64 id = f.rig->open(kH3Server, true);
        f.rig->write(kH3Server, id, payload({kWtBidi, 0U}));
        f.rig->settle();
        CHECK(f.rig->find(kH3Client, Http3EventKind::WtStream) != nullptr);
    }
    SECTION("anything else")
    {
        Fixture f;
        f.client();
        const u64 id = f.rig->open(kH3Server, true);
        f.rig->write(kH3Server, id, get_frame());
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == kH3StreamCreationError);
    }
    SECTION("ended before its first bytes")
    {
        Fixture f;
        f.client();
        const u64 id = f.rig->open(kH3Server, true);
        f.rig->write(kH3Server, id, H3Wire{}, true);
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == 0U);
        CHECK(f.rig->how_many(kH3Client, Http3EventKind::WtStream) == 0U);
    }
    SECTION("without the offer, none")
    {
        Fixture f;
        f.make(true, false, h3_config(QuicSide::Client), h3_config(QuicSide::Server));
        const u64 id = f.rig->open(kH3Server, true);
        f.rig->write(kH3Server, id, payload({kWtBidi, 0U}));
        f.rig->settle();
        CHECK(f.rig->h3[kH3Client].error() == kH3StreamCreationError);
    }
}

// ---- Ends ----

TEST_CASE("webtransport: a stream that ends inside its header names no session", "[http3][webtransport]")
{
    const u8 partial[] = {0x40U}; // the first byte of a two-byte session ID
    SECTION("with a FIN, unidirectional")
    {
        Fixture f;
        f.server();
        const u32 before = f.rig->streams[kH3Server].open_streams();
        const u64 id = f.rig->open(kH3Client, false);
        H3Wire wire = payload({kWtUni});
        wire.byte(partial[0]);
        f.rig->write(kH3Client, id, wire, true);
        f.rig->settle();
        CHECK(f.rig->h3[kH3Server].error() == 0U);
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtStream) == 0U);
        CHECK(f.rig->streams[kH3Server].open_streams() == before);
    }
    SECTION("with a reliable size that cuts it")
    {
        for (const bool bidirectional : {false, true})
        {
            Fixture f;
            f.server();
            const u32 before = f.rig->streams[kH3Server].open_streams();
            const u64 id = f.rig->open(kH3Client, bidirectional);
            f.rig->write(kH3Client, id, payload({bidirectional ? kWtBidi : kWtUni, 64U}));
            REQUIRE(f.rig->streams[kH3Client].reset(id, 5U, 3U) == QuicStreamError::None); // of four bytes
            f.rig->settle();
            CHECK(f.rig->h3[kH3Server].error() == 0U);
            CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtStream) == 0U);
            CHECK(f.rig->how_many(kH3Server, Http3EventKind::WtAborted) == 0U);
            CHECK(f.rig->streams[kH3Server].open_streams() == before);
        }
    }
    SECTION("with a plain RESET_STREAM before anything")
    {
        for (const bool bidirectional : {false, true})
        {
            Fixture f;
            f.server();
            const u32 before = f.rig->streams[kH3Server].open_streams();
            const u64 id = f.rig->open(kH3Client, bidirectional);
            REQUIRE(f.rig->streams[kH3Client].reset(id, 5U) == QuicStreamError::None);
            f.rig->settle();
            CHECK(f.rig->h3[kH3Server].error() == 0U);
            CHECK(f.rig->streams[kH3Server].open_streams() == before);
        }
    }
    SECTION("a stream that ends right after its header still arrives")
    {
        Fixture f;
        f.server();
        const u32 before = f.rig->streams[kH3Server].open_streams();
        const u64 id = f.rig->open(kH3Client, false);
        f.rig->write(kH3Client, id, payload({kWtUni, 0U}), true);
        f.rig->settle();
        CHECK(f.rig->find(kH3Server, Http3EventKind::WtStream) != nullptr);
        const QuicStreamRead in = f.rig->streams[kH3Server].peek(id);
        CHECK(in.fin);
        CHECK(in.data.empty());
        REQUIRE(f.rig->streams[kH3Server].consume(id, 0U) == QuicStreamError::None);
        f.rig->settle();
        CHECK(f.rig->streams[kH3Server].open_streams() == before);
    }
}

TEST_CASE("webtransport: a request stream reset under a reliable size stays a request", "[http3][webtransport]")
{
    Fixture f;
    f.server();
    const u64 id = f.rig->open(kH3Client, true);
    f.rig->write(kH3Client, id, get_frame());
    REQUIRE(f.rig->streams[kH3Client].reset(id, 5U, 1U) == QuicStreamError::None); // cuts the first frame
    f.rig->settle();
    CHECK(f.rig->h3[kH3Server].error() == 0U);
    CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
}

TEST_CASE("webtransport: a reliable reset delivers the header and what the application kept", "[http3][webtransport]")
{
    Fixture f;
    f.both();
    const u64 session = f.session();
    u64 id = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_wt_stream(session, false, id) == Http3Error::None);
    usize taken = 0U;
    REQUIRE(f.rig->streams[kH3Client].write(id, text_span("abcdef"), false, taken) == QuicStreamError::None);

    SECTION("everything was still unsent: the final size stops at what was kept")
    {
        REQUIRE(f.rig->h3[kH3Client].reset_wt(id, 7U, 3U) == Http3Error::None);
        f.rig->settle();
        REQUIRE(f.rig->find(kH3Server, Http3EventKind::WtStream) != nullptr);
        CHECK(f.drain(kH3Server, id) == "abc");
        f.rig->settle();
        const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::WtAborted);
        REQUIRE(got != nullptr);
        u32 code = 0U;
        REQUIRE(wt_application_code(got->event.error, code));
        CHECK(code == 7U);
        CHECK(got->event.final_size == 6U);
        CHECK(got->event.id == session);
    }
    SECTION("all of it was sent: the final size counts every byte")
    {
        f.rig->settle();
        REQUIRE(f.rig->h3[kH3Client].reset_wt(id, 7U, 3U) == Http3Error::None);
        f.rig->settle();
        CHECK(f.drain(kH3Server, id) == "abc");
        f.rig->settle();
        const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::WtAborted);
        REQUIRE(got != nullptr);
        CHECK(got->event.final_size == 9U);
    }
    SECTION("no body kept: the session is still named")
    {
        REQUIRE(f.rig->h3[kH3Client].reset_wt(id, 0xFFFFFFFFU) == Http3Error::None);
        f.rig->settle();
        REQUIRE(f.rig->find(kH3Server, Http3EventKind::WtStream) != nullptr);
        CHECK(f.drain(kH3Server, id).empty());
        f.rig->settle();
        const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::WtAborted);
        REQUIRE(got != nullptr);
        CHECK(got->event.error == kWtApplicationErrorLast);
    }
    SECTION("the kept bytes can be lowered")
    {
        REQUIRE(f.rig->h3[kH3Client].reset_wt(id, 7U, 5U) == Http3Error::None);
        REQUIRE(f.rig->h3[kH3Client].reset_wt(id, 7U, 1U) == Http3Error::None);
        f.rig->settle();
        CHECK(f.drain(kH3Server, id) == "a");
    }
    SECTION("an offset past what was written is refused")
    {
        CHECK(f.rig->h3[kH3Client].reset_wt(id, 7U, 7U) == Http3Error::Invalid);
    }
}

TEST_CASE("webtransport: a peer without RESET_STREAM_AT gets a plain reset", "[http3][webtransport]")
{
    QuicStreamLimits no_reset = h3_limits();
    no_reset.reset_stream_at = false;
    Fixture f;
    f.make(true, false, h3_wt_config(QuicSide::Client), h3_wt_config(QuicSide::Server), h3_limits(), &no_reset);
    f.hand_settings(kH3Server, settings({kEnableConnect, 1U, kDatagram, 1U, kWtLegacy, 1U}));
    f.rig->settle();
    u64 session = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_request(wt_legacy_connect().span(), false, session) == Http3Error::None);
    u64 id = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_wt_stream(session, false, id) == Http3Error::None);
    usize taken = 0U;
    REQUIRE(f.rig->streams[kH3Client].write(id, text_span("abc"), false, taken) == QuicStreamError::None);
    f.rig->settle();
    CHECK(f.rig->h3[kH3Client].reset_wt(id, 3U, 1U) == Http3Error::Invalid);
    CHECK(f.rig->h3[kH3Client].reset_wt(id, 3U) == Http3Error::None);
    f.rig->settle();
    CHECK(f.rig->streams[kH3Server].peek(id).reset);
    CHECK(f.rig->streams[kH3Server].peek(id).reset_error == wt_http_code(3U));
}

TEST_CASE("webtransport: stopping and aborting reach the peer with their codes", "[http3][webtransport]")
{
    Fixture f;
    f.both();
    const u64 session = f.session();
    u64 id = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_wt_stream(session, true, id) == Http3Error::None);
    usize taken = 0U;
    REQUIRE(f.rig->streams[kH3Client].write(id, text_span("ab"), false, taken) == QuicStreamError::None);
    f.rig->settle();
    REQUIRE(f.rig->find(kH3Server, Http3EventKind::WtStream) != nullptr);

    SECTION("STOP_SENDING")
    {
        REQUIRE(f.rig->h3[kH3Server].stop_wt(id, 9U) == Http3Error::None);
        f.rig->settle();
        const H3Seen* const got = f.rig->find(kH3Client, Http3EventKind::WtStopped);
        REQUIRE(got != nullptr);
        CHECK(got->event.stream == id);
        CHECK(got->event.id == session);
        u32 code = 0U;
        REQUIRE(wt_application_code(got->event.error, code));
        CHECK(code == 9U);
    }
    SECTION("both directions with a protocol code")
    {
        REQUIRE(f.rig->h3[kH3Server].abort_wt(id, kWtSessionGone) == Http3Error::None);
        f.rig->settle();
        const H3Seen* const stopped = f.rig->find(kH3Client, Http3EventKind::WtStopped);
        REQUIRE(stopped != nullptr);
        CHECK(stopped->event.error == kWtSessionGone);
        CHECK(f.drain(kH3Client, id).empty());
        f.rig->settle();
        const H3Seen* const aborted = f.rig->find(kH3Client, Http3EventKind::WtAborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == kWtSessionGone);
        u32 code = 0U;
        CHECK_FALSE(wt_application_code(aborted->event.error, code));
    }
    SECTION("a reset of the direction that carries no header is plain, or keeps only the body")
    {
        REQUIRE(f.rig->h3[kH3Server].reset_wt(id, 4U) == Http3Error::None);
        f.rig->settle();
        REQUIRE(f.rig->find(kH3Client, Http3EventKind::WtAborted) != nullptr);
        CHECK(f.rig->find(kH3Client, Http3EventKind::WtAborted)->event.final_size == 0U);
    }
    SECTION("the kept body of that direction is its first bytes")
    {
        REQUIRE(f.rig->streams[kH3Server].write(id, text_span("xyz"), false, taken) == QuicStreamError::None);
        REQUIRE(f.rig->h3[kH3Server].reset_wt(id, 4U, 2U) == Http3Error::None);
        f.rig->settle();
        CHECK(f.drain(kH3Client, id) == "xy");
    }
    SECTION("a reset with no application code is told as one")
    {
        REQUIRE(f.rig->streams[kH3Server].reset(id, 0x123U) == QuicStreamError::None);
        f.rig->settle();
        const H3Seen* const got = f.rig->find(kH3Client, Http3EventKind::WtAborted);
        REQUIRE(got != nullptr);
        u32 code = 0U;
        CHECK_FALSE(wt_application_code(got->event.error, code));
    }
}

TEST_CASE("webtransport: a stream past its window says when it has room", "[http3][webtransport]")
{
    QuicStreamLimits limits = h3_limits();
    limits.send_bytes = 8192U;
    limits.receive_bytes = 8192U;
    limits.stream_window = 8192U;
    Fixture f;
    f.make(true, true, h3_wt_config(QuicSide::Client), h3_wt_config(QuicSide::Server), limits);
    const u64 session = f.session();
    u64 id = 0U;
    REQUIRE(f.rig->h3[kH3Client].open_wt_stream(session, false, id) == Http3Error::None);
    u8 big[9000] = {};
    usize taken = 0U;
    REQUIRE(f.rig->streams[kH3Client].write(id, containers::ConstSpan<u8>(big, sizeof(big)), false, taken) ==
            QuicStreamError::None);
    CHECK(taken < sizeof(big));
    f.rig->settle();
    CHECK(f.rig->how_many(kH3Client, Http3EventKind::WtWritable) != 0U);
    CHECK(f.rig->find(kH3Client, Http3EventKind::WtWritable)->event.id == session);
}

// ---- GOAWAY ----

TEST_CASE("webtransport: GOAWAY does not touch WebTransport streams", "[http3][webtransport]")
{
    SECTION("a server's refuses requests above it, not streams of a session")
    {
        Fixture f;
        f.server();
        REQUIRE(f.rig->h3[kH3Server].goaway(true));
        f.rig->settle();
        const u64 wt = f.rig->open(kH3Client, true);
        f.rig->write(kH3Client, wt, payload({kWtBidi, 0U}));
        const u64 get = f.rig->open(kH3Client, true);
        f.rig->write(kH3Client, get, get_frame());
        f.rig->settle();
        CHECK(f.rig->find(kH3Server, Http3EventKind::WtStream) != nullptr);
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 0U);
        CHECK(f.rig->streams[kH3Client].peek(get).reset);
        CHECK(f.rig->streams[kH3Client].peek(get).reset_error == kH3RequestRejected);
        CHECK_FALSE(f.rig->streams[kH3Client].peek(wt).reset);
    }
    SECTION("a client told GOAWAY cannot open a session")
    {
        Fixture f;
        f.make(true, false);
        const u64 control = f.rig->open_typed(kH3Server, kUniControl);
        f.rig->write(kH3Server, control, settings({kEnableConnect, 1U, kDatagram, 1U, kWtEnabled, 1U}));
        H3Wire wire;
        wire.frame(kGoAway, payload({0U}));
        f.rig->write(kH3Server, control, wire);
        f.rig->settle();
        u64 stream = 0U;
        CHECK(f.rig->h3[kH3Client].open_request(h3_wt_connect().span(), false, stream) == Http3Error::GoingAway);
    }
    SECTION("a client's session above the GOAWAY is cancelled; its data stream is not an HTTP request")
    {
        Fixture f;
        f.make(true, false);
        const u64 control = f.rig->open_typed(kH3Server, kUniControl);
        f.rig->write(kH3Server, control, settings({kEnableConnect, 1U, kDatagram, 1U, kWtEnabled, 1U}));
        f.rig->settle();
        u64 session = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(h3_wt_connect().span(), false, session) == Http3Error::None);
        u64 id = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_wt_stream(session, true, id) == Http3Error::None);
        f.rig->settle();
        H3Wire wire;
        wire.frame(kGoAway, payload({0U}));
        f.rig->write(kH3Server, control, wire);
        f.rig->settle();
        const H3Seen* const got = f.rig->find(kH3Client, Http3EventKind::Aborted);
        REQUIRE(got != nullptr);
        CHECK(got->event.stream == session);
        CHECK(f.rig->how_many(kH3Client, Http3EventKind::Aborted) == 1U);
        CHECK(f.rig->h3[kH3Client].wt_header(id) != 0U);
    }
}

// ---- Datagrams ----

TEST_CASE("webtransport: a datagram ahead of its session is handed up", "[http3][webtransport]")
{
    const auto datagram = [](H3Rig& rig, usize to, std::initializer_list<u8> bytes)
    {
        QuicFrame frame;
        frame.kind = QuicFrameKind::Datagram;
        frame.data = containers::ConstSpan<u8>(bytes.begin(), bytes.size());
        REQUIRE(rig.datagrams[to].on_frame(frame) == QuicTransportError::NoError);
    };
    SECTION("no stream yet")
    {
        Fixture f;
        f.server();
        datagram(*f.rig, kH3Server, {0x01U, 'x'}); // quarter 1: stream 4
        f.rig->settle();
        const H3Seen* const got = f.rig->find(kH3Server, Http3EventKind::Datagram);
        REQUIRE(got != nullptr);
        CHECK(got->event.stream == 4U);
        CHECK(got->payload == "x");
    }
    SECTION("the stream open, its first bytes not read")
    {
        Fixture f;
        f.server();
        const u64 id = f.rig->open(kH3Client, true);
        f.rig->write(kH3Client, id, H3Wire{}, false);
        datagram(*f.rig, kH3Server, {0x00U, 'y'});
        f.rig->settle();
        REQUIRE(f.rig->find(kH3Server, Http3EventKind::Datagram) != nullptr);
        CHECK(f.rig->h3[kH3Server].error() == 0U);
    }
    SECTION("without the offer it goes, and does not end the request it may belong to")
    {
        Fixture f;
        f.make(false, true, h3_config(QuicSide::Client), h3_config(QuicSide::Server));
        f.hand_settings(kH3Client, settings({}));
        f.rig->settle();
        const u64 id = f.rig->open(kH3Client, true);
        H3Wire first;
        first.byte(0x21U).byte(0U); // a reserved frame, so the stream is open with its request unread
        f.rig->write(kH3Client, id, first);
        f.rig->settle();
        datagram(*f.rig, kH3Server, {0x00U, 'z'});
        f.rig->settle();
        CHECK(f.rig->find(kH3Server, Http3EventKind::Datagram) == nullptr);
        CHECK(f.rig->find(kH3Server, Http3EventKind::Aborted) == nullptr);
        f.rig->write(kH3Client, id, get_frame());
        f.rig->settle();
        CHECK(f.rig->how_many(kH3Server, Http3EventKind::Headers) == 1U);
    }
}

// ---- Slots ----

TEST_CASE("webtransport: streams give their slots back", "[http3][webtransport]")
{
    SECTION("sessions' streams, far more than the slots")
    {
        Fixture f;
        f.both();
        const u64 session = f.session();
        const u32 before = f.rig->streams[kH3Server].open_streams();
        for (u32 n = 0U; n < 64U; ++n)
        {
            u64 id = 0U;
            REQUIRE(f.rig->h3[kH3Client].open_wt_stream(session, false, id) == Http3Error::None);
            usize taken = 0U;
            REQUIRE(f.rig->streams[kH3Client].write(id, text_span("x"), true, taken) == QuicStreamError::None);
            f.rig->settle();
            CHECK(f.drain(kH3Server, id) == "x");
            REQUIRE(f.rig->streams[kH3Server].consume(id, 0U) == QuicStreamError::None);
            f.rig->settle();
            f.rig->count[kH3Server] = 0U;
            f.rig->count[kH3Client] = 0U;
            REQUIRE(f.rig->streams[kH3Server].open_streams() == before);
        }
    }
    SECTION("a hostile peer cannot pin slots with streams it gives up unclassified")
    {
        Fixture f;
        f.server();
        const u32 before = f.rig->streams[kH3Server].open_streams();
        for (u32 n = 0U; n < 64U; ++n)
        {
            const u64 id = f.rig->open(kH3Client, true);
            REQUIRE(f.rig->streams[kH3Client].reset(id, 5U) == QuicStreamError::None);
            f.rig->settle();
            f.forget_events(kH3Client);
            f.rig->settle();
            f.rig->count[kH3Server] = 0U;
            REQUIRE(f.rig->h3[kH3Server].error() == 0U);
            REQUIRE(f.rig->streams[kH3Server].open_streams() == before);
        }
    }
}

// ---- Calls ----

TEST_CASE("webtransport: calls answer for what they cannot do", "[http3][webtransport]")
{
    SECTION("no offer")
    {
        Fixture f;
        f.make(true, true, h3_config(QuicSide::Client), h3_config(QuicSide::Server));
        u64 id = 0U;
        CHECK(f.rig->h3[kH3Client].open_wt_stream(0U, false, id) == Http3Error::NotReady);
        CHECK_FALSE(f.rig->h3[kH3Client].wt_ready(false));
    }
    SECTION("not a session")
    {
        Fixture f;
        f.both();
        f.rig->settle();
        u64 id = 0U;
        CHECK(f.rig->h3[kH3Client].open_wt_stream(0U, false, id) == Http3Error::Unknown);
        u64 get = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(h3_get().span(), false, get) == Http3Error::None);
        CHECK(f.rig->h3[kH3Client].open_wt_stream(get, false, id) == Http3Error::Unknown);
        CHECK(f.rig->h3[kH3Client].reset_wt(get, 1U) == Http3Error::Unknown);
        CHECK(f.rig->h3[kH3Client].stop_wt(get, 1U) == Http3Error::Unknown);
        CHECK(f.rig->h3[kH3Client].abort_wt(get, kWtSessionGone) == Http3Error::Unknown);
        CHECK(f.rig->h3[kH3Client].wt_header(get) == 0U);
    }
    SECTION("a client may open streams before the 2xx")
    {
        Fixture f;
        f.both();
        f.rig->settle();
        u64 session = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(h3_wt_connect().span(), false, session) == Http3Error::None);
        u64 id = 0U;
        CHECK(f.rig->h3[kH3Client].open_wt_stream(session, false, id) == Http3Error::None);
    }
    SECTION("a session refused closes its stream to new ones")
    {
        Fixture f;
        f.both();
        f.rig->settle();
        u64 session = 0U;
        REQUIRE(f.rig->h3[kH3Client].open_request(h3_wt_connect().span(), false, session) == Http3Error::None);
        f.rig->settle();
        REQUIRE(f.rig->h3[kH3Server].send_response(session, H3Fields{{":status", "403"}}.span(), true) ==
                Http3Error::None);
        f.rig->settle();
        u64 id = 0U;
        CHECK(f.rig->h3[kH3Client].open_wt_stream(session, false, id) == Http3Error::Unknown);
    }
    SECTION("no stream the QUIC limits allow")
    {
        Fixture f;
        f.both();
        const u64 session = f.session();
        u64 id = 0U;
        Http3Error status = Http3Error::None;
        for (u32 n = 0U; n < 16U && status == Http3Error::None; ++n)
        {
            status = f.rig->h3[kH3Client].open_wt_stream(session, false, id);
        }
        CHECK(status == Http3Error::NoStream);
    }
    SECTION("after a connection error")
    {
        Fixture f;
        f.server();
        const u64 uni = f.rig->open(kH3Client, false);
        f.rig->write(kH3Client, uni, payload({kWtUni, 1U}));
        f.rig->settle();
        REQUIRE(f.rig->h3[kH3Server].error() == kH3IdError);
        u64 id = 0U;
        CHECK(f.rig->h3[kH3Server].open_wt_stream(0U, false, id) == Http3Error::Closed);
        CHECK(f.rig->h3[kH3Server].reset_wt(uni, 1U) == Http3Error::Closed);
        CHECK(f.rig->h3[kH3Server].stop_wt(uni, 1U) == Http3Error::Closed);
        CHECK(f.rig->h3[kH3Server].abort_wt(uni, kWtSessionGone) == Http3Error::Closed);
    }
}
} // namespace
