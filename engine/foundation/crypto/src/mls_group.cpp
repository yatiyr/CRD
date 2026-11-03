// crd-crypto -- an MLS group, RFC 9420: extensions, leaf node and key package validation against a group, GroupInfo
// and Welcome, a new group and joining one from a Welcome (NET.2; contract: docs/design/network-foundation.md#mls).
#include <crd/crypto/mls_group.hpp>

#include "byte_spans.hpp"
#include "mls_detail.hpp"

#include <crd/crypto/policy.hpp>

#include <algorithm>

namespace crd::crypto::mls
{
namespace
{
using detail::LabelBuffer;
using detail::refuse;
using detail::repeats;
using detail::same_bytes;

constexpr cont::StringView kKeyPackageLabel = "KeyPackageTBS";
constexpr cont::StringView kGroupInfoLabel  = "GroupInfoTBS";
constexpr cont::StringView kWelcomeLabel    = "Welcome";
constexpr cont::StringView kKeyPackageRef   = "MLS 1.0 KeyPackage Reference";

// Lists this long are checked for repeats pair by pair; longer ones through a bitmap of the 16-bit codepoint space.
constexpr usize kPairwiseEntries = 32U;

u16 codepoint(cont::ConstSpan<u8> list, usize i) noexcept
{
    return static_cast<u16>(static_cast<u16>(list[2U * i] << 8U) | list[2U * i + 1U]);
}

// The bits of the 16-bit codepoint space (8 KiB).
class CodepointSet
{
public:
    // Whether `value` was already in the set; adds it.
    bool insert(u16 value) noexcept
    {
        u64&      word = m_words[value / 64U];
        const u64 bit  = u64{1} << (value % 64U);
        const bool had = (word & bit) != 0U;
        word |= bit;
        return had;
    }
    [[nodiscard]] bool has(u16 value) const noexcept
    {
        return (m_words[value / 64U] & (u64{1} << (value % 64U))) != 0U;
    }
    void erase(u16 value) noexcept { m_words[value / 64U] &= ~(u64{1} << (value % 64U)); }

private:
    u64 m_words[1024] = {};
};

// The sources section 7.3 admits for a leaf where it appears.
bool source_admitted(LeafRole role, LeafNodeSource source) noexcept
{
    switch (role)
    {
    case LeafRole::KeyPackage:
        return source == LeafNodeSource::KeyPackage;
    case LeafRole::Update:
        return source == LeafNodeSource::Update;
    case LeafRole::Commit:
        return source == LeafNodeSource::Commit;
    case LeafRole::Tree:
        return true;
    }
    return false;
}

// Whether `test` holds for every codepoint of a list's body.
template <typename Test>
bool every_codepoint(cont::ConstSpan<u8> list, Test test) noexcept
{
    for (usize i = 0U; i < list.size() / 2U; ++i)
    {
        if (!test(codepoint(list, i)))
        {
            return false;
        }
    }
    return true;
}

// Section 11.1: the capabilities support every extension, proposal and credential type the group requires.
CryptoError check_required(cont::ConstSpan<u8> extensions, const Capabilities& capabilities) noexcept
{
    cont::ConstSpan<u8> data;
    if (!find_extension(extensions, static_cast<u16>(ExtensionType::RequiredCapabilities), data))
    {
        return CryptoError::None;
    }
    RequiredCapabilities required;
    if (!take_required_capabilities(data, required))
    {
        return CryptoError::Malformed;
    }
    const bool supported =
        every_codepoint(required.extensions, [&](u16 type) { return supports_extension(capabilities, type); }) &&
        every_codepoint(required.proposals, [&](u16 type) { return supports_proposal(capabilities, type); }) &&
        every_codepoint(required.credentials, [&](u16 type) { return supports_credential(capabilities, type); });
    return supported ? CryptoError::None : CryptoError::Unsupported;
}

// KeyPackageTBS: the package's fields before its signature.
usize key_package_tbs_bytes(const KeyPackage& package) noexcept
{
    return Size()
        .fixed(4U)
        .vector(package.init_key.size())
        .field(leaf_node_bytes(package.leaf_node))
        .vector(package.extensions.size())
        .bytes();
}

void put_key_package_tbs(cont::WireWriter& out, const KeyPackage& package) noexcept
{
    out.put_u16(package.version);
    out.put_u16(package.cipher_suite);
    put_vector(out, package.init_key);
    put_leaf_node(out, package.leaf_node);
    put_vector(out, package.extensions);
}

// GroupInfoTBS: the GroupInfo's fields before its signature.
usize group_info_tbs_bytes(const GroupInfo& info) noexcept
{
    return Size()
        .field(group_context_bytes(info.context))
        .vector(info.extensions.size())
        .vector(info.confirmation_tag.size())
        .fixed(4U)
        .bytes();
}

void put_group_info_tbs(cont::WireWriter& out, const GroupInfo& info) noexcept
{
    put_group_context(out, info.context);
    put_vector(out, info.extensions);
    put_vector(out, info.confirmation_tag);
    out.put_u32(info.signer);
}

// The public key of `private_key` in the suite's KEM is `public_key`.
CryptoError check_private_key(const CipherSuite& suite, cont::ConstSpan<u8> private_key,
                              cont::ConstSpan<u8> public_key) noexcept
{
    HpkeKey key;
    u8      derived[kMaxNodeKeyBytes];
    const usize bytes = hpke_sizes(suite.kem).public_key;
    if (bytes == 0U || bytes > sizeof(derived) || key.from_private(suite.kem, private_key) != CryptoError::None ||
        key.public_key(cont::Span<u8>(derived, bytes)) != CryptoError::None)
    {
        return CryptoError::BadKey;
    }
    return same_bytes(cont::ConstSpan<u8>(derived, bytes), public_key) ? CryptoError::None : CryptoError::BadKey;
}

// Reserves `bytes` bytes of the writer's output, zeroed, and returns them: room for a value computed once the
// structure around it is laid out.
cont::Span<u8> reserve(cont::WireWriter& writer, cont::Span<u8> buffer, usize bytes) noexcept
{
    const usize at = writer.written();
    for (usize i = 0U; i < bytes; ++i)
    {
        writer.put_u8(0U);
    }
    return buffer.subspan(at, bytes);
}

} // namespace

// ---- Extensions ----

bool default_extension(u16 type) noexcept
{
    return type >= static_cast<u16>(ExtensionType::ApplicationId) &&
           type <= static_cast<u16>(ExtensionType::ExternalSenders);
}

bool default_proposal(u16 type) noexcept
{
    return type >= static_cast<u16>(ProposalType::Add) &&
           type <= static_cast<u16>(ProposalType::GroupContextExtensions);
}

bool lists(cont::ConstSpan<u8> codepoints, u16 value) noexcept
{
    return !every_codepoint(codepoints, [value](u16 listed) { return listed != value; });
}

bool supports_extension(const Capabilities& capabilities, u16 type) noexcept
{
    return default_extension(type) || (!grease(type) && lists(capabilities.extensions, type));
}

bool supports_proposal(const Capabilities& capabilities, u16 type) noexcept
{
    return default_proposal(type) || (!grease(type) && lists(capabilities.proposals, type));
}

bool supports_credential(const Capabilities& capabilities, u16 type) noexcept
{
    return !grease(type) && lists(capabilities.credentials, type);
}

bool extensions_distinct(cont::ConstSpan<u8> body) noexcept
{
    usize count = 0U;
    if (!each_extension(body,
                        [&count](u16, cont::ConstSpan<u8>)
                        {
                            ++count;
                            return true;
                        }))
    {
        return false;
    }
    if (count <= kPairwiseEntries)
    {
        u16   types[kPairwiseEntries] = {};
        usize seen                    = 0U;
        return each_extension(body,
                              [&](u16 type, cont::ConstSpan<u8>)
                              {
                                  for (usize i = 0U; i < seen; ++i)
                                  {
                                      if (types[i] == type)
                                      {
                                          return false;
                                      }
                                  }
                                  types[seen++] = type;
                                  return true;
                              });
    }
    CodepointSet types;
    return each_extension(body, [&types](u16 type, cont::ConstSpan<u8>) { return !types.insert(type); });
}

bool find_extension(cont::ConstSpan<u8> body, u16 type, cont::ConstSpan<u8>& data) noexcept
{
    bool found = false;
    data       = {};
    each_extension(body,
                   [&](u16 at, cont::ConstSpan<u8> bytes)
                   {
                       found = at == type;
                       if (found)
                       {
                           data = bytes;
                       }
                       return !found;
                   });
    return found;
}

bool take_required_capabilities(cont::ConstSpan<u8> data, RequiredCapabilities& out) noexcept
{
    cont::WireReader in(data);
    out = {};
    return take_u16_list(in, out.extensions) && take_u16_list(in, out.proposals) &&
           take_u16_list(in, out.credentials) && in.done();
}

usize required_capabilities_bytes(const RequiredCapabilities& required) noexcept
{
    const bool whole = required.extensions.size() % 2U == 0U && required.proposals.size() % 2U == 0U &&
                       required.credentials.size() % 2U == 0U;
    return whole ? Size()
                       .vector(required.extensions.size())
                       .vector(required.proposals.size())
                       .vector(required.credentials.size())
                       .bytes()
                 : 0U;
}

void put_required_capabilities(cont::WireWriter& out, const RequiredCapabilities& required) noexcept
{
    put_vector(out, required.extensions);
    put_vector(out, required.proposals);
    put_vector(out, required.credentials);
}

bool take_external_sender(cont::WireReader& in, ExternalSender& out) noexcept
{
    out = {};
    return take_vector(in, out.signature_key) && take_credential(in, out.credential);
}

usize external_sender_bytes(const ExternalSender& sender) noexcept
{
    return Size().vector(sender.signature_key.size()).field(credential_bytes(sender.credential)).bytes();
}

void put_external_sender(cont::WireWriter& out, const ExternalSender& sender) noexcept
{
    put_vector(out, sender.signature_key);
    put_credential(out, sender.credential);
}

bool take_external_senders(cont::ConstSpan<u8> data, cont::ConstSpan<u8>& body) noexcept
{
    cont::WireReader in(data);
    return take_list<ExternalSender>(in, body, take_external_sender) && in.done();
}

bool take_external_pub(cont::ConstSpan<u8> data, cont::ConstSpan<u8>& key) noexcept
{
    cont::WireReader in(data);
    if (take_vector(in, key) && in.done())
    {
        return true;
    }
    key = {};
    return false;
}

usize external_pub_bytes(cont::ConstSpan<u8> key) noexcept
{
    return vector_bytes(key.size());
}

void put_external_pub(cont::WireWriter& out, cont::ConstSpan<u8> key) noexcept
{
    put_vector(out, key);
}

// ---- Leaf nodes and key packages against a group ----

CryptoError supports_context(cont::ConstSpan<u8> extensions, const Capabilities& capabilities) noexcept
{
    const auto supported = [&capabilities](u16 type, cont::ConstSpan<u8>)
    { return supports_extension(capabilities, type); };
    return each_extension(extensions, supported) ? check_required(extensions, capabilities) : CryptoError::Unsupported;
}

CryptoError check_node_key(const CipherSuite& suite, cont::ConstSpan<u8> key) noexcept
{
    const CryptoError error = hpke_check_public_key(suite.kem, key);
    return error == CryptoError::None || error == CryptoError::ProviderFailure ? error : CryptoError::BadKey;
}

CryptoError check_psk_id(const CipherSuite& suite, const PreSharedKeyId& id) noexcept
{
    return id.nonce.size() == suite.hash_bytes() ? CryptoError::None : CryptoError::Malformed;
}

namespace
{
// Whether every extension of `extensions` is of a default type or one `capabilities` list (sections 7.2 and 10).
bool extensions_listed(const Capabilities& capabilities, cont::ConstSpan<u8> extensions) noexcept
{
    const auto listed = [&capabilities](u16 type, cont::ConstSpan<u8>)
    { return default_extension(type) || lists(capabilities.extensions, type); };
    return each_extension(extensions, listed);
}
} // namespace

CryptoError check_leaf(const GroupContext& context, const LeafNode& leaf, LeafRole role, const LeafPolicy& policy,
                       const AuthenticationService& authentication) noexcept
{
    const CipherSuite* suite = cipher_suite(context.cipher_suite);
    if (suite == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (!source_admitted(role, leaf.source) || !extensions_distinct(leaf.extensions))
    {
        return CryptoError::Malformed;
    }
    const CryptoError key = check_node_key(*suite, leaf.encryption_key);
    if (key != CryptoError::None)
    {
        return key;
    }
    if (!authentication.validate(leaf.credential, leaf.signature_key))
    {
        return CryptoError::Forged;
    }
    // The leaf's own extensions are those its capabilities list, GREASE included (section 13.5); its credential type is
    // one they support.
    const Capabilities& capabilities = leaf.capabilities;
    if (!supports_credential(capabilities, leaf.credential.type) || !extensions_listed(capabilities, leaf.extensions))
    {
        return CryptoError::Unsupported;
    }
    const CryptoError group = supports_context(context.extensions, capabilities);
    if (group != CryptoError::None)
    {
        return group;
    }
    if (leaf.source == LeafNodeSource::KeyPackage && (role != LeafRole::Tree || policy.tree_lifetimes))
    {
        const bool within = leaf.not_before <= leaf.not_after &&
                            leaf.not_after - leaf.not_before <= policy.max_lifetime && leaf.not_before <= policy.now &&
                            policy.now <= leaf.not_after;
        if (!within)
        {
            return CryptoError::Expired;
        }
    }
    return CryptoError::None;
}

CryptoError check_credential_types(const RatchetTree& tree) noexcept
{
    // The types in use, each once; then each member's listed types marked in a second set and checked against them.
    cont::Array<CodepointSet> sets;
    cont::Array<u16>          in_use;
    if (!sets.try_reserve(2U))
    {
        return CryptoError::ProviderFailure;
    }
    sets.resize(2U);
    CodepointSet& used   = sets[0];
    CodepointSet& listed = sets[1];
    for (u32 leaf = 0U; leaf < tree.leaves(); ++leaf)
    {
        LeafNode node{};
        if (tree.leaf_node(leaf, node) && !used.insert(node.credential.type))
        {
            if (!in_use.try_reserve(in_use.size() + 1U))
            {
                return CryptoError::ProviderFailure;
            }
            in_use.push_back(node.credential.type);
        }
    }
    for (u32 leaf = 0U; leaf < tree.leaves(); ++leaf)
    {
        LeafNode node{};
        if (!tree.leaf_node(leaf, node))
        {
            continue;
        }
        const cont::ConstSpan<u8> credentials = node.capabilities.credentials;
        every_codepoint(credentials,
                        [&listed](u16 type)
                        {
                            listed.insert(type);
                            return true;
                        });
        bool all = true;
        for (const u16 type : in_use)
        {
            all = all && listed.has(type);
        }
        every_codepoint(credentials,
                        [&listed](u16 type)
                        {
                            listed.erase(type);
                            return true;
                        });
        if (!all)
        {
            return CryptoError::Unsupported;
        }
    }
    return CryptoError::None;
}

CryptoError sign_key_package(const CipherSuite& suite, const Signer& signer, const KeyPackage& package,
                             cont::Span<u8> signature, usize& written) noexcept
{
    written           = 0U;
    const usize bytes = key_package_tbs_bytes(package);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, signature);
    }
    LabelBuffer      tbs(bytes);
    cont::WireWriter writer(tbs.span());
    put_key_package_tbs(writer, package);
    return sign_with_label(suite, signer, kKeyPackageLabel, tbs.span(), signature, written);
}

