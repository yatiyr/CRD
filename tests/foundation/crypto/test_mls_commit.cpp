// crd-crypto -- MLS proposals and commits, RFC 9420 sections 5.2 and 12.1 to 12.4.2 (NET.2;
// docs/design/network-foundation.md#mls, M3 layer 9 and decision M10), against the MLS working group's test vectors
// (the pinned mlswg/mls-implementations test-vectors, read at run time): every passive-client-handling-commit vector of
// the profile and the passive-client-random vector (a member joins from the Welcome, then processes each epoch's
// proposals and commit, by value and by reference, with external and resumption PSKs, Adds, Updates, Removes and
// GroupContextExtensions, and reaches each epoch's authenticator). Then groups of Cerid members in every suite that
// propose, commit (public and private, with and without a path, empty, partial and full) and merge, welcome joiners,
// exchange application messages and remove members, every member reaching the same epoch; and every refusal of
// sections 6, 12.1, 12.2 and 12.4.2 leaving the group as it was.

#include "mls_model.hpp"

#include <crd/crypto/mls_group.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireWriter;
using namespace crd::crypto;
using namespace crd::crypto::test;
namespace json = crd::json;
namespace mls  = crd::crypto::mls;

// ---- The working group's vectors ----

// One passive-client vector: join from its Welcome, then each epoch's proposals and commit, each epoch reaching its
// authenticator. Returns the epochs processed.
u32 run_passive_client(const JsonFile& vectors, u32 vector, const mls::CipherSuite& suite)
{
    const ProviderVerifier verifier;
    const TestAuthority    authority;
    TestStore              store;
    const u32              psks = json::find(vectors.doc(), vector, "external_psks");
    for (u32 i = 0U; i < json::count_of(vectors.doc(), psks); ++i)
    {
        const u32      entry = json::at(vectors.doc(), psks, i);
        TestStore::Key key;
        key.name   = vectors.hex(entry, "psk_id");
        key.secret = vectors.hex(entry, "psk");
        store.keys.push_back(key);
    }
    const Bytes tree = vectors.has(vector, "ratchet_tree") ? vectors.hex(vector, "ratchet_tree") : Bytes();
    const Bytes        package_bytes = vectors.hex(vector, "key_package");
    const Bytes        welcome_bytes = vectors.hex(vector, "welcome");
    const mls::Message package       = message_of(package_bytes);
    const mls::Message welcome       = message_of(welcome_bytes);
    // The vectors' own key packages live from 0 to the end of time, the members' already in the group from March 2024
    // to March 2025: every lifetime is checked (section 7.3's RECOMMENDED) at a time inside them all.
    mls::LeafPolicy policy = timeless();
    policy.now             = 1720000000U;
    const mls::GroupConfig config{&verifier, &authority, &store, policy, {}};
    mls::Group             group;
    REQUIRE(group.join(welcome.body, package.key_package, vectors.hex(vector, "init_priv"),
                       vectors.hex(vector, "encryption_priv"), view(tree), config) == CryptoError::None);
    REQUIRE(equal(group.epoch_authenticator(), vectors.hex(vector, "initial_epoch_authenticator")));

    const u32 epochs = json::find(vectors.doc(), vector, "epochs");
    for (u32 e = 0U; e < json::count_of(vectors.doc(), epochs); ++e)
    {
        INFO("epoch " << e);
        const u32 epoch     = json::at(vectors.doc(), epochs, e);
        const u32 proposals = json::find(vectors.doc(), epoch, "proposals");
        for (u32 p = 0U; p < json::count_of(vectors.doc(), proposals); ++p)
        {
            const Bytes          bytes = from_hex(vectors.text(json::at(vectors.doc(), proposals, p)));
            mls::ReceivedMessage received;
            REQUIRE(group.process(view(bytes), received) == CryptoError::None);
            CHECK(received.content_type() == mls::ContentType::Proposal);
            CHECK(received.proposal_ref().size() == suite.hash_bytes());
            // The same proposal again is refused: its reference is cached.
            mls::ReceivedMessage again;
            CHECK(group.process(view(bytes), again) == CryptoError::Replayed);
        }
        CHECK(group.pending_proposals() == json::count_of(vectors.doc(), proposals));
        const Bytes          commit = vectors.hex(epoch, "commit");
        mls::ReceivedMessage received;
        REQUIRE(group.process(view(commit), received) == CryptoError::None);
        REQUIRE(received.content_type() == mls::ContentType::Commit);
        mls::StagedCommit& staged = received.commit();
        REQUIRE(staged.ready());
        CHECK_FALSE(staged.removes_self());
        CHECK(staged.epoch() == group.epoch() + 1U);
        CHECK(equal(staged.epoch_authenticator(), vectors.hex(epoch, "epoch_authenticator")));
        const u64 before = group.epoch();
        REQUIRE(group.merge(staged) == CryptoError::None);
        CHECK_FALSE(staged.ready());
        CHECK(group.epoch() == before + 1U);
        CHECK(group.pending_proposals() == 0U);
        REQUIRE(equal(group.epoch_authenticator(), vectors.hex(epoch, "epoch_authenticator")));
        // The commit of the epoch that ended is refused in the new one.
        mls::ReceivedMessage late;
        CHECK(group.process(view(commit), late) == CryptoError::OutOfWindow);
    }
    return json::count_of(vectors.doc(), epochs);
}

