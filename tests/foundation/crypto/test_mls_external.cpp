// crd-crypto -- MLS external joins, RFC 9420 sections 8.3 and 12.4.3.2 (NET.2; docs/design/network-foundation.md#mls,
// M3 layer 10a and decision M11). The working group's vectors hold no external commit (none of the 382 commits of the
// passive-client vectors has a new_member_commit sender), so the joiner and the members are held to each other and to
// the RFC's rules: groups of Cerid members in every suite that new members join from a published GroupInfo (with and
// without its tree, with a PSK, one after another and racing one another), that members resync into from their former
// leaves, and that then commit, welcome and exchange messages with every member agreeing; every refusal on both sides,
// the joiner left empty and each member's group as it was; the tree operations an external joiner takes; and GREASE
// (section 13.5) where a GroupInfo, a key package and a leaf carry it and where no member may send it.

#include "mls_model.hpp"

#include <crd/crypto/hpke.hpp>
#include <crd/crypto/mls_group.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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
namespace mls = crd::crypto::mls;

Bytes bytes_of(const crd::containers::Array<u8>& array)
{
    return Bytes(array.begin(), array.end());
}

// The GroupInfo a member publishes for external joiners.
Bytes published(const Member& member, bool ratchet_tree = true, bool external_pub = true)
{
    mls::GroupInfoOptions options;
    options.ratchet_tree = ratchet_tree;
    options.external_pub = external_pub;
    crd::containers::Array<u8> out;
    REQUIRE(member.group.group_info(member.who.id.signer, options, out) == CryptoError::None);
    return bytes_of(out);
}

// The ratchet tree a joiner fetches beside a GroupInfo that does not carry it.
Bytes fetched_tree(const mls::Group& group)
{
    Bytes      tree = filled(group.tree().encoded_bytes(), 0U);
    WireWriter writer(span_of(tree));
    group.tree().encode(writer);
    return tree;
}

// A GroupInfo MLSMessage decoded (its fields view `info`).
mls::GroupInfo decoded_info(const Bytes& info)
{
    mls::Message message;
    REQUIRE(mls::take_message(view(info), message));
    REQUIRE(message.wire_format == mls::WireFormat::GroupInfo);
    WireReader     in(message.body);
    mls::GroupInfo decoded;
    REQUIRE(mls::take_group_info(in, decoded));
    REQUIRE(in.done());
    return decoded;
}

// `info` with other extensions or another confirmation tag, signed again by `signer` (the member at the GroupInfo's
// signer leaf), so a joiner's refusal is the change's and not the signature's.
Bytes resigned(const mls::CipherSuite& suite, const Bytes& info, const Member& signer, const ConstSpan<u8>* extensions,
               const ConstSpan<u8>* tag = nullptr)
{
    mls::GroupInfo decoded = decoded_info(info);
    if (extensions != nullptr)
    {
        decoded.extensions = *extensions;
    }
    if (tag != nullptr)
    {
        decoded.confirmation_tag = *tag;
    }
    Bytes signature = filled(signature_sizes(suite.signature).signature, 0U);
    usize written   = 0U;
    REQUIRE(mls::sign_group_info(suite, signer.who.id.signer, decoded, span_of(signature), written) ==
            CryptoError::None);
    signature.resize(written);
    decoded.signature = view(signature);
    Bytes      out    = filled(4U + mls::group_info_bytes(decoded), 0U);
    WireWriter writer(span_of(out));
    writer.put_u16(mls::kProtocolVersion);
    writer.put_u16(static_cast<u16>(mls::WireFormat::GroupInfo));
    mls::put_group_info(writer, decoded);
    return out;
}

// `joiner` joins the model's group externally from `info`; its commit is delivered to and merged by every other active
// member, and every member then agrees.
mls::ExternalJoinResult join_externally(Model& model, usize joiner, const Bytes& info,
                                        const mls::ExternalJoinOptions& options = {}, ConstSpan<u8> tree = {})
{
    Member&                 member = model.at(joiner);
    mls::ExternalJoinResult result;
    REQUIRE(member.group.join_external(member.who.id.signer, member.who.id.leaf, view(info), tree, options,
                                       member.config, result) == CryptoError::None);
    CHECK(wire_format_of(result.message) == mls::WireFormat::PublicMessage);
    CHECK(wire_format_of(result.group_info) == mls::WireFormat::GroupInfo);
    model.deliver(joiner, result.message);
    model.check_agreement();
    return result;
}

// An external commit's content decoded from its message (views of `message`).
mls::Commit commit_in(const Bytes& message, mls::Message& decoded)
{
    REQUIRE(mls::take_message(view(message), decoded));
    REQUIRE(decoded.wire_format == mls::WireFormat::PublicMessage);
    CHECK(decoded.public_message.content.sender.type == mls::SenderType::NewMemberCommit);
    CHECK(decoded.public_message.membership_tag.empty());
    WireReader  in(decoded.public_message.content.body);
    mls::Commit commit;
    REQUIRE(mls::take_commit(in, commit));
    REQUIRE(in.done());
    REQUIRE(commit.has_path);
    return commit;
}

Bytes path_of(const mls::Commit& commit)
{
    Bytes      out = filled(mls::update_path_bytes(commit.path), 0U);
    WireWriter writer(span_of(out));
    mls::put_update_path(writer, commit.path);
    return out;
}

mls::Proposal external_init_of(ConstSpan<u8> kem_output)
{
    mls::Proposal proposal;
    proposal.type       = static_cast<u16>(mls::ProposalType::ExternalInit);
    proposal.kem_output = kem_output;
    return proposal;
}

// A public key of the suite's KEM that hpke_check_public_key refuses: a DHKEM's all-zero key (off the curve, of small
// order for X25519), and all one bits for a KEM with an ML-KEM part, whose coefficients then exceed q (FIPS 203's input
// check; an all-zero ML-KEM key passes it).
Bytes unusable_key(const mls::CipherSuite& suite)
{
    const bool lattice = algorithm_class(suite.kem) != AlgorithmClass::ClassicalKeyExchange;
    return filled(hpke_sizes(suite.kem).public_key, lattice ? 0xFFU : 0x00U);
}

