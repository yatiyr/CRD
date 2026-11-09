// crd-fuzz-net-websocket: RFC 6455's frames and opening handshake, and QMux carried over them (4.2a;
// docs/design/network-foundation.md#websocket), under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: a mode byte, then the mode's. Mode 0 reads the rest as one frame header, Close payload, request and response.
// Modes 1 and 2 put a real TLS peer against a server and a client and drive it with operations: plaintext cut anywhere,
// a frame of any opcode, the opening handshake's other half, time, reads, QX_PING, closes and the transport's end.
//
// Oracles. Parsers: a frame header that reads writes back the same; a request that earns 101 earns a response its own
// client accepts, with the same subprotocol. Connections: nothing is allocated after init(), the state only moves
// forward; what this side sends is one head and then frames, every one legal for its side, each Binary one a whole
// record of allowed frames, nothing after a Close, and no frame after a refusal.
#include "qmux_fuzz_fixture.hpp"

#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/websocket.hpp>

#include <cstring>
#include <new>

namespace
{
using crd::u16;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using crd::containers::WireWriter;
using crd::net::testing::FuzzInput;
using crd::net::testing::QmuxRecordOracle;
using namespace crd::crypto;
using namespace crd::net;

constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm};
constexpr StringView kProtocols[] = {"http/1.1"};
constexpr u64 kStartNs = 1000000000U;
constexpr u64 kMsNs = 1000000U;
constexpr usize kOracleBytes = 96U * 1024U;

ConstSpan<u8> text(const char* literal)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(literal), std::strlen(literal));
}

struct Identity
{
    SigningKey key;
    TlsCredential credential;
    TlsContext client;
    TlsContext server;
    bool ready = false;
    ConstSpan<u8> protocols[2];
    ConstSpan<u8> origins[1];
    WebSocketConfig websocket;

    Identity()
    {
        u8 seed[32] = {4U, 5U, 6U};
        CRD_FUZZ_REQUIRE(key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed)) == CryptoError::None);
        CRD_FUZZ_REQUIRE(credential.init(key, "game.cerid.test") == TlsError::None);
        const KeyId pin = credential.key_id();
        TlsConfig config;
        config.role = TlsRole::Client;
        config.groups = kGroups;
        config.signatures = kSignatures;
        config.aeads = kAeads;
        config.pinned = ConstSpan<KeyId>(&pin, 1U);
        config.server_name = "game.cerid.test";
        config.application_protocols = kProtocols;
        CRD_FUZZ_REQUIRE(client.init(config) == TlsError::None);
        TlsConfig server_config;
        server_config.role = TlsRole::Server;
        server_config.groups = kGroups;
        server_config.aeads = kAeads;
        server_config.credential = &credential;
        server_config.server_name = "game.cerid.test";
        server_config.application_protocols = kProtocols;
        CRD_FUZZ_REQUIRE(server.init(server_config) == TlsError::None);
        protocols[0] = text("qmux-02.game");
        protocols[1] = text("other");
        origins[0] = text("https://game.example");
        websocket.protocols = ConstSpan<ConstSpan<u8>>(protocols, 2U);
        websocket.origins = ConstSpan<ConstSpan<u8>>(origins, 1U);
        websocket.path = text("/g");
        websocket.host = text("game.cerid.test");
        ready = true;
    }
};

Identity& identity()
{
    static Identity held;
    CRD_FUZZ_REQUIRE(held.ready);
    return held;
}

const char kAccept[] = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
const char kKey[] = "dGhlIHNhbXBsZSBub25jZQ==";

