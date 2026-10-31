// ceridc comply and ceridc screen -- crd-comply's queries on the command line (COMPLY.0 and COMPLY.1; contract:
// docs/design/registers.md#rule-generations and #screening). The stores, the decoders, the query and the screening
// are crd-comply's; these verbs only validate the request, load the requested files and write the answer as JSON.

#include <crd/assetio/json_write.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/comply/rules.hpp>
#include <crd/comply/sanctions.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstdio>

namespace crd::ceridc
{
namespace
{
namespace comply = crd::comply;
namespace cont = crd::containers;
using crd::assetio::JsonWriter;

constexpr const char* kScopeStates[] = {"planned", "live"};
constexpr const char* kRuleKinds[] = {"obligation", "user-action"};
constexpr const char* kRuleStates[] = {"planned", "implemented"};
constexpr const char* kChecks[] = {"planned", "active"};
constexpr const char* kCounsel[] = {"none", "needed", "reviewed"};
constexpr const char* kProofs[] = {"proven", "unproven"};

[[nodiscard]] cont::String refuse(crd::memory::IAllocator* alloc, const char* verb, const char* reason)
{
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("ok", false);
    w.kv("verb", verb);
    w.kv("reason", reason);
    w.end_object();
    return cont::String(w.str());
}

// A refusal that names the module's error and key: a file's line, or line 0 for the request itself.
[[nodiscard]] cont::String refuse_decode(crd::memory::IAllocator* alloc, const char* verb, const char* reason,
                                         cont::StringView file, const comply::DecodeFailure& failure)
{
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("ok", false);
    w.kv("verb", verb);
    w.kv("reason", reason);
    if (!file.empty())
    {
        w.kv("file", file);
    }
    w.kv("error", comply::comply_error_name(failure.error));
    w.kv("line", failure.line);
    w.kv("key", failure.key);
    w.end_object();
    return cont::String(w.str());
}

// `day` as a calendar day, exactly YYYY-MM-DD.
[[nodiscard]] bool read_day(const char* day, comply::Day& out)
{
    return cont::StringView(day).size() == 10U && comply::parse_day(day, out);
}

// The items of a comma-separated list, none empty and at most `max` of them.
[[nodiscard]] bool split_list(const char* text, crd::usize max, cont::Array<cont::StringView>& out)
{
    for (cont::StringView rest(text); !rest.empty();)
    {
        const crd::usize comma = rest.find(',');
        out.push_back(rest.substr(0U, comma));
        rest = comma == cont::StringView::npos ? cont::StringView() : rest.substr(comma + 1U);
        if (out.back().empty() || (comma != cont::StringView::npos && rest.empty()) || out.size() > max)
        {
            return false;
        }
    }
    return !out.empty();
}

// <dir>/<name><suffix>, the name lower-cased (jurisdiction codes are upper case, file names lower case).
[[nodiscard]] cont::String file_in(const char* dir, cont::StringView name, const char* suffix,
                                   crd::memory::IAllocator* alloc)
{
    cont::String path(dir, alloc);
    path.append("/");
    for (const char c : name)
    {
        path.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    path.append(suffix);
    return path;
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

// A threshold in thousandths: one to four decimal digits (the module refuses one above 1000 or out of order).
[[nodiscard]] bool read_threshold(const char* text, crd::u32& out)
{
    if (text == nullptr)
    {
        return true; // the policy's default
    }
    const cont::StringView digits(text);
    crd::u32 value = 0U;
    for (const char c : digits)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        value = value * 10U + static_cast<crd::u32>(c - '0');
    }
    out = value;
    return !digits.empty() && digits.size() <= 4U;
}

void write_hits(JsonWriter& w, const comply::Screening& screening)
{
    w.key("gaps");
    w.begin_array();
    for (const comply::ListGap& gap : screening.gaps)
    {
        w.begin_object();
        w.kv("authority", cont::StringView(gap.authority));
        w.kv("gap", comply::gap_name(gap.gap));
        w.end_object();
    }
    w.end_array();
    w.key("parties");
    w.begin_array();
    for (const comply::PartyHit& hit : screening.parties)
    {
        w.begin_object();
        w.kv("authority", cont::StringView(hit.authority));
        w.kv("generation", hit.generation);
        w.kv("party", cont::StringView(hit.party));
        w.kv("name", cont::StringView(hit.name));
        w.kv("score", hit.score);
        w.kv("disposition", comply::disposition_name(hit.disposition));
        w.kv("birth_agrees", hit.birth_agrees);
        w.kv("country_agrees", hit.country_agrees);
        w.kv("identifier_agrees", hit.identifier_agrees);
        w.end_object();
    }
    w.end_array();
    w.key("destinations");
    w.begin_array();
    for (const comply::DestinationHit& hit : screening.destinations)
    {
        w.begin_object();
        w.kv("authority", cont::StringView(hit.authority));
        w.kv("generation", hit.generation);
        w.kv("region", cont::StringView(hit.destination.region));
        w.kv("program", cont::StringView(hit.destination.program));
        w.kv("measure", comply::measure_name(hit.destination.measure));
        w.kv("disposition", comply::disposition_name(hit.disposition));
        w.end_object();
    }
    w.end_array();
}

[[nodiscard]] cont::Array<cont::StringView> views_of(cont::ConstSpan<const char*> texts, crd::memory::IAllocator* alloc)
{
    cont::Array<cont::StringView> out(alloc);
    for (const char* text : texts)
    {
        out.push_back(cont::StringView(text));
    }
    return out;
}
} // namespace

cont::String verb_comply(const char* rules_dir, const char* scope, const char* jurisdictions, const char* day,
                         crd::memory::IAllocator* alloc)
{
    constexpr const char* verb = "comply";
    // Everything is validated before the first file is read.
    if (rules_dir == nullptr || scope == nullptr || jurisdictions == nullptr || day == nullptr)
    {
        return refuse(alloc, verb, "comply needs --rules, --scope, --jurisdiction and --day");
    }
    comply::Day query_day = comply::kNoDay;
    if (!read_day(day, query_day))
    {
        return refuse(alloc, verb, "--day must be a YYYY-MM-DD calendar day");
    }
    cont::Array<cont::StringView> codes(alloc);
    if (!split_list(jurisdictions, comply::kMaxJurisdictions, codes))
    {
        return refuse(alloc, verb,
                      "--jurisdiction is a comma-separated list of codes without empty items, at most as many as a "
                      "store holds");
    }

    comply::RuleStore store;
    cont::Array<crd::u64> generations(alloc); // per requested code; 0 when it has no generation file
    for (const cont::StringView code : codes)
    {
        const cont::String path = file_in(rules_dir, code, ".rules.toml", alloc);
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
            return refuse_decode(alloc, verb, "a rule generation was refused", cont::StringView(path), failure);
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
    w.kv("verb", verb);
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

cont::String verb_screen(const ScreenFlags& flags, crd::memory::IAllocator* alloc)
{
    constexpr const char* verb = "screen";
    // Everything is validated before the first file is read.
    if (flags.lists == nullptr || flags.authorities == nullptr || flags.day == nullptr)
    {
        return refuse(alloc, verb, "screen needs --lists, --authority and --day");
    }
    comply::ScreenRequest request;
    if (!read_day(flags.day, request.day))
    {
        return refuse(alloc, verb, "--day must be a YYYY-MM-DD calendar day");
    }
    cont::Array<cont::StringView> authorities(alloc);
    if (!split_list(flags.authorities, comply::kMaxLists, authorities))
    {
        return refuse(alloc, verb,
                      "--authority is a comma-separated list of authorities without empty items, at most as many as "
                      "a store holds");
    }
    request.authorities = cont::ConstSpan<cont::StringView>(authorities.data(), authorities.size());
    const bool described =
        flags.kind != nullptr || flags.birth != nullptr || !flags.countries.empty() || !flags.identifiers.empty();
    if (flags.names.empty() && described)
    {
        return refuse(alloc, verb, "--kind, --birth, --country and --identifier describe a subject --name names");
    }
    if (flags.names.empty() && flags.destination == nullptr)
    {
        return refuse(alloc, verb, "screen needs a subject (--name) or a --destination");
    }
    // A subject's kind is never assumed: a wrong one discounts the very party that matches it.
    comply::Subject subject;
    if (!flags.names.empty() && (flags.kind == nullptr || !comply::parse_party_kind(flags.kind, subject.kind)))
    {
        return refuse(alloc, verb, "--name needs --kind individual, entity, vessel or aircraft");
    }
    if (!read_threshold(flags.review, request.policy.review) || !read_threshold(flags.match, request.policy.match))
    {
        return refuse(alloc, verb, "--review and --match are thresholds in thousandths");
    }
    const cont::Array<cont::StringView> names = views_of(flags.names, alloc);
    const cont::Array<cont::StringView> countries = views_of(flags.countries, alloc);
    const cont::Array<cont::StringView> identifiers = views_of(flags.identifiers, alloc);
    subject.names = cont::ConstSpan<cont::StringView>(names.data(), names.size());
    subject.birth = flags.birth == nullptr ? cont::StringView() : cont::StringView(flags.birth);
    subject.countries = cont::ConstSpan<cont::StringView>(countries.data(), countries.size());
    subject.identifiers = cont::ConstSpan<cont::StringView>(identifiers.data(), identifiers.size());
    request.subject = names.empty() ? nullptr : &subject;
    request.destination = flags.destination == nullptr ? cont::StringView() : cont::StringView(flags.destination);

    // The module's own request rules, run against an empty store, judge the rest of the request (the authorities'
    // shape among them, which keeps every file name below inside --lists).
    comply::SanctionsStore store;
    comply::Screening screening;
    const comply::DecodeFailure malformed = store.screen(request, screening);
    if (malformed.error != comply::ComplyError::None)
    {
        return refuse_decode(alloc, verb, "the screening request was refused", cont::StringView(), malformed);
    }

    // A list file that cannot be read is a missing list: the screening fails closed on it.
    for (const cont::StringView authority : authorities)
    {
        if (store.generation(authority) != 0U)
        {
            continue; // named twice
        }
        const cont::String path = file_in(flags.lists, authority, ".sanctions.toml", alloc);
        cont::Array<crd::u8> bytes(alloc);
        if (!crd::platform::fs::read_file_binary(crd::platform::fs::Path(cont::StringView(path)), bytes))
        {
            continue;
        }
        const comply::DecodeFailure failure =
            store.apply(cont::StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size())); // NOLINT
        if (failure.error != comply::ComplyError::None)
        {
            return refuse_decode(alloc, verb, "a sanctions list was refused", cont::StringView(path), failure);
        }
        if (store.generation(authority) == 0U)
        {
            JsonWriter w(alloc);
            w.begin_object();
            w.kv("ok", false);
            w.kv("verb", verb);
            w.kv("reason", "a sanctions list file holds another authority's list");
            w.kv("file", cont::StringView(path));
            w.kv("authority", authority);
            w.end_object();
            return cont::String(w.str());
        }
    }
    (void)store.screen(request, screening); // only the request can be refused, and it passed above

    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", verb);
    w.kv("ok", true);
    w.kv("day", flags.day);
    w.key("lists");
    w.begin_array();
    for (crd::usize i = 0U; i < authorities.size(); ++i)
    {
        w.begin_object();
        w.kv("authority", authorities[i]);
        w.kv("present", store.generation(authorities[i]) != 0U);
        w.kv("generation", store.generation(authorities[i]));
        w.end_object();
    }
    w.end_array();
    w.kv("verdict", comply::verdict_name(screening.verdict));
    write_hits(w, screening);
    w.end_object();
    return cont::String(w.str());
}
} // namespace crd::ceridc
