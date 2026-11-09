// crd-fuzz-net-channel-link: a session's channels in crd-net (4.2a; docs/design/network-foundation.md#channels) under
// the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: a mode byte, then that mode's script (0 past the end).
//   0, 1, 2  a hostile peer of a link on QUIC, QMux and WebTransport: any routes and bytes on any stream, ends, resets
//   and
//            stops, bidirectional streams, any datagrams, the clock, and an application writing at any time. The pool
//            stays within its bound, a frame is what admission says, and when the link goes every quota comes back.
//   3, 4, 5  honest ends under tight buffers: frames cut anywhere and interleaved over reliable channels, whose every
//            message arrives whole, on an ordered channel in order, and no link fails.
#include "../channel_link_fixture.hpp"

#include <crd/fuzz/harness.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::test::Buffers;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u32 kStreams = 3U;
constexpr usize kTrack = 8U;

struct Input
{
    const u8* data;
    usize size;
    usize at = 0U;

    [[nodiscard]] bool more() const { return at < size; }
    u8 byte() { return (at < size) ? data[at++] : u8{0}; }
    ConstSpan<u8> bytes(usize count)
    {
        const usize rest = (count < size - at) ? count : size - at;
        const ConstSpan<u8> out(data + at, rest);
        at += rest;
        return out;
    }
};

constexpr LinkPath kPaths[] = {LinkPath::Quic, LinkPath::Qmux, LinkPath::WebTransport};
constexpr u32 kPicks[] = {kLinkHello, kLinkNote,  kLinkSnapshot, kLinkVoice,
                          kLinkEdit,  kLinkCrawl, kLinkBlob,     kLinkOnce};
constexpr u32 kChannels[] = {kLinkControl, kLinkEvent, kLinkPadded, kLinkBulk};
constexpr u32 kMessagesOf[] = {kLinkHello, kLinkNote, kLinkEdit, kLinkBlob};
constexpr usize kFrames = 12U;
constexpr usize kBodyBytes = 6144U;

[[nodiscard]] ChannelLinkConfig tight_config()
{
    ChannelLinkConfig config;
    config.streams = kStreams;
    config.fragment_slots = 2U;
    config.fragment_timeout_ns = 1000000U;
    return config;
}

[[nodiscard]] QuicStreamLimits tight_limits(LinkPath path)
{
    QuicStreamLimits limits = link_limits(path);
    limits.send_bytes = 4096U;
    limits.receive_bytes = (path == LinkPath::WebTransport) ? 8192U : 4096U; // HTTP/3 needs a whole field section
    limits.stream_window = limits.receive_bytes;
    return limits;
}

[[nodiscard]] u32 frame_message(u8 pick)
{
    return kPicks[pick % (sizeof(kPicks) / sizeof(kPicks[0]))];
}

