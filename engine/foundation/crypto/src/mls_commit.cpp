// crd-crypto -- MLS proposals and commits, RFC 9420 sections 5.2 and 12.1 to 12.4.2: a member's messages of an epoch
// (proposals, application data and commits) received and sent, and a commit staged and merged (NET.2; contract:
// docs/design/network-foundation.md#mls, M3 layer 9 and decision M10).
#include <crd/crypto/mls_group.hpp>

#include "byte_spans.hpp"
#include "mls_detail.hpp"

#include <algorithm>

namespace crd::crypto::mls
{
namespace
{
using detail::bytes_less;
using detail::LabelBuffer;
using detail::refuse;
using detail::repeats;
using detail::same_bytes;

constexpr cont::StringView kProposalRefLabel = "MLS 1.0 Proposal Reference";

constexpr u16 code(ProposalType type) noexcept
{
    return static_cast<u16>(type);
}

// The members of a framed content's sender a group can name.
bool is_member(const Sender& sender) noexcept
{
    return sender.type == SenderType::Member;
}

// An MLSMessage's header before its body.
void put_message_header(cont::WireWriter& out, WireFormat wire_format) noexcept
{
    out.put_u16(kProtocolVersion);
    out.put_u16(static_cast<u16>(wire_format));
}

// The leaves of a sorted list that occur twice.
bool repeated(cont::Array<u32>& leaves) noexcept
{
    std::sort(leaves.begin(), leaves.end());
    return std::adjacent_find(leaves.begin(), leaves.end()) != leaves.end();
}
} // namespace

// ---- Proposals ----

bool path_required(u16 type) noexcept
{
    return type != code(ProposalType::Add) && type != code(ProposalType::PreSharedKey) &&
           type != code(ProposalType::ReInit);
}

bool external_proposal(u16 type) noexcept
{
    return type == code(ProposalType::Add) || type == code(ProposalType::Remove) ||
           type == code(ProposalType::PreSharedKey) || type == code(ProposalType::ReInit) ||
           type == code(ProposalType::GroupContextExtensions);
}

CryptoError proposal_ref(const CipherSuite& suite, WireFormat wire_format, const FramedContent& content,
                         const ContentAuth& auth, cont::Span<u8> out) noexcept
{
    const usize bytes = authenticated_content_bytes(content, auth);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, out);
    }
    LabelBuffer      encoded(bytes);
    cont::WireWriter writer(encoded.span());
    put_authenticated_content(writer, wire_format, content, auth);
    return ref_hash(suite, kProposalRefLabel, encoded.span(), out);
}

// ---- Staged commits and received messages ----

StagedCommit::~StagedCommit() noexcept
{
    clear();
}

void StagedCommit::clear() noexcept
{
    m_group      = nullptr;
    m_from_epoch = 0U;
    m_committer  = {};
    m_removed    = false;
    m_has_path   = false;
    fresh_buffer(m_bytes, 0U);
    m_entries.clear();
    m_added.clear();
    m_next.clear();
    m_update.clear();
}

bool StagedCommit::proposal(usize i, Proposal& out, Sender& sender) const noexcept
{
    out    = {};
    sender = {};
    if (i >= m_entries.size())
    {
        return false;
    }
    const Entry&     entry = m_entries[i];
    cont::WireReader in(cont::ConstSpan<u8>(m_bytes.data() + entry.offset, entry.bytes));
    sender = entry.sender;
    return take_proposal(in, out);
}

ReceivedMessage::~ReceivedMessage() noexcept
{
    wipe();
}

void ReceivedMessage::wipe() noexcept
{
    fresh_buffer(m_bytes, 0U);
    m_wire_format = WireFormat::PublicMessage;
    m_content     = {};
    m_auth        = {};
    m_proposal    = {};
    secure_zero(m_ref);
    m_ref_bytes = 0U;
    m_commit.clear();
}

// ---- A commit's list (sections 12.2 and 12.3) ----

// One proposal of a commit's list: the proposal, its sender (the committer for one by value), its encoding and its
// reference when committed by one.
struct Group::Item
{
    Proposal            proposal;
    Sender              sender;
    cont::ConstSpan<u8> encoding;
    cont::ConstSpan<u8> reference;
};

// A commit's list and what checking it found: its PSKs in the list's order and whether it requires a path.
struct Group::Plan
{
    cont::Array<Item>         items;
    cont::Array<PreSharedKey> psks;
    bool                      needs_path = false;
};

cont::ConstSpan<u8> Group::psk_of(const PreSharedKeyId& id) const noexcept
{
    if (id.type == PskType::Resumption && id.usage == ResumptionUsage::Application &&
        same_bytes(id.group_id, view(m_group_id)))
    {
        if (id.epoch == m_now.epoch)
        {
            return m_now.secrets.get(EpochSecret::Resumption);
        }
        for (const Resumption& past : m_resumption)
        {
            if (past.epoch == id.epoch)
            {
                return cont::ConstSpan<u8>(past.secret, m_suite->hash_bytes());
            }
        }
    }
    return m_config.store != nullptr ? m_config.store->psk(id) : cont::ConstSpan<u8>();
}

const Group::Cached* Group::cached(cont::ConstSpan<u8> ref) const noexcept
{
    const usize hash = m_suite->hash_bytes();
    for (const Cached& entry : m_cache)
    {
        if (same_bytes(cont::ConstSpan<u8>(entry.ref, hash), ref))
        {
            return &entry;
        }
    }
    return nullptr;
}

usize Group::pending_leaf(cont::ConstSpan<u8> public_key) const noexcept
{
    u8                   bytes[kMaxNodeKeyBytes] = {};
    const cont::Span<u8> key(bytes, hpke_sizes(m_suite->kem).public_key);
    for (usize i = 0U; i < m_pending_leaves.size(); ++i)
    {
        if (m_pending_leaves[i].public_key(key) == CryptoError::None && same_bytes(key, public_key))
        {
            return i;
        }
    }
    return m_pending_leaves.size();
}