TEST_CASE("MLS: every passive-client-handling-commit vector of the profile reaches each epoch's authenticator",
          "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("passive-client-handling-commit.json");
    u32            checked = 0U;
    u32            epochs  = 0U;
    for (u32 v = 0U; v < json::count_of(vectors.doc(), vectors.root()); ++v)
    {
        const u32               vector = json::at(vectors.doc(), vectors.root(), v);
        const mls::CipherSuite* suite  = profile_suite(vectors, vector);
        if (suite == nullptr)
        {
            continue;
        }
        INFO("vector " << v << ", cipher suite " << suite->id);
        epochs += run_passive_client(vectors, vector, *suite);
        ++checked;
    }
    CHECK(checked == 52U); // thirteen of each suite of the profile
    CHECK(epochs == 104U);
}

TEST_CASE("MLS: the passive-client-random vector reaches each of its 200 epochs' authenticators",
          "[crypto][net-2][mls]")
{
    const JsonFile vectors = mls_vectors("passive-client-random.json");
    u32            checked = 0U;
    for (u32 v = 0U; v < json::count_of(vectors.doc(), vectors.root()); ++v)
    {
        const u32               vector = json::at(vectors.doc(), vectors.root(), v);
        const mls::CipherSuite* suite  = profile_suite(vectors, vector);
        REQUIRE(suite != nullptr);
        CHECK(run_passive_client(vectors, vector, *suite) == 200U);
        ++checked;
    }
    CHECK(checked == 1U);
}
// ---- Groups of Cerid members ----

TEST_CASE("MLS: members propose, commit, merge, welcome and remove in every suite, as public and private messages",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        for (const mls::WireFormat format : {mls::WireFormat::PublicMessage, mls::WireFormat::PrivateMessage})
        {
            INFO("cipher suite " << suite.id << ", wire format " << static_cast<u16>(format));
            Model             model(suite);
            mls::SendOptions  send;
            send.wire_format        = format;
            const Bytes aad         = Bytes{'a', 'a', 'd'};
            send.authenticated_data = view(aad);
            mls::SendOptions private_send;
            private_send.authenticated_data = view(aad);

            // A full commit of four Adds by value: four joiners from one Welcome.
            const mls::Proposal adds[4] = {add_of(model.at(1)), add_of(model.at(2)), add_of(model.at(3)),
                                           add_of(model.at(4))};
            mls::CommitOptions  options;
            options.send      = send;
            options.proposals = ConstSpan<mls::Proposal>(adds, 4U);
            model.commit(0U, options, {1U, 2U, 3U, 4U});
            CHECK(model.at(0).group.tree().leaves() == 8U);
            model.exchange(private_send);
            const u64   resumption_epoch  = model.at(0).group.epoch();
            const Bytes resumption_secret = Bytes(model.at(0).group.secret(mls::EpochSecret::Resumption).begin(),
                                                  model.at(0).group.secret(mls::EpochSecret::Resumption).end());

            // Proposals by reference from three members: an Update, a Remove and an external PSK; a full commit by a
            // fourth removes member 3, adopts member 1's new leaf and injects the PSK.
            const Bytes nonce = random_of(suite.hash_bytes());
            mls::PreSharedKeyId external;
            external.id    = view(model.external_id);
            external.nonce = view(nonce);
            (void)model.propose_update(1U, send);
            (void)model.propose(2U, remove_of(model.at(3).group.own_leaf()), send);
            (void)model.propose(4U, psk_of(external), send);
            mls::CommitOptions pending;
            pending.send = send;
            model.commit(0U, pending);
            CHECK_FALSE(model.active(3U));
            model.exchange(private_send);

            // An empty commit: the committer's path alone (section 12.4).
            mls::CommitOptions empty;
            empty.send = send;
            model.commit(2U, empty);

            // A partial commit (an Add and a resumption PSK of this group's past epoch, no path): the joiner holds
            // the PSK, as section 12.4.3.1 requires.
            const Bytes resumption_nonce = random_of(suite.hash_bytes());
            mls::PreSharedKeyId resumption;
            resumption.type     = mls::PskType::Resumption;
            resumption.usage    = mls::ResumptionUsage::Application;
            resumption.group_id = view(model.group_id);
            resumption.epoch    = resumption_epoch;
            resumption.nonce    = view(resumption_nonce);
            TestStore::Key held;
            held.type   = mls::PskType::Resumption;
            held.name   = model.group_id;
            held.epoch  = resumption_epoch;
            held.secret = resumption_secret;
            model.at(5).store.keys.push_back(held);
            (void)model.propose(4U, add_of(model.at(5)), send);
            mls::CommitOptions partial;
            partial.send                    = send;
            partial.path                    = false;
            partial.group_info.ratchet_tree = false;
            const mls::Proposal resumed[1]  = {psk_of(resumption)};
            partial.proposals               = ConstSpan<mls::Proposal>(resumed, 1U);
            mls::CommitResult result;
            REQUIRE(model.at(1).group.commit(model.at(1).who.id.signer, partial, result) == CryptoError::None);
            CHECK_FALSE(result.staged.has_path());
            CHECK(result.staged.proposal_count() == 2U);
            model.deliver(1U, result.message);
            REQUIRE(model.at(1).group.merge(result.staged) == CryptoError::None);
            // The joiner fetches the tree (the GroupInfo does not carry it).
            Bytes      tree = filled(model.at(1).group.tree().encoded_bytes(), 0U);
            WireWriter tree_writer(span_of(tree));
            model.at(1).group.tree().encode(tree_writer);
            const Bytes        welcome_bytes(result.welcome.begin(), result.welcome.end());
            const mls::Message welcome = message_of(welcome_bytes);
            REQUIRE(model.at(5).group.join(welcome.body, model.at(5).who.package, view(model.at(5).who.init_private),
                                           view(model.at(5).who.id.hpke_private), view(tree),
                                           model.at(5).config) == CryptoError::None);
            model.check_agreement();
            model.exchange(private_send);

            // GroupContextExtensions (required_capabilities every member meets) by reference, an Update of the
            // joiner's leaf, and a Remove by value: member 4 leaves.
            const Bytes   required = Bytes{0x00U, 0x02U, 0x00U, 0x01U, 0x02U, 0x00U, 0x01U};
            const Bytes   group_extensions = extension(static_cast<u16>(mls::ExtensionType::RequiredCapabilities),
                                                       view(required));
            (void)model.propose(2U, extensions_of(view(group_extensions)), send);
            (void)model.propose_update(5U, send);
            mls::CommitOptions last;
            last.send                      = send;
            const mls::Proposal removal[1] = {remove_of(model.at(4).group.own_leaf())};
            last.proposals                 = ConstSpan<mls::Proposal>(removal, 1U);
            model.commit(0U, last);
            CHECK_FALSE(model.active(4U));
            CHECK(equal(model.at(0).group.context().extensions, view(group_extensions)));
            model.exchange(private_send);
            // Members 0, 1, 2 and 5 remain, and every one of them commits once more.
            for (const usize committer : {5U, 1U, 0U, 2U})
            {
                model.commit(committer, empty);
            }
        }
    }
}
// ---- Every refusal ----

