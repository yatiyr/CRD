// crd-crypto -- QUIC address-validation tokens (NET.2; docs/design/network-foundation.md#address-validation): Retry
// and NEW_TOKEN tokens validated for exactly what they are bound to (address, port, connection IDs, version, kind) and
// refused as Forged for anything else, every byte changed, every length, the lifetimes and the clock skew at their
// edges, a fleet's key rotation, every refusal, unique tokens, a NEW_TOKEN token's single use through a replay
// filter whose period covers the lifetime and the clock skew, and keys and a filter shared by several threads. The
// filter's own periods, slot reuse and false positive rate are test_replay.cpp's.
//
// The round trips a Retry and a NEW_TOKEN token cost against the amplification limit are measured with real
// handshakes in test_quic_handshake.cpp.

#include <crd/containers/array.hpp>
#include <crd/crypto/quic.hpp>

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
using namespace crd::crypto;

constexpr QuicVersion kVersions[] = {QuicVersion::V1, QuicVersion::V2};

constexpr u8 kKeyA[kQuicTokenKeyBytes] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB,
                                          0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
                                          0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0};
constexpr u8 kKeyB[kQuicTokenKeyBytes] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
                                          0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                                          0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30};
constexpr u8 kKeyC[kQuicTokenKeyBytes] = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B,
                                          0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66,
                                          0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70};

constexpr u8 kIpv4[]      = {192, 0, 2, 7};
constexpr u8 kIpv4Other[] = {192, 0, 2, 8};
constexpr u8 kIpv6[]      = {0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07};
constexpr u8 kMapped[]    = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 0, 2, 7};

constexpr u8 kOriginal[] = {0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08}; // the client's first Destination ID
constexpr u8 kClient[]   = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8}; // the client's Source ID
constexpr u8 kRetry[]    = {0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61}; // the Retry's Source ID

constexpr u64 kNow = 1793577600U; // 2026-11-02T00:00:00Z

struct Token
{
    u8    bytes[kQuicMaxTokenBytes] = {};
    usize size                      = 0U;

    [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(bytes, size); }
};

QuicTokenPeer peer_of(ConstSpan<u8> address, u16 port)
{
    QuicTokenPeer peer;
    peer.address = address;
    peer.port    = port;
    return peer;
}

const QuicTokenPeer kPeer = peer_of(ConstSpan<u8>(kIpv4), 4433U);

Token retry_token(const QuicTokenKeys& keys, QuicVersion version = QuicVersion::V1, u64 now = kNow,
                  const QuicTokenPeer& peer = kPeer)
{
    Token token;
    REQUIRE(keys.mint_retry(version, peer, ConstSpan<u8>(kOriginal), ConstSpan<u8>(kClient), ConstSpan<u8>(kRetry),
                            now, Span<u8>(token.bytes), token.size) == CryptoError::None);
    return token;
}

Token new_token(const QuicTokenKeys& keys, QuicVersion version = QuicVersion::V1, u64 now = kNow,
                const QuicTokenPeer& peer = kPeer)
{
    Token token;
    REQUIRE(keys.mint_new_token(version, peer, now, Span<u8>(token.bytes), token.size) == CryptoError::None);
    return token;
}

// The validation of `token` as it arrives in the client's next Initial: to the Retry's connection ID, from its own.
CryptoError check(const QuicTokenKeys& keys, ConstSpan<u8> token, QuicVersion version = QuicVersion::V1,
                  u64 now = kNow, const QuicTokenPeer& peer = kPeer, ReplayFilter* replay = nullptr)
{
    QuicTokenClaims claims;
    return keys.validate(version, token, peer, ConstSpan<u8>(kRetry), ConstSpan<u8>(kClient), now, claims, replay);
}

bool equal(ConstSpan<u8> a, ConstSpan<u8> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
} // namespace

