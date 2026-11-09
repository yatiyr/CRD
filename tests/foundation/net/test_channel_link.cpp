// crd-net -- a session's channels over every path (4.2a; docs/design/network-foundation.md#channels). Each case runs on
// each path the channels promise to look the same on.

#include "channel_link_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::crypto::QuicSide;
using namespace crd::net;
using namespace crd::net::testing;
using crd::net::test::Buffers;

constexpr usize kMaxBody = 32768U;
constexpr usize kMaxGot = 256U;

struct Got
{
    ChannelEventKind kind = ChannelEventKind::Frame;
    u32 channel = 0U;
    u32 code = 0U;
    AdmitVerdict verdict = AdmitVerdict::Admitted;
    u32 message = 0U;
    bool reassembled = false;
    Text body;
};

Text pattern(usize size, u8 seed)
{
    Text text;
    for (usize at = 0U; at < size; ++at)
    {
        text += static_cast<char>('a' + ((at + seed) % 26U));
    }
    return text;
}

ConstSpan<u8> bytes_of(const Text& text)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

// A rig, both links, and what each reported.
struct Rig
{
    explicit Rig(LinkPath path, const QuotaLimits& limits = QuotaLimits{}, const ChannelLinkConfig& config = {},
                 const QuicStreamLimits* stream_limits = nullptr, const Http3Config* client = nullptr,
                 const Http3Config* server = nullptr)
        : rig(allocator, path, limits, stream_limits, client, server)
    {
        REQUIRE(rig.ok);
        REQUIRE(rig.link_both(allocator, config));
        got[0] = std::make_unique<Got[]>(kMaxGot);
        got[1] = std::make_unique<Got[]>(kMaxGot);
    }

    Buffers allocator;
    LinkRig rig;
    std::unique_ptr<Got[]> got[2];
    usize count[2] = {};

    void settle()
    {
        const auto visit = [this](usize side, const ChannelEvent& event)
        {
            record(side, event);
        };
        rig.settle(visit);
    }

    void record(usize side, const ChannelEvent& event)
    {
        REQUIRE(count[side] < kMaxGot);
        Got& slot = got[side][count[side]++];
        slot.kind = event.kind;
        slot.channel = event.channel;
        slot.code = event.code;
        slot.verdict = event.verdict;
        if (event.kind == ChannelEventKind::Frame)
        {
            slot.message = event.frame.message->id;
            slot.reassembled = event.frame.reassembled;
            slot.body = Text(reinterpret_cast<const char*>(event.frame.body.data()), event.frame.body.size());
        }
    }

    [[nodiscard]] LinkEnd& at(usize side) { return *rig.end[side]; }
    [[nodiscard]] ChannelLink& link(usize side) { return *rig.end[side]->link; }
    [[nodiscard]] u64& now() { return rig.wire->now; }

    [[nodiscard]] usize frames(usize side, u32 channel) const
    {
        usize total = 0U;
        for (usize at_got = 0U; at_got < count[side]; ++at_got)
        {
            total +=
                (got[side][at_got].kind == ChannelEventKind::Frame && got[side][at_got].channel == channel) ? 1U : 0U;
        }
        return total;
    }

    [[nodiscard]] const Got* find(usize side, ChannelEventKind kind, usize nth = 0U) const
    {
        for (usize at_got = 0U; at_got < count[side]; ++at_got)
        {
            if (got[side][at_got].kind == kind)
            {
                if (nth == 0U)
                {
                    return &got[side][at_got];
                }
                --nth;
            }
        }
        return nullptr;
    }

    // A frame's bytes on a channel, all of them taken.
    void send(usize side, u32 channel, u32 message, const Text& body)
    {
        static u8 out[kMaxBody + 64U];
        const usize size = link_frame(message, bytes_of(body), Span<u8>(out, sizeof(out)));
        REQUIRE(size != 0U);
        usize taken = 0U;
        REQUIRE(link(side).write(channel, ConstSpan<u8>(out, size), taken) == ChannelError::None);
        REQUIRE(taken == size);
    }

    void datagram(usize side, u32 channel, u32 message, const Text& body)
    {
        static u8 out[2048U];
        const usize route = write_route(Route{channel}, Span<u8>(out, sizeof(out)));
        const usize frame =
            link_frame(message, bytes_of(body), Span<u8>(out + route, sizeof(out) - route), HeaderForm::ToEnd);
        REQUIRE(frame != 0U);
        REQUIRE(link(side).send_datagram(channel, Span<u8>(out, sizeof(out)), route + frame, now()) ==
                ChannelError::None);
    }

    // A stream of this end not written by a link, its route and bytes on it.
    [[nodiscard]] u64 hand_stream(usize side, bool bidirectional = false)
    {
        u64 id = 0U;
        REQUIRE(at(side).carrier.open(bidirectional, id) == ChannelError::None);
        return id;
    }

