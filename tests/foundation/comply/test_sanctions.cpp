// crd-comply -- sanctions and embargo screening (COMPLY.1; docs/design/registers.md#screening): the whole decoder
// and every refusal with its key, periods, the monotonic live swap per authority, request validation, party
// dispositions (match by birth, country or identifier, possible, discounted by kind or birth), destinations
// (comprehensive, sectoral, a region inside a country, a country holding a region), failing closed on a missing,
// stale or future list, a concurrent applier and screener, and a list of 20 000 parties. Every name is invented.

#include <crd/comply/sanctions.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <initializer_list>
#include <thread>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using namespace crd::comply;
namespace cont = crd::containers;

constexpr cont::StringView kList = R"toml(# a test list
format = "cerid-sanctions/1"
authority = "test.ofac"
jurisdiction = "US"
generation = 2026103101
published = "2026-10-31"
max_age_days = 2
source = "https://example.test/sdn.xml"
source_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[destination]]
region = "CU"
program = "TEST-CUBA"
measure = "comprehensive"

[[destination]]
region = "UA-43"
program = "TEST-CRIMEA"
measure = "comprehensive"

[[destination]]
region = "RU"
program = "TEST-RUSSIA-SOFTWARE"
measure = "sectoral"

[[party]]
id = "test.ofac.1001"
kind = "individual"
names = ["Aras Demirkol", "Aras Demirkoll"]
programs = ["TEST-SDGT"]
listed = "2020-05-01"
births = ["1971-03-14", "1972"]
countries = ["TR"]
identifiers = ["passport:A 1234-567"]

[[party]]
id = "test.ofac.1002"
kind = "entity"
names = ["Kuzey Deniz Ticaret A.\u015E."]
programs = ["TEST-SDGT"]
listed = "2021-01"
births = []
countries = ["TR", "AE"]
identifiers = ["registration:998877"]

[[party]]
id = "test.ofac.1003"
kind = "vessel"
names = ["Sea Falcon"]
programs = ["TEST-IRAN"]
listed = "2022"
births = []
countries = ["IR"]
identifiers = ["imo:9123456"]
)toml";

constexpr Day kToday = 20261101U;

// The decimal digits of `value`.
cont::String digits(u64 value)
{
    char buffer[24] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return cont::String(buffer);
}

cont::String joined(std::initializer_list<cont::StringView> parts)
{
    cont::String out;
    for (const cont::StringView part : parts)
    {
        out.append(part);
    }
    return out;
}

cont::String with(cont::StringView from, cont::StringView to, cont::StringView base = kList)
{
    const usize at = base.find(from);
    REQUIRE(at != cont::StringView::npos);
    cont::String text(base.substr(0U, at));
    text.append(to);
    text.append(base.substr(at + from.size()));
    return text;
}

void check_refusal(cont::StringView text, ComplyError error, cont::StringView key)
{
    SanctionsList list;
    const DecodeFailure failure = decode_sanctions(text, list);
    INFO(comply_error_name(failure.error) << " key " << failure.key);
    CHECK(failure.error == error);
    CHECK(failure.key == key);
}

struct Request
{
    cont::Array<cont::StringView> authorities;
    cont::Array<cont::StringView> names;
    cont::Array<cont::StringView> countries;
    cont::Array<cont::StringView> identifiers;
    Subject subject;
    ScreenRequest request;

    Request(std::initializer_list<cont::StringView> lists, std::initializer_list<cont::StringView> subject_names,
            PartyKind kind = PartyKind::Individual)
    {
        for (const cont::StringView authority : lists)
        {
            authorities.push_back(authority);
        }
        for (const cont::StringView name : subject_names)
        {
            names.push_back(name);
        }
        subject.kind = kind;
        request.day = kToday;
        refresh();
    }

    void refresh()
    {
        subject.names = cont::ConstSpan<cont::StringView>(names.data(), names.size());
        subject.countries = cont::ConstSpan<cont::StringView>(countries.data(), countries.size());
        subject.identifiers = cont::ConstSpan<cont::StringView>(identifiers.data(), identifiers.size());
        request.authorities = cont::ConstSpan<cont::StringView>(authorities.data(), authorities.size());
        request.subject = names.empty() ? nullptr : &subject;
    }
};

