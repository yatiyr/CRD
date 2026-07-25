// frame_asset.cpp — REN-36.1: parse + VALIDATE a `.frame.toml`, and cook it to a canonical `.crdr` blob.
// Contract + rationale: frame_asset.hpp and docs/design/ren-36-authorable-frame-graph.md.

#include <crd/framecook/frame_asset.hpp>

#include <toml++/toml.hpp>

#include <cstring>
#include <string_view>

namespace crd::framecook
{
namespace
{

// ── the cooked container ─────────────────────────────────────────────────────────────────────────────────────
// FourCC 'FRAM'. CANONICAL + PACKED + PADDING-FREE + little-endian by construction: every field is written
// explicitly, so the bytes are a pure function of the DESCRIPTION and a graph cooked under MSVC loads
// byte-identically under gcc/clang. (The ckir_serialize scar — never memcpy a POD into an artifact.)
constexpr crd::u32 kFourCC = (static_cast<crd::u32>('F')) | (static_cast<crd::u32>('R') << 8U)
                             | (static_cast<crd::u32>('A') << 16U) | (static_cast<crd::u32>('M') << 24U);
constexpr crd::u32 kBlobVersion = 1U;

using Bytes = crd::containers::Array<crd::u8>;

void put_u8(Bytes& b, crd::u8 v) { b.push_back(v); }
void put_u32(Bytes& b, crd::u32 v)
{
    for (crd::u32 s = 0; s < 32U; s += 8U) { b.push_back(static_cast<crd::u8>((v >> s) & 0xFFU)); }
}
void put_f32(Bytes& b, float v)
{
    crd::u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(b, bits); // the BIT PATTERN, never a decimal round-trip
}
void put_f64(Bytes& b, double v)
{
    crd::u64 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    for (crd::u32 s = 0; s < 64U; s += 8U) { b.push_back(static_cast<crd::u8>((bits >> s) & 0xFFU)); }
}
void put_str(Bytes& b, const crd::containers::String& s)
{
    put_u32(b, static_cast<crd::u32>(s.size()));
    for (crd::usize i = 0; i < s.size(); ++i) { b.push_back(static_cast<crd::u8>(s.c_str()[i])); }
}

struct Cursor
{
    crd::containers::ConstSpan<crd::u8> in;
    crd::u64                            pos = 0;
    bool                                ok  = true;