    void hand_write(usize side, u64 id, ConstSpan<u8> data, bool fin = false)
    {
        usize taken = 0U;
        REQUIRE(at(side).carrier.write(id, data, fin, taken) == ChannelError::None);
        REQUIRE(taken == data.size());
    }

    [[nodiscard]] u64 hand_route(usize side, u32 channel)
    {
        const u64 id = hand_stream(side);
        u8 route[kMaxRouteBytes];
        const usize size = write_route(Route{channel}, Span<u8>(route, sizeof(route)));
        hand_write(side, id, ConstSpan<u8>(route, size));
        return id;
    }

    void hand_frame(usize side, u64 id, u32 message, const Text& body, bool fin = false)
    {
        static u8 out[kMaxBody + 64U];
        const usize size = link_frame(message, bytes_of(body), Span<u8>(out, sizeof(out)));
        REQUIRE(size != 0U);
        hand_write(side, id, ConstSpan<u8>(out, size), fin);
    }

    void gauges_empty(usize side)
    {
        CHECK(at(side).quota.streams.held() == 0U);
        CHECK(at(side).quota.pending.held() == 0U);
        CHECK(at(side).quota.reassembly.held() == 0U);
    }
};

LinkPath any_path()
{
    return GENERATE(LinkPath::Quic, LinkPath::Qmux, LinkPath::WebTransport);
}

} // namespace

TEST_CASE("channels: init refuses what it cannot run", "[channels]")
{
    Buffers allocator;
    LinkRig rig(allocator, LinkPath::Quic);
    REQUIRE(rig.ok);
    LinkEnd& end = *rig.end[kH3Client];
    ChannelLinkConfig config;

    SECTION("no streams")
    {
        config.streams = 0U;
        ChannelLink link;
        CHECK_FALSE(link.init(allocator, end.admission, end.carrier, config));
    }
    SECTION("too many streams")
    {
        config.streams = kChannelLinkMaxStreams + 1U;
        ChannelLink link;
        CHECK_FALSE(link.init(allocator, end.admission, end.carrier, config));
    }
    SECTION("no timeout for pieces")
    {
        config.fragment_timeout_ns = 0U;
        ChannelLink link;
        CHECK_FALSE(link.init(allocator, end.admission, end.carrier, config));
    }
    SECTION("no carrier")
    {
        ChannelLink link;
        CHECK_FALSE(link.init(allocator, end.admission, ChannelCarrier{}, config));
    }
    SECTION("an allocator that refuses holds nothing")
    {
        const u32 live = allocator.live;
        allocator.refuse_all = true;
        ChannelLink link;
        CHECK_FALSE(link.init(allocator, end.admission, end.carrier, config));
        allocator.refuse_all = false;
        CHECK(allocator.live == live);
    }
    SECTION("once")
    {
        ChannelLink link;
        CHECK(link.init(allocator, end.admission, end.carrier, config));
        CHECK_FALSE(link.init(allocator, end.admission, end.carrier, config));
    }
}

TEST_CASE("channels: a reliable ordered channel keeps its order on one stream and copies nothing", "[channels]")
{
    const LinkPath path = any_path();
    const usize room = GENERATE(usize{1200}, usize{37});
    Rig test(path);
    test.rig.wire->max_frame_room = room;
    const u32 before = test.rig.wire->streams[kH3Client].open_streams();
    const u32 allocations = test.allocator.total;
    for (u8 at = 0U; at < 20U; ++at)
    {
        test.send(kH3Client, kLinkControl, kLinkHello, pattern(100U + at, at));
    }
    CHECK(test.rig.wire->streams[kH3Client].open_streams() == before + 1U);
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkControl) == 20U);
    for (u8 at = 0U; at < 20U; ++at)
    {
        const Got& got = test.got[kH3Server][at];
        CHECK(got.message == kLinkHello);
        CHECK(got.body == pattern(100U + at, at));
        CHECK_FALSE(got.reassembled);
    }
    CHECK(test.allocator.total == allocations); // nothing allocates after init
    CHECK(test.link(kH3Server).streams_held() == 1U);

    // And the other way, on the server's own stream.
    test.send(kH3Server, kLinkControl, kLinkHello, pattern(9U, 3U));
    test.settle();
    REQUIRE(test.frames(kH3Client, kLinkControl) == 1U);
    CHECK(test.got[kH3Client][0].body == pattern(9U, 3U));
}

TEST_CASE("channels: a reliable unordered channel has a stream for each write", "[channels]")
{
    Rig test(any_path());
    for (u8 at = 0U; at < 8U; ++at)
    {
        test.send(kH3Client, kLinkEvent, kLinkNote, pattern(40U, at));
    }
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkEvent) == 8U);
    CHECK(test.link(kH3Server).streams_held() == 0U);
    test.gauges_empty(kH3Server);
}

