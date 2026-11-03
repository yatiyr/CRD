// crd-crypto -- the TPM 2.0 key protector and key device on Linux (NET.2; contract:
// docs/design/network-foundation.md#key-store, decisions K11 to K13), through the TCG's Enhanced System API (tss2).
//
// libtss2-esys, libtss2-tctildr and libtss2-mu are loaded at run time and never unloaded, so nothing links them and a
// system without them answers Unavailable. Each call opens its own TCTI and ESYS context under one process-wide lock
// (a TPM runs one command at a time, and a simulator's socket takes one client), creates the TCG's ECC P-256 storage
// root transiently in the owner hierarchy and starts a session salted to it (AES-128-CFB parameter encryption), so
// a master key, a key's authorization and an index's contents never cross the bus in the clear. What a store keeps in
// the TPM lives in an ordinary NV index of the owner's range: the master key itself (the protector), or the TPM-wrapped
// blob of a sign-only ECDSA key (the device). The index and the key are authorized by HMAC-SHA256 of the store's
// identifier under the reference's random secret, TPMA_NV_NO_DA and TPMA_OBJECT_NODA keeping a probe with another
// store's identifier out of the dictionary-attack counter; an index is written once and write-locked
// (TPMA_NV_WRITEDEFINE), and forgetting deletes it after proving its authorization, so no other index is touched.
#include <crd/core/platform.hpp>

#if CRD_OS_LINUX
#include <crd/crypto/key_store.hpp>
#include <crd/platform/dynamic_library.hpp>

#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_tctildr.h>

#include <cstring>
#include <mutex>
#include <new>

