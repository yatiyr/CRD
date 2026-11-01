// crd-crypto -- TLS 1.3 sessions over the provider's TLS stack (NET.2; docs/design/network-foundation.md#tls-sessions
// and #tls-13-profile): pairs of sessions in one process, every group (the three hybrid groups included) and every
// signature algorithm negotiated, the key shares read off the wire and checked against the hybrid groups' layout with
// the provider interface's own primitives, HelloRetryRequest, server preference, pinned keys both ways, server names,
// alerts, bounds, close_notify, adversarial bytes, every configuration refusal and one context serving several threads.
//
// The independent peer (OpenSSL 3.5's hybrid key exchange) is tests/foundation/crypto/interop/tls_interop.py's.

#include <crd/containers/array.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <openssl/aead.h> // EVP_has_aes_hardware: the test asks the provider what the preference must follow

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace
{
using crd::u16;
using crd::u32;
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

constexpr const char* kServerName = "game.cerid.test";

constexpr u8 kHandshakeFailure      = 40U;
constexpr u8 kBadCertificate        = 42U;
constexpr u8 kCertificateRequired   = 116U;
constexpr u8 kRecordChangeCipherSpec = 20U;
constexpr u8 kRecordHandshake       = 22U;
constexpr u16 kKeyShareExtension    = 51U;

// A signing key from a fixed seed (an ECDSA scalar of 1..n-1: the bytes 1, 2, 3, ...).
void make_key(SigningKey& key, AlgorithmId algorithm, u8 salt)
{
    u8 seed[48];
    for (usize i = 0U; i < sizeof(seed); ++i)
    {
        seed[i] = static_cast<u8>(i + 1U + salt);
    }
    REQUIRE(key.from_seed(algorithm, ConstSpan<u8>(seed, signature_sizes(algorithm).seed)) == CryptoError::None);
}

// A server or client: its key, credential, context and session.
struct Endpoint
{
    SigningKey    key;
    TlsCredential credential;
    TlsContext    context;
    TlsSession    session;
};

struct Options
{
    ConstSpan<AlgorithmId> client_groups  = kAllGroups;
    ConstSpan<AlgorithmId> server_groups  = kAllGroups;
    ConstSpan<AlgorithmId> client_aeads   = kAllAeads;
    ConstSpan<AlgorithmId> server_aeads   = kAllAeads;
    ConstSpan<AlgorithmId> client_accepts = kAllSignatures;
    AlgorithmId            server_key     = AlgorithmId::Ed25519;
    AlgorithmId            client_key{};             // a client credential (mutual authentication) when set
    bool                   server_pins_client = false;
    bool                   client_pins_server = true;  // false: the client pins another key
    StringView             client_names       = kServerName;
    StringView             server_names       = kServerName;
};

// Configures both ends; the client's first flight is pending when this returns.
void setup(Endpoint& client, Endpoint& server, const Options& options)
{
    make_key(server.key, options.server_key, 0U);
    REQUIRE(server.credential.init(server.key, kServerName) == TlsError::None);
    KeyId client_pin = server.credential.key_id();
    if (!options.client_pins_server)
    {
        client_pin.bytes[0] ^= 1U;
    }
    TlsConfig client_config;
    client_config.role        = TlsRole::Client;
    client_config.groups      = options.client_groups;
    client_config.signatures  = options.client_accepts;
    client_config.aeads       = options.client_aeads;
    client_config.pinned      = ConstSpan<KeyId>(&client_pin, 1U);
    client_config.server_name = options.client_names;
    KeyId server_pin;
    if (options.client_key != AlgorithmId{})
    {
        make_key(client.key, options.client_key, 7U);
        REQUIRE(client.credential.init(client.key, "player.cerid.test") == TlsError::None);
        client_config.credential = &client.credential;
        server_pin               = client.credential.key_id();
    }
    REQUIRE(client.context.init(client_config) == TlsError::None);

    TlsConfig server_config;
    server_config.role        = TlsRole::Server;
    server_config.groups      = options.server_groups;
    server_config.aeads       = options.server_aeads;
    server_config.credential  = &server.credential;
    server_config.server_name = options.server_names;
    if (options.server_pins_client)
    {
        server_config.signatures = kAllSignatures;
        server_config.pinned     = ConstSpan<KeyId>(&server_pin, 1U);
    }
    REQUIRE(server.context.init(server_config) == TlsError::None);
    REQUIRE(client.session.init(client.context) == TlsError::None);
    REQUIRE(server.session.init(server.context) == TlsError::None);
    REQUIRE(client.session.pending() > 0U);
}

Bytes drain(TlsSession& session)
{
    Bytes out;
    out.resize(session.pending());
    usize written = 0U;
    REQUIRE(session.take(Span<u8>(out.data(), out.size()), written) == TlsError::None);
    REQUIRE(written == out.size());
    return out;
}

// Moves one end's pending bytes to the other; the receiver's answer.
TlsError deliver(TlsSession& from, TlsSession& to)
{
    const Bytes bytes = drain(from);
    if (bytes.empty() || to.state() == TlsState::Failed)
    {
        return TlsError::None;
    }
    return to.receive(ConstSpan<u8>(bytes.data(), bytes.size()));
}

// Flights back and forth until neither side has anything to send.
void pump(Endpoint& client, Endpoint& server)
{
    for (int round = 0; round < 16 && (client.session.pending() > 0U || server.session.pending() > 0U); ++round)
    {
        (void)deliver(client.session, server.session);
        (void)deliver(server.session, client.session);
    }
    REQUIRE(client.session.pending() == 0U);
    REQUIRE(server.session.pending() == 0U);
}

Bytes read_all(TlsSession& session)
{
    Bytes out;
    u8    chunk[4096];
    for (;;)
    {
        usize          written = 0U;
        const TlsError error   = session.read(Span<u8>(chunk, sizeof(chunk)), written);
        REQUIRE(error == TlsError::None);
        if (written == 0U)
        {
            return out;
        }
        for (usize i = 0U; i < written; ++i)
        {
            out.push_back(chunk[i]);
        }
    }
}

// Application data both ways, `size` bytes each.
void exchange(Endpoint& client, Endpoint& server, usize size)
{
    Bytes message;
    message.resize(size);
    for (usize i = 0U; i < size; ++i)
    {
        message[i] = static_cast<u8>(i * 7U + 3U);
    }
    REQUIRE(client.session.send(ConstSpan<u8>(message.data(), size)) == TlsError::None);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    const Bytes at_server = read_all(server.session);
    REQUIRE(at_server.size() == size);
    CHECK(std::memcmp(at_server.data(), message.data(), size) == 0);
    REQUIRE(server.session.send(ConstSpan<u8>(message.data(), size)) == TlsError::None);
    REQUIRE(deliver(server.session, client.session) == TlsError::None);
    const Bytes at_client = read_all(client.session);
    REQUIRE(at_client.size() == size);
    CHECK(std::memcmp(at_client.data(), message.data(), size) == 0);
}

u16 be16(const u8* p)
{
    return static_cast<u16>((p[0] << 8U) | p[1]);
}

u32 be24(const u8* p)
{
    return (static_cast<u32>(p[0]) << 16U) | (static_cast<u32>(p[1]) << 8U) | p[2];
}

// One key share as the wire carries it.
struct Share
{
    u16              group = 0U;
    ConstSpan<u8>    bytes;
};

// The key shares of the first handshake message of a flight (a ClientHello, a ServerHello or a HelloRetryRequest),
// read with the RFC 9846 section 4 layout. A HelloRetryRequest's key_share holds only the selected group (empty bytes).
usize read_shares(const Bytes& flight, Share (&out)[4])
{
    // A change_cipher_spec record (middlebox compatibility, RFC 9846 appendix D.4) may come first.
    usize record = 0U;
    while (record + 5U <= flight.size() && flight[record] == kRecordChangeCipherSpec)
    {
        record += 5U + be16(flight.data() + record + 3U);
    }
    REQUIRE(flight.size() > record + 9U);
    REQUIRE(flight[record] == kRecordHandshake);
    const u8*   message = flight.data() + record + 5U;
    const usize length  = be24(message + 1U);
    REQUIRE(record + 5U + 4U + length <= flight.size());
    const bool client_hello = message[0] == 1U;
    REQUIRE((client_hello || message[0] == 2U));
    usize at = 4U + 2U + 32U;             // header, legacy_version, random
    at += 1U + message[at];               // legacy_session_id
    if (client_hello)
    {
        at += 2U + be16(message + at);    // cipher_suites
        at += 1U + message[at];           // legacy_compression_methods
    }
    else
    {
        at += 2U + 1U;                    // cipher_suite, legacy_compression_method
    }
    const usize end   = 4U + length;
    usize       count = 0U;
    at += 2U;                             // extensions length
    while (at + 4U <= end)
    {
        const u16   type = be16(message + at);
        const usize size = be16(message + at + 2U);
        const u8*   body = message + at + 4U;
        at += 4U + size;
        if (type != kKeyShareExtension)
        {
            continue;
        }
        if (!client_hello)
        {
            const bool retry = size == 2U;
            out[count++]     = Share{be16(body), retry ? ConstSpan<u8>() : ConstSpan<u8>(body + 4U, be16(body + 2U))};
            continue;
        }
        for (usize share = 2U; share + 4U <= size && count < 4U; )
        {
            const usize bytes = be16(body + share + 2U);
            out[count++]      = Share{be16(body + share), ConstSpan<u8>(body + share + 4U, bytes)};
            share += 4U + bytes;
        }
    }
    REQUIRE(at == end);
    return count;
}

// The hybrid groups' layout (draft-ietf-tls-ecdhe-mlkem, RFC 10024): X25519MLKEM768 puts ML-KEM first, the SecP groups
// put the EC point first. The client's share is an encapsulation key and a public key; the server's a ciphertext and a
// public key. Each EC point must be on its curve and each encapsulation key must pass FIPS 203's modulus check, through
// the provider interface's own primitives, so a share in the wrong order fails here.
struct Layout
{
    AlgorithmId group;
    AlgorithmId curve;
    AlgorithmId kem;
    bool        kem_first;
};

constexpr Layout kHybridLayouts[] = {
    {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519, AlgorithmId::MlKem768, true},
    {AlgorithmId::Secp256r1Mlkem768, AlgorithmId::Secp256r1, AlgorithmId::MlKem768, false},
    {AlgorithmId::Secp384r1Mlkem1024, AlgorithmId::Secp384r1, AlgorithmId::MlKem1024, false},
};

void check_component(AlgorithmId curve, ConstSpan<u8> point)
{
    AgreementKey key;
    REQUIRE(key.generate(curve) == CryptoError::None);
    u8 secret[48];
    CHECK(key.derive(point, Span<u8>(secret, agreement_sizes(curve).secret)) == CryptoError::None);
}

void check_share(AlgorithmId group, ConstSpan<u8> share, bool client)
{
    if (algorithm_class(group) == AlgorithmClass::ClassicalKeyExchange)
    {
        REQUIRE(share.size() == agreement_sizes(group).public_key);
        check_component(group, share);
        return;
    }
    const Layout* layout = nullptr;
    for (const Layout& entry : kHybridLayouts)
    {
        layout = entry.group == group ? &entry : layout;
    }
    REQUIRE(layout != nullptr);
    const usize point = agreement_sizes(layout->curve).public_key;
    const usize kem   = client ? kem_sizes(layout->kem).public_key : kem_sizes(layout->kem).ciphertext;
    REQUIRE(share.size() == point + kem);
    const ConstSpan<u8> kem_part   = layout->kem_first ? share.subspan(0U, kem) : share.subspan(point, kem);
    const ConstSpan<u8> point_part = layout->kem_first ? share.subspan(kem, point) : share.subspan(0U, point);
    check_component(layout->curve, point_part);
    if (client)
    {
        u8 ciphertext[1568];
        u8 secret[32];
        CHECK(kem_encapsulate(layout->kem, kem_part, Span<u8>(ciphertext, kem_sizes(layout->kem).ciphertext),
                              Span<u8>(secret, sizeof(secret))) == CryptoError::None);
    }
}

// An established session that reads the peer's fatal alert fails with it.
void expect_failed_read(TlsSession& session, u8 alert)
{
    u8    out[16];
    usize written = 0U;
    CHECK(session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Failed);
    CHECK(written == 0U);
    CHECK(session.state() == TlsState::Failed);
    CHECK(session.alert_received().description == alert);
}

void establish(Endpoint& client, Endpoint& server)
{
    pump(client, server);
    REQUIRE(client.session.state() == TlsState::Established);
    REQUIRE(server.session.state() == TlsState::Established);
}
} // namespace

