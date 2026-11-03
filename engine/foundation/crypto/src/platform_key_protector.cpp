// crd-crypto -- the operating system's key protector and key device (NET.2; contract:
// docs/design/network-foundation.md#key-store, decisions K2 and K15): DPAPI on Windows, nothing where no protector or
// device exists (the Secret Service's protector is secret_service.cpp, the Android Keystore's android_keystore.cpp).

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
#include <dpapi.h>
#endif

namespace crd::crypto
{
#if CRD_OS_WINDOWS
namespace
{
constexpr char  kEntropyLabel[] = "cerid key store master 1";
constexpr usize kEntropyBytes   = sizeof(kEntropyLabel) - 1U + kKeyStoreIdBytes;

// DPAPI's optional entropy: the label, then the store's identifier, so a reference recovers only in its own store.
[[nodiscard]] bool entropy_of(cont::ConstSpan<u8> store_id, u8 (&entropy)[kEntropyBytes]) noexcept
{
    if (store_id.size() != kKeyStoreIdBytes)
    {
        return false;
    }
    std::memcpy(entropy, kEntropyLabel, sizeof(kEntropyLabel) - 1U);
    std::memcpy(entropy + sizeof(kEntropyLabel) - 1U, store_id.data(), kKeyStoreIdBytes);
    return true;
}

// DPAPI shows its own dialog only where a policy asks for strong key protection: forbidden unless prompts are allowed.
[[nodiscard]] DWORD ui_flags(u32 prompt_milliseconds) noexcept
{
    return prompt_milliseconds == 0U ? CRYPTPROTECT_UI_FORBIDDEN : 0U;
}

// DPAPI's blobs point at mutable bytes it only reads: the callers hand it their own copies.
[[nodiscard]] DATA_BLOB blob_of(cont::Span<u8> bytes) noexcept
{
    DATA_BLOB blob{};
    blob.cbData = static_cast<DWORD>(bytes.size());
    blob.pbData = bytes.data();
    return blob;
}
} // namespace

bool PlatformKeyProtector::available() noexcept
{
    return true;
}

cont::StringView PlatformKeyProtector::name() const noexcept
{
    return "dpapi";
}

bool PlatformKeyProtector::forgets() const noexcept
{
    return false; // a DPAPI blob recovers for as long as its user's master keys exist
}

CryptoError PlatformKeyProtector::protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                          cont::Array<u8>& reference) noexcept
{
    reference.clear();
    u8 entropy[kEntropyBytes];
    if (master.size() != kKeyStoreMasterBytes || !entropy_of(store_id, entropy))
    {
        return CryptoError::BadLength;
    }
    u8 secret[kKeyStoreMasterBytes];
    std::memcpy(secret, master.data(), kKeyStoreMasterBytes);
    DATA_BLOB  in        = blob_of(cont::Span<u8>(secret));
    DATA_BLOB  salt      = blob_of(cont::Span<u8>(entropy));
    DATA_BLOB  out{};
    const bool guarded =
        CryptProtectData(&in, L"Cerid key store", &salt, nullptr, nullptr, ui_flags(m_prompt_milliseconds), &out) != 0;
    secure_zero(cont::Span<u8>(secret));
    if (!guarded)
    {
        return CryptoError::Unavailable;
    }
    const bool fits = out.cbData != 0U && out.cbData <= kKeyReferenceMaxBytes;
    if (fits)
    {
        reference.resize(out.cbData);
        std::memcpy(reference.data(), out.pbData, out.cbData);
    }
    (void)LocalFree(out.pbData);
    return fits ? CryptoError::None : CryptoError::Unsupported;
}

CryptoError PlatformKeyProtector::recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                          cont::Span<u8> master) noexcept
{
    u8 entropy[kEntropyBytes];
    if (master.size() != kKeyStoreMasterBytes || !entropy_of(store_id, entropy))
    {
        secure_zero(master);
        return CryptoError::BadLength;
    }
    cont::Array<u8> sealed;
    sealed.resize(reference.size());
    if (!reference.empty())
    {
        std::memcpy(sealed.data(), reference.data(), reference.size());
    }
    DATA_BLOB in   = blob_of(cont::Span<u8>(sealed.data(), sealed.size()));
    DATA_BLOB salt = blob_of(cont::Span<u8>(entropy));
    DATA_BLOB out{};
    if (CryptUnprotectData(&in, nullptr, &salt, nullptr, nullptr, ui_flags(m_prompt_milliseconds), &out) == 0)
    {
        secure_zero(master);
        return CryptoError::Denied; // another user's blob, another store's, or a changed one: DPAPI tells none apart
    }
    const bool fits = out.cbData == kKeyStoreMasterBytes;
    if (fits)
    {
        std::memcpy(master.data(), out.pbData, kKeyStoreMasterBytes);
    }
    else
    {
        secure_zero(master);
    }
    SecureZeroMemory(out.pbData, out.cbData);
    (void)LocalFree(out.pbData);
    return fits ? CryptoError::None : CryptoError::Denied;
}

CryptoError PlatformKeyProtector::forget(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*reference*/) noexcept
{
    return CryptoError::None; // nothing to destroy: forgets() is false
}
#elif !CRD_OS_LINUX
bool PlatformKeyProtector::available() noexcept
{
    return false;
}

cont::StringView PlatformKeyProtector::name() const noexcept
{
    return "unavailable";
}

bool PlatformKeyProtector::forgets() const noexcept
{
    return false;
}

CryptoError PlatformKeyProtector::protect(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*master*/,
                                          cont::Array<u8>& reference) noexcept
{
    reference.clear();
    return CryptoError::Unavailable;
}

CryptoError PlatformKeyProtector::recover(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*reference*/,
                                          cont::Span<u8> master) noexcept
{
    secure_zero(master);
    return CryptoError::Unavailable;
}

CryptoError PlatformKeyProtector::forget(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*reference*/) noexcept
{
    return CryptoError::Unavailable;
}
#endif

#if !CRD_OS_ANDROID
bool PlatformKeyDevice::available() noexcept
{
    return false;
}

cont::StringView PlatformKeyDevice::name() const noexcept
{
    return "unavailable";
}

CryptoError PlatformKeyDevice::create(cont::ConstSpan<u8> /*store_id*/, AlgorithmId /*algorithm*/,
                                      cont::Array<u8>& reference, cont::Span<u8> public_key) noexcept
{
    reference.clear();
    secure_zero(public_key);
    return CryptoError::Unavailable;
}

CryptoError PlatformKeyDevice::sign_digest(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*reference*/,
                                           AlgorithmId /*algorithm*/, cont::ConstSpan<u8> /*digest*/,
                                           cont::Span<u8> signature) const noexcept
{
    secure_zero(signature);
    return CryptoError::Unavailable;
}

CryptoError PlatformKeyDevice::forget(cont::ConstSpan<u8> /*store_id*/, cont::ConstSpan<u8> /*reference*/) noexcept
{
    return CryptoError::Unavailable;
}
#endif
} // namespace crd::crypto
