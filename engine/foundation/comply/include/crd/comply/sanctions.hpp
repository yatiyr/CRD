#pragma once

// crd-comply -- sanctions and embargo screening for distribution, accounts and payments (COMPLY.1; contract:
// docs/design/registers.md#screening).
//
// Each authority's list (OFAC, the EU's consolidated list, the UN Security Council's consolidated list, the UK
// sanctions list, Turkey's asset-freeze decisions) arrives as one cerid-sanctions/1 generation imported from the
// authority's own publication: its designated parties and the destinations it embargoes, the publication day, the
// age after which it is stale and the SHA-256 of the official file. Rules:
//
//   decoded whole   bounded sizes, every key known and typed, ids unique, regions ISO 3166, days real, names
//                   well-formed UTF-8, before anything changes.
//   monotonic       an authority's generation numbers only grow (rollback, conflict and already-current refused with
//                   the store unchanged), and apply() swaps one authority's list under the store's lock.
//   fail closed     a list the request names that the store does not hold, or holds stale (screened after its
//                   publication day plus its maximum age, or before its publication day), makes the verdict
//                   Unavailable unless a certain match already blocks: a missing list never reads as clear.
//   no silent drop  a name match that the subject's kind or birth contradicts is reported Discounted, not dropped.
//
// Verdicts: Blocked (a party match, or a destination inside a comprehensive embargo), else Unavailable, else Review
// (a possible party match, a sectoral embargo, or a country whose embargoed region the request does not exclude),
// else Clear. Authenticity on delivery is NET.2's signed envelope; this module judges content only.

#include <crd/comply/generation.hpp>
#include <crd/comply/names.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <mutex>

