// crd-fuzz-crypto-key-store: the key store of crd-crypto (NET.2; docs/design/network-foundation.md#key-store) under
// the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a store body: sealed by the harness under a protector whose master key it knows (the store's header and
//      sealing restated from decision K3), written as the store file and opened;
//   1  a store payload as bytes, written as the store file and opened;
//   2  a sequence of changes (generate, import, rotate, rotate_import, revoke, destroy, reopen) read from the bytes and
//      applied to one store.
//
// Oracles, none of them the store's own verdict:
//   * a restatement of decision K3's body rules (order, generations, kinds, lengths, identifiers, a held private input
//     its public key comes from) decides whether a sealed body opens or is Malformed, and an opened store lists
//     exactly the restated entries, then takes a new key and reopens with it and every one of them;
//   * a payload the harness did not seal never opens;
//   * a model of decisions K4 and K5 decides every change's exact result and the store's listing after it, and the
//     listing survives reopening.
#include <crd/fuzz/harness.hpp>

#include <crd/crypto/key_store.hpp>
#include <crd/platform/durable_file.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstdint>
#include <cstring>
#include <string>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::String;
using crd::containers::StringView;
using namespace crd::crypto;
namespace fs = crd::platform::fs;

using Bytes = Array<u8>;

constexpr u8 kStoreId[kKeyStoreIdBytes] = {0xC0, 0xDE, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
constexpr u8 kReference[2]              = {'r', 'f'};

// The harness's protector: one fixed master key behind the reference "rf" for any store, so the harness can seal a
// store itself; every other reference is Denied.
class FixedProtector final : public KeyProtector
{
public:
    [[nodiscard]] StringView    name() const noexcept override { return "fuzz"; }
    [[nodiscard]] KeyProtection protection() const noexcept override { return KeyProtection::Application; }
    [[nodiscard]] bool          forgets() const noexcept override { return false; }

    [[nodiscard]] CryptoError protect(ConstSpan<u8>, ConstSpan<u8> master, Bytes& reference) noexcept override
    {
        // The store's own master keys are kept too, so its saves after the harness's reopen.
        if (master.size() != kKeyStoreMasterBytes)
        {
            return CryptoError::BadLength;
        }
        std::memcpy(m_master, master.data(), kKeyStoreMasterBytes);
        reference.clear();
        reference.push_back('s');
        return CryptoError::None;
    }

    [[nodiscard]] CryptoError recover(ConstSpan<u8>, ConstSpan<u8> reference, Span<u8> master) noexcept override
    {
        if (reference.size() == 2U && std::memcmp(reference.data(), kReference, 2U) == 0)
        {
            std::memcpy(master.data(), fixed(), kKeyStoreMasterBytes);
            return CryptoError::None;
        }
        if (reference.size() == 1U && reference[0] == 's')
        {
            std::memcpy(master.data(), m_master, kKeyStoreMasterBytes);
            return CryptoError::None;
        }
        secure_zero(master);
        return CryptoError::Denied;
    }

    [[nodiscard]] CryptoError forget(ConstSpan<u8>, ConstSpan<u8>) noexcept override { return CryptoError::None; }

    [[nodiscard]] static const u8* fixed() noexcept
    {
        static const u8 kMaster[kKeyStoreMasterBytes] = {0x6B, 0x65, 0x79, 0x20, 0x73, 0x74, 0x6F, 0x72,
                                                         0x65, 0x20, 0x66, 0x75, 0x7A, 0x7A, 0x20, 0x6D,
                                                         0x61, 0x73, 0x74, 0x65, 0x72, 0x20, 0x6B, 0x65,
                                                         0x79, 0x20, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35};
        return kMaster;
    }

private:
    u8 m_master[kKeyStoreMasterBytes] = {};
};

// The store directory of this process (named by the address of a static, which differs between processes that run
// at once under address-space randomisation).
[[nodiscard]] fs::Path make_store_dir()
{
    static const char kTag = 0;
    String            leaf("crd_fuzz_key_store_");
    leaf.append(std::to_string(reinterpret_cast<std::uintptr_t>(&kTag)));
    const fs::Path path = fs::temp_directory() / StringView(leaf.data(), leaf.size());
    (void)fs::remove_all(path);
    CRD_FUZZ_REQUIRE(fs::create_private_directory(path));
    return path;
}

[[nodiscard]] const fs::Path& store_dir()
{
    static const fs::Path kDir = make_store_dir();
    return kDir;
}

void reset_store()
{
    (void)fs::remove_file(store_dir() / "keys");
}

void put(Bytes& out, u64 value, usize width)
{
    for (usize i = 0U; i < width; ++i)
    {
        out.push_back(static_cast<u8>(value >> (8U * i)));
    }
}

void put(Bytes& out, ConstSpan<u8> bytes)
{
    for (const u8 b : bytes)
    {
        out.push_back(b);
    }
}

// Decision K3, restated: the header, then the body sealed with AES-256-GCM under HKDF-SHA256 of the master key salted
// with the header's salt, expanded under "cerid key store 1" into the key, then the nonce.
[[nodiscard]] Bytes seal_store(ConstSpan<u8> body)
{
    Bytes payload;
    put(payload, ConstSpan<u8>(reinterpret_cast<const u8*>("CRDKEYST"), 8U));
    put(payload, 1U, 2U);
    put(payload, 0U, 2U);
    put(payload, ConstSpan<u8>(kStoreId));
    put(payload, 1U, 8U);
    put(payload, 4U, 1U);
    put(payload, ConstSpan<u8>(reinterpret_cast<const u8*>("fuzz"), 4U));
    put(payload, 2U, 2U);
    put(payload, ConstSpan<u8>(kReference));
    put(payload, 0U, 2U);
    u8 salt[32];
    for (usize i = 0U; i < sizeof(salt); ++i)
    {
        salt[i] = static_cast<u8>(0x5AU ^ i);
    }
    put(payload, ConstSpan<u8>(salt));
    const usize header = payload.size();

    u8         prk[32];
    u8         okm[44];
    const auto label = ConstSpan<u8>(reinterpret_cast<const u8*>("cerid key store 1"), 17U);
    CRD_FUZZ_REQUIRE(hkdf_extract(AlgorithmId::HkdfSha256, ConstSpan<u8>(salt),
                                  ConstSpan<u8>(FixedProtector::fixed(), kKeyStoreMasterBytes),
                                  Span<u8>(prk)) == CryptoError::None);
    CRD_FUZZ_REQUIRE(hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk), label, Span<u8>(okm)) ==
                     CryptoError::None);
    AeadKey aead;
    CRD_FUZZ_REQUIRE(aead.init(AlgorithmId::Aes256Gcm, ConstSpan<u8>(okm, 32U)) == CryptoError::None);
    payload.resize(header + body.size() + kAeadTagBytes);
    if (!body.empty())
    {
        std::memcpy(payload.data() + header, body.data(), body.size());
    }
    CRD_FUZZ_REQUIRE(aead.seal(ConstSpan<u8>(okm + 32U, kAeadNonceBytes),
                               ConstSpan<u8>(payload.data() + header, body.size()),
                               ConstSpan<u8>(payload.data(), header),
                               Span<u8>(payload.data() + header, body.size() + kAeadTagBytes)) == CryptoError::None);
    return payload;
}