TEST_CASE("MLS: a commit is refused for each fault of sections 6, 12.1, 12.2 and 12.4.2, the group unchanged",
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
        const Bytes update_ref = model.propose_update(2U, send);
        Member&     committer  = model.at(0);
        mls::Group& receiver   = model.at(1).group;
        const auto  commit_of  = [&](std::initializer_list<Bytes> entries, const Framing& framing = {})
        { return frame(suite, committer, mls::ContentType::Commit, view(commit_body(entries)), framing); };

        // Section 12.2's rules of the list.
        mls::Proposal own_update;
        own_update.type   = static_cast<u16>(mls::ProposalType::Update);
        own_update.update = committer.who.id.leaf;
        refused(receiver, commit_of({by_value(own_update)}), CryptoError::Malformed);
        refused(receiver, commit_of({by_value(remove_of(0U))}), CryptoError::Malformed);
        refused(receiver, commit_of({by_value(remove_of(3U))}), CryptoError::Malformed); // a blank leaf
        refused(receiver, commit_of({by_value(remove_of(9U))}), CryptoError::Malformed); // outside the tree
        refused(receiver, commit_of({by_value(remove_of(1U)), by_value(remove_of(1U))}), CryptoError::Malformed);
        refused(receiver, commit_of({by_reference(update_ref), by_value(remove_of(2U))}), CryptoError::Malformed);
        const Bytes   repeated = Bytes{0x00U, 0x05U, 0x00U, 0x00U, 0x05U, 0x00U};
        const Bytes   single   = Bytes{0x00U, 0x01U, 0x00U};
        refused(receiver, commit_of({by_value(extensions_of(view(single))), by_value(extensions_of(view(single)))}),
                CryptoError::Malformed);
        refused(receiver, commit_of({by_value(extensions_of(view(repeated)))}), CryptoError::Malformed);
        const Bytes         nonce = random_of(suite.hash_bytes());
        mls::PreSharedKeyId external;
        external.id    = view(model.external_id);
        external.nonce = view(nonce);
        refused(receiver, commit_of({by_value(psk_of(external)), by_value(psk_of(external))}), CryptoError::Malformed);
        mls::PreSharedKeyId short_nonce = external;
        short_nonce.nonce               = view(nonce).first(suite.hash_bytes() - 1U);
        refused(receiver, commit_of({by_value(psk_of(short_nonce))}), CryptoError::Malformed);
        mls::PreSharedKeyId reinit;
        reinit.type     = mls::PskType::Resumption;
        reinit.usage    = mls::ResumptionUsage::Reinit;
        reinit.group_id = view(model.group_id);
        reinit.nonce    = view(nonce);
        refused(receiver, commit_of({by_value(psk_of(reinit))}), CryptoError::Malformed); // section 12.1.4
        mls::PreSharedKeyId branch = reinit;
        branch.usage               = mls::ResumptionUsage::Branch;
        refused(receiver, commit_of({by_value(psk_of(branch))}), CryptoError::Malformed);
        const Bytes         unknown_id = Bytes{'n', 'o', 'n', 'e'};
        mls::PreSharedKeyId unknown    = external;
        unknown.id                     = view(unknown_id);
        refused(receiver, commit_of({by_value(psk_of(unknown))}), CryptoError::NotReady);
        mls::Proposal reinit_proposal;
        reinit_proposal.type                = static_cast<u16>(mls::ProposalType::ReInit);
        reinit_proposal.reinit.group_id     = view(model.group_id);
        reinit_proposal.reinit.cipher_suite = suite.id;
        // Section 12.2: a ReInit beside any other proposal; 12.1.5: a version older than the group's, and 13.4: an
        // extension named twice.
        refused(receiver, commit_of({by_value(reinit_proposal), by_value(remove_of(2U))}), CryptoError::Malformed);
        refused(receiver, commit_of({by_value(psk_of(external)), by_value(reinit_proposal)}), CryptoError::Malformed);
        refused(receiver, commit_of({by_value(reinit_proposal), by_value(reinit_proposal)}), CryptoError::Malformed);
        refused(receiver, commit_of({by_reference(update_ref), by_value(reinit_proposal)}), CryptoError::Malformed);
        mls::Proposal old_version  = reinit_proposal;
        old_version.reinit.version = 0U;
        refused(receiver, commit_of({by_value(old_version)}), CryptoError::Malformed);
        const Bytes   one   = extension(0x0A0AU, view(nonce));
        Bytes         twice(one.begin(), one.end());
        for (const u8 byte : one)
        {
            twice.push_back(byte);
        }
        mls::Proposal repeated_extension     = reinit_proposal;
        repeated_extension.reinit.extensions = view(twice);
        refused(receiver, commit_of({by_value(repeated_extension)}), CryptoError::Malformed);
        mls::Proposal external_init;
        external_init.type       = static_cast<u16>(mls::ProposalType::ExternalInit);
        external_init.kem_output = view(nonce);
        refused(receiver, commit_of({by_value(external_init)}), CryptoError::Malformed);
        mls::Proposal custom;
        custom.type   = 0x0A0AU;
        custom.custom = view(nonce);
        refused(receiver, commit_of({by_value(custom)}), CryptoError::Unsupported);
        refused(receiver, commit_of({by_reference(random_of(suite.hash_bytes()))}), CryptoError::NotReady);
        refused(receiver, commit_of({by_reference(random_of(3U))}), CryptoError::NotReady);

        // Section 12.1's checks of each proposal against the group.
        Joiner forged;
        make_joiner(suite, forged, 6U);
        Bytes bad_signature           = forged.signature;
        bad_signature[0]             ^= 0x01U;
        forged.package.signature      = view(bad_signature);
        mls::Proposal forged_add      = add_of(model.at(5));
        forged_add.add                = forged.package;
        refused(receiver, commit_of({by_value(forged_add)}), CryptoError::Forged);
        refused(receiver, commit_of({by_value(add_of(model.at(2)))}), CryptoError::BadKey); // a member already in it
        refused(receiver, commit_of({by_value(add_of(model.at(5))), by_value(add_of(model.at(5)))}),
                CryptoError::BadKey); // one client twice
        const Bytes required = Bytes{0x02U, 0xFFU, 0x00U, 0x00U, 0x00U};
        const Bytes demands  = extension(static_cast<u16>(mls::ExtensionType::RequiredCapabilities), view(required));
        refused(receiver, commit_of({by_value(extensions_of(view(demands)))}), CryptoError::Unsupported);

        // Section 12.4's path: required by an empty list, a Remove and GroupContextExtensions.
        refused(receiver, commit_of(std::initializer_list<Bytes>()), CryptoError::Malformed);
        refused(receiver, commit_of({by_value(remove_of(1U))}), CryptoError::Malformed);
        refused(receiver, commit_of({by_value(extensions_of(view(single)))}), CryptoError::Malformed);

        // Section 12.4.2's confirmation tag, and section 6's framing.
        refused(receiver, commit_of({by_value(add_of(model.at(5)))}), CryptoError::Forged);
        Framing framing;
        framing.wrong_membership = true;
        refused(receiver, commit_of({by_value(add_of(model.at(5)))}, framing), CryptoError::Forged);
        framing                  = {};
        framing.wrong_signer     = true;
        refused(receiver, commit_of({by_value(add_of(model.at(5)))}, framing), CryptoError::Forged);
        framing                  = {};
        framing.epoch_shift      = 1U;
        refused(receiver, commit_of({by_value(add_of(model.at(5)))}, framing), CryptoError::OutOfWindow);
        framing                  = {};
        framing.other_group      = true;
        refused(receiver, commit_of({by_value(add_of(model.at(5)))}, framing), CryptoError::Malformed);
        framing                  = {};
        framing.sender_given     = true;
        framing.sender           = {mls::SenderType::Member, 3U}; // a blank leaf
        refused(receiver, commit_of({by_value(add_of(model.at(5)))}, framing), CryptoError::OutOfWindow);
        framing.sender           = {mls::SenderType::NewMemberCommit, 0U}; // an external commit without a path
        refused(receiver, commit_of({by_value(add_of(model.at(5)))}, framing), CryptoError::Malformed);
        framing.sender           = {mls::SenderType::Member, 1U}; // the receiver itself
        refused(receiver, commit_of({by_value(add_of(model.at(5)))}, framing), CryptoError::Replayed);
        Bytes truncated = commit_of({by_value(add_of(model.at(5)))});
        truncated.pop_back();
        refused(receiver, truncated, CryptoError::Malformed);

        // Proposals a sender may not send, and messages of no group.
        refused(receiver, frame(suite, committer, mls::ContentType::Proposal, view(encoded(external_init))),
                CryptoError::Malformed);
        framing.sender = {mls::SenderType::External, 0U}; // the group lists no external sender
        refused(receiver, frame(suite, committer, mls::ContentType::Proposal, view(encoded(remove_of(1U))), framing),
                CryptoError::OutOfWindow);
        framing.sender = {mls::SenderType::NewMemberProposal, 0U};
        refused(receiver, frame(suite, committer, mls::ContentType::Proposal, view(encoded(remove_of(1U))), framing),
                CryptoError::Malformed);
        mls::Proposal external_update = own_update;
        framing.sender                = {mls::SenderType::External, 0U};
        refused(receiver, frame(suite, committer, mls::ContentType::Proposal, view(encoded(external_update)), framing),
                CryptoError::Malformed);
        refused(receiver, model.at(5).who.signature, CryptoError::Malformed);
        refused(receiver, Bytes(), CryptoError::Malformed);
        Bytes welcome_message = Bytes{0x00U, 0x01U, 0x00U, 0x03U, 0x00U, 0x01U, 0x00U, 0x00U};
        refused(receiver, welcome_message, CryptoError::Unsupported);

        // The member's own commit, echoed by the delivery service, is merged from its staged commit instead.
        mls::CommitOptions pending;
        pending.send = send;
        mls::CommitResult result;
        REQUIRE(committer.group.commit(committer.who.id.signer, pending, result) == CryptoError::None);
        CHECK(result.staged.proposal_count() == 1U); // member 2's Update
        refused(committer.group, Bytes(result.message.begin(), result.message.end()), CryptoError::Replayed);
        result.staged.clear();

        // After every refusal the group still takes a valid commit: all three members agree.
        model.commit(0U, pending);
    }
}

