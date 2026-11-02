// crd-crypto -- MLS groups that resume a prior one, RFC 9420 sections 11.2, 11.3, 12.1.4, 12.1.5, 12.2, 12.4.2 and
// 12.4.3.1 (NET.2; docs/design/network-foundation.md#mls, M3 layer 10b and decision M12). The working group's vectors
// hold no ReInit commit and no Welcome that resumes a group (the passive-client vectors passed while both were
// refused), so the creators and joiners are held to each other, to the RFC's rules and to a hand-made committer that
// makes each fault a hostile creator could: groups in every suite that a ReInit ends and that reinitialize into the
// next suite with new extensions (the ReInit proposed by a member or an external sender, the new group created by
// another member than the committer, the group id kept or changed), subgroups branched from a group that goes on, the
// new groups committing and exchanging messages; a ReInit committed alone and only when nothing else is; a group a
// ReInit ended refusing every send and every message; every refusal of a creator and of a joiner, each left empty.

#include "mls_committer.hpp"
#include "mls_model.hpp"

#include <crd/crypto/mls_group.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using namespace crd::crypto;
using namespace crd::crypto::test;
namespace mls = crd::crypto::mls;

// A model member's identity in a resumed group: its name again (so the same credential, the same member to the
// Authentication Service), a key package in the new group's suite, its store, configuration and new group.
struct Successor
{
    Joiner           who;
    TestStore        store;
    mls::GroupConfig config;
    mls::Group       group;
};

void make_successor(const mls::CipherSuite& suite, Model& model, u32 name, Successor& out)
{
    make_joiner(suite, out.who, name);
    out.config = mls::GroupConfig{&model.verifier, &model.authority, &out.store, timeless(), {}};
}

mls::Proposal reinit_of(ConstSpan<u8> group_id, u16 cipher_suite, ConstSpan<u8> extensions)
{
    mls::Proposal proposal;
    proposal.type                = static_cast<u16>(mls::ProposalType::ReInit);
    proposal.reinit.group_id     = group_id;
    proposal.reinit.cipher_suite = cipher_suite;
    proposal.reinit.extensions   = extensions;
    return proposal;
}

// A required_capabilities extension every model member meets (proposal type 1, credential type basic).
Bytes required_extensions()
{
    const Bytes required = Bytes{0x00U, 0x02U, 0x00U, 0x01U, 0x02U, 0x00U, 0x01U};
    return extension(static_cast<u16>(mls::ExtensionType::RequiredCapabilities), view(required));
}

// `from`'s commit of `options`, delivered to every other member and merged by all; returns whether it reinitializes.
bool commit_all(Model& model, usize from, const mls::CommitOptions& options)
{
    mls::CommitResult result;
    REQUIRE(model.at(from).group.commit(model.at(from).who.id.signer, options, result) == CryptoError::None);
    const bool reinitializes = result.staged.reinitializes();
    model.deliver(from, result.message);
    REQUIRE(model.at(from).group.merge(result.staged) == CryptoError::None);
    model.check_agreement();
    return reinitializes;
}

// A group of members 0 to `last` (each added by member 0).
void populate(Model& model, usize last)
{
    crd::containers::Array<mls::Proposal> adds;
    for (usize i = 1U; i <= last; ++i)
    {
        adds.push_back(add_of(model.at(i)));
    }
    mls::CommitOptions options;
    options.proposals = ConstSpan<mls::Proposal>(adds.data(), adds.size());
    mls::CommitResult result;
    REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, options, result) == CryptoError::None);
    model.deliver(0U, result.message);
    REQUIRE(model.at(0).group.merge(result.staged) == CryptoError::None);
    const Bytes welcome_bytes(result.welcome.begin(), result.welcome.end());
    for (usize i = 1U; i <= last; ++i)
    {
        const mls::Message welcome = message_of(welcome_bytes);
        Member&            joiner  = model.at(i);
        REQUIRE(joiner.group.join(welcome.body, joiner.who.package, view(joiner.who.init_private),
                                  view(joiner.who.id.hpke_private), {}, joiner.config) == CryptoError::None);
    }
    model.check_agreement();
}

