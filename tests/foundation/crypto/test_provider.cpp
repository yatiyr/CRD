// crd-crypto -- the provider interface (NET.2; docs/design/network-foundation.md#provider): every operation against
// Project Wycheproof's vectors (the pinned C2SP/wycheproof testvectors_v1, read at run time) and RFC 8032's
// deterministic signatures, every refusal and length rule, in-place and aliasing rules, zeroing on failure, keys
// generated, moved and cleared, concurrent use of one key, and a policy generation signed by real ML-DSA and
// Ed25519 keys through the production verifier.

#include "wycheproof.hpp"

#include <crd/crypto/policy.hpp>
#include <crd/crypto/provider.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
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
using crd::containers::StringView;
using namespace crd::crypto;
using namespace crd::crypto::test;
namespace json = crd::assetio::json;

// ---- Provider identity and helpers ----

TEST_CASE("Provider: identity, the implemented set and error names", "[crypto][net-2]")
{
    CHECK(provider_name() == StringView("AWS-LC 5.12.0"));
    const ConstSpan<AlgorithmId> implemented = provider_algorithms();
    REQUIRE(implemented.size() == 20U);
    for (usize i = 0U; i < implemented.size(); ++i)
    {
        CHECK(provider_implements(implemented[i]));
        if (i > 0U)
        {
            CHECK(static_cast<crd::u16>(implemented[i - 1U]) < static_cast<crd::u16>(implemented[i]));
        }
    }
    // The hybrid groups run inside the TLS stack, proven by test_tls.cpp and against OpenSSL (decision P6).
    CHECK(provider_implements(AlgorithmId::X25519Mlkem768));
    CHECK(provider_implements(AlgorithmId::Secp256r1Mlkem768));
    CHECK(provider_implements(AlgorithmId::Secp384r1Mlkem1024));
    CHECK_FALSE(provider_implements(AlgorithmId{}));

    // Each table answers only for its own class.
    for (const AlgorithmId id : kAlgorithmIds)
    {
        const AlgorithmClass type = algorithm_class(id);
        CHECK((aead_key_bytes(id) != 0U) == (type == AlgorithmClass::Aead));
        CHECK((hkdf_hash_bytes(id) != 0U) == (type == AlgorithmClass::Kdf));
        const bool signature =
            type == AlgorithmClass::PostQuantumSignature || type == AlgorithmClass::ClassicalSignature;
        CHECK((signature_sizes(id).public_key != 0U) == signature);
        CHECK((agreement_sizes(id).public_key != 0U) == (type == AlgorithmClass::ClassicalKeyExchange));
        CHECK((kem_sizes(id).public_key != 0U) == (type == AlgorithmClass::Kem));
        CHECK(provider_implements(id));
    }

    const CryptoError errors[] = {CryptoError::None,       CryptoError::Unsupported, CryptoError::NotReady,
                                  CryptoError::BadLength,  CryptoError::Overlap,     CryptoError::BadKey,
                                  CryptoError::Forged,     CryptoError::WeakSecret,  CryptoError::Exhausted,
                                  CryptoError::ProviderFailure, CryptoError::Expired, CryptoError::Replayed};
    const char* const names[]  = {"none",    "unsupported", "not-ready",   "bad-length", "overlap",
                                  "bad-key", "forged",      "weak-secret", "exhausted",  "provider-failure",
                                  "expired", "replayed"};
    for (usize i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i)
    {
        CHECK(crypto_error_name(errors[i]) == StringView(names[i]));
    }
    CHECK(crypto_error_name(static_cast<CryptoError>(200)) == StringView("unknown"));
}

TEST_CASE("Provider: randomness, constant-time equality, zeroing and key identifiers", "[crypto][net-2]")
{
    u8 a[32] = {};
    u8 b[32] = {};
    REQUIRE(random_bytes(a) == CryptoError::None);
    REQUIRE(random_bytes(b) == CryptoError::None);
    CHECK_FALSE(all_zero(a));
    CHECK_FALSE(equal(a, b)); // 2^-256
    CHECK(random_bytes(Span<u8>()) == CryptoError::None);

    CHECK(constant_time_equal(a, a));
    CHECK_FALSE(constant_time_equal(a, b));
    CHECK_FALSE(constant_time_equal(ConstSpan<u8>(a, 31U), ConstSpan<u8>(a, 32U)));
    CHECK(constant_time_equal(ConstSpan<u8>(), ConstSpan<u8>()));

    secure_zero(a);
    CHECK(all_zero(a));
    secure_zero(Span<u8>());

    // SHA-256("abc"), FIPS 180-4 appendix B.1.
    const u8    abc[] = {'a', 'b', 'c'};
    const Bytes want  = from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const KeyId id    = key_id_of(abc);
    CHECK(equal(id.bytes, ConstSpan<u8>(want.data(), want.size())));
}

// ---- AEAD ----