// How the members refuse an ExternalInit whose kem_output is all zero bytes: WeakSecret for DHKEM(X25519)'s point of
// small order, BadKey where a NIST curve's point (alone or a hybrid's half) is off the curve; an ML-KEM ciphertext
// always decapsulates (implicit rejection), as does X-Wing's (its X25519 half taken as RFC 7748 computes it), so the
// init secret is another and the commit fails at its confirmation tag.
CryptoError zero_output_refusal(const mls::CipherSuite& suite)
{
    switch (suite.kem)
    {
    case AlgorithmId::X25519:
        return CryptoError::WeakSecret;
    case AlgorithmId::MlKem768:
    case AlgorithmId::MlKem1024:
    case AlgorithmId::X25519Mlkem768:
        return CryptoError::Forged;
    default:
        return CryptoError::BadKey;
    }
}

// A fresh encapsulation to the external key `info` publishes, as an ExternalInit's kem_output.
Bytes fresh_kem_output(const mls::CipherSuite& suite, const Bytes& info)
{
    const mls::GroupInfo decoded = decoded_info(info);
    ConstSpan<u8>        data;
    ConstSpan<u8>        key;
    REQUIRE(mls::find_extension(decoded.extensions, static_cast<u16>(mls::ExtensionType::ExternalPub), data));
    REQUIRE(mls::take_external_pub(data, key));
    Bytes enc    = filled(hpke_sizes(suite.kem).encapsulation, 0U);
    Bytes secret = filled(suite.hash_bytes(), 0U);
    REQUIRE(hpke_export(suite.hpke(), key, {}, ConstSpan<u8>(), span_of(enc), span_of(secret)) == CryptoError::None);
    return enc;
}

// The members' view of every active member: each processes `message` and merges it.
void deliver_all(Model& model, const crd::containers::Array<u8>& message, u32 joiner_leaf)
{
    for (usize i = 0U; i < kMembers; ++i)
    {
        if (!model.active(i))
        {
            continue;
        }
        INFO("receiver " << i);
        mls::ReceivedMessage received;
        REQUIRE(model.at(i).group.process(ConstSpan<u8>(message.data(), message.size()), received) ==
                CryptoError::None);
        CHECK(received.commit().committer().type == mls::SenderType::NewMemberCommit);
        CHECK(received.commit().committer().index == joiner_leaf);
        REQUIRE(model.at(i).group.merge(received.commit()) == CryptoError::None);
    }
}

TEST_CASE("MLS: new members join by external commits in every suite, and the group goes on", "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        Model               model(suite);
        const mls::Proposal adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
        mls::CommitOptions  options;
        options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
        model.commit(0U, options, {1U, 2U});
        const mls::SendOptions private_send;

        // Member 3 joins from member 0's GroupInfo, which carries the tree and the external key: the leftmost free
        // leaf, one epoch on, every member agreeing; its own GroupInfo carries the external key again.
        const u64                before = model.at(0).group.epoch();
        const Bytes              aad    = Bytes{'j', 'o', 'i', 'n'};
        mls::ExternalJoinOptions first_options;
        first_options.authenticated_data      = view(aad);
        first_options.group_info.external_pub = true;
        const mls::ExternalJoinResult first   = join_externally(model, 3U, published(model.at(0)), first_options);
        CHECK(model.at(3).group.own_leaf() == 3U);
        CHECK(model.at(3).group.epoch() == before + 1U);
        CHECK(model.at(3).group.tree().leaves() == 4U);
        const Bytes       first_message = bytes_of(first.message);
        mls::Message      decoded;
        const mls::Commit commit = commit_in(first_message, decoded);
        CHECK(equal(decoded.public_message.content.authenticated_data, view(aad)));
        CHECK(commit.path.leaf_node.source == mls::LeafNodeSource::Commit);
        const Bytes          first_bytes = bytes_of(first.group_info);
        const mls::GroupInfo first_info  = decoded_info(first_bytes);
        ConstSpan<u8>        ignored;
        CHECK(first_info.signer == 3U);
        CHECK(mls::find_extension(first_info.extensions, static_cast<u16>(mls::ExtensionType::RatchetTree), ignored));
        CHECK(mls::find_extension(first_info.extensions, static_cast<u16>(mls::ExtensionType::ExternalPub), ignored));
        model.exchange(private_send);

        // Member 4 joins from a GroupInfo without the tree (fetched beside it): the tree doubles for it. Its own
        // GroupInfo carries neither the tree nor the external key (nor GREASE, asked not to).
        mls::ExternalJoinOptions plain;
        plain.group_info.ratchet_tree = false;
        plain.group_info.grease       = false;
        const mls::ExternalJoinResult second = join_externally(model, 4U, published(model.at(3), false), plain,
                                                               view(fetched_tree(model.at(3).group)));
        CHECK(model.at(4).group.own_leaf() == 4U);
        CHECK(model.at(4).group.tree().leaves() == 8U);
        CHECK(decoded_info(bytes_of(second.group_info)).extensions.empty());

        // Member 5 joins with an external PSK every member holds.
        const Bytes         nonce = random_of(suite.hash_bytes());
        mls::PreSharedKeyId external;
        external.id    = view(model.external_id);
        external.nonce = view(nonce);
        mls::ExternalJoinOptions with_psk;
        with_psk.psks = ConstSpan<mls::PreSharedKeyId>(&external, 1U);
        (void)join_externally(model, 5U, published(model.at(4)), with_psk);
        CHECK(model.at(5).group.own_leaf() == 5U);

        // Member 1 lost its state and resyncs: its commit removes its former leaf, which it takes again.
        mls::ExternalJoinOptions resync_one;
        resync_one.resync = 1U;
        (void)join_externally(model, 1U, published(model.at(2)), resync_one);
        CHECK(model.at(1).group.own_leaf() == 1U);
        model.exchange(private_send);

        // The joiners commit and welcome like any member.
        const mls::Proposal add_six[1] = {add_of(model.at(6))};
        mls::CommitOptions  welcome;
        welcome.proposals               = ConstSpan<mls::Proposal>(add_six, 1U);
        welcome.group_info.external_pub = true; // the Welcome's GroupInfo carries the key, which its joiner ignores
        model.commit(3U, welcome, {6U});
        model.commit(5U, {});
        model.commit(1U, {});
        model.exchange(private_send);

        // Member 2 resyncs while its former state still runs: that state processes the commit, which removes it.
        Member&                  two = model.at(2);
        mls::Group               fresh;
        mls::ExternalJoinResult  resynced;
        mls::ExternalJoinOptions resync_two;
        resync_two.resync = 2U;
        REQUIRE(fresh.join_external(two.who.id.signer, two.who.id.leaf, view(published(model.at(0))), {}, resync_two,
                                    two.config, resynced) == CryptoError::None);
        CHECK(fresh.own_leaf() == 2U);
        mls::ReceivedMessage own_removal;
        REQUIRE(two.group.process(ConstSpan<u8>(resynced.message.data(), resynced.message.size()), own_removal) ==
                CryptoError::None);
        CHECK(own_removal.commit().removes_self());
        REQUIRE(two.group.merge(own_removal.commit()) == CryptoError::None);
        CHECK_FALSE(two.group.ready());
        deliver_all(model, resynced.message, 2U);
        model.check_agreement();
        CHECK(fresh.epoch() == model.at(0).group.epoch());
        CHECK(equal(fresh.epoch_authenticator(), model.at(0).group.epoch_authenticator()));
        CHECK(equal(fresh.tree().root_hash(), model.at(0).group.tree().root_hash()));

        // Two joiners race from one GroupInfo: the delivery service takes member 7's; the other's commit is then of
        // an epoch the group left, and it joins again from a newer GroupInfo, at a leaf of a doubled tree.
        Joiner           racer;
        TestStore        racer_store;
        mls::GroupConfig racer_config{&model.verifier, &model.authority, &racer_store, timeless(), {}};
        make_joiner(suite, racer, 9U);
        const Bytes             contested = published(model.at(0));
        mls::Group              loser;
        mls::ExternalJoinResult lost;
        REQUIRE(loser.join_external(racer.id.signer, racer.id.leaf, view(contested), {}, {}, racer_config, lost) ==
                CryptoError::None);
        (void)join_externally(model, 7U, contested);
        CHECK(model.at(7).group.own_leaf() == 7U);
        refused(model.at(0).group, bytes_of(lost.message), CryptoError::OutOfWindow);
        REQUIRE(loser.join_external(racer.id.signer, racer.id.leaf, view(published(model.at(7))), {}, {},
                                    racer_config, lost) == CryptoError::None);
        CHECK(loser.own_leaf() == 8U);
        deliver_all(model, lost.message, 8U);
        model.check_agreement();
        CHECK(equal(loser.epoch_authenticator(), model.at(0).group.epoch_authenticator()));
        CHECK(model.at(0).group.tree().leaves() == 16U);
        model.commit(7U, {});
        model.exchange(private_send);

        // Member 0 lost its state and resyncs into leaf 0 (the leaf an empty member's keys name), injecting the
        // resumption PSK of the epoch it joins from, which its store kept: a joiner holds no secret of the group, the
        // members their own.
        const u64      resumed = model.at(7).group.epoch();
        TestStore::Key kept;
        kept.type   = mls::PskType::Resumption;
        kept.name   = model.group_id;
        kept.epoch  = resumed;
        kept.secret = Bytes(model.at(7).group.secret(mls::EpochSecret::Resumption).begin(),
                            model.at(7).group.secret(mls::EpochSecret::Resumption).end());
        model.at(0).store.keys.push_back(kept);
        const Bytes         resumption_nonce = random_of(suite.hash_bytes());
        mls::PreSharedKeyId resumption;
        resumption.type     = mls::PskType::Resumption;
        resumption.usage    = mls::ResumptionUsage::Application;
        resumption.group_id = view(model.group_id);
        resumption.epoch    = resumed;
        resumption.nonce    = view(resumption_nonce);
        mls::ExternalJoinOptions resync_zero;
        resync_zero.resync = 0U;
        resync_zero.psks   = ConstSpan<mls::PreSharedKeyId>(&resumption, 1U);
        (void)join_externally(model, 0U, published(model.at(7)), resync_zero);
        CHECK(model.at(0).group.own_leaf() == 0U);
        model.exchange(private_send);
    }
}

