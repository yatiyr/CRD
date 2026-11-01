// crd-crypto -- the provider's key encapsulation (ML-KEM-768 and ML-KEM-1024, FIPS 203) and keyed PRF (SipHash-2-4)
// (NET.2; docs/design/network-foundation.md#provider, decisions P7 and P8): every Project Wycheproof ML-KEM and
// SipHash-2-4 vector (the pinned C2SP/wycheproof testvectors_v1, read at run time), the vectors outside Cerid's
// profile refused, implicit rejection, every refusal and length rule, aliasing, zeroing on failure, keys generated,
// moved and cleared, and concurrent use of one key.
//
// The encapsulation vectors fix ML-KEM's randomness m, which kem_encapsulate never takes (FIPS 203 section 6.2 keeps
// ML-KEM.Encaps_internal from applications). Their known answers are checked through the provider's own
// deterministic encapsulation, the same encapsulation kem_encapsulate runs with a fresh m; kem_encapsulate itself is
// held to every invalid vector, and to a round trip through a decapsulation the decapsulation vectors prove.

#include "wycheproof.hpp"

#include <crd/crypto/provider.hpp>

#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>
#include <openssl/experimental/kem_deterministic_api.h>
#include <openssl/nid.h>

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
using crd::containers::String;
using crd::containers::StringView;
using namespace crd::crypto;
using namespace crd::crypto::test;

struct KemSuite
{
    const char* parameter_set; // the Wycheproof file infix
    AlgorithmId algorithm;
    int         nid; // the provider's parameter set, for its deterministic encapsulation
};

constexpr KemSuite kKemSuites[] = {
    {"768", AlgorithmId::MlKem768, NID_MLKEM768},
    {"1024", AlgorithmId::MlKem1024, NID_MLKEM1024},
};

Bytes sized(usize size, u8 fill = 0xAAU)
{
    Bytes out;
    out.resize(size);
    std::memset(out.data(), fill, size);
    return out;
}

Bytes public_key_of(const KemKey& key)
{
    Bytes out = sized(kem_sizes(key.algorithm()).public_key);
    REQUIRE(key.public_key(out) == CryptoError::None);
    return out;
}

// kem_encapsulate to `peer` and `key`'s decapsulation of the result agree.
void round_trip(const KemKey& key, ConstSpan<u8> peer)
{
    const KemSizes sizes      = kem_sizes(key.algorithm());
    Bytes          ciphertext = sized(sizes.ciphertext);
    Bytes          sent       = sized(sizes.secret);
    Bytes          received   = sized(sizes.secret);
    REQUIRE(kem_encapsulate(key.algorithm(), peer, ciphertext, sent) == CryptoError::None);
    REQUIRE(key.decapsulate(ciphertext, received) == CryptoError::None);
    CHECK(equal(sent, received));
    CHECK_FALSE(all_zero(sent));
}

// The provider's deterministic encapsulation with the vector's m (its experimental API; see the file comment).
bool provider_encapsulate_with(int nid, const Bytes& peer, const Bytes& m, Bytes& ciphertext, Bytes& secret)
{
    const bssl::UniquePtr<EVP_PKEY>     pkey(EVP_PKEY_kem_new_raw_public_key(nid, peer.data(), peer.size()));
    const bssl::UniquePtr<EVP_PKEY_CTX> context(pkey ? EVP_PKEY_CTX_new(pkey.get(), nullptr) : nullptr);
    usize                               ciphertext_length = ciphertext.size();
    usize                               secret_length     = secret.size();
    usize                               m_length          = m.size();
    return context &&
           EVP_PKEY_encapsulate_deterministic(context.get(), ciphertext.data(), &ciphertext_length, secret.data(),
                                              &secret_length, m.data(), &m_length) == 1 &&
           ciphertext_length == ciphertext.size() && secret_length == secret.size();
}

u64 load_le64(const Bytes& bytes)
{
    REQUIRE(bytes.size() == 8U);
    u64 value = 0U;
    for (usize i = 0U; i < 8U; ++i)
    {
        value |= static_cast<u64>(bytes[i]) << (8U * i);
    }
    return value;
}

