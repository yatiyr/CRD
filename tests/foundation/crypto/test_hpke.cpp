// crd-crypto -- hybrid public key encryption (NET.2; docs/design/network-foundation.md#hpke): every base-mode vector
// of RFC 9180's test-vectors.json in the profile (DeriveKeyPair for the recipient and the ephemeral key, the private
// keys' serialization, the opening of each context's first message and each of its exports), sealing and exporting
// round trips over every suite, and every refusal: suites outside the profile, lengths, keys off the curve,
// small-order X25519 keys, forgeries, overlapping buffers and keys that hold none.

#include "wycheproof.hpp"

#include <crd/crypto/hpke.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

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
namespace json = crd::assetio::json;

// The profile's algorithm for an RFC 9180 identifier; AlgorithmId{} outside it.
AlgorithmId kem_of(u64 id)
{
    switch (id)
    {
    case iana::kHpkeKemDhkemP256HkdfSha256:
        return AlgorithmId::Secp256r1;
    case iana::kHpkeKemDhkemP384HkdfSha384:
        return AlgorithmId::Secp384r1;
    case iana::kHpkeKemDhkemX25519HkdfSha256:
        return AlgorithmId::X25519;
    default:
        return AlgorithmId{};
    }
}

AlgorithmId kdf_of(u64 id)
{
    switch (id)
    {
    case iana::kHpkeKdfHkdfSha256:
        return AlgorithmId::HkdfSha256;
    case iana::kHpkeKdfHkdfSha384:
        return AlgorithmId::HkdfSha384;
    default:
        return AlgorithmId{};
    }
}

AlgorithmId aead_of(u64 id)
{
    switch (id)
    {
    case iana::kHpkeAeadAes128Gcm:
        return AlgorithmId::Aes128Gcm;
    case iana::kHpkeAeadAes256Gcm:
        return AlgorithmId::Aes256Gcm;
    case iana::kHpkeAeadChacha20poly1305:
        return AlgorithmId::Chacha20Poly1305;
    default:
        return AlgorithmId{};
    }
}

Bytes public_of(const HpkeKey& key)
{
    Bytes out;
    out.resize(hpke_sizes(key.kem()).public_key);
    REQUIRE(key.public_key(Span<u8>(out.data(), out.size())) == CryptoError::None);
    return out;
}

constexpr HpkeSuite kSuites[] = {
    {AlgorithmId::X25519, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm},
    {AlgorithmId::X25519, AlgorithmId::HkdfSha256, AlgorithmId::Chacha20Poly1305},
    {AlgorithmId::Secp256r1, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm},
    {AlgorithmId::Secp256r1, AlgorithmId::HkdfSha384, AlgorithmId::Aes256Gcm},
    {AlgorithmId::Secp384r1, AlgorithmId::HkdfSha384, AlgorithmId::Aes256Gcm},
    {AlgorithmId::Secp384r1, AlgorithmId::HkdfSha256, AlgorithmId::Chacha20Poly1305},
};

// A sealed message to a fresh recipient key of `suite`.
struct Sealed
{
    HpkeKey recipient;
    Bytes   enc;
    Bytes   ciphertext;
};

void seal_to(const HpkeSuite& suite, ConstSpan<u8> info, ConstSpan<u8> aad, ConstSpan<u8> plaintext, Sealed& out)
{
    REQUIRE(out.recipient.generate(suite.kem) == CryptoError::None);
    const Bytes recipient = public_of(out.recipient);
    out.enc.resize(hpke_sizes(suite.kem).public_key);
    out.ciphertext.resize(plaintext.size() + kHpkeTagBytes);
    REQUIRE(hpke_seal(suite, recipient, info, aad, plaintext, Span<u8>(out.enc.data(), out.enc.size()),
                      Span<u8>(out.ciphertext.data(), out.ciphertext.size())) == CryptoError::None);
}

