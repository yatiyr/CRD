// crd-crypto -- MLS's ratchet tree and TreeKEM, RFC 9420 section 7 (NET.2; contract:
// docs/design/network-foundation.md#mls).
#include <crd/crypto/mls_tree.hpp>

#include "byte_spans.hpp"
#include "mls_detail.hpp"

#include <algorithm>
#include <utility>

namespace crd::crypto::mls
{
namespace
{
using detail::bytes_less;
using detail::LabelBuffer;
using detail::refuse;
using detail::repeats;
using detail::same_bytes;

constexpr cont::StringView kLeafLabel = "LeafNodeTBS";
constexpr cont::StringView kPathLabel = "UpdatePathNode";

// The longest leaf signature of the profile (P-384 ECDSA in DER).
constexpr usize kMaxSignatureBytes = 104U;

// Occupancy bits of a subtree: whether it holds a non-blank node (its resolution is not empty) and a non-blank leaf
// (Remove's truncation, section 12.1.3).
constexpr u8 kHoldsNode = 1U;
constexpr u8 kHoldsLeaf = 2U;

u32 big_endian_u32(const u8* bytes) noexcept
{
    return static_cast<u32>(bytes[0]) << 24U | static_cast<u32>(bytes[1]) << 16U | static_cast<u32>(bytes[2]) << 8U |
           static_cast<u32>(bytes[3]);
}

// The ancestor at level `l` of node `x` in the infinite array tree (any tree holding both).
u32 ancestor_at(u32 x, u32 l) noexcept
{
    return (x | ((u32{1} << l) - 1U)) & ~(u32{1} << l);
}

// Whether `node` lies in the subtree of `top` (itself included).
bool below(u32 node, u32 top) noexcept
{
    const u32 k = tree::level(top);
    return tree::level(node) <= k && ancestor_at(node, k) == top;
}

// The leaf indices a node's subtree spans.
struct LeafSpan
{
    u32 first = 0U;
    u32 last  = 0U;
};

LeafSpan leaves_under(u32 node) noexcept
{
    const u32 half = (u32{1} << tree::level(node)) - 1U;
    return {(node - half) / 2U, (node + half) / 2U};
}

// The child of `parent` whose subtree holds `node`, and the other one.
u32 child_toward(u32 parent, u32 node) noexcept
{
    return node < parent ? left_of(parent) : right_of(parent);
}

u32 child_away(u32 parent, u32 node) noexcept
{
    return node < parent ? right_of(parent) : left_of(parent);
}

// Whether a sorted list of leaf indices holds one in `span`.
bool any_within(cont::ConstSpan<u32> sorted, LeafSpan span) noexcept
{
    const auto at = std::lower_bound(sorted.begin(), sorted.end(), span.first);
    return at != sorted.end() && *at <= span.last;
}

bool listed(cont::ConstSpan<u32> list, u32 leaf) noexcept
{
    return std::find(list.begin(), list.end(), leaf) != list.end();
}

// A parent node's unmerged leaves as indices.
CryptoError unmerged_of(const ParentNode& node, cont::Array<u32>& out) noexcept
{
    out.clear();
    if (!out.try_reserve(node.unmerged_count()))
    {
        return CryptoError::ProviderFailure;
    }
    for (usize i = 0U; i < node.unmerged_count(); ++i)
    {
        out.push_back(node.unmerged(i));
    }
    return CryptoError::None;
}

// TreeHashInput (section 7.8): a leaf's {1, leaf_index, optional<LeafNode>} or a parent's {2, optional<ParentNode>,
// left_hash<V>, right_hash<V>}, the node's encoding given (empty for a blank node).
CryptoError tree_hash_of(const CipherSuite& suite, u32 node, cont::ConstSpan<u8> encoding, cont::ConstSpan<u8> left,
                         cont::ConstSpan<u8> right, cont::Span<u8> out) noexcept
{
    const bool  leaf  = tree::level(node) == 0U;
    const usize bytes = leaf ? 1U + 4U + 1U + encoding.size()
                             : 1U + 1U + encoding.size() + vector_bytes(left.size()) + vector_bytes(right.size());
    LabelBuffer      input(bytes);
    cont::WireWriter writer(input.span());
    writer.put_u8(static_cast<u8>(leaf ? NodeType::Leaf : NodeType::Parent));
    if (leaf)
    {
        writer.put_u32(node / 2U);
    }
    writer.put_u8(encoding.empty() ? 0U : 1U);
    writer.put_bytes(encoding);
    if (!leaf)
    {
        put_vector(writer, left);
        put_vector(writer, right);
    }
    return kdf_hash(suite.kdf, input.span(), out);
}

// A parent node's encoding into owned bytes.
CryptoError encode_parent(const ParentNode& node, cont::Array<u8>& out) noexcept
{
    const usize bytes = parent_node_bytes(node);
    if (bytes == 0U)
    {
        return CryptoError::BadLength;
    }
    out.clear();
    if (!out.try_reserve(bytes))
    {
        return CryptoError::ProviderFailure;
    }
    out.resize(bytes);
    cont::WireWriter writer(cont::Span<u8>(out.data(), out.size()));
    put_parent_node(writer, node);
    return CryptoError::None;
}

// The body of an unmerged-leaves list.
CryptoError encode_leaf_list(cont::ConstSpan<u32> leaves, cont::Array<u8>& out) noexcept
{
    out.clear();
    if (!out.try_reserve(leaves.size() * 4U))
    {
        return CryptoError::ProviderFailure;
    }
    out.resize(leaves.size() * 4U);
    cont::WireWriter writer(cont::Span<u8>(out.data(), out.size()));
    for (const u32 leaf : leaves)
    {
        writer.put_u32(leaf);
    }
    return CryptoError::None;
}

CryptoError encode_leaf(const LeafNode& node, cont::Array<u8>& out) noexcept
{
    const usize bytes = leaf_node_bytes(node);
    if (bytes == 0U)
    {
        return CryptoError::BadLength;
    }
    out.clear();
    if (!out.try_reserve(bytes))
    {
        return CryptoError::ProviderFailure;
    }
    out.resize(bytes);
    cont::WireWriter writer(cont::Span<u8>(out.data(), out.size()));
    put_leaf_node(writer, node);
    return CryptoError::None;
}

// LeafNodeTBS (section 7.2): the fields, then the group id and leaf index of an update or commit leaf.
usize leaf_tbs_bytes(const LeafNode& node, cont::ConstSpan<u8> group_id) noexcept
{
    const usize fields = leaf_fields_bytes(node);
    if (fields == 0U || node.source == LeafNodeSource::KeyPackage)
    {
        return fields;
    }
    const usize id = vector_bytes(group_id.size());
    return id == 0U ? 0U : fields + id + 4U;
}

void put_leaf_tbs(cont::WireWriter& out, const LeafNode& node, cont::ConstSpan<u8> group_id, u32 leaf_index) noexcept
{
    put_leaf_fields(out, node);
    if (node.source != LeafNodeSource::KeyPackage)
    {
        put_vector(out, group_id);
        out.put_u32(leaf_index);
    }
}

// The key a path secret gives its node: DeriveKeyPair(DeriveSecret(path_secret, "node")), checked against the
// public key the tree or the path holds.
CryptoError node_key(const CipherSuite& suite, cont::ConstSpan<u8> path_secret, cont::ConstSpan<u8> expected,
                     HpkeKey& out) noexcept
{
    u8                   node_bytes[kMaxHashBytes] = {};
    const cont::Span<u8> node_secret(node_bytes, suite.hash_bytes());
    CryptoError          error = derive_secret(suite, path_secret, "node", node_secret);
    if (error == CryptoError::None)
    {
        error = out.derive(suite.kem, node_secret);
    }
    secure_zero(node_secret);
    if (error != CryptoError::None)
    {
        return error;
    }
    u8                   public_bytes[kMaxNodeKeyBytes] = {};
    const cont::Span<u8> public_key(public_bytes, hpke_sizes(suite.kem).public_key);
    error = out.public_key(public_key);
    if (error == CryptoError::None && !same_bytes(public_key, expected))
    {
        error = CryptoError::BadKey;
    }
    if (error != CryptoError::None)
    {
        out.clear();
    }
    return error;
}

// The parsed nodes of an UpdatePath, up to kMaxDepth: Malformed when there are more.
struct PathNodes
{
    UpdatePathNode node[tree::kMaxDepth];
    usize          count = 0U;
};

bool take_path_nodes(cont::ConstSpan<u8> body, PathNodes& out) noexcept
{
    cont::WireReader in(body);
    out.count = 0U;
    while (!in.done())
    {
        if (out.count == tree::kMaxDepth || !take_update_path_node(in, out.node[out.count]))
        {
            return false;
        }
        ++out.count;
    }
    return true;
}

// The `index`th HPKECiphertext of a node's list, or false.
bool ciphertext_at(cont::ConstSpan<u8> list, usize index, usize& count, HpkeCiphertext& out) noexcept
{
    cont::WireReader in(list);
    HpkeCiphertext   item{};
    bool             found = false;
    count                  = 0U;
    while (!in.done())
    {
        if (!take_hpke_ciphertext(in, item))
        {
            return false;
        }
        if (count == index)
        {
            out   = item;
            found = true;
        }
        ++count;
    }
    return found;
}
} // namespace

// ---- Nodes ----

u32 ParentNode::unmerged(usize i) const noexcept
{
    return big_endian_u32(unmerged_leaves.data() + 4U * i);
}

bool take_parent_node(cont::WireReader& in, ParentNode& out) noexcept
{
    out = {};
    return take_vector(in, out.encryption_key) && take_vector(in, out.parent_hash) &&
           take_vector(in, out.unmerged_leaves) && out.unmerged_leaves.size() % 4U == 0U;
}

usize parent_node_bytes(const ParentNode& node) noexcept
{
    const usize key    = vector_bytes(node.encryption_key.size());
    const usize hash   = vector_bytes(node.parent_hash.size());
    const usize leaves = vector_bytes(node.unmerged_leaves.size());
    return key == 0U || hash == 0U || leaves == 0U || node.unmerged_leaves.size() % 4U != 0U ? 0U
                                                                                               : key + hash + leaves;
}

void put_parent_node(cont::WireWriter& out, const ParentNode& node) noexcept
{
    put_vector(out, node.encryption_key);
    put_vector(out, node.parent_hash);
    put_vector(out, node.unmerged_leaves);
}

// ---- Leaf node signatures ----

CryptoError sign_leaf_node(const CipherSuite& suite, const Signer& signer, const LeafNode& node,
                           cont::ConstSpan<u8> group_id, u32 leaf_index, cont::Span<u8> signature,
                           usize& written) noexcept
{
    written           = 0U;
    const usize bytes = leaf_tbs_bytes(node, group_id);
    if (bytes == 0U)
    {
        return refuse(CryptoError::BadLength, signature);
    }
    LabelBuffer      tbs(bytes);
    cont::WireWriter writer(tbs.span());
    put_leaf_tbs(writer, node, group_id, leaf_index);
    return sign_with_label(suite, signer, kLeafLabel, tbs.span(), signature, written);
}

Verification verify_leaf_node(const CipherSuite& suite, const SignatureVerifier& verifier, const LeafNode& node,
                              cont::ConstSpan<u8> group_id, u32 leaf_index) noexcept
{
    const usize bytes = leaf_tbs_bytes(node, group_id);
    if (bytes == 0U)
    {
        return Verification::Invalid;
    }
    LabelBuffer      tbs(bytes);
    cont::WireWriter writer(tbs.span());
    put_leaf_tbs(writer, node, group_id, leaf_index);
    return verify_with_label(suite, verifier, node.signature_key, kLeafLabel, tbs.span(), node.signature);
}

// ---- TreeKEM's secrets ----

PathUpdate::~PathUpdate() noexcept
{
    secure_zero(cont::Span<u8>(&secrets[0][0], sizeof(secrets)));
    secure_zero(commit);
}

cont::ConstSpan<u8> PathUpdate::path_secret(u32 node) const noexcept
{
    const u32 l = tree::level(node);
    return l != 0U && l <= tree::kMaxDepth && learned[l] && below(2U * leaf, node)
               ? cont::ConstSpan<u8>(secrets[l], hash_bytes)
               : cont::ConstSpan<u8>();
}

cont::ConstSpan<u8> PathUpdate::commit_secret() const noexcept
{
    return cont::ConstSpan<u8>(commit, hash_bytes);
}

void PathUpdate::clear() noexcept
{
    leaf_key.clear();
    for (u32 l = 0U; l <= tree::kMaxDepth; ++l)
    {
        keys[l].clear();
        learned[l] = false;
    }
    secure_zero(cont::Span<u8>(&secrets[0][0], sizeof(secrets)));
    secure_zero(commit);
    leaf       = 0U;
    replaced   = 0U;
    hash_bytes = 0U;
}

namespace
{
CryptoError refuse_update(CryptoError error, PathUpdate& update) noexcept
{
    update.clear();
    return error;
}
} // namespace

// ---- The ratchet tree ----

struct RatchetTree::PathPlan
{
    u32                 nodes[tree::kMaxDepth] = {};
    cont::ConstSpan<u8> keys[tree::kMaxDepth];
    usize               count = 0U;
    u32                 sender = 0U;
    // hashes[i] is the parent_hash field of nodes[i] (empty for the top one), hashes[count] the leaf's.
    u8 hashes[tree::kMaxDepth + 1U][kMaxHashBytes] = {};

