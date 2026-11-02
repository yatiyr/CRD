#pragma once

// crd-crypto -- the Messaging Layer Security protocol, RFC 9420 (NET.2; contract:
// docs/design/network-foundation.md#mls): end-to-end encrypted groups whose relays and delivery servers carry
// ciphertext they cannot read (threat T14), the group keys of calls (SFrame, NET-LIVE) and of shared projects.
//
// Cerid's own implementation over the provider interface (decision M1): the cipher suites of the profile (M2), the
// presentation language's variable-length vectors and optional values (section 2.1), array-based tree math
// (Appendix C), the labelled operations every later layer derives from (sections 5.1.2, 5.1.3, 5.2, 8 and 9), the
// secret tree with its sender ratchets and the sender data keys (sections 6.3.2 and 9), and the key schedule with its
// pre-shared keys, exporter and epoch authenticator (section 8). The MLS working group's test vectors, which the other
// implementations generate and check, hold each layer to the byte.
//
//   lengths     a hash, a derived secret and a reference are the suite's Nh; an expansion is 1 to 65535 bytes and at
//               most 255 * Nh. A wrong length is BadLength.
//   refusals    a key or a signer of another algorithm than the suite's is Unsupported; HPKE's refusals pass through
//               (hpke.hpp). Every refusal zeroes the outputs.
//   allocation  none for labels and contexts that fit 256 bytes together (every per-message derivation); a longer
//               context or a signed message is laid out once from the default allocator. A secret tree allocates once,
//               when its epoch starts; none of its message operations allocates.
//   secrets     every secret a tree or an epoch holds is zeroed when it is used up (section 9.2's deletion schedule),
//               replaced or destroyed.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/hpke.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/verifier.hpp>

#include <optional>

namespace crd::crypto::mls
{
// ---- Cipher suites (sections 5.1 and 17.1) ----

// One cipher suite of the profile: its HPKE algorithms, its hash (the KDF's, which is also the MAC's: HMAC with it)
// and its signature algorithm.
struct CipherSuite
{
    u16         id = 0U; // the MLS Cipher Suites codepoint
    AlgorithmId kem{};   // the DHKEM's group (hpke.hpp)
    AlgorithmId kdf{};
    AlgorithmId aead{};
    AlgorithmId signature{};