TEST_CASE("channels: datagrams, whole, listed and in pieces", "[channels]")
{
    Rig test(any_path());
    test.datagram(kH3Client, kLinkState, kLinkSnapshot, pattern(300U, 1U));
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkState) == 1U);
    CHECK(test.got[kH3Server][0].body == pattern(300U, 1U));

    SECTION("two frames in one datagram")
    {
        u8 out[1024];
        Span<u8> buffer(out, sizeof(out));
        Route route{kLinkState, RouteForm::Listed};
        usize used = write_route(route, buffer);
        used += link_frame(kLinkSnapshot, bytes_of(pattern(20U, 2U)), buffer.subspan(used));
        used += link_frame(kLinkSnapshot, bytes_of(pattern(21U, 3U)), buffer.subspan(used));
        REQUIRE(test.link(kH3Client).send_datagram(kLinkState, buffer, used, test.now()) == ChannelError::None);
        test.settle();
        REQUIRE(test.frames(kH3Server, kLinkState) == 3U);
        CHECK(test.got[kH3Server][1].body == pattern(20U, 2U));
        CHECK(test.got[kH3Server][2].body == pattern(21U, 3U));
    }
    SECTION("a message in pieces, whose buffer goes back")
    {
        const Text whole = pattern(1000U, 5U);
        const wire::MessageDesc* const message = wire::find_message(kLinkSchema, kLinkSnapshot);
        REQUIRE(message != nullptr);
        const FrameShape shape = frame_shape(kLinkSchema.channels[message->channel], *message);
        for (u32 index = 0U; index < 3U; ++index)
        {
            u8 out[1024];
            FrameHeader header;
            header.message = kLinkSnapshot;
            header.length = 1000U;
            header.object = 7U;
            header.chunk = 400U;
            header.sequence = 9U;
            header.index = index;
            const usize used = write_fragment(kLinkState, shape, header, bytes_of(whole), Span<u8>(out, sizeof(out)));
            REQUIRE(used != 0U);
            REQUIRE(test.link(kH3Client).send_datagram(kLinkState, Span<u8>(out, sizeof(out)), used, test.now()) ==
                    ChannelError::None);
        }
        test.settle();
        REQUIRE(test.frames(kH3Server, kLinkState) == 2U);
        const Got& got = test.got[kH3Server][1];
        CHECK(got.reassembled);
        CHECK(got.body == whole);
    }
    SECTION("a reassembly the pieces never finish gives up at its timer")
    {
        const Text whole = pattern(1000U, 5U);
        const wire::MessageDesc* const message = wire::find_message(kLinkSchema, kLinkSnapshot);
        REQUIRE(message != nullptr);
        const FrameShape shape = frame_shape(kLinkSchema.channels[message->channel], *message);
        u8 out[1024];
        FrameHeader header;
        header.message = kLinkSnapshot;
        header.length = 1000U;
        header.object = 7U;
        header.chunk = 400U;
        header.sequence = 11U;
        const usize used = write_fragment(kLinkState, shape, header, bytes_of(whole), Span<u8>(out, sizeof(out)));
        REQUIRE(test.link(kH3Client).send_datagram(kLinkState, Span<u8>(out, sizeof(out)), used, test.now()) ==
                ChannelError::None);
        test.settle();
        CHECK(test.at(kH3Server).quota.reassembly.held() == 1000U);
        const u64 timer = test.link(kH3Server).next_timer_ns();
        REQUIRE(timer != kNeverNs);
        test.now() = timer;
        test.settle();
        CHECK(test.at(kH3Server).quota.reassembly.held() == 0U);
        CHECK(test.link(kH3Server).next_timer_ns() == kNeverNs);
    }
    SECTION("a datagram on a reliable channel is refused alone")
    {
        u8 out[64];
        const usize route = write_route(Route{kLinkControl}, Span<u8>(out, sizeof(out)));
        const usize frame = link_frame(kLinkHello, bytes_of(pattern(4U, 0U)),
                                       Span<u8>(out + route, sizeof(out) - route), HeaderForm::ToEnd);
        REQUIRE(test.at(kH3Client).carrier.send_datagram(ConstSpan<u8>(out, route + frame), test.now()) ==
                ChannelError::None);
        test.settle();
        CHECK(test.link(kH3Server).counters().datagrams_refused == 1U);
        CHECK_FALSE(test.link(kH3Server).failed());
    }
    SECTION("the carrier's limit")
    {
        CHECK(test.link(kH3Client).max_datagram() != 0U);
        CHECK(test.link(kH3Client).max_datagram() <= 1200U);
        u8 huge[2048] = {};
        CHECK(test.link(kH3Client).send_datagram(kLinkState, Span<u8>(huge, sizeof(huge)), 2000U, test.now()) ==
              ChannelError::TooLarge);
    }
    SECTION("the wrong carrier for the channel")
    {
        u8 out[8] = {};
        usize taken = 0U;
        CHECK(test.link(kH3Client).write(kLinkState, ConstSpan<u8>(out, 1U), taken) == ChannelError::Invalid);
        CHECK(test.link(kH3Client).send_datagram(kLinkControl, Span<u8>(out, sizeof(out)), 1U, test.now()) ==
              ChannelError::Invalid);
        CHECK(test.link(kH3Client).write(77U, ConstSpan<u8>(out, 1U), taken) == ChannelError::Invalid);
    }
}

