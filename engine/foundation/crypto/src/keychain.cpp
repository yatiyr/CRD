// crd-crypto -- the macOS Keychain protector and the Secure Enclave key device (NET.2; contract:
// docs/design/network-foundation.md#key-store, decisions K2, K8 and K16): PlatformKeyProtector and PlatformKeyDevice
// on macOS.
//
// Security.framework's item interface is C over CoreFoundation: every object this unit creates or copies is released
// by its Cf owner. A master key is a generic password item of the data-protection keychain where the process holds
// the entitlement for it (a signed application), of the user's default file keychain otherwise; the reference's
// format byte names which, so recover() asks the keychain that holds it. A Secure Enclave key is a permanent P-256
// key of the data-protection keychain, which only a signed process on a Mac with the enclave can make.
#include <crd/core/platform.hpp>

#if CRD_OS_MAC
#include "byte_spans.hpp"
#include "provider_internal.hpp"

#include <crd/crypto/key_store.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <cstring>

namespace crd::crypto
{
namespace
{
// ---- CoreFoundation ownership ----

// One owned CoreFoundation reference (a Create or Copy result), released with its owner.
template <typename T> class Cf
{
public:
    Cf() noexcept = default;
    explicit Cf(T ref) noexcept : m_ref(ref) {}
    Cf(const Cf&)            = delete;
    Cf& operator=(const Cf&) = delete;
    Cf(Cf&& other) noexcept : m_ref(other.m_ref) { other.m_ref = nullptr; }
    Cf& operator=(Cf&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_ref       = other.m_ref;
            other.m_ref = nullptr;
        }
        return *this;
    }
    ~Cf() noexcept { release(); }

    [[nodiscard]] T get() const noexcept { return m_ref; }
    // The slot a Copy function writes its result into.
    [[nodiscard]] T* slot() noexcept { return &m_ref; }

private:
    void release() noexcept
    {
        if (m_ref != nullptr)
        {
            CFRelease(m_ref);
        }
    }

    T m_ref = nullptr;
};

using Dictionary = Cf<CFMutableDictionaryRef>;

[[nodiscard]] Dictionary dictionary() noexcept
{
    return Dictionary(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks));
}

[[nodiscard]] Cf<CFDataRef> data_of(cont::ConstSpan<u8> bytes) noexcept
{
    return Cf<CFDataRef>(CFDataCreate(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size())));
}

// ---- Names and references ----

constexpr u8    kFileKeychain           = 1U; // the reference's format: the user's default file keychain
constexpr u8    kDataProtectionKeychain = 2U; // the data-protection keychain of a signed application
constexpr usize kHandleBytes            = 16U;
constexpr usize kReferenceBytes         = 1U + kHandleBytes;
constexpr char  kMasterPrefix[]         = "cerid.master.";
constexpr char  kEnclavePrefix[]        = "cerid.p256.";
// A name: the prefix, the store's identifier and the handle in hexadecimal, a dot between them.
constexpr usize kNameBytes = sizeof(kMasterPrefix) - 1U + 2U * kKeyStoreIdBytes + 1U + 2U * kHandleBytes;
static_assert(kReferenceBytes <= kKeyDeviceReferenceMaxBytes, "a reference fits a device key's field");
static_assert(sizeof(kEnclavePrefix) <= sizeof(kMasterPrefix), "the master key's prefix is the longest");

struct Name
{
    char  text[kNameBytes];
    usize bytes = 0U;
};

template <usize N> [[nodiscard]] Name name_of(const char (&prefix)[N], cont::ConstSpan<u8> store_id,
                                              cont::ConstSpan<u8> handle) noexcept
{
    Name name;
    std::memcpy(name.text, prefix, N - 1U);
    char* out = detail::hex_into(store_id, name.text + N - 1U);
    *out++    = '.';
    out       = detail::hex_into(handle, out);
    name.bytes = static_cast<usize>(out - name.text);
    return name;
}

[[nodiscard]] Cf<CFStringRef> string_of(const Name& name) noexcept
{
    return Cf<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(name.text),
                                                   static_cast<CFIndex>(name.bytes), kCFStringEncodingASCII, false));
}

