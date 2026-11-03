// crd-crypto -- the PKCS #11 key device (NET.2; contract: docs/design/network-foundation.md#key-store, decision K14).
//
// A token's module is loaded at run time from the path its caller names, initialised once (CKF_OS_LOCKING_OK) and
// never finalised or unloaded, since another user of the module in the process may still hold sessions; only the
// PKCS #11 2.40 function list (C_GetFunctionList) is called, so every module from 2.20 on answers. One process-wide
// lock orders every call, and each call opens a session of its own: login state belongs to the token and the whole
// application (C_Logout in any session logs out all of them), so the device logs the user in only when no session of
// the process has and never logs out; the token closes the login with its last session. A key is a sign-only ECDSA
// pair whose CKA_ID is a random handle and whose CKA_LABEL names the store, and every search names the handle, the
// label and the curve, so another store's reference, or a key of another curve, is found nowhere.
#include "byte_spans.hpp"

#include <crd/crypto/key_store.hpp>
#include <crd/platform/dynamic_library.hpp>

// The five macros OASIS's header asks its includer to define, and the 1-byte packing Cryptoki uses on Windows.
// NOLINTBEGIN(cppcoreguidelines-macro-usage, bugprone-macro-parentheses): the header's own interface
#define CK_PTR *
#define CK_DECLARE_FUNCTION(returnType, name) returnType name
#define CK_DECLARE_FUNCTION_POINTER(returnType, name) returnType(*name)
#define CK_CALLBACK_FUNCTION(returnType, name) returnType(*name)
#ifndef NULL_PTR
#define NULL_PTR nullptr
#endif
// NOLINTEND(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)
#if defined(_WIN32)
#pragma pack(push, cryptoki, 1)
#endif
#include <pkcs11.h>
#if defined(_WIN32)
#pragma pack(pop, cryptoki)
#endif