    [[nodiscard]] cont::ConstSpan<u8> field(usize i, usize hash_bytes) const noexcept
    {
        return cont::ConstSpan<u8>(hashes[i], i + 1U < count || (i == count && count != 0U) ? hash_bytes : 0U);
    }
};

RatchetTree::~RatchetTree() noexcept
{
    wipe();
}

RatchetTree::RatchetTree(RatchetTree&& other) noexcept
    : m_suite(other.m_suite), m_leaves(other.m_leaves), m_nodes(std::move(other.m_nodes)),
      m_hashes(std::move(other.m_hashes)), m_occupied(std::move(other.m_occupied))
{
    other.m_leaves = 0U;
}

RatchetTree& RatchetTree::operator=(RatchetTree&& other) noexcept
{
    if (this != &other)
    {
        wipe();
        m_suite        = other.m_suite;
        m_leaves       = other.m_leaves;
        m_nodes        = std::move(other.m_nodes);
        m_hashes       = std::move(other.m_hashes);
        m_occupied     = std::move(other.m_occupied);
        other.m_leaves = 0U;
    }
    return *this;
}

void RatchetTree::wipe() noexcept
{
    m_nodes.clear();
    m_hashes.clear();
    m_occupied.clear();
    m_leaves = 0U;
}

CryptoError RatchetTree::resize(u32 leaves) noexcept
{
    const u32 width = tree::node_width(leaves);
    const u32 old   = node_count();
    if (width > old && (!m_nodes.try_reserve(width) || !m_hashes.try_reserve(usize{width} * m_suite.hash_bytes()) ||
                        !m_occupied.try_reserve(width)))
    {
        return CryptoError::ProviderFailure;
    }
    m_nodes.resize(width);
    m_hashes.resize(usize{width} * m_suite.hash_bytes());
    m_occupied.resize(width);
    m_leaves = leaves;
    // A grown tree's new right half and root are blank: their hashes are computed bottom-up. A halved tree keeps
    // its left half, whose nodes and hashes are unchanged.
    for (u32 l = 0U; width > old && (u32{1} << l) <= leaves; ++l)
    {
        const u32 step = u32{1} << (l + 1U);
        for (u32 node = (u32{1} << l) - 1U; node < width; node += step)
        {
            const CryptoError error = node >= old ? rehash(node) : CryptoError::None;
            if (error != CryptoError::None)
            {
                return error;
            }
        }
    }
    return CryptoError::None;
}

CryptoError RatchetTree::rehash(u32 node) noexcept
{
    const usize          nh = m_suite.hash_bytes();
    const cont::Span<u8> out(m_hashes.data() + usize{node} * nh, nh);
    const Stored&        stored = m_nodes[node];
    const cont::ConstSpan<u8> encoding(stored.bytes.data(), stored.bytes.size());
    if (tree::level(node) == 0U)
    {
        m_occupied[node] = static_cast<u8>(encoding.empty() ? 0U : kHoldsNode | kHoldsLeaf);
        return tree_hash_of(m_suite, node, encoding, {}, {}, out);
    }
    const u32 left   = left_of(node);
    const u32 right  = right_of(node);
    const u8  own    = encoding.empty() ? u8{0} : kHoldsNode;
    m_occupied[node] = static_cast<u8>(own | m_occupied[left] | m_occupied[right]);
    return tree_hash_of(m_suite, node, encoding, tree_hash(left), tree_hash(right), out);
}

CryptoError RatchetTree::rehash_path(u32 node) noexcept
{
    const u32   root  = m_leaves - 1U;
    CryptoError error = rehash(node);
    for (u32 at = node; error == CryptoError::None && at != root;)
    {
        at    = ancestor_at(at, tree::level(at) + 1U);
        error = rehash(at);
    }
    if (error != CryptoError::None)
    {
        wipe(); // a provider failure: no tree is better than one whose hashes are stale
    }
    return error;
}

CryptoError RatchetTree::rehash_all() noexcept
{
    const u32 width = node_count();
    for (u32 l = 0U; (u32{1} << l) <= m_leaves; ++l)
    {
        const u32 step = u32{1} << (l + 1U);
        for (u32 node = (u32{1} << l) - 1U; node < width; node += step)
        {
            const CryptoError error = rehash(node);
            if (error != CryptoError::None)
            {
                wipe();
                return error;
            }
        }
    }
    return CryptoError::None;
}

CryptoError RatchetTree::create(const CipherSuite& suite, const LeafNode& creator) noexcept
{
    wipe();
    m_suite = suite;
    cont::Array<u8> encoding;
    CryptoError     error = encode_leaf(creator, encoding);
    if (error == CryptoError::None)
    {
        error = resize(1U);
    }
    if (error == CryptoError::None)
    {
        m_nodes[0].bytes = std::move(encoding);
        error            = rehash_all();
    }
    if (error != CryptoError::None)
    {
        wipe();
    }
    return error;
}

CryptoError RatchetTree::decode(const CipherSuite& suite, cont::ConstSpan<u8> encoded) noexcept
{
    wipe();
    m_suite = suite;
    cont::WireReader    outer(encoded);
    cont::ConstSpan<u8> body;
    if (!take_vector(outer, body) || !outer.done())
    {
        return CryptoError::Malformed;
    }
    // First the syntax and the count, then the nodes stored.
    const u32        max_nodes = tree::node_width(kMaxSecretTreeLeaves);
    cont::WireReader items(body);
    u32              count        = 0U;
    bool             last_present = false;
    while (!items.done())
    {
        if (count == max_nodes)
        {
            return CryptoError::BadLength;
        }
        bool present = false;
        u8   type    = 0U;
        if (!take_presence(items, present))
        {
            return CryptoError::Malformed;
        }
        if (present)
        {
            const NodeType expected = count % 2U == 0U ? NodeType::Leaf : NodeType::Parent;
            LeafNode       leaf{};
            ParentNode     parent{};
            if (!items.take_u8(type) || type != static_cast<u8>(expected) ||
                !(expected == NodeType::Leaf ? take_leaf_node(items, leaf) : take_parent_node(items, parent)))
            {
                return CryptoError::Malformed;
            }
        }
        last_present = present;
        ++count;
    }
    if (!last_present)
    {
        return CryptoError::Malformed; // empty, or a trailing blank node (section 12.4.3.3)
    }
    u32 leaves = 1U;
    while (tree::node_width(leaves) < count)
    {
        leaves *= 2U;
    }
    CryptoError error = resize(leaves);
    items             = cont::WireReader(body);
    for (u32 node = 0U; error == CryptoError::None && node < count; ++node)
    {
        bool present = false;
        u8   type    = 0U;
        (void)take_presence(items, present);
        if (!present)
        {
            continue;
        }
        (void)items.take_u8(type);
        const cont::ConstSpan<u8> start = items.rest();
        LeafNode                  leaf{};
        ParentNode                parent{};
        (void)(node % 2U == 0U ? take_leaf_node(items, leaf) : take_parent_node(items, parent));
        const cont::ConstSpan<u8> encoding = start.first(start.size() - items.rest().size());
        cont::Array<u8>&          bytes    = m_nodes[node].bytes;
        if (!bytes.try_reserve(encoding.size()))
        {
            error = CryptoError::ProviderFailure;
            break;
        }
        bytes.resize(encoding.size());
        copy_bytes(encoding, bytes.data());
    }
    if (error == CryptoError::None)
    {
        error = rehash_all();
    }
    if (error != CryptoError::None)
    {
        wipe();
    }
    return error;
}

CryptoError RatchetTree::copy_from(const RatchetTree& other) noexcept
{
    if (this == &other)
    {
        return CryptoError::None;
    }
    wipe();
    m_suite           = other.m_suite;
    const u32   width = other.node_count();
    const usize nh    = m_suite.hash_bytes();
    if (!m_nodes.try_reserve(width) || !m_hashes.try_reserve(usize{width} * nh) || !m_occupied.try_reserve(width))
    {
        wipe();
        return CryptoError::ProviderFailure;
    }
    m_nodes.resize(width);
    for (u32 node = 0U; node < width; ++node)
    {
        const cont::Array<u8>& from = other.m_nodes[node].bytes;
        cont::Array<u8>&       to   = m_nodes[node].bytes;
        if (!to.try_reserve(from.size()))
        {
            wipe();
            return CryptoError::ProviderFailure;
        }
        to.resize(from.size());
        copy_bytes(cont::ConstSpan<u8>(from.data(), from.size()), to.data());
    }
    m_hashes.resize(other.m_hashes.size());
    copy_bytes(cont::ConstSpan<u8>(other.m_hashes.data(), other.m_hashes.size()), m_hashes.data());
    m_occupied.resize(width);
    copy_bytes(cont::ConstSpan<u8>(other.m_occupied.data(), width), m_occupied.data());
    m_leaves = other.m_leaves;
    return CryptoError::None;
}

usize RatchetTree::encoded_body(u32& end) const noexcept
{
    usize body = 0U;
    usize last = 0U;
    end        = 0U;
    for (u32 node = 0U; node < node_count(); ++node)
    {
        body += blank(node) ? 1U : 2U + m_nodes[node].bytes.size();
        if (!blank(node))
        {
            last = body;
            end  = node + 1U;
        }
    }
    return last;
}

usize RatchetTree::encoded_bytes() const noexcept
{
    u32 end = 0U;
    return vector_bytes(encoded_body(end));
}

void RatchetTree::encode(cont::WireWriter& out) const noexcept
{
    u32 end = 0U;
    put_varint(out, encoded_body(end));
    for (u32 node = 0U; node < end; ++node)
    {
        if (blank(node))
        {
            out.put_u8(0U);
            continue;
        }
        out.put_u8(1U);
        out.put_u8(static_cast<u8>(tree::level(node) == 0U ? NodeType::Leaf : NodeType::Parent));
        out.put_bytes(cont::ConstSpan<u8>(m_nodes[node].bytes.data(), m_nodes[node].bytes.size()));
    }
}

bool RatchetTree::blank(u32 node) const noexcept
{
    return node >= node_count() || m_nodes[node].bytes.empty();
}

bool RatchetTree::leaf_node(u32 leaf, LeafNode& out) const noexcept
{
    out = {};
    if (leaf >= m_leaves || blank(2U * leaf))
    {
        return false;
    }
    const cont::Array<u8>& bytes = m_nodes[2U * leaf].bytes;
    cont::WireReader       in(cont::ConstSpan<u8>(bytes.data(), bytes.size()));
    return take_leaf_node(in, out);
}

bool RatchetTree::parent_node(u32 node, ParentNode& out) const noexcept
{
    out = {};
    if (tree::level(node) == 0U || blank(node))
    {
        return false;
    }
    const cont::Array<u8>& bytes = m_nodes[node].bytes;
    cont::WireReader       in(cont::ConstSpan<u8>(bytes.data(), bytes.size()));
    return take_parent_node(in, out);
}

cont::ConstSpan<u8> RatchetTree::encryption_key(u32 node) const noexcept
{
    LeafNode   leaf{};
    ParentNode parent{};
    if (tree::level(node) == 0U)
    {
        return leaf_node(node / 2U, leaf) ? leaf.encryption_key : cont::ConstSpan<u8>();
    }
    return parent_node(node, parent) ? parent.encryption_key : cont::ConstSpan<u8>();
}

cont::ConstSpan<u8> RatchetTree::tree_hash(u32 node) const noexcept
{
    const usize nh = m_suite.hash_bytes();
    return node < node_count() ? cont::ConstSpan<u8>(m_hashes.data() + usize{node} * nh, nh) : cont::ConstSpan<u8>();
}

cont::ConstSpan<u8> RatchetTree::root_hash() const noexcept
{
    return ready() ? tree_hash(m_leaves - 1U) : cont::ConstSpan<u8>();
}

// ---- Resolutions and paths ----

template <typename Visit>
void RatchetTree::visit_resolution(u32 node, cont::ConstSpan<u32> excluded, Visit& visit) const noexcept
{
    if ((m_occupied[node] & kHoldsNode) == 0U)
    {
        return;
    }
    if (blank(node))
    {
        visit_resolution(left_of(node), excluded, visit);
        visit_resolution(right_of(node), excluded, visit);
        return;
    }
    if (tree::level(node) == 0U)
    {
        if (!listed(excluded, node / 2U))
        {
            visit(node);
        }
        return;
    }
    visit(node);
    ParentNode parent{};
    (void)parent_node(node, parent);
    for (usize i = 0U; i < parent.unmerged_count(); ++i)
    {
        const u32 leaf = parent.unmerged(i);
        if (leaf < m_leaves && !listed(excluded, leaf))
        {
            visit(2U * leaf);
        }
    }
}

CryptoError RatchetTree::resolution(u32 node, cont::ConstSpan<u32> excluded, cont::Array<u32>& out) const noexcept
{
    if (node >= node_count())
    {
        return CryptoError::None;
    }
    usize count   = 0U;
    auto  counter = [&count](u32) { ++count; };
    visit_resolution(node, excluded, counter);
    if (!out.try_reserve(out.size() + count))
    {
        return CryptoError::ProviderFailure;
    }
    auto append = [&out](u32 entry) { out.push_back(entry); };
    visit_resolution(node, excluded, append);
    return CryptoError::None;
}

usize RatchetTree::filtered_direct_path(u32 leaf, cont::Span<u32> out) const noexcept
{
    u32         path[tree::kMaxDepth] = {};
    const usize depth                 = leaf < m_leaves ? tree::direct_path(2U * leaf, m_leaves, path) : 0U;
    usize       count                 = 0U;
    for (usize i = 0U; i < depth; ++i)
    {
        if ((m_occupied[child_away(path[i], 2U * leaf)] & kHoldsNode) == 0U)
        {
            continue;
        }
        if (count == out.size())
        {
            return 0U;
        }
        out[count++] = path[i];
    }
    return count;
}

// ---- Hashes ----

CryptoError RatchetTree::hash_without(u32 node, cont::ConstSpan<u32> excluded, cont::Span<u8> out) const noexcept
{
    if (!any_within(excluded, leaves_under(node)))
    {
        copy_bytes(tree_hash(node), out.data());
        return CryptoError::None;
    }
    if (tree::level(node) == 0U)
    {
        return tree_hash_of(m_suite, node, {}, {}, {}, out); // an excluded leaf hashes as blank
    }
    const usize          nh                         = m_suite.hash_bytes();
    u8                   left_bytes[kMaxHashBytes]  = {};
    u8                   right_bytes[kMaxHashBytes] = {};
    const cont::Span<u8> left(left_bytes, nh);
    const cont::Span<u8> right(right_bytes, nh);
    CryptoError          error = hash_without(left_of(node), excluded, left);
    if (error == CryptoError::None)
    {
        error = hash_without(right_of(node), excluded, right);
    }
    ParentNode parent{};
    if (error != CryptoError::None)
    {
        return error;
    }
    if (!parent_node(node, parent))
    {
        return tree_hash_of(m_suite, node, {}, left, right, out);
    }
    // The node with the excluded leaves dropped from its unmerged list.
    cont::Array<u32> kept;
    cont::Array<u8>  list;
    cont::Array<u8>  encoding;
    error = unmerged_of(parent, kept);
    if (error == CryptoError::None)
    {
        const u32* end = std::remove_if(kept.data(), kept.data() + kept.size(), [excluded](u32 leaf) {
            return std::binary_search(excluded.begin(), excluded.end(), leaf);
        });
        kept.resize(static_cast<usize>(end - kept.data()));
        error = encode_leaf_list(cont::ConstSpan<u32>(kept.data(), kept.size()), list);
    }
    if (error == CryptoError::None)
    {
        parent.unmerged_leaves = cont::ConstSpan<u8>(list.data(), list.size());
        error                  = encode_parent(parent, encoding);
    }
    if (error != CryptoError::None)
    {
        return error;
    }
    return tree_hash_of(m_suite, node, cont::ConstSpan<u8>(encoding.data(), encoding.size()), left, right, out);
}

CryptoError RatchetTree::parent_hash(cont::ConstSpan<u8> encryption_key, cont::ConstSpan<u8> parent_hash,
                                     cont::ConstSpan<u8> sibling_hash, cont::Span<u8> out) const noexcept
{
    // ParentHashInput (section 7.9): {encryption_key<V>, parent_hash<V>, original_sibling_tree_hash<V>}.
    const usize key     = vector_bytes(encryption_key.size());
    const usize above   = vector_bytes(parent_hash.size());
    const usize sibling = vector_bytes(sibling_hash.size());
    if (key == 0U || above == 0U || sibling == 0U)
    {
        return CryptoError::BadLength;
    }
    LabelBuffer      input(key + above + sibling);
    cont::WireWriter writer(input.span());
    put_vector(writer, encryption_key);
    put_vector(writer, parent_hash);
    put_vector(writer, sibling_hash);
    return kdf_hash(m_suite.kdf, input.span(), out);
}

// ---- Validation ----

namespace
{
bool unmerged_lists(const ParentNode& node, u32 leaf) noexcept
{
    usize low  = 0U;
    usize high = node.unmerged_count();
    while (low < high)
    {
        const usize mid = low + (high - low) / 2U;
        const u32   at  = node.unmerged(mid);
        if (at == leaf)
        {
            return true;
        }
        if (at < leaf)
        {
            low = mid + 1U;
        }
        else
        {
            high = mid;
        }
    }
    return false;
}
} // namespace

CryptoError RatchetTree::check_unmerged() const noexcept
{
    const u32 width = node_count();
    // Each list strictly increasing, its leaves present and below the node.
    for (u32 node = 1U; node < width; node += 2U)
    {
        ParentNode parent{};
        if (!parent_node(node, parent))
        {
            continue;
        }
        for (usize i = 0U; i < parent.unmerged_count(); ++i)
        {
            const u32 leaf = parent.unmerged(i);
            if ((i != 0U && leaf <= parent.unmerged(i - 1U)) || leaf >= m_leaves || blank(2U * leaf) ||
                !below(2U * leaf, node))
            {
                return CryptoError::Malformed;
            }
        }
    }
    // Then every non-blank node between such a leaf and the node lists it too (section 12.4.3.1).
    for (u32 node = 1U; node < width; node += 2U)
    {
        ParentNode parent{};
        if (!parent_node(node, parent))
        {
            continue;
        }
        for (usize i = 0U; i < parent.unmerged_count(); ++i)
        {
            const u32 leaf = parent.unmerged(i);
            for (u32 l = 1U; l < tree::level(node); ++l)
            {
                ParentNode between{};
                if (parent_node(ancestor_at(2U * leaf, l), between) && !unmerged_lists(between, leaf))
                {
                    return CryptoError::Malformed;
                }
            }
        }
    }
    return CryptoError::None;
}

CryptoError RatchetTree::check_unique_keys() const noexcept
{
    cont::Array<cont::ConstSpan<u8>> encryption;
    cont::Array<cont::ConstSpan<u8>> signature;
    if (!encryption.try_reserve(node_count()) || !signature.try_reserve(m_leaves))
    {
        return CryptoError::ProviderFailure;
    }
    for (u32 node = 0U; node < node_count(); ++node)
    {
        LeafNode leaf{};
        if (!blank(node))
        {
            encryption.push_back(encryption_key(node));
        }
        if (tree::level(node) == 0U && leaf_node(node / 2U, leaf))
        {
            signature.push_back(leaf.signature_key);
        }
    }
    return repeats(encryption) || repeats(signature) ? CryptoError::BadKey : CryptoError::None;
}

CryptoError RatchetTree::parent_hash_valid_via(u32 parent, u32 child, bool& valid) const noexcept
{
    valid = false;
    ParentNode node{};
    (void)parent_node(parent, node);
    cont::Array<u32> unmerged;
    cont::Array<u32> resolved;
    CryptoError      error = unmerged_of(node, unmerged);
    if (error == CryptoError::None)
    {
        error = resolution(child, {}, resolved);
    }
    if (error != CryptoError::None)
    {
        return error;
    }
    // The child's resolution must be one descendant D and the parent's unmerged leaves below the child (section
    // 7.9.2); check_unmerged has made both lists sets.
    const LeafSpan span     = leaves_under(child);
    const usize    under_us = static_cast<usize>(std::count_if(unmerged.begin(), unmerged.end(), [span](u32 leaf) {
        return leaf >= span.first && leaf <= span.last;
    }));
    if (resolved.size() != under_us + 1U)
    {
        return CryptoError::None;
    }
    u32   d     = 0U;
    usize found = 0U;
    for (const u32 entry : resolved)
    {
        if (tree::level(entry) == 0U && std::binary_search(unmerged.begin(), unmerged.end(), entry / 2U))
        {
            continue;
        }
        d = entry;
        ++found;
    }
    if (found != 1U)
    {
        return CryptoError::None;
    }
    LeafNode            leaf{};
    ParentNode          descendant{};
    cont::ConstSpan<u8> claimed;
    if (tree::level(d) == 0U)
    {
        (void)leaf_node(d / 2U, leaf);
        claimed = leaf.parent_hash; // empty unless a commit set the leaf: no chain starts at another
    }
    else
    {
        (void)parent_node(d, descendant);
        claimed = descendant.parent_hash;
    }
    const usize          nh                            = m_suite.hash_bytes();
    u8                   sibling_bytes[kMaxHashBytes]  = {};
    u8                   expected_bytes[kMaxHashBytes] = {};
    const cont::Span<u8> sibling(sibling_bytes, nh);
    const cont::Span<u8> expected(expected_bytes, nh);
    error = hash_without(child == left_of(parent) ? right_of(parent) : left_of(parent),
                         cont::ConstSpan<u32>(unmerged.data(), unmerged.size()), sibling);
    if (error == CryptoError::None)
    {
        error = parent_hash(node.encryption_key, node.parent_hash, sibling, expected);
    }
    valid = error == CryptoError::None && same_bytes(claimed, expected);
    return error;
}

CryptoError RatchetTree::check_parent_hashes() const noexcept
{
    for (u32 node = 1U; node < node_count(); node += 2U)
    {
        if (blank(node))
        {
            continue;
        }
        bool        via_left  = false;
        bool        via_right = false;
        CryptoError error     = parent_hash_valid_via(node, left_of(node), via_left);
        if (error == CryptoError::None)
        {
            error = parent_hash_valid_via(node, right_of(node), via_right);
        }
        if (error != CryptoError::None)
        {
            return error;
        }
        if (via_left == via_right)
        {
            return CryptoError::Forged; // valid relative to no descendant, or to two
        }
    }
    return CryptoError::None;
}

CryptoError RatchetTree::validate(const SignatureVerifier& verifier, cont::ConstSpan<u8> group_id) const noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    CryptoError error = check_unmerged();
    if (error == CryptoError::None)
    {
        error = check_unique_keys();
    }
    for (u32 leaf = 0U; error == CryptoError::None && leaf < m_leaves; ++leaf)
    {
        LeafNode node{};
        if (leaf_node(leaf, node) && verify_leaf_node(m_suite, verifier, node, group_id, leaf) != Verification::Valid)
        {
            error = CryptoError::Forged;
        }
    }
    return error == CryptoError::None ? check_parent_hashes() : error;
}

