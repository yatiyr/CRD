// crd-fuzz-net-qmux-early: QMux's 0-RTT (4.2a; docs/design/network-foundation.md#qmux) under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md). Each input resumes a real ticket and plays one of two parts.
//
// Input: a first byte (its low bit: server or client under test), then operations.
//   A server under test reads hostile early data from a real TLS client, answers at 0.5-RTT while that client holds
//   back its Finished, and goes through any records, time, reads, pings and closes.
//   A client under test sends real early data to a real TLS server that accepts or refuses it and answers with
//   anything: parameters that lower its limits, malformed frames, its 0.5-RTT or its 1-RTT data, in any order against
//   the application's writes.
//
// Oracles. Nothing is allocated after start(). What the side under test sends parses as records of at most 16382 bytes
// whose first frame is its transport parameters and whose frames are all ones QMux allows, nothing after its
// CONNECTION_CLOSE. Its state only moves forward. A client reports no datagram delivered before its server has
// answered, and a server's early marks never exceed what it read.
#include "qmux_fuzz_fixture.hpp"

#include <crd/crypto/provider.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>

#include <ctime>
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
constexpr u8 kTicketKey[kTlsTicketKeyBytes] = {7U};
constexpr u8 kContext[] = {'f', 'u', 'z', 'z'};
constexpr u8 kOtherContext[] = {'o', 't', 'h', 'e', 'r'};
constexpr u8 kLimits[] = {0x04U, 0x02U, 0x60U, 0x00U, 0x05U, 0x02U, 0x60U, 0x00U, 0x06U, 0x02U, 0x60U, 0x00U, 0x08U,
                          0x01U, 0x08U, 0x20U, 0x02U, 0x42U, 0x00U};

class Store final : public TlsTicketStore
{
public:
    void keep(const TlsBinding& binding, ConstSpan<u8> ticket) noexcept override
    {
        if (ticket.size() <= sizeof(m_ticket))
        {
            m_binding = binding;
            std::memcpy(m_ticket, ticket.data(), ticket.size());
            m_bytes = ticket.size();
        }
    }

    ConstSpan<u8> take(const TlsBinding& binding) noexcept override
    {
        if (m_bytes == 0U || !(m_binding == binding))
        {
            return {};
        }
        const usize bytes = m_bytes;
        m_bytes = 0U;
        return ConstSpan<u8>(m_ticket, bytes);
    }

    [[nodiscard]] bool holds() const noexcept { return m_bytes != 0U; }

private:
    TlsBinding m_binding;
    u8 m_ticket[4096] = {};
    usize m_bytes = 0U;
};

// Contexts that allow early data, under one replay filter: a run's own, so that a run's ticket is always fresh.
struct Fleet
{
    SigningKey key;
    TlsCredential credential;
    ReplayFilter filter;
    TlsContext client;
    TlsContext server;
    TlsContext plain;

    Fleet()
    {
        u8 seed[32] = {1U, 2U, 3U};
        CRD_FUZZ_REQUIRE(key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed)) == CryptoError::None);
        CRD_FUZZ_REQUIRE(credential.init(key, "game.cerid.test") == TlsError::None);
        CRD_FUZZ_REQUIRE(filter.init(64U, kTlsDefaultTicketLifetime + kTlsMaxTicketAgeSkew,
                                     static_cast<u64>(std::time(nullptr))) == CryptoError::None);
        const KeyId pin = credential.key_id();
        TlsConfig config;
        config.role = TlsRole::Client;
        config.groups = kGroups;
        config.signatures = kSignatures;
        config.aeads = kAeads;
        config.pinned = ConstSpan<KeyId>(&pin, 1U);
        config.server_name = "game.cerid.test";
        config.application_protocols = kProtocols;
        config.early_data = true;
        CRD_FUZZ_REQUIRE(client.init(config) == TlsError::None);
        TlsConfig server_config;
        server_config.role = TlsRole::Server;
        server_config.groups = kGroups;
        server_config.aeads = kAeads;
        server_config.credential = &credential;
        server_config.server_name = "game.cerid.test";
        server_config.application_protocols = kProtocols;
        server_config.ticket_key = ConstSpan<u8>(kTicketKey);
        CRD_FUZZ_REQUIRE(plain.init(server_config) == TlsError::None);
        server_config.early_data = true;
        server_config.early_data_filter = &filter;
        CRD_FUZZ_REQUIRE(server.init(server_config) == TlsError::None);
    }
};

