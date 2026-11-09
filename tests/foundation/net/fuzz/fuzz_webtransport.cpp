// crd-fuzz-net-webtransport: WebTransport sessions in crd-net (4.2a; docs/design/network-foundation.md#webtransport)
// under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: a mode byte, then that mode's script (0 past the end).
//   0, 1  a peer driving HTTP/3 by hand against sessions on a server (0) or a client (1): any capsule, on any CONNECT
//         stream, any stream header naming any session, ends, resets and stops, datagrams, and an application calling
//         every method at any time. The pool and the waiting room stay within their bounds, every session ID reported
//         is a client's bidirectional stream, a close message is valid text of at most 1024 bytes, and a connection
//         error is one the draft names.
//   2     both sides honest under tight limits, over streams, writes, ends, reliable resets and stops in any order: no
//         session is ever closed, each stream that ended arrives with exactly its bytes, a reset one with at least what
//         it kept, and after it all the sender's window has come back: nothing leaks.
#include "../http3_fixture.hpp"

#include <crd/core/utf8.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/webtransport.hpp>

#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::crypto::QuicSide;
using crd::fuzz::BudgetAllocator;
using namespace crd::net;
using namespace crd::net::testing;

constexpr usize kTrack = 32U;
constexpr u32 kPoolSessions = 2U;
constexpr u32 kPoolStreams = 2U;

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

[[nodiscard]] Http3Config limited(QuicSide side, u64 uni, u64 bidi, u64 data)
{
    Http3Config config = h3_wt_config(side);
    config.webtransport.max_streams_uni = uni;
    config.webtransport.max_streams_bidi = bidi;
    config.webtransport.max_data = data;
    return config;
}

[[nodiscard]] WebTransportConfig fuzz_config()
{
    WebTransportConfig config;
    config.sessions = kPoolSessions;
    config.waiting_streams = kPoolStreams;
    config.waiting_datagrams = 2U;
    config.datagram_bytes = 256U;
    config.any_origin = true;
    return config;
}

[[nodiscard]] bool known_error(u64 code)
{
    return code == kH3DatagramError || (code >= kH3NoError && code <= kH3ConnectError);
}

[[nodiscard]] bool valid_text(ConstSpan<u8> text)
{
    for (usize at = 0U; at < text.size();)
    {
        const usize length = crd::utf8_sequence_length(text.data() + at, text.size() - at);
        if (length == 0U)
        {
            return false;
        }
        at += length;
    }
    return true;
}

struct Hostile
{
    Hostile(H3Rig& rig_in, WebTransport& wt_in, usize under_in) : rig(rig_in), wt(wt_in), under(under_in) {}

    H3Rig& rig;
    WebTransport& wt;
    usize under;
    u64 sessions[kTrack] = {};
    usize session_count = 0U;
    u64 streams[kTrack] = {};
    usize stream_count = 0U;

    void remember(u64* list, usize& count, u64 id)
    {
        if (count < kTrack)
        {
            list[count++] = id;
        }
    }

    void poll()
    {
        WtEvent event;
        while (wt.poll(event))
        {
            if (event.kind == WtEventKind::Http)
            {
                continue;
            }
            CRD_FUZZ_REQUIRE((event.session & 3U) == 0U);
            if (event.kind == WtEventKind::SessionRequest || event.kind == WtEventKind::SessionReady)
            {
                remember(sessions, session_count, event.session);
            }
            if (event.kind == WtEventKind::SessionClosed && event.has_code)
            {
                CRD_FUZZ_REQUIRE(event.bytes.size() <= kWtMaxCloseMessage && valid_text(event.bytes));
            }
            if (event.kind == WtEventKind::StreamOpened)
            {
                remember(streams, stream_count, event.stream);
            }
            if (event.kind == WtEventKind::Datagram)
            {
                CRD_FUZZ_REQUIRE(event.bytes.size() <= 1500U);
            }
        }
        CRD_FUZZ_REQUIRE(wt.sessions_alive() <= kPoolSessions);
        CRD_FUZZ_REQUIRE(wt.streams_waiting() <= kPoolStreams);
        CRD_FUZZ_REQUIRE(rig.h3[under].error() == 0U || known_error(rig.h3[under].error()));
    }

    void settle()
    {
        for (u32 round = 0U; round < 256U; ++round)
        {
            poll();
            rig.poll(1U - under);
            const bool a = rig.deliver(0U);
            const bool b = rig.deliver(1U);
            if (!a && !b)
            {
                break;
            }
        }
        poll();
    }

