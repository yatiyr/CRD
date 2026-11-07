// ceridc policy -- the crypto policy generation's issuing tool (NET.2; contract:
// docs/design/network-foundation.md#policy-issue). The layout and its checks, the store and the provider's
// verification are crd-crypto's; these verbs read the reviewed source and the trust set, write and join the signed
// bytes and report what an installation would decide. The key holders' keys live in crd-crypto's key store: `key`
// makes one and prints its trust set entry, `sign` signs a body with it.

#include <crd/ceridc/verbs.hpp>

#include "decimal.hpp"

#include <crd/comply/generation.hpp>
#include <crd/containers/array.hpp>
#include <crd/crypto/key_store.hpp>
#include <crd/crypto/policy.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/json/json_write.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/toml/toml.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <string_view>

namespace crd::ceridc
{
namespace
{
namespace crypto = crd::crypto;
namespace cont = crd::containers;
namespace fs = crd::platform::fs;
using crd::json::JsonWriter;
using crypto::AlgorithmId;
using crypto::PolicyContent;
using crypto::PolicyError;
using crypto::PolicyScope;

constexpr const char* kVerb = "policy";
constexpr crd::usize kMaxTextBytes = crd::usize{1} << 20U; // a source or trust file
constexpr crd::usize kMaxPublicKeyBytes = 4096U;           // a trust key (ML-DSA-87: 2592)
constexpr crd::u64 kDaySeconds = 86400U;
constexpr crd::u64 kLastFormattable = 253402300799U;                // 9999-12-31T23:59:59Z
constexpr const char* kScopes[] = {"everywhere", "cerid-services"}; // by PolicyScope value
constexpr const char* kStatuses[] = {"refused", "accepted", "preferred"};
constexpr const char* kFreshness[] = {"none", "current", "grace", "expired"};
constexpr const char* kPlacements[] = {"software", "hardware"}; // by KeyPlacement value

// Why a request or an input was refused: the reason, and for a file its key and line.
struct Failure
{
    cont::StringView reason;
    cont::String key;
    crd::u32 line = 0U;
};

[[nodiscard]] cont::String refuse(crd::memory::IAllocator* alloc, const char* action, const Failure& failure,
                                  const char* file = nullptr)
{
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", false);
    w.kv("action", action != nullptr ? action : "");
    w.kv("reason", failure.reason);
    if (file != nullptr)
    {
        w.kv("file", file);
    }
    if (!failure.key.empty())
    {
        w.kv("key", cont::StringView(failure.key));
    }
    if (failure.line != 0U)
    {
        w.kv("line", failure.line);
    }
    w.end_object();
    return cont::String(w.str());
}

[[nodiscard]] cont::String refuse(crd::memory::IAllocator* alloc, const char* action, cont::StringView reason,
                                  const char* file = nullptr)
{
    Failure failure;
    failure.reason = reason;
    return refuse(alloc, action, failure, file);
}

[[nodiscard]] bool fail(Failure& failure, cont::StringView reason, cont::StringView key, const crd::toml::node* node)
{
    failure.reason = reason;
    failure.key = cont::String(key);
    failure.line = node != nullptr ? node->source().begin.line : 0U;
    return false;
}

// The Unix second `text` begins (00:00:00 UTC): exactly YYYY-MM-DD, 1970-01-01 or later.
[[nodiscard]] bool day_start(std::string_view text, crd::u64& out)
{
    crd::comply::Day day = crd::comply::kNoDay;
    if (text.size() != 10U || !crd::comply::parse_day(cont::StringView(text.data(), text.size()), day) ||
        day < 19700101U)
    {
        return false;
    }
    const std::chrono::year_month_day civil{std::chrono::year{static_cast<int>(day / 10000U)},
                                            std::chrono::month{(day / 100U) % 100U}, std::chrono::day{day % 100U}};
    out = static_cast<crd::u64>(std::chrono::sys_days(civil).time_since_epoch().count()) * kDaySeconds;
    return true;
}

// `seconds` as an ISO 8601 UTC instant, "never" for kNever, decimal past year 9999.
void instant_text(crd::u64 seconds, char (&out)[32])
{
    if (seconds == crypto::kNever)
    {
        (void)std::snprintf(out, sizeof(out), "never");
        return;
    }
    if (seconds > kLastFormattable)
    {
        (void)std::snprintf(out, sizeof(out), "%llu", static_cast<unsigned long long>(seconds));
        return;
    }
    const std::chrono::year_month_day civil{
        std::chrono::sys_days{std::chrono::days{static_cast<std::chrono::days::rep>(seconds / kDaySeconds)}}};
    const crd::u64 rest = seconds % kDaySeconds;
    (void)std::snprintf(out, sizeof(out), "%04d-%02u-%02uT%02u:%02u:%02uZ", static_cast<int>(civil.year()),
                        static_cast<unsigned>(civil.month()), static_cast<unsigned>(civil.day()),
                        static_cast<unsigned>(rest / 3600U), static_cast<unsigned>((rest / 60U) % 60U),
                        static_cast<unsigned>(rest % 60U));
}

void hex_text(cont::ConstSpan<crd::u8> bytes, cont::String& out)
{
    constexpr char digits[] = "0123456789abcdef";
    out.clear();
    for (const crd::u8 byte : bytes)
    {
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 0x0FU]);
    }
}