// The members of a resumed group join from its Welcome, each with its prior group; every one then agrees with the
// creator, reads each other's application data, and takes a commit of the first joiner's.
void join_resumed(Model& model, Successor& creator, Successor* joiners, const usize* names, usize count,
                  const mls::ResumeResult& out)
{
    REQUIRE(creator.group.epoch() == 1U);
    CHECK(wire_format_of(out.group_info) == mls::WireFormat::GroupInfo);
    CHECK(out.welcome.empty() == (count == 0U));
    const Bytes welcome_bytes(out.welcome.begin(), out.welcome.end());
    for (usize i = 0U; i < count; ++i)
    {
        INFO("joiner " << names[i]);
        const mls::Message welcome = message_of(welcome_bytes);
        Successor&         joiner  = joiners[i];
        REQUIRE(joiner.group.join(welcome.body, joiner.who.package, view(joiner.who.init_private),
                                  view(joiner.who.id.hpke_private), {}, joiner.config,
                                  &model.at(names[i]).group) == CryptoError::None);
        CHECK(joiner.group.epoch() == 1U);
        CHECK(&joiner.group.suite() == &creator.group.suite());
        CHECK(equal(joiner.group.epoch_authenticator(), creator.group.epoch_authenticator()));
        CHECK(equal(joiner.group.tree().root_hash(), creator.group.tree().root_hash()));
        CHECK(equal(joiner.group.context().group_id, creator.group.context().group_id));
        CHECK(equal(joiner.group.context().extensions, creator.group.context().extensions));
        CHECK(equal(joiner.group.interim_transcript_hash(), creator.group.interim_transcript_hash()));
        CHECK(joiner.group.keys().holds(2U * joiner.group.own_leaf()));
        CHECK_FALSE(joiner.group.ended());
    }
    // Application data from the creator, then a commit of the first joiner's.
    const Bytes                data = random_of(77U);
    crd::containers::Array<u8> message;
    REQUIRE(creator.group.protect(creator.who.id.signer, view(data), {}, message) == CryptoError::None);
    for (usize i = 0U; i < count; ++i)
    {
        mls::ReceivedMessage received;
        REQUIRE(joiners[i].group.process(view(message), received) == CryptoError::None);
        CHECK(equal(received.application_data(), view(data)));
    }
    if (count == 0U)
    {
        return;
    }
    mls::CommitResult result;
    REQUIRE(joiners[0].group.commit(joiners[0].who.id.signer, {}, result) == CryptoError::None);
    mls::ReceivedMessage at_creator;
    REQUIRE(creator.group.process(view(result.message), at_creator) == CryptoError::None);
    REQUIRE(creator.group.merge(at_creator.commit()) == CryptoError::None);
    for (usize i = 1U; i < count; ++i)
    {
        mls::ReceivedMessage received;
        REQUIRE(joiners[i].group.process(view(result.message), received) == CryptoError::None);
        REQUIRE(joiners[i].group.merge(received.commit()) == CryptoError::None);
        CHECK(equal(joiners[i].group.epoch_authenticator(), creator.group.epoch_authenticator()));
    }
    REQUIRE(joiners[0].group.merge(result.staged) == CryptoError::None);
    CHECK(joiners[0].group.epoch() == 2U);
    CHECK(equal(joiners[0].group.epoch_authenticator(), creator.group.epoch_authenticator()));
}

// Every send of a group a ReInit ended is NotReady and every message it is given OutOfWindow, the group unchanged.
void check_ended(const mls::CipherSuite& suite, Model& model, usize member, usize other)
{
    Member&     ended = model.at(member);
    const Bytes data  = random_of(9U);
    REQUIRE(ended.group.ended());
    crd::containers::Array<u8> out;
    Bytes                      ref = filled(suite.hash_bytes(), 0U);
    CHECK(ended.group.protect(ended.who.id.signer, view(data), {}, out) == CryptoError::NotReady);
    CHECK(ended.group.propose(ended.who.id.signer, remove_of(static_cast<u32>(other)), {}, out, span_of(ref)) ==
          CryptoError::NotReady);
    CHECK(ended.group.propose_update(ended.who.id.signer, {}, out, span_of(ref)) == CryptoError::NotReady);
    CHECK(ended.group.group_info(ended.who.id.signer, {}, out) == CryptoError::NotReady);
    CHECK(out.empty());
    mls::CommitResult result;
    CHECK(ended.group.commit(ended.who.id.signer, {}, result) == CryptoError::NotReady);
    CHECK(result.message.empty());
    const Bytes proposal = encoded(remove_of(static_cast<u32>(other)));
    refused(ended.group, frame(suite, model.at(other), mls::ContentType::Proposal, view(proposal)),
            CryptoError::OutOfWindow);
    CHECK(ended.group.ended());
}
} // namespace

// ---- Reinitialization (section 11.2) ----