// The handle of a well-formed reference whose format is one of `formats`, or an empty span.
[[nodiscard]] cont::ConstSpan<u8> handle_of(cont::ConstSpan<u8> reference, u8 first, u8 last) noexcept
{
    if (reference.size() != kReferenceBytes || reference[0] < first || reference[0] > last)
    {
        return {};
    }
    return reference.subspan(1U, kHandleBytes);
}

// ---- Results ----

// What a Keychain status means for the store (decision K9): the item or key is not this caller's to use (Denied:
// missing, another application's, a locked keychain or a refused prompt) or the Keychain does not answer
// (Unavailable: no keychain, no entitlement, no enclave).
[[nodiscard]] CryptoError error_of(OSStatus status) noexcept
{
    switch (status)
    {
    case errSecSuccess:
        return CryptoError::None;
    case errSecItemNotFound:
    case errSecInteractionNotAllowed:
    case errSecAuthFailed:
    case errSecUserCanceled:
    case errSecNoAccessForItem:
    case errSecInvalidOwnerEdit:
        return CryptoError::Denied;
    default:
        return CryptoError::Unavailable;
    }
}

[[nodiscard]] OSStatus status_of(CFErrorRef error) noexcept
{
    return error == nullptr ? errSecInternalComponent : static_cast<OSStatus>(CFErrorGetCode(error));
}

// The no-prompt switches, deprecated without a C replacement (LocalAuthentication's context is Objective-C): a locked
// keychain or an access prompt is then refused with errSecInteractionNotAllowed instead of asking the user.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
void forbid_prompts(CFMutableDictionaryRef query) noexcept
{
    CFDictionarySetValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
}

// Whether the default file keychain is unlocked: an item is added there only then when prompts are forbidden, since
// adding to a locked one asks the user to unlock it whatever the query says.
[[nodiscard]] CryptoError default_keychain_open() noexcept
{
    Cf<SecKeychainRef> keychain;
    const OSStatus     found = SecKeychainCopyDefault(keychain.slot());
    if (found != errSecSuccess)
    {
        return CryptoError::Unavailable;
    }
    SecKeychainStatus state = 0U;
    if (SecKeychainGetStatus(keychain.get(), &state) != errSecSuccess)
    {
        return CryptoError::Unavailable; // a default keychain whose file is gone
    }
    return (state & kSecUnlockStateStatus) != 0U ? CryptoError::None : CryptoError::Denied;
}

// Limits a no-prompt query of the file keychains to the unlocked ones of the user's own search list (the system
// domain's keychains hold no item of a store, which adds to the user's default keychain): asked for an item, a
// locked keychain shows its unlock prompt whatever kSecUseAuthenticationUI says (the macOS lane, 2026-11-04), so one
// is never asked. `skipped` says whether a keychain was left out, so "not found" then proves nothing. Denied when no
// keychain of the list is unlocked, Unavailable when the list cannot be read.
[[nodiscard]] CryptoError search_unlocked(CFMutableDictionaryRef query, bool& skipped) noexcept
{
    skipped = false;
    Cf<CFArrayRef>              all;
    const Cf<CFMutableArrayRef> open(CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks));
    const OSStatus              listed = SecKeychainCopyDomainSearchList(kSecPreferencesDomainUser, all.slot());
    if (listed != errSecSuccess || all.get() == nullptr || open.get() == nullptr)
    {
        return CryptoError::Unavailable;
    }
    for (CFIndex i = 0; i < CFArrayGetCount(all.get()); ++i)
    {
        const void* const value = CFArrayGetValueAtIndex(all.get(), i);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): a CFArray hands out its values as const void*
        const auto        keychain = static_cast<SecKeychainRef>(const_cast<void*>(value));
        SecKeychainStatus state    = 0U;
        if (SecKeychainGetStatus(keychain, &state) == errSecSuccess && (state & kSecUnlockStateStatus) != 0U)
        {
            CFArrayAppendValue(open.get(), keychain);
        }
        else
        {
            skipped = true;
        }
    }
    if (CFArrayGetCount(open.get()) == 0)
    {
        return CryptoError::Denied;
    }
    CFDictionarySetValue(query, kSecMatchSearchList, open.get());
    return CryptoError::None;
}
#pragma clang diagnostic pop

