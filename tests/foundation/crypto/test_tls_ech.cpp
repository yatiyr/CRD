// crd-crypto -- Encrypted Client Hello (NET.2, RFC 9849; docs/design/network-foundation.md#ech, decision S14): ECH
// keys and the ECHConfigs and lists they publish, read with RFC 9849 section 4's layout; the true ClientHello reaching
// its server encrypted for every group, through a HelloRetryRequest and on resumption, with the server name absent
// from every byte the client sends; a stale configuration rejected, the retry configurations authenticated by a pinned
// key and used for one retried connection, a second rejection refused, a server that turned ECH off, a rejection that
// an unpinned server cannot authenticate and one under mutual authentication; key rotation; GREASE; the padding that
// hides the server name's length; lists the client skips or cannot use; the public name refused as a destination of
// its own; the same over QUIC; and every configuration refusal.
//
// The independent peer (OpenSSL 4.0's ECH) is tests/foundation/crypto/interop/tls_interop.py's.

#include <crd/containers/array.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <openssl/aead.h> // EVP_has_aes_hardware: the HPKE AEAD a client picks follows it

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iterator>

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
constexpr QuicLevel   kLevels[]   = {QuicLevel::Initial, QuicLevel::Handshake, QuicLevel::Application};

constexpr StringView kServerName = "game.cerid.test";
constexpr StringView kPublicName = "front.cerid.test";
constexpr StringView kGame[]     = {"cerid-game/1"};
constexpr u8         kParameters[] = {0x04U, 0x04U, 0x80U, 0x10U, 0x00U, 0x00U};

constexpr u16 kExtensionServerName   = 0U;
constexpr u16 kExtensionPreSharedKey = 41U;
constexpr u16 kExtensionEch          = 0xFE0DU;
constexpr u16 kExtensionOuter        = 0xFD00U; // ech_outer_extensions, never in a ClientHelloOuter
constexpr u16 kKemX25519             = 0x0020U;
constexpr u16 kKdfSha256             = 0x0001U;
constexpr u16 kAeadAes128Gcm         = 0x0001U;
constexpr u16 kAeadChacha20Poly1305  = 0x0003U;
constexpr u8  kRecordChangeCipherSpec = 20U;
constexpr u8  kRecordHandshake       = 22U;
constexpr u8  kGeneralError          = 117U;
constexpr u8  kBadCertificate        = 42U;
constexpr u8  kEchRequired           = 121U; // RFC 9849 section 11.2

u16 be16(const u8* p)
{
    return static_cast<u16>((p[0] << 8U) | p[1]);
}

void make_key(SigningKey& key, AlgorithmId algorithm, u8 salt)
{
    u8 seed[48];
    for (usize i = 0U; i < sizeof(seed); ++i)
    {
        seed[i] = static_cast<u8>(i + 1U + salt);
    }
    REQUIRE(key.from_seed(algorithm, ConstSpan<u8>(seed, signature_sizes(algorithm).seed)) == CryptoError::None);
}

// An ECH key from a fixed private key (`seed`), or a fresh one (0).
void make_ech(TlsEchKey& key, u8 seed, u8 config_id, StringView name = kPublicName, u8 max_name_length = 0U)
{
    u8 secret[kTlsEchKeyBytes];
    for (usize i = 0U; i < sizeof(secret); ++i)
    {
        secret[i] = static_cast<u8>(seed * 31U + i);
    }
    if (seed == 0U)
    {
        REQUIRE(random_bytes(Span<u8>(secret)) == CryptoError::None);
    }
    REQUIRE(key.init(ConstSpan<u8>(secret), config_id, name, max_name_length) == TlsError::None);
}

Bytes list_of(std::initializer_list<const TlsEchKey*> keys)
{
    Bytes list;
    list.resize(kTlsMaxEchConfigListBytes);
    usize written = 0U;
    REQUIRE(tls_ech_config_list(ConstSpan<const TlsEchKey*>(keys.begin(), keys.size()),
                                Span<u8>(list.data(), list.size()), written) == TlsError::None);
    list.resize(written);
    return list;
}

ConstSpan<u8> span_of(const Bytes& bytes)
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

bool contains(const Bytes& haystack, StringView needle)
{
    for (usize at = 0U; at + needle.size() <= haystack.size(); ++at)
    {
        if (std::memcmp(haystack.data() + at, needle.data(), needle.size()) == 0)
        {
            return true;
        }
    }
    return false;
}

// What a ClientHello (its handshake header first) shows on the wire, read with RFC 9846 section 4.1's and RFC 9849
// section 5's layouts.
struct Hello
{
    usize      size = 0U;
    StringView name;
    bool       ech           = false;
    u8         ech_type      = 0xFFU;
    u16        kdf           = 0U;
    u16        aead          = 0U;
    u8         config_id     = 0U;
    usize      enc_bytes     = 0U;
    usize      payload_bytes = 0U;
    bool       psk           = false;
    bool       outer_list    = false;
};

Hello read_hello(ConstSpan<u8> message)
{
    REQUIRE(message.size() > 4U + 2U + 32U + 1U);
    REQUIRE(message[0] == 1U);
    Hello hello;
    hello.size = message.size();
    usize at   = 4U + 2U + 32U;
    at += 1U + message[at];                    // legacy_session_id
    at += 2U + be16(message.data() + at);      // cipher_suites
    at += 1U + message[at];                    // legacy_compression_methods
    REQUIRE(at + 2U + be16(message.data() + at) == message.size());
    at += 2U;
    while (at + 4U <= message.size())
    {
        const u16   type = be16(message.data() + at);
        const usize size = be16(message.data() + at + 2U);
        const u8*   body = message.data() + at + 4U;
        REQUIRE(at + 4U + size <= message.size());
        if (type == kExtensionServerName)
        {
            REQUIRE(size >= 5U);
            hello.name = StringView(reinterpret_cast<const char*>(body + 5U), be16(body + 3U));
        }
        else if (type == kExtensionEch)
        {
            hello.ech      = true;
            hello.ech_type = body[0];
            if (hello.ech_type == 0U)
            {
                hello.kdf           = be16(body + 1U);
                hello.aead          = be16(body + 3U);
                hello.config_id     = body[5];
                hello.enc_bytes     = be16(body + 6U);
                hello.payload_bytes = be16(body + 8U + hello.enc_bytes);
                CHECK(10U + hello.enc_bytes + hello.payload_bytes == size);
            }
        }
        hello.psk        = hello.psk || type == kExtensionPreSharedKey;
        hello.outer_list = hello.outer_list || type == kExtensionOuter;
        at += 4U + size;
    }
    REQUIRE(at == message.size());
    return hello;
}

