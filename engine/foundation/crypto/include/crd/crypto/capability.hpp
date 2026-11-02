#pragma once

// crd-crypto -- capabilities (NET.2; contract: docs/design/network-foundation.md#capability-tokens).
//
// Nothing a connection carries is executed for its own sake: a message runs only when a capability covers its object,
// operation and budget (no ambient authority, D5). A capability is a token: what an issuing service granted a
// principal, signed by that service's keys, narrowed by each holder that delegated it and presented in a session by
// the last holder. Rules:
//
//   identities  a principal (user, device, service, server, agent) is named by its long-term key: the SHA-256 of the
//               public key (KeyId), the identifier TLS pins. The subject is the principal the grant is about; the
//               holder is the key that may present it (a user's device key, say).
//   issuers     every key of the audience's issuer set whose issuing window holds the token's issue time signs it, at
//               least one post-quantum and one traditional key (a hybrid signature, D7). Rotation is a new pair whose
//               window starts where the old one ends: tokens the old pair signed verify until they expire.
//   attenuation a holder delegates by appending a block, signed with its own key over every byte before the block's
//               end, that names the next holder and an equal or narrower grant, binding and expiry. Nothing widens.
//   binding     a token is bound to its holder's key, and each presentation to its session: the holder signs the
//               session's exporter under its session type's label (Proof), or the session's own mutual authentication
//               is the proof (Peer: the TLS peer key is the holder's). A stolen token is useless without the key, and a
//               proof is useless in any other session. Service links accept Peer only (mutual authentication).
//   revocation  a signed, monotonic revocation generation lists revoked tokens and keys, a cut-off for everything
//               issued before a time and the oldest policy generation still honoured. A service without a current
//               generation admits nothing (fail closed). Every admission and every renewal verifies again.
//   policy      every signature algorithm on the token must be accepted by the session's policy snapshot.
//
// Times are Unix seconds from the caller. Decoding and verification allocate nothing; decoded views point into the
// decoded bytes.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/policy.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <mutex>

namespace crd::crypto
{
namespace cont = crd::containers;

inline constexpr u8    kCapabilityFormat        = 1U; // the layout this build writes
inline constexpr u8    kCapabilityFormatOldest  = 1U; // the oldest layout it reads (N-1 once layout 2 exists)
inline constexpr u8    kCapabilityMagic[8]      = {'C', 'R', 'D', 'C', 'A', 'P', 'T', 'K'};
inline constexpr u8    kCapabilityProofMagic[8] = {'C', 'R', 'D', 'C', 'A', 'P', 'P', 'R'};
inline constexpr u8    kRevocationMagic[8]      = {'C', 'R', 'D', 'R', 'E', 'V', 'O', 'K'};
inline constexpr usize kCapabilityIdBytes       = 16U;    // token, scope and audience identifiers
inline constexpr usize kCapabilityBindingBytes  = 32U;    // a session's exporter value for its type's label
inline constexpr u32   kMaxCapabilityRanges     = 16U;    // object ranges in one grant
inline constexpr u32   kMaxAttenuations         = 4U;     // blocks after the root
inline constexpr u32   kMaxCapabilitySignatures = 4U;     // issuer signatures on a root or a revocation generation
inline constexpr u32   kMaxIssuerKeys           = 8U;     // keys of one issuer set, past and future windows included
inline constexpr usize kMaxHolderKeyBytes       = 2592U;  // a holder's public key (ML-DSA-87)
inline constexpr usize kMaxCapabilityBytes      = 49152U; // one token
inline constexpr usize kMaxCapabilityProofBytes = 4U + kMaxHolderKeyBytes + kMaxSignatureBytes;
inline constexpr u64   kMaxCapabilityLifetime   = 86400U;  // a token's issue to expiry: hours for collaboration
inline constexpr u32   kMaxRevokedTokens        = 4096U;   // token identifiers in one revocation generation
inline constexpr u32   kMaxRevokedKeys          = 512U;    // keys in one revocation generation
inline constexpr u64   kMaxRevocationLifetime   = 604800U; // a revocation generation's issue to expiry: seven days
inline constexpr usize kMaxRevocationBytes      = 163840U;

struct CapabilityId
{
    u8 bytes[kCapabilityIdBytes] = {};

