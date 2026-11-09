// crd-net -- WebTransport sessions (4.2a; docs/design/network-foundation.md#webtransport;
// draft-ietf-webtrans-http3-17): the pool, what a server answers, protocol negotiation, capsules, streams and datagrams that arrive before their
// session, flow control at both ends and every way a session ends.

#include "http3_fixture.hpp"
#include "ingress_fixture.hpp"
#include "quic_connection_fixture.hpp"

#include <crd/containers/wire_bytes.hpp>
#include <crd/net/http_capsule.hpp>
#include <crd/net/webtransport.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::crypto::QuicSide;

constexpr u64 kClose = 0x2843U;
constexpr u64 kDrain = 0x78AEU;
constexpr u64 kMaxData = 0x190B4D3DU;
constexpr u64 kMaxStreamData = 0x190B4D3EU;
constexpr u64 kMaxStreamsBidi = 0x190B4D3FU;
constexpr u64 kMaxStreamsUni = 0x190B4D40U;
constexpr u64 kDataBlocked = 0x190B4D41U;
constexpr u64 kStreamDataBlocked = 0x190B4D42U;
constexpr u64 kStreamsBlockedBidi = 0x190B4D43U;
constexpr u64 kStreamsBlockedUni = 0x190B4D44U;
constexpr usize kMaxSeen = 256U;

containers::ConstSpan<u8> text_span(const char* text)
{
    return containers::ConstSpan<u8>(reinterpret_cast<const u8*>(text), std::strlen(text));
}

H3Wire capsule(u64 type, const H3Wire& value)
{
    H3Wire wire;
    wire.varint(type).varint(value.size).raw(value);
    return wire;
}

H3Wire number(u64 value)
{
    H3Wire wire;
    wire.varint(value);
    return wire;
}

H3Wire close_value(u32 code, const char* message)
{
    H3Wire wire;
    wire.byte(static_cast<u8>(code >> 24U)).byte(static_cast<u8>(code >> 16U));
    wire.byte(static_cast<u8>(code >> 8U)).byte(static_cast<u8>(code));
    wire.raw(text_span(message));
    return wire;
}

Http3Config limited(QuicSide side, u64 uni, u64 bidi, u64 data)
{
    Http3Config config = h3_wt_config(side);
    config.webtransport.max_streams_uni = uni;
    config.webtransport.max_streams_bidi = bidi;
    config.webtransport.max_data = data;
    return config;
}

const H3Fields& legacy_connect()
{
    static const H3Fields kFields{{":method", "CONNECT"},
                                  {":protocol", "webtransport"},
                                  {":scheme", "https"},
                                  {":authority", "a"},
                                  {":path", "/wt"}};
    return kFields;
}

H3Fields connect_with(const char* name, const char* value)
{
    H3Fields fields{{":method", "CONNECT"},
                    {":protocol", "webtransport-h3"},
                    {":scheme", "https"},
                    {":authority", "a"},
                    {":path", "/wt"}};
    fields.add(name, value);
    return fields;
}

struct Seen
{
    WtEvent event;
    Text bytes;
};

WebTransportConfig open_origins()
{
    WebTransportConfig config;
    config.any_origin = true;
    return config;
}

struct Rig
{
    CountingAllocator allocator;
    std::unique_ptr<H3Rig> base;
    std::unique_ptr<WebTransport> wt[2];
    std::unique_ptr<Seen[]> seen[2];
    usize count[2] = {};
    bool drop_new_data = false;

    // `layer[side]`: whether that side runs sessions; a side without drives its Http3Connection by hand.
    void make(bool client_layer, bool server_layer, const Http3Config& client = h3_wt_config(QuicSide::Client),
              const Http3Config& server = h3_wt_config(QuicSide::Server),
              const WebTransportConfig& config = open_origins(), const QuicStreamLimits& limits = h3_limits())
    {
        base = std::make_unique<H3Rig>(allocator, true, true, client, server, limits);
        REQUIRE(base->ok);
        const bool layer[2] = {client_layer, server_layer};
        for (usize side = 0U; side < 2U; ++side)
        {
            seen[side] = std::make_unique<Seen[]>(kMaxSeen);
            if (layer[side])
            {
                wt[side] = std::make_unique<WebTransport>();
                REQUIRE(wt[side]->init(allocator, base->h3[side], base->streams[side], config));
                base->active[side] = false;
            }
        }
        settle();
    }

    void poll(usize side)
    {
        if (wt[side] == nullptr)
        {
            base->poll(side);
            return;
        }
        WtEvent event;
        while (wt[side]->poll(event))
        {
            REQUIRE(count[side] < kMaxSeen);
            Seen& slot = seen[side][count[side]++];
            slot.event = event;
            slot.bytes = Text(reinterpret_cast<const char*>(event.bytes.data()), event.bytes.size());
        }
    }

    void settle()
    {
        for (u32 round = 0U; round < 1024U; ++round)
        {
            poll(kH3Client);
            poll(kH3Server);
            const bool a = base->deliver(kH3Client);
            const bool b = base->deliver(kH3Server);
            if (!a && !b)
            {
                break;
            }
        }
        poll(kH3Client);
        poll(kH3Server);
    }

    [[nodiscard]] const Seen* find(usize side, WtEventKind kind, usize nth = 0U) const
    {
        for (usize at = 0U; at < count[side]; ++at)
        {
            if (seen[side][at].event.kind == kind)
            {
                if (nth == 0U)
                {
                    return &seen[side][at];
                }
                --nth;
            }
        }
        return nullptr;
    }

    [[nodiscard]] usize how_many(usize side, WtEventKind kind) const
    {
        usize total = 0U;
        for (usize at = 0U; at < count[side]; ++at)
        {
            total += (seen[side][at].event.kind == kind) ? 1U : 0U;
        }
        return total;
    }

    // A session between two layered ends.
    u64 session(const H3Fields& fields = h3_wt_connect())
    {
        u64 id = 0U;
        REQUIRE(wt[kH3Client]->connect(fields.span(), id) == Http3Error::None);
        settle();
        REQUIRE(find(kH3Server, WtEventKind::SessionRequest) != nullptr);
        REQUIRE(wt[kH3Server]->accept(id) == Http3Error::None);
        settle();
        REQUIRE(find(kH3Client, WtEventKind::SessionReady) != nullptr);
        return id;
    }

    // A server under test with its client driving HTTP/3 by hand: the session accepted.
    u64 session_with_hand_client(const H3Fields& fields = h3_wt_connect())
    {
        u64 id = 0U;
        REQUIRE(base->h3[kH3Client].open_request(fields.span(), false, id) == Http3Error::None);
        settle();
        REQUIRE(find(kH3Server, WtEventKind::SessionRequest) != nullptr);
        REQUIRE(wt[kH3Server]->accept(id) == Http3Error::None);
        settle();
        return id;
    }

    // A client under test with its server driving HTTP/3 by hand: the session accepted.
    u64 session_with_hand_server()
    {
        u64 id = 0U;
        REQUIRE(wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        settle();
        REQUIRE(base->h3[kH3Server].send_response(id, h3_ok().span(), false) == Http3Error::None);
        settle();
        REQUIRE(find(kH3Client, WtEventKind::SessionReady) != nullptr);
        return id;
    }

    // Writes a capsule on a CONNECT stream driven by hand.
    void send_capsule(usize side, u64 id, const H3Wire& wire, bool fin = false)
    {
        usize taken = 0U;
        REQUIRE(base->h3[side].send_body(id, wire.span(), fin, taken) == Http3Error::None);
        REQUIRE(taken == wire.size);
        settle();
    }

    [[nodiscard]] Text read(usize side, u64 stream)
    {
        Text got;
        for (QuicStreamRead in = wt[side]->peek(stream); !in.data.empty(); in = wt[side]->peek(stream))
        {
            got += Text(reinterpret_cast<const char*>(in.data.data()), in.data.size());
            REQUIRE(wt[side]->consume(stream, in.data.size()) == Http3Error::None);
        }
        return got;
    }

    [[nodiscard]] usize write(usize side, u64 stream, usize size, bool fin = false)
    {
        static const u8 kBytes[4096] = {};
        REQUIRE(size <= sizeof(kBytes));
        usize taken = 0U;
        static_cast<void>(wt[side]->write(stream, containers::ConstSpan<u8>(kBytes, size), fin, taken));
        return taken;
    }
};
} // namespace

