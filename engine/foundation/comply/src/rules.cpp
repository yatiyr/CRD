#include <crd/comply/rules.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/toml/toml.hpp>

#include <algorithm>
#include <chrono>

namespace crd::comply
{
namespace
{
using toml::node;

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

[[nodiscard]] bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool is_lower_alnum(char c) noexcept
{
    return is_digit(c) || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_alnum(char c) noexcept
{
    return is_lower_alnum(c) || (c >= 'A' && c <= 'Z');
}

// Runs of `word` characters joined by single '.' or '-': the register's identifiers (lower case) and row names.
template <typename Word> [[nodiscard]] bool is_dotted(cont::StringView text, Word word) noexcept
{
    if (text.empty() || !word(text.front()) || !word(text.back()))
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        const bool joiner = text[i] == '.' || text[i] == '-';
        if (!joiner && !word(text[i]))
        {
            return false;
        }
        if (joiner && (text[i + 1U] == '.' || text[i + 1U] == '-'))
        {
            return false;
        }
    }
    return true;
}

// ISO 3166 with an optional subdivision (TR, US-CA), INT or PLATFORM.
[[nodiscard]] bool is_jurisdiction(cont::StringView text) noexcept
{
    if (text == "INT" || text == "PLATFORM")
    {
        return true;
    }
    const auto upper = [](char c)
    {
        return c >= 'A' && c <= 'Z';
    };
    if (text.size() < 2U || !upper(text[0]) || !upper(text[1]))
    {
        return false;
    }
    if (text.size() == 2U)
    {
        return true;
    }
    if (text[2] != '-' || text.size() < 4U || text.size() > 6U)
    {
        return false;
    }
    for (usize i = 3U; i < text.size(); ++i)
    {
        if (!upper(text[i]) && !is_digit(text[i]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_https_url(cont::StringView text) noexcept
{
    constexpr cont::StringView scheme = "https://";
    if (!text.starts_with(scheme) || text.size() == scheme.size())
    {
        return false;
    }
    for (const char c : text)
    {
        if (static_cast<unsigned char>(c) <= 0x20U || c == 0x7F)
        {
            return false;
        }
    }
    const char first = text[scheme.size()];
    return first != '/' && first != ':' && first != '?' && first != '#';
}

[[nodiscard]] bool read_digits(cont::StringView text, usize from, usize count, u32& out) noexcept
{
    out = 0U;
    for (usize i = from; i < from + count; ++i)
    {
        if (!is_digit(text[i]))
        {
            return false;
        }
        out = (out * 10U) + static_cast<u32>(text[i] - '0');
    }
    return true;
}

constexpr usize kNotFound = ~usize{0U};

class Decoder
{
public:
    explicit Decoder(RuleGeneration& out) noexcept : m_out(out) {}

    [[nodiscard]] DecodeFailure run(cont::StringView text)
    {
        if (text.size() > kMaxRulesBytes)
        {
            return fail(RuleError::TooLarge, nullptr, {});
        }
        const toml::parse_result parsed = toml::parse(text, "rules");
        if (!parsed)
        {
            return DecodeFailure{RuleError::Syntax, parsed.error().line(), {}};
        }
        const node& root = parsed.table();
        if (!known_keys(root, kTopKeys) || !top(root) || !scopes(root) || !rules(root))
        {
            return m_failure;
        }
        const auto* bytes =
            reinterpret_cast<const u8*>(text.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        m_out.digest = cont::xxh3_64(cont::ConstSpan<u8>(bytes, text.size()));
        return m_failure;
    }

private:
    DecodeFailure fail(RuleError error, const node* at, cont::StringView key) noexcept
    {
        if (m_failure.error == RuleError::None)
        {
            m_failure = DecodeFailure{error, at != nullptr ? at->source().begin.line : 0U, key};
        }
        return m_failure;
    }

    bool refuse(RuleError error, const node* at, cont::StringView key) noexcept
    {
        (void)fail(error, at, key);
        return false;
    }

    template <usize N> bool known_keys(const node& table, const cont::StringView (&keys)[N]) noexcept
    {
        for (usize i = 0; i < table.size(); ++i)
        {
            bool known = false;
            for (const cont::StringView key : keys)
            {
                known = known || table.key_at(i) == key;
            }
            if (!known)
            {
                return refuse(RuleError::UnknownKey, &table.child_at(i), {});
            }
        }
        return true;
    }

    // A required string of at most kMaxTextBytes; `may_be_empty` admits "" (an absent day or counsel day).
    bool string_at(const node& table, cont::StringView key, cont::StringView& out, bool may_be_empty = false) noexcept
    {
        const node* value = table.get(key);
        if (value == nullptr)
        {
            return refuse(RuleError::MissingKey, &table, key);
        }
        const auto string = value->value<std::string_view>();
        if (!string.has_value())
        {
            return refuse(RuleError::WrongType, value, key);
        }
        if (string->size() > kMaxTextBytes)
        {
            return refuse(RuleError::TooLarge, value, key);
        }
        if (string->empty() && !may_be_empty)
        {
            return refuse(RuleError::BadValue, value, key);
        }
        out = *string;
        return true;
    }

    bool owned(const node& table, cont::StringView key, cont::String& out)
    {
        cont::StringView view;
        if (!string_at(table, key, view))
        {
            return false;
        }
        out = cont::String(view);
        return true;
    }

    template <typename Enum, usize N>
    bool enumeration(const node& table, cont::StringView key, const cont::StringView (&names)[N], Enum& out) noexcept
    {
        cont::StringView view;
        if (!string_at(table, key, view))
        {
            return false;
        }
        for (usize i = 0; i < N; ++i)
        {
            if (names[i] == view)
            {
                out = static_cast<Enum>(i);
                return true;
            }
        }
        return refuse(RuleError::BadValue, table.get(key), key);
    }

    // A day string; the empty string is "none", which only an optional day may be.
    bool day(const node& table, cont::StringView key, Day& out, bool required) noexcept
    {
        cont::StringView view;
        if (!string_at(table, key, view, true))
        {
            return false;
        }
        out = kNoDay;
        if ((view.empty() && required) || (!view.empty() && !parse_day(view, out)))
        {
            return refuse(RuleError::BadDay, table.get(key), key);
        }
        return true;
    }

    bool top(const node& root)
    {
        cont::StringView format;
        cont::StringView jurisdiction;
        if (!string_at(root, "format", format))
        {
            return false;
        }
        if (format != kFormat)
        {
            return refuse(RuleError::UnsupportedFormat, root.get("format"), "format");
        }
        if (!string_at(root, "jurisdiction", jurisdiction))
        {
            return false;
        }
        if (!is_jurisdiction(jurisdiction))
        {
            return refuse(RuleError::BadValue, root.get("jurisdiction"), "jurisdiction");
        }
        m_out.jurisdiction = cont::String(jurisdiction);
        const node* number = root.get("generation");
        if (number == nullptr)
        {
            return refuse(RuleError::MissingKey, &root, "generation");
        }
        const auto value = number->value<crd::i64>();
        if (!value.has_value())
        {
            return refuse(RuleError::WrongType, number, "generation");
        }
        if (*value < 1)
        {
            return refuse(RuleError::BadGeneration, number, "generation");
        }
        m_out.number = static_cast<u64>(*value);
        return day(root, "register", m_out.register_day, true);
    }

    // The array of tables at `key`: present, of at most `bound` tables, and not empty.
    const node* tables(const node& root, cont::StringView key, u32 bound) noexcept
    {
        const node* list = root.get(key);
        if (list == nullptr)
        {
            (void)refuse(RuleError::MissingKey, &root, key);
            return nullptr;
        }
        if (!list->is_array_of_tables())
        {
            (void)refuse(RuleError::WrongType, list, key);
            return nullptr;
        }
        if (list->size() > bound)
        {
            (void)refuse(RuleError::TooLarge, list, key);
            return nullptr;
        }
        return list;
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
                return refuse(RuleError::BadValue, table.get("id"), "id");
            }
            if (find_scope(id) != kNotFound)
            {
                return refuse(RuleError::DuplicateScope, table.get("id"), "id");
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

    // A list of at most kMaxListItems strings, not empty; each passes `valid`.
    template <typename Valid>
    bool list(const node& table, cont::StringView key, Valid valid, cont::Array<cont::StringView>& out) noexcept
    {
        const node* value = table.get(key);
        if (value == nullptr)
        {
            return refuse(RuleError::MissingKey, &table, key);
        }
        if (!value->is_array())
        {
            return refuse(RuleError::WrongType, value, key);
        }
        if (value->size() > kMaxListItems)
        {
            return refuse(RuleError::TooLarge, value, key);
        }
        if (value->size() == 0U)
        {
            return refuse(RuleError::BadValue, value, key);
        }
        for (const node& item : *value)
        {
            const auto string = item.value<std::string_view>();
            if (!string.has_value())
            {
                return refuse(RuleError::WrongType, &item, key);
            }
            if (!valid(*string))
            {
                return refuse(RuleError::BadValue, &item, key);
            }
            out.push_back(*string);
        }
        return true;
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
            return refuse(RuleError::BadValue, table.get("id"), "id");
        }
        for (usize i = 0; i + 1U < m_out.rules.size(); ++i)
        {
            if (cont::StringView(m_out.rules[i].id) == id)
            {
                return refuse(RuleError::DuplicateRule, table.get("id"), "id");
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
            return refuse(RuleError::BadValue, table.get("topic"), "topic");
        }
        if (!is_https_url(primary))
        {
            return refuse(RuleError::BadValue, table.get("primary"), "primary");
        }
        rule.topic = cont::String(topic);
        rule.primary = cont::String(primary);
        const node* read = table.get("read");
        if (read == nullptr)
        {
            return refuse(RuleError::MissingKey, &table, "read");
        }
        if (!read->is_boolean())
        {
            return refuse(RuleError::WrongType, read, "read");
        }
        rule.read = read->value_or(false);
        return rule_lists(table, rule) && rule_days(table, rule) && rule_proof(table, rule);
    }

    bool rule_lists(const node& table, Rule& rule)
    {
        cont::Array<cont::StringView> names;
        if (!list(table, "applies_to", [](cont::StringView s) { return is_dotted(s, is_lower_alnum); }, names))
        {
            return false;
        }
        for (const cont::StringView name : names)
        {
            const usize index = find_scope(name);
            if (index == kNotFound)
            {
                return refuse(RuleError::UnknownScope, table.get("applies_to"), "applies_to");
            }
            for (const u32 earlier : rule.applies_to)
            {
                if (earlier == index)
                {
                    return refuse(RuleError::BadValue, table.get("applies_to"), "applies_to");
                }
            }
            rule.applies_to.push_back(static_cast<u32>(index));
        }
        names.clear();
        if (!list(table, "rows", [](cont::StringView s) { return is_dotted(s, is_alnum); }, names))
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
            return refuse(RuleError::BadDay, table.get("sunset"), "sunset");
        }
        if ((rule.counsel == Counsel::Reviewed) != (rule.counsel_reviewed != kNoDay))
        {
            return refuse(RuleError::BadDay, table.get("counsel_reviewed"), "counsel_reviewed");
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
            return refuse(RuleError::BadValue, table.get("state"), "state");
        }
        return true;
    }

    RuleGeneration& m_out;
    DecodeFailure m_failure;
};

[[nodiscard]] const RuleGeneration* find_generation(const cont::Array<RuleGeneration>& generations,
                                                    cont::StringView jurisdiction) noexcept
{
    for (const RuleGeneration& generation : generations)
    {
        if (cont::StringView(generation.jurisdiction) == jurisdiction)
        {
            return &generation;
        }
    }
    return nullptr;
}
} // namespace

bool parse_day(cont::StringView text, Day& out) noexcept
{
    const usize size = text.size();
    if (size != 4U && size != 7U && size != 10U)
    {
        return false;
    }
    u32 year = 0U;
    u32 month = 1U;
    u32 day = 1U;
    if (!read_digits(text, 0U, 4U, year) || year < 1900U)
    {
        return false;
    }
    if (size >= 7U && (text[4] != '-' || !read_digits(text, 5U, 2U, month)))
    {
        return false;
    }
    if (size == 10U && (text[7] != '-' || !read_digits(text, 8U, 2U, day)))
    {
        return false;
    }
    const std::chrono::year_month_day civil{std::chrono::year{static_cast<int>(year)}, std::chrono::month{month},
                                            std::chrono::day{day}};
    if (!civil.ok())
    {
        return false;
    }
    out = (year * 10000U) + (month * 100U) + day;
    return true;
}

cont::StringView rule_error_name(RuleError error) noexcept
{
    switch (error)
    {
        case RuleError::None:
            return "none";
        case RuleError::TooLarge:
            return "too-large";
        case RuleError::Syntax:
            return "syntax";
        case RuleError::UnsupportedFormat:
            return "unsupported-format";
        case RuleError::MissingKey:
            return "missing-key";
        case RuleError::UnknownKey:
            return "unknown-key";
        case RuleError::WrongType:
            return "wrong-type";
        case RuleError::BadValue:
            return "bad-value";
        case RuleError::BadDay:
            return "bad-day";
        case RuleError::BadGeneration:
            return "bad-generation";
        case RuleError::DuplicateScope:
            return "duplicate-scope";
        case RuleError::UnknownScope:
            return "unknown-scope";
        case RuleError::DuplicateRule:
            return "duplicate-rule";
        case RuleError::Rollback:
            return "rollback";
        case RuleError::Conflict:
            return "conflict";
        case RuleError::AlreadyCurrent:
            return "already-current";
        case RuleError::StoreFull:
            return "store-full";
    }
    return "unknown";
}

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
    if (decoded.error != RuleError::None)
    {
        return decoded;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (RuleGeneration& current : m_generations)
    {
        if (cont::StringView(current.jurisdiction) != cont::StringView(generation.jurisdiction))
        {
            continue;
        }
        if (generation.number < current.number)
        {
            return DecodeFailure{RuleError::Rollback, 0U, "generation"};
        }
        if (generation.number == current.number)
        {
            const bool same = generation.digest == current.digest;
            return DecodeFailure{same ? RuleError::AlreadyCurrent : RuleError::Conflict, 0U, "generation"};
        }
        current = static_cast<RuleGeneration&&>(generation);
        return DecodeFailure{};
    }
    if (m_generations.size() >= kMaxJurisdictions)
    {
        return DecodeFailure{RuleError::StoreFull, 0U, "jurisdiction"};
    }
    m_generations.push_back(static_cast<RuleGeneration&&>(generation));
    return DecodeFailure{};
}

u64 RuleStore::generation(cont::StringView jurisdiction) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const RuleGeneration* found = find_generation(m_generations, jurisdiction);
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
        const RuleGeneration* generation = repeats ? nullptr : find_generation(m_generations, code);
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