TEST_CASE("MLS: section 12.2's rules refuse a list that carries a real path", "[crypto][net-2][mls]")
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
        send.wire_format     = mls::WireFormat::PublicMessage;
        Member&     committer = model.at(0);
        mls::Group& receiver  = model.at(1).group;
        const Bytes path      = honest_path(committer);
        const auto  commit_of = [&](std::initializer_list<Bytes> entries)
        { return frame(suite, committer, mls::ContentType::Commit, view(commit_body(entries, view(path)))); };

        // The honest path under no list would only fail its confirmation tag (frame's is random).
        refused(receiver, commit_of(std::initializer_list<Bytes>()), CryptoError::Forged);
        // The committer's own Update by reference, and two GroupContextExtensions.
        const Bytes own_ref = model.propose_update(0U, send);
        refused(receiver, commit_of({by_reference(own_ref)}), CryptoError::Malformed);
        const Bytes single = Bytes{0x00U, 0x01U, 0x00U};
        refused(receiver, commit_of({by_value(extensions_of(view(single))), by_value(extensions_of(view(single)))}),
                CryptoError::Malformed);
        // Member 2's Update that keeps its encryption key (section 7.3), signed as an update leaf.
        Identity& member = model.at(2).who.id;
        mls::LeafNode same_key = member.leaf;
        same_key.source        = mls::LeafNodeSource::Update;
        same_key.not_after     = 0U;
        Bytes signature        = filled(signature_sizes(suite.signature).signature, 0U);
        usize written          = 0U;
        REQUIRE(mls::sign_leaf_node(suite, member.signer, same_key, view(model.group_id),
                                    model.at(2).group.own_leaf(), span_of(signature), written) == CryptoError::None);
        signature.resize(written);
        same_key.signature = view(signature);
        mls::Proposal update;
        update.type   = static_cast<u16>(mls::ProposalType::Update);
        update.update = same_key;
        const Bytes proposal = frame(suite, model.at(2), mls::ContentType::Proposal, view(encoded(update)));
        mls::ReceivedMessage received;
        REQUIRE(receiver.process(view(proposal), received) == CryptoError::None);
        const Bytes same_ref(received.proposal_ref().begin(), received.proposal_ref().end());
        refused(receiver, commit_of({by_reference(same_ref)}), CryptoError::BadKey);

        // A member's own commit leaves out its own Update and the second Remove of one leaf (section 12.2's
        // preferences), and every member takes it.
        mls::ReceivedMessage cached;
        REQUIRE(committer.group.process(view(proposal), cached) == CryptoError::None);
        (void)model.propose(1U, remove_of(2U), send);
        (void)model.propose(0U, remove_of(2U), send);
        mls::CommitOptions pending;
        pending.send = send;
        mls::CommitResult result;
        REQUIRE(committer.group.commit(committer.who.id.signer, pending, result) == CryptoError::None);
        REQUIRE(result.staged.proposal_count() == 1U);
        mls::Proposal chosen;
        mls::Sender   sender;
        REQUIRE(result.staged.proposal(0U, chosen, sender));
        CHECK(chosen.type == static_cast<u16>(mls::ProposalType::Remove));
        CHECK(sender.index == model.at(1).group.own_leaf());
        CHECK(result.staged.has_path());
        result.staged.clear();
        model.commit(0U, pending);
        CHECK_FALSE(model.active(2U));
    }
}

