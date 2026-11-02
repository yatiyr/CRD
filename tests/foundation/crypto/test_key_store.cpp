// crd-crypto -- the key store (NET.2; docs/design/network-foundation.md#key-store, decisions K1 to K10): every kind
// of key generated, imported, used, rotated, revoked and destroyed across reopenings; the master key replaced on
// revocation so an earlier copy of the store no longer opens; an interrupted replacement completed by the next change;
// every byte of a store file changed and refused; a damaged, truncated, older or substituted store refused; writers in
// several threads ordered by the store's lock; DPAPI guarding the master key on Windows and the Secret Service on Linux
// (under a pinned gnome-keyring, hidden from the default run); a TLS credential from a stored signer.

#include <crd/core/environment.hpp>
#include <crd/core/platform.hpp>
#include <crd/crypto/key_store.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/platform/durable_file.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/threading.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::String;
using crd::containers::StringView;
using namespace crd::crypto;
namespace fs = crd::platform::fs;

using Bytes = Array<u8>;

// A store directory under the system's scratch directory, removed with everything in it at the end of its scope.
class TempStore
{
public:
    explicit TempStore(const char* tag)
    {
        static std::atomic<u32> serial{0U};
        String                  leaf("crd_key_store_");
        leaf.append(tag);
        leaf.push_back('_');
        leaf.append(std::to_string(crd::platform::threading::current_thread_id()));
        leaf.push_back('_');
        leaf.append(std::to_string(serial.fetch_add(1U)));
        m_path = fs::temp_directory() / StringView(leaf.data(), leaf.size());
        (void)fs::remove_all(m_path);
    }
    TempStore(const TempStore&)            = delete;
    TempStore& operator=(const TempStore&) = delete;
    ~TempStore() { (void)fs::remove_all(m_path); }

    [[nodiscard]] StringView dir() const { return m_path.generic(); }
    [[nodiscard]] fs::Path   file() const { return m_path / "keys"; }

private:
    fs::Path m_path;
};

// The application's protector of the tests: master keys held in memory under random references, each bound to its
// store; it can forget, refuse or fail on demand, and counts its calls.
class TestProtector final : public KeyProtector
{
public:
    explicit TestProtector(bool forgets = true, StringView name = "test") : m_forgets(forgets), m_name(name) {}

    [[nodiscard]] StringView    name() const noexcept override { return m_name; }
    [[nodiscard]] KeyProtection protection() const noexcept override { return KeyProtection::Application; }
    [[nodiscard]] bool          forgets() const noexcept override { return m_forgets; }

    [[nodiscard]] CryptoError protect(ConstSpan<u8> store_id, ConstSpan<u8> master, Bytes& reference) noexcept override
    {
        ++protects;
        reference.clear();
        if (fail_protect != CryptoError::None)
        {
            return fail_protect;
        }
        Held held;
        if (random_bytes(Span<u8>(held.reference)) != CryptoError::None)
        {
            return CryptoError::ProviderFailure;
        }
        std::memcpy(held.store, store_id.data(), kKeyStoreIdBytes);
        std::memcpy(held.master, master.data(), kKeyStoreMasterBytes);
        m_held.push_back(held);
        reference.resize(sizeof(held.reference));
        std::memcpy(reference.data(), held.reference, sizeof(held.reference));
        return CryptoError::None;
    }

    [[nodiscard]] CryptoError recover(ConstSpan<u8> store_id, ConstSpan<u8> reference,
                                      Span<u8> master) noexcept override
    {
        ++recovers;
        const Held* held = find(store_id, reference);
        if (fail_recover != CryptoError::None || held == nullptr)
        {
            secure_zero(master);
            return fail_recover != CryptoError::None ? fail_recover : CryptoError::Denied;
        }
        std::memcpy(master.data(), held->master, kKeyStoreMasterBytes);
        return CryptoError::None;
    }

    [[nodiscard]] CryptoError forget(ConstSpan<u8> store_id, ConstSpan<u8> reference) noexcept override
    {
        ++forgets_called;
        if (fail_forget != CryptoError::None)
        {
            return fail_forget;
        }
        if (const Held* held = find(store_id, reference); held != nullptr && m_forgets)
        {
            m_held.erase(static_cast<usize>(held - m_held.data()));
        }
        return CryptoError::None;
    }

    [[nodiscard]] usize held() const { return m_held.size(); }

    CryptoError        fail_protect = CryptoError::None;
    CryptoError        fail_recover = CryptoError::None;
    CryptoError        fail_forget  = CryptoError::None;
    std::atomic<usize> protects{0U}; // recover() runs on several threads at once (the writers' case)
    std::atomic<usize> recovers{0U};
    std::atomic<usize> forgets_called{0U};

private:
    struct Held
    {
        u8 store[kKeyStoreIdBytes]      = {};
        u8 reference[16]                = {};
        u8 master[kKeyStoreMasterBytes] = {};
    };

    [[nodiscard]] const Held* find(ConstSpan<u8> store_id, ConstSpan<u8> reference) const
    {
        for (const Held& held : m_held)
        {
            if (store_id.size() == kKeyStoreIdBytes && reference.size() == sizeof(held.reference) &&
                std::memcmp(held.store, store_id.data(), kKeyStoreIdBytes) == 0 &&
                std::memcmp(held.reference, reference.data(), sizeof(held.reference)) == 0)
            {
                return &held;
            }
        }
        return nullptr;
    }

    bool        m_forgets;
    StringView  m_name;
    Array<Held> m_held;
};

[[nodiscard]] Bytes read_file(const fs::Path& path)
{
    Bytes bytes;
    REQUIRE(fs::read_file_binary(path, bytes));
    return bytes;
}

// The store's payload: its file without the durable save's envelope.
[[nodiscard]] Bytes payload_of(const TempStore& store)
{
    Bytes payload;
    REQUIRE(fs::load_generation(store.file(), 0U, payload).status == fs::SaveStatus::Ok);
    return payload;
}

void write_payload(const TempStore& store, ConstSpan<u8> payload)
{
    REQUIRE(fs::save_generation(store.file(), payload, 0U).ok);
}

void write_file(const fs::Path& path, const Bytes& bytes)
{
    REQUIRE(fs::write_file_binary(path, ConstSpan<u8>(bytes.data(), bytes.size())));
}

[[nodiscard]] Bytes public_of(const KeyStore& store, StringView name, u32 generation)
{
    const KeyInfo* info = store.find(name, generation);
    REQUIRE(info != nullptr);
    usize length = signature_sizes(info->algorithm).public_key;
    length       = length != 0U ? length : agreement_sizes(info->algorithm).public_key;
    length       = length != 0U ? length : kem_sizes(info->algorithm).public_key;
    Bytes out;
    out.resize(length);
    REQUIRE(store.public_key(name, generation, Span<u8>(out.data(), out.size())) == CryptoError::None);
    return out;
}

