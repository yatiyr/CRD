#pragma once

// crd-crypto -- an MLS group, RFC 9420 (NET.2; contract: docs/design/network-foundation.md#mls, M3 layers 8 to 10b and
// decisions M8 to M12): the group's extensions (sections 7.2, 11.1, 12.1.8.1, 12.4.3.2 and 13.4), the validation of
// leaf nodes and key packages against a group (5.3.1, 7.3, 10.1 and 13.3), the GroupInfo and Welcome a committer seals
// for new members (12.4.3 and 12.4.3.1), and a member's group state: a new group (section 11), one joined from a
// Welcome or by an external commit (12.4.3.2), one that reinitializes or branches a prior group (11.2 and 11.3), and
// its epochs' messages: proposals, application data and commits received and sent, and a commit staged and merged
// (5.2 and 12.1 to 12.4.2; mls_commit.cpp).
//
//   time        every lifetime check takes the caller's clock (LeafPolicy::now); nothing here reads one.
//   identity    whether a credential binds acceptable identifiers to its key is the application's Authentication
//               Service (section 5.3.1); which pre-shared keys and groups a member holds is its GroupStore.
//   atomicity   a group that refuses a create or a join is left empty; a refused message changes nothing of the
//               group but a PrivateMessage's spent generation, and a commit changes it only when merged.
//   secrets     the joiner, path, PSK and epoch secrets, the decrypted GroupSecrets and the private keys are zeroed
//               when replaced or destroyed.
//   allocation  the handshake path: a join allocates the tree, the decrypted Welcome and the epoch's secret tree; a
//               commit the next epoch's; a received message owns a copy of its bytes, opened in place.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/mls.hpp>
#include <crd/crypto/mls_message.hpp>
#include <crd/crypto/mls_tree.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/verifier.hpp>

namespace crd::crypto::mls
{
// ---- Extensions (sections 7.2, 11.1, 12.1.8.1 and 13.4) ----

// NOLINTNEXTLINE(performance-enum-size): a 16-bit codepoint of its IANA registry, as the wire carries it
enum class ExtensionType : u16
{
    ApplicationId        = 1U,
    RatchetTree          = 2U,
    RequiredCapabilities = 3U,
    ExternalPub          = 4U,
    ExternalSenders      = 5U,
};

// Whether every client implements an extension type (RFC 9420's five) or a proposal type (its seven): the "default"
// types section 7.2 says capabilities never list, which a list that does name them still means.
[[nodiscard]] bool default_extension(u16 type) noexcept;
[[nodiscard]] bool default_proposal(u16 type) noexcept;

// Whether the body of a list of 16-bit codepoints (a capabilities list) holds `value`.
[[nodiscard]] bool lists(cont::ConstSpan<u8> codepoints, u16 value) noexcept;

// Whether a leaf's capabilities support an extension or a proposal type (a default one or one they list) or a
// credential type (one they list). Capabilities are compared without their GREASE values (section 13.5): a GREASE
// value is never supported, so no group can require or use one.
[[nodiscard]] bool supports_extension(const Capabilities& capabilities, u16 type) noexcept;
[[nodiscard]] bool supports_proposal(const Capabilities& capabilities, u16 type) noexcept;
[[nodiscard]] bool supports_credential(const Capabilities& capabilities, u16 type) noexcept;

// Over the body of an Extension list whose syntax take_extensions checked: whether it names each type at most once
// (section 13.4), and the data of the extension of `type` (false when the list has none).
[[nodiscard]] bool extensions_distinct(cont::ConstSpan<u8> body) noexcept;
[[nodiscard]] bool find_extension(cont::ConstSpan<u8> body, u16 type, cont::ConstSpan<u8>& data) noexcept;

// The required_capabilities extension (section 11.1): the bodies of its three lists of 16-bit codepoints.
struct RequiredCapabilities
{
    cont::ConstSpan<u8> extensions;
    cont::ConstSpan<u8> proposals;
    cont::ConstSpan<u8> credentials;
};

// The whole of an extension's data; false for anything else. The writer's sizing is 0 for a list too long.
[[nodiscard]] bool  take_required_capabilities(cont::ConstSpan<u8> data, RequiredCapabilities& out) noexcept;
[[nodiscard]] usize required_capabilities_bytes(const RequiredCapabilities& required) noexcept;
void                put_required_capabilities(cont::WireWriter& out, const RequiredCapabilities& required) noexcept;

// One entry of the external_senders extension (section 12.1.8.1): a key that may sign external proposals and the
// credential that names it.
struct ExternalSender
{
    cont::ConstSpan<u8> signature_key;
    Credential          credential;
};

[[nodiscard]] bool  take_external_sender(cont::WireReader& in, ExternalSender& out) noexcept;
[[nodiscard]] usize external_sender_bytes(const ExternalSender& sender) noexcept;
void                put_external_sender(cont::WireWriter& out, const ExternalSender& sender) noexcept;
// The whole of an extension's data as the list's body (read with take_external_sender); false for anything else.
[[nodiscard]] bool take_external_senders(cont::ConstSpan<u8> data, cont::ConstSpan<u8>& body) noexcept;

// The external_pub extension of a GroupInfo (section 12.4.3.2): the whole of its data as the HPKE public key an
// external joiner encapsulates to (false for anything else), and that data's bytes and writer.
[[nodiscard]] bool  take_external_pub(cont::ConstSpan<u8> data, cont::ConstSpan<u8>& key) noexcept;
[[nodiscard]] usize external_pub_bytes(cont::ConstSpan<u8> key) noexcept;
void                put_external_pub(cont::WireWriter& out, cont::ConstSpan<u8> key) noexcept;

// ---- What the application supplies ----

// The Authentication Service (section 5.3.1): the application's judgement of the credentials members present.
class AuthenticationService
{
public:
    AuthenticationService()                                        = default;
    AuthenticationService(const AuthenticationService&)            = delete;
    AuthenticationService& operator=(const AuthenticationService&) = delete;
    AuthenticationService(AuthenticationService&&)                 = delete;
    AuthenticationService& operator=(AuthenticationService&&)      = delete;
    virtual ~AuthenticationService()                               = default;

    // Whether `credential` legitimately presents identifiers bound to `signature_key` that the application accepts
    // for a member of the group (an X.509 chain whose end-entity key is `signature_key` and that the application
    // trusts; a basic identity the application has bound to that key). Asked for every credential that enters a
    // group: each member's when joining, each added member's, each external sender's. Safe from several threads.
    [[nodiscard]] virtual bool validate(const Credential&   credential,
                                        cont::ConstSpan<u8> signature_key) const noexcept = 0;
    // Whether `successor` presents identifiers the application accepts as the successor of `predecessor`'s for one
    // member (section 5.3.1): asked whenever a member's leaf is replaced, by an Update, by a commit's path, and by an
    // external commit whose joiner removes its own former leaf (12.4.3.2), so a stranger cannot take a member's place.
    // Safe from several threads.
    [[nodiscard]] virtual bool valid_successor(const Credential& predecessor,
                                               const Credential& successor) const noexcept = 0;
};

// What a member keeps across its groups (sections 8.4 and 12.4.3.1).
class GroupStore
{
public:
    GroupStore()                             = default;
    GroupStore(const GroupStore&)            = delete;
    GroupStore& operator=(const GroupStore&) = delete;
    GroupStore(GroupStore&&)                 = delete;
    GroupStore& operator=(GroupStore&&)      = delete;
    virtual ~GroupStore()                    = default;