    [[nodiscard]] HpkeSuite hpke() const noexcept { return {kem, kdf, aead}; }
    // Nh: the length of a hash, of KDF.Extract's output and of a MAC.
    [[nodiscard]] usize hash_bytes() const noexcept { return hkdf_hash_bytes(kdf); }
};

// The suites of the profile, in codepoint order: 0x0001, 0x0002, 0x0003 and 0x0007 (decision M2).
[[nodiscard]] cont::ConstSpan<CipherSuite> cipher_suites() noexcept;
// The suite `id` names; null for a suite outside the profile.
[[nodiscard]] const CipherSuite* cipher_suite(u16 id) noexcept;

// ---- The presentation language (section 2.1) ----

// The largest length a variable-size vector header encodes (2^30 - 1).
inline constexpr usize kMaxVarint = (usize{1} << 30U) - 1U;

// The bytes of the minimal header of `value`: 1, 2 or 4; 0 above kMaxVarint.
[[nodiscard]] usize varint_bytes(usize value) noexcept;
// The bytes of `length` bytes behind their header; 0 above kMaxVarint.
[[nodiscard]] usize vector_bytes(usize length) noexcept;

// Writes the minimal header of `value`, which is at most kMaxVarint (a larger one stops the process: the layout that
// sized `out` was wrong).
void put_varint(cont::WireWriter& out, usize value) noexcept;
// A header and its bytes.
void put_vector(cont::WireWriter& out, cont::ConstSpan<u8> bytes) noexcept;

// Takes a header: false for a short input, the prefix 0b11 and an encoding longer than needed, which are malformed.
// After false the reader's position is unspecified and the decoder refuses the whole message.
[[nodiscard]] bool take_varint(cont::WireReader& in, usize& value) noexcept;
// A header and the bytes it counts, as a view of the input.
[[nodiscard]] bool take_vector(cont::WireReader& in, cont::ConstSpan<u8>& out) noexcept;
// The presence octet of an optional value: false for a short input and any octet but 0 and 1 (section 2.1.1).
[[nodiscard]] bool take_presence(cont::WireReader& in, bool& present) noexcept;

// ---- Array-based trees (Appendix C) ----

// A ratchet tree is a perfect binary tree of 2^d leaves (section 4.1). Nodes are numbered from the left, leaves at the
// even indices (leaf i is node 2i) and parents at the odd ones; a tree of n leaves has 2n - 1 nodes. Every function
// answers for a tree of 1 to 2^31 leaves, a power of two, and a node inside it; any other input has no answer
// (nullopt, 0 or an empty path), so a size or an index read off the wire never reaches past the tree.
namespace tree
{
inline constexpr u32 kMaxLeaves = u32{1} << 31U;
inline constexpr u32 kMaxDepth  = 31U; // the longest direct path

[[nodiscard]] bool valid_size(u32 leaves) noexcept;
// The node count of a tree of `leaves` leaves (0 for an invalid size).
[[nodiscard]] u32 node_width(u32 leaves) noexcept;
[[nodiscard]] std::optional<u32> root(u32 leaves) noexcept;
// A node's level: 0 for a leaf, the level of its children plus one for a parent.
[[nodiscard]] u32                level(u32 node) noexcept;
[[nodiscard]] std::optional<u32> left(u32 node, u32 leaves) noexcept;
[[nodiscard]] std::optional<u32> right(u32 node, u32 leaves) noexcept;
[[nodiscard]] std::optional<u32> parent(u32 node, u32 leaves) noexcept;
[[nodiscard]] std::optional<u32> sibling(u32 node, u32 leaves) noexcept;
// The direct path (a node's ancestors, from its parent to the root) and the copath (the sibling of the node and of
// each ancestor below the root), into `out` (kMaxDepth entries hold any); the count written.
[[nodiscard]] usize direct_path(u32 node, u32 leaves, cont::Span<u32> out) noexcept;
[[nodiscard]] usize copath(u32 node, u32 leaves, cont::Span<u32> out) noexcept;
// The lowest node whose subtree holds both `a` and `b` (either, when one is an ancestor of the other).
[[nodiscard]] std::optional<u32> common_ancestor(u32 a, u32 b, u32 leaves) noexcept;
} // namespace tree

// ---- Labelled operations ----

// "MLS 1.0 " before every label of a signature, an encryption and a key derivation (sections 5.1.2, 5.1.3, 8).
inline constexpr cont::StringView kLabelPrefix = "MLS 1.0 ";

// MAC(key, message) = HMAC with the suite's hash (section 5.1), `out` Nh long: the membership and confirmation tags.
[[nodiscard]] CryptoError mac(const CipherSuite& suite, cont::ConstSpan<u8> key, cont::ConstSpan<u8> message,
                              cont::Span<u8> out) noexcept;

// RefHash(label, value) = Hash(RefHashInput{label, value}) (section 5.2); `label` is complete ("MLS 1.0 KeyPackage
// Reference"), `out` Nh long.
[[nodiscard]] CryptoError ref_hash(const CipherSuite& suite, cont::StringView label, cont::ConstSpan<u8> value,
                                   cont::Span<u8> out) noexcept;

// ExpandWithLabel(secret, label, context, out.size()) = KDF.Expand(secret, KDFLabel{length, "MLS 1.0 " + label,
// context}, length) (section 8).
[[nodiscard]] CryptoError expand_with_label(const CipherSuite& suite, cont::ConstSpan<u8> secret,
                                            cont::StringView label, cont::ConstSpan<u8> context,
                                            cont::Span<u8> out) noexcept;

// DeriveSecret(secret, label) = ExpandWithLabel(secret, label, "", Nh).
[[nodiscard]] CryptoError derive_secret(const CipherSuite& suite, cont::ConstSpan<u8> secret, cont::StringView label,
                                        cont::Span<u8> out) noexcept;

// DeriveTreeSecret(secret, label, generation, out.size()) = ExpandWithLabel(secret, label, generation as a big-endian
// uint32, length) (section 9.1).
[[nodiscard]] CryptoError derive_tree_secret(const CipherSuite& suite, cont::ConstSpan<u8> secret,
                                             cont::StringView label, u32 generation, cont::Span<u8> out) noexcept;

// SignWithLabel(key, label, content) = Signature.Sign(key, SignContent{"MLS 1.0 " + label, content}) (section 5.1.2):
// `signer` holds the suite's signature algorithm, `out` and `written` as Signer::sign.
[[nodiscard]] CryptoError sign_with_label(const CipherSuite& suite, const Signer& signer, cont::StringView label,
                                          cont::ConstSpan<u8> content, cont::Span<u8> out, usize& written) noexcept;

// VerifyWithLabel(key, label, content, signature) under the suite's signature algorithm.
[[nodiscard]] Verification verify_with_label(const CipherSuite& suite, const SignatureVerifier& verifier,
                                             cont::ConstSpan<u8> public_key, cont::StringView label,
                                             cont::ConstSpan<u8> content, cont::ConstSpan<u8> signature) noexcept;

// EncryptWithLabel(public_key, label, context, plaintext) = SealBase(public_key, EncryptContext{"MLS 1.0 " + label,
// context}, "", plaintext) (section 5.1.3): `kem_output` and `ciphertext` as hpke_seal's `enc` and `ciphertext`.
[[nodiscard]] CryptoError encrypt_with_label(const CipherSuite& suite, cont::ConstSpan<u8> public_key,
                                             cont::StringView label, cont::ConstSpan<u8> context,
                                             cont::ConstSpan<u8> plaintext, cont::Span<u8> kem_output,
                                             cont::Span<u8> ciphertext) noexcept;

// DecryptWithLabel(private_key, label, context, kem_output, ciphertext) = OpenBase(kem_output, private_key,
// EncryptContext, "", ciphertext): `key` holds the suite's KEM, `plaintext` as HpkeKey::open's `out`.
[[nodiscard]] CryptoError decrypt_with_label(const CipherSuite& suite, const HpkeKey& key, cont::StringView label,
                                             cont::ConstSpan<u8> context, cont::ConstSpan<u8> kem_output,
                                             cont::ConstSpan<u8> ciphertext, cont::Span<u8> plaintext) noexcept;

// The longest hash and AEAD key of the profile's suites (SHA-384, AES-256): every secret a tree or an epoch holds fits.
inline constexpr usize kMaxHashBytes    = 48U;
inline constexpr usize kMaxAeadKeyBytes = 32U;

// ---- The key schedule (section 8) ----

// mls10, the protocol version every GroupContext and message names.
inline constexpr u16 kProtocolVersion = 1U;

// An epoch's GroupContext (section 8.1): the protocol version, then these fields.
struct GroupContext
{
    u16                 cipher_suite = 0U;
    cont::ConstSpan<u8> group_id;
    u64                 epoch = 0U;
    cont::ConstSpan<u8> tree_hash;
    cont::ConstSpan<u8> confirmed_transcript_hash;
    cont::ConstSpan<u8> extensions; // the encoded Extension list: the body of the extensions vector
};

// The bytes of a GroupContext's encoding; 0 when a field is too long for its vector.
[[nodiscard]] usize group_context_bytes(const GroupContext& context) noexcept;
// Writes the encoding, whose fields group_context_bytes accepted.
void put_group_context(cont::WireWriter& out, const GroupContext& context) noexcept;

// A pre-shared key's identity (section 8.4): an external key's id, or a resumption key's usage, group and epoch; then
// the nonce that makes each use distinct.
enum class PskType : u8
{
    External   = 1U,
    Resumption = 2U,
};
enum class ResumptionUsage : u8
{
    Application = 1U,
    Reinit      = 2U,
    Branch      = 3U,
};
struct PreSharedKeyId
{
    PskType             type = PskType::External;
    cont::ConstSpan<u8> id;                                     // an external key's
    ResumptionUsage     usage = ResumptionUsage::Application;  // a resumption key's
    cont::ConstSpan<u8> group_id;                               // a resumption key's
    u64                 epoch = 0U;                             // a resumption key's
    cont::ConstSpan<u8> nonce;
};
// The bytes of a PreSharedKeyID's encoding; 0 when a field is too long for its vector or the type or usage is outside
// its enum.
[[nodiscard]] usize psk_id_bytes(const PreSharedKeyId& id) noexcept;
// Writes an identity psk_id_bytes accepted.
void put_psk_id(cont::WireWriter& out, const PreSharedKeyId& id) noexcept;
// Takes one, its fields views of the input: false for a short input and a type or usage outside the enums.
[[nodiscard]] bool take_psk_id(cont::WireReader& in, PreSharedKeyId& out) noexcept;

struct PreSharedKey
{
    PreSharedKeyId      id;
    cont::ConstSpan<u8> secret;
};

// psk_secret (section 8.4) over `psks` in order, Nh bytes: each key extracted, expanded with its PSKLabel ("derived
// psk"; its identity, index and the count) and chained by KDF.Extract; Nh zero bytes for none. More than 65535 keys,
// an identity field too long for its vector or a type or usage outside the enums is BadLength.
[[nodiscard]] CryptoError psk_secret(const CipherSuite& suite, cont::ConstSpan<PreSharedKey> psks,
                                     cont::Span<u8> out) noexcept;

// The secrets of one epoch that outlive its derivation (section 8), named by EpochSecret.
enum class EpochSecret : u8
{
    Joiner,         // what a Welcome carries to new members
    Welcome,        // seals the GroupInfo of a Welcome
    SenderData,     // sender_data_secret: the sender data keys
    Encryption,     // the secret tree's root
    Exporter,       // MLS-Exporter
    External,       // the external key pair (HpkeKey::derive)
    Confirmation,   // confirmation_key: a commit's confirmation tag
    Membership,     // membership_key: a public message's membership tag
    Resumption,     // resumption_psk
    Authentication, // epoch_authenticator
    Init,           // init_secret: the next epoch's
};
inline constexpr usize kEpochSecretCount = 11U;

// Each the suite's Nh long; zeroed when destroyed or derived again. The epoch secret and the member secret between the
// joiner secret and it are zeroed inside the derivation.
class EpochSecrets
{
public:
    EpochSecrets() noexcept = default;
    ~EpochSecrets() noexcept;
    EpochSecrets(const EpochSecrets&)            = delete;
    EpochSecrets& operator=(const EpochSecrets&) = delete;
    EpochSecrets(EpochSecrets&&)                 = delete;
    EpochSecrets& operator=(EpochSecrets&&)      = delete;