CryptoError check_key_package(const GroupContext& context, const KeyPackage& package,
                              const SignatureVerifier& verifier, const AuthenticationService& authentication,
                              const LeafPolicy& policy) noexcept
{
    const CipherSuite* suite = cipher_suite(package.cipher_suite);
    if (suite == nullptr || package.version != kProtocolVersion || package.cipher_suite != context.cipher_suite)
    {
        return CryptoError::Unsupported;
    }
    if (!extensions_distinct(package.extensions))
    {
        return CryptoError::Malformed;
    }
    const CryptoError leaf = check_leaf(context, package.leaf_node, LeafRole::KeyPackage, policy, authentication);
    if (leaf != CryptoError::None)
    {
        return leaf;
    }
    // Section 10: the package's extensions too are those its leaf's capabilities list, GREASE included.
    if (!extensions_listed(package.leaf_node.capabilities, package.extensions))
    {
        return CryptoError::Unsupported;
    }
    const usize bytes = key_package_tbs_bytes(package);
    if (bytes == 0U || verify_leaf_node(*suite, verifier, package.leaf_node, {}, 0U) != Verification::Valid)
    {
        return CryptoError::Forged;
    }
    LabelBuffer      tbs(bytes);
    cont::WireWriter writer(tbs.span());
    put_key_package_tbs(writer, package);
    if (verify_with_label(*suite, verifier, package.leaf_node.signature_key, kKeyPackageLabel, tbs.span(),
                          package.signature) != Verification::Valid)
    {
        return CryptoError::Forged;
    }
    if (same_bytes(package.init_key, package.leaf_node.encryption_key))
    {
        return CryptoError::BadKey;
    }
    return check_node_key(*suite, package.init_key);
}

