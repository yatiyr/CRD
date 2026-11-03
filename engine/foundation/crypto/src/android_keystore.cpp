// crd-crypto -- the Android Keystore protector and key device (NET.2; contract:
// docs/design/network-foundation.md#key-store, decisions K2, K8 and K15): PlatformKeyProtector and PlatformKeyDevice
// on Android.
//
// The Keystore is a Java API, so every call goes through the application's own Java VM: JNI_GetCreatedJavaVMs from
// libnativehelper (a public NDK library from API 31, loaded at run time, so a process without it or without a VM
// answers Unavailable), the calling thread attached for the call when it is not and detached again after, every local
// reference released with the call's frame and every Java exception cleared where it is raised. Only framework classes
// are named, which the system class loader of an attached thread finds. A key is generated in StrongBox where the
// device has one (P-256 and AES only: StrongBox holds no P-384 key) and in its TEE otherwise. A master key crosses
// into a Java byte array for the length of one call and is overwritten there before the call returns.
#include <crd/core/platform.hpp>

#if CRD_OS_ANDROID
#include "byte_spans.hpp"
#include "provider_internal.hpp"

#include <crd/crypto/key_store.hpp>
#include <crd/platform/dynamic_library.hpp>

#include <cstring>
#include <jni.h>
#include <new>

namespace crd::crypto
{
namespace
{
// ---- The application's Java VM ----

using CreatedVms = jint (*)(JavaVM**, jsize, jsize*);

struct NativeHelper
{
    platform::DynamicLibrary library;
    CreatedVms created_vms = nullptr;
};

NativeHelper load_native_helper() noexcept
{
    NativeHelper helper;
    helper.library = platform::DynamicLibrary::open(platform::fs::Path("libnativehelper.so"), false);
    if (helper.library.is_valid())
    {
        helper.created_vms = helper.library.resolve_as<CreatedVms>("JNI_GetCreatedJavaVMs");
    }
    return helper;
}

// Loaded once and never unloaded, as the process's VM never is.
const NativeHelper& native_helper() noexcept
{
    alignas(NativeHelper) static unsigned char storage[sizeof(NativeHelper)];
    static const NativeHelper* const kLoaded = ::new (static_cast<void*>(storage)) NativeHelper(load_native_helper());
    return *kLoaded;
}

JavaVM* java_vm() noexcept
{
    const CreatedVms created_vms = native_helper().created_vms;
    JavaVM* vm = nullptr;
    jsize count = 0;
    if (created_vms == nullptr || created_vms(&vm, 1, &count) != JNI_OK || count < 1)
    {
        return nullptr;
    }
    return vm;
}

jvalue arg(jobject value) noexcept
{
    jvalue v;
    v.l = value;
    return v;
}

jvalue arg(jint value) noexcept
{
    jvalue v;
    v.i = value;
    return v;
}

jvalue flag(bool value) noexcept
{
    jvalue v;
    v.z = value ? JNI_TRUE : JNI_FALSE;
    return v;
}

// One call's local references: a generation refused for want of StrongBox and its TEE retry (about 15 each) fit.
constexpr jint kLocalReferences = 64;

// The exceptions that mean the Keystore will not use a key (Denied): a key that no longer works or a tag that does
// not authenticate. Every other exception means the Keystore does not work (Unavailable).
constexpr const char* kDeniedExceptions[] = {
    "javax/crypto/AEADBadTagException", "java/security/UnrecoverableKeyException",
    "java/security/InvalidKeyException", // KeyPermanentlyInvalidatedException, UserNotAuthenticatedException
};

// One call's way into the VM. Every step does nothing once one has failed, so a call is written as its steps in order
// and checked once: error() is None, or the first failure.
class Java
{
public:
    Java() noexcept
    {
        m_vm = java_vm();
        if (m_vm == nullptr)
        {
            return;
        }
        void* env = nullptr;
        const jint status = m_vm->GetEnv(&env, JNI_VERSION_1_6);
        if (status == JNI_EDETACHED)
        {
            m_attached = m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK;
        }
        else if (status == JNI_OK)
        {
            m_env = static_cast<JNIEnv*>(env);
        }
        if (m_env == nullptr)
        {
            return;
        }
        if (m_env->PushLocalFrame(kLocalReferences) != JNI_OK)
        {
            m_env->ExceptionClear();
            return;
        }
        m_framed = true;
        m_error = CryptoError::None;
    }

