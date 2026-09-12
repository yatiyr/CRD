// Phase 3.0 v1n6 — ProfileResolver + predicate evaluation + context detection.
//
// Four cases closing v1n:
//
//   1. evaluate_predicate against every operator (Equal / GreaterEq /
//      LessEq / InMask) across enum and integer fields.
//   2. Single-profile matching — one rule, multiple predicates, all must
//      pass.
//   3. Additive multi-profile composition — three profiles with mixed
//      priorities; matched bundles concatenate in priority-ascending
//      order; tie-break stable on file order.
//   4. No-match → empty bundle; null resource → empty bundle; OS
//      detection returns a known-platform value (not Unknown on a
//      supported runner); cpu_cores returns >= 1.

#include <crd/memory/allocator.hpp>
#include <crd/profile/profile.hpp>
#include <crd/profile/profile_artifact_builder.hpp>
#include <crd/profile/profile_context.hpp>
#include <crd/profile/profile_loader.hpp>
#include <crd/profile/profile_predicate.hpp>
#include <crd/profile/profile_resolver.hpp>
#include <crd/profile/profile_resource.hpp>
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
    crd::resources::LoadContext lc{};
    lc.id        = id;
    lc.bytes     = bytes;
    lc.allocator = alloc;
    return lc;
}

[[nodiscard]] crd::profile::PredicateRecord make_pred(crd::profile::PredicateField f,
                                                     crd::profile::PredicateOp    op,
                                                     crd::u32                     v) noexcept
{
    return crd::profile::PredicateRecord{f, op, {0U, 0U}, v};
}

// Build + load a profile artifact. The loader's allocator survives the
// returned ProfileResource* — caller must call `loader.unload(payload)`.
struct LoadedProfile
{
    crd::profile::ProfileResource* res    = nullptr;
    void*                          handle = nullptr;
};
} // namespace

TEST_CASE("evaluate_predicate covers all operators across enum + integer fields",
          "[profile][resolver][predicate]")
{
    crd::profile::ProfileContext ctx{};
    ctx.os         = crd::profile::OperatingSystem::Linux;   // 2
    ctx.gpu_tier   = crd::profile::GpuTier::High;            // 3
    ctx.domain     = crd::profile::ProjectDomain::Game;      // 1
    ctx.mode       = crd::profile::AppMode::Runtime;         // 2
    ctx.target_fps = 144;
    ctx.cpu_cores  = 16;

    // ===== Equal =====
    CHECK( crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::Os,
                  crd::profile::PredicateOp::Equal,
                  static_cast<crd::u32>(crd::profile::OperatingSystem::Linux)), ctx));
    CHECK_FALSE(crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::Os,
                  crd::profile::PredicateOp::Equal,
                  static_cast<crd::u32>(crd::profile::OperatingSystem::Windows)), ctx));

    CHECK( crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::TargetFps,
                  crd::profile::PredicateOp::Equal, 144U), ctx));
    CHECK_FALSE(crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::TargetFps,
                  crd::profile::PredicateOp::Equal, 60U), ctx));

    // ===== GreaterEq =====
    CHECK( crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::GpuTier,
                  crd::profile::PredicateOp::GreaterEq,
                  static_cast<crd::u32>(crd::profile::GpuTier::High)), ctx));
    CHECK( crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::GpuTier,
                  crd::profile::PredicateOp::GreaterEq,
                  static_cast<crd::u32>(crd::profile::GpuTier::Mid)), ctx));
    CHECK_FALSE(crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::GpuTier,
                  crd::profile::PredicateOp::GreaterEq,
                  static_cast<crd::u32>(crd::profile::GpuTier::Ultra)), ctx));

    // GreaterEq on signed integer field with negative comparand (sign-preserved
    // through u32 reinterpret).
    {
        const crd::i32 neg = -50;
        crd::u32 packed = 0;
        std::memcpy(&packed, &neg, sizeof(packed));
        CHECK(crd::profile::evaluate_predicate(
            make_pred(crd::profile::PredicateField::TargetFps,
                      crd::profile::PredicateOp::GreaterEq, packed), ctx));
    }

    // ===== LessEq =====
    CHECK( crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::CpuCores,
                  crd::profile::PredicateOp::LessEq, 16U), ctx));
    CHECK( crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::CpuCores,
                  crd::profile::PredicateOp::LessEq, 32U), ctx));
    CHECK_FALSE(crd::profile::evaluate_predicate(
        make_pred(crd::profile::PredicateField::CpuCores,
                  crd::profile::PredicateOp::LessEq, 8U), ctx));

    // ===== InMask =====
    {
        const crd::u32 game_or_sim =
            (1U << static_cast<crd::u32>(crd::profile::ProjectDomain::Game)) |
            (1U << static_cast<crd::u32>(crd::profile::ProjectDomain::Simulation));
        CHECK(crd::profile::evaluate_predicate(
            make_pred(crd::profile::PredicateField::Domain,
                      crd::profile::PredicateOp::InMask, game_or_sim), ctx));

        const crd::u32 daw_or_cinematic =
            (1U << static_cast<crd::u32>(crd::profile::ProjectDomain::Daw)) |
            (1U << static_cast<crd::u32>(crd::profile::ProjectDomain::Cinematic));
        CHECK_FALSE(crd::profile::evaluate_predicate(
            make_pred(crd::profile::PredicateField::Domain,
                      crd::profile::PredicateOp::InMask, daw_or_cinematic), ctx));
    }
}