// The ClientHello at the front of a stream flight, after a change_cipher_spec record (middlebox compatibility, RFC
// 9846 appendix D.4) that a retried hello follows.
Hello read_stream_hello(const Bytes& flight)
{
    usize record = 0U;
    while (record + 5U <= flight.size() && flight[record] == kRecordChangeCipherSpec)
    {
        record += 5U + be16(flight.data() + record + 3U);
    }
    REQUIRE(flight.size() > record + 9U);
    REQUIRE(flight[record] == kRecordHandshake);
    const u8*   message = flight.data() + record + 5U;
    const usize length  = (static_cast<usize>(message[1]) << 16U) | (static_cast<usize>(message[2]) << 8U) | message[3];
    REQUIRE(record + 5U + 4U + length <= flight.size());
    return read_hello(ConstSpan<u8>(message, 4U + length));
}

u16 expected_hpke_aead()
{
    return EVP_has_aes_hardware() ? kAeadAes128Gcm : kAeadChacha20Poly1305;
}

// ---- Two stream ends ----

struct Endpoint
{
    SigningKey    key;
    TlsCredential credential;
    TlsContext    context;
    TlsSession    session;
    QuicHandshake quic;
};

struct Setup
{
    ConstSpan<u8>               client_ech;          // the client's ECHConfigList
    bool                        grease = true;
    ConstSpan<const TlsEchKey*> server_keys;
    ConstSpan<const TlsEchKey*> previous_keys;
    StringView                  client_name = kServerName;
    StringView                  server_name = kServerName; // empty: any
    ConstSpan<AlgorithmId>      client_groups = kAllGroups;
    ConstSpan<AlgorithmId>      server_groups = kAllGroups;
    bool                        client_pins_server = true;
    AlgorithmId                 client_key{};            // mutual authentication when set
    ConstSpan<StringView>       protocols;
};

void configure(Endpoint& client, Endpoint& server, const Setup& setup)
{
    make_key(server.key, AlgorithmId::Ed25519, 0U);
    REQUIRE(server.credential.init(server.key, kServerName) == TlsError::None);
    KeyId client_pin = server.credential.key_id();
    if (!setup.client_pins_server)
    {
        client_pin.bytes[0] ^= 1U;
    }
    TlsConfig client_config;
    client_config.role                  = TlsRole::Client;
    client_config.groups                = setup.client_groups;
    client_config.signatures            = kAllSignatures;
    client_config.aeads                 = kAllAeads;
    client_config.pinned                = ConstSpan<KeyId>(&client_pin, 1U);
    client_config.server_name           = setup.client_name;
    client_config.application_protocols = setup.protocols;
    client_config.ech_configs           = setup.client_ech;
    client_config.ech_grease            = setup.grease;
    KeyId server_pin;
    if (setup.client_key != AlgorithmId{})
    {
        make_key(client.key, setup.client_key, 7U);
        REQUIRE(client.credential.init(client.key, "player.cerid.test") == TlsError::None);
        client_config.credential = &client.credential;
        server_pin               = client.credential.key_id();
    }
    REQUIRE(client.context.init(client_config) == TlsError::None);

    TlsConfig server_config;
    server_config.role                  = TlsRole::Server;
    server_config.groups                = setup.server_groups;
    server_config.aeads                 = kAllAeads;
    server_config.credential            = &server.credential;
    server_config.server_name           = setup.server_name;
    server_config.application_protocols = setup.protocols;
    server_config.ech_keys              = setup.server_keys;
    server_config.previous_ech_keys     = setup.previous_keys;
    if (setup.client_key != AlgorithmId{})
    {
        server_config.signatures = kAllSignatures;
        server_config.pinned     = ConstSpan<KeyId>(&server_pin, 1U);
    }
    REQUIRE(server.context.init(server_config) == TlsError::None);
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

void append(Bytes& to, const Bytes& bytes)
{
    for (const u8 byte : bytes)
    {
        to.push_back(byte);
    }
}

// Flights back and forth until neither side has anything to send; every byte each side sent, and the first error each
// side's receive() returned.
struct Run
{
    Bytes    client_bytes;
    Bytes    server_bytes;
    Bytes    first_flight;
    TlsError client_error = TlsError::None;
    TlsError server_error = TlsError::None;
};

void note(TlsError& first, TlsError error)
{
    if (first == TlsError::None)
    {
        first = error;
    }
}

Run run(Endpoint& client, Endpoint& server)
{
    Run result;
    for (int round = 0; round < 16 && (client.session.pending() > 0U || server.session.pending() > 0U); ++round)
    {
        const Bytes from_client = drain(client.session);
        if (round == 0)
        {
            result.first_flight = from_client;
        }
        append(result.client_bytes, from_client);
        if (!from_client.empty() && server.session.state() != TlsState::Failed &&
            server.session.state() != TlsState::Idle)
        {
            note(result.server_error, server.session.receive(span_of(from_client)));
        }
        const Bytes from_server = drain(server.session);
        append(result.server_bytes, from_server);
        if (!from_server.empty() && client.session.state() != TlsState::Failed &&
            client.session.state() != TlsState::Idle)
        {
            note(result.client_error, client.session.receive(span_of(from_server)));
        }
    }
    REQUIRE(client.session.pending() == 0U);
    REQUIRE(server.session.pending() == 0U);
    return result;
}

Run connect(Endpoint& client, Endpoint& server, ConstSpan<u8> retry = {}, TlsTicketStore* tickets = nullptr)
{
    REQUIRE(client.session.init(client.context, tickets, retry) == TlsError::None);
    REQUIRE(server.session.init(server.context) == TlsError::None);
    return run(client, server);
}

// Application data both ways over an established pair.
void exchange(Endpoint& client, Endpoint& server)
{
    const u8 message[] = {'e', 'c', 'h', ' ', 'p', 'i', 'n', 'g'};
    u8       received[64];
    usize    written = 0U;
    REQUIRE(client.session.send(ConstSpan<u8>(message)) == TlsError::None);
    const Bytes to_server = drain(client.session);
    REQUIRE(server.session.receive(span_of(to_server)) == TlsError::None);
    REQUIRE(server.session.read(Span<u8>(received), written) == TlsError::None);
    REQUIRE(written == sizeof(message));
    CHECK(std::memcmp(received, message, sizeof(message)) == 0);
    REQUIRE(server.session.send(ConstSpan<u8>(message)) == TlsError::None);
    const Bytes to_client = drain(server.session);
    REQUIRE(client.session.receive(span_of(to_client)) == TlsError::None);
    REQUIRE(client.session.read(Span<u8>(received), written) == TlsError::None);
    REQUIRE(written == sizeof(message));
}

void expect_accepted(Endpoint& client, Endpoint& server, const Run& result)
{
    REQUIRE(client.session.state() == TlsState::Established);
    REQUIRE(server.session.state() == TlsState::Established);
    CHECK(client.session.ech() == TlsEch::Accepted);
    CHECK(server.session.ech() == TlsEch::Accepted);
    CHECK(client.session.ech_retry_configs().empty());
    // The true name never crosses the wire, either way; the public name stands in the outer hello.
    CHECK_FALSE(contains(result.client_bytes, kServerName));
    CHECK_FALSE(contains(result.server_bytes, kServerName));
    const Hello hello = read_stream_hello(result.first_flight);
    CHECK(hello.name == kPublicName);
    CHECK(hello.ech);
    CHECK(hello.ech_type == 0U);
    CHECK(hello.kdf == kKdfSha256);
    CHECK(hello.aead == expected_hpke_aead());
    CHECK(hello.enc_bytes == 32U);
    CHECK_FALSE(hello.outer_list);
    exchange(client, server);
}

// Who wins a client's rejected offer: the server refuses the public name once the client's Finished arrives, or fails
// first on the client's empty Certificate under mutual authentication; either way it never establishes.
void expect_rejected(Endpoint& client, Endpoint& server, const Run& result, TlsEch expected)
{
    CHECK(result.client_error == TlsError::EchRejected);
    CHECK(client.session.state() == TlsState::Failed);
    CHECK(client.session.ech() == expected);
    CHECK(client.session.alert_sent().present);
    CHECK(client.session.alert_sent().description == kEchRequired);
    CHECK(server.session.state() == TlsState::Failed);
    CHECK(server.session.ech() != TlsEch::Accepted);
    CHECK_FALSE(contains(result.client_bytes, kServerName));
}
} // namespace