    bool have(crd::u64 n) noexcept
    {
        if (!ok || pos + n > in.size()) { ok = false; }
        return ok;
    }
    crd::u8  u8v() noexcept { return have(1U) ? in[pos++] : static_cast<crd::u8>(0); }
    crd::u32 u32v() noexcept
    {
        if (!have(4U)) { return 0U; }
        crd::u32 v = 0;
        for (crd::u32 i = 0; i < 4U; ++i) { v |= static_cast<crd::u32>(in[pos + i]) << (i * 8U); }
        pos += 4U;
        return v;
    }
    float f32v() noexcept
    {
        const crd::u32 bits = u32v();
        float          v    = 0.0F;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    double f64v() noexcept
    {
        if (!have(8U)) { return 0.0; }
        crd::u64 bits = 0;
        for (crd::u32 i = 0; i < 8U; ++i) { bits |= static_cast<crd::u64>(in[pos + i]) << (i * 8U); }
        pos += 8U;
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    void strv(crd::containers::String& out) noexcept
    {
        const crd::u32 n = u32v();
        if (!have(n)) { return; }
        out.clear();
        for (crd::u32 i = 0; i < n; ++i)
        {
            const char c[2] = {static_cast<char>(in[pos + i]), '\0'};
            out.append(static_cast<const char*>(c));
        }
        pos += n;
    }
};

// ── string → enum. Every miss is a NAMED cook error, never a silent default. ─────────────────────────────────
bool to_pass_kind(std::string_view s, FramePassKind& out)
{
    if (s == "raster.geometry")   { out = FramePassKind::RasterGeometry;   return true; }
    if (s == "raster.depth_only") { out = FramePassKind::RasterDepthOnly;  return true; }
    if (s == "raster.fullscreen") { out = FramePassKind::RasterFullscreen; return true; }
    if (s == "raster.mrt")        { out = FramePassKind::RasterMrt;        return true; }
    if (s == "compute")           { out = FramePassKind::Compute;          return true; }
    if (s == "present")           { out = FramePassKind::Present;          return true; }
    return false;
}
bool to_format(std::string_view s, crd::gpu::FgImageFormat& out)
{
    using F = crd::gpu::FgImageFormat;
    if (s == "RGBA8Unorm") { out = F::RGBA8Unorm; return true; }
    if (s == "RGBA8Srgb")  { out = F::RGBA8Srgb;  return true; }
    if (s == "RGBA16F")    { out = F::RGBA16F;    return true; }
    if (s == "R16F")       { out = F::R16F;       return true; }
    if (s == "R32F")       { out = F::R32F;       return true; }
    if (s == "R32Uint")    { out = F::R32Uint;    return true; }
    if (s == "D32Float")   { out = F::D32Float;   return true; }
    return false;
}
bool to_compare(std::string_view s, crd::gpu::DepthCompare& out)
{
    using C = crd::gpu::DepthCompare;
    if (s == "Never")        { out = C::Never;        return true; }
    if (s == "Less")         { out = C::Less;         return true; }
    if (s == "Equal")        { out = C::Equal;        return true; }
    if (s == "LessEqual")    { out = C::LessEqual;    return true; }
    if (s == "Greater")      { out = C::Greater;      return true; }
    if (s == "NotEqual")     { out = C::NotEqual;     return true; }
    if (s == "GreaterEqual") { out = C::GreaterEqual; return true; }
    if (s == "Always")       { out = C::Always;       return true; }
    return false;
}
bool to_material_pass(std::string_view s, FrameMaterialPass& out)
{
    if (s == "Shadow")       { out = FrameMaterialPass::Shadow;       return true; }
    if (s == "DepthPrepass") { out = FrameMaterialPass::DepthPrepass; return true; }
    if (s == "GBuffer")      { out = FrameMaterialPass::GBuffer;      return true; }
    if (s == "Forward")      { out = FrameMaterialPass::Forward;      return true; }
    return false;
}
bool to_cull(std::string_view s, FrameCullMode& out)
{
    if (s == "none")              { out = FrameCullMode::None;             return true; }
    if (s == "frustum")           { out = FrameCullMode::Frustum;          return true; }
    if (s == "frustum+occlusion") { out = FrameCullMode::FrustumOcclusion; return true; }
    return false;
}
bool to_sort(std::string_view s, FrameSortMode& out)
{
    if (s == "none")           { out = FrameSortMode::None;        return true; }
    if (s == "front_to_back")  { out = FrameSortMode::FrontToBack; return true; }
    if (s == "back_to_front")  { out = FrameSortMode::BackToFront; return true; }
    if (s == "material")       { out = FrameSortMode::Material;    return true; }
    return false;
}
// `light.0.cascades` → (LightCascades, 0). The numeric arg is parsed out of the generator name.
bool to_for_each(std::string_view s, FrameForEach& out, crd::u32& arg)
{
    arg = 0U;
    if (s == "views.stereo")          { out = FrameForEach::StereoViews;         return true; }
    if (s == "cube.faces")            { out = FrameForEach::CubeFaces;           return true; }
    if (s == "lights.shadow_casting") { out = FrameForEach::ShadowCastingLights; return true; }
    if (s.size() > 7U && s.starts_with("light.") && s.ends_with(".cascades"))
    {
        crd::u32 n   = 0U;
        bool     any = false;
        for (crd::usize i = 6; i < s.size() - 9U; ++i)
        {
            if (s[i] < '0' || s[i] > '9') { return false; }
            n = (n * 10U) + static_cast<crd::u32>(s[i] - '0');
            any = true;
        }
        if (!any) { return false; }
        out = FrameForEach::LightCascades;
        arg = n;
        return true;
    }
    return false;
}

void set_str(crd::containers::String& dst, std::string_view s)
{
    dst.clear();
    for (const char c : s)
    {
        const char one[2] = {c, '\0'};
        dst.append(static_cast<const char*>(one));
    }
}
void set_where(crd::containers::String* where, std::string_view s)
{
    if (where != nullptr) { set_str(*where, s); }
}
bool str_eq(const crd::containers::String& a, std::string_view b)
{
    return a.size() == b.size() && std::memcmp(a.c_str(), b.data(), b.size()) == 0;
}

// A reference like `shadow_atlas[$index]` splits into the name + the indexed flag.
void parse_ref(std::string_view s, FrameResourceRef& out)
{
    constexpr std::string_view idx_suffix = "[$index]";
    if (s.size() > idx_suffix.size() && s.ends_with(idx_suffix))
    {
        out.indexed = true;
        set_str(out.name, s.substr(0, s.size() - idx_suffix.size()));
        return;
    }
    out.indexed = false;
    set_str(out.name, s);
}

} // namespace

const char* frame_cook_error_text(FrameCookError err) noexcept
{
    switch (err)
    {
    case FrameCookError::Ok:                    return "ok";
    case FrameCookError::ParseFailed:           return "not valid TOML";
    case FrameCookError::BadSchema:             return "missing or unsupported `schema`";
    case FrameCookError::MissingName:           return "the graph, a resource, a draw list or a pass has no `name`";
    case FrameCookError::DuplicateName:         return "two entries in the same category share a name";
    case FrameCookError::UnknownPassKind:       return "unknown pass `kind`";
    case FrameCookError::UnknownFormat:         return "unknown resource `format`";
    case FrameCookError::UnknownCompare:        return "unknown `depth` comparison";
    case FrameCookError::UnknownSort:           return "unknown draw-list `sort`";
    case FrameCookError::UnknownCull:           return "unknown draw-list `cull`";
    case FrameCookError::UnknownMaterialPass:   return "unknown `material_pass`";
    case FrameCookError::UnknownForEach:        return "unknown `for_each` generator";
    case FrameCookError::UnknownResource:       return "a pass reads or writes a resource that was never declared";
    case FrameCookError::ResourceNeverWritten:  return "a declared resource is never written by any pass";
    case FrameCookError::DependencyCycle:       return "the passes form a dependency CYCLE";
    case FrameCookError::MissingShader:         return "a fullscreen pass needs `shader` (a compute pass needs `kernel`)";
    case FrameCookError::MissingDrawList:       return "a geometry or depth-only pass needs `draw_list`";
    case FrameCookError::SubscriptOnNonLayered: return "`[$index]` used on a resource with layers == 1";
    case FrameCookError::IndexWithoutForEach:   return "`[$index]` used by a pass that declares no `for_each`";
    case FrameCookError::NoOutputPass:          return "no pass writes `@output`";
    case FrameCookError::BadResourceSize:       return "a resource needs either width+height or scale";
    case FrameCookError::LayersOutOfRange:      return "`layers` must be between 1 and 16";
    }
    return "unknown error";
}

FrameCookError parse_frame_toml(crd::containers::StringView toml_text, FrameGraphDesc& out,
                                crd::containers::String* where)
{
    auto* alloc = out.resources.allocator();
    const std::string_view text(toml_text.data(), toml_text.size());
    const toml::parse_result res = toml::parse(text);
    if (!res) { return FrameCookError::ParseFailed; }
    const toml::table& root = res.table();

    const auto sch = root["schema"].value<int64_t>();
    if (!sch || *sch != static_cast<int64_t>(kFrameSchemaVersion)) { return FrameCookError::BadSchema; }
    out.schema = kFrameSchemaVersion;

    const auto nm = root["name"].value<std::string_view>();
    if (!nm || nm->empty()) { return FrameCookError::MissingName; }
    set_str(out.name, *nm);

    if (const auto fb = root["fallback"].value<std::string_view>()) { set_str(out.fallback, *fb); }
    if (const auto* caps = root["requires"].as_array())
    {
        for (const auto& c : *caps)
        {
            const auto s = c.value<std::string_view>();
            if (!s) { return FrameCookError::ParseFailed; }
            crd::containers::String cap(alloc);
            set_str(cap, *s);
            out.requires_caps.push_back(static_cast<crd::containers::String&&>(cap));
        }
    }

    // ── resources ──
    if (const auto* arr = root["resource"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FrameResourceDesc r(alloc);
            const auto rn = (*t)["name"].value<std::string_view>();
            if (!rn || rn->empty()) { return FrameCookError::MissingName; }
            set_str(r.name, *rn);
            for (crd::usize i = 0; i < out.resources.size(); ++i)
            {
                if (str_eq(out.resources[i].name, *rn)) { set_where(where, *rn); return FrameCookError::DuplicateName; }
            }
            const auto kind = (*t)["kind"].value_or(std::string_view{"transient_image"});
            r.kind = (kind == "transient_buffer") ? FrameResourceKind::TransientBuffer : FrameResourceKind::TransientImage;
            if (const auto f = (*t)["format"].value<std::string_view>())
            {
                if (!to_format(*f, r.format)) { set_where(where, *f); return FrameCookError::UnknownFormat; }
            }
            r.width      = static_cast<crd::u32>((*t)["width"].value_or<int64_t>(0));
            r.height     = static_cast<crd::u32>((*t)["height"].value_or<int64_t>(0));
            r.scale      = static_cast<float>((*t)["scale"].value_or<double>(0.0));
            r.layers     = static_cast<crd::u32>((*t)["layers"].value_or<int64_t>(1));
            r.samples    = static_cast<crd::u32>((*t)["samples"].value_or<int64_t>(1));
            r.sampled    = (*t)["sampled"].value_or(false);
            r.storage    = (*t)["storage"].value_or(false);
            r.size_bytes = static_cast<crd::u32>((*t)["size_bytes"].value_or<int64_t>(0));
            if (r.kind == FrameResourceKind::TransientImage
                && ((r.width == 0U || r.height == 0U) && r.scale <= 0.0F))
            {
                set_where(where, *rn);
                return FrameCookError::BadResourceSize;
            }
            if (r.kind == FrameResourceKind::TransientBuffer && r.size_bytes == 0U)
            {
                set_where(where, *rn);
                return FrameCookError::BadResourceSize;
            }
            out.resources.push_back(static_cast<FrameResourceDesc&&>(r));
        }
    }

    // ── draw lists (ECS queries the graph declares itself) ──
    if (const auto* arr = root["draw_list"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FrameDrawListDesc d(alloc);
            const auto dn = (*t)["name"].value<std::string_view>();
            if (!dn || dn->empty()) { return FrameCookError::MissingName; }
            set_str(d.name, *dn);
            for (crd::usize i = 0; i < out.draw_lists.size(); ++i)
            {
                if (str_eq(out.draw_lists[i].name, *dn)) { set_where(where, *dn); return FrameCookError::DuplicateName; }
            }
            const auto comps = [&](const char* key, crd::containers::Array<crd::containers::String>& dst) {
                if (const auto* a2 = (*t)[key].as_array())
                {
                    for (const auto& c : *a2)
                    {
                        const auto s = c.value<std::string_view>();
                        if (!s) { continue; }
                        crd::containers::String cs(alloc);
                        set_str(cs, *s);
                        dst.push_back(static_cast<crd::containers::String&&>(cs));
                    }
                }
            };
            comps("all", d.all);
            comps("any", d.any);
            comps("none", d.none);
            if (const auto c = (*t)["cull"].value<std::string_view>())
            {
                if (!to_cull(*c, d.cull)) { set_where(where, *c); return FrameCookError::UnknownCull; }
            }
            if (const auto s = (*t)["sort"].value<std::string_view>())
            {
                if (!to_sort(*s, d.sort)) { set_where(where, *s); return FrameCookError::UnknownSort; }
            }
            d.limit = static_cast<crd::u32>((*t)["limit"].value_or<int64_t>(0));
            out.draw_lists.push_back(static_cast<FrameDrawListDesc&&>(d));
        }
    }

    // ── passes ──
    if (const auto* arr = root["pass"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FramePassDesc p(alloc);
            const auto pn = (*t)["name"].value<std::string_view>();
            if (!pn || pn->empty()) { return FrameCookError::MissingName; }
            set_str(p.name, *pn);
            for (crd::usize i = 0; i < out.passes.size(); ++i)
            {
                if (str_eq(out.passes[i].name, *pn)) { set_where(where, *pn); return FrameCookError::DuplicateName; }
            }
            const auto kd = (*t)["kind"].value<std::string_view>();
            if (!kd || !to_pass_kind(*kd, p.kind))
            {
                set_where(where, kd ? *kd : std::string_view{"<missing>"});
                return FrameCookError::UnknownPassKind;
            }
            const auto refs = [&](const char* key, crd::containers::Array<FrameResourceRef>& dst) {
                if (const auto* a2 = (*t)[key].as_array())
                {
                    for (const auto& c : *a2)
                    {
                        const auto s = c.value<std::string_view>();
                        if (!s) { continue; }
                        FrameResourceRef r(alloc);
                        parse_ref(*s, r);
                        dst.push_back(static_cast<FrameResourceRef&&>(r));
                    }
                }
            };
            refs("reads", p.reads);
            refs("writes", p.writes);
            if (const auto v = (*t)["draw_list"].value<std::string_view>()) { set_str(p.draw_list, *v); }
            if (const auto v = (*t)["view"].value<std::string_view>())      { set_str(p.view, *v); }
            if (const auto v = (*t)["shader"].value<std::string_view>())    { set_str(p.shader, *v); }
            if (const auto v = (*t)["kernel"].value<std::string_view>())    { set_str(p.kernel, *v); }
            if (const auto v = (*t)["material_pass"].value<std::string_view>())
            {
                if (!to_material_pass(*v, p.material_pass)) { set_where(where, *v); return FrameCookError::UnknownMaterialPass; }
            }
            if (const auto v = (*t)["for_each"].value<std::string_view>())
            {
                if (!to_for_each(*v, p.for_each, p.for_each_arg)) { set_where(where, *v); return FrameCookError::UnknownForEach; }
            }
            if (const auto v = (*t)["depth"].value<std::string_view>())
            {
                if (!to_compare(*v, p.depth)) { set_where(where, *v); return FrameCookError::UnknownCompare; }
            }
            if (const auto* cc = (*t)["clear_color"].as_array())
            {
                p.has_clear_color = true;
                crd::u32 i = 0;
                for (const auto& c : *cc)
                {
                    if (i < 4U) { p.clear_color[i++] = static_cast<float>(c.value_or<double>(0.0)); }
                }
            }
            if (const auto cd = (*t)["clear_depth"].value<double>())
            {
                p.has_clear_depth = true;
                p.clear_depth     = static_cast<float>(*cd);
            }
            if (const toml::table* pp = (*t)["params"].as_table())
            {
                for (const auto& [k, v] : *pp)
                {
                    FrameParam prm(alloc);
                    set_str(prm.name, std::string_view(k.str().data(), k.str().size()));
                    if (const auto* av = v.as_array())
                    {
                        prm.type = FrameParamType::Vec4;
                        crd::u32 i = 0;
                        for (const auto& e : *av) { if (i < 4U) { prm.v[i++] = e.value_or<double>(0.0); } }
                    }
                    else if (v.is_boolean())      { prm.type = FrameParamType::Bool;  prm.v[0] = v.value_or(false) ? 1.0 : 0.0; }
                    else if (v.is_integer())      { prm.type = FrameParamType::Int;   prm.v[0] = static_cast<double>(v.value_or<int64_t>(0)); }
                    else                          { prm.type = FrameParamType::Float; prm.v[0] = v.value_or<double>(0.0); }
                    p.params.push_back(static_cast<FrameParam&&>(prm));
                }
            }
            out.passes.push_back(static_cast<FramePassDesc&&>(p));
        }
    }

    return validate_frame_graph(out, where);
}

FrameCookError validate_frame_graph(const FrameGraphDesc& desc, crd::containers::String* where)
{
    auto* alloc = desc.resources.allocator();
    // ── VALIDATION. Every rejection is named, at COOK time — never at runtime on a user's machine. ──
    const auto find_resource = [&](const crd::containers::String& n) -> const FrameResourceDesc* {
        for (crd::usize i = 0; i < desc.resources.size(); ++i)
        {
            if (desc.resources[i].name.size() == n.size()
                && std::memcmp(desc.resources[i].name.c_str(), n.c_str(), n.size()) == 0)
            {
                return &desc.resources[i];
            }
        }
        return nullptr;
    };
    const auto is_output = [](const crd::containers::String& n) { return str_eq(n, "@output"); };

    // REN-3.2: the layer-count bound lives HERE, in the validator, not in the TOML parser — a PROGRAMMATIC graph
    // never passes through the parser, and the hard rule is that the two provenances are equal. A check only the
    // text path performed would make the ergonomic path the unsafe one.
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        if (r.kind == FrameResourceKind::TransientImage
            && (r.layers == 0U || r.layers > crd::gpu::kFgMaxImageLayers))
        {
            set_where(where, std::string_view(r.name.c_str(), r.name.size()));
            return FrameCookError::LayersOutOfRange;
        }
    }

