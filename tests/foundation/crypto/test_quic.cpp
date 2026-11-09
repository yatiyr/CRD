// crd-crypto -- QUIC packet protection (NET.2; docs/design/network-foundation.md#quic-protection): the sample packets
// of RFC 9001 appendix A and RFC 9369 appendix A byte for byte (the Initial secrets, the client's and the server's
// Initial packets sealed and header-protected and then read back, the Retry integrity tags, the ChaCha20-Poly1305
// short header packet and its key update), RFC 9000's packet number examples, every refusal and length rule, the
// usage limits, tampering anywhere in a packet, random round trips in every suite and version, one key opening on
// several threads, and a packet number space's window opening each packet once through reordering, duplication,
// replay and forgery.

#include "quic_vectors.hpp"
#include "wycheproof.hpp"

#include <crd/crypto/quic.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <thread>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using namespace crd::crypto::test;
using namespace crd::quic_vectors;

struct InitialSample
{
    QuicVersion version;
    const char* client_secret;
    const char* server_secret;
    const char* client_header;
    const char* client_protected;
    const char* server_header;
    const char* server_protected;
    const char* retry;
    const char* chacha_next_secret;
    const char* chacha_packet;
};

constexpr InitialSample kSamples[] = {
    {QuicVersion::V1, kV1ClientSecret, kV1ServerSecret, kV1ClientHeader, kV1ClientProtected, kV1ServerHeader,
     kV1ServerProtected, kV1Retry, kV1ChachaNextSecret, kV1ChachaPacket},
    {QuicVersion::V2, kV2ClientSecret, kV2ServerSecret, kV2ClientHeader, kV2ClientProtected, kV2ServerHeader,
     kV2ServerProtected, kV2Retry, kV2ChachaNextSecret, kV2ChachaPacket},
};

constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};

Bytes concat(ConstSpan<u8> a, ConstSpan<u8> b, usize total)
{
    Bytes out;
    out.resize(total);
    std::memset(out.data(), 0, total);
    std::memcpy(out.data(), a.data(), a.size());
    std::memcpy(out.data() + a.size(), b.data(), b.size());
    return out;
}

Bytes initial_secret(QuicVersion version, QuicSide side)
{
    Bytes secret;
    secret.resize(kQuicInitialSecretBytes);
    REQUIRE(quic_initial_secret(version, side, from_hex(kClientDcid), secret) == CryptoError::None);
    return secret;
}

// Seals and header-protects `header` + `payload` (zero padding up to the length of `expected`) and compares it with
// `expected`, then removes the protection, decodes the packet number (the receiver's largest is the one before) and
// opens it back to the plaintext.
void protect_and_read_back(QuicVersion version, AlgorithmId aead, ConstSpan<u8> secret, ConstSpan<u8> header,
                           ConstSpan<u8> payload, usize packet_number_offset, u64 packet_number, ConstSpan<u8> expected)
{
    QuicPacketKey key;
    QuicHeaderKey hp;
    REQUIRE(key.init(version, aead, secret) == CryptoError::None);
    REQUIRE(hp.init(version, aead, secret) == CryptoError::None);
    Bytes packet = concat(header, payload, expected.size());
    const Bytes plain = packet;
    REQUIRE(key.seal(packet_number, packet, header.size()) == CryptoError::None);
    CHECK(key.sealed() == 1U);
    REQUIRE(hp.protect(packet, packet_number_offset) == CryptoError::None);
    CHECK(equal(packet, expected));

    usize     number_bytes    = 0U;
    u64       truncated       = 0U;
    const u64 expected_number = packet_number; // the sample's own number: the closest candidate is itself
    REQUIRE(hp.unprotect(packet, packet_number_offset, number_bytes, truncated) == CryptoError::None);
    CHECK(number_bytes == header.size() - packet_number_offset);
    const u64 decoded = quic_decode_packet_number(truncated, number_bytes, expected_number);
    CHECK(decoded == packet_number);
    CHECK(equal(ConstSpan<u8>(packet).first(header.size()), header));
    REQUIRE(key.open(decoded, packet, header.size()) == CryptoError::None);
    CHECK(equal(ConstSpan<u8>(packet).first(packet.size() - kQuicTagBytes),
                ConstSpan<u8>(plain).first(plain.size() - kQuicTagBytes)));
}
} // namespace