#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace crd::crypto
{
namespace
{
// ---- Modules, loaded once ----

constexpr usize kModulesMax = 8U;  // distinct modules one process opens
constexpr usize kSlotsMax   = 64U; // slots with a token present that one module lists

struct Module
{
    char                     path[kPkcs11ModuleMaxBytes + 1U] = {};
    platform::DynamicLibrary library;
    CK_FUNCTION_LIST_PTR     functions = nullptr;
};

struct Modules
{
    Module entries[kModulesMax];
    usize  count = 0U;
};

// Never destroyed: an initialised module stays mapped for the life of the process.
Modules& modules() noexcept
{
    alignas(Modules) static unsigned char storage[sizeof(Modules)];
    static Modules* const kModules = ::new (static_cast<void*>(storage)) Modules();
    return *kModules;
}

// One PKCS #11 conversation at a time in this process.
std::mutex& pkcs11_lock() noexcept
{
    static std::mutex lock;
    return lock;
}

// The module at `path`, loaded and initialised on first use; null when it is not a PKCS #11 module or cannot be
// loaded. Under the lock.
[[nodiscard]] const CK_FUNCTION_LIST* module_at(const char* path) noexcept
{
    Modules& loaded = modules();
    for (usize i = 0U; i < loaded.count; ++i)
    {
        if (std::strcmp(loaded.entries[i].path, path) == 0)
        {
            return loaded.entries[i].functions;
        }
    }
    if (loaded.count == kModulesMax)
    {
        return nullptr;
    }
    platform::DynamicLibrary library = platform::DynamicLibrary::open(platform::fs::Path(path), false);
    const CK_C_GetFunctionList get_list =
        library.is_valid() ? library.resolve_as<CK_C_GetFunctionList>("C_GetFunctionList") : nullptr;
    CK_FUNCTION_LIST_PTR functions = nullptr;
    if (get_list == nullptr || get_list(&functions) != CKR_OK || functions == nullptr ||
        functions->version.major < 2U || functions->C_Initialize == nullptr)
    {
        return nullptr;
    }
    CK_C_INITIALIZE_ARGS arguments{};
    arguments.flags     = CKF_OS_LOCKING_OK;
    const CK_RV started = functions->C_Initialize(&arguments);
    if (started != CKR_OK && started != CKR_CRYPTOKI_ALREADY_INITIALIZED)
    {
        return nullptr;
    }
    Module& entry = loaded.entries[loaded.count++];
    std::memcpy(entry.path, path, std::strlen(path) + 1U);
    entry.library   = std::move(library);
    entry.functions = functions;
    return functions;
}

// ---- Return values ----

[[nodiscard]] CryptoError failure_of(CK_RV rv) noexcept
{
    switch (rv)
    {
    case CKR_OK:
        return CryptoError::None;
    case CKR_MECHANISM_INVALID:
    case CKR_CURVE_NOT_SUPPORTED:
    case CKR_DOMAIN_PARAMS_INVALID:
    case CKR_TEMPLATE_INCONSISTENT:
    case CKR_ATTRIBUTE_TYPE_INVALID:
    case CKR_ATTRIBUTE_VALUE_INVALID:
        return CryptoError::Unsupported; // the curve, or a sign-only key that never leaves the token
    case CKR_USER_NOT_LOGGED_IN:
    case CKR_USER_ANOTHER_ALREADY_LOGGED_IN:
    case CKR_KEY_HANDLE_INVALID:
    case CKR_OBJECT_HANDLE_INVALID:
    case CKR_KEY_FUNCTION_NOT_PERMITTED:
    case CKR_ACTION_PROHIBITED:
        return CryptoError::Denied;
    case CKR_DEVICE_MEMORY:
        return CryptoError::StorageFailure; // the token is full
    case CKR_HOST_MEMORY:
        return CryptoError::ProviderFailure;
    default:
        return CryptoError::Unavailable; // removed, faulted or not answering
    }
}

// ---- Keys and references ----

constexpr u8    kReferenceFormat = 1U;
constexpr usize kHandleBytes     = 16U;
constexpr usize kReferenceBytes  = 1U + kHandleBytes;
constexpr char  kLabelPrefix[]   = "cerid key store ";
constexpr usize kLabelBytes      = sizeof(kLabelPrefix) - 1U + 2U * kKeyStoreIdBytes;

constexpr CK_BYTE kP256[] = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07}; // prime256v1
constexpr CK_BYTE kP384[] = {0x06, 0x05, 0x2B, 0x81, 0x04, 0x00, 0x22};                   // secp384r1

// A curve as a token takes it: its OID (CKA_EC_PARAMS, a copy a template may point into) and its field length.
struct Curve
{
    CK_BYTE oid[sizeof(kP256)] = {};
    usize   oid_bytes          = 0U;
    usize   field              = 0U;

    [[nodiscard]] CK_ATTRIBUTE params() noexcept
    {
        return CK_ATTRIBUTE{CKA_EC_PARAMS, oid, static_cast<CK_ULONG>(oid_bytes)};
    }
};

[[nodiscard]] Curve curve_of(AlgorithmId algorithm) noexcept
{
    Curve curve;
    if (algorithm == AlgorithmId::EcdsaSecp256r1Sha256)
    {
        std::memcpy(curve.oid, kP256, sizeof(kP256));
        curve.oid_bytes = sizeof(kP256);
        curve.field     = 32U;
    }
    else if (algorithm == AlgorithmId::EcdsaSecp384r1Sha384)
    {
        std::memcpy(curve.oid, kP384, sizeof(kP384));
        curve.oid_bytes = sizeof(kP384);
        curve.field     = 48U;
    }
    return curve;
}

// The label every key of the store carries: the prefix and the store's identifier in hexadecimal.
void label_of(cont::ConstSpan<u8> store_id, char (&label)[kLabelBytes]) noexcept
{
    std::memcpy(label, kLabelPrefix, sizeof(kLabelPrefix) - 1U);
    (void)detail::hex_into(store_id, label + sizeof(kLabelPrefix) - 1U);
}

[[nodiscard]] bool parse(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> bytes, u8 (&handle)[kHandleBytes]) noexcept
{
    if (store_id.size() != kKeyStoreIdBytes || bytes.size() != kReferenceBytes || bytes[0] != kReferenceFormat)
    {
        return false;
    }
    std::memcpy(handle, bytes.data() + 1U, kHandleBytes);
    return true;
}

// ---- One conversation with the token ----

// The present token whose label is `token`'s, in a session of its own; the user logged in by login().
class Conversation
{
public:
    Conversation() noexcept = default;
    Conversation(const Conversation&)            = delete;
    Conversation& operator=(const Conversation&) = delete;
    ~Conversation() noexcept
    {
        if (m_open)
        {
            (void)m_f->C_CloseSession(m_session);
        }
    }

    [[nodiscard]] CryptoError open(const Pkcs11Token& token, bool writes) noexcept
    {
        if (!token.usable())
        {
            return CryptoError::Unavailable;
        }
        m_f = module_at(token.module());
        if (m_f == nullptr)
        {
            return CryptoError::Unavailable;
        }
        CK_SLOT_ID slots[kSlotsMax];
        CK_ULONG   count = kSlotsMax;
        CK_RV      rv    = m_f->C_GetSlotList(CK_TRUE, slots, &count);
        if (rv != CKR_OK)
        {
            return CryptoError::Unavailable;
        }
        usize matches = 0U;
        for (CK_ULONG i = 0U; i < count; ++i)
        {
            CK_TOKEN_INFO info{};
            if (m_f->C_GetTokenInfo(slots[i], &info) == CKR_OK && labelled(info, token.label()))
            {
                m_slot  = slots[i];
                m_flags = info.flags;
                ++matches;
            }
        }
        if (matches != 1U)
        {
            return CryptoError::Unavailable; // no token, or no single token, of that label
        }
        const CK_FLAGS session_flags = CKF_SERIAL_SESSION | (writes ? CKF_RW_SESSION : 0U);
        rv                           = m_f->C_OpenSession(m_slot, session_flags, nullptr, nullptr, &m_session);
        m_open                       = rv == CKR_OK;
        return failure_of(rv);
    }

    // The user's login: none when a session of the process holds it already; one attempt with the PIN otherwise,
    // never on a token that reports its PIN locked, on its final try or due for a change, nor after `refused`.
    [[nodiscard]] CryptoError login(const Pkcs11Token& token, bool& refused) noexcept
    {
        CK_SESSION_INFO info{};
        const CK_RV     state = m_f->C_GetSessionInfo(m_session, &info);
        if (state != CKR_OK)
        {
            return failure_of(state);
        }
        if (info.state == CKS_RO_USER_FUNCTIONS || info.state == CKS_RW_USER_FUNCTIONS)
        {
            return CryptoError::None;
        }
        constexpr CK_FLAGS spent  = CKF_USER_PIN_LOCKED | CKF_USER_PIN_FINAL_TRY | CKF_USER_PIN_TO_BE_CHANGED;
        const bool         pad    = (m_flags & CKF_PROTECTED_AUTHENTICATION_PATH) != 0U;
        if (refused || (m_flags & spent) != 0U || (m_flags & CKF_USER_PIN_INITIALIZED) == 0U ||
            (token.pin().empty() && !pad))
        {
            return CryptoError::Denied;
        }
        u8 pin[kPkcs11PinMaxBytes];
        std::memcpy(pin, token.pin().data(), token.pin().size());
        const CK_RV rv = m_f->C_Login(m_session, CKU_USER, token.pin().empty() ? nullptr : pin,
                                      static_cast<CK_ULONG>(token.pin().size()));
        secure_zero(cont::Span<u8>(pin));
        switch (rv)
        {
        case CKR_OK:
        case CKR_USER_ALREADY_LOGGED_IN:
            return CryptoError::None;
        case CKR_PIN_INCORRECT:
        case CKR_PIN_INVALID:
        case CKR_PIN_LEN_RANGE:
        case CKR_PIN_LOCKED:
        case CKR_PIN_EXPIRED:
            refused = true;
            return CryptoError::Denied;
        default:
            return failure_of(rv);
        }
    }

    // Whether the token implements `mechanism` with `flag`.
    [[nodiscard]] bool implements(CK_MECHANISM_TYPE mechanism, CK_FLAGS flag) const noexcept
    {
        CK_MECHANISM_INFO info{};
        return m_f->C_GetMechanismInfo(m_slot, mechanism, &info) == CKR_OK && (info.flags & flag) != 0U;
    }

    // Up to `capacity` objects matching `templ`, into `found`.
    [[nodiscard]] CryptoError find(CK_ATTRIBUTE* templ, CK_ULONG attributes, CK_OBJECT_HANDLE* found,
                                   CK_ULONG capacity, CK_ULONG& count) const noexcept
    {
        count    = 0U;
        CK_RV rv = m_f->C_FindObjectsInit(m_session, templ, attributes);
        if (rv != CKR_OK)
        {
            return failure_of(rv);
        }
        rv                = m_f->C_FindObjects(m_session, found, capacity, &count);
        const CK_RV ended = m_f->C_FindObjectsFinal(m_session);
        return failure_of(rv != CKR_OK ? rv : ended);
    }

    [[nodiscard]] const CK_FUNCTION_LIST& f() const noexcept { return *m_f; }
    [[nodiscard]] CK_SESSION_HANDLE       session() const noexcept { return m_session; }

private:
    [[nodiscard]] static bool labelled(const CK_TOKEN_INFO& info, cont::StringView label) noexcept
    {
        if (std::memcmp(info.label, label.data(), label.size()) != 0)
        {
            return false;
        }
        for (usize i = label.size(); i < sizeof(info.label); ++i)
        {
            if (info.label[i] != ' ')
            {
                return false;
            }
        }
        return true;
    }

    const CK_FUNCTION_LIST* m_f       = nullptr;
    CK_SLOT_ID              m_slot    = 0U;
    CK_FLAGS                m_flags   = 0U;
    CK_SESSION_HANDLE       m_session = CK_INVALID_HANDLE;
    bool                    m_open    = false;
};

template <typename Value> [[nodiscard]] CK_ATTRIBUTE attribute(CK_ATTRIBUTE_TYPE type, Value& value) noexcept
{
    return CK_ATTRIBUTE{type, &value, static_cast<CK_ULONG>(sizeof(value))};
}

// Whether a generated private key is kept inside: sensitive and never extractable since its generation, and signing.
[[nodiscard]] bool kept_inside(const Conversation& token, CK_OBJECT_HANDLE key) noexcept
{
    CK_BBOOL     sensitive         = CK_FALSE;
    CK_BBOOL     extractable       = CK_TRUE;
    CK_BBOOL     always_sensitive  = CK_FALSE;
    CK_BBOOL     never_extractable = CK_FALSE;
    CK_BBOOL     signs             = CK_FALSE;
    CK_ATTRIBUTE read[]            = {
        attribute(CKA_SENSITIVE, sensitive),
        attribute(CKA_EXTRACTABLE, extractable),
        attribute(CKA_ALWAYS_SENSITIVE, always_sensitive),
        attribute(CKA_NEVER_EXTRACTABLE, never_extractable),
        attribute(CKA_SIGN, signs),
    };
    return token.f().C_GetAttributeValue(token.session(), key, read, sizeof(read) / sizeof(read[0])) == CKR_OK &&
           sensitive == CK_TRUE && extractable == CK_FALSE && always_sensitive == CK_TRUE &&
           never_extractable == CK_TRUE && signs == CK_TRUE;
}

// The uncompressed point of a public key's CKA_EC_POINT, a DER OCTET STRING around it (PKCS #11 2.40 section 2.3.3).
[[nodiscard]] bool point_of(const Conversation& token, CK_OBJECT_HANDLE key, cont::Span<u8> point) noexcept
{
    u8           encoded[2U + kDevicePublicKeyMaxBytes];
    CK_ATTRIBUTE read = attribute(CKA_EC_POINT, encoded);
    if (token.f().C_GetAttributeValue(token.session(), key, &read, 1U) != CKR_OK ||
        read.ulValueLen != 2U + point.size() || encoded[0] != 0x04U || encoded[1] != point.size() ||
        encoded[2] != 0x04U)
    {
        return false;
    }
    std::memcpy(point.data(), encoded + 2U, point.size());
    return true;
}
} // namespace