    friend bool operator==(const CapabilityId&, const CapabilityId&) = default;
};

enum class PrincipalKind : u8
{
    User = 1,
    Device,
    Service,
    Server,
    Agent,
};

struct Principal
{
    PrincipalKind kind{};
    KeyId         key; // the SHA-256 of the principal's long-term public key

    friend bool operator==(const Principal&, const Principal&) = default;
};

// What a grant is about, and so the session type it is presented in (D3): each has its own exporter label.
enum class ScopeKind : u8
{
    World = 1, // game sessions
    Project,   // project sessions
    Room,      // media sessions: a call, a webinar, a live performance
    Upload,    // crash upload sessions
    Tool,      // agent sessions
    Service,   // service links
};

// The exporter label a session of `kind`'s type binds presentations with ("EXPORTER-Cerid world capability", ...);
// empty for anything else.
[[nodiscard]] cont::StringView capability_binding_label(ScopeKind kind) noexcept;

// The objects first to last, both included.
struct ObjectRange
{
    u64 first = 0U;
    u64 last  = 0U;
};

// What a holder may spend (admission enforces it, NET.3): each narrower block keeps every bound at or below its
// parent's.
struct CapabilityBudget
{
    u32 operations_per_second = 0U;
    u64 bytes_per_second      = 0U;
    u32 objects               = 0U; // objects the holder may create or hold
};

// The authority itself. Ranges ascend and neither overlap nor touch (last + 1 < next first); none at all means every
// object of the scope. Operation bits are the audience's schema's (NET.3).
struct Grant
{
    ScopeKind        kind{};
    CapabilityId     scope;
    ObjectRange      ranges[kMaxCapabilityRanges];
    u32              range_count = 0U;
    u64              operations  = 0U;
    CapabilityBudget budget;
};

// Whether `narrower` grants nothing `wider` does not: the same scope, every object within `wider`'s ranges, a subset of
// its operations and every budget at or below its own.
[[nodiscard]] bool grant_within(const Grant& narrower, const Grant& wider) noexcept;

enum class CapabilityBinding : u8
{
    Proof = 1, // the holder signs the session's exporter
    Peer,      // the session's mutual authentication proves the holder: no Peer token ever becomes a Proof one
};

struct CapabilityHolder
{
    AlgorithmId       algorithm{};
    KeyId             key;
    CapabilityBinding binding = CapabilityBinding::Proof;
};

// What the issuing service signs.
struct CapabilityRoot
{
    CapabilityId     token;             // random, unique: what a revocation names
    u64              issued  = 0U;
    u64              expires = 0U;      // after issued, at most kMaxCapabilityLifetime later
    u64              policy_generation = 0U; // the policy generation the issuer held
    CapabilityId     audience;          // the service that admits it
    Principal        subject;
    CapabilityHolder holder;
    Grant            grant;
};

// What a holder appends to delegate.
struct CapabilityAttenuation
{
    u64              expires = 0U;
    CapabilityHolder holder;
    Grant            grant;
};

struct CapabilityBlock
{
    CapabilityAttenuation content;
    cont::ConstSpan<u8>   signed_bytes;  // the token from its first byte to this block's grant
    cont::ConstSpan<u8>   signer_public; // the previous holder's public key
    cont::ConstSpan<u8>   signature;     // its signature of signed_bytes
};

// One decoded token.
struct CapabilityToken
{
    u8                  format = kCapabilityFormat;
    CapabilityRoot      root;
    cont::ConstSpan<u8> root_bytes; // what the issuers signed: the magic to the root's grant
    KeySignature        signatures[kMaxCapabilitySignatures];
    u32                 signature_count = 0U;
    CapabilityBlock     blocks[kMaxAttenuations];
    u32                 block_count = 0U;