TEST_CASE("TLS: every group negotiates, with each key share laid out as its group defines", "[crypto][tls][net-2]")
{
    for (const AlgorithmId group : kAllGroups)
    {
        CAPTURE(static_cast<u16>(group));
        Endpoint client;
        Endpoint server;
        Options  options;
        options.client_groups = ConstSpan<AlgorithmId>(&group, 1U);
        options.server_groups = ConstSpan<AlgorithmId>(&group, 1U);
        setup(client, server, options);

        const Bytes hello = drain(client.session);
        Share       shares[4];
        REQUIRE(read_shares(hello, shares) == 1U);
        CHECK(shares[0].group == tls_group_codepoint(group));
        check_share(group, shares[0].bytes, true);
        REQUIRE(server.session.receive(ConstSpan<u8>(hello.data(), hello.size())) == TlsError::None);

        const Bytes answer = drain(server.session);
        REQUIRE(read_shares(answer, shares) == 1U);
        CHECK(shares[0].group == tls_group_codepoint(group));
        check_share(group, shares[0].bytes, false);
        REQUIRE(client.session.receive(ConstSpan<u8>(answer.data(), answer.size())) == TlsError::None);
        establish(client, server);

        CHECK(client.session.group() == group);
        CHECK(server.session.group() == group);
        CHECK_FALSE(client.session.hello_retry());
        CHECK_FALSE(server.session.hello_retry());
        CHECK(client.session.peer_algorithm() == AlgorithmId::Ed25519);
        CHECK(client.session.peer_key() == server.credential.key_id());
        CHECK(server.session.peer_algorithm() == AlgorithmId{});
        CHECK_FALSE(client.session.alert_sent().present);
        CHECK_FALSE(server.session.alert_received().present);
        exchange(client, server, 1000U);
    }
}