// Signs through the store's signer and verifies with the stored public key.
void check_signs(const KeyStore& store, StringView name)
{
    StoredSigner signer;
    REQUIRE(store.signer(name, signer) == CryptoError::None);
    REQUIRE(signer.ready());
    const KeyInfo* current = store.find(name);
    REQUIRE(current != nullptr);
    CHECK(signer.info().generation == current->generation);
    CHECK(signer.info().id == current->id);
    const u8 message[] = {'c', 'e', 'r', 'i', 'd'};
    Bytes    signature;
    signature.resize(signature_sizes(signer.algorithm()).signature);
    usize written = 0U;
    REQUIRE(signer.sign(ConstSpan<u8>(message), Span<u8>(signature.data(), signature.size()), written) ==
            CryptoError::None);
    const Bytes public_key = public_of(store, name, current->generation);
    Bytes       from_signer;
    from_signer.resize(public_key.size());
    REQUIRE(signer.public_key(Span<u8>(from_signer.data(), from_signer.size())) == CryptoError::None);
    CHECK(std::memcmp(from_signer.data(), public_key.data(), public_key.size()) == 0);
    const ProviderVerifier verifier;
    CHECK(verifier.verify(signer.algorithm(), ConstSpan<u8>(public_key.data(), public_key.size()),
                          ConstSpan<u8>(message), ConstSpan<u8>(signature.data(), written)) == Verification::Valid);
}

// `generation`, or the Current one's for 0.
[[nodiscard]] u32 resolved(const KeyStore& store, StringView name, u32 generation)
{
    if (generation != 0U)
    {
        return generation;
    }
    const KeyInfo* current = store.find(name);
    REQUIRE(current != nullptr);
    return current->generation;
}

// Agrees with a fresh peer both ways through the stored key of `generation`.
void check_agrees(const KeyStore& store, StringView name, u32 generation)
{
    AgreementKey own;
    REQUIRE(store.agreement_key(name, generation, own) == CryptoError::None);
    const AgreementSizes sizes = agreement_sizes(own.algorithm());
    AgreementKey         peer;
    REQUIRE(peer.generate(own.algorithm()) == CryptoError::None);
    Bytes peer_public;
    peer_public.resize(sizes.public_key);
    REQUIRE(peer.public_key(Span<u8>(peer_public.data(), peer_public.size())) == CryptoError::None);
    const Bytes own_public = public_of(store, name, resolved(store, name, generation));
    u8          a[48]      = {};
    u8          b[48]      = {};
    REQUIRE(own.derive(ConstSpan<u8>(peer_public.data(), peer_public.size()), Span<u8>(a, sizes.secret)) ==
            CryptoError::None);
    REQUIRE(peer.derive(ConstSpan<u8>(own_public.data(), own_public.size()), Span<u8>(b, sizes.secret)) ==
            CryptoError::None);
    CHECK(std::memcmp(a, b, sizes.secret) == 0);
}

// Encapsulates to the stored public key and decapsulates through the stored key.
void check_decapsulates(const KeyStore& store, StringView name, u32 generation)
{
    KemKey own;
    REQUIRE(store.kem_key(name, generation, own) == CryptoError::None);
    const KemSizes sizes      = kem_sizes(own.algorithm());
    const Bytes    own_public = public_of(store, name, resolved(store, name, generation));
    Bytes          ciphertext;
    ciphertext.resize(sizes.ciphertext);
    u8 sent[32]     = {};
    u8 received[32] = {};
    REQUIRE(kem_encapsulate(own.algorithm(), ConstSpan<u8>(own_public.data(), own_public.size()),
                            Span<u8>(ciphertext.data(), ciphertext.size()), Span<u8>(sent)) == CryptoError::None);
    REQUIRE(own.decapsulate(ConstSpan<u8>(ciphertext.data(), ciphertext.size()), Span<u8>(received)) ==
            CryptoError::None);
    CHECK(std::memcmp(sent, received, sizeof(sent)) == 0);
}

[[nodiscard]] usize secret_length(AlgorithmId algorithm)
{
    usize length = aead_key_bytes(algorithm);
    length       = length != 0U ? length : hkdf_hash_bytes(algorithm);
    return length != 0U ? length : kPrfKeyBytes;
}

[[nodiscard]] Bytes secret_of(const KeyStore& store, StringView name, u32 generation)
{
    const KeyInfo* info = generation == 0U ? store.find(name) : store.find(name, generation);
    REQUIRE(info != nullptr);
    Bytes out;
    out.resize(secret_length(info->algorithm));
    REQUIRE(store.secret(name, generation, Span<u8>(out.data(), out.size())) == CryptoError::None);
    return out;
}

[[nodiscard]] bool same_infos(const KeyStore& a, const KeyStore& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (usize i = 0U; i < a.size(); ++i)
    {
        const KeyInfo& x = a.key(i);
        const KeyInfo& y = b.key(i);
        if (x.name() != y.name() || x.generation != y.generation || x.algorithm != y.algorithm || x.kind != y.kind ||
            x.state != y.state || x.placement != y.placement || !(x.id == y.id) || x.created != y.created ||
            x.changed != y.changed)
        {
            return false;
        }
    }
    return true;
}

// The name of the writers' case's key `i` of thread `t`.
[[nodiscard]] String writer_name(usize t, usize i)
{
    String name("t");
    name.append(std::to_string(t));
    name.push_back('-');
    name.append(std::to_string(i));
    return name;
}

struct Kept
{
    const char* name;
    AlgorithmId algorithm;
    KeyKind     kind;
};

constexpr Kept kEveryKind[] = {
    {"agree.p256", AlgorithmId::Secp256r1, KeyKind::Agreement},
    {"agree.p384", AlgorithmId::Secp384r1, KeyKind::Agreement},
    {"agree.x25519", AlgorithmId::X25519, KeyKind::Agreement},
    {"kem.1024", AlgorithmId::MlKem1024, KeyKind::Kem},
    {"kem.768", AlgorithmId::MlKem768, KeyKind::Kem},
    {"secret.aes128", AlgorithmId::Aes128Gcm, KeyKind::Secret},
    {"secret.aes256", AlgorithmId::Aes256Gcm, KeyKind::Secret},
    {"secret.chacha", AlgorithmId::Chacha20Poly1305, KeyKind::Secret},
    {"secret.hkdf256", AlgorithmId::HkdfSha256, KeyKind::Secret},
    {"secret.hkdf384", AlgorithmId::HkdfSha384, KeyKind::Secret},
    {"secret.prf", AlgorithmId::Siphash24, KeyKind::Secret},
    {"sign.ecdsa256", AlgorithmId::EcdsaSecp256r1Sha256, KeyKind::Signature},
    {"sign.ecdsa384", AlgorithmId::EcdsaSecp384r1Sha384, KeyKind::Signature},
    {"sign.ed25519", AlgorithmId::Ed25519, KeyKind::Signature},
    {"sign.mldsa44", AlgorithmId::MlDsa44, KeyKind::Signature},
    {"sign.mldsa65", AlgorithmId::MlDsa65, KeyKind::Signature},
    {"sign.mldsa87", AlgorithmId::MlDsa87, KeyKind::Signature},
};

void check_uses(const KeyStore& store, const Kept& kept, u32 generation)
{
    switch (kept.kind)
    {
    case KeyKind::Signature:
        check_signs(store, kept.name);
        break;
    case KeyKind::Agreement:
        check_agrees(store, kept.name, generation);
        break;
    case KeyKind::Kem:
        check_decapsulates(store, kept.name, generation);
        break;
    case KeyKind::Secret:
    {
        const Bytes value = secret_of(store, kept.name, generation);
        KeyId       named;
        const auto  label = ConstSpan<u8>(reinterpret_cast<const u8*>("cerid key id"), 12U);
        u8          prk[32];
        REQUIRE(hkdf_extract(AlgorithmId::HkdfSha256, {}, ConstSpan<u8>(value.data(), value.size()), Span<u8>(prk)) ==
                CryptoError::None);
        REQUIRE(hkdf_expand(AlgorithmId::HkdfSha256, ConstSpan<u8>(prk), label, Span<u8>(named.bytes)) ==
                CryptoError::None);
        const KeyInfo* info = generation == 0U ? store.find(kept.name) : store.find(kept.name, generation);
        REQUIRE(info != nullptr);
        CHECK(named == info->id);
        break;
    }
    }
}
} // namespace

