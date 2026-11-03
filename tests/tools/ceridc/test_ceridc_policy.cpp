// NET.2: `ceridc policy` renders the committed source of crypto policy generation 1 (assets/crypto-policy), joins
// signatures made by real provider keys and judges the result as an installation would; the key holders' keys are
// made in and sign from crd-crypto's key store; the verb and the real binary give the same bytes, and every malformed
// request, source, signed file and trust set is refused before any file is written. Contract:
// docs/design/network-foundation.md#policy-issue.

#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/environment.hpp>
#include <crd/core/platform.hpp>
#include <crd/crypto/key_store.hpp>
#include <crd/crypto/policy.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace
{
namespace fs = crd::platform::fs;
namespace cont = crd::containers;
namespace crypto = crd::crypto;
using crd::u64;
using crd::u8;
using crd::usize;
using crypto::AlgorithmId;
using crypto::PolicyError;
using crypto::PolicyScope;

constexpr const char* kSource = CRD_REPO_DIR "/assets/crypto-policy/generation-1.policy.toml";
constexpr u64 kServicesRefusal = 1793491200U;   // 2026-11-01: classical-only key exchange, Cerid services
constexpr u64 kEverywhereRefusal = 1893456000U; // 2030-01-01: classical-only key exchange, every session
constexpr u64 kSignatureRefusal = 2082758400U;  // 2036-01-01: traditional signatures

#if CRD_OS_WINDOWS
constexpr const char* kShellQuote = "\"";
#else
constexpr const char* kShellQuote = "";
#endif

bool has(const cont::String& text, cont::StringView needle)
{
    return cont::StringView(text).find(needle) != cont::StringView::npos;
}

cont::String body(const char* source, const char* out)
{
    crd::ceridc::PolicyFlags flags;
    flags.action = "body";
    flags.source = source;
    flags.out = out;
    return crd::ceridc::verb_policy(flags, crd::memory::default_allocator());
}

cont::String assemble(std::initializer_list<const char*> in, const char* out)
{
    crd::ceridc::PolicyFlags flags;
    flags.action = "assemble";
    flags.in = cont::ConstSpan<const char*>(in.begin(), in.size());
    flags.out = out;
    return crd::ceridc::verb_policy(flags, crd::memory::default_allocator());
}

cont::String verify(const char* generation, const char* trust, const char* now, const char* floor = nullptr,
                    const char* source = nullptr)
{
    crd::ceridc::PolicyFlags flags;
    flags.action = "verify";
    flags.generation = generation;
    flags.trust = trust;
    flags.now = now;
    flags.floor = floor;
    flags.source = source;
    return crd::ceridc::verb_policy(flags, crd::memory::default_allocator());
}

cont::Array<u8> read_bytes(const char* path)
{
    cont::Array<u8> bytes;
    REQUIRE(fs::read_file_binary(fs::Path(cont::StringView(path)), bytes));
    return bytes;
}

void write_bytes(const char* path, cont::ConstSpan<u8> bytes)
{
    REQUIRE(fs::write_file_binary(fs::Path(cont::StringView(path)), bytes));
}

void write_text(const char* path, cont::StringView text)
{
    REQUIRE(fs::write_file_text(fs::Path(path), text));
}

cont::String read_text(const char* path)
{
    cont::String text;
    REQUIRE(fs::read_file_text(fs::Path(path), text));
    return text;
}

// One trust key of the issue: a fresh provider key and its public key.
struct IssueKey
{
    crypto::SigningKey key;
    u8 public_key[4096] = {};
    usize public_size = 0U;

    explicit IssueKey(AlgorithmId algorithm)
    {
        REQUIRE(key.generate(algorithm) == crypto::CryptoError::None);
        public_size = crypto::signature_sizes(algorithm).public_key;
        REQUIRE(key.public_key(cont::Span<u8>(public_key, public_size)) == crypto::CryptoError::None);
    }

    [[nodiscard]] cont::ConstSpan<u8> public_view() const { return cont::ConstSpan<u8>(public_key, public_size); }

    // This key's signature of the body at `body_path`, as a one-signature generation at `out`.
    void sign(const char* body_path, const char* out) const
    {
        const cont::Array<u8> signed_body = read_bytes(body_path);
        const cont::ConstSpan<u8> message(signed_body.data(), signed_body.size());
        u8 signature[8192];
        usize written = 0U;
        REQUIRE(key.sign(message, signature, written) == crypto::CryptoError::None);
        const crypto::KeySignature entry{crypto::key_id_of(public_view()), key.algorithm(),
                                         cont::ConstSpan<u8>(signature, written)};
        cont::Array<u8> generation;
        generation.resize(crypto::kMaxPolicyBytes);
        usize size = 0U;
        REQUIRE(crypto::write_policy(message, cont::ConstSpan<crypto::KeySignature>(&entry, 1U),
                                     cont::Span<u8>(generation.data(), generation.size()), size) == PolicyError::None);
        write_bytes(out, cont::ConstSpan<u8>(generation.data(), size));
    }
};

void write_trust(const char* path, std::initializer_list<const IssueKey*> keys)
{
    constexpr char digits[] = "0123456789abcdef";
    cont::String text;
    for (const IssueKey* key : keys)
    {
        text += "[[key]]\nalgorithm = \"";
        text += crypto::algorithm_name(key->key.algorithm());
        text += "\"\npublic_key = \"";
        for (const u8 byte : key->public_view())
        {
            text.push_back(digits[byte >> 4U]);
            text.push_back(digits[byte & 0x0FU]);
        }
        text += "\"\n\n";
    }
    write_text(path, text);
}

// The committed source with `from` replaced by `to`, at `path`.
void write_variant(const char* path, cont::StringView from, cont::StringView to)
{
    cont::String text;
    REQUIRE(fs::read_file_text(fs::Path(kSource), text));
    const usize at = cont::StringView(text).find(from);
    REQUIRE(at != cont::StringView::npos);
    cont::String changed(cont::StringView(text).substr(0U, at));
    changed += to;
    changed += cont::StringView(text).substr(at + from.size());
    write_text(path, changed);
}

// The rule generation 1 holds for `algorithm` in `scope`; null when it has none.
const crypto::AlgorithmRule* rule_of(const crypto::PolicyContent& content, AlgorithmId algorithm, PolicyScope scope)
{
    for (crd::u32 i = 0U; i < content.rule_count; ++i)
    {
        if (content.rules[i].algorithm == algorithm && content.rules[i].scope == scope)
        {
            return &content.rules[i];
        }
    }
    return nullptr;
}

// The issue rehearsed with fresh keys: ML-DSA-65 and ECDSA P-256 each sign the rendered body, the two signed files
// are joined at "g1.policy" and the trust set naming both is "trust.toml".
struct Issue
{
    IssueKey post_quantum{AlgorithmId::MlDsa65};
    IssueKey classical{AlgorithmId::EcdsaSecp256r1Sha256};

    Issue()
    {
        REQUIRE(has(body(kSource, "g1.body"), R"("ok":true)"));
        post_quantum.sign("g1.body", "pq.signed");
        classical.sign("g1.body", "classical.signed");
        const cont::String joined = assemble({"pq.signed", "classical.signed"}, "g1.policy");
        INFO(joined.c_str());
        REQUIRE(has(joined, R"("ok":true)"));
        write_trust("trust.toml", {&post_quantum, &classical});
    }
};

// ---- The key holders' stored keys ----

// The tests' protector: each store's master key in memory, behind a one-byte reference bound to the store.
class MemoryProtector final : public crypto::KeyProtector
{
public:
    explicit MemoryProtector(cont::StringView name = "memory") : m_name(name) {}

    [[nodiscard]] cont::StringView name() const noexcept override { return m_name; }
    [[nodiscard]] crypto::KeyProtection protection() const noexcept override
    {
        return crypto::KeyProtection::Application;
    }
    [[nodiscard]] bool forgets() const noexcept override { return false; }

    [[nodiscard]] crypto::CryptoError protect(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> master,
                                              cont::Array<u8>& reference) noexcept override
    {
        if (m_count == kHeld)
        {
            return crypto::CryptoError::ProviderFailure;
        }
        Held& held = m_held[m_count];
        std::memcpy(held.store, store_id.data(), crypto::kKeyStoreIdBytes);
        std::memcpy(held.master, master.data(), crypto::kKeyStoreMasterBytes);
        reference.clear();
        reference.push_back(static_cast<u8>(m_count++));
        return crypto::CryptoError::None;
    }

    [[nodiscard]] crypto::CryptoError recover(cont::ConstSpan<u8> store_id, cont::ConstSpan<u8> reference,
                                              cont::Span<u8> master) noexcept override
    {
        if (reference.size() != 1U || reference[0] >= m_count ||
            std::memcmp(m_held[reference[0]].store, store_id.data(), crypto::kKeyStoreIdBytes) != 0)
        {
            crypto::secure_zero(master);
            return crypto::CryptoError::Denied;
        }
        std::memcpy(master.data(), m_held[reference[0]].master, crypto::kKeyStoreMasterBytes);
        return crypto::CryptoError::None;
    }

    [[nodiscard]] crypto::CryptoError forget(cont::ConstSpan<u8>, cont::ConstSpan<u8>) noexcept override
    {
        return crypto::CryptoError::None;
    }

private:
    static constexpr usize kHeld = 32U;

    struct Held
    {
        u8 store[crypto::kKeyStoreIdBytes] = {};
        u8 master[crypto::kKeyStoreMasterBytes] = {};
    };

    cont::StringView m_name;
    Held m_held[kHeld];
    usize m_count = 0U;
};

cont::String key(crypto::KeyProtector* protector, const char* store, const char* name, const char* algorithm,
                 const char* placement = nullptr)
{
    crd::ceridc::PolicyFlags flags;
    flags.action = "key";
    flags.store = store;
    flags.name = name;
    flags.algorithm = algorithm;
    flags.placement = placement;
    flags.protector = protector;
    return crd::ceridc::verb_policy(flags, crd::memory::default_allocator());
}

cont::String sign(crypto::KeyProtector* protector, const char* store, const char* name, const char* source,
                  const char* signed_body, const char* out)
{
    crd::ceridc::PolicyFlags flags;
    flags.action = "sign";
    flags.store = store;
    flags.name = name;
    flags.source = source;
    flags.body = signed_body;
    flags.out = out;
    flags.protector = protector;
    return crd::ceridc::verb_policy(flags, crd::memory::default_allocator());
}

// The text of the first "`field`":"..." of a report.
cont::String json_text(const cont::String& report, cont::StringView field)
{
    cont::String needle("\"");
    needle += field;
    needle += "\":\"";
    const cont::StringView view(report);
    const usize at = view.find(cont::StringView(needle));
    REQUIRE(at != cont::StringView::npos);
    const usize begin = at + needle.size();
    const usize end = view.find('"', begin);
    REQUIRE(end != cont::StringView::npos);
    return cont::String(view.substr(begin, end - begin));
}

// A trust set naming the keys the `key` reports describe, as their trust entries say.
void write_trust_of(const char* path, std::initializer_list<const cont::String*> reports)
{
    cont::String text;
    for (const cont::String* report : reports)
    {
        text += "[[key]]\nalgorithm = \"";
        text += json_text(*report, "algorithm");
        text += "\"\npublic_key = \"";
        text += json_text(*report, "public_key");
        text += "\"\n\n";
    }
    write_text(path, text);
}

void remove_tree(const char* path)
{
    (void)fs::remove_all(fs::Path(cont::StringView(path)));
    REQUIRE_FALSE(fs::exists(fs::Path(cont::StringView(path))));
}
} // namespace

