// crd-crypto -- MLS's ratchet tree and TreeKEM, RFC 9420 section 7 (NET.2; docs/design/network-foundation.md#mls),
// against the MLS working group's test vectors (the pinned mlswg/mls-implementations test-vectors, read at run time):
// every tree-validation vector of the profile (the tree decoded and written back, each node's resolution and tree
// hash, the join-time checks), every tree-operations vector (Add, Update and Remove) and every treekem vector (the
// members' private states, each update path merged, decrypted by every other member and written again by its sender).
// Then groups that add, update, remove and commit at random, every member's tree, keys and commit secret compared at
// every epoch against TreeKEM's invariant, and every refusal.

#include "wycheproof.hpp"

#include <crd/crypto/mls_tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <utility>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::containers::WireWriter;
using namespace crd::crypto;
using namespace crd::crypto::test;
namespace json = crd::assetio::json;
namespace mls  = crd::crypto::mls;

JsonFile mls_vectors(const char* name)
{
    return JsonFile(vector_path(CRD_MLS_VECTORS_DIR, name));
}

Span<u8> span_of(Bytes& bytes)
{
    return Span<u8>(bytes.data(), bytes.size());
}

ConstSpan<u8> view(const Bytes& bytes)
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

// The suite of vector `v`, or null (with the check that it is one of the three outside the profile).
const mls::CipherSuite* profile_suite(const JsonFile& vectors, u32 v)
{
    const u16               id    = static_cast<u16>(vectors.number(v, "cipher_suite"));
    const mls::CipherSuite* suite = mls::cipher_suite(id);
    if (suite == nullptr)
    {
        CHECK((id >= 4U && id <= 6U)); // X448, Ed448 and P-521: outside the profile
    }
    return suite;
}

Bytes encoded(const mls::RatchetTree& tree)
{
    Bytes out = filled(tree.encoded_bytes(), 0U);
    REQUIRE(!out.empty());
    WireWriter writer(span_of(out));
    tree.encode(writer);
    REQUIRE(writer.written() == out.size());
    return out;
}

mls::RatchetTree decoded_tree(const mls::CipherSuite& suite, ConstSpan<u8> bytes)
{
    mls::RatchetTree tree;
    REQUIRE(tree.decode(suite, bytes) == CryptoError::None);
    CHECK(equal(encoded(tree), bytes));
    return tree;
}

mls::RatchetTree copy_of(const mls::RatchetTree& tree)
{
    mls::RatchetTree copy;
    REQUIRE(copy.copy_from(tree) == CryptoError::None);
    return copy;
}

Bytes context_bytes(const mls::GroupContext& context)
{
    Bytes out = filled(mls::group_context_bytes(context), 0U);
    REQUIRE(!out.empty());
    WireWriter writer(span_of(out));
    mls::put_group_context(writer, context);
    return out;
}

mls::UpdatePath update_path(ConstSpan<u8> bytes)
{
    WireReader      in(bytes);
    mls::UpdatePath path;
    REQUIRE(mls::take_update_path(in, path));
    REQUIRE(in.done());
    return path;
}

Bytes path_written(const mls::UpdatePath& path)
{
    Bytes out = filled(mls::update_path_bytes(path), 0U);
    REQUIRE(!out.empty());
    WireWriter writer(span_of(out));
    mls::put_update_path(writer, path);
    return out;
}

u32 lowest_common(u32 a, u32 b, u32 leaves)
{
    const auto node = mls::tree::common_ancestor(2U * a, 2U * b, leaves);
    REQUIRE(node.has_value());
    return *node;
}

// A tree's extension encoding from its nodes' encodings (empty for a blank node), as a hostile sender would write it.
Bytes tree_of(const crd::containers::Array<Bytes>& nodes)
{
    usize body = 0U;
    for (const Bytes& node : nodes)
    {
        body += node.empty() ? 1U : 2U + node.size();
    }
    Bytes      out = filled(mls::vector_bytes(body), 0U);
    WireWriter writer(span_of(out));
    mls::put_varint(writer, body);
    for (usize i = 0U; i < nodes.size(); ++i)
    {
        writer.put_u8(nodes[i].empty() ? 0U : 1U);
        if (!nodes[i].empty())
        {
            writer.put_u8(i % 2U == 0U ? 1U : 2U);
            writer.put_bytes(view(nodes[i]));
        }
    }
    return out;
}

Bytes leaf_bytes(const mls::LeafNode& leaf)
{
    Bytes      out = filled(mls::leaf_node_bytes(leaf), 0U);
    WireWriter writer(span_of(out));
    mls::put_leaf_node(writer, leaf);
    return out;
}

Bytes parent_bytes(const mls::ParentNode& parent)
{
    Bytes      out = filled(mls::parent_node_bytes(parent), 0U);
    WireWriter writer(span_of(out));
    mls::put_parent_node(writer, parent);
    return out;
}

// Every node of a tree, encoded.
crd::containers::Array<Bytes> nodes_of(const mls::RatchetTree& tree)
{
    crd::containers::Array<Bytes> nodes;
    for (u32 node = 0U; node < tree.node_count(); ++node)
    {
        mls::LeafNode   leaf{};
        mls::ParentNode parent{};
        if (node % 2U == 0U && tree.leaf_node(node / 2U, leaf))
        {
            nodes.push_back(leaf_bytes(leaf));
        }
        else if (tree.parent_node(node, parent))
        {
            nodes.push_back(parent_bytes(parent));
        }
        else
        {
            nodes.push_back(Bytes());
        }
    }
    while (!nodes.empty() && nodes.back().empty())
    {
        nodes.pop_back();
    }
    return nodes;
}

Bytes unmerged_list(std::initializer_list<u32> leaves)
{
    Bytes      out = filled(4U * leaves.size(), 0U);
    WireWriter writer(span_of(out));
    for (const u32 leaf : leaves)
    {
        writer.put_u32(leaf);
    }
    return out;
}

// ---- A member of a model group ----

// A deterministic generator for the model groups' choices.
class Choices
{
public:
    explicit Choices(u64 seed) : m_state(seed) {}
    u32 below(u32 bound)
    {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;
        return static_cast<u32>(m_state % bound);
    }

private:
    u64 m_state;
};

// A member's long-term signer and its current leaf, whose fields view the member's own bytes.
struct Identity
{
    SigningKey    signer;
    Bytes         signature_key;
    Bytes         hpke_private;
    Bytes         hpke_public;
    Bytes         credential;
    Bytes         versions;
    Bytes         suites;
    Bytes         credentials;
    Bytes         signature;
    mls::LeafNode leaf;
};

Bytes random_of(usize size)
{
    Bytes out = filled(size, 0U);
    REQUIRE(random_bytes(span_of(out)) == CryptoError::None);
    return out;
}

