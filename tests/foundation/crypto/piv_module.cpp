// A PIV card behind a PKCS #11 module, as Yubico's ykcs11 2.7 presents one (NET.2 decision K14; read from
// yubico-piv-tool 2.7.3's ykcs11/ykcs11.c, objects.c and token.c): one slot and token; keys in fixed slots named by a
// one-byte CKA_ID (1 to 25), each with a private and a public key object, the slot's certificate (a token object) and,
// for a key made on the card, an attestation certificate (a session object); fixed labels; C_GenerateKeyPair only under
// the security officer's login, overwriting the slot's key and writing its certificate; C_DestroyObject, also only
// under that login, deleting the slot's certificate and never its key; the login closed with the last session. The
// cryptography is the test's (CrdPivHooks) and each refusal a fault bit (piv_module.hpp). Only the functions the key
// device calls are in the function list; the others are null.
// NOLINTBEGIN(cppcoreguidelines-macro-usage): the exports of a module's C interface
#if defined(_WIN32)
#define CK_DECLARE_FUNCTION(returnType, name) __declspec(dllexport) returnType name
#define CRD_PIV_EXPORT extern "C" __declspec(dllexport)
#else
#define CK_DECLARE_FUNCTION(returnType, name) __attribute__((visibility("default"))) returnType name
#define CRD_PIV_EXPORT extern "C" __attribute__((visibility("default")))
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

#include "piv_module.hpp"

#include "../../../engine/foundation/crypto/src/cryptoki.hpp"

#include <cstring>
#include <mutex>