CryptoError key_package_ref(const CipherSuite& suite, const KeyPackage& package, cont::Span<u8> out) noexcept
{
    const usize bytes = key_package_bytes(package);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, out);
    }
    LabelBuffer      encoding(bytes);
    cont::WireWriter writer(encoding.span());
    put_key_package(writer, package);
    return ref_hash(suite, kKeyPackageRef, encoding.span(), out);
}

// ---- GroupInfo and Welcome ----

bool take_group_info(cont::WireReader& in, GroupInfo& out) noexcept
{
    const cont::ConstSpan<u8> start = in.rest();
    out                             = {};
    if (!take_group_context(in, out.context) || !take_extensions(in, out.extensions) ||
        !take_vector(in, out.confirmation_tag) || !in.take_u32(out.signer))
    {
        return false;
    }
    out.signed_fields = taken_since(start, in);
    return take_vector(in, out.signature);
}

usize group_info_bytes(const GroupInfo& info) noexcept
{
    return Size().field(group_info_tbs_bytes(info)).vector(info.signature.size()).bytes();
}

void put_group_info(cont::WireWriter& out, const GroupInfo& info) noexcept
{
    put_group_info_tbs(out, info);
    put_vector(out, info.signature);
}

CryptoError sign_group_info(const CipherSuite& suite, const Signer& signer, const GroupInfo& info,
                            cont::Span<u8> signature, usize& written) noexcept
{
    written           = 0U;
    const usize bytes = group_info_tbs_bytes(info);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, signature);
    }
    LabelBuffer      tbs(bytes);
    cont::WireWriter writer(tbs.span());
    put_group_info_tbs(writer, info);
    return sign_with_label(suite, signer, kGroupInfoLabel, tbs.span(), signature, written);
}

