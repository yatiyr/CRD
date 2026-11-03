// crd-crypto -- the freedesktop Secret Service protector (NET.2; contract: docs/design/network-foundation.md#key-store,
// decisions K2 and K10): PlatformKeyProtector on Linux.
//
// libdbus-1 is loaded at run time (libdbus-1.so.3), so no Cerid build or package links it and a system without it
// answers Unavailable. Each call opens its own private connection to the session bus and, where a secret crosses it,
// its own dh-ietf1024-sha256-aes128-cbc-pkcs7 transport session, and closes both before it returns: a prompt belongs to
// the connection that asked for it, so one call keeps one connection throughout. Every reply's signature is checked
// before a byte of it is read; libdbus validates the wire format itself.
#include <crd/core/platform.hpp>

#if CRD_OS_LINUX && !CRD_OS_ANDROID
#include "byte_spans.hpp"

#include <crd/crypto/key_store.hpp>
#include <crd/platform/dynamic_library.hpp>

#include <chrono>
#include <cstring>
#include <new>

namespace crd::crypto
{
namespace
{
// ---- libdbus-1 (1.x, its ABI stable since 1.0) ----

// The two structures a caller allocates, laid out as libdbus 1.14.10's dbus-errors.h and dbus-message.h declare them.
struct DBusError
{
    const char* name;
    const char* message;
    unsigned int flags; // dummy1 to dummy5, one bit each
    void* padding;
};

struct DBusMessageIter
{
    void* dummy1;
    void* dummy2;
    unsigned int dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    void* pad2;
    void* pad3;
};

struct DBusConnection;
struct DBusMessage;
using DBusBool = unsigned int;

constexpr int kTypeBoolean = 'b';
constexpr int kTypeByte = 'y';
constexpr int kTypeString = 's';
constexpr int kTypeObjectPath = 'o';
constexpr int kTypeArray = 'a';
constexpr int kTypeVariant = 'v';
constexpr int kTypeStruct = 'r';
constexpr int kTypeDictEntry = 'e';
constexpr int kBusSession = 0;
constexpr int kCallMilliseconds = 25000; // libdbus's own default reply timeout, stated
constexpr char kLibrary[] = "libdbus-1.so.3";

// The calls, resolved once; a library that lacks one (older than 1.11.16) is not used.
struct Dbus
{
    platform::DynamicLibrary library;
    DBusBool (*threads_init_default)() = nullptr;
    void (*error_init)(DBusError*) = nullptr;
    void (*error_free)(DBusError*) = nullptr;
    DBusConnection* (*bus_get_private)(int, DBusError*) = nullptr;
    void (*bus_add_match)(DBusConnection*, const char*, DBusError*) = nullptr;
    void (*connection_set_exit_on_disconnect)(DBusConnection*, DBusBool) = nullptr;
    void (*connection_close)(DBusConnection*) = nullptr;
    void (*connection_unref)(DBusConnection*) = nullptr;
    DBusBool (*connection_read_write)(DBusConnection*, int) = nullptr;
    DBusMessage* (*connection_pop_message)(DBusConnection*) = nullptr;
    DBusMessage* (*connection_send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*) = nullptr;
    DBusMessage* (*message_new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
    DBusMessage* (*message_copy)(const DBusMessage*) = nullptr;
    void (*message_unref)(DBusMessage*) = nullptr;
    const char* (*message_get_signature)(DBusMessage*) = nullptr;
    const char* (*message_get_path)(DBusMessage*) = nullptr;
    DBusBool (*message_is_signal)(DBusMessage*, const char*, const char*) = nullptr;
    void (*iter_init_append)(DBusMessage*, DBusMessageIter*) = nullptr;
    DBusBool (*iter_append_basic)(DBusMessageIter*, int, const void*) = nullptr;
    DBusBool (*iter_append_fixed_array)(DBusMessageIter*, int, const void*, int) = nullptr;
    DBusBool (*iter_open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*) = nullptr;
    DBusBool (*iter_close_container)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    void (*iter_abandon_container_if_open)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    DBusBool (*iter_init)(DBusMessage*, DBusMessageIter*) = nullptr;
    int (*iter_get_arg_type)(DBusMessageIter*) = nullptr;
    int (*iter_get_element_type)(DBusMessageIter*) = nullptr;
    void (*iter_recurse)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    DBusBool (*iter_next)(DBusMessageIter*) = nullptr;
    void (*iter_get_basic)(DBusMessageIter*, void*) = nullptr;
    void (*iter_get_fixed_array)(DBusMessageIter*, void*, int*) = nullptr;
    bool loaded = false;
};

template <typename Function> bool bind(const platform::DynamicLibrary& library, Function& function, const char* name)
{
    function = library.resolve_as<Function>(name);
    return function != nullptr;
}

Dbus load_dbus() noexcept
{
    Dbus d;
    d.library = platform::DynamicLibrary::open(platform::fs::Path(kLibrary), false);
    if (!d.library.is_valid())
    {
        return d;
    }
    const platform::DynamicLibrary& l = d.library;
    const bool bound =
        bind(l, d.threads_init_default, "dbus_threads_init_default") && bind(l, d.error_init, "dbus_error_init") &&
        bind(l, d.error_free, "dbus_error_free") && bind(l, d.bus_get_private, "dbus_bus_get_private") &&
        bind(l, d.bus_add_match, "dbus_bus_add_match") &&
        bind(l, d.connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect") &&
        bind(l, d.connection_close, "dbus_connection_close") && bind(l, d.connection_unref, "dbus_connection_unref") &&
        bind(l, d.connection_read_write, "dbus_connection_read_write") &&
        bind(l, d.connection_pop_message, "dbus_connection_pop_message") &&
        bind(l, d.connection_send_with_reply_and_block, "dbus_connection_send_with_reply_and_block") &&
        bind(l, d.message_new_method_call, "dbus_message_new_method_call") &&
        bind(l, d.message_copy, "dbus_message_copy") &&
        bind(l, d.message_unref, "dbus_message_unref") &&
        bind(l, d.message_get_signature, "dbus_message_get_signature") &&
        bind(l, d.message_get_path, "dbus_message_get_path") &&
        bind(l, d.message_is_signal, "dbus_message_is_signal") &&
        bind(l, d.iter_init_append, "dbus_message_iter_init_append") &&
        bind(l, d.iter_append_basic, "dbus_message_iter_append_basic") &&
        bind(l, d.iter_append_fixed_array, "dbus_message_iter_append_fixed_array") &&
        bind(l, d.iter_open_container, "dbus_message_iter_open_container") &&
        bind(l, d.iter_close_container, "dbus_message_iter_close_container") &&
        bind(l, d.iter_abandon_container_if_open, "dbus_message_iter_abandon_container_if_open") &&
        bind(l, d.iter_init, "dbus_message_iter_init") &&
        bind(l, d.iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
        bind(l, d.iter_get_element_type, "dbus_message_iter_get_element_type") &&
        bind(l, d.iter_recurse, "dbus_message_iter_recurse") && bind(l, d.iter_next, "dbus_message_iter_next") &&
        bind(l, d.iter_get_basic, "dbus_message_iter_get_basic") &&
        bind(l, d.iter_get_fixed_array, "dbus_message_iter_get_fixed_array");
    // libdbus initialises its locks itself since 1.7; the call is idempotent and costs nothing on those versions.
    d.loaded = bound && d.threads_init_default() != 0U;
    return d;
}

// Loaded once and never unloaded: libdbus keeps process-wide state (its locks, the session bus's address) that only
// its own globals point to, so it must stay mapped for the life of the process, as every other libdbus user keeps it.
const Dbus& dbus() noexcept
{
    alignas(Dbus) static unsigned char storage[sizeof(Dbus)];
    static const Dbus* const           kLoaded = ::new (static_cast<void*>(storage)) Dbus(load_dbus());
    return *kLoaded;
}

// ---- The Secret Service API (draft 0.2) ----

constexpr char kService[] = "org.freedesktop.secrets";
constexpr char kServicePath[] = "/org/freedesktop/secrets";
constexpr char kServiceInterface[] = "org.freedesktop.Secret.Service";
constexpr char kItemInterface[] = "org.freedesktop.Secret.Item";
constexpr char kPromptInterface[] = "org.freedesktop.Secret.Prompt";
constexpr char kTransport[] = "dh-ietf1024-sha256-aes128-cbc-pkcs7";
constexpr char kNoObject[] = "/";
constexpr char kSchema[] = "org.cerid.KeyStore.Master"; // the xdg:schema attribute libsecret's tools show
constexpr char kItemLabel[] = "Cerid key store master key";
constexpr char kCollectionLabel[] = "Default keyring";
constexpr char kContentType[] = "application/octet-stream";
constexpr u8 kReferenceFormat = 1U;
constexpr usize kHandleBytes = 16U;
constexpr usize kReferenceBytes = 1U + kHandleBytes; // the format, then the item's random handle
constexpr usize kHexBytes = 2U * kKeyStoreIdBytes + 1U;
static_assert(kHandleBytes == kKeyStoreIdBytes, "the handle and the store identifier share one text length");

// The errors a call answers with Denied: the service will not (a locked item, another user's or a forgotten item);
// every other error, and a reply the protocol does not allow, is Unavailable (no service that works).
constexpr const char* kDeniedErrors[] = {
    "org.freedesktop.Secret.Error.IsLocked",
    "org.freedesktop.Secret.Error.NoSuchObject",
    "org.freedesktop.DBus.Error.AccessDenied",
    "org.freedesktop.DBus.Error.UnknownObject",
};

constexpr char kUnknownMethod[] = "org.freedesktop.DBus.Error.UnknownMethod";

CryptoError error_of(const char* name) noexcept
{
    for (const char* denied : kDeniedErrors)
    {
        if (std::strcmp(name, denied) == 0)
        {
            return CryptoError::Denied;
        }
    }
    return CryptoError::Unavailable;
}

void hex_of(cont::ConstSpan<u8> bytes, char (&out)[kHexBytes]) noexcept
{
    *detail::hex_into(bytes, out) = '\0';
}

// The attributes that find one master key: Cerid's schema, the store and the reference's handle.
struct Attributes
{
    char store[kHexBytes] = {};
    char handle[kHexBytes] = {};
};

class Message
{
public:
    explicit Message(DBusMessage* message = nullptr) noexcept : m_message(message) {}
    ~Message() noexcept { reset(); }
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    Message(Message&&) = delete;
    Message& operator=(Message&&) = delete;

    void reset(DBusMessage* message = nullptr) noexcept
    {
        if (m_message != nullptr)
        {
            dbus().message_unref(m_message);
        }
        m_message = message;
    }

    [[nodiscard]] DBusMessage* get() const noexcept { return m_message; }

private:
    DBusMessage* m_message;
};

// An open container of a message being written: closed by close(), abandoned if it never was.
class Container
{
public:
    Container(DBusMessageIter& parent, int type, const char* signature) noexcept : m_parent(parent)
    {
        m_ok = dbus().iter_open_container(&m_parent, type, signature, &m_sub) != 0U;
    }
    ~Container() noexcept { dbus().iter_abandon_container_if_open(&m_parent, &m_sub); }
    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    Container(Container&&) = delete;
    Container& operator=(Container&&) = delete;

    [[nodiscard]] DBusMessageIter& sub() noexcept { return m_sub; }
    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] bool close() noexcept { return m_ok && dbus().iter_close_container(&m_parent, &m_sub) != 0U; }

private:
    DBusMessageIter& m_parent;
    DBusMessageIter m_sub{}; // all zero: DBUS_MESSAGE_ITER_INIT_CLOSED
    bool m_ok = false;
};

bool append_text(DBusMessageIter& it, int type, const char* text) noexcept
{
    return dbus().iter_append_basic(&it, type, static_cast<const void*>(&text)) != 0U;
}

bool append_bool(DBusMessageIter& it, bool value) noexcept
{
    const DBusBool flag = value ? 1U : 0U;
    return dbus().iter_append_basic(&it, kTypeBoolean, &flag) != 0U;
}

bool append_bytes(DBusMessageIter& it, cont::ConstSpan<u8> bytes) noexcept
{
    Container array(it, kTypeArray, "y");
    const u8* data = bytes.data();
    return array.ok() &&
           dbus().iter_append_fixed_array(&array.sub(), kTypeByte, static_cast<const void*>(&data),
                                          static_cast<int>(bytes.size())) != 0U &&
           array.close();
}

bool append_paths(DBusMessageIter& it, const cont::Array<cont::String>& paths) noexcept
{
    Container array(it, kTypeArray, "o");
    if (!array.ok())
    {
        return false;
    }
    for (const cont::String& path : paths)
    {
        if (!append_text(array.sub(), kTypeObjectPath, path.c_str()))
        {
            return false;
        }
    }
    return array.close();
}

bool append_attributes(DBusMessageIter& it, const Attributes& attributes) noexcept
{
    const char* const pairs[][2] = {
        {"xdg:schema", kSchema}, {"store", attributes.store}, {"handle", attributes.handle}};
    Container array(it, kTypeArray, "{ss}");
    if (!array.ok())
    {
        return false;
    }
    for (const auto& pair : pairs)
    {
        Container entry(array.sub(), kTypeDictEntry, nullptr);
        if (!entry.ok() || !append_text(entry.sub(), kTypeString, pair[0]) ||
            !append_text(entry.sub(), kTypeString, pair[1]) || !entry.close())
        {
            return false;
        }
    }
    return array.close();
}

// One property of a{sv} whose value is a string.
bool append_text_property(DBusMessageIter& properties, const char* name, const char* value) noexcept
{
    Container entry(properties, kTypeDictEntry, nullptr);
    if (!entry.ok() || !append_text(entry.sub(), kTypeString, name))
    {
        return false;
    }
    Container variant(entry.sub(), kTypeVariant, "s");
    return variant.ok() && append_text(variant.sub(), kTypeString, value) && variant.close() && entry.close();
}

// A reply's arguments, read in the order its checked signature fixes.
class Reader
{
public:
    explicit Reader(const Message& message) noexcept { (void)dbus().iter_init(message.get(), &m_it); }

    [[nodiscard]] DBusMessageIter& it() noexcept { return m_it; }

private:
    DBusMessageIter m_it{};
};

const char* read_text(DBusMessageIter& it) noexcept
{
    const char* text = nullptr;
    dbus().iter_get_basic(&it, static_cast<void*>(&text));
    (void)dbus().iter_next(&it);
    return text;
}

bool read_bool(DBusMessageIter& it) noexcept
{
    DBusBool flag = 0U;
    dbus().iter_get_basic(&it, &flag);
    (void)dbus().iter_next(&it);
    return flag != 0U;
}

// An `ay` the iterator stands on; the bytes stay the message's.
cont::ConstSpan<u8> read_bytes(DBusMessageIter& it) noexcept
{
    DBusMessageIter array{};
    dbus().iter_recurse(&it, &array);
    const u8* data = nullptr;
    int count = 0;
    dbus().iter_get_fixed_array(&array, static_cast<void*>(&data), &count);
    (void)dbus().iter_next(&it);
    return count > 0 ? cont::ConstSpan<u8>(data, static_cast<usize>(count)) : cont::ConstSpan<u8>();
}

void read_paths(DBusMessageIter& it, cont::Array<cont::String>& paths) noexcept
{
    DBusMessageIter array{};
    dbus().iter_recurse(&it, &array);
    while (dbus().iter_get_arg_type(&array) == kTypeObjectPath)
    {
        paths.push_back(cont::String(read_text(array)));
    }
    (void)dbus().iter_next(&it);
}

// One protector call: a private connection to the session bus, and the transport session where a secret crosses it.
class Session
{
public:
    explicit Session(u32 prompt_milliseconds) noexcept : m_prompt_milliseconds(prompt_milliseconds) {}
    ~Session() noexcept
    {
        if (m_connection != nullptr)
        {
            dbus().connection_close(m_connection);
            dbus().connection_unref(m_connection);
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // The bus, never the process's shared connection: closing it disturbs no other user of libdbus.
    [[nodiscard]] CryptoError connect() noexcept
    {
        if (!dbus().loaded)
        {
            return CryptoError::Unavailable;
        }
        DBusError error{};
        dbus().error_init(&error);
        m_connection = dbus().bus_get_private(kBusSession, &error);
        dbus().error_free(&error);
        if (m_connection == nullptr)
        {
            return CryptoError::Unavailable;
        }
        dbus().connection_set_exit_on_disconnect(m_connection, 0U);
        return CryptoError::None;
    }

    // Sends a call and checks its reply's signature: the reply's arguments are then read without further checks. A
    // service that answers UnknownMethod is asked once more, with a copy of the call: gnome-keyring 46 now and then
    // answers so for a Secret Service method while other clients' calls run (four threads of the Secret Service test,
    // about one call in two thousand), and a method it did not dispatch never ran. A second such answer stands
    // (Unavailable: a service without the method).
    [[nodiscard]] CryptoError call(const Message& call, const char* signature, Message& reply) noexcept
    {
        if (call.get() == nullptr)
        {
            return CryptoError::ProviderFailure;
        }
        bool        unknown = false;
        CryptoError answer  = send(call.get(), reply, unknown);
        if (unknown)
        {
            const Message again(dbus().message_copy(call.get()));
            if (again.get() == nullptr)
            {
                return CryptoError::ProviderFailure;
            }
            answer = send(again.get(), reply, unknown);
        }
        if (answer != CryptoError::None || reply.get() == nullptr)
        {
            return answer != CryptoError::None ? answer : CryptoError::Unavailable;
        }
        return std::strcmp(dbus().message_get_signature(reply.get()), signature) == 0 ? CryptoError::None
                                                                                      : CryptoError::Unavailable;
    }

    // The service's transport session: the client's public value out, the service's back.
    [[nodiscard]] CryptoError open_transport() noexcept
    {
        u8          mine[kSecretTransportGroupBytes];
        usize       mine_bytes = 0U;
        CryptoError result     = m_transport.generate();
        if (result == CryptoError::None)
        {
            result = m_transport.public_key(cont::Span<u8, kSecretTransportGroupBytes>(mine), mine_bytes);
        }
        if (result != CryptoError::None)
        {
            return result;
        }
        Message call(new_call(kServicePath, kServiceInterface, "OpenSession"));
        DBusMessageIter it{};
        if (call.get() != nullptr)
        {
            dbus().iter_init_append(call.get(), &it);
            if (!append_text(it, kTypeString, kTransport))
            {
                return CryptoError::ProviderFailure;
            }
            Container variant(it, kTypeVariant, "ay");
            if (!variant.ok() || !append_bytes(variant.sub(), cont::ConstSpan<u8>(mine, mine_bytes)) ||
                !variant.close())
            {
                return CryptoError::ProviderFailure;
            }
        }
        Message reply;
        result = this->call(call, "vo", reply);
        if (result != CryptoError::None)
        {
            return result;
        }
        Reader read(reply);
        DBusMessageIter value{};
        dbus().iter_recurse(&read.it(), &value);
        if (dbus().iter_get_arg_type(&value) != kTypeArray || dbus().iter_get_element_type(&value) != kTypeByte)
        {
            return CryptoError::Unavailable;
        }
        const cont::ConstSpan<u8> theirs = read_bytes(value);
        (void)dbus().iter_next(&read.it());
        m_session = cont::String(read_text(read.it()));
        result = m_transport.agree(theirs);
        return result == CryptoError::None ? result : CryptoError::Unavailable;
    }

    // The default collection, made if there is none (the service may prompt), and unlocked.
    [[nodiscard]] CryptoError default_collection(cont::String& path) noexcept
    {
        CryptoError result = read_alias(path);
        if (result == CryptoError::None && path == kNoObject)
        {
            result = create_default_collection();
            if (result == CryptoError::None)
            {
                result = read_alias(path);
            }
            if (result == CryptoError::None && path == kNoObject)
            {
                result = CryptoError::Denied;
            }
        }
        if (result != CryptoError::None)
        {
            return result;
        }
        cont::Array<cont::String> paths;
        paths.push_back(cont::String(path.c_str()));
        return unlock(paths);
    }

    // Unlocks `paths`; a prompt the service asks for is shown only where the caller allows prompts.
    [[nodiscard]] CryptoError unlock(const cont::Array<cont::String>& paths) noexcept
    {
        Message call(new_call(kServicePath, kServiceInterface, "Unlock"));
        DBusMessageIter it{};
        if (call.get() != nullptr)
        {
            dbus().iter_init_append(call.get(), &it);
            if (!append_paths(it, paths))
            {
                return CryptoError::ProviderFailure;
            }
        }
        Message reply;
        const CryptoError result = this->call(call, "aoo", reply);
        if (result != CryptoError::None)
        {
            return result;
        }
        Reader read(reply);
        (void)dbus().iter_next(&read.it());
        return prompt(read_text(read.it()));
    }

    // The items holding the master key `attributes` names, unlocked first.
    [[nodiscard]] CryptoError find(const Attributes& attributes, cont::Array<cont::String>& items) noexcept
    {
        Message call(new_call(kServicePath, kServiceInterface, "SearchItems"));
        DBusMessageIter it{};
        if (call.get() != nullptr)
        {
            dbus().iter_init_append(call.get(), &it);
            if (!append_attributes(it, attributes))
            {
                return CryptoError::ProviderFailure;
            }
        }
        Message reply;
        CryptoError result = this->call(call, "aoao", reply);
        if (result != CryptoError::None)
        {
            return result;
        }
        cont::Array<cont::String> locked;
        Reader read(reply);
        read_paths(read.it(), items);
        read_paths(read.it(), locked);
        if (!locked.empty())
        {
            result = unlock(locked);
            for (const cont::String& path : locked)
            {
                items.push_back(cont::String(path.c_str()));
            }
        }
        return result;
    }

    [[nodiscard]] CryptoError create_item(const cont::String& collection, const Attributes& attributes,
                                          cont::ConstSpan<u8> master) noexcept
    {
        u8          iv[kSecretTransportIvBytes];
        u8          sealed[secret_transport_sealed_bytes(kKeyStoreMasterBytes)];
        CryptoError result = random_bytes(cont::Span<u8>(iv));
        if (result == CryptoError::None)
        {
            result = m_transport.seal(cont::ConstSpan<u8>(iv), master, cont::Span<u8>(sealed));
        }
        if (result != CryptoError::None)
        {
            return result;
        }
        Message call(new_call(collection.c_str(), "org.freedesktop.Secret.Collection", "CreateItem"));
        DBusMessageIter it{};
        if (call.get() != nullptr)
        {
            dbus().iter_init_append(call.get(), &it);
            if (!append_item_properties(it, attributes) ||
                !append_secret(it, cont::ConstSpan<u8>(iv), cont::ConstSpan<u8>(sealed)) ||
                !append_bool(it, false))
            {
                return CryptoError::ProviderFailure;
            }
        }
        Message reply;
        result = this->call(call, "oo", reply);
        if (result != CryptoError::None)
        {
            return result;
        }
        Reader read(reply);
        (void)dbus().iter_next(&read.it());
        return prompt(read_text(read.it()));
    }

    // The item's secret, opened from the transport session into `master` (exactly its length): Denied for a secret
    // of another session, length or padding, which no master key this protector stored can be.
    [[nodiscard]] CryptoError read_secret(const cont::String& item, cont::Span<u8> master) noexcept
    {
        Message call(new_call(item.c_str(), kItemInterface, "GetSecret"));
        DBusMessageIter it{};
        if (call.get() != nullptr)
        {
            dbus().iter_init_append(call.get(), &it);
            if (!append_text(it, kTypeObjectPath, m_session.c_str()))
            {
                return CryptoError::ProviderFailure;
            }
        }
        Message reply;
        const CryptoError result = this->call(call, "(oayays)", reply);
        if (result != CryptoError::None)
        {
            return result;
        }
        Reader read(reply);
        DBusMessageIter secret{};
        dbus().iter_recurse(&read.it(), &secret);
        const bool ours = m_session == read_text(secret);
        const cont::ConstSpan<u8> iv = read_bytes(secret);
        const cont::ConstSpan<u8> sealed = read_bytes(secret);
        if (!ours || m_transport.open(iv, sealed, master) != CryptoError::None)
        {
            secure_zero(master);
            return CryptoError::Denied;
        }
        return CryptoError::None;
    }

    [[nodiscard]] CryptoError delete_item(const cont::String& item) noexcept
    {
        Message call(new_call(item.c_str(), kItemInterface, "Delete"));
        Message reply;
        CryptoError result = this->call(call, "o", reply);
        if (result == CryptoError::None)
        {
            Reader read(reply);
            result = prompt(read_text(read.it()));
        }
        return result;
    }

    // Whether the service answers on the bus (started by the bus where it is activatable).
    [[nodiscard]] CryptoError ping() noexcept
    {
        Message call(new_call(kServicePath, "org.freedesktop.DBus.Peer", "Ping"));
        Message reply;
        return this->call(call, "", reply);
    }

private:
    // One round trip: the reply, or the error's answer; `unknown` when the error is UnknownMethod.
    [[nodiscard]] CryptoError send(DBusMessage* message, Message& reply, bool& unknown) noexcept
    {
        DBusError error{};
        dbus().error_init(&error);
        reply.reset(dbus().connection_send_with_reply_and_block(m_connection, message, kCallMilliseconds, &error));
        unknown = error.name != nullptr && std::strcmp(error.name, kUnknownMethod) == 0;
        const CryptoError answer = error.name != nullptr ? error_of(error.name) : CryptoError::None;
        dbus().error_free(&error);
        return answer;
    }

    [[nodiscard]] static DBusMessage* new_call(const char* path, const char* interface, const char* method) noexcept
    {
        return dbus().message_new_method_call(kService, path, interface, method);
    }

    [[nodiscard]] CryptoError read_alias(cont::String& path) noexcept
    {
        Message call(new_call(kServicePath, kServiceInterface, "ReadAlias"));
        DBusMessageIter it{};
        if (call.get() != nullptr)
        {
            dbus().iter_init_append(call.get(), &it);
            if (!append_text(it, kTypeString, "default"))
            {
                return CryptoError::ProviderFailure;
            }
        }
        Message reply;
        const CryptoError result = this->call(call, "o", reply);
        if (result == CryptoError::None)
        {
            Reader read(reply);
            path = cont::String(read_text(read.it()));
        }
        return result;
    }

    [[nodiscard]] CryptoError create_default_collection() noexcept
    {
        Message call(new_call(kServicePath, kServiceInterface, "CreateCollection"));
        DBusMessageIter it{};
        if (call.get() != nullptr)
        {
            dbus().iter_init_append(call.get(), &it);
            Container properties(it, kTypeArray, "{sv}");
            if (!properties.ok() ||
                !append_text_property(properties.sub(), "org.freedesktop.Secret.Collection.Label", kCollectionLabel) ||
                !properties.close() || !append_text(it, kTypeString, "default"))
            {
                return CryptoError::ProviderFailure;
            }
        }
        Message reply;
        const CryptoError result = this->call(call, "oo", reply);
        if (result != CryptoError::None)
        {
            return result;
        }
        Reader read(reply);
        (void)dbus().iter_next(&read.it());
        return prompt(read_text(read.it()));
    }

    [[nodiscard]] static bool append_item_properties(DBusMessageIter& it, const Attributes& attributes) noexcept
    {
        Container properties(it, kTypeArray, "{sv}");
        if (!properties.ok() ||
            !append_text_property(properties.sub(), "org.freedesktop.Secret.Item.Label", kItemLabel))
        {
            return false;
        }
        Container entry(properties.sub(), kTypeDictEntry, nullptr);
        if (!entry.ok() || !append_text(entry.sub(), kTypeString, "org.freedesktop.Secret.Item.Attributes"))
        {
            return false;
        }
        Container variant(entry.sub(), kTypeVariant, "a{ss}");
        return variant.ok() && append_attributes(variant.sub(), attributes) && variant.close() && entry.close() &&
               properties.close();
    }

    [[nodiscard]] bool append_secret(DBusMessageIter& it, cont::ConstSpan<u8> iv, cont::ConstSpan<u8> sealed) noexcept
    {
        Container secret(it, kTypeStruct, nullptr);
        return secret.ok() && append_text(secret.sub(), kTypeObjectPath, m_session.c_str()) &&
               append_bytes(secret.sub(), iv) && append_bytes(secret.sub(), sealed) &&
               append_text(secret.sub(), kTypeString, kContentType) && secret.close();
    }

    // Runs the prompt at `path` ("/" when the service needs none) and waits, at most the caller's bound, for its
    // Completed signal: Denied when prompts are not allowed, when the user dismissed it or when the bound passed.
    [[nodiscard]] CryptoError prompt(const char* path) noexcept
    {
        if (std::strcmp(path, kNoObject) == 0)
        {
            return CryptoError::None;
        }
        if (m_prompt_milliseconds == 0U)
        {
            return CryptoError::Denied;
        }
        const cont::String prompt_path(path);
        cont::String rule("type='signal',interface='org.freedesktop.Secret.Prompt',member='Completed',path='");
        rule += prompt_path.c_str();
        rule += "'";
        DBusError error{};
        dbus().error_init(&error);
        dbus().bus_add_match(m_connection, rule.c_str(), &error);
        const bool matched = error.name == nullptr;
        dbus().error_free(&error);
        if (!matched)
        {
            return CryptoError::Unavailable;
        }
        Message call(new_call(prompt_path.c_str(), kPromptInterface, "Prompt"));
        DBusMessageIter it{};
        if (call.get() != nullptr)
        {
            dbus().iter_init_append(call.get(), &it);
            if (!append_text(it, kTypeString, ""))
            {
                return CryptoError::ProviderFailure;
            }
        }
        Message reply;
        CryptoError result = this->call(call, "", reply);
        if (result != CryptoError::None)
        {
            return result;
        }
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::milliseconds(m_prompt_milliseconds);
        for (;;)
        {
            for (Message signal(dbus().connection_pop_message(m_connection)); signal.get() != nullptr;
                 signal.reset(dbus().connection_pop_message(m_connection)))
            {
                const char* from = dbus().message_get_path(signal.get());
                if (dbus().message_is_signal(signal.get(), kPromptInterface, "Completed") != 0U && from != nullptr &&
                    prompt_path == from)
                {
                    if (std::strcmp(dbus().message_get_signature(signal.get()), "bv") != 0)
                    {
                        return CryptoError::Unavailable;
                    }
                    Reader read(signal);
                    return read_bool(read.it()) ? CryptoError::Denied : CryptoError::None;
                }
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0)
            {
                Message dismiss(new_call(prompt_path.c_str(), kPromptInterface, "Dismiss"));
                Message ignored;
                (void)this->call(dismiss, "", ignored);
                return CryptoError::Denied;
            }
            const int wait = static_cast<int>(left < kCallMilliseconds ? left : kCallMilliseconds);
            if (dbus().connection_read_write(m_connection, wait) == 0U)
            {
                return CryptoError::Unavailable; // the bus went away
            }
        }
    }

    SecretTransportKey m_transport;
    cont::String m_session;
    DBusConnection* m_connection = nullptr;
    u32 m_prompt_milliseconds;
};

// A reference this protector wrote: its format byte, then the item's handle.
[[nodiscard]] bool attributes_of(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                 Attributes& attributes) noexcept
{
    if (store_id.size() != kKeyStoreIdBytes || reference.size() != kReferenceBytes || reference[0] != kReferenceFormat)
    {
        return false;
    }
    hex_of(store_id, attributes.store);
    hex_of(reference.subspan(1U), attributes.handle);
    return true;
}
} // namespace

bool PlatformKeyProtector::available() noexcept
{
    Session session(0U);
    return session.connect() == CryptoError::None && session.ping() == CryptoError::None;
}

cont::StringView PlatformKeyProtector::name() const noexcept
{
    return "secret-service";
}

bool PlatformKeyProtector::forgets() const noexcept
{
    return true; // Item.Delete destroys the item, and the master key with it
}

CryptoError PlatformKeyProtector::protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                          cont::Array<u8>& reference) noexcept
{
    reference.clear();
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    u8 written[kReferenceBytes] = {kReferenceFormat};
    Attributes attributes;
    CryptoError result = random_bytes(cont::Span<u8>(written + 1U, kHandleBytes));
    if (result != CryptoError::None || !attributes_of(store_id, cont::ConstSpan<u8>(written), attributes))
    {
        return result != CryptoError::None ? result : CryptoError::ProviderFailure;
    }
    Session session(m_prompt_milliseconds);
    cont::String collection;
    result = session.connect();
    if (result == CryptoError::None)
    {
        result = session.open_transport();
    }
    if (result == CryptoError::None)
    {
        result = session.default_collection(collection);
    }
    if (result == CryptoError::None)
    {
        result = session.create_item(collection, attributes, master);
    }
    if (result == CryptoError::None)
    {
        reference.resize(kReferenceBytes);
        std::memcpy(reference.data(), written, kReferenceBytes);
    }
    return result;
}

CryptoError PlatformKeyProtector::recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                          cont::Span<u8> master) noexcept
{
    secure_zero(master);
    if (store_id.size() != kKeyStoreIdBytes || master.size() != kKeyStoreMasterBytes)
    {
        return CryptoError::BadLength;
    }
    Attributes attributes;
    if (!attributes_of(store_id, reference, attributes))
    {
        return CryptoError::Denied; // not a reference this protector wrote
    }
    Session session(m_prompt_milliseconds);
    cont::Array<cont::String> items;
    CryptoError result = session.connect();
    if (result == CryptoError::None)
    {
        result = session.open_transport();
    }
    if (result == CryptoError::None)
    {
        result = session.find(attributes, items);
    }
    if (result == CryptoError::None)
    {
        // A handle is random and written once, so a second item is not this protector's: it recovers nothing.
        result = items.size() == 1U ? session.read_secret(items[0], master) : CryptoError::Denied;
    }
    if (result != CryptoError::None)
    {
        secure_zero(master);
    }
    return result;
}

CryptoError PlatformKeyProtector::forget(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference) noexcept
{
    Attributes attributes;
    if (!attributes_of(store_id, reference, attributes))
    {
        return CryptoError::Denied;
    }
    Session session(m_prompt_milliseconds);
    cont::Array<cont::String> items;
    CryptoError result = session.connect();
    if (result == CryptoError::None)
    {
        result = session.find(attributes, items);
    }
    for (usize i = 0U; result == CryptoError::None && i < items.size(); ++i)
    {
        result = session.delete_item(items[i]);
    }
    return result;
}
} // namespace crd::crypto
#endif