void hostile(Input& in, u8 mode)
{
    Buffers allocator;
    LinkRig rig(allocator, kPaths[mode]);
    CRD_FUZZ_REQUIRE(rig.ok);
    CRD_FUZZ_REQUIRE(rig.link(kH3Server, allocator, tight_config()));
    ChannelLink& victim = *rig.end[kH3Server]->link;
    ChannelCarrier& peer = rig.end[kH3Client]->carrier;
    u64 streams[kTrack] = {};
    usize count = 0U;
    const auto visit = [&](usize, const ChannelEvent& event)
    {
        if (event.kind == ChannelEventKind::Frame)
        {
            CRD_FUZZ_REQUIRE(event.frame.message != nullptr);
            CRD_FUZZ_REQUIRE(event.frame.body.size() <= 100000U);
        }
    };
    const auto settle = [&]()
    {
        rig.settle(visit);
        CRD_FUZZ_REQUIRE(victim.streams_held() <= kStreams);
    };
    for (u32 ops = 0U; ops < 256U && in.more(); ++ops)
    {
        const u8 op = in.byte();
        const u64 stream = (count != 0U) ? streams[in.byte() % count] : ~u64{0};
        usize taken = 0U;
        u8 buffer[512];
        switch (op % 12U)
        {
            case 0U:
            {
                u64 id = 0U;
                if (peer.open(false, id) == ChannelError::None && count < kTrack)
                {
                    streams[count++] = id;
                    const usize route = write_route(Route{in.byte() % 10U}, Span<u8>(buffer, sizeof(buffer)));
                    static_cast<void>(peer.write(id, ConstSpan<u8>(buffer, route), false, taken));
                }
                break;
            }
            case 1U:
                static_cast<void>(peer.write(stream, in.bytes(in.byte()), false, taken));
                break;
            case 2U:
            {
                const usize size =
                    link_frame(frame_message(in.byte()), in.bytes(in.byte()), Span<u8>(buffer, sizeof(buffer)));
                static_cast<void>(peer.write(stream, ConstSpan<u8>(buffer, size), false, taken));
                break;
            }
            case 3U:
                static_cast<void>(peer.write(stream, ConstSpan<u8>{}, true, taken));
                break;
            case 4U:
                static_cast<void>(peer.reset(stream, in.byte()));
                break;
            case 5U:
            {
                u64 id = 0U;
                if (peer.open(true, id) == ChannelError::None)
                {
                    static_cast<void>(peer.write(id, in.bytes(4U), false, taken));
                }
                break;
            }
            case 6U:
                static_cast<void>(peer.send_datagram(in.bytes(in.byte()), rig.wire->now));
                break;
            case 7U:
            {
                const u32 channel = in.byte() % 4U;
                const usize route = write_route(Route{channel}, Span<u8>(buffer, sizeof(buffer)));
                const usize frame = link_frame(frame_message(in.byte()), in.bytes(in.byte()),
                                               Span<u8>(buffer + route, sizeof(buffer) - route), HeaderForm::ToEnd);
                static_cast<void>(peer.send_datagram(ConstSpan<u8>(buffer, route + frame), rig.wire->now));
                break;
            }
            case 8U:
                rig.wire->now += u64{in.byte()} * 1000000U;
                break;
            case 9U:
            {
                const usize size =
                    link_frame(frame_message(in.byte()), in.bytes(in.byte()), Span<u8>(buffer, sizeof(buffer)));
                static_cast<void>(victim.write(in.byte() % 8U, ConstSpan<u8>(buffer, size), taken));
                break;
            }
            case 10U:
            {
                const u32 channel = in.byte() % 4U;
                const usize route = write_route(Route{channel}, Span<u8>(buffer, sizeof(buffer)));
                const usize frame = link_frame(frame_message(in.byte()), in.bytes(in.byte()),
                                               Span<u8>(buffer + route, sizeof(buffer) - route), HeaderForm::ToEnd);
                static_cast<void>(
                    victim.send_datagram(channel, Span<u8>(buffer, sizeof(buffer)), route + frame, rig.wire->now));
                break;
            }
            default:
                settle();
                break;
        }
    }
    settle();
    rig.end[kH3Server]->link.reset();
    PrincipalQuota& quota = rig.end[kH3Server]->quota;
    CRD_FUZZ_REQUIRE(quota.streams.held() == 0U);
    CRD_FUZZ_REQUIRE(quota.pending.held() == 0U);
    CRD_FUZZ_REQUIRE(quota.reassembly.held() == 0U);
    CRD_FUZZ_REQUIRE(rig.end[kH3Server]->buffers.live == 0U);
}

struct Outgoing
{
    u32 channel = 0U;
    u32 message = 0U;
    u32 sequence = 0U;
    usize size = 0U; // the whole frame
    usize sent = 0U;
    usize body = 0U;
};