    ~Java() noexcept
    {
        if (m_framed)
        {
            (void)m_env->PopLocalFrame(nullptr);
        }
        if (m_attached)
        {
            (void)m_vm->DetachCurrentThread();
        }
    }

    Java(const Java&) = delete;
    Java& operator=(const Java&) = delete;
    Java(Java&&) = delete;
    Java& operator=(Java&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_error == CryptoError::None; }
    [[nodiscard]] CryptoError error() const noexcept { return m_error; }

    // A step's own refusal: a key that is not there, or not of its kind.
    void deny() noexcept { fail(CryptoError::Denied); }

    // A step's own failure: an answer the Java API does not allow.
    void fail(CryptoError error = CryptoError::ProviderFailure) noexcept
    {
        if (ok())
        {
            m_error = error;
        }
    }

    // Whether the step that failed raised an instance of the class `name`.
    [[nodiscard]] bool thrown_is(const char* name) noexcept
    {
        if (m_thrown == nullptr)
        {
            return false;
        }
        const jclass type = m_env->FindClass(name);
        if (type == nullptr)
        {
            m_env->ExceptionClear();
            return false;
        }
        return m_env->IsInstanceOf(m_thrown, type) == JNI_TRUE;
    }

    // Starts again after a failure the caller answers itself (a key made without StrongBox).
    void retry() noexcept
    {
        if (m_framed)
        {
            m_error = CryptoError::None;
            m_thrown = nullptr;
        }
    }

    [[nodiscard]] jstring text(const char* value) noexcept
    {
        return ok() ? checked(m_env->NewStringUTF(value)) : nullptr;
    }

    // A String[] holding `value` alone.
    [[nodiscard]] jobjectArray texts(const char* value) noexcept
    {
        const jstring element = text(value);
        const jclass type = find("java/lang/String");
        return ok() ? checked(m_env->NewObjectArray(1, type, element)) : nullptr;
    }

    [[nodiscard]] jbyteArray bytes(cont::ConstSpan<u8> value) noexcept
    {
        const jbyteArray array = ok() ? checked(m_env->NewByteArray(static_cast<jsize>(value.size()))) : nullptr;
        if (array != nullptr)
        {
            m_env->SetByteArrayRegion(array, 0, static_cast<jsize>(value.size()),
                                      reinterpret_cast<const jbyte*>(value.data()));
        }
        return array;
    }

    // Reads `array` into the start of `out`: its length, or 0 (a failure) when it is null or longer than `out`.
    [[nodiscard]] usize read(jbyteArray array, cont::Span<u8> out) noexcept
    {
        if (ok() && array == nullptr)
        {
            fail();
        }
        const usize length = ok() ? static_cast<usize>(m_env->GetArrayLength(array)) : 0U;
        if (length > out.size())
        {
            fail();
        }
        if (!ok())
        {
            return 0U;
        }
        m_env->GetByteArrayRegion(array, 0, static_cast<jsize>(length), reinterpret_cast<jbyte*>(out.data()));
        return length;
    }

    // Reads `array`, which must be exactly as long as `out`.
    void read_exact(jbyteArray array, cont::Span<u8> out) noexcept
    {
        if (read(array, out) != out.size())
        {
            fail();
        }
    }

    // Overwrites a Java copy of a secret; it runs after a failure too.
    void wipe(jbyteArray array) noexcept
    {
        if (!m_framed || array == nullptr)
        {
            return;
        }
        constexpr jsize chunk = 64;
        const jbyte zeros[chunk] = {};
        const jsize length = m_env->GetArrayLength(array);
        for (jsize at = 0; at < length; at += chunk)
        {
            m_env->SetByteArrayRegion(array, at, length - at < chunk ? length - at : chunk, zeros);
        }
    }

