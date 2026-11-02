#pragma once

// crd-crypto -- MLS's ratchet tree and TreeKEM, RFC 9420 section 7 (NET.2; contract:
// docs/design/network-foundation.md#mls, M3 layer 7): the tree of the group's leaf nodes and parent nodes, its
// resolutions and filtered direct paths (section 4.1), tree hashes (7.8), parent hashes and their verification (7.9),
// the checks a joiner runs on a received tree (7.3 and 12.4.3.1), the tree changes of Add, Update and Remove (7.7 and
// 12.1), and TreeKEM: creating an UpdatePath, merging it and decrypting its path secret (7.4 to 7.6, 12.4).
//
//   ownership   a RatchetTree owns each node's encoding (the messages it came from are views and do not outlive it);
//               LeafNode and ParentNode are views of those bytes, valid until the node changes.
//   hashes      every node's tree hash is held and recomputed along the nodes a change touches, so the root hash is a
//               read; a tree of n leaves holds 2n - 1 hashes.
//   atomicity   every refusal leaves a tree as it was, except create_update_path, which builds on the commit's
//               provisional tree (a refused commit discards it). Path keys change only through PathKeys::apply, once
//               the commit they come from is confirmed.
//   allocation  a tree allocates per node it stores and the handshake operations allocate their scratch; no message
//               path runs here. Memory that cannot be had is ProviderFailure.
//   bounds      at most kMaxSecretTreeLeaves leaves (decision M5), so a size read off the wire never reaches further.
//   secrets     path secrets, node secrets and private keys are zeroed when replaced or destroyed (section 7.5).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/hpke.hpp>
#include <crd/crypto/mls.hpp>
#include <crd/crypto/mls_message.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/verifier.hpp>

namespace crd::crypto::mls
{
// ---- Nodes (section 7.1) ----

enum class NodeType : u8
{
    Leaf   = 1U,
    Parent = 2U,
};

// A parent node: its public key, the parent hash of the node above it on the path that set it, and the leaves below
// it that do not hold its private key, as the body of a list of big-endian uint32 leaf indices in increasing order.
struct ParentNode
{
    cont::ConstSpan<u8> encryption_key;
    cont::ConstSpan<u8> parent_hash;
    cont::ConstSpan<u8> unmerged_leaves;

    [[nodiscard]] usize unmerged_count() const noexcept { return unmerged_leaves.size() / 4U; }
    [[nodiscard]] u32   unmerged(usize i) const noexcept;
};

// Syntax only, as the decoders of mls_message.hpp: the list holds whole indices (their order and meaning are the
// tree's checks).
[[nodiscard]] bool  take_parent_node(cont::WireReader& in, ParentNode& out) noexcept;
[[nodiscard]] usize parent_node_bytes(const ParentNode& node) noexcept;
void                put_parent_node(cont::WireWriter& out, const ParentNode& node) noexcept;

// ---- Leaf node signatures (section 7.2) ----

// signature = SignWithLabel(key, "LeafNodeTBS", LeafNodeTBS): the leaf's fields before its signature and, for a leaf
// of an update or a commit, the group id and the leaf's index. Laid out from the fields, so a leaf being built signs
// as one decoded verifies. `signature` and `written` as sign_with_label.
[[nodiscard]] CryptoError sign_leaf_node(const CipherSuite& suite, const Signer& signer, const LeafNode& node,
                                         cont::ConstSpan<u8> group_id, u32 leaf_index, cont::Span<u8> signature,
                                         usize& written) noexcept;
// Under the leaf's own signature key.
[[nodiscard]] Verification verify_leaf_node(const CipherSuite& suite, const SignatureVerifier& verifier,
                                            const LeafNode& node, cont::ConstSpan<u8> group_id,
                                            u32 leaf_index) noexcept;

// ---- TreeKEM's secrets (sections 7.4 and 7.5) ----

// The keys one commit's path gives a member, held until the commit is confirmed (PathKeys::apply), and its secrets:
// the path secret of each node of the path the member learned (the committer's for a Welcome, section 12.4.1) and the
// commit secret. Zeroed when destroyed.
struct PathUpdate
{
    PathUpdate() noexcept = default;
    ~PathUpdate() noexcept;
    PathUpdate(const PathUpdate&)            = delete;
    PathUpdate& operator=(const PathUpdate&) = delete;
    PathUpdate(PathUpdate&&)                 = delete;
    PathUpdate& operator=(PathUpdate&&)      = delete;

    // The path secret of `node` (empty when the member did not learn it) and the commit secret, Nh bytes.
    [[nodiscard]] cont::ConstSpan<u8> path_secret(u32 node) const noexcept;
    [[nodiscard]] cont::ConstSpan<u8> commit_secret() const noexcept;