// ---- The proposals' changes ----

CryptoError RatchetTree::add(const LeafNode& leaf, u32& index) noexcept
{
    index = 0U;
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    cont::Array<u8> encoding;
    CryptoError     error = encode_leaf(leaf, encoding);
    if (error != CryptoError::None)
    {
        return error;
    }
    u32 at = 0U;
    while (at < m_leaves && !blank(2U * at))
    {
        ++at;
    }
    const bool grow = at == m_leaves;
    if (grow && m_leaves >= kMaxSecretTreeLeaves)
    {
        return CryptoError::BadLength;
    }
    if (grow)
    {
        error = resize(2U * m_leaves);
    }
    // The non-blank nodes above list the new leaf as unmerged (section 12.1.1), staged before anything changes.
    struct Staged
    {
        u32             node = 0U;
        cont::Array<u8> bytes;
    };
    Staged           staged[tree::kMaxDepth];
    usize            count                 = 0U;
    u32              path[tree::kMaxDepth] = {};
    const usize      depth = error == CryptoError::None ? tree::direct_path(2U * at, m_leaves, path) : 0U;
    cont::Array<u32> list;
    cont::Array<u8>  body;
    for (usize i = 0U; error == CryptoError::None && i < depth; ++i)
    {
        ParentNode parent{};
        if (!parent_node(path[i], parent))
        {
            continue;
        }
        error = unmerged_of(parent, list);
        if (error == CryptoError::None && !list.try_reserve(list.size() + 1U))
        {
            error = CryptoError::ProviderFailure;
        }
        if (error == CryptoError::None)
        {
            list.push_back(at);
            u32* begin = list.data();
            u32* last  = begin + list.size() - 1U;
            std::rotate(std::lower_bound(begin, last, at), last, last + 1);
            error = encode_leaf_list(cont::ConstSpan<u32>(list.data(), list.size()), body);
        }
        if (error == CryptoError::None)
        {
            parent.unmerged_leaves = cont::ConstSpan<u8>(body.data(), body.size());
            staged[count].node     = path[i];
            error                  = encode_parent(parent, staged[count].bytes);
            ++count;
        }
    }
    if (error != CryptoError::None)
    {
        if (grow)
        {
            wipe(); // the grown tree cannot be left half built
        }
        return error;
    }
    m_nodes[2U * at].bytes = std::move(encoding);
    for (usize i = 0U; i < count; ++i)
    {
        m_nodes[staged[i].node].bytes = std::move(staged[i].bytes);
    }
    error = rehash_path(2U * at);
    index = error == CryptoError::None ? at : 0U;
    return error;
}

