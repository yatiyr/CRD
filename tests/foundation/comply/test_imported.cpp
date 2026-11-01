// crd-comply -- the official sanctions lists as imported (COMPLY.1; docs/design/registers.md#screening):
// scripts/import-sanctions.py writes, from real-byte excerpts of the OFAC SDN List, the EU financial sanctions file,
// the UN Security Council Consolidated List, the UK Sanctions List and MASAK's answers for Turkey's asset freezes
// (tests/foundation/comply/lists), the cerid-sanctions/1 generations this case decodes, adopts and screens: the
// official formats end to end, the birth periods the importer derives (ranges, circa, the Hijri and Solar Hijri
// calendars, an unknown century, a numeric day read both ways), identifiers with their issuing country, parties of
// unknown kind and names in their own scripts; and the embargoed destinations the importer writes from
// the register's authored readings: 31 CFR chapter V with the Executive orders it incorporates, the EU's Article 215
// regulations, the UK's sanctions regulations with the Export Control Order 2008 and the Security Council's
// resolutions, each destination read in its own instrument. The CTest
// crd-comply-sanctions-import writes the generations first (a CTest fixture), so the cases are hidden from discovery
// and run as crd-comply-sanctions-imported.

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

// Turkey's asset freezes, MASAK's table posted by sanction type: Law No. 6415's Articles 5 (the UN's lists), 6
// (foreign requests) and 7 (domestic freezes), and Law No. 7262; each published the instant it was read.
constexpr Imported kTurkish[] = {
    {"tr.masak.un", "TR", 20261101110457ULL, 6U},
    {"tr.masak.foreign", "TR", 20261101110457ULL, 2U},
    {"tr.masak.domestic", "TR", 20261101110458ULL, 6U},
    {"tr.masak.7262", "TR", 20261101110458ULL, 6U},
};

