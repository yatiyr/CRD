// crd-crypto -- hybrid public key encryption, RFC 9180 base mode (NET.2; contract:
// docs/design/network-foundation.md#hpke).
//
// RFC 9180's construction, step by step, over the provider interface: DHKEM (section 4.1) over AgreementKey and its
// DeriveKeyPair (section 7.1.3); ML-KEM over KemKey and the post-quantum/traditional hybrids (the CG framework of
// draft-irtf-cfrg-hybrid-kems section 5.5 with the components of draft-irtf-cfrg-concrete-hybrid-kems section 4) over
// both, with draft-ietf-hpke-pq's SHAKE256 DeriveKeyPair (sections 3 and 4); the key schedule (section 5.1) over HKDF,
// the first message of a context (section 5.2) over AeadKey and the context's exporter (section 5.3). Every secret
// this unit computes lives on the stack of one call and is zeroed before it returns.
#include <crd/crypto/hpke.hpp>

#include "byte_spans.hpp"

#include <crd/containers/wire_bytes.hpp>

#include <cstring>
#include <utility>

namespace crd::crypto
{
namespace
{
using detail::label_of;
using detail::LabelBuffer;
using detail::overlaps;
using detail::refuse;

constexpr u8    kVersion[]         = {'H', 'P', 'K', 'E', '-', 'v', '1'};
constexpr usize kMaxHashBytes      = 48U;  // Nh of HKDF-SHA384
constexpr usize kMaxElementBytes   = 97U;  // a P-384 point: a DHKEM's Npk, a hybrid's traditional element
constexpr usize kMaxSecretBytes    = 48U;  // Ndh and Nsk of P-384
constexpr usize kMaxKeyBytes       = 32U;  // Nk of AES-256-GCM and ChaCha20-Poly1305
constexpr usize kKemSecretBytes    = 32U;  // Nsecret of an ML-KEM and of a hybrid (SHA3-256's output)
constexpr usize kHybridSeedBytes   = 32U;  // a hybrid's Nseed and Nsk
constexpr usize kMlKemSeedBytes    = 64U;  // an ML-KEM's d || z
constexpr usize kMaxGroupSeedBytes = 128U; // Group_T.Nseed of P-256, the longest
constexpr usize kMinPostQuantumIkm = 32U;  // DeriveKeyPair's shortest input for an ML-KEM or a hybrid (decision H2)
constexpr u8    kModeBase          = 0x00U;

enum class KemKind : u8
{
    Dhkem,
    MlKem,
    Hybrid,
};

// The combiner labels of draft-irtf-cfrg-concrete-hybrid-kems section 4.
constexpr u8 kLabelP256[]   = {'M', 'L', 'K', 'E', 'M', '7', '6', '8', '-', 'P', '2', '5', '6'};
constexpr u8 kLabelX25519[] = {0x5CU, 0x2EU, 0x2FU, 0x2FU, 0x5EU, 0x5CU}; // X-Wing's
constexpr u8 kLabelP384[]   = {'M', 'L', 'K', 'E', 'M', '1', '0', '2', '4', '-', 'P', '3', '8', '4'};

struct Kem
{
    AlgorithmId         id;     // the name the interface takes
    u16                 code;   // the HPKE KEM identifier
    KemKind             kind;
    AlgorithmId         kdf;    // a DHKEM's own KDF, whose Nh is Nsecret
    AlgorithmId         group;  // a DHKEM's group, or a hybrid's traditional half
    AlgorithmId         pq;     // an ML-KEM, or a hybrid's post-quantum half
    usize               seed_t; // a hybrid's Group_T.Nseed
    cont::ConstSpan<u8> label;  // a hybrid's combiner label
    bool                scalar; // a DHKEM's DeriveKeyPair samples a scalar below the group order (the NIST curves)
};

constexpr Kem kKems[] = {
    {AlgorithmId::X25519, iana::kHpkeKemDhkemX25519HkdfSha256, KemKind::Dhkem, AlgorithmId::HkdfSha256,
     AlgorithmId::X25519, AlgorithmId{}, 0U, {}, false},
    {AlgorithmId::Secp256r1, iana::kHpkeKemDhkemP256HkdfSha256, KemKind::Dhkem, AlgorithmId::HkdfSha256,
     AlgorithmId::Secp256r1, AlgorithmId{}, 0U, {}, true},
    {AlgorithmId::Secp384r1, iana::kHpkeKemDhkemP384HkdfSha384, KemKind::Dhkem, AlgorithmId::HkdfSha384,
     AlgorithmId::Secp384r1, AlgorithmId{}, 0U, {}, true},
    {AlgorithmId::MlKem768, iana::kHpkeKemMlKem768, KemKind::MlKem, AlgorithmId{}, AlgorithmId{},
     AlgorithmId::MlKem768, 0U, {}, false},
    {AlgorithmId::MlKem1024, iana::kHpkeKemMlKem1024, KemKind::MlKem, AlgorithmId{}, AlgorithmId{},
     AlgorithmId::MlKem1024, 0U, {}, false},
    {AlgorithmId::X25519Mlkem768, iana::kHpkeKemXWing, KemKind::Hybrid, AlgorithmId{}, AlgorithmId::X25519,
     AlgorithmId::MlKem768, 32U, kLabelX25519, false},
    {AlgorithmId::Secp256r1Mlkem768, iana::kHpkeKemMlkem768P256, KemKind::Hybrid, AlgorithmId{}, AlgorithmId::Secp256r1,
     AlgorithmId::MlKem768, 128U, kLabelP256, false},
    {AlgorithmId::Secp384r1Mlkem1024, iana::kHpkeKemMlkem1024P384, KemKind::Hybrid, AlgorithmId{},
     AlgorithmId::Secp384r1, AlgorithmId::MlKem1024, 48U, kLabelP384, false},
};

const Kem* find_kem(AlgorithmId id) noexcept
{
    for (const Kem& kem : kKems)
    {
        if (kem.id == id)
        {
            return &kem;
        }
    }
    return nullptr;
}

HpkeSizes sizes_of(const Kem& kem) noexcept
{
    const AgreementSizes group = agreement_sizes(kem.group);
    const KemSizes       pq    = kem_sizes(kem.pq);
    switch (kem.kind)
    {
    case KemKind::Dhkem:
        return {group.public_key, group.public_key, group.private_key};
    case KemKind::MlKem:
        return {pq.public_key, pq.ciphertext, pq.seed};
    case KemKind::Hybrid:
        return {pq.public_key + group.public_key, pq.ciphertext + group.public_key, kHybridSeedBytes};
    }
    return {};
}

// Nsecret: a DHKEM's is its KDF's Nh, an ML-KEM's and a hybrid's 32 bytes.
usize secret_bytes(const Kem& kem) noexcept
{
    return kem.kind == KemKind::Dhkem ? hkdf_hash_bytes(kem.kdf) : kKemSecretBytes;
}

u16 kdf_id(AlgorithmId kdf) noexcept
{
    switch (kdf)
    {
    case AlgorithmId::HkdfSha256:
        return iana::kHpkeKdfHkdfSha256;
    case AlgorithmId::HkdfSha384:
        return iana::kHpkeKdfHkdfSha384;
    default:
        return 0U;
    }
}

u16 aead_id(AlgorithmId aead) noexcept
{
    switch (aead)
    {
    case AlgorithmId::Aes128Gcm:
        return iana::kHpkeAeadAes128Gcm;
    case AlgorithmId::Aes256Gcm:
        return iana::kHpkeAeadAes256Gcm;
    case AlgorithmId::Chacha20Poly1305:
        return iana::kHpkeAeadChacha20poly1305;
    default:
        return 0U;
    }
}

// The implicit suite_id of a labelled KDF call: "KEM" || kem_id inside the KEM, "HPKE" || kem_id || kdf_id || aead_id
// in the key schedule.
struct SuiteId
{
    u8    bytes[10] = {};
    usize size      = 0U;