// ---- PlatformKeyProtector: one generic password item for each master key ----

// The query that finds a master key's item: its service, its account (the store and the handle) and its keychain.
[[nodiscard]] Dictionary master_query(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle, u8 keychain,
                                      bool prompts) noexcept
{
    Dictionary                 query   = dictionary();
    const Cf<CFStringRef>      account = string_of(name_of(kMasterPrefix, store_id, handle));
    if (query.get() == nullptr || account.get() == nullptr)
    {
        return Dictionary();
    }
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, CFSTR("org.cerid.KeyStore.Master"));
    CFDictionarySetValue(query.get(), kSecAttrAccount, account.get());
    if (keychain == kDataProtectionKeychain)
    {
        CFDictionarySetValue(query.get(), kSecUseDataProtectionKeychain, kCFBooleanTrue);
    }
    if (!prompts)
    {
        forbid_prompts(query.get());
    }
    return query;
}

// Adds the master key's item to `keychain`; the master key crosses into the Keychain from `secret`, never copied.
[[nodiscard]] OSStatus add_master(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle, u8 keychain,
                                  cont::ConstSpan<u8> secret) noexcept
{
    Dictionary item = master_query(store_id, handle, keychain, true);
    const Cf<CFDataRef> value(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, secret.data(),
                                                          static_cast<CFIndex>(secret.size()), kCFAllocatorNull));
    if (item.get() == nullptr || value.get() == nullptr)
    {
        return errSecAllocate;
    }
    CFDictionarySetValue(item.get(), kSecValueData, value.get());
    CFDictionarySetValue(item.get(), kSecAttrLabel, CFSTR("Cerid key store master key"));
    // Never in a backup or on another device: the store's file is useless without this Mac's item, by design.
    CFDictionarySetValue(item.get(), kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
    return SecItemAdd(item.get(), nullptr);
}

// The query that finds the item a reference names, for recover() and forget(): without prompts, a file keychain item
// only in the unlocked keychains (search_unlocked; `skipped` when a locked one was left out).
[[nodiscard]] CryptoError lookup_query(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle, u8 keychain,
                                       bool prompts, Dictionary& query, bool& skipped) noexcept
{
    skipped = false;
    query   = master_query(store_id, handle, keychain, prompts);
    if (query.get() == nullptr)
    {
        return CryptoError::Unavailable;
    }
    return keychain == kFileKeychain && !prompts ? search_unlocked(query.get(), skipped) : CryptoError::None;
}

// ---- PlatformKeyDevice: permanent P-256 keys in the Secure Enclave ----

[[nodiscard]] Dictionary enclave_query(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle) noexcept
{
    Dictionary          query = dictionary();
    const Name          name  = name_of(kEnclavePrefix, store_id, handle);
    const Cf<CFDataRef> tag   = data_of(cont::ConstSpan<u8>(reinterpret_cast<const u8*>(name.text), name.bytes));
    if (query.get() == nullptr || tag.get() == nullptr)
    {
        return Dictionary();
    }
    CFDictionarySetValue(query.get(), kSecClass, kSecClassKey);
    CFDictionarySetValue(query.get(), kSecAttrKeyClass, kSecAttrKeyClassPrivate);
    CFDictionarySetValue(query.get(), kSecAttrApplicationTag, tag.get());
    CFDictionarySetValue(query.get(), kSecAttrTokenID, kSecAttrTokenIDSecureEnclave);
    CFDictionarySetValue(query.get(), kSecUseDataProtectionKeychain, kCFBooleanTrue);
    forbid_prompts(query.get());
    return query;
}

