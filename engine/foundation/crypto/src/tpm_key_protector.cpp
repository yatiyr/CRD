// crd-crypto -- the TPM 2.0 key protector and key device (NET.2; contract: docs/design/network-foundation.md#key-store,
// decisions K11 and K12): on Windows, through the Platform Crypto Provider, a decrypt-only RSA-2048 key the TPM holds
// for one master key, and sign-only ECDSA keys it holds for a store's Hardware signature keys. Linux's, through tss2,
// are tpm_tss2.cpp (decision K13); elsewhere every call is Unavailable.

#include "byte_spans.hpp"

#include <crd/core/platform.hpp>
#include <crd/crypto/key_store.hpp>

#include <cstring>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ncrypt.h>
#endif

namespace crd::crypto
{
TpmConnection::TpmConnection(cont::StringView tcti) noexcept
{
    if (tcti.size() > kTpmTctiMaxBytes)
    {
        m_usable = false;
        return;
    }
    if (tcti.empty())
    {
        return; // tss2's default search
    }
    std::memcpy(m_tcti, tcti.data(), tcti.size());
    m_tcti[tcti.size()] = '\0';
    m_length            = tcti.size();
}

#if CRD_OS_WINDOWS
namespace
{
constexpr u8    kReferenceFormat      = 1U;
constexpr usize kHandleBytes          = 16U;
constexpr usize kModulusBits          = 2048U;
constexpr usize kWrappedBytes         = kModulusBits / 8U;
constexpr usize kReferenceBytes       = 1U + kHandleBytes + kWrappedBytes; // the format, the key's handle, the wrapped key
constexpr usize kDeviceReferenceBytes = 1U + kHandleBytes;                 // the format, the signing key's handle
constexpr char  kLabelPrefix[]        = "cerid master 1";
constexpr char  kNamePrefix[]         = "Cerid key store ";
constexpr char  kSignerPrefix[]       = "Cerid signer    "; // as long as kNamePrefix: the names share one length
static_assert(sizeof(kSignerPrefix) == sizeof(kNamePrefix), "protector and device key names share one length");

// The OAEP label: the prefix, the store's identifier and a closing zero byte. A TPM ends a label that does not end in
// zero with one, so the label carries its own and encryption and decryption agree wherever they run; it holds at most
// a digest of the TPM's largest hash (TPM_RC_SIZE otherwise), 32 bytes on a TPM with SHA-256 alone.
constexpr usize kLabelBytes = sizeof(kLabelPrefix) - 1U + kKeyStoreIdBytes + 1U;
static_assert(kLabelBytes <= 32U, "a TPM with SHA-256 alone takes labels of up to 32 bytes");
// The persisted key's name: the prefix, the store's identifier and the handle in hex, a space between them.
constexpr usize kNameChars = sizeof(kNamePrefix) - 1U + 2U * kKeyStoreIdBytes + 1U + 2U * kHandleBytes + 1U;

void label_of(cont::ConstSpan<u8> store_id, u8 (&label)[kLabelBytes]) noexcept
{
    std::memcpy(label, kLabelPrefix, sizeof(kLabelPrefix) - 1U);
    std::memcpy(label + sizeof(kLabelPrefix) - 1U, store_id.data(), kKeyStoreIdBytes);
    label[kLabelBytes - 1U] = 0U;
}

void name_of(const char (&prefix)[sizeof(kNamePrefix)], cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle,
             wchar_t (&name)[kNameChars]) noexcept
{
    wchar_t* out = name;
    for (usize i = 0U; i + 1U < sizeof(kNamePrefix); ++i)
    {
        *out++ = static_cast<wchar_t>(prefix[i]);
    }
    out    = detail::hex_into(store_id, out);
    *out++ = L' ';
    out    = detail::hex_into(handle, out);
    *out   = L'\0';
}

// A reference this protector wrote: its format byte, the key's handle and the wrapped master key.
[[nodiscard]] bool well_formed(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    return store_id.size() == kKeyStoreIdBytes && reference.size() == kReferenceBytes &&
           reference[0] == kReferenceFormat;
}

// One NCrypt handle, freed when it goes.
class Handle
{
public:
    Handle() noexcept = default;
    ~Handle() noexcept { reset(); }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] NCRYPT_HANDLE* put() noexcept { return &m_handle; }
    [[nodiscard]] NCRYPT_HANDLE  get() const noexcept { return m_handle; }
    // Gives the handle up (NCryptDeleteKey frees the key's handle itself).
    [[nodiscard]] NCRYPT_HANDLE release() noexcept
    {
        const NCRYPT_HANDLE handle = m_handle;
        m_handle                   = 0U;
        return handle;
    }
    void reset() noexcept
    {
        if (m_handle != 0U)
        {
            (void)NCryptFreeObject(m_handle);
            m_handle = 0U;
        }
    }

private:
    NCRYPT_HANDLE m_handle = 0U;
};

[[nodiscard]] bool open_provider(Handle& provider) noexcept
{
    return NCryptOpenStorageProvider(reinterpret_cast<NCRYPT_PROV_HANDLE*>(provider.put()),
                                     MS_PLATFORM_CRYPTO_PROVIDER, 0U) == ERROR_SUCCESS;
}

// The persisted key `prefix` names for the store and `handle`: None, Denied when there is none (forgotten, or another
// store's), Unavailable when the provider does not answer.
[[nodiscard]] CryptoError open_key(const char (&prefix)[sizeof(kNamePrefix)], cont::ConstSpan<u8> store_id,
                                   cont::ConstSpan<u8> handle, Handle& key) noexcept
{
    Handle provider;
    if (!open_provider(provider))
    {
        return CryptoError::Unavailable;
    }
    wchar_t name[kNameChars];
    name_of(prefix, store_id, handle, name);
    const SECURITY_STATUS status =
        NCryptOpenKey(static_cast<NCRYPT_PROV_HANDLE>(provider.get()), reinterpret_cast<NCRYPT_KEY_HANDLE*>(key.put()),
                      name, 0U, NCRYPT_SILENT_FLAG);
    if (status == NTE_BAD_KEYSET)
    {
        return CryptoError::Denied;
    }
    return status == ERROR_SUCCESS ? CryptoError::None : CryptoError::Unavailable;
}

[[nodiscard]] BCRYPT_OAEP_PADDING_INFO oaep_of(u8 (&label)[kLabelBytes]) noexcept
{
    BCRYPT_OAEP_PADDING_INFO padding{};
    padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;
    padding.pbLabel  = label;
    padding.cbLabel  = static_cast<ULONG>(kLabelBytes);
    return padding;
}

// A new decrypt-only RSA key in the TPM named for the store and `handle`, which wraps `master` into `wrapped`.
[[nodiscard]] CryptoError wrap(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle, cont::ConstSpan<u8> master,
                               cont::Span<u8> wrapped) noexcept
{
    Handle provider;
    if (!open_provider(provider))
    {
        return CryptoError::Unavailable;
    }
    wchar_t name[kNameChars];
    name_of(kNamePrefix, store_id, handle, name);
    Handle key;
    if (NCryptCreatePersistedKey(static_cast<NCRYPT_PROV_HANDLE>(provider.get()),
                                 reinterpret_cast<NCRYPT_KEY_HANDLE*>(key.put()), BCRYPT_RSA_ALGORITHM, name, 0U,
                                 0U) != ERROR_SUCCESS)
    {
        return CryptoError::Unavailable;
    }
    const auto key_handle = static_cast<NCRYPT_KEY_HANDLE>(key.get());
    DWORD      bits       = static_cast<DWORD>(kModulusBits);
    DWORD      usage      = NCRYPT_ALLOW_DECRYPT_FLAG;
    if (NCryptSetProperty(key_handle, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof(bits), 0U) !=
            ERROR_SUCCESS ||
        NCryptSetProperty(key_handle, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&usage), sizeof(usage),
                          0U) != ERROR_SUCCESS ||
        NCryptFinalizeKey(key_handle, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS)
    {
        return CryptoError::Unavailable; // an unfinalised key is not persisted
    }
    u8 label[kLabelBytes];
    label_of(store_id, label);
    BCRYPT_OAEP_PADDING_INFO padding = oaep_of(label);
    u8                       secret[kKeyStoreMasterBytes];
    std::memcpy(secret, master.data(), kKeyStoreMasterBytes);
    DWORD                 written = 0U;
    const SECURITY_STATUS status =
        NCryptEncrypt(key_handle, secret, static_cast<DWORD>(kKeyStoreMasterBytes), &padding, wrapped.data(),
                      static_cast<DWORD>(wrapped.size()), &written, NCRYPT_PAD_OAEP_FLAG | NCRYPT_SILENT_FLAG);
    secure_zero(cont::Span<u8>(secret));
    if (status == ERROR_SUCCESS && written == wrapped.size())
    {
        return CryptoError::None;
    }
    (void)NCryptDeleteKey(static_cast<NCRYPT_KEY_HANDLE>(key.release()), 0U);
    return CryptoError::ProviderFailure;
}

// Deletes the persisted key `prefix` names for the store and `handle`; None when there is none.
[[nodiscard]] CryptoError delete_key(const char (&prefix)[sizeof(kNamePrefix)], cont::ConstSpan<u8> store_id,
                                     cont::ConstSpan<u8> handle) noexcept
{
    Handle            key;
    const CryptoError result = open_key(prefix, store_id, handle, key);
    if (result == CryptoError::Denied)
    {
        return CryptoError::None; // forgotten already
    }
    if (result != CryptoError::None)
    {
        return result;
    }
    // The provider takes no flag on deletion (NCRYPT_SILENT_FLAG is NTE_BAD_FLAGS there).
    return NCryptDeleteKey(static_cast<NCRYPT_KEY_HANDLE>(key.release()), 0U) == ERROR_SUCCESS
               ? CryptoError::None
               : CryptoError::StorageFailure;
}

// The provider's name of an ECDSA curve the device keeps keys on; null for any other algorithm.
[[nodiscard]] LPCWSTR curve_of(AlgorithmId algorithm) noexcept
{
    switch (algorithm)
    {
    case AlgorithmId::EcdsaSecp256r1Sha256:
        return BCRYPT_ECDSA_P256_ALGORITHM;
    case AlgorithmId::EcdsaSecp384r1Sha384:
        return BCRYPT_ECDSA_P384_ALGORITHM;
    default:
        return nullptr;
    }
}

// The field length of the curve `algorithm` names (its coordinates, r and s).
[[nodiscard]] usize field_of(AlgorithmId algorithm) noexcept
{
    return algorithm == AlgorithmId::EcdsaSecp384r1Sha384 ? 48U : 32U;
}

// The public key of a device key as an uncompressed SEC 1 point (0x04, x, y), from the provider's ECC public blob.
[[nodiscard]] CryptoError export_point(NCRYPT_KEY_HANDLE key, AlgorithmId algorithm, cont::Span<u8> point) noexcept
{
    const usize field = field_of(algorithm);
    u8          blob[sizeof(BCRYPT_ECCKEY_BLOB) + 2U * 48U];
    DWORD       written = 0U;
    if (point.size() != 1U + 2U * field ||
        NCryptExportKey(key, 0U, BCRYPT_ECCPUBLIC_BLOB, nullptr, blob, static_cast<DWORD>(sizeof(blob)), &written,
                        NCRYPT_SILENT_FLAG) != ERROR_SUCCESS)
    {
        return CryptoError::ProviderFailure;
    }
    BCRYPT_ECCKEY_BLOB header{};
    std::memcpy(&header, blob, sizeof(header));
    if (written != sizeof(header) + 2U * field || header.cbKey != field)
    {
        return CryptoError::ProviderFailure;
    }
    point[0] = 0x04U;
    std::memcpy(point.data() + 1U, blob + sizeof(header), 2U * field);
    return CryptoError::None;
}

// A new persisted sign-only ECDSA key in the TPM named for the store and `handle`, and its public key.
[[nodiscard]] CryptoError create_signer(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle,
                                        AlgorithmId algorithm, cont::Span<u8> point) noexcept
{
    Handle provider;
    if (!open_provider(provider))
    {
        return CryptoError::Unavailable;
    }
    wchar_t name[kNameChars];
    name_of(kSignerPrefix, store_id, handle, name);
    Handle                key;
    const SECURITY_STATUS created =
        NCryptCreatePersistedKey(static_cast<NCRYPT_PROV_HANDLE>(provider.get()),
                                 reinterpret_cast<NCRYPT_KEY_HANDLE*>(key.put()), curve_of(algorithm), name, 0U, 0U);
    if (created != ERROR_SUCCESS)
    {
        return created == NTE_NOT_SUPPORTED || created == NTE_BAD_ALGID ? CryptoError::Unsupported
                                                                         : CryptoError::Unavailable;
    }
    const auto      key_handle = static_cast<NCRYPT_KEY_HANDLE>(key.get());
    DWORD           usage      = NCRYPT_ALLOW_SIGNING_FLAG;
    SECURITY_STATUS status =
        NCryptSetProperty(key_handle, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&usage), sizeof(usage), 0U);
    if (status == ERROR_SUCCESS)
    {
        status = NCryptFinalizeKey(key_handle, NCRYPT_SILENT_FLAG);
    }
    if (status != ERROR_SUCCESS)
    {
        // An unfinalised key is not persisted; a TPM without the curve refuses it here.
        return status == NTE_NOT_SUPPORTED || status == NTE_BAD_ALGID ? CryptoError::Unsupported
                                                                       : CryptoError::Unavailable;
    }
    const CryptoError exported = export_point(key_handle, algorithm, point);
    if (exported != CryptoError::None)
    {
        (void)NCryptDeleteKey(static_cast<NCRYPT_KEY_HANDLE>(key.release()), 0U);
    }
    return exported;
}
} // namespace

