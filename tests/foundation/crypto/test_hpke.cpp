// crd-crypto -- hybrid public key encryption (NET.2; docs/design/network-foundation.md#hpke): every base-mode vector
// of RFC 9180's test-vectors.json and of draft-ietf-hpke-pq's in the profile (DeriveKeyPair for the recipient and,
// for a DHKEM, the ephemeral key, the private keys' serialization, the opening of each context's first message and
// each of its exports), sealing and exporting round trips over every suite, the post-quantum and hybrid KEMs' own
// rules (implicit rejection, a hybrid's X25519 half taken as computed, its NIST-curve half refused off the curve, the
// public key checks), and every refusal: suites outside the profile, lengths, keys off the curve, small-order X25519
// keys, forgeries, overlapping buffers and keys that hold none.

#include "wycheproof.hpp"

#include <crd/crypto/hpke.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <utility>

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
namespace json = crd::json;

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
    case iana::kHpkeKemMlKem768:
        return AlgorithmId::MlKem768;
    case iana::kHpkeKemMlKem1024:
        return AlgorithmId::MlKem1024;
    case iana::kHpkeKemXWing:
        return AlgorithmId::X25519Mlkem768;
    case iana::kHpkeKemMlkem768P256:
        return AlgorithmId::Secp256r1Mlkem768;
    case iana::kHpkeKemMlkem1024P384:
        return AlgorithmId::Secp384r1Mlkem1024;
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

// The vector's identifiers name `suite` through the public mapping both ways (hpke_codepoints, hpke_suite_of: what an
// ECHConfig's suites are read with), checked against this file's own mapping above.
void check_codepoints(const HpkeSuite& suite, u64 kem, u64 kdf, u64 aead)
{
    const HpkeCodepoints codepoints = hpke_codepoints(suite);
    CHECK(codepoints.kem == kem);
    CHECK(codepoints.kdf == kdf);
    CHECK(codepoints.aead == aead);
    HpkeSuite read;
    REQUIRE(hpke_suite_of(codepoints, read));
    CHECK(read.kem == suite.kem);
    CHECK(read.kdf == suite.kdf);
    CHECK(read.aead == suite.aead);
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
    {AlgorithmId::MlKem768, AlgorithmId::HkdfSha384, AlgorithmId::Aes256Gcm},
    {AlgorithmId::MlKem1024, AlgorithmId::HkdfSha256, AlgorithmId::Chacha20Poly1305},
    {AlgorithmId::X25519Mlkem768, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm},
    {AlgorithmId::Secp256r1Mlkem768, AlgorithmId::HkdfSha384, AlgorithmId::Chacha20Poly1305},
    {AlgorithmId::Secp384r1Mlkem1024, AlgorithmId::HkdfSha384, AlgorithmId::Aes256Gcm},
};

