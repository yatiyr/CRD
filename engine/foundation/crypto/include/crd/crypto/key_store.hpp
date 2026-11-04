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
//               file atomically; open() and refresh() read under the same lock (which they create in an existing
//               directory), so readers see one whole save. A KeyStore is read-only after open(): its lookups and
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
inline constexpr usize kKeyDeviceReferenceMaxBytes = kKeyPrivateMaxBytes; // what names a device's key
inline constexpr usize kDevicePublicKeyMaxBytes    = 97U; // the longest device key's public key: a P-384 point
inline constexpr usize kTpmTctiMaxBytes            = 255U; // the longest tss2 TCTI configuration a TPM takes
inline constexpr usize kPkcs11ModuleMaxBytes       = 1023U; // the longest path of a PKCS #11 module
inline constexpr usize kPkcs11LabelBytes           = 32U;   // a PKCS #11 token's label (CK_TOKEN_INFO)
inline constexpr usize kPkcs11PinMaxBytes          = 128U;  // the longest PIN a token is opened with

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

// The operating system's protector for the calling user (decisions K2, K10 and K15): DPAPI on Windows ("dpapi": user
// scope, its entropy binding the store; it cannot forget), the freedesktop Secret Service on Linux ("secret-service":
// an item in the default collection that the store's identifier and a random handle find, its secret passed over the
// dh-ietf1024-sha256-aes128-cbc-pkcs7 transport; it forgets by deleting the item), the Android Keystore on Android
// ("android-keystore", decision K15: a non-exportable AES-256-GCM key of its own for each master key, which it wraps
// with the store's identifier as associated data, reached through the application's Java VM; it forgets by deleting
// the key), the Keychain on macOS ("keychain", decision K16: a generic password item of the data-protection keychain
// for a signed application, of the default file keychain otherwise; it forgets by deleting the item). Elsewhere every
// call answers Unavailable. available() is whether the vault answers now: a Linux session without a bus or a Secret
// Service, an Android process without a Java VM, or a macOS process without a keychain, has none.
class PlatformKeyProtector final : public KeyProtector
{
public:
    // `prompt_milliseconds` 0 (the default) never lets the vault ask the user (a locked collection is then Denied);
    // otherwise the Secret Service may show its unlock or confirmation prompt and is waited for that long, a dismissed
    // or unanswered prompt being Denied; the Keychain may show its prompt and is waited for until the user answers.
    // DPAPI asks the user nothing either way.
    explicit PlatformKeyProtector(u32 prompt_milliseconds = 0U) noexcept : m_prompt_milliseconds(prompt_milliseconds) {}

    [[nodiscard]] static bool available() noexcept;
    [[nodiscard]] u32         prompt_milliseconds() const noexcept { return m_prompt_milliseconds; }

    [[nodiscard]] cont::StringView name() const noexcept override;
    [[nodiscard]] KeyProtection    protection() const noexcept override { return KeyProtection::Platform; }
    [[nodiscard]] bool             forgets() const noexcept override;
    [[nodiscard]] CryptoError      protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                           cont::Array<u8>& reference) noexcept override;
    [[nodiscard]] CryptoError      recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                           cont::Span<u8> master) noexcept override;
    [[nodiscard]] CryptoError forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept override;

private:
    u32 m_prompt_milliseconds;
};

// Which TPM 2.0 a protector or device talks to. On Linux a tss2 TCTI configuration ("device:/dev/tpmrm0",
// "swtpm:host=127.0.0.1,port=2321"); empty (the default) lets tss2 search its own order, the kernel's resource manager
// first. Windows has one TPM, behind the Platform Crypto Provider, and ignores it. A configuration longer than
// kTpmTctiMaxBytes names no TPM: every call through it is Unavailable.
class TpmConnection
{
public:
    TpmConnection() noexcept = default;
    explicit TpmConnection(cont::StringView tcti) noexcept;

