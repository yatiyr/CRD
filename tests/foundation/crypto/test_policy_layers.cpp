// crd-crypto -- a product's policy layer over Cerid's (decision S25, docs/design/network-foundation.md#policy-layers):
// the product layer only tightens (both layers accept, the earlier refusal wins, the product's preference among what
// both accept, every attempted loosening ignored and reported), scopes and protocol minimums compose, either layer
// missing or expired fails closed, each layer keeps its own trust set, floor and saved state, and the composed answer
// is what sessions read.

#include "policy_test_signer.hpp"
#include "temp_directory.hpp"

#include <crd/crypto/policy.hpp>
#include <crd/crypto/policy_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using crd::crypto::test::accepting_all;
using crd::crypto::test::kProductTrust;
using crd::crypto::test::kTestTrust;
using crd::crypto::test::prefer_rule;
using crd::crypto::test::refuse_rule;
using crd::crypto::test::set_rule;
using crd::crypto::test::sign_generation;
using crd::crypto::test::TempDirectory;
using crd::crypto::test::TestInstallation;
using crd::crypto::test::TestVerifier;

constexpr u64 kDay = 86400U;
constexpr u64 kT0  = 1800000000U; // 2027-01-15

// Cerid's layer and a product layer over it, the product with its own trust set.
struct Layers
{
    TestInstallation cerid;
    TestInstallation product{ConstSpan<TrustKey>(kProductTrust), &cerid.store};
};

// A generation accepting every algorithm with no preference: the product's starting point.
PolicyContent plain(u64 number, u64 issued = kT0)
{
    PolicyContent content = accepting_all(number, issued);
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        content.rules[i].preferred_from  = 0U;
        content.rules[i].preferred_until = 0U;
    }
    return content;
}

struct Signed
{
    u8    bytes[4096] = {};
    usize size        = 0U;

    [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(bytes, size); }
};

Signed signed_by(ConstSpan<TrustKey> trust, const PolicyContent& content)
{
    Signed out;
    REQUIRE(sign_generation(trust, content, out.bytes, out.size) == PolicyError::None);
    return out;
}
} // namespace

TEST_CASE("Policy layers: a product layer only tightens", "[crypto][net-std][policy-layers]")
{
    Layers layers;
    // Cerid's: everything accepted, the hybrid groups preferred, X25519 refused from day 10.
    PolicyContent cerid = accepting_all(1U, kT0);
    set_rule(cerid, {AlgorithmId::X25519, 0U, 0U, 0U, kT0 + 10U * kDay});
    REQUIRE(layers.cerid.apply(cerid, kT0) == PolicyError::None);

    // The product's: nothing preferred but ChaCha20-Poly1305, AES-128-GCM refused, X25519 refused from day 5.
    PolicyContent product = plain(1U, kT0);
    prefer_rule(product, AlgorithmId::Chacha20Poly1305);
    refuse_rule(product, AlgorithmId::Aes128Gcm);
    set_rule(product, {AlgorithmId::X25519, 0U, 0U, 0U, kT0 + 5U * kDay});
    REQUIRE(layers.product.apply(product, kT0) == PolicyError::None);

    const PolicySnapshot snapshot = layers.product.store.snapshot();
    CHECK(snapshot.layered());
    CHECK(snapshot.generation() == 1U);
    CHECK(snapshot.base_generation() == 1U);

    // The product's preference decides among what both accept: Cerid's preferred hybrid only Accepted here.
    CHECK(layers.cerid.store.snapshot().status(AlgorithmId::X25519Mlkem768, kT0) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::X25519Mlkem768, kT0) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::Chacha20Poly1305, kT0) == AlgorithmStatus::Preferred);
    // A product refusal tightens.
    CHECK(layers.cerid.store.snapshot().status(AlgorithmId::Aes128Gcm, kT0) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::Aes128Gcm, kT0) == AlgorithmStatus::Refused);
    // The earlier refusal date wins.
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 5U * kDay - 1U) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 5U * kDay) == AlgorithmStatus::Refused);
    // A suite is refused when either layer refuses one of its algorithms.
    const AlgorithmId suite[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::Aes128Gcm};
    CHECK(layers.cerid.store.snapshot().status(ConstSpan<AlgorithmId>(suite), kT0) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(ConstSpan<AlgorithmId>(suite), kT0) == AlgorithmStatus::Refused);

    // Nothing above loosens Cerid's layer.
    PolicyLoosening found[4];
    CHECK(snapshot.loosenings(Span<PolicyLoosening>(found)) == 0U);
    // An unlayered snapshot reports none and has no base.
    const PolicySnapshot alone = layers.cerid.store.snapshot();
    CHECK_FALSE(alone.layered());
    CHECK(alone.base_generation() == 0U);
    CHECK(alone.loosenings(Span<PolicyLoosening>(found)) == 0U);
}

