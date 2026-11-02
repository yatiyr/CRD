#pragma once

// crd-crypto -- MLS messages, RFC 9420 (NET.2; contract: docs/design/network-foundation.md#mls, M3 layer 6): the
// structures a handshake message carries (credentials, capabilities and extensions, leaf nodes, key packages,
// proposals, commits and update paths; sections 5.3, 7.2, 10, 12.1 and 12.4), their framing (FramedContent, the
// signature over FramedContentTBS, PublicMessage and its membership tag; sections 6.1 and 6.2), their protection in a
// PrivateMessage with its sender data, reuse guard and padding (section 6.3), and the transcript hashes and the
// confirmation tag a commit advances (section 8.2).
//
//   decoding    `take_*` reads one structure, its fields views of the input, and checks its syntax: every vector header
//               minimal, every enum value one the protocol defines, every optional's presence octet 0 or 1, every list
//               of fixed-size entries a whole number of them and every list of structures a sequence of them. A
//               credential or a proposal of a type this build does not know is a type and an opaque vector, as OpenMLS
//               reads it. After false the reader's position is unspecified and the caller refuses the whole message.
//   encoding    `*_bytes` sizes a structure (0 when a field is too long for its vector or a value is outside its enum)
//               and `put_*` writes what it sized. A decoded structure writes back to the bytes it was read from.
//   semantics   a leaf node's signature, lifetime and capabilities, a key package's, a commit's proposals and a
//               path's keys are the ratchet tree's and the commit's (layers past this one). The senders and content
//               types a wire format admits (section 6) are checked where a message is protected or verified, never
//               where it is decoded, so every syntactically valid message round-trips.
//   allocation  none on the private-message path: protect and open write into the caller's buffers, the AEAD's
//               associated data is the message's own header, and a PrivateMessage opens in place. A signature, a
//               membership tag and a transcript hash lay their input out once, inline up to 256 bytes and from the
//               default allocator beyond (the handshake path, and an application message's signature).

#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/mls.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/verifier.hpp>