// A fresh HPKE key pair for the identity (a random scalar, drawn again in the rare case it is out of range).
void fresh_hpke(const mls::CipherSuite& suite, Identity& id)
{
    HpkeKey key;
    do
    {
        id.hpke_private = random_of(hpke_sizes(suite.kem).private_key);
    } while (key.from_private(suite.kem, view(id.hpke_private)) != CryptoError::None);
    id.hpke_public = filled(hpke_sizes(suite.kem).public_key, 0U);
    REQUIRE(key.public_key(span_of(id.hpke_public)) == CryptoError::None);
}

// The identity's leaf of `source`, signed (with the group and index for an update leaf).
void sign_leaf(const mls::CipherSuite& suite, Identity& id, mls::LeafNodeSource source, ConstSpan<u8> group_id,
               u32 index)
{
    id.leaf                        = {};
    id.leaf.encryption_key         = view(id.hpke_public);
    id.leaf.signature_key          = view(id.signature_key);
    id.leaf.credential.type        = static_cast<u16>(mls::CredentialType::Basic);
    id.leaf.credential.content     = view(id.credential);
    id.leaf.capabilities.versions  = view(id.versions);
    id.leaf.capabilities.cipher_suites = view(id.suites);
    id.leaf.capabilities.credentials   = view(id.credentials);
    id.leaf.source                 = source;
    id.leaf.not_before             = 0U;
    id.leaf.not_after              = ~u64{0};
    id.signature                   = filled(signature_sizes(suite.signature).signature, 0U);
    usize written                  = 0U;
    REQUIRE(mls::sign_leaf_node(suite, id.signer, id.leaf, group_id, index, span_of(id.signature), written) ==
            CryptoError::None);
    id.signature.resize(written);
    id.leaf.signature = view(id.signature);
}

void make_identity(const mls::CipherSuite& suite, Identity& id, u32 name)
{
    REQUIRE(id.signer.generate(suite.signature) == CryptoError::None);
    id.signature_key = filled(signature_sizes(suite.signature).public_key, 0U);
    REQUIRE(id.signer.public_key(span_of(id.signature_key)) == CryptoError::None);
    id.credential  = filled(4U, 0U);
    id.credential[3] = static_cast<u8>(name);
    id.versions    = Bytes{0x00U, 0x01U};
    id.suites      = Bytes{static_cast<u8>(suite.id >> 8U), static_cast<u8>(suite.id)};
    id.credentials = Bytes{0x00U, 0x01U};
    fresh_hpke(suite, id);
    sign_leaf(suite, id, mls::LeafNodeSource::KeyPackage, {}, 0U);
}

struct Seat
{
    bool             active = false;
    Identity         id;
    mls::PathKeys    keys;
    mls::RatchetTree tree;
};

// TreeKEM's invariant (sections 4.1.1 and 7.5): a member holds the private key of every non-blank node on its direct
// path for which it is not an unmerged leaf, and no other.
void check_keys(const mls::RatchetTree& tree, const mls::PathKeys& keys)
{
    CHECK(keys.holds(2U * keys.leaf()));
    u32         path[mls::tree::kMaxDepth] = {};
    const usize depth = mls::tree::direct_path(2U * keys.leaf(), tree.leaves(), path);
    for (usize i = 0U; i < depth; ++i)
    {
        mls::ParentNode parent{};
        if (!tree.parent_node(path[i], parent))
        {
            CHECK(!keys.holds(path[i]));
            continue;
        }
        bool unmerged = false;
        for (usize k = 0U; k < parent.unmerged_count(); ++k)
        {
            unmerged = unmerged || parent.unmerged(k) == keys.leaf();
        }
        INFO("leaf " << keys.leaf() << " node " << path[i]);
        CHECK(keys.holds(path[i]) == !unmerged);
    }
}