namespace
{
constexpr CK_ULONG kIds         = 25U; // CKA_ID 1 to 25: 9a, 9c, 9d, 9e, 82 to 95, f9
constexpr CK_ULONG kSessionsMax = 16U;
constexpr CK_ULONG kFoundMax    = 4U * kIds;

constexpr CK_BYTE kP256[] = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
constexpr CK_BYTE kP384[] = {0x06, 0x05, 0x2B, 0x81, 0x04, 0x00, 0x22};

// An object's handle: its kind times 32 plus its slot's CKA_ID.
enum Kind : unsigned char
{
    PrivateKey  = 1U,
    PublicKey   = 2U,
    Certificate = 3U, // the slot's, a token object
    Attestation = 4U, // a session object
};

[[nodiscard]] CK_OBJECT_HANDLE handle_of(CK_ULONG kind, CK_ULONG id)
{
    return kind * 32U + id;
}

struct Slot
{
    int      key         = -1;
    bool     certificate = false;
    bool     local       = false;
    unsigned field       = 0U;
    unsigned generations = 0U;
    CK_BYTE  point[97]   = {};
};

struct Session
{
    bool             open     = false;
    bool             writes   = false;
    CK_OBJECT_HANDLE found[kFoundMax];
    CK_ULONG         found_count = 0U;
    CK_ULONG         found_next  = 0U;
    bool             finding     = false;
    CK_OBJECT_HANDLE signing     = CK_INVALID_HANDLE;
    bool             context     = false; // CKU_CONTEXT_SPECIFIC given for the operation
};

enum class Login : unsigned char
{
    Public,
    User,
    So,
};

struct Card
{
    std::mutex   lock;
    CrdPivHooks  hooks{};
    unsigned     faults      = 0U;
    bool         initialized = false;
    Slot         slots[kIds + 1U];
    Session      sessions[kSessionsMax];
    Login        login = Login::Public;
    CrdPivCounts counts{};
};

Card& card()
{
    static Card instance;
    return instance;
}

[[nodiscard]] bool fault(const Card& c, unsigned bit)
{
    return (c.faults & bit) != 0U;
}

[[nodiscard]] Session* session_of(Card& c, CK_SESSION_HANDLE handle)
{
    return handle >= 1U && handle <= kSessionsMax && c.sessions[handle - 1U].open ? &c.sessions[handle - 1U] : nullptr;
}

// Whether the object behind `handle` exists now: a slot's keys and attestation while it holds a key made on the card
// (ykcs11 finds them through the attestation), its certificate while written.
[[nodiscard]] bool exists(const Card& c, CK_OBJECT_HANDLE handle)
{
    const CK_ULONG kind = handle / 32U;
    const CK_ULONG id   = handle % 32U;
    if (id < 1U || id > kIds)
    {
        return false;
    }
    const Slot& slot = c.slots[id];
    switch (kind)
    {
    case PrivateKey:
    case PublicKey:
        return slot.key >= 0;
    case Certificate:
        return slot.certificate;
    case Attestation:
        return slot.key >= 0 && slot.local;
    default:
        return false;
    }
}

// One attribute's value, into a scratch buffer: false when the object has no such attribute.
[[nodiscard]] bool value_of(const Card& c, CK_OBJECT_HANDLE handle, CK_ATTRIBUTE_TYPE type, CK_BYTE (&scratch)[128],
                            CK_ULONG& length)
{
    const CK_ULONG kind = handle / 32U;
    const Slot&    slot = c.slots[handle % 32U];
    CK_ULONG       word = 0U;
    CK_BBOOL       flag = CK_FALSE;
    const auto     put  = [&](const void* bytes, CK_ULONG count)
    {
        std::memcpy(scratch, bytes, count);
        length = count;
        return true;
    };
    const auto put_word = [&](CK_ULONG value)
    {
        word = value;
        return put(&word, sizeof(word));
    };
    const auto put_flag = [&](bool value)
    {
        flag = value ? CK_TRUE : CK_FALSE;
        return put(&flag, sizeof(flag));
    };
    switch (type)
    {
    case CKA_CLASS:
        if (kind == PrivateKey)
        {
            return put_word(CKO_PRIVATE_KEY);
        }
        return put_word(kind == PublicKey ? CKO_PUBLIC_KEY : CKO_CERTIFICATE);
    case CKA_ID:
    {
        const CK_BYTE id = static_cast<CK_BYTE>(handle % 32U);
        return put(&id, 1U);
    }
    case CKA_TOKEN:
        return put_flag(kind != Attestation);
    case CKA_LABEL:
        return put("Retired Key", 11U); // ykcs11's labels are fixed per object
    default:
        break;
    }
    if (kind == Certificate || kind == Attestation)
    {
        return false;
    }
    const bool p256 = slot.field == 32U;
    switch (type)
    {
    case CKA_KEY_TYPE:
        return put_word(CKK_EC);
    case CKA_EC_PARAMS:
        return p256 ? put(kP256, sizeof(kP256)) : put(kP384, sizeof(kP384));
    case CKA_PRIVATE:
        return put_flag(kind == PrivateKey);
    default:
        break;
    }
    if (kind == PublicKey)
    {
        switch (type)
        {
        case CKA_VERIFY:
            return put_flag(true);
        case CKA_EC_POINT:
        {
            const CK_ULONG point = 1U + 2U * slot.field;
            scratch[0]           = fault(c, CrdPivBadPoint) ? 0x03U : 0x04U;
            scratch[1]           = static_cast<CK_BYTE>(point);
            std::memcpy(scratch + 2U, slot.point, point);
            length = 2U + point;
            return true;
        }
        default:
            return false;
        }
    }
    switch (type)
    {
    case CKA_SENSITIVE:
    case CKA_ALWAYS_SENSITIVE:
    case CKA_NEVER_EXTRACTABLE: // ykcs11 answers these for an imported key too
    case CKA_SIGN:
    case CKA_DERIVE:
        return put_flag(true);
    case CKA_EXTRACTABLE:
    case CKA_DECRYPT:
        return put_flag(false);
    case CKA_LOCAL:
        return put_flag(slot.local && !fault(c, CrdPivImported));
    case CKA_ALWAYS_AUTHENTICATE:
        return put_flag(fault(c, CrdPivAlwaysAuthenticate));
    default:
        return false;
    }
}

// ---- Templates, as ykcs11's check_ec_pubkey_template and check_pvtkey_template take them ----

[[nodiscard]] bool flag_of(const CK_ATTRIBUTE& attribute, CK_BBOOL& value)
{
    if (attribute.pValue == nullptr || attribute.ulValueLen != sizeof(CK_BBOOL))
    {
        return false;
    }
    std::memcpy(&value, attribute.pValue, sizeof(value));
    return true;
}

[[nodiscard]] bool word_of(const CK_ATTRIBUTE& attribute, CK_ULONG& value)
{
    if (attribute.pValue == nullptr || attribute.ulValueLen != sizeof(CK_ULONG))
    {
        return false;
    }
    std::memcpy(&value, attribute.pValue, sizeof(value));
    return true;
}

// The slot a template's CKA_ID names (its first byte, as ykcs11 reads it), 0 when it names none.
[[nodiscard]] CK_RV id_of(const CK_ATTRIBUTE& attribute, CK_BYTE& id)
{
    if (attribute.pValue == nullptr || attribute.ulValueLen == 0U)
    {
        return CKR_ATTRIBUTE_VALUE_INVALID;
    }
    CK_BYTE named = 0U;
    std::memcpy(&named, attribute.pValue, 1U);
    if (named < 1U || named > kIds || (id != 0U && id != named))
    {
        return named < 1U || named > kIds ? CKR_ATTRIBUTE_VALUE_INVALID : CKR_TEMPLATE_INCONSISTENT;
    }
    id = named;
    return CKR_OK;
}

[[nodiscard]] CK_RV public_template(const CK_ATTRIBUTE* attributes, CK_ULONG count, CK_BYTE& id, unsigned& field)
{
    for (CK_ULONG i = 0U; i < count; ++i)
    {
        const CK_ATTRIBUTE& attribute = attributes[i];
        CK_ULONG            word      = 0U;
        CK_RV               rv        = CKR_OK;
        switch (attribute.type)
        {
        case CKA_CLASS:
            rv = word_of(attribute, word) && word == CKO_PUBLIC_KEY ? CKR_OK : CKR_TEMPLATE_INCONSISTENT;
            break;
        case CKA_KEY_TYPE:
            rv = word_of(attribute, word) && word == CKK_EC ? CKR_OK : CKR_TEMPLATE_INCONSISTENT;
            break;
        case CKA_EC_PARAMS:
            if (attribute.ulValueLen == sizeof(kP256) && std::memcmp(attribute.pValue, kP256, sizeof(kP256)) == 0)
            {
                field = 32U;
            }
            else if (attribute.ulValueLen == sizeof(kP384) &&
                     std::memcmp(attribute.pValue, kP384, sizeof(kP384)) == 0)
            {
                field = 48U;
            }
            else
            {
                rv = CKR_ATTRIBUTE_VALUE_INVALID;
            }
            break;
        case CKA_ID:
            rv = id_of(attribute, id);
            break;
        case CKA_COPYABLE:
        case CKA_DESTROYABLE:
        case CKA_EXTRACTABLE:
        case CKA_SENSITIVE:
        case CKA_TOKEN:
        case CKA_ENCRYPT:
        case CKA_DECRYPT:
        case CKA_SIGN:
        case CKA_SIGN_RECOVER:
        case CKA_VERIFY:
        case CKA_VERIFY_RECOVER:
        case CKA_WRAP:
        case CKA_UNWRAP:
        case CKA_DERIVE:
        case CKA_PRIVATE:
        case CKA_LABEL:
        case CKA_ISSUER:
        case CKA_SUBJECT:
            break; // ignored, as ykcs11 does
        default:
            rv = CKR_ATTRIBUTE_TYPE_INVALID;
            break;
        }
        if (rv != CKR_OK)
        {
            return rv;
        }
    }
    return CKR_OK;
}

[[nodiscard]] CK_RV private_template(const CK_ATTRIBUTE* attributes, CK_ULONG count, CK_BYTE& id)
{
    for (CK_ULONG i = 0U; i < count; ++i)
    {
        const CK_ATTRIBUTE& attribute = attributes[i];
        CK_ULONG            word      = 0U;
        CK_BBOOL            flag      = CK_FALSE;
        CK_RV               rv        = CKR_OK;
        switch (attribute.type)
        {
        case CKA_CLASS: // NOLINT(bugprone-branch-clone): CKO_PRIVATE_KEY and CKK_EC are both 3
            rv = word_of(attribute, word) && word == CKO_PRIVATE_KEY ? CKR_OK : CKR_TEMPLATE_INCONSISTENT;
            break;
        case CKA_KEY_TYPE:
            rv = word_of(attribute, word) && word == CKK_EC ? CKR_OK : CKR_TEMPLATE_INCONSISTENT;
            break;
        case CKA_ID:
            rv = id_of(attribute, id);
            break;
        case CKA_TOKEN:
        case CKA_PRIVATE:
        case CKA_SENSITIVE:
            rv = flag_of(attribute, flag) && flag == CK_TRUE ? CKR_OK : CKR_ATTRIBUTE_VALUE_INVALID;
            break;
        case CKA_EXTRACTABLE:
            rv = flag_of(attribute, flag) && flag == CK_FALSE ? CKR_OK : CKR_ATTRIBUTE_VALUE_INVALID;
            break;
        case CKA_ALWAYS_AUTHENTICATE:
        case CKA_COPYABLE:
        case CKA_DESTROYABLE:
        case CKA_ENCRYPT:
        case CKA_DECRYPT:
        case CKA_WRAP:
        case CKA_UNWRAP:
        case CKA_SIGN:
        case CKA_SIGN_RECOVER:
        case CKA_VERIFY:
        case CKA_VERIFY_RECOVER:
        case CKA_DERIVE:
        case CKA_LABEL:
        case CKA_ISSUER:
        case CKA_SUBJECT:
            break;
        default:
            rv = CKR_ATTRIBUTE_TYPE_INVALID;
            break;
        }
        if (rv != CKR_OK)
        {
            return rv;
        }
    }
    return CKR_OK;
}

// ---- The entry points ----

CK_RV initialize(CK_VOID_PTR /*arguments*/)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    if (c.initialized)
    {
        return CKR_CRYPTOKI_ALREADY_INITIALIZED;
    }
    c.initialized = true;
    return CKR_OK;
}