namespace crd::crypto
{
namespace
{
// ---- tss2, resolved once ----

struct Tss2
{
    platform::DynamicLibrary esys_library;
    platform::DynamicLibrary loader_library;
    platform::DynamicLibrary mu_library;
    decltype(&Tss2_TctiLdr_Initialize) tcti_initialize = nullptr;
    decltype(&Tss2_TctiLdr_Finalize) tcti_finalize = nullptr;
    decltype(&Esys_Initialize) initialize = nullptr;
    decltype(&Esys_Finalize) finalize = nullptr;
    decltype(&Esys_Free) free = nullptr;
    decltype(&Esys_GetCapability) get_capability = nullptr;
    decltype(&Esys_CreatePrimary) create_primary = nullptr;
    decltype(&Esys_StartAuthSession) start_auth_session = nullptr;
    decltype(&Esys_TRSess_SetAttributes) set_session_attributes = nullptr;
    decltype(&Esys_TR_SetAuth) set_auth = nullptr;
    decltype(&Esys_TR_FromTPMPublic) from_tpm_public = nullptr;
    decltype(&Esys_TR_Close) close = nullptr;
    decltype(&Esys_FlushContext) flush_context = nullptr;
    decltype(&Esys_NV_DefineSpace) nv_define_space = nullptr;
    decltype(&Esys_NV_UndefineSpace) nv_undefine_space = nullptr;
    decltype(&Esys_NV_ReadPublic) nv_read_public = nullptr;
    decltype(&Esys_NV_Write) nv_write = nullptr;
    decltype(&Esys_NV_WriteLock) nv_write_lock = nullptr;
    decltype(&Esys_NV_Read) nv_read = nullptr;
    decltype(&Esys_Create) create = nullptr;
    decltype(&Esys_Load) load = nullptr;
    decltype(&Esys_Sign) sign = nullptr;
    decltype(&Tss2_MU_TPM2B_PUBLIC_Marshal) public_marshal = nullptr;
    decltype(&Tss2_MU_TPM2B_PUBLIC_Unmarshal) public_unmarshal = nullptr;
    decltype(&Tss2_MU_TPM2B_PRIVATE_Marshal) private_marshal = nullptr;
    decltype(&Tss2_MU_TPM2B_PRIVATE_Unmarshal) private_unmarshal = nullptr;
    bool loaded = false;
};

template <typename Function> bool bind(const platform::DynamicLibrary& library, Function& function, const char* name)
{
    function = library.resolve_as<Function>(name);
    return function != nullptr;
}

Tss2 load_tss2() noexcept
{
    Tss2 t;
    t.esys_library = platform::DynamicLibrary::open(platform::fs::Path("libtss2-esys.so.0"), false);
    t.loader_library = platform::DynamicLibrary::open(platform::fs::Path("libtss2-tctildr.so.0"), false);
    t.mu_library = platform::DynamicLibrary::open(platform::fs::Path("libtss2-mu.so.0"), false);
    if (!t.esys_library.is_valid() || !t.loader_library.is_valid() || !t.mu_library.is_valid())
    {
        return t;
    }
    const platform::DynamicLibrary& e = t.esys_library;
    const platform::DynamicLibrary& m = t.mu_library;
    t.loaded = bind(t.loader_library, t.tcti_initialize, "Tss2_TctiLdr_Initialize") &&
               bind(t.loader_library, t.tcti_finalize, "Tss2_TctiLdr_Finalize") &&
               bind(e, t.initialize, "Esys_Initialize") && bind(e, t.finalize, "Esys_Finalize") &&
               bind(e, t.free, "Esys_Free") && bind(e, t.get_capability, "Esys_GetCapability") &&
               bind(e, t.create_primary, "Esys_CreatePrimary") &&
               bind(e, t.start_auth_session, "Esys_StartAuthSession") &&
               bind(e, t.set_session_attributes, "Esys_TRSess_SetAttributes") &&
               bind(e, t.set_auth, "Esys_TR_SetAuth") && bind(e, t.from_tpm_public, "Esys_TR_FromTPMPublic") &&
               bind(e, t.close, "Esys_TR_Close") && bind(e, t.flush_context, "Esys_FlushContext") &&
               bind(e, t.nv_define_space, "Esys_NV_DefineSpace") &&
               bind(e, t.nv_undefine_space, "Esys_NV_UndefineSpace") &&
               bind(e, t.nv_read_public, "Esys_NV_ReadPublic") && bind(e, t.nv_write, "Esys_NV_Write") &&
               bind(e, t.nv_write_lock, "Esys_NV_WriteLock") && bind(e, t.nv_read, "Esys_NV_Read") &&
               bind(e, t.create, "Esys_Create") && bind(e, t.load, "Esys_Load") && bind(e, t.sign, "Esys_Sign") &&
               bind(m, t.public_marshal, "Tss2_MU_TPM2B_PUBLIC_Marshal") &&
               bind(m, t.public_unmarshal, "Tss2_MU_TPM2B_PUBLIC_Unmarshal") &&
               bind(m, t.private_marshal, "Tss2_MU_TPM2B_PRIVATE_Marshal") &&
               bind(m, t.private_unmarshal, "Tss2_MU_TPM2B_PRIVATE_Unmarshal");
    return t;
}

// Loaded once and never unloaded: ESYS initialises its crypto backend's process-wide state, which must stay mapped.
const Tss2& tss2() noexcept
{
    alignas(Tss2) static unsigned char storage[sizeof(Tss2)];
    static const Tss2* const kLoaded = ::new (static_cast<void*>(storage)) Tss2(load_tss2());
    return *kLoaded;
}

// One TPM conversation at a time in this process.
std::mutex& tpm_lock() noexcept
{
    static std::mutex lock;
    return lock;
}

// ---- Response codes ----

// A code from the TPM itself (tss2's layer 0); anything else is the library's or the transport's.
[[nodiscard]] bool from_tpm(TSS2_RC rc) noexcept
{
    return (rc & TSS2_RC_LAYER_MASK) == 0U;
}

// The TPM's code without the handle, session or parameter number a format-one code carries.
[[nodiscard]] TSS2_RC base_of(TSS2_RC rc) noexcept
{
    const TSS2_RC code = rc & 0xFFFFU;
    return (code & TPM2_RC_FMT1) != 0U ? code & (TPM2_RC_FMT1 | 0x3FU) : code;
}

[[nodiscard]] bool is(TSS2_RC rc, TSS2_RC expected) noexcept
{
    return from_tpm(rc) && base_of(rc) == expected;
}

// A failure that is not one a caller tells apart: the TPM's own refusals are a provider failure, the rest (no TPM, a
// lost connection, a library error) Unavailable.
[[nodiscard]] CryptoError failure_of(TSS2_RC rc) noexcept
{
    return from_tpm(rc) ? CryptoError::ProviderFailure : CryptoError::Unavailable;
}

// ---- Constants and references ----

constexpr u8    kReferenceFormat = 1U;
constexpr usize kSecretBytes     = 32U;
constexpr usize kReferenceBytes  = 1U + 4U + kSecretBytes; // the format, the index's handle, the secret
constexpr usize kAuthBytes       = 32U;                    // SHA-256, the objects' name algorithm
constexpr u32   kOwnerIndexFirst = 0x01000000U;            // the owner's NV range of the TCG handle registry
constexpr u32   kOwnerIndexMask  = 0x003FFFFFU;            // 0x01000000 to 0x013FFFFF
constexpr usize kDefineAttempts  = 8U;
constexpr usize kBlobMaxBytes    = sizeof(TPM2B_PRIVATE) + sizeof(TPM2B_PUBLIC);
constexpr TPMA_NV kIndexAttributes =
    TPMA_NV_AUTHREAD | TPMA_NV_AUTHWRITE | TPMA_NV_NO_DA | TPMA_NV_WRITEDEFINE; // TPM2_NT_ORDINARY: type bits 0
constexpr TPMA_NV kIndexLocked = kIndexAttributes | TPMA_NV_WRITTEN | TPMA_NV_WRITELOCKED;
constexpr char  kMasterLabel[] = "cerid tpm master 1";
constexpr char  kBlobLabel[]   = "cerid tpm signer index 1";
constexpr char  kKeyLabel[]    = "cerid tpm signer key 1";

struct Reference
{
    u32 index = 0U;
    u8  secret[kSecretBytes] = {};
};

// A reference this protector or device wrote, for a store's identifier: its format byte, an index of the owner's range
// and the secret.
[[nodiscard]] bool parse(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> bytes, Reference& reference) noexcept
{
    if (store_id.size() != kKeyStoreIdBytes || bytes.size() != kReferenceBytes || bytes[0] != kReferenceFormat)
    {
        return false;
    }
    reference.index = (u32{bytes[1]} << 24U) | (u32{bytes[2]} << 16U) | (u32{bytes[3]} << 8U) | u32{bytes[4]};
    std::memcpy(reference.secret, bytes.data() + 5U, kSecretBytes);
    return (reference.index & ~kOwnerIndexMask) == kOwnerIndexFirst;
}

void write_reference(const Reference& reference, cont::Array<u8>& out) noexcept
{
    out.resize(kReferenceBytes);
    out[0] = kReferenceFormat;
    out[1] = static_cast<u8>(reference.index >> 24U);
    out[2] = static_cast<u8>(reference.index >> 16U);
    out[3] = static_cast<u8>(reference.index >> 8U);
    out[4] = static_cast<u8>(reference.index);
    std::memcpy(out.data() + 5U, reference.secret, kSecretBytes);
}

// HMAC-SHA256 under the reference's secret of the label and the store's identifier: an authorization that only this
// reference in this store derives.
template <usize N>
[[nodiscard]] CryptoError derive_auth(const char (&label)[N], cont::ConstSpan<u8> store_id, const Reference& reference,
                                      TPM2B_AUTH& auth) noexcept
{
    u8 message[N - 1U + kKeyStoreIdBytes];
    std::memcpy(message, label, N - 1U);
    std::memcpy(message + N - 1U, store_id.data(), kKeyStoreIdBytes);
    auth      = TPM2B_AUTH{};
    auth.size = static_cast<UINT16>(kAuthBytes);
    return kdf_mac(AlgorithmId::HkdfSha256, cont::ConstSpan<u8>(reference.secret), cont::ConstSpan<u8>(message),
                   cont::Span<u8>(auth.buffer, kAuthBytes));
}

// An ESYS allocation, released with Esys_Free.
template <typename T> class Owned
{
public:
    Owned() noexcept = default;
    ~Owned() noexcept
    {
        if (m_value != nullptr)
        {
            tss2().free(m_value);
        }
    }
    Owned(const Owned&)            = delete;
    Owned& operator=(const Owned&) = delete;

    [[nodiscard]] T** put() noexcept { return &m_value; }
    [[nodiscard]] T*  get() const noexcept { return m_value; }
    [[nodiscard]] T*  operator->() const noexcept { return m_value; }

private:
    T* m_value = nullptr;
};

// The TCG's ECC P-256 storage root template (TPM 2.0 Provisioning Guidance, the SRK): the same owner seed makes the
// same key, so a transient one serves every call.
[[nodiscard]] TPM2B_PUBLIC storage_root_template() noexcept
{
    TPM2B_PUBLIC in{};
    TPMT_PUBLIC& area                       = in.publicArea;
    area.type                               = TPM2_ALG_ECC;
    area.nameAlg                            = TPM2_ALG_SHA256;
    area.objectAttributes                   = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT |
                            TPMA_OBJECT_SENSITIVEDATAORIGIN | TPMA_OBJECT_USERWITHAUTH | TPMA_OBJECT_NODA |
                            TPMA_OBJECT_RESTRICTED | TPMA_OBJECT_DECRYPT;
    area.parameters.eccDetail.symmetric.algorithm   = TPM2_ALG_AES;
    area.parameters.eccDetail.symmetric.keyBits.aes = 128U;
    area.parameters.eccDetail.symmetric.mode.aes    = TPM2_ALG_CFB;
    area.parameters.eccDetail.scheme.scheme         = TPM2_ALG_NULL;
    area.parameters.eccDetail.curveID               = TPM2_ECC_NIST_P256;
    area.parameters.eccDetail.kdf.scheme            = TPM2_ALG_NULL;
    area.unique.ecc.x.size                          = 32U;
    area.unique.ecc.y.size                          = 32U;
    return in;
}

[[nodiscard]] TPM2_ECC_CURVE curve_of(AlgorithmId algorithm) noexcept
{
    switch (algorithm)
    {
    case AlgorithmId::EcdsaSecp256r1Sha256:
        return TPM2_ECC_NIST_P256;
    case AlgorithmId::EcdsaSecp384r1Sha384:
        return TPM2_ECC_NIST_P384;
    default:
        return TPM2_ECC_NONE;
    }
}

[[nodiscard]] usize field_of(AlgorithmId algorithm) noexcept
{
    return algorithm == AlgorithmId::EcdsaSecp384r1Sha384 ? 48U : 32U;
}

// A big-endian TPM integer of at most `out.size()` bytes, left-padded into `out`.
[[nodiscard]] bool pad_into(const TPM2B_ECC_PARAMETER& value, cont::Span<u8> out) noexcept
{
    if (value.size > out.size())
    {
        return false;
    }
    const usize gap = out.size() - value.size;
    std::memset(out.data(), 0, gap);
    std::memcpy(out.data() + gap, value.buffer, value.size);
    return true;
}

// ---- One conversation with the TPM ----

class Session
{
public:
    Session() noexcept = default;
    ~Session() noexcept { close(); }
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // Connects, refuses a TPM whose owner hierarchy has an authorization set (its storage root and indices need the
    // owner), reads its NV buffer limit, creates the storage root and starts the salted session. A failure is
    // Unavailable: the TPM cannot be used here.
    [[nodiscard]] CryptoError open(const TpmConnection& tpm) noexcept
    {
        const Tss2& t = tss2();
        if (!t.loaded || !tpm.usable() || t.tcti_initialize(tpm.tcti(), &m_tcti) != TSS2_RC_SUCCESS ||
            t.initialize(&m_esys, m_tcti, nullptr) != TSS2_RC_SUCCESS)
        {
            return CryptoError::Unavailable;
        }
        u32 owner_auth = 0U;
        u32 buffer_max = 0U;
        if (!property(TPM2_PT_PERMANENT, owner_auth) || (owner_auth & TPMA_PERMANENT_OWNERAUTHSET) != 0U ||
            !property(TPM2_PT_NV_BUFFER_MAX, buffer_max) || buffer_max == 0U)
        {
            return CryptoError::Unavailable;
        }
        m_buffer_max = buffer_max < sizeof(TPM2B_MAX_NV_BUFFER::buffer) ? buffer_max
                                                                        : sizeof(TPM2B_MAX_NV_BUFFER::buffer);
        const TPM2B_SENSITIVE_CREATE sensitive{};
        const TPM2B_PUBLIC           templ = storage_root_template();
        const TPM2B_DATA             outside{};
        const TPML_PCR_SELECTION     pcrs{};
        if (t.create_primary(m_esys, ESYS_TR_RH_OWNER, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE, &sensitive, &templ,
                             &outside, &pcrs, &m_root, nullptr, nullptr, nullptr, nullptr) != TSS2_RC_SUCCESS)
        {
            m_root = ESYS_TR_NONE;
            return CryptoError::Unavailable;
        }
        TPMT_SYM_DEF aes{};
        aes.algorithm   = TPM2_ALG_AES;
        aes.keyBits.aes = 128U;
        aes.mode.aes    = TPM2_ALG_CFB;
        if (t.start_auth_session(m_esys, m_root, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, nullptr,
                                 TPM2_SE_HMAC, &aes, TPM2_ALG_SHA256, &m_session) != TSS2_RC_SUCCESS)
        {
            m_session = ESYS_TR_NONE;
            return CryptoError::Unavailable;
        }
        return CryptoError::None;
    }