// A new P-256 key in the enclave: permanent under `tag` when one is given (only a signed process with a keychain
// entitlement may keep one), otherwise for this call alone; usable for signing only while the Mac is unlocked.
[[nodiscard]] Cf<SecKeyRef> enclave_key(CFDataRef tag, OSStatus& status) noexcept
{
    Dictionary private_attributes = dictionary();
    Dictionary attributes         = dictionary();
    Cf<CFErrorRef> error;
    const Cf<SecAccessControlRef> access(SecAccessControlCreateWithFlags(
        kCFAllocatorDefault, kSecAttrAccessibleWhenUnlockedThisDeviceOnly, kSecAccessControlPrivateKeyUsage,
        error.slot()));
    const int bits = 256;
    const Cf<CFNumberRef> size(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bits));
    if (private_attributes.get() == nullptr || attributes.get() == nullptr || access.get() == nullptr ||
        size.get() == nullptr)
    {
        status = errSecAllocate;
        return Cf<SecKeyRef>();
    }
    CFDictionarySetValue(private_attributes.get(), kSecAttrAccessControl, access.get());
    CFDictionarySetValue(private_attributes.get(), kSecAttrIsPermanent,
                         tag != nullptr ? kCFBooleanTrue : kCFBooleanFalse);
    if (tag != nullptr)
    {
        CFDictionarySetValue(private_attributes.get(), kSecAttrApplicationTag, tag);
    }
    CFDictionarySetValue(attributes.get(), kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(attributes.get(), kSecAttrKeySizeInBits, size.get());
    CFDictionarySetValue(attributes.get(), kSecAttrTokenID, kSecAttrTokenIDSecureEnclave);
    CFDictionarySetValue(attributes.get(), kSecUseDataProtectionKeychain, kCFBooleanTrue);
    CFDictionarySetValue(attributes.get(), kSecPrivateKeyAttrs, private_attributes.get());
    Cf<SecKeyRef> key(SecKeyCreateRandomKey(attributes.get(), error.slot()));
    status = key.get() != nullptr ? errSecSuccess : status_of(error.get());
    return key;
}

// The uncompressed SEC 1 point of `key`'s public half into `public_key` (65 bytes).
[[nodiscard]] CryptoError public_point(SecKeyRef key, cont::Span<u8> public_key) noexcept
{
    const Cf<SecKeyRef> public_half(SecKeyCopyPublicKey(key));
    Cf<CFErrorRef>      error;
    const Cf<CFDataRef> point(public_half.get() != nullptr
                                  ? SecKeyCopyExternalRepresentation(public_half.get(), error.slot())
                                  : nullptr);
    if (point.get() == nullptr || static_cast<usize>(CFDataGetLength(point.get())) != public_key.size() ||
        CFDataGetBytePtr(point.get())[0] != 0x04U)
    {
        return CryptoError::ProviderFailure;
    }
    std::memcpy(public_key.data(), CFDataGetBytePtr(point.get()), public_key.size());
    return CryptoError::None;
}

// The forms a call must have before the device is asked: the curve it holds, the store's identifier and the lengths.
[[nodiscard]] CryptoError enclave_form(AlgorithmId algorithm, cont::ConstSpan<u8> store_id) noexcept
{
    if (algorithm != AlgorithmId::EcdsaSecp256r1Sha256)
    {
        return CryptoError::Unsupported; // the Secure Enclave holds P-256 keys only
    }
    return store_id.size() == kKeyStoreIdBytes ? CryptoError::None : CryptoError::BadLength;
}
} // namespace

bool PlatformKeyProtector::available() noexcept
{
    // A user session has a default keychain (locked or not); a lookup answers "not found" even without one, and the
    // data-protection keychain answers lookups for a process it refuses to store for (the macOS lane, 2026-11-04).
    return default_keychain_open() != CryptoError::Unavailable;
}

cont::StringView PlatformKeyProtector::name() const noexcept
{
    return "keychain";
}

bool PlatformKeyProtector::forgets() const noexcept
{
    return true; // deleting the item destroys the only copy of the master key
}