    void application(Input& in)
    {
        const u64 session = (session_count != 0U) ? sessions[in.byte() % session_count] : u64{in.byte()} * 4U;
        const u64 stream = (stream_count != 0U) ? streams[in.byte() % stream_count] : u64{in.byte()};
        usize taken = 0U;
        u64 opened = 0U;
        switch (in.byte() % 11U)
        {
            case 0U:
                static_cast<void>(wt.accept(session, {}, in.bytes(in.byte() & 7U)));
                break;
            case 1U:
                static_cast<void>(wt.reject(session, static_cast<crd::u16>(250U + (u32{in.byte()} * 2U))));
                break;
            case 2U:
                static_cast<void>(wt.close(session, in.byte(), in.bytes(in.byte())));
                break;
            case 3U:
                static_cast<void>(wt.drain(session));
                break;
            case 4U:
                if (wt.open_stream(session, (in.byte() & 1U) != 0U, opened) == Http3Error::None)
                {
                    remember(streams, stream_count, opened);
                }
                break;
            case 5U:
                static_cast<void>(wt.write(stream, in.bytes(in.byte()), (in.byte() & 1U) != 0U, taken));
                break;
            case 6U:
                static_cast<void>(wt.reset(stream, in.byte(), in.byte() & 7U));
                break;
            case 7U:
                static_cast<void>(wt.stop(stream, in.byte()));
                break;
            case 8U:
            {
                const QuicStreamRead readable = wt.peek(stream);
                static_cast<void>(wt.consume(stream, (readable.data.size() < in.byte()) ? readable.data.size() : 0U));
                break;
            }
            case 9U:
                static_cast<void>(wt.send_datagram(session, in.bytes(in.byte()), 1U));
                break;
            default:
            {
                u64 fresh = 0U;
                static_cast<void>(wt.connect(h3_wt_connect().span(), fresh, (in.byte() & 1U) != 0U));
                break;
            }
        }
    }
};

void hostile(Input& in, usize under)
{
    BudgetAllocator allocator;
    const u8 tight = in.byte();
    const u64 uni = tight & 3U;
    const u64 bidi = (tight >> 2U) & 3U;
    const u64 data = (tight & 0x10U) * 8U;
    H3Rig rig(allocator, true, true, limited(QuicSide::Client, uni, bidi, data),
              limited(QuicSide::Server, uni, bidi, data), h3_limits(), false);
    WebTransport wt;
    if (!rig.ok || !wt.init(allocator, rig.h3[under], rig.streams[under], fuzz_config()))
    {
        return;
    }
    rig.active[under] = false;
    const usize peer = 1U - under;
    Hostile state(rig, wt, under);
    state.settle();
    u64 connects[kTrack] = {};
    usize connect_count = 0U;
    u64 raw[kTrack] = {};
    usize raw_count = 0U;
    while (in.more())
    {
        const u8 op = in.byte();
        const u64 connect = (connect_count != 0U) ? connects[in.byte() % connect_count] : u64{0};
        const u64 stream = (raw_count != 0U) ? raw[in.byte() % raw_count] : u64{2};
        Http3Connection& h3 = rig.h3[peer];
        usize taken = 0U;
        u64 id = 0U;
        switch (op % 12U)
        {
            case 0U:
                if (((peer == kH3Client) ? h3.open_request(h3_wt_connect().span(), false, id)
                                         : wt.connect(h3_wt_connect().span(), id)) == Http3Error::None)
                {
                    state.remember(connects, connect_count, id);
                }
                break;
            case 1U:
                static_cast<void>(h3.send_response(connect, h3_ok().span(), false));
                break;
            case 2U:
                static_cast<void>(h3.send_body(connect, in.bytes(in.byte()), (in.byte() & 1U) != 0U, taken));
                break;
            case 3U:
            {
                static const u64 kTypes[] = {0x2843U,     0x78AEU,     0x190B4D3DU, 0x190B4D3EU, 0x190B4D3FU,
                                             0x190B4D40U, 0x190B4D41U, 0x190B4D42U, 0x190B4D43U, 0x190B4D44U,
                                             0U,          0x21U};
                H3Wire wire;
                const ConstSpan<u8> value = in.bytes(in.byte() & 0x3FU);
                wire.varint(kTypes[in.byte() % (sizeof(kTypes) / sizeof(kTypes[0]))]);
                wire.varint(value.size()).raw(value);
                static_cast<void>(h3.send_body(connect, wire.span(), false, taken));
                break;
            }
            case 4U:
                static_cast<void>(h3.finish(connect));
                break;
            case 5U:
                static_cast<void>(h3.cancel(connect, in.byte()));
                break;
            case 6U:
                if (h3.open_wt_stream(connect, (in.byte() & 1U) != 0U, id) == Http3Error::None)
                {
                    state.remember(raw, raw_count, id);
                }
                break;
            case 7U:
                rig.write(peer, stream, in.bytes(in.byte()));
                break;
            case 8U:
                if ((in.byte() & 1U) != 0U)
                {
                    static_cast<void>(rig.streams[peer].reset(stream, in.byte(), in.byte() & 7U));
                }
                else
                {
                    static_cast<void>(rig.streams[peer].stop_sending(stream, in.byte()));
                }
                break;
            case 9U:
                static_cast<void>(h3.send_datagram(connect, in.bytes(in.byte()), 1U));
                break;
            case 10U:
                state.application(in);
                break;
            default:
                state.settle();
                break;
        }
    }
    state.settle();
    rig.write_ok = true;
    if (rig.h3[under].error() != 0U)
    {
        WtEvent event;
        CRD_FUZZ_REQUIRE(!wt.poll(event));
    }
}

