#pragma once

// crd-crypto -- an MLS group, RFC 9420 (NET.2; contract: docs/design/network-foundation.md#mls, M3 layer 8 and
// decisions M8 and M9): the group's extensions (sections 7.2, 11.1, 12.1.8.1 and 13.4), the validation of leaf nodes
// and key packages against a group (5.3.1, 7.3, 10.1 and 13.3), the GroupInfo and Welcome a committer seals for new
// members (12.4.3 and 12.4.3.1), and a member's group state: a new group (section 11) and one joined from a Welcome.
//
//   time        every lifetime check takes the caller's clock (LeafPolicy::now); nothing here reads one.
//   identity    whether a credential binds acceptable identifiers to its key is the application's Authentication
//               Service (section 5.3.1); which pre-shared keys and groups a member holds is its GroupStore.
//   atomicity   a group that refuses a create or a join is left empty: nothing of the refused state remains.
//   secrets     the joiner, path, PSK and epoch secrets, the decrypted GroupSecrets and the private keys are zeroed
//               when replaced or destroyed.
//   allocation  the handshake path: a join allocates the tree, the decrypted Welcome and the epoch's secret tree.

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

// Whether a leaf's capabilities support an extension or a proposal type: a default one or one they list.
[[nodiscard]] bool supports_extension(const Capabilities& capabilities, u16 type) noexcept;
[[nodiscard]] bool supports_proposal(const Capabilities& capabilities, u16 type) noexcept;

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
//   BadKey       an encryption key no agreement in the suite's KEM can use (check_agreement_public_key);
//   Forged       a credential the Authentication Service refuses for the leaf's signature key;
//   Unsupported  capabilities without the leaf's own credential type, a non-default leaf extension they do not
//                list, or a GroupContext extension or required capability (section 11.1) they do not support; an
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

// A Welcome opened for one member: its GroupSecrets and GroupInfo, views of the decrypted bytes it owns, and the PSK
// secret of the GroupSecrets' keys. Zeroed when replaced or destroyed.
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

private:
    friend CryptoError open_welcome(cont::ConstSpan<u8>, const KeyPackage&, cont::ConstSpan<u8>, const GroupStore&,
                                    OpenedWelcome&) noexcept;
    void wipe() noexcept;

    const CipherSuite* m_suite = nullptr;
    cont::Array<u8>    m_secrets_bytes; // the decrypted GroupSecrets
    cont::Array<u8>    m_info_bytes;    // the decrypted GroupInfo
    GroupSecrets       m_secrets;
    GroupInfo          m_info;
    u8                 m_psk_secret[kMaxHashBytes] = {};
};

// The first steps of joining (section 12.4.3.1) for the member whose key package is `own` and whose init key is
// `init_private` (SerializePrivateKey form), in this order:
//   Malformed    a Welcome, GroupSecrets or GroupInfo that does not decode whole, secrets or tags of the wrong
//                length, a PSK identity named twice or with a nonce not Nh long, or an extension list naming a type
//                twice;
//   Unsupported  a cipher suite outside the profile or other than the key package's, a GroupInfo of another suite,
//                or a resumption PSK of usage reinit or branch (section 11's reinitialization and branching);
//   BadKey       no entry for the key package's KeyPackageRef, or an init key that is not the package's;
//   NotReady     a PSK the store does not hold;
//   Forged       GroupSecrets or a GroupInfo that does not open (HPKE's other refusals pass through).
// The GroupInfo's signature, tree and confirmation tag are Group::join's. A refusal leaves `out` empty.
[[nodiscard]] CryptoError open_welcome(cont::ConstSpan<u8> welcome, const KeyPackage& own,
                                       cont::ConstSpan<u8> init_private, const GroupStore& store,
                                       OpenedWelcome& out) noexcept;

// ---- A member's group state (sections 8, 11 and 12.4.3.1; decision M9) ----

// What a group consults: the signature verifier, the Authentication Service and the member's store (each outliving
// the group), the leaf policy and the secret tree's receive window.
struct GroupConfig
{
    const SignatureVerifier*     verifier       = nullptr;
    const AuthenticationService* authentication = nullptr;
    const GroupStore*            store          = nullptr;
    LeafPolicy                   policy;
    RatchetWindow                window;
};

// One member's state of one epoch: the GroupContext's fields, the ratchet tree, the member's leaf and path keys, the
// epoch's secrets, its secret tree and the interim transcript hash.
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
    // profile or a config without a verifier and an Authentication Service; BadKey when `leaf_private` is not the
    // leaf's key; the checks' refusals pass through.
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
    // `own`, `init_private` and `leaf_private` are the member's key package and its two private keys. Replaces what
    // the group held; a refusal leaves it empty.
    [[nodiscard]] CryptoError join(cont::ConstSpan<u8> welcome, const KeyPackage& own,
                                   cont::ConstSpan<u8> init_private, cont::ConstSpan<u8> leaf_private,
                                   cont::ConstSpan<u8> ratchet_tree, const GroupConfig& config) noexcept;

    [[nodiscard]] bool               ready() const noexcept { return m_suite != nullptr; }
    [[nodiscard]] const CipherSuite& suite() const noexcept { return *m_suite; }
    [[nodiscard]] u64                epoch() const noexcept { return m_epoch; }
    [[nodiscard]] u32                own_leaf() const noexcept { return m_keys.leaf(); }
    // The epoch's GroupContext, its fields views of the group's state (valid until it changes).
    [[nodiscard]] GroupContext        context() const noexcept;
    [[nodiscard]] const RatchetTree&  tree() const noexcept { return m_tree; }
    [[nodiscard]] const PathKeys&     keys() const noexcept { return m_keys; }
    [[nodiscard]] SecretTree&         secret_tree() noexcept { return m_secret_tree; }
    [[nodiscard]] const SecretTree&   secret_tree() const noexcept { return m_secret_tree; }
    [[nodiscard]] cont::ConstSpan<u8> secret(EpochSecret which) const noexcept { return m_secrets.get(which); }
    [[nodiscard]] cont::ConstSpan<u8> epoch_authenticator() const noexcept
    {
        return m_secrets.get(EpochSecret::Authentication);
    }
    [[nodiscard]] cont::ConstSpan<u8> interim_transcript_hash() const noexcept
    {
        return cont::ConstSpan<u8>(m_interim.data(), m_interim.size());
    }

private:
    void wipe() noexcept;
    // The interim transcript hash from the epoch's confirmation tag; Forged when `tag` is not the tag the epoch's
    // confirmation key gives its confirmed transcript hash.
    [[nodiscard]] CryptoError confirm(cont::ConstSpan<u8> tag) noexcept;
    [[nodiscard]] CryptoError own_bytes(cont::ConstSpan<u8> group_id, cont::ConstSpan<u8> extensions,
                                        cont::ConstSpan<u8> confirmed) noexcept;

    const CipherSuite* m_suite = nullptr;
    GroupConfig        m_config;
    u64                m_epoch = 0U;
    cont::Array<u8>    m_group_id;
    cont::Array<u8>    m_extensions;
    cont::Array<u8>    m_confirmed;
    cont::Array<u8>    m_interim;
    RatchetTree        m_tree;
    PathKeys           m_keys;
    EpochSecrets       m_secrets;
    SecretTree         m_secret_tree;
};
} // namespace crd::crypto::mls