    bool wrote_output = false;
    for (crd::usize pi = 0; pi < desc.passes.size(); ++pi)
    {
        const FramePassDesc& p = desc.passes[pi];
        if ((p.kind == FramePassKind::RasterGeometry || p.kind == FramePassKind::RasterDepthOnly
             || p.kind == FramePassKind::RasterMrt)
            && p.draw_list.empty())
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::MissingDrawList;
        }
        if (p.kind == FramePassKind::RasterFullscreen && p.shader.empty())
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::MissingShader;
        }
        if (p.kind == FramePassKind::Compute && p.kernel.empty())
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::MissingShader;
        }
        const auto check_refs = [&](const crd::containers::Array<FrameResourceRef>& refs) -> FrameCookError {
            for (crd::usize i = 0; i < refs.size(); ++i)
            {
                if (is_output(refs[i].name)) { continue; }
                const FrameResourceDesc* r = find_resource(refs[i].name);
                if (r == nullptr)
                {
                    set_where(where, std::string_view(refs[i].name.c_str(), refs[i].name.size()));
                    return FrameCookError::UnknownResource;
                }
                if (refs[i].indexed && r->layers <= 1U)
                {
                    set_where(where, std::string_view(refs[i].name.c_str(), refs[i].name.size()));
                    return FrameCookError::SubscriptOnNonLayered;
                }
                if (refs[i].indexed && p.for_each == FrameForEach::None)
                {
                    set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                    return FrameCookError::IndexWithoutForEach;
                }
            }
            return FrameCookError::Ok;
        };
        const FrameCookError e1 = check_refs(p.reads);
        if (e1 != FrameCookError::Ok) { return e1; }
        const FrameCookError e2 = check_refs(p.writes);
        if (e2 != FrameCookError::Ok) { return e2; }
        for (crd::usize i = 0; i < p.writes.size(); ++i)
        {
            if (is_output(p.writes[i].name)) { wrote_output = true; }
        }
    }
    if (!wrote_output) { return FrameCookError::NoOutputPass; }

    // every DECLARED resource must be produced by some pass (an unwritten transient is dead weight and, more
    // importantly, a sign the author mistyped a name — `build()` already rejects it at runtime; we reject earlier)
    for (crd::usize ri = 0; ri < desc.resources.size(); ++ri)
    {
        bool written = false;
        for (crd::usize pi = 0; pi < desc.passes.size() && !written; ++pi)
        {
            for (crd::usize wi = 0; wi < desc.passes[pi].writes.size(); ++wi)
            {
                if (str_eq(desc.resources[ri].name,
                           std::string_view(desc.passes[pi].writes[wi].name.c_str(), desc.passes[pi].writes[wi].name.size())))
                {
                    written = true;
                    break;
                }
            }
        }
        if (!written)
        {
            set_where(where, std::string_view(desc.resources[ri].name.c_str(), desc.resources[ri].name.size()));
            return FrameCookError::ResourceNeverWritten;
        }
    }

    // CYCLE detection — pass A depends on B when A reads something B writes. Kahn's algorithm over the pass DAG.
    const crd::usize np = desc.passes.size();
    crd::containers::Array<crd::u32> indeg(alloc);
    indeg.resize(np, 0U);
    crd::containers::Array<crd::u8> edge(alloc); // np*np adjacency (graphs are small; clarity over cleverness)
    edge.resize(np * np, 0U);
    for (crd::usize a = 0; a < np; ++a)
    {
        for (crd::usize b = 0; b < np; ++b)
        {
            if (a == b) { continue; }
            bool dep = false;
            for (crd::usize r = 0; r < desc.passes[a].reads.size() && !dep; ++r)
            {
                for (crd::usize w = 0; w < desc.passes[b].writes.size(); ++w)
                {
                    if (str_eq(desc.passes[a].reads[r].name,
                               std::string_view(desc.passes[b].writes[w].name.c_str(), desc.passes[b].writes[w].name.size())))
                    {
                        dep = true;
                        break;
                    }
                }
            }
            if (dep && edge[(b * np) + a] == 0U)
            {
                edge[(b * np) + a] = 1U; // b → a
                ++indeg[a];
            }
        }
    }
    crd::containers::Array<crd::u32> queue(alloc);
    for (crd::usize i = 0; i < np; ++i)
    {
        if (indeg[i] == 0U) { queue.push_back(static_cast<crd::u32>(i)); }
    }
    crd::usize visited = 0;
    for (crd::usize qi = 0; qi < queue.size(); ++qi)
    {
        const crd::u32 n = queue[qi];
        ++visited;
        for (crd::usize a = 0; a < np; ++a)
        {
            if (edge[(static_cast<crd::usize>(n) * np) + a] != 0U && --indeg[a] == 0U)
            {
                queue.push_back(static_cast<crd::u32>(a));
            }
        }
    }
    if (visited != np) { return FrameCookError::DependencyCycle; }

    return FrameCookError::Ok;
}

