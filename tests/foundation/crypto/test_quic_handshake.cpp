// crd-crypto -- the TLS 1.3 handshake over QUIC (NET.2; docs/design/network-foundation.md#quic-handshake): pairs of
// QuicHandshakes in one process exchanging CRYPTO data level by level, every group, AEAD and signature algorithm with
// each level's secrets turned into packet and header keys that open the other end's packets in both QUIC versions,
// the order in which the levels' secrets arrive, a HelloRetryRequest, pinned keys both ways and their alerts at their
// levels, application protocols (and the same rules for stream sessions), transport parameters both ways and a
// ClientHello without them, certificate compression, tickets and resumption under QUIC's own binding, the flights'
// sizes against QUIC's amplification limit, a forbidden TLS KeyUpdate, data at levels the handshake does not read,
// the receive limit, refusals and one context on several threads.
//
// The independent peer (OpenSSL 3.5's QUIC TLS interface) is tests/foundation/crypto/interop/tls_interop.py's.

#include <crd/containers/array.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/tls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <thread>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using namespace crd::crypto;

using Bytes = crd::containers::Array<u8>;

constexpr AlgorithmId kAllGroups[] = {
    AlgorithmId::X25519Mlkem768, AlgorithmId::Secp256r1Mlkem768, AlgorithmId::Secp384r1Mlkem1024,
    AlgorithmId::X25519,         AlgorithmId::Secp256r1,         AlgorithmId::Secp384r1,
};
constexpr AlgorithmId kAllSignatures[] = {
    AlgorithmId::MlDsa44, AlgorithmId::MlDsa65,
    AlgorithmId::MlDsa87, AlgorithmId::Ed25519,
    AlgorithmId::EcdsaSecp256r1Sha256, AlgorithmId::EcdsaSecp384r1Sha384,
};
constexpr AlgorithmId kAllAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};
constexpr QuicLevel   kLevels[]   = {QuicLevel::Initial, QuicLevel::Handshake, QuicLevel::Application};

constexpr const char* kServerName = "game.cerid.test";
constexpr StringView  kGame[]     = {"cerid-game/1"};

// Transport parameters as the transport would encode them (RFC 9000 section 18): the handshake carries them as bytes.
constexpr u8 kClientParameters[] = {0x04U, 0x04U, 0x80U, 0x10U, 0x00U, 0x00U, 0x0FU, 0x04U, 0x0AU, 0x0BU, 0x0CU, 0x0DU};
constexpr u8 kServerParameters[] = {0x00U, 0x04U, 0x01U, 0x02U, 0x03U, 0x04U, 0x0FU, 0x02U, 0x51U, 0x52U};

constexpr u8 kUnexpectedMessage = 10U;
constexpr u8 kBadCertificate    = 42U;
constexpr u8 kMissingExtension  = 109U;
constexpr u8 kHandshakeFailure  = 40U;

void make_key(SigningKey& key, AlgorithmId algorithm, u8 salt)
{
    u8 seed[48];
    for (usize i = 0U; i < sizeof(seed); ++i)
    {
        seed[i] = static_cast<u8>(i + 1U + salt);
    }
    REQUIRE(key.from_seed(algorithm, ConstSpan<u8>(seed, signature_sizes(algorithm).seed)) == CryptoError::None);
}

struct End
{
    SigningKey    key;
    TlsCredential credential;
    TlsContext    context;
    QuicHandshake handshake;
};

// A client's tickets: the latest one kept per binding, handed out once.
class Store final : public TlsTicketStore
{
public:
    void keep(const TlsBinding& binding, ConstSpan<u8> ticket) noexcept override
    {
        m_binding = binding;
        m_ticket.resize(ticket.size());
        std::memcpy(m_ticket.data(), ticket.data(), ticket.size());
        ++kept;
    }

    ConstSpan<u8> take(const TlsBinding& binding) noexcept override
    {
        taken_under = binding;
        m_handed    = m_ticket;
        m_ticket.clear();
        if (m_handed.empty() || !(binding == m_binding))
        {
            return {};
        }
        return ConstSpan<u8>(m_handed.data(), m_handed.size());
    }

    [[nodiscard]] TlsBinding binding() const { return m_binding; }

    usize      kept = 0U;
    TlsBinding taken_under;

private:
    TlsBinding m_binding;
    Bytes      m_ticket;
    Bytes      m_handed;
};

struct Options
{
    ConstSpan<AlgorithmId> client_groups      = kAllGroups;
    ConstSpan<AlgorithmId> server_groups      = kAllGroups;
    ConstSpan<AlgorithmId> client_aeads       = kAllAeads;
    ConstSpan<AlgorithmId> server_aeads       = kAllAeads;
    AlgorithmId            server_key         = AlgorithmId::Ed25519;
    AlgorithmId            client_key{};             // a client credential (mutual authentication) when set
    bool                   server_pins_client = false;
    bool                   client_pins_server = true;
    bool                   client_key_pinned  = true; // false: the server pins another client key
    ConstSpan<StringView>  client_protocols   = kGame;
    ConstSpan<StringView>  server_protocols   = kGame;
    ConstSpan<u16>         client_compression = kTlsCertificateCompression;
    ConstSpan<u8>          ticket_key;
};

