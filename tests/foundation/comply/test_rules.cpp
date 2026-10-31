// crd-comply -- reviewed rule generations (COMPLY.0; docs/design/registers.md#rule-generations): days, the whole
// decoder and every refusal with its line and key, monotonic live swap per jurisdiction, the query's in-force and
// scope rules, proof, a concurrent applier and reader, and the committed generations of assets/comply.

#include <crd/comply/rules.hpp>
#include <crd/platform/filesystem.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <thread>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using namespace crd::comply;
namespace cont = crd::containers;

constexpr cont::StringView kBase = R"toml(# a test generation
format = "cerid-rules/1"
jurisdiction = "TR"
generation = 3
register = "2026-10-31"

[[scope]]
id = "flow.crash-reports"
kind = "flow"
state = "live"

[[scope]]
id = "product.store"
kind = "product"
state = "planned"

[[rule]]
id = "tr.kvkk.processing"
kind = "obligation"
topic = "privacy"
state = "planned"
title = "KVKK processing"
instrument = "Law No. 6698"
article = "Articles 4-7"
primary = "https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=6698"
read = false
obligation = "Process personal data lawfully."
applies_to = ["flow.crash-reports"]
check = "planned"
check_name = "data inventory CTest"
check_ref = "docs/ROADMAP.md#slice-comply.2"
effective = "2016-04-07"
sunset = ""
counsel = "needed"
counsel_reviewed = ""
rows = ["COMPLY.2", "CRASH-FIX.1"]

[[rule]]
id = "tr.5651.game-platform"
kind = "obligation"
topic = "online-safety"
state = "implemented"
title = "Game platform duties"
instrument = "Law No. 5651"
article = "Additional Article 4"
primary = "https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=5651"
read = true
obligation = "Rate games by age."
applies_to = ["product.store", "flow.crash-reports"]
check = "active"
check_name = "age rating test"
check_ref = "tests/store/test_ratings.cpp"
effective = "2026-11"
sunset = "2027-01-01"
counsel = "reviewed"
counsel_reviewed = "2026-10-30"
rows = ["STORE.1"]

[[rule]]
id = "tr.verbis.registration"
kind = "user-action"
topic = "privacy"
state = "planned"
title = "VERBIS registration"
instrument = "VERBIS By-Law"
article = "Article 9"
primary = "https://www.kvkk.gov.tr/verbis"
read = false
obligation = "Register with VERBIS."
applies_to = ["product.store"]
check = "planned"
check_name = "registration record"
check_ref = "docs/ROADMAP.md#slice-comply.2"
effective = "2027"
sunset = ""
counsel = "none"
counsel_reviewed = ""
rows = ["COMPLY.2"]
)toml";

// kBase with the first occurrence of `from` replaced by `to` (the case fails when `from` is absent).
cont::String with(cont::StringView from, cont::StringView to, cont::StringView base = kBase)
{
    const usize at = base.find(from);
    REQUIRE(at != cont::StringView::npos);
    cont::String text(base.substr(0U, at));
    text.append(to);
    text.append(base.substr(at + from.size()));
    return text;
}

u32 line_of(cont::StringView text, cont::StringView needle)
{
    const usize at = text.find(needle);
    REQUIRE(at != cont::StringView::npos);
    u32 line = 1U;
    for (usize i = 0U; i < at; ++i)
    {
        line += text[i] == '\n' ? 1U : 0U;
    }
    return line;
}

DecodeFailure decode(cont::StringView text)
{
    RuleGeneration generation;
    return decode_rules(text, generation);
}

void check_refusal(cont::StringView text, ComplyError error, cont::StringView key)
{
    const DecodeFailure failure = decode(text);
    INFO(comply_error_name(failure.error) << " key " << failure.key);
    CHECK(failure.error == error);
    CHECK(failure.key == key);
}

Day day(cont::StringView text)
{
    Day out = kNoDay;
    REQUIRE(parse_day(text, out));
    return out;
}