QmuxConfig limits_of(const TlsContext& tls)
{
    QmuxConfig config;
    config.tls = &tls;
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
    config.datagrams.outcomes = true;
    config.padding_block = 64U;
    return config;
}

// A QMux side against a real TLS peer: what both parts share.
class Run
{
public:
    void go(FuzzInput input)
    {
        if ((input.byte() & 1U) == 0U)
        {
            server_part(input);
        }
        else
        {
            client_part(input);
        }
    }

private:
    // ---- A server under test ----

    void server_part(FuzzInput& input)
    {
        first_connection_to_qmux();
        CRD_FUZZ_REQUIRE(m_side.init(m_allocator, limits_of(m_fleet.server)));
        CRD_FUZZ_REQUIRE(m_side.start(m_now));
        CRD_FUZZ_REQUIRE(m_peer.init(m_fleet.client, &m_store) == TlsError::None);
        for (usize left = input.byte() % 4U; left != 0U; --left)
        {
            static_cast<void>(m_peer.send_early(input.run()));
        }
        peer_to_side();
        m_quiet = (input.byte() & 1U) == 0U;
        flush();
        const usize live = m_allocator.live_bytes();
        while (input.more())
        {
            server_step(input);
            flush();
            hold_invariants(live);
        }
    }

    void server_step(FuzzInput& input)
    {
        switch (input.byte() % 8U)
        {
            case 0U:
                static_cast<void>(m_peer.send(input.run()));
                break;
            case 1U:
            {
                const ConstSpan<u8> frames = input.run();
                u8 header[8];
                WireWriter writer{Span<u8>(header)};
                put_quic_varint(writer, frames.size());
                static_cast<void>(m_peer.send(ConstSpan<u8>(header, writer.written())));
                static_cast<void>(m_peer.send(frames));
                break;
            }
            case 2U:
                advance_time(input);
                break;
            case 3U:
                read_side(input.byte());
                break;
            case 4U:
                m_side.ping();
                break;
            case 5U:
                m_side.close(input.byte(), input.run(), m_now);
                break;
            case 6U:
                if (input.byte() < 8U)
                {
                    m_side.transport_ended((input.byte() & 1U) != 0U, m_now);
                }
                break;
            default:
                m_quiet = false; // the client's Finished goes
                break;
        }
    }

    // ---- A client under test ----

    void client_part(FuzzInput& input)
    {
        first_connection_to_raw();
        const u8 answer = input.byte() % 3U;
        const TlsContext& context = (answer == 2U) ? m_fleet.plain : m_fleet.server;
        const ConstSpan<u8> early_context =
            (answer == 2U) ? ConstSpan<u8>() : ConstSpan<u8>(answer == 0U ? kContext : kOtherContext,
                                                             answer == 0U ? sizeof(kContext) : sizeof(kOtherContext));
        QmuxConfig config = limits_of(m_fleet.client);
        config.tickets = &m_store;
        CRD_FUZZ_REQUIRE(m_side.init(m_allocator, config));
        CRD_FUZZ_REQUIRE(m_side.start(m_now));
        CRD_FUZZ_REQUIRE(m_peer.init(context, nullptr, {}, early_context) == TlsError::None);
        const usize live = m_allocator.live_bytes();
        while (input.more())
        {
            client_step(input);
            hold_invariants(live);
            if (m_side.handshake().early_data() == TlsEarlyData::Offered)
            {
                QuicDatagramReport report;
                CRD_FUZZ_REQUIRE(!m_side.datagrams().poll(report));
            }
        }
        m_quiet = false;
        flush();
    }

    void client_step(FuzzInput& input)
    {
        switch (input.byte() % 10U)
        {
            case 0U:
            {
                u64 id = 0U;
                static_cast<void>(m_side.streams().open(true, id));
                usize taken = 0U;
                static_cast<void>(m_side.streams().write(input.byte() % 16U, input.run(), false, taken));
                break;
            }
            case 1U:
            {
                u64 id = 0U;
                static_cast<void>(m_side.datagrams().queue(input.run(), m_now, id));
                break;
            }
            case 2U:
                peer_send_record(input.run());
                break;
            case 3U:
                peer_send(input.run());
                break;
            case 4U:
                m_quiet = (input.byte() & 1U) == 0U;
                peer_to_side();
                break;
            case 5U:
                side_to_peer();
                break;
            case 6U:
                advance_time(input);
                break;
            case 7U:
                read_side(input.byte());
                break;
            case 8U:
                m_side.close(input.byte(), input.run(), m_now);
                break;
            default:
                if (input.byte() < 8U)
                {
                    m_side.transport_ended((input.byte() & 1U) != 0U, m_now);
                }
                break;
        }
    }