Pkcs11Token::Pkcs11Token(cont::StringView module, cont::StringView label, cont::ConstSpan<u8> pin) noexcept
{
    if (module.empty() || module.size() > kPkcs11ModuleMaxBytes || label.empty() || label.size() > kPkcs11LabelBytes ||
        pin.size() > kPkcs11PinMaxBytes || std::memchr(module.data(), '\0', module.size()) != nullptr)
    {
        return;
    }
    std::memcpy(m_module, module.data(), module.size());
    std::memcpy(m_label, label.data(), label.size());
    if (!pin.empty())
    {
        std::memcpy(m_pin, pin.data(), pin.size());
    }
    m_label_bytes = label.size();
    m_pin_bytes   = pin.size();
    m_usable      = true;
}

Pkcs11Token::~Pkcs11Token() noexcept
{
    secure_zero(cont::Span<u8>(m_pin));
}

bool Pkcs11KeyDevice::available(const Pkcs11Token& token) noexcept
{
    const std::lock_guard<std::mutex> lock(pkcs11_lock());
    Conversation                      conversation;
    return conversation.open(token, false) == CryptoError::None;
}

cont::StringView Pkcs11KeyDevice::name() const noexcept
{
    return "pkcs11";
}

CryptoError Pkcs11KeyDevice::create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm, cont::Array<u8>& reference,
                                    cont::Span<u8> public_key) noexcept
{
    reference.clear();
    Curve curve = curve_of(algorithm);
    if (curve.field == 0U)
    {
        return CryptoError::Unsupported;
    }
    if (store_id.size() != kKeyStoreIdBytes || public_key.size() != 1U + 2U * curve.field)
    {
        return CryptoError::BadLength;
    }
    const std::lock_guard<std::mutex> lock(pkcs11_lock());
    Conversation                      token;
    CryptoError                       result = token.open(m_token, true);
    if (result == CryptoError::None &&
        (!token.implements(CKM_EC_KEY_PAIR_GEN, CKF_GENERATE_KEY_PAIR) || !token.implements(CKM_ECDSA, CKF_SIGN)))
    {
        result = CryptoError::Unsupported;
    }
    if (result == CryptoError::None)
    {
        result = token.login(m_token, m_pin_refused);
    }
    u8 handle[kHandleBytes];
    if (result == CryptoError::None)
    {
        result = random_bytes(cont::Span<u8>(handle));
    }
    if (result != CryptoError::None)
    {
        return result;
    }
    char label[kLabelBytes];
    label_of(store_id, label);
    CK_OBJECT_CLASS public_class      = CKO_PUBLIC_KEY;
    CK_OBJECT_CLASS private_class     = CKO_PRIVATE_KEY;
    CK_KEY_TYPE     type              = CKK_EC;
    CK_BBOOL        yes               = CK_TRUE;
    CK_BBOOL        no                = CK_FALSE;
    CK_ATTRIBUTE    public_template[] = {
        attribute(CKA_CLASS, public_class), attribute(CKA_KEY_TYPE, type), attribute(CKA_TOKEN, yes),
        attribute(CKA_PRIVATE, no),         attribute(CKA_VERIFY, yes),    attribute(CKA_ENCRYPT, no),
        attribute(CKA_WRAP, no),            attribute(CKA_DERIVE, no),     curve.params(),
        attribute(CKA_ID, handle),          attribute(CKA_LABEL, label),
    };
    CK_ATTRIBUTE private_template[] = {
        attribute(CKA_CLASS, private_class), attribute(CKA_KEY_TYPE, type), attribute(CKA_TOKEN, yes),
        attribute(CKA_PRIVATE, yes),         attribute(CKA_SENSITIVE, yes), attribute(CKA_EXTRACTABLE, no),
        attribute(CKA_SIGN, yes),            attribute(CKA_DECRYPT, no),    attribute(CKA_UNWRAP, no),
        attribute(CKA_DERIVE, no),           attribute(CKA_ID, handle),     attribute(CKA_LABEL, label),
    };
    CK_MECHANISM     generation{CKM_EC_KEY_PAIR_GEN, nullptr, 0U};
    CK_OBJECT_HANDLE public_half  = CK_INVALID_HANDLE;
    CK_OBJECT_HANDLE private_half = CK_INVALID_HANDLE;
    const CK_RV      rv = token.f().C_GenerateKeyPair(
        token.session(), &generation, public_template, sizeof(public_template) / sizeof(public_template[0]),
        private_template, sizeof(private_template) / sizeof(private_template[0]), &public_half, &private_half);
    result = failure_of(rv);
    if (result != CryptoError::None)
    {
        return result;
    }
    // A token that made the key but did not keep it inside, or answers with a malformed point, keeps neither half.
    public_key[0] = 0x04U;
    if (!kept_inside(token, private_half))
    {
        result = CryptoError::Denied;
    }
    else if (!point_of(token, public_half, public_key))
    {
        result = CryptoError::ProviderFailure;
    }
    if (result != CryptoError::None)
    {
        (void)token.f().C_DestroyObject(token.session(), private_half);
        (void)token.f().C_DestroyObject(token.session(), public_half);
        secure_zero(public_key);
        return result;
    }
    reference.resize(kReferenceBytes);
    reference[0] = kReferenceFormat;
    std::memcpy(reference.data() + 1U, handle, kHandleBytes);
    return CryptoError::None;
}