TEST_CASE("MLS: a group a ReInit ends reinitializes into the group the ReInit names, in every suite and the next",
          "[crypto][net-2][mls]")
{
    const ConstSpan<mls::CipherSuite> suites = mls::cipher_suites();
    for (usize k = 0U; k < suites.size(); ++k)
    {
        const mls::CipherSuite& suite  = suites[k];
        const mls::CipherSuite& target = suites[(k + 1U) % suites.size()];
        for (const bool keep_id : {false, true})
        {
            INFO("cipher suite " << suite.id << " into " << target.id << ", group id kept " << keep_id);
            Model model(suite);
            populate(model, 3U);
            mls::SendOptions send;
            send.wire_format = k % 2U == 0U ? mls::WireFormat::PrivateMessage : mls::WireFormat::PublicMessage;
            const Bytes new_id     = keep_id ? Bytes(model.group_id.begin(), model.group_id.end()) : random_of(24U);
            const Bytes extensions = required_extensions();
            (void)model.propose(2U, reinit_of(view(new_id), target.id, view(extensions)), send);

            // Member 0's commit holds the cached ReInit alone, and every member's group ends with it (section 12.4.2).
            mls::CommitOptions options;
            options.send = send;
            const u64 epoch = model.at(0).group.epoch() + 1U;
            CHECK(commit_all(model, 0U, options));
            for (usize i = 0U; i < 4U; ++i)
            {
                INFO("member " << i);
                const mls::Group& group = model.at(i).group;
                CHECK(group.ended());
                CHECK(group.epoch() == epoch);
                mls::ReInit reinit;
                REQUIRE(group.reinit_proposal(reinit));
                CHECK(equal(reinit.group_id, view(new_id)));
                CHECK(reinit.cipher_suite == target.id);
                CHECK(reinit.version == mls::kProtocolVersion);
                CHECK(equal(reinit.extensions, view(extensions)));
            }
            check_ended(suite, model, 1U, 3U);

            // Member 3, not the committer, creates the new group (section 11.2's step 3) for the other three.
            Successor   next[4];
            const usize names[3] = {0U, 1U, 2U};
            for (u32 i = 0U; i < 4U; ++i)
            {
                make_successor(target, model, i, next[i]);
            }
            if (keep_id)
            {
                // The store still names the prior group: a reinitialization that keeps its id is not a repeat.
                for (Successor& member : next)
                {
                    member.store.groups.push_back(model.group_id);
                }
            }
            const mls::KeyPackage packages[3] = {next[0].who.package, next[1].who.package, next[2].who.package};
            mls::ResumeOptions    resume;
            resume.members = ConstSpan<mls::KeyPackage>(packages, 3U);
            mls::ResumeResult out;
            REQUIRE(next[3].group.reinit(model.at(3).group, next[3].who.id.signer, next[3].who.id.leaf,
                                         view(next[3].who.id.hpke_private), resume, next[3].config,
                                         out) == CryptoError::None);
            CHECK(&next[3].group.suite() == &target);
            CHECK(equal(next[3].group.context().group_id, view(new_id)));
            CHECK(equal(next[3].group.context().extensions, view(extensions)));
            CHECK(next[3].group.tree().leaves() == 4U);
            CHECK(next[3].group.pending_proposals() == 0U);
            CHECK(model.at(3).group.ended()); // the prior group is left as it was
            join_resumed(model, next[3], next, names, 3U, out);
        }
    }
}