// The KEMs of the profile beyond the DHKEMs: ML-KEM and the hybrids.
constexpr AlgorithmId kPostQuantumKems[] = {AlgorithmId::MlKem768, AlgorithmId::MlKem1024,
                                            AlgorithmId::X25519Mlkem768, AlgorithmId::Secp256r1Mlkem768,
                                            AlgorithmId::Secp384r1Mlkem1024};

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
    out.enc.resize(hpke_sizes(suite.kem).encapsulation);
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
            HpkeSuite read;
            CHECK_FALSE(hpke_suite_of({static_cast<crd::u16>(vectors.number(v, "kem_id")),
                                       static_cast<crd::u16>(vectors.number(v, "kdf_id")),
                                       static_cast<crd::u16>(vectors.number(v, "aead_id"))},
                                      read));
            CHECK_FALSE(hpke_supported(read));
            ++outside;
            continue;
        }
        check_codepoints(suite, vectors.number(v, "kem_id"), vectors.number(v, "kdf_id"), vectors.number(v, "aead_id"));
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
            Bytes enc      = filled(hpke_sizes(suite.kem).encapsulation, 0U);
            // Unlike fillings make agreement prove both sides wrote; a one-byte export is zero once in 256 runs, so
            // only an export of at least Nh bytes is checked to be nonzero.
            Bytes sent     = filled(length, 0xA5U);
            Bytes received = filled(length, 0x5AU);
            REQUIRE(hpke_export(suite, peer, info, context, Span<u8>(enc.data(), enc.size()),
                                Span<u8>(sent.data(), sent.size())) == CryptoError::None);
            REQUIRE(recipient.export_secret(suite, enc, info, context, Span<u8>(received.data(), received.size())) ==
                    CryptoError::None);
            CHECK(equal(sent, received));
            if (length >= nh)
            {
                CHECK_FALSE(all_zero(sent));
            }
        }
        // Another context, another info, another key or another KDF derive another secret; the exports of one
        // context are prefixes of no other length (the length is in the label).
        Bytes enc  = filled(hpke_sizes(suite.kem).encapsulation, 0U);
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
    // Sizes answer for the profile's KEMs only: Npk, Nenc and Nsk (RFC 9180 section 7.1; draft-ietf-hpke-pq Table 2
    // and section 4, as IANA's HPKE registry lists them).
    const auto sizes = [](AlgorithmId kem, usize npk, usize nenc, usize nsk)
    {
        const HpkeSizes got = hpke_sizes(kem);
        return got.public_key == npk && got.encapsulation == nenc && got.private_key == nsk;
    };
    CHECK(sizes(AlgorithmId::X25519, 32U, 32U, 32U));
    CHECK(sizes(AlgorithmId::Secp256r1, 65U, 65U, 32U));
    CHECK(sizes(AlgorithmId::Secp384r1, 97U, 97U, 48U));
    CHECK(sizes(AlgorithmId::MlKem768, 1184U, 1088U, 64U));
    CHECK(sizes(AlgorithmId::MlKem1024, 1568U, 1568U, 64U));
    CHECK(sizes(AlgorithmId::X25519Mlkem768, 1216U, 1120U, 32U));
    CHECK(sizes(AlgorithmId::Secp256r1Mlkem768, 1249U, 1153U, 32U));
    CHECK(sizes(AlgorithmId::Secp384r1Mlkem1024, 1665U, 1665U, 32U));
    CHECK(sizes(AlgorithmId::Ed25519, 0U, 0U, 0U));
    CHECK(sizes(AlgorithmId::MlDsa65, 0U, 0U, 0U));

    // Suites outside the profile.
    CHECK_FALSE(hpke_supported({AlgorithmId::Ed25519, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm}));
    CHECK_FALSE(hpke_supported({AlgorithmId::X25519, AlgorithmId::Aes128Gcm, AlgorithmId::Aes128Gcm}));
    CHECK_FALSE(hpke_supported({AlgorithmId::X25519, AlgorithmId::HkdfSha256, AlgorithmId::HkdfSha256}));
    CHECK_FALSE(hpke_supported({}));

    HpkeKey key;
    CHECK_FALSE(key.ready());
    CHECK(key.generate(AlgorithmId::MlDsa65) == CryptoError::Unsupported);
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
TEST_CASE("HPKE: draft-ietf-hpke-pq's base-mode vectors", "[crypto][net-2][hpke]")
{
    const JsonFile vectors(crd::containers::String(CRD_HPKE_PQ_VECTORS));
    const u32      count   = json::count_of(vectors.doc(), vectors.root());
    usize          in              = 0U;
    usize          outside         = 0U;
    usize          opened          = 0U;
    usize          exported_values = 0U;
    REQUIRE(count == 13U);
    for (u32 i = 0U; i < count; ++i)
    {
        const u32 v = json::at(vectors.doc(), vectors.root(), i);
        REQUIRE(vectors.number(v, "mode") == 0U);
        const HpkeSuite suite{kem_of(vectors.number(v, "kem_id")), kdf_of(vectors.number(v, "kdf_id")),
                              aead_of(vectors.number(v, "aead_id"))};
        INFO("kem " << vectors.number(v, "kem_id") << " kdf " << vectors.number(v, "kdf_id") << " aead "
                    << vectors.number(v, "aead_id"));
        if (!hpke_supported(suite))
        {
            // ML-KEM-512 and the single-stage SHAKE and TurboSHAKE KDFs are outside the profile.
            ++outside;
            continue;
        }
        check_codepoints(suite, vectors.number(v, "kem_id"), vectors.number(v, "kdf_id"), vectors.number(v, "aead_id"));
        ++in;
        // DeriveKeyPair and the serialized private key give the vector's public key.
        HpkeKey derived;
        HpkeKey serialized;
        REQUIRE(derived.derive(suite.kem, vectors.hex(v, "ikmR")) == CryptoError::None);
        REQUIRE(serialized.from_private(suite.kem, vectors.hex(v, "skRm")) == CryptoError::None);
        CHECK(equal(public_of(derived), vectors.hex(v, "pkRm")));
        CHECK(equal(public_of(serialized), vectors.hex(v, "pkRm")));
        CHECK(hpke_check_public_key(suite.kem, vectors.hex(v, "pkRm")) == CryptoError::None);

        // Each message of the context opens only as the first one (the vectors' sequence number 0); the others,
        // sealed under later nonces, do not authenticate.
        const Bytes enc         = vectors.hex(v, "enc");
        const Bytes info        = vectors.hex(v, "info");
        const u32   encryptions = json::find(vectors.doc(), v, "encryptions");
        const u32   messages    = json::count_of(vectors.doc(), encryptions);
        REQUIRE(messages == 10U);
        for (u32 m = 0U; m < messages; ++m)
        {
            const u32   message    = json::at(vectors.doc(), encryptions, m);
            const Bytes ciphertext = vectors.hex(message, "ct");
            Bytes       plaintext  = filled(ciphertext.size() - kHpkeTagBytes, 0xA5U);
            const auto  result     = derived.open(suite, enc, info, vectors.hex(message, "aad"), ciphertext,
                                                  Span<u8>(plaintext.data(), plaintext.size()));
            if (m == 0U)
            {
                CHECK(equal(vectors.hex(message, "nonce"), vectors.hex(v, "base_nonce")));
                REQUIRE(result == CryptoError::None);
                CHECK(equal(plaintext, vectors.hex(message, "pt")));
                ++opened;
            }
            else
            {
                CHECK(result == CryptoError::Forged);
                CHECK(all_zero(plaintext));
            }
        }

        // Every export of the context, from both keys.
        const u32 exports = json::find(vectors.doc(), v, "exports");
        REQUIRE(json::count_of(vectors.doc(), exports) == 5U);
        for (u32 e = 0U; e < 5U; ++e)
        {
            const u32   entry = json::at(vectors.doc(), exports, e);
            const Bytes value = vectors.hex(entry, "exported_value");
            CHECK(vectors.number(entry, "L") == value.size());
            Bytes from_derived    = filled(value.size(), 0xA5U);
            Bytes from_serialized = filled(value.size(), 0xA5U);
            REQUIRE(derived.export_secret(suite, enc, info, vectors.hex(entry, "exporter_context"),
                                          Span<u8>(from_derived.data(), from_derived.size())) == CryptoError::None);
            REQUIRE(serialized.export_secret(suite, enc, info, vectors.hex(entry, "exporter_context"),
                                             Span<u8>(from_serialized.data(), from_serialized.size())) ==
                    CryptoError::None);
            CHECK(equal(from_derived, value));
            CHECK(equal(from_serialized, value));
            ++exported_values;
        }

        // The derived key's seed is the vector's private key in full: its ML-KEM half's implicit rejection, a function
        // of z, which the public key does not show, agrees with the serialized key's for a changed ciphertext.
        Bytes changed = enc;
        changed[0] ^= 0x01U;
        Bytes a = filled(32U, 0U);
        Bytes b = filled(32U, 0U);
        REQUIRE(derived.export_secret(suite, changed, info, {}, Span<u8>(a.data(), a.size())) == CryptoError::None);
        REQUIRE(serialized.export_secret(suite, changed, info, {}, Span<u8>(b.data(), b.size())) ==
                CryptoError::None);
        CHECK(equal(a, b));
        HpkeKey other;
        Bytes   other_seed = vectors.hex(v, "skRm");
        other_seed[other_seed.size() - 1U] ^= 0x01U;
        REQUIRE(other.from_private(suite.kem, other_seed) == CryptoError::None);
        REQUIRE(other.export_secret(suite, changed, info, {}, Span<u8>(b.data(), b.size())) == CryptoError::None);
        CHECK_FALSE(equal(a, b));
    }
    CHECK(in == 5U);       // ML-KEM-768, ML-KEM-1024 and the three hybrids with HKDF
    CHECK(outside == 8U);  // ML-KEM-512 and the single-stage KDFs
    CHECK(opened == 5U);
    CHECK(exported_values == 25U);
}

