// lod_asset.cpp — REN-40-C1. See lod_asset.hpp for why the policy is a file.

#include <crd/lod/lod_asset.hpp>

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <utility>
#include <string_view>

namespace crd::lod
{
namespace
{
void set_where(crd::containers::String* where, std::string_view w)
{
    if (where == nullptr) { return; }
    where->clear();
    where->append(crd::containers::StringView(w.data(), w.size()));
}

void app(crd::containers::String& o, const char* s) { o.append(crd::containers::StringView(s)); }

void app_f32(crd::containers::String& o, crd::f32 v)
{
    char buf[32]{};
    // ⛔ Fixed precision, not "%g": the canonical form is what the identity hash
    // reads, so a value that prints differently on two platforms is a different
    // asset to the cache while being the same asset to the author.
    (void)std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(v));
    app(o, buf);
}

void hash_u64(crd::u64& h, crd::u64 v) noexcept
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
}

void hash_f32(crd::u64& h, crd::f32 v) noexcept
{
    // ⛔ the BIT PATTERN, not the value: two policies that differ in the last ulp
    // of a switch distance cook different chains, and a value-compare would collide
    // them onto one cache entry.
    crd::u32 bits = 0;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(static_cast<void*>(&bits), static_cast<const void*>(&v), sizeof(bits));
    hash_u64(h, bits);
}
} // namespace

const char* lod_cook_error_text(LodCookError err) noexcept
{
    switch (err)
    {
    case LodCookError::Ok: return "ok";
    case LodCookError::ParseFailed: return "the file is not valid TOML";
    case LodCookError::BadSchema: return "`schema` is missing or not 1";
    case LodCookError::NoLevels: return "the policy declares no `[[level]]` beyond the source mesh";
    case LodCookError::RatioOutOfRange: return "a level `ratio` is outside (0, 1) — it would not REDUCE anything";
    case LodCookError::RatiosNotDescending: return "level ratios must DESCEND (each level coarser than the last)";
    case LodCookError::ThresholdsNotDescending:
        return "level `screen_height` values must DESCEND, or selection is undefined";
    case LodCookError::TooManyLevels: return "more levels than `kMaxLodLevels`";
    }
    return "unknown";
}

LodCookError parse_lod_toml(crd::containers::StringView text, LodPolicy& out, crd::containers::String* where)
{
    out = LodPolicy{};
    toml::parse_result pr = toml::parse(std::string_view(text.data(), text.size()));
    if (!pr) { return LodCookError::ParseFailed; }
    toml::table root = std::move(pr).table();

    if (root["schema"].value_or<int64_t>(0) != 1) { return LodCookError::BadSchema; }
    out.boundary_weight = static_cast<crd::f32>(root["boundary_weight"].value_or<double>(1000.0));
    // ⛔ The two ACCEPTANCE tests a generated level has to pass (see LodPolicy): a triangle floor, and a shape
    // test. Both authored, because "still a surface" and "still the object" are properties of the content.
    out.min_triangles    = static_cast<crd::u32>(root["min_triangles"].value_or<int64_t>(64));
    out.min_extent_ratio = static_cast<crd::f32>(root["min_extent_ratio"].value_or<double>(0.5));
    out.min_area_ratio   = static_cast<crd::f32>(root["min_area_ratio"].value_or<double>(0.5));
    // ⭐⭐ REN-40-C4: the transition knobs (see LodPolicy). Clamped to sane ranges rather than trusted: a
    // NEGATIVE hysteresis would invert the band and make the boundary oscillate harder, which is the exact
    // artefact it exists to remove.
    out.hysteresis  = static_cast<crd::f32>(root["hysteresis"].value_or<double>(0.15));
    out.dither_band = static_cast<crd::f32>(root["dither_band"].value_or<double>(0.25));
    if (!(out.hysteresis >= 0.0F) || out.hysteresis > 4.0F) { out.hysteresis = 0.0F; }
    if (!(out.dither_band >= 0.0F) || out.dither_band > 1.0F) { out.dither_band = 0.0F; }
    // ⭐⭐ REN-40-C5: octahedral impostors. 0 = disabled (the parity arm). Grid clamped to [2, 16] when
    // non-zero: below 2 there are not enough views to reconstruct a direction, above 16 the atlas is 1024²
    // per tile (a 64-tile atlas at 64 px/tile is already 4096² and 64 MB — the next step is streaming, not a
    // bigger atlas). Tile clamped to [8, 128] — below 8 the coverage mask has no resolution, above 128 the
    // atlas exceeds what a sub-16 px instance can ever sample.
    out.impostor_grid = static_cast<crd::u32>(root["impostor_grid"].value_or<int64_t>(0));
    out.impostor_tile = static_cast<crd::u32>(root["impostor_tile"].value_or<int64_t>(64));
    if (out.impostor_grid != 0U)
    {
        if (out.impostor_grid < 2U) { out.impostor_grid = 2U; }
        if (out.impostor_grid > 16U) { out.impostor_grid = 16U; }
    }
    if (out.impostor_tile < 8U) { out.impostor_tile = 8U; }
    if (out.impostor_tile > 128U) { out.impostor_tile = 128U; }
    // ⭐⭐ REN-40-C3: the PER-VIEW BIAS (see LodPolicy) — `view_bias = [camera, cascade0, cascade1, ...]`.
    // ⛔ Every view defaults to 1.0, and a non-positive entry is REFUSED back to 1.0: a bias of 0 would drive the
    // projected height to zero and pin that whole view to the coarsest level — a shadow map that silently drew
    // impostors, which reads as broken shadows rather than as a policy typo.
    for (crd::u32 vb = 0; vb < kMaxLodLevels; ++vb) { out.view_bias[vb] = 1.0F; }
    if (const auto* vba = root["view_bias"].as_array())
    {
        crd::u32 vi = 0U;
        for (const auto& node : *vba)
        {
            if (vi >= kMaxLodLevels) { break; }
            const auto v      = static_cast<crd::f32>(node.value_or<double>(1.0));
            out.view_bias[vi] = v > 0.0F ? v : 1.0F;
            ++vi;
        }
    }

    const auto* levels = root["level"].as_array();
    if (levels == nullptr || levels->size() == 0U) { return LodCookError::NoLevels; }
    if (levels->size() > static_cast<crd::usize>(kMaxLodLevels - 1U)) { return LodCookError::TooManyLevels; }

    crd::u32 n = 0U;
    for (const auto& node : *levels)
    {
        const auto* t = node.as_table();
        if (t == nullptr) { continue; }
        const auto ratio  = static_cast<crd::f32>((*t)["ratio"].value_or<double>(0.0));
        const auto height = static_cast<crd::f32>((*t)["screen_height"].value_or<double>(0.0));
        char       buf[32]{};
        (void)std::snprintf(buf, sizeof(buf), "level %u", static_cast<unsigned>(n + 1U));
        if (!(ratio > 0.0F) || !(ratio < 1.0F))
        {
            set_where(where, buf);
            return LodCookError::RatioOutOfRange;
        }
        if (n > 0U && !(ratio < out.ratio[n - 1U]))
        {
            set_where(where, buf);
            return LodCookError::RatiosNotDescending;
        }
        out.ratio[n] = ratio;
        // ⛔ `screen_height[0]` belongs to LEVEL 0 (the source): it is the height
        // below which level 1 takes over. So level i's entry sits at index i, and
        // the array carries one more value than there are decimated levels.
        out.screen_height[n] = height;
        ++n;
    }
    if (n == 0U) { return LodCookError::NoLevels; }
    for (crd::u32 i = 1; i < n; ++i)
    {
        if (!(out.screen_height[i] < out.screen_height[i - 1U]))
        {
            char buf[32]{};
            (void)std::snprintf(buf, sizeof(buf), "level %u", static_cast<unsigned>(i + 1U));
            set_where(where, buf);
            return LodCookError::ThresholdsNotDescending;
        }
    }
    // the coarsest level stays selected all the way to zero height
    out.screen_height[n] = 0.0F;
    out.extra_levels     = n;
    return LodCookError::Ok;
}