TEST_CASE("MLS: a ReInit is committed alone, and only when no other proposal is (sections 12.1.5 and 12.2)",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        // An external sender the group lists, which may propose a ReInit (section 12.1.8).
        Identity                  server;
        make_identity(suite, server, 50U);
        const mls::ExternalSender listed{view(server.signature_key), server.leaf.credential};
        Bytes                     entry = filled(mls::external_sender_bytes(listed), 0U);
        crd::containers::WireWriter entry_writer(span_of(entry));
        mls::put_external_sender(entry_writer, listed);
        Bytes                       data = filled(mls::vector_bytes(entry.size()), 0U);
        crd::containers::WireWriter data_writer(span_of(data));
        mls::put_vector(data_writer, view(entry));
        const Bytes senders = extension(static_cast<u16>(mls::ExtensionType::ExternalSenders), view(data));
        Model       model(suite, view(senders));
        populate(model, 3U);
        mls::SendOptions send;
        send.wire_format = mls::WireFormat::PublicMessage;
        const Bytes   new_id = random_of(8U);
        const auto    every  = [&](const Bytes& message)
        {
            for (usize i = 0U; i < 4U; ++i)
            {
                if (!model.active(i))
                {
                    continue;
                }
                mls::ReceivedMessage received;
                REQUIRE(model.at(i).group.process(view(message), received) == CryptoError::None);
            }
        };
        const auto first_proposal = [](const mls::CommitResult& result, mls::Proposal& out)
        {
            mls::Sender sender;
            REQUIRE(result.staged.proposal_count() == 1U);
            REQUIRE(result.staged.proposal(0U, out, sender));
        };

        // A ReInit and a Remove cached: the commit holds the Remove, the ReInit is dropped with the epoch.
        (void)model.propose(1U, reinit_of(view(new_id), suite.id, {}), send);
        (void)model.propose(2U, remove_of(3U), send);
        // A PSK of usage branch, which only a branched group's first commit holds, cached though the store holds the
        // key it names: never committed (section 12.1.4).
        const Bytes    held_group = random_of(7U);
        const Bytes    held_nonce = random_of(suite.hash_bytes());
        TestStore::Key held;
        held.type   = mls::PskType::Resumption;
        held.name   = Bytes(held_group.begin(), held_group.end());
        held.epoch  = 5U;
        held.secret = random_of(suite.hash_bytes());
        for (usize i = 0U; i < 4U; ++i)
        {
            model.at(i).store.keys.push_back(TestStore::Key{held.type, Bytes(held.name.begin(), held.name.end()),
                                                            held.epoch, Bytes(held.secret.begin(), held.secret.end())});
        }
        mls::PreSharedKeyId branch_key;
        branch_key.type     = mls::PskType::Resumption;
        branch_key.usage    = mls::ResumptionUsage::Branch;
        branch_key.group_id = view(held_group);
        branch_key.epoch    = 5U;
        branch_key.nonce    = view(held_nonce);
        (void)model.propose(1U, psk_of(branch_key), send);
        {
            mls::CommitResult result;
            REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, {}, result) == CryptoError::None);
            mls::Proposal chosen;
            first_proposal(result, chosen);
            CHECK(chosen.type == static_cast<u16>(mls::ProposalType::Remove));
            CHECK_FALSE(result.staged.reinitializes());
        }
        // A by-value proposal keeps the cached ReInit out too; a by-value ReInit beside a cached proposal is refused.
        {
            const mls::Proposal own[1] = {remove_of(2U)};
            mls::CommitOptions  options;
            options.pending   = true;
            options.proposals = ConstSpan<mls::Proposal>(own, 1U);
            mls::CommitResult result;
            REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, options, result) == CryptoError::None);
            CHECK(result.staged.proposal_count() == 2U); // the cached Remove of 3 and its own of 2
            CHECK_FALSE(result.staged.reinitializes());
            const mls::Proposal reinit[1] = {reinit_of(view(new_id), suite.id, {})};
            options.proposals             = ConstSpan<mls::Proposal>(reinit, 1U);
            CHECK(model.at(0).group.commit(model.at(0).who.id.signer, options, result) == CryptoError::Malformed);
            CHECK(result.message.empty());
            CHECK_FALSE(result.staged.ready());
        }
        CHECK_FALSE(commit_all(model, 0U, {}));
        CHECK(model.active(3U) == false);
        CHECK_FALSE(model.at(0).group.ended());

        // A cached ReInit and a by-value proposal alone: the commit holds the proposal, without the ReInit.
        const Bytes first_id = random_of(11U);
        (void)model.propose(1U, reinit_of(view(first_id), suite.id, {}), send);
        {
            const mls::Proposal own_remove[1] = {remove_of(2U)};
            mls::CommitOptions  options;
            options.proposals = ConstSpan<mls::Proposal>(own_remove, 1U);
            mls::CommitResult result;
            REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, options, result) == CryptoError::None);
            mls::Proposal chosen;
            first_proposal(result, chosen);
            CHECK(chosen.type == static_cast<u16>(mls::ProposalType::Remove));
            CHECK_FALSE(result.staged.reinitializes());
        }

        // Two valid ReInits and one of an older version cached: the latest valid one is committed, alone.
        const Bytes other_id = random_of(9U);
        (void)model.propose(1U, reinit_of(view(new_id), suite.id, {}), send);
        Framing framing;
        framing.sender_given = true;
        framing.sender       = {mls::SenderType::External, 0U};
        framing.signer       = &server.signer;
        const Bytes server_reinit_body = encoded(reinit_of(view(other_id), suite.id, {}));
        every(frame(suite, model.at(0), mls::ContentType::Proposal, view(server_reinit_body), framing));
        mls::Proposal old_version  = reinit_of(view(new_id), suite.id, {});
        old_version.reinit.version = 0U;
        (void)model.propose(2U, old_version, send);
        {
            // Its GroupInfo may not open the group it ends to external joiners.
            mls::CommitOptions options;
            options.group_info.external_pub = true;
            mls::CommitResult result;
            CHECK(model.at(0).group.commit(model.at(0).who.id.signer, options, result) == CryptoError::Malformed);
            CHECK(result.group_info.empty());
            REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, {}, result) == CryptoError::None);
            mls::Proposal chosen;
            first_proposal(result, chosen);
            REQUIRE(chosen.type == static_cast<u16>(mls::ProposalType::ReInit));
            CHECK(equal(chosen.reinit.group_id, view(other_id)));
            mls::Sender committed_sender;
            REQUIRE(result.staged.proposal(0U, chosen, committed_sender));
            CHECK(committed_sender.type == mls::SenderType::External);
            CHECK(result.staged.reinitializes());
        }
        // A ReInit by value, with no cached proposal taken, commits too: every member's group ends.
        const mls::Proposal own[1] = {reinit_of(view(new_id), suite.id, {})};
        mls::CommitOptions  options;
        options.pending   = false;
        options.proposals = ConstSpan<mls::Proposal>(own, 1U);
        CHECK(commit_all(model, 1U, options));
        for (usize i = 0U; i < 3U; ++i)
        {
            mls::ReInit reinit;
            REQUIRE(model.at(i).group.reinit_proposal(reinit));
            CHECK(equal(reinit.group_id, view(new_id)));
        }
        check_ended(suite, model, 2U, 0U);
    }
}