CryptoError Group::stage_list(u32 committer, Plan& plan, StagedCommit& staged) const noexcept
{
    const CipherSuite&  suite = *m_suite;
    const GroupConfig&  config = m_config;
    const RatchetTree&  now    = m_now.tree;
    const usize         count  = plan.items.size();
    cont::Array<u32>    leaves; // the leaves Updates and Removes apply to
    cont::Array<cont::ConstSpan<u8>> psk_ids;
    const Item*         extensions = nullptr;
    if (!leaves.try_reserve(count) || !psk_ids.try_reserve(count) || !plan.psks.try_reserve(count) ||
        !staged.m_added.try_reserve(count))
    {
        return CryptoError::ProviderFailure;
    }

    // Section 12.2's rules of the list as a whole, and each PSK's (12.1.4).
    plan.needs_path = count == 0U;
    for (const Item& item : plan.items)
    {
        const Proposal& proposal = item.proposal;
        plan.needs_path          = plan.needs_path || path_required(proposal.type);
        switch (proposal.type)
        {
        case code(ProposalType::Add):
            break;
        case code(ProposalType::Update):
            if (!is_member(item.sender) || item.sender.index == committer)
            {
                return CryptoError::Malformed;
            }
            leaves.push_back(item.sender.index);
            break;
        case code(ProposalType::Remove):
            if (proposal.remove == committer || proposal.remove >= now.leaves() || now.blank(2U * proposal.remove))
            {
                return CryptoError::Malformed;
            }
            leaves.push_back(proposal.remove);
            break;
        case code(ProposalType::PreSharedKey):
        {
            const CryptoError checked = check_psk_id(suite, proposal.psk);
            if (checked != CryptoError::None)
            {
                return checked;
            }
            const PreSharedKey key{proposal.psk, psk_of(proposal.psk)};
            if (key.secret.empty())
            {
                return CryptoError::NotReady;
            }
            plan.psks.push_back(key);
            psk_ids.push_back(item.encoding.subspan(2U)); // the PreSharedKeyID after the proposal's type
            break;
        }
        case code(ProposalType::GroupContextExtensions):
            if (extensions != nullptr || !extensions_distinct(proposal.extensions))
            {
                return CryptoError::Malformed;
            }
            extensions = &item;
            break;
        case code(ProposalType::ExternalInit):
            return CryptoError::Malformed; // only an external commit holds one
        default:
            return CryptoError::Unsupported; // ReInit lands with section 11.2 (decision M10); no other type is ours
        }
    }
    if (repeated(leaves) || repeats(psk_ids))
    {
        return CryptoError::Malformed;
    }

    // Section 12.3: the new GroupContext's extensions, then Updates, Removes and Adds on a copy of the tree, each
    // proposal checked as section 12.1 says against the new extensions.
    RatchetTree&              tree = staged.m_next.tree;
    const cont::ConstSpan<u8> next_extensions =
        extensions != nullptr ? extensions->proposal.extensions : view(m_now.extensions);
    const GroupContext next{suite.id, view(m_group_id), m_now.epoch + 1U, {}, {}, next_extensions};
    CryptoError        error = tree.copy_from(now);
    for (const Item& item : plan.items)
    {
        if (error != CryptoError::None || item.proposal.type != code(ProposalType::Update))
        {
            continue;
        }
        const LeafNode& leaf    = item.proposal.update;
        LeafNode        current = {};
        error = tree.leaf_node(item.sender.index, current)
                    ? check_leaf(next, leaf, LeafRole::Update, config.policy, *config.authentication)
                    : CryptoError::OutOfWindow;
        if (error == CryptoError::None &&
            verify_leaf_node(suite, *config.verifier, leaf, view(m_group_id), item.sender.index) !=
                Verification::Valid)
        {
            error = CryptoError::Forged;
        }
        if (error == CryptoError::None && same_bytes(leaf.encryption_key, current.encryption_key))
        {
            error = CryptoError::BadKey; // section 7.3: an Update replaces the leaf's encryption key
        }
        if (error == CryptoError::None)
        {
            error = tree.update(item.sender.index, leaf);
        }
    }
    for (const Item& item : plan.items)
    {
        if (error == CryptoError::None && item.proposal.type == code(ProposalType::Remove))
        {
            staged.m_removed = staged.m_removed || item.proposal.remove == own_leaf();
            error            = tree.remove(item.proposal.remove);
        }
    }
    for (const Item& item : plan.items)
    {
        if (error != CryptoError::None || item.proposal.type != code(ProposalType::Add))
        {
            continue;
        }
        u32 index = 0U;
        error = check_key_package(next, item.proposal.add, *config.verifier, *config.authentication, config.policy);
        if (error == CryptoError::None)
        {
            error = tree.add(item.proposal.add.leaf_node, index);
        }
        if (error == CryptoError::None)
        {
            staged.m_added.push_back(index);
        }
    }
    // Section 12.1.7: every member the commit keeps supports new extensions (those it adds were checked above).
    for (u32 leaf = 0U; error == CryptoError::None && extensions != nullptr && leaf < tree.leaves(); ++leaf)
    {
        LeafNode node{};
        if (tree.leaf_node(leaf, node))
        {
            error = supports_context(next_extensions, node.capabilities);
        }
    }
    return error == CryptoError::None ? copy_into(next_extensions, staged.m_next.extensions) : error;
}

namespace
{
// Section 7.3's checks a commit's new tree must keep: unique keys and the credential types (section 13.3).
CryptoError check_new_tree(const RatchetTree& tree) noexcept
{
    const CryptoError error = tree.check_unique_keys();
    return error == CryptoError::None ? check_credential_types(tree) : error;
}
} // namespace

CryptoError Group::advance(StagedCommit& staged, const Plan& plan, WireFormat wire_format,
                           const FramedContent& content, cont::ConstSpan<u8> signature,
                           cont::ConstSpan<u8> commit_secret) const noexcept
{
    const CipherSuite& suite = *m_suite;
    const usize        hash  = suite.hash_bytes();
    GroupEpoch&        next  = staged.m_next;
    next.epoch               = m_now.epoch + 1U;
    if (!fresh_buffer(next.confirmed, hash))
    {
        return CryptoError::ProviderFailure;
    }
    CryptoError error = confirmed_transcript_hash(suite, view(m_now.interim), wire_format, content, signature,
                                                  cont::Span<u8>(next.confirmed.data(), hash));
    if (error != CryptoError::None || staged.m_removed)
    {
        return error; // a member the commit removes has no part in the new epoch's secrets
    }
    const GroupContext context{suite.id,           view(m_group_id), next.epoch, next.tree.root_hash(),
                               view(next.confirmed), view(next.extensions)};
    const usize        bytes = group_context_bytes(context);
    if (bytes == 0U)
    {
        return CryptoError::BadLength;
    }
    LabelBuffer      encoded(bytes);
    cont::WireWriter writer(encoded.span());
    put_group_context(writer, context);
    const u8 zero[kMaxHashBytes] = {};
    Secret   psk;
    error = psk_secret(suite, cont::ConstSpan<PreSharedKey>(plan.psks.data(), plan.psks.size()), psk.span(hash));
    if (error == CryptoError::None)
    {
        error = next.secrets.advance(suite, m_now.secrets.get(EpochSecret::Init),
                                     commit_secret.empty() ? cont::ConstSpan<u8>(zero, hash) : commit_secret,
                                     psk.span(hash), encoded.span());
    }
    return error == CryptoError::None ? next.secret_tree.init(suite, next.secrets.get(EpochSecret::Encryption),
                                                              next.tree.leaves(), m_config.window)
                                      : error;
}