TEST_CASE("MLS: a member publishes its epoch's GroupInfo, with the external key only when asked",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        Model               model(suite);
        const mls::Proposal adds[1] = {add_of(model.at(1))};
        mls::CommitOptions  options;
        options.proposals = ConstSpan<mls::Proposal>(adds, 1U);
        model.commit(0U, options, {1U});
        const mls::Group& group = model.at(1).group;

        // By default the tree and no external key: no one joins externally from it.
        crd::containers::Array<u8> out;
        REQUIRE(group.group_info(model.at(1).who.id.signer, {}, out) == CryptoError::None);
        const Bytes          plain      = bytes_of(out);
        const mls::GroupInfo plain_info = decoded_info(plain);
        ConstSpan<u8>        data;
        CHECK(mls::find_extension(plain_info.extensions, static_cast<u16>(mls::ExtensionType::RatchetTree), data));
        CHECK(equal(data, view(fetched_tree(group))));
        CHECK_FALSE(
            mls::find_extension(plain_info.extensions, static_cast<u16>(mls::ExtensionType::ExternalPub), data));

        // Asked for, the external key is DeriveKeyPair(external_secret)'s (section 8), which only members can compute;
        // the GroupInfo states the epoch (its GroupContext, and a confirmation tag that gives the interim transcript
        // hash) and verifies under the signer's leaf key.
        const Bytes          info    = published(model.at(1));
        const mls::GroupInfo decoded = decoded_info(info);
        ConstSpan<u8>        key;
        REQUIRE(mls::find_extension(decoded.extensions, static_cast<u16>(mls::ExtensionType::ExternalPub), data));
        REQUIRE(mls::take_external_pub(data, key));
        HpkeKey external;
        REQUIRE(external.derive(suite.kem, group.secret(mls::EpochSecret::External)) == CryptoError::None);
        Bytes derived = filled(hpke_sizes(suite.kem).public_key, 0U);
        REQUIRE(external.public_key(span_of(derived)) == CryptoError::None);
        CHECK(equal(key, view(derived)));
        CHECK(decoded.signer == 1U);
        CHECK(decoded.context.epoch == group.epoch());
        CHECK(equal(decoded.context.tree_hash, group.tree().root_hash()));
        CHECK(equal(decoded.context.confirmed_transcript_hash, group.context().confirmed_transcript_hash));
        Bytes interim = filled(suite.hash_bytes(), 0U);
        REQUIRE(mls::interim_transcript_hash(suite, decoded.context.confirmed_transcript_hash,
                                             decoded.confirmation_tag, span_of(interim)) == CryptoError::None);
        CHECK(equal(view(interim), group.interim_transcript_hash()));
        mls::LeafNode signer_leaf{};
        REQUIRE(group.tree().leaf_node(1U, signer_leaf));
        const ProviderVerifier verifier;
        CHECK(mls::verify_group_info(suite, verifier, signer_leaf.signature_key, decoded) == Verification::Valid);
        // Another member's GroupInfo of the epoch carries the same key.
        const Bytes          other_bytes = published(model.at(0));
        const mls::GroupInfo other       = decoded_info(other_bytes);
        ConstSpan<u8>        other_data;
        REQUIRE(mls::find_extension(other.extensions, static_cast<u16>(mls::ExtensionType::ExternalPub), other_data));
        CHECK(equal(other_data, data));

        // The extension's codec: a key behind its header, nothing after it.
        Bytes      encoded = filled(mls::external_pub_bytes(view(derived)), 0U);
        WireWriter writer(span_of(encoded));
        mls::put_external_pub(writer, view(derived));
        CHECK(equal(view(encoded), data));
        Bytes longer = encoded;
        longer.push_back(0U);
        CHECK_FALSE(mls::take_external_pub(view(longer), key));
        CHECK(key.empty());
        CHECK_FALSE(mls::take_external_pub(ConstSpan<u8>(encoded.data(), encoded.size() - 1U), key));
        CHECK_FALSE(mls::take_external_pub({}, key));

        // Refusals: options naming the tree or the external key, or a type twice; another signer; an empty group.
        mls::GroupInfoOptions bad;
        const Bytes           tree_ext = extension(static_cast<u16>(mls::ExtensionType::RatchetTree), {});
        const Bytes           pub_ext  = extension(static_cast<u16>(mls::ExtensionType::ExternalPub), {});
        const Bytes           twice    = filled(6U, 0U);
        for (const Bytes* extensions : {&tree_ext, &pub_ext, &twice})
        {
            bad.extensions = view(*extensions);
            CHECK(group.group_info(model.at(1).who.id.signer, bad, out) == CryptoError::Malformed);
            CHECK(out.empty());
        }
        CHECK(group.group_info(model.at(0).who.id.signer, {}, out) == CryptoError::Unsupported);
        const mls::Group empty;
        CHECK(empty.group_info(model.at(1).who.id.signer, {}, out) == CryptoError::NotReady);
        CHECK(out.empty());

        // A commit's GroupInfo carries the new epoch's key when asked.
        mls::CommitOptions open;
        open.group_info.external_pub = true;
        mls::CommitResult committed;
        REQUIRE(model.at(1).group.commit(model.at(1).who.id.signer, open, committed) == CryptoError::None);
        model.deliver(1U, committed.message);
        REQUIRE(model.at(1).group.merge(committed.staged) == CryptoError::None);
        const Bytes          committed_bytes = bytes_of(committed.group_info);
        const mls::GroupInfo committed_info  = decoded_info(committed_bytes);
        REQUIRE(
            mls::find_extension(committed_info.extensions, static_cast<u16>(mls::ExtensionType::ExternalPub), data));
        REQUIRE(mls::take_external_pub(data, key));
        REQUIRE(external.derive(suite.kem, group.secret(mls::EpochSecret::External)) == CryptoError::None);
        REQUIRE(external.public_key(span_of(derived)) == CryptoError::None);
        CHECK(equal(key, view(derived)));
        CHECK(committed_info.context.epoch == group.epoch());
    }
}

