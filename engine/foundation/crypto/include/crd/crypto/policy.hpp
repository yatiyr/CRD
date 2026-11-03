#pragma once

// crd-crypto -- signed crypto policy generations (NET-STD; contract: docs/design/network-foundation.md#agility,
// #policy-wire and #policy-start).
//
// The algorithms a Cerid installation accepts are not compiled in: they are a policy generation, a signed artefact
// that names every algorithm by its register identifier with a dated window, the minimum version of each Cerid
// protocol, its own validity and an offline grace period. Rules:
//
//   signed      every key of the installation's trust set signs every generation (at least one post-quantum and one
//               traditional key: a hybrid signature, D7); signatures by keys outside the set are ignored, so a later
//               build can add a key without breaking an older one.
//   monotonic   generation numbers only grow. A generation below the newest one the installation ever accepted is
//               refused before any signature is checked; the same number with other content is a conflict.
//   windows     an algorithm is accepted in [accepted_from, refused_from) and preferred in [preferred_from,
//               preferred_until); outside a window, or without a rule, it is refused. A generation that refuses an
//               algorithm at once is the remote disable.
//   scopes      a rule governs every session or only Cerid service sessions, which follow their own rule where the
//               generation has one: the dated refusal of classical-only key exchange reaches Cerid services first.
//   ceiling     a rule takes effect only for an algorithm the build's providers implement, so a generation can never
//               enable what a reviewed build does not hold, while removals reach every build.
//   freshness   a generation is current until it expires, usable for its offline grace period after that, and then
//               expired: every algorithm is refused until a newer generation arrives.
//   live swap   a session takes a PolicySnapshot when it starts and keeps it; applying a generation changes only what
//               later snapshots hold.
//
// Times are Unix seconds from the caller (the wall clock in a product, the seeded clock in NET-SIM). Decoding and
// verification allocate nothing; decoded signatures view the decoded bytes.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/register_ids.hpp>
#include <crd/crypto/verifier.hpp>

#include <mutex>

namespace crd::crypto
{
namespace cont = crd::containers;

inline constexpr u8    kPolicyFormat        = 2U;          // the layout this build writes (rules carry a scope)
inline constexpr u8    kPolicyFormatOldest  = 1U;          // the oldest layout it reads (N-1: format 1, no scopes)
inline constexpr u8    kPolicyMagic[8]      = {'C', 'R', 'D', 'P', 'O', 'L', 'C', 'Y'};
inline constexpr usize kKeyIdBytes          = 32U;         // a key's identifier (the SHA-256 of its public key)
inline constexpr u32   kMaxPolicyRules      = 32U;         // algorithm rules in one generation
inline constexpr u32   kMaxPolicyMinimums   = 16U;         // protocol minimum versions in one generation
inline constexpr u32   kMaxPolicySignatures = 8U;          // signatures on one generation
inline constexpr u32   kMaxTrustKeys        = 4U;          // keys of one trust set
inline constexpr usize kMaxSignatureBytes   = 8192U;       // one signature (ML-DSA-87: 4627, SLH-DSA-128s: 7856)
inline constexpr usize kMaxPolicyBytes      = 32768U;      // one encoded generation
inline constexpr u64   kMaxPolicyLifetime   = 34560000U;   // 400 days from issue to expiry
inline constexpr u32   kMaxOfflineGrace     = 7776000U;    // 90 days of use after expiry
inline constexpr u64   kMaxIssueSkew        = 300U;        // seconds a generation may be issued ahead of the caller
inline constexpr u64   kNever               = ~u64{0U};    // a window bound that never comes

struct KeyId
{
    u8 bytes[kKeyIdBytes] = {};

