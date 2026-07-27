// lighting_asset.cpp — REN-38-E1..E6: parse, validate, emit and COOK the lighting vocabulary.
//
// ⛔⛔ 1100 lines of gold-standard shading in `ckir_lighting.hpp` were UNREACHABLE because the technique ABI
// carried exactly one directional light. This file is the vocabulary that reaches them.

#include <crd/lightcook/lighting_asset.hpp>

#include <crd/kir/ckir_lighting.hpp>
#include <crd/kir/ckir_nodes.hpp>
#include <crd/kir/ckir_shape.hpp> // REN-38 audit: the cook refuses a shape-invalid graph by name

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace crd::lightcook
{
namespace
{
using crd::kir::DType;
using crd::kir::KGraph;
using crd::kir::KOp;
namespace lt = crd::kir::lighting;
namespace nd = crd::kir::nodes;

void set_str(crd::containers::String& d, std::string_view v)
{
    d.clear();
    for (char c : v)
    {
        const char one[2] = {c, 0};
        d.append(static_cast<const char*>(one));
    }
}
void set_where(crd::containers::String* w, std::string_view v)
{
    if (w != nullptr) { set_str(*w, v); }
}

// The pull helper, identical in spirit to the vertex program's. ⛔ A float in the storage buffer is a BIT
// REINTERPRETATION of the word, never a numeric cast — a cast would read the bit pattern as a number and produce
// lights that are enormous or zero, never subtly wrong.
struct Lx
{
    KGraph&         g;
    crd::kir::Shape sh;

    explicit Lx(KGraph& graph) : g(graph), sh(crd::kir::make_shape({1})) {}

    [[nodiscard]] int kf(double v) { return g.constant(v, sh, DType::F32); }
    [[nodiscard]] int ku(crd::u32 v) { return g.constant(static_cast<double>(v), sh, DType::U32); }
    [[nodiscard]] int add(int a, int b) { return g.binary(KOp::Add, a, b); }
    [[nodiscard]] int sub(int a, int b) { return g.binary(KOp::Sub, a, b); }
    [[nodiscard]] int mul(int a, int b) { return g.binary(KOp::Mul, a, b); }
    [[nodiscard]] int dvd(int a, int b) { return g.binary(KOp::Div, a, b); }
    [[nodiscard]] int mxf(int a, int b) { return g.binary(KOp::Max, a, b); }
    [[nodiscard]] int loadu(int idx) { return g.storage_load(idx); }
    [[nodiscard]] int loadf(int idx) { return g.int_bits_to_float(g.cast(loadu(idx), DType::I32)); }
    [[nodiscard]] int hdru(crd::u32 word) { return loadu(ku(word)); }
    [[nodiscard]] int hdrf(crd::u32 word) { return loadf(ku(word)); }
    [[nodiscard]] int f_at(int base, crd::u32 off) { return loadf(add(base, ku(off))); }
    [[nodiscard]] int v3_at(int base, crd::u32 off)
    {
        return g.vec3(f_at(base, off), f_at(base, off + 1U), f_at(base, off + 2U));
    }
    // out = M·(x,y,z,1) from `count` header/record words at `base` (column-major).
    [[nodiscard]] int mat4_vec(int base, crd::u32 off, int p3, int out[4])
    {
        const int v[4] = {g.swizzle(p3, 0), g.swizzle(p3, 1), g.swizzle(p3, 2), kf(1.0)};
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            int acc = mul(f_at(base, off + 0U * 4U + i), v[0]);
            acc     = add(acc, mul(f_at(base, off + 1U * 4U + i), v[1]));
            acc     = add(acc, mul(f_at(base, off + 2U * 4U + i), v[2]));
            acc     = add(acc, mul(f_at(base, off + 3U * 4U + i), v[3]));
            out[i]  = acc;
        }
        return out[0];
    }
};

[[nodiscard]] const char* light_type_name(LightType t) noexcept
{
    switch (t)
    {
    case LightType::Directional: return "directional";
    case LightType::Point:       return "point";
    case LightType::Spot:        return "spot";
    case LightType::Rect:        return "rect";
    case LightType::Tube:        return "tube";
    case LightType::Disk:        return "disk";
    case LightType::Count:       break;
    }
    return "?";
}
[[nodiscard]] const char* shadow_mode_name(ShadowMode m) noexcept
{
    switch (m)
    {
    case ShadowMode::Csm:  return "csm";
    case ShadowMode::Map:  return "map";
    case ShadowMode::Cube: return "cube";
    case ShadowMode::None: break;
    }
    return "none";
}
[[nodiscard]] ShadowMode shadow_mode_of(std::string_view s) noexcept
{
    if (s == "csm") { return ShadowMode::Csm; }
    if (s == "map") { return ShadowMode::Map; }
    if (s == "cube") { return ShadowMode::Cube; }
    return ShadowMode::None;
}
[[nodiscard]] const char* shadow_filter_name(ShadowFilter f) noexcept
{
    switch (f)
    {
    case ShadowFilter::Pcf:  return "pcf";
    case ShadowFilter::Pcss: return "pcss";
    case ShadowFilter::Evsm: return "evsm";
    case ShadowFilter::Msm:  return "msm";
    case ShadowFilter::Hard: break;
    }
    return "hard";
}
[[nodiscard]] ShadowFilter shadow_filter_of(std::string_view s) noexcept
{
    if (s == "pcf") { return ShadowFilter::Pcf; }
    if (s == "pcss") { return ShadowFilter::Pcss; }
    if (s == "evsm") { return ShadowFilter::Evsm; }
    if (s == "msm") { return ShadowFilter::Msm; }
    return ShadowFilter::Hard;
}
} // namespace

// ── QUERIES ───────────────────────────────────────────────────────────────────────────────────────────────
crd::u32 lighting_total_lights(const LightingDesc& d) noexcept
{
    crd::u32 n = 0U;
    for (crd::u32 i = 0; i < kLightTypeCount; ++i) { n += d.set.count[i]; }
    return n;
}
crd::u32 lighting_type_first(const LightingDesc& d, LightType t) noexcept
{
    crd::u32 n = 0U;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(t) && i < kLightTypeCount; ++i) { n += d.set.count[i]; }
    return n;
}
bool lighting_needs_shadow_atlas(const LightingDesc& d) noexcept
{
    for (crd::u32 i = 0; i < kLightTypeCount; ++i)
    {
        if (d.set.count[i] > 0U && d.shadow.mode[i] != ShadowMode::None) { return true; }
    }
    return false;
}
bool lighting_needs_csm(const LightingDesc& d) noexcept
{
    return d.set.count[static_cast<crd::u32>(LightType::Directional)] > 0U
           && d.shadow.mode[static_cast<crd::u32>(LightType::Directional)] == ShadowMode::Csm;
}
bool lighting_needs_ltc(const LightingDesc& d) noexcept
{
    return d.set.count[static_cast<crd::u32>(LightType::Rect)] > 0U
           || d.set.count[static_cast<crd::u32>(LightType::Tube)] > 0U
           || d.set.count[static_cast<crd::u32>(LightType::Disk)] > 0U;
}
bool lighting_needs_ies(const LightingDesc& d) noexcept { return d.record.has_ies; }
bool lighting_needs_depth(const LightingDesc& d) noexcept { return d.shadow.contact; }
bool lighting_shadow_is_comparison(const LightingDesc& d) noexcept
{
    return d.shadow.filter != ShadowFilter::Evsm && d.shadow.filter != ShadowFilter::Msm;
}
bool lighting_needs_plain_shadow_sampler(const LightingDesc& d) noexcept
{
    return d.shadow.filter == ShadowFilter::Pcss;
}