    [[nodiscard]] ESYS_CONTEXT* esys() const noexcept { return m_esys; }
    [[nodiscard]] ESYS_TR       root() const noexcept { return m_root; }
    [[nodiscard]] usize         buffer_max() const noexcept { return m_buffer_max; }

    // The salted session for the next command: it encrypts the command's first parameter (`decrypt`) or the
    // response's (`encrypt`), which the TPM allows only where that parameter is a sized buffer.
    [[nodiscard]] ESYS_TR session(bool decrypt, bool encrypt) noexcept
    {
        const TPMA_SESSION flags = static_cast<TPMA_SESSION>(TPMA_SESSION_CONTINUESESSION |
                                                             (decrypt ? TPMA_SESSION_DECRYPT : 0U) |
                                                             (encrypt ? TPMA_SESSION_ENCRYPT : 0U));
        (void)tss2().set_session_attributes(m_esys, m_session, flags, 0xFFU);
        return m_session;
    }

    // Whether the TPM implements `curve`.
    [[nodiscard]] bool implements(TPM2_ECC_CURVE curve) noexcept
    {
        Owned<TPMS_CAPABILITY_DATA> data;
        TPMI_YES_NO                 more = TPM2_NO;
        if (tss2().get_capability(m_esys, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, TPM2_CAP_ECC_CURVES, 0U,
                                  TPM2_MAX_ECC_CURVES, &more, data.put()) != TSS2_RC_SUCCESS)
        {
            return false;
        }
        for (u32 i = 0U; i < data->data.eccCurves.count; ++i)
        {
            if (data->data.eccCurves.eccCurves[i] == curve)
            {
                return true;
            }
        }
        return false;
    }

private:
    [[nodiscard]] bool property(TPM2_PT which, u32& value) noexcept
    {
        Owned<TPMS_CAPABILITY_DATA> data;
        TPMI_YES_NO                 more = TPM2_NO;
        if (tss2().get_capability(m_esys, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, TPM2_CAP_TPM_PROPERTIES, which, 1U,
                                  &more, data.put()) != TSS2_RC_SUCCESS ||
            data.get() == nullptr || data->data.tpmProperties.count != 1U ||
            data->data.tpmProperties.tpmProperty[0].property != which)
        {
            return false;
        }
        value = data->data.tpmProperties.tpmProperty[0].value;
        return true;
    }