CryptoError Group::record(const Plan& plan, u32 committer, bool has_path, StagedCommit& staged) const noexcept
{
    usize total = 0U;
    for (const Item& item : plan.items)
    {
        total += item.encoding.size();
    }
    if (!staged.m_bytes.try_reserve(total) || !staged.m_entries.try_reserve(plan.items.size()))
    {
        return CryptoError::ProviderFailure;
    }
    for (const Item& item : plan.items)
    {
        staged.m_entries.push_back({staged.m_bytes.size(), item.encoding.size(), item.sender});
        for (const u8 byte : item.encoding)
        {
            staged.m_bytes.push_back(byte);
        }
    }
    staged.m_group      = this;
    staged.m_from_epoch = m_now.epoch;
    staged.m_committer  = {SenderType::Member, committer};
    staged.m_has_path   = has_path;
    return CryptoError::None;
}

// ---- Receiving ----

CryptoError Group::cache(WireFormat wire_format, const FramedContent& content, const ContentAuth& auth,
                         cont::Span<u8> ref) noexcept
{
    const usize hash = m_suite->hash_bytes();
    CryptoError error = ref.size() == hash ? proposal_ref(*m_suite, wire_format, content, auth, ref)
                                           : CryptoError::BadLength;
    if (error != CryptoError::None)
    {
        return refuse(error, ref);
    }
    if (cached(ref) != nullptr)
    {
        return refuse(CryptoError::Replayed, ref);
    }
    if (m_cache.size() >= m_config.pending_proposals)
    {
        return refuse(CryptoError::Exhausted, ref);
    }
    Cached entry;
    copy_bytes(ref, entry.ref);
    entry.sender = content.sender;
    error        = copy_into(content.body, entry.bytes);
    if (error == CryptoError::None && !m_cache.try_push_back(std::move(entry)))
    {
        error = CryptoError::ProviderFailure;
    }
    return error == CryptoError::None ? error : refuse(error, ref);
}

CryptoError Group::sender_key(const ReceivedMessage& message, cont::ConstSpan<u8>& key) const noexcept
{
    const Sender& sender = message.m_content.sender;
    key                  = {};
    switch (sender.type)
    {
    case SenderType::Member:
    {
        LeafNode leaf{};
        if (!m_now.tree.leaf_node(sender.index, leaf))
        {
            return CryptoError::OutOfWindow;
        }
        key = leaf.signature_key;
        return sender.index == own_leaf() ? CryptoError::Replayed : CryptoError::None;
    }
    case SenderType::External:
    {
        cont::ConstSpan<u8> data;
        cont::ConstSpan<u8> senders;
        if (!find_extension(view(m_now.extensions), static_cast<u16>(ExtensionType::ExternalSenders), data) ||
            !take_external_senders(data, senders))
        {
            return CryptoError::OutOfWindow;
        }
        cont::WireReader in(senders);
        ExternalSender   entry;
        for (u32 i = 0U; i <= sender.index; ++i)
        {
            if (in.done() || !take_external_sender(in, entry))
            {
                return CryptoError::OutOfWindow;
            }
        }
        key = entry.signature_key;
        return CryptoError::None;
    }
    case SenderType::NewMemberProposal:
        // A new member's Add, signed with its key package's leaf key (section 12.1.8).
        key = message.m_proposal.add.leaf_node.signature_key;
        return CryptoError::None;
    case SenderType::NewMemberCommit:
        return CryptoError::Unsupported; // an external join (decision M10)
    }
    return CryptoError::Malformed;
}

CryptoError Group::process(cont::ConstSpan<u8> message, ReceivedMessage& out) noexcept
{
    out.wipe();
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    CryptoError error = copy_into(message, out.m_bytes);
    Message     decoded;
    if (error == CryptoError::None && !take_message(view(out.m_bytes), decoded))
    {
        error = CryptoError::Malformed;
    }
    const GroupContext context = this->context();
    FramedContent&     content = out.m_content;
    if (error == CryptoError::None)
    {
        out.m_wire_format = decoded.wire_format;
        switch (decoded.wire_format)
        {
        case WireFormat::PublicMessage:
        {
            const PublicMessage& received = decoded.public_message;
            if (!same_bytes(received.content.group_id, view(m_group_id)))
            {
                error = CryptoError::Malformed;
            }
            else if (received.content.epoch != m_now.epoch)
            {
                error = CryptoError::OutOfWindow;
            }
            else
            {
                error = verify_public(*m_suite, m_now.secrets.get(EpochSecret::Membership), context, received);
            }
            content    = received.content;
            out.m_auth = received.auth;
            break;
        }
        case WireFormat::PrivateMessage:
        {
            const PrivateMessage& received = decoded.private_message;
            if (!same_bytes(received.group_id, view(m_group_id)))
            {
                error = CryptoError::Malformed;
            }
            else if (received.epoch != m_now.epoch)
            {
                error = CryptoError::OutOfWindow;
            }
            else
            {
                // Opened in place: the plaintext overwrites the ciphertext the message owns.
                const usize at = static_cast<usize>(received.ciphertext.data() - out.m_bytes.data());
                const cont::Span<u8> plaintext(out.m_bytes.data() + at,
                                               received.ciphertext.size() >= kAeadTagBytes
                                                   ? received.ciphertext.size() - kAeadTagBytes
                                                   : 0U);
                error = open_private(*m_suite, m_now.secret_tree, m_now.secrets.get(EpochSecret::SenderData),
                                     received, plaintext, content, out.m_auth);
            }
            break;
        }
        default:
            error = CryptoError::Unsupported;
            break;
        }
    }
    // A proposal decodes before its signature is checked: a new member's is signed with its key package's key.
    if (error == CryptoError::None && content.content_type == ContentType::Proposal)
    {
        cont::WireReader in(content.body);
        const u16        type = take_proposal(in, out.m_proposal) && in.done() ? out.m_proposal.type : 0U;
        const bool       admitted =
            type != 0U && type != code(ProposalType::ExternalInit) &&
            (content.sender.type != SenderType::External || external_proposal(type)) &&
            (content.sender.type != SenderType::NewMemberProposal || type == code(ProposalType::Add));
        error = admitted ? CryptoError::None : CryptoError::Malformed;
    }
    cont::ConstSpan<u8> key;
    if (error == CryptoError::None)
    {
        error = sender_key(out, key);
    }
    if (error == CryptoError::None &&
        verify_content(*m_suite, *m_config.verifier, key, out.m_wire_format, content, context,
                       out.m_auth.signature) != Verification::Valid)
    {
        error = CryptoError::Forged;
    }
    if (error == CryptoError::None)
    {
        switch (content.content_type)
        {
        case ContentType::Application:
            break;
        case ContentType::Proposal:
            out.m_ref_bytes = m_suite->hash_bytes();
            error = cache(out.m_wire_format, content, out.m_auth, cont::Span<u8>(out.m_ref, out.m_ref_bytes));
            break;
        case ContentType::Commit:
            error = process_commit(out);
            break;
        }
    }
    if (error != CryptoError::None)
    {
        out.wipe();
    }
    return error;
}

