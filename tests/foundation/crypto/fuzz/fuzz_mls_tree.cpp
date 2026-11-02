// crd-fuzz-crypto-mls-tree: MLS's ratchet tree and TreeKEM (NET.2; docs/design/network-foundation.md#mls) under the
// bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data, read field by field (a short input ends the case):
//   0  decoding: the suite, then the rest as a ratchet_tree extension and as an UpdatePath;
//   1  tree changes: the suite, then operations of two bytes each, the first picking add, update or remove (mod 3)
//      and the second the leaf (and the pooled leaf node an add or update takes);
//   2  an update path changed: the byte to change (u16; past the end changes none) and its XOR mask, applied to an
//      honest path a member of a four-member group sent.
//
// Oracles, none of them the code's verdict on the same call:
//   * a tree the decoder accepts writes back to exactly its input (the encoding is canonical: no trailing blank), a
//     copy writes back the same, every node's resolution is a restatement of section 4.1.1's recursion over the
//     public node accessors, every leaf's filtered direct path is section 4.1.2's restated over those resolutions,
//     and the join-time checks give one verdict however often they run; an accepted UpdatePath writes back;
//   * after every change the tree is a model's: the leaf count by section 7.7's doubling and section 12.1.3's
//     truncation restated, each leaf the model's pooled leaf node or blank, each changed leaf's direct path blank or
//     listing it as unmerged as section 12.1 says, and the held tree hashes equal to those of the same tree decoded
//     afresh from its encoding (the incremental hashes are the from-scratch ones);
//   * a changed path is refused by the merge with the tree unchanged, or merged; then each member either refuses to
//     decrypt it or reaches the honest commit secret: no change yields a different secret that is accepted.
#include <crd/fuzz/harness.hpp>

#include <crd/crypto/mls_tree.hpp>

#include <algorithm>
#include <cstring>

namespace
{
using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::containers::WireWriter;
using namespace crd::crypto;
namespace mls = crd::crypto::mls;

class Reader
{
public:
    Reader(const u8* data, usize size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] bool done() const noexcept { return m_at >= m_size; }

    u8 byte() noexcept
    {
        if (m_at >= m_size)
        {
            m_ok = false;
            return 0U;
        }
        return m_data[m_at++];
    }

    u16 u16be() noexcept { return static_cast<u16>(static_cast<u16>(byte() << 8U) | byte()); }