TEST_CASE("Policy layers: every attempted loosening is ignored and reported", "[crypto][net-std][policy-layers]")
{
    Layers layers;
    // Cerid's: X25519 refused from day 10, Ed25519 accepted only from day 2, AES-128-GCM refused, ML-DSA-44 without
    // a rule.
    PolicyContent cerid = accepting_all(1U, kT0);
    set_rule(cerid, {AlgorithmId::X25519, 0U, 0U, 0U, kT0 + 10U * kDay});
    set_rule(cerid, {AlgorithmId::Ed25519, kT0 + 2U * kDay, kT0 + 2U * kDay, kT0 + 2U * kDay, kNever});
    refuse_rule(cerid, AlgorithmId::Aes128Gcm);
    for (u32 i = 0U; i < cerid.rule_count; ++i)
    {
        if (cerid.rules[i].algorithm == AlgorithmId::MlDsa44)
        {
            cerid.rules[i] = cerid.rules[--cerid.rule_count];
            break;
        }
    }
    REQUIRE(layers.cerid.apply(cerid, kT0) == PolicyError::None);

    // The product tries each loosening: X25519 until day 20, Ed25519 from 1970, AES-128-GCM preferred, ML-DSA-44.
    PolicyContent product = plain(1U, kT0);
    set_rule(product, {AlgorithmId::X25519, 0U, 0U, 0U, kT0 + 20U * kDay});
    prefer_rule(product, AlgorithmId::Ed25519);
    prefer_rule(product, AlgorithmId::Aes128Gcm);
    REQUIRE(layers.product.apply(product, kT0) == PolicyError::None);
    const PolicySnapshot snapshot = layers.product.store.snapshot();

    // None of them takes effect.
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 10U * kDay - 1U) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 10U * kDay) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::Ed25519, kT0 + 2U * kDay - 1U) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::Ed25519, kT0 + 2U * kDay) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::Aes128Gcm, kT0) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::MlDsa44, kT0) == AlgorithmStatus::Refused);

    // Each is reported once, in the product's rule order, with the base rule it exceeds (none for ML-DSA-44).
    PolicyLoosening found[8];
    const u32       count = snapshot.loosenings(Span<PolicyLoosening>(found));
    REQUIRE(count == 4U);
    u32 seen = 0U;
    for (u32 i = 0U; i < count; ++i)
    {
        const PolicyLoosening& l = found[i];
        if (l.rule.algorithm == AlgorithmId::X25519)
        {
            CHECK(l.base.refused_from == kT0 + 10U * kDay);
            seen |= 1U;
        }
        else if (l.rule.algorithm == AlgorithmId::Ed25519)
        {
            CHECK(l.base.accepted_from == kT0 + 2U * kDay);
            seen |= 2U;
        }
        else if (l.rule.algorithm == AlgorithmId::Aes128Gcm)
        {
            CHECK(l.base.refused_from == 1U);
            seen |= 4U;
        }
        else if (l.rule.algorithm == AlgorithmId::MlDsa44)
        {
            CHECK(l.base.algorithm == AlgorithmId{});
            seen |= 8U;
        }
    }
    CHECK(seen == 15U);
    // A short buffer holds the first ones and the count stays whole.
    PolicyLoosening first[1];
    CHECK(snapshot.loosenings(Span<PolicyLoosening>(first)) == 4U);
    CHECK(first[0].rule.algorithm == found[0].rule.algorithm);
    CHECK(snapshot.loosenings(Span<PolicyLoosening>()) == 4U);
}