void run_aead_vectors(const char* file, AlgorithmId (*pick)(u64 key_bits), u32 want_in_profile, u32 want_outside)
{
    const Vectors v(file);
    u32           in_profile = 0U;
    u32           outside    = 0U;
    v.each(
        [&](u32 group, u32 test, StringView result)
        {
            const Bytes       key   = v.hex(test, "key");
            const Bytes       iv    = v.hex(test, "iv");
            const Bytes       aad   = v.hex(test, "aad");
            const Bytes       msg   = v.hex(test, "msg");
            const Bytes       ct    = v.hex(test, "ct");
            const Bytes       tag   = v.hex(test, "tag");
            const AlgorithmId alg   = pick(v.number(group, "keySize"));
            Bytes             sealed;
            sealed.resize(ct.size() + tag.size());
            for (usize i = 0U; i < ct.size(); ++i)
            {
                sealed[i] = ct[i];
            }
            for (usize i = 0U; i < tag.size(); ++i)
            {
                sealed[ct.size() + i] = tag[i];
            }
            AeadKey aead;
            const CryptoError init = aead.init(alg, key);
            const bool profile = v.number(group, "ivSize") == 96U && v.number(group, "tagSize") == 128U &&
                                 key.size() == aead_key_bytes(alg);
            Bytes plain;
            plain.resize(sealed.size() >= kAeadTagBytes ? sealed.size() - kAeadTagBytes : 0U);
            if (!profile)
            {
                // Outside Cerid's profile (12-byte nonces, 16-byte tags, 128- and 256-bit AES keys): a key of another
                // length is refused, another nonce length is refused by seal and open, and a vector with a shorter
                // tag never opens.
                ++outside;
                if (init != CryptoError::None)
                {
                    CHECK(init == CryptoError::BadLength);
                    return;
                }
                if (iv.size() != kAeadNonceBytes)
                {
                    Bytes out;
                    out.resize(msg.size() + kAeadTagBytes);
                    CHECK(aead.seal(iv, msg, aad, out) == CryptoError::BadLength);
                }
                if (sealed.size() >= kAeadTagBytes)
                {
                    CHECK(aead.open(iv, sealed, aad, plain) != CryptoError::None);
                }
                return;
            }
            ++in_profile;
            REQUIRE(init == CryptoError::None);
            const CryptoError opened = aead.open(iv, sealed, aad, plain);
            if (result == StringView("valid"))
            {
                REQUIRE(opened == CryptoError::None);
                CHECK(equal(plain, msg));
                Bytes out;
                out.resize(msg.size() + kAeadTagBytes);
                REQUIRE(aead.seal(iv, msg, aad, out) == CryptoError::None);
                CHECK(equal(out, sealed));
            }
            else
            {
                CHECK(opened == CryptoError::Forged);
                CHECK(all_zero(plain));
            }
        });
    CHECK(in_profile == want_in_profile);
    CHECK(outside == want_outside);
}

TEST_CASE("Provider: AES-GCM against Wycheproof", "[crypto][net-2][wycheproof]")
{
    run_aead_vectors(
        "aes_gcm_test", [](u64 bits) { return bits == 256U ? AlgorithmId::Aes256Gcm : AlgorithmId::Aes128Gcm; },
        133U, 183U);
}

TEST_CASE("Provider: ChaCha20-Poly1305 against Wycheproof", "[crypto][net-2][wycheproof]")
{
    run_aead_vectors("chacha20_poly1305_test", [](u64) { return AlgorithmId::Chacha20Poly1305; }, 316U, 9U);
}