[[nodiscard]] int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return -1; // upper case is refused: one spelling per key
}

[[nodiscard]] bool read_hex(std::string_view text, cont::Array<crd::u8>& out)
{
    if (text.empty() || text.size() % 2U != 0U || text.size() > 2U * kMaxPublicKeyBytes)
    {
        return false;
    }
    out.clear();
    for (crd::usize i = 0U; i < text.size(); i += 2U)
    {
        const int high = hex_digit(text[i]);
        const int low = hex_digit(text[i + 1U]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        out.push_back(static_cast<crd::u8>((high << 4) | low));
    }
    return true;
}

// The register name of `id`, or its value in hex for one this build does not know.
void algorithm_text(AlgorithmId id, char (&out)[32])
{
    const char* name = crypto::algorithm_name(id);
    (void)std::snprintf(out, sizeof(out), "%s", name);
    if (name[0] == '\0')
    {
        (void)std::snprintf(out, sizeof(out), "0x%04x", static_cast<unsigned>(id));
    }
}

[[nodiscard]] bool algorithm_named(std::string_view name, AlgorithmId& out)
{
    for (const AlgorithmId id : crypto::kAlgorithmIds)
    {
        if (name == crypto::algorithm_name(id))
        {
            out = id;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool read_file(const char* path, crd::usize max, cont::Array<crd::u8>& out)
{
    return fs::read_file_binary(fs::Path(cont::StringView(path)), out) && out.size() <= max;
}

// ---- The reviewed TOML of a source or a trust set: every key known, every value in range ----

template <crd::usize N>
[[nodiscard]] bool known_keys(const crd::toml::node& table, const char* const (&allowed)[N], Failure& failure)
{
    for (auto&& [key, value] : table.items())
    {
        bool found = false;
        for (const char* name : allowed)
        {
            found = found || key == cont::StringView(name);
        }
        if (!found)
        {
            return fail(failure, "unknown-key", key, &value);
        }
    }
    return true;
}

[[nodiscard]] bool read_integer(const crd::toml::node& table, const char* key, crd::i64 low, crd::i64 high,
                                crd::i64& out, Failure& failure)
{
    const crd::toml::node* node = table.get(key);
    if (node == nullptr)
    {
        return fail(failure, "missing-key", key, &table);
    }
    const std::optional<crd::i64> value = node->is_integer() ? node->value<crd::i64>() : std::nullopt;
    if (!value.has_value() || *value < low || *value > high)
    {
        return fail(failure, "bad-value", key, node);
    }
    out = *value;
    return true;
}

[[nodiscard]] bool read_text(const crd::toml::node& table, const char* key, std::string_view& out, Failure& failure)
{
    const crd::toml::node* node = table.get(key);
    if (node == nullptr)
    {
        return fail(failure, "missing-key", key, &table);
    }
    const std::optional<std::string_view> value = node->value<std::string_view>();
    if (!value.has_value())
    {
        return fail(failure, "bad-value", key, node);
    }
    out = *value;
    return true;
}

// The instant of day field `key`: `fallback` when absent, kNever for "never" where `never` allows it.
[[nodiscard]] bool read_instant(const crd::toml::node& table, const char* key, bool never, crd::u64 fallback,
                                crd::u64& out, Failure& failure)
{
    const crd::toml::node* node = table.get(key);
    if (node == nullptr)
    {
        out = fallback;
        return true;
    }
    const std::optional<std::string_view> text = node->value<std::string_view>();
    if (text.has_value() && never && *text == "never")
    {
        out = crypto::kNever;
        return true;
    }
    if (!text.has_value() || !day_start(*text, out))
    {
        return fail(failure, "bad-day", key, node);
    }
    return true;
}

// The tables of array `key` (absent: none), at most `max` of them.
[[nodiscard]] bool read_tables(const crd::toml::node& root, const char* key, crd::usize max,
                               const crd::toml::node*& out, Failure& failure)
{
    out = root.get(key);
    if (out != nullptr && (!out->is_array_of_tables() || out->size() > max))
    {
        return fail(failure, out->is_array_of_tables() ? "too-many" : "bad-value", key, out);
    }
    return true;
}

[[nodiscard]] bool parse_rule(const crd::toml::node& table, crypto::AlgorithmRule& rule, Failure& failure)
{
    static constexpr const char* kKeys[] = {"algorithm",      "scope",           "accepted_from",
                                            "preferred_from", "preferred_until", "refused_from"};
    std::string_view algorithm;
    std::string_view scope;
    if (!known_keys(table, kKeys, failure) || !read_text(table, "algorithm", algorithm, failure) ||
        !read_text(table, "scope", scope, failure))
    {
        return false;
    }
    if (!algorithm_named(algorithm, rule.algorithm))
    {
        return fail(failure, "unknown-algorithm", "algorithm", table.get("algorithm"));
    }
    if (scope == kScopes[0] || scope == kScopes[1])
    {
        rule.scope = scope == kScopes[0] ? PolicyScope::Everywhere : PolicyScope::CeridServices;
    }
    else
    {
        return fail(failure, "unknown-scope", "scope", table.get("scope"));
    }
    if (!read_instant(table, "accepted_from", false, 0U, rule.accepted_from, failure) ||
        !read_instant(table, "refused_from", true, crypto::kNever, rule.refused_from, failure))
    {
        return false;
    }
    if (table.get("preferred_from") == nullptr)
    {
        if (table.get("preferred_until") != nullptr)
        {
            return fail(failure, "bad-value", "preferred_until", table.get("preferred_until"));
        }
        rule.preferred_from = rule.accepted_from; // an empty preferred window: never preferred
        rule.preferred_until = rule.accepted_from;
        return true;
    }
    return read_instant(table, "preferred_from", false, 0U, rule.preferred_from, failure) &&
           read_instant(table, "preferred_until", true, rule.refused_from, rule.preferred_until, failure);
}

// The content of a reviewed generation source, checked as a decoder would check its bytes.
[[nodiscard]] bool parse_source(cont::ConstSpan<crd::u8> text, PolicyContent& content, Failure& failure)
{
    const std::string_view view(reinterpret_cast<const char*>(text.data()), text.size()); // NOLINT
    const crd::toml::parse_result parsed = crd::toml::parse(view);
    if (!parsed.succeeded())
    {
        failure.reason = "toml";
        failure.key = cont::String(parsed.error().description());
        failure.line = parsed.error().line();
        return false;
    }
    const crd::toml::node& root = parsed.table();
    static constexpr const char* kKeys[] = {"generation", "issued", "expires", "offline_grace_days", "minimum", "rule"};
    static constexpr crd::i64 kMaxGraceDays = crypto::kMaxOfflineGrace / kDaySeconds;
    crd::i64 generation = 0;
    crd::i64 grace_days = 0;
    if (!known_keys(root, kKeys, failure) || !read_integer(root, "generation", 1, INT64_MAX, generation, failure) ||
        !read_integer(root, "offline_grace_days", 0, kMaxGraceDays, grace_days, failure))
    {
        return false;
    }
    for (const char* key : {"issued", "expires"})
    {
        if (root.get(key) == nullptr)
        {
            return fail(failure, "missing-key", key, &root);
        }
    }
    content.generation = static_cast<crd::u64>(generation);
    content.offline_grace = static_cast<crd::u32>(grace_days) * static_cast<crd::u32>(kDaySeconds);
    const crd::toml::node* minimums = nullptr;
    const crd::toml::node* rules = nullptr;
    if (!read_instant(root, "issued", false, 0U, content.issued, failure) ||
        !read_instant(root, "expires", false, 0U, content.expires, failure) ||
        !read_tables(root, "minimum", crypto::kMaxPolicyMinimums, minimums, failure) ||
        !read_tables(root, "rule", crypto::kMaxPolicyRules, rules, failure))
    {
        return false;
    }
    static constexpr const char* kMinimumKeys[] = {"protocol", "major", "minor"};
    for (crd::usize i = 0U; minimums != nullptr && i < minimums->size(); ++i)
    {
        const crd::toml::node& table = minimums->child_at(i);
        crd::i64 values[3] = {};
        if (!known_keys(table, kMinimumKeys, failure) ||
            !read_integer(table, "protocol", 1, UINT16_MAX, values[0], failure) ||
            !read_integer(table, "major", 0, UINT16_MAX, values[1], failure) ||
            !read_integer(table, "minor", 0, UINT16_MAX, values[2], failure))
        {
            return false;
        }
        content.minimums[content.minimum_count++] = {static_cast<crd::u16>(values[0]), static_cast<crd::u16>(values[1]),
                                                     static_cast<crd::u16>(values[2])};
    }
    for (crd::usize i = 0U; rules != nullptr && i < rules->size(); ++i)
    {
        if (!parse_rule(rules->child_at(i), content.rules[content.rule_count++], failure))
        {
            return false;
        }
    }
    const PolicyError error = crypto::validate_content(content);
    if (error != PolicyError::None)
    {
        failure.reason = crypto::policy_error_name(error);
        return false;
    }
    return true;
}

// The body `source` renders, in `body`.
[[nodiscard]] bool render_source(const char* source, PolicyContent& content, cont::Array<crd::u8>& body,
                                 Failure& failure)
{
    cont::Array<crd::u8> text;
    if (!read_file(source, kMaxTextBytes, text))
    {
        failure.reason = "unreadable";
        return false;
    }
    if (!parse_source(cont::as_const_span(text), content, failure))
    {
        return false;
    }
    body.resize(crypto::policy_body_size(content));
    crd::usize written = 0U;
    const PolicyError error =
        crypto::write_policy_body(content, cont::Span<crd::u8>(body.data(), body.size()), written);
    if (error != PolicyError::None || written != body.size())
    {
        failure.reason = crypto::policy_error_name(error);
        return false;
    }
    return true;
}

// The installation's trust set: each [[key]]'s algorithm (by register name) and public key (lower-case hex).
struct TrustSet
{
    crypto::TrustKey keys[crypto::kMaxTrustKeys];
    cont::Array<crd::u8> public_keys[crypto::kMaxTrustKeys];
    crd::u32 count = 0U;
};

[[nodiscard]] bool read_trust(const char* path, TrustSet& trust, Failure& failure)
{
    cont::Array<crd::u8> text;
    if (!read_file(path, kMaxTextBytes, text))
    {
        failure.reason = "unreadable";
        return false;
    }
    const std::string_view view(reinterpret_cast<const char*>(text.data()), text.size()); // NOLINT
    const crd::toml::parse_result parsed = crd::toml::parse(view);
    if (!parsed.succeeded())
    {
        failure.reason = "toml";
        failure.key = cont::String(parsed.error().description());
        failure.line = parsed.error().line();
        return false;
    }
    static constexpr const char* kKeys[] = {"key"};
    static constexpr const char* kKeyKeys[] = {"algorithm", "public_key"};
    const crd::toml::node* keys = nullptr;
    if (!known_keys(parsed.table(), kKeys, failure) ||
        !read_tables(parsed.table(), "key", crypto::kMaxTrustKeys, keys, failure))
    {
        return false;
    }
    for (crd::usize i = 0U; keys != nullptr && i < keys->size(); ++i)
    {
        const crd::toml::node& table = keys->child_at(i);
        crypto::TrustKey& key = trust.keys[trust.count];
        cont::Array<crd::u8>& public_key = trust.public_keys[trust.count];
        std::string_view algorithm;
        std::string_view hex;
        if (!known_keys(table, kKeyKeys, failure) || !read_text(table, "algorithm", algorithm, failure) ||
            !read_text(table, "public_key", hex, failure))
        {
            return false;
        }
        if (!algorithm_named(algorithm, key.algorithm))
        {
            return fail(failure, "unknown-algorithm", "algorithm", table.get("algorithm"));
        }
        if (!read_hex(hex, public_key))
        {
            return fail(failure, "bad-hex", "public_key", table.get("public_key"));
        }
        key.public_key = cont::as_const_span(public_key);
        key.id = crypto::key_id_of(key.public_key);
        ++trust.count;
    }
    return true;
}

// An instant on the command line: YYYY-MM-DD (its first second) or Unix seconds.
[[nodiscard]] bool read_now(const char* text, crd::u64& out)
{
    const cont::StringView view(text);
    return day_start(std::string_view(view.data(), view.size()), out) || read_decimal(view, out);
}

// ---- The report ----

void write_digest(JsonWriter& w, cont::ConstSpan<crd::u8> bytes)
{
    crd::u8 digest[crypto::kSha256Bytes];
    crypto::sha256(bytes, digest);
    cont::String hex;
    hex_text(cont::ConstSpan<crd::u8>(digest), hex);
    w.kv("bytes", static_cast<crd::u64>(bytes.size()));
    w.kv("sha256", cont::StringView(hex));
}

// The content; with a snapshot, each rule's status at `now` in its scope and whether this build implements it.
void write_content(JsonWriter& w, const PolicyContent& content, const crypto::PolicySnapshot* snapshot, crd::u64 now)
{
    char text[32];
    w.kv("generation", content.generation);
    instant_text(content.issued, text);
    w.kv("issued", static_cast<const char*>(text));
    instant_text(content.expires, text);
    w.kv("expires", static_cast<const char*>(text));
    w.kv("offline_grace_seconds", content.offline_grace);
    w.key("minimums");
    w.begin_array();
    for (crd::u32 i = 0U; i < content.minimum_count; ++i)
    {
        const crypto::MinimumVersion& minimum = content.minimums[i];
        w.begin_object();
        w.kv("protocol", static_cast<crd::u32>(minimum.protocol));
        w.kv("major", static_cast<crd::u32>(minimum.major));
        w.kv("minor", static_cast<crd::u32>(minimum.minor));
        w.end_object();
    }
    w.end_array();
    w.key("rules");
    w.begin_array();
    for (crd::u32 i = 0U; i < content.rule_count; ++i)
    {
        const crypto::AlgorithmRule& rule = content.rules[i];
        const auto scope = static_cast<crd::u8>(rule.scope);
        const bool known_scope = scope < sizeof(kScopes) / sizeof(kScopes[0]);
        w.begin_object();
        algorithm_text(rule.algorithm, text);
        w.kv("algorithm", static_cast<const char*>(text));
        (void)std::snprintf(text, sizeof(text), "%s", known_scope ? kScopes[scope] : "unknown");
        w.kv("scope", static_cast<const char*>(text));
        const crd::u64 bounds[] = {rule.accepted_from, rule.preferred_from, rule.preferred_until, rule.refused_from};
        const char* names[] = {"accepted_from", "preferred_from", "preferred_until", "refused_from"};
        for (crd::usize b = 0U; b < 4U; ++b)
        {
            instant_text(bounds[b], text);
            w.kv(names[b], static_cast<const char*>(text));
        }
        if (snapshot != nullptr)
        {
            w.kv("in_build", crypto::provider_implements(rule.algorithm));
            if (known_scope)
            {
                w.kv("status", kStatuses[static_cast<crd::u8>(snapshot->status(rule.algorithm, now, rule.scope))]);
            }
        }
        w.end_object();
    }
    w.end_array();
}

// A product layer's rules that would accept what its base layer refuses (decision S25): they change nothing.
void write_loosenings(JsonWriter& w, const crypto::PolicySnapshot& snapshot)
{
    crypto::PolicyLoosening found[crypto::kMaxPolicyRules];
    const crd::u32 count = snapshot.loosenings(cont::Span<crypto::PolicyLoosening>(found));
    char text[32];
    w.key("loosenings");
    w.begin_array();
    for (crd::u32 i = 0U; i < count; ++i)
    {
        const crypto::PolicyLoosening& loosening = found[i];
        w.begin_object();
        algorithm_text(loosening.rule.algorithm, text);
        w.kv("algorithm", static_cast<const char*>(text));
        w.kv("scope", kScopes[static_cast<crd::u8>(loosening.rule.scope)]);
        instant_text(loosening.rule.accepted_from, text);
        w.kv("accepted_from", static_cast<const char*>(text));
        instant_text(loosening.rule.refused_from, text);
        w.kv("refused_from", static_cast<const char*>(text));
        const bool governed = loosening.base.algorithm != crypto::AlgorithmId{};
        w.kv("base_rule", governed);
        if (governed)
        {
            instant_text(loosening.base.accepted_from, text);
            w.kv("base_accepted_from", static_cast<const char*>(text));
            instant_text(loosening.base.refused_from, text);
            w.kv("base_refused_from", static_cast<const char*>(text));
        }
        w.end_object();
    }
    w.end_array();
}

void write_signatures(JsonWriter& w, const crypto::PolicyMessage& message, const TrustSet* trust)
{
    char text[32];
    cont::String hex;
    w.key("signatures");
    w.begin_array();
    for (crd::u32 i = 0U; i < message.signature_count; ++i)
    {
        const crypto::KeySignature& signature = message.signatures[i];
        w.begin_object();
        hex_text(cont::ConstSpan<crd::u8>(signature.key.bytes), hex);
        w.kv("key", cont::StringView(hex));
        algorithm_text(signature.algorithm, text);
        w.kv("algorithm", static_cast<const char*>(text));
        w.kv("bytes", static_cast<crd::u64>(signature.signature.size()));
        if (trust != nullptr)
        {
            bool trusted = false;
            for (crd::u32 k = 0U; k < trust->count; ++k)
            {
                trusted = trusted || trust->keys[k].id == signature.key;
            }
            w.kv("trusted", trusted);
        }
        w.end_object();
    }
    w.end_array();
}

[[nodiscard]] bool write_out(const char* path, cont::ConstSpan<crd::u8> bytes)
{
    return fs::write_file_binary(fs::Path(cont::StringView(path)), bytes);
}

// ---- The key holders' keys ----

[[nodiscard]] bool placement_named(cont::StringView name, crypto::PlacementRequest& out)
{
    if (name.empty() || name == "software")
    {
        out = crypto::PlacementRequest::Software;
        return true;
    }
    if (name == "hardware-preferred")
    {
        out = crypto::PlacementRequest::HardwarePreferred;
        return true;
    }
    if (name == "hardware-required")
    {
        out = crypto::PlacementRequest::HardwareRequired;
        return true;
    }
    return false;
}

// Whether a key kept at `kept` answers a request for `requested`.
[[nodiscard]] bool placement_answers(crypto::PlacementRequest requested, crypto::KeyPlacement kept)
{
    switch (requested)
    {
    case crypto::PlacementRequest::Software:
        return kept == crypto::KeyPlacement::Software;
    case crypto::PlacementRequest::HardwareRequired:
        return kept == crypto::KeyPlacement::Hardware;
    case crypto::PlacementRequest::HardwarePreferred:
        return true;
    }
    return false;
}

// A key's report, with the [[key]] entry read_trust reads for its trust set.
void write_key(JsonWriter& w, const crypto::KeyInfo& info, cont::ConstSpan<crd::u8> public_key)
{
    char algorithm[32];
    algorithm_text(info.algorithm, algorithm);
    cont::String hex;
    w.key("signer");
    w.begin_object();
    w.kv("name", info.name());
    w.kv("generation", info.generation);
    w.kv("algorithm", static_cast<const char*>(algorithm));
    w.kv("placement", kPlacements[static_cast<crd::u8>(info.placement)]);
    hex_text(cont::ConstSpan<crd::u8>(info.id.bytes), hex);
    w.kv("key", cont::StringView(hex));
    hex_text(public_key, hex);
    w.kv("public_key", cont::StringView(hex));
    cont::String entry("[[key]]\nalgorithm = \"");
    entry += static_cast<const char*>(algorithm);
    entry += "\"\npublic_key = \"";
    entry += cont::StringView(hex);
    entry += "\"\n";
    w.kv("trust_entry", cont::StringView(entry));
    w.end_object();
}

// The store at --store under the flags' protector and device; `create` makes one where there is none.
[[nodiscard]] crypto::CryptoError open_store(const PolicyFlags& flags, bool create, crypto::KeyStore& store)
{
    const cont::StringView directory(flags.store);
    const crypto::CryptoError opened = store.open(directory, *flags.protector, flags.device);
    if (opened == crypto::CryptoError::NotFound && create)
    {
        return store.create(directory, *flags.protector, flags.device);
    }
    return opened;
}

// ---- The actions ----

[[nodiscard]] cont::String policy_body(const PolicyFlags& flags, crd::memory::IAllocator* alloc)
{
    if (flags.source == nullptr || flags.out == nullptr)
    {
        return refuse(alloc, flags.action, "body needs --source and --out");
    }
    PolicyContent content;
    cont::Array<crd::u8> body;
    Failure failure;
    if (!render_source(flags.source, content, body, failure))
    {
        return refuse(alloc, flags.action, failure, flags.source);
    }
    if (!write_out(flags.out, cont::as_const_span(body)))
    {
        return refuse(alloc, flags.action, "unwritable", flags.out);
    }
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", true);
    w.kv("action", "body");
    w.kv("out", flags.out);
    write_digest(w, cont::as_const_span(body));
    write_content(w, content, nullptr, 0U);
    w.end_object();
    return cont::String(w.str());
}

[[nodiscard]] cont::String policy_assemble(const PolicyFlags& flags, crd::memory::IAllocator* alloc)
{
    if (flags.in.empty() || flags.out == nullptr)
    {
        return refuse(alloc, flags.action, "assemble needs --in (one or more) and --out");
    }
    if (flags.in.size() > crypto::kMaxPolicySignatures)
    {
        return refuse(alloc, flags.action, crypto::policy_error_name(PolicyError::TooLarge));
    }
    cont::Array<crd::u8> files[crypto::kMaxPolicySignatures];
    crypto::PolicyMessage messages[crypto::kMaxPolicySignatures];
    crypto::KeySignature signatures[crypto::kMaxPolicySignatures];
    crd::u32 count = 0U;
    for (crd::usize i = 0U; i < flags.in.size(); ++i)
    {
        if (!read_file(flags.in[i], crypto::kMaxPolicyBytes, files[i]))
        {
            return refuse(alloc, flags.action, "unreadable", flags.in[i]);
        }
        const PolicyError decoded = crypto::decode_policy(cont::as_const_span(files[i]), messages[i]);
        if (decoded != PolicyError::None)
        {
            return refuse(alloc, flags.action, crypto::policy_error_name(decoded), flags.in[i]);
        }
        const cont::ConstSpan<crd::u8> body = messages[i].signed_bytes;
        const cont::ConstSpan<crd::u8> first = messages[0].signed_bytes;
        if (body.size() != first.size() || std::memcmp(body.data(), first.data(), body.size()) != 0)
        {
            return refuse(alloc, flags.action, "different-bodies", flags.in[i]);
        }
        if (count + messages[i].signature_count > crypto::kMaxPolicySignatures)
        {
            return refuse(alloc, flags.action, crypto::policy_error_name(PolicyError::TooLarge), flags.in[i]);
        }
        for (crd::u32 s = 0U; s < messages[i].signature_count; ++s)
        {
            signatures[count++] = messages[i].signatures[s];
        }
    }
    cont::Array<crd::u8> joined;
    joined.resize(crypto::kMaxPolicyBytes);
    crd::usize written = 0U;
    const PolicyError error =
        crypto::write_policy(messages[0].signed_bytes, cont::ConstSpan<crypto::KeySignature>(signatures, count),
                             cont::Span<crd::u8>(joined.data(), joined.size()), written);
    if (error != PolicyError::None)
    {
        return refuse(alloc, flags.action, crypto::policy_error_name(error));
    }
    joined.resize(written);
    crypto::PolicyMessage message;
    const PolicyError decoded = crypto::decode_policy(cont::as_const_span(joined), message);
    if (decoded != PolicyError::None)
    {
        return refuse(alloc, flags.action, crypto::policy_error_name(decoded));
    }
    if (!write_out(flags.out, cont::as_const_span(joined)))
    {
        return refuse(alloc, flags.action, "unwritable", flags.out);
    }
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", true);
    w.kv("action", "assemble");
    w.kv("out", flags.out);
    write_digest(w, cont::as_const_span(joined));
    w.kv("generation", message.content.generation);
    write_signatures(w, message, nullptr);
    w.end_object();
    return cont::String(w.str());
}

[[nodiscard]] cont::String policy_verify(const PolicyFlags& flags, crd::memory::IAllocator* alloc)
{
    if (flags.generation == nullptr || flags.trust == nullptr || flags.now == nullptr)
    {
        return refuse(alloc, flags.action, "verify needs --generation, --trust and --now");
    }
    crd::u64 now = 0U;
    crd::u64 floor = 0U;
    if (!read_now(flags.now, now))
    {
        return refuse(alloc, flags.action, "--now must be YYYY-MM-DD or Unix seconds");
    }
    if (flags.floor != nullptr && !read_decimal(cont::StringView(flags.floor), floor))
    {
        return refuse(alloc, flags.action, "--floor must be a generation number");
    }
    TrustSet trust;
    Failure failure;
    if (!read_trust(flags.trust, trust, failure))
    {
        return refuse(alloc, flags.action, failure, flags.trust);
    }
    cont::Array<crd::u8> bytes;
    if (!read_file(flags.generation, crypto::kMaxPolicyBytes, bytes))
    {
        return refuse(alloc, flags.action, "unreadable", flags.generation);
    }
    crypto::PolicyMessage message;
    const PolicyError decoded = crypto::decode_policy(cont::as_const_span(bytes), message);
    if (decoded != PolicyError::None)
    {
        return refuse(alloc, flags.action, crypto::policy_error_name(decoded), flags.generation);
    }
    if (flags.source != nullptr)
    {
        PolicyContent content;
        cont::Array<crd::u8> body;
        if (!render_source(flags.source, content, body, failure))
        {
            return refuse(alloc, flags.action, failure, flags.source);
        }
        const cont::ConstSpan<crd::u8> signed_bytes = message.signed_bytes;
        if (body.size() != signed_bytes.size() || std::memcmp(body.data(), signed_bytes.data(), body.size()) != 0)
        {
            return refuse(alloc, flags.action, "source-mismatch", flags.source);
        }
    }
    // A product layer (decision S25): the layer it tightens, judged first at the same time and under its own trust set.
    const bool layered = flags.base_generation != nullptr || flags.base_trust != nullptr;
    TrustSet base_trust;
    cont::Array<crd::u8> base_bytes;
    if (layered)
    {
        if (flags.base_generation == nullptr || flags.base_trust == nullptr)
        {
            return refuse(alloc, flags.action, "--base-generation and --base-trust go together");
        }
        if (!read_trust(flags.base_trust, base_trust, failure))
        {
            return refuse(alloc, flags.action, failure, flags.base_trust);
        }
        if (!read_file(flags.base_generation, crypto::kMaxPolicyBytes, base_bytes))
        {
            return refuse(alloc, flags.action, "unreadable", flags.base_generation);
        }
    }
    crypto::ProviderVerifier verifier;
    const crypto::PolicyConfig base_config{cont::ConstSpan<crypto::TrustKey>(base_trust.keys, base_trust.count),
                                           &verifier, crypto::provider_algorithms(), 0U};
    if (layered && crypto::validate_config(base_config) != PolicyError::None)
    {
        return refuse(alloc, flags.action, crypto::policy_error_name(PolicyError::BadConfig), flags.base_trust);
    }
    crypto::PolicyStore base_store(base_config);
    const PolicyError base_error = layered ? base_store.apply(cont::as_const_span(base_bytes), now) : PolicyError::None;
    const crypto::PolicyConfig config{cont::ConstSpan<crypto::TrustKey>(trust.keys, trust.count), &verifier,
                                      crypto::provider_algorithms(), floor, layered ? &base_store : nullptr};
    if (crypto::validate_config(config) != PolicyError::None)
    {
        return refuse(alloc, flags.action, crypto::policy_error_name(PolicyError::BadConfig), flags.trust);
    }
    crypto::PolicyStore store(config);
    const PolicyError error = store.apply(cont::as_const_span(bytes), now);
    const crypto::PolicySnapshot snapshot = store.snapshot();
    char text[32];
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", error == PolicyError::None && base_error == PolicyError::None);
    w.kv("action", "verify");
    w.kv("error", crypto::policy_error_name(error));
    instant_text(now, text);
    w.kv("now", static_cast<const char*>(text));
    w.kv("floor", store.floor_generation());
    w.kv("freshness", kFreshness[static_cast<crd::u8>(snapshot.freshness(now))]);
    if (layered)
    {
        w.key("base");
        w.begin_object();
        w.kv("error", crypto::policy_error_name(base_error));
        w.kv("generation", snapshot.base_generation());
        w.kv("freshness", kFreshness[static_cast<crd::u8>(base_store.snapshot().freshness(now))]);
        write_digest(w, cont::as_const_span(base_bytes));
        w.end_object();
    }
    write_digest(w, cont::as_const_span(bytes));
    write_content(w, message.content, error == PolicyError::None ? &snapshot : nullptr, now);
    if (layered)
    {
        write_loosenings(w, snapshot);
    }
    write_signatures(w, message, &trust);
    w.end_object();
    return cont::String(w.str());
}

// A key holder's signature key: generation 1 of --name in the store at --store (made there when it has none), or the
// key it already holds under that name, algorithm and placement; reported with its trust set entry.
[[nodiscard]] cont::String policy_key(const PolicyFlags& flags, crd::memory::IAllocator* alloc)
{
    if (flags.store == nullptr || flags.name == nullptr || flags.algorithm == nullptr || flags.protector == nullptr)
    {
        return refuse(alloc, flags.action, "key needs --store, --name, --algorithm and a --protector");
    }
    AlgorithmId algorithm{};
    crypto::KeyKind kind = crypto::KeyKind::Secret;
    if (!algorithm_named(flags.algorithm, algorithm) || !crypto::key_kind_of(algorithm, kind) ||
        kind != crypto::KeyKind::Signature)
    {
        return refuse(alloc, flags.action, "not-a-signature-algorithm");
    }
    crypto::PlacementRequest placement = crypto::PlacementRequest::Software;
    if (!placement_named(cont::StringView(flags.placement != nullptr ? flags.placement : ""), placement))
    {
        return refuse(alloc, flags.action, "--placement must be software, hardware-preferred or hardware-required");
    }
    const cont::StringView name(flags.name);
    if (!crypto::valid_key_name(name))
    {
        return refuse(alloc, flags.action, "bad-key-name");
    }
    crypto::KeyStore store;
    crypto::CryptoError error = open_store(flags, true, store);
    if (error != crypto::CryptoError::None)
    {
        return refuse(alloc, flags.action, crypto::crypto_error_name(error), flags.store);
    }
    const crypto::KeyInfo* info = store.find(name);
    const bool made = info == nullptr;
    if (made)
    {
        error = store.generate(name, algorithm, placement, static_cast<crd::u64>(std::time(nullptr)));
        if (error != crypto::CryptoError::None)
        {
            return refuse(alloc, flags.action, crypto::crypto_error_name(error));
        }
        info = store.find(name);
        if (info == nullptr)
        {
            return refuse(alloc, flags.action, crypto::crypto_error_name(crypto::CryptoError::NotFound));
        }
    }
    if (info->algorithm != algorithm)
    {
        return refuse(alloc, flags.action, "name-holds-another-algorithm");
    }
    if (!placement_answers(placement, info->placement))
    {
        return refuse(alloc, flags.action, "name-holds-another-placement");
    }
    crd::u8 public_key[kMaxPublicKeyBytes];
    const cont::Span<crd::u8> public_view(public_key, crypto::signature_sizes(algorithm).public_key);
    error = store.public_key(name, info->generation, public_view);
    if (error != crypto::CryptoError::None)
    {
        return refuse(alloc, flags.action, crypto::crypto_error_name(error));
    }
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", true);
    w.kv("action", "key");
    w.kv("store", flags.store);
    w.kv("protection", crypto::key_protection_name(store.protection()));
    w.kv("made", made);
    write_key(w, *info, public_view);
    w.end_object();
    return cont::String(w.str());
}

// One key holder's signature: the body --source renders (or the body file --body) signed by --name of the store at
// --store, written as a one-signature generation for assemble. The signature is checked under the stored public key
// and the generation decoded before anything is written.
[[nodiscard]] cont::String policy_sign(const PolicyFlags& flags, crd::memory::IAllocator* alloc)
{
    if (flags.store == nullptr || flags.name == nullptr || flags.out == nullptr || flags.protector == nullptr ||
        (flags.source == nullptr) == (flags.body == nullptr))
    {
        return refuse(alloc, flags.action, "sign needs --store, --name, --out, a --protector and --source or --body");
    }
    cont::Array<crd::u8> body;
    if (flags.source != nullptr)
    {
        PolicyContent content;
        Failure failure;
        if (!render_source(flags.source, content, body, failure))
        {
            return refuse(alloc, flags.action, failure, flags.source);
        }
    }
    else if (!read_file(flags.body, crypto::kMaxPolicyBytes, body))
    {
        return refuse(alloc, flags.action, "unreadable", flags.body);
    }
    PolicyError policy = crypto::check_policy_body(cont::as_const_span(body));
    if (policy != PolicyError::None)
    {
        return refuse(alloc, flags.action, crypto::policy_error_name(policy), flags.body); // nothing is signed
    }
    crypto::KeyStore store;
    crypto::CryptoError error = open_store(flags, false, store);
    if (error != crypto::CryptoError::None)
    {
        return refuse(alloc, flags.action, crypto::crypto_error_name(error), flags.store);
    }
    crypto::StoredSigner signer;
    error = store.signer(cont::StringView(flags.name), signer);
    if (error != crypto::CryptoError::None)
    {
        return refuse(alloc, flags.action, crypto::crypto_error_name(error));
    }
    crd::u8 public_key[kMaxPublicKeyBytes];
    const cont::Span<crd::u8> public_view(public_key, crypto::signature_sizes(signer.algorithm()).public_key);
    crd::u8 signature[crypto::kMaxSignatureBytes];
    crd::usize signature_size = 0U;
    const cont::ConstSpan<crd::u8> message = cont::as_const_span(body);
    error = signer.public_key(public_view);
    if (error == crypto::CryptoError::None)
    {
        error = signer.sign(message, cont::Span<crd::u8>(signature), signature_size);
    }
    if (error != crypto::CryptoError::None)
    {
        return refuse(alloc, flags.action, crypto::crypto_error_name(error));
    }
    const cont::ConstSpan<crd::u8> signed_view(signature, signature_size);
    const crypto::ProviderVerifier verifier;
    if (verifier.verify(signer.algorithm(), public_view, message, signed_view) != crypto::Verification::Valid)
    {
        return refuse(alloc, flags.action, "signature-does-not-verify");
    }
    const crypto::KeySignature entry{crypto::key_id_of(public_view), signer.algorithm(), signed_view};
    cont::Array<crd::u8> generation;
    generation.resize(crypto::kMaxPolicyBytes);
    crd::usize written = 0U;
    policy = crypto::write_policy(message, cont::ConstSpan<crypto::KeySignature>(&entry, 1U),
                                  cont::Span<crd::u8>(generation.data(), generation.size()), written);
    generation.resize(written);
    crypto::PolicyMessage decoded;
    if (policy == PolicyError::None)
    {
        policy = crypto::decode_policy(cont::as_const_span(generation), decoded);
    }
    if (policy != PolicyError::None)
    {
        return refuse(alloc, flags.action, crypto::policy_error_name(policy));
    }
    if (!write_out(flags.out, cont::as_const_span(generation)))
    {
        return refuse(alloc, flags.action, "unwritable", flags.out);
    }
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", true);
    w.kv("action", "sign");
    w.kv("out", flags.out);
    write_digest(w, cont::as_const_span(generation));
    write_content(w, decoded.content, nullptr, 0U); // what the key holder signed
    write_signatures(w, decoded, nullptr);
    write_key(w, signer.info(), public_view);
    w.end_object();
    return cont::String(w.str());
}
} // namespace

cont::String verb_policy(const PolicyFlags& flags, crd::memory::IAllocator* alloc)
{
    const cont::StringView action(flags.action != nullptr ? flags.action : "");
    if (action == "body")
    {
        return policy_body(flags, alloc);
    }
    if (action == "assemble")
    {
        return policy_assemble(flags, alloc);
    }
    if (action == "verify")
    {
        return policy_verify(flags, alloc);
    }
    if (action == "key")
    {
        return policy_key(flags, alloc);
    }
    if (action == "sign")
    {
        return policy_sign(flags, alloc);
    }
    return refuse(alloc, flags.action, "the action must be body, assemble, verify, key or sign");
}
} // namespace crd::ceridc