// ---- Subgroup branching (section 11.3) ----

TEST_CASE("MLS: a subgroup branches from a group in every suite, and the group goes on", "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        Model model(suite);
        populate(model, 4U);
        // Member 1 branches with members 3 and 4, under new extensions; member 2 alone branches too, from one epoch.
        Successor   sub[3];
        const usize names[2] = {3U, 4U};
        make_successor(suite, model, 1U, sub[0]);
        make_successor(suite, model, 3U, sub[1]);
        make_successor(suite, model, 4U, sub[2]);
        const mls::KeyPackage packages[2] = {sub[1].who.package, sub[2].who.package};
        mls::ResumeOptions    resume;
        resume.members                 = ConstSpan<mls::KeyPackage>(packages, 2U);
        resume.group_info.external_pub = true;
        const Bytes       sub_id     = random_of(12U);
        const Bytes       extensions = required_extensions();
        mls::ResumeResult out;
        REQUIRE(sub[0].group.branch(model.at(1).group, view(sub_id), view(extensions), sub[0].who.id.signer,
                                    sub[0].who.id.leaf, view(sub[0].who.id.hpke_private), resume, sub[0].config,
                                    out) == CryptoError::None);
        CHECK(&sub[0].group.suite() == &suite);
        CHECK(equal(sub[0].group.context().group_id, view(sub_id)));
        join_resumed(model, sub[0], sub + 1, names, 2U, out);

        Successor alone;
        make_successor(suite, model, 2U, alone);
        mls::ResumeResult solo;
        REQUIRE(alone.group.branch(model.at(2).group, view(random_of(12U)), {}, alone.who.id.signer, alone.who.id.leaf,
                                   view(alone.who.id.hpke_private), {}, alone.config, solo) == CryptoError::None);
        join_resumed(model, alone, nullptr, nullptr, 0U, solo);

        // The prior group goes on: its members commit and exchange messages as before.
        model.exchange({});
        CHECK_FALSE(commit_all(model, 2U, {}));
        model.exchange({});
    }
}

// ---- Every refusal of a creator ----