namespace crd::crypto::mls
{
// ---- The structures a message carries ----

// The body of an Extension list (section 7.2): each {u16 extension_type; opaque extension_data<V>}.
[[nodiscard]] bool take_extensions(cont::WireReader& in, cont::ConstSpan<u8>& body) noexcept;

// NOLINTNEXTLINE(performance-enum-size): a 16-bit codepoint of its IANA registry, as the wire carries it
enum class CredentialType : u16
{
    Basic = 1U, // an identity
    X509  = 2U, // a certificate chain: the body of a vector of Certificate{opaque cert_data<V>}
};

// A credential (section 5.3): its type and the bytes of its one vector; a type this build does not know keeps its
// vector opaque.
struct Credential
{
    u16                 type = 0U;
    cont::ConstSpan<u8> content;
};

// A member's capabilities (section 7.2): the bodies of its five lists of 16-bit codepoints.
struct Capabilities
{
    cont::ConstSpan<u8> versions;
    cont::ConstSpan<u8> cipher_suites;
    cont::ConstSpan<u8> extensions;
    cont::ConstSpan<u8> proposals;
    cont::ConstSpan<u8> credentials;
};

enum class LeafNodeSource : u8
{
    KeyPackage = 1U,
    Update     = 2U,
    Commit     = 3U,
};

// A leaf node (section 7.2). `signed_fields` views the encoding from encryption_key through extensions: what
// LeafNodeTBS signs, before the group id and leaf index an update or a commit leaf adds.
struct LeafNode
{
    cont::ConstSpan<u8> encryption_key;
    cont::ConstSpan<u8> signature_key;
    Credential          credential;
    Capabilities        capabilities;
    LeafNodeSource      source     = LeafNodeSource::KeyPackage;
    u64                 not_before = 0U;  // a key package leaf's lifetime
    u64                 not_after  = 0U;
    cont::ConstSpan<u8> parent_hash;      // a commit leaf's
    cont::ConstSpan<u8> extensions;       // the Extension list's body
    cont::ConstSpan<u8> signature;
    cont::ConstSpan<u8> signed_fields;
};

[[nodiscard]] bool  take_leaf_node(cont::WireReader& in, LeafNode& out) noexcept;
[[nodiscard]] usize leaf_node_bytes(const LeafNode& node) noexcept;
void                put_leaf_node(cont::WireWriter& out, const LeafNode& node) noexcept;

// A key package (section 10). `signed_fields` views the encoding from version through extensions: KeyPackageTBS.
struct KeyPackage
{
    u16                 version      = kProtocolVersion;
    u16                 cipher_suite = 0U;
    cont::ConstSpan<u8> init_key;
    LeafNode            leaf_node;
    cont::ConstSpan<u8> extensions;
    cont::ConstSpan<u8> signature;
    cont::ConstSpan<u8> signed_fields;
};

[[nodiscard]] bool  take_key_package(cont::WireReader& in, KeyPackage& out) noexcept;
[[nodiscard]] usize key_package_bytes(const KeyPackage& package) noexcept;
void                put_key_package(cont::WireWriter& out, const KeyPackage& package) noexcept;

// NOLINTNEXTLINE(performance-enum-size): a 16-bit codepoint of its IANA registry, as the wire carries it
enum class ProposalType : u16
{
    Add                    = 1U,
    Update                 = 2U,
    Remove                 = 3U,
    PreSharedKey           = 4U,
    ReInit                 = 5U,
    ExternalInit           = 6U,
    GroupContextExtensions = 7U,
};

struct ReInit
{
    cont::ConstSpan<u8> group_id;
    u16                 version      = kProtocolVersion;
    u16                 cipher_suite = 0U;
    cont::ConstSpan<u8> extensions;
};

// A proposal (section 12.1): its type and the one field of it that type holds. A type this build does not know is
// `custom`, its opaque vector.
struct Proposal
{
    u16                 type = 0U; // a ProposalType or another codepoint
    KeyPackage          add;
    LeafNode            update;
    u32                 remove = 0U; // the removed member's leaf index
    PreSharedKeyId      psk;
    ReInit              reinit;
    cont::ConstSpan<u8> kem_output; // ExternalInit
    cont::ConstSpan<u8> extensions; // GroupContextExtensions: the Extension list's body
    cont::ConstSpan<u8> custom;
};

[[nodiscard]] bool  take_proposal(cont::WireReader& in, Proposal& out) noexcept;
[[nodiscard]] usize proposal_bytes(const Proposal& proposal) noexcept;
void                put_proposal(cont::WireWriter& out, const Proposal& proposal) noexcept;

// One entry of a commit's proposal list: a proposal inline or a reference to one sent before (a ProposalRef).
struct ProposalOrRef
{
    bool                is_reference = false;
    Proposal            proposal;
    cont::ConstSpan<u8> reference;
};

[[nodiscard]] bool  take_proposal_or_ref(cont::WireReader& in, ProposalOrRef& out) noexcept;
[[nodiscard]] usize proposal_or_ref_bytes(const ProposalOrRef& entry) noexcept;
void                put_proposal_or_ref(cont::WireWriter& out, const ProposalOrRef& entry) noexcept;

// An HPKE ciphertext (section 7.6) and one node of an update path: its new public key and the body of its list of
// HPKECiphertext, one per resolution node of its copath child.
struct HpkeCiphertext
{
    cont::ConstSpan<u8> kem_output;
    cont::ConstSpan<u8> ciphertext;
};
[[nodiscard]] bool take_hpke_ciphertext(cont::WireReader& in, HpkeCiphertext& out) noexcept;

struct UpdatePathNode
{
    cont::ConstSpan<u8> encryption_key;
    cont::ConstSpan<u8> encrypted_path_secrets;
};
[[nodiscard]] bool take_update_path_node(cont::WireReader& in, UpdatePathNode& out) noexcept;

// An update path (section 7.6): the committer's new leaf and the body of its UpdatePathNode list.
struct UpdatePath
{
    LeafNode            leaf_node;
    cont::ConstSpan<u8> nodes;
};

[[nodiscard]] bool  take_update_path(cont::WireReader& in, UpdatePath& out) noexcept;
[[nodiscard]] usize update_path_bytes(const UpdatePath& path) noexcept;
void                put_update_path(cont::WireWriter& out, const UpdatePath& path) noexcept;

// A commit (section 12.4): the body of its ProposalOrRef list (read with take_proposal_or_ref) and its optional path.
struct Commit
{
    cont::ConstSpan<u8> proposals;
    bool                has_path = false;
    UpdatePath          path;
};

[[nodiscard]] bool  take_commit(cont::WireReader& in, Commit& out) noexcept;
[[nodiscard]] usize commit_bytes(const Commit& commit) noexcept;
void                put_commit(cont::WireWriter& out, const Commit& commit) noexcept;

// ---- Framing (section 6) ----

// NOLINTNEXTLINE(performance-enum-size): a 16-bit codepoint of its IANA registry, as the wire carries it
enum class WireFormat : u16
{
    PublicMessage  = 1U,
    PrivateMessage = 2U,
    Welcome        = 3U,
    GroupInfo      = 4U,
    KeyPackage     = 5U,
};

enum class ContentType : u8
{
    Application = 1U,
    Proposal    = 2U,
    Commit      = 3U,
};

enum class SenderType : u8
{
    Member            = 1U, // `index` is its leaf index
    External          = 2U, // `index` is its place in the group's external_senders extension
    NewMemberProposal = 3U,
    NewMemberCommit   = 4U,
};

struct Sender
{
    SenderType type  = SenderType::Member;
    u32        index = 0U; // 0 for a new member
};

// What a message says (FramedContent, section 6). `body` is an application message's data, or the encoding of its
// Proposal or Commit (read with take_proposal or take_commit).
struct FramedContent
{
    cont::ConstSpan<u8> group_id;
    u64                 epoch = 0U;
    Sender              sender;
    cont::ConstSpan<u8> authenticated_data;
    ContentType         content_type = ContentType::Application;
    cont::ConstSpan<u8> body;
};

// FramedContentAuthData: the sender's signature and, for a commit only, its confirmation tag.
struct ContentAuth
{
    cont::ConstSpan<u8> signature;
    cont::ConstSpan<u8> confirmation_tag;
};

[[nodiscard]] bool  take_framed_content(cont::WireReader& in, FramedContent& out) noexcept;
[[nodiscard]] usize framed_content_bytes(const FramedContent& content) noexcept;
void                put_framed_content(cont::WireWriter& out, const FramedContent& content) noexcept;

// AuthenticatedContent (section 6.1): what the transcript hash and a ProposalRef cover.
[[nodiscard]] bool  take_authenticated_content(cont::WireReader& in, WireFormat& wire_format, FramedContent& content,
                                               ContentAuth& auth) noexcept;
[[nodiscard]] usize authenticated_content_bytes(const FramedContent& content, const ContentAuth& auth) noexcept;
void put_authenticated_content(cont::WireWriter& out, WireFormat wire_format, const FramedContent& content,
                               const ContentAuth& auth) noexcept;

// Whether section 6 lets `sender` send `content_type` in `wire_format`: a PrivateMessage only from a member; a
// PublicMessage never with application data, from an external sender or a new member's proposal only a proposal and
// from a new member's commit only a commit.
[[nodiscard]] bool framing_admits(WireFormat wire_format, const Sender& sender, ContentType content_type) noexcept;

// signature = SignWithLabel(key, "FramedContentTBS", FramedContentTBS{mls10, wire_format, content, and the epoch's
// GroupContext when the sender is a member or a new member committing}) (section 6.1). Malformed when
// framing_admits refuses; `signature` and `written` as sign_with_label.
[[nodiscard]] CryptoError sign_content(const CipherSuite& suite, const Signer& signer, WireFormat wire_format,
                                       const FramedContent& content, const GroupContext& context,
                                       cont::Span<u8> signature, usize& written) noexcept;
[[nodiscard]] Verification verify_content(const CipherSuite& suite, const SignatureVerifier& verifier,
                                          cont::ConstSpan<u8> public_key, WireFormat wire_format,
                                          const FramedContent& content, const GroupContext& context,
                                          cont::ConstSpan<u8> signature) noexcept;

// ---- Messages (MLSMessage, section 6) ----

// A PublicMessage: its content and authentication, and a member's membership tag (section 6.2).
struct PublicMessage
{
    FramedContent       content;
    ContentAuth         auth;
    cont::ConstSpan<u8> membership_tag;
};

// A PrivateMessage (section 6.3). `header` views its encoding from group_id through authenticated_data, which is its
// PrivateContentAAD, and its first `sender_data_aad_bytes` bytes are its SenderDataAAD.
struct PrivateMessage
{
    cont::ConstSpan<u8> group_id;
    u64                 epoch        = 0U;
    ContentType         content_type = ContentType::Application;
    cont::ConstSpan<u8> authenticated_data;
    cont::ConstSpan<u8> encrypted_sender_data;
    cont::ConstSpan<u8> ciphertext;
    cont::ConstSpan<u8> header;
    usize               sender_data_aad_bytes = 0U;
};

// An MLSMessage of protocol version mls10: a public or a private message or a key package decoded, a Welcome's or a
// GroupInfo's encoding in `body` (their layers decode it).
struct Message
{
    WireFormat          wire_format = WireFormat::PublicMessage;
    PublicMessage       public_message;
    PrivateMessage      private_message;
    KeyPackage          key_package;
    cont::ConstSpan<u8> body;
};

// Decodes the whole of `bytes` as one MLSMessage: false for another version, an unknown wire format, a malformed
// structure, an empty Welcome or GroupInfo, or a byte left over.
[[nodiscard]] bool take_message(cont::ConstSpan<u8> bytes, Message& out) noexcept;
// The bytes of a message's encoding (0 for a field too long), and its writer. A Welcome or a GroupInfo writes `body`.
[[nodiscard]] usize message_bytes(const Message& message) noexcept;
void                put_message(cont::WireWriter& out, const Message& message) noexcept;

// membership_tag = MAC(membership_key, AuthenticatedContentTBM{FramedContentTBS, auth}) of a member's PublicMessage
// (section 6.2); `out` Nh long.
[[nodiscard]] CryptoError membership_tag(const CipherSuite& suite, cont::ConstSpan<u8> membership_key,
                                         const FramedContent& content, const ContentAuth& auth,
                                         const GroupContext& context, cont::Span<u8> out) noexcept;

// Writes a signed content as an MLSMessage holding a PublicMessage, with a member's membership tag under
// `membership_key` (Nh bytes; unused for other senders). Malformed when framing_admits refuses, BadLength when `out`
// is shorter than the message (written is its length).
[[nodiscard]] CryptoError protect_public(const CipherSuite& suite, cont::ConstSpan<u8> membership_key,
                                         const GroupContext& context, const FramedContent& content,
                                         const ContentAuth& auth, cont::Span<u8> out, usize& written) noexcept;
// Checks a received PublicMessage before its signature is verified: Malformed when framing_admits refuses it or a
// commit lacks its tag, Forged when a member's membership tag is not the one computed under `membership_key`.
[[nodiscard]] CryptoError verify_public(const CipherSuite& suite, cont::ConstSpan<u8> membership_key,
                                        const GroupContext& context, const PublicMessage& message) noexcept;

// The sender data of a PrivateMessage: leaf index, generation and reuse guard, sealed with its 16-byte tag.
inline constexpr usize kReuseGuardBytes       = 4U;
inline constexpr usize kSenderDataBytes       = 4U + 4U + kReuseGuardBytes;
inline constexpr usize kSealedSenderDataBytes = kSenderDataBytes + kAeadTagBytes;

// The padding Cerid adds to a PrivateMessageContent of `length` bytes (decision M6): Padme, which keeps the top
// floor(log2(floor(log2(length)))) + 1 bits of the length and rounds the bits below them up, so a padded length
// leaks O(log log length) bits for at most 12 % more bytes.
[[nodiscard]] usize padme_padding(usize length) noexcept;

// The bytes of the MLSMessage protect_private writes for `content` and `auth` with `padding` zero bytes; 0 when a
// field is too long.
[[nodiscard]] usize private_message_bytes(const FramedContent& content, const ContentAuth& auth,
                                          usize padding) noexcept;

// Protects a signed content from a member as a PrivateMessage (section 6.3): the next generation of the sender's
// handshake ratchet (a proposal or a commit) or application ratchet (`tree.send_key`, spent), a fresh random reuse
// guard XORed into the nonce's first four bytes, the content, its authentication and `padding` zero bytes sealed
// under the message's header, then the sender data sealed under the keys `sender_data_secret` derives from the
// ciphertext's sample. Writes the MLSMessage into `out` (exactly private_message_bytes long; BadLength otherwise) and
// allocates nothing. Malformed when the sender is not a member or a commit lacks its tag; the tree's refusals pass
// through; every refusal zeroes `out`.
[[nodiscard]] CryptoError protect_private(const CipherSuite& suite, SecretTree& tree,
                                          cont::ConstSpan<u8> sender_data_secret, const FramedContent& content,
                                          const ContentAuth& auth, usize padding, cont::Span<u8> out) noexcept;

// Opens a PrivateMessage take_message decoded: the sender data under the keys of `sender_data_secret`, then the
// content under the key `tree` keeps for the sender's leaf and generation (receive_key), into `plaintext`
// (ciphertext.size() - 16 bytes; it may be the ciphertext itself, in place). Only then is the generation spent
// (consume), so a forged message spends nothing; `content` and `auth` view `plaintext`. Forged when either seal does
// not authenticate; Malformed when the sender data's length is wrong, the plaintext does not decode, a commit's tag
// is not Nh bytes long or the padding holds a byte other than zero; the tree's refusals (Replayed, OutOfWindow) pass
// through. The caller then verifies the signature with the sender leaf's key (verify_content) and that the leaf is
// not blank. Every refusal zeroes `plaintext`.
[[nodiscard]] CryptoError open_private(const CipherSuite& suite, SecretTree& tree,
                                       cont::ConstSpan<u8> sender_data_secret, const PrivateMessage& message,
                                       cont::Span<u8> plaintext, FramedContent& content, ContentAuth& auth) noexcept;

// ---- Transcript hashes (section 8.2) ----

// confirmed_transcript_hash = Hash(interim_transcript_hash || ConfirmedTranscriptHashInput{wire_format, content,
// signature}) of a commit; `out` Nh long.
[[nodiscard]] CryptoError confirmed_transcript_hash(const CipherSuite& suite, cont::ConstSpan<u8> interim,
                                                    WireFormat wire_format, const FramedContent& content,
                                                    cont::ConstSpan<u8> signature, cont::Span<u8> out) noexcept;
// interim_transcript_hash = Hash(confirmed_transcript_hash || InterimTranscriptHashInput{confirmation_tag}).
[[nodiscard]] CryptoError interim_transcript_hash(const CipherSuite& suite, cont::ConstSpan<u8> confirmed,
                                                  cont::ConstSpan<u8> confirmation_tag, cont::Span<u8> out) noexcept;
// confirmation_tag = MAC(confirmation_key, confirmed_transcript_hash), and its check in constant time.
[[nodiscard]] CryptoError confirmation_tag(const CipherSuite& suite, cont::ConstSpan<u8> confirmation_key,
                                           cont::ConstSpan<u8> confirmed, cont::Span<u8> out) noexcept;
[[nodiscard]] bool        confirmation_tag_valid(const CipherSuite& suite, cont::ConstSpan<u8> confirmation_key,
                                                 cont::ConstSpan<u8> confirmed, cont::ConstSpan<u8> tag) noexcept;
} // namespace crd::crypto::mls