TEST_CASE("HPKE: RFC 9180's base-mode vectors", "[crypto][net-2][hpke]")
{
    const JsonFile vectors(crd::containers::String(CRD_HPKE_VECTORS));
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    usize          in              = 0U;
    usize          outside         = 0U;
    usize          exported_values = 0U;
    REQUIRE(count == 128U);
    for (u32 i = 0U; i < count; ++i)
    {
        const u32 v = json::at(vectors.doc(), vectors.root(), i);
        if (vectors.number(v, "mode") != 0U)
        {
            continue; // PSK and authenticated modes: no consumer, outside the interface
        }
        const HpkeSuite suite{kem_of(vectors.number(v, "kem_id")), kdf_of(vectors.number(v, "kdf_id")),
                              aead_of(vectors.number(v, "aead_id"))};
        INFO("kem " << vectors.number(v, "kem_id") << " kdf " << vectors.number(v, "kdf_id") << " aead "
                    << vectors.number(v, "aead_id"));
        if (!hpke_supported(suite))
        {
            // X448, P-521, HKDF-SHA512 and the export-only AEAD are outside the profile.
            ++outside;
            continue;
        }
        ++in;
        // DeriveKeyPair for both key pairs, and the recipient's serialized private key.
        HpkeKey recipient;
        HpkeKey ephemeral;
        REQUIRE(recipient.derive(suite.kem, vectors.hex(v, "ikmR")) == CryptoError::None);
        REQUIRE(ephemeral.derive(suite.kem, vectors.hex(v, "ikmE")) == CryptoError::None);
        CHECK(equal(public_of(recipient), vectors.hex(v, "pkRm")));
        CHECK(equal(public_of(ephemeral), vectors.hex(v, "pkEm")));
        CHECK(equal(vectors.hex(v, "enc"), vectors.hex(v, "pkEm")));
        HpkeKey serialized;
        REQUIRE(serialized.from_private(suite.kem, vectors.hex(v, "skRm")) == CryptoError::None);
        CHECK(equal(public_of(serialized), vectors.hex(v, "pkRm")));

        // The first message of the context, whose nonce is the base nonce.
        const u32 encryptions = json::find(vectors.doc(), v, "encryptions");
        const u32 first       = json::at(vectors.doc(), encryptions, 0U);
        CHECK(equal(vectors.hex(first, "nonce"), vectors.hex(v, "base_nonce")));
        const Bytes ciphertext = vectors.hex(first, "ct");
        const Bytes expected   = vectors.hex(first, "pt");
        Bytes       plaintext  = filled(ciphertext.size() - kHpkeTagBytes, 0xA5U);
        REQUIRE(recipient.open(suite, vectors.hex(v, "enc"), vectors.hex(v, "info"), vectors.hex(first, "aad"),
                               ciphertext, Span<u8>(plaintext.data(), plaintext.size())) == CryptoError::None);
        CHECK(equal(plaintext, expected));

        // A message of a later sequence number is not a first message: it fails to authenticate.
        const u32   second      = json::at(vectors.doc(), encryptions, 1U);
        const Bytes later       = vectors.hex(second, "ct");
        Bytes       later_plain = filled(later.size() - kHpkeTagBytes, 0xA5U);
        CHECK(recipient.open(suite, vectors.hex(v, "enc"), vectors.hex(v, "info"), vectors.hex(second, "aad"), later,
                             Span<u8>(later_plain.data(), later_plain.size())) == CryptoError::Forged);
        CHECK(all_zero(later_plain));

        // Every export of the context (section 5.3), opened from the encapsulation.
        const u32 exports = json::find(vectors.doc(), v, "exports");
        REQUIRE(json::count_of(vectors.doc(), exports) == 3U); // an empty, a one-byte and a word context
        for (u32 e = 0U; e < 3U; ++e)
        {
            const u32   entry    = json::at(vectors.doc(), exports, e);
            const Bytes value = vectors.hex(entry, "exported_value");
            CHECK(vectors.number(entry, "L") == value.size());
            Bytes exported = filled(value.size(), 0xA5U);
            REQUIRE(recipient.export_secret(suite, vectors.hex(v, "enc"), vectors.hex(v, "info"),
                                            vectors.hex(entry, "exporter_context"),
                                            Span<u8>(exported.data(), exported.size())) == CryptoError::None);
            CHECK(equal(exported, value));
            ++exported_values;
        }
    }
    CHECK(exported_values == 18U);
    CHECK(in == 6U);       // DHKEM(P-256) and DHKEM(X25519) with HKDF-SHA256 and each AEAD
    CHECK(outside == 26U); // the other base-mode suites
}