TEST_CASE("channels: only a path that cannot pad its packets pads in band", "[channels]")
{
    const LinkPath path = any_path();
    Rig test(path);
    const bool in_band = path == LinkPath::WebTransport;
    const Text body = pattern(50U, 2U);
    u8 frame[256];
    const usize frame_size = link_frame(kLinkEdit, bytes_of(body), Span<u8>(frame, sizeof(frame)));
    u8 route[kMaxRouteBytes];
    const usize route_size = write_route(Route{kLinkPadded}, Span<u8>(route, sizeof(route)));
    test.send(kH3Client, kLinkPadded, kLinkEdit, body);
    if (in_band)
    {
        const u64 padding = test.link(kH3Client).counters().padding_bytes;
        CHECK(padding != 0U);
        CHECK((route_size + frame_size + padding) % kLinkPaddingBlock == 0U);
    }
    else
    {
        CHECK(test.link(kH3Client).counters().padding_bytes == 0U);
    }
    const u64 before_datagram = test.link(kH3Client).counters().padding_bytes;
    test.datagram(kH3Client, kLinkMedia, kLinkVoice, pattern(7U, 1U));
    CHECK((test.link(kH3Client).counters().padding_bytes != before_datagram) == in_band);
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkPadded) == 1U);
    REQUIRE(test.frames(kH3Server, kLinkMedia) == 1U);
    CHECK(test.got[kH3Server][0].body == body);
    CHECK_FALSE(test.link(kH3Server).failed());
    if (in_band)
    {
        CHECK(test.at(kH3Server).admission.count(AdmitVerdict::PaddingFrame) == 1U);
    }
}

TEST_CASE("channels: padding that does not fit whole waits for the next write", "[channels]")
{
    const Text first = pattern(50U, 2U);
    const Text second = pattern(20U, 3U);
    u8 frame[256];
    const usize first_size = link_frame(kLinkEdit, bytes_of(first), Span<u8>(frame, sizeof(frame)));
    u8 route[kMaxRouteBytes];
    const usize route_size = write_route(Route{kLinkPadded}, Span<u8>(route, sizeof(route)));

    // Room for the route, the frame and one more byte: the padding would be cut.
    Http3Config client = h3_wt_config(QuicSide::Client);
    Http3Config server = h3_wt_config(QuicSide::Server);
    server.webtransport.max_data = route_size + first_size + 1U;
    Rig test(LinkPath::WebTransport, QuotaLimits{}, ChannelLinkConfig{}, nullptr, &client, &server);
    test.send(kH3Client, kLinkPadded, kLinkEdit, first);
    CHECK(test.link(kH3Client).counters().padding_bytes == 0U);
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkPadded) == 1U);

    // The window came back with the read; the stream is still correct.
    test.send(kH3Client, kLinkPadded, kLinkEdit, second);
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkPadded) == 2U);
    CHECK(test.got[kH3Server][1].body == second);
    CHECK_FALSE(test.link(kH3Server).failed());
}

TEST_CASE("channels: a limit holds the reader, so flow control holds the sender", "[channels]")
{
    const LinkPath path = any_path();
    QuotaLimits limits;
    limits.tolerance_ns = 1U; // no burst: exactly one message a second
    Rig test(path, limits);
    test.send(kH3Client, kLinkSlow, kLinkCrawl, pattern(40U, 1U));
    test.send(kH3Client, kLinkSlow, kLinkCrawl, pattern(41U, 2U));
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkSlow) == 1U); // one message a second
    const u64 timer = test.link(kH3Server).next_timer_ns();
    REQUIRE(timer != kNeverNs);
    CHECK(timer > test.now());
    CHECK(test.at(kH3Server).admission.count(AdmitVerdict::ChannelMessages) != 0U);
    CHECK(test.link(kH3Server).streams_held() == 1U);

    // Nothing of the second frame was given up.
    u64 first_stream = 0U;
    for (u64 id = 2U; id < 64U && first_stream == 0U; id += 4U)
    {
        if (!test.at(kH3Server).carrier.peek(id).data.empty())
        {
            first_stream = id;
        }
    }
    CHECK(first_stream != 0U);

    test.settle(); // a turn with no new time reads nothing more
    CHECK(test.frames(kH3Server, kLinkSlow) == 1U);
    test.now() = timer;
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkSlow) == 2U);
    CHECK(test.link(kH3Server).next_timer_ns() == kNeverNs);
}