TEST_CASE("QUIC tokens: a Retry token validates for exactly its client, connection IDs and version",
          "[crypto][net-2][quic-token]")
{
    QuicTokenKeys keys;
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}) == CryptoError::None);
    for (const QuicVersion version : kVersions)
    {
        const Token token = retry_token(keys, version);
        CHECK(token.size == kQuicNewTokenBytes + 1U + sizeof(kOriginal));
        CHECK(quic_token_kind(token.view()) == QuicTokenKind::Retry);

        QuicTokenClaims claims;
        REQUIRE(keys.validate(version, token.view(), kPeer, ConstSpan<u8>(kRetry), ConstSpan<u8>(kClient), kNow,
                              claims) == CryptoError::None);
        CHECK(claims.kind == QuicTokenKind::Retry);
        CHECK(claims.issued == kNow);
        CHECK(equal(claims.original_dcid_view(), ConstSpan<u8>(kOriginal)));

        // Anything else it is bound to: Forged, and the claims untouched.
        QuicTokenClaims untouched;
        const QuicVersion other_version = version == QuicVersion::V1 ? QuicVersion::V2 : QuicVersion::V1;
        CHECK(keys.validate(other_version, token.view(), kPeer, ConstSpan<u8>(kRetry), ConstSpan<u8>(kClient), kNow,
                            untouched) == CryptoError::Forged);
        CHECK(keys.validate(version, token.view(), peer_of(ConstSpan<u8>(kIpv4Other), 4433U), ConstSpan<u8>(kRetry),
                            ConstSpan<u8>(kClient), kNow, untouched) == CryptoError::Forged);
        CHECK(keys.validate(version, token.view(), peer_of(ConstSpan<u8>(kIpv4), 4434U), ConstSpan<u8>(kRetry),
                            ConstSpan<u8>(kClient), kNow, untouched) == CryptoError::Forged);
        CHECK(keys.validate(version, token.view(), peer_of(ConstSpan<u8>(kMapped), 4433U), ConstSpan<u8>(kRetry),
                            ConstSpan<u8>(kClient), kNow, untouched) == CryptoError::Forged);
        CHECK(keys.validate(version, token.view(), kPeer, ConstSpan<u8>(kOriginal), ConstSpan<u8>(kClient), kNow,
                            untouched) == CryptoError::Forged);
        CHECK(keys.validate(version, token.view(), kPeer, ConstSpan<u8>(kRetry), ConstSpan<u8>(kRetry), kNow,
                            untouched) == CryptoError::Forged);
        CHECK(keys.validate(version, token.view(), kPeer, ConstSpan<u8>(kRetry), ConstSpan<u8>(kClient, 7U), kNow,
                            untouched) == CryptoError::Forged);
        CHECK(keys.validate(version, token.view(), kPeer, ConstSpan<u8>(kClient), ConstSpan<u8>(kRetry), kNow,
                            untouched) == CryptoError::Forged);
        CHECK(untouched.kind == QuicTokenKind::None);
        CHECK(untouched.issued == 0U);
        CHECK(untouched.original_dcid_bytes == 0U);
    }

    // IPv6, empty and longest connection IDs: the longest Retry token is kQuicMaxRetryTokenBytes.
    const QuicTokenPeer six = peer_of(ConstSpan<u8>(kIpv6), 65535U);
    u8                  longest[kQuicMaxConnectionIdBytes];
    for (usize i = 0U; i < sizeof(longest); ++i)
    {
        longest[i] = static_cast<u8>(0xE0U + i);
    }
    Token token;
    REQUIRE(keys.mint_retry(QuicVersion::V2, six, ConstSpan<u8>(longest), {}, ConstSpan<u8>(kRetry), kNow,
                            Span<u8>(token.bytes), token.size) == CryptoError::None);
    CHECK(token.size == kQuicMaxRetryTokenBytes);
    QuicTokenClaims claims;
    REQUIRE(keys.validate(QuicVersion::V2, token.view(), six, ConstSpan<u8>(kRetry), {}, kNow, claims) ==
            CryptoError::None);
    CHECK(equal(claims.original_dcid_view(), ConstSpan<u8>(longest)));
    CHECK(keys.validate(QuicVersion::V2, token.view(), six, ConstSpan<u8>(kRetry), ConstSpan<u8>(kClient), kNow,
                        claims) == CryptoError::Forged);
    REQUIRE(keys.mint_retry(QuicVersion::V1, six, {}, ConstSpan<u8>(longest), ConstSpan<u8>(kRetry), kNow,
                            Span<u8>(token.bytes), token.size) == CryptoError::None);
    CHECK(token.size == kQuicNewTokenBytes + 1U);
    REQUIRE(keys.validate(QuicVersion::V1, token.view(), six, ConstSpan<u8>(kRetry), ConstSpan<u8>(longest), kNow,
                          claims) == CryptoError::None);
    CHECK(claims.original_dcid_bytes == 0U);
}