void write_lod_toml(const LodPolicy& policy, crd::containers::String& out)
{
    out.clear();
    app(out, "schema = 1\nboundary_weight = ");
    app_f32(out, policy.boundary_weight);
    // ⛔ Canonical form carries it, or a saved policy silently reverts to the default — the field-survival scar:
    // a field dropped by BOTH the writer and the reader round-trips "byte-identically" and is quietly lost.
    app(out, "\nmin_triangles = ");
    app_f32(out, static_cast<crd::f32>(policy.min_triangles));
    app(out, "\nmin_extent_ratio = ");
    app_f32(out, policy.min_extent_ratio);
    app(out, "\nmin_area_ratio = ");
    app_f32(out, policy.min_area_ratio);
    app(out, "\nhysteresis = ");
    app_f32(out, policy.hysteresis);
    app(out, "\ndither_band = ");
    app_f32(out, policy.dither_band);
    app(out, "\nimpostor_grid = ");
    app_f32(out, static_cast<crd::f32>(policy.impostor_grid));
    app(out, "\nimpostor_tile = ");
    app_f32(out, static_cast<crd::f32>(policy.impostor_tile));
    app(out, "\nview_bias = [");
    for (crd::u32 vb = 0; vb < kMaxLodLevels; ++vb)
    {
        if (vb > 0U) { app(out, ", "); }
        app_f32(out, policy.view_bias[vb]);
    }
    app(out, "]");
    app(out, "\n");
    for (crd::u32 i = 0; i < policy.extra_levels; ++i)
    {
        app(out, "\n[[level]]\nratio = ");
        app_f32(out, policy.ratio[i]);
        app(out, "\nscreen_height = ");
        app_f32(out, policy.screen_height[i]);
        app(out, "\n");
    }
}

crd::u64 lod_policy_identity(const LodPolicy& policy) noexcept
{
    crd::u64 h = 0xCBF29CE484222325ULL;
    hash_u64(h, policy.extra_levels);
    hash_f32(h, policy.boundary_weight);
    // ⛔ It changes the COOKED CHAIN — how many levels exist at all — so it changes the identity.
    hash_u64(h, policy.min_triangles);
    hash_f32(h, policy.min_extent_ratio);
    hash_f32(h, policy.min_area_ratio);
    hash_f32(h, policy.hysteresis);
    hash_f32(h, policy.dither_band);
    hash_u64(h, policy.impostor_grid);
    hash_u64(h, policy.impostor_tile);
    // ⛔ It changes WHICH LEVEL each view selects, so two policies differing only here are different assets.
    for (crd::u32 vb = 0; vb < kMaxLodLevels; ++vb) { hash_f32(h, policy.view_bias[vb]); }
    for (crd::u32 i = 0; i < policy.extra_levels; ++i)
    {
        hash_f32(h, policy.ratio[i]);
        hash_f32(h, policy.screen_height[i]);
    }
    // ⛔ the LAST threshold too — it is the one that decides where the COARSEST
    // level takes over, and leaving it out collides two policies that differ only
    // in how far away the cheapest mesh starts being used.
    hash_f32(h, policy.screen_height[policy.extra_levels]);
    return h;
}

} // namespace crd::lod