void write_store(ConstSpan<u8> payload)
{
    reset_store();
    CRD_FUZZ_REQUIRE(fs::save_generation(store_dir() / "keys", payload, 0U).ok);
}

// ---- The restated body ------------------------------------------------------------------------------------------

struct Restated
{
    char        name[kKeyNameMaxBytes] = {};
    u8          name_bytes             = 0U;
    u32         generation             = 0U;
    AlgorithmId algorithm{};
    u8          kind      = 0U;
    u8          state     = 0U;
    u64         created   = 0U;
    u64         changed   = 0U;
    KeyId       id;

    [[nodiscard]] StringView text() const { return StringView(name, name_bytes); }
};

[[nodiscard]] bool restated_name(ConstSpan<u8> name)
{
    if (name.empty() || name.size() > 64U)
    {
        return false;
    }
    for (usize i = 0U; i < name.size(); ++i)
    {
        const u8   c        = name[i];
        const bool alnum    = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        const bool trailing = c == '.' || c == '_' || c == '-';
        if (!alnum && (i == 0U || !trailing))
        {
            return false;
        }
    }
    return true;
}

// The kind byte of an algorithm (0 signature, 1 agreement, 2 KEM, 3 secret) and its private and public lengths;
// false for one the store does not keep.
[[nodiscard]] bool restated_kind(AlgorithmId algorithm, u8& kind, usize& private_length, usize& public_length)
{
    if (signature_sizes(algorithm).seed != 0U)
    {
        kind           = 0U;
        private_length = signature_sizes(algorithm).seed;
        public_length  = signature_sizes(algorithm).public_key;
        return true;
    }
    if (agreement_sizes(algorithm).private_key != 0U)
    {
        kind           = 1U;
        private_length = agreement_sizes(algorithm).private_key;
        public_length  = agreement_sizes(algorithm).public_key;
        return true;
    }
    if (kem_sizes(algorithm).seed != 0U)
    {
        kind           = 2U;
        private_length = kem_sizes(algorithm).seed;
        public_length  = kem_sizes(algorithm).public_key;
        return true;
    }
    usize secret = aead_key_bytes(algorithm);
    secret       = secret != 0U ? secret : hkdf_hash_bytes(algorithm);
    if (secret == 0U && algorithm == AlgorithmId::Siphash24)
    {
        secret = kPrfKeyBytes;
    }
    kind           = 3U;
    private_length = secret;
    public_length  = 0U;
    return secret != 0U;
}

