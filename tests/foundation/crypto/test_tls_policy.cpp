// crd-crypto -- the crypto policy generation in TLS 1.3 sessions and QUIC handshakes (NET.2 decision S20;
// docs/design/network-foundation.md#tls-policy): each session takes a snapshot when it starts and offers and accepts
// only what its generation allows in the context's scope, preferred algorithms first; a refused credential, a list
// left empty or a store without a generation refuses the session before a byte; a resumed ticket of a peer whose
// algorithm is now refused is refused; a generation applied while a session runs reaches it at its next key update
// (its own, one send() makes, or the peer's), which closes it cleanly; Cerid service contexts follow their own rules;
// and a QUIC transport's check before a packet key update.

#include "policy_test_signer.hpp"

#include <crd/containers/array.hpp>
#include <crd/crypto/policy.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <ctime>

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
using crd::crypto::test::kTestSignatureBytes;
using crd::crypto::test::kTestTrust;
using crd::crypto::test::test_sign;
using crd::crypto::test::TestVerifier;

using Bytes = crd::containers::Array<u8>;

constexpr u64 kDay = 86400U;
constexpr const char* kServerName = "game.cerid.test";
constexpr StringView kGame[] = {"cerid-game/1"};
constexpr u8 kClientParameters[] = {0x04U, 0x04U, 0x80U, 0x10U, 0x00U, 0x00U};
constexpr u8 kServerParameters[] = {0x00U, 0x04U, 0x01U, 0x02U, 0x03U, 0x04U};
constexpr AlgorithmId kAllGroups[] = {
    AlgorithmId::X25519,         AlgorithmId::Secp256r1,         AlgorithmId::Secp384r1,
    AlgorithmId::X25519Mlkem768, AlgorithmId::Secp256r1Mlkem768, AlgorithmId::Secp384r1Mlkem1024,
};
constexpr AlgorithmId kAllSignatures[] = {AlgorithmId::Ed25519, AlgorithmId::MlDsa65,
                                          AlgorithmId::EcdsaSecp256r1Sha256};
constexpr AlgorithmId kAllAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};

u64 now_seconds()
{
    return static_cast<u64>(std::time(nullptr));
}

// Generation `number`, issued a minute ago and valid 30 days, accepting every algorithm of the register and preferring
// the hybrid groups: the tests narrow it with refuse() and prefer().
PolicyContent accepting_all(u64 number)
{
    PolicyContent content;
    content.generation = number;
    content.issued = now_seconds() - 60U;
    content.expires = content.issued + 30U * kDay;
    content.offline_grace = 7U * kDay;
    for (const AlgorithmId algorithm : kAlgorithmIds)
    {
        const bool hybrid = algorithm_class(algorithm) == AlgorithmClass::HybridKeyExchange;
        content.rules[content.rule_count++] = {algorithm, 0U, 0U, hybrid ? kNever : 0U, kNever};
    }
    return content;
}

AlgorithmRule& rule_of(PolicyContent& content, AlgorithmId algorithm)
{
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        if (content.rules[i].algorithm == algorithm && content.rules[i].scope == PolicyScope::Everywhere)
        {
            return content.rules[i];
        }
    }
    FAIL("no rule");
    return content.rules[0];
}

void refuse(PolicyContent& content, AlgorithmId algorithm)
{
    rule_of(content, algorithm) = {algorithm, 0U, 0U, 0U, 1U}; // refused since 1970
}

void prefer(PolicyContent& content, AlgorithmId algorithm)
{
    rule_of(content, algorithm) = {algorithm, 0U, 0U, kNever, kNever};
}

// An installation's policy: the test trust set, a build that supports the whole register, and its store.
struct Policy
{
    TestVerifier verifier;
    PolicyStore store{
        PolicyConfig{ConstSpan<TrustKey>(kTestTrust), &verifier, ConstSpan<AlgorithmId>(kAlgorithmIds), 0U}};