bool TpmKeyProtector::available(const TpmConnection& /*tpm*/) noexcept
{
    Handle provider;
    if (!open_provider(provider))
    {
        return false;
    }
    // The provider opens without a TPM on some systems; its platform type answers only with one.
    DWORD written = 0U;
    return NCryptGetProperty(provider.get(), NCRYPT_PCP_PLATFORM_TYPE_PROPERTY, nullptr, 0U, &written, 0U) ==
               ERROR_SUCCESS &&
           written != 0U;
}

cont::StringView TpmKeyProtector::name() const noexcept
{
    return "tpm-pcp";
}

bool TpmKeyProtector::forgets() const noexcept
{
    return true; // NCryptDeleteKey evicts the TPM key, and the wrapped master key opens nowhere after it
}

CryptoError TpmKeyProtector::protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                     cont::Array<u8>& reference) noexcept
{
    reference.clear();
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    u8          written[kReferenceBytes] = {kReferenceFormat};
    CryptoError result                   = random_bytes(cont::Span<u8>(written + 1U, kHandleBytes));
    if (result == CryptoError::None)
    {
        result = wrap(store_id, cont::ConstSpan<u8>(written + 1U, kHandleBytes), master,
                      cont::Span<u8>(written + 1U + kHandleBytes, kWrappedBytes));
    }
    if (result == CryptoError::None)
    {
        reference.resize(kReferenceBytes);
        std::memcpy(reference.data(), written, kReferenceBytes);
    }
    return result;
}

