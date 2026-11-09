// crd-net -- WebSocket as a path of QMux (4.2a; docs/design/network-foundation.md#websocket): RFC 6455's frames and
// opening handshake, every refusal of both, and QMux's records carried one per message through a real TLS session.

#include "ingress_fixture.hpp"
#include "loop_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"
#include "stream_pipe_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/qmux_host.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/register_ids.hpp>
#include <crd/net/ticket_cache.hpp>
#include <crd/net/websocket.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>
#include <initializer_list>
#include <memory>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using crd::containers::WireWriter;
using crd::crypto::TlsError;
using crd::crypto::TlsSession;
using crd::crypto::TlsState;

using Bytes = crd::containers::Array<u8>;

constexpr u64 kNow = 1000000000U;
constexpr u64 kMsNs = 1000000U;
constexpr usize kBuffer = 16U * 1024U;
constexpr const char* kProtocol = "qmux-02.game";
constexpr const char* kPath = "/game";
constexpr const char* kSampleKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr const char* kSampleAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
constexpr StringView kHttp11Protocols[] = {"http/1.1"};
constexpr StringView kBothProtocols[] = {"cerid-game/1", "http/1.1"};

ConstSpan<u8> span_of(const char* text)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text), std::strlen(text));
}

ConstSpan<u8> view(const Bytes& bytes)
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

Bytes bytes_of(ConstSpan<u8> span)
{
    Bytes bytes;
    bytes.resize(span.size());
    if (!span.empty())
    {
        std::memcpy(bytes.data(), span.data(), span.size());
    }
    return bytes;
}

Bytes bytes_of(const char* text)
{
    return bytes_of(span_of(text));
}

Bytes bytes_of(std::initializer_list<u8> list)
{
    Bytes bytes;
    bytes.resize(list.size());
    usize at = 0U;
    for (const u8 byte : list)
    {
        bytes[at++] = byte;
    }
    return bytes;
}

void append(Bytes& to, ConstSpan<u8> more)
{
    const usize old = to.size();
    to.resize(old + more.size());
    if (!more.empty())
    {
        std::memcpy(to.data() + old, more.data(), more.size());
    }
}

bool holds(const Bytes& a, const char* b)
{
    return same(view(a), span_of(b));
}

// ---- Requests and responses ----

struct Line
{
    const char* key;
    const char* text;
};

constexpr Line kRequest[] = {
    {"start", "GET /game HTTP/1.1\r\n"},
    {"host", "Host: game.cerid.test\r\n"},
    {"upgrade", "Upgrade: websocket\r\n"},
    {"connection", "Connection: Upgrade\r\n"},
    {"key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"},
    {"version", "Sec-WebSocket-Version: 13\r\n"},
    {"protocol", "Sec-WebSocket-Protocol: qmux-02.game\r\n"},
};

constexpr Line kResponse[] = {
    {"start", "HTTP/1.1 101 Switching Protocols\r\n"},
    {"upgrade", "Upgrade: websocket\r\n"},
    {"connection", "Connection: Upgrade\r\n"},
    {"accept", "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"},
    {"protocol", "Sec-WebSocket-Protocol: qmux-02.game\r\n"},
};

// A head of `lines` where the line `key` reads `text` (null drops it), and `extra` comes before the empty line.
template <usize N>
Bytes head_of(const Line (&lines)[N], const char* key = "", const char* text = "", const char* extra = "")
{
    Bytes bytes;
    for (const Line& line : lines)
    {
        const bool chosen = std::strcmp(line.key, key) == 0;
        if (chosen && text == nullptr)
        {
            continue;
        }
        append(bytes, span_of(chosen ? text : line.text));
    }
    append(bytes, span_of(extra));
    append(bytes, span_of("\r\n"));
    return bytes;
}

struct Policy
{
    ConstSpan<u8> protocols[3];
    ConstSpan<u8> origins[2];
    WebSocketConfig config;

    Policy()
    {
        protocols[0] = span_of(kProtocol);
        protocols[1] = span_of("qmux-01.game");
        protocols[2] = span_of("other");
        origins[0] = span_of("https://game.example");
        origins[1] = span_of("https://www.game.example");
        config.protocols = ConstSpan<ConstSpan<u8>>(protocols, 3U);
        config.origins = ConstSpan<ConstSpan<u8>>(origins, 2U);
        config.path = span_of(kPath);
        config.host = span_of("game.cerid.test");
    }
};

u16 status_of(const Bytes& head, const WebSocketConfig& config)
{
    usize scanned = 0U;
    WsHandshake handshake;
    REQUIRE(ws_read_request(view(head), config, scanned, handshake) == HttpDecode::Ok);
    return handshake.status;
}

u16 status_of(const Bytes& head)
{
    const Policy policy;
    return status_of(head, policy.config);
}

u16 response_status(const Bytes& head)
{
    const Policy policy;
    usize scanned = 0U;
    WsHandshake handshake;
    REQUIRE(ws_read_response(view(head), policy.config, span_of(kSampleAccept), scanned, handshake) == HttpDecode::Ok);
    return handshake.status;
}

// ---- A connection through real TLS ----

QmuxConfig qmux_config(const crypto::TlsContext& tls, const WebSocketConfig* websocket, u64 idle_ms = 30000U)
{
    QmuxConfig config;
    config.tls = &tls;
    config.websocket = websocket;
    config.streams.peer_bidi = 8U;
    config.streams.peer_uni = 4U;
    config.streams.local_bidi = 8U;
    config.streams.local_uni = 4U;
    config.streams.receive_bytes = 64U * 1024U;
    config.streams.send_bytes = 64U * 1024U;
    config.streams.stream_window = 32U * 1024U;
    config.streams.connection_window = 64U * 1024U;
    config.streams.connection_max_window = 256U * 1024U;
    config.datagrams.max_frame_bytes = 1200U;
    config.datagrams.receive_bytes = 8192U;
    config.datagrams.receive_count = 32U;
    config.datagrams.send_bytes = 8192U;
    config.datagrams.send_count = 32U;
    config.datagrams.outcomes = true;
    config.idle_timeout_ns = idle_ms * kMsNs;
    return config;
}

// The contexts: a client asking for http/1.1, one asking for the QMux identifier, and a server listing both.
struct WsIdentity
{
    crypto::SigningKey key;
    crypto::TlsCredential credential;
    crypto::TlsContext client;
    crypto::TlsContext qmux_client;
    crypto::TlsContext server;

    explicit WsIdentity(crypto::ReplayFilter* early = nullptr)
    {
        u8 seed[32] = {7U, 7U, 7U};
        REQUIRE(key.from_seed(crypto::AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) ==
                crypto::CryptoError::None);
        REQUIRE(credential.init(key, kServerName) == TlsError::None);
        const crypto::KeyId pin = credential.key_id();
        crypto::TlsConfig tls;
        tls.role = crypto::TlsRole::Client;
        tls.groups = kGroups;
        tls.signatures = kSignatures;
        tls.aeads = kAeads;
        tls.pinned = ConstSpan<crypto::KeyId>(&pin, 1U);
        tls.server_name = kServerName;
        tls.application_protocols = kHttp11Protocols;
        tls.early_data = early != nullptr;
        REQUIRE(client.init(tls) == TlsError::None);
        tls.application_protocols = kProtocols;
        tls.early_data = false;
        REQUIRE(qmux_client.init(tls) == TlsError::None);
        crypto::TlsConfig answer;
        answer.role = crypto::TlsRole::Server;
        answer.groups = kGroups;
        answer.aeads = kAeads;
        answer.credential = &credential;
        answer.server_name = kServerName;
        answer.application_protocols = kBothProtocols;
        if (early != nullptr)
        {
            answer.ticket_key = ConstSpan<u8>(kTicketKey);
            answer.early_data = true;
            answer.early_data_filter = early;
        }
        REQUIRE(server.init(answer) == TlsError::None);
    }
};

void pump(QmuxConnection& a, QmuxConnection& b, u64 now, usize chunk = kBuffer)
{
    u8 bytes[kBuffer];
    for (bool moved = true; moved;)
    {
        moved = false;
        for (QmuxConnection* from : {&a, &b})
        {
            QmuxConnection& to = (from == &a) ? b : a;
            for (usize got = from->take(Span<u8>(bytes), now); got != 0U; got = from->take(Span<u8>(bytes), now))
            {
                moved = true;
                for (usize at = 0U; at < got; at += chunk)
                {
                    to.receive(ConstSpan<u8>(bytes + at, (got - at < chunk) ? got - at : chunk), now);
                }
            }
        }
    }
}

struct Pair
{
    WsIdentity identity;
    CountingAllocator allocator;
    Policy policy;
    QmuxConnection client;
    QmuxConnection server;

    Pair()
    {
        REQUIRE(client.init(allocator, qmux_config(identity.client, &policy.config)));
        REQUIRE(server.init(allocator, qmux_config(identity.server, &policy.config)));
    }

    void connect(usize chunk = kBuffer)
    {
        REQUIRE(client.start(kNow));
        REQUIRE(server.start(kNow));
        pump(client, server, kNow, chunk);
        REQUIRE(client.state() == QmuxState::Established);
        REQUIRE(server.state() == QmuxState::Established);
    }
};

struct ServerFrame
{
    WsOpcode opcode = WsOpcode::Continuation;
    bool fin = false;
    Bytes payload;
};

