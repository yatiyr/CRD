// technique_asset.cpp — REN-37.2: parse + VALIDATE a `.crdt`, cook it to a canonical `.crdk` blob, and (REN-37.3)
// VERIFY a technique's declared pass-frequency bindings against the frame-graph pass that drives it.
// Contract + rationale: technique_asset.hpp and docs/design/ren-37-material-technique-composition.md §4.

#include <crd/techniquecook/technique_asset.hpp>

#include <toml++/toml.hpp>

#include <cstring>
#include <string_view>

namespace crd::techniquecook
{
namespace
{

// FourCC 'TECH'. Canonical, packed, padding-free, little-endian by construction — every field written explicitly,
// so the bytes are a pure function of the DESCRIPTION and a blob cooked under MSVC loads byte-identically under
// gcc/clang (the `ckir_serialize` scar: never memcpy a POD into an artifact).
constexpr crd::u32 kFourCC = (static_cast<crd::u32>('T')) | (static_cast<crd::u32>('E') << 8U)
                             | (static_cast<crd::u32>('C') << 16U) | (static_cast<crd::u32>('H') << 24U);
constexpr crd::u32 kBlobVersion = 1U;

using Bytes = crd::containers::Array<crd::u8>;

void put_u8(Bytes& b, crd::u8 v) { b.push_back(v); }
void put_u32(Bytes& b, crd::u32 v)
{
    for (crd::u32 s = 0; s < 32U; s += 8U) { b.push_back(static_cast<crd::u8>((v >> s) & 0xFFU)); }
}
void put_i32(Bytes& b, int v) { put_u32(b, static_cast<crd::u32>(v)); }
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
    crd::u8 u8v() noexcept { return have(1U) ? in[pos++] : static_cast<crd::u8>(0); }
    crd::u32 u32v() noexcept
    {
        if (!have(4U)) { return 0U; }
        crd::u32 v = 0;
        for (crd::u32 i = 0; i < 4U; ++i) { v |= static_cast<crd::u32>(in[pos + i]) << (i * 8U); }
        pos += 4U;
        return v;
    }
    int i32v() noexcept { return static_cast<int>(u32v()); }
    void strv(crd::containers::String& out)
    {
        const crd::u32 n = u32v();
        if (!have(n)) { return; }
        out.clear();
        for (crd::u32 i = 0; i < n; ++i)
        {
            const char one[2] = {static_cast<char>(in[pos + i]), '\0'};
            out.append(static_cast<const char*>(one));
        }
        pos += n;
    }
};

// ── string → enum. Every miss is a NAMED cook error, never a silent default. ─────────────────────────────────
bool to_bind_type(std::string_view s, BindType& out, bool& is_array)
{
    is_array = false;
    if (s == "float")                { out = BindType::Float;                return true; }
    if (s == "vec2")                 { out = BindType::Vec2;                 return true; }
    if (s == "vec3")                 { out = BindType::Vec3;                 return true; }
    if (s == "vec4")                 { out = BindType::Vec4;                 return true; }
    if (s == "mat4")                 { out = BindType::Mat4;                 return true; }
    if (s == "float[]")              { out = BindType::FloatArray; is_array = true; return true; }
    if (s == "mat4[]")               { out = BindType::Mat4Array;  is_array = true; return true; }
    if (s == "texture2D")            { out = BindType::Texture2D;            return true; }
    if (s == "texture2DArray")       { out = BindType::Texture2DArray;       return true; }
    if (s == "textureCube")          { out = BindType::TextureCube;          return true; }
    if (s == "texture2DShadow")      { out = BindType::Texture2DShadow;      return true; }
    if (s == "texture2DArrayShadow") { out = BindType::Texture2DArrayShadow; return true; }
    return false;
}
bool to_frequency(std::string_view s, BindFrequency& out)
{
    if (s == "frame")    { out = BindFrequency::Frame;    return true; }
    if (s == "pass")     { out = BindFrequency::Pass;     return true; }
    if (s == "material") { out = BindFrequency::Material; return true; }
    if (s == "object")   { out = BindFrequency::Object;   return true; }
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
[[nodiscard]] std::string_view sv(const crd::containers::String& s)
{
    return std::string_view(s.c_str(), s.size());
}

// The ONE surface contract this engine defines today. Named rather than implied so a future second contract
// (a thin-film slab, a volumetric slab) is a declared value with its own version, not a silent reinterpretation.
constexpr std::string_view kOpenPbrSurface = "OpenPBRSurface";

} // namespace

const char* technique_cook_error_text(TechniqueCookError err) noexcept
{
    switch (err)
    {
    case TechniqueCookError::Ok:                     return "ok";
    case TechniqueCookError::ParseFailed:            return "not valid TOML";
    case TechniqueCookError::BadSchema:              return "missing or unsupported `schema`";
    case TechniqueCookError::MissingName:            return "the technique or a binding/option has no `name`";
    case TechniqueCookError::MissingBody:            return "`body` must be `builtin:<name>` or `crd://<path>`";
    case TechniqueCookError::UnknownSurface:         return "`surface` names a contract this engine does not define";
    case TechniqueCookError::UnknownBindType:        return "unknown binding `type`";
    case TechniqueCookError::UnknownFrequency:       return "unknown binding `frequency`";
    case TechniqueCookError::DuplicateBinding:       return "two bindings share a name";
    case TechniqueCookError::DuplicateOption:        return "two options share a name";
    case TechniqueCookError::BadArrayCount:          return "`float[]`/`mat4[]` needs count > 1; a scalar type must not set one";
    case TechniqueCookError::BadOptionRange:         return "option `min` > `max`, or `default` outside [min, max]";
    case TechniqueCookError::PassMissingBinding:     return "the pass never reads a PASS-frequency input the technique requires";
    case TechniqueCookError::PassBindingNotLayered:  return "an array-shadow binding is wired to a resource with layers == 1";
    case TechniqueCookError::PassUnknownTechnique:   return "the pass names a technique nothing defines";
    }
    return "unknown error";
}

TechniqueCookError parse_technique_toml(crd::containers::StringView toml_text, TechniqueDesc& out,
                                        crd::containers::String* where)
{
    auto*                  alloc = out.bindings.allocator();
    const std::string_view text(toml_text.data(), toml_text.size());
    const toml::parse_result res = toml::parse(text);
    if (!res) { return TechniqueCookError::ParseFailed; }
    const toml::table& root = res.table();

    const auto sch = root["schema"].value<int64_t>();
    if (!sch || *sch != static_cast<int64_t>(kTechniqueSchemaVersion)) { return TechniqueCookError::BadSchema; }
    out.schema = kTechniqueSchemaVersion;

    const auto nm = root["name"].value<std::string_view>();
    if (!nm || nm->empty()) { return TechniqueCookError::MissingName; }
    set_str(out.name, *nm);

    set_str(out.surface, root["surface"].value_or(kOpenPbrSurface));

    const auto bd = root["body"].value<std::string_view>();
    if (!bd || bd->empty()) { return TechniqueCookError::MissingBody; }
    if (bd->starts_with("builtin:"))
    {
        out.body_kind = TechniqueBodyKind::Builtin;
        set_str(out.body, bd->substr(8));
    }
    else if (bd->starts_with("crd://"))
    {
        out.body_kind = TechniqueBodyKind::Graph;
        set_str(out.body, *bd);
    }
    else
    {
        set_where(where, *bd);
        return TechniqueCookError::MissingBody;
    }
    if (out.body.size() == 0U) { set_where(where, *bd); return TechniqueCookError::MissingBody; }

    if (const auto* arr = root["binding"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return TechniqueCookError::ParseFailed; }
            TechniqueBindingDesc b(alloc);
            const auto bn = (*t)["name"].value<std::string_view>();
            if (!bn || bn->empty()) { return TechniqueCookError::MissingName; }
            set_str(b.name, *bn);
            for (crd::usize i = 0; i < out.bindings.size(); ++i)
            {
                if (str_eq(out.bindings[i].name, *bn)) { set_where(where, *bn); return TechniqueCookError::DuplicateBinding; }
            }
            const auto ty = (*t)["type"].value<std::string_view>();
            bool       is_array = false;
            if (!ty || !to_bind_type(*ty, b.type, is_array))
            {
                set_where(where, ty ? *ty : *bn);
                return TechniqueCookError::UnknownBindType;
            }
            const auto fq = (*t)["frequency"].value_or(std::string_view{"pass"});
            if (!to_frequency(fq, b.freq)) { set_where(where, fq); return TechniqueCookError::UnknownFrequency; }
            b.count = static_cast<crd::u32>((*t)["count"].value_or<int64_t>(is_array ? 0 : 1));
            out.bindings.push_back(static_cast<TechniqueBindingDesc&&>(b));
        }
    }