TEST_CASE("webtransport sessions: WT-Available-Protocols reads as a List of Strings", "[webtransport]")
{
    const auto walk = [](const char* text)
    {
        const containers::ConstSpan<u8> list = text_span(text);
        Text joined;
        usize at = 0U;
        u8 room[32];
        usize bytes = 0U;
        while (wt_next_protocol(list, at, containers::Span<u8>(room, sizeof(room)), bytes))
        {
            joined += Text(reinterpret_cast<const char*>(room), bytes) + "|";
        }
        return (at == list.size()) ? joined : Text("bad");
    };
    CHECK(walk("") == "");
    CHECK(walk("\"a\"") == "a|");
    CHECK(walk("\"a\", \"b\"  ,\t\"c\"") == "a|b|c|");
    CHECK(walk("\"q\\\"r\", \"s\\\\t\"") == "q\"r|s\\t|");
    CHECK(walk("\"a\";x;y=1;z=\"v\";w=tok;u=?1;d=-1.5;b=:QUJD:, \"b\"") == "a|b|");
    CHECK(walk("tok") == "bad");
    CHECK(walk("\"a\", tok") == "bad");
    CHECK(walk("(\"a\" \"b\")") == "bad");
    CHECK(walk("12") == "bad");
    CHECK(walk("?1") == "bad");
    CHECK(walk("\"a\",") == "bad");
    CHECK(walk("\"a\" \"b\"") == "bad");
    CHECK(walk("\"unterminated") == "bad");
    CHECK(walk("\"bad\\escape\"") == "bad");
    CHECK(walk("\"a\x01\"") == "bad");
    CHECK(walk("\"a\";UPPER=1") == "bad");
    CHECK(walk("\"a\";x=") == "bad");
    CHECK(walk("\"a\";x=;y") == "bad");
}

TEST_CASE("webtransport sessions: figure 6's exporter context, byte for byte", "[webtransport]")
{
    u8 out[kWtMaxExporterContext];
    const usize size = wt_exporter_context(0x0102030405060708ULL, text_span("ab"), text_span("xyz"),
                                           containers::Span<u8>(out, sizeof(out)));
    const u8 expected[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 2U, 'a', 'b', 3U, 'x', 'y', 'z'};
    REQUIRE(size == sizeof(expected));
    CHECK(std::memcmp(out, expected, size) == 0);
    CHECK(wt_exporter_context(4U, {}, {}, containers::Span<u8>(out, 10U)) == 10U);
    CHECK(wt_exporter_context(4U, {}, {}, containers::Span<u8>(out, 9U)) == 0U);
    const u8 long_text[kWtMaxExporterInput + 1U] = {};
    const containers::ConstSpan<u8> too_long(long_text, sizeof(long_text));
    CHECK(wt_exporter_context(4U, too_long, {}, containers::Span<u8>(out, sizeof(out))) == 0U);
    CHECK(wt_exporter_context(4U, {}, too_long, containers::Span<u8>(out, sizeof(out))) == 0U);
}

TEST_CASE("webtransport sessions: the exporter is the connection's, kept apart by session", "[webtransport]")
{
    const u8 client_cid[8] = {0xC0U, 0xC1U, 0xC2U, 0xC3U, 0xC4U, 0xC5U, 0xC6U, 0xC7U};
    const u8 initial_dcid[8] = {0xD0U, 0xD1U, 0xD2U, 0xD3U, 0xD4U, 0xD5U, 0xD6U, 0xD7U};
    const Identity identity;
    CountingAllocator allocator;
    QuicConnection client(allocator, config_for(QuicSide::Client, identity, crypto::QuicVersion::V1, 0U));
    QuicConnection server(allocator, config_for(QuicSide::Server, identity, crypto::QuicVersion::V1, 1U));
    REQUIRE(client.ready());
    REQUIRE(server.ready());
    REQUIRE(client.connect(containers::ConstSpan<u8>(client_cid, sizeof(client_cid)),
                           containers::ConstSpan<u8>(initial_dcid, sizeof(initial_dcid)), kStartNs));
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return client.confirmed() && server.confirmed(); }));

    Rig rig;
    rig.make(true, true);
    const u64 first = rig.session();
    u64 second = 0U;
    REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), second) == Http3Error::None);
    rig.settle();
    REQUIRE(rig.wt[kH3Server]->accept(second) == Http3Error::None);
    rig.settle();

    using crypto::TlsError;
    const containers::ConstSpan<u8> label = text_span("app");
    const containers::ConstSpan<u8> context = text_span("ctx");
    u8 a[32];
    u8 b[32];
    u8 c[32];
    const containers::Span<u8> a_out(a, sizeof(a));
    const containers::Span<u8> b_out(b, sizeof(b));
    const containers::Span<u8> c_out(c, sizeof(c));
    WebTransport& near = *rig.wt[kH3Client];
    REQUIRE(near.export_keying_material(client.handshake(), first, label, context, a_out) == TlsError::None);
    REQUIRE(rig.wt[kH3Server]->export_keying_material(server.handshake(), first, label, context, b_out) ==
            TlsError::None);
    CHECK(std::memcmp(a, b, sizeof(a)) == 0);

    u8 built[kWtMaxExporterContext];
    const usize size = wt_exporter_context(first, label, context, containers::Span<u8>(built, sizeof(built)));
    REQUIRE(client.handshake().export_keying_material("EXPORTER-WebTransport", containers::ConstSpan<u8>(built, size),
                                                      c_out) == TlsError::None);
    CHECK(std::memcmp(a, c, sizeof(a)) == 0);

    REQUIRE(near.export_keying_material(client.handshake(), second, label, context, b_out) == TlsError::None);
    CHECK(std::memcmp(a, b, sizeof(a)) != 0);
    REQUIRE(near.export_keying_material(client.handshake(), first, text_span("other"), context, b_out) ==
            TlsError::None);
    CHECK(std::memcmp(a, b, sizeof(a)) != 0);

    CHECK(near.export_keying_material(client.handshake(), first + 64U, label, context, a_out) == TlsError::NotReady);
    const u8 long_label[kWtMaxExporterInput + 1U] = {};
    CHECK(near.export_keying_material(client.handshake(), first,
                                      containers::ConstSpan<u8>(long_label, sizeof(long_label)), context,
                                      a_out) == TlsError::BadConfig);
}

TEST_CASE("webtransport sessions: init refuses what it cannot run", "[webtransport]")
{
    Rig rig;
    rig.make(false, false);
    WebTransport wt;
    WebTransportConfig config;
    config.sessions = 0U;
    CHECK_FALSE(wt.init(rig.allocator, rig.base->h3[0], rig.base->streams[0], config));
    config = WebTransportConfig{};
    config.sessions = kWtMaxSessions + 1U;
    CHECK_FALSE(wt.init(rig.allocator, rig.base->h3[0], rig.base->streams[0], config));
    config = WebTransportConfig{};
    config.datagram_bytes = 0U;
    CHECK_FALSE(wt.init(rig.allocator, rig.base->h3[0], rig.base->streams[0], config));

    H3Rig plain(rig.allocator, true, true);
    REQUIRE(plain.ok);
    WebTransport unoffered;
    CHECK_FALSE(unoffered.init(rig.allocator, plain.h3[0], plain.streams[0], WebTransportConfig{}));

    CountingAllocator refusing;
    H3Rig funded(refusing, true, true, h3_wt_config(QuicSide::Client), h3_wt_config(QuicSide::Server));
    REQUIRE(funded.ok);
    refusing.refuse_after(0U);
    WebTransport broke;
    CHECK_FALSE(broke.init(refusing, funded.h3[0], funded.streams[0], WebTransportConfig{}));
}