    void close() noexcept
    {
        const Tss2& t = tss2();
        if (m_session != ESYS_TR_NONE)
        {
            (void)t.flush_context(m_esys, m_session);
        }
        if (m_root != ESYS_TR_NONE)
        {
            (void)t.flush_context(m_esys, m_root);
        }
        if (m_esys != nullptr)
        {
            t.finalize(&m_esys);
        }
        if (m_tcti != nullptr)
        {
            t.tcti_finalize(&m_tcti);
        }
    }

    TSS2_TCTI_CONTEXT* m_tcti       = nullptr;
    ESYS_CONTEXT*      m_esys       = nullptr;
    ESYS_TR            m_root       = ESYS_TR_NONE;
    ESYS_TR            m_session    = ESYS_TR_NONE;
    usize              m_buffer_max = 0U;
};

// An ESYS handle for a TPM index or object, closed (not flushed) when it goes.
class Handle
{
public:
    explicit Handle(ESYS_CONTEXT* esys) noexcept : m_esys(esys) {}
    ~Handle() noexcept
    {
        if (m_handle != ESYS_TR_NONE)
        {
            (void)tss2().close(m_esys, &m_handle);
        }
    }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] ESYS_TR* put() noexcept { return &m_handle; }
    [[nodiscard]] ESYS_TR  get() const noexcept { return m_handle; }

private:
    ESYS_CONTEXT* m_esys;
    ESYS_TR       m_handle = ESYS_TR_NONE;
};

// A loaded key, flushed from the TPM when it goes.
class Loaded
{
public:
    explicit Loaded(ESYS_CONTEXT* esys) noexcept : m_esys(esys) {}
    ~Loaded() noexcept
    {
        if (m_handle != ESYS_TR_NONE)
        {
            (void)tss2().flush_context(m_esys, m_handle);
        }
    }
    Loaded(const Loaded&)            = delete;
    Loaded& operator=(const Loaded&) = delete;