CryptoError Group::process_commit(ReceivedMessage& out) noexcept
{
    const CipherSuite&   suite     = *m_suite;
    const GroupConfig&   config    = m_config;
    const FramedContent& content   = out.m_content;
    StagedCommit&        staged    = out.m_commit;
    const u32            committer = content.sender.index;
    cont::WireReader     in(content.body);
    Commit               commit;
    if (!take_commit(in, commit) || !in.done())
    {
        return CryptoError::Malformed;
    }
    // The list: each proposal by value from the committer, or by reference from the epoch's cache.
    Plan             plan;
    cont::WireReader list(commit.proposals);
    while (!list.done())
    {
        const cont::ConstSpan<u8> start = list.rest();
        ProposalOrRef             entry;
        Item                      item;
        if (!take_proposal_or_ref(list, entry))
        {
            return CryptoError::Malformed;
        }
        if (entry.is_reference)
        {
            const Cached* found = cached(entry.reference);
            if (found == nullptr)
            {
                return CryptoError::NotReady;
            }
            cont::WireReader bytes(view(found->bytes));
            (void)take_proposal(bytes, item.proposal); // cached only after it decoded
            item.sender    = found->sender;
            item.encoding  = view(found->bytes);
            item.reference = entry.reference;
        }
        else
        {
            item.proposal = entry.proposal;
            item.sender   = {SenderType::Member, committer};
            item.encoding = taken_since(start, list).subspan(1U);
        }
        if (!plan.items.try_push_back(item))
        {
            return CryptoError::ProviderFailure;
        }
    }
    CryptoError error = stage_list(committer, plan, staged);
    if (error == CryptoError::None && plan.needs_path && !commit.has_path)
    {
        error = CryptoError::Malformed;
    }

    // The path (section 12.4.2): its leaf checked, merged into the new tree, and the path secret decrypted.
    RatchetTree& tree = staged.m_next.tree;
    if (error == CryptoError::None && commit.has_path)
    {
        const LeafNode&    leaf = commit.path.leaf_node;
        const GroupContext next{suite.id, view(m_group_id), m_now.epoch + 1U, {}, {}, view(staged.m_next.extensions)};
        error = check_leaf(next, leaf, LeafRole::Commit, config.policy, *config.authentication);
        if (error == CryptoError::None &&
            verify_leaf_node(suite, *config.verifier, leaf, view(m_group_id), committer) != Verification::Valid)
        {
            error = CryptoError::Forged;
        }
        if (error == CryptoError::None)
        {
            error = tree.merge_path(committer, commit.path);
        }
        if (error == CryptoError::None && !staged.m_removed)
        {
            // An Update of the member's own leaf in this commit: its kept key decrypts in its place.
            LeafNode before{};
            LeafNode after{};
            (void)m_now.tree.leaf_node(own_leaf(), before);
            (void)tree.leaf_node(own_leaf(), after);
            const HpkeKey* replaced = nullptr;
            if (!same_bytes(before.encryption_key, after.encryption_key))
            {
                const usize kept = pending_leaf(after.encryption_key);
                replaced         = kept < m_pending_leaves.size() ? &m_pending_leaves[kept] : nullptr;
                error            = replaced != nullptr ? CryptoError::None : CryptoError::BadKey;
            }
            const GroupContext provisional{suite.id,  view(m_group_id), m_now.epoch + 1U, tree.root_hash(),
                                           view(m_now.confirmed), view(staged.m_next.extensions)};
            const usize        bytes = group_context_bytes(provisional);
            LabelBuffer        encoded(bytes);
            cont::WireWriter   writer(encoded.span());
            put_group_context(writer, provisional);
            if (error == CryptoError::None)
            {
                error = bytes == 0U ? CryptoError::BadLength
                                    : process_update_path(m_keys, tree, committer, commit.path, encoded.span(),
                                                          staged.added(), staged.m_update, replaced);
            }
        }
    }
    if (error == CryptoError::None)
    {
        error = check_new_tree(tree);
    }
    if (error == CryptoError::None)
    {
        const bool derives = commit.has_path && !staged.m_removed;
        error = advance(staged, plan, out.m_wire_format, content, out.m_auth.signature,
                        derives ? staged.m_update.commit_secret() : cont::ConstSpan<u8>());
    }
    if (error == CryptoError::None && !staged.m_removed)
    {
        error = confirm(staged.m_next, out.m_auth.confirmation_tag);
    }
    return error == CryptoError::None ? record(plan, committer, commit.has_path, staged) : error;
}

// ---- Sending ----

CryptoError Group::check_signer(const Signer& signer) const noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    LeafNode    leaf{};
    u8          key[kMaxNodeKeyBytes] = {};
    const usize bytes                 = signature_sizes(m_suite->signature).public_key;
    const bool  same = signer.algorithm() == m_suite->signature && bytes <= sizeof(key) &&
                      m_now.tree.leaf_node(own_leaf(), leaf) &&
                      signer.public_key(cont::Span<u8>(key, bytes)) == CryptoError::None &&
                      same_bytes(cont::ConstSpan<u8>(key, bytes), leaf.signature_key);
    return same ? CryptoError::None : CryptoError::Unsupported;
}