TEST_CASE("ProfileResolver matches single profile when all predicates pass",
          "[profile][resolver][match]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xCAFEULL, 0x1ULL};

    // Profile: priority 50, predicates {os==Linux, gpu_tier>=High}, bundle of 2 ids.
    crd::containers::Array<crd::profile::PredicateRecord> preds(alloc);
    preds.push_back(make_pred(crd::profile::PredicateField::Os,
                              crd::profile::PredicateOp::Equal,
                              static_cast<crd::u32>(crd::profile::OperatingSystem::Linux)));
    preds.push_back(make_pred(crd::profile::PredicateField::GpuTier,
                              crd::profile::PredicateOp::GreaterEq,
                              static_cast<crd::u32>(crd::profile::GpuTier::High)));

    crd::containers::Array<crd::resources::ResourceId> bundle(alloc);
    bundle.push_back(crd::resources::ResourceId{0xAAULL, 0xAAULL});
    bundle.push_back(crd::resources::ResourceId{0xBBULL, 0xBBULL});

    crd::profile::ProfileArtifactBuilder builder{alloc, 1U, id};
    builder.add_rule(50U,
                     crd::containers::ConstSpan<crd::profile::PredicateRecord>{preds.data(), preds.size()},
                     crd::containers::ConstSpan<crd::resources::ResourceId>{bundle.data(), bundle.size()});
    auto bytes = builder.build();

    crd::profile::ProfileLoader loader{alloc};
    auto* payload = loader.load(make_ctx(
        id, crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()}, alloc));
    REQUIRE(payload != nullptr);
    auto* res = static_cast<crd::profile::ProfileResource*>(payload);

    crd::profile::ProfileResolver resolver{alloc};
    resolver.set_resource(res);
    REQUIRE(resolver.resource() == res);

    crd::containers::Array<crd::resources::ResourceId> out(alloc);

    // Matching context.
    {
        crd::profile::ProfileContext ctx{};
        ctx.os       = crd::profile::OperatingSystem::Linux;
        ctx.gpu_tier = crd::profile::GpuTier::Ultra; // High also OK; Ultra >= High is true.
        const crd::u32 match_count = resolver.resolve(ctx, out);
        CHECK(match_count == 1U);
        REQUIRE(out.size() == 2U);
        CHECK(out[0] == crd::resources::ResourceId{0xAAULL, 0xAAULL});
        CHECK(out[1] == crd::resources::ResourceId{0xBBULL, 0xBBULL});
    }

    // Non-matching context (Windows fails the os predicate).
    {
        crd::profile::ProfileContext ctx{};
        ctx.os       = crd::profile::OperatingSystem::Windows;
        ctx.gpu_tier = crd::profile::GpuTier::Ultra;
        const crd::u32 match_count = resolver.resolve(ctx, out);
        CHECK(match_count == 0U);
        CHECK(out.empty());
    }

    loader.unload(payload);
}