CK_RV finalize(CK_VOID_PTR /*reserved*/)
{
    return CKR_OK;
}

CK_RV slot_list(CK_BBOOL /*present*/, CK_SLOT_ID_PTR slots, CK_ULONG_PTR count)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    const CK_ULONG                    present = fault(c, CrdPivRemoved) ? 0U : 1U;
    if (slots != nullptr && *count < present)
    {
        *count = present;
        return CKR_BUFFER_TOO_SMALL;
    }
    if (slots != nullptr && present == 1U)
    {
        slots[0] = 0U;
    }
    *count = present;
    return CKR_OK;
}

CK_RV token_info(CK_SLOT_ID slot, CK_TOKEN_INFO_PTR info)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    if (slot != 0U || fault(c, CrdPivRemoved))
    {
        return CKR_TOKEN_NOT_PRESENT;
    }
    *info = CK_TOKEN_INFO{};
    std::memset(info->label, ' ', sizeof(info->label)); // blank-padded, as every token's label
    std::memcpy(info->label, kCrdPivLabel, sizeof(kCrdPivLabel) - 1U);
    info->flags = CKF_RNG | CKF_LOGIN_REQUIRED | CKF_USER_PIN_INITIALIZED | CKF_TOKEN_INITIALIZED |
                  (fault(c, CrdPivSoFinalTry) ? CKF_SO_PIN_FINAL_TRY : 0U);
    info->ulMaxSessionCount = kSessionsMax;
    info->ulMaxPinLen       = 8U;
    info->ulMinPinLen       = 6U;
    return CKR_OK;
}