void parsers(ConstSpan<u8> bytes)
{
    const WebSocketConfig& config = identity().websocket;
    for (const bool from_client : {false, true})
    {
        WsFrameHeader header;
        u16 failure = 0U;
        const HttpDecode decode = ws_read_frame_header(bytes, from_client, kQmuxMaxRecord, header, failure);
        if (decode == HttpDecode::Invalid)
        {
            CRD_FUZZ_REQUIRE(failure == kWsCloseProtocolError || failure == kWsCloseTooBig);
        }
        if (decode != HttpDecode::Ok)
        {
            continue;
        }
        CRD_FUZZ_REQUIRE(failure == 0U && header.header_bytes <= bytes.size() && header.masked == from_client);
        u8 written[kWsMaxHeaderBytes];
        CRD_FUZZ_REQUIRE(ws_frame_header_bytes(header.payload_bytes, header.masked) == header.header_bytes);
        ws_write_frame_header(Span<u8>(written, header.header_bytes), header.opcode, header.fin, header.payload_bytes,
                              header.masked ? header.mask : nullptr);
        CRD_FUZZ_REQUIRE(std::memcmp(written, bytes.data(), header.header_bytes) == 0);
    }
    u16 code = 0U;
    ConstSpan<u8> reason;
    const u16 failure = ws_read_close(bytes.first((bytes.size() < 125U) ? bytes.size() : 125U), code, reason);
    CRD_FUZZ_REQUIRE(failure == 0U || failure == kWsCloseProtocolError || failure == kWsCloseInvalidPayload);
    CRD_FUZZ_REQUIRE(failure != 0U || code == kWsCloseNoStatus || ws_close_code_valid(code));

    usize scanned = 0U;
    WsHandshake handshake;
    if (ws_read_request(bytes, config, scanned, handshake) == HttpDecode::Ok && handshake.status == 101U)
    {
        u8 response[kWsMaxResponseBytes];
        const usize used = ws_write_response(Span<u8>(response), config, handshake);
        usize again = 0U;
        WsHandshake read;
        CRD_FUZZ_REQUIRE(ws_read_response(ConstSpan<u8>(response, used), config,
                                          ConstSpan<u8>(handshake.accept, kWsAcceptChars), again,
                                          read) == HttpDecode::Ok);
        CRD_FUZZ_REQUIRE(read.status == 101U && read.protocol == handshake.protocol && read.head_bytes == used);
    }
    usize response_scanned = 0U;
    WsHandshake answer;
    static_cast<void>(ws_read_response(bytes, config, text(kAccept), response_scanned, answer));
}

// Checks what the side under test says after its TLS: one head, then frames.
class WsOracle
{
public:
    explicit WsOracle(bool server) noexcept : m_server(server) {}

    void plaintext(ConstSpan<u8> bytes)
    {
        CRD_FUZZ_REQUIRE(m_fill + bytes.size() <= sizeof(m_buffer));
        if (!bytes.empty())
        {
            std::memcpy(m_buffer + m_fill, bytes.data(), bytes.size());
            m_fill += bytes.size();
        }
        if (!m_head_read && !head())
        {
            return;
        }
        frames();
    }

private:
    [[nodiscard]] bool head()
    {
        usize end = 0U;
        for (usize at = 0U; at + 4U <= m_fill && end == 0U; ++at)
        {
            end = (std::memcmp(m_buffer + at, "\r\n\r\n", 4U) == 0) ? at + 4U : 0U;
        }
        if (end == 0U)
        {
            CRD_FUZZ_REQUIRE(m_fill < kWsMaxHeadBytes);
            return false;
        }
        if (m_server)
        {
            CRD_FUZZ_REQUIRE(m_fill >= 12U && std::memcmp(m_buffer, "HTTP/1.1 ", 9U) == 0);
            m_refused = std::memcmp(m_buffer + 9U, "101", 3U) != 0;
        }
        else
        {
            usize scanned = 0U;
            WsHandshake handshake;
            CRD_FUZZ_REQUIRE(ws_read_request(ConstSpan<u8>(m_buffer, end), identity().websocket, scanned, handshake) ==
                             HttpDecode::Ok);
            CRD_FUZZ_REQUIRE(handshake.status == 101U);
        }
        std::memmove(m_buffer, m_buffer + end, m_fill - end);
        m_fill -= end;
        m_head_read = true;
        return true;
    }