TEST_CASE("webtransport sessions: a session, its streams and datagrams, end to end", "[webtransport]")
{
    Rig rig;
    rig.make(true, true);
    const usize allocations = rig.allocator.calls();
    const u64 id = rig.session();
    const Seen* const request = rig.find(kH3Server, WtEventKind::SessionRequest);
    REQUIRE(request != nullptr);
    CHECK_FALSE(request->event.legacy);
    CHECK(rig.wt[kH3Client]->sessions_alive() == 1U);

    u64 bidi = 0U;
    REQUIRE(rig.wt[kH3Client]->open_stream(id, true, bidi) == Http3Error::None);
    usize taken = 0U;
    REQUIRE(rig.wt[kH3Client]->write(bidi, text_span("hello"), true, taken) == Http3Error::None);
    CHECK(taken == 5U);
    rig.settle();
    const Seen* const opened = rig.find(kH3Server, WtEventKind::StreamOpened);
    REQUIRE(opened != nullptr);
    CHECK(opened->event.session == id);
    CHECK(opened->event.stream == bidi);
    CHECK(rig.read(kH3Server, bidi) == "hello");

    REQUIRE(rig.wt[kH3Server]->write(bidi, text_span("world"), true, taken) == Http3Error::None);
    rig.settle();
    CHECK(rig.find(kH3Client, WtEventKind::StreamReadable) != nullptr);
    CHECK(rig.read(kH3Client, bidi) == "world");

    u64 server_uni = 0U;
    REQUIRE(rig.wt[kH3Server]->open_stream(id, false, server_uni) == Http3Error::None);
    REQUIRE(rig.wt[kH3Server]->write(server_uni, text_span("down"), true, taken) == Http3Error::None);
    rig.settle();
    CHECK(rig.read(kH3Client, server_uni) == "down");

    REQUIRE(rig.wt[kH3Server]->send_datagram(id, text_span("dg"), 1U) == Http3Error::None);
    rig.settle();
    const Seen* const datagram = rig.find(kH3Client, WtEventKind::Datagram);
    REQUIRE(datagram != nullptr);
    CHECK(datagram->bytes == "dg");
    CHECK(datagram->event.session == id);
    CHECK(rig.allocator.calls() == allocations); // nothing allocates after init
}

TEST_CASE("webtransport sessions: other requests pass through untouched", "[webtransport]")
{
    Rig rig;
    rig.make(true, true);
    u64 id = 0U;
    REQUIRE(rig.base->h3[kH3Client].open_request(h3_get().span(), true, id) == Http3Error::None);
    rig.settle();
    usize headers = 0U;
    for (usize at = 0U; at < rig.count[kH3Server]; ++at)
    {
        const WtEvent& event = rig.seen[kH3Server][at].event;
        headers += (event.kind == WtEventKind::Http && event.http.kind == Http3EventKind::Headers &&
                    event.http.stream == id)
                       ? 1U
                       : 0U;
    }
    CHECK(headers == 1U);
    CHECK(rig.wt[kH3Server]->sessions_alive() == 0U);
}

TEST_CASE("webtransport sessions: a server's answers", "[webtransport]")
{
    for (const u16 status : {u16{403}, u16{405}, u16{429}, u16{301}})
    {
        Rig rig;
        rig.make(true, true);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Server]->reject(id, 200U) == Http3Error::Invalid);
        CHECK(rig.wt[kH3Server]->reject(id + 4U, 403U) == Http3Error::Unknown);
        REQUIRE(rig.wt[kH3Server]->reject(id, status) == Http3Error::None);
        rig.settle();
        const Seen* const refused = rig.find(kH3Client, WtEventKind::SessionRefused);
        REQUIRE(refused != nullptr);
        CHECK(refused->event.status == status); // a redirect included: never followed
        CHECK(rig.wt[kH3Client]->sessions_alive() == 0U);
        CHECK(rig.wt[kH3Server]->sessions_alive() == 0U);
        CHECK(rig.find(kH3Client, WtEventKind::SessionReady) == nullptr);
        CHECK(rig.wt[kH3Server]->accept(id) == Http3Error::Unknown);
    }
}

TEST_CASE("webtransport sessions: Origin is checked before the application hears of a request", "[webtransport]")
{
    const containers::ConstSpan<u8> allowed[] = {text_span("https://good.example")};
    WebTransportConfig config;
    config.origins = containers::ConstSpan<containers::ConstSpan<u8>>(allowed, 1U);

    const auto run = [&](const H3Fields& fields, const WebTransportConfig& used)
    {
        auto rig = std::make_unique<Rig>();
        rig->make(true, true, h3_wt_config(QuicSide::Client), h3_wt_config(QuicSide::Server), used);
        u64 id = 0U;
        REQUIRE(rig->wt[kH3Client]->connect(fields.span(), id) == Http3Error::None);
        rig->settle();
        return rig;
    };
    auto good = run(connect_with("origin", "https://good.example"), config);
    CHECK(good->find(kH3Server, WtEventKind::SessionRequest) != nullptr);

    auto bad = run(connect_with("origin", "https://evil.example"), config);
    CHECK(bad->find(kH3Server, WtEventKind::SessionRequest) == nullptr);
    CHECK(bad->wt[kH3Server]->sessions_alive() == 0U);
    const Seen* const refused = bad->find(kH3Client, WtEventKind::SessionRefused);
    REQUIRE(refused != nullptr);
    CHECK(refused->event.status == 403U);

    auto native = run(connect_with("x-client", "native"), config); // no Origin: not from a browser
    CHECK(native->find(kH3Server, WtEventKind::SessionRequest) != nullptr);

    auto none_listed = run(connect_with("origin", "https://good.example"), WebTransportConfig{});
    CHECK(none_listed->find(kH3Server, WtEventKind::SessionRequest) == nullptr);

    H3Fields twice = connect_with("origin", "https://good.example");
    twice.add("origin", "https://good.example");
    auto doubled = run(twice, config);
    CHECK(doubled->find(kH3Server, WtEventKind::SessionRequest) == nullptr);

    auto anyone = run(connect_with("origin", "https://evil.example"), open_origins());
    CHECK(anyone->find(kH3Server, WtEventKind::SessionRequest) != nullptr);
}

TEST_CASE("webtransport sessions: a CONNECT that is not https is malformed", "[webtransport]")
{
    Rig rig;
    rig.make(false, true);
    const H3Fields fields{{":method", "CONNECT"},
                          {":protocol", "webtransport-h3"},
                          {":scheme", "http"},
                          {":authority", "a"},
                          {":path", "/wt"}};
    u64 id = 0U;
    REQUIRE(rig.base->h3[kH3Client].open_request(fields.span(), false, id) == Http3Error::None);
    rig.settle();
    CHECK(rig.find(kH3Server, WtEventKind::SessionRequest) == nullptr);
    const H3Seen* const aborted = rig.base->find(kH3Client, Http3EventKind::Aborted);
    REQUIRE(aborted != nullptr);
    CHECK(aborted->event.error == kH3MessageError);

    Rig client;
    client.make(true, true);
    u64 refused = 0U;
    CHECK(client.wt[kH3Client]->connect(fields.span(), refused) == Http3Error::Invalid);
    CHECK(client.wt[kH3Client]->connect(h3_get().span(), refused) == Http3Error::Invalid);
    CHECK(client.wt[kH3Server]->connect(h3_wt_connect().span(), refused) == Http3Error::Invalid);
}

TEST_CASE("webtransport sessions: how many sessions a connection takes", "[webtransport]")
{
    SECTION("the pool's limit is H3_REQUEST_REJECTED")
    {
        Rig rig;
        WebTransportConfig config = open_origins();
        config.sessions = 2U;
        rig.make(false, true, h3_wt_config(QuicSide::Client), h3_wt_config(QuicSide::Server), config);
        u64 ids[3] = {};
        for (u64& id : ids)
        {
            REQUIRE(rig.base->h3[kH3Client].open_request(h3_wt_connect().span(), false, id) == Http3Error::None);
        }
        rig.settle();
        CHECK(rig.how_many(kH3Server, WtEventKind::SessionRequest) == 2U);
        const H3Seen* const aborted = rig.base->find(kH3Client, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == kH3RequestRejected);
        CHECK(rig.base->how_many(kH3Client, Http3EventKind::Aborted) == 1U);
    }
    SECTION("without flow control, one at a time")
    {
        Rig rig;
        rig.make(false, true, limited(QuicSide::Client, 0U, 0U, 0U), h3_wt_config(QuicSide::Server));
        u64 first = 0U;
        u64 second = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_request(h3_wt_connect().span(), false, first) == Http3Error::None);
        REQUIRE(rig.base->h3[kH3Client].open_request(h3_wt_connect().span(), false, second) == Http3Error::None);
        rig.settle();
        CHECK(rig.how_many(kH3Server, WtEventKind::SessionRequest) == 1U);
        const H3Seen* const aborted = rig.base->find(kH3Client, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == kH3RequestRejected);
    }
    SECTION("a client without flow control opens one")
    {
        Rig rig;
        rig.make(true, true, limited(QuicSide::Client, 0U, 0U, 0U), h3_wt_config(QuicSide::Server));
        u64 first = 0U;
        u64 second = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), first) == Http3Error::None);
        CHECK(rig.wt[kH3Client]->connect(h3_wt_connect().span(), second) == Http3Error::NoStream);
    }
    SECTION("the legacy spelling's own count")
    {
        Rig rig;
        rig.make(true, true);
        u64 first = 0U;
        u64 second = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(legacy_connect().span(), first) == Http3Error::None);
        CHECK(rig.wt[kH3Client]->connect(legacy_connect().span(), second) == Http3Error::NoStream);
        rig.settle();
        const Seen* const request = rig.find(kH3Server, WtEventKind::SessionRequest);
        REQUIRE(request != nullptr);
        CHECK(request->event.legacy);
    }
    SECTION("the count comes back when a session goes")
    {
        Rig rig;
        rig.make(true, true);
        const u64 id = rig.session(legacy_connect());
        REQUIRE(rig.wt[kH3Client]->close(id, 0U) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Client]->sessions_alive() == 0U);
        u64 again = 0U;
        CHECK(rig.wt[kH3Client]->connect(legacy_connect().span(), again) == Http3Error::None);
    }
}