    void apply(const PolicyContent& content)
    {
        u8 body[2048];
        usize body_size = 0U;
        REQUIRE(write_policy_body(content, body, body_size) == PolicyError::None);
        const ConstSpan<u8> message(body, body_size);
        u8 tags[2][kTestSignatureBytes];
        KeySignature signatures[2];
        for (u32 k = 0U; k < 2U; ++k)
        {
            test_sign(kTestTrust[k].algorithm, kTestTrust[k].public_key, message, tags[k]);
            signatures[k] = {kTestTrust[k].id, kTestTrust[k].algorithm, ConstSpan<u8>(tags[k])};
        }
        u8 bytes[4096];
        usize size = 0U;
        REQUIRE(write_policy(message, ConstSpan<KeySignature>(signatures), bytes, size) == PolicyError::None);
        REQUIRE(store.apply(ConstSpan<u8>(bytes, size), now_seconds()) == PolicyError::None);
    }
};

void make_key(SigningKey& key, AlgorithmId algorithm, u8 salt)
{
    u8 seed[48];
    for (usize i = 0U; i < sizeof(seed); ++i)
    {
        seed[i] = static_cast<u8>(i + 1U + salt);
    }
    REQUIRE(key.from_seed(algorithm, ConstSpan<u8>(seed, signature_sizes(algorithm).seed)) == CryptoError::None);
}

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
        return ConstSpan<u8>(m_handed.data(), m_handed.size());
    }

    [[nodiscard]] bool holds() const { return !m_ticket.empty(); }

private:
    Bytes m_ticket;
    Bytes m_handed;
};

struct Setup
{
    ConstSpan<AlgorithmId> client_groups = kAllGroups;
    ConstSpan<AlgorithmId> server_groups = kAllGroups;
    ConstSpan<AlgorithmId> client_aeads = kAllAeads;
    ConstSpan<AlgorithmId> server_aeads = kAllAeads;
    ConstSpan<AlgorithmId> client_accepts = kAllSignatures;
    AlgorithmId server_key = AlgorithmId::MlDsa65;
    const PolicyStore* client_policy = nullptr;
    const PolicyStore* server_policy = nullptr;
    PolicyScope client_scope = PolicyScope::Everywhere;
    u64 client_key_records = 0U;
    ConstSpan<StringView> protocols;
};

struct Pair
{
    SigningKey server_key;
    TlsCredential credential;
    TlsContext client;
    TlsContext server;
    TlsSession client_session;
    TlsSession server_session;
};

void configure(Pair& pair, const Setup& setup)
{
    make_key(pair.server_key, setup.server_key, 0U);
    REQUIRE(pair.credential.init(pair.server_key, kServerName) == TlsError::None);
    const KeyId pin = pair.credential.key_id();

    TlsConfig client;
    client.role = TlsRole::Client;
    client.groups = setup.client_groups;
    client.signatures = setup.client_accepts;
    client.aeads = setup.client_aeads;
    client.pinned = ConstSpan<KeyId>(&pin, 1U);
    client.server_name = kServerName;
    client.key_update_records = setup.client_key_records;
    client.application_protocols = setup.protocols;
    client.policy = setup.client_policy;
    client.policy_scope = setup.client_scope;
    REQUIRE(pair.client.init(client) == TlsError::None);

    TlsConfig server;
    server.role = TlsRole::Server;
    server.groups = setup.server_groups;
    server.aeads = setup.server_aeads;
    server.credential = &pair.credential;
    server.server_name = kServerName;
    server.application_protocols = setup.protocols;
    server.policy = setup.server_policy;
    REQUIRE(pair.server.init(server) == TlsError::None);
}

Bytes drain(TlsSession& session)
{
    Bytes out;
    out.resize(session.pending());
    usize written = 0U;
    REQUIRE(session.take(Span<u8>(out.data(), out.size()), written) == TlsError::None);
    return out;
}

TlsError deliver(TlsSession& from, TlsSession& to)
{
    const Bytes bytes = drain(from);
    if (bytes.empty() || to.state() == TlsState::Failed)
    {
        return TlsError::None;
    }
    return to.receive(ConstSpan<u8>(bytes.data(), bytes.size()));
}

void pump(Pair& pair)
{
    for (int round = 0; round < 16 && (pair.client_session.pending() > 0U || pair.server_session.pending() > 0U);
         ++round)
    {
        (void)deliver(pair.client_session, pair.server_session);
        (void)deliver(pair.server_session, pair.client_session);
    }
}

// Starts both sessions (the client's tickets from `tickets`) and runs the handshake as far as it goes.
void connect(Pair& pair, Store* tickets = nullptr)
{
    REQUIRE(pair.client_session.init(pair.client, tickets) == TlsError::None);
    REQUIRE(pair.server_session.init(pair.server) == TlsError::None);
    pump(pair);
}