    // The configuration, zero-terminated; null when the default search applies or the configuration was too long.
    [[nodiscard]] const char* tcti() const noexcept { return m_length == 0U ? nullptr : m_tcti; }
    [[nodiscard]] bool        usable() const noexcept { return m_usable; }

private:
    char  m_tcti[kTpmTctiMaxBytes + 1U] = {};
    usize m_length                      = 0U;
    bool  m_usable                      = true;
};

// The TPM 2.0 protector for the calling user (decision K11). On Windows "tpm-pcp": a decrypt-only RSA-2048 key the TPM
// creates for each master key through the Platform Crypto Provider, which wraps it with RSA-OAEP (SHA-256, the store's
// identifier in the label); the reference holds the key's random handle and the wrapped key, never the public key. On
// Linux "tpm-tss2" (decision K13): the master key itself in an NV index of the TPM's owner hierarchy, written once over
// a salted, parameter-encrypted session and read only with an authorization derived from the reference's secret and
// the store's identifier; the reference holds the index's handle and that secret. Either forgets by deleting what the
// TPM holds, so a copy of the store file opens nowhere once its master key is replaced; a store it guards opens only on
// the machine whose TPM holds the key. Elsewhere every call answers Unavailable. available() is whether a TPM answers
// now (on Linux, one whose owner hierarchy has no authorization set: tss2 loaded, the TPM reached).
class TpmKeyProtector final : public KeyProtector
{
public:
    TpmKeyProtector() noexcept = default;
    explicit TpmKeyProtector(const TpmConnection& tpm) noexcept : m_tpm(tpm) {}

    [[nodiscard]] static bool available(const TpmConnection& tpm = TpmConnection()) noexcept;

    [[nodiscard]] cont::StringView name() const noexcept override;
    [[nodiscard]] KeyProtection    protection() const noexcept override { return KeyProtection::Hardware; }
    [[nodiscard]] bool             forgets() const noexcept override;
    [[nodiscard]] CryptoError      protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                           cont::Array<u8>& reference) noexcept override;
    [[nodiscard]] CryptoError      recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                           cont::Span<u8> master) noexcept override;
    [[nodiscard]] CryptoError forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept override;

private:
    TpmConnection m_tpm;
};

// A device that holds signature keys and signs with them without releasing them (decisions K8 and K12): a TPM, a
// PIV token, a Secure Enclave. A store records, for each key a device holds, the reference create() writes (a handle
// naming the device's key, never key material), sealed with the rest of the store, and gives it back to sign_digest()
// and forget() with the store's identifier, which the device binds its keys to. One device serves a store.
class KeyDevice
{
public:
    // A stable name: 1 to kKeyProtectorNameBytes of [a-z0-9-].
    [[nodiscard]] virtual cont::StringView name() const noexcept = 0;

    // A new key of `algorithm` (ECDSA P-256 or P-384) in the device: its reference (1 to kKeyDeviceReferenceMaxBytes)
    // into `reference` and its public key (signature_sizes(algorithm).public_key, an uncompressed SEC 1 point) into
    // `public_key`. Unsupported when the device holds no key of `algorithm`, Unavailable when it does not answer.
    [[nodiscard]] virtual CryptoError create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm,
                                             cont::Array<u8>& reference, cont::Span<u8> public_key) noexcept = 0;

    // Signs `digest` (the hash `algorithm` names) with the key `reference` names into `signature`: r || s, each the
    // curve's field length. Denied when the device holds no such key (forgotten, another store's). Safe to call from
    // several threads at once.
    [[nodiscard]] virtual CryptoError sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                                  AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                                  cont::Span<u8> signature) const noexcept = 0;

    // Deletes the key `reference` names; None when there is none.
    [[nodiscard]] virtual CryptoError forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept = 0;

protected:
    KeyDevice() noexcept                            = default;
    KeyDevice(const KeyDevice&) noexcept            = default;
    KeyDevice& operator=(const KeyDevice&) noexcept = default;
    KeyDevice(KeyDevice&&) noexcept                 = default;
    KeyDevice& operator=(KeyDevice&&) noexcept      = default;
    ~KeyDevice() noexcept                           = default;
};