    // Empty before a derivation succeeded.
    [[nodiscard]] cont::ConstSpan<u8> get(EpochSecret which) const noexcept;

    // A member's next epoch: joiner_secret = ExpandWithLabel(KDF.Extract(init_secret, commit_secret), "joiner",
    // group_context, Nh), then as `join`. `init_secret` is the previous epoch's Init (or a new group's random one);
    // `commit_secret` is the commit's path secret, `psk_secret` the PSKs', Nh bytes each and Nh zero bytes when the
    // commit has no path or no PSK; `group_context` is the new epoch's encoded GroupContext.
    [[nodiscard]] CryptoError advance(const CipherSuite& suite, cont::ConstSpan<u8> init_secret,
                                      cont::ConstSpan<u8> commit_secret, cont::ConstSpan<u8> psk_secret,
                                      cont::ConstSpan<u8> group_context) noexcept;
    // A joiner's epoch from the joiner secret of its Welcome: member = KDF.Extract(joiner_secret, psk_secret), welcome
    // = DeriveSecret(member, "welcome"), epoch = ExpandWithLabel(member, "epoch", group_context, Nh), and each secret
    // of the epoch DeriveSecret(epoch, its label).
    [[nodiscard]] CryptoError join(const CipherSuite& suite, cont::ConstSpan<u8> joiner_secret,
                                   cont::ConstSpan<u8> psk_secret, cont::ConstSpan<u8> group_context) noexcept;

private:
    void wipe() noexcept;