CK_RV mechanism_info(CK_SLOT_ID /*slot*/, CK_MECHANISM_TYPE type, CK_MECHANISM_INFO_PTR info)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    *info = CK_MECHANISM_INFO{256U, 384U, 0U};
    if (type == CKM_EC_KEY_PAIR_GEN && !fault(c, CrdPivNoEcGeneration))
    {
        info->flags = CKF_HW | CKF_GENERATE_KEY_PAIR | CKF_EC_F_P;
        return CKR_OK;
    }
    if (type == CKM_ECDSA)
    {
        info->flags = CKF_HW | CKF_SIGN;
        return CKR_OK;
    }
    return CKR_MECHANISM_INVALID;
}

CK_RV open_session(CK_SLOT_ID slot, CK_FLAGS flags, CK_VOID_PTR /*application*/, CK_NOTIFY /*notify*/,
                   CK_SESSION_HANDLE_PTR handle)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    if (slot != 0U || fault(c, CrdPivRemoved))
    {
        return CKR_TOKEN_NOT_PRESENT;
    }
    if ((flags & CKF_SERIAL_SESSION) == 0U)
    {
        return CKR_SESSION_PARALLEL_NOT_SUPPORTED;
    }
    for (CK_ULONG i = 0U; i < kSessionsMax; ++i)
    {
        if (!c.sessions[i].open)
        {
            c.sessions[i]        = Session();
            c.sessions[i].open   = true;
            c.sessions[i].writes = (flags & CKF_RW_SESSION) != 0U;
            *handle              = i + 1U;
            return CKR_OK;
        }
    }
    return CKR_SESSION_COUNT;
}