TEST_CASE("MLS: a joiner refuses each fault of a GroupInfo and of its own commit, and is left empty",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        Model               model(suite);
        const mls::Proposal adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
        mls::CommitOptions  options;
        options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
        model.commit(0U, options, {1U, 2U});
        const Bytes info   = published(model.at(0));
        Member&     joiner = model.at(3);
        const auto  refuse = [&](const Bytes& from, const mls::ExternalJoinOptions& join, CryptoError expected,
                                const mls::GroupConfig* config = nullptr, ConstSpan<u8> tree = {},
                                const mls::LeafNode* leaf = nullptr)
        {
            mls::ExternalJoinResult result;
            CHECK(joiner.group.join_external(joiner.who.id.signer, leaf != nullptr ? *leaf : joiner.who.id.leaf,
                                             view(from), tree, join, config != nullptr ? *config : joiner.config,
                                             result) == expected);
            CHECK_FALSE(joiner.group.ready());
            CHECK(result.message.empty());
            CHECK(result.group_info.empty());
        };

        // Malformed: not a GroupInfo, a byte left over, options naming the tree, a tag not Nh long, an external key
        // that does not decode, a GroupInfo naming an extension twice.
        mls::CommitResult result;
        REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, {}, result) == CryptoError::None);
        refuse(bytes_of(result.message), {}, CryptoError::Malformed);
        result.staged.clear();
        Bytes longer = info;
        longer.push_back(0U);
        refuse(longer, {}, CryptoError::Malformed);
        mls::ExternalJoinOptions naming_tree;
        const Bytes              tree_ext = extension(static_cast<u16>(mls::ExtensionType::RatchetTree), {});
        naming_tree.group_info.extensions = view(tree_ext);
        refuse(info, naming_tree, CryptoError::Malformed);
        const Bytes         short_tag = random_of(suite.hash_bytes() - 1U);
        const ConstSpan<u8> short_view(view(short_tag));
        refuse(resigned(suite, info, model.at(0), nullptr, &short_view), {}, CryptoError::Malformed);
        const mls::GroupInfo decoded = decoded_info(info);
        Bytes                bad_pub = Bytes(decoded.extensions.begin(), decoded.extensions.end());
        const Bytes          pub_ext = extension(static_cast<u16>(mls::ExtensionType::ExternalPub), {});
        Bytes                tree_only;
        ConstSpan<u8>        tree_data;
        REQUIRE(mls::find_extension(decoded.extensions, static_cast<u16>(mls::ExtensionType::RatchetTree), tree_data));
        tree_only = extension(static_cast<u16>(mls::ExtensionType::RatchetTree), tree_data);
        bad_pub   = tree_only;
        for (const u8 byte : pub_ext) // external_pub's data empty: no key header
        {
            bad_pub.push_back(byte);
        }
        const ConstSpan<u8> bad_pub_view(view(bad_pub));
        refuse(resigned(suite, info, model.at(0), &bad_pub_view), {}, CryptoError::Malformed);
        Bytes repeated = tree_only;
        for (const u8 byte : tree_only)
        {
            repeated.push_back(byte);
        }
        const ConstSpan<u8> repeated_view(view(repeated));
        refuse(resigned(suite, info, model.at(0), &repeated_view), {}, CryptoError::Malformed);

        // Unsupported: a suite outside the profile, a config without a verifier, a signer that is not the leaf's, a
        // GroupInfo without the external key.
        Bytes outside = info;
        outside[6]    = 0x00U; // the GroupContext's cipher suite: 0x0004, X448, outside the profile
        outside[7]    = 0x04U;
        refuse(outside, {}, CryptoError::Unsupported);
        mls::GroupConfig no_verifier = joiner.config;
        no_verifier.verifier         = nullptr;
        refuse(info, {}, CryptoError::Unsupported, &no_verifier);
        const mls::LeafNode other_leaf = model.at(4).who.id.leaf;
        refuse(info, {}, CryptoError::Unsupported, nullptr, {}, &other_leaf);
        const ConstSpan<u8> tree_only_view(view(tree_only));
        refuse(resigned(suite, info, model.at(0), &tree_only_view), {}, CryptoError::Unsupported);
        refuse(published(model.at(0), true, false), {}, CryptoError::Unsupported);

        // The tree: none given, one whose root is not the GroupContext's, a GroupInfo signature that does not verify.
        refuse(published(model.at(0), false), {}, CryptoError::NotReady);
        Bytes other_tree;
        {
            Model other(suite);
            other_tree = fetched_tree(other.at(0).group);
        }
        refuse(published(model.at(0), false), {}, CryptoError::Forged, nullptr, view(other_tree));
        Bytes forged = info;
        forged.back() ^= 0x01U;
        refuse(forged, {}, CryptoError::Forged);
        // An Authentication Service that refuses a member's credential refuses the tree.
        model.authority.refused = Bytes(model.at(1).who.id.credential.begin(), model.at(1).who.id.credential.end());
        refuse(info, {}, CryptoError::Forged);
        model.authority.refused.clear();

        // An external key no encapsulation can use.
        Bytes       zero_pub  = tree_only;
        const Bytes zero_key  = unusable_key(suite);
        Bytes       zero_data = filled(mls::external_pub_bytes(view(zero_key)), 0U);
        WireWriter  zero_writer(span_of(zero_data));
        mls::put_external_pub(zero_writer, view(zero_key));
        for (const u8 byte : extension(static_cast<u16>(mls::ExtensionType::ExternalPub), view(zero_data)))
        {
            zero_pub.push_back(byte);
        }
        const ConstSpan<u8> zero_view(view(zero_pub));
        refuse(resigned(suite, info, model.at(0), &zero_view), {}, CryptoError::BadKey);

        // The joiner's own commit, checked as the members will check it: a resync of a blank or outside leaf, of
        // another member (the Authentication Service refuses the successor), two of one PSK, an unknown one, one
        // whose nonce is not Nh long; the joiner's own credential refused; a joiner holding a member's signature key.
        mls::ExternalJoinOptions resync;
        resync.resync = 3U; // blank
        refuse(info, resync, CryptoError::Malformed);
        resync.resync = 9U; // outside
        refuse(info, resync, CryptoError::Malformed);
        resync.resync = 2U; // member 2, another client
        refuse(info, resync, CryptoError::Forged);
        const Bytes         nonce = random_of(suite.hash_bytes());
        mls::PreSharedKeyId psks[2];
        psks[0].id    = view(model.external_id);
        psks[0].nonce = view(nonce);
        psks[1]       = psks[0];
        mls::ExternalJoinOptions with_psks;
        with_psks.psks = ConstSpan<mls::PreSharedKeyId>(psks, 2U);
        refuse(info, with_psks, CryptoError::Malformed);
        const Bytes unknown_id = Bytes{'n', 'o', 'n', 'e'};
        psks[0].id             = view(unknown_id);
        with_psks.psks         = ConstSpan<mls::PreSharedKeyId>(psks, 1U);
        refuse(info, with_psks, CryptoError::NotReady);
        psks[0].id    = view(model.external_id);
        psks[0].nonce = view(nonce).first(suite.hash_bytes() - 1U);
        refuse(info, with_psks, CryptoError::Malformed);
        model.authority.refused = Bytes(joiner.who.id.credential.begin(), joiner.who.id.credential.end());
        refuse(info, {}, CryptoError::Forged);
        model.authority.refused.clear();
        Member& two = model.at(2);
        mls::ExternalJoinResult twin;
        CHECK(joiner.group.join_external(two.who.id.signer, two.who.id.leaf, view(info), {}, {}, joiner.config, twin) ==
              CryptoError::BadKey);
        CHECK_FALSE(joiner.group.ready());

        // A joiner whose group held state loses it to the join, and the faults above changed no member.
        (void)join_externally(model, 3U, info);
        refuse(longer, {}, CryptoError::Malformed);
    }
}