void establish(Pair& pair, Store* tickets = nullptr)
{
    connect(pair, tickets);
    REQUIRE(pair.client_session.state() == TlsState::Established);
    REQUIRE(pair.server_session.state() == TlsState::Established);
}

// Reads what waits until nothing does; the last result.
TlsError read_out(TlsSession& session, usize& total)
{
    total = 0U;
    u8 chunk[1024];
    for (;;)
    {
        usize written = 0U;
        const TlsError error = session.read(Span<u8>(chunk, sizeof(chunk)), written);
        total += written;
        if (error != TlsError::None || written == 0U)
        {
            return error;
        }
    }
}

// One message each way.
void exchange(Pair& pair)
{
    const u8 message[] = {'p', 'i', 'n', 'g'};
    REQUIRE(pair.client_session.send(ConstSpan<u8>(message)) == TlsError::None);
    REQUIRE(deliver(pair.client_session, pair.server_session) == TlsError::None);
    usize total = 0U;
    REQUIRE(read_out(pair.server_session, total) == TlsError::None);
    REQUIRE(total == sizeof(message));
    REQUIRE(pair.server_session.send(ConstSpan<u8>(message)) == TlsError::None);
    REQUIRE(deliver(pair.server_session, pair.client_session) == TlsError::None);
    REQUIRE(read_out(pair.client_session, total) == TlsError::None);
    REQUIRE(total == sizeof(message));
}
} // namespace

TEST_CASE("TLS policy: a session offers and accepts only what its generation allows, preferred first",
          "[crypto][tls][net-2]")
{
    Policy policy;
    policy.apply(accepting_all(1U));

    SECTION("without a policy a server's configured order decides; under it the preferred hybrid leads")
    {
        constexpr AlgorithmId kClassicalFirst[] = {AlgorithmId::X25519, AlgorithmId::X25519Mlkem768};
        Setup setup;
        setup.client_groups = kClassicalFirst;
        setup.server_groups = kClassicalFirst;
        Pair plain;
        configure(plain, setup);
        establish(plain);
        CHECK(plain.server_session.group() == AlgorithmId::X25519);
        CHECK(plain.client_session.policy_generation() == 0U);

        setup.server_policy = &policy.store;
        Pair governed;
        configure(governed, setup);
        establish(governed);
        CHECK(governed.server_session.group() == AlgorithmId::X25519Mlkem768);
        CHECK(governed.server_session.policy_generation() == 1U);
        CHECK(governed.client_session.policy_generation() == 0U);
        CHECK(governed.server_session.check_policy() == TlsError::None);
    }
    SECTION("a refused group leaves a client's offer and a server's acceptance")
    {
        PolicyContent content = accepting_all(2U);
        refuse(content, AlgorithmId::X25519Mlkem768);
        policy.apply(content);
        constexpr AlgorithmId kHybrid[] = {AlgorithmId::X25519Mlkem768};
        constexpr AlgorithmId kHybridOrP384[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::Secp384r1};
        Setup setup;
        setup.server_groups = kHybrid; // only the group the client's policy refuses
        setup.client_policy = &policy.store;
        Pair offered;
        configure(offered, setup);
        connect(offered);
        CHECK(offered.client_session.state() == TlsState::Failed);
        CHECK(offered.server_session.state() == TlsState::Failed);

        setup.client_groups = kHybrid; // a client without a policy offers only the group the server's refuses
        setup.server_groups = kHybridOrP384;
        setup.client_policy = nullptr;
        setup.server_policy = &policy.store;
        Pair accepted;
        configure(accepted, setup);
        connect(accepted);
        CHECK(accepted.client_session.state() == TlsState::Failed);
        CHECK(accepted.server_session.state() == TlsState::Failed);

        setup.client_groups = kAllGroups; // the next preferred hybrid instead
        setup.server_groups = kAllGroups;
        Pair other;
        configure(other, setup);
        establish(other);
        CHECK(other.server_session.group() == AlgorithmId::Secp256r1Mlkem768);
    }
    SECTION("a preferred AEAD leads a client's offer")
    {
        PolicyContent content = accepting_all(2U);
        prefer(content, AlgorithmId::Chacha20Poly1305);
        policy.apply(content);
        Setup setup; // the client configures AES-128-GCM first
        setup.client_policy = &policy.store;
        Pair pair;
        configure(pair, setup);
        establish(pair);
        CHECK(pair.client_session.aead() == AlgorithmId::Chacha20Poly1305);
    }
    SECTION("refused AEADs and peer signature algorithms drop out")
    {
        PolicyContent content = accepting_all(2U);
        refuse(content, AlgorithmId::Aes128Gcm);
        refuse(content, AlgorithmId::Aes256Gcm);
        refuse(content, AlgorithmId::Ed25519);
        policy.apply(content);
        Setup setup;
        setup.client_policy = &policy.store;
        Pair aead;
        configure(aead, setup);
        establish(aead);
        CHECK(aead.client_session.aead() == AlgorithmId::Chacha20Poly1305);
        CHECK(aead.client_session.peer_algorithm() == AlgorithmId::MlDsa65);

        setup.server_key = AlgorithmId::Ed25519; // a server whose key the client's policy now refuses
        Pair refused;
        configure(refused, setup);
        connect(refused);
        CHECK(refused.client_session.state() == TlsState::Failed);
        CHECK(refused.server_session.state() != TlsState::Established);
    }
}