TEST_CASE("Policy layers: scopes and protocol minimums compose", "[crypto][net-std][policy-layers]")
{
    Layers layers;
    // Cerid's refuses classical X25519 for its own services from day 3 and sets game protocol 2 to at least 1.2.
    PolicyContent cerid = accepting_all(1U, kT0);
    set_rule(cerid, {AlgorithmId::X25519, 0U, 0U, 0U, kT0 + 3U * kDay, PolicyScope::CeridServices});
    cerid.minimums[0]   = {0x0002U, 1U, 2U};
    cerid.minimum_count = 1U;
    REQUIRE(layers.cerid.apply(cerid, kT0) == PolicyError::None);

    // The product accepts X25519 everywhere (its Everywhere rule also governs its service sessions), refuses
    // ChaCha20-Poly1305 for service sessions only and sets protocol 2 to at least 1.1 and protocol 3 to 2.0.
    PolicyContent product = plain(1U, kT0);
    refuse_rule(product, AlgorithmId::Chacha20Poly1305, PolicyScope::CeridServices);
    product.minimums[0]   = {0x0002U, 1U, 1U};
    product.minimums[1]   = {0x0003U, 2U, 0U};
    product.minimum_count = 2U;
    REQUIRE(layers.product.apply(product, kT0) == PolicyError::None);
    const PolicySnapshot snapshot = layers.product.store.snapshot();

    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 3U * kDay) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 3U * kDay, PolicyScope::CeridServices) ==
          AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::Chacha20Poly1305, kT0) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::Chacha20Poly1305, kT0, PolicyScope::CeridServices) ==
          AlgorithmStatus::Refused);
    // A version runs when both layers allow it: the higher minimum of each protocol.
    CHECK_FALSE(snapshot.allows_version(0x0002U, 1U, 1U));
    CHECK(snapshot.allows_version(0x0002U, 1U, 2U));
    CHECK_FALSE(snapshot.allows_version(0x0003U, 1U, 9U));
    CHECK(snapshot.allows_version(0x0003U, 2U, 0U));
    CHECK(snapshot.allows_version(0x0004U, 0U, 0U));
    // The product's services refusal tightens, its Everywhere X25519 rule stays within Cerid's Everywhere rule.
    PolicyLoosening found[2];
    CHECK(snapshot.loosenings(Span<PolicyLoosening>(found)) == 0U);
}