CryptoError Group::sign_own(const Signer& signer, WireFormat wire_format, const FramedContent& content,
                            cont::Span<u8> signature, usize& written) const noexcept
{
    return sign_content(*m_suite, signer, wire_format, content, context(), signature, written);
}

CryptoError Group::wrap(const SendOptions& options, const FramedContent& content, const ContentAuth& auth,
                        cont::Array<u8>& out) noexcept
{
    out.clear();
    if (options.wire_format == WireFormat::PublicMessage)
    {
        const u8 tag[kMaxHashBytes] = {};
        Message  sized;
        sized.public_message = {content, auth, cont::ConstSpan<u8>(tag, m_suite->hash_bytes())};
        const usize bytes    = message_bytes(sized);
        if (bytes == 0U)
        {
            return CryptoError::BadLength;
        }
        if (!out.try_reserve(bytes))
        {
            return CryptoError::ProviderFailure;
        }
        out.resize(bytes);
        usize             written = 0U;
        const CryptoError error   = protect_public(*m_suite, m_now.secrets.get(EpochSecret::Membership), context(),
                                                   content, auth, cont::Span<u8>(out.data(), bytes), written);
        if (error != CryptoError::None)
        {
            out.clear();
        }
        return error;
    }
    const usize inner = private_content_bytes(content, auth);
    const usize pad   = padme_padding(inner) + options.extra_padding;
    const usize bytes = inner == 0U || pad < options.extra_padding ? 0U : private_message_bytes(content, auth, pad);
    if (bytes == 0U)
    {
        return CryptoError::BadLength;
    }
    if (!out.try_reserve(bytes))
    {
        return CryptoError::ProviderFailure;
    }
    out.resize(bytes);
    const CryptoError error = protect_private(*m_suite, m_now.secret_tree, m_now.secrets.get(EpochSecret::SenderData),
                                              content, auth, pad, cont::Span<u8>(out.data(), bytes));
    if (error != CryptoError::None)
    {
        out.clear();
    }
    return error;
}

CryptoError Group::send_proposal(const Signer& signer, const Proposal& proposal, const SendOptions& options,
                                 cont::Array<u8>& out, cont::Span<u8> ref) noexcept
{
    out.clear();
    CryptoError error = check_signer(signer);
    if (error != CryptoError::None)
    {
        return refuse(error, ref);
    }
    const usize hash  = m_suite->hash_bytes();
    const usize bytes = proposal_bytes(proposal);
    if (ref.size() != hash)
    {
        return refuse(CryptoError::BadLength, ref);
    }
    if (bytes == 0U || (options.wire_format != WireFormat::PublicMessage &&
                        options.wire_format != WireFormat::PrivateMessage))
    {
        return refuse(CryptoError::Malformed, ref);
    }
    if (m_cache.size() >= m_config.pending_proposals)
    {
        return refuse(CryptoError::Exhausted, ref);
    }
    LabelBuffer      body(bytes);
    cont::WireWriter writer(body.span());
    put_proposal(writer, proposal);
    const FramedContent content{view(m_group_id), m_now.epoch, {SenderType::Member, own_leaf()},
                                options.authenticated_data, ContentType::Proposal, body.span()};
    u8    signature[kMaxSignatureBytes] = {};
    usize written                       = 0U;
    error = sign_own(signer, options.wire_format, content, signature, written);
    const ContentAuth auth{cont::ConstSpan<u8>(signature, written), {}};
    if (error == CryptoError::None)
    {
        error = wrap(options, content, auth, out);
    }
    if (error == CryptoError::None)
    {
        error = cache(options.wire_format, content, auth, ref);
    }
    if (error != CryptoError::None)
    {
        out.clear();
        return refuse(error, ref);
    }
    return CryptoError::None;
}

CryptoError Group::propose(const Signer& signer, const Proposal& proposal, const SendOptions& options,
                           cont::Array<u8>& out, cont::Span<u8> ref) noexcept
{
    const u16  type = proposal.type;
    const bool ours = type == code(ProposalType::Add) || type == code(ProposalType::Remove) ||
                      type == code(ProposalType::PreSharedKey) ||
                      type == code(ProposalType::GroupContextExtensions);
    CryptoError error = CryptoError::None;
    if (!ready())
    {
        error = CryptoError::NotReady;
    }
    else if (type == code(ProposalType::Update) || type == code(ProposalType::ExternalInit))
    {
        error = CryptoError::Malformed;
    }
    else if (!ours)
    {
        error = CryptoError::Unsupported; // ReInit lands with section 11.2 (decision M10); no other type is ours
    }
    if (error != CryptoError::None)
    {
        out.clear();
        return refuse(error, ref);
    }
    return send_proposal(signer, proposal, options, out, ref);
}

CryptoError Group::propose_update(const Signer& signer, const SendOptions& options, cont::Array<u8>& out,
                                  cont::Span<u8> ref) noexcept
{
    out.clear();
    CryptoError error = check_signer(signer);
    if (error != CryptoError::None)
    {
        return refuse(error, ref);
    }
    const CipherSuite& suite = *m_suite;
    const usize        npk   = hpke_sizes(suite.kem).public_key;
    HpkeKey            key;
    LeafNode           leaf{};
    u8                 public_key[kMaxNodeKeyBytes]  = {};
    u8                 signature[kMaxSignatureBytes] = {};
    usize              written                       = 0U;
    (void)m_now.tree.leaf_node(own_leaf(), leaf);
    error = key.generate(suite.kem);
    if (error == CryptoError::None)
    {
        error = key.public_key(cont::Span<u8>(public_key, npk));
    }
    leaf.encryption_key = cont::ConstSpan<u8>(public_key, npk);
    leaf.source         = LeafNodeSource::Update;
    leaf.not_before     = 0U;
    leaf.not_after      = 0U;
    leaf.parent_hash    = {};
    leaf.signature      = {};
    if (error == CryptoError::None)
    {
        error = sign_leaf_node(suite, signer, leaf, view(m_group_id), own_leaf(), signature, written);
    }
    leaf.signature = cont::ConstSpan<u8>(signature, written);
    Proposal proposal;
    proposal.type   = code(ProposalType::Update);
    proposal.update = leaf;
    if (error == CryptoError::None && !m_pending_leaves.try_reserve(m_pending_leaves.size() + 1U))
    {
        error = CryptoError::ProviderFailure;
    }
    if (error == CryptoError::None)
    {
        error = send_proposal(signer, proposal, options, out, ref);
    }
    if (error != CryptoError::None)
    {
        return refuse(error, ref);
    }
    m_pending_leaves.push_back(std::move(key));
    return CryptoError::None;
}

