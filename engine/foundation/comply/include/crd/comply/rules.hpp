#pragma once

// crd-comply -- the legal and platform rules products consume (COMPLY.0; contract:
// docs/design/registers.md#rule-generations).
//
// cmake/legal-register.json is the authored source; scripts/gen-comply-rules.py writes one reviewed rule generation
// per jurisdiction (assets/comply/<code>.rules.toml, format cerid-rules/1). Rules:
//
//   decoded whole   a generation is decoded and validated completely (bounded sizes, every key known and typed, days
//                   real calendar days, scopes unique and named, enumerations closed) before anything changes.
//   monotonic       a jurisdiction's generation numbers only grow: a lower number is a rollback, the same number with
//                   other bytes a conflict and the same bytes already current; each is refused with the store
//                   unchanged.
//   live swap       apply() replaces one jurisdiction's generation under the store's lock and a query runs under the
//                   same lock, so a query sees one whole generation of each jurisdiction, never a mix.
//   no assumed proof a rule is Proven only when it is implemented with an active check (the register guard holds that
//                   an implemented entry carries evidence); every other rule in force is Unproven.
//
// Authenticity on delivery is the signed envelope NET.2 gives every signed artefact (crd-crypto); this module judges
// content only.

#include <crd/comply/generation.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <mutex>

namespace crd::comply
{
inline constexpr usize kMaxRulesBytes = 1048576U; // one encoded generation
inline constexpr u32 kMaxRules = 4096U;           // rules in one generation
inline constexpr u32 kMaxScopes = 256U;           // scopes in one generation
inline constexpr u32 kMaxListItems = 64U;         // scopes one rule applies to, rows that own it
inline constexpr u32 kMaxJurisdictions = 64U;     // jurisdictions one store holds

enum class RuleKind : u8
{
    Obligation, // a duty of the product or the publisher
    UserAction, // a registration, filing, account or counsel review the publisher takes
};

enum class RuleState : u8
{
    Planned,
    Implemented,
};

enum class ScopeKind : u8
{
    Product,
    Flow,
    Channel,
};

enum class ScopeState : u8
{
    Planned, // nothing reaches people outside the publisher yet
    Live,    // a product shipped, a flow leaves a machine, a channel is in use
};

enum class CheckState : u8
{
    Planned, // the ROADMAP row that adds the check
    Active,  // a test, script or CI step that runs in every tree
};

enum class Counsel : u8
{
    None,     // interpretation needs no counsel
    Needed,   // a counsel review is outstanding (a user action)
    Reviewed, // counsel answered on counsel_reviewed
};

struct Scope
{
    cont::String id;
    ScopeKind kind = ScopeKind::Product;
    ScopeState state = ScopeState::Planned;
};

struct Rule
{
    cont::String id;
    RuleKind kind = RuleKind::Obligation;
    RuleState state = RuleState::Planned;
    cont::String topic;
    cont::String title;
    cont::String instrument;
    cont::String article;
    cont::String primary;        // the official text's https URL
    bool read = false;           // the consolidated text was read from the primary source
    cont::String obligation;     // the duty in plain words
    cont::Array<u32> applies_to; // indices into RuleGeneration::scopes
    CheckState check = CheckState::Planned;
    cont::String check_name;
    cont::String check_ref;
    Day effective = kNoDay;
    Day sunset = kNoDay; // kNoDay: no end
    Counsel counsel = Counsel::None;
    Day counsel_reviewed = kNoDay;
    cont::Array<cont::String> rows; // the owning ROADMAP rows
};

struct RuleGeneration
{
    cont::String jurisdiction; // TR, EU, US-CA, INT, PLATFORM, ...
    u64 number = 0U;
    Day register_day = kNoDay; // the register snapshot the generation was made from
    u64 digest = 0U;           // XXH3-64 of the encoded bytes (the conflict check)
    cont::Array<Scope> scopes;
    cont::Array<Rule> rules;
};

// Decode and validate one generation. On failure `out` is unspecified and the failure says where.
[[nodiscard]] DecodeFailure decode_rules(cont::StringView text, RuleGeneration& out);

enum class Proof : u8
{
    Proven,   // implemented, with an active check
    Unproven, // in force without a proven implementation: what still owes work or evidence
};

// Whether a rule applies on `day`: effective on or before it and its sunset (if any) after it.
[[nodiscard]] bool in_force(const Rule& rule, Day day) noexcept;
[[nodiscard]] Proof proof_of(const Rule& rule) noexcept;

struct RuleQuery
{
    cont::StringView scope;                          // a scope id, such as flow.crash-reports
    cont::ConstSpan<cont::StringView> jurisdictions; // the jurisdictions the product serves
    Day day = kNoDay;
};

struct Binding
{
    cont::String jurisdiction;
    u64 generation = 0U;
    ScopeState scope_state = ScopeState::Planned;
    Proof proof = Proof::Unproven;
    Rule rule;
};

// The installation's rules: one generation per jurisdiction. apply() and query() may run on different threads.
class RuleStore
{
public:
    // Decode `text` and adopt it as its jurisdiction's generation. On any error the store is unchanged.
    [[nodiscard]] DecodeFailure apply(cont::StringView text);

    // The jurisdiction's current generation number, 0 when it has none: what a caller persists as its floor.
    [[nodiscard]] u64 generation(cont::StringView jurisdiction) const;

    // Every rule of the queried jurisdictions that applies to the scope and is in force on the day, in jurisdiction
    // order and then generation order, appended to `out`. A jurisdiction the store does not hold answers nothing.
    void query(const RuleQuery& request, cont::Array<Binding>& out) const;

private:
    mutable std::mutex m_mutex;
    cont::Array<RuleGeneration> m_generations;
};
} // namespace crd::comply