TEST_CASE("MLS: external senders and new members propose, and a member commits their proposals",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        // Two external senders the group lists (section 12.1.8.1), the second one proposing.
        Identity first;
        Identity second;
        make_identity(suite, first, 40U);
        make_identity(suite, second, 41U);
        const mls::ExternalSender senders[2] = {{view(first.signature_key), first.leaf.credential},
                                                {view(second.signature_key), second.leaf.credential}};
        Bytes list;
        for (const mls::ExternalSender& sender : senders)
        {
            Bytes      entry = filled(mls::external_sender_bytes(sender), 0U);
            WireWriter writer(span_of(entry));
            mls::put_external_sender(writer, sender);
            for (const u8 byte : entry)
            {
                list.push_back(byte);
            }
        }
        Bytes      data = filled(mls::vector_bytes(list.size()), 0U);
        WireWriter data_writer(span_of(data));
        mls::put_vector(data_writer, view(list));
        const Bytes extensions = extension(static_cast<u16>(mls::ExtensionType::ExternalSenders), view(data));
        Model               model(suite, view(extensions));
        const mls::Proposal adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
        mls::CommitOptions  options;
        options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
        model.commit(0U, options, {1U, 2U});

        // Every member caches each external proposal: the second sender's Remove and Add, the first sender's PSK.
        const auto deliver = [&](const Bytes& message)
        {
            for (const usize i : {0U, 1U, 2U})
            {
                mls::ReceivedMessage received;
                REQUIRE(model.at(i).group.process(view(message), received) == CryptoError::None);
                CHECK(received.content_type() == mls::ContentType::Proposal);
            }
        };
        Framing framing;
        framing.sender_given = true;
        framing.sender       = {mls::SenderType::External, 1U};
        framing.signer       = &second.signer;
        deliver(frame(suite, model.at(0), mls::ContentType::Proposal, view(encoded(remove_of(2U))), framing));
        deliver(frame(suite, model.at(0), mls::ContentType::Proposal, view(encoded(add_of(model.at(3)))), framing));
        const Bytes         nonce = random_of(suite.hash_bytes());
        mls::PreSharedKeyId external;
        external.id    = view(model.external_id);
        external.nonce = view(nonce);
        framing.sender = {mls::SenderType::External, 0U};
        framing.signer = &first.signer;
        deliver(frame(suite, model.at(0), mls::ContentType::Proposal, view(encoded(psk_of(external))), framing));
        // A sender the list does not hold at its index, and a listed index signed by the other sender's key.
        framing.sender = {mls::SenderType::External, 2U};
        refused(model.at(1).group,
                frame(suite, model.at(0), mls::ContentType::Proposal, view(encoded(remove_of(1U))), framing),
                CryptoError::OutOfWindow);
        framing.sender = {mls::SenderType::External, 1U};
        refused(model.at(1).group,
                frame(suite, model.at(0), mls::ContentType::Proposal, view(encoded(remove_of(1U))), framing),
                CryptoError::Forged);

        // A new member's own Add, signed with its key package's key (section 12.1.8).
        Framing joining;
        joining.sender_given = true;
        joining.sender       = {mls::SenderType::NewMemberProposal, 0U};
        joining.signer       = &model.at(4).who.id.signer;
        deliver(frame(suite, model.at(0), mls::ContentType::Proposal, view(encoded(add_of(model.at(4)))), joining));
        joining.signer = &model.at(5).who.id.signer; // another key than its package's
        refused(model.at(1).group,
                frame(suite, model.at(0), mls::ContentType::Proposal, view(encoded(add_of(model.at(4)))), joining),
                CryptoError::Forged);

        // Member 1 commits them all by reference: member 2 leaves, the two joiners enter, the PSK is injected.
        mls::CommitOptions pending;
        mls::CommitResult  result;
        REQUIRE(model.at(1).group.commit(model.at(1).who.id.signer, pending, result) == CryptoError::None);
        CHECK(result.staged.proposal_count() == 4U);
        result.staged.clear();
        model.commit(1U, pending, {3U, 4U});
        CHECK_FALSE(model.active(2U));
        CHECK(model.at(3).group.ready());
        CHECK(model.at(4).group.ready());
        model.exchange({});
    }
}