    if (const auto* arr = root["option"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return TechniqueCookError::ParseFailed; }
            TechniqueOptionDesc o(alloc);
            const auto on = (*t)["name"].value<std::string_view>();
            if (!on || on->empty()) { return TechniqueCookError::MissingName; }
            set_str(o.name, *on);
            for (crd::usize i = 0; i < out.options.size(); ++i)
            {
                if (str_eq(out.options[i].name, *on)) { set_where(where, *on); return TechniqueCookError::DuplicateOption; }
            }
            o.min_value     = static_cast<int>((*t)["min"].value_or<int64_t>(0));
            o.max_value     = static_cast<int>((*t)["max"].value_or<int64_t>(0));
            o.default_value = static_cast<int>((*t)["default"].value_or<int64_t>(o.min_value));
            out.options.push_back(static_cast<TechniqueOptionDesc&&>(o));
        }
    }

    return validate_technique(out, where);
}

TechniqueCookError validate_technique(const TechniqueDesc& desc, crd::containers::String* where)
{
    if (desc.schema != kTechniqueSchemaVersion) { return TechniqueCookError::BadSchema; }
    if (desc.name.size() == 0U) { return TechniqueCookError::MissingName; }
    if (desc.body.size() == 0U) { return TechniqueCookError::MissingBody; }
    if (!str_eq(desc.surface, kOpenPbrSurface))
    {
        set_where(where, sv(desc.surface));
        return TechniqueCookError::UnknownSurface;
    }

    for (crd::usize i = 0; i < desc.bindings.size(); ++i)
    {
        const TechniqueBindingDesc& b = desc.bindings[i];
        if (b.name.size() == 0U) { return TechniqueCookError::MissingName; }
        for (crd::usize k = 0; k < i; ++k)
        {
            if (str_eq(desc.bindings[k].name, sv(b.name)))
            {
                set_where(where, sv(b.name));
                return TechniqueCookError::DuplicateBinding;
            }
        }
        const bool is_array = (b.type == BindType::FloatArray || b.type == BindType::Mat4Array);
        // An array with count < 2 is not an array, and a scalar with a count is a lie about its shape. Both would
        // silently produce the wrong ABI slot count when the body is spliced, so both are rejected BY NAME.
        if (is_array != (b.count > 1U))
        {
            set_where(where, sv(b.name));
            return TechniqueCookError::BadArrayCount;
        }
    }

    for (crd::usize i = 0; i < desc.options.size(); ++i)
    {
        const TechniqueOptionDesc& o = desc.options[i];
        if (o.name.size() == 0U) { return TechniqueCookError::MissingName; }
        for (crd::usize k = 0; k < i; ++k)
        {
            if (str_eq(desc.options[k].name, sv(o.name)))
            {
                set_where(where, sv(o.name));
                return TechniqueCookError::DuplicateOption;
            }
        }
        if (o.min_value > o.max_value || o.default_value < o.min_value || o.default_value > o.max_value)
        {
            set_where(where, sv(o.name));
            return TechniqueCookError::BadOptionRange;
        }
    }
    return TechniqueCookError::Ok;
}