TEST_CASE("policy body: generation 1 refuses classical-only key exchange for Cerid services, then everywhere from "
          "2030",
          "[ceridc][policy][net-2]")
{
    const cont::String out = body(kSource, "g1.body");
    INFO(out.c_str());
    CHECK(has(out, R"("verb":"policy","ok":true,"action":"body","out":"g1.body")"));
    CHECK(has(out, R"("generation":1,"issued":"2026-11-03T00:00:00Z","expires":"2027-11-03T00:00:00Z")"));
    CHECK(has(out, R"("offline_grace_seconds":7776000,"minimums":[])"));
    CHECK(has(out, R"({"algorithm":"x25519","scope":"cerid-services","accepted_from":"1970-01-01T00:00:00Z",)"
                   R"("preferred_from":"1970-01-01T00:00:00Z","preferred_until":"1970-01-01T00:00:00Z",)"
                   R"("refused_from":"2026-11-01T00:00:00Z"})"));
    CHECK(has(out, R"({"algorithm":"x25519","scope":"everywhere","accepted_from":"1970-01-01T00:00:00Z",)"
                   R"("preferred_from":"1970-01-01T00:00:00Z","preferred_until":"1970-01-01T00:00:00Z",)"
                   R"("refused_from":"2030-01-01T00:00:00Z"})"));
    CHECK(has(out, R"({"algorithm":"x25519-mlkem768","scope":"everywhere","accepted_from":"1970-01-01T00:00:00Z",)"
                   R"("preferred_from":"1970-01-01T00:00:00Z","preferred_until":"never","refused_from":"never"})"));

    // Signed and decoded, the bytes hold a rule for every algorithm this build implements (an algorithm without one
    // is refused, decision 4) and the dated refusals exactly.
    const Issue issue;
    const cont::Array<u8> bytes = read_bytes("g1.policy");
    crypto::PolicyMessage message;
    REQUIRE(crypto::decode_policy(cont::ConstSpan<u8>(bytes.data(), bytes.size()), message) == PolicyError::None);
    const crypto::PolicyContent& content = message.content;
    CHECK(content.rule_count == 23U);
    for (const AlgorithmId algorithm : crypto::provider_algorithms())
    {
        INFO(crypto::algorithm_name(algorithm));
        CHECK(rule_of(content, algorithm, PolicyScope::Everywhere) != nullptr);
    }
    for (const AlgorithmId algorithm : crypto::kAlgorithmIds)
    {
        INFO(crypto::algorithm_name(algorithm));
        const crypto::AlgorithmRule* everywhere = rule_of(content, algorithm, PolicyScope::Everywhere);
        const crypto::AlgorithmRule* services = rule_of(content, algorithm, PolicyScope::CeridServices);
        REQUIRE(everywhere != nullptr);
        switch (crypto::algorithm_class(algorithm))
        {
            case crypto::AlgorithmClass::ClassicalKeyExchange:
                CHECK(everywhere->refused_from == kEverywhereRefusal);
                REQUIRE(services != nullptr);
                CHECK(services->refused_from == kServicesRefusal);
                CHECK(services->accepted_from < services->refused_from);
                break;
            case crypto::AlgorithmClass::ClassicalSignature:
                CHECK(everywhere->refused_from == kSignatureRefusal);
                CHECK(services == nullptr);
                break;
            default:
                CHECK(everywhere->refused_from == crypto::kNever);
                CHECK(services == nullptr);
                break;
        }
    }
    CHECK(content.expires - content.issued <= crypto::kMaxPolicyLifetime);
    CHECK(content.offline_grace == crypto::kMaxOfflineGrace);
}