CryptoError Group::protect(const Signer& signer, cont::ConstSpan<u8> data, const SendOptions& options,
                           cont::Array<u8>& out) noexcept
{
    out.clear();
    CryptoError error = check_signer(signer);
    if (error != CryptoError::None)
    {
        return error;
    }
    if (options.wire_format != WireFormat::PrivateMessage)
    {
        return CryptoError::Malformed;
    }
    if (!m_cache.empty())
    {
        return CryptoError::NotReady; // section 12.4: commit the epoch's proposals before sending application data
    }
    const FramedContent content{view(m_group_id), m_now.epoch, {SenderType::Member, own_leaf()},
                                options.authenticated_data, ContentType::Application, data};
    u8    signature[kMaxSignatureBytes] = {};
    usize written                       = 0U;
    error = sign_own(signer, options.wire_format, content, signature, written);
    return error == CryptoError::None ? wrap(options, content, {cont::ConstSpan<u8>(signature, written), {}}, out)
                                      : error;
}

// ---- Committing ----

CryptoError Group::select_pending(u32 committer, Plan& plan) const noexcept
{
    const CipherSuite& suite  = *m_suite;
    const GroupConfig& config = m_config;
    const RatchetTree& now    = m_now.tree;
    const usize        hash   = suite.hash_bytes();
    const usize        count  = m_cache.size();
    cont::Array<Item>  all;
    cont::Array<u8>    chosen;
    cont::Array<u8>    leaves; // per leaf: 1 removed, 2 updated
    if (!all.try_reserve(count) || !chosen.try_reserve(count) || !leaves.try_reserve(now.leaves()))
    {
        return CryptoError::ProviderFailure;
    }
    chosen.resize(count, 0U);
    leaves.resize(now.leaves(), 0U);
    for (const Cached& entry : m_cache)
    {
        Item             item;
        cont::WireReader in(view(entry.bytes));
        (void)take_proposal(in, item.proposal);
        item.sender    = entry.sender;
        item.encoding  = view(entry.bytes);
        item.reference = cont::ConstSpan<u8>(entry.ref, hash);
        all.push_back(item);
    }
    const auto is = [&all](usize i, ProposalType type) { return all[i].proposal.type == code(type); };

    // The latest GroupContextExtensions every member supports.
    cont::ConstSpan<u8> extensions = view(m_now.extensions);
    for (usize i = count; i-- > 0U;)
    {
        if (!is(i, ProposalType::GroupContextExtensions) || !extensions_distinct(all[i].proposal.extensions))
        {
            continue;
        }
        bool supported = true;
        for (u32 leaf = 0U; supported && leaf < now.leaves(); ++leaf)
        {
            LeafNode node{};
            supported = !now.leaf_node(leaf, node) ||
                        supports_context(all[i].proposal.extensions, node.capabilities) == CryptoError::None;
        }
        if (supported)
        {
            chosen[i]  = 1U;
            extensions = all[i].proposal.extensions;
            break;
        }
    }
    const GroupContext next{suite.id, view(m_group_id), m_now.epoch + 1U, {}, {}, extensions};
    // The first Remove of each member but the committer, then the latest Update of each member neither removed nor
    // the committer.
    for (usize i = 0U; i < count; ++i)
    {
        const u32 target = all[i].proposal.remove;
        if (is(i, ProposalType::Remove) && target != committer && target < now.leaves() && !now.blank(2U * target) &&
            leaves[target] == 0U)
        {
            chosen[i]      = 1U;
            leaves[target] = 1U;
        }
    }
    for (usize i = count; i-- > 0U;)
    {
        const Item& item = all[i];
        const u32   from = item.sender.index;
        LeafNode    current{};
        if (!is(i, ProposalType::Update) || !is_member(item.sender) || from == committer ||
            !now.leaf_node(from, current) || leaves[from] != 0U)
        {
            continue;
        }
        const bool valid =
            check_leaf(next, item.proposal.update, LeafRole::Update, config.policy, *config.authentication) ==
                CryptoError::None &&
            verify_leaf_node(suite, *config.verifier, item.proposal.update, view(m_group_id), from) ==
                Verification::Valid &&
            !same_bytes(item.proposal.update.encryption_key, current.encryption_key);
        if (valid)
        {
            chosen[i]    = 1U;
            leaves[from] = 2U;
        }
    }
    // Valid Adds, the first of each signature key and none of a member the commit keeps; the first of each held PSK.
    struct Keyed
    {
        u16                 type = 0U;
        cont::ConstSpan<u8> key;
        usize               index = 0U;
    };
    cont::Array<Keyed>               candidates;
    cont::Array<cont::ConstSpan<u8>> members;
    if (!candidates.try_reserve(count) || !members.try_reserve(now.leaves()))
    {
        return CryptoError::ProviderFailure;
    }
    for (u32 leaf = 0U; leaf < now.leaves(); ++leaf)
    {
        LeafNode node{};
        if (now.leaf_node(leaf, node) && leaves[leaf] != 1U)
        {
            members.push_back(node.signature_key);
        }
    }
    std::sort(members.begin(), members.end(), bytes_less);
    for (usize i = 0U; i < count; ++i)
    {
        const Proposal& proposal = all[i].proposal;
        if (is(i, ProposalType::Add) &&
            check_key_package(next, proposal.add, *config.verifier, *config.authentication, config.policy) ==
                CryptoError::None &&
            !std::binary_search(members.begin(), members.end(), proposal.add.leaf_node.signature_key, bytes_less))
        {
            candidates.push_back({proposal.type, proposal.add.leaf_node.signature_key, i});
        }
        if (is(i, ProposalType::PreSharedKey) && check_psk_id(suite, proposal.psk) == CryptoError::None &&
            !psk_of(proposal.psk).empty())
        {
            candidates.push_back({proposal.type, all[i].encoding, i});
        }
    }
    // Adds keyed by signature key and PSKs by their identity's encoding, each type apart from the other.
    std::stable_sort(candidates.begin(), candidates.end(), [](const Keyed& a, const Keyed& b)
                     { return a.type != b.type ? a.type < b.type : bytes_less(a.key, b.key); });
    for (usize i = 0U; i < candidates.size(); ++i)
    {
        if (i == 0U || candidates[i].type != candidates[i - 1U].type ||
            !same_bytes(candidates[i].key, candidates[i - 1U].key))
        {
            chosen[candidates[i].index] = 1U;
        }
    }
    for (usize i = 0U; i < count; ++i)
    {
        if (chosen[i] != 0U && !plan.items.try_push_back(all[i]))
        {
            return CryptoError::ProviderFailure;
        }
    }
    return CryptoError::None;
}

