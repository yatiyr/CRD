// ceridc comply -- the rule generations' query on the command line (COMPLY.0; contract:
// docs/design/registers.md#rule-generations). The store, the decoder and the query are crd-comply's; this verb only
// loads the requested jurisdictions' files and writes the answer as JSON.

#include <crd/assetio/json_write.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/comply/rules.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstdio>

namespace crd::ceridc
{
namespace
{
namespace comply = crd::comply;
namespace cont = crd::containers;
using crd::assetio::JsonWriter;

constexpr cont::StringView kVerb = "comply";

constexpr const char* kScopeStates[] = {"planned", "live"};
constexpr const char* kRuleKinds[] = {"obligation", "user-action"};
constexpr const char* kRuleStates[] = {"planned", "implemented"};
constexpr const char* kChecks[] = {"planned", "active"};
constexpr const char* kCounsel[] = {"none", "needed", "reviewed"};
constexpr const char* kProofs[] = {"proven", "unproven"};

[[nodiscard]] cont::String refuse(crd::memory::IAllocator* alloc, const char* reason)
{
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("ok", false);
    w.kv("verb", kVerb);
    w.kv("reason", reason);
    w.end_object();
    return cont::String(w.str());
}

// The day yyyymmdd as YYYY-MM-DD (kNoDay as the empty string).
void day_text(comply::Day day, char (&out)[16])
{
    out[0] = '\0';
    if (day != comply::kNoDay)
    {
        std::snprintf(out, sizeof(out), "%04u-%02u-%02u", day / 10000U, (day / 100U) % 100U, day % 100U);
    }
}

void write_binding(JsonWriter& w, const comply::Binding& binding)
{
    const comply::Rule& rule = binding.rule;
    char day[16];
    w.begin_object();
    w.kv("jurisdiction", cont::StringView(binding.jurisdiction));
    w.kv("generation", binding.generation);
    w.kv("id", cont::StringView(rule.id));
    w.kv("kind", kRuleKinds[static_cast<crd::u8>(rule.kind)]);
    w.kv("state", kRuleStates[static_cast<crd::u8>(rule.state)]);
    w.kv("proof", kProofs[static_cast<crd::u8>(binding.proof)]);
    w.kv("scope_state", kScopeStates[static_cast<crd::u8>(binding.scope_state)]);
    w.kv("topic", cont::StringView(rule.topic));
    w.kv("title", cont::StringView(rule.title));
    w.kv("instrument", cont::StringView(rule.instrument));
    w.kv("article", cont::StringView(rule.article));
    w.kv("primary", cont::StringView(rule.primary));
    w.kv("read", rule.read);
    w.kv("obligation", cont::StringView(rule.obligation));
    w.key("check");
    w.begin_object();
    w.kv("state", kChecks[static_cast<crd::u8>(rule.check)]);
    w.kv("name", cont::StringView(rule.check_name));
    w.kv("ref", cont::StringView(rule.check_ref));
    w.end_object();
    day_text(rule.effective, day);
    w.kv("effective", day);
    day_text(rule.sunset, day);
    w.kv("sunset", day);
    w.key("counsel");
    w.begin_object();
    w.kv("status", kCounsel[static_cast<crd::u8>(rule.counsel)]);
    day_text(rule.counsel_reviewed, day);
    w.kv("reviewed", day);
    w.end_object();
    w.key("rows");
    w.begin_array();
    for (const cont::String& row : rule.rows)
    {
        w.value_string(cont::StringView(row));
    }
    w.end_array();
    w.end_object();
}
} // namespace

cont::String verb_comply(const char* rules_dir, const char* scope, const char* jurisdictions, const char* day,
                         crd::memory::IAllocator* alloc)
{
    // Everything is validated before the first file is read.
    if (rules_dir == nullptr || scope == nullptr || jurisdictions == nullptr || day == nullptr)
    {
        return refuse(alloc, "comply needs --rules, --scope, --jurisdiction and --day");
    }
    comply::Day query_day = comply::kNoDay;
    if (cont::StringView(day).size() != 10U || !comply::parse_day(day, query_day))
    {
        return refuse(alloc, "--day must be a YYYY-MM-DD calendar day");
    }
    cont::Array<cont::StringView> codes(alloc);
    for (cont::StringView rest(jurisdictions); !rest.empty();)
    {
        const crd::usize comma = rest.find(',');
        codes.push_back(rest.substr(0U, comma));
        rest = comma == cont::StringView::npos ? cont::StringView() : rest.substr(comma + 1U);
        if (codes.back().empty() || (comma != cont::StringView::npos && rest.empty()))
        {
            return refuse(alloc, "--jurisdiction is a comma-separated list of codes without empty items");
        }
        if (codes.size() > comply::kMaxJurisdictions)
        {
            return refuse(alloc, "--jurisdiction names more jurisdictions than a store holds");
        }
    }
    if (codes.empty())
    {
        return refuse(alloc, "--jurisdiction names no jurisdiction");
    }

    comply::RuleStore store;
    cont::Array<crd::u64> generations(alloc); // per requested code; 0 when it has no generation file
    for (const cont::StringView code : codes)
    {
        cont::String path(rules_dir, alloc);
        path.append("/");
        for (const char c : code)
        {
            path.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
        path.append(".rules.toml");
        cont::Array<crd::u8> bytes(alloc);
        if (!crd::platform::fs::read_file_binary(crd::platform::fs::Path(cont::StringView(path)), bytes))
        {
            generations.push_back(0U);
            continue;
        }
        const comply::DecodeFailure failure =
            store.apply(cont::StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size())); // NOLINT
        if (failure.error != comply::ComplyError::None && failure.error != comply::ComplyError::AlreadyCurrent)
        {
            JsonWriter w(alloc);
            w.begin_object();
            w.kv("ok", false);
            w.kv("verb", kVerb);
            w.kv("reason", "a rule generation was refused");
            w.kv("file", cont::StringView(path));
            w.kv("error", comply::comply_error_name(failure.error));
            w.kv("line", failure.line);
            w.kv("key", failure.key);
            w.end_object();
            return cont::String(w.str());
        }
        generations.push_back(store.generation(code));
    }

    cont::Array<comply::Binding> bindings(alloc);
    store.query(comply::RuleQuery{scope, cont::ConstSpan<cont::StringView>(codes.data(), codes.size()), query_day},
                bindings);
    crd::u64 proven = 0U;
    for (const comply::Binding& binding : bindings)
    {
        proven += binding.proof == comply::Proof::Proven ? 1U : 0U;
    }

    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", true);
    w.kv("scope", scope);
    w.kv("day", day);
    w.key("jurisdictions");
    w.begin_array();
    for (crd::usize i = 0U; i < codes.size(); ++i)
    {
        w.begin_object();
        w.kv("code", codes[i]);
        w.kv("present", generations[i] != 0U);
        w.kv("generation", generations[i]);
        w.end_object();
    }
    w.end_array();
    w.kv("count", static_cast<crd::u64>(bindings.size()));
    w.kv("proven", proven);
    w.kv("unproven", static_cast<crd::u64>(bindings.size()) - proven);
    w.key("bindings");
    w.begin_array();
    for (const comply::Binding& binding : bindings)
    {
        write_binding(w, binding);
    }
    w.end_array();
    w.end_object();
    return cont::String(w.str());
}
} // namespace crd::ceridc