TEST_CASE("policy verify: the hybrid trust set's two signatures, judged as an installation would",
          "[ceridc][policy][net-2]")
{
    const Issue issue;
    const cont::String good = verify("g1.policy", "trust.toml", "2026-11-10", nullptr, kSource);
    INFO(good.c_str());
    CHECK(has(good, R"("verb":"policy","ok":true,"action":"verify","error":"none","now":"2026-11-10T00:00:00Z",)"
                    R"("floor":1,"freshness":"current")"));
    CHECK(has(good, R"("algorithm":"x25519","scope":"cerid-services",)"));
    CHECK(has(good, R"("refused_from":"2026-11-01T00:00:00Z","in_build":true,"status":"refused"})"));
    CHECK(has(good, R"("refused_from":"2030-01-01T00:00:00Z","in_build":true,"status":"accepted"})"));
    CHECK(has(good, R"("preferred_until":"never","refused_from":"never","in_build":true,"status":"preferred"})"));
    CHECK(has(good, R"("algorithm":"ml-dsa-65","bytes":)"));
    CHECK(has(good, R"("algorithm":"ecdsa-secp256r1-sha256","bytes":)"));
    CHECK_FALSE(has(good, R"("trusted":false)"));

    // Unix seconds read as a day does; the offline grace period keeps it usable after expiry, then nothing is.
    CHECK(has(verify("g1.policy", "trust.toml", "1794268800"), R"("now":"2026-11-10T00:00:00Z")"));
    CHECK(has(verify("g1.policy", "trust.toml", "2027-12-01"), R"("ok":true,"action":"verify","error":"none")"));
    CHECK(has(verify("g1.policy", "trust.toml", "2027-12-01"), R"("freshness":"grace")"));
    CHECK(has(verify("g1.policy", "trust.toml", "2028-03-01"), R"("ok":false,"action":"verify","error":"expired")"));
    CHECK(has(verify("g1.policy", "trust.toml", "2026-11-02"), R"("error":"not-yet-issued")"));
    CHECK(has(verify("g1.policy", "trust.toml", "2026-11-10", "2"), R"("error":"rollback","now")"));

    // One signature of the hybrid pair is not enough, and a changed byte breaks the one it falls in.
    CHECK(has(verify("pq.signed", "trust.toml", "2026-11-10"), R"("error":"missing-signature")"));
    cont::Array<u8> forged = read_bytes("g1.policy");
    forged[forged.size() - 1U] ^= 0x01U;
    write_bytes("forged.policy", cont::ConstSpan<u8>(forged.data(), forged.size()));
    CHECK(has(verify("forged.policy", "trust.toml", "2026-11-10"), R"("error":"bad-signature")"));

    // A trust set of one key is no hybrid; a source that differs from the signed content by one value is refused.
    write_trust("one.toml", {&issue.post_quantum});
    CHECK(has(verify("g1.policy", "one.toml", "2026-11-10"), R"("reason":"bad-config","file":"one.toml")"));
    write_variant("other.policy.toml", "offline_grace_days = 90", "offline_grace_days = 60");
    CHECK(has(verify("g1.policy", "trust.toml", "2026-11-10", nullptr, "other.policy.toml"),
              R"("reason":"source-mismatch","file":"other.policy.toml")"));

    // Malformed requests and trust sets.
    CHECK(has(verify(nullptr, "trust.toml", "2026-11-10"), "verify needs"));
    CHECK(has(verify("g1.policy", "trust.toml", "2026-13-01"), "--now must be"));
    CHECK(has(verify("g1.policy", "trust.toml", "99999999999999999999"), "--now must be"));
    CHECK(has(verify("g1.policy", "trust.toml", "2026-11-10", "-1"), "--floor must be"));
    CHECK(
        has(verify("missing.policy", "trust.toml", "2026-11-10"), R"("reason":"unreadable","file":"missing.policy")"));
    write_text("bad-hex.toml", "[[key]]\nalgorithm = \"ed25519\"\npublic_key = \"0A\"\n");
    CHECK(has(verify("g1.policy", "bad-hex.toml", "2026-11-10"),
              R"("reason":"bad-hex","file":"bad-hex.toml","key":"public_key","line":3)"));
    write_text("bad-alg.toml", "[[key]]\nalgorithm = \"rsa-2048\"\npublic_key = \"0a\"\n");
    CHECK(has(verify("g1.policy", "bad-alg.toml", "2026-11-10"), R"("reason":"unknown-algorithm")"));
    write_text("extra.toml", "[[key]]\nalgorithm = \"ed25519\"\npublic_key = \"0a\"\nlabel = \"x\"\n");
    CHECK(has(verify("g1.policy", "extra.toml", "2026-11-10"),
              R"("reason":"unknown-key","file":"extra.toml","key":"label","line":4)"));
}