CryptoError PlatformKeyProtector::protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                          cont::Array<u8>& reference) noexcept
{
    reference.clear();
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    u8          written[kReferenceBytes] = {kDataProtectionKeychain};
    CryptoError result                   = random_bytes(cont::Span<u8>(written + 1U, kHandleBytes));
    if (result != CryptoError::None)
    {
        return result;
    }
    const cont::ConstSpan<u8> handle(written + 1U, kHandleBytes);
    u8 secret[kKeyStoreMasterBytes];
    std::memcpy(secret, master.data(), kKeyStoreMasterBytes);
    OSStatus status = add_master(store_id, handle, kDataProtectionKeychain, cont::ConstSpan<u8>(secret));
    if (status == errSecMissingEntitlement)
    {
        // An unsigned process (a tool, a test) has no data-protection keychain: the user's default file keychain.
        written[0] = kFileKeychain;
        result     = m_prompt_milliseconds == 0U ? default_keychain_open() : CryptoError::None;
        if (result == CryptoError::None)
        {
            status = add_master(store_id, handle, kFileKeychain, cont::ConstSpan<u8>(secret));
        }
    }
    secure_zero(cont::Span<u8>(secret));
    result = result != CryptoError::None ? result : error_of(status);
    if (result != CryptoError::None)
    {
        return result;
    }
    reference.resize(kReferenceBytes);
    std::memcpy(reference.data(), written, kReferenceBytes);
    return CryptoError::None;
}

CryptoError PlatformKeyProtector::recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                          cont::Span<u8> master) noexcept
{
    secure_zero(master);
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    const cont::ConstSpan<u8> handle = handle_of(reference, kFileKeychain, kDataProtectionKeychain);
    if (handle.empty())
    {
        return CryptoError::Denied; // not a reference this protector wrote
    }
    Dictionary        query;
    bool              skipped = false;
    const CryptoError scoped =
        lookup_query(store_id, handle, reference[0], m_prompt_milliseconds != 0U, query, skipped);
    if (scoped != CryptoError::None)
    {
        return scoped;
    }
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    Cf<CFTypeRef>  found;
    const OSStatus status = SecItemCopyMatching(query.get(), found.slot());
    if (status != errSecSuccess)
    {
        return error_of(status);
    }
    if (found.get() == nullptr || CFGetTypeID(found.get()) != CFDataGetTypeID())
    {
        return CryptoError::Denied;
    }
    const auto data  = static_cast<CFDataRef>(found.get());
    const usize bytes = static_cast<usize>(CFDataGetLength(data));
    // The Keychain's copy is this call's own buffer, zeroed before it is released (decision K7).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): a CFData's bytes are its own heap buffer
    const cont::Span<u8> copy(const_cast<u8*>(CFDataGetBytePtr(data)), bytes);
    const bool           fits = bytes == kKeyStoreMasterBytes;
    if (fits)
    {
        std::memcpy(master.data(), copy.data(), kKeyStoreMasterBytes);
    }
    secure_zero(copy);
    return fits ? CryptoError::None : CryptoError::Denied;
}

CryptoError PlatformKeyProtector::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    const cont::ConstSpan<u8> handle = handle_of(reference, kFileKeychain, kDataProtectionKeychain);
    if (store_id.size() != kKeyStoreIdBytes || handle.empty())
    {
        return CryptoError::Denied;
    }
    Dictionary        query;
    bool              skipped = false;
    const CryptoError scoped =
        lookup_query(store_id, handle, reference[0], m_prompt_milliseconds != 0U, query, skipped);
    if (scoped != CryptoError::None)
    {
        return scoped;
    }
    // Not found is nothing to destroy only when every keychain was searched: an item in a locked one survives, and
    // claiming it gone would let the store drop the reference while the master key still recovers (decision K5).
    const OSStatus status = SecItemDelete(query.get());
    if (status == errSecItemNotFound)
    {
        return skipped ? CryptoError::Denied : CryptoError::None;
    }
    return error_of(status);
}

bool PlatformKeyDevice::available() noexcept
{
    // An ephemeral key, which the enclave makes for any process on a Mac that has one, and which nothing keeps.
    OSStatus status = errSecSuccess;
    return enclave_key(nullptr, status).get() != nullptr;
}

cont::StringView PlatformKeyDevice::name() const noexcept
{
    return "secure-enclave";
}