// ---- ML-KEM against Wycheproof ----

TEST_CASE("Provider: ML-KEM keys from seeds against Wycheproof", "[crypto][net-2][wycheproof]")
{
    for (const KemSuite& suite : kKemSuites)
    {
        INFO("ML-KEM-" << suite.parameter_set);
        String name("mlkem_");
        name.append(suite.parameter_set);
        name.append("_keygen_seed_test");
        const Vectors v(name.c_str());
        u32           keys = 0U;
        v.each(
            [&](u32, u32 test, StringView result)
            {
                REQUIRE(result == StringView("valid"));
                KemKey key;
                REQUIRE(key.from_seed(suite.algorithm, v.hex(test, "seed")) == CryptoError::None);
                CHECK(key.algorithm() == suite.algorithm);
                const Bytes ek = public_key_of(key);
                CHECK(equal(ek, v.hex(test, "ek")));
                round_trip(key, ek);
                ++keys;
            });
        CHECK(keys == 100U);
    }
}

TEST_CASE("Provider: ML-KEM decapsulation and implicit rejection against Wycheproof", "[crypto][net-2][wycheproof]")
{
    struct Counts
    {
        u32 valid;
        u32 seed_length;
        u32 ciphertext_length;
    };
    const Counts want[] = {{161U, 20U, 20U}, {162U, 20U, 20U}};
    for (usize s = 0U; s < 2U; ++s)
    {
        const KemSuite& suite = kKemSuites[s];
        INFO("ML-KEM-" << suite.parameter_set);
        String name("mlkem_");
        name.append(suite.parameter_set);
        name.append("_test");
        const Vectors  v(name.c_str());
        const KemSizes sizes = kem_sizes(suite.algorithm);
        Counts         seen{0U, 0U, 0U};
        v.each(
            [&](u32, u32 test, StringView result)
            {
                const Bytes seed = v.hex(test, "seed");
                const Bytes c    = v.hex(test, "c");
                KemKey      key;
                if (seed.size() != sizes.seed)
                {
                    // An expanded or truncated private key: Cerid takes only the 64-byte seed.
                    REQUIRE(result == StringView("invalid"));
                    CHECK(key.from_seed(suite.algorithm, seed) == CryptoError::BadLength);
                    CHECK_FALSE(key.ready());
                    ++seen.seed_length;
                    return;
                }
                REQUIRE(key.from_seed(suite.algorithm, seed) == CryptoError::None);
                if (v.has(test, "ek") && !v.text(test, "ek").empty())
                {
                    CHECK(equal(public_key_of(key), v.hex(test, "ek")));
                }
                Bytes secret = sized(sizes.secret);
                const CryptoError opened = key.decapsulate(c, secret);
                if (c.size() != sizes.ciphertext)
                {
                    REQUIRE(result == StringView("invalid"));
                    CHECK(opened == CryptoError::BadLength);
                    CHECK(all_zero(secret));
                    ++seen.ciphertext_length;
                    return;
                }
                // Every ciphertext of the right length decapsulates: the strcmp, malleable, bit-flipped and random
                // ones to their implicit-rejection secret, which the vector names.
                REQUIRE(result == StringView("valid"));
                REQUIRE(opened == CryptoError::None);
                CHECK(equal(secret, v.hex(test, "K")));
                ++seen.valid;
            });
        CHECK(seen.valid == want[s].valid);
        CHECK(seen.seed_length == want[s].seed_length);
        CHECK(seen.ciphertext_length == want[s].ciphertext_length);
    }
}

