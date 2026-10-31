// crd-fuzz-comply-sanctions: crd-comply's sanctions lists, name matcher and screening (COMPLY.1;
// docs/design/registers.md#screening) under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a list as text, decoded;
//   1  two names separated by a zero byte, folded and scored;
//   2  a screening request read from the bytes, against a fixed list held by one store.
//
// Oracles, none of them the decoder's or the screener's own verdict:
//   * an accepted list satisfies the contract's invariants (named authority, real days, a bounded age, a SHA-256,
//     unique parties and destinations, ISO regions and countries, births only for individuals and in order, folded
//     names within their bounds, identifiers in compared form) and decodes again to the same digest;
//   * folding is idempotent (the folded words, written as UTF-8 and folded again, are the same words), a name scores
//     1000 against itself, a name without words scores 0 and every score is at most 1000;
//   * a screening's gaps, hits and verdict equal a model restated from the contract: the gap of each named authority
//     from the day, a party hit exactly when the brute-force best score reaches the review threshold or an identifier
//     agrees, its disposition from the agreements, the destinations by region, and the verdict's precedence.
#include <crd/comply/sanctions.hpp>
#include <crd/core/text_fold.hpp>
#include <crd/fuzz/harness.hpp>

#include <cstdio>
#include <initializer_list>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::comply;
namespace cont = crd::containers;

constexpr cont::StringView kList = R"toml(format = "cerid-sanctions/1"
authority = "fuzz.list"
jurisdiction = "TR"
generation = 7
published = "2026-10-31"
max_age_days = 3
source = "https://example.test/list.xml"
source_sha256 = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

[[destination]]
region = "KP"
program = "FUZZ-DPRK"
measure = "comprehensive"

[[destination]]
region = "UA-43"
program = "FUZZ-REGION"
measure = "comprehensive"

[[destination]]
region = "BY"
program = "FUZZ-SECTORAL"
measure = "sectoral"

[[party]]
id = "fuzz.1"
kind = "individual"
names = ["Deniz Kaya", "Denis Kaja"]
programs = ["FUZZ"]
listed = "2020"
births = ["1980-05"]
countries = ["TR"]
identifiers = ["passport:U1234"]

[[party]]
id = "fuzz.2"
kind = "entity"
names = ["Mavi Liman Ticaret A.Ş.", "Blue Harbour Trading"]
programs = ["FUZZ"]
listed = "2021-02-03"
births = []
countries = ["AE"]
identifiers = ["registration:55-66"]

[[party]]
id = "fuzz.3"
kind = "vessel"
names = ["Kuzey Yıldızı"]
programs = ["FUZZ"]
listed = "2022"
births = []
countries = []
identifiers = ["imo:7654321"]
)toml";

