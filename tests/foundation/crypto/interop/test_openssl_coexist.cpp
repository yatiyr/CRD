// crd-crypto-coexist-tests (NET.2 decision P5): crd-crypto over its prefixed AWS-LC and the pinned OpenSSL 4.0 in one
// process. Each library computes the same answers as the other through its own code: the provider is still AWS-LC,
// AWS-LC's SHA-256 and AES-256-GCM agree with OpenSSL's, and OpenSSL completes its own TLS 1.3 connection. The program
// also runs crd-crypto's TLS and QUIC handshake tests, so AWS-LC's libssl works beside OpenSSL's.

#include "openssl_coexist.h"

#include <crd/crypto/provider.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using namespace crd::crypto;

constexpr usize kMessageBytes = 61U;

} // namespace

TEST_CASE("Provider: crd-crypto and OpenSSL in one process each run their own library", "[crypto][net-2][coexist]")
{
    CHECK(provider_name() == StringView("AWS-LC 5.12.0"));
    CHECK(crd_coexist_openssl_version() >= 0x40000000UL); // OpenSSL 4.0 or later, by its own answer

    u8 message[kMessageBytes] = {};
    for (usize i = 0U; i < kMessageBytes; ++i)
    {
        message[i] = static_cast<u8>(i * 7U + 3U);
    }
    for (const usize size : {usize{0U}, usize{1U}, usize{55U}, kMessageBytes})
    {
        u8 ours[kSha256Bytes]   = {};
        u8 theirs[kSha256Bytes] = {};
        sha256(ConstSpan<u8>(message, size), Span<u8, kSha256Bytes>(ours));
        REQUIRE(crd_coexist_openssl_sha256(message, size, theirs) == 1);
        CHECK(constant_time_equal(ConstSpan<u8>(ours), ConstSpan<u8>(theirs)));
    }

    u8 key[32]                               = {};
    u8 nonce[kAeadNonceBytes]                = {};
    u8 associated[13]                        = {};
    u8 sealed[kMessageBytes + kAeadTagBytes] = {};
    u8 opened[kMessageBytes]                 = {};
    REQUIRE(random_bytes(Span<u8>(key)) == CryptoError::None);
    REQUIRE(random_bytes(Span<u8>(nonce)) == CryptoError::None);
    AeadKey aead;
    REQUIRE(aead.init(AlgorithmId::Aes256Gcm, ConstSpan<u8>(key)) == CryptoError::None);
    REQUIRE(aead.seal(ConstSpan<u8>(nonce), ConstSpan<u8>(message), ConstSpan<u8>(associated), Span<u8>(sealed)) ==
            CryptoError::None);
    REQUIRE(crd_coexist_openssl_open(key, nonce, sealed, sizeof sealed, associated, sizeof associated, opened) == 1);
    CHECK(constant_time_equal(ConstSpan<u8>(opened), ConstSpan<u8>(message)));
    sealed[0] ^= 1U;
    CHECK(crd_coexist_openssl_open(key, nonce, sealed, sizeof sealed, associated, sizeof associated, opened) == 0);

    CHECK(crd_coexist_openssl_tls() == 1);
}