TEST_CASE("webtransport sessions: protocol negotiation", "[webtransport]")
{
    const H3Fields offered = connect_with("wt-available-protocols", R"("p1", "p\"2")");

    SECTION("a server picks from the offer, and a client reads its choice")
    {
        Rig rig;
        rig.make(true, true);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(offered.span(), id, true) == Http3Error::None);
        rig.settle();
        const Seen* const request = rig.find(kH3Server, WtEventKind::SessionRequest);
        REQUIRE(request != nullptr);
        CHECK(request->bytes == "\"p1\", \"p\\\"2\"");
        CHECK(rig.wt[kH3Server]->accept(id, {}, text_span("nope")) == Http3Error::Invalid);
        REQUIRE(rig.wt[kH3Server]->accept(id, {}, text_span("p\"2")) == Http3Error::None);
        rig.settle();
        const Seen* const ready = rig.find(kH3Client, WtEventKind::SessionReady);
        REQUIRE(ready != nullptr);
        CHECK(ready->bytes == "p\"2");
    }
    SECTION("several field lines are one list")
    {
        Rig rig;
        rig.make(false, true);
        H3Fields two = connect_with("wt-available-protocols", "\"a\"");
        two.add("wt-available-protocols", "\"b\"");
        u64 id = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_request(two.span(), false, id) == Http3Error::None);
        rig.settle();
        const Seen* const request = rig.find(kH3Server, WtEventKind::SessionRequest);
        REQUIRE(request != nullptr);
        CHECK(request->bytes == "\"a\", \"b\"");
    }
    SECTION("a malformed list counts as absent at a server, and is the client's error")
    {
        Rig rig;
        rig.make(false, true);
        const H3Fields bad = connect_with("wt-available-protocols", "token");
        u64 id = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_request(bad.span(), false, id) == Http3Error::None);
        rig.settle();
        const Seen* const request = rig.find(kH3Server, WtEventKind::SessionRequest);
        REQUIRE(request != nullptr);
        CHECK(request->bytes.empty());
        CHECK(rig.wt[kH3Server]->accept(id, {}, text_span("token")) == Http3Error::Invalid);

        Rig client;
        client.make(true, true);
        CHECK(client.wt[kH3Client]->connect(bad.span(), id) == Http3Error::Invalid);
    }

    const auto answer = [&](const char* value, bool require)
    {
        auto rig = std::make_unique<Rig>();
        rig->make(true, false);
        u64 id = 0U;
        REQUIRE(rig->wt[kH3Client]->connect(offered.span(), id, require) == Http3Error::None);
        rig->settle();
        H3Fields fields{{":status", "200"}};
        if (value != nullptr)
        {
            fields.add("wt-protocol", value);
        }
        REQUIRE(rig->base->h3[kH3Server].send_response(id, fields.span(), false) == Http3Error::None);
        rig->settle();
        return rig;
    };
    const auto refused = [](const Rig& rig)
    {
        const Seen* const closed = rig.find(kH3Client, WtEventKind::SessionClosed);
        return closed != nullptr && closed->event.error == kWtAlpnError &&
               rig.find(kH3Client, WtEventKind::SessionReady) == nullptr;
    };
    SECTION("what a client requires and is not given is WT_ALPN_ERROR")
    {
        auto none = answer(nullptr, true);
        CHECK(refused(*none));
        const H3Seen* const aborted = none->base->find(kH3Server, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == kWtAlpnError);
        CHECK(refused(*answer("token", true))); // malformed: ignored, so absent
        CHECK(refused(*answer("\"p3\"", true)));
        CHECK(refused(*answer("\"p3\"", false))); // not listed: an error required or not
        CHECK(answer("\"p1\"; a=1", true)->find(kH3Client, WtEventKind::SessionReady) != nullptr);
        CHECK(answer(nullptr, false)->find(kH3Client, WtEventKind::SessionReady) != nullptr);
        CHECK(answer("token", false)->find(kH3Client, WtEventKind::SessionReady) != nullptr);
    }
}

TEST_CASE("webtransport sessions: a choice with nothing offered is not an error unless one is required",
          "[webtransport]")
{
    Rig rig;
    rig.make(true, false);
    u64 id = 0U;
    REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
    rig.settle();
    H3Fields fields{{":status", "200"}};
    fields.add("wt-protocol", "\"p1\"");
    REQUIRE(rig.base->h3[kH3Server].send_response(id, fields.span(), false) == Http3Error::None);
    rig.settle();
    const Seen* const ready = rig.find(kH3Client, WtEventKind::SessionReady);
    REQUIRE(ready != nullptr);
    CHECK(ready->bytes.empty());
}

TEST_CASE("webtransport sessions: a stream outlives its session only as a name", "[webtransport]")
{
    Rig rig;
    rig.make(true, true, limited(QuicSide::Client, 4U, 4U, 64U), limited(QuicSide::Server, 4U, 4U, 64U));
    const u64 first = rig.session();
    u64 old = 0U;
    REQUIRE(rig.wt[kH3Client]->open_stream(first, true, old) == Http3Error::None);
    CHECK(rig.write(kH3Client, old, 40U, false) == 40U);
    rig.settle();
    REQUIRE(rig.wt[kH3Client]->close(first, 0U) == Http3Error::None);
    rig.settle();
    REQUIRE(rig.wt[kH3Client]->sessions_alive() == 0U);

    u64 second = 0U;
    REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), second) == Http3Error::None); // the same pool slot
    usize taken = 0U;
    CHECK(rig.wt[kH3Client]->write(old, text_span("x"), false, taken) == Http3Error::Unknown);
    CHECK(rig.wt[kH3Client]->reset(old, 1U) == Http3Error::Unknown);
    CHECK(rig.wt[kH3Client]->stop(old, 1U) == Http3Error::Unknown);
    CHECK(rig.wt[kH3Client]->consume(old, 0U) == Http3Error::Unknown);
    CHECK(rig.wt[kH3Client]->peek(old).data.empty());
    rig.settle();
    REQUIRE(rig.wt[kH3Server]->accept(second) == Http3Error::None);
    rig.settle();
    u64 next = 0U;
    REQUIRE(rig.wt[kH3Client]->open_stream(second, false, next) == Http3Error::None);
    CHECK(rig.write(kH3Client, next, 100U, false) == 64U); // the old session's bytes are nothing to this one
}

TEST_CASE("webtransport sessions: capsules wait for the 2xx, and are gone with a rejection", "[webtransport]")
{
    SECTION("optimistic capsules are read after accept")
    {
        Rig rig;
        rig.make(false, true);
        u64 id = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_request(h3_wt_connect().span(), false, id) == Http3Error::None);
        rig.send_capsule(kH3Client, id, capsule(kDrain, H3Wire{}));
        CHECK(rig.find(kH3Server, WtEventKind::SessionDraining) == nullptr);
        REQUIRE(rig.wt[kH3Server]->accept(id) == Http3Error::None);
        rig.settle();
        CHECK(rig.find(kH3Server, WtEventKind::SessionDraining) != nullptr);
    }
    SECTION("a rejection drops them")
    {
        Rig rig;
        rig.make(false, true);
        u64 id = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_request(h3_wt_connect().span(), false, id) == Http3Error::None);
        rig.send_capsule(kH3Client, id, capsule(kClose, close_value(9U, "x")));
        REQUIRE(rig.wt[kH3Server]->reject(id, 403U) == Http3Error::None);
        rig.settle();
        CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) == nullptr);
        CHECK(rig.base->h3[kH3Server].error() == 0U);
    }
}