Verification verify_group_info(const CipherSuite& suite, const SignatureVerifier& verifier,
                               cont::ConstSpan<u8> public_key, const GroupInfo& info) noexcept
{
    const usize bytes = group_info_tbs_bytes(info);
    if (bytes == 0U)
    {
        return Verification::Invalid;
    }
    LabelBuffer      tbs(bytes);
    cont::WireWriter writer(tbs.span());
    put_group_info_tbs(writer, info);
    return verify_with_label(suite, verifier, public_key, kGroupInfoLabel, tbs.span(), info.signature);
}

bool take_group_secrets(cont::WireReader& in, GroupSecrets& out) noexcept
{
    out = {};
    return take_vector(in, out.joiner_secret) && take_presence(in, out.has_path_secret) &&
           (!out.has_path_secret || take_vector(in, out.path_secret)) &&
           take_list<PreSharedKeyId>(in, out.psks, take_psk_id);
}

usize group_secrets_bytes(const GroupSecrets& secrets) noexcept
{
    Size size;
    size.vector(secrets.joiner_secret.size()).fixed(1U);
    if (secrets.has_path_secret)
    {
        size.vector(secrets.path_secret.size());
    }
    return size.vector(secrets.psks.size()).bytes();
}

void put_group_secrets(cont::WireWriter& out, const GroupSecrets& secrets) noexcept
{
    put_vector(out, secrets.joiner_secret);
    out.put_u8(secrets.has_path_secret ? 1U : 0U);
    if (secrets.has_path_secret)
    {
        put_vector(out, secrets.path_secret);
    }
    put_vector(out, secrets.psks);
}

bool take_encrypted_group_secrets(cont::WireReader& in, EncryptedGroupSecrets& out) noexcept
{
    out = {};
    return take_vector(in, out.new_member) && take_hpke_ciphertext(in, out.encrypted_group_secrets);
}

bool take_welcome(cont::WireReader& in, Welcome& out) noexcept
{
    out = {};
    return in.take_u16(out.cipher_suite) &&
           take_list<EncryptedGroupSecrets>(in, out.secrets, take_encrypted_group_secrets) &&
           take_vector(in, out.encrypted_group_info);
}

usize welcome_bytes(const Welcome& welcome) noexcept
{
    return Size().fixed(2U).vector(welcome.secrets.size()).vector(welcome.encrypted_group_info.size()).bytes();
}

void put_welcome(cont::WireWriter& out, const Welcome& welcome) noexcept
{
    out.put_u16(welcome.cipher_suite);
    put_vector(out, welcome.secrets);
    put_vector(out, welcome.encrypted_group_info);
}

namespace
{
// The GroupInfo's AEAD key and nonce of a joiner secret and a PSK secret.
struct WelcomeSeal
{
    Secret  key;
    Secret  nonce;
    AeadKey aead;

    [[nodiscard]] CryptoError init(const CipherSuite& suite, cont::ConstSpan<u8> joiner_secret,
                                   cont::ConstSpan<u8> psk_secret) noexcept
    {
        Secret      secret;
        const usize hash  = suite.hash_bytes();
        const usize bytes = aead_key_bytes(suite.aead);
        CryptoError error = welcome_secret(suite, joiner_secret, psk_secret, secret.span(hash));
        if (error == CryptoError::None)
        {
            error = welcome_key(suite, secret.span(hash), key.span(bytes), nonce.span(kAeadNonceBytes));
        }
        return error == CryptoError::None ? aead.init(suite.aead, key.span(bytes)) : error;
    }
};
} // namespace

CryptoError seal_welcome(const CipherSuite& suite, cont::ConstSpan<u8> joiner_secret,
                         cont::ConstSpan<PreSharedKey> psks, cont::ConstSpan<u8> group_info,
                         cont::ConstSpan<WelcomeMember> members, cont::Array<u8>& out) noexcept
{
    out.clear();
    const usize hash = suite.hash_bytes();
    const usize nenc = hpke_sizes(suite.kem).encapsulation;
    usize       ids  = 0U;
    for (const PreSharedKey& psk : psks)
    {
        const usize bytes = psk_id_bytes(psk.id);
        if (bytes == 0U)
        {
            return CryptoError::BadLength;
        }
        ids += bytes;
    }
    if (joiner_secret.size() != hash || hash == 0U || nenc == 0U || vector_bytes(ids) == 0U)
    {
        return CryptoError::BadLength;
    }
    LabelBuffer      id_list(ids);
    cont::WireWriter id_writer(id_list.span());
    for (const PreSharedKey& psk : psks)
    {
        put_psk_id(id_writer, psk.id);
    }
    const auto secrets_of = [&](const WelcomeMember& member)
    { return GroupSecrets{joiner_secret, !member.path_secret.empty(), member.path_secret, id_list.span()}; };
    usize entries = 0U;
    for (const WelcomeMember& member : members)
    {
        if (member.package.cipher_suite != suite.id)
        {
            return CryptoError::Unsupported;
        }
        if (!member.path_secret.empty() && member.path_secret.size() != hash)
        {
            return CryptoError::BadLength;
        }
        const usize sealed_secrets = group_secrets_bytes(secrets_of(member)) + kHpkeTagBytes;
        entries += Size().vector(hash).vector(nenc).vector(sealed_secrets).bytes();
    }
    const usize sealed = group_info.size() + kHpkeTagBytes;
    const usize total  = Size().fixed(2U).vector(entries).vector(sealed).bytes();
    cont::Array<usize> rooms; // per member: where its KEM output and its sealed GroupSecrets go
    if (total == 0U)
    {
        return CryptoError::BadLength;
    }
    if (!out.try_reserve(total) || !rooms.try_reserve(2U * members.size()))
    {
        return CryptoError::ProviderFailure;
    }
    Secret      psk;
    WelcomeSeal seal;
    CryptoError error = psk_secret(suite, psks, psk.span(hash));
    if (error == CryptoError::None)
    {
        error = seal.init(suite, joiner_secret, psk.span(hash));
    }
    // The layout first, each entry's reference written and its KEM output and ciphertext left as room: the
    // GroupSecrets are sealed under the sealed GroupInfo, which comes last.
    out.resize(total);
    const cont::Span<u8> buffer(out.data(), out.size());
    cont::WireWriter     writer(buffer);
    writer.put_u16(suite.id);
    put_varint(writer, entries);
    for (usize i = 0U; error == CryptoError::None && i < members.size(); ++i)
    {
        put_varint(writer, hash);
        error = key_package_ref(suite, members[i].package, reserve(writer, buffer, hash));
        put_varint(writer, nenc);
        rooms.push_back(writer.written());
        reserve(writer, buffer, nenc);
        const usize ciphertext = group_secrets_bytes(secrets_of(members[i])) + kHpkeTagBytes;
        put_varint(writer, ciphertext);
        rooms.push_back(writer.written());
        reserve(writer, buffer, ciphertext);
    }
    if (error == CryptoError::None)
    {
        put_varint(writer, sealed);
        const cont::Span<u8> info = reserve(writer, buffer, sealed);
        error                     = seal.aead.seal(seal.nonce.span(kAeadNonceBytes), group_info, {}, info);
        for (usize i = 0U; error == CryptoError::None && i < members.size(); ++i)
        {
            const GroupSecrets secrets = secrets_of(members[i]);
            const usize        plain   = group_secrets_bytes(secrets);
            LabelBuffer        encoded(plain);
            cont::WireWriter   encoded_writer(encoded.span());
            put_group_secrets(encoded_writer, secrets);
            error = encrypt_with_label(suite, members[i].package.init_key, kWelcomeLabel, info, encoded.span(),
                                       buffer.subspan(rooms[2U * i], nenc),
                                       buffer.subspan(rooms[2U * i + 1U], plain + kHpkeTagBytes));
        }
    }
    if (error != CryptoError::None)
    {
        out.clear();
    }
    return error;
}