    // The last block's holder, or the root's.
    [[nodiscard]] const CapabilityHolder& holder() const noexcept;
};

enum class CapabilityError : u8
{
    None,
    BadConfig,            // the issuer set, the verifier, the kind or the lifetime breaks CapabilityConfig's rules; a
                          // signer that is not a signature key; a proof asked for a Peer token
    BufferTooSmall,       // the output cannot hold the bytes
    TooLarge,             // a token, a generation, a count or a field above its bound
    Truncated,            // the bytes end inside a field
    TrailingBytes,        // bytes after a revocation generation's last field
    BadMagic,             // not a token, or not a revocation generation
    UnsupportedFormat,    // a layout outside the readable range
    BadField,             // a kind, binding or algorithm outside its set, a range out of order, a zero identifier
                          // or generation, a signer key of no bytes
    BadDates,             // expiry not after issue, or a lifetime above its bound
    BadList,              // a revocation list out of ascending order, or with an entry twice
    Unsigned,             // no signature
    BadSignatureEntry,    // an empty signature
    DuplicateSignature,   // two signatures by one key
    WrongHolder,          // the signer is not the holder: its key does not hash to the holder's
    Widened,              // a block grants more, lives longer or loosens a Peer binding
    WrongAudience,        // another service's token, or another session type's
    TooLong,              // a lifetime above the audience's bound
    NotYetValid,          // issued more than kMaxIssueSkew after now
    Expired,              // at or past its expiry; a revocation generation past its own
    PeerRequired,         // a Proof token at a service that admits Peer only
    RevocationStale,      // no revocation generation, or the current one expired: nothing is admitted
    Revoked,              // the token, a key on it, its issue time or its policy generation is revoked
    UntrustedIssuer,      // no issuer keys, or no hybrid set, cover its issue time
    AlgorithmRefused,     // the policy snapshot refuses a signature algorithm on it
    MissingSignature,     // an issuer key that must sign did not
    UnsupportedSignature, // the verifier does not implement a signature's algorithm
    BadSignature,         // a signature does not verify, or names another algorithm than its key
    Unbound,              // the presentation does not prove the holder in this session
    Rollback,             // a revocation generation below the newest one ever accepted
    Conflict,             // the current revocation generation's number with other content
    AlreadyCurrent,       // the current revocation generation again: nothing changes
    NotYetIssued,         // a revocation generation issued more than kMaxIssueSkew after now
    IssuedBeforeCurrent,  // a newer revocation generation issued before the current one
    ProviderFailure,      // a signer failed
};

// "none", "bad-config", "buffer-too-small", "too-large", "truncated", "trailing-bytes", "bad-magic",
// "unsupported-format", "bad-field", "bad-dates", "bad-list", "unsigned", "bad-signature-entry",
// "duplicate-signature", "wrong-holder", "widened", "wrong-audience", "too-long", "not-yet-valid", "expired",
// "peer-required", "revocation-stale", "revoked", "untrusted-issuer", "algorithm-refused", "missing-signature",
// "unsupported-signature", "bad-signature", "unbound", "rollback", "conflict", "already-current", "not-yet-issued",
// "issued-before-current", "provider-failure".
[[nodiscard]] cont::StringView capability_error_name(CapabilityError error) noexcept;

[[nodiscard]] CapabilityError validate_root(const CapabilityRoot& root) noexcept;

// A token the `issuers` sign (1 to kMaxCapabilitySignatures signers, each another key): the root, then each issuer's
// signature of it. The issuers must be the audience's keys whose windows hold root.issued.
[[nodiscard]] CapabilityError issue_capability(const CapabilityRoot& root, cont::ConstSpan<const Signer*> issuers,
                                               cont::Span<u8> out, usize& written) noexcept;

// Decode one token; exact and bounded, signatures unchecked. On an error `out` holds no meaning.
[[nodiscard]] CapabilityError decode_capability(cont::ConstSpan<u8> bytes, CapabilityToken& out) noexcept;

// `token` with one more block: `block` must be within the current holder's grant, expiry and binding (Widened) and
// `holder` must be the current holder's key (WrongHolder). `out` must not overlap `token`.
[[nodiscard]] CapabilityError attenuate_capability(cont::ConstSpan<u8> token, const CapabilityAttenuation& block,
                                                   const Signer& holder, cont::Span<u8> out, usize& written) noexcept;

// A session's binding for `kind`: kCapabilityBindingBytes of its exporter under capability_binding_label(kind), equal
// at both ends. As export_keying_material: NotReady outside Established; BadConfig for another kind or length.
[[nodiscard]] TlsError capability_binding(const detail::TlsHandshakeState& session, ScopeKind kind,
                                          cont::Span<u8> out) noexcept;

// The holder's proof that it presents `token` in the session of `binding` (Proof tokens only): its public key and its
// signature of kCapabilityProofMagic, the binding and the SHA-256 of the token. At most kMaxCapabilityProofBytes.
[[nodiscard]] CapabilityError prove_capability(cont::ConstSpan<u8> token, cont::ConstSpan<u8> binding,
                                               const Signer& holder, cont::Span<u8> out, usize& written) noexcept;

// What the audience saw of the session a token arrived in.
struct CapabilityPresentation
{
    cont::ConstSpan<u8> binding;          // capability_binding() for the audience's kind (Proof)
    cont::ConstSpan<u8> proof;            // prove_capability()'s bytes (Proof)
    KeyId               peer_key;         // the session's authenticated peer key (Peer)
    AlgorithmId         peer_algorithm{}; // and its algorithm
};

// What a verified token grants its holder in this session.
struct Capability
{
    CapabilityId     token;
    CapabilityId     audience;
    Principal        subject;
    CapabilityHolder holder;
    Grant            grant; // the last block's
    u64              issued            = 0U;
    u64              expires           = 0U; // the last block's
    u64              policy_generation = 0U;
    u32              depth             = 0U; // blocks after the root

