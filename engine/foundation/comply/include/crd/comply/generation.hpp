#pragma once

// crd-comply -- what every reviewed generation the comply owner decodes shares (COMPLY.0 and COMPLY.1; contract:
// docs/design/registers.md#rule-generations): civil days, the decode and adoption errors and where decoding failed.
// The rule generations (rules.hpp) and the sanctions lists (sanctions.hpp) are decoded whole, refused with the store
// unchanged, and adopted only above their key's current generation number.

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::comply
{
namespace cont = crd::containers;

inline constexpr usize kMaxTextBytes = 4096U; // one string value of a generation

// A civil day as the number yyyymmdd, which orders as days do; 0 is "none". A YYYY or YYYY-MM period counts from its
// first day, as the register guard counts it, so a rule is never in force later than the truth.
using Day = u32;
inline constexpr Day kNoDay = 0U;

// Parses YYYY, YYYY-MM or YYYY-MM-DD (years 1900-9999, a real calendar day) into the period's first day.
[[nodiscard]] bool parse_day(cont::StringView text, Day& out) noexcept;

enum class ComplyError : u8
{
    None,
    TooLarge,             // the text, a count, a list or a string above its bound
    Syntax,               // not TOML crd-toml reads
    UnsupportedFormat,    // format is not the decoder's (cerid-rules/1, cerid-sanctions/1)
    MissingKey,           // a required key is absent
    UnknownKey,           // a key the format does not define
    WrongType,            // a value of another TOML type
    BadValue,             // an empty or malformed identifier or text, or a value outside its enumeration
    BadDay,               // not a real YYYY, YYYY-MM or YYYY-MM-DD day, or a sunset not after its effective day
    BadGeneration,        // generation number 0 or negative
    DuplicateScope,       // a scope listed twice
    UnknownScope,         // a rule applies to a scope the generation does not list
    DuplicateRule,        // a rule id listed twice
    DuplicateParty,       // a sanctioned party's id listed twice
    DuplicateDestination, // an embargoed destination listed twice
    Rollback,             // below the key's current generation
    Conflict,             // the current generation's number with other bytes
    AlreadyCurrent,       // the current generation again: nothing changes
    StoreFull,            // a key beyond the store's bound
};

// "none", "too-large", "syntax", "unsupported-format", "missing-key", "unknown-key", "wrong-type", "bad-value",
// "bad-day", "bad-generation", "duplicate-scope", "unknown-scope", "duplicate-rule", "duplicate-party",
// "duplicate-destination", "rollback", "conflict", "already-current", "store-full".
[[nodiscard]] cont::StringView comply_error_name(ComplyError error) noexcept;

// Where decoding or adoption failed: the error, the 1-based line (0 when the error has none) and the key it concerns.
struct DecodeFailure
{
    ComplyError error = ComplyError::None;
    u32 line = 0U;
    cont::StringView key; // a static name from the format, empty when none
};
} // namespace crd::comply
