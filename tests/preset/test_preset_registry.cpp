// Phase 3.0 v1n1 — PresetRegistry + PresetLoader + PresetArtifactBuilder tests.
//
// Five test cases exercising the substrate end-to-end:
//   1. register_type<T>() returns a valid TypeInfo with the schema's FourCC,
//      version, size, alignment, and the user-supplied name.
//   2. Re-registering the same T is idempotent — same TypeInfo pointer; the
//      second call's name is ignored.
//   3. Round-trip: PresetArtifactBuilder → CRDR bytes → PresetLoader → a
//      PresetResource whose fourcc/version/bytes/chain match the input.
//   4. Mismatched FourCC: a loader configured for one type rejects bytes
//      built for a different type (returns nullptr).
//   5. PCHN round-trip: multiple chain entries are preserved bit-exactly
//      through the build/load cycle.

#include <crd/memory/allocator.hpp>
#include <crd/preset/preset_artifact_builder.hpp>
#include <crd/preset/preset_loader.hpp>
#include <crd/preset/preset_registry.hpp>
#include <crd/preset/preset_resource.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
// Two distinct fake schema types for these tests; concrete preset types
// (QualityPreset, CameraPreset) ship in v1n2 / v1n3.
struct AlphaSchema
{
    // `fourcc` and `version` names are part of the PresetRegistry contract
    // (see engine/preset/include/crd/preset/preset_registry.hpp): the
    // template requires `T::fourcc` / `T::version` static constexpr members.
    static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P', 'R', 'A', 'L'); // NOLINT(readability-identifier-naming)
    static constexpr crd::u32 version = 1U;                                                // NOLINT(readability-identifier-naming)

    crd::u32 a = 11U;
    crd::u32 b = 22U;
    crd::f32 c = 0.5F;
};

struct BetaSchema
{
    // Contract-mandated identifier names — see AlphaSchema comment.
    static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P', 'R', 'B', 'T'); // NOLINT(readability-identifier-naming)
    static constexpr crd::u32 version = 2U;                                                // NOLINT(readability-identifier-naming)

    crd::u32 x = 7U;
    crd::u32 y = 9U;
};

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

TEST_CASE("PresetRegistry::register_type<T> populates TypeInfo and is queryable",
          "[preset][registry]")
{
    auto* alloc = crd::memory::default_allocator();
    crd::preset::PresetRegistry reg{alloc};

    const auto& info = reg.register_type<AlphaSchema>(crd::containers::StringView{"Alpha"});

    CHECK(info.fourcc                 == AlphaSchema::fourcc);
    CHECK(info.latest_schema_version  == AlphaSchema::version);
    CHECK(info.size_bytes             == sizeof(AlphaSchema));
    CHECK(info.alignment              == alignof(AlphaSchema));
    CHECK(info.name                   == crd::containers::StringView{"Alpha"});
    REQUIRE(info.loader               != nullptr);
    CHECK(info.loader->type_fourcc()  == AlphaSchema::fourcc);

    const auto* by_fourcc = reg.find(AlphaSchema::fourcc);
    REQUIRE(by_fourcc != nullptr);
    CHECK(by_fourcc->fourcc == AlphaSchema::fourcc);

    const auto* by_name = reg.find(crd::containers::StringView{"Alpha"});
    REQUIRE(by_name != nullptr);
    CHECK(by_name->fourcc == AlphaSchema::fourcc);

    CHECK(reg.find(0xDEADBEEFU) == nullptr);
    CHECK(reg.find(crd::containers::StringView{"Bogus"}) == nullptr);
    CHECK(reg.size() == 1U);
}

TEST_CASE("PresetRegistry::register_type<T> is idempotent on re-registration",
          "[preset][registry]")
{
    auto* alloc = crd::memory::default_allocator();
    crd::preset::PresetRegistry reg{alloc};

    const auto& first  = reg.register_type<AlphaSchema>(crd::containers::StringView{"Alpha"});
    const auto& second = reg.register_type<AlphaSchema>(crd::containers::StringView{"Alpha-renamed"});

    // Same TypeInfo address — backing storage is the registry's owned array.
    CHECK(&first == &second);
    // Original name preserved (re-registration's name is ignored).
    CHECK(second.name == crd::containers::StringView{"Alpha"});
    CHECK(reg.size()  == 1U);

    // Registering a *different* type adds a second entry.
    const auto& beta = reg.register_type<BetaSchema>(crd::containers::StringView{"Beta"});
    CHECK(beta.fourcc == BetaSchema::fourcc);
    CHECK(reg.size()  == 2U);
}