    [[nodiscard]] ESYS_TR* put() noexcept { return &m_handle; }
    [[nodiscard]] ESYS_TR  get() const noexcept { return m_handle; }

private:
    ESYS_CONTEXT* m_esys;
    ESYS_TR       m_handle = ESYS_TR_NONE;
};

// The index `reference` names, if it is an index this code defined (its attributes and `size`, written and locked)
// and `auth` authorizes it: None, Denied when there is none or it is not ours, a failure otherwise.
[[nodiscard]] CryptoError open_index(Session& session, const Reference& reference, const TPM2B_AUTH& auth, usize& size,
                                     Handle& index) noexcept
{
    const Tss2&   t  = tss2();
    const TSS2_RC rc = t.from_tpm_public(session.esys(), reference.index, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                                         index.put());
    if (rc != TSS2_RC_SUCCESS)
    {
        *index.put() = ESYS_TR_NONE;
        return is(rc, TPM2_RC_HANDLE) ? CryptoError::Denied : failure_of(rc);
    }
    Owned<TPM2B_NV_PUBLIC> nv_public;
    Owned<TPM2B_NAME>      name;
    const TSS2_RC read = t.nv_read_public(session.esys(), index.get(), ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                                          nv_public.put(), name.put());
    if (read != TSS2_RC_SUCCESS)
    {
        return failure_of(read);
    }
    const TPMS_NV_PUBLIC& area = nv_public->nvPublic;
    if (area.nameAlg != TPM2_ALG_SHA256 || area.attributes != kIndexLocked || area.authPolicy.size != 0U ||
        area.dataSize == 0U)
    {
        return CryptoError::Denied; // another owner's index at the handle
    }
    size = area.dataSize;
    return t.set_auth(session.esys(), index.get(), &auth) == TSS2_RC_SUCCESS ? CryptoError::None
                                                                             : CryptoError::Unavailable;
}

// Reads all of an open index (`out` its size), over the encrypting session in chunks the TPM takes; Denied when the
// authorization fails.
[[nodiscard]] CryptoError read_index(Session& session, const Handle& index, cont::Span<u8> out) noexcept
{
    for (usize at = 0U; at < out.size();)
    {
        const usize                  chunk = out.size() - at < session.buffer_max() ? out.size() - at
                                                                                    : session.buffer_max();
        Owned<TPM2B_MAX_NV_BUFFER>   data;
        const TSS2_RC rc = tss2().nv_read(session.esys(), index.get(), index.get(), session.session(false, true),
                                          ESYS_TR_NONE, ESYS_TR_NONE, static_cast<UINT16>(chunk),
                                          static_cast<UINT16>(at), data.put());
        if (rc != TSS2_RC_SUCCESS)
        {
            return is(rc, TPM2_RC_AUTH_FAIL) || is(rc, TPM2_RC_BAD_AUTH) ? CryptoError::Denied : failure_of(rc);
        }
        if (data->size != chunk)
        {
            return CryptoError::ProviderFailure;
        }
        std::memcpy(out.data() + at, data->buffer, chunk);
        secure_zero(cont::Span<u8>(data->buffer, chunk));
        at += chunk;
    }
    return CryptoError::None;
}

// Deletes an index this code defined; the owner authorizes the deletion.
[[nodiscard]] CryptoError undefine(Session& session, Handle& index) noexcept
{
    const TSS2_RC rc = tss2().nv_undefine_space(session.esys(), ESYS_TR_RH_OWNER, index.get(),
                                                session.session(false, false), ESYS_TR_NONE, ESYS_TR_NONE);
    if (rc != TSS2_RC_SUCCESS)
    {
        return failure_of(rc);
    }
    *index.put() = ESYS_TR_NONE; // ESYS released the handle with the index
    return CryptoError::None;
}

// A new index holding `contents` at a random handle of the owner's range (into `index_handle`), authorized by `auth`,
// written once over the session and locked; a partly made index is deleted again.
[[nodiscard]] CryptoError define_index(Session& session, cont::ConstSpan<u8> contents, const TPM2B_AUTH& auth,
                                       u32& index_handle) noexcept
{
    const Tss2& t = tss2();
    Handle      index(session.esys());
    for (usize attempt = 0U;; ++attempt)
    {
        u8                handle[4];
        const CryptoError drawn = random_bytes(cont::Span<u8>(handle));
        if (drawn != CryptoError::None)
        {
            return drawn;
        }
        index_handle = kOwnerIndexFirst | (((u32{handle[0]} << 24U) | (u32{handle[1]} << 16U) |
                                            (u32{handle[2]} << 8U) | u32{handle[3]}) &
                                           kOwnerIndexMask);
        TPM2B_NV_PUBLIC info{};
        info.nvPublic.nvIndex    = index_handle;
        info.nvPublic.nameAlg    = TPM2_ALG_SHA256;
        info.nvPublic.attributes = kIndexAttributes;
        info.nvPublic.dataSize   = static_cast<UINT16>(contents.size());
        const TSS2_RC rc = t.nv_define_space(session.esys(), ESYS_TR_RH_OWNER, session.session(true, false),
                                             ESYS_TR_NONE, ESYS_TR_NONE, &auth, &info, index.put());
        if (rc == TSS2_RC_SUCCESS)
        {
            break;
        }
        *index.put() = ESYS_TR_NONE;
        if (!is(rc, TPM2_RC_NV_DEFINED) || attempt + 1U == kDefineAttempts)
        {
            return is(rc, TPM2_RC_NV_SPACE) ? CryptoError::StorageFailure : failure_of(rc);
        }
    }
    TSS2_RC rc = TSS2_RC_SUCCESS;
    for (usize at = 0U; rc == TSS2_RC_SUCCESS && at < contents.size();)
    {
        const usize         chunk = contents.size() - at < session.buffer_max() ? contents.size() - at
                                                                                : session.buffer_max();
        TPM2B_MAX_NV_BUFFER data{};
        data.size = static_cast<UINT16>(chunk);
        std::memcpy(data.buffer, contents.data() + at, chunk);
        rc = t.nv_write(session.esys(), index.get(), index.get(), session.session(true, false), ESYS_TR_NONE,
                        ESYS_TR_NONE, &data, static_cast<UINT16>(at));
        secure_zero(cont::Span<u8>(data.buffer, chunk));
        at += chunk;
    }
    if (rc == TSS2_RC_SUCCESS)
    {
        rc = t.nv_write_lock(session.esys(), index.get(), index.get(), session.session(false, false), ESYS_TR_NONE,
                             ESYS_TR_NONE);
    }
    if (rc != TSS2_RC_SUCCESS)
    {
        (void)undefine(session, index);
        return failure_of(rc);
    }
    return CryptoError::None;
}

[[nodiscard]] CryptoError master_auth(cont::ConstSpan<u8> store_id, const Reference& reference,
                                      TPM2B_AUTH& auth) noexcept
{
    return derive_auth(kMasterLabel, store_id, reference, auth);
}

[[nodiscard]] CryptoError blob_auth(cont::ConstSpan<u8> store_id, const Reference& reference, TPM2B_AUTH& auth) noexcept
{
    return derive_auth(kBlobLabel, store_id, reference, auth);
}

// Deletes the index `reference` names once `auth` is proven by reading it; None when there is none or it is not this
// reference's (forgotten already, another store's).
[[nodiscard]] CryptoError forget_index(const TpmConnection& tpm, const Reference& reference,
                                       const TPM2B_AUTH& auth) noexcept
{
    const std::lock_guard<std::mutex> lock(tpm_lock());
    Session                           session;
    CryptoError                       result = session.open(tpm);
    Handle                            index(session.esys());
    usize  size = 0U;
    if (result == CryptoError::None)
    {
        result = open_index(session, reference, auth, size, index);
    }
    u8 probe[1];
    if (result == CryptoError::None)
    {
        result = read_index(session, index, cont::Span<u8>(probe));
    }
    secure_zero(cont::Span<u8>(probe));
    if (result == CryptoError::Denied)
    {
        return CryptoError::None;
    }
    return result == CryptoError::None ? undefine(session, index) : result;
}

// The device key's template: sign-only ECDSA on `curve` with the curve's hash, authorized by its value alone.
[[nodiscard]] TPM2B_PUBLIC signer_template(AlgorithmId algorithm) noexcept
{
    TPM2B_PUBLIC in{};
    TPMT_PUBLIC& area     = in.publicArea;
    area.type             = TPM2_ALG_ECC;
    area.nameAlg          = TPM2_ALG_SHA256;
    area.objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT | TPMA_OBJECT_SENSITIVEDATAORIGIN |
                            TPMA_OBJECT_USERWITHAUTH | TPMA_OBJECT_NODA | TPMA_OBJECT_SIGN_ENCRYPT;
    area.parameters.eccDetail.symmetric.algorithm       = TPM2_ALG_NULL;
    area.parameters.eccDetail.scheme.scheme             = TPM2_ALG_ECDSA;
    area.parameters.eccDetail.scheme.details.ecdsa.hashAlg =
        algorithm == AlgorithmId::EcdsaSecp384r1Sha384 ? TPM2_ALG_SHA384 : TPM2_ALG_SHA256;
    area.parameters.eccDetail.curveID    = curve_of(algorithm);
    area.parameters.eccDetail.kdf.scheme = TPM2_ALG_NULL;
    return in;
}

// Whether a public area read back is the template's key: its type, attributes, scheme and curve.
[[nodiscard]] bool matches(const TPMT_PUBLIC& area, const TPMT_PUBLIC& expected) noexcept
{
    return area.type == expected.type && area.nameAlg == expected.nameAlg &&
           area.objectAttributes == expected.objectAttributes && area.authPolicy.size == 0U &&
           area.parameters.eccDetail.scheme.scheme == expected.parameters.eccDetail.scheme.scheme &&
           area.parameters.eccDetail.scheme.details.ecdsa.hashAlg ==
               expected.parameters.eccDetail.scheme.details.ecdsa.hashAlg &&
           area.parameters.eccDetail.curveID == expected.parameters.eccDetail.curveID;
}
} // namespace