Bytes frame_of(WsOpcode opcode, bool fin, ConstSpan<u8> payload, bool masked = true)
{
    constexpr u8 mask[kWsMaskBytes] = {0x11U, 0x22U, 0x33U, 0x44U};
    const usize head = ws_frame_header_bytes(payload.size(), masked);
    Bytes bytes;
    bytes.resize(head + payload.size());
    ws_write_frame_header(Span<u8>(bytes.data(), head), opcode, fin, payload.size(), masked ? mask : nullptr);
    if (!payload.empty())
    {
        std::memcpy(bytes.data() + head, payload.data(), payload.size());
    }
    if (masked)
    {
        ws_mask(Span<u8>(bytes.data() + head, payload.size()), mask);
    }
    return bytes;
}

// The transport parameters frame that must open a peer's records, with no parameters.
Bytes parameters_frame()
{
    Bytes frame;
    frame.resize(16U);
    WireWriter writer{Span<u8>(frame.data(), frame.size())};
    put_quic_varint(writer, iana::kQuicFrameQxTransportParameters);
    put_quic_varint(writer, 0U);
    frame.resize(writer.written());
    return frame;
}

Bytes close_payload(u16 code, const char* reason = "")
{
    Bytes bytes = bytes_of({static_cast<u8>(code >> 8U), static_cast<u8>(code & 0xFFU)});
    append(bytes, span_of(reason));
    return bytes;
}

// A hand-driven TLS client of a WebSocket server: everything a well-behaved client never sends.
struct RawClient
{
    WsIdentity identity;
    CountingAllocator allocator;
    Policy policy;
    TlsSession peer;
    QmuxConnection server;
    u64 now = kNow;
    Bytes plain;
    usize frames_at = 0U;

    explicit RawClient(u64 idle_ms = 30000U)
    {
        REQUIRE(server.init(allocator, qmux_config(identity.server, &policy.config, idle_ms)));
        REQUIRE(server.start(now));
        REQUIRE(peer.init(identity.client) == TlsError::None);
        flush();
        REQUIRE(peer.state() == TlsState::Established);
        REQUIRE(server.state() == QmuxState::Handshaking);
        REQUIRE(server.websocket());
    }

    void flush()
    {
        u8 bytes[kBuffer];
        for (bool moved = true; moved;)
        {
            moved = false;
            for (usize got = 1U; got != 0U;)
            {
                REQUIRE(peer.take(Span<u8>(bytes), got) == TlsError::None);
                if (got != 0U)
                {
                    moved = true;
                    server.receive(ConstSpan<u8>(bytes, got), now);
                }
            }
            for (usize out = server.take(Span<u8>(bytes), now); out != 0U; out = server.take(Span<u8>(bytes), now))
            {
                moved = true;
                const TlsError error = peer.receive(ConstSpan<u8>(bytes, out));
                REQUIRE((error == TlsError::None || error == TlsError::Closed));
            }
        }
        for (usize got = 1U; got != 0U;)
        {
            const TlsError error = peer.read(Span<u8>(bytes), got);
            REQUIRE((error == TlsError::None || error == TlsError::Closed));
            got = (error == TlsError::None) ? got : 0U;
            append(plain, ConstSpan<u8>(bytes, got));
        }
    }

    void send(ConstSpan<u8> bytes)
    {
        REQUIRE(peer.send(bytes) == TlsError::None);
        flush();
    }

    void send(const Bytes& bytes) { send(view(bytes)); }

    // The opening handshake, leaving the server's frames to read.
    void upgrade()
    {
        send(head_of(kRequest));
        REQUIRE(server.state() == QmuxState::Established);
        REQUIRE(plain.size() > 12U);
        REQUIRE(same(ConstSpan<u8>(plain.data(), 12U), span_of("HTTP/1.1 101")));
        for (frames_at = 4U; std::memcmp(plain.data() + frames_at - 4U, "\r\n\r\n", 4U) != 0; ++frames_at)
        {
            REQUIRE(frames_at < plain.size());
        }
    }

    void opened()
    {
        upgrade();
        send(frame_of(WsOpcode::Binary, true, view(parameters_frame())));
        REQUIRE(server.peer_parameters_known());
    }

    // What the server has framed since the last call.
    crd::containers::Array<ServerFrame> frames()
    {
        crd::containers::Array<ServerFrame> out;
        while (frames_at < plain.size())
        {
            WsFrameHeader header;
            u16 failure = 0U;
            REQUIRE(ws_read_frame_header(ConstSpan<u8>(plain.data() + frames_at, plain.size() - frames_at), false,
                                         kQmuxMaxRecord, header, failure) == HttpDecode::Ok);
            ServerFrame frame;
            frame.opcode = header.opcode;
            frame.fin = header.fin;
            frame.payload =
                bytes_of(ConstSpan<u8>(plain.data() + frames_at + header.header_bytes, header.payload_bytes));
            frames_at += header.header_bytes + header.payload_bytes;
            out.push_back(frame);
        }
        return out;
    }

    // The status of the Close frame that ends what the server said; 0 when it has not sent one.
    u16 close_status()
    {
        const auto said = frames();
        if (said.empty() || said[said.size() - 1U].opcode != WsOpcode::Close ||
            said[said.size() - 1U].payload.size() < 2U)
        {
            return 0U;
        }
        const Bytes& payload = said[said.size() - 1U].payload;
        return static_cast<u16>((u16{payload[0]} << 8U) | payload[1]);
    }
};

// A hand-driven TLS server of a WebSocket client.
struct RawServer
{
    WsIdentity identity;
    CountingAllocator allocator;
    Policy policy;
    TlsSession peer;
    QmuxConnection client;
    u64 now = kNow;
    Bytes plain;

    RawServer()
    {
        REQUIRE(client.init(allocator, qmux_config(identity.client, &policy.config)));
        REQUIRE(client.start(now));
        REQUIRE(peer.init(identity.server, nullptr, {}, ConstSpan<u8>()) == TlsError::None);
        flush();
        REQUIRE(peer.state() == TlsState::Established);
        REQUIRE(client.websocket());
        REQUIRE(client.state() == QmuxState::Handshaking);
    }

    void flush()
    {
        u8 bytes[kBuffer];
        for (bool moved = true; moved;)
        {
            moved = false;
            for (usize out = client.take(Span<u8>(bytes), now); out != 0U; out = client.take(Span<u8>(bytes), now))
            {
                moved = true;
                const TlsError error = peer.receive(ConstSpan<u8>(bytes, out));
                REQUIRE((error == TlsError::None || error == TlsError::Closed));
            }
            for (usize got = 1U; got != 0U;)
            {
                REQUIRE(peer.take(Span<u8>(bytes), got) == TlsError::None);
                if (got != 0U)
                {
                    moved = true;
                    client.receive(ConstSpan<u8>(bytes, got), now);
                }
            }
        }
        for (usize got = 1U; got != 0U;)
        {
            const TlsError error = peer.read(Span<u8>(bytes), got);
            REQUIRE((error == TlsError::None || error == TlsError::Closed || error == TlsError::NotReady));
            got = (error == TlsError::None) ? got : 0U;
            append(plain, ConstSpan<u8>(bytes, got));
        }
    }

    void send(const Bytes& bytes)
    {
        REQUIRE(peer.send(view(bytes)) == TlsError::None);
        flush();
    }

    // The 101 for the request the client sent.
    Bytes answer()
    {
        WebSocketConfig config = policy.config;
        usize scanned = 0U;
        WsHandshake handshake;
        REQUIRE(ws_read_request(view(plain), config, scanned, handshake) == HttpDecode::Ok);
        REQUIRE(handshake.status == 101U);
        u8 out[kWsMaxResponseBytes];
        const usize used = ws_write_response(Span<u8>(out), config, handshake);
        plain = bytes_of(ConstSpan<u8>(plain.data() + handshake.head_bytes, plain.size() - handshake.head_bytes));
        return bytes_of(ConstSpan<u8>(out, used));
    }
};

// A server and a client host over one pipe network; the server's context lists both protocols.
struct Recorder final : QmuxHostEvents
{
    void on_accept(QuicHandle, QmuxConnection&, const Endpoint&) noexcept override {}

    void on_service(QuicHandle, QmuxConnection& connection, u64) noexcept override
    {
        if (connection.state() != QmuxState::Established || !connection.peer_parameters_known())
        {
            return;
        }
        if (client && !opened)
        {
            u64 id = 0U;
            opened = connection.streams().open(true, id) == QuicStreamError::None;
        }
        opened = opened || connection.streams().recv_state(0U) != QuicRecvState::None;
        if (opened && written < transfer)
        {
            static_cast<void>(write_pattern(connection.streams(), 0U, written, transfer, client ? 3U : 5U));
        }
        if (opened && !finished)
        {
            finished = drain(connection.streams(), 0U, read, client ? 5U : 3U);
        }
    }

    void on_closed(QuicHandle, const QmuxConnection& connection, SocketError) noexcept override
    {
        ended = true;
        info = connection.close_info();
        websocket = connection.websocket();
    }

    bool client = false;
    u64 transfer = 0U;
    bool opened = false;
    bool finished = false;
    bool ended = false;
    bool websocket = false;
    u64 written = 0U;
    u64 read = 0U;
    QmuxCloseInfo info;
};

} // namespace

TEST_CASE("legacy SHA-1 matches FIPS 180-4's examples", "[net][websocket]")
{
    u8 digest[crypto::kSha1Bytes];
    crypto::legacy_sha1(span_of("abc"), Span<u8, crypto::kSha1Bytes>(digest, crypto::kSha1Bytes));
    constexpr u8 expected[crypto::kSha1Bytes] = {0xA9U, 0x99U, 0x3EU, 0x36U, 0x47U, 0x06U, 0x81U, 0x6AU, 0xBAU, 0x3EU,
                                                 0x25U, 0x71U, 0x78U, 0x50U, 0xC2U, 0x6CU, 0x9CU, 0xD0U, 0xD8U, 0x9DU};
    CHECK(std::memcmp(digest, expected, sizeof(expected)) == 0);
}