TEST_CASE("QUIC: Initial secrets of RFC 9001 and RFC 9369", "[crypto][net-2][quic]")
{
    for (const InitialSample& sample : kSamples)
    {
        CHECK(equal(initial_secret(sample.version, QuicSide::Client), from_hex(sample.client_secret)));
        CHECK(equal(initial_secret(sample.version, QuicSide::Server), from_hex(sample.server_secret)));
    }
    CHECK(quic_version_supported(0x00000001U));
    CHECK(quic_version_supported(0x6B3343CFU));
    CHECK_FALSE(quic_version_supported(0U));             // version negotiation
    CHECK_FALSE(quic_version_supported(0x1A2A3A4AU));    // a reserved (greased) version
    CHECK_FALSE(quic_version_supported(0xFF00001DU));    // draft-29

    u8 secret[kQuicInitialSecretBytes];
    u8 cid[kQuicMaxConnectionIdBytes + 1U] = {};
    CHECK(quic_initial_secret(static_cast<QuicVersion>(0xFF00001DU), QuicSide::Client, ConstSpan<u8>(cid, 8U),
                              secret) == CryptoError::Unsupported);
    CHECK(quic_initial_secret(QuicVersion::V1, QuicSide::Client, cid, secret) == CryptoError::BadLength);
    CHECK(quic_initial_secret(QuicVersion::V1, QuicSide::Client, ConstSpan<u8>(cid, 8U), Span<u8>(secret, 31U)) ==
          CryptoError::BadLength);
    // Every length from the empty connection ID up to 20 bytes is a valid Initial (RFC 9000 section 7.2).
    for (usize length = 0U; length <= kQuicMaxConnectionIdBytes; ++length)
    {
        CHECK(quic_initial_secret(QuicVersion::V2, QuicSide::Server, ConstSpan<u8>(cid, length), secret) ==
              CryptoError::None);
    }
}

TEST_CASE("QUIC: the client's and the server's Initial packets of RFC 9001 and RFC 9369", "[crypto][net-2][quic]")
{
    for (const InitialSample& sample : kSamples)
    {
        CAPTURE(static_cast<u32>(sample.version));
        protect_and_read_back(sample.version, AlgorithmId::Aes128Gcm, initial_secret(sample.version, QuicSide::Client),
                              from_hex(sample.client_header), from_hex(kClientCryptoFrame), 18U, 2U,
                              from_hex(sample.client_protected));
        CHECK(from_hex(sample.client_protected).size() == 22U + kClientPayloadBytes + kQuicTagBytes);
        protect_and_read_back(sample.version, AlgorithmId::Aes128Gcm, initial_secret(sample.version, QuicSide::Server),
                              from_hex(sample.server_header), from_hex(kServerPayload), 18U, 1U,
                              from_hex(sample.server_protected));
    }
}

TEST_CASE("QUIC: the ChaCha20-Poly1305 short header packet and its key update", "[crypto][net-2][quic]")
{
    const Bytes secret = from_hex(kChachaSecret);
    const Bytes header = from_hex("4200bff4");
    for (const InitialSample& sample : kSamples)
    {
        CAPTURE(static_cast<u32>(sample.version));
        const u8 ping = 0x01U;
        protect_and_read_back(sample.version, AlgorithmId::Chacha20Poly1305, secret, header, ConstSpan<u8>(&ping, 1U),
                              1U, kChachaPacketNumber, from_hex(sample.chacha_packet));
        // The next key phase seals exactly as a key made from RFC's "ku" secret, and differs from the first.
        QuicPacketKey first;
        QuicPacketKey next;
        QuicPacketKey expected;
        REQUIRE(first.init(sample.version, AlgorithmId::Chacha20Poly1305, secret) == CryptoError::None);
        REQUIRE(next.update(first) == CryptoError::None);
        REQUIRE(expected.init(sample.version, AlgorithmId::Chacha20Poly1305, from_hex(sample.chacha_next_secret)) ==
                CryptoError::None);
        CHECK(next.version() == sample.version);
        CHECK(next.algorithm() == AlgorithmId::Chacha20Poly1305);
        Bytes a = concat(header, ConstSpan<u8>(&ping, 1U), 21U);
        Bytes b = a;
        Bytes c = a;
        REQUIRE(next.seal(kChachaPacketNumber, a, 4U) == CryptoError::None);
        REQUIRE(expected.seal(kChachaPacketNumber, b, 4U) == CryptoError::None);
        REQUIRE(first.seal(kChachaPacketNumber, c, 4U) == CryptoError::None);
        CHECK(equal(a, b));
        CHECK_FALSE(equal(a, c));
        // A packet of the old phase does not open under the new one: the receiver keeps both while packets reorder.
        CHECK(next.open(kChachaPacketNumber, c, 4U) == CryptoError::Forged);
    }
}

