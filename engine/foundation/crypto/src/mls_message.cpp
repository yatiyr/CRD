// crd-crypto -- MLS messages, RFC 9420: the structures a handshake message carries, framing, PublicMessage and
// PrivateMessage protection and the transcript hashes (NET.2; contract: docs/design/network-foundation.md#mls).
#include <crd/crypto/mls_message.hpp>

#include "byte_spans.hpp"
#include "mls_detail.hpp"

#include <bit>

namespace crd::crypto::mls
{
namespace
{
using detail::LabelBuffer;
using detail::refuse;

constexpr cont::StringView kContentLabel = "FramedContentTBS";

// The total of a structure's fields; a field that cannot be encoded (0 bytes) makes it 0.
class Size
{
public:
    Size& fixed(usize bytes) noexcept
    {
        m_bytes += bytes;
        return *this;
    }
    // A field sized by its own *_bytes or vector_bytes.
    Size& field(usize bytes) noexcept
    {
        m_ok = m_ok && bytes != 0U;
        m_bytes += bytes;
        return *this;
    }
    Size& vector(usize length) noexcept { return field(vector_bytes(length)); }

    [[nodiscard]] usize bytes() const noexcept { return m_ok ? m_bytes : 0U; }

private:
    usize m_bytes = 0U;
    bool  m_ok    = true;
};

// The bytes a decoder took since `start` (the reader's rest() before it began), as a view of the input.
cont::ConstSpan<u8> taken_since(cont::ConstSpan<u8> start, const cont::WireReader& in) noexcept
{
    return start.first(start.size() - in.rest().size());
}

// A list whose body is a sequence of `take` structures.
template <typename T, typename Take>
bool take_list(cont::WireReader& in, cont::ConstSpan<u8>& body, Take take) noexcept
{
    if (!take_vector(in, body))
    {
        return false;
    }
    cont::WireReader items(body);
    while (!items.done())
    {
        T item{};
        if (!take(items, item))
        {
            return false;
        }
    }
    return true;
}

// A list of 16-bit codepoints.
bool take_u16_list(cont::WireReader& in, cont::ConstSpan<u8>& body) noexcept
{
    return take_vector(in, body) && body.size() % 2U == 0U;
}

bool take_opaque(cont::WireReader& in, cont::ConstSpan<u8>& out) noexcept
{
    return take_vector(in, out);
}

bool take_extension(cont::WireReader& in, cont::ConstSpan<u8>& data) noexcept
{
    u16 type = 0U;
    return in.take_u16(type) && take_vector(in, data);
}

// ---- Credentials and capabilities ----

bool take_credential(cont::WireReader& in, Credential& out) noexcept
{
    if (!in.take_u16(out.type) || out.type == 0U)
    {
        return false;
    }
    if (out.type == static_cast<u16>(CredentialType::X509))
    {
        return take_list<cont::ConstSpan<u8>>(in, out.content, take_opaque);
    }
    return take_vector(in, out.content);
}

usize credential_bytes(const Credential& credential) noexcept
{
    return credential.type == 0U ? 0U : Size().fixed(2U).vector(credential.content.size()).bytes();
}

void put_credential(cont::WireWriter& out, const Credential& credential) noexcept
{
    out.put_u16(credential.type);
    put_vector(out, credential.content);
}

bool take_capabilities(cont::WireReader& in, Capabilities& out) noexcept
{
    return take_u16_list(in, out.versions) && take_u16_list(in, out.cipher_suites) &&
           take_u16_list(in, out.extensions) && take_u16_list(in, out.proposals) &&
           take_u16_list(in, out.credentials);
}

usize capabilities_bytes(const Capabilities& caps) noexcept
{
    return Size()
        .vector(caps.versions.size())
        .vector(caps.cipher_suites.size())
        .vector(caps.extensions.size())
        .vector(caps.proposals.size())
        .vector(caps.credentials.size())
        .bytes();
}

void put_capabilities(cont::WireWriter& out, const Capabilities& caps) noexcept
{
    put_vector(out, caps.versions);
    put_vector(out, caps.cipher_suites);
    put_vector(out, caps.extensions);
    put_vector(out, caps.proposals);
    put_vector(out, caps.credentials);
}

// ---- Framing helpers ----

bool known_content_type(u8 type) noexcept
{
    return type >= static_cast<u8>(ContentType::Application) && type <= static_cast<u8>(ContentType::Commit);
}

bool known_wire_format(u16 format) noexcept
{
    return format >= static_cast<u16>(WireFormat::PublicMessage) && format <= static_cast<u16>(WireFormat::KeyPackage);
}

bool indexed(SenderType type) noexcept
{
    return type == SenderType::Member || type == SenderType::External;
}

// FramedContentTBS carries the GroupContext for a member and a new member committing (section 6.1).
bool signs_context(SenderType type) noexcept
{
    return type == SenderType::Member || type == SenderType::NewMemberCommit;
}

bool take_sender(cont::WireReader& in, Sender& out) noexcept
{
    u8 type = 0U;
    if (!in.take_u8(type) || type < static_cast<u8>(SenderType::Member) ||
        type > static_cast<u8>(SenderType::NewMemberCommit))
    {
        return false;
    }
    out.type  = static_cast<SenderType>(type);
    out.index = 0U;
    return !indexed(out.type) || in.take_u32(out.index);
}

usize sender_bytes(const Sender& sender) noexcept
{
    switch (sender.type)
    {
    case SenderType::Member:
    case SenderType::External:
        return 5U;
    case SenderType::NewMemberProposal:
    case SenderType::NewMemberCommit:
        return 1U;
    }
    return 0U;
}

void put_sender(cont::WireWriter& out, const Sender& sender) noexcept
{
    out.put_u8(static_cast<u8>(sender.type));
    if (indexed(sender.type))
    {
        out.put_u32(sender.index);
    }
}

// The body of a content of `type`: an application message's data vector, or one Proposal or Commit, as a view.
bool take_body(cont::WireReader& in, ContentType type, cont::ConstSpan<u8>& body) noexcept
{
    if (type == ContentType::Application)
    {
        return take_vector(in, body);
    }
    const cont::ConstSpan<u8> start = in.rest();
    bool                      ok    = false;
    if (type == ContentType::Proposal)
    {
        Proposal proposal;
        ok = take_proposal(in, proposal);
    }
    else
    {
        Commit commit;
        ok = take_commit(in, commit);
    }
    body = taken_since(start, in);
    return ok;
}

usize body_bytes(const FramedContent& content) noexcept
{
    if (content.content_type == ContentType::Application)
    {
        return vector_bytes(content.body.size());
    }
    return known_content_type(static_cast<u8>(content.content_type)) && !content.body.empty() ? content.body.size()
                                                                                                 : 0U;
}

void put_body(cont::WireWriter& out, const FramedContent& content) noexcept
{
    if (content.content_type == ContentType::Application)
    {
        put_vector(out, content.body);
    }
    else
    {
        out.put_bytes(content.body);
    }
}

bool take_auth(cont::WireReader& in, ContentType type, ContentAuth& out) noexcept
{
    out = {};
    return take_vector(in, out.signature) && (type != ContentType::Commit || take_vector(in, out.confirmation_tag));
}

usize auth_bytes(ContentType type, const ContentAuth& auth) noexcept
{
    Size size;
    size.vector(auth.signature.size());
    if (type == ContentType::Commit)
    {
        size.vector(auth.confirmation_tag.size());
    }
    return size.bytes();
}

void put_auth(cont::WireWriter& out, ContentType type, const ContentAuth& auth) noexcept
{
    put_vector(out, auth.signature);
    if (type == ContentType::Commit)
    {
        put_vector(out, auth.confirmation_tag);
    }
}

// A commit's authentication carries a tag of Nh bytes and only a commit's carries one.
bool auth_fits(const CipherSuite& suite, ContentType type, const ContentAuth& auth) noexcept
{
    return type == ContentType::Commit ? auth.confirmation_tag.size() == suite.hash_bytes()
                                       : auth.confirmation_tag.empty();
}

// FramedContentTBS: the version, the wire format, the content and, for the senders that sign it, the GroupContext.
usize content_tbs_bytes(const FramedContent& content, const GroupContext& context) noexcept
{
    Size size;
    size.fixed(4U).field(framed_content_bytes(content));
    if (signs_context(content.sender.type))
    {
        size.field(group_context_bytes(context));
    }
    return size.bytes();
}

void put_content_tbs(cont::WireWriter& out, WireFormat wire_format, const FramedContent& content,
                     const GroupContext& context) noexcept
{
    out.put_u16(kProtocolVersion);
    out.put_u16(static_cast<u16>(wire_format));
    put_framed_content(out, content);
    if (signs_context(content.sender.type))
    {
        put_group_context(out, context);
    }
}

// The SignContent of a FramedContentTBS, laid out once.
usize sign_content_bytes(const FramedContent& content, const GroupContext& context, usize& tbs) noexcept
{
    tbs               = content_tbs_bytes(content, context);
    const usize head  = tbs == 0U ? 0U : labelled_head_bytes(kContentLabel, tbs);
    return head == 0U ? 0U : head + tbs;
}

void put_sign_content(cont::WireWriter& out, WireFormat wire_format, const FramedContent& content,
                      const GroupContext& context, usize tbs) noexcept
{
    put_labelled_head(out, kContentLabel, tbs);
    put_content_tbs(out, wire_format, content, context);
}

// ---- PrivateMessage ----

usize private_struct_bytes(const PrivateMessage& message) noexcept
{
    return Size()
        .vector(message.group_id.size())
        .fixed(9U)
        .vector(message.authenticated_data.size())
        .vector(message.encrypted_sender_data.size())
        .vector(message.ciphertext.size())
        .bytes();
}

void put_private_header(cont::WireWriter& out, cont::ConstSpan<u8> group_id, u64 epoch, ContentType type,
                        cont::ConstSpan<u8> authenticated_data) noexcept
{
    put_vector(out, group_id);
    out.put_u64(epoch);
    out.put_u8(static_cast<u8>(type));
    put_vector(out, authenticated_data);
}

bool take_private(cont::WireReader& in, PrivateMessage& out) noexcept
{
    const cont::ConstSpan<u8> start = in.rest();
    u8                        type  = 0U;
    if (!take_vector(in, out.group_id) || !in.take_u64(out.epoch) || !in.take_u8(type) || !known_content_type(type))
    {
        return false;
    }
    out.content_type          = static_cast<ContentType>(type);
    out.sender_data_aad_bytes = taken_since(start, in).size();
    if (!take_vector(in, out.authenticated_data))
    {
        return false;
    }
    out.header = taken_since(start, in);
    return take_vector(in, out.encrypted_sender_data) && take_vector(in, out.ciphertext);
}

usize public_struct_bytes(const PublicMessage& message) noexcept
{
    Size size;
    size.field(framed_content_bytes(message.content)).field(auth_bytes(message.content.content_type, message.auth));
    if (message.content.sender.type == SenderType::Member)
    {
        size.vector(message.membership_tag.size());
    }
    return size.bytes();
}

RatchetKind ratchet_of(ContentType type) noexcept
{
    return type == ContentType::Application ? RatchetKind::Application : RatchetKind::Handshake;
}

// The content's nonce: the key's with the reuse guard XORed into its first four bytes (section 6.3.1).
void guard_nonce(const MessageKey& key, cont::ConstSpan<u8> guard, u8 (&nonce)[kAeadNonceBytes]) noexcept
{
    for (usize i = 0U; i < kAeadNonceBytes; ++i)
    {
        nonce[i] = key.nonce[i];
    }
    for (usize i = 0U; i < kReuseGuardBytes; ++i)
    {
        nonce[i] ^= guard[i];
    }
}

// An AEAD key and nonce on the stack, zeroed when they go out of scope.
struct SealKey
{
    u8      key[kMaxAeadKeyBytes]  = {};
    u8      nonce[kAeadNonceBytes] = {};
    AeadKey aead;