// ── PARSE ─────────────────────────────────────────────────────────────────────────────────────────────────
LightingCookError parse_lighting_toml(crd::containers::StringView toml_text, LightingDesc& out,
                                      crd::containers::String* where)
{
    // NON-THROWING parse (TOML_EXCEPTIONS=0, the frame-cook pattern). A thrown parse_error unwinding
    // while a live GPU device (validation layer hooked into SEH dispatch) CRASHED the process — an
    // authored-asset TYPO became a process kill once disk-first loading made user edits reachable.
    // Result-checked also kills the mixed-mode ODR hazard (three cookers threw, three did not).
    toml::parse_result pr = toml::parse(std::string_view(toml_text.data(), toml_text.size()));
    if (!pr) { return LightingCookError::ParseFailed; }
    toml::table root = std::move(pr).table();
    // ⛔ RESET FIRST — the scar the vertex/material cookers both carried: parsing into a reused descriptor
    // APPENDED to it, so a tool with a load button silently merged two declarations.
    out.name.clear();
    out.header  = LightingHeaderMap{};
    out.record  = LightRecordDesc{};
    out.set     = LightSetDesc{};
    out.shadow  = ShadowDesc{};
    out.ibl     = IblDesc{};
    out.cluster = ClusterDesc{};
    out.decal   = DecalDesc{};

    const auto sch = root["schema"].value<int64_t>();
    if (!sch || *sch != static_cast<int64_t>(kLightingSchemaVersion)) { return LightingCookError::BadSchema; }
    out.schema = kLightingSchemaVersion;
    const auto nm = root["name"].value<std::string_view>();
    if (!nm || nm->empty()) { return LightingCookError::MissingName; }
    set_str(out.name, *nm);

    if (const auto* h = root["header"].as_table())
    {
        const auto w = [&](const char* k, crd::u32& dst) {
            if (const auto v = (*h)[k].value<int64_t>()) { dst = static_cast<crd::u32>(*v); }
        };
        w("view_proj", out.header.view_proj);
        w("csm_splits", out.header.csm_splits);
        w("light_off", out.header.light_off);
        w("decal_off", out.header.decal_off);
        w("cluster_off", out.header.cluster_off);
    }

    if (const auto* r = root["record"].as_table())
    {
        const auto w = [&](const char* k, crd::u32& dst) {
            if (const auto v = (*r)[k].value<int64_t>()) { dst = static_cast<crd::u32>(*v); }
        };
        const auto wf = [&](const char* k, crd::u32& dst, bool& has) {
            if (const auto v = (*r)[k].value<int64_t>())
            {
                dst = static_cast<crd::u32>(*v);
                has = true;
            }
        };
        w("stride", out.record.stride);
        w("position", out.record.position);
        w("color", out.record.color);
        w("direction", out.record.direction);
        w("falloff", out.record.falloff);
        w("spot_scale", out.record.spot_scale);
        w("spot_offset", out.record.spot_offset);
        if ((*r)["p0"].value<int64_t>())
        {
            out.record.has_points = true;
            w("p0", out.record.p0);
            w("p1", out.record.p1);
            w("p2", out.record.p2);
            w("p3", out.record.p3);
        }
        wf("radius", out.record.radius, out.record.has_radius);
        wf("shadow_index", out.record.shadow_index, out.record.has_shadow_index);
        wf("shadow_vp", out.record.shadow_vp, out.record.has_shadow_vp);
        wf("shadow_range", out.record.shadow_range, out.record.has_shadow_range);
        wf("ies_index", out.record.ies_index, out.record.has_ies);
    }

    if (const auto* c = root["counts"].as_table())
    {
        for (crd::u32 i = 0; i < kLightTypeCount; ++i)
        {
            out.set.count[i] =
                static_cast<crd::u32>((*c)[light_type_name(static_cast<LightType>(i))].value_or<int64_t>(0));
        }
    }

    if (const auto* s = root["shadow"].as_table())
    {
        for (crd::u32 i = 0; i < kLightTypeCount; ++i)
        {
            out.shadow.mode[i] = shadow_mode_of(
                (*s)[light_type_name(static_cast<LightType>(i))].value_or<std::string_view>("none"));
        }
        out.shadow.filter        = shadow_filter_of((*s)["filter"].value_or<std::string_view>("pcf"));
        out.shadow.taps          = static_cast<crd::u32>((*s)["taps"].value_or<int64_t>(8));
        out.shadow.cascades      = static_cast<crd::u32>((*s)["cascades"].value_or<int64_t>(4));
        out.shadow.contact       = (*s)["contact"].value_or<bool>(false);
        out.shadow.contact_steps = static_cast<crd::u32>((*s)["contact_steps"].value_or<int64_t>(8));
    }

    if (const auto* i = root["ibl"].as_table())
    {
        out.ibl.diffuse  = (*i)["diffuse"].value_or<bool>(false);
        out.ibl.specular = (*i)["specular"].value_or<bool>(false);
    }

    if (const auto* c = root["cluster"].as_table())
    {
        out.cluster.enabled = (*c)["enabled"].value_or<bool>(false);
        if (const auto* gr = (*c)["grid"].as_array())
        {
            crd::u32 k = 0;
            for (const auto& e : *gr)
            {
                if (k < 3U) { out.cluster.grid[k++] = static_cast<crd::u32>(e.value_or<int64_t>(1)); }
            }
        }
        out.cluster.max_per_cluster = static_cast<crd::u32>((*c)["max_per_cluster"].value_or<int64_t>(8));
    }

    if (const auto* d = root["decal"].as_table())
    {
        out.decal.count      = static_cast<crd::u32>((*d)["count"].value_or<int64_t>(0));
        out.decal.stride     = static_cast<crd::u32>((*d)["stride"].value_or<int64_t>(20));
        out.decal.projection = static_cast<crd::u32>((*d)["projection"].value_or<int64_t>(0));
        out.decal.tint       = static_cast<crd::u32>((*d)["tint"].value_or<int64_t>(16));
    }

    return validate_lighting(out, where);
}