TEST_CASE("PresetArtifactBuilder + PresetLoader round-trip schema bytes",
          "[preset][loader][round-trip]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xCAFEBABEULL, 0x1234ULL};

    AlphaSchema source{};
    source.a = 0x11111111U;
    source.b = 0x22222222U;
    source.c = 3.14F;

    crd::preset::PresetArtifactBuilder builder{alloc, AlphaSchema::fourcc, AlphaSchema::version, id};
    builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&source), sizeof(source)});
    auto bytes = builder.build();
    REQUIRE(!bytes.empty());

    crd::preset::PresetLoader loader{AlphaSchema::fourcc, AlphaSchema::version, sizeof(AlphaSchema), alloc};
    auto* payload = loader.load(make_ctx(
        id,
        crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()},
        alloc));
    REQUIRE(payload != nullptr);

    auto* res = static_cast<crd::preset::PresetResource*>(payload);
    CHECK(res->fourcc()         == AlphaSchema::fourcc);
    CHECK(res->schema_version() == AlphaSchema::version);
    REQUIRE(res->bytes().size() == sizeof(AlphaSchema));

    AlphaSchema parsed{};
    std::memcpy(&parsed, res->bytes().data(), sizeof(AlphaSchema));
    CHECK(parsed.a == source.a);
    CHECK(parsed.b == source.b);
    CHECK(parsed.c == source.c);
    CHECK(res->chain_dependencies().empty());

    loader.unload(payload);
}

TEST_CASE("PresetLoader rejects mismatched FourCC artifacts",
          "[preset][loader]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{1ULL, 2ULL};

    BetaSchema beta{};
    crd::preset::PresetArtifactBuilder builder{alloc, BetaSchema::fourcc, BetaSchema::version, id};
    builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&beta), sizeof(beta)});
    auto bytes = builder.build();

    // Loader configured for AlphaSchema; artifact carries BetaSchema. Reject.
    crd::preset::PresetLoader alpha_loader{AlphaSchema::fourcc, AlphaSchema::version, sizeof(AlphaSchema), alloc};
    auto* payload = alpha_loader.load(make_ctx(
        id,
        crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()},
        alloc));
    CHECK(payload == nullptr);

    // The matching loader accepts it.
    crd::preset::PresetLoader beta_loader{BetaSchema::fourcc, BetaSchema::version, sizeof(BetaSchema), alloc};
    auto* good = beta_loader.load(make_ctx(
        id,
        crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()},
        alloc));
    REQUIRE(good != nullptr);
    beta_loader.unload(good);
}

TEST_CASE("PCHN chain dependencies round-trip through the artifact",
          "[preset][loader][pchn]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{42ULL, 99ULL};

    AlphaSchema schema{};
    crd::preset::PresetArtifactBuilder builder{alloc, AlphaSchema::fourcc, AlphaSchema::version, id};
    builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&schema), sizeof(schema)});

    builder.add_chain_dependency(0xAAAA1111ULL, 0xCCCC0001ULL);
    builder.add_chain_dependency(0xBBBB2222ULL, 0xCCCC0002ULL);
    builder.add_chain_dependency(0xDDDD3333ULL, 0xCCCC0003ULL);

    auto bytes = builder.build();

    crd::preset::PresetLoader loader{AlphaSchema::fourcc, AlphaSchema::version, sizeof(AlphaSchema), alloc};
    auto* payload = loader.load(make_ctx(
        id,
        crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()},
        alloc));
    REQUIRE(payload != nullptr);

    auto* res = static_cast<crd::preset::PresetResource*>(payload);
    const auto chain = res->chain_dependencies();
    REQUIRE(chain.size() == 3U);
    CHECK(chain[0].path_hash    == 0xAAAA1111ULL);
    CHECK(chain[0].content_hash == 0xCCCC0001ULL);
    CHECK(chain[1].path_hash    == 0xBBBB2222ULL);
    CHECK(chain[1].content_hash == 0xCCCC0002ULL);
    CHECK(chain[2].path_hash    == 0xDDDD3333ULL);
    CHECK(chain[2].content_hash == 0xCCCC0003ULL);

    loader.unload(payload);
}