// The public key and identifier a private input of `kind` gives; false when the provider refuses the input.
[[nodiscard]] bool restated_public(u8 kind, AlgorithmId algorithm, ConstSpan<u8> priv, Bytes& pub, KeyId& id)
{
    CryptoError error = CryptoError::None;
    if (kind == 0U)
    {
        SigningKey key;
        error = key.from_seed(algorithm, priv);
        error = error == CryptoError::None ? key.public_key(Span<u8>(pub.data(), pub.size())) : error;
    }
    else if (kind == 1U)
    {
        AgreementKey key;
        error = key.from_private(algorithm, priv);
        error = error == CryptoError::None ? key.public_key(Span<u8>(pub.data(), pub.size())) : error;
    }
    else if (kind == 2U)
    {
        KemKey key;
        error = key.from_seed(algorithm, priv);
        error = error == CryptoError::None ? key.public_key(Span<u8>(pub.data(), pub.size())) : error;
    }
    else
    {
        u8         prk[32];
        const auto label = ConstSpan<u8>(reinterpret_cast<const u8*>("cerid key id"), 12U);
        CRD_FUZZ_REQUIRE(hkdf_extract(AlgorithmId::HkdfSha256, {}, priv, Span<u8>(prk)) == CryptoError::None);
        CRD_FUZZ_REQUIRE(hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk), label, Span<u8>(id.bytes)) ==
                         CryptoError::None);
        return true;
    }
    if (error != CryptoError::None)
    {
        return false;
    }
    id = key_id_of(ConstSpan<u8>(pub.data(), pub.size()));
    return true;
}

class Cursor
{
public:
    Cursor(const u8* data, usize size) : m_data(data), m_size(size) {}

    [[nodiscard]] ConstSpan<u8> take(usize n)
    {
        if (m_size - m_at < n)
        {
            m_ok = false;
            return {};
        }
        const ConstSpan<u8> out(m_data + m_at, n);
        m_at += n;
        return out;
    }

    [[nodiscard]] u64 le(usize width)
    {
        const ConstSpan<u8> bytes = take(width);
        u64                 v     = 0U;
        for (usize i = 0U; i < bytes.size(); ++i)
        {
            v |= static_cast<u64>(bytes[i]) << (8U * i);
        }
        return v;
    }