Screening screen(const SanctionsStore& store, Request& request)
{
    request.refresh();
    Screening out;
    const DecodeFailure failure = store.screen(request.request, out);
    INFO(comply_error_name(failure.error) << " key " << failure.key);
    REQUIRE(failure.error == ComplyError::None);
    return out;
}

const PartyHit* hit_of(const Screening& screening, cont::StringView party)
{
    for (const PartyHit& hit : screening.parties)
    {
        if (cont::StringView(hit.party) == party)
        {
            return &hit;
        }
    }
    return nullptr;
}

SanctionsStore& loaded(SanctionsStore& store)
{
    REQUIRE(store.apply(kList).error == ComplyError::None);
    return store;
}
} // namespace

TEST_CASE("sanctions: a list decodes whole, every field typed", "[comply][sanctions]")
{
    SanctionsList list;
    REQUIRE(decode_sanctions(kList, list).error == ComplyError::None);
    CHECK(list.authority == "test.ofac");
    CHECK(list.jurisdiction == "US");
    CHECK(list.number == 2026103101U);
    CHECK(list.published == 20261031U);
    CHECK(list.max_age_days == 2U);
    CHECK(list.source == "https://example.test/sdn.xml");
    CHECK(list.source_sha256.size() == 64U);
    CHECK(list.digest != 0U);
    REQUIRE(list.destinations.size() == 3U);
    CHECK(list.destinations[1].region == "UA-43");
    CHECK(list.destinations[2].measure == Measure::Sectoral);
    REQUIRE(list.parties.size() == 3U);
    const Party& person = list.parties[0];
    CHECK(person.id == "test.ofac.1001");
    CHECK(person.kind == PartyKind::Individual);
    REQUIRE(person.names.size() == 2U);
    REQUIRE(person.folded.size() == 2U);
    CHECK(person.folded[0].ends.size() == 2U);
    CHECK(person.listed == 20200501U);
    REQUIRE(person.births.size() == 2U);
    CHECK(person.births[0].first == 19710314U);
    CHECK(person.births[0].last == 19710314U);
    CHECK(person.births[1].first == 19720101U);
    CHECK(person.births[1].last == 19721231U);
    REQUIRE(person.identifiers.size() == 1U);
    CHECK(person.identifiers[0] == "passport:a1234567");
    CHECK(list.parties[1].listed == 20210101U);
    CHECK(list.parties[1].folded[0].ends.size() == 3U); // A.S. is a legal form
    CHECK(list.parties[2].kind == PartyKind::Vessel);

    SanctionsList again;
    REQUIRE(decode_sanctions(kList, again).error == ComplyError::None);
    CHECK(again.digest == list.digest);
    REQUIRE(decode_sanctions(with("max_age_days = 2", "max_age_days = 3"), again).error == ComplyError::None);
    CHECK(again.digest != list.digest);
}

TEST_CASE("sanctions: periods cover their days", "[comply][sanctions]")
{
    Period period;
    REQUIRE(parse_period("1971-03-14", period));
    CHECK(period.first == 19710314U);
    CHECK(period.last == 19710314U);
    REQUIRE(parse_period("2024-02", period));
    CHECK(period.first == 20240201U);
    CHECK(period.last == 20240229U);
    REQUIRE(parse_period("2023-02", period));
    CHECK(period.last == 20230228U);
    REQUIRE(parse_period("1999", period));
    CHECK(period.first == 19990101U);
    CHECK(period.last == 19991231U);
    for (const cont::StringView bad : {"", "1899", "2023-02-29", "2026-13", "71-03-14", "1971-3-14"})
    {
        INFO(bad);
        CHECK_FALSE(parse_period(bad, period));
    }
}