TEST_CASE("policy assemble: one body, each signer once, and every input decoded", "[ceridc][policy][net-2]")
{
    const Issue issue;
    const cont::String joined = assemble({"pq.signed", "classical.signed"}, "again.policy");
    INFO(joined.c_str());
    CHECK(has(joined, R"("verb":"policy","ok":true,"action":"assemble","out":"again.policy","bytes":)"));
    CHECK(has(joined, R"("generation":1,"signatures":[{"key":")"));

    // A signature of another body (generation 2 of the same rules) is never joined to generation 1.
    write_variant("g2.policy.toml", "generation = 1", "generation = 2");
    REQUIRE(has(body("g2.policy.toml", "g2.body"), R"("ok":true)"));
    issue.classical.sign("g2.body", "g2-classical.signed");
    CHECK(has(assemble({"pq.signed", "g2-classical.signed"}, "mixed.policy"),
              R"("reason":"different-bodies","file":"g2-classical.signed")"));
    CHECK(has(assemble({"pq.signed", "pq.signed"}, "twice.policy"), R"("reason":"duplicate-signature")"));
    CHECK(has(assemble({"g1.policy", "pq.signed"}, "twice.policy"), R"("reason":"duplicate-signature")"));
    CHECK_FALSE(fs::exists(fs::Path("mixed.policy")));
    CHECK_FALSE(fs::exists(fs::Path("twice.policy")));

    CHECK(has(assemble({"g1.body"}, "body.policy"), R"("reason":"truncated","file":"g1.body")"));
    write_text("junk.policy", "not a policy generation");
    CHECK(has(assemble({"junk.policy"}, "junk.out"), R"("reason":"bad-magic")"));
    CHECK(has(assemble({"missing.signed"}, "x.policy"), R"("reason":"unreadable","file":"missing.signed")"));
    CHECK(has(assemble({}, "x.policy"), "assemble needs"));
    CHECK(has(assemble({"1", "2", "3", "4", "5", "6", "7", "8", "9"}, "x.policy"), R"("reason":"too-large")"));
}

TEST_CASE("policy body: a malformed source is refused with its key and line, and nothing is written",
          "[ceridc][policy][net-2]")
{
    struct Case
    {
        const char* from;
        const char* to;
        const char* expected;
    };
    const Case cases[] = {
        {"generation = 1", "generation = 1\ncolour = 3", R"("reason":"unknown-key","file":"case.toml","key":"colour")"},
        {"generation = 1", "generation = 0", R"("reason":"bad-value","file":"case.toml","key":"generation")"},
        {"generation = 1", "generation = \"1\"", R"("reason":"bad-value","file":"case.toml","key":"generation")"},
        {"issued = \"2026-11-03\"", "", R"("reason":"missing-key","file":"case.toml","key":"issued")"},
        {"issued = \"2026-11-03\"", "issued = \"2026-02-30\"",
         R"("reason":"bad-day","file":"case.toml","key":"issued")"},
        {"issued = \"2026-11-03\"", "issued = \"1969-12-31\"",
         R"("reason":"bad-day","file":"case.toml","key":"issued")"},
        {"issued = \"2026-11-03\"", "issued = \"never\"", R"("reason":"bad-day","file":"case.toml","key":"issued")"},
        {"offline_grace_days = 90", "offline_grace_days = 91",
         R"("reason":"bad-value","file":"case.toml","key":"offline_grace_days")"},
        {"expires = \"2027-11-03\"", "expires = \"2026-11-03\"", R"("reason":"bad-dates","file":"case.toml"})"},
        {"expires = \"2027-11-03\"", "expires = \"2027-12-09\"", R"("reason":"bad-dates","file":"case.toml"})"},
        {"algorithm = \"siphash-2-4\"", "algorithm = \"rot13\"",
         R"("reason":"unknown-algorithm","file":"case.toml","key":"algorithm")"},
        {"algorithm = \"siphash-2-4\"\nscope = \"everywhere\"", "algorithm = \"siphash-2-4\"\nscope = \"lan\"",
         R"("reason":"unknown-scope","file":"case.toml","key":"scope")"},
        {"algorithm = \"siphash-2-4\"\nscope = \"everywhere\"", "algorithm = \"siphash-2-4\"",
         R"("reason":"missing-key","file":"case.toml","key":"scope")"},
        {"algorithm = \"siphash-2-4\"", "algorithm = \"hkdf-sha384\"",
         R"("reason":"duplicate-rule","file":"case.toml"})"},
        {"algorithm = \"siphash-2-4\"\nscope = \"everywhere\"",
         "algorithm = \"siphash-2-4\"\nscope = \"everywhere\"\npreferred_until = \"2030-01-01\"",
         R"("reason":"bad-value","file":"case.toml","key":"preferred_until")"},
        {"algorithm = \"siphash-2-4\"\nscope = \"everywhere\"",
         "algorithm = \"siphash-2-4\"\nscope = \"everywhere\"\naccepted_from = \"2030-01-01\"\nrefused_from = "
         "\"2029-01-01\"",
         R"("reason":"bad-rule","file":"case.toml"})"},
        {"algorithm = \"siphash-2-4\"\nscope = \"everywhere\"",
         "algorithm = \"siphash-2-4\"\nscope = \"everywhere\"\naccepted_from = \"never\"",
         R"("reason":"bad-day","file":"case.toml","key":"accepted_from")"},
        {"[[rule]]\nalgorithm = \"siphash-2-4\"",
         "[[minimum]]\nprotocol = 0\nmajor = 1\nminor = 0\n\n[[rule]]\nalgorithm = \"siphash-2-4\"",
         R"("reason":"bad-value","file":"case.toml","key":"protocol")"},
        {"generation = 1", "generation = 1\nminimum = 3", R"("reason":"bad-value","file":"case.toml","key":"minimum")"},
        {"generation = 1", "generation = = 1", R"("reason":"toml","file":"case.toml")"},
    };
    for (const Case& c : cases)
    {
        INFO(c.to);
        write_variant("case.toml", c.from, c.to);
        const cont::String out = body("case.toml", "case.body");
        INFO(out.c_str());
        CHECK(has(out, R"("verb":"policy","ok":false,"action":"body")"));
        CHECK(has(out, c.expected));
        CHECK_FALSE(fs::exists(fs::Path("case.body")));
    }

    // A minimum version, and the 33rd rule beyond the bound.
    write_variant("minimum.toml", "[[rule]]\nalgorithm = \"siphash-2-4\"",
                  "[[minimum]]\nprotocol = 2\nmajor = 1\nminor = 3\n\n[[rule]]\nalgorithm = \"siphash-2-4\"");
    CHECK(has(body("minimum.toml", "minimum.body"), R"("minimums":[{"protocol":2,"major":1,"minor":3}])"));
    cont::String many;
    for (int i = 0; i < 10; ++i)
    {
        many += "\n[[rule]]\nalgorithm = \"siphash-2-4\"\nscope = \"cerid-services\"\n";
    }
    cont::String text;
    REQUIRE(fs::read_file_text(fs::Path(kSource), text));
    text += many;
    write_text("many.toml", text);
    CHECK(has(body("many.toml", "many.body"), R"("reason":"too-many","file":"many.toml","key":"rule")"));

    CHECK(has(body(nullptr, "x.body"), "body needs --source and --out"));
    CHECK(has(body("missing.toml", "x.body"), R"("reason":"unreadable","file":"missing.toml")"));
    crd::ceridc::PolicyFlags flags;
    flags.action = "seal";
    CHECK(has(crd::ceridc::verb_policy(flags, crd::memory::default_allocator()), "the action must be"));
    flags.action = nullptr;
    CHECK(has(crd::ceridc::verb_policy(flags, crd::memory::default_allocator()), "the action must be"));
}

TEST_CASE("policy key and sign: the key holders' stored keys issue generation 1", "[ceridc][policy][net-2]")
{
    remove_tree("keys-pq");
    remove_tree("keys-classical");
    MemoryProtector protector;
    const cont::String pq = key(&protector, "keys-pq", "policy-pq", "ml-dsa-65");
    INFO(pq.c_str());
    CHECK(has(pq, R"("verb":"policy","ok":true,"action":"key","store":"keys-pq","protection":"application",)"
                  R"("made":true,"signer":{"name":"policy-pq","generation":1,"algorithm":"ml-dsa-65",)"
                  R"("placement":"software","key":")"));
    CHECK(has(pq, R"("trust_entry":"[[key]]\nalgorithm = \"ml-dsa-65\"\npublic_key = \")"));
    CHECK(json_text(pq, "public_key").size() == 2U * 1952U);
    const cont::String classical = key(&protector, "keys-classical", "policy-ec", "ecdsa-secp256r1-sha256");
    REQUIRE(has(classical, R"("ok":true)"));
    CHECK(json_text(classical, "public_key").size() == 2U * 65U);

    // Asked again, the store answers with the key it holds; another algorithm or placement under the name is refused.
    const cont::String again = key(&protector, "keys-pq", "policy-pq", "ml-dsa-65", "hardware-preferred");
    CHECK(has(again, R"("made":false)"));
    CHECK(json_text(again, "public_key") == json_text(pq, "public_key"));
    CHECK(has(key(&protector, "keys-pq", "policy-pq", "ml-dsa-87"), R"("reason":"name-holds-another-algorithm")"));
    CHECK(has(key(&protector, "keys-pq", "policy-pq", "ml-dsa-65", "hardware-required"),
              R"("reason":"name-holds-another-placement")"));
    // Without a device no key is placed in hardware.
    CHECK(has(key(&protector, "keys-pq", "policy-hw", "ecdsa-secp256r1-sha256", "hardware-required"),
              R"("reason":"unsupported")"));

    // Each holder signs the rendered body: from the source, or from the body file; joined, the pair verifies.
    REQUIRE(has(body(kSource, "g1.body"), R"("ok":true)"));
    const cont::String signed_pq = sign(&protector, "keys-pq", "policy-pq", kSource, nullptr, "pq-stored.signed");
    INFO(signed_pq.c_str());
    CHECK(has(signed_pq, R"("verb":"policy","ok":true,"action":"sign","out":"pq-stored.signed","bytes":)"));
    CHECK(has(signed_pq, R"("generation":1,"issued":"2026-11-03T00:00:00Z","expires":"2027-11-03T00:00:00Z")"));
    CHECK(has(signed_pq, R"({"algorithm":"x25519","scope":"cerid-services","accepted_from":"1970-01-01T00:00:00Z",)"));
    CHECK(has(signed_pq, R"("signatures":[{"key":")"));
    CHECK(json_text(signed_pq, "key") == json_text(pq, "key")); // the signature names the stored key
    const cont::String signed_ec =
        sign(&protector, "keys-classical", "policy-ec", nullptr, "g1.body", "ec-stored.signed");
    REQUIRE(has(signed_ec, R"("ok":true)"));
    REQUIRE(has(assemble({"pq-stored.signed", "ec-stored.signed"}, "stored.policy"), R"("ok":true)"));
    write_trust_of("stored-trust.toml", {&pq, &classical});
    const cont::String judged = verify("stored.policy", "stored-trust.toml", "2026-11-10", nullptr, kSource);
    INFO(judged.c_str());
    CHECK(has(judged, R"("ok":true,"action":"verify","error":"none")"));
    CHECK_FALSE(has(judged, R"("trusted":false)"));

    // Refusals, each before anything is written.
    CHECK(has(key(nullptr, "keys-pq", "policy-pq", "ml-dsa-65"), "key needs"));
    CHECK(has(key(&protector, "keys-pq", "policy-aead", "aes-256-gcm"), R"("reason":"not-a-signature-algorithm")"));
    CHECK(has(key(&protector, "keys-pq", "policy-rsa", "rsa-2048"), R"("reason":"not-a-signature-algorithm")"));
    CHECK(has(key(&protector, "keys-pq", "Bad Name", "ml-dsa-65"), R"("reason":"bad-key-name")"));
    CHECK(has(key(&protector, "keys-pq", "policy-pq", "ml-dsa-65", "tpm"), "--placement must be"));
    MemoryProtector stranger("stranger");
    CHECK(has(key(&stranger, "keys-pq", "policy-pq", "ml-dsa-65"), R"("reason":"unsupported","file":"keys-pq")"));
    CHECK(has(sign(&protector, "keys-pq", "policy-pq", kSource, "g1.body", "x.signed"), "sign needs"));
    CHECK(has(sign(&protector, "keys-pq", "policy-pq", nullptr, nullptr, "x.signed"), "sign needs"));
    CHECK(has(sign(nullptr, "keys-pq", "policy-pq", kSource, nullptr, "x.signed"), "sign needs"));
    CHECK(has(sign(&protector, "keys-none", "policy-pq", kSource, nullptr, "x.signed"),
              R"("reason":"not-found","file":"keys-none")"));
    CHECK(has(sign(&protector, "keys-pq", "policy-none", kSource, nullptr, "x.signed"), R"("reason":"not-found")"));
    write_text("junk.body", "not a policy body");
    CHECK(has(sign(&protector, "keys-pq", "policy-pq", nullptr, "junk.body", "x.signed"),
              R"("reason":"bad-magic","file":"junk.body")"));
    CHECK(has(sign(&protector, "keys-pq", "policy-pq", nullptr, "missing.body", "x.signed"),
              R"("reason":"unreadable","file":"missing.body")"));
    write_variant("bad-source.policy.toml", "generation = 1", "generation = 0");
    CHECK(has(sign(&protector, "keys-pq", "policy-pq", "bad-source.policy.toml", nullptr, "x.signed"),
              R"("file":"bad-source.policy.toml")"));
    CHECK_FALSE(fs::exists(fs::Path("x.signed")));
    CHECK_FALSE(fs::exists(fs::Path("keys-none")));
}

TEST_CASE("policy: the real binary gives the verb's bytes", "[ceridc][policy][net-2]")
{
    cont::String exe;
    REQUIRE(crd::read_environment_text("CRD_CERIDC_EXE", exe)); // wired by CMake
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd), R"(%s"%s" policy body --source "%s" --out bin.body > policy_out.json%s)",
                        kShellQuote, exe.c_str(), kSource, kShellQuote);
    REQUIRE(std::system(cmd) == 0);
    cont::String out;
    REQUIRE(fs::read_file_text(fs::Path("policy_out.json"), out));
    while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
    {
        out.pop_back();
    }
    const cont::String expected = body(kSource, "bin.body");
    CHECK(cont::StringView(out) == cont::StringView(expected));

    (void)std::snprintf(cmd, sizeof(cmd), R"(%s"%s" policy seal > policy_out.json%s)", kShellQuote, exe.c_str(),
                        kShellQuote);
    CHECK(std::system(cmd) != 0);
    CHECK(has(read_text("policy_out.json"), "the action must be body, assemble, verify, key or sign"));
    (void)std::snprintf(cmd, sizeof(cmd), R"(%s"%s" policy key --protector vault > policy_out.json%s)", kShellQuote,
                        exe.c_str(), kShellQuote);
    CHECK(std::system(cmd) != 0); // an unknown protector is a usage error
    (void)std::snprintf(cmd, sizeof(cmd), R"(%s"%s" policy key --protector platform --device pkcs11 > out.json%s)",
                        kShellQuote, exe.c_str(), kShellQuote);
    CHECK(std::system(cmd) != 0); // a PKCS #11 device needs its module, label and PIN file
}

TEST_CASE("policy: the real binary keeps the key holders' keys under the operating system's protector",
          "[ceridc][policy][net-2]")
{
    cont::String exe;
    REQUIRE(crd::read_environment_text("CRD_CERIDC_EXE", exe)); // wired by CMake
    remove_tree("os-keys-pq");
    remove_tree("os-keys-ec");
    char cmd[1024];
    const auto run = [&](const char* arguments, const char* out)
    {
        (void)std::snprintf(cmd, sizeof(cmd), R"(%s"%s" policy %s --protector platform > %s%s)", kShellQuote,
                            exe.c_str(), arguments, out, kShellQuote);
        CHECK(std::system(cmd) != -1); // the shell ran; the exit status is the report's "ok"
        return read_text(out);
    };
    const cont::String pq = run("key --store os-keys-pq --name policy-pq --algorithm ml-dsa-65", "os_pq.json");
    if (!crypto::PlatformKeyProtector::available())
    {
        // No vault answers here (a Linux session without a Secret Service): the store is refused, nothing is made.
        CHECK(has(pq, R"("ok":false,"action":"key","reason":"unavailable")"));
        CHECK_FALSE(fs::exists(fs::Path("os-keys-pq/keys")));
        return;
    }
    REQUIRE(has(pq, R"("ok":true,"action":"key","store":"os-keys-pq","protection":"platform","made":true)"));
    const cont::String ec =
        run("key --store os-keys-ec --name policy-ec --algorithm ecdsa-secp256r1-sha256", "os_ec.json");
    REQUIRE(has(ec, R"("ok":true)"));
    char arguments[512];
    (void)std::snprintf(arguments, sizeof(arguments),
                        R"(sign --store os-keys-pq --name policy-pq --source "%s" --out os-pq.signed)", kSource);
    REQUIRE(has(run(arguments, "os_sign_pq.json"), R"("ok":true,"action":"sign")"));
    REQUIRE(has(body(kSource, "os.body"), R"("ok":true)"));
    REQUIRE(has(run("sign --store os-keys-ec --name policy-ec --body os.body --out os-ec.signed", "os_sign_ec.json"),
                R"("ok":true,"action":"sign")"));
    REQUIRE(has(assemble({"os-pq.signed", "os-ec.signed"}, "os.policy"), R"("ok":true)"));
    write_trust_of("os-trust.toml", {&pq, &ec});
    const cont::String judged = verify("os.policy", "os-trust.toml", "2026-11-10", nullptr, kSource);
    INFO(judged.c_str());
    CHECK(has(judged, R"("ok":true,"action":"verify","error":"none")"));
    CHECK_FALSE(has(judged, R"("trusted":false)"));
}