TEST_CASE("QUIC: Retry integrity tags of RFC 9001 and RFC 9369", "[crypto][net-2][quic]")
{
    const Bytes dcid = from_hex(kClientDcid);
    for (const InitialSample& sample : kSamples)
    {
        CAPTURE(static_cast<u32>(sample.version));
        const Bytes retry = from_hex(sample.retry);
        CHECK(quic_retry_verify(sample.version, dcid, retry) == CryptoError::None);
        u8 tag[kQuicTagBytes];
        REQUIRE(quic_retry_tag(sample.version, dcid, ConstSpan<u8>(retry).first(retry.size() - kQuicTagBytes), tag) ==
                CryptoError::None);
        CHECK(equal(tag, ConstSpan<u8>(retry).last(kQuicTagBytes)));

        // Every bit of the packet, and of the original connection ID it does not carry, is authenticated.
        for (usize bit = 0U; bit < retry.size() * 8U; ++bit)
        {
            Bytes changed = retry;
            changed[bit / 8U] ^= static_cast<u8>(1U << (bit % 8U));
            CHECK(quic_retry_verify(sample.version, dcid, changed) == CryptoError::Forged);
        }
        Bytes other = dcid;
        other[7] ^= 0x01U;
        CHECK(quic_retry_verify(sample.version, other, retry) == CryptoError::Forged);
        CHECK(quic_retry_verify(sample.version, ConstSpan<u8>(dcid).first(7U), retry) == CryptoError::Forged);
        const QuicVersion wrong = sample.version == QuicVersion::V1 ? QuicVersion::V2 : QuicVersion::V1;
        CHECK(quic_retry_verify(wrong, dcid, retry) == CryptoError::Forged);
    }

    u8 big[kQuicMaxRetryBytes + 1U] = {};
    u8 cid[kQuicMaxConnectionIdBytes + 1U] = {};
    u8 tag[kQuicTagBytes];
    CHECK(quic_retry_verify(QuicVersion::V1, ConstSpan<u8>(cid, 8U), ConstSpan<u8>(big, kQuicTagBytes - 1U)) ==
          CryptoError::BadLength);
    CHECK(quic_retry_verify(QuicVersion::V1, ConstSpan<u8>(cid, 8U), big) == CryptoError::BadLength);
    CHECK(quic_retry_verify(QuicVersion::V1, cid, ConstSpan<u8>(big, 40U)) == CryptoError::BadLength);
    CHECK(quic_retry_verify(static_cast<QuicVersion>(7U), ConstSpan<u8>(cid, 8U), ConstSpan<u8>(big, 40U)) ==
          CryptoError::Unsupported);
    // The longest Retry and the longest connection ID fit the pseudo-packet; one byte more is refused.
    REQUIRE(quic_retry_tag(QuicVersion::V2, ConstSpan<u8>(cid, kQuicMaxConnectionIdBytes),
                           ConstSpan<u8>(big, kQuicMaxRetryBytes - kQuicTagBytes), tag) == CryptoError::None);
    std::memcpy(big + kQuicMaxRetryBytes - kQuicTagBytes, tag, kQuicTagBytes);
    CHECK(quic_retry_verify(QuicVersion::V2, ConstSpan<u8>(cid, kQuicMaxConnectionIdBytes),
                            ConstSpan<u8>(big, kQuicMaxRetryBytes)) == CryptoError::None);
    std::memset(tag, 0xAA, sizeof(tag));
    CHECK(quic_retry_tag(QuicVersion::V2, ConstSpan<u8>(cid, 8U), ConstSpan<u8>(big, kQuicMaxRetryBytes), tag) ==
          CryptoError::BadLength);
    CHECK(all_zero(tag));
    CHECK(quic_retry_tag(QuicVersion::V2, ConstSpan<u8>(cid, 8U), ConstSpan<u8>(big, 20U), Span<u8>(tag, 15U)) ==
          CryptoError::BadLength);
}