TEST_CASE("sanctions: format, syntax, size and empty-list refusals", "[comply][sanctions]")
{
    check_refusal(with("cerid-sanctions/1", "cerid-sanctions/2"), ComplyError::UnsupportedFormat, "format");
    SanctionsList list;
    CHECK(decode_sanctions("format = ", list).error == ComplyError::Syntax);
    const cont::String huge(kMaxSanctionsBytes + 1U, ' ');
    CHECK(decode_sanctions(huge, list).error == ComplyError::TooLarge);
    const usize at = kList.find("[[destination]]");
    check_refusal(kList.substr(0U, at), ComplyError::BadValue, "party");
    const cont::String long_name(kMaxTextBytes + 1U, 'a');
    check_refusal(with("\"Sea Falcon\"", joined({"\"", long_name, "\""})), ComplyError::TooLarge, "names");
    cont::String not_tables(kList.substr(0U, kList.find("[[destination]]")));
    not_tables.append("party = [\"x\"]\n");
    check_refusal(not_tables, ComplyError::WrongType, "party");
}

TEST_CASE("sanctions: every required key, no unknown key, every type", "[comply][sanctions]")
{
    for (const cont::StringView key :
         {"authority", "jurisdiction", "generation", "published", "max_age_days", "source", "source_sha256"})
    {
        const usize at = kList.find(joined({key, " = "}));
        REQUIRE(at != cont::StringView::npos);
        const usize end = kList.find('\n', at);
        cont::String text(kList.substr(0U, at));
        text.append(kList.substr(end + 1U));
        check_refusal(text, ComplyError::MissingKey, key);
    }
    check_refusal(with("format = \"cerid-sanctions/1\"\n", ""), ComplyError::MissingKey, "format");
    for (const cont::StringView key :
         {"region = \"CU\"\n", "program = \"TEST-CUBA\"\n", "measure = \"comprehensive\"\n",
          "id = \"test.ofac.1003\"\n", "kind = \"vessel\"\n", "names = [\"Sea Falcon\"]\n",
          "programs = [\"TEST-IRAN\"]\n", "listed = \"2022\"\n", "countries = [\"IR\"]\n",
          "identifiers = [\"imo:9123456\"]\n", "births = []\n"})
    {
        INFO(key);
        SanctionsList list;
        CHECK(decode_sanctions(with(key, ""), list).error == ComplyError::MissingKey);
    }
    check_refusal(with("max_age_days = 2", "max_age_days = 2\nowner = \"x\""), ComplyError::UnknownKey, "");
    check_refusal(with("measure = \"sectoral\"", "measure = \"sectoral\"\nnote = \"x\""), ComplyError::UnknownKey, "");
    check_refusal(with("listed = \"2022\"", "listed = \"2022\"\naddress = \"x\""), ComplyError::UnknownKey, "");
    check_refusal(with("generation = 2026103101", "generation = \"2026103101\""), ComplyError::WrongType, "generation");
    check_refusal(with("max_age_days = 2", "max_age_days = 2.5"), ComplyError::WrongType, "max_age_days");
    check_refusal(with("authority = \"test.ofac\"", "authority = 7"), ComplyError::WrongType, "authority");
    check_refusal(with("names = [\"Sea Falcon\"]", "names = \"Sea Falcon\""), ComplyError::WrongType, "names");
    check_refusal(with("names = [\"Sea Falcon\"]", "names = [7]"), ComplyError::WrongType, "names");
}