cont::Array<Binding> query(const RuleStore& store, cont::StringView scope, cont::StringView today,
                           std::initializer_list<cont::StringView> codes)
{
    cont::Array<cont::StringView> list;
    for (const cont::StringView code : codes)
    {
        list.push_back(code);
    }
    cont::Array<Binding> out;
    store.query(RuleQuery{scope, cont::ConstSpan<cont::StringView>(list.data(), list.size()), day(today)}, out);
    return out;
}
} // namespace

TEST_CASE("comply: days are real calendar days, a period counting from its first day", "[comply]")
{
    CHECK(day("2026-11-01") == 20261101U);
    CHECK(day("2026-11") == 20261101U);
    CHECK(day("2027") == 20270101U);
    CHECK(day("2024-02-29") == 20240229U);
    CHECK(day("1900-01-01") == 19000101U);
    Day out = 7U;
    for (const cont::StringView bad :
         {"2025-02-29", "2026-13", "2026-00", "2026-04-31", "2026-01-00", "1899", "202", "20261", "2026-1-01",
          "2026/11/01", "2026-11-1", "2026-11-01 ", "abcd", "", "2026-11-01-02", "-2026"})
    {
        INFO(bad);
        CHECK_FALSE(parse_day(bad, out));
    }
    CHECK(out == 7U);
}

TEST_CASE("comply: a generation decodes whole, every field typed", "[comply]")
{
    RuleGeneration generation;
    REQUIRE(decode_rules(kBase, generation).error == ComplyError::None);
    CHECK(cont::StringView(generation.jurisdiction) == "TR");
    CHECK(generation.number == 3U);
    CHECK(generation.register_day == 20261031U);
    CHECK(generation.digest != 0U);
    REQUIRE(generation.scopes.size() == 2U);
    CHECK(cont::StringView(generation.scopes[0].id) == "flow.crash-reports");
    CHECK(generation.scopes[0].kind == ScopeKind::Flow);
    CHECK(generation.scopes[0].state == ScopeState::Live);
    CHECK(generation.scopes[1].kind == ScopeKind::Product);
    CHECK(generation.scopes[1].state == ScopeState::Planned);
    REQUIRE(generation.rules.size() == 3U);

    const Rule& kvkk = generation.rules[0];
    CHECK(cont::StringView(kvkk.id) == "tr.kvkk.processing");
    CHECK(kvkk.kind == RuleKind::Obligation);
    CHECK(kvkk.state == RuleState::Planned);
    CHECK(cont::StringView(kvkk.topic) == "privacy");
    CHECK(cont::StringView(kvkk.title) == "KVKK processing");
    CHECK(cont::StringView(kvkk.instrument) == "Law No. 6698");
    CHECK(cont::StringView(kvkk.article) == "Articles 4-7");
    CHECK(cont::StringView(kvkk.primary) == "https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=6698");
    CHECK_FALSE(kvkk.read);
    CHECK(cont::StringView(kvkk.obligation) == "Process personal data lawfully.");
    REQUIRE(kvkk.applies_to.size() == 1U);
    CHECK(kvkk.applies_to[0] == 0U);
    CHECK(kvkk.check == CheckState::Planned);
    CHECK(cont::StringView(kvkk.check_name) == "data inventory CTest");
    CHECK(cont::StringView(kvkk.check_ref) == "docs/ROADMAP.md#slice-comply.2");
    CHECK(kvkk.effective == 20160407U);
    CHECK(kvkk.sunset == kNoDay);
    CHECK(kvkk.counsel == Counsel::Needed);
    CHECK(kvkk.counsel_reviewed == kNoDay);
    REQUIRE(kvkk.rows.size() == 2U);
    CHECK(cont::StringView(kvkk.rows[1]) == "CRASH-FIX.1");

    const Rule& platform = generation.rules[1];
    CHECK(platform.state == RuleState::Implemented);
    CHECK(platform.read);
    CHECK(platform.check == CheckState::Active);
    REQUIRE(platform.applies_to.size() == 2U);
    CHECK(platform.applies_to[0] == 1U);
    CHECK(platform.applies_to[1] == 0U);
    CHECK(platform.effective == 20261101U);
    CHECK(platform.sunset == 20270101U);
    CHECK(platform.counsel == Counsel::Reviewed);
    CHECK(platform.counsel_reviewed == 20261030U);
    CHECK(generation.rules[2].kind == RuleKind::UserAction);
    CHECK(generation.rules[2].counsel == Counsel::None);
    CHECK(generation.rules[2].effective == 20270101U);

    // Non-ASCII text, escapes and another generation's bytes.
    RuleGeneration other;
    REQUIRE(decode_rules(with("title = \"KVKK processing\"", "title = \"Kişisel veri \\\"işleme\\\"\""), other).error ==
            ComplyError::None);
    CHECK(cont::StringView(other.rules[0].title) == "Kişisel veri \"işleme\"");
    CHECK(other.digest != generation.digest);
}