CK_RV close_session(CK_SESSION_HANDLE handle)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    Session* const                    session = session_of(c, handle);
    if (session == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    session->open = false;
    bool others   = false;
    for (const Session& s : c.sessions)
    {
        others = others || s.open;
    }
    if (!others)
    {
        c.login = Login::Public; // ykcs11's cleanup_slot: the last session closes the login
    }
    return CKR_OK;
}

CK_RV session_info(CK_SESSION_HANDLE handle, CK_SESSION_INFO_PTR info)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    const Session* const              session = session_of(c, handle);
    if (session == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    info->slotID        = 0U;
    info->flags         = CKF_SERIAL_SESSION | (session->writes ? CKF_RW_SESSION : 0U);
    info->ulDeviceError = 0U;
    switch (c.login)
    {
    case Login::Public:
        info->state = session->writes ? CKS_RW_PUBLIC_SESSION : CKS_RO_PUBLIC_SESSION;
        break;
    case Login::User:
        info->state = session->writes ? CKS_RW_USER_FUNCTIONS : CKS_RO_USER_FUNCTIONS;
        break;
    case Login::So:
        info->state = CKS_RW_SO_FUNCTIONS;
        break;
    }
    return CKR_OK;
}

[[nodiscard]] bool secret_is(CK_UTF8CHAR_PTR given, CK_ULONG length, const char* expected)
{
    return given != nullptr && length == std::strlen(expected) && std::memcmp(given, expected, length) == 0;
}