    friend bool operator==(const KeyId&, const KeyId&) = default;
};

// The sessions a rule governs (decision 9). A Cerid service session (TlsConfig::policy_scope) follows its algorithm's
// CeridServices rule when the generation has one and the Everywhere rule otherwise; every other session follows the
// Everywhere rule. A scope this build does not know decodes, stays signed and governs nothing here, so a later build
// can add one. Format 1 rules are Everywhere rules.
enum class PolicyScope : u8
{
    Everywhere    = 0U,
    CeridServices = 1U,
};

// One algorithm's window in one scope; the bounds keep accepted_from <= preferred_from <= preferred_until <=
// refused_from and accepted_from < refused_from. preferred_from == preferred_until: never preferred.
struct AlgorithmRule
{
    AlgorithmId algorithm{};
    u64         accepted_from   = 0U;
    u64         preferred_from  = 0U;
    u64         preferred_until = 0U;
    u64         refused_from    = kNever;
    PolicyScope scope = PolicyScope::Everywhere;
};

// The lowest version of a Cerid protocol (crd-net's ProtocolId) a session may negotiate.
struct MinimumVersion
{
    u16 protocol = 0U;
    u16 major    = 0U;
    u16 minor    = 0U;
};

// What one generation says. Rules and minimums keep their order; each algorithm appears once per scope and each
// protocol once.
struct PolicyContent
{
    u64            generation    = 0U; // 1 or more
    u64            issued        = 0U;
    u64            expires       = 0U; // after issued, at most kMaxPolicyLifetime later
    u32            offline_grace = 0U; // at most kMaxOfflineGrace
    MinimumVersion minimums[kMaxPolicyMinimums];
    u32            minimum_count = 0U;
    AlgorithmRule  rules[kMaxPolicyRules];
    u32            rule_count = 0U;
};

struct KeySignature
{
    KeyId               key;
    AlgorithmId         algorithm{};
    cont::ConstSpan<u8> signature; // 1 to kMaxSignatureBytes bytes
};

// One decoded generation. Every signature covers `signed_bytes`, the body from the magic to the last rule.
struct PolicyMessage
{
    u8                  format = kPolicyFormat;
    PolicyContent       content;
    cont::ConstSpan<u8> signed_bytes;
    KeySignature        signatures[kMaxPolicySignatures];
    u32                 signature_count = 0U;
};

enum class PolicyError : u8
{
    None,
    BadConfig,            // the trust set, verifier or floor breaks PolicyConfig's rules
    BufferTooSmall,       // the output cannot hold the bytes
    TooLarge,             // a generation, a count or a signature above its bound
    Truncated,            // the bytes end inside a field
    TrailingBytes,        // bytes after the last field
    BadMagic,             // not a policy generation
    UnsupportedFormat,    // a layout outside [kPolicyFormatOldest, kPolicyFormat]
    BadGeneration,        // generation number 0
    BadDates,             // expiry not after issue, a lifetime or a grace period above its bound
    BadRule,              // algorithm 0 or a window out of order
    DuplicateRule,        // an algorithm with two rules in one scope
    DuplicateMinimum,     // a protocol with two minimums
    Unsigned,             // no signature
    BadSignatureEntry,    // an empty signature
    DuplicateSignature,   // two signatures by one key
    MissingSignature,     // a trust key did not sign
    UnsupportedSignature, // the verifier does not implement a trust key's algorithm
    BadSignature,         // a trust key's signature does not verify, or names another algorithm than the key
    Rollback,             // a generation below the newest one ever accepted
    Conflict,             // the current generation's number with other content
    AlreadyCurrent,       // the current generation again: nothing changes
    NotYetIssued,         // issued more than kMaxIssueSkew after now
    IssuedBeforeCurrent,  // a newer number issued before the current generation
    Expired,              // past its expiry and its offline grace period
    Absent,               // no generation was given (PolicyStore::start)
};

// "none", "bad-config", "buffer-too-small", "too-large", "truncated", "trailing-bytes", "bad-magic",
// "unsupported-format", "bad-generation", "bad-dates", "bad-rule", "duplicate-rule", "duplicate-minimum", "unsigned",
// "bad-signature-entry", "duplicate-signature", "missing-signature", "unsupported-signature", "bad-signature",
// "rollback", "conflict", "already-current", "not-yet-issued", "issued-before-current", "expired", "absent".
[[nodiscard]] cont::StringView policy_error_name(PolicyError error) noexcept;

[[nodiscard]] PolicyError validate_content(const PolicyContent& content) noexcept;

// The encoded size of `content`'s body (valid content assumed).
[[nodiscard]] usize policy_body_size(const PolicyContent& content) noexcept;

// The body the signers sign: the magic, the format, the content.
[[nodiscard]] PolicyError write_policy_body(const PolicyContent& content, cont::Span<u8> out, usize& written) noexcept;

// Whether `body` is exactly one body as write_policy_body writes it: what a signer checks before it signs.
[[nodiscard]] PolicyError check_policy_body(cont::ConstSpan<u8> body) noexcept;

// A whole generation: `body` (as write_policy_body wrote it) followed by its signatures (1 to kMaxPolicySignatures,
// each by another key).
[[nodiscard]] PolicyError write_policy(cont::ConstSpan<u8> body, cont::ConstSpan<KeySignature> signatures,
                                       cont::Span<u8> out, usize& written) noexcept;

// Decode one generation; exact and bounded. On an error `out` holds no meaning.
[[nodiscard]] PolicyError decode_policy(cont::ConstSpan<u8> bytes, PolicyMessage& out) noexcept;

struct TrustKey
{
    KeyId               id;
    AlgorithmId         algorithm{};
    cont::ConstSpan<u8> public_key;
};

// What an installation trusts. The spans and the verifier outlive every store built from it.
//   trust             2 to kMaxTrustKeys keys with distinct identifiers and non-empty public keys, each a signature
//                     algorithm, at least one post-quantum and one traditional; every one signs every generation.
//   supported         the algorithms this build's providers implement: no generation enables another.
//   floor_generation  the newest generation the installation ever accepted (PolicyStore::floor_generation(),
//                     persisted by the caller and passed back at start), so a restart cannot be rolled back.
struct PolicyConfig
{
    cont::ConstSpan<TrustKey>    trust;
    const SignatureVerifier*     verifier = nullptr;
    cont::ConstSpan<AlgorithmId> supported;
    u64                          floor_generation = 0U;
};

[[nodiscard]] PolicyError validate_config(const PolicyConfig& config) noexcept;

enum class Freshness : u8
{
    None,    // no generation yet
    Current, // before its expiry
    Grace,   // expired, within its offline grace period
    Expired, // past both: nothing is accepted
};

enum class AlgorithmStatus : u8
{
    Refused,
    Accepted,
    Preferred,
};

// One generation as a session holds it: a value, unaffected by later generations. Its rules are the generation's
// rules for the algorithms the build supports in the scopes it knows, in the generation's order.
class PolicySnapshot
{
public:
    [[nodiscard]] u64                  generation() const noexcept { return m_content.generation; }
    [[nodiscard]] const PolicyContent& content() const noexcept { return m_content; }
    [[nodiscard]] Freshness            freshness(u64 now) const noexcept;

