#include "decoder.hpp"

#include <crd/comply/rules.hpp>

#include <algorithm>

namespace crd::comply
{
namespace
{
using detail::is_alnum;
using detail::is_dotted;
using detail::is_lower_alnum;
using detail::kNotFound;
using detail::node;

constexpr cont::StringView kFormat = "cerid-rules/1";

constexpr cont::StringView kTopKeys[] = {"format", "jurisdiction", "generation", "register", "scope", "rule"};
constexpr cont::StringView kScopeKeys[] = {"id", "kind", "state"};
constexpr cont::StringView kRuleKeys[] = {
    "id",         "kind",      "topic",     "state",      "title",      "instrument",
    "article",    "primary",   "read",      "obligation", "applies_to", "check",
    "check_name", "check_ref", "effective", "sunset",     "counsel",    "counsel_reviewed",
    "rows"};

constexpr cont::StringView kRuleKindNames[] = {"obligation", "user-action"};
constexpr cont::StringView kRuleStateNames[] = {"planned", "implemented"};
constexpr cont::StringView kScopeKindNames[] = {"product", "flow", "channel"};
constexpr cont::StringView kScopeStateNames[] = {"planned", "live"};
constexpr cont::StringView kCheckStateNames[] = {"planned", "active"};
constexpr cont::StringView kCounselNames[] = {"none", "needed", "reviewed"};

class Decoder : public detail::TomlDecoder
{
public:
    explicit Decoder(RuleGeneration& out) noexcept : m_out(out) {}

    [[nodiscard]] DecodeFailure run(cont::StringView text)
    {
        toml::parse_result parsed;
        const node* root = parse(parsed, text, kMaxRulesBytes, "rules");
        if (root == nullptr || !known_keys(*root, kTopKeys) || !top(*root) || !scopes(*root) || !rules(*root))
        {
            return m_failure;
        }
        m_out.digest = detail::digest_of(text);
        return m_failure;
    }

private:
    bool top(const node& root)
    {
        cont::StringView jurisdiction;
        if (!format(root, kFormat) || !string_at(root, "jurisdiction", jurisdiction))
        {
            return false;
        }
        if (!detail::is_jurisdiction(jurisdiction))
        {
            return refuse(ComplyError::BadValue, root.get("jurisdiction"), "jurisdiction");
        }
        m_out.jurisdiction = cont::String(jurisdiction);
        return generation(root, m_out.number) && day(root, "register", m_out.register_day, true);
    }

    bool scopes(const node& root)
    {
        const node* list_node = tables(root, "scope", kMaxScopes);
        if (list_node == nullptr)
        {
            return false;
        }
        for (const node& table : *list_node)
        {
            Scope scope;
            cont::StringView id;
            if (!known_keys(table, kScopeKeys) || !string_at(table, "id", id) ||
                !enumeration(table, "kind", kScopeKindNames, scope.kind) ||
                !enumeration(table, "state", kScopeStateNames, scope.state))
            {
                return false;
            }
            if (!is_dotted(id, is_lower_alnum))
            {
                return refuse(ComplyError::BadValue, table.get("id"), "id");
            }
            if (find_scope(id) != kNotFound)
            {
                return refuse(ComplyError::DuplicateScope, table.get("id"), "id");
            }
            scope.id = id;
            m_out.scopes.push_back(static_cast<Scope&&>(scope));
        }
        return true;
    }

    [[nodiscard]] usize find_scope(cont::StringView id) const noexcept
    {
        for (usize i = 0; i < m_out.scopes.size(); ++i)
        {
            if (cont::StringView(m_out.scopes[i].id) == id)
            {
                return i;
            }
        }
        return kNotFound;
    }

    bool rules(const node& root)
    {
        const node* list_node = tables(root, "rule", kMaxRules);
        if (list_node == nullptr)
        {
            return false;
        }
        return std::ranges::all_of(*list_node,
                                   [this](const node& table) { return known_keys(table, kRuleKeys) && rule(table); });
    }

    bool rule(const node& table)
    {
        Rule& rule = m_out.rules.emplace_back();
        cont::StringView id;
        if (!string_at(table, "id", id))
        {
            return false;
        }
        if (!is_dotted(id, is_lower_alnum))
        {
            return refuse(ComplyError::BadValue, table.get("id"), "id");
        }
        for (usize i = 0; i + 1U < m_out.rules.size(); ++i)
        {
            if (cont::StringView(m_out.rules[i].id) == id)
            {
                return refuse(ComplyError::DuplicateRule, table.get("id"), "id");
            }
        }
        rule.id = cont::String(id);
        cont::StringView topic;
        cont::StringView primary;
        if (!enumeration(table, "kind", kRuleKindNames, rule.kind) ||
            !enumeration(table, "state", kRuleStateNames, rule.state) || !string_at(table, "topic", topic) ||
            !owned(table, "title", rule.title) || !owned(table, "instrument", rule.instrument) ||
            !owned(table, "article", rule.article) || !string_at(table, "primary", primary) ||
            !owned(table, "obligation", rule.obligation) ||
            !enumeration(table, "check", kCheckStateNames, rule.check) ||
            !owned(table, "check_name", rule.check_name) || !owned(table, "check_ref", rule.check_ref) ||
            !enumeration(table, "counsel", kCounselNames, rule.counsel))
        {
            return false;
        }
        if (!is_dotted(topic, is_lower_alnum))
        {
            return refuse(ComplyError::BadValue, table.get("topic"), "topic");
        }
        if (!detail::is_https_url(primary))
        {
            return refuse(ComplyError::BadValue, table.get("primary"), "primary");
        }
        rule.topic = cont::String(topic);
        rule.primary = cont::String(primary);
        return boolean(table, "read", rule.read) && rule_lists(table, rule) && rule_days(table, rule) &&
               rule_proof(table, rule);
    }