TEST_CASE("sanctions: values, identifiers and enumerations are closed", "[comply][sanctions]")
{
    check_refusal(with("authority = \"test.ofac\"", "authority = \"Test.OFAC\""), ComplyError::BadValue, "authority");
    check_refusal(with("jurisdiction = \"US\"", "jurisdiction = \"usa\""), ComplyError::BadValue, "jurisdiction");
    check_refusal(with("generation = 2026103101", "generation = 0"), ComplyError::BadGeneration, "generation");
    check_refusal(with("published = \"2026-10-31\"", "published = \"2026-10-32\""), ComplyError::BadDay, "published");
    check_refusal(with("published = \"2026-10-31\"", "published = \"\""), ComplyError::BadDay, "published");
    check_refusal(with("max_age_days = 2", "max_age_days = 0"), ComplyError::BadValue, "max_age_days");
    check_refusal(with("max_age_days = 2", "max_age_days = 367"), ComplyError::BadValue, "max_age_days");
    SanctionsList longest;
    CHECK(decode_sanctions(with("max_age_days = 2", "max_age_days = 366"), longest).error == ComplyError::None);
    check_refusal(with("https://example.test/sdn.xml", "http://example.test/sdn.xml"), ComplyError::BadValue, "source");
    check_refusal(with("0123456789abcdef0123", "0123456789ABCDEF0123"), ComplyError::BadValue, "source_sha256");
    check_refusal(with("0123456789abcdef0123", "0123456789abcdef012"), ComplyError::BadValue, "source_sha256");
    check_refusal(with("region = \"CU\"", "region = \"Cuba\""), ComplyError::BadValue, "region");
    check_refusal(with("region = \"UA-43\"", "region = \"UA-\""), ComplyError::BadValue, "region");
    check_refusal(with("measure = \"comprehensive\"", "measure = \"total\""), ComplyError::BadValue, "measure");
    check_refusal(with("program = \"TEST-CUBA\"", "program = \"\""), ComplyError::BadValue, "program");
    check_refusal(with("id = \"test.ofac.1003\"", "id = \"test ofac 1003\""), ComplyError::BadValue, "id");
    check_refusal(with("kind = \"vessel\"", "kind = \"ship\""), ComplyError::BadValue, "kind");
    check_refusal(with("listed = \"2022\"", "listed = \"\""), ComplyError::BadDay, "listed");
    check_refusal(with("names = [\"Sea Falcon\"]", "names = []"), ComplyError::BadValue, "names");
    check_refusal(with("names = [\"Sea Falcon\"]", "names = [\" - \"]"), ComplyError::BadValue, "names");
    // A byte that is no UTF-8 (0xFF): refused by the TOML reader or by the name check.
    const cont::String ill_formed_text = with("Sea Falcon", "Sea \xFF.Falcon");
    SanctionsList ill_formed;
    CHECK(decode_sanctions(ill_formed_text, ill_formed).error != ComplyError::None);
    check_refusal(with("programs = [\"TEST-IRAN\"]", "programs = [\"\"]"), ComplyError::BadValue, "programs");
    check_refusal(with("programs = [\"TEST-IRAN\"]", "programs = []"), ComplyError::BadValue, "programs");
    check_refusal(with(R"(births = ["1971-03-14", "1972"])", R"(births = ["1971-02-30"])"), ComplyError::BadDay,
                  "births");
    check_refusal(with("names = [\"Sea Falcon\"]\nprograms = [\"TEST-IRAN\"]\nlisted = \"2022\"\nbirths = []",
                       "names = [\"Sea Falcon\"]\nprograms = [\"TEST-IRAN\"]\nlisted = \"2022\"\nbirths = [\"1990\"]"),
                  ComplyError::BadValue, "births");
    check_refusal(with("countries = [\"IR\"]", "countries = [\"IRN\"]"), ComplyError::BadValue, "countries");
    check_refusal(with("countries = [\"IR\"]", "countries = [\"ir\"]"), ComplyError::BadValue, "countries");
    check_refusal(with("imo:9123456", "9123456"), ComplyError::BadValue, "identifiers");
    check_refusal(with("imo:9123456", "imo:-"), ComplyError::BadValue, "identifiers");
    check_refusal(with("imo:9123456", "IMO:9123456"), ComplyError::BadValue, "identifiers");
}

TEST_CASE("sanctions: parties and destinations are unique", "[comply][sanctions]")
{
    check_refusal(with("id = \"test.ofac.1003\"", "id = \"test.ofac.1001\""), ComplyError::DuplicateParty, "id");
    check_refusal(
        with("region = \"RU\"\nprogram = \"TEST-RUSSIA-SOFTWARE\"", "region = \"CU\"\nprogram = \"TEST-CUBA\""),
        ComplyError::DuplicateDestination, "region");
    SanctionsList list; // one region under two programmes is two destinations
    CHECK(decode_sanctions(with("region = \"RU\"", "region = \"CU\""), list).error == ComplyError::None);
}