CryptoError PlatformKeyDevice::create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm, cont::Array<u8>& reference,
                                      cont::Span<u8> public_key) noexcept
{
    reference.clear();
    secure_zero(public_key);
    CryptoError result = enclave_form(algorithm, store_id);
    if (result != CryptoError::None)
    {
        return result;
    }
    if (public_key.size() != 65U)
    {
        return CryptoError::BadLength;
    }
    u8 written[kReferenceBytes] = {kDataProtectionKeychain};
    result                      = random_bytes(cont::Span<u8>(written + 1U, kHandleBytes));
    if (result != CryptoError::None)
    {
        return result;
    }
    const cont::ConstSpan<u8> handle(written + 1U, kHandleBytes);
    const Name                name = name_of(kEnclavePrefix, store_id, handle);
    const Cf<CFDataRef>       tag  = data_of(cont::ConstSpan<u8>(reinterpret_cast<const u8*>(name.text), name.bytes));
    if (tag.get() == nullptr)
    {
        return CryptoError::Unavailable;
    }
    OSStatus            status = errSecSuccess;
    const Cf<SecKeyRef> key    = enclave_key(tag.get(), status);
    if (key.get() == nullptr)
    {
        return error_of(status) == CryptoError::Denied ? CryptoError::Denied : CryptoError::Unavailable;
    }
    result = public_point(key.get(), public_key);
    if (result != CryptoError::None)
    {
        secure_zero(public_key);
        (void)forget(store_id, cont::ConstSpan<u8>(written)); // a key whose public half was not read is no store's
        return result;
    }
    reference.resize(kReferenceBytes);
    std::memcpy(reference.data(), written, kReferenceBytes);
    return CryptoError::None;
}

CryptoError PlatformKeyDevice::sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                           AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                           cont::Span<u8> signature) const noexcept
{
    secure_zero(signature);
    const CryptoError form = enclave_form(algorithm, store_id);
    if (form != CryptoError::None)
    {
        return form;
    }
    if (digest.size() != 32U || signature.size() != 64U)
    {
        return CryptoError::BadLength;
    }
    const cont::ConstSpan<u8> handle = handle_of(reference, kDataProtectionKeychain, kDataProtectionKeychain);
    if (handle.empty())
    {
        return CryptoError::Denied; // not a reference this device wrote
    }
    const Dictionary query = enclave_query(store_id, handle);
    if (query.get() == nullptr)
    {
        return CryptoError::Unavailable;
    }
    CFDictionarySetValue(query.get(), kSecReturnRef, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    Cf<CFTypeRef>  found;
    const OSStatus status = SecItemCopyMatching(query.get(), found.slot());
    if (status != errSecSuccess)
    {
        return error_of(status);
    }
    if (found.get() == nullptr || CFGetTypeID(found.get()) != SecKeyGetTypeID())
    {
        return CryptoError::Denied;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): SecItemCopyMatching returns every result as CFTypeRef
    const auto          key    = static_cast<SecKeyRef>(const_cast<void*>(found.get()));
    const Cf<CFDataRef> hashed = data_of(digest);
    Cf<CFErrorRef>      error;
    const SecKeyAlgorithm digest_signature = kSecKeyAlgorithmECDSASignatureDigestX962SHA256;
    const Cf<CFDataRef>   der(hashed.get() != nullptr
                                  ? SecKeyCreateSignature(key, digest_signature, hashed.get(), error.slot())
                                  : nullptr);
    if (der.get() == nullptr)
    {
        return error_of(status_of(error.get())) == CryptoError::Denied ? CryptoError::Denied : CryptoError::Unavailable;
    }
    return provider_internal::ecdsa_raw_of(
        algorithm, cont::ConstSpan<u8>(CFDataGetBytePtr(der.get()), static_cast<usize>(CFDataGetLength(der.get()))),
        signature);
}

CryptoError PlatformKeyDevice::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    const cont::ConstSpan<u8> handle = handle_of(reference, kDataProtectionKeychain, kDataProtectionKeychain);
    if (store_id.size() != kKeyStoreIdBytes || handle.empty())
    {
        return CryptoError::Denied;
    }
    const Dictionary query = enclave_query(store_id, handle);
    if (query.get() == nullptr)
    {
        return CryptoError::Unavailable;
    }
    const OSStatus status = SecItemDelete(query.get());
    return status == errSecItemNotFound ? CryptoError::None : error_of(status);
}
} // namespace crd::crypto
#endif