// The TPM 2.0 as a key device for the calling user (decision K12). On Windows "tpm-pcp": a persisted sign-only ECDSA
// key the TPM creates through the Platform Crypto Provider, named for the store and a random handle (the reference: a
// format byte and the handle). On Linux "tpm-tss2" (decision K13): a sign-only ECDSA key the TPM creates under its
// storage root, its TPM-wrapped blob kept in an NV index of the owner hierarchy, the key and the index authorized by
// values derived from the reference's secret and the store's identifier (the reference: a format byte, the index's
// handle and that secret); forgetting deletes the index. Unsupported for a curve the TPM does not implement. Elsewhere
// every call answers Unavailable. available() is whether a TPM answers now.
class TpmKeyDevice final : public KeyDevice
{
public:
    TpmKeyDevice() noexcept = default;
    explicit TpmKeyDevice(const TpmConnection& tpm) noexcept : m_tpm(tpm) {}

    [[nodiscard]] static bool available(const TpmConnection& tpm = TpmConnection()) noexcept;

    [[nodiscard]] cont::StringView name() const noexcept override;
    [[nodiscard]] CryptoError      create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm, cont::Array<u8>& reference,
                                          cont::Span<u8> public_key) noexcept override;
    [[nodiscard]] CryptoError      sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                               AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                               cont::Span<u8> signature) const noexcept override;
    [[nodiscard]] CryptoError forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept override;

private:
    TpmConnection m_tpm;
};

// The operating system's key store as a key device for the calling application (decision K15). On Android
// "android-keystore": a sign-only ECDSA key the Android Keystore generates under an alias that names the store, the
// curve and a random handle (the reference: a format byte and the handle), kept in the device's TEE or StrongBox
// where it has one; it signs through the Java security API in the application's own Java VM and forgetting deletes
// the alias. On macOS "secure-enclave" (decision K16): a permanent P-256 key the Secure Enclave makes under an
// application tag that names the store and a random handle, which only a signed application with a keychain
// entitlement may keep; the enclave holds no P-384 key (Unsupported). Elsewhere every call answers Unavailable.
// available() is whether the key store answers now: a process without a Java VM (a plain executable) or a Mac
// without an enclave (a virtual machine) has none.
class PlatformKeyDevice final : public KeyDevice
{
public:
    [[nodiscard]] static bool available() noexcept;

    [[nodiscard]] cont::StringView name() const noexcept override;
    [[nodiscard]] CryptoError      create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm, cont::Array<u8>& reference,
                                          cont::Span<u8> public_key) noexcept override;
    [[nodiscard]] CryptoError      sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                               AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                               cont::Span<u8> signature) const noexcept override;
    [[nodiscard]] CryptoError forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept override;
};

// How a PKCS #11 token keeps keys (decision K14). Objects: a token that makes key objects of its own (an HSM, SoftHSM,
// OpenSC's cards), each named by a random CKA_ID and the store's CKA_LABEL. Piv: a PIV card through Yubico's ykcs11,
// whose keys live in fixed slots (CKA_ID 1 to 25), whose generation needs the security officer's login (the card's
// management key, or the PIN that protects it) and whose objects cannot be deleted, only overwritten.
enum class Pkcs11Profile : u8
{
    Objects,
    Piv,
};