    // The secret of a pre-shared key the member holds (an external key by its id, a resumption key by its group and
    // epoch); empty when it holds none. The view stays valid until the call that asked for it returns.
    [[nodiscard]] virtual cont::ConstSpan<u8> psk(const PreSharedKeyId& id) const noexcept = 0;
    // Whether the member already takes part in a group of this id.
    [[nodiscard]] virtual bool member_of(cont::ConstSpan<u8> group_id) const noexcept = 0;
};

// ---- Leaf nodes and key packages against a group (sections 7.3, 10.1, 11.1 and 13.3; decision M8) ----

// Where a leaf node appears: the source it must have (any, in a tree a member joins) and whose clock judges its
// lifetime (a sender MUST check it, a receiver SHOULD).
enum class LeafRole : u8
{
    KeyPackage,
    Update,
    Commit,
    Tree,
};

// The longest total lifetime a key-package leaf may claim (decision M8): OpenMLS's default key package lifetime of
// twelve weeks and its one-hour margin, so the two accept each other's key packages.
inline constexpr u64 kDefaultMaxLifetime = u64{12U} * 7U * 24U * 3600U + 3600U;

struct LeafPolicy
{
    u64  now            = 0U;                  // seconds since the Unix epoch, the caller's clock
    u64  max_lifetime   = kDefaultMaxLifetime; // the longest not_after - not_before accepted (section 7.2's MUST)
    bool tree_lifetimes = true; // whether key-package leaves of a tree a member joins are checked (section 7.3's
                                // RECOMMENDED; an application whose members update less often turns it off)
};

// Section 7.3's checks of one leaf node's own content against the group of `context`, in this order:
//   Malformed    a source other than `role` names, or a leaf or GroupContext extension list naming a type twice;
//   BadKey       an encryption key the suite's KEM cannot encapsulate to (hpke_check_public_key);
//   Forged       a credential the Authentication Service refuses for the leaf's signature key;
//   Unsupported  capabilities that do not support the leaf's own credential type (a GREASE type never is), a
//                non-default leaf extension they do not list (a GREASE one they list is accepted, section 13.5), or a
//                GroupContext extension or required capability (section 11.1) they do not support; an
//                undecodable required_capabilities is Malformed (whether they list the group's version and cipher
//                suite is a creator's choice of parameters, section 11, not a check of the leaf: decision M8);
//   Expired      a key-package leaf outside its lifetime at `policy.now` or claiming more than `max_lifetime`
//                (for role Tree only when `policy.tree_lifetimes`).
// The leaf's signature, the uniqueness of its keys and the credential types of the other members are the caller's
// (verify_leaf_node, RatchetTree::validate and check_credential_types).
[[nodiscard]] CryptoError check_leaf(const GroupContext& context, const LeafNode& leaf, LeafRole role,
                                     const LeafPolicy& policy, const AuthenticationService& authentication) noexcept;

// Section 13.3 over a tree: every member's capabilities list the credential type of every member (each type in use is
// supported by all, and each member supports every type in use). Unsupported otherwise; ProviderFailure without memory.
[[nodiscard]] CryptoError check_credential_types(const RatchetTree& tree) noexcept;

// KeyPackageTBS's signature (section 10): SignWithLabel(key, "KeyPackageTBS", the fields before the signature), laid
// out from the fields; `signature` and `written` as sign_with_label.
[[nodiscard]] CryptoError sign_key_package(const CipherSuite& suite, const Signer& signer, const KeyPackage& package,
                                           cont::Span<u8> signature, usize& written) noexcept;

// Section 10.1's checks of a key package for the group of `context`: Unsupported for another version or cipher
// suite, check_leaf's refusals for its leaf (role KeyPackage), Forged when the leaf's or the package's signature does
// not verify under the leaf's signature key, BadKey when the init key is the leaf's encryption key or no agreement
// can use it.
[[nodiscard]] CryptoError check_key_package(const GroupContext& context, const KeyPackage& package,
                                            const SignatureVerifier& verifier,
                                            const AuthenticationService& authentication,
                                            const LeafPolicy& policy) noexcept;

// KeyPackageRef = RefHash("MLS 1.0 KeyPackage Reference", the key package's encoding) under `suite` (section 5.2);
// `out` Nh long, BadLength for a package that does not encode.
[[nodiscard]] CryptoError key_package_ref(const CipherSuite& suite, const KeyPackage& package,
                                          cont::Span<u8> out) noexcept;

// ---- GroupInfo and Welcome (section 12.4.3) ----

// A GroupInfo. `signed_fields` views its encoding from group_context through signer: GroupInfoTBS.
struct GroupInfo
{
    GroupContext        context;
    cont::ConstSpan<u8> extensions; // the Extension list's body
    cont::ConstSpan<u8> confirmation_tag;
    u32                 signer = 0U; // the signing member's leaf index
    cont::ConstSpan<u8> signature;
    cont::ConstSpan<u8> signed_fields;
};

// The decoders of this section read one structure, its fields views of the input, with mls_message.hpp's syntax rules;
// the sizings are 0 for a field too long for its vector, and a decoded structure writes back to its bytes.
[[nodiscard]] bool  take_group_info(cont::WireReader& in, GroupInfo& out) noexcept;
[[nodiscard]] usize group_info_bytes(const GroupInfo& info) noexcept;
void                put_group_info(cont::WireWriter& out, const GroupInfo& info) noexcept;

// signature = SignWithLabel(key, "GroupInfoTBS", GroupInfoTBS), laid out from the fields; `signature` and `written`
// as sign_with_label. The verification is under the signer leaf's key, which the caller looks up.
[[nodiscard]] CryptoError  sign_group_info(const CipherSuite& suite, const Signer& signer, const GroupInfo& info,
                                           cont::Span<u8> signature, usize& written) noexcept;
[[nodiscard]] Verification verify_group_info(const CipherSuite& suite, const SignatureVerifier& verifier,
                                             cont::ConstSpan<u8> public_key, const GroupInfo& info) noexcept;

// What a Welcome carries to one new member: the joiner secret, the path secret of the lowest node it shares with
// the committer when the commit had a path, and the body of the PreSharedKeyID list of the commit's PSKs.
struct GroupSecrets
{
    cont::ConstSpan<u8> joiner_secret;
    bool                has_path_secret = false;
    cont::ConstSpan<u8> path_secret;
    cont::ConstSpan<u8> psks;
};

[[nodiscard]] bool  take_group_secrets(cont::WireReader& in, GroupSecrets& out) noexcept;
[[nodiscard]] usize group_secrets_bytes(const GroupSecrets& secrets) noexcept;
void                put_group_secrets(cont::WireWriter& out, const GroupSecrets& secrets) noexcept;

// One entry of a Welcome: the KeyPackageRef of the member it is for and its GroupSecrets, HPKE-sealed to the
// package's init key.
struct EncryptedGroupSecrets
{
    cont::ConstSpan<u8> new_member;
    HpkeCiphertext      encrypted_group_secrets;
};

[[nodiscard]] bool take_encrypted_group_secrets(cont::WireReader& in, EncryptedGroupSecrets& out) noexcept;

// A Welcome (an MLSMessage's body of wire format welcome): its cipher suite, the body of its EncryptedGroupSecrets
// list (read with take_encrypted_group_secrets) and the sealed GroupInfo.
struct Welcome
{
    u16                 cipher_suite = 0U;
    cont::ConstSpan<u8> secrets;
    cont::ConstSpan<u8> encrypted_group_info;
};

[[nodiscard]] bool  take_welcome(cont::WireReader& in, Welcome& out) noexcept;
[[nodiscard]] usize welcome_bytes(const Welcome& welcome) noexcept;
void                put_welcome(cont::WireWriter& out, const Welcome& welcome) noexcept;

// A new member a Welcome is for: its key package and the path secret of the lowest common ancestor of its leaf and the
// committer's (empty when the commit had no path).
struct WelcomeMember
{
    KeyPackage          package;
    cont::ConstSpan<u8> path_secret;
};

// The committer's side of section 12.4.3.1: `group_info` (an encoded, signed GroupInfo of the new epoch) sealed under
// the welcome key and nonce of `joiner_secret` and the PSKs, and to each member its GroupSecrets (the joiner secret,
// its path secret and the PSKs' identities) sealed with EncryptWithLabel(init_key, "Welcome", the sealed GroupInfo).
// Writes the Welcome's encoding (an MLSMessage's body) into `out`. Unsupported for a member of another cipher suite,
// BadLength for a secret not Nh long or a list too long; the refusals of the operations pass through; on a refusal
// `out` is empty. Every Welcome of one commit seals the same GroupInfo (the key and nonce are the joiner secret's).
[[nodiscard]] CryptoError seal_welcome(const CipherSuite& suite, cont::ConstSpan<u8> joiner_secret,
                                       cont::ConstSpan<PreSharedKey> psks, cont::ConstSpan<u8> group_info,
                                       cont::ConstSpan<WelcomeMember> members, cont::Array<u8>& out) noexcept;

class Group;

// A Welcome opened for one member: its GroupSecrets and GroupInfo, views of the decrypted bytes it owns, the PSK
// secret of the GroupSecrets' keys and the usage of the one that resumes a prior group (Application when none does).
// Zeroed when replaced or destroyed.
class OpenedWelcome
{
public:
    OpenedWelcome() noexcept = default;
    ~OpenedWelcome() noexcept;
    OpenedWelcome(const OpenedWelcome&)            = delete;
    OpenedWelcome& operator=(const OpenedWelcome&) = delete;
    OpenedWelcome(OpenedWelcome&&)                 = delete;
    OpenedWelcome& operator=(OpenedWelcome&&)      = delete;