    // Whether `value` is an instance of `type_name` (false for null).
    [[nodiscard]] bool is(jobject value, const char* type_name) noexcept
    {
        const jclass type = find(type_name);
        return ok() && value != nullptr && m_env->IsInstanceOf(value, type) == JNI_TRUE;
    }

    [[nodiscard]] jobject make(const char* type_name, const char* signature, const jvalue* args) noexcept
    {
        const jclass type = find(type_name);
        const jmethodID method = ok() ? checked(m_env->GetMethodID(type, "<init>", signature)) : nullptr;
        return ok() ? checked(m_env->NewObjectA(type, method, args)) : nullptr;
    }

    [[nodiscard]] jobject call_static(const char* type_name, const char* name, const char* signature,
                                      const jvalue* args) noexcept
    {
        const jclass type = find(type_name);
        const jmethodID method = ok() ? checked(m_env->GetStaticMethodID(type, name, signature)) : nullptr;
        return ok() ? checked(m_env->CallStaticObjectMethodA(type, method, args)) : nullptr;
    }

    // An instance method of `type_name` called on `target`; a null target fails the call.
    jobject call(jobject target, const char* type_name, const char* name, const char* signature,
                 const jvalue* args = nullptr) noexcept
    {
        const jmethodID method = instance_method(target, type_name, name, signature);
        return ok() ? checked(m_env->CallObjectMethodA(target, method, args)) : nullptr;
    }

    void call_void(jobject target, const char* type_name, const char* name, const char* signature,
                   const jvalue* args) noexcept
    {
        const jmethodID method = instance_method(target, type_name, name, signature);
        if (ok())
        {
            m_env->CallVoidMethodA(target, method, args);
            (void)raised();
        }
    }

private:
    [[nodiscard]] jclass find(const char* name) noexcept { return ok() ? checked(m_env->FindClass(name)) : nullptr; }

    [[nodiscard]] jmethodID instance_method(jobject target, const char* type_name, const char* name,
                                            const char* signature) noexcept
    {
        if (ok() && target == nullptr)
        {
            fail();
        }
        const jclass type = find(type_name);
        return ok() ? checked(m_env->GetMethodID(type, name, signature)) : nullptr;
    }

    // Whether the last JNI call raised: the exception is then cleared and gives the call its error.
    bool raised() noexcept
    {
        if (m_env->ExceptionCheck() == JNI_FALSE)
        {
            return false;
        }
        m_thrown = m_env->ExceptionOccurred();
        m_env->ExceptionClear();
        m_error = CryptoError::Unavailable;
        for (const char* denied : kDeniedExceptions)
        {
            if (thrown_is(denied))
            {
                m_error = CryptoError::Denied;
                break;
            }
        }
        return true;
    }

    template <typename Value> [[nodiscard]] Value checked(Value value) noexcept { return raised() ? Value{} : value; }

