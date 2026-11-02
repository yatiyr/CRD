#pragma once

// crd-crypto tests -- a model MLS group the commit suites drive (test_mls_commit.cpp, test_mls_external.cpp): every
// member's view of one group, driven message by message and checked to agree, the proposals a test commits, and how a
// hostile or faulty member frames a message (any sender, epoch, group, signer and membership key), so a receiver's
// refusal of it can be checked to leave its group as it was.

#include "mls_committer.hpp"
#include "mls_test_support.hpp"

#include <crd/crypto/mls_group.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

namespace crd::crypto::test
{
using containers::ConstSpan;
using containers::WireReader;
using containers::WireWriter;

inline mls::WireFormat wire_format_of(const crd::containers::Array<u8>& message)
{
    mls::Message decoded;
    REQUIRE(mls::take_message(ConstSpan<u8>(message.data(), message.size()), decoded));
    return decoded.wire_format;
}

inline constexpr usize kMembers = 8U;

// A member of a model group: its identity and key package, its store, its configuration and its group.
struct Member
{
    Joiner           who;
    TestStore        store;
    mls::GroupConfig config;
    mls::Group       group;
};

// A model group: every member's view of one group, driven message by message.
class Model
{
public:
    explicit Model(const mls::CipherSuite& suite_in, ConstSpan<u8> extensions = {}) : suite(suite_in)
    {
        external_id     = Bytes{'e', 'x', 't'};
        external_secret = random_of(32U);
        for (usize i = 0U; i < kMembers; ++i)
        {
            make_joiner(suite, members[i].who, static_cast<u32>(i));
            TestStore::Key key;
            key.name   = external_id;
            key.secret = external_secret;
            members[i].store.keys.push_back(key);
            members[i].config = mls::GroupConfig{&verifier, &authority, &members[i].store, timeless(), {}};
        }
        group_id = random_of(16U);
        REQUIRE(members[0].group.create(suite, view(group_id), extensions, members[0].who.id.leaf,
                                        view(members[0].who.id.hpke_private), members[0].config) == CryptoError::None);
    }

    [[nodiscard]] Member& at(usize i) { return members[i]; }
    [[nodiscard]] bool    active(usize i) const { return members[i].group.ready(); }

    // `from`'s message processed by every other member (a commit merged); returns each receiver's verdict.
    void deliver(usize from, const crd::containers::Array<u8>& message)
    {
        for (usize i = 0U; i < kMembers; ++i)
        {
            if (i == from || !active(i))
            {
                continue;
            }
            INFO("receiver " << i);
            mls::ReceivedMessage received;
            REQUIRE(members[i].group.process(view(message), received) == CryptoError::None);
            // An external commit's sender names no leaf on the wire; its staged commit names the joiner's.
            const bool external = received.sender().type == mls::SenderType::NewMemberCommit;
            CHECK((external ? received.commit().committer() : received.sender()).index ==
                  members[from].group.own_leaf());
            if (received.content_type() == mls::ContentType::Commit)
            {
                const bool removed = received.commit().removes_self();
                REQUIRE(members[i].group.merge(received.commit()) == CryptoError::None);
                CHECK(members[i].group.ready() == !removed);
            }
        }
    }

    // `committer`'s commit delivered, merged by it and joined by `joiners` from its Welcome.
    void commit(usize committer, const mls::CommitOptions& options, std::initializer_list<usize> joiners = {})
    {
        mls::CommitResult result;
        REQUIRE(members[committer].group.commit(members[committer].who.id.signer, options, result) ==
                CryptoError::None);
        CHECK(wire_format_of(result.message) == options.send.wire_format);
        CHECK(result.welcome.empty() == (joiners.size() == 0U));
        CHECK(wire_format_of(result.group_info) == mls::WireFormat::GroupInfo);
        deliver(committer, result.message);
        const u64 epoch = members[committer].group.epoch() + 1U;
        REQUIRE(members[committer].group.merge(result.staged) == CryptoError::None);
        CHECK(members[committer].group.epoch() == epoch);
        const Bytes welcome_bytes(result.welcome.begin(), result.welcome.end());
        for (const usize j : joiners)
        {
            INFO("joiner " << j);
            const mls::Message welcome = message_of(welcome_bytes);
            Member& joiner = members[j];
            REQUIRE(joiner.group.join(welcome.body, joiner.who.package, view(joiner.who.init_private),
                                      view(joiner.who.id.hpke_private), {}, joiner.config) == CryptoError::None);
        }
        check_agreement();
    }