TEST_CASE("TLS ECH: a key publishes its ECHConfig as RFC 9849 section 4 lays it out", "[crypto][tls][ech][net-2]")
{
    TlsEchKey key;
    make_ech(key, 3U, 0x5AU, kPublicName, 64U);
    REQUIRE(key.ready());
    const ConstSpan<u8> config = key.config();
    REQUIRE(config.size() == 55U + kPublicName.size());
    CHECK(be16(config.data()) == 0xFE0DU);                         // version
    CHECK(be16(config.data() + 2U) == config.size() - 4U);         // length
    CHECK(config[4] == 0x5AU);                                      // config_id
    CHECK(be16(config.data() + 5U) == kKemX25519);                  // kem_id
    CHECK(be16(config.data() + 7U) == 32U);                         // public_key
    CHECK(be16(config.data() + 41U) == 8U);                         // cipher_suites: two pairs
    CHECK(be16(config.data() + 43U) == kKdfSha256);
    CHECK(be16(config.data() + 45U) == kAeadAes128Gcm);
    CHECK(be16(config.data() + 47U) == kKdfSha256);
    CHECK(be16(config.data() + 49U) == kAeadChacha20Poly1305);
    CHECK(config[51] == 64U);                                       // maximum_name_length
    CHECK(config[52] == kPublicName.size());                        // public_name
    CHECK(std::memcmp(config.data() + 53U, kPublicName.data(), kPublicName.size()) == 0);
    CHECK(be16(config.data() + 53U + kPublicName.size()) == 0U);    // no extensions
    CHECK(key.config_id() == 0x5AU);
    CHECK(key.public_name() == kPublicName);

    // The same private key publishes the same public key; another one another.
    TlsEchKey same;
    TlsEchKey other;
    make_ech(same, 3U, 0x5AU, kPublicName, 64U);
    make_ech(other, 4U, 0x5AU, kPublicName, 64U);
    CHECK(std::memcmp(same.config().data(), config.data(), config.size()) == 0);
    CHECK(std::memcmp(other.config().data() + 9U, config.data() + 9U, 32U) != 0);

    // A list holds the configurations in order after its length.
    const Bytes list = list_of({&key, &other});
    REQUIRE(list.size() == 2U + 2U * config.size());
    CHECK(be16(list.data()) == list.size() - 2U);
    CHECK(std::memcmp(list.data() + 2U, config.data(), config.size()) == 0);

    // Refusals: the key's length, names that are not DNS names or read as IPv4 literals; a refused init clears.
    u8 secret[kTlsEchKeyBytes] = {1U};
    CHECK(key.init(ConstSpan<u8>(secret, 31U), 1U, kPublicName) == TlsError::BadConfig);
    CHECK_FALSE(key.ready());
    CHECK(key.config().empty());
    for (const StringView name : {StringView("192.0.2.1"), StringView("front.cerid.0x1F"), StringView("front.0X"),
                                  StringView("front.123"), StringView("-front.cerid.test"), StringView(""),
                                  StringView("front..test"), StringView("front.cerid.test.")})
    {
        CAPTURE(name);
        CHECK(key.init(ConstSpan<u8>(secret), 1U, name) == TlsError::BadConfig);
    }
    for (const StringView name : {StringView("front.0x1g"), StringView("front.1a"), StringView("localhost")})
    {
        CAPTURE(name);
        CHECK(key.init(ConstSpan<u8>(secret), 1U, name) == TlsError::None);
    }
    char longest[kTlsMaxNameBytes];
    for (usize i = 0U; i < sizeof(longest); ++i)
    {
        longest[i] = (i % 64U) == 63U ? '.' : 'a';
    }
    REQUIRE(key.init(ConstSpan<u8>(secret), 1U, StringView(longest, sizeof(longest))) == TlsError::None);
    CHECK(key.config().size() == kTlsMaxEchConfigBytes);

    // The list builder: no key, a key not ready, more than the bound, a buffer too small.
    TlsEchKey   idle;
    u8          out[2048];
    usize       written = 1U;
    CHECK(tls_ech_config_list({}, Span<u8>(out), written) == TlsError::BadConfig);
    CHECK(written == 0U);
    const TlsEchKey* with_idle[] = {&other, &idle};
    CHECK(tls_ech_config_list(ConstSpan<const TlsEchKey*>(with_idle), Span<u8>(out), written) == TlsError::BadConfig);
    const TlsEchKey* five[] = {&other, &other, &other, &other, &other};
    CHECK(tls_ech_config_list(ConstSpan<const TlsEchKey*>(five), Span<u8>(out), written) == TlsError::BadConfig);
    const TlsEchKey* one[] = {&other};
    CHECK(tls_ech_config_list(ConstSpan<const TlsEchKey*>(one), Span<u8>(out, 2U + 70U), written) == TlsError::Full);
    CHECK(written == 0U);
    CHECK(tls_ech_config_list(ConstSpan<const TlsEchKey*>(one), Span<u8>(out, 2U + 71U), written) == TlsError::None);
    CHECK(written == 2U + 71U);
}

