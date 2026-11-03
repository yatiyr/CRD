// crd-crypto -- the key store (NET.2; contract: docs/design/network-foundation.md#key-store).

#include <crd/crypto/key_store.hpp>

#include "byte_spans.hpp"
#include "provider_internal.hpp"

#include <crd/platform/durable_file.hpp>
#include <crd/platform/filesystem.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <cstring>

namespace crd::crypto
{
namespace
{
namespace fs = crd::platform::fs;
using detail::label_of;

// The store file's payload (decision K3), integers little-endian:
//   magic "CRDKEYST" | format u16 | flags u16 | store id (16) | sequence u64 | protector name (u8 length, bytes) |
//   reference (u16 length, bytes) | pending reference (u16 length, bytes; a replaced master key's, to forget) |
//   salt (32) | the sealed body: AES-256-GCM under the key and nonce HKDF-SHA256 derives from the master key and the
//   salt, authenticating every byte before it.
// The body: a u32 count, then per generation, ordered by name and generation:
//   name (u8 length, bytes) | generation u32 | algorithm u16 | kind u8 | state u8 | placement u8 | created u64 |
//   changed u64 | identifier (32) | public key (u16 length, bytes) | private input (u8 length, bytes; none if revoked;
//   a Hardware key's device reference)
constexpr u8    kMagic[8]          = {'C', 'R', 'D', 'K', 'E', 'Y', 'S', 'T'};
constexpr u16   kFormat            = 1U;
constexpr usize kSaltBytes         = 32U;
constexpr usize kFixedHeaderBytes  = sizeof(kMagic) + 2U + 2U + kKeyStoreIdBytes + 8U;
constexpr usize kEntryFixedBytes   = 1U + 4U + 2U + 1U + 1U + 1U + 8U + 8U + kKeyIdBytes + 2U + 1U;
constexpr usize kGenerationTries   = 64U; // fresh ECDSA and ECDH scalars land outside [1, n - 1] with odds 2^-32
constexpr char  kStoreFileName[]   = "keys";
constexpr char  kStoreTempName[]   = "keys.tmp"; // the durable save's file in flight (durable_file.hpp)
constexpr char  kLockFileName[]    = "lock";
constexpr char  kBodyLabel[]       = "cerid key store 1";
constexpr char  kSecretIdLabel[]   = "cerid key id";

// ---- byte strings ----

// Writes front to back into a buffer sized beforehand; the sizes are computed from the same fields.
class Writer
{
public:
    explicit Writer(u8* at) noexcept : m_at(at) {}

    void bytes(cont::ConstSpan<u8> value) noexcept
    {
        if (!value.empty())
        {
            std::memcpy(m_at, value.data(), value.size());
        }
        m_at += value.size();
    }

    void le(u64 value, usize width) noexcept
    {
        for (usize i = 0U; i < width; ++i)
        {
            *m_at++ = static_cast<u8>(value >> (8U * i));
        }
    }

    [[nodiscard]] u8* at() const noexcept { return m_at; }

private:
    u8* m_at;
};

// Reads front to back; a read past the end fails it, and every later read answers zero bytes.
class Reader
{
public:
    explicit Reader(cont::ConstSpan<u8> bytes) noexcept : m_bytes(bytes) {}

    [[nodiscard]] cont::ConstSpan<u8> bytes(usize count) noexcept
    {
        if (!m_ok || m_bytes.size() - m_at < count)
        {
            m_ok = false;
            return {};
        }
        const cont::ConstSpan<u8> out = m_bytes.subspan(m_at, count);
        m_at += count;
        return out;
    }

    [[nodiscard]] u64 le(usize width) noexcept
    {
        const cont::ConstSpan<u8> field = bytes(width);
        u64                       value = 0U;
        for (usize i = 0U; i < field.size(); ++i)
        {
            value |= static_cast<u64>(field[i]) << (8U * i);
        }
        return value;
    }