TEST_CASE("sanctions: the store adopts only newer generations, unchanged on every refusal", "[comply][sanctions]")
{
    SanctionsStore store;
    CHECK(store.generation("test.ofac") == 0U);
    loaded(store);
    CHECK(store.generation("test.ofac") == 2026103101U);
    CHECK(store.apply(kList).error == ComplyError::AlreadyCurrent);
    CHECK(store.apply(with("max_age_days = 2", "max_age_days = 3")).error == ComplyError::Conflict);
    CHECK(store.apply(with("generation = 2026103101", "generation = 2026103100")).error == ComplyError::Rollback);
    CHECK(store.apply(with("generation = 2026103101", "generation = 2026103102\nbogus = 1")).error ==
          ComplyError::UnknownKey);
    CHECK(store.generation("test.ofac") == 2026103101U);

    // The newer generation delists the vessel: screening sees the new list only.
    const usize at = kList.find("[[party]]\nid = \"test.ofac.1003\"");
    cont::String next = with("generation = 2026103101", "generation = 2026110101", kList.substr(0U, at));
    REQUIRE(store.apply(next).error == ComplyError::None);
    CHECK(store.generation("test.ofac") == 2026110101U);
    Request request({"test.ofac"}, {"Sea Falcon"}, PartyKind::Vessel);
    CHECK(screen(store, request).parties.empty());

    for (u32 i = 1U; i < kMaxLists; ++i)
    {
        cont::String authority("authority = \"test.list-");
        authority.push_back(static_cast<char>('a' + (i % 26U)));
        authority.push_back(static_cast<char>('a' + (i / 26U)));
        authority.push_back('"');
        REQUIRE(store.apply(with("authority = \"test.ofac\"", authority)).error == ComplyError::None);
    }
    const DecodeFailure full = store.apply(with("authority = \"test.ofac\"", "authority = \"test.one-too-many\""));
    CHECK(full.error == ComplyError::StoreFull);
    CHECK(full.key == "authority");
    CHECK(store.generation("test.one-too-many") == 0U);
}

TEST_CASE("sanctions: a malformed request is refused with its key and an empty screening", "[comply][sanctions]")
{
    SanctionsStore store;
    loaded(store);
    const auto refused = [&store](Request& request, ComplyError error, cont::StringView key)
    {
        request.refresh();
        Screening out;
        out.verdict = Verdict::Blocked;
        const DecodeFailure failure = store.screen(request.request, out);
        CHECK(failure.error == error);
        CHECK(failure.key == key);
        CHECK(out.verdict == Verdict::Clear);
        CHECK(out.parties.empty());
    };
    Request none({}, {"Aras Demirkol"});
    refused(none, ComplyError::BadValue, "authorities");
    Request bad_authority({"Test OFAC"}, {"Aras Demirkol"});
    refused(bad_authority, ComplyError::BadValue, "authorities");
    Request no_day({"test.ofac"}, {"Aras Demirkol"});
    no_day.request.day = kNoDay;
    refused(no_day, ComplyError::BadDay, "day");
    Request bad_day({"test.ofac"}, {"Aras Demirkol"});
    bad_day.request.day = 20260230U;
    refused(bad_day, ComplyError::BadDay, "day");
    Request policy({"test.ofac"}, {"Aras Demirkol"});
    policy.request.policy = ScreenPolicy{960U, 950U};
    refused(policy, ComplyError::BadValue, "policy");
    policy.request.policy = ScreenPolicy{900U, 1001U};
    refused(policy, ComplyError::BadValue, "policy");
    Request destination({"test.ofac"}, {});
    destination.request.destination = "Cuba";
    refused(destination, ComplyError::BadValue, "destination");
    Request wordless({"test.ofac"}, {" - "});
    refused(wordless, ComplyError::BadValue, "names");
    Request birth({"test.ofac"}, {"Aras Demirkol"});
    birth.subject.birth = "1971-02-30";
    refused(birth, ComplyError::BadDay, "birth");
    Request country({"test.ofac"}, {"Aras Demirkol"});
    country.countries.push_back("Turkey");
    refused(country, ComplyError::BadValue, "countries");
    Request identifier({"test.ofac"}, {"Aras Demirkol"});
    identifier.identifiers.push_back("A1234567");
    refused(identifier, ComplyError::BadValue, "identifiers");
}