TEST_CASE("comply: format, syntax and size refusals", "[comply]")
{
    CHECK(decode("format = ").error == ComplyError::Syntax);
    const cont::String broken = with("check = \"planned\"", "check = \"planned");
    const DecodeFailure syntax = decode(broken);
    CHECK(syntax.error == ComplyError::Syntax);
    CHECK(syntax.line == line_of(broken, "check = \"planned"));
    check_refusal(with("cerid-rules/1", "cerid-rules/2"), ComplyError::UnsupportedFormat, "format");

    cont::String huge(kBase);
    huge.append(cont::String(kMaxRulesBytes, '#'));
    CHECK(decode(huge).error == ComplyError::TooLarge);

    const cont::String long_title(kMaxTextBytes + 1U, 'x');
    cont::String title("title = \"");
    title.append(long_title);
    title.append("\"");
    check_refusal(with("title = \"KVKK processing\"", title), ComplyError::TooLarge, "title");
    const cont::String fits(kMaxTextBytes, 'x');
    cont::String at_bound("title = \"");
    at_bound.append(fits);
    at_bound.append("\"");
    CHECK(decode(with("title = \"KVKK processing\"", at_bound)).error == ComplyError::None);

    cont::String rows("rows = [");
    for (u32 i = 0U; i <= kMaxListItems; ++i)
    {
        rows.append(i == 0U ? "\"R.1\"" : ", \"R.1\"");
    }
    rows.append("]");
    check_refusal(with("rows = [\"STORE.1\"]", rows), ComplyError::TooLarge, "rows");

    cont::String scopes(kBase);
    for (u32 i = 0U; i < kMaxScopes; ++i)
    {
        scopes.append("\n[[scope]]\nid = \"flow.extra");
        char number[16];
        std::snprintf(number, sizeof(number), "%u", i);
        scopes.append(number);
        scopes.append("\"\nkind = \"flow\"\nstate = \"planned\"\n");
    }
    check_refusal(scopes, ComplyError::TooLarge, "scope");
}