[[nodiscard]] bool hex_digest(cont::StringView text) noexcept
{
    bool ok = text.size() == 64U;
    for (const char c : text)
    {
        ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    return ok;
}

[[nodiscard]] bool upper_pair(cont::StringView text) noexcept
{
    return text.size() >= 2U && text[0] >= 'A' && text[0] <= 'Z' && text[1] >= 'A' && text[1] <= 'Z';
}

void decode_checked(const u8* data, usize size)
{
    const cont::StringView text(reinterpret_cast<const char*>(data), size); // NOLINT
    SanctionsList list;
    if (decode_sanctions(text, list).error != ComplyError::None)
    {
        return;
    }
    CRD_FUZZ_REQUIRE(!list.authority.empty() && list.number >= 1U && list.published >= 19000101U);
    CRD_FUZZ_REQUIRE(list.max_age_days >= 1U && list.max_age_days <= kMaxListAgeDays);
    CRD_FUZZ_REQUIRE(hex_digest(list.source_sha256) && cont::StringView(list.source).starts_with("https://"));
    CRD_FUZZ_REQUIRE(!list.destinations.empty() || !list.parties.empty());
    for (usize i = 0U; i < list.destinations.size(); ++i)
    {
        const Destination& destination = list.destinations[i];
        CRD_FUZZ_REQUIRE(upper_pair(destination.region) && !destination.program.empty());
        for (usize j = 0U; j < i; ++j)
        {
            CRD_FUZZ_REQUIRE(list.destinations[j].region != cont::StringView(destination.region) ||
                             list.destinations[j].program != cont::StringView(destination.program));
        }
    }
    for (usize i = 0U; i < list.parties.size(); ++i)
    {
        const Party& party = list.parties[i];
        for (usize j = 0U; j < i; ++j)
        {
            CRD_FUZZ_REQUIRE(list.parties[j].id != cont::StringView(party.id));
        }
        CRD_FUZZ_REQUIRE(!party.names.empty() && party.names.size() == party.folded.size());
        for (const FoldedName& name : party.folded)
        {
            CRD_FUZZ_REQUIRE(!name.ends.empty() && name.ends.size() <= kMaxNameWords);
            CRD_FUZZ_REQUIRE(name.ends.size() == name.skeleton_ends.size());
        }
        CRD_FUZZ_REQUIRE(party.births.empty() || party.kind == PartyKind::Individual);
        for (const Period& birth : party.births)
        {
            CRD_FUZZ_REQUIRE(birth.first != kNoDay && birth.first <= birth.last);
        }
        for (const cont::String& country : party.countries)
        {
            CRD_FUZZ_REQUIRE(country.size() == 2U && upper_pair(country));
        }
        for (const cont::String& identifier : party.identifiers)
        {
            const usize colon = cont::StringView(identifier).find(':');
            CRD_FUZZ_REQUIRE(colon != cont::StringView::npos && colon + 1U < identifier.size());
            for (const char c : cont::StringView(identifier).substr(colon + 1U))
            {
                CRD_FUZZ_REQUIRE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'));
            }
        }
    }
    SanctionsList again;
    CRD_FUZZ_REQUIRE(decode_sanctions(text, again).error == ComplyError::None && again.digest == list.digest);
}

void append_utf8(cont::String& out, u32 cp)
{
    if (cp < 0x80U)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800U)
    {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
    else if (cp < 0x10000U)
    {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

// The folded words written back as UTF-8, separated by spaces.
cont::String written(const FoldedName& name)
{
    cont::String out;
    u32 begin = 0U;
    for (const u32 end : name.ends)
    {
        if (!out.empty())
        {
            out.push_back(' ');
        }
        for (u32 i = begin; i < end; ++i)
        {
            append_utf8(out, name.text[i]);
        }
        begin = end;
    }
    return out;
}

[[nodiscard]] bool same_words(const FoldedName& a, const FoldedName& b) noexcept
{
    if (a.text.size() != b.text.size() || a.ends.size() != b.ends.size())
    {
        return false;
    }
    for (usize i = 0U; i < a.text.size(); ++i)
    {
        if (a.text[i] != b.text[i])
        {
            return false;
        }
    }
    for (usize i = 0U; i < a.ends.size(); ++i)
    {
        if (a.ends[i] != b.ends[i])
        {
            return false;
        }
    }
    return true;
}

void names_checked(const u8* data, usize size)
{
    usize split = 0U;
    while (split < size && data[split] != 0U)
    {
        ++split;
    }
    const cont::StringView first(reinterpret_cast<const char*>(data), split); // NOLINT
    const cont::StringView second = split < size ? cont::StringView(reinterpret_cast<const char*>(data) + split + 1U,
                                                                    size - split - 1U) // NOLINT
                                                 : cont::StringView();
    const FoldedName a = fold_name(first);
    const FoldedName b = fold_name(second);
    for (const FoldedName* name : {&a, &b})
    {
        CRD_FUZZ_REQUIRE(name->ends.size() <= kMaxNameWords);
        u32 begin = 0U;
        for (const u32 end : name->ends)
        {
            CRD_FUZZ_REQUIRE(end > begin && end - begin <= kMaxWordCodePoints);
            begin = end;
        }
        for (const u32 cp : name->text)
        {
            const crd::CodePointFold fold = crd::fold_code_point(cp);
            CRD_FUZZ_REQUIRE(fold.kind == crd::FoldKind::Letter && fold.count == 1U && fold.out[0] == cp);
        }
        CRD_FUZZ_REQUIRE(same_words(fold_name(written(*name)), *name));
        CRD_FUZZ_REQUIRE(name_score(*name, *name) == (name->ends.empty() ? 0U : kMaxScore));
    }
    const u32 score = name_score(a, b);
    CRD_FUZZ_REQUIRE(score <= kMaxScore);
    CRD_FUZZ_REQUIRE(!(a.ends.empty() || b.ends.empty()) || score == 0U);
}

// ---- Screening against a model --------------------------------------------------------------------------------

struct Reader
{
    const u8* data;
    usize size;
    usize at = 0U;

    u8 byte() noexcept { return at < size ? data[at++] : 0U; }
};

constexpr cont::StringView kAuthorities[] = {"fuzz.list", "fuzz.absent"};
// Before the publication day, on it, inside the three days of freshness, on the last fresh day and after.
constexpr Day kDays[] = {20261028U, 20261030U, 20261031U, 20261101U, 20261103U, 20261104U, 20261231U};
constexpr cont::StringView kRegions[] = {"", "KP", "KP-01", "UA", "UA-43", "UA-30", "BY", "TR", "DE-BY"};
constexpr cont::StringView kBirths[] = {"", "1980", "1980-05", "1980-05-17", "1979", "1981-12-31"};
constexpr cont::StringView kCountries[] = {"TR", "AE", "IR", "DE"};
constexpr cont::StringView kIdentifiers[] = {"passport:U-1234", "registration:5566", "imo:7654321", "imo:1"};
constexpr cont::StringView kNames[] = {"Deniz Kaya",
                                       "DEN\xC4\xB0Z KAYA",
                                       "Kaya Deniz",
                                       "Denis Kaja",
                                       "Deniz Kara",
                                       "Mavi Liman Ticaret Ltd. \xC5\x9Eti.",
                                       "Blue Harbor Trading",
                                       "Kuzey Yildizi",
                                       "Guney Yildizi",
                                       "Ali Veli",
                                       "Deniz Ali Kaya"};

[[nodiscard]] bool overlaps(const Period& a, const Period& b) noexcept
{
    return a.first <= b.last && b.first <= a.last;
}

void screening_checked(const u8* data, usize size)
{
    static SanctionsStore* store = []
    {
        auto* held = new SanctionsStore(); // NOLINT(cppcoreguidelines-owning-memory): one store for the process
        CRD_FUZZ_REQUIRE(held->apply(kList).error == ComplyError::None);
        return held;
    }();
    static SanctionsList list = []
    {
        SanctionsList decoded;
        CRD_FUZZ_REQUIRE(decode_sanctions(kList, decoded).error == ComplyError::None);
        return decoded;
    }();

    Reader in{data, size};
    const u8 shape = in.byte();
    cont::Array<cont::StringView> authorities;
    for (u32 i = 0U; i < 1U + (shape & 3U); ++i)
    {
        authorities.push_back(kAuthorities[in.byte() % 2U]);
    }
    cont::Array<cont::StringView> names;
    for (u32 i = 0U; i < ((shape >> 2U) & 3U); ++i)
    {
        names.push_back(kNames[in.byte() % (sizeof(kNames) / sizeof(kNames[0]))]);
    }
    cont::Array<cont::StringView> countries;
    cont::Array<cont::StringView> identifiers;
    const u8 extras = in.byte();
    for (u32 i = 0U; i < (extras & 3U); ++i)
    {
        countries.push_back(kCountries[in.byte() % 4U]);
    }
    for (u32 i = 0U; i < ((extras >> 2U) & 3U); ++i)
    {
        identifiers.push_back(kIdentifiers[in.byte() % 4U]);
    }
    Subject subject;
    subject.kind = static_cast<PartyKind>(in.byte() % 4U);
    subject.names = cont::ConstSpan<cont::StringView>(names.data(), names.size());
    subject.birth = kBirths[in.byte() % 6U];
    subject.countries = cont::ConstSpan<cont::StringView>(countries.data(), countries.size());
    subject.identifiers = cont::ConstSpan<cont::StringView>(identifiers.data(), identifiers.size());
    ScreenRequest request;
    request.authorities = cont::ConstSpan<cont::StringView>(authorities.data(), authorities.size());
    request.day = kDays[in.byte() % 7U];
    request.subject = names.empty() ? nullptr : &subject;
    request.destination = kRegions[in.byte() % 9U];
    request.policy.review = 700U + (in.byte() % 4U) * 50U;
    request.policy.match = request.policy.review + (in.byte() % 3U) * 50U;

    Screening out;
    const DecodeFailure failure = store->screen(request, out);
    CRD_FUZZ_REQUIRE(failure.error == ComplyError::None);

    // Gaps: the absent authority is missing, the list outside [published, published + 3 days] is not fresh.
    usize gap = 0U;
    bool list_seen = false;
    bool absent_seen = false;
    for (const cont::StringView authority : authorities)
    {
        if (authority == "fuzz.absent" && !absent_seen)
        {
            absent_seen = true;
            CRD_FUZZ_REQUIRE(gap < out.gaps.size() && out.gaps[gap].gap == Gap::Missing);
            ++gap;
        }
        if (authority == "fuzz.list" && !list_seen)
        {
            list_seen = true;
            if (request.day < 20261031U || request.day > 20261103U)
            {
                CRD_FUZZ_REQUIRE(gap < out.gaps.size());
                CRD_FUZZ_REQUIRE(out.gaps[gap].gap == (request.day < 20261031U ? Gap::NotYetPublished : Gap::Stale));
                ++gap;
            }
        }
    }
    CRD_FUZZ_REQUIRE(gap == out.gaps.size());

    // Party hits: the brute-force best score and the agreements, in list order.
    usize hit = 0U;
    Period birth;
    const bool has_birth = !subject.birth.empty() && parse_period(subject.birth, birth);
    bool blocked = false;
    bool review = false;
    for (const Party& party : list.parties)
    {
        if (!list_seen || request.subject == nullptr)
        {
            break;
        }
        u32 best = 0U;
        for (const cont::StringView name : names)
        {
            for (const FoldedName& listed : party.folded)
            {
                const u32 score = name_score(fold_name(name), listed);
                best = score > best ? score : best;
            }
        }
        bool identifier = false;
        for (const cont::StringView mine : identifiers)
        {
            cont::String normal(mine.substr(0U, mine.find(':') + 1U));
            for (const char c : mine.substr(mine.find(':') + 1U))
            {
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))
                {
                    normal.push_back(c);
                }
                else if (c >= 'A' && c <= 'Z')
                {
                    normal.push_back(static_cast<char>(c - 'A' + 'a'));
                }
            }
            for (const cont::String& theirs : party.identifiers)
            {
                identifier = identifier || cont::StringView(normal) == cont::StringView(theirs);
            }
        }
        if (best < request.policy.review && !identifier)
        {
            continue;
        }
        bool birth_agrees = false;
        for (const Period& listed : party.births)
        {
            birth_agrees = birth_agrees || (has_birth && overlaps(birth, listed));
        }
        bool country = false;
        for (const cont::StringView mine : countries)
        {
            for (const cont::String& theirs : party.countries)
            {
                country = country || mine == cont::StringView(theirs);
            }
        }
        const bool contradicts = subject.kind != party.kind || (has_birth && !party.births.empty() && !birth_agrees);
        Disposition expected = Disposition::Possible;
        if (best >= request.policy.review && contradicts)
        {
            expected = Disposition::Discounted;
        }
        else if (best >= request.policy.review &&
                 (identifier || (best >= request.policy.match && (birth_agrees || country))))
        {
            expected = Disposition::Match;
        }
        CRD_FUZZ_REQUIRE(hit < out.parties.size());
        const PartyHit& got = out.parties[hit];
        CRD_FUZZ_REQUIRE(got.party == cont::StringView(party.id) && got.score == best);
        CRD_FUZZ_REQUIRE(got.disposition == expected && got.identifier_agrees == identifier);
        CRD_FUZZ_REQUIRE(got.birth_agrees == birth_agrees && got.country_agrees == country);
        blocked = blocked || expected == Disposition::Match;
        review = review || expected == Disposition::Possible;
        ++hit;
    }
    CRD_FUZZ_REQUIRE(hit == out.parties.size());

    // Destinations by region, in list order.
    usize place = 0U;
    const cont::StringView asked = request.destination;
    for (const Destination& destination : list.destinations)
    {
        if (!list_seen || asked.empty())
        {
            break;
        }
        const cont::StringView region = destination.region;
        Disposition expected = Disposition::Discounted; // none
        if (region == asked || (region.size() == 2U && asked.substr(0U, 2U) == region))
        {
            expected = Disposition::Match;
        }
        else if (region.size() > 2U && asked.size() == 2U && region.substr(0U, 2U) == asked)
        {
            expected = Disposition::Possible;
        }
        if (expected == Disposition::Discounted)
        {
            continue;
        }
        CRD_FUZZ_REQUIRE(place < out.destinations.size());
        CRD_FUZZ_REQUIRE(out.destinations[place].destination.region == region &&
                         out.destinations[place].disposition == expected);
        const bool blocks = expected == Disposition::Match && destination.measure == Measure::Comprehensive;
        blocked = blocked || blocks;
        review = review || !blocks;
        ++place;
    }
    CRD_FUZZ_REQUIRE(place == out.destinations.size());

    Verdict expected = review ? Verdict::Review : Verdict::Clear;
    if (blocked)
    {
        expected = Verdict::Blocked;
    }
    else if (!out.gaps.empty())
    {
        expected = Verdict::Unavailable;
    }
    CRD_FUZZ_REQUIRE(out.verdict == expected);
}

void add(crd::fuzz::SeedSink& sink, const char* name, u8 mode, cont::StringView data)
{
    cont::String input;
    input.push_back(static_cast<char>(mode));
    input.append(data);
    sink.add(name, reinterpret_cast<const u8*>(input.data()), input.size()); // NOLINT
}

cont::String replaced(cont::StringView from, cont::StringView to)
{
    const usize at = kList.find(from);
    cont::String text(kList.substr(0U, at));
    text.append(to);
    text.append(kList.substr(at + from.size()));
    return text;
}

// A string literal with its embedded zero bytes, without the terminator.
template <usize N> cont::StringView sized(const char (&text)[N]) // NOLINT(modernize-avoid-c-arrays)
{
    return cont::StringView(text, N - 1U);
}

void add_bytes(crd::fuzz::SeedSink& sink, const char* name, u8 mode, std::initializer_list<u8> bytes)
{
    cont::String data;
    for (const u8 b : bytes)
    {
        data.push_back(static_cast<char>(b));
    }
    add(sink, name, mode, data);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    switch (data[0] % 3U)
    {
        case 0U:
            decode_checked(data + 1U, size - 1U);
            break;
        case 1U:
            names_checked(data + 1U, size - 1U);
            break;
        default:
            screening_checked(data + 1U, size - 1U);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    add(sink, "list", 0U, kList);
    add(sink, "list-truncated", 0U, kList.substr(0U, kList.size() / 2U));
    add(sink, "list-duplicate-party", 0U, replaced("id = \"fuzz.3\"", "id = \"fuzz.1\""));
    add(sink, "list-duplicate-destination", 0U,
        replaced("region = \"BY\"\nprogram = \"FUZZ-SECTORAL\"", "region = \"KP\"\nprogram = \"FUZZ-DPRK\""));
    add(sink, "list-entity-birth", 0U,
        replaced("listed = \"2021-02-03\"\nbirths = []", "listed = \"2021-02-03\"\nbirths = [\"1999\"]"));
    add(sink, "list-bad-region", 0U, replaced("region = \"UA-43\"", "region = \"UA-\""));
    add(sink, "list-stale-age", 0U, replaced("max_age_days = 3", "max_age_days = 400"));
    add(sink, "list-bad-identifier", 0U, replaced("imo:7654321", "IMO:7654321"));
    add(sink, "list-wordless-name", 0U, replaced("\"Blue Harbour Trading\"", "\" .-, \""));
    add(sink, "list-future-format", 0U, replaced("cerid-sanctions/1", "cerid-sanctions/2"));
    add(sink, "names-turkish", 1U,
        sized("\xC5\x9E"
              "ah\xC4\xB1n \xC3\x96zt\xC3\xBCrk\0Sahin Ozturk"));
    add(sink, "names-subset", 1U, sized("Oleg Petrenko\0Oleg Vladimirovich Petrenko"));
    add(sink, "names-joined", 1U, sized("Abdulrahman Al-Qasim\0Abdul Rahman al Qasim"));
    add(sink, "names-legal-forms", 1U, sized("Deniz A.\xC5\x9E.\0L.L.C. Ltd"));
    add(sink, "names-scripts", 1U, sized("\xD0\x99\xD0\xBE\xD0\xB4 \xCE\x86\xCE\xBB\xCF\x82\0\xEF\xBC\xA1\xC3\x9F"));
    add(sink, "names-ill-formed", 1U,
        sized("Iv\xFF"
              "an\xC0\x80\0\xE2\x80"));
    // shape, authorities, names, extras, countries, identifiers, kind, birth, day, destination, review, match
    add_bytes(sink, "screen-match-birth", 2U, {0x04U, 0U, 0U, 0x00U, 0U, 2U, 3U, 0U, 0U});
    add_bytes(sink, "screen-identifier", 2U, {0x04U, 0U, 9U, 0x04U, 0U, 0U, 0U, 4U, 0U, 2U, 2U});
    add_bytes(sink, "screen-discounted", 2U, {0x04U, 0U, 1U, 0x00U, 1U, 4U, 3U, 0U, 1U, 1U});
    add_bytes(sink, "screen-entity", 2U, {0x08U, 0U, 5U, 6U, 0x01U, 1U, 1U, 0U, 3U, 7U, 3U, 0U});
    add_bytes(sink, "screen-missing-stale", 2U, {0x05U, 0U, 1U, 7U, 0x00U, 0U, 0U, 6U, 4U});
    add_bytes(sink, "screen-destinations", 2U, {0x01U, 0U, 1U, 0x00U, 0U, 0U, 3U, 3U});
    add_bytes(sink, "screen-early", 2U, {0x00U, 0U, 0x00U, 0U, 0U, 0U, 2U});
}