TEST_CASE("Provider: ML-KEM encapsulation against Wycheproof", "[crypto][net-2][wycheproof]")
{
    struct Counts
    {
        u32 valid;
        u32 unreduced;
        u32 length;
    };
    const Counts want[] = {{133U, 112U, 20U}, {133U, 116U, 20U}};
    for (usize s = 0U; s < 2U; ++s)
    {
        const KemSuite& suite = kKemSuites[s];
        INFO("ML-KEM-" << suite.parameter_set);
        String name("mlkem_");
        name.append(suite.parameter_set);
        name.append("_encaps_test");
        const Vectors  v(name.c_str());
        const KemSizes sizes = kem_sizes(suite.algorithm);
        Counts         seen{0U, 0U, 0U};
        v.each(
            [&](u32, u32 test, StringView result)
            {
                const Bytes       ek         = v.hex(test, "ek");
                Bytes             ciphertext = sized(sizes.ciphertext);
                Bytes             secret     = sized(sizes.secret);
                const CryptoError sealed     = kem_encapsulate(suite.algorithm, ek, ciphertext, secret);
                if (result == StringView("invalid"))
                {
                    // A key of another length, or one encoding a coefficient of q or more (FIPS 203 section 7.2).
                    if (ek.size() != sizes.public_key)
                    {
                        CHECK(sealed == CryptoError::BadLength);
                        ++seen.length;
                    }
                    else
                    {
                        CHECK(sealed == CryptoError::BadKey);
                        ++seen.unreduced;
                    }
                    CHECK(all_zero(ciphertext));
                    CHECK(all_zero(secret));
                    return;
                }
                REQUIRE(result == StringView("valid"));
                REQUIRE(sealed == CryptoError::None);
                CHECK_FALSE(all_zero(secret));
                Bytes known_ciphertext = sized(sizes.ciphertext);
                Bytes known_secret     = sized(sizes.secret);
                REQUIRE(provider_encapsulate_with(suite.nid, ek, v.hex(test, "m"), known_ciphertext, known_secret));
                CHECK(equal(known_ciphertext, v.hex(test, "c")));
                CHECK(equal(known_secret, v.hex(test, "K")));
                // A fresh m: another ciphertext and secret for the same key (2^-256 to collide).
                CHECK_FALSE(equal(ciphertext, known_ciphertext));
                CHECK_FALSE(equal(secret, known_secret));
                ++seen.valid;
            });
        CHECK(seen.valid == want[s].valid);
        CHECK(seen.unreduced == want[s].unreduced);
        CHECK(seen.length == want[s].length);
    }
}

TEST_CASE("Provider: expanded ML-KEM decapsulation keys are outside Cerid's profile", "[crypto][net-2][wycheproof]")
{
    // Wycheproof's semi-expanded vectors test importers of FIPS 203's expanded decapsulation key (its hash and
    // embedded-key checks). Cerid imports only seeds, so each of those keys is refused by its length.
    for (const KemSuite& suite : kKemSuites)
    {
        INFO("ML-KEM-" << suite.parameter_set);
        String name("mlkem_");
        name.append(suite.parameter_set);
        name.append("_semi_expanded_decaps_test");
        const Vectors v(name.c_str());
        u32           refused = 0U;
        v.each(
            [&](u32, u32 test, StringView)
            {
                KemKey key;
                CHECK(key.from_seed(suite.algorithm, v.hex(test, "dk")) == CryptoError::BadLength);
                ++refused;
            });
        CHECK(refused == 9U);
    }
}

// ---- ML-KEM: the interface's rules ----