CK_RV log_in(CK_SESSION_HANDLE handle, CK_USER_TYPE user, CK_UTF8CHAR_PTR secret, CK_ULONG length)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    Session* const                    session = session_of(c, handle);
    if (session == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    switch (user)
    {
    case CKU_USER:
        if (c.login != Login::Public)
        {
            return c.login == Login::User ? CKR_USER_ALREADY_LOGGED_IN : CKR_USER_ANOTHER_ALREADY_LOGGED_IN;
        }
        ++c.counts.user;
        if (fault(c, CrdPivUserRefused) || !secret_is(secret, length, kCrdPivPin))
        {
            return CKR_PIN_INCORRECT;
        }
        c.login = Login::User;
        return CKR_OK;
    case CKU_SO:
        if (c.login != Login::Public)
        {
            return c.login == Login::So ? CKR_USER_ALREADY_LOGGED_IN : CKR_USER_ANOTHER_ALREADY_LOGGED_IN;
        }
        for (const Session& s : c.sessions)
        {
            if (s.open && !s.writes)
            {
                return CKR_SESSION_READ_ONLY_EXISTS;
            }
        }
        if (fault(c, CrdPivReadOnlyExists))
        {
            return CKR_SESSION_READ_ONLY_EXISTS;
        }
        ++c.counts.so;
        if (fault(c, CrdPivSoRefused) || !secret_is(secret, length, kCrdPivSo))
        {
            return CKR_PIN_INCORRECT;
        }
        c.login = Login::So;
        return CKR_OK;
    case CKU_CONTEXT_SPECIFIC:
        ++c.counts.context;
        if (c.login != Login::User || session->signing == CK_INVALID_HANDLE)
        {
            return CKR_OPERATION_NOT_INITIALIZED;
        }
        if (fault(c, CrdPivUserRefused) || !secret_is(secret, length, kCrdPivPin))
        {
            return CKR_PIN_INCORRECT;
        }
        session->context = true;
        return CKR_OK;
    default:
        return CKR_USER_TYPE_INVALID;
    }
}

CK_RV attribute_value(CK_SESSION_HANDLE handle, CK_OBJECT_HANDLE object, CK_ATTRIBUTE_PTR attributes, CK_ULONG count)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    if (session_of(c, handle) == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    if (!exists(c, object))
    {
        return CKR_OBJECT_HANDLE_INVALID;
    }
    CK_RV rv = CKR_OK;
    for (CK_ULONG i = 0U; i < count; ++i)
    {
        CK_BYTE  scratch[128];
        CK_ULONG length = 0U;
        if (!value_of(c, object, attributes[i].type, scratch, length))
        {
            attributes[i].ulValueLen = CK_UNAVAILABLE_INFORMATION;
            rv                       = CKR_ATTRIBUTE_TYPE_INVALID;
        }
        else if (attributes[i].pValue == nullptr)
        {
            attributes[i].ulValueLen = length;
        }
        else if (attributes[i].ulValueLen < length)
        {
            attributes[i].ulValueLen = CK_UNAVAILABLE_INFORMATION;
            rv                       = CKR_BUFFER_TOO_SMALL;
        }
        else
        {
            std::memcpy(attributes[i].pValue, scratch, length);
            attributes[i].ulValueLen = length;
        }
    }
    return rv;
}

CK_RV find_init(CK_SESSION_HANDLE handle, CK_ATTRIBUTE_PTR terms, CK_ULONG count)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    Session* const                    session = session_of(c, handle);
    if (session == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    if (session->finding)
    {
        return CKR_OPERATION_ACTIVE;
    }
    session->finding     = true;
    session->found_count = 0U;
    session->found_next  = 0U;
    for (CK_ULONG kind = PrivateKey; kind <= Attestation; ++kind)
    {
        for (CK_ULONG id = 1U; id <= kIds; ++id)
        {
            const CK_OBJECT_HANDLE object = handle_of(kind, id);
            bool                   match  = exists(c, object);
            for (CK_ULONG i = 0U; match && i < count; ++i)
            {
                CK_BYTE  scratch[128];
                CK_ULONG length = 0U;
                match = value_of(c, object, terms[i].type, scratch, length) && length == terms[i].ulValueLen &&
                        std::memcmp(scratch, terms[i].pValue, length) == 0;
            }
            if (match)
            {
                session->found[session->found_count++] = object;
            }
        }
    }
    return CKR_OK;
}

CK_RV find(CK_SESSION_HANDLE handle, CK_OBJECT_HANDLE_PTR objects, CK_ULONG capacity, CK_ULONG_PTR count)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    Session* const                    session = session_of(c, handle);
    if (session == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    if (!session->finding)
    {
        return CKR_OPERATION_NOT_INITIALIZED;
    }
    *count = 0U;
    while (*count < capacity && session->found_next < session->found_count)
    {
        objects[(*count)++] = session->found[session->found_next++];
    }
    return CKR_OK;
}

