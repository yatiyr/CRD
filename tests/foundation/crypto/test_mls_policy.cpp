// crd-crypto -- the crypto policy generation over MLS cipher suites (NET.2 decision M14;
// docs/design/network-foundation.md#mls): a group is created, joined from a Welcome and joined externally only under a
// suite its generation allows in its scope, and the scope's configuration refusals; an epoch begins only under an
// allowed suite, whether the member commits or receives the commit, while the epoch's keys keep carrying application
// data; a ReInit to a refused suite is refused and one to an allowed suite is how the group leaves its refused one;
// and a dated refusal reaches a group when its clock passes the date, a clock that never goes back.

#include "mls_model.hpp"
#include "policy_test_signer.hpp"

#include <crd/crypto/mls_group.hpp>
#include <crd/crypto/policy.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u16;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using namespace crd::crypto;
using namespace crd::crypto::test;
namespace mls = crd::crypto::mls;

constexpr u64 kNow = 1700000000U; // the model's clock (timeless())

// Generation `number` at the model's clock, accepting the whole register; with `refuse_x25519` classical-only key
// exchange (X25519) is refused in `scope` from `from`.
void apply_generation(TestInstallation& installation, u64 number, bool refuse_x25519,
                      PolicyScope scope = PolicyScope::Everywhere, u64 from = 1U)
{
    PolicyContent content = accepting_all(number, kNow - 60U);
    if (refuse_x25519)
    {
        refuse_rule(content, AlgorithmId::X25519, scope, from);
    }
    REQUIRE(installation.apply(content, kNow) == PolicyError::None);
}

mls::Proposal reinit_to(ConstSpan<u8> group_id, u16 cipher_suite)
{
    mls::Proposal proposal;
    proposal.type                = static_cast<u16>(mls::ProposalType::ReInit);
    proposal.reinit.group_id     = group_id;
    proposal.reinit.cipher_suite = cipher_suite;
    return proposal;
}

const mls::CipherSuite& classical()
{
    return suite_of(iana::kMls128Dhkemx25519Aes128gcmSha256Ed25519);
}

const mls::CipherSuite& hybrid()
{
    return suite_of(iana::unverified::kMls128Mlkem768x25519Aes128gcmSha256Ed25519);
}
} // namespace