    [[nodiscard]] bool                ready() const noexcept { return m_suite != nullptr; }
    [[nodiscard]] const CipherSuite&  suite() const noexcept { return *m_suite; }
    [[nodiscard]] const GroupSecrets& secrets() const noexcept { return m_secrets; }
    [[nodiscard]] const GroupInfo&    group_info() const noexcept { return m_info; }
    [[nodiscard]] cont::ConstSpan<u8> psk_secret() const noexcept
    {
        return cont::ConstSpan<u8>(m_psk_secret, ready() ? m_suite->hash_bytes() : 0U);
    }
    [[nodiscard]] ResumptionUsage resumption() const noexcept { return m_resumption; }

private:
    friend CryptoError open_welcome(cont::ConstSpan<u8>, const KeyPackage&, cont::ConstSpan<u8>, const GroupStore&,
                                    const Group*, OpenedWelcome&) noexcept;
    void wipe() noexcept;

    const CipherSuite* m_suite = nullptr;
    cont::Array<u8>    m_secrets_bytes; // the decrypted GroupSecrets
    cont::Array<u8>    m_info_bytes;    // the decrypted GroupInfo
    GroupSecrets       m_secrets;
    GroupInfo          m_info;
    u8                 m_psk_secret[kMaxHashBytes] = {};
    ResumptionUsage    m_resumption                = ResumptionUsage::Application;
};

// The first steps of joining (section 12.4.3.1) for the member whose key package is `own` and whose init key is
// `init_private` (SerializePrivateKey form), in this order:
//   Malformed    a Welcome, GroupSecrets or GroupInfo that does not decode whole, secrets or tags of the wrong
//                length, a PSK identity named twice or with a nonce not Nh long, a second resumption PSK of usage
//                reinit or branch (section 12.4.3.1: the only such PSK), or an extension list naming a type twice;
//   Unsupported  a cipher suite outside the profile or other than the key package's, or a GroupInfo of another
//                suite;
//   BadKey       no entry for the key package's KeyPackageRef, or an init key that is not the package's;
//   NotReady     a PSK the member does not hold: an external or application resumption PSK the store does not hold,
//                or a reinit or branch PSK that does not name `prior` (the group the Welcome resumes, nullptr for
//                none) and its current epoch, whose resumption secret it then is (sections 11.2 and 11.3);
//   Forged       GroupSecrets or a GroupInfo that does not open (HPKE's other refusals pass through).
// The GroupInfo's signature, tree and confirmation tag, and a resumed group's rules, are Group::join's. A refusal
// leaves `out` empty.
[[nodiscard]] CryptoError open_welcome(cont::ConstSpan<u8> welcome, const KeyPackage& own,
                                       cont::ConstSpan<u8> init_private, const GroupStore& store, const Group* prior,
                                       OpenedWelcome& out) noexcept;

// ---- Proposals and commits (sections 5.2 and 12.1 to 12.4.3.2; M3 layers 9 and 10a, decisions M10 and M11) ----

// Whether a commit holding a proposal of `type` must carry a path (section 12.4's pathRequiredTypes, the Path Required
// column of section 17.4): update, remove, external_init and group_context_extensions, and any type this build does
// not know.
[[nodiscard]] bool path_required(u16 type) noexcept;

// Whether section 12.1.8 lets an external sender send a proposal of `type`: add, remove, psk, reinit and
// group_context_extensions.
[[nodiscard]] bool external_proposal(u16 type) noexcept;

// ProposalRef = RefHash("MLS 1.0 Proposal Reference", AuthenticatedContent) of an authenticated proposal (section
// 5.2); `out` Nh long, BadLength for a content that does not encode.
[[nodiscard]] CryptoError proposal_ref(const CipherSuite& suite, WireFormat wire_format, const FramedContent& content,
                                       const ContentAuth& auth, cont::Span<u8> out) noexcept;

// ---- A member's group state (sections 8, 11, 12, 12.4.3.1 and 12.4.3.2; decisions M9 to M11) ----

// What a group keeps beyond its epoch (decision M10): the resumption PSKs of this many past epochs (56 bytes each),
// which a commit may inject (section 8.6), and at most this many proposals cached in one epoch, so a flood of
// proposals costs bounded memory (Exhausted past it).
inline constexpr u32 kDefaultResumptionEpochs = 32U;
inline constexpr u32 kMaxResumptionEpochs     = 1024U;
inline constexpr u32 kDefaultPendingProposals = 256U;
inline constexpr u32 kMaxPendingProposals     = 65536U;

// What a group consults: the signature verifier, the Authentication Service and the member's store (each outliving
// the group), the leaf policy, the secret tree's receive window and the bounds of decision M10.
struct GroupConfig
{
    const SignatureVerifier*     verifier       = nullptr;
    const AuthenticationService* authentication = nullptr;
    const GroupStore*            store          = nullptr;
    LeafPolicy                   policy;
    RatchetWindow                window;
    u32                          resumption_epochs = kDefaultResumptionEpochs;
    u32                          pending_proposals = kDefaultPendingProposals;
};

// One epoch of a member's state: what a Group holds, and what a StagedCommit derives for the next epoch.
struct GroupEpoch
{
    u64             epoch = 0U;
    cont::Array<u8> extensions; // the GroupContext's Extension list body
    cont::Array<u8> confirmed;  // the confirmed transcript hash
    cont::Array<u8> tag;        // the confirmation tag of the commit that began the epoch, which its GroupInfo carries
    cont::Array<u8> interim;    // the interim transcript hash
    RatchetTree     tree;
    EpochSecrets    secrets;
    SecretTree      secret_tree;