TEST_CASE("TLS ECH: configurations outside the profile are refused", "[crypto][tls][ech][net-2]")
{
    TlsEchKey a;
    TlsEchKey b;
    TlsEchKey c;
    TlsEchKey d;
    TlsEchKey e;
    make_ech(a, 1U, 1U);
    make_ech(b, 2U, 2U);
    make_ech(c, 3U, 3U);
    make_ech(d, 4U, 4U);
    make_ech(e, 5U, 5U);
    TlsEchKey twin;
    make_ech(twin, 6U, 1U);
    TlsEchKey idle;
    const Bytes list = list_of({&a});

    SigningKey    signing;
    TlsCredential credential;
    make_key(signing, AlgorithmId::Ed25519, 0U);
    REQUIRE(credential.init(signing, kServerName) == TlsError::None);
    const KeyId pin = credential.key_id();

    const auto server_with = [&](ConstSpan<const TlsEchKey*> keys, ConstSpan<const TlsEchKey*> previous,
                                 ConstSpan<u8> configs) {
        TlsConfig config;
        config.role              = TlsRole::Server;
        config.groups            = kAllGroups;
        config.aeads             = kAllAeads;
        config.credential        = &credential;
        config.ech_keys          = keys;
        config.previous_ech_keys = previous;
        config.ech_configs       = configs;
        TlsContext context;
        return context.init(config);
    };
    const auto client_config = [&](ConstSpan<u8> configs) {
        TlsConfig config;
        config.role        = TlsRole::Client;
        config.groups      = kAllGroups;
        config.signatures  = kAllSignatures;
        config.aeads       = kAllAeads;
        config.pinned      = ConstSpan<KeyId>(&pin, 1U);
        config.server_name = kServerName;
        config.ech_configs = configs;
        return config;
    };

    const TlsEchKey* four[]      = {&a, &b, &c, &d};
    const TlsEchKey* current[]   = {&a, &b};
    const TlsEchKey* older[]     = {&c, &d, &e};
    const TlsEchKey* twins[]     = {&a, &twin};
    const TlsEchKey* not_ready[] = {&a, &idle};
    const TlsEchKey* missing[]   = {nullptr};
    const TlsEchKey* other_old[] = {&twin};
    CHECK(server_with(ConstSpan<const TlsEchKey*>(four), {}, {}) == TlsError::None);
    CHECK(server_with(ConstSpan<const TlsEchKey*>(current), ConstSpan<const TlsEchKey*>(older), {}) ==
          TlsError::BadConfig);
    CHECK(server_with(ConstSpan<const TlsEchKey*>(twins), {}, {}) == TlsError::BadConfig);
    CHECK(server_with(ConstSpan<const TlsEchKey*>(current), ConstSpan<const TlsEchKey*>(other_old), {}) ==
          TlsError::BadConfig);
    CHECK(server_with(ConstSpan<const TlsEchKey*>(not_ready), {}, {}) == TlsError::BadConfig);
    CHECK(server_with(ConstSpan<const TlsEchKey*>(missing), {}, {}) == TlsError::BadConfig);
    CHECK(server_with({}, ConstSpan<const TlsEchKey*>(current), {}) == TlsError::BadConfig);
    CHECK(server_with({}, {}, span_of(list)) == TlsError::BadConfig);

    TlsContext client;
    TlsConfig  with_keys = client_config({});
    with_keys.ech_keys   = ConstSpan<const TlsEchKey*>(current);
    CHECK(client.init(with_keys) == TlsError::BadConfig);
    CHECK(client.init(client_config(span_of(list))) == TlsError::None);

    // Malformed lists: a length that is not the rest, a truncated configuration, nothing listed, only other versions,
    // past the bound.
    Bytes bad = list;
    bad[1]    = static_cast<u8>(bad[1] + 1U);
    CHECK(client.init(client_config(span_of(bad))) == TlsError::BadConfig);
    bad = list;
    bad.pop_back();
    CHECK(client.init(client_config(span_of(bad))) == TlsError::BadConfig);
    const u8 empty_list[] = {0U, 0U};
    CHECK(client.init(client_config(ConstSpan<u8>(empty_list))) == TlsError::BadConfig);
    Bytes other_version = list;
    other_version[2]    = 0xFFU;
    other_version[3]    = 0x01U;
    CHECK(client.init(client_config(span_of(other_version))) == TlsError::BadConfig);
    Bytes huge;
    huge.resize(kTlsMaxEchConfigListBytes + 1U);
    huge[0] = static_cast<u8>((huge.size() - 2U) >> 8U);
    huge[1] = static_cast<u8>(huge.size() - 2U);
    huge[2] = 0xFFU;
    huge[3] = 0x01U;
    huge[4] = static_cast<u8>((huge.size() - 6U) >> 8U);
    huge[5] = static_cast<u8>(huge.size() - 6U);
    CHECK(client.init(client_config(span_of(huge))) == TlsError::BadConfig);

    // A well-formed list of version 0xfe0d whose only configuration uses a KEM the client lacks (ML-KEM-768's HPKE
    // identifier): the session refuses to start, and nothing waits to be sent.
    Bytes unusable = list;
    unusable[2 + 5] = 0x00U;
    unusable[2 + 6] = 0x41U;
    REQUIRE(client.init(client_config(span_of(unusable))) == TlsError::None);
    TlsSession session;
    CHECK(session.init(client) == TlsError::BadConfig);
    CHECK(session.state() == TlsState::Idle);
    CHECK(session.pending() == 0U);
    QuicHandshake quic;
    TlsConfig     quic_config    = client_config(span_of(unusable));
    quic_config.application_protocols = kGame;
    REQUIRE(client.init(quic_config) == TlsError::None);
    CHECK(quic.init(client, ConstSpan<u8>(kParameters)) == TlsError::BadConfig);
    CHECK(quic.state() == TlsState::Idle);
    CHECK(quic.pending(QuicLevel::Initial) == 0U);

    // Retry configurations: never to a server, never malformed, never without a configuration of version 0xfe0d.
    REQUIRE(client.init(client_config({})) == TlsError::None);
    CHECK(session.init(client, nullptr, span_of(bad)) == TlsError::BadConfig);
    CHECK(session.init(client, nullptr, span_of(other_version)) == TlsError::BadConfig);
    CHECK(session.init(client, nullptr, span_of(huge)) == TlsError::BadConfig);
    CHECK(session.init(client, nullptr, span_of(list)) == TlsError::None);
    const TlsEchKey* served[] = {&a};
    TlsConfig        server;
    server.role       = TlsRole::Server;
    server.groups     = kAllGroups;
    server.aeads      = kAllAeads;
    server.credential = &credential;
    server.ech_keys   = ConstSpan<const TlsEchKey*>(served);
    TlsContext server_context;
    REQUIRE(server_context.init(server) == TlsError::None);
    CHECK(session.init(server_context, nullptr, span_of(list)) == TlsError::BadConfig);
    CHECK(session.init(server_context) == TlsError::None);

    CHECK(tls_error_name(TlsError::EchRejected) == "ech-rejected");
    CHECK(tls_ech_name(TlsEch::None) == "none");
    CHECK(tls_ech_name(TlsEch::Accepted) == "accepted");
    CHECK(tls_ech_name(TlsEch::Retry) == "retry");
    CHECK(tls_ech_name(TlsEch::Disabled) == "disabled");
    CHECK(tls_ech_name(TlsEch::Refused) == "refused");
}