crd::containers::Array<crd::u8> cook_frame_graph(const FrameGraphDesc& desc, crd::memory::IAllocator* a)
{
    Bytes out(a);
    put_u32(out, kFourCC);
    put_u32(out, kBlobVersion);
    put_u32(out, desc.schema);
    put_str(out, desc.name);
    put_str(out, desc.fallback);

    put_u32(out, static_cast<crd::u32>(desc.requires_caps.size()));
    for (crd::usize i = 0; i < desc.requires_caps.size(); ++i) { put_str(out, desc.requires_caps[i]); }

    put_u32(out, static_cast<crd::u32>(desc.resources.size()));
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        put_str(out, r.name);
        put_u8(out, static_cast<crd::u8>(r.kind));
        put_u8(out, static_cast<crd::u8>(r.format));
        put_u32(out, r.width);
        put_u32(out, r.height);
        put_f32(out, r.scale);
        put_u32(out, r.layers);
        put_u32(out, r.samples);
        put_u8(out, r.sampled ? 1U : 0U);
        put_u8(out, r.storage ? 1U : 0U);
        put_u32(out, r.size_bytes);
    }

    put_u32(out, static_cast<crd::u32>(desc.draw_lists.size()));
    for (crd::usize i = 0; i < desc.draw_lists.size(); ++i)
    {
        const FrameDrawListDesc& d = desc.draw_lists[i];
        put_str(out, d.name);
        const auto put_list = [&](const crd::containers::Array<crd::containers::String>& l) {
            put_u32(out, static_cast<crd::u32>(l.size()));
            for (crd::usize k = 0; k < l.size(); ++k) { put_str(out, l[k]); }
        };
        put_list(d.all);
        put_list(d.any);
        put_list(d.none);
        put_u8(out, static_cast<crd::u8>(d.cull));
        put_u8(out, static_cast<crd::u8>(d.sort));
        put_u32(out, d.limit);
    }

    put_u32(out, static_cast<crd::u32>(desc.passes.size()));
    for (crd::usize i = 0; i < desc.passes.size(); ++i)
    {
        const FramePassDesc& p = desc.passes[i];
        put_str(out, p.name);
        put_u8(out, static_cast<crd::u8>(p.kind));
        const auto put_refs = [&](const crd::containers::Array<FrameResourceRef>& refs) {
            put_u32(out, static_cast<crd::u32>(refs.size()));
            for (crd::usize k = 0; k < refs.size(); ++k)
            {
                put_str(out, refs[k].name);
                put_u8(out, refs[k].indexed ? 1U : 0U);
            }
        };
        put_refs(p.reads);
        put_refs(p.writes);
        put_str(out, p.draw_list);
        put_str(out, p.view);
        put_str(out, p.shader);
        put_str(out, p.kernel);
        put_u8(out, static_cast<crd::u8>(p.material_pass));
        put_u8(out, static_cast<crd::u8>(p.for_each));
        put_u32(out, p.for_each_arg);
        put_u8(out, p.has_clear_color ? 1U : 0U);
        for (crd::u32 c = 0; c < 4U; ++c) { put_f32(out, p.clear_color[c]); }
        put_u8(out, p.has_clear_depth ? 1U : 0U);
        put_f32(out, p.clear_depth);
        put_u8(out, static_cast<crd::u8>(p.depth));
        put_u32(out, static_cast<crd::u32>(p.params.size()));
        for (crd::usize k = 0; k < p.params.size(); ++k)
        {
            put_str(out, p.params[k].name);
            put_u8(out, static_cast<crd::u8>(p.params[k].type));
            for (crd::u32 c = 0; c < 4U; ++c) { put_f64(out, p.params[k].v[c]); }
        }
    }
    return out;
}