TEST_CASE("QUIC tokens: a NEW_TOKEN token binds its client's address and version but no port or connection ID",
          "[crypto][net-2][quic-token]")
{
    QuicTokenKeys keys;
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}) == CryptoError::None);
    for (const QuicVersion version : kVersions)
    {
        for (const ConstSpan<u8> address : {ConstSpan<u8>(kIpv4), ConstSpan<u8>(kIpv6)})
        {
            const Token token = new_token(keys, version, kNow, peer_of(address, 50000U));
            CHECK(token.size == kQuicNewTokenBytes);
            CHECK(quic_token_kind(token.view()) == QuicTokenKind::NewToken);
            QuicTokenClaims claims;
            // A later connection: another port and connection IDs of its own.
            REQUIRE(keys.validate(version, token.view(), peer_of(address, 50001U), ConstSpan<u8>(kOriginal),
                                  ConstSpan<u8>(kClient), kNow + 3600U, claims) == CryptoError::None);
            CHECK(claims.kind == QuicTokenKind::NewToken);
            CHECK(claims.issued == kNow);
            CHECK(claims.original_dcid_bytes == 0U);
            REQUIRE(keys.validate(version, token.view(), peer_of(address, 1U), {}, {}, kNow, claims) ==
                    CryptoError::None);
            const QuicVersion other = version == QuicVersion::V1 ? QuicVersion::V2 : QuicVersion::V1;
            CHECK(keys.validate(other, token.view(), peer_of(address, 50000U), {}, {}, kNow, claims) ==
                  CryptoError::Forged);
        }
        const Token token = new_token(keys, version);
        CHECK(check(keys, token.view(), version, kNow, peer_of(ConstSpan<u8>(kIpv4Other), 4433U)) ==
              CryptoError::Forged);
        CHECK(check(keys, token.view(), version, kNow, peer_of(ConstSpan<u8>(kMapped), 4433U)) ==
              CryptoError::Forged);
    }
}

TEST_CASE("QUIC tokens: every changed byte and every other length is refused", "[crypto][net-2][quic-token]")
{
    QuicTokenKeys keys;
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}) == CryptoError::None);
    for (const bool retry : {true, false})
    {
        const Token token = retry ? retry_token(keys) : new_token(keys);
        REQUIRE(check(keys, token.view()) == CryptoError::None);
        for (usize at = 0U; at < token.size; ++at)
        {
            for (u32 bit = 0U; bit < 8U; ++bit)
            {
                Token changed = token;
                changed.bytes[at] = static_cast<u8>(changed.bytes[at] ^ (1U << bit));
                CAPTURE(retry, at, bit);
                CHECK(check(keys, changed.view()) == CryptoError::Forged);
            }
        }
        // The kind byte swapped: the other kind never validates (RFC 9000 section 8.1.1).
        Token swapped    = token;
        swapped.bytes[0] = static_cast<u8>(retry ? QuicTokenKind::NewToken : QuicTokenKind::Retry);
        CHECK(check(keys, swapped.view()) == CryptoError::Forged);
        for (usize size = 0U; size <= kQuicMaxTokenBytes + 1U; ++size)
        {
            if (size == token.size)
            {
                continue;
            }
            Token other = token;
            other.size  = size;
            CAPTURE(retry, size);
            const CryptoError expected = size < kQuicNewTokenBytes || size > kQuicMaxTokenBytes
                                             ? CryptoError::BadLength
                                             : CryptoError::Forged;
            CHECK(check(keys, other.view()) == expected);
        }
    }
    // The kind read in the clear: nothing for an empty token or another format.
    CHECK(quic_token_kind({}) == QuicTokenKind::None);
    const u8 unknown[] = {0x13U};
    CHECK(quic_token_kind(ConstSpan<u8>(unknown)) == QuicTokenKind::None);
}