TEST_CASE("webtransport sessions: capsules split anywhere, and unknown ones skipped", "[webtransport]")
{
    Rig rig;
    rig.make(false, true);
    const u64 id = rig.session_with_hand_client();
    H3Wire wire = capsule(0x7777U, close_value(1U, "ignored"));
    wire.raw(capsule(kDrain, H3Wire{}));
    wire.raw(capsule(kHttpCapsuleDatagram, close_value(0x41424344U, "!")));
    for (usize at = 0U; at < wire.size; ++at)
    {
        H3Wire one;
        one.byte(wire.bytes[at]);
        rig.send_capsule(kH3Client, id, one);
    }
    CHECK(rig.find(kH3Server, WtEventKind::SessionDraining) != nullptr);
    const Seen* const datagram = rig.find(kH3Server, WtEventKind::Datagram);
    REQUIRE(datagram != nullptr);
    CHECK(datagram->bytes == "ABCD!");
    CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) == nullptr);
}

TEST_CASE("webtransport sessions: streams and datagrams that arrive before their session", "[webtransport]")
{
    SECTION("a stream waits for its CONNECT and is reported after the accept")
    {
        Rig rig;
        rig.make(true, true);
        u64 id = 0U;
        rig.base->held_stream[kH3Client] = 0U; // the first bidirectional stream: the CONNECT
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        REQUIRE(id == 0U);
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        usize taken = 0U;
        REQUIRE(rig.wt[kH3Client]->write(stream, text_span("early"), false, taken) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Server]->streams_waiting() == 1U);
        CHECK(rig.find(kH3Server, WtEventKind::StreamOpened) == nullptr);
        rig.base->held_stream[kH3Client] = ~u64{0};
        rig.settle();
        REQUIRE(rig.find(kH3Server, WtEventKind::SessionRequest) != nullptr);
        CHECK(rig.wt[kH3Server]->streams_waiting() == 1U);
        REQUIRE(rig.wt[kH3Server]->accept(id) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Server]->streams_waiting() == 0U);
        const Seen* const opened = rig.find(kH3Server, WtEventKind::StreamOpened);
        REQUIRE(opened != nullptr);
        CHECK(opened->event.stream == stream);
        CHECK(rig.read(kH3Server, stream) == "early");
    }
    SECTION("past the bound, WT_BUFFERED_STREAM_REJECTED")
    {
        Rig rig;
        WebTransportConfig config = open_origins();
        config.waiting_streams = 1U;
        rig.make(true, true, h3_wt_config(QuicSide::Client), h3_wt_config(QuicSide::Server), config);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        rig.settle();
        u64 first = 0U;
        u64 second = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, first) == Http3Error::None);
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, second) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Server]->streams_waiting() == 1U);
        const Seen* const stopped = rig.find(kH3Client, WtEventKind::StreamStopped);
        REQUIRE(stopped != nullptr);
        CHECK(stopped->event.stream == second);
        CHECK_FALSE(stopped->event.has_code);
        CHECK(stopped->event.error == kWtBufferedStreamRejected);
    }
    SECTION("a request that turns out to be something else")
    {
        Rig rig;
        rig.make(false, true);
        u64 connect = 0U;
        rig.base->held_stream[kH3Client] = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_request(h3_get().span(), false, connect) == Http3Error::None);
        rig.settle();
        // The stream's session is held back, and turns out to be a plain GET.
        const u64 stream = rig.base->open(kH3Client, false);
        H3Wire head;
        head.varint(0x54U).varint(connect);
        rig.base->write(kH3Client, stream, head);
        rig.settle();
        CHECK(rig.wt[kH3Server]->streams_waiting() == 1U);
        rig.base->held_stream[kH3Client] = ~u64{0};
        rig.settle();
        CHECK(rig.wt[kH3Server]->streams_waiting() == 0U);
        CHECK(rig.base->streams[kH3Client].send_state(stream) != QuicSendState::Send); // reset: WT_SESSION_GONE
    }
    SECTION("a rejection ends what waited")
    {
        Rig rig;
        rig.make(true, true);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Server]->streams_waiting() == 1U);
        REQUIRE(rig.wt[kH3Server]->reject(id, 403U) == Http3Error::None);
        CHECK(rig.wt[kH3Server]->streams_waiting() == 0U);
    }
    SECTION("the peer may give a waiting stream up")
    {
        Rig rig;
        rig.make(true, true);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        rig.settle();
        REQUIRE(rig.wt[kH3Server]->streams_waiting() == 1U);
        REQUIRE(rig.wt[kH3Client]->reset(stream, 5U) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Server]->streams_waiting() == 0U);
        REQUIRE(rig.wt[kH3Server]->accept(id) == Http3Error::None);
        rig.settle();
        CHECK(rig.find(kH3Server, WtEventKind::StreamOpened) == nullptr);
    }
    SECTION("streams and datagrams wait at a client too, for its 2xx")
    {
        Rig rig;
        rig.make(true, false);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        rig.settle();
        u64 stream = 0U;
        REQUIRE(rig.base->h3[kH3Server].open_wt_stream(id, false, stream) == Http3Error::None);
        REQUIRE(rig.base->h3[kH3Server].send_datagram(id, text_span("dg"), 1U) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Client]->streams_waiting() == 1U);
        CHECK(rig.find(kH3Client, WtEventKind::Datagram) == nullptr);
        REQUIRE(rig.base->h3[kH3Server].send_response(id, h3_ok().span(), false) == Http3Error::None);
        rig.settle();
        CHECK(rig.find(kH3Client, WtEventKind::StreamOpened) != nullptr);
        const Seen* const datagram = rig.find(kH3Client, WtEventKind::Datagram);
        REQUIRE(datagram != nullptr);
        CHECK(datagram->bytes == "dg");
    }
    SECTION("a client refuses a stream of a session it does not have")
    {
        Rig rig;
        rig.make(true, false);
        u64 stream = 0U;
        const u64 id = rig.session_with_hand_server();
        REQUIRE(rig.base->h3[kH3Server].open_wt_stream(id, false, stream) == Http3Error::None);
        REQUIRE(rig.wt[kH3Client]->close(id, 0U) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Client]->streams_waiting() == 0U);
        const u64 late = rig.base->open(kH3Server, false);
        H3Wire head;
        head.varint(0x54U).varint(id);
        rig.base->write(kH3Server, late, head);
        rig.settle();
        CHECK(rig.wt[kH3Client]->streams_waiting() == 0U);
        CHECK(rig.base->streams[kH3Server].send_state(late) != QuicSendState::Send); // stopped: WT_SESSION_GONE
    }
    SECTION("datagrams wait in a few slots and the oldest goes")
    {
        Rig rig;
        WebTransportConfig config = open_origins();
        config.waiting_datagrams = 2U;
        rig.make(true, true, h3_wt_config(QuicSide::Client), h3_wt_config(QuicSide::Server), config);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        rig.settle();
        for (const char* payload : {"one", "two", "three"})
        {
            REQUIRE(rig.wt[kH3Client]->send_datagram(id, text_span(payload), 1U) == Http3Error::None);
            rig.settle();
        }
        CHECK(rig.how_many(kH3Server, WtEventKind::Datagram) == 0U);
        REQUIRE(rig.wt[kH3Server]->accept(id) == Http3Error::None);
        rig.settle();
        REQUIRE(rig.how_many(kH3Server, WtEventKind::Datagram) == 2U);
        CHECK(rig.find(kH3Server, WtEventKind::Datagram, 0U)->bytes == "two");
        CHECK(rig.find(kH3Server, WtEventKind::Datagram, 1U)->bytes == "three");
    }
    SECTION("a datagram before its CONNECT was even seen waits as well")
    {
        Rig rig;
        rig.make(true, true);
        u64 id = 0U;
        rig.base->held_stream[kH3Client] = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        REQUIRE(rig.wt[kH3Client]->send_datagram(id, text_span("first"), 1U) == Http3Error::None);
        rig.settle();
        rig.base->held_stream[kH3Client] = ~u64{0};
        rig.settle();
        REQUIRE(rig.find(kH3Server, WtEventKind::SessionRequest) != nullptr);
        CHECK(rig.how_many(kH3Server, WtEventKind::Datagram) == 0U);
        REQUIRE(rig.wt[kH3Server]->accept(id) == Http3Error::None);
        rig.settle();
        const Seen* const datagram = rig.find(kH3Server, WtEventKind::Datagram);
        REQUIRE(datagram != nullptr);
        CHECK(datagram->bytes == "first");
    }
}