void RatchetTree::blank_direct_path(u32 leaf) noexcept
{
    u32         path[tree::kMaxDepth] = {};
    const usize depth                 = tree::direct_path(2U * leaf, m_leaves, path);
    for (usize i = 0U; i < depth; ++i)
    {
        m_nodes[path[i]].bytes = cont::Array<u8>();
    }
}

CryptoError RatchetTree::update(u32 leaf, const LeafNode& node) noexcept
{
    if (leaf >= m_leaves || blank(2U * leaf))
    {
        return CryptoError::OutOfWindow;
    }
    cont::Array<u8>   encoding;
    const CryptoError error = encode_leaf(node, encoding);
    if (error != CryptoError::None)
    {
        return error;
    }
    m_nodes[2U * leaf].bytes = std::move(encoding);
    blank_direct_path(leaf);
    return rehash_path(2U * leaf);
}

CryptoError RatchetTree::remove(u32 leaf) noexcept
{
    if (leaf >= m_leaves || blank(2U * leaf))
    {
        return CryptoError::OutOfWindow;
    }
    m_nodes[2U * leaf].bytes = cont::Array<u8>();
    blank_direct_path(leaf);
    CryptoError error = rehash_path(2U * leaf);
    // Truncation (section 12.1.3): while the right half holds no member, the left half is the tree.
    while (error == CryptoError::None && m_leaves > 1U && (m_occupied[right_of(m_leaves - 1U)] & kHoldsLeaf) == 0U)
    {
        error = resize(m_leaves / 2U);
    }
    return error;
}