TEST_CASE("comply: every required key, no unknown key, every type", "[comply]")
{
    check_refusal(with("format = \"cerid-rules/1\"\n", ""), ComplyError::MissingKey, "format");
    check_refusal(with("jurisdiction = \"TR\"\n", ""), ComplyError::MissingKey, "jurisdiction");
    check_refusal(with("generation = 3\n", ""), ComplyError::MissingKey, "generation");
    check_refusal(with("register = \"2026-10-31\"\n", ""), ComplyError::MissingKey, "register");
    for (const cont::StringView line :
         {"kind = \"obligation\"\n", "topic = \"privacy\"\n", "title = \"KVKK processing\"\n",
          "instrument = \"Law No. 6698\"\n", "article = \"Articles 4-7\"\n",
          "primary = \"https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=6698\"\n", "read = false\n",
          "obligation = \"Process personal data lawfully.\"\n", "applies_to = [\"flow.crash-reports\"]\n",
          "check_name = \"data inventory CTest\"\n", "effective = \"2016-04-07\"\n", "counsel = \"needed\"\n",
          "rows = [\"COMPLY.2\", \"CRASH-FIX.1\"]\n"})
    {
        INFO(line);
        CHECK(decode(with(line, "")).error == ComplyError::MissingKey);
    }
    check_refusal(with("id = \"product.store\"\n", ""), ComplyError::MissingKey, "id");

    const cont::String top = with("generation = 3", "generation = 3\nnote = \"x\"");
    const DecodeFailure unknown = decode(top);
    CHECK(unknown.error == ComplyError::UnknownKey);
    CHECK(unknown.line == line_of(top, "note ="));
    CHECK(decode(with("kind = \"flow\"", "kind = \"flow\"\nowner = \"x\"")).error == ComplyError::UnknownKey);
    CHECK(decode(with("rows = [\"STORE.1\"]", "rows = [\"STORE.1\"]\nverified = true")).error == ComplyError::UnknownKey);

    check_refusal(with("generation = 3", "generation = \"3\""), ComplyError::WrongType, "generation");
    check_refusal(with("jurisdiction = \"TR\"", "jurisdiction = 90"), ComplyError::WrongType, "jurisdiction");
    check_refusal(with("read = false", "read = \"false\""), ComplyError::WrongType, "read");
    check_refusal(with("applies_to = [\"flow.crash-reports\"]", "applies_to = \"flow.crash-reports\""),
                  ComplyError::WrongType, "applies_to");
    check_refusal(with("rows = [\"STORE.1\"]", "rows = [\"STORE.1\", 2]"), ComplyError::WrongType, "rows");
    const cont::String no_scopes = with("[[scope]]\nid = \"flow.crash-reports\"\nkind = \"flow\"\nstate = \"live\"\n\n"
                                        "[[scope]]\nid = \"product.store\"\nkind = \"product\"\nstate = \"planned\"\n",
                                        "");
    check_refusal(with("generation = 3", "generation = 3\nscope = 1", no_scopes), ComplyError::WrongType, "scope");
    check_refusal(no_scopes, ComplyError::MissingKey, "scope");
}

TEST_CASE("comply: values, identifiers and enumerations are closed", "[comply]")
{
    check_refusal(with("generation = 3", "generation = 0"), ComplyError::BadGeneration, "generation");
    check_refusal(with("generation = 3", "generation = -4"), ComplyError::BadGeneration, "generation");
    for (const cont::StringView code : {"tr", "T", "TUR", "US-", "US-CALIF", "US_CA", "INTL", ""})
    {
        cont::String line("jurisdiction = \"");
        line.append(code);
        line.append("\"");
        INFO(code);
        CHECK(decode(with("jurisdiction = \"TR\"", line)).error == ComplyError::BadValue);
    }
    for (const cont::StringView code : {"US-CA", "INT", "PLATFORM", "GB", "US-123"})
    {
        cont::String line("jurisdiction = \"");
        line.append(code);
        line.append("\"");
        INFO(code);
        CHECK(decode(with("jurisdiction = \"TR\"", line)).error == ComplyError::None);
    }
    for (const cont::StringView id : {"Tr.kvkk", "tr..kvkk", "tr.kvkk.", ".tr", "tr kvkk", "tr_kvkk", "tr.-kvkk"})
    {
        cont::String line("id = \"");
        line.append(id);
        line.append("\"");
        INFO(id);
        CHECK(decode(with("id = \"tr.kvkk.processing\"", line)).error == ComplyError::BadValue);
    }
    check_refusal(with("id = \"product.store\"", "id = \"Product.store\""), ComplyError::BadValue, "id");
    check_refusal(with("topic = \"privacy\"", "topic = \"Privacy\""), ComplyError::BadValue, "topic");
    check_refusal(with("title = \"KVKK processing\"", "title = \"\""), ComplyError::BadValue, "title");
    for (const cont::StringView url :
         {"http://www.mevzuat.gov.tr/x", "https://", "https:///x", "https://a b", "ftp://x", "www.mevzuat.gov.tr"})
    {
        cont::String line("primary = \"");
        line.append(url);
        line.append("\"");
        INFO(url);
        CHECK(decode(with("primary = \"https://www.mevzuat.gov.tr/mevzuat?MevzuatNo=6698\"", line)).error ==
              ComplyError::BadValue);
    }
    check_refusal(with("kind = \"obligation\"", "kind = \"duty\""), ComplyError::BadValue, "kind");
    check_refusal(with("state = \"planned\"\ntitle", "state = \"watch\"\ntitle"), ComplyError::BadValue, "state");
    check_refusal(with("kind = \"flow\"", "kind = \"stream\""), ComplyError::BadValue, "kind");
    check_refusal(with("state = \"live\"", "state = \"shipped\""), ComplyError::BadValue, "state");
    check_refusal(with("check = \"planned\"", "check = \"passing\""), ComplyError::BadValue, "check");
    check_refusal(with("counsel = \"needed\"", "counsel = \"maybe\""), ComplyError::BadValue, "counsel");
    check_refusal(with("applies_to = [\"flow.crash-reports\"]", "applies_to = []"), ComplyError::BadValue, "applies_to");
    check_refusal(with("rows = [\"STORE.1\"]", "rows = [\"STORE..1\"]"), ComplyError::BadValue, "rows");
    check_refusal(with(R"(applies_to = ["product.store", "flow.crash-reports"])",
                       R"(applies_to = ["product.store", "product.store"])"),
                  ComplyError::BadValue, "applies_to");
}