TEST_CASE("TLS ECH: the true ClientHello reaches its server encrypted for every group, and its name never crosses "
          "the wire",
          "[crypto][tls][ech][net-2]")
{
    TlsEchKey key;
    make_ech(key, 1U, 7U);
    const Bytes      list   = list_of({&key});
    const TlsEchKey* keys[] = {&key};
    for (const AlgorithmId group : kAllGroups)
    {
        CAPTURE(group);
        Endpoint          client;
        Endpoint          server;
        const AlgorithmId only[] = {group};
        Setup             setup;
        setup.client_ech    = span_of(list);
        setup.server_keys   = ConstSpan<const TlsEchKey*>(keys);
        setup.client_groups = ConstSpan<AlgorithmId>(only);
        setup.server_groups = ConstSpan<AlgorithmId>(only);
        configure(client, server, setup);
        const Run result = connect(client, server);
        expect_accepted(client, server, result);
        CHECK(client.session.group() == group);
        CHECK(read_stream_hello(result.first_flight).config_id == 7U);
    }

    // A server whose preferred group has no share asks for one; the second ClientHello offers ECH again, and the
    // server keeps decrypting with the same context (RFC 9849 section 6.1.5).
    Endpoint          client;
    Endpoint          server;
    const AlgorithmId offered[]   = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519, AlgorithmId::Secp384r1Mlkem1024};
    const AlgorithmId preferred[] = {AlgorithmId::Secp384r1Mlkem1024};
    Setup             setup;
    setup.client_ech    = span_of(list);
    setup.server_keys   = ConstSpan<const TlsEchKey*>(keys);
    setup.client_groups = ConstSpan<AlgorithmId>(offered);
    setup.server_groups = ConstSpan<AlgorithmId>(preferred);
    configure(client, server, setup);
    REQUIRE(client.session.init(client.context) == TlsError::None);
    REQUIRE(server.session.init(server.context) == TlsError::None);
    const Bytes first = drain(client.session);
    REQUIRE(server.session.receive(span_of(first)) == TlsError::None);
    const Bytes retry_request = drain(server.session);
    REQUIRE(client.session.receive(span_of(retry_request)) == TlsError::None);
    const Bytes second = drain(client.session);
    const Hello again  = read_stream_hello(second);
    CHECK(again.ech);
    CHECK(again.ech_type == 0U);
    CHECK(again.name == kPublicName);
    CHECK(again.enc_bytes == 0U); // the HPKE context continues: no new encapsulation
    REQUIRE(server.session.receive(span_of(second)) == TlsError::None);
    Run rest = run(client, server);
    append(rest.client_bytes, first);
    append(rest.client_bytes, second);
    rest.first_flight = first;
    CHECK(client.session.hello_retry());
    CHECK(client.session.group() == AlgorithmId::Secp384r1Mlkem1024);
    expect_accepted(client, server, rest);
}

namespace
{
// A client's tickets: the latest one, handed out once.
class Store final : public TlsTicketStore
{
public:
    void keep(const TlsBinding&, ConstSpan<u8> ticket) noexcept override
    {
        m_ticket.resize(ticket.size());
        std::memcpy(m_ticket.data(), ticket.data(), ticket.size());
    }

    ConstSpan<u8> take(const TlsBinding&) noexcept override
    {
        m_handed = m_ticket;
        m_ticket.clear();
        return span_of(m_handed);
    }

private:
    Bytes m_ticket;
    Bytes m_handed;
};

void read_tickets(TlsSession& session)
{
    u8    buffer[64];
    usize written = 0U;
    REQUIRE(session.read(Span<u8>(buffer), written) == TlsError::None);
}
} // namespace