    // Zeroes and drops everything; takes `other`'s state (zeroing what this held), leaving `other` empty.
    void clear() noexcept;
    void take(GroupEpoch& other) noexcept;
};

class Group;

// A commit a member created or received, checked in full and its next epoch derived, but not yet the group's
// (section 12.4): Group::merge takes it. Until then its proposals stay readable, so the application can apply its own
// rules (section 12.2 lets it add some) and merge it or drop it. Its secrets are zeroed when dropped.
class StagedCommit
{
public:
    StagedCommit() noexcept = default;
    ~StagedCommit() noexcept;
    StagedCommit(const StagedCommit&)            = delete;
    StagedCommit& operator=(const StagedCommit&) = delete;
    StagedCommit(StagedCommit&&)                 = delete;
    StagedCommit& operator=(StagedCommit&&)      = delete;

    // Drops the commit, zeroing its secrets.
    void clear() noexcept;

    [[nodiscard]] bool ready() const noexcept { return m_group != nullptr; }
    [[nodiscard]] u64  epoch() const noexcept { return m_next.epoch; } // the new epoch
    // A member's leaf, or an external joiner's (type NewMemberCommit) with the leaf the commit gives it.
    [[nodiscard]] Sender committer() const noexcept { return m_committer; }
    // Whether the commit removes the member: merging it leaves the group empty (section 12.4.2).
    [[nodiscard]] bool removes_self() const noexcept { return m_removed; }
    // Whether the commit is a ReInit's (its only proposal, section 12.2): merging it ends the group (Group::ended).
    [[nodiscard]] bool reinitializes() const noexcept { return m_reinit; }
    [[nodiscard]] bool has_path() const noexcept { return m_has_path; }
    // The committed proposals in the commit's order, each with its sender (the committer for one sent by value).
    [[nodiscard]] usize proposal_count() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool  proposal(usize i, Proposal& out, Sender& sender) const noexcept;
    // The leaf index of each member the commit adds, in the order of its Add proposals.
    [[nodiscard]] cont::ConstSpan<u32> added() const noexcept
    {
        return cont::ConstSpan<u32>(m_added.data(), m_added.size());
    }
    // The next epoch's ratchet tree and epoch authenticator (empty for a member the commit removes).
    [[nodiscard]] const RatchetTree&  tree() const noexcept { return m_next.tree; }
    [[nodiscard]] cont::ConstSpan<u8> epoch_authenticator() const noexcept
    {
        return m_next.secrets.get(EpochSecret::Authentication);
    }

private:
    friend class Group;
    struct Entry
    {
        usize  offset = 0U; // the proposal's encoding in m_bytes
        usize  bytes  = 0U;
        Sender sender;
    };