TEST_CASE("comply: scopes and rules are unique and named", "[comply]")
{
    check_refusal(with("id = \"product.store\"", "id = \"flow.crash-reports\""), ComplyError::DuplicateScope, "id");
    check_refusal(with("applies_to = [\"flow.crash-reports\"]", "applies_to = [\"flow.telemetry\"]"),
                  ComplyError::UnknownScope, "applies_to");
    check_refusal(with("id = \"tr.verbis.registration\"", "id = \"tr.kvkk.processing\""), ComplyError::DuplicateRule,
                  "id");
}

TEST_CASE("comply: days, sunsets and counsel days are consistent", "[comply]")
{
    check_refusal(with("effective = \"2016-04-07\"", "effective = \"2016-04-31\""), ComplyError::BadDay, "effective");
    check_refusal(with("effective = \"2016-04-07\"", "effective = \"\""), ComplyError::BadDay, "effective");
    check_refusal(with("register = \"2026-10-31\"", "register = \"\""), ComplyError::BadDay, "register");
    check_refusal(with("register = \"2026-10-31\"", "register = \"2026-10-32\""), ComplyError::BadDay, "register");
    check_refusal(with("sunset = \"2027-01-01\"", "sunset = \"2026-11-01\""), ComplyError::BadDay, "sunset");
    check_refusal(with("sunset = \"2027-01-01\"", "sunset = \"2026-10\""), ComplyError::BadDay, "sunset");
    CHECK(decode(with("sunset = \"2027-01-01\"", "sunset = \"2026-11-02\"")).error == ComplyError::None);
    check_refusal(with("counsel_reviewed = \"2026-10-30\"", "counsel_reviewed = \"\""), ComplyError::BadDay,
                  "counsel_reviewed");
    check_refusal(with("counsel = \"needed\"\ncounsel_reviewed = \"\"", "counsel = \"needed\"\ncounsel_reviewed = "
                                                                        "\"2026-10-01\""),
                  ComplyError::BadDay, "counsel_reviewed");
}

TEST_CASE("comply: an implemented rule cannot claim a proof the register would refuse", "[comply]")
{
    check_refusal(with("check = \"active\"", "check = \"planned\""), ComplyError::BadValue, "state");
    check_refusal(with("read = true", "read = false"), ComplyError::BadValue, "state");
    check_refusal(with("counsel = \"reviewed\"\ncounsel_reviewed = \"2026-10-30\"",
                       "counsel = \"needed\"\ncounsel_reviewed = \"\""),
                  ComplyError::BadValue, "state");
    // A planned rule may already have an active check: it is still unproven.
    RuleGeneration generation;
    REQUIRE(decode_rules(with("check = \"planned\"", "check = \"active\""), generation).error == ComplyError::None);
    CHECK(proof_of(generation.rules[0]) == Proof::Unproven);
    CHECK(proof_of(generation.rules[1]) == Proof::Proven);
}