    u8    m_secrets[kEpochSecretCount][kMaxHashBytes] = {};
    usize m_hash_bytes                                = 0U;
};

// MLS-Exporter(label, context, out.size()) = ExpandWithLabel(DeriveSecret(exporter_secret, label), "exported",
// Hash(context), length) (section 8.5).
[[nodiscard]] CryptoError export_secret(const CipherSuite& suite, cont::ConstSpan<u8> exporter_secret,
                                        cont::StringView label, cont::ConstSpan<u8> context,
                                        cont::Span<u8> out) noexcept;

// ---- The secret tree (sections 6.3.2 and 9) ----

// The key and nonce of a message's sender data: ExpandWithLabel(sender_data_secret, "key" and "nonce", the first Nh
// bytes of the ciphertext (all of it when shorter), AEAD.Nk and AEAD.Nn). `key` is Nk and `nonce` Nn long.
[[nodiscard]] CryptoError sender_data_key(const CipherSuite& suite, cont::ConstSpan<u8> sender_data_secret,
                                          cont::ConstSpan<u8> ciphertext, cont::Span<u8> key,
                                          cont::Span<u8> nonce) noexcept;

// The ratchet of a sender's leaf a message is protected under: handshake for proposals and commits, application for
// application data.
enum class RatchetKind : u8
{
    Handshake,
    Application,
};

// The AEAD key and nonce of one generation of one ratchet; zeroed when destroyed.
struct MessageKey
{
    u8    key[kMaxAeadKeyBytes]  = {};
    u8    nonce[kAeadNonceBytes] = {};
    usize key_length             = 0U; // the suite's AEAD Nk
    u32   generation             = 0U;