    // Whether the grant holds every bit of `operations` (at least one) on `object`.
    [[nodiscard]] bool covers(u64 operations, u64 object) const noexcept;
};

struct IssuerKey
{
    TrustKey key;
    u64      issues_from  = 0U;     // the first issue time it signs
    u64      issues_until = kNever; // and the first it no longer signs
};

// What an audience trusts. The spans and the verifier outlive the authority built from it.
//   issuers           1 to kMaxIssuerKeys signature keys with distinct identifiers, non-empty public keys and
//                     non-empty windows.
//   kind, audience    the session type and the service this authority admits tokens for; the audience is not zero.
//   max_lifetime      the longest issue-to-expiry it admits: 0 for kMaxCapabilityLifetime, else at most that.
//   peer_only         a service link: only Peer tokens, presented over mutually authenticated sessions.
//   revocation_floor  the newest revocation generation ever accepted (persisted by the caller), so a restart cannot
//                     be rolled back.
struct CapabilityConfig
{
    cont::ConstSpan<IssuerKey> issuers;
    const SignatureVerifier*   verifier = nullptr;
    ScopeKind                  kind{};
    CapabilityId               audience;
    u64                        max_lifetime     = 0U;
    bool                       peer_only        = false;
    u64                        revocation_floor = 0U;
};

[[nodiscard]] CapabilityError validate_config(const CapabilityConfig& config) noexcept;

// What a revocation generation says. Tokens and keys ascend (bytewise) with no entry twice.
struct RevocationList
{
    u64                           generation            = 0U; // 1 or more
    u64                           issued                = 0U;
    u64                           expires               = 0U; // after issued, at most kMaxRevocationLifetime later
    u64                           revoke_before         = 0U; // every token issued before this time is revoked
    u64                           min_policy_generation = 0U; // every token issued under an older one is revoked
    cont::ConstSpan<CapabilityId> tokens;
    cont::ConstSpan<KeyId>        keys; // subjects and holders
};

// One decoded revocation generation: the lists as their bytes (kCapabilityIdBytes and kKeyIdBytes per entry).
struct RevocationMessage
{
    u8                  format = kCapabilityFormat;
    u64                 generation            = 0U;
    u64                 issued                = 0U;
    u64                 expires               = 0U;
    u64                 revoke_before         = 0U;
    u64                 min_policy_generation = 0U;
    cont::ConstSpan<u8> tokens;
    u32                 token_count = 0U;
    cont::ConstSpan<u8> keys;
    u32                 key_count = 0U;
    cont::ConstSpan<u8> signed_bytes;
    KeySignature        signatures[kMaxCapabilitySignatures];
    u32                 signature_count = 0U;
};

// A revocation generation the `issuers` sign, as issue_capability.
[[nodiscard]] CapabilityError issue_revocations(const RevocationList& list, cont::ConstSpan<const Signer*> issuers,
                                                cont::Span<u8> out, usize& written) noexcept;

[[nodiscard]] CapabilityError decode_revocations(cont::ConstSpan<u8> bytes, RevocationMessage& out) noexcept;

// An audience's verifier: its configuration and its current revocation generation. apply_revocations() and verify()
// may run on different threads. About 80 KiB: keep it in static or heap storage.
class CapabilityAuthority
{
public:
    explicit CapabilityAuthority(const CapabilityConfig& config) noexcept;