bool TpmKeyProtector::available(const TpmConnection& tpm) noexcept
{
    const std::lock_guard<std::mutex> lock(tpm_lock());
    Session                           session;
    return session.open(tpm) == CryptoError::None;
}

cont::StringView TpmKeyProtector::name() const noexcept
{
    return "tpm-tss2";
}

bool TpmKeyProtector::forgets() const noexcept
{
    return true; // the master key lives in the index, which forget deletes
}

CryptoError TpmKeyProtector::protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                     cont::Array<u8>& reference) noexcept
{
    reference.clear();
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    Reference   made;
    TPM2B_AUTH  auth{};
    CryptoError result = random_bytes(cont::Span<u8>(made.secret));
    if (result == CryptoError::None)
    {
        result = master_auth(store_id, made, auth);
    }
    if (result == CryptoError::None)
    {
        const std::lock_guard<std::mutex> lock(tpm_lock());
        Session                           session;
        result = session.open(m_tpm);
        if (result == CryptoError::None)
        {
            result = define_index(session, master, auth, made.index);
        }
    }
    if (result == CryptoError::None)
    {
        write_reference(made, reference);
    }
    secure_zero(cont::Span<u8>(made.secret));
    secure_zero(cont::Span<u8>(auth.buffer, sizeof(auth.buffer)));
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
    Reference held;
    if (!parse(store_id, reference, held))
    {
        return CryptoError::Denied; // not a reference this protector wrote
    }
    TPM2B_AUTH                        auth{};
    CryptoError                       result = master_auth(store_id, held, auth);
    const std::lock_guard<std::mutex> lock(tpm_lock());
    Session                           session;
    if (result == CryptoError::None)
    {
        result = session.open(m_tpm);
    }
    Handle index(session.esys());
    usize  size = 0U;
    if (result == CryptoError::None)
    {
        result = open_index(session, held, auth, size, index);
    }
    if (result == CryptoError::None && size != kKeyStoreMasterBytes)
    {
        result = CryptoError::Denied; // a device's index, not a master key's
    }
    if (result == CryptoError::None)
    {
        result = read_index(session, index, master);
    }
    if (result != CryptoError::None)
    {
        secure_zero(master);
    }
    secure_zero(cont::Span<u8>(held.secret));
    secure_zero(cont::Span<u8>(auth.buffer, sizeof(auth.buffer)));
    return result;
}