    HpkeKey leaf_key;                     // a committer's new leaf key; not ready for a receiver
    HpkeKey keys[tree::kMaxDepth + 1U];   // by level on the member's direct path
    u8      secrets[tree::kMaxDepth + 1U][kMaxHashBytes] = {};
    bool    learned[tree::kMaxDepth + 1U]                = {};
    u8      commit[kMaxHashBytes]                        = {};
    u32     leaf      = 0U;
    u32     replaced  = 0U; // the lowest level whose keys the update replaces
    usize   hash_bytes = 0U;
};

// ---- The ratchet tree ----

// A group's ratchet tree (section 7): 2^d leaves, each blank or a member's LeafNode, and the parents above them,
// each blank or a ParentNode; nodes are numbered as in mls.hpp's tree math.
class RatchetTree
{
public:
    RatchetTree() noexcept = default;
    ~RatchetTree() noexcept;
    RatchetTree(const RatchetTree&)            = delete;
    RatchetTree& operator=(const RatchetTree&) = delete;
    RatchetTree(RatchetTree&& other) noexcept;
    RatchetTree& operator=(RatchetTree&& other) noexcept;

    // A new group's tree: its creator's leaf alone.
    [[nodiscard]] CryptoError create(const CipherSuite& suite, const LeafNode& creator) noexcept;
    // The whole of `encoded` as the ratchet_tree extension's optional<Node> ratchet_tree<V> (section 12.4.3.3): every
    // node of the type its position holds (a leaf at even positions, a parent at odd ones), the last one present, the
    // tree extended with blanks to the next full size. Malformed for any other encoding, BadLength past
    // kMaxSecretTreeLeaves leaves. Semantic checks are `validate`'s.
    [[nodiscard]] CryptoError decode(const CipherSuite& suite, cont::ConstSpan<u8> encoded) noexcept;
    // An exact copy (a commit's provisional tree).
    [[nodiscard]] CryptoError copy_from(const RatchetTree& other) noexcept;

    // The extension's encoding: the nodes up to the last one present (the sender MUST NOT send trailing blanks).
    [[nodiscard]] usize encoded_bytes() const noexcept;
    void                encode(cont::WireWriter& out) const noexcept;

    [[nodiscard]] bool               ready() const noexcept { return m_leaves != 0U; }
    [[nodiscard]] const CipherSuite& suite() const noexcept { return m_suite; }
    [[nodiscard]] u32                leaves() const noexcept { return m_leaves; }
    [[nodiscard]] u32                node_count() const noexcept { return tree::node_width(m_leaves); }

    // Whether a node inside the tree is blank (true outside it).
    [[nodiscard]] bool blank(u32 node) const noexcept;
    // A non-blank leaf's node and a non-blank parent's (false when blank or outside).
    [[nodiscard]] bool leaf_node(u32 leaf, LeafNode& out) const noexcept;
    [[nodiscard]] bool parent_node(u32 node, ParentNode& out) const noexcept;
    // A non-blank node's HPKE public key; empty otherwise.
    [[nodiscard]] cont::ConstSpan<u8> encryption_key(u32 node) const noexcept;

    // A node's tree hash (section 7.8), Nh bytes; empty outside the tree. The root's is the GroupContext's tree_hash.
    [[nodiscard]] cont::ConstSpan<u8> tree_hash(u32 node) const noexcept;
    [[nodiscard]] cont::ConstSpan<u8> root_hash() const noexcept;

    // The resolution of a node (section 4.1.1), appended to `out` as node indices: a non-blank node and its unmerged
    // leaves, nothing for a blank leaf, a blank parent's children's resolutions in turn. Leaves in `excluded` (the
    // members a commit adds, whom its path does not encrypt to) are left out. ProviderFailure without memory.
    [[nodiscard]] CryptoError resolution(u32 node, cont::ConstSpan<u32> excluded, cont::Array<u32>& out) const noexcept;
    // A leaf's filtered direct path (section 4.1.2): the nodes of its direct path whose copath child's resolution is
    // not empty, from the bottom; the count written into `out` (kMaxDepth entries hold any), 0 for a leaf outside.
    [[nodiscard]] usize filtered_direct_path(u32 leaf, cont::Span<u32> out) const noexcept;

    // The checks a member runs on a tree it joins (sections 7.3, 7.9.2 and 12.4.3.1), in this order:
    //   Malformed   a parent's unmerged leaves not strictly increasing, or one that is outside the tree, blank, not
    //               below it, or missing from a non-blank node between it and the parent;
    //   BadKey      an encryption key held by two nodes, or a signature key by two leaves;
    //   Forged      a leaf whose signature does not verify (with `group_id` for update and commit leaves), or a
    //               non-blank parent that is not parent-hash valid relative to exactly one descendant.
    // The tree hash against the GroupInfo and each leaf's capabilities and lifetime are the group's checks.
    [[nodiscard]] CryptoError validate(const SignatureVerifier& verifier, cont::ConstSpan<u8> group_id) const noexcept;

