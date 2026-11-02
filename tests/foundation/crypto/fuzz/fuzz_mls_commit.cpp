// crd-fuzz-crypto-mls-commit: an MLS member processing proposals, commits and external commits, and a new member
// joining by an external commit (NET.2; docs/design/network-foundation.md#mls, M3 layers 9 and 10a) under the bounded
// fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the second the cipher suite and the wire format (public or private), the rest
// is the mode's data, read field by field (a short input ends the case). Each case starts a fresh receiver: a member
// that joined an honest group of three from its Welcome, in the epoch where another member proposed an Update and the
// creator committed it with a path, and where the creator published the epoch's GroupInfo with its external key, from
// which a fourth member joined by an external commit.
//   0  any message: the rest, after the honest proposal, processed as an MLSMessage;
//   1  the honest commit changed: the byte to change (u16; past the end changes none) and its XOR mask;
//   2  the honest proposal changed: the byte to change and its mask;
//   3  the honest external commit changed: the byte to change and its mask;
//   4  the published GroupInfo changed, which a new member joins from: the byte to change and its mask.
//
// Oracles, none of them the code's verdict on the same call:
//   * a refused message leaves the receiver as it was (epoch, authenticator, tree, cached proposals) and stages
//     nothing; the honest message is accepted and no other one is;
//   * the honest proposal is cached under the reference its sender computed, and the honest commit and external
//     commit stage exactly the epoch their committers derived, which merging makes the receiver's;
//   * a joiner refuses every changed GroupInfo (each of its bytes is signed or framing) and is left empty; from the
//     intact one it joins, and the receiver takes its commit into the epoch the joiner holds.
#include "mls_fuzz_support.hpp"