    bool rule_lists(const node& table, Rule& rule)
    {
        cont::Array<cont::StringView> names;
        if (!list(
                table, "applies_to", [](cont::StringView s) { return is_dotted(s, is_lower_alnum); }, names,
                kMaxListItems))
        {
            return false;
        }
        for (const cont::StringView name : names)
        {
            const usize index = find_scope(name);
            if (index == kNotFound)
            {
                return refuse(ComplyError::UnknownScope, table.get("applies_to"), "applies_to");
            }
            for (const u32 earlier : rule.applies_to)
            {
                if (earlier == index)
                {
                    return refuse(ComplyError::BadValue, table.get("applies_to"), "applies_to");
                }
            }
            rule.applies_to.push_back(static_cast<u32>(index));
        }
        names.clear();
        if (!list(table, "rows", [](cont::StringView s) { return is_dotted(s, is_alnum); }, names, kMaxListItems))
        {
            return false;
        }
        for (const cont::StringView name : names)
        {
            rule.rows.push_back(cont::String(name));
        }
        return true;
    }

    bool rule_days(const node& table, Rule& rule) noexcept
    {
        if (!day(table, "effective", rule.effective, true) || !day(table, "sunset", rule.sunset, false) ||
            !day(table, "counsel_reviewed", rule.counsel_reviewed, false))
        {
            return false;
        }
        if (rule.sunset != kNoDay && rule.sunset <= rule.effective)
        {
            return refuse(ComplyError::BadDay, table.get("sunset"), "sunset");
        }
        if ((rule.counsel == Counsel::Reviewed) != (rule.counsel_reviewed != kNoDay))
        {
            return refuse(ComplyError::BadDay, table.get("counsel_reviewed"), "counsel_reviewed");
        }
        return true;
    }

    // An implemented rule is the register guard's implemented entry: an active check, its text read, and counsel
    // answered when it was needed. A generation cannot claim a proof the register would refuse.
    bool rule_proof(const node& table, const Rule& rule) noexcept
    {
        if (rule.state == RuleState::Implemented &&
            (rule.check != CheckState::Active || !rule.read || rule.counsel == Counsel::Needed))
        {
            return refuse(ComplyError::BadValue, table.get("state"), "state");
        }
        return true;
    }

    RuleGeneration& m_out;
};

[[nodiscard]] cont::StringView jurisdiction_of(const RuleGeneration& generation) noexcept
{
    return generation.jurisdiction;
}
} // namespace

DecodeFailure decode_rules(cont::StringView text, RuleGeneration& out)
{
    out = RuleGeneration();
    return Decoder(out).run(text);
}

bool in_force(const Rule& rule, Day day) noexcept
{
    return rule.effective != kNoDay && rule.effective <= day && (rule.sunset == kNoDay || day < rule.sunset);
}

Proof proof_of(const Rule& rule) noexcept
{
    return rule.state == RuleState::Implemented && rule.check == CheckState::Active ? Proof::Proven : Proof::Unproven;
}

DecodeFailure RuleStore::apply(cont::StringView text)
{
    RuleGeneration generation;
    const DecodeFailure decoded = decode_rules(text, generation);
    if (decoded.error != ComplyError::None)
    {
        return decoded;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    return detail::adopt(m_generations, static_cast<RuleGeneration&&>(generation), jurisdiction_of, kMaxJurisdictions,
                         "jurisdiction");
}

u64 RuleStore::generation(cont::StringView jurisdiction) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const RuleGeneration* found = detail::find_held(m_generations, jurisdiction, jurisdiction_of);
    return found != nullptr ? found->number : 0U;
}

void RuleStore::query(const RuleQuery& request, cont::Array<Binding>& out) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (usize j = 0; j < request.jurisdictions.size(); ++j)
    {
        const cont::StringView code = request.jurisdictions[j];
        bool repeats = false;
        for (usize k = 0; k < j; ++k)
        {
            repeats = repeats || request.jurisdictions[k] == code;
        }
        const RuleGeneration* generation = repeats ? nullptr : detail::find_held(m_generations, code, jurisdiction_of);
        if (generation == nullptr)
        {
            continue;
        }
        for (u32 s = 0; s < generation->scopes.size(); ++s)
        {
            const Scope& scope = generation->scopes[s];
            if (cont::StringView(scope.id) != request.scope)
            {
                continue;
            }
            for (const Rule& rule : generation->rules)
            {
                bool applies = false;
                for (const u32 index : rule.applies_to)
                {
                    applies = applies || index == s;
                }
                if (applies && in_force(rule, request.day))
                {
                    Binding& binding = out.emplace_back();
                    binding.jurisdiction = generation->jurisdiction;
                    binding.generation = generation->number;
                    binding.scope_state = scope.state;
                    binding.proof = proof_of(rule);
                    binding.rule = rule;
                }
            }
        }
    }
}
} // namespace crd::comply