TEST_CASE("QUIC tokens: lifetimes and the clock skew hold at their edges", "[crypto][net-2][quic-token]")
{
    QuicTokenKeys keys;
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}, 3600U) == CryptoError::None);
    CHECK(keys.new_token_lifetime() == 3600U);
    const Token retry = retry_token(keys);
    CHECK(check(keys, retry.view(), QuicVersion::V1, kNow + kQuicRetryTokenLifetime) == CryptoError::None);
    CHECK(check(keys, retry.view(), QuicVersion::V1, kNow + kQuicRetryTokenLifetime + 1U) == CryptoError::Expired);
    CHECK(check(keys, retry.view(), QuicVersion::V1, kNow - kQuicTokenClockSkew) == CryptoError::None);
    CHECK(check(keys, retry.view(), QuicVersion::V1, kNow - kQuicTokenClockSkew - 1U) == CryptoError::Expired);
    const Token fresh = new_token(keys);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + 3600U) == CryptoError::None);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + 3601U) == CryptoError::Expired);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow - kQuicTokenClockSkew) == CryptoError::None);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow - kQuicTokenClockSkew - 1U) == CryptoError::Expired);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, ~u64{0}) == CryptoError::Expired);

    // The extremes of the clock never wrap.
    const Token first = new_token(keys, QuicVersion::V1, 0U);
    CHECK(check(keys, first.view(), QuicVersion::V1, 0U) == CryptoError::None);
    CHECK(check(keys, first.view(), QuicVersion::V1, ~u64{0}) == CryptoError::Expired);
    const Token last = new_token(keys, QuicVersion::V1, ~u64{0});
    CHECK(check(keys, last.view(), QuicVersion::V1, ~u64{0}) == CryptoError::None);
    CHECK(check(keys, last.view(), QuicVersion::V1, 0U) == CryptoError::Expired);

    // The default NEW_TOKEN lifetime, and the longest.
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}) == CryptoError::None);
    CHECK(keys.new_token_lifetime() == kQuicNewTokenLifetime);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + kQuicNewTokenLifetime) == CryptoError::None);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + kQuicNewTokenLifetime + 1U) == CryptoError::Expired);
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}, kQuicMaxNewTokenLifetime) == CryptoError::None);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + kQuicMaxNewTokenLifetime) == CryptoError::None);
}

TEST_CASE("QUIC tokens: a fleet shares its key and rotates it without refusing a valid token",
          "[crypto][net-2][quic-token]")
{
    QuicTokenKeys first;
    QuicTokenKeys second;
    REQUIRE(first.init(ConstSpan<u8>(kKeyA), {}) == CryptoError::None);
    REQUIRE(second.init(ConstSpan<u8>(kKeyA), {}) == CryptoError::None);
    const Token under_a = retry_token(first);
    const Token fresh_a = new_token(second);
    CHECK(check(second, under_a.view()) == CryptoError::None);
    CHECK(check(first, fresh_a.view()) == CryptoError::None);
    CHECK(std::memcmp(under_a.bytes + 1U, fresh_a.bytes + 1U, kQuicTokenNameBytes) == 0); // the key's public name

    // Rotation: B seals, A still opens; then C seals, B opens and A's tokens are Forged, not told apart.
    QuicTokenKeys rotated;
    REQUIRE(rotated.init(ConstSpan<u8>(kKeyB), ConstSpan<u8>(kKeyA)) == CryptoError::None);
    CHECK(check(rotated, under_a.view()) == CryptoError::None);
    CHECK(check(rotated, fresh_a.view()) == CryptoError::None);
    const Token under_b = new_token(rotated);
    CHECK(std::memcmp(under_b.bytes + 1U, fresh_a.bytes + 1U, kQuicTokenNameBytes) != 0);
    CHECK(check(first, under_b.view()) == CryptoError::Forged);
    REQUIRE(rotated.init(ConstSpan<u8>(kKeyC), ConstSpan<u8>(kKeyB)) == CryptoError::None);
    CHECK(check(rotated, under_b.view()) == CryptoError::None);
    CHECK(check(rotated, under_a.view()) == CryptoError::Forged);
    CHECK(check(rotated, fresh_a.view()) == CryptoError::Forged);
    REQUIRE(rotated.init(ConstSpan<u8>(kKeyB), {}) == CryptoError::None);
    CHECK(check(rotated, under_a.view()) == CryptoError::Forged);
    CHECK(check(rotated, under_b.view()) == CryptoError::None);
}