void honest(Input& in, u8 mode)
{
    Buffers allocator;
    const LinkPath path = kPaths[mode - 3U];
    const QuicStreamLimits limits = tight_limits(path);
    LinkRig rig(allocator, path, QuotaLimits{}, &limits);
    CRD_FUZZ_REQUIRE(rig.ok);
    CRD_FUZZ_REQUIRE(rig.link_both(allocator));
    rig.wire->max_frame_room = usize{40U} + (in.byte() % 200U);
    ChannelLink& writer = *rig.end[kH3Client]->link;
    Outgoing frames[kFrames];
    usize planned = 0U;
    while (planned < kFrames && in.more())
    {
        const u8 pick = in.byte() % 4U;
        Outgoing& frame = frames[planned++];
        frame.channel = kChannels[pick];
        frame.message = kMessagesOf[pick];
        frame.sequence = static_cast<u32>(planned);
        frame.body = 4U + (usize{in.byte()} * (kBodyBytes / 256U));
    }
    u32 seen[4] = {};
    usize delivered = 0U;
    bool failed = false;
    const auto visit = [&](usize side, const ChannelEvent& event)
    {
        failed = failed || event.kind == ChannelEventKind::Failed;
        if (side != kH3Server || event.kind != ChannelEventKind::Frame)
        {
            return;
        }
        ++delivered;
        CRD_FUZZ_REQUIRE(event.frame.body.size() >= sizeof(u32));
        u32 sequence = 0U;
        std::memcpy(&sequence, event.frame.body.data(), sizeof(sequence));
        for (u32 at = 0U; at < 4U; ++at)
        {
            if (kChannels[at] == event.channel && at != 1U) // the event channel has no order to keep
            {
                CRD_FUZZ_REQUIRE(sequence > seen[at]);
                seen[at] = sequence;
            }
        }
    };
    static u8 body[kBodyBytes + 64U];
    static u8 wire_bytes[kBodyBytes + 64U];
    // One frame at a time per channel: each continues only its own, as an ordered stream requires.
    for (u32 round = 0U; round < 2048U; ++round)
    {
        bool busy[kChannels[3] + 1U] = {};
        bool left = false;
        for (usize at = 0U; at < planned; ++at)
        {
            Outgoing& frame = frames[at];
            if (frame.sent == frame.size && frame.size != 0U)
            {
                continue;
            }
            left = true;
            if (busy[frame.channel])
            {
                continue;
            }
            busy[frame.channel] = true;
            std::memset(body, 0, frame.body);
            std::memcpy(body, &frame.sequence, sizeof(frame.sequence));
            frame.size =
                link_frame(frame.message, ConstSpan<u8>(body, frame.body), Span<u8>(wire_bytes, sizeof(wire_bytes)));
            usize taken = 0U;
            const ChannelError error =
                writer.write(frame.channel, ConstSpan<u8>(wire_bytes + frame.sent, frame.size - frame.sent), taken);
            CRD_FUZZ_REQUIRE(error == ChannelError::None || error == ChannelError::Blocked);
            frame.sent += taken;
        }
        rig.settle(visit);
        CRD_FUZZ_REQUIRE(!failed);
        if (!left)
        {
            break;
        }
    }
    rig.settle(visit);
    usize sent_whole = 0U;
    for (usize at = 0U; at < planned; ++at)
    {
        sent_whole += (frames[at].sent == frames[at].size && frames[at].size != 0U) ? 1U : 0U;
    }
    CRD_FUZZ_REQUIRE(delivered == sent_whole);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > 4096U)
    {
        return 0;
    }
    Input in{data, size};
    const u8 mode = in.byte() % 6U;
    if (mode < 3U)
    {
        hostile(in, mode);
    }
    else
    {
        honest(in, mode);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A route, a frame, a settle; a hostile datagram; an honest mix.
    const u8 hostile_script[] = {0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x01U, 0x08U, 0x61U, 0x62U, 0x0BU, 0x00U};
    sink.add("hostile-quic", hostile_script, sizeof(hostile_script));
    const u8 webtransport_script[] = {0x02U, 0x07U, 0x00U, 0x01U, 0x02U, 0x04U,
                                      0x61U, 0x62U, 0x63U, 0x64U, 0x0BU, 0x00U};
    sink.add("hostile-webtransport", webtransport_script, sizeof(webtransport_script));
    const u8 honest_script[] = {0x03U, 0x50U, 0x00U, 0x10U, 0x01U, 0x20U, 0x02U, 0x30U, 0x03U, 0xFFU};
    sink.add("honest", honest_script, sizeof(honest_script));
}