TEST_CASE("HPKE: sealing round trips over every suite of the profile", "[crypto][net-2][hpke]")
{
    const u8 info[]      = {'c', 'e', 'r', 'i', 'd', ' ', 'h', 'p', 'k', 'e'};
    const u8 aad[]       = {0x01U, 0x02U, 0x03U};
    const u8 plaintext[] = {'g', 'r', 'o', 'u', 'p', ' ', 's', 'e', 'c', 'r', 'e', 't'};
    for (const HpkeSuite& suite : kSuites)
    {
        REQUIRE(hpke_supported(suite));
        Sealed sealed;
        seal_to(suite, info, aad, plaintext, sealed);
        Bytes opened = filled(sizeof(plaintext), 0U);
        REQUIRE(sealed.recipient.open(suite, sealed.enc, info, aad, sealed.ciphertext,
                                      Span<u8>(opened.data(), opened.size())) == CryptoError::None);
        CHECK(equal(opened, ConstSpan<u8>(plaintext)));

        // A second seal to the same key uses a fresh ephemeral key: another encapsulation, another ciphertext.
        Sealed again;
        again.enc.resize(sealed.enc.size());
        again.ciphertext.resize(sealed.ciphertext.size());
        REQUIRE(hpke_seal(suite, public_of(sealed.recipient), info, aad, plaintext,
                          Span<u8>(again.enc.data(), again.enc.size()),
                          Span<u8>(again.ciphertext.data(), again.ciphertext.size())) == CryptoError::None);
        CHECK_FALSE(equal(again.enc, sealed.enc));
        CHECK_FALSE(equal(again.ciphertext, sealed.ciphertext));

        // Another info, another associated data, another key, a changed encapsulation or ciphertext: forged, and no
        // byte released.
        const u8 other[] = {'x'};
        HpkeKey  stranger;
        REQUIRE(stranger.generate(suite.kem) == CryptoError::None);
        Bytes changed_ct = sealed.ciphertext;
        changed_ct[changed_ct.size() - 1U] ^= 0x01U; // the tag's last byte
        Bytes changed_enc = sealed.enc;
        changed_enc[changed_enc.size() - 1U] ^= 0x01U;
        const Span<u8> out(opened.data(), opened.size());
        CHECK(sealed.recipient.open(suite, sealed.enc, other, aad, sealed.ciphertext, out) == CryptoError::Forged);
        CHECK(all_zero(opened));
        CHECK(sealed.recipient.open(suite, sealed.enc, info, other, sealed.ciphertext, out) == CryptoError::Forged);
        CHECK(stranger.open(suite, sealed.enc, info, aad, sealed.ciphertext, out) == CryptoError::Forged);
        CHECK(sealed.recipient.open(suite, sealed.enc, info, aad, changed_ct, out) == CryptoError::Forged);
        const CryptoError tampered = sealed.recipient.open(suite, changed_enc, info, aad, sealed.ciphertext, out);
        // An X25519 encapsulation with a bit changed is another point; an EC one is usually off the curve.
        CHECK((tampered == CryptoError::Forged || tampered == CryptoError::BadKey));
        CHECK(all_zero(opened));

        // The same suite with another KDF or AEAD derives other keys.
        const HpkeSuite other_kdf{suite.kem,
                                  suite.kdf == AlgorithmId::HkdfSha256 ? AlgorithmId::HkdfSha384
                                                                       : AlgorithmId::HkdfSha256,
                                  suite.aead};
        CHECK(sealed.recipient.open(other_kdf, sealed.enc, info, aad, sealed.ciphertext, out) ==
              CryptoError::Forged);

        // An empty plaintext seals to the tag alone.
        Sealed empty;
        seal_to(suite, {}, {}, {}, empty);
        CHECK(empty.ciphertext.size() == kHpkeTagBytes);
        CHECK(empty.recipient.open(suite, empty.enc, {}, {}, empty.ciphertext, Span<u8>()) == CryptoError::None);
    }
}