    const Group*        m_group      = nullptr; // the group it was staged for, at m_from_epoch
    u64                 m_from_epoch = 0U;
    Sender              m_committer;
    bool                m_removed  = false;
    bool                m_has_path = false;
    bool                m_reinit   = false;
    cont::Array<u8>     m_bytes; // the committed proposals' encodings
    cont::Array<Entry>  m_entries;
    cont::Array<u32>    m_added;
    GroupEpoch          m_next;
    PathUpdate          m_update; // the path's keys for the member, applied when merged
};

// How a member's own message is framed (section 6): a PrivateMessage by default, or a PublicMessage (never for
// application data), its authenticated data, and the padding of a PrivateMessage: Padme's (decision M6), plus
// `extra_padding` zero bytes (a call's frames brought to one size).
struct SendOptions
{
    WireFormat          wire_format = WireFormat::PrivateMessage;
    cont::ConstSpan<u8> authenticated_data;
    usize               extra_padding = 0U;
};

// What a GroupInfo carries (section 12.4.3): the ratchet tree (section 12.4.3.3), the external_pub extension that
// lets a new member join by an external commit (12.4.3.2; off by default: only a member who publishes the key, which
// no one else can derive, opens the group to external joins), an empty extension of a random GREASE type (section
// 13.5; on by default, and none is added when `extensions` already names a GREASE type), and more extensions (an
// Extension list's body, each type once, neither ratchet_tree nor external_pub).
struct GroupInfoOptions
{
    bool                ratchet_tree = true;
    bool                external_pub = false;
    bool                grease       = true;
    cont::ConstSpan<u8> extensions;
};

// What a member's commit holds (section 12.4.1).
struct CommitOptions
{
    SendOptions send;
    // The member's own proposals, committed by value after the cached ones, in this order: Add, Remove, PreSharedKey
    // and GroupContextExtensions (an Update by value would be the committer's own, which section 12.2 refuses), or a
    // ReInit alone (section 12.2: a ReInit beside any other proposal, cached ones included, is refused).
    cont::ConstSpan<Proposal> proposals;
    // Every proposal cached this epoch that keeps the list valid, by reference, as section 12.2 prefers: a Remove over
    // an Update of the same leaf, the latest Update of a leaf, none of the committer's own Updates or removal, the
    // first of repeated Adds of one client, PreSharedKeys and Removes, the latest GroupContextExtensions, and only
    // proposals valid on their own (section 12.1); the latest valid ReInit only when no other proposal is committed
    // (section 12.2 prefers the others; the ReInit is proposed again in a later epoch).
    bool pending = true;
    // Whether the commit carries a path when no proposal requires one and the list is not empty (section 12.4).
    bool path = true;
    // The new epoch's GroupInfo, which the Welcome seals and CommitResult::group_info carries.
    GroupInfoOptions group_info;
};

// A member's commit: its MLSMessage, the Welcome's MLSMessage for the members it adds (empty when none), the signed
// GroupInfo's MLSMessage of the new epoch, and the commit staged for Group::merge once the delivery service accepted
// it.
struct CommitResult
{
    cont::Array<u8> message;
    cont::Array<u8> welcome;
    cont::Array<u8> group_info;
    StagedCommit    staged;
};

// What an external joiner's commit holds (section 12.4.3.2) besides its ExternalInit: the Remove of the joiner's own
// former leaf (`resync`; the members accept the new leaf only as its successor), pre-shared keys the store holds (each
// identity with its fresh Nh-byte nonce), and the new epoch's GroupInfo.
struct ExternalJoinOptions
{
    cont::ConstSpan<u8>             authenticated_data;
    std::optional<u32>              resync;
    cont::ConstSpan<PreSharedKeyId> psks;
    GroupInfoOptions                group_info;
};

// An external joiner's commit, a PublicMessage MLSMessage for the delivery service, and the signed GroupInfo of the
// epoch it begins.
struct ExternalJoinResult
{
    cont::Array<u8> message;
    cont::Array<u8> group_info;
};

// What the first commit of a group that resumes a prior one holds (sections 11.2 and 11.3) besides the reinit or
// branch PSK: an Add of each other member, from key packages of the new group's cipher suite, and the new group's
// GroupInfo, which the Welcome seals.
struct ResumeOptions
{
    cont::ConstSpan<KeyPackage> members;
    GroupInfoOptions            group_info;
};

// A group that resumes a prior one, at its epoch 1: the Welcome's MLSMessage for its other members (empty when it has
// none) and the signed GroupInfo's MLSMessage.
struct ResumeResult
{
    cont::Array<u8> welcome;
    cont::Array<u8> group_info;
};

// A message a member processed (Group::process): what it carried, as views of the bytes it owns (a PrivateMessage is
// opened in place), valid until it is processed again or destroyed.
class ReceivedMessage
{
public:
    ReceivedMessage() noexcept = default;
    ~ReceivedMessage() noexcept;
    ReceivedMessage(const ReceivedMessage&)            = delete;
    ReceivedMessage& operator=(const ReceivedMessage&) = delete;
    ReceivedMessage(ReceivedMessage&&)                 = delete;
    ReceivedMessage& operator=(ReceivedMessage&&)      = delete;

    [[nodiscard]] WireFormat          wire_format() const noexcept { return m_wire_format; }
    [[nodiscard]] ContentType         content_type() const noexcept { return m_content.content_type; }
    [[nodiscard]] Sender              sender() const noexcept { return m_content.sender; }
    [[nodiscard]] cont::ConstSpan<u8> authenticated_data() const noexcept { return m_content.authenticated_data; }
    // An application message's data.
    [[nodiscard]] cont::ConstSpan<u8> application_data() const noexcept
    {
        return m_content.content_type == ContentType::Application ? m_content.body : cont::ConstSpan<u8>();
    }
    // A proposal and the reference the group cached it under.
    [[nodiscard]] const Proposal&     proposal() const noexcept { return m_proposal; }
    [[nodiscard]] cont::ConstSpan<u8> proposal_ref() const noexcept { return cont::ConstSpan<u8>(m_ref, m_ref_bytes); }
    // A commit, staged.
    [[nodiscard]] StagedCommit&       commit() noexcept { return m_commit; }
    [[nodiscard]] const StagedCommit& commit() const noexcept { return m_commit; }

private:
    friend class Group;
    void wipe() noexcept;

    cont::Array<u8> m_bytes;
    WireFormat      m_wire_format = WireFormat::PublicMessage;
    FramedContent   m_content;
    ContentAuth     m_auth;
    Proposal        m_proposal;
    u8              m_ref[kMaxHashBytes] = {};
    usize           m_ref_bytes          = 0U;
    StagedCommit    m_commit;
};

// One member's state of one epoch (GroupEpoch), the member's leaf and path keys, the proposals it cached this epoch,
// the resumption PSKs of past epochs and the leaf keys of its own pending Updates.
class Group
{
public:
    Group() noexcept = default;
    ~Group() noexcept;
    Group(const Group&)            = delete;
    Group& operator=(const Group&) = delete;
    Group(Group&&)                 = delete;
    Group& operator=(Group&&)      = delete;

    // A new group (section 11): `creator` alone in its tree, epoch 0, the epoch's secrets from a fresh random init
    // secret with no commit or PSK secret (so epoch_secret is a fresh random value), the confirmation tag over the
    // empty confirmed transcript hash and the interim hash from it. `creator` is a key-package leaf whose private
    // HPKE key is `leaf_private`, checked as a sent leaf (check_leaf and its signature); `extensions` is the body of
    // the GroupContext's Extension list, each type one the creator supports. Unsupported for a suite outside the
    // profile or a config without a verifier and an Authentication Service, BadLength for bounds past decision M10's;
    // BadKey when `leaf_private` is not the leaf's key; the checks' refusals pass through.
    [[nodiscard]] CryptoError create(const CipherSuite& suite, cont::ConstSpan<u8> group_id,
                                     cont::ConstSpan<u8> extensions, const LeafNode& creator,
                                     cont::ConstSpan<u8> leaf_private, const GroupConfig& config) noexcept;

    // Joins from a Welcome (section 12.4.3.1): open_welcome, then
    //   Replayed     a group id the store says the member already takes part in;
    //   NotReady     no ratchet tree: neither `ratchet_tree` (the tree the member fetched, the ratchet_tree
    //                extension's encoding) nor the GroupInfo's ratchet_tree extension; the tree's decoder refusals
    //                pass through, and a tree given both ways is taken from `ratchet_tree`;
    //   Forged       a tree whose root hash is not the GroupContext's tree_hash, a GroupInfo whose signer leaf is
    //                blank or whose signature does not verify, or a confirmation tag that does not;
    //   (the tree)   RatchetTree::validate's refusals, then check_leaf's for every leaf (role Tree; on the member's
    //                own leaf, which is its package's, that is section 13.4's rule that the joiner supports every
    //                GroupContext extension), BadKey for a node's key no agreement can use, check_credential_types',
    //                and the Authentication Service's refusal of an external sender (Forged);
    //   BadKey       no leaf identical to `own`'s, a `leaf_private` that is not its key, or a path secret whose keys
    //                are not the tree's (PathKeys::implant at the common ancestor of the signer's and the member's
    //                leaves).
    // A Welcome whose PSKs resume `prior` (its reinit or branch PSK, open_welcome's) joins a group that reinitializes
    // or branches it (sections 11.2, 11.3 and 12.4.3.1; decision M12), after the checks above:
    //   Malformed    a group past epoch 1; for a reinit, a group id, cipher suite or extensions other than the
    //                ReInit's (or a ReInit of another version); for a branch, another cipher suite than `prior`'s;
    //   NotReady     a reinit PSK of a `prior` that no ReInit ended;
    //   Forged       a member the Authentication Service does not judge one of `prior`'s (valid_successor of a
    //                distinct leaf of `prior`), or, for a reinit, a member of `prior` the new group lacks.
    // A reinitialization may keep `prior`'s group id, which the store's check then spares. `prior` is another group
    // than this one (Malformed otherwise) and is consulted only for such a Welcome.
    // `own`, `init_private` and `leaf_private` are the member's key package and its two private keys. Replaces what
    // the group held; a refusal leaves it empty.
    [[nodiscard]] CryptoError join(cont::ConstSpan<u8> welcome, const KeyPackage& own,
                                   cont::ConstSpan<u8> init_private, cont::ConstSpan<u8> leaf_private,
                                   cont::ConstSpan<u8> ratchet_tree, const GroupConfig& config,
                                   const Group* prior = nullptr) noexcept;