TEST_CASE("MLS: sending, committing and merging refuse what they cannot do", "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = mls::cipher_suites()[0];
    Model                   model(suite);
    const mls::Proposal     adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
    mls::CommitOptions      options;
    options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
    model.commit(0U, options, {1U, 2U});
    Member&                    member = model.at(1);
    crd::containers::Array<u8> out;
    Bytes                      ref = filled(suite.hash_bytes(), 0U);
    const Bytes                data = Bytes{'h', 'i'};
    mls::SendOptions           send;

    // Proposals: their types, the signer, the reference's length, the wire format and the epoch's bound.
    mls::Proposal update;
    update.type   = static_cast<u16>(mls::ProposalType::Update);
    update.update = member.who.id.leaf;
    CHECK(member.group.propose(member.who.id.signer, update, send, out, span_of(ref)) == CryptoError::Malformed);
    mls::Proposal custom;
    custom.type = 0x0A0AU;
    CHECK(member.group.propose(member.who.id.signer, custom, send, out, span_of(ref)) == CryptoError::Unsupported);
    CHECK(member.group.propose(model.at(2).who.id.signer, remove_of(2U), send, out, span_of(ref)) ==
          CryptoError::Unsupported);
    Bytes short_ref = filled(suite.hash_bytes() - 1U, 0U);
    CHECK(member.group.propose(member.who.id.signer, remove_of(2U), send, out, span_of(short_ref)) ==
          CryptoError::BadLength);
    mls::SendOptions welcome_format;
    welcome_format.wire_format = mls::WireFormat::Welcome;
    CHECK(member.group.propose(member.who.id.signer, remove_of(2U), welcome_format, out, span_of(ref)) ==
          CryptoError::Malformed);
    CHECK(out.empty());
    CHECK(member.group.pending_proposals() == 0U);
    mls::Group empty;
    CHECK(empty.propose(member.who.id.signer, remove_of(2U), send, out, span_of(ref)) == CryptoError::NotReady);
    CHECK(empty.propose_update(member.who.id.signer, send, out, span_of(ref)) == CryptoError::NotReady);

    // Application data: never public, never while proposals wait (section 12.4), never from another signer.
    mls::SendOptions public_send;
    public_send.wire_format = mls::WireFormat::PublicMessage;
    CHECK(member.group.protect(member.who.id.signer, view(data), public_send, out) == CryptoError::Malformed);
    CHECK(member.group.protect(model.at(0).who.id.signer, view(data), send, out) == CryptoError::Unsupported);
    REQUIRE(member.group.propose(member.who.id.signer, remove_of(2U), send, out, span_of(ref)) == CryptoError::None);
    CHECK(member.group.protect(member.who.id.signer, view(data), send, out) == CryptoError::NotReady);

    // Commits: an Update by value, the GroupInfo's ratchet tree named twice, another wire format, another signer.
    mls::CommitResult  result;
    mls::CommitOptions bad;
    const mls::Proposal own[1] = {update};
    bad.proposals              = ConstSpan<mls::Proposal>(own, 1U);
    CHECK(member.group.commit(member.who.id.signer, bad, result) == CryptoError::Malformed);
    bad                       = {};
    const Bytes tree_ext      = extension(static_cast<u16>(mls::ExtensionType::RatchetTree), view(data));
    bad.group_info.extensions = view(tree_ext);
    CHECK(member.group.commit(member.who.id.signer, bad, result) == CryptoError::Malformed);
    const Bytes pub_ext       = extension(static_cast<u16>(mls::ExtensionType::ExternalPub), view(data));
    bad.group_info.extensions = view(pub_ext);
    CHECK(member.group.commit(member.who.id.signer, bad, result) == CryptoError::Malformed);
    const Bytes twice         = filled(6U, 0U); // extension type 0 twice, each with empty data
    bad.group_info.extensions = view(twice);
    CHECK(member.group.commit(member.who.id.signer, bad, result) == CryptoError::Malformed);
    bad                  = {};
    bad.send.wire_format = mls::WireFormat::KeyPackage;
    CHECK(member.group.commit(member.who.id.signer, bad, result) == CryptoError::Malformed);
    CHECK(member.group.commit(model.at(2).who.id.signer, {}, result) == CryptoError::Unsupported);
    CHECK(result.message.empty());
    CHECK_FALSE(result.staged.ready());
    // A committed list section 12.2 refuses: the member's own removal by value.
    const mls::Proposal self[1] = {remove_of(1U)};
    bad                         = {};
    bad.proposals               = ConstSpan<mls::Proposal>(self, 1U);
    CHECK(member.group.commit(member.who.id.signer, bad, result) == CryptoError::Malformed);

    // Merging: an empty staged commit, one staged for another group, one the group moved past.
    mls::StagedCommit nothing;
    CHECK(member.group.merge(nothing) == CryptoError::NotReady);
    mls::CommitResult first;
    mls::CommitResult second;
    REQUIRE(member.group.commit(member.who.id.signer, {}, first) == CryptoError::None);
    REQUIRE(member.group.commit(member.who.id.signer, {}, second) == CryptoError::None);
    CHECK(model.at(2).group.merge(first.staged) == CryptoError::OutOfWindow);
    CHECK_FALSE(first.staged.ready());
    REQUIRE(member.group.merge(second.staged) == CryptoError::None);
    // A staged commit of an epoch the group has left.
    mls::CommitResult late;
    REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, {}, late) == CryptoError::None);
    mls::CommitResult later;
    REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, {}, later) == CryptoError::None);
    REQUIRE(model.at(0).group.merge(later.staged) == CryptoError::None);
    CHECK(model.at(0).group.merge(late.staged) == CryptoError::OutOfWindow);
}