// A model group of up to 16 members that runs `steps` commits, each with one random proposal or none and a path,
// every member checked at every epoch.
void run_group(const mls::CipherSuite& suite, u64 seed, u32 steps)
{
    constexpr u32          seat_count = 16U;
    Choices                choices(seed);
    const ProviderVerifier verifier;
    Seat                   seats[seat_count];
    const Bytes            group_id = random_of(16U);
    u64                    epoch    = 0U;
    Bytes                  transcript = random_of(suite.hash_bytes());

    make_identity(suite, seats[0].id, 0U);
    REQUIRE(seats[0].tree.create(suite, seats[0].id.leaf) == CryptoError::None);
    REQUIRE(seats[0].keys.init(suite, 0U, view(seats[0].id.hpke_private)) == CryptoError::None);
    seats[0].active = true;
    usize adds           = 0U;
    usize updates        = 0U;
    usize removes        = 0U;
    usize unmerged_paths = 0U;

    for (u32 step = 0U; step < steps; ++step)
    {
        INFO("step " << step);
        u32 active[seat_count] = {};
        u32 count          = 0U;
        for (u32 s = 0U; s < seat_count; ++s)
        {
            if (seats[s].active)
            {
                active[count++] = s;
            }
        }
        const u32 committer = active[choices.below(count)];
        const u32 kind      = choices.below(4U); // 0: none, 1: add, 2: update, 3: remove
        u32       added     = seat_count;
        if (kind == 1U && count < 12U)
        {
            // The new member's key package leaf goes into every member's tree, at the same leftmost blank leaf.
            u32 seat = 0U;
            while (seats[seat].active)
            {
                ++seat;
            }
            Identity& id = seats[seat].id;
            make_identity(suite, id, seat);
            for (u32 i = 0U; i < count; ++i)
            {
                u32 index = 0U;
                REQUIRE(seats[active[i]].tree.add(id.leaf, index) == CryptoError::None);
                REQUIRE(index == seat); // seats are leaf indices: the leftmost blank leaf
            }
            added = seat;
            ++adds;
        }
        else if (kind == 2U && count > 1U)
        {
            u32 updater = active[choices.below(count)];
            updater     = updater == committer ? active[(choices.below(count - 1U) + 1U) % count] : updater;
            if (updater != committer)
            {
                Identity& id = seats[updater].id;
                fresh_hpke(suite, id);
                sign_leaf(suite, id, mls::LeafNodeSource::Update, view(group_id), updater);
                for (u32 i = 0U; i < count; ++i)
                {
                    REQUIRE(seats[active[i]].tree.update(updater, id.leaf) == CryptoError::None);
                }
                REQUIRE(seats[updater].keys.init(suite, updater, view(id.hpke_private)) == CryptoError::None);
                ++updates;
            }
        }
        else if (kind == 3U && count > 2U)
        {
            const u32 removed = active[choices.below(count)];
            if (removed != committer)
            {
                seats[removed].active = false;
                seats[removed].tree   = mls::RatchetTree();
                for (u32 s = 0U; s < seat_count; ++s)
                {
                    if (seats[s].active)
                    {
                        REQUIRE(seats[s].tree.remove(removed) == CryptoError::None);
                    }
                }
                ++removes;
            }
        }

        // The committer's path on its provisional tree.
        const u32 excluded_leaves[1] = {added};
        const ConstSpan<u32> excluded(excluded_leaves, added == seat_count ? 0U : 1U);
        mls::GroupContext    context{suite.id, view(group_id), epoch + 1U, {}, view(transcript), {}};
        Seat&                lead = seats[committer];
        lead.keys.prune(lead.tree);
        mls::LeafNode template_leaf{};
        REQUIRE(lead.tree.leaf_node(committer, template_leaf));
        mls::PathUpdate                 sent;
        crd::containers::Array<u8>      path_bytes;
        mls::RatchetTree                provisional = copy_of(lead.tree);
        REQUIRE(mls::create_update_path(provisional, committer, lead.id.signer, template_leaf, context, excluded,
                                        path_bytes, sent) == CryptoError::None);
        const mls::UpdatePath path = update_path(view(path_bytes));
        CHECK(equal(path_written(path), view(path_bytes)));
        CHECK(mls::verify_leaf_node(suite, verifier, path.leaf_node, view(group_id), committer) ==
              Verification::Valid);
        REQUIRE(provisional.validate(verifier, view(group_id)) == CryptoError::None);
        context.tree_hash         = provisional.root_hash();
        const Bytes provisional_context = context_bytes(context);

        // Every other member merges and decrypts it.
        for (u32 s = 0U; s < seat_count; ++s)
        {
            if (!seats[s].active || s == committer)
            {
                continue;
            }
            INFO("member " << s);
            seats[s].keys.prune(seats[s].tree);
            REQUIRE(seats[s].tree.merge_path(committer, path) == CryptoError::None);
            CHECK(equal(seats[s].tree.root_hash(), provisional.root_hash()));
            mls::PathUpdate received;
            REQUIRE(mls::process_update_path(seats[s].keys, seats[s].tree, committer, path,
                                             view(provisional_context), excluded, received) == CryptoError::None);
            CHECK(equal(received.commit_secret(), sent.commit_secret()));
            const u32 meet = lowest_common(committer, s, provisional.leaves());
            CHECK(equal(received.path_secret(meet), sent.path_secret(meet)));
            seats[s].keys.apply(received);
        }
        // A new member joins from the committer's tree and the path secret of their lowest common ancestor (the
        // Welcome's, section 12.4.3.1).
        if (added != seat_count)
        {
            Seat& joiner = seats[added];
            joiner.tree  = decoded_tree(suite, view(encoded(provisional)));
            REQUIRE(joiner.tree.validate(verifier, view(group_id)) == CryptoError::None);
            REQUIRE(joiner.keys.init(suite, added, view(joiner.id.hpke_private)) == CryptoError::None);
            const u32 meet = lowest_common(committer, added, provisional.leaves());
            REQUIRE(joiner.keys.implant(joiner.tree, meet, sent.path_secret(meet)) == CryptoError::None);
            joiner.active = true;
        }
        lead.tree = std::move(provisional);
        lead.keys.apply(sent);
        ++epoch;
        transcript = random_of(suite.hash_bytes());

        // One tree, and every member's keys exactly what TreeKEM gives it.
        const Bytes reference = encoded(lead.tree);
        for (const Seat& seat : seats)
        {
            if (seat.active)
            {
                CHECK(equal(encoded(seat.tree), view(reference)));
                check_keys(seat.tree, seat.keys);
            }
        }
        for (u32 node = 1U; node < lead.tree.node_count(); node += 2U)
        {
            mls::ParentNode parent{};
            unmerged_paths += lead.tree.parent_node(node, parent) && parent.unmerged_count() != 0U ? 1U : 0U;
        }
    }
    // The run reached every operation, and trees holding unmerged leaves.
    CHECK(adds > 0U);
    CHECK(updates > 0U);
    CHECK(removes > 0U);
    CHECK(unmerged_paths > 0U);
}

// A small group built through the API: `members` leaves, each added and then committing in turn, so every parent
// node is set and the last member is unmerged nowhere.
struct Group
{
    const mls::CipherSuite* suite = nullptr;
    Bytes                   group_id;
    Identity                ids[8];
    mls::PathKeys           keys[8];
    mls::RatchetTree        tree;
};

void commit_in(Group& group, u32 committer, u32 members)
{
    mls::LeafNode leaf{};
    REQUIRE(group.tree.leaf_node(committer, leaf));
    const Bytes                transcript = filled(group.suite->hash_bytes(), 0x5AU);
    const mls::GroupContext    context{group.suite->id, view(group.group_id), 1U, {}, view(transcript), {}};
    mls::PathUpdate            sent;
    crd::containers::Array<u8> path;
    REQUIRE(mls::create_update_path(group.tree, committer, group.ids[committer].signer, leaf, context, {}, path,
                                    sent) == CryptoError::None);
    group.keys[committer].apply(sent);
    // The others' keys follow the path the way a receiver's do (each processes on its own copy in run_group); here
    // every member re-derives from the committer's path secrets, which is the same key material.
    for (u32 m = 0U; m < members; ++m)
    {
        if (m != committer)
        {
            group.keys[m].prune(group.tree);
            const u32 meet = lowest_common(committer, m, group.tree.leaves());
            REQUIRE(group.keys[m].implant(group.tree, meet, sent.path_secret(meet)) == CryptoError::None);
        }
    }
}

void build_group(Group& group, const mls::CipherSuite& suite, u32 members)
{
    group.suite    = &suite;
    group.group_id = Bytes{0x47U, 0x52U, 0x4FU, 0x55U, 0x50U};
    for (u32 m = 0U; m < members; ++m)
    {
        make_identity(suite, group.ids[m], m);
        if (m == 0U)
        {
            REQUIRE(group.tree.create(suite, group.ids[0].leaf) == CryptoError::None);
        }
        else
        {
            u32 index = 0U;
            REQUIRE(group.tree.add(group.ids[m].leaf, index) == CryptoError::None);
            REQUIRE(index == m);
        }
        REQUIRE(group.keys[m].init(suite, m, view(group.ids[m].hpke_private)) == CryptoError::None);
        commit_in(group, m, m + 1U);
    }
}

const mls::CipherSuite& suite_one()
{
    const mls::CipherSuite* suite = mls::cipher_suite(1U);
    REQUIRE(suite != nullptr);
    return *suite;
}
} // namespace

// ---- The working group's vectors ----