TEST_CASE("webtransport sessions: stream limits", "[webtransport]")
{
    SECTION("a sender opens what it was granted, says so once, and goes on when granted more")
    {
        Rig rig;
        rig.make(true, false, h3_wt_config(QuicSide::Client), limited(QuicSide::Server, 1U, 1U, 1024U));
        const u64 id = rig.session_with_hand_server();
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        CHECK(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::NoStream);
        CHECK(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::NoStream);
        rig.settle();
        const Text once(reinterpret_cast<const char*>(capsule(kStreamsBlockedUni, number(1U)).bytes),
                        capsule(kStreamsBlockedUni, number(1U)).size);
        CHECK(rig.base->received[kH3Server] == once);

        rig.send_capsule(kH3Server, id, capsule(kMaxStreamsUni, number(2U)));
        CHECK(rig.find(kH3Client, WtEventKind::SessionWritable) != nullptr);
        CHECK(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
    }
    SECTION("a receiver grants streams as they close")
    {
        Rig rig;
        rig.make(true, true, limited(QuicSide::Client, 2U, 2U, 4096U), limited(QuicSide::Server, 2U, 2U, 4096U));
        const u64 id = rig.session();
        u64 first = 0U;
        u64 second = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, first) == Http3Error::None);
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, second) == Http3Error::None);
        u64 third = 0U;
        CHECK(rig.wt[kH3Client]->open_stream(id, false, third) == Http3Error::NoStream);
        CHECK(rig.write(kH3Client, first, 4U, true) == 4U);
        rig.settle();
        CHECK(rig.read(kH3Server, first).size() == 4U);
        rig.settle();
        CHECK(rig.find(kH3Client, WtEventKind::SessionWritable) != nullptr);
        CHECK(rig.wt[kH3Client]->open_stream(id, false, third) == Http3Error::None);
    }
    SECTION("a stream over the limit ends the session")
    {
        Rig rig;
        rig.make(false, true, h3_wt_config(QuicSide::Client), limited(QuicSide::Server, 1U, 1U, 1024U));
        const u64 id = rig.session_with_hand_client();
        u64 stream = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_wt_stream(id, false, stream) == Http3Error::None);
        REQUIRE(rig.base->h3[kH3Client].open_wt_stream(id, false, stream) == Http3Error::None);
        rig.settle();
        const Seen* const closed = rig.find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.error == kWtFlowControlError);
        const H3Seen* const aborted = rig.base->find(kH3Client, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.error == kWtFlowControlError);
    }
    SECTION("a bidirectional stream counts on its own limit, opened by a server too")
    {
        Rig rig;
        rig.make(true, false, limited(QuicSide::Client, 4U, 1U, 1024U), h3_wt_config(QuicSide::Server));
        const u64 id = rig.session_with_hand_server();
        u64 stream = 0U;
        REQUIRE(rig.base->h3[kH3Server].open_wt_stream(id, true, stream) == Http3Error::None);
        rig.settle();
        CHECK(rig.find(kH3Client, WtEventKind::SessionClosed) == nullptr);
        REQUIRE(rig.base->h3[kH3Server].open_wt_stream(id, true, stream) == Http3Error::None);
        rig.settle();
        const Seen* const closed = rig.find(kH3Client, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.error == kWtFlowControlError);
    }
}

TEST_CASE("webtransport sessions: capsules a flow controlled session refuses", "[webtransport]")
{
    const auto closes_with = [](const H3Wire& wire, u64 error)
    {
        auto rig = std::make_unique<Rig>();
        rig->make(false, true, h3_wt_config(QuicSide::Client), limited(QuicSide::Server, 2U, 2U, 1024U));
        const u64 id = rig->session_with_hand_client();
        rig->send_capsule(kH3Client, id, wire);
        const Seen* const closed = rig->find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.error == error);
    };
    // h3_wt_config grants 16 uni, 17 bidi, 2^20 data to the server's sends.
    closes_with(capsule(kMaxData, number(1U << 20U)), kWtFlowControlError); // not an increase
    closes_with(capsule(kMaxData, number(4U)), kWtFlowControlError);
    closes_with(capsule(kMaxStreamsUni, number(16U)), kWtFlowControlError);
    closes_with(capsule(kMaxStreamsBidi, number(1U)), kWtFlowControlError);
    closes_with(capsule(kMaxStreamsBidi, number((u64{1} << 60U) + 1U)), kWtFlowControlError);
    closes_with(capsule(kStreamsBlockedUni, number((u64{1} << 60U) + 1U)), kWtFlowControlError);
    closes_with(capsule(kStreamsBlockedBidi, number((u64{1} << 60U) + 1U)), kWtFlowControlError);
    closes_with(capsule(kMaxStreamData, number(4U)), kWtFlowControlError);
    closes_with(capsule(kStreamDataBlocked, number(4U)), kWtFlowControlError);
    H3Wire short_value;
    short_value.byte(0x40U);
    closes_with(capsule(kMaxData, short_value), kH3MessageError);
    H3Wire long_value = number((u64{1} << 21U));
    long_value.byte(0U);
    closes_with(capsule(kMaxData, long_value), kH3MessageError);
    closes_with(capsule(kMaxData, close_value(0U, "123456789")), kH3MessageError);
    closes_with(capsule(kDrain, number(1U)), kH3MessageError);

    SECTION("without flow control those four are ignored, the two prohibited ones not")
    {
        Rig rig;
        rig.make(false, true, limited(QuicSide::Client, 0U, 0U, 0U), h3_wt_config(QuicSide::Server));
        const u64 id = rig.session_with_hand_client();
        rig.send_capsule(kH3Client, id, capsule(kMaxData, number(1U)));
        rig.send_capsule(kH3Client, id, capsule(kMaxStreamsUni, number(1U)));
        rig.send_capsule(kH3Client, id, capsule(kMaxStreamsBidi, number(1U)));
        rig.send_capsule(kH3Client, id, capsule(kDataBlocked, number(1U)));
        rig.send_capsule(kH3Client, id, capsule(kStreamsBlockedUni, number(1U)));
        CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) == nullptr);
        rig.send_capsule(kH3Client, id, capsule(kMaxStreamData, number(1U)));
        CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) != nullptr);
    }
    SECTION("a legacy session has no limits at all")
    {
        Rig rig;
        rig.make(true, true, limited(QuicSide::Client, 1U, 1U, 8U), limited(QuicSide::Server, 1U, 1U, 8U));
        const u64 id = rig.session(legacy_connect());
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        CHECK(rig.write(kH3Client, stream, 64U, false) == 64U);
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        rig.settle();
        CHECK(rig.how_many(kH3Server, WtEventKind::StreamOpened) == 2U);
        CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) == nullptr);
    }
}

