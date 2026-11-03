// crd-fuzz-crypto-tls: crd-crypto's TLS 1.3 sessions (NET.2; docs/design/network-foundation.md#tls-sessions) fed
// hostile bytes under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. Byte 0 picks the session (bit 0: 0 a server that has received nothing, 1 a client that has sent its
// ClientHello), byte 1 the size of each delivery (0: all at once), the rest are the bytes the peer "sent". With bit 1
// set the rest is instead the ticket a client's store hands out (a ticket corrupted at rest), and nothing is received.
// The server seals and opens tickets under a fixed key, so a seed can lead its identity with that key's name. With bit
// 2 set the input is instead a compressed Certificate (RFC 8879) for the client's decoder: byte 1 its algorithm, three
// bytes its claimed length, then the compressed bytes. With bit 3 set the session is instead a QUIC handshake (RFC
// 9001; bit 0 as above) and the rest is CRYPTO data: pieces of a level byte (taken modulo three), a 16-bit length and
// that many bytes, each provided at its level. With bit 4 set the session runs Encrypted Client Hello (RFC 9849): a
// client given the rest as its ECHConfigList (a stream session and a QUIC handshake), or a server holding ECH keys fed
// the rest as above. With bit 5 set the session runs early data (0-RTT, decision S17), over records or, with bit 3,
// over QUIC: a client resumes a real ticket of the early data server (a full connection in-process first) and offers
// early data, then the rest is what its "server" sent; or a server accepts a real client's ticket and early data and
// the rest is what that "client" sent after them.
//
// Oracles, none of them the provider's verdict on one call:
//   * neither session is ever established: a server needs a Finished under keys derived from its own fresh share, and
//     a client a CertificateVerify by its pinned key, which no fixed input can hold;
//   * receive() answers only None, Failed, PeerRefused, Full or Closed while the session handshakes, NotReady once it
//     failed and Closed once the peer closed it; a failed or closed session stays so, and a session closed by its
//     peer (a close_notify, after a user_canceled or not) has sent no alert;
//   * the session never holds more than kTlsMaxBufferedBytes of received bytes, and its outgoing bytes are bounded;
//   * send(), read(), close() and update_keys() are refused outside an established session (Closed once closed);
//   * a client offered any ticket starts its handshake all the same, and keeps no ticket before it is established;
//   * a QUIC handshake is never established either; provide() answers only None, Failed, PeerRefused or Full while it
//     handshakes and NotReady once it failed, which is final; its CRYPTO data waiting to be sent stays bounded; a
//     client never holds an Application secret and a server never its client's (each needs a Finished under keys
//     from the handshake's own share); every secret it holds derives keys once; a sent alert names a level; and the
//     transport parameters it received are at most what the input carried;
//   * a client given an ECHConfigList: its context refuses exactly the lists that are not a 2-byte length holding the
//     rest, tiled by ECHConfigs of a 2-byte version and length, one of version 0xfe0d (restated), at most
//     kTlsMaxEchConfigListBytes; a session of an accepted context either refuses to start (BadConfig, nothing to send)
//     or sends a ClientHello whose outer encrypted_client_hello carries a 32-byte encapsulation and which names
//     another server than its own, and no byte it sends at the Initial level holds its server's name; under a
//     generation refusing ChaCha20-Poly1305 (decision S21) a session refuses to start with PolicyRefused exactly when
//     no configuration is left (restated: version 0xfe0d, DHKEM(X25519), a whole suite list with a suite the
//     provider could pick and none of ChaCha20-Poly1305), and otherwise its hello never encrypts with it;
//   * a server holding ECH keys reports no ECH (it needs a client's Finished) whatever hello reaches it;
//   * a ticket record a client's store hands out offers early data only when it is a record of a real session (no fixed
//     input is), so the client offers none;
//   * a client that offered early data stays Offered or is Rejected, never Accepted (that needs its server's Finished),
//     sends early data only while Offered and holds its 0-RTT keys over QUIC only then; a server that accepted a real
//     client's early data returns exactly that client's early bytes from read_early() and none more (no input holds
//     the client's early keys), never from read(), and is never established;
//   * a compressed Certificate decodes only for zstd or zlib, to a claim within [4, kTlsMaxCertificateBytes] that, for
//     zlib, its bytes can reach (restated here: DEFLATE's 1032 bytes per byte between zlib's 6 framing bytes), and a
//     refused claim leaves the output untouched; a zlib decoding carries the Adler-32 of its output (restated) and a
//     decoding is the same twice.
#include "../policy_test_signer.hpp"

#include <crd/fuzz/harness.hpp>

#include <crd/compression/deflate.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

namespace
{
using crd::u16;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::Secp256r1Mlkem768,
                                   AlgorithmId::Secp384r1Mlkem1024, AlgorithmId::X25519, AlgorithmId::Secp256r1,
                                   AlgorithmId::Secp384r1};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::MlDsa44, AlgorithmId::MlDsa65, AlgorithmId::MlDsa87,
                                       AlgorithmId::Ed25519, AlgorithmId::EcdsaSecp256r1Sha256,
                                       AlgorithmId::EcdsaSecp384r1Sha384};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};
constexpr const char* kName    = "game.cerid.test";
constexpr crd::containers::StringView kProtocols[] = {"cerid-game/1"};
constexpr u8 kParameters[] = {0x04U, 0x04U, 0x80U, 0x10U, 0x00U, 0x00U}; // initial_max_data, as a transport sends it
constexpr usize kQuicPiece = 3U; // a CRYPTO piece's level byte and 16-bit length
constexpr const char* kPublicName = "front.cerid.test"; // the fuzz server's ECH public name
constexpr u16         kEchVersion = 0xFE0DU;
constexpr u8          kEchConfigIds[] = {1U, 2U};        // the fuzz server's current and previous ECH keys
constexpr u8 kEarlyMessage[] = {'w', 'a', 'l', 'k', ' ', 'n', 'o', 'r', 't', 'h'}; // a real client's early data
constexpr u8 kEarlyContext[] = {0x04U, 0x04U, 0x80U, 0x10U, 0x00U, 0x00U, 'g'};    // the early servers' data context
constexpr crd::u64 kEarlyPeriod = kTlsDefaultTicketLifetime + kTlsMaxTicketAgeSkew;

// The fuzz server's ticket key: the bytes 0x40, 0x41, ... 0x5F.
struct TicketKey
{
    u8 bytes[kTlsTicketKeyBytes] = {};

    constexpr TicketKey() noexcept
    {
        for (usize i = 0U; i < sizeof(bytes); ++i)
        {
            bytes[i] = static_cast<u8>(0x40U + i);
        }
    }
};
constexpr TicketKey kTicketKey;

// The outgoing bytes a handshake can leave in a session: a server's first flight with an Ed25519 credential, or a
// client's ClientHello, and an alert; far below the bound.
constexpr usize kMaxFlight = 8192U;

constexpr u8    kUntouched      = 0xA5U; // what a refused decoding leaves in the output
constexpr usize kClaimLimit     = kTlsMaxCertificateBytes + 1U; // claims above the bound are cut to this
constexpr usize kCompressedHead = 5U;    // mode, algorithm and the 24-bit claim

// The fuzz server's ECH key `index`: the private key 0x60 + index, 0x61 + index, ...
void make_ech_key(TlsEchKey& key, usize index) noexcept
{
    u8 secret[kTlsEchKeyBytes];
    for (usize i = 0U; i < sizeof(secret); ++i)
    {
        secret[i] = static_cast<u8>(0x60U + index + i);
    }
    (void)key.init(ConstSpan<u8>(secret), kEchConfigIds[index], kPublicName, 32U);
}

// One server and one client context, built once: an Ed25519 credential from a fixed seed, every group, signature and
// AEAD, the client pinning the server's key; the same pair naming an application protocol, for QUIC; and a server
// holding two ECH keys (a current and a previous one).
struct Contexts
{
    SigningKey    key;
    TlsCredential credential;
    TlsEchKey     ech_keys[2];
    TlsContext    server;
    TlsContext    client;
    TlsContext    quic_server;
    TlsContext    quic_client;
    TlsContext    ech_server;
    ReplayFilter early_filter; // the early data servers', made afresh for every input
    TlsContext early_server;
    TlsContext early_client;
    TlsContext quic_early_server;
    TlsContext quic_early_client;
    crd::crypto::test::TestInstallation no_chacha; // a generation refusing ChaCha20-Poly1305 (decision S21)
    bool          ready = false;