    MessageKey() noexcept = default;
    ~MessageKey() noexcept;
    MessageKey(const MessageKey&)            = delete;
    MessageKey& operator=(const MessageKey&) = delete;
    MessageKey(MessageKey&&)                 = delete;
    MessageKey& operator=(MessageKey&&)      = delete;

    [[nodiscard]] cont::ConstSpan<u8> aead_key() const noexcept { return cont::ConstSpan<u8>(key, key_length); }
};

// What a receiver keeps of every ratchet (decision M5, OpenMLS's defaults): the keys of up to `out_of_order`
// generations below the next unconsumed one, for late and reordered messages, and at most `max_forward` generations
// derived ahead of it, so a hostile generation costs a bounded chain before its message is refused.
struct RatchetWindow
{
    u32 out_of_order = 5U;
    u32 max_forward  = 1000U;
};
inline constexpr u32 kMaxOutOfOrder = 1024U;
inline constexpr u32 kMaxForward    = u32{1} << 16U;
// The largest secret tree: a group of 2^20 members, about 0.7 GiB with the default window (decision M5).
inline constexpr u32 kMaxSecretTreeLeaves = u32{1} << 20U;

// One epoch's secret tree (section 9): the encryption secret at the root, each parent's two children derived together
// ("tree", "left" and "right") the first time a leaf below them is needed, and each leaf's handshake and application
// ratchets. A secret is zeroed once what it derives is held (section 9.2): a parent's after its children, a leaf's
// after its two ratchets, a ratchet's after the next generation's, a key once consumed or once it leaves the window.
//
//   receiving   `receive_key` reads the key of a generation without changing the tree, so a message is opened before
//               anything is spent; `consume` spends it once the message authenticated. A spent generation is
//               Replayed; one more than `out_of_order` below the next unconsumed generation, one whose key was not
//               kept when the ratchet moved past it, or one more than `max_forward` above it is OutOfWindow.
//   sending     `send_key` hands out the next generation of the sender's own ratchet and spends it; a ratchet past
//               generation 2^32 - 1 is Exhausted (the group must start a new epoch).
//   memory      one allocation when the epoch starts: per leaf two node secrets, two ratchets and 2 * `out_of_order`
//               window slots (Node, Ratchet and Slot below: about 0.7 KiB with the default window). A leaf outside
//               the tree is OutOfWindow.
class SecretTree
{
public:
    SecretTree() noexcept = default;
    ~SecretTree() noexcept;
    SecretTree(const SecretTree&)            = delete;
    SecretTree& operator=(const SecretTree&) = delete;
    SecretTree(SecretTree&& other) noexcept;
    SecretTree& operator=(SecretTree&& other) noexcept;