    // The tree changes of the proposals (section 12.1). Add puts the leaf in the leftmost blank leaf, extending the
    // tree when there is none, and lists it as unmerged in every non-blank node above it (`index` its leaf index);
    // Update replaces a member's leaf and blanks its direct path; Remove blanks a member's leaf and direct path and
    // halves the tree while its right half is blank. A blank or outside leaf is OutOfWindow; a leaf that does not
    // encode, BadLength. Leaf nodes are checked by the caller (their source and signature, sections 7.3 and 12.2).
    [[nodiscard]] CryptoError add(const LeafNode& leaf, u32& index) noexcept;
    [[nodiscard]] CryptoError update(u32 leaf, const LeafNode& node) noexcept;
    [[nodiscard]] CryptoError remove(u32 leaf) noexcept;

    // Merges a received UpdatePath from `sender` (section 7.5) into the tree its commit's proposals produced: the
    // sender's direct path blanked, each node of its filtered direct path set to the path's public key with no
    // unmerged leaves, its leaf replaced. Before anything changes: Malformed when the path has not one node per node of
    // the filtered direct path or the leaf's source is not commit; Forged when the leaf's parent_hash is not the parent
    // hash its path computes (7.9.2); BadKey when the new leaf keeps the sender's encryption key or a key of the path
    // is held by a node outside it (12.4.2). An external committer's leaf must be placed first (add).
    [[nodiscard]] CryptoError merge_path(u32 sender, const UpdatePath& path) noexcept;

private:
    struct Stored
    {
        cont::Array<u8> bytes; // the node's encoding, empty when blank
    };

    void                      blank_direct_path(u32 leaf) noexcept;
    [[nodiscard]] CryptoError resize(u32 leaves) noexcept;
    // The bytes of the encoding's body (its nodes up to the last one present) and the count of those nodes.
    [[nodiscard]] usize encoded_body(u32& end) const noexcept;
    // Recomputes the hash and occupancy of `node` and of every node above it.
    [[nodiscard]] CryptoError rehash_path(u32 node) noexcept;
    [[nodiscard]] CryptoError rehash_all() noexcept;
    [[nodiscard]] CryptoError rehash(u32 node) noexcept;
    // The tree hash of `node` in the tree with the leaves of `excluded` (sorted) blanked and dropped from every
    // unmerged list (section 7.9: the original sibling tree hash).
    [[nodiscard]] CryptoError hash_without(u32 node, cont::ConstSpan<u32> excluded, cont::Span<u8> out) const noexcept;
    [[nodiscard]] CryptoError parent_hash(cont::ConstSpan<u8> encryption_key, cont::ConstSpan<u8> parent_hash,
                                          cont::ConstSpan<u8> sibling_hash, cont::Span<u8> out) const noexcept;
    [[nodiscard]] CryptoError check_unmerged() const noexcept;
    [[nodiscard]] CryptoError check_unique_keys() const noexcept;
    [[nodiscard]] CryptoError check_parent_hashes() const noexcept;
    // Whether `parent` is parent-hash valid relative to a descendant below `child` (section 7.9.2).
    [[nodiscard]] CryptoError parent_hash_valid_via(u32 parent, u32 child, bool& valid) const noexcept;
    template <typename Visit>
    void visit_resolution(u32 node, cont::ConstSpan<u32> excluded, Visit& visit) const noexcept;
    void                      wipe() noexcept;

    friend CryptoError create_update_path(RatchetTree&, u32, const Signer&, const LeafNode&, const GroupContext&,
                                          cont::ConstSpan<u32>, cont::Array<u8>&, PathUpdate&) noexcept;
    struct PathPlan; // a path's nodes, their new keys and the parent hashes (mls_tree.cpp)
    // The parent hashes the plan's keys give the nodes of `sender`'s filtered direct path and its leaf, computed root
    // first over the tree's current copath hashes.
    [[nodiscard]] CryptoError plan_parent_hashes(PathPlan& plan) const noexcept;
    [[nodiscard]] CryptoError apply_path(u32 sender, const LeafNode& leaf, const PathPlan& plan) noexcept;
    // BadKey when the path's keys repeat or one is held by a node the merge keeps (section 12.4.2).
    [[nodiscard]] CryptoError check_new_keys(u32 sender, cont::ConstSpan<u8> leaf_key,
                                             const PathPlan& plan) const noexcept;