TEST_CASE("TLS policy: a session the generation leaves nothing to run on is refused before a byte",
          "[crypto][tls][net-2]")
{
    Policy policy;
    PolicyContent content = accepting_all(1U);
    refuse(content, AlgorithmId::Chacha20Poly1305);
    refuse(content, AlgorithmId::MlDsa65);
    refuse(content, AlgorithmId::X25519);
    policy.apply(content);

    const auto refused_client = [&policy](const Setup& base)
    {
        Setup setup = base;
        setup.client_policy = &policy.store;
        Pair pair;
        configure(pair, setup);
        CHECK(pair.client_session.init(pair.client) == TlsError::PolicyRefused);
        CHECK(pair.client_session.state() == TlsState::Idle);
        CHECK(pair.client_session.pending() == 0U);
    };
    constexpr AlgorithmId kChacha[] = {AlgorithmId::Chacha20Poly1305};
    constexpr AlgorithmId kMlDsa[] = {AlgorithmId::MlDsa65};
    constexpr AlgorithmId kX25519[] = {AlgorithmId::X25519};
    Setup no_aead;
    no_aead.client_aeads = kChacha;
    refused_client(no_aead);
    Setup no_signature;
    no_signature.client_accepts = kMlDsa;
    refused_client(no_signature);
    Setup no_group;
    no_group.client_groups = kX25519;
    refused_client(no_group);

    // A server whose own credential is refused serves nothing; the store's next generation lets it serve again.
    Setup setup;
    setup.server_policy = &policy.store;
    Pair pair;
    configure(pair, setup);
    CHECK(pair.server_session.init(pair.server) == TlsError::PolicyRefused);
    policy.apply(accepting_all(2U));
    CHECK(pair.server_session.init(pair.server) == TlsError::None);
    CHECK(pair.server_session.policy_generation() == 2U);

    // A store that holds no generation yet refuses everything.
    Policy empty;
    Setup unset;
    unset.client_policy = &empty.store;
    Pair none;
    configure(none, unset);
    CHECK(none.client_session.init(none.client) == TlsError::PolicyRefused);
}

TEST_CASE("TLS policy: Cerid service contexts follow their own rules first", "[crypto][tls][net-2]")
{
    // Classical-only key exchange refused for Cerid services, still accepted everywhere else.
    Policy policy;
    PolicyContent content = accepting_all(1U);
    for (const AlgorithmId classical : {AlgorithmId::X25519, AlgorithmId::Secp256r1, AlgorithmId::Secp384r1})
    {
        content.rules[content.rule_count++] = {classical, 0U, 0U, 0U, 1U, PolicyScope::CeridServices};
    }
    policy.apply(content);
    constexpr AlgorithmId kClassical[] = {AlgorithmId::X25519, AlgorithmId::Secp256r1, AlgorithmId::Secp384r1};
    Setup setup;
    setup.client_groups = kClassical;
    setup.server_groups = kClassical;
    setup.client_policy = &policy.store;
    Pair everywhere;
    configure(everywhere, setup);
    establish(everywhere);
    CHECK(everywhere.client_session.group() == AlgorithmId::X25519);

    setup.client_scope = PolicyScope::CeridServices;
    Pair service;
    configure(service, setup);
    CHECK(service.client_session.init(service.client) == TlsError::PolicyRefused);
    setup.client_groups = kAllGroups;
    setup.server_groups = kAllGroups;
    Pair hybrid;
    configure(hybrid, setup);
    establish(hybrid);
    CHECK(algorithm_class(hybrid.client_session.group()) == AlgorithmClass::HybridKeyExchange);

    // A scope needs a store, and only the known scopes are scopes.
    Pair bad;
    TlsConfig config;
    config.role = TlsRole::Client;
    config.groups = kAllGroups;
    config.signatures = kAllSignatures;
    config.aeads = kAllAeads;
    const KeyId pin;
    config.pinned = ConstSpan<KeyId>(&pin, 1U);
    config.server_name = kServerName;
    config.policy_scope = PolicyScope::CeridServices;
    CHECK(bad.client.init(config) == TlsError::BadConfig);
    config.policy = &policy.store;
    config.policy_scope = static_cast<PolicyScope>(7U);
    CHECK(bad.client.init(config) == TlsError::BadConfig);
    config.policy_scope = PolicyScope::CeridServices;
    CHECK(bad.client.init(config) == TlsError::None);
    CHECK(tls_error_name(TlsError::PolicyRefused) == StringView("policy-refused"));
}