TEST_CASE("MLS policy: a group is created and joined only under a suite its generation allows in its scope",
          "[crypto][net-2][mls]")
{
    TestInstallation everywhere;
    TestInstallation services;
    apply_generation(everywhere, 1U, true);
    apply_generation(services, 1U, true, PolicyScope::CeridServices);
    Model       model(classical());
    const Bytes group_id = random_of(16U);

    struct Case
    {
        const char*        name;
        const PolicyStore* store;
        PolicyScope        scope;
        CryptoError        expected;
    };
    const Case cases[] = {
        {"refused everywhere", &everywhere.store, PolicyScope::Everywhere, CryptoError::PolicyRefused},
        {"refused for Cerid services, a service group", &services.store, PolicyScope::CeridServices,
         CryptoError::PolicyRefused},
        {"refused for Cerid services, another group", &services.store, PolicyScope::Everywhere, CryptoError::None},
        {"no store", nullptr, PolicyScope::Everywhere, CryptoError::None},
        {"a scope without a store", nullptr, PolicyScope::CeridServices, CryptoError::Unsupported},
        {"a scope this build does not know", &everywhere.store, static_cast<PolicyScope>(7U),
         CryptoError::Unsupported},
    };
    for (const Case& test : cases)
    {
        CAPTURE(test.name);
        Member&          member = model.at(1);
        mls::GroupConfig config = member.config;
        config.crypto_policy    = test.store;
        config.policy_scope     = test.scope;
        if (test.expected != CryptoError::Unsupported)
        {
            CHECK(check_suite_policy(config, classical()) == test.expected);
            CHECK(check_suite_policy(config, hybrid()) == CryptoError::None);
        }
        mls::Group group;
        CHECK(group.create(classical(), view(group_id), {}, member.who.id.leaf, view(member.who.id.hpke_private),
                           config) == test.expected);
        CHECK(group.ready() == (test.expected == CryptoError::None));
    }

    // The hybrid suite's KEM is not classical-only: its group is created under the same generation.
    const Model post_quantum(hybrid(), {}, &everywhere.store);
    CHECK(post_quantum.active(0U));

    // A Welcome into the classical group: refused where X25519 is, joined where it is not; a refused joiner is empty.
    const mls::Proposal adds[] = {add_of(model.at(2))};
    mls::CommitOptions  options;
    options.proposals = ConstSpan<mls::Proposal>(adds);
    mls::CommitResult result;
    REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, options, result) == CryptoError::None);
    const Bytes welcome_bytes(result.welcome.begin(), result.welcome.end());
    for (const bool refused : {true, false})
    {
        CAPTURE(refused);
        Member&          joiner = model.at(2);
        mls::GroupConfig config = joiner.config;
        config.crypto_policy    = refused ? &everywhere.store : &services.store;
        const mls::Message welcome = message_of(welcome_bytes);
        mls::Group         group;
        CHECK(group.join(welcome.body, joiner.who.package, view(joiner.who.init_private),
                         view(joiner.who.id.hpke_private), {}, config) ==
              (refused ? CryptoError::PolicyRefused : CryptoError::None));
        CHECK(group.ready() == !refused);
    }

    // An external join from the group's GroupInfo: refused the same way, the joiner empty and nothing to send.
    mls::GroupInfoOptions info_options;
    info_options.external_pub = true;
    crd::containers::Array<u8> info;
    REQUIRE(model.at(0).group.group_info(model.at(0).who.id.signer, info_options, info) == CryptoError::None);
    for (const bool refused : {true, false})
    {
        CAPTURE(refused);
        Member&          outsider = model.at(3);
        mls::GroupConfig config   = outsider.config;
        config.crypto_policy      = refused ? &everywhere.store : &services.store;
        mls::Group              group;
        mls::ExternalJoinResult out;
        CHECK(group.join_external(outsider.who.id.signer, outsider.who.id.leaf, view(info), {}, {}, config, out) ==
              (refused ? CryptoError::PolicyRefused : CryptoError::None));
        CHECK(group.ready() == !refused);
        CHECK(out.message.empty() == refused);
    }
}

