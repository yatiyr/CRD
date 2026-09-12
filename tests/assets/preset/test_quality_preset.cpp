// Phase 3.0 v1n2 — QualityPreset tests.
//
// Four test cases exercising the first concrete preset type end-to-end on
// the substrate from v1n1:
//
//   1. Default-constructed QualityPreset has the documented field defaults
//      and the schema FourCC / version match ADR-0059 §1.
//   2. PresetRegistry::register_type<QualityPreset> mints a TypeInfo with
//      the canonical FourCC / size / alignment.
//   3. Round-trip a fully-populated QualityPreset (non-default fields +
//      post_fx ResourceIds) through PresetArtifactBuilder + PresetLoader;
//      every byte is preserved.
//   4. IPresetTarget::apply(QualityPreset) fires on a derived class that
//      overrides it; the empty-base default is observable on a non-overriding
//      target.

#include <crd/memory/allocator.hpp>
#include <crd/preset/preset_artifact_builder.hpp>
#include <crd/preset/preset_loader.hpp>
#include <crd/preset/preset_registry.hpp>
#include <crd/preset/preset_resource.hpp>
#include <crd/preset/preset_target.hpp>
#include <crd/preset/quality_preset.hpp>
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

// Recording target — observes how many times apply(QualityPreset) fires.
// `using IPresetTarget::apply;` imports the un-overridden CameraPreset
// overload so GCC's -Woverloaded-virtual doesn't flag the partial override
// (see preset_target.hpp's "Partial-override convention").
class RecordingTarget : public crd::preset::IPresetTarget
{
public:
    using crd::preset::IPresetTarget::apply;

    void apply(const crd::preset::QualityPreset& preset) override
    {
        ++m_apply_count;
        m_last = preset;
    }

    [[nodiscard]] crd::u32 apply_count() const noexcept { return m_apply_count; }
    [[nodiscard]] const crd::preset::QualityPreset& last() const noexcept { return m_last; }

private:
    crd::u32                     m_apply_count = 0U;
    crd::preset::QualityPreset   m_last{};
};

// Silent target — never overrides apply; verifies the default-empty base.
class SilentTarget : public crd::preset::IPresetTarget
{
public:
    SilentTarget() = default;
};

} // namespace

TEST_CASE("QualityPreset has the documented schema defaults and identity",
          "[preset][quality][schema]")
{
    crd::preset::QualityPreset p{};

    // Schema identity (ADR-0059 §1; v2 since v1o3 added enable_depth_prepass).
    CHECK(crd::preset::QualityPreset::fourcc  == crd::resources::make_fourcc('P', 'R', 'Q', 'L'));
    CHECK(crd::preset::QualityPreset::version == 2U);

    // Documented defaults.
    CHECK(p.shadow_resolution    == 2048U);
    CHECK(p.msaa_samples         == 4U);
    CHECK(p.ssr_quality          == 2U);
    CHECK(p.ssao_quality         == 2U);
    CHECK(p.post_fx_count        == 0U);
    CHECK(p.enable_depth_prepass == 1U);
    for (const auto& ref : p.post_fx)
    {
        CHECK(ref.is_null());
    }

    // Binary-layout pin — v2 keeps the v1 144-byte layout by repurposing
    // one byte of the original `_reserved[8]`.
    CHECK(sizeof(crd::preset::QualityPreset)  == 144U);
    CHECK(alignof(crd::preset::QualityPreset) == 8U);
}

TEST_CASE("PresetRegistry registers QualityPreset with canonical TypeInfo",
          "[preset][quality][registry]")
{
    auto* alloc = crd::memory::default_allocator();
    crd::preset::PresetRegistry reg{alloc};

    const auto& info = reg.register_type<crd::preset::QualityPreset>(
        crd::containers::StringView{"Quality"});

    CHECK(info.fourcc                == crd::preset::QualityPreset::fourcc);
    CHECK(info.latest_schema_version == crd::preset::QualityPreset::version);
    CHECK(info.size_bytes            == sizeof(crd::preset::QualityPreset));
    CHECK(info.alignment             == alignof(crd::preset::QualityPreset));
    CHECK(info.name                  == crd::containers::StringView{"Quality"});
    REQUIRE(info.loader              != nullptr);
    CHECK(info.loader->type_fourcc() == crd::preset::QualityPreset::fourcc);

    // Same lookup paths the cooker / consumer code will use.
    const auto* by_fourcc = reg.find(crd::preset::QualityPreset::fourcc);
    REQUIRE(by_fourcc != nullptr);
    CHECK(by_fourcc->fourcc == crd::preset::QualityPreset::fourcc);

    const auto* by_name = reg.find(crd::containers::StringView{"Quality"});
    REQUIRE(by_name != nullptr);
    CHECK(by_name == by_fourcc); // single backing record
}