TEST_CASE("MLS: a creator refuses to reinitialize or branch what it may not, and is left empty",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        Model live(suite);
        populate(live, 2U);
        Successor next[4];
        for (u32 i = 0U; i < 4U; ++i)
        {
            make_successor(suite, live, i, next[i]);
        }
        Successor stranger;
        make_successor(suite, live, 9U, stranger);
        const auto check_refused = [](Successor& creator, const mls::ResumeResult& out)
        {
            CHECK_FALSE(creator.group.ready());
            CHECK(out.welcome.empty());
            CHECK(out.group_info.empty());
        };
        const auto reinit = [&](Successor& creator, const mls::Group& prior,
                                std::initializer_list<const Successor*> members)
        {
            crd::containers::Array<mls::KeyPackage> packages;
            for (const Successor* member : members)
            {
                packages.push_back(member->who.package);
            }
            mls::ResumeOptions options;
            options.members = ConstSpan<mls::KeyPackage>(packages.data(), packages.size());
            mls::ResumeResult out;
            const CryptoError error = creator.group.reinit(prior, creator.who.id.signer, creator.who.id.leaf,
                                                           view(creator.who.id.hpke_private), options, creator.config,
                                                           out);
            if (error != CryptoError::None)
            {
                check_refused(creator, out);
            }
            return error;
        };
        const auto branch = [&](Successor& creator, const mls::Group& prior,
                                std::initializer_list<const Successor*> members)
        {
            crd::containers::Array<mls::KeyPackage> packages;
            for (const Successor* member : members)
            {
                packages.push_back(member->who.package);
            }
            mls::ResumeOptions options;
            options.members = ConstSpan<mls::KeyPackage>(packages.data(), packages.size());
            mls::ResumeResult out;
            const CryptoError error =
                creator.group.branch(prior, view(random_of(6U)), {}, creator.who.id.signer, creator.who.id.leaf,
                                     view(creator.who.id.hpke_private), options, creator.config, out);
            if (error != CryptoError::None)
            {
                check_refused(creator, out);
            }
            return error;
        };

        // A group no ReInit ended, an empty one and the creator's own group.
        CHECK(reinit(next[0], live.at(0).group, {&next[1], &next[2]}) == CryptoError::NotReady);
        mls::Group empty;
        CHECK(branch(next[0], empty, {}) == CryptoError::NotReady);
        mls::ResumeResult scratch;
        CHECK(next[0].group.branch(next[0].group, {}, {}, next[0].who.id.signer, next[0].who.id.leaf,
                                   view(next[0].who.id.hpke_private), {}, next[0].config, scratch) ==
              CryptoError::Malformed);
        // A branch with a stranger, without the creator's own name, or of a member's key package twice.
        CHECK(branch(next[0], live.at(0).group, {&stranger}) == CryptoError::Forged);
        CHECK(branch(stranger, live.at(0).group, {&next[1]}) == CryptoError::Forged);
        CHECK(branch(next[0], live.at(0).group, {&next[1], &next[1]}) != CryptoError::None);
        CHECK(branch(next[0], live.at(0).group, {&next[1]}) == CryptoError::None);

        // A group a ReInit of this build's suite ended: a member missing, a stranger, a member twice, a key package
        // of another suite, the creator's own group.
        mls::CommitOptions  options;
        const Bytes         new_id = random_of(10U);
        const mls::Proposal own[1] = {reinit_of(view(new_id), suite.id, {})};
        options.proposals          = ConstSpan<mls::Proposal>(own, 1U);
        CHECK(commit_all(live, 0U, options));
        // A subgroup may still branch from a group a ReInit ended (a way on when the ReInit names what this build
        // cannot create).
        CHECK(branch(next[1], live.at(1).group, {&next[0]}) == CryptoError::None);
        CHECK(reinit(next[2], live.at(2).group, {&next[1]}) == CryptoError::Forged);
        CHECK(reinit(next[2], live.at(2).group, {&next[0], &next[1], &stranger}) == CryptoError::Forged);
        CHECK(reinit(next[2], live.at(2).group, {&next[0], &next[1], &next[1]}) != CryptoError::None);
        Successor other_suite;
        const mls::CipherSuite& another = mls::cipher_suites()[suite.id == 1U ? 1U : 0U];
        make_successor(another, live, 1U, other_suite);
        CHECK(reinit(next[2], live.at(2).group, {&next[0], &other_suite}) == CryptoError::Unsupported);
        CHECK(next[2].group.reinit(next[2].group, next[2].who.id.signer, next[2].who.id.leaf,
                                   view(next[2].who.id.hpke_private), {}, next[2].config, scratch) ==
              CryptoError::Malformed);
        CHECK(reinit(next[2], live.at(2).group, {&next[0], &next[1]}) == CryptoError::None);

        // A ReInit to a cipher suite outside the profile, or to a later version: valid, so the group ends, but this
        // build cannot create the group it names.
        for (const bool later_version : {false, true})
        {
            Model model(suite);
            populate(model, 1U);
            mls::Proposal unknown = reinit_of(view(new_id), later_version ? suite.id : u16{0x0004U}, {});
            if (later_version)
            {
                unknown.reinit.version = 2U;
            }
            const mls::Proposal proposals[1] = {unknown};
            mls::CommitOptions  unknown_options;
            unknown_options.proposals = ConstSpan<mls::Proposal>(proposals, 1U);
            CHECK(commit_all(model, 1U, unknown_options));
            Successor creator;
            make_successor(suite, model, 0U, creator);
            CHECK(reinit(creator, model.at(0).group, {}) == CryptoError::Unsupported);
        }
    }
}

// ---- Every refusal of a joiner ----