struct Track
{
    u64 id = 0U;
    usize from = 0U;
    u64 sent = 0U;
    u64 kept = 0U;
    u64 received = 0U;
    bool fin = false;
    bool reset = false;
    bool stopped = false;
    bool ended = false; // the reader saw the end
};

struct Honest
{
    Honest(H3Rig& rig_in, WebTransport (&wt_in)[2]) : rig(rig_in), wt(wt_in) {}

    H3Rig& rig;
    WebTransport (&wt)[2];
    u64 session = 0U;
    bool ready[2] = {};
    Track tracks[kTrack];
    usize count = 0U;

    [[nodiscard]] Track* find(u64 id)
    {
        for (usize at = 0U; at < count; ++at)
        {
            if (tracks[at].id == id)
            {
                return &tracks[at];
            }
        }
        return nullptr;
    }

    void drain(usize side, u64 stream)
    {
        Track* const track = find(stream);
        CRD_FUZZ_REQUIRE(track != nullptr);
        for (;;)
        {
            const QuicStreamRead in = wt[side].peek(stream);
            for (usize at = 0U; at < in.data.size(); ++at)
            {
                CRD_FUZZ_REQUIRE(in.data[at] == static_cast<u8>(track->received + at));
            }
            track->received += in.data.size();
            const bool done = in.fin;
            if (in.data.empty() && !done)
            {
                return;
            }
            CRD_FUZZ_REQUIRE(wt[side].consume(stream, in.data.size()) == Http3Error::None);
            if (done)
            {
                track->ended = true;
                return;
            }
        }
    }

    void poll(usize side)
    {
        WtEvent event;
        while (wt[side].poll(event))
        {
            CRD_FUZZ_REQUIRE(event.kind != WtEventKind::SessionClosed && event.kind != WtEventKind::SessionRefused);
            switch (event.kind)
            {
                case WtEventKind::SessionRequest:
                    CRD_FUZZ_REQUIRE(wt[side].accept(event.session) == Http3Error::None);
                    ready[side] = true;
                    break;
                case WtEventKind::SessionReady:
                    ready[side] = true;
                    break;
                case WtEventKind::StreamOpened:
                case WtEventKind::StreamReadable:
                    drain(side, event.stream);
                    break;
                default:
                    break;
            }
        }
    }

    void settle()
    {
        for (u32 round = 0U; round < 1024U; ++round)
        {
            poll(kH3Client);
            poll(kH3Server);
            const bool a = rig.deliver(kH3Client);
            const bool b = rig.deliver(kH3Server);
            if (!a && !b)
            {
                break;
            }
        }
        poll(kH3Client);
        poll(kH3Server);
    }

    void write(Track& track, usize size, bool fin)
    {
        u8 bytes[256];
        for (usize at = 0U; at < size; ++at)
        {
            bytes[at] = static_cast<u8>(track.sent + at);
        }
        usize taken = 0U;
        const Http3Error error = wt[track.from].write(track.id, ConstSpan<u8>(bytes, size), fin, taken);
        CRD_FUZZ_REQUIRE(error == Http3Error::None || error == Http3Error::Blocked);
        track.sent += taken;
        track.fin = track.fin || (fin && taken == size && error == Http3Error::None);
    }

    void open(usize from, bool bidirectional)
    {
        u64 id = 0U;
        if (count < kTrack && wt[from].open_stream(session, bidirectional, id) == Http3Error::None)
        {
            tracks[count].id = id;
            tracks[count].from = from;
            ++count;
        }
    }
};