    SealKey() noexcept = default;
    ~SealKey() noexcept
    {
        secure_zero(key);
        secure_zero(nonce);
    }
    SealKey(const SealKey&)            = delete;
    SealKey& operator=(const SealKey&) = delete;
    SealKey(SealKey&&)                 = delete;
    SealKey& operator=(SealKey&&)      = delete;

    // The sender data key and nonce `sender_data_secret` derives from the ciphertext's sample.
    [[nodiscard]] CryptoError sender_data(const CipherSuite& suite, cont::ConstSpan<u8> sender_data_secret,
                                          cont::ConstSpan<u8> ciphertext) noexcept
    {
        const usize       bytes = aead_key_bytes(suite.aead);
        const CryptoError error =
            sender_data_key(suite, sender_data_secret, ciphertext, cont::Span<u8>(key, bytes), nonce);
        return error == CryptoError::None ? aead.init(suite.aead, cont::ConstSpan<u8>(key, bytes)) : error;
    }
};

bool all_zero(cont::ConstSpan<u8> bytes) noexcept
{
    u8 any = 0U;
    for (const u8 byte : bytes)
    {
        any = static_cast<u8>(any | byte);
    }
    return any == 0U;
}
} // namespace

// ---- The structures a message carries ----

bool take_extensions(cont::WireReader& in, cont::ConstSpan<u8>& body) noexcept
{
    return take_list<cont::ConstSpan<u8>>(in, body, take_extension);
}

bool take_leaf_node(cont::WireReader& in, LeafNode& out) noexcept
{
    const cont::ConstSpan<u8> start  = in.rest();
    u8                        source = 0U;
    out                              = {};
    if (!take_vector(in, out.encryption_key) || !take_vector(in, out.signature_key) ||
        !take_credential(in, out.credential) || !take_capabilities(in, out.capabilities) || !in.take_u8(source))
    {
        return false;
    }
    switch (source)
    {
    case static_cast<u8>(LeafNodeSource::KeyPackage):
        if (!in.take_u64(out.not_before) || !in.take_u64(out.not_after))
        {
            return false;
        }
        break;
    case static_cast<u8>(LeafNodeSource::Update):
        break;
    case static_cast<u8>(LeafNodeSource::Commit):
        if (!take_vector(in, out.parent_hash))
        {
            return false;
        }
        break;
    default:
        return false;
    }
    out.source = static_cast<LeafNodeSource>(source);
    if (!take_extensions(in, out.extensions))
    {
        return false;
    }
    out.signed_fields = taken_since(start, in);
    return take_vector(in, out.signature);
}

usize leaf_node_bytes(const LeafNode& node) noexcept
{
    return Size().field(leaf_fields_bytes(node)).vector(node.signature.size()).bytes();
}

void put_leaf_node(cont::WireWriter& out, const LeafNode& node) noexcept
{
    put_leaf_fields(out, node);
    put_vector(out, node.signature);
}

usize leaf_fields_bytes(const LeafNode& node) noexcept
{
    Size size;
    size.vector(node.encryption_key.size())
        .vector(node.signature_key.size())
        .field(credential_bytes(node.credential))
        .field(capabilities_bytes(node.capabilities))
        .fixed(1U);
    switch (node.source)
    {
    case LeafNodeSource::KeyPackage:
        size.fixed(16U);
        break;
    case LeafNodeSource::Update:
        break;
    case LeafNodeSource::Commit:
        size.vector(node.parent_hash.size());
        break;
    default:
        return 0U;
    }
    return size.vector(node.extensions.size()).bytes();
}

void put_leaf_fields(cont::WireWriter& out, const LeafNode& node) noexcept
{
    put_vector(out, node.encryption_key);
    put_vector(out, node.signature_key);
    put_credential(out, node.credential);
    put_capabilities(out, node.capabilities);
    out.put_u8(static_cast<u8>(node.source));
    if (node.source == LeafNodeSource::KeyPackage)
    {
        out.put_u64(node.not_before);
        out.put_u64(node.not_after);
    }
    else if (node.source == LeafNodeSource::Commit)
    {
        put_vector(out, node.parent_hash);
    }
    put_vector(out, node.extensions);
}

bool take_key_package(cont::WireReader& in, KeyPackage& out) noexcept
{
    const cont::ConstSpan<u8> start = in.rest();
    out                             = {};
    if (!in.take_u16(out.version) || !in.take_u16(out.cipher_suite) || !take_vector(in, out.init_key) ||
        !take_leaf_node(in, out.leaf_node) || !take_extensions(in, out.extensions))
    {
        return false;
    }
    out.signed_fields = taken_since(start, in);
    return take_vector(in, out.signature);
}

usize key_package_bytes(const KeyPackage& package) noexcept
{
    return Size()
        .fixed(4U)
        .vector(package.init_key.size())
        .field(leaf_node_bytes(package.leaf_node))
        .vector(package.extensions.size())
        .vector(package.signature.size())
        .bytes();
}

void put_key_package(cont::WireWriter& out, const KeyPackage& package) noexcept
{
    out.put_u16(package.version);
    out.put_u16(package.cipher_suite);
    put_vector(out, package.init_key);
    put_leaf_node(out, package.leaf_node);
    put_vector(out, package.extensions);
    put_vector(out, package.signature);
}

bool take_proposal(cont::WireReader& in, Proposal& out) noexcept
{
    out = {};
    if (!in.take_u16(out.type) || out.type == 0U)
    {
        return false;
    }
    switch (out.type)
    {
    case static_cast<u16>(ProposalType::Add):
        return take_key_package(in, out.add);
    case static_cast<u16>(ProposalType::Update):
        return take_leaf_node(in, out.update);
    case static_cast<u16>(ProposalType::Remove):
        return in.take_u32(out.remove);
    case static_cast<u16>(ProposalType::PreSharedKey):
        return take_psk_id(in, out.psk);
    case static_cast<u16>(ProposalType::ReInit):
        return take_vector(in, out.reinit.group_id) && in.take_u16(out.reinit.version) &&
               in.take_u16(out.reinit.cipher_suite) && take_extensions(in, out.reinit.extensions);
    case static_cast<u16>(ProposalType::ExternalInit):
        return take_vector(in, out.kem_output);
    case static_cast<u16>(ProposalType::GroupContextExtensions):
        return take_extensions(in, out.extensions);
    default:
        return take_vector(in, out.custom);
    }
}

usize proposal_bytes(const Proposal& proposal) noexcept
{
    Size size;
    size.fixed(2U);
    switch (proposal.type)
    {
    case 0U:
        return 0U;
    case static_cast<u16>(ProposalType::Add):
        size.field(key_package_bytes(proposal.add));
        break;
    case static_cast<u16>(ProposalType::Update):
        size.field(leaf_node_bytes(proposal.update));
        break;
    case static_cast<u16>(ProposalType::Remove):
        size.fixed(4U);
        break;
    case static_cast<u16>(ProposalType::PreSharedKey):
        size.field(psk_id_bytes(proposal.psk));
        break;
    case static_cast<u16>(ProposalType::ReInit):
        size.vector(proposal.reinit.group_id.size()).fixed(4U).vector(proposal.reinit.extensions.size());
        break;
    case static_cast<u16>(ProposalType::ExternalInit):
        size.vector(proposal.kem_output.size());
        break;
    case static_cast<u16>(ProposalType::GroupContextExtensions):
        size.vector(proposal.extensions.size());
        break;
    default:
        size.vector(proposal.custom.size());
        break;
    }
    return size.bytes();
}

void put_proposal(cont::WireWriter& out, const Proposal& proposal) noexcept
{
    out.put_u16(proposal.type);
    switch (proposal.type)
    {
    case static_cast<u16>(ProposalType::Add):
        put_key_package(out, proposal.add);
        break;
    case static_cast<u16>(ProposalType::Update):
        put_leaf_node(out, proposal.update);
        break;
    case static_cast<u16>(ProposalType::Remove):
        out.put_u32(proposal.remove);
        break;
    case static_cast<u16>(ProposalType::PreSharedKey):
        put_psk_id(out, proposal.psk);
        break;
    case static_cast<u16>(ProposalType::ReInit):
        put_vector(out, proposal.reinit.group_id);
        out.put_u16(proposal.reinit.version);
        out.put_u16(proposal.reinit.cipher_suite);
        put_vector(out, proposal.reinit.extensions);
        break;
    case static_cast<u16>(ProposalType::ExternalInit):
        put_vector(out, proposal.kem_output);
        break;
    case static_cast<u16>(ProposalType::GroupContextExtensions):
        put_vector(out, proposal.extensions);
        break;
    default:
        put_vector(out, proposal.custom);
        break;
    }
}

bool take_proposal_or_ref(cont::WireReader& in, ProposalOrRef& out) noexcept
{
    u8 type = 0U;
    out     = {};
    if (!in.take_u8(type))
    {
        return false;
    }
    if (type == 1U)
    {
        return take_proposal(in, out.proposal);
    }
    out.is_reference = true;
    return type == 2U && take_vector(in, out.reference);
}

usize proposal_or_ref_bytes(const ProposalOrRef& entry) noexcept
{
    return Size()
        .fixed(1U)
        .field(entry.is_reference ? vector_bytes(entry.reference.size()) : proposal_bytes(entry.proposal))
        .bytes();
}

void put_proposal_or_ref(cont::WireWriter& out, const ProposalOrRef& entry) noexcept
{
    out.put_u8(entry.is_reference ? 2U : 1U);
    if (entry.is_reference)
    {
        put_vector(out, entry.reference);
    }
    else
    {
        put_proposal(out, entry.proposal);
    }
}

bool take_hpke_ciphertext(cont::WireReader& in, HpkeCiphertext& out) noexcept
{
    return take_vector(in, out.kem_output) && take_vector(in, out.ciphertext);
}

bool take_update_path_node(cont::WireReader& in, UpdatePathNode& out) noexcept
{
    return take_vector(in, out.encryption_key) &&
           take_list<HpkeCiphertext>(in, out.encrypted_path_secrets, take_hpke_ciphertext);
}

bool take_update_path(cont::WireReader& in, UpdatePath& out) noexcept
{
    out = {};
    return take_leaf_node(in, out.leaf_node) && take_list<UpdatePathNode>(in, out.nodes, take_update_path_node);
}

usize update_path_bytes(const UpdatePath& path) noexcept
{
    return Size().field(leaf_node_bytes(path.leaf_node)).vector(path.nodes.size()).bytes();
}

void put_update_path(cont::WireWriter& out, const UpdatePath& path) noexcept
{
    put_leaf_node(out, path.leaf_node);
    put_vector(out, path.nodes);
}

bool take_commit(cont::WireReader& in, Commit& out) noexcept
{
    out = {};
    return take_list<ProposalOrRef>(in, out.proposals, take_proposal_or_ref) && take_presence(in, out.has_path) &&
           (!out.has_path || take_update_path(in, out.path));
}

usize commit_bytes(const Commit& commit) noexcept
{
    Size size;
    size.vector(commit.proposals.size()).fixed(1U);
    if (commit.has_path)
    {
        size.field(update_path_bytes(commit.path));
    }
    return size.bytes();
}

void put_commit(cont::WireWriter& out, const Commit& commit) noexcept
{
    put_vector(out, commit.proposals);
    out.put_u8(commit.has_path ? 1U : 0U);
    if (commit.has_path)
    {
        put_update_path(out, commit.path);
    }
}

// ---- Framing ----

bool take_framed_content(cont::WireReader& in, FramedContent& out) noexcept
{
    u8 type = 0U;
    out     = {};
    if (!take_vector(in, out.group_id) || !in.take_u64(out.epoch) || !take_sender(in, out.sender) ||
        !take_vector(in, out.authenticated_data) || !in.take_u8(type) || !known_content_type(type))
    {
        return false;
    }
    out.content_type = static_cast<ContentType>(type);
    return take_body(in, out.content_type, out.body);
}

usize framed_content_bytes(const FramedContent& content) noexcept
{
    return Size()
        .vector(content.group_id.size())
        .fixed(8U)
        .field(sender_bytes(content.sender))
        .vector(content.authenticated_data.size())
        .fixed(1U)
        .field(body_bytes(content))
        .bytes();
}

void put_framed_content(cont::WireWriter& out, const FramedContent& content) noexcept
{
    put_vector(out, content.group_id);
    out.put_u64(content.epoch);
    put_sender(out, content.sender);
    put_vector(out, content.authenticated_data);
    out.put_u8(static_cast<u8>(content.content_type));
    put_body(out, content);
}

bool take_authenticated_content(cont::WireReader& in, WireFormat& wire_format, FramedContent& content,
                                ContentAuth& auth) noexcept
{
    u16 format = 0U;
    if (!in.take_u16(format) || !known_wire_format(format) || !take_framed_content(in, content) ||
        !take_auth(in, content.content_type, auth))
    {
        return false;
    }
    wire_format = static_cast<WireFormat>(format);
    return true;
}

usize authenticated_content_bytes(const FramedContent& content, const ContentAuth& auth) noexcept
{
    return Size().fixed(2U).field(framed_content_bytes(content)).field(auth_bytes(content.content_type, auth)).bytes();
}

void put_authenticated_content(cont::WireWriter& out, WireFormat wire_format, const FramedContent& content,
                               const ContentAuth& auth) noexcept
{
    out.put_u16(static_cast<u16>(wire_format));
    put_framed_content(out, content);
    put_auth(out, content.content_type, auth);
}

bool framing_admits(WireFormat wire_format, const Sender& sender, ContentType content_type) noexcept
{
    if (!known_content_type(static_cast<u8>(content_type)))
    {
        return false;
    }
    if (wire_format == WireFormat::PrivateMessage)
    {
        return sender.type == SenderType::Member;
    }
    if (wire_format != WireFormat::PublicMessage || content_type == ContentType::Application)
    {
        return false;
    }
    switch (sender.type)
    {
    case SenderType::Member:
        return true;
    case SenderType::External:
    case SenderType::NewMemberProposal:
        return content_type == ContentType::Proposal;
    case SenderType::NewMemberCommit:
        return content_type == ContentType::Commit;
    }
    return false;
}

CryptoError sign_content(const CipherSuite& suite, const Signer& signer, WireFormat wire_format,
                         const FramedContent& content, const GroupContext& context, cont::Span<u8> signature,
                         usize& written) noexcept
{
    written = 0U;
    if (!framing_admits(wire_format, content.sender, content.content_type))
    {
        return refuse(CryptoError::Malformed, signature);
    }
    if (signer.algorithm() != suite.signature)
    {
        return refuse(CryptoError::Unsupported, signature);
    }
    usize       tbs   = 0U;
    const usize bytes = sign_content_bytes(content, context, tbs);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, signature);
    }
    LabelBuffer      input(bytes);
    cont::WireWriter writer(input.span());
    put_sign_content(writer, wire_format, content, context, tbs);
    const CryptoError error = signer.sign(input.span(), signature, written);
    if (error != CryptoError::None)
    {
        written = 0U;
        return refuse(error, signature);
    }
    return error;
}