TEST_CASE("QUIC: a session secret gives each side its own traffic secret and connection ID", "[crypto][net-2][quic]")
{
    u8 session[kQuicKeyedSecretBytes];
    std::memset(session, 0x2A, sizeof(session));
    u8 client[kQuicMaxSecretBytes];
    u8 server[kQuicMaxSecretBytes];
    usize client_bytes = 0U;
    usize server_bytes = 0U;

    // Fixed once: both ends of every release must derive the same keys.
    REQUIRE(quic_keyed_traffic_secret(AlgorithmId::Aes128Gcm, session, QuicSide::Client, client, client_bytes) ==
            CryptoError::None);
    const Bytes expected_client = from_hex("d367362093b73f6ffde66e57049bcc428b519e1c84094a5bc89db74e8ca14bd0");
    REQUIRE(client_bytes == expected_client.size());
    CHECK(std::memcmp(client, expected_client.data(), client_bytes) == 0);
    REQUIRE(quic_keyed_traffic_secret(AlgorithmId::Aes256Gcm, session, QuicSide::Server, server, server_bytes) ==
            CryptoError::None);
    const Bytes expected_server =
        from_hex("6df0b8accfbf51e950359b7b4060d2216adaae5cc381481397eea718f4776b41e9d6d6166b17"
                 "5ed281aa20a8057567a3");
    REQUIRE(server_bytes == expected_server.size());
    CHECK(std::memcmp(server, expected_server.data(), server_bytes) == 0);
    u8 id[8];
    REQUIRE(quic_keyed_connection_id(session, QuicSide::Server, id) == CryptoError::None);
    const Bytes expected_id = from_hex("5ae42cfebc8347bb");
    CHECK(std::memcmp(id, expected_id.data(), sizeof(id)) == 0);

    for (const AlgorithmId aead : kAeads)
    {
        REQUIRE(quic_keyed_traffic_secret(aead, session, QuicSide::Client, client, client_bytes) == CryptoError::None);
        REQUIRE(quic_keyed_traffic_secret(aead, session, QuicSide::Server, server, server_bytes) == CryptoError::None);
        CHECK(client_bytes == ((aead == AlgorithmId::Aes256Gcm) ? 48U : 32U));
        CHECK(client_bytes == server_bytes);
        CHECK(std::memcmp(client, server, client_bytes) != 0);
        QuicPacketKey key;
        CHECK(key.init(QuicVersion::V1, aead, ConstSpan<u8>(client, client_bytes)) == CryptoError::None);
    }
    u8 other[kQuicKeyedSecretBytes];
    std::memset(other, 0x2B, sizeof(other));
    u8 changed[kQuicMaxSecretBytes];
    REQUIRE(quic_keyed_traffic_secret(AlgorithmId::Aes128Gcm, other, QuicSide::Client, changed, client_bytes) ==
            CryptoError::None);
    REQUIRE(quic_keyed_traffic_secret(AlgorithmId::Aes128Gcm, session, QuicSide::Client, client, client_bytes) ==
            CryptoError::None);
    CHECK(std::memcmp(client, changed, client_bytes) != 0);

    u8 client_id[20];
    u8 server_id[20];
    REQUIRE(quic_keyed_connection_id(session, QuicSide::Client, client_id) == CryptoError::None);
    REQUIRE(quic_keyed_connection_id(session, QuicSide::Server, server_id) == CryptoError::None);
    CHECK(std::memcmp(client_id, server_id, sizeof(client_id)) != 0);

    CHECK(quic_keyed_traffic_secret(AlgorithmId::X25519, session, QuicSide::Client, client, client_bytes) ==
          CryptoError::Unsupported);
    CHECK(client_bytes == 0U);
    CHECK(quic_keyed_traffic_secret(AlgorithmId::Aes128Gcm, ConstSpan<u8>(session, 31U), QuicSide::Client, client,
                                    client_bytes) == CryptoError::BadLength);
    CHECK(client[0] == 0U);
    CHECK(quic_keyed_traffic_secret(AlgorithmId::Aes128Gcm, session, QuicSide::Client, Span<u8>(client, 47U),
                                    client_bytes) == CryptoError::BadLength);
    CHECK(quic_keyed_connection_id(ConstSpan<u8>(session, 33U), QuicSide::Client, id) == CryptoError::BadLength);
    u8 too_long[21];
    CHECK(quic_keyed_connection_id(session, QuicSide::Client, too_long) == CryptoError::BadLength);
}