TEST_CASE("QUIC tokens: refusals outside the contract", "[crypto][net-2][quic-token]")
{
    QuicTokenKeys keys;
    Token         token;
    CHECK_FALSE(keys.ready());
    CHECK(keys.mint_new_token(QuicVersion::V1, kPeer, kNow, Span<u8>(token.bytes), token.size) ==
          CryptoError::NotReady);
    CHECK(check(keys, ConstSpan<u8>(token.bytes, kQuicNewTokenBytes)) == CryptoError::NotReady);

    CHECK(keys.init(ConstSpan<u8>(kKeyA, 31U), {}) == CryptoError::BadLength);
    CHECK(keys.init(ConstSpan<u8>(kKeyA), ConstSpan<u8>(kKeyB, 16U)) == CryptoError::BadLength);
    CHECK(keys.init(ConstSpan<u8>(kKeyA), {}, 0U) == CryptoError::BadLength);
    CHECK(keys.init(ConstSpan<u8>(kKeyA), {}, kQuicMaxNewTokenLifetime + 1U) == CryptoError::BadLength);
    CHECK(keys.init(ConstSpan<u8>(kKeyA), ConstSpan<u8>(kKeyA)) == CryptoError::BadKey);
    CHECK_FALSE(keys.ready());
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}) == CryptoError::None);
    CHECK(keys.ready());

    // Minting: lengths, the Retry's own connection ID equal to the original one, a version not implemented.
    const u8 five[] = {1, 2, 3, 4, 5};
    u8       long_id[kQuicMaxConnectionIdBytes + 1U] = {};
    CHECK(keys.mint_new_token(QuicVersion::V1, peer_of(ConstSpan<u8>(five), 1U), kNow, Span<u8>(token.bytes),
                              token.size) == CryptoError::BadLength);
    CHECK(keys.mint_new_token(QuicVersion::V1, peer_of({}, 1U), kNow, Span<u8>(token.bytes), token.size) ==
          CryptoError::BadLength);
    CHECK(keys.mint_new_token(QuicVersion::V1, kPeer, kNow, Span<u8>(token.bytes, kQuicNewTokenBytes - 1U),
                              token.size) == CryptoError::BadLength);
    CHECK(token.size == 0U);
    CHECK(keys.mint_new_token(static_cast<QuicVersion>(0xFF000020U), kPeer, kNow, Span<u8>(token.bytes),
                              token.size) == CryptoError::Unsupported);
    CHECK(keys.mint_retry(QuicVersion::V1, kPeer, ConstSpan<u8>(long_id), ConstSpan<u8>(kClient),
                          ConstSpan<u8>(kRetry), kNow, Span<u8>(token.bytes), token.size) == CryptoError::BadLength);
    CHECK(keys.mint_retry(QuicVersion::V1, kPeer, ConstSpan<u8>(kOriginal), ConstSpan<u8>(long_id),
                          ConstSpan<u8>(kRetry), kNow, Span<u8>(token.bytes), token.size) == CryptoError::BadLength);
    CHECK(keys.mint_retry(QuicVersion::V1, kPeer, ConstSpan<u8>(kOriginal), ConstSpan<u8>(kClient),
                          ConstSpan<u8>(long_id), kNow, Span<u8>(token.bytes), token.size) == CryptoError::BadLength);
    CHECK(keys.mint_retry(QuicVersion::V1, kPeer, ConstSpan<u8>(kOriginal), ConstSpan<u8>(kClient),
                          ConstSpan<u8>(kRetry), kNow, Span<u8>(token.bytes, kQuicNewTokenBytes + 8U),
                          token.size) == CryptoError::BadLength);
    CHECK(keys.mint_retry(QuicVersion::V1, kPeer, ConstSpan<u8>(kOriginal), ConstSpan<u8>(kClient),
                          ConstSpan<u8>(kOriginal), kNow, Span<u8>(token.bytes), token.size) == CryptoError::Overlap);
    CHECK(keys.mint_retry(QuicVersion::V1, kPeer, {}, ConstSpan<u8>(kClient), {}, kNow, Span<u8>(token.bytes),
                          token.size) == CryptoError::Overlap);
    CHECK(token.size == 0U);

    // Validating: lengths of the address and the connection IDs, a version not implemented, a replay filter that
    // is not ready or covers less than the NEW_TOKEN lifetime.
    const Token     fresh = new_token(keys);
    QuicTokenClaims claims;
    CHECK(keys.validate(QuicVersion::V1, fresh.view(), peer_of(ConstSpan<u8>(five), 1U), {}, {}, kNow, claims) ==
          CryptoError::BadLength);
    CHECK(keys.validate(QuicVersion::V1, fresh.view(), kPeer, ConstSpan<u8>(long_id), {}, kNow, claims) ==
          CryptoError::BadLength);
    CHECK(keys.validate(QuicVersion::V1, fresh.view(), kPeer, {}, ConstSpan<u8>(long_id), kNow, claims) ==
          CryptoError::BadLength);
    CHECK(keys.validate(static_cast<QuicVersion>(0xFF000020U), fresh.view(), kPeer, {}, {}, kNow, claims) ==
          CryptoError::Unsupported);
    ReplayFilter replay;
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow, kPeer, &replay) == CryptoError::NotReady);
    REQUIRE(replay.init(16U, kQuicNewTokenLifetime - 1U, kNow) == CryptoError::None);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow, kPeer, &replay) == CryptoError::NotReady);
    // The filter's period covers the clock skew too: tokens living less than the skew still need a period of it.
    QuicTokenKeys brief;
    REQUIRE(brief.init(ConstSpan<u8>(kKeyA), {}, 1U) == CryptoError::None);
    const Token short_lived = new_token(brief);
    REQUIRE(replay.init(16U, kQuicTokenClockSkew - 1U, kNow) == CryptoError::None);
    CHECK(check(brief, short_lived.view(), QuicVersion::V1, kNow, kPeer, &replay) == CryptoError::NotReady);
    REQUIRE(replay.init(16U, kQuicTokenClockSkew, kNow) == CryptoError::None);
    CHECK(check(brief, short_lived.view(), QuicVersion::V1, kNow, kPeer, &replay) == CryptoError::None);
    CHECK(replay.init(0U, kQuicNewTokenLifetime, kNow) == CryptoError::BadLength);
    CHECK_FALSE(replay.ready());

    keys.clear();
    CHECK_FALSE(keys.ready());
    CHECK(check(keys, fresh.view()) == CryptoError::NotReady);
}