TEST_CASE("Provider: ML-KEM refusals, lengths, aliasing, implicit rejection and key lifetime", "[crypto][net-2]")
{
    CHECK(kem_sizes(AlgorithmId::MlKem768).public_key == 1184U);
    CHECK(kem_sizes(AlgorithmId::MlKem768).ciphertext == 1088U);
    CHECK(kem_sizes(AlgorithmId::MlKem1024).public_key == 1568U);
    CHECK(kem_sizes(AlgorithmId::MlKem1024).ciphertext == 1568U);
    CHECK(kem_sizes(AlgorithmId::X25519Mlkem768).seed == 0U); // a hybrid group is not this class

    for (const KemSuite& suite : kKemSuites)
    {
        INFO("ML-KEM-" << suite.parameter_set);
        const AlgorithmId alg   = suite.algorithm;
        const KemSizes    sizes = kem_sizes(alg);
        CHECK(sizes.seed == 64U);
        CHECK(sizes.secret == 32U);
        KemKey key;
        Bytes  ek         = sized(sizes.public_key);
        Bytes  ciphertext = sized(sizes.ciphertext);
        Bytes  secret     = sized(sizes.secret);
        CHECK(key.public_key(ek) == CryptoError::NotReady);
        CHECK(key.decapsulate(ciphertext, secret) == CryptoError::NotReady);
        REQUIRE(key.generate(alg) == CryptoError::None);
        CHECK(key.ready());
        CHECK(key.algorithm() == alg);
        ek = public_key_of(key);
        round_trip(key, ek);

        // Implicit rejection: a changed ciphertext decapsulates, deterministically, to another secret.
        REQUIRE(kem_encapsulate(alg, ek, ciphertext, secret) == CryptoError::None);
        Bytes changed = ciphertext;
        changed[sizes.ciphertext / 2U] ^= 0x01U;
        Bytes rejected = sized(sizes.secret);
        Bytes again    = sized(sizes.secret);
        REQUIRE(key.decapsulate(changed, rejected) == CryptoError::None);
        REQUIRE(key.decapsulate(changed, again) == CryptoError::None);
        CHECK(equal(rejected, again));
        CHECK_FALSE(equal(rejected, secret));
        // Another key's ciphertext: also a secret, and not the sender's.
        KemKey other;
        REQUIRE(other.generate(alg) == CryptoError::None);
        CHECK_FALSE(equal(public_key_of(other), ek));
        REQUIRE(other.decapsulate(ciphertext, again) == CryptoError::None);
        CHECK_FALSE(equal(again, secret));

        // Lengths: refused, outputs zeroed.
        Bytes short_ek = ek;
        short_ek.resize(ek.size() - 1U);
        CHECK(kem_encapsulate(alg, short_ek, ciphertext, secret) == CryptoError::BadLength);
        CHECK(all_zero(ciphertext));
        CHECK(all_zero(secret));
        Bytes long_ciphertext = sized(sizes.ciphertext + 1U);
        CHECK(kem_encapsulate(alg, ek, long_ciphertext, secret) == CryptoError::BadLength);
        CHECK(kem_encapsulate(alg, ek, ciphertext, Span<u8>(secret.data(), 31U)) == CryptoError::BadLength);
        secret = sized(sizes.secret);
        CHECK(key.decapsulate(ConstSpan<u8>(ciphertext.data(), sizes.ciphertext - 1U), secret) ==
              CryptoError::BadLength);
        CHECK(all_zero(secret));
        Bytes wide = sized(sizes.secret + 1U);
        CHECK(key.decapsulate(ciphertext, wide) == CryptoError::BadLength);
        CHECK(key.public_key(Span<u8>(ek.data(), ek.size() - 1U)) == CryptoError::BadLength);
        Bytes seed = sized(sizes.seed + 1U);
        CHECK(other.from_seed(alg, seed) == CryptoError::BadLength);
        CHECK_FALSE(other.ready());

        // Outputs overlapping an input, or each other: refused, nothing written.
        Bytes joint = sized(sizes.public_key + sizes.ciphertext);
        std::memcpy(joint.data(), ek.data(), ek.size());
        Bytes snapshot = joint;
        CHECK(kem_encapsulate(alg, ConstSpan<u8>(joint.data(), sizes.public_key),
                              Span<u8>(joint.data() + sizes.public_key - 1U, sizes.ciphertext),
                              secret) == CryptoError::Overlap);
        CHECK(kem_encapsulate(alg, ConstSpan<u8>(joint.data(), sizes.public_key), ciphertext,
                              Span<u8>(joint.data() + 8U, sizes.secret)) == CryptoError::Overlap);
        CHECK(equal(joint, snapshot));
        Bytes both = sized(sizes.ciphertext + sizes.secret);
        CHECK(kem_encapsulate(alg, ek, Span<u8>(both.data(), sizes.ciphertext),
                              Span<u8>(both.data() + sizes.ciphertext - 1U, sizes.secret)) == CryptoError::Overlap);
        REQUIRE(kem_encapsulate(alg, ek, Span<u8>(both.data(), sizes.ciphertext),
                                Span<u8>(both.data() + sizes.ciphertext, sizes.secret)) == CryptoError::None);
        CHECK(key.decapsulate(ConstSpan<u8>(both.data(), sizes.ciphertext),
                              Span<u8>(both.data() + sizes.ciphertext - 1U, sizes.secret)) == CryptoError::Overlap);

        // Moving keeps the key; the source is empty.
        KemKey moved(static_cast<KemKey&&>(key));
        CHECK_FALSE(key.ready());
        CHECK(key.algorithm() == AlgorithmId{});
        CHECK(moved.algorithm() == alg);
        round_trip(moved, ek);
        key = static_cast<KemKey&&>(moved);
        CHECK(key.ready());
        CHECK_FALSE(moved.ready());
        key.clear();
        CHECK_FALSE(key.ready());
        CHECK(key.algorithm() == AlgorithmId{});
    }

    // Other classes are refused, with the outputs zeroed.
    KemKey key;
    Bytes  ek         = sized(1184U);
    Bytes  ciphertext = sized(1088U);
    Bytes  secret     = sized(32U);
    CHECK(key.generate(AlgorithmId::X25519) == CryptoError::Unsupported);
    CHECK(key.from_seed(AlgorithmId::X25519Mlkem768, sized(64U)) == CryptoError::Unsupported);
    CHECK(kem_encapsulate(AlgorithmId::Secp256r1, ek, ciphertext, secret) == CryptoError::Unsupported);
    CHECK(all_zero(ciphertext));
    CHECK(all_zero(secret));

    // A seed determines its key: one seed, one encapsulation key; d and z both count.
    Bytes  seed = sized(64U, 0x5CU);
    KemKey a;
    KemKey b;
    REQUIRE(a.from_seed(AlgorithmId::MlKem768, seed) == CryptoError::None);
    REQUIRE(b.from_seed(AlgorithmId::MlKem768, seed) == CryptoError::None);
    CHECK(equal(public_key_of(a), public_key_of(b)));
    seed[0] ^= 0x01U; // d: another encapsulation key
    REQUIRE(b.from_seed(AlgorithmId::MlKem768, seed) == CryptoError::None);
    CHECK_FALSE(equal(public_key_of(a), public_key_of(b)));
    seed[0] ^= 0x01U;
    seed[63] ^= 0x01U; // z: the same encapsulation key, another implicit-rejection secret
    REQUIRE(b.from_seed(AlgorithmId::MlKem768, seed) == CryptoError::None);
    CHECK(equal(public_key_of(a), public_key_of(b)));
    const Bytes garbage = sized(1088U, 0x33U);
    Bytes       from_a  = sized(32U);
    Bytes       from_b  = sized(32U);
    REQUIRE(a.decapsulate(garbage, from_a) == CryptoError::None);
    REQUIRE(b.decapsulate(garbage, from_b) == CryptoError::None);
    CHECK_FALSE(equal(from_a, from_b));
}