TEST_CASE("QUIC: packet number encoding and decoding of RFC 9000", "[crypto][net-2][quic]")
{
    // Appendix A.2: 0xac5c02 after 0xabe8b3 was acknowledged needs 16 bits; 0xace8fe needs 24.
    CHECK(quic_packet_number_bytes(0xAC5C02U, 0xABE8B3U, true) == 2U);
    CHECK(quic_packet_number_bytes(0xACE8FEU, 0xABE8B3U, true) == 3U);
    // Appendix A.3: 0x9b32 after 0xa82f30ea decodes to 0xa82f9b32.
    CHECK(quic_decode_packet_number(0x9B32U, 2U, 0xA82F30EBU) == 0xA82F9B32U);

    // Twice the unacknowledged range: up to 2^(8n-1) packets fit n bytes.
    CHECK(quic_packet_number_bytes(0U, 0U, false) == 1U);
    CHECK(quic_packet_number_bytes(127U, 0U, false) == 1U);
    CHECK(quic_packet_number_bytes(128U, 0U, false) == 2U);
    CHECK(quic_packet_number_bytes(128U, 0U, true) == 1U);
    CHECK(quic_packet_number_bytes(129U, 0U, true) == 2U);
    CHECK(quic_packet_number_bytes(1000U + (u64{1} << 23U), 1000U, true) == 3U);
    CHECK(quic_packet_number_bytes(1001U + (u64{1} << 23U), 1000U, true) == 4U);
    CHECK(quic_packet_number_bytes(1000U + (u64{1} << 31U), 1000U, true) == 4U);
    CHECK(quic_packet_number_bytes(1001U + (u64{1} << 31U), 1000U, true) == 0U); // no encoding reaches it
    CHECK(quic_packet_number_bytes(kQuicMaxPacketNumber, kQuicMaxPacketNumber - 1U, true) == 1U);
    CHECK(quic_packet_number_bytes(kQuicMaxPacketNumber + 1U, kQuicMaxPacketNumber, true) == 0U);
    CHECK(quic_packet_number_bytes(5U, 5U, true) == 0U);
    CHECK(quic_packet_number_bytes(4U, 5U, true) == 0U);

    // Every encodable distance round-trips, at the edges of each window and of the packet number space.
    const u64 largest[] = {0U, 1U, 126U, 127U, 128U, 255U, 256U, 0xFFFFU, 0x10000U, 0xA82F30EAU,
                           kQuicMaxPacketNumber - (u64{1} << 31U), kQuicMaxPacketNumber - 2U};
    for (const u64 acked : largest)
    {
        for (usize bytes = 1U; bytes <= 4U; ++bytes)
        {
            const u64 half = u64{1} << (8U * bytes - 1U);
            for (const u64 ahead : {u64{1}, u64{2}, half - 1U, half})
            {
                const u64 number = acked + ahead;
                if (number > kQuicMaxPacketNumber)
                {
                    continue;
                }
                const usize length = quic_packet_number_bytes(number, acked, true);
                REQUIRE(length != 0U);
                CHECK(length <= bytes);
                const u64 mask = (u64{1} << (8U * length)) - 1U;
                // The receiver's largest received is at least the sender's largest acknowledged.
                CHECK(quic_decode_packet_number(number & mask, length, acked + 1U) == number);
            }
        }
    }
    // At exactly half a window either way the higher candidate is kept.
    CHECK(quic_decode_packet_number(0x00U, 1U, 0x80U) == 0x100U);
    CHECK(quic_decode_packet_number(0x80U, 1U, 0x100U) == 0x180U);
    CHECK(quic_decode_packet_number(0x80U, 1U, 0x00U) == 0x80U);
    CHECK(quic_decode_packet_number(0x7FU, 1U, 0x100U) == 0x17FU);
    // Near zero and at the top the candidate never leaves [0, 2^62).
    CHECK(quic_decode_packet_number(0xFFU, 1U, 0U) == 0xFFU);
    CHECK(quic_decode_packet_number(0x00U, 1U, kQuicMaxPacketNumber) == kQuicMaxPacketNumber - 0xFFU);
    CHECK(quic_decode_packet_number(0xFFFFFFFFU, 4U, kQuicMaxPacketNumber) == kQuicMaxPacketNumber);
    CHECK(quic_decode_packet_number(0x1234U, 0U, 77U) == 0x1234U);
    CHECK(quic_decode_packet_number(0x1234U, 5U, 77U) == 0x1234U);
}

TEST_CASE("QUIC: packet keys refuse what is outside their contract", "[crypto][net-2][quic]")
{
    u8            secret32[32] = {1, 2, 3};
    u8            secret48[48] = {4, 5, 6};
    QuicPacketKey key;
    QuicHeaderKey hp;
    u8            packet[64] = {};
    CHECK_FALSE(key.ready());
    CHECK(key.seal(0U, packet, 8U) == CryptoError::NotReady);
    CHECK(key.open(0U, packet, 8U) == CryptoError::NotReady);
    usize bytes = 9U;
    u64   truncated = 9U;
    CHECK(hp.protect(packet, 8U) == CryptoError::NotReady);
    CHECK(hp.unprotect(packet, 8U, bytes, truncated) == CryptoError::NotReady);
    CHECK((bytes == 0U && truncated == 0U));
    u8 mask[kQuicMaskBytes];
    CHECK(hp.mask(ConstSpan<u8>(packet, kQuicSampleBytes), mask) == CryptoError::NotReady);

    // Suites: the hash follows the AEAD; any other algorithm or version is refused and leaves no key.
    CHECK(key.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret48) == CryptoError::BadLength);
    CHECK(key.init(QuicVersion::V1, AlgorithmId::Aes256Gcm, secret32) == CryptoError::BadLength);
    CHECK(key.init(QuicVersion::V1, AlgorithmId::Aes256Gcm, secret48) == CryptoError::None);
    CHECK(key.init(QuicVersion::V1, AlgorithmId::HkdfSha256, secret32) == CryptoError::Unsupported);
    CHECK_FALSE(key.ready());
    CHECK(key.init(static_cast<QuicVersion>(0xFF00001DU), AlgorithmId::Aes128Gcm, secret32) ==
          CryptoError::Unsupported);
    CHECK(hp.init(QuicVersion::V2, AlgorithmId::Chacha20Poly1305, secret48) == CryptoError::BadLength);
    CHECK(hp.init(QuicVersion::V2, AlgorithmId::Siphash24, secret32) == CryptoError::Unsupported);
    CHECK_FALSE(hp.ready());

    REQUIRE(key.init(QuicVersion::V2, AlgorithmId::Aes128Gcm, secret32) == CryptoError::None);
    REQUIRE(hp.init(QuicVersion::V2, AlgorithmId::Aes128Gcm, secret32) == CryptoError::None);
    CHECK(key.seal(kQuicMaxPacketNumber + 1U, packet, 8U) == CryptoError::BadLength);
    CHECK(key.open(kQuicMaxPacketNumber + 1U, packet, 8U) == CryptoError::BadLength);
    CHECK(key.seal(0U, packet, 65U) == CryptoError::BadLength);
    CHECK(key.seal(0U, packet, 64U - kQuicTagBytes + 1U) == CryptoError::BadLength);
    CHECK(key.open(0U, Span<u8>(packet, 10U), 0U) == CryptoError::BadLength);
    CHECK(key.sealed() == 0U);
    CHECK(key.seal(kQuicMaxPacketNumber, packet, 64U - kQuicTagBytes) == CryptoError::None); // an empty payload
    CHECK(key.open(kQuicMaxPacketNumber, packet, 64U - kQuicTagBytes) == CryptoError::None);

    // Header protection needs the 16-byte sample 4 bytes past the packet number's start.
    CHECK(hp.protect(packet, 0U) == CryptoError::BadLength);
    CHECK(hp.protect(Span<u8>(packet, 8U + 4U + kQuicSampleBytes - 1U), 8U) == CryptoError::BadLength);
    CHECK(hp.protect(Span<u8>(packet, 8U + 4U + kQuicSampleBytes), 8U) == CryptoError::None);
    CHECK(hp.protect(packet, 65U) == CryptoError::BadLength);
    CHECK(hp.mask(ConstSpan<u8>(packet, 15U), mask) == CryptoError::BadLength);
    CHECK(hp.mask(ConstSpan<u8>(packet, 16U), Span<u8>(mask, 4U)) == CryptoError::BadLength);

    // Updates: from nothing, onto itself.
    QuicPacketKey empty;
    QuicPacketKey next;
    CHECK(next.update(empty) == CryptoError::NotReady);
    CHECK(key.update(key) == CryptoError::Overlap);
    CHECK(key.ready());
    REQUIRE(next.update(key) == CryptoError::None);
    CHECK(next.version() == QuicVersion::V2);
    key.clear();
    CHECK_FALSE(key.ready());
    CHECK(key.sealed() == 0U);
    hp.clear();
    CHECK_FALSE(hp.ready());
}

