// Phase 3.0 v1n5 — Profile substrate tests (ADR-0060).
//
// Four cases exercising the data layout + load round-trip; the resolver
// (additive composition, predicate evaluation against ProfileContext) lands
// in v1n6 and gets its own test surface.
//
//   1. PredicateRecord + ProfileFileInfo binary layouts pinned (sizes,
//      alignments, enum byte values per ADR-0060 §2 + §5).
//   2. Single-profile round-trip — one rule with two predicates and a
//      two-id apply bundle.
//   3. Multi-profile round-trip — three rules with different predicate
//      sets / bundles; cross-link integrity (FBND.rule_idx == seq index).
//   4. ProfileLoader rejects mismatched FourCC (artifact built as 'PROF'
//      passes; an empty / wrong-FourCC blob fails cleanly).

#include <crd/memory/allocator.hpp>
#include <crd/profile/profile.hpp>
#include <crd/profile/profile_artifact_builder.hpp>
#include <crd/profile/profile_context.hpp>
#include <crd/profile/profile_loader.hpp>
#include <crd/profile/profile_predicate.hpp>
#include <crd/profile/profile_resource.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
[[nodiscard]] crd::resources::LoadContext make_ctx(
    crd::resources::ResourceId id,
    crd::containers::ConstSpan<crd::u8> bytes,
    crd::memory::IAllocator* alloc) noexcept
{
    crd::resources::LoadContext ctx{};
    ctx.id        = id;
    ctx.bytes     = bytes;
    ctx.allocator = alloc;
    return ctx;
}
} // namespace

TEST_CASE("Profile substrate: PredicateRecord + ProfileFileInfo binary layouts pinned",
          "[profile][schema][layout]")
{
    // Predicate field enum codes (append-only — never insert).
    CHECK(static_cast<crd::u8>(crd::profile::PredicateField::Os)        == 0U);
    CHECK(static_cast<crd::u8>(crd::profile::PredicateField::GpuTier)   == 1U);
    CHECK(static_cast<crd::u8>(crd::profile::PredicateField::Domain)    == 2U);
    CHECK(static_cast<crd::u8>(crd::profile::PredicateField::Mode)      == 3U);
    CHECK(static_cast<crd::u8>(crd::profile::PredicateField::TargetFps) == 4U);
    CHECK(static_cast<crd::u8>(crd::profile::PredicateField::CpuCores)  == 5U);

    // Operator enum codes.
    CHECK(static_cast<crd::u8>(crd::profile::PredicateOp::Equal)     == 0U);
    CHECK(static_cast<crd::u8>(crd::profile::PredicateOp::GreaterEq) == 1U);
    CHECK(static_cast<crd::u8>(crd::profile::PredicateOp::LessEq)    == 2U);
    CHECK(static_cast<crd::u8>(crd::profile::PredicateOp::InMask)    == 3U);

    // Domain / mode / GPU / OS enums — first/last sentinels checked.
    CHECK(static_cast<crd::u8>(crd::profile::OperatingSystem::Unknown) == 0U);
    CHECK(static_cast<crd::u8>(crd::profile::OperatingSystem::MacOS)   == 3U);
    CHECK(static_cast<crd::u8>(crd::profile::GpuTier::Unknown)         == 0U);
    CHECK(static_cast<crd::u8>(crd::profile::GpuTier::Ultra)           == 4U);
    CHECK(static_cast<crd::u8>(crd::profile::ProjectDomain::Unknown)   == 0U);
    CHECK(static_cast<crd::u8>(crd::profile::ProjectDomain::Cinematic) == 4U);
    CHECK(static_cast<crd::u8>(crd::profile::AppMode::Unknown)         == 0U);
    CHECK(static_cast<crd::u8>(crd::profile::AppMode::Headless)        == 3U);

    // Pinned layouts.
    CHECK(sizeof(crd::profile::PredicateRecord)  == 8U);
    CHECK(alignof(crd::profile::PredicateRecord) == 4U);
    CHECK(sizeof(crd::profile::ProfileFileInfo)  == 16U);

    // ProfileContext defaults — used by detect_* helpers in v1n6.
    crd::profile::ProfileContext ctx{};
    CHECK(ctx.os         == crd::profile::OperatingSystem::Unknown);
    CHECK(ctx.gpu_tier   == crd::profile::GpuTier::Unknown);
    CHECK(ctx.domain     == crd::profile::ProjectDomain::Unknown);
    CHECK(ctx.mode       == crd::profile::AppMode::Unknown);
    CHECK(ctx.target_fps == 60);
    CHECK(ctx.cpu_cores  == 1);
}