// ---- Merging a path ----

CryptoError RatchetTree::plan_parent_hashes(PathPlan& plan) const noexcept
{
    const usize nh = m_suite.hash_bytes();
    for (usize i = plan.count; i-- > 0U;)
    {
        const u32            sibling = child_away(plan.nodes[i], 2U * plan.sender);
        const cont::Span<u8> lower(plan.hashes[i == 0U ? plan.count : i - 1U], nh);
        const CryptoError    error = parent_hash(plan.keys[i], plan.field(i, nh), tree_hash(sibling), lower);
        if (error != CryptoError::None)
        {
            return error;
        }
    }
    return CryptoError::None;
}

CryptoError RatchetTree::apply_path(u32 sender, const LeafNode& leaf, const PathPlan& plan) noexcept
{
    const usize     nh = m_suite.hash_bytes();
    cont::Array<u8> leaf_bytes;
    cont::Array<u8> staged[tree::kMaxDepth];
    CryptoError     error = encode_leaf(leaf, leaf_bytes);
    for (usize i = 0U; error == CryptoError::None && i < plan.count; ++i)
    {
        const ParentNode node{plan.keys[i], plan.field(i, nh), {}};
        error = encode_parent(node, staged[i]);
    }
    if (error != CryptoError::None)
    {
        return error;
    }
    blank_direct_path(sender);
    m_nodes[2U * sender].bytes = std::move(leaf_bytes);
    for (usize i = 0U; i < plan.count; ++i)
    {
        m_nodes[plan.nodes[i]].bytes = std::move(staged[i]);
    }
    return rehash_path(2U * sender);
}

