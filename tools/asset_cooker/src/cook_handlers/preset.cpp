// Phase 3.0 v1o3 — `.preset.toml` → PRES cooker handler (ADR-0059).
//
// Reads a TOML file declaring a preset type ("Quality" / "Camera") plus
// per-field key/value entries; emits a PINF/PDAT[/PCHN] CRDR blob via
// `PresetArtifactBuilder`. The on-disk binary layout is the schema's
// in-memory layout (memcpy-clean) — so this handler's job is just
// "decode TOML → build a `T{}` populated struct → memcpy bytes into
// the builder."
//
// Format example:
//
//   type = "Quality"
//   shadow_resolution = 4096
//   msaa_samples = 4
//   ssr_quality = 2
//   ssao_quality = 2
//   post_fx_count = 0
//   enable_depth_prepass = 1
//
// Adding a new preset type means adding a new dispatch case here and
// authoring the TOML reader. Closed-by-types grammar (ADR-0059 §3) —
// content can't introduce a type we don't have a writer for.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/preset/camera_preset.hpp>
#include <crd/preset/preset_artifact_builder.hpp>
#include <crd/preset/quality_preset.hpp>

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace fs = crd::platform::fs;

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kPresetHandlerVersion = 1U;

// Helper: read an optional u32 from TOML root by key; clamp to [0, max].
[[nodiscard]] crd::u32 read_u32_or(const toml::table& root, std::string_view key,
                                   crd::u32 fallback, crd::u32 max_inclusive = 0xFFFF'FFFFU)
{
    const toml::node* n = root.get(key);
    if (n == nullptr)
        return fallback;
    if (auto v = n->value<int64_t>(); v.has_value())
    {
        const auto raw = *v;
        if (raw < 0)
            return 0U;
        const auto u = static_cast<crd::u64>(raw);
        return u > max_inclusive ? max_inclusive : static_cast<crd::u32>(u);
    }
    return fallback;
}

[[nodiscard]] crd::u8 read_u8_or(const toml::table& root, std::string_view key, crd::u8 fallback)
{
    return static_cast<crd::u8>(read_u32_or(root, key, fallback, 0xFFU));
}

[[nodiscard]] float read_f32_or(const toml::table& root, std::string_view key, float fallback)
{
    const toml::node* n = root.get(key);
    if (n == nullptr)
        return fallback;
    if (auto v = n->value<double>(); v.has_value())
        return static_cast<float>(*v);
    if (auto v = n->value<int64_t>(); v.has_value())
        return static_cast<float>(*v);
    return fallback;
}

// Quality writer — fills a QualityPreset, emits its bytes.
[[nodiscard]] crd::containers::Array<crd::u8>
emit_quality(const toml::table& root, const CookContext& ctx)
{
    crd::preset::QualityPreset q{};
    q.shadow_resolution    = read_u32_or(root, "shadow_resolution",    q.shadow_resolution);
    q.msaa_samples         = read_u8_or (root, "msaa_samples",         q.msaa_samples);
    q.ssr_quality          = read_u8_or (root, "ssr_quality",          q.ssr_quality);
    q.ssao_quality         = read_u8_or (root, "ssao_quality",         q.ssao_quality);
    q.post_fx_count        = read_u8_or (root, "post_fx_count",        q.post_fx_count);
    q.enable_depth_prepass = read_u8_or (root, "enable_depth_prepass", q.enable_depth_prepass);

    crd::preset::PresetArtifactBuilder b{
        ctx.allocator,
        crd::preset::QualityPreset::fourcc,
        crd::preset::QualityPreset::version,
        ctx.id};
    b.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&q), sizeof(q)});
    return b.build();
}

// Camera writer — fills a CameraPreset, emits its bytes.
[[nodiscard]] crd::containers::Array<crd::u8>
emit_camera(const toml::table& root, const CookContext& ctx)
{
    crd::preset::CameraPreset c{};
    c.fov_y_radians     = read_f32_or(root, "fov_y_radians",     c.fov_y_radians);
    c.near_plane        = read_f32_or(root, "near_plane",        c.near_plane);
    c.far_plane         = read_f32_or(root, "far_plane",         c.far_plane);
    c.aperture_f_stop   = read_f32_or(root, "aperture_f_stop",   c.aperture_f_stop);
    c.shutter_seconds   = read_f32_or(root, "shutter_seconds",   c.shutter_seconds);
    c.iso               = read_f32_or(root, "iso",               c.iso);
    c.exposure_comp_ev  = read_f32_or(root, "exposure_comp_ev",  c.exposure_comp_ev);
    c.ev100_min         = read_f32_or(root, "ev100_min",         c.ev100_min);
    c.ev100_max         = read_f32_or(root, "ev100_max",         c.ev100_max);

    if (const toml::node* n = root.get("lens_model"); n != nullptr)
    {
        if (auto s = n->value<std::string>(); s.has_value())
        {
            const auto& v = *s;
            if (v == "Perspective")       c.lens_model = crd::preset::LensModel::Perspective;
            else if (v == "Orthographic") c.lens_model = crd::preset::LensModel::Orthographic;
        }
    }
    if (const toml::node* n = root.get("exposure_mode"); n != nullptr)
    {
        if (auto s = n->value<std::string>(); s.has_value())
        {
            const auto& v = *s;
            if (v == "Manual")         c.exposure_mode = crd::preset::ExposureMode::Manual;
            else if (v == "AutoEV100") c.exposure_mode = crd::preset::ExposureMode::AutoEV100;
        }
    }

    crd::preset::PresetArtifactBuilder b{
        ctx.allocator,
        crd::preset::CameraPreset::fourcc,
        crd::preset::CameraPreset::version,
        ctx.id};
    b.set_payload(crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&c), sizeof(c)});
    return b.build();
}

CookResult preset_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::String text(ctx.allocator);
    if (!fs::read_file_text(fs::Path(ctx.source_path), text))
        return result;

    const auto parsed = toml::parse(std::string_view{text.data(), text.size()});
    if (!parsed)
    {
        std::fprintf(stderr, "preset cook: TOML parse error in %.*s\n",
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        return result;
    }

    const toml::table& root = parsed.table();

    const toml::node* type_node = root.get("type");
    if (type_node == nullptr)
    {
        std::fprintf(stderr, "preset cook: %.*s missing required `type` (string) key\n",
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        return result;
    }
    auto type_str = type_node->value<std::string>();
    if (!type_str.has_value())
    {
        std::fprintf(stderr, "preset cook: %.*s `type` must be a string\n",
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        return result;
    }

    crd::containers::Array<crd::u8> bytes(ctx.allocator);
    crd::u32                        fourcc = 0U;
    if (*type_str == "Quality")
    {
        bytes  = emit_quality(root, ctx);
        fourcc = crd::preset::QualityPreset::fourcc;
    }
    else if (*type_str == "Camera")
    {
        bytes  = emit_camera(root, ctx);
        fourcc = crd::preset::CameraPreset::fourcc;
    }
    else
    {
        std::fprintf(stderr,
                     "preset cook: %.*s unknown type='%s' (supported: Quality, Camera)\n",
                     static_cast<int>(ctx.source_path.size()),
                     ctx.source_path.data(), type_str->c_str());
        return result;
    }

    if (bytes.empty())
        return result;

    result.type_fourcc     = fourcc;
    result.cooked_bytes    = std::move(bytes);
    result.handler_version = kPresetHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

void register_preset_handler()
{
    register_cook_handler(".preset.toml", preset_handler);
}

} // namespace crd::cooker