Verification verify_content(const CipherSuite& suite, const SignatureVerifier& verifier,
                            cont::ConstSpan<u8> public_key, WireFormat wire_format, const FramedContent& content,
                            const GroupContext& context, cont::ConstSpan<u8> signature) noexcept
{
    usize       tbs   = 0U;
    const usize bytes = sign_content_bytes(content, context, tbs);
    if (!framing_admits(wire_format, content.sender, content.content_type) || bytes == 0U)
    {
        return Verification::Invalid;
    }
    LabelBuffer      input(bytes);
    cont::WireWriter writer(input.span());
    put_sign_content(writer, wire_format, content, context, tbs);
    return verifier.verify(suite.signature, public_key, input.span(), signature);
}

// ---- Messages ----

bool take_message(cont::ConstSpan<u8> bytes, Message& out) noexcept
{
    cont::WireReader in(bytes);
    u16              version = 0U;
    u16              format  = 0U;
    out                      = {};
    if (!in.take_u16(version) || version != kProtocolVersion || !in.take_u16(format) || !known_wire_format(format))
    {
        return false;
    }
    out.wire_format = static_cast<WireFormat>(format);
    bool ok         = false;
    switch (out.wire_format)
    {
    case WireFormat::PublicMessage:
    {
        PublicMessage& message = out.public_message;
        ok = take_framed_content(in, message.content) && take_auth(in, message.content.content_type, message.auth) &&
             (message.content.sender.type != SenderType::Member || take_vector(in, message.membership_tag));
        break;
    }
    case WireFormat::PrivateMessage:
        ok = take_private(in, out.private_message);
        break;
    case WireFormat::KeyPackage:
        ok = take_key_package(in, out.key_package);
        break;
    case WireFormat::Welcome:
    case WireFormat::GroupInfo:
        // Neither structure encodes to nothing: an empty body is refused here, as message_bytes would refuse it.
        out.body = in.rest();
        ok       = !out.body.empty() && in.take_bytes(out.body.size(), out.body);
        break;
    }
    return ok && in.done();
}