CryptoError TpmKeyProtector::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    Reference held;
    if (!parse(store_id, reference, held))
    {
        return CryptoError::Denied;
    }
    TPM2B_AUTH  auth{};
    CryptoError result = master_auth(store_id, held, auth);
    if (result == CryptoError::None)
    {
        result = forget_index(m_tpm, held, auth);
    }
    secure_zero(cont::Span<u8>(held.secret));
    secure_zero(cont::Span<u8>(auth.buffer, sizeof(auth.buffer)));
    return result;
}

bool TpmKeyDevice::available(const TpmConnection& tpm) noexcept
{
    return TpmKeyProtector::available(tpm);
}

cont::StringView TpmKeyDevice::name() const noexcept
{
    return "tpm-tss2";
}

CryptoError TpmKeyDevice::create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm, cont::Array<u8>& reference,
                                 cont::Span<u8> public_key) noexcept
{
    reference.clear();
    if (curve_of(algorithm) == TPM2_ECC_NONE)
    {
        return CryptoError::Unsupported;
    }
    const usize field = field_of(algorithm);
    if (store_id.size() != kKeyStoreIdBytes || public_key.size() != 1U + 2U * field)
    {
        return CryptoError::BadLength;
    }
    const Tss2&                       t = tss2();
    const std::lock_guard<std::mutex> lock(tpm_lock());
    Session                           session;
    CryptoError                       result = session.open(m_tpm);
    if (result == CryptoError::None && !session.implements(curve_of(algorithm)))
    {
        result = CryptoError::Unsupported;
    }
    // One secret authorizes the key and its index, each under its own label.
    Reference made;
    if (result == CryptoError::None)
    {
        result = random_bytes(cont::Span<u8>(made.secret));
    }
    TPM2B_SENSITIVE_CREATE sensitive{};
    if (result == CryptoError::None)
    {
        result = derive_auth(kKeyLabel, store_id, made, sensitive.sensitive.userAuth);
    }
    Owned<TPM2B_PRIVATE> wrapped;
    Owned<TPM2B_PUBLIC>  created;
    const TPM2B_PUBLIC   templ = signer_template(algorithm);
    if (result == CryptoError::None)
    {
        const TPM2B_DATA         outside{};
        const TPML_PCR_SELECTION pcrs{};
        const TSS2_RC rc = t.create(session.esys(), session.root(), session.session(true, false), ESYS_TR_NONE,
                                    ESYS_TR_NONE, &sensitive, &templ, &outside, &pcrs, wrapped.put(), created.put(),
                                    nullptr, nullptr, nullptr);
        result = rc == TSS2_RC_SUCCESS ? CryptoError::None : failure_of(rc);
    }
    secure_zero(cont::Span<u8>(sensitive.sensitive.userAuth.buffer, sizeof(sensitive.sensitive.userAuth.buffer)));
    public_key[0] = 0x04U; // an uncompressed SEC 1 point
    if (result == CryptoError::None &&
        (!matches(created->publicArea, templ.publicArea) ||
         !pad_into(created->publicArea.unique.ecc.x, public_key.subspan(1U, field)) ||
         !pad_into(created->publicArea.unique.ecc.y, public_key.subspan(1U + field, field))))
    {
        result = CryptoError::ProviderFailure;
    }
    // The blob: the TPM-wrapped private area, then the public area, as tss2 marshals them.
    u8     blob[kBlobMaxBytes];
    size_t length = 0U;
    if (result == CryptoError::None &&
        (t.private_marshal(wrapped.get(), blob, sizeof(blob), &length) != TSS2_RC_SUCCESS ||
         t.public_marshal(created.get(), blob, sizeof(blob), &length) != TSS2_RC_SUCCESS))
    {
        result = CryptoError::ProviderFailure;
    }
    if (result == CryptoError::None)
    {
        TPM2B_AUTH auth{};
        result = blob_auth(store_id, made, auth);
        if (result == CryptoError::None)
        {
            result = define_index(session, cont::ConstSpan<u8>(blob, length), auth, made.index);
        }
        secure_zero(cont::Span<u8>(auth.buffer, sizeof(auth.buffer)));
    }
    if (result == CryptoError::None)
    {
        write_reference(made, reference);
    }
    else
    {
        secure_zero(public_key);
    }
    secure_zero(cont::Span<u8>(made.secret));
    return result;
}