// ---- Opening a Welcome ----

OpenedWelcome::~OpenedWelcome() noexcept
{
    wipe();
}

void OpenedWelcome::wipe() noexcept
{
    fresh_buffer(m_secrets_bytes, 0U);
    fresh_buffer(m_info_bytes, 0U);
    secure_zero(m_psk_secret);
    m_secrets    = {};
    m_info       = {};
    m_suite      = nullptr;
    m_resumption = ResumptionUsage::Application;
    m_resumed    = nullptr;
}

namespace
{
// The GroupSecrets' pre-shared keys (section 12.4.3.1) into `psks`, and in `resumed` the usage of the one that resumes
// a prior group (Application for none) and in `resumed_group` that group, `prior` (null for none): Malformed for an
// identity named twice, a nonce not Nh long or a second key that resumes a group; NotReady for a key the member does
// not hold: the store's, or for a reinit or branch key the resumption secret of `prior` at the epoch it names, which
// must be its current one (decision M12).
CryptoError welcome_psks(const CipherSuite& suite, cont::ConstSpan<u8> list, const GroupStore& store,
                         const Group* prior, cont::Array<PreSharedKey>& psks, ResumptionUsage& resumed,
                         const Group*& resumed_group) noexcept
{
    cont::Array<cont::ConstSpan<u8>> encodings;
    cont::WireReader                 in(list);
    resumed       = ResumptionUsage::Application;
    resumed_group = nullptr;
    while (!in.done())
    {
        const cont::ConstSpan<u8> start = in.rest();
        PreSharedKey              psk;
        if (!take_psk_id(in, psk.id))
        {
            return CryptoError::Malformed;
        }
        const CryptoError checked = check_psk_id(suite, psk.id);
        if (checked != CryptoError::None)
        {
            return checked;
        }
        if (!resumes_group(psk.id))
        {
            psk.secret = store.psk(psk.id);
        }
        else if (resumed != ResumptionUsage::Application)
        {
            return CryptoError::Malformed; // section 12.4.3.1: the only such key
        }
        else
        {
            resumed         = psk.id.usage;
            const bool held = prior != nullptr && prior->ready() &&
                              same_bytes(psk.id.group_id, prior->context().group_id) && psk.id.epoch == prior->epoch();
            psk.secret      = held ? prior->secret(EpochSecret::Resumption) : cont::ConstSpan<u8>();
            resumed_group   = held ? prior : nullptr;
        }
        if (psk.secret.empty())
        {
            return CryptoError::NotReady;
        }
        if (!psks.try_reserve(psks.size() + 1U) || !encodings.try_reserve(encodings.size() + 1U))
        {
            return CryptoError::ProviderFailure;
        }
        psks.push_back(psk);
        encodings.push_back(taken_since(start, in));
    }
    return repeats(encodings) ? CryptoError::Malformed : CryptoError::None;
}
} // namespace