TEST_CASE("QUIC tokens: every token is unique and carries no readable connection detail",
          "[crypto][net-2][quic-token]")
{
    QuicTokenKeys keys;
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}) == CryptoError::None);
    constexpr usize count = 512U;
    crd::containers::Array<Token> tokens;
    for (usize i = 0U; i < count; ++i)
    {
        // An IPv6 client: its 16 address bytes never occur by chance in a token's random bytes.
        const QuicTokenPeer six = peer_of(ConstSpan<u8>(kIpv6), 4433U);
        tokens.push_back(i % 2U == 0U ? new_token(keys, QuicVersion::V1, kNow, six)
                                      : retry_token(keys, QuicVersion::V1, kNow, six));
    }
    for (usize i = 0U; i < count; ++i)
    {
        for (usize j = i + 1U; j < count; ++j)
        {
            CHECK(std::memcmp(tokens[i].bytes + 5U, tokens[j].bytes + 5U, kQuicTokenSaltBytes) != 0);
        }
        // Neither the address nor the original connection ID appears in the clear (RFC 9000 section 8.1.3).
        const Token& token = tokens[i];
        for (usize at = 0U; at + sizeof(kOriginal) <= token.size; ++at)
        {
            CHECK(std::memcmp(token.bytes + at, kOriginal, sizeof(kOriginal)) != 0);
        }
        for (usize at = 0U; at + sizeof(kIpv6) <= token.size; ++at)
        {
            CHECK(std::memcmp(token.bytes + at, kIpv6, sizeof(kIpv6)) != 0);
        }
    }
}