    JavaVM* m_vm = nullptr;
    JNIEnv* m_env = nullptr;
    jthrowable m_thrown = nullptr;
    bool m_attached = false;
    bool m_framed = false;
    CryptoError m_error = CryptoError::Unavailable;
};

// ---- The Keystore ----

constexpr char kProvider[] = "AndroidKeyStore";
constexpr char kKeyStore[] = "java/security/KeyStore";
constexpr char kCipher[] = "javax/crypto/Cipher";
constexpr char kBuilder[] = "android/security/keystore/KeyGenParameterSpec$Builder";
constexpr char kTextsStep[] = "([Ljava/lang/String;)Landroid/security/keystore/KeyGenParameterSpec$Builder;";
constexpr char kStrongBoxMissing[] = "android/security/keystore/StrongBoxUnavailableException";
constexpr char kMasterLabel[] = "cerid key store master 1"; // DPAPI's entropy label: one binding, one text
constexpr char kMasterPrefix[] = "cerid.master.";
constexpr char kP256Prefix[] = "cerid.p256.";
constexpr char kP384Prefix[] = "cerid.p384.";
constexpr jint kPurposeEncrypt = 1; // android.security.keystore.KeyProperties
constexpr jint kPurposeDecrypt = 2;
constexpr jint kPurposeSign = 4;
constexpr jint kEncryptMode = 1; // javax.crypto.Cipher
constexpr jint kDecryptMode = 2;
constexpr jint kAesKeyBits = 256;
constexpr jint kTagBits = 128;
constexpr u8 kReferenceFormat = 1U;
constexpr usize kHandleBytes = 16U;
constexpr usize kIvBytes = 12U; // the Keystore's GCM nonce, which it chooses for every encryption
constexpr usize kWrappedBytes = kKeyStoreMasterBytes + static_cast<usize>(kTagBits) / 8U;
constexpr usize kAssociatedBytes = sizeof(kMasterLabel) - 1U + kKeyStoreIdBytes;
constexpr usize kSpkiMaxBytes = 128U;               // a P-384 SubjectPublicKeyInfo is 120 bytes
constexpr usize kSignatureMaxBytes = 2U * 49U + 6U; // exactly the longest P-384 Ecdsa-Sig-Value: 104 bytes
// A reference: the format, the key's random handle and, for a master key, the Keystore's nonce and the wrapped key.
constexpr usize kProtectorReferenceBytes = 1U + kHandleBytes + kIvBytes + kWrappedBytes;
constexpr usize kDeviceReferenceBytes = 1U + kHandleBytes;
// An alias: a prefix (the master key's is the longest), the store's identifier and the handle in hexadecimal.
constexpr usize kAliasBytes = sizeof(kMasterPrefix) - 1U + 2U * kKeyStoreIdBytes + 1U + 2U * kHandleBytes + 1U;
static_assert(kProtectorReferenceBytes <= kKeyReferenceMaxBytes, "a master key's reference fits the store's field");
static_assert(kDeviceReferenceBytes <= kKeyDeviceReferenceMaxBytes, "a device key's reference fits the store's field");
static_assert(sizeof(kP256Prefix) <= sizeof(kMasterPrefix) && sizeof(kP384Prefix) <= sizeof(kMasterPrefix),
              "the master key's prefix is the longest");

// The alias of a key: the prefix, the store's identifier and the handle in hexadecimal, a dot between them.
template <usize N>
void alias_of(const char (&prefix)[N], cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle,
              char (&alias)[kAliasBytes]) noexcept
{
    std::memcpy(alias, prefix, N - 1U);
    char* out = detail::hex_into(store_id, alias + N - 1U);
    *out++ = '.';
    *detail::hex_into(handle, out) = '\0';
}

// The alias of a device key of `algorithm`, whose curve it names, so a key of the other curve is never found.
void signer_alias_of(AlgorithmId algorithm, cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> handle,
                     char (&alias)[kAliasBytes]) noexcept
{
    if (algorithm == AlgorithmId::EcdsaSecp256r1Sha256)
    {
        alias_of(kP256Prefix, store_id, handle, alias);
    }
    else
    {
        alias_of(kP384Prefix, store_id, handle, alias);
    }
}

// The handle of a well-formed reference of `bytes` bytes, or an empty span.
[[nodiscard]] cont::ConstSpan<u8> handle_of(cont::ConstSpan<u8> reference, usize bytes) noexcept
{
    if (reference.size() != bytes || reference[0] != kReferenceFormat)
    {
        return {};
    }
    return reference.subspan(1U, kHandleBytes);
}

// The master key's associated data: the label and the store's identifier, so a reference recovers in its store alone.
void associated_of(cont::ConstSpan<u8> store_id, u8 (&associated)[kAssociatedBytes]) noexcept
{
    std::memcpy(associated, kMasterLabel, sizeof(kMasterLabel) - 1U);
    std::memcpy(associated + sizeof(kMasterLabel) - 1U, store_id.data(), kKeyStoreIdBytes);
}

// The Keystore, loaded.
jobject keystore(Java& java) noexcept
{
    const jvalue type[] = {arg(java.text(kProvider))};
    const jobject store =
        java.call_static(kKeyStore, "getInstance", "(Ljava/lang/String;)Ljava/security/KeyStore;", type);
    const jvalue none[] = {arg(static_cast<jobject>(nullptr))};
    java.call_void(store, kKeyStore, "load", "(Ljava/security/KeyStore$LoadStoreParameter;)V", none);
    return store;
}

// The key `alias` names, which must be an instance of `type_name`: Denied when there is none (forgotten, never made,
// another store's) or it is another kind of key.
jobject stored_key(Java& java, const char* alias, const char* type_name) noexcept
{
    const jobject store = keystore(java);
    const jvalue args[] = {arg(java.text(alias)), arg(static_cast<jobject>(nullptr))};
    const jobject key = java.call(store, kKeyStore, "getKey", "(Ljava/lang/String;[C)Ljava/security/Key;", args);
    if (java.ok() && !java.is(key, type_name))
    {
        java.deny();
    }
    return key;
}

// Deletes the key `alias` names; the Keystore deletes a missing alias without complaint.
CryptoError delete_alias(const char* alias) noexcept
{
    Java java;
    const jobject store = keystore(java);
    const jvalue args[] = {arg(java.text(alias))};
    java.call_void(store, kKeyStore, "deleteEntry", "(Ljava/lang/String;)V", args);
    return java.error();
}

// One builder step that takes a String[] holding `value`.
void set_texts(Java& java, jobject builder, const char* method, const char* value) noexcept
{
    const jvalue args[] = {arg(java.texts(value))};
    (void)java.call(builder, kBuilder, method, kTextsStep, args);
}

// The parameters of a key under `alias` for `purposes`, which `configure` completes, in StrongBox when `strongbox`.
template <typename Configure>
jobject key_spec(Java& java, const char* alias, jint purposes, bool strongbox, Configure configure) noexcept
{
    const jvalue head[] = {arg(java.text(alias)), arg(purposes)};
    const jobject builder = java.make(kBuilder, "(Ljava/lang/String;I)V", head);
    configure(builder);
    const jvalue backing[] = {flag(strongbox)};
    (void)java.call(builder, kBuilder, "setIsStrongBoxBacked",
                    "(Z)Landroid/security/keystore/KeyGenParameterSpec$Builder;", backing);
    return java.call(builder, kBuilder, "build", "()Landroid/security/keystore/KeyGenParameterSpec;");
}

// A key made by `generate(java, strongbox)`: in StrongBox first when `prefer_strongbox`, in the TEE when the device has
// no StrongBox.
template <typename Generate> jobject in_strongbox_or_tee(Java& java, bool prefer_strongbox, Generate generate) noexcept
{
    jobject made = generate(java, prefer_strongbox);
    if (prefer_strongbox && !java.ok() && java.thrown_is(kStrongBoxMissing))
    {
        java.retry();
        made = generate(java, false);
    }
    return made;
}

// A non-exportable AES-256-GCM key under `alias` that encrypts and decrypts only, the Keystore choosing every nonce.
jobject master_key(Java& java, const char* alias, bool strongbox) noexcept
{
    const jobject spec =
        key_spec(java, alias, kPurposeEncrypt | kPurposeDecrypt, strongbox,
                 [&java](jobject builder)
                 {
                     set_texts(java, builder, "setBlockModes", "GCM");
                     set_texts(java, builder, "setEncryptionPaddings", "NoPadding");
                     const jvalue bits[] = {arg(kAesKeyBits)};
                     (void)java.call(builder, kBuilder, "setKeySize",
                                     "(I)Landroid/security/keystore/KeyGenParameterSpec$Builder;", bits);
                 });
    const jvalue names[] = {arg(java.text("AES")), arg(java.text(kProvider))};
    const jobject generator =
        java.call_static("javax/crypto/KeyGenerator", "getInstance",
                         "(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/KeyGenerator;", names);
    const jvalue parameters[] = {arg(spec)};
    java.call_void(generator, "javax/crypto/KeyGenerator", "init", "(Ljava/security/spec/AlgorithmParameterSpec;)V",
                   parameters);
    return java.call(generator, "javax/crypto/KeyGenerator", "generateKey", "()Ljavax/crypto/SecretKey;");
}

// A non-exportable ECDSA key pair of `curve` under `alias` that signs digests only (DIGEST_NONE: the store hashes).
jobject signing_key(Java& java, const char* alias, const char* curve, bool strongbox) noexcept
{
    const jobject spec = key_spec(
        java, alias, kPurposeSign, strongbox,
        [&java, curve](jobject builder)
        {
            const jvalue name[] = {arg(java.text(curve))};
            const jvalue parameters[] = {
                arg(java.make("java/security/spec/ECGenParameterSpec", "(Ljava/lang/String;)V", name))};
            (void)java.call(
                builder, kBuilder, "setAlgorithmParameterSpec",
                "(Ljava/security/spec/AlgorithmParameterSpec;)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
                parameters);
            set_texts(java, builder, "setDigests", "NONE");
        });
    const jvalue names[] = {arg(java.text("EC")), arg(java.text(kProvider))};
    const jobject generator =
        java.call_static("java/security/KeyPairGenerator", "getInstance",
                         "(Ljava/lang/String;Ljava/lang/String;)Ljava/security/KeyPairGenerator;", names);
    const jvalue parameters[] = {arg(spec)};
    java.call_void(generator, "java/security/KeyPairGenerator", "initialize",
                   "(Ljava/security/spec/AlgorithmParameterSpec;)V", parameters);
    return java.call(generator, "java/security/KeyPairGenerator", "generateKeyPair", "()Ljava/security/KeyPair;");
}

// A Cipher for the Keystore's AES-GCM, initialised for `mode` with `key` (and `parameters`, when not null) and given
// the store's associated data.
jobject gcm_cipher(Java& java, jint mode, jobject key, jobject parameters, cont::ConstSpan<u8> store_id) noexcept
{
    const jvalue type[] = {arg(java.text("AES/GCM/NoPadding"))};
    const jobject cipher = java.call_static(kCipher, "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;", type);
    if (parameters == nullptr)
    {
        const jvalue init[] = {arg(mode), arg(key)};
        java.call_void(cipher, kCipher, "init", "(ILjava/security/Key;)V", init);
    }
    else
    {
        const jvalue init[] = {arg(mode), arg(key), arg(parameters)};
        java.call_void(cipher, kCipher, "init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V",
                       init);
    }
    u8 associated[kAssociatedBytes];
    associated_of(store_id, associated);
    const jvalue aad[] = {arg(java.bytes(cont::ConstSpan<u8>(associated)))};
    java.call_void(cipher, kCipher, "updateAAD", "([B)V", aad);
    return cipher;
}
} // namespace

// ---- PlatformKeyProtector: one Keystore key for each master key ----

bool PlatformKeyProtector::available() noexcept
{
    Java java;
    (void)keystore(java);
    return java.ok();
}

cont::StringView PlatformKeyProtector::name() const noexcept
{
    return "android-keystore";
}

bool PlatformKeyProtector::forgets() const noexcept
{
    return true; // deleting the alias destroys the only key that unwraps the reference
}

CryptoError PlatformKeyProtector::protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                          cont::Array<u8>& reference) noexcept
{
    reference.clear();
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    u8 written[kProtectorReferenceBytes] = {kReferenceFormat};
    CryptoError result = random_bytes(cont::Span<u8>(written + 1U, kHandleBytes));
    if (result != CryptoError::None)
    {
        return result;
    }
    char alias[kAliasBytes];
    alias_of(kMasterPrefix, store_id, cont::ConstSpan<u8>(written + 1U, kHandleBytes), alias);
    {
        Java java;
        const jobject key = in_strongbox_or_tee(java, true, [&alias](Java& on, bool strongbox)
                                                { return master_key(on, alias, strongbox); });
        const jobject cipher = gcm_cipher(java, kEncryptMode, key, nullptr, store_id);
        const jbyteArray plain = java.bytes(master);
        const jvalue input[] = {arg(plain)};
        const auto wrapped = static_cast<jbyteArray>(java.call(cipher, kCipher, "doFinal", "([B)[B", input));
        java.wipe(plain);
        const auto iv = static_cast<jbyteArray>(java.call(cipher, kCipher, "getIV", "()[B"));
        u8* const out = written + 1U + kHandleBytes;
        java.read_exact(iv, cont::Span<u8>(out, kIvBytes));
        java.read_exact(wrapped, cont::Span<u8>(out + kIvBytes, kWrappedBytes));
        result = java.error();
    }
    if (result != CryptoError::None)
    {
        (void)delete_alias(alias); // a key made for a master key that was not wrapped guards nothing
        return result;
    }
    reference.resize(kProtectorReferenceBytes);
    std::memcpy(reference.data(), written, kProtectorReferenceBytes);
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
    const cont::ConstSpan<u8> handle = handle_of(reference, kProtectorReferenceBytes);
    if (handle.empty())
    {
        return CryptoError::Denied; // not a reference this protector wrote
    }
    char alias[kAliasBytes];
    alias_of(kMasterPrefix, store_id, handle, alias);
    Java java;
    const jobject key = stored_key(java, alias, "javax/crypto/SecretKey");
    const jvalue nonce[] = {arg(kTagBits), arg(java.bytes(reference.subspan(1U + kHandleBytes, kIvBytes)))};
    const jobject parameters = java.make("javax/crypto/spec/GCMParameterSpec", "(I[B)V", nonce);
    const jobject cipher = gcm_cipher(java, kDecryptMode, key, parameters, store_id);
    const jvalue input[] = {arg(java.bytes(reference.subspan(1U + kHandleBytes + kIvBytes)))};
    const auto plain = static_cast<jbyteArray>(java.call(cipher, kCipher, "doFinal", "([B)[B", input));
    java.read_exact(plain, master);
    java.wipe(plain);
    if (!java.ok())
    {
        secure_zero(master);
    }
    return java.error();
}