    void frames()
    {
        usize at = 0U;
        while (at < m_fill)
        {
            CRD_FUZZ_REQUIRE(!m_refused && !m_closed); // a refusal is the last thing said, and so is a Close
            WsFrameHeader header;
            u16 failure = 0U;
            const HttpDecode decode = ws_read_frame_header(ConstSpan<u8>(m_buffer + at, m_fill - at), !m_server,
                                                           kQmuxMaxRecord, header, failure);
            CRD_FUZZ_REQUIRE(decode != HttpDecode::Invalid);
            if (decode == HttpDecode::NeedMore || m_fill - at - header.header_bytes < header.payload_bytes)
            {
                break;
            }
            CRD_FUZZ_REQUIRE(header.fin);
            u8* const payload = m_buffer + at + header.header_bytes;
            if (header.masked)
            {
                ws_mask(Span<u8>(payload, header.payload_bytes), header.mask);
            }
            if (header.opcode == WsOpcode::Binary)
            {
                CRD_FUZZ_REQUIRE(header.payload_bytes != 0U);
                u8 size[8];
                WireWriter writer{Span<u8>(size)};
                put_quic_varint(writer, header.payload_bytes);
                m_records.plaintext(ConstSpan<u8>(size, writer.written()));
                m_records.plaintext(ConstSpan<u8>(payload, header.payload_bytes));
            }
            else if (header.opcode == WsOpcode::Close)
            {
                u16 code = 0U;
                ConstSpan<u8> reason;
                CRD_FUZZ_REQUIRE(ws_read_close(ConstSpan<u8>(payload, header.payload_bytes), code, reason) == 0U);
                m_closed = true;
            }
            else
            {
                CRD_FUZZ_REQUIRE(header.opcode == WsOpcode::Pong);
            }
            at += header.header_bytes + header.payload_bytes;
        }
        std::memmove(m_buffer, m_buffer + at, m_fill - at);
        m_fill -= at;
    }

    bool m_server;
    bool m_head_read = false;
    bool m_refused = false;
    bool m_closed = false;
    QmuxRecordOracle m_records;
    usize m_fill = 0U;
    u8 m_buffer[kOracleBytes] = {};
};

class Run
{
public:
    explicit Run(bool server) : m_server_under_test(server), m_oracle(server) {}

    void go(FuzzInput input)
    {
        QmuxConfig config;
        config.tls = m_server_under_test ? &identity().server : &identity().client;
        config.websocket = &identity().websocket;
        config.streams.peer_bidi = 4U;
        config.streams.peer_uni = 2U;
        config.streams.local_bidi = 2U;
        config.streams.local_uni = 2U;
        config.streams.receive_bytes = 8192U;
        config.streams.send_bytes = 8192U;
        config.streams.stream_window = 4096U;
        config.streams.connection_window = 16384U;
        config.streams.connection_max_window = 65536U;
        config.datagrams.max_frame_bytes = 600U;
        config.datagrams.receive_bytes = 2048U;
        config.datagrams.receive_count = 8U;
        config.datagrams.send_bytes = 2048U;
        config.datagrams.send_count = 8U;
        config.padding_block = 64U;
        CRD_FUZZ_REQUIRE(m_connection.init(m_allocator, config));
        CRD_FUZZ_REQUIRE(m_connection.start(m_now));
        const TlsContext& peer_context = m_server_under_test ? identity().client : identity().server;
        CRD_FUZZ_REQUIRE(m_peer.init(peer_context) == TlsError::None);
        flush();
        CRD_FUZZ_REQUIRE(m_peer.state() == TlsState::Established && m_connection.websocket());
        const usize live = m_allocator.live_bytes();
        while (input.more())
        {
            step(input);
            CRD_FUZZ_REQUIRE(static_cast<u8>(m_connection.state()) >= m_state_floor);
            m_state_floor = static_cast<u8>(m_connection.state());
            CRD_FUZZ_REQUIRE(m_allocator.live_bytes() == live);
        }
        flush();
        check_end();
    }

private:
    void check_end()
    {
        if (m_connection.state() != QmuxState::Closing && m_connection.state() != QmuxState::Closed)
        {
            return;
        }
        const QmuxCloseInfo& info = m_connection.close_info();
        CRD_FUZZ_REQUIRE(info.reason != QmuxCloseReason::None);
        if (info.reason == QmuxCloseReason::WebSocketFailed)
        {
            CRD_FUZZ_REQUIRE(info.error_code == kWsCloseProtocolError || info.error_code == kWsCloseUnsupportedData ||
                             info.error_code == kWsCloseInvalidPayload || info.error_code == kWsCloseTooBig);
        }
    }