TEST_CASE("TLS ECH: a ticket resumes inside the encrypted hello, never in the outer one", "[crypto][tls][ech][net-2]")
{
    TlsEchKey key;
    make_ech(key, 1U, 7U);
    const Bytes      list   = list_of({&key});
    const TlsEchKey* keys[] = {&key};
    Endpoint         client;
    Endpoint         server;
    Setup            setup;
    setup.client_ech  = span_of(list);
    setup.server_keys = ConstSpan<const TlsEchKey*>(keys);
    configure(client, server, setup);
    Store store;
    Run   result = connect(client, server, {}, &store);
    expect_accepted(client, server, result);
    read_tickets(client.session);

    result = connect(client, server, {}, &store);
    expect_accepted(client, server, result);
    CHECK(client.session.resumed());
    CHECK(server.session.resumed());
    CHECK_FALSE(read_stream_hello(result.first_flight).psk);
}

TEST_CASE("TLS ECH: a stale configuration is rejected, its retry configurations authenticated and used once",
          "[crypto][tls][ech][net-2]")
{
    TlsEchKey stale;
    TlsEchKey current;
    TlsEchKey older;
    TlsEchKey next;
    make_ech(stale, 1U, 1U);
    make_ech(current, 2U, 2U);
    make_ech(older, 3U, 3U);
    make_ech(next, 4U, 4U);
    const Bytes      stale_list   = list_of({&stale});
    const Bytes      current_list = list_of({&current});
    const TlsEchKey* keys[]       = {&current};
    const TlsEchKey* previous[]   = {&older};

    for (const bool mutual : {false, true})
    {
        CAPTURE(mutual);
        Endpoint client;
        Endpoint server;
        Setup    setup;
        setup.client_ech    = span_of(stale_list);
        setup.server_keys   = ConstSpan<const TlsEchKey*>(keys);
        setup.previous_keys = ConstSpan<const TlsEchKey*>(previous);
        if (mutual)
        {
            setup.client_key = AlgorithmId::MlDsa44;
        }
        configure(client, server, setup);
        Run result = connect(client, server);
        expect_rejected(client, server, result, TlsEch::Retry);
        // The server answered under its public name, then refused it (or, under mutual authentication, the client's
        // empty Certificate: a rejected client never shows its credential).
        CHECK(result.server_error == (mutual ? TlsError::Failed : TlsError::PeerRefused));
        if (!mutual)
        {
            CHECK(server.session.alert_sent().description == kGeneralError);
        }
        CHECK(server.session.peer_key() == KeyId{});
        // Only the current keys are retry configurations.
        const ConstSpan<u8> retry = client.session.ech_retry_configs();
        REQUIRE(retry.size() == current_list.size());
        CHECK(std::memcmp(retry.data(), current_list.data(), retry.size()) == 0);
        const Bytes kept(retry.begin(), retry.end());

        // One retried connection with them: accepted.
        result = connect(client, server, span_of(kept));
        expect_accepted(client, server, result);
        if (mutual)
        {
            CHECK(server.session.peer_key() == client.credential.key_id());
        }
    }

    // A connection that already retried and is rejected again: the server's configurations disagree.
    Endpoint client;
    Endpoint server;
    Setup    setup;
    setup.client_ech              = span_of(stale_list);
    const TlsEchKey* rotated[]    = {&next};
    setup.server_keys             = ConstSpan<const TlsEchKey*>(rotated);
    configure(client, server, setup);
    const Run result = connect(client, server, span_of(current_list));
    expect_rejected(client, server, result, TlsEch::Refused);
    CHECK(client.session.ech_retry_configs().empty());

    // The previous key still decrypts.
    Endpoint old_client;
    Endpoint old_server;
    Setup    rotation;
    const Bytes older_list   = list_of({&older});
    rotation.client_ech      = span_of(older_list);
    rotation.server_keys     = ConstSpan<const TlsEchKey*>(keys);
    rotation.previous_keys   = ConstSpan<const TlsEchKey*>(previous);
    configure(old_client, old_server, rotation);
    expect_accepted(old_client, old_server, connect(old_client, old_server));
}

TEST_CASE("TLS ECH: a rejection only a pinned key authenticates, and a server that turned ECH off",
          "[crypto][tls][ech][net-2]")
{
    TlsEchKey stale;
    TlsEchKey current;
    make_ech(stale, 1U, 1U);
    make_ech(current, 2U, 2U);
    const Bytes      stale_list = list_of({&stale});
    const TlsEchKey* keys[]     = {&current};

    // A server key that is not pinned: the rejection is not authenticated, and its retry configurations are never
    // used (RFC 9849 section 6.1.6).
    {
        Endpoint client;
        Endpoint server;
        Setup    setup;
        setup.client_ech         = span_of(stale_list);
        setup.server_keys        = ConstSpan<const TlsEchKey*>(keys);
        setup.client_pins_server = false;
        configure(client, server, setup);
        const Run result = connect(client, server);
        CHECK(result.client_error == TlsError::PeerRefused);
        CHECK(client.session.ech() == TlsEch::None);
        CHECK(client.session.ech_retry_configs().empty());
        CHECK(client.session.alert_sent().description == kBadCertificate);
    }

    // A server without ECH keys that answers the public name (one without a name, or named so): the client learns,
    // authenticated, that ECH is off; whether to connect without it is the caller's.
    for (const StringView name : {StringView(), kPublicName})
    {
        CAPTURE(name);
        Endpoint client;
        Endpoint server;
        Setup    setup;
        setup.client_ech  = span_of(stale_list);
        setup.server_name = name;
        configure(client, server, setup);
        const Run result = connect(client, server);
        CHECK(result.client_error == TlsError::EchRejected);
        CHECK(client.session.ech() == TlsEch::Disabled);
        CHECK(client.session.ech_retry_configs().empty());
        CHECK(client.session.alert_sent().description == kEchRequired);
        // The server completed its handshake on the client's Finished; the alert behind it fails the session.
        u8    buffer[16];
        usize written = 0U;
        CHECK(server.session.read(Span<u8>(buffer), written) == TlsError::Failed);
        CHECK(server.session.alert_received().description == kEchRequired);
    }

    // A server without ECH keys whose name is not the public name refuses the outer hello itself.
    Endpoint client;
    Endpoint server;
    Setup    setup;
    setup.client_ech = span_of(stale_list);
    configure(client, server, setup);
    const Run result = connect(client, server);
    CHECK(result.server_error == TlsError::PeerRefused);
    CHECK(client.session.state() == TlsState::Failed);
    CHECK(client.session.ech() == TlsEch::None);
}