    [[nodiscard]] u8 byte() { return m_at < m_size ? m_data[m_at++] : 0U; }

    [[nodiscard]] bool  ok() const { return m_ok; }
    [[nodiscard]] usize left() const { return m_size - m_at; }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
    bool      m_ok = true;
};

// Whether `body` keeps decision K3's and K4's rules; its entries in `out`.
[[nodiscard]] bool restate_body(ConstSpan<u8> body, Array<Restated>& out)
{
    Cursor    in(body.data(), body.size());
    const u64 count = in.le(4U);
    if (!in.ok() || count > kKeyStoreMaxKeys)
    {
        return false;
    }
    Bytes derived;
    for (u64 i = 0U; i < count; ++i)
    {
        Restated            r;
        const ConstSpan<u8> name = in.take(static_cast<usize>(in.le(1U)));
        r.generation             = static_cast<u32>(in.le(4U));
        r.algorithm              = static_cast<AlgorithmId>(in.le(2U));
        r.kind                   = static_cast<u8>(in.le(1U));
        r.state                  = static_cast<u8>(in.le(1U));
        const u64 placement      = in.le(1U);
        r.created                = in.le(8U);
        r.changed                = in.le(8U);
        const ConstSpan<u8> id   = in.take(kKeyIdBytes);
        const ConstSpan<u8> pub  = in.take(static_cast<usize>(in.le(2U)));
        const ConstSpan<u8> priv = in.take(static_cast<usize>(in.le(1U)));
        u8                  kind = 0U;
        usize               private_length = 0U;
        usize               public_length  = 0U;
        if (!in.ok() || !restated_name(name) || !restated_kind(r.algorithm, kind, private_length, public_length) ||
            r.kind != kind || r.state > 2U || placement != 0U || r.generation == 0U)
        {
            return false;
        }
        std::memcpy(r.name, name.data(), name.size());
        r.name_bytes = static_cast<u8>(name.size());
        std::memcpy(r.id.bytes, id.data(), kKeyIdBytes);
        if (!out.empty() && out.back().text() == r.text())
        {
            const Restated& p = out.back();
            if (r.generation != p.generation + 1U || r.algorithm != p.algorithm || p.state == 0U)
            {
                return false;
            }
        }
        else if (r.generation != 1U || (!out.empty() && !(out.back().text() < r.text())))
        {
            return false;
        }
        const bool revoked = r.state == 2U;
        if (pub.size() != public_length || priv.size() != (revoked ? 0U : private_length))
        {
            return false;
        }
        KeyId named;
        if (kind != 3U)
        {
            named = key_id_of(pub);
        }
        if (!revoked)
        {
            derived.resize(public_length);
            KeyId from_private;
            if (!restated_public(kind, r.algorithm, priv, derived, from_private) || !(from_private == r.id) ||
                (public_length != 0U && std::memcmp(derived.data(), pub.data(), public_length) != 0))
            {
                return false;
            }
            named = from_private;
        }
        if (!(named == r.id) && (kind != 3U || !revoked))
        {
            return false;
        }
        out.push_back(r);
    }
    return in.left() == 0U;
}

void check_listing(const KeyStore& store, const Array<Restated>& expected)
{
    CRD_FUZZ_REQUIRE(store.size() == expected.size());
    for (usize i = 0U; i < expected.size(); ++i)
    {
        const KeyInfo&  info = store.key(i);
        const Restated& r    = expected[i];
        CRD_FUZZ_REQUIRE(info.name() == r.text() && info.generation == r.generation && info.algorithm == r.algorithm &&
                         static_cast<u8>(info.kind) == r.kind && static_cast<u8>(info.state) == r.state &&
                         info.placement == KeyPlacement::Software && info.created == r.created &&
                         info.changed == r.changed && info.id == r.id);
    }
}