crd::containers::String emit_technique_toml(const TechniqueDesc& desc, crd::memory::IAllocator* a)
{
    crd::containers::String out(a);
    const auto put = [&](const char* s) { out.append(s); };
    const auto put_sv = [&](const crd::containers::String& s) { out.append(s.c_str()); };
    const auto put_int = [&](long long v) {
        char  buf[24];
        int   n   = 0;
        bool  neg = v < 0;
        unsigned long long u = neg ? static_cast<unsigned long long>(-v) : static_cast<unsigned long long>(v);
        if (u == 0U) { buf[n++] = '0'; }
        while (u > 0U) { buf[n++] = static_cast<char>('0' + (u % 10U)); u /= 10U; }
        if (neg) { buf[n++] = '-'; }
        for (int i = n - 1; i >= 0; --i) { const char one[2] = {buf[i], '\0'}; out.append(static_cast<const char*>(one)); }
    };

    put("schema = ");
    put_int(static_cast<long long>(desc.schema));
    put("\nname = \"");
    put_sv(desc.name);
    put("\"\nsurface = \"");
    put_sv(desc.surface);
    put("\"\nbody = \"");
    if (desc.body_kind == TechniqueBodyKind::Builtin) { put("builtin:"); }
    put_sv(desc.body);
    put("\"\n");

    for (crd::usize i = 0; i < desc.bindings.size(); ++i)
    {
        const TechniqueBindingDesc& b = desc.bindings[i];
        put("\n[[binding]]\nname = \"");
        put_sv(b.name);
        put("\"\ntype = \"");
        put(crd::kir::technique::bind_type_text(b.type));
        put("\"\nfrequency = \"");
        put(crd::kir::technique::bind_frequency_text(b.freq));
        put("\"\n");
        if (b.count > 1U)
        {
            put("count = ");
            put_int(static_cast<long long>(b.count));
            put("\n");
        }
    }
    for (crd::usize i = 0; i < desc.options.size(); ++i)
    {
        const TechniqueOptionDesc& o = desc.options[i];
        put("\n[[option]]\nname = \"");
        put_sv(o.name);
        put("\"\nmin = ");
        put_int(o.min_value);
        put("\nmax = ");
        put_int(o.max_value);
        put("\ndefault = ");
        put_int(o.default_value);
        put("\n");
    }
    return out;
}