TEST_CASE("MLS: members refuse each fault of an external commit, the group unchanged", "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        Model               model(suite);
        const mls::Proposal adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
        mls::CommitOptions  options;
        options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
        model.commit(0U, options, {1U, 2U});
        const Bytes info = published(model.at(0));

        // An honest external commit of member 3's, not yet delivered: its path and kem_output carry the hostile lists.
        Member&                 joiner = model.at(3);
        mls::ExternalJoinResult honest;
        REQUIRE(joiner.group.join_external(joiner.who.id.signer, joiner.who.id.leaf, view(info), {}, {}, joiner.config,
                                           honest) == CryptoError::None);
        const Bytes         honest_message = bytes_of(honest.message);
        mls::Message        decoded;
        const mls::Commit   commit = commit_in(honest_message, decoded);
        const Bytes         path   = path_of(commit);
        mls::Proposal       init;
        WireReader          list(commit.proposals);
        mls::ProposalOrRef  entry;
        REQUIRE(mls::take_proposal_or_ref(list, entry));
        REQUIRE(list.done());
        REQUIRE(entry.proposal.type == static_cast<u16>(mls::ProposalType::ExternalInit));
        const Bytes kem_output = Bytes(entry.proposal.kem_output.begin(), entry.proposal.kem_output.end());
        init                   = external_init_of(view(kem_output));
        mls::Group& receiver   = model.at(1).group;
        Framing     as_joiner;
        as_joiner.sender_given = true;
        as_joiner.sender       = {mls::SenderType::NewMemberCommit, 0U};
        as_joiner.signer       = &joiner.who.id.signer;
        const auto external_commit = [&](std::initializer_list<Bytes> entries, const Framing& framing,
                                         bool with_path = true)
        {
            return frame(suite, model.at(0), mls::ContentType::Commit,
                         view(commit_body(entries, with_path ? view(path) : ConstSpan<u8>())), framing);
        };

        // The honest commit stages; its leaf is the leftmost free one.
        {
            mls::ReceivedMessage received;
            REQUIRE(receiver.process(view(honest_message), received) == CryptoError::None);
            CHECK(received.commit().committer().type == mls::SenderType::NewMemberCommit);
            CHECK(received.commit().committer().index == 3U);
            CHECK(received.commit().has_path());
            CHECK(received.commit().added().empty());
            mls::Proposal proposal;
            mls::Sender   sender;
            REQUIRE(received.commit().proposal_count() == 1U);
            REQUIRE(received.commit().proposal(0U, proposal, sender));
            CHECK(proposal.type == static_cast<u16>(mls::ProposalType::ExternalInit));
            CHECK(sender.type == mls::SenderType::NewMemberCommit);
            CHECK(sender.index == 3U);
        }

        // Section 12.4.3.2's list: no ExternalInit, two, one with an Add, an Update, GroupContextExtensions, a ReInit,
        // two Removes, a kem_output not Nenc long, a proposal by reference; and a commit without a path.
        mls::Proposal update;
        update.type   = static_cast<u16>(mls::ProposalType::Update);
        update.update = model.at(1).who.id.leaf;
        const Bytes   single = Bytes{0x00U, 0x01U, 0x00U};
        mls::Proposal reinit;
        reinit.type                = static_cast<u16>(mls::ProposalType::ReInit);
        reinit.reinit.group_id     = view(model.group_id);
        reinit.reinit.cipher_suite = suite.id;
        const Bytes ref            = random_of(suite.hash_bytes());
        const Bytes short_output   = Bytes(kem_output.begin(), kem_output.end() - 1);
        const auto malformed = [&](std::initializer_list<Bytes> entries)
        { refused(receiver, external_commit(entries, as_joiner), CryptoError::Malformed); };
        malformed({});
        malformed({by_value(init), by_value(init)});
        malformed({by_value(init), by_value(add_of(model.at(4)))});
        malformed({by_value(init), by_value(update)});
        malformed({by_value(init), by_value(extensions_of(view(single)))});
        malformed({by_value(init), by_value(reinit)});
        malformed({by_value(init), by_value(remove_of(1U)), by_value(remove_of(2U))});
        malformed({by_value(external_init_of(view(short_output)))});
        malformed({by_value(init), by_reference(ref)});
        malformed({by_value(init), by_value(remove_of(3U))}); // a blank leaf
        refused(receiver, external_commit({by_value(init)}, as_joiner, false), CryptoError::Malformed);

        // PSKs: two of one, an unknown one.
        const Bytes         nonce = random_of(suite.hash_bytes());
        mls::PreSharedKeyId external;
        external.id    = view(model.external_id);
        external.nonce = view(nonce);
        refused(receiver, external_commit({by_value(init), by_value(psk_of(external)), by_value(psk_of(external))},
                                          as_joiner),
                CryptoError::Malformed);
        const Bytes         unknown_id = Bytes{'n', 'o', 'n', 'e'};
        mls::PreSharedKeyId unknown    = external;
        unknown.id                     = view(unknown_id);
        refused(receiver, external_commit({by_value(init), by_value(psk_of(unknown))}, as_joiner),
                CryptoError::NotReady);

        // The signature is the path leaf's key's: another signer is Forged.
        Framing other_signer = as_joiner;
        other_signer.signer  = &model.at(4).who.id.signer;
        refused(receiver, external_commit({by_value(init)}, other_signer), CryptoError::Forged);

        // The honest list and path with an all-zero kem_output, or a fresh one to the group's external key with the
        // honest confirmation tag: the init secret is then another, and the commit fails at its confirmation tag.
        const Bytes zero_output = filled(kem_output.size(), 0U);
        refused(receiver, external_commit({by_value(external_init_of(view(zero_output)))}, as_joiner),
                zero_output_refusal(suite));
        const Bytes fresh_output = fresh_kem_output(suite, info);
        Framing     honest_tag   = as_joiner;
        honest_tag.confirmation_tag = decoded.public_message.auth.confirmation_tag;
        refused(receiver, external_commit({by_value(external_init_of(view(fresh_output)))}, honest_tag),
                CryptoError::Forged);

        // A joiner's credential the Authentication Service refuses, and a resync of another client's leaf it made
        // while the Authentication Service accepted any successor.
        model.authority.refused = Bytes(joiner.who.id.credential.begin(), joiner.who.id.credential.end());
        refused(receiver, honest_message, CryptoError::Forged);
        model.authority.refused.clear();
        model.authority.any_successor = true;
        mls::Group               impostor;
        mls::ExternalJoinResult  taken;
        mls::ExternalJoinOptions resync;
        resync.resync = 2U;
        REQUIRE(impostor.join_external(joiner.who.id.signer, joiner.who.id.leaf, view(info), {}, resync, joiner.config,
                                       taken) == CryptoError::None);
        {
            mls::ReceivedMessage accepted; // the same Authentication Service at the receiver accepts it
            CHECK(receiver.process(ConstSpan<u8>(taken.message.data(), taken.message.size()), accepted) ==
                  CryptoError::None);
        }
        model.authority.any_successor = false;
        refused(receiver, bytes_of(taken.message), CryptoError::Forged);

        // Delivered, the honest commit is merged; in the next epoch it is OutOfWindow.
        model.deliver(3U, honest.message);
        model.check_agreement();
        refused(receiver, honest_message, CryptoError::OutOfWindow);
    }
}

