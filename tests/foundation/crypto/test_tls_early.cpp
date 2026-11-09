// crd-crypto -- early data (0-RTT) over TLS records and QUIC (NET.2, decision S17; docs/design/network-foundation.md
// #early-data): a resumed client's early data reaching its server once and only through read_early(), however the
// bytes arrive and are read; a first flight replayed to the server and a ticket offered twice resuming nothing (the
// server's ReplayFilter), while another server of the fleet, with a filter of its own, accepts the replay (the
// filter's scope); the provider's freshness check on the ticket age; a HelloRetryRequest, another application
// protocol and a server without early data refusing it, each followed by the client sending its bytes again after the
// handshake; the exact allowance a ticket states; the pins checked before the first early byte; adversarial early
// bytes; every configuration refusal; a ticket sealed under another early data context (another message schema)
// resuming without its early data and spending no filter entry; one server context and filter serving 0-RTT on several
// threads; RFC 9850 key logging of the early traffic secret by the client and the server that accepts it; and over
// QUIC,
// the client's 0-RTT keys meeting the server's with the transport parameters the client remembered, another early
// data context and a HelloRetryRequest refusing them (the client's keys zeroed), a replayed Initial, and the early
// data context's refusals.
//
// The independent peer (OpenSSL 4.0's early data, over records and through its QUIC TLS interface) is
// tests/foundation/crypto/interop/tls_interop.py's.

#include <crd/containers/array.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>

#include "tls_key_log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <openssl/ssl.h> // SSL_SESSION_set_time: a client's ticket aged at rest, for the provider's freshness check

#include <atomic>
#include <cstring>
#include <iterator>
#include <ctime>
#include <initializer_list>
#include <thread>

namespace
{
using crd::i64;
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
    AlgorithmId::MlDsa44,
    AlgorithmId::MlDsa65,
    AlgorithmId::MlDsa87,
    AlgorithmId::Ed25519,
    AlgorithmId::EcdsaSecp256r1Sha256,
    AlgorithmId::EcdsaSecp384r1Sha384,
};
constexpr AlgorithmId kAllAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};
constexpr QuicLevel kLevels[] = {QuicLevel::Initial, QuicLevel::Handshake, QuicLevel::Application};

constexpr const char* kServerName = "game.cerid.test";
constexpr StringView kGame[] = {"cerid-game/1"};

// The QUIC transport parameters each end sends, and two early data contexts a server could name.
constexpr u8 kClientParameters[] = {0x04U, 0x04U, 0x80U, 0x10U, 0x00U, 0x00U, 0x0FU, 0x04U, 0x0AU, 0x0BU, 0x0CU, 0x0DU};
constexpr u8 kServerParameters[] = {0x00U, 0x04U, 0x01U, 0x02U, 0x03U, 0x04U, 0x0FU, 0x02U, 0x51U, 0x52U};
constexpr u8 kContextA[] = {0x04U, 0x04U, 0x80U, 0x10U, 0x00U, 0x00U, 'h', '3'};
constexpr u8 kContextB[] = {0x04U, 0x04U, 0x80U, 0x08U, 0x00U, 0x00U, 'h', '3'};

constexpr u8 kBadRecordMac = 20U;

// Two application states a records server's early data could depend on: digests of two message schemas (S18).
constexpr u8 kSchemaContext[] = {0x5CU, 0x11U, 0xE7U, 0x02U, 0x9AU, 0x43U, 0x0DU, 0xB8U, 0x76U, 0x21U, 0xF0U,
                                 0x3EU, 0xC5U, 0x68U, 0x1FU, 0x94U, 0x2BU, 0xD7U, 0x80U, 0x4AU, 0x33U, 0xE1U,
                                 0x0CU, 0x9FU, 0x56U, 0xBAU, 0x17U, 0x6DU, 0xA2U, 0x48U, 0xF9U, 0x05U};
constexpr u8 kOtherSchema[]   = {0x5CU, 0x11U, 0xE7U, 0x02U, 0x9AU, 0x43U, 0x0DU, 0xB8U, 0x76U, 0x21U, 0xF0U,
                                 0x3EU, 0xC5U, 0x68U, 0x1FU, 0x94U, 0x2BU, 0xD7U, 0x80U, 0x4AU, 0x33U, 0xE1U,
                                 0x0CU, 0x9FU, 0x56U, 0xBAU, 0x17U, 0x6DU, 0xA2U, 0x48U, 0xF9U, 0x06U};

// A filter whose period is exactly what a context with the default ticket lifetime needs.
constexpr u64 kPeriod = kTlsDefaultTicketLifetime + kTlsMaxTicketAgeSkew;