TEST_CASE("Policy layers: either layer missing or expired refuses everything", "[crypto][net-std][policy-layers]")
{
    Layers layers;
    const auto refused_all = [](const PolicySnapshot& snapshot, u64 now)
    {
        return std::ranges::all_of(kAlgorithmIds,
                                   [&](AlgorithmId algorithm)
                                   {
                                       return snapshot.status(algorithm, now) == AlgorithmStatus::Refused &&
                                              snapshot.status(algorithm, now, PolicyScope::CeridServices) ==
                                                  AlgorithmStatus::Refused;
                                   });
    };

    // No generation in either layer, then Cerid's alone: a configured product layer without a generation is closed.
    CHECK(layers.product.store.snapshot().freshness(kT0) == Freshness::None);
    REQUIRE(layers.cerid.apply(accepting_all(1U, kT0), kT0) == PolicyError::None);
    CHECK(layers.cerid.store.snapshot().status(AlgorithmId::X25519, kT0) == AlgorithmStatus::Accepted);
    CHECK(layers.product.store.snapshot().freshness(kT0) == Freshness::None);
    CHECK(refused_all(layers.product.store.snapshot(), kT0));

    // The product's generation alone, over an empty Cerid layer, is closed too.
    {
        Layers empty_base;
        REQUIRE(empty_base.product.apply(plain(1U, kT0), kT0) == PolicyError::None);
        CHECK(empty_base.product.store.snapshot().freshness(kT0) == Freshness::None);
        CHECK(refused_all(empty_base.product.store.snapshot(), kT0));
    }

    // Both: Cerid's (issued kT0) lasts 30 days and 7 of grace, the product's (issued day 10) until day 40 and 47.
    REQUIRE(layers.product.apply(plain(1U, kT0 + 10U * kDay), kT0 + 10U * kDay) == PolicyError::None);
    const PolicySnapshot snapshot = layers.product.store.snapshot();
    CHECK(snapshot.freshness(kT0 + 29U * kDay) == Freshness::Current);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 29U * kDay) == AlgorithmStatus::Accepted);
    // Cerid's grace is the composed grace; its expiry closes the product.
    CHECK(snapshot.freshness(kT0 + 30U * kDay) == Freshness::Grace);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 36U * kDay) == AlgorithmStatus::Accepted);
    CHECK(snapshot.freshness(kT0 + 37U * kDay) == Freshness::Expired);
    CHECK(refused_all(snapshot, kT0 + 37U * kDay));

    // The product's expiry closes it under a current Cerid layer.
    REQUIRE(layers.cerid.apply(accepting_all(2U, kT0 + 40U * kDay), kT0 + 40U * kDay) == PolicyError::None);
    const PolicySnapshot later = layers.product.store.snapshot();
    CHECK(later.base_generation() == 2U);
    CHECK(later.freshness(kT0 + 40U * kDay) == Freshness::Grace);
    CHECK(later.status(AlgorithmId::X25519, kT0 + 46U * kDay) == AlgorithmStatus::Accepted);
    CHECK(later.freshness(kT0 + 47U * kDay) == Freshness::Expired);
    CHECK(refused_all(later, kT0 + 47U * kDay));
}

TEST_CASE("Policy layers: each layer keeps its own trust set, floor and generations",
          "[crypto][net-std][policy-layers]")
{
    Layers layers;
    REQUIRE(layers.cerid.apply(accepting_all(5U, kT0), kT0) == PolicyError::None);

    // A product generation signed by Cerid's keys is not the product's; Cerid's store refuses the product's keys.
    CHECK(layers.product.store.apply(signed_by(ConstSpan<TrustKey>(kTestTrust), plain(1U)).view(), kT0) ==
          PolicyError::MissingSignature);
    CHECK(layers.cerid.store.apply(signed_by(ConstSpan<TrustKey>(kProductTrust), accepting_all(6U, kT0)).view(),
                                   kT0) == PolicyError::MissingSignature);

    // Generation numbers grow per layer: the product's 1 under Cerid's 5, then its own rollback is refused.
    REQUIRE(layers.product.apply(plain(2U), kT0) == PolicyError::None);
    CHECK(layers.product.apply(plain(1U), kT0) == PolicyError::Rollback);
    CHECK(layers.product.store.floor_generation() == 2U);
    CHECK(layers.cerid.store.floor_generation() == 5U);

    // A remote disable in Cerid's layer reaches the product's later snapshots; a running session keeps its own.
    const PolicySnapshot running = layers.product.store.snapshot();
    PolicyContent        disable = accepting_all(6U, kT0 + kDay);
    refuse_rule(disable, AlgorithmId::Chacha20Poly1305);
    REQUIRE(layers.cerid.apply(disable, kT0 + kDay) == PolicyError::None);
    CHECK(running.status(AlgorithmId::Chacha20Poly1305, kT0 + kDay) == AlgorithmStatus::Accepted);
    CHECK(running.base_generation() == 5U);
    const PolicySnapshot after = layers.product.store.snapshot();
    CHECK(after.status(AlgorithmId::Chacha20Poly1305, kT0 + kDay) == AlgorithmStatus::Refused);
    CHECK(after.base_generation() == 6U);
    CHECK(after.generation() == 2U);

    // Two layers at most: a layer over the product layer is a bad config and refuses everything.
    TestInstallation third(ConstSpan<TrustKey>(kProductTrust), &layers.product.store);
    CHECK(validate_config(PolicyConfig{ConstSpan<TrustKey>(kProductTrust), &third.verifier,
                                       ConstSpan<AlgorithmId>(kAlgorithmIds), 0U, &layers.product.store}) ==
          PolicyError::BadConfig);
    CHECK(third.apply(plain(1U), kT0) == PolicyError::BadConfig);
    CHECK(third.store.snapshot().freshness(kT0) == Freshness::None);
    CHECK(third.store.snapshot().status(AlgorithmId::X25519, kT0 + kDay) == AlgorithmStatus::Refused);
}