TEST_CASE("comply: the store adopts only newer generations, unchanged on every refusal", "[comply]")
{
    RuleStore store;
    CHECK(store.generation("TR") == 0U);
    REQUIRE(store.apply(kBase).error == ComplyError::None);
    CHECK(store.generation("TR") == 3U);
    CHECK(store.apply(kBase).error == ComplyError::AlreadyCurrent);

    const cont::String changed = with("Rate games by age.", "Rate games by age and content.");
    const DecodeFailure conflict = store.apply(changed);
    CHECK(conflict.error == ComplyError::Conflict);
    CHECK(conflict.key == "generation");
    CHECK(store.apply(with("generation = 3", "generation = 2", changed)).error == ComplyError::Rollback);
    CHECK(store.apply(with("cerid-rules/1", "cerid-rules/9")).error == ComplyError::UnsupportedFormat);
    REQUIRE(query(store, "product.store", "2026-12-01", {"TR"}).size() == 1U);
    CHECK(cont::StringView(query(store, "product.store", "2026-12-01", {"TR"})[0].rule.obligation) ==
          "Rate games by age.");

    REQUIRE(store.apply(with("generation = 3", "generation = 4", changed)).error == ComplyError::None);
    CHECK(store.generation("TR") == 4U);
    const cont::Array<Binding> swapped = query(store, "product.store", "2026-12-01", {"TR"});
    REQUIRE(swapped.size() == 1U);
    CHECK(swapped[0].generation == 4U);
    CHECK(cont::StringView(swapped[0].rule.obligation) == "Rate games by age and content.");

    // Jurisdictions are independent; the store holds kMaxJurisdictions of them.
    REQUIRE(store.apply(with("jurisdiction = \"TR\"", "jurisdiction = \"EU\"")).error == ComplyError::None);
    CHECK(store.generation("EU") == 3U);
    CHECK(store.generation("TR") == 4U);
    for (u32 i = 2U; i < kMaxJurisdictions; ++i)
    {
        const char code[3] = {static_cast<char>('A' + (i / 26U)), static_cast<char>('A' + (i % 26U)), '\0'};
        cont::String line("jurisdiction = \"");
        line.append(code);
        line.append("\"");
        REQUIRE(store.apply(with("jurisdiction = \"TR\"", line)).error == ComplyError::None);
    }
    const DecodeFailure full = store.apply(with("jurisdiction = \"TR\"", "jurisdiction = \"ZZ\""));
    CHECK(full.error == ComplyError::StoreFull);
    CHECK(store.generation("ZZ") == 0U);
    CHECK(store.apply(with("generation = 3", "generation = 5")).error == ComplyError::None);
}

TEST_CASE("comply: the query answers rules in force for the scope, in jurisdiction order", "[comply]")
{
    RuleStore store;
    REQUIRE(store.apply(kBase).error == ComplyError::None);
    REQUIRE(store.apply(with("jurisdiction = \"TR\"", "jurisdiction = \"EU\"")).error == ComplyError::None);

    // tr.kvkk.processing from 2016-04-07; tr.5651.game-platform in [2026-11-01, 2027-01-01).
    CHECK(query(store, "flow.crash-reports", "2016-04-06", {"TR"}).empty());
    cont::Array<Binding> crash = query(store, "flow.crash-reports", "2016-04-07", {"TR"});
    REQUIRE(crash.size() == 1U);
    CHECK(cont::StringView(crash[0].rule.id) == "tr.kvkk.processing");
    CHECK(cont::StringView(crash[0].jurisdiction) == "TR");
    CHECK(crash[0].generation == 3U);
    CHECK(crash[0].scope_state == ScopeState::Live);
    CHECK(crash[0].proof == Proof::Unproven);
    CHECK(query(store, "flow.crash-reports", "2026-10-31", {"TR"}).size() == 1U);
    crash = query(store, "flow.crash-reports", "2026-11-01", {"TR"});
    REQUIRE(crash.size() == 2U);
    CHECK(cont::StringView(crash[1].rule.id) == "tr.5651.game-platform");
    CHECK(crash[1].proof == Proof::Proven);
    CHECK(query(store, "flow.crash-reports", "2026-12-31", {"TR"}).size() == 2U);
    CHECK(query(store, "flow.crash-reports", "2027-01-01", {"TR"}).size() == 1U);

    // The store scope: the platform duty, then the user action from 2027.
    CHECK(query(store, "product.store", "2026-12-31", {"TR"}).size() == 1U);
    const cont::Array<Binding> store_rules = query(store, "product.store", "2027-01-01", {"TR"});
    REQUIRE(store_rules.size() == 1U);
    CHECK(store_rules[0].rule.kind == RuleKind::UserAction);
    CHECK(store_rules[0].scope_state == ScopeState::Planned);

    // Order follows the request; repeats and unknown jurisdictions or scopes answer nothing more.
    const cont::Array<Binding> both = query(store, "flow.crash-reports", "2026-11-01", {"EU", "GB", "TR", "EU"});
    REQUIRE(both.size() == 4U);
    CHECK(cont::StringView(both[0].jurisdiction) == "EU");
    CHECK(cont::StringView(both[1].jurisdiction) == "EU");
    CHECK(cont::StringView(both[2].jurisdiction) == "TR");
    CHECK(query(store, "flow.telemetry", "2026-11-01", {"TR", "EU"}).empty());
    CHECK(query(store, "flow.crash-reports", "2026-11-01", {}).empty());

    // A query appends.
    cont::Array<Binding> out = query(store, "flow.crash-reports", "2026-11-01", {"TR"});
    const cont::StringView codes[] = {"EU"};
    store.query(RuleQuery{"flow.crash-reports", cont::ConstSpan<cont::StringView>(codes, 1U), day("2026-11-01")}, out);
    CHECK(out.size() == 4U);
}

