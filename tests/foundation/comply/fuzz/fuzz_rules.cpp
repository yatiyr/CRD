// crd-fuzz-comply-rules: crd-comply's reviewed rule generations (COMPLY.0; docs/design/registers.md#rule-generations)
// under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a generation as text, decoded;
//   1  a generated sequence: up to eight generations whose jurisdiction, number and rule are read from the bytes,
//      printed in the canonical layout and applied to one store, then queried.
//
// Oracles, none of them the decoder's or the store's own verdict:
//   * an accepted generation satisfies the contract's invariants (named scopes, unique ids, real and ordered days, no
//     claimed proof without an active check, a read text and answered counsel) and, printed again in the canonical
//     layout restated here from scripts/gen-comply-rules.py, decodes to the same generation;
//   * a model of monotonic adoption that compares whole texts (not digests) decides every apply's exact result and
//     every jurisdiction's number after it;
//   * every query equals a brute-force answer over the model's adopted generations, in request order, repeats and
//     unknown jurisdictions included.
#include <crd/comply/rules.hpp>
#include <crd/fuzz/harness.hpp>

#include <cstdio>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::comply;
namespace cont = crd::containers;

constexpr const char* kScopeKinds[] = {"product", "flow", "channel"};
constexpr const char* kScopeStates[] = {"planned", "live"};
constexpr const char* kRuleKinds[] = {"obligation", "user-action"};
constexpr const char* kRuleStates[] = {"planned", "implemented"};
constexpr const char* kChecks[] = {"planned", "active"};
constexpr const char* kCounsel[] = {"none", "needed", "reviewed"};

// ---- The canonical layout, restated ------------------------------------------------------------------------------

