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

TEST_CASE("Register ids: the IANA and NIST codepoints the first refresh confirmed", "[crypto][net-std]")
{
    // Checked against the IANA registry files and the NIST object identifier register on 2026-10-31
    // (docs/sessions/2026-10-31-standards-register-refresh.md).
    namespace iana = crd::crypto::iana;
    CHECK(iana::kGroupX25519mlkem768 == 0x11ECU);
    CHECK(iana::kGroupSecp256r1mlkem768 == 0x11EBU);
    CHECK(iana::kGroupSecp384r1mlkem1024 == 0x11EDU);
    CHECK(iana::kGroupX25519 == 0x001DU);
    CHECK(iana::kGroupSecp256r1 == 0x0017U);
    CHECK(iana::kGroupSecp384r1 == 0x0018U);
    CHECK(iana::kTlsExtensionEncryptedClientHello == 0xFE0DU);
    CHECK(iana::kTlsExtensionCompressCertificate == 27U);
    CHECK(iana::kOidIdMlDsa44 == "2.16.840.1.101.3.4.3.17");
    CHECK(iana::kOidIdMlDsa65 == "2.16.840.1.101.3.4.3.18");
    CHECK(iana::kOidIdMlDsa87 == "2.16.840.1.101.3.4.3.19");
    CHECK(iana::kSignatureSchemeEd25519 == 0x0807U);
    CHECK(iana::kSignatureSchemeEcdsaSecp256r1Sha256 == 0x0403U);
    CHECK(iana::kSignatureSchemeEcdsaSecp384r1Sha384 == 0x0503U);
    CHECK(iana::kAeadAes128Gcm == 1U);
    CHECK(iana::kAeadAes256Gcm == 2U);
    CHECK(iana::kAeadChacha20Poly1305 == 29U);
}