// ── VALIDATE ──────────────────────────────────────────────────────────────────────────────────────────────
LightingCookError validate_lighting(const LightingDesc& d, crd::containers::String* where)
{
    const crd::u32 total = lighting_total_lights(d);
    // ⛔ A lighting technique that lights NOTHING is a black screen with no error anywhere to explain it.
    if (total == 0U && !d.ibl.diffuse && !d.ibl.specular)
    {
        set_where(where, std::string_view(d.name.c_str(), d.name.size()));
        return LightingCookError::NoLights;
    }
    if (total > kMaxTotalLights) { return LightingCookError::TooManyLights; }
    for (crd::u32 i = 0; i < kLightTypeCount; ++i)
    {
        if (d.set.count[i] > kMaxPerType)
        {
            set_where(where, light_type_name(static_cast<LightType>(i)));
            return LightingCookError::TooManyLights;
        }
    }

    // ⛔ EVERY FIELD A DECLARED TYPE NEEDS must fit the record, and a field past the stride reads the NEXT
    // light's words — a spotlight taking its neighbour's cone, in a scene that still renders.
    const auto fits = [&](crd::u32 off, crd::u32 n) { return off + n <= d.record.stride; };
    const auto need = [&](bool cond, crd::u32 off, crd::u32 n, const char* what) {
        if (!cond) { return LightingCookError::Ok; }
        if (!fits(off, n))
        {
            set_where(where, what);
            return LightingCookError::FieldOutOfRecord;
        }
        return LightingCookError::Ok;
    };
    const bool any_punctual = d.set.count[1] > 0U || d.set.count[2] > 0U;
    const bool any_dir      = d.set.count[0] > 0U || d.set.count[2] > 0U;
    const bool any_area     = lighting_needs_ltc(d);
    LightingCookError e = need(total > 0U, d.record.color, 3U, "color");
    if (e != LightingCookError::Ok) { return e; }
    e = need(any_punctual || any_area, d.record.position, 3U, "position");
    if (e != LightingCookError::Ok) { return e; }
    e = need(any_dir, d.record.direction, 3U, "direction");
    if (e != LightingCookError::Ok) { return e; }
    e = need(any_punctual, d.record.falloff, 1U, "falloff");
    if (e != LightingCookError::Ok) { return e; }
    e = need(d.set.count[2] > 0U, d.record.spot_scale, 1U, "spot_scale");
    if (e != LightingCookError::Ok) { return e; }
    e = need(d.set.count[2] > 0U, d.record.spot_offset, 1U, "spot_offset");
    if (e != LightingCookError::Ok) { return e; }

    // ⛔ AN AREA LIGHT NEEDS A SHAPE. Without the corner/endpoint fields the LTC solve would integrate over
    // adjacent lights' words — an area light illuminating from a polygon that does not exist.
    if (any_area)
    {
        if (!d.record.has_points)
        {
            set_where(where, "record.p0");
            return LightingCookError::MissingField;
        }
        const crd::u32 pts[4] = {d.record.p0, d.record.p1, d.record.p2, d.record.p3};
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            if (!fits(pts[i], 3U))
            {
                set_where(where, "record.p*");
                return LightingCookError::FieldOutOfRecord;
            }
        }
        if (d.set.count[static_cast<crd::u32>(LightType::Tube)] > 0U
            || d.set.count[static_cast<crd::u32>(LightType::Disk)] > 0U)
        {
            if (!d.record.has_radius)
            {
                set_where(where, "record.radius");
                return LightingCookError::MissingField;
            }
            if (!fits(d.record.radius, 1U)) { return LightingCookError::FieldOutOfRecord; }
        }
    }

    // ── shadows
    for (crd::u32 i = 0; i < kLightTypeCount; ++i)
    {
        const ShadowMode m = d.shadow.mode[i];
        if (m == ShadowMode::None) { continue; }
        if (d.set.count[i] == 0U) { continue; }
        const auto t = static_cast<LightType>(i);
        // ⛔ A SCHEME THAT DOES NOT APPLY. CSM is a DIRECTIONAL construction (it splits the view frustum along
        // the camera's depth); a cube shadow is a POINT one (six faces around a position). Accepting a mismatch
        // would cook a projection that has no meaning for the light and darken the scene semi-randomly.
        const bool okmode = (t == LightType::Directional && m == ShadowMode::Csm)
                            || (t == LightType::Spot && m == ShadowMode::Map)
                            || (t == LightType::Point && m == ShadowMode::Cube);
        if (!okmode)
        {
            set_where(where, light_type_name(t));
            return LightingCookError::BadShadowMode;
        }
        if (!d.record.has_shadow_index && m != ShadowMode::Csm)
        {
            set_where(where, "record.shadow_index");
            return LightingCookError::MissingField;
        }
        if (m == ShadowMode::Map && !d.record.has_shadow_vp)
        {
            set_where(where, "record.shadow_vp");
            return LightingCookError::MissingField;
        }
        // ⛔ A CUBE shadow compares a RADIAL DISTANCE, so it needs the light's far range as the denominator.
        // Without it the compare is against an unnormalised metre value and everything is either lit or black.
        if (m == ShadowMode::Cube && !d.record.has_shadow_range)
        {
            set_where(where, "record.shadow_range");
            return LightingCookError::MissingField;
        }
        if (m == ShadowMode::Map && !fits(d.record.shadow_vp, 16U))
        {
            set_where(where, "record.shadow_vp");
            return LightingCookError::FieldOutOfRecord;
        }
    }
    if (lighting_needs_shadow_atlas(d))
    {
        if (d.shadow.taps != 1U && d.shadow.taps != 4U && d.shadow.taps != 8U && d.shadow.taps != 16U)
        {
            return LightingCookError::BadFilter;
        }
    }
    if (lighting_needs_csm(d) && (d.shadow.cascades < 1U || d.shadow.cascades > kMaxCascades))
    {
        return LightingCookError::BadCascades;
    }
    if (d.shadow.contact && (d.shadow.contact_steps < 1U || d.shadow.contact_steps > kMaxContactSteps))
    {
        return LightingCookError::BadContact;
    }

    if (d.cluster.enabled)
    {
        if (d.cluster.grid[0] == 0U || d.cluster.grid[1] == 0U || d.cluster.grid[2] == 0U
            || d.cluster.max_per_cluster == 0U || d.cluster.max_per_cluster > kMaxTotalLights)
        {
            return LightingCookError::BadCluster;
        }
    }
    if (d.decal.count > 0U)
    {
        if (d.decal.count > kMaxDecals || d.decal.projection + 16U > d.decal.stride
            || d.decal.tint + 4U > d.decal.stride)
        {
            return LightingCookError::BadDecal;
        }
    }
    return LightingCookError::Ok;
}

// ── THE VARIANT IDENTITY ──────────────────────────────────────────────────────────────────────────────────
namespace
{
constexpr crd::u64 kFnvOffset = 14695981039346656037ULL;
constexpr crd::u64 kFnvPrime  = 1099511628211ULL;
void               hash_u64(crd::u64& h, crd::u64 v) noexcept
{
    for (int i = 0; i < 8; ++i)
    {
        h ^= (v >> (i * 8)) & 0xFFULL;
        h *= kFnvPrime;
    }
}
} // namespace