TEST_CASE("Key store: names, kinds and protections", "[crypto][key-store]")
{
    CHECK(valid_key_name("a"));
    CHECK(valid_key_name("0"));
    CHECK(valid_key_name("server.tls-identity_2"));
    CHECK(valid_key_name(StringView("abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz01", 64U)));
    CHECK_FALSE(valid_key_name(StringView("abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz012", 65U)));
    CHECK_FALSE(valid_key_name(""));
    CHECK_FALSE(valid_key_name(".hidden"));
    CHECK_FALSE(valid_key_name("-x"));
    CHECK_FALSE(valid_key_name("Upper"));
    CHECK_FALSE(valid_key_name("a/b"));
    CHECK_FALSE(valid_key_name("a b"));
    CHECK_FALSE(valid_key_name(StringView("a\0b", 3U)));

    for (const AlgorithmId id : kAlgorithmIds)
    {
        KeyKind    kind = KeyKind::Secret;
        const bool kept = key_kind_of(id, kind);
        const bool hybrid =
            id == AlgorithmId::X25519Mlkem768 || id == AlgorithmId::Secp256r1Mlkem768 ||
            id == AlgorithmId::Secp384r1Mlkem1024;
        CHECK(kept == !hybrid);
        if (kept)
        {
            bool listed = false;
            for (const Kept& every : kEveryKind)
            {
                listed = listed || (every.algorithm == id && every.kind == kind);
            }
            CHECK(listed);
        }
    }
    KeyKind kind = KeyKind::Secret;
    CHECK_FALSE(key_kind_of(AlgorithmId{}, kind));
    CHECK_FALSE(key_kind_of(static_cast<AlgorithmId>(0x7777U), kind));

    CHECK(key_protection_name(KeyProtection::Application) == StringView("application"));
    CHECK(key_protection_name(KeyProtection::Platform) == StringView("platform"));
    CHECK(key_protection_name(KeyProtection::Hardware) == StringView("hardware"));
    CHECK(key_protection_name(static_cast<KeyProtection>(9)) == StringView("unknown"));
}

TEST_CASE("Key store: every kind of key generated, reopened and used", "[crypto][key-store]")
{
    TempStore     dir("kinds");
    TestProtector protector;
    KeyStore      store;
    CHECK_FALSE(store.ready());
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);
    CHECK(store.ready());
    CHECK(store.size() == 0U);
    CHECK(store.protection() == KeyProtection::Application);
    CHECK(protector.protects.load() == 1U);

    u64 now = 1000U;
    // Generated out of order: the store keeps them by name.
    for (usize i = sizeof(kEveryKind) / sizeof(kEveryKind[0]); i > 0U; --i)
    {
        const Kept& kept = kEveryKind[i - 1U];
        REQUIRE(store.generate(kept.name, kept.algorithm, PlacementRequest::Software, ++now) == CryptoError::None);
    }
    REQUIRE(store.size() == sizeof(kEveryKind) / sizeof(kEveryKind[0]));
    for (usize i = 0U; i < store.size(); ++i)
    {
        const KeyInfo& info = store.key(i);
        CHECK(info.name() == StringView(kEveryKind[i].name));
        CHECK(info.algorithm == kEveryKind[i].algorithm);
        CHECK(info.kind == kEveryKind[i].kind);
        CHECK(info.generation == 1U);
        CHECK(info.state == KeyState::Current);
        CHECK(info.placement == KeyPlacement::Software);
        CHECK(info.created == info.changed);
        check_uses(store, kEveryKind[i], 0U);
        check_uses(store, kEveryKind[i], 1U);
    }

    // Reopened by another store object: the same keys, and the same material behind them.
    KeyStore reopened;
    REQUIRE(reopened.open(dir.dir(), protector) == CryptoError::None);
    CHECK(same_infos(store, reopened));
    CHECK(std::memcmp(reopened.id().data(), store.id().data(), kKeyStoreIdBytes) == 0);
    for (const Kept& kept : kEveryKind)
    {
        check_uses(reopened, kept, 0U);
        if (kept.kind == KeyKind::Secret)
        {
            const Bytes a = secret_of(store, kept.name, 0U);
            const Bytes b = secret_of(reopened, kept.name, 0U);
            CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
        }
        else
        {
            const Bytes a = public_of(store, kept.name, 1U);
            const Bytes b = public_of(reopened, kept.name, 1U);
            CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
        }
    }

    // Each key is used only as its kind; the store answers for nothing it does not hold.
    StoredSigner signer;
    AgreementKey agreement;
    KemKey       kem;
    u8           secret[32] = {1};
    CHECK(store.signer("agree.x25519", signer) == CryptoError::Unsupported);
    CHECK_FALSE(signer.ready());
    CHECK(store.agreement_key("sign.ed25519", 0U, agreement) == CryptoError::Unsupported);
    CHECK(store.kem_key("secret.aes256", 0U, kem) == CryptoError::Unsupported);
    CHECK(store.secret("kem.768", 0U, Span<u8>(secret)) == CryptoError::Unsupported);
    CHECK(secret[0] == 0U);
    CHECK(store.signer("absent", signer) == CryptoError::NotFound);
    CHECK(store.agreement_key("agree.x25519", 2U, agreement) == CryptoError::NotFound);
    CHECK(store.find("absent") == nullptr);
    CHECK(store.find("sign.ed25519", 2U) == nullptr);
    u8 public_key[32];
    CHECK(store.public_key("secret.aes256", 1U, Span<u8>(public_key)) == CryptoError::Unsupported);
    CHECK(store.public_key("sign.ed25519", 1U, Span<u8>(public_key, 31U)) == CryptoError::BadLength);
    CHECK(store.public_key("absent", 1U, Span<u8>(public_key)) == CryptoError::NotFound);
    secret[0] = 1U;
    CHECK(store.secret("secret.aes256", 0U, Span<u8>(secret, 16U)) == CryptoError::BadLength);
    CHECK(secret[0] == 0U);

    // A closed store holds nothing and answers NotReady.
    store.close();
    CHECK_FALSE(store.ready());
    CHECK(store.size() == 0U);
    CHECK(store.signer("sign.ed25519", signer) == CryptoError::NotReady);
    CHECK(store.public_key("sign.ed25519", 1U, Span<u8>(public_key)) == CryptoError::NotReady);
    CHECK(store.generate("x", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::NotReady);
    CHECK(store.refresh() == CryptoError::NotReady);
}