TEST_CASE("MLS: an external joiner's leaf: reserved, its path made and merged, its keys started",
          "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = suite_of(0x0001U);
    Model                   model(suite);
    const mls::Proposal     adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
    mls::CommitOptions      options;
    options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
    model.commit(0U, options, {1U, 2U});

    // The leftmost blank leaf, and the tree doubled when it has none; a tree that holds none is NotReady.
    mls::RatchetTree tree;
    u32              index = 9U;
    CHECK(tree.reserve_leaf(index) == CryptoError::NotReady);
    CHECK(index == 0U);
    REQUIRE(tree.copy_from(model.at(0).group.tree()) == CryptoError::None);
    REQUIRE(tree.reserve_leaf(index) == CryptoError::None);
    CHECK(index == 3U);
    CHECK(tree.leaves() == 4U);
    CHECK(tree.blank(6U));
    u32 added = 0U;
    REQUIRE(tree.add(model.at(3).who.id.leaf, added) == CryptoError::None);
    CHECK(added == 3U);
    REQUIRE(tree.reserve_leaf(index) == CryptoError::None);
    CHECK(index == 4U);
    CHECK(tree.leaves() == 8U);
    CHECK(tree.blank(8U));

    // A joining path needs a blank leaf, a member's a present one; merging likewise.
    mls::RatchetTree           joining;
    crd::containers::Array<u8> path_bytes;
    mls::PathUpdate            update;
    const mls::GroupContext    context = model.at(0).group.context();
    REQUIRE(joining.copy_from(model.at(0).group.tree()) == CryptoError::None);
    CHECK(mls::create_update_path(joining, 1U, model.at(1).who.id.signer, model.at(1).who.id.leaf, context, {},
                                  path_bytes, update, true) == CryptoError::OutOfWindow);
    CHECK(mls::create_update_path(joining, 3U, model.at(3).who.id.signer, model.at(3).who.id.leaf, context, {},
                                  path_bytes, update) == CryptoError::OutOfWindow);
    REQUIRE(joining.reserve_leaf(index) == CryptoError::None);
    REQUIRE(mls::create_update_path(joining, index, model.at(3).who.id.signer, model.at(3).who.id.leaf, context, {},
                                    path_bytes, update, true) == CryptoError::None);
    CHECK_FALSE(joining.blank(2U * index));
    mls::UpdatePath path;
    WireReader      in(ConstSpan<u8>(path_bytes.data(), path_bytes.size()));
    REQUIRE(mls::take_update_path(in, path));
    mls::RatchetTree merged;
    REQUIRE(merged.copy_from(model.at(0).group.tree()) == CryptoError::None);
    REQUIRE(merged.reserve_leaf(index) == CryptoError::None);
    CHECK(merged.merge_path(index, path) == CryptoError::OutOfWindow); // a member's merge of a blank leaf
    CHECK(merged.merge_path(1U, path, true) == CryptoError::OutOfWindow); // a joining merge of a member's leaf
    REQUIRE(merged.merge_path(index, path, true) == CryptoError::None);
    CHECK(equal(merged.root_hash(), joining.root_hash()));

    // The joiner's keys start from its own path's: its leaf and every node of its path.
    mls::PathKeys   keys;
    mls::PathUpdate none;
    CHECK(keys.start(suite, index, none) == CryptoError::NotReady);
    CHECK_FALSE(keys.ready());
    CHECK(keys.start(suite, mls::kMaxSecretTreeLeaves, update) == CryptoError::OutOfWindow);
    REQUIRE(keys.start(suite, index, update) == CryptoError::None);
    CHECK(keys.leaf() == index);
    CHECK(keys.holds(2U * index));
    u32         nodes[mls::tree::kMaxDepth] = {};
    const usize count = joining.filtered_direct_path(index, Span<u32>(nodes));
    for (usize i = 0U; i < count; ++i)
    {
        CHECK(keys.holds(nodes[i]));
    }
    CHECK_FALSE(update.leaf_key.ready());
}