    // Refused when the generation is missing or expired, the build lacks the algorithm or `now` is outside the window
    // of the rule `scope` follows; Preferred inside its preferred window; Accepted otherwise.
    [[nodiscard]] AlgorithmStatus status(AlgorithmId algorithm, u64 now,
                                         PolicyScope scope = PolicyScope::Everywhere) const noexcept;
    // A suite's status, the algorithms it runs on together (an HPKE suite, an MLS cipher suite): Refused when one of
    // them is, Preferred when every one is, Accepted otherwise; Refused for none.
    [[nodiscard]] AlgorithmStatus status(cont::ConstSpan<AlgorithmId> algorithms, u64 now,
                                         PolicyScope scope = PolicyScope::Everywhere) const noexcept;

    // Whether a session may use `major.minor` of `protocol`: true when the generation sets no minimum for it.
    [[nodiscard]] bool allows_version(u16 protocol, u16 major, u16 minor) const noexcept;

private:
    friend class PolicyStore;

    PolicyContent m_content;
};

// What PolicyStore::start() did with each generation an installation holds when it starts: apply()'s answer, or
// Absent for one it does not hold.
struct PolicyStart
{
    PolicyError persisted = PolicyError::Absent;
    PolicyError bootstrap = PolicyError::Absent;
};

// The installation's current generation. apply() and snapshot() may run on different threads.
class PolicyStore
{
public:
    explicit PolicyStore(const PolicyConfig& config) noexcept;

    // Decode, check and adopt one generation received at `now`. A generation below the floor is refused before its
    // signatures are verified; every trust key's signature is verified before anything else is judged. On any
    // error the store is unchanged (AlreadyCurrent included).
    [[nodiscard]] PolicyError apply(cont::ConstSpan<u8> generation, u64 now) noexcept;

    // A store's start (decision S23, #policy-start): the persisted generation (the newest one the installation
    // accepted, saved with floor_generation()), then the build's bootstrap generation; an empty span is one it does
    // not hold. A bootstrap newer than the persisted one supersedes it; an older one is a Rollback and the same one
    // AlreadyCurrent, both harmless. So a floor above the bootstrap keeps the persisted generation, and a store is
    // empty after start only when neither is usable (lost, expired past its grace, below the floor): every algorithm
    // is then refused until a newer generation is applied.
    [[nodiscard]] PolicyStart start(cont::ConstSpan<u8> persisted, cont::ConstSpan<u8> bootstrap, u64 now) noexcept;

    // Raises the floor to `floor` (never lowers it): the floor an installation saved beside its generation, or one
    // another process of it saved since (decision S24, #policy-state). The current generation stays current.
    void raise_floor(u64 floor) noexcept;

    [[nodiscard]] PolicySnapshot snapshot() const noexcept;

    // The newest generation number ever accepted, the configured floor included: what the caller persists.
    [[nodiscard]] u64 floor_generation() const noexcept;

private:
    mutable std::mutex m_mutex;
    PolicyConfig       m_config;
    PolicyContent      m_accepted; // the current generation as signed, for the conflict check
    PolicySnapshot     m_current;
    u64                m_floor = 0U;
};
} // namespace crd::crypto