TEST_CASE("Key store: imports, rotation and retired generations", "[crypto][key-store]")
{
    TempStore     dir("rotate");
    TestProtector protector;
    KeyStore      store;
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);

    // RFC 8032 section 7.1, TEST 1: the imported private key gives the RFC's public key.
    const u8 seed[32]     = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                             0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                             0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
    const u8 expected[32] = {0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
                             0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
                             0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
    REQUIRE(store.import("identity", AlgorithmId::Ed25519, ConstSpan<u8>(seed), 10U) == CryptoError::None);
    const Bytes imported = public_of(store, "identity", 1U);
    CHECK(std::memcmp(imported.data(), expected, sizeof(expected)) == 0);
    CHECK(store.find("identity")->id == key_id_of(ConstSpan<u8>(expected)));
    check_signs(store, "identity");

    // Rotation: generation 2 signs, generation 1 is retired and keeps its public key.
    REQUIRE(store.rotate("identity", 20U) == CryptoError::None);
    REQUIRE(store.size() == 2U);
    const KeyInfo* first  = store.find("identity", 1U);
    const KeyInfo* second = store.find("identity", 2U);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->state == KeyState::Retired);
    CHECK(first->changed == 20U);
    CHECK(first->created == 10U);
    CHECK(second->state == KeyState::Current);
    CHECK(second->created == 20U);
    CHECK(store.find("identity") == second);
    CHECK_FALSE(first->id == second->id);
    check_signs(store, "identity");
    StoredSigner signer;
    REQUIRE(store.signer("identity", signer) == CryptoError::None);
    CHECK(signer.info().generation == 2U);
    CHECK(std::memcmp(public_of(store, "identity", 1U).data(), expected, sizeof(expected)) == 0);

    // A retired agreement or KEM key still opens what was sent to it; a retired secret still reads.
    REQUIRE(store.generate("hpke", AlgorithmId::X25519, PlacementRequest::HardwarePreferred, 30U) == CryptoError::None);
    CHECK(store.find("hpke")->placement == KeyPlacement::Software);
    REQUIRE(store.generate("init", AlgorithmId::MlKem768, PlacementRequest::Software, 30U) == CryptoError::None);
    REQUIRE(store.rotate("hpke", 40U) == CryptoError::None);
    REQUIRE(store.rotate("init", 40U) == CryptoError::None);
    check_agrees(store, "hpke", 1U);
    check_agrees(store, "hpke", 2U);
    check_decapsulates(store, "init", 1U);
    check_decapsulates(store, "init", 2U);

    // A fleet's ticket key: imported, then rotated to the value every server receives.
    u8 ticket_a[32];
    u8 ticket_b[32];
    for (usize i = 0U; i < 32U; ++i)
    {
        ticket_a[i] = static_cast<u8>(i);
        ticket_b[i] = static_cast<u8>(0xA0U + i);
    }
    REQUIRE(store.import("tickets", AlgorithmId::HkdfSha256, ConstSpan<u8>(ticket_a), 50U) == CryptoError::None);
    REQUIRE(store.rotate_import("tickets", ConstSpan<u8>(ticket_b), 60U) == CryptoError::None);
    CHECK(std::memcmp(secret_of(store, "tickets", 0U).data(), ticket_b, 32U) == 0);
    CHECK(std::memcmp(secret_of(store, "tickets", 1U).data(), ticket_a, 32U) == 0);
    CHECK(std::memcmp(secret_of(store, "tickets", 2U).data(), ticket_b, 32U) == 0);

    // Every change is on disk.
    KeyStore reopened;
    REQUIRE(reopened.open(dir.dir(), protector) == CryptoError::None);
    CHECK(same_infos(store, reopened));
    check_signs(reopened, "identity");
    check_agrees(reopened, "hpke", 1U);
    CHECK(std::memcmp(secret_of(reopened, "tickets", 1U).data(), ticket_a, 32U) == 0);

    // Refusals of imports and rotations.
    const u8 zero_scalar[32] = {};
    CHECK(store.import("p256", AlgorithmId::EcdsaSecp256r1Sha256, ConstSpan<u8>(zero_scalar), 1U) ==
          CryptoError::BadKey);
    CHECK(store.import("short", AlgorithmId::Ed25519, ConstSpan<u8>(seed, 31U), 1U) == CryptoError::BadLength);
    CHECK(store.import("empty", AlgorithmId::Ed25519, {}, 1U) == CryptoError::BadLength);
    CHECK(store.import("identity", AlgorithmId::Ed25519, ConstSpan<u8>(seed), 1U) == CryptoError::Exists);
    CHECK(store.rotate("absent", 1U) == CryptoError::NotFound);
    CHECK(store.rotate_import("tickets", ConstSpan<u8>(ticket_a, 16U), 1U) == CryptoError::BadLength);
    CHECK(store.rotate_import("tickets", {}, 1U) == CryptoError::BadLength);
    CHECK(store.find("p256") == nullptr);
    CHECK(store.find("short") == nullptr);
    CHECK(store.find("tickets")->generation == 2U);
}