    void peer_send_record(ConstSpan<u8> frames)
    {
        u8 header[8];
        WireWriter writer{Span<u8>(header)};
        put_quic_varint(writer, frames.size());
        peer_send(ConstSpan<u8>(header, writer.written()));
        peer_send(frames);
    }

    void peer_send(ConstSpan<u8> bytes)
    {
        if (m_peer.state() == TlsState::Established)
        {
            static_cast<void>(m_peer.send(bytes));
        }
        else
        {
            static_cast<void>(m_peer.send_half_rtt(bytes));
        }
    }

    // ---- What both parts share ----

    // A full handshake between two QMux connections, which leaves a ticket in the store.
    void first_connection_to_qmux()
    {
        CRD_FUZZ_REQUIRE(m_first.init(m_allocator, ticket_config()));
        CRD_FUZZ_REQUIRE(m_first_server.init(m_allocator, limits_of(m_fleet.server)));
        CRD_FUZZ_REQUIRE(m_first.start(m_now) && m_first_server.start(m_now));
        u8 bytes[16384];
        for (bool moved = true; moved;)
        {
            moved = false;
            for (usize got = m_first.take(Span<u8>(bytes), m_now); got != 0U;
                 got = m_first.take(Span<u8>(bytes), m_now))
            {
                moved = true;
                m_first_server.receive(ConstSpan<u8>(bytes, got), m_now);
            }
            for (usize got = m_first_server.take(Span<u8>(bytes), m_now); got != 0U;
                 got = m_first_server.take(Span<u8>(bytes), m_now))
            {
                moved = true;
                m_first.receive(ConstSpan<u8>(bytes, got), m_now);
            }
        }
        CRD_FUZZ_REQUIRE(m_store.holds());
    }

    // A full handshake with a real TLS server of the context the accepting peer uses, which states `kLimits`.
    void first_connection_to_raw()
    {
        CRD_FUZZ_REQUIRE(m_first.init(m_allocator, ticket_config()));
        CRD_FUZZ_REQUIRE(m_first.start(m_now));
        TlsSession raw;
        CRD_FUZZ_REQUIRE(raw.init(m_fleet.server, nullptr, {}, ConstSpan<u8>(kContext)) == TlsError::None);
        u8 frame[64];
        WireWriter writer{Span<u8>(frame)};
        const u64 type = crd::net::iana::kQuicFrameQxTransportParameters;
        put_quic_varint(writer, quic_varint_bytes(type) + quic_varint_bytes(sizeof(kLimits)) + sizeof(kLimits));
        put_quic_varint(writer, type);
        put_quic_varint(writer, sizeof(kLimits));
        writer.put_bytes(ConstSpan<u8>(kLimits));
        u8 bytes[16384];
        bool stated = false;
        for (bool moved = true; moved;)
        {
            moved = false;
            for (usize got = m_first.take(Span<u8>(bytes), m_now); got != 0U;
                 got = m_first.take(Span<u8>(bytes), m_now))
            {
                moved = true;
                CRD_FUZZ_REQUIRE(raw.receive(ConstSpan<u8>(bytes, got)) == TlsError::None);
            }
            if (raw.state() == TlsState::Established && !stated)
            {
                stated = true;
                CRD_FUZZ_REQUIRE(raw.send(ConstSpan<u8>(frame, writer.written())) == TlsError::None);
            }
            usize out = 0U;
            CRD_FUZZ_REQUIRE(raw.take(Span<u8>(bytes), out) == TlsError::None);
            if (out != 0U)
            {
                moved = true;
                m_first.receive(ConstSpan<u8>(bytes, out), m_now);
            }
        }
        CRD_FUZZ_REQUIRE(m_store.holds());
    }

    [[nodiscard]] QmuxConfig ticket_config()
    {
        QmuxConfig config = limits_of(m_fleet.client);
        config.tickets = &m_store;
        return config;
    }

    void advance_time(FuzzInput& input)
    {
        m_now += (u64{input.byte()} << 12U) * kMsNs;
        m_side.on_timeout(m_now);
    }

    void read_side(u8 which)
    {
        for (u64 id = which % 16U; id < 16U; id += 4U)
        {
            const QuicStreamRead read = m_side.streams().peek(id);
            CRD_FUZZ_REQUIRE(read.early_bytes <= read.data.size());
            static_cast<void>(m_side.streams().consume(id, read.data.size()));
        }
        ConstSpan<u8> held;
        while (m_side.datagrams().peek(held))
        {
            m_side.datagrams().pop();
        }
        QuicStreamEvent event;
        while (m_side.streams().poll(event))
        {
        }
    }

