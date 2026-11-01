// Example: the package's cryptography (NET.2).
//
//   diag-consumer crypto
//
// Cerid::crypto carries AWS-LC's libssl and libcrypto behind it, link-only: an application reaches them only through
// crd/crypto/provider.hpp and crd/crypto/tls.hpp and never includes an OpenSSL header. This route runs one TLS 1.3
// connection between two sessions in this process, both ends authenticated by pinned ML-DSA-65 keys over the
// X25519MLKEM768 hybrid group, the server's certificate compressed with zstd through Cerid::compression; it sends a
// message each way, closes with close_notify both ways and prints CONSUMER_CRYPTO with what was negotiated and the
// verdict, so both provider archives and the compression edge are proven to resolve from the installed prefix alone.
#include "examples.hpp"

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/register_ids.hpp>
#include <crd/crypto/tls.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace diag_consumer
{
namespace
{
namespace cc   = crd::crypto;
namespace cont = crd::containers;

struct Endpoint
{
    cc::SigningKey    key;
    cc::TlsCredential credential;
    cc::TlsContext    context;
    cc::TlsSession    session;
};

// Moves every byte `from` has to send into `to`; false when the receiver refuses them.
bool deliver(cc::TlsSession& from, cc::TlsSession& to)
{
    static crd::u8 bytes[cc::kTlsMaxBufferedBytes];
    crd::usize     written = 0U;
    if (from.take(cont::Span<crd::u8>(bytes, sizeof(bytes)), written) != cc::TlsError::None)
    {
        return false;
    }
    if (written == 0U)
    {
        return true;
    }
    const cc::TlsError error = to.receive(cont::ConstSpan<crd::u8>(bytes, written));
    return error == cc::TlsError::None || error == cc::TlsError::Closed;
}

// Flights back and forth until neither end has anything to send.
bool pump(Endpoint& client, Endpoint& server)
{
    for (int round = 0; round < 16; ++round)
    {
        if (client.session.pending() == 0U && server.session.pending() == 0U)
        {
            return true;
        }
        if (!deliver(client.session, server.session) || !deliver(server.session, client.session))
        {
            return false;
        }
    }
    return false;
}

// Sends `message` from one end and reads it back at the other.
bool carried(cc::TlsSession& from, cc::TlsSession& to, cont::ConstSpan<crd::u8> message)
{
    if (from.send(message) != cc::TlsError::None || !deliver(from, to))
    {
        return false;
    }
    crd::u8    received[256];
    crd::usize written = 0U;
    return to.read(cont::Span<crd::u8>(received, sizeof(received)), written) == cc::TlsError::None &&
           written == message.size() && std::memcmp(received, message.data(), written) == 0;
}

bool make_endpoint(Endpoint& endpoint, const char* name)
{
    return endpoint.key.generate(cc::AlgorithmId::MlDsa65) == cc::CryptoError::None &&
           endpoint.credential.init(endpoint.key, cont::StringView(name, std::strlen(name))) == cc::TlsError::None;
}

} // namespace

int example_crypto()
{
    static constexpr cc::AlgorithmId kGroups[]     = {cc::AlgorithmId::X25519Mlkem768};
    static constexpr cc::AlgorithmId kSignatures[] = {cc::AlgorithmId::MlDsa65};
    static constexpr char            kServerName[] = "orders.example.test";

    static Endpoint client;
    static Endpoint server;
    bool            ok = make_endpoint(client, "till.example.test") && make_endpoint(server, kServerName);
    const cc::KeyId server_pin = server.credential.key_id();
    const cc::KeyId client_pin = client.credential.key_id();

    cc::TlsConfig client_config;
    client_config.role        = cc::TlsRole::Client;
    client_config.groups      = cont::ConstSpan<cc::AlgorithmId>(kGroups, 1U);
    client_config.signatures  = cont::ConstSpan<cc::AlgorithmId>(kSignatures, 1U);
    client_config.aeads       = cc::tls_aead_preference();
    client_config.credential  = &client.credential;
    client_config.pinned      = cont::ConstSpan<cc::KeyId>(&server_pin, 1U);
    client_config.server_name = cont::StringView(kServerName, sizeof(kServerName) - 1U);

    cc::TlsConfig server_config = client_config;
    server_config.role          = cc::TlsRole::Server;
    server_config.credential    = &server.credential;
    server_config.pinned        = cont::ConstSpan<cc::KeyId>(&client_pin, 1U);

    ok = ok && client.context.init(client_config) == cc::TlsError::None &&
         server.context.init(server_config) == cc::TlsError::None &&
         client.session.init(client.context) == cc::TlsError::None &&
         server.session.init(server.context) == cc::TlsError::None && pump(client, server);
    const bool handshake = ok && client.session.state() == cc::TlsState::Established &&
                           server.session.state() == cc::TlsState::Established;
    const bool mutual = handshake && client.session.peer_key() == server_pin && server.session.peer_key() == client_pin;

    static constexpr char kOrder[] = "order 7 booked, total still positive";
    static constexpr char kReply[] = "order 7 confirmed";
    const bool round_trip =
        mutual &&
        carried(client.session, server.session,
                cont::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(kOrder), sizeof(kOrder) - 1U)) &&
        carried(server.session, client.session,
                cont::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(kReply), sizeof(kReply) - 1U));
    // close_notify both ways: the server reads the client's (read() reports Closed) and answers with its own.
    crd::u8    rest[16];
    crd::usize rest_bytes = 0U;
    const bool closed = round_trip && client.session.close() == cc::TlsError::None &&
                        deliver(client.session, server.session) &&
                        server.session.read(cont::Span<crd::u8>(rest, sizeof(rest)), rest_bytes) ==
                            cc::TlsError::Closed &&
                        server.session.state() == cc::TlsState::Closed && server.session.alert_received().present &&
                        server.session.alert_received().description == 0U &&
                        server.session.close() == cc::TlsError::None && deliver(server.session, client.session) &&
                        client.session.state() == cc::TlsState::Closed && server.session.pending() == 0U;

    // The negotiated algorithms as Cerid's register identifiers, the compression as its RFC 8879 code.
    std::printf("CONSUMER_CRYPTO group=0x%04x aead=0x%04x signature=0x%04x certificate_compression=%u handshake=%d "
                "mutual=%d round_trip=%d closed=%d\n",
                static_cast<unsigned>(client.session.group()), static_cast<unsigned>(client.session.aead()),
                static_cast<unsigned>(client.session.peer_algorithm()),
                static_cast<unsigned>(client.session.certificate_compression()), handshake ? 1 : 0, mutual ? 1 : 0,
                round_trip ? 1 : 0, closed ? 1 : 0);
    return closed ? 0 : 7;
}

} // namespace diag_consumer