crd::containers::Array<crd::u8> cook_technique(const TechniqueDesc& desc, crd::memory::IAllocator* a)
{
    Bytes out(a);
    put_u32(out, kFourCC);
    put_u32(out, kBlobVersion);
    put_u32(out, desc.schema);
    put_str(out, desc.name);
    put_str(out, desc.surface);
    put_u8(out, static_cast<crd::u8>(desc.body_kind));
    put_str(out, desc.body);

    put_u32(out, static_cast<crd::u32>(desc.bindings.size()));
    for (crd::usize i = 0; i < desc.bindings.size(); ++i)
    {
        const TechniqueBindingDesc& b = desc.bindings[i];
        put_str(out, b.name);
        put_u8(out, static_cast<crd::u8>(b.type));
        put_u8(out, static_cast<crd::u8>(b.freq));
        put_u32(out, b.count);
    }
    put_u32(out, static_cast<crd::u32>(desc.options.size()));
    for (crd::usize i = 0; i < desc.options.size(); ++i)
    {
        const TechniqueOptionDesc& o = desc.options[i];
        put_str(out, o.name);
        put_i32(out, o.min_value);
        put_i32(out, o.max_value);
        put_i32(out, o.default_value);
    }
    return out;
}

bool read_technique(crd::containers::ConstSpan<crd::u8> bytes, TechniqueDesc& out)
{
    Cursor c{bytes, 0, true};
    if (c.u32v() != kFourCC || !c.ok) { return false; }
    if (c.u32v() != kBlobVersion) { return false; }
    out.schema = c.u32v();
    c.strv(out.name);
    c.strv(out.surface);
    out.body_kind = static_cast<TechniqueBodyKind>(c.u8v());
    c.strv(out.body);

    auto*          alloc = out.bindings.allocator();
    const crd::u32 nb    = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < nb; ++i)
    {
        TechniqueBindingDesc b(alloc);
        c.strv(b.name);
        b.type  = static_cast<BindType>(c.u8v());
        b.freq  = static_cast<BindFrequency>(c.u8v());
        b.count = c.u32v();
        out.bindings.push_back(static_cast<TechniqueBindingDesc&&>(b));
    }
    const crd::u32 no = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < no; ++i)
    {
        TechniqueOptionDesc o(alloc);
        c.strv(o.name);
        o.min_value     = c.i32v();
        o.max_value     = c.i32v();
        o.default_value = c.i32v();
        out.options.push_back(static_cast<TechniqueOptionDesc&&>(o));
    }
    return c.ok;
}