usize message_bytes(const Message& message) noexcept
{
    usize body = 0U;
    switch (message.wire_format)
    {
    case WireFormat::PublicMessage:
        body = public_struct_bytes(message.public_message);
        break;
    case WireFormat::PrivateMessage:
        body = private_struct_bytes(message.private_message);
        break;
    case WireFormat::KeyPackage:
        body = key_package_bytes(message.key_package);
        break;
    case WireFormat::Welcome:
    case WireFormat::GroupInfo:
        body = message.body.empty() ? 0U : message.body.size();
        break;
    }
    return body == 0U ? 0U : 4U + body;
}

void put_message(cont::WireWriter& out, const Message& message) noexcept
{
    out.put_u16(kProtocolVersion);
    out.put_u16(static_cast<u16>(message.wire_format));
    switch (message.wire_format)
    {
    case WireFormat::PublicMessage:
    {
        const PublicMessage& public_message = message.public_message;
        put_framed_content(out, public_message.content);
        put_auth(out, public_message.content.content_type, public_message.auth);
        if (public_message.content.sender.type == SenderType::Member)
        {
            put_vector(out, public_message.membership_tag);
        }
        break;
    }
    case WireFormat::PrivateMessage:
    {
        const PrivateMessage& private_message = message.private_message;
        put_private_header(out, private_message.group_id, private_message.epoch, private_message.content_type,
                           private_message.authenticated_data);
        put_vector(out, private_message.encrypted_sender_data);
        put_vector(out, private_message.ciphertext);
        break;
    }
    case WireFormat::KeyPackage:
        put_key_package(out, message.key_package);
        break;
    case WireFormat::Welcome:
    case WireFormat::GroupInfo:
        out.put_bytes(message.body);
        break;
    }
}