CryptoError RatchetTree::check_new_keys(u32 sender, cont::ConstSpan<u8> leaf_key, const PathPlan& plan) const noexcept
{
    cont::Array<cont::ConstSpan<u8>> fresh;
    if (!fresh.try_reserve(plan.count + 1U))
    {
        return CryptoError::ProviderFailure;
    }
    fresh.push_back(leaf_key);
    for (usize i = 0U; i < plan.count; ++i)
    {
        fresh.push_back(plan.keys[i]);
    }
    if (repeats(fresh))
    {
        return CryptoError::BadKey;
    }
    // No key of the path in a node the merge keeps (section 12.4.2): any but the sender's leaf and direct path.
    for (u32 node = 0U; node < node_count(); ++node)
    {
        if (!blank(node) && !below(2U * sender, node) &&
            std::binary_search(fresh.begin(), fresh.end(), encryption_key(node), bytes_less))
        {
            return CryptoError::BadKey;
        }
    }
    return CryptoError::None;
}

CryptoError RatchetTree::merge_path(u32 sender, const UpdatePath& path) noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    LeafNode current{};
    if (!leaf_node(sender, current))
    {
        return CryptoError::OutOfWindow;
    }
    PathPlan  plan;
    PathNodes parsed;
    plan.sender = sender;
    plan.count  = filtered_direct_path(sender, plan.nodes);
    if (path.leaf_node.source != LeafNodeSource::Commit || !take_path_nodes(path.nodes, parsed) ||
        parsed.count != plan.count)
    {
        return CryptoError::Malformed;
    }
    for (usize i = 0U; i < plan.count; ++i)
    {
        plan.keys[i] = parsed.node[i].encryption_key;
    }
    CryptoError error = plan_parent_hashes(plan);
    if (error != CryptoError::None)
    {
        return error;
    }
    if (!same_bytes(path.leaf_node.parent_hash, plan.field(plan.count, m_suite.hash_bytes())))
    {
        return CryptoError::Forged;
    }
    if (same_bytes(path.leaf_node.encryption_key, current.encryption_key))
    {
        return CryptoError::BadKey;
    }
    error = check_new_keys(sender, path.leaf_node.encryption_key, plan);
    return error == CryptoError::None ? apply_path(sender, path.leaf_node, plan) : error;
}

// ---- A member's keys ----

CryptoError PathKeys::init(const CipherSuite& suite, u32 leaf, cont::ConstSpan<u8> leaf_private) noexcept
{
    for (HpkeKey& key : m_keys)
    {
        key.clear();
    }
    m_suite = suite;
    m_leaf  = leaf;
    return leaf < kMaxSecretTreeLeaves ? m_leaf_key.from_private(suite.kem, leaf_private) : CryptoError::OutOfWindow;
}