TEST_CASE("MLS: every tree-validation vector of the profile", "[crypto][net-2][mls]")
{
    const JsonFile         vectors = mls_vectors("tree-validation.json");
    const ProviderVerifier verifier;
    usize                  checked = 0U;
    for (u32 v = 0U; v < json::count_of(vectors.doc(), vectors.root()); ++v)
    {
        const u32               vector = json::at(vectors.doc(), vectors.root(), v);
        const mls::CipherSuite* suite  = profile_suite(vectors, vector);
        if (suite == nullptr)
        {
            continue;
        }
        INFO("vector " << v);
        const mls::RatchetTree tree        = decoded_tree(*suite, view(vectors.hex(vector, "tree")));
        const u32              hashes      = json::find(vectors.doc(), vector, "tree_hashes");
        const u32              resolutions = json::find(vectors.doc(), vector, "resolutions");
        REQUIRE(json::count_of(vectors.doc(), hashes) == tree.node_count());
        REQUIRE(json::count_of(vectors.doc(), resolutions) == tree.node_count());
        for (u32 node = 0U; node < tree.node_count(); ++node)
        {
            INFO("node " << node);
            CHECK(equal(tree.tree_hash(node), view(from_hex(vectors.text(json::at(vectors.doc(), hashes, node))))));
            crd::containers::Array<u32> resolved;
            REQUIRE(tree.resolution(node, {}, resolved) == CryptoError::None);
            const u32 expected = json::at(vectors.doc(), resolutions, node);
            REQUIRE(resolved.size() == json::count_of(vectors.doc(), expected));
            for (u32 i = 0U; i < resolved.size(); ++i)
            {
                CHECK(resolved[i] == vectors.number(json::at(vectors.doc(), expected, i)));
            }
        }
        CHECK(equal(tree.root_hash(), tree.tree_hash(tree.leaves() - 1U)));
        CHECK(tree.validate(verifier, view(vectors.hex(vector, "group_id"))) == CryptoError::None);
        ++checked;
    }
    CHECK(checked == 56U); // 14 trees in each of the four suites of the profile
}

TEST_CASE("MLS: every tree-operations vector", "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("tree-operations.json");
    usize          seen[4] = {};
    for (u32 v = 0U; v < json::count_of(vectors.doc(), vectors.root()); ++v)
    {
        const u32               vector = json::at(vectors.doc(), vectors.root(), v);
        const mls::CipherSuite* suite  = profile_suite(vectors, vector);
        REQUIRE(suite != nullptr);
        INFO("vector " << v);
        mls::RatchetTree tree = decoded_tree(*suite, view(vectors.hex(vector, "tree_before")));
        CHECK(equal(tree.root_hash(), view(vectors.hex(vector, "tree_hash_before"))));
        const Bytes   proposal_bytes = vectors.hex(vector, "proposal");
        WireReader    in(view(proposal_bytes));
        mls::Proposal proposal;
        REQUIRE(mls::take_proposal(in, proposal));
        REQUIRE(in.done());
        const u32 sender = static_cast<u32>(vectors.number(vector, "proposal_sender"));
        switch (static_cast<mls::ProposalType>(proposal.type))
        {
        case mls::ProposalType::Add:
        {
            u32 index = 0U;
            REQUIRE(tree.add(proposal.add.leaf_node, index) == CryptoError::None);
            break;
        }
        case mls::ProposalType::Update:
            REQUIRE(tree.update(sender, proposal.update) == CryptoError::None);
            break;
        case mls::ProposalType::Remove:
            REQUIRE(tree.remove(proposal.remove) == CryptoError::None);
            break;
        default:
            FAIL("a proposal the vectors do not hold");
        }
        ++seen[proposal.type];
        CHECK(equal(encoded(tree), view(vectors.hex(vector, "tree_after"))));
        CHECK(equal(tree.root_hash(), view(vectors.hex(vector, "tree_hash_after"))));
    }
    CHECK(seen[1] == 2U); // two adds, one of them growing the tree
    CHECK(seen[2] == 1U);
    CHECK(seen[3] == 2U); // two removes, one of them truncating it
}

