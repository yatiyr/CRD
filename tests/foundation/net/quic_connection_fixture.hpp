#pragma once

// What the QUIC connection and server tests share (4.2a; docs/design/network-foundation.md#quic-connection): the
// server identity and the TLS contexts that trust it, connection configurations with deterministic entropy, and the
// seeded generator of the hostile paths.

#include <crd/containers/string_view.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/endpoint.hpp>
#include <crd/net/quic_connection.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <ctime>

namespace crd::net::testing
{
namespace cont = crd::containers;

inline constexpr crypto::AlgorithmId kGroups[] = {crypto::AlgorithmId::X25519Mlkem768, crypto::AlgorithmId::X25519};
inline constexpr crypto::AlgorithmId kSignatures[] = {crypto::AlgorithmId::Ed25519};
inline constexpr crypto::AlgorithmId kAeads[] = {crypto::AlgorithmId::Aes128Gcm, crypto::AlgorithmId::Chacha20Poly1305};
inline constexpr cont::StringView kProtocols[] = {"cerid-game/1"};
inline constexpr const char* kServerName = "game.cerid.test";
inline constexpr crypto::QuicVersion kVersions[] = {crypto::QuicVersion::V1, crypto::QuicVersion::V2};
inline constexpr u64 kStartNs = 1000000000U;
inline constexpr u64 kMs = 1000000U;
inline constexpr usize kDatagram = 1500U;
inline constexpr u64 kUnix = 1793577600U; // 2026-11-02T00:00:00Z: the fleet clock tokens carry
inline constexpr u8 kTokenKey[crypto::kQuicTokenKeyBytes] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
inline const Endpoint kClientPeer{IpAddress::v4(192, 0, 2, 9), 4433U};

inline constexpr u8 kTicketKey[crypto::kTlsTicketKeyBytes] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF};

// A server key and the client and server contexts that trust it; `wrong_pin` makes the client pin another key. With
// an `early` replay filter both ends configure early data (0-RTT) under one ticket key, and `plain` is a server of
// the same fleet that takes none.
struct Identity
{
    crypto::SigningKey key;
    crypto::SigningKey other;
    crypto::TlsCredential credential;
    crypto::TlsContext client;
    crypto::TlsContext server;
    crypto::TlsContext plain;

    explicit Identity(bool wrong_pin = false, crypto::ReplayFilter* early = nullptr)
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        REQUIRE(key.from_seed(crypto::AlgorithmId::Ed25519, cont::ConstSpan<u8>(seed, sizeof(seed))) ==
                crypto::CryptoError::None);
        seed[0] = 0xEEU;
        REQUIRE(other.from_seed(crypto::AlgorithmId::Ed25519, cont::ConstSpan<u8>(seed, sizeof(seed))) ==
                crypto::CryptoError::None);
        REQUIRE(credential.init(key, kServerName) == crypto::TlsError::None);
        crypto::TlsCredential stranger;
        REQUIRE(stranger.init(other, kServerName) == crypto::TlsError::None);
        const crypto::KeyId pin = wrong_pin ? stranger.key_id() : credential.key_id();

        crypto::TlsConfig client_config;
        client_config.role = crypto::TlsRole::Client;
        client_config.groups = kGroups;
        client_config.signatures = kSignatures;
        client_config.aeads = kAeads;
        client_config.pinned = cont::ConstSpan<crypto::KeyId>(&pin, 1U);
        client_config.server_name = kServerName;
        client_config.application_protocols = kProtocols;
        client_config.early_data = early != nullptr;
        REQUIRE(client.init(client_config) == crypto::TlsError::None);

        crypto::TlsConfig server_config;
        server_config.role = crypto::TlsRole::Server;
        server_config.groups = kGroups;
        server_config.aeads = kAeads;
        server_config.credential = &credential;
        server_config.server_name = kServerName;
        server_config.application_protocols = kProtocols;
        if (early != nullptr)
        {
            server_config.ticket_key = cont::ConstSpan<u8>(kTicketKey);
            REQUIRE(plain.init(server_config) == crypto::TlsError::None);
            server_config.early_data = true;
            server_config.early_data_filter = early;
        }
        REQUIRE(server.init(server_config) == crypto::TlsError::None);
    }
};

inline constexpr u8 kSeeds[4][crypto::kPrfKeyBytes] = {{0x01}, {0x02}, {0x03}, {0x04}};

inline QuicConnectionConfig config_for(crypto::QuicSide side, const Identity& identity, crypto::QuicVersion version,
                                       usize seed = 0U)
{
    QuicConnectionConfig config;
    config.side = side;
    config.version = version;
    config.tls = (side == crypto::QuicSide::Client) ? &identity.client : &identity.server;
    config.parameters.max_idle_timeout = 30000U;
    config.entropy_seed = cont::ConstSpan<u8>(kSeeds[seed % 4U], crypto::kPrfKeyBytes);
    return config;
}

// splitmix64: the seeded paths' impairments.
struct Rng
{
    u64 state = 0U;

    u64 next()
    {
        u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    bool chance(u32 permille) { return (next() % 1000U) < permille; }
};

// A path's impairments, each datagram decided by the seeded generator.
struct Path
{
    u32 loss = 0U;      // permille
    u32 duplicate = 0U; // permille
    u64 delay_ns = 10U * kMs;
    u64 jitter_ns = 0U; // a uniform extra delay, which reorders
};

// A client's ticket store holding one ticket, handed out once.
class Tickets final : public crypto::TlsTicketStore
{
public:
    void keep(const crypto::TlsBinding& binding, cont::ConstSpan<u8> ticket) noexcept override
    {
        if (ticket.size() <= sizeof(m_ticket))
        {
            m_binding = binding;
            std::memcpy(m_ticket, ticket.data(), ticket.size());
            m_bytes = ticket.size();
        }
    }

    cont::ConstSpan<u8> take(const crypto::TlsBinding& binding) noexcept override
    {
        if (m_bytes == 0U || !(m_binding == binding))
        {
            return {};
        }
        std::memcpy(m_handed, m_ticket, m_bytes);
        const usize bytes = m_bytes;
        m_bytes = 0U;
        return cont::ConstSpan<u8>(m_handed, bytes);
    }

    [[nodiscard]] bool holds() const { return m_bytes != 0U; }

private:
    crypto::TlsBinding m_binding;
    u8 m_ticket[4096] = {};
    u8 m_handed[4096] = {};
    usize m_bytes = 0U;
};

// The window a ticket can be presented in: what the early data replay filter covers.
inline constexpr u64 kTicketPeriod = crypto::kTlsDefaultTicketLifetime + crypto::kTlsMaxTicketAgeSkew;

// A replay filter ready before the contexts that hold it are built.
struct ReadyFilter
{
    crypto::ReplayFilter filter;

    ReadyFilter()
    {
        REQUIRE(filter.init(1024U, kTicketPeriod, static_cast<u64>(std::time(nullptr))) == crypto::CryptoError::None);
    }
};

} // namespace crd::net::testing