TEST_CASE("channels: a reader held back by the frames in hand goes on the moment one is given up", "[channels]")
{
    QuotaLimits limits;
    limits.pending_messages = 1U;
    Rig test(any_path(), limits);
    const Text body = pattern(3000U, 1U);
    static u8 frame[kMaxBody + 64U];
    const usize size = link_frame(kLinkHello, bytes_of(body), Span<u8>(frame, sizeof(frame)));
    const u64 partial = test.hand_route(kH3Client, kLinkControl);
    test.hand_write(kH3Client, partial, ConstSpan<u8>(frame, size - 5U));
    test.settle();
    CHECK(test.at(kH3Server).quota.pending.held() == 1U); // the frame in pieces holds the place

    test.send(kH3Client, kLinkPadded, kLinkEdit, pattern(11U, 2U));
    test.settle();
    CHECK(test.frames(kH3Server, kLinkPadded) == 0U);
    CHECK(test.at(kH3Server).admission.count(AdmitVerdict::PendingFull) != 0U);
    CHECK(test.link(kH3Server).next_timer_ns() != kNeverNs);

    test.hand_write(kH3Client, partial, ConstSpan<u8>(frame + size - 5U, 5U));
    test.settle();
    CHECK(test.frames(kH3Server, kLinkControl) == 1U);
    CHECK(test.frames(kH3Server, kLinkPadded) == 1U);
    CHECK(test.link(kH3Server).next_timer_ns() == kNeverNs);
}

TEST_CASE("channels: a frame larger than the stream's buffer comes whole, in a buffer that goes back", "[channels]")
{
    const LinkPath path = any_path();
    Rig test(path);
    const Text body = pattern(20000U, 7U);
    static u8 frame[kMaxBody + 64U];
    const usize size = link_frame(kLinkBlob, bytes_of(body), Span<u8>(frame, sizeof(frame)));
    usize sent = 0U;
    while (sent < size)
    {
        usize taken = 0U;
        const ChannelError error =
            test.link(kH3Client).write(kLinkBulk, ConstSpan<u8>(frame + sent, size - sent), taken);
        REQUIRE((error == ChannelError::None || error == ChannelError::Blocked));
        sent += taken;
        test.settle();
    }
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkBulk) == 1U);
    const Got* const got = test.find(kH3Server, ChannelEventKind::Frame);
    REQUIRE(got != nullptr);
    CHECK(got->reassembled);
    CHECK(got->body == body);
    test.settle();
    CHECK(test.at(kH3Server).quota.reassembly.held() == 0U);
    CHECK(test.at(kH3Server).quota.pending.held() == 0U);
    CHECK(test.find(kH3Client, ChannelEventKind::Writable) != nullptr);
}

TEST_CASE("channels: every channel at once, each message larger than the buffers it crosses", "[channels]")
{
    const LinkPath path = any_path();
    QuicStreamLimits limits = link_limits(path);
    limits.send_bytes = 4096U;
    limits.receive_bytes = (path == LinkPath::WebTransport) ? 8192U : 4096U; // HTTP/3 needs a whole field section
    limits.stream_window = limits.receive_bytes;
    Rig test(path, QuotaLimits{}, ChannelLinkConfig{}, &limits);
    test.rig.wire->max_frame_room = 120U;
    struct Message
    {
        u32 channel;
        u32 message;
        usize body;
    };
    const Message messages[] = {
        {kLinkControl, kLinkHello, 388U}, {kLinkBulk, kLinkBlob, 28U},    {kLinkControl, kLinkHello, 772U},
        {kLinkPadded, kLinkEdit, 1156U},  {kLinkEvent, kLinkNote, 6124U},
    };
    usize delivered = 0U;
    for (const Message& message : messages)
    {
        const Text body = pattern(message.body, 4U);
        static u8 frame[kMaxBody + 64U];
        const usize size = link_frame(message.message, bytes_of(body), Span<u8>(frame, sizeof(frame)));
        usize sent = 0U;
        for (u32 round = 0U; round < 64U && sent < size; ++round)
        {
            usize taken = 0U;
            const ChannelError error =
                test.link(kH3Client).write(message.channel, ConstSpan<u8>(frame + sent, size - sent), taken);
            REQUIRE((error == ChannelError::None || error == ChannelError::Blocked));
            sent += taken;
            test.settle();
        }
        REQUIRE(sent == size);
        ++delivered;
        test.settle();
        CHECK(test.frames(kH3Server, message.channel) != 0U);
    }
    CHECK_FALSE(test.link(kH3Server).failed());
    usize total = 0U;
    for (const u32 channel : {kLinkControl, kLinkBulk, kLinkPadded, kLinkEvent})
    {
        total += test.frames(kH3Server, channel);
    }
    CHECK(total == delivered);
}