    Contexts() noexcept
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        if (key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) != CryptoError::None ||
            credential.init(key, kName) != TlsError::None)
        {
            return;
        }
        TlsConfig config;
        config.role        = TlsRole::Server;
        config.groups      = kGroups;
        config.aeads       = kAeads;
        config.credential  = &credential;
        config.server_name = kName;
        config.ticket_key  = ConstSpan<u8>(kTicketKey.bytes, sizeof(kTicketKey.bytes));
        if (server.init(config) != TlsError::None)
        {
            return;
        }
        config.application_protocols = kProtocols;
        if (quic_server.init(config) != TlsError::None)
        {
            return;
        }
        const auto    now        = static_cast<crd::u64>(std::time(nullptr));
        PolicyContent generation = crd::crypto::test::accepting_all(1U, now - 60U);
        crd::crypto::test::refuse_rule(generation, AlgorithmId::Chacha20Poly1305);
        if (no_chacha.apply(generation, now) != PolicyError::None)
        {
            return;
        }
        make_ech_key(ech_keys[0], 0U);
        make_ech_key(ech_keys[1], 1U);
        const TlsEchKey* current[]  = {&ech_keys[0]};
        const TlsEchKey* previous[] = {&ech_keys[1]};
        config.application_protocols = {};
        config.ech_keys              = current;
        config.previous_ech_keys     = previous;
        if (ech_server.init(config) != TlsError::None)
        {
            return;
        }
        config.ech_keys          = {};
        config.previous_ech_keys = {};
        config.early_data = true;
        config.early_data_filter = &early_filter;
        if (early_filter.init(16U, kEarlyPeriod, static_cast<crd::u64>(std::time(nullptr))) != CryptoError::None ||
            early_server.init(config) != TlsError::None)
        {
            return;
        }
        config.application_protocols = kProtocols;
        if (quic_early_server.init(config) != TlsError::None)
        {
            return;
        }
        config.early_data = false;
        config.early_data_filter = nullptr;
        const KeyId pin    = credential.key_id();
        config.role        = TlsRole::Client;
        config.credential  = nullptr;
        config.ticket_key  = {};
        config.signatures  = kSignatures;
        config.pinned      = ConstSpan<KeyId>(&pin, 1U);
        ready              = quic_client.init(config) == TlsError::None;
        config.early_data = true;
        ready = ready && quic_early_client.init(config) == TlsError::None;
        config.application_protocols = {};
        ready = ready && early_client.init(config) == TlsError::None;
        config.early_data = false;
        ready              = ready && client.init(config) == TlsError::None;
    }
};

// A client's store that hands out the input once as its ticket and counts the tickets it is given.
class InputStore final : public TlsTicketStore
{
public:
    explicit InputStore(ConstSpan<u8> ticket) noexcept : m_ticket(ticket) {}

    void keep(const TlsBinding&, ConstSpan<u8>) noexcept override { ++kept; }

    ConstSpan<u8> take(const TlsBinding&) noexcept override
    {
        ++takes;
        return takes == 1U ? m_ticket : ConstSpan<u8>();
    }

    usize kept  = 0U;
    usize takes = 0U;

private:
    ConstSpan<u8> m_ticket;
};

Contexts& contexts() noexcept
{
    static Contexts instance;
    return instance;
}

// A client's store that keeps the newest real ticket its server issues and hands it out once.
class KeepStore final : public TlsTicketStore
{
public:
    void keep(const TlsBinding&, ConstSpan<u8> ticket) noexcept override
    {
        m_size = ticket.size() <= sizeof(m_ticket) ? ticket.size() : 0U;
        std::memcpy(m_ticket, ticket.data(), m_size);
    }

    ConstSpan<u8> take(const TlsBinding&) noexcept override
    {
        const usize size = m_size;
        m_size = 0U;
        return ConstSpan<u8>(m_ticket, size);
    }

    [[nodiscard]] bool holds() const noexcept { return m_size != 0U; }

private:
    u8 m_ticket[8192] = {};
    usize m_size = 0U;
};

u8 g_moved[kTlsMaxBufferedBytes];

// Moves what one end waits to send to the other; false when either refuses.
bool move(TlsSession& from, TlsSession& to) noexcept
{
    usize written = 0U;
    return from.take(Span<u8>(g_moved), written) == TlsError::None &&
           (written == 0U || to.receive(ConstSpan<u8>(g_moved, written)) == TlsError::None);
}

bool move(QuicHandshake& from, QuicHandshake& to) noexcept
{
    for (const QuicLevel level : {QuicLevel::Initial, QuicLevel::Handshake, QuicLevel::Application})
    {
        usize written = 0U;
        if (from.take(level, Span<u8>(g_moved), written) != TlsError::None ||
            (written != 0U && to.provide(level, ConstSpan<u8>(g_moved, written)) != TlsError::None))
        {
            return false;
        }
    }
    return true;
}

// A real ticket of the early data server in `store`, over records or QUIC: a full connection in-process, after the
// server's filter was made afresh.
bool earn_ticket(KeepStore& store, bool quic) noexcept
{
    Contexts& shared = contexts();
    if (shared.early_filter.init(16U, kEarlyPeriod, static_cast<crd::u64>(std::time(nullptr))) != CryptoError::None)
    {
        return false;
    }
    if (quic)
    {
        QuicHandshake client;
        QuicHandshake server;
        if (client.init(shared.quic_early_client, ConstSpan<u8>(kParameters), &store) != TlsError::None ||
            server.init(shared.quic_early_server, ConstSpan<u8>(kParameters), nullptr, {},
                        ConstSpan<u8>(kEarlyContext)) != TlsError::None)
        {
            return false;
        }
        // The server's ticket follows the client's Finished.
        for (int round = 0; round < 8 && !store.holds(); ++round)
        {
            if (!move(client, server) || !move(server, client))
            {
                return false;
            }
        }
        return store.holds();
    }
    TlsSession client;
    TlsSession server;
    if (client.init(shared.early_client, &store) != TlsError::None ||
        server.init(shared.early_server, nullptr, {}, ConstSpan<u8>(kEarlyContext)) != TlsError::None)
    {
        return false;
    }
    for (int round = 0; round < 8 && server.state() != TlsState::Established; ++round)
    {
        if (!move(client, server) || !move(server, client))
        {
            return false;
        }
    }
    u8 byte = 0U;
    usize written = 0U;
    return move(server, client) && client.read(Span<u8>(&byte, 1U), written) == TlsError::None && store.holds();
}

// The oracles of a client that offered early data, over records or QUIC (`keys`: it holds its 0-RTT keys).
void check_early_client(const detail::TlsHandshakeState& state, bool keys) noexcept
{
    CRD_FUZZ_REQUIRE(state.early_data() == TlsEarlyData::Offered || state.early_data() == TlsEarlyData::Rejected);
    CRD_FUZZ_REQUIRE(state.early_data() == TlsEarlyData::Offered || !keys);
    CRD_FUZZ_REQUIRE(state.state() != TlsState::Established);
}