TEST_CASE("MLS: every treekem vector of the profile", "[crypto][net-2][mls]")
{
    const JsonFile         vectors = mls_vectors("treekem.json");
    const ProviderVerifier verifier;
    usize                  checked = 0U;
    usize                  paths   = 0U;
    for (u32 v = 0U; v < json::count_of(vectors.doc(), vectors.root()); ++v)
    {
        const u32               vector = json::at(vectors.doc(), vectors.root(), v);
        const mls::CipherSuite* suite  = profile_suite(vectors, vector);
        if (suite == nullptr)
        {
            continue;
        }
        INFO("vector " << v);
        const mls::RatchetTree tree      = decoded_tree(*suite, view(vectors.hex(vector, "ratchet_tree")));
        const Bytes            group_id  = vectors.hex(vector, "group_id");
        const Bytes            confirmed = vectors.hex(vector, "confirmed_transcript_hash");
        const mls::GroupContext base{suite->id, view(group_id), vectors.number(vector, "epoch"), {}, view(confirmed),
                                     {}};

        // Each member's private state from its leaf key and path secrets, consistent with the tree.
        const u32 privates = json::find(vectors.doc(), vector, "leaves_private");
        const u32 members  = json::count_of(vectors.doc(), privates);
        crd::containers::Array<mls::PathKeys> keys;
        keys.resize(members);
        for (u32 m = 0U; m < members; ++m)
        {
            const u32 member = json::at(vectors.doc(), privates, m);
            const u32 index  = static_cast<u32>(vectors.number(member, "index"));
            const Bytes leaf_private = vectors.hex(member, "encryption_priv");
            REQUIRE(keys[m].init(*suite, index, view(leaf_private)) == CryptoError::None);
            HpkeKey leaf_key;
            Bytes   leaf_public = filled(hpke_sizes(suite->kem).public_key, 0U);
            REQUIRE(leaf_key.from_private(suite->kem, view(leaf_private)) == CryptoError::None);
            REQUIRE(leaf_key.public_key(span_of(leaf_public)) == CryptoError::None);
            CHECK(equal(view(leaf_public), tree.encryption_key(2U * index)));
            const u32 secrets = json::find(vectors.doc(), member, "path_secrets");
            for (u32 s = 0U; s < json::count_of(vectors.doc(), secrets); ++s)
            {
                const u32 entry = json::at(vectors.doc(), secrets, s);
                const u32 node  = static_cast<u32>(vectors.number(entry, "node"));
                CHECK(keys[m].set_path_secret(tree, node, view(vectors.hex(entry, "path_secret"))) ==
                      CryptoError::None);
                CHECK(keys[m].holds(node));
            }
        }

        const u32 updates = json::find(vectors.doc(), vector, "update_paths");
        for (u32 p = 0U; p < json::count_of(vectors.doc(), updates); ++p)
        {
            INFO("update path " << p);
            const u32             entry      = json::at(vectors.doc(), updates, p);
            const u32             sender     = static_cast<u32>(vectors.number(entry, "sender"));
            const Bytes           path_bytes = vectors.hex(entry, "update_path");
            const mls::UpdatePath path       = update_path(view(path_bytes));
            CHECK(equal(path_written(path), view(path_bytes)));

            // Parent-hash valid relative to the tree, merged to the tree hash after it.
            mls::RatchetTree merged = copy_of(tree);
            REQUIRE(merged.merge_path(sender, path) == CryptoError::None);
            CHECK(equal(merged.root_hash(), view(vectors.hex(entry, "tree_hash_after"))));
            mls::GroupContext context = base;
            context.tree_hash         = merged.root_hash();
            const Bytes provisional   = context_bytes(context);

            // Every other member decrypts its path secret and reaches the commit secret.
            const u32 expected_secrets = json::find(vectors.doc(), entry, "path_secrets");
            const u32 sender_member    = [&] {
                u32 found = members;
                for (u32 m = 0U; m < members; ++m)
                {
                    found = keys[m].leaf() == sender ? m : found;
                }
                return found;
            }();
            REQUIRE(sender_member != members);
            for (u32 m = 0U; m < members; ++m)
            {
                if (m == sender_member)
                {
                    continue;
                }
                const u32 leaf = keys[m].leaf();
                INFO("member " << leaf);
                mls::PathUpdate received;
                REQUIRE(mls::process_update_path(keys[m], merged, sender, path, view(provisional), {}, received) ==
                        CryptoError::None);
                const u32 meet = lowest_common(sender, leaf, tree.leaves());
                CHECK(equal(received.path_secret(meet),
                            view(from_hex(vectors.text(json::at(vectors.doc(), expected_secrets, leaf))))));
                CHECK(equal(received.commit_secret(), view(vectors.hex(entry, "commit_secret"))));
            }

            // A new path from the sender, which every other member merges and decrypts to the same commit secret.
            SigningKey signer;
            REQUIRE(signer.from_seed(suite->signature,
                                     view(vectors.hex(json::at(vectors.doc(), privates, sender_member),
                                                      "signature_priv"))) == CryptoError::None);
            mls::LeafNode template_leaf{};
            REQUIRE(tree.leaf_node(sender, template_leaf));
            mls::RatchetTree           mine = copy_of(tree);
            crd::containers::Array<u8> fresh;
            mls::PathUpdate            sent;
            REQUIRE(mls::create_update_path(mine, sender, signer, template_leaf, base, {}, fresh, sent) ==
                    CryptoError::None);
            CHECK(mine.validate(verifier, view(group_id)) == CryptoError::None);
            const mls::UpdatePath new_path = update_path(view(fresh));
            for (u32 m = 0U; m < members; ++m)
            {
                if (m == sender_member)
                {
                    continue;
                }
                mls::RatchetTree theirs = copy_of(tree);
                REQUIRE(theirs.merge_path(sender, new_path) == CryptoError::None);
                CHECK(equal(encoded(theirs), view(encoded(mine))));
                context.tree_hash = theirs.root_hash();
                mls::PathUpdate received;
                REQUIRE(mls::process_update_path(keys[m], theirs, sender, new_path, view(context_bytes(context)), {},
                                                 received) == CryptoError::None);
                CHECK(equal(received.commit_secret(), sent.commit_secret()));
            }
            ++paths;
        }
        ++checked;
    }
    CHECK(checked == 44U); // 11 trees in each of the four suites of the profile
    CHECK(paths > checked);
}

// ---- Groups at random ----

TEST_CASE("MLS: groups that add, update, remove and commit at random keep one tree and TreeKEM's keys",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("suite " << suite.id);
        run_group(suite, 0x9E3779B97F4A7C15ULL + suite.id, suite.id == 1U ? 160U : 80U);
    }
}

TEST_CASE("MLS: a tree grows by doubling, truncates its blank right half and writes no trailing blank",
          "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_one();
    Identity                ids[5];
    mls::RatchetTree        tree;
    make_identity(suite, ids[0], 0U);
    REQUIRE(tree.create(suite, ids[0].leaf) == CryptoError::None);
    CHECK(tree.leaves() == 1U);
    CHECK(tree.node_count() == 1U);
    const u32 sizes[5] = {1U, 2U, 4U, 4U, 8U};
    for (u32 m = 1U; m < 5U; ++m)
    {
        make_identity(suite, ids[m], m);
        u32 index = 0U;
        REQUIRE(tree.add(ids[m].leaf, index) == CryptoError::None);
        CHECK(index == m);
        CHECK(tree.leaves() == sizes[m]);
    }
    // Five members in eight leaves: the encoding ends at leaf 4 (node 8), not at node 14.
    const Bytes   grown = encoded(tree);
    WireReader    in(view(grown));
    ConstSpan<u8> body;
    REQUIRE(mls::take_vector(in, body));
    usize nodes = 0U;
    for (WireReader items(body); !items.done(); ++nodes)
    {
        bool present = false;
        REQUIRE(mls::take_presence(items, present));
        if (present)
        {
            u8 type = 0U;
            REQUIRE(items.take_u8(type));
            mls::LeafNode   leaf{};
            mls::ParentNode parent{};
            REQUIRE((type == 1U ? mls::take_leaf_node(items, leaf) : mls::take_parent_node(items, parent)));
        }
    }
    CHECK(nodes == 9U);

    // Removing a middle member leaves the size; removing the last halves it while its right half is blank.
    REQUIRE(tree.remove(2U) == CryptoError::None);
    CHECK(tree.leaves() == 8U);
    REQUIRE(tree.remove(4U) == CryptoError::None);
    CHECK(tree.leaves() == 4U);
    REQUIRE(tree.remove(3U) == CryptoError::None);
    CHECK(tree.leaves() == 2U);
    // A new member grows the tree again and takes the leftmost blank leaf.
    u32 index = 0U;
    REQUIRE(tree.add(ids[4].leaf, index) == CryptoError::None);
    CHECK(index == 2U);
    CHECK(tree.leaves() == 4U);
    REQUIRE(tree.remove(1U) == CryptoError::None);
    REQUIRE(tree.remove(2U) == CryptoError::None);
    CHECK(tree.leaves() == 1U);
    REQUIRE(tree.remove(0U) == CryptoError::None);
    CHECK(tree.leaves() == 1U);
    CHECK(tree.blank(0U));
    CHECK(tree.encoded_bytes() == 1U); // an empty list: no member left to send
}

// ---- Refusals ----