void sealed_body(const u8* data, usize size)
{
    const ConstSpan<u8> body(data, size);
    const Bytes         payload = seal_store(body);
    write_store(ConstSpan<u8>(payload.data(), payload.size()));
    Array<Restated>   expected;
    const bool        valid = restate_body(body, expected);
    FixedProtector    protector;
    KeyStore          store;
    const CryptoError error = store.open(store_dir().generic(), protector);
    CRD_FUZZ_REQUIRE(error == (valid ? CryptoError::None : CryptoError::Malformed));
    if (!valid)
    {
        return;
    }
    check_listing(store, expected);

    // A change writes the store back: every entry survives it, beside the new key.
    const StringView added = "zzzz.fuzz";
    if (store.find(added, 1U) == nullptr && expected.size() < kKeyStoreMaxKeys)
    {
        CRD_FUZZ_REQUIRE(store.generate(added, AlgorithmId::Ed25519, PlacementRequest::Software, 7U) ==
                         CryptoError::None);
        KeyStore reopened;
        CRD_FUZZ_REQUIRE(reopened.open(store_dir().generic(), protector) == CryptoError::None);
        CRD_FUZZ_REQUIRE(reopened.size() == expected.size() + 1U);
        for (usize i = 0U; i < reopened.size(); ++i)
        {
            const KeyInfo& info = reopened.key(i);
            if (info.name() != added)
            {
                const KeyInfo* before = store.find(info.name(), info.generation);
                CRD_FUZZ_REQUIRE(before != nullptr && before->id == info.id && before->state == info.state);
            }
        }
    }
}

// Whether `payload` frames a store of the harness's protector whose body opens under the master key its reference
// names ("rf": the fixed key; "s": the key a fresh protector holds, all zero) and keeps the body's rules.
[[nodiscard]] bool restate_payload(ConstSpan<u8> payload, Array<Restated>& entries)
{
    Cursor              in(payload.data(), payload.size());
    const ConstSpan<u8> magic     = in.take(8U);
    const u64           format    = in.le(2U);
    const u64           flags     = in.le(2U);
    (void)in.take(kKeyStoreIdBytes);
    const u64           sequence  = in.le(8U);
    const ConstSpan<u8> protector = in.take(static_cast<usize>(in.le(1U)));
    const ConstSpan<u8> reference = in.take(static_cast<usize>(in.le(2U)));
    const u64           pending   = in.le(2U);
    (void)in.take(static_cast<usize>(pending));
    const ConstSpan<u8> salt   = in.take(32U);
    const usize         header = payload.size() - in.left();
    if (!in.ok() || std::memcmp(magic.data(), "CRDKEYST", 8U) != 0 || format != 1U || flags != 0U || sequence == 0U ||
        protector.size() != 4U || std::memcmp(protector.data(), "fuzz", 4U) != 0 || pending > kKeyReferenceMaxBytes ||
        in.left() < kAeadTagBytes)
    {
        return false;
    }
    u8 master[kKeyStoreMasterBytes] = {};
    if (reference.size() == 2U && std::memcmp(reference.data(), kReference, 2U) == 0)
    {
        std::memcpy(master, FixedProtector::fixed(), kKeyStoreMasterBytes);
    }
    else if (reference.size() != 1U || reference[0] != 's')
    {
        return false;
    }
    u8         prk[32];
    u8         okm[44];
    const auto label = ConstSpan<u8>(reinterpret_cast<const u8*>("cerid key store 1"), 17U);
    CRD_FUZZ_REQUIRE(hkdf_extract(AlgorithmId::HkdfSha256, salt, ConstSpan<u8>(master), Span<u8>(prk)) ==
                     CryptoError::None);
    CRD_FUZZ_REQUIRE(hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk), label, Span<u8>(okm)) ==
                     CryptoError::None);
    AeadKey aead;
    CRD_FUZZ_REQUIRE(aead.init(AlgorithmId::Aes256Gcm, ConstSpan<u8>(okm, 32U)) == CryptoError::None);
    Bytes body;
    body.resize(payload.size() - header - kAeadTagBytes);
    if (aead.open(ConstSpan<u8>(okm + 32U, kAeadNonceBytes), payload.subspan(header),
                  payload.subspan(0U, header), Span<u8>(body.data(), body.size())) != CryptoError::None)
    {
        return false;
    }
    return restate_body(ConstSpan<u8>(body.data(), body.size()), entries);
}

