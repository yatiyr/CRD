// crd-crypto -- hybrid public key encryption, RFC 9180 base mode (NET.2; contract:
// docs/design/network-foundation.md#hpke).
//
// RFC 9180's construction, step by step, over the provider interface: DHKEM (section 4.1) over AgreementKey, its
// DeriveKeyPair (section 7.1.3), the key schedule (section 5.1) over HKDF, the first message of a context (section
// 5.2) over AeadKey and the context's exporter (section 5.3). Every secret this unit computes lives on the stack of
// one call and is zeroed before it returns.
#include <crd/crypto/hpke.hpp>

#include "byte_spans.hpp"

#include <crd/containers/wire_bytes.hpp>

#include <cstring>

namespace crd::crypto
{
namespace
{
using detail::label_of;
using detail::LabelBuffer;
using detail::overlaps;
using detail::refuse;

constexpr u8    kVersion[]      = {'H', 'P', 'K', 'E', '-', 'v', '1'};
constexpr usize kMaxHashBytes   = 48U; // Nh of HKDF-SHA384
constexpr usize kMaxPublicBytes = 97U; // Npk of P-384
constexpr usize kMaxSecretBytes = 48U; // Ndh and Nsk of P-384
constexpr usize kMaxKeyBytes    = 32U; // Nk of AES-256-GCM and ChaCha20-Poly1305
constexpr u8    kModeBase       = 0x00U;

struct Dhkem
{
    AlgorithmId group;
    u16         id;
    AlgorithmId kdf;    // the DHKEM's own KDF, whose Nh is Nsecret
    bool        scalar; // DeriveKeyPair samples a scalar below the group order (the NIST curves)
};

constexpr Dhkem kDhkems[] = {
    {AlgorithmId::X25519, iana::kHpkeKemDhkemX25519HkdfSha256, AlgorithmId::HkdfSha256, false},
    {AlgorithmId::Secp256r1, iana::kHpkeKemDhkemP256HkdfSha256, AlgorithmId::HkdfSha256, true},
    {AlgorithmId::Secp384r1, iana::kHpkeKemDhkemP384HkdfSha384, AlgorithmId::HkdfSha384, true},
};

const Dhkem* find_kem(AlgorithmId group) noexcept
{
    for (const Dhkem& kem : kDhkems)
    {
        if (kem.group == group)
        {
            return &kem;
        }
    }
    return nullptr;
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

SuiteId kem_suite(const Dhkem& kem) noexcept
{
    SuiteId          id;
    cont::WireWriter out(cont::Span<u8>(id.bytes, 5U));
    out.put_bytes(label_of("KEM"));
    out.put_u16(kem.id);
    id.size = out.written();
    return id;
}

SuiteId hpke_suite(const Dhkem& kem, const HpkeSuite& suite) noexcept
{
    SuiteId          id;
    cont::WireWriter out(cont::Span<u8>(id.bytes));
    out.put_bytes(label_of("HPKE"));
    out.put_u16(kem.id);
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

// ExtractAndExpand(dh, enc || pkRm) into `shared_secret` (Nsecret, the DHKEM KDF's Nh).
CryptoError extract_and_expand(const Dhkem& kem, cont::ConstSpan<u8> dh, cont::ConstSpan<u8> enc,
                               cont::ConstSpan<u8> recipient, cont::Span<u8> shared_secret)
{
    u8 context[2U * kMaxPublicBytes];
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

// KeySchedule(mode_base, shared_secret, info, "", "") (section 5.1) up to its `secret` (Nh of the suite's KDF) and
// the key_schedule_context the context's keys expand with (`context`, 1 + 2 Nh bytes).
CryptoError schedule(const Dhkem& kem, const HpkeSuite& suite, cont::ConstSpan<u8> shared_secret,
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
CryptoError key_schedule(const Dhkem& kem, const HpkeSuite& suite, cont::ConstSpan<u8> shared_secret,
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
CryptoError export_from(const Dhkem& kem, const HpkeSuite& suite, cont::ConstSpan<u8> shared_secret,
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

// Encap(pkR) (section 4.1) with a fresh ephemeral key: `enc` its public key, `shared_secret` the KEM's.
CryptoError encapsulate(const Dhkem& kem, cont::ConstSpan<u8> peer_public, cont::Span<u8> enc,
                        cont::Span<u8> shared_secret)
{
    AgreementKey   ephemeral;
    u8             dh_bytes[kMaxSecretBytes];
    cont::Span<u8> dh(dh_bytes, agreement_sizes(kem.group).secret);
    CryptoError    error = ephemeral.generate(kem.group);
    if (error == CryptoError::None)
    {
        error = ephemeral.derive(peer_public, dh);
    }
    if (error == CryptoError::None)
    {
        error = ephemeral.public_key(enc);
    }
    if (error == CryptoError::None)
    {
        error = extract_and_expand(kem, dh, enc, peer_public, shared_secret);
    }
    secure_zero(dh);
    return error;
}

// Decap(enc, skR) (section 4.1) under the recipient's `key`.
CryptoError decapsulate(const Dhkem& kem, const AgreementKey& key, cont::ConstSpan<u8> enc,
                        cont::Span<u8> shared_secret)
{
    const AgreementSizes sizes = agreement_sizes(kem.group);
    u8                   dh_bytes[kMaxSecretBytes];
    u8                   recipient[kMaxPublicBytes];
    cont::Span<u8>       dh(dh_bytes, sizes.secret);
    CryptoError          error = key.derive(enc, dh);
    if (error == CryptoError::None)
    {
        error = key.public_key(cont::Span<u8>(recipient, sizes.public_key));
    }
    if (error == CryptoError::None)
    {
        error = extract_and_expand(kem, dh, enc, cont::ConstSpan<u8>(recipient, sizes.public_key), shared_secret);
    }
    secure_zero(dh);
    return error;
}

// Whether an export of `bytes` is one the suite's KDF can give: 1 to 255 * Nh bytes (section 5.3).
bool export_length(const HpkeSuite& suite, usize bytes) noexcept
{
    return bytes != 0U && bytes <= 255U * hkdf_hash_bytes(suite.kdf);
}
} // namespace

HpkeSizes hpke_sizes(AlgorithmId kem) noexcept
{
    if (find_kem(kem) == nullptr)
    {
        return {};
    }
    const AgreementSizes sizes = agreement_sizes(kem);
    return {sizes.public_key, sizes.private_key};
}

bool hpke_supported(const HpkeSuite& suite) noexcept
{
    return find_kem(suite.kem) != nullptr && kdf_id(suite.kdf) != 0U && aead_id(suite.aead) != 0U;
}

CryptoError HpkeKey::generate(AlgorithmId kem) noexcept
{
    clear();
    return find_kem(kem) != nullptr ? m_key.generate(kem) : CryptoError::Unsupported;
}

CryptoError HpkeKey::derive(AlgorithmId kem, cont::ConstSpan<u8> ikm) noexcept
{
    clear();
    const Dhkem* entry = find_kem(kem);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    const usize private_bytes = agreement_sizes(kem).private_key;
    if (ikm.size() < private_bytes)
    {
        return CryptoError::BadLength;
    }
    const SuiteId  suite = kem_suite(*entry);
    u8             prk_bytes[kMaxHashBytes];
    u8             candidate_bytes[kMaxSecretBytes];
    cont::Span<u8> prk(prk_bytes, hkdf_hash_bytes(entry->kdf));
    cont::Span<u8> candidate(candidate_bytes, private_bytes);
    CryptoError    error = labeled_extract(entry->kdf, suite, {}, label_of("dkp_prk"), ikm, prk);
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
    if (error != CryptoError::None)
    {
        clear();
    }
    return error;
}

CryptoError HpkeKey::from_private(AlgorithmId kem, cont::ConstSpan<u8> private_key) noexcept
{
    clear();
    return find_kem(kem) != nullptr ? m_key.from_private(kem, private_key) : CryptoError::Unsupported;
}

CryptoError HpkeKey::public_key(cont::Span<u8> out) const noexcept
{
    return m_key.public_key(out);
}

CryptoError HpkeKey::open(const HpkeSuite& suite, cont::ConstSpan<u8> enc, cont::ConstSpan<u8> info,
                          cont::ConstSpan<u8> aad, cont::ConstSpan<u8> ciphertext, cont::Span<u8> out) const noexcept
{
    if (!ready())
    {
        return refuse(CryptoError::NotReady, out);
    }
    const Dhkem* kem = find_kem(suite.kem);
    if (kem == nullptr || suite.kem != m_key.algorithm() || !hpke_supported(suite))
    {
        return refuse(CryptoError::Unsupported, out);
    }
    if (enc.size() != agreement_sizes(suite.kem).public_key || ciphertext.size() < kHpkeTagBytes ||
        out.size() != ciphertext.size() - kHpkeTagBytes)
    {
        return refuse(CryptoError::BadLength, out);
    }
    if (overlaps(out, enc) || overlaps(out, info) || overlaps(out, aad) || overlaps(out, ciphertext))
    {
        return CryptoError::Overlap;
    }
    u8             shared_bytes[kMaxHashBytes];
    cont::Span<u8> shared(shared_bytes, hkdf_hash_bytes(kem->kdf));
    CryptoError    error = decapsulate(*kem, m_key, enc, shared);
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
    const Dhkem* kem = find_kem(suite.kem);
    if (kem == nullptr || suite.kem != m_key.algorithm() || !hpke_supported(suite))
    {
        return refuse(CryptoError::Unsupported, out);
    }
    if (enc.size() != agreement_sizes(suite.kem).public_key || !export_length(suite, out.size()))
    {
        return refuse(CryptoError::BadLength, out);
    }
    if (overlaps(out, enc) || overlaps(out, info) || overlaps(out, exporter_context))
    {
        return CryptoError::Overlap;
    }
    u8             shared_bytes[kMaxHashBytes];
    cont::Span<u8> shared(shared_bytes, hkdf_hash_bytes(kem->kdf));
    CryptoError    error = decapsulate(*kem, m_key, enc, shared);
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
    const Dhkem* kem = find_kem(suite.kem);
    if (kem == nullptr || !hpke_supported(suite))
    {
        return refuse(CryptoError::Unsupported, enc, ciphertext);
    }
    const usize npk = agreement_sizes(suite.kem).public_key;
    if (peer_public.size() != npk || enc.size() != npk || ciphertext.size() != plaintext.size() + kHpkeTagBytes)
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
    cont::Span<u8> shared(shared_bytes, hkdf_hash_bytes(kem->kdf));
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
    const Dhkem* kem = find_kem(suite.kem);
    if (kem == nullptr || !hpke_supported(suite))
    {
        return refuse(CryptoError::Unsupported, enc, out);
    }
    const usize npk = agreement_sizes(suite.kem).public_key;
    if (peer_public.size() != npk || enc.size() != npk || !export_length(suite, out.size()))
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
    cont::Span<u8> shared(shared_bytes, hkdf_hash_bytes(kem->kdf));
    CryptoError    error = encapsulate(*kem, peer_public, enc, shared);
    if (error == CryptoError::None)
    {
        error = export_from(*kem, suite, shared, info, exporter_context, out);
    }
    secure_zero(shared);
    return error == CryptoError::None ? error : refuse(error, enc, out);
}
} // namespace crd::crypto