// A PKCS #11 token (decision K14): its module, the path of the library a token's vendor ships (libsofthsm2.so,
// opensc-pkcs11.so, libykcs11.so, an HSM's), loaded at run time; the label C_InitToken gave it (1 to kPkcs11LabelBytes
// bytes, matched against every present token's blank-padded label: exactly one must match); the user PIN, empty only
// for a token with a protected authentication path (a PIN pad); and, for the Piv profile, the security officer's
// secret (ykcs11's CKU_SO login: the management key in hexadecimal, or the PIN of a PIN-protected management key),
// empty only with a protected authentication path. Both secrets are held in fixed storage and zeroed with the object.
// Unusable, so every call through it answers Unavailable, when a field is out of range.
class Pkcs11Token
{
public:
    Pkcs11Token() noexcept = default; // names no token
    Pkcs11Token(cont::StringView module, cont::StringView label, cont::ConstSpan<u8> pin) noexcept; // Objects
    Pkcs11Token(const Pkcs11Token& other) noexcept            = default;
    Pkcs11Token& operator=(const Pkcs11Token& other) noexcept = default;
    ~Pkcs11Token() noexcept;

    // A PIV card's token (the Piv profile).
    [[nodiscard]] static Pkcs11Token piv(cont::StringView module, cont::StringView label, cont::ConstSpan<u8> pin,
                                         cont::ConstSpan<u8> so_secret) noexcept;

    [[nodiscard]] const char*         module() const noexcept { return m_module; } // zero-terminated
    [[nodiscard]] cont::StringView    label() const noexcept { return cont::StringView(m_label, m_label_bytes); }
    [[nodiscard]] cont::ConstSpan<u8> pin() const noexcept { return cont::ConstSpan<u8>(m_pin, m_pin_bytes); }
    [[nodiscard]] cont::ConstSpan<u8> so_secret() const noexcept { return cont::ConstSpan<u8>(m_so, m_so_bytes); }
    [[nodiscard]] Pkcs11Profile       profile() const noexcept { return m_profile; }
    [[nodiscard]] bool                usable() const noexcept { return m_usable; }

private:
    char          m_module[kPkcs11ModuleMaxBytes + 1U] = {};
    char          m_label[kPkcs11LabelBytes]           = {};
    u8            m_pin[kPkcs11PinMaxBytes]            = {};
    u8            m_so[kPkcs11PinMaxBytes]             = {};
    usize         m_label_bytes                        = 0U;
    usize         m_pin_bytes                          = 0U;
    usize         m_so_bytes                           = 0U;
    Pkcs11Profile m_profile                            = Pkcs11Profile::Objects;
    bool          m_usable                             = false;
};

// A PKCS #11 token as a key device (decision K14): "pkcs11" for the Objects profile, "pkcs11-piv" for Piv. Each key is
// a sign-only ECDSA P-256 or P-384 pair the token generates and keeps inside (private, sensitive, never extractable,
// made on the token). Objects: its CKA_ID a random 16-byte handle and its CKA_LABEL naming the store, so a reference
// (a format byte and the handle) finds a key only under its own store's identifier; forgetting destroys both halves.
// Piv: the key takes the first retired key-management slot (CKA_ID 5 to 24, PIV slots 82 to 95) that holds no
// certificate, under the security officer's login (ykcs11 writes the slot's certificate with the key, which marks
// the slot taken); the reference is a format byte, the slot and a tag that binds the store's identifier, the slot and
// the slot's public key, so another store's reference, or a slot another key has since taken, is Denied; forgetting
// overwrites the slot with a key nobody holds and deletes its certificate, which frees the slot. Every call runs under
// one process-wide lock (a token runs one command at a time) in a session of its own, logging in when no session of
// the process has; a token is never logged out, since that would log out every session of the process, and closes its
// login with its last session. A refused PIN or security officer's secret is tried once per device: every later call
// through it that needs that login answers Denied without one, as does a token that reports that secret locked, on
// its final try or due for a change, so the device never spends a token's last attempt. Unsupported when the token
// lacks EC key generation, ECDSA or the curve, or refuses a sign-only, non-extractable key; StorageFailure when no
// slot or object is free; Unavailable when the module or the token cannot be reached. available() is whether the
// token answers now.
class Pkcs11KeyDevice final : public KeyDevice
{
public:
    explicit Pkcs11KeyDevice(const Pkcs11Token& token) noexcept : m_token(token) {}

    [[nodiscard]] static bool available(const Pkcs11Token& token) noexcept;