TEST_CASE("QUIC tokens: a NEW_TOKEN token is accepted once, a Retry token within its lifetime",
          "[crypto][net-2][quic-token]")
{
    QuicTokenKeys keys;
    REQUIRE(keys.init(ConstSpan<u8>(kKeyA), {}, 3600U) == CryptoError::None);
    ReplayFilter replay;
    REQUIRE(replay.init(1024U, 3600U, kNow) == CryptoError::None);
    CHECK(replay.ready());
    CHECK(replay.period() == 3600U);
    CHECK(replay.bytes() == 4U * 32U * 64U);

    const Token fresh = new_token(keys);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + 5U, kPeer, &replay) == CryptoError::None);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + 6U, kPeer, &replay) == CryptoError::Replayed);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + 3600U, kPeer, &replay) == CryptoError::Replayed);
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + 3601U, kPeer, &replay) == CryptoError::Expired);
    // Without the filter the check is the token's alone.
    CHECK(check(keys, fresh.view(), QuicVersion::V1, kNow + 6U) == CryptoError::None);
    // A forged or expired token never reaches the filter: the honest token after it is still new.
    const Token other = new_token(keys);
    Token       forged = other;
    forged.bytes[kQuicNewTokenBytes - 1U] ^= 1U;
    CHECK(check(keys, forged.view(), QuicVersion::V1, kNow, kPeer, &replay) == CryptoError::Forged);
    CHECK(check(keys, other.view(), QuicVersion::V1, kNow + 4000U, kPeer, &replay) == CryptoError::Expired);
    CHECK(check(keys, other.view(), QuicVersion::V1, kNow, kPeer, &replay) == CryptoError::None);

    const Token retry = retry_token(keys);
    CHECK(check(keys, retry.view(), QuicVersion::V1, kNow, kPeer, &replay) == CryptoError::None);
    CHECK(check(keys, retry.view(), QuicVersion::V1, kNow + 1U, kPeer, &replay) == CryptoError::None);
}

TEST_CASE("QUIC tokens: keys and a replay filter serve several threads", "[crypto][net-2][quic-token]")
{
    QuicTokenKeys keys;
    REQUIRE(keys.init(ConstSpan<u8>(kKeyB), ConstSpan<u8>(kKeyA)) == CryptoError::None);
    ReplayFilter replay;
    REQUIRE(replay.init(4096U, kQuicNewTokenLifetime, kNow) == CryptoError::None);
    constexpr usize threads   = 4U;
    constexpr usize per       = 256U;
    crd::containers::Array<Token> tokens;
    for (usize i = 0U; i < threads * per; ++i)
    {
        tokens.push_back(new_token(keys));
    }
    std::atomic<usize> failures{0U};
    {
        std::thread workers[threads];
        for (usize t = 0U; t < threads; ++t)
        {
            workers[t] = std::thread(
                [&, t]
                {
                    for (usize i = t * per; i < (t + 1U) * per; ++i)
                    {
                        QuicTokenClaims claims;
                        Token           minted;
                        if (keys.validate(QuicVersion::V1, tokens[i].view(), kPeer, {}, {}, kNow, claims, &replay) !=
                                CryptoError::None ||
                            keys.mint_retry(QuicVersion::V1, kPeer, ConstSpan<u8>(kOriginal), ConstSpan<u8>(kClient),
                                            ConstSpan<u8>(kRetry), kNow, Span<u8>(minted.bytes),
                                            minted.size) != CryptoError::None ||
                            keys.validate(QuicVersion::V1, minted.view(), kPeer, ConstSpan<u8>(kRetry),
                                          ConstSpan<u8>(kClient), kNow, claims, &replay) != CryptoError::None)
                        {
                            failures.fetch_add(1U);
                        }
                    }
                });
        }
        for (std::thread& worker : workers)
        {
            worker.join();
        }
    }
    CHECK(failures.load() == 0U);
    for (const Token& token : tokens)
    {
        CHECK(check(keys, token.view(), QuicVersion::V1, kNow, kPeer, &replay) == CryptoError::Replayed);
    }
}