TEST_CASE("TLS policy: a ticket of a peer whose algorithm is now refused does not resume", "[crypto][tls][net-2]")
{
    Policy policy;
    policy.apply(accepting_all(1U));
    Setup setup;
    setup.server_key = AlgorithmId::Ed25519;
    setup.client_policy = &policy.store;
    Pair pair;
    Store tickets;
    configure(pair, setup);
    establish(pair, &tickets);
    usize read = 0U;
    REQUIRE(read_out(pair.client_session, read) == TlsError::None); // takes the server's NewSessionTicket
    REQUIRE(tickets.holds());

    PolicyContent content = accepting_all(2U);
    refuse(content, AlgorithmId::Ed25519);
    policy.apply(content);
    REQUIRE(pair.client_session.init(pair.client, &tickets) == TlsError::None);
    REQUIRE(pair.server_session.init(pair.server) == TlsError::None);
    pump(pair);
    CHECK(pair.client_session.state() == TlsState::Failed);
    CHECK(pair.server_session.state() != TlsState::Established);
}

TEST_CASE("TLS policy: a generation applied while a session runs reaches it at its next key update",
          "[crypto][tls][net-2]")
{
    // Both ends run on ChaCha20-Poly1305; the client's store then refuses it.
    constexpr AlgorithmId kChacha[] = {AlgorithmId::Chacha20Poly1305};
    Policy client_policy;
    Policy server_policy;
    client_policy.apply(accepting_all(1U));
    server_policy.apply(accepting_all(1U));
    Setup setup;
    setup.client_aeads = kChacha;
    setup.client_policy = &client_policy.store;
    setup.server_policy = &server_policy.store;
    PolicyContent disable = accepting_all(2U);
    refuse(disable, AlgorithmId::Chacha20Poly1305);

    SECTION("its own key update closes it")
    {
        Pair pair;
        configure(pair, setup);
        establish(pair);
        REQUIRE(pair.client_session.update_keys(false) == TlsError::None);
        client_policy.apply(disable);
        CHECK(pair.client_session.check_policy() == TlsError::PolicyRefused);
        exchange(pair); // the running keys stay until the next update
        CHECK(pair.client_session.update_keys(false) == TlsError::PolicyRefused);
        CHECK(pair.client_session.state() == TlsState::Closed);
        CHECK(pair.client_session.key_updates_sent() == 1U);
        REQUIRE(deliver(pair.client_session, pair.server_session) == TlsError::None);
        usize total = 0U;
        CHECK(read_out(pair.server_session, total) == TlsError::Closed);
        CHECK(total == 0U);
        // New sessions of the context have no AEAD left.
        CHECK(pair.client_session.init(pair.client) == TlsError::PolicyRefused);
    }
    SECTION("a key update send() makes closes it before any record")
    {
        setup.client_key_records = kTlsMinKeyUpdateRecords; // one data record per key
        Pair pair;
        configure(pair, setup);
        establish(pair);
        client_policy.apply(disable);
        const u8 message[2 * 16384] = {};
        CHECK(pair.client_session.send(ConstSpan<u8>(message)) == TlsError::PolicyRefused);
        CHECK(pair.client_session.state() == TlsState::Closed);
        CHECK(pair.client_session.key_updates_sent() == 0U);
        REQUIRE(deliver(pair.client_session, pair.server_session) == TlsError::None);
        usize total = 0U;
        CHECK(read_out(pair.server_session, total) == TlsError::Closed);
        CHECK(total == 0U);
    }
    SECTION("the peer's key update closes it after the data before it")
    {
        Pair pair;
        configure(pair, setup);
        establish(pair);
        client_policy.apply(disable);
        const u8 message[] = {'l', 'a', 's', 't'};
        REQUIRE(pair.server_session.send(ConstSpan<u8>(message)) == TlsError::None);
        REQUIRE(pair.server_session.update_keys(false) == TlsError::None);
        REQUIRE(deliver(pair.server_session, pair.client_session) == TlsError::None);
        u8 out[64];
        usize written = 0U;
        TlsError error = pair.client_session.read(Span<u8>(out, sizeof(out)), written);
        usize total = written;
        while (error == TlsError::None && written != 0U)
        {
            error = pair.client_session.read(Span<u8>(out + total, sizeof(out) - total), written);
            total += written;
        }
        CHECK(error == TlsError::PolicyRefused);
        CHECK(total == sizeof(message));
        CHECK(pair.client_session.state() == TlsState::Closed);
        CHECK(pair.client_session.pending() > 0U); // close_notify
    }
    SECTION("an allowed generation changes nothing")
    {
        Pair pair;
        configure(pair, setup);
        establish(pair);
        client_policy.apply(accepting_all(2U));
        CHECK(pair.client_session.update_keys(true) == TlsError::None);
        REQUIRE(deliver(pair.client_session, pair.server_session) == TlsError::None);
        exchange(pair);
        CHECK(pair.client_session.state() == TlsState::Established);
        CHECK(pair.client_session.policy_generation() == 1U); // the generation it started under
    }
}