TEST_CASE("comply: in_force and proof_of at their edges", "[comply]")
{
    Rule rule;
    CHECK_FALSE(in_force(rule, 20990101U)); // no effective day: never in force
    rule.effective = 20261101U;
    CHECK_FALSE(in_force(rule, 20261031U));
    CHECK(in_force(rule, 20261101U));
    CHECK(in_force(rule, 99991231U));
    rule.sunset = 20261102U;
    CHECK(in_force(rule, 20261101U));
    CHECK_FALSE(in_force(rule, 20261102U));
    CHECK(proof_of(rule) == Proof::Unproven);
    rule.state = RuleState::Implemented;
    CHECK(proof_of(rule) == Proof::Unproven);
    rule.check = CheckState::Active;
    CHECK(proof_of(rule) == Proof::Proven);
}

TEST_CASE("comply: a query never sees half a swap", "[comply]")
{
    // Generation n carries n rules on the scope; a reader checks every answer is one whole generation.
    auto generation_text = [](u32 number)
    {
        cont::String text("format = \"cerid-rules/1\"\njurisdiction = \"TR\"\ngeneration = ");
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%u", number);
        text.append(buffer);
        text.append("\nregister = \"2026-10-31\"\n[[scope]]\nid = \"flow.x\"\nkind = \"flow\"\nstate = \"live\"\n");
        for (u32 i = 0U; i < number; ++i)
        {
            std::snprintf(buffer, sizeof(buffer), "%u", i);
            text.append("[[rule]]\nid = \"r.");
            text.append(buffer);
            text.append("\"\nkind = \"obligation\"\ntopic = \"privacy\"\nstate = \"planned\"\ntitle = \"t\"\n"
                        "instrument = \"i\"\narticle = \"a\"\nprimary = \"https://x.example\"\nread = false\n"
                        "obligation = \"o\"\napplies_to = [\"flow.x\"]\ncheck = \"planned\"\ncheck_name = \"c\"\n"
                        "check_ref = \"r\"\neffective = \"2020\"\nsunset = \"\"\ncounsel = \"none\"\n"
                        "counsel_reviewed = \"\"\nrows = [\"COMPLY.0\"]\n");
        }
        return text;
    };
    constexpr u32 last = 40U;
    cont::Array<cont::String> texts;
    for (u32 number = 1U; number <= last; ++number)
    {
        texts.push_back(generation_text(number));
    }
    RuleStore store;
    REQUIRE(store.apply(texts[0]).error == ComplyError::None);
    std::atomic<bool> torn{false};
    std::atomic<bool> done{false};
    std::thread reader(
        [&]
        {
            while (!done.load())
            {
                const cont::Array<Binding> answer = query(store, "flow.x", "2026-01-01", {"TR"});
                const u64 number = answer.empty() ? 0U : answer[0].generation;
                bool whole = answer.size() == number;
                for (const Binding& binding : answer)
                {
                    whole = whole && binding.generation == number;
                }
                torn = torn.load() || !whole;
            }
        });
    for (u32 number = 2U; number <= last; ++number)
    {
        CHECK(store.apply(texts[number - 1U]).error == ComplyError::None);
    }
    done = true;
    reader.join();
    CHECK_FALSE(torn.load());
    CHECK(store.generation("TR") == last);
}