u64 now()
{
    return static_cast<u64>(std::time(nullptr));
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

Bytes bytes_of(StringView text)
{
    Bytes out;
    out.resize(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

Bytes pattern(usize size, u8 salt)
{
    Bytes out;
    out.resize(size);
    for (usize i = 0U; i < size; ++i)
    {
        out[i] = static_cast<u8>(i * 31U + salt);
    }
    return out;
}

bool same(const Bytes& a, const Bytes& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

void append(Bytes& to, const u8* bytes, usize count)
{
    for (usize i = 0U; i < count; ++i)
    {
        to.push_back(bytes[i]);
    }
}

// Cerid's ticket record as a store holds it: its version, the early data allowed, the transport parameters and the
// provider's session.
struct Record
{
    u8 version = 0U;
    u32 early = 0U;
    Bytes parameters;
    Bytes session;
};

Record read_record(const Bytes& ticket)
{
    REQUIRE(ticket.size() > 7U);
    Record record;
    record.version = ticket[0];
    record.early = (u32{ticket[1]} << 24U) | (u32{ticket[2]} << 16U) | (u32{ticket[3]} << 8U) | ticket[4];
    const usize parameters = (usize{ticket[5]} << 8U) | ticket[6];
    REQUIRE(7U + parameters < ticket.size());
    append(record.parameters, ticket.data() + 7U, parameters);
    append(record.session, ticket.data() + 7U + parameters, ticket.size() - 7U - parameters);
    return record;
}

// A client's tickets: up to four, each handed out once with its binding; with `reuse` the store hands out its newest
// ticket and keeps it (a client that offers one ticket twice).
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
    }

    ConstSpan<u8> take(const TlsBinding& binding) noexcept override
    {
        m_handed.clear();
        for (usize i = m_count; i-- > 0U;)
        {
            if (m_bindings[i] == binding)
            {
                m_handed = m_tickets[i];
                if (!reuse)
                {
                    drop(i);
                }
                break;
            }
        }
        return ConstSpan<u8>(m_handed.data(), m_handed.size());
    }

    [[nodiscard]] usize size() const { return m_count; }
    [[nodiscard]] const Bytes& newest() const { return m_tickets[m_count - 1U]; }

    // Moves the creation time of the newest ticket's session `seconds` back: the client's clock then puts the ticket
    // that much older than its server's does (newer for a negative count).
    void age_newest(i64 seconds)
    {
        Bytes& ticket = m_tickets[m_count - 1U];
        const Record record = read_record(ticket);
        const bssl::UniquePtr<SSL_CTX> context(SSL_CTX_new(TLS_method()));
        const bssl::UniquePtr<SSL_SESSION> session(
            SSL_SESSION_from_bytes(record.session.data(), record.session.size(), context.get()));
        REQUIRE(session);
        const u64 created = SSL_SESSION_get_time(session.get());
        (void)SSL_SESSION_set_time(session.get(), static_cast<u64>(static_cast<i64>(created) - seconds));
        uint8_t* bytes = nullptr;
        size_t size = 0U;
        REQUIRE(SSL_SESSION_to_bytes(session.get(), &bytes, &size) == 1);
        Bytes out;
        append(out, ticket.data(), 7U + record.parameters.size());
        append(out, bytes, size);
        OPENSSL_free(bytes);
        ticket = out;
    }

    bool reuse = false;

private:
    void drop(usize i)
    {
        for (usize j = i; j + 1U < m_count; ++j)
        {
            m_tickets[j] = m_tickets[j + 1U];
            m_bindings[j] = m_bindings[j + 1U];
        }
        --m_count;
        m_tickets[m_count].clear();
    }

    Bytes m_tickets[kCapacity];
    TlsBinding m_bindings[kCapacity];
    usize m_count = 0U;
    Bytes m_handed;
};

struct Options
{
    bool client_early = true;
    bool server_early = true;
    ReplayFilter* filter = nullptr; // the server's (required when it accepts early data)
    ConstSpan<AlgorithmId> client_groups = kAllGroups;
    ConstSpan<AlgorithmId> server_groups = kAllGroups;
    ConstSpan<StringView> client_protocols;
    ConstSpan<StringView> server_protocols;
    bool mutual = false; // the server pins the client's key
    crd::crypto::test::KeyLog* client_log = nullptr; // RFC 9850 key logs of each context (decision S19)
    crd::crypto::test::KeyLog* server_log = nullptr;
};

// Both ends' credentials, keys and contexts; every server of a test shares one ticket key, as a fleet does.
struct Pair
{
    SigningKey server_key;
    SigningKey client_key;
    TlsCredential server_credential;
    TlsCredential client_credential;
    TlsContext client;
    TlsContext server;
    u8 ticket_key[kTlsTicketKeyBytes] = {};
};

void configure_server(Pair& pair, TlsContext& context, const Options& options)
{
    TlsConfig config;
    config.role = TlsRole::Server;
    config.groups = options.server_groups;
    config.aeads = kAllAeads;
    config.credential = &pair.server_credential;
    config.server_name = kServerName;
    config.ticket_key = ConstSpan<u8>(pair.ticket_key);
    config.application_protocols = options.server_protocols;
    config.early_data = options.server_early;
    config.early_data_filter = options.server_early ? options.filter : nullptr;
    const KeyId client_pin = pair.client_credential.key_id();
    if (options.mutual)
    {
        config.signatures = kAllSignatures;
        config.pinned = ConstSpan<KeyId>(&client_pin, 1U);
    }
    if (options.server_log != nullptr)
    {
        options.server_log->attach(config);
    }
    REQUIRE(context.init(config) == TlsError::None);
}

void configure(Pair& pair, const Options& options)
{
    make_key(pair.server_key, AlgorithmId::Ed25519, 0U);
    make_key(pair.client_key, AlgorithmId::Ed25519, 9U);
    REQUIRE(pair.server_credential.init(pair.server_key, kServerName) == TlsError::None);
    REQUIRE(pair.client_credential.init(pair.client_key, "player.cerid.test") == TlsError::None);
    for (usize i = 0U; i < kTlsTicketKeyBytes; ++i)
    {
        pair.ticket_key[i] = static_cast<u8>(0xA0U + i);
    }
    const KeyId server_pin = pair.server_credential.key_id();
    TlsConfig config;
    config.role = TlsRole::Client;
    config.groups = options.client_groups;
    config.signatures = kAllSignatures;
    config.aeads = kAllAeads;
    config.pinned = ConstSpan<KeyId>(&server_pin, 1U);
    config.server_name = kServerName;
    config.application_protocols = options.client_protocols;
    config.credential = options.mutual ? &pair.client_credential : nullptr;
    config.early_data = options.client_early;
    if (options.client_log != nullptr)
    {
        options.client_log->attach(config);
    }
    REQUIRE(pair.client.init(config) == TlsError::None);
    configure_server(pair, pair.server, options);
}

// ---- Records ----

// A server session of `context`, naming `early_context` when the context accepts early data.
TlsError serve(TlsSession& server, const TlsContext& context, ConstSpan<u8> early_context = kSchemaContext)
{
    return server.init(context, nullptr, {}, context.early_data() ? early_context : ConstSpan<u8>());
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

TlsError give(TlsSession& to, const Bytes& bytes)
{
    return bytes.empty() ? TlsError::None : to.receive(ConstSpan<u8>(bytes.data(), bytes.size()));
}

// What read() or read_early() returns until it returns nothing, `chunk` bytes at a time.
Bytes read_all(TlsSession& session, bool early, usize chunk = 4096U)
{
    Bytes out;
    u8 buffer[4096];
    for (;;)
    {
        usize written = 0U;
        const TlsError error = early ? session.read_early(Span<u8>(buffer, chunk), written)
                                     : session.read(Span<u8>(buffer, chunk), written);
        REQUIRE(error == TlsError::None);
        if (written == 0U)
        {
            return out;
        }
        append(out, buffer, written);
    }
}

// Flights back and forth until neither side sends; a server that took early data reads it as it comes into `early`.
void pump(TlsSession& client, TlsSession& server, Bytes& early)
{
    for (int round = 0;
         round < 16 && (client.pending() > 0U || server.pending() > 0U ||
                        (server.early_data() == TlsEarlyData::Accepted && server.state() == TlsState::Handshaking));
         ++round)
    {
        (void)give(server, drain(client));
        if (server.early_data() == TlsEarlyData::Accepted && server.state() == TlsState::Handshaking)
        {
            const Bytes more = read_all(server, true);
            append(early, more.data(), more.size());
        }
        if (client.state() != TlsState::Failed)
        {
            (void)give(client, drain(server));
        }
    }
}

void establish(TlsSession& client, TlsSession& server, Bytes& early)
{
    pump(client, server, early);
    REQUIRE(client.state() == TlsState::Established);
    REQUIRE(server.state() == TlsState::Established);
}

// 1-RTT data from the client; what the server reads of it with read().
Bytes send_once(TlsSession& client, TlsSession& server, const Bytes& message)
{
    REQUIRE(client.send(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
    REQUIRE(give(server, drain(client)) == TlsError::None);
    return read_all(server, false);
}

// The client takes in the tickets its server issued (they come after the handshake, or with a server's flight that
// accepted early data) by reading.
void read_tickets(TlsSession& client, TlsSession& server)
{
    if (server.pending() > 0U)
    {
        REQUIRE(give(client, drain(server)) == TlsError::None);
    }
    CHECK(read_all(client, false).empty());
}

// A full connection that leaves a ticket in `store`.
void first_connection(Pair& pair, Store& store)
{
    TlsSession client;
    TlsSession server;
    REQUIRE(client.init(pair.client, &store) == TlsError::None);
    REQUIRE(serve(server, pair.server) == TlsError::None);
    Bytes early;
    establish(client, server, early);
    CHECK_FALSE(client.resumed());
    CHECK(client.early_data() == TlsEarlyData::None);
    CHECK(server.early_data() == TlsEarlyData::None);
    const usize before = store.size();
    read_tickets(client, server);
    REQUIRE(store.size() == before + 1U);
}

// A resumed connection's start: the client offers its newest ticket and sends `message` as early data.
void start_early(TlsSession& client, TlsSession& server, const TlsContext& client_context,
                 const TlsContext& server_context, Store& store, const Bytes& message)
{
    REQUIRE(client.init(client_context, &store) == TlsError::None);
    REQUIRE(client.early_data() == TlsEarlyData::Offered);
    REQUIRE(client.send_early(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
    REQUIRE(serve(server, server_context) == TlsError::None);
}
} // namespace

TEST_CASE("TLS 0-RTT: a resumed client's early data reaches its server once, through read_early only, before its "
          "1-RTT data",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    Store store;
    first_connection(pair, store);

    // The ticket says what it allows: the provider's 14,336 bytes, no transport parameters over records.
    const Record record = read_record(store.newest());
    CHECK(record.version == 1U);
    CHECK(record.early == kTlsEarlyDataBytes);
    CHECK(record.parameters.empty());

    // How the server's caller reads: as the bytes come, only after the client's Finished arrived too, one byte at a
    // time, a client that sends no early byte, and a client whose first 1-RTT bytes come with its Finished (the read
    // that ends the handshake leaves them to read()).
    enum class Reading : u8
    {
        AsTheyCome,
        AfterFinished,
        ByteByByte,
        Nothing,
        WithOneRtt,
    };
    for (const Reading reading :
         {Reading::AsTheyCome, Reading::AfterFinished, Reading::ByteByByte, Reading::Nothing, Reading::WithOneRtt})
    {
        CAPTURE(static_cast<int>(reading));
        const Bytes message = reading == Reading::Nothing ? Bytes{} : bytes_of("move north; idempotent");
        TlsSession client;
        TlsSession server;
        REQUIRE(client.init(pair.client, &store) == TlsError::None);
        CHECK(client.early_data() == TlsEarlyData::Offered);
        CHECK(client.state() == TlsState::Handshaking);
        CHECK(client.early_data_room() == kTlsEarlyDataBytes);
        CHECK(client.send(ConstSpan<u8>(message.data(), message.size())) == TlsError::NotReady);
        REQUIRE(client.send_early(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
        CHECK(client.early_data_room() == kTlsEarlyDataBytes - message.size());
        REQUIRE(serve(server, pair.server) == TlsError::None);

        // The ClientHello and the early data in one flight: the server takes the early data, resumed, and stays in
        // the handshake; read() gives none of it.
        REQUIRE(give(server, drain(client)) == TlsError::None);
        CHECK(server.early_data() == TlsEarlyData::Accepted);
        CHECK(server.state() == TlsState::Handshaking);
        CHECK(server.resumed());
        u8 byte = 0U;
        usize written = 0U;
        CHECK(server.read(Span<u8>(&byte, 1U), written) == TlsError::NotReady);
        CHECK(written == 0U);
        CHECK(client.read_early(Span<u8>(&byte, 1U), written) == TlsError::NotReady);
        CHECK(server.send_early(ConstSpan<u8>(&byte, 1U)) == TlsError::NotReady);

        Bytes early;
        if (reading == Reading::AsTheyCome || reading == Reading::ByteByByte)
        {
            early = read_all(server, true, reading == Reading::ByteByByte ? 1U : 4096U);
            CHECK(server.state() == TlsState::Handshaking);
        }
        // The server's flight (and its half-RTT ticket) to the client, which finishes; then its EndOfEarlyData and
        // Finished to the server, which read_early() reads on to the end of the handshake.
        REQUIRE(give(client, drain(server)) == TlsError::None);
        CHECK(client.state() == TlsState::Established);
        CHECK(client.early_data() == TlsEarlyData::Accepted);
        CHECK(client.early_data_room() == 0U);
        const Bytes after = bytes_of("attack; not idempotent");
        if (reading == Reading::WithOneRtt)
        {
            REQUIRE(client.send(ConstSpan<u8>(after.data(), after.size())) == TlsError::None);
        }
        REQUIRE(give(server, drain(client)) == TlsError::None);
        CHECK(server.state() == TlsState::Handshaking);
        const Bytes rest = read_all(server, true, reading == Reading::ByteByByte ? 1U : 4096U);
        append(early, rest.data(), rest.size());
        CHECK(same(early, message));
        CHECK(server.state() == TlsState::Established);
        CHECK(server.early_data() == TlsEarlyData::Accepted);
        CHECK(server.resumed());
        CHECK(client.resumed());
        CHECK(read_all(server, true).empty());

        // 1-RTT data after it comes from read() only, and the half-RTT ticket allows early data again.
        CHECK(same(reading == Reading::WithOneRtt ? read_all(server, false) : send_once(client, server, after), after));
        CHECK(client.send_early(ConstSpan<u8>(after.data(), after.size())) == TlsError::NotReady);
        read_tickets(client, server);
        CHECK(read_record(store.newest()).early == kTlsEarlyDataBytes);
    }
}

TEST_CASE("TLS 0-RTT: a stream protocol's parameters reach its tickets, before or after them",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    const Bytes parameters = pattern(40U, 1U);
    const auto connect = [&](Store& store, TlsSession& client, TlsSession& server)
    {
        REQUIRE(client.init(pair.client, &store) == TlsError::None);
        client.hold_tickets();
        REQUIRE(serve(server, pair.server) == TlsError::None);
        Bytes early;
        establish(client, server, early);
    };

    SECTION("the ticket first: it waits for them")
    {
        Store store;
        TlsSession client;
        TlsSession server;
        connect(store, client, server);
        read_tickets(client, server);
        CHECK(store.size() == 0U);
        client.remember_parameters(ConstSpan<u8>(parameters.data(), parameters.size()));
        REQUIRE(store.size() == 1U);
        const Record record = read_record(store.newest());
        CHECK(record.early == kTlsEarlyDataBytes);
        CHECK(record.parameters == parameters);

        TlsSession resumed;
        REQUIRE(resumed.init(pair.client, &store) == TlsError::None);
        CHECK(resumed.early_data() == TlsEarlyData::Offered);
        const ConstSpan<u8> remembered = resumed.remembered_parameters();
        REQUIRE(remembered.size() == parameters.size());
        CHECK(std::memcmp(remembered.data(), parameters.data(), parameters.size()) == 0);
    }
    SECTION("the parameters first: the ticket goes straight in, and a later call changes nothing")
    {
        Store store;
        TlsSession client;
        TlsSession server;
        connect(store, client, server);
        client.remember_parameters(ConstSpan<u8>(parameters.data(), parameters.size()));
        CHECK(store.size() == 0U);
        const Bytes other = pattern(8U, 9U);
        client.remember_parameters(ConstSpan<u8>(other.data(), other.size()));
        read_tickets(client, server);
        REQUIRE(store.size() == 1U);
        CHECK(read_record(store.newest()).parameters == parameters);
    }
    SECTION("parameters that never come: the ticket goes with the session")
    {
        Store store;
        {
            TlsSession client;
            TlsSession server;
            connect(store, client, server);
            read_tickets(client, server);
        }
        CHECK(store.size() == 0U);
    }
    SECTION("parameters a record cannot hold: no ticket")
    {
        Store store;
        TlsSession client;
        TlsSession server;
        connect(store, client, server);
        read_tickets(client, server);
        const Bytes huge = pattern(kQuicMaxTransportParameterBytes + 1U, 3U);
        client.remember_parameters(ConstSpan<u8>(huge.data(), huge.size()));
        CHECK(store.size() == 0U);
    }
    SECTION("without hold_tickets tickets go straight to the store, and remember_parameters does nothing")
    {
        Store store;
        TlsSession client;
        TlsSession server;
        REQUIRE(client.init(pair.client, &store) == TlsError::None);
        REQUIRE(serve(server, pair.server) == TlsError::None);
        Bytes early;
        establish(client, server, early);
        client.remember_parameters(ConstSpan<u8>(parameters.data(), parameters.size()));
        read_tickets(client, server);
        REQUIRE(store.size() == 1U);
        CHECK(read_record(store.newest()).parameters.empty());
    }
    SECTION("a client without a store, and a server, hold nothing")
    {
        TlsSession client;
        TlsSession server;
        REQUIRE(client.init(pair.client) == TlsError::None);
        REQUIRE(serve(server, pair.server) == TlsError::None);
        client.hold_tickets();
        server.hold_tickets();
        Bytes early;
        establish(client, server, early);
        client.remember_parameters(ConstSpan<u8>(parameters.data(), parameters.size()));
        server.remember_parameters(ConstSpan<u8>(parameters.data(), parameters.size()));
        read_tickets(client, server);
        CHECK(client.remembered_parameters().empty());
    }
}

TEST_CASE("TLS 0-RTT: a server that accepted early data answers it at 0.5-RTT", "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    Store store;
    first_connection(pair, store);

    TlsSession client;
    TlsSession server;
    const Bytes request = bytes_of("what is the time");
    start_early(client, server, pair.client, pair.server, store, request);
    const Bytes answer = bytes_of("noon");
    // Before its ClientHello a server has accepted nothing, and a client has no 0.5-RTT at all.
    CHECK(server.send_half_rtt(ConstSpan<u8>(answer.data(), answer.size())) == TlsError::NotReady);
    CHECK(client.send_half_rtt(ConstSpan<u8>(answer.data(), answer.size())) == TlsError::NotReady);
    REQUIRE(give(server, drain(client)) == TlsError::None);
    REQUIRE(server.early_data() == TlsEarlyData::Accepted);
    REQUIRE(server.state() == TlsState::Handshaking);
    CHECK(read_all(server, true) == request);

    CHECK(server.send_half_rtt({}) == TlsError::None);
    REQUIRE(server.send_half_rtt(ConstSpan<u8>(answer.data(), answer.size())) == TlsError::None);
    const Bytes large = pattern(40000U, 5U);
    REQUIRE(server.send_half_rtt(ConstSpan<u8>(large.data(), large.size())) == TlsError::None);
    const Bytes too_much = pattern(kTlsMaxBufferedBytes + 1U, 6U);
    CHECK(server.send_half_rtt(ConstSpan<u8>(too_much.data(), too_much.size())) == TlsError::Full);
    usize room = kTlsMaxBufferedBytes - server.pending();
    const Bytes filler = pattern(room, 7U);
    CHECK(server.send_half_rtt(ConstSpan<u8>(filler.data(), filler.size())) == TlsError::Full);

    // The client has its answer before it has sent its Finished.
    REQUIRE(give(client, drain(server)) == TlsError::None);
    REQUIRE(client.state() == TlsState::Established);
    Bytes received = read_all(client, false);
    Bytes expected = answer;
    append(expected, large.data(), large.size());
    CHECK(received == expected);

    REQUIRE(give(server, drain(client)) == TlsError::None);
    CHECK(read_all(server, true).empty());
    CHECK(server.state() == TlsState::Established);
    CHECK(server.send_half_rtt(ConstSpan<u8>(answer.data(), answer.size())) == TlsError::NotReady);
}

TEST_CASE("TLS 0-RTT: a replayed first flight gets no early data from the server it reached, but another server of "
          "the fleet accepts it",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    ReplayFilter other_filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    REQUIRE(other_filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    TlsContext other_server; // another server of the fleet: the same ticket key, its own filter
    Options other = options;
    other.filter = &other_filter;
    configure_server(pair, other_server, other);
    Store store;
    first_connection(pair, store);

    const Bytes message = bytes_of("buy item 7");
    TlsSession client;
    TlsSession server;
    start_early(client, server, pair.client, pair.server, store, message);
    const Bytes flight = drain(client);
    REQUIRE(give(server, flight) == TlsError::None);
    REQUIRE(server.early_data() == TlsEarlyData::Accepted);
    CHECK(same(read_all(server, true), message));

    // The attacker sends the same bytes to the same server: its filter has seen the ticket, so the handshake runs in
    // full, without the early data (the attacker cannot finish it).
    TlsSession replayed;
    REQUIRE(serve(replayed, pair.server) == TlsError::None);
    REQUIRE(give(replayed, flight) == TlsError::None);
    CHECK(replayed.early_data() == TlsEarlyData::Replayed);
    CHECK(replayed.state() == TlsState::Handshaking);
    usize written = 0U;
    u8 byte = 0U;
    CHECK(replayed.read_early(Span<u8>(&byte, 1U), written) == TlsError::NotReady);
    CHECK(replayed.read(Span<u8>(&byte, 1U), written) == TlsError::NotReady);

    // Another server's filter is its own: it accepts the replay (RFC 9846 section 8's zones), which is why early data
    // carries only operations their schema declares idempotent.
    TlsSession elsewhere;
    REQUIRE(serve(elsewhere, other_server) == TlsError::None);
    REQUIRE(give(elsewhere, flight) == TlsError::None);
    CHECK(elsewhere.early_data() == TlsEarlyData::Accepted);
    CHECK(same(read_all(elsewhere, true), message));

    // The genuine connection goes on.
    Bytes early;
    establish(client, server, early);
    CHECK(early.empty());
    CHECK(client.early_data() == TlsEarlyData::Accepted);
}

TEST_CASE("TLS 0-RTT: a ticket offered twice resumes nothing the second time; the client sends its bytes again, and "
          "its next ticket allows early data",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    Store store;
    first_connection(pair, store);
    store.reuse = true;

    const Bytes message = bytes_of("open door 3");
    for (int use = 0; use < 2; ++use)
    {
        CAPTURE(use);
        TlsSession client;
        TlsSession server;
        start_early(client, server, pair.client, pair.server, store, message);
        Bytes early;
        establish(client, server, early);
        if (use == 0)
        {
            CHECK(server.early_data() == TlsEarlyData::Accepted);
            CHECK(client.early_data() == TlsEarlyData::Accepted);
            CHECK(same(early, message));
            CHECK(server.resumed());
            continue;
        }
        // The second use: a full handshake (the certificate authenticates the server again), no early byte reached
        // the server's caller, and the client sends the bytes again as 1-RTT data.
        CHECK(server.early_data() == TlsEarlyData::Replayed);
        CHECK(client.early_data() == TlsEarlyData::Rejected);
        CHECK(early.empty());
        CHECK_FALSE(server.resumed());
        CHECK_FALSE(client.resumed());
        CHECK(same(send_once(client, server, message), message));

        // The full handshake's ticket allows early data, and the next connection uses it.
        store.reuse = false;
        read_tickets(client, server);
        REQUIRE(read_record(store.newest()).early == kTlsEarlyDataBytes);
        TlsSession next_client;
        TlsSession next_server;
        start_early(next_client, next_server, pair.client, pair.server, store, message);
        Bytes next_early;
        establish(next_client, next_server, next_early);
        CHECK(next_server.early_data() == TlsEarlyData::Accepted);
        CHECK(same(next_early, message));
    }
}

TEST_CASE("TLS 0-RTT: the provider refuses early data whose ticket age is more than a minute off",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    Store store;
    first_connection(pair, store);

    // The client's clock puts the ticket that much older than its server's does: the skew is that, less at most a
    // second the test takes, against the provider's bound of kTlsMaxTicketAgeSkew either way.
    struct Age
    {
        i64 seconds;
        TlsEarlyData server;
    };
    constexpr Age ages[] = {
        {30, TlsEarlyData::Accepted}, {57, TlsEarlyData::Accepted}, {63, TlsEarlyData::Stale},
        {120, TlsEarlyData::Stale},   {3600, TlsEarlyData::Stale},
    };
    const Bytes message = bytes_of("ping");
    for (const Age& age : ages)
    {
        CAPTURE(age.seconds);
        store.age_newest(age.seconds);
        TlsSession client;
        TlsSession server;
        start_early(client, server, pair.client, pair.server, store, message);
        Bytes early;
        establish(client, server, early);
        CHECK(server.early_data() == age.server);
        CHECK(server.resumed());
        if (age.server == TlsEarlyData::Accepted)
        {
            CHECK(client.early_data() == TlsEarlyData::Accepted);
            CHECK(same(early, message));
        }
        else
        {
            CHECK(client.early_data() == TlsEarlyData::Rejected);
            CHECK(early.empty());
            CHECK(same(send_once(client, server, message), message));
        }
        read_tickets(client, server);
    }

    // A client whose clock puts the ticket in the future offers it to no one (the provider refuses a ticket from the
    // future), so it neither resumes nor sends early data.
    store.age_newest(-120);
    TlsSession client;
    TlsSession server;
    REQUIRE(client.init(pair.client, &store) == TlsError::None);
    CHECK(client.early_data() == TlsEarlyData::None);
    REQUIRE(serve(server, pair.server) == TlsError::None);
    Bytes early;
    establish(client, server, early);
    CHECK_FALSE(client.resumed());
    CHECK(server.early_data() == TlsEarlyData::None);
}

TEST_CASE("TLS 0-RTT: a HelloRetryRequest, another application protocol and a server without early data refuse it, "
          "and the client sends again",
          "[crypto][tls][early][net-2]")
{
    // The client shares X25519MLKEM768 and X25519; the server prefers SecP384r1MLKEM1024, so every hello is retried.
    constexpr AlgorithmId client_groups[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519,
                                             AlgorithmId::Secp384r1Mlkem1024};
    constexpr AlgorithmId server_groups[] = {AlgorithmId::Secp384r1Mlkem1024, AlgorithmId::X25519Mlkem768};
    constexpr StringView offered[] = {"cerid-a", "cerid-b"};
    constexpr StringView first_a[] = {"cerid-a"};
    constexpr StringView first_b[] = {"cerid-b", "cerid-a"};
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);

    enum class Refusal : u8
    {
        HelloRetry,
        Protocol,
        ServerWithout,
    };
    for (const Refusal refusal : {Refusal::HelloRetry, Refusal::Protocol, Refusal::ServerWithout})
    {
        CAPTURE(static_cast<int>(refusal));
        Pair pair;
        Options options;
        options.filter = &filter;
        if (refusal == Refusal::HelloRetry)
        {
            options.client_groups = client_groups;
            options.server_groups = server_groups;
        }
        else
        {
            options.client_protocols = offered;
            options.server_protocols = first_a;
        }
        configure(pair, options);
        Store store;
        first_connection(pair, store);
        // The second connection's server: the same one, one that now prefers the other protocol, or one without
        // early data; all three share the ticket key.
        TlsContext second;
        Options changed = options;
        if (refusal == Refusal::Protocol)
        {
            changed.server_protocols = first_b;
        }
        if (refusal == Refusal::ServerWithout)
        {
            changed.server_early = false;
        }
        configure_server(pair, second, changed);

        const Bytes message = bytes_of("chat: hello");
        TlsSession client;
        TlsSession server;
        start_early(client, server, pair.client, second, store, message);
        Bytes early;
        establish(client, server, early);
        CHECK(server.early_data() == TlsEarlyData::Rejected);
        CHECK(client.early_data() == TlsEarlyData::Rejected);
        CHECK(early.empty());
        CHECK(server.resumed());
        CHECK(server.hello_retry() == (refusal == Refusal::HelloRetry));
        CHECK(same(send_once(client, server, message), message));
        if (refusal == Refusal::Protocol)
        {
            CHECK(client.application_protocol() == StringView("cerid-b"));
        }
    }
}

TEST_CASE("TLS 0-RTT: early data stays off unless both ends configure it, and a ticket states what it allows",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    for (const bool client_early : {false, true})
    {
        for (const bool server_early : {false, true})
        {
            CAPTURE(client_early, server_early);
            Pair pair;
            Options options;
            options.client_early = client_early;
            options.server_early = server_early;
            options.filter = &filter;
            configure(pair, options);
            Store store;
            first_connection(pair, store);
            CHECK(read_record(store.newest()).early == (server_early ? kTlsEarlyDataBytes : 0U));
            TlsSession client;
            TlsSession server;
            REQUIRE(client.init(pair.client, &store) == TlsError::None);
            const TlsEarlyData expected = client_early && server_early ? TlsEarlyData::Offered : TlsEarlyData::None;
            CHECK(client.early_data() == expected);
            CHECK(client.early_data_room() == (expected == TlsEarlyData::Offered ? kTlsEarlyDataBytes : 0U));
            u8 byte = 1U;
            CHECK(client.send_early(ConstSpan<u8>(&byte, 1U)) ==
                  (expected == TlsEarlyData::Offered ? TlsError::None : TlsError::NotReady));
            REQUIRE(serve(server, pair.server) == TlsError::None);
            Bytes early;
            establish(client, server, early);
            CHECK(client.resumed());
            CHECK(client.early_data() ==
                  (expected == TlsEarlyData::Offered ? TlsEarlyData::Accepted : TlsEarlyData::None));
            CHECK(server.early_data() == client.early_data());
        }
    }
}

TEST_CASE("TLS 0-RTT: a ticket's allowance is exact, whole messages or nothing", "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    Store store;
    first_connection(pair, store);

    TlsSession client;
    TlsSession server;
    REQUIRE(client.init(pair.client, &store) == TlsError::None);
    const Bytes all = pattern(kTlsEarlyDataBytes + 1U, 3U);
    const usize hello = client.pending();
    CHECK(client.send_early(ConstSpan<u8>(all.data(), all.size())) == TlsError::Full);
    CHECK(client.pending() == hello);
    CHECK(client.early_data_room() == kTlsEarlyDataBytes);
    REQUIRE(client.send_early(ConstSpan<u8>(all.data(), 100U)) == TlsError::None);
    CHECK(client.send_early(ConstSpan<u8>(all.data() + 100U, kTlsEarlyDataBytes - 99U)) == TlsError::Full);
    REQUIRE(client.send_early(ConstSpan<u8>(all.data() + 100U, kTlsEarlyDataBytes - 100U)) == TlsError::None);
    CHECK(client.early_data_room() == 0U);
    CHECK(client.send_early(ConstSpan<u8>(all.data(), 1U)) == TlsError::Full);
    CHECK(client.send_early({}) == TlsError::None);

    REQUIRE(serve(server, pair.server) == TlsError::None);
    Bytes early;
    establish(client, server, early);
    CHECK(server.early_data() == TlsEarlyData::Accepted);
    REQUIRE(early.size() == kTlsEarlyDataBytes);
    CHECK(std::memcmp(early.data(), all.data(), early.size()) == 0);
}

TEST_CASE("TLS 0-RTT: under mutual authentication the server knows its client's key before the first early byte",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    options.mutual = true;
    configure(pair, options);
    Store store;
    first_connection(pair, store);

    const Bytes message = bytes_of("sign in");
    TlsSession client;
    TlsSession server;
    start_early(client, server, pair.client, pair.server, store, message);
    REQUIRE(give(server, drain(client)) == TlsError::None);
    REQUIRE(server.early_data() == TlsEarlyData::Accepted);
    CHECK(server.peer_key() == pair.client_credential.key_id());
    CHECK(server.peer_algorithm() == AlgorithmId::Ed25519);
    CHECK(same(read_all(server, true), message));
    Bytes early;
    establish(client, server, early);
    CHECK(server.peer_key() == pair.client_credential.key_id());
}

TEST_CASE("TLS 0-RTT: a ticket sealed under another early data context resumes without its early data and spends "
          "no filter entry",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    options.mutual = true;
    configure(pair, options);
    Store store;
    first_connection(pair, store); // its ticket sealed under kSchemaContext

    // The server now runs another schema: the resumption stands, its 0-RTT does not, and the client sends again.
    const Bytes message = bytes_of("look around");
    TlsSession client;
    TlsSession server;
    REQUIRE(client.init(pair.client, &store) == TlsError::None);
    REQUIRE(client.send_early(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
    const Bytes flight = drain(client);
    REQUIRE(serve(server, pair.server, kOtherSchema) == TlsError::None);
    REQUIRE(give(server, flight) == TlsError::None);
    CHECK(server.early_data() == TlsEarlyData::Rejected);
    usize written = 0U;
    u8    byte    = 0U;
    CHECK(server.read_early(Span<u8>(&byte, 1U), written) == TlsError::NotReady);
    Bytes early;
    establish(client, server, early);
    CHECK(early.empty());
    CHECK(client.early_data() == TlsEarlyData::Rejected);
    CHECK(server.resumed());
    CHECK(server.peer_key() == pair.client_credential.key_id()); // the resumption kept the ticket's authentication
    CHECK(same(send_once(client, server, message), message));

    // The refusal spent no filter entry: the same first flight reaching a server under the ticket's own context is
    // the ticket's first use there.
    TlsSession same_schema;
    REQUIRE(serve(same_schema, pair.server) == TlsError::None);
    REQUIRE(give(same_schema, flight) == TlsError::None);
    CHECK(same_schema.early_data() == TlsEarlyData::Accepted);
    CHECK(same(read_all(same_schema, true), message));

    // The resumed connection's ticket is sealed under the new context, which allows 0-RTT again; the next ticket, also
    // sealed under it, is refused by a server back on the old context.
    read_tickets(client, server);
    REQUIRE(store.size() == 1U);
    CHECK(read_record(store.newest()).early == kTlsEarlyDataBytes);
    for (const bool same_context : {true, false})
    {
        CAPTURE(same_context);
        TlsSession next_client;
        TlsSession next_server;
        REQUIRE(next_client.init(pair.client, &store) == TlsError::None);
        REQUIRE(next_client.send_early(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
        REQUIRE(serve(next_server, pair.server, same_context ? ConstSpan<u8>(kOtherSchema)
                                                             : ConstSpan<u8>(kSchemaContext)) == TlsError::None);
        Bytes next_early;
        establish(next_client, next_server, next_early);
        const TlsEarlyData expected = same_context ? TlsEarlyData::Accepted : TlsEarlyData::Rejected;
        CHECK(next_server.early_data() == expected);
        CHECK(next_client.early_data() == expected);
        CHECK(same(next_early, same_context ? message : Bytes()));
        CHECK(next_server.resumed());
        read_tickets(next_client, next_server);
    }
}

TEST_CASE("TLS 0-RTT: an early data context only where early data is accepted, within its bound",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(16U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    TlsContext without; // a server without early data
    Options plain = options;
    plain.server_early = false;
    configure_server(pair, without, plain);
    CHECK(pair.server.early_data());
    CHECK(pair.client.early_data());
    CHECK_FALSE(without.early_data());
    Bytes large;
    large.resize(kTlsMaxEarlyDataContextBytes + 1U);

    TlsSession session;
    CHECK(session.init(pair.server) == TlsError::BadConfig); // a server with early data names one
    CHECK(session.state() == TlsState::Idle);
    CHECK(session.init(pair.server, nullptr, {}, ConstSpan<u8>(large.data(), large.size())) == TlsError::BadConfig);
    CHECK(session.init(pair.server, nullptr, {}, ConstSpan<u8>(large.data(), kTlsMaxEarlyDataContextBytes)) ==
          TlsError::None);
    CHECK(session.init(without, nullptr, {}, ConstSpan<u8>(kSchemaContext)) == TlsError::BadConfig);
    CHECK(session.init(without) == TlsError::None);
    CHECK(session.init(pair.client, nullptr, {}, ConstSpan<u8>(kSchemaContext)) == TlsError::BadConfig);
    CHECK(session.state() == TlsState::Idle);
    CHECK(session.init(pair.client) == TlsError::None);
}

TEST_CASE("TLS 0-RTT: early bytes changed on the way fail the session with bad_record_mac",
          "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);
    Store store;
    first_connection(pair, store);

    TlsSession client;
    TlsSession server;
    REQUIRE(client.init(pair.client, &store) == TlsError::None);
    const usize hello = client.pending();
    const Bytes message = pattern(64U, 5U);
    REQUIRE(client.send_early(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
    Bytes flight = drain(client);
    REQUIRE(flight.size() > hello + 10U);
    flight[flight.size() - 10U] ^= 0x40U; // inside the early record's ciphertext
    REQUIRE(serve(server, pair.server) == TlsError::None);
    REQUIRE(give(server, flight) == TlsError::None);
    REQUIRE(server.early_data() == TlsEarlyData::Accepted);
    u8 buffer[128];
    usize written = 0U;
    CHECK(server.read_early(Span<u8>(buffer), written) == TlsError::Failed);
    CHECK(written == 0U);
    CHECK(server.state() == TlsState::Failed);
    CHECK(server.alert_sent().present);
    CHECK(server.alert_sent().description == kBadRecordMac);
}

TEST_CASE("TLS 0-RTT: configurations without a fit filter are refused", "[crypto][tls][early][net-2]")
{
    SigningKey key;
    TlsCredential credential;
    make_key(key, AlgorithmId::Ed25519, 0U);
    REQUIRE(credential.init(key, kServerName) == TlsError::None);
    ReplayFilter fit;
    ReplayFilter short_period;
    ReplayFilter unready;
    REQUIRE(fit.init(16U, kPeriod, now()) == CryptoError::None);
    REQUIRE(short_period.init(16U, kPeriod - 1U, now()) == CryptoError::None);
    const KeyId pin = credential.key_id();

    TlsConfig server;
    server.role = TlsRole::Server;
    server.groups = kAllGroups;
    server.aeads = kAllAeads;
    server.credential = &credential;
    TlsConfig client;
    client.role = TlsRole::Client;
    client.groups = kAllGroups;
    client.signatures = kAllSignatures;
    client.aeads = kAllAeads;
    client.pinned = ConstSpan<KeyId>(&pin, 1U);
    client.server_name = kServerName;

    struct Case
    {
        const TlsConfig* base;
        bool early;
        ReplayFilter* filter;
        u32 lifetime;
        TlsError expected;
    };
    const Case cases[] = {
        {&server, true, &fit, 0U, TlsError::None},
        {&server, true, nullptr, 0U, TlsError::BadConfig},       // a server with early data needs a filter
        {&server, true, &unready, 0U, TlsError::BadConfig},      // that is ready
        {&server, true, &short_period, 0U, TlsError::BadConfig}, // and covers the lifetime and the skew
        {&server, true, &short_period, 3600U, TlsError::None},   // a shorter lifetime needs a shorter period
        {&server, true, &fit, kTlsMaxTicketLifetime, TlsError::BadConfig},
        {&server, false, &fit, 0U, TlsError::BadConfig}, // a filter without early data
        {&client, true, nullptr, 0U, TlsError::None},
        {&client, true, &fit, 0U, TlsError::BadConfig}, // a client has no filter
        {&client, false, &fit, 0U, TlsError::BadConfig},
    };
    for (const Case& test : cases)
    {
        TlsConfig config = *test.base;
        config.early_data = test.early;
        config.early_data_filter = test.filter;
        config.ticket_lifetime = test.lifetime;
        TlsContext context;
        CHECK(context.init(config) == test.expected);
        CHECK(context.ready() == (test.expected == TlsError::None));
    }
    CHECK(tls_early_data_name(TlsEarlyData::None) == StringView("none"));
    CHECK(tls_early_data_name(TlsEarlyData::Offered) == StringView("offered"));
    CHECK(tls_early_data_name(TlsEarlyData::Accepted) == StringView("accepted"));
    CHECK(tls_early_data_name(TlsEarlyData::Rejected) == StringView("rejected"));
    CHECK(tls_early_data_name(TlsEarlyData::Replayed) == StringView("replayed"));
    CHECK(tls_early_data_name(TlsEarlyData::Stale) == StringView("stale"));
}

TEST_CASE("TLS 0-RTT: one server context and filter serve early data on several threads", "[crypto][tls][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(4096U, kPeriod, now()) == CryptoError::None);
    Pair pair;
    Options options;
    options.filter = &filter;
    configure(pair, options);

    // Each thread resumes its own client's tickets against the one server context; Catch2's assertions stay on this
    // thread, so a thread counts what goes wrong.
    constexpr int    thread_count = 4;
    constexpr int    connections  = 12;
    std::atomic<int> accepted{0};
    std::atomic<int> wrong{0};
    std::thread      threads[thread_count];
    for (int t = 0; t < thread_count; ++t)
    {
        threads[t] = std::thread(
            [&pair, &accepted, &wrong, t]()
            {
                Store store;
                u8 buffer[kTlsMaxBufferedBytes];
                // Moves every pending byte from one session to the other; false when anything fails.
                const auto move = [&buffer](TlsSession& from, TlsSession& to)
                {
                    usize written = 0U;
                    return from.take(Span<u8>(buffer), written) == TlsError::None &&
                           (written == 0U || to.receive(ConstSpan<u8>(buffer, written)) == TlsError::None);
                };
                for (int c = 0; c < connections; ++c)
                {
                    TlsSession client;
                    TlsSession server;
                    const Bytes message = pattern(32U + static_cast<usize>(c), static_cast<u8>(t * 16 + c));
                    bool fine = client.init(pair.client, &store) == TlsError::None &&
                                serve(server, pair.server) == TlsError::None;
                    const bool offered = client.early_data() == TlsEarlyData::Offered;
                    fine = fine && (!offered ||
                                    client.send_early(ConstSpan<u8>(message.data(), message.size())) == TlsError::None);
                    Bytes early;
                    for (int round = 0; fine && round < 8 && server.state() != TlsState::Established; ++round)
                    {
                        fine = move(client, server);
                        usize written = 0U;
                        while (fine && server.early_data() == TlsEarlyData::Accepted &&
                               server.state() == TlsState::Handshaking &&
                               server.read_early(Span<u8>(buffer), written) == TlsError::None && written != 0U)
                        {
                            append(early, buffer, written);
                        }
                        fine = fine && move(server, client);
                    }
                    usize written = 0U;
                    fine = fine && move(server, client) && client.read(Span<u8>(buffer), written) == TlsError::None;
                    if (!fine || client.state() != TlsState::Established || server.state() != TlsState::Established ||
                        offered != (server.early_data() == TlsEarlyData::Accepted) ||
                        (offered && !same(early, message)) || (c != 0) != offered)
                    {
                        ++wrong;
                    }
                    accepted += offered ? 1 : 0;
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(wrong.load() == 0);
    CHECK(accepted.load() == thread_count * (connections - 1));
}

// ---- QUIC ----

namespace
{
struct QuicPair
{
    Pair pair;
    QuicHandshake client;
    QuicHandshake server;
};

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

void deliver(QuicHandshake& from, QuicHandshake& to)
{
    for (const QuicLevel level : kLevels)
    {
        const Bytes bytes = drain(from, level);
        if (!bytes.empty() && (to.state() == TlsState::Handshaking || to.state() == TlsState::Established))
        {
            (void)to.provide(level, ConstSpan<u8>(bytes.data(), bytes.size()));
        }
    }
}

void quic_establish(QuicHandshake& client, QuicHandshake& server)
{
    for (int round = 0; round < 16 && (pending(client) > 0U || pending(server) > 0U); ++round)
    {
        deliver(client, server);
        deliver(server, client);
    }
    REQUIRE(client.state() == TlsState::Established);
    REQUIRE(server.state() == TlsState::Established);
}

void quic_configure(QuicPair& quic, ReplayFilter& filter)
{
    Options options;
    options.filter = &filter;
    options.client_protocols = kGame;
    options.server_protocols = kGame;
    configure(quic.pair, options);
}

// A 0-RTT packet (a long header of type 0-RTT, RFC 9000 section 17.2.3) sealed and protected by the client's Early
// keys opens at the server to the same payload: both ends hold the same secret.
void check_early_keys(QuicHandshake& client, QuicHandshake& server, QuicVersion version)
{
    QuicPacketKey seal;
    QuicPacketKey open;
    QuicHeaderKey protect;
    QuicHeaderKey unprotect;
    REQUIRE(client.derive_keys(QuicLevel::Early, QuicSide::Client, version, seal, protect) == TlsError::None);
    REQUIRE(server.derive_keys(QuicLevel::Early, QuicSide::Client, version, open, unprotect) == TlsError::None);
    CHECK_FALSE(client.has_keys(QuicLevel::Early, QuicSide::Client));
    CHECK_FALSE(server.has_keys(QuicLevel::Early, QuicSide::Client));
    const u8 header[] = {0xD1U, 0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 0x00U, 0x40U, 0x32U};
    Bytes packet;
    append(packet, header, sizeof(header));
    const usize number_at = packet.size();
    packet.push_back(0x00U);
    packet.push_back(0x07U);
    const usize header_bytes = packet.size();
    for (usize i = 0U; i < 32U + kQuicTagBytes; ++i)
    {
        packet.push_back(static_cast<u8>(i * 11U + 1U));
    }
    const Bytes plain = packet;
    REQUIRE(seal.seal(7U, Span<u8>(packet.data(), packet.size()), header_bytes) == CryptoError::None);
    REQUIRE(protect.protect(Span<u8>(packet.data(), packet.size()), number_at) == CryptoError::None);
    usize number_bytes = 0U;
    u64 truncated = 0U;
    REQUIRE(unprotect.unprotect(Span<u8>(packet.data(), packet.size()), number_at, number_bytes, truncated) ==
            CryptoError::None);
    CHECK(truncated == 7U);
    REQUIRE(open.open(7U, Span<u8>(packet.data(), packet.size()), header_bytes) == CryptoError::None);
    CHECK(std::memcmp(packet.data(), plain.data(), packet.size() - kQuicTagBytes) == 0);
}

// A first QUIC connection under `context` that leaves a ticket in `store`.
void quic_first(QuicPair& quic, Store& store, ConstSpan<u8> context)
{
    REQUIRE(quic.client.init(quic.pair.client, ConstSpan<u8>(kClientParameters), &store) == TlsError::None);
    REQUIRE(quic.server.init(quic.pair.server, ConstSpan<u8>(kServerParameters), nullptr, {}, context) ==
            TlsError::None);
    CHECK(quic.client.early_data() == TlsEarlyData::None);
    CHECK(quic.client.remembered_transport_parameters().empty());
    quic_establish(quic.client, quic.server);
    REQUIRE(store.size() == 1U);
}
} // namespace

TEST_CASE("QUIC 0-RTT: the client's early keys meet the server's, with the transport parameters it remembered",
          "[crypto][quic-handshake][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    for (const QuicVersion version : {QuicVersion::V1, QuicVersion::V2})
    {
        CAPTURE(static_cast<u32>(version));
        QuicPair quic;
        quic_configure(quic, filter);
        Store store;
        quic_first(quic, store, ConstSpan<u8>(kContextA));

        // The ticket allows any amount (RFC 9001 section 4.6.1) and carries the server's transport parameters.
        const Record record = read_record(store.newest());
        CHECK(record.early == kQuicEarlyDataBytes);
        CHECK(same(record.parameters,
                   []
                   {
                       Bytes parameters;
                       append(parameters, kServerParameters, sizeof(kServerParameters));
                       return parameters;
                   }()));

        QuicHandshake client;
        QuicHandshake server;
        REQUIRE(client.init(quic.pair.client, ConstSpan<u8>(kClientParameters), &store) == TlsError::None);
        CHECK(client.early_data() == TlsEarlyData::Offered);
        CHECK(client.has_keys(QuicLevel::Early, QuicSide::Client));
        CHECK_FALSE(client.has_keys(QuicLevel::Early, QuicSide::Server));
        const ConstSpan<u8> remembered = client.remembered_transport_parameters();
        REQUIRE(remembered.size() == sizeof(kServerParameters));
        CHECK(std::memcmp(remembered.data(), kServerParameters, remembered.size()) == 0);

        REQUIRE(server.init(quic.pair.server, ConstSpan<u8>(kServerParameters), nullptr, {},
                            ConstSpan<u8>(kContextA)) == TlsError::None);
        deliver(client, server);
        CHECK(server.early_data() == TlsEarlyData::Accepted);
        CHECK(server.state() == TlsState::Handshaking);
        CHECK(server.resumed());
        CHECK(server.has_keys(QuicLevel::Early, QuicSide::Client));
        CHECK(server.has_keys(QuicLevel::Application, QuicSide::Server));
        check_early_keys(client, server, version);

        quic_establish(client, server);
        CHECK(client.early_data() == TlsEarlyData::Accepted);
        CHECK(server.early_data() == TlsEarlyData::Accepted);
        CHECK(client.resumed());
        CHECK(store.size() == 1U); // the half-RTT ticket
    }
}

TEST_CASE("QUIC 0-RTT: another early data context, a HelloRetryRequest or a replayed Initial refuse it, and the "
          "client's early keys go",
          "[crypto][quic-handshake][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);

    // Another context: the server lowered a limit since the ticket's connection, so 0-RTT is refused; the
    // resumption stands.
    {
        QuicPair quic;
        quic_configure(quic, filter);
        Store store;
        quic_first(quic, store, ConstSpan<u8>(kContextA));
        QuicHandshake client;
        QuicHandshake server;
        REQUIRE(client.init(quic.pair.client, ConstSpan<u8>(kClientParameters), &store) == TlsError::None);
        REQUIRE(client.has_keys(QuicLevel::Early, QuicSide::Client));
        REQUIRE(server.init(quic.pair.server, ConstSpan<u8>(kServerParameters), nullptr, {},
                            ConstSpan<u8>(kContextB)) == TlsError::None);
        quic_establish(client, server);
        CHECK(server.early_data() == TlsEarlyData::Rejected);
        CHECK(client.early_data() == TlsEarlyData::Rejected);
        CHECK(server.resumed());
        CHECK_FALSE(client.has_keys(QuicLevel::Early, QuicSide::Client));
        CHECK_FALSE(server.has_keys(QuicLevel::Early, QuicSide::Client));
        QuicPacketKey packet;
        QuicHeaderKey header;
        CHECK(client.derive_keys(QuicLevel::Early, QuicSide::Client, QuicVersion::V1, packet, header) ==
              TlsError::NotReady);
    }

    // A HelloRetryRequest: the server prefers a group the client did not share.
    {
        constexpr AlgorithmId client_groups[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519,
                                                 AlgorithmId::Secp384r1Mlkem1024};
        constexpr AlgorithmId server_groups[] = {AlgorithmId::Secp384r1Mlkem1024, AlgorithmId::X25519Mlkem768};
        QuicPair quic;
        Options options;
        options.filter = &filter;
        options.client_protocols = kGame;
        options.server_protocols = kGame;
        options.client_groups = client_groups;
        options.server_groups = server_groups;
        configure(quic.pair, options);
        Store store;
        quic_first(quic, store, ConstSpan<u8>(kContextA));
        QuicHandshake client;
        QuicHandshake server;
        REQUIRE(client.init(quic.pair.client, ConstSpan<u8>(kClientParameters), &store) == TlsError::None);
        REQUIRE(server.init(quic.pair.server, ConstSpan<u8>(kServerParameters), nullptr, {},
                            ConstSpan<u8>(kContextA)) == TlsError::None);
        quic_establish(client, server);
        CHECK(server.hello_retry());
        CHECK(server.early_data() == TlsEarlyData::Rejected);
        CHECK(client.early_data() == TlsEarlyData::Rejected);
        CHECK_FALSE(client.has_keys(QuicLevel::Early, QuicSide::Client));
    }

    // A replayed Initial: the same server's filter has seen the ticket.
    {
        QuicPair quic;
        quic_configure(quic, filter);
        Store store;
        quic_first(quic, store, ConstSpan<u8>(kContextA));
        QuicHandshake client;
        QuicHandshake server;
        QuicHandshake replayed;
        REQUIRE(client.init(quic.pair.client, ConstSpan<u8>(kClientParameters), &store) == TlsError::None);
        REQUIRE(server.init(quic.pair.server, ConstSpan<u8>(kServerParameters), nullptr, {},
                            ConstSpan<u8>(kContextA)) == TlsError::None);
        REQUIRE(replayed.init(quic.pair.server, ConstSpan<u8>(kServerParameters), nullptr, {},
                              ConstSpan<u8>(kContextA)) == TlsError::None);
        const Bytes initial = drain(client, QuicLevel::Initial);
        REQUIRE(server.provide(QuicLevel::Initial, ConstSpan<u8>(initial.data(), initial.size())) == TlsError::None);
        CHECK(server.early_data() == TlsEarlyData::Accepted);
        REQUIRE(replayed.provide(QuicLevel::Initial, ConstSpan<u8>(initial.data(), initial.size())) == TlsError::None);
        CHECK(replayed.early_data() == TlsEarlyData::Replayed);
        CHECK_FALSE(replayed.has_keys(QuicLevel::Early, QuicSide::Client));
        CHECK(replayed.state() == TlsState::Handshaking);
    }
}

TEST_CASE("QUIC 0-RTT: an early data context only where early data is accepted, within its bound",
          "[crypto][quic-handshake][early][net-2]")
{
    ReplayFilter filter;
    REQUIRE(filter.init(16U, kPeriod, now()) == CryptoError::None);
    QuicPair quic;
    quic_configure(quic, filter);
    TlsContext without; // a server without early data
    Options options;
    options.server_early = false;
    options.server_protocols = kGame;
    configure_server(quic.pair, without, options);
    Bytes large;
    large.resize(kTlsMaxEarlyDataContextBytes + 1U);

    QuicHandshake handshake;
    const ConstSpan<u8> parameters(kServerParameters);
    CHECK(handshake.init(quic.pair.server, parameters) == TlsError::BadConfig); // a server with early data names one
    CHECK(handshake.state() == TlsState::Idle);
    CHECK(handshake.init(quic.pair.server, parameters, nullptr, {}, ConstSpan<u8>(large.data(), large.size())) ==
          TlsError::BadConfig);
    CHECK(handshake.init(quic.pair.server, parameters, nullptr, {},
                         ConstSpan<u8>(large.data(), kTlsMaxEarlyDataContextBytes)) == TlsError::None);
    CHECK(handshake.init(without, parameters, nullptr, {}, ConstSpan<u8>(kContextA)) == TlsError::BadConfig);
    CHECK(handshake.init(without, parameters) == TlsError::None);
    CHECK(handshake.init(quic.pair.client, ConstSpan<u8>(kClientParameters), nullptr, {}, ConstSpan<u8>(kContextA)) ==
          TlsError::BadConfig);
    CHECK(handshake.init(quic.pair.client, ConstSpan<u8>(kClientParameters)) == TlsError::None);
    CHECK(handshake.early_data() == TlsEarlyData::None);
}

TEST_CASE("TLS 0-RTT: the client logs its early traffic secret, and a server logs it only when it accepts early data",
          "[crypto][tls][early][key-log][net-2]")
{
    if (!tls_key_log_compiled())
    {
        SKIP("key logging is compiled out of this build");
    }
    using crd::crypto::test::KeyLog;
    using crd::crypto::test::KeyLogLine;
    ReplayFilter filter;
    REQUIRE(filter.init(1024U, kPeriod, now()) == CryptoError::None);
    KeyLog  client_log;
    KeyLog  server_log;
    Pair    pair;
    Options options;
    options.filter     = &filter;
    options.client_log = &client_log;
    options.server_log = &server_log;
    configure(pair, options);
    TlsContext without;
    KeyLog     without_log;
    Options    refusing = options;
    refusing.server_early = false;
    refusing.server_log   = &without_log;
    configure_server(pair, without, refusing);

    for (const bool accepted : {true, false})
    {
        CAPTURE(accepted);
        Store store;
        first_connection(pair, store);
        client_log.clear();
        server_log.clear();
        TlsSession  client;
        TlsSession  server;
        const Bytes message = bytes_of("logged early");
        start_early(client, server, pair.client, accepted ? pair.server : without, store, message);
        const Bytes flight = drain(client);
        REQUIRE(flight.size() > 43U);
        const ConstSpan<u8> random(flight.data() + 11U, 32U);
        REQUIRE(give(server, flight) == TlsError::None);
        Bytes early;
        establish(client, server, early);
        CHECK(server.early_data() == (accepted ? TlsEarlyData::Accepted : TlsEarlyData::Rejected));

        const KeyLog& server_side = accepted ? server_log : without_log;
        CHECK_FALSE(client_log.malformed());
        CHECK_FALSE(server_side.malformed());
        CHECK(client_log.size() == std::size(crd::crypto::test::kKeyLogTraffic) + 1U);
        CHECK(client_log.all_of_random(random));
        const KeyLogLine* logged = client_log.find("CLIENT_EARLY_TRAFFIC_SECRET");
        REQUIRE(logged != nullptr);
        const KeyLogLine* served = server_side.find("CLIENT_EARLY_TRAFFIC_SECRET");
        if (accepted)
        {
            REQUIRE(served != nullptr);
            CHECK(*served == *logged);
            CHECK(client_log.same(server_side));
        }
        else
        {
            CHECK(served == nullptr);
            CHECK(server_side.size() == std::size(crd::crypto::test::kKeyLogTraffic));
            for (const char* label : crd::crypto::test::kKeyLogTraffic)
            {
                CAPTURE(label);
                const KeyLogLine* mine   = client_log.find(label);
                const KeyLogLine* theirs = server_side.find(label);
                REQUIRE(mine != nullptr);
                REQUIRE(theirs != nullptr);
                CHECK(*mine == *theirs);
            }
        }
        without_log.clear();
        read_tickets(client, server);
    }
}
