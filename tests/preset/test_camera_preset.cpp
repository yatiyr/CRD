// Phase 3.0 v1n3 — CameraPreset tests.
//
// Three test cases. The substrate is fully exercised by v1n1's
// test_preset_registry.cpp + v1n2's test_quality_preset.cpp; v1n3's surface
// is small enough that a focused trio is sufficient:
//
//   1. Default-constructed CameraPreset has the documented field defaults
//      and the schema FourCC / version / size / alignment match ADR-0059
//      §1 + §7.
//   2. Round-trip a fully-populated CameraPreset (non-default scalars +
//      both enum members) bit-exact through PresetArtifactBuilder +
//      PresetLoader.
//   3. IPresetTarget::apply(CameraPreset) dispatches to overrides; the
//      default-empty base body is observable on a non-overriding target.
//      Cross-test the QualityPreset overload to confirm the two are
//      independently dispatched (no overload-resolution surprises).

#include <crd/memory/allocator.hpp>
#include <crd/preset/camera_preset.hpp>
#include <crd/preset/preset_artifact_builder.hpp>
#include <crd/preset/preset_loader.hpp>
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

// Records both Camera and Quality apply calls so the dispatch test can
// confirm the overload set fires the right body for each input type.
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

class SilentTarget : public crd::preset::IPresetTarget {};

} // namespace

TEST_CASE("CameraPreset has the documented schema defaults and identity",
          "[preset][camera][schema]")
{
    crd::preset::CameraPreset p{};

    // Schema identity (ADR-0059 §1, §7).
    CHECK(crd::preset::CameraPreset::fourcc  == crd::resources::make_fourcc('P', 'R', 'C', 'M'));
    CHECK(crd::preset::CameraPreset::version == 1U);

    // Documented defaults.
    CHECK(p.fov_y_radians     == 1.0471975512F); // ≈ 60°
    CHECK(p.near_plane        == 0.1F);
    CHECK(p.far_plane         == 1000.0F);
    CHECK(p.aperture_f_stop   == 2.8F);
    CHECK(p.shutter_seconds   == 1.0F / 60.0F);
    CHECK(p.iso               == 100.0F);
    CHECK(p.exposure_comp_ev  == 0.0F);
    CHECK(p.ev100_min         == -8.0F);
    CHECK(p.ev100_max         == 16.0F);
    CHECK(p.lens_model        == crd::preset::LensModel::Perspective);
    CHECK(p.exposure_mode     == crd::preset::ExposureMode::Manual);

    // Binary-layout pin — adding fields would bump version=1.
    CHECK(sizeof(crd::preset::CameraPreset)  == 40U);
    CHECK(alignof(crd::preset::CameraPreset) == 4U);
}

TEST_CASE("CameraPreset round-trips bit-exact through artifact + loader",
          "[preset][camera][round-trip]")
{
    auto* alloc = crd::memory::default_allocator();
    const crd::resources::ResourceId id{0xFEED'FEEDULL, 0xBEEF'BEEFULL};

    // Populate every field with non-default values so memcmp is meaningful.
    crd::preset::CameraPreset src{};
    src.fov_y_radians      = 0.7853981634F;     // 45°
    src.near_plane         = 0.05F;
    src.far_plane          = 5000.0F;
    src.aperture_f_stop    = 1.4F;
    src.shutter_seconds    = 1.0F / 250.0F;
    src.iso                = 800.0F;
    src.exposure_comp_ev   = -1.5F;
    src.ev100_min          = -12.0F;
    src.ev100_max          = 18.0F;
    src.lens_model         = crd::preset::LensModel::Orthographic;
    src.exposure_mode      = crd::preset::ExposureMode::AutoEV100;

    crd::preset::PresetArtifactBuilder builder{
        alloc,
        crd::preset::CameraPreset::fourcc,
        crd::preset::CameraPreset::version,
        id};
    builder.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&src), sizeof(src)});
    auto bytes = builder.build();
    REQUIRE(!bytes.empty());

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
    CHECK(res->fourcc()         == crd::preset::CameraPreset::fourcc);
    CHECK(res->schema_version() == crd::preset::CameraPreset::version);
    REQUIRE(res->bytes().size() == sizeof(crd::preset::CameraPreset));

    crd::preset::CameraPreset rt{};
    std::memcpy(&rt, res->bytes().data(), sizeof(rt));

    CHECK(rt.fov_y_radians     == src.fov_y_radians);
    CHECK(rt.near_plane        == src.near_plane);
    CHECK(rt.far_plane         == src.far_plane);
    CHECK(rt.aperture_f_stop   == src.aperture_f_stop);
    CHECK(rt.shutter_seconds   == src.shutter_seconds);
    CHECK(rt.iso               == src.iso);
    CHECK(rt.exposure_comp_ev  == src.exposure_comp_ev);
    CHECK(rt.ev100_min         == src.ev100_min);
    CHECK(rt.ev100_max         == src.ev100_max);
    CHECK(rt.lens_model        == src.lens_model);
    CHECK(rt.exposure_mode     == src.exposure_mode);

    // Bit-exact (catches any silent padding / endianness drift).
    CHECK(std::memcmp(&rt, &src, sizeof(src)) == 0);

    loader.unload(payload);
}

TEST_CASE("IPresetTarget::apply(CameraPreset) dispatches independently of QualityPreset overload",
          "[preset][camera][apply][target]")
{
    crd::preset::CameraPreset cam{};
    cam.fov_y_radians = 1.5708F; // 90°
    cam.lens_model    = crd::preset::LensModel::Orthographic;

    crd::preset::QualityPreset qual{};
    qual.shadow_resolution = 4096U;

    DualRecordingTarget rec{};
    crd::preset::IPresetTarget& as_base = rec;

    // Independently dispatched overloads.
    as_base.apply(cam);
    as_base.apply(qual);
    as_base.apply(cam);

    CHECK(rec.camera_count()  == 2U);
    CHECK(rec.quality_count() == 1U);
    CHECK(rec.last_camera().lens_model        == crd::preset::LensModel::Orthographic);
    CHECK(rec.last_quality().shadow_resolution == 4096U);

    // Silent target — empty default bodies for both overloads compile + run.
    SilentTarget silent{};
    crd::preset::IPresetTarget& silent_base = silent;
    silent_base.apply(cam);
    silent_base.apply(qual);
    SUCCEED("Silent target ran apply(CameraPreset) + apply(QualityPreset) with no observable effect");
}