CK_RV find_final(CK_SESSION_HANDLE handle)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    Session* const                    session = session_of(c, handle);
    if (session == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    session->finding = false;
    return CKR_OK;
}

CK_RV generate_key_pair(CK_SESSION_HANDLE handle, CK_MECHANISM_PTR mechanism, CK_ATTRIBUTE_PTR public_terms,
                        CK_ULONG public_count, CK_ATTRIBUTE_PTR private_terms, CK_ULONG private_count,
                        CK_OBJECT_HANDLE_PTR public_half, CK_OBJECT_HANDLE_PTR private_half)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    if (session_of(c, handle) == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    if (mechanism == nullptr || mechanism->mechanism != CKM_EC_KEY_PAIR_GEN || fault(c, CrdPivNoEcGeneration))
    {
        return CKR_MECHANISM_INVALID;
    }
    CK_BYTE  id    = 0U;
    unsigned field = 0U;
    CK_RV    rv    = public_template(public_terms, public_count, id, field);
    if (rv == CKR_OK)
    {
        rv = private_template(private_terms, private_count, id);
    }
    if (rv != CKR_OK)
    {
        return rv;
    }
    if (field == 0U || id == 0U)
    {
        return CKR_TEMPLATE_INCOMPLETE;
    }
    if (c.login != Login::So)
    {
        return CKR_USER_TYPE_INVALID; // "Authentication as SO required to generate keys"
    }
    if (fault(c, CrdPivGenerationFails) || c.hooks.generate == nullptr)
    {
        return CKR_DEVICE_ERROR;
    }
    Slot&     slot = c.slots[id];
    const int key  = c.hooks.generate(c.hooks.context, field, slot.point);
    if (key < 0)
    {
        return CKR_DEVICE_ERROR;
    }
    slot.key         = key;
    slot.field       = field;
    slot.local       = true;
    slot.certificate = true; // token_generate_key writes the slot's certificate with the key
    ++slot.generations;
    *public_half  = handle_of(PublicKey, id);
    *private_half = handle_of(PrivateKey, id);
    return CKR_OK;
}

CK_RV destroy_object(CK_SESSION_HANDLE handle, CK_OBJECT_HANDLE object)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    if (session_of(c, handle) == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    const CK_ULONG id = object % 32U;
    if (id < 1U || id > kIds)
    {
        return CKR_OBJECT_HANDLE_INVALID;
    }
    if (c.login != Login::So)
    {
        return CKR_USER_TYPE_INVALID;
    }
    c.slots[id].certificate = false; // token_delete_cert: the slot's key stays
    return CKR_OK;
}

CK_RV sign_init(CK_SESSION_HANDLE handle, CK_MECHANISM_PTR mechanism, CK_OBJECT_HANDLE key)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    Session* const                    session = session_of(c, handle);
    if (session == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    if (mechanism == nullptr || mechanism->mechanism != CKM_ECDSA)
    {
        return CKR_MECHANISM_INVALID;
    }
    if (key / 32U != PrivateKey || !exists(c, key))
    {
        return CKR_KEY_HANDLE_INVALID;
    }
    if (c.login != Login::User)
    {
        return CKR_USER_NOT_LOGGED_IN;
    }
    session->signing = key;
    session->context = false;
    return CKR_OK;
}