TEST_CASE("MLS: the ratchet tree's decoder refuses what section 12.4.3.3 does not allow", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_one();
    Group                   group;
    build_group(group, suite, 3U);
    const crd::containers::Array<Bytes> nodes = nodes_of(group.tree);
    REQUIRE(nodes.size() == 5U); // three leaves and the two parents between them; leaf 3 blank and dropped
    mls::RatchetTree tree;
    CHECK(tree.decode(suite, {}) == CryptoError::Malformed);
    const u8 empty[1] = {0x00U};
    CHECK(tree.decode(suite, empty) == CryptoError::Malformed);
    const u8 one_blank[2] = {0x01U, 0x00U};
    CHECK(tree.decode(suite, one_blank) == CryptoError::Malformed); // its last node is blank

    crd::containers::Array<Bytes> trailing = nodes;
    trailing.push_back(Bytes());
    CHECK(tree.decode(suite, view(tree_of(trailing))) == CryptoError::Malformed);

    // The header's length, then a parent where a leaf belongs and the reverse.
    const Bytes   whole = tree_of(nodes);
    WireReader    outer(view(whole));
    ConstSpan<u8> body;
    REQUIRE(mls::take_vector(outer, body));
    const usize head    = whole.size() - body.size();
    Bytes       swapped = whole;
    swapped[head + 1U]  = 2U; // node 0's type
    CHECK(tree.decode(suite, view(swapped)) == CryptoError::Malformed);
    crd::containers::Array<Bytes> parent_first;
    parent_first.push_back(nodes[1]);
    CHECK(tree.decode(suite, view(tree_of(parent_first))) == CryptoError::Malformed);

    // A presence octet of 2, an unknown node type, a byte past the vector, a non-minimal header and a truncation.
    Bytes presence = whole;
    presence[head] = 2U;
    CHECK(tree.decode(suite, view(presence)) == CryptoError::Malformed);
    Bytes type      = whole;
    type[head + 1U] = 3U;
    CHECK(tree.decode(suite, view(type)) == CryptoError::Malformed);
    Bytes extra = whole;
    extra.push_back(0U);
    CHECK(tree.decode(suite, view(extra)) == CryptoError::Malformed);
    const u8 long_header[3] = {0x40U, 0x01U, 0x00U};
    CHECK(tree.decode(suite, long_header) == CryptoError::Malformed);
    CHECK(tree.decode(suite, ConstSpan<u8>(whole.data(), whole.size() - 1U)) == CryptoError::Malformed);
    CHECK(!tree.ready());

    // More nodes than a tree of 2^20 leaves holds.
    const usize max_nodes = mls::tree::node_width(mls::kMaxSecretTreeLeaves);
    Bytes       huge      = filled(mls::vector_bytes(max_nodes + 1U), 0U);
    WireWriter  writer(span_of(huge));
    mls::put_varint(writer, max_nodes + 1U);
    CHECK(tree.decode(suite, view(huge)) == CryptoError::BadLength);

    // A valid tree decodes after the refusals, and a tree of one leaf and an extension of odd length round-trip.
    CHECK(equal(encoded(decoded_tree(suite, view(whole))), view(whole)));
    crd::containers::Array<Bytes> single;
    single.push_back(nodes[0]);
    CHECK(decoded_tree(suite, view(tree_of(single))).leaves() == 1U);
}

TEST_CASE("MLS: a joined tree's checks refuse each fault of sections 7.3, 7.9.2 and 12.4.3.1", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_one();
    const ProviderVerifier  verifier;
    Group                   group;
    build_group(group, suite, 5U);
    // Members 5 and 6 added after member 4's commit set the root: the root lists both as unmerged.
    for (u32 m = 5U; m < 7U; ++m)
    {
        make_identity(suite, group.ids[m], m);
        u32 index = 0U;
        REQUIRE(group.tree.add(group.ids[m].leaf, index) == CryptoError::None);
        REQUIRE(index == m);
    }
    const ConstSpan<u8> group_id = view(group.group_id);
    REQUIRE(group.tree.validate(verifier, group_id) == CryptoError::None);
    const crd::containers::Array<Bytes> nodes = nodes_of(group.tree);
    mls::ParentNode                     root{};
    REQUIRE(group.tree.parent_node(7U, root));
    REQUIRE(root.unmerged_count() == 2U);

    const auto with_root_unmerged = [&](std::initializer_list<u32> leaves) {
        crd::containers::Array<Bytes> changed = nodes;
        mls::ParentNode               node    = root;
        const Bytes                   list    = unmerged_list(leaves);
        node.unmerged_leaves                  = view(list);
        changed[7]                            = parent_bytes(node);
        return decoded_tree(suite, view(tree_of(changed)));
    };
    CHECK(with_root_unmerged({6U, 5U}).validate(verifier, group_id) == CryptoError::Malformed); // not increasing
    CHECK(with_root_unmerged({5U, 5U}).validate(verifier, group_id) == CryptoError::Malformed); // repeated
    CHECK(with_root_unmerged({5U, 7U}).validate(verifier, group_id) == CryptoError::Malformed); // a blank leaf
    CHECK(with_root_unmerged({5U, 9U}).validate(verifier, group_id) == CryptoError::Malformed); // outside the tree
    // Leaf 0 listed at the root, but not by the non-blank nodes between it and the root.
    CHECK(with_root_unmerged({0U, 5U, 6U}).validate(verifier, group_id) == CryptoError::Malformed);
    // The root listing one of its two unmerged leaves: leaf 6 is in the resolution with no chain to the root.
    CHECK(with_root_unmerged({5U}).validate(verifier, group_id) == CryptoError::Forged);
    {
        // Node 1 (above leaves 0 and 1) listing leaf 5, which is not below it.
        crd::containers::Array<Bytes> changed = nodes;
        mls::ParentNode               node{};
        REQUIRE(group.tree.parent_node(1U, node));
        const Bytes list     = unmerged_list({5U});
        node.unmerged_leaves = view(list);
        changed[1]           = parent_bytes(node);
        CHECK(decoded_tree(suite, view(tree_of(changed))).validate(verifier, group_id) == CryptoError::Malformed);
    }
    {
        // A parent between leaf 5 and the root holding a key, but not listing leaf 5 (section 12.4.3.1).
        crd::containers::Array<Bytes> changed = nodes;
        mls::ParentNode               between{};
        const Bytes                   key = filled(32U, 0x42U);
        between.encryption_key          = view(key);
        changed[11]                     = parent_bytes(between);
        CHECK(decoded_tree(suite, view(tree_of(changed))).validate(verifier, group_id) == CryptoError::Malformed);
    }
    {
        // Two nodes with one encryption key, and two leaves with one signature key.
        crd::containers::Array<Bytes> changed = nodes;
        mls::ParentNode               node{};
        REQUIRE(group.tree.parent_node(1U, node));
        mls::LeafNode leaf{};
        REQUIRE(group.tree.leaf_node(2U, leaf));
        node.encryption_key = leaf.encryption_key;
        changed[1]          = parent_bytes(node);
        CHECK(decoded_tree(suite, view(tree_of(changed))).validate(verifier, group_id) == CryptoError::BadKey);
        changed = nodes;
        mls::LeafNode other{};
        REQUIRE(group.tree.leaf_node(3U, other));
        leaf.signature_key = other.signature_key;
        changed[4]         = leaf_bytes(leaf);
        CHECK(decoded_tree(suite, view(tree_of(changed))).validate(verifier, group_id) == CryptoError::BadKey);
    }
    {
        // A leaf's signature changed, a leaf signed for another group, and a parent hash changed.
        crd::containers::Array<Bytes> changed = nodes;
        Bytes                         bad     = nodes[2];
        bad.back() ^= 0x01U;
        changed[2] = bad;
        CHECK(decoded_tree(suite, view(tree_of(changed))).validate(verifier, group_id) == CryptoError::Forged);
        const u8 other_group[3] = {1U, 2U, 3U};
        CHECK(group.tree.validate(verifier, other_group) == CryptoError::Forged);
        changed = nodes;
        mls::ParentNode node{};
        REQUIRE(group.tree.parent_node(5U, node)); // set by member 3's path below node 3
        REQUIRE(!node.parent_hash.empty());
        Bytes hash = filled(node.parent_hash.size(), 0U);
        std::copy(node.parent_hash.begin(), node.parent_hash.end(), hash.begin());
        hash[0] ^= 0x01U;
        node.parent_hash = view(hash);
        changed[5]       = parent_bytes(node);
        CHECK(decoded_tree(suite, view(tree_of(changed))).validate(verifier, group_id) == CryptoError::Forged);
    }
    {
        // A parent whose key changed: the node below it no longer chains to it.
        crd::containers::Array<Bytes> changed = nodes;
        mls::ParentNode               node{};
        REQUIRE(group.tree.parent_node(3U, node));
        const Bytes key     = filled(node.encryption_key.size(), 0x24U);
        node.encryption_key = view(key);
        changed[3]          = parent_bytes(node);
        CHECK(decoded_tree(suite, view(tree_of(changed))).validate(verifier, group_id) == CryptoError::Forged);
    }
    {
        // Member 0's key package leaf (validly signed) where its commit leaf was: node 1's chain needs a commit leaf
        // below it, and leaf 1's parent hash covered the leaf it replaces.
        crd::containers::Array<Bytes> changed = nodes;
        changed[0]                            = leaf_bytes(group.ids[0].leaf);
        CHECK(decoded_tree(suite, view(tree_of(changed))).validate(verifier, group_id) == CryptoError::Forged);
    }
    mls::RatchetTree empty;
    CHECK(empty.validate(verifier, group_id) == CryptoError::NotReady);
}