TEST_CASE("TLS: a client sends a post-quantum and a classical share, and the server's hybrid preference wins",
          "[crypto][tls][net-2]")
{
    // The client prefers X25519, but also shares its first post-quantum group: the server takes the hybrid group
    // without a HelloRetryRequest.
    constexpr AlgorithmId client_groups[] = {AlgorithmId::X25519, AlgorithmId::Secp256r1,
                                             AlgorithmId::X25519Mlkem768};
    constexpr AlgorithmId server_groups[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519};
    Endpoint              client;
    Endpoint              server;
    Options               options;
    options.client_groups = client_groups;
    options.server_groups = server_groups;
    setup(client, server, options);
    const Bytes hello = drain(client.session);
    Share       shares[4];
    REQUIRE(read_shares(hello, shares) == 2U);
    CHECK(shares[0].group == iana::kGroupX25519);
    CHECK(shares[1].group == iana::kGroupX25519mlkem768);
    REQUIRE(server.session.receive(ConstSpan<u8>(hello.data(), hello.size())) == TlsError::None);
    establish(client, server);
    CHECK(client.session.group() == AlgorithmId::X25519Mlkem768);
    CHECK(server.session.group() == AlgorithmId::X25519Mlkem768);
    CHECK_FALSE(client.session.hello_retry());
}