TEST_CASE("HPKE: the post-quantum and hybrid KEMs' keys, checks and refusals", "[crypto][net-2][hpke]")
{
    const u8 info[]      = {'p', 'q'};
    const u8 plaintext[] = {'k', 'e', 'y'};

    // DeriveKeyPair takes 32 bytes or more, is a function of its input, and its key is one from_private takes.
    for (const AlgorithmId kem : kPostQuantumKems)
    {
        INFO("kem " << static_cast<u32>(kem));
        const HpkeSizes sizes = hpke_sizes(kem);
        HpkeKey         key;
        CHECK(key.derive(kem, filled(31U, 1U)) == CryptoError::BadLength);
        CHECK_FALSE(key.ready());
        REQUIRE(key.derive(kem, filled(32U, 1U)) == CryptoError::None);
        CHECK(key.kem() == kem);
        HpkeKey same;
        REQUIRE(same.derive(kem, filled(32U, 1U)) == CryptoError::None);
        CHECK(equal(public_of(key), public_of(same)));
        REQUIRE(same.derive(kem, filled(32U, 2U)) == CryptoError::None);
        CHECK_FALSE(equal(public_of(key), public_of(same)));
        CHECK(key.from_private(kem, filled(sizes.private_key - 1U, 1U)) == CryptoError::BadLength);
        CHECK(key.from_private(kem, filled(sizes.private_key + 1U, 1U)) == CryptoError::BadLength);
        CHECK_FALSE(key.ready());
        // Every seed is a key, the all-zero one included.
        REQUIRE(key.from_private(kem, filled(sizes.private_key, 0U)) == CryptoError::None);
        CHECK(hpke_check_public_key(kem, public_of(key)) == CryptoError::None);
        Bytes public_bytes = filled(sizes.public_key, 0U);
        CHECK(key.public_key(Span<u8>(public_bytes.data(), sizes.public_key - 1U)) == CryptoError::BadLength);

        // A moved key keeps its pair; the key it left holds none.
        const Bytes before = public_of(key);
        HpkeKey     moved(std::move(key));
        CHECK_FALSE(key.ready()); // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved): the moved-from state
        CHECK(equal(public_of(moved), before));
        HpkeKey assigned;
        REQUIRE(assigned.generate(kem) == CryptoError::None);
        assigned = std::move(moved);
        CHECK(equal(public_of(assigned), before));
        CHECK_FALSE(moved.ready()); // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
    }

    // ML-KEM: a changed ciphertext decapsulates by implicit rejection, so the message is forged, never refused; the
    // encapsulation key must pass FIPS 203's input check (a coefficient of 4095, at least q).
    const HpkeSuite mlkem{AlgorithmId::MlKem768, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm};
    Sealed          sealed;
    seal_to(mlkem, info, {}, plaintext, sealed);
    Bytes          opened = filled(sizeof(plaintext), 0x33U);
    const Span<u8> out(opened.data(), opened.size());
    Bytes          changed = sealed.enc;
    changed[17] ^= 0x40U;
    CHECK(sealed.recipient.open(mlkem, changed, info, {}, sealed.ciphertext, out) == CryptoError::Forged);
    CHECK(all_zero(opened));
    Bytes bad_key = public_of(sealed.recipient);
    bad_key[0]    = 0xFFU;
    bad_key[1]    = static_cast<u8>(bad_key[1] | 0x0FU);
    CHECK(hpke_check_public_key(mlkem.kem, bad_key) == CryptoError::BadKey);
    Bytes          enc = filled(hpke_sizes(mlkem.kem).encapsulation, 0x11U);
    Bytes          ct  = filled(sizeof(plaintext) + kHpkeTagBytes, 0x11U);
    const Span<u8> enc_out(enc.data(), enc.size());
    const Span<u8> ct_out(ct.data(), ct.size());
    CHECK(hpke_seal(mlkem, bad_key, info, {}, plaintext, enc_out, ct_out) == CryptoError::BadKey);
    CHECK(all_zero(enc));
    CHECK(all_zero(ct));
    CHECK(hpke_seal(mlkem, public_of(sealed.recipient), info, {}, plaintext, Span<u8>(enc.data(), 1087U), ct_out) ==
          CryptoError::BadLength);
    CHECK(sealed.recipient.open(mlkem, ConstSpan<u8>(sealed.enc.data(), 1087U), info, {}, sealed.ciphertext, out) ==
          CryptoError::BadLength);
    const HpkeSuite mlkem_1024{AlgorithmId::MlKem1024, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm};
    CHECK(sealed.recipient.open(mlkem_1024, sealed.enc, info, {}, sealed.ciphertext, out) ==
          CryptoError::Unsupported);

    // MLKEM768-X25519 (X-Wing): the traditional half is any 32 bytes; an all-zero encapsulation element gives the
    // all-zero X25519 secret, which the combiner binds, so the exporter derives a secret rather than refusing (and the
    // first message, sealed under another element, is forged). An all-zero recipient half seals too.
    const HpkeSuite xwing{AlgorithmId::X25519Mlkem768, AlgorithmId::HkdfSha256, AlgorithmId::Aes128Gcm};
    Sealed          x;
    seal_to(xwing, info, {}, plaintext, x);
    Bytes zero_element = x.enc;
    std::memset(zero_element.data() + 1088U, 0, 32U);
    Bytes secret = filled(32U, 0U);
    CHECK(x.recipient.export_secret(xwing, zero_element, info, {}, Span<u8>(secret.data(), secret.size())) ==
          CryptoError::None);
    CHECK_FALSE(all_zero(secret));
    CHECK(x.recipient.open(xwing, zero_element, info, {}, x.ciphertext, out) == CryptoError::Forged);
    Bytes zero_half = public_of(x.recipient);
    std::memset(zero_half.data() + 1184U, 0, 32U);
    CHECK(hpke_check_public_key(xwing.kem, zero_half) == CryptoError::None);
    Bytes x_enc = filled(1120U, 0U);
    CHECK(hpke_seal(xwing, zero_half, info, {}, plaintext, Span<u8>(x_enc.data(), x_enc.size()), ct_out) ==
          CryptoError::None);

    // MLKEM768-P256 and MLKEM1024-P384: an element or a recipient half off the curve is refused explicitly (a public
    // input, draft-irtf-cfrg-hybrid-kems section 4.2), as is an ML-KEM half failing the input check.
    for (const AlgorithmId kem : {AlgorithmId::Secp256r1Mlkem768, AlgorithmId::Secp384r1Mlkem1024})
    {
        INFO("kem " << static_cast<u32>(kem));
        const HpkeSuite suite{kem, AlgorithmId::HkdfSha384, AlgorithmId::Aes256Gcm};
        const usize     pq_public     = kem == AlgorithmId::Secp256r1Mlkem768 ? 1184U : 1568U;
        const usize     pq_ciphertext = kem == AlgorithmId::Secp256r1Mlkem768 ? 1088U : 1568U;
        Sealed          hybrid;
        seal_to(suite, info, {}, plaintext, hybrid);
        Bytes off_curve = hybrid.enc;
        off_curve[off_curve.size() - 1U] ^= 0x01U;
        std::memset(opened.data(), 0x33, opened.size());
        CHECK(hybrid.recipient.open(suite, off_curve, info, {}, hybrid.ciphertext, out) == CryptoError::BadKey);
        CHECK(all_zero(opened));
        Bytes compressed          = hybrid.enc;
        compressed[pq_ciphertext] = 0x02U;
        CHECK(hybrid.recipient.open(suite, compressed, info, {}, hybrid.ciphertext, out) == CryptoError::BadKey);
        Bytes bad_half = public_of(hybrid.recipient);
        bad_half[bad_half.size() - 1U] ^= 0x01U;
        CHECK(hpke_check_public_key(kem, bad_half) == CryptoError::BadKey);
        Bytes          hybrid_enc = filled(hpke_sizes(kem).encapsulation, 0x11U);
        const Span<u8> hybrid_out(hybrid_enc.data(), hybrid_enc.size());
        CHECK(hpke_seal(suite, bad_half, info, {}, plaintext, hybrid_out, ct_out) == CryptoError::BadKey);
        CHECK(all_zero(hybrid_enc));
        Bytes bad_pq = public_of(hybrid.recipient);
        bad_pq[0]    = 0xFFU;
        bad_pq[1]    = static_cast<u8>(bad_pq[1] | 0x0FU);
        CHECK(hpke_check_public_key(kem, bad_pq) == CryptoError::BadKey);
        CHECK(hpke_seal(suite, bad_pq, info, {}, plaintext, hybrid_out, ct_out) == CryptoError::BadKey);
        CHECK(hpke_check_public_key(kem, ConstSpan<u8>(bad_pq.data(), pq_public)) == CryptoError::BadLength);
    }

    // The checks of the DHKEMs are the agreement's; a KEM outside the profile has none.
    HpkeKey dhkem;
    REQUIRE(dhkem.generate(AlgorithmId::X25519) == CryptoError::None);
    CHECK(hpke_check_public_key(AlgorithmId::X25519, public_of(dhkem)) == CryptoError::None);
    CHECK(hpke_check_public_key(AlgorithmId::X25519, filled(32U, 0U)) == CryptoError::WeakSecret);
    CHECK(hpke_check_public_key(AlgorithmId::Secp256r1, filled(65U, 4U)) == CryptoError::BadKey);
    CHECK(hpke_check_public_key(AlgorithmId::MlDsa65, filled(32U, 0U)) == CryptoError::Unsupported);
    CHECK(hpke_check_public_key(AlgorithmId::MlKem768, filled(1183U, 0U)) == CryptoError::BadLength);
}

