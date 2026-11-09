// crd-fuzz-net-http3: an HTTP/3 connection in crd-net (4.2a; docs/design/network-foundation.md#http3) under the bounded
// fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: a mode byte, then that mode's script (0 past the end).
//   0, 1  a hand-written peer writing any bytes, frames, streams, ends and resets at the connection under test, a
//         server for mode 0 and a client for mode 1, whose application calls every method at any time. Every message
//         reported passes the message checks, a connection error is one the RFCs name and stays, and everything then
//         answers Closed.
//   2     both sides honest over frames of any size and a held encoder stream: each request and body arrives exactly,
//         each response comes back, and neither side errs.
//   3     capsules read at any split yield what one reader given every byte at once does.
#include "../http3_fixture.hpp"

#include <crd/fuzz/harness.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/http_capsule.hpp>
#include <crd/net/http_message.hpp>

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

[[nodiscard]] bool known_error(u64 code)
{
    switch (code)
    {
        case kH3GeneralProtocolError:
        case kH3StreamCreationError:
        case kH3ClosedCriticalStream:
        case kH3FrameUnexpected:
        case kH3FrameError:
        case kHttp3ExcessiveLoad:
        case kH3IdError:
        case kH3SettingsError:
        case kH3MissingSettings:
        case kH3DatagramError:
        case kQpackDecompressionFailed:
        case kQpackEncoderStreamError:
        case kQpackDecoderStreamError:
            return true;
        default:
            return false;
    }
}

u32 g_side = 0U;

void check_event(const Http3Event& event)
{
    HttpMessageInfo info;
    if (event.kind == Http3EventKind::Headers && g_side == kH3Server)
    {
        CRD_FUZZ_REQUIRE(http_check_request(event.fields, info));
    }
    if ((event.kind == Http3EventKind::Headers || event.kind == Http3EventKind::Interim) && g_side == kH3Client)
    {
        CRD_FUZZ_REQUIRE(http_check_response(event.fields, info));
    }
    if (event.kind == Http3EventKind::Trailers)
    {
        CRD_FUZZ_REQUIRE(http_check_trailers(event.fields));
    }
    if (event.kind == Http3EventKind::Interim)
    {
        CRD_FUZZ_REQUIRE(info.status < 200U);
    }
}

const H3Fields& extended_connect()
{
    static const H3Fields kFields{
        {":method", "CONNECT"}, {":protocol", "w"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}};
    return kFields;
}

// The application of the connection under test calling anything.
void application_call(H3Rig& rig, usize side, Input& in, u64& stream)
{
    Http3Connection& h3 = rig.h3[side];
    usize taken = 0U;
    switch (in.byte() % 12U)
    {
        case 0U:
            static_cast<void>(h3.open_request(h3_get().span(), (in.byte() & 1U) != 0U, stream));
            break;
        case 1U:
            static_cast<void>(h3.open_request(extended_connect().span(), (in.byte() & 1U) != 0U, stream));
            break;
        case 2U:
            static_cast<void>(h3.send_response(stream, h3_ok().span(), (in.byte() & 1U) != 0U));
            break;
        case 3U:
            static_cast<void>(h3.send_response(stream, H3Fields{{":status", "100"}}.span(), false));
            break;
        case 4U:
        {
            const ConstSpan<u8> body = in.bytes(in.byte());
            static_cast<void>(h3.send_body(stream, body, (in.byte() & 1U) != 0U, taken));
            CRD_FUZZ_REQUIRE(taken <= body.size());
            break;
        }
        case 5U:
            static_cast<void>(h3.send_trailers(stream, H3Fields{{"x", "y"}}.span()));
            break;
        case 6U:
            static_cast<void>(h3.finish(stream));
            break;
        case 7U:
            static_cast<void>(h3.cancel(stream, kH3RequestCancelled));
            break;
        case 8U:
            static_cast<void>(h3.send_datagram(stream, in.bytes(in.byte()), rig.now));
            break;
        case 9U:
            static_cast<void>(h3.goaway((in.byte() & 1U) != 0U));
            break;
        case 10U:
        {
            const Http3Body body = h3.peek_body(stream);
            const usize count = (in.byte() < 128U) ? body.data.size() : usize{1};
            static_cast<void>(h3.consume_body(stream, (count <= body.data.size()) ? count : body.data.size()));
            break;
        }
        default:
            stream = (in.byte() & 0x3FU) * 4U;
            break;
    }
}