TEST_CASE("WebSocket: RFC 6455 section 1.3's key and accept", "[net][websocket]")
{
    u8 accept[kWsAcceptChars];
    ws_accept_of(span_of(kSampleKey), Span<u8, kWsAcceptChars>(accept, kWsAcceptChars));
    CHECK(same(ConstSpan<u8>(accept, kWsAcceptChars), span_of(kSampleAccept)));
    constexpr u8 nonce[kWsKeyBytes] = {'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};
    u8 key[kWsKeyChars];
    ws_key_of(nonce, Span<u8, kWsKeyChars>(key, kWsKeyChars));
    CHECK(same(ConstSpan<u8>(key, kWsKeyChars), span_of(kSampleKey)));
}

TEST_CASE("WebSocket: section 5.7's frames read and written", "[net][websocket]")
{
    struct Example
    {
        Bytes bytes;
        WsOpcode opcode;
        bool fin;
        bool masked;
        usize header;
        usize payload;
    };
    const Example examples[] = {
        {bytes_of({0x81U, 0x05U, 0x48U, 0x65U, 0x6CU, 0x6CU, 0x6FU}), WsOpcode::Text, true, false, 2U, 5U},
        {bytes_of({0x81U, 0x85U, 0x37U, 0xFAU, 0x21U, 0x3DU, 0x7FU, 0x9FU, 0x4DU, 0x51U, 0x58U}), WsOpcode::Text, true,
         true, 6U, 5U},
        {bytes_of({0x01U, 0x03U, 0x48U, 0x65U, 0x6CU}), WsOpcode::Text, false, false, 2U, 3U},
        {bytes_of({0x80U, 0x02U, 0x6CU, 0x6FU}), WsOpcode::Continuation, true, false, 2U, 2U},
        {bytes_of({0x89U, 0x05U, 0x48U, 0x65U, 0x6CU, 0x6CU, 0x6FU}), WsOpcode::Ping, true, false, 2U, 5U},
        {bytes_of({0x82U, 0x7EU, 0x01U, 0x00U}), WsOpcode::Binary, true, false, 4U, 256U},
        {bytes_of({0x82U, 0x7FU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U}), WsOpcode::Binary, true,
         false, 10U, 65536U},
    };
    for (const Example& example : examples)
    {
        WsFrameHeader header;
        u16 failure = 77U;
        REQUIRE(ws_read_frame_header(view(example.bytes), example.masked, 1U << 20U, header, failure) ==
                HttpDecode::Ok);
        CHECK(failure == 0U);
        CHECK(header.opcode == example.opcode);
        CHECK(header.fin == example.fin);
        CHECK(header.masked == example.masked);
        CHECK(header.header_bytes == example.header);
        CHECK(header.payload_bytes == example.payload);
        CHECK(ws_frame_header_bytes(example.payload, example.masked) == example.header);
        Bytes written;
        written.resize(example.header);
        ws_write_frame_header(Span<u8>(written.data(), written.size()), example.opcode, example.fin, example.payload,
                              example.masked ? header.mask : nullptr);
        CHECK(same(view(written), ConstSpan<u8>(example.bytes.data(), example.header)));
    }
    // The masked example's payload unmasks to "Hello".
    const u8 mask[kWsMaskBytes] = {0x37U, 0xFAU, 0x21U, 0x3DU};
    u8 masked[] = {0x7FU, 0x9FU, 0x4DU, 0x51U, 0x58U};
    ws_mask(Span<u8>(masked, sizeof(masked)), mask);
    CHECK(same(ConstSpan<u8>(masked, sizeof(masked)), span_of("Hello")));
}

TEST_CASE("WebSocket: every header section 5 refuses is refused", "[net][websocket]")
{
    struct Refused
    {
        Bytes bytes;
        bool from_client;
        u16 failure;
    };
    const Refused refused[] = {
        {bytes_of({0xC2U, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // RSV1
        {bytes_of({0xA2U, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // RSV2
        {bytes_of({0x92U, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // RSV3
        {bytes_of({0x83U, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // opcode 3, reserved
        {bytes_of({0x87U, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // opcode 7
        {bytes_of({0x8BU, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // opcode B
        {bytes_of({0x8FU, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // opcode F
        {bytes_of({0x82U, 0x00U}), true, kWsCloseProtocolError},                           // a client's frame unmasked
        {bytes_of({0x82U, 0x80U, 1U, 2U, 3U, 4U}), false, kWsCloseProtocolError},          // a server's frame masked
        {bytes_of({0x09U, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // a fragmented Ping
        {bytes_of({0x08U, 0x80U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},           // a fragmented Close
        {bytes_of({0x89U, 0xFEU, 0U, 126U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError}, // a control frame over 125
        {bytes_of({0x82U, 0xFEU, 0U, 125U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError}, // 16 bits where 7 would do
        {bytes_of({0x82U, 0xFFU, 0U, 0U, 0U, 0U, 0U, 0U, 0xFFU, 0xFFU, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},
        {bytes_of({0x82U, 0xFFU, 0x80U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 2U, 3U, 4U}), true, kWsCloseProtocolError},
        {bytes_of({0x82U, 0xFEU, 0x03U, 0xE9U, 1U, 2U, 3U, 4U}), true,
         kWsCloseTooBig}, // 1001 bytes over a limit of 1000
    };
    for (const Refused& row : refused)
    {
        WsFrameHeader header;
        u16 failure = 0U;
        CHECK(ws_read_frame_header(view(row.bytes), row.from_client, 1000U, header, failure) == HttpDecode::Invalid);
        CHECK(failure == row.failure);
    }
}

TEST_CASE("WebSocket: a header waits for all of its bytes, and a data frame may be exactly its limit",
          "[net][websocket]")
{
    const Bytes whole = bytes_of({0x82U, 0xFFU, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 1U, 2U, 3U, 4U});
    WsFrameHeader header;
    u16 failure = 0U;
    for (usize size = 0U; size < whole.size(); ++size)
    {
        CHECK(ws_read_frame_header(ConstSpan<u8>(whole.data(), size), true, 1U << 20U, header, failure) ==
              HttpDecode::NeedMore);
    }
    REQUIRE(ws_read_frame_header(view(whole), true, 65536U, header, failure) == HttpDecode::Ok);
    CHECK(header.payload_bytes == 65536U);
    CHECK(header.mask[2] == 3U);
    CHECK(ws_read_frame_header(view(whole), true, 65535U, header, failure) == HttpDecode::Invalid);
    CHECK(failure == kWsCloseTooBig);
}

TEST_CASE("WebSocket: masking is its own inverse at every length and matches section 5.3", "[net][websocket]")
{
    const u8 mask[kWsMaskBytes] = {0xA1U, 0x02U, 0xF3U, 0x44U};
    for (usize size = 0U; size <= 64U; ++size)
    {
        Bytes bytes;
        bytes.resize(size);
        for (usize at = 0U; at < size; ++at)
        {
            bytes[at] = static_cast<u8>(at * 13U);
        }
        const Bytes original = bytes;
        ws_mask(Span<u8>(bytes.data(), bytes.size()), mask);
        for (usize at = 0U; at < size; ++at)
        {
            CHECK(bytes[at] == static_cast<u8>(original[at] ^ mask[at % 4U]));
        }
        ws_mask(Span<u8>(bytes.data(), bytes.size()), mask);
        CHECK(same(view(bytes), view(original)));
    }
}

TEST_CASE("WebSocket: Close codes and payloads", "[net][websocket]")
{
    for (const u16 code :
         {u16{1000U}, u16{1001U}, u16{1002U}, u16{1003U}, u16{1007U}, u16{1008U}, u16{1009U}, u16{1010U}, u16{1011U},
          u16{1012U}, u16{1014U}, u16{3000U}, u16{3999U}, u16{4000U}, u16{4999U}})
    {
        CHECK(ws_close_code_valid(code));
    }
    for (const u16 code : {u16{0U}, u16{999U}, u16{1004U}, u16{1005U}, u16{1006U}, u16{1015U}, u16{1016U}, u16{2999U},
                           u16{5000U}, u16{65535U}})
    {
        CHECK_FALSE(ws_close_code_valid(code));
    }
    u16 code = 0U;
    ConstSpan<u8> reason;
    CHECK(ws_read_close(ConstSpan<u8>(), code, reason) == 0U);
    CHECK(code == kWsCloseNoStatus);
    CHECK(ws_read_close(view(bytes_of({0x03U})), code, reason) == kWsCloseProtocolError);
    CHECK(ws_read_close(view(close_payload(1005U)), code, reason) == kWsCloseProtocolError);
    const Bytes good = close_payload(1000U, "gone");
    CHECK(ws_read_close(view(good), code, reason) == 0U);
    CHECK(code == 1000U);
    CHECK(same(reason, span_of("gone")));
    Bytes invalid = close_payload(1000U);
    append(invalid, view(bytes_of({0xC0U, 0x80U}))); // overlong
    CHECK(ws_read_close(view(invalid), code, reason) == kWsCloseInvalidPayload);
    Bytes truncated = close_payload(1000U);
    append(truncated, view(bytes_of({0xE2U, 0x82U})));
    CHECK(ws_read_close(view(truncated), code, reason) == kWsCloseInvalidPayload);
    Bytes multibyte = close_payload(1000U);
    append(multibyte, view(bytes_of({0xE2U, 0x82U, 0xACU})));
    CHECK(ws_read_close(view(multibyte), code, reason) == 0U);
}

TEST_CASE("WebSocket: a good request is answered 101 with the section 4.2.2 fields", "[net][websocket]")
{
    const Policy policy;
    const Bytes request = head_of(kRequest);
    usize scanned = 0U;
    WsHandshake handshake;
    REQUIRE(ws_read_request(view(request), policy.config, scanned, handshake) == HttpDecode::Ok);
    CHECK(handshake.status == 101U);
    CHECK(handshake.head_bytes == request.size());
    CHECK(handshake.protocol == 0U);
    u8 out[kWsMaxResponseBytes];
    const usize used = ws_write_response(Span<u8>(out), policy.config, handshake);
    CHECK(same(ConstSpan<u8>(out, used),
               span_of("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                       "Sec-WebSocket-Protocol: qmux-02.game\r\n\r\n")));
}

TEST_CASE("WebSocket: every request the server should take", "[net][websocket]")
{
    CHECK(status_of(head_of(kRequest, "upgrade", "UPGRADE:   WebSocket  \r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "upgrade", "Upgrade: h2c, websocket\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "connection", "connection: keep-alive, Upgrade\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "connection", "Connection: keep-alive\r\nConnection: upgrade\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "start", "GET /game?token=abc HTTP/1.1\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "start", "GET https://game.cerid.test/game HTTP/1.1\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "start", "GET wss://game.cerid.test:443/game?x HTTP/1.1\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "", "", "Content-Length: 0\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "", "", "Sec-WebSocket-Extensions: permessage-deflate\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "", "", "Origin: https://www.game.example\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "", "", "Cookie: a=b;\t c=d\r\n")) == 101U);
    CHECK(status_of(head_of(kRequest, "protocol", "Sec-WebSocket-Protocol: unknown, qmux-02.game\r\n")) == 101U);
}

TEST_CASE("WebSocket: the server's own order chooses the subprotocol", "[net][websocket]")
{
    const Policy policy;
    const Bytes request = head_of(kRequest, "protocol",
                                  "Sec-WebSocket-Protocol: qmux-01.game\r\nSec-WebSocket-Protocol: junk, other\r\n");
    usize scanned = 0U;
    WsHandshake handshake;
    REQUIRE(ws_read_request(view(request), policy.config, scanned, handshake) == HttpDecode::Ok);
    CHECK(handshake.status == 101U);
    CHECK(handshake.protocol == 1U);
}

TEST_CASE("WebSocket: every request the server should refuse, by status", "[net][websocket]")
{
    struct Refused
    {
        const char* key;
        const char* text;
        const char* extra;
        u16 status;
    };
    const Refused refused[] = {
        // RFC 9112's grammar.
        {"start", "GET  /game HTTP/1.1\r\n", "", 400U},
        {"start", "GET /game  HTTP/1.1\r\n", "", 400U},
        {"start", "GET /game\r\n", "", 400U},
        {"start", "GET /game HTTP/1\r\n", "", 400U},
        {"start", "G@T /game HTTP/1.1\r\n", "", 400U},
        {"start", "GET /ga\tme HTTP/1.1\r\n", "", 400U},
        {"host", "Host : game.cerid.test\r\n", "", 400U},
        {"host", " Host: game.cerid.test\r\n", "", 400U},
        {"host", "Host: a\r\n b\r\n", "", 400U},
        {"host", "Host: game\ncerid\r\n", "", 400U},
        {"host", "Host: game\rcerid\r\n", "", 400U},
        {"", "", "X: a\rBC: d\r\n", 400U},
        {"host", "Host game.cerid.test\r\n", "", 400U},
        {"", "", "X: a\x01z\r\n", 400U},
        {"", "", "X: a\x7Fz\r\n", 400U},
        // Versions and methods.
        {"start", "GET /game HTTP/1.0\r\n", "", 505U},
        {"start", "GET /game HTTP/2.0\r\n", "", 505U},
        {"start", "POST /game HTTP/1.1\r\n", "", 405U},
        {"start", "HEAD /game HTTP/1.1\r\n", "", 405U},
        // Bodies and what could smuggle one.
        {"", "", "Content-Length: 5\r\n", 400U},
        {"", "", "Content-Length: 0\r\nContent-Length: 0\r\n", 400U},
        {"", "", "Transfer-Encoding: chunked\r\n", 400U},
        // The upgrade itself.
        {"upgrade", nullptr, "", 426U},
        {"upgrade", "Upgrade: h2c\r\n", "", 426U},
        {"upgrade", "Upgrade: websocket2\r\n", "", 426U},
        {"connection", nullptr, "", 426U},
        {"connection", "Connection: close\r\n", "", 426U},
        {"version", nullptr, "", 426U},
        {"version", "Sec-WebSocket-Version: 8\r\n", "", 426U},
        {"version", "Sec-WebSocket-Version: 13, 8\r\n", "", 426U},
        {"version", "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Version: 13\r\n", "", 426U},
        // The key.
        {"key", nullptr, "", 400U},
        {"key", "Sec-WebSocket-Key: \r\n", "", 400U},
        {"key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=\r\n", "", 400U},
        {"key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ===\r\n", "", 400U},
        {"key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZ!==\r\n", "", 400U},
        {"key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n", "", 400U}, // trailing bits set
        {"key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZTE=\r\n", "", 400U}, // 17 bytes
        {"key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQE=\r\n", "", 400U}, // padded after more than 16 bytes
        {"key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQAA\r\n", "", 400U}, // unpadded
        {"", "", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n", 400U},
        // Host.
        {"host", nullptr, "", 400U},
        {"host", "Host:\r\n", "", 400U},
        {"", "", "Host: other\r\n", 400U},
        // The resource.
        {"start", "GET /other HTTP/1.1\r\n", "", 404U},
        {"start", "GET /game/ HTTP/1.1\r\n", "", 404U},
        {"start", "GET /GAME HTTP/1.1\r\n", "", 404U},
        {"start", "GET /game#x HTTP/1.1\r\n", "", 400U},
        {"start", "GET game HTTP/1.1\r\n", "", 400U},
        {"start", "GET * HTTP/1.1\r\n", "", 400U},
        {"start", "GET ftp://x/game HTTP/1.1\r\n", "", 400U},
        {"start", "GET https:///game HTTP/1.1\r\n", "", 400U},
        // The browser policy.
        {"", "", "Origin: https://evil.example\r\n", 403U},
        {"", "", "Origin: https://GAME.example\r\n", 403U},
        {"", "", "Origin: https://game.example\r\nOrigin: https://game.example\r\n", 400U},
        // The subprotocol.
        {"protocol", nullptr, "", 400U},
        {"protocol", "Sec-WebSocket-Protocol: unknown\r\n", "", 400U},
        {"protocol", "Sec-WebSocket-Protocol: QMUX-02.GAME\r\n", "", 400U},
    };
    for (const Refused& row : refused)
    {
        CAPTURE(row.key, row.text, row.extra);
        CHECK(status_of(head_of(kRequest, row.key, row.text, row.extra)) == row.status);
    }
}

TEST_CASE("WebSocket: any Origin and an empty list", "[net][websocket]")
{
    Policy policy;
    const Bytes request = head_of(kRequest, "", "", "Origin: https://anywhere.example\r\n");
    CHECK(status_of(request, policy.config) == 403U);
    policy.config.any_origin = true;
    CHECK(status_of(request, policy.config) == 101U);
    policy.config.any_origin = false;
    policy.config.origins = {};
    CHECK(status_of(request, policy.config) == 403U);
    CHECK(status_of(head_of(kRequest), policy.config) == 101U); // no Origin: not a browser
}

TEST_CASE("WebSocket: a head's size and count of fields are bounded", "[net][websocket]")
{
    Bytes fields;
    for (usize index = 0U; index < kWsMaxHeadFields - 6U; ++index) // the request has six of its own
    {
        append(fields, span_of("X-A: b\r\n"));
    }
    const Policy policy;
    Bytes request = head_of(kRequest);
    request.resize(request.size() - 2U);
    append(request, view(fields));
    Bytes one_more = request;
    append(one_more, span_of("X-A: b\r\n\r\n"));
    append(request, span_of("\r\n"));
    CHECK(status_of(request, policy.config) == 101U);
    CHECK(status_of(one_more, policy.config) == 431U);

    // Without an end, and with an end past the limit.
    Bytes endless;
    endless.resize(kWsMaxHeadBytes, static_cast<u8>('a'));
    CHECK(status_of(endless, policy.config) == 431U);
    Bytes late = bytes_of("GET /game HTTP/1.1\r\nX: ");
    late.resize(kWsMaxHeadBytes, static_cast<u8>('b'));
    append(late, span_of("\r\n\r\n"));
    CHECK(status_of(late, policy.config) == 431U);

    Bytes justfits = bytes_of("GET /game HTTP/1.1\r\n");
    const Bytes rest = head_of(kRequest, "start", nullptr);
    const usize filler = kWsMaxHeadBytes - justfits.size() - rest.size() - 4U - 2U;
    append(justfits, span_of("X: "));
    const usize at = justfits.size();
    justfits.resize(at + filler - 1U, static_cast<u8>('c'));
    append(justfits, span_of("\r\n"));
    append(justfits, view(rest));
    CHECK(justfits.size() <= kWsMaxHeadBytes);
    CHECK(status_of(justfits, policy.config) == 101U);
}

TEST_CASE("WebSocket: a request arriving a byte at a time waits, and its search does not start over",
          "[net][websocket]")
{
    const Policy policy;
    const Bytes request = head_of(kRequest);
    usize scanned = 0U;
    WsHandshake handshake;
    for (usize size = 0U; size < request.size(); ++size)
    {
        CHECK(ws_read_request(ConstSpan<u8>(request.data(), size), policy.config, scanned, handshake) ==
              HttpDecode::NeedMore);
        CHECK(scanned == size);
    }
    REQUIRE(ws_read_request(view(request), policy.config, scanned, handshake) == HttpDecode::Ok);
    CHECK(handshake.status == 101U);
    // Bytes after the head are not part of it.
    Bytes more = request;
    append(more, span_of("trailing"));
    usize again = 0U;
    REQUIRE(ws_read_request(view(more), policy.config, again, handshake) == HttpDecode::Ok);
    CHECK(handshake.head_bytes == request.size());
}

TEST_CASE("WebSocket: refusals carry what RFC 9110 and RFC 6455 ask", "[net][websocket]")
{
    const Policy policy;
    const auto answer = [&policy](u16 status)
    {
        WsHandshake handshake;
        handshake.status = status;
        u8 out[kWsMaxResponseBytes];
        const usize used = ws_write_response(Span<u8>(out), policy.config, handshake);
        return bytes_of(ConstSpan<u8>(out, used));
    };
    CHECK(holds(answer(400U), "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
    CHECK(holds(answer(403U), "HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
    CHECK(holds(answer(404U), "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
    CHECK(holds(answer(405U),
                "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\nAllow: GET\r\nContent-Length: 0\r\n\r\n"));
    CHECK(holds(answer(426U), "HTTP/1.1 426 Upgrade Required\r\nUpgrade: websocket\r\nConnection: Upgrade, close\r\n"
                              "Sec-WebSocket-Version: 13\r\nContent-Length: 0\r\n\r\n"));
    CHECK(holds(answer(431U),
                "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
    CHECK(holds(answer(505U),
                "HTTP/1.1 505 HTTP Version Not Supported\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
}

TEST_CASE("WebSocket: a client's request passes the server's reading", "[net][websocket]")
{
    Policy policy;
    policy.config.path = span_of("/game?room=7");
    policy.config.origin = span_of("https://game.example");
    u8 out[kWsMaxHeadBytes];
    const usize used = ws_write_request(Span<u8>(out), policy.config, span_of(kSampleKey));
    CHECK(same(ConstSpan<u8>(out, used),
               span_of("GET /game?room=7 HTTP/1.1\r\nHost: game.cerid.test\r\nUpgrade: websocket\r\n"
                       "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "Sec-WebSocket-Version: 13\r\nOrigin: https://game.example\r\n"
                       "Sec-WebSocket-Protocol: qmux-02.game, qmux-01.game, other\r\n\r\n")));
    policy.config.path = span_of(kPath);
    CHECK(status_of(bytes_of(ConstSpan<u8>(out, used)), policy.config) == 101U);
}

TEST_CASE("WebSocket: every response a client should take or refuse", "[net][websocket]")
{
    CHECK(response_status(head_of(kResponse)) == 101U);
    CHECK(response_status(head_of(kResponse, "upgrade", "upgrade:WEBSOCKET\r\n")) == 101U);
    CHECK(response_status(head_of(kResponse, "connection", "Connection: foo, upgrade\r\n")) == 101U);
    CHECK(response_status(head_of(kResponse, "start", "HTTP/1.1 101 \r\n")) == 101U);
    CHECK(response_status(head_of(kResponse, "", "", "X-Other: y\r\n")) == 101U);

    // Another status is the server's answer, whatever else it says.
    CHECK(response_status(head_of(kResponse, "start", "HTTP/1.1 403 Forbidden\r\n")) == 403U);
    CHECK(response_status(head_of(kResponse, "start", "HTTP/1.1 200 OK\r\n")) == 200U);
    CHECK(response_status(head_of(kResponse, "start", "HTTP/1.1 302 Found\r\n")) == 302U);

    // Refused: status 0.
    struct Refused
    {
        const char* key;
        const char* text;
        const char* extra;
    };
    const Refused refused[] = {
        {"start", "HTTP/1.1 101\r\n", ""},
        {"start", "HTTP/1.1 1O1 x\r\n", ""},
        {"start", "http/1.1 101 x\r\n", ""},
        {"start", "HTTP/1.1  101 x\r\n", ""},
        {"upgrade", nullptr, ""},
        {"upgrade", "Upgrade: h2c\r\n", ""},
        {"connection", nullptr, ""},
        {"connection", "Connection: close\r\n", ""},
        {"accept", nullptr, ""},
        {"accept", "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n", ""},
        {"", "", "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"},
        {"protocol", nullptr, ""},
        {"protocol", "Sec-WebSocket-Protocol: unknown\r\n", ""},
        {"protocol", "Sec-WebSocket-Protocol: qmux-02.game, other\r\n", ""},
        {"", "", "Sec-WebSocket-Protocol: other\r\n"},
        {"", "", "Sec-WebSocket-Extensions: permessage-deflate\r\n"},
        {"", "", "Bad Name: x\r\n"},
    };
    for (const Refused& row : refused)
    {
        CAPTURE(row.key, row.text, row.extra);
        CHECK(response_status(head_of(kResponse, row.key, row.text, row.extra)) == 0U);
    }

    Bytes endless;
    endless.resize(kWsMaxHeadBytes, static_cast<u8>('a'));
    CHECK(response_status(endless) == 0U);
}

TEST_CASE("WebSocket: a configuration is checked for its side", "[net][websocket]")
{
    Policy policy;
    CHECK(ws_config_valid(policy.config, true));
    CHECK(ws_config_valid(policy.config, false));
    WebSocketConfig config = policy.config;
    config.protocols = {};
    CHECK_FALSE(ws_config_valid(config, false));
    config = policy.config;
    const ConstSpan<u8> many[kWsMaxProtocols + 1U] = {};
    config.protocols = ConstSpan<ConstSpan<u8>>(many, kWsMaxProtocols + 1U);
    CHECK_FALSE(ws_config_valid(config, false));
    config = policy.config;
    const ConstSpan<u8> spaced[] = {span_of("two words")};
    config.protocols = ConstSpan<ConstSpan<u8>>(spaced, 1U);
    CHECK_FALSE(ws_config_valid(config, false));
    config = policy.config;
    Bytes long_token;
    long_token.resize(kWsMaxProtocolBytes + 1U, static_cast<u8>('x'));
    const ConstSpan<u8> longest[] = {view(long_token)};
    config.protocols = ConstSpan<ConstSpan<u8>>(longest, 1U);
    CHECK_FALSE(ws_config_valid(config, false));
    config = policy.config;
    config.path = span_of("game");
    CHECK_FALSE(ws_config_valid(config, false));
    config.path = {};
    CHECK_FALSE(ws_config_valid(config, false));
    config.path = span_of("/a b");
    CHECK_FALSE(ws_config_valid(config, false));
    config.path = span_of("/a?b");
    CHECK_FALSE(ws_config_valid(config, false)); // a server compares paths only
    CHECK(ws_config_valid(config, true));
    config = policy.config;
    config.host = {};
    CHECK_FALSE(ws_config_valid(config, true));
    CHECK(ws_config_valid(config, false));
    config = policy.config;
    config.host = span_of("a b");
    CHECK_FALSE(ws_config_valid(config, true));
    config = policy.config;
    config.origin = span_of("bad\r\ninjected");
    CHECK_FALSE(ws_config_valid(config, true));
    config = policy.config;
    Bytes big;
    big.resize(kWsMaxHeadBytes, static_cast<u8>('h'));
    config.host = view(big);
    CHECK_FALSE(ws_config_valid(config, true));
}

TEST_CASE("WebSocket: QMux's connection refuses what it cannot run", "[net][websocket]")
{
    WsIdentity identity;
    ReadyFilter ready;
    WsIdentity early(&ready.filter);
    CountingAllocator allocator;
    Policy policy;
    {
        QmuxConnection connection;
        CHECK_FALSE(connection.init(allocator, qmux_config(early.client, &policy.config))); // a client's early data
    }
    {
        WebSocketConfig bad = policy.config;
        bad.path = {};
        QmuxConnection connection;
        CHECK_FALSE(connection.init(allocator, qmux_config(identity.server, &bad)));
    }
    {
        QmuxConnection connection;
        CHECK(connection.init(allocator, qmux_config(early.server, &policy.config))); // a server's: QMux may use it
    }
}

TEST_CASE("WebSocket: two connections open, name their subprotocol and carry streams past every window",
          "[net][websocket]")
{
    const usize chunk = GENERATE(usize{1U}, usize{7U}, kBuffer);
    CAPTURE(chunk);
    Pair pair;
    pair.connect(chunk);
    CHECK(pair.client.websocket());
    CHECK(pair.server.websocket());
    CHECK(same(pair.client.websocket_protocol(), span_of(kProtocol)));
    CHECK(same(pair.server.websocket_protocol(), span_of(kProtocol)));
    CHECK(pair.client.peer_parameters_known());
    CHECK(pair.server.peer_parameters_known());
    CHECK(pair.client.handshake().application_protocol() == StringView("http/1.1"));

    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    constexpr u64 total = 300U * 1024U;
    u64 client_written = 0U;
    u64 server_written = 0U;
    static_cast<void>(write_pattern(pair.client.streams(), id, client_written, total, 3U));
    pump(pair.client, pair.server, kNow, chunk); // the server's side of the stream exists from here
    u64 client_read = 0U;
    u64 server_read = 0U;
    const usize allocations = pair.allocator.calls();
    bool client_done = false;
    bool server_done = false;
    for (usize turn = 0U; turn < 100000U && !(client_done && server_done); ++turn)
    {
        static_cast<void>(write_pattern(pair.client.streams(), id, client_written, total, 3U));
        static_cast<void>(write_pattern(pair.server.streams(), id, server_written, total, 5U));
        pump(pair.client, pair.server, kNow, chunk);
        client_done = client_done || drain(pair.client.streams(), id, client_read, 5U);
        server_done = server_done || drain(pair.server.streams(), id, server_read, 3U);
    }
    CHECK(client_read == total);
    CHECK(server_read == total);
    CHECK(pair.allocator.calls() == allocations); // nothing allocates after init
}

TEST_CASE("WebSocket: datagrams at the peer's limit cross both ways", "[net][websocket]")
{
    Pair pair;
    pair.connect();
    const usize limit = pair.client.datagrams().max_payload();
    REQUIRE(limit != 0U);
    REQUIRE(limit == pair.server.datagrams().max_payload());
    Bytes message;
    message.resize(limit, static_cast<u8>(0x5AU));
    u64 id = 0U;
    REQUIRE(pair.client.datagrams().queue(view(message), kNow, id) == QuicDatagramError::None);
    REQUIRE(pair.server.datagrams().queue(view(message), kNow, id) == QuicDatagramError::None);
    pump(pair.client, pair.server, kNow);
    for (QmuxConnection* connection : {&pair.client, &pair.server})
    {
        ConstSpan<u8> received;
        REQUIRE(connection->datagrams().peek(received));
        CHECK(same(received, view(message)));
        connection->datagrams().pop();
    }
}

TEST_CASE("WebSocket: an application close sends CONNECTION_CLOSE, then Close 1000, then close_notify",
          "[net][websocket]")
{
    RawClient raw;
    raw.opened();
    static_cast<void>(raw.frames());
    raw.server.close(42U, span_of("done"), raw.now);
    raw.flush();
    CHECK(raw.server.state() == QmuxState::Closing);
    CHECK(raw.server.send_finished());
    const auto said = raw.frames();
    REQUIRE(said.size() == 2U);
    CHECK(said[0].opcode == WsOpcode::Binary);
    CHECK(said[1].opcode == WsOpcode::Close);
    CHECK(same(view(said[1].payload), view(close_payload(1000U))));
    CHECK(raw.peer.state() == TlsState::Closed);
    CHECK(raw.server.close_info().reason == QmuxCloseReason::Local);
}

TEST_CASE("WebSocket: the server's records are unfragmented, unmasked Binary messages with no Size", "[net][websocket]")
{
    RawClient raw;
    raw.upgrade();
    const auto said = raw.frames();
    REQUIRE_FALSE(said.empty());
    CHECK(said[0].opcode == WsOpcode::Binary);
    CHECK(said[0].fin);
    // The first bytes are the QX_TRANSPORT_PARAMETERS frame's type, not a Size.
    Bytes type;
    type.resize(8U);
    WireWriter writer{Span<u8>(type.data(), type.size())};
    put_quic_varint(writer, iana::kQuicFrameQxTransportParameters);
    CHECK(same(ConstSpan<u8>(said[0].payload.data(), 8U), view(type)));
}

TEST_CASE("WebSocket: a record in fragments, with control frames between them and cuts anywhere", "[net][websocket]")
{
    const usize chunk = GENERATE(usize{1U}, usize{5U}, usize{4096U});
    CAPTURE(chunk);
    RawClient raw;
    raw.opened();
    static_cast<void>(raw.frames());
    // The client's bidirectional stream 0: offset 0, a FIN and a length, in a record split over three fragments.
    Bytes body;
    body.resize(4096U);
    for (usize at = 0U; at < body.size(); ++at)
    {
        body[at] = pattern(9U, at);
    }
    Bytes record;
    record.resize(body.size() + 16U);
    WireWriter writer{Span<u8>(record.data(), record.size())};
    writer.put_u8(0x0BU);
    put_quic_varint(writer, 0U);
    put_quic_varint(writer, body.size());
    writer.put_bytes(view(body));
    record.resize(writer.written());

    Bytes wire;
    const Bytes first = bytes_of(ConstSpan<u8>(record.data(), 10U));
    append(wire, view(frame_of(WsOpcode::Binary, false, view(first))));
    const Bytes ping = bytes_of("are you there");
    append(wire, view(frame_of(WsOpcode::Ping, true, view(ping))));
    append(wire, view(frame_of(WsOpcode::Continuation, false, ConstSpan<u8>())));
    append(wire, view(frame_of(WsOpcode::Pong, true, ConstSpan<u8>())));
    const Bytes second = bytes_of(ConstSpan<u8>(record.data() + 10U, record.size() - 10U));
    append(wire, view(frame_of(WsOpcode::Continuation, true, view(second))));

    for (usize at = 0U; at < wire.size(); at += chunk)
    {
        raw.send(ConstSpan<u8>(wire.data() + at, (wire.size() - at < chunk) ? wire.size() - at : chunk));
        CHECK(raw.server.state() == QmuxState::Established);
    }
    u64 read = 0U;
    CHECK(drain(raw.server.streams(), 0U, read, 9U));
    CHECK(read == body.size());
    bool answered = false;
    for (const ServerFrame& frame : raw.frames())
    {
        answered = answered || (frame.opcode == WsOpcode::Pong && same(view(frame.payload), view(ping)));
    }
    CHECK(answered);
}

TEST_CASE("WebSocket: frames in one write and frames behind the opening handshake are all read", "[net][websocket]")
{
    RawClient raw;
    Bytes wire = head_of(kRequest);
    append(wire, view(frame_of(WsOpcode::Binary, true, view(parameters_frame()))));
    append(wire, view(frame_of(WsOpcode::Ping, true, view(bytes_of("x")))));
    raw.send(wire);
    CHECK(raw.server.state() == QmuxState::Established);
    CHECK(raw.server.peer_parameters_known());
}

TEST_CASE("WebSocket: a message of exactly a record's limit is taken, one byte more fails with 1009 before it arrives",
          "[net][websocket]")
{
    RawClient raw;
    raw.upgrade();
    Bytes record = parameters_frame();
    record.resize(kQmuxMaxRecord, 0U); // PADDING after the parameters
    raw.send(frame_of(WsOpcode::Binary, true, view(record)));
    CHECK(raw.server.state() == QmuxState::Established);
    CHECK(raw.server.counters().records_received == 1U);
    static_cast<void>(raw.frames());

    SECTION("announced")
    {
        Bytes head;
        head.resize(ws_frame_header_bytes(kQmuxMaxRecord + 1U, true));
        const u8 mask[kWsMaskBytes] = {1U, 2U, 3U, 4U};
        ws_write_frame_header(Span<u8>(head.data(), head.size()), WsOpcode::Binary, true, kQmuxMaxRecord + 1U, mask);
        raw.send(head);
    }
    SECTION("across fragments")
    {
        Bytes half;
        half.resize(kQmuxMaxRecord);
        raw.send(frame_of(WsOpcode::Binary, false, view(half)));
        CHECK(raw.server.state() == QmuxState::Established);
        raw.send(frame_of(WsOpcode::Continuation, true, view(bytes_of("x"))));
    }
    CHECK(raw.server.state() == QmuxState::Closed);
    CHECK(raw.server.close_info().reason == QmuxCloseReason::WebSocketFailed);
    CHECK(raw.server.close_info().error_code == kWsCloseTooBig);
    CHECK(raw.close_status() == kWsCloseTooBig);
}

TEST_CASE("WebSocket: every frame section 5 refuses fails the connection with its status", "[net][websocket]")
{
    struct Row
    {
        Bytes wire;
        u16 status;
    };
    const Bytes record = parameters_frame();
    const Row rows[] = {
        {frame_of(WsOpcode::Binary, true, view(record), false), kWsCloseProtocolError},       // unmasked
        {bytes_of({0xC2U, 0x80U, 1U, 2U, 3U, 4U}), kWsCloseProtocolError},                    // RSV1
        {bytes_of({0x83U, 0x80U, 1U, 2U, 3U, 4U}), kWsCloseProtocolError},                    // reserved opcode
        {frame_of(WsOpcode::Text, true, view(record)), kWsCloseUnsupportedData},              // Text
        {frame_of(WsOpcode::Continuation, true, view(record)), kWsCloseProtocolError},        // no message begun
        {frame_of(WsOpcode::Close, true, view(bytes_of({0x03U}))), kWsCloseProtocolError},    // a one-byte Close
        {frame_of(WsOpcode::Close, true, view(close_payload(1006U))), kWsCloseProtocolError}, // a code never sent
    };
    for (const Row& row : rows)
    {
        RawClient raw;
        raw.upgrade();
        static_cast<void>(raw.frames());
        raw.send(row.wire);
        CHECK(raw.server.state() == QmuxState::Closed);
        CHECK(raw.server.close_info().reason == QmuxCloseReason::WebSocketFailed);
        CHECK(raw.server.close_info().error_code == row.status);
        CHECK(raw.close_status() == row.status);
        CHECK(raw.peer.state() == TlsState::Closed);
    }
}

TEST_CASE("WebSocket: a Close whose reason is not UTF-8 fails with 1007, a data frame inside a message with 1002",
          "[net][websocket]")
{
    {
        RawClient raw;
        raw.upgrade();
        static_cast<void>(raw.frames());
        Bytes payload = close_payload(1000U);
        append(payload, view(bytes_of({0xFFU})));
        raw.send(frame_of(WsOpcode::Close, true, view(payload)));
        CHECK(raw.server.close_info().error_code == kWsCloseInvalidPayload);
        CHECK(raw.close_status() == kWsCloseInvalidPayload);
    }
    {
        RawClient raw;
        raw.upgrade();
        static_cast<void>(raw.frames());
        Bytes wire = frame_of(WsOpcode::Binary, false, view(bytes_of("a")));
        append(wire, view(frame_of(WsOpcode::Binary, true, view(bytes_of("b")))));
        raw.send(wire);
        CHECK(raw.server.close_info().error_code == kWsCloseProtocolError);
        CHECK(raw.close_status() == kWsCloseProtocolError);
    }
    {
        RawClient raw;
        raw.upgrade();
        static_cast<void>(raw.frames());
        Bytes wire = frame_of(WsOpcode::Binary, false, view(bytes_of("a")));
        append(wire, view(frame_of(WsOpcode::Text, true, view(bytes_of("b")))));
        raw.send(wire);
        CHECK(raw.server.close_info().error_code == kWsCloseUnsupportedData);
    }
}

TEST_CASE("WebSocket: QMux's own refusals still hold on the message path", "[net][websocket]")
{
    SECTION("an empty message is a record of no frames")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        raw.send(frame_of(WsOpcode::Binary, true, ConstSpan<u8>()));
        CHECK(raw.server.state() == QmuxState::Closing);
        CHECK(raw.server.close_info().reason == QmuxCloseReason::Error);
        CHECK(raw.server.close_info().error_code == static_cast<u64>(QuicTransportError::ProtocolViolation));
        const auto said = raw.frames();
        REQUIRE(said.size() == 2U);
        CHECK(said[0].opcode == WsOpcode::Binary); // CONNECTION_CLOSE
        CHECK(said[1].opcode == WsOpcode::Close);
        CHECK(same(view(said[1].payload), view(close_payload(kWsCloseProtocolError))));
    }
    SECTION("an empty message in fragments is the same")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        Bytes wire = frame_of(WsOpcode::Binary, false, ConstSpan<u8>());
        append(wire, view(frame_of(WsOpcode::Continuation, true, ConstSpan<u8>())));
        raw.send(wire);
        CHECK(raw.server.close_info().error_code == static_cast<u64>(QuicTransportError::ProtocolViolation));
    }
    SECTION("the first frame must be the parameters")
    {
        RawClient raw;
        raw.upgrade();
        raw.send(frame_of(WsOpcode::Binary, true, view(bytes_of({0x00U}))));
        CHECK(raw.server.close_info().reason == QmuxCloseReason::Error);
        CHECK(raw.server.close_info().error_code == static_cast<u64>(QuicTransportError::ProtocolViolation));
    }
    SECTION("a peer's CONNECTION_CLOSE ends it, answered with Close")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        Bytes record;
        record.resize(32U);
        QuicFrame frame;
        frame.kind = QuicFrameKind::ConnectionClose;
        frame.application = true;
        frame.error_code = 9U;
        const usize used = write_quic_frame(frame, Span<u8>(record.data(), record.size()));
        record.resize(used);
        raw.send(frame_of(WsOpcode::Binary, true, view(record)));
        CHECK(raw.server.state() == QmuxState::Closed);
        CHECK(raw.server.close_info().reason == QmuxCloseReason::Peer);
        CHECK(raw.server.close_info().error_code == 9U);
        CHECK(raw.close_status() == kWsCloseNormal);
    }
}

TEST_CASE("WebSocket: a peer's Close is answered with its status and ends the connection", "[net][websocket]")
{
    SECTION("with a code and a reason")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        raw.send(frame_of(WsOpcode::Close, true, view(close_payload(4000U, "bye"))));
        CHECK(raw.server.state() == QmuxState::Closed);
        CHECK(raw.server.close_info().reason == QmuxCloseReason::PeerShutdown);
        CHECK(raw.server.close_info().error_code == 4000U);
        CHECK(same(raw.server.close_reason(), span_of("bye")));
        CHECK(raw.close_status() == 4000U);
        CHECK(raw.peer.state() == TlsState::Closed);
        CHECK(raw.server.send_finished());
    }
    SECTION("with none")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        raw.send(frame_of(WsOpcode::Close, true, ConstSpan<u8>()));
        CHECK(raw.server.close_info().reason == QmuxCloseReason::PeerShutdown);
        CHECK(raw.server.close_info().error_code == kWsCloseNoStatus);
        CHECK(raw.close_status() == kWsCloseNormal);
    }
    SECTION("nothing behind it is read")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        Bytes wire = frame_of(WsOpcode::Close, true, ConstSpan<u8>());
        append(wire, view(frame_of(WsOpcode::Binary, true, view(parameters_frame()))));
        const u64 received = raw.server.counters().records_received;
        raw.send(wire);
        CHECK(raw.server.counters().records_received == received);
    }
    SECTION("a close_notify with no Close is the peer's shutdown, answered")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        REQUIRE(raw.peer.close() == TlsError::None);
        raw.flush();
        CHECK(raw.server.state() == QmuxState::Closed);
        CHECK(raw.server.close_info().reason == QmuxCloseReason::PeerShutdown);
    }
}

TEST_CASE("WebSocket: a Ping is answered with its payload, only the latest one owed", "[net][websocket]")
{
    SECTION("the largest")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        Bytes payload;
        payload.resize(kWsMaxControlBytes, static_cast<u8>('p'));
        raw.send(frame_of(WsOpcode::Ping, true, view(payload)));
        const auto said = raw.frames();
        REQUIRE(said.size() == 1U);
        CHECK(said[0].opcode == WsOpcode::Pong);
        CHECK(same(view(said[0].payload), view(payload)));
    }
    SECTION("an empty one")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        raw.send(frame_of(WsOpcode::Ping, true, ConstSpan<u8>()));
        const auto said = raw.frames();
        REQUIRE(said.size() == 1U);
        CHECK(said[0].opcode == WsOpcode::Pong);
        CHECK(said[0].payload.empty());
    }
    SECTION("two before the server writes")
    {
        RawClient raw;
        raw.opened();
        static_cast<void>(raw.frames());
        Bytes wire = frame_of(WsOpcode::Ping, true, view(bytes_of("one")));
        append(wire, view(frame_of(WsOpcode::Ping, true, view(bytes_of("two")))));
        raw.send(wire);
        const auto said = raw.frames();
        REQUIRE(said.size() == 1U);
        CHECK(holds(said[0].payload, "two"));
    }
}

TEST_CASE("WebSocket: a Pong handed on restarts the idle timeout, which ends with Close 1001", "[net][websocket]")
{
    RawClient raw(1000U);
    raw.opened();
    static_cast<void>(raw.frames());
    raw.now += 900U * kMsNs;
    raw.send(frame_of(WsOpcode::Ping, true, ConstSpan<u8>()));
    static_cast<void>(raw.frames());
    CHECK(raw.server.next_timer_ns() ==
          raw.now + 1000U * kMsNs); // draft section 7.1: every record, and this, handed on
    raw.now = raw.server.next_timer_ns();
    raw.server.on_timeout(raw.now);
    raw.flush();
    CHECK(raw.server.state() == QmuxState::Closed);
    CHECK(raw.server.close_info().reason == QmuxCloseReason::IdleTimeout);
    CHECK(raw.close_status() == kWsCloseGoingAway);
}

TEST_CASE("WebSocket: the handshake timeout covers the opening handshake", "[net][websocket]")
{
    RawClient raw;
    const Bytes partial = bytes_of("GET /game HTTP/1.1\r\nHost:");
    raw.send(partial);
    CHECK(raw.server.state() == QmuxState::Handshaking);
    raw.now = raw.server.next_timer_ns();
    raw.server.on_timeout(raw.now);
    CHECK(raw.server.state() == QmuxState::Closed);
    CHECK(raw.server.close_info().reason == QmuxCloseReason::HandshakeTimeout);
}

TEST_CASE("WebSocket: a refused request is answered and ends the connection", "[net][websocket]")
{
    RawClient raw;
    raw.send(head_of(kRequest, "", "", "Origin: https://evil.example\r\n"));
    CHECK(raw.server.state() == QmuxState::Closed);
    CHECK(raw.server.close_info().reason == QmuxCloseReason::UpgradeRefused);
    CHECK(raw.server.close_info().error_code == 403U);
    CHECK(raw.plain.size() > 12U);
    CHECK(same(ConstSpan<u8>(raw.plain.data(), 12U), span_of("HTTP/1.1 403")));
    CHECK(raw.peer.state() == TlsState::Closed);
    CHECK_FALSE(raw.server.websocket_protocol().size() != 0U);
}

TEST_CASE("WebSocket: close() before the upgrade abandons it", "[net][websocket]")
{
    RawClient raw;
    raw.server.close(1U, ConstSpan<u8>(), raw.now);
    CHECK(raw.server.state() == QmuxState::Closed);
    CHECK(raw.server.close_info().reason == QmuxCloseReason::Local);
}

TEST_CASE("WebSocket: a client's opening handshake and frames", "[net][websocket]")
{
    SECTION("a good answer establishes it, and every frame is masked anew")
    {
        RawServer raw;
        raw.send(raw.answer());
        REQUIRE(raw.client.state() == QmuxState::Established);
        CHECK(same(raw.client.websocket_protocol(), span_of(kProtocol)));

        u32 masks[8];
        for (u32& mask : masks)
        {
            raw.client.ping();
            raw.flush();
            usize at = 0U;
            bool found = false;
            while (at < raw.plain.size())
            {
                WsFrameHeader header;
                u16 failure = 0U;
                REQUIRE(ws_read_frame_header(ConstSpan<u8>(raw.plain.data() + at, raw.plain.size() - at), true,
                                             kQmuxMaxRecord, header, failure) == HttpDecode::Ok);
                CHECK(header.masked);
                std::memcpy(&mask, header.mask, sizeof(mask));
                // Unmasked, a record starts with a frame type: the parameters first, then the pings.
                ws_mask(Span<u8>(raw.plain.data() + at + header.header_bytes, header.payload_bytes), header.mask);
                const u64 expected =
                    !found && &mask == masks ? iana::kQuicFrameQxTransportParameters : iana::kQuicFrameQxPingRequest;
                crd::containers::WireReader reader(
                    ConstSpan<u8>(raw.plain.data() + at + header.header_bytes, header.payload_bytes));
                u64 type = 0U;
                REQUIRE(take_quic_varint(reader, type));
                CHECK(type == expected);
                found = true;
                at += header.header_bytes + header.payload_bytes;
            }
            CHECK(found);
            raw.plain.resize(0U);
        }
        bool differ = false;
        for (const u32 mask : masks)
        {
            differ = differ || mask != masks[0];
        }
        CHECK(differ);
    }
    SECTION("a refusal names the server's status")
    {
        RawServer raw;
        raw.send(bytes_of("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"));
        CHECK(raw.client.state() == QmuxState::Closed);
        CHECK(raw.client.close_info().reason == QmuxCloseReason::UpgradeRefused);
        CHECK(raw.client.close_info().error_code == 403U);
    }
    SECTION("an answer with the wrong Accept is refused")
    {
        RawServer raw;
        static_cast<void>(raw.answer());
        raw.send(head_of(kResponse, "accept", "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));
        CHECK(raw.client.state() == QmuxState::Closed);
        CHECK(raw.client.close_info().reason == QmuxCloseReason::UpgradeRefused);
        CHECK(raw.client.close_info().error_code == 0U);
    }
    SECTION("a masked frame from the server fails it")
    {
        RawServer raw;
        raw.send(raw.answer());
        raw.send(frame_of(WsOpcode::Binary, true, view(parameters_frame()), true));
        CHECK(raw.client.state() == QmuxState::Closed);
        CHECK(raw.client.close_info().reason == QmuxCloseReason::WebSocketFailed);
        CHECK(raw.client.close_info().error_code == kWsCloseProtocolError);
    }
    SECTION("frames behind the 101 in one write are read")
    {
        RawServer raw;
        Bytes wire = raw.answer();
        append(wire, view(frame_of(WsOpcode::Binary, true, view(parameters_frame()), false)));
        raw.send(wire);
        CHECK(raw.client.state() == QmuxState::Established);
        CHECK(raw.client.peer_parameters_known());
    }
    SECTION("a peer's end during the upgrade is its shutdown")
    {
        RawServer raw;
        REQUIRE(raw.peer.close() == TlsError::None);
        raw.flush();
        CHECK(raw.client.state() == QmuxState::Closed);
        CHECK(raw.client.close_info().reason == QmuxCloseReason::PeerShutdown);
    }
}

TEST_CASE("WebSocket: one server context serves QMux and WebSocket by ALPN", "[net][websocket]")
{
    WsIdentity identity;
    CountingAllocator allocator;
    Policy policy;
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(allocator, qmux_config(identity.qmux_client, nullptr)));
    REQUIRE(server.init(allocator, qmux_config(identity.server, &policy.config)));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    pump(client, server, kNow);
    CHECK(client.state() == QmuxState::Established);
    CHECK(server.state() == QmuxState::Established);
    CHECK_FALSE(server.websocket());
    CHECK(server.peer_parameters_known());
}

TEST_CASE("WebSocket: without a configuration http/1.1 is not read as WebSocket", "[net][websocket]")
{
    RawClient reference;
    CountingAllocator allocator;
    QmuxConnection server;
    REQUIRE(server.init(allocator, qmux_config(reference.identity.server, nullptr)));
    REQUIRE(server.start(kNow));
    TlsSession peer;
    REQUIRE(peer.init(reference.identity.client) == TlsError::None);
    u8 bytes[kBuffer];
    for (bool moved = true; moved;)
    {
        moved = false;
        usize got = 0U;
        REQUIRE(peer.take(Span<u8>(bytes), got) == TlsError::None);
        if (got != 0U)
        {
            moved = true;
            server.receive(ConstSpan<u8>(bytes, got), kNow);
        }
        const usize out = server.take(Span<u8>(bytes), kNow);
        if (out != 0U)
        {
            moved = true;
            static_cast<void>(peer.receive(ConstSpan<u8>(bytes, out)));
        }
    }
    CHECK_FALSE(server.websocket());
}

TEST_CASE("WebSocket: a server that accepted early data on http/1.1 refuses it", "[net][websocket]")
{
    ReadyFilter ready;
    WsIdentity identity(&ready.filter);
    CountingAllocator allocator;
    Policy policy;
    TicketCache tickets{allocator, TicketCacheConfig{}};
    u8 bytes[kBuffer];
    {
        TlsSession first;
        QmuxConnection server;
        REQUIRE(server.init(allocator, qmux_config(identity.server, &policy.config)));
        REQUIRE(server.start(kNow));
        REQUIRE(first.init(identity.client, &tickets) == TlsError::None);
        first.hold_tickets();
        for (usize turn = 0U; turn < 16U; ++turn)
        {
            usize got = 0U;
            REQUIRE(first.take(Span<u8>(bytes), got) == TlsError::None);
            if (got != 0U)
            {
                server.receive(ConstSpan<u8>(bytes, got), kNow);
            }
            const usize out = server.take(Span<u8>(bytes), kNow);
            if (out != 0U)
            {
                static_cast<void>(first.receive(ConstSpan<u8>(bytes, out)));
                static_cast<void>(first.read(Span<u8>(bytes), got)); // tickets are read with what follows the handshake
            }
        }
        REQUIRE(first.state() == TlsState::Established);
        first.remember_parameters(view(parameters_frame()));
    }
    TlsSession peer;
    REQUIRE(peer.init(identity.client, &tickets) == TlsError::None);
    REQUIRE(peer.early_data() == crypto::TlsEarlyData::Offered);
    REQUIRE(peer.send_early(view(head_of(kRequest))) == TlsError::None);
    QmuxConnection server;
    REQUIRE(server.init(allocator, qmux_config(identity.server, &policy.config)));
    REQUIRE(server.start(kNow));
    usize got = 0U;
    REQUIRE(peer.take(Span<u8>(bytes), got) == TlsError::None);
    server.receive(ConstSpan<u8>(bytes, got), kNow);
    CHECK(server.state() == QmuxState::Closed);
    CHECK(server.close_info().reason == QmuxCloseReason::TlsFailed);
    CHECK(server.tls_error() == TlsError::PeerRefused);
}

TEST_CASE("WebSocket: the host carries it with nothing added, a transfer and a close over pipes", "[net][websocket]")
{
    ReadyFilter ready;
    WsIdentity identity;
    Policy policy;
    CountingAllocator allocator;
    TestClock clock;
    PipeNetwork network;
    const Endpoint address{IpAddress::v4_loopback(), 4433U};
    PipePoller server_poller{network, address};
    PipePoller client_poller{network, Endpoint{IpAddress::v4_loopback(), 50000U}};
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    Recorder server_events;
    Recorder client_events;
    client_events.client = true;
    client_events.transfer = 1024U * 1024U;
    server_events.transfer = 1024U * 1024U;
    network.listener = &server_poller;
    QmuxHostConfig server_config;
    server_config.connection = qmux_config(identity.server, &policy.config);
    server_config.connections = 4U;
    QmuxHostConfig client_config;
    client_config.connection = qmux_config(identity.client, &policy.config);
    client_config.connection.tls = nullptr;
    client_config.connections = 4U;
    QmuxHost server(allocator, server_poller, clock, &buckets, server_events, server_config);
    QmuxHost client(allocator, client_poller, clock, nullptr, client_events, client_config);
    REQUIRE(server.ready());
    REQUIRE(client.ready());
    SocketError error = SocketError::None;
    const QuicHandle handle = client.connect(QmuxConnect{address, &identity.client}, clock.now, error);
    REQUIRE(handle.valid());
    for (usize turn = 0U; turn < 20000U && !(client_events.finished && server_events.finished); ++turn)
    {
        static_cast<void>(server.poll(0U));
        static_cast<void>(client.poll(0U));
        clock.now += kMsNs;
    }
    CHECK(client_events.finished);
    CHECK(server_events.finished);
    CHECK(client_events.read == client_events.transfer);
    CHECK(server_events.read == server_events.transfer);
    QmuxConnection* const connection = client.connection(handle);
    REQUIRE(connection != nullptr);
    connection->close(7U, ConstSpan<u8>(), clock.now);
    client.wake(handle);
    for (usize turn = 0U; turn < 32U; ++turn)
    {
        static_cast<void>(server.poll(0U));
        static_cast<void>(client.poll(0U));
        clock.now += kMsNs;
    }
    REQUIRE(client_events.ended);
    REQUIRE(server_events.ended);
    CHECK(client_events.websocket);
    CHECK(server_events.websocket);
    CHECK(server_events.info.reason == QmuxCloseReason::Peer);
    CHECK(server_events.info.error_code == 7U);
    CHECK(client_poller.resets == 0U);
    CHECK(server_poller.resets == 0U);
}