CryptoError membership_tag(const CipherSuite& suite, cont::ConstSpan<u8> membership_key,
                           const FramedContent& content, const ContentAuth& auth, const GroupContext& context,
                           cont::Span<u8> out) noexcept
{
    const usize tbs   = content_tbs_bytes(content, context);
    const usize tail  = auth_bytes(content.content_type, auth);
    if (tbs == 0U || tail == 0U || out.size() != suite.hash_bytes())
    {
        return refuse(CryptoError::BadLength, out);
    }
    LabelBuffer      input(tbs + tail);
    cont::WireWriter writer(input.span());
    put_content_tbs(writer, WireFormat::PublicMessage, content, context);
    put_auth(writer, content.content_type, auth);
    return mac(suite, membership_key, input.span(), out);
}

CryptoError protect_public(const CipherSuite& suite, cont::ConstSpan<u8> membership_key, const GroupContext& context,
                           const FramedContent& content, const ContentAuth& auth, cont::Span<u8> out,
                           usize& written) noexcept
{
    written = 0U;
    if (!framing_admits(WireFormat::PublicMessage, content.sender, content.content_type) ||
        !auth_fits(suite, content.content_type, auth))
    {
        return refuse(CryptoError::Malformed, out);
    }
    u8      tag[kMaxHashBytes] = {};
    Message message;
    message.public_message = {content, auth, {}};
    if (content.sender.type == SenderType::Member)
    {
        const cont::Span<u8> tag_span(tag, suite.hash_bytes());
        const CryptoError    error = membership_tag(suite, membership_key, content, auth, context, tag_span);
        if (error != CryptoError::None)
        {
            return refuse(error, out);
        }
        message.public_message.membership_tag = tag_span;
    }
    const usize bytes = message_bytes(message);
    if (bytes == 0U || out.size() < bytes)
    {
        return refuse(CryptoError::BadLength, out);
    }
    cont::WireWriter writer(out);
    put_message(writer, message);
    written = bytes;
    return CryptoError::None;
}