TEST_CASE("HPKE: a long info is laid out beyond the inline buffer", "[crypto][net-2][hpke]")
{
    const HpkeSuite suite{AlgorithmId::X25519, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm};
    const Bytes     info        = filled(4096U, 0x5AU);
    const u8        plaintext[] = {1U, 2U, 3U};
    Sealed          sealed;
    seal_to(suite, info, {}, plaintext, sealed);
    Bytes opened = filled(sizeof(plaintext), 0U);
    REQUIRE(sealed.recipient.open(suite, sealed.enc, info, {}, sealed.ciphertext,
                                  Span<u8>(opened.data(), opened.size())) == CryptoError::None);
    CHECK(equal(opened, ConstSpan<u8>(plaintext)));
    Bytes shorter = info;
    shorter.resize(4095U);
    CHECK(sealed.recipient.open(suite, sealed.enc, shorter, {}, sealed.ciphertext,
                                Span<u8>(opened.data(), opened.size())) == CryptoError::Forged);
}

TEST_CASE("HPKE: exports agree on both sides of every suite, and their refusals", "[crypto][net-2][hpke]")
{
    const u8 info[]    = {'i', 'n', 'f', 'o'};
    const u8 context[] = {'M', 'L', 'S', ' ', 'e', 'x', 'p', 'o', 'r', 't'};
    for (const HpkeSuite& suite : kSuites)
    {
        HpkeKey recipient;
        REQUIRE(recipient.generate(suite.kem) == CryptoError::None);
        const Bytes peer = public_of(recipient);
        const usize nh   = suite.kdf == AlgorithmId::HkdfSha256 ? 32U : 48U;
        for (const usize length : {usize{1}, nh, 255U * nh})
        {
            Bytes enc      = filled(peer.size(), 0U);
            Bytes sent     = filled(length, 0U);
            Bytes received = filled(length, 0U);
            REQUIRE(hpke_export(suite, peer, info, context, Span<u8>(enc.data(), enc.size()),
                                Span<u8>(sent.data(), sent.size())) == CryptoError::None);
            REQUIRE(recipient.export_secret(suite, enc, info, context, Span<u8>(received.data(), received.size())) ==
                    CryptoError::None);
            CHECK(equal(sent, received));
            CHECK_FALSE(all_zero(sent));
        }
        // Another context, another info, another key or another KDF derive another secret; the exports of one
        // context are prefixes of no other length (the length is in the label).
        Bytes enc  = filled(peer.size(), 0U);
        Bytes sent = filled(nh, 0U);
        REQUIRE(hpke_export(suite, peer, info, context, Span<u8>(enc.data(), enc.size()),
                            Span<u8>(sent.data(), sent.size())) == CryptoError::None);
        Bytes      other = filled(nh, 0U);
        const auto again = [&](const HpkeKey& key, const HpkeSuite& as, ConstSpan<u8> i, ConstSpan<u8> c, usize n)
        {
            other = filled(n, 0U);
            REQUIRE(key.export_secret(as, enc, i, c, Span<u8>(other.data(), other.size())) == CryptoError::None);
            return equal(ConstSpan<u8>(other.data(), n), ConstSpan<u8>(sent.data(), n));
        };
        CHECK_FALSE(again(recipient, suite, info, ConstSpan<u8>(context, 3U), nh));
        CHECK_FALSE(again(recipient, suite, ConstSpan<u8>(), context, nh));
        CHECK_FALSE(again(recipient, suite, info, context, nh - 1U));
        HpkeKey stranger;
        REQUIRE(stranger.generate(suite.kem) == CryptoError::None);
        CHECK_FALSE(again(stranger, suite, info, context, nh));
        const HpkeSuite other_kdf{suite.kem,
                                  suite.kdf == AlgorithmId::HkdfSha256 ? AlgorithmId::HkdfSha384
                                                                       : AlgorithmId::HkdfSha256,
                                  suite.aead};
        CHECK_FALSE(again(recipient, other_kdf, info, context, 32U));
    }

    // Refusals: a key that holds none, another KEM, a suite outside the profile, lengths (0 and past 255 * Nh, an
    // encapsulation or peer of another length), overlapping buffers, a small-order X25519 peer; every output zeroed.
    const HpkeSuite x25519{AlgorithmId::X25519, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm};
    const HpkeSuite p256{AlgorithmId::Secp256r1, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm};
    HpkeKey         recipient;
    REQUIRE(recipient.generate(x25519.kem) == CryptoError::None);
    const Bytes    peer = public_of(recipient);
    Bytes          enc  = filled(32U, 0x11U);
    Bytes          out  = filled(32U, 0x11U);
    const Span<u8> encapsulation(enc.data(), enc.size());
    const Span<u8> exported(out.data(), out.size());
    REQUIRE(hpke_export(x25519, peer, {}, context, encapsulation, exported) == CryptoError::None);
    const Bytes good_enc = enc;
    HpkeKey     empty;
    CHECK(empty.export_secret(x25519, good_enc, {}, context, exported) == CryptoError::NotReady);
    CHECK(all_zero(out));
    CHECK(recipient.export_secret(p256, good_enc, {}, context, exported) == CryptoError::Unsupported);
    const HpkeSuite no_kdf{AlgorithmId::X25519, AlgorithmId::Aes128Gcm, AlgorithmId::Aes128Gcm};
    CHECK(recipient.export_secret(no_kdf, good_enc, {}, context, exported) == CryptoError::Unsupported);
    CHECK(hpke_export(no_kdf, peer, {}, context, encapsulation, exported) == CryptoError::Unsupported);
    CHECK(recipient.export_secret(x25519, good_enc, {}, context, Span<u8>()) == CryptoError::BadLength);
    Bytes too_long = filled(255U * 32U + 1U, 0U);
    CHECK(recipient.export_secret(x25519, good_enc, {}, context, Span<u8>(too_long.data(), too_long.size())) ==
          CryptoError::BadLength);
    CHECK(hpke_export(x25519, peer, {}, context, encapsulation, Span<u8>(too_long.data(), too_long.size())) ==
          CryptoError::BadLength);
    CHECK(all_zero(too_long));
    CHECK(recipient.export_secret(x25519, ConstSpan<u8>(good_enc.data(), 31U), {}, context, exported) ==
          CryptoError::BadLength);
    CHECK(hpke_export(x25519, ConstSpan<u8>(peer.data(), 31U), {}, context, encapsulation, exported) ==
          CryptoError::BadLength);
    CHECK(hpke_export(x25519, peer, {}, context, Span<u8>(enc.data(), 31U), exported) == CryptoError::BadLength);
    Bytes shared = filled(64U, 0U);
    CHECK(hpke_export(x25519, peer, {}, context, Span<u8>(shared.data(), 32U), Span<u8>(shared.data() + 16U, 32U)) ==
          CryptoError::Overlap);
    Bytes context_bytes = filled(32U, 3U);
    CHECK(hpke_export(x25519, peer, {}, context_bytes, encapsulation, Span<u8>(context_bytes.data(), 32U)) ==
          CryptoError::Overlap);
    Bytes in_place = good_enc;
    CHECK(recipient.export_secret(x25519, in_place, {}, context, Span<u8>(in_place.data(), 32U)) ==
          CryptoError::Overlap);
    const Bytes zero_point = filled(32U, 0U);
    std::memset(out.data(), 0x11, out.size());
    CHECK(recipient.export_secret(x25519, zero_point, {}, context, exported) == CryptoError::WeakSecret);
    CHECK(all_zero(out));
    std::memset(enc.data(), 0x11, enc.size());
    std::memset(out.data(), 0x11, out.size());
    CHECK(hpke_export(x25519, zero_point, {}, context, encapsulation, exported) == CryptoError::WeakSecret);
    CHECK(all_zero(enc));
    CHECK(all_zero(out));
}