void drain(TlsSession& session) noexcept
{
    u8    out[kMaxFlight];
    usize written = 0U;
    CRD_FUZZ_REQUIRE(session.pending() <= kMaxFlight);
    CRD_FUZZ_REQUIRE(session.take(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CRD_FUZZ_REQUIRE(session.pending() == 0U);
}

void feed(TlsSession& session, ConstSpan<u8> bytes, usize chunk) noexcept
{
    for (usize at = 0U; at < bytes.size();)
    {
        const usize    size   = chunk == 0U ? bytes.size() - at : std::min(chunk, bytes.size() - at);
        const TlsState before = session.state();
        const TlsError result = session.receive(bytes.subspan(at, size));
        at += size;
        if (before == TlsState::Failed || before == TlsState::Closed)
        {
            CRD_FUZZ_REQUIRE(result == (before == TlsState::Failed ? TlsError::NotReady : TlsError::Closed));
            CRD_FUZZ_REQUIRE(session.state() == before);
            continue;
        }
        CRD_FUZZ_REQUIRE(result == TlsError::None || result == TlsError::Failed || result == TlsError::PeerRefused ||
                         result == TlsError::Full || result == TlsError::Closed);
        CRD_FUZZ_REQUIRE((result == TlsError::Failed || result == TlsError::PeerRefused) ==
                         (session.state() == TlsState::Failed));
        CRD_FUZZ_REQUIRE((result == TlsError::Closed) == (session.state() == TlsState::Closed));
        CRD_FUZZ_REQUIRE(session.state() != TlsState::Closed || !session.alert_sent().present);
        CRD_FUZZ_REQUIRE(session.state() != TlsState::Established && session.state() != TlsState::Idle);
        drain(session);
    }
    const TlsError refused     = session.state() == TlsState::Closed ? TlsError::Closed : TlsError::NotReady;
    u8             scratch[16] = {};
    usize          written     = 0U;
    CRD_FUZZ_REQUIRE(session.send(ConstSpan<u8>(scratch, sizeof(scratch))) == refused);
    CRD_FUZZ_REQUIRE(session.read(Span<u8>(scratch, sizeof(scratch)), written) == refused);
    CRD_FUZZ_REQUIRE(written == 0U);
    CRD_FUZZ_REQUIRE(session.close() == refused);
    CRD_FUZZ_REQUIRE(session.update_keys(true) == refused);
    CRD_FUZZ_REQUIRE(session.pending() == 0U || session.state() == TlsState::Failed);
    CRD_FUZZ_REQUIRE(session.key_updates_sent() == 0U && session.key_updates_received() == 0U);
    CRD_FUZZ_REQUIRE(session.group() == AlgorithmId{} && session.peer_key() == KeyId{});
}
// The CRYPTO data a QUIC handshake waits to send, taken level by level; bounded like a session's outgoing bytes.
void drain(QuicHandshake& handshake) noexcept
{
    usize total = 0U;
    for (const QuicLevel level : {QuicLevel::Initial, QuicLevel::Handshake, QuicLevel::Application})
    {
        u8    out[kMaxFlight];
        usize written = 0U;
        total += handshake.pending(level);
        CRD_FUZZ_REQUIRE(handshake.pending(level) <= kMaxFlight);
        CRD_FUZZ_REQUIRE(handshake.take(level, Span<u8>(out, sizeof(out)), written) == TlsError::None);
        CRD_FUZZ_REQUIRE(handshake.pending(level) == 0U);
    }
    CRD_FUZZ_REQUIRE(total <= kTlsMaxBufferedBytes);
}

// Provides `data`'s CRYPTO pieces to a QUIC handshake of `client`'s role, checking every answer. A handshake that
// already read a real peer's hello holds that peer's transport parameters, `received`, whatever follows.
void feed_quic(QuicHandshake& handshake, bool client, ConstSpan<u8> data, ConstSpan<u8> received = {}) noexcept
{
    for (usize at = 0U; at + kQuicPiece <= data.size();)
    {
        const auto  level  = static_cast<QuicLevel>(data[at] % 3U);
        const usize length = std::min((usize{data[at + 1U]} << 8U) | data[at + 2U], data.size() - at - kQuicPiece);
        const TlsState before = handshake.state();
        const TlsError result = handshake.provide(level, data.subspan(at + kQuicPiece, length));
        at += kQuicPiece + length;
        if (before == TlsState::Failed)
        {
            CRD_FUZZ_REQUIRE(result == TlsError::NotReady && handshake.state() == TlsState::Failed);
            continue;
        }
        CRD_FUZZ_REQUIRE(result == TlsError::None || result == TlsError::Failed || result == TlsError::PeerRefused ||
                         result == TlsError::Full);
        CRD_FUZZ_REQUIRE((result == TlsError::Failed || result == TlsError::PeerRefused) ==
                         (handshake.state() == TlsState::Failed));
        CRD_FUZZ_REQUIRE(handshake.state() == TlsState::Handshaking || handshake.state() == TlsState::Failed);
        CRD_FUZZ_REQUIRE(!handshake.alert_sent().present ||
                         static_cast<usize>(handshake.alert_level()) < kQuicLevels);
        const ConstSpan<u8> parameters = handshake.peer_transport_parameters();
        CRD_FUZZ_REQUIRE(received.empty() ? parameters.size() <= data.size()
                                          : parameters.size() == received.size() &&
                                                std::memcmp(parameters.data(), received.data(), received.size()) == 0);
        drain(handshake);
    }
    CRD_FUZZ_REQUIRE(!handshake.has_keys(QuicLevel::Application, client ? QuicSide::Server : QuicSide::Client));
    CRD_FUZZ_REQUIRE(!client || !handshake.has_keys(QuicLevel::Application, QuicSide::Client));
    // A server that accepted a real client's early data settled its handshake's parameters then.
    CRD_FUZZ_REQUIRE(handshake.early_data() == TlsEarlyData::Accepted ||
                     (handshake.group() == AlgorithmId{} && handshake.peer_key() == KeyId{}));
    CRD_FUZZ_REQUIRE(!handshake.has_keys(QuicLevel::Early, QuicSide::Server));
    for (const QuicLevel level : {QuicLevel::Handshake, QuicLevel::Application, QuicLevel::Early})
    {
        for (const QuicSide side : {QuicSide::Client, QuicSide::Server})
        {
            QuicPacketKey packet;
            QuicHeaderKey header;
            const bool    held = handshake.has_keys(level, side);
            CRD_FUZZ_REQUIRE((handshake.derive_keys(level, side, QuicVersion::V1, packet, header) == TlsError::None) ==
                             held);
            CRD_FUZZ_REQUIRE(packet.ready() == held && header.ready() == held && !handshake.has_keys(level, side));
        }
    }
}

// Early data over records: a client offering it on a real ticket and fed the rest as its server's bytes, or a server
// that accepted a real client's early data fed the rest after it, `chunk` bytes at a time.
void early_records(bool client, ConstSpan<u8> rest, usize chunk) noexcept
{
    const Contexts& shared = contexts();
    KeepStore store;
    CRD_FUZZ_REQUIRE(earn_ticket(store, false));
    if (client)
    {
        TlsSession session;
        CRD_FUZZ_REQUIRE(session.init(shared.early_client, &store) == TlsError::None);
        CRD_FUZZ_REQUIRE(session.early_data() == TlsEarlyData::Offered);
        CRD_FUZZ_REQUIRE(session.early_data_room() == kTlsEarlyDataBytes);
        CRD_FUZZ_REQUIRE(session.send_early(ConstSpan<u8>(kEarlyMessage)) == TlsError::None);
        drain(session);
        feed(session, rest, chunk);
        check_early_client(session, false);
        const bool open = session.early_data() == TlsEarlyData::Offered && session.state() == TlsState::Handshaking;
        CRD_FUZZ_REQUIRE(
            session.early_data_room() ==
            (session.early_data() == TlsEarlyData::Offered ? kTlsEarlyDataBytes - sizeof(kEarlyMessage) : 0U));
        CRD_FUZZ_REQUIRE(session.send_early(ConstSpan<u8>(kEarlyMessage)) ==
                         (open ? TlsError::None : TlsError::NotReady));
        return;
    }
    TlsSession peer;
    TlsSession session;
    CRD_FUZZ_REQUIRE(peer.init(shared.early_client, &store) == TlsError::None);
    CRD_FUZZ_REQUIRE(peer.send_early(ConstSpan<u8>(kEarlyMessage)) == TlsError::None);
    CRD_FUZZ_REQUIRE(session.init(shared.early_server, nullptr, {}, ConstSpan<u8>(kEarlyContext)) == TlsError::None);
    CRD_FUZZ_REQUIRE(move(peer, session));
    CRD_FUZZ_REQUIRE(session.early_data() == TlsEarlyData::Accepted && session.resumed());
    u8 early[sizeof(kEarlyMessage) + 1U] = {};
    usize written = 0U;
    CRD_FUZZ_REQUIRE(session.read_early(Span<u8>(early), written) == TlsError::None);
    CRD_FUZZ_REQUIRE(written == sizeof(kEarlyMessage) && std::memcmp(early, kEarlyMessage, written) == 0);
    drain(session);
    for (usize at = 0U; at < rest.size();)
    {
        const usize size = chunk == 0U ? rest.size() - at : std::min(chunk, rest.size() - at);
        const TlsState before = session.state();
        const TlsError taken = session.receive(rest.subspan(at, size));
        at += size;
        CRD_FUZZ_REQUIRE(before != TlsState::Handshaking
                             ? taken == (before == TlsState::Failed ? TlsError::NotReady : TlsError::Closed)
                             : taken == TlsError::None || taken == TlsError::Full);
        if (session.state() != TlsState::Handshaking)
        {
            continue;
        }
        // Whatever came, no more early byte can be read: only the real client holds its early keys.
        const TlsError read = session.read_early(Span<u8>(early), written);
        CRD_FUZZ_REQUIRE(written == 0U);
        CRD_FUZZ_REQUIRE(read == TlsError::None || read == TlsError::Failed || read == TlsError::PeerRefused ||
                         read == TlsError::Closed);
        CRD_FUZZ_REQUIRE((read == TlsError::Failed || read == TlsError::PeerRefused) ==
                         (session.state() == TlsState::Failed));
        CRD_FUZZ_REQUIRE(session.state() != TlsState::Established);
        drain(session);
    }
    u8 scratch[16] = {};
    CRD_FUZZ_REQUIRE(session.read(Span<u8>(scratch), written) ==
                     (session.state() == TlsState::Closed ? TlsError::Closed : TlsError::NotReady));
    CRD_FUZZ_REQUIRE(session.send(ConstSpan<u8>(scratch)) ==
                     (session.state() == TlsState::Closed ? TlsError::Closed : TlsError::NotReady));
    CRD_FUZZ_REQUIRE(session.early_data() == TlsEarlyData::Accepted);
}

// Early data over QUIC: a client whose 0-RTT keys are ready on a real ticket, fed the rest as CRYPTO data; or a server
// that accepted a real client's Initial with its early data, fed the rest after it.
void early_quic(bool client, ConstSpan<u8> rest) noexcept
{
    const Contexts& shared = contexts();
    KeepStore store;
    CRD_FUZZ_REQUIRE(earn_ticket(store, true));
    QuicHandshake handshake;
    if (client)
    {
        CRD_FUZZ_REQUIRE(handshake.init(shared.quic_early_client, ConstSpan<u8>(kParameters), &store) ==
                         TlsError::None);
        CRD_FUZZ_REQUIRE(handshake.early_data() == TlsEarlyData::Offered);
        CRD_FUZZ_REQUIRE(handshake.has_keys(QuicLevel::Early, QuicSide::Client));
        const ConstSpan<u8> remembered = handshake.remembered_transport_parameters();
        CRD_FUZZ_REQUIRE(remembered.size() == sizeof(kParameters) &&
                         std::memcmp(remembered.data(), kParameters, sizeof(kParameters)) == 0);
        drain(handshake);
        // feed_quic derives every key it holds: the 0-RTT keys' check comes before it.
        feed_quic(handshake, true, rest);
        check_early_client(handshake, false);
        return;
    }
    QuicHandshake peer;
    CRD_FUZZ_REQUIRE(peer.init(shared.quic_early_client, ConstSpan<u8>(kParameters), &store) == TlsError::None);
    CRD_FUZZ_REQUIRE(handshake.init(shared.quic_early_server, ConstSpan<u8>(kParameters), nullptr, {},
                                    ConstSpan<u8>(kEarlyContext)) == TlsError::None);
    CRD_FUZZ_REQUIRE(move(peer, handshake));
    CRD_FUZZ_REQUIRE(handshake.early_data() == TlsEarlyData::Accepted && handshake.resumed());
    CRD_FUZZ_REQUIRE(handshake.has_keys(QuicLevel::Early, QuicSide::Client));
    drain(handshake);
    feed_quic(handshake, false, rest, ConstSpan<u8>(kParameters));
    CRD_FUZZ_REQUIRE(handshake.early_data() == TlsEarlyData::Accepted);
}

// Whether an ECHConfigList has RFC 9849 section 4's outer shape, restated: a 2-byte length holding exactly the rest,
// tiled by ECHConfigs of a 2-byte version and a 2-byte length, one of them of version 0xfe0d, within the bound.
bool ech_list_shaped(ConstSpan<u8> list) noexcept
{
    if (list.size() < 2U || list.size() > kTlsMaxEchConfigListBytes ||
        ((usize{list[0]} << 8U) | list[1]) != list.size() - 2U || list.size() == 2U)
    {
        return false;
    }
    bool current = false;
    for (usize at = 2U; at < list.size();)
    {
        if (list.size() - at < 4U)
        {
            return false;
        }
        const usize length = (usize{list[at + 2U]} << 8U) | list[at + 3U];
        if (length > list.size() - at - 4U)
        {
            return false;
        }
        current = current || ((u16{list[at]} << 8U) | list[at + 1U]) == kEchVersion;
        at += 4U + length;
    }
    return current;
}

// Decision S21 restated for a generation refusing ChaCha20-Poly1305 alone: whether a shaped `list` holds a
// configuration a client may offer: version 0xfe0d, KEM DHKEM(X25519, HKDF-SHA256), a public key and a suite list of
// whole 4-byte suites inside it, at least one suite of HKDF-SHA256 with AES-128-GCM, AES-256-GCM or ChaCha20-Poly1305
// (what the provider can pick), and none of those with ChaCha20-Poly1305.
bool ech_list_allowed(ConstSpan<u8> list) noexcept
{
    for (usize at = 2U; at < list.size();)
    {
        const u16   version = static_cast<u16>((list[at] << 8U) | list[at + 1U]);
        const usize length  = (usize{list[at + 2U]} << 8U) | list[at + 3U];
        const u8*   config  = list.data() + at + 4U;
        at += 4U + length;
        if (version != kEchVersion || length < 5U || ((u16{config[1]} << 8U) | config[2]) != 0x0020U)
        {
            continue;
        }
        const usize key = (usize{config[3]} << 8U) | config[4];
        if (length < 7U + key)
        {
            continue;
        }
        const usize suites = (usize{config[5U + key]} << 8U) | config[6U + key];
        if (length < 7U + key + suites || suites % 4U != 0U)
        {
            continue;
        }
        bool pickable = false;
        bool refused  = false;
        for (usize s = 7U + key; s < 7U + key + suites; s += 4U)
        {
            const u16 kdf  = static_cast<u16>((config[s] << 8U) | config[s + 1U]);
            const u16 aead = static_cast<u16>((config[s + 2U] << 8U) | config[s + 3U]);
            if (kdf == 1U && aead >= 1U && aead <= 3U)
            {
                pickable = true;
                refused  = refused || aead == 3U;
            }
        }
        if (pickable && !refused)
        {
            return true;
        }
    }
    return false;
}

bool holds(ConstSpan<u8> bytes, const char* text) noexcept
{
    const usize length = std::strlen(text);
    for (usize at = 0U; at + length <= bytes.size(); ++at)
    {
        if (std::memcmp(bytes.data() + at, text, length) == 0)
        {
            return true;
        }
    }
    return false;
}

// A ClientHello message (handshake header first) read for ECH: whether it names `name` and carries an outer
// encrypted_client_hello with a 32-byte encapsulation, and its HPKE AEAD, restated from RFC 9846 section 4.1 and RFC
// 9849 section 5.
bool hello_offers_ech(ConstSpan<u8> message, bool& names, u16& aead) noexcept
{
    names = false;
    aead  = 0U;
    if (message.size() < 4U + 2U + 32U + 1U || message[0] != 1U)
    {
        return false;
    }
    usize at = 4U + 2U + 32U;
    at += 1U + message[at];
    if (at + 2U > message.size())
    {
        return false;
    }
    at += 2U + ((usize{message[at]} << 8U) | message[at + 1U]);
    if (at + 1U > message.size())
    {
        return false;
    }
    at += 1U + message[at] + 2U;
    bool offered = false;
    while (at + 4U <= message.size())
    {
        const u16   type   = static_cast<u16>((message[at] << 8U) | message[at + 1U]);
        const usize length = (usize{message[at + 2U]} << 8U) | message[at + 3U];
        const u8*   body   = message.data() + at + 4U;
        if (at + 4U + length > message.size())
        {
            return false;
        }
        if (type == 0U && length >= 5U)
        {
            const usize size = (usize{body[3]} << 8U) | body[4];
            names            = size == std::strlen(kName) && std::memcmp(body + 5U, kName, size) == 0;
        }
        if (type == kEchVersion && length >= 8U)
        {
            offered = body[0] == 0U && ((usize{body[6]} << 8U) | body[7]) == 32U;
            aead    = static_cast<u16>((body[3] << 8U) | body[4]);
        }
        at += 4U + length;
    }
    return offered;
}

// A client given `list` as its ECHConfigList, over records and over QUIC.
void offer_ech(ConstSpan<u8> list) noexcept
{
    const Contexts& shared = contexts();
    const KeyId     pin    = shared.credential.key_id();
    TlsConfig       config;
    config.role        = TlsRole::Client;
    config.groups      = kGroups;
    config.signatures  = kSignatures;
    config.aeads       = kAeads;
    config.pinned      = ConstSpan<KeyId>(&pin, 1U);
    config.server_name = kName;
    config.ech_configs = list;
    TlsContext     context;
    const TlsError made = context.init(config);
    // No list is a client without ECH (its hellos carry a GREASE extension), which every context accepts.
    if (list.empty())
    {
        CRD_FUZZ_REQUIRE(made == TlsError::None);
        return;
    }
    CRD_FUZZ_REQUIRE((made == TlsError::None) == ech_list_shaped(list));
    CRD_FUZZ_REQUIRE(made == TlsError::None || made == TlsError::BadConfig);
    if (made != TlsError::None)
    {
        return;
    }
    TlsSession     session;
    const TlsError started = session.init(context);
    CRD_FUZZ_REQUIRE(started == TlsError::None || started == TlsError::BadConfig);
    if (started == TlsError::BadConfig)
    {
        CRD_FUZZ_REQUIRE(session.state() == TlsState::Idle && session.pending() == 0U);
    }
    else
    {
        u8    flight[kMaxFlight];
        usize written = 0U;
        CRD_FUZZ_REQUIRE(session.pending() <= kMaxFlight);
        CRD_FUZZ_REQUIRE(session.take(Span<u8>(flight), written) == TlsError::None);
        CRD_FUZZ_REQUIRE(written > 9U && flight[0] == 22U);
        bool names = true;
        u16  aead  = 0U;
        CRD_FUZZ_REQUIRE(hello_offers_ech(ConstSpan<u8>(flight + 5U, written - 5U), names, aead));
        CRD_FUZZ_REQUIRE(!names && !holds(ConstSpan<u8>(flight, written), kName));
    }

    // Decision S21: the same list under a generation refusing ChaCha20-Poly1305.
    TlsConfig narrowed  = config;
    narrowed.policy     = &shared.no_chacha.store;
    TlsContext governed;
    CRD_FUZZ_REQUIRE(governed.init(narrowed) == TlsError::None);
    TlsSession     filtered;
    const TlsError kept = filtered.init(governed);
    CRD_FUZZ_REQUIRE((kept == TlsError::PolicyRefused) == !ech_list_allowed(list));
    CRD_FUZZ_REQUIRE(kept == TlsError::None || kept == TlsError::BadConfig || kept == TlsError::PolicyRefused);
    if (kept == TlsError::None)
    {
        u8    flight[kMaxFlight];
        usize written = 0U;
        bool  names   = true;
        u16   aead    = 0U;
        CRD_FUZZ_REQUIRE(filtered.take(Span<u8>(flight), written) == TlsError::None && written > 9U);
        CRD_FUZZ_REQUIRE(hello_offers_ech(ConstSpan<u8>(flight + 5U, written - 5U), names, aead));
        CRD_FUZZ_REQUIRE(!names && aead != 3U);
    }
    else
    {
        CRD_FUZZ_REQUIRE(filtered.state() == TlsState::Idle && filtered.pending() == 0U);
    }

    config.application_protocols = kProtocols;
    CRD_FUZZ_REQUIRE(context.init(config) == TlsError::None);
    QuicHandshake  handshake;
    const TlsError quic = handshake.init(context, ConstSpan<u8>(kParameters));
    CRD_FUZZ_REQUIRE(quic == started);
    if (quic == TlsError::BadConfig)
    {
        CRD_FUZZ_REQUIRE(handshake.state() == TlsState::Idle && handshake.pending(QuicLevel::Initial) == 0U);
        return;
    }
    u8    initial[kMaxFlight];
    usize written = 0U;
    CRD_FUZZ_REQUIRE(handshake.pending(QuicLevel::Initial) <= kMaxFlight);
    CRD_FUZZ_REQUIRE(handshake.take(QuicLevel::Initial, Span<u8>(initial), written) == TlsError::None);
    bool names = true;
    u16  aead  = 0U;
    CRD_FUZZ_REQUIRE(hello_offers_ech(ConstSpan<u8>(initial, written), names, aead));
    CRD_FUZZ_REQUIRE(!names && !holds(ConstSpan<u8>(initial, written), kName));
}

// Adler-32 (RFC 1950 section 8.2), restated.
crd::u32 adler32_of(ConstSpan<u8> bytes) noexcept
{
    crd::u32 a = 1U;
    crd::u32 b = 0U;
    for (const u8 byte : bytes)
    {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

// Whether `compressed` can decode to `claim` bytes of `algorithm`, restated from RFC 1951 (zstd: the bound alone).
bool claim_reachable(u8 algorithm, usize compressed, usize claim) noexcept
{
    if (claim < 4U || claim > kTlsMaxCertificateBytes)
    {
        return false;
    }
    return algorithm == iana::kCertCompressionZstd ||
           (algorithm == iana::kCertCompressionZlib && compressed > 6U && claim <= (compressed - 6U) * 1032U);
}

void decode_certificate(const u8* data, usize size) noexcept
{
    static u8           out[kClaimLimit];
    static u8           again[kClaimLimit];
    const u8            algorithm = data[1];
    const usize         claimed   = (usize{data[2]} << 16U) | (usize{data[3]} << 8U) | data[4];
    const usize         claim     = std::min(claimed, kClaimLimit);
    const ConstSpan<u8> compressed(data + kCompressedHead, size - kCompressedHead);
    std::memset(out, kUntouched, claim);
    const bool decoded = detail::tls_decompress_certificate(algorithm, compressed, Span<u8>(out, claim));
    if (!claim_reachable(algorithm, compressed.size(), claim))
    {
        CRD_FUZZ_REQUIRE(!decoded);
        CRD_FUZZ_REQUIRE(std::all_of(out, out + claim, [](u8 byte) { return byte == kUntouched; }));
        return;
    }
    if (!decoded)
    {
        return;
    }
    if (algorithm == iana::kCertCompressionZlib)
    {
        const u8* trailer = compressed.data() + compressed.size() - 4U;
        CRD_FUZZ_REQUIRE(adler32_of(ConstSpan<u8>(out, claim)) ==
                         ((crd::u32{trailer[0]} << 24U) | (crd::u32{trailer[1]} << 16U) |
                          (crd::u32{trailer[2]} << 8U) | trailer[3]));
    }
    CRD_FUZZ_REQUIRE(detail::tls_decompress_certificate(algorithm, compressed, Span<u8>(again, claim)));
    CRD_FUZZ_REQUIRE(std::memcmp(out, again, claim) == 0);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    if ((data[0] & 4U) != 0U)
    {
        if (size >= kCompressedHead)
        {
            decode_certificate(data, size);
        }
        return 0;
    }
    Contexts& shared = contexts();
    CRD_FUZZ_REQUIRE(shared.ready);
    const bool client = (data[0] & 1U) != 0U;
    if ((data[0] & 32U) != 0U)
    {
        if ((data[0] & 8U) != 0U)
        {
            early_quic(client, ConstSpan<u8>(data + 2U, size - 2U));
        }
        else
        {
            early_records(client, ConstSpan<u8>(data + 2U, size - 2U), data[1]);
        }
        return 0;
    }
    if ((data[0] & 16U) != 0U && client)
    {
        offer_ech(ConstSpan<u8>(data + 2U, size - 2U));
        return 0;
    }
    if ((data[0] & 16U) != 0U)
    {
        TlsSession session;
        CRD_FUZZ_REQUIRE(session.init(shared.ech_server) == TlsError::None);
        feed(session, ConstSpan<u8>(data + 2U, size - 2U), data[1]);
        CRD_FUZZ_REQUIRE(session.ech() == TlsEch::None && session.ech_retry_configs().empty());
        return 0;
    }
    if ((data[0] & 8U) != 0U)
    {
        QuicHandshake handshake;
        CRD_FUZZ_REQUIRE(handshake.init(client ? shared.quic_client : shared.quic_server,
                                        ConstSpan<u8>(kParameters)) == TlsError::None);
        CRD_FUZZ_REQUIRE(handshake.pending(QuicLevel::Initial) > 0U || !client);
        drain(handshake);
        feed_quic(handshake, client, ConstSpan<u8>(data + 2U, size - 2U));
        return 0;
    }
    TlsSession session;
    if ((data[0] & 2U) != 0U)
    {
        InputStore store(ConstSpan<u8>(data + 2U, size - 2U));
        CRD_FUZZ_REQUIRE(session.init(shared.client, &store) == TlsError::None);
        CRD_FUZZ_REQUIRE(store.takes == 1U);
        CRD_FUZZ_REQUIRE(session.state() == TlsState::Handshaking && session.pending() > 0U);
        drain(session);
        feed(session, {}, 0U);
        CRD_FUZZ_REQUIRE(store.kept == 0U && !session.resumed());
        CRD_FUZZ_REQUIRE(session.early_data() == TlsEarlyData::None && session.early_data_room() == 0U);
        return 0;
    }
    CRD_FUZZ_REQUIRE(session.init(client ? shared.client : shared.server) == TlsError::None);
    drain(session);
    feed(session, ConstSpan<u8>(data + 2U, size - 2U), data[1]);
    return 0;
}

namespace
{
// A byte writer for the seeds' TLS messages.
struct Writer
{
    u8    bytes[2048] = {};
    usize size        = 0U;

    void put(u8 value) noexcept { bytes[size++] = value; }
    void put16(usize value) noexcept
    {
        put(static_cast<u8>(value >> 8U));
        put(static_cast<u8>(value));
    }
    void put(const u8* data, usize count) noexcept
    {
        std::memcpy(bytes + size, data, count);
        size += count;
    }
    // Writes the 16-bit (or 24-bit) length of everything written since `start` (its length field) at `start`.
    void patch16(usize start) noexcept
    {
        const usize length = size - start - 2U;
        bytes[start]       = static_cast<u8>(length >> 8U);
        bytes[start + 1U]  = static_cast<u8>(length);
    }
    void patch24(usize start) noexcept
    {
        const usize length = size - start - 3U;
        bytes[start]       = static_cast<u8>(length >> 16U);
        bytes[start + 1U]  = static_cast<u8>(length >> 8U);
        bytes[start + 2U]  = static_cast<u8>(length);
    }
};

// The extensions a QUIC hello carries: the application protocol (16) and the transport parameters (57).
void protocol_and_parameters(Writer& out) noexcept
{
    const crd::containers::StringView protocol = kProtocols[0];
    out.put16(iana::kTlsExtensionApplicationLayerProtocolNegotiation);
    out.put16(2U + 1U + protocol.size());
    out.put16(1U + protocol.size());
    out.put(static_cast<u8>(protocol.size()));
    out.put(reinterpret_cast<const u8*>(protocol.data()), protocol.size());
    out.put16(0x0039U); // quic_transport_parameters (crd-net's iana::kTlsExtensionQuicTransportParameters)
    out.put16(sizeof(kParameters));
    out.put(kParameters, sizeof(kParameters));
}

// A TLS 1.3 ClientHello for the fuzz server: X25519 with RFC 7748's Alice's public key, Ed25519, the server's name and
// TLS_AES_128_GCM_SHA256, in one handshake record (RFC 9846 section 4.1.2). With `ticket` it offers a ticket last
// (psk_dhe_ke): an identity led by the server's ticket key name, a zero salt and a sealed state no key opens, and a
// binder of the right length (RFC 9846 section 4.2.11). It names `server`, and with `ech` carries it as its
// encrypted_client_hello after the name (RFC 9849 section 5).
void client_hello(Writer& out, u8 mode, u8 chunk, bool ticket = false, bool quic = false, const char* server = kName,
                  ConstSpan<u8> ech = {}) noexcept
{
    constexpr u8 alice[32] = {0x85U, 0x20U, 0xF0U, 0x09U, 0x89U, 0x30U, 0xA7U, 0x54U, 0x74U, 0x8BU, 0x7DU,
                               0xDCU, 0xB4U, 0x3EU, 0xF7U, 0x5AU, 0x0DU, 0xBFU, 0x3AU, 0x0DU, 0x26U, 0x38U,
                               0x1AU, 0xF4U, 0xEBU, 0xA4U, 0xA9U, 0x8EU, 0xAAU, 0x9BU, 0x4EU, 0x6AU};
    out.put(mode);
    out.put(chunk);
    out.put(22U); // handshake record
    out.put16(0x0301U);
    const usize record = out.size;
    out.put16(0U);
    out.put(1U); // client_hello
    const usize message = out.size;
    out.put(0U);
    out.put16(0U);
    out.put16(0x0303U);
    for (u8 i = 0U; i < 32U; ++i)
    {
        out.put(static_cast<u8>(0xC0U + i));
    }
    out.put(0U);        // legacy_session_id
    out.put16(2U);      // cipher_suites
    out.put16(0x1301U); // TLS_AES_128_GCM_SHA256
    out.put(1U);        // legacy_compression_methods
    out.put(0U);
    const usize extensions = out.size;
    out.put16(0U);
    out.put16(0x0000U); // server_name
    out.put16(std::strlen(server) + 5U);
    out.put16(std::strlen(server) + 3U);
    out.put(0U); // host_name
    out.put16(std::strlen(server));
    out.put(reinterpret_cast<const u8*>(server), std::strlen(server));
    if (!ech.empty())
    {
        out.put16(kEchVersion); // encrypted_client_hello
        out.put16(ech.size());
        out.put(ech.data(), ech.size());
    }
    out.put16(0x000AU); // supported_groups: x25519
    out.put16(4U);
    out.put16(2U);
    out.put16(0x001DU);
    out.put16(0x000DU); // signature_algorithms: ed25519
    out.put16(4U);
    out.put16(2U);
    out.put16(0x0807U);
    out.put16(0x002BU); // supported_versions: TLS 1.3
    out.put16(3U);
    out.put(2U);
    out.put16(0x0304U);
    out.put16(0x0033U); // key_share: x25519
    out.put16(2U + 4U + 32U);
    out.put16(4U + 32U);
    out.put16(0x001DU);
    out.put16(32U);
    out.put(alice, sizeof(alice));
    if (quic)
    {
        protocol_and_parameters(out);
    }
    if (ticket)
    {
        out.put16(0x002DU); // psk_key_exchange_modes: psk_dhe_ke
        out.put16(2U);
        out.put(1U);
        out.put(1U);
        constexpr char info[] = "cerid tls 1.3 ticket name";
        u8             prk[32];
        u8             name[16];
        if (hkdf_extract(AlgorithmId::HkdfSha256, {}, ConstSpan<u8>(kTicketKey.bytes), Span<u8>(prk)) !=
                CryptoError::None ||
            hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk),
                        ConstSpan<u8>(reinterpret_cast<const u8*>(info), sizeof(info) - 1U),
                        Span<u8>(name)) != CryptoError::None)
        {
            return;
        }
        constexpr usize identity = 16U + 32U + 96U + 16U; // name, salt, a state's worth of bytes and the tag
        out.put16(0x0029U);                                 // pre_shared_key
        out.put16(2U + 2U + identity + 4U + 2U + 1U + 32U);
        out.put16(2U + identity + 4U);
        out.put16(identity);
        out.put(name, sizeof(name));
        for (usize i = sizeof(name); i < identity; ++i)
        {
            out.put(static_cast<u8>(i < 48U ? 0U : i));
        }
        out.put16(0x0001U); // obfuscated_ticket_age
        out.put16(0x0203U);
        out.put16(1U + 32U); // binders: one HMAC-SHA256
        out.put(32U);
        for (u8 i = 0U; i < 32U; ++i)
        {
            out.put(static_cast<u8>(0xB0U + i));
        }
    }
    out.patch16(extensions);
    out.patch24(message);
    out.patch16(record);
}

// A ServerHello for the fuzz client: TLS_AES_128_GCM_SHA256 and X25519 with RFC 7748's Bob's public key.
void server_hello(Writer& out) noexcept
{
    constexpr u8 bob[32] = {0xDEU, 0x9EU, 0xDBU, 0x7DU, 0x7BU, 0x7DU, 0xC1U, 0xB4U, 0xD3U, 0x5BU, 0x61U,
                             0xC2U, 0xECU, 0xE4U, 0x35U, 0x37U, 0x3FU, 0x83U, 0x43U, 0xC8U, 0x5BU, 0x78U,
                             0x67U, 0x4DU, 0xADU, 0xFCU, 0x7EU, 0x14U, 0x6FU, 0x88U, 0x2BU, 0x4FU};
    out.put(1U);
    out.put(0U);
    out.put(22U);
    out.put16(0x0303U);
    const usize record = out.size;
    out.put16(0U);
    out.put(2U); // server_hello
    const usize message = out.size;
    out.put(0U);
    out.put16(0U);
    out.put16(0x0303U);
    for (u8 i = 0U; i < 32U; ++i)
    {
        out.put(static_cast<u8>(0x50U + i));
    }
    out.put(0U); // legacy_session_id_echo
    out.put16(0x1301U);
    out.put(0U);
    const usize extensions = out.size;
    out.put16(0U);
    out.put16(0x002BU); // supported_versions
    out.put16(2U);
    out.put16(0x0304U);
    out.put16(0x0033U); // key_share
    out.put16(4U + 32U);
    out.put16(0x001DU);
    out.put16(32U);
    out.put(bob, sizeof(bob));
    out.patch16(extensions);
    out.patch24(message);
    out.patch16(record);
}
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    Writer hello;
    client_hello(hello, 0U, 0U);
    sink.add("server-client-hello", hello.bytes, hello.size);
    hello.bytes[1] = 7U;
    sink.add("server-client-hello-in-7-byte-pieces", hello.bytes, hello.size);
    Writer resuming;
    client_hello(resuming, 0U, 0U, true);
    sink.add("server-client-hello-with-a-forged-ticket", resuming.bytes, resuming.size);

    // Ticket records a client's store hands out (version 1, the early data allowed, the transport parameters, the
    // session): no serialized session, nothing at all, another version, parameters past the end, and no session.
    const u8 not_a_session[] = {3U, 0U, 1U, 0U, 0U, 0x38U, 0U, 0U, 0U, 0x30U, 0x82U, 0x01U, 0x00U, 0x02U, 0x01U, 0x01U};
    sink.add("client-ticket-not-a-session", not_a_session, sizeof(not_a_session));
    const u8 empty_ticket[] = {3U, 0U};
    sink.add("client-empty-ticket", empty_ticket, sizeof(empty_ticket));
    const u8 record_version[] = {3U, 0U, 2U, 0U, 0U, 0U, 0U, 0U, 0U, 0x30U, 0x03U, 0x02U, 0x01U, 0x01U};
    sink.add("client-ticket-record-of-another-version", record_version, sizeof(record_version));
    const u8 past_the_end[] = {3U, 0U, 1U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x09U, 0x01U, 0x02U, 0x03U};
    sink.add("client-ticket-record-parameters-past-the-end", past_the_end, sizeof(past_the_end));
    const u8 no_session[] = {3U, 0U, 1U, 0U, 0U, 0x38U, 0U, 0U, 2U, 0x0FU, 0x00U};
    sink.add("client-ticket-record-without-a-session", no_session, sizeof(no_session));

    Writer answer;
    server_hello(answer);
    sink.add("client-server-hello", answer.bytes, answer.size);

    // A record header that claims more than a record may hold, an SSL 2.0 hello, a TLS 1.2-only hello's start, an
    // alert and nothing at all.
    const u8 oversized[] = {0U, 0U, 22U, 0x03U, 0x03U, 0xFFU, 0xFFU, 1U};
    sink.add("server-oversized-record", oversized, sizeof(oversized));
    const u8 sslv2[] = {0U, 0U, 0x80U, 0x2EU, 0x01U, 0x03U, 0x01U, 0x00U, 0x15U, 0x00U, 0x00U, 0x00U, 0x10U};
    sink.add("server-sslv2-hello", sslv2, sizeof(sslv2));
    const u8 alert[] = {1U, 0U, 21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x02U, 0x28U};
    sink.add("client-handshake-failure-alert", alert, sizeof(alert));
    const u8 nothing[] = {0U, 0U};
    sink.add("server-nothing", nothing, sizeof(nothing));

    // A peer that cancels: user_canceled, which is ignored, then close_notify, which closes the session; five warnings
    // in a row, past what the provider tolerates; and a user_canceled ahead of an honest ClientHello.
    const u8 canceled[] = {1U, 0U, 21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x01U, 0x5AU,
                           21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x01U, 0x00U};
    sink.add("client-user-canceled-close-notify", canceled, sizeof(canceled));
    u8 warnings[2U + 5U * 7U] = {0U, 0U};
    for (usize i = 0U; i < 5U; ++i)
    {
        std::memcpy(warnings + 2U + i * 7U, canceled + 2U, 7U);
    }
    sink.add("server-five-user-canceled", warnings, sizeof(warnings));
    Writer late;
    late.put(0U);
    late.put(0U);
    late.put(canceled + 2U, 7U);
    Writer second;
    client_hello(second, 0U, 0U);
    late.put(second.bytes + 2U, second.size - 2U);
    sink.add("server-user-canceled-then-client-hello", late.bytes, late.size);

    // Compressed Certificates: a Certificate-shaped message (an empty context, one 600-byte entry, no extensions) as
    // zstd and as zlib, each claiming its length, one byte more, and far more than the bound; and a brotli claim.
    u8 certificate[609] = {0U, 0U, 0x02U, 0x5DU, 0U, 0x02U, 0x58U};
    for (usize i = 7U; i + 2U < sizeof(certificate); ++i)
    {
        certificate[i] = static_cast<u8>(i % 61U == 0U ? i : 0x30U + i % 7U);
    }
    const ConstSpan<u8>        plain(certificate, sizeof(certificate));
    crd::containers::Array<u8> zstd_form;
    if (!crd::compression::zstd_compress(plain, crd::compression::kZstdDefaultLevel, zstd_form))
    {
        return;
    }
    const crd::containers::Array<u8> zlib_form =
        crd::compression::zlib_deflate(plain, crd::memory::default_allocator());
    struct Form
    {
        const char*                       name;
        u8                                algorithm;
        const crd::containers::Array<u8>* bytes;
    };
    const Form forms[] = {{"zstd", static_cast<u8>(iana::kCertCompressionZstd), &zstd_form},
                          {"zlib", static_cast<u8>(iana::kCertCompressionZlib), &zlib_form}};
    for (const Form& form : forms)
    {
        const usize claims[] = {sizeof(certificate), sizeof(certificate) + 1U, 0xFFFFFFU};
        const char* labels[] = {"exact", "one-more", "bomb"};
        for (usize i = 0U; i < 3U; ++i)
        {
            Writer seed;
            seed.put(4U);
            seed.put(form.algorithm);
            seed.put(static_cast<u8>(claims[i] >> 16U));
            seed.put16(claims[i] & 0xFFFFU);
            seed.put(form.bytes->data(), form.bytes->size());
            char name[64];
            std::snprintf(name, sizeof(name), "compressed-certificate-%s-%s", form.name, labels[i]);
            sink.add(name, seed.bytes, seed.size);
        }
    }
    const u8 brotli[] = {4U, 2U, 0U, 0U, 16U, 0x0BU, 0x07U, 0x80U, 0x61U, 0x62U, 0x63U, 0x03U};
    sink.add("compressed-certificate-brotli", brotli, sizeof(brotli));

    // QUIC handshakes: the hellos above as CRYPTO data at the Initial level (their messages out of their records), a
    // QUIC ClientHello with its protocol and transport parameters, a ServerHello followed by EncryptedExtensions at the
    // Handshake level (which a client reads under keys from Bob's share), data at a level not read yet, a TLS
    // KeyUpdate, and a piece longer than the input.
    const auto quic = [&sink](const char* name, u8 mode, std::initializer_list<ConstSpan<u8>> pieces, u8 level) {
        Writer seed;
        seed.put(static_cast<u8>(8U | mode));
        seed.put(0U);
        for (const ConstSpan<u8> piece : pieces)
        {
            seed.put(level);
            seed.put16(piece.size());
            seed.put(piece.data(), piece.size());
            level = 1U;
        }
        sink.add(name, seed.bytes, seed.size);
    };
    Writer quic_hello;
    client_hello(quic_hello, 0U, 0U, false, true);
    const ConstSpan<u8> quic_hello_message(quic_hello.bytes + 7U, quic_hello.size - 7U);
    quic("quic-server-client-hello", 0U, {quic_hello_message}, 0U);
    quic("quic-server-stream-client-hello", 0U, {ConstSpan<u8>(hello.bytes + 7U, hello.size - 7U)}, 0U);
    Writer ticket_hello;
    client_hello(ticket_hello, 0U, 0U, true, true);
    quic("quic-server-client-hello-with-a-forged-ticket", 0U,
         {ConstSpan<u8>(ticket_hello.bytes + 7U, ticket_hello.size - 7U)}, 0U);
    Writer encrypted;
    encrypted.put(8U); // encrypted_extensions
    const usize encrypted_message = encrypted.size;
    encrypted.put(0U);
    encrypted.put16(0U);
    const usize encrypted_extensions = encrypted.size;
    encrypted.put16(0U);
    protocol_and_parameters(encrypted);
    encrypted.patch16(encrypted_extensions);
    encrypted.patch24(encrypted_message);
    const ConstSpan<u8> server_hello_message(answer.bytes + 7U, answer.size - 7U);
    quic("quic-client-server-hello", 1U, {server_hello_message}, 0U);
    quic("quic-client-server-hello-encrypted-extensions", 1U,
         {server_hello_message, ConstSpan<u8>(encrypted.bytes, encrypted.size)}, 0U);
    constexpr u8 key_update[] = {24U, 0U, 0U, 1U, 0U};
    quic("quic-server-handshake-level-first", 0U, {quic_hello_message}, 1U);
    quic("quic-client-key-update", 1U, {ConstSpan<u8>(key_update)}, 0U);
    // Early data: what follows a real client's ClientHello and early data at its server (an application data record
    // no key opens, a plaintext close_notify, another ClientHello, a ChangeCipherSpec, nothing), in pieces too, and
    // what reaches a client that offered it (a ServerHello that resumes nothing, a handshake_failure alert); over QUIC
    // a forged Finished at the Handshake level for the server and the ServerHello for the client.
    Writer forged;
    forged.put(32U);
    forged.put(0U);
    forged.put(23U);
    forged.put16(0x0303U);
    forged.put16(48U);
    for (u8 i = 0U; i < 48U; ++i)
    {
        forged.put(static_cast<u8>(0x11U * i + 7U));
    }
    sink.add("early-server-forged-application-data", forged.bytes, forged.size);
    forged.bytes[1] = 5U;
    sink.add("early-server-forged-application-data-in-5-byte-pieces", forged.bytes, forged.size);
    const u8 early_close[] = {32U, 0U, 21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x01U, 0x00U};
    sink.add("early-server-plaintext-close-notify", early_close, sizeof(early_close));
    const u8 early_ccs[] = {32U, 0U, 20U, 0x03U, 0x03U, 0x00U, 0x01U, 0x01U};
    sink.add("early-server-change-cipher-spec", early_ccs, sizeof(early_ccs));
    Writer again;
    client_hello(again, 32U, 0U);
    sink.add("early-server-another-client-hello", again.bytes, again.size);
    const u8 early_nothing[] = {32U, 0U};
    sink.add("early-server-nothing", early_nothing, sizeof(early_nothing));
    Writer early_answer;
    server_hello(early_answer);
    early_answer.bytes[0] = 33U;
    sink.add("early-client-server-hello-without-a-psk", early_answer.bytes, early_answer.size);
    const u8 early_alert[] = {33U, 0U, 21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x02U, 0x28U};
    sink.add("early-client-handshake-failure-alert", early_alert, sizeof(early_alert));
    constexpr u8 forged_finished[] = {20U, 0U,  0U,  32U, 1U,  2U,  3U,  4U,  5U,  6U,  7U,  8U,
                                      9U,  10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U,
                                      21U, 22U, 23U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U, 32U};
    quic("early-quic-server-forged-finished", 32U, {ConstSpan<u8>(forged_finished)}, 1U);
    quic("early-quic-client-server-hello-without-a-psk", 33U, {server_hello_message}, 0U);
    const u8 overlong[] = {8U, 0U, 0U, 0xFFU, 0xFFU, 1U, 0U, 0U, 9U};
    sink.add("quic-server-overlong-piece", overlong, sizeof(overlong));

    // ECH clients: the fuzz server's list, the same after a configuration of another version, its configuration with
    // a KEM the provider lacks (ML-KEM-768's HPKE identifier), with a mandatory extension, with a public name an IPv4
    // parser reads as an address, cut short, and nothing listed.
    TlsEchKey ech_keys[2];
    make_ech_key(ech_keys[0], 0U);
    make_ech_key(ech_keys[1], 1U);
    const ConstSpan<u8> config = ech_keys[0].config();
    const auto          list   = [&sink](const char* name, std::initializer_list<ConstSpan<u8>> configs) {
        Writer seed;
        seed.put(17U);
        seed.put(0U);
        const usize start = seed.size;
        seed.put16(0U);
        for (const ConstSpan<u8> entry : configs)
        {
            seed.put(entry.data(), entry.size());
        }
        seed.patch16(start);
        sink.add(name, seed.bytes, seed.size);
    };
    list("ech-client-list", {config});
    const u8 other_version[] = {0xFFU, 0x01U, 0x00U, 0x02U, 0xAAU, 0xBBU};
    list("ech-client-list-other-version-first", {ConstSpan<u8>(other_version), config});
    u8 changed[kTlsMaxEchConfigBytes + 4U];
    std::memcpy(changed, config.data(), config.size());
    changed[6] = 0x41U; // kem_id 0x0041
    list("ech-client-list-unusable-kem", {ConstSpan<u8>(changed, config.size())});
    std::memcpy(changed, config.data(), config.size());
    changed[3] = static_cast<u8>(changed[3] + 4U);
    changed[config.size() - 1U] = 4U; // extensions: one of 4 bytes, a mandatory type with nothing in it
    changed[config.size() + 0U] = 0xFAU;
    changed[config.size() + 1U] = 0xCEU;
    changed[config.size() + 2U] = 0x00U;
    changed[config.size() + 3U] = 0x00U;
    list("ech-client-list-mandatory-extension", {ConstSpan<u8>(changed, config.size() + 4U)});
    TlsEchKey   numeric;
    u8          numeric_secret[kTlsEchKeyBytes] = {7U};
    const char* address = "front.cerid.123";
    if (numeric.init(ConstSpan<u8>(numeric_secret), 9U, "front.cerid.abc", 32U) == TlsError::None)
    {
        std::memcpy(changed, numeric.config().data(), numeric.config().size());
        std::memcpy(changed + 53U, address, std::strlen(address)); // the same length: "abc" becomes "123"
        list("ech-client-list-ipv4-public-name", {ConstSpan<u8>(changed, numeric.config().size())});
    }
    list("ech-client-list-cut-short", {config.first(config.size() - 3U)});
    list("ech-client-list-empty", {});

    // ECH servers: outer hellos naming the public name with an encrypted_client_hello the server cannot open (its
    // current config_id, an unknown one, a previous key's), one of the inner type, and one whose lengths lie.
    u8 offer[1U + 4U + 1U + 2U + 32U + 2U + 160U] = {0U, 0x00U, 0x01U, 0x00U, 0x01U, kEchConfigIds[0], 0x00U, 32U};
    for (usize i = 8U; i < sizeof(offer); ++i)
    {
        offer[i] = static_cast<u8>(0x11U * i);
    }
    offer[8U + 32U]      = 0U;
    offer[8U + 32U + 1U] = 160U;
    const auto outer = [&sink](const char* name, ConstSpan<u8> ech) {
        Writer seed;
        client_hello(seed, 16U, 0U, false, false, kPublicName, ech);
        sink.add(name, seed.bytes, seed.size);
    };
    outer("ech-server-outer-hello", ConstSpan<u8>(offer));
    offer[5] = 0x99U;
    outer("ech-server-outer-hello-unknown-config", ConstSpan<u8>(offer));
    offer[5] = kEchConfigIds[1];
    outer("ech-server-outer-hello-previous-config", ConstSpan<u8>(offer));
    const u8 inner[] = {1U};
    outer("ech-server-inner-type-in-outer", ConstSpan<u8>(inner));
    offer[7] = 0xFFU;
    outer("ech-server-ech-lengths-lie", ConstSpan<u8>(offer));
}