CryptoError TpmKeyDevice::sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                      AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                      cont::Span<u8> signature) const noexcept
{
    secure_zero(signature);
    if (curve_of(algorithm) == TPM2_ECC_NONE)
    {
        return CryptoError::Unsupported;
    }
    const usize field = field_of(algorithm);
    if (store_id.size() != kKeyStoreIdBytes || digest.size() != field || signature.size() != 2U * field)
    {
        return CryptoError::BadLength;
    }
    Reference held;
    if (!parse(store_id, reference, held))
    {
        return CryptoError::Denied; // not a reference this device wrote
    }
    const Tss2&                       t = tss2();
    const std::lock_guard<std::mutex> lock(tpm_lock());
    Session                           session;
    CryptoError                       result = session.open(m_tpm);
    TPM2B_AUTH                        auth{};
    if (result == CryptoError::None)
    {
        result = blob_auth(store_id, held, auth);
    }
    Handle index(session.esys());
    usize  size = 0U;
    if (result == CryptoError::None)
    {
        result = open_index(session, held, auth, size, index);
    }
    u8 blob[kBlobMaxBytes];
    if (result == CryptoError::None && size > sizeof(blob))
    {
        result = CryptoError::Denied; // not a device key's index
    }
    if (result == CryptoError::None)
    {
        result = read_index(session, index, cont::Span<u8>(blob, size));
    }
    TPM2B_PRIVATE wrapped{};
    TPM2B_PUBLIC  loaded_public{};
    size_t        offset = 0U;
    if (result == CryptoError::None &&
        (t.private_unmarshal(blob, size, &offset, &wrapped) != TSS2_RC_SUCCESS ||
         t.public_unmarshal(blob, size, &offset, &loaded_public) != TSS2_RC_SUCCESS || offset != size ||
         !matches(loaded_public.publicArea, signer_template(algorithm).publicArea)))
    {
        result = CryptoError::Denied; // not a key of this algorithm
    }
    Loaded key(session.esys());
    if (result == CryptoError::None)
    {
        const TSS2_RC rc = t.load(session.esys(), session.root(), session.session(false, false), ESYS_TR_NONE,
                                  ESYS_TR_NONE, &wrapped, &loaded_public, key.put());
        result           = rc == TSS2_RC_SUCCESS ? CryptoError::None : failure_of(rc);
    }
    if (result == CryptoError::None)
    {
        result = derive_auth(kKeyLabel, store_id, held, auth);
    }
    if (result == CryptoError::None && t.set_auth(session.esys(), key.get(), &auth) != TSS2_RC_SUCCESS)
    {
        result = CryptoError::Unavailable;
    }
    secure_zero(cont::Span<u8>(auth.buffer, sizeof(auth.buffer)));
    secure_zero(cont::Span<u8>(held.secret));
    Owned<TPMT_SIGNATURE> signed_digest;
    if (result == CryptoError::None)
    {
        TPM2B_DIGEST hash{};
        hash.size = static_cast<UINT16>(field);
        std::memcpy(hash.buffer, digest.data(), field);
        TPMT_SIG_SCHEME scheme{};
        scheme.scheme                    = TPM2_ALG_ECDSA;
        scheme.details.ecdsa.hashAlg     = loaded_public.publicArea.parameters.eccDetail.scheme.details.ecdsa.hashAlg;
        TPMT_TK_HASHCHECK validation{};
        validation.tag       = TPM2_ST_HASHCHECK;
        validation.hierarchy = TPM2_RH_NULL;
        const TSS2_RC rc = t.sign(session.esys(), key.get(), session.session(false, false), ESYS_TR_NONE, ESYS_TR_NONE,
                                  &hash, &scheme, &validation, signed_digest.put());
        result = rc == TSS2_RC_SUCCESS ? CryptoError::None : failure_of(rc);
    }
    if (result == CryptoError::None &&
        (signed_digest->sigAlg != TPM2_ALG_ECDSA ||
         !pad_into(signed_digest->signature.ecdsa.signatureR, signature.subspan(0U, field)) ||
         !pad_into(signed_digest->signature.ecdsa.signatureS, signature.subspan(field, field))))
    {
        result = CryptoError::ProviderFailure;
    }
    if (result != CryptoError::None)
    {
        secure_zero(signature);
    }
    return result;
}

CryptoError TpmKeyDevice::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    Reference held;
    if (!parse(store_id, reference, held))
    {
        return CryptoError::Denied;
    }
    TPM2B_AUTH  auth{};
    CryptoError result = blob_auth(store_id, held, auth);
    if (result == CryptoError::None)
    {
        result = forget_index(m_tpm, held, auth);
    }
    secure_zero(cont::Span<u8>(held.secret));
    secure_zero(cont::Span<u8>(auth.buffer, sizeof(auth.buffer)));
    return result;
}
} // namespace crd::crypto
#endif