TEST_CASE("Single-profile round-trip through ProfileArtifactBuilder + ProfileLoader",
          "[profile][round-trip]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xCAFE'BABEULL, 0x1ULL};

    // Build: priority 100, two predicates (os==Windows, gpu_tier>=High),
    // bundle of 2 ResourceIds.
    crd::containers::Array<crd::profile::PredicateRecord> preds(alloc);
    preds.push_back(crd::profile::PredicateRecord{
        crd::profile::PredicateField::Os,
        crd::profile::PredicateOp::Equal,
        {0U, 0U},
        static_cast<crd::u32>(crd::profile::OperatingSystem::Windows),
    });
    preds.push_back(crd::profile::PredicateRecord{
        crd::profile::PredicateField::GpuTier,
        crd::profile::PredicateOp::GreaterEq,
        {0U, 0U},
        static_cast<crd::u32>(crd::profile::GpuTier::High),
    });

    crd::containers::Array<crd::resources::ResourceId> bundle(alloc);
    bundle.push_back(crd::resources::ResourceId{0x11ULL, 0x22ULL});
    bundle.push_back(crd::resources::ResourceId{0x33ULL, 0x44ULL});

    crd::profile::ProfileArtifactBuilder builder{alloc, 1U, id};
    builder.add_rule(100U,
                     crd::containers::ConstSpan<crd::profile::PredicateRecord>{preds.data(), preds.size()},
                     crd::containers::ConstSpan<crd::resources::ResourceId>{bundle.data(), bundle.size()});
    auto bytes = builder.build();
    REQUIRE(!bytes.empty());

    crd::profile::ProfileLoader loader{alloc};
    auto* payload = loader.load(make_ctx(
        id,
        crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()},
        alloc));
    REQUIRE(payload != nullptr);

    auto* res = static_cast<crd::profile::ProfileResource*>(payload);
    CHECK(res->schema_version() == 1U);
    REQUIRE(res->profiles().size() == 1U);

    const auto& p = res->profiles()[0];
    CHECK(p.priority == 100U);
    REQUIRE(p.predicates.size() == 2U);
    CHECK(p.predicates[0].field == crd::profile::PredicateField::Os);
    CHECK(p.predicates[0].op    == crd::profile::PredicateOp::Equal);
    CHECK(p.predicates[0].value == static_cast<crd::u32>(crd::profile::OperatingSystem::Windows));
    CHECK(p.predicates[1].field == crd::profile::PredicateField::GpuTier);
    CHECK(p.predicates[1].op    == crd::profile::PredicateOp::GreaterEq);
    CHECK(p.predicates[1].value == static_cast<crd::u32>(crd::profile::GpuTier::High));

    REQUIRE(p.apply_bundle.size() == 2U);
    CHECK(p.apply_bundle[0] == crd::resources::ResourceId{0x11ULL, 0x22ULL});
    CHECK(p.apply_bundle[1] == crd::resources::ResourceId{0x33ULL, 0x44ULL});

    loader.unload(payload);
}