TEST_CASE("sanctions: a name match is a match only with a birth, country or identifier that agrees",
          "[comply][sanctions]")
{
    SanctionsStore store;
    loaded(store);

    Request bare({"test.ofac"}, {"Aras Demirkol"});
    Screening screening = screen(store, bare);
    CHECK(screening.verdict == Verdict::Review);
    REQUIRE(screening.parties.size() == 1U);
    CHECK(screening.parties[0].disposition == Disposition::Possible);
    CHECK(screening.parties[0].score == 1000U);
    CHECK(screening.parties[0].name == "Aras Demirkol");
    CHECK(screening.parties[0].authority == "test.ofac");
    CHECK(screening.parties[0].generation == 2026103101U);

    Request born({"test.ofac"}, {"ARAS DEM\xC4\xB0RKOL"});
    born.subject.birth = "1971";
    screening = screen(store, born);
    CHECK(screening.verdict == Verdict::Blocked);
    REQUIRE(screening.parties.size() == 1U);
    CHECK(screening.parties[0].disposition == Disposition::Match);
    CHECK(screening.parties[0].birth_agrees);

    Request resident({"test.ofac"}, {"Demirkol, Aras"});
    resident.countries.push_back("TR");
    screening = screen(store, resident);
    CHECK(screening.verdict == Verdict::Blocked);
    CHECK(screening.parties[0].country_agrees);

    Request passport({"test.ofac"}, {"Aras Demirkol"});
    passport.identifiers.push_back("passport:a1234567");
    screening = screen(store, passport);
    CHECK(screening.verdict == Verdict::Blocked);
    CHECK(screening.parties[0].identifier_agrees);

    // A variant below the match threshold stays possible even when the country agrees.
    Request variant({"test.ofac"}, {"Aras Demirkan"});
    variant.countries.push_back("TR");
    screening = screen(store, variant);
    REQUIRE(screening.parties.size() == 1U);
    CHECK(screening.parties[0].score < 950U);
    CHECK(screening.parties[0].score >= 850U);
    CHECK(screening.parties[0].disposition == Disposition::Possible);
    CHECK(screening.verdict == Verdict::Review);

    // The identifier alone, the name unlike: a possible match to review.
    Request alias({"test.ofac"}, {"Kerem Sonmez"});
    alias.identifiers.push_back("passport:A1234567");
    screening = screen(store, alias);
    REQUIRE(screening.parties.size() == 1U);
    CHECK(screening.parties[0].disposition == Disposition::Possible);
    CHECK(screening.parties[0].score < 850U);
}

