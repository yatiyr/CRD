#pragma once

// crd-net -- version and extension negotiation (NET-STD; contract: docs/design/network-foundation.md#negotiation).
//
// Every Cerid wire protocol opens with one offer (client to server) and one answer (server to client). The offer
// names the protocol, every version major the client speaks with its highest minor, and the extensions it requests as
// (identifier, length, value) entries. The server selects the highest major both sides speak (minor: the lower of the
// two), keeps the offered extensions it knows and answers with the selection, its own available versions and the
// values of the extensions it accepts. Rules:
//
//   ignore-unknown   an extension or a version major a side does not know is skipped, never fatal, in both
//                    directions; only a known extension the client did not offer is refused in an answer.
//   GREASE           every offer and every answer, in every build, carries one GREASE version major and two GREASE
//                    extensions (kGreaseIds, RFC 8701 practice) placed and filled from caller entropy, so a peer or
//                    middlebox that refuses unknown values fails in testing. A GREASE value is never selected.
//   downgrade        the client checks the selection against the answer's available versions (the RFC 9368
//                    pattern): a higher common major means an attacker stripped the offer, and the answer is refused.
//                    The answer itself is authenticated by binding: write_transcript gives the exact bytes the
//                    handshake (NET.2) folds into its transcript, so any change to either message fails the
//                    handshake.
//   bounds           a message holds at most kMaxVersions versions, kMaxExtensions extensions and kMaxMessageBytes
//                    bytes; an extension value at most kMaxExtensionBytes. Decoding is exact: a short field, a count
//                    above its bound, a duplicate major or identifier, or a byte after the last field is refused.
//
// Nothing here allocates. Decoded extension values are views into the decoded bytes, which must outlive them.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/net/register_ids.hpp>