    // The new group of a ReInit (section 11.2; decision M12) from `ended`, a group a commit of that ReInit ended: the
    // ReInit's group id, cipher suite and extensions (`creator`, `leaf_private` and `signer` as `create` and `commit`
    // take them, in the new suite), then its first commit, merged at once (the creator is its only member at epoch
    // 0): an Add of each of `options.members`, the PSK of usage reinit naming `ended` and its epoch (the epoch the
    // ReInit's commit began) under a fresh Nh-byte nonce, with `ended`'s resumption secret, and a path; the group
    // is then at epoch 1 and `out` holds its Welcome and GroupInfo. In this order: Malformed for `ended` this group,
    // NotReady for a group no ReInit ended, Unsupported for a ReInit of a version or a cipher suite this build does
    // not implement; `create`'s and `commit`'s refusals; check_resumed's, so the new group is one its members take.
    // Replaces what the group held; a refusal leaves it and `out` empty.
    [[nodiscard]] CryptoError reinit(const Group& ended, const Signer& signer, const LeafNode& creator,
                                     cont::ConstSpan<u8> leaf_private, const ResumeOptions& options,
                                     const GroupConfig& config, ResumeResult& out) noexcept;

    // A subgroup branched from `from` (section 11.3; decision M12): a new group of `group_id` and `extensions` in
    // `from`'s cipher suite, whose first commit is `reinit`'s with a PSK of usage branch naming `from` and its current
    // epoch. Malformed for `from` this group, NotReady for an empty one; then as `reinit`.
    [[nodiscard]] CryptoError branch(const Group& from, cont::ConstSpan<u8> group_id, cont::ConstSpan<u8> extensions,
                                     const Signer& signer, const LeafNode& creator, cont::ConstSpan<u8> leaf_private,
                                     const ResumeOptions& options, const GroupConfig& config,
                                     ResumeResult& out) noexcept;

    // Joins by an external commit (section 12.4.3.2; decision M11) from `group_info`, a GroupInfo MLSMessage that
    // carries the external_pub extension: the ExternalInit's init secret exported to the group's external key (section
    // 8.3), the options' Remove and PSKs, the joiner at the leftmost free leaf with a fresh path (`leaf`'s signature
    // key, which is `signer`'s, its credential, capabilities and extensions), the commit signed under the GroupInfo's
    // GroupContext as a PublicMessage, the new epoch derived and confirmed and its GroupInfo signed by the joiner. The
    // group is then at the new epoch, with no state of the one before: when the delivery service refuses the commit
    // (another commit won the epoch), the application drops the group and joins again from a newer GroupInfo. In
    // this order:
    //   Malformed    a message that is not a GroupInfo or does not decode whole, a confirmation tag not Nh long, an
    //                extension list naming a type twice, an external_pub that does not decode, or GroupInfoOptions
    //                extensions naming a type twice, ratchet_tree or external_pub;
    //   Unsupported  a cipher suite outside the profile, a config without a verifier and an Authentication Service, a
    //                signer of another algorithm or key than `leaf`'s, or a GroupInfo without external_pub;
    //   (the tree)   Group::join's refusals of the tree, its root hash, the GroupInfo's signature and the members;
    //   BadKey       an external key no agreement can use;
    //   (the list)   `process`'s refusals of an external commit's list, leaf and path, as the members will check them
    //                (a resync of a blank or outside leaf is Malformed, a successor the Authentication Service
    //                refuses Forged, a PSK the store does not hold NotReady).
    // Replaces what the group held; a refusal leaves it and `out` empty.
    [[nodiscard]] CryptoError join_external(const Signer& signer, const LeafNode& leaf, cont::ConstSpan<u8> group_info,
                                            cont::ConstSpan<u8> ratchet_tree, const ExternalJoinOptions& options,
                                            const GroupConfig& config, ExternalJoinResult& out) noexcept;

    // Processes one MLSMessage of the group (sections 6, 12.1, 12.4.2 and 12.4.3.2), in this order:
    //   NotReady     an empty group; OutOfWindow for one a ReInit ended (section 12.4.2), which takes no message;
    //   Unsupported  a wire format other than a public or private message;
    //   Malformed    a message that does not decode, of another group, framing section 6 refuses, a PrivateMessage
    //                whose padding or content is malformed, a proposal of a type its sender may not send (an
    //                ExternalInit, an external sender's Update, a new member's proposal other than an Add), a commit
    //                whose list section 12.2 refuses (a ReInit beside another proposal, or of an older version; a
    //                reinit or branch PSK, which only a resumed group's first commit holds) or that lacks a required
    //                path, or an external commit (section
    //                12.4.3.2) without a path, with a proposal by reference, or whose list is not one ExternalInit
    //                (its kem_output Npk long), at most one Remove and PSKs;
    //   OutOfWindow  another epoch, a sender leaf that is blank or outside the tree, or an external sender the group
    //                does not list; the secret tree's refusals of a PrivateMessage (Replayed, OutOfWindow) pass
    //                through;
    //   Forged       a membership tag, an AEAD seal, a signature or a confirmation tag that does not verify (an
    //                external commit whose init secret is not the group's external key's shows here), or a new leaf
    //                the Authentication Service refuses as the successor of the leaf it replaces (an Update, a path,
    //                an external commit's resync);
    //   Replayed     a proposal already cached, or the member's own commit (its StagedCommit is merged instead);
    //   Exhausted    a proposal past the epoch's cache (decision M10);
    //   NotReady     a commit referencing a proposal the member never cached or a PSK it does not hold; and a
    //                commit's checks against the group: each proposal's (check_key_package, check_leaf and its
    //                signature, a Remove of a blank leaf as Malformed), the new tree's keys and credential types
    //                (BadKey, Unsupported) and the path's (merge_path, process_update_path; an external joiner's
    //                leaf at the leftmost free leaf, section 12.4.2); an ExternalInit's kem_output no agreement can
    //                use is BadKey or WeakSecret.
    // A proposal is cached under its reference, an application message's data is read, and a commit is staged in
    // `out.commit()` for merge: nothing of the group changes but the secret tree's spent generation of a
    // PrivateMessage (section 9.2). A refusal leaves `out` empty.
    [[nodiscard]] CryptoError process(cont::ConstSpan<u8> message, ReceivedMessage& out) noexcept;