TEST_CASE("sanctions: a contradicting kind or birth discounts a name match without dropping it", "[comply][sanctions]")
{
    SanctionsStore store;
    loaded(store);
    Request later({"test.ofac"}, {"Aras Demirkol"});
    later.subject.birth = "1990-06-01";
    later.countries.push_back("TR");
    Screening screening = screen(store, later);
    REQUIRE(screening.parties.size() == 1U);
    CHECK(screening.parties[0].disposition == Disposition::Discounted);
    CHECK(screening.verdict == Verdict::Clear);

    Request company({"test.ofac"}, {"Aras Demirkol"}, PartyKind::Entity);
    screening = screen(store, company);
    REQUIRE(screening.parties.size() == 1U);
    CHECK(screening.parties[0].disposition == Disposition::Discounted);
    CHECK(screening.verdict == Verdict::Clear);

    Request ship({"test.ofac"}, {"SEA FALCON"}, PartyKind::Vessel);
    ship.identifiers.push_back("imo:9123456");
    screening = screen(store, ship);
    CHECK(screening.verdict == Verdict::Blocked);
    CHECK(hit_of(screening, "test.ofac.1003") != nullptr);

    Request firm({"test.ofac"},
                 {"Kuzey Deniz Ticaret Ltd. \xC5\x9E"
                  "ti."},
                 PartyKind::Entity);
    firm.countries.push_back("AE");
    screening = screen(store, firm);
    REQUIRE(hit_of(screening, "test.ofac.1002") != nullptr);
    CHECK(hit_of(screening, "test.ofac.1002")->disposition == Disposition::Match);

    Request stranger({"test.ofac"}, {"Mehmet Yildiz"});
    screening = screen(store, stranger);
    CHECK(screening.parties.empty());
    CHECK(screening.verdict == Verdict::Clear);
}

TEST_CASE("sanctions: destinations block, review or pass by region and measure", "[comply][sanctions]")
{
    SanctionsStore store;
    loaded(store);
    const auto verdict = [&store](cont::StringView region, Disposition* disposition = nullptr)
    {
        Request request({"test.ofac"}, {});
        request.request.destination = region;
        const Screening screening = screen(store, request);
        if (disposition != nullptr)
        {
            REQUIRE(screening.destinations.size() == 1U);
            *disposition = screening.destinations[0].disposition;
        }
        return screening.verdict;
    };
    Disposition disposition = Disposition::Discounted;
    CHECK(verdict("CU", &disposition) == Verdict::Blocked);
    CHECK(disposition == Disposition::Match);
    CHECK(verdict("CU-03", &disposition) == Verdict::Blocked); // a place inside the embargoed country
    CHECK(disposition == Disposition::Match);
    CHECK(verdict("UA-43", &disposition) == Verdict::Blocked); // the embargoed region itself
    CHECK(verdict("UA", &disposition) == Verdict::Review);     // the country: the region is not excluded
    CHECK(disposition == Disposition::Possible);
    CHECK(verdict("UA-30") == Verdict::Clear);             // another region of that country
    CHECK(verdict("RU", &disposition) == Verdict::Review); // sectoral
    CHECK(disposition == Disposition::Match);
    CHECK(verdict("TR") == Verdict::Clear);
}

TEST_CASE("sanctions: a missing, stale or future list fails closed unless a match blocks", "[comply][sanctions]")
{
    SanctionsStore store;
    loaded(store);
    Request both({"test.ofac", "test.eu"}, {"Mehmet Yildiz"});
    Screening screening = screen(store, both);
    CHECK(screening.verdict == Verdict::Unavailable);
    REQUIRE(screening.gaps.size() == 1U);
    CHECK(screening.gaps[0].authority == "test.eu");
    CHECK(screening.gaps[0].gap == Gap::Missing);

    Request fresh({"test.ofac"}, {"Mehmet Yildiz"});
    fresh.request.day = 20261102U; // published + 2 days: still fresh
    CHECK(screen(store, fresh).verdict == Verdict::Clear);
    Request stale({"test.ofac"}, {"Mehmet Yildiz"});
    stale.request.day = 20261103U;
    screening = screen(store, stale);
    CHECK(screening.verdict == Verdict::Unavailable);
    REQUIRE(screening.gaps.size() == 1U);
    CHECK(screening.gaps[0].gap == Gap::Stale);
    Request early({"test.ofac"}, {"Mehmet Yildiz"});
    early.request.day = 20261030U;
    screening = screen(store, early);
    CHECK(screening.verdict == Verdict::Unavailable);
    CHECK(screening.gaps[0].gap == Gap::NotYetPublished);

    // A stale list is still screened: its match blocks.
    Request blocked({"test.ofac", "test.eu"}, {"Aras Demirkol"});
    blocked.identifiers.push_back("passport:A1234567");
    blocked.request.day = 20261231U;
    screening = screen(store, blocked);
    CHECK(screening.verdict == Verdict::Blocked);
    CHECK(screening.gaps.size() == 2U);

    // Each authority is screened once.
    Request twice({"test.ofac", "test.ofac"}, {"Aras Demirkol"});
    CHECK(screen(store, twice).parties.size() == 1U);
}