// ---- SipHash-2-4 ----

TEST_CASE("Provider: SipHash-2-4 against Wycheproof", "[crypto][net-2][wycheproof]")
{
    const Vectors v("siphash_2_4_test");
    u32           tags = 0U;
    v.each(
        [&](u32 group, u32 test, StringView result)
        {
            REQUIRE(result == StringView("valid"));
            REQUIRE(v.number(group, "keySize") == 128U);
            REQUIRE(v.number(group, "tagSize") == 64U);
            PrfKey key;
            REQUIRE(key.init(AlgorithmId::Siphash24, v.hex(test, "key")) == CryptoError::None);
            u64 value = 0U;
            REQUIRE(key.hash(v.hex(test, "msg"), value) == CryptoError::None);
            CHECK(value == load_le64(v.hex(test, "tag")));
            ++tags;
        });
    CHECK(tags == 40U);
}

TEST_CASE("Provider: the table PRF's keys, refusals and lifetime", "[crypto][net-2]")
{
    PrfKey    key;
    const u8  message[] = {'c', 'o', 'n', 'n', 'e', 'c', 't', 'i', 'o', 'n'};
    u64       value     = 1U;
    CHECK_FALSE(key.ready());
    CHECK(key.hash(message, value) == CryptoError::NotReady);
    CHECK(value == 0U);
    CHECK(key.generate(AlgorithmId::HkdfSha256) == CryptoError::Unsupported);
    CHECK(key.init(AlgorithmId::Aes128Gcm, sized(kPrfKeyBytes)) == CryptoError::Unsupported);
    CHECK(key.init(AlgorithmId::Siphash24, sized(kPrfKeyBytes - 1U)) == CryptoError::BadLength);
    CHECK(key.init(AlgorithmId::Siphash24, sized(kPrfKeyBytes + 1U)) == CryptoError::BadLength);
    CHECK_FALSE(key.ready());

    // Fresh keys: each table hashes the same input to its own value (2^-64 to collide).
    PrfKey other;
    REQUIRE(key.generate(AlgorithmId::Siphash24) == CryptoError::None);
    REQUIRE(other.generate(AlgorithmId::Siphash24) == CryptoError::None);
    CHECK(key.algorithm() == AlgorithmId::Siphash24);
    u64 mine   = 0U;
    u64 theirs = 0U;
    u64 again  = 0U;
    REQUIRE(key.hash(message, mine) == CryptoError::None);
    REQUIRE(other.hash(message, theirs) == CryptoError::None);
    REQUIRE(key.hash(message, again) == CryptoError::None);
    CHECK(mine == again);
    CHECK(mine != theirs);
    REQUIRE(key.hash(ConstSpan<u8>(), again) == CryptoError::None);

    // A shared key: two processes' tables agree; k0 and k1 are not interchangeable.
    Bytes shared = sized(kPrfKeyBytes);
    for (usize i = 0U; i < shared.size(); ++i)
    {
        shared[i] = static_cast<u8>(i);
    }
    REQUIRE(key.init(AlgorithmId::Siphash24, shared) == CryptoError::None);
    REQUIRE(other.init(AlgorithmId::Siphash24, shared) == CryptoError::None);
    REQUIRE(key.hash(message, mine) == CryptoError::None);
    REQUIRE(other.hash(message, theirs) == CryptoError::None);
    CHECK(mine == theirs);
    Bytes swapped = sized(kPrfKeyBytes);
    std::memcpy(swapped.data(), shared.data() + 8U, 8U);
    std::memcpy(swapped.data() + 8U, shared.data(), 8U);
    REQUIRE(other.init(AlgorithmId::Siphash24, swapped) == CryptoError::None);
    REQUIRE(other.hash(message, theirs) == CryptoError::None);
    CHECK(mine != theirs);

    key.clear();
    CHECK_FALSE(key.ready());
    CHECK(key.algorithm() == AlgorithmId{});
    CHECK(key.hash(message, value) == CryptoError::NotReady);
    CHECK(value == 0U);
}