const HpkeKey* PathKeys::key_of(u32 node) const noexcept
{
    if (!ready())
    {
        return nullptr;
    }
    if (node == 2U * m_leaf)
    {
        return &m_leaf_key;
    }
    const u32 l = tree::level(node);
    return l != 0U && l <= tree::kMaxDepth && below(2U * m_leaf, node) && m_keys[l].ready() ? &m_keys[l] : nullptr;
}

bool PathKeys::holds(u32 node) const noexcept
{
    return key_of(node) != nullptr;
}

CryptoError PathKeys::path_check(const RatchetTree& tree, u32 node, cont::ConstSpan<u8> path_secret) const noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    const u32 l = tree::level(node);
    if (l == 0U || node >= tree.node_count() || !below(2U * m_leaf, node))
    {
        return CryptoError::OutOfWindow;
    }
    return path_secret.size() == m_suite.hash_bytes() ? CryptoError::None : CryptoError::BadLength;
}

CryptoError PathKeys::set_path_secret(const RatchetTree& tree, u32 node, cont::ConstSpan<u8> path_secret) noexcept
{
    CryptoError error = path_check(tree, node, path_secret);
    HpkeKey     key;
    if (error == CryptoError::None)
    {
        error = node_key(m_suite, path_secret, tree.encryption_key(node), key);
    }
    if (error == CryptoError::None)
    {
        m_keys[tree::level(node)] = std::move(key);
    }
    return error;
}

CryptoError PathKeys::implant(const RatchetTree& tree, u32 node, cont::ConstSpan<u8> path_secret) noexcept
{
    CryptoError error = path_check(tree, node, path_secret);
    if (error != CryptoError::None)
    {
        return error;
    }
    // Every key derived before any is kept: the node's, then each non-blank node's above it.
    const usize          nh = m_suite.hash_bytes();
    HpkeKey              staged[tree::kMaxDepth + 1U];
    u8                   secret_bytes[2][kMaxHashBytes] = {};
    cont::Span<u8>       secret(secret_bytes[0], nh);
    cont::Span<u8>       next(secret_bytes[1], nh);
    const u32            top = tree::level(tree.leaves() - 1U);
    copy_bytes(path_secret, secret.data());
    error = node_key(m_suite, secret, tree.encryption_key(node), staged[tree::level(node)]);
    for (u32 l = tree::level(node) + 1U; error == CryptoError::None && l <= top; ++l)
    {
        const u32 above = ancestor_at(node, l);
        if (tree.blank(above))
        {
            continue;
        }
        error = derive_secret(m_suite, secret, "path", next);
        std::swap(secret, next);
        if (error == CryptoError::None)
        {
            error = node_key(m_suite, secret, tree.encryption_key(above), staged[l]);
        }
    }
    secure_zero(cont::Span<u8>(&secret_bytes[0][0], sizeof(secret_bytes)));
    for (u32 l = tree::level(node); error == CryptoError::None && l <= top; ++l)
    {
        if (staged[l].ready())
        {
            m_keys[l] = std::move(staged[l]);
        }
    }
    return error;
}

void PathKeys::prune(const RatchetTree& tree) noexcept
{
    u8                   public_bytes[kMaxNodeKeyBytes] = {};
    const cont::Span<u8> public_key(public_bytes, hpke_sizes(m_suite.kem).public_key);
    for (u32 l = 1U; l <= tree::kMaxDepth; ++l)
    {
        const u32 node = ancestor_at(2U * m_leaf, l);
        if (m_keys[l].ready() && (node >= tree.node_count() || m_keys[l].public_key(public_key) != CryptoError::None ||
                                  !same_bytes(public_key, tree.encryption_key(node))))
        {
            m_keys[l].clear();
        }
    }
}

void PathKeys::apply(PathUpdate& update) noexcept
{
    if (update.leaf_key.ready())
    {
        m_leaf_key = std::move(update.leaf_key);
    }
    for (u32 l = update.replaced == 0U ? 1U : update.replaced; l <= tree::kMaxDepth; ++l)
    {
        m_keys[l] = std::move(update.keys[l]);
        update.keys[l].clear();
    }
    update.leaf_key.clear();
}

void PathKeys::replace_leaf(HpkeKey& key) noexcept
{
    m_leaf_key = std::move(key);
    key.clear();
}

// ---- TreeKEM ----