TEST_CASE("TLS: a server whose preferred group has no share asks for one", "[crypto][tls][net-2]")
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
        CAPTURE(static_cast<u16>(retry.wanted));
        const AlgorithmId client_groups[] = {retry.offered, retry.wanted};
        Endpoint          client;
        Endpoint          server;
        Options           options;
        options.client_groups = client_groups;
        options.server_groups = ConstSpan<AlgorithmId>(&retry.wanted, 1U);
        setup(client, server, options);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        const Bytes request = drain(server.session);
        Share       shares[4];
        REQUIRE(read_shares(request, shares) == 1U);
        CHECK(shares[0].group == tls_group_codepoint(retry.wanted));
        CHECK(shares[0].bytes.empty());
        REQUIRE(client.session.receive(ConstSpan<u8>(request.data(), request.size())) == TlsError::None);
        const Bytes second = drain(client.session);
        // The second ClientHello follows a change_cipher_spec only in middlebox compatibility mode; its first record
        // is the ClientHello either way when the client sends no legacy session identifier.
        REQUIRE(read_shares(second, shares) == 1U);
        CHECK(shares[0].group == tls_group_codepoint(retry.wanted));
        check_share(retry.wanted, shares[0].bytes, true);
        REQUIRE(server.session.receive(ConstSpan<u8>(second.data(), second.size())) == TlsError::None);
        establish(client, server);
        CHECK(client.session.hello_retry());
        CHECK(server.session.hello_retry());
        CHECK(client.session.group() == retry.wanted);
        CHECK(server.session.group() == retry.wanted);
    }
}

TEST_CASE("TLS: no common group fails with handshake_failure", "[crypto][tls][net-2]")
{
    constexpr AlgorithmId client_groups[] = {AlgorithmId::X25519, AlgorithmId::Secp256r1};
    constexpr AlgorithmId server_groups[] = {AlgorithmId::X25519Mlkem768};
    Endpoint              client;
    Endpoint              server;
    Options               options;
    options.client_groups = client_groups;
    options.server_groups = server_groups;
    setup(client, server, options);
    CHECK(deliver(client.session, server.session) == TlsError::Failed);
    CHECK(server.session.state() == TlsState::Failed);
    CHECK(server.session.alert_sent().present);
    CHECK(server.session.alert_sent().description == kHandshakeFailure);
    CHECK(deliver(server.session, client.session) == TlsError::Failed);
    CHECK(client.session.state() == TlsState::Failed);
    CHECK(client.session.alert_received().description == kHandshakeFailure);
    CHECK(server.session.receive(ConstSpan<u8>()) == TlsError::NotReady);
}

TEST_CASE("TLS: every signature algorithm authenticates a server, and the server's AEAD set bounds the choice",
          "[crypto][tls][net-2]")
{
    usize turn = 0U;
    for (const AlgorithmId algorithm : kAllSignatures)
    {
        CAPTURE(static_cast<u16>(algorithm));
        const AlgorithmId server_aead = kAllAeads[turn++ % 3U];
        Endpoint          client;
        Endpoint          server;
        Options           options;
        options.server_key   = algorithm;
        options.server_aeads = ConstSpan<AlgorithmId>(&server_aead, 1U);
        setup(client, server, options);
        establish(client, server);
        CHECK(client.session.peer_algorithm() == algorithm);
        CHECK(client.session.peer_key() == server.credential.key_id());
        CHECK(client.session.aead() == server_aead);
        CHECK(server.session.aead() == server_aead);
        exchange(client, server, 64U);
    }
}