TEST_CASE("HPKE: keys and refusals", "[crypto][net-2][hpke]")
{
    // Sizes answer for the profile's DHKEMs only.
    CHECK(hpke_sizes(AlgorithmId::X25519).public_key == 32U);
    CHECK(hpke_sizes(AlgorithmId::X25519).private_key == 32U);
    CHECK(hpke_sizes(AlgorithmId::Secp256r1).public_key == 65U);
    CHECK(hpke_sizes(AlgorithmId::Secp384r1).private_key == 48U);
    CHECK(hpke_sizes(AlgorithmId::MlKem768).public_key == 0U);
    CHECK(hpke_sizes(AlgorithmId::Ed25519).public_key == 0U);

    // Suites outside the profile.
    CHECK_FALSE(hpke_supported({AlgorithmId::Ed25519, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm}));
    CHECK_FALSE(hpke_supported({AlgorithmId::X25519, AlgorithmId::Aes128Gcm, AlgorithmId::Aes128Gcm}));
    CHECK_FALSE(hpke_supported({AlgorithmId::X25519, AlgorithmId::HkdfSha256, AlgorithmId::HkdfSha256}));
    CHECK_FALSE(hpke_supported({}));

    HpkeKey key;
    CHECK_FALSE(key.ready());
    CHECK(key.generate(AlgorithmId::MlKem768) == CryptoError::Unsupported);
    CHECK(key.derive(AlgorithmId::Ed25519, filled(32U, 1U)) == CryptoError::Unsupported);
    CHECK(key.from_private(AlgorithmId::Aes128Gcm, filled(32U, 1U)) == CryptoError::Unsupported);
    // DeriveKeyPair takes at least Nsk bytes.
    CHECK(key.derive(AlgorithmId::X25519, filled(31U, 1U)) == CryptoError::BadLength);
    CHECK(key.derive(AlgorithmId::Secp384r1, filled(47U, 1U)) == CryptoError::BadLength);
    CHECK_FALSE(key.ready());
    // A P-256 scalar of zero, or at the group order, is no key.
    CHECK(key.from_private(AlgorithmId::Secp256r1, filled(32U, 0U)) == CryptoError::BadKey);
    CHECK(key.from_private(AlgorithmId::Secp256r1, filled(32U, 0xFFU)) == CryptoError::BadKey);
    CHECK(key.from_private(AlgorithmId::X25519, filled(31U, 1U)) == CryptoError::BadLength);

    // DeriveKeyPair is a function of its input; its key is a private key like any other.
    HpkeKey a;
    HpkeKey b;
    for (const AlgorithmId kem : {AlgorithmId::X25519, AlgorithmId::Secp256r1, AlgorithmId::Secp384r1})
    {
        const Bytes ikm = filled(hpke_sizes(kem).private_key + 16U, 0x42U);
        REQUIRE(a.derive(kem, ikm) == CryptoError::None);
        REQUIRE(b.derive(kem, ikm) == CryptoError::None);
        CHECK(a.kem() == kem);
        CHECK(equal(public_of(a), public_of(b)));
        Bytes other = ikm;
        other[0] ^= 0x01U;
        REQUIRE(b.derive(kem, other) == CryptoError::None);
        CHECK_FALSE(equal(public_of(a), public_of(b)));
    }

    const HpkeSuite suite{AlgorithmId::Secp256r1, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm};
    const u8        plaintext[] = {9U, 8U, 7U, 6U};
    Sealed          sealed;
    seal_to(suite, {}, {}, plaintext, sealed);
    Bytes          opened = filled(sizeof(plaintext), 0x33U);
    const Span<u8> out(opened.data(), opened.size());

    // A key that holds none, a suite of another KEM, a suite outside the profile.
    HpkeKey empty;
    CHECK(empty.open(suite, sealed.enc, {}, {}, sealed.ciphertext, out) == CryptoError::NotReady);
    CHECK(all_zero(opened));
    const HpkeSuite x25519{AlgorithmId::X25519, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm};
    CHECK(sealed.recipient.open(x25519, sealed.enc, {}, {}, sealed.ciphertext, out) == CryptoError::Unsupported);
    const HpkeSuite no_aead{AlgorithmId::Secp256r1, AlgorithmId::HkdfSha256, AlgorithmId::Ed25519};
    CHECK(sealed.recipient.open(no_aead, sealed.enc, {}, {}, sealed.ciphertext, out) == CryptoError::Unsupported);

    // Lengths.
    CHECK(sealed.recipient.open(suite, ConstSpan<u8>(sealed.enc.data(), 64U), {}, {}, sealed.ciphertext, out) ==
          CryptoError::BadLength);
    CHECK(sealed.recipient.open(suite, sealed.enc, {}, {}, ConstSpan<u8>(sealed.ciphertext.data(), 15U),
                                Span<u8>()) == CryptoError::BadLength);
    CHECK(sealed.recipient.open(suite, sealed.enc, {}, {}, sealed.ciphertext, Span<u8>(opened.data(), 3U)) ==
          CryptoError::BadLength);

    // An encapsulation off the curve, or in compressed form, is BadKey; the output is zeroed.
    std::memset(opened.data(), 0x33, opened.size());
    Bytes off_curve = sealed.enc;
    off_curve[64]   = static_cast<u8>(off_curve[64] ^ 0x01U);
    CHECK(sealed.recipient.open(suite, off_curve, {}, {}, sealed.ciphertext, out) == CryptoError::BadKey);
    CHECK(all_zero(opened));
    Bytes compressed = sealed.enc;
    compressed[0]    = 0x02U;
    CHECK(sealed.recipient.open(suite, compressed, {}, {}, sealed.ciphertext, out) == CryptoError::BadKey);

    // Overlapping buffers.
    Bytes in_place = sealed.ciphertext;
    CHECK(sealed.recipient.open(suite, sealed.enc, {}, {}, in_place, Span<u8>(in_place.data(), sizeof(plaintext))) ==
          CryptoError::Overlap);

    // Sealing: to a key off the curve, to a key of another length, with outputs of other lengths or overlapping.
    const Bytes    recipient = public_of(sealed.recipient);
    Bytes          enc       = filled(65U, 0x11U);
    Bytes          ct        = filled(sizeof(plaintext) + kHpkeTagBytes, 0x11U);
    const Span<u8> enc_out(enc.data(), enc.size());
    const Span<u8> ct_out(ct.data(), ct.size());
    Bytes          bad_peer = recipient;
    bad_peer[64]            = static_cast<u8>(bad_peer[64] ^ 0x01U);
    CHECK(hpke_seal(suite, bad_peer, {}, {}, plaintext, enc_out, ct_out) == CryptoError::BadKey);
    CHECK(all_zero(enc));
    CHECK(all_zero(ct));
    CHECK(hpke_seal(suite, ConstSpan<u8>(recipient.data(), 64U), {}, {}, plaintext, enc_out, ct_out) ==
          CryptoError::BadLength);
    CHECK(hpke_seal(suite, recipient, {}, {}, plaintext, Span<u8>(enc.data(), 64U), ct_out) ==
          CryptoError::BadLength);
    CHECK(hpke_seal(suite, recipient, {}, {}, plaintext, enc_out, Span<u8>(ct.data(), 4U)) ==
          CryptoError::BadLength);
    CHECK(hpke_seal(x25519, recipient, {}, {}, plaintext, enc_out, ct_out) == CryptoError::BadLength);
    CHECK(hpke_seal(no_aead, recipient, {}, {}, plaintext, enc_out, ct_out) == CryptoError::Unsupported);
    Bytes shared = filled(65U + sizeof(plaintext) + kHpkeTagBytes, 0U);
    CHECK(hpke_seal(suite, recipient, {}, {}, plaintext, Span<u8>(shared.data(), 65U),
                    Span<u8>(shared.data() + 64U, sizeof(plaintext) + kHpkeTagBytes)) == CryptoError::Overlap);
    Bytes text = filled(sizeof(plaintext) + kHpkeTagBytes, 7U);
    CHECK(hpke_seal(suite, recipient, {}, {}, ConstSpan<u8>(text.data(), sizeof(plaintext)), enc_out,
                    Span<u8>(text.data(), text.size())) == CryptoError::Overlap);

    // X25519: an encapsulation or a peer of small order (the all-zero point) gives the all-zero secret, refused
    // (RFC 9180 section 7.1.4).
    Sealed x;
    seal_to(x25519, {}, {}, plaintext, x);
    const Bytes zero_point = filled(32U, 0U);
    CHECK(x.recipient.open(x25519, zero_point, {}, {}, x.ciphertext, out) == CryptoError::WeakSecret);
    CHECK(all_zero(opened));
    Bytes x_enc = filled(32U, 0x11U);
    CHECK(hpke_seal(x25519, zero_point, {}, {}, plaintext, Span<u8>(x_enc.data(), x_enc.size()), ct_out) ==
          CryptoError::WeakSecret);
    CHECK(all_zero(x_enc));

    // A cleared key holds none.
    sealed.recipient.clear();
    CHECK_FALSE(sealed.recipient.ready());
    CHECK(sealed.recipient.open(suite, sealed.enc, {}, {}, sealed.ciphertext, out) == CryptoError::NotReady);
}
} // namespace