    void hold_invariants(usize live)
    {
        CRD_FUZZ_REQUIRE(static_cast<u8>(m_side.state()) >= m_state_floor);
        m_state_floor = static_cast<u8>(m_side.state());
        CRD_FUZZ_REQUIRE(m_allocator.live_bytes() == live);
    }

    void flush()
    {
        for (bool moved = true; moved;)
        {
            moved = !m_quiet && peer_to_side();
            moved = side_to_peer() || moved;
        }
    }

    bool peer_to_side()
    {
        u8 bytes[16384];
        bool moved = false;
        for (;;)
        {
            usize got = 0U;
            if (m_peer.state() == TlsState::Idle || m_peer.take(Span<u8>(bytes), got) != TlsError::None || got == 0U)
            {
                return moved;
            }
            moved = true;
            m_side.receive(ConstSpan<u8>(bytes, got), m_now);
        }
    }

    bool side_to_peer()
    {
        u8 bytes[16384];
        u8 plain[16384];
        bool moved = false;
        for (usize out = m_side.take(Span<u8>(bytes), m_now); out != 0U; out = m_side.take(Span<u8>(bytes), m_now))
        {
            moved = true;
            if (m_peer.state() != TlsState::Failed && m_peer.state() != TlsState::Closed)
            {
                static_cast<void>(m_peer.receive(ConstSpan<u8>(bytes, out)));
            }
            for (;;)
            {
                usize got = 0U;
                const bool early =
                    m_peer.state() == TlsState::Handshaking && m_peer.early_data() == TlsEarlyData::Accepted;
                const Span<u8> room(plain);
                const TlsError error = early ? m_peer.read_early(room, got) : m_peer.read(room, got);
                if (error != TlsError::None || got == 0U)
                {
                    break;
                }
                m_oracle.plaintext(ConstSpan<u8>(plain, got));
            }
        }
        return moved;
    }

    crd::fuzz::BudgetAllocator m_allocator;
    Fleet m_fleet;
    Store m_store;
    QmuxConnection m_first;
    QmuxConnection m_first_server;
    QmuxConnection m_side;
    TlsSession m_peer;
    QmuxRecordOracle m_oracle;
    u64 m_now = kStartNs;
    u8 m_state_floor = 0U;
    bool m_quiet = false;
};
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A server under test: one early run holding the parameters and a stream frame, Finished held back, then released.
    const u8 server[] = {0x00U, 0x01U, 0x00U, 0x14U, 0x13U, 0xFFU, 0x51U, 0x53U, 0x30U, 0x0DU, 0x0AU, 0x0DU,
                         0x0AU, 0x04U, 0x04U, 0x02U, 0x44U, 0x00U, 0x0BU, 0x00U, 0x03U, 0xAAU, 0xBBU, 0xCCU,
                         0x00U, 0x03U, 0x00U, 0x07U};
    sink.add("server", server, sizeof(server));
    // A client under test: writes early, is accepted, then hears lowered limits.
    const u8 accepted[] = {0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x41U, 0x42U, 0x43U, 0x44U, 0x05U,
                           0x02U, 0x00U, 0x0DU, 0xFFU, 0x51U, 0x53U, 0x30U, 0x0DU, 0x0AU, 0x0DU, 0x0AU,
                           0x04U, 0x04U, 0x02U, 0x40U, 0x40U, 0x04U, 0x01U, 0x05U};
    sink.add("accepted", accepted, sizeof(accepted));
    // A client under test whose server refuses its early data, in two ways.
    const u8 refused[] = {0x01U, 0x01U, 0x00U, 0x00U, 0x00U, 0x02U,
                          0x41U, 0x42U, 0x05U, 0x04U, 0x00U, 0x05U, 0x07U, 0x00U};
    sink.add("refused", refused, sizeof(refused));
    const u8 plain[] = {0x01U, 0x02U, 0x01U, 0x00U, 0x00U, 0x10U, 0x05U, 0x04U, 0x00U, 0x05U};
    sink.add("plain", plain, sizeof(plain));
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    // The connections' record buffers are too large for a fuzzer's stack: one static run, rebuilt per input.
    alignas(Run) static unsigned char storage[sizeof(Run)];
    Run* const run = new (storage) Run();
    run->go(FuzzInput{data, size});
    run->~Run();
    return 0;
}