TechniqueCookError verify_technique_bindings(const TechniqueDesc& tech,
                                             const crd::framecook::FrameGraphDesc& graph,
                                             const crd::framecook::FramePassDesc&  pass,
                                             crd::containers::String* where)
{
    for (crd::usize i = 0; i < tech.bindings.size(); ++i)
    {
        const TechniqueBindingDesc& b = tech.bindings[i];
        // Only PASS frequency is the graph author's responsibility. Frame/material/object inputs are the engine's
        // to supply and are checked by the renderer's resolver instead — asking the graph to declare them would
        // make every asset restate the camera.
        if (b.freq != BindFrequency::Pass) { continue; }
        // ⛔ AND only the RESOURCE-class ones. A frame graph's `reads` list describes RESOURCE FLOW — it is what
        // the lifetime analysis, the barriers and the aliasing allocator are derived from. A pass-frequency VALUE
        // (`csm_light_vp`, `csm_map_size`) is not a graph resource at all; it is engine state the renderer's
        // resolver supplies, and requiring the asset to list it would put non-resources into the dependency graph
        // and make every one of them a phantom node the cooker would then reject as never-written.
        //
        // Value bindings are still CHECKED, just one layer down: `resolve_scene_bindings` returns false for a
        // declared binding the renderer cannot supply, and `init_programs` fails. What is verified HERE is the
        // half only the graph can get wrong — and, crucially, the half that fails SILENTLY if unchecked.
        if (!crd::kir::technique::bind_type_is_texture(b.type)) { continue; }

        const crd::framecook::FrameResourceDesc* res = nullptr;
        bool                                     found = false;
        for (crd::usize k = 0; k < pass.reads.size(); ++k)
        {
            if (!str_eq(pass.reads[k].name, sv(b.name))) { continue; }
            found = true;
            for (crd::usize r = 0; r < graph.resources.size(); ++r)
            {
                if (str_eq(graph.resources[r].name, sv(b.name))) { res = &graph.resources[r]; break; }
            }
            break;
        }
        if (!found)
        {
            set_where(where, sv(b.name));
            return TechniqueCookError::PassMissingBinding;
        }
        // ⛔ SHAPE, not just presence. A `texture2DArrayShadow` wired to a single-layer resource compiles, binds
        // and renders — every cascade from slice 0. That is the exact degenerate failure the REN-3.2 device gate
        // was built to catch, and catching it HERE moves it from "looks like art direction" to a named rejection.
        if (b.type == BindType::Texture2DArrayShadow || b.type == BindType::Texture2DArray)
        {
            if (res == nullptr || res->layers <= 1U)
            {
                set_where(where, sv(b.name));
                return TechniqueCookError::PassBindingNotLayered;
            }
        }
    }
    return TechniqueCookError::Ok;
}

crd::kir::technique::Technique
make_runtime_technique(const TechniqueDesc& desc,
                       crd::containers::Array<crd::kir::technique::TechniqueBinding>& scratch_bindings,
                       crd::containers::Array<crd::kir::technique::TechniqueOption>&  scratch_options,
                       crd::kir::technique::TechniqueBody body, void* user, const crd::u8* blob, crd::u64 blob_size)
{
    scratch_bindings.clear();
    for (crd::usize i = 0; i < desc.bindings.size(); ++i)
    {
        crd::kir::technique::TechniqueBinding b;
        b.name  = desc.bindings[i].name.c_str();
        b.type  = desc.bindings[i].type;
        b.freq  = desc.bindings[i].freq;
        b.count = desc.bindings[i].count;
        scratch_bindings.push_back(b);
    }
    scratch_options.clear();
    for (crd::usize i = 0; i < desc.options.size(); ++i)
    {
        crd::kir::technique::TechniqueOption o;
        o.name          = desc.options[i].name.c_str();
        o.min_value     = desc.options[i].min_value;
        o.max_value     = desc.options[i].max_value;
        o.default_value = desc.options[i].default_value;
        scratch_options.push_back(o);
    }

    crd::kir::technique::Technique t;
    t.name       = desc.name.c_str();
    t.bindings   = scratch_bindings.data();
    t.n_bindings = static_cast<int>(scratch_bindings.size());
    t.options    = scratch_options.data();
    t.n_options  = static_cast<int>(scratch_options.size());
    if (desc.body_kind == TechniqueBodyKind::Builtin)
    {
        t.body = body;
        t.user = user;
    }
    else
    {
        t.blob      = blob;
        t.blob_size = blob_size;
    }
    return t;
}

} // namespace crd::techniquecook