void configure(End& client, End& server, const Options& options)
{
    make_key(server.key, options.server_key, 0U);
    REQUIRE(server.credential.init(server.key, kServerName) == TlsError::None);
    KeyId client_pin = server.credential.key_id();
    if (!options.client_pins_server)
    {
        client_pin.bytes[0] ^= 1U;
    }
    TlsConfig client_config;
    client_config.role                    = TlsRole::Client;
    client_config.groups                  = options.client_groups;
    client_config.signatures              = kAllSignatures;
    client_config.aeads                   = options.client_aeads;
    client_config.pinned                  = ConstSpan<KeyId>(&client_pin, 1U);
    client_config.server_name             = kServerName;
    client_config.certificate_compression = options.client_compression;
    client_config.application_protocols   = options.client_protocols;
    KeyId server_pin;
    if (options.client_key != AlgorithmId{})
    {
        make_key(client.key, options.client_key, 7U);
        REQUIRE(client.credential.init(client.key, "player.cerid.test") == TlsError::None);
        client_config.credential = &client.credential;
        server_pin               = client.credential.key_id();
        if (!options.client_key_pinned)
        {
            server_pin.bytes[0] ^= 1U;
        }
    }
    REQUIRE(client.context.init(client_config) == TlsError::None);

    TlsConfig server_config;
    server_config.role                  = TlsRole::Server;
    server_config.groups                = options.server_groups;
    server_config.aeads                 = options.server_aeads;
    server_config.credential            = &server.credential;
    server_config.server_name           = kServerName;
    server_config.ticket_key            = options.ticket_key;
    server_config.application_protocols = options.server_protocols;
    if (options.server_pins_client)
    {
        server_config.signatures = kAllSignatures;
        server_config.pinned     = ConstSpan<KeyId>(&server_pin, 1U);
    }
    REQUIRE(server.context.init(server_config) == TlsError::None);
}

void start(End& client, End& server, Store* tickets = nullptr)
{
    REQUIRE(client.handshake.init(client.context, ConstSpan<u8>(kClientParameters), tickets) == TlsError::None);
    REQUIRE(server.handshake.init(server.context, ConstSpan<u8>(kServerParameters)) == TlsError::None);
}

Bytes drain(QuicHandshake& handshake, QuicLevel level)
{
    Bytes out;
    out.resize(handshake.pending(level));
    usize written = 0U;
    REQUIRE(handshake.take(level, Span<u8>(out.data(), out.size()), written) == TlsError::None);
    REQUIRE(written == out.size());
    return out;
}

usize pending(const QuicHandshake& handshake)
{
    usize total = 0U;
    for (const QuicLevel level : kLevels)
    {
        total += handshake.pending(level);
    }
    return total;
}

bool live(const QuicHandshake& handshake)
{
    return handshake.state() == TlsState::Handshaking || handshake.state() == TlsState::Established;
}

// Moves one end's CRYPTO data to the other, level by level in order; the CRYPTO bytes moved.
usize deliver(QuicHandshake& from, QuicHandshake& to)
{
    usize moved = 0U;
    for (const QuicLevel level : kLevels)
    {
        const Bytes bytes = drain(from, level);
        moved += bytes.size();
        if (!bytes.empty() && live(to))
        {
            (void)to.provide(level, ConstSpan<u8>(bytes.data(), bytes.size()));
        }
    }
    return moved;
}

void pump(End& client, End& server)
{
    for (int round = 0; round < 16 && (pending(client.handshake) > 0U || pending(server.handshake) > 0U); ++round)
    {
        (void)deliver(client.handshake, server.handshake);
        (void)deliver(server.handshake, client.handshake);
    }
    REQUIRE(pending(client.handshake) == 0U);
    REQUIRE(pending(server.handshake) == 0U);
}

void establish(End& client, End& server)
{
    pump(client, server);
    REQUIRE(client.handshake.state() == TlsState::Established);
    REQUIRE(server.handshake.state() == TlsState::Established);
}

