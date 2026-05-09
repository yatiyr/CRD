// Phase 3.0 v1n4 — preset resolver tests (ADR-0059 §2 five-layer stack).
//
// Four test cases. The substrate (v1n1) and concrete types (v1n2 +
// v1n3) are already exercised; v1n4 only ships the resolver primitive
// `resolve_preset<T>` and the convenience `apply_preset<T>(target, ...)`.
// Tests cover:
//
//   1. resolve_preset<T>(nullptr)         → schema defaults (L0).
//   2. resolve_preset<T>(&res)            → resource bytes (L1+L2 via cook).
//   3. resolve_preset<T>(&res, &override) → runtime override wins (L4).
//   4. apply_preset<T>(target, ...) dispatches to the correct
//      IPresetTarget::apply() overload — both Quality and Camera variants
//      share the same resolver and reach the right body via the overload set.

#include <crd/memory/allocator.hpp>
#include <crd/preset/camera_preset.hpp>
#include <crd/preset/preset_artifact_builder.hpp>
#include <crd/preset/preset_loader.hpp>
#include <crd/preset/preset_resolver.hpp>
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

// Recording target for dispatch verification (see preset_target.hpp's
// "Partial-override convention" — this overrides BOTH overloads so no
// `using` declaration is required).
class DualRecordingTarget : public crd::preset::IPresetTarget
{
public:
    void apply(const crd::preset::QualityPreset& q) override
    {
        ++m_quality_count;
        m_last_quality = q;
    }
    void apply(const crd::preset::CameraPreset& c) override
    {
        ++m_camera_count;
        m_last_camera = c;
    }

    [[nodiscard]] crd::u32 quality_count() const noexcept { return m_quality_count; }
    [[nodiscard]] crd::u32 camera_count()  const noexcept { return m_camera_count; }
    [[nodiscard]] const crd::preset::QualityPreset& last_quality() const noexcept { return m_last_quality; }
    [[nodiscard]] const crd::preset::CameraPreset&  last_camera()  const noexcept { return m_last_camera; }

private:
    crd::u32                    m_quality_count = 0U;
    crd::u32                    m_camera_count  = 0U;
    crd::preset::QualityPreset  m_last_quality{};
    crd::preset::CameraPreset   m_last_camera{};
};

} // namespace

TEST_CASE("resolve_preset returns schema defaults when no resource is supplied",
          "[preset][resolver][L0]")
{
    // L0 only — the resolver should produce `T{}`.
    const auto q = crd::preset::resolve_preset<crd::preset::QualityPreset>(nullptr);
    const auto c = crd::preset::resolve_preset<crd::preset::CameraPreset>(nullptr);

    CHECK(q.shadow_resolution == 2048U);
    CHECK(q.msaa_samples      == 4U);
    CHECK(q.ssr_quality       == 2U);
    CHECK(q.ssao_quality      == 2U);
    CHECK(q.post_fx_count     == 0U);

    CHECK(c.fov_y_radians  == 1.0471975512F);
    CHECK(c.near_plane     == 0.1F);
    CHECK(c.far_plane      == 1000.0F);
    CHECK(c.lens_model     == crd::preset::LensModel::Perspective);
    CHECK(c.exposure_mode  == crd::preset::ExposureMode::Manual);
}

TEST_CASE("resolve_preset reads bytes from the loaded PresetResource (L1+L2)",
          "[preset][resolver][L2]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xAAAA'AAAAULL, 0xBBBB'BBBBULL};

    crd::preset::QualityPreset src{};
    src.shadow_resolution = 8192U;
    src.msaa_samples      = 8U;
    src.ssr_quality       = 3U;
    src.ssao_quality      = 3U;

    crd::preset::PresetArtifactBuilder builder{
        alloc, crd::preset::QualityPreset::fourcc, crd::preset::QualityPreset::version, id};
    builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&src), sizeof(src)});
    auto bytes = builder.build();

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

    const auto resolved = crd::preset::resolve_preset<crd::preset::QualityPreset>(res);

    // Resource values win over schema defaults.
    CHECK(resolved.shadow_resolution == 8192U);
    CHECK(resolved.msaa_samples      == 8U);
    CHECK(resolved.ssr_quality       == 3U);
    CHECK(resolved.ssao_quality      == 3U);

    // Bit-exact through the resolver.
    CHECK(std::memcmp(&resolved, &src, sizeof(src)) == 0);

    loader.unload(payload);
}