TEST_CASE("HPKE: a hybrid's P-256 scalar is the first chunk of its seed below the group order", "[crypto][net-2][hpke]")
{
    // An MLKEM768-P256 seed whose expansion's first scalar chunk (SHAKE256(seed) bytes 64 to 95) begins ffffffff53e7,
    // above the P-256 order (found by search, about one seed in 2^32): RandomScalar takes the second chunk (bytes 96
    // to 127; draft-irtf-cfrg-concrete-hybrid-kems section 3.1.1), and the ML-KEM half is the first 64 bytes' key.
    const Bytes seed             = from_hex("77057225989bcf24bf83be4d4be80f41145df0f22754338a6a71473973394471");
    u8          full[64U + 128U] = {};
    REQUIRE(shake256(seed, full) == CryptoError::None);
    AgreementKey rejected;
    CHECK(rejected.from_private(AlgorithmId::Secp256r1, ConstSpan<u8>(full + 64U, 32U)) == CryptoError::BadKey);
    AgreementKey scalar;
    REQUIRE(scalar.from_private(AlgorithmId::Secp256r1, ConstSpan<u8>(full + 96U, 32U)) == CryptoError::None);
    Bytes group_half = filled(65U, 0U);
    REQUIRE(scalar.public_key(Span<u8>(group_half.data(), group_half.size())) == CryptoError::None);
    KemKey pq;
    REQUIRE(pq.from_seed(AlgorithmId::MlKem768, ConstSpan<u8>(full, 64U)) == CryptoError::None);
    Bytes pq_half = filled(1184U, 0U);
    REQUIRE(pq.public_key(Span<u8>(pq_half.data(), pq_half.size())) == CryptoError::None);

    HpkeKey key;
    REQUIRE(key.from_private(AlgorithmId::Secp256r1Mlkem768, seed) == CryptoError::None);
    const Bytes public_key = public_of(key);
    CHECK(equal(ConstSpan<u8>(public_key.data(), 1184U), pq_half));
    CHECK(equal(ConstSpan<u8>(public_key.data() + 1184U, 65U), group_half));
}
} // namespace