    // Decode, check and adopt one revocation generation received at `now`, as PolicyStore::apply adopts a policy
    // generation: below the floor is Rollback before any signature is verified; the issuer keys covering its issue
    // time must all sign (each algorithm accepted by `policy`); then Conflict or AlreadyCurrent for the current number,
    // NotYetIssued, Expired and IssuedBeforeCurrent. On any error nothing changes.
    [[nodiscard]] CapabilityError apply_revocations(cont::ConstSpan<u8> generation, const PolicySnapshot& policy,
                                                    u64 now) noexcept;

    // The newest revocation generation number ever accepted, the configured floor included: what the caller persists.
    [[nodiscard]] u64 revocation_floor() const noexcept;

    // Verify `token` as presented in a session at `now`, checking in this order: the configuration (BadConfig), the
    // decoding, the audience and kind (WrongAudience), the audience's lifetime (TooLong), each block within its parent
    // (Widened), the issue time and expiry (NotYetValid, Expired), Peer at a peer-only service (PeerRequired), the
    // revocation generation (RevocationStale, Revoked), the issuer set (UntrustedIssuer), every signature algorithm
    // under `policy` (AlgorithmRefused), the issuers' signatures, each block's signer and signature, and the
    // presentation (Unbound). `out` is written only on None.
    [[nodiscard]] CapabilityError verify(cont::ConstSpan<u8> token, const CapabilityPresentation& presentation,
                                         const PolicySnapshot& policy, u64 now, Capability& out) const noexcept;

    // The same with the presentation read off `session` (Established): its binding for the authority's kind and its
    // authenticated peer key, with `proof` from the holder.
    [[nodiscard]] CapabilityError verify(cont::ConstSpan<u8> token, const detail::TlsHandshakeState& session,
                                         cont::ConstSpan<u8> proof, const PolicySnapshot& policy, u64 now,
                                         Capability& out) const noexcept;

private:
    [[nodiscard]] CapabilityError check_revoked(const CapabilityToken& token, u64 now) const noexcept;

    mutable std::mutex m_mutex;
    CapabilityConfig   m_config;
    u64                m_floor = 0U;
    // The current revocation generation (generation 0: none yet).
    u64          m_generation            = 0U;
    u64          m_issued                = 0U;
    u64          m_expires               = 0U;
    u64          m_revoke_before         = 0U;
    u64          m_min_policy_generation = 0U;
    u32          m_token_count           = 0U;
    u32          m_key_count             = 0U;
    CapabilityId m_tokens[kMaxRevokedTokens];
    KeyId        m_keys[kMaxRevokedKeys];
};
} // namespace crd::crypto