TEST_CASE("QualityPreset round-trips bit-exact through artifact + loader",
          "[preset][quality][round-trip]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xCAFE'CAFEULL, 0x1234'5678ULL};

    // Populate every field with non-default values so a memcmp is meaningful.
    crd::preset::QualityPreset src{};
    src.shadow_resolution    = 4096U;
    src.msaa_samples         = 8U;
    src.ssr_quality          = 3U;
    src.ssao_quality         = 3U;
    src.post_fx_count        = 3U;
    src.enable_depth_prepass = 0U; // toggle off — covers the v2 field
    src.post_fx[0]        = crd::resources::ResourceId{0x1111'1111ULL, 0x2222'2222ULL};
    src.post_fx[1]        = crd::resources::ResourceId{0x3333'3333ULL, 0x4444'4444ULL};
    src.post_fx[2]        = crd::resources::ResourceId{0x5555'5555ULL, 0x6666'6666ULL};
    // Slots [3..7] left null on purpose — confirms the loader preserves zero
    // entries beyond post_fx_count.

    crd::preset::PresetArtifactBuilder builder{
        alloc,
        crd::preset::QualityPreset::fourcc,
        crd::preset::QualityPreset::version,
        id};
    builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&src), sizeof(src)});
    auto bytes = builder.build();
    REQUIRE(!bytes.empty());

    crd::preset::PresetLoader loader{
        crd::preset::QualityPreset::fourcc,
        crd::preset::QualityPreset::version,
        sizeof(crd::preset::QualityPreset),
        alloc};
    auto* payload = loader.load(make_ctx(
        id,
        crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()},
        alloc));
    REQUIRE(payload != nullptr);

    auto* res = static_cast<crd::preset::PresetResource*>(payload);
    CHECK(res->fourcc()         == crd::preset::QualityPreset::fourcc);
    CHECK(res->schema_version() == crd::preset::QualityPreset::version);
    REQUIRE(res->bytes().size() == sizeof(crd::preset::QualityPreset));

    crd::preset::QualityPreset round_tripped{};
    std::memcpy(&round_tripped, res->bytes().data(), sizeof(round_tripped));

    CHECK(round_tripped.shadow_resolution    == src.shadow_resolution);
    CHECK(round_tripped.msaa_samples         == src.msaa_samples);
    CHECK(round_tripped.ssr_quality          == src.ssr_quality);
    CHECK(round_tripped.ssao_quality         == src.ssao_quality);
    CHECK(round_tripped.post_fx_count        == src.post_fx_count);
    CHECK(round_tripped.enable_depth_prepass == src.enable_depth_prepass);
    for (crd::usize i = 0; i < 8U; ++i)
    {
        CHECK(round_tripped.post_fx[i] == src.post_fx[i]);
    }

    // Bit-exact (catches any silent padding / endianness drift).
    CHECK(std::memcmp(&round_tripped, &src, sizeof(src)) == 0);

    loader.unload(payload);
}

TEST_CASE("IPresetTarget::apply(QualityPreset) dispatches to overrides; default base is no-op",
          "[preset][quality][apply][target]")
{
    crd::preset::QualityPreset p{};
    p.shadow_resolution = 8192U;
    p.msaa_samples      = 8U;

    RecordingTarget rec{};
    crd::preset::IPresetTarget& as_base = rec;
    as_base.apply(p);

    REQUIRE(rec.apply_count() == 1U);
    CHECK(rec.last().shadow_resolution == 8192U);
    CHECK(rec.last().msaa_samples      == 8U);

    // Apply again with a different value — confirms each call updates state.
    p.shadow_resolution = 1024U;
    as_base.apply(p);
    CHECK(rec.apply_count()            == 2U);
    CHECK(rec.last().shadow_resolution == 1024U);

    // Silent target — observable: no crash, no side-effect, default body is empty.
    SilentTarget silent{};
    crd::preset::IPresetTarget& silent_base = silent;
    silent_base.apply(p); // no-op; verifying the default body compiles + runs cleanly
    silent_base.apply(p);
    SUCCEED("Silent target's apply(QualityPreset) ran twice with no observable effect");
}