CryptoError open_welcome(cont::ConstSpan<u8> welcome, const KeyPackage& own, cont::ConstSpan<u8> init_private,
                         const GroupStore& store, const Group* prior, OpenedWelcome& out) noexcept
{
    out.wipe();
    cont::WireReader in(welcome);
    Welcome          message;
    if (!take_welcome(in, message) || !in.done())
    {
        return CryptoError::Malformed;
    }
    const CipherSuite* suite = cipher_suite(message.cipher_suite);
    if (suite == nullptr || message.cipher_suite != own.cipher_suite)
    {
        return CryptoError::Unsupported;
    }
    const usize hash = suite->hash_bytes();
    Secret      reference;
    CryptoError error = key_package_ref(*suite, own, reference.span(hash));
    if (error != CryptoError::None)
    {
        return error;
    }
    // The first entry for this key package (section 12.4.3.1).
    cont::WireReader      entries(message.secrets);
    EncryptedGroupSecrets mine;
    bool                  found = false;
    while (!found && !entries.done())
    {
        found = take_encrypted_group_secrets(entries, mine) && same_bytes(mine.new_member, reference.span(hash));
    }
    if (!found)
    {
        return CryptoError::BadKey;
    }
    HpkeKey init;
    if (check_private_key(*suite, init_private, own.init_key) != CryptoError::None ||
        init.from_private(suite->kem, init_private) != CryptoError::None)
    {
        return CryptoError::BadKey;
    }
    const cont::ConstSpan<u8> sealed = mine.encrypted_group_secrets.ciphertext;
    if (sealed.size() < kHpkeTagBytes)
    {
        return CryptoError::Forged;
    }
    if (!fresh_buffer(out.m_secrets_bytes, sealed.size() - kHpkeTagBytes))
    {
        return CryptoError::ProviderFailure;
    }
    error = decrypt_with_label(*suite, init, kWelcomeLabel, message.encrypted_group_info,
                               mine.encrypted_group_secrets.kem_output, sealed,
                               cont::Span<u8>(out.m_secrets_bytes.data(), out.m_secrets_bytes.size()));
    cont::WireReader secrets(view(out.m_secrets_bytes));
    if (error == CryptoError::None &&
        (!take_group_secrets(secrets, out.m_secrets) || !secrets.done() ||
         out.m_secrets.joiner_secret.size() != hash ||
         (out.m_secrets.has_path_secret && out.m_secrets.path_secret.size() != hash)))
    {
        error = CryptoError::Malformed;
    }
    cont::Array<PreSharedKey> psks;
    if (error == CryptoError::None)
    {
        error = welcome_psks(*suite, out.m_secrets.psks, store, prior, psks, out.m_resumption, out.m_resumed);
    }
    if (error == CryptoError::None)
    {
        error = psk_secret(*suite, cont::ConstSpan<PreSharedKey>(psks.data(), psks.size()),
                           cont::Span<u8>(out.m_psk_secret, hash));
    }
    // The GroupInfo, under the key and nonce of the joiner secret and the PSKs.
    WelcomeSeal               seal;
    const cont::ConstSpan<u8> info = message.encrypted_group_info;
    if (error == CryptoError::None)
    {
        error = seal.init(*suite, out.m_secrets.joiner_secret, cont::ConstSpan<u8>(out.m_psk_secret, hash));
    }
    if (error == CryptoError::None && info.size() < kAeadTagBytes)
    {
        error = CryptoError::Forged;
    }
    if (error == CryptoError::None && !fresh_buffer(out.m_info_bytes, info.size() - kAeadTagBytes))
    {
        error = CryptoError::ProviderFailure;
    }
    if (error == CryptoError::None)
    {
        error = seal.aead.open(seal.nonce.span(kAeadNonceBytes), info, {},
                               cont::Span<u8>(out.m_info_bytes.data(), out.m_info_bytes.size()));
    }
    cont::WireReader group_info(view(out.m_info_bytes));
    if (error == CryptoError::None &&
        (!take_group_info(group_info, out.m_info) || !group_info.done() ||
         out.m_info.confirmation_tag.size() != hash || !extensions_distinct(out.m_info.extensions) ||
         !extensions_distinct(out.m_info.context.extensions)))
    {
        error = CryptoError::Malformed;
    }
    if (error == CryptoError::None && out.m_info.context.cipher_suite != message.cipher_suite)
    {
        error = CryptoError::Unsupported;
    }
    if (error != CryptoError::None)
    {
        out.wipe();
        return error;
    }
    out.m_suite = suite;
    return CryptoError::None;
}

// ---- A member's group state ----

void GroupEpoch::clear() noexcept
{
    epoch = 0U;
    fresh_buffer(extensions, 0U);
    fresh_buffer(confirmed, 0U);
    fresh_buffer(tag, 0U);
    fresh_buffer(interim, 0U);
    tree        = RatchetTree();
    secret_tree = SecretTree();
    secrets.clear();
}

void GroupEpoch::take(GroupEpoch& other) noexcept
{
    if (&other == this)
    {
        return;
    }
    clear();
    epoch       = other.epoch;
    extensions  = std::move(other.extensions);
    confirmed   = std::move(other.confirmed);
    tag         = std::move(other.tag);
    interim     = std::move(other.interim);
    tree        = std::move(other.tree);
    secret_tree = std::move(other.secret_tree);
    secrets.take(other.secrets);
    other.clear();
}

Group::~Group() noexcept
{
    wipe();
}

void Group::forget_epoch() noexcept
{
    for (Cached& entry : m_cache)
    {
        fresh_buffer(entry.bytes, 0U);
    }
    m_cache.clear();
    m_pending_leaves.clear();
}

void Group::wipe() noexcept
{
    forget_epoch();
    m_suite  = nullptr;
    m_config = {};
    m_group_id.clear();
    m_now.clear();
    m_keys = PathKeys();
    for (Resumption& past : m_resumption)
    {
        secure_zero(past.secret);
    }
    m_resumption.clear();
    m_resumption_next = 0U;
    fresh_buffer(m_reinit, 0U);
}

CryptoError check_suite_policy(const GroupConfig& config, const CipherSuite& suite) noexcept
{
    if (config.crypto_policy == nullptr)
    {
        return CryptoError::None;
    }
    const AlgorithmId algorithms[] = {suite.kem, suite.kdf, suite.aead, suite.signature};
    const AlgorithmStatus status   = config.crypto_policy->snapshot().status(algorithms, config.policy.now,
                                                                             config.policy_scope);
    return status == AlgorithmStatus::Refused ? CryptoError::PolicyRefused : CryptoError::None;
}

void Group::set_now(u64 now) noexcept
{
    m_config.policy.now = std::max(m_config.policy.now, now);
}

CryptoError Group::check_config(const GroupConfig& config) const noexcept
{
    const bool known_scope =
        config.policy_scope == PolicyScope::Everywhere || config.policy_scope == PolicyScope::CeridServices;
    if (config.verifier == nullptr || config.authentication == nullptr || !known_scope ||
        (config.crypto_policy == nullptr && config.policy_scope != PolicyScope::Everywhere))
    {
        return CryptoError::Unsupported;
    }
    return config.resumption_epochs > kMaxResumptionEpochs || config.pending_proposals > kMaxPendingProposals
               ? CryptoError::BadLength
               : CryptoError::None;
}

GroupContext Group::context() const noexcept
{
    if (!ready())
    {
        return {};
    }
    return GroupContext{m_suite->id,           view(m_group_id), m_now.epoch, m_now.tree.root_hash(),
                        view(m_now.confirmed), view(m_now.extensions)};
}

CryptoError Group::own_bytes(cont::ConstSpan<u8> group_id, cont::ConstSpan<u8> extensions,
                             cont::ConstSpan<u8> confirmed) noexcept
{
    CryptoError error = copy_into(group_id, m_group_id);
    if (error == CryptoError::None)
    {
        error = copy_into(extensions, m_now.extensions);
    }
    if (error == CryptoError::None && !m_resumption.try_reserve(m_config.resumption_epochs))
    {
        error = CryptoError::ProviderFailure;
    }
    return error == CryptoError::None ? copy_into(confirmed, m_now.confirmed) : error;
}

CryptoError Group::confirm(GroupEpoch& epoch, cont::ConstSpan<u8> tag) const noexcept
{
    const usize hash = m_suite->hash_bytes();
    if (!confirmation_tag_valid(*m_suite, epoch.secrets.get(EpochSecret::Confirmation), view(epoch.confirmed), tag))
    {
        return CryptoError::Forged;
    }
    if (copy_into(tag, epoch.tag) != CryptoError::None || !fresh_buffer(epoch.interim, hash))
    {
        return CryptoError::ProviderFailure;
    }
    return mls::interim_transcript_hash(*m_suite, view(epoch.confirmed), tag,
                                        cont::Span<u8>(epoch.interim.data(), hash));
}