TEST_CASE("Key store: refusals of a change", "[crypto][key-store]")
{
    TempStore     dir("refuse");
    TestProtector protector;
    KeyStore      store;
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);
    CHECK(store.generate("Bad", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::Malformed);
    CHECK(store.generate("", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::Malformed);
    CHECK(store.generate("hybrid", AlgorithmId::X25519Mlkem768, PlacementRequest::Software, 1U) ==
          CryptoError::Unsupported);
    CHECK(store.generate("none", AlgorithmId{}, PlacementRequest::Software, 1U) == CryptoError::Unsupported);
    CHECK(store.generate("device", AlgorithmId::EcdsaSecp256r1Sha256, PlacementRequest::HardwareRequired, 1U) ==
          CryptoError::Unsupported);
    REQUIRE(store.generate("dup", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    CHECK(store.generate("dup", AlgorithmId::MlDsa65, PlacementRequest::Software, 1U) == CryptoError::Exists);
    CHECK(store.revoke("dup", 2U, 1U) == CryptoError::NotFound);
    CHECK(store.revoke("dup", 0U, 1U) == CryptoError::NotFound);
    CHECK(store.revoke("absent", 1U, 1U) == CryptoError::NotFound);
    CHECK(store.destroy("absent") == CryptoError::NotFound);
    CHECK(store.size() == 1U);

    // Nothing was written by a refused change.
    KeyStore reopened;
    REQUIRE(reopened.open(dir.dir(), protector) == CryptoError::None);
    CHECK(same_infos(store, reopened));

    // A protector that fails leaves the store as it was.
    TempStore     other("refuse-protect");
    TestProtector failing;
    failing.fail_protect = CryptoError::Unavailable;
    KeyStore none;
    CHECK(none.create(other.dir(), failing) == CryptoError::Unavailable);
    CHECK_FALSE(none.ready());
    CHECK(none.open(other.dir(), failing) == CryptoError::NotFound);

    // A store exists once; another directory holds no store.
    KeyStore again;
    CHECK(again.create(dir.dir(), protector) == CryptoError::Exists);
    CHECK_FALSE(again.ready());
    TempStore empty("refuse-empty");
    CHECK(again.open(empty.dir(), protector) == CryptoError::NotFound);

    // A protector of another name, or one that will not recover the master key.
    TestProtector stranger(true, "stranger");
    CHECK(again.open(dir.dir(), stranger) == CryptoError::Unsupported);
    TestProtector forgetful; // the right name, no master keys
    CHECK(again.open(dir.dir(), forgetful) == CryptoError::Denied);
    protector.fail_recover = CryptoError::Unavailable;
    CHECK(again.open(dir.dir(), protector) == CryptoError::Unavailable);
    protector.fail_recover = CryptoError::None;
    TestProtector badly_named(true, "Not Valid");
    CHECK(again.create(other.dir(), badly_named) == CryptoError::Unsupported);
}

TEST_CASE("Key store: revocation erases the key and replaces the master key", "[crypto][key-store]")
{
    TempStore     dir("revoke");
    TestProtector protector;
    KeyStore      store;
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);
    REQUIRE(store.generate("identity", AlgorithmId::MlDsa65, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(store.generate("tickets", AlgorithmId::Aes256Gcm, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(store.rotate("identity", 2U) == CryptoError::None);
    const Bytes before_revocation = read_file(dir.file());
    const KeyId first_id          = store.find("identity", 1U)->id;
    const Bytes first_public      = public_of(store, "identity", 1U);
    CHECK(protector.held() == 1U);

    // Revoking the retired generation: its material is gone, its public key and identifier stay.
    REQUIRE(store.revoke("identity", 1U, 3U) == CryptoError::None);
    const KeyInfo* revoked = store.find("identity", 1U);
    REQUIRE(revoked != nullptr);
    CHECK(revoked->state == KeyState::Revoked);
    CHECK(revoked->changed == 3U);
    CHECK(revoked->id == first_id);
    CHECK(std::memcmp(public_of(store, "identity", 1U).data(), first_public.data(), first_public.size()) == 0);
    check_signs(store, "identity");
    CHECK(protector.protects.load() == 2U);
    CHECK(protector.held() == 1U); // the old master key forgotten

    // An earlier copy of the store file no longer opens: the master key it was sealed under is gone.
    const Bytes after_revocation = read_file(dir.file());
    write_file(dir.file(), before_revocation);
    KeyStore old_copy;
    CHECK(old_copy.open(dir.dir(), protector) == CryptoError::Denied);
    write_file(dir.file(), after_revocation);

    // Revoking again changes nothing and writes nothing.
    const usize protects = protector.protects.load();
    REQUIRE(store.revoke("identity", 1U, 9U) == CryptoError::None);
    CHECK(store.find("identity", 1U)->changed == 3U);
    CHECK(protector.protects.load() == protects);
    const Bytes unchanged = read_file(dir.file());
    CHECK(unchanged.size() == after_revocation.size());
    CHECK(std::memcmp(unchanged.data(), after_revocation.data(), unchanged.size()) == 0);

    // Revoking the Current generation leaves the name without one until it rotates.
    REQUIRE(store.revoke("identity", 2U, 4U) == CryptoError::None);
    StoredSigner signer;
    CHECK(store.signer("identity", signer) == CryptoError::Revoked);
    CHECK(store.find("identity") == nullptr);
    REQUIRE(store.rotate("identity", 5U) == CryptoError::None);
    CHECK(store.find("identity")->generation == 3U);
    check_signs(store, "identity");
    CHECK(store.find("identity", 2U)->state == KeyState::Revoked);

    // A revoked secret refuses to be read and zeroes the output.
    REQUIRE(store.revoke("tickets", 1U, 6U) == CryptoError::None);
    u8 out[32] = {7};
    CHECK(store.secret("tickets", 1U, Span<u8>(out)) == CryptoError::Revoked);
    CHECK(out[0] == 0U);
    AgreementKey none;
    CHECK(store.agreement_key("identity", 1U, none) == CryptoError::Unsupported);

    // Everything holds after reopening, and only the newest master key remains.
    KeyStore reopened;
    REQUIRE(reopened.open(dir.dir(), protector) == CryptoError::None);
    CHECK(same_infos(store, reopened));
    CHECK(protector.held() == 1U);

    // A protector that cannot forget replaces nothing: the master key stays, and DPAPI is one.
    TempStore     kept_dir("revoke-kept");
    TestProtector keeping(false);
    KeyStore      kept;
    REQUIRE(kept.create(kept_dir.dir(), keeping) == CryptoError::None);
    REQUIRE(kept.generate("k", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(kept.revoke("k", 1U, 2U) == CryptoError::None);
    CHECK(keeping.protects.load() == 1U);
    CHECK(keeping.forgets_called.load() == 0U);
}

TEST_CASE("Key store: destruction removes a name and replaces the master key", "[crypto][key-store]")
{
    TempStore     dir("destroy");
    TestProtector protector;
    KeyStore      store;
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);
    REQUIRE(store.generate("a", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(store.generate("b", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(store.generate("c", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(store.rotate("b", 2U) == CryptoError::None);
    REQUIRE(store.rotate("b", 3U) == CryptoError::None);
    const KeyId old_b = store.find("b")->id;
    REQUIRE(store.destroy("b") == CryptoError::None);
    REQUIRE(store.size() == 2U);
    CHECK(store.key(0U).name() == StringView("a"));
    CHECK(store.key(1U).name() == StringView("c"));
    CHECK(store.find("b", 1U) == nullptr);
    CHECK(protector.held() == 1U);
    CHECK(protector.protects.load() == 2U);

    // The name is free again, for a new key.
    REQUIRE(store.generate("b", AlgorithmId::Ed25519, PlacementRequest::Software, 4U) == CryptoError::None);
    CHECK(store.find("b")->generation == 1U);
    CHECK_FALSE(store.find("b")->id == old_b);
    KeyStore reopened;
    REQUIRE(reopened.open(dir.dir(), protector) == CryptoError::None);
    CHECK(same_infos(store, reopened));
}

TEST_CASE("Key store: an interrupted replacement of the master key completes with the next change",
          "[crypto][key-store]")
{
    TempStore     dir("pending");
    TestProtector protector;
    KeyStore      store;
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);
    REQUIRE(store.generate("a", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);

    // The old master key cannot be forgotten now: the revocation stands, its reference is kept pending.
    protector.fail_forget = CryptoError::Unavailable;
    REQUIRE(store.revoke("a", 1U, 2U) == CryptoError::None);
    CHECK(protector.held() == 2U);
    CHECK(store.find("a", 1U)->state == KeyState::Revoked);

    // A change while the protector still fails changes nothing.
    CHECK(store.generate("b", AlgorithmId::Ed25519, PlacementRequest::Software, 3U) == CryptoError::Unavailable);
    CHECK(store.find("b") == nullptr);

    // The next change forgets it first, also one that is itself refused.
    protector.fail_forget = CryptoError::None;
    CHECK(store.generate("a", AlgorithmId::Ed25519, PlacementRequest::Software, 4U) == CryptoError::Exists);
    CHECK(protector.held() == 1U);
    REQUIRE(store.generate("b", AlgorithmId::Ed25519, PlacementRequest::Software, 5U) == CryptoError::None);
    CHECK(protector.held() == 1U);

    KeyStore reopened;
    REQUIRE(reopened.open(dir.dir(), protector) == CryptoError::None);
    CHECK(same_infos(store, reopened));
}

TEST_CASE("Key store: every changed byte of a store is refused", "[crypto][key-store]")
{
    TempStore     dir("tamper");
    TestProtector protector;
    KeyStore      store;
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);
    REQUIRE(store.generate("identity", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(store.generate("tickets", AlgorithmId::Aes128Gcm, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(store.rotate("tickets", 2U) == CryptoError::None);
    const Bytes payload = payload_of(dir);

    // Each byte changed, in a store whose durable envelope is intact: the framing, the protector or the AEAD refuse
    // it, never with a store opened.
    usize damaged = 0U;
    usize forged  = 0U;
    for (usize at = 0U; at < payload.size(); ++at)
    {
        Bytes changed = payload;
        changed[at] ^= 0x01U;
        write_payload(dir, ConstSpan<u8>(changed.data(), changed.size()));
        KeyStore          opened;
        const CryptoError error = opened.open(dir.dir(), protector);
        CHECK(error != CryptoError::None);
        CHECK_FALSE(opened.ready());
        damaged += error == CryptoError::Damaged ? 1U : 0U;
        forged += error == CryptoError::Forged ? 1U : 0U;
    }
    CHECK(damaged > 0U);
    CHECK(forged > payload.size() / 2U);

    // Payloads cut inside the header are not a store; cut inside the sealed body, or extended, they do not
    // authenticate.
    for (const usize length : {usize{0U}, usize{8U}, usize{40U}})
    {
        write_payload(dir, ConstSpan<u8>(payload.data(), length));
        KeyStore opened;
        CHECK(opened.open(dir.dir(), protector) == CryptoError::Damaged);
    }
    for (const usize length : {payload.size() - 17U, payload.size() - 1U})
    {
        write_payload(dir, ConstSpan<u8>(payload.data(), length));
        KeyStore opened;
        CHECK(opened.open(dir.dir(), protector) == CryptoError::Forged);
    }
    Bytes longer = payload;
    longer.push_back(0U);
    write_payload(dir, ConstSpan<u8>(longer.data(), longer.size()));
    KeyStore extended;
    CHECK(extended.open(dir.dir(), protector) == CryptoError::Forged);

    // The file's own envelope damaged: the durable save's checks refuse it.
    write_payload(dir, ConstSpan<u8>(payload.data(), payload.size()));
    Bytes file = read_file(dir.file());
    file[file.size() - 3U] ^= 0x80U;
    write_file(dir.file(), file);
    KeyStore damaged_file;
    CHECK(damaged_file.open(dir.dir(), protector) == CryptoError::Damaged);

    // Intact again, it opens.
    write_payload(dir, ConstSpan<u8>(payload.data(), payload.size()));
    KeyStore intact;
    CHECK(intact.open(dir.dir(), protector) == CryptoError::None);
}

TEST_CASE("Key store: an older or substituted store is refused by an open one", "[crypto][key-store]")
{
    TempStore     dir("rollback");
    TestProtector protector(false); // nothing forgotten: only the sequence notices an older save
    KeyStore      store;
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);
    REQUIRE(store.generate("a", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    const Bytes older = read_file(dir.file());
    REQUIRE(store.revoke("a", 1U, 2U) == CryptoError::None);

    // The older save put back: an open store refuses to go back to it, and changes nothing.
    write_file(dir.file(), older);
    CHECK(store.refresh() == CryptoError::Replayed);
    CHECK(store.find("a", 1U)->state == KeyState::Revoked);
    CHECK(store.generate("b", AlgorithmId::Ed25519, PlacementRequest::Software, 3U) == CryptoError::Replayed);
    CHECK(store.find("b") == nullptr);

    // Another store put in its place.
    TempStore other("rollback-other");
    KeyStore  stranger;
    REQUIRE(stranger.create(other.dir(), protector) == CryptoError::None);
    write_file(dir.file(), read_file(other.file()));
    CHECK(store.refresh() == CryptoError::Forged);
    CHECK(store.rotate("a", 4U) == CryptoError::Forged);

    // Removed: NotFound.
    REQUIRE(fs::remove_file(dir.file()));
    CHECK(store.refresh() == CryptoError::NotFound);
}

TEST_CASE("Key store: writers in several threads and stores are ordered by the lock", "[crypto][key-store]")
{
    TempStore     dir("writers");
    TestProtector protector;
    KeyStore      first;
    REQUIRE(first.create(dir.dir(), protector) == CryptoError::None);

    // Two store objects on one directory: each change applies to the newest store on disk.
    KeyStore second;
    REQUIRE(second.open(dir.dir(), protector) == CryptoError::None);
    REQUIRE(first.generate("from-first", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(second.generate("from-second", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::None);
    CHECK(second.size() == 2U);
    CHECK(first.size() == 1U);
    REQUIRE(first.refresh() == CryptoError::None);
    CHECK(same_infos(first, second));
    CHECK(second.generate("from-first", AlgorithmId::Ed25519, PlacementRequest::Software, 1U) == CryptoError::Exists);

    // Threads, each with its own store object, adding and rotating their own names: nothing is lost.
    constexpr usize threads = 4U;
    constexpr usize each    = 6U;
    std::atomic<usize> failures{0U};
    std::thread        writers[threads];
    for (usize t = 0U; t < threads; ++t)
    {
        writers[t] = std::thread(
            [&, t]()
            {
                KeyStore own;
                if (own.open(dir.dir(), protector) != CryptoError::None)
                {
                    failures.fetch_add(1U);
                    return;
                }
                for (usize i = 0U; i < each; ++i)
                {
                    const String name = writer_name(t, i);
                    const StringView view(name.data(), name.size());
                    if (own.generate(view, AlgorithmId::Ed25519, PlacementRequest::Software, i) != CryptoError::None ||
                        own.rotate(view, i + 1U) != CryptoError::None)
                    {
                        failures.fetch_add(1U);
                    }
                }
            });
    }
    for (std::thread& writer : writers)
    {
        writer.join();
    }
    CHECK(failures.load() == 0U);
    REQUIRE(first.refresh() == CryptoError::None);
    CHECK(first.size() == 2U + threads * each * 2U);
    for (usize t = 0U; t < threads; ++t)
    {
        for (usize i = 0U; i < each; ++i)
        {
            const String name = writer_name(t, i);
            const KeyInfo* info = first.find(StringView(name.data(), name.size()));
            REQUIRE(info != nullptr);
            CHECK(info->generation == 2U);
        }
    }
}

TEST_CASE("Key store: the operating system's protector", "[crypto][key-store]")
{
    PlatformKeyProtector platform;
    CHECK(platform.protection() == KeyProtection::Platform);
    CHECK(platform.prompt_milliseconds() == 0U);
    CHECK(PlatformKeyProtector(5000U).prompt_milliseconds() == 5000U);
    const u8 store_id[kKeyStoreIdBytes]   = {1, 2, 3};
    u8       master[kKeyStoreMasterBytes] = {};
    for (usize i = 0U; i < kKeyStoreMasterBytes; ++i)
    {
        master[i] = static_cast<u8>(0x40U + i);
    }
#if CRD_OS_WINDOWS
    const u8 other_id[kKeyStoreIdBytes] = {9, 9, 9};
    REQUIRE(PlatformKeyProtector::available());
    CHECK(platform.name() == StringView("dpapi"));
    CHECK_FALSE(platform.forgets());
    Bytes reference;
    REQUIRE(platform.protect(ConstSpan<u8>(store_id), ConstSpan<u8>(master), reference) == CryptoError::None);
    CHECK_FALSE(reference.empty());
    u8 recovered[kKeyStoreMasterBytes] = {};
    REQUIRE(platform.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size()),
                             Span<u8>(recovered)) == CryptoError::None);
    CHECK(std::memcmp(recovered, master, sizeof(master)) == 0);

    // Bound to its store, and to every byte of it.
    std::memset(recovered, 0x55, sizeof(recovered));
    CHECK(platform.recover(ConstSpan<u8>(other_id), ConstSpan<u8>(reference.data(), reference.size()),
                           Span<u8>(recovered)) == CryptoError::Denied);
    CHECK(recovered[0] == 0U);
    Bytes changed = reference;
    changed[changed.size() / 2U] ^= 0x10U;
    CHECK(platform.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(changed.data(), changed.size()),
                           Span<u8>(recovered)) == CryptoError::Denied);
    CHECK(platform.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size()),
                           Span<u8>(recovered, 16U)) == CryptoError::BadLength);
    CHECK(platform.protect(ConstSpan<u8>(store_id, 15U), ConstSpan<u8>(master), reference) == CryptoError::BadLength);
    CHECK(reference.empty());
    CHECK(platform.forget(ConstSpan<u8>(store_id), {}) == CryptoError::None);

    // Allowing prompts changes nothing DPAPI asks: the same reference recovers.
    PlatformKeyProtector prompting(1000U);
    REQUIRE(prompting.protect(ConstSpan<u8>(store_id), ConstSpan<u8>(master), reference) == CryptoError::None);
    REQUIRE(platform.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size()),
                             Span<u8>(recovered)) == CryptoError::None);
    CHECK(std::memcmp(recovered, master, sizeof(master)) == 0);

    // A store under DPAPI: created, reopened, used.
    TempStore dir("dpapi");
    KeyStore  store;
    REQUIRE(store.create(dir.dir(), platform) == CryptoError::None);
    CHECK(store.protection() == KeyProtection::Platform);
    REQUIRE(store.generate("identity", AlgorithmId::MlDsa65, PlacementRequest::Software, 1U) == CryptoError::None);
    KeyStore reopened;
    REQUIRE(reopened.open(dir.dir(), platform) == CryptoError::None);
    CHECK(same_infos(store, reopened));
    check_signs(reopened, "identity");
    TestProtector stranger(true, "test");
    CHECK(reopened.open(dir.dir(), stranger) == CryptoError::Unsupported);
#else
    // A well-formed Secret Service reference (its format byte, then a handle), so only the vault can answer.
    Bytes reference;
    reference.resize(1U + kKeyStoreIdBytes);
    std::memset(reference.data(), 0x11, reference.size());
    reference[0] = 1U;
#if CRD_OS_LINUX
    // Without a session bus the Secret Service cannot be reached: every call is Unavailable. The vault itself is
    // proven under a pinned gnome-keyring ([secret-service], the crd-crypto-secret-service CTests).
    CHECK(platform.name() == StringView("secret-service"));
    CHECK(platform.forgets());
    String bus;
    const bool had_bus = crd::read_environment_text("DBUS_SESSION_BUS_ADDRESS", bus);
    REQUIRE(::setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/cerid-no-bus", 1) == 0);
#else
    CHECK(platform.name() == StringView("unavailable"));
    CHECK_FALSE(platform.forgets());
#endif
    CHECK_FALSE(PlatformKeyProtector::available());
    Bytes written;
    written.push_back(1U);
    CHECK(platform.protect(ConstSpan<u8>(store_id), ConstSpan<u8>(master), written) == CryptoError::Unavailable);
    CHECK(written.empty());
    CHECK(platform.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size()),
                           Span<u8>(master)) == CryptoError::Unavailable);
    CHECK(master[0] == 0U);
    CHECK(platform.forget(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size())) ==
          CryptoError::Unavailable);
    TempStore dir("unavailable");
    KeyStore  store;
    CHECK(store.create(dir.dir(), platform) == CryptoError::Unavailable);
    CHECK_FALSE(store.ready());
#if CRD_OS_LINUX
    if (had_bus)
    {
        REQUIRE(::setenv("DBUS_SESSION_BUS_ADDRESS", bus.c_str(), 1) == 0);
    }
    else
    {
        REQUIRE(::unsetenv("DBUS_SESSION_BUS_ADDRESS") == 0);
    }
#endif
#endif
}

// ---- The Secret Service under a pinned gnome-keyring (tests/foundation/crypto/secret_service_session.py) ----
//
// Hidden from the default run: the crd-crypto-secret-service CTests run them inside a session bus of their own with a
// gnome-keyring daemon whose login collection the session made, unlocked ([secret-service]) or seeded and then locked
// ([secret-service-locked]).

#if CRD_OS_LINUX
// The session's scratch directory, which the locked session shares between its two runs.
[[nodiscard]] fs::Path session_scratch()
{
    String scratch;
    REQUIRE(crd::read_environment_text("CRD_SECRET_SERVICE_SCRATCH", scratch));
    return fs::Path(StringView(scratch.data(), scratch.size()));
}

constexpr u8 kSeededStore[kKeyStoreIdBytes]      = {0xC0, 0xFF, 0xEE};
constexpr u8 kSeededMaster[kKeyStoreMasterBytes] = {0x5E, 0xED};
#endif

TEST_CASE("Key store: the Secret Service keeps, recovers and forgets a master key", "[.secret-service]")
{
#if CRD_OS_LINUX
    REQUIRE(PlatformKeyProtector::available());
    PlatformKeyProtector vault;
    CHECK(vault.name() == StringView("secret-service"));
    CHECK(vault.forgets());
    const u8 store_id[kKeyStoreIdBytes]   = {1, 2, 3};
    const u8 other_id[kKeyStoreIdBytes]   = {9, 9, 9};
    u8       master[kKeyStoreMasterBytes] = {};
    for (usize i = 0U; i < kKeyStoreMasterBytes; ++i)
    {
        master[i] = static_cast<u8>(0x40U + i);
    }
    Bytes reference;
    REQUIRE(vault.protect(ConstSpan<u8>(store_id), ConstSpan<u8>(master), reference) == CryptoError::None);
    REQUIRE(reference.size() == 1U + kKeyStoreIdBytes);
    CHECK(reference[0] == 1U);
    u8 recovered[kKeyStoreMasterBytes] = {};
    REQUIRE(vault.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size()),
                          Span<u8>(recovered)) == CryptoError::None);
    CHECK(std::memcmp(recovered, master, sizeof(master)) == 0);

    // Bound to its store and its handle; a reference of another form is none of this protector's.
    const auto denied = [&](ConstSpan<u8> id, const Bytes& changed)
    {
        std::memset(recovered, 0x55, sizeof(recovered));
        const CryptoError result =
            vault.recover(id, ConstSpan<u8>(changed.data(), changed.size()), Span<u8>(recovered));
        return result == CryptoError::Denied && recovered[0] == 0U && recovered[31] == 0U;
    };
    CHECK(denied(ConstSpan<u8>(other_id), reference));
    for (const usize at : {usize{0}, usize{1}, usize{16}})
    {
        Bytes changed = reference;
        changed[at] ^= 0x01U;
        CHECK(denied(ConstSpan<u8>(store_id), changed));
    }
    Bytes shorter = reference;
    shorter.pop_back();
    CHECK(denied(ConstSpan<u8>(store_id), shorter));
    CHECK(vault.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size()),
                        Span<u8>(recovered, 16U)) == CryptoError::BadLength);
    Bytes refused;
    CHECK(vault.protect(ConstSpan<u8>(store_id, 15U), ConstSpan<u8>(master), refused) == CryptoError::BadLength);
    CHECK(vault.protect(ConstSpan<u8>(store_id), ConstSpan<u8>(master, 31U), refused) == CryptoError::BadLength);
    CHECK(refused.empty());
    CHECK(vault.forget(ConstSpan<u8>(store_id), shorter) == CryptoError::Denied);

    // Forgetting deletes the item: the reference recovers nothing after it, and forgetting again finds nothing to
    // destroy.
    REQUIRE(vault.forget(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size())) ==
            CryptoError::None);
    CHECK(denied(ConstSpan<u8>(store_id), reference));
    CHECK(vault.forget(ConstSpan<u8>(store_id), ConstSpan<u8>(reference.data(), reference.size())) ==
          CryptoError::None);

    // Every call is its own transport session: 256 of them, so a shared value with a leading zero byte (once in 256)
    // is all but certainly among them and the two sides' left padding agrees.
    for (usize round = 0U; round < 256U; ++round)
    {
        REQUIRE(random_bytes(Span<u8>(master)) == CryptoError::None);
        Bytes kept;
        REQUIRE(vault.protect(ConstSpan<u8>(store_id), ConstSpan<u8>(master), kept) == CryptoError::None);
        REQUIRE(vault.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(kept.data(), kept.size()), Span<u8>(recovered)) ==
                CryptoError::None);
        REQUIRE(std::memcmp(recovered, master, sizeof(master)) == 0);
        REQUIRE(vault.forget(ConstSpan<u8>(store_id), ConstSpan<u8>(kept.data(), kept.size())) == CryptoError::None);
    }

    // Four threads at once, each call on its own connection.
    std::atomic<u32> failures{0U};
    std::thread      threads[4];
    for (u32 t = 0U; t < 4U; ++t)
    {
        threads[t] = std::thread(
            [&, t]
            {
                for (u32 i = 0U; i < 16U; ++i)
                {
                    u8 mine[kKeyStoreMasterBytes];
                    std::memset(mine, static_cast<int>(t * 16U + i), sizeof(mine));
                    u8    back[kKeyStoreMasterBytes] = {};
                    Bytes kept;
                    const bool ok =
                        vault.protect(ConstSpan<u8>(store_id), ConstSpan<u8>(mine), kept) == CryptoError::None &&
                        vault.recover(ConstSpan<u8>(store_id), ConstSpan<u8>(kept.data(), kept.size()),
                                      Span<u8>(back)) == CryptoError::None &&
                        std::memcmp(back, mine, sizeof(mine)) == 0 &&
                        vault.forget(ConstSpan<u8>(store_id), ConstSpan<u8>(kept.data(), kept.size())) ==
                            CryptoError::None;
                    if (!ok)
                    {
                        failures.fetch_add(1U);
                    }
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(failures.load() == 0U);

    // A store under the Secret Service: created, reopened and used; a revocation replaces its master key and the
    // vault forgets the old one, so an earlier copy of the store file no longer opens (decision K5).
    TempStore dir("secret-service");
    KeyStore  store;
    REQUIRE(store.create(dir.dir(), vault) == CryptoError::None);
    CHECK(store.protection() == KeyProtection::Platform);
    REQUIRE(store.generate("identity", AlgorithmId::MlDsa65, PlacementRequest::Software, 1U) == CryptoError::None);
    REQUIRE(store.rotate("identity", 2U) == CryptoError::None);
    KeyStore reopened;
    REQUIRE(reopened.open(dir.dir(), vault) == CryptoError::None);
    CHECK(same_infos(store, reopened));
    check_signs(reopened, "identity");
    const Bytes before_revocation = read_file(dir.file());
    REQUIRE(store.revoke("identity", 1U, 3U) == CryptoError::None);
    const Bytes after_revocation = read_file(dir.file());
    write_file(dir.file(), before_revocation);
    KeyStore old_copy;
    CHECK(old_copy.open(dir.dir(), vault) == CryptoError::Denied);
    write_file(dir.file(), after_revocation);
    KeyStore current;
    REQUIRE(current.open(dir.dir(), vault) == CryptoError::None);
    check_signs(current, "identity");
    TestProtector stranger(true, "test");
    CHECK(current.open(dir.dir(), stranger) == CryptoError::Unsupported);
#else
    SUCCEED("the Secret Service is Linux's protector");
#endif
}

TEST_CASE("Key store: a master key seeded for the locked Secret Service", "[.secret-service-seed]")
{
#if CRD_OS_LINUX
    PlatformKeyProtector vault;
    Bytes                reference;
    REQUIRE(vault.protect(ConstSpan<u8>(kSeededStore), ConstSpan<u8>(kSeededMaster), reference) == CryptoError::None);
    write_file(session_scratch() / "reference", reference);
#else
    SUCCEED("the Secret Service is Linux's protector");
#endif
}

TEST_CASE("Key store: a locked Secret Service is Denied, and prompts only where allowed", "[.secret-service-locked]")
{
#if CRD_OS_LINUX
    REQUIRE(PlatformKeyProtector::available());
    const Bytes reference = read_file(session_scratch() / "reference");
    const ConstSpan<u8> held(reference.data(), reference.size());
    u8 recovered[kKeyStoreMasterBytes];
    std::memset(recovered, 0x55, sizeof(recovered));

    // Never prompting: the locked item, and the locked default collection a new key would go into, are refused.
    PlatformKeyProtector never;
    CHECK(never.recover(ConstSpan<u8>(kSeededStore), held, Span<u8>(recovered)) == CryptoError::Denied);
    CHECK(recovered[0] == 0U);
    CHECK(never.forget(ConstSpan<u8>(kSeededStore), held) == CryptoError::Denied);
    Bytes written;
    CHECK(never.protect(ConstSpan<u8>(kSeededStore), ConstSpan<u8>(kSeededMaster), written) == CryptoError::Denied);
    CHECK(written.empty());

    // Prompting allowed: the service's prompt runs, and with no prompter to answer it, it is dismissed (or the bound
    // passes) and the call is Denied within its bound.
    PlatformKeyProtector prompting(10000U);
    const auto           started = std::chrono::steady_clock::now();
    CHECK(prompting.recover(ConstSpan<u8>(kSeededStore), held, Span<u8>(recovered)) == CryptoError::Denied);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(20));
    CHECK(recovered[0] == 0U);
#else
    SUCCEED("the Secret Service is Linux's protector");
#endif
}

TEST_CASE("Key store: a TLS credential from a stored signer", "[crypto][key-store]")
{
    TempStore     dir("tls");
    TestProtector protector;
    KeyStore      store;
    REQUIRE(store.create(dir.dir(), protector) == CryptoError::None);
    REQUIRE(store.generate("server", AlgorithmId::EcdsaSecp256r1Sha256, PlacementRequest::Software, 1U) ==
            CryptoError::None);
    StoredSigner signer;
    REQUIRE(store.signer("server", signer) == CryptoError::None);
    TlsCredential credential;
    REQUIRE(credential.init(signer, "server.example") == TlsError::None);
    CHECK(credential.ready());
    CHECK(credential.algorithm() == AlgorithmId::EcdsaSecp256r1Sha256);
    CHECK(credential.key_id() == store.find("server")->id);

    // The signer may be cleared once the credential holds the key; an empty signer makes no credential.
    signer.clear();
    CHECK_FALSE(signer.ready());
    CHECK(credential.ready());
    TlsCredential none;
    CHECK(none.init(signer, "server.example") == TlsError::NotReady);
}