CryptoError PlatformKeyProtector::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    const cont::ConstSpan<u8> handle = handle_of(reference, kProtectorReferenceBytes);
    if (store_id.size() != kKeyStoreIdBytes || handle.empty())
    {
        return CryptoError::Denied;
    }
    char alias[kAliasBytes];
    alias_of(kMasterPrefix, store_id, handle, alias);
    return delete_alias(alias);
}

// ---- PlatformKeyDevice: sign-only ECDSA keys ----

bool PlatformKeyDevice::available() noexcept
{
    return PlatformKeyProtector::available();
}

cont::StringView PlatformKeyDevice::name() const noexcept
{
    return "android-keystore";
}

CryptoError PlatformKeyDevice::create(cont::ConstSpan<u8> store_id, AlgorithmId algorithm, cont::Array<u8>& reference,
                                      cont::Span<u8> public_key) noexcept
{
    reference.clear();
    secure_zero(public_key);
    const usize field = provider_internal::ecdsa_field_bytes(algorithm);
    if (field == 0U)
    {
        return CryptoError::Unsupported;
    }
    if (store_id.size() != kKeyStoreIdBytes || public_key.size() != 1U + 2U * field)
    {
        return CryptoError::BadLength;
    }
    u8 written[kDeviceReferenceBytes] = {kReferenceFormat};
    CryptoError result = random_bytes(cont::Span<u8>(written + 1U, kHandleBytes));
    if (result != CryptoError::None)
    {
        return result;
    }
    char alias[kAliasBytes];
    signer_alias_of(algorithm, store_id, cont::ConstSpan<u8>(written + 1U, kHandleBytes), alias);
    const bool p256 = algorithm == AlgorithmId::EcdsaSecp256r1Sha256;
    const char* curve = p256 ? "secp256r1" : "secp384r1";
    u8 spki[kSpkiMaxBytes];
    usize spki_bytes = 0U;
    {
        Java java;
        const jobject pair = in_strongbox_or_tee(java, p256, [&alias, curve](Java& on, bool strongbox)
                                                 { return signing_key(on, alias, curve, strongbox); });
        const jobject public_half =
            java.call(pair, "java/security/KeyPair", "getPublic", "()Ljava/security/PublicKey;");
        const auto encoded = static_cast<jbyteArray>(java.call(public_half, "java/security/Key", "getEncoded", "()[B"));
        spki_bytes = java.read(encoded, cont::Span<u8>(spki));
        result = java.error();
    }
    if (result == CryptoError::None)
    {
        result = provider_internal::ecdsa_public_of_spki(algorithm, cont::ConstSpan<u8>(spki, spki_bytes), public_key);
    }
    if (result != CryptoError::None)
    {
        secure_zero(public_key);
        (void)delete_alias(alias); // a key whose public half was not read is no store's
        return result;
    }
    reference.resize(kDeviceReferenceBytes);
    std::memcpy(reference.data(), written, kDeviceReferenceBytes);
    return CryptoError::None;
}