void raw_payload(const u8* data, usize size)
{
    write_store(ConstSpan<u8>(data, size));
    Array<Restated> expected;
    const bool      opens = restate_payload(ConstSpan<u8>(data, size), expected);
    FixedProtector  protector;
    KeyStore        store;
    CRD_FUZZ_REQUIRE((store.open(store_dir().generic(), protector) == CryptoError::None) == opens);
    CRD_FUZZ_REQUIRE(store.ready() == opens);
    if (opens)
    {
        check_listing(store, expected);
    }
}

// ---- The change model -------------------------------------------------------------------------------------------

constexpr AlgorithmId kAlgorithms[] = {AlgorithmId::Ed25519, AlgorithmId::X25519, AlgorithmId::Aes128Gcm,
                                       AlgorithmId::HkdfSha256, AlgorithmId::Siphash24};
constexpr const char* kNames[]       = {"a", "b", "c.d", "e_1"};

constexpr usize kSteps = 24U;

struct Modelled
{
    u32         generations = 0U; // 0: no such name
    AlgorithmId algorithm{};
    u8          states[kSteps + 1U]      = {}; // per generation: 0 Current, 1 Retired, 2 Revoked
    bool        known[kSteps + 1U]       = {}; // a secret the model knows (imported), per generation
    u8          secrets[kSteps + 1U][32] = {};
};

// The private input length of `algorithm`.
[[nodiscard]] usize private_length_of(AlgorithmId algorithm)
{
    u8    kind           = 0U;
    usize private_length = 0U;
    usize public_length  = 0U;
    CRD_FUZZ_REQUIRE(restated_kind(algorithm, kind, private_length, public_length));
    return private_length;
}

void check_model(const KeyStore& store, const Modelled (&model)[4])
{
    usize total = 0U;
    for (usize n = 0U; n < 4U; ++n)
    {
        const Modelled& m = model[n];
        total += m.generations;
        for (u32 g = 1U; g <= m.generations; ++g)
        {
            const KeyInfo* info = store.find(kNames[n], g);
            CRD_FUZZ_REQUIRE(info != nullptr && info->algorithm == m.algorithm &&
                             static_cast<u8>(info->state) == m.states[g - 1U]);
            KeyKind kind = KeyKind::Signature;
            CRD_FUZZ_REQUIRE(key_kind_of(m.algorithm, kind));
            if (kind == KeyKind::Secret)
            {
                const usize       length = private_length_of(m.algorithm);
                u8                out[32];
                const CryptoError read = store.secret(kNames[n], g, Span<u8>(out, length));
                CRD_FUZZ_REQUIRE(read == (m.states[g - 1U] == 2U ? CryptoError::Revoked : CryptoError::None));
                CRD_FUZZ_REQUIRE(read != CryptoError::None || !m.known[g - 1U] ||
                                 std::memcmp(out, m.secrets[g - 1U], length) == 0);
            }
        }
        CRD_FUZZ_REQUIRE(store.find(kNames[n], m.generations + 1U) == nullptr);
    }
    CRD_FUZZ_REQUIRE(store.size() == total);
}