TEST_CASE("MLS: merging and decrypting an update path refuse a hostile path and change nothing",
          "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_one();
    Group                   group;
    build_group(group, suite, 4U);
    const Bytes             transcript = filled(suite.hash_bytes(), 0x5AU);
    mls::GroupContext       context{suite.id, view(group.group_id), 2U, {}, view(transcript), {}};
    mls::LeafNode           leaf{};
    REQUIRE(group.tree.leaf_node(0U, leaf));
    mls::RatchetTree           mine = copy_of(group.tree);
    crd::containers::Array<u8> path_bytes;
    mls::PathUpdate            sent;
    REQUIRE(mls::create_update_path(mine, 0U, group.ids[0].signer, leaf, context, {}, path_bytes, sent) ==
            CryptoError::None);
    const mls::UpdatePath path   = update_path(view(path_bytes));
    const Bytes           before = encoded(group.tree);
    const auto            merge  = [&](const mls::UpdatePath& hostile) {
        mls::RatchetTree  tree  = copy_of(group.tree);
        const CryptoError error = tree.merge_path(0U, hostile);
        CHECK(equal(encoded(tree), view(before))); // a refusal changes nothing
        return error;
    };

    // One node too few, a leaf of another source, a parent hash changed, the old leaf key, another member's key,
    // and a key the path repeats.
    WireReader          nodes(path.nodes);
    mls::UpdatePathNode first{};
    REQUIRE(mls::take_update_path_node(nodes, first));
    mls::UpdatePath shorter = path;
    shorter.nodes           = path.nodes.subspan(path.nodes.size() - nodes.rest().size());
    CHECK(merge(shorter) == CryptoError::Malformed);
    mls::UpdatePath update_source = path;
    update_source.leaf_node.source = mls::LeafNodeSource::Update;
    CHECK(merge(update_source) == CryptoError::Malformed);
    Bytes hash = filled(path.leaf_node.parent_hash.size(), 0U);
    std::copy(path.leaf_node.parent_hash.begin(), path.leaf_node.parent_hash.end(), hash.begin());
    hash[3] ^= 0x10U;
    mls::UpdatePath forged_hash = path;
    forged_hash.leaf_node.parent_hash = view(hash);
    CHECK(merge(forged_hash) == CryptoError::Forged);
    mls::UpdatePath old_key = path;
    old_key.leaf_node.encryption_key = leaf.encryption_key;
    CHECK(merge(old_key) == CryptoError::BadKey);
    mls::LeafNode other{};
    REQUIRE(group.tree.leaf_node(3U, other));
    mls::UpdatePath stolen = path;
    stolen.leaf_node.encryption_key = other.encryption_key;
    CHECK(merge(stolen) == CryptoError::BadKey);
    mls::UpdatePath repeated = path;
    repeated.leaf_node.encryption_key = first.encryption_key;
    CHECK(merge(repeated) == CryptoError::BadKey);
    mls::RatchetTree not_ready;
    CHECK(not_ready.merge_path(0U, path) == CryptoError::NotReady);
    CHECK(copy_of(group.tree).merge_path(7U, path) == CryptoError::OutOfWindow);

    // Decryption: member 1 on the honestly merged tree.
    mls::RatchetTree merged = copy_of(group.tree);
    REQUIRE(merged.merge_path(0U, path) == CryptoError::None);
    context.tree_hash  = merged.root_hash();
    const Bytes good   = context_bytes(context);
    const auto  decrypt = [&](u32 member, const mls::UpdatePath& received, ConstSpan<u8> ctx,
                             ConstSpan<u32> excluded) {
        mls::PathUpdate   update;
        const CryptoError error =
            mls::process_update_path(group.keys[member], merged, 0U, received, ctx, excluded, update);
        if (error != CryptoError::None)
        {
            CHECK(update.commit_secret().empty()); // a refusal leaves nothing behind
            CHECK(update.path_secret(1U).empty());
        }
        return error;
    };
    CHECK(decrypt(1U, path, view(good), {}) == CryptoError::None);
    CHECK(decrypt(0U, path, view(good), {}) == CryptoError::Malformed); // the sender itself
    Bytes other_context = good;
    other_context.back() ^= 0x01U;
    CHECK(decrypt(1U, path, view(other_context), {}) == CryptoError::Forged);
    const u32 excluded_leaf[1] = {1U};
    CHECK(decrypt(1U, path, view(good), excluded_leaf) == CryptoError::Malformed); // no ciphertext for itself
    // A ciphertext changed in place.
    Bytes tampered = filled(path_bytes.size(), 0U);
    std::copy(path_bytes.begin(), path_bytes.end(), tampered.begin());
    tampered.back() ^= 0x01U;
    CHECK(decrypt(3U, update_path(view(tampered)), view(good), {}) == CryptoError::Forged);
    // A path whose public keys are not the secrets' (member 2 derives node 3's key from node 1's secret).
    mls::UpdatePath swapped_keys = path;
    Bytes           nodes_copy   = filled(path.nodes.size(), 0U);
    std::copy(path.nodes.begin(), path.nodes.end(), nodes_copy.begin());
    nodes_copy[1] ^= 0x01U; // the first node's key (its vector header is one byte)
    swapped_keys.nodes = view(nodes_copy);
    CHECK(decrypt(1U, swapped_keys, view(good), {}) == CryptoError::BadKey);
    mls::PathKeys   none;
    mls::PathUpdate update;
    CHECK(mls::process_update_path(none, merged, 0U, path, view(good), {}, update) == CryptoError::NotReady);
}

