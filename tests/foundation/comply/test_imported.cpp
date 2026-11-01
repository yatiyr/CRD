// crd-comply -- the official sanctions lists as imported (COMPLY.1; docs/design/registers.md#screening):
// scripts/import-sanctions.py writes, from real-byte excerpts of the OFAC SDN List, the EU financial sanctions file,
// the UN Security Council Consolidated List and the UK Sanctions List (tests/foundation/comply/lists), the
// cerid-sanctions/1 generations this case decodes, adopts and screens: the official formats end to end, the birth
// periods the importer derives (ranges, circa, the Hijri and Solar Hijri calendars, an unknown century), identifiers
// with their issuing country, and names in their own scripts; and the US embargoed destinations the importer writes
// from the register's authored reading of 31 CFR chapter V. The CTest crd-comply-sanctions-import writes the
// generations first (a CTest fixture), so the cases are hidden from discovery and run as crd-comply-sanctions-imported.

#include <crd/comply/sanctions.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <initializer_list>

namespace
{
using namespace crd::comply;
namespace cont = crd::containers;
namespace fs = crd::platform::fs;

constexpr Day kDay = 20261101U; // the day the files were read: every list is fresh

struct Imported
{
    const char* authority;
    const char* jurisdiction;
    crd::u64 generation;
    crd::usize parties;
};

constexpr Imported kImported[] = {
    {"us.ofac.sdn", "US", 20261028000000ULL, 8U},
    {"eu.fsf", "EU", 20261023150543ULL, 7U},
    {"un.sc", "INT", 20261031230007ULL, 7U},
    {"gb.fcdo", "GB", 20261030000000ULL, 6U},
};

// The destinations 31 CFR chapter V embargoes, authored in cmake/legal-register.json from the text read on 2026-11-01.
constexpr Imported kEmbargo = {"us.ofac.embargo", "US", 20261101000000ULL, 0U};

// Reads the generation the importer wrote for `expected`, checks its header and adopts it.
SanctionsList adopt(SanctionsStore& store, const Imported& expected)
{
    cont::String path(CRD_COMPLY_IMPORTED_DIR "/");
    path.append(expected.authority);
    path.append(".sanctions.toml");
    INFO(path.c_str());
    cont::Array<crd::u8> bytes;
    REQUIRE(fs::read_file_binary(fs::Path(cont::StringView(path)), bytes));
    const cont::StringView text(reinterpret_cast<const char*>(bytes.data()), bytes.size()); // NOLINT
    SanctionsList list;
    const DecodeFailure failure = decode_sanctions(text, list);
    INFO(comply_error_name(failure.error) << " line " << failure.line << " key " << failure.key);
    REQUIRE(failure.error == ComplyError::None);
    CHECK(list.authority == cont::StringView(expected.authority));
    CHECK(list.jurisdiction == cont::StringView(expected.jurisdiction));
    CHECK(list.number == expected.generation);
    CHECK(list.parties.size() == expected.parties);
    REQUIRE(store.apply(text).error == ComplyError::None);
    CHECK(store.generation(expected.authority) == expected.generation);
    return list;
}

// One subject screened against every imported list.
struct Check
{
    cont::Array<cont::StringView> authorities;
    cont::Array<cont::StringView> names;
    cont::Array<cont::StringView> countries;
    cont::Array<cont::StringView> identifiers;
    Subject subject;
    ScreenRequest request;

    Check(cont::StringView name, PartyKind kind, cont::StringView birth = {},
          std::initializer_list<cont::StringView> subject_countries = {},
          std::initializer_list<cont::StringView> subject_identifiers = {})
    {
        for (const Imported& list : kImported)
        {
            authorities.push_back(list.authority);
        }
        names.push_back(name);
        for (const cont::StringView country : subject_countries)
        {
            countries.push_back(country);
        }
        for (const cont::StringView identifier : subject_identifiers)
        {
            identifiers.push_back(identifier);
        }
        subject.kind = kind;
        subject.birth = birth;
        subject.names = cont::ConstSpan<cont::StringView>(names.data(), names.size());
        subject.countries = cont::ConstSpan<cont::StringView>(countries.data(), countries.size());
        subject.identifiers = cont::ConstSpan<cont::StringView>(identifiers.data(), identifiers.size());
        request.authorities = cont::ConstSpan<cont::StringView>(authorities.data(), authorities.size());
        request.day = kDay;
        request.subject = &subject;
    }