TEST_CASE("channels: what ends the session and what ends a stream", "[channels]")
{
    const LinkPath path = any_path();
    Rig test(path);

    SECTION("a message of another channel")
    {
        const u64 id = test.hand_route(kH3Client, kLinkControl);
        test.hand_frame(kH3Client, id, kLinkNote, pattern(4U, 0U));
        test.settle();
        const Got* const failed = test.find(kH3Server, ChannelEventKind::Failed);
        REQUIRE(failed != nullptr);
        CHECK(failed->verdict == AdmitVerdict::OtherChannel);
        CHECK(test.link(kH3Server).failed());
        usize taken = 0U;
        u8 byte = 0U;
        CHECK(test.link(kH3Server).write(kLinkControl, ConstSpan<u8>(&byte, 1U), taken) == ChannelError::Closed);
        CHECK(test.link(kH3Server).send_datagram(kLinkState, Span<u8>(&byte, 1U), 1U, test.now()) ==
              ChannelError::Closed);
        const usize seen = test.count[kH3Server];
        test.settle();
        CHECK(test.count[kH3Server] == seen); // once, and then nothing
    }
    SECTION("a route no channel has")
    {
        (void)test.hand_route(kH3Client, 99U);
        test.settle();
        const Got* const failed = test.find(kH3Server, ChannelEventKind::Failed);
        REQUIRE(failed != nullptr);
        CHECK(failed->verdict == AdmitVerdict::BadChannel);
    }
    SECTION("a stream on an unreliable channel")
    {
        (void)test.hand_route(kH3Client, kLinkState);
        test.settle();
        const Got* const failed = test.find(kH3Server, ChannelEventKind::Failed);
        REQUIRE(failed != nullptr);
        CHECK(failed->verdict == AdmitVerdict::WrongCarrier);
    }
    SECTION("a stream that ends inside a frame")
    {
        const u64 id = test.hand_route(kH3Client, kLinkControl);
        u8 frame[64];
        const usize size = link_frame(kLinkHello, bytes_of(pattern(40U, 0U)), Span<u8>(frame, sizeof(frame)));
        test.hand_write(kH3Client, id, ConstSpan<u8>(frame, size - 5U), true);
        test.settle();
        const Got* const failed = test.find(kH3Server, ChannelEventKind::Failed);
        REQUIRE(failed != nullptr);
        CHECK(failed->verdict == AdmitVerdict::Malformed);
    }
    SECTION("a stream that ends between frames does not")
    {
        const u64 id = test.hand_route(kH3Client, kLinkControl);
        test.hand_frame(kH3Client, id, kLinkHello, pattern(40U, 0U), true);
        test.settle();
        CHECK(test.frames(kH3Server, kLinkControl) == 1U);
        CHECK_FALSE(test.link(kH3Server).failed());
        CHECK(test.link(kH3Server).streams_held() == 0U);
    }
    SECTION("nor one with a FIN and nothing on it")
    {
        const u64 id = test.hand_stream(kH3Client);
        test.hand_write(kH3Client, id, ConstSpan<u8>{}, true);
        test.settle();
        CHECK_FALSE(test.link(kH3Server).failed());
        CHECK(test.link(kH3Server).streams_held() == 0U);
    }
    SECTION("a bidirectional stream is refused alone")
    {
        const u64 id = test.hand_stream(kH3Client, true);
        u8 byte = 1U;
        test.hand_write(kH3Client, id, ConstSpan<u8>(&byte, 1U));
        test.settle();
        CHECK(test.link(kH3Server).counters().streams_refused == 1U);
        CHECK_FALSE(test.link(kH3Server).failed());
        CHECK(test.link(kH3Server).streams_held() == 0U);
    }
    SECTION("the peer's reset drops a partial frame and everything it held")
    {
        const u64 id = test.hand_route(kH3Client, kLinkControl);
        u8 frame[64];
        const usize size = link_frame(kLinkHello, bytes_of(pattern(40U, 0U)), Span<u8>(frame, sizeof(frame)));
        test.hand_write(kH3Client, id, ConstSpan<u8>(frame, size - 5U));
        test.settle();
        CHECK(test.link(kH3Server).streams_held() == 1U);
        REQUIRE(test.at(kH3Client).carrier.reset(id, 9U) == ChannelError::None);
        test.settle();
        CHECK(test.link(kH3Server).streams_held() == 0U);
        CHECK(test.link(kH3Server).counters().streams_aborted == 1U);
        CHECK_FALSE(test.link(kH3Server).failed());
        test.gauges_empty(kH3Server);
    }
}

TEST_CASE("channels: a full pool refuses a stream and takes the next once one ends", "[channels]")
{
    const LinkPath path = any_path();
    ChannelLinkConfig config;
    config.streams = 2U;
    Rig test(path, QuotaLimits{}, config);
    const u64 first = test.hand_route(kH3Client, kLinkControl);
    (void)test.hand_route(kH3Client, kLinkPadded);
    test.settle();
    CHECK(test.link(kH3Server).streams_held() == 2U);
    (void)test.hand_route(kH3Client, kLinkBulk);
    test.settle();
    CHECK(test.link(kH3Server).counters().streams_refused == 1U);
    CHECK(test.link(kH3Server).streams_held() == 2U);

    test.hand_write(kH3Client, first, ConstSpan<u8>{}, true);
    test.settle();
    CHECK(test.link(kH3Server).streams_held() == 1U);
    (void)test.hand_route(kH3Client, kLinkSlow);
    test.settle();
    CHECK(test.link(kH3Server).streams_held() == 2U);
    CHECK(test.link(kH3Server).counters().streams_refused == 1U);
}