    // A tree of `leaves` leaves (a power of two, at most kMaxSecretTreeLeaves) under `encryption_secret` (Nh bytes).
    // Another size or length, or a window past kMaxOutOfOrder or kMaxForward, is BadLength; ProviderFailure when the
    // memory cannot be had. Replaces, zeroed, what the tree held; a refusal leaves it empty.
    [[nodiscard]] CryptoError init(const CipherSuite& suite, cont::ConstSpan<u8> encryption_secret, u32 leaves,
                                   RatchetWindow window = {}) noexcept;

    [[nodiscard]] bool ready() const noexcept { return m_leaves != 0U; }
    [[nodiscard]] u32  leaves() const noexcept { return m_leaves; }

    // The key of `generation` of `leaf`'s ratchet, the tree unchanged. NotReady before init; the output is zeroed on
    // every refusal.
    [[nodiscard]] CryptoError receive_key(u32 leaf, RatchetKind kind, u32 generation, MessageKey& out) const noexcept;
    // Spends `generation` once its message authenticated: the ratchet moves past it and keeps the keys of the
    // generations it skipped that the window holds. The refusals are receive_key's.
    [[nodiscard]] CryptoError consume(u32 leaf, RatchetKind kind, u32 generation) noexcept;
    // The next generation of the sender's own ratchet, spent.
    [[nodiscard]] CryptoError send_key(u32 leaf, RatchetKind kind, MessageKey& out) noexcept;

private:
    enum class SlotState : u8
    {
        Empty,
        Retained, // a skipped generation's key, kept for a late message
        Spent,    // consumed
    };
    struct Node
    {
        u8   secret[kMaxHashBytes] = {};
        bool present               = false;
    };
    struct Ratchet
    {
        u64  next                  = 0U;    // the next unconsumed generation; 2^32 once the last is spent
        u8   secret[kMaxHashBytes] = {};    // the ratchet secret of generation `next`
        bool live                  = false; // derived from its leaf's secret
    };
    struct Slot // generation g of a ratchet lives in its slot g % out_of_order
    {
        u32       generation             = 0U;
        SlotState state                  = SlotState::Empty;
        u8        key[kMaxAeadKeyBytes]  = {};
        u8        nonce[kAeadNonceBytes] = {};
    };

    // The ratchet of `leaf` and `kind` and the generation it would answer, or a refusal.
    [[nodiscard]] CryptoError locate(u32 leaf, RatchetKind kind, u32 generation, usize& ratchet) const noexcept;
    // The slot holding `generation` of the ratchet, or m_slots.size().
    [[nodiscard]] usize slot_index(usize ratchet, u32 generation) const noexcept;
    // The ratchet secret of generation 0 of a leaf whose ratchets are not live, from the lowest node on its path that
    // holds a secret.
    [[nodiscard]] CryptoError first_secret(u32 leaf, RatchetKind kind, cont::Span<u8> out) const noexcept;
    // Derives the leaf's two ratchets, storing both children of each parent on the way down and zeroing the parent's
    // secret and the leaf's.
    [[nodiscard]] CryptoError open_leaf(u32 leaf) noexcept;
    void                      wipe() noexcept;

    CipherSuite          m_suite{};
    u32                  m_leaves = 0U;
    RatchetWindow        m_window{};
    cont::Array<Node>    m_nodes;
    cont::Array<Ratchet> m_ratchets; // leaf i's handshake ratchet at 2i, its application ratchet at 2i + 1
    cont::Array<Slot>    m_slots;    // out_of_order per ratchet
};
} // namespace crd::crypto::mls