TEST_CASE("sanctions: a screening never sees half a swap", "[comply][sanctions]")
{
    SanctionsStore store;
    loaded(store);
    const usize at = kList.find("[[party]]\nid = \"test.ofac.1003\"");
    std::atomic<bool> stop{false};
    std::thread applier(
        [&store, &stop, at]
        {
            for (u64 n = 1U; n <= 200U; ++n)
            {
                // Odd generations keep the vessel, even ones delist it.
                const cont::String number = joined({"generation = ", digits(2026103101U + n)});
                const cont::StringView base = (n % 2U) != 0U ? kList : kList.substr(0U, at);
                (void)store.apply(with("generation = 2026103101", number, base));
            }
            stop = true;
        });
    u32 seen = 0U;
    while (!stop)
    {
        Request request({"test.ofac"}, {"Sea Falcon"}, PartyKind::Vessel);
        request.identifiers.push_back("imo:9123456");
        const Screening screening = screen(store, request);
        CHECK(
            (screening.parties.empty() ? screening.verdict == Verdict::Clear : screening.verdict == Verdict::Blocked));
        ++seen;
    }
    applier.join();
    CHECK(store.generation("test.ofac") == 2026103101U + 200U);
    CHECK(seen > 0U);
}

TEST_CASE("sanctions: a list of 20 000 parties decodes and screens", "[comply][sanctions]")
{
    cont::String text(kList.substr(0U, kList.find("[[party]]")));
    for (u32 i = 0U; i < 20000U; ++i)
    {
        const cont::String number = digits(100000U + i);
        text.append("[[party]]\nid = \"test.ofac.p");
        text.append(number);
        text.append("\"\nkind = \"entity\"\nnames = [\"Firm ");
        text.append(number);
        text.append(" Holding\"]\nprograms = [\"TEST\"]\nlisted = \"2020\"\nbirths = []\ncountries = []\n"
                    "identifiers = []\n\n");
    }
    SanctionsStore store;
    REQUIRE(store.apply(text).error == ComplyError::None);
    Request request({"test.ofac"}, {"Firm 119999 Holding"}, PartyKind::Entity);
    const Screening screening = screen(store, request);
    REQUIRE(hit_of(screening, "test.ofac.p119999") != nullptr);
    CHECK(hit_of(screening, "test.ofac.p119999")->score == 1000U);
}

TEST_CASE("sanctions: names of verdicts, dispositions, gaps, kinds and measures", "[comply][sanctions]")
{
    CHECK(verdict_name(Verdict::Clear) == "clear");
    CHECK(verdict_name(Verdict::Review) == "review");
    CHECK(verdict_name(Verdict::Unavailable) == "unavailable");
    CHECK(verdict_name(Verdict::Blocked) == "blocked");
    CHECK(disposition_name(Disposition::Match) == "match");
    CHECK(disposition_name(Disposition::Possible) == "possible");
    CHECK(disposition_name(Disposition::Discounted) == "discounted");
    CHECK(gap_name(Gap::Missing) == "missing");
    CHECK(gap_name(Gap::Stale) == "stale");
    CHECK(gap_name(Gap::NotYetPublished) == "not-yet-published");
    CHECK(measure_name(Measure::Comprehensive) == "comprehensive");
    CHECK(measure_name(Measure::Sectoral) == "sectoral");
    for (const cont::StringView name : {"individual", "entity", "vessel", "aircraft"})
    {
        PartyKind kind = PartyKind::Individual;
        REQUIRE(parse_party_kind(name, kind));
        CHECK(party_kind_name(kind) == name);
    }
    PartyKind kind = PartyKind::Vessel;
    CHECK_FALSE(parse_party_kind("ship", kind));
    CHECK(kind == PartyKind::Vessel);
}