CryptoError verify_public(const CipherSuite& suite, cont::ConstSpan<u8> membership_key, const GroupContext& context,
                          const PublicMessage& message) noexcept
{
    const FramedContent& content = message.content;
    if (!framing_admits(WireFormat::PublicMessage, content.sender, content.content_type) ||
        !auth_fits(suite, content.content_type, message.auth))
    {
        return CryptoError::Malformed;
    }
    if (content.sender.type != SenderType::Member)
    {
        return CryptoError::None;
    }
    u8                   tag[kMaxHashBytes] = {};
    const cont::Span<u8> tag_span(tag, suite.hash_bytes());
    const CryptoError    error = membership_tag(suite, membership_key, content, message.auth, context, tag_span);
    if (error != CryptoError::None)
    {
        return error;
    }
    return message.membership_tag.size() == tag_span.size() && constant_time_equal(tag_span, message.membership_tag)
               ? CryptoError::None
               : CryptoError::Forged;
}

usize padme_padding(usize length) noexcept
{
    if (length < 2U)
    {
        return 0U;
    }
    const usize exponent  = static_cast<usize>(std::bit_width(length)) - 1U; // floor(log2(length))
    const usize kept      = static_cast<usize>(std::bit_width(exponent));    // floor(log2(exponent)) + 1
    const usize low       = exponent - kept;
    const usize mask      = (usize{1} << low) - 1U;
    return ((length + mask) & ~mask) - length;
}