bool read_frame_graph(crd::containers::ConstSpan<crd::u8> bytes, FrameGraphDesc& out)
{
    Cursor c{bytes, 0, true};
    if (c.u32v() != kFourCC || !c.ok) { return false; }
    if (c.u32v() != kBlobVersion) { return false; }
    out.schema = c.u32v();
    c.strv(out.name);
    c.strv(out.fallback);
    auto* alloc = out.resources.allocator();

    const crd::u32 ncap = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < ncap; ++i)
    {
        crd::containers::String s(alloc);
        c.strv(s);
        out.requires_caps.push_back(static_cast<crd::containers::String&&>(s));
    }

    const crd::u32 nres = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < nres; ++i)
    {
        FrameResourceDesc r(alloc);
        c.strv(r.name);
        r.kind       = static_cast<FrameResourceKind>(c.u8v());
        r.format     = static_cast<crd::gpu::FgImageFormat>(c.u8v());
        r.width      = c.u32v();
        r.height     = c.u32v();
        r.scale      = c.f32v();
        r.layers     = c.u32v();
        r.samples    = c.u32v();
        r.sampled    = c.u8v() != 0U;
        r.storage    = c.u8v() != 0U;
        r.size_bytes = c.u32v();
        out.resources.push_back(static_cast<FrameResourceDesc&&>(r));
    }

    const crd::u32 ndl = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < ndl; ++i)
    {
        FrameDrawListDesc d(alloc);
        c.strv(d.name);
        const auto get_list = [&](crd::containers::Array<crd::containers::String>& l) {
            const crd::u32 n = c.u32v();
            for (crd::u32 k = 0; k < n && c.ok; ++k)
            {
                crd::containers::String s(alloc);
                c.strv(s);
                l.push_back(static_cast<crd::containers::String&&>(s));
            }
        };
        get_list(d.all);
        get_list(d.any);
        get_list(d.none);
        d.cull  = static_cast<FrameCullMode>(c.u8v());
        d.sort  = static_cast<FrameSortMode>(c.u8v());
        d.limit = c.u32v();
        out.draw_lists.push_back(static_cast<FrameDrawListDesc&&>(d));
    }

    const crd::u32 npass = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < npass; ++i)
    {
        FramePassDesc p(alloc);
        c.strv(p.name);
        p.kind = static_cast<FramePassKind>(c.u8v());
        const auto get_refs = [&](crd::containers::Array<FrameResourceRef>& refs) {
            const crd::u32 n = c.u32v();
            for (crd::u32 k = 0; k < n && c.ok; ++k)
            {
                FrameResourceRef r(alloc);
                c.strv(r.name);
                r.indexed = c.u8v() != 0U;
                refs.push_back(static_cast<FrameResourceRef&&>(r));
            }
        };
        get_refs(p.reads);
        get_refs(p.writes);
        c.strv(p.draw_list);
        c.strv(p.view);
        c.strv(p.shader);
        c.strv(p.kernel);
        p.material_pass   = static_cast<FrameMaterialPass>(c.u8v());
        p.for_each        = static_cast<FrameForEach>(c.u8v());
        p.for_each_arg    = c.u32v();
        p.has_clear_color = c.u8v() != 0U;
        for (crd::u32 k = 0; k < 4U; ++k) { p.clear_color[k] = c.f32v(); }
        p.has_clear_depth = c.u8v() != 0U;
        p.clear_depth     = c.f32v();
        p.depth           = static_cast<crd::gpu::DepthCompare>(c.u8v());
        const crd::u32 nprm = c.u32v();
        for (crd::u32 k = 0; k < nprm && c.ok; ++k)
        {
            FrameParam prm(alloc);
            c.strv(prm.name);
            prm.type = static_cast<FrameParamType>(c.u8v());
            for (crd::u32 v = 0; v < 4U; ++v) { prm.v[v] = c.f64v(); }
            p.params.push_back(static_cast<FrameParam&&>(prm));
        }
        out.passes.push_back(static_cast<FramePassDesc&&>(p));
    }
    return c.ok;
}