TEST_CASE("ProfileResolver composes multiple matching profiles in priority-ascending order",
          "[profile][resolver][additive][composition]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xFEEDULL, 0x2ULL};

    crd::profile::ProfileArtifactBuilder builder{alloc, 1U, id};

    const crd::resources::ResourceId platform_preset{0x10ULL, 0x10ULL};
    const crd::resources::ResourceId tier_preset    {0x20ULL, 0x20ULL};
    const crd::resources::ResourceId domain_preset_a{0x30ULL, 0x30ULL};
    const crd::resources::ResourceId domain_preset_b{0x31ULL, 0x31ULL};

    // Three profiles factored along ADR-0060 §3's authoring example.
    {
        // Rule 0: priority 10 — match os==Linux only; bundle = [platform_preset].
        crd::containers::Array<crd::profile::PredicateRecord> p(alloc);
        p.push_back(make_pred(crd::profile::PredicateField::Os,
                              crd::profile::PredicateOp::Equal,
                              static_cast<crd::u32>(crd::profile::OperatingSystem::Linux)));
        crd::containers::Array<crd::resources::ResourceId> b(alloc);
        b.push_back(platform_preset);
        builder.add_rule(10U,
                         crd::containers::ConstSpan<crd::profile::PredicateRecord>{p.data(), p.size()},
                         crd::containers::ConstSpan<crd::resources::ResourceId>{b.data(), b.size()});
    }
    {
        // Rule 1: priority 20 — match gpu_tier>=High; bundle = [tier_preset].
        crd::containers::Array<crd::profile::PredicateRecord> p(alloc);
        p.push_back(make_pred(crd::profile::PredicateField::GpuTier,
                              crd::profile::PredicateOp::GreaterEq,
                              static_cast<crd::u32>(crd::profile::GpuTier::High)));
        crd::containers::Array<crd::resources::ResourceId> b(alloc);
        b.push_back(tier_preset);
        builder.add_rule(20U,
                         crd::containers::ConstSpan<crd::profile::PredicateRecord>{p.data(), p.size()},
                         crd::containers::ConstSpan<crd::resources::ResourceId>{b.data(), b.size()});
    }
    {
        // Rule 2: priority 30 — match domain==Game; bundle = [domain_preset_a, domain_preset_b].
        crd::containers::Array<crd::profile::PredicateRecord> p(alloc);
        p.push_back(make_pred(crd::profile::PredicateField::Domain,
                              crd::profile::PredicateOp::Equal,
                              static_cast<crd::u32>(crd::profile::ProjectDomain::Game)));
        crd::containers::Array<crd::resources::ResourceId> b(alloc);
        b.push_back(domain_preset_a);
        b.push_back(domain_preset_b);
        builder.add_rule(30U,
                         crd::containers::ConstSpan<crd::profile::PredicateRecord>{p.data(), p.size()},
                         crd::containers::ConstSpan<crd::resources::ResourceId>{b.data(), b.size()});
    }

    auto bytes = builder.build();

    crd::profile::ProfileLoader loader{alloc};
    auto* payload = loader.load(make_ctx(
        id, crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()}, alloc));
    REQUIRE(payload != nullptr);
    auto* res = static_cast<crd::profile::ProfileResource*>(payload);

    crd::profile::ProfileResolver resolver{alloc};
    resolver.set_resource(res);

    crd::containers::Array<crd::resources::ResourceId> out(alloc);

    // Linux + Ultra + Game → all three rules match.
    {
        crd::profile::ProfileContext ctx{};
        ctx.os       = crd::profile::OperatingSystem::Linux;
        ctx.gpu_tier = crd::profile::GpuTier::Ultra;
        ctx.domain   = crd::profile::ProjectDomain::Game;
        const crd::u32 n = resolver.resolve(ctx, out);
        CHECK(n == 3U);
        REQUIRE(out.size() == 4U);
        // Priority-ascending: [10:platform, 20:tier, 30:domain_a, 30:domain_b]
        CHECK(out[0] == platform_preset);
        CHECK(out[1] == tier_preset);
        CHECK(out[2] == domain_preset_a);
        CHECK(out[3] == domain_preset_b);
    }

    // Linux + Ultra + Cinematic → rules 0 + 1 match (rule 2 fails on domain).
    {
        crd::profile::ProfileContext ctx{};
        ctx.os       = crd::profile::OperatingSystem::Linux;
        ctx.gpu_tier = crd::profile::GpuTier::Ultra;
        ctx.domain   = crd::profile::ProjectDomain::Cinematic;
        const crd::u32 n = resolver.resolve(ctx, out);
        CHECK(n == 2U);
        REQUIRE(out.size() == 2U);
        CHECK(out[0] == platform_preset);
        CHECK(out[1] == tier_preset);
    }

    // Windows + Low + Game → only rule 2 matches (os fails rule 0; tier fails rule 1).
    {
        crd::profile::ProfileContext ctx{};
        ctx.os       = crd::profile::OperatingSystem::Windows;
        ctx.gpu_tier = crd::profile::GpuTier::Low;
        ctx.domain   = crd::profile::ProjectDomain::Game;
        const crd::u32 n = resolver.resolve(ctx, out);
        CHECK(n == 1U);
        REQUIRE(out.size() == 2U);
        CHECK(out[0] == domain_preset_a);
        CHECK(out[1] == domain_preset_b);
    }

    loader.unload(payload);
}