usize private_message_bytes(const FramedContent& content, const ContentAuth& auth, usize padding) noexcept
{
    const usize body = body_bytes(content);
    const usize tail = auth_bytes(content.content_type, auth);
    if (body == 0U || tail == 0U || padding > kMaxVarint)
    {
        return 0U;
    }
    return Size()
        .fixed(4U)
        .vector(content.group_id.size())
        .fixed(9U)
        .vector(content.authenticated_data.size())
        .vector(kSealedSenderDataBytes)
        .vector(body + tail + padding + kAeadTagBytes)
        .bytes();
}

CryptoError protect_private(const CipherSuite& suite, SecretTree& tree, cont::ConstSpan<u8> sender_data_secret,
                            const FramedContent& content, const ContentAuth& auth, usize padding,
                            cont::Span<u8> out) noexcept
{
    if (!framing_admits(WireFormat::PrivateMessage, content.sender, content.content_type) ||
        !auth_fits(suite, content.content_type, auth))
    {
        return refuse(CryptoError::Malformed, out);
    }
    const usize bytes = private_message_bytes(content, auth, padding);
    if (bytes == 0U || out.size() != bytes)
    {
        return refuse(CryptoError::BadLength, out);
    }
    MessageKey        key;
    const RatchetKind kind  = ratchet_of(content.content_type);
    CryptoError       error = tree.send_key(content.sender.index, kind, key);
    u8                guard[kReuseGuardBytes] = {};
    if (error == CryptoError::None)
    {
        error = random_bytes(guard);
    }
    if (error != CryptoError::None)
    {
        return refuse(error, out);
    }

    // The header (PrivateContentAAD, whose prefix is SenderDataAAD), then the sender data's place and the content.
    const usize      plain = body_bytes(content) + auth_bytes(content.content_type, auth) + padding;
    cont::WireWriter writer(out);
    writer.put_u16(kProtocolVersion);
    writer.put_u16(static_cast<u16>(WireFormat::PrivateMessage));
    put_vector(writer, content.group_id);
    writer.put_u64(content.epoch);
    writer.put_u8(static_cast<u8>(content.content_type));
    const usize sender_aad_end = writer.written();
    put_vector(writer, content.authenticated_data);
    const usize header_end = writer.written();
    put_varint(writer, kSealedSenderDataBytes);
    const usize sender_data_at = writer.written();
    const usize ciphertext_at  = sender_data_at + kSealedSenderDataBytes + varint_bytes(plain + kAeadTagBytes);
    const cont::Span<u8> header      = out.subspan(4U, header_end - 4U);
    const cont::Span<u8> sealed      = out.subspan(ciphertext_at, plain + kAeadTagBytes);
    const cont::Span<u8> sender_data = out.subspan(sender_data_at, kSealedSenderDataBytes);
    {
        cont::WireWriter tail(out.subspan(sender_data_at + kSealedSenderDataBytes));
        put_varint(tail, plain + kAeadTagBytes);
        cont::WireWriter text(sealed.first(plain));
        put_body(text, content);
        put_auth(text, content.content_type, auth);
        for (usize i = 0U; i < padding; ++i)
        {
            text.put_u8(0U);
        }
    }

    SealKey content_key;
    guard_nonce(key, guard, content_key.nonce);
    error = content_key.aead.init(suite.aead, key.aead_key());
    if (error == CryptoError::None)
    {
        error = content_key.aead.seal(content_key.nonce, sealed.first(plain), header, sealed);
    }
    SealKey sender_key;
    if (error == CryptoError::None)
    {
        error = sender_key.sender_data(suite, sender_data_secret, sealed);
    }
    if (error == CryptoError::None)
    {
        u8               fields[kSenderDataBytes] = {};
        cont::WireWriter data(fields);
        data.put_u32(content.sender.index);
        data.put_u32(key.generation);
        data.put_bytes(guard);
        error = sender_key.aead.seal(sender_key.nonce, fields, header.first(sender_aad_end - 4U), sender_data);
    }
    return error == CryptoError::None ? error : refuse(error, out);
}