CryptoError TpmKeyProtector::recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                     cont::Span<u8> master) noexcept
{
    secure_zero(master);
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    if (!well_formed(store_id, reference))
    {
        return CryptoError::Denied; // not a reference this protector wrote
    }
    Handle      key;
    CryptoError result = open_key(kNamePrefix, store_id, reference.subspan(1U, kHandleBytes), key);
    if (result != CryptoError::None)
    {
        return result;
    }
    u8 wrapped[kWrappedBytes];
    std::memcpy(wrapped, reference.data() + 1U + kHandleBytes, kWrappedBytes);
    u8 label[kLabelBytes];
    label_of(store_id, label);
    BCRYPT_OAEP_PADDING_INFO padding = oaep_of(label);
    u8                       plain[kWrappedBytes];
    DWORD                    written = 0U;
    const SECURITY_STATUS    status =
        NCryptDecrypt(static_cast<NCRYPT_KEY_HANDLE>(key.get()), wrapped, static_cast<DWORD>(kWrappedBytes), &padding,
                      plain, static_cast<DWORD>(kWrappedBytes), &written, NCRYPT_PAD_OAEP_FLAG | NCRYPT_SILENT_FLAG);
    // A changed wrapped key or label fails OAEP's check inside the TPM, as does a reference of another store's key.
    result = status == ERROR_SUCCESS && written == kKeyStoreMasterBytes ? CryptoError::None : CryptoError::Denied;
    if (result == CryptoError::None)
    {
        std::memcpy(master.data(), plain, kKeyStoreMasterBytes);
    }
    secure_zero(cont::Span<u8>(plain));
    return result;
}