TEST_CASE("webtransport sessions: the session's data limit", "[webtransport]")
{
    const Http3Config tight_client = limited(QuicSide::Client, 4U, 4U, 64U);
    const Http3Config tight_server = limited(QuicSide::Server, 4U, 4U, 64U);

    SECTION("a sender stops at the limit, says so once, and resumes as the peer reads")
    {
        Rig rig;
        rig.make(true, true, tight_client, tight_server);
        const u64 id = rig.session();
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, true, stream) == Http3Error::None);
        CHECK(rig.write(kH3Client, stream, 100U, true) == 64U);
        usize taken = 7U;
        CHECK(rig.wt[kH3Client]->write(stream, text_span("x"), false, taken) == Http3Error::Blocked);
        CHECK(taken == 0U);
        rig.settle();
        CHECK(rig.read(kH3Server, stream).size() == 64U);
        rig.settle();
        CHECK(rig.find(kH3Client, WtEventKind::SessionWritable) != nullptr);
        CHECK(rig.write(kH3Client, stream, 36U, true) == 36U);
        rig.settle();
        CHECK(rig.read(kH3Server, stream).size() == 36U);
        CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) == nullptr);
    }
    SECTION("a FIN needs no credit")
    {
        Rig rig;
        rig.make(true, true, tight_client, tight_server);
        const u64 id = rig.session();
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        CHECK(rig.write(kH3Client, stream, 64U, false) == 64U);
        usize taken = 0U;
        CHECK(rig.wt[kH3Client]->write(stream, {}, true, taken) == Http3Error::None);
        rig.settle();
        CHECK(rig.read(kH3Server, stream).size() == 64U);
        CHECK(rig.base->streams[kH3Server].slot_of(stream) == kQuicNoStreamSlot); // the end was read too
    }
    SECTION("a reset gives back what it never sent")
    {
        Rig rig;
        rig.make(true, true, tight_client, tight_server);
        const u64 id = rig.session();
        for (u32 repeat = 0U; repeat < 3U; ++repeat)
        {
            u64 stream = 0U;
            REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
            CHECK(rig.write(kH3Client, stream, 64U, false) == 64U); // nothing has gone yet
            REQUIRE(rig.wt[kH3Client]->reset(stream, 9U) == Http3Error::None);
        }
        rig.settle();
        CHECK(rig.how_many(kH3Server, WtEventKind::StreamAborted) == 3U);
        CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) == nullptr);
        CHECK(rig.find(kH3Client, WtEventKind::SessionClosed) == nullptr);
    }
    SECTION("a reset counts its final size at the receiver, and what was never read comes back")
    {
        Rig rig;
        rig.make(true, true, tight_client, tight_server);
        const u64 id = rig.session();
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        CHECK(rig.write(kH3Client, stream, 40U, false) == 40U);
        rig.settle(); // delivered and unread: the peer holds 40 bytes of its window
        REQUIRE(rig.wt[kH3Client]->reset(stream, 9U) == Http3Error::None);
        rig.settle();
        const Seen* const aborted = rig.find(kH3Server, WtEventKind::StreamAborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.has_code);
        CHECK(aborted->event.code == 9U);
        // 40 bytes back: the limit rose to 104, and 40 stay spent.
        u64 next = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, next) == Http3Error::None);
        CHECK(rig.write(kH3Client, next, 100U, false) == 64U);
    }
    SECTION("reliable bytes keep their count, and a STOP_SENDING gives back the rest")
    {
        Rig rig;
        rig.make(true, true, tight_client, tight_server);
        const u64 id = rig.session();
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        CHECK(rig.write(kH3Client, stream, 30U, false) == 30U);
        REQUIRE(rig.wt[kH3Client]->reset(stream, 9U, 10U) == Http3Error::None);
        u64 next = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, next) == Http3Error::None);
        CHECK(rig.write(kH3Client, next, 100U, false) == 54U); // 64 less the ten that stay owed
        rig.settle();
        CHECK(rig.read(kH3Server, stream).size() == 10U);

        u64 stopped = 0U;
        REQUIRE(rig.wt[kH3Server]->open_stream(id, false, stopped) == Http3Error::None);
        CHECK(rig.write(kH3Server, stopped, 64U, false) == 64U);
        rig.settle();
        REQUIRE(rig.wt[kH3Client]->stop(stopped, 3U) == Http3Error::None);
        rig.settle();
        const Seen* const told = rig.find(kH3Server, WtEventKind::StreamStopped);
        REQUIRE(told != nullptr);
        CHECK(told->event.has_code);
        CHECK(told->event.code == 3U);
    }
    SECTION("body bytes over the limit end the session; a header does not count")
    {
        Rig rig;
        rig.make(false, true, h3_wt_config(QuicSide::Client), tight_server);
        const u64 id = rig.session_with_hand_client();
        u64 stream = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_wt_stream(id, false, stream) == Http3Error::None);
        u8 body[65] = {};
        rig.base->write(kH3Client, stream, containers::ConstSpan<u8>(body, 64U));
        rig.settle();
        CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) == nullptr);
        rig.base->write(kH3Client, stream, containers::ConstSpan<u8>(body, 1U));
        rig.settle();
        const Seen* const closed = rig.find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.error == kWtFlowControlError);
    }
    SECTION("a waiting stream's bytes count when its session comes")
    {
        Rig rig;
        rig.make(false, true, h3_wt_config(QuicSide::Client), tight_server);
        u64 id = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_request(h3_wt_connect().span(), false, id) == Http3Error::None);
        rig.settle();
        u64 stream = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_wt_stream(id, false, stream) == Http3Error::None);
        u8 body[100] = {};
        rig.base->write(kH3Client, stream, containers::ConstSpan<u8>(body, sizeof(body)));
        rig.settle();
        REQUIRE(rig.wt[kH3Server]->accept(id) == Http3Error::None);
        rig.settle();
        const Seen* const closed = rig.find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.error == kWtFlowControlError);
    }
    SECTION("a window of nothing grants nothing")
    {
        Rig rig;
        rig.make(true, true, limited(QuicSide::Client, 1U, 0U, 0U), limited(QuicSide::Server, 1U, 0U, 0U));
        const u64 id = rig.session();
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
        usize taken = 0U;
        CHECK(rig.wt[kH3Client]->write(stream, text_span("x"), false, taken) == Http3Error::Blocked);
        CHECK(rig.wt[kH3Client]->open_stream(id, true, stream) == Http3Error::NoStream);
    }
}

TEST_CASE("webtransport sessions: a close, on the wire and at each end", "[webtransport]")
{
    SECTION("the capsule goes with a FIN, and the streams with WT_SESSION_GONE")
    {
        Rig rig;
        rig.make(true, false);
        const u64 id = rig.session_with_hand_server();
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, true, stream) == Http3Error::None);
        rig.settle();
        REQUIRE(rig.wt[kH3Client]->close(id, 0x01020304U, text_span("bye")) == Http3Error::None);
        CHECK(rig.wt[kH3Client]->sessions_alive() == 0U);
        rig.settle();
        const H3Wire expected = capsule(kClose, close_value(0x01020304U, "bye"));
        CHECK(rig.base->received[kH3Server] == Text(reinterpret_cast<const char*>(expected.bytes), expected.size));
        CHECK(rig.base->find(kH3Server, Http3EventKind::Finished) != nullptr);
        CHECK(rig.find(kH3Client, WtEventKind::SessionClosed) == nullptr);

        u64 refused = 0U;
        CHECK(rig.wt[kH3Client]->open_stream(id, false, refused) == Http3Error::Unknown);
        CHECK(rig.wt[kH3Client]->send_datagram(id, text_span("x"), 1U) == Http3Error::Unknown);
        CHECK(rig.wt[kH3Client]->drain(id) == Http3Error::Unknown);
        CHECK(rig.wt[kH3Client]->close(id, 0U) == Http3Error::Unknown);
    }
    SECTION("the peer hears the code and message, its streams end and it answers with a FIN")
    {
        Rig rig;
        rig.make(true, true);
        const u64 id = rig.session();
        u64 stream = 0U;
        REQUIRE(rig.wt[kH3Client]->open_stream(id, true, stream) == Http3Error::None);
        CHECK(rig.write(kH3Client, stream, 1U, false) == 1U);
        rig.settle();
        REQUIRE(rig.wt[kH3Server]->close(id, 77U, text_span("done")) == Http3Error::None);
        rig.settle();
        const Seen* const closed = rig.find(kH3Client, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.has_code);
        CHECK(closed->event.code == 77U);
        CHECK(closed->bytes == "done");
        CHECK(rig.wt[kH3Client]->sessions_alive() == 0U);
        CHECK(rig.wt[kH3Server]->sessions_alive() == 0U);
        CHECK(rig.base->streams[kH3Client].send_state(stream) != QuicSendState::Send);
    }
    SECTION("a message is cut at a character")
    {
        Rig rig;
        rig.make(true, false);
        const u64 id = rig.session_with_hand_server();
        Text message(1023U, 'a');
        message += "\xC3\xA9tail"; // the two-byte character would end at 1025
        REQUIRE(rig.wt[kH3Client]->close(id, 1U, text_span(message.c_str())) == Http3Error::None);
        rig.settle();
        const H3Wire head = capsule(kClose, H3Wire{}); // only for its first bytes' length
        CHECK(rig.base->received[kH3Server].size() == head.size - 1U + 2U + 4U + 1023U);
    }
    SECTION("a message that is not UTF-8 is refused")
    {
        Rig rig;
        rig.make(true, false);
        const u64 id = rig.session_with_hand_server();
        CHECK(rig.wt[kH3Client]->close(id, 1U, text_span("\xFF")) == Http3Error::Invalid);
        CHECK(rig.wt[kH3Client]->sessions_alive() == 1U);
    }
    SECTION("a close before the response")
    {
        Rig rig;
        rig.make(true, true);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        REQUIRE(rig.wt[kH3Client]->close(id, 1U) == Http3Error::None);
        rig.settle();
        CHECK(rig.wt[kH3Client]->sessions_alive() == 0U);
    }
    SECTION("what has no room waits for it")
    {
        QuicStreamLimits limits = h3_limits();
        limits.send_bytes = 4096U;
        limits.stream_window = 4096U;
        limits.receive_bytes = 4096U;
        Rig rig;
        Http3Config client = h3_wt_config(QuicSide::Client);
        client.field_bytes = 1024U;
        Http3Config server = h3_wt_config(QuicSide::Server);
        server.field_bytes = 1024U;
        rig.make(true, false, client, server, open_origins(), limits);
        const u64 id = rig.session_with_hand_server();
        // The CONNECT stream's send buffer filled with capsules the peer does not read: nothing moves.
        u8 filler[4096] = {};
        const usize room = rig.base->streams[kH3Client].send_room(id, sizeof(filler));
        usize filled = 0U;
        REQUIRE(rig.base->streams[kH3Client].write(id, containers::ConstSpan<u8>(filler, room), false, filled) ==
                QuicStreamError::None);
        Text long_message(1000U, 'm');
        CHECK(rig.wt[kH3Client]->drain(id) == Http3Error::Blocked);
        REQUIRE(rig.wt[kH3Client]->close(id, 2U, text_span(long_message.c_str())) == Http3Error::None);
        CHECK(rig.wt[kH3Client]->sessions_alive() == 1U);
        rig.settle();
        CHECK(rig.wt[kH3Client]->sessions_alive() == 0U);
    }
}