    // The member's own proposal (section 12.1) as an MLSMessage in `out`, cached as received (`ref`, Nh bytes, its
    // reference): an Add, Remove, PreSharedKey, ReInit or GroupContextExtensions. NotReady for an empty group or one a
    // ReInit ended (check_signer's, as for every send); Malformed for an Update (propose_update's), an ExternalInit, a
    // wire format other than a public or private message or a proposal that does not encode; Unsupported for another
    // type, or a signer that is not the member's leaf's or of another algorithm; BadLength for `ref` not Nh long;
    // Exhausted past the epoch's cache. On a refusal `out` and `ref` are empty.
    [[nodiscard]] CryptoError propose(const Signer& signer, const Proposal& proposal, const SendOptions& options,
                                      cont::Array<u8>& out, cont::Span<u8> ref) noexcept;
    // An Update of the member's own leaf (section 12.1.2): a fresh HPKE key, the leaf re-signed with source update,
    // proposed as `propose` does; the private key is kept until a commit adopts the leaf or the epoch ends.
    [[nodiscard]] CryptoError propose_update(const Signer& signer, const SendOptions& options, cont::Array<u8>& out,
                                             cont::Span<u8> ref) noexcept;
    // An application message (section 6.3), always a PrivateMessage; NotReady while the member holds proposals this
    // epoch (section 12.4: a member that saw valid proposals commits before it sends application data).
    [[nodiscard]] CryptoError protect(const Signer& signer, cont::ConstSpan<u8> data, const SendOptions& options,
                                      cont::Array<u8>& out) noexcept;

    // The member's commit (section 12.4.1): the proposals CommitOptions names, validated as a receiver would (the
    // refusals of `process` for a commit), a path when required or asked for, the commit signed under the current
    // GroupContext and protected with the current epoch's keys, the new epoch's GroupInfo signed, and a Welcome for the
    // members it adds (each with its path secret, every PSK named); a ReInit's commit publishes no external key
    // (Malformed with GroupInfoOptions::external_pub: the group it ends takes no joiner). Nothing of the group changes
    // until `merge`, but a PrivateMessage spends the sender's handshake generation. On a refusal `out` is empty.
    [[nodiscard]] CryptoError commit(const Signer& signer, const CommitOptions& options, CommitResult& out) noexcept;

    // The current epoch's GroupInfo MLSMessage (section 12.4.3), signed by the member: what it publishes for new
    // members to join from by an external commit. Malformed for options extensions naming a type twice, ratchet_tree
    // or external_pub; check_signer's refusals.
    [[nodiscard]] CryptoError group_info(const Signer& signer, const GroupInfoOptions& options,
                                         cont::Array<u8>& out) const noexcept;

    // Makes a staged commit the group's epoch: the past epoch's resumption PSK kept, the member's path keys pruned
    // (section 7.5) and the path's applied, an Update of the member's own leaf adopting its kept key, the proposal
    // cache dropped. A commit that removes the member leaves the group empty; a ReInit's ends it (`ended`). OutOfWindow
    // for a commit staged for another group or epoch (the group moved on), NotReady for an empty one, ProviderFailure
    // when a ReInit cannot be kept (the group unchanged); `staged` is left empty either way.
    [[nodiscard]] CryptoError merge(StagedCommit& staged) noexcept;

    // Whether a merged ReInit's commit ended the group (section 12.4.2): it sends and processes no more messages, keeps
    // its last epoch's secrets, and waits for the Welcome of the new group the ReInit names, which `reinit` creates and
    // `join` with this group as `prior` enters. `reinit_proposal` reads the ReInit (its fields views of the group's
    // state); false for a group no ReInit ended.
    [[nodiscard]] bool ended() const noexcept { return !m_reinit.empty(); }
    [[nodiscard]] bool reinit_proposal(ReInit& out) const noexcept;

    [[nodiscard]] bool               ready() const noexcept { return m_suite != nullptr; }
    [[nodiscard]] const CipherSuite& suite() const noexcept { return *m_suite; }
    [[nodiscard]] u64                epoch() const noexcept { return m_now.epoch; }
    [[nodiscard]] u32                own_leaf() const noexcept { return m_keys.leaf(); }
    // The epoch's GroupContext, its fields views of the group's state (valid until it changes).
    [[nodiscard]] GroupContext        context() const noexcept;
    [[nodiscard]] const RatchetTree&  tree() const noexcept { return m_now.tree; }
    [[nodiscard]] const PathKeys&     keys() const noexcept { return m_keys; }
    [[nodiscard]] SecretTree&         secret_tree() noexcept { return m_now.secret_tree; }
    [[nodiscard]] const SecretTree&   secret_tree() const noexcept { return m_now.secret_tree; }
    [[nodiscard]] cont::ConstSpan<u8> secret(EpochSecret which) const noexcept { return m_now.secrets.get(which); }
    [[nodiscard]] cont::ConstSpan<u8> epoch_authenticator() const noexcept
    {
        return m_now.secrets.get(EpochSecret::Authentication);
    }
    [[nodiscard]] cont::ConstSpan<u8> interim_transcript_hash() const noexcept
    {
        return cont::ConstSpan<u8>(m_now.interim.data(), m_now.interim.size());
    }
    // The proposals cached this epoch.
    [[nodiscard]] usize pending_proposals() const noexcept { return m_cache.size(); }

private:
    struct Cached // one proposal received or sent this epoch
    {
        u8              ref[kMaxHashBytes] = {};
        Sender          sender;
        cont::Array<u8> bytes; // the Proposal's encoding
    };
    struct Resumption // a past epoch's resumption PSK
    {
        u64 epoch                 = 0U;
        u8  secret[kMaxHashBytes] = {};
    };
    struct Item; // one proposal of a commit's list (mls_commit.cpp)
    struct Plan; // a commit's list and what applying it gives (mls_commit.cpp)