// Stateless reset tokens (RFC 9000 section 10.3.2): HMAC-SHA256 of the connection ID under the static key, truncated
// to 16 bytes. The expected values were computed with Python's hmac module, independently of the provider.
TEST_CASE("QUIC stateless reset tokens: HMAC-SHA256 of the connection ID, truncated", "[crypto][net-2][quic-token]")
{
    u8 key[kQuicResetKeyBytes];
    for (usize i = 0U; i < sizeof(key); ++i)
    {
        key[i] = static_cast<u8>(i);
    }
    QuicResetKey reset;
    CHECK_FALSE(reset.ready());
    REQUIRE(reset.init(ConstSpan<u8>(key)) == CryptoError::None);
    REQUIRE(reset.ready());
    struct Vector
    {
        u8 id[kQuicMaxConnectionIdBytes];
        usize bytes;
        u8 token[kQuicResetTokenBytes];
    };
    const Vector vectors[] = {
        {{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77},
         16U,
         {0x83, 0xc2, 0xdc, 0x14, 0xb0, 0x45, 0x32, 0x47, 0x3e, 0xd6, 0xc1, 0xba, 0x6c, 0x59, 0x08, 0x0c}},
        {{0x5a}, 1U, {0xd3, 0xda, 0x88, 0x61, 0xb0, 0x92, 0x4d, 0xf7, 0x28, 0x96, 0xb8, 0x2c, 0x83, 0xd3, 0xa1, 0x3b}},
        {{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
          0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13},
         20U,
         {0x0c, 0xd4, 0x38, 0xc0, 0xf4, 0xf4, 0x46, 0x37, 0xdd, 0xeb, 0xa1, 0x46, 0xb3, 0xb3, 0xa5, 0xf8}},
    };
    for (const Vector& vector : vectors)
    {
        u8 token[kQuicResetTokenBytes];
        REQUIRE(reset.token(ConstSpan<u8>(vector.id, vector.bytes), Span<u8>(token)) == CryptoError::None);
        CHECK(std::memcmp(token, vector.token, sizeof(token)) == 0);
    }

    // A fleet that shares the key derives the same token; a fresh key derives another.
    QuicResetKey shared;
    REQUIRE(shared.init(ConstSpan<u8>(key)) == CryptoError::None);
    QuicResetKey fresh;
    REQUIRE(fresh.generate() == CryptoError::None);
    u8 mine[kQuicResetTokenBytes];
    u8 theirs[kQuicResetTokenBytes];
    u8 other[kQuicResetTokenBytes];
    const ConstSpan<u8> id(vectors[0].id, vectors[0].bytes);
    REQUIRE(reset.token(id, Span<u8>(mine)) == CryptoError::None);
    REQUIRE(shared.token(id, Span<u8>(theirs)) == CryptoError::None);
    REQUIRE(fresh.token(id, Span<u8>(other)) == CryptoError::None);
    CHECK(std::memcmp(mine, theirs, sizeof(mine)) == 0);
    CHECK(std::memcmp(mine, other, sizeof(mine)) != 0);

    // Refusals zero the output.
    u8 out[kQuicResetTokenBytes + 1U];
    const u8 long_id[kQuicMaxConnectionIdBytes + 1U] = {};
    std::memset(out, 0xEE, sizeof(out));
    CHECK(reset.token({}, Span<u8>(out, kQuicResetTokenBytes)) == CryptoError::BadLength);
    CHECK(out[0] == 0U);
    std::memset(out, 0xEE, sizeof(out));
    CHECK(reset.token(ConstSpan<u8>(long_id), Span<u8>(out, kQuicResetTokenBytes)) == CryptoError::BadLength);
    CHECK(out[kQuicResetTokenBytes - 1U] == 0U);
    CHECK(reset.token(id, Span<u8>(out, sizeof(out))) == CryptoError::BadLength);
    CHECK(reset.token(id, Span<u8>(out, kQuicResetTokenBytes - 1U)) == CryptoError::BadLength);
    CHECK(reset.init(ConstSpan<u8>(key, kQuicResetKeyBytes - 1U)) == CryptoError::BadLength);
    CHECK_FALSE(reset.ready());
    std::memset(out, 0xEE, sizeof(out));
    CHECK(reset.token(id, Span<u8>(out, kQuicResetTokenBytes)) == CryptoError::NotReady);
    CHECK(out[3] == 0U);
    REQUIRE(reset.init(ConstSpan<u8>(key)) == CryptoError::None);
    reset.clear();
    CHECK(reset.token(id, Span<u8>(out, kQuicResetTokenBytes)) == CryptoError::NotReady);
}