// ── FrameGraphBuilder — the PROGRAMMATIC path. Same description, same validator, different provenance. ────────
namespace
{
void bset(crd::containers::String& dst, crd::containers::StringView s)
{
    dst.clear();
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const char one[2] = {s[i], '\0'};
        dst.append(static_cast<const char*>(one));
    }
}
} // namespace

FrameGraphBuilder::FrameGraphBuilder(crd::memory::IAllocator* alloc, crd::containers::StringView name)
    : m_desc(alloc), m_alloc(alloc)
{
    m_desc.schema = kFrameSchemaVersion;
    bset(m_desc.name, name);
}

crd::u32 FrameGraphBuilder::add_image(crd::containers::StringView name, crd::gpu::FgImageFormat format,
                                      crd::u32 width, crd::u32 height, bool sampled, crd::u32 layers)
{
    FrameResourceDesc r(m_alloc);
    bset(r.name, name);
    r.kind    = FrameResourceKind::TransientImage;
    r.format  = format;
    r.width   = width;
    r.height  = height;
    r.layers  = layers;
    r.sampled = sampled;
    m_desc.resources.push_back(static_cast<FrameResourceDesc&&>(r));
    return static_cast<crd::u32>(m_desc.resources.size() - 1U);
}

crd::u32 FrameGraphBuilder::add_scaled_image(crd::containers::StringView name, crd::gpu::FgImageFormat format,
                                             float scale, bool sampled)
{
    FrameResourceDesc r(m_alloc);
    bset(r.name, name);
    r.format  = format;
    r.scale   = scale;
    r.sampled = sampled;
    m_desc.resources.push_back(static_cast<FrameResourceDesc&&>(r));
    return static_cast<crd::u32>(m_desc.resources.size() - 1U);
}