TEST_CASE("Provider: AEAD lengths, aliasing, refusals and forgeries", "[crypto][net-2]")
{
    u8 key[32];
    for (usize i = 0U; i < sizeof(key); ++i)
    {
        key[i] = static_cast<u8>(i);
    }
    const u8 nonce[kAeadNonceBytes] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const u8 aad[]                  = {'h', 'd', 'r'};
    u8       text[40];
    for (usize i = 0U; i < sizeof(text); ++i)
    {
        text[i] = static_cast<u8>(0xA0U + i);
    }

    AeadKey aead;
    u8      out[sizeof(text) + kAeadTagBytes] = {};
    u8      back[sizeof(text)]                = {};
    CHECK(aead.seal(nonce, text, aad, out) == CryptoError::NotReady);
    CHECK(aead.open(nonce, out, aad, back) == CryptoError::NotReady);
    CHECK(aead.init(AlgorithmId::HkdfSha256, key) == CryptoError::Unsupported);
    CHECK(aead.init(AlgorithmId::Ed25519, key) == CryptoError::Unsupported);
    CHECK(aead.init(AlgorithmId::Aes128Gcm, key) == CryptoError::BadLength); // 32 bytes for a 16-byte key
    CHECK_FALSE(aead.ready());

    for (const AlgorithmId alg : {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305})
    {
        INFO(static_cast<int>(alg));
        REQUIRE(aead.init(alg, ConstSpan<u8>(key, aead_key_bytes(alg))) == CryptoError::None);
        CHECK(aead.ready());
        CHECK(aead.algorithm() == alg);

        // Lengths are exact.
        CHECK(aead.seal(ConstSpan<u8>(nonce, 11U), text, aad, out) == CryptoError::BadLength);
        CHECK(aead.seal(nonce, text, aad, Span<u8>(out, sizeof(out) - 1U)) == CryptoError::BadLength);
        CHECK(aead.open(nonce, ConstSpan<u8>(out, kAeadTagBytes - 1U), aad, Span<u8>()) == CryptoError::BadLength);
        CHECK(aead.open(nonce, out, aad, Span<u8>(back, sizeof(back) - 1U)) == CryptoError::BadLength);

        // Apart, then in place: the same bytes either way.
        REQUIRE(aead.seal(nonce, text, aad, out) == CryptoError::None);
        u8 inplace[sizeof(out)];
        std::memcpy(inplace, text, sizeof(text));
        REQUIRE(aead.seal(nonce, ConstSpan<u8>(inplace, sizeof(text)), aad, inplace) == CryptoError::None);
        CHECK(equal(inplace, out));
        REQUIRE(aead.open(nonce, inplace, aad, Span<u8>(inplace, sizeof(text))) == CryptoError::None);
        CHECK(equal(ConstSpan<u8>(inplace, sizeof(text)), text));
        REQUIRE(aead.open(nonce, out, aad, back) == CryptoError::None);
        CHECK(equal(back, text));

        // A partial overlap of the text, or any overlap of the nonce or associated data, is refused.
        u8 shifted[sizeof(out) + 1U] = {};
        CHECK(aead.seal(nonce, ConstSpan<u8>(shifted, sizeof(text)), aad, Span<u8>(shifted + 1U, sizeof(out))) ==
              CryptoError::Overlap);
        CHECK(aead.seal(ConstSpan<u8>(out, kAeadNonceBytes), text, aad, out) == CryptoError::Overlap);
        CHECK(aead.open(nonce, out, ConstSpan<u8>(back + 2U, 3U), back) == CryptoError::Overlap);

        // Empty text and empty associated data.
        u8 tag_only[kAeadTagBytes];
        REQUIRE(aead.seal(nonce, ConstSpan<u8>(), ConstSpan<u8>(), tag_only) == CryptoError::None);
        CHECK(aead.open(nonce, tag_only, ConstSpan<u8>(), Span<u8>()) == CryptoError::None);

        // Every tampered input is a forgery, and nothing unauthenticated is released.
        for (const usize flip : {usize{0}, sizeof(text) - 1U, sizeof(text), sizeof(out) - 1U})
        {
            u8 bad[sizeof(out)];
            std::memcpy(bad, out, sizeof(out));
            bad[flip] ^= 0x80U;
            std::memset(back, 0x55, sizeof(back));
            CHECK(aead.open(nonce, bad, aad, back) == CryptoError::Forged);
            CHECK(all_zero(back));
        }
        u8 other_nonce[kAeadNonceBytes];
        std::memcpy(other_nonce, nonce, sizeof(nonce));
        other_nonce[11] ^= 1U;
        CHECK(aead.open(other_nonce, out, aad, back) == CryptoError::Forged);
        CHECK(aead.open(nonce, out, ConstSpan<u8>(aad, 2U), back) == CryptoError::Forged);
    }

    // Concurrent seals under one key give one answer (the key is read-only after init).
    REQUIRE(aead.init(AlgorithmId::Aes256Gcm, key) == CryptoError::None);
    u8 expected[sizeof(out)];
    REQUIRE(aead.seal(nonce, text, aad, expected) == CryptoError::None);
    std::atomic<u32> mismatches{0U};
    std::thread      threads[4];
    for (std::thread& thread : threads)
    {
        thread = std::thread(
            [&]
            {
                u8 mine[sizeof(out)];
                for (u32 i = 0U; i < 200U; ++i)
                {
                    if (aead.seal(nonce, text, aad, mine) != CryptoError::None || !equal(mine, expected))
                    {
                        mismatches.fetch_add(1U);
                    }
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(mismatches.load() == 0U);

    aead.clear();
    CHECK_FALSE(aead.ready());
    CHECK(aead.seal(nonce, text, aad, out) == CryptoError::NotReady);
}

// ---- HKDF ----

TEST_CASE("Provider: HKDF against Wycheproof", "[crypto][net-2][wycheproof]")
{
    struct Suite
    {
        const char* file;
        AlgorithmId alg;
        u32         valid;
    };
    for (const Suite& suite : {Suite{"hkdf_sha256_test", AlgorithmId::HkdfSha256, 83U},
                               Suite{"hkdf_sha384_test", AlgorithmId::HkdfSha384, 80U}})
    {
        const AlgorithmId alg = suite.alg;
        const Vectors     v(suite.file);
        u32           valid = 0U;
        v.each(
            [&](u32, u32 test, StringView result)
            {
                const Bytes ikm  = v.hex(test, "ikm");
                const Bytes salt = v.hex(test, "salt");
                const Bytes info = v.hex(test, "info");
                const usize size = static_cast<usize>(v.number(test, "size"));
                u8          prk[48];
                const auto  prk_span = Span<u8>(prk, hkdf_hash_bytes(alg));
                REQUIRE(hkdf_extract(alg, salt, ikm, prk_span) == CryptoError::None);
                Bytes okm;
                okm.resize(size);
                const CryptoError expanded = hkdf_expand(alg, prk_span, info, okm);
                if (result == StringView("valid"))
                {
                    ++valid;
                    REQUIRE(expanded == CryptoError::None);
                    CHECK(equal(okm, v.hex(test, "okm")));
                }
                else
                {
                    CHECK(expanded == CryptoError::BadLength);
                }
            });
        CHECK(valid == suite.valid);
    }
}

TEST_CASE("Provider: HKDF refusals", "[crypto][net-2]")
{
    u8 prk[32] = {};
    u8 out[64] = {};
    CHECK(hkdf_hash_bytes(AlgorithmId::HkdfSha256) == 32U);
    CHECK(hkdf_hash_bytes(AlgorithmId::HkdfSha384) == 48U);
    CHECK(hkdf_extract(AlgorithmId::Aes128Gcm, ConstSpan<u8>(), prk, prk) == CryptoError::Unsupported);
    CHECK(hkdf_expand(AlgorithmId::X25519, prk, ConstSpan<u8>(), out) == CryptoError::Unsupported);
    CHECK(hkdf_extract(AlgorithmId::HkdfSha384, ConstSpan<u8>(), prk, prk) == CryptoError::BadLength);
    CHECK(hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk, 31U), ConstSpan<u8>(), out) ==
          CryptoError::BadLength);
    CHECK(hkdf_expand(AlgorithmId::HkdfSha256, prk, ConstSpan<u8>(), Span<u8>()) == CryptoError::BadLength);
    Bytes most;
    most.resize(255U * 32U);
    CHECK(hkdf_expand(AlgorithmId::HkdfSha256, prk, ConstSpan<u8>(), most) == CryptoError::None);
    most.resize(255U * 32U + 1U);
    CHECK(hkdf_expand(AlgorithmId::HkdfSha256, prk, ConstSpan<u8>(), most) == CryptoError::BadLength);
}

// ---- Key agreement ----

TEST_CASE("Provider: X25519 against Wycheproof", "[crypto][net-2][wycheproof]")
{
    const Vectors v("x25519_test");
    u32           weak = 0U;
    v.each(
        [&](u32, u32 test, StringView result)
        {
            AgreementKey key;
            REQUIRE(key.from_private(AlgorithmId::X25519, v.hex(test, "private")) == CryptoError::None);
            u8 secret[32];
            std::memset(secret, 0xAA, sizeof(secret));
            const CryptoError derived = key.derive(v.hex(test, "public"), secret);
            if (v.flagged(test, "ZeroSharedSecret"))
            {
                // Acceptable to Wycheproof, refused by Cerid: a contributory secret is required (RFC 8446 7.4.2).
                ++weak;
                CHECK(derived == CryptoError::WeakSecret);
                CHECK(all_zero(secret));
                return;
            }
            REQUIRE(result != StringView("invalid"));
            REQUIRE(derived == CryptoError::None);
            const Bytes shared = v.hex(test, "shared");
            CHECK(equal(secret, shared));
        });
    CHECK(weak == 31U);
}

// An EC private key from Wycheproof's integer form: big-endian, possibly with a leading zero byte or short.
Bytes scalar(const Bytes& integer, usize length)
{
    Bytes out;
    out.resize(length);
    usize skip = 0U;
    while (integer.size() - skip > length)
    {
        REQUIRE(integer[skip] == 0U);
        ++skip;
    }
    const usize pad = length - (integer.size() - skip);
    for (usize i = 0U; i < length; ++i)
    {
        out[i] = i < pad ? u8{0} : integer[skip + i - pad];
    }
    return out;
}

TEST_CASE("Provider: ECDH over P-256 and P-384 against Wycheproof", "[crypto][net-2][wycheproof]")
{
    struct Suite
    {
        const char* file;
        AlgorithmId alg;
        u32         refused;
    };
    for (const Suite& suite : {Suite{"ecdh_secp256r1_ecpoint_test", AlgorithmId::Secp256r1, 25U},
                               Suite{"ecdh_secp384r1_ecpoint_test", AlgorithmId::Secp384r1, 19U}})
    {
        const AlgorithmId    alg = suite.alg;
        const Vectors        v(suite.file);
        const AgreementSizes sizes   = agreement_sizes(alg);
        u32                  invalid = 0U;
        v.each(
            [&](u32, u32 test, StringView result)
            {
                AgreementKey key;
                const Bytes own = scalar(v.hex(test, "private"), sizes.private_key);
                REQUIRE(key.from_private(alg, own) == CryptoError::None);
                u8 secret[48];
                std::memset(secret, 0xAA, sizeof(secret));
                const auto        out     = Span<u8>(secret, sizes.secret);
                const CryptoError derived = key.derive(v.hex(test, "public"), out);
                if (result == StringView("valid"))
                {
                    REQUIRE(derived == CryptoError::None);
                    CHECK(equal(out, v.hex(test, "shared")));
                    return;
                }
                // Invalid (off-curve and wrong-curve points, bad encodings) and acceptable (compressed points, which
                // TLS 1.3 forbids) are refused, and the secret is zeroed.
                ++invalid;
                CHECK((derived == CryptoError::BadKey || derived == CryptoError::BadLength));
                CHECK(all_zero(out));
            });
        CHECK(invalid == suite.refused);
    }
}

TEST_CASE("Provider: agreement keys generate, agree, move and refuse", "[crypto][net-2]")
{
    for (const AlgorithmId alg : {AlgorithmId::X25519, AlgorithmId::Secp256r1, AlgorithmId::Secp384r1})
    {
        INFO(static_cast<int>(alg));
        const AgreementSizes sizes = agreement_sizes(alg);
        AgreementKey         alice;
        AgreementKey         bob;
        u8                   pub_a[97];
        u8                   pub_b[97];
        u8                   s_a[48];
        u8                   s_b[48];
        const auto           pa = Span<u8>(pub_a, sizes.public_key);
        const auto           pb = Span<u8>(pub_b, sizes.public_key);
        const auto           sa = Span<u8>(s_a, sizes.secret);
        const auto           sb = Span<u8>(s_b, sizes.secret);
        CHECK(alice.public_key(pa) == CryptoError::NotReady);
        CHECK(alice.derive(pb, sa) == CryptoError::NotReady);
        REQUIRE(alice.generate(alg) == CryptoError::None);
        REQUIRE(bob.generate(alg) == CryptoError::None);
        REQUIRE(alice.public_key(pa) == CryptoError::None);
        REQUIRE(bob.public_key(pb) == CryptoError::None);
        CHECK_FALSE(equal(pa, pb));
        REQUIRE(alice.derive(pb, sa) == CryptoError::None);
        REQUIRE(bob.derive(pa, sb) == CryptoError::None);
        CHECK(equal(sa, sb));
        CHECK_FALSE(all_zero(sa));

        CHECK(alice.public_key(Span<u8>(pub_a, sizes.public_key - 1U)) == CryptoError::BadLength);
        CHECK(alice.derive(ConstSpan<u8>(pub_b, sizes.public_key - 1U), sa) == CryptoError::BadLength);
        CHECK(alice.derive(pb, Span<u8>(s_a, sizes.secret - 1U)) == CryptoError::BadLength);

        // Moving keeps the key; the source is empty.
        AgreementKey moved(static_cast<AgreementKey&&>(alice));
        CHECK_FALSE(alice.ready());
        CHECK(moved.ready());
        CHECK(moved.algorithm() == alg);
        REQUIRE(moved.derive(pb, sa) == CryptoError::None);
        CHECK(equal(sa, sb));
        alice = static_cast<AgreementKey&&>(moved);
        CHECK(alice.ready());
        CHECK_FALSE(moved.ready());
        alice.clear();
        CHECK_FALSE(alice.ready());
        CHECK(alice.algorithm() == AlgorithmId{});
    }

    // An X25519 public key is the agreement with the base point u = 9.
    AgreementKey x;
    REQUIRE(x.generate(AlgorithmId::X25519) == CryptoError::None);
    u8 pub[32];
    u8 base[32] = {9};
    u8 derived[32];
    REQUIRE(x.public_key(pub) == CryptoError::None);
    REQUIRE(x.derive(base, derived) == CryptoError::None);
    CHECK(equal(pub, derived));
    // A secret overlapping the peer's key is refused before anything is written.
    u8 joint[40] = {9};
    CHECK(x.derive(ConstSpan<u8>(joint, 32U), Span<u8>(joint + 8U, 32U)) == CryptoError::Overlap);
    CHECK(joint[0] == 9U);
    CHECK(all_zero(ConstSpan<u8>(joint + 1U, 39U)));

    // EC scalars outside [1, n - 1], compressed and off-curve points are refused.
    AgreementKey ec;
    u8           zero[32] = {};
    u8           ones[32];
    std::memset(ones, 0xFF, sizeof(ones));
    CHECK(ec.from_private(AlgorithmId::Secp256r1, zero) == CryptoError::BadKey);
    CHECK(ec.from_private(AlgorithmId::Secp256r1, ones) == CryptoError::BadKey);
    CHECK(ec.from_private(AlgorithmId::Secp256r1, ConstSpan<u8>(ones, 31U)) == CryptoError::BadLength);
    CHECK(ec.from_private(AlgorithmId::Ed25519, zero) == CryptoError::Unsupported);
    CHECK(ec.generate(AlgorithmId::Aes128Gcm) == CryptoError::Unsupported);
    REQUIRE(ec.generate(AlgorithmId::Secp256r1) == CryptoError::None);
    u8 point[65];
    u8 secret[32];
    std::memset(secret, 0xAA, sizeof(secret));
    REQUIRE(ec.public_key(point) == CryptoError::None);
    CHECK(point[0] == 0x04U);
    for (const u8 form : {u8{0x02}, u8{0x03}, u8{0x06}, u8{0x07}, u8{0x00}}) // compressed, hybrid, infinity
    {
        point[0] = form;
        CHECK(ec.derive(point, secret) == CryptoError::BadKey);
    }
    point[0] = 0x04U;
    point[64] ^= 1U;
    CHECK(ec.derive(point, secret) == CryptoError::BadKey);
    CHECK(all_zero(secret));
}

// ---- Signatures ----

TEST_CASE("Provider: Ed25519 against Wycheproof and RFC 8032", "[crypto][net-2][wycheproof]")
{
    const ProviderVerifier verifier;
    const Vectors          v("ed25519_test");
    u32                    checked = 0U;
    v.each(
        [&](u32 group, u32 test, StringView result)
        {
            const Bytes        pk    = from_hex(v.text(json::find(v.doc(), group, "publicKey"), "pk"));
            const Verification found =
                verifier.verify(AlgorithmId::Ed25519, pk, v.hex(test, "msg"), v.hex(test, "sig"));
            if (result == StringView("acceptable"))
            {
                // Small- and mixed-order public keys: either answer is allowed, but never "unsupported".
                CHECK(found != Verification::Unsupported);
                return;
            }
            ++checked;
            CHECK(found == (result == StringView("valid") ? Verification::Valid : Verification::Invalid));
        });
    CHECK(checked == 181U);

    // RFC 8032 section 7.1, TEST 1 and TEST 2: signing is deterministic.
    struct Known
    {
        const char* secret;
        const char* public_key;
        const char* message;
        const char* signature;
    };
    const Known known[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
         "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    };
    for (const Known& k : known)
    {
        SigningKey key;
        REQUIRE(key.from_seed(AlgorithmId::Ed25519, from_hex(k.secret)) == CryptoError::None);
        u8 pub[32];
        REQUIRE(key.public_key(pub) == CryptoError::None);
        CHECK(equal(pub, from_hex(k.public_key)));
        u8    sig[64];
        usize written = 0U;
        REQUIRE(key.sign(from_hex(k.message), sig, written) == CryptoError::None);
        CHECK(equal(ConstSpan<u8>(sig, written), from_hex(k.signature)));
        CHECK(verifier.verify(AlgorithmId::Ed25519, pub, from_hex(k.message), ConstSpan<u8>(sig, written)) ==
              Verification::Valid);
    }
}

TEST_CASE("Provider: ECDSA over P-256 and P-384 against Wycheproof", "[crypto][net-2][wycheproof]")
{
    const ProviderVerifier verifier;
    struct Suite
    {
        const char* file;
        AlgorithmId alg;
        u32         valid;
    };
    for (const Suite& suite : {Suite{"ecdsa_secp256r1_sha256_test", AlgorithmId::EcdsaSecp256r1Sha256, 174U},
                               Suite{"ecdsa_secp384r1_sha384_test", AlgorithmId::EcdsaSecp384r1Sha384, 194U}})
    {
        const AlgorithmId alg = suite.alg;
        const Vectors     v(suite.file);
        u32           valid   = 0U;
        u32           invalid = 0U;
        v.each(
            [&](u32 group, u32 test, StringView result)
            {
                const Bytes pk = from_hex(v.text(json::find(v.doc(), group, "publicKey"), "uncompressed"));
                const Verification found = verifier.verify(alg, pk, v.hex(test, "msg"), v.hex(test, "sig"));
                if (result == StringView("valid"))
                {
                    ++valid;
                    CHECK(found == Verification::Valid);
                }
                else
                {
                    // BER encodings, malleated and out-of-range signatures: strict DER only.
                    ++invalid;
                    CHECK(found == Verification::Invalid);
                }
            });
        CHECK(valid == suite.valid);
        CHECK(invalid == 310U);
    }
}

TEST_CASE("Provider: ML-DSA against Wycheproof", "[crypto][net-2][wycheproof]")
{
    const ProviderVerifier verifier;
    struct Set
    {
        AlgorithmId alg;
        const char* verify_file;
        const char* sign_file;
        u32         judged;   // verify vectors without a context string
        u32         keys;     // sign vectors whose seed is valid
        u32         messages; // of those, over a message without a context string
    };
    for (const Set& set :
         {Set{AlgorithmId::MlDsa44, "mldsa_44_verify_test", "mldsa_44_sign_seed_test", 172U, 83U, 70U},
          Set{AlgorithmId::MlDsa65, "mldsa_65_verify_test", "mldsa_65_sign_seed_test", 202U, 102U, 80U},
          Set{AlgorithmId::MlDsa87, "mldsa_87_verify_test", "mldsa_87_sign_seed_test", 233U, 93U, 71U}})
    {
        INFO(set.verify_file);
        const SignatureSizes sizes = signature_sizes(set.alg);
        u32                  judged = 0U;
        const Vectors        verify(set.verify_file);
        verify.each(
            [&](u32 group, u32 test, StringView result)
            {
                // Cerid signs purely with an empty context (decision P4); a context string is outside its interface.
                if (verify.has(test, "ctx"))
                {
                    return;
                }
                ++judged;
                const Verification found = verifier.verify(set.alg, verify.hex(group, "publicKey"),
                                                           verify.hex(test, "msg"), verify.hex(test, "sig"));
                CHECK(found == (result == StringView("valid") ? Verification::Valid : Verification::Invalid));
            });
        CHECK(judged == set.judged);

        // Key generation from the seed is FIPS 204's: the derived public key is the vector's, and every signature
        // Cerid makes and every vector signature verify.
        u32           keys     = 0U;
        u32           messages = 0U;
        const Vectors sign(set.sign_file);
        sign.each(
            [&](u32 group, u32 test, StringView result)
            {
                SigningKey        key;
                const Bytes       seed   = sign.hex(group, "privateSeed");
                const CryptoError loaded = key.from_seed(set.alg, seed);
                if (!sign.has(group, "publicKey"))
                {
                    CHECK(result == StringView("invalid"));
                    CHECK(loaded == CryptoError::BadLength);
                    return;
                }
                REQUIRE(loaded == CryptoError::None);
                const Bytes want = sign.hex(group, "publicKey");
                Bytes       pub;
                pub.resize(sizes.public_key);
                REQUIRE(key.public_key(pub) == CryptoError::None);
                CHECK(equal(pub, want));
                ++keys;
                if (sign.has(test, "ctx") || !sign.has(test, "msg"))
                {
                    return; // a context string, or the internal interface over a precomputed mu
                }
                REQUIRE(result == StringView("valid"));
                ++messages;
                const Bytes msg = sign.hex(test, "msg");
                CHECK(verifier.verify(set.alg, pub, msg, sign.hex(test, "sig")) == Verification::Valid);
                Bytes sig;
                sig.resize(sizes.signature);
                usize written = 0U;
                REQUIRE(key.sign(msg, sig, written) == CryptoError::None);
                CHECK(written == sizes.signature);
                CHECK(verifier.verify(set.alg, pub, msg, ConstSpan<u8>(sig.data(), written)) == Verification::Valid);
            });
        CHECK(keys == set.keys);
        CHECK(messages == set.messages);
    }
}

TEST_CASE("Provider: signing keys generate, sign, verify, move and refuse", "[crypto][net-2]")
{
    const ProviderVerifier verifier;
    const u8               message[] = {'p', 'o', 'l', 'i', 'c', 'y'};
    for (const AlgorithmId alg : {AlgorithmId::MlDsa44, AlgorithmId::MlDsa65, AlgorithmId::MlDsa87,
                                  AlgorithmId::Ed25519, AlgorithmId::EcdsaSecp256r1Sha256,
                                  AlgorithmId::EcdsaSecp384r1Sha384})
    {
        INFO(static_cast<int>(alg));
        const SignatureSizes sizes = signature_sizes(alg);
        SigningKey           key;
        Bytes                pub;
        Bytes                sig;
        pub.resize(sizes.public_key);
        sig.resize(sizes.signature);
        usize written = 0U;
        CHECK(key.public_key(pub) == CryptoError::NotReady);
        CHECK(key.sign(message, sig, written) == CryptoError::NotReady);
        REQUIRE(key.generate(alg) == CryptoError::None);
        CHECK(key.algorithm() == alg);
        REQUIRE(key.public_key(pub) == CryptoError::None);
        REQUIRE(key.sign(message, sig, written) == CryptoError::None);
        const ConstSpan<u8> signature(sig.data(), written);
        CHECK(verifier.verify(alg, pub, message, signature) == Verification::Valid);

        // Another message, a flipped signature byte, a flipped key byte, another algorithm's label.
        CHECK(verifier.verify(alg, pub, ConstSpan<u8>(message, 5U), signature) == Verification::Invalid);
        Bytes bad = sig;
        bad[written / 2U] ^= 0x01U;
        CHECK(verifier.verify(alg, pub, message, ConstSpan<u8>(bad.data(), written)) == Verification::Invalid);
        Bytes other_key = pub;
        other_key[other_key.size() - 1U] ^= 0x01U;
        CHECK(verifier.verify(alg, other_key, message, signature) == Verification::Invalid);
        CHECK(verifier.verify(alg, ConstSpan<u8>(pub.data(), pub.size() - 1U), message, signature) ==
              Verification::Invalid);
        CHECK(verifier.verify(alg, pub, message, ConstSpan<u8>()) == Verification::Invalid);
        CHECK(verifier.verify(AlgorithmId::Aes256Gcm, pub, message, signature) == Verification::Unsupported);
        CHECK(verifier.verify(AlgorithmId::X25519Mlkem768, pub, message, signature) == Verification::Unsupported);

        // Lengths.
        CHECK(key.public_key(Span<u8>(pub.data(), pub.size() - 1U)) == CryptoError::BadLength);
        CHECK(key.sign(message, Span<u8>(sig.data(), sizes.signature - 1U), written) == CryptoError::BadLength);
        CHECK(written == 0U);
        Bytes seed;
        seed.resize(sizes.seed + 1U);
        CHECK(key.from_seed(alg, seed) == CryptoError::BadLength);
        CHECK_FALSE(key.ready());

        // Moving keeps the key.
        REQUIRE(key.generate(alg) == CryptoError::None);
        SigningKey moved(static_cast<SigningKey&&>(key));
        CHECK_FALSE(key.ready());
        REQUIRE(moved.public_key(pub) == CryptoError::None);
        REQUIRE(moved.sign(message, sig, written) == CryptoError::None);
        CHECK(verifier.verify(alg, pub, message, ConstSpan<u8>(sig.data(), written)) == Verification::Valid);
        key = static_cast<SigningKey&&>(moved);
        CHECK(key.ready());
        CHECK_FALSE(moved.ready());
    }

    SigningKey key;
    CHECK(key.generate(AlgorithmId::X25519) == CryptoError::Unsupported);
    CHECK(key.from_seed(AlgorithmId::HkdfSha256, ConstSpan<u8>()) == CryptoError::Unsupported);
    u8 zero[48] = {};
    u8 ones[48];
    std::memset(ones, 0xFF, sizeof(ones));
    CHECK(key.from_seed(AlgorithmId::EcdsaSecp384r1Sha384, zero) == CryptoError::BadKey);
    CHECK(key.from_seed(AlgorithmId::EcdsaSecp384r1Sha384, ones) == CryptoError::BadKey);
    // Seeds determine keys: one seed, one public key.
    SigningKey again;
    REQUIRE(key.from_seed(AlgorithmId::MlDsa65, ConstSpan<u8>(ones, 32U)) == CryptoError::None);
    REQUIRE(again.from_seed(AlgorithmId::MlDsa65, ConstSpan<u8>(ones, 32U)) == CryptoError::None);
    Bytes a;
    Bytes b;
    a.resize(signature_sizes(AlgorithmId::MlDsa65).public_key);
    b.resize(a.size());
    REQUIRE(key.public_key(a) == CryptoError::None);
    REQUIRE(again.public_key(b) == CryptoError::None);
    CHECK(equal(a, b));
}

TEST_CASE("Provider: one verifier and one key serve several threads", "[crypto][net-2]")
{
    const ProviderVerifier verifier;
    SigningKey             key;
    REQUIRE(key.generate(AlgorithmId::MlDsa65) == CryptoError::None);
    Bytes pub;
    pub.resize(signature_sizes(AlgorithmId::MlDsa65).public_key);
    REQUIRE(key.public_key(pub) == CryptoError::None);
    const u8         message[] = {'t', 'h', 'r', 'e', 'a', 'd', 's'};
    std::atomic<u32> failures{0U};
    std::thread      threads[4];
    for (std::thread& thread : threads)
    {
        thread = std::thread(
            [&]
            {
                Bytes sig;
                sig.resize(signature_sizes(AlgorithmId::MlDsa65).signature);
                for (u32 i = 0U; i < 8U; ++i)
                {
                    usize written = 0U;
                    if (key.sign(message, sig, written) != CryptoError::None ||
                        verifier.verify(AlgorithmId::MlDsa65, pub, message, ConstSpan<u8>(sig.data(), written)) !=
                            Verification::Valid)
                    {
                        failures.fetch_add(1U);
                    }
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(failures.load() == 0U);
}

// ---- The policy generation through the production verifier ----

TEST_CASE("Provider: a policy generation signed by real ML-DSA-65 and Ed25519 keys", "[crypto][net-2][net-std]")
{
    // The installation's hybrid trust set, from fixed seeds so the keys are reproducible.
    u8 pq_seed[32];
    u8 ed_seed[32];
    for (usize i = 0U; i < 32U; ++i)
    {
        pq_seed[i] = static_cast<u8>(0x11U * (i + 1U));
        ed_seed[i] = static_cast<u8>(0x80U ^ i);
    }
    SigningKey pq;
    SigningKey ed;
    REQUIRE(pq.from_seed(AlgorithmId::MlDsa65, pq_seed) == CryptoError::None);
    REQUIRE(ed.from_seed(AlgorithmId::Ed25519, ed_seed) == CryptoError::None);
    Bytes pq_public;
    pq_public.resize(signature_sizes(AlgorithmId::MlDsa65).public_key);
    u8 ed_public[32];
    REQUIRE(pq.public_key(pq_public) == CryptoError::None);
    REQUIRE(ed.public_key(ed_public) == CryptoError::None);
    const TrustKey trust[] = {
        {key_id_of(pq_public), AlgorithmId::MlDsa65, ConstSpan<u8>(pq_public.data(), pq_public.size())},
        {key_id_of(ed_public), AlgorithmId::Ed25519, ConstSpan<u8>(ed_public)},
    };
    const ProviderVerifier verifier;
    const PolicyConfig     config{trust, &verifier, provider_algorithms(), 0U};
    REQUIRE(validate_config(config) == PolicyError::None);

    constexpr u64 now = 1800000000U;
    PolicyContent content;
    content.generation    = 1U;
    content.issued        = now;
    content.expires       = now + 30U * 86400U;
    content.offline_grace = 86400U;
    content.rules[0]      = {AlgorithmId::Chacha20Poly1305, 0U, 0U, kNever, kNever};
    content.rules[1]      = {AlgorithmId::X25519Mlkem768, 0U, 0U, kNever, kNever};
    // A hybrid group a later register could add: above this build's ceiling, so the rule takes no effect.
    content.rules[2]   = {static_cast<AlgorithmId>(0x01FFU), 0U, 0U, kNever, kNever};
    content.rule_count = 3U;
    u8    body[1024];
    usize body_size = 0U;
    REQUIRE(write_policy_body(content, body, body_size) == PolicyError::None);
    const ConstSpan<u8> message(body, body_size);

    Bytes pq_sig;
    pq_sig.resize(signature_sizes(AlgorithmId::MlDsa65).signature);
    u8    ed_sig[64];
    usize pq_written = 0U;
    usize ed_written = 0U;
    REQUIRE(pq.sign(message, pq_sig, pq_written) == CryptoError::None);
    REQUIRE(ed.sign(message, ed_sig, ed_written) == CryptoError::None);
    const KeySignature signatures[] = {
        {trust[0].id, AlgorithmId::MlDsa65, ConstSpan<u8>(pq_sig.data(), pq_written)},
        {trust[1].id, AlgorithmId::Ed25519, ConstSpan<u8>(ed_sig, ed_written)},
    };
    u8    generation[8192];
    usize size = 0U;
    REQUIRE(write_policy(message, signatures, generation, size) == PolicyError::None);

    // One flipped byte in the post-quantum signature: refused, the store unchanged.
    PolicyStore store(config);
    u8          forged[8192];
    std::memcpy(forged, generation, size);
    forged[body_size + 1U + kKeyIdBytes + 4U + 100U] ^= 0x01U;
    CHECK(store.apply(ConstSpan<u8>(forged, size), now) == PolicyError::BadSignature);
    CHECK(store.snapshot().generation() == 0U);

    REQUIRE(store.apply(ConstSpan<u8>(generation, size), now) == PolicyError::None);
    const PolicySnapshot snapshot = store.snapshot();
    CHECK(snapshot.generation() == 1U);
    CHECK(snapshot.status(AlgorithmId::Chacha20Poly1305, now) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::X25519Mlkem768, now) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(static_cast<AlgorithmId>(0x01FFU), now) == AlgorithmStatus::Refused);
    CHECK(store.apply(ConstSpan<u8>(generation, size), now) == PolicyError::AlreadyCurrent);
}
} // namespace