TEST_CASE("Multi-profile round-trip preserves rule order + bundle cross-links",
          "[profile][round-trip][multi]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xFEEDULL, 0xFACEULL};

    // 3 rules with different predicate counts + bundles.
    auto make_pred = [](crd::profile::PredicateField f,
                       crd::profile::PredicateOp    op,
                       crd::u32                     v) {
        return crd::profile::PredicateRecord{f, op, {0U, 0U}, v};
    };

    crd::profile::ProfileArtifactBuilder builder{alloc, 1U, id};

    // Rule 0: priority 10 — empty predicates, single-id bundle (always-on baseline).
    crd::containers::Array<crd::resources::ResourceId> b0(alloc);
    b0.push_back(crd::resources::ResourceId{0xA0ULL, 0xA0ULL});
    builder.add_rule(10U,
                     crd::containers::ConstSpan<crd::profile::PredicateRecord>{},
                     crd::containers::ConstSpan<crd::resources::ResourceId>{b0.data(), b0.size()});

    // Rule 1: priority 50 — one predicate, three-id bundle.
    crd::containers::Array<crd::profile::PredicateRecord> p1(alloc);
    p1.push_back(make_pred(crd::profile::PredicateField::Domain,
                           crd::profile::PredicateOp::Equal,
                           static_cast<crd::u32>(crd::profile::ProjectDomain::Game)));
    crd::containers::Array<crd::resources::ResourceId> b1(alloc);
    b1.push_back(crd::resources::ResourceId{0xB1ULL, 0xB1ULL});
    b1.push_back(crd::resources::ResourceId{0xB2ULL, 0xB2ULL});
    b1.push_back(crd::resources::ResourceId{0xB3ULL, 0xB3ULL});
    builder.add_rule(50U,
                     crd::containers::ConstSpan<crd::profile::PredicateRecord>{p1.data(), p1.size()},
                     crd::containers::ConstSpan<crd::resources::ResourceId>{b1.data(), b1.size()});

    // Rule 2: priority 200 — three predicates, empty bundle (match-only profile).
    crd::containers::Array<crd::profile::PredicateRecord> p2(alloc);
    p2.push_back(make_pred(crd::profile::PredicateField::TargetFps,
                           crd::profile::PredicateOp::GreaterEq,
                           120U));
    p2.push_back(make_pred(crd::profile::PredicateField::CpuCores,
                           crd::profile::PredicateOp::GreaterEq,
                           8U));
    p2.push_back(make_pred(crd::profile::PredicateField::Mode,
                           crd::profile::PredicateOp::Equal,
                           static_cast<crd::u32>(crd::profile::AppMode::Runtime)));
    builder.add_rule(200U,
                     crd::containers::ConstSpan<crd::profile::PredicateRecord>{p2.data(), p2.size()},
                     crd::containers::ConstSpan<crd::resources::ResourceId>{});

    auto bytes = builder.build();

    crd::profile::ProfileLoader loader{alloc};
    auto* payload = loader.load(make_ctx(
        id,
        crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()},
        alloc));
    REQUIRE(payload != nullptr);

    auto* res = static_cast<crd::profile::ProfileResource*>(payload);
    REQUIRE(res->profiles().size() == 3U);

    // Rule 0 — empty predicates, single bundle.
    CHECK(res->profiles()[0].priority == 10U);
    CHECK(res->profiles()[0].predicates.empty());
    REQUIRE(res->profiles()[0].apply_bundle.size() == 1U);
    CHECK(res->profiles()[0].apply_bundle[0] == crd::resources::ResourceId{0xA0ULL, 0xA0ULL});

    // Rule 1 — one predicate, three-id bundle.
    CHECK(res->profiles()[1].priority == 50U);
    REQUIRE(res->profiles()[1].predicates.size() == 1U);
    CHECK(res->profiles()[1].predicates[0].field == crd::profile::PredicateField::Domain);
    REQUIRE(res->profiles()[1].apply_bundle.size() == 3U);

    // Rule 2 — three predicates, empty bundle.
    CHECK(res->profiles()[2].priority == 200U);
    REQUIRE(res->profiles()[2].predicates.size() == 3U);
    CHECK(res->profiles()[2].predicates[0].field == crd::profile::PredicateField::TargetFps);
    CHECK(res->profiles()[2].predicates[1].field == crd::profile::PredicateField::CpuCores);
    CHECK(res->profiles()[2].predicates[2].field == crd::profile::PredicateField::Mode);
    CHECK(res->profiles()[2].apply_bundle.empty());

    loader.unload(payload);
}

TEST_CASE("ProfileLoader rejects mismatched FourCC + malformed input",
          "[profile][loader][validation]")
{
    auto* alloc = crd::memory::default_allocator();

    // Empty bytes → CRDR header invalid → loader rejects.
    crd::profile::ProfileLoader loader{alloc};
    crd::resources::LoadContext empty_ctx{};
    empty_ctx.id        = crd::resources::ResourceId{1ULL, 1ULL};
    empty_ctx.bytes     = crd::containers::ConstSpan<crd::u8>{};
    empty_ctx.allocator = alloc;
    CHECK(loader.load(empty_ctx) == nullptr);

    // A CRDR blob with a wrong type_fourcc — fabricate via CrdrWriter.
    crd::resources::CrdrWriter wrong_writer{
        alloc,
        crd::resources::ResourceId{2ULL, 2ULL},
        crd::resources::make_fourcc('B', 'A', 'D', 'X')};
    crd::profile::ProfileFileInfo dummy_info{};
    dummy_info.schema_version = 1U;
    wrong_writer.add_chunk(crd::profile::kFourCC_FINF,
                           crd::containers::ConstSpan<crd::u8>{
                               reinterpret_cast<const crd::u8*>(&dummy_info), sizeof(dummy_info)});
    auto wrong_bytes = wrong_writer.finish();

    CHECK(loader.load(make_ctx(
              crd::resources::ResourceId{2ULL, 2ULL},
              crd::containers::ConstSpan<crd::u8>{wrong_bytes.data(), wrong_bytes.size()},
              alloc)) == nullptr);

    // Confirmation: a valid empty PROF artifact (zero rules) loads cleanly.
    crd::profile::ProfileArtifactBuilder good{alloc, 1U,
                                              crd::resources::ResourceId{3ULL, 3ULL}};
    auto good_bytes = good.build();
    auto* good_payload = loader.load(make_ctx(
        crd::resources::ResourceId{3ULL, 3ULL},
        crd::containers::ConstSpan<crd::u8>{good_bytes.data(), good_bytes.size()},
        alloc));
    REQUIRE(good_payload != nullptr);
    auto* good_res = static_cast<crd::profile::ProfileResource*>(good_payload);
    CHECK(good_res->profiles().empty());
    loader.unload(good_payload);
}