    // Every active member holds the same epoch: its number, authenticator, tree and GroupContext.
    void check_agreement()
    {
        const mls::Group* first = nullptr;
        for (usize i = 0U; i < kMembers; ++i)
        {
            if (!active(i))
            {
                continue;
            }
            const mls::Group& group = members[i].group;
            INFO("member " << i);
            CHECK(group.keys().holds(2U * group.own_leaf()));
            CHECK(group.secret_tree().leaves() == group.tree().leaves());
            CHECK(group.pending_proposals() == 0U);
            if (first == nullptr)
            {
                first = &group;
                continue;
            }
            CHECK(group.epoch() == first->epoch());
            CHECK(equal(group.epoch_authenticator(), first->epoch_authenticator()));
            CHECK(equal(group.tree().root_hash(), first->tree().root_hash()));
            CHECK(equal(group.context().extensions, first->context().extensions));
            CHECK(equal(group.interim_transcript_hash(), first->interim_transcript_hash()));
        }
        REQUIRE(first != nullptr);
    }

    // Each active member's application message read by every other.
    void exchange(const mls::SendOptions& options)
    {
        for (usize from = 0U; from < kMembers; ++from)
        {
            if (!active(from))
            {
                continue;
            }
            Bytes data = random_of(1U + from * 37U);
            crd::containers::Array<u8> message;
            REQUIRE(members[from].group.protect(members[from].who.id.signer, view(data), options, message) ==
                    CryptoError::None);
            for (usize i = 0U; i < kMembers; ++i)
            {
                if (i == from || !active(i))
                {
                    continue;
                }
                mls::ReceivedMessage received;
                REQUIRE(members[i].group.process(view(message), received) == CryptoError::None);
                CHECK(received.content_type() == mls::ContentType::Application);
                CHECK(equal(received.application_data(), view(data)));
                CHECK(equal(received.authenticated_data(), options.authenticated_data));
                // Replayed: its generation is spent.
                mls::ReceivedMessage again;
                CHECK(members[i].group.process(view(message), again) == CryptoError::Replayed);
            }
        }
    }

    // `from`'s proposal sent and received by every other member; returns its reference.
    Bytes propose(usize from, const mls::Proposal& proposal, const mls::SendOptions& options)
    {
        Bytes                      ref = filled(suite.hash_bytes(), 0U);
        crd::containers::Array<u8> message;
        REQUIRE(members[from].group.propose(members[from].who.id.signer, proposal, options, message, span_of(ref)) ==
                CryptoError::None);
        deliver_proposal(from, message, ref);
        return ref;
    }

    Bytes propose_update(usize from, const mls::SendOptions& options)
    {
        Bytes                      ref = filled(suite.hash_bytes(), 0U);
        crd::containers::Array<u8> message;
        REQUIRE(members[from].group.propose_update(members[from].who.id.signer, options, message, span_of(ref)) ==
                CryptoError::None);
        deliver_proposal(from, message, ref);
        return ref;
    }