CryptoError PlatformKeyDevice::sign_digest(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                           AlgorithmId algorithm, cont::ConstSpan<u8> digest,
                                           cont::Span<u8> signature) const noexcept
{
    secure_zero(signature);
    const usize field = provider_internal::ecdsa_field_bytes(algorithm);
    if (field == 0U)
    {
        return CryptoError::Unsupported;
    }
    if (store_id.size() != kKeyStoreIdBytes || digest.size() != field || signature.size() != 2U * field)
    {
        return CryptoError::BadLength;
    }
    const cont::ConstSpan<u8> handle = handle_of(reference, kDeviceReferenceBytes);
    if (handle.empty())
    {
        return CryptoError::Denied; // not a reference this device wrote
    }
    char alias[kAliasBytes];
    signer_alias_of(algorithm, store_id, handle, alias);
    u8 der[kSignatureMaxBytes];
    usize der_bytes = 0U;
    CryptoError result = CryptoError::None;
    {
        Java java;
        const jobject key = stored_key(java, alias, "java/security/PrivateKey");
        const jvalue name[] = {arg(java.text("NONEwithECDSA"))};
        const jobject signer = java.call_static("java/security/Signature", "getInstance",
                                                "(Ljava/lang/String;)Ljava/security/Signature;", name);
        const jvalue init[] = {arg(key)};
        java.call_void(signer, "java/security/Signature", "initSign", "(Ljava/security/PrivateKey;)V", init);
        const jvalue input[] = {arg(java.bytes(digest))};
        java.call_void(signer, "java/security/Signature", "update", "([B)V", input);
        const auto signed_bytes = static_cast<jbyteArray>(java.call(signer, "java/security/Signature", "sign", "()[B"));
        der_bytes = java.read(signed_bytes, cont::Span<u8>(der));
        result = java.error();
    }
    return result != CryptoError::None
               ? result
               : provider_internal::ecdsa_raw_of(algorithm, cont::ConstSpan<u8>(der, der_bytes), signature);
}

CryptoError PlatformKeyDevice::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    const cont::ConstSpan<u8> handle = handle_of(reference, kDeviceReferenceBytes);
    if (store_id.size() != kKeyStoreIdBytes || handle.empty())
    {
        return CryptoError::Denied;
    }
    // The reference names no curve; its random handle is under one of the two aliases at most.
    char p256[kAliasBytes];
    char p384[kAliasBytes];
    signer_alias_of(AlgorithmId::EcdsaSecp256r1Sha256, store_id, handle, p256);
    signer_alias_of(AlgorithmId::EcdsaSecp384r1Sha384, store_id, handle, p384);
    const CryptoError result = delete_alias(p256);
    return result != CryptoError::None ? result : delete_alias(p384);
}
} // namespace crd::crypto
#endif