    // The request points into the arrays: a check stays where it was built.
    Check(const Check&) = delete;
    Check& operator=(const Check&) = delete;
};

const PartyHit& hit(const SanctionsStore& store, const Check& check, cont::StringView party, Screening& out)
{
    const DecodeFailure failure = store.screen(check.request, out);
    INFO(comply_error_name(failure.error) << " key " << failure.key);
    REQUIRE(failure.error == ComplyError::None);
    CHECK(out.gaps.empty());
    const PartyHit* found = nullptr;
    for (const PartyHit& each : out.parties)
    {
        found = cont::StringView(each.party) == party ? &each : found;
    }
    INFO(party);
    REQUIRE(found != nullptr);
    return *found;
}

Disposition disposition(const SanctionsStore& store, const Check& check, cont::StringView party)
{
    Screening out;
    return hit(store, check, party, out).disposition;
}
} // namespace

TEST_CASE("sanctions: the imported official lists decode, adopt and screen", "[.imported]")
{
    SanctionsStore store;
    for (const Imported& expected : kImported)
    {
        // Designation lists: the embargoed destinations are other generations.
        CHECK(adopt(store, expected).destinations.empty());
    }

    // OFAC: an exact name alone is possible, with its IMO number or a listed digital currency address a match.
    CHECK(disposition(store, Check("Banco Nacional de Cuba", PartyKind::Entity), "us.ofac.sdn.306") ==
          Disposition::Possible);
    CHECK(disposition(store, Check("Tifon", PartyKind::Vessel, {}, {}, {"imo:7206512"}), "us.ofac.sdn.4249") ==
          Disposition::Match);
    const Check wallet("ISIL Khorasan", PartyKind::Entity, {}, {},
                       {"crypto-xmr:44dZUJ7w1T3fKAvFW8XyXUVoAGSbFvXef2wcbnsjNKGWYorpLJBjth5VKSFhLGkpYKJb2J341tdZHB"
                        "nbpv72WL7e8zuxfR2"});
    Screening screening;
    CHECK(hit(store, wallet, "us.ofac.sdn.18647", screening).identifier_agrees);
    CHECK(screening.verdict == Verdict::Blocked);

    // A passport printed with its issuing country agrees with that country's, or one naming none, not another's.
    const Check turkish("Ceylan Duzcan", PartyKind::Individual, {}, {}, {"passport.tr:315408"});
    CHECK(hit(store, turkish, "us.ofac.sdn.10589", screening).identifier_agrees);
    const Check unnamed("Ceylan Duzcan", PartyKind::Individual, {}, {}, {"passport:315408"});
    CHECK(hit(store, unnamed, "us.ofac.sdn.10589", screening).identifier_agrees);
    const Check german("Ceylan Duzcan", PartyKind::Individual, {}, {}, {"passport.de:315408"});
    CHECK_FALSE(hit(store, german, "us.ofac.sdn.10589", screening).identifier_agrees);

    // OFAC's "1961 to 1962" is one period; "circa 1958" records none, so no birth contradicts it.
    CHECK(disposition(store, Check("Mahmoud Nikousokhan", PartyKind::Individual, "1962-06"), "us.ofac.sdn.15962") ==
          Disposition::Match);
    CHECK(disposition(store, Check("Mahmoud Nikousokhan", PartyKind::Individual, "1970"), "us.ofac.sdn.15962") ==
          Disposition::Discounted);
    CHECK(disposition(store, Check("Senad Sahinpasic", PartyKind::Individual, "1990"), "us.ofac.sdn.7782") ==
          Disposition::Possible);

    // The EU prints an Iranian birth year 1343 in the Islamic calendar: the lunar and the Solar Hijri years agree.
    for (const cont::StringView born : {"1964-12", "1925-01"})
    {
        INFO(born);
        CHECK(disposition(store, Check("Gholamreza Soleimani", PartyKind::Individual, born), "eu.fsf.127388") ==
              Disposition::Match);
    }
    CHECK(disposition(store, Check("Gholamreza Soleimani", PartyKind::Individual, "1990"), "eu.fsf.127388") ==
          Disposition::Discounted);
    // "Born by 1980" (an open range) runs from 1900.
    CHECK(disposition(store, Check("Ermies Ghermay", PartyKind::Individual, "1955"), "eu.fsf.117053") ==
          Disposition::Match);

    // The UN's Hijri 24/06/1402 is 19 April 1982, two days either side; the EU prints the same person.
    const Check asiri("Ibrahim Hassan Tali Al-Asiri", PartyKind::Individual, "1982-04-21");
    CHECK(hit(store, asiri, "un.sc.qdi.291", screening).disposition == Disposition::Match);
    CHECK(disposition(store, asiri, "eu.fsf.6177") == Disposition::Discounted); // the EU prints 18 and 19 April
    CHECK(disposition(store, Check("Ibrahim Hassan Tali Al-Asiri", PartyKind::Individual, {}, {},
                                   {"passport.sa:F654645"}),
                      "un.sc.qdi.291") == Disposition::Match);

    // The UK prints "dd/mm/19yy": the century agrees. Names in Cyrillic match their own script.
    CHECK(disposition(store, Check("Ekaterina Romanova", PartyKind::Individual, "1980-08-15"), "gb.fcdo.rus2701") ==
          Disposition::Match);
    CHECK(disposition(store,
                      Check("\xD0\x95\xD0\xBA\xD0\xB0\xD1\x82\xD0\xB5\xD1\x80\xD0\xB8\xD0\xBD\xD0\xB0 "
                            "\xD0\xA0\xD0\x9E\xD0\x9C\xD0\x90\xD0\x9D\xD0\x9E\xD0\x92\xD0\x90",
                            PartyKind::Individual, {}, {"RU"}),
                      "gb.fcdo.rus2701") == Disposition::Match);
    CHECK(disposition(store, Check("Petrel 8", PartyKind::Vessel, {}, {}, {"imo:9562233"}), "gb.fcdo.dpr0075") ==
          Disposition::Match);
}