// The destinations 31 CFR chapter V embargoes with the Executive orders it incorporates, those the EU's Article 215
// regulations restrict, those the UK's sanctions regulations and Export Control Order 2008 restrict and those the
// Security Council's resolutions restrict, authored in cmake/legal-register.json from the texts read on 2026-11-01.
constexpr Imported kUsEmbargo = {"us.ofac.embargo", "US", 20261101000000ULL, 0U};
constexpr Imported kEuEmbargo = {"eu.council.embargo", "EU", 20261101000000ULL, 0U};
constexpr Imported kGbEmbargo = {"gb.fcdo.embargo", "GB", 20261101000000ULL, 0U};
constexpr Imported kUnEmbargo = {"un.sc.embargo", "INT", 20261101000000ULL, 0U};
constexpr Day kEmbargoFresh = 20270201U; // every reading is fresh for 92 days

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
        : Check(kImported, name, kind, birth, subject_countries, subject_identifiers)
    {
    }

    // Screened against `lists` instead of the designation lists of kImported.
    template <crd::usize N>
    Check(const Imported (&lists)[N], cont::StringView name, PartyKind kind, cont::StringView birth = {},
          std::initializer_list<cont::StringView> subject_countries = {},
          std::initializer_list<cont::StringView> subject_identifiers = {})
    {
        for (const Imported& list : lists)
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

// One destination screened against the authored embargoes named: the verdict, after checking that each of the `hits`
// destinations has the disposition and its embargo's generation, and that a reading past its maximum age is a gap.
Verdict screened(const SanctionsStore& store, std::initializer_list<const Imported*> embargoes, cont::StringView region,
                 Day day, crd::usize hits, Disposition disposition)
{
    INFO(region << " on " << day);
    cont::Array<cont::StringView> authorities;
    for (const Imported* embargo : embargoes)
    {
        authorities.push_back(embargo->authority);
    }
    ScreenRequest request;
    request.authorities = cont::ConstSpan<cont::StringView>(authorities.data(), authorities.size());
    request.day = day;
    request.destination = region;
    Screening out;
    REQUIRE(store.screen(request, out).error == ComplyError::None);
    REQUIRE(out.destinations.size() == hits);
    for (const DestinationHit& each : out.destinations)
    {
        CHECK(each.disposition == disposition);
        const Imported* const* named = embargoes.begin();
        while (named != embargoes.end() && !(cont::StringView(each.authority) == (*named)->authority))
        {
            ++named;
        }
        REQUIRE(named != embargoes.end());
        CHECK(each.generation == (*named)->generation);
    }
    CHECK(out.gaps.size() == (day > kEmbargoFresh ? embargoes.size() : 0U));
    return out.verdict;
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

TEST_CASE("sanctions: Turkey's asset freezes from MASAK's table decode, adopt and screen", "[.imported]")
{
    SanctionsStore store;
    for (const Imported& expected : kTurkish)
    {
        CHECK(adopt(store, expected).destinations.empty());
    }

    // A Turkish citizen: the TCKN agrees, the birth (4/13/1978, month first) agrees, a later birth discounts.
    Screening screening;
    const Check yildirim(kTurkish, "Abdi Yildirim", PartyKind::Individual, {}, {}, {"national-id.tr:55771274198"});
    CHECK(hit(store, yildirim, "tr.masak.domestic.24", screening).disposition == Disposition::Match);
    CHECK(screening.verdict == Verdict::Blocked);
    const Check born_then(kTurkish, "Abdi Yildirim", PartyKind::Individual, "1978-04", {"TR"});
    CHECK(disposition(store, born_then, "tr.masak.domestic.24") == Disposition::Match);
    const Check born_later(kTurkish, "Abdi Yildirim", PartyKind::Individual, "1990");
    CHECK(disposition(store, born_later, "tr.masak.domestic.24") == Disposition::Discounted);

    // 7/2/1966 reads both ways: either day agrees, another day of 1966 does not.
    for (const cont::StringView born : {"1966-02-07", "1966-07-02"})
    {
        INFO(born);
        const Check azzouz(kTurkish, "Abd al-Baset Azzouz", PartyKind::Individual, born);
        CHECK(disposition(store, azzouz, "tr.masak.un.5") == Disposition::Match);
    }
    const Check march(kTurkish, "Abd al-Baset Azzouz", PartyKind::Individual, "1966-03");
    CHECK(disposition(store, march, "tr.masak.un.5") == Disposition::Discounted);

    // The table names no kind: an entity's name is never discounted by the subject's kind.
    for (const PartyKind kind : {PartyKind::Entity, PartyKind::Individual})
    {
        const Check brigades(kTurkish, "Abdallah Azzam Brigades", kind);
        CHECK(disposition(store, brigades, "tr.masak.un.7") == Disposition::Possible);
    }

    // Identifiers: an IMO number, a TCKN and a VKN typed together, a VKN whose leading zero the table lost, an
    // Afghan passport written out in Turkish ("D 000857").
    const Check vessel(kTurkish, "Chonmyong Shipping Co", PartyKind::Vessel, {}, {}, {"imo:5571322"});
    CHECK(disposition(store, vessel, "tr.masak.7262.529") == Disposition::Match);
    const Check glued(kTurkish, "Arzu Pesmen", PartyKind::Individual, {}, {}, {"tax-id.tr:7291094811"});
    CHECK(disposition(store, glued, "tr.masak.domestic.347") == Disposition::Match);
    const Check company(kTurkish, "Almjd Land Trading Dis Ticaret Limited Sirketi", PartyKind::Entity, {}, {},
                        {"tax-id.tr:0551606028"});
    CHECK(disposition(store, company, "tr.masak.domestic.292") == Disposition::Match);
    const Check afghan(kTurkish, "Abdullah Hamad Mohammad Karim", PartyKind::Individual, {}, {}, {"passport:D000857"});
    CHECK(disposition(store, afghan, "tr.masak.un.64") == Disposition::Match);

    // An alias in its own script, an alias numbered "3-" glued to the one before it.
    const Check cyrillic(kTurkish,
                         "\xD0\x90\xD0\xA5\xD0\x9C\xD0\x90\xD0\x94 "
                         "\xD0\xA8\xD0\x98\xD0\xA8\xD0\x90\xD0\x9D\xD0\x98",
                         PartyKind::Individual, {}, {"RU"});
    CHECK(disposition(store, cyrillic, "tr.masak.un.211") == Disposition::Match);
    const Check numbered(kTurkish, "Walid Ayssa", PartyKind::Individual, "1996-01-21");
    CHECK(disposition(store, numbered, "tr.masak.domestic.1505") == Disposition::Match);
}

TEST_CASE("sanctions: the authored US embargo decodes, adopts and screens destinations", "[.imported]")
{
    SanctionsStore store;
    const SanctionsList list = adopt(store, kUsEmbargo);
    CHECK(list.destinations.size() == 10U);
    CHECK(list.published == 20261101U);
    CHECK(list.max_age_days == 92U);

    const auto screened_us = [&store](cont::StringView region, Day day, crd::usize hits, Disposition disposition)
    {
        return screened(store, {&kUsEmbargo}, region, day, hits, disposition);
    };
    // Cuba, Iran and North Korea and the Crimea region (the Autonomous Republic and Sevastopol) block, a place inside
    // an embargoed country too; Ukraine as a whole names the country the region belongs to, which needs review.
    for (const cont::StringView region : {"CU", "IR", "KP", "UA-43", "UA-40", "CU-03"})
    {
        CHECK(screened_us(region, kDay, 1U, Disposition::Match) == Verdict::Blocked);
    }
    CHECK(screened_us("UA", kDay, 4U, Disposition::Possible) == Verdict::Review);
    // E.O. 14065's Covered Regions lie in the Donetsk and Luhansk oblasts, their boundary drawn by no subdivision: a
    // place in either needs review.
    CHECK(screened_us("UA-14", kDay, 1U, Disposition::Match) == Verdict::Review);
    CHECK(screened_us("UA-09", kDay, 1U, Disposition::Match) == Verdict::Review);
    // The Russian Federation (chapter V's directives and E.O. 14071's services) and Venezuela are sectoral: a match
    // needs review.
    CHECK(screened_us("RU", kDay, 2U, Disposition::Match) == Verdict::Review);
    CHECK(screened_us("VE", kDay, 1U, Disposition::Match) == Verdict::Review);
    // No comprehensive Syria programme is in the text read; another region of Ukraine and Turkey are clear.
    for (const cont::StringView region : {"SY", "UA-30", "TR"})
    {
        CHECK(screened_us(region, kDay, 0U, Disposition::Match) == Verdict::Clear);
    }
    // The reading is fresh for 92 days (to 2027-02-01); after that it is stale, still blocks what it lists and keeps
    // every other destination from clear.
    CHECK(screened_us("TR", kEmbargoFresh, 0U, Disposition::Match) == Verdict::Clear);
    CHECK(screened_us("CU", 20270202U, 1U, Disposition::Match) == Verdict::Blocked);
    CHECK(screened_us("TR", 20270202U, 0U, Disposition::Match) == Verdict::Unavailable);
}

TEST_CASE("sanctions: the authored EU embargo decodes, adopts and screens destinations", "[.imported]")
{
    SanctionsStore store;
    const SanctionsList list = adopt(store, kEuEmbargo);
    CHECK(list.destinations.size() == 25U);
    CHECK(list.published == 20261101U);
    CHECK(list.max_age_days == 92U);

    const auto screened_eu = [&store](cont::StringView region, Day day, crd::usize hits, Disposition disposition)
    {
        return screened(store, {&kEuEmbargo}, region, day, hits, disposition);
    };
    // No Article 215 regulation embargoes a place outright: Belarus, Crimea and Sevastopol, the DPRK and Syria, and
    // every place an arms, internal-repression or monitoring regime restricts supply to, are sectoral, so a match needs
    // review and nothing blocks.
    for (const cont::StringView region : {"BY", "UA-43", "UA-40", "KP", "SY", "MM", "VE", "LY", "LB", "SO", "SS", "SD",
                                          "CF", "CD", "ZW", "HT"})
    {
        CHECK(screened_eu(region, kDay, 1U, Disposition::Match) == Verdict::Review);
    }
    // Russia is restricted by two regulations (No 833/2014 and the internal-repression regime 2024/1485), a place
    // inside it too, and Iran by three (non-proliferation, human rights, military support): one hit each.
    CHECK(screened_eu("RU", kDay, 2U, Disposition::Match) == Verdict::Review);
    CHECK(screened_eu("RU-MOW", kDay, 2U, Disposition::Match) == Verdict::Review);
    CHECK(screened_eu("IR", kDay, 3U, Disposition::Match) == Verdict::Review);
    // Regulation (EU) 2022/263's specified territories lie in four oblasts, their boundary drawn by no subdivision: a
    // place in any of them needs review.
    for (const cont::StringView region : {"UA-14", "UA-65", "UA-09", "UA-23"})
    {
        CHECK(screened_eu(region, kDay, 1U, Disposition::Match) == Verdict::Review);
    }
    // Ukraine as a whole names the country six restricted regions belong to. Cuba (no EU regime), Iraq (its
    // regulation restricts trade in cultural property removed from it, no supply to it), Yemen and Afghanistan (their
    // measures attach only to listed persons), another region of Ukraine and Turkey are clear.
    CHECK(screened_eu("UA", kDay, 6U, Disposition::Possible) == Verdict::Review);
    for (const cont::StringView region : {"CU", "IQ", "YE", "AF", "UA-30", "TR"})
    {
        CHECK(screened_eu(region, kDay, 0U, Disposition::Match) == Verdict::Clear);
    }
    // Past its maximum age the reading is a gap that keeps even a listed destination from a settled answer.
    CHECK(screened_eu("RU", 20270202U, 2U, Disposition::Match) == Verdict::Unavailable);
}

TEST_CASE("sanctions: a destination screened against the US and EU embargoes together", "[.imported]")
{
    SanctionsStore store;
    adopt(store, kUsEmbargo);
    adopt(store, kEuEmbargo);
    const auto both = [&store](cont::StringView region, crd::usize hits, Disposition disposition)
    {
        return screened(store, {&kUsEmbargo, &kEuEmbargo}, region, kDay, hits, disposition);
    };
    // The comprehensive US embargo blocks whatever the EU's sectoral ones say; each programme's hit is reported (Iran
    // once under chapter V and three times under the EU's regulations).
    CHECK(both("IR", 4U, Disposition::Match) == Verdict::Blocked);
    CHECK(both("KP", 2U, Disposition::Match) == Verdict::Blocked);
    CHECK(both("UA-43", 2U, Disposition::Match) == Verdict::Blocked);
    CHECK(both("CU", 1U, Disposition::Match) == Verdict::Blocked);
    // Donetsk is partial under both; Venezuela sectoral under both; Kherson, Belarus and Myanmar only under the EU;
    // the Russian Federation four times (chapter V's directives, E.O. 14071 and two EU regulations).
    CHECK(both("UA-14", 2U, Disposition::Match) == Verdict::Review);
    CHECK(both("VE", 2U, Disposition::Match) == Verdict::Review);
    CHECK(both("UA-65", 1U, Disposition::Match) == Verdict::Review);
    CHECK(both("BY", 1U, Disposition::Match) == Verdict::Review);
    CHECK(both("MM", 1U, Disposition::Match) == Verdict::Review);
    CHECK(both("RU", 4U, Disposition::Match) == Verdict::Review);
    CHECK(both("TR", 0U, Disposition::Match) == Verdict::Clear);
}

TEST_CASE("sanctions: the authored UK embargo decodes, adopts and screens destinations", "[.imported]")
{
    SanctionsStore store;
    const SanctionsList list = adopt(store, kGbEmbargo);
    CHECK(list.destinations.size() == 40U);
    CHECK(list.published == 20261101U);
    CHECK(list.max_age_days == 92U);

    const auto screened_gb = [&store](cont::StringView region, Day day, crd::usize hits, Disposition disposition)
    {
        return screened(store, {&kGbEmbargo}, region, day, hits, disposition);
    };
    // No UK instrument embargoes a place outright: every place a regulation restricts a supply to is sectoral, so a
    // match needs review and nothing blocks. The places the Export Control Order 2008 embargoes are hit twice (their
    // own regulations and the order's article 20); China, Hong Kong and Macao only by the order.
    for (const cont::StringView region : {"BY", "KP", "MM", "VE", "LY", "LB", "SS", "SD", "CF", "CD", "ZW"})
    {
        CHECK(screened_gb(region, kDay, 2U, Disposition::Match) == Verdict::Review);
    }
    for (const cont::StringView region : {"IQ", "SO", "SY", "HT", "UA-43", "UA-40", "CN", "HK", "MO"})
    {
        CHECK(screened_gb(region, kDay, 1U, Disposition::Match) == Verdict::Review);
    }
    // Iran by both its regimes and the order, Russia by its regulations and the order, a place inside Russia too.
    CHECK(screened_gb("IR", kDay, 3U, Disposition::Match) == Verdict::Review);
    CHECK(screened_gb("RU", kDay, 2U, Disposition::Match) == Verdict::Review);
    CHECK(screened_gb("RU-MOW", kDay, 2U, Disposition::Match) == Verdict::Review);
    // The non-government controlled areas lie in four oblasts, their boundary drawn by no subdivision: a place in any
    // of them needs review.
    for (const cont::StringView region : {"UA-14", "UA-65", "UA-09", "UA-23"})
    {
        CHECK(screened_gb(region, kDay, 1U, Disposition::Match) == Verdict::Review);
    }
    // Ukraine as a whole names the country six restricted regions belong to. Afghanistan, Yemen, Mali, Nicaragua,
    // Guinea and Bosnia and Herzegovina (their measures attach only to designated persons), Cuba (no UK regime),
    // another region of Ukraine and Turkey are clear.
    CHECK(screened_gb("UA", kDay, 6U, Disposition::Possible) == Verdict::Review);
    for (const cont::StringView region : {"AF", "YE", "ML", "NI", "GN", "BA", "CU", "UA-30", "TR"})
    {
        CHECK(screened_gb(region, kDay, 0U, Disposition::Match) == Verdict::Clear);
    }
    // Past its maximum age the reading is a gap that keeps even a listed destination from a settled answer.
    CHECK(screened_gb("RU", 20270202U, 2U, Disposition::Match) == Verdict::Unavailable);
}

TEST_CASE("sanctions: the authored UN embargo decodes, adopts and screens destinations", "[.imported]")
{
    SanctionsStore store;
    const SanctionsList list = adopt(store, kUnEmbargo);
    CHECK(list.destinations.size() == 15U);
    CHECK(list.published == 20261101U);
    CHECK(list.max_age_days == 92U);

    const auto screened_un = [&store](cont::StringView region, Day day, crd::usize hits, Disposition disposition)
    {
        return screened(store, {&kUnEmbargo}, region, day, hits, disposition);
    };
    // No resolution embargoes a place outright: the DPRK (1718 (2006) as the resolutions after it extend it), Iran
    // (1737 (2006) to 1929 (2010), applied again under 2231 (2015)), the arms embargoes on Iraq, Libya, Lebanon,
    // Somalia, South Sudan, Haiti, the DRC and the Central African Republic and the five Darfur states are sectoral,
    // so a match needs review and nothing blocks; a place inside a restricted country matches too.
    for (const cont::StringView region : {"KP", "IR", "IQ", "LY", "LB", "SO", "SS", "HT", "CD", "CF", "SD-DN", "SD-DS",
                                          "SD-DW", "SD-DC", "SD-DE", "KP-01"})
    {
        CHECK(screened_un(region, kDay, 1U, Disposition::Match) == Verdict::Review);
    }
    // Sudan as a whole names the country the five Darfur states belong to, which needs review; another state of
    // Sudan is clear. The ISIL and Al-Qaida, Taliban, Guinea-Bissau and Yemen regimes attach only to the persons
    // listed (the Consolidated List screens them), so Afghanistan, Guinea-Bissau and Yemen are clear, as are Cuba,
    // Syria and Turkey (no Security Council measure on the place).
    CHECK(screened_un("SD", kDay, 5U, Disposition::Possible) == Verdict::Review);
    for (const cont::StringView region : {"SD-KH", "AF", "GW", "YE", "CU", "SY", "TR"})
    {
        CHECK(screened_un(region, kDay, 0U, Disposition::Match) == Verdict::Clear);
    }
    // Past its maximum age the reading is a gap that keeps even a listed destination from a settled answer.
    CHECK(screened_un("KP", 20270202U, 1U, Disposition::Match) == Verdict::Unavailable);
}

TEST_CASE("sanctions: a destination screened against the US, EU, UK and UN embargoes together", "[.imported]")
{
    SanctionsStore store;
    adopt(store, kUsEmbargo);
    adopt(store, kEuEmbargo);
    adopt(store, kGbEmbargo);
    adopt(store, kUnEmbargo);
    const auto all = [&store](cont::StringView region, crd::usize hits)
    {
        return screened(store, {&kUsEmbargo, &kEuEmbargo, &kGbEmbargo, &kUnEmbargo}, region, kDay, hits,
                        Disposition::Match);
    };
    // The comprehensive US embargo still blocks; each authority's programmes are reported (Iran once under chapter V,
    // three times under the EU's regulations, three times under the UK's instruments and once under the resolutions;
    // the DPRK once, once, twice and once).
    CHECK(all("IR", 8U) == Verdict::Blocked);
    CHECK(all("KP", 5U) == Verdict::Blocked);
    CHECK(all("UA-43", 3U) == Verdict::Blocked);
    // The Russian Federation six times (two programmes under each authority but the UN); Iraq only under the UK and
    // the UN, China only under the UK, Kherson under the EU and the UK; Libya once under the EU and the UN and twice
    // under the UK; a Darfur state under the EU's and the UK's Sudan programmes and the UN's Darfur one.
    CHECK(all("RU", 6U) == Verdict::Review);
    CHECK(all("IQ", 2U) == Verdict::Review);
    CHECK(all("CN", 1U) == Verdict::Review);
    CHECK(all("UA-65", 2U) == Verdict::Review);
    CHECK(all("LY", 4U) == Verdict::Review);
    CHECK(all("SD-DN", 4U) == Verdict::Review);
    CHECK(all("TR", 0U) == Verdict::Clear);
}