TEST_CASE("TLS: the client's AEAD order decides, with ChaCha20-Poly1305 for a server without AES instructions",
          "[crypto][tls][net-2]")
{
    // The preference puts AES-GCM first exactly when the CPU has AES instructions.
    const ConstSpan<AlgorithmId> preferred = tls_aead_preference();
    REQUIRE(preferred.size() == 3U);
    CHECK((preferred[0] == AlgorithmId::Chacha20Poly1305) == !EVP_has_aes_hardware());
    for (const AlgorithmId aead : kAllAeads)
    {
        CHECK(std::find(preferred.begin(), preferred.end(), aead) != preferred.end());
    }

    constexpr AlgorithmId chacha_first[] = {AlgorithmId::Chacha20Poly1305, AlgorithmId::Aes128Gcm};
    constexpr AlgorithmId aes_first[]    = {AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305,
                                            AlgorithmId::Aes128Gcm};
    struct Order
    {
        ConstSpan<AlgorithmId> client;
        AlgorithmId            expected;
    };
    const Order orders[] = {
        {chacha_first, AlgorithmId::Chacha20Poly1305},
        {aes_first, EVP_has_aes_hardware() ? AlgorithmId::Aes256Gcm : AlgorithmId::Chacha20Poly1305},
        {preferred, preferred[0]},
    };
    for (const Order& order : orders)
    {
        Endpoint client;
        Endpoint server;
        Options  options;
        options.client_aeads = order.client;
        setup(client, server, options);
        establish(client, server);
        CHECK(client.session.aead() == order.expected);
        CHECK(server.session.aead() == order.expected);
    }
}

TEST_CASE("TLS: a server key that is not pinned, or of an algorithm not accepted, is refused", "[crypto][tls][net-2]")
{
    SECTION("another key")
    {
        Endpoint client;
        Endpoint server;
        Options  options;
        options.client_pins_server = false;
        setup(client, server, options);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        CHECK(deliver(server.session, client.session) == TlsError::PeerRefused);
        CHECK(client.session.state() == TlsState::Failed);
        CHECK(client.session.alert_sent().description == kBadCertificate);
        CHECK(client.session.peer_key() == KeyId{});
        CHECK(deliver(client.session, server.session) == TlsError::Failed);
        CHECK(server.session.alert_received().description == kBadCertificate);
    }
    SECTION("an algorithm the client does not accept")
    {
        constexpr AlgorithmId accepts[] = {AlgorithmId::MlDsa65};
        Endpoint              client;
        Endpoint              server;
        Options               options;
        options.client_accepts = accepts;
        options.server_key     = AlgorithmId::Ed25519;
        setup(client, server, options);
        pump(client, server);
        CHECK(client.session.state() == TlsState::Failed);
        CHECK(server.session.state() == TlsState::Failed);
    }
}

TEST_CASE("TLS: mutual authentication pins the client's key, and a server tells an unknown client nothing",
          "[crypto][tls][net-2]")
{
    SECTION("a pinned client")
    {
        Endpoint client;
        Endpoint server;
        Options  options;
        options.client_key         = AlgorithmId::MlDsa44;
        options.server_pins_client = true;
        setup(client, server, options);
        establish(client, server);
        CHECK(server.session.peer_algorithm() == AlgorithmId::MlDsa44);
        CHECK(server.session.peer_key() == client.credential.key_id());
        exchange(client, server, 10U);
    }
    SECTION("a client without a credential")
    {
        Endpoint client;
        Endpoint server;
        Options  options;
        options.server_pins_client = true;
        setup(client, server, options);
        pump(client, server);
        CHECK(server.session.state() == TlsState::Failed);
        CHECK(server.session.alert_sent().description == kCertificateRequired);
        // TLS 1.3 clients finish first: the client learns of the refusal from the alert after its handshake.
        expect_failed_read(client.session, kCertificateRequired);
    }
    SECTION("a client whose key is not pinned")
    {
        Endpoint client;
        Endpoint server;
        Options  options;
        options.client_key         = AlgorithmId::Ed25519;
        options.server_pins_client = true;
        setup(client, server, options);
        Endpoint other;
        make_key(other.key, AlgorithmId::Ed25519, 99U);
        // The server pins a key the client does not hold: its context is rebuilt around another pin.
        REQUIRE(other.credential.init(other.key, "other.cerid.test") == TlsError::None);
        const KeyId pin = other.credential.key_id();
        TlsConfig   config;
        config.role        = TlsRole::Server;
        config.groups      = kAllGroups;
        config.aeads       = kAllAeads;
        config.signatures  = kAllSignatures;
        config.pinned      = ConstSpan<KeyId>(&pin, 1U);
        config.credential  = &server.credential;
        config.server_name = kServerName;
        REQUIRE(server.context.init(config) == TlsError::None);
        REQUIRE(server.session.init(server.context) == TlsError::None);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        REQUIRE(deliver(server.session, client.session) == TlsError::None);
        CHECK(deliver(client.session, server.session) == TlsError::PeerRefused);
        CHECK(server.session.alert_sent().description == kTlsAlertGeneralError);
        CHECK(server.session.peer_key() == KeyId{});
        REQUIRE(deliver(server.session, client.session) == TlsError::None);
        expect_failed_read(client.session, kTlsAlertGeneralError);
    }
}