TEST_CASE("comply: error names", "[comply]")
{
    CHECK(comply_error_name(ComplyError::None) == "none");
    CHECK(comply_error_name(ComplyError::TooLarge) == "too-large");
    CHECK(comply_error_name(ComplyError::Syntax) == "syntax");
    CHECK(comply_error_name(ComplyError::UnsupportedFormat) == "unsupported-format");
    CHECK(comply_error_name(ComplyError::MissingKey) == "missing-key");
    CHECK(comply_error_name(ComplyError::UnknownKey) == "unknown-key");
    CHECK(comply_error_name(ComplyError::WrongType) == "wrong-type");
    CHECK(comply_error_name(ComplyError::BadValue) == "bad-value");
    CHECK(comply_error_name(ComplyError::BadDay) == "bad-day");
    CHECK(comply_error_name(ComplyError::BadGeneration) == "bad-generation");
    CHECK(comply_error_name(ComplyError::DuplicateScope) == "duplicate-scope");
    CHECK(comply_error_name(ComplyError::UnknownScope) == "unknown-scope");
    CHECK(comply_error_name(ComplyError::DuplicateRule) == "duplicate-rule");
    CHECK(comply_error_name(ComplyError::DuplicateParty) == "duplicate-party");
    CHECK(comply_error_name(ComplyError::DuplicateDestination) == "duplicate-destination");
    CHECK(comply_error_name(ComplyError::Rollback) == "rollback");
    CHECK(comply_error_name(ComplyError::Conflict) == "conflict");
    CHECK(comply_error_name(ComplyError::AlreadyCurrent) == "already-current");
    CHECK(comply_error_name(ComplyError::StoreFull) == "store-full");
}

TEST_CASE("comply: the committed generations decode, and Turkish law binds the crash pool", "[comply]")
{
    namespace fs = crd::platform::fs;
    RuleStore store;
    u32 loaded = 0U;
    for (const cont::StringView code : {"AU", "BR", "CA", "EU", "GB", "INT", "JP", "PLATFORM", "TR", "US", "US-CA"})
    {
        cont::String path(CRD_COMPLY_RULES_DIR "/");
        for (const char c : code)
        {
            path.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
        path.append(".rules.toml");
        cont::Array<crd::u8> bytes;
        INFO(path.c_str());
        REQUIRE(fs::read_file_binary(fs::Path(cont::StringView(path)), bytes));
        const cont::StringView text(reinterpret_cast<const char*>(bytes.data()), bytes.size()); // NOLINT
        RuleGeneration generation;
        const DecodeFailure failure = decode_rules(text, generation);
        INFO(comply_error_name(failure.error) << " line " << failure.line << " key " << failure.key);
        REQUIRE(failure.error == ComplyError::None);
        CHECK(cont::StringView(generation.jurisdiction) == code);
        CHECK(generation.number >= 1U);
        REQUIRE(store.apply(text).error == ComplyError::None);
        ++loaded;
    }
    CHECK(loaded == 11U);
    // Nothing leaves a machine yet: every rule is planned, so every binding is unproven, and KVKK binds the crash
    // pool in Turkey.
    const cont::Array<Binding> crash = query(store, "flow.crash-reports", "2026-11-01", {"TR", "EU"});
    bool kvkk = false;
    for (const Binding& binding : crash)
    {
        CHECK(binding.proof == Proof::Unproven);
        CHECK(binding.scope_state == ScopeState::Planned);
        kvkk = kvkk || cont::StringView(binding.rule.id) == "tr.kvkk.processing";
    }
    CHECK(kvkk);
}