CryptoError open_private(const CipherSuite& suite, SecretTree& tree, cont::ConstSpan<u8> sender_data_secret,
                         const PrivateMessage& message, cont::Span<u8> plaintext, FramedContent& content,
                         ContentAuth& auth) noexcept
{
    content = {};
    auth    = {};
    if (message.header.empty() || message.sender_data_aad_bytes == 0U ||
        message.sender_data_aad_bytes > message.header.size())
    {
        return refuse(CryptoError::BadLength, plaintext);
    }
    if (message.encrypted_sender_data.size() != kSealedSenderDataBytes || message.ciphertext.size() < kAeadTagBytes)
    {
        return refuse(CryptoError::Malformed, plaintext);
    }
    if (plaintext.size() != message.ciphertext.size() - kAeadTagBytes)
    {
        return refuse(CryptoError::BadLength, plaintext);
    }

    // The sender data first: whose leaf, which generation, and the reuse guard.
    SealKey     sender_key;
    u8          fields[kSenderDataBytes] = {};
    CryptoError error                    = sender_key.sender_data(suite, sender_data_secret, message.ciphertext);
    if (error == CryptoError::None)
    {
        error = sender_key.aead.open(sender_key.nonce, message.encrypted_sender_data,
                                     message.header.first(message.sender_data_aad_bytes), fields);
    }
    if (error != CryptoError::None)
    {
        return refuse(error, plaintext);
    }
    cont::WireReader    data(fields);
    u32                 leaf       = 0U;
    u32                 generation = 0U;
    cont::ConstSpan<u8> guard;
    if (!data.take_u32(leaf) || !data.take_u32(generation) || !data.take_bytes(kReuseGuardBytes, guard))
    {
        return refuse(CryptoError::Malformed, plaintext);
    }

    // Then the content, under the key the tree keeps for that generation; nothing is spent before it authenticated.
    const RatchetKind kind = ratchet_of(message.content_type);
    MessageKey        key;
    error = tree.receive_key(leaf, kind, generation, key);
    SealKey content_key;
    if (error == CryptoError::None)
    {
        guard_nonce(key, guard, content_key.nonce);
        error = content_key.aead.init(suite.aead, key.aead_key());
    }
    if (error == CryptoError::None)
    {
        error = content_key.aead.open(content_key.nonce, message.ciphertext, message.header, plaintext);
    }
    if (error != CryptoError::None)
    {
        return refuse(error, plaintext);
    }

    // PrivateMessageContent: the body, the authentication, and padding that must be zero (section 6.3.1).
    cont::WireReader    text(plaintext);
    cont::ConstSpan<u8> body;
    ContentAuth         received;
    if (!take_body(text, message.content_type, body) || !take_auth(text, message.content_type, received) ||
        !auth_fits(suite, message.content_type, received) || !all_zero(text.rest()))
    {
        return refuse(CryptoError::Malformed, plaintext);
    }
    error = tree.consume(leaf, kind, generation);
    if (error != CryptoError::None)
    {
        return refuse(error, plaintext);
    }
    content = {message.group_id, message.epoch, {SenderType::Member, leaf}, message.authenticated_data,
               message.content_type, body};
    auth    = received;
    return CryptoError::None;
}

// ---- Transcript hashes ----

CryptoError confirmed_transcript_hash(const CipherSuite& suite, cont::ConstSpan<u8> interim, WireFormat wire_format,
                                      const FramedContent& content, cont::ConstSpan<u8> signature,
                                      cont::Span<u8> out) noexcept
{
    const usize fields = Size().fixed(2U).field(framed_content_bytes(content)).vector(signature.size()).bytes();
    if (fields == 0U || out.size() != suite.hash_bytes())
    {
        return refuse(CryptoError::BadLength, out);
    }
    LabelBuffer      input(interim.size() + fields);
    cont::WireWriter writer(input.span());
    writer.put_bytes(interim);
    writer.put_u16(static_cast<u16>(wire_format));
    put_framed_content(writer, content);
    put_vector(writer, signature);
    const CryptoError error = kdf_hash(suite.kdf, input.span(), out);
    return error == CryptoError::None ? error : refuse(error, out);
}

CryptoError interim_transcript_hash(const CipherSuite& suite, cont::ConstSpan<u8> confirmed,
                                    cont::ConstSpan<u8> confirmation_tag, cont::Span<u8> out) noexcept
{
    const usize tag = vector_bytes(confirmation_tag.size());
    if (tag == 0U || out.size() != suite.hash_bytes())
    {
        return refuse(CryptoError::BadLength, out);
    }
    LabelBuffer      input(confirmed.size() + tag);
    cont::WireWriter writer(input.span());
    writer.put_bytes(confirmed);
    put_vector(writer, confirmation_tag);
    const CryptoError error = kdf_hash(suite.kdf, input.span(), out);
    return error == CryptoError::None ? error : refuse(error, out);
}

CryptoError confirmation_tag(const CipherSuite& suite, cont::ConstSpan<u8> confirmation_key,
                             cont::ConstSpan<u8> confirmed, cont::Span<u8> out) noexcept
{
    return mac(suite, confirmation_key, confirmed, out);
}

bool confirmation_tag_valid(const CipherSuite& suite, cont::ConstSpan<u8> confirmation_key,
                            cont::ConstSpan<u8> confirmed, cont::ConstSpan<u8> tag) noexcept
{
    u8                   expected[kMaxHashBytes] = {};
    const cont::Span<u8> span(expected, suite.hash_bytes());
    return tag.size() == span.size() &&
           confirmation_tag(suite, confirmation_key, confirmed, span) == CryptoError::None &&
           constant_time_equal(span, tag);
}
} // namespace crd::crypto::mls