    ConstSpan<u8> rest() noexcept
    {
        const ConstSpan<u8> out(m_data + m_at, m_size - m_at);
        m_at = m_size;
        return out;
    }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
    bool      m_ok = true;
};

bool same(ConstSpan<u8> a, ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

const mls::CipherSuite& suite_of(u8 selector) noexcept
{
    const ConstSpan<mls::CipherSuite> suites = mls::cipher_suites();
    return suites[selector % suites.size()];
}

Array<u8> encoding_of(const mls::RatchetTree& tree)
{
    Array<u8> out;
    out.resize(tree.encoded_bytes());
    WireWriter writer(Span<u8>(out.data(), out.size()));
    tree.encode(writer);
    CRD_FUZZ_REQUIRE(writer.written() == out.size());
    return out;
}

ConstSpan<u8> view(const Array<u8>& bytes) noexcept
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

// ---- Section 4.1 restated over the public accessors ----

void restated_resolution(const mls::RatchetTree& tree, u32 node, Array<u32>& out)
{
    mls::ParentNode parent{};
    if (node % 2U == 0U)
    {
        if (!tree.blank(node))
        {
            out.push_back(node);
        }
        return;
    }
    if (tree.parent_node(node, parent))
    {
        out.push_back(node);
        for (usize i = 0U; i < parent.unmerged_count(); ++i)
        {
            if (parent.unmerged(i) < tree.leaves())
            {
                out.push_back(2U * parent.unmerged(i));
            }
        }
        return;
    }
    const u32 half = 1U << (mls::tree::level(node) - 1U);
    restated_resolution(tree, node - half, out);
    restated_resolution(tree, node + half, out);
}

void check_paths(const mls::RatchetTree& tree)
{
    for (u32 node = 0U; node < tree.node_count(); ++node)
    {
        Array<u32> expected;
        Array<u32> resolved;
        restated_resolution(tree, node, expected);
        CRD_FUZZ_REQUIRE(tree.resolution(node, {}, resolved) == CryptoError::None);
        CRD_FUZZ_REQUIRE(resolved.size() == expected.size());
        CRD_FUZZ_REQUIRE(std::equal(resolved.begin(), resolved.end(), expected.begin()));
    }
    for (u32 leaf = 0U; leaf < tree.leaves(); ++leaf)
    {
        u32         path[mls::tree::kMaxDepth]     = {};
        u32         filtered[mls::tree::kMaxDepth] = {};
        const usize depth = mls::tree::direct_path(2U * leaf, tree.leaves(), path);
        const usize count = tree.filtered_direct_path(leaf, filtered);
        usize       kept  = 0U;
        for (usize i = 0U; i < depth; ++i)
        {
            const u32  half    = 1U << (mls::tree::level(path[i]) - 1U);
            const u32  copath  = 2U * leaf < path[i] ? path[i] + half : path[i] - half;
            Array<u32> resolved;
            restated_resolution(tree, copath, resolved);
            if (!resolved.empty())
            {
                CRD_FUZZ_REQUIRE(kept < count && filtered[kept] == path[i]);
                ++kept;
            }
        }
        CRD_FUZZ_REQUIRE(kept == count);
    }
}

// ---- Mode 0: decoding ----

void decoding(Reader& in)
{
    const mls::CipherSuite& suite = suite_of(in.byte());
    const ConstSpan<u8>     bytes = in.rest();
    if (!in.ok())
    {
        return;
    }
    mls::RatchetTree tree;
    if (tree.decode(suite, bytes) == CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(same(view(encoding_of(tree)), bytes));
        mls::RatchetTree copy;
        CRD_FUZZ_REQUIRE(copy.copy_from(tree) == CryptoError::None);
        CRD_FUZZ_REQUIRE(same(view(encoding_of(copy)), bytes));
        CRD_FUZZ_REQUIRE(same(copy.root_hash(), tree.root_hash()));
        check_paths(tree);
        const ProviderVerifier verifier;
        const u8               group[] = {'g'};
        const CryptoError      first   = tree.validate(verifier, group);
        CRD_FUZZ_REQUIRE(tree.validate(verifier, group) == first);
    }
    else
    {
        CRD_FUZZ_REQUIRE(!tree.ready());
    }
    WireReader      reader(bytes);
    mls::UpdatePath path;
    if (mls::take_update_path(reader, path))
    {
        const usize taken = bytes.size() - reader.rest().size();
        CRD_FUZZ_REQUIRE(mls::update_path_bytes(path) == taken);
        Array<u8> written;
        written.resize(taken);
        WireWriter writer(Span<u8>(written.data(), written.size()));
        mls::put_update_path(writer, path);
        CRD_FUZZ_REQUIRE(same(view(written), bytes.first(taken)));
    }
}

// ---- Mode 1: tree changes against a model ----

constexpr u32 kPool     = 8U;
constexpr u32 kMaxModel = 64U;

// Eight leaf nodes of distinct keys (their signatures are not the tree changes' concern).
struct Pool
{
    u8            keys[kPool][2][32] = {};
    u8            credential[kPool]  = {};
    mls::LeafNode leaf[kPool];

    Pool() noexcept
    {
        for (u32 i = 0U; i < kPool; ++i)
        {
            std::memset(keys[i][0], static_cast<int>(0x10U + i), 32U);
            std::memset(keys[i][1], static_cast<int>(0x80U + i), 32U);
            credential[i]                   = static_cast<u8>('a' + i);
            leaf[i].encryption_key          = ConstSpan<u8>(keys[i][0], 32U);
            leaf[i].signature_key           = ConstSpan<u8>(keys[i][1], 32U);
            leaf[i].credential.type         = static_cast<u16>(mls::CredentialType::Basic);
            leaf[i].credential.content      = ConstSpan<u8>(&credential[i], 1U);
            leaf[i].source                  = mls::LeafNodeSource::KeyPackage;
            leaf[i].signature               = ConstSpan<u8>(keys[i][1], 4U);
        }
    }
};

const Pool kLeaves;

// A hand-written tree: three leaves of the pool and the two parents between them, the upper one listing leaf 2.
usize seed_tree(u8* out)
{
    Array<u8> body;
    const auto put = [&body](ConstSpan<u8> bytes) {
        for (const u8 b : bytes)
        {
            body.push_back(b);
        }
    };
    u8 scratch[256];
    for (u32 node = 0U; node < 5U; ++node)
    {
        WireWriter writer(Span<u8>(scratch, sizeof(scratch)));
        if (node % 2U == 0U)
        {
            writer.put_u8(1U);
            writer.put_u8(1U);
            mls::put_leaf_node(writer, kLeaves.leaf[node / 2U]);
        }
        else
        {
            const u8        key[32]   = {static_cast<u8>(node)};
            const u8        listed[4] = {0U, 0U, 0U, 2U};
            mls::ParentNode parent{ConstSpan<u8>(key, 32U), {}, node == 3U ? ConstSpan<u8>(listed) : ConstSpan<u8>()};
            writer.put_u8(1U);
            writer.put_u8(2U);
            mls::put_parent_node(writer, parent);
        }
        put(ConstSpan<u8>(scratch, writer.written()));
    }
    WireWriter writer(Span<u8>(out, 1024U));
    mls::put_varint(writer, body.size());
    writer.put_bytes(view(body));
    return writer.written();
}

// The model: each leaf's pooled node (kPool for a blank leaf) and the leaf count.
struct Model
{
    u32 leaf[kMaxModel] = {};
    u32 leaves          = 1U;

    [[nodiscard]] bool right_half_blank() const noexcept
    {
        return std::all_of(leaf + leaves / 2U, leaf + leaves, [](u32 at) { return at == kPool; });
    }
};

void check_model(const mls::RatchetTree& tree, const Model& model, const mls::CipherSuite& suite)
{
    CRD_FUZZ_REQUIRE(tree.leaves() == model.leaves);
    for (u32 i = 0U; i < model.leaves; ++i)
    {
        mls::LeafNode leaf{};
        const bool    present = tree.leaf_node(i, leaf);
        CRD_FUZZ_REQUIRE(present == (model.leaf[i] != kPool));
        CRD_FUZZ_REQUIRE(!present || same(leaf.encryption_key, kLeaves.leaf[model.leaf[i]].encryption_key));
    }
    // The held hashes are the ones a fresh decode computes (a tree with no member left has no encoding to decode).
    if (std::any_of(model.leaf, model.leaf + model.leaves, [](u32 at) { return at != kPool; }))
    {
        mls::RatchetTree fresh;
        CRD_FUZZ_REQUIRE(fresh.decode(suite, view(encoding_of(tree))) == CryptoError::None);
        for (u32 node = 0U; node < tree.node_count(); ++node)
        {
            CRD_FUZZ_REQUIRE(same(fresh.tree_hash(node), tree.tree_hash(node)));
        }
    }
    check_paths(tree);
}

// Whether every non-blank node above `leaf` lists it as unmerged (an add) or every node above is blank (an update or
// a remove).
bool path_is(const mls::RatchetTree& tree, u32 leaf, bool listed) noexcept
{
    for (u32 l = 1U; (1U << l) <= tree.leaves(); ++l)
    {
        const u32       node = ((2U * leaf) | ((1U << l) - 1U)) & ~(1U << l);
        mls::ParentNode parent{};
        if (!tree.parent_node(node, parent))
        {
            continue;
        }
        bool found = false;
        for (usize i = 0U; i < parent.unmerged_count(); ++i)
        {
            found = found || parent.unmerged(i) == leaf;
        }
        if (!listed || !found)
        {
            return false;
        }
    }
    return true;
}

void changes(Reader& in)
{
    const mls::CipherSuite& suite = suite_of(in.byte());
    if (!in.ok())
    {
        return;
    }
    // The hand-written tree (leaves 0 to 2 of the pool, both parents set, the root listing leaf 2), so adds meet
    // non-blank parents and unmerged lists from the start.
    Model            model;
    mls::RatchetTree tree;
    u8               start[1024] = {};
    const usize      bytes       = seed_tree(start);
    CRD_FUZZ_REQUIRE(tree.decode(suite, ConstSpan<u8>(start, bytes)) == CryptoError::None);
    model.leaves  = 4U;
    model.leaf[0] = 0U;
    model.leaf[1] = 1U;
    model.leaf[2] = 2U;
    model.leaf[3] = kPool;
    while (!in.done())
    {
        const u8  op   = in.byte();
        const u8  arg  = in.byte();
        const u32 node = arg % kPool;
        if (!in.ok())
        {
            return;
        }
        switch (op % 3U)
        {
        case 0U:
        {
            u32 index = 0U;
            u32 at    = 0U;
            while (at < model.leaves && model.leaf[at] != kPool)
            {
                ++at;
            }
            if (at == model.leaves && model.leaves * 2U > kMaxModel)
            {
                return;
            }
            if (at == model.leaves)
            {
                std::fill(model.leaf + model.leaves, model.leaf + 2U * model.leaves, kPool);
                model.leaves *= 2U;
            }
            CRD_FUZZ_REQUIRE(tree.add(kLeaves.leaf[node], index) == CryptoError::None);
            CRD_FUZZ_REQUIRE(index == at);
            model.leaf[at] = node;
            CRD_FUZZ_REQUIRE(path_is(tree, at, true));
            break;
        }
        case 1U:
        {
            const u32         leaf   = (arg / kPool) % model.leaves;
            const CryptoError result = tree.update(leaf, kLeaves.leaf[node]);
            CRD_FUZZ_REQUIRE(result == (model.leaf[leaf] == kPool ? CryptoError::OutOfWindow : CryptoError::None));
            if (result == CryptoError::None)
            {
                model.leaf[leaf] = node;
                CRD_FUZZ_REQUIRE(path_is(tree, leaf, false));
            }
            break;
        }
        default:
        {
            const u32         leaf   = (arg / kPool) % model.leaves;
            const CryptoError result = tree.remove(leaf);
            CRD_FUZZ_REQUIRE(result == (model.leaf[leaf] == kPool ? CryptoError::OutOfWindow : CryptoError::None));
            if (result == CryptoError::None)
            {
                model.leaf[leaf] = kPool;
                while (model.leaves > 1U && model.right_half_blank())
                {
                    model.leaves /= 2U;
                }
            }
            break;
        }
        }
        check_model(tree, model, suite);
    }
}

// ---- Mode 2: an honest update path changed in one byte ----

constexpr u32 kMembers = 4U;

// A four-member group of the first suite, every member having committed, and member 0's next path.
class Group
{
public:
    Group()
    {
        u8 seed[32];
        for (u32 m = 0U; m < kMembers; ++m)
        {
            std::memset(seed, static_cast<int>(0x40U + m), sizeof(seed));
            CRD_FUZZ_REQUIRE(m_signers[m].from_seed(m_suite.signature, ConstSpan<u8>(seed, 32U)) == CryptoError::None);
            CRD_FUZZ_REQUIRE(m_signers[m].public_key(Span<u8>(m_signature_keys[m], 32U)) == CryptoError::None);
            std::memset(m_private[m], static_cast<int>(0x60U + m), 32U);
            HpkeKey key;
            CRD_FUZZ_REQUIRE(key.from_private(m_suite.kem, ConstSpan<u8>(m_private[m], 32U)) == CryptoError::None);
            CRD_FUZZ_REQUIRE(key.public_key(Span<u8>(m_public[m], 32U)) == CryptoError::None);
            m_leaves[m].encryption_key     = ConstSpan<u8>(m_public[m], 32U);
            m_leaves[m].signature_key      = ConstSpan<u8>(m_signature_keys[m], 32U);
            m_leaves[m].credential.type    = static_cast<u16>(mls::CredentialType::Basic);
            m_leaves[m].credential.content = ConstSpan<u8>(m_signature_keys[m], 1U);
            m_leaves[m].source             = mls::LeafNodeSource::KeyPackage;
            m_leaves[m].signature          = ConstSpan<u8>(m_signature_keys[m], 2U);
            u32 index                      = 0U;
            CRD_FUZZ_REQUIRE((m == 0U ? m_tree.create(m_suite, m_leaves[0]) : m_tree.add(m_leaves[m], index)) ==
                             CryptoError::None);
            CRD_FUZZ_REQUIRE(m_keys[m].init(m_suite, m, ConstSpan<u8>(m_private[m], 32U)) == CryptoError::None);
            commit(m, m + 1U);
        }
        // Member 0's path, merged into a copy, and the provisional context the others decrypt under.
        mls::LeafNode leaf{};
        CRD_FUZZ_REQUIRE(m_tree.leaf_node(0U, leaf));
        CRD_FUZZ_REQUIRE(m_after.copy_from(m_tree) == CryptoError::None);
        CRD_FUZZ_REQUIRE(mls::create_update_path(m_after, 0U, m_signers[0], leaf, context(), {}, m_path, m_sent) ==
                         CryptoError::None);
        provisional(m_after, m_context);
    }

    [[nodiscard]] const mls::RatchetTree& tree() const noexcept { return m_tree; }
    [[nodiscard]] ConstSpan<u8>           path() const noexcept { return view(m_path); }
    [[nodiscard]] const mls::PathKeys&    keys(u32 m) const noexcept { return m_keys[m]; }
    [[nodiscard]] ConstSpan<u8>           commit_secret() const noexcept { return m_sent.commit_secret(); }

    [[nodiscard]] mls::GroupContext context() const noexcept
    {
        return {m_suite.id, ConstSpan<u8>(kGroup), 2U, {}, ConstSpan<u8>(kTranscript), {}};
    }
    void provisional(const mls::RatchetTree& tree, Array<u8>& out) const
    {
        mls::GroupContext ctx = context();
        ctx.tree_hash         = tree.root_hash();
        out.resize(mls::group_context_bytes(ctx));
        WireWriter writer(Span<u8>(out.data(), out.size()));
        mls::put_group_context(writer, ctx);
    }

private:
    static constexpr u8 kGroup[4]       = {'t', 'r', 'e', 'e'};
    static constexpr u8 kTranscript[32] = {};

    void commit(u32 committer, u32 members)
    {
        mls::LeafNode leaf{};
        CRD_FUZZ_REQUIRE(m_tree.leaf_node(committer, leaf));
        Array<u8>       path;
        mls::PathUpdate sent;
        CRD_FUZZ_REQUIRE(mls::create_update_path(m_tree, committer, m_signers[committer], leaf, context(), {}, path,
                                                 sent) == CryptoError::None);
        m_keys[committer].apply(sent);
        for (u32 m = 0U; m < members; ++m)
        {
            if (m == committer)
            {
                continue;
            }
            m_keys[m].prune(m_tree);
            const auto meet = mls::tree::common_ancestor(2U * committer, 2U * m, m_tree.leaves());
            CRD_FUZZ_REQUIRE(meet.has_value());
            CRD_FUZZ_REQUIRE(m_keys[m].implant(m_tree, *meet, sent.path_secret(*meet)) == CryptoError::None);
        }
    }

    const mls::CipherSuite& m_suite = mls::cipher_suites()[0];
    SigningKey              m_signers[kMembers];
    u8                      m_signature_keys[kMembers][32] = {};
    u8                      m_private[kMembers][32]        = {};
    u8                      m_public[kMembers][32]         = {};
    mls::LeafNode           m_leaves[kMembers];
    mls::PathKeys           m_keys[kMembers];
    mls::RatchetTree        m_tree;
    mls::RatchetTree        m_after;
    Array<u8>               m_path;
    Array<u8>               m_context;
    mls::PathUpdate         m_sent;
};

void path_changed(Reader& in)
{
    static const Group kFixture;
    const u16          at   = in.u16be();
    const u8           mask = in.byte();
    if (!in.ok())
    {
        return;
    }
    Array<u8> changed;
    changed.resize(kFixture.path().size());
    std::memcpy(changed.data(), kFixture.path().data(), changed.size());
    if (at < changed.size())
    {
        changed[at] ^= mask;
    }
    WireReader      reader(view(changed));
    mls::UpdatePath path;
    if (!mls::take_update_path(reader, path) || !reader.done())
    {
        return;
    }
    mls::RatchetTree tree;
    CRD_FUZZ_REQUIRE(tree.copy_from(kFixture.tree()) == CryptoError::None);
    const Array<u8> before = encoding_of(tree);
    if (tree.merge_path(0U, path) != CryptoError::None)
    {
        CRD_FUZZ_REQUIRE(same(view(encoding_of(tree)), view(before)));
        return;
    }
    Array<u8> context;
    kFixture.provisional(tree, context);
    for (u32 m = 1U; m < kMembers; ++m)
    {
        mls::PathUpdate update;
        if (mls::process_update_path(kFixture.keys(m), tree, 0U, path, view(context), {}, update) == CryptoError::None)
        {
            CRD_FUZZ_REQUIRE(same(update.commit_secret(), kFixture.commit_secret()));
        }
        else
        {
            CRD_FUZZ_REQUIRE(update.commit_secret().empty());
        }
    }
}

// ---- Seeds ----

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Reader in(data + 1U, size - 1U);
    switch (data[0] % 3U)
    {
    case 0U:
        decoding(in);
        break;
    case 1U:
        changes(in);
        break;
    default:
        path_changed(in);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Decoding: the hand-written tree in each suite, it without its last node (a trailing parent), a lone blank node.
    u8          tree[1024 + 2] = {};
    const usize bytes          = seed_tree(tree + 2U);
    const char* names[4]       = {"decode-tree-0", "decode-tree-1", "decode-tree-2", "decode-tree-3"};
    for (u8 suite = 0U; suite < 4U; ++suite)
    {
        tree[1] = suite;
        sink.add(names[suite], tree, bytes + 2U);
    }
    // The same tree without its last leaf: it ends at a parent, which the decoder extends.
    const usize last = mls::leaf_node_bytes(kLeaves.leaf[2]) + 2U;
    u8          trimmed[1024 + 2] = {};
    WireWriter  writer(Span<u8>(trimmed + 2U, 1024U));
    WireReader  reader(ConstSpan<u8>(tree + 2U, bytes));
    usize       length = 0U;
    CRD_FUZZ_REQUIRE(mls::take_varint(reader, length));
    mls::put_varint(writer, length - last);
    writer.put_bytes(reader.rest().first(length - last));
    sink.add("decode-trailing-parent", trimmed, writer.written() + 2U);
    const u8 blank[] = {0U, 0U, 1U, 0U};
    sink.add("decode-blank", blank, sizeof(blank));
    const u8 empty_path[] = {0U, 0U, 0U};
    sink.add("decode-short", empty_path, sizeof(empty_path));

    // Tree changes: adds that grow the tree, updates, removes that truncate it, and changes of blank leaves.
    const u8 grow[] = {1U, 0U, 0U, 1U, 0U, 2U, 0U, 3U, 0U, 4U, 0U, 5U};
    sink.add("changes-grow", grow, sizeof(grow));
    const u8 churn[] = {1U, 1U, 0U, 1U, 0U, 2U, 1U, 9U, 2U, 8U, 0U, 3U, 2U, 16U, 2U, 9U, 1U, 17U, 0U, 6U, 2U, 0U};
    sink.add("changes-churn", churn, sizeof(churn));
    const u8 shrink[] = {1U, 3U, 0U, 1U, 0U, 2U, 0U, 3U, 0U, 4U, 2U, 32U, 2U, 24U, 2U, 16U, 2U, 8U, 0U, 7U};
    sink.add("changes-shrink", shrink, sizeof(shrink));
    const u8 blank_ops[] = {1U, 2U, 1U, 8U, 2U, 8U, 0U, 1U, 2U, 8U, 2U, 8U};
    sink.add("changes-blank", blank_ops, sizeof(blank_ops));

    // A path unchanged, and changed in its leaf, its first node's key, its parent hash region and its ciphertexts.
    const u8 unchanged[] = {2U, 0xFFU, 0xFFU, 0U};
    sink.add("path-unchanged", unchanged, sizeof(unchanged));
    const u16   offsets[] = {2U, 40U, 120U, 150U, 200U, 260U, 330U, 400U};
    const char* path_names[8] = {"path-at-2",   "path-at-40",  "path-at-120", "path-at-150",
                                 "path-at-200", "path-at-260", "path-at-330", "path-at-400"};
    for (usize i = 0U; i < 8U; ++i)
    {
        const u8 changed[] = {2U, static_cast<u8>(offsets[i] >> 8U), static_cast<u8>(offsets[i]), 0x01U};
        sink.add(path_names[i], changed, sizeof(changed));
    }
}