CryptoError TpmKeyProtector::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    if (!well_formed(store_id, reference))
    {
        return CryptoError::Denied;
    }
    return delete_key(kNamePrefix, store_id, reference.subspan(1U, kHandleBytes));
}

bool TpmKeyDevice::available(const TpmConnection& tpm) noexcept
{
    return TpmKeyProtector::available(tpm);
}

cont::StringView TpmKeyDevice::name() const noexcept
{
    return "tpm-pcp";
}

CryptoError TpmKeyDevice::create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm, cont::Array<u8>& reference,
                                 cont::Span<u8> public_key) noexcept
{
    reference.clear();
    if (curve_of(algorithm) == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (store_id.size() != kKeyStoreIdBytes || public_key.size() != 1U + 2U * field_of(algorithm))
    {
        return CryptoError::BadLength;
    }
    u8          written[kDeviceReferenceBytes] = {kReferenceFormat};
    CryptoError result                         = random_bytes(cont::Span<u8>(written + 1U, kHandleBytes));
    if (result == CryptoError::None)
    {
        result = create_signer(store_id, cont::ConstSpan<u8>(written + 1U, kHandleBytes), algorithm, public_key);
    }
    if (result == CryptoError::None)
    {
        reference.resize(kDeviceReferenceBytes);
        std::memcpy(reference.data(), written, kDeviceReferenceBytes);
    }
    return result;
}

CryptoError TpmKeyDevice::sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                      AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                      cont::Span<u8> signature) const noexcept
{
    secure_zero(signature);
    const usize field = curve_of(algorithm) != nullptr ? field_of(algorithm) : 0U;
    if (field == 0U)
    {
        return CryptoError::Unsupported;
    }
    if (store_id.size() != kKeyStoreIdBytes || digest.size() != field || signature.size() != 2U * field)
    {
        return CryptoError::BadLength;
    }
    if (reference.size() != kDeviceReferenceBytes || reference[0] != kReferenceFormat)
    {
        return CryptoError::Denied; // not a reference this device wrote
    }
    // A handle per call: the provider's key handles are not documented as safe across threads, and the TPM
    // serialises its commands anyway.
    Handle            key;
    const CryptoError result = open_key(kSignerPrefix, store_id, reference.subspan(1U, kHandleBytes), key);
    if (result != CryptoError::None)
    {
        return result;
    }
    u8    hash[48];
    DWORD written = 0U;
    std::memcpy(hash, digest.data(), field);
    const SECURITY_STATUS status =
        NCryptSignHash(static_cast<NCRYPT_KEY_HANDLE>(key.get()), nullptr, hash, static_cast<DWORD>(field),
                       signature.data(), static_cast<DWORD>(signature.size()), &written, NCRYPT_SILENT_FLAG);
    if (status != ERROR_SUCCESS || written != signature.size())
    {
        secure_zero(signature);
        return CryptoError::ProviderFailure;
    }
    return CryptoError::None;
}