    [[nodiscard]] cont::ConstSpan<u8> span() const noexcept { return cont::ConstSpan<u8>(bytes, size); }
};

SuiteId kem_suite(const Kem& kem) noexcept
{
    SuiteId          id;
    cont::WireWriter out(cont::Span<u8>(id.bytes, 5U));
    out.put_bytes(label_of("KEM"));
    out.put_u16(kem.code);
    id.size = out.written();
    return id;
}

SuiteId hpke_suite(const Kem& kem, const HpkeSuite& suite) noexcept
{
    SuiteId          id;
    cont::WireWriter out(cont::Span<u8>(id.bytes));
    out.put_bytes(label_of("HPKE"));
    out.put_u16(kem.code);
    out.put_u16(kdf_id(suite.kdf));
    out.put_u16(aead_id(suite.aead));
    id.size = out.written();
    return id;
}

// LabeledExtract(salt, label, ikm) = Extract(salt, "HPKE-v1" || suite_id || label || ikm).
CryptoError labeled_extract(AlgorithmId kdf, const SuiteId& suite, cont::ConstSpan<u8> salt, cont::ConstSpan<u8> label,
                            cont::ConstSpan<u8> ikm, cont::Span<u8> prk)
{
    LabelBuffer      input(sizeof(kVersion) + suite.size + label.size() + ikm.size());
    cont::WireWriter out(input.span());
    out.put_bytes(kVersion);
    out.put_bytes(suite.span());
    out.put_bytes(label);
    out.put_bytes(ikm);
    return hkdf_extract(kdf, salt, input.span(), prk);
}

// LabeledExpand(prk, label, info, L) = Expand(prk, I2OSP(L, 2) || "HPKE-v1" || suite_id || label || info, L).
CryptoError labeled_expand(AlgorithmId kdf, const SuiteId& suite, cont::ConstSpan<u8> prk, cont::ConstSpan<u8> label,
                           cont::ConstSpan<u8> info, cont::Span<u8> out)
{
    LabelBuffer      input(2U + sizeof(kVersion) + suite.size + label.size() + info.size());
    cont::WireWriter writer(input.span());
    writer.put_u16(static_cast<u16>(out.size()));
    writer.put_bytes(kVersion);
    writer.put_bytes(suite.span());
    writer.put_bytes(label);
    writer.put_bytes(info);
    return hkdf_expand(kdf, prk, input.span(), out);
}

// SHAKE256's LabeledDerive(ikm, label, "", L) (draft-ietf-hpke-hpke section 4.4, draft-ietf-hpke-pq section 5):
// SHAKE256(ikm || "HPKE-v1" || suite_id || lengthPrefixed(label) || I2OSP(L, 2)) into `out`, L = out.size().
CryptoError labeled_derive(const SuiteId& suite, cont::ConstSpan<u8> ikm, cont::ConstSpan<u8> label,
                           cont::Span<u8> out)
{
    LabelBuffer      input(ikm.size() + sizeof(kVersion) + suite.size + 2U + label.size() + 2U);
    cont::WireWriter writer(input.span());
    writer.put_bytes(ikm);
    writer.put_bytes(kVersion);
    writer.put_bytes(suite.span());
    writer.put_u16(static_cast<u16>(label.size()));
    writer.put_bytes(label);
    writer.put_u16(static_cast<u16>(out.size()));
    return shake256(input.span(), out);
}

// ExtractAndExpand(dh, enc || pkRm) into `shared_secret` (Nsecret, the DHKEM KDF's Nh).
CryptoError extract_and_expand(const Kem& kem, cont::ConstSpan<u8> dh, cont::ConstSpan<u8> enc,
                               cont::ConstSpan<u8> recipient, cont::Span<u8> shared_secret)
{
    u8 context[2U * kMaxElementBytes];
    std::memcpy(context, enc.data(), enc.size());
    std::memcpy(context + enc.size(), recipient.data(), recipient.size());
    const SuiteId  suite = kem_suite(kem);
    u8             prk_bytes[kMaxHashBytes];
    cont::Span<u8> prk(prk_bytes, shared_secret.size());
    CryptoError    error = labeled_extract(kem.kdf, suite, {}, label_of("eae_prk"), dh, prk);
    if (error == CryptoError::None)
    {
        error = labeled_expand(kem.kdf, suite, prk, label_of("shared_secret"),
                               cont::ConstSpan<u8>(context, enc.size() + recipient.size()), shared_secret);
    }
    secure_zero(prk);
    return error;
}

// Group_T.RandomScalar(seed) (draft-irtf-cfrg-concrete-hybrid-kems section 3.1) into `key`: X25519's scalar is the
// seed itself; a NIST curve's is the first Nscalar-byte chunk of the seed in [1, n - 1], so a chunk out of range
// (BadKey from the agreement key) moves on to the next, and a seed with none in range is BadKey.
CryptoError random_scalar(AlgorithmId group, cont::ConstSpan<u8> seed, AgreementKey& key) noexcept
{
    const usize scalar = agreement_sizes(group).private_key;
    CryptoError error  = CryptoError::BadKey;
    for (usize at = 0U; at + scalar <= seed.size() && error == CryptoError::BadKey; at += scalar)
    {
        error = key.from_private(group, seed.subspan(at, scalar));
    }
    return error;
}

// Group_T.ElementToSharedSecret(Group_T.Exp(element, key)): the agreement's secret, except that an X25519 result of
// all zeros is the secret RFC 7748 computes (the CG combiner binds the element and the encapsulation key, as X-Wing
// does; AgreementKey::derive leaves the zeros in `secret` when it reports WeakSecret).
CryptoError exponentiate(const AgreementKey& key, cont::ConstSpan<u8> element, cont::Span<u8> secret) noexcept
{
    const CryptoError error = key.derive(element, secret);
    return error == CryptoError::WeakSecret && key.algorithm() == AlgorithmId::X25519 ? CryptoError::None : error;
}

// C2PRICombiner(ss_PQ, ss_T, ct_T, ek_T, label) = SHA3-256(ss_PQ || ss_T || ct_T || ek_T || label)
// (draft-irtf-cfrg-hybrid-kems section 5.1.3) into `shared_secret` (32 bytes).
CryptoError combine(const Kem& kem, cont::ConstSpan<u8> pq_secret, cont::ConstSpan<u8> group_secret,
                    cont::ConstSpan<u8> element, cont::ConstSpan<u8> group_public, cont::Span<u8> shared_secret)
{
    u8          input[kKemSecretBytes + kMaxSecretBytes + 2U * kMaxElementBytes + sizeof(kLabelP384)];
    const usize bytes =
        pq_secret.size() + group_secret.size() + element.size() + group_public.size() + kem.label.size();
    cont::WireWriter writer(cont::Span<u8>(input, bytes));
    writer.put_bytes(pq_secret);
    writer.put_bytes(group_secret);
    writer.put_bytes(element);
    writer.put_bytes(group_public);
    writer.put_bytes(kem.label);
    const CryptoError error = sha3_256(cont::ConstSpan<u8>(input, bytes), shared_secret);
    secure_zero(input);
    return error;
}

// KeySchedule(mode_base, shared_secret, info, "", "") (section 5.1) up to its `secret` (Nh of the suite's KDF) and
// the key_schedule_context the context's keys expand with (`context`, 1 + 2 Nh bytes).
CryptoError schedule(const Kem& kem, const HpkeSuite& suite, cont::ConstSpan<u8> shared_secret,
                     cont::ConstSpan<u8> info, cont::Span<u8> context, cont::Span<u8> secret)
{
    const SuiteId id   = hpke_suite(kem, suite);
    const usize   hash = secret.size();
    context[0]         = kModeBase;
    CryptoError error  = labeled_extract(suite.kdf, id, {}, label_of("psk_id_hash"), {}, context.subspan(1U, hash));
    if (error == CryptoError::None)
    {
        error = labeled_extract(suite.kdf, id, {}, label_of("info_hash"), info, context.subspan(1U + hash, hash));
    }
    if (error == CryptoError::None)
    {
        error = labeled_extract(suite.kdf, id, shared_secret, label_of("secret"), {}, secret);
    }
    return error;
}

// The AEAD key and base nonce of the context's first message.
CryptoError key_schedule(const Kem& kem, const HpkeSuite& suite, cont::ConstSpan<u8> shared_secret,
                         cont::ConstSpan<u8> info, AeadKey& key, u8 (&nonce)[kAeadNonceBytes])
{
    const SuiteId        id   = hpke_suite(kem, suite);
    const usize          hash = hkdf_hash_bytes(suite.kdf);
    u8                   context_bytes[1U + 2U * kMaxHashBytes];
    u8                   secret_bytes[kMaxHashBytes];
    u8                   key_bytes[kMaxKeyBytes];
    const cont::Span<u8> context(context_bytes, 1U + 2U * hash);
    const cont::Span<u8> secret(secret_bytes, hash);
    const cont::Span<u8> aead_key(key_bytes, aead_key_bytes(suite.aead));
    CryptoError          error = schedule(kem, suite, shared_secret, info, context, secret);
    if (error == CryptoError::None)
    {
        error = labeled_expand(suite.kdf, id, secret, label_of("key"), context, aead_key);
    }
    if (error == CryptoError::None)
    {
        error = labeled_expand(suite.kdf, id, secret, label_of("base_nonce"), context, cont::Span<u8>(nonce));
    }
    if (error == CryptoError::None)
    {
        error = key.init(suite.aead, aead_key);
    }
    secure_zero(secret);
    secure_zero(aead_key);
    return error;
}

// The context's Export(exporter_context, L) (section 5.3): exporter_secret = LabeledExpand(secret, "exp",
// key_schedule_context, Nh), then LabeledExpand(exporter_secret, "sec", exporter_context, L) into `out`.
CryptoError export_from(const Kem& kem, const HpkeSuite& suite, cont::ConstSpan<u8> shared_secret,
                        cont::ConstSpan<u8> info, cont::ConstSpan<u8> exporter_context, cont::Span<u8> out)
{
    const SuiteId        id   = hpke_suite(kem, suite);
    const usize          hash = hkdf_hash_bytes(suite.kdf);
    u8                   context_bytes[1U + 2U * kMaxHashBytes];
    u8                   secret_bytes[kMaxHashBytes];
    u8                   exporter_bytes[kMaxHashBytes];
    const cont::Span<u8> context(context_bytes, 1U + 2U * hash);
    const cont::Span<u8> secret(secret_bytes, hash);
    const cont::Span<u8> exporter(exporter_bytes, hash);
    CryptoError          error = schedule(kem, suite, shared_secret, info, context, secret);
    if (error == CryptoError::None)
    {
        error = labeled_expand(suite.kdf, id, secret, label_of("exp"), context, exporter);
    }
    if (error == CryptoError::None)
    {
        error = labeled_expand(suite.kdf, id, exporter, label_of("sec"), exporter_context, out);
    }
    secure_zero(secret);
    secure_zero(exporter);
    return error;
}

// Encap(pkR) with fresh randomness: `enc` (Nenc) and the KEM's `shared_secret` (Nsecret). A DHKEM's ephemeral key
// (RFC 9180 section 4.1); ML-KEM.Encaps (draft-ietf-hpke-pq section 3); a hybrid's Encaps (draft-irtf-cfrg-hybrid-kems
// section 5.5): ML-KEM.Encaps to the post-quantum half, an ephemeral scalar from Group_T.Nseed random bytes, enc =
// ct_PQ || ct_T and the combined secret.
CryptoError encapsulate(const Kem& kem, cont::ConstSpan<u8> peer_public, cont::Span<u8> enc,
                        cont::Span<u8> shared_secret)
{
    if (kem.kind == KemKind::MlKem)
    {
        return kem_encapsulate(kem.pq, peer_public, enc, shared_secret);
    }
    const AgreementSizes group = agreement_sizes(kem.group);
    const KemSizes       pq    = kem_sizes(kem.pq);
    const bool           dhkem = kem.kind == KemKind::Dhkem;
    // A hybrid's traditional halves: of the encapsulation key ek_PQ || ek_T and of the encapsulation ct_PQ || ct_T.
    const cont::ConstSpan<u8> group_public = dhkem ? peer_public : peer_public.subspan(pq.public_key);
    const cont::Span<u8>      element      = dhkem ? enc : enc.subspan(pq.ciphertext);
    u8                        pq_bytes[kKemSecretBytes];
    u8                        seed_bytes[kMaxGroupSeedBytes];
    u8                        dh_bytes[kMaxSecretBytes];
    const cont::Span<u8>      pq_secret(pq_bytes, dhkem ? 0U : pq.secret);
    const cont::Span<u8>      seed(seed_bytes, kem.seed_t);
    const cont::Span<u8>      dh(dh_bytes, group.secret);
    AgreementKey              ephemeral;
    CryptoError               error = CryptoError::None;
    if (!dhkem)
    {
        error = kem_encapsulate(kem.pq, peer_public.first(pq.public_key), enc.first(pq.ciphertext), pq_secret);
    }
    if (error == CryptoError::None)
    {
        error = dhkem ? ephemeral.generate(kem.group) : random_bytes(seed);
    }
    if (error == CryptoError::None && !dhkem)
    {
        error = random_scalar(kem.group, seed, ephemeral);
    }
    if (error == CryptoError::None)
    {
        error = dhkem ? ephemeral.derive(group_public, dh) : exponentiate(ephemeral, group_public, dh);
    }
    if (error == CryptoError::None)
    {
        error = ephemeral.public_key(element);
    }
    if (error == CryptoError::None)
    {
        error = dhkem ? extract_and_expand(kem, dh, enc, peer_public, shared_secret)
                      : combine(kem, pq_secret, dh, element, group_public, shared_secret);
    }
    secure_zero(pq_secret);
    secure_zero(seed);
    secure_zero(dh);
    return error;
}

// The KEM entry of a suite whose every algorithm is in the profile; null otherwise.
const Kem* suite_kem(const HpkeSuite& suite) noexcept
{
    return kdf_id(suite.kdf) != 0U && aead_id(suite.aead) != 0U ? find_kem(suite.kem) : nullptr;
}

// Whether an export of `bytes` is one the suite's KDF can give: 1 to 255 * Nh bytes (section 5.3).
bool export_length(const HpkeSuite& suite, usize bytes) noexcept
{
    return bytes != 0U && bytes <= 255U * hkdf_hash_bytes(suite.kdf);
}
} // namespace

HpkeSizes hpke_sizes(AlgorithmId kem) noexcept
{
    const Kem* entry = find_kem(kem);
    return entry != nullptr ? sizes_of(*entry) : HpkeSizes{};
}

bool hpke_supported(const HpkeSuite& suite) noexcept
{
    return suite_kem(suite) != nullptr;
}

CryptoError hpke_check_public_key(AlgorithmId kem, cont::ConstSpan<u8> public_key) noexcept
{
    const Kem* entry = find_kem(kem);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (public_key.size() != sizes_of(*entry).public_key)
    {
        return CryptoError::BadLength;
    }
    switch (entry->kind)
    {
    case KemKind::Dhkem:
        return check_agreement_public_key(entry->group, public_key);
    case KemKind::MlKem:
        return check_kem_public_key(entry->pq, public_key);
    case KemKind::Hybrid:
        break;
    }
    const usize       pq    = kem_sizes(entry->pq).public_key;
    const CryptoError error = check_kem_public_key(entry->pq, public_key.first(pq));
    if (error != CryptoError::None || entry->group == AlgorithmId::X25519)
    {
        return error;
    }
    return check_agreement_public_key(entry->group, public_key.subspan(pq));
}

// A moved-from key holds none: its halves are moved out and its KEM forgotten.
HpkeKey::HpkeKey(HpkeKey&& other) noexcept
    : m_key(std::move(other.m_key)), m_pq(std::move(other.m_pq)), m_kem(other.m_kem)
{
    other.clear();
}

HpkeKey& HpkeKey::operator=(HpkeKey&& other) noexcept
{
    if (this != &other)
    {
        clear();
        m_key = std::move(other.m_key);
        m_pq  = std::move(other.m_pq);
        m_kem = other.m_kem;
        other.clear();
    }
    return *this;
}

void HpkeKey::clear() noexcept
{
    m_key.clear();
    m_pq.clear();
    m_kem = AlgorithmId{};
}

CryptoError HpkeKey::expand(AlgorithmId kem, cont::ConstSpan<u8> seed) noexcept
{
    const Kem* found = find_kem(kem);
    if (found == nullptr)
    {
        return CryptoError::Unsupported;
    }
    const Kem& entry = *found;
    if (entry.kind == KemKind::MlKem)
    {
        return m_pq.from_seed(entry.pq, seed);
    }
    // expandDecapsKeyG (draft-irtf-cfrg-hybrid-kems section 5.1.1): SHAKE256(seed) into seed_PQ || seed_T, the
    // ML-KEM key of seed_PQ and the scalar Group_T.RandomScalar(seed_T).
    u8                   full_bytes[kMlKemSeedBytes + kMaxGroupSeedBytes];
    const cont::Span<u8> full(full_bytes, kMlKemSeedBytes + entry.seed_t);
    CryptoError          error = shake256(seed, full);
    if (error == CryptoError::None)
    {
        error = m_pq.from_seed(entry.pq, full.first(kMlKemSeedBytes));
    }
    if (error == CryptoError::None)
    {
        error = random_scalar(entry.group, full.subspan(kMlKemSeedBytes), m_key);
    }
    secure_zero(full);
    return error;
}

CryptoError HpkeKey::generate(AlgorithmId kem) noexcept
{
    clear();
    const Kem* entry = find_kem(kem);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    CryptoError error = CryptoError::None;
    if (entry->kind == KemKind::Dhkem)
    {
        error = m_key.generate(entry->group);
    }
    else
    {
        u8                   seed_bytes[kMlKemSeedBytes];
        const cont::Span<u8> seed(seed_bytes, sizes_of(*entry).private_key);
        error = random_bytes(seed);
        if (error == CryptoError::None)
        {
            error = expand(kem, seed);
        }
        secure_zero(seed);
    }
    if (error != CryptoError::None)
    {
        clear();
        return error;
    }
    m_kem = kem;
    return CryptoError::None;
}

CryptoError HpkeKey::derive(AlgorithmId kem, cont::ConstSpan<u8> ikm) noexcept
{
    clear();
    const Kem* entry = find_kem(kem);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    const usize   private_bytes = sizes_of(*entry).private_key;
    const SuiteId suite         = kem_suite(*entry);
    if (ikm.size() < (entry->kind == KemKind::Dhkem ? private_bytes : kMinPostQuantumIkm))
    {
        return CryptoError::BadLength;
    }
    CryptoError error = CryptoError::None;
    if (entry->kind != KemKind::Dhkem)
    {
        // The seed SHAKE256.LabeledDerive(ikm, "DeriveKeyPair", "", Nsk) and the key it expands to
        // (draft-ietf-hpke-pq sections 3 and 4).
        u8                   seed_bytes[kMlKemSeedBytes];
        const cont::Span<u8> seed(seed_bytes, private_bytes);
        error = labeled_derive(suite, ikm, label_of("DeriveKeyPair"), seed);
        if (error == CryptoError::None)
        {
            error = expand(kem, seed);
        }
        secure_zero(seed);
    }
    else
    {
        u8             prk_bytes[kMaxHashBytes];
        u8             candidate_bytes[kMaxSecretBytes];
        cont::Span<u8> prk(prk_bytes, hkdf_hash_bytes(entry->kdf));
        cont::Span<u8> candidate(candidate_bytes, private_bytes);
        error = labeled_extract(entry->kdf, suite, {}, label_of("dkp_prk"), ikm, prk);
        if (error == CryptoError::None && !entry->scalar)
        {
            error = labeled_expand(entry->kdf, suite, prk, label_of("sk"), {}, candidate);
            if (error == CryptoError::None)
            {
                error = m_key.from_private(kem, candidate);
            }
        }
        else if (error == CryptoError::None)
        {
            // Rejection sampling: the first candidate in [1, n - 1] (bitmask 0xFF for P-256 and P-384, so no bit is
            // cleared). A candidate out of range is BadKey from the agreement key, and the next counter is tried; 256
            // failures in a row (probability about 2^-8192 for P-256) are DeriveKeyPairError, reported as BadKey.
            error = CryptoError::BadKey;
            for (u32 counter = 0U; counter <= 0xFFU && error == CryptoError::BadKey; ++counter)
            {
                const u8 info[1] = {static_cast<u8>(counter)};
                error            = labeled_expand(entry->kdf, suite, prk, label_of("candidate"), info, candidate);
                if (error == CryptoError::None)
                {
                    error = m_key.from_private(kem, candidate);
                }
            }
        }
        secure_zero(prk);
        secure_zero(candidate);
    }
    if (error != CryptoError::None)
    {
        clear();
        return error;
    }
    m_kem = kem;
    return CryptoError::None;
}

CryptoError HpkeKey::from_private(AlgorithmId kem, cont::ConstSpan<u8> private_key) noexcept
{
    clear();
    const Kem* entry = find_kem(kem);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    CryptoError error = CryptoError::None;
    if (entry->kind == KemKind::Dhkem)
    {
        error = m_key.from_private(kem, private_key);
    }
    else
    {
        error = private_key.size() == sizes_of(*entry).private_key ? expand(kem, private_key) : CryptoError::BadLength;
    }
    if (error != CryptoError::None)
    {
        clear();
        return error;
    }
    m_kem = kem;
    return CryptoError::None;
}

CryptoError HpkeKey::public_key(cont::Span<u8> out) const noexcept
{
    const Kem* entry = find_kem(m_kem);
    if (entry == nullptr)
    {
        return CryptoError::NotReady;
    }
    if (out.size() != sizes_of(*entry).public_key)
    {
        return CryptoError::BadLength;
    }
    switch (entry->kind)
    {
    case KemKind::Dhkem:
        return m_key.public_key(out);
    case KemKind::MlKem:
        return m_pq.public_key(out);
    case KemKind::Hybrid:
        break;
    }
    // ek_PQ || ek_T (draft-irtf-cfrg-hybrid-kems section 5.5's DeriveKeyPair).
    const usize       pq    = kem_sizes(entry->pq).public_key;
    const CryptoError error = m_pq.public_key(out.first(pq));
    return error == CryptoError::None ? m_key.public_key(out.subspan(pq)) : refuse(error, out);
}

// Decap(enc, skR) into `shared_secret` (Nsecret): a DHKEM's (RFC 9180 section 4.1), ML-KEM.Decaps (draft-ietf-hpke-pq
// section 3) or a hybrid's Decaps (draft-irtf-cfrg-hybrid-kems section 5.5: ML-KEM.Decaps of ct_PQ, the scalar times
// ct_T, refused when ct_T is no point of its NIST curve, and the combined secret).
CryptoError HpkeKey::decapsulate(cont::ConstSpan<u8> enc, cont::Span<u8> shared_secret) const noexcept
{
    const Kem* found = find_kem(m_kem);
    if (found == nullptr)
    {
        return refuse(CryptoError::NotReady, shared_secret);
    }
    const Kem& kem = *found;
    if (kem.kind == KemKind::MlKem)
    {
        return m_pq.decapsulate(enc, shared_secret);
    }
    const AgreementSizes      group   = agreement_sizes(kem.group);
    const KemSizes            pq      = kem_sizes(kem.pq);
    const bool                dhkem   = kem.kind == KemKind::Dhkem;
    const cont::ConstSpan<u8> element = dhkem ? enc : enc.subspan(pq.ciphertext);
    u8                        pq_bytes[kKemSecretBytes];
    u8                        dh_bytes[kMaxSecretBytes];
    u8                        recipient[kMaxElementBytes];
    const cont::Span<u8>      pq_secret(pq_bytes, dhkem ? 0U : pq.secret);
    const cont::Span<u8>      dh(dh_bytes, group.secret);
    const cont::Span<u8>      group_public(recipient, group.public_key);
    CryptoError               error = CryptoError::None;
    if (!dhkem)
    {
        error = m_pq.decapsulate(enc.first(pq.ciphertext), pq_secret);
    }
    if (error == CryptoError::None)
    {
        error = dhkem ? m_key.derive(element, dh) : exponentiate(m_key, element, dh);
    }
    if (error == CryptoError::None)
    {
        error = m_key.public_key(group_public);
    }
    if (error == CryptoError::None)
    {
        error = dhkem ? extract_and_expand(kem, dh, element, group_public, shared_secret)
                      : combine(kem, pq_secret, dh, element, group_public, shared_secret);
    }
    secure_zero(pq_secret);
    secure_zero(dh);
    return error;
}

CryptoError HpkeKey::open(const HpkeSuite& suite, cont::ConstSpan<u8> enc, cont::ConstSpan<u8> info,
                          cont::ConstSpan<u8> aad, cont::ConstSpan<u8> ciphertext, cont::Span<u8> out) const noexcept
{
    if (!ready())
    {
        return refuse(CryptoError::NotReady, out);
    }
    const Kem* kem = suite_kem(suite);
    if (kem == nullptr || suite.kem != m_kem)
    {
        return refuse(CryptoError::Unsupported, out);
    }
    if (enc.size() != sizes_of(*kem).encapsulation || ciphertext.size() < kHpkeTagBytes ||
        out.size() != ciphertext.size() - kHpkeTagBytes)
    {
        return refuse(CryptoError::BadLength, out);
    }
    if (overlaps(out, enc) || overlaps(out, info) || overlaps(out, aad) || overlaps(out, ciphertext))
    {
        return CryptoError::Overlap;
    }
    u8             shared_bytes[kMaxHashBytes];
    cont::Span<u8> shared(shared_bytes, secret_bytes(*kem));
    CryptoError    error = decapsulate(enc, shared);
    AeadKey        key;
    u8             nonce[kAeadNonceBytes] = {};
    if (error == CryptoError::None)
    {
        error = key_schedule(*kem, suite, shared, info, key, nonce);
    }
    if (error == CryptoError::None)
    {
        error = key.open(nonce, ciphertext, aad, out);
    }
    secure_zero(shared);
    secure_zero(nonce);
    return error == CryptoError::None ? error : refuse(error, out);
}

CryptoError HpkeKey::export_secret(const HpkeSuite& suite, cont::ConstSpan<u8> enc, cont::ConstSpan<u8> info,
                                   cont::ConstSpan<u8> exporter_context, cont::Span<u8> out) const noexcept
{
    if (!ready())
    {
        return refuse(CryptoError::NotReady, out);
    }
    const Kem* kem = suite_kem(suite);
    if (kem == nullptr || suite.kem != m_kem)
    {
        return refuse(CryptoError::Unsupported, out);
    }
    if (enc.size() != sizes_of(*kem).encapsulation || !export_length(suite, out.size()))
    {
        return refuse(CryptoError::BadLength, out);
    }
    if (overlaps(out, enc) || overlaps(out, info) || overlaps(out, exporter_context))
    {
        return CryptoError::Overlap;
    }
    u8             shared_bytes[kMaxHashBytes];
    cont::Span<u8> shared(shared_bytes, secret_bytes(*kem));
    CryptoError    error = decapsulate(enc, shared);
    if (error == CryptoError::None)
    {
        error = export_from(*kem, suite, shared, info, exporter_context, out);
    }
    secure_zero(shared);
    return error == CryptoError::None ? error : refuse(error, out);
}

CryptoError hpke_seal(const HpkeSuite& suite, cont::ConstSpan<u8> peer_public, cont::ConstSpan<u8> info,
                      cont::ConstSpan<u8> aad, cont::ConstSpan<u8> plaintext, cont::Span<u8> enc,
                      cont::Span<u8> ciphertext) noexcept
{
    const Kem* kem = suite_kem(suite);
    if (kem == nullptr)
    {
        return refuse(CryptoError::Unsupported, enc, ciphertext);
    }
    const HpkeSizes sizes = sizes_of(*kem);
    if (peer_public.size() != sizes.public_key || enc.size() != sizes.encapsulation ||
        ciphertext.size() != plaintext.size() + kHpkeTagBytes)
    {
        return refuse(CryptoError::BadLength, enc, ciphertext);
    }
    for (const cont::ConstSpan<u8> input : {peer_public, info, aad, plaintext})
    {
        if (overlaps(enc, input) || overlaps(ciphertext, input))
        {
            return CryptoError::Overlap;
        }
    }
    if (overlaps(enc, ciphertext))
    {
        return CryptoError::Overlap;
    }
    u8             shared_bytes[kMaxHashBytes];
    cont::Span<u8> shared(shared_bytes, secret_bytes(*kem));
    CryptoError    error = encapsulate(*kem, peer_public, enc, shared);
    AeadKey        key;
    u8             nonce[kAeadNonceBytes] = {};
    if (error == CryptoError::None)
    {
        error = key_schedule(*kem, suite, shared, info, key, nonce);
    }
    if (error == CryptoError::None)
    {
        error = key.seal(nonce, plaintext, aad, ciphertext);
    }
    secure_zero(shared);
    secure_zero(nonce);
    return error == CryptoError::None ? error : refuse(error, enc, ciphertext);
}

CryptoError hpke_export(const HpkeSuite& suite, cont::ConstSpan<u8> peer_public, cont::ConstSpan<u8> info,
                        cont::ConstSpan<u8> exporter_context, cont::Span<u8> enc, cont::Span<u8> out) noexcept
{
    const Kem* kem = suite_kem(suite);
    if (kem == nullptr)
    {
        return refuse(CryptoError::Unsupported, enc, out);
    }
    const HpkeSizes sizes = sizes_of(*kem);
    if (peer_public.size() != sizes.public_key || enc.size() != sizes.encapsulation ||
        !export_length(suite, out.size()))
    {
        return refuse(CryptoError::BadLength, enc, out);
    }
    for (const cont::ConstSpan<u8> input : {peer_public, info, exporter_context})
    {
        if (overlaps(enc, input) || overlaps(out, input))
        {
            return CryptoError::Overlap;
        }
    }
    if (overlaps(enc, out))
    {
        return CryptoError::Overlap;
    }
    u8             shared_bytes[kMaxHashBytes];
    cont::Span<u8> shared(shared_bytes, secret_bytes(*kem));
    CryptoError    error = encapsulate(*kem, peer_public, enc, shared);
    if (error == CryptoError::None)
    {
        error = export_from(*kem, suite, shared, info, exporter_context, out);
    }
    secure_zero(shared);
    return error == CryptoError::None ? error : refuse(error, enc, out);
}
} // namespace crd::crypto
