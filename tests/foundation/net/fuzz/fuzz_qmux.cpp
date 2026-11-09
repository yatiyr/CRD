// crd-fuzz-net-qmux: a QMux server (4.2a; docs/design/network-foundation.md#qmux) under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md). A real TLS client finishes the handshake, then the input drives it.
//
// Input: operations, each an operation byte and what it reads. Record bytes: [length][bytes] sent as one TLS write to
// the server, cut into pieces of any size; frames: [length][bytes] the same behind a valid Size; time steps; a read of
// the streams and datagrams; a ping; a close; the transport's end.
//
// Oracles. Nothing is allocated after init(). What the server sends parses as records of at most 16382 bytes whose
// first frame is its transport parameters and whose frames are all ones QMux allows. A server that closes names a
// transport error or the peer's own close, and sends no record after its CONNECTION_CLOSE. Stream reads never pass what
// was sent, and the server's state only moves forward.
#include "qmux_fuzz_fixture.hpp"

#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <new>

namespace
{
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
constexpr StringView kProtocols[] = {"cerid-game/1"};
constexpr u64 kStartNs = 1000000000U;
constexpr u64 kMsNs = 1000000U;

struct Identity
{
    SigningKey key;
    TlsCredential credential;
    TlsContext client;
    TlsContext server;
    bool ready = false;

    Identity()
    {
        u8 seed[32] = {1U, 2U, 3U};
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
        ready = true;
    }
};

Identity& identity()
{
    static Identity held;
    CRD_FUZZ_REQUIRE(held.ready);
    return held;
}

class Run
{
public:
    void go(FuzzInput input)
    {
        QmuxConfig config;
        config.tls = &identity().server;
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
        CRD_FUZZ_REQUIRE(m_server.init(m_allocator, config));
        CRD_FUZZ_REQUIRE(m_server.start(m_now));
        CRD_FUZZ_REQUIRE(m_peer.init(identity().client) == TlsError::None);
        flush();
        CRD_FUZZ_REQUIRE(m_server.state() == QmuxState::Established && m_peer.state() == TlsState::Established);
        const usize live = m_allocator.live_bytes();
        while (input.more())
        {
            step(input);
            CRD_FUZZ_REQUIRE(static_cast<u8>(m_server.state()) >= m_state_floor);
            m_state_floor = static_cast<u8>(m_server.state());
            CRD_FUZZ_REQUIRE(m_allocator.live_bytes() == live);
        }
        flush();
        if (m_server.state() == QmuxState::Closing || m_server.state() == QmuxState::Closed)
        {
            const QmuxCloseInfo& info = m_server.close_info();
            CRD_FUZZ_REQUIRE(info.reason != QmuxCloseReason::None);
        }
    }

private:
    void step(FuzzInput& input)
    {
        switch (input.byte() % 7U)
        {
            case 0U:
            {
                send_cut(input.run(), input.byte());
                break;
            }
            case 1U:
            {
                const ConstSpan<u8> frames = input.run();
                u8 header[8];
                WireWriter writer{Span<u8>(header)};
                put_quic_varint(writer, frames.size());
                send_cut(ConstSpan<u8>(header, writer.written()), 0U);
                send_cut(frames, input.byte());
                break;
            }
            case 2U:
                m_now += (u64{input.byte()} << 12U) * kMsNs;
                m_server.on_timeout(m_now);
                break;
            case 3U:
                read_all(input.byte());
                break;
            case 4U:
                m_server.ping();
                break;
            case 5U:
                if (input.byte() < 8U)
                {
                    m_server.close(input.byte(), input.run(), m_now);
                }
                break;
            default:
                if (input.byte() < 8U)
                {
                    m_server.transport_ended((input.byte() & 1U) != 0U, m_now);
                }
                break;
        }
        flush();
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
        static_cast<void>(m_peer.send(bytes.subspan(first)));
    }

    void read_all(u8 which)
    {
        for (u64 id = which % 32U; id < 32U; id += 4U)
        {
            const QuicStreamRead read = m_server.streams().peek(id);
            static_cast<void>(m_server.streams().consume(id, read.data.size()));
        }
        ConstSpan<u8> held;
        while (m_server.datagrams().peek(held))
        {
            m_server.datagrams().pop();
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
                m_server.receive(ConstSpan<u8>(bytes, got), m_now);
            }
            for (usize out = m_server.take(Span<u8>(bytes), m_now); out != 0U;
                 out = m_server.take(Span<u8>(bytes), m_now))
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

    crd::fuzz::BudgetAllocator m_allocator;
    QmuxConnection m_server;
    TlsSession m_peer;
    QmuxRecordOracle m_oracle;
    u64 m_now = kStartNs;
    u8 m_state_floor = 0U;
};
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // The peer's parameters (initial_max_data 1024), then a STREAM frame and a read.
    const u8 opened[] = {0x01U, 0x00U, 0x0DU, 0xFFU, 0x51U, 0x53U, 0x30U, 0x0DU, 0x0AU,
                         0x0DU, 0x0AU, 0x03U, 0x04U, 0x01U, 0x40U, 0x00U, 0x01U, 0x00U,
                         0x05U, 0x0BU, 0x00U, 0x03U, 0xAAU, 0xBBU, 0xCCU, 0x03U, 0x00U};
    sink.add("opened", opened, sizeof(opened));
    // A record whose Size is split from its frames by a cut, then time passing.
    const u8 cut[] = {0x00U, 0x00U, 0x06U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U, 0x02U, 0xFFU};
    sink.add("cut", cut, sizeof(cut));
    // A ping, a close with a reason, the transport's end.
    const u8 closing[] = {0x04U, 0x05U, 0x01U, 0x09U, 0x00U, 0x03U, 0x41U, 0x42U, 0x43U, 0x06U, 0x00U, 0x01U};
    sink.add("closing", closing, sizeof(closing));
    // A frame this path refuses: PING before the parameters.
    const u8 refused[] = {0x01U, 0x00U, 0x01U, 0x01U, 0x00U};
    sink.add("refused", refused, sizeof(refused));
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    // The connection's two record buffers are too large for a fuzzer's stack: one static run, rebuilt per input.
    alignas(Run) static unsigned char storage[sizeof(Run)];
    Run* const run = new (storage) Run();
    run->go(FuzzInput{data, size});
    run->~Run();
    return 0;
}