void hostile(Input& in, usize under_test)
{
    BudgetAllocator allocator;
    g_side = static_cast<u32>(under_test);
    H3Rig rig(allocator, under_test == kH3Client, under_test == kH3Server, h3_config(QuicSide::Client),
              h3_config(QuicSide::Server), h3_limits(), false);
    if (!rig.ok)
    {
        return;
    }
    rig.drain_bodies = (in.byte() & 1U) != 0U;
    rig.observe = &check_event;
    const usize peer = 1U - under_test;
    u64 opened[32] = {};
    usize open_count = 0U;
    u64 stream = 0U;
    while (in.more())
    {
        const u8 op = in.byte();
        const u64 target = (open_count != 0U) ? opened[in.byte() % open_count] : u64{0};
        switch (op % 10U)
        {
            case 0U:
                if (open_count < 32U)
                {
                    opened[open_count++] = rig.open_typed(peer, in.byte() & 7U);
                }
                break;
            case 1U:
                if (open_count < 32U)
                {
                    opened[open_count++] = rig.open(peer, (in.byte() & 1U) != 0U);
                }
                break;
            case 2U:
                rig.write(peer, target, in.bytes(in.byte()));
                break;
            case 3U:
            {
                H3Wire wire;
                const u8 selector = in.byte();
                const ConstSpan<u8> payload = in.bytes(selector & 0x7FU);
                wire.varint(in.byte());
                wire.varint(((selector & 0x80U) != 0U) ? u64{in.byte()} : payload.size());
                wire.raw(payload);
                rig.write(peer, target, wire);
                break;
            }
            case 4U:
                rig.write(peer, target, H3Wire{}, true);
                break;
            case 5U:
                static_cast<void>(rig.streams[peer].reset(target, in.byte()));
                break;
            case 6U:
                static_cast<void>(rig.streams[peer].stop_sending(target, in.byte()));
                break;
            case 7U:
                rig.settle();
                break;
            case 8U:
                application_call(rig, under_test, in, stream);
                break;
            default:
            {
                QuicFrame frame;
                frame.kind = QuicFrameKind::Datagram;
                frame.data = in.bytes(in.byte());
                static_cast<void>(rig.datagrams[under_test].on_frame(frame));
                break;
            }
        }
        CRD_FUZZ_REQUIRE(rig.h3[under_test].error() == 0U || known_error(rig.h3[under_test].error()));
    }
    rig.settle();
    Http3Connection& h3 = rig.h3[under_test];
    if (h3.error() != 0U)
    {
        CRD_FUZZ_REQUIRE(known_error(h3.error()));
        const u64 kept = h3.error();
        Http3Event event;
        CRD_FUZZ_REQUIRE(!h3.poll(event));
        u64 unused = 0U;
        CRD_FUZZ_REQUIRE(h3.open_request(h3_get().span(), true, unused) == Http3Error::Closed);
        CRD_FUZZ_REQUIRE(h3.finish(0U) == Http3Error::Closed);
        CRD_FUZZ_REQUIRE(h3.error() == kept);
    }
}

