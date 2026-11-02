#pragma once

// crd-crypto -- the key store (NET.2; contract: docs/design/network-foundation.md#key-store).
//
// A principal's long-term keys and the secrets a fleet shares, kept at rest in one sealed file under a master key that
// a protector guards: the operating system's (DPAPI on Windows), a hardware one, or the application's own. Rules:
//
//   material    signature, agreement and KEM private keys never leave the store: they are generated in it (or
//               imported once) and used through StoredSigner and the provider key objects it loads. Secrets
//               (AEAD, HKDF and PRF keys a fleet shares) are read out, since every server of the fleet holds them.
//   generations a name holds generations 1, 2, ...: rotate() adds the next as Current and retires the previous one,
//               which still opens and verifies but never signs; revoke() erases a generation's private material and
//               keeps its public key and identifier. Revocation and destruction replace the master key, so a copy of
//               an earlier store file cannot be opened once the protector forgets the old one (decision K5).
//   writers     every change runs under the store's file lock on the newest store on disk and replaces the store
//               file atomically; readers see one whole save. A KeyStore is read-only after open(): its lookups and
//               loads may run on several threads at once, its changes need the caller's exclusive access.
//   secrets     the master key and every private input are zeroed when the store is closed or destroyed.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>

namespace crd::crypto
{
inline constexpr usize kKeyStoreMasterBytes   = 32U;
inline constexpr usize kKeyStoreIdBytes       = 16U;
inline constexpr usize kKeyNameMaxBytes       = 64U;
inline constexpr usize kKeyStoreMaxKeys       = 4096U; // generations of every name together
inline constexpr usize kKeyProtectorNameBytes = 32U;
inline constexpr usize kKeyReferenceMaxBytes  = 4096U;
inline constexpr usize kKeyPrivateMaxBytes    = 64U; // the longest private input: an ML-KEM seed d || z

// What guards a store's master key, weakest first (decision K2).
enum class KeyProtection : u8
{
    Application, // the application's own protector: a key it holds, a cloud key service, a deployment secret
    Platform,    // the operating system's per-user vault (DPAPI, the Secret Service, the Keychain)
    Hardware,    // sealed to a device (a TPM, a Secure Enclave, a hardware keystore)
};

// "application", "platform", "hardware".
[[nodiscard]] cont::StringView key_protection_name(KeyProtection protection) noexcept;

// The guardian of a store's master key. A store records the protector's name and the reference protect() returns,
// and gives the reference back to recover() and forget(). Each call names the store (`store_id`, kKeyStoreIdBytes),
// which a protector binds its reference to, so a reference copied into another store does not recover there.
class KeyProtector
{
public:
    // The name recorded in the store (1 to kKeyProtectorNameBytes of [a-z0-9-]): a store opens only with a protector
    // of the name that created it.
    [[nodiscard]] virtual cont::StringView name() const noexcept = 0;
    [[nodiscard]] virtual KeyProtection    protection() const noexcept = 0;

    // Whether forget() destroys what recovers a master key, so a revoked key in an earlier copy of the store file is
    // gone for good; a protector whose references recover forever (DPAPI) cannot, and says so.
    [[nodiscard]] virtual bool forgets() const noexcept = 0;

    // Guards `master` (kKeyStoreMasterBytes) and writes the reference that recovers it (1 to kKeyReferenceMaxBytes)
    // into `reference`.
    [[nodiscard]] virtual CryptoError protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                              cont::Array<u8>& reference) noexcept = 0;

    // Recovers the master key `reference` guards into `master` (kKeyStoreMasterBytes); on any error `master` is
    // zeroed. Denied when the protector will not (another user's reference, a locked vault, a refused prompt).
    [[nodiscard]] virtual CryptoError recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                              cont::Span<u8> master) noexcept = 0;

    // Destroys what recovers the master key behind `reference`, where the protector can (forgets()); None when there
    // is nothing to destroy.
    [[nodiscard]] virtual CryptoError forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept = 0;