namespace
{
// A hand-made creator of a resumed group (mls_committer.hpp): a new group of `group_id`, `extensions` and `suite`,
// moved `epochs` epochs on, whose next epoch adds `joiners` with `psks`; returns the Welcome's body.
struct Forger
{
    Identity   id;
    TestStore  store;
    mls::Group group;
    NextEpoch  epoch;
};

void forge(const mls::CipherSuite& suite, Model& model, ConstSpan<u8> group_id, ConstSpan<u8> extensions,
           u64 epochs, Joiner* joiners, usize count, ConstSpan<mls::PreSharedKey> psks, Forger& out)
{
    make_identity(suite, out.id, 0U);
    const mls::GroupConfig config{&model.verifier, &model.authority, &out.store, timeless(), {}};
    REQUIRE(out.group.create(suite, group_id, extensions, out.id.leaf, view(out.id.hpke_private), config) ==
            CryptoError::None);
    for (u64 i = 0U; i < epochs; ++i)
    {
        mls::CommitResult result;
        REQUIRE(out.group.commit(out.id.signer, {}, result) == CryptoError::None);
        REQUIRE(out.group.merge(result.staged) == CryptoError::None);
    }
    AddOptions options;
    options.psks = psks;
    commit_adds(suite, out.id, out.group, joiners, count, options, out.epoch);
}
} // namespace