TEST_CASE("webtransport sessions: a close the peer got wrong", "[webtransport]")
{
    const auto check_error = [](Rig& rig, u64 id)
    {
        const Seen* const closed = rig.find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.error == kH3MessageError);
        const H3Seen* const aborted = rig.base->find(kH3Client, Http3EventKind::Aborted);
        REQUIRE(aborted != nullptr);
        CHECK(aborted->event.stream == id);
        CHECK(aborted->event.error == kH3MessageError);
        CHECK(rig.wt[kH3Server]->sessions_alive() == 0U);
    };
    SECTION("too short")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        H3Wire value;
        value.byte(1U).byte(2U).byte(3U);
        rig.send_capsule(kH3Client, id, capsule(kClose, value));
        check_error(rig, id);
    }
    SECTION("too long")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        const Text message(1025U, 'x');
        rig.send_capsule(kH3Client, id, capsule(kClose, close_value(1U, message.c_str())));
        check_error(rig, id);
    }
    SECTION("the longest allowed passes")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        const Text message(1024U, 'x');
        rig.send_capsule(kH3Client, id, capsule(kClose, close_value(1U, message.c_str())));
        const Seen* const closed = rig.find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->bytes.size() == 1024U);
    }
    SECTION("not UTF-8")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        rig.send_capsule(kH3Client, id, capsule(kClose, close_value(1U, "\xC0\x80")));
        check_error(rig, id);
    }
    SECTION("data after the capsule")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        rig.send_capsule(kH3Client, id, capsule(kClose, close_value(1U, "")));
        REQUIRE(rig.find(kH3Server, WtEventKind::SessionClosed) != nullptr);
        REQUIRE(rig.wt[kH3Server]->sessions_alive() == 1U); // it waits for the FIN
        rig.send_capsule(kH3Client, id, number(1U));
        CHECK(rig.wt[kH3Server]->sessions_alive() == 0U);
        const H3Seen* const stopped = rig.base->find(kH3Client, Http3EventKind::Stopped);
        REQUIRE(stopped != nullptr); // this side's FIN already went: all that is left of a reset is the stop
        CHECK(stopped->event.error == kH3MessageError);
    }
    SECTION("a FIN inside a capsule")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        H3Wire partial;
        partial.varint(kDrain).varint(4U);
        rig.send_capsule(kH3Client, id, partial, true);
        check_error(rig, id);
    }
}

TEST_CASE("webtransport sessions: other ends of the CONNECT stream", "[webtransport]")
{
    SECTION("a clean FIN is code 0 and the other side answers in kind")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        REQUIRE(rig.base->h3[kH3Client].finish(id) == Http3Error::None);
        rig.settle();
        const Seen* const closed = rig.find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.has_code);
        CHECK(closed->event.code == 0U);
        CHECK(closed->bytes.empty());
        CHECK(closed->event.error == 0U);
        CHECK(rig.base->find(kH3Client, Http3EventKind::Finished) != nullptr);
        CHECK(rig.wt[kH3Server]->sessions_alive() == 0U);
    }
    SECTION("the capsule and then a FIN free the session")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        rig.send_capsule(kH3Client, id, capsule(kClose, close_value(3U, "bye")), true);
        const Seen* const closed = rig.find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.code == 3U);
        CHECK(rig.wt[kH3Server]->sessions_alive() == 0U);
    }
    SECTION("a reset of an established session")
    {
        Rig rig;
        rig.make(false, true);
        const u64 id = rig.session_with_hand_client();
        REQUIRE(rig.base->h3[kH3Client].cancel(id, 0x123U) == Http3Error::None);
        rig.settle();
        const Seen* const closed = rig.find(kH3Server, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.error == 0x123U);
        CHECK_FALSE(closed->event.has_code);
        CHECK(rig.wt[kH3Server]->sessions_alive() == 0U);
    }
    SECTION("a reset before the application answered")
    {
        Rig rig;
        rig.make(false, true);
        u64 id = 0U;
        REQUIRE(rig.base->h3[kH3Client].open_request(h3_wt_connect().span(), false, id) == Http3Error::None);
        rig.settle();
        REQUIRE(rig.base->h3[kH3Client].cancel(id, 0x123U) == Http3Error::None);
        rig.settle();
        CHECK(rig.find(kH3Server, WtEventKind::SessionClosed) != nullptr);
        CHECK(rig.wt[kH3Server]->accept(id) == Http3Error::Unknown);
    }
    SECTION("a reset before a client's response")
    {
        Rig rig;
        rig.make(true, false);
        u64 id = 0U;
        REQUIRE(rig.wt[kH3Client]->connect(h3_wt_connect().span(), id) == Http3Error::None);
        rig.settle();
        REQUIRE(rig.base->h3[kH3Server].cancel(id, kH3RequestRejected) == Http3Error::None);
        rig.settle();
        const Seen* const refused = rig.find(kH3Client, WtEventKind::SessionRefused);
        REQUIRE(refused != nullptr);
        CHECK(refused->event.error == kH3RequestRejected);
        CHECK(refused->event.status == 0U);
    }
    SECTION("a STOP_SENDING on it ends the session")
    {
        Rig rig;
        rig.make(true, false);
        const u64 id = rig.session_with_hand_server();
        REQUIRE(rig.base->streams[kH3Server].stop_sending(id, 0x55U) == QuicStreamError::None);
        rig.settle();
        const Seen* const closed = rig.find(kH3Client, WtEventKind::SessionClosed);
        REQUIRE(closed != nullptr);
        CHECK(closed->event.error == 0x55U);
    }
    SECTION("a draining session stays usable")
    {
        Rig rig;
        rig.make(true, true);
        const u64 id = rig.session();
        REQUIRE(rig.wt[kH3Server]->drain(id) == Http3Error::None);
        rig.settle();
        CHECK(rig.find(kH3Client, WtEventKind::SessionDraining) != nullptr);
        u64 stream = 0U;
        CHECK(rig.wt[kH3Client]->open_stream(id, false, stream) == Http3Error::None);
    }
    SECTION("a connection error leaves every call Closed")
    {
        Rig rig;
        rig.make(true, true);
        const u64 id = rig.session();
        const u64 control = rig.base->open_typed(kH3Client, 0U); // a second control stream
        CHECK(control != 0U);
        rig.settle();
        REQUIRE(rig.base->h3[kH3Server].error() == kH3StreamCreationError);
        u64 stream = 0U;
        usize taken = 0U;
        CHECK(rig.wt[kH3Server]->open_stream(id, false, stream) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->write(stream, {}, false, taken) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->reset(stream, 0U) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->stop(stream, 0U) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->consume(stream, 0U) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->send_datagram(id, {}, 1U) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->close(id, 0U) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->drain(id) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->accept(id) == Http3Error::Closed);
        CHECK(rig.wt[kH3Server]->reject(id, 403U) == Http3Error::Closed);
    }
}