TEST_CASE("QUIC: usage limits refuse a packet past the key's limit", "[crypto][net-2][quic]")
{
    CHECK(quic_confidentiality_limit(AlgorithmId::Aes128Gcm) == (u64{1} << 23U));
    CHECK(quic_confidentiality_limit(AlgorithmId::Aes256Gcm) == (u64{1} << 23U));
    CHECK(quic_confidentiality_limit(AlgorithmId::Chacha20Poly1305) == kQuicMaxPacketNumber + 1U);
    CHECK(quic_confidentiality_limit(AlgorithmId::HkdfSha256) == 0U);
    CHECK(quic_integrity_limit(AlgorithmId::Aes128Gcm) == (u64{1} << 52U));
    CHECK(quic_integrity_limit(AlgorithmId::Aes256Gcm) == (u64{1} << 52U));
    CHECK(quic_integrity_limit(AlgorithmId::Chacha20Poly1305) == (u64{1} << 36U));
    CHECK(quic_integrity_limit(AlgorithmId::Ed25519) == 0U);

    const u8      secret[32] = {7};
    QuicPacketKey key;
    key.set_limit(5U); // no key: ignored
    CHECK(key.limit() == 0U);
    REQUIRE(key.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    CHECK(key.limit() == (u64{1} << 23U));
    key.set_limit(u64{1} << 40U); // never above the AEAD's limit
    CHECK(key.limit() == (u64{1} << 23U));
    key.set_limit(0U);
    CHECK(key.limit() == 1U);
    key.set_limit(8U);
    u8 packet[40] = {};
    for (u64 n = 0U; n < 8U; ++n)
    {
        CHECK(key.update_due() == (n >= 6U));
        REQUIRE(key.seal(n, packet, 8U) == CryptoError::None);
    }
    CHECK(key.update_due());
    u8 before[40];
    std::memcpy(before, packet, sizeof(packet));
    CHECK(key.seal(8U, packet, 8U) == CryptoError::Exhausted);
    CHECK(std::memcmp(before, packet, sizeof(packet)) == 0);
    CHECK(key.sealed() == 8U);
    CHECK(key.open(7U, packet, 8U) == CryptoError::None); // opening is never limited
    QuicPacketKey next;
    REQUIRE(next.update(key) == CryptoError::None);
    CHECK(next.sealed() == 0U);
    CHECK(next.limit() == (u64{1} << 23U));
    CHECK_FALSE(next.update_due());
}

TEST_CASE("QUIC: a changed byte anywhere in a protected packet is refused", "[crypto][net-2][quic]")
{
    const Bytes secret = initial_secret(QuicVersion::V1, QuicSide::Server);
    QuicPacketKey key;
    QuicHeaderKey hp;
    REQUIRE(key.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    REQUIRE(hp.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    const Bytes protected_packet = from_hex(kV1ServerProtected);
    for (usize at = 0U; at < protected_packet.size(); ++at)
    {
        Bytes packet = protected_packet;
        packet[at] ^= 0x40U;
        usize bytes = 0U;
        u64   truncated = 0U;
        REQUIRE(hp.unprotect(packet, 18U, bytes, truncated) == CryptoError::None);
        const usize header = 18U + bytes;
        const u64   number = quic_decode_packet_number(truncated, bytes, 0U);
        if (header > packet.size() - kQuicTagBytes)
        {
            continue; // a changed length bit made the header swallow the payload
        }
        CHECK(key.open(number, packet, header) == CryptoError::Forged);
        CHECK(all_zero(ConstSpan<u8>(packet).subspan(header, packet.size() - header - kQuicTagBytes)));
    }
}

TEST_CASE("QUIC: random packets round-trip in every suite and version", "[crypto][net-2][quic]")
{
    u8 secret[kQuicMaxSecretBytes];
    u8 buffer[1500];
    u8 original[1500];
    for (const QuicVersion version : {QuicVersion::V1, QuicVersion::V2})
    {
        for (const AlgorithmId aead : kAeads)
        {
            const usize secret_bytes = aead == AlgorithmId::Aes256Gcm ? 48U : 32U;
            REQUIRE(random_bytes(Span<u8>(secret, secret_bytes)) == CryptoError::None);
            QuicPacketKey key;
            QuicHeaderKey hp;
            REQUIRE(key.init(version, aead, ConstSpan<u8>(secret, secret_bytes)) == CryptoError::None);
            REQUIRE(hp.init(version, aead, ConstSpan<u8>(secret, secret_bytes)) == CryptoError::None);
            u64 largest_acked = 0U;
            u64 number        = 0U;
            for (usize round = 0U; round < 200U; ++round)
            {
                u8 random[8];
                REQUIRE(random_bytes(random) == CryptoError::None);
                number += 1U + random[0] % 50U;
                if (random[1] % 4U == 0U)
                {
                    largest_acked = number - 1U - random[2] % (number < 64U ? number : 64U);
                }
                const usize length = quic_packet_number_bytes(number, largest_acked, round != 0U);
                REQUIRE(length != 0U);
                const bool  long_header = (random[3] & 1U) != 0U;
                const usize pn_offset   = long_header ? 1U + 4U + 1U + 8U + 1U + 8U + 2U : 1U + 8U;
                const usize header      = pn_offset + length;
                const usize size        = header + 4U + random[4] + (random[5] & 3U) * 256U + kQuicTagBytes;
                REQUIRE(random_bytes(Span<u8>(buffer, size)) == CryptoError::None);
                buffer[0] = static_cast<u8>((long_header ? 0xC0U : 0x40U) | (random[6] & 0x3CU) | (length - 1U));
                for (usize i = 0U; i < length; ++i)
                {
                    buffer[pn_offset + i] = static_cast<u8>(number >> (8U * (length - 1U - i)));
                }
                std::memcpy(original, buffer, size);
                REQUIRE(key.seal(number, Span<u8>(buffer, size), header) == CryptoError::None);
                REQUIRE(hp.protect(Span<u8>(buffer, size), pn_offset) == CryptoError::None);
                CHECK((buffer[0] & (long_header ? 0xF0U : 0xE0U)) == (original[0] & (long_header ? 0xF0U : 0xE0U)));

                usize bytes = 0U;
                u64   truncated = 0U;
                REQUIRE(hp.unprotect(Span<u8>(buffer, size), pn_offset, bytes, truncated) == CryptoError::None);
                CHECK(bytes == length);
                const u64 decoded = quic_decode_packet_number(truncated, bytes, largest_acked + 1U);
                CHECK(decoded == number);
                REQUIRE(key.open(decoded, Span<u8>(buffer, size), header) == CryptoError::None);
                CHECK(std::memcmp(buffer, original, size - kQuicTagBytes) == 0);
            }
        }
    }
}

TEST_CASE("QUIC: one packet key and one header key serve several threads", "[crypto][net-2][quic]")
{
    const Bytes   secret = initial_secret(QuicVersion::V2, QuicSide::Client);
    QuicPacketKey key;
    QuicHeaderKey hp;
    REQUIRE(key.init(QuicVersion::V2, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    REQUIRE(hp.init(QuicVersion::V2, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    const Bytes protected_packet = from_hex(kV2ClientProtected);
    const Bytes expected_header  = from_hex(kV2ClientHeader);
    bool        ok[4]            = {};
    std::thread threads[4];
    for (usize t = 0U; t < 4U; ++t)
    {
        threads[t] = std::thread(
            [&, t]
            {
                bool good = true;
                for (usize i = 0U; i < 50U; ++i)
                {
                    Bytes packet = protected_packet;
                    usize bytes  = 0U;
                    u64   truncated = 0U;
                    good = good && hp.unprotect(packet, 18U, bytes, truncated) == CryptoError::None &&
                           key.open(truncated, packet, 18U + bytes) == CryptoError::None &&
                           equal(ConstSpan<u8>(packet).first(22U), expected_header);
                }
                ok[t] = good;
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK((ok[0] && ok[1] && ok[2] && ok[3]));
}

TEST_CASE("QUIC: a packet number space's window opens each packet once and a forged packet moves nothing",
          "[crypto][net-2][quic][replay]")
{
    // A sender's 600 short header packets, delivered with local reordering, every seventh one twice, every fiftieth
    // again 300 packets later, and a forged packet claiming a far higher number. The receiver unprotects the header,
    // decodes the number against its window, checks it before the AEAD and accepts it after (RFC 9000 section 12.3).
    constexpr usize count = 600U;
    constexpr usize size  = 1U + 8U + 2U + 40U + kQuicTagBytes; // a 2-byte packet number
    constexpr usize pn_at = 1U + 8U;
    u8              secret[32];
    REQUIRE(random_bytes(Span<u8>(secret)) == CryptoError::None);
    QuicPacketKey sender;
    QuicPacketKey receiver;
    QuicHeaderKey hp;
    REQUIRE(sender.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, ConstSpan<u8>(secret)) == CryptoError::None);
    REQUIRE(receiver.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, ConstSpan<u8>(secret)) == CryptoError::None);
    REQUIRE(hp.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, ConstSpan<u8>(secret)) == CryptoError::None);
    static u8 packets[count][size];
    for (usize n = 0U; n < count; ++n)
    {
        u8* const packet = packets[n];
        for (usize i = 0U; i < size; ++i)
        {
            packet[i] = static_cast<u8>(n + 3U * i);
        }
        packet[0]         = 0x41U;
        packet[pn_at]     = static_cast<u8>(n >> 8U);
        packet[pn_at + 1] = static_cast<u8>(n);
        REQUIRE(sender.seal(n, Span<u8>(packet, size), pn_at + 2U) == CryptoError::None);
        REQUIRE(hp.protect(Span<u8>(packet, size), pn_at) == CryptoError::None);
    }

    QuicPacketWindow window;
    usize            opened    = 0U;
    usize            dropped   = 0U;
    bool             seen[count] = {};
    // One received datagram: true when its packet is processed.
    const auto receive = [&](const u8* datagram) -> bool
    {
        u8 packet[size];
        std::memcpy(packet, datagram, size);
        usize bytes     = 0U;
        u64   truncated = 0U;
        REQUIRE(hp.unprotect(Span<u8>(packet), pn_at, bytes, truncated) == CryptoError::None);
        const u64 number = quic_decode_packet_number(truncated, bytes, window.any() ? window.largest() + 1U : 0U);
        if (window.check(number) != ReplayVerdict::Fresh)
        {
            ++dropped;
            return false;
        }
        ++opened;
        if (receiver.open(number, Span<u8>(packet), pn_at + bytes) != CryptoError::None)
        {
            return false;
        }
        REQUIRE(window.accept(number) == ReplayVerdict::Fresh);
        REQUIRE(number < count);
        CHECK_FALSE(seen[number]);
        seen[number] = true;
        return true;
    };

    usize expected_drops = 0U;
    for (usize block = 0U; block < count; block += 8U)
    {
        // Each run of eight arrives in a fixed shuffled order.
        constexpr usize order[8] = {2U, 0U, 1U, 5U, 3U, 7U, 4U, 6U};
        for (const usize at : order)
        {
            const usize n = block + at;
            CHECK(receive(packets[n]));
            if (n % 7U == 0U)
            {
                CHECK_FALSE(receive(packets[n])); // duplicated on the path
                ++expected_drops;
            }
            if (n >= 300U && (n - 300U) % 50U == 0U)
            {
                CHECK_FALSE(receive(packets[n - 300U])); // replayed from 300 packets back: Stale
                ++expected_drops;
            }
        }
        if (block == 296U)
        {
            // A forged packet: header protection under the right key, a number far ahead, a wrong tag.
            u8 forged[size];
            std::memcpy(forged, packets[block], size);
            forged[pn_at] ^= 0x40U;
            forged[size - 1U] ^= 0x01U;
            const u64 before = window.largest();
            CHECK_FALSE(receive(forged));
            CHECK(window.largest() == before);
        }
    }
    CHECK(window.largest() == count - 1U);
    CHECK(dropped == expected_drops);
    CHECK(opened == count + 1U); // every packet once, and the forged one, whose AEAD fails
    for (usize n = 0U; n < count; ++n)
    {
        CHECK(seen[n]);
    }
}