TEST_CASE("TLS policy: a QUIC handshake runs under the policy and its transport checks before a key update",
          "[crypto][quic][net-2]")
{
    Policy policy;
    policy.apply(accepting_all(1U));
    constexpr AlgorithmId kClassicalFirst[] = {AlgorithmId::X25519, AlgorithmId::X25519Mlkem768};
    Setup setup;
    setup.client_groups = kClassicalFirst;
    setup.server_groups = kClassicalFirst;
    setup.client_policy = &policy.store;
    setup.server_policy = &policy.store;
    setup.protocols = kGame;
    Pair pair;
    configure(pair, setup);

    QuicHandshake client;
    QuicHandshake server;
    CHECK(client.check_policy() == TlsError::NotReady);
    REQUIRE(client.init(pair.client, ConstSpan<u8>(kClientParameters)) == TlsError::None);
    REQUIRE(server.init(pair.server, ConstSpan<u8>(kServerParameters)) == TlsError::None);
    constexpr QuicLevel kLevels[] = {QuicLevel::Initial, QuicLevel::Handshake, QuicLevel::Application};
    for (int round = 0; round < 16; ++round)
    {
        usize moved = 0U;
        for (QuicHandshake* from : {&client, &server})
        {
            QuicHandshake* to = from == &client ? &server : &client;
            for (const QuicLevel level : kLevels)
            {
                Bytes bytes;
                bytes.resize(from->pending(level));
                usize written = 0U;
                REQUIRE(from->take(level, Span<u8>(bytes.data(), bytes.size()), written) == TlsError::None);
                moved += written;
                if (written != 0U && to->state() == TlsState::Handshaking)
                {
                    (void)to->provide(level, ConstSpan<u8>(bytes.data(), written));
                }
            }
        }
        if (moved == 0U)
        {
            break;
        }
    }
    REQUIRE(client.state() == TlsState::Established);
    REQUIRE(server.state() == TlsState::Established);
    CHECK(server.group() == AlgorithmId::X25519Mlkem768);
    CHECK(client.policy_generation() == 1U);
    CHECK(client.check_policy() == TlsError::None);

    PolicyContent content = accepting_all(2U);
    refuse(content, AlgorithmId::X25519Mlkem768);
    policy.apply(content);
    CHECK(client.check_policy() == TlsError::PolicyRefused);
    CHECK(server.check_policy() == TlsError::PolicyRefused);
    CHECK(client.state() == TlsState::Established); // the transport closes the connection
}