TEST_CASE("ProfileResolver edge cases + context detection helpers",
          "[profile][resolver][edges][detect]")
{
    auto* alloc = crd::memory::default_allocator();

    // No resource → empty bundle, zero matches.
    {
        crd::profile::ProfileResolver resolver{alloc};
        REQUIRE(resolver.resource() == nullptr);
        crd::containers::Array<crd::resources::ResourceId> out(alloc);
        const crd::profile::ProfileContext ctx{};
        CHECK(resolver.resolve(ctx, out) == 0U);
        CHECK(out.empty());
    }

    // Empty profile resource → empty bundle, zero matches.
    {
        const crd::resources::ResourceId id{0x33ULL, 0x33ULL};
        crd::profile::ProfileArtifactBuilder builder{alloc, 1U, id};
        auto bytes = builder.build();

        crd::profile::ProfileLoader loader{alloc};
        auto* payload = loader.load(make_ctx(
            id, crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()}, alloc));
        REQUIRE(payload != nullptr);

        crd::profile::ProfileResolver resolver{alloc};
        resolver.set_resource(static_cast<crd::profile::ProfileResource*>(payload));

        crd::containers::Array<crd::resources::ResourceId> out(alloc);
        out.push_back(crd::resources::ResourceId{0x99ULL, 0x99ULL}); // resolve must clear
        CHECK(resolver.resolve(crd::profile::ProfileContext{}, out) == 0U);
        CHECK(out.empty());

        loader.unload(payload);
    }

    // Detection helpers — must produce sensible values on every supported runner.
    const auto os = crd::profile::detect_os();
    CHECK((os == crd::profile::OperatingSystem::Windows
        || os == crd::profile::OperatingSystem::Linux
        || os == crd::profile::OperatingSystem::MacOS));

    const auto cores = crd::profile::detect_cpu_cores();
    CHECK(cores >= 1);
}
