// crd-crypto -- TLS 1.3 sessions over the provider's TLS stack (NET.2; docs/design/network-foundation.md#tls-sessions
// and #tls-13-profile): pairs of sessions in one process, every group (the three hybrid groups included) and every
// signature algorithm negotiated, the key shares read off the wire and checked against the hybrid groups' layout with
// the provider interface's own primitives, HelloRetryRequest, server preference, pinned keys both ways, server names,
// alerts, bounds, close_notify, key updates against the usage limits and the update bound, a peer's user_canceled and
// close_notify during the handshake, tickets and resumption (single use, bindings, a fleet's shared key and its
// rotation, a client no longer pinned, tickets changed at rest or expired, a HelloRetryRequest), certificate
// compression (the forms a server sends and their sizes, the algorithm both ends allow, the bounded decoder, and a
// hostile server of the provider's own stack whose compressed Certificates a client refuses), adversarial bytes, every
// configuration refusal, one context serving several threads, and RFC 9850 key logging (each logged secret opening
// the records or deriving the exporter it names, apart from the provider; refused where it is compiled out).
//
// The independent peer (OpenSSL 4.0's hybrid key exchange) is tests/foundation/crypto/interop/tls_interop.py's.

#include <crd/compression/deflate.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include "tls_key_log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <openssl/aead.h> // EVP_has_aes_hardware: the test asks the provider what the preference must follow
#include <openssl/bio.h>  // a server of the provider's own stack with a hostile certificate compressor
#include <openssl/bytestring.h>
#include <openssl/evp.h>
#include <openssl/ssl.h> // SSL_SESSION_from_bytes: the provider reads back the lifetime a client keeps
#include <openssl/x509.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <thread>
#include <utility>

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

constexpr const char* kServerName = "game.cerid.test";

constexpr u8 kHandshakeFailure      = 40U;
constexpr u8 kBadCertificate        = 42U;
constexpr u8 kCertificateRequired   = 116U;
constexpr u8 kRecordChangeCipherSpec = 20U;
constexpr u8 kRecordHandshake       = 22U;
constexpr u16 kKeyShareExtension    = 51U;
constexpr u16 kSupportedGroupsExtension   = 10U;
constexpr u16 kSupportedVersionsExtension = 43U;
constexpr u16 kZstd = iana::kCertCompressionZstd;
constexpr u16 kZlib = iana::kCertCompressionZlib;

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

// A client's tickets: at most four, the oldest dropped for a fifth, each handed out once with its binding. A `careless`
// store hands out its oldest ticket whatever the binding (one the session must not trust), and `flip` changes a byte
// of the copy it hands out (a ticket corrupted at rest; the mask 0 changes nothing).
class Store final : public TlsTicketStore
{
public:
    static constexpr usize kCapacity = 4U;

    void keep(const TlsBinding& binding, ConstSpan<u8> ticket) noexcept override
    {
        if (m_count == kCapacity)
        {
            drop(0U);
        }
        m_bindings[m_count] = binding;
        m_tickets[m_count].resize(ticket.size());
        std::memcpy(m_tickets[m_count].data(), ticket.data(), ticket.size());
        ++m_count;
        ++kept;
    }

    ConstSpan<u8> take(const TlsBinding& binding) noexcept override
    {
        ++takes;
        last_binding = binding;
        secure_zero(Span<u8>(m_handed.data(), m_handed.size()));
        m_handed.clear();
        for (usize i = 0U; i < m_count; ++i)
        {
            if (careless || m_bindings[i] == binding)
            {
                m_handed = m_tickets[i];
                drop(i);
                if (flip_at < m_handed.size())
                {
                    m_handed[flip_at] ^= flip_mask;
                }
                return ConstSpan<u8>(m_handed.data(), m_handed.size());
            }
        }
        return {};
    }

    [[nodiscard]] usize        size() const { return m_count; }
    [[nodiscard]] const Bytes& ticket(usize i) const { return m_tickets[i]; }
    [[nodiscard]] TlsBinding   binding(usize i) const { return m_bindings[i]; }

    bool       careless  = false;
    usize      flip_at   = 0U;
    u8         flip_mask = 0U;
    usize      kept      = 0U;
    usize      takes     = 0U;
    TlsBinding last_binding;

private:
    void drop(usize i)
    {
        secure_zero(Span<u8>(m_tickets[i].data(), m_tickets[i].size()));
        for (usize j = i; j + 1U < m_count; ++j)
        {
            m_tickets[j]  = m_tickets[j + 1U];
            m_bindings[j] = m_bindings[j + 1U];
        }
        --m_count;
        m_tickets[m_count].clear();
    }

    Bytes      m_tickets[kCapacity];
    TlsBinding m_bindings[kCapacity];
    usize      m_count = 0U;
    Bytes      m_handed;
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
    u64                    client_key_records = 0U; // TlsConfig::key_update_records of each end
    u64                    server_key_records = 0U;
    u64                    client_max_updates = 0U; // TlsConfig::max_key_updates of each end
    u64                    server_max_updates = 0U;
    Store*                 client_tickets     = nullptr; // the client session's ticket store
    ConstSpan<u8>          ticket_key;                   // the server's TlsConfig ticket settings
    ConstSpan<u8>          previous_ticket_key;
    u32                    ticket_lifetime = 0U;
    ConstSpan<u16>         client_compression = kTlsCertificateCompression; // TlsConfig::certificate_compression
    ConstSpan<u16>         server_compression = kTlsCertificateCompression;
    StringView             credential_name    = kServerName; // the name the server's certificate carries
    crd::crypto::test::KeyLog* client_log    = nullptr;        // RFC 9850 key logs of each context (decision S19)
    crd::crypto::test::KeyLog* server_log    = nullptr;
    bool                       log_exporters = false;          // TlsConfig::key_log_exporters of both
};