namespace crd::comply
{
inline constexpr usize kMaxSanctionsBytes = 67108864U; // one encoded list
inline constexpr u32 kMaxParties = 131072U;            // designated parties in one list
inline constexpr u32 kMaxDestinations = 1024U;         // embargoed destinations in one list
inline constexpr u32 kMaxPartyNames = 512U;            // a party's name and aliases
inline constexpr u32 kMaxPartyItems = 64U;             // a party's programmes, births, countries or identifiers
inline constexpr u32 kMaxLists = 32U;                  // authorities one store holds
inline constexpr u32 kMaxListAgeDays = 366U;           // the longest maximum age a list may declare
inline constexpr u32 kMaxSubjectItems = 64U;           // a subject's names, countries or identifiers

enum class PartyKind : u8
{
    Individual,
    Entity,
    Vessel,
    Aircraft,
};

enum class Measure : u8
{
    Comprehensive, // nothing may be supplied to the destination: a match blocks
    Sectoral,      // some goods, services or parties are restricted: a match needs review
};

// A YYYY, YYYY-MM or YYYY-MM-DD read as the days it covers.
struct Period
{
    Day first = kNoDay;
    Day last = kNoDay;
};

[[nodiscard]] bool parse_period(cont::StringView text, Period& out) noexcept;

struct Party
{
    cont::String id; // the authority's reference, such as us.ofac.sdn.12345
    PartyKind kind = PartyKind::Individual;
    cont::Array<cont::String> names; // the primary name first, then the aliases
    cont::Array<FoldedName> folded;  // fold_name() of each name
    cont::Array<cont::String> programs;
    Day listed = kNoDay;
    cont::Array<Period> births;            // individuals only
    cont::Array<cont::String> countries;   // ISO 3166-1 alpha-2: nationality, residence, registration, flag
    cont::Array<cont::String> identifiers; // type:value (passport:..., imo:..., registration:...)
};

struct Destination
{
    cont::String region;  // ISO 3166-1 alpha-2, or a subdivision (UA-43)
    cont::String program; // the authority's programme or regime
    Measure measure = Measure::Comprehensive;
};

struct SanctionsList
{
    cont::String authority;    // us.ofac, eu.fsf, un.sc, gb.ofsi, tr.asset-freeze, ...
    cont::String jurisdiction; // US, EU, INT, GB, TR, ...
    u64 number = 0U;           // the generation: the authority's publications in order
    Day published = kNoDay;
    u32 max_age_days = 0U;
    cont::String source;        // the official file's https URL
    cont::String source_sha256; // its SHA-256, lower-case hex
    u64 digest = 0U;            // XXH3-64 of the encoded bytes (the conflict check)
    cont::Array<Destination> destinations;
    cont::Array<Party> parties;
};

// Decode and validate one list. On failure `out` is unspecified and the failure says where.
[[nodiscard]] DecodeFailure decode_sanctions(cont::StringView text, SanctionsList& out);

// Who is screened: a customer, a payee, a publisher's counterparty.
struct Subject
{
    PartyKind kind = PartyKind::Individual;
    cont::ConstSpan<cont::StringView> names;       // at least one
    cont::StringView birth;                        // "" unknown, else YYYY, YYYY-MM or YYYY-MM-DD
    cont::ConstSpan<cont::StringView> countries;   // ISO 3166-1 alpha-2
    cont::ConstSpan<cont::StringView> identifiers; // type:value
};

// The thresholds of a name match, in thousandths (names.hpp's name_score).
struct ScreenPolicy
{
    u32 review = 850U; // at or above: a possible match to review
    u32 match = 950U;  // at or above, with a birth, country or identifier that agrees: a match
};

struct ScreenRequest
{
    cont::ConstSpan<cont::StringView> authorities; // the lists the product's jurisdictions require; at least one
    Day day = kNoDay;                              // the day screened
    const Subject* subject = nullptr;              // nullptr: the destination only
    cont::StringView destination;                  // "" none, else a region (CU, UA, UA-43)
    ScreenPolicy policy;
};

enum class Verdict : u8
{
    Clear,
    Review,
    Unavailable,
    Blocked,
};

enum class Disposition : u8
{
    Match,      // blocks
    Possible,   // needs review
    Discounted, // the name matched, the subject's kind or birth contradicts it
};

enum class Gap : u8
{
    Missing,         // the store holds no list of the authority
    Stale,           // screened later than the publication day plus the maximum age
    NotYetPublished, // screened before the publication day: the clock or the list is wrong
};

struct ListGap
{
    cont::String authority;
    Gap gap = Gap::Missing;
};

struct PartyHit
{
    cont::String authority;
    u64 generation = 0U;
    cont::String party;
    cont::String name; // the listed name that scored best
    u32 score = 0U;
    Disposition disposition = Disposition::Possible;
    bool birth_agrees = false;
    bool country_agrees = false;
    bool identifier_agrees = false;
};

struct DestinationHit
{
    cont::String authority;
    u64 generation = 0U;
    Destination destination;
    Disposition disposition = Disposition::Match; // Possible: the request names the country, the entry a region
};

struct Screening
{
    Verdict verdict = Verdict::Clear;
    cont::Array<ListGap> gaps;
    cont::Array<PartyHit> parties; // in request authority order, then list order
    cont::Array<DestinationHit> destinations;
};

[[nodiscard]] cont::StringView verdict_name(Verdict verdict) noexcept;       // clear, review, unavailable, blocked
[[nodiscard]] cont::StringView disposition_name(Disposition value) noexcept; // match, possible, discounted
[[nodiscard]] cont::StringView gap_name(Gap gap) noexcept;                   // missing, stale, not-yet-published
[[nodiscard]] cont::StringView party_kind_name(PartyKind kind) noexcept;     // individual, entity, vessel, aircraft
[[nodiscard]] cont::StringView measure_name(Measure measure) noexcept;       // comprehensive, sectoral
[[nodiscard]] bool parse_party_kind(cont::StringView text, PartyKind& out) noexcept;

// The installation's lists: one generation per authority. apply() and screen() may run on different threads.
class SanctionsStore
{
public:
    // Decode `text` and adopt it as its authority's generation. On any error the store is unchanged.
    [[nodiscard]] DecodeFailure apply(cont::StringView text);

    // The authority's current generation number, 0 when it has none: what a caller persists as its floor.
    [[nodiscard]] u64 generation(cont::StringView authority) const;

    // Screen the request against the lists it names (each authority once). A malformed request (no authority, no
    // day, a subject without names or with a bad birth, country or identifier, a bad destination, a review threshold
    // above the match threshold or a threshold above 1000) is refused as BadValue or BadDay with its key, and `out`
    // is left empty.
    [[nodiscard]] DecodeFailure screen(const ScreenRequest& request, Screening& out) const;

private:
    mutable std::mutex m_mutex;
    cont::Array<SanctionsList> m_lists;
};
} // namespace crd::comply