TEST_CASE("channels: the principal's stream limit ends that stream alone", "[channels]")
{
    QuotaLimits limits;
    limits.streams = 1U;
    Rig test(any_path(), limits);
    (void)test.hand_route(kH3Client, kLinkControl);
    (void)test.hand_route(kH3Client, kLinkPadded);
    test.settle();
    CHECK(test.link(kH3Server).streams_held() == 1U);
    CHECK(test.link(kH3Server).counters().streams_refused == 1U);
    CHECK_FALSE(test.link(kH3Server).failed());
}

TEST_CASE("channels: STOP_SENDING reports the channel and the next write opens another stream", "[channels]")
{
    Rig test(any_path());
    test.send(kH3Client, kLinkControl, kLinkHello, pattern(10U, 1U));
    test.settle();
    u64 id = ~u64{0};
    for (u64 candidate = 2U; candidate < 64U && id == ~u64{0}; candidate += 4U)
    {
        if (test.link(kH3Server).streams_held() != 0U &&
            test.at(kH3Server).carrier.stop(candidate, 77U) == ChannelError::None)
        {
            id = candidate;
        }
    }
    REQUIRE(id != ~u64{0});
    test.settle();
    const Got* const stopped = test.find(kH3Client, ChannelEventKind::Stopped);
    REQUIRE(stopped != nullptr);
    CHECK(stopped->channel == kLinkControl);
    CHECK(stopped->code == 77U);

    test.send(kH3Client, kLinkControl, kLinkHello, pattern(11U, 2U));
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkControl) == 2U);
    CHECK(test.got[kH3Server][1].body == pattern(11U, 2U));
}

TEST_CASE("channels: a write that met a limit hears when it can go on", "[channels]")
{
    SECTION("the stream's buffer, partly taken")
    {
        const LinkPath path = any_path();
        QuicStreamLimits limits = link_limits(path);
        limits.send_bytes = 4096U;
        Rig test(path, QuotaLimits{}, ChannelLinkConfig{}, &limits);
        static u8 frame[kMaxBody + 64U];
        const Text body = pattern(12000U, 3U);
        const usize size = link_frame(kLinkBlob, bytes_of(body), Span<u8>(frame, sizeof(frame)));
        usize sent = 0U;
        usize wakes = 0U;
        for (u32 round = 0U; round < 64U && sent < size; ++round)
        {
            usize taken = 0U;
            const ChannelError error =
                test.link(kH3Client).write(kLinkBulk, ConstSpan<u8>(frame + sent, size - sent), taken);
            REQUIRE((error == ChannelError::None || error == ChannelError::Blocked));
            sent += taken;
            const usize before = test.count[kH3Client];
            test.settle();
            wakes += (test.count[kH3Client] != before) ? 1U : 0U;
        }
        REQUIRE(sent == size);
        CHECK(wakes != 0U);
        test.settle();
        REQUIRE(test.frames(kH3Server, kLinkBulk) == 1U);
    }
    SECTION("the peer's stream limit")
    {
        const LinkPath path = GENERATE(LinkPath::Quic, LinkPath::Qmux);
        QuicStreamLimits limits = link_limits(path);
        limits.peer_uni = 1U;
        Rig test(path, QuotaLimits{}, ChannelLinkConfig{}, &limits);
        test.send(kH3Client, kLinkEvent, kLinkNote, pattern(5U, 1U));
        u8 frame[32];
        const usize size = link_frame(kLinkNote, bytes_of(pattern(5U, 2U)), Span<u8>(frame, sizeof(frame)));
        usize taken = 0U;
        CHECK(test.link(kH3Client).write(kLinkEvent, ConstSpan<u8>(frame, size), taken) == ChannelError::Blocked);
        CHECK(taken == 0U);
        test.settle();
        const Got* const writable = test.find(kH3Client, ChannelEventKind::Writable);
        REQUIRE(writable != nullptr);
        CHECK(writable->channel == kLinkEvent);
        CHECK(test.link(kH3Client).write(kLinkEvent, ConstSpan<u8>(frame, size), taken) == ChannelError::None);
        test.settle();
        CHECK(test.frames(kH3Server, kLinkEvent) == 2U);
    }
    SECTION("the session's credit on WebTransport")
    {
        Http3Config client = h3_wt_config(QuicSide::Client);
        Http3Config server = h3_wt_config(QuicSide::Server);
        client.webtransport.max_data = 64U;
        server.webtransport.max_data = 64U;
        Rig test(LinkPath::WebTransport, QuotaLimits{}, ChannelLinkConfig{}, nullptr, &client, &server);
        static u8 frame[kMaxBody + 64U];
        const Text body = pattern(300U, 3U);
        const usize size = link_frame(kLinkBlob, bytes_of(body), Span<u8>(frame, sizeof(frame)));
        usize sent = 0U;
        for (u32 round = 0U; round < 64U && sent < size; ++round)
        {
            usize taken = 0U;
            const ChannelError error =
                test.link(kH3Client).write(kLinkBulk, ConstSpan<u8>(frame + sent, size - sent), taken);
            REQUIRE((error == ChannelError::None || error == ChannelError::Blocked));
            sent += taken;
            test.settle();
        }
        REQUIRE(sent == size);
        CHECK(test.find(kH3Client, ChannelEventKind::Writable) != nullptr);
        test.settle();
        CHECK(test.frames(kH3Server, kLinkBulk) == 1U);
    }
}

