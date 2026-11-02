#pragma once

// crd-crypto tests -- what the MLS suites share (test_mls*.cpp): the working group's vector files, the suite of a
// vector, byte views, a deterministic generator for the model groups' choices, and a member's long-term identity with
// its signed leaf node.

#include "wycheproof.hpp"

#include <crd/crypto/mls_tree.hpp>
#include <crd/crypto/provider.hpp>

#include <catch2/catch_test_macros.hpp>

namespace crd::crypto::test
{
inline JsonFile mls_vectors(const char* name)
{
    return JsonFile(vector_path(CRD_MLS_VECTORS_DIR, name));
}

inline containers::Span<u8> span_of(Bytes& bytes)
{
    return containers::Span<u8>(bytes.data(), bytes.size());
}

inline containers::ConstSpan<u8> view(const Bytes& bytes)
{
    return containers::ConstSpan<u8>(bytes.data(), bytes.size());
}

inline const mls::CipherSuite& suite_of(u16 id)
{
    const mls::CipherSuite* suite = mls::cipher_suite(id);
    REQUIRE(suite != nullptr);
    return *suite;
}

// The suite of vector `v`, or null (with the check that it is one of the three outside the profile).
inline const mls::CipherSuite* profile_suite(const JsonFile& vectors, u32 v)
{
    const u16               id    = static_cast<u16>(vectors.number(v, "cipher_suite"));
    const mls::CipherSuite* suite = mls::cipher_suite(id);
    if (suite == nullptr)
    {
        CHECK((id >= 4U && id <= 6U)); // X448, Ed448 and P-521: outside the profile
    }
    return suite;
}

// A deterministic generator for the model groups' choices.
class Choices
{
public:
    explicit Choices(u64 seed) : m_state(seed) {}
    u32 below(u32 bound)
    {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;
        return static_cast<u32>(m_state % bound);
    }

private:
    u64 m_state;
};

// A member's long-term signer and its current leaf, whose fields view the member's own bytes.
struct Identity
{
    SigningKey    signer;
    Bytes         signature_key;
    Bytes         hpke_private;
    Bytes         hpke_public;
    Bytes         credential;
    Bytes         versions;
    Bytes         suites;
    Bytes         credentials;
    Bytes         extension_types; // the non-default extension types the member's capabilities list
    Bytes         signature;
    mls::LeafNode leaf;
};

inline Bytes random_of(usize size)
{
    Bytes out = filled(size, 0U);
    REQUIRE(random_bytes(span_of(out)) == CryptoError::None);
    return out;
}

// A fresh HPKE key pair for the identity (a random scalar, drawn again in the rare case it is out of range).
inline void fresh_hpke(const mls::CipherSuite& suite, Identity& id)
{
    HpkeKey key;
    do
    {
        id.hpke_private = random_of(hpke_sizes(suite.kem).private_key);
    } while (key.from_private(suite.kem, view(id.hpke_private)) != CryptoError::None);
    id.hpke_public = filled(hpke_sizes(suite.kem).public_key, 0U);
    REQUIRE(key.public_key(span_of(id.hpke_public)) == CryptoError::None);
}

// The identity's leaf of `source`, signed (with the group and index for an update or commit leaf); a key-package
// leaf lives from 0 to the end of time.
inline void sign_leaf(const mls::CipherSuite& suite, Identity& id, mls::LeafNodeSource source,
                      containers::ConstSpan<u8> group_id, u32 index)
{
    id.leaf                            = {};
    id.leaf.encryption_key             = view(id.hpke_public);
    id.leaf.signature_key              = view(id.signature_key);
    id.leaf.credential.type            = static_cast<u16>(mls::CredentialType::Basic);
    id.leaf.credential.content         = view(id.credential);
    id.leaf.capabilities.versions      = view(id.versions);
    id.leaf.capabilities.cipher_suites = view(id.suites);
    id.leaf.capabilities.extensions    = view(id.extension_types);
    id.leaf.capabilities.credentials   = view(id.credentials);
    id.leaf.source                     = source;
    id.leaf.not_before                 = 0U;
    id.leaf.not_after                  = ~u64{0};
    id.signature                       = filled(signature_sizes(suite.signature).signature, 0U);
    usize written                      = 0U;
    REQUIRE(mls::sign_leaf_node(suite, id.signer, id.leaf, group_id, index, span_of(id.signature), written) ==
            CryptoError::None);
    id.signature.resize(written);
    id.leaf.signature = view(id.signature);
}

// A member named `name` (its basic credential's last byte) of the suite: mls10, the suite, basic credentials.
inline void make_identity(const mls::CipherSuite& suite, Identity& id, u32 name)
{
    REQUIRE(id.signer.generate(suite.signature) == CryptoError::None);
    id.signature_key = filled(signature_sizes(suite.signature).public_key, 0U);
    REQUIRE(id.signer.public_key(span_of(id.signature_key)) == CryptoError::None);
    id.credential    = filled(4U, 0U);
    id.credential[3] = static_cast<u8>(name);
    id.versions      = Bytes{0x00U, 0x01U};
    id.suites        = Bytes{static_cast<u8>(suite.id >> 8U), static_cast<u8>(suite.id)};
    id.credentials   = Bytes{0x00U, 0x01U};
    fresh_hpke(suite, id);
    sign_leaf(suite, id, mls::LeafNodeSource::KeyPackage, {}, 0U);
}
} // namespace crd::crypto::test