TEST_CASE("MLS: an epoch caches at most its bound of proposals and keeps the resumption PSKs of its bound of epochs",
          "[crypto][net-2][mls]")
{
    const mls::CipherSuite& suite = mls::cipher_suites()[0];
    Model                   model(suite);
    const mls::Proposal     adds[2] = {add_of(model.at(1)), add_of(model.at(2))};
    mls::CommitOptions      options;
    options.proposals = ConstSpan<mls::Proposal>(adds, 2U);
    model.at(1).config.pending_proposals = 1U;
    model.at(1).config.resumption_epochs = 2U;
    model.commit(0U, options, {1U, 2U});
    mls::SendOptions send;
    send.wire_format = mls::WireFormat::PublicMessage;
    Bytes                      ref = filled(suite.hash_bytes(), 0U);
    crd::containers::Array<u8> first;
    crd::containers::Array<u8> second;
    REQUIRE(model.at(0).group.propose(model.at(0).who.id.signer, remove_of(2U), send, first, span_of(ref)) ==
            CryptoError::None);
    REQUIRE(model.at(2).group.propose_update(model.at(2).who.id.signer, send, second, span_of(ref)) ==
            CryptoError::None);
    mls::ReceivedMessage received;
    REQUIRE(model.at(1).group.process(view(first), received) == CryptoError::None);
    const Snapshot before(model.at(1).group);
    CHECK(model.at(1).group.process(view(second), received) == CryptoError::Exhausted);
    before.check(model.at(1).group);
    Bytes own_ref = filled(suite.hash_bytes(), 0U);
    CHECK(model.at(1).group.propose(model.at(1).who.id.signer, remove_of(2U), send, first, span_of(own_ref)) ==
          CryptoError::Exhausted);

    // The resumption PSK of an epoch the member was in, while its bound keeps it; then NotReady.
    mls::Group fresh;
    CHECK(fresh.create(suite, view(model.group_id), {}, model.at(0).who.id.leaf, view(model.at(0).who.id.hpke_private),
                       mls::GroupConfig{&model.verifier, &model.authority, nullptr, timeless(), {},
                                        mls::kMaxResumptionEpochs + 1U}) == CryptoError::BadLength);
    CHECK(fresh.create(suite, view(model.group_id), {}, model.at(0).who.id.leaf, view(model.at(0).who.id.hpke_private),
                       mls::GroupConfig{&model.verifier, &model.authority, nullptr, timeless(), {}, 1U,
                                        mls::kMaxPendingProposals + 1U}) == CryptoError::BadLength);
    mls::CommitOptions none;
    none.pending = false;
    const u64 joined = model.at(1).group.epoch();
    for (u32 i = 0U; i < 3U; ++i)
    {
        model.commit(2U, none);
    }
    const Bytes         nonce = random_of(suite.hash_bytes());
    mls::PreSharedKeyId resumption;
    resumption.type     = mls::PskType::Resumption;
    resumption.group_id = view(model.group_id);
    resumption.nonce    = view(nonce);
    mls::Proposal       resumed[1];
    mls::CommitOptions  with_psk;
    with_psk.pending   = false;
    with_psk.proposals = ConstSpan<mls::Proposal>(resumed, 1U);
    mls::CommitResult result;
    // Member 1 keeps two past epochs: the current one and the two before it resolve, the one it joined does not.
    for (const u64 epoch : {joined + 3U, joined + 2U, joined + 1U})
    {
        resumption.epoch = epoch;
        resumed[0]       = psk_of(resumption);
        REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, with_psk, result) == CryptoError::None);
        mls::ReceivedMessage processed;
        CHECK(model.at(1).group.process(view(result.message), processed) == CryptoError::None);
        result.staged.clear();
    }
    resumption.epoch = joined;
    resumed[0]       = psk_of(resumption);
    REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, with_psk, result) == CryptoError::None);
    mls::ReceivedMessage processed;
    CHECK(model.at(1).group.process(view(result.message), processed) == CryptoError::NotReady);
}
} // namespace