void quoted(cont::String& out, cont::StringView text)
{
    out.push_back('"');
    for (const char c : text)
    {
        const auto code = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(c);
        }
        else if (code < 0x20U || code == 0x7FU)
        {
            char escape[8];
            (void)std::snprintf(escape, sizeof(escape), "\\u%04X", code);
            out.append(escape);
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void day_value(cont::String& out, Day day)
{
    char text[16] = {};
    if (day != kNoDay)
    {
        (void)std::snprintf(text, sizeof(text), "%04u-%02u-%02u", day / 10000U, (day / 100U) % 100U, day % 100U);
    }
    quoted(out, text);
}

void line(cont::String& out, const char* key, cont::StringView value)
{
    out.append(key);
    out.append(" = ");
    quoted(out, value);
    out.push_back('\n');
}

void day_line(cont::String& out, const char* key, Day day)
{
    out.append(key);
    out.append(" = ");
    day_value(out, day);
    out.push_back('\n');
}

cont::String print(const RuleGeneration& generation)
{
    cont::String out("format = \"cerid-rules/1\"\n");
    line(out, "jurisdiction", generation.jurisdiction);
    char number[32];
    (void)std::snprintf(number, sizeof(number), "generation = %llu\n",
                        static_cast<unsigned long long>(generation.number));
    out.append(number);
    day_line(out, "register", generation.register_day);
    for (const Scope& scope : generation.scopes)
    {
        out.append("\n[[scope]]\n");
        line(out, "id", scope.id);
        line(out, "kind", kScopeKinds[static_cast<u8>(scope.kind)]);
        line(out, "state", kScopeStates[static_cast<u8>(scope.state)]);
    }
    for (const Rule& rule : generation.rules)
    {
        out.append("\n[[rule]]\n");
        line(out, "id", rule.id);
        line(out, "kind", kRuleKinds[static_cast<u8>(rule.kind)]);
        line(out, "topic", rule.topic);
        line(out, "state", kRuleStates[static_cast<u8>(rule.state)]);
        line(out, "title", rule.title);
        line(out, "instrument", rule.instrument);
        line(out, "article", rule.article);
        line(out, "primary", rule.primary);
        out.append(rule.read ? "read = true\n" : "read = false\n");
        line(out, "obligation", rule.obligation);
        out.append("applies_to = [");
        for (usize i = 0U; i < rule.applies_to.size(); ++i)
        {
            out.append(i == 0U ? "" : ", ");
            quoted(out, generation.scopes[rule.applies_to[i]].id);
        }
        out.append("]\n");
        line(out, "check", kChecks[static_cast<u8>(rule.check)]);
        line(out, "check_name", rule.check_name);
        line(out, "check_ref", rule.check_ref);
        day_line(out, "effective", rule.effective);
        day_line(out, "sunset", rule.sunset);
        line(out, "counsel", kCounsel[static_cast<u8>(rule.counsel)]);
        day_line(out, "counsel_reviewed", rule.counsel_reviewed);
        out.append("rows = [");
        for (usize i = 0U; i < rule.rows.size(); ++i)
        {
            out.append(i == 0U ? "" : ", ");
            quoted(out, rule.rows[i]);
        }
        out.append("]\n");
    }
    return out;
}

bool same(const RuleGeneration& a, const RuleGeneration& b)
{
    if (cont::StringView(a.jurisdiction) != cont::StringView(b.jurisdiction) || a.number != b.number ||
        a.register_day != b.register_day || a.scopes.size() != b.scopes.size() || a.rules.size() != b.rules.size())
    {
        return false;
    }
    for (usize i = 0U; i < a.scopes.size(); ++i)
    {
        if (cont::StringView(a.scopes[i].id) != cont::StringView(b.scopes[i].id) ||
            a.scopes[i].kind != b.scopes[i].kind || a.scopes[i].state != b.scopes[i].state)
        {
            return false;
        }
    }
    for (usize i = 0U; i < a.rules.size(); ++i)
    {
        const Rule& x = a.rules[i];
        const Rule& y = b.rules[i];
        bool equal = cont::StringView(x.id) == cont::StringView(y.id) && x.kind == y.kind && x.state == y.state &&
                     cont::StringView(x.topic) == cont::StringView(y.topic) &&
                     cont::StringView(x.title) == cont::StringView(y.title) &&
                     cont::StringView(x.instrument) == cont::StringView(y.instrument) &&
                     cont::StringView(x.article) == cont::StringView(y.article) &&
                     cont::StringView(x.primary) == cont::StringView(y.primary) && x.read == y.read &&
                     cont::StringView(x.obligation) == cont::StringView(y.obligation) && x.check == y.check &&
                     cont::StringView(x.check_name) == cont::StringView(y.check_name) &&
                     cont::StringView(x.check_ref) == cont::StringView(y.check_ref) && x.effective == y.effective &&
                     x.sunset == y.sunset && x.counsel == y.counsel && x.counsel_reviewed == y.counsel_reviewed &&
                     x.applies_to.size() == y.applies_to.size() && x.rows.size() == y.rows.size();
        for (usize j = 0U; equal && j < x.applies_to.size(); ++j)
        {
            equal = x.applies_to[j] == y.applies_to[j];
        }
        for (usize j = 0U; equal && j < x.rows.size(); ++j)
        {
            equal = cont::StringView(x.rows[j]) == cont::StringView(y.rows[j]);
        }
        if (!equal)
        {
            return false;
        }
    }
    return true;
}

bool real_day(Day day)
{
    char text[16];
    (void)std::snprintf(text, sizeof(text), "%04u-%02u-%02u", day / 10000U, (day / 100U) % 100U, day % 100U);
    Day parsed = kNoDay;
    return parse_day(text, parsed) && parsed == day;
}

void check_invariants(const RuleGeneration& generation)
{
    CRD_FUZZ_REQUIRE(generation.number >= 1U && real_day(generation.register_day));
    CRD_FUZZ_REQUIRE(!generation.scopes.empty() && generation.scopes.size() <= kMaxScopes);
    CRD_FUZZ_REQUIRE(!generation.rules.empty() && generation.rules.size() <= kMaxRules);
    for (usize i = 0U; i < generation.scopes.size(); ++i)
    {
        for (usize j = 0U; j < i; ++j)
        {
            CRD_FUZZ_REQUIRE(cont::StringView(generation.scopes[i].id) != cont::StringView(generation.scopes[j].id));
        }
    }
    for (usize i = 0U; i < generation.rules.size(); ++i)
    {
        const Rule& rule = generation.rules[i];
        for (usize j = 0U; j < i; ++j)
        {
            CRD_FUZZ_REQUIRE(cont::StringView(rule.id) != cont::StringView(generation.rules[j].id));
        }
        CRD_FUZZ_REQUIRE(!rule.applies_to.empty() && !rule.rows.empty());
        for (const u32 index : rule.applies_to)
        {
            CRD_FUZZ_REQUIRE(index < generation.scopes.size());
        }
        CRD_FUZZ_REQUIRE(real_day(rule.effective));
        CRD_FUZZ_REQUIRE(rule.sunset == kNoDay || (real_day(rule.sunset) && rule.sunset > rule.effective));
        CRD_FUZZ_REQUIRE((rule.counsel == Counsel::Reviewed) == (rule.counsel_reviewed != kNoDay));
        CRD_FUZZ_REQUIRE(rule.counsel_reviewed == kNoDay || real_day(rule.counsel_reviewed));
        CRD_FUZZ_REQUIRE(rule.state == RuleState::Planned ||
                         (rule.check == CheckState::Active && rule.read && rule.counsel != Counsel::Needed));
        CRD_FUZZ_REQUIRE(cont::StringView(rule.primary).starts_with("https://"));
    }
}

void decode_checked(const u8* data, usize size)
{
    const cont::StringView text(reinterpret_cast<const char*>(data), size); // NOLINT
    RuleGeneration generation;
    if (decode_rules(text, generation).error != RuleError::None)
    {
        return;
    }
    check_invariants(generation);
    const cont::String printed = print(generation);
    RuleGeneration again;
    CRD_FUZZ_REQUIRE(decode_rules(printed, again).error == RuleError::None);
    CRD_FUZZ_REQUIRE(same(generation, again));
    CRD_FUZZ_REQUIRE(print(again) == cont::StringView(printed));
}

// ---- Generated sequences against the adoption and query model -----------------------------------------------------

constexpr cont::StringView kCodes[] = {"TR", "EU", "US-CA"};
constexpr cont::StringView kScopes[] = {"flow.a", "product.b"};
constexpr u32 kSteps = 8U;

class Reader
{
public:
    Reader(const u8* data, usize size) noexcept : m_data(data), m_size(size) {}

    u8 next() noexcept { return m_at < m_size ? m_data[m_at++] : 0U; }
    [[nodiscard]] bool done() const noexcept { return m_at >= m_size; }

private:
    const u8* m_data;
    usize m_size;
    usize m_at = 0U;
};

// A generation with both scopes and one rule whose dates, state and scopes come from the bytes.
RuleGeneration drawn(Reader& in)
{
    RuleGeneration generation;
    generation.jurisdiction = kCodes[in.next() % 3U];
    generation.number = 1U + (in.next() % 6U);
    generation.register_day = 20261031U;
    for (u32 s = 0U; s < 2U; ++s)
    {
        Scope& scope = generation.scopes.emplace_back();
        scope.id = kScopes[s];
        scope.kind = s == 0U ? ScopeKind::Flow : ScopeKind::Product;
        scope.state = (in.next() % 2U) == 0U ? ScopeState::Planned : ScopeState::Live;
    }
    const u8 shape = in.next();
    Rule& rule = generation.rules.emplace_back();
    char id[32];
    (void)std::snprintf(id, sizeof(id), "r.%u", static_cast<unsigned>(shape % 4U));
    rule.id = id;
    rule.topic = "privacy";
    rule.title = "t";
    rule.instrument = "i";
    rule.article = "a";
    rule.primary = "https://x.example/l";
    rule.check_name = "c";
    rule.check_ref = "r";
    char obligation[32];
    (void)std::snprintf(obligation, sizeof(obligation), "o%u", static_cast<unsigned>(in.next() % 3U));
    rule.obligation = obligation;
    rule.rows.push_back(cont::String("COMPLY.0"));
    rule.effective = (2020U + (shape / 4U) % 10U) * 10000U + 101U;
    const u32 length = (shape / 40U) % 3U;
    rule.sunset = length == 0U ? kNoDay : rule.effective + length * 10000U;
    if ((shape & 0x80U) != 0U)
    {
        rule.state = RuleState::Implemented;
        rule.check = CheckState::Active;
        rule.read = true;
    }
    const u8 scopes = in.next() % 3U;
    if (scopes != 1U)
    {
        rule.applies_to.push_back(0U);
    }
    if (scopes != 0U)
    {
        rule.applies_to.push_back(1U);
    }
    return generation;
}

struct Adopted
{
    cont::String text;
    RuleGeneration generation;
    bool held = false;
};

void generated_sequence(const u8* data, usize size)
{
    Reader in(data, size);
    RuleStore store;
    Adopted model[3];
    const u32 steps = 1U + (in.next() % kSteps);
    for (u32 step = 0U; step < steps && !in.done(); ++step)
    {
        const RuleGeneration generation = drawn(in);
        const cont::String text = print(generation);
        usize slot = 0U;
        while (kCodes[slot] != cont::StringView(generation.jurisdiction))
        {
            ++slot;
        }
        Adopted& held = model[slot];
        RuleError want = RuleError::None;
        if (held.held && generation.number < held.generation.number)
        {
            want = RuleError::Rollback;
        }
        else if (held.held && generation.number == held.generation.number)
        {
            want = held.text == cont::StringView(text) ? RuleError::AlreadyCurrent : RuleError::Conflict;
        }
        CRD_FUZZ_REQUIRE(store.apply(text).error == want);
        if (want == RuleError::None)
        {
            held.text = text;
            held.generation = generation;
            held.held = true;
        }
        for (usize code = 0U; code < 3U; ++code)
        {
            CRD_FUZZ_REQUIRE(store.generation(kCodes[code]) == (model[code].held ? model[code].generation.number : 0U));
        }
    }

    // Queries in a drawn order with a repeat and an unknown code, at drawn days, on both scopes and an unknown one.
    const cont::StringView order[] = {kCodes[in.next() % 3U], "GB", kCodes[in.next() % 3U], kCodes[in.next() % 3U]};
    for (u32 q = 0U; q < 4U; ++q)
    {
        const Day day = (2019U + (in.next() % 14U)) * 10000U + 101U - (in.next() % 2U) * 9070U;
        const cont::StringView scope = q < 3U ? kScopes[q % 2U] : cont::StringView("flow.none");
        cont::Array<Binding> answer;
        store.query(RuleQuery{scope, cont::ConstSpan<cont::StringView>(order, 4U), day}, answer);
        usize at = 0U;
        for (usize o = 0U; o < 4U; ++o)
        {
            bool repeat = false;
            for (usize earlier = 0U; earlier < o; ++earlier)
            {
                repeat = repeat || order[earlier] == order[o];
            }
            usize slot = 3U;
            for (usize code = 0U; code < 3U; ++code)
            {
                slot = kCodes[code] == order[o] ? code : slot;
            }
            if (repeat || slot == 3U || !model[slot].held)
            {
                continue;
            }
            const RuleGeneration& generation = model[slot].generation;
            for (u32 s = 0U; s < generation.scopes.size(); ++s)
            {
                if (cont::StringView(generation.scopes[s].id) != scope)
                {
                    continue;
                }
                for (const Rule& rule : generation.rules)
                {
                    bool applies = false;
                    for (const u32 index : rule.applies_to)
                    {
                        applies = applies || index == s;
                    }
                    const bool effective = rule.effective <= day && (rule.sunset == kNoDay || day < rule.sunset);
                    if (!applies || !effective)
                    {
                        continue;
                    }
                    CRD_FUZZ_REQUIRE(at < answer.size());
                    const Binding& binding = answer[at++];
                    const bool proven = rule.state == RuleState::Implemented && rule.check == CheckState::Active;
                    CRD_FUZZ_REQUIRE(cont::StringView(binding.jurisdiction) == kCodes[slot]);
                    CRD_FUZZ_REQUIRE(binding.generation == generation.number);
                    CRD_FUZZ_REQUIRE(binding.scope_state == generation.scopes[s].state);
                    CRD_FUZZ_REQUIRE(binding.proof == (proven ? Proof::Proven : Proof::Unproven));
                    CRD_FUZZ_REQUIRE(cont::StringView(binding.rule.id) == cont::StringView(rule.id));
                    CRD_FUZZ_REQUIRE(cont::StringView(binding.rule.obligation) == cont::StringView(rule.obligation));
                }
            }
        }
        CRD_FUZZ_REQUIRE(at == answer.size());
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, u8 mode, cont::StringView data)
{
    cont::String input;
    input.push_back(static_cast<char>(mode));
    input.append(data);
    sink.add(name, reinterpret_cast<const u8*>(input.data()), input.size()); // NOLINT
}

constexpr cont::StringView kSeed = R"toml(format = "cerid-rules/1"
jurisdiction = "TR"
generation = 2
register = "2026-10-31"

[[scope]]
id = "flow.crash-reports"
kind = "flow"
state = "live"

[[scope]]
id = "channel.cerid-store"
kind = "channel"
state = "planned"

[[rule]]
id = "tr.kvkk.processing"
kind = "obligation"
topic = "privacy"
state = "planned"
title = "KVKK \"işleme\""
instrument = "Law No. 6698"
article = "Articles 4-7"
primary = "https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=6698"
read = false
obligation = "Process personal data lawfully."
applies_to = ["flow.crash-reports", "channel.cerid-store"]
check = "planned"
check_name = "data inventory CTest"
check_ref = "docs/ROADMAP.md#slice-comply.2"
effective = "2016-04"
sunset = "2030-01-01"
counsel = "needed"
counsel_reviewed = ""
rows = ["COMPLY.2", "CRASH-FIX.1"]

[[rule]]
id = "tr.5651.game-platform"
kind = "user-action"
topic = "online-safety"
state = "implemented"
title = "Game platform duties"
instrument = "Law No. 5651"
article = "Additional Article 4"
primary = "https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=5651"
read = true
obligation = "Rate games by age."
applies_to = ["channel.cerid-store"]
check = "active"
check_name = "age rating test"
check_ref = "tests/store/test_ratings.cpp"
effective = "2026"
sunset = ""
counsel = "reviewed"
counsel_reviewed = "2026-10-30"
rows = ["STORE.1"]
)toml";

cont::String replaced(cont::StringView from, cont::StringView to)
{
    const usize at = kSeed.find(from);
    cont::String text(kSeed.substr(0U, at));
    text.append(to);
    text.append(kSeed.substr(at + from.size()));
    return text;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    if (data[0] % 2U == 0U)
    {
        decode_checked(data + 1U, size - 1U);
    }
    else
    {
        generated_sequence(data + 1U, size - 1U);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    add(sink, "rules", 0U, kSeed);
    add(sink, "rules-truncated", 0U, kSeed.substr(0U, kSeed.size() / 2U));
    add(sink, "rules-unknown-key", 0U, replaced("read = false", "read = false\nverified = true"));
    add(sink, "rules-unknown-scope", 0U, replaced("\"channel.cerid-store\"]", "\"channel.none\"]"));
    add(sink, "rules-duplicate-scope", 0U, replaced("id = \"channel.cerid-store\"", "id = \"flow.crash-reports\""));
    add(sink, "rules-bad-day", 0U, replaced("effective = \"2016-04\"", "effective = \"2016-02-30\""));
    add(sink, "rules-sunset-first", 0U, replaced("sunset = \"2030-01-01\"", "sunset = \"2016-04-01\""));
    add(sink, "rules-claimed-proof", 0U, replaced("read = true", "read = false"));
    add(sink, "rules-future-format", 0U, replaced("cerid-rules/1", "cerid-rules/2"));
    add(sink, "rules-escapes", 0U, replaced(R"(title = "Game platform duties")", R"(title = "a\tbç")"));
    const u8 adopt[] = {7U, 0U, 2U, 1U, 0x85U, 1U, 2U, 0U, 3U, 0U, 0x10U, 0U, 1U, 0U, 1U, 0U, 0x2AU,
                        2U, 0U, 1U, 0U, 0U,    3U, 1U, 0U, 0U, 1U, 0U,    1U, 2U, 1U, 0U, 9U, 1U};
    const u8 conflict[] = {3U, 1U, 4U, 0U,    1U, 0x91U, 0U, 1U, 1U, 4U, 0U, 1U, 0x91U, 1U, 1U, 1U,
                           4U, 0U, 1U, 0x91U, 0U, 1U,    0U, 1U, 2U, 5U, 1U, 7U, 0U,    3U, 0U};
    const u8 rollback[] = {4U, 2U, 5U,    1U, 1U, 0x07U, 2U, 2U, 2U, 2U, 0U, 0U, 0x07U, 2U,  2U, 0U, 3U,
                           1U, 0U, 0x30U, 0U, 0U, 2U,    0U, 2U, 2U, 6U, 0U, 1U, 0U,    13U, 1U, 0U, 0U};
    // One TR generation in force on [2021-01-01, 2022-01-01) on flow.a, queried on both edge days.
    const u8 edges[] = {0U, 0U, 0U, 0U, 0U, 44U, 0U, 0U, 0U, 0U, 0U, 2U, 0U, 0U, 0U, 3U, 0U, 0U, 0U};
    add(sink, "sequence-edges", 1U, cont::StringView(reinterpret_cast<const char*>(edges), sizeof(edges)));
    add(sink, "sequence-adopt", 1U, cont::StringView(reinterpret_cast<const char*>(adopt), sizeof(adopt)));
    add(sink, "sequence-conflict", 1U, cont::StringView(reinterpret_cast<const char*>(conflict), sizeof(conflict)));
    add(sink, "sequence-rollback", 1U, cont::StringView(reinterpret_cast<const char*>(rollback), sizeof(rollback)));
}