TEST_CASE("channels: a channel's priority is its stream's urgency", "[channels]")
{
    Rig test(any_path());
    test.settle();
    test.send(kH3Client, kLinkBulk, kLinkBlob, pattern(100U, 1U));
    test.send(kH3Client, kLinkControl, kLinkHello, pattern(10U, 2U));
    QuicFrame frame;
    QuicStreamRecord record;
    u64 first = ~u64{0};
    u64 largest = 0U;
    QuicStreamSet& set = test.rig.wire->streams[kH3Client];
    while (set.next_frame(1200U, test.now(), kH3Rtt, frame, record))
    {
        if (frame.kind == QuicFrameKind::Stream)
        {
            first = (first == ~u64{0}) ? frame.stream_id : first;
            largest = (frame.stream_id > largest) ? frame.stream_id : largest;
            REQUIRE(test.rig.wire->streams[kH3Server].on_frame(frame) == QuicTransportError::NoError);
        }
        set.on_acked(record);
    }
    CHECK(first == largest); // the control channel's stream, opened last, went first
}

TEST_CASE("channels: bytes that may be replayed reach admission marked so", "[channels]")
{
    Rig test(LinkPath::Quic);
    QuicStreamSet& from = test.rig.wire->streams[kH3Client];
    QuicStreamSet& to = test.rig.wire->streams[kH3Server];
    const u64 id = test.hand_route(kH3Client, kLinkControl);
    test.hand_frame(kH3Client, id, kLinkReplayed, pattern(4U, 1U));
    test.hand_frame(kH3Client, id, kLinkOnce, pattern(5U, 2U));
    QuicFrame frame;
    QuicStreamRecord record;
    while (from.next_frame(1200U, test.now(), kH3Rtt, frame, record))
    {
        REQUIRE(to.on_frame(frame, true) == QuicTransportError::NoError);
        from.on_acked(record);
    }
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkControl) == 1U);
    CHECK(test.got[kH3Server][0].message == kLinkReplayed); // the idempotent one; the other is stepped over
    CHECK(test.at(kH3Server).admission.count(AdmitVerdict::Replayable) == 1U);
    CHECK_FALSE(test.link(kH3Server).failed());

    // Once the handshake is over, bytes after them are the session's own.
    test.hand_frame(kH3Client, id, kLinkOnce, pattern(6U, 3U));
    test.settle();
    REQUIRE(test.frames(kH3Server, kLinkControl) == 2U);
    CHECK(test.got[kH3Server][1].message == kLinkOnce);
}

TEST_CASE("channels: a link gives everything back when it goes, whatever is in flight", "[channels]")
{
    const LinkPath path = any_path();
    Rig test(path);
    const Text body = pattern(20000U, 1U);
    static u8 frame[kMaxBody + 64U];
    const usize size = link_frame(kLinkBlob, bytes_of(body), Span<u8>(frame, sizeof(frame)));
    const usize part = size - 100U;
    const u64 partial = test.hand_route(kH3Client, kLinkBulk);
    usize written = 0U;
    for (u32 round = 0U; round < 64U && written < part; ++round)
    {
        usize taken = 0U;
        const usize chunk = (part - written < 4096U) ? part - written : 4096U;
        REQUIRE(test.at(kH3Client).carrier.write(partial, ConstSpan<u8>(frame + written, chunk), false, taken) ==
                ChannelError::None);
        written += taken;
        test.settle();
    }
    test.settle();
    CHECK(test.at(kH3Server).quota.reassembly.held() == 20000U);
    CHECK(test.at(kH3Server).quota.pending.held() == 1U);
    test.rig.end[kH3Server]->link.reset();
    test.gauges_empty(kH3Server);
}

TEST_CASE("channels: a WebTransport session's end leaves its link nothing to hold", "[channels]")
{
    Rig test(LinkPath::WebTransport);
    test.send(kH3Client, kLinkControl, kLinkHello, pattern(10U, 1U));
    test.settle();
    CHECK(test.link(kH3Server).streams_held() == 1U);
    REQUIRE(test.rig.transport[kH3Client]->close(test.rig.session, 7U) == Http3Error::None);
    test.settle();
    CHECK(test.link(kH3Server).streams_held() == 0U);
    test.gauges_empty(kH3Server);
}