TEST_CASE("MLS: every replaced leaf is accepted only as its predecessor's successor (section 5.3.1)",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        Model               model(suite);
        const mls::Proposal adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
        mls::CommitOptions  options;
        options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
        model.commit(0U, options, {1U, 2U});
        mls::SendOptions send;
        send.wire_format = mls::WireFormat::PublicMessage;
        const Bytes stranger = Bytes{0U, 0U, 0U, 99U}; // another client's basic credential

        // Member 2's Update to a leaf presenting another client's credential, cached by members 0 and 1.
        Member&       two = model.at(2);
        mls::LeafNode leaf{};
        REQUIRE(two.group.tree().leaf_node(2U, leaf));
        HpkeKey key;
        REQUIRE(key.generate(suite.kem) == CryptoError::None);
        Bytes public_key = filled(hpke_sizes(suite.kem).public_key, 0U);
        REQUIRE(key.public_key(span_of(public_key)) == CryptoError::None);
        leaf.encryption_key     = view(public_key);
        leaf.credential.content = view(stranger);
        leaf.source             = mls::LeafNodeSource::Update;
        leaf.not_before         = 0U;
        leaf.not_after          = 0U;
        leaf.parent_hash        = {};
        leaf.signature          = {};
        Bytes signature         = filled(signature_sizes(suite.signature).signature, 0U);
        usize written           = 0U;
        REQUIRE(mls::sign_leaf_node(suite, two.who.id.signer, leaf, view(model.group_id), 2U, span_of(signature),
                                    written) == CryptoError::None);
        signature.resize(written);
        leaf.signature = view(signature);
        mls::Proposal update;
        update.type          = static_cast<u16>(mls::ProposalType::Update);
        update.update        = leaf;
        const Bytes proposal = frame(suite, two, mls::ContentType::Proposal, view(encoded(update)));
        for (const usize member : {0U, 1U})
        {
            mls::ReceivedMessage received;
            REQUIRE(model.at(member).group.process(view(proposal), received) == CryptoError::None);
        }

        // A committer leaves it out of the proposals it prefers; with any successor accepted it commits it, and a
        // member refuses that commit as Forged, the successor refused, unless it too accepts any successor.
        mls::CommitOptions pending;
        pending.send = send;
        mls::CommitResult left_out;
        REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, pending, left_out) == CryptoError::None);
        CHECK(left_out.staged.proposal_count() == 0U);
        left_out.staged.clear();
        model.authority.any_successor = true;
        mls::CommitResult taken;
        REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, pending, taken) == CryptoError::None);
        CHECK(taken.staged.proposal_count() == 1U);
        {
            mls::ReceivedMessage accepted;
            CHECK(model.at(1).group.process(ConstSpan<u8>(taken.message.data(), taken.message.size()), accepted) ==
                  CryptoError::None);
        }
        model.authority.any_successor = false;
        const u32 before              = model.authority.successors_refused;
        refused(model.at(1).group, Bytes(taken.message.begin(), taken.message.end()), CryptoError::Forged);
        CHECK(model.authority.successors_refused == before + 1U);
        taken.staged.clear();

        // A commit whose path leaf presents another client's credential: refused at the successor, before its
        // (random) confirmation tag is ever read.
        Member&          zero = model.at(0);
        mls::RatchetTree tree;
        REQUIRE(tree.copy_from(zero.group.tree()) == CryptoError::None);
        mls::LeafNode template_leaf{};
        REQUIRE(tree.leaf_node(0U, template_leaf));
        template_leaf.credential.content    = view(stranger);
        const mls::GroupContext now         = zero.group.context();
        const mls::GroupContext provisional = {
            suite.id, now.group_id, now.epoch + 1U, {}, now.confirmed_transcript_hash, now.extensions};
        crd::containers::Array<u8> path;
        mls::PathUpdate            path_keys;
        REQUIRE(mls::create_update_path(tree, 0U, zero.who.id.signer, template_leaf, provisional, {}, path,
                                        path_keys) == CryptoError::None);
        const Bytes body    = commit_body({}, ConstSpan<u8>(path.data(), path.size()));
        const Bytes hostile = frame(suite, zero, mls::ContentType::Commit, view(body));
        const u32 refused_before = model.authority.successors_refused;
        refused(model.at(1).group, hostile, CryptoError::Forged);
        CHECK(model.authority.successors_refused == refused_before + 1U);
        model.authority.any_successor = true;
        refused(model.at(1).group, hostile, CryptoError::Forged); // now at its confirmation tag
        CHECK(model.authority.successors_refused == refused_before + 1U);
        model.authority.any_successor = false;
    }
}