TEST_CASE("TLS ECH: a hello without ECH that names the public name is refused once its handshake ends",
          "[crypto][tls][ech][net-2]")
{
    TlsEchKey key;
    make_ech(key, 1U, 1U);
    const TlsEchKey* keys[] = {&key};
    for (const bool grease : {true, false})
    {
        CAPTURE(grease);
        Endpoint client;
        Endpoint server;
        Setup    setup;
        setup.server_keys = ConstSpan<const TlsEchKey*>(keys);
        setup.client_name = kPublicName;
        setup.grease      = grease;
        configure(client, server, setup);
        const Run result = connect(client, server);
        CHECK(result.server_error == TlsError::PeerRefused);
        CHECK(server.session.state() == TlsState::Failed);
        CHECK(server.session.alert_sent().description == kGeneralError);
        // The client finished first; the server's alert waits behind its Finished and fails the session at once.
        u8    buffer[16];
        usize written = 0U;
        CHECK(client.session.read(Span<u8>(buffer), written) == TlsError::Failed);
        CHECK(client.session.alert_received().description == kGeneralError);
    }

    // The server's own name, by a client that offers ECH through GREASE only: served.
    Endpoint client;
    Endpoint server;
    Setup    setup;
    setup.server_keys = ConstSpan<const TlsEchKey*>(keys);
    configure(client, server, setup);
    const Run result = connect(client, server);
    REQUIRE(client.session.state() == TlsState::Established);
    REQUIRE(server.session.state() == TlsState::Established);
    CHECK(client.session.ech() == TlsEch::None);
    CHECK(server.session.ech() == TlsEch::None);
    exchange(client, server);
}

TEST_CASE("TLS ECH: GREASE makes a hello without a configuration look like one with it", "[crypto][tls][ech][net-2]")
{
    TlsEchKey key;
    make_ech(key, 1U, 9U);
    const Bytes      list   = list_of({&key});
    const TlsEchKey* keys[] = {&key};

    Hello real;
    {
        Endpoint client;
        Endpoint server;
        Setup    setup;
        setup.client_ech  = span_of(list);
        setup.server_keys = ConstSpan<const TlsEchKey*>(keys);
        configure(client, server, setup);
        real = read_stream_hello(connect(client, server).first_flight);
    }
    u8 config_ids[8] = {};
    for (usize i = 0U; i < 8U; ++i)
    {
        Endpoint client;
        Endpoint server;
        Setup    setup;
        setup.server_keys = ConstSpan<const TlsEchKey*>(keys);
        configure(client, server, setup);
        const Run   result = connect(client, server);
        const Hello grease = read_stream_hello(result.first_flight);
        // Served without ECH; the retry configurations the server sends a GREASE client are ignored.
        REQUIRE(client.session.state() == TlsState::Established);
        CHECK(client.session.ech() == TlsEch::None);
        CHECK(grease.ech);
        CHECK(grease.ech_type == 0U);
        CHECK(grease.kdf == kKdfSha256);
        CHECK(grease.aead == real.aead);
        CHECK(grease.enc_bytes == real.enc_bytes);
        CHECK(grease.name == kServerName);
        config_ids[i] = grease.config_id;
    }
    CHECK(std::count(std::begin(config_ids), std::end(config_ids), config_ids[0]) < 8);

    Endpoint client;
    Endpoint server;
    Setup    setup;
    setup.grease = false;
    configure(client, server, setup);
    CHECK_FALSE(read_stream_hello(connect(client, server).first_flight).ech);
}

TEST_CASE("TLS ECH: the inner hello is padded to the configuration's longest name", "[crypto][tls][ech][net-2]")
{
    // Two server names 48 bytes apart: equal ClientHellos under a maximum_name_length that covers both, unequal ones
    // without it (the padding rounds to 32 bytes only, RFC 9849 section 6.1.3).
    constexpr StringView short_name = "a.cerid.test";
    constexpr StringView long_name  = "a-much-longer-server-name-for-the-anonymity-set.cerid.test";
    for (const u8 max_name_length : {u8{0U}, u8{64U}})
    {
        CAPTURE(max_name_length);
        TlsEchKey key;
        make_ech(key, 1U, 1U, kPublicName, max_name_length);
        const Bytes list = list_of({&key});
        usize       sizes[2] = {};
        for (usize i = 0U; i < 2U; ++i)
        {
            Endpoint client;
            Endpoint server;
            Setup    setup;
            setup.client_ech  = span_of(list);
            setup.client_name = i == 0U ? short_name : long_name;
            configure(client, server, setup);
            REQUIRE(client.session.init(client.context) == TlsError::None);
            sizes[i] = read_stream_hello(drain(client.session)).size;
        }
        CAPTURE(sizes[0], sizes[1]);
        CHECK((sizes[0] == sizes[1]) == (max_name_length != 0U));
    }
}

TEST_CASE("TLS ECH: a client skips configurations it cannot use and takes the first it can",
          "[crypto][tls][ech][net-2]")
{
    TlsEchKey first;
    TlsEchKey second;
    make_ech(first, 1U, 1U);
    make_ech(second, 2U, 2U);
    // An ECHConfig of another version, then one with an HPKE KEM the client lacks, then the server's.
    Bytes list;
    list.push_back(0U);
    list.push_back(0U);
    const u8 other_version[] = {0xFFU, 0x01U, 0x00U, 0x03U, 1U, 2U, 3U};
    for (const u8 byte : other_version)
    {
        list.push_back(byte);
    }
    for (usize i = 0U; i < first.config().size(); ++i)
    {
        list.push_back(i == 6U ? u8{0x41U} : first.config()[i]); // kem_id 0x0041
    }
    for (const u8 byte : second.config())
    {
        list.push_back(byte);
    }
    list[0] = static_cast<u8>((list.size() - 2U) >> 8U);
    list[1] = static_cast<u8>(list.size() - 2U);

    const TlsEchKey* keys[] = {&first, &second};
    Endpoint         client;
    Endpoint         server;
    Setup            setup;
    setup.client_ech  = span_of(list);
    setup.server_keys = ConstSpan<const TlsEchKey*>(keys);
    configure(client, server, setup);
    const Run result = connect(client, server);
    expect_accepted(client, server, result);
    CHECK(read_stream_hello(result.first_flight).config_id == 2U);
}