namespace
{
// The body of a commit's ProposalOrRef list: each item by reference when it has one, else by value.
template <typename Items>
usize proposal_list_bytes(const Items& items) noexcept
{
    usize total = 0U;
    for (const auto& item : items)
    {
        total += 1U + (item.reference.empty() ? item.encoding.size() : vector_bytes(item.reference.size()));
    }
    return total;
}

template <typename Items>
void put_proposal_list(cont::WireWriter& out, const Items& items) noexcept
{
    for (const auto& item : items)
    {
        out.put_u8(item.reference.empty() ? 1U : 2U);
        if (item.reference.empty())
        {
            out.put_bytes(item.encoding);
        }
        else
        {
            put_vector(out, item.reference);
        }
    }
}

// An Extension list's body with the ratchet tree's extension first when `tree` is given, then `more`.
CryptoError group_info_extensions(const RatchetTree* tree, cont::ConstSpan<u8> more, cont::Array<u8>& out) noexcept
{
    out.clear();
    const usize tree_bytes = tree != nullptr ? tree->encoded_bytes() : 0U;
    const usize head       = tree != nullptr ? 2U + vector_bytes(tree_bytes) : 0U;
    if (tree != nullptr && vector_bytes(tree_bytes) == 0U)
    {
        return CryptoError::BadLength;
    }
    if (!out.try_reserve(head + more.size()))
    {
        return CryptoError::ProviderFailure;
    }
    out.resize(head + more.size());
    cont::WireWriter writer(cont::Span<u8>(out.data(), out.size()));
    if (tree != nullptr)
    {
        writer.put_u16(static_cast<u16>(ExtensionType::RatchetTree));
        put_varint(writer, tree_bytes);
        tree->encode(writer);
    }
    writer.put_bytes(more);
    return CryptoError::None;
}
} // namespace