TEST_CASE("sanctions: the authored US embargo decodes, adopts and screens destinations", "[.imported]")
{
    SanctionsStore store;
    const SanctionsList list = adopt(store, kEmbargo);
    CHECK(list.destinations.size() == 7U);
    CHECK(list.published == 20261101U);
    CHECK(list.max_age_days == 92U);

    const auto screened = [&store](cont::StringView region, Day day, crd::usize hits, Disposition disposition)
    {
        INFO(region << " on " << day);
        const cont::StringView authorities[] = {kEmbargo.authority};
        ScreenRequest request;
        request.authorities = cont::ConstSpan<cont::StringView>(authorities, 1U);
        request.day = day;
        request.destination = region;
        Screening out;
        REQUIRE(store.screen(request, out).error == ComplyError::None);
        REQUIRE(out.destinations.size() == hits);
        for (const DestinationHit& each : out.destinations)
        {
            CHECK(each.disposition == disposition);
            CHECK(each.generation == kEmbargo.generation);
        }
        CHECK(out.gaps.size() == (day > 20270201U ? 1U : 0U));
        return out.verdict;
    };
    // Cuba, Iran and North Korea and the Crimea region (the Autonomous Republic and Sevastopol) block, a place inside
    // an embargoed country too; Ukraine as a whole names the country the region belongs to, which needs review.
    for (const cont::StringView region : {"CU", "IR", "KP", "UA-43", "UA-40", "CU-03"})
    {
        CHECK(screened(region, kDay, 1U, Disposition::Match) == Verdict::Blocked);
    }
    CHECK(screened("UA", kDay, 2U, Disposition::Possible) == Verdict::Review);
    // The Russian Federation and Venezuela are sectoral: a match needs review.
    CHECK(screened("RU", kDay, 1U, Disposition::Match) == Verdict::Review);
    CHECK(screened("VE", kDay, 1U, Disposition::Match) == Verdict::Review);
    // No comprehensive Syria programme is in the text read; another region of Ukraine and Turkey are clear.
    for (const cont::StringView region : {"SY", "UA-30", "TR"})
    {
        CHECK(screened(region, kDay, 0U, Disposition::Match) == Verdict::Clear);
    }
    // The reading is fresh for 92 days (to 2027-02-01); after that it is stale, still blocks what it lists and keeps
    // every other destination from clear.
    CHECK(screened("TR", 20270201U, 0U, Disposition::Match) == Verdict::Clear);
    CHECK(screened("CU", 20270202U, 1U, Disposition::Match) == Verdict::Blocked);
    CHECK(screened("TR", 20270202U, 0U, Disposition::Match) == Verdict::Unavailable);
}
