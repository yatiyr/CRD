// crd-crypto -- the identifiers generated from the NET-STD standards register (register_ids.hpp). The drift CTest
// (crd-crypto-register-ids-drift) proves the header is the generator's output for the committed register; these
// tests prove the generated values keep the register's rules.

#include <crd/crypto/register_ids.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
using crd::u16;
using crd::usize;
using crd::crypto::algorithm_class;
using crd::crypto::algorithm_register_id;
using crd::crypto::AlgorithmClass;
using crd::crypto::AlgorithmId;
using crd::crypto::kAlgorithmIds;

constexpr usize kCount = sizeof(kAlgorithmIds) / sizeof(kAlgorithmIds[0]);

bool known_class(AlgorithmId id)
{
    const AlgorithmClass type = algorithm_class(id);
    return type >= AlgorithmClass::HybridKeyExchange && type <= AlgorithmClass::Kdf;
}
} // namespace

TEST_CASE("Register ids: every algorithm has a unique identifier, a class and its register entry", "[crypto][net-std]")
{
    static_assert(kCount == 17U, "the algorithms the network foundation's provider policy names");
    for (usize i = 0U; i < kCount; ++i)
    {
        const AlgorithmId id = kAlgorithmIds[i];
        INFO("algorithm 0x" << std::hex << static_cast<u16>(id));
        CHECK(known_class(id));
        CHECK((static_cast<u16>(id) & 0xFFU) != 0U);
        const char* entry = algorithm_register_id(id);
        CHECK((std::strncmp(entry, "crypto.", 7U) == 0 || std::strncmp(entry, "tls.", 4U) == 0));
        for (usize j = 0U; j < i; ++j)
        {
            CHECK(kAlgorithmIds[j] != id);
        }
    }
    CHECK(std::strcmp(algorithm_register_id(AlgorithmId::X25519Mlkem768), "tls.hybrid-kex") == 0);
    CHECK(std::strcmp(algorithm_register_id(AlgorithmId::MlDsa87), "crypto.ml-dsa") == 0);
    CHECK(std::strcmp(algorithm_register_id(static_cast<AlgorithmId>(0x7F01U)), "") == 0);
}

TEST_CASE("Register ids: the class byte separates hybrids, traditional groups, signatures and symmetric algorithms",
          "[crypto][net-std]")
{
    CHECK(algorithm_class(AlgorithmId::X25519Mlkem768) == AlgorithmClass::HybridKeyExchange);
    CHECK(algorithm_class(AlgorithmId::Secp384r1Mlkem1024) == AlgorithmClass::HybridKeyExchange);
    CHECK(algorithm_class(AlgorithmId::X25519) == AlgorithmClass::ClassicalKeyExchange);
    CHECK(algorithm_class(AlgorithmId::MlDsa65) == AlgorithmClass::PostQuantumSignature);
    CHECK(algorithm_class(AlgorithmId::Ed25519) == AlgorithmClass::ClassicalSignature);
    CHECK(algorithm_class(AlgorithmId::EcdsaSecp384r1Sha384) == AlgorithmClass::ClassicalSignature);
    CHECK(algorithm_class(AlgorithmId::Chacha20Poly1305) == AlgorithmClass::Aead);
    CHECK(algorithm_class(AlgorithmId::HkdfSha384) == AlgorithmClass::Kdf);
    CHECK_FALSE(known_class(static_cast<AlgorithmId>(0x7F01U)));
    CHECK_FALSE(known_class(static_cast<AlgorithmId>(0x0001U)));
}

TEST_CASE("Register ids: IANA values taken from knowledge stay marked unverified", "[crypto][net-std]")
{
    namespace unverified = crd::crypto::iana::unverified;
    CHECK(unverified::kGroupX25519mlkem768 == 0x11ECU);
    CHECK(unverified::kGroupX25519 == 0x001DU);
    CHECK(unverified::kSignatureSchemeEd25519 == 0x0807U);
    CHECK(unverified::kAeadChacha20Poly1305 == 29U);
    CHECK(unverified::kOidIdMlDsa87 == "2.16.840.1.101.3.4.3.19");
}