CryptoError Group::create(const CipherSuite& suite, cont::ConstSpan<u8> group_id, cont::ConstSpan<u8> extensions,
                          const LeafNode& creator, cont::ConstSpan<u8> leaf_private,
                          const GroupConfig& config) noexcept
{
    wipe();
    const CipherSuite* profile = cipher_suite(suite.id);
    CryptoError        error   = profile == nullptr ? CryptoError::Unsupported : check_config(config);
    if (error == CryptoError::None)
    {
        error = check_suite_policy(config, *profile);
    }
    if (error != CryptoError::None)
    {
        return error;
    }
    if (!extensions_distinct(extensions) || vector_bytes(group_id.size()) == 0U)
    {
        return CryptoError::Malformed;
    }
    m_config                   = config;
    const GroupContext initial{profile->id, group_id, 0U, {}, {}, extensions};
    error = check_leaf(initial, creator, LeafRole::KeyPackage, config.policy, *config.authentication);
    if (error == CryptoError::None &&
        verify_leaf_node(*profile, *config.verifier, creator, {}, 0U) != Verification::Valid)
    {
        error = CryptoError::Forged;
    }
    if (error == CryptoError::None)
    {
        error = check_private_key(*profile, leaf_private, creator.encryption_key);
    }
    if (error == CryptoError::None)
    {
        error = m_now.tree.create(*profile, creator);
    }
    if (error == CryptoError::None)
    {
        error = m_keys.init(*profile, 0U, leaf_private);
    }
    if (error == CryptoError::None)
    {
        error = own_bytes(group_id, extensions, {});
    }
    // Epoch 0 from a fresh random init secret, with no commit or PSK secret (section 11).
    const usize hash                = profile->hash_bytes();
    const u8    zero[kMaxHashBytes] = {};
    Secret      init;
    Secret      tag;
    if (error == CryptoError::None)
    {
        m_suite = profile;
        error   = random_bytes(init.span(hash));
    }
    if (error == CryptoError::None)
    {
        const GroupContext context = this->context();
        LabelBuffer        encoded(group_context_bytes(context));
        cont::WireWriter   writer(encoded.span());
        put_group_context(writer, context);
        error = m_now.secrets.advance(*profile, init.span(hash), cont::ConstSpan<u8>(zero, hash),
                                      cont::ConstSpan<u8>(zero, hash), encoded.span());
    }
    if (error == CryptoError::None)
    {
        error = confirmation_tag(*profile, m_now.secrets.get(EpochSecret::Confirmation), {}, tag.span(hash));
    }
    if (error == CryptoError::None)
    {
        error = confirm(m_now, tag.span(hash));
    }
    if (error == CryptoError::None)
    {
        error = m_now.secret_tree.init(*profile, m_now.secrets.get(EpochSecret::Encryption), 1U, config.window);
    }
    if (error != CryptoError::None)
    {
        wipe();
    }
    return error;
}

namespace
{
// Section 7.3 over every member of a tree a member joins and section 12.1.8.1's external senders: each leaf's
// check_leaf (role Tree), each parent's key usable, the credential types, and every external sender's credential.
CryptoError check_members(const RatchetTree& tree, const GroupContext& context, const GroupConfig& config) noexcept
{
    CryptoError error = CryptoError::None;
    for (u32 node = 0U; error == CryptoError::None && node < tree.node_count(); ++node)
    {
        LeafNode leaf{};
        if (tree::level(node) == 0U)
        {
            error = tree.leaf_node(node / 2U, leaf)
                        ? check_leaf(context, leaf, LeafRole::Tree, config.policy, *config.authentication)
                        : CryptoError::None;
        }
        else if (!tree.blank(node))
        {
            error = check_node_key(tree.suite(), tree.encryption_key(node));
        }
    }
    if (error == CryptoError::None)
    {
        error = check_credential_types(tree);
    }
    cont::ConstSpan<u8> data;
    cont::ConstSpan<u8> senders;
    if (error != CryptoError::None ||
        !find_extension(context.extensions, static_cast<u16>(ExtensionType::ExternalSenders), data))
    {
        return error;
    }
    if (!take_external_senders(data, senders))
    {
        return CryptoError::Malformed;
    }
    cont::WireReader in(senders);
    while (!in.done())
    {
        ExternalSender sender;
        if (!take_external_sender(in, sender) ||
            !config.authentication->validate(sender.credential, sender.signature_key))
        {
            return CryptoError::Forged;
        }
    }
    return CryptoError::None;
}

// The leaf of `tree` identical to `own` (section 12.4.3.1): found by its encryption key, which a validated tree holds
// once, then compared whole.
bool find_own_leaf(const RatchetTree& tree, const LeafNode& own, u32& index) noexcept
{
    const usize bytes = leaf_node_bytes(own);
    if (bytes == 0U)
    {
        return false;
    }
    LabelBuffer      encoded(bytes);
    cont::WireWriter writer(encoded.span());
    put_leaf_node(writer, own);
    for (u32 leaf = 0U; leaf < tree.leaves(); ++leaf)
    {
        LeafNode node{};
        if (tree.leaf_node(leaf, node) && same_bytes(node.encryption_key, own.encryption_key))
        {
            index = leaf;
            return node.signed_fields.size() + vector_bytes(node.signature.size()) == bytes &&
                   same_bytes(node.signed_fields, encoded.span().first(node.signed_fields.size())) &&
                   same_bytes(node.signature, encoded.span().last(node.signature.size()));
        }
    }
    return false;
}
} // namespace

CryptoError Group::take_tree(const CipherSuite& suite, const GroupInfo& info, cont::ConstSpan<u8> ratchet_tree) noexcept
{
    const GroupContext& context    = info.context;
    cont::ConstSpan<u8> tree_bytes = ratchet_tree;
    if (tree_bytes.empty() &&
        !find_extension(info.extensions, static_cast<u16>(ExtensionType::RatchetTree), tree_bytes))
    {
        return CryptoError::NotReady;
    }
    CryptoError error = m_now.tree.decode(suite, tree_bytes);
    LeafNode    signer{};
    if (error == CryptoError::None &&
        (!same_bytes(m_now.tree.root_hash(), context.tree_hash) || !m_now.tree.leaf_node(info.signer, signer) ||
         verify_group_info(suite, *m_config.verifier, signer.signature_key, info) != Verification::Valid))
    {
        error = CryptoError::Forged;
    }
    if (error == CryptoError::None)
    {
        error = m_now.tree.validate(*m_config.verifier, context.group_id);
    }
    return error == CryptoError::None ? check_members(m_now.tree, context, m_config) : error;
}