CryptoError Pkcs11KeyDevice::sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                         AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                         cont::Span<u8> signature) const noexcept
{
    secure_zero(signature);
    Curve curve = curve_of(algorithm);
    if (curve.field == 0U)
    {
        return CryptoError::Unsupported;
    }
    if (store_id.size() != kKeyStoreIdBytes || digest.size() != curve.field || signature.size() != 2U * curve.field)
    {
        return CryptoError::BadLength;
    }
    u8 handle[kHandleBytes];
    if (!parse(store_id, reference, handle))
    {
        return CryptoError::Denied; // not a reference this device wrote
    }
    char label[kLabelBytes];
    label_of(store_id, label);
    const std::lock_guard<std::mutex> lock(pkcs11_lock());
    Conversation                      token;
    CryptoError                       result = token.open(m_token, false);
    if (result == CryptoError::None)
    {
        result = token.login(m_token, m_pin_refused);
    }
    if (result != CryptoError::None)
    {
        return result;
    }
    CK_OBJECT_CLASS  private_class = CKO_PRIVATE_KEY;
    CK_KEY_TYPE      type          = CKK_EC;
    CK_ATTRIBUTE     search[]      = {
        attribute(CKA_CLASS, private_class), attribute(CKA_KEY_TYPE, type), attribute(CKA_ID, handle),
        attribute(CKA_LABEL, label),         curve.params(),
    };
    CK_OBJECT_HANDLE found[2];
    CK_ULONG         count = 0U;
    result = token.find(search, sizeof(search) / sizeof(search[0]), found, 2U, count);
    if (result == CryptoError::None && count != 1U)
    {
        result = CryptoError::Denied; // forgotten, another store's or another curve's
    }
    if (result != CryptoError::None)
    {
        return result;
    }
    CK_MECHANISM mechanism{CKM_ECDSA, nullptr, 0U};
    CK_RV        rv = token.f().C_SignInit(token.session(), &mechanism, found[0]);
    // A key the token makes reauthenticate for each operation (CKA_ALWAYS_AUTHENTICATE) takes the PIN again.
    CK_BBOOL     always = CK_FALSE;
    CK_ATTRIBUTE asked  = attribute(CKA_ALWAYS_AUTHENTICATE, always);
    if (rv == CKR_OK && token.f().C_GetAttributeValue(token.session(), found[0], &asked, 1U) == CKR_OK &&
        always == CK_TRUE)
    {
        if (m_pin_refused || m_token.pin().empty())
        {
            return CryptoError::Denied;
        }
        u8 pin[kPkcs11PinMaxBytes];
        std::memcpy(pin, m_token.pin().data(), m_token.pin().size());
        rv = token.f().C_Login(token.session(), CKU_CONTEXT_SPECIFIC, pin,
                               static_cast<CK_ULONG>(m_token.pin().size()));
        secure_zero(cont::Span<u8>(pin));
        if (rv == CKR_PIN_INCORRECT || rv == CKR_PIN_LOCKED)
        {
            m_pin_refused = true;
            return CryptoError::Denied;
        }
    }
    u8 hashed[48];
    std::memcpy(hashed, digest.data(), digest.size());
    CK_ULONG length = static_cast<CK_ULONG>(signature.size());
    if (rv == CKR_OK)
    {
        rv = token.f().C_Sign(token.session(), hashed, static_cast<CK_ULONG>(digest.size()), signature.data(), &length);
    }
    result = failure_of(rv);
    if (result == CryptoError::None && length != signature.size())
    {
        result = CryptoError::ProviderFailure; // not r || s of the curve
    }
    if (result != CryptoError::None)
    {
        secure_zero(signature);
    }
    return result;
}

CryptoError Pkcs11KeyDevice::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    u8 handle[kHandleBytes];
    if (!parse(store_id, reference, handle))
    {
        return CryptoError::Denied;
    }
    char label[kLabelBytes];
    label_of(store_id, label);
    const std::lock_guard<std::mutex> lock(pkcs11_lock());
    Conversation                      token;
    CryptoError                       result = token.open(m_token, true);
    if (result == CryptoError::None)
    {
        result = token.login(m_token, m_pin_refused);
    }
    // Both halves, and nothing else: an object is destroyed only when its handle and its store's label both match.
    CK_ATTRIBUTE     search[] = {attribute(CKA_ID, handle), attribute(CKA_LABEL, label)};
    CK_OBJECT_HANDLE found[4];
    CK_ULONG         count = 0U;
    if (result == CryptoError::None)
    {
        result = token.find(search, 2U, found, 4U, count);
    }
    for (CK_ULONG i = 0U; result == CryptoError::None && i < count; ++i)
    {
        result = failure_of(token.f().C_DestroyObject(token.session(), found[i]));
    }
    return result;
}
} // namespace crd::crypto