namespace
{
// ---- Two QUIC ends ----

Bytes drain(QuicHandshake& handshake, QuicLevel level)
{
    Bytes out;
    out.resize(handshake.pending(level));
    usize written = 0U;
    REQUIRE(handshake.take(level, Span<u8>(out.data(), out.size()), written) == TlsError::None);
    REQUIRE(written == out.size());
    return out;
}

bool quic_live(const QuicHandshake& handshake)
{
    return handshake.state() == TlsState::Handshaking || handshake.state() == TlsState::Established;
}

struct QuicRun
{
    Bytes    client_bytes;
    Bytes    first_hello;
    TlsError client_error = TlsError::None;
    TlsError server_error = TlsError::None;
};

QuicRun quic_connect(Endpoint& client, Endpoint& server, ConstSpan<u8> retry = {})
{
    REQUIRE(client.quic.init(client.context, ConstSpan<u8>(kParameters), nullptr, retry) == TlsError::None);
    REQUIRE(server.quic.init(server.context, ConstSpan<u8>(kParameters)) == TlsError::None);
    QuicRun result;
    result.first_hello = drain(client.quic, QuicLevel::Initial);
    append(result.client_bytes, result.first_hello);
    note(result.server_error, server.quic.provide(QuicLevel::Initial, span_of(result.first_hello)));
    for (int round = 0; round < 16; ++round)
    {
        usize moved = 0U;
        for (const QuicLevel level : kLevels)
        {
            const Bytes from_server = drain(server.quic, level);
            moved += from_server.size();
            if (!from_server.empty() && quic_live(client.quic))
            {
                note(result.client_error, client.quic.provide(level, span_of(from_server)));
            }
        }
        for (const QuicLevel level : kLevels)
        {
            const Bytes from_client = drain(client.quic, level);
            moved += from_client.size();
            append(result.client_bytes, from_client);
            if (!from_client.empty() && quic_live(server.quic))
            {
                note(result.server_error, server.quic.provide(level, span_of(from_client)));
            }
        }
        if (moved == 0U)
        {
            break;
        }
    }
    return result;
}
} // namespace

TEST_CASE("TLS ECH: over QUIC the hello is encrypted, a rejection retries and GREASE stays in the first datagrams",
          "[crypto][quic-handshake][ech][net-2]")
{
    TlsEchKey stale;
    TlsEchKey key;
    make_ech(stale, 1U, 1U);
    make_ech(key, 2U, 2U, kPublicName, 32U);
    const Bytes      list       = list_of({&key});
    const Bytes      stale_list = list_of({&stale});
    const TlsEchKey* keys[]     = {&key};

    Endpoint client;
    Endpoint server;
    Setup    setup;
    setup.client_ech  = span_of(list);
    setup.server_keys = ConstSpan<const TlsEchKey*>(keys);
    setup.protocols   = kGame;
    configure(client, server, setup);
    QuicRun result = quic_connect(client, server);
    REQUIRE(client.quic.state() == TlsState::Established);
    REQUIRE(server.quic.state() == TlsState::Established);
    CHECK(client.quic.ech() == TlsEch::Accepted);
    CHECK(server.quic.ech() == TlsEch::Accepted);
    CHECK(client.quic.application_protocol() == "cerid-game/1");
    CHECK_FALSE(contains(result.client_bytes, kServerName));
    const Hello accepted = read_hello(span_of(result.first_hello));
    CHECK(accepted.name == kPublicName);
    CHECK(accepted.ech);
    CHECK(accepted.config_id == 2U);

    // A stale configuration over QUIC: rejected, retried once with the server's configurations.
    Endpoint stale_client;
    Endpoint stale_server;
    setup.client_ech = span_of(stale_list);
    configure(stale_client, stale_server, setup);
    result = quic_connect(stale_client, stale_server);
    CHECK(result.client_error == TlsError::EchRejected);
    CHECK(stale_client.quic.ech() == TlsEch::Retry);
    CHECK(stale_client.quic.alert_sent().description == kEchRequired);
    CHECK(quic_crypto_error(stale_client.quic.alert_sent().description) == 0x0179U);
    CHECK(stale_server.quic.state() != TlsState::Established);
    const ConstSpan<u8> retry = stale_client.quic.ech_retry_configs();
    const Bytes         kept(retry.begin(), retry.end());
    REQUIRE(kept.size() == list.size());
    result = quic_connect(stale_client, stale_server, span_of(kept));
    CHECK(stale_client.quic.ech() == TlsEch::Accepted);
    CHECK(stale_server.quic.ech() == TlsEch::Accepted);

    // The Initial CRYPTO data of the three kinds of hello: none, GREASE and ECH (padded to a 32-byte name). Each
    // takes two 1200-byte datagrams, as a hybrid ClientHello already does.
    Endpoint plain_client;
    Endpoint plain_server;
    Setup    plain;
    plain.grease    = false;
    plain.protocols = kGame;
    configure(plain_client, plain_server, plain);
    const usize none = quic_connect(plain_client, plain_server).first_hello.size();
    Endpoint grease_client;
    Endpoint grease_server;
    Setup    grease;
    grease.protocols = kGame;
    configure(grease_client, grease_server, grease);
    const usize greased = quic_connect(grease_client, grease_server).first_hello.size();
    const usize ech     = accepted.size;
    CAPTURE(none, greased, ech);
    CHECK(none < greased);
    CHECK(none < ech);
    CHECK(ech < 2U * 1200U - 2U * 64U); // two datagrams, with room for the packet headers and tags
    CHECK(greased < 2U * 1200U - 2U * 64U);
    SUCCEED("ClientHello CRYPTO bytes, X25519MLKEM768 first: without ECH, with GREASE, with ECH");
}