CK_RV sign(CK_SESSION_HANDLE handle, CK_BYTE_PTR digest, CK_ULONG digest_bytes, CK_BYTE_PTR signature,
           CK_ULONG_PTR signature_bytes)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    Session* const                    session = session_of(c, handle);
    if (session == nullptr)
    {
        return CKR_SESSION_HANDLE_INVALID;
    }
    if (session->signing == CK_INVALID_HANDLE)
    {
        return CKR_OPERATION_NOT_INITIALIZED;
    }
    const Slot&    slot = c.slots[session->signing % 32U];
    const CK_ULONG full = 2U * slot.field;
    const CK_ULONG made = fault(c, CrdPivShortSignature) ? full - 1U : full;
    if (signature == nullptr)
    {
        *signature_bytes = made;
        return CKR_OK;
    }
    session->signing = CK_INVALID_HANDLE;
    if (fault(c, CrdPivAlwaysAuthenticate) && !session->context)
    {
        return CKR_USER_NOT_LOGGED_IN;
    }
    if (*signature_bytes < made)
    {
        return CKR_BUFFER_TOO_SMALL;
    }
    if (digest_bytes != slot.field || c.hooks.sign == nullptr ||
        c.hooks.sign(c.hooks.context, slot.key, digest, digest_bytes, signature, full) != 0)
    {
        return CKR_FUNCTION_FAILED;
    }
    *signature_bytes = made;
    return CKR_OK;
}

CK_FUNCTION_LIST make_list()
{
    CK_FUNCTION_LIST list{};
    list.version             = CK_VERSION{2U, 40U};
    list.C_Initialize        = &initialize;
    list.C_Finalize          = &finalize;
    list.C_GetSlotList       = &slot_list;
    list.C_GetTokenInfo      = &token_info;
    list.C_GetMechanismInfo  = &mechanism_info;
    list.C_OpenSession       = &open_session;
    list.C_CloseSession      = &close_session;
    list.C_GetSessionInfo    = &session_info;
    list.C_Login             = &log_in;
    list.C_GetAttributeValue = &attribute_value;
    list.C_FindObjectsInit   = &find_init;
    list.C_FindObjects       = &find;
    list.C_FindObjectsFinal  = &find_final;
    list.C_GenerateKeyPair   = &generate_key_pair;
    list.C_DestroyObject     = &destroy_object;
    list.C_SignInit          = &sign_init;
    list.C_Sign              = &sign;
    return list;
}
} // namespace

CK_RV C_GetFunctionList(CK_FUNCTION_LIST_PTR_PTR list)
{
    static CK_FUNCTION_LIST functions = make_list();
    *list                             = &functions;
    return CKR_OK;
}

CRD_PIV_EXPORT void crd_piv_reset(const CrdPivHooks* hooks)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    c.hooks  = hooks != nullptr ? *hooks : CrdPivHooks{};
    c.faults = 0U;
    c.login  = Login::Public;
    c.counts = CrdPivCounts{};
    for (Slot& slot : c.slots)
    {
        slot = Slot();
    }
    for (Session& session : c.sessions)
    {
        session = Session();
    }
}

CRD_PIV_EXPORT void crd_piv_faults(unsigned faults)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    c.faults = faults;
}

CRD_PIV_EXPORT void crd_piv_occupy(unsigned id, int key, int certificate)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    if (id < 1U || id > kIds)
    {
        return;
    }
    Slot& slot       = c.slots[id];
    slot.key         = key;
    slot.certificate = certificate != 0;
    slot.local       = false; // another application's key, imported or made elsewhere
    if (key >= 0)
    {
        slot.field = 32U;
        slot.point[0] = 0x04U;
    }
}

CRD_PIV_EXPORT void crd_piv_read(unsigned id, CrdPivSlot* out)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    *out = CrdPivSlot{-1, 0, 0U, 0U, {}};
    if (id < 1U || id > kIds)
    {
        return;
    }
    const Slot& slot = c.slots[id];
    out->key         = slot.key;
    out->certificate = slot.certificate ? 1 : 0;
    out->field       = slot.field;
    out->generations = slot.generations;
    std::memcpy(out->point, slot.point, sizeof(out->point));
}

CRD_PIV_EXPORT void crd_piv_logins(CrdPivCounts* out)
{
    Card&                       c = card();
    const std::lock_guard<std::mutex> lock(c.lock);
    *out = c.counts;
}
