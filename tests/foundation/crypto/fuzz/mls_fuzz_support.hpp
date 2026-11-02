#pragma once

// crd-crypto fuzz -- what the MLS group targets share (fuzz_mls_group.cpp, fuzz_mls_commit.cpp): a field reader over
// the input, byte views and comparison, a structure written through its codec, and a member with its signer, HPKE keys
// and signed key-package leaf, with the Authentication Service and store a fuzzed group consults.

#include <crd/fuzz/harness.hpp>

#include <crd/crypto/mls_group.hpp>

#include <cstdio>
#include <cstring>

namespace crd::crypto::fuzz_mls
{
namespace mls = crd::crypto::mls;
using crd::u16;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireWriter;

// The profile's cipher suites (decision M2): honest groups are kept per suite, the count checked where they are made.
inline constexpr usize kSuites = 15U;

// The input read field by field; a short input ends the case (ok() is false).
class Reader
{
public:
    Reader(const u8* data, usize size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }

    u8 byte() noexcept
    {
        if (m_at >= m_size)
        {
            m_ok = false;
            return 0U;
        }
        return m_data[m_at++];
    }

    u16 u16be() noexcept { return static_cast<u16>(static_cast<u16>(byte() << 8U) | byte()); }

    ConstSpan<u8> rest() noexcept
    {
        const ConstSpan<u8> out(m_data + m_at, m_size - m_at);
        m_at = m_size;
        return out;
    }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
    bool      m_ok = true;
};

inline bool same(ConstSpan<u8> a, ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

inline ConstSpan<u8> view(const Array<u8>& bytes) noexcept
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

template <typename T, typename Size, typename Put>
Array<u8> written(const T& value, Size size, Put put)
{
    Array<u8> out;
    out.resize(size(value));
    WireWriter writer(Span<u8>(out.data(), out.size()));
    put(writer, value);
    CRD_FUZZ_REQUIRE(writer.written() == out.size());
    return out;
}

class AnyCredential final : public mls::AuthenticationService
{
public:
    [[nodiscard]] bool validate(const mls::Credential&, ConstSpan<u8>) const noexcept override { return true; }
    [[nodiscard]] bool valid_successor(const mls::Credential&, const mls::Credential&) const noexcept override
    {
        return true;
    }
};

class EmptyStore final : public mls::GroupStore
{
public:
    [[nodiscard]] ConstSpan<u8> psk(const mls::PreSharedKeyId&) const noexcept override { return {}; }
    [[nodiscard]] bool          member_of(ConstSpan<u8>) const noexcept override { return false; }
};

// A key pair of the suite's KEM: a random private key, drawn again when out of range.
inline void hpke_pair(const mls::CipherSuite& suite, Array<u8>& private_key, Array<u8>& public_key)
{
    HpkeKey key;
    private_key.resize(hpke_sizes(suite.kem).private_key);
    do
    {
        CRD_FUZZ_REQUIRE(random_bytes(Span<u8>(private_key.data(), private_key.size())) == CryptoError::None);
    } while (key.from_private(suite.kem, view(private_key)) != CryptoError::None);
    public_key.resize(hpke_sizes(suite.kem).public_key);
    CRD_FUZZ_REQUIRE(key.public_key(Span<u8>(public_key.data(), public_key.size())) == CryptoError::None);
}

// A member's signer, keys and key-package leaf, whose fields view the member's own bytes.
struct Member
{
    SigningKey    signer;
    Array<u8>     signature_key;
    Array<u8>     leaf_private;
    Array<u8>     leaf_public;
    Array<u8>     init_private;
    Array<u8>     init_public;
    Array<u8>     leaf_signature;
    Array<u8>     package_signature;
    mls::LeafNode leaf;
};

inline constexpr u8 kBasic[]      = {0x00U, 0x01U};
inline constexpr u8 kVersions[]   = {0x00U, 0x01U};
inline constexpr u8 kCredential[] = {'m', 'e', 'm', 'b', 'e', 'r'};

inline void make_member(const mls::CipherSuite& suite, Member& member, const u8 (&suites)[2])
{
    CRD_FUZZ_REQUIRE(member.signer.generate(suite.signature) == CryptoError::None);
    member.signature_key.resize(signature_sizes(suite.signature).public_key);
    CRD_FUZZ_REQUIRE(member.signer.public_key(Span<u8>(member.signature_key.data(), member.signature_key.size())) ==
                     CryptoError::None);
    hpke_pair(suite, member.leaf_private, member.leaf_public);
    hpke_pair(suite, member.init_private, member.init_public);
    member.leaf                            = {};
    member.leaf.encryption_key             = view(member.leaf_public);
    member.leaf.signature_key              = view(member.signature_key);
    member.leaf.credential.type            = 1U;
    member.leaf.credential.content         = kCredential;
    member.leaf.capabilities.versions      = kVersions;
    member.leaf.capabilities.cipher_suites = suites;
    member.leaf.capabilities.credentials   = kBasic;
    member.leaf.not_after                  = ~u64{0};
    member.leaf_signature.resize(signature_sizes(suite.signature).signature);
    usize written_bytes = 0U;
    CRD_FUZZ_REQUIRE(mls::sign_leaf_node(suite, member.signer, member.leaf, {}, 0U,
                                         Span<u8>(member.leaf_signature.data(), member.leaf_signature.size()),
                                         written_bytes) == CryptoError::None);
    member.leaf_signature.resize(written_bytes);
    member.leaf.signature = view(member.leaf_signature);
}

// The member's key package of `suite` around its leaf and init key, signed.
inline void make_package(const mls::CipherSuite& suite, Member& member, mls::KeyPackage& package)
{
    package              = {};
    package.cipher_suite = suite.id;
    package.init_key     = view(member.init_public);
    package.leaf_node    = member.leaf;
    member.package_signature.resize(signature_sizes(suite.signature).signature);
    usize written_bytes = 0U;
    CRD_FUZZ_REQUIRE(mls::sign_key_package(suite, member.signer, package,
                                           Span<u8>(member.package_signature.data(), member.package_signature.size()),
                                           written_bytes) == CryptoError::None);
    member.package_signature.resize(written_bytes);
    package.signature = view(member.package_signature);
}
} // namespace crd::crypto::fuzz_mls