crd::u64 lighting_variant_id(const LightingDesc& d) noexcept
{
    crd::u64 h = kFnvOffset;
    for (crd::usize i = 0; i < d.name.size(); ++i)
    {
        h ^= static_cast<crd::u8>(d.name.c_str()[i]);
        h *= kFnvPrime;
    }
    for (crd::u32 i = 0; i < kLightTypeCount; ++i)
    {
        hash_u64(h, d.set.count[i]);
        hash_u64(h, static_cast<crd::u64>(d.shadow.mode[i]));
    }
    hash_u64(h, static_cast<crd::u64>(d.shadow.filter));
    hash_u64(h, d.shadow.taps);
    hash_u64(h, d.shadow.cascades);
    hash_u64(h, d.shadow.contact ? d.shadow.contact_steps + 1ULL : 0ULL);
    hash_u64(h, d.ibl.diffuse ? 1ULL : 0ULL);
    hash_u64(h, d.ibl.specular ? 1ULL : 0ULL);
    hash_u64(h, d.cluster.enabled ? d.cluster.max_per_cluster + 1ULL : 0ULL);
    for (crd::u32 i = 0; i < 3U; ++i) { hash_u64(h, d.cluster.grid[i]); }
    hash_u64(h, d.decal.count);
    hash_u64(h, d.decal.stride);
    hash_u64(h, d.decal.projection);
    hash_u64(h, d.decal.tint);
    const crd::u32 rec[] = {d.record.stride,       d.record.position,     d.record.color,
                            d.record.direction,    d.record.falloff,      d.record.spot_scale,
                            d.record.spot_offset,  d.record.p0,           d.record.p1,
                            d.record.p2,           d.record.p3,           d.record.radius,
                            d.record.shadow_index, d.record.shadow_vp,    d.record.shadow_range,
                            d.record.ies_index};
    for (const crd::u32 v : rec) { hash_u64(h, v); }
    const bool flags[] = {d.record.has_points,       d.record.has_radius,    d.record.has_shadow_index,
                          d.record.has_shadow_vp,    d.record.has_shadow_range, d.record.has_ies};
    for (const bool f : flags) { hash_u64(h, f ? 1ULL : 0ULL); }
    const crd::u32 hdr[] = {d.header.view_proj, d.header.csm_splits, d.header.light_off, d.header.decal_off,
                            d.header.cluster_off};
    for (const crd::u32 v : hdr) { hash_u64(h, v); }
    return h;
}

// ── EMIT ──────────────────────────────────────────────────────────────────────────────────────────────────
namespace
{
void app(crd::containers::String& o, const char* t) { o.append(t); }
void app_u32(crd::containers::String& o, crd::u32 v)
{
    char buf[16];
    (void)std::snprintf(static_cast<char*>(buf), sizeof(buf), "%u", v);
    o.append(static_cast<const char*>(buf));
}
void kv(crd::containers::String& o, const char* k, crd::u32 v)
{
    app(o, k);
    app(o, " = ");
    app_u32(o, v);
    app(o, "\n");
}
} // namespace

crd::containers::String emit_lighting_toml(const LightingDesc& d, crd::memory::IAllocator* a)
{
    crd::containers::String o(a);
    app(o, "schema = 1\nname   = \"");
    o.append(d.name.c_str());
    app(o, "\"\n\n[header]\n");
    kv(o, "view_proj", d.header.view_proj);
    kv(o, "csm_splits", d.header.csm_splits);
    kv(o, "light_off", d.header.light_off);
    kv(o, "decal_off", d.header.decal_off);
    kv(o, "cluster_off", d.header.cluster_off);

    app(o, "\n[record]\n");
    kv(o, "stride", d.record.stride);
    kv(o, "position", d.record.position);
    kv(o, "color", d.record.color);
    kv(o, "direction", d.record.direction);
    kv(o, "falloff", d.record.falloff);
    kv(o, "spot_scale", d.record.spot_scale);
    kv(o, "spot_offset", d.record.spot_offset);
    if (d.record.has_points)
    {
        kv(o, "p0", d.record.p0);
        kv(o, "p1", d.record.p1);
        kv(o, "p2", d.record.p2);
        kv(o, "p3", d.record.p3);
    }
    if (d.record.has_radius) { kv(o, "radius", d.record.radius); }
    if (d.record.has_shadow_index) { kv(o, "shadow_index", d.record.shadow_index); }
    if (d.record.has_shadow_vp) { kv(o, "shadow_vp", d.record.shadow_vp); }
    if (d.record.has_shadow_range) { kv(o, "shadow_range", d.record.shadow_range); }
    if (d.record.has_ies) { kv(o, "ies_index", d.record.ies_index); }

    app(o, "\n[counts]\n");
    for (crd::u32 i = 0; i < kLightTypeCount; ++i)
    {
        kv(o, light_type_name(static_cast<LightType>(i)), d.set.count[i]);
    }

    app(o, "\n[shadow]\n");
    for (crd::u32 i = 0; i < kLightTypeCount; ++i)
    {
        app(o, light_type_name(static_cast<LightType>(i)));
        app(o, " = \"");
        app(o, shadow_mode_name(d.shadow.mode[i]));
        app(o, "\"\n");
    }
    app(o, "filter = \"");
    app(o, shadow_filter_name(d.shadow.filter));
    app(o, "\"\n");
    kv(o, "taps", d.shadow.taps);
    kv(o, "cascades", d.shadow.cascades);
    app(o, d.shadow.contact ? "contact = true\n" : "contact = false\n");
    kv(o, "contact_steps", d.shadow.contact_steps);

    app(o, "\n[ibl]\n");
    app(o, d.ibl.diffuse ? "diffuse  = true\n" : "diffuse  = false\n");
    app(o, d.ibl.specular ? "specular = true\n" : "specular = false\n");

    app(o, "\n[cluster]\n");
    app(o, d.cluster.enabled ? "enabled = true\n" : "enabled = false\n");
    app(o, "grid    = [");
    for (crd::u32 i = 0; i < 3U; ++i)
    {
        if (i > 0U) { app(o, ", "); }
        app_u32(o, d.cluster.grid[i]);
    }
    app(o, "]\n");
    kv(o, "max_per_cluster", d.cluster.max_per_cluster);

    app(o, "\n[decal]\n");
    kv(o, "count", d.decal.count);
    kv(o, "stride", d.decal.stride);
    kv(o, "projection", d.decal.projection);
    kv(o, "tint", d.decal.tint);
    return o;
}