// ---- GREASE (section 13.5) ----

// How many extensions of a GroupInfo have GREASE types.
usize grease_extensions(const mls::GroupInfo& info)
{
    usize      count = 0U;
    WireReader in(info.extensions);
    while (!in.done())
    {
        u16           type = 0U;
        ConstSpan<u8> data;
        REQUIRE(in.take_u16(type));
        REQUIRE(mls::take_vector(in, data));
        count += mls::grease(type) ? 1U : 0U;
    }
    return count;
}

TEST_CASE("MLS: GREASE values go where receivers ignore unknown values and never where they cannot",
          "[crypto][net-2][mls]")
{
    // The fifteen reserved values, 0x0A0A to 0xEAEA; 0xFAFA is private use.
    usize reserved = 0U;
    for (u32 value = 0U; value <= 0xFFFFU; ++value)
    {
        if (mls::grease(static_cast<u16>(value)))
        {
            const u32 digit = value >> 12U;
            CHECK(value == (digit << 12U | 0x0A00U | digit << 4U | 0x0AU));
            CHECK(digit <= 0x0EU);
            ++reserved;
        }
    }
    CHECK(reserved == 15U);
    CHECK_FALSE(mls::grease(0xFAFAU));
    // Drawn at random: only those, and each of them (a value missed in 1,500 draws has odds below 10^-40).
    bool drawn[15] = {};
    for (u32 i = 0U; i < 1500U; ++i)
    {
        u16 value = 0U;
        REQUIRE(mls::random_grease(value) == CryptoError::None);
        REQUIRE(mls::grease(value));
        drawn[value >> 12U] = true;
    }
    CHECK(std::ranges::all_of(drawn, [](bool seen) { return seen; }));

    for (const u16 id : {u16{0x0001U}, u16{0x0022U}})
    {
        const mls::CipherSuite& suite = suite_of(id);
        INFO("cipher suite " << suite.id);
        // Every member's capabilities, leaf and key package carry GREASE (mls_test_support.hpp): the creator's leaf
        // keeps its GREASE extension in the tree, and joiners enter from a Welcome and by an external commit.
        Model         model(suite);
        mls::LeafNode leaf{};
        ConstSpan<u8> data;
        REQUIRE(model.at(0).group.tree().leaf_node(0U, leaf));
        CHECK(mls::find_extension(leaf.extensions, model.at(0).who.id.grease, data));
        CHECK(mls::lists(leaf.capabilities.cipher_suites, model.at(0).who.id.grease));
        const mls::Proposal adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
        mls::CommitOptions  options;
        options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
        model.commit(0U, options, {1U, 2U});

        // Every GroupInfo carries one GREASE extension: none when asked not to, and none beside the application's own.
        CHECK(grease_extensions(decoded_info(published(model.at(0)))) == 1U);
        mls::GroupInfoOptions      plain;
        crd::containers::Array<u8> out;
        plain.grease = false;
        REQUIRE(model.at(1).group.group_info(model.at(1).who.id.signer, plain, out) == CryptoError::None);
        CHECK(grease_extensions(decoded_info(bytes_of(out))) == 0U);
        const Bytes           mine = extension(0x3A3AU, view(Bytes{1U, 2U}));
        mls::GroupInfoOptions own;
        own.extensions = view(mine);
        REQUIRE(model.at(1).group.group_info(model.at(1).who.id.signer, own, out) == CryptoError::None);
        const Bytes          carried_bytes = bytes_of(out);
        const mls::GroupInfo carried       = decoded_info(carried_bytes);
        CHECK(grease_extensions(carried) == 1U);
        REQUIRE(mls::find_extension(carried.extensions, 0x3A3AU, data));
        CHECK(equal(data, view(Bytes{1U, 2U})));
        const mls::ExternalJoinResult joined = join_externally(model, 3U, published(model.at(2)));
        CHECK(grease_extensions(decoded_info(bytes_of(joined.group_info))) == 1U);
        mls::CommitOptions path;
        model.commit(3U, path);
        model.exchange({});

        // Never in a GroupContext's extensions, a GroupContextExtensions proposal or a credential's type, though the
        // sender's own capabilities list it: a member compares its capabilities without GREASE.
        Member&     creator  = model.at(4);
        const Bytes greased  = extension(creator.who.id.grease, {});
        const Bytes group_id = random_of(16U);
        mls::Group  refused;
        CHECK(refused.create(suite, view(group_id), view(greased), creator.who.id.leaf,
                             view(creator.who.id.hpke_private), creator.config) == CryptoError::Unsupported);
        CHECK_FALSE(refused.ready());
        mls::LeafNode typed   = creator.who.id.leaf;
        typed.credential.type = creator.who.id.grease;
        const mls::GroupContext context{suite.id, view(group_id), 0U, {}, {}, {}};
        CHECK(mls::check_leaf(context, typed, mls::LeafRole::KeyPackage, timeless(), model.authority) ==
              CryptoError::Unsupported);
        const mls::GroupContext with_grease{suite.id, view(group_id), 0U, {}, {}, view(greased)};
        CHECK(mls::check_leaf(with_grease, creator.who.id.leaf, mls::LeafRole::KeyPackage, timeless(),
                              model.authority) == CryptoError::Unsupported);
        const Bytes         listed            = extension(model.at(0).who.id.grease, {});
        const mls::Proposal context_change[1] = {extensions_of(view(listed))};
        mls::CommitOptions  changing;
        changing.proposals = ConstSpan<mls::Proposal>(context_change, 1U);
        mls::CommitResult result;
        CHECK(model.at(0).group.commit(model.at(0).who.id.signer, changing, result) == CryptoError::Unsupported);
        CHECK(result.message.empty());
    }
}
} // namespace