crd::u32 FrameGraphBuilder::add_draw_list(crd::containers::StringView name)
{
    FrameDrawListDesc d(m_alloc);
    bset(d.name, name);
    m_desc.draw_lists.push_back(static_cast<FrameDrawListDesc&&>(d));
    return static_cast<crd::u32>(m_desc.draw_lists.size() - 1U);
}
void FrameGraphBuilder::draw_list_all(crd::u32 list, crd::containers::StringView component)
{
    crd::containers::String c(m_alloc);
    bset(c, component);
    m_desc.draw_lists[list].all.push_back(static_cast<crd::containers::String&&>(c));
}
void FrameGraphBuilder::draw_list_none(crd::u32 list, crd::containers::StringView component)
{
    crd::containers::String c(m_alloc);
    bset(c, component);
    m_desc.draw_lists[list].none.push_back(static_cast<crd::containers::String&&>(c));
}
void FrameGraphBuilder::draw_list_policy(crd::u32 list, FrameCullMode cull, FrameSortMode sort)
{
    m_desc.draw_lists[list].cull = cull;
    m_desc.draw_lists[list].sort = sort;
}

crd::u32 FrameGraphBuilder::add_pass(crd::containers::StringView name, FramePassKind kind)
{
    FramePassDesc p(m_alloc);
    bset(p.name, name);
    p.kind = kind;
    m_desc.passes.push_back(static_cast<FramePassDesc&&>(p));
    return static_cast<crd::u32>(m_desc.passes.size() - 1U);
}
void FrameGraphBuilder::pass_reads(crd::u32 pass, crd::containers::StringView resource, bool indexed)
{
    FrameResourceRef r(m_alloc);
    bset(r.name, resource);
    r.indexed = indexed;
    m_desc.passes[pass].reads.push_back(static_cast<FrameResourceRef&&>(r));
}
void FrameGraphBuilder::pass_writes(crd::u32 pass, crd::containers::StringView resource, bool indexed)
{
    FrameResourceRef r(m_alloc);
    bset(r.name, resource);
    r.indexed = indexed;
    m_desc.passes[pass].writes.push_back(static_cast<FrameResourceRef&&>(r));
}
void FrameGraphBuilder::pass_shader(crd::u32 pass, crd::containers::StringView id) { bset(m_desc.passes[pass].shader, id); }
void FrameGraphBuilder::pass_kernel(crd::u32 pass, crd::containers::StringView id) { bset(m_desc.passes[pass].kernel, id); }
void FrameGraphBuilder::pass_draw_list(crd::u32 pass, crd::containers::StringView n) { bset(m_desc.passes[pass].draw_list, n); }
void FrameGraphBuilder::pass_view(crd::u32 pass, crd::containers::StringView n) { bset(m_desc.passes[pass].view, n); }
void FrameGraphBuilder::pass_material(crd::u32 pass, FrameMaterialPass mp) { m_desc.passes[pass].material_pass = mp; }
void FrameGraphBuilder::pass_for_each(crd::u32 pass, FrameForEach gen, crd::u32 arg)
{
    m_desc.passes[pass].for_each     = gen;
    m_desc.passes[pass].for_each_arg = arg;
}
void FrameGraphBuilder::pass_clear_color(crd::u32 pass, float r, float g, float b, float a)
{
    FramePassDesc& p    = m_desc.passes[pass];
    p.has_clear_color   = true;
    p.clear_color[0]    = r;
    p.clear_color[1]    = g;
    p.clear_color[2]    = b;
    p.clear_color[3]    = a;
}
void FrameGraphBuilder::pass_clear_depth(crd::u32 pass, float d)
{
    m_desc.passes[pass].has_clear_depth = true;
    m_desc.passes[pass].clear_depth     = d;
}
void FrameGraphBuilder::pass_depth(crd::u32 pass, crd::gpu::DepthCompare cmp) { m_desc.passes[pass].depth = cmp; }
void FrameGraphBuilder::pass_param(crd::u32 pass, crd::containers::StringView name, double value)
{
    FrameParam prm(m_alloc);
    bset(prm.name, name);
    prm.type = FrameParamType::Float;
    prm.v[0] = value;
    m_desc.passes[pass].params.push_back(static_cast<FrameParam&&>(prm));
}
void FrameGraphBuilder::requires_capability(crd::containers::StringView cap)
{
    crd::containers::String c(m_alloc);
    bset(c, cap);
    m_desc.requires_caps.push_back(static_cast<crd::containers::String&&>(c));
}
void FrameGraphBuilder::fallback_to(crd::containers::StringView graph_name) { bset(m_desc.fallback, graph_name); }

} // namespace crd::framecook