CryptoError create_update_path(RatchetTree& tree, u32 sender, const Signer& signer, const LeafNode& leaf_template,
                               const GroupContext& context, cont::ConstSpan<u32> excluded, cont::Array<u8>& path,
                               PathUpdate& update) noexcept
{
    update.clear();
    path.clear();
    const CipherSuite& suite = tree.suite();
    const usize        nh    = suite.hash_bytes();
    const usize        npk   = hpke_sizes(suite.kem).public_key;
    if (!tree.ready())
    {
        return CryptoError::NotReady;
    }
    if (sender >= tree.leaves() || tree.blank(2U * sender))
    {
        return CryptoError::OutOfWindow;
    }
    if (signer.algorithm() != suite.signature || signature_sizes(suite.signature).signature > kMaxSignatureBytes)
    {
        return CryptoError::Unsupported;
    }
    RatchetTree::PathPlan plan;
    plan.sender = sender;
    plan.count  = tree.filtered_direct_path(sender, plan.nodes);
    update.leaf       = sender;
    update.hash_bytes = nh;
    update.replaced   = 1U;

    // The keys: a fresh leaf key, a random first path secret and each one after it (section 7.4).
    u8          leaf_public[kMaxNodeKeyBytes]                 = {};
    u8          node_public[tree::kMaxDepth][kMaxNodeKeyBytes] = {};
    u8          secret_bytes[kMaxHashBytes]                   = {};
    CryptoError error = update.leaf_key.generate(suite.kem);
    if (error == CryptoError::None)
    {
        error = update.leaf_key.public_key(cont::Span<u8>(leaf_public, npk));
    }
    if (error == CryptoError::None)
    {
        error = random_bytes(cont::Span<u8>(secret_bytes, nh));
    }
    for (usize i = 0U; error == CryptoError::None && i < plan.count; ++i)
    {
        const u32 l = tree::level(plan.nodes[i]);
        if (i != 0U)
        {
            error = derive_secret(suite, cont::ConstSpan<u8>(secret_bytes, nh), "path",
                                  cont::Span<u8>(update.secrets[l], nh));
        }
        else
        {
            copy_bytes(cont::ConstSpan<u8>(secret_bytes, nh), update.secrets[l]);
        }
        update.learned[l] = true;
        copy_bytes(cont::ConstSpan<u8>(update.secrets[l], nh), secret_bytes);
        HpkeKey& key = update.keys[l];
        if (error == CryptoError::None)
        {
            u8                   node_secret[kMaxHashBytes] = {};
            const cont::Span<u8> node_span(node_secret, nh);
            error = derive_secret(suite, cont::ConstSpan<u8>(update.secrets[l], nh), "node", node_span);
            if (error == CryptoError::None)
            {
                error = key.derive(suite.kem, node_span);
            }
            secure_zero(node_span);
        }
        if (error == CryptoError::None)
        {
            error = key.public_key(cont::Span<u8>(node_public[i], npk));
        }
        plan.keys[i] = cont::ConstSpan<u8>(node_public[i], npk);
    }
    if (error == CryptoError::None)
    {
        error = derive_secret(suite, cont::ConstSpan<u8>(secret_bytes, nh), "path", cont::Span<u8>(update.commit, nh));
    }
    secure_zero(cont::Span<u8>(secret_bytes, nh));
    if (error == CryptoError::None)
    {
        error = tree.plan_parent_hashes(plan);
    }

    // The new leaf (section 7.5), signed and merged.
    LeafNode leaf       = leaf_template;
    u8       signature[kMaxSignatureBytes] = {};
    usize    written                       = 0U;
    leaf.encryption_key = cont::ConstSpan<u8>(leaf_public, npk);
    leaf.source         = LeafNodeSource::Commit;
    leaf.parent_hash    = plan.field(plan.count, nh);
    leaf.not_before     = 0U;
    leaf.not_after      = 0U;
    leaf.signature      = {};
    if (error == CryptoError::None)
    {
        error = sign_leaf_node(suite, signer, leaf, context.group_id, sender, signature, written);
    }
    leaf.signature = cont::ConstSpan<u8>(signature, written);
    if (error == CryptoError::None)
    {
        error = tree.apply_path(sender, leaf, plan);
    }
    if (error == CryptoError::None)
    {
        // The template may view the leaf the merge just replaced: the path writes the leaf as the tree now holds it.
        (void)tree.leaf_node(sender, leaf);
    }

    // The provisional GroupContext over the new tree, and each path secret to its copath child's resolution.
    GroupContext provisional = context;
    provisional.tree_hash    = tree.root_hash();
    const usize context_size = group_context_bytes(provisional);
    if (error == CryptoError::None && context_size == 0U)
    {
        error = CryptoError::BadLength;
    }
    LabelBuffer      encoded_context(error == CryptoError::None ? context_size : 0U);
    cont::WireWriter context_writer(encoded_context.span());
    if (error == CryptoError::None)
    {
        put_group_context(context_writer, provisional);
    }
    cont::Array<u32> targets;
    usize            first[tree::kMaxDepth + 1U] = {};
    for (usize i = 0U; error == CryptoError::None && i < plan.count; ++i)
    {
        first[i] = targets.size();
        error    = tree.resolution(child_away(plan.nodes[i], 2U * sender), excluded, targets);
    }
    first[plan.count]       = targets.size();
    const usize ciphertext  = vector_bytes(npk) + vector_bytes(nh + kHpkeTagBytes);
    usize       nodes_bytes = 0U;
    for (usize i = 0U; i < plan.count; ++i)
    {
        nodes_bytes += vector_bytes(npk) + vector_bytes((first[i + 1U] - first[i]) * ciphertext);
    }
    const usize leaf_bytes = leaf_node_bytes(leaf);
    if (error == CryptoError::None && (leaf_bytes == 0U || vector_bytes(nodes_bytes) == 0U))
    {
        error = CryptoError::BadLength;
    }
    if (error == CryptoError::None && !path.try_reserve(leaf_bytes + vector_bytes(nodes_bytes)))
    {
        error = CryptoError::ProviderFailure;
    }
    if (error == CryptoError::None)
    {
        path.resize(leaf_bytes + vector_bytes(nodes_bytes));
        cont::WireWriter writer(cont::Span<u8>(path.data(), path.size()));
        put_leaf_node(writer, leaf);
        put_varint(writer, nodes_bytes);
        for (usize i = 0U; error == CryptoError::None && i < plan.count; ++i)
        {
            put_vector(writer, plan.keys[i]);
            put_varint(writer, (first[i + 1U] - first[i]) * ciphertext);
            const cont::ConstSpan<u8> secret(update.secrets[tree::level(plan.nodes[i])], nh);
            for (usize t = first[i]; error == CryptoError::None && t < first[i + 1U]; ++t)
            {
                u8 kem_output[kMaxNodeKeyBytes]           = {};
                u8 sealed[kMaxHashBytes + kHpkeTagBytes] = {};
                error = encrypt_with_label(suite, tree.encryption_key(targets[t]), kPathLabel, encoded_context.span(),
                                           secret, cont::Span<u8>(kem_output, npk),
                                           cont::Span<u8>(sealed, nh + kHpkeTagBytes));
                put_vector(writer, cont::ConstSpan<u8>(kem_output, npk));
                put_vector(writer, cont::ConstSpan<u8>(sealed, nh + kHpkeTagBytes));
            }
        }
    }
    if (error != CryptoError::None)
    {
        path.clear();
        return refuse_update(error, update);
    }
    return CryptoError::None;
}

CryptoError process_update_path(const PathKeys& keys, const RatchetTree& tree, u32 sender, const UpdatePath& path,
                                cont::ConstSpan<u8> context, cont::ConstSpan<u32> excluded, PathUpdate& update,
                                const HpkeKey* leaf_key) noexcept
{
    update.clear();
    if (!keys.ready() || !tree.ready())
    {
        return CryptoError::NotReady;
    }
    const CipherSuite& suite = tree.suite();
    const usize        nh    = suite.hash_bytes();
    const u32          me    = 2U * keys.leaf();
    u32                nodes[tree::kMaxDepth] = {};
    const usize        count = tree.filtered_direct_path(sender, nodes);
    PathNodes          parsed;
    if (sender == keys.leaf() || !take_path_nodes(path.nodes, parsed) || parsed.count != count)
    {
        return CryptoError::Malformed;
    }
    // The lowest node of the path above the member, the child of it toward the member and the node of that child's
    // resolution whose key the member holds (section 7.5).
    usize lowest = 0U;
    while (lowest < count && !below(me, nodes[lowest]))
    {
        ++lowest;
    }
    if (lowest == count)
    {
        return CryptoError::Malformed;
    }
    cont::Array<u32> resolved;
    CryptoError      error = tree.resolution(child_toward(nodes[lowest], me), excluded, resolved);
    if (error != CryptoError::None)
    {
        return error;
    }
    const auto key_for = [&](u32 node) -> const HpkeKey*
    { return node == me && leaf_key != nullptr ? leaf_key : keys.key_of(node); };
    usize held = 0U;
    while (held < resolved.size() && key_for(resolved[held]) == nullptr)
    {
        ++held;
    }
    usize          listed_count = 0U;
    HpkeCiphertext sealed{};
    if (held == resolved.size() ||
        !ciphertext_at(parsed.node[lowest].encrypted_path_secrets, held, listed_count, sealed) ||
        listed_count != resolved.size() || sealed.ciphertext.size() != nh + kHpkeTagBytes)
    {
        return CryptoError::Malformed;
    }
    const u32 start = tree::level(nodes[lowest]);
    error           = decrypt_with_label(suite, *key_for(resolved[held]), kPathLabel, context, sealed.kem_output,
                                         sealed.ciphertext, cont::Span<u8>(update.secrets[start], nh));
    // Each path secret above it, each key checked against the path's (section 7.4).
    for (usize i = lowest; error == CryptoError::None && i < count; ++i)
    {
        const u32 l = tree::level(nodes[i]);
        if (i != lowest)
        {
            const u32 previous = tree::level(nodes[i - 1U]);
            error = derive_secret(suite, cont::ConstSpan<u8>(update.secrets[previous], nh), "path",
                                  cont::Span<u8>(update.secrets[l], nh));
        }
        update.learned[l] = error == CryptoError::None;
        if (error == CryptoError::None)
        {
            error = node_key(suite, cont::ConstSpan<u8>(update.secrets[l], nh), parsed.node[i].encryption_key,
                             update.keys[l]);
        }
    }
    if (error == CryptoError::None)
    {
        error = derive_secret(suite, cont::ConstSpan<u8>(update.secrets[tree::level(nodes[count - 1U])], nh), "path",
                              cont::Span<u8>(update.commit, nh));
    }
    if (error != CryptoError::None)
    {
        return refuse_update(error, update);
    }
    update.leaf       = keys.leaf();
    update.replaced   = start;
    update.hash_bytes = nh;
    return CryptoError::None;
}
} // namespace crd::crypto::mls