protected:
    KeyProtector() noexcept                               = default;
    KeyProtector(const KeyProtector&) noexcept            = default;
    KeyProtector& operator=(const KeyProtector&) noexcept = default;
    KeyProtector(KeyProtector&&) noexcept                 = default;
    KeyProtector& operator=(KeyProtector&&) noexcept      = default;
    ~KeyProtector() noexcept                              = default;
};

// The operating system's protector for the calling user (decision K2): DPAPI on Windows ("dpapi": user scope, no
// prompt, its entropy binding the store; it cannot forget). Elsewhere every call answers Unavailable until that
// platform's protector lands (NET.2's remaining key store clause).
class PlatformKeyProtector final : public KeyProtector
{
public:
    [[nodiscard]] static bool available() noexcept;

    [[nodiscard]] cont::StringView name() const noexcept override;
    [[nodiscard]] KeyProtection    protection() const noexcept override { return KeyProtection::Platform; }
    [[nodiscard]] bool             forgets() const noexcept override;
    [[nodiscard]] CryptoError      protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                           cont::Array<u8>& reference) noexcept override;
    [[nodiscard]] CryptoError      recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                           cont::Span<u8> master) noexcept override;
    [[nodiscard]] CryptoError forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept override;
};

enum class KeyKind : u8
{
    Signature, // signature_sizes(algorithm).seed bytes of private input
    Agreement, // agreement_sizes(algorithm).private_key bytes
    Kem,       // kem_sizes(algorithm).seed bytes
    Secret,    // an AEAD key (aead_key_bytes), an HKDF secret (hkdf_hash_bytes) or a PRF key (kPrfKeyBytes)
};

// The kind an algorithm's keys are kept as; false for an algorithm the store does not keep (the hybrid groups, which
// exist only inside a handshake).
[[nodiscard]] bool key_kind_of(AlgorithmId algorithm, KeyKind& kind) noexcept;

enum class KeyState : u8
{
    Current, // the generation that signs, and that a fleet seals with
    Retired, // an earlier generation: opens, decapsulates and verifies, never signs
    Revoked, // its private material is erased; its public key and identifier remain
};

// Where a key is kept (decision K8): a signature key may live in a device that signs without releasing it; every
// other kind, and every key without such a device, is kept in the sealed file.
enum class KeyPlacement : u8
{
    Software,
    Hardware,
};

enum class PlacementRequest : u8
{
    Software,          // in the sealed file
    HardwarePreferred, // in a device when one holds the algorithm, otherwise in the sealed file
    HardwareRequired,  // in a device, or Unsupported
};

struct KeyInfo
{
    u32          generation = 0U;
    AlgorithmId  algorithm{};
    KeyKind      kind      = KeyKind::Signature;
    KeyState     state     = KeyState::Current;
    KeyPlacement placement = KeyPlacement::Software;
    KeyId        id;              // key_id_of(public key); a secret's is HKDF-SHA256 of it under "cerid key id"
    u64          created = 0U;    // the times the caller gave, in its unit (Unix seconds by convention)
    u64          changed = 0U;    // when it was retired or revoked; `created` while Current
    u8           name_bytes = 0U;
    char         name_text[kKeyNameMaxBytes] = {};

    [[nodiscard]] cont::StringView name() const noexcept { return cont::StringView(name_text, name_bytes); }
};

// Whether `name` is a key name: 1 to kKeyNameMaxBytes of [a-z0-9._-], starting with a letter or digit.
[[nodiscard]] bool valid_key_name(cont::StringView name) noexcept;

// What signs for a stored signature key. It hides the key's placement: code that signs takes a Signer.
class StoredSigner final : public Signer
{
public:
    StoredSigner() noexcept = default;

    [[nodiscard]] bool ready() const noexcept { return m_key.ready(); }
    void               clear() noexcept;

    // The generation it signs with.
    [[nodiscard]] const KeyInfo& info() const noexcept { return m_info; }