namespace
{
using crd::u16;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using namespace crd::crypto::fuzz_mls;

// One suite's and wire format's honest epoch: the receiver's Welcome and keys, the Update proposed and the commit.
struct Honest
{
    u8                suites[2] = {};
    Member            creator;
    Member            receiver;
    Member            proposer;
    Member            joiner;
    mls::KeyPackage   receiver_package;
    mls::KeyPackage   proposer_package;
    ProviderVerifier  verifier;
    AnyCredential     authority;
    EmptyStore        store;
    mls::GroupConfig  config;
    Array<u8>         welcome;
    Array<u8>         proposal;
    Array<u8>         ref;
    Array<u8>         commit;
    Array<u8>         authenticator;
    Array<u8>         group_info; // the epoch's, with the external key
    Array<u8>         external;   // the joiner's external commit
    Array<u8>         external_authenticator;
};

Array<u8> copied(ConstSpan<u8> bytes)
{
    Array<u8> out;
    out.resize(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

ConstSpan<u8> body_of(const Array<u8>& message)
{
    return view(message).subspan(4U); // an MLSMessage's version and wire format
}

void seal_honest(const mls::CipherSuite& suite, mls::WireFormat format, Honest& honest)
{
    honest.suites[0] = static_cast<u8>(suite.id >> 8U);
    honest.suites[1] = static_cast<u8>(suite.id);
    make_member(suite, honest.creator, honest.suites);
    make_member(suite, honest.receiver, honest.suites);
    make_member(suite, honest.proposer, honest.suites);
    make_member(suite, honest.joiner, honest.suites);
    make_package(suite, honest.receiver, honest.receiver_package);
    make_package(suite, honest.proposer, honest.proposer_package);
    honest.config.verifier            = &honest.verifier;
    honest.config.authentication      = &honest.authority;
    honest.config.store               = &honest.store;
    honest.config.policy.now          = 1700000000U;
    honest.config.policy.max_lifetime = ~crd::u64{0};

    // The creator adds both; the proposer joins and proposes an Update; the creator commits it with a path.
    mls::Group     creator;
    mls::Group     proposer;
    const u8       group_id[16] = {'f', 'u', 'z', 'z', '-', 'c', 'o', 'm', 'm', 'i', 't'};
    mls::Proposal  adds[2];
    adds[0].type = static_cast<u16>(mls::ProposalType::Add);
    adds[0].add  = honest.receiver_package;
    adds[1].type = static_cast<u16>(mls::ProposalType::Add);
    adds[1].add  = honest.proposer_package;
    CRD_FUZZ_REQUIRE(creator.create(suite, group_id, {}, honest.creator.leaf, view(honest.creator.leaf_private),
                                    honest.config) == CryptoError::None);
    mls::CommitOptions added;
    added.proposals = ConstSpan<mls::Proposal>(adds, 2U);
    mls::CommitResult first;
    CRD_FUZZ_REQUIRE(creator.commit(honest.creator.signer, added, first) == CryptoError::None);
    CRD_FUZZ_REQUIRE(creator.merge(first.staged) == CryptoError::None);
    honest.welcome = first.welcome;
    CRD_FUZZ_REQUIRE(proposer.join(body_of(first.welcome), honest.proposer_package,
                                   view(honest.proposer.init_private), view(honest.proposer.leaf_private), {},
                                   honest.config) == CryptoError::None);
    // The epoch's GroupInfo with its external key, and a fourth member's external commit from it.
    mls::GroupInfoOptions published;
    published.external_pub = true;
    CRD_FUZZ_REQUIRE(creator.group_info(honest.creator.signer, published, honest.group_info) == CryptoError::None);
    mls::Group              joined;
    mls::ExternalJoinResult external;
    CRD_FUZZ_REQUIRE(joined.join_external(honest.joiner.signer, honest.joiner.leaf, view(honest.group_info), {}, {},
                                          honest.config, external) == CryptoError::None);
    honest.external               = external.message;
    honest.external_authenticator = copied(joined.epoch_authenticator());
    mls::SendOptions send;
    send.wire_format = format;
    honest.ref.resize(suite.hash_bytes());
    CRD_FUZZ_REQUIRE(proposer.propose_update(honest.proposer.signer, send, honest.proposal,
                                             Span<u8>(honest.ref.data(), honest.ref.size())) == CryptoError::None);
    mls::ReceivedMessage received;
    CRD_FUZZ_REQUIRE(creator.process(view(honest.proposal), received) == CryptoError::None);
    mls::CommitOptions pending;
    pending.send = send;
    mls::CommitResult second;
    CRD_FUZZ_REQUIRE(creator.commit(honest.creator.signer, pending, second) == CryptoError::None);
    CRD_FUZZ_REQUIRE(second.staged.proposal_count() == 1U);
    honest.commit        = second.message;
    honest.authenticator = copied(second.staged.epoch_authenticator());
}

Honest& honest_of(u8 selector)
{
    static Honest honests[8];
    static bool   sealed[8] = {};
    const usize   at        = selector % 8U;
    if (!sealed[at])
    {
        seal_honest(mls::cipher_suites()[at % 4U],
                    at < 4U ? mls::WireFormat::PublicMessage : mls::WireFormat::PrivateMessage, honests[at]);
        sealed[at] = true;
    }
    return honests[at];
}

// What a receiver holds of its epoch.
struct Snapshot
{
    crd::u64  epoch   = 0U;
    usize     pending = 0U;
    Array<u8> authenticator;
    Array<u8> root;
};

Snapshot snapshot_of(const mls::Group& group)
{
    Snapshot out;
    out.epoch   = group.epoch();
    out.pending = group.pending_proposals();
    out.authenticator.resize(group.epoch_authenticator().size());
    std::memcpy(out.authenticator.data(), group.epoch_authenticator().data(), out.authenticator.size());
    out.root.resize(group.tree().root_hash().size());
    std::memcpy(out.root.data(), group.tree().root_hash().data(), out.root.size());
    return out;
}

void require_unchanged(const mls::Group& group, const Snapshot& before, const mls::ReceivedMessage& received)
{
    CRD_FUZZ_REQUIRE(group.ready() && group.epoch() == before.epoch && group.pending_proposals() == before.pending);
    CRD_FUZZ_REQUIRE(same(group.epoch_authenticator(), view(before.authenticator)));
    CRD_FUZZ_REQUIRE(same(group.tree().root_hash(), view(before.root)));
    CRD_FUZZ_REQUIRE(!received.commit().ready() && received.application_data().empty());
}

// A Welcome-joined receiver, then `message` processed: refused with nothing changed, or it is `intact` and is taken
// (a commit into the epoch whose authenticator is `authenticator`).
void receive(Honest& honest, mls::Group& group, ConstSpan<u8> message, ConstSpan<u8> intact, bool commit,
             ConstSpan<u8> authenticator = {})
{
    const Snapshot       before = snapshot_of(group);
    mls::ReceivedMessage received;
    const CryptoError    result = group.process(message, received);
    CRD_FUZZ_REQUIRE(!same(message, intact) || result == CryptoError::None);
    if (result != CryptoError::None)
    {
        require_unchanged(group, before, received);
        return;
    }
    CRD_FUZZ_REQUIRE(same(message, intact));
    if (!commit)
    {
        CRD_FUZZ_REQUIRE(same(received.proposal_ref(), view(honest.ref)));
        CRD_FUZZ_REQUIRE(group.pending_proposals() == before.pending + 1U);
        return;
    }
    CRD_FUZZ_REQUIRE(same(received.commit().epoch_authenticator(), authenticator));
    CRD_FUZZ_REQUIRE(group.merge(received.commit()) == CryptoError::None);
    CRD_FUZZ_REQUIRE(same(group.epoch_authenticator(), authenticator));
}

// A new member joining from `info`: refused, and left empty, unless it is the intact GroupInfo; then the receiver
// takes its commit into the epoch the joiner holds.
void join_from(Honest& honest, mls::Group& receiver, ConstSpan<u8> info)
{
    mls::Group              joiner;
    mls::ExternalJoinResult out;
    const CryptoError       result =
        joiner.join_external(honest.joiner.signer, honest.joiner.leaf, info, {}, {}, honest.config, out);
    const bool intact = same(info, view(honest.group_info));
    CRD_FUZZ_REQUIRE((result == CryptoError::None) == intact);
    if (!intact)
    {
        CRD_FUZZ_REQUIRE(!joiner.ready() && out.message.empty() && out.group_info.empty());
        return;
    }
    receive(honest, receiver, view(out.message), view(out.message), true, joiner.epoch_authenticator());
}

// The byte at `at` (past the end: none) of `bytes` XORed with `mask`.
Array<u8> changed(const Array<u8>& bytes, u16 at, u8 mask)
{
    Array<u8> out = bytes;
    if (at < out.size())
    {
        out[at] = static_cast<u8>(out[at] ^ mask);
    }
    return out;
}

void run(u8 mode, Reader& in)
{
    Honest& honest = honest_of(in.byte());
    if (!in.ok())
    {
        return;
    }
    mls::Group group;
    CRD_FUZZ_REQUIRE(group.join(body_of(honest.welcome), honest.receiver_package, view(honest.receiver.init_private),
                                view(honest.receiver.leaf_private), {}, honest.config) == CryptoError::None);
    if (mode == 0U)
    {
        mls::ReceivedMessage received;
        CRD_FUZZ_REQUIRE(group.process(view(honest.proposal), received) == CryptoError::None);
        receive(honest, group, in.rest(), view(honest.commit), true, view(honest.authenticator));
        return;
    }
    const u16 at   = in.u16be();
    const u8  mask = in.byte();
    if (!in.ok())
    {
        return;
    }
    switch (mode)
    {
    case 1U:
    {
        mls::ReceivedMessage received;
        CRD_FUZZ_REQUIRE(group.process(view(honest.proposal), received) == CryptoError::None);
        receive(honest, group, view(changed(honest.commit, at, mask)), view(honest.commit), true,
                view(honest.authenticator));
        break;
    }
    case 2U:
        receive(honest, group, view(changed(honest.proposal, at, mask)), view(honest.proposal), false);
        break;
    case 3U:
        receive(honest, group, view(changed(honest.external, at, mask)), view(honest.external), true,
                view(honest.external_authenticator));
        break;
    default:
        join_from(honest, group, view(changed(honest.group_info, at, mask)));
        break;
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Reader in(data + 1U, size - 1U);
    run(static_cast<u8>(data[0] % 5U), in);
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Any message: an empty one, a public and a private header of no group, a Welcome's and a short input.
    const u8 empty[]   = {0U, 0U};
    const u8 header[]  = {0U, 1U, 0x00U, 0x01U, 0x00U, 0x01U, 0x02U, 0xAAU, 0xBBU, 0x00U, 0x00U, 0x00U,
                          0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U};
    const u8 secret[]  = {0U, 5U, 0x00U, 0x01U, 0x00U, 0x02U, 0x02U, 0xAAU, 0xBBU, 0x00U, 0x00U, 0x00U, 0x00U,
                          0x00U, 0x00U, 0x00U, 0x01U, 0x03U, 0x00U, 0x0CU, 0x01U};
    const u8 welcome[] = {0U, 2U, 0x00U, 0x01U, 0x00U, 0x03U, 0x00U, 0x01U, 0x00U, 0x00U};
    const u8 truncated[] = {0U};
    sink.add("message-empty", empty, sizeof(empty));
    sink.add("message-public-header", header, sizeof(header));
    sink.add("message-private-header", secret, sizeof(secret));
    sink.add("message-welcome", welcome, sizeof(welcome));
    sink.add("message-short", truncated, sizeof(truncated));

    // The honest commit and proposal unchanged in each suite and wire format, and changed at offsets across them.
    const char* commit_names[8]   = {"commit-unchanged-0", "commit-unchanged-1", "commit-unchanged-2",
                                     "commit-unchanged-3", "commit-unchanged-4", "commit-unchanged-5",
                                     "commit-unchanged-6", "commit-unchanged-7"};
    const char* proposal_names[8] = {"proposal-unchanged-0", "proposal-unchanged-1", "proposal-unchanged-2",
                                     "proposal-unchanged-3", "proposal-unchanged-4", "proposal-unchanged-5",
                                     "proposal-unchanged-6", "proposal-unchanged-7"};
    for (u8 selector = 0U; selector < 8U; ++selector)
    {
        const u8 commit[]   = {1U, selector, 0xFFU, 0xFFU, 0x00U};
        const u8 proposal[] = {2U, selector, 0xFFU, 0xFFU, 0x00U};
        sink.add(commit_names[selector], commit, sizeof(commit));
        sink.add(proposal_names[selector], proposal, sizeof(proposal));
    }
    const u16   offsets[]  = {0U, 3U, 8U, 20U, 40U, 90U, 160U, 300U, 500U, 800U, 1200U, 1600U};
    const char* names[12]  = {"commit-at-0",   "commit-at-3",   "commit-at-8",    "commit-at-20",
                              "commit-at-40",  "commit-at-90",  "commit-at-160",  "commit-at-300",
                              "commit-at-500", "commit-at-800", "commit-at-1200", "commit-at-1600"};
    const char* pnames[12] = {"proposal-at-0",   "proposal-at-3",   "proposal-at-8",    "proposal-at-20",
                              "proposal-at-40",  "proposal-at-90",  "proposal-at-160",  "proposal-at-300",
                              "proposal-at-500", "proposal-at-800", "proposal-at-1200", "proposal-at-1600"};
    for (usize i = 0U; i < 12U; ++i)
    {
        const u8 commit[]   = {1U, static_cast<u8>(i % 8U), static_cast<u8>(offsets[i] >> 8U),
                               static_cast<u8>(offsets[i]), 0x01U};
        const u8 proposal[] = {2U, static_cast<u8>((i + 3U) % 8U), static_cast<u8>(offsets[i] >> 8U),
                               static_cast<u8>(offsets[i]), 0x80U};
        sink.add(names[i], commit, sizeof(commit));
        sink.add(pnames[i], proposal, sizeof(proposal));
    }

    // The honest external commit and GroupInfo unchanged in each suite, and changed at offsets across them.
    const char* external_names[4] = {"external-unchanged-0", "external-unchanged-1", "external-unchanged-2",
                                     "external-unchanged-3"};
    const char* info_names[4]     = {"info-unchanged-0", "info-unchanged-1", "info-unchanged-2", "info-unchanged-3"};
    for (u8 selector = 0U; selector < 4U; ++selector)
    {
        const u8 external[] = {3U, selector, 0xFFU, 0xFFU, 0x00U};
        const u8 info[]     = {4U, selector, 0xFFU, 0xFFU, 0x00U};
        sink.add(external_names[selector], external, sizeof(external));
        sink.add(info_names[selector], info, sizeof(info));
    }
    const u16   at[]       = {2U, 12U, 30U, 70U, 150U, 400U, 700U, 1000U};
    const char* xnames[8]  = {"external-at-2",   "external-at-12",  "external-at-30",  "external-at-70",
                              "external-at-150", "external-at-400", "external-at-700", "external-at-1000"};
    const char* inames[8]  = {"info-at-2",   "info-at-12",  "info-at-30",  "info-at-70",
                              "info-at-150", "info-at-400", "info-at-700", "info-at-1000"};
    for (usize i = 0U; i < 8U; ++i)
    {
        const u8 external[] = {3U, static_cast<u8>(i % 4U), static_cast<u8>(at[i] >> 8U), static_cast<u8>(at[i]),
                               0x04U};
        const u8 info[]     = {4U, static_cast<u8>((i + 1U) % 4U), static_cast<u8>(at[i] >> 8U),
                               static_cast<u8>(at[i]), 0x20U};
        sink.add(xnames[i], external, sizeof(external));
        sink.add(inames[i], info, sizeof(info));
    }
}