    CipherSuite          m_suite{};
    u32                  m_leaves = 0U;
    cont::Array<Stored>  m_nodes;
    cont::Array<u8>      m_hashes;   // Nh per node
    cont::Array<u8>      m_occupied; // per node: whether its subtree holds a non-blank node
};

// ---- TreeKEM (sections 7.4 to 7.6) ----

// A member's private keys: its leaf's HPKE key and the keys it holds of nodes on its direct path (section 4.1.1).
class PathKeys
{
public:
    // A member at `leaf` whose leaf key is `leaf_private` (SerializePrivateKey form) and who holds no other key.
    [[nodiscard]] CryptoError init(const CipherSuite& suite, u32 leaf, cont::ConstSpan<u8> leaf_private) noexcept;
    // The key of `node`, an ancestor of the member's leaf, from its path secret (DeriveSecret(path_secret, "node") and
    // DeriveKeyPair): BadKey when its public key is not the tree's at `node` (or the node is blank), OutOfWindow when
    // `node` is not above the leaf.
    [[nodiscard]] CryptoError set_path_secret(const RatchetTree& tree, u32 node,
                                              cont::ConstSpan<u8> path_secret) noexcept;
    // As set_path_secret, then each non-blank node above `node` from the next path secret (DeriveSecret(.., "path")):
    // what a Welcome's path secret gives a joiner (section 12.4.3.1).
    [[nodiscard]] CryptoError implant(const RatchetTree& tree, u32 node, cont::ConstSpan<u8> path_secret) noexcept;
    // Drops every held key whose node is blank, outside the tree or holds another public key (section 7.5's deletion
    // after proposals blank or replace a path).
    void prune(const RatchetTree& tree) noexcept;
    // Takes the keys of a confirmed commit's path (move: `update`'s keys are left empty).
    void apply(PathUpdate& update) noexcept;

    [[nodiscard]] bool ready() const noexcept { return m_leaf_key.ready(); }
    [[nodiscard]] u32  leaf() const noexcept { return m_leaf; }
    // Whether the member holds the private key of `node` (its leaf or an ancestor).
    [[nodiscard]] bool holds(u32 node) const noexcept;

private:
    friend CryptoError process_update_path(const PathKeys&, const RatchetTree&, u32, const UpdatePath&,
                                           cont::ConstSpan<u8>, cont::ConstSpan<u32>, PathUpdate&) noexcept;
    [[nodiscard]] const HpkeKey* key_of(u32 node) const noexcept;
    // NotReady, OutOfWindow for a node not above the leaf, BadLength for a secret not Nh long.
    [[nodiscard]] CryptoError path_check(const RatchetTree& tree, u32 node,
                                         cont::ConstSpan<u8> path_secret) const noexcept;

    CipherSuite m_suite{};
    u32         m_leaf = 0U;
    HpkeKey     m_leaf_key;
    HpkeKey     m_keys[tree::kMaxDepth + 1U]; // by level on the leaf's direct path
};

// The committer's side (sections 7.4 to 7.6 and 12.4.1) on the commit's provisional tree, its proposals applied: a
// fresh leaf key and a random first path secret, each node of the filtered direct path's path secret and key, the
// parent hashes, the new leaf (`leaf_template`'s signature key, credential, capabilities and extensions; source
// commit, the new key and parent hash, signed by `signer` with `context.group_id`) merged into `tree`, then each path
// secret encrypted to its copath child's resolution without `excluded` under the provisional GroupContext (`context`
// with the tree's new root hash). `leaf_template` may view the sender's leaf in `tree`. Writes the UpdatePath's
// encoding into `path` and the keys and secrets into `update`.
// OutOfWindow when `sender` is blank or outside; the refusals of the operations pass through; on a refusal `tree` is
// to be discarded and `update` holds nothing.
[[nodiscard]] CryptoError create_update_path(RatchetTree& tree, u32 sender, const Signer& signer,
                                             const LeafNode& leaf_template, const GroupContext& context,
                                             cont::ConstSpan<u32> excluded, cont::Array<u8>& path,
                                             PathUpdate& update) noexcept;

// A receiver's side (sections 7.5 and 12.4.2) on the tree `path` was merged into (merge_path): the lowest node of the
// sender's filtered direct path above the member, the node of its copath child's resolution (without `excluded`)
// whose key the member holds, that node's ciphertext opened under `context` (the provisional GroupContext's
// encoding), then each path secret above it derived and each key checked against the path's. Malformed when the
// node's ciphertext count is not its resolution's size or the member holds no key of it (or is the sender); BadKey
// when a derived public key is not the path's; HPKE's refusals pass through. `keys` is unchanged; `update` receives
// the new keys and secrets on success and holds nothing on a refusal.
[[nodiscard]] CryptoError process_update_path(const PathKeys& keys, const RatchetTree& tree, u32 sender,
                                              const UpdatePath& path, cont::ConstSpan<u8> context,
                                              cont::ConstSpan<u32> excluded, PathUpdate& update) noexcept;
} // namespace crd::crypto::mls