    [[nodiscard]] AlgorithmId algorithm() const noexcept override { return m_key.algorithm(); }
    [[nodiscard]] CryptoError public_key(cont::Span<u8> out) const noexcept override;
    [[nodiscard]] CryptoError sign(cont::ConstSpan<u8> message, cont::Span<u8> out,
                                   usize& written) const noexcept override;

private:
    friend class KeyStore;
    friend class TlsCredential;

    SigningKey m_key;
    KeyInfo    m_info;
};

class KeyStore
{
public:
    KeyStore() noexcept = default;
    ~KeyStore() noexcept { close(); }
    KeyStore(const KeyStore&)            = delete;
    KeyStore& operator=(const KeyStore&) = delete;
    KeyStore(KeyStore&&)                 = delete;
    KeyStore& operator=(KeyStore&&)      = delete;

    // A new, empty store in `directory` (UTF-8; created owner-only with its parents), its master key guarded by
    // `protector`, which must outlive the store. Exists when the directory holds a store already.
    [[nodiscard]] CryptoError create(cont::StringView directory, KeyProtector& protector) noexcept;

    // The store in `directory`: NotFound without one, Unsupported when `protector` is not the protector that
    // created it, Damaged when its file fails its storage checks or framing, Forged when it does not authenticate
    // under the master key the protector recovers, Malformed when its authentic contents break the store's rules.
    [[nodiscard]] CryptoError open(cont::StringView directory, KeyProtector& protector) noexcept;

    // Reads the newest save of the open store (another process's changes).
    [[nodiscard]] CryptoError refresh() noexcept;
    void                      close() noexcept;

    [[nodiscard]] bool                ready() const noexcept { return m_protector != nullptr; }
    [[nodiscard]] cont::ConstSpan<u8> id() const noexcept { return cont::ConstSpan<u8>(m_id, kKeyStoreIdBytes); }
    [[nodiscard]] KeyProtection       protection() const noexcept;

    // Generation 1 of a new key `name` from the provider's randomness (Exists when the name is taken).
    [[nodiscard]] CryptoError generate(cont::StringView name, AlgorithmId algorithm, PlacementRequest placement,
                                       u64 now) noexcept;
    // Generation 1 of a new key `name` from `private_input` (the from_seed, from_private or secret bytes of its kind,
    // exactly that long): a key made offline, or a secret the fleet already shares. Kept in the sealed file.
    [[nodiscard]] CryptoError import(cont::StringView name, AlgorithmId algorithm, cont::ConstSpan<u8> private_input,
                                     u64 now) noexcept;

    // The next generation of `name` (same algorithm and placement) as Current, the Current one Retired: fresh, or
    // from `private_input` (rotate_import, a secret the fleet rotates together).
    [[nodiscard]] CryptoError rotate(cont::StringView name, u64 now) noexcept;
    [[nodiscard]] CryptoError rotate_import(cont::StringView name, cont::ConstSpan<u8> private_input, u64 now) noexcept;

    // Erases the private material of `generation` of `name` and replaces the master key (decision K5). A revoked
    // Current generation leaves the name without one until it is rotated.
    [[nodiscard]] CryptoError revoke(cont::StringView name, u32 generation, u64 now) noexcept;

    // Removes `name` and every generation of it, replacing the master key.
    [[nodiscard]] CryptoError destroy(cont::StringView name) noexcept;

    // Every generation, ordered by name, then generation.
    [[nodiscard]] usize          size() const noexcept { return m_entries.size(); }
    [[nodiscard]] const KeyInfo& key(usize index) const noexcept { return m_entries[index].info; }

    // Generation `generation` of `name`, or its Current generation for 0; null when there is none.
    [[nodiscard]] const KeyInfo* find(cont::StringView name, u32 generation = 0U) const noexcept;