TEST_CASE("MLS: a joiner refuses a resumed group for each fault of sections 11.2, 11.3 and 12.4.3.1, left empty",
          "[crypto][net-2][mls]")
{
    for (const mls::CipherSuite& suite : mls::cipher_suites())
    {
        INFO("cipher suite " << suite.id);
        const mls::CipherSuite& another = mls::cipher_suites()[suite.id == 1U ? 1U : 0U];
        // Members 0, 1 and 2 of a group a ReInit ended; of another at the same epoch, whose ReInit names the same
        // group in a later version; and of a live one.
        Model       ended(suite);
        Model       later(suite);
        const Bytes new_id     = random_of(10U);
        const Bytes extensions = required_extensions();
        for (Model* model : {&ended, &later})
        {
            populate(*model, 2U);
            mls::Proposal own[1] = {reinit_of(view(new_id), suite.id, view(extensions))};
            own[0].reinit.version = model == &later ? u16{2U} : mls::kProtocolVersion;
            mls::CommitOptions reinit_options;
            reinit_options.proposals = ConstSpan<mls::Proposal>(own, 1U);
            CHECK(commit_all(*model, 0U, reinit_options));
        }
        REQUIRE(later.at(1).group.epoch() == ended.at(1).group.epoch());
        Model live(suite);
        populate(live, 2U);

        const auto key_of = [](const mls::Group& group, mls::ResumptionUsage usage, const Bytes& key_nonce)
        {
            mls::PreSharedKey key;
            key.id.type     = mls::PskType::Resumption;
            key.id.usage    = usage;
            key.id.group_id = group.context().group_id;
            key.id.epoch    = group.epoch();
            key.id.nonce    = view(key_nonce);
            key.secret      = group.secret(mls::EpochSecret::Resumption);
            return key;
        };
        // Member 1's successor joins the forged group from its Welcome with `resumed` as the prior group (with the
        // group joining as the prior one when `itself`), the store naming the group id when `known`.
        const auto join = [&](const mls::CipherSuite& joined_suite, ConstSpan<u8> group_id, ConstSpan<u8> group_ext,
                              u64 epochs, std::initializer_list<u32> names, ConstSpan<mls::PreSharedKey> psks,
                              const mls::Group* resumed, bool known = false, bool itself = false)
        {
            Joiner joiners[4];
            usize  count = 0U;
            for (const u32 name : names)
            {
                make_joiner(joined_suite, joiners[count++], name);
            }
            Forger forger;
            forge(joined_suite, ended, group_id, group_ext, epochs, joiners, count, psks, forger);
            TestStore store;
            if (known)
            {
                store.groups.push_back(Bytes(group_id.begin(), group_id.end()));
            }
            const mls::GroupConfig config{&ended.verifier, &ended.authority, &store, timeless(), {}};
            mls::Group             group;
            const CryptoError      error = group.join(view(forger.epoch.welcome), joiners[0].package,
                                                      view(joiners[0].init_private), view(joiners[0].id.hpke_private),
                                                      {}, config, itself ? &group : resumed);
            CHECK(group.ready() == (error == CryptoError::None));
            return error;
        };
        const Bytes             nonce  = random_of(suite.hash_bytes());
        const Bytes             nonce2 = random_of(suite.hash_bytes());
        const mls::Group&       prior  = ended.at(1).group;
        const mls::PreSharedKey reinit = key_of(prior, mls::ResumptionUsage::Reinit, nonce);
        const mls::PreSharedKey branch = key_of(live.at(1).group, mls::ResumptionUsage::Branch, nonce);
        const ConstSpan<u8>     id     = view(new_id);
        const ConstSpan<u8>     ext    = view(extensions);
        const auto              one    = [](const mls::PreSharedKey& key)
        { return ConstSpan<mls::PreSharedKey>(&key, 1U); };

        // The honest forms (the joiner is member 1; the forger is member 0; member 2 is the third).
        CHECK(join(suite, id, ext, 0U, {1U, 2U}, one(reinit), &prior) == CryptoError::None);
        CHECK(join(suite, view(random_of(4U)), {}, 0U, {1U, 2U}, one(branch), &live.at(1).group) == CryptoError::None);
        CHECK(join(suite, view(random_of(4U)), {}, 0U, {1U}, one(branch), &live.at(1).group) == CryptoError::None);
        // Section 11.2: the ReInit's group id, cipher suite and extensions, epoch 1, every member of the prior group.
        CHECK(join(suite, view(random_of(10U)), ext, 0U, {1U, 2U}, one(reinit), &prior) == CryptoError::Malformed);
        mls::PreSharedKey reinit_other = reinit; // the same key under the other suite's nonce length
        const Bytes       long_nonce   = random_of(another.hash_bytes());
        reinit_other.id.nonce          = view(long_nonce);
        CHECK(join(another, id, ext, 0U, {1U, 2U}, one(reinit_other), &prior) == CryptoError::Malformed);
        CHECK(join(suite, id, {}, 0U, {1U, 2U}, one(reinit), &prior) == CryptoError::Malformed);
        CHECK(join(suite, id, ext, 1U, {1U, 2U}, one(reinit), &prior) == CryptoError::Malformed);
        CHECK(join(suite, id, ext, 0U, {1U}, one(reinit), &prior) == CryptoError::Forged);
        CHECK(join(suite, id, ext, 0U, {1U, 2U, 9U}, one(reinit), &prior) == CryptoError::Forged);
        CHECK(join(suite, id, ext, 0U, {1U, 2U, 2U}, one(reinit), &prior) != CryptoError::None);
        // The key: held only by the group it names, at its current epoch; the prior's last commit a ReInit's.
        CHECK(join(suite, id, ext, 0U, {1U, 2U}, one(reinit), nullptr) == CryptoError::NotReady);
        CHECK(join(suite, id, ext, 0U, {1U, 2U}, one(reinit), &live.at(1).group) == CryptoError::NotReady);
        CHECK(join(suite, id, ext, 0U, {1U, 2U}, one(reinit), &later.at(1).group) == CryptoError::NotReady);
        // The prior group's ReInit names this group but a later version: a group of MLS 1.0 is not the one it names.
        const mls::PreSharedKey later_key = key_of(later.at(1).group, mls::ResumptionUsage::Reinit, nonce);
        CHECK(join(suite, id, ext, 0U, {1U, 2U}, one(later_key), &later.at(1).group) == CryptoError::Malformed);
        mls::PreSharedKey past = reinit;
        past.id.epoch          = prior.epoch() - 1U;
        CHECK(join(suite, id, ext, 0U, {1U, 2U}, one(past), &prior) == CryptoError::NotReady);
        const mls::PreSharedKey not_ended = key_of(live.at(1).group, mls::ResumptionUsage::Reinit, nonce);
        CHECK(join(suite, view(random_of(4U)), {}, 0U, {1U, 2U}, one(not_ended), &live.at(1).group) ==
              CryptoError::NotReady);
        // Section 12.4.3.1: the only such key.
        const mls::PreSharedKey second = key_of(prior, mls::ResumptionUsage::Branch, nonce2);
        const mls::PreSharedKey two[2] = {reinit, second};
        CHECK(join(suite, id, ext, 0U, {1U, 2U}, ConstSpan<mls::PreSharedKey>(two, 2U), &prior) ==
              CryptoError::Malformed);
        // Section 11.3: the prior group's cipher suite, epoch 1, members of the prior group only.
        mls::PreSharedKey branch_other = branch;
        branch_other.id.nonce          = view(long_nonce);
        CHECK(join(another, view(random_of(4U)), {}, 0U, {1U}, one(branch_other), &live.at(1).group) ==
              CryptoError::Malformed);
        CHECK(join(suite, view(random_of(4U)), {}, 1U, {1U}, one(branch), &live.at(1).group) ==
              CryptoError::Malformed);
        CHECK(join(suite, view(random_of(4U)), {}, 0U, {1U, 9U}, one(branch), &live.at(1).group) ==
              CryptoError::Forged);
        // A group id the store names is a repeat, but for a reinitialization that keeps the prior group's.
        const Bytes known = random_of(4U);
        CHECK(join(suite, view(known), {}, 0U, {1U}, one(branch), &live.at(1).group, true) == CryptoError::Replayed);
        // The prior group may not be the group joining.
        CHECK(join(suite, id, ext, 0U, {1U, 2U}, one(reinit), &prior, false, true) == CryptoError::Malformed);
    }
}