    [[nodiscard]] bool  ok() const noexcept { return m_ok; }
    [[nodiscard]] usize at() const noexcept { return m_at; }
    [[nodiscard]] usize left() const noexcept { return m_bytes.size() - m_at; }

private:
    cont::ConstSpan<u8> m_bytes;
    usize               m_at = 0U;
    bool                m_ok = true;
};

[[nodiscard]] cont::ConstSpan<u8> bytes_of(cont::StringView text) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

[[nodiscard]] cont::ConstSpan<u8> bytes_of(const cont::Array<u8>& bytes) noexcept
{
    return cont::ConstSpan<u8>(bytes.data(), bytes.size());
}

void assign(cont::Array<u8>& out, cont::ConstSpan<u8> bytes)
{
    out.resize(bytes.size());
    if (!bytes.empty())
    {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
}

// Zeroes a buffer of secrets when its scope ends, on every path.
class ZeroOnExit
{
public:
    explicit ZeroOnExit(cont::Array<u8>& bytes) noexcept : m_bytes(bytes) {}
    ZeroOnExit(const ZeroOnExit&)            = delete;
    ZeroOnExit& operator=(const ZeroOnExit&) = delete;
    ~ZeroOnExit() { secure_zero(cont::Span<u8>(m_bytes.data(), m_bytes.size())); }

private:
    cont::Array<u8>& m_bytes;
};

// ---- kinds and lengths ----

[[nodiscard]] usize secret_bytes(AlgorithmId algorithm) noexcept
{
    if (const usize aead = aead_key_bytes(algorithm); aead != 0U)
    {
        return aead;
    }
    if (const usize hkdf = hkdf_hash_bytes(algorithm); hkdf != 0U)
    {
        return hkdf;
    }
    return algorithm == AlgorithmId::Siphash24 && provider_implements(algorithm) ? kPrfKeyBytes : 0U;
}

[[nodiscard]] usize private_bytes(KeyKind kind, AlgorithmId algorithm) noexcept
{
    switch (kind)
    {
    case KeyKind::Signature:
        return signature_sizes(algorithm).seed;
    case KeyKind::Agreement:
        return agreement_sizes(algorithm).private_key;
    case KeyKind::Kem:
        return kem_sizes(algorithm).seed;
    case KeyKind::Secret:
        return secret_bytes(algorithm);
    }
    return 0U;
}

[[nodiscard]] usize public_bytes(KeyKind kind, AlgorithmId algorithm) noexcept
{
    switch (kind)
    {
    case KeyKind::Signature:
        return signature_sizes(algorithm).public_key;
    case KeyKind::Agreement:
        return agreement_sizes(algorithm).public_key;
    case KeyKind::Kem:
        return kem_sizes(algorithm).public_key;
    case KeyKind::Secret:
        return 0U;
    }
    return 0U;
}

// A secret's identifier: HKDF-SHA256 of it with an empty salt, expanded under "cerid key id".
[[nodiscard]] CryptoError secret_id(cont::ConstSpan<u8> secret, KeyId& id) noexcept
{
    return provider_internal::sealing_key_name(secret, label_of(kSecretIdLabel), cont::Span<u8>(id.bytes));
}

// The public key and identifier `private_input` of `kind` determines: BadLength or BadKey as the provider's key
// objects answer for an input they refuse.
[[nodiscard]] CryptoError derive_public(KeyKind kind, AlgorithmId algorithm, cont::ConstSpan<u8> private_input,
                                        cont::Span<u8> public_key, KeyId& id) noexcept
{
    CryptoError error = CryptoError::None;
    switch (kind)
    {
    case KeyKind::Signature:
    {
        SigningKey key;
        error = key.from_seed(algorithm, private_input);
        if (error == CryptoError::None)
        {
            error = key.public_key(public_key);
        }
        break;
    }
    case KeyKind::Agreement:
    {
        AgreementKey key;
        error = key.from_private(algorithm, private_input);
        if (error == CryptoError::None)
        {
            error = key.public_key(public_key);
        }
        break;
    }
    case KeyKind::Kem:
    {
        KemKey key;
        error = key.from_seed(algorithm, private_input);
        if (error == CryptoError::None)
        {
            error = key.public_key(public_key);
        }
        break;
    }
    case KeyKind::Secret:
        return private_input.size() == secret_bytes(algorithm) ? secret_id(private_input, id) : CryptoError::BadLength;
    }
    if (error == CryptoError::None)
    {
        id = key_id_of(public_key);
    }
    return error;
}

[[nodiscard]] fs::Path file_in(cont::StringView directory, cont::StringView name)
{
    return fs::Path(directory) / name;
}

// The header fields of a payload; the spans point into it.
struct Header
{
    u8                  id[kKeyStoreIdBytes] = {};
    u64                 sequence             = 0U;
    cont::ConstSpan<u8> protector;
    cont::ConstSpan<u8> reference;
    cont::ConstSpan<u8> pending;
    cont::ConstSpan<u8> salt;
    usize               body_at = 0U; // where the sealed body starts: the associated data is everything before it
};

[[nodiscard]] bool valid_protector_name(cont::ConstSpan<u8> name) noexcept
{
    if (name.empty() || name.size() > kKeyProtectorNameBytes)
    {
        return false;
    }
    return std::all_of(name.begin(), name.end(),
                       [](u8 c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// Damaged unless `payload` frames a store: the magic, the format, zero flags, a nonzero sequence, valid lengths and a
// sealed body at least a tag long.
[[nodiscard]] CryptoError read_header(cont::ConstSpan<u8> payload, Header& out) noexcept
{
    Reader                    in(payload);
    const cont::ConstSpan<u8> magic = in.bytes(sizeof(kMagic));
    const u64                 format = in.le(2U);
    const u64                 flags  = in.le(2U);
    const cont::ConstSpan<u8> id     = in.bytes(kKeyStoreIdBytes);
    out.sequence                     = in.le(8U);
    out.protector                    = in.bytes(static_cast<usize>(in.le(1U)));
    out.reference                    = in.bytes(static_cast<usize>(in.le(2U)));
    out.pending                      = in.bytes(static_cast<usize>(in.le(2U)));
    out.salt                         = in.bytes(kSaltBytes);
    out.body_at                      = in.at();
    if (!in.ok() || !detail::same_bytes(magic, cont::ConstSpan<u8>(kMagic)) || format != kFormat || flags != 0U ||
        out.sequence == 0U || !valid_protector_name(out.protector) || out.reference.empty() ||
        out.reference.size() > kKeyReferenceMaxBytes || out.pending.size() > kKeyReferenceMaxBytes ||
        in.left() < kAeadTagBytes)
    {
        return CryptoError::Damaged;
    }
    std::memcpy(out.id, id.data(), kKeyStoreIdBytes);
    return CryptoError::None;
}

[[nodiscard]] bool same_name(const KeyInfo& info, cont::StringView name) noexcept
{
    return info.name() == name;
}

// Ordered by name, then generation: the store's order.
[[nodiscard]] bool precedes(const KeyInfo& info, cont::StringView name, u32 generation) noexcept
{
    const int order = info.name().compare(name);
    return order < 0 || (order == 0 && info.generation < generation);
}
} // namespace

cont::StringView key_protection_name(KeyProtection protection) noexcept
{
    switch (protection)
    {
    case KeyProtection::Application:
        return "application";
    case KeyProtection::Platform:
        return "platform";
    case KeyProtection::Hardware:
        return "hardware";
    }
    return "unknown";
}

bool key_kind_of(AlgorithmId algorithm, KeyKind& kind) noexcept
{
    if (signature_sizes(algorithm).seed != 0U)
    {
        kind = KeyKind::Signature;
    }
    else if (agreement_sizes(algorithm).private_key != 0U)
    {
        kind = KeyKind::Agreement;
    }
    else if (kem_sizes(algorithm).seed != 0U)
    {
        kind = KeyKind::Kem;
    }
    else if (secret_bytes(algorithm) != 0U)
    {
        kind = KeyKind::Secret;
    }
    else
    {
        return false;
    }
    return true;
}

bool valid_key_name(cont::StringView name) noexcept
{
    if (name.empty() || name.size() > kKeyNameMaxBytes)
    {
        return false;
    }
    const auto alphanumeric = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return alphanumeric(name[0]) && std::all_of(name.begin(), name.end(), [&](char c)
                                                { return alphanumeric(c) || c == '.' || c == '_' || c == '-'; });
}

// Whether a device may hold keys of `algorithm` (decision K8: ECDSA, the one signature every device implements).
[[nodiscard]] bool device_algorithm(KeyKind kind, AlgorithmId algorithm) noexcept
{
    return kind == KeyKind::Signature && provider_internal::ecdsa_field_bytes(algorithm) != 0U;
}

// ---- DeviceSigner and StoredSigner ----

void DeviceSigner::clear() noexcept
{
    *this = DeviceSigner{};
}

CryptoError DeviceSigner::public_key(cont::Span<u8> out) const noexcept
{
    const usize length = signature_sizes(m_algorithm).public_key;
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (out.size() != length)
    {
        return CryptoError::BadLength;
    }
    std::memcpy(out.data(), m_public_key, length);
    return CryptoError::None;
}

CryptoError DeviceSigner::sign(cont::ConstSpan<u8> message, cont::Span<u8> out, usize& written) const noexcept
{
    written = 0U;
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (out.size() < signature_sizes(m_algorithm).signature)
    {
        return CryptoError::BadLength;
    }
    // SHA-256 for P-256 and SHA-384 for P-384: each digest is its curve's field length, as are r and s.
    const usize field = provider_internal::ecdsa_field_bytes(m_algorithm);
    u8          digest[48];
    u8          raw[96];
    CryptoError error = provider_internal::ecdsa_digest(m_algorithm, message, cont::Span<u8>(digest, field));
    if (error == CryptoError::None)
    {
        error = m_device->sign_digest(cont::ConstSpan<u8>(m_store_id), cont::ConstSpan<u8>(m_reference, m_reference_bytes),
                                      m_algorithm, cont::ConstSpan<u8>(digest, field), cont::Span<u8>(raw, 2U * field));
    }
    if (error == CryptoError::None)
    {
        error = provider_internal::ecdsa_der_of(m_algorithm, cont::ConstSpan<u8>(raw, 2U * field), out, written);
    }
    // A signature leaves only once it verifies under the stored key: a device that signs with another key, or faults
    // while signing, is caught here.
    const cont::ConstSpan<u8> public_key(m_public_key, signature_sizes(m_algorithm).public_key);
    if (error == CryptoError::None &&
        ProviderVerifier{}.verify(m_algorithm, public_key, message, cont::ConstSpan<u8>(out.data(), written)) !=
            Verification::Valid)
    {
        error = CryptoError::ProviderFailure;
    }
    if (error != CryptoError::None)
    {
        secure_zero(out);
        written = 0U;
    }
    return error;
}

void StoredSigner::clear() noexcept
{
    m_key.clear();
    m_device.clear();
    m_info = KeyInfo{};
}

CryptoError StoredSigner::public_key(cont::Span<u8> out) const noexcept
{
    return m_device.ready() ? m_device.public_key(out) : m_key.public_key(out);
}

CryptoError StoredSigner::sign(cont::ConstSpan<u8> message, cont::Span<u8> out, usize& written) const noexcept
{
    return m_device.ready() ? m_device.sign(message, out, written) : m_key.sign(message, out, written);
}

// ---- KeyStore ----

KeyStore::Entry::~Entry() noexcept
{
    secure_zero(cont::Span<u8>(private_input));
}

KeyStore::Snapshot::~Snapshot() noexcept
{
    secure_zero(cont::Span<u8>(master));
}

enum class ChangeKind : u8
{
    Add,
    Rotate,
    Revoke,
    Destroy,
};

struct KeyStore::Change
{
    ChangeKind          kind = ChangeKind::Add;
    cont::StringView    name;
    AlgorithmId         algorithm{};
    PlacementRequest    placement = PlacementRequest::Software;
    cont::ConstSpan<u8> private_input; // empty: fresh from the provider's randomness
    u32                 generation = 0U;
    u64                 now        = 0U;
    KeyDevice*          device     = nullptr; // the store's, for Hardware keys
    cont::ConstSpan<u8> store_id;
    cont::Array<u8>*    created = nullptr; // the device reference of a key made for the change, forgotten if unsaved
};

KeyProtection KeyStore::protection() const noexcept
{
    return m_protector != nullptr ? m_protector->protection() : KeyProtection::Application;
}

void KeyStore::close() noexcept
{
    secure_zero(cont::Span<u8>(m_master));
    m_entries.clear();
    m_reference.clear();
    m_directory.clear();
    std::memset(m_id, 0, sizeof(m_id));
    m_sequence  = 0U;
    m_protector = nullptr;
    m_device    = nullptr;
}

CryptoError KeyStore::save(cont::ConstSpan<Entry> entries, u64 sequence, cont::ConstSpan<u8> reference,
                           cont::ConstSpan<u8> pending, cont::ConstSpan<u8> master) const noexcept
{
    const cont::ConstSpan<u8> protector = bytes_of(m_protector->name());
    usize                     body      = 4U;
    for (const Entry& entry : entries)
    {
        body += kEntryFixedBytes + entry.info.name_bytes + entry.public_key.size() + entry.private_bytes;
    }
    const usize header = kFixedHeaderBytes + 1U + protector.size() + 2U + reference.size() + 2U + pending.size() +
                         kSaltBytes;
    cont::Array<u8> payload;
    payload.resize(header + body + kAeadTagBytes);
    const ZeroOnExit zero_payload(payload); // the plaintext body is laid out in it before it is sealed in place

    Writer out(payload.data());
    out.bytes(cont::ConstSpan<u8>(kMagic));
    out.le(kFormat, 2U);
    out.le(0U, 2U);
    out.bytes(cont::ConstSpan<u8>(m_id));
    out.le(sequence, 8U);
    out.le(protector.size(), 1U);
    out.bytes(protector);
    out.le(reference.size(), 2U);
    out.bytes(reference);
    out.le(pending.size(), 2U);
    out.bytes(pending);
    u8* const salt = out.at();
    if (random_bytes(cont::Span<u8>(salt, kSaltBytes)) != CryptoError::None)
    {
        return CryptoError::ProviderFailure;
    }
    out = Writer(salt + kSaltBytes);
    out.le(entries.size(), 4U);
    for (const Entry& entry : entries)
    {
        const KeyInfo& info = entry.info;
        out.le(info.name_bytes, 1U);
        out.bytes(bytes_of(info.name()));
        out.le(info.generation, 4U);
        out.le(static_cast<u16>(info.algorithm), 2U);
        out.le(static_cast<u8>(info.kind), 1U);
        out.le(static_cast<u8>(info.state), 1U);
        out.le(static_cast<u8>(info.placement), 1U);
        out.le(info.created, 8U);
        out.le(info.changed, 8U);
        out.bytes(cont::ConstSpan<u8>(info.id.bytes));
        out.le(entry.public_key.size(), 2U);
        out.bytes(bytes_of(entry.public_key));
        out.le(entry.private_bytes, 1U);
        out.bytes(cont::ConstSpan<u8>(entry.private_input, entry.private_bytes));
    }

    AeadKey aead;
    u8      nonce[kAeadNonceBytes];
    if (provider_internal::salted_aead(master, cont::ConstSpan<u8>(salt, kSaltBytes), label_of(kBodyLabel), aead,
                                       nonce) != CryptoError::None)
    {
        return CryptoError::ProviderFailure;
    }
    const cont::Span<u8> sealed(payload.data() + header, body + kAeadTagBytes);
    if (aead.seal(cont::ConstSpan<u8>(nonce), cont::ConstSpan<u8>(sealed.data(), body),
                  cont::ConstSpan<u8>(payload.data(), header), sealed) != CryptoError::None)
    {
        return CryptoError::ProviderFailure;
    }
    const fs::SaveWrite written =
        fs::save_generation(file_in(m_directory, kStoreFileName), bytes_of(payload), 0U);
    return written.ok ? CryptoError::None : CryptoError::StorageFailure;
}

CryptoError KeyStore::read(cont::StringView directory, KeyProtector& protector, Snapshot& out) const noexcept
{
    cont::Array<u8>      payload;
    const ZeroOnExit     zero_payload(payload); // opened in place
    const fs::SaveLoad   loaded = fs::load_generation(file_in(directory, kStoreFileName), 0U, payload);
    if (loaded.status == fs::SaveStatus::NotFound)
    {
        return CryptoError::NotFound;
    }
    if (loaded.status != fs::SaveStatus::Ok && loaded.status != fs::SaveStatus::Recovered)
    {
        return CryptoError::Damaged;
    }
    Header            header;
    const CryptoError framed = read_header(bytes_of(payload), header);
    if (framed != CryptoError::None)
    {
        return framed;
    }
    if (!detail::same_bytes(header.protector, bytes_of(protector.name())))
    {
        return CryptoError::Unsupported;
    }

    // The master key: the one already held when the reference is unchanged, the protector's otherwise.
    std::memcpy(out.id, header.id, kKeyStoreIdBytes);
    out.sequence = header.sequence;
    assign(out.reference, header.reference);
    assign(out.pending, header.pending);
    if (m_protector != nullptr && detail::same_bytes(header.reference, bytes_of(m_reference)))
    {
        std::memcpy(out.master, m_master, kKeyStoreMasterBytes);
    }
    else
    {
        const CryptoError recovered = protector.recover(cont::ConstSpan<u8>(header.id), header.reference,
                                                        cont::Span<u8>(out.master));
        if (recovered != CryptoError::None)
        {
            return recovered;
        }
    }

    AeadKey aead;
    u8      nonce[kAeadNonceBytes];
    if (provider_internal::salted_aead(cont::ConstSpan<u8>(out.master), header.salt, label_of(kBodyLabel), aead,
                                       nonce) != CryptoError::None)
    {
        return CryptoError::ProviderFailure;
    }
    const cont::Span<u8> sealed(payload.data() + header.body_at, payload.size() - header.body_at);
    const cont::Span<u8> body(sealed.data(), sealed.size() - kAeadTagBytes);
    if (aead.open(cont::ConstSpan<u8>(nonce), sealed, cont::ConstSpan<u8>(payload.data(), header.body_at), body) !=
        CryptoError::None)
    {
        return CryptoError::Forged;
    }
    return decode(body, out.entries);
}

CryptoError KeyStore::decode(cont::ConstSpan<u8> body, cont::Array<Entry>& entries) noexcept
{
    entries.clear();
    Reader    in(body);
    const u64 count = in.le(4U);
    if (!in.ok() || count > kKeyStoreMaxKeys)
    {
        return CryptoError::Malformed;
    }
    entries.reserve(static_cast<usize>(count));
    cont::Array<u8> derived; // a held private input's public key, compared with the stored one
    for (u64 i = 0U; i < count; ++i)
    {
        entries.push_back(Entry{});
        Entry&                    entry = entries.back();
        KeyInfo&                  info  = entry.info;
        const cont::ConstSpan<u8> name  = in.bytes(static_cast<usize>(in.le(1U)));
        info.generation                 = static_cast<u32>(in.le(4U));
        info.algorithm                  = static_cast<AlgorithmId>(in.le(2U));
        const u64 kind                  = in.le(1U);
        const u64 state                 = in.le(1U);
        const u64 placement             = in.le(1U);
        info.created                    = in.le(8U);
        info.changed                    = in.le(8U);
        const cont::ConstSpan<u8> id    = in.bytes(kKeyIdBytes);
        const cont::ConstSpan<u8> pub   = in.bytes(static_cast<usize>(in.le(2U)));
        const cont::ConstSpan<u8> priv  = in.bytes(static_cast<usize>(in.le(1U)));
        if (!in.ok())
        {
            return CryptoError::Malformed;
        }
        const cont::StringView text(reinterpret_cast<const char*>(name.data()), name.size());
        KeyKind                expected = KeyKind::Signature;
        const bool             hardware = placement == static_cast<u8>(KeyPlacement::Hardware);
        if (!valid_key_name(text) || !key_kind_of(info.algorithm, expected) || kind != static_cast<u8>(expected) ||
            state > static_cast<u8>(KeyState::Revoked) || placement > static_cast<u8>(KeyPlacement::Hardware) ||
            (hardware && !device_algorithm(expected, info.algorithm)) || info.generation == 0U)
        {
            return CryptoError::Malformed;
        }
        info.kind       = expected;
        info.state      = static_cast<KeyState>(state);
        info.placement  = static_cast<KeyPlacement>(placement);
        info.name_bytes = static_cast<u8>(name.size());
        std::memcpy(info.name_text, name.data(), name.size());
        std::memcpy(info.id.bytes, id.data(), kKeyIdBytes);

        // Generations of a name run 1, 2, ... with one algorithm; only the last may be Current, every earlier one
        // that is not Revoked is Retired.
        const Entry* previous = i == 0U ? nullptr : &entries[static_cast<usize>(i) - 1U];
        if (previous != nullptr && same_name(previous->info, text))
        {
            if (info.generation != previous->info.generation + 1U || info.algorithm != previous->info.algorithm ||
                previous->info.state == KeyState::Current)
            {
                return CryptoError::Malformed;
            }
        }
        else if (info.generation != 1U || (previous != nullptr && !precedes(previous->info, text, 0U)))
        {
            return CryptoError::Malformed;
        }

        // A Hardware key holds its device's reference (1 to kKeyDeviceReferenceMaxBytes) in place of a private input;
        // its public key is the one the device gave when it made the key, which only the identifier binds.
        const bool revoked = info.state == KeyState::Revoked;
        usize held_min = private_bytes(info.kind, info.algorithm);
        usize held_max = held_min;
        if (revoked)
        {
            held_min = 0U;
            held_max = 0U;
        }
        else if (hardware)
        {
            held_min = 1U;
            held_max = kKeyDeviceReferenceMaxBytes;
        }
        if (pub.size() != public_bytes(info.kind, info.algorithm) || priv.size() < held_min || priv.size() > held_max)
        {
            return CryptoError::Malformed;
        }
        assign(entry.public_key, pub);
        entry.private_bytes = static_cast<u8>(priv.size());
        if (!priv.empty())
        {
            std::memcpy(entry.private_input, priv.data(), priv.size());
        }

        // The identifier is an asymmetric key's public key's, and the one a held private input gives (its public
        // key's, or a secret's): so, through SHA-256, the stored public key is the held private input's.
        if (info.kind != KeyKind::Secret && !(key_id_of(pub) == info.id))
        {
            return CryptoError::Malformed;
        }
        if (!revoked && !hardware)
        {
            derived.resize(pub.size());
            KeyId             held;
            const CryptoError error =
                derive_public(info.kind, info.algorithm, priv, cont::Span<u8>(derived.data(), derived.size()), held);
            if (error == CryptoError::ProviderFailure)
            {
                return error;
            }
            if (error != CryptoError::None || !(held == info.id))
            {
                return CryptoError::Malformed;
            }
        }
    }
    return in.left() == 0U ? CryptoError::None : CryptoError::Malformed;
}

void KeyStore::adopt(KeyProtector& protector, cont::StringView directory, Snapshot& snapshot) noexcept
{
    m_protector = &protector;
    m_directory = cont::String(directory);
    std::memcpy(m_id, snapshot.id, kKeyStoreIdBytes);
    std::memcpy(m_master, snapshot.master, kKeyStoreMasterBytes);
    m_sequence  = snapshot.sequence;
    m_reference = std::move(snapshot.reference);
    m_entries   = std::move(snapshot.entries);
}

CryptoError KeyStore::create(cont::StringView directory, KeyProtector& protector, KeyDevice* device) noexcept
{
    close();
    if (!valid_protector_name(bytes_of(protector.name())) ||
        (device != nullptr && !valid_protector_name(bytes_of(device->name()))))
    {
        return CryptoError::Unsupported;
    }
    if (!fs::create_private_directory(fs::Path(directory)))
    {
        return CryptoError::StorageFailure;
    }
    fs::FileLock lock;
    if (!lock.acquire(file_in(directory, kLockFileName)))
    {
        return CryptoError::StorageFailure;
    }
    cont::Array<u8>    existing;
    const ZeroOnExit   zero_existing(existing);
    const fs::SaveLoad loaded = fs::load_generation(file_in(directory, kStoreFileName), 0U, existing);
    if (loaded.status != fs::SaveStatus::NotFound)
    {
        return CryptoError::Exists;
    }

    Snapshot snapshot;
    snapshot.sequence = 1U;
    if (random_bytes(cont::Span<u8>(snapshot.id)) != CryptoError::None ||
        random_bytes(cont::Span<u8>(snapshot.master)) != CryptoError::None)
    {
        return CryptoError::ProviderFailure;
    }
    const CryptoError guarded = protector.protect(cont::ConstSpan<u8>(snapshot.id),
                                                  cont::ConstSpan<u8>(snapshot.master), snapshot.reference);
    if (guarded != CryptoError::None)
    {
        return guarded;
    }
    if (snapshot.reference.empty() || snapshot.reference.size() > kKeyReferenceMaxBytes)
    {
        (void)protector.forget(cont::ConstSpan<u8>(snapshot.id), bytes_of(snapshot.reference));
        return CryptoError::Unsupported;
    }
    // save() writes through the members, so the store is adopted first and closed again if the save fails.
    adopt(protector, directory, snapshot);
    const CryptoError saved =
        save({}, m_sequence, bytes_of(m_reference), {}, cont::ConstSpan<u8>(m_master));
    if (saved != CryptoError::None)
    {
        (void)protector.forget(id(), bytes_of(m_reference));
        close();
        return saved;
    }
    m_device = device;
    return CryptoError::None;
}

CryptoError KeyStore::open(cont::StringView directory, KeyProtector& protector, KeyDevice* device) noexcept
{
    close();
    if (device != nullptr && !valid_protector_name(bytes_of(device->name())))
    {
        return CryptoError::Unsupported;
    }
    Snapshot          snapshot;
    const CryptoError read_error = read(directory, protector, snapshot);
    if (read_error == CryptoError::None)
    {
        adopt(protector, directory, snapshot);
        m_device = device;
    }
    return read_error;
}

CryptoError KeyStore::refresh() noexcept
{
    if (m_protector == nullptr)
    {
        return CryptoError::NotReady;
    }
    Snapshot          snapshot;
    const CryptoError read_error = read(m_directory, *m_protector, snapshot);
    if (read_error != CryptoError::None)
    {
        return read_error;
    }
    const CryptoError same = same_store(snapshot);
    if (same == CryptoError::None)
    {
        adopt(*m_protector, cont::StringView(m_directory.data(), m_directory.size()), snapshot);
    }
    return same;
}

CryptoError KeyStore::same_store(const Snapshot& snapshot) const noexcept
{
    if (!detail::same_bytes(cont::ConstSpan<u8>(snapshot.id), id()))
    {
        return CryptoError::Forged; // another store was put in its place
    }
    return snapshot.sequence < m_sequence ? CryptoError::Replayed : CryptoError::None;
}

const KeyStore::Entry* KeyStore::last_of(cont::ConstSpan<Entry> entries, cont::StringView name) noexcept
{
    const Entry* const end   = entries.data() + entries.size();
    const Entry* const after = std::upper_bound(entries.data(), end, name, [](cont::StringView key, const Entry& entry)
                                                { return key < entry.info.name(); });
    return after != entries.data() && same_name((after - 1)->info, name) ? after - 1 : nullptr;
}

const KeyStore::Entry* KeyStore::entry(cont::StringView name, u32 generation) const noexcept
{
    const cont::ConstSpan<Entry> entries(m_entries.data(), m_entries.size());
    if (generation == 0U)
    {
        return last_of(entries, name);
    }
    const Entry* const end = entries.data() + entries.size();
    const Entry* const at  = std::lower_bound(entries.data(), end, name, [&](const Entry& entry, cont::StringView key)
                                              { return precedes(entry.info, key, generation); });
    return at != end && same_name(at->info, name) && at->info.generation == generation ? at : nullptr;
}

const KeyInfo* KeyStore::find(cont::StringView name, u32 generation) const noexcept
{
    const Entry* const found = entry(name, generation);
    if (found == nullptr || (generation == 0U && found->info.state != KeyState::Current))
    {
        return nullptr;
    }
    return &found->info;
}

CryptoError KeyStore::usable(cont::StringView name, u32 generation, KeyKind kind, const Entry*& out) const noexcept
{
    if (m_protector == nullptr)
    {
        return CryptoError::NotReady;
    }
    out = entry(name, generation);
    if (out == nullptr)
    {
        return CryptoError::NotFound;
    }
    if (out->info.kind != kind)
    {
        return CryptoError::Unsupported;
    }
    if (out->info.state == KeyState::Revoked)
    {
        return CryptoError::Revoked;
    }
    return CryptoError::None;
}

CryptoError KeyStore::public_key(cont::StringView name, u32 generation, cont::Span<u8> out) const noexcept
{
    const Entry* const found = m_protector != nullptr ? entry(name, generation) : nullptr;
    if (found == nullptr)
    {
        return m_protector == nullptr ? CryptoError::NotReady : CryptoError::NotFound;
    }
    if (found->info.kind == KeyKind::Secret)
    {
        return CryptoError::Unsupported;
    }
    if (out.size() != found->public_key.size())
    {
        return CryptoError::BadLength;
    }
    std::memcpy(out.data(), found->public_key.data(), out.size());
    return CryptoError::None;
}

CryptoError KeyStore::signer(cont::StringView name, StoredSigner& out) const noexcept
{
    out.clear();
    const Entry*      found = nullptr;
    const CryptoError error = usable(name, 0U, KeyKind::Signature, found);
    if (error != CryptoError::None)
    {
        return error;
    }
    if (found->info.placement == KeyPlacement::Hardware)
    {
        if (m_device == nullptr)
        {
            return CryptoError::Unavailable;
        }
        DeviceSigner& device     = out.m_device;
        device.m_device          = m_device;
        device.m_algorithm       = found->info.algorithm;
        device.m_reference_bytes = found->private_bytes;
        std::memcpy(device.m_store_id, m_id, kKeyStoreIdBytes);
        std::memcpy(device.m_reference, found->private_input, found->private_bytes);
        std::memcpy(device.m_public_key, found->public_key.data(), found->public_key.size());
        out.m_info = found->info;
        return CryptoError::None;
    }
    const CryptoError loaded =
        out.m_key.from_seed(found->info.algorithm, cont::ConstSpan<u8>(found->private_input, found->private_bytes));
    if (loaded == CryptoError::None)
    {
        out.m_info = found->info;
    }
    return loaded;
}

CryptoError KeyStore::agreement_key(cont::StringView name, u32 generation, AgreementKey& out) const noexcept
{
    out.clear();
    const Entry*      found = nullptr;
    const CryptoError error = usable(name, generation, KeyKind::Agreement, found);
    if (error != CryptoError::None)
    {
        return error;
    }
    return out.from_private(found->info.algorithm, cont::ConstSpan<u8>(found->private_input, found->private_bytes));
}

CryptoError KeyStore::kem_key(cont::StringView name, u32 generation, KemKey& out) const noexcept
{
    out.clear();
    const Entry*      found = nullptr;
    const CryptoError error = usable(name, generation, KeyKind::Kem, found);
    if (error != CryptoError::None)
    {
        return error;
    }
    return out.from_seed(found->info.algorithm, cont::ConstSpan<u8>(found->private_input, found->private_bytes));
}

CryptoError KeyStore::secret(cont::StringView name, u32 generation, cont::Span<u8> out) const noexcept
{
    const Entry*      found = nullptr;
    const CryptoError error = usable(name, generation, KeyKind::Secret, found);
    if (error != CryptoError::None)
    {
        return detail::refuse(error, out);
    }
    if (out.size() != found->private_bytes)
    {
        return detail::refuse(CryptoError::BadLength, out);
    }
    std::memcpy(out.data(), found->private_input, out.size());
    return CryptoError::None;
}

// ---- changes ----

CryptoError KeyStore::make(const Change& change, KeyKind kind, AlgorithmId algorithm, u32 generation,
                           Entry& out) noexcept
{
    KeyInfo& info   = out.info;
    info.generation = generation;
    info.algorithm  = algorithm;
    info.kind       = kind;
    info.state      = KeyState::Current;
    info.placement  = KeyPlacement::Software;
    info.created    = change.now;
    info.changed    = change.now;
    info.name_bytes = static_cast<u8>(change.name.size());
    std::memcpy(info.name_text, change.name.data(), change.name.size());

    // In the device when the change asks for it and the device holds the algorithm; HardwarePreferred falls back to
    // the sealed file otherwise (decision K8).
    const bool wants_device = change.placement != PlacementRequest::Software;
    if (wants_device && change.device != nullptr && device_algorithm(kind, algorithm))
    {
        out.public_key.resize(public_bytes(kind, algorithm));
        cont::Array<u8>&  reference = *change.created;
        const CryptoError made      = change.device->create(
            change.store_id, algorithm, reference, cont::Span<u8>(out.public_key.data(), out.public_key.size()));
        if (made == CryptoError::None)
        {
            const cont::ConstSpan<u8>       public_key(out.public_key.data(), out.public_key.size());
            const bssl::UniquePtr<EVP_PKEY> point(provider_internal::signature_public_pkey(algorithm, public_key));
            if (reference.empty() || reference.size() > kKeyDeviceReferenceMaxBytes || !point)
            {
                (void)change.device->forget(change.store_id, bytes_of(reference));
                reference.clear();
                return CryptoError::BadKey; // a device that answers with something no store can keep
            }
            info.placement    = KeyPlacement::Hardware;
            info.id           = key_id_of(public_key);
            out.private_bytes = static_cast<u8>(reference.size());
            std::memcpy(out.private_input, reference.data(), reference.size());
            return CryptoError::None;
        }
        if (made != CryptoError::Unsupported || change.placement == PlacementRequest::HardwareRequired)
        {
            return made;
        }
    }
    else if (change.placement == PlacementRequest::HardwareRequired)
    {
        return CryptoError::Unsupported;
    }

    const usize length = private_bytes(kind, algorithm);
    out.private_bytes  = static_cast<u8>(length);
    out.public_key.resize(public_bytes(kind, algorithm));
    const cont::Span<u8> private_input(out.private_input, length);
    const cont::Span<u8> public_key(out.public_key.data(), out.public_key.size());
    if (!change.private_input.empty())
    {
        if (change.private_input.size() != length)
        {
            return CryptoError::BadLength;
        }
        std::memcpy(private_input.data(), change.private_input.data(), length);
        return derive_public(kind, algorithm, private_input, public_key, info.id);
    }
    // A fresh input; an ECDSA or ECDH scalar outside [1, n - 1] is drawn again.
    for (usize attempt = 0U; attempt < kGenerationTries; ++attempt)
    {
        if (random_bytes(private_input) != CryptoError::None)
        {
            return CryptoError::ProviderFailure;
        }
        const CryptoError derived = derive_public(kind, algorithm, private_input, public_key, info.id);
        if (derived != CryptoError::BadKey)
        {
            return derived;
        }
    }
    return CryptoError::ProviderFailure;
}

CryptoError KeyStore::change_entries(const Change& change, cont::Array<Entry>& entries, bool& modified) noexcept
{
    modified = false;
    const cont::ConstSpan<Entry> view(entries.data(), entries.size());
    const Entry* const           last = last_of(view, change.name);
    const usize                  at   = last == nullptr ? 0U : static_cast<usize>(last - entries.data());
    switch (change.kind)
    {
    case ChangeKind::Add:
    {
        KeyKind kind = KeyKind::Signature;
        if (!key_kind_of(change.algorithm, kind) ||
            (!change.private_input.empty() && change.placement != PlacementRequest::Software))
        {
            return CryptoError::Unsupported;
        }
        if (last != nullptr)
        {
            return CryptoError::Exists;
        }
        if (entries.size() >= kKeyStoreMaxKeys)
        {
            return CryptoError::Exhausted;
        }
        Entry             fresh;
        const CryptoError made = make(change, kind, change.algorithm, 1U, fresh);
        if (made != CryptoError::None)
        {
            return made;
        }
        const usize place = static_cast<usize>(
            std::lower_bound(entries.data(), entries.data() + entries.size(), change.name,
                             [](const Entry& entry, cont::StringView key) { return entry.info.name() < key; }) -
            entries.data());
        entries.push_back(std::move(fresh));
        std::rotate(entries.data() + place, entries.data() + entries.size() - 1U, entries.data() + entries.size());
        modified = true;
        return CryptoError::None;
    }
    case ChangeKind::Rotate:
    {
        if (last == nullptr)
        {
            return CryptoError::NotFound;
        }
        if (last->info.generation == ~u32{0U} || entries.size() >= kKeyStoreMaxKeys)
        {
            return CryptoError::Exhausted;
        }
        // The next generation keeps the name's placement: a Hardware name rotates within the device, and only there.
        const bool hardware = last->info.placement == KeyPlacement::Hardware;
        if (hardware && (change.device == nullptr || !change.private_input.empty()))
        {
            return change.device == nullptr ? CryptoError::Unavailable : CryptoError::Unsupported;
        }
        Change placed    = change;
        placed.placement = hardware ? PlacementRequest::HardwareRequired : PlacementRequest::Software;
        Entry             fresh;
        const CryptoError made = make(placed, last->info.kind, last->info.algorithm, last->info.generation + 1U, fresh);
        if (made != CryptoError::None)
        {
            return made;
        }
        Entry& previous = entries[at];
        if (previous.info.state == KeyState::Current)
        {
            previous.info.state   = KeyState::Retired;
            previous.info.changed = change.now;
        }
        entries.push_back(std::move(fresh));
        std::rotate(entries.data() + at + 1U, entries.data() + entries.size() - 1U, entries.data() + entries.size());
        modified = true;
        return CryptoError::None;
    }
    case ChangeKind::Revoke:
    {
        if (last == nullptr || change.generation == 0U || change.generation > last->info.generation)
        {
            return CryptoError::NotFound;
        }
        Entry& revoked = entries[at - (last->info.generation - change.generation)];
        if (revoked.info.state != KeyState::Revoked)
        {
            const CryptoError forgotten = forget_device_key(change, revoked);
            if (forgotten != CryptoError::None)
            {
                return forgotten;
            }
            revoked.info.state   = KeyState::Revoked;
            revoked.info.changed = change.now;
            secure_zero(cont::Span<u8>(revoked.private_input));
            revoked.private_bytes = 0U;
            modified              = true;
        }
        return CryptoError::None;
    }
    case ChangeKind::Destroy:
    {
        if (last == nullptr)
        {
            return CryptoError::NotFound;
        }
        const usize first = at + 1U - last->info.generation;
        for (usize i = first; i <= at; ++i)
        {
            const CryptoError forgotten = forget_device_key(change, entries[i]);
            if (forgotten != CryptoError::None)
            {
                return forgotten;
            }
        }
        for (usize i = at + 1U; i > first; --i)
        {
            entries.erase(i - 1U);
        }
        modified = true;
        return CryptoError::None;
    }
    }
    return CryptoError::Unsupported;
}

// A device key is deleted before the store that drops it is saved (decision K12): a crash in between leaves an entry
// whose key no longer signs, which the change completes when repeated, never a revoked key that still signs.
CryptoError KeyStore::forget_device_key(const Change& change, const Entry& entry) noexcept
{
    if (entry.info.placement != KeyPlacement::Hardware || entry.info.state == KeyState::Revoked)
    {
        return CryptoError::None;
    }
    if (change.device == nullptr)
    {
        return CryptoError::Unavailable;
    }
    return change.device->forget(change.store_id, cont::ConstSpan<u8>(entry.private_input, entry.private_bytes));
}

CryptoError KeyStore::apply(Change& change) noexcept
{
    if (m_protector == nullptr)
    {
        return CryptoError::NotReady;
    }
    cont::Array<u8> created;
    change.device   = m_device;
    change.store_id = id();
    change.created  = &created;
    if (!valid_key_name(change.name))
    {
        return CryptoError::Malformed;
    }
    const cont::StringView directory(m_directory.data(), m_directory.size());
    fs::FileLock           lock;
    if (!lock.acquire(file_in(directory, kLockFileName)))
    {
        return CryptoError::StorageFailure;
    }

    // The newest store on disk, which another process may have changed since this one read it.
    Snapshot    snapshot;
    CryptoError error = read(directory, *m_protector, snapshot);
    if (error == CryptoError::None)
    {
        error = same_store(snapshot);
    }
    if (error != CryptoError::None)
    {
        return error;
    }
    // A master key replaced before an interruption is forgotten first, before anything else changes.
    const bool forgot_pending = !snapshot.pending.empty();
    if (forgot_pending)
    {
        error = m_protector->forget(id(), bytes_of(snapshot.pending));
        if (error != CryptoError::None)
        {
            return error;
        }
        snapshot.pending.clear();
    }

    bool              modified = false;
    const CryptoError outcome  = change_entries(change, snapshot.entries, modified);
    if (!modified && !forgot_pending)
    {
        return outcome;
    }
    error = commit(change, snapshot, modified, directory);
    if (error != CryptoError::None && !created.empty())
    {
        (void)m_device->forget(id(), bytes_of(created)); // a device key no saved store names
    }
    return error == CryptoError::None ? outcome : error;
}

CryptoError KeyStore::commit(const Change& change, Snapshot& snapshot, bool modified,
                             cont::StringView directory) noexcept
{
    CryptoError error = CryptoError::None;

    // Revocation and destruction replace the master key where the protector can forget the old one (decision K5):
    // the store is saved under the new key with the old reference pending, the old key forgotten, then the pending
    // reference cleared.
    const bool rekey =
        modified && (change.kind == ChangeKind::Revoke || change.kind == ChangeKind::Destroy) && m_protector->forgets();
    const cont::ConstSpan<Entry> entries(snapshot.entries.data(), snapshot.entries.size());
    ++snapshot.sequence;
    if (rekey)
    {
        u8              master[kKeyStoreMasterBytes];
        cont::Array<u8> reference;
        if (random_bytes(cont::Span<u8>(master)) != CryptoError::None)
        {
            return CryptoError::ProviderFailure;
        }
        error = m_protector->protect(id(), cont::ConstSpan<u8>(master), reference);
        if (error == CryptoError::None && (reference.empty() || reference.size() > kKeyReferenceMaxBytes))
        {
            error = CryptoError::Unsupported;
        }
        if (error == CryptoError::None)
        {
            error = save(entries, snapshot.sequence, bytes_of(reference), bytes_of(snapshot.reference),
                         cont::ConstSpan<u8>(master));
            if (error != CryptoError::None)
            {
                (void)m_protector->forget(id(), bytes_of(reference));
            }
        }
        if (error != CryptoError::None)
        {
            secure_zero(cont::Span<u8>(master));
            return error;
        }
        // The new store is durable: forget the old master key, then clear its reference. An interruption leaves
        // it pending, and the next change forgets it.
        if (m_protector->forget(id(), bytes_of(snapshot.reference)) == CryptoError::None &&
            save(entries, snapshot.sequence + 1U, bytes_of(reference), {}, cont::ConstSpan<u8>(master)) ==
                CryptoError::None)
        {
            ++snapshot.sequence;
        }
        std::memcpy(snapshot.master, master, kKeyStoreMasterBytes);
        secure_zero(cont::Span<u8>(master));
        snapshot.reference = std::move(reference);
    }
    else
    {
        error = save(entries, snapshot.sequence, bytes_of(snapshot.reference), {},
                     cont::ConstSpan<u8>(snapshot.master));
        if (error != CryptoError::None)
        {
            return error;
        }
    }
    adopt(*m_protector, directory, snapshot);
    return CryptoError::None;
}

CryptoError KeyStore::erase() noexcept
{
    if (m_protector == nullptr)
    {
        return CryptoError::NotReady;
    }
    const cont::StringView directory(m_directory.data(), m_directory.size());
    fs::FileLock           lock;
    if (!lock.acquire(file_in(directory, kLockFileName)))
    {
        return CryptoError::StorageFailure;
    }
    Snapshot    snapshot;
    CryptoError error = read(directory, *m_protector, snapshot);
    if (error == CryptoError::None)
    {
        error = same_store(snapshot);
    }
    // The device's keys go first, while the store still names them; then the master keys.
    Change change;
    change.device   = m_device;
    change.store_id = id();
    for (usize i = 0U; i < snapshot.entries.size() && error == CryptoError::None; ++i)
    {
        error = forget_device_key(change, snapshot.entries[i]);
    }
    if (error == CryptoError::None && !snapshot.pending.empty())
    {
        error = m_protector->forget(id(), bytes_of(snapshot.pending));
    }
    if (error == CryptoError::None)
    {
        error = m_protector->forget(id(), bytes_of(snapshot.reference));
    }
    if (error != CryptoError::None)
    {
        return error;
    }
    // The master key is forgotten first, so a file that cannot be removed no longer opens either.
    (void)fs::remove_file(file_in(directory, kStoreTempName)); // an interrupted save's, if any
    if (!fs::remove_file(file_in(directory, kStoreFileName)))
    {
        return CryptoError::StorageFailure;
    }
    close();
    return CryptoError::None;
}

CryptoError KeyStore::generate(cont::StringView name, AlgorithmId algorithm, PlacementRequest placement,
                               u64 now) noexcept
{
    Change change;
    change.kind      = ChangeKind::Add;
    change.name      = name;
    change.algorithm = algorithm;
    change.placement = placement;
    change.now       = now;
    return apply(change);
}

CryptoError KeyStore::import(cont::StringView name, AlgorithmId algorithm, cont::ConstSpan<u8> private_input,
                             u64 now) noexcept
{
    if (private_input.empty())
    {
        return CryptoError::BadLength;
    }
    Change change;
    change.kind          = ChangeKind::Add;
    change.name          = name;
    change.algorithm     = algorithm;
    change.private_input = private_input;
    change.now           = now;
    return apply(change);
}

CryptoError KeyStore::rotate(cont::StringView name, u64 now) noexcept
{
    Change change;
    change.kind = ChangeKind::Rotate;
    change.name = name;
    change.now  = now;
    return apply(change);
}

CryptoError KeyStore::rotate_import(cont::StringView name, cont::ConstSpan<u8> private_input, u64 now) noexcept
{
    if (private_input.empty())
    {
        return CryptoError::BadLength;
    }
    Change change;
    change.kind          = ChangeKind::Rotate;
    change.name          = name;
    change.private_input = private_input;
    change.now           = now;
    return apply(change);
}

CryptoError KeyStore::revoke(cont::StringView name, u32 generation, u64 now) noexcept
{
    Change change;
    change.kind       = ChangeKind::Revoke;
    change.name       = name;
    change.generation = generation;
    change.now        = now;
    return apply(change);
}

CryptoError KeyStore::destroy(cont::StringView name) noexcept
{
    Change change;
    change.kind = ChangeKind::Destroy;
    change.name = name;
    return apply(change);
}
} // namespace crd::crypto