    [[nodiscard]] cont::StringView name() const noexcept override;
    [[nodiscard]] CryptoError      create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm, cont::Array<u8>& reference,
                                          cont::Span<u8> public_key) noexcept override;
    [[nodiscard]] CryptoError      sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                               AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                               cont::Span<u8> signature) const noexcept override;
    [[nodiscard]] CryptoError forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept override;

private:
    Pkcs11Token  m_token;
    mutable bool m_pin_refused = false; // under the process-wide PKCS #11 lock
    bool         m_so_refused  = false; // under the process-wide PKCS #11 lock
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

// A signature key a device holds, as a Signer (decision K12): the device signs the digest of each message, and each
// signature is verified under the stored public key before it is given out, so a device that signs with another key
// or faults is caught (ProviderFailure). DER Ecdsa-Sig-Value signatures, as SigningKey's. Plain data, copied freely;
// the device must outlive every copy.
class DeviceSigner final : public Signer
{
public:
    DeviceSigner() noexcept = default;

    [[nodiscard]] bool ready() const noexcept { return m_device != nullptr; }
    void               clear() noexcept;

    [[nodiscard]] AlgorithmId algorithm() const noexcept override { return m_algorithm; }
    [[nodiscard]] CryptoError public_key(cont::Span<u8> out) const noexcept override;
    [[nodiscard]] CryptoError sign(cont::ConstSpan<u8> message, cont::Span<u8> out,
                                   usize& written) const noexcept override;

private:
    friend class KeyStore;

    const KeyDevice* m_device = nullptr;
    AlgorithmId      m_algorithm{};
    u8               m_store_id[kKeyStoreIdBytes]             = {};
    u8               m_reference_bytes                        = 0U;
    u8               m_reference[kKeyDeviceReferenceMaxBytes] = {};
    u8               m_public_key[kDevicePublicKeyMaxBytes]   = {};
};

// What signs for a stored signature key. It hides the key's placement: code that signs takes a Signer.
class StoredSigner final : public Signer
{
public:
    StoredSigner() noexcept = default;

    [[nodiscard]] bool ready() const noexcept { return m_key.ready() || m_device.ready(); }
    void               clear() noexcept;

    // The generation it signs with.
    [[nodiscard]] const KeyInfo& info() const noexcept { return m_info; }
    // The device signer of a Hardware key; null for a key in the sealed file.
    [[nodiscard]] const DeviceSigner* device() const noexcept { return m_device.ready() ? &m_device : nullptr; }

    [[nodiscard]] AlgorithmId algorithm() const noexcept override { return m_info.algorithm; }
    [[nodiscard]] CryptoError public_key(cont::Span<u8> out) const noexcept override;
    [[nodiscard]] CryptoError sign(cont::ConstSpan<u8> message, cont::Span<u8> out,
                                   usize& written) const noexcept override;

private:
    friend class KeyStore;
    friend class TlsCredential;

    SigningKey   m_key;    // a Software key
    DeviceSigner m_device; // a Hardware key
    KeyInfo      m_info;
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
    // `protector`, its Hardware keys held by `device` (none: every key in the sealed file); both must outlive the
    // store, and the device every signer it gives. Exists when the directory holds a store already.
    [[nodiscard]] CryptoError create(cont::StringView directory, KeyProtector& protector,
                                     KeyDevice* device = nullptr) noexcept;

    // The store in `directory`: NotFound without one, Unsupported when `protector` is not the protector that
    // created it, Damaged when its file fails its storage checks or framing, Forged when it does not authenticate
    // under the master key the protector recovers, Malformed when its authentic contents break the store's rules.
    // Without `device` its Hardware keys are listed but cannot sign (Unavailable) and changes to them are refused.
    [[nodiscard]] CryptoError open(cont::StringView directory, KeyProtector& protector,
                                   KeyDevice* device = nullptr) noexcept;

    // Reads the newest save of the open store (another process's changes).
    [[nodiscard]] CryptoError refresh() noexcept;
    void                      close() noexcept;

