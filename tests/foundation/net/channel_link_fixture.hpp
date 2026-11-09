#pragma once

// What the channel tests share (4.2a; docs/design/network-foundation.md#channels): one schema with a channel of every
// shape, two ends of a session over a QUIC-shaped stream set, a QMux-shaped one or a WebTransport session, and frames
// as a writer lays them.

#include "admission_fixture.hpp"
#include "http3_fixture.hpp"

#include <crd/net/channel_link.hpp>
#include <crd/net/frame.hpp>
#include <crd/net/webtransport.hpp>

#include <cstring>
#include <memory>

namespace crd::net::testing
{

enum class LinkPath : u8
{
    Quic,
    Qmux,
    WebTransport,
};

inline constexpr u32 kLinkControl = 0U;
inline constexpr u32 kLinkEvent = 1U;
inline constexpr u32 kLinkState = 2U;
inline constexpr u32 kLinkMedia = 3U;
inline constexpr u32 kLinkPadded = 4U;
inline constexpr u32 kLinkSlow = 5U;
inline constexpr u32 kLinkBulk = 6U;

inline constexpr u32 kLinkHello = 1U;
inline constexpr u32 kLinkNote = 2U;
inline constexpr u32 kLinkSnapshot = 3U;
inline constexpr u32 kLinkVoice = 4U;
inline constexpr u32 kLinkEdit = 5U;
inline constexpr u32 kLinkCrawl = 6U;
inline constexpr u32 kLinkBlob = 7U;
inline constexpr u32 kLinkReplayed = 8U;
inline constexpr u32 kLinkOnce = 9U;

inline constexpr u32 kLinkPaddingBlock = 64U;
inline constexpr u32 kLinkMediaBlock = 32U;

inline constexpr wire::ChannelDesc kLinkChannels[] = {
    {"control", wire::ChannelClass::Control, true, true, false, 0U, 100000U, 100000U, 1000000000ULL, 0U, false},
    {"event", wire::ChannelClass::Event, true, false, false, 2U, 100000U, 100000U, 1000000000ULL, 0U, false},
    {"state", wire::ChannelClass::State, false, false, true, 1U, 100000U, 100000U, 1000000000ULL, 0U, false},
    {"media", wire::ChannelClass::Media, false, false, false, 5U, 100000U, 100000U, 1000000000ULL, kLinkMediaBlock,
     false},
    {"padded", wire::ChannelClass::Transaction, true, true, false, 3U, 100000U, 100000U, 1000000000ULL,
     kLinkPaddingBlock, false},
    {"slow", wire::ChannelClass::Event, true, true, false, 4U, 100000U, 1U, 1000000000ULL, 0U, false},
    {"bulk", wire::ChannelClass::Bulk, true, true, false, 6U, 100000U, 100000U, 1000000000ULL, 0U, false},
};

inline constexpr wire::MessageDesc kLinkMessages[] = {
    {"Hello", kLinkHello, 0U, 0U, 1ULL, 0U, 100000U},       {"Note", kLinkNote, 1U, 0U, 1ULL, 0U, 100000U},
    {"Snapshot", kLinkSnapshot, 2U, 0U, 1ULL, 0U, 100000U}, {"Voice", kLinkVoice, 3U, 0U, 1ULL, 0U, 100000U},
    {"Edit", kLinkEdit, 4U, 0U, 1ULL, 0U, 100000U},         {"Crawl", kLinkCrawl, 5U, 0U, 1ULL, 0U, 100000U},
    {"Blob", kLinkBlob, 6U, 0U, 1ULL, 0U, 100000U},         {"Replayed", kLinkReplayed, 0U, 0U, 2ULL, 0U, 100000U},
    {"Once", kLinkOnce, 0U, 0U, 4ULL, 0U, 100000U},
};

inline constexpr wire::WireSchema kLinkSchema{
    "link",
    {0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U, 0x58U, 0x59U, 0x5AU, 0x5BU, 0x5CU, 0x5DU, 0x5EU, 0x5FU, 0x60U},
    static_cast<u8>(crypto::ScopeKind::World),
    1U,
    1U,
    7ULL,
    2ULL, // Replayed may run on early data; Hello and Once may not
    kLinkChannels,
    kLinkMessages,
};

// One Sized (or, for a datagram, ToEnd) frame of `body` at the start of `out`; its size, 0 for no room.
[[nodiscard]] inline usize link_frame(u32 message, cont::ConstSpan<u8> body, cont::Span<u8> out,
                                      HeaderForm form = HeaderForm::Sized)
{
    const wire::MessageDesc* const found = wire::find_message(kLinkSchema, message);
    if (found == nullptr)
    {
        return 0U;
    }
    FrameHeader header;
    header.message = message;
    header.length = static_cast<u32>(body.size());
    header.object = 7U;
    const FrameShape shape = frame_shape(kLinkSchema.channels[found->channel], *found);
    const usize used = write_frame_header(form, shape, header, out);
    if (used == 0U || used + body.size() > out.size())
    {
        return 0U;
    }
    if (!body.empty())
    {
        std::memcpy(out.data() + used, body.data(), body.size());
    }
    return used + body.size();
}

inline QuicStreamLimits link_limits(LinkPath path)
{
    QuicStreamLimits limits = h3_limits();
    limits.peer_uni = 32U;
    limits.local_uni = 32U;
    limits.peer_bidi = 8U;
    limits.local_bidi = 8U;
    limits.in_order = path == LinkPath::Qmux;
    limits.reset_stream_at = path != LinkPath::Qmux;
    return limits;
}

// One end of a session: its admission, its link and what its link reported.
struct LinkEnd
{
    explicit LinkEnd(const QuotaLimits& limits) : quota(limits), admission(quota, buffers, channels)
    {
        authorized = test::authorized_for(kLinkSchema);
        capability = test::capability_for(authorized);
        const SessionSchema schemas[] = {{&kLinkSchema, &authorized}};
        bound =
            admission.bind(schemas) && admission.announce(cont::ConstSpan<u8>(kLinkSchema.id, sizeof(kLinkSchema.id)),
                                                          kLinkSchema.major, kLinkSchema.minor);
        admission.set_capability(&capability);
    }