CryptoError Group::commit(const Signer& signer, const CommitOptions& options, CommitResult& out) noexcept
{
    out.message.clear();
    out.welcome.clear();
    out.group_info.clear();
    out.staged.clear();
    CryptoError error = check_signer(signer);
    if (error != CryptoError::None)
    {
        return error;
    }
    const CipherSuite& suite = *m_suite;
    const usize        hash  = suite.hash_bytes();
    const u32          me    = own_leaf();
    cont::ConstSpan<u8> ignored;
    const bool framed = options.send.wire_format == WireFormat::PublicMessage ||
                        options.send.wire_format == WireFormat::PrivateMessage;
    if (!framed || !extensions_distinct(options.group_info_extensions) ||
        find_extension(options.group_info_extensions, static_cast<u16>(ExtensionType::RatchetTree), ignored))
    {
        return CryptoError::Malformed;
    }

    // The list: the cached proposals section 12.2 prefers, then the member's own by value.
    StagedCommit&   staged = out.staged;
    Plan            plan;
    cont::Array<u8> own; // the encodings of the proposals by value
    usize           own_total = 0U;
    for (const Proposal& proposal : options.proposals)
    {
        const usize bytes = proposal_bytes(proposal);
        if (bytes == 0U || proposal.type == code(ProposalType::Update))
        {
            return CryptoError::Malformed; // an Update by value would be the committer's own (section 12.2)
        }
        own_total += bytes;
    }
    if (options.pending)
    {
        error = select_pending(me, plan);
    }
    if (error == CryptoError::None &&
        (!own.try_reserve(own_total) || !plan.items.try_reserve(plan.items.size() + options.proposals.size())))
    {
        error = CryptoError::ProviderFailure;
    }
    for (usize i = 0U; error == CryptoError::None && i < options.proposals.size(); ++i)
    {
        const usize at = own.size();
        own.resize(at + proposal_bytes(options.proposals[i]));
        cont::WireWriter writer(cont::Span<u8>(own.data() + at, own.size() - at));
        put_proposal(writer, options.proposals[i]);
        Item item;
        item.proposal = options.proposals[i];
        item.sender   = {SenderType::Member, me};
        item.encoding = cont::ConstSpan<u8>(own.data() + at, own.size() - at);
        plan.items.push_back(item);
    }
    if (error == CryptoError::None)
    {
        error = stage_list(me, plan, staged);
    }

    // The path (section 12.4.1) under the provisional GroupContext, its keys staged.
    RatchetTree&    tree = staged.m_next.tree;
    const bool      path = options.path || plan.needs_path;
    cont::Array<u8> path_bytes;
    if (error == CryptoError::None && path)
    {
        LeafNode template_leaf{};
        (void)tree.leaf_node(me, template_leaf);
        const GroupContext provisional{suite.id, view(m_group_id), m_now.epoch + 1U, {}, view(m_now.confirmed),
                                       view(staged.m_next.extensions)};
        error = create_update_path(tree, me, signer, template_leaf, provisional, staged.added(), path_bytes,
                                   staged.m_update);
    }
    if (error == CryptoError::None)
    {
        error = check_new_tree(tree);
    }

    // The Commit framed, signed under the current GroupContext, and the new epoch derived and confirmed.
    const usize list  = proposal_list_bytes(plan.items);
    const usize total = vector_bytes(list) + 1U + path_bytes.size();
    if (error == CryptoError::None && vector_bytes(list) == 0U)
    {
        error = CryptoError::BadLength;
    }
    cont::Array<u8> body;
    if (error == CryptoError::None && !body.try_reserve(total))
    {
        error = CryptoError::ProviderFailure;
    }
    u8    signature[kMaxSignatureBytes] = {};
    usize written                       = 0U;
    u8    tag[kMaxHashBytes]            = {};
    if (error == CryptoError::None)
    {
        body.resize(total);
        cont::WireWriter writer(cont::Span<u8>(body.data(), total));
        put_varint(writer, list);
        put_proposal_list(writer, plan.items);
        writer.put_u8(path ? 1U : 0U);
        writer.put_bytes(view(path_bytes));
    }
    const FramedContent content{view(m_group_id), m_now.epoch, {SenderType::Member, me},
                                options.send.authenticated_data, ContentType::Commit, view(body)};
    if (error == CryptoError::None)
    {
        error = sign_own(signer, options.send.wire_format, content, signature, written);
    }
    if (error == CryptoError::None)
    {
        error = advance(staged, plan, options.send.wire_format, content, cont::ConstSpan<u8>(signature, written),
                        path ? staged.m_update.commit_secret() : cont::ConstSpan<u8>());
    }
    GroupEpoch& next = staged.m_next;
    if (error == CryptoError::None)
    {
        error = confirmation_tag(suite, next.secrets.get(EpochSecret::Confirmation), view(next.confirmed),
                                 cont::Span<u8>(tag, hash));
    }
    if (error == CryptoError::None)
    {
        error = confirm(next, cont::ConstSpan<u8>(tag, hash));
    }

    // The new epoch's GroupInfo (section 12.4.3), signed; the Welcome for the members the commit adds.
    cont::Array<u8> info_extensions;
    if (error == CryptoError::None)
    {
        error = group_info_extensions(options.tree_in_welcome ? &tree : nullptr, options.group_info_extensions,
                                      info_extensions);
    }
    GroupInfo info;
    info.context          = {suite.id,           view(m_group_id), next.epoch, tree.root_hash(),
                             view(next.confirmed), view(next.extensions)};
    info.extensions       = view(info_extensions);
    info.confirmation_tag = cont::ConstSpan<u8>(tag, hash);
    info.signer           = me;
    u8    info_signature[kMaxSignatureBytes] = {};
    usize info_written                       = 0U;
    if (error == CryptoError::None)
    {
        error = sign_group_info(suite, signer, info, info_signature, info_written);
    }
    info.signature          = cont::ConstSpan<u8>(info_signature, info_written);
    const usize info_bytes  = group_info_bytes(info);
    if (error == CryptoError::None && info_bytes == 0U)
    {
        error = CryptoError::BadLength;
    }
    if (error == CryptoError::None && !out.group_info.try_reserve(4U + info_bytes))
    {
        error = CryptoError::ProviderFailure;
    }
    if (error == CryptoError::None)
    {
        out.group_info.resize(4U + info_bytes);
        cont::WireWriter writer(cont::Span<u8>(out.group_info.data(), out.group_info.size()));
        put_message_header(writer, WireFormat::GroupInfo);
        put_group_info(writer, info);
    }
    cont::Array<WelcomeMember> members;
    if (error == CryptoError::None && !members.try_reserve(staged.m_added.size()))
    {
        error = CryptoError::ProviderFailure;
    }
    usize added = 0U;
    for (const Item& item : plan.items)
    {
        if (error != CryptoError::None || item.proposal.type != code(ProposalType::Add))
        {
            continue;
        }
        WelcomeMember member;
        member.package = item.proposal.add;
        if (path)
        {
            const std::optional<u32> ancestor =
                tree::common_ancestor(2U * me, 2U * staged.m_added[added], tree.leaves());
            member.path_secret = ancestor ? staged.m_update.path_secret(*ancestor) : cont::ConstSpan<u8>();
        }
        members.push_back(member);
        ++added;
    }
    cont::Array<u8> welcome;
    if (error == CryptoError::None && !members.empty())
    {
        error = seal_welcome(suite, next.secrets.get(EpochSecret::Joiner),
                             cont::ConstSpan<PreSharedKey>(plan.psks.data(), plan.psks.size()),
                             view(out.group_info).subspan(4U),
                             cont::ConstSpan<WelcomeMember>(members.data(), members.size()), welcome);
    }
    if (error == CryptoError::None && !members.empty())
    {
        if (out.welcome.try_reserve(4U + welcome.size()))
        {
            out.welcome.resize(4U + welcome.size());
            cont::WireWriter writer(cont::Span<u8>(out.welcome.data(), out.welcome.size()));
            put_message_header(writer, WireFormat::Welcome);
            writer.put_bytes(view(welcome));
        }
        else
        {
            error = CryptoError::ProviderFailure;
        }
    }
    if (error == CryptoError::None)
    {
        error = record(plan, me, path, staged);
    }
    // Protected last: a PrivateMessage spends the sender's handshake generation of the current epoch.
    if (error == CryptoError::None)
    {
        const ContentAuth auth{cont::ConstSpan<u8>(signature, written), cont::ConstSpan<u8>(tag, hash)};
        error = wrap(options.send, content, auth, out.message);
    }
    if (error != CryptoError::None)
    {
        out.message.clear();
        fresh_buffer(out.welcome, 0U);
        out.group_info.clear();
        staged.clear();
    }
    return error;
}

CryptoError Group::merge(StagedCommit& staged) noexcept
{
    CryptoError error = CryptoError::None;
    if (!staged.ready())
    {
        error = CryptoError::NotReady;
    }
    else if (staged.m_group != this || !ready() || staged.m_from_epoch != m_now.epoch)
    {
        error = CryptoError::OutOfWindow;
    }
    if (error != CryptoError::None)
    {
        staged.clear();
        return error;
    }
    if (staged.m_removed)
    {
        staged.clear();
        wipe(); // section 12.4.2: a removed member deletes its group state
        return CryptoError::None;
    }
    // The epoch that ends keeps its resumption PSK (decision M10).
    if (m_config.resumption_epochs != 0U)
    {
        if (m_resumption.size() < m_config.resumption_epochs)
        {
            m_resumption.push_back({}); // reserved when the group started
        }
        Resumption& slot = m_resumption[m_resumption_next];
        slot.epoch       = m_now.epoch;
        copy_bytes(m_now.secrets.get(EpochSecret::Resumption), slot.secret);
        m_resumption_next = (m_resumption_next + 1U) % m_config.resumption_epochs;
    }
    // The member's own leaf key when an Update of its leaf was committed, then the path's keys.
    LeafNode before{};
    LeafNode after{};
    (void)m_now.tree.leaf_node(own_leaf(), before);
    (void)staged.m_next.tree.leaf_node(own_leaf(), after);
    const usize kept = same_bytes(before.encryption_key, after.encryption_key) ? m_pending_leaves.size()
                                                                                : pending_leaf(after.encryption_key);
    if (kept < m_pending_leaves.size())
    {
        m_keys.replace_leaf(m_pending_leaves[kept]);
    }
    m_now.take(staged.m_next);
    m_keys.prune(m_now.tree);
    if (staged.m_has_path)
    {
        m_keys.apply(staged.m_update);
    }
    forget_epoch();
    staged.clear();
    return CryptoError::None;
}
} // namespace crd::crypto::mls