void changes(const u8* data, usize size)
{
    reset_store();
    FixedProtector protector;
    KeyStore       store;
    CRD_FUZZ_REQUIRE(store.create(store_dir().generic(), protector) == CryptoError::None);
    Modelled model[4];
    Cursor   in(data, size);
    for (usize step = 0U; step < kSteps && in.left() >= 2U; ++step)
    {
        const u8         op   = in.byte();
        const u8         arg  = in.byte();
        const usize      n    = arg % 4U;
        Modelled&        m    = model[n];
        const StringView name = kNames[n];
        const u64        now  = step + 1U;
        CryptoError      want = CryptoError::None;
        CryptoError      got  = CryptoError::None;
        // The next generation of `m`: Current, the Current one before it Retired.
        const auto next = [&](bool known, ConstSpan<u8> value)
        {
            if (m.generations != 0U && m.states[m.generations - 1U] == 0U)
            {
                m.states[m.generations - 1U] = 1U;
            }
            m.states[m.generations] = 0U;
            m.known[m.generations]  = known;
            if (known)
            {
                std::memcpy(m.secrets[m.generations], value.data(), value.size());
            }
            ++m.generations;
        };
        switch (op % 7U)
        {
        case 0U: // generate
        {
            const AlgorithmId algorithm = kAlgorithms[(arg / 4U) % 5U];
            want = m.generations != 0U ? CryptoError::Exists : CryptoError::None;
            got  = store.generate(name, algorithm, PlacementRequest::Software, now);
            if (want == CryptoError::None)
            {
                m.algorithm = algorithm;
                next(false, {});
            }
            break;
        }
        case 1U: // import an AES-128-GCM key: the 16 bytes that follow
        {
            const ConstSpan<u8> value = in.take(16U);
            if (!in.ok())
            {
                return;
            }
            want = m.generations != 0U ? CryptoError::Exists : CryptoError::None;
            got  = store.import(name, AlgorithmId::Aes128Gcm, value, now);
            if (want == CryptoError::None)
            {
                m.algorithm = AlgorithmId::Aes128Gcm;
                next(true, value);
            }
            break;
        }
        case 2U: // rotate_import: the private input length of the name's algorithm that follows (any 32 bytes are an
                 // Ed25519 or X25519 key, any bytes of the length a secret)
        {
            if (m.generations == 0U)
            {
                want = CryptoError::NotFound;
                got  = store.rotate_import(name, ConstSpan<u8>(&arg, 1U), now);
                break;
            }
            const ConstSpan<u8> value = in.take(private_length_of(m.algorithm));
            if (!in.ok())
            {
                return;
            }
            KeyKind kind = KeyKind::Signature;
            CRD_FUZZ_REQUIRE(key_kind_of(m.algorithm, kind));
            got = store.rotate_import(name, value, now);
            next(kind == KeyKind::Secret, value);
            break;
        }
        case 3U: // rotate
            want = m.generations == 0U ? CryptoError::NotFound : CryptoError::None;
            got  = store.rotate(name, now);
            if (want == CryptoError::None)
            {
                next(false, {});
            }
            break;
        case 4U: // revoke
        {
            const u32 generation = (arg / 4U) % 4U;
            want = generation == 0U || generation > m.generations ? CryptoError::NotFound : CryptoError::None;
            got  = store.revoke(name, generation, now);
            if (want == CryptoError::None)
            {
                m.states[generation - 1U] = 2U;
            }
            break;
        }
        case 5U: // destroy
            want          = m.generations == 0U ? CryptoError::NotFound : CryptoError::None;
            got           = store.destroy(name);
            m.generations = 0U;
            break;
        default: // reopen
            got = store.open(store_dir().generic(), protector);
            break;
        }
        CRD_FUZZ_REQUIRE(got == want);
        check_model(store, model);
    }
    KeyStore reopened;
    CRD_FUZZ_REQUIRE(reopened.open(store_dir().generic(), protector) == CryptoError::None);
    check_model(reopened, model);
}