CryptoError Group::join(cont::ConstSpan<u8> welcome, const KeyPackage& own, cont::ConstSpan<u8> init_private,
                        cont::ConstSpan<u8> leaf_private, cont::ConstSpan<u8> ratchet_tree, const GroupConfig& config,
                        const Group* prior) noexcept
{
    wipe();
    if (prior == this)
    {
        return CryptoError::Malformed;
    }
    CryptoError error = config.store == nullptr ? CryptoError::Unsupported : check_config(config);
    if (error != CryptoError::None)
    {
        return error;
    }
    OpenedWelcome opened;
    error = open_welcome(welcome, own, init_private, *config.store, prior, opened);
    if (error == CryptoError::None)
    {
        error = check_suite_policy(config, opened.suite());
    }
    if (error != CryptoError::None)
    {
        return error;
    }
    m_config = config;
    const CipherSuite&    suite   = opened.suite();
    const GroupInfo&      info    = opened.group_info();
    const GroupContext&   context = info.context;
    const ResumptionUsage usage   = opened.resumption();
    const Group*          resumed = opened.resumed(); // `prior`, when it held the reinit or branch key
    // A reinitialization may keep the id of the group it ends, in which the member takes no further part.
    const bool same_group = resumed != nullptr && usage == ResumptionUsage::Reinit &&
                            same_bytes(context.group_id, resumed->context().group_id);
    if (!same_group && config.store->member_of(context.group_id))
    {
        return CryptoError::Replayed;
    }
    error = take_tree(suite, info, ratchet_tree);
    // The member's own leaf and keys, then the path secret's (section 12.4.3.1).
    u32 leaf = 0U;
    if (error == CryptoError::None && !find_own_leaf(m_now.tree, own.leaf_node, leaf))
    {
        error = CryptoError::BadKey;
    }
    if (error == CryptoError::None)
    {
        error = check_private_key(suite, leaf_private, own.leaf_node.encryption_key);
    }
    if (error == CryptoError::None)
    {
        error = m_keys.init(suite, leaf, leaf_private);
    }
    if (error == CryptoError::None && opened.secrets().has_path_secret)
    {
        const std::optional<u32> ancestor = tree::common_ancestor(2U * info.signer, 2U * leaf, m_now.tree.leaves());
        error = ancestor && info.signer != leaf ? m_keys.implant(m_now.tree, *ancestor, opened.secrets().path_secret)
                                                : CryptoError::Malformed;
    }
    if (error == CryptoError::None)
    {
        error = own_bytes(context.group_id, context.extensions, context.confirmed_transcript_hash);
    }
    if (error == CryptoError::None)
    {
        m_suite = &suite;
        m_now.epoch = context.epoch;
        LabelBuffer      encoded(group_context_bytes(context));
        cont::WireWriter writer(encoded.span());
        put_group_context(writer, context);
        error = m_now.secrets.join(suite, opened.secrets().joiner_secret, opened.psk_secret(), encoded.span());
    }
    if (error == CryptoError::None)
    {
        error = confirm(m_now, info.confirmation_tag);
    }
    if (error == CryptoError::None && resumed != nullptr)
    {
        error = check_resumed(*resumed, usage);
    }
    if (error == CryptoError::None)
    {
        error = m_now.secret_tree.init(suite, m_now.secrets.get(EpochSecret::Encryption), m_now.tree.leaves(),
                                       config.window);
    }
    if (error != CryptoError::None)
    {
        wipe();
    }
    return error;
}

// ---- Resumed groups (sections 11.2, 11.3 and 12.4.3.1; decision M12) ----

bool Group::reinit_proposal(ReInit& out) const noexcept
{
    out = {};
    cont::WireReader in(view(m_reinit));
    Proposal         proposal;
    if (!ended() || !take_proposal(in, proposal))
    {
        return false;
    }
    out = proposal.reinit;
    return true;
}

CryptoError Group::check_resumed(const Group& prior, ResumptionUsage usage) const noexcept
{
    if (m_now.epoch != 1U)
    {
        return CryptoError::Malformed; // the resumed group's first epoch
    }
    const bool reinit = usage == ResumptionUsage::Reinit;
    if (reinit)
    {
        // Section 11.2: the group the prior group's last commit, a ReInit's, names.
        ReInit params;
        if (!prior.reinit_proposal(params))
        {
            return CryptoError::NotReady;
        }
        if (params.version != kProtocolVersion || params.cipher_suite != m_suite->id ||
            !same_bytes(params.group_id, view(m_group_id)) || !same_bytes(params.extensions, view(m_now.extensions)))
        {
            return CryptoError::Malformed;
        }
    }
    else if (prior.suite().id != m_suite->id)
    {
        return CryptoError::Malformed; // section 11.3: the prior group's version and cipher suite
    }
    // Each member one of the prior group's, judged by the Authentication Service (the same member as a distinct leaf
    // of the prior group's tree, whose identifiers it accepts as successors); for a reinitialization, all of them.
    const RatchetTree& old = prior.tree();
    cont::Array<u8>    matched;
    if (!matched.try_reserve(old.leaves()))
    {
        return CryptoError::ProviderFailure;
    }
    matched.resize(old.leaves(), 0U);
    for (u32 leaf = 0U; leaf < m_now.tree.leaves(); ++leaf)
    {
        LeafNode member{};
        if (!m_now.tree.leaf_node(leaf, member))
        {
            continue;
        }
        bool found = false;
        for (u32 i = 0U; !found && i < old.leaves(); ++i)
        {
            LeafNode before{};
            found = matched[i] == 0U && old.leaf_node(i, before) &&
                    m_config.authentication->valid_successor(before.credential, member.credential);
            matched[i] = found ? 1U : matched[i];
        }
        if (!found)
        {
            return CryptoError::Forged;
        }
    }
    for (u32 i = 0U; reinit && i < old.leaves(); ++i)
    {
        if (matched[i] == 0U && !old.blank(2U * i))
        {
            return CryptoError::Forged;
        }
    }
    return CryptoError::None;
}
} // namespace crd::crypto::mls