TEST_CASE("TLS: a named server refuses another name with general_error and matches names without case",
          "[crypto][tls][net-2]")
{
    SECTION("another name")
    {
        Endpoint client;
        Endpoint server;
        Options  options;
        options.client_names = "other.cerid.test";
        setup(client, server, options);
        CHECK(deliver(client.session, server.session) == TlsError::PeerRefused);
        CHECK(server.session.alert_sent().description == kTlsAlertGeneralError);
        CHECK(deliver(server.session, client.session) == TlsError::Failed);
    }
    SECTION("the name in other case")
    {
        Endpoint client;
        Endpoint server;
        Options  options;
        options.client_names = "GAME.Cerid.Test";
        setup(client, server, options);
        establish(client, server);
    }
    SECTION("a server without a name serves any")
    {
        Endpoint client;
        Endpoint server;
        Options  options;
        options.client_names = "anything.example";
        options.server_names = "";
        setup(client, server, options);
        establish(client, server);
    }
}

TEST_CASE("TLS: large messages, bounds and close_notify", "[crypto][tls][net-2]")
{
    Endpoint client;
    Endpoint server;
    setup(client, server, Options{});
    u8 scratch[16];
    usize written = 0U;
    CHECK(client.session.send(ConstSpan<u8>(scratch, 1U)) == TlsError::NotReady);
    CHECK(client.session.read(Span<u8>(scratch, sizeof(scratch)), written) == TlsError::NotReady);
    CHECK(client.session.close() == TlsError::NotReady);
    establish(client, server);
    exchange(client, server, 40000U); // three records each way

    // The bounds: more received bytes than a session holds, and a message that cannot fit, are refused whole.
    Bytes big;
    big.resize(kTlsMaxBufferedBytes + 1U);
    CHECK(server.session.receive(ConstSpan<u8>(big.data(), big.size())) == TlsError::Full);
    CHECK(server.session.state() == TlsState::Established);
    CHECK(client.session.send(ConstSpan<u8>(big.data(), kTlsMaxBufferedBytes)) == TlsError::Full);
    CHECK(client.session.pending() == 0U);
    CHECK(client.session.send(ConstSpan<u8>(big.data(), 60000U)) == TlsError::None);
    CHECK(client.session.send(ConstSpan<u8>(big.data(), 6000U)) == TlsError::Full);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    CHECK(read_all(server.session).size() == 60000U);

    REQUIRE(client.session.close() == TlsError::None);
    CHECK(client.session.state() == TlsState::Closed);
    CHECK(client.session.close() == TlsError::Closed);
    CHECK(client.session.send(ConstSpan<u8>(scratch, 1U)) == TlsError::Closed);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    CHECK(server.session.read(Span<u8>(scratch, sizeof(scratch)), written) == TlsError::Closed);
    CHECK(server.session.state() == TlsState::Closed);
    CHECK(server.session.alert_received().present);
    CHECK(server.session.alert_received().description == 0U); // close_notify
    REQUIRE(server.session.close() == TlsError::None);
    CHECK(client.session.receive(ConstSpan<u8>(scratch, 1U)) == TlsError::Closed);
}

TEST_CASE("TLS: adversarial bytes fail the session with an alert and nothing else", "[crypto][tls][net-2]")
{
    SECTION("a server fed garbage")
    {
        Endpoint client;
        Endpoint server;
        setup(client, server, Options{});
        u8 garbage[300];
        for (usize i = 0U; i < sizeof(garbage); ++i)
        {
            garbage[i] = static_cast<u8>(i * 31U + 5U);
        }
        CHECK(server.session.receive(ConstSpan<u8>(garbage, sizeof(garbage))) == TlsError::Failed);
        CHECK(server.session.state() == TlsState::Failed);
        CHECK(server.session.alert_sent().present);
        CHECK(server.session.pending() > 0U);
        CHECK(server.session.receive(ConstSpan<u8>(garbage, 1U)) == TlsError::NotReady);
    }
    SECTION("an established session fed a forged record")
    {
        Endpoint client;
        Endpoint server;
        setup(client, server, Options{});
        establish(client, server);
        const u8 hello[] = {'h', 'i'};
        REQUIRE(client.session.send(ConstSpan<u8>(hello, sizeof(hello))) == TlsError::None);
        Bytes record = drain(client.session);
        record[record.size() - 1U] ^= 0x01U;
        REQUIRE(server.session.receive(ConstSpan<u8>(record.data(), record.size())) == TlsError::None);
        u8    out[16];
        usize written = 0U;
        CHECK(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Failed);
        CHECK(written == 0U);
        CHECK(server.session.state() == TlsState::Failed);
        CHECK(server.session.alert_sent().description == 20U); // bad_record_mac
    }
}