    const mls::CipherSuite& suite;
    ProviderVerifier        verifier;
    TestAuthority           authority;
    Member                  members[kMembers];
    Bytes                   group_id;
    Bytes                   external_id;
    Bytes                   external_secret;

private:
    void deliver_proposal(usize from, const crd::containers::Array<u8>& message, const Bytes& ref)
    {
        for (usize i = 0U; i < kMembers; ++i)
        {
            if (i == from || !active(i))
            {
                continue;
            }
            mls::ReceivedMessage received;
            REQUIRE(members[i].group.process(view(message), received) == CryptoError::None);
            CHECK(received.content_type() == mls::ContentType::Proposal);
            CHECK(equal(received.proposal_ref(), view(ref)));
        }
    }
};

inline mls::Proposal add_of(const Member& member)
{
    mls::Proposal proposal;
    proposal.type = static_cast<u16>(mls::ProposalType::Add);
    proposal.add  = member.who.package;
    return proposal;
}

inline mls::Proposal remove_of(u32 leaf)
{
    mls::Proposal proposal;
    proposal.type   = static_cast<u16>(mls::ProposalType::Remove);
    proposal.remove = leaf;
    return proposal;
}

inline mls::Proposal psk_of(const mls::PreSharedKeyId& id)
{
    mls::Proposal proposal;
    proposal.type = static_cast<u16>(mls::ProposalType::PreSharedKey);
    proposal.psk  = id;
    return proposal;
}

inline mls::Proposal extensions_of(ConstSpan<u8> extensions)
{
    mls::Proposal proposal;
    proposal.type       = static_cast<u16>(mls::ProposalType::GroupContextExtensions);
    proposal.extensions = extensions;
    return proposal;
}

// ---- Hostile and faulty messages ----

// How a hostile or faulty member frames a message: any sender, epoch, group, signer and membership key.
struct Framing
{
    bool            other_group      = false;
    u64             epoch_shift      = 0U;
    bool            wrong_signer     = false;
    bool            wrong_membership = false;
    bool            sender_given     = false;
    mls::Sender     sender;
    const Signer*   signer = nullptr; // another signer than the member's (an external sender, a new member)
    ConstSpan<u8>   confirmation_tag; // a commit's; a random one, never the epoch's, when empty
};

// `from`'s framing of any content as a PublicMessage: signed and tagged as its group would, so a receiver's refusal is
// the content's unless the framing says otherwise.
inline Bytes frame(const mls::CipherSuite& suite, const Member& from, mls::ContentType type, ConstSpan<u8> body,
            const Framing& framing = {})
{
    const mls::GroupContext context = from.group.context();
    const Bytes             other   = Bytes{'o', 't', 'h', 'e', 'r'};
    mls::FramedContent      content;
    content.group_id     = framing.other_group ? view(other) : context.group_id;
    content.epoch        = context.epoch + framing.epoch_shift;
    content.sender       = framing.sender_given ? framing.sender
                                                : mls::Sender{mls::SenderType::Member, from.group.own_leaf()};
    content.content_type = type;
    content.body         = body;
    SigningKey stranger;
    REQUIRE(stranger.generate(suite.signature) == CryptoError::None);
    Bytes signature = filled(signature_sizes(suite.signature).signature, 0U);
    usize written   = 0U;
    const Signer* signer = framing.signer;
    if (signer == nullptr)
    {
        signer = framing.wrong_signer ? static_cast<const Signer*>(&stranger) : &from.who.id.signer;
    }
    REQUIRE(mls::sign_content(suite, *signer,
                              mls::WireFormat::PublicMessage, content, context, span_of(signature),
                              written) == CryptoError::None);
    signature.resize(written);
    const Bytes         random_tag = random_of(suite.hash_bytes());
    const ConstSpan<u8> tag        = framing.confirmation_tag.empty() ? view(random_tag) : framing.confirmation_tag;
    const mls::ContentAuth auth{view(signature), type == mls::ContentType::Commit ? tag : ConstSpan<u8>()};
    Bytes key = Bytes(from.group.secret(mls::EpochSecret::Membership).begin(),
                      from.group.secret(mls::EpochSecret::Membership).end());
    if (framing.wrong_membership)
    {
        key[0] ^= 0x01U;
    }
    Bytes out = filled(1024U + body.size(), 0U);
    REQUIRE(mls::protect_public(suite, view(key), context, content, auth, span_of(out), written) == CryptoError::None);
    out.resize(written);
    return out;
}

inline Bytes encoded(const mls::Proposal& proposal)
{
    Bytes      out = filled(mls::proposal_bytes(proposal), 0U);
    WireWriter writer(span_of(out));
    mls::put_proposal(writer, proposal);
    return out;
}

// A commit's list entries: a proposal by value, a reference.
inline Bytes by_value(const mls::Proposal& proposal)
{
    Bytes out = Bytes{1U};
    for (const u8 byte : encoded(proposal))
    {
        out.push_back(byte);
    }
    return out;
}

inline Bytes by_reference(const Bytes& ref)
{
    Bytes      out = filled(1U + mls::vector_bytes(ref.size()), 0U);
    WireWriter writer(span_of(out));
    writer.put_u8(2U);
    mls::put_vector(writer, view(ref));
    return out;
}

// A Commit's body of `entries` and `path` (an UpdatePath's encoding; none when empty).
inline Bytes commit_body(std::initializer_list<Bytes> entries, ConstSpan<u8> path = {})
{
    Bytes list;
    for (const Bytes& entry : entries)
    {
        for (const u8 byte : entry)
        {
            list.push_back(byte);
        }
    }
    Bytes      out = filled(mls::vector_bytes(list.size()) + 1U + path.size(), 0U);
    WireWriter writer(span_of(out));
    mls::put_vector(writer, view(list));
    writer.put_u8(path.empty() ? 0U : 1U);
    writer.put_bytes(path);
    return out;
}

// The path of an honest empty commit of `from`'s (not merged): a real path to hang a hostile list on, so a list rule
// is the refusal and not the path's absence.
inline Bytes honest_path(Member& from)
{
    mls::CommitOptions empty;
    empty.pending          = false;
    empty.send.wire_format = mls::WireFormat::PublicMessage;
    mls::CommitResult result;
    REQUIRE(from.group.commit(from.who.id.signer, empty, result) == CryptoError::None);
    mls::Message message;
    REQUIRE(mls::take_message(ConstSpan<u8>(result.message.data(), result.message.size()), message));
    WireReader  in(message.public_message.content.body);
    mls::Commit commit;
    REQUIRE(mls::take_commit(in, commit));
    REQUIRE(commit.has_path);
    Bytes      out = filled(mls::update_path_bytes(commit.path), 0U);
    WireWriter writer(span_of(out));
    mls::put_update_path(writer, commit.path);
    result.staged.clear();
    return out;
}

// What a member holds of its epoch, to see that a refusal changed nothing.
struct Snapshot
{
    explicit Snapshot(const mls::Group& group)
        : epoch(group.epoch()),
          authenticator(group.epoch_authenticator().begin(), group.epoch_authenticator().end()),
          root(group.tree().root_hash().begin(), group.tree().root_hash().end()), pending(group.pending_proposals())
    {
    }
    void check(const mls::Group& group) const
    {
        CHECK(group.ready());
        CHECK(group.epoch() == epoch);
        CHECK(equal(group.epoch_authenticator(), view(authenticator)));
        CHECK(equal(group.tree().root_hash(), view(root)));
        CHECK(group.pending_proposals() == pending);
    }

    u64   epoch = 0U;
    Bytes authenticator;
    Bytes root;
    usize pending = 0U;
};

inline void refused(mls::Group& group, const Bytes& message, CryptoError expected)
{
    const Snapshot       before(group);
    mls::ReceivedMessage received;
    CHECK(group.process(view(message), received) == expected);
    CHECK_FALSE(received.commit().ready());
    CHECK(received.application_data().empty());
    before.check(group);
}
} // namespace crd::crypto::test