    void step(FuzzInput& input)
    {
        switch (input.byte() % 9U)
        {
            case 0U:
                send_cut(input.run(), input.byte());
                break;
            case 1U:
                frame(input);
                break;
            case 2U:
                m_now += (u64{input.byte()} << 12U) * kMsNs;
                m_connection.on_timeout(m_now);
                break;
            case 3U:
                read_all(input.byte());
                break;
            case 4U:
                m_connection.ping();
                break;
            case 5U:
                if (input.byte() < 8U)
                {
                    m_connection.close(input.byte(), input.run(), m_now);
                }
                break;
            case 6U:
                if (input.byte() < 8U)
                {
                    m_connection.transport_ended((input.byte() & 1U) != 0U, m_now);
                }
                break;
            case 7U:
                upgrade(input.byte());
                break;
            default:
                if (input.byte() < 8U && m_peer.state() == TlsState::Established)
                {
                    static_cast<void>(m_peer.close());
                }
                break;
        }
        flush();
    }

    // The other half of the opening handshake, right unless the byte says otherwise.
    void upgrade(u8 choice)
    {
        if (m_server_under_test)
        {
            WireWriterText request;
            request.add(
                "GET /g HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
            request.add(kKey);
            request.add("\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: qmux-02.game\r\n");
            if ((choice & 1U) != 0U)
            {
                request.add("Origin: https://elsewhere.example\r\n");
            }
            request.add("\r\n");
            send_cut(request.view(), choice);
            return;
        }
        WireWriterText response;
        response.add(
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ");
        // The client's key is random: only a refusal or a failure can be scripted without reading it.
        response.add((choice & 1U) != 0U ? kAccept : "x");
        response.add("\r\nSec-WebSocket-Protocol: qmux-02.game\r\n\r\n");
        send_cut(response.view(), choice);
    }

    void frame(FuzzInput& input)
    {
        const u8 flags = input.byte();
        const ConstSpan<u8> run = input.run();
        const ConstSpan<u8> payload = run.first((run.size() < 16382U) ? run.size() : 16382U);
        static const WsOpcode kOpcodes[] = {WsOpcode::Binary, WsOpcode::Continuation, WsOpcode::Text,
                                            WsOpcode::Close,  WsOpcode::Ping,         WsOpcode::Pong};
        const WsOpcode opcode = kOpcodes[(flags & 0x07U) % 6U];
        const bool masked = m_server_under_test != ((flags & 0x10U) != 0U); // legal unless the flag asks otherwise
        const u8 mask[kWsMaskBytes] = {flags, 7U, 11U, 13U};
        const usize head = ws_frame_header_bytes(payload.size(), masked);
        u8 bytes[kWsMaxHeaderBytes + 16382U];
        ws_write_frame_header(Span<u8>(bytes, head), opcode, (flags & 0x08U) == 0U, payload.size(),
                              masked ? mask : nullptr);
        if (!payload.empty())
        {
            std::memcpy(bytes + head, payload.data(), payload.size());
            if (masked)
            {
                ws_mask(Span<u8>(bytes + head, payload.size()), mask);
            }
        }
        send_cut(ConstSpan<u8>(bytes, head + payload.size()), input.byte());
    }

    void send_cut(ConstSpan<u8> bytes, u8 cut)
    {
        if (bytes.empty() || m_peer.state() != TlsState::Established)
        {
            return;
        }
        const usize first = (usize{cut} * bytes.size()) / 256U;
        if (first != 0U)
        {
            static_cast<void>(m_peer.send(bytes.first(first)));
            flush();
        }
        if (m_peer.state() == TlsState::Established)
        {
            static_cast<void>(m_peer.send(bytes.subspan(first)));
        }
    }

    void read_all(u8 which)
    {
        for (u64 id = which % 32U; id < 32U; id += 4U)
        {
            const QuicStreamRead read = m_connection.streams().peek(id);
            static_cast<void>(m_connection.streams().consume(id, read.data.size()));
        }
        ConstSpan<u8> held;
        while (m_connection.datagrams().peek(held))
        {
            m_connection.datagrams().pop();
        }
    }

    void flush()
    {
        u8 bytes[16384];
        for (bool moved = true; moved;)
        {
            moved = false;
            for (;;)
            {
                usize got = 0U;
                if (m_peer.state() == TlsState::Idle || m_peer.take(Span<u8>(bytes), got) != TlsError::None ||
                    got == 0U)
                {
                    break;
                }
                moved = true;
                m_connection.receive(ConstSpan<u8>(bytes, got), m_now);
            }
            for (usize out = m_connection.take(Span<u8>(bytes), m_now); out != 0U;
                 out = m_connection.take(Span<u8>(bytes), m_now))
            {
                moved = true;
                if (m_peer.state() != TlsState::Failed && m_peer.state() != TlsState::Closed)
                {
                    static_cast<void>(m_peer.receive(ConstSpan<u8>(bytes, out)));
                }
                drain_peer();
            }
        }
    }

    void drain_peer()
    {
        u8 plain[16384];
        for (;;)
        {
            usize got = 0U;
            if (m_peer.read(Span<u8>(plain), got) != TlsError::None || got == 0U)
            {
                return;
            }
            m_oracle.plaintext(ConstSpan<u8>(plain, got));
        }
    }

    struct WireWriterText
    {
        u8 bytes[512] = {};
        usize used = 0U;

        void add(const char* more)
        {
            const usize size = std::strlen(more);
            std::memcpy(bytes + used, more, size);
            used += size;
        }

        [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(bytes, used); }
    };

    bool m_server_under_test;
    crd::fuzz::BudgetAllocator m_allocator;
    QmuxConnection m_connection;
    TlsSession m_peer;
    WsOracle m_oracle;
    u64 m_now = kStartNs;
    u8 m_state_floor = 0U;
};
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    const char request[] = "\x00GET /g HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                           "Sec-WebSocket-Protocol: qmux-02.game\r\n\r\n";
    sink.add("request", reinterpret_cast<const u8*>(request), sizeof(request) - 1U);
    // A server: the handshake, a fragment, a Ping and a Close.
    const u8 server[] = {1U, 7U, 0U, 1U,  8U,  0U, 3U, 'a', 'b', 'c', 40U,   1U,
                         4U, 0U, 1U, 'p', 90U, 1U, 3U, 0U,  2U,  3U,  0xE8U, 7U};
    sink.add("server", server, sizeof(server));
    // A client: a failed 101, then time.
    const u8 client[] = {2U, 7U, 1U, 2U, 3U};
    sink.add("client", client, sizeof(client));
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    if (data[0] % 3U == 0U)
    {
        parsers(ConstSpan<u8>(data + 1U, size - 1U));
        return 0;
    }
    // The connection's two record buffers are too large for a fuzzer's stack: one static run, rebuilt per input.
    alignas(Run) static unsigned char storage[sizeof(Run)];
    Run* const run = new (storage) Run(data[0] % 3U == 1U);
    run->go(FuzzInput{data + 1U, size - 1U});
    run->~Run();
    return 0;
}