TEST_CASE("TLS: configurations outside the profile are refused", "[crypto][tls][net-2]")
{
    Endpoint server;
    make_key(server.key, AlgorithmId::Ed25519, 0U);
    REQUIRE(server.credential.init(server.key, kServerName) == TlsError::None);
    const KeyId pin = server.credential.key_id();

    const auto client_with = [&](auto&& change) {
        TlsConfig config;
        config.role        = TlsRole::Client;
        config.groups      = kAllGroups;
        config.signatures  = kAllSignatures;
        config.aeads       = kAllAeads;
        config.pinned      = ConstSpan<KeyId>(&pin, 1U);
        config.server_name = kServerName;
        change(config);
        TlsContext context;
        return context.init(config);
    };
    CHECK(client_with([](TlsConfig&) {}) == TlsError::None);
    CHECK(client_with([](TlsConfig& c) { c.groups = {}; }) == TlsError::BadConfig);
    constexpr AlgorithmId wrong_class[] = {AlgorithmId::Aes128Gcm};
    CHECK(client_with([&](TlsConfig& c) { c.groups = wrong_class; }) == TlsError::BadConfig);
    CHECK(client_with([&](TlsConfig& c) { c.signatures = wrong_class; }) == TlsError::BadConfig);
    constexpr AlgorithmId kem_as_group[] = {AlgorithmId::MlKem768};
    CHECK(client_with([&](TlsConfig& c) { c.groups = kem_as_group; }) == TlsError::BadConfig);
    constexpr AlgorithmId unknown[] = {static_cast<AlgorithmId>(0x01FFU)};
    CHECK(client_with([&](TlsConfig& c) { c.groups = unknown; }) == TlsError::BadConfig);
    constexpr AlgorithmId twice[] = {AlgorithmId::X25519, AlgorithmId::X25519};
    CHECK(client_with([&](TlsConfig& c) { c.groups = twice; }) == TlsError::BadConfig);
    CHECK(client_with([](TlsConfig& c) { c.aeads = {}; }) == TlsError::BadConfig);
    constexpr AlgorithmId hkdf[] = {AlgorithmId::HkdfSha256};
    CHECK(client_with([&](TlsConfig& c) { c.aeads = hkdf; }) == TlsError::BadConfig);
    CHECK(client_with([](TlsConfig& c) { c.pinned = {}; }) == TlsError::BadConfig);
    CHECK(client_with([](TlsConfig& c) { c.signatures = {}; }) == TlsError::BadConfig);
    CHECK(client_with([](TlsConfig& c) { c.server_name = ""; }) == TlsError::BadConfig);
    KeyId many[kTlsMaxPins + 1U];
    CHECK(client_with([&](TlsConfig& c) { c.pinned = ConstSpan<KeyId>(many, kTlsMaxPins + 1U); }) ==
          TlsError::BadConfig);
    CHECK(client_with([&](TlsConfig& c) { c.pinned = ConstSpan<KeyId>(many, kTlsMaxPins); }) == TlsError::None);
    TlsCredential unready;
    CHECK(client_with([&](TlsConfig& c) { c.credential = &unready; }) == TlsError::BadConfig);

    const char* const bad_names[] = {"-game.cerid.test", "game-.cerid.test", "game..cerid.test", "game.cerid.test.",
                                     ".game", "game_1.test", "g\xC3\xA4me.test", "game cerid"};
    for (const char* name : bad_names)
    {
        CAPTURE(name);
        CHECK(client_with([&](TlsConfig& c) { c.server_name = name; }) == TlsError::BadConfig);
    }
    char long_label[64 + 6] = {};
    std::memset(long_label, 'a', 64U);
    std::memcpy(long_label + 64, ".test", sizeof(".test"));
    CHECK(client_with([&](TlsConfig& c) { c.server_name = long_label; }) == TlsError::BadConfig);
    long_label[63] = '.';
    CHECK(client_with([&](TlsConfig& c) { c.server_name = StringView(long_label, 63U); }) == TlsError::None);
    char long_name[kTlsMaxNameBytes + 2U] = {};
    for (usize i = 0U; i <= kTlsMaxNameBytes; ++i)
    {
        long_name[i] = i % 2U == 0U ? 'a' : '.';
    }
    CHECK(client_with([&](TlsConfig& c) { c.server_name = StringView(long_name, kTlsMaxNameBytes); }) ==
          TlsError::None);
    CHECK(client_with([&](TlsConfig& c) { c.server_name = StringView(long_name, kTlsMaxNameBytes + 2U); }) ==
          TlsError::BadConfig);

    TlsConfig server_config;
    server_config.role   = TlsRole::Server;
    server_config.groups = kAllGroups;
    server_config.aeads  = kAllAeads;
    TlsContext context;
    CHECK(context.init(server_config) == TlsError::BadConfig); // no credential
    server_config.credential = &server.credential;
    CHECK(context.init(server_config) == TlsError::None);
    server_config.signatures = kAllSignatures; // signatures without pins
    CHECK(context.init(server_config) == TlsError::BadConfig);
    CHECK_FALSE(context.ready());

    TlsSession session;
    CHECK(session.init(context) == TlsError::NotReady);
    usize written = 0U;
    u8    out[4];
    CHECK(session.take(Span<u8>(out, sizeof(out)), written) == TlsError::NotReady);
    CHECK(session.receive(ConstSpan<u8>(out, 1U)) == TlsError::NotReady);
    CHECK(session.state() == TlsState::Idle);

    SigningKey    empty;
    TlsCredential credential;
    CHECK(credential.init(empty, kServerName) == TlsError::NotReady);
    CHECK(credential.init(server.key, "not a name") == TlsError::BadConfig);
    CHECK_FALSE(credential.ready());

    CHECK(tls_group_codepoint(AlgorithmId::Aes128Gcm) == 0U);
    CHECK(tls_signature_codepoint(AlgorithmId::X25519) == 0U);
    CHECK(tls_error_name(TlsError::PeerRefused) == "peer-refused");
    CHECK(tls_error_name(static_cast<TlsError>(200U)) == "unknown");
}