// An honest body of every kind, from fixed private inputs.
[[nodiscard]] Bytes honest_body(bool revoke_second)
{
    struct Item
    {
        const char* name;
        AlgorithmId algorithm;
        u8          kind;
        u32         generation;
        u8          state;
    };
    const Item items[] = {
        {"agree", AlgorithmId::X25519, 1U, 1U, 0U},
        {"kem", AlgorithmId::MlKem768, 2U, 1U, 0U},
        {"secret", AlgorithmId::Aes256Gcm, 3U, 1U, 1U},
        {"secret", AlgorithmId::Aes256Gcm, 3U, 2U, 0U},
        {"sign", AlgorithmId::Ed25519, 0U, 1U, static_cast<u8>(revoke_second ? 2U : 1U)},
        {"sign", AlgorithmId::Ed25519, 0U, 2U, 0U},
    };
    Bytes body;
    put(body, sizeof(items) / sizeof(items[0]), 4U);
    u8 counter = 1U;
    for (const Item& item : items)
    {
        u8    kind = 0U;
        usize private_length = 0U;
        usize public_length  = 0U;
        CRD_FUZZ_REQUIRE(restated_kind(item.algorithm, kind, private_length, public_length));
        u8 priv[64];
        for (usize i = 0U; i < private_length; ++i)
        {
            priv[i] = static_cast<u8>(counter * 7U + i);
        }
        ++counter;
        Bytes pub;
        pub.resize(public_length);
        KeyId id;
        CRD_FUZZ_REQUIRE(restated_public(kind, item.algorithm, ConstSpan<u8>(priv, private_length), pub, id));
        put(body, std::strlen(item.name), 1U);
        put(body, ConstSpan<u8>(reinterpret_cast<const u8*>(item.name), std::strlen(item.name)));
        put(body, item.generation, 4U);
        put(body, static_cast<u16>(item.algorithm), 2U);
        put(body, kind, 1U);
        put(body, item.state, 1U);
        put(body, 0U, 1U);
        put(body, 100U, 8U);
        put(body, 100U + item.state, 8U);
        put(body, ConstSpan<u8>(id.bytes));
        put(body, public_length, 2U);
        put(body, ConstSpan<u8>(pub.data(), pub.size()));
        const usize held = item.state == 2U ? 0U : private_length;
        put(body, held, 1U);
        put(body, ConstSpan<u8>(priv, held));
    }
    return body;
}

void add(crd::fuzz::SeedSink& sink, const char* name, u8 mode, const Bytes& data)
{
    Bytes input;
    input.push_back(mode);
    put(input, ConstSpan<u8>(data.data(), data.size()));
    sink.add(name, input.data(), input.size());
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    switch (data[0] % 3U)
    {
    case 0U:
        sealed_body(data + 1U, size - 1U);
        break;
    case 1U:
        raw_payload(data + 1U, size - 1U);
        break;
    default:
        changes(data + 1U, size - 1U);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    const Bytes honest = honest_body(false);
    add(sink, "body-honest", 0U, honest);
    add(sink, "body-revoked", 0U, honest_body(true));
    Bytes empty;
    put(empty, 0U, 4U);
    add(sink, "body-empty", 0U, empty);
    Bytes changed = honest;
    changed[5] ^= 0x20U; // the first name's first letter: out of order
    add(sink, "body-unordered", 0U, changed);
    changed = honest;
    changed.push_back(0U);
    add(sink, "body-trailing", 0U, changed);
    changed = honest;
    changed[changed.size() - 1U] ^= 0x01U; // the last private input: not its public key's
    add(sink, "body-mismatched", 0U, changed);

    const Bytes payload = seal_store(ConstSpan<u8>(honest.data(), honest.size()));
    add(sink, "payload-sealed", 1U, payload);
    changed = payload;
    changed[changed.size() - 1U] ^= 0x01U;
    add(sink, "payload-forged-tag", 1U, changed);

    const u8 sequence[] = {0, 0, 0, 1, 3, 0, 4, 4, 0, 2, 5, 1, 6, 0, 3, 0, 4, 0};
    Bytes    ops;
    put(ops, ConstSpan<u8>(sequence));
    add(sink, "changes-mixed", 2U, ops);
    Bytes secrets;
    put(secrets, ConstSpan<u8>(reinterpret_cast<const u8*>("\x01\x02"), 2U));
    for (u8 i = 0U; i < 16U; ++i)
    {
        secrets.push_back(i);
    }
    put(secrets, ConstSpan<u8>(reinterpret_cast<const u8*>("\x02\x02"), 2U));
    for (u8 i = 0U; i < 16U; ++i)
    {
        secrets.push_back(static_cast<u8>(0x80U + i));
    }
    const u8 tail[] = {4, 6, 6, 0, 5, 2};
    put(secrets, ConstSpan<u8>(tail));
    add(sink, "changes-secrets", 2U, secrets);
}