TEST_CASE("resolve_preset honours runtime override (L4 wins over L2)",
          "[preset][resolver][L4]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xCCCC'CCCCULL, 0xDDDD'DDDDULL};

    // Cooked resource carries L2 bytes.
    crd::preset::CameraPreset cooked_value{};
    cooked_value.fov_y_radians   = 1.0471975512F; // 60°
    cooked_value.aperture_f_stop = 4.0F;

    crd::preset::PresetArtifactBuilder builder{
        alloc, crd::preset::CameraPreset::fourcc, crd::preset::CameraPreset::version, id};
    builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&cooked_value), sizeof(cooked_value)});
    auto bytes = builder.build();

    crd::preset::PresetLoader loader{
        crd::preset::CameraPreset::fourcc,
        crd::preset::CameraPreset::version,
        sizeof(crd::preset::CameraPreset),
        alloc};
    auto* payload = loader.load(make_ctx(
        id,
        crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()},
        alloc));
    REQUIRE(payload != nullptr);
    auto* res = static_cast<crd::preset::PresetResource*>(payload);

    // Runtime override (L4): caller-supplied full T (e.g. ImGui slider state).
    crd::preset::CameraPreset runtime{};
    runtime.fov_y_radians   = 1.5708F;     // 90°
    runtime.aperture_f_stop = 1.4F;
    runtime.lens_model      = crd::preset::LensModel::Orthographic;

    const auto resolved = crd::preset::resolve_preset<crd::preset::CameraPreset>(res, &runtime);

    // L4 wins entirely — the override replaces the L2 value.
    CHECK(resolved.fov_y_radians   == runtime.fov_y_radians);
    CHECK(resolved.aperture_f_stop == runtime.aperture_f_stop);
    CHECK(resolved.lens_model      == crd::preset::LensModel::Orthographic);

    // Sanity: without the override, the resource value comes through.
    const auto no_override = crd::preset::resolve_preset<crd::preset::CameraPreset>(res);
    CHECK(no_override.fov_y_radians   == cooked_value.fov_y_radians);
    CHECK(no_override.aperture_f_stop == cooked_value.aperture_f_stop);
    CHECK(no_override.lens_model      == crd::preset::LensModel::Perspective);

    loader.unload(payload);
}

TEST_CASE("apply_preset dispatches to the correct IPresetTarget overload",
          "[preset][resolver][apply][dispatch]")
{
    auto* alloc = crd::memory::default_allocator();

    // Cook a QualityPreset.
    crd::preset::QualityPreset q_src{};
    q_src.shadow_resolution = 4096U;
    crd::preset::PresetArtifactBuilder q_builder{
        alloc, crd::preset::QualityPreset::fourcc, crd::preset::QualityPreset::version,
        crd::resources::ResourceId{0x10ULL, 0x10ULL}};
    q_builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&q_src), sizeof(q_src)});
    auto q_bytes = q_builder.build();
    crd::preset::PresetLoader q_loader{
        crd::preset::QualityPreset::fourcc, crd::preset::QualityPreset::version,
        sizeof(crd::preset::QualityPreset), alloc};
    auto* q_payload = q_loader.load(make_ctx(
        crd::resources::ResourceId{0x10ULL, 0x10ULL},
        crd::containers::ConstSpan<crd::u8>{q_bytes.data(), q_bytes.size()},
        alloc));
    REQUIRE(q_payload != nullptr);
    auto* q_res = static_cast<crd::preset::PresetResource*>(q_payload);

    // Cook a CameraPreset.
    crd::preset::CameraPreset c_src{};
    c_src.fov_y_radians = 0.5F;
    crd::preset::PresetArtifactBuilder c_builder{
        alloc, crd::preset::CameraPreset::fourcc, crd::preset::CameraPreset::version,
        crd::resources::ResourceId{0x20ULL, 0x20ULL}};
    c_builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&c_src), sizeof(c_src)});
    auto c_bytes = c_builder.build();
    crd::preset::PresetLoader c_loader{
        crd::preset::CameraPreset::fourcc, crd::preset::CameraPreset::version,
        sizeof(crd::preset::CameraPreset), alloc};
    auto* c_payload = c_loader.load(make_ctx(
        crd::resources::ResourceId{0x20ULL, 0x20ULL},
        crd::containers::ConstSpan<crd::u8>{c_bytes.data(), c_bytes.size()},
        alloc));
    REQUIRE(c_payload != nullptr);
    auto* c_res = static_cast<crd::preset::PresetResource*>(c_payload);

    DualRecordingTarget target{};

    // No-resource path: applies schema defaults.
    crd::preset::apply_preset<crd::preset::QualityPreset>(target, nullptr);
    REQUIRE(target.quality_count() == 1U);
    CHECK(target.last_quality().shadow_resolution == 2048U);

    // Resource path: applies cooked bytes.
    crd::preset::apply_preset<crd::preset::QualityPreset>(target, q_res);
    REQUIRE(target.quality_count() == 2U);
    CHECK(target.last_quality().shadow_resolution == 4096U);

    // Camera dispatch is independent.
    crd::preset::apply_preset<crd::preset::CameraPreset>(target, c_res);
    REQUIRE(target.camera_count() == 1U);
    CHECK(target.last_camera().fov_y_radians == 0.5F);

    // Runtime override dispatch.
    crd::preset::CameraPreset c_override{};
    c_override.fov_y_radians = 1.0F;
    crd::preset::apply_preset<crd::preset::CameraPreset>(target, c_res, &c_override);
    CHECK(target.camera_count() == 2U);
    CHECK(target.last_camera().fov_y_radians == 1.0F);

    q_loader.unload(q_payload);
    c_loader.unload(c_payload);
}