    void wipe() noexcept;
    void forget_epoch() noexcept; // the proposal cache and the pending leaves
    [[nodiscard]] CryptoError check_config(const GroupConfig& config) const noexcept;
    // The interim transcript hash from the epoch's confirmation tag; Forged when `tag` is not the tag the epoch's
    // confirmation key gives its confirmed transcript hash.
    [[nodiscard]] CryptoError confirm(GroupEpoch& epoch, cont::ConstSpan<u8> tag) const noexcept;
    [[nodiscard]] CryptoError own_bytes(cont::ConstSpan<u8> group_id, cont::ConstSpan<u8> extensions,
                                        cont::ConstSpan<u8> confirmed) noexcept;
    // The secret of a pre-shared key: this group's own resumption PSKs first, then the store's.
    [[nodiscard]] cont::ConstSpan<u8> psk_of(const PreSharedKeyId& id) const noexcept;
    // The kept leaf key of the member's own Update whose public key is `public_key` (its index; size() for none).
    [[nodiscard]] usize pending_leaf(cont::ConstSpan<u8> public_key) const noexcept;
    // NotReady for an empty group or one a ReInit ended; Unsupported unless `signer` holds the suite's algorithm and
    // the member's leaf's signature key.
    [[nodiscard]] CryptoError check_signer(const Signer& signer) const noexcept;
    // Decision M12's checks of the group a Welcome or a member's `resume` made against the `prior` group it resumes by
    // a PSK of `usage` (sections 11.2, 11.3 and 12.4.3.1): Group::join's refusals of a resumed group.
    [[nodiscard]] CryptoError check_resumed(const Group& prior, ResumptionUsage usage) const noexcept;
    // `reinit` and `branch`: the new group of `suite`, `group_id` and `extensions`, its first commit with the PSK of
    // `usage` naming `from` and its current epoch, merged, and check_resumed.
    [[nodiscard]] CryptoError resume(const Group& from, ResumptionUsage usage, const CipherSuite& suite,
                                     cont::ConstSpan<u8> group_id, cont::ConstSpan<u8> extensions,
                                     const Signer& signer, const LeafNode& creator, cont::ConstSpan<u8> leaf_private,
                                     const ResumeOptions& options, const GroupConfig& config,
                                     ResumeResult& out) noexcept;
    // `commit` with the key a resumed group's first commit holds (`resumed`; nullptr for every other commit).
    [[nodiscard]] CryptoError commit_with(const Signer& signer, const CommitOptions& options,
                                          const PreSharedKey* resumed, CommitResult& out) noexcept;
    // The tree of a GroupInfo a member joins from (`ratchet_tree`, else its extension) and Group::join's checks of it.
    [[nodiscard]] CryptoError take_tree(const CipherSuite& suite, const GroupInfo& info,
                                        cont::ConstSpan<u8> ratchet_tree) noexcept;
    // `epoch`'s GroupInfo signed by the member at `signer_leaf`, as an MLSMessage in `out`.
    [[nodiscard]] CryptoError seal_group_info(const Signer& signer, u32 signer_leaf, const GroupEpoch& epoch,
                                              const GroupInfoOptions& options, cont::Array<u8>& out) const noexcept;
    // An external commit's init secret (section 8.3) from its ExternalInit's kem_output, under the epoch's external
    // key.
    [[nodiscard]] CryptoError external_init_secret(cont::ConstSpan<u8> kem_output, cont::Span<u8> out) const noexcept;
    // A member's content signed under the current GroupContext, and protected with the current epoch's keys.
    [[nodiscard]] CryptoError sign_own(const Signer& signer, WireFormat wire_format, const FramedContent& content,
                                       cont::Span<u8> signature, usize& written) const noexcept;
    [[nodiscard]] CryptoError wrap(const SendOptions& options, const FramedContent& content, const ContentAuth& auth,
                                   cont::Array<u8>& out) noexcept;
    [[nodiscard]] CryptoError send_proposal(const Signer& signer, const Proposal& proposal, const SendOptions& options,
                                            cont::Array<u8>& out, cont::Span<u8> ref) noexcept;
    // A verified proposal cached under its reference (`ref`): Replayed when it already is, Exhausted past the bound.
    [[nodiscard]] CryptoError cache(WireFormat wire_format, const FramedContent& content, const ContentAuth& auth,
                                    cont::Span<u8> ref) noexcept;
    [[nodiscard]] const Cached* cached(cont::ConstSpan<u8> ref) const noexcept;
    // The signature key of a received message's sender: a member's leaf (Replayed for the member itself), an external
    // sender the group lists, a new member's key package.
    [[nodiscard]] CryptoError sender_key(const ReceivedMessage& message, cont::ConstSpan<u8>& key) const noexcept;
    // Sections 12.2 and 12.3 over a commit's list from `committer` (a member, or an external joiner: section
    // 12.4.3.2's list): the list checked and applied to a copy of the tree and GroupContext in `staged`, its PSKs
    // looked up into `plan`.
    [[nodiscard]] CryptoError stage_list(const Sender& committer, Plan& plan, StagedCommit& staged) const noexcept;
    // A commit path's leaf at `index` against the staged next epoch: check_leaf (role Commit), its signature, and the
    // Authentication Service's acceptance of it as the successor of `predecessor` (the committer's former leaf, or an
    // external joiner's removed one; none for a joiner that removes no leaf).
    [[nodiscard]] CryptoError check_path_leaf(const LeafNode& leaf, u32 index, const Credential* predecessor,
                                              const StagedCommit& staged) const noexcept;
    // The proposals a commit holds by value appended to `plan` from `sender`, their encodings into `encodings`.
    [[nodiscard]] static CryptoError add_by_value(cont::ConstSpan<Proposal> proposals, const Sender& sender,
                                                  cont::Array<u8>& encodings, Plan& plan) noexcept;
    // The commit's key schedule (section 12.4.2): the confirmed transcript hash, the new GroupContext, the epoch's
    // secrets from `init_secret` (the current epoch's, or an external commit's), the commit secret and the PSKs, and
    // its secret tree.
    [[nodiscard]] CryptoError advance(StagedCommit& staged, const Plan& plan, WireFormat wire_format,
                                      const FramedContent& content, cont::ConstSpan<u8> signature,
                                      cont::ConstSpan<u8> init_secret,
                                      cont::ConstSpan<u8> commit_secret) const noexcept;
    // The committed proposals' encodings copied into `staged`, which then names this group and epoch.
    [[nodiscard]] CryptoError record(const Plan& plan, const Sender& committer, bool has_path,
                                     StagedCommit& staged) const noexcept;
    [[nodiscard]] CryptoError process_commit(ReceivedMessage& out) noexcept;
    // The cached proposals section 12.2 prefers for the member's commit (CommitOptions::pending); a ReInit only when
    // nothing else, `by_value` included, is committed.
    [[nodiscard]] CryptoError select_pending(u32 committer, bool by_value, Plan& plan) const noexcept;

    const CipherSuite*       m_suite = nullptr;
    GroupConfig              m_config;
    cont::Array<u8>          m_group_id;
    GroupEpoch               m_now;
    PathKeys                 m_keys;
    cont::Array<Cached>      m_cache;
    cont::Array<Resumption>  m_resumption; // a ring of config.resumption_epochs past epochs
    usize                    m_resumption_next = 0U;
    cont::Array<HpkeKey>     m_pending_leaves; // the leaf keys of the member's own Updates
    cont::Array<u8>          m_reinit;         // the encoding of the ReInit proposal that ended the group
};
} // namespace crd::crypto::mls