    [[nodiscard]] bool                ready() const noexcept { return m_protector != nullptr; }
    [[nodiscard]] cont::ConstSpan<u8> id() const noexcept { return cont::ConstSpan<u8>(m_id, kKeyStoreIdBytes); }
    [[nodiscard]] KeyProtection       protection() const noexcept;

    // Generation 1 of a new key `name` from the provider's randomness, or in the store's device (decision K8): a
    // signature key whose placement asks for one, where the device holds its algorithm. HardwareRequired is
    // Unsupported without such a device. Exists when the name is taken.
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

    // Erases the private material of `generation` of `name` (a Hardware key's device key is deleted first) and
    // replaces the master key (decision K5). A revoked Current generation leaves the name without one until it is
    // rotated.
    [[nodiscard]] CryptoError revoke(cont::StringView name, u32 generation, u64 now) noexcept;

    // Removes `name` and every generation of it (deleting its device keys first), replacing the master key.
    [[nodiscard]] CryptoError destroy(cont::StringView name) noexcept;

    // Erases the whole store and closes it: the device deletes every key it holds for the store, the protector
    // forgets its master key (and a pending one), so no copy of the store file opens again where it can (forgets()),
    // then the store file is removed. The directory and its lock file stay, since another process may hold the lock;
    // create() may make a new store there.
    [[nodiscard]] CryptoError erase() noexcept;

    // Every generation, ordered by name, then generation.
    [[nodiscard]] usize          size() const noexcept { return m_entries.size(); }
    [[nodiscard]] const KeyInfo& key(usize index) const noexcept { return m_entries[index].info; }

    // Generation `generation` of `name`, or its Current generation for 0; null when there is none.
    [[nodiscard]] const KeyInfo* find(cont::StringView name, u32 generation = 0U) const noexcept;

    // The public key of an asymmetric generation, Revoked ones included (`out` exactly its algorithm's length).
    [[nodiscard]] CryptoError public_key(cont::StringView name, u32 generation, cont::Span<u8> out) const noexcept;

    // The Current generation of a signature key, ready to sign (NotFound without one, Revoked when it was revoked,
    // Unavailable for a Hardware key of a store opened without its device).
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
    [[nodiscard]] static CryptoError  forget_device_key(const Change& change, const Entry& entry) noexcept;

    [[nodiscard]] const Entry* entry(cont::StringView name, u32 generation) const noexcept;
    [[nodiscard]] CryptoError  usable(cont::StringView name, u32 generation, KeyKind kind,
                                      const Entry*& out) const noexcept;
    [[nodiscard]] CryptoError  read(cont::StringView directory, KeyProtector& protector,
                                    Snapshot& out) const noexcept;
    [[nodiscard]] CryptoError  read_locked(cont::StringView directory, KeyProtector& protector,
                                           Snapshot& out) const noexcept;
    [[nodiscard]] CryptoError  same_store(const Snapshot& snapshot) const noexcept;
    void                       adopt(KeyProtector& protector, cont::StringView directory, Snapshot& snapshot) noexcept;
    [[nodiscard]] CryptoError  apply(Change& change) noexcept;
    [[nodiscard]] CryptoError  commit(const Change& change, Snapshot& snapshot, bool modified,
                                      cont::StringView directory) noexcept;
    [[nodiscard]] CryptoError  save(cont::ConstSpan<Entry> entries, u64 sequence, cont::ConstSpan<u8> reference,
                                    cont::ConstSpan<u8> pending, cont::ConstSpan<u8> master) const noexcept;

    KeyProtector*      m_protector = nullptr;
    KeyDevice*         m_device    = nullptr;
    cont::String       m_directory;
    u8                 m_id[kKeyStoreIdBytes]         = {};
    u64                m_sequence                     = 0U;
    u8                 m_master[kKeyStoreMasterBytes] = {};
    cont::Array<u8>    m_reference;
    cont::Array<Entry> m_entries;
};
} // namespace crd::crypto