TEST_CASE("TLS: credentials carry their key in a self-signed certificate that outlives the key object",
          "[crypto][tls][net-2]")
{
    // The credential shares the key, so clearing the SigningKey leaves the credential usable; clearing a context
    // leaves its sessions usable.
    Endpoint client;
    Endpoint server;
    setup(client, server, Options{});
    server.key.clear();
    client.context.clear();
    server.context.clear();
    establish(client, server);
    CHECK(client.session.peer_key() == server.credential.key_id());

    SigningKey key;
    make_key(key, AlgorithmId::EcdsaSecp384r1Sha384, 3U);
    TlsCredential first;
    TlsCredential second;
    REQUIRE(first.init(key, "a.test") == TlsError::None);
    REQUIRE(second.init(key, "b.test") == TlsError::None);
    u8 public_key[97];
    REQUIRE(key.public_key(Span<u8>(public_key, sizeof(public_key))) == CryptoError::None);
    CHECK(first.key_id() == key_id_of(ConstSpan<u8>(public_key, sizeof(public_key))));
    CHECK(first.key_id() == second.key_id());
    CHECK(first.algorithm() == AlgorithmId::EcdsaSecp384r1Sha384);
    first.clear();
    CHECK_FALSE(first.ready());
    CHECK(first.key_id() == KeyId{});
}

TEST_CASE("TLS: one server context serves sessions on several threads", "[crypto][tls][net-2]")
{
    Endpoint server;
    make_key(server.key, AlgorithmId::MlDsa65, 0U);
    REQUIRE(server.credential.init(server.key, kServerName) == TlsError::None);
    TlsConfig server_config;
    server_config.role        = TlsRole::Server;
    server_config.groups      = kAllGroups;
    server_config.aeads       = kAllAeads;
    server_config.credential  = &server.credential;
    server_config.server_name = kServerName;
    REQUIRE(server.context.init(server_config) == TlsError::None);
    const KeyId pin = server.credential.key_id();
    TlsConfig   client_config;
    client_config.groups      = kAllGroups;
    client_config.signatures  = kAllSignatures;
    client_config.aeads       = kAllAeads;
    client_config.pinned      = ConstSpan<KeyId>(&pin, 1U);
    client_config.server_name = kServerName;
    TlsContext client_context;
    REQUIRE(client_context.init(client_config) == TlsError::None);

    std::atomic<int> established{0};
    std::thread      threads[4];
    for (std::thread& thread : threads)
    {
        thread = std::thread(
            [&]
            {
                for (int i = 0; i < 8; ++i)
                {
                    TlsSession client;
                    TlsSession accepted;
                    if (client.init(client_context) != TlsError::None ||
                        accepted.init(server.context) != TlsError::None)
                    {
                        return;
                    }
                    u8 buffer[kTlsMaxBufferedBytes];
                    for (int round = 0; round < 8; ++round)
                    {
                        usize written = 0U;
                        (void)client.take(Span<u8>(buffer, sizeof(buffer)), written);
                        (void)accepted.receive(ConstSpan<u8>(buffer, written));
                        (void)accepted.take(Span<u8>(buffer, sizeof(buffer)), written);
                        (void)client.receive(ConstSpan<u8>(buffer, written));
                    }
                    if (client.state() == TlsState::Established && accepted.state() == TlsState::Established &&
                        client.group() == AlgorithmId::X25519Mlkem768)
                    {
                        established.fetch_add(1);
                    }
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(established.load() == 32);
}