namespace crd::net
{
namespace cont = crd::containers;

inline constexpr u8    kNegotiationFormat       = 1U;    // the message layout this build writes
inline constexpr u8    kNegotiationFormatOldest = 1U;    // the oldest layout it reads (N-1 once layout 2 exists)
inline constexpr u32   kMaxVersions             = 8U;    // version entries in one message, GREASE included
inline constexpr u32   kMaxExtensions           = 32U;   // extension entries in one message, GREASE included
inline constexpr u32   kMaxExtensionBytes       = 1024U; // one extension value
inline constexpr usize kMaxMessageBytes         = 4096U; // one encoded message
inline constexpr u32   kGreaseVersions          = 1U;    // GREASE version majors in every message
inline constexpr u32   kGreaseExtensions        = 2U;    // GREASE extensions in every message
inline constexpr u32   kGreaseMaxValueBytes     = 7U;    // a GREASE extension value is 0 to 7 bytes
inline constexpr usize kGreaseEntropyBytes      = 24U;   // what one message's GREASE draws on
inline constexpr usize kTranscriptOverhead      = 16U;   // write_transcript's label and two length fields

enum class MessageKind : u8
{
    Offer  = 1U,
    Answer = 2U,
};

// One version major and its highest minor (minors add compatible fields; a major change is incompatible).
struct ProtocolVersion
{
    u16 major = 0U;
    u16 minor = 0U;
};

struct ExtensionEntry
{
    u16                 id = 0U;
    cont::ConstSpan<u8> value;
};

// What one side speaks: its protocol, its version majors (each at most once, at least 1, never a GREASE value; at
// most kMaxVersions - kGreaseVersions) and the extension identifiers it understands (each at most once, never a
// GREASE value; at most kMaxExtensions - kGreaseExtensions).
struct NegotiationPolicy
{
    ProtocolId                       protocol = ProtocolId::SessionControl;
    cont::ConstSpan<ProtocolVersion> versions;
    cont::ConstSpan<u16>             known_extensions;
};

// Unpredictable bytes for one message's GREASE: which values, where they sit and what the extensions hold. The
// transport fills them from the crd-crypto CSPRNG (NET.2); NET-SIM replays them from its seeded stream.
struct GreaseEntropy
{
    u8 bytes[kGreaseEntropyBytes] = {};
};

enum class NegotiationError : u8
{
    None,
    BadPolicy,            // the local policy breaks NegotiationPolicy's rules
    BufferTooSmall,       // the output cannot hold the message
    TooLarge,             // a message, a count or a value above its bound
    Truncated,            // the bytes end inside a field
    TrailingBytes,        // bytes after the last field
    WrongKind,            // an answer where an offer was expected, or the reverse
    UnsupportedFormat,    // a layout outside [kNegotiationFormatOldest, kNegotiationFormat]
    NoVersions,           // a version list without an entry
    DuplicateVersion,     // a version major listed twice
    DuplicateExtension,   // an extension identifier listed twice
    WrongProtocol,        // the peer speaks another protocol
    NoCommonVersion,      // no version major both sides speak
    GreaseSelected,       // the answer selected a GREASE major
    UnofferedVersion,     // the answer selected a major the offer did not hold, or a minor above the offer's
    InconsistentAnswer,   // the selection disagrees with the answer's own available versions
    Downgrade,            // the answer's available versions share a higher major with the offer than the selection
    UnsolicitedExtension, // an extension the client knows but did not offer, in an answer or an accepted list
};

// "none", "bad-policy", "buffer-too-small", "too-large", "truncated", "trailing-bytes", "wrong-kind",
// "unsupported-format", "no-versions", "duplicate-version", "duplicate-extension", "wrong-protocol",
// "no-common-version", "grease-selected", "unoffered-version", "inconsistent-answer", "downgrade",
// "unsolicited-extension".
[[nodiscard]] cont::StringView negotiation_error_name(NegotiationError error) noexcept;

// One decoded message. `versions` is the offer's version list or the answer's available versions; `selected` is set
// for an answer only. Extension values view the decoded bytes.
struct NegotiationMessage
{
    MessageKind     kind     = MessageKind::Offer;
    u8              format   = kNegotiationFormat;
    u16             protocol = 0U;
    ProtocolVersion selected;
    ProtocolVersion versions[kMaxVersions];
    u32             version_count = 0U;
    ExtensionEntry  extensions[kMaxExtensions];
    u32             extension_count = 0U;
};

// The server's choice for one offer: the version and the offered extensions it knows (GREASE and unknown dropped),
// in offer order, whose values view the offer's bytes.
struct NegotiationSelection
{
    ProtocolVersion version;
    ExtensionEntry  offered[kMaxExtensions];
    u32             offered_count = 0U;
};

// What the client accepts from an answer: the version and the accepted extensions with the server's values.
struct NegotiationResult
{
    ProtocolVersion version;
    ExtensionEntry  extensions[kMaxExtensions];
    u32             extension_count = 0U;
};

[[nodiscard]] NegotiationError validate_policy(const NegotiationPolicy& policy) noexcept;

// Decode one message of kind `expected`. On an error `out` holds no meaning.
[[nodiscard]] NegotiationError decode_negotiation(cont::ConstSpan<u8> bytes, MessageKind expected,
                                                  NegotiationMessage& out) noexcept;

// Client: write the offer of `policy` requesting `requested` (each identifier known to the policy, at most once).
[[nodiscard]] NegotiationError write_offer(const NegotiationPolicy& policy, cont::ConstSpan<ExtensionEntry> requested,
                                           const GreaseEntropy& grease, cont::Span<u8> out, usize& written) noexcept;

// Server: choose the version and the known extensions of a decoded `offer`.
[[nodiscard]] NegotiationError select_negotiation(const NegotiationPolicy& policy, const NegotiationMessage& offer,
                                                  NegotiationSelection& out) noexcept;

// Server: write the answer to `selection`, accepting `accepted` (each identifier offered in `selection`, at most once,
// with the server's value).
[[nodiscard]] NegotiationError write_answer(const NegotiationPolicy& policy, const NegotiationSelection& selection,
                                            cont::ConstSpan<ExtensionEntry> accepted, const GreaseEntropy& grease,
                                            cont::Span<u8> out, usize& written) noexcept;

// Client: check a decoded `answer` against the offer it sent (decoded from its own bytes) and read the result.
[[nodiscard]] NegotiationError verify_answer(const NegotiationPolicy& policy, const NegotiationMessage& offer,
                                             const NegotiationMessage& answer, NegotiationResult& out) noexcept;

// The bytes a handshake binds: the 8-byte label "crdneg01", the offer's length (u32, big-endian), the offer, the
// answer's length and the answer. Each side writes it from the bytes it sent and received; NET.2 folds it into the
// handshake transcript, so a message changed in flight makes the two sides' transcripts differ.
[[nodiscard]] constexpr usize transcript_size(usize offer_bytes, usize answer_bytes) noexcept
{
    return kTranscriptOverhead + offer_bytes + answer_bytes;
}

[[nodiscard]] NegotiationError write_transcript(cont::ConstSpan<u8> offer, cont::ConstSpan<u8> answer,
                                                cont::Span<u8> out, usize& written) noexcept;
} // namespace crd::net