CryptoError TpmKeyDevice::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    if (store_id.size() != kKeyStoreIdBytes || reference.size() != kDeviceReferenceBytes ||
        reference[0] != kReferenceFormat)
    {
        return CryptoError::Denied;
    }
    return delete_key(kSignerPrefix, store_id, reference.subspan(1U, kHandleBytes));
}
#elif !CRD_OS_LINUX || CRD_OS_ANDROID // Android has no TPM
bool TpmKeyProtector::available(const TpmConnection& /*tpm*/) noexcept
{
    return false;
}

cont::StringView TpmKeyProtector::name() const noexcept
{
    return "unavailable";
}

bool TpmKeyProtector::forgets() const noexcept
{
    return false;
}

CryptoError TpmKeyProtector::protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                     cont::Array<u8>& reference) noexcept
{
    reference.clear();
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    return CryptoError::Unavailable;
}

CryptoError TpmKeyProtector::recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> /*reference*/,
                                     cont::Span<u8> master) noexcept
{
    secure_zero(master);
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    return CryptoError::Unavailable;
}

CryptoError TpmKeyProtector::forget(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*reference*/) noexcept
{
    return CryptoError::Unavailable;
}

bool TpmKeyDevice::available(const TpmConnection& /*tpm*/) noexcept
{
    return false;
}

cont::StringView TpmKeyDevice::name() const noexcept
{
    return "unavailable";
}

CryptoError TpmKeyDevice::create(cont::ConstSpan<u8> /*store_id*/, AlgorithmId /*algorithm*/,
                                 cont::Array<u8>& reference, cont::Span<u8> /*public_key*/) noexcept
{
    reference.clear();
    return CryptoError::Unavailable;
}

CryptoError TpmKeyDevice::sign_digest(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*reference*/,
                                      AlgorithmId /*algorithm*/, cont::ConstSpan<u8> /*digest*/,
                                      cont::Span<u8> signature) const noexcept
{
    secure_zero(signature);
    return CryptoError::Unavailable;
}

CryptoError TpmKeyDevice::forget(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*reference*/) noexcept
{
    return CryptoError::Unavailable;
}
#endif
} // namespace crd::crypto