    test::Buffers buffers;
    PrincipalQuota quota;
    ChannelState channels[16];
    Admission admission;
    crypto::AuthorizedSchema authorized;
    crypto::Capability capability;
    bool bound = false;
    std::unique_ptr<ChannelLink> link;
    ChannelCarrier carrier;
};

// A session between two ends. `drive` reads each link to the end whenever it is called.
class LinkRig
{
public:
    LinkRig(memory::IAllocator& allocator, LinkPath path_in, const QuotaLimits& limits = QuotaLimits{},
            const QuicStreamLimits* stream_limits = nullptr, const Http3Config* client = nullptr,
            const Http3Config* server = nullptr)
        : path(path_in)
    {
        const QuicStreamLimits streams = (stream_limits != nullptr) ? *stream_limits : link_limits(path);
        const bool http = path == LinkPath::WebTransport;
        const Http3Config client_config = (client != nullptr) ? *client : h3_wt_config(crypto::QuicSide::Client);
        const Http3Config server_config = (server != nullptr) ? *server : h3_wt_config(crypto::QuicSide::Server);
        wire = std::make_unique<H3Rig>(allocator, http, http, client_config, server_config, streams, false);
        ok = wire->ok;
        for (usize side = 0U; side < 2U; ++side)
        {
            end[side] = std::make_unique<LinkEnd>(limits);
            ok = ok && end[side]->bound;
        }
        if (ok && http)
        {
            ok = open_session(allocator);
        }
        for (usize side = 0U; ok && side < 2U; ++side)
        {
            end[side]->carrier = http ? ChannelCarrier(*transport[side], session)
                                      : ChannelCarrier(wire->streams[side], wire->datagrams[side]);
        }
    }

    [[nodiscard]] bool link(usize side, memory::IAllocator& allocator, const ChannelLinkConfig& config = {})
    {
        end[side]->link = std::make_unique<ChannelLink>();
        return end[side]->link->init(allocator, end[side]->admission, end[side]->carrier, config);
    }

    [[nodiscard]] bool link_both(memory::IAllocator& allocator, const ChannelLinkConfig& config = {})
    {
        return link(kH3Client, allocator, config) && link(kH3Server, allocator, config);
    }

    template <typename Visit> void poll(usize side, Visit& visit)
    {
        LinkEnd& here = *end[side];
        if (path == LinkPath::WebTransport)
        {
            WtEvent event;
            while (transport[side]->poll(event))
            {
                if (here.link != nullptr && event.session == session)
                {
                    here.link->on_webtransport(event, wire->now);
                    drain(side, visit);
                }
            }
        }
        if (here.link != nullptr)
        {
            drain(side, visit);
        }
    }

    template <typename Visit> void drain(usize side, Visit& visit)
    {
        ChannelEvent event;
        while (end[side]->link->next(event, wire->now))
        {
            visit(side, event);
        }
    }

    // Until nothing moves.
    template <typename Visit> void settle(Visit& visit)
    {
        for (u32 round = 0U; round < 1024U; ++round)
        {
            poll(kH3Client, visit);
            poll(kH3Server, visit);
            const bool a = wire->deliver(kH3Client);
            const bool b = wire->deliver(kH3Server);
            if (!a && !b)
            {
                break;
            }
        }
        poll(kH3Client, visit);
        poll(kH3Server, visit);
    }

    LinkPath path;
    bool ok = false;
    std::unique_ptr<H3Rig> wire;
    std::unique_ptr<WebTransport> transport[2];
    std::unique_ptr<LinkEnd> end[2];
    u64 session = 0U;

private:
    [[nodiscard]] bool open_session(memory::IAllocator& allocator)
    {
        WebTransportConfig config;
        config.any_origin = true;
        for (usize side = 0U; side < 2U; ++side)
        {
            transport[side] = std::make_unique<WebTransport>();
            if (!transport[side]->init(allocator, wire->h3[side], wire->streams[side], config))
            {
                return false;
            }
            wire->active[side] = false;
        }
        for (u32 round = 0U; round < 16U && !m_session_ready; ++round)
        {
            shuttle(round == kSettingsRounds);
        }
        return m_session_ready;
    }

    void shuttle(bool connect_now)
    {
        WtEvent event;
        for (usize side = 0U; side < 2U; ++side)
        {
            while (transport[side]->poll(event))
            {
                if (event.kind == WtEventKind::SessionRequest)
                {
                    static_cast<void>(transport[side]->accept(event.session));
                }
                if (event.kind == WtEventKind::SessionReady)
                {
                    m_session_ready = true;
                }
            }
        }
        if (connect_now)
        {
            static_cast<void>(transport[kH3Client]->connect(h3_wt_connect().span(), session));
        }
        static_cast<void>(wire->deliver(kH3Client));
        static_cast<void>(wire->deliver(kH3Server));
    }

    static constexpr u32 kSettingsRounds = 4U; // the peers' SETTINGS have crossed
    bool m_session_ready = false;
};

} // namespace crd::net::testing