TEST_CASE("MLS: tree changes, path keys and path creation refuse what they cannot do", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_one();
    Group                   group;
    build_group(group, suite, 3U);
    mls::RatchetTree& tree = group.tree;
    const Bytes       before = encoded(tree);

    // Members that are not there; a leaf that does not encode; a tree that is not.
    CHECK(tree.update(3U, group.ids[1].leaf) == CryptoError::OutOfWindow);
    CHECK(tree.update(9U, group.ids[1].leaf) == CryptoError::OutOfWindow);
    CHECK(tree.remove(3U) == CryptoError::OutOfWindow);
    CHECK(tree.remove(1000U) == CryptoError::OutOfWindow);
    mls::LeafNode unencodable = group.ids[1].leaf;
    unencodable.credential.type = 0U;
    u32 index = 0U;
    CHECK(tree.add(unencodable, index) == CryptoError::BadLength);
    CHECK(tree.update(1U, unencodable) == CryptoError::BadLength);
    CHECK(equal(encoded(tree), view(before)));
    mls::RatchetTree empty;
    CHECK(empty.add(group.ids[1].leaf, index) == CryptoError::NotReady);
    CHECK(empty.blank(0U));
    CHECK(empty.root_hash().empty());
    CHECK(tree.tree_hash(tree.node_count()).empty());
    mls::LeafNode   leaf{};
    mls::ParentNode parent{};
    CHECK(!tree.leaf_node(3U, leaf));
    CHECK(!tree.parent_node(0U, parent));
    CHECK(tree.encryption_key(6U).empty());

    // Path keys: a node not above the leaf, a secret of another length, a secret that is not the node's.
    mls::PathKeys& keys   = group.keys[0];
    const Bytes    secret = filled(suite.hash_bytes(), 0x11U);
    CHECK(keys.set_path_secret(tree, 5U, view(secret)) == CryptoError::OutOfWindow);
    CHECK(keys.set_path_secret(tree, 0U, view(secret)) == CryptoError::OutOfWindow);
    CHECK(keys.set_path_secret(tree, 1U, ConstSpan<u8>(secret.data(), 16U)) == CryptoError::BadLength);
    CHECK(keys.set_path_secret(tree, 1U, view(secret)) == CryptoError::BadKey);
    CHECK(keys.implant(tree, 3U, view(secret)) == CryptoError::BadKey);
    CHECK(keys.holds(1U)); // the refused secrets left the held keys
    mls::PathKeys none;
    CHECK(none.set_path_secret(tree, 1U, view(secret)) == CryptoError::NotReady);
    CHECK(!none.holds(0U));
    // Pruning drops the keys of a path blanked by another member's update, and keeps the rest.
    REQUIRE(keys.holds(3U));
    Identity fresh;
    make_identity(suite, fresh, 9U);
    sign_leaf(suite, fresh, mls::LeafNodeSource::Update, view(group.group_id), 2U);
    REQUIRE(tree.update(2U, fresh.leaf) == CryptoError::None);
    keys.prune(tree);
    CHECK(keys.holds(1U));
    CHECK(!keys.holds(3U));
    // A confirmed update replaces every level from its lowest: a level it holds no key for is left empty.
    mls::PathUpdate nothing;
    nothing.replaced = 1U;
    keys.apply(nothing);
    CHECK(!keys.holds(1U));
    CHECK(keys.holds(0U)); // the leaf key stays unless the update brings one

    // Path creation: a blank or outside sender, a signer of another algorithm.
    const mls::GroupContext    context{suite.id, view(group.group_id), 1U, {}, {}, {}};
    crd::containers::Array<u8> path;
    mls::PathUpdate            update;
    REQUIRE(tree.leaf_node(0U, leaf));
    CHECK(mls::create_update_path(tree, 3U, group.ids[0].signer, leaf, context, {}, path, update) ==
          CryptoError::OutOfWindow);
    SigningKey ecdsa;
    REQUIRE(ecdsa.generate(AlgorithmId::EcdsaSecp256r1Sha256) == CryptoError::None);
    CHECK(mls::create_update_path(tree, 0U, ecdsa, leaf, context, {}, path, update) == CryptoError::Unsupported);
    CHECK(path.empty());
    CHECK(update.commit_secret().empty());
    CHECK(mls::create_update_path(empty, 0U, group.ids[0].signer, leaf, context, {}, path, update) ==
          CryptoError::NotReady);
}

TEST_CASE("MLS: a lone member's path and a Welcome's path secret", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_one();
    const ProviderVerifier  verifier;
    Identity                id;
    make_identity(suite, id, 0U);
    mls::RatchetTree tree;
    REQUIRE(tree.create(suite, id.leaf) == CryptoError::None);
    const Bytes                group_id = filled(8U, 0x33U);
    const mls::GroupContext    context{suite.id, view(group_id), 1U, {}, {}, {}};
    crd::containers::Array<u8> path;
    mls::PathUpdate            update;
    REQUIRE(mls::create_update_path(tree, 0U, id.signer, id.leaf, context, {}, path, update) == CryptoError::None);
    // No filtered direct path: no node, an empty parent hash, and still a fresh commit secret.
    const mls::UpdatePath decoded = update_path(view(path));
    CHECK(decoded.nodes.empty());
    CHECK(decoded.leaf_node.parent_hash.empty());
    CHECK(decoded.leaf_node.source == mls::LeafNodeSource::Commit);
    CHECK(update.commit_secret().size() == suite.hash_bytes());
    CHECK(!all_zero(update.commit_secret()));
    CHECK(tree.validate(verifier, view(group_id)) == CryptoError::None);
    mls::PathKeys keys;
    REQUIRE(keys.init(suite, 0U, view(id.hpke_private)) == CryptoError::None);
    keys.apply(update);
    CHECK(keys.holds(0U));
}