// Configures both ends; the client's first flight is pending when this returns.
void setup(Endpoint& client, Endpoint& server, const Options& options)
{
    make_key(server.key, options.server_key, 0U);
    REQUIRE(server.credential.init(server.key, options.credential_name) == TlsError::None);
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
    client_config.key_update_records = options.client_key_records;
    client_config.max_key_updates    = options.client_max_updates;
    client_config.certificate_compression = options.client_compression;
    if (options.client_log != nullptr)
    {
        options.client_log->attach(client_config, options.log_exporters);
    }
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
    server_config.key_update_records = options.server_key_records;
    server_config.max_key_updates    = options.server_max_updates;
    server_config.ticket_key          = options.ticket_key;
    server_config.previous_ticket_key = options.previous_ticket_key;
    server_config.ticket_lifetime     = options.ticket_lifetime;
    server_config.certificate_compression = options.server_compression;
    if (options.server_log != nullptr)
    {
        options.server_log->attach(server_config, options.log_exporters);
    }
    if (options.server_pins_client)
    {
        server_config.signatures = kAllSignatures;
        server_config.pinned     = ConstSpan<KeyId>(&server_pin, 1U);
    }
    REQUIRE(server.context.init(server_config) == TlsError::None);
    REQUIRE(client.session.init(client.context, options.client_tickets) == TlsError::None);
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

// RFC 8701 section 2's reserved values, read independently of the provider: 0x0A0A, 0x1A1A ... 0xFAFA.
constexpr bool is_grease(u16 value)
{
    // u16 promotes to int, so widen to unsigned first and compare unsigned with unsigned (MSVC C4389 on /WX)
    const u32 bits = value;
    return (bits & 0x0F0FU) == 0x0A0AU && (bits >> 12U) == ((bits >> 4U) & 0x0FU);
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

// The first handshake message of a flight (a ClientHello, a ServerHello or a HelloRetryRequest), header included; a
// change_cipher_spec record (middlebox compatibility, RFC 9846 appendix D.4) may come first.
ConstSpan<u8> first_hello(const Bytes& flight)
{
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
    REQUIRE((message[0] == 1U || message[0] == 2U));
    return ConstSpan<u8>(message, 4U + length);
}

// Where a hello's cipher_suites (a ClientHello's list, a ServerHello's one suite) start, after the header,
// legacy_version, random and legacy_session_id (RFC 9846 section 4.1).
usize cipher_suites_at(ConstSpan<u8> hello)
{
    const usize at = 4U + 2U + 32U;
    return at + 1U + hello[at];
}

// Calls `visit(type, body)` for each extension of a hello, read with the RFC 9846 section 4.1 layout.
template <typename Visit>
void for_each_extension(ConstSpan<u8> hello, const Visit& visit)
{
    usize at = cipher_suites_at(hello);
    if (hello[0] == 1U)
    {
        at += 2U + be16(hello.data() + at);  // cipher_suites
        at += 1U + hello[at];                // legacy_compression_methods
    }
    else
    {
        at += 2U + 1U;                       // cipher_suite, legacy_compression_method
    }
    at += 2U;                                // extensions length
    while (at + 4U <= hello.size())
    {
        const u16   type = be16(hello.data() + at);
        const usize size = be16(hello.data() + at + 2U);
        REQUIRE(at + 4U + size <= hello.size());
        visit(type, hello.subspan(at + 4U, size));
        at += 4U + size;
    }
    REQUIRE(at == hello.size());
}

// The key shares of a flight's hello. A HelloRetryRequest's key_share holds only the selected group (empty bytes). A
// ClientHello's GREASE share (RFC 8701) is left to read_grease.
usize read_shares(const Bytes& flight, Share (&out)[4])
{
    const ConstSpan<u8> hello        = first_hello(flight);
    const bool          client_hello = hello[0] == 1U;
    usize               count        = 0U;
    for_each_extension(hello, [&](u16 type, ConstSpan<u8> body)
    {
        if (type != kKeyShareExtension)
        {
            return;
        }
        if (!client_hello)
        {
            const bool retry = body.size() == 2U;
            out[count++] = Share{be16(body.data()), retry ? ConstSpan<u8>() : body.subspan(4U, be16(body.data() + 2U))};
            return;
        }
        for (usize share = 2U; share + 4U <= body.size() && count < 4U;)
        {
            const u16   group = be16(body.data() + share);
            const usize bytes = be16(body.data() + share + 2U);
            if (!is_grease(group))
            {
                out[count++] = Share{group, body.subspan(share + 4U, bytes)};
            }
            share += 4U + bytes;
        }
    });
    return count;
}

// The GREASE values (RFC 8701) of a ClientHello, field by field: the first one in each list, 0 where there is none.
struct Grease
{
    u16   cipher_suite    = 0U;
    u16   group           = 0U;  // supported_groups
    u16   version         = 0U;  // supported_versions
    u16   key_share       = 0U;  // a key share's group
    usize key_share_bytes = 0U;
    u16   extensions[4]   = {};  // extension types, in hello order
    usize extension_bytes[4] = {};
    usize extension_count = 0U;
};

// The first GREASE value of a list of 16-bit values.
u16 grease_in(ConstSpan<u8> values)
{
    for (usize at = 0U; at + 2U <= values.size(); at += 2U)
    {
        if (is_grease(be16(values.data() + at)))
        {
            return be16(values.data() + at);
        }
    }
    return 0U;
}

Grease read_grease(const Bytes& flight)
{
    const ConstSpan<u8> hello = first_hello(flight);
    REQUIRE(hello[0] == 1U);
    Grease      grease;
    const usize suites   = cipher_suites_at(hello);
    grease.cipher_suite  = grease_in(hello.subspan(suites + 2U, be16(hello.data() + suites)));
    for_each_extension(hello, [&](u16 type, ConstSpan<u8> body)
    {
        if (is_grease(type))
        {
            REQUIRE(grease.extension_count < 4U);
            grease.extension_bytes[grease.extension_count] = body.size();
            grease.extensions[grease.extension_count++]    = type;
        }
        else if (type == kSupportedGroupsExtension)
        {
            grease.group = grease_in(body.subspan(2U));
        }
        else if (type == kSupportedVersionsExtension)
        {
            grease.version = grease_in(body.subspan(1U));
        }
        else if (type == kKeyShareExtension)
        {
            for (usize share = 2U; share + 4U <= body.size(); share += 4U + be16(body.data() + share + 2U))
            {
                if (grease.key_share == 0U && is_grease(be16(body.data() + share)))
                {
                    grease.key_share       = be16(body.data() + share);
                    grease.key_share_bytes = be16(body.data() + share + 2U);
                }
            }
        }
    });
    return grease;
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

// RFC 8701 (ietf.grease): every build's ClientHello carries a reserved value in its cipher suites, supported groups,
// supported versions and key shares (a one-byte share) and two reserved extensions (the first empty, the second one
// byte), chosen per connection; a server ignores them and never negotiates one. A ClientHello sent again after a
// HelloRetryRequest keeps the first one's values (RFC 9846 section 4.1.2 allows only the listed changes) and shares
// only the group asked for.
TEST_CASE("TLS: a client GREASEs every hello, a retried hello keeps its values and a server ignores them",
          "[crypto][tls][net-2]")
{
    const auto check_hello = [](const Grease& grease)
    {
        CHECK(is_grease(grease.cipher_suite));
        CHECK(is_grease(grease.group));
        CHECK(is_grease(grease.version));
        REQUIRE(grease.extension_count == 2U);
        CHECK(grease.extensions[0] != grease.extensions[1]);
        CHECK(grease.extension_bytes[0] == 0U);
        CHECK(grease.extension_bytes[1] == 1U);
    };

    constexpr usize connections = 8U;
    u16             suites[connections];
    for (usize i = 0U; i < connections; ++i)
    {
        Endpoint client;
        Endpoint server;
        setup(client, server, Options{});
        const Bytes  hello  = drain(client.session);
        const Grease grease = read_grease(hello);
        check_hello(grease);
        CHECK(is_grease(grease.key_share));
        CHECK(grease.key_share_bytes == 1U);
        suites[i] = grease.cipher_suite;
        REQUIRE(server.session.receive(ConstSpan<u8>(hello.data(), hello.size())) == TlsError::None);
        establish(client, server);
        CHECK(client.session.group() == AlgorithmId::X25519Mlkem768);
        CHECK_FALSE(server.session.alert_sent().present);
        exchange(client, server, 100U);
    }
    // Eight connections drawing the same one of sixteen values: 16^-7.
    CHECK(std::any_of(suites + 1, suites + connections, [&](u16 suite) { return suite != suites[0]; }));

    constexpr AlgorithmId client_groups[] = {AlgorithmId::Secp256r1Mlkem768, AlgorithmId::X25519Mlkem768};
    constexpr AlgorithmId server_groups[] = {AlgorithmId::X25519Mlkem768};
    Endpoint              client;
    Endpoint              server;
    Options               options;
    options.client_groups = client_groups;
    options.server_groups = server_groups;
    setup(client, server, options);
    const Bytes  hello = drain(client.session);
    const Grease first = read_grease(hello);
    REQUIRE(server.session.receive(ConstSpan<u8>(hello.data(), hello.size())) == TlsError::None);
    const Bytes request = drain(server.session);
    REQUIRE(client.session.receive(ConstSpan<u8>(request.data(), request.size())) == TlsError::None);
    const Bytes  again  = drain(client.session);
    const Grease second = read_grease(again);
    check_hello(second);
    CHECK(second.cipher_suite == first.cipher_suite);
    CHECK(second.group == first.group);
    CHECK(second.version == first.version);
    CHECK(second.extensions[0] == first.extensions[0]);
    CHECK(second.extensions[1] == first.extensions[1]);
    CHECK(second.key_share == 0U);
    Share shares[4];
    REQUIRE(read_shares(again, shares) == 1U);
    CHECK(shares[0].group == iana::kGroupX25519mlkem768);
    REQUIRE(server.session.receive(ConstSpan<u8>(again.data(), again.size())) == TlsError::None);
    establish(client, server);
    CHECK(client.session.hello_retry());
    CHECK(server.session.group() == AlgorithmId::X25519Mlkem768);
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

// A KeyUpdate's record (RFC 9846 sections 4.7.3 and 5.2): header (5), handshake header (4), request_update (1), inner
// content type (1) and the AEAD tag (16); and an application data record's overhead without its plaintext.
constexpr usize kKeyUpdateRecord   = 27U;
constexpr usize kRecordOverhead    = 22U;
constexpr u8    kUnexpectedMessage = 10U;

// Sends `count` one-byte messages, each its own record, checking the budget after every one.
void send_bytes(TlsSession& session, usize count)
{
    for (usize i = 0U; i < count; ++i)
    {
        const u8 byte = static_cast<u8>(i);
        REQUIRE(session.send(ConstSpan<u8>(&byte, 1U)) == TlsError::None);
        CHECK(session.records_under_key() <= session.key_update_records() - 2U);
    }
}

TEST_CASE("TLS: a session updates its sending key before its record budget, and its peer reads on",
          "[crypto][tls][net-2]")
{
    CHECK(tls_key_usage_limit(AlgorithmId::Aes128Gcm) == 23726566U); // floor(2^24.5)
    CHECK(tls_key_usage_limit(AlgorithmId::Aes256Gcm) == 23726566U);
    CHECK(tls_key_usage_limit(AlgorithmId::Chacha20Poly1305) == ~u64{0});
    CHECK(tls_key_usage_limit(AlgorithmId::X25519) == 0U);
    CHECK(kTlsMaxKeyUpdates == 281474976710655U); // 2^48-1
    for (const AlgorithmId aead : kAllAeads)
    {
        CAPTURE(static_cast<u32>(aead));
        const AlgorithmId only[] = {aead};
        Options           options;
        options.client_aeads       = only;
        options.client_key_records = kTlsMinKeyUpdateRecords; // one data record per key
        options.server_key_records = kTlsMinKeyUpdateRecords; // the server only receives: it never enforces its peer
        Endpoint client;
        Endpoint server;
        setup(client, server, options);
        CHECK(client.session.key_update_records() == 0U);
        establish(client, server);
        CHECK(client.session.aead() == aead);
        CHECK(client.session.key_update_records() == kTlsMinKeyUpdateRecords);
        CHECK(client.session.records_under_key() == 0U);

        send_bytes(client.session, 10U);
        CHECK(client.session.key_updates_sent() == 9U);
        CHECK(client.session.records_under_key() == 1U);
        CHECK(client.session.pending() == 10U * (1U + kRecordOverhead) + 9U * kKeyUpdateRecord);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        const Bytes received = read_all(server.session);
        REQUIRE(received.size() == 10U);
        for (usize i = 0U; i < received.size(); ++i)
        {
            CHECK(received[i] == i);
        }
        CHECK(server.session.key_updates_received() == 9U);
        CHECK(server.session.key_updates_sent() == 0U);
        CHECK(server.session.pending() == 0U);
        CHECK(client.session.key_updates_received() == 0U);
        exchange(client, server, 100U);
    }

    SECTION("a message of several records updates the key between them, and the outgoing bound counts the updates")
    {
        Options options;
        options.client_key_records = kTlsMinKeyUpdateRecords;
        Endpoint client;
        Endpoint server;
        setup(client, server, options);
        establish(client, server);
        Bytes message;
        message.resize(60000U); // four records: three key updates between them
        for (usize i = 0U; i < message.size(); ++i)
        {
            message[i] = static_cast<u8>(i * 13U + 1U);
        }
        REQUIRE(client.session.send(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
        CHECK(client.session.key_updates_sent() == 3U);
        CHECK(client.session.pending() == 60000U + 4U * kRecordOverhead + 3U * kKeyUpdateRecord);
        // 65536 - 60169 = 5367 bytes are left: a record of 5318 bytes behind its key update (5318 + 22 + 27) fits,
        // one byte more does not, and a refused message leaves the session as it was.
        const u64 updates = client.session.key_updates_sent();
        CHECK(client.session.send(ConstSpan<u8>(message.data(), 5319U)) == TlsError::Full);
        CHECK(client.session.key_updates_sent() == updates);
        REQUIRE(client.session.send(ConstSpan<u8>(message.data(), 5318U)) == TlsError::None);
        CHECK(client.session.pending() == kTlsMaxBufferedBytes);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        const Bytes received = read_all(server.session);
        REQUIRE(received.size() == 60000U + 5318U);
        CHECK(std::memcmp(received.data(), message.data(), 60000U) == 0);
        CHECK(std::memcmp(received.data() + 60000U, message.data(), 5318U) == 0);
        CHECK(server.session.key_updates_received() == 4U);
    }
    SECTION("a budget of four records keeps two data records under each key")
    {
        Options options;
        options.client_key_records = 4U;
        Endpoint client;
        Endpoint server;
        setup(client, server, options);
        establish(client, server);
        send_bytes(client.session, 7U);
        CHECK(client.session.key_updates_sent() == 3U);
        CHECK(client.session.records_under_key() == 1U);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        CHECK(read_all(server.session).size() == 7U);
    }
    SECTION("a budget above the negotiated AEAD's limit is lowered to it")
    {
        const AlgorithmId aes[] = {AlgorithmId::Aes128Gcm};
        Options           options;
        options.client_aeads       = aes;
        options.client_key_records = ~u64{0};
        Endpoint client;
        Endpoint server;
        setup(client, server, options);
        establish(client, server);
        CHECK(client.session.key_update_records() == tls_key_usage_limit(AlgorithmId::Aes128Gcm));
        CHECK(server.session.key_update_records() == tls_key_usage_limit(AlgorithmId::Aes128Gcm));
    }
}

TEST_CASE("TLS: key updates on demand, the peer's answer and one request outstanding at a time",
          "[crypto][tls][net-2]")
{
    Endpoint client;
    Endpoint server;
    setup(client, server, Options{});
    CHECK(client.session.update_keys(false) == TlsError::NotReady);
    establish(client, server);
    u8    out[16];
    usize written = 0U;

    // Not requested: the server updates its receiving key and answers nothing.
    REQUIRE(client.session.update_keys(false) == TlsError::None);
    CHECK(client.session.key_updates_sent() == 1U);
    CHECK(client.session.pending() == kKeyUpdateRecord);
    CHECK(client.session.records_under_key() == 0U);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    REQUIRE(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(written == 0U);
    CHECK(server.session.key_updates_received() == 1U);
    CHECK(server.session.pending() == 0U);

    // Requested: reading the request answers it at once.
    REQUIRE(client.session.update_keys(true) == TlsError::None);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    REQUIRE(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(server.session.key_updates_sent() == 1U);
    CHECK(server.session.pending() == kKeyUpdateRecord);
    REQUIRE(deliver(server.session, client.session) == TlsError::None);
    REQUIRE(client.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(client.session.key_updates_received() == 1U);
    CHECK(client.session.pending() == 0U); // an answer asks for nothing

    // While a request is outstanding a second one only updates the client's own key: the server answers once.
    REQUIRE(client.session.update_keys(true) == TlsError::None);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    REQUIRE(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(server.session.key_updates_sent() == 2U);
    const Bytes answer = drain(server.session); // held back: the client's request stays outstanding
    REQUIRE(client.session.update_keys(true) == TlsError::None);
    CHECK(client.session.key_updates_sent() == 4U);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    REQUIRE(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(server.session.key_updates_received() == 4U);
    CHECK(server.session.key_updates_sent() == 2U);
    CHECK(server.session.pending() == 0U);
    REQUIRE(client.session.receive(ConstSpan<u8>(answer.data(), answer.size())) == TlsError::None);
    REQUIRE(client.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(client.session.key_updates_received() == 2U);

    // The answer arrived, so the next request is one again.
    REQUIRE(client.session.update_keys(true) == TlsError::None);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    REQUIRE(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(server.session.key_updates_sent() == 3U);
    pump(client, server);
    exchange(client, server, 20000U);

    // An answer the outgoing bytes have no room for waits inside the provider, counted when queued, and goes out
    // ahead of the next record or update; send() and update_keys() count it against the bound.
    Bytes filler;
    filler.resize(60000U);
    REQUIRE(server.session.send(ConstSpan<u8>(filler.data(), filler.size())) == TlsError::None);
    REQUIRE(server.session.send(ConstSpan<u8>(filler.data(), 5400U)) == TlsError::None);
    const usize full = server.session.pending();
    CHECK(full == 65400U + 5U * kRecordOverhead);
    REQUIRE(client.session.update_keys(true) == TlsError::None);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    const u64 sent = server.session.key_updates_sent();
    REQUIRE(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(server.session.key_updates_sent() == sent + 1U);
    CHECK(server.session.pending() == full);
    const u8 ping[] = {'p'};
    CHECK(server.session.send(ConstSpan<u8>(ping, sizeof(ping))) == TlsError::Full); // 1 + 22 + 27 > 26
    CHECK(server.session.update_keys(false) == TlsError::Full);
    const Bytes data = drain(server.session);
    REQUIRE(server.session.update_keys(false) == TlsError::None);
    CHECK(server.session.pending() == 2U * kKeyUpdateRecord);
    CHECK(server.session.key_updates_sent() == sent + 2U);
    const u64 received = client.session.key_updates_received();
    REQUIRE(client.session.receive(ConstSpan<u8>(data.data(), data.size())) == TlsError::None);
    CHECK(read_all(client.session).size() == 65400U);
    REQUIRE(deliver(server.session, client.session) == TlsError::None);
    CHECK(read_all(client.session).empty());
    CHECK(client.session.key_updates_received() == received + 2U);
    exchange(client, server, 10U);

    // No room for an update in the outgoing bytes: Full, nothing queued.
    REQUIRE(client.session.send(ConstSpan<u8>(filler.data(), filler.size())) == TlsError::None);
    REQUIRE(client.session.send(ConstSpan<u8>(filler.data(), 5400U)) == TlsError::None);
    const usize held = client.session.pending();
    CHECK(client.session.update_keys(false) == TlsError::Full);
    CHECK(client.session.pending() == held);
    pump(client, server);
    REQUIRE(client.session.close() == TlsError::None);
    CHECK(client.session.update_keys(false) == TlsError::Closed);
}

TEST_CASE("TLS: a session never passes its key update bound and closes cleanly at it", "[crypto][tls][net-2]")
{
    u8    out[16];
    usize written = 0U;
    SECTION("updates of its own")
    {
        Options options;
        options.client_max_updates = 3U;
        Endpoint client;
        Endpoint server;
        setup(client, server, options);
        establish(client, server);
        REQUIRE(client.session.update_keys(false) == TlsError::None);
        REQUIRE(client.session.update_keys(true) == TlsError::None);
        CHECK(client.session.update_keys(false) == TlsError::Closed);
        CHECK(client.session.key_updates_sent() == 2U);
        CHECK(client.session.state() == TlsState::Closed);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        CHECK(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Closed);
        CHECK(server.session.key_updates_received() == 2U);
        CHECK(server.session.alert_received().description == 0U); // close_notify
    }
    SECTION("the record budget reaching the bound closes before a byte of the message is sent")
    {
        Options options;
        options.client_key_records = kTlsMinKeyUpdateRecords;
        options.client_max_updates = 2U;
        Endpoint client;
        Endpoint server;
        setup(client, server, options);
        establish(client, server);
        send_bytes(client.session, 2U);
        CHECK(client.session.key_updates_sent() == 1U);
        const u8 last[] = {'x', 'y'};
        CHECK(client.session.send(ConstSpan<u8>(last, 1U)) == TlsError::Closed);
        CHECK(client.session.key_updates_sent() == 1U);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        for (int i = 0; i < 2; ++i)
        {
            REQUIRE(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
            CHECK(written == 1U);
        }
        CHECK(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Closed);
        CHECK(server.session.key_updates_received() == 1U);
    }
    SECTION("a message whose key updates would reach the bound is not sent in part")
    {
        Options options;
        options.client_key_records = kTlsMinKeyUpdateRecords;
        options.client_max_updates = 2U;
        Endpoint client;
        Endpoint server;
        setup(client, server, options);
        establish(client, server);
        Bytes message;
        message.resize(40000U); // three records, two key updates
        CHECK(client.session.send(ConstSpan<u8>(message.data(), message.size())) == TlsError::Closed);
        CHECK(client.session.key_updates_sent() == 0U);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        CHECK(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Closed);
        CHECK(written == 0U);
    }
    SECTION("the peer's request takes the last update, then the session closes")
    {
        Options options;
        options.client_max_updates = 2U;
        Endpoint client;
        Endpoint server;
        setup(client, server, options);
        establish(client, server);
        REQUIRE(client.session.update_keys(false) == TlsError::None);
        REQUIRE(server.session.update_keys(true) == TlsError::None);
        const u8 data[] = {'d'};
        REQUIRE(server.session.send(ConstSpan<u8>(data, sizeof(data))) == TlsError::None);
        REQUIRE(deliver(server.session, client.session) == TlsError::None);
        REQUIRE(client.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
        CHECK(written == 1U); // the data read with the request; the session closed after answering
        CHECK(client.session.key_updates_sent() == 2U);
        CHECK(client.session.state() == TlsState::Closed);
        CHECK(client.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Closed);
        REQUIRE(deliver(client.session, server.session) == TlsError::None);
        CHECK(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Closed);
        CHECK(server.session.key_updates_received() == 2U);
    }
}

TEST_CASE("TLS: a peer's user_canceled is ignored and its close_notify ends the handshake cleanly",
          "[crypto][tls][net-2]")
{
    // Before the first flight both alerts travel in plaintext records (RFC 9846 sections 5.1 and 6.1).
    const u8 user_canceled[] = {21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x01U, kTlsAlertUserCanceled};
    const u8 close_notify[]  = {21U, 0x03U, 0x03U, 0x00U, 0x02U, 0x01U, 0x00U};
    SECTION("a server: user_canceled alone changes nothing, and the handshake goes on")
    {
        Endpoint client;
        Endpoint server;
        setup(client, server, Options{});
        REQUIRE(server.session.receive(ConstSpan<u8>(user_canceled, sizeof(user_canceled))) == TlsError::None);
        CHECK(server.session.state() == TlsState::Handshaking);
        CHECK(server.session.alert_received().description == kTlsAlertUserCanceled);
        CHECK(server.session.pending() == 0U);
        establish(client, server);
        exchange(client, server, 10U);
    }
    SECTION("a server: user_canceled, then close_notify")
    {
        Endpoint client;
        Endpoint server;
        setup(client, server, Options{});
        REQUIRE(server.session.receive(ConstSpan<u8>(user_canceled, sizeof(user_canceled))) == TlsError::None);
        CHECK(server.session.receive(ConstSpan<u8>(close_notify, sizeof(close_notify))) == TlsError::Closed);
        CHECK(server.session.state() == TlsState::Closed);
        CHECK_FALSE(server.session.alert_sent().present);
        CHECK(server.session.pending() == 0U);
        CHECK(server.session.close() == TlsError::Closed);
        CHECK(server.session.pending() == 0U);
        CHECK(server.session.update_keys(false) == TlsError::Closed);
        CHECK(server.session.receive(ConstSpan<u8>(close_notify, sizeof(close_notify))) == TlsError::Closed);
    }
    SECTION("a client that sent its ClientHello: both alerts in one delivery")
    {
        Endpoint client;
        Endpoint server;
        setup(client, server, Options{});
        (void)drain(client.session);
        u8 both[sizeof(user_canceled) + sizeof(close_notify)];
        std::memcpy(both, user_canceled, sizeof(user_canceled));
        std::memcpy(both + sizeof(user_canceled), close_notify, sizeof(close_notify));
        CHECK(client.session.receive(ConstSpan<u8>(both, sizeof(both))) == TlsError::Closed);
        CHECK(client.session.state() == TlsState::Closed);
        CHECK(client.session.alert_received().description == kTlsAlertUserCanceled);
        CHECK(client.session.pending() == 0U);
    }
    SECTION("more warnings in a row than the provider tolerates fail with unexpected_message")
    {
        Endpoint client;
        Endpoint server;
        setup(client, server, Options{});
        for (int i = 0; i < 4; ++i)
        {
            REQUIRE(server.session.receive(ConstSpan<u8>(user_canceled, sizeof(user_canceled))) == TlsError::None);
        }
        CHECK(server.session.receive(ConstSpan<u8>(user_canceled, sizeof(user_canceled))) == TlsError::Failed);
        CHECK(server.session.alert_sent().description == kUnexpectedMessage);
    }
}

namespace
{
constexpr u16 kPreSharedKeyExtension  = 41U;
constexpr u16 kPskKeyExchangeModes    = 45U;
constexpr u8  kPskDheKe               = 1U;
constexpr usize kTicketNameBytes      = 16U;
constexpr usize kTicketOverheadBytes  = 16U + 32U + 16U; // the key's name, the salt and the AEAD tag

void fill_key(u8 (&key)[kTlsTicketKeyBytes], u8 seed)
{
    for (usize i = 0U; i < sizeof(key); ++i)
    {
        key[i] = static_cast<u8>(seed + i * 13U);
    }
}

bool same_bytes(const Bytes& a, const Bytes& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// A ticket key's public name, restated from the tickets' format (tls.cpp): HKDF-SHA256 of the key with an empty salt,
// expanded under "cerid tls 1.3 ticket name".
void ticket_name(ConstSpan<u8> key, u8 (&name)[kTicketNameBytes])
{
    constexpr char info[] = "cerid tls 1.3 ticket name";
    u8             prk[32];
    REQUIRE(hkdf_extract(AlgorithmId::HkdfSha256, {}, key, Span<u8>(prk, sizeof(prk))) == CryptoError::None);
    REQUIRE(hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk, sizeof(prk)),
                        ConstSpan<u8>(reinterpret_cast<const u8*>(info), sizeof(info) - 1U),
                        Span<u8>(name, sizeof(name))) == CryptoError::None);
}

// Where `name` starts in `bytes`; bytes.size() when it does not occur.
usize find_name(const Bytes& bytes, const u8 (&name)[kTicketNameBytes])
{
    for (usize at = 0U; at + sizeof(name) <= bytes.size(); ++at)
    {
        if (std::memcmp(bytes.data() + at, name, sizeof(name)) == 0)
        {
            return at;
        }
    }
    return bytes.size();
}

// The length of the DER OCTET STRING whose contents start at `at` (short or two-byte long form); 0 for neither.
usize der_length_before(const Bytes& bytes, usize at)
{
    if (at >= 4U && at <= bytes.size() && bytes[at - 4U] == 0x04U && bytes[at - 3U] == 0x82U)
    {
        return be16(bytes.data() + at - 2U);
    }
    if (at >= 3U && at <= bytes.size() && bytes[at - 3U] == 0x04U && bytes[at - 2U] == 0x81U)
    {
        return bytes[at - 1U];
    }
    return 0U;
}

// The flights of one connection until both ends go quiet: the even ones the client's, the odd ones the server's (empty
// when that end had nothing to send). Returns how many were run.
usize run_flights(Endpoint& client, Endpoint& server, Bytes (&flights)[8])
{
    usize count = 0U;
    while (count + 2U <= 8U && (client.session.pending() > 0U || server.session.pending() > 0U))
    {
        flights[count] = drain(client.session);
        if (!flights[count].empty() && server.session.state() != TlsState::Failed)
        {
            (void)server.session.receive(ConstSpan<u8>(flights[count].data(), flights[count].size()));
        }
        flights[count + 1U] = drain(server.session);
        if (!flights[count + 1U].empty() && client.session.state() != TlsState::Failed)
        {
            (void)client.session.receive(ConstSpan<u8>(flights[count + 1U].data(), flights[count + 1U].size()));
        }
        count += 2U;
    }
    REQUIRE(client.session.pending() == 0U);
    REQUIRE(server.session.pending() == 0U);
    return count;
}

// The client reads what arrived after its handshake: the server's NewSessionTicket, into its store when it has one.
void read_tickets(TlsSession& client)
{
    u8    out[16];
    usize written = 1U;
    REQUIRE(client.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    REQUIRE(written == 0U);
}

// A new connection over the same contexts, the client taking a ticket from `store` when it holds one.
void reconnect(Endpoint& client, Endpoint& server, Store* store)
{
    REQUIRE(client.session.init(client.context, store) == TlsError::None);
    REQUIRE(server.session.init(server.context) == TlsError::None);
}

// Connects again and reads the new ticket; whether both ends resumed (they must agree).
bool resume(Endpoint& client, Endpoint& server, Store& store)
{
    reconnect(client, server, &store);
    establish(client, server);
    CHECK(client.session.resumed() == server.session.resumed());
    read_tickets(client.session);
    return client.session.resumed();
}

// A hello's pre_shared_key and psk_key_exchange_modes (RFC 9846 sections 4.2.9 and 4.2.11): a ClientHello's
// identities (the first one kept) and whether pre_shared_key is its last extension, a ServerHello's selected identity.
struct Psk
{
    bool          offered    = false;
    bool          last       = false;
    usize         identities = 0U;
    ConstSpan<u8> identity;
    ConstSpan<u8> modes;
    bool          selected          = false;
    u16           selected_identity = 0U;
};

Psk read_psk(const Bytes& flight)
{
    const ConstSpan<u8> hello        = first_hello(flight);
    const bool          client_hello = hello[0] == 1U;
    Psk                 psk;
    u16                 last_type = 0U;
    for_each_extension(hello, [&](u16 type, ConstSpan<u8> body)
    {
        last_type = type;
        if (type == kPskKeyExchangeModes)
        {
            psk.modes = body;
        }
        else if (type == kPreSharedKeyExtension && client_hello)
        {
            psk.offered       = true;
            const usize end   = 2U + be16(body.data());
            for (usize at = 2U; at + 2U <= end;)
            {
                const usize size = be16(body.data() + at);
                if (psk.identities++ == 0U)
                {
                    psk.identity = body.subspan(at + 2U, size);
                }
                at += 2U + size + 4U; // the identity, then its obfuscated_ticket_age
            }
        }
        else if (type == kPreSharedKeyExtension)
        {
            REQUIRE(body.size() == 2U);
            psk.selected          = true;
            psk.selected_identity = be16(body.data());
        }
    });
    psk.last = psk.offered && last_type == kPreSharedKeyExtension;
    return psk;
}

// A client context like setup()'s, pinning `pins` and accepting `signatures`, with `change` applied.
template <typename Change>
void client_context(TlsContext& context, ConstSpan<KeyId> pins, const Change& change)
{
    TlsConfig config;
    config.role        = TlsRole::Client;
    config.groups      = kAllGroups;
    config.signatures  = kAllSignatures;
    config.aeads       = kAllAeads;
    config.pinned      = pins;
    config.server_name = kServerName;
    change(config);
    REQUIRE(context.init(config) == TlsError::None);
}

// A server context like setup()'s for `credential`, with `change` applied.
template <typename Change>
void server_context(TlsContext& context, const TlsCredential& credential, const Change& change)
{
    TlsConfig config;
    config.role        = TlsRole::Server;
    config.groups      = kAllGroups;
    config.aeads       = kAllAeads;
    config.credential  = &credential;
    config.server_name = kServerName;
    change(config);
    REQUIRE(context.init(config) == TlsError::None);
}
} // namespace

// Decision S9 and the TLS 1.3 profile's tickets: a server issues one ticket with every connection; a client keeps them
// only in its store and offers each once; resumption is psk_dhe_ke with a fresh hybrid share and takes the same round
// trips as a full handshake, without the server's certificate.
TEST_CASE("TLS: a client resumes once with its server's ticket, over a fresh hybrid share and without the certificate",
          "[crypto][tls][net-2]")
{
    u8 key[kTlsTicketKeyBytes];
    fill_key(key, 0x11U);
    Store    store;
    Endpoint client;
    Endpoint server;
    Options  options;
    options.server_key     = AlgorithmId::MlDsa65;
    options.client_tickets = &store;
    options.ticket_key     = key;
    setup(client, server, options);
    CHECK(store.takes == 1U); // the empty store was asked, under the context's binding
    const TlsBinding binding = store.last_binding;

    Bytes       full[8];
    const usize full_count = run_flights(client, server, full);
    REQUIRE(client.session.state() == TlsState::Established);
    REQUIRE(server.session.state() == TlsState::Established);
    CHECK_FALSE(client.session.resumed());
    CHECK_FALSE(server.session.resumed());
    // ClientHello; ServerHello to Finished; the client's Finished; the NewSessionTicket, the one record under the
    // server's key so far.
    CHECK(full_count == 4U);
    CHECK_FALSE(full[2].empty());
    CHECK_FALSE(full[3].empty());
    CHECK(server.session.records_under_key() == 1U);
    const Psk first_offer = read_psk(full[0]);
    CHECK_FALSE(first_offer.offered);
    REQUIRE(first_offer.modes.size() == 2U); // psk_dhe_ke alone: a server may send tickets, never psk_ke
    CHECK(first_offer.modes[0] == 1U);
    CHECK(first_offer.modes[1] == kPskDheKe);
    CHECK(store.size() == 0U);
    read_tickets(client.session);
    REQUIRE(store.size() == 1U);
    CHECK(store.binding(0U) == binding);
    const Bytes first_ticket = store.ticket(0U);

    reconnect(client, server, &store);
    CHECK(store.size() == 0U);
    CHECK(store.takes == 2U);
    Bytes       again[8];
    const usize count = run_flights(client, server, again);
    REQUIRE(client.session.state() == TlsState::Established);
    REQUIRE(server.session.state() == TlsState::Established);
    CHECK(client.session.resumed());
    CHECK(server.session.resumed());
    CHECK(count == full_count); // the same round trips
    // No Certificate and CertificateVerify: an ML-DSA-65 key (1,952 bytes) and two signatures (3,309 bytes each).
    CHECK(again[1].size() + 8000U < full[1].size());
    CHECK(client.session.group() == AlgorithmId::X25519Mlkem768);
    CHECK(server.session.group() == AlgorithmId::X25519Mlkem768);
    CHECK(client.session.peer_key() == server.credential.key_id());
    CHECK(client.session.peer_algorithm() == AlgorithmId::MlDsa65);

    // On the wire: one identity, pre_shared_key last, the identity led by the ticket key's name and sealed with the
    // overhead of the format, and a fresh hybrid share each way.
    const Psk offer = read_psk(again[0]);
    CHECK(offer.offered);
    CHECK(offer.last);
    CHECK(offer.identities == 1U);
    u8 name[kTicketNameBytes];
    ticket_name(ConstSpan<u8>(key), name);
    REQUIRE(offer.identity.size() > kTicketOverheadBytes);
    CHECK(std::memcmp(offer.identity.data(), name, sizeof(name)) == 0);
    Share shares[4];
    REQUIRE(read_shares(again[0], shares) >= 1U);
    CHECK(shares[0].group == iana::kGroupX25519mlkem768);
    check_share(AlgorithmId::X25519Mlkem768, shares[0].bytes, true);
    const Psk accepted = read_psk(again[1]);
    CHECK(accepted.selected);
    CHECK(accepted.selected_identity == 0U);
    REQUIRE(read_shares(again[1], shares) == 1U);
    CHECK(shares[0].group == iana::kGroupX25519mlkem768);
    check_share(AlgorithmId::X25519Mlkem768, shares[0].bytes, false);

    // A resumed connection brings a new ticket, sealed under a salt of its own; the one used is gone.
    read_tickets(client.session);
    REQUIRE(store.size() == 1U);
    CHECK_FALSE(same_bytes(store.ticket(0U), first_ticket));
    const usize first_at = find_name(first_ticket, name);
    const usize new_at   = find_name(store.ticket(0U), name);
    REQUIRE(first_at + kTicketOverheadBytes < first_ticket.size());
    REQUIRE(new_at + kTicketOverheadBytes < store.ticket(0U).size());
    CHECK(std::memcmp(first_ticket.data() + first_at + kTicketNameBytes,
                      store.ticket(0U).data() + new_at + kTicketNameBytes, 32U) != 0);
    exchange(client, server, 100U);

    // Without a store a client offers nothing and ignores the server's ticket.
    reconnect(client, server, nullptr);
    Bytes plain[8];
    (void)run_flights(client, server, plain);
    CHECK_FALSE(read_psk(plain[0]).offered);
    CHECK_FALSE(client.session.resumed());
    read_tickets(client.session);
    CHECK(store.size() == 1U);
    CHECK(store.takes == 2U);
    CHECK(store.kept == 2U);
}

TEST_CASE("TLS: a ticket resumes only under the binding that received it", "[crypto][tls][net-2]")
{
    Store    store;
    Endpoint client;
    Endpoint server;
    Options  options;
    options.client_tickets = &store;
    setup(client, server, options);
    establish(client, server);
    read_tickets(client.session);
    REQUIRE(store.size() == 1U);
    const KeyId pin = server.credential.key_id();

    // Offers what the client context would offer: its first flight's pre_shared_key.
    const auto offers = [&](TlsContext& context) {
        TlsSession session;
        REQUIRE(session.init(context, &store) == TlsError::None);
        return read_psk(drain(session)).offered;
    };
    SECTION("other pins, other accepted algorithms, an own credential or another name: no ticket of this binding")
    {
        KeyId other = pin;
        other.bytes[0] ^= 1U;
        const KeyId both[] = {other, pin};
        TlsContext  more_pins;
        client_context(more_pins, ConstSpan<KeyId>(both), [](TlsConfig&) {});
        CHECK_FALSE(offers(more_pins));
        constexpr AlgorithmId ed25519[] = {AlgorithmId::Ed25519};
        TlsContext            fewer_algorithms;
        client_context(fewer_algorithms, ConstSpan<KeyId>(&pin, 1U), [&](TlsConfig& c) { c.signatures = ed25519; });
        CHECK_FALSE(offers(fewer_algorithms));
        SigningKey    own_key;
        TlsCredential own;
        make_key(own_key, AlgorithmId::Ed25519, 9U);
        REQUIRE(own.init(own_key, "player.cerid.test") == TlsError::None);
        TlsContext with_credential;
        client_context(with_credential, ConstSpan<KeyId>(&pin, 1U), [&](TlsConfig& c) { c.credential = &own; });
        CHECK_FALSE(offers(with_credential));
        TlsContext other_name;
        client_context(other_name, ConstSpan<KeyId>(&pin, 1U),
                       [](TlsConfig& c) { c.server_name = "lobby.cerid.test"; });
        CHECK_FALSE(offers(other_name));
        CHECK(store.size() == 1U); // still there for its own binding
        CHECK(resume(client, server, store));
    }
    SECTION("the server name binds without case, as names match")
    {
        TlsContext upper;
        client_context(upper, ConstSpan<KeyId>(&pin, 1U), [](TlsConfig& c) { c.server_name = "GAME.Cerid.TEST"; });
        REQUIRE(client.session.init(upper, &store) == TlsError::None);
        REQUIRE(server.session.init(server.context) == TlsError::None);
        establish(client, server);
        CHECK(client.session.resumed());
        CHECK(server.session.resumed());
    }
    SECTION("a careless store's ticket of another binding is dropped, not offered")
    {
        store.careless = true;
        KeyId other    = pin;
        other.bytes[1] ^= 1U;
        const KeyId both[] = {pin, other};
        TlsContext  more_pins;
        client_context(more_pins, ConstSpan<KeyId>(both), [](TlsConfig&) {});
        CHECK_FALSE(offers(more_pins));
        CHECK(store.size() == 0U);
    }
    SECTION("a ticket that does not parse is dropped and the handshake runs in full")
    {
        store.flip_at   = 0U; // the DER SEQUENCE tag of the serialized session
        store.flip_mask = 0xFFU;
        reconnect(client, server, &store);
        Bytes flights[8];
        (void)run_flights(client, server, flights);
        CHECK_FALSE(read_psk(flights[0]).offered);
        CHECK(client.session.state() == TlsState::Established);
        CHECK_FALSE(client.session.resumed());
    }
}

TEST_CASE("TLS: a server resumes the tickets of its fleet and of its last rotation, and no other",
          "[crypto][tls][net-2]")
{
    u8 key_a[kTlsTicketKeyBytes];
    u8 key_b[kTlsTicketKeyBytes];
    u8 key_c[kTlsTicketKeyBytes];
    fill_key(key_a, 0x21U);
    fill_key(key_b, 0x52U);
    fill_key(key_c, 0x83U);
    Store    store;
    Endpoint client;
    Endpoint server;
    Options  options;
    options.client_tickets = &store;
    options.ticket_key     = key_a;
    setup(client, server, options);
    establish(client, server);
    read_tickets(client.session);
    const auto keys = [&](ConstSpan<u8> current, ConstSpan<u8> previous) {
        server_context(server.context, server.credential, [&](TlsConfig& c) {
            c.ticket_key          = current;
            c.previous_ticket_key = previous;
        });
    };
    u8 name[kTicketNameBytes];

    keys(key_a, {}); // another server of the fleet: a context of its own, the same key
    CHECK(resume(client, server, store));
    keys(key_b, key_a); // rotated: the ticket under A opens, the new one is sealed under B
    CHECK(resume(client, server, store));
    ticket_name(ConstSpan<u8>(key_b), name);
    CHECK(find_name(store.ticket(0U), name) < store.ticket(0U).size());
    keys(key_c, key_b);
    CHECK(resume(client, server, store));
    keys(key_a, {}); // a key that sealed none of the tickets held: a full handshake that authenticates again
    CHECK_FALSE(resume(client, server, store));
    CHECK(client.session.peer_key() == server.credential.key_id());
    CHECK(resume(client, server, store));

    // A context that draws its own key opens only its own tickets.
    keys({}, {});
    CHECK_FALSE(resume(client, server, store));
    CHECK(resume(client, server, store));
    keys({}, {});
    CHECK_FALSE(resume(client, server, store));

    // Another credential under the same name and ticket key: its binding differs, so it authenticates in full.
    Endpoint other;
    make_key(other.key, AlgorithmId::MlDsa44, 3U);
    REQUIRE(other.credential.init(other.key, kServerName) == TlsError::None);
    const KeyId both[] = {server.credential.key_id(), other.credential.key_id()};
    client_context(client.context, ConstSpan<KeyId>(both), [](TlsConfig&) {});
    server_context(server.context, server.credential, [&](TlsConfig& c) { c.ticket_key = key_a; });
    CHECK_FALSE(resume(client, server, store));
    CHECK(resume(client, server, store));
    server_context(server.context, other.credential, [&](TlsConfig& c) { c.ticket_key = key_a; });
    CHECK_FALSE(resume(client, server, store));
    CHECK(client.session.peer_key() == other.credential.key_id());
    CHECK(client.session.peer_algorithm() == AlgorithmId::MlDsa44);
}

TEST_CASE("TLS: a resumed server knows its pinned client, and a client no longer pinned cannot resume",
          "[crypto][tls][net-2]")
{
    u8 key[kTlsTicketKeyBytes];
    fill_key(key, 0x35U);
    Store    store;
    Endpoint client;
    Endpoint server;
    Options  options;
    options.client_key         = AlgorithmId::EcdsaSecp256r1Sha256;
    options.server_pins_client = true;
    options.client_tickets     = &store;
    options.ticket_key         = key;
    setup(client, server, options);
    establish(client, server);
    read_tickets(client.session);
    CHECK(resume(client, server, store));
    CHECK(server.session.peer_key() == client.credential.key_id());
    CHECK(server.session.peer_algorithm() == AlgorithmId::EcdsaSecp256r1Sha256);
    CHECK(client.session.peer_key() == server.credential.key_id());

    // The server stops pinning the client's key: the ticket no longer opens under its binding, and the full handshake
    // refuses the key without saying whether it is known.
    KeyId other = client.credential.key_id();
    other.bytes[2] ^= 1U;
    server_context(server.context, server.credential, [&](TlsConfig& c) {
        c.signatures = kAllSignatures;
        c.pinned     = ConstSpan<KeyId>(&other, 1U);
        c.ticket_key = ConstSpan<u8>(key);
    });
    reconnect(client, server, &store);
    Bytes flights[8];
    (void)run_flights(client, server, flights);
    CHECK(read_psk(flights[0]).offered);
    CHECK_FALSE(read_psk(flights[1]).selected);
    CHECK(server.session.state() == TlsState::Failed);
    CHECK(server.session.alert_sent().description == kTlsAlertGeneralError);
    CHECK_FALSE(server.session.resumed());
    CHECK_FALSE(client.session.resumed());
    expect_failed_read(client.session, kTlsAlertGeneralError);
}

TEST_CASE("TLS: a ticket changed at rest or past its lifetime runs a full handshake", "[crypto][tls][net-2]")
{
    u8 key[kTlsTicketKeyBytes];
    fill_key(key, 0x47U);
    u8 name[kTicketNameBytes];
    ticket_name(ConstSpan<u8>(key), name);
    Store    store;
    Endpoint client;
    Endpoint server;
    Options  options;
    options.client_tickets = &store;
    options.ticket_key     = key;
    setup(client, server, options);
    establish(client, server);
    read_tickets(client.session);

    SECTION("a byte of the key's name, the salt, the ciphertext or the tag")
    {
        // The serialized session holds the ticket as a DER OCTET STRING, whose length a clean resumption's identity
        // confirms.
        const usize at = find_name(store.ticket(0U), name);
        const usize length = der_length_before(store.ticket(0U), at);
        REQUIRE(length > kTicketOverheadBytes);
        reconnect(client, server, &store);
        Bytes flights[8];
        (void)run_flights(client, server, flights);
        REQUIRE(client.session.resumed());
        REQUIRE(read_psk(flights[0]).identity.size() == length);
        read_tickets(client.session);
        const usize offsets[] = {0U, kTicketNameBytes + 5U, kTicketNameBytes + 32U + 3U, length - 1U};
        for (const usize offset : offsets)
        {
            CAPTURE(offset);
            const usize ticket_at = find_name(store.ticket(0U), name);
            const usize size      = der_length_before(store.ticket(0U), ticket_at);
            REQUIRE(ticket_at + size <= store.ticket(0U).size());
            store.flip_at   = ticket_at + (offset == length - 1U ? size - 1U : offset);
            store.flip_mask = 0x40U;
            reconnect(client, server, &store);
            (void)run_flights(client, server, flights);
            CHECK(read_psk(flights[0]).offered);
            CHECK_FALSE(read_psk(flights[1]).selected);
            CHECK(client.session.state() == TlsState::Established);
            CHECK(server.session.state() == TlsState::Established);
            CHECK_FALSE(server.session.resumed());
            read_tickets(client.session);
        }
        store.flip_mask = 0U;
        CHECK(resume(client, server, store));
    }
    SECTION("a ticket past its lifetime is not offered")
    {
        // A resumption keeps the longer of the ticket's remaining lifetime and the configured one, so a new key
        // forces the full handshake whose ticket lives one second.
        u8 other_key[kTlsTicketKeyBytes];
        fill_key(other_key, 0x99U);
        server_context(server.context, server.credential, [&](TlsConfig& c) {
            c.ticket_key      = ConstSpan<u8>(other_key);
            c.ticket_lifetime = 1U;
        });
        CHECK_FALSE(resume(client, server, store));
        REQUIRE(store.size() == 1U);
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        reconnect(client, server, &store);
        Bytes flights[8];
        (void)run_flights(client, server, flights);
        CHECK_FALSE(read_psk(flights[0]).offered);
        CHECK(client.session.state() == TlsState::Established);
        CHECK_FALSE(client.session.resumed());
    }
}

TEST_CASE("TLS: a client keeps a ticket for the lifetime its server states", "[crypto][tls][net-2]")
{
    // The lifetime the provider reads back from the session in the store's ticket record (its version, no early data
    // and no transport parameters over records, then the session); up to a second has passed since the ticket came.
    const auto kept_lifetime = [](const Bytes& ticket)
    {
        REQUIRE(ticket.size() > 7U);
        CHECK(ticket[0] == 1U);
        CHECK(std::all_of(ticket.data() + 1, ticket.data() + 7, [](u8 byte) { return byte == 0U; }));
        const bssl::UniquePtr<SSL_CTX>     context(SSL_CTX_new(TLS_method()));
        const bssl::UniquePtr<SSL_SESSION> session(
            SSL_SESSION_from_bytes(ticket.data() + 7U, ticket.size() - 7U, context.get()));
        REQUIRE(session);
        return SSL_SESSION_get_timeout(session.get());
    };
    const u32 lifetimes[] = {0U, 60U, kTlsMaxTicketLifetime};
    for (const u32 lifetime : lifetimes)
    {
        CAPTURE(lifetime);
        const u32 stated = lifetime != 0U ? lifetime : kTlsDefaultTicketLifetime;
        Store     store;
        Endpoint  client;
        Endpoint  server;
        Options   options;
        options.client_tickets  = &store;
        options.ticket_lifetime = lifetime;
        setup(client, server, options);
        establish(client, server);
        read_tickets(client.session);
        REQUIRE(store.size() == 1U);
        const u32 kept = kept_lifetime(store.ticket(0U));
        CHECK(kept <= stated);
        CHECK(kept + 1U >= stated);
    }
}

TEST_CASE("TLS: a resumption keeps the server's hybrid preference through a HelloRetryRequest", "[crypto][tls][net-2]")
{
    constexpr AlgorithmId client_groups[] = {AlgorithmId::Secp256r1Mlkem768, AlgorithmId::X25519Mlkem768};
    constexpr AlgorithmId server_groups[] = {AlgorithmId::X25519Mlkem768};
    Store                 store;
    Endpoint              client;
    Endpoint              server;
    Options               options;
    options.client_groups  = client_groups;
    options.server_groups  = server_groups;
    options.client_tickets = &store;
    setup(client, server, options);
    establish(client, server);
    read_tickets(client.session);
    reconnect(client, server, &store);
    Bytes       flights[8];
    const usize count = run_flights(client, server, flights);
    CHECK(count == 6U); // the retry adds its round trip
    REQUIRE(client.session.state() == TlsState::Established);
    CHECK(client.session.resumed());
    CHECK(server.session.resumed());
    CHECK(client.session.hello_retry());
    CHECK(client.session.group() == AlgorithmId::X25519Mlkem768);
    CHECK(read_psk(flights[0]).offered);
    CHECK(read_psk(flights[2]).offered); // the retried ClientHello offers the ticket again, with a new binder
    CHECK(read_psk(flights[3]).selected);
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
    // The record sequence number is the AEAD nonce (tls_key_update.spthy leaves in-epoch order to this test): a genuine
    // record replayed, delivered out of order, or replayed from the epoch before a KeyUpdate fails to open.
    SECTION("an established session fed genuine records replayed, reordered or from an earlier key")
    {
        Endpoint client;
        Endpoint server;
        setup(client, server, Options{});
        establish(client, server);
        const u8 first[] = {'o', 'n', 'e'};
        const u8 second[] = {'t', 'w', 'o'};
        REQUIRE(client.session.send(ConstSpan<u8>(first, sizeof(first))) == TlsError::None);
        const Bytes one = drain(client.session);
        REQUIRE(client.session.send(ConstSpan<u8>(second, sizeof(second))) == TlsError::None);
        const Bytes two = drain(client.session);
        const auto rejects = [&server](const Bytes& record)
        {
            REQUIRE(server.session.receive(ConstSpan<u8>(record.data(), record.size())) == TlsError::None);
            u8 out[16];
            usize written = 0U;
            CHECK(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Failed);
            CHECK(written == 0U);
            CHECK(server.session.state() == TlsState::Failed);
            CHECK(server.session.alert_sent().description == 20U); // bad_record_mac
        };
        const auto reads = [&server](const Bytes& record, usize expected)
        {
            REQUIRE(server.session.receive(ConstSpan<u8>(record.data(), record.size())) == TlsError::None);
            u8 out[16];
            usize written = 0U;
            REQUIRE(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
            CHECK(written == expected);
        };
        SECTION("replayed")
        {
            reads(one, sizeof(first));
            rejects(one);
        }
        SECTION("reordered")
        {
            rejects(two);
        }
        SECTION("from the key before an update")
        {
            reads(one, sizeof(first));
            reads(two, sizeof(second));
            REQUIRE(client.session.update_keys(false) == TlsError::None);
            reads(drain(client.session), 0U);
            CHECK(server.session.key_updates_received() == 1U);
            rejects(two);
        }
    }
}

namespace
{
#if CRD_ENABLE_ASSERTS
std::atomic<int> g_failed_asserts{0};

int count_assert(const char* /*report*/)
{
    g_failed_asserts.fetch_add(1);
    return 0;
}

// Counts failed assertions instead of reporting them while it lives; a REQUIRE that throws still removes it.
struct CountedAsserts
{
    CountedAsserts() noexcept
    {
        g_failed_asserts.store(0);
        crd::set_assert_platform_handler(&count_assert);
    }
    ~CountedAsserts() { crd::set_assert_platform_handler(nullptr); }
    CountedAsserts(const CountedAsserts&)            = delete;
    CountedAsserts& operator=(const CountedAsserts&) = delete;
};
#endif

// A failed session, then records, key updates, a partial take and empty reads and takes on a fresh pair.
void records_and_failure()
{
    u8    out[16];
    usize written = 0U;
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
        CHECK(server.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::Failed);
    }
    Options options;
    options.client_key_records = kTlsMinKeyUpdateRecords;
    Endpoint client;
    Endpoint server;
    setup(client, server, options);
    establish(client, server);
    exchange(client, server, 40000U); // three records, two of them after a key update
    send_bytes(client.session, 10U);  // and a key update before each of these
    CHECK(client.session.key_updates_sent() == 12U);

    // A partial take leaves the rest for the next one, and the two parts are the records.
    const usize pending = client.session.pending();
    REQUIRE(client.session.take(Span<u8>(out, 5U), written) == TlsError::None);
    CHECK(written == 5U);
    CHECK(client.session.pending() == pending - 5U);
    REQUIRE(server.session.receive(ConstSpan<u8>(out, 5U)) == TlsError::None);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    CHECK(read_all(server.session).size() == 10U);

    // Nothing to read and nothing to take: the provider asks for more bytes, which is no error.
    CHECK(client.session.read(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(written == 0U);
    CHECK(client.session.take(Span<u8>(out, sizeof(out)), written) == TlsError::None);
    CHECK(written == 0U);

    // A requested update, answered as the request is read.
    REQUIRE(server.session.update_keys(true) == TlsError::None);
    REQUIRE(deliver(server.session, client.session) == TlsError::None);
    CHECK(read_all(client.session).empty());
    CHECK(client.session.pending() == kKeyUpdateRecord);
    REQUIRE(deliver(client.session, server.session) == TlsError::None);
    CHECK(read_all(server.session).empty());
    exchange(client, server, 300U);
}
} // namespace

// The record paths skip the look at the provider's error queue when every call in them succeeded, and an asserting
// build checks there that the queue is empty (provider_internal.hpp's ErrorScope). A failed session goes first: what
// its failure queued must be gone, or the records after it on this thread would find it.
TEST_CASE("TLS: records, key updates and a failed session leave the provider's error queue empty",
          "[crypto][tls][net-2]")
{
#if CRD_ENABLE_ASSERTS
    {
        const CountedAsserts counted;
        records_and_failure();
    }
    CHECK(g_failed_asserts.load() == 0);
#else
    records_and_failure();
#endif
}

namespace
{
constexpr u8 kDecodeError = 50U;

// The algorithms a ClientHello offers in compress_certificate (RFC 8879 section 3), in order; 0 without the extension.
usize offered_compression(const Bytes& flight, u16 (&out)[4])
{
    usize count = 0U;
    for_each_extension(first_hello(flight), [&](u16 type, ConstSpan<u8> body)
    {
        if (type != iana::kTlsExtensionCompressCertificate)
        {
            return;
        }
        REQUIRE(body.size() >= 3U);
        REQUIRE(usize{body[0]} + 1U == body.size());
        for (usize at = 1U; at + 2U <= body.size() && count < 4U; at += 2U)
        {
            out[count++] = be16(body.data() + at);
        }
    });
    return count;
}

// A full handshake between fresh sessions of the endpoints' contexts: the algorithms the ClientHello offers and the
// bytes of the server's first flight (ServerHello to Finished).
struct Measured
{
    u16   offered[4] = {};
    usize offers     = 0U;
    usize flight     = 0U;
};

Measured handshake(Endpoint& client, Endpoint& server)
{
    Measured    measured;
    const Bytes hello = drain(client.session);
    measured.offers   = offered_compression(hello, measured.offered);
    REQUIRE(server.session.receive(ConstSpan<u8>(hello.data(), hello.size())) == TlsError::None);
    const Bytes flight = drain(server.session);
    measured.flight    = flight.size();
    REQUIRE(client.session.receive(ConstSpan<u8>(flight.data(), flight.size())) == TlsError::None);
    pump(client, server);
    REQUIRE(client.session.state() == TlsState::Established);
    REQUIRE(server.session.state() == TlsState::Established);
    exchange(client, server, 1000U);
    return measured;
}

// A 122-byte DNS name: a certificate carrying it three times (subject, issuer, subjectAltName) shrinks under zlib too.
constexpr const char* kLongName = "a1234567890123456789012345678901234567890123456789012345678."
                                  "b12345678901234567890123456789012345678901234567890.cerid.test";
} // namespace

TEST_CASE("TLS: a server sends its Certificate compressed when that is smaller, in the form its client decodes",
          "[crypto][tls][net-2]")
{
    // The server's first flight with the client offering zstd and zlib and with it offering nothing, from the same
    // credential. Ed25519 signs deterministically, so its bytes are exact; an ML-DSA signature is hedged (random bytes,
    // compressed differently each time) and an ECDSA signature varies in length too, so theirs save within a range.
    // An ECDSA certificate's zstd and zlib forms lie within a few bytes of each other, so its random signature decides
    // which is smaller from run to run (the choice of the smaller form is exact in the Ed25519 row and the next case).
    struct Expected
    {
        AlgorithmId key;
        u16         algorithm; // what the server compresses with: the smaller form; 0 where either can be
        usize       plain;     // the flight with the Certificate uncompressed, 0 where it varies
        usize       least;     // the bytes compression saves, at least and at most
        usize       most;
    };
    constexpr Expected expected[] = {
        {AlgorithmId::Ed25519, kZlib, 1651U, 39U, 39U},
        {AlgorithmId::MlDsa44, kZstd, 7668U, 30U, 80U},
        {AlgorithmId::MlDsa65, kZstd, 10086U, 30U, 80U},
        {AlgorithmId::MlDsa87, kZstd, 13362U, 30U, 80U},
        {AlgorithmId::EcdsaSecp256r1Sha256, 0U, 0U, 10U, 80U},
        {AlgorithmId::EcdsaSecp384r1Sha384, 0U, 0U, 10U, 80U},
    };
    for (const Expected& row : expected)
    {
        CAPTURE(algorithm_register_id(row.key));
        Endpoint client;
        Endpoint server;
        Options  options;
        options.server_key = row.key;
        setup(client, server, options);
        const Measured compressed = handshake(client, server);
        REQUIRE(compressed.offers == 2U);
        CHECK(compressed.offered[0] == kZstd);
        CHECK(compressed.offered[1] == kZlib);
        const u16 chosen = server.session.certificate_compression();
        CHECK((row.algorithm != 0U ? chosen == row.algorithm : (chosen == kZstd || chosen == kZlib)));
        CHECK(client.session.certificate_compression() == chosen);

        // The same server context and a client that decodes nothing: no extension, the Certificate as it is.
        Endpoint plain_client;
        TlsConfig config;
        config.role                    = TlsRole::Client;
        config.groups                  = kAllGroups;
        config.signatures              = kAllSignatures;
        config.aeads                   = kAllAeads;
        const KeyId pin                = server.credential.key_id();
        config.pinned                  = ConstSpan<KeyId>(&pin, 1U);
        config.server_name             = kServerName;
        config.certificate_compression = {};
        REQUIRE(plain_client.context.init(config) == TlsError::None);
        REQUIRE(plain_client.session.init(plain_client.context) == TlsError::None);
        REQUIRE(server.session.init(server.context) == TlsError::None);
        const Measured plain = handshake(plain_client, server);
        CHECK(plain.offers == 0U);
        CHECK(server.session.certificate_compression() == 0U);
        CHECK(plain_client.session.certificate_compression() == 0U);

        CAPTURE(plain.flight, compressed.flight);
        CHECK((row.plain == 0U || plain.flight == row.plain));
        REQUIRE(compressed.flight < plain.flight);
        CHECK(plain.flight - compressed.flight >= row.least);
        CHECK(plain.flight - compressed.flight <= row.most);
    }
}

TEST_CASE("TLS: the algorithm is the smallest form both ends allow, and none is a choice", "[crypto][tls][net-2]")
{
    constexpr u16 zstd_only[] = {kZstd};
    constexpr u16 zlib_only[] = {kZlib};
    struct Case
    {
        ConstSpan<u16> client;
        ConstSpan<u16> server;
        u16            expected;
    };
    const Case cases[] = {
        {kTlsCertificateCompression, kTlsCertificateCompression, kZlib},
        {zlib_only, kTlsCertificateCompression, kZlib},
        {zstd_only, kTlsCertificateCompression, kZstd},
        {kTlsCertificateCompression, zlib_only, kZlib},
        {kTlsCertificateCompression, {}, 0U},
        {{}, kTlsCertificateCompression, 0U},
        {zlib_only, zstd_only, 0U},
    };
    for (const Case& test : cases)
    {
        CAPTURE(test.client.size(), test.server.size(), test.expected);
        Endpoint client;
        Endpoint server;
        Options  options;
        options.credential_name    = kLongName;
        options.client_compression = test.client;
        options.server_compression = test.server;
        setup(client, server, options);
        const Measured measured = handshake(client, server);
        CHECK(measured.offers == test.client.size());
        CHECK(server.session.certificate_compression() == test.expected);
        CHECK(client.session.certificate_compression() == test.expected);
    }

    {
        // The engine's zlib (fixed Huffman) makes an ML-DSA-65 certificate larger, so a client that decodes only zlib
        // is sent the plain Certificate.
        Endpoint client;
        Endpoint server;
        Options  options;
        options.server_key         = AlgorithmId::MlDsa65;
        options.client_compression = zlib_only;
        setup(client, server, options);
        (void)handshake(client, server);
        CHECK(server.session.certificate_compression() == 0U);
        CHECK(client.session.certificate_compression() == 0U);
    }

    // A resumption sends no Certificate, so nothing is compressed.
    Store    store;
    Endpoint client;
    Endpoint server;
    Options  options;
    options.client_tickets = &store;
    setup(client, server, options);
    (void)handshake(client, server);
    CHECK(client.session.certificate_compression() == kZlib);
    REQUIRE(store.size() == 1U);
    REQUIRE(client.session.init(client.context, &store) == TlsError::None);
    REQUIRE(server.session.init(server.context) == TlsError::None);
    (void)handshake(client, server);
    CHECK(client.session.resumed());
    CHECK(client.session.certificate_compression() == 0U);
    CHECK(server.session.certificate_compression() == 0U);
}

TEST_CASE("TLS: a compressed Certificate decodes only to exactly the length it claims, within the bounds",
          "[crypto][tls][net-2]")
{
    Bytes message;
    message.resize(600U);
    for (usize i = 0U; i < message.size(); ++i)
    {
        message[i] = static_cast<u8>(i % 53U == 0U ? i * 7U : 0x40U + i % 5U);
    }
    const ConstSpan<u8> plain(message.data(), message.size());
    Bytes               zstd_form;
    REQUIRE(crd::compression::zstd_compress(plain, crd::compression::kZstdDefaultLevel, zstd_form));
    const Bytes zlib_form = crd::compression::zlib_deflate(plain, crd::memory::default_allocator());
    REQUIRE(!zlib_form.empty());

    Bytes out;
    out.resize(kTlsMaxCertificateBytes + 1U);
    const auto decode = [&](u16 algorithm, const Bytes& form, usize claim) {
        std::memset(out.data(), 0xA5, out.size());
        return detail::tls_decompress_certificate(algorithm, ConstSpan<u8>(form.data(), form.size()),
                                                  Span<u8>(out.data(), claim));
    };
    const auto untouched = [&] { return std::all_of(out.begin(), out.end(), [](u8 b) { return b == u8{0xA5U}; }); };

    const std::pair<u16, const Bytes*> forms[] = {{kZstd, &zstd_form}, {kZlib, &zlib_form}};
    for (const auto& [algorithm, form] : forms)
    {
        CAPTURE(algorithm);
        CHECK(decode(algorithm, *form, message.size()));
        CHECK(std::memcmp(out.data(), message.data(), message.size()) == 0);
        CHECK_FALSE(decode(algorithm, *form, message.size() - 1U)); // decodes to more than the claim
        CHECK_FALSE(decode(algorithm, *form, message.size() + 1U)); // to less
        Bytes cut = *form;
        cut.resize(cut.size() - 1U);
        CHECK_FALSE(decode(algorithm, cut, message.size()));
        // A claim outside [4, kTlsMaxCertificateBytes] is refused before a byte is written.
        CHECK_FALSE(decode(algorithm, *form, kTlsMaxCertificateBytes + 1U));
        CHECK(untouched());
        CHECK_FALSE(decode(algorithm, *form, 3U));
        CHECK(untouched());
    }
    // zlib's Adler-32 catches a changed byte (zstd frames carry no checksum here; the Certificate is then parsed and
    // its key pinned like any other).
    Bytes changed = zlib_form;
    changed[changed.size() / 2U] ^= 0x10U;
    CHECK_FALSE(decode(kZlib, changed, message.size()));

    // A zlib claim its bytes cannot reach is refused untouched: 9 bytes of zlib hold at most 3 * 1032 bytes of DEFLATE.
    // Two bytes are no zstd frame.
    const u8    tiny_zlib[] = {0x78U, 0x9CU, 0x4BU, 0x04U, 0x00U, 0x00U, 0x62U, 0x00U, 0x62U};
    const Bytes tiny(tiny_zlib, tiny_zlib + sizeof(tiny_zlib));
    CHECK_FALSE(decode(kZlib, tiny, 3U * 1032U + 1U));
    CHECK(untouched());
    const Bytes two(zstd_form.begin(), zstd_form.begin() + 2);
    CHECK_FALSE(decode(kZstd, two, 4U));
    CHECK(untouched());
    // Only zstd and zlib: brotli, the reserved 0 and an unassigned value decode nothing.
    for (const u16 algorithm : {u16{2U}, u16{0U}, u16{4U}})
    {
        CHECK_FALSE(decode(algorithm, zstd_form, message.size()));
        CHECK(untouched());
    }
}

namespace
{
// A server of the provider's own TLS stack, outside Cerid's sessions, whose compressor the test chooses: it sends a
// Cerid client a CompressedCertificate that a Cerid server never would. Its certificate carries the Ed25519 key of
// make_key(.., 0), which the client pins.
enum class Hostile : u8
{
    Honest,      // zstd of the message
    Stored,      // zlib with stored blocks: valid, and larger than the message
    Longer,      // zstd of the message and one byte more
    Shorter,     // zstd of the message without its last byte
    Garbage,     // bytes no zstd frame starts with
    BadChecksum, // zlib whose Adler-32 is changed
};

Hostile g_hostile = Hostile::Honest;

// The provider calls this from C: it reports a failure by its result, never by a test macro.
int hostile_compress(SSL*, CBB* out, const uint8_t* in, size_t in_len)
{
    Bytes message(in, in + in_len);
    if (g_hostile == Hostile::Longer)
    {
        message.push_back(0U);
    }
    else if (g_hostile == Hostile::Shorter)
    {
        message.resize(in_len - 1U);
    }
    Bytes form;
    switch (g_hostile)
    {
    case Hostile::Honest:
    case Hostile::Longer:
    case Hostile::Shorter:
        if (!crd::compression::zstd_compress(ConstSpan<u8>(message.data(), message.size()), 3, form))
        {
            return 0;
        }
        break;
    case Hostile::Garbage:
        form.resize(32U);
        std::memset(form.data(), 0xFF, form.size());
        break;
    case Hostile::Stored:
    case Hostile::BadChecksum:
    {
        // RFC 1950 and RFC 1951 section 3.2.4: the header, one final stored block, the Adler-32.
        if (in_len > 0xFFFFU)
        {
            return 0;
        }
        form = {0x78U, 0x01U, 0x01U, static_cast<u8>(in_len), static_cast<u8>(in_len >> 8U),
                static_cast<u8>(~in_len), static_cast<u8>(~in_len >> 8U)};
        for (const u8 byte : message)
        {
            form.push_back(byte);
        }
        u32 adler = crd::compression::adler32(ConstSpan<u8>(message.data(), message.size()));
        if (g_hostile == Hostile::BadChecksum)
        {
            adler ^= 1U;
        }
        for (const u32 shift : {24U, 16U, 8U, 0U})
        {
            form.push_back(static_cast<u8>(adler >> shift));
        }
        break;
    }
    }
    return CBB_add_bytes(out, form.data(), form.size());
}

struct RawServer
{
    bssl::UniquePtr<SSL_CTX> context;
    bssl::UniquePtr<SSL>     ssl;
    BIO*                     incoming = nullptr;
    BIO*                     outgoing = nullptr;

    explicit RawServer(u16 algorithm)
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        const bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed,
                                                                         sizeof(seed)));
        const bssl::UniquePtr<X509> certificate(X509_new());
        REQUIRE(key);
        REQUIRE(certificate);
        X509_NAME* name = X509_get_subject_name(certificate.get());
        REQUIRE(X509_set_version(certificate.get(), X509_VERSION_3));
        REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1));
        REQUIRE(X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_ASC,
                                           reinterpret_cast<const unsigned char*>(kServerName), -1, -1, 0));
        REQUIRE(X509_set_issuer_name(certificate.get(), name));
        REQUIRE(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -3600) != nullptr);
        REQUIRE(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr);
        REQUIRE(X509_set_pubkey(certificate.get(), key.get()));
        REQUIRE(X509_sign(certificate.get(), key.get(), nullptr) > 0);
        context.reset(SSL_CTX_new(TLS_method()));
        REQUIRE(context);
        REQUIRE(SSL_CTX_set_min_proto_version(context.get(), TLS1_3_VERSION));
        REQUIRE(SSL_CTX_use_certificate(context.get(), certificate.get()));
        REQUIRE(SSL_CTX_use_PrivateKey(context.get(), key.get()));
        REQUIRE(SSL_CTX_add_cert_compression_alg(context.get(), algorithm, &hostile_compress, nullptr));
        ssl.reset(SSL_new(context.get()));
        REQUIRE(ssl);
        incoming = BIO_new(BIO_s_mem());
        outgoing = BIO_new(BIO_s_mem());
        REQUIRE(incoming != nullptr);
        REQUIRE(outgoing != nullptr);
        BIO_set_mem_eof_return(incoming, -1);
        SSL_set_bio(ssl.get(), incoming, outgoing);
        SSL_set_accept_state(ssl.get());
    }

    // Takes the client's bytes, runs the handshake and returns what the server sends.
    Bytes answer(const Bytes& received)
    {
        if (!received.empty())
        {
            REQUIRE(BIO_write(incoming, received.data(), static_cast<int>(received.size())) ==
                    static_cast<int>(received.size()));
        }
        (void)SSL_do_handshake(ssl.get());
        Bytes      sent;
        const auto size = static_cast<usize>(BIO_pending(outgoing));
        sent.resize(size);
        if (size != 0U)
        {
            REQUIRE(BIO_read(outgoing, sent.data(), static_cast<int>(size)) == static_cast<int>(size));
        }
        return sent;
    }
};

// A Cerid client decoding `offered` against a hostile server compressing with `algorithm`; the client's state when the
// exchange ends.
TlsState against_hostile(Hostile hostile, u16 algorithm, Endpoint& client,
                         ConstSpan<u16> offered = kTlsCertificateCompression)
{
    g_hostile = hostile;
    SigningKey key;
    make_key(key, AlgorithmId::Ed25519, 0U);
    u8 public_key[32];
    REQUIRE(key.public_key(Span<u8>(public_key)) == CryptoError::None);
    const KeyId pin = key_id_of(ConstSpan<u8>(public_key));
    TlsConfig   config;
    config.role        = TlsRole::Client;
    config.groups      = kAllGroups;
    config.signatures  = kAllSignatures;
    config.aeads       = kAllAeads;
    config.pinned      = ConstSpan<KeyId>(&pin, 1U);
    config.server_name = kServerName;
    config.certificate_compression = offered;
    REQUIRE(client.context.init(config) == TlsError::None);
    REQUIRE(client.session.init(client.context) == TlsError::None);
    RawServer server(algorithm);
    for (int round = 0; round < 8; ++round)
    {
        const Bytes sent   = drain(client.session);
        const Bytes answer = server.answer(sent);
        if (answer.empty() && sent.empty())
        {
            break;
        }
        if (!answer.empty() && client.session.state() == TlsState::Handshaking)
        {
            (void)client.session.receive(ConstSpan<u8>(answer.data(), answer.size()));
        }
    }
    return client.session.state();
}
} // namespace

TEST_CASE("TLS: a client decodes any valid compressed Certificate and refuses every other with decode_error",
          "[crypto][tls][net-2]")
{
    {
        Endpoint client;
        CHECK(against_hostile(Hostile::Honest, kZstd, client) == TlsState::Established);
        CHECK(client.session.certificate_compression() == kZstd);
    }
    {
        // Larger than the message, and still accepted: the client checks the decoding, not the gain.
        Endpoint client;
        CHECK(against_hostile(Hostile::Stored, kZlib, client) == TlsState::Established);
        CHECK(client.session.certificate_compression() == kZlib);
    }
    // A decoding of another length, bytes that decode to nothing and a changed checksum fail the handshake; the
    // provider answers with decode_error where RFC 8879 section 4 names bad_certificate.
    const std::pair<Hostile, u16> refused[] = {{Hostile::Longer, kZstd}, {Hostile::Shorter, kZstd},
                                               {Hostile::Garbage, kZstd}, {Hostile::BadChecksum, kZlib}};
    for (const auto& [hostile, algorithm] : refused)
    {
        CAPTURE(static_cast<int>(hostile));
        Endpoint client;
        CHECK(against_hostile(hostile, algorithm, client) == TlsState::Failed);
        CHECK(client.session.alert_sent().description == kDecodeError);
        CHECK(client.session.certificate_compression() == algorithm);
        CHECK(client.session.peer_key() == KeyId{});
    }
    // A client that offers only zstd is never sent zlib: the server finds no algorithm in common and sends the
    // Certificate as it is.
    constexpr u16 zstd_only[] = {kZstd};
    Endpoint      client;
    CHECK(against_hostile(Hostile::Stored, kZlib, client, zstd_only) == TlsState::Established);
    CHECK(client.session.certificate_compression() == 0U);
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
    CHECK(client_with([](TlsConfig& c) { c.key_update_records = kTlsMinKeyUpdateRecords - 1U; }) ==
          TlsError::BadConfig);
    CHECK(client_with([](TlsConfig& c) { c.key_update_records = 1U; }) == TlsError::BadConfig);
    CHECK(client_with([](TlsConfig& c) { c.key_update_records = kTlsMinKeyUpdateRecords; }) == TlsError::None);
    CHECK(client_with([](TlsConfig& c) { c.max_key_updates = kTlsMaxKeyUpdates + 1U; }) == TlsError::BadConfig);
    CHECK(client_with([](TlsConfig& c) { c.max_key_updates = kTlsMaxKeyUpdates; }) == TlsError::None);
    CHECK(client_with([](TlsConfig& c) { c.max_key_updates = 1U; }) == TlsError::None);

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
    // Ticket keys and lifetimes are a server's; a key is kTlsTicketKeyBytes long, a previous key needs a current one.
    u8 ticket_key[kTlsTicketKeyBytes + 1U] = {};
    CHECK(client_with([&](TlsConfig& c) { c.ticket_key = ConstSpan<u8>(ticket_key, kTlsTicketKeyBytes); }) ==
          TlsError::BadConfig);
    CHECK(client_with([&](TlsConfig& c) { c.previous_ticket_key = ConstSpan<u8>(ticket_key, kTlsTicketKeyBytes); }) ==
          TlsError::BadConfig);
    CHECK(client_with([](TlsConfig& c) { c.ticket_lifetime = 60U; }) == TlsError::BadConfig);
    const auto server_with = [&](auto&& change) {
        TlsConfig config;
        config.role       = TlsRole::Server;
        config.groups     = kAllGroups;
        config.aeads      = kAllAeads;
        config.credential = &server.credential;
        change(config);
        TlsContext ticketed;
        return ticketed.init(config);
    };
    const ConstSpan<u8> key(ticket_key, kTlsTicketKeyBytes);
    CHECK(server_with([&](TlsConfig& c) { c.ticket_key = key; }) == TlsError::None);
    CHECK(server_with([&](TlsConfig& c) { c.ticket_key = key.first(kTlsTicketKeyBytes - 1U); }) ==
          TlsError::BadConfig);
    CHECK(server_with([&](TlsConfig& c) { c.ticket_key = ConstSpan<u8>(ticket_key, sizeof(ticket_key)); }) ==
          TlsError::BadConfig);
    CHECK(server_with([&](TlsConfig& c) { c.previous_ticket_key = key; }) == TlsError::BadConfig);
    CHECK(server_with([&](TlsConfig& c) {
              c.ticket_key          = key;
              c.previous_ticket_key = key.first(1U);
          }) == TlsError::BadConfig);
    CHECK(server_with([&](TlsConfig& c) {
              c.ticket_key          = key;
              c.previous_ticket_key = key;
          }) == TlsError::None);
    CHECK(server_with([](TlsConfig& c) { c.ticket_lifetime = kTlsMaxTicketLifetime; }) == TlsError::None);
    CHECK(server_with([](TlsConfig& c) { c.ticket_lifetime = kTlsMaxTicketLifetime + 1U; }) == TlsError::BadConfig);

    // Certificate compression: RFC 8879 algorithms Cerid speaks, each once, either way; none at all is a choice.
    constexpr u16 zlib_only[]  = {kZlib};
    constexpr u16 zlib_first[] = {kZlib, kZstd};
    constexpr u16 brotli[]     = {2U};
    constexpr u16 reserved[]   = {0U};
    constexpr u16 repeated[]   = {kZstd, kZstd};
    constexpr u16 three[]      = {kZstd, kZlib, kZstd};
    for (const ConstSpan<u16> fine : {ConstSpan<u16>(), ConstSpan<u16>(zlib_only), ConstSpan<u16>(zlib_first)})
    {
        CHECK(client_with([&](TlsConfig& c) { c.certificate_compression = fine; }) == TlsError::None);
        CHECK(server_with([&](TlsConfig& c) { c.certificate_compression = fine; }) == TlsError::None);
    }
    for (const ConstSpan<u16> wrong : {ConstSpan<u16>(brotli), ConstSpan<u16>(reserved), ConstSpan<u16>(repeated),
                                       ConstSpan<u16>(three)})
    {
        CHECK(client_with([&](TlsConfig& c) { c.certificate_compression = wrong; }) == TlsError::BadConfig);
        CHECK(server_with([&](TlsConfig& c) { c.certificate_compression = wrong; }) == TlsError::BadConfig);
    }

    server_config.signatures = kAllSignatures; // signatures without pins
    CHECK(context.init(server_config) == TlsError::BadConfig);
    CHECK_FALSE(context.ready());

    TlsSession session;
    CHECK(session.init(context) == TlsError::NotReady);
    {
        // A server takes no ticket store: its tickets are sealed, not kept.
        server_config.signatures = {};
        TlsContext serving;
        REQUIRE(serving.init(server_config) == TlsError::None);
        Store store;
        CHECK(session.init(serving, &store) == TlsError::BadConfig);
        CHECK(session.state() == TlsState::Idle);
        CHECK(store.takes == 0U);
    }
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
    // Every DNS name makes a credential: past 64 characters (ub-common-name) the subjectAltName alone carries it.
    char dotted[kTlsMaxNameBytes] = {};
    for (usize i = 0U; i < sizeof(dotted); ++i)
    {
        dotted[i] = i % 2U == 0U ? 'a' : '.';
    }
    for (const usize size : {usize{63U}, usize{65U}, kTlsMaxNameBytes})
    {
        CAPTURE(size);
        CHECK(credential.init(server.key, StringView(dotted, size)) == TlsError::None);
        CHECK(credential.ready());
    }

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

namespace
{
Bytes exported(const TlsSession& session, StringView label, ConstSpan<u8> context, usize size)
{
    Bytes out;
    out.resize(size);
    REQUIRE(session.export_keying_material(label, context, Span<u8>(out.data(), out.size())) == TlsError::None);
    return out;
}

bool same(const Bytes& a, const Bytes& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

bool zero(ConstSpan<u8> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](u8 byte) { return byte == 0U; });
}
} // namespace

TEST_CASE("TLS: the exporter is equal at both ends, unique to each connection and unchanged by key updates",
          "[crypto][tls][net-2]")
{
    Store    store;
    Endpoint client;
    Endpoint server;
    Options  options;
    options.client_tickets = &store;
    setup(client, server, options);
    u8 early[32] = {1U};
    CHECK(client.session.export_keying_material("EXPERIMENTAL test", {}, early) == TlsError::NotReady);
    CHECK(zero(early));
    establish(client, server);
    read_tickets(client.session);

    const u8    context[] = {'x'};
    const Bytes first     = exported(client.session, "EXPERIMENTAL test", {}, 32U);
    CHECK(same(first, exported(server.session, "EXPERIMENTAL test", {}, 32U)));
    CHECK_FALSE(zero(ConstSpan<u8>(first.data(), first.size())));
    // The label, the context and the length each change every byte: the length is in HKDF-Expand-Label's info.
    const Bytes other_label   = exported(client.session, "EXPERIMENTAL other", {}, 32U);
    const Bytes with_context  = exported(client.session, "EXPERIMENTAL test", context, 32U);
    const Bytes shorter       = exported(client.session, "EXPERIMENTAL test", {}, 16U);
    CHECK(same(with_context, exported(server.session, "EXPERIMENTAL test", context, 32U)));
    CHECK_FALSE(same(first, other_label));
    CHECK_FALSE(same(first, with_context));
    CHECK(std::memcmp(first.data(), shorter.data(), shorter.size()) != 0);

    // The bounds: a label of 249 bytes and 8,160 exported bytes are taken; one more of either is BadConfig, zeroed.
    char longest[kTlsMaxExporterLabelBytes + 1U];
    std::memset(longest, 'L', sizeof(longest));
    const StringView label_bound(longest, kTlsMaxExporterLabelBytes);
    CHECK(same(exported(client.session, label_bound, {}, 32U), exported(server.session, label_bound, {}, 32U)));
    CHECK(same(exported(client.session, "EXPERIMENTAL test", {}, kTlsMaxExporterBytes),
               exported(server.session, "EXPERIMENTAL test", {}, kTlsMaxExporterBytes)));
    Bytes refused;
    refused.resize(kTlsMaxExporterBytes + 1U);
    std::memset(refused.data(), 0xA5, refused.size());
    const Span<u8> all(refused.data(), refused.size());
    CHECK(client.session.export_keying_material("EXPERIMENTAL test", {}, all) == TlsError::BadConfig);
    CHECK(zero(all));
    std::memset(refused.data(), 0xA5, refused.size());
    CHECK(client.session.export_keying_material(StringView(longest, sizeof(longest)), {}, all.first(32U)) ==
          TlsError::BadConfig);
    CHECK(zero(all.first(32U)));
    CHECK(client.session.export_keying_material("", {}, all.first(32U)) == TlsError::BadConfig);
    CHECK(client.session.export_keying_material("EXPERIMENTAL test", {}, Span<u8>()) == TlsError::BadConfig);

    // Key updates both ways change the record keys, never the exporter.
    REQUIRE(client.session.update_keys(true) == TlsError::None);
    pump(client, server);
    exchange(client, server, 100U);
    REQUIRE(client.session.key_updates_sent() == 1U);
    REQUIRE(server.session.key_updates_sent() == 1U);
    CHECK(same(first, exported(client.session, "EXPERIMENTAL test", {}, 32U)));
    CHECK(same(first, exported(server.session, "EXPERIMENTAL test", {}, 32U)));

    // A resumed connection has its own exporter, equal at its ends.
    REQUIRE(resume(client, server, store));
    const Bytes resumed = exported(client.session, "EXPERIMENTAL test", {}, 32U);
    CHECK(same(resumed, exported(server.session, "EXPERIMENTAL test", {}, 32U)));
    CHECK_FALSE(same(first, resumed));

    // A closed session exports nothing.
    REQUIRE(client.session.close() == TlsError::None);
    u8 closed[32] = {1U};
    CHECK(client.session.export_keying_material("EXPERIMENTAL test", {}, closed) == TlsError::NotReady);
    CHECK(zero(closed));
}

// ---- RFC 9850 key logging (decision S19, threat T22) ----

namespace
{
using crd::crypto::test::KeyLog;
using crd::crypto::test::KeyLogLine;
using crd::crypto::test::kKeyLogTraffic;

constexpr u8 kRecordApplicationData = 23U;
constexpr u8 kEncryptedExtensions   = 8U;

AlgorithmId hkdf_of(AlgorithmId aead)
{
    return aead == AlgorithmId::Aes256Gcm ? AlgorithmId::HkdfSha384 : AlgorithmId::HkdfSha256;
}

// HKDF-Expand-Label (RFC 9846 section 7.1) as the RFC writes it, apart from the provider's TLS stack.
void expand_label(AlgorithmId hkdf, ConstSpan<u8> secret, StringView label, ConstSpan<u8> context, Span<u8> out)
{
    u8    info[2U + 1U + 255U + 1U + 255U] = {};
    usize size                             = 0U;
    info[size++]                           = static_cast<u8>(out.size() >> 8U);
    info[size++]                           = static_cast<u8>(out.size());
    info[size++]                           = static_cast<u8>(6U + label.size());
    for (const StringView part : {StringView("tls13 "), label})
    {
        for (const char c : part)
        {
            info[size++] = static_cast<u8>(c);
        }
    }
    info[size++] = static_cast<u8>(context.size());
    for (const u8 byte : context)
    {
        info[size++] = byte;
    }
    REQUIRE(hkdf_expand(hkdf, secret, ConstSpan<u8>(info, size), out) == CryptoError::None);
}

// Opens the TLS 1.3 record `record` as record number `sequence` under the traffic secret `secret` (RFC 9846 sections
// 5.2 and 5.3): its content type and its content, the padding stripped; false when it does not open.
bool open_record(AlgorithmId aead, ConstSpan<u8> secret, u64 sequence, ConstSpan<u8> record, u8& type, Bytes& content)
{
    const AlgorithmId hkdf          = hkdf_of(aead);
    u8                key_bytes[32] = {};
    u8                nonce[kAeadNonceBytes];
    const Span<u8>    key(key_bytes, aead_key_bytes(aead));
    expand_label(hkdf, secret, "key", {}, key);
    expand_label(hkdf, secret, "iv", {}, nonce);
    for (usize i = 0U; i < 8U; ++i)
    {
        nonce[kAeadNonceBytes - 1U - i] ^= static_cast<u8>(sequence >> (8U * i));
    }
    AeadKey opener;
    REQUIRE(opener.init(aead, key) == CryptoError::None);
    if (record.size() < 5U + kAeadTagBytes + 1U)
    {
        return false;
    }
    Bytes plain;
    plain.resize(record.size() - 5U - kAeadTagBytes);
    if (opener.open(nonce, record.subspan(5U), record.first(5U), Span<u8>(plain.data(), plain.size())) !=
        CryptoError::None)
    {
        return false;
    }
    usize end = plain.size();
    while (end > 0U && plain[end - 1U] == 0U)
    {
        --end;
    }
    if (end == 0U)
    {
        return false;
    }
    type = plain[end - 1U];
    content.clear();
    for (usize i = 0U; i + 1U < end; ++i)
    {
        content.push_back(plain[i]);
    }
    return true;
}

// The first record of `flight` whose outer type is application_data (a protected record).
ConstSpan<u8> first_protected(const Bytes& flight)
{
    for (usize at = 0U; at + 5U <= flight.size();)
    {
        const usize length = (static_cast<usize>(flight[at + 3U]) << 8U) | flight[at + 4U];
        REQUIRE(at + 5U + length <= flight.size());
        if (flight[at] == kRecordApplicationData)
        {
            return ConstSpan<u8>(flight.data() + at, 5U + length);
        }
        at += 5U + length;
    }
    FAIL("the flight holds no protected record");
    return {};
}

ConstSpan<u8> secret_of(const KeyLog& log, StringView label)
{
    const KeyLogLine* line = log.find(label);
    REQUIRE(line != nullptr);
    return line->value();
}

Bytes text_bytes(StringView text)
{
    Bytes bytes;
    for (const char c : text)
    {
        bytes.push_back(static_cast<u8>(c));
    }
    return bytes;
}
} // namespace

TEST_CASE("TLS key log: a build states whether it has RFC 9850 key logging, and one without it refuses a sink",
          "[crypto][tls][key-log][net-2]")
{
    // The expectation comes from the shipping condition through the test's own definition, so a shipping lane fails
    // the moment key logging reaches a shipping build.
    CHECK(tls_key_log_compiled() == (CRD_TEST_EXPECT_TLS_KEY_LOG != 0));

    SigningKey    key;
    TlsCredential credential;
    make_key(key, AlgorithmId::Ed25519, 0U);
    REQUIRE(credential.init(key, kServerName) == TlsError::None);
    const KeyId pin = credential.key_id();
    KeyLog      log;
    TlsContext  context;

    TlsConfig client;
    client.role         = TlsRole::Client;
    client.groups       = kAllGroups;
    client.signatures   = kAllSignatures;
    client.aeads        = kAllAeads;
    client.pinned       = ConstSpan<KeyId>(&pin, 1U);
    client.server_name  = kServerName;
    client.key_log_user = &log;
    CHECK(context.init(client) == TlsError::BadConfig);
    client.key_log_user      = nullptr;
    client.key_log_exporters = true;
    CHECK(context.init(client) == TlsError::BadConfig);
    CHECK_FALSE(context.ready());

    TlsConfig server;
    server.role       = TlsRole::Server;
    server.groups     = kAllGroups;
    server.aeads      = kAllAeads;
    server.credential = &credential;
    for (TlsConfig* config : {&client, &server})
    {
        log.attach(*config, true);
        CHECK(context.init(*config) == (tls_key_log_compiled() ? TlsError::None : TlsError::BadConfig));
        CHECK(context.ready() == tls_key_log_compiled());
    }
    CHECK(log.size() == 0U);
}

TEST_CASE("TLS key log: both ends log the same RFC 9850 lines, whose secrets open the connection's records",
          "[crypto][tls][key-log][net-2]")
{
    if (!tls_key_log_compiled())
    {
        SKIP("key logging is compiled out of this build");
    }
    for (const AlgorithmId aead : kAllAeads)
    {
        CAPTURE(static_cast<u16>(aead));
        Endpoint client;
        Endpoint server;
        KeyLog   client_log;
        KeyLog   server_log;
        Options  options;
        options.client_aeads = ConstSpan<AlgorithmId>(&aead, 1U);
        options.server_aeads = ConstSpan<AlgorithmId>(&aead, 1U);
        options.client_log   = &client_log;
        options.server_log   = &server_log;
        setup(client, server, options);

        // The ClientHello's random (after the record header, the handshake header and legacy_version) names every
        // line; the server's first protected record opens under its handshake secret as EncryptedExtensions.
        const Bytes hello = drain(client.session);
        REQUIRE(hello.size() > 43U);
        const ConstSpan<u8> random(hello.data() + 11U, 32U);
        REQUIRE(server.session.receive(ConstSpan<u8>(hello.data(), hello.size())) == TlsError::None);
        const Bytes flight = drain(server.session);
        u8          type   = 0U;
        Bytes       content;
        REQUIRE(open_record(aead, secret_of(server_log, "SERVER_HANDSHAKE_TRAFFIC_SECRET"), 0U,
                            first_protected(flight), type, content));
        CHECK(type == kRecordHandshake);
        CHECK((!content.empty() && content[0] == kEncryptedExtensions));
        REQUIRE(client.session.receive(ConstSpan<u8>(flight.data(), flight.size())) == TlsError::None);
        establish(client, server);

        CHECK_FALSE(client_log.malformed());
        CHECK_FALSE(server_log.malformed());
        CHECK(client_log.size() == std::size(kKeyLogTraffic));
        for (const char* label : kKeyLogTraffic)
        {
            CAPTURE(label);
            const KeyLogLine* line = client_log.find(label);
            REQUIRE(line != nullptr);
            CHECK(line->secret_bytes == hkdf_hash_bytes(hkdf_of(aead)));
        }
        CHECK(client_log.find("EXPORTER_SECRET") == nullptr);
        CHECK(client_log.same(server_log));
        CHECK(client_log.all_of_random(random));

        // Application data opens under its traffic secret as its first record, and under nothing else.
        const Bytes message = text_bytes("logged for a capture");
        REQUIRE(client.session.send(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
        const Bytes         record    = drain(client.session);
        const ConstSpan<u8> protected_record = first_protected(record);
        REQUIRE(open_record(aead, secret_of(client_log, "CLIENT_TRAFFIC_SECRET_0"), 0U, protected_record, type,
                            content));
        CHECK(type == kRecordApplicationData);
        CHECK(same_bytes(content, message));
        CHECK_FALSE(open_record(aead, secret_of(client_log, "SERVER_TRAFFIC_SECRET_0"), 0U, protected_record, type,
                                content));
        CHECK_FALSE(open_record(aead, secret_of(client_log, "CLIENT_TRAFFIC_SECRET_0"), 1U, protected_record, type,
                                content));
        REQUIRE(server.session.receive(ConstSpan<u8>(record.data(), record.size())) == TlsError::None);
        CHECK(same_bytes(read_all(server.session), message));
    }
}

TEST_CASE("TLS key log: EXPORTER_SECRET only where the context allows it, and every exporter value follows from it",
          "[crypto][tls][key-log][net-2]")
{
    if (!tls_key_log_compiled())
    {
        SKIP("key logging is compiled out of this build");
    }
    Endpoint client;
    Endpoint server;
    KeyLog   client_log;
    KeyLog   server_log;
    Options  options;
    options.client_log    = &client_log;
    options.server_log    = &server_log;
    options.log_exporters = true;
    setup(client, server, options);
    establish(client, server);
    CHECK_FALSE(client_log.malformed());
    CHECK(client_log.size() == std::size(kKeyLogTraffic) + 1U);
    CHECK(client_log.same(server_log));

    // RFC 9846 section 7.5: HKDF-Expand-Label(Derive-Secret(exporter secret, label, ""), "exporter", Hash(context), L).
    const AlgorithmId hkdf             = hkdf_of(client.session.aead());
    const usize       hash             = hkdf_hash_bytes(hkdf);
    const u8          context[]        = {1U, 2U, 3U};
    const StringView  label            = "EXPERIMENTAL key log";
    u8                empty_hash[48]   = {};
    u8                context_hash[48] = {};
    u8                derived[48]      = {};
    u8                expected[40]     = {};
    u8                exported[40]     = {};
    REQUIRE(kdf_hash(hkdf, {}, Span<u8>(empty_hash, hash)) == CryptoError::None);
    REQUIRE(kdf_hash(hkdf, context, Span<u8>(context_hash, hash)) == CryptoError::None);
    expand_label(hkdf, secret_of(client_log, "EXPORTER_SECRET"), label, ConstSpan<u8>(empty_hash, hash),
                 Span<u8>(derived, hash));
    expand_label(hkdf, ConstSpan<u8>(derived, hash), "exporter", ConstSpan<u8>(context_hash, hash), expected);
    for (TlsSession* session : {&client.session, &server.session})
    {
        REQUIRE(session->export_keying_material(label, context, exported) == TlsError::None);
        CHECK(std::memcmp(exported, expected, sizeof(expected)) == 0);
    }
}