void honest(Input& in)
{
    BudgetAllocator allocator;
    const u64 uni = 1U + (in.byte() % 4U);
    const u64 bidi = 1U + (in.byte() % 4U);
    const u64 window = 16U + (u64{in.byte()} * 4U);
    H3Rig rig(allocator, true, true, limited(QuicSide::Client, uni, bidi, window),
              limited(QuicSide::Server, uni, bidi, window), h3_limits(), false);
    WebTransport wt[2];
    if (!rig.ok || !wt[0].init(allocator, rig.h3[0], rig.streams[0], fuzz_config()) ||
        !wt[1].init(allocator, rig.h3[1], rig.streams[1], fuzz_config()))
    {
        return;
    }
    rig.active[0] = false;
    rig.active[1] = false;
    Honest state(rig, wt);
    state.settle();
    CRD_FUZZ_REQUIRE(wt[kH3Client].connect(h3_wt_connect().span(), state.session) == Http3Error::None);
    state.settle();
    CRD_FUZZ_REQUIRE(state.ready[0] && state.ready[1]);
    u64 probe = 0U;
    CRD_FUZZ_REQUIRE(wt[kH3Client].open_stream(state.session, false, probe) == Http3Error::None);
    state.tracks[0].id = probe;
    state.count = 1U;
    while (in.more())
    {
        const u8 op = in.byte();
        const usize side = op & 1U;
        const usize pick = (state.count > 1U) ? 1U + (in.byte() % (state.count - 1U)) : 0U;
        Track& track = state.tracks[pick];
        const bool live = pick != 0U && !track.fin && !track.reset && !track.stopped;
        switch ((op >> 1U) % 7U)
        {
            case 0U:
                state.open(side, (in.byte() & 1U) != 0U);
                break;
            case 1U:
                if (live)
                {
                    state.write(track, in.byte(), false);
                }
                break;
            case 2U:
                if (live)
                {
                    state.write(track, in.byte() & 0x3FU, true);
                }
                break;
            case 3U:
                if (live)
                {
                    const u64 most = (track.sent < 16U) ? track.sent : 16U;
                    const u64 offset = (most != 0U) ? in.byte() % (most + 1U) : 0U;
                    if (wt[track.from].reset(track.id, in.byte(), offset) == Http3Error::None)
                    {
                        track.reset = true;
                        track.kept = offset;
                    }
                }
                break;
            case 4U:
                if (live)
                {
                    const usize reader = 1U - track.from;
                    if (wt[reader].stop(track.id, in.byte()) == Http3Error::None)
                    {
                        track.stopped = true;
                    }
                }
                break;
            case 5U:
                static_cast<void>(rig.deliver(side));
                break;
            default:
                state.settle();
                break;
        }
        CRD_FUZZ_REQUIRE(rig.h3[0].error() == 0U && rig.h3[1].error() == 0U);
    }
    // Everything ends: what is still open is finished so the sender's whole window can come back.
    for (usize at = 1U; at < state.count; ++at)
    {
        Track& track = state.tracks[at];
        if (!track.fin && !track.reset && !track.stopped)
        {
            state.write(track, 0U, true);
        }
    }
    state.settle();
    for (usize at = 1U; at < state.count; ++at)
    {
        const Track& track = state.tracks[at];
        if (track.stopped)
        {
            continue;
        }
        if (track.reset)
        {
            CRD_FUZZ_REQUIRE(track.received >= track.kept);
        }
        else
        {
            CRD_FUZZ_REQUIRE(track.fin && track.ended && track.received == track.sent);
        }
    }
    // Credit: at most half a window (the granting threshold) may be outstanding when nothing is held.
    u8 fill[256 * 4 + 16] = {};
    usize total = 0U;
    while (total < window)
    {
        usize taken = 0U;
        const usize want = ((window - total) < sizeof(fill)) ? static_cast<usize>(window - total) : sizeof(fill);
        const Http3Error error = wt[kH3Client].write(probe, ConstSpan<u8>(fill, want), false, taken);
        total += taken;
        if (error != Http3Error::None || taken == 0U)
        {
            break;
        }
    }
    CRD_FUZZ_REQUIRE(total >= window / 2U);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > 8192U)
    {
        return 0;
    }
    Input in{data, size};
    switch (in.byte() % 3U)
    {
        case 0U:
            hostile(in, kH3Server);
            break;
        case 1U:
            hostile(in, kH3Client);
            break;
        default:
            honest(in);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A CONNECT, the application's accept, a stream with its header, a capsule.
    const u8 server[] = {0x00U, 0x15U, 0x00U, 0x0BU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x0BU, 0x06U,
                         0x00U, 0x00U, 0x01U, 0x0BU, 0x03U, 0x00U, 0x00U, 0x04U, 0x61U, 0x62U, 0x0BU};
    sink.add("server", server, sizeof(server));
    const u8 client[] = {0x01U, 0x15U, 0x00U, 0x0BU, 0x01U, 0x00U, 0x0BU, 0x06U, 0x00U, 0x00U, 0x0BU};
    sink.add("client", client, sizeof(client));
    const u8 honest_script[] = {0x02U, 0x03U, 0x03U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x40U, 0x0CU,
                                0x00U, 0x00U, 0x05U, 0x00U, 0x06U, 0x00U, 0x10U, 0x0CU, 0x07U, 0x00U, 0x00U};
    sink.add("honest", honest_script, sizeof(honest_script));
}