void honest(Input& in)
{
    BudgetAllocator allocator;
    g_side = kH3Client;
    H3Rig rig(allocator, true, true);
    CRD_FUZZ_REQUIRE(rig.ok);
    rig.max_frame_room = 64U + (usize{in.byte()} * 4U);
    rig.settle();

    const usize requests = 1U + (in.byte() % 6U);
    usize expected = 0U;
    u64 streams[8] = {};
    for (usize n = 0U; n < requests; ++n)
    {
        H3Fields fields = h3_get();
        const char* const values[] = {"alpha", "beta", "alpha", "gamma"};
        fields.add("x-repeat", values[in.byte() % 4U]);
        CRD_FUZZ_REQUIRE(rig.h3[kH3Client].open_request(fields.span(), false, streams[n]) == Http3Error::None);
        rig.held_stream[kH3Client] = ((in.byte() & 1U) != 0U) ? 6U : ~u64{0};
        const ConstSpan<u8> body = in.bytes(in.byte());
        usize sent = 0U;
        while (sent < body.size())
        {
            usize taken = 0U;
            const Http3Error error = rig.h3[kH3Client].send_body(streams[n], body.subspan(sent), false, taken);
            CRD_FUZZ_REQUIRE(error == Http3Error::None || error == Http3Error::Blocked);
            sent += taken;
            rig.settle();
        }
        expected += body.size();
        CRD_FUZZ_REQUIRE(rig.h3[kH3Client].finish(streams[n]) == Http3Error::None);
        rig.settle();
    }
    rig.held_stream[kH3Client] = ~u64{0};
    rig.settle();
    CRD_FUZZ_REQUIRE(rig.h3[kH3Server].error() == 0U);
    CRD_FUZZ_REQUIRE(rig.h3[kH3Client].error() == 0U);
    CRD_FUZZ_REQUIRE(rig.how_many(kH3Server, Http3EventKind::Headers) == requests);
    CRD_FUZZ_REQUIRE(rig.how_many(kH3Server, Http3EventKind::Finished) == requests);
    CRD_FUZZ_REQUIRE(rig.received[kH3Server].size() == expected);
    for (usize n = 0U; n < requests; ++n)
    {
        CRD_FUZZ_REQUIRE(rig.h3[kH3Server].send_response(streams[n], h3_ok().span(), true) == Http3Error::None);
    }
    rig.settle();
    CRD_FUZZ_REQUIRE(rig.h3[kH3Client].error() == 0U);
    CRD_FUZZ_REQUIRE(rig.how_many(kH3Client, Http3EventKind::Headers) == requests);
    CRD_FUZZ_REQUIRE(rig.how_many(kH3Client, Http3EventKind::Finished) == requests);
}
// Capsules read at any split come out the same as a reader given every byte at once, and within their bounds.
void capsules(Input& in)
{
    const ConstSpan<u8> script = in.bytes(in.size);
    HttpCapsuleReader whole;
    HttpCapsuleReader split;
    u64 whole_total = 0U;
    u64 split_total = 0U;
    const auto drain = [](HttpCapsuleReader& reader, ConstSpan<u8> bytes, u64& total)
    {
        for (;;)
        {
            HttpCapsuleChunk chunk;
            bool got = false;
            const usize used = reader.next(bytes, chunk, got);
            CRD_FUZZ_REQUIRE(used <= bytes.size());
            bytes = bytes.subspan(used);
            if (got)
            {
                CRD_FUZZ_REQUIRE(chunk.offset + chunk.value.size() <= chunk.length);
                total += chunk.value.size();
            }
            if (used == 0U && !got)
            {
                return;
            }
        }
    };
    drain(whole, script, whole_total);
    for (usize at = 0U; at < script.size();)
    {
        const usize piece = 1U + (script[at] & 7U);
        const usize count = (piece < script.size() - at) ? piece : script.size() - at;
        drain(split, script.subspan(at, count), split_total);
        at += count;
    }
    CRD_FUZZ_REQUIRE(whole_total == split_total);
    CRD_FUZZ_REQUIRE(whole.at_boundary() == split.at_boundary());
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > 16384U)
    {
        return 0;
    }
    Input in{data, size};
    switch (in.byte() % 4U)
    {
        case 0U:
            hostile(in, kH3Server);
            break;
        case 1U:
            hostile(in, kH3Client);
            break;
        case 2U:
            honest(in);
            break;
        default:
            capsules(in);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A client's control stream with SETTINGS, then a request stream with a static-table GET and its FIN, delivered.
    const u8 request[] = {0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x01U,
                          0x01U, 0x01U, 0x02U, 0x01U, 0x08U, 0x01U, 0x08U, 0x00U, 0x00U, 0xD1U, 0xC1U, 0xD7U,
                          0x50U, 0x01U, 0x61U, 0x04U, 0x01U, 0x07U, 0x08U, 0x08U, 0x02U, 0x00U, 0x07U};
    sink.add("request", request, sizeof(request));
    // A server's control stream and a response to a request the client opened.
    const u8 response[] = {0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x04U, 0x00U,
                           0x07U, 0x08U, 0x00U, 0x00U, 0x00U, 0x07U, 0x08U, 0x02U, 0x00U};
    sink.add("response", response, sizeof(response));
    // Both sides honest, small frames, three requests, a held encoder stream.
    const u8 honest_script[] = {0x02U, 0x10U, 0x03U, 0x00U, 0x01U, 0x05U, 0x61U, 0x62U, 0x63U, 0x64U,
                                0x65U, 0x02U, 0x00U, 0x02U, 0x41U, 0x42U, 0x00U, 0x00U, 0x00U};
    sink.add("honest", honest_script, sizeof(honest_script));
}