// ── ⭐ THE COOK ───────────────────────────────────────────────────────────────────────────────────────────
namespace
{
// The filtered shadow lookup, shared by every scheme. `uv` is vec2, `ref` the reference depth, `slice` the atlas
// layer as a float node.
[[nodiscard]] int filtered_shadow(Lx& c, const LightingDesc& d, const LightingBindings& b, int uv, int ref,
                                  int slice, int frag_xy)
{
    KGraph&   g   = c.g;
    const int tex = b.shadow_atlas;
    const int smp = b.shadow_sampler;
    if (tex < 0 || smp < 0) { return -1; }
    const int uv3 = g.vec3(g.swizzle(uv, 0), g.swizzle(uv, 1), slice);

    if (d.shadow.filter == ShadowFilter::Evsm || d.shadow.filter == ShadowFilter::Msm)
    {
        // ⛔ A MOMENT map is a filterable COLOUR texture, not a comparison one — the sample returns the stored
        // moments and the test is analytic. Sampling it through a comparison sampler would return a depth test
        // against a variance number: shadows that are wrong, not shadows that error.
        const int moments = g.tex_sample(tex, smp, uv3);
        if (d.shadow.filter == ShadowFilter::Evsm)
        {
            return lt::evsm_shadow(g, moments, ref, c.kf(40.0), c.kf(5.0), c.kf(0.0001), c.kf(0.2));
        }
        return lt::msm_hamburger(g, moments, ref, c.kf(0.0), c.kf(0.0003));
    }

    // Hard / PCF / PCSS all sample a COMPARISON texture: the sample IS the test.
    const int u0 = g.swizzle(uv, 0);
    const int v0 = g.swizzle(uv, 1);
    if (d.shadow.filter == ShadowFilter::Hard)
    {
        return g.tex_sample_cmp(tex, smp, g.vec3(u0, v0, slice), ref);
    }

    int radius = c.dvd(c.kf(1.0), c.mxf(b.csm_map_size >= 0 ? b.csm_map_size : c.kf(2048.0), c.kf(1.0)));
    if (d.shadow.filter == ShadowFilter::Pcss)
    {
        // ⭐ PCSS: a blocker search sets the penumbra, so a contact hardens and a distant edge softens. ⛔ Without
        // the search the radius is constant and every shadow has the same softness at every distance — which
        // reads as a stylistic choice rather than a missing algorithm.
        // ⛔⛔ THE BLOCKER SEARCH READS DEPTH VALUES, NOT A COMPARISON. The combined GLSL type follows the
        // SAMPLER node, so reading through the comparison sampler emitted `texture(sampler2DArrayShadow, vec3)`
        // — an overload that does not exist. The shader failed to compile with nothing pointing here; the shape
        // checker found it. The declaration REQUIRES the separate plain sampler, and its absence FAILS by name.
        if (b.shadow_plain_sampler < 0) { return -1; }
        const int z_blocker = g.swizzle(g.tex_sample(tex, b.shadow_plain_sampler, uv3), 0);
        const int pen       = lt::pcss_penumbra(g, ref, z_blocker, c.kf(0.02));
        radius              = c.mul(radius, c.mxf(c.mul(pen, c.kf(16.0)), c.kf(1.0)));
    }

    // ⛔⛔ THE TAPS ARE BUILT HERE, not by `lighting::pcf_shadow`. That helper takes a **vec2** uv — a 2-D shadow
    // map — and offsets it with a vec2; every atlas in this system is LAYERED, so the sample needs a THIRD
    // component for the slice. Handing it a vec3 lands on `detail::bin`'s "two mismatched vectors — a caller
    // error" arm, which builds a shape-invalid node: the graph assembles, `cook_lighting` returns a valid id, and
    // the SHADER FAILS TO COMPILE with nothing pointing at the uv width. So the slice rides the tap coordinate.
    //
    // The tap count is a declared SHAPE option, never a uniform — there is no branch that unrolls a loop.
    const int  r    = c.mul(radius, c.kf(static_cast<double>(d.shadow.taps)));
    const int  ang  = c.mul(lt::detail::ign(g, frag_xy), c.kf(6.28318530718));
    const int  cs   = g.unary(KOp::Cos, ang);
    const int  sn   = g.unary(KOp::Sin, ang);
    const auto taps = static_cast<int>(d.shadow.taps);
    int        sum  = c.kf(0.0);
    for (int i = 0; i < taps; ++i)
    {
        const int p  = lt::detail::poisson8(g, r, i % 8);
        const int px = g.swizzle(p, 0);
        const int py = g.swizzle(p, 1);
        // the per-pixel rotation is what hides the tap pattern; without it the 8 taps read as banding
        const int rx = c.sub(c.mul(px, cs), c.mul(py, sn));
        const int ry = c.add(c.mul(px, sn), c.mul(py, cs));
        const int tu = c.add(u0, c.mul(rx, r));
        const int tv = c.add(v0, c.mul(ry, r));
        sum          = c.add(sum, g.tex_sample_cmp(tex, smp, g.vec3(tu, tv, slice), ref));
    }
    return c.mul(sum, c.kf(1.0 / static_cast<double>(taps)));
}

// CSM: containment-based cascade selection, then the filtered lookup. ⛔ Selecting by SPLIT DISTANCE alone puts a
// point just outside a cascade's map into a lookup that samples off the edge — a bright fringe that moves with
// the camera. Containment picks the tightest cascade that actually holds the point.
[[nodiscard]] int csm_shadow(Lx& c, const LightingDesc& d, const LightingBindings& b, int world_pos, int frag_xy)
{
    KGraph&   g   = c.g;
    const int wp4 = g.vec4(g.swizzle(world_pos, 0), g.swizzle(world_pos, 1), g.swizzle(world_pos, 2), c.kf(1.0));
    int       result = c.kf(1.0);
    int       taken  = c.kf(0.0);
    for (crd::u32 ci = d.shadow.cascades; ci-- > 0U;)
    {
        const int vp = b.csm_light_vp[ci];
        if (vp < 0) { return -1; }
        const int lp = g.mat_mul_vec(vp, wp4);
        const int iw = c.dvd(c.kf(1.0), c.mxf(g.swizzle(lp, 3), c.kf(1.0e-6)));
        const int u  = c.add(c.mul(c.mul(g.swizzle(lp, 0), iw), c.kf(0.5)), c.kf(0.5));
        const int v  = c.add(c.mul(c.mul(g.swizzle(lp, 1), iw), c.kf(0.5)), c.kf(0.5));
        const int z  = c.mul(g.swizzle(lp, 2), iw);
        int       in = g.binary(KOp::Step, c.kf(0.02), u);
        in           = c.mul(in, g.binary(KOp::Step, u, c.kf(0.98)));
        in           = c.mul(in, g.binary(KOp::Step, c.kf(0.02), v));
        in           = c.mul(in, g.binary(KOp::Step, v, c.kf(0.98)));
        in           = c.mul(in, g.binary(KOp::Step, c.kf(0.0), z));
        in           = c.mul(in, g.binary(KOp::Step, z, c.kf(1.0)));
        const int s  = filtered_shadow(c, d, b, g.vec2(u, v), z, c.kf(static_cast<double>(ci)), frag_xy);
        if (s < 0) { return -1; }
        // walking from the LAST cascade back means the tightest containing one wins
        result = g.select(g.binary(KOp::CmpGt, in, c.kf(0.5)), s, result);
        taken  = c.mxf(taken, in);
    }
    return result;
}
} // namespace