// A packet of `level` as the transport would build it: a long Handshake header or a short 1-RTT header, a 2-byte
// packet number, a payload and room for the tag. `number_at` is where the packet number starts.
Bytes packet_of(QuicLevel level, u64 packet_number, usize& header_bytes, usize& number_at)
{
    Bytes packet;
    if (level == QuicLevel::Handshake)
    {
        const u8 header[] = {0xE1U, 0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 0x00U,
                             0x40U, 0x32U};
        packet.resize(sizeof(header));
        std::memcpy(packet.data(), header, sizeof(header));
    }
    else
    {
        const u8 header[] = {0x41U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
        packet.resize(sizeof(header));
        std::memcpy(packet.data(), header, sizeof(header));
    }
    number_at = packet.size();
    packet.push_back(static_cast<u8>(packet_number >> 8U));
    packet.push_back(static_cast<u8>(packet_number));
    header_bytes = packet.size();
    for (usize i = 0U; i < 32U + kQuicTagBytes; ++i)
    {
        packet.push_back(static_cast<u8>(i * 13U + 5U));
    }
    return packet;
}

// `side`'s keys at `level` agree at both ends: a packet the writer seals and protects opens at the reader to the same
// payload, under the handshake's AEAD. Each end's secret is consumed.
void check_keys(QuicHandshake& writer, QuicHandshake& reader, QuicLevel level, QuicSide side, QuicVersion version)
{
    QuicPacketKey seal;
    QuicPacketKey open;
    QuicHeaderKey protect;
    QuicHeaderKey unprotect;
    REQUIRE(writer.derive_keys(level, side, version, seal, protect) == TlsError::None);
    REQUIRE(reader.derive_keys(level, side, version, open, unprotect) == TlsError::None);
    CHECK(seal.algorithm() == open.algorithm());
    CHECK(seal.version() == version);
    CHECK_FALSE(writer.has_keys(level, side));
    CHECK(writer.derive_keys(level, side, version, seal, protect) == TlsError::NotReady);

    const u64   packet_number = 0x1234U;
    usize       header_bytes  = 0U;
    usize       number_at     = 0U;
    Bytes       packet        = packet_of(level, packet_number, header_bytes, number_at);
    const Bytes plain         = packet;
    REQUIRE(seal.seal(packet_number, Span<u8>(packet.data(), packet.size()), header_bytes) == CryptoError::None);
    REQUIRE(protect.protect(Span<u8>(packet.data(), packet.size()), number_at) == CryptoError::None);
    usize number_bytes = 0U;
    u64   truncated    = 0U;
    REQUIRE(unprotect.unprotect(Span<u8>(packet.data(), packet.size()), number_at, number_bytes, truncated) ==
            CryptoError::None);
    CHECK(number_bytes == 2U);
    CHECK(truncated == packet_number);
    REQUIRE(open.open(packet_number, Span<u8>(packet.data(), packet.size()), header_bytes) == CryptoError::None);
    CHECK(std::memcmp(packet.data(), plain.data(), packet.size() - kQuicTagBytes) == 0);
}

// Every Handshake and Application key of both sides agrees.
void check_all_keys(End& client, End& server, QuicVersion version)
{
    for (const QuicLevel level : {QuicLevel::Handshake, QuicLevel::Application})
    {
        check_keys(client.handshake, server.handshake, level, QuicSide::Client, version);
        check_keys(server.handshake, client.handshake, level, QuicSide::Server, version);
    }
}

bool equal(ConstSpan<u8> a, ConstSpan<u8> b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// The type of the first handshake message in a level's CRYPTO data.
u8 first_message(const Bytes& crypto)
{
    return crypto.empty() ? 0U : crypto[0];
}
} // namespace

TEST_CASE("QUIC handshake: every group and AEAD agree on every level's keys in both versions",
          "[crypto][quic-handshake][net-2]")
{
    usize round = 0U;
    for (const AlgorithmId group : kAllGroups)
    {
        for (const AlgorithmId aead : kAllAeads)
        {
            End     client;
            End     server;
            Options options;
            options.client_groups = ConstSpan<AlgorithmId>(&group, 1U);
            options.client_aeads  = ConstSpan<AlgorithmId>(&aead, 1U);
            configure(client, server, options);
            start(client, server);
            establish(client, server);
            CHECK(client.handshake.group() == group);
            CHECK(server.handshake.group() == group);
            CHECK(client.handshake.aead() == aead);
            CHECK(server.handshake.aead() == aead);
            CHECK(client.handshake.application_protocol() == "cerid-game/1");
            CHECK(server.handshake.application_protocol() == "cerid-game/1");
            CHECK(equal(client.handshake.peer_transport_parameters(), ConstSpan<u8>(kServerParameters)));
            CHECK(equal(server.handshake.peer_transport_parameters(), ConstSpan<u8>(kClientParameters)));
            CHECK(client.handshake.peer_key() == server.credential.key_id());
            check_all_keys(client, server, round % 2U == 0U ? QuicVersion::V1 : QuicVersion::V2);
            ++round;
        }
    }
}

TEST_CASE("QUIC handshake: every signature algorithm authenticates a server, and mutual authentication a client",
          "[crypto][quic-handshake][net-2]")
{
    for (const AlgorithmId signature : kAllSignatures)
    {
        End     client;
        End     server;
        Options options;
        options.server_key         = signature;
        options.client_key         = signature;
        options.server_pins_client = true;
        configure(client, server, options);
        start(client, server);
        establish(client, server);
        CHECK(client.handshake.peer_algorithm() == signature);
        CHECK(client.handshake.peer_key() == server.credential.key_id());
        CHECK(server.handshake.peer_algorithm() == signature);
        CHECK(server.handshake.peer_key() == client.credential.key_id());
        check_all_keys(client, server, QuicVersion::V1);
    }
}

TEST_CASE("QUIC handshake: each level carries its messages and its secrets arrive writer first",
          "[crypto][quic-handshake][net-2]")
{
    End client;
    End server;
    configure(client, server, Options{});
    start(client, server);

    // The ClientHello alone, at the Initial level; no secret yet.
    CHECK(client.handshake.pending(QuicLevel::Handshake) == 0U);
    CHECK(client.handshake.pending(QuicLevel::Application) == 0U);
    const Bytes hello = drain(client.handshake, QuicLevel::Initial);
    CHECK(first_message(hello) == 1U);
    CHECK_FALSE(client.handshake.has_keys(QuicLevel::Handshake, QuicSide::Client));
    CHECK(client.handshake.receive_limit(QuicLevel::Initial) >= 4096U);
    CHECK(client.handshake.receive_limit(QuicLevel::Handshake) >= 4096U);
    CHECK(server.handshake.peer_transport_parameters().empty());

    // The server's ServerHello at Initial, EncryptedExtensions to Finished at Handshake; it may write half-RTT data
    // before the client is authenticated, and reads 1-RTT only after the client's Finished.
    REQUIRE(server.handshake.provide(QuicLevel::Initial, ConstSpan<u8>(hello.data(), hello.size())) == TlsError::None);
    CHECK(equal(server.handshake.peer_transport_parameters(), ConstSpan<u8>(kClientParameters)));
    CHECK(server.handshake.state() == TlsState::Handshaking);
    const Bytes server_hello = drain(server.handshake, QuicLevel::Initial);
    const Bytes encrypted    = drain(server.handshake, QuicLevel::Handshake);
    CHECK(first_message(server_hello) == 2U);
    CHECK(first_message(encrypted) == 8U); // EncryptedExtensions
    CHECK(server.handshake.pending(QuicLevel::Application) == 0U);
    CHECK(server.handshake.has_keys(QuicLevel::Handshake, QuicSide::Server));
    CHECK(server.handshake.has_keys(QuicLevel::Handshake, QuicSide::Client));
    CHECK(server.handshake.has_keys(QuicLevel::Application, QuicSide::Server));
    CHECK_FALSE(server.handshake.has_keys(QuicLevel::Application, QuicSide::Client));
    CHECK_FALSE(server.handshake.has_keys(QuicLevel::Initial, QuicSide::Server));

    // The client reads both and answers with its Finished at the Handshake level; it is established.
    REQUIRE(client.handshake.provide(QuicLevel::Initial, ConstSpan<u8>(server_hello.data(), server_hello.size())) ==
            TlsError::None);
    REQUIRE(client.handshake.provide(QuicLevel::Handshake, ConstSpan<u8>(encrypted.data(), encrypted.size())) ==
            TlsError::None);
    CHECK(client.handshake.state() == TlsState::Established);
    CHECK(client.handshake.pending(QuicLevel::Initial) == 0U);
    const Bytes finished = drain(client.handshake, QuicLevel::Handshake);
    CHECK(first_message(finished) == 20U);
    CHECK(client.handshake.has_keys(QuicLevel::Application, QuicSide::Client));
    CHECK(client.handshake.has_keys(QuicLevel::Application, QuicSide::Server));

    // The server reads the Finished, is established and sends its ticket at the Application level.
    REQUIRE(server.handshake.provide(QuicLevel::Handshake, ConstSpan<u8>(finished.data(), finished.size())) ==
            TlsError::None);
    CHECK(server.handshake.state() == TlsState::Established);
    CHECK(server.handshake.has_keys(QuicLevel::Application, QuicSide::Client));
    const Bytes ticket = drain(server.handshake, QuicLevel::Application);
    CHECK(first_message(ticket) == 4U); // NewSessionTicket
    CHECK(pending(server.handshake) == 0U);
    REQUIRE(client.handshake.provide(QuicLevel::Application, ConstSpan<u8>(ticket.data(), ticket.size())) ==
            TlsError::None);
    CHECK(client.handshake.state() == TlsState::Established);
    check_all_keys(client, server, QuicVersion::V2);
}

TEST_CASE("QUIC handshake: a server whose preferred group has no share asks for one at the Initial level",
          "[crypto][quic-handshake][net-2]")
{
    struct Retry
    {
        AlgorithmId offered;
        AlgorithmId wanted;
    };
    constexpr Retry retries[] = {
        {AlgorithmId::Secp256r1Mlkem768, AlgorithmId::X25519Mlkem768},
        {AlgorithmId::X25519Mlkem768, AlgorithmId::Secp384r1Mlkem1024},
        {AlgorithmId::X25519, AlgorithmId::Secp384r1},
    };
    for (const Retry& retry : retries)
    {
        const AlgorithmId client_groups[] = {retry.offered, retry.wanted};
        End               client;
        End               server;
        Options           options;
        options.client_groups = client_groups;
        options.server_groups = ConstSpan<AlgorithmId>(&retry.wanted, 1U);
        configure(client, server, options);
        start(client, server);
        (void)deliver(client.handshake, server.handshake);
        // The HelloRetryRequest is a ServerHello at the Initial level, with nothing at the Handshake level yet.
        CHECK(server.handshake.pending(QuicLevel::Initial) > 0U);
        CHECK(server.handshake.pending(QuicLevel::Handshake) == 0U);
        CHECK_FALSE(server.handshake.has_keys(QuicLevel::Handshake, QuicSide::Server));
        establish(client, server);
        CHECK(client.handshake.hello_retry());
        CHECK(server.handshake.hello_retry());
        CHECK(client.handshake.group() == retry.wanted);
        check_all_keys(client, server, QuicVersion::V1);
    }
}

TEST_CASE("QUIC handshake: an unpinned key is refused with its alert at its level", "[crypto][quic-handshake][net-2]")
{
    SECTION("a client refuses a server key it does not pin with bad_certificate at the Handshake level")
    {
        End     client;
        End     server;
        Options options;
        options.client_pins_server = false;
        configure(client, server, options);
        start(client, server);
        pump(client, server);
        CHECK(client.handshake.state() == TlsState::Failed);
        CHECK(client.handshake.alert_sent().present);
        CHECK(client.handshake.alert_sent().description == kBadCertificate);
        CHECK(client.handshake.alert_level() == QuicLevel::Handshake);
        CHECK(quic_crypto_error(client.handshake.alert_sent().description) == 0x012AU);
        // QUIC carries no alert to the peer: the transport's CONNECTION_CLOSE does.
        CHECK_FALSE(server.handshake.alert_received().present);
        CHECK(client.handshake.provide(QuicLevel::Handshake, ConstSpan<u8>(kClientParameters)) ==
              TlsError::NotReady);
    }
    SECTION("a server refuses a client key it does not pin with general_error")
    {
        End     client;
        End     server;
        Options options;
        options.client_key         = AlgorithmId::Ed25519;
        options.server_pins_client = true;
        options.client_key_pinned  = false;
        configure(client, server, options);
        start(client, server);
        pump(client, server);
        CHECK(server.handshake.state() == TlsState::Failed);
        CHECK(server.handshake.alert_sent().description == kTlsAlertGeneralError);
        // The server writes at the Application level already (half-RTT): the alert goes there, and the transport
        // repeats the close at the levels its client can read (RFC 9000 section 10.2.3).
        CHECK(server.handshake.alert_level() == QuicLevel::Application);
        // The client was established before its Finished was judged: the transport's close tells it.
        CHECK(client.handshake.state() == TlsState::Established);
    }
}

TEST_CASE("QUIC handshake: application protocols are required and selected in the server's order",
          "[crypto][quic-handshake][net-2]")
{
    constexpr StringView client_list[] = {"cerid-project/1", "cerid-game/2", "cerid-game/1"};
    constexpr StringView server_list[] = {"cerid-game/1", "cerid-game/2"};
    constexpr StringView other[]       = {"cerid-agent/1"};
    SECTION("the server's first protocol its client offers")
    {
        End     client;
        End     server;
        Options options;
        options.client_protocols = client_list;
        options.server_protocols = server_list;
        configure(client, server, options);
        start(client, server);
        establish(client, server);
        CHECK(client.handshake.application_protocol() == "cerid-game/1");
        CHECK(server.handshake.application_protocol() == "cerid-game/1");
    }
    SECTION("no common protocol: no_application_protocol at the Initial level")
    {
        End     client;
        End     server;
        Options options;
        options.server_protocols = other;
        configure(client, server, options);
        start(client, server);
        pump(client, server);
        CHECK(server.handshake.state() == TlsState::Failed);
        CHECK(server.handshake.alert_sent().description == kTlsAlertNoApplicationProtocol);
        CHECK(server.handshake.alert_level() == QuicLevel::Initial);
        CHECK(quic_crypto_error(kTlsAlertNoApplicationProtocol) == 0x0178U);
    }
    SECTION("a context without protocols starts no QUIC handshake")
    {
        End     client;
        End     server;
        Options options;
        options.client_protocols = {};
        options.server_protocols = {};
        configure(client, server, options);
        CHECK(client.handshake.init(client.context, ConstSpan<u8>(kClientParameters)) == TlsError::BadConfig);
        CHECK(server.handshake.init(server.context, ConstSpan<u8>(kServerParameters)) == TlsError::BadConfig);
        CHECK(client.handshake.state() == TlsState::Idle);
    }
}

TEST_CASE("TLS: application protocols over a stream are agreed or refused with no_application_protocol",
          "[crypto][tls][quic-handshake][net-2]")
{
    // The same contexts drive TlsSession: a stream session follows the same rules.
    constexpr StringView list[] = {"cerid-project/1", "cerid-game/1"};
    struct Ends
    {
        End        client;
        End        server;
        TlsSession client_session;
        TlsSession server_session;

        void run()
        {
            REQUIRE(client_session.init(client.context) == TlsError::None);
            REQUIRE(server_session.init(server.context) == TlsError::None);
            for (int round = 0; round < 8; ++round)
            {
                for (TlsSession* from : {&client_session, &server_session})
                {
                    TlsSession* to = from == &client_session ? &server_session : &client_session;
                    Bytes       bytes;
                    bytes.resize(from->pending());
                    usize written = 0U;
                    REQUIRE(from->take(Span<u8>(bytes.data(), bytes.size()), written) == TlsError::None);
                    if (written != 0U && to->state() == TlsState::Handshaking)
                    {
                        (void)to->receive(ConstSpan<u8>(bytes.data(), written));
                    }
                }
            }
        }
    };
    SECTION("agreed")
    {
        Ends    ends;
        Options options;
        options.client_protocols = list;
        configure(ends.client, ends.server, options);
        ends.run();
        REQUIRE(ends.client_session.state() == TlsState::Established);
        CHECK(ends.client_session.application_protocol() == "cerid-game/1");
        CHECK(ends.server_session.application_protocol() == "cerid-game/1");
    }
    SECTION("neither end names one")
    {
        Ends    ends;
        Options options;
        options.client_protocols = {};
        options.server_protocols = {};
        configure(ends.client, ends.server, options);
        ends.run();
        REQUIRE(ends.server_session.state() == TlsState::Established);
        CHECK(ends.client_session.application_protocol().empty());
    }
    SECTION("a client that offers none to a server that names some")
    {
        Ends    ends;
        Options options;
        options.client_protocols = {};
        configure(ends.client, ends.server, options);
        ends.run();
        CHECK(ends.server_session.state() == TlsState::Failed);
        CHECK(ends.server_session.alert_sent().description == kTlsAlertNoApplicationProtocol);
        CHECK(ends.client_session.alert_received().description == kTlsAlertNoApplicationProtocol);
    }
    SECTION("a server that selects none for a client that offers some")
    {
        Ends    ends;
        Options options;
        options.server_protocols = {};
        configure(ends.client, ends.server, options);
        ends.run();
        CHECK(ends.client_session.state() == TlsState::Failed);
        CHECK(ends.client_session.alert_sent().description == kTlsAlertNoApplicationProtocol);
        // The client objects after its Finished: the server, established, reads the alert after it.
        REQUIRE(ends.server_session.state() == TlsState::Established);
        u8    sink[16];
        usize read = 0U;
        CHECK(ends.server_session.read(Span<u8>(sink), read) == TlsError::Failed);
        CHECK(ends.server_session.alert_received().description == kTlsAlertNoApplicationProtocol);
        CHECK(ends.server_session.state() == TlsState::Failed);
    }
}

TEST_CASE("QUIC handshake: a ClientHello without transport parameters is refused with missing_extension",
          "[crypto][quic-handshake][net-2]")
{
    // A stream session's ClientHello carries no quic_transport_parameters; its handshake message, out of its record,
    // is what a QUIC client without them would send.
    End client;
    End server;
    configure(client, server, Options{});
    TlsSession stream;
    REQUIRE(stream.init(client.context) == TlsError::None);
    Bytes record;
    record.resize(stream.pending());
    usize written = 0U;
    REQUIRE(stream.take(Span<u8>(record.data(), record.size()), written) == TlsError::None);
    REQUIRE(record.size() > 5U);
    REQUIRE(server.handshake.init(server.context, ConstSpan<u8>(kServerParameters)) == TlsError::None);
    CHECK(server.handshake.provide(QuicLevel::Initial, ConstSpan<u8>(record.data() + 5U, record.size() - 5U)) ==
          TlsError::Failed);
    CHECK(server.handshake.alert_sent().description == kMissingExtension);
    CHECK(server.handshake.alert_level() == QuicLevel::Initial);
}

TEST_CASE("QUIC handshake: certificate compression and the flights against the amplification limit",
          "[crypto][quic-handshake][net-2]")
{
    // RFC 9000 section 8.1: before its client's address is validated a server sends at most three times what it
    // received, and a client's Initial datagram is at least 1200 bytes. A hybrid ClientHello's CRYPTO data alone is
    // longer than one such datagram, so it takes two: the server may then send 7200 bytes, or 8832 when its client
    // fills both to a 1500-byte path (1472 bytes of UDP payload each). The flight is the server's CRYPTO data at the
    // Initial and Handshake levels (packets add their headers and tags to it).
    constexpr usize two_minimum = 3U * 2U * 1200U;
    constexpr usize two_full    = 3U * 2U * 1472U;
    for (const AlgorithmId signature : kAllSignatures)
    {
        End     client;
        End     server;
        Store   tickets;
        Options options;
        options.server_key = signature;
        configure(client, server, options);
        start(client, server, &tickets);
        const usize hello = deliver(client.handshake, server.handshake);
        const usize full  = server.handshake.pending(QuicLevel::Initial) +
                            server.handshake.pending(QuicLevel::Handshake);
        establish(client, server);
        CHECK(client.handshake.certificate_compression() == server.handshake.certificate_compression());
        if (signature == AlgorithmId::Ed25519)
        {
            CHECK(server.handshake.certificate_compression() == iana::kCertCompressionZlib);
        }
        if (signature == AlgorithmId::MlDsa44 || signature == AlgorithmId::MlDsa65 || signature == AlgorithmId::MlDsa87)
        {
            CHECK(server.handshake.certificate_compression() == iana::kCertCompressionZstd);
        }
        REQUIRE(tickets.kept == 1U);

        // The resumed flight carries no Certificate or CertificateVerify.
        REQUIRE(client.handshake.init(client.context, ConstSpan<u8>(kClientParameters), &tickets) == TlsError::None);
        REQUIRE(server.handshake.init(server.context, ConstSpan<u8>(kServerParameters)) == TlsError::None);
        const usize resumed_hello = deliver(client.handshake, server.handshake);
        const usize resumed       = server.handshake.pending(QuicLevel::Initial) +
                                    server.handshake.pending(QuicLevel::Handshake);
        establish(client, server);
        CHECK(client.handshake.resumed());
        CHECK(server.handshake.resumed());
        CAPTURE(signature, hello, full, resumed_hello, resumed);
        CHECK(hello > 1200U);
        CHECK(resumed_hello > 1200U);
        CHECK(resumed < full);
        CHECK(resumed < two_minimum);
        const bool post_quantum = signature == AlgorithmId::MlDsa44 || signature == AlgorithmId::MlDsa65 ||
                                  signature == AlgorithmId::MlDsa87;
        // A classical certificate fits; an ML-DSA one does not fit two minimum datagrams' limit, and ML-DSA-65 and
        // -87 not even two full ones': such a server validates its client's address first, or resumes.
        CHECK((full < two_minimum) != post_quantum);
        if (signature == AlgorithmId::MlDsa65 || signature == AlgorithmId::MlDsa87)
        {
            CHECK(full > two_full);
        }
    }
}

TEST_CASE("QUIC handshake: tickets resume under QUIC's own binding and never cross transports",
          "[crypto][quic-handshake][net-2]")
{
    u8 ticket_key[kTlsTicketKeyBytes];
    for (usize i = 0U; i < sizeof(ticket_key); ++i)
    {
        ticket_key[i] = static_cast<u8>(i + 1U);
    }
    End     client;
    End     server;
    Store   tickets;
    Options options;
    options.ticket_key = ConstSpan<u8>(ticket_key);
    configure(client, server, options);
    start(client, server, &tickets);
    establish(client, server);
    REQUIRE(tickets.kept == 1U);
    const TlsBinding quic_binding = tickets.binding();
    CHECK_FALSE(client.handshake.resumed());

    // A stream session of the same context keeps its ticket under the context's own binding.
    TlsSession stream_client;
    TlsSession stream_server;
    Store      stream_tickets;
    REQUIRE(stream_client.init(client.context, &stream_tickets) == TlsError::None);
    REQUIRE(stream_server.init(server.context) == TlsError::None);
    for (int round = 0; round < 8; ++round)
    {
        for (TlsSession* from : {&stream_client, &stream_server})
        {
            TlsSession* to = from == &stream_client ? &stream_server : &stream_client;
            Bytes       bytes;
            bytes.resize(from->pending());
            usize written = 0U;
            REQUIRE(from->take(Span<u8>(bytes.data(), bytes.size()), written) == TlsError::None);
            if (written != 0U)
            {
                (void)to->receive(ConstSpan<u8>(bytes.data(), written));
            }
        }
    }
    u8    sink[64];
    usize read = 0U;
    REQUIRE(stream_client.state() == TlsState::Established);
    (void)stream_client.read(Span<u8>(sink, sizeof(sink)), read);
    REQUIRE(stream_tickets.kept == 1U);
    CHECK_FALSE(stream_tickets.binding() == quic_binding);

    // The QUIC client asks under its binding, so the stream ticket is not offered, and its own resumes once.
    REQUIRE(client.handshake.init(client.context, ConstSpan<u8>(kClientParameters), &stream_tickets) ==
            TlsError::None);
    CHECK(stream_tickets.taken_under == quic_binding);
    REQUIRE(server.handshake.init(server.context, ConstSpan<u8>(kServerParameters)) == TlsError::None);
    establish(client, server);
    CHECK_FALSE(client.handshake.resumed());

    REQUIRE(client.handshake.init(client.context, ConstSpan<u8>(kClientParameters), &tickets) == TlsError::None);
    REQUIRE(server.handshake.init(server.context, ConstSpan<u8>(kServerParameters)) == TlsError::None);
    establish(client, server);
    CHECK(client.handshake.resumed());
    CHECK(server.handshake.resumed());
    CHECK(client.handshake.peer_key() == server.credential.key_id());
    CHECK(client.handshake.certificate_compression() == 0U);
    check_all_keys(client, server, QuicVersion::V1);
}

TEST_CASE("QUIC handshake: a TLS KeyUpdate and data at a level not read now fail the handshake",
          "[crypto][quic-handshake][net-2]")
{
    // KeyUpdate (24) with update_not_requested: QUIC updates its keys itself (RFC 9001 section 6).
    constexpr u8 key_update[] = {24U, 0U, 0U, 1U, 0U};
    SECTION("a KeyUpdate after the handshake")
    {
        End client;
        End server;
        configure(client, server, Options{});
        start(client, server);
        establish(client, server);
        CHECK(client.handshake.provide(QuicLevel::Application, ConstSpan<u8>(key_update)) == TlsError::Failed);
        CHECK(client.handshake.state() == TlsState::Failed);
        CHECK(client.handshake.alert_sent().description == kUnexpectedMessage);
        CHECK(client.handshake.alert_level() == QuicLevel::Application);
        CHECK(server.handshake.provide(QuicLevel::Application, ConstSpan<u8>(key_update)) == TlsError::Failed);
        CHECK(server.handshake.alert_sent().description == kUnexpectedMessage);
    }
    SECTION("Handshake data before the ServerHello, Initial data after it")
    {
        End client;
        End server;
        configure(client, server, Options{});
        start(client, server);
        CHECK(client.handshake.provide(QuicLevel::Handshake, ConstSpan<u8>(key_update)) == TlsError::Failed);
        CHECK_FALSE(client.handshake.alert_sent().present); // a PROTOCOL_VIOLATION for the transport, no alert

        End client2;
        End server2;
        configure(client2, server2, Options{});
        start(client2, server2);
        (void)deliver(client2.handshake, server2.handshake);
        const Bytes initial = drain(server2.handshake, QuicLevel::Initial);
        REQUIRE(client2.handshake.provide(QuicLevel::Initial, ConstSpan<u8>(initial.data(), initial.size())) ==
                TlsError::None);
        CHECK(client2.handshake.provide(QuicLevel::Initial, ConstSpan<u8>(initial.data(), initial.size())) ==
              TlsError::Failed);
    }
    SECTION("garbage at the Initial level")
    {
        End client;
        End server;
        configure(client, server, Options{});
        start(client, server);
        constexpr u8 garbage[] = {1U, 0U, 0U, 4U, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
        CHECK(server.handshake.provide(QuicLevel::Initial, ConstSpan<u8>(garbage)) == TlsError::Failed);
        CHECK(server.handshake.alert_sent().present);
        CHECK(server.handshake.alert_level() == QuicLevel::Initial);
    }
}

TEST_CASE("QUIC handshake: CRYPTO data past the receive limit is refused and nothing is taken",
          "[crypto][quic-handshake][net-2]")
{
    End client;
    End server;
    configure(client, server, Options{});
    start(client, server);
    const usize limit = server.handshake.receive_limit(QuicLevel::Initial);
    REQUIRE(limit >= 4096U);
    // A ClientHello header claiming more than the limit, then the limit's worth of its body.
    Bytes oversized;
    oversized.resize(limit + 1U);
    oversized[0] = 1U;
    oversized[1] = 0x01U;
    CHECK(server.handshake.provide(QuicLevel::Initial, ConstSpan<u8>(oversized.data(), oversized.size())) ==
          TlsError::Full);
    CHECK(server.handshake.state() == TlsState::Handshaking);
    // Nothing was taken: the real ClientHello still runs the handshake.
    establish(client, server);
}

TEST_CASE("QUIC handshake: refusals before and outside the handshake", "[crypto][quic-handshake][net-2]")
{
    End client;
    End server;
    configure(client, server, Options{});
    QuicHandshake idle;
    CHECK(idle.state() == TlsState::Idle);
    CHECK(idle.provide(QuicLevel::Initial, ConstSpan<u8>(kClientParameters)) == TlsError::NotReady);
    CHECK(idle.pending(QuicLevel::Initial) == 0U);
    CHECK(idle.receive_limit(QuicLevel::Initial) == 0U);
    CHECK(idle.peer_transport_parameters().empty());
    CHECK(idle.application_protocol().empty());
    usize written = 1U;
    u8    out[8];
    CHECK(idle.take(QuicLevel::Initial, Span<u8>(out), written) == TlsError::NotReady);
    CHECK(written == 0U);

    TlsContext empty;
    CHECK(idle.init(empty, ConstSpan<u8>(kClientParameters)) == TlsError::NotReady);
    CHECK(idle.init(client.context, ConstSpan<u8>()) == TlsError::BadConfig);
    Bytes large;
    large.resize(kQuicMaxTransportParameterBytes + 1U);
    CHECK(idle.init(client.context, ConstSpan<u8>(large.data(), large.size())) == TlsError::BadConfig);
    Store tickets;
    CHECK(idle.init(server.context, ConstSpan<u8>(kServerParameters), &tickets) == TlsError::BadConfig);
    CHECK(idle.state() == TlsState::Idle);
    CHECK(idle.init(client.context, ConstSpan<u8>(large.data(), kQuicMaxTransportParameterBytes)) ==
          TlsError::None);

    start(client, server);
    QuicPacketKey packet;
    QuicHeaderKey header;
    CHECK(client.handshake.derive_keys(QuicLevel::Handshake, QuicSide::Client, QuicVersion::V1, packet, header) ==
          TlsError::NotReady);
    establish(client, server);
    CHECK(client.handshake.derive_keys(QuicLevel::Initial, QuicSide::Client, QuicVersion::V1, packet, header) ==
          TlsError::BadConfig);
    CHECK(client.handshake.derive_keys(QuicLevel::Handshake, QuicSide::Client, static_cast<QuicVersion>(0x0AU),
                                       packet, header) == TlsError::BadConfig);
    CHECK_FALSE(packet.ready());
    CHECK(client.handshake.take(static_cast<QuicLevel>(3U), Span<u8>(out), written) == TlsError::NotReady);
    CHECK(client.handshake.pending(static_cast<QuicLevel>(3U)) == 0U);

    // clear() forgets everything; a second init() starts afresh.
    client.handshake.clear();
    CHECK(client.handshake.state() == TlsState::Idle);
    CHECK_FALSE(client.handshake.has_keys(QuicLevel::Application, QuicSide::Server));
    CHECK(client.handshake.peer_transport_parameters().empty());
    start(client, server);
    establish(client, server);
    check_all_keys(client, server, QuicVersion::V2);
}

TEST_CASE("QUIC handshake: no common group fails with handshake_failure at the Initial level",
          "[crypto][quic-handshake][net-2]")
{
    constexpr AlgorithmId client_only[] = {AlgorithmId::X25519};
    constexpr AlgorithmId server_only[] = {AlgorithmId::Secp384r1};
    End     client;
    End     server;
    Options options;
    options.client_groups = client_only;
    options.server_groups = server_only;
    configure(client, server, options);
    start(client, server);
    pump(client, server);
    CHECK(server.handshake.state() == TlsState::Failed);
    CHECK(server.handshake.alert_sent().description == kHandshakeFailure);
    CHECK(server.handshake.alert_level() == QuicLevel::Initial);
}

TEST_CASE("QUIC handshake: one server context serves handshakes on several threads", "[crypto][quic-handshake][net-2]")
{
    End client;
    End server;
    configure(client, server, Options{});
    std::atomic<int> established{0};
    std::thread      workers[4];
    for (std::thread& worker : workers)
    {
        worker = std::thread([&]() {
            for (int i = 0; i < 4; ++i)
            {
                QuicHandshake a;
                QuicHandshake b;
                if (a.init(client.context, ConstSpan<u8>(kClientParameters)) != TlsError::None ||
                    b.init(server.context, ConstSpan<u8>(kServerParameters)) != TlsError::None)
                {
                    return;
                }
                for (int round = 0; round < 8; ++round)
                {
                    for (QuicHandshake* from : {&a, &b})
                    {
                        QuicHandshake* to = from == &a ? &b : &a;
                        for (const QuicLevel level : kLevels)
                        {
                            u8    bytes[16384];
                            usize written = 0U;
                            if (from->take(level, Span<u8>(bytes), written) == TlsError::None && written != 0U)
                            {
                                (void)to->provide(level, ConstSpan<u8>(bytes, written));
                            }
                        }
                    }
                }
                if (a.state() == TlsState::Established && b.state() == TlsState::Established &&
                    a.application_protocol() == "cerid-game/1")
                {
                    established.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    CHECK(established.load() == 16);
}