TEST_CASE("MLS policy: an epoch begins only under an allowed suite, and a ReInit leaves a refused one",
          "[crypto][net-2][mls]")
{
    TestInstallation installation;
    TestInstallation lagging; // member 2's installation, which never receives generation 2
    apply_generation(installation, 1U, false);
    apply_generation(lagging, 1U, false);
    Model model(classical(), {}, &installation.store);
    model.at(2).config.crypto_policy = &lagging.store;
    const mls::Proposal adds[]       = {add_of(model.at(1)), add_of(model.at(2))};
    mls::CommitOptions  options;
    options.proposals = ConstSpan<mls::Proposal>(adds);
    model.commit(0U, options, {1U, 2U});
    const u64 epoch = model.at(0).group.epoch();

    // Generation 2 refuses classical-only key exchange: no member under it begins another epoch of the group.
    apply_generation(installation, 2U, true);
    mls::CommitResult refused;
    CHECK(model.at(0).group.commit(model.at(0).who.id.signer, {}, refused) == CryptoError::PolicyRefused);
    CHECK(refused.message.empty());
    CHECK_FALSE(refused.staged.ready());

    // The epoch's keys stay until it ends, as a TLS session's until its next key update: application data flows.
    model.exchange({});

    // The lagging member's commit is refused by both members under generation 2, their epoch unchanged.
    mls::CommitResult stale;
    REQUIRE(model.at(2).group.commit(model.at(2).who.id.signer, {}, stale) == CryptoError::None);
    for (const usize i : {usize{0U}, usize{1U}})
    {
        mls::ReceivedMessage received;
        CHECK(model.at(i).group.process(view(stale.message), received) == CryptoError::PolicyRefused);
        CHECK_FALSE(received.commit().ready());
        CHECK(model.at(i).group.epoch() == epoch);
    }

    // A ReInit to a suite generation 2 refuses (X25519 with ChaCha20-Poly1305) is refused; one to the hybrid suite
    // is committed, received by the lagging member too, and ends the group everywhere.
    const Bytes   new_id = random_of(16U);
    mls::Proposal target[] = {reinit_to(view(new_id), iana::kMls128Dhkemx25519Chacha20poly1305Sha256Ed25519)};
    mls::CommitOptions reinit;
    reinit.proposals = ConstSpan<mls::Proposal>(target);
    mls::CommitResult result;
    CHECK(model.at(0).group.commit(model.at(0).who.id.signer, reinit, result) == CryptoError::PolicyRefused);
    target[0] = reinit_to(view(new_id), hybrid().id);
    REQUIRE(model.at(0).group.commit(model.at(0).who.id.signer, reinit, result) == CryptoError::None);
    model.deliver(0U, result.message);
    REQUIRE(model.at(0).group.merge(result.staged) == CryptoError::None);
    for (usize i = 0U; i < 3U; ++i)
    {
        CHECK(model.at(i).group.ended());
    }

    // Member 0 creates the hybrid group under generation 2; members 1 and 2 join it from its Welcome.
    Joiner    creator;
    Joiner    joiners[2];
    TestStore stores[3];
    make_joiner(hybrid(), creator, 0U);
    mls::GroupConfig creator_config = model.at(0).config;
    creator_config.store            = &stores[0];
    mls::KeyPackage packages[2];
    for (usize i = 0U; i < 2U; ++i)
    {
        make_joiner(hybrid(), joiners[i], static_cast<crd::u32>(i + 1U));
        packages[i] = joiners[i].package;
    }
    mls::ResumeOptions resume;
    resume.members = ConstSpan<mls::KeyPackage>(packages);
    mls::Group        next;
    mls::ResumeResult out;
    REQUIRE(next.reinit(model.at(0).group, creator.id.signer, creator.id.leaf, view(creator.id.hpke_private), resume,
                        creator_config, out) == CryptoError::None);
    CHECK(&next.suite() == &hybrid());
    const Bytes welcome_bytes(out.welcome.begin(), out.welcome.end());
    for (usize i = 0U; i < 2U; ++i)
    {
        mls::GroupConfig   config  = model.at(i + 1U).config;
        config.store               = &stores[i + 1U];
        const mls::Message welcome = message_of(welcome_bytes);
        mls::Group         joined;
        const CryptoError  error   = joined.join(welcome.body, joiners[i].package, view(joiners[i].init_private),
                                                 view(joiners[i].id.hpke_private), {}, config, &model.at(i + 1U).group);
        REQUIRE(error == CryptoError::None);
        CHECK(&joined.suite() == &hybrid());
        CHECK(equal(joined.epoch_authenticator(), next.epoch_authenticator()));
    }
    mls::CommitResult onward;
    CHECK(next.commit(creator.id.signer, {}, onward) == CryptoError::None);
}

TEST_CASE("MLS policy: a dated refusal reaches a group when its clock passes the date, and the clock never goes back",
          "[crypto][net-2][mls]")
{
    TestInstallation installation;
    apply_generation(installation, 1U, true, PolicyScope::Everywhere, kNow + 100U);
    Model       model(classical(), {}, &installation.store);
    mls::Group& group  = model.at(0).group;
    const auto& signer = model.at(0).who.id.signer;
    mls::CommitResult result;
    REQUIRE(group.commit(signer, {}, result) == CryptoError::None);
    REQUIRE(group.merge(result.staged) == CryptoError::None);

    group.set_now(kNow + 99U);
    REQUIRE(group.commit(signer, {}, result) == CryptoError::None);
    REQUIRE(group.merge(result.staged) == CryptoError::None);
    group.set_now(kNow + 100U);
    CHECK(group.commit(signer, {}, result) == CryptoError::PolicyRefused);
    group.set_now(kNow);
    CHECK(group.commit(signer, {}, result) == CryptoError::PolicyRefused);
    CHECK(group.epoch() == 2U);
}