int cook_lighting(const LightingDesc& d, KGraph& g, const LightingInputs& in, const LightingBindings& b,
                  crd::kir::ShapeIssue* shape_issue)
{
    if (validate_lighting(d, nullptr) != LightingCookError::Ok) { return -1; }
    if (in.base_color < 0 || in.metallic < 0 || in.roughness < 0 || in.normal < 0 || in.view_dir < 0
        || in.world_pos < 0)
    {
        return -1;
    }
    Lx        c(g);
    const int light_base = c.hdru(d.header.light_off);
    const int frag_xy    = in.frag_xy >= 0 ? in.frag_xy : g.vec2(c.kf(0.5), c.kf(0.5));

    // ── ⭐ E4 DECALS, applied to the SURFACE before shading. A decal blended after shading would paint over the
    // lighting and glow in shadow; before it, a decal is simply a different material at that pixel.
    int base_color = in.base_color;
    if (d.decal.count > 0U)
    {
        if (b.decal_atlas < 0 || b.decal_sampler < 0) { return -1; }
        const int dbase = c.hdru(d.header.decal_off);
        for (crd::u32 i = 0; i < d.decal.count; ++i)
        {
            const int rec = c.add(dbase, c.ku(i * d.decal.stride));
            int       clip[4];
            (void)c.mat4_vec(rec, d.decal.projection, in.world_pos, clip);
            const int iw = c.dvd(c.kf(1.0), c.mxf(clip[3], c.kf(1.0e-6)));
            const int u  = c.add(c.mul(c.mul(clip[0], iw), c.kf(0.5)), c.kf(0.5));
            const int v  = c.add(c.mul(c.mul(clip[1], iw), c.kf(0.5)), c.kf(0.5));
            // ⛔ INSIDE THE PROJECTOR BOX ONLY. A decal without the bounds test wraps its texture across the
            // whole world, which looks like a broken uv rather than a missing clip.
            int inside = g.binary(KOp::Step, c.kf(0.0), u);
            inside     = c.mul(inside, g.binary(KOp::Step, u, c.kf(1.0)));
            inside     = c.mul(inside, g.binary(KOp::Step, c.kf(0.0), v));
            inside     = c.mul(inside, g.binary(KOp::Step, v, c.kf(1.0)));
            const int uvw = g.vec3(u, v, c.kf(static_cast<double>(i)));
            const int tex = g.tex_sample(b.decal_atlas, b.decal_sampler, uvw);
            const int tint = g.vec3(c.f_at(rec, d.decal.tint + 0U), c.f_at(rec, d.decal.tint + 1U),
                                    c.f_at(rec, d.decal.tint + 2U));
            const int w   = c.mul(c.mul(c.f_at(rec, d.decal.tint + 3U), inside), g.swizzle(tex, 3));
            const int rgb = nd::detail::bin(g, KOp::Mul, g.vec3(g.swizzle(tex, 0), g.swizzle(tex, 1),
                                                                g.swizzle(tex, 2)),
                                            tint);
            base_color = nd::detail::tern(g, KOp::Mix, base_color, rgb, w);
        }
    }

    // ── ⭐ E5 CLUSTERED CULLING. With a cluster list the unrolled bound is `max_per_cluster` rather than the
    // scene's light count, which is the whole point: a 200-light scene costs 8 lights per pixel.
    // ⛔ The froxel index is computed from the pixel and the view depth; without the depth term every pixel in a
    // column shares one cluster and distant lights leak into the foreground.
    int cluster_index = -1;
    if (d.cluster.enabled)
    {
        const int sx = c.mul(g.swizzle(frag_xy, 0), c.kf(static_cast<double>(d.cluster.grid[0])));
        const int sy = c.mul(g.swizzle(frag_xy, 1), c.kf(static_cast<double>(d.cluster.grid[1])));
        const int wz = b.cluster_grid >= 0 ? b.cluster_grid : c.kf(1.0);
        const int sz = nd::clamp(g, c.mul(wz, c.kf(static_cast<double>(d.cluster.grid[2]))), c.kf(0.0),
                                 c.kf(static_cast<double>(d.cluster.grid[2] - 1U)));
        const int fi = c.add(c.add(g.unary(KOp::Floor, sx),
                                   c.mul(g.unary(KOp::Floor, sy), c.kf(static_cast<double>(d.cluster.grid[0])))),
                             c.mul(g.unary(KOp::Floor, sz),
                                   c.kf(static_cast<double>(d.cluster.grid[0] * d.cluster.grid[1]))));
        cluster_index = g.cast(fi, DType::U32);
    }

    // ── the light loop, per type, unrolled over the DECLARED count.
    int acc = g.vec3(c.kf(0.0), c.kf(0.0), c.kf(0.0));
    const auto record_of = [&](crd::u32 flat_index) {
        if (cluster_index < 0) { return c.add(light_base, c.ku(flat_index * d.record.stride)); }
        // ⭐ Clustered: the slot holds an INDEX into the light array, read from this pixel's cluster run.
        const int slot = c.loadu(c.add(c.add(c.hdru(d.header.cluster_off),
                                             c.mul(cluster_index, c.ku(d.cluster.max_per_cluster))),
                                       c.ku(flat_index)));
        return c.add(light_base, g.binary(KOp::Mul, slot, c.ku(d.record.stride)));
    };

    const int nrm  = in.normal;
    const int view = in.view_dir;

    for (crd::u32 ti = 0; ti < kLightTypeCount; ++ti)
    {
        const auto     type  = static_cast<LightType>(ti);
        crd::u32       n     = d.set.count[ti];
        const crd::u32 first = lighting_type_first(d, type);
        // Under clustering the punctual types walk the per-cluster run instead of the global array.
        const bool clustered = d.cluster.enabled && (type == LightType::Point || type == LightType::Spot);
        if (clustered) { n = n < d.cluster.max_per_cluster ? n : d.cluster.max_per_cluster; }
        for (crd::u32 i = 0; i < n; ++i)
        {
            const int rec   = clustered ? record_of(i) : c.add(light_base, c.ku((first + i) * d.record.stride));
            const int color = c.v3_at(rec, d.record.color);
            int       contrib = -1;
            int       l_dir   = -1; // the unit vector surface → light, for contact shadows

            if (type == LightType::Directional)
            {
                const int dir = c.v3_at(rec, d.record.direction);
                contrib = lt::directional_light(g, base_color, in.metallic, in.roughness, nrm, view, dir, color);
                l_dir   = g.normalize(g.unary(KOp::Neg, dir));
            }
            else if (type == LightType::Point)
            {
                const int pos = c.v3_at(rec, d.record.position);
                contrib = lt::point_light(g, base_color, in.metallic, in.roughness, nrm, view, in.world_pos, pos,
                                          color, c.f_at(rec, d.record.falloff));
                l_dir   = g.normalize(g.binary(KOp::Sub, pos, in.world_pos));
            }
            else if (type == LightType::Spot)
            {
                const int pos = c.v3_at(rec, d.record.position);
                contrib = lt::spot_light(g, base_color, in.metallic, in.roughness, nrm, view, in.world_pos, pos,
                                         color, c.f_at(rec, d.record.falloff), c.v3_at(rec, d.record.direction),
                                         c.f_at(rec, d.record.spot_scale), c.f_at(rec, d.record.spot_offset));
                l_dir   = g.normalize(g.binary(KOp::Sub, pos, in.world_pos));
            }
            else
            {
                // ── ⭐ E2 AREA LIGHTS (Heitz LTC). The fitted Minv comes from the LUT, indexed by roughness and
                // N·V — which is why an area light needs a texture binding a punctual one does not.
                if (b.ltc_lut < 0 || b.ltc_sampler < 0) { return -1; }
                const int nov  = c.mxf(g.dot(nrm, view), c.kf(1.0e-4));
                const int uv   = lt::ltc_lut_coord(g, in.roughness, nov);
                const int t1   = g.tex_sample(b.ltc_lut, b.ltc_sampler, uv);
                const int minv = lt::ltc_matrix(g, t1);
                const int p0   = c.v3_at(rec, d.record.p0);
                const int p1   = c.v3_at(rec, d.record.p1);
                if (type == LightType::Rect)
                {
                    const int p2 = c.v3_at(rec, d.record.p2);
                    const int p3 = c.v3_at(rec, d.record.p3);
                    contrib = lt::ltc_evaluate_rect(g, nrm, view, in.world_pos, minv, p0, p1, p2, p3,
                                                    g.swizzle(t1, 3), true);
                }
                else if (type == LightType::Tube)
                {
                    contrib = lt::ltc_evaluate_line(g, nrm, view, in.world_pos, minv, p0, p1,
                                                    c.f_at(rec, d.record.radius));
                }
                else
                {
                    const int p2 = c.v3_at(rec, d.record.p2);
                    contrib = lt::ltc_evaluate_disk(g, nrm, view, in.world_pos, minv, p0, p1, p2,
                                                    g.swizzle(t1, 3), true);
                }
                contrib = nd::detail::bin(g, KOp::Mul, g.splat(contrib, 3), color);
                l_dir   = g.normalize(g.binary(KOp::Sub, p0, in.world_pos));
            }
            if (contrib < 0) { return -1; }

            // ── ⭐ E2 IES PROFILES. A real luminaire is not a cone: its intensity varies with angle, and the
            // profile is what makes a wall washer look like a wall washer rather than a spotlight.
            if (d.record.has_ies)
            {
                if (b.ies_atlas < 0 || b.ies_sampler < 0) { return -1; }
                const int dir   = type == LightType::Directional ? c.v3_at(rec, d.record.direction)
                                                                 : g.unary(KOp::Neg, l_dir);
                const int cosa  = nd::clamp(g, g.dot(g.normalize(dir), g.normalize(l_dir)), c.kf(-1.0), c.kf(1.0));
                const int vcoord = c.add(c.mul(cosa, c.kf(-0.5)), c.kf(0.5)); // angle → [0,1]
                const int row    = c.f_at(rec, d.record.ies_index);
                const int prof   = g.tex_sample(b.ies_atlas, b.ies_sampler, g.vec2(vcoord, row));
                contrib          = nd::detail::bin(g, KOp::Mul, contrib, g.splat(g.swizzle(prof, 0), 3));
            }

            // ── ⭐ E6 SHADOWS, per light type.
            const ShadowMode mode = d.shadow.mode[ti];
            if (mode != ShadowMode::None)
            {
                int vis = -1;
                if (mode == ShadowMode::Csm) { vis = csm_shadow(c, d, b, in.world_pos, frag_xy); }
                else if (mode == ShadowMode::Map)
                {
                    int clip[4];
                    (void)c.mat4_vec(rec, d.record.shadow_vp, in.world_pos, clip);
                    const int iw = c.dvd(c.kf(1.0), c.mxf(clip[3], c.kf(1.0e-6)));
                    const int u  = c.add(c.mul(c.mul(clip[0], iw), c.kf(0.5)), c.kf(0.5));
                    const int v  = c.add(c.mul(c.mul(clip[1], iw), c.kf(0.5)), c.kf(0.5));
                    vis = filtered_shadow(c, d, b, g.vec2(u, v), c.mul(clip[2], iw),
                                          c.f_at(rec, d.record.shadow_index), frag_xy);
                }
                else
                {
                    // ⭐ CUBE: compare the RADIAL DISTANCE, not a projected depth. Six faces, selected by the
                    // major axis of the light→surface vector; the slice run starts at `shadow_index`.
                    const int pos  = c.v3_at(rec, d.record.position);
                    const int dvec = g.binary(KOp::Sub, in.world_pos, pos);
                    const int ax   = g.unary(KOp::Abs, g.swizzle(dvec, 0));
                    const int ay   = g.unary(KOp::Abs, g.swizzle(dvec, 1));
                    const int az   = g.unary(KOp::Abs, g.swizzle(dvec, 2));
                    const int amax = c.mxf(ax, c.mxf(ay, az));
                    // face = 2·axis + (component < 0)
                    const int is_x  = g.binary(KOp::CmpGe, ax, amax);
                    const int is_y  = g.binary(KOp::CmpGe, ay, amax);
                    const int negx  = g.binary(KOp::CmpLt, g.swizzle(dvec, 0), c.kf(0.0));
                    const int negy  = g.binary(KOp::CmpLt, g.swizzle(dvec, 1), c.kf(0.0));
                    const int negz  = g.binary(KOp::CmpLt, g.swizzle(dvec, 2), c.kf(0.0));
                    const int fx    = g.select(negx, c.kf(1.0), c.kf(0.0));
                    const int fy    = g.select(negy, c.kf(3.0), c.kf(2.0));
                    const int fz    = g.select(negz, c.kf(5.0), c.kf(4.0));
                    const int face  = g.select(is_x, fx, g.select(is_y, fy, fz));
                    const int inv   = c.dvd(c.kf(1.0), c.mxf(amax, c.kf(1.0e-6)));
                    // the two minor axes, mapped to [0,1] — the standard cube-face parameterisation
                    const int u = c.add(c.mul(c.mul(g.swizzle(dvec, 2), inv), c.kf(0.5)), c.kf(0.5));
                    const int v = c.add(c.mul(c.mul(g.swizzle(dvec, 1), inv), c.kf(0.5)), c.kf(0.5));
                    const int range = c.mxf(c.f_at(rec, d.record.shadow_range), c.kf(1.0e-4));
                    const int refd  = c.dvd(g.vlength(dvec), range);
                    vis = filtered_shadow(c, d, b, g.vec2(u, v),
                                          c.sub(refd, c.kf(0.005)), // constant bias: a radial compare has no slope
                                          c.add(c.f_at(rec, d.record.shadow_index), face), frag_xy);
                }
                if (vis < 0) { return -1; }

                // ── ⭐ E6 CONTACT SHADOWS. A shadow map at any practical resolution loses the CONTACT — the
                // few-pixel darkening where an object meets a surface — and its absence is what makes objects
                // look like they float. The screen-space march recovers exactly that band.
                if (d.shadow.contact)
                {
                    if (b.depth_tex < 0 || b.depth_sampler < 0) { return -1; }
                    // ⛔⛔ `lt::contact_shadow` IS A 4-TAP WINDOW — it swizzles lanes 0..3 of its ray/scene
                    // depths. This loop used to hand it SCALARS, so the helper read lanes 1..3 of a 1-wide
                    // value — undefined on every backend — and the close gate measured NODE COUNTS, so it
                    // cooked green. The shape checker (REN-38 audit) found it. The marched samples are packed
                    // four to a vec4 now; the tail group pads by repetition (a duplicate tap cannot change a
                    // max).
                    int ray_d[kMaxContactSteps]   = {};
                    int scene_d[kMaxContactSteps] = {};
                    for (crd::u32 s = 1; s <= d.shadow.contact_steps; ++s)
                    {
                        const double t   = static_cast<double>(s) / static_cast<double>(d.shadow.contact_steps);
                        const int    pw  = nd::detail::bin(g, KOp::Add, in.world_pos,
                                                           g.splat(c.mul(c.kf(t), c.kf(0.15)), 3));
                        const int    ray = nd::detail::bin(g, KOp::Add, pw, g.splat(c.kf(0.0), 3));
                        int          clipv[4];
                        const int    p4 = g.vec4(g.swizzle(ray, 0), g.swizzle(ray, 1), g.swizzle(ray, 2), c.kf(1.0));
                        for (crd::u32 k = 0; k < 4U; ++k)
                        {
                            int a2 = c.mul(c.hdrf(d.header.view_proj + 0U * 4U + k), g.swizzle(p4, 0));
                            a2     = c.add(a2, c.mul(c.hdrf(d.header.view_proj + 1U * 4U + k), g.swizzle(p4, 1)));
                            a2     = c.add(a2, c.mul(c.hdrf(d.header.view_proj + 2U * 4U + k), g.swizzle(p4, 2)));
                            a2     = c.add(a2, c.mul(c.hdrf(d.header.view_proj + 3U * 4U + k), g.swizzle(p4, 3)));
                            clipv[k] = a2;
                        }
                        const int iw2 = c.dvd(c.kf(1.0), c.mxf(clipv[3], c.kf(1.0e-6)));
                        const int su  = c.add(c.mul(c.mul(clipv[0], iw2), c.kf(0.5)), c.kf(0.5));
                        const int sv  = c.add(c.mul(c.mul(clipv[1], iw2), c.kf(0.5)), c.kf(0.5));
                        ray_d[s - 1U]   = c.mul(clipv[2], iw2);
                        scene_d[s - 1U] =
                            g.swizzle(g.tex_sample(b.depth_tex, b.depth_sampler, g.vec2(su, sv)), 0);
                    }
                    int occl = c.kf(0.0);
                    for (crd::u32 g0 = 0; g0 < d.shadow.contact_steps; g0 += 4U)
                    {
                        const auto at = [&](crd::u32 i) {
                            const crd::u32 idx = g0 + i < d.shadow.contact_steps ? g0 + i
                                                                                 : d.shadow.contact_steps - 1U;
                            return idx;
                        };
                        const int rz4 = g.vec4(ray_d[at(0U)], ray_d[at(1U)], ray_d[at(2U)], ray_d[at(3U)]);
                        const int sz4 = g.vec4(scene_d[at(0U)], scene_d[at(1U)], scene_d[at(2U)], scene_d[at(3U)]);
                        occl = c.mxf(occl, lt::contact_shadow(g, rz4, sz4, c.kf(0.002), c.kf(0.05), c.kf(0.5)));
                    }
                    vis = c.mul(vis, nd::clamp01(g, c.sub(c.kf(1.0), occl)));
                }
                contrib = nd::detail::bin(g, KOp::Mul, contrib, g.splat(vis, 3));
            }
            acc = nd::detail::bin(g, KOp::Add, acc, contrib);
        }
    }

    // ── ⭐ E3 IBL. `sh_irradiance` + the Karis split-sum specular have existed in CKIR since B8-e with no
    // binding type able to reach them.
    if (d.ibl.diffuse)
    {
        for (const int s : b.sh)
        {
            if (s < 0) { return -1; }
        }
        const int irr     = lt::sh_irradiance(g, nrm, static_cast<const int*>(b.sh));
        const int diffuse = nd::detail::bin(g, KOp::Mul, base_color,
                                            g.splat(c.sub(c.kf(1.0), in.metallic), 3));
        acc = nd::detail::bin(g, KOp::Add, acc, lt::ibl_diffuse(g, diffuse, irr));
    }
    if (d.ibl.specular)
    {
        if (b.prefiltered < 0 || b.env_sampler < 0) { return -1; }
        const int nov = c.mxf(g.dot(nrm, view), c.kf(1.0e-4));
        const int r   = g.reflect(g.unary(KOp::Neg, view), nrm);
        const int env = g.tex_sample(b.prefiltered, b.env_sampler, r);
        // F0 = mix(0.04, base_color, metallic) — the standard metallic-roughness remap.
        const int f0 = nd::detail::tern(g, KOp::Mix, g.vec3(c.kf(0.04), c.kf(0.04), c.kf(0.04)), base_color,
                                        g.splat(in.metallic, 3));
        acc = nd::detail::bin(g, KOp::Add, acc,
                              lt::ibl_specular(g, g.vec3(g.swizzle(env, 0), g.swizzle(env, 1), g.swizzle(env, 2)),
                                               f0, in.roughness, nov));
    }

    if (in.emissive >= 0) { acc = nd::detail::bin(g, KOp::Add, acc, in.emissive); }
    const int lit = nd::clamp01(g, acc);
    // ⛔ THE SHAPE CHECK (REN-38 audit). This very function is where the class was discovered: a vec3 uv into
    // `lighting::pcf_shadow`'s vec2 parameter landed on `detail::bin`'s mismatched-vector arm, the cook
    // returned a valid node id, and the SHADER failed to compile with nothing pointing at the uv width. The
    // callers were fixed then; this refuses the whole class at the cook boundary, for every future scheme.
    if (lit >= 0 && !crd::kir::graph_shapes_valid(g, lit, d.name.allocator(), shape_issue)) { return -1; }
    return lit;
}

const char* lighting_cook_error_text(LightingCookError e) noexcept
{
    switch (e)
    {
    case LightingCookError::Ok:               return "ok";
    case LightingCookError::ParseFailed:      return "not valid TOML";
    case LightingCookError::BadSchema:        return "missing or unsupported `schema`";
    case LightingCookError::MissingName:      return "no `name`";
    case LightingCookError::NoLights:         return "a lighting technique that lights nothing";
    case LightingCookError::TooManyLights:    return "more lights than the unrolled cap";
    case LightingCookError::FieldOutOfRecord: return "a light field reads past the record stride";
    case LightingCookError::MissingField:     return "a declared light type needs a field the record lacks";
    case LightingCookError::BadShadowMode:    return "a shadow scheme that does not apply to that light type";
    case LightingCookError::BadFilter:        return "a tap count that is not 1, 4, 8 or 16";
    case LightingCookError::BadCascades:      return "a cascade count outside 1..4";
    case LightingCookError::BadCluster:       return "a froxel grid or per-cluster cap that cannot work";
    case LightingCookError::BadDecal:         return "a decal record that does not fit its stride";
    case LightingCookError::BadContact:       return "a contact-shadow step count outside 1..16";
    }
    return "unknown error";
}

} // namespace crd::lightcook