    // The public key of an asymmetric generation, Revoked ones included (`out` exactly its algorithm's length).
    [[nodiscard]] CryptoError public_key(cont::StringView name, u32 generation, cont::Span<u8> out) const noexcept;

    // The Current generation of a signature key, ready to sign (NotFound without one, Revoked when it was revoked).
    [[nodiscard]] CryptoError signer(cont::StringView name, StoredSigner& out) const noexcept;

    // A Current or Retired generation (0: the Current one) loaded into a provider key object, or a secret's bytes
    // into `out` (exactly its length; the caller zeroes it). Revoked for a revoked generation; Unsupported for a key
    // of another kind.
    [[nodiscard]] CryptoError agreement_key(cont::StringView name, u32 generation, AgreementKey& out) const noexcept;
    [[nodiscard]] CryptoError kem_key(cont::StringView name, u32 generation, KemKey& out) const noexcept;
    [[nodiscard]] CryptoError secret(cont::StringView name, u32 generation, cont::Span<u8> out) const noexcept;

private:
    struct Entry
    {
        Entry() noexcept = default;
        ~Entry() noexcept;
        Entry(const Entry&)                = delete;
        Entry& operator=(const Entry&)     = delete;
        Entry(Entry&&) noexcept            = default;
        Entry& operator=(Entry&&) noexcept = default;

        KeyInfo         info;
        u8              private_bytes                      = 0U;
        u8              private_input[kKeyPrivateMaxBytes] = {}; // zeroed on destruction
        cont::Array<u8> public_key;
    };

    // One save of the store as read, before it is adopted.
    struct Snapshot
    {
        Snapshot() noexcept = default;
        ~Snapshot() noexcept;
        Snapshot(const Snapshot&)            = delete;
        Snapshot& operator=(const Snapshot&) = delete;

        u8                 id[kKeyStoreIdBytes]         = {};
        u64                sequence                     = 0U;
        u8                 master[kKeyStoreMasterBytes] = {}; // zeroed on destruction
        cont::Array<u8>    reference;
        cont::Array<u8>    pending;
        cont::Array<Entry> entries;
    };

    struct Change;

    [[nodiscard]] static const Entry* last_of(cont::ConstSpan<Entry> entries, cont::StringView name) noexcept;
    [[nodiscard]] static CryptoError  decode(cont::ConstSpan<u8> body, cont::Array<Entry>& entries) noexcept;
    [[nodiscard]] static CryptoError  make(const Change& change, KeyKind kind, AlgorithmId algorithm, u32 generation,
                                           Entry& out) noexcept;
    [[nodiscard]] static CryptoError  change_entries(const Change& change, cont::Array<Entry>& entries,
                                                     bool& modified) noexcept;

    [[nodiscard]] const Entry* entry(cont::StringView name, u32 generation) const noexcept;
    [[nodiscard]] CryptoError  usable(cont::StringView name, u32 generation, KeyKind kind,
                                      const Entry*& out) const noexcept;
    [[nodiscard]] CryptoError  read(cont::StringView directory, KeyProtector& protector,
                                    Snapshot& out) const noexcept;
    [[nodiscard]] CryptoError  same_store(const Snapshot& snapshot) const noexcept;
    void                       adopt(KeyProtector& protector, cont::StringView directory, Snapshot& snapshot) noexcept;
    [[nodiscard]] CryptoError  apply(const Change& change) noexcept;
    [[nodiscard]] CryptoError  save(cont::ConstSpan<Entry> entries, u64 sequence, cont::ConstSpan<u8> reference,
                                    cont::ConstSpan<u8> pending, cont::ConstSpan<u8> master) const noexcept;

    KeyProtector*      m_protector = nullptr;
    cont::String       m_directory;
    u8                 m_id[kKeyStoreIdBytes]         = {};
    u64                m_sequence                     = 0U;
    u8                 m_master[kKeyStoreMasterBytes] = {};
    cont::Array<u8>    m_reference;
    cont::Array<Entry> m_entries;
};
} // namespace crd::crypto