// ---- Concurrency ----

TEST_CASE("Provider: one KEM key and one PRF key serve several threads", "[crypto][net-2]")
{
    KemKey kem;
    REQUIRE(kem.generate(AlgorithmId::MlKem768) == CryptoError::None);
    const Bytes ek = public_key_of(kem);
    PrfKey      prf;
    REQUIRE(prf.generate(AlgorithmId::Siphash24) == CryptoError::None);
    const u8 message[] = {'t', 'h', 'r', 'e', 'a', 'd', 's'};
    u64      expected  = 0U;
    REQUIRE(prf.hash(message, expected) == CryptoError::None);
    std::atomic<u32> failures{0U};
    std::thread      threads[4];
    for (std::thread& thread : threads)
    {
        thread = std::thread(
            [&]
            {
                u8 ciphertext[1088];
                u8 sent[32];
                u8 received[32];
                for (u32 i = 0U; i < 16U; ++i)
                {
                    u64 value = 0U;
                    if (kem_encapsulate(AlgorithmId::MlKem768, ek, ciphertext, sent) != CryptoError::None ||
                        kem.decapsulate(ciphertext, received) != CryptoError::None ||
                        std::memcmp(sent, received, sizeof(sent)) != 0 ||
                        prf.hash(message, value) != CryptoError::None || value != expected)
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
} // namespace
