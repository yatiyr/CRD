// crd-fuzz-net-http2: HTTP/2 in crd-net (4.2a; docs/design/network-foundation.md#http2) under the bounded harness
// (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: a mode byte, then that mode's bytes (0 past the end).
//   0  a hostile client at a server: any bytes after a real preface, cut anywhere, the application answering what it is
//      told about. Every event's views lie in the input or the connection's storage, Body bytes never exceed what the
//      peer's frames could hold, the open streams stay within the limit, the output stays within its buffer and parses
//      as frames, and once the connection failed nothing more happens.
//   1  a hostile server at a client: the same, with requests opened as the input says.
//   2  both sides honest over a wire that cuts anywhere: every request and response arrives with its fields, its body
//      in order and its end, whatever the windows, frame sizes and pieces.
#include "text_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/http2.hpp>

#include <algorithm>
#include <string_view>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::fuzz::BudgetAllocator;
using namespace crd::net::testing;
namespace cont = crd::containers;
using namespace crd::net;

constexpr char kPrefaceText[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr u64 kStepNs = 50'000'000U;

struct Bytes
{
    ConstSpan<u8> data;
    usize at = 0U;

    [[nodiscard]] u8 next() noexcept { return (at < data.size()) ? data[at++] : u8{0}; }
    [[nodiscard]] bool done() const noexcept { return at >= data.size(); }
};

Http2Config config_for(Http2Side side, Bytes& in)
{
    Http2Config config;
    config.side = side;
    config.field_bytes = 1024U + (u32{in.next()} * 16U);
    config.streams = 1U + (in.next() % 8U);
    config.stream_window = 1U + (u32{in.next()} * 257U);
    config.connection_window = kHttp2InitialWindow + (u32{in.next()} * 1021U);
    config.frame_bytes = kHttp2MinFrameBytes + (u32{in.next()} * 16U);
    config.table_bytes = u32{in.next()} * 16U;
    config.encoder_table_bytes = u32{in.next()} * 16U;
    config.output_bytes = kHttp2MinOutputBytes + (usize{in.next()} * 32U);
    config.connect_protocol = side == Http2Side::Server && (in.next() & 1U) != 0U;
    return config;
}

void check_output(const Http2Connection& connection, const Http2Config& config)
{
    CRD_FUZZ_REQUIRE(connection.output().size() <= config.output_bytes);
}

const HttpField kStatus{http_text(":status"), http_text("200"), false};
const HttpField kRequest[] = {
    {http_text(":method"), http_text("POST"), false},
    {http_text(":scheme"), http_text("https"), false},
    {http_text(":authority"), http_text("a.example"), false},
    {http_text(":path"), http_text("/fuzz"), false},
};

const u8 kBody[64] = {};

ConstSpan<HttpField> request_fields()
{
    return ConstSpan<HttpField>(kRequest, sizeof(kRequest) / sizeof(kRequest[0]));
}

// What an application does with one event: everything the event allows, as the input says.
void answer(Http2Connection& connection, const Http2Event& event, Bytes& in)
{
    usize taken = 0U;
    const u8 choice = in.next();
    if (event.kind == Http2EventKind::Body)
    {
        static_cast<void>(connection.consume_body(event.stream, event.payload.size()));
    }
    if (event.kind == Http2EventKind::Headers && connection.config().side == Http2Side::Server)
    {
        static_cast<void>(
            connection.send_response(event.stream, ConstSpan<HttpField>(&kStatus, 1U), (choice & 1U) != 0U));
    }
    if ((choice & 6U) == 2U)
    {
        static_cast<void>(connection.send_body(event.stream, ConstSpan<u8>(kBody, choice >> 2U), false, taken));
    }
    if ((choice & 6U) == 4U)
    {
        static_cast<void>(connection.finish(event.stream));
    }
    if ((choice & 0xF8U) == 0xF8U)
    {
        static_cast<void>(connection.cancel(event.stream, kH2Cancel));
    }
    if (choice == 0xA5U)
    {
        static_cast<void>(connection.goaway(false));
    }
    if (choice == 0xA7U)
    {
        static_cast<void>(connection.goaway(true));
    }
}

// Feeds the hostile bytes in random pieces, reading every event.
void hostile(const u8* data, usize size, Http2Side side)
{
    Bytes in{ConstSpan<u8>(data, size)};
    BudgetAllocator allocator;
    const Http2Config config = config_for(side, in);
    Http2Connection connection;
    u64 now = 1U;
    if (!connection.init(allocator, config, now))
    {
        return;
    }
    cont::Array<u8> wire;
    wire.reserve(size + kHttp2PrefaceBytes);
    if (side == Http2Side::Server)
    {
        for (const char c : std::string_view(kPrefaceText))
        {
            wire.push_back(static_cast<u8>(c));
        }
    }
    while (!in.done())
    {
        wire.push_back(in.next());
    }
    in.at = in.data.size() / 2U; // the application's choices come from the second half again
    usize held = 0U;
    usize at = 0U;
    usize steps = 0U;
    while (steps++ < 200000U)
    {
        held = std::min(wire.size(), held + 1U + (usize{in.next()} % 4096U));
        Http2Event event;
        usize taken = 0U;
        const bool had_error = connection.error() != 0U;
        const bool more = connection.poll(ConstSpan<u8>(wire.data() + at, held - at), now, taken, event);
        CRD_FUZZ_REQUIRE(taken <= held - at);
        CRD_FUZZ_REQUIRE(!had_error || (!more && taken == 0U));
        check_output(connection, config);
        CRD_FUZZ_REQUIRE(connection.open_streams() <= config.streams);
        if (more)
        {
            for (const HttpField& field : event.fields)
            {
                CRD_FUZZ_REQUIRE(field.name.size() + field.value.size() <= config.field_bytes);
            }
            if (event.kind == Http2EventKind::Body)
            {
                CRD_FUZZ_REQUIRE(!event.payload.empty());
                CRD_FUZZ_REQUIRE(event.payload.data() >= wire.data() + at);
                CRD_FUZZ_REQUIRE(event.payload.data() + event.payload.size() <= wire.data() + at + taken);
            }
            answer(connection, event, in);
        }
        at += taken;
        const usize drain = (in.next() & 3U) == 0U ? 0U : connection.output().size();
        connection.output_taken(drain);
        now += kStepNs;
        if (side == Http2Side::Client && (in.next() & 15U) == 0U)
        {
            u32 id = 0U;
            static_cast<void>(connection.open_request(request_fields(), false, id));
        }
        if (!more && held == wire.size() && taken == 0U && drain == 0U && connection.output().empty())
        {
            break;
        }
        if (connection.error() != 0U)
        {
            break;
        }
    }
}

// One end of the honest exchange.
struct Honest
{
    Http2Connection connection;
    cont::Array<u8> in;
    usize at = 0U;
    Text body;
    Text fields;
    u32 finished = 0U;

    void take(u64 now, ConstSpan<u8> reply, bool server)
    {
        for (u32 steps = 0U; steps < 4096U; ++steps)
        {
            Http2Event event;
            usize taken = 0U;
            const bool more = connection.poll(ConstSpan<u8>(in.data() + at, in.size() - at), now, taken, event);
            CRD_FUZZ_REQUIRE(connection.error() == 0U);
            if (more)
            {
                switch (event.kind)
                {
                    case Http2EventKind::Headers:
                        fields += joined(event.fields);
                        if (server)
                        {
                            const ConstSpan<HttpField> status(&kStatus, 1U);
                            CRD_FUZZ_REQUIRE(connection.send_response(event.stream, status, false) == Http2Error::None);
                            stream = event.stream;
                        }
                        break;
                    case Http2EventKind::Body:
                        body += text_of(event.payload);
                        CRD_FUZZ_REQUIRE(connection.consume_body(event.stream, event.payload.size()) ==
                                         Http2Error::None);
                        break;
                    case Http2EventKind::Finished:
                        ++finished;
                        break;
                    case Http2EventKind::Aborted:
                        CRD_FUZZ_REQUIRE(false);
                        break;
                    default:
                        break;
                }
            }
            at += taken;
            if (!more)
            {
                break;
            }
        }
        send(reply);
    }

    void send(ConstSpan<u8> reply)
    {
        while (stream != 0U && sent < reply.size())
        {
            usize taken = 0U;
            const Http2Error error = connection.send_body(stream, reply.subspan(sent), true, taken);
            sent += taken;
            if (error != Http2Error::None)
            {
                return;
            }
        }
    }

    u32 stream = 0U;
    usize sent = 0U;
};

void honest(const u8* data, usize size)
{
    Bytes in{ConstSpan<u8>(data, size)};
    BudgetAllocator allocator;
    Http2Config client_config = config_for(Http2Side::Client, in);
    Http2Config server_config = config_for(Http2Side::Server, in);
    client_config.stream_window = std::max<u32>(client_config.stream_window, 4096U);
    server_config.stream_window = std::max<u32>(server_config.stream_window, 4096U);
    client_config.settings_timeout_ns = kNeverNs; // the wire may move one byte per round
    server_config.settings_timeout_ns = kNeverNs;
    Honest client;
    Honest server;
    u64 now = 1U;
    if (!client.connection.init(allocator, client_config, now) ||
        !server.connection.init(allocator, server_config, now))
    {
        return;
    }
    cont::Array<u8> up;
    cont::Array<u8> down;
    up.push_back(1U);
    down.push_back(1U);
    for (usize at = 0U; at < 4096U && !in.done(); ++at)
    {
        up.push_back(in.next());
        down.push_back(static_cast<u8>(at * 7U));
    }
    u32 id = 0U;
    CRD_FUZZ_REQUIRE(client.connection.open_request(request_fields(), false, id) == Http2Error::None);
    client.stream = id;
    for (u32 round = 0U; round < 200000U; ++round)
    {
        const auto move = [&in](Honest& from, Honest& to)
        {
            const ConstSpan<u8> bytes = from.connection.output();
            const usize count = std::min<usize>(bytes.size(), 1U + (usize{in.next()} % 1500U));
            for (usize at = 0U; at < count; ++at)
            {
                to.in.push_back(bytes[at]);
            }
            from.connection.output_taken(count);
        };
        move(client, server);
        move(server, client);
        now += kStepNs;
        client.take(now, ConstSpan<u8>(up.data(), up.size()), false);
        server.take(now, ConstSpan<u8>(down.data(), down.size()), true);
        if (client.finished == 1U && server.finished == 1U && client.connection.output().empty() &&
            server.connection.output().empty())
        {
            break;
        }
    }
    CRD_FUZZ_REQUIRE(server.body == text_of(ConstSpan<u8>(up.data(), up.size())));
    CRD_FUZZ_REQUIRE(client.body == text_of(ConstSpan<u8>(down.data(), down.size())));
    CRD_FUZZ_REQUIRE(client.finished == 1U);
    CRD_FUZZ_REQUIRE(server.finished == 1U);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const u8* data, size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    switch (data[0] % 3U)
    {
        case 0U:
            hostile(data + 1U, size - 1U, Http2Side::Server);
            break;
        case 1U:
            hostile(data + 1U, size - 1U, Http2Side::Client);
            break;
        default:
            honest(data + 1U, size - 1U);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // The configuration's nine bytes, then SETTINGS, a request of static entries, DATA, PING, RST_STREAM and GOAWAY.
    const u8 server[] = {
        0x00U, 0x10U, 0x02U, 0x40U, 0x08U, 0x00U, 0x10U, 0x10U, 0x10U, 0x00U,        //
        0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,               //
        0x00U, 0x00U, 0x04U, 0x01U, 0x04U, 0x00U, 0x00U, 0x00U, 0x01U,               //
        0x83U, 0x87U, 0x84U, 0x82U,                                                  //
        0x00U, 0x00U, 0x02U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x61U, 0x62U, //
        0x00U, 0x00U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,               //
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,                      //
        0x00U, 0x00U, 0x04U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,               //
        0x00U, 0x00U, 0x00U, 0x08U,                                                  //
        0x00U, 0x00U, 0x08U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,               //
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    sink.add("server", server, sizeof(server));
    // A server's SETTINGS with its ACK and a response of a static entry on the client's stream.
    const u8 client[] = {
        0x01U, 0x10U, 0x02U, 0x40U, 0x08U, 0x00U, 0x10U, 0x10U, 0x10U, 0x00U, //
        0x00U, 0x00U, 0x06U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,        //
        0x00U, 0x04U, 0x00U, 0x01U, 0x00U, 0x00U,                             //
        0x00U, 0x00U, 0x00U, 0x04U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,        //
        0x00U, 0x00U, 0x01U, 0x01U, 0x05U, 0x00U, 0x00U, 0x00U, 0x01U, 0x88U,
    };
    sink.add("client", client, sizeof(client));
    // Both honest, a few bytes of body, cut byte by byte.
    const u8 both[] = {
        0x02U, 0x10U, 0x02U, 0x40U, 0x08U, 0x00U, 0x10U, 0x10U, 0x10U, 0x00U, //
        0x10U, 0x02U, 0x40U, 0x08U, 0x00U, 0x10U, 0x10U, 0x10U, 0x01U,        //
        0x31U, 0x32U, 0x33U, 0x34U,
    };
    sink.add("both", both, sizeof(both));
}
