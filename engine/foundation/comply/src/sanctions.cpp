#include "decoder.hpp"

#include <crd/comply/sanctions.hpp>
#include <crd/containers/hash_set.hpp>
#include <crd/core/utf8.hpp>

#include <chrono>

namespace crd::comply
{
namespace
{
using detail::is_dotted;
using detail::is_lower_alnum;
using detail::node;

constexpr cont::StringView kFormat = "cerid-sanctions/1";

constexpr cont::StringView kTopKeys[] = {"format",       "authority", "jurisdiction",  "generation",  "published",
                                         "max_age_days", "source",    "source_sha256", "destination", "party"};
constexpr cont::StringView kDestinationKeys[] = {"region", "program", "measure"};
constexpr cont::StringView kPartyKeys[] = {"id",     "kind",   "names",     "programs",
                                           "listed", "births", "countries", "identifiers"};

constexpr cont::StringView kPartyKindNames[] = {"individual", "entity", "vessel", "aircraft"};
constexpr cont::StringView kMeasureNames[] = {"comprehensive", "sectoral"};

[[nodiscard]] bool is_country(cont::StringView text) noexcept
{
    return text.size() == 2U && detail::is_upper(text[0]) && detail::is_upper(text[1]);
}

// Whether the text is well-formed UTF-8 (RFC 3629).
[[nodiscard]] bool is_utf8(cont::StringView text) noexcept
{
    const auto* bytes = reinterpret_cast<const u8*>(text.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    usize i = 0U;
    while (i < text.size())
    {
        const usize length = utf8_sequence_length(bytes + i, text.size() - i);
        if (length == 0U)
        {
            return false;
        }
        i += length;
    }
    return true;
}

// A non-empty, well-formed UTF-8 string.
[[nodiscard]] bool is_text(cont::StringView text) noexcept
{
    return !text.empty() && is_utf8(text);
}

// type:value, the type a dotted lower-case word, the value well-formed UTF-8 holding at least one ASCII letter or
// digit; `out` receives type:value with the value reduced to its ASCII letters and digits in lower case, the form
// identifiers are compared in (A 1234-567 and a1234567 are one passport number).
[[nodiscard]] bool normalise_identifier(cont::StringView text, cont::String& out)
{
    const usize colon = text.find(':');
    if (colon == cont::StringView::npos || !is_dotted(text.substr(0U, colon), is_lower_alnum) ||
        !is_utf8(text.substr(colon + 1U)))
    {
        return false;
    }
    out = cont::String(text.substr(0U, colon + 1U));
    const usize prefix = out.size();
    for (const char c : text.substr(colon + 1U))
    {
        if (detail::is_alnum(c))
        {
            out.push_back(detail::is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c);
        }
    }
    return out.size() > prefix;
}

[[nodiscard]] std::chrono::sys_days days_of(Day day) noexcept
{
    const std::chrono::year_month_day civil{std::chrono::year{static_cast<int>(day / 10000U)},
                                            std::chrono::month{(day / 100U) % 100U}, std::chrono::day{day % 100U}};
    return std::chrono::sys_days{civil};
}

[[nodiscard]] bool is_real_day(Day day) noexcept
{
    const std::chrono::year_month_day civil{std::chrono::year{static_cast<int>(day / 10000U)},
                                            std::chrono::month{(day / 100U) % 100U}, std::chrono::day{day % 100U}};
    return day >= 19000101U && day <= 99991231U && civil.ok();
}

[[nodiscard]] Day day_of(std::chrono::sys_days days) noexcept
{
    const std::chrono::year_month_day civil{days};
    return (static_cast<u32>(static_cast<int>(civil.year())) * 10000U) + (static_cast<u32>(civil.month()) * 100U) +
           static_cast<u32>(civil.day());
}

class Decoder : public detail::TomlDecoder
{
public:
    explicit Decoder(SanctionsList& out) noexcept : m_out(out) {}

    [[nodiscard]] DecodeFailure run(cont::StringView text)
    {
        toml::parse_result parsed;
        const node* root = parse(parsed, text, kMaxSanctionsBytes, "sanctions");
        if (root == nullptr || !known_keys(*root, kTopKeys) || !top(*root) || !destinations(*root) || !parties(*root))
        {
            return m_failure;
        }
        if (m_out.destinations.empty() && m_out.parties.empty())
        {
            return fail(ComplyError::BadValue, root, "party");
        }
        m_out.digest = detail::digest_of(text);
        return m_failure;
    }

private:
    bool top(const node& root)
    {
        cont::StringView authority;
        cont::StringView jurisdiction;
        cont::StringView source;
        cont::StringView sha256;
        i64 age = 0;
        if (!format(root, kFormat) || !string_at(root, "authority", authority) ||
            !string_at(root, "jurisdiction", jurisdiction) || !generation(root, m_out.number) ||
            !day(root, "published", m_out.published, true) || !integer(root, "max_age_days", 1, kMaxListAgeDays, age) ||
            !string_at(root, "source", source) || !string_at(root, "source_sha256", sha256))
        {
            return false;
        }
        if (!is_dotted(authority, is_lower_alnum))
        {
            return refuse(ComplyError::BadValue, root.get("authority"), "authority");
        }
        if (!detail::is_jurisdiction(jurisdiction))
        {
            return refuse(ComplyError::BadValue, root.get("jurisdiction"), "jurisdiction");
        }
        if (!detail::is_https_url(source))
        {
            return refuse(ComplyError::BadValue, root.get("source"), "source");
        }
        bool hex = sha256.size() == 64U;
        for (const char c : sha256)
        {
            hex = hex && (detail::is_digit(c) || (c >= 'a' && c <= 'f'));
        }
        if (!hex)
        {
            return refuse(ComplyError::BadValue, root.get("source_sha256"), "source_sha256");
        }
        m_out.authority = cont::String(authority);
        m_out.jurisdiction = cont::String(jurisdiction);
        m_out.source = cont::String(source);
        m_out.source_sha256 = cont::String(sha256);
        m_out.max_age_days = static_cast<u32>(age);
        return true;
    }

    bool destinations(const node& root)
    {
        const node* entries = tables(root, "destination", kMaxDestinations, true);
        if (entries == nullptr)
        {
            return m_failure.error == ComplyError::None;
        }
        for (const node& table : *entries)
        {
            Destination destination;
            cont::StringView region;
            if (!known_keys(table, kDestinationKeys) || !string_at(table, "region", region) ||
                !owned(table, "program", destination.program) ||
                !enumeration(table, "measure", kMeasureNames, destination.measure))
            {
                return false;
            }
            if (!detail::is_region(region))
            {
                return refuse(ComplyError::BadValue, table.get("region"), "region");
            }
            for (const Destination& earlier : m_out.destinations)
            {
                if (cont::StringView(earlier.region) == region &&
                    cont::StringView(earlier.program) == cont::StringView(destination.program))
                {
                    return refuse(ComplyError::DuplicateDestination, table.get("region"), "region");
                }
            }
            destination.region = cont::String(region);
            m_out.destinations.push_back(static_cast<Destination&&>(destination));
        }
        return true;
    }

    bool parties(const node& root)
    {
        const node* entries = tables(root, "party", kMaxParties, true);
        if (entries == nullptr)
        {
            return m_failure.error == ComplyError::None;
        }
        cont::HashSet<cont::String> ids(entries->size());
        for (const node& table : *entries)
        {
            if (!known_keys(table, kPartyKeys) || !read_party(table, ids))
            {
                return false;
            }
        }
        return true;
    }

    bool read_party(const node& table, cont::HashSet<cont::String>& ids)
    {
        Party& party = m_out.parties.emplace_back();
        cont::StringView id;
        if (!string_at(table, "id", id) || !enumeration(table, "kind", kPartyKindNames, party.kind) ||
            !day(table, "listed", party.listed, true))
        {
            return false;
        }
        if (!is_dotted(id, is_lower_alnum))
        {
            return refuse(ComplyError::BadValue, table.get("id"), "id");
        }
        party.id = cont::String(id);
        if (!ids.insert(party.id))
        {
            return refuse(ComplyError::DuplicateParty, table.get("id"), "id");
        }
        cont::Array<cont::StringView> items;
        if (!list(table, "names", is_utf8, items, kMaxPartyNames))
        {
            return false;
        }
        for (const cont::StringView name : items)
        {
            FoldedName folded = fold_name(name);
            if (folded.ends.empty())
            {
                return refuse(ComplyError::BadValue, table.get("names"), "names");
            }
            party.names.push_back(cont::String(name));
            party.folded.push_back(static_cast<FoldedName&&>(folded));
        }
        items.clear();
        if (!list(table, "programs", is_text, items, kMaxPartyItems))
        {
            return false;
        }
        for (const cont::StringView program : items)
        {
            party.programs.push_back(cont::String(program));
        }
        return read_births(table, party) && read_countries(table, party) && read_identifiers(table, party);
    }

    bool read_births(const node& table, Party& party)
    {
        cont::Array<cont::StringView> items;
        if (!list(table, "births", is_text, items, kMaxPartyItems, true))
        {
            return false;
        }
        if (!items.empty() && party.kind != PartyKind::Individual)
        {
            return refuse(ComplyError::BadValue, table.get("births"), "births");
        }
        for (const cont::StringView text : items)
        {
            Period period;
            if (!parse_period(text, period))
            {
                return refuse(ComplyError::BadDay, table.get("births"), "births");
            }
            party.births.push_back(period);
        }
        return true;
    }

    bool read_countries(const node& table, Party& party)
    {
        cont::Array<cont::StringView> items;
        if (!list(table, "countries", is_country, items, kMaxPartyItems, true))
        {
            return false;
        }
        for (const cont::StringView country : items)
        {
            party.countries.push_back(cont::String(country));
        }
        return true;
    }

    bool read_identifiers(const node& table, Party& party)
    {
        cont::Array<cont::StringView> items;
        if (!list(table, "identifiers", is_text, items, kMaxPartyItems, true))
        {
            return false;
        }
        for (const cont::StringView text : items)
        {
            cont::String normal;
            if (!normalise_identifier(text, normal))
            {
                return refuse(ComplyError::BadValue, table.get("identifiers"), "identifiers");
            }
            party.identifiers.push_back(static_cast<cont::String&&>(normal));
        }
        return true;
    }

    SanctionsList& m_out;
};

[[nodiscard]] cont::StringView authority_of(const SanctionsList& list) noexcept
{
    return list.authority;
}

// The subject in the form it is compared in.
struct Prepared
{
    PartyKind kind = PartyKind::Individual;
    cont::Array<FoldedName> names;
    bool has_birth = false;
    Period birth;
    cont::ConstSpan<cont::StringView> countries;
    cont::Array<cont::String> identifiers;
};

[[nodiscard]] DecodeFailure prepare(const ScreenRequest& request, Prepared& out)
{
    if (request.authorities.empty())
    {
        return DecodeFailure{ComplyError::BadValue, 0U, "authorities"};
    }
    for (const cont::StringView authority : request.authorities)
    {
        if (!is_dotted(authority, is_lower_alnum))
        {
            return DecodeFailure{ComplyError::BadValue, 0U, "authorities"};
        }
    }
    if (!is_real_day(request.day))
    {
        return DecodeFailure{ComplyError::BadDay, 0U, "day"};
    }
    if (request.policy.review > request.policy.match || request.policy.match > kMaxScore)
    {
        return DecodeFailure{ComplyError::BadValue, 0U, "policy"};
    }
    if (!request.destination.empty() && !detail::is_region(request.destination))
    {
        return DecodeFailure{ComplyError::BadValue, 0U, "destination"};
    }
    const Subject* subject = request.subject;
    if (subject == nullptr)
    {
        return DecodeFailure{};
    }
    if (subject->names.empty() || subject->names.size() > kMaxSubjectItems)
    {
        return DecodeFailure{ComplyError::BadValue, 0U, "names"};
    }
    for (const cont::StringView name : subject->names)
    {
        FoldedName folded = fold_name(name);
        if (folded.ends.empty() || name.size() > kMaxTextBytes)
        {
            return DecodeFailure{ComplyError::BadValue, 0U, "names"};
        }
        out.names.push_back(static_cast<FoldedName&&>(folded));
    }
    out.kind = subject->kind;
    out.has_birth = !subject->birth.empty();
    if (out.has_birth && !parse_period(subject->birth, out.birth))
    {
        return DecodeFailure{ComplyError::BadDay, 0U, "birth"};
    }
    if (subject->countries.size() > kMaxSubjectItems)
    {
        return DecodeFailure{ComplyError::BadValue, 0U, "countries"};
    }
    for (const cont::StringView country : subject->countries)
    {
        if (!is_country(country))
        {
            return DecodeFailure{ComplyError::BadValue, 0U, "countries"};
        }
    }
    out.countries = subject->countries;
    if (subject->identifiers.size() > kMaxSubjectItems)
    {
        return DecodeFailure{ComplyError::BadValue, 0U, "identifiers"};
    }
    for (const cont::StringView text : subject->identifiers)
    {
        cont::String normal;
        if (text.size() > kMaxTextBytes || !normalise_identifier(text, normal))
        {
            return DecodeFailure{ComplyError::BadValue, 0U, "identifiers"};
        }
        out.identifiers.push_back(static_cast<cont::String&&>(normal));
    }
    return DecodeFailure{};
}

void screen_destinations(const SanctionsList& list, cont::StringView request, Screening& out)
{
    const cont::StringView country = request.substr(0U, 2U);
    for (const Destination& destination : list.destinations)
    {
        const cont::StringView region = destination.region;
        Disposition disposition = Disposition::Match;
        if (region == request || (region.size() == 2U && region == country))
        {
            disposition = Disposition::Match; // the region itself, or a place inside the embargoed country
        }
        else if (region.size() > 2U && request.size() == 2U && region.substr(0U, 2U) == request)
        {
            disposition = Disposition::Possible; // the country, which the embargoed region is part of
        }
        else
        {
            continue;
        }
        DestinationHit& hit = out.destinations.emplace_back();
        hit.authority = list.authority;
        hit.generation = list.number;
        hit.destination = destination;
        hit.disposition = disposition;
    }
}

void screen_parties(const SanctionsList& list, const Prepared& subject, const ScreenPolicy& policy, Screening& out)
{
    for (const Party& party : list.parties)
    {
        u32 best = 0U;
        usize best_name = 0U;
        for (const FoldedName& name : subject.names)
        {
            for (usize n = 0U; n < party.folded.size(); ++n)
            {
                const u32 score = name_score(name, party.folded[n]);
                if (score > best)
                {
                    best = score;
                    best_name = n;
                }
            }
        }
        bool identifier_agrees = false;
        for (const cont::String& mine : subject.identifiers)
        {
            for (const cont::String& theirs : party.identifiers)
            {
                identifier_agrees = identifier_agrees || cont::StringView(mine) == cont::StringView(theirs);
            }
        }
        if (best < policy.review && !identifier_agrees)
        {
            continue;
        }
        bool birth_agrees = false;
        for (const Period& birth : party.births)
        {
            birth_agrees = birth_agrees || (subject.has_birth && subject.birth.first <= birth.last &&
                                            birth.first <= subject.birth.last);
        }
        const bool birth_contradicts = subject.has_birth && !party.births.empty() && !birth_agrees;
        bool country_agrees = false;
        for (const cont::StringView mine : subject.countries)
        {
            for (const cont::String& theirs : party.countries)
            {
                country_agrees = country_agrees || mine == cont::StringView(theirs);
            }
        }
        const bool kind_contradicts = subject.kind != party.kind;
        Disposition disposition = Disposition::Possible;
        if (best >= policy.review && (kind_contradicts || birth_contradicts))
        {
            disposition = Disposition::Discounted;
        }
        else if (best >= policy.review &&
                 (identifier_agrees || (best >= policy.match && (birth_agrees || country_agrees))))
        {
            disposition = Disposition::Match;
        }
        PartyHit& hit = out.parties.emplace_back();
        hit.authority = list.authority;
        hit.generation = list.number;
        hit.party = party.id;
        hit.name = party.names[best_name];
        hit.score = best;
        hit.disposition = disposition;
        hit.birth_agrees = birth_agrees;
        hit.country_agrees = country_agrees;
        hit.identifier_agrees = identifier_agrees;
    }
}

[[nodiscard]] Verdict verdict_of(const Screening& screening) noexcept
{
    bool review = false;
    for (const PartyHit& hit : screening.parties)
    {
        if (hit.disposition == Disposition::Match)
        {
            return Verdict::Blocked;
        }
        review = review || hit.disposition == Disposition::Possible;
    }
    for (const DestinationHit& hit : screening.destinations)
    {
        if (hit.disposition == Disposition::Match && hit.destination.measure == Measure::Comprehensive)
        {
            return Verdict::Blocked;
        }
        review = true;
    }
    if (!screening.gaps.empty())
    {
        return Verdict::Unavailable;
    }
    return review ? Verdict::Review : Verdict::Clear;
}
} // namespace

bool parse_period(cont::StringView text, Period& out) noexcept
{
    Day first = kNoDay;
    if (!parse_day(text, first))
    {
        return false;
    }
    if (text.size() == 4U)
    {
        out = Period{first, ((first / 10000U) * 10000U) + 1231U};
        return true;
    }
    if (text.size() == 7U)
    {
        const std::chrono::year_month_day_last last{
            std::chrono::year{static_cast<int>(first / 10000U)},
            std::chrono::month_day_last{std::chrono::month{(first / 100U) % 100U}}};
        out = Period{first, day_of(std::chrono::sys_days{last})};
        return true;
    }
    out = Period{first, first};
    return true;
}

DecodeFailure decode_sanctions(cont::StringView text, SanctionsList& out)
{
    out = SanctionsList();
    return Decoder(out).run(text);
}

cont::StringView verdict_name(Verdict verdict) noexcept
{
    switch (verdict)
    {
        case Verdict::Clear:
            return "clear";
        case Verdict::Review:
            return "review";
        case Verdict::Unavailable:
            return "unavailable";
        case Verdict::Blocked:
            return "blocked";
    }
    return "unknown";
}

cont::StringView disposition_name(Disposition value) noexcept
{
    switch (value)
    {
        case Disposition::Match:
            return "match";
        case Disposition::Possible:
            return "possible";
        case Disposition::Discounted:
            return "discounted";
    }
    return "unknown";
}

cont::StringView gap_name(Gap gap) noexcept
{
    switch (gap)
    {
        case Gap::Missing:
            return "missing";
        case Gap::Stale:
            return "stale";
        case Gap::NotYetPublished:
            return "not-yet-published";
    }
    return "unknown";
}

cont::StringView party_kind_name(PartyKind kind) noexcept
{
    const auto index = static_cast<usize>(kind);
    return index < 4U ? kPartyKindNames[index] : cont::StringView("unknown");
}

cont::StringView measure_name(Measure measure) noexcept
{
    const auto index = static_cast<usize>(measure);
    return index < 2U ? kMeasureNames[index] : cont::StringView("unknown");
}

bool parse_party_kind(cont::StringView text, PartyKind& out) noexcept
{
    for (usize i = 0U; i < 4U; ++i)
    {
        if (kPartyKindNames[i] == text)
        {
            out = static_cast<PartyKind>(i);
            return true;
        }
    }
    return false;
}

DecodeFailure SanctionsStore::apply(cont::StringView text)
{
    SanctionsList list;
    const DecodeFailure decoded = decode_sanctions(text, list);
    if (decoded.error != ComplyError::None)
    {
        return decoded;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    return detail::adopt(m_lists, static_cast<SanctionsList&&>(list), authority_of, kMaxLists, "authority");
}

u64 SanctionsStore::generation(cont::StringView authority) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const SanctionsList* found = detail::find_held(m_lists, authority, authority_of);
    return found != nullptr ? found->number : 0U;
}

DecodeFailure SanctionsStore::screen(const ScreenRequest& request, Screening& out) const
{
    out = Screening();
    Prepared subject;
    const DecodeFailure prepared = prepare(request, subject);
    if (prepared.error != ComplyError::None)
    {
        return prepared;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (usize a = 0U; a < request.authorities.size(); ++a)
    {
        const cont::StringView authority = request.authorities[a];
        bool repeats = false;
        for (usize b = 0U; b < a; ++b)
        {
            repeats = repeats || request.authorities[b] == authority;
        }
        if (repeats)
        {
            continue;
        }
        const SanctionsList* list = detail::find_held(m_lists, authority, authority_of);
        if (list == nullptr)
        {
            out.gaps.push_back(ListGap{cont::String(authority), Gap::Missing});
            continue;
        }
        // A stale list is still screened: what it lists blocks, and the gap keeps anything else from reading clear.
        if (request.day < list->published)
        {
            out.gaps.push_back(ListGap{list->authority, Gap::NotYetPublished});
        }
        else if (days_of(request.day) >
                 days_of(list->published) + std::chrono::days(static_cast<int>(list->max_age_days)))
        {
            out.gaps.push_back(ListGap{list->authority, Gap::Stale});
        }
        if (!request.destination.empty())
        {
            screen_destinations(*list, request.destination, out);
        }
        if (request.subject != nullptr)
        {
            screen_parties(*list, subject, request.policy, out);
        }
    }
    out.verdict = verdict_of(out);
    return DecodeFailure{};
}
} // namespace crd::comply