TEST_CASE("Policy layers: a product installation saves its own generation and floor",
          "[crypto][net-std][policy-layers]")
{
    const TestVerifier  verifier;
    const TempDirectory cerid_dir("policy-layer-cerid");
    const TempDirectory product_dir("policy-layer-product");
    const PolicyConfig  cerid_config{ConstSpan<TrustKey>(kTestTrust), &verifier, ConstSpan<AlgorithmId>(kAlgorithmIds),
                                     0U};
    const Signed        cerid_one = signed_by(ConstSpan<TrustKey>(kTestTrust), accepting_all(1U, kT0));
    PolicyContent       tightened = plain(3U, kT0);
    refuse_rule(tightened, AlgorithmId::X25519);
    const Signed product_three = signed_by(ConstSpan<TrustKey>(kProductTrust), tightened);
    const Signed product_two   = signed_by(ConstSpan<TrustKey>(kProductTrust), plain(2U, kT0));
    {
        PolicyInstallation cerid(cerid_config);
        REQUIRE(cerid.open(cerid_dir.dir(), cerid_one.view(), kT0).start.bootstrap == PolicyError::None);
        PolicyInstallation product(PolicyConfig{ConstSpan<TrustKey>(kProductTrust), &verifier,
                                                ConstSpan<AlgorithmId>(kAlgorithmIds), 0U, &cerid.store()});
        const PolicyOpen opened = product.open(product_dir.dir(), product_three.view(), kT0);
        CHECK(opened.start.bootstrap == PolicyError::None);
        CHECK(opened.saved == PolicyStateError::None);
        CHECK(product.store().snapshot().status(AlgorithmId::X25519, kT0) == AlgorithmStatus::Refused);
        CHECK(product.store().snapshot().status(AlgorithmId::Ed25519, kT0) == AlgorithmStatus::Accepted);
    }
    {
        // A restart with an older product bootstrap keeps the saved generation 3: the product's floor is its own.
        PolicyInstallation cerid(cerid_config);
        REQUIRE(cerid.open(cerid_dir.dir(), cerid_one.view(), kT0).start.persisted == PolicyError::None);
        PolicyInstallation product(PolicyConfig{ConstSpan<TrustKey>(kProductTrust), &verifier,
                                                ConstSpan<AlgorithmId>(kAlgorithmIds), 0U, &cerid.store()});
        const PolicyOpen opened = product.open(product_dir.dir(), product_two.view(), kT0);
        CHECK(opened.start.persisted == PolicyError::None);
        CHECK(opened.start.bootstrap == PolicyError::Rollback);
        CHECK(product.store().floor_generation() == 3U);
        CHECK(cerid.store().floor_generation() == 1U);
        const PolicySnapshot snapshot = product.store().snapshot();
        CHECK(snapshot.generation() == 3U);
        CHECK(snapshot.base_generation() == 1U);
        CHECK(snapshot.status(AlgorithmId::X25519, kT0) == AlgorithmStatus::Refused);
    }
}
