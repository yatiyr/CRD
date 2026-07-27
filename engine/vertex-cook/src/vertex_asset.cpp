// vertex_asset.cpp — REN-38-D1/D2/D3/D4: parse, validate, emit and COOK a `.crdv`.
//
// ⛔ THE VERTEX PROGRAM WAS ~200 LINES OF C++ with the pull layout compiled in. This file is the replacement: the
// header word map, the vertex record, the instance record, skinning, morphing, displacement, the transform and the
// varying set are all DECLARED, and the cook builds the same CKIR `KEntry` the hand-written builders returned.

#include <crd/vertexcook/vertex_asset.hpp>

#include <crd/kir/ckir_shape.hpp> // REN-38 audit: the cook refuses a shape-invalid entry by name
#include <crd/matcook/material_asset.hpp>

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace crd::vertcook
{
namespace
{

using crd::kir::DType;
using crd::kir::KGraph;
using crd::kir::KOp;

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
[[nodiscard]] std::string_view sv_of(const crd::containers::String& s) noexcept
{
    return {s.c_str(), s.size()};
}
[[nodiscard]] bool str_eq(const crd::containers::String& a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.c_str(), b.data(), b.size()) == 0;
}

// ── The IR-building helper: the pull idiom, once. ────────────────────────────────────────────────────────────
// ⛔ The storage buffer is a flat u32 array, so every float is a BIT REINTERPRETATION of the word — not a cast.
// `loadf` reading through `cast(…, F32)` would convert the integer VALUE and produce geometry that is simply the
// bit pattern read as a number: enormous, or zero, never subtly wrong. Kept in one place so it cannot drift.
struct Vx
{
    KGraph&         g;
    crd::kir::Shape sh;

    explicit Vx(KGraph& graph) : g(graph), sh(crd::kir::make_shape({1})) {}

    [[nodiscard]] int kf(double v) { return g.constant(v, sh, DType::F32); }
    [[nodiscard]] int ku(crd::u32 v) { return g.constant(static_cast<double>(v), sh, DType::U32); }
    [[nodiscard]] int add(int a, int b) { return g.binary(KOp::Add, a, b); }
    [[nodiscard]] int sub(int a, int b) { return g.binary(KOp::Sub, a, b); }
    [[nodiscard]] int mul(int a, int b) { return g.binary(KOp::Mul, a, b); }
    [[nodiscard]] int dvd(int a, int b) { return g.binary(KOp::Div, a, b); }
    // ⭐⭐ REN-38 (scene-buffer consolidation): `base >= 0` rebases EVERY load by the draw's region base —
    // the ONE choke point that turns "each group owns a buffer" into "one scene buffer with per-group
    // regions". Header offsets stored IN the region stay region-relative, so the chain
    // `loadu(add(hdru(index_off), i))` lands at base + index_off_value + i with no other change anywhere.
    int               base = -1;
    [[nodiscard]] int loadu(int idx) { return g.storage_load(base >= 0 ? add(base, idx) : idx); }
    [[nodiscard]] int loadf(int idx) { return g.int_bits_to_float(g.cast(loadu(idx), DType::I32)); }
    [[nodiscard]] int hdru(crd::u32 word) { return loadu(ku(word)); }
    [[nodiscard]] int hdrf(crd::u32 word) { return loadf(ku(word)); }

    // out = M · (x, y, z, w), M column-major in `m[16]`.
    void mul_mat4(const int m[16], int x, int y, int z, int w, int out[4])
    {
        const int v[4] = {x, y, z, w};
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            int acc = mul(m[0U * 4U + i], v[0]);
            acc     = add(acc, mul(m[1U * 4U + i], v[1]));
            acc     = add(acc, mul(m[2U * 4U + i], v[2]));
            acc     = add(acc, mul(m[3U * 4U + i], v[3]));
            out[i]  = acc;
        }
    }
    // out = M3x3 · d — the ROTATION part only. ⛔ Using the full 4×4 on a direction adds the translation column,
    // so a normal drifts with the instance's world position: lighting that follows the object around, which reads
    // as a broken material rather than a broken transform.
    void mul_mat3(const int m[16], int x, int y, int z, int out[3])
    {
        const int v[3] = {x, y, z};
        for (crd::u32 i = 0; i < 3U; ++i)
        {
            int acc = mul(m[0U * 4U + i], v[0]);
            acc     = add(acc, mul(m[1U * 4U + i], v[1]));
            acc     = add(acc, mul(m[2U * 4U + i], v[2]));
            out[i]  = acc;
        }
    }
    void cross(const int a[3], const int b[3], int out[3])
    {
        out[0] = sub(mul(a[1], b[2]), mul(a[2], b[1]));
        out[1] = sub(mul(a[2], b[0]), mul(a[0], b[2]));
        out[2] = sub(mul(a[0], b[1]), mul(a[1], b[0]));
    }
    [[nodiscard]] int vecn(const int* c, crd::u32 n)
    {
        if (n <= 1U) { return c[0]; }
        if (n == 2U) { return g.vec2(c[0], c[1]); }
        if (n == 3U) { return g.vec3(c[0], c[1], c[2]); }
        return g.vec4(c[0], c[1], c[2], c[3]);
    }
};

// A pulled attribute, in both spaces. ⛔ Both are kept because a varying may legitimately want either — a
// tangent-space effect reads the OBJECT normal while lighting reads the WORLD one, and silently supplying one for
// the other is a picture that looks lit from the wrong place only when the object rotates.
struct AttrVals
{
    int      obj[4] = {-1, -1, -1, -1};
    int      wld[4] = {-1, -1, -1, -1};
    crd::u32 comps  = 0U;
};

[[nodiscard]] crd::i32 find_attr(const crd::containers::Array<VertexAttrDesc>& a, std::string_view n) noexcept
{
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        if (str_eq(a[i].name, n)) { return static_cast<crd::i32>(i); }
    }
    return -1;
}

} // namespace

// ── PARSE ─────────────────────────────────────────────────────────────────────────────────────────────────
namespace
{
void read_value(const toml::node& n, double* v, crd::u32& comps)
{
    if (const auto* arr = n.as_array())
    {
        crd::u32 i = 0;
        for (const auto& e : *arr)
        {
            if (i < 4U) { v[i++] = e.value_or<double>(0.0); }
        }
        comps = i > 0U ? i : 1U;
        return;
    }
    v[0]  = n.value_or<double>(0.0);
    comps = 1U;
}

[[nodiscard]] AttrKind attr_kind_of(std::string_view s) noexcept
{
    if (s == "position") { return AttrKind::Position; }
    if (s == "direction") { return AttrKind::Direction; }
    return AttrKind::Value;
}
[[nodiscard]] const char* attr_kind_name(AttrKind k) noexcept
{
    switch (k)
    {
    case AttrKind::Position:  return "position";
    case AttrKind::Direction: return "direction";
    case AttrKind::Value:     break;
    }
    return "value";
}
[[nodiscard]] const char* skin_scheme_name(SkinScheme s) noexcept
{
    switch (s)
    {
    case SkinScheme::LinearBlend:    return "linear_blend";
    case SkinScheme::DualQuaternion: return "dual_quaternion";
    case SkinScheme::None:           break;
    }
    return "none";
}
[[nodiscard]] StageKind stage_of(std::string_view s) noexcept
{
    if (s == "tess_control") { return StageKind::TessControl; }
    if (s == "tess_eval") { return StageKind::TessEval; }
    if (s == "task") { return StageKind::Task; }
    if (s == "mesh") { return StageKind::Mesh; }
    if (s == "visbuffer") { return StageKind::VisBuffer; }
    if (s == "cull") { return StageKind::Cull; }
    if (s == "raygen") { return StageKind::RayGen; }
    if (s == "closest_hit") { return StageKind::ClosestHit; }
    if (s == "miss") { return StageKind::Miss; }
    if (s == "any_hit") { return StageKind::AnyHit; }
    if (s == "intersection") { return StageKind::Intersection; }
    if (s == "callable") { return StageKind::CallableStage; }
    return StageKind::Vertex;
}
[[nodiscard]] const char* stage_name(StageKind k) noexcept
{
    switch (k)
    {
    case StageKind::TessControl: return "tess_control";
    case StageKind::TessEval:    return "tess_eval";
    case StageKind::Task:        return "task";
    case StageKind::Mesh:        return "mesh";
    case StageKind::VisBuffer:   return "visbuffer";
    case StageKind::Cull:        return "cull";
    case StageKind::RayGen:      return "raygen";
    case StageKind::ClosestHit:  return "closest_hit";
    case StageKind::Miss:        return "miss";
    case StageKind::AnyHit:      return "any_hit";
    case StageKind::Intersection: return "intersection";
    case StageKind::CallableStage: return "callable";
    case StageKind::Vertex:      break;
    }
    return "vertex";
}
// ⭐ The stages that PULL geometry (and therefore need a position attribute and the record). Ray-tracing and
// culling stages do not — requiring one of them would refuse a perfectly valid raygen. ⛔ REN-38-F6: neither do
// the tessellation, mesh or task stages — a hull passes the patch through, a domain displaces the emitter's
// bilerped patch point, a mesh stage generates its grid procedurally and a task stage only amplifies. Listing
// them here forced a position attribute on declarations that have no vertex record to read one from.
[[nodiscard]] bool stage_pulls(StageKind k) noexcept
{
    return k == StageKind::Vertex || k == StageKind::VisBuffer;
}
[[nodiscard]] const char* transform_name(VertexTransform t) noexcept
{
    switch (t)
    {
    case VertexTransform::LightVp: return "light_vp";
    case VertexTransform::None:    return "none";
    case VertexTransform::ViewProj: break;
    }
    return "view_proj";
}

// A varying source term is written as one string: `position` (object), `world:position`, `instance:color`,
// `clip.w`, `node:<name>`. ⛔ PREFIXED rather than guessed from context — the same spelling meaning "the
// attribute" in one asset and "the node" in another is exactly the ambiguity `.crdm`'s `$` prefix removed.
void parse_source_term(std::string_view s, VaryingSource& out)
{
    const auto starts = [&](const char* p) {
        const crd::usize n = std::strlen(p);
        return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
    };
    if (s == "clip.w")
    {
        out.kind  = VaryingSourceKind::ClipW;
        out.comps = 1U;
        return;
    }
    if (starts("world:"))
    {
        out.kind = VaryingSourceKind::World;
        set_str(out.name, s.substr(6));
        return;
    }
    if (starts("instance:"))
    {
        out.kind = VaryingSourceKind::Instance;
        set_str(out.name, s.substr(9));
        return;
    }
    if (starts("node:"))
    {
        out.kind = VaryingSourceKind::Node;
        set_str(out.name, s.substr(5));
        return;
    }
    out.kind = VaryingSourceKind::Attribute;
    set_str(out.name, s);
}

VertexCookError parse_attr_array(const toml::array& arr, crd::containers::Array<VertexAttrDesc>& out,
                                 crd::memory::IAllocator* alloc, crd::containers::String* where)
{
    for (const auto& e : arr)
    {
        const toml::table* t = e.as_table();
        if (t == nullptr) { continue; }
        VertexAttrDesc a(alloc);
        const auto     nm = (*t)["name"].value<std::string_view>();
        if (!nm || nm->empty()) { return VertexCookError::MissingName; }
        set_str(a.name, *nm);
        for (crd::usize i = 0; i < out.size(); ++i)
        {
            if (str_eq(out[i].name, *nm))
            {
                set_where(where, *nm);
                return VertexCookError::DuplicateName;
            }
        }
        a.offset = static_cast<crd::u32>((*t)["offset"].value_or<int64_t>(0));
        a.comps  = static_cast<crd::u32>((*t)["comps"].value_or<int64_t>(1));
        a.kind   = attr_kind_of((*t)["kind"].value_or<std::string_view>("value"));
        out.push_back(static_cast<VertexAttrDesc&&>(a));
    }
    return VertexCookError::Ok;
}
} // namespace

VertexCookError parse_vertex_toml(crd::containers::StringView toml_text, VertexProgramDesc& out,
                                  crd::containers::String* where)
{
    // NON-THROWING parse (TOML_EXCEPTIONS=0, the frame-cook pattern). A thrown parse_error unwinding
    // while a live GPU device (validation layer hooked into SEH dispatch) CRASHED the process — an
    // authored-asset TYPO became a process kill once disk-first loading made user edits reachable.
    // Result-checked also kills the mixed-mode ODR hazard (three cookers threw, three did not).
    toml::parse_result pr = toml::parse(std::string_view(toml_text.data(), toml_text.size()));
    if (!pr) { return VertexCookError::ParseFailed; }
    toml::table root = std::move(pr).table();
    auto* alloc = out.attrs.allocator();

    // ⛔ RESET THE OUTPUT FIRST. Parsing into a descriptor that already held a program APPENDED to it: the second
    // asset's attributes joined the first's, and a name that appeared in both came back as `DuplicateName` — an
    // error naming the wrong thing. Worse when the names differ: a silently MERGED layout that reads two assets'
    // words out of one vertex record.
    out.name.clear();
    out.attrs.clear();
    out.instance.attrs.clear();
    out.instance = InstanceLayoutDesc(alloc);
    out.skin     = SkinDesc{};
    out.morph.target_count = 0U;
    out.morph.stride       = 3U;
    out.morph.targets_apply_to.clear();
    out.nodes.clear();
    out.displace.clear();
    out.varyings.clear();
    out.header        = VertexHeaderMap{};
    out.transform     = VertexTransform::ViewProj;
    out.cascade       = 0U;
    out.vertex_stride = 12U;
    out.stage = StageKind::Vertex;
    out.tess  = TessDesc{};
    out.mesh  = MeshDesc{};
    out.task  = TaskDesc{};
    out.rt    = RtDesc{};
    out.cull  = CullDesc{};
    out.position_node.clear();
    out.expand = ExpandDesc{};

    const auto sch = root["schema"].value<int64_t>();
    if (!sch || *sch != static_cast<int64_t>(kVertexSchemaVersion)) { return VertexCookError::BadSchema; }
    out.schema = kVertexSchemaVersion;
    const auto nm = root["name"].value<std::string_view>();
    if (!nm || nm->empty()) { return VertexCookError::MissingName; }
    set_str(out.name, *nm);

    if (const auto* h = root["header"].as_table())
    {
        const auto w = [&](const char* k, crd::u32& dst) {
            if (const auto v = (*h)[k].value<int64_t>()) { dst = static_cast<crd::u32>(*v); }
        };
        w("index_count", out.header.index_count);
        w("index_off", out.header.index_off);
        w("vertex_off", out.header.vertex_off);
        w("instance_off", out.header.instance_off);
        w("visible_off", out.header.visible_off);
        w("view_proj", out.header.view_proj);
        w("light_vp", out.header.light_vp);
        w("skin_off", out.header.skin_off);
        w("palette_off", out.header.palette_off);
        w("joint_count", out.header.joint_count);
        w("morph_off", out.header.morph_off);
        w("morph_weights", out.header.morph_weights);
        w("instance_count", out.header.instance_count);
    }

    if (const auto* v = root["vertex"].as_table())
    {
        out.vertex_stride = static_cast<crd::u32>((*v)["stride"].value_or<int64_t>(12));
    }
    if (const auto* aa = root["attribute"].as_array())
    {
        const VertexCookError e = parse_attr_array(*aa, out.attrs, alloc, where);
        if (e != VertexCookError::Ok) { return e; }
    }

    if (const auto* i = root["instance"].as_table())
    {
        out.instance.stride    = static_cast<crd::u32>((*i)["stride"].value_or<int64_t>(20));
        out.instance.transform = static_cast<crd::u32>((*i)["transform"].value_or<int64_t>(0));
        if (const auto nt = (*i)["normal_transform"].value<int64_t>())
        {
            out.instance.has_normal_transform = true;
            out.instance.normal_transform     = static_cast<crd::u32>(*nt);
        }
    }
    if (const auto* ia = root["instance_attribute"].as_array())
    {
        const VertexCookError e = parse_attr_array(*ia, out.instance.attrs, alloc, where);
        if (e != VertexCookError::Ok) { return e; }
    }

    if (const auto* s = root["skin"].as_table())
    {
        const auto scheme = (*s)["scheme"].value_or<std::string_view>("none");
        out.skin.scheme   = SkinScheme::None;
        if (scheme == "linear_blend") { out.skin.scheme = SkinScheme::LinearBlend; }
        else if (scheme == "dual_quaternion") { out.skin.scheme = SkinScheme::DualQuaternion; }
        out.skin.influences     = static_cast<crd::u32>((*s)["influences"].value_or<int64_t>(4));
        out.skin.stride         = static_cast<crd::u32>((*s)["stride"].value_or<int64_t>(6));
        out.skin.joint_words    = static_cast<crd::u32>((*s)["joint_words"].value_or<int64_t>(2));
        out.skin.weight_off     = static_cast<crd::u32>((*s)["weight_off"].value_or<int64_t>(2));
        out.skin.palette_stride = static_cast<crd::u32>(
            (*s)["palette_stride"].value_or<int64_t>(out.skin.scheme == SkinScheme::DualQuaternion ? 8 : 16));
    }

    if (const auto* m = root["morph"].as_table())
    {
        out.morph.target_count = static_cast<crd::u32>((*m)["targets"].value_or<int64_t>(0));
        out.morph.stride       = static_cast<crd::u32>((*m)["stride"].value_or<int64_t>(3));
        if (const auto* ap = (*m)["apply_to"].as_array())
        {
            for (const auto& e : *ap)
            {
                crd::containers::String s(alloc);
                set_str(s, e.value_or<std::string_view>(""));
                out.morph.targets_apply_to.push_back(static_cast<crd::containers::String&&>(s));
            }
        }
    }

    out.stage = stage_of(root["stage"].value_or<std::string_view>("vertex"));
    if (const auto* t = root["tess"].as_table())
    {
        out.tess.patch_size = static_cast<crd::u32>((*t)["patch_size"].value_or<int64_t>(4));
        out.tess.inner      = (*t)["inner"].value_or<double>(8.0);
        out.tess.outer      = (*t)["outer"].value_or<double>(8.0);
    }
    if (const auto* m = root["mesh"].as_table())
    {
        out.mesh.max_vertices   = static_cast<crd::u32>((*m)["max_vertices"].value_or<int64_t>(64));
        out.mesh.max_primitives = static_cast<crd::u32>((*m)["max_primitives"].value_or<int64_t>(124));
        out.mesh.workgroup      = static_cast<crd::u32>((*m)["workgroup"].value_or<int64_t>(64));
        out.mesh.fetch          = (*m)["fetch"].value_or<bool>(false);   // REN-38-F6+: pull real geometry
        out.mesh.payload        = (*m)["payload"].value_or<bool>(false); // REN-38-F6+: dispatched by a task
    }
    if (const auto* t = root["task"].as_table())
    {
        out.task.emit        = static_cast<crd::u32>((*t)["emit"].value_or<int64_t>(1));
        out.task.emit_header = static_cast<crd::i32>((*t)["emit_header"].value_or<int64_t>(-1)); // REN-38-F6+
    }
    // ⭐ REN-38-F7: the PROCEDURAL vertex mode — a node-computed clip position plus the expansion contract.
    // `position = "node:<name>"` (root key); anything else after `node:` semantics would be a second spelling.
    if (const auto pn = root["position"].value<std::string_view>())
    {
        const std::string_view pv = *pn;
        if (pv.size() <= 5U || pv.substr(0, 5) != std::string_view("node:"))
        {
            set_where(where, pv);
            return VertexCookError::BadPositionNode;
        }
        set_str(out.position_node, pv.substr(5));
    }
    if (const auto* ex = root["expand"].as_table())
    {
        out.expand.verts_per_instance = static_cast<crd::u32>((*ex)["verts_per_instance"].value_or<int64_t>(1));
        out.expand.instance_words     = static_cast<crd::u32>((*ex)["instance_words"].value_or<int64_t>(0));
        out.expand.instance_off       = static_cast<crd::u32>((*ex)["instance_off"].value_or<int64_t>(32));
        if ((*ex)["category_field"] || (*ex)["category_mask_word"])
        {
            out.expand.has_category       = true;
            out.expand.category_field     = static_cast<crd::u32>((*ex)["category_field"].value_or<int64_t>(0));
            out.expand.category_mask_word = static_cast<crd::u32>((*ex)["category_mask_word"].value_or<int64_t>(0));
        }
    }
    if (const auto* r = root["rt"].as_table())
    {
        out.rt.payload_words = static_cast<crd::u32>((*r)["payload_words"].value_or<int64_t>(1));
        out.rt.as_set        = static_cast<crd::u32>((*r)["as_set"].value_or<int64_t>(0));
        out.rt.as_binding    = static_cast<crd::u32>((*r)["as_binding"].value_or<int64_t>(0));
        out.rt.out_binding   = static_cast<crd::u32>((*r)["out_binding"].value_or<int64_t>(1));
        out.rt.alpha_cutoff  = (*r)["alpha_cutoff"].value_or<double>(0.0);
        out.rt.sphere_radius  = (*r)["sphere_radius"].value_or<double>(0.5);
        out.rt.callable_scale = (*r)["callable_scale"].value_or<double>(2.0);
        out.rt.callable_bias  = (*r)["callable_bias"].value_or<double>(1.0);
        out.rt.use_callable   = (*r)["use_callable"].value_or<bool>(false);
    }
    if (const auto* cu = root["cull"].as_table())
    {
        out.cull.frustum   = (*cu)["frustum"].value_or<bool>(true);
        out.cull.workgroup = static_cast<crd::u32>((*cu)["workgroup"].value_or<int64_t>(64));
        out.cull.args_off  = static_cast<crd::u32>((*cu)["args_off"].value_or<int64_t>(0));
    }

    const auto tr = root["transform"].value_or<std::string_view>("view_proj");
    out.transform = VertexTransform::ViewProj;
    if (tr == "light_vp") { out.transform = VertexTransform::LightVp; }
    else if (tr == "none") { out.transform = VertexTransform::None; }
    out.cascade = static_cast<crd::u32>(root["cascade"].value_or<int64_t>(0));
    // ⭐⭐ REN-38: `rebase_table = <word>` — the DRAW-TABLE offset. Non-zero makes the pull VS read its region
    // base from `sbuf[rebase_table + DrawIndex]` and rebase every load by it (the scene-buffer consolidation).
    out.rebase_table = static_cast<crd::u32>(root["rebase_table"].value_or<int64_t>(0));

    if (const auto* nt = root["node"].as_array())
    {
        for (const auto& e : *nt)
        {
            const toml::table* t = e.as_table();
            if (t == nullptr) { continue; }
            VertNodeDesc n(alloc);
            const auto   nn = (*t)["name"].value<std::string_view>();
            const auto   op = (*t)["op"].value<std::string_view>();
            if (!nn || nn->empty() || !op || op->empty()) { return VertexCookError::MissingName; }
            set_str(n.name, *nn);
            set_str(n.op, *op);
            for (crd::usize i = 0; i < out.nodes.size(); ++i)
            {
                if (str_eq(out.nodes[i].name, *nn))
                {
                    set_where(where, *nn);
                    return VertexCookError::DuplicateName;
                }
            }
            if (const auto* ia = (*t)["inputs"].as_array())
            {
                for (const auto& ie : *ia)
                {
                    VertInput in(alloc);
                    // ⛔ `@name` is a VERTEX ATTRIBUTE, a bare name is another node. Prefixed, for the same
                    // reason `.crdm` prefixes a parameter: one spelling must not mean two things.
                    // ⭐ REN-38-F7: the PROCEDURAL spellings — expansion indices (`@corner`/`@instance`/
                    // `@category`), instance-record words (`field:`/`fieldu:`/`fieldc:`) and header words
                    // (`hdr:`/`hdru:`/`hdrc:`). Prefixed like everything else, and validated against the
                    // declared [expand] contract below.
                    if (const auto s = ie.value<std::string_view>())
                    {
                        const auto pref = [&](const char* p) {
                            const std::string_view pv(p);
                            return s->size() > pv.size() && s->starts_with(pv);
                        };
                        const auto word_of = [&](crd::usize skip) {
                            crd::u32 w = 0U;
                            for (crd::usize ci = skip; ci < s->size(); ++ci)
                            {
                                const char ch = (*s)[ci];
                                if (ch < '0' || ch > '9') { return ~0U; }
                                w = w * 10U + static_cast<crd::u32>(ch - '0');
                            }
                            return w;
                        };
                        if (*s == "@corner") { in.kind = VertInputKind::Corner; }
                        else if (*s == "@instance") { in.kind = VertInputKind::Instance; }
                        else if (*s == "@category") { in.kind = VertInputKind::Category; }
                        else if (pref("field:")) { in.kind = VertInputKind::Field; in.word = word_of(6U); }
                        else if (pref("fieldu:")) { in.kind = VertInputKind::FieldU; in.word = word_of(7U); }
                        else if (pref("fieldc:")) { in.kind = VertInputKind::FieldC; in.word = word_of(7U); }
                        else if (pref("hdr:")) { in.kind = VertInputKind::Hdr; in.word = word_of(4U); }
                        else if (pref("hdru:")) { in.kind = VertInputKind::HdrU; in.word = word_of(5U); }
                        else if (pref("hdrc:")) { in.kind = VertInputKind::HdrC; in.word = word_of(5U); }
                        else if (!s->empty() && (*s)[0] == '@')
                        {
                            in.kind = VertInputKind::Attribute;
                            set_str(in.name, s->substr(1));
                        }
                        else
                        {
                            in.kind = VertInputKind::Node;
                            set_str(in.name, *s);
                        }
                        // a malformed word index (`field:abc`) is a named error, not a silent word 0
                        if (in.word == ~0U)
                        {
                            set_where(where, *s);
                            return VertexCookError::BadExpand;
                        }
                    }
                    else
                    {
                        in.kind = VertInputKind::Literal;
                        read_value(ie, static_cast<double*>(in.value), in.comps);
                    }
                    n.inputs.push_back(static_cast<VertInput&&>(in));
                }
            }
            out.nodes.push_back(static_cast<VertNodeDesc&&>(n));
        }
    }
    if (const auto d = root["displace"].value<std::string_view>()) { set_str(out.displace, *d); }

    if (const auto* va = root["varying"].as_array())
    {
        for (const auto& e : *va)
        {
            const toml::table* t = e.as_table();
            if (t == nullptr) { continue; }
            VaryingDesc v(alloc);
            const auto  vn = (*t)["name"].value<std::string_view>();
            if (!vn || vn->empty()) { return VertexCookError::MissingName; }
            set_str(v.name, *vn);
            for (crd::usize i = 0; i < out.varyings.size(); ++i)
            {
                if (str_eq(out.varyings[i].name, *vn))
                {
                    set_where(where, *vn);
                    return VertexCookError::DuplicateName;
                }
            }
            v.location = static_cast<crd::u32>((*t)["location"].value_or<int64_t>(0));
            v.flat     = (*t)["interp"].value_or<std::string_view>("smooth") == "flat";
            if (const auto* sa = (*t)["source"].as_array())
            {
                for (const auto& se : *sa)
                {
                    VaryingSource s(alloc);
                    parse_source_term(se.value_or<std::string_view>(""), s);
                    s.comps = static_cast<crd::u32>(se.value_or<int64_t>(0)); // unused for strings
                    s.comps = 0U;
                    v.source.push_back(static_cast<VaryingSource&&>(s));
                }
            }
            // A `node:` term must state its width (see the header note); `node_comps` lists them in term order.
            if (const auto* nc = (*t)["node_comps"].as_array())
            {
                crd::usize k = 0;
                for (const auto& ce : *nc)
                {
                    while (k < v.source.size() && v.source[k].kind != VaryingSourceKind::Node) { ++k; }
                    if (k >= v.source.size()) { break; }
                    v.source[k].comps = static_cast<crd::u32>(ce.value_or<int64_t>(0));
                    ++k;
                }
            }
            out.varyings.push_back(static_cast<VaryingDesc&&>(v));
        }
    }

    return validate_vertex_program(out, where);
}

// ── VALIDATE ──────────────────────────────────────────────────────────────────────────────────────────────
crd::u32 varying_width(const VertexProgramDesc& desc, crd::u32 varying_index) noexcept
{
    if (varying_index >= desc.varyings.size()) { return 0U; }
    const VaryingDesc& v = desc.varyings[varying_index];
    crd::u32           w = 0U;
    for (crd::usize i = 0; i < v.source.size(); ++i)
    {
        const VaryingSource& s = v.source[i];
        switch (s.kind)
        {
        case VaryingSourceKind::ClipW: w += 1U; break;
        case VaryingSourceKind::Node:  w += s.comps; break;
        case VaryingSourceKind::Instance:
        {
            const crd::i32 ai = find_attr(desc.instance.attrs, sv_of(s.name));
            w += ai < 0 ? 0U : desc.instance.attrs[static_cast<crd::usize>(ai)].comps;
            break;
        }
        case VaryingSourceKind::Attribute:
        case VaryingSourceKind::World:
        {
            const crd::i32 ai = find_attr(desc.attrs, sv_of(s.name));
            w += ai < 0 ? 0U : desc.attrs[static_cast<crd::usize>(ai)].comps;
            break;
        }
        }
    }
    return w;
}

VertexCookError validate_vertex_program(const VertexProgramDesc& desc, crd::containers::String* where)
{
    if (desc.attrs.size() > kMaxAttributes) { return VertexCookError::TooManyAttributes; }
    if (desc.varyings.size() > kMaxVaryings) { return VertexCookError::TooManyVaryings; }

    bool has_position = false;
    const auto check_attrs = [&](const crd::containers::Array<VertexAttrDesc>& a, crd::u32 stride) {
        for (crd::usize i = 0; i < a.size(); ++i)
        {
            if (a[i].comps < 1U || a[i].comps > 4U)
            {
                set_where(where, sv_of(a[i].name));
                return VertexCookError::BadComponentCount;
            }
            // ⛔ AN ATTRIBUTE THAT READS PAST THE STRIDE pulls the NEXT record's words. The geometry still draws:
            // every vertex takes its neighbour's data, which looks like a corrupt mesh rather than a bad offset.
            if (a[i].offset + a[i].comps > stride)
            {
                set_where(where, sv_of(a[i].name));
                return VertexCookError::AttrOutOfRecord;
            }
            if (a[i].kind == AttrKind::Position) { has_position = true; }
        }
        return VertexCookError::Ok;
    };
    VertexCookError e = check_attrs(desc.attrs, desc.vertex_stride);
    if (e != VertexCookError::Ok) { return e; }
    {
        const bool saw = has_position;
        e = check_attrs(desc.instance.attrs, desc.instance.stride);
        has_position = saw; // an instance attribute is never the thing being projected
        if (e != VertexCookError::Ok) { return e; }
    }
    // ⛔ The instance TRANSFORM is a mat4 — sixteen words that must fit the record, or the last rows read the
    // next instance's matrix and every object inherits a neighbour's orientation.
    if (desc.instance.transform + 16U > desc.instance.stride)
    {
        set_where(where, "instance.transform");
        return VertexCookError::AttrOutOfRecord;
    }
    if (desc.instance.has_normal_transform && desc.instance.normal_transform + 16U > desc.instance.stride)
    {
        set_where(where, "instance.normal_transform");
        return VertexCookError::AttrOutOfRecord;
    }
    // (a PROCEDURAL vertex stage — `position = "node:…"` — generates its position; no attribute to require)
    if (!has_position && desc.transform != VertexTransform::None && stage_pulls(desc.stage)
        && desc.position_node.empty())
    {
        set_where(where, sv_of(desc.name));
        return VertexCookError::NoPosition;
    }

    // ── REN-38-F: per-stage validation. ⛔ Each of these is a value that renders WRONG rather than failing:
    // a zero tess level collapses the patch to nothing, a meshlet budget above the promise writes past the
    // output arrays, an amplification factor of zero launches no work at all, and a payload width the three RT
    // stages disagree on reads the neighbouring slot.
    if (desc.stage == StageKind::TessControl || desc.stage == StageKind::TessEval)
    {
        if (desc.tess.patch_size != 4U || desc.tess.inner <= 0.0 || desc.tess.outer <= 0.0
            || desc.tess.inner > 64.0 || desc.tess.outer > 64.0)
        {
            set_where(where, "tess");
            return VertexCookError::BadTess;
        }
    }
    if (desc.stage == StageKind::Mesh)
    {
        if (desc.mesh.max_vertices == 0U || desc.mesh.max_vertices > 256U || desc.mesh.max_primitives == 0U
            || desc.mesh.max_primitives > 256U || desc.mesh.workgroup == 0U || desc.mesh.workgroup > 128U)
        {
            set_where(where, "mesh");
            return VertexCookError::BadMesh;
        }
        // ⛔ REN-38-F6: the procedural meshlet grid writes vertex tid as corner (tid % 3) of triangle (tid / 3),
        // so the budgets must agree — vertices past 3·primitives are emitted but never indexed, and primitives
        // past vertices/3 index vertices that never existed (a device-UB read inside the workgroup output).
        if (desc.mesh.max_vertices != 3U * desc.mesh.max_primitives)
        {
            set_where(where, "mesh");
            return VertexCookError::BadMesh;
        }
        // ── REN-38-F6+ meshlet FETCH: pulling needs a vertex record and a position to pull, and the fetch
        // path implements NO deformers — a declared morph/skin would be silently ignored, which is exactly
        // the class of quiet loss this cooker exists to refuse.
        if (desc.mesh.fetch)
        {
            bool has_pos = false;
            for (crd::usize i = 0; i < desc.attrs.size(); ++i)
            {
                if (desc.attrs[i].kind == AttrKind::Position) { has_pos = true; }
            }
            if (desc.vertex_stride == 0U || !has_pos || desc.morph.target_count > 0U
                || desc.skin.scheme != SkinScheme::None)
            {
                set_where(where, "mesh");
                return VertexCookError::BadMesh;
            }
        }
    }
    if (desc.stage == StageKind::Task && (desc.task.emit == 0U || desc.task.emit_header > 1023))
    {
        set_where(where, "task");
        return VertexCookError::BadTask;
    }
    if (desc.stage == StageKind::RayGen || desc.stage == StageKind::ClosestHit || desc.stage == StageKind::Miss
        || desc.stage == StageKind::AnyHit || desc.stage == StageKind::Intersection
        || desc.stage == StageKind::CallableStage)
    {
        if (desc.rt.payload_words == 0U || desc.rt.payload_words > 8U)
        {
            set_where(where, "rt");
            return VertexCookError::BadRt;
        }
        // F13: a sphere of zero (or negative) radius reports no hit for any ray — refused, not rendered empty
        if (desc.stage == StageKind::Intersection && desc.rt.sphere_radius <= 0.0)
        {
            set_where(where, "rt");
            return VertexCookError::BadRt;
        }
        // REN-38 audit: an any-hit cutoff is a barycentric threshold — a value outside [0, 2] can never
        // change a hit (u + v spans 0..1 within a triangle), so the declaration is refused, not obeyed.
        if (desc.stage == StageKind::AnyHit && (desc.rt.alpha_cutoff < 0.0 || desc.rt.alpha_cutoff > 2.0))
        {
            set_where(where, "rt");
            return VertexCookError::BadRt;
        }
    }
    if (desc.stage == StageKind::Cull && (desc.cull.workgroup == 0U || desc.cull.workgroup > 256U))
    {
        set_where(where, "cull");
        return VertexCookError::BadCull;
    }

    // ⛔ A CASCADE the header cannot hold. `light_vp` is a fixed run of matrices; cascade 9 reads whatever follows
    // it — a shadow map projected by uninitialised memory, which flickers rather than failing.
    if (desc.transform == VertexTransform::LightVp && desc.cascade >= 4U)
    {
        set_where(where, sv_of(desc.name));
        return VertexCookError::BadTransform;
    }

    // ── skin
    if (desc.skin.scheme != SkinScheme::None)
    {
        if (desc.skin.influences < 1U || desc.skin.influences > kMaxInfluences
            || desc.skin.joint_words * 2U < desc.skin.influences
            || desc.skin.weight_off + desc.skin.influences > desc.skin.stride
            || desc.skin.joint_words > desc.skin.weight_off)
        {
            set_where(where, "skin");
            return VertexCookError::BadSkin;
        }
        const crd::u32 want = desc.skin.scheme == SkinScheme::DualQuaternion ? 8U : 16U;
        if (desc.skin.palette_stride != want)
        {
            set_where(where, "skin.palette_stride");
            return VertexCookError::BadSkin;
        }
    }

    // ── morph
    if (desc.morph.target_count > 0U)
    {
        if (desc.morph.target_count > kMaxMorphTargets || desc.morph.stride == 0U
            || desc.header.morph_off == 0U || desc.header.morph_weights == 0U
            || desc.morph.targets_apply_to.empty())
        {
            set_where(where, "morph");
            return VertexCookError::BadMorph;
        }
        for (crd::usize i = 0; i < desc.morph.targets_apply_to.size(); ++i)
        {
            const crd::i32 ai = find_attr(desc.attrs, sv_of(desc.morph.targets_apply_to[i]));
            if (ai < 0)
            {
                set_where(where, sv_of(desc.morph.targets_apply_to[i]));
                return VertexCookError::UnknownSource;
            }
            // ⛔ A morph delta wider than the attribute it drives would write past it; narrower would leave the
            // tail at rest. Either way the mesh deforms in a way no one authored.
            if (desc.attrs[static_cast<crd::usize>(ai)].comps > desc.morph.stride)
            {
                set_where(where, sv_of(desc.morph.targets_apply_to[i]));
                return VertexCookError::BadMorph;
            }
        }
    }

    // ── the displacement graph: the same rules `.crdm` applies, for the same reasons.
    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        const VertNodeDesc& n  = desc.nodes[i];
        const crd::i32      oi = [&] {
            for (crd::u32 k = 0; k < crd::matcook::material_op_count(); ++k)
            {
                if (str_eq(n.op, crd::matcook::material_op_name(k))) { return static_cast<crd::i32>(k); }
            }
            return -1;
        }();
        // ⭐ REN-38-F7: `view_proj` is a VERTEX-COOK-LOCAL op (header matrix · (p,1) → clip vec4) — the material
        // registry deliberately has no header concept, and a procedural vertex is the only stage that names it.
        const bool is_view_proj = str_eq(n.op, std::string_view("view_proj"));
        if (is_view_proj && desc.position_node.empty())
        {
            set_where(where, sv_of(n.op));
            return VertexCookError::UnknownOp;
        }
        if (oi < 0 && !is_view_proj)
        {
            set_where(where, sv_of(n.op));
            return VertexCookError::UnknownOp;
        }
        const crd::u32 arity = is_view_proj ? 1U : crd::matcook::material_op_arity(static_cast<crd::u32>(oi));
        if (n.inputs.size() != arity)
        {
            set_where(where, sv_of(n.name));
            return VertexCookError::WrongArity;
        }
        // (no `continue` for view_proj: `material_op_arg_is_attr` answers false for an out-of-range op index,
        // so the per-input source checks below still run for its one input)
        for (crd::usize k = 0; k < n.inputs.size(); ++k)
        {
            const VertInput& in = n.inputs[k];
            if (crd::matcook::material_op_arg_is_attr(static_cast<crd::u32>(oi), static_cast<crd::u32>(k)))
            {
                if (in.kind != VertInputKind::Literal)
                {
                    set_where(where, sv_of(n.name));
                    return VertexCookError::AttrNotConstant;
                }
                const auto v = static_cast<crd::i32>(in.value[0]);
                if (static_cast<double>(v) != in.value[0]
                    || v < crd::matcook::material_op_attr_min(static_cast<crd::u32>(oi))
                    || v > crd::matcook::material_op_attr_max(static_cast<crd::u32>(oi)))
                {
                    set_where(where, sv_of(n.name));
                    return VertexCookError::AttrOutOfRange;
                }
                continue;
            }
            if (in.kind == VertInputKind::Attribute)
            {
                if (find_attr(desc.attrs, sv_of(in.name)) < 0)
                {
                    set_where(where, sv_of(in.name));
                    return VertexCookError::UnknownSource;
                }
            }
            else if (in.kind == VertInputKind::Node)
            {
                crd::i32 src = -1;
                for (crd::usize s = 0; s < desc.nodes.size(); ++s)
                {
                    if (str_eq(desc.nodes[s].name, sv_of(in.name))) { src = static_cast<crd::i32>(s); }
                }
                if (src < 0)
                {
                    set_where(where, sv_of(in.name));
                    return VertexCookError::UnknownSource;
                }
                // The `.crdm` rule, restated: a DAG enforced by DECLARATION ORDER, so the cook is one forward
                // pass and a cycle is impossible to write rather than merely rejected.
                if (static_cast<crd::usize>(src) >= i)
                {
                    set_where(where, sv_of(n.name));
                    return VertexCookError::NodeCycle;
                }
            }
            // ⭐ REN-38-F7: the PROCEDURAL inputs are only meaningful under `position = "node:…"` (the pull and
            // tess/mesh paths have no expansion contract to resolve them against), and every record term must
            // land INSIDE the declared record — a word past `instance_words` reads the NEXT instance's data,
            // which draws as corrupt geometry rather than failing.
            // REN-38-F6+ exception: `hdr:` / `hdru:` HEADER words need no expansion contract — only a bound
            // storage buffer — so a TESS-EVAL graph (the #25 storage-tess seam) and a FETCH mesh (whose whole
            // meaning is "the buffer is bound") may read them too.
            else if (in.kind != VertInputKind::Literal)
            {
                const bool is_hdr_word  = in.kind == VertInputKind::Hdr || in.kind == VertInputKind::HdrU;
                const bool hdr_stage_ok = is_hdr_word
                                          && (desc.stage == StageKind::TessEval
                                              || (desc.stage == StageKind::Mesh && desc.mesh.fetch));
                if (desc.position_node.empty() && !hdr_stage_ok)
                {
                    set_where(where, sv_of(n.name));
                    return VertexCookError::BadExpand;
                }
                const bool is_field = in.kind == VertInputKind::Field || in.kind == VertInputKind::FieldU
                                      || in.kind == VertInputKind::FieldC;
                if ((is_field || in.kind == VertInputKind::Category)
                    && (desc.expand.instance_words == 0U
                        || (is_field && in.word >= desc.expand.instance_words)))
                {
                    set_where(where, sv_of(n.name));
                    return VertexCookError::BadExpand;
                }
                if (in.kind == VertInputKind::Category
                    && (!desc.expand.has_category || desc.expand.category_field >= desc.expand.instance_words))
                {
                    set_where(where, sv_of(n.name));
                    return VertexCookError::BadExpand;
                }
            }
        }
    }
    // ⭐ REN-38-F7: `position = "node:…"` — the node must exist, and the mode is a VERTEX-stage construct (every
    // other stage has its own position contract). The vec4 width is checked at cook, where the graph exists.
    if (!desc.position_node.empty())
    {
        if (desc.stage != StageKind::Vertex)
        {
            set_where(where, sv_of(desc.position_node));
            return VertexCookError::BadPositionNode;
        }
        bool pn_found = false;
        for (crd::usize s = 0; s < desc.nodes.size(); ++s)
        {
            if (str_eq(desc.nodes[s].name, sv_of(desc.position_node))) { pn_found = true; }
        }
        if (!pn_found)
        {
            set_where(where, sv_of(desc.position_node));
            return VertexCookError::BadPositionNode;
        }
        // ⛔ an expansion of zero divides the vertex id by nothing at all
        if (desc.expand.verts_per_instance == 0U || desc.expand.verts_per_instance > 64U)
        {
            set_where(where, "expand");
            return VertexCookError::BadExpand;
        }
    }
    if (!desc.displace.empty())
    {
        bool found = false;
        for (crd::usize s = 0; s < desc.nodes.size(); ++s)
        {
            if (str_eq(desc.nodes[s].name, sv_of(desc.displace))) { found = true; }
        }
        if (!found)
        {
            set_where(where, sv_of(desc.displace));
            return VertexCookError::UnknownSource;
        }
    }

    // ── the varyings
    for (crd::usize i = 0; i < desc.varyings.size(); ++i)
    {
        const VaryingDesc& v = desc.varyings[i];
        if (v.source.empty())
        {
            set_where(where, sv_of(v.name));
            return VertexCookError::EmptySource;
        }
        if (v.source.size() > kMaxVaryingSrc)
        {
            set_where(where, sv_of(v.name));
            return VertexCookError::VaryingTooWide;
        }
        // ⛔ TWO VARYINGS AT ONE LOCATION: one silently wins, and which one depends on emission order. The
        // fragment side reads a field that is sometimes the other one's.
        for (crd::usize k = 0; k < i; ++k)
        {
            if (desc.varyings[k].location == v.location)
            {
                set_where(where, sv_of(v.name));
                return VertexCookError::DuplicateLocation;
            }
        }
        for (crd::usize k = 0; k < v.source.size(); ++k)
        {
            const VaryingSource& s = v.source[k];
            if (s.kind == VaryingSourceKind::ClipW) { continue; }
            if (s.kind == VaryingSourceKind::Node)
            {
                if (s.comps < 1U || s.comps > 4U)
                {
                    set_where(where, sv_of(v.name));
                    return VertexCookError::BadComponentCount;
                }
                bool found = false;
                for (crd::usize n = 0; n < desc.nodes.size(); ++n)
                {
                    if (str_eq(desc.nodes[n].name, sv_of(s.name))) { found = true; }
                }
                if (!found)
                {
                    set_where(where, sv_of(s.name));
                    return VertexCookError::UnknownSource;
                }
                continue;
            }
            const auto& pool = s.kind == VaryingSourceKind::Instance ? desc.instance.attrs : desc.attrs;
            if (find_attr(pool, sv_of(s.name)) < 0)
            {
                set_where(where, sv_of(s.name));
                return VertexCookError::UnknownSource;
            }
        }
        const crd::u32 w = varying_width(desc, static_cast<crd::u32>(i));
        if (w < 1U || w > 4U)
        {
            set_where(where, sv_of(v.name));
            return VertexCookError::VaryingTooWide;
        }
    }
    return VertexCookError::Ok;
}

// ── 38-D4: THE VARYING CONTRACT ───────────────────────────────────────────────────────────────────────────
VertexCookError verify_varying_contract(const VertexProgramDesc& desc, const VaryingRequirement* req, crd::u32 n_req,
                                        crd::containers::String* where)
{
    if (req == nullptr && n_req > 0U) { return VertexCookError::ContractMismatch; }
    for (crd::u32 r = 0; r < n_req; ++r)
    {
        const VaryingRequirement& want = req[r];
        crd::i32                  hit  = -1;
        for (crd::usize i = 0; i < desc.varyings.size(); ++i)
        {
            // A NAMED requirement matches by name (asset ↔ asset). A NAMELESS one — the form derived from a
            // cooked fragment graph, which knows locations but not authoring names — matches by LOCATION.
            if (want.name != nullptr ? str_eq(desc.varyings[i].name, std::string_view(want.name))
                                     : desc.varyings[i].location == want.location)
            {
                hit = static_cast<crd::i32>(i);
                break;
            }
        }
        if (hit < 0)
        {
            if (want.name != nullptr) { set_where(where, std::string_view(want.name)); }
            else if (where != nullptr)
            {
                char loc[32];
                (void)std::snprintf(static_cast<char*>(loc), sizeof(loc), "location %u", want.location);
                set_where(where, std::string_view(static_cast<const char*>(loc)));
            }
            return VertexCookError::ContractMismatch;
        }
        const VaryingDesc& got = desc.varyings[static_cast<crd::usize>(hit)];
        // ⛔⛔ LOCATION, WIDTH AND INTERPOLATION, not just the name. A name-only check passes a VS that emits
        // `world_normal` as a vec2 at location 3 while the FS reads a vec3 at 0 — which links, binds, and shades
        // from whatever occupies the neighbouring slot. And a SMOOTH varying read as FLAT takes the provoking
        // vertex's value across the whole triangle: faceted output that reads as a normals bug.
        if (got.location != want.location || varying_width(desc, static_cast<crd::u32>(hit)) != want.comps
            || got.flat != want.flat)
        {
            set_where(where, sv_of(got.name));
            return VertexCookError::ContractMismatch;
        }
    }
    return VertexCookError::Ok;
}

// ── REN-38 audit: derive a cooked FRAGMENT entry's read set (the live half of the 38-D4 contract). ─────────
bool fs_varying_requirements(const crd::kir::KGraph& g, const crd::kir::KEntry& fs, VaryingRequirement* out,
                             crd::u32 cap, crd::u32* n_out, crd::memory::IAllocator* alloc)
{
    if (out == nullptr || n_out == nullptr || alloc == nullptr) { return false; }
    *n_out = 0U;

    crd::containers::Array<crd::u8> seen(alloc);
    seen.resize(static_cast<crd::usize>(g.size()));
    for (crd::usize i = 0; i < seen.size(); ++i) { seen[i] = 0U; }
    crd::containers::Array<int> stack(alloc);
    const auto push_root = [&](int r) {
        if (r >= 0 && r < g.size()) { stack.push_back(r); }
    };
    push_root(fs.frag_depth);
    push_root(fs.discard_cond);
    push_root(fs.storage_write_index);
    push_root(fs.storage_write_value);
    for (int i = 0; i < fs.n_out; ++i) { push_root(fs.out[i].node); }

    bool consistent = true;
    while (!stack.empty())
    {
        const int id = stack[stack.size() - 1U];
        stack.pop_back();
        if (seen[static_cast<crd::usize>(id)] != 0U) { continue; }
        seen[static_cast<crd::usize>(id)] = 1U;
        const crd::kir::KNode& n = g.node(id);
        if (n.op == crd::kir::KOp::StageIn)
        {
            const auto loc   = static_cast<crd::u32>(n.iidx);
            const auto width = static_cast<crd::u32>(n.type.comps());
            const bool flat  = static_cast<crd::kir::Interp>(n.dset) == crd::kir::Interp::Flat;
            crd::i32   hit   = -1;
            for (crd::u32 r = 0; r < *n_out; ++r)
            {
                if (out[r].location == loc) { hit = static_cast<crd::i32>(r); break; }
            }
            if (hit >= 0)
            {
                // ⛔ One location read at two widths or interpolations is a fragment program that disagrees
                // with ITSELF — no vertex declaration can satisfy it, so the derivation reports it rather
                // than letting the contract check blame the `.crdv`.
                if (out[hit].comps != width || out[hit].flat != flat) { consistent = false; }
            }
            else if (*n_out < cap)
            {
                out[*n_out] = VaryingRequirement{nullptr, loc, width, flat};
                ++(*n_out);
            }
            else { consistent = false; } // more read locations than the caller can hold — never truncate silently
        }
        const int ops[4] = {n.a, n.b, n.c, n.d};
        for (const int o : ops) { push_root(o); }
        for (int k = 0; k < static_cast<int>(n.n_ext); ++k) { push_root(g.ext_operand(n, k)); }
    }
    return consistent;
}

// ── THE LAYOUT IDENTITY ───────────────────────────────────────────────────────────────────────────────────
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
void hash_str(crd::u64& h, const crd::containers::String& s) noexcept
{
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        h ^= static_cast<crd::u8>(s.c_str()[i]);
        h *= kFnvPrime;
    }
    h *= kFnvPrime;
}
} // namespace

crd::u64 vertex_layout_id(const VertexProgramDesc& desc) noexcept
{
    // ⛔ Hashed FIELD BY FIELD, never over the struct's bytes. A POD memcpy would fold PADDING — uninitialised
    // stack history — into the id, so the same layout would hash differently between runs and the variant cache
    // would miss every time (the 2026-07-25 cook-dedup scar, one asset over).
    crd::u64 h = kFnvOffset;
    hash_u64(h, desc.vertex_stride);
    hash_u64(h, desc.instance.stride);
    hash_u64(h, desc.instance.transform);
    hash_u64(h, desc.instance.has_normal_transform ? desc.instance.normal_transform + 1ULL : 0ULL);
    hash_u64(h, static_cast<crd::u64>(desc.transform));
    hash_u64(h, desc.cascade);
    const auto hash_attrs = [&](const crd::containers::Array<VertexAttrDesc>& a) {
        for (crd::usize i = 0; i < a.size(); ++i)
        {
            hash_str(h, a[i].name);
            hash_u64(h, a[i].offset);
            hash_u64(h, a[i].comps);
            hash_u64(h, static_cast<crd::u64>(a[i].kind));
        }
    };
    hash_attrs(desc.attrs);
    hash_attrs(desc.instance.attrs);
    hash_u64(h, static_cast<crd::u64>(desc.skin.scheme));
    hash_u64(h, desc.skin.influences);
    hash_u64(h, desc.skin.stride);
    hash_u64(h, desc.skin.joint_words);
    hash_u64(h, desc.skin.weight_off);
    hash_u64(h, desc.skin.palette_stride);
    hash_u64(h, desc.morph.target_count);
    hash_u64(h, desc.morph.stride);
    for (crd::usize i = 0; i < desc.morph.targets_apply_to.size(); ++i) { hash_str(h, desc.morph.targets_apply_to[i]); }
    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        hash_str(h, desc.nodes[i].name);
        hash_str(h, desc.nodes[i].op);
        for (crd::usize k = 0; k < desc.nodes[i].inputs.size(); ++k)
        {
            const VertInput& in = desc.nodes[i].inputs[k];
            hash_u64(h, static_cast<crd::u64>(in.kind));
            hash_str(h, in.name);
            hash_u64(h, in.comps);
            for (int c = 0; c < 4; ++c)
            {
                crd::u64 bits = 0;
                std::memcpy(&bits, &in.value[c], sizeof(bits));
                hash_u64(h, bits);
            }
        }
    }
    hash_str(h, desc.displace);
    for (crd::usize i = 0; i < desc.varyings.size(); ++i)
    {
        hash_str(h, desc.varyings[i].name);
        hash_u64(h, desc.varyings[i].location);
        hash_u64(h, desc.varyings[i].flat ? 1ULL : 0ULL);
        for (crd::usize k = 0; k < desc.varyings[i].source.size(); ++k)
        {
            hash_u64(h, static_cast<crd::u64>(desc.varyings[i].source[k].kind));
            hash_str(h, desc.varyings[i].source[k].name);
            hash_u64(h, desc.varyings[i].source[k].comps);
        }
    }
    const crd::u32* hw = &desc.header.index_count;
    for (crd::u32 i = 0; i < sizeof(VertexHeaderMap) / sizeof(crd::u32); ++i) { hash_u64(h, hw[i]); }
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
void app_quoted(crd::containers::String& o, const crd::containers::String& v)
{
    app(o, "\"");
    o.append(v.c_str());
    app(o, "\"");
}
void app_f64(crd::containers::String& o, double v)
{
    char buf[40];
    (void)std::snprintf(static_cast<char*>(buf), sizeof(buf), "%.17g", v);
    o.append(static_cast<const char*>(buf));
    bool plain = true;
    for (const char* p = static_cast<const char*>(buf); *p != 0; ++p)
    {
        if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') { plain = false; }
    }
    if (plain) { app(o, ".0"); }
}
void emit_attrs(crd::containers::String& o, const crd::containers::Array<VertexAttrDesc>& a, const char* table)
{
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        app(o, "\n[[");
        app(o, table);
        app(o, "]]\nname   = ");
        app_quoted(o, a[i].name);
        app(o, "\noffset = ");
        app_u32(o, a[i].offset);
        app(o, "\ncomps  = ");
        app_u32(o, a[i].comps);
        app(o, "\nkind   = \"");
        app(o, attr_kind_name(a[i].kind));
        app(o, "\"\n");
    }
}
} // namespace

crd::containers::String emit_vertex_toml(const VertexProgramDesc& desc, crd::memory::IAllocator* a)
{
    crd::containers::String o(a);
    app(o, "schema = 1\nname   = ");
    app_quoted(o, desc.name);
    app(o, "\nstage = \"");
    app(o, stage_name(desc.stage));
    app(o, "\"");
    app(o, "\ntransform = \"");
    app(o, transform_name(desc.transform));
    app(o, "\"\ncascade   = ");
    app_u32(o, desc.cascade);
    app(o, "\n");
    if (!desc.displace.empty())
    {
        app(o, "displace  = ");
        app_quoted(o, desc.displace);
        app(o, "\n");
    }
    // REN-38-F7: the procedural position node — a ROOT key, so it must precede every table (the scoping scar)
    if (!desc.position_node.empty())
    {
        app(o, "position  = \"node:");
        o.append(desc.position_node.c_str());
        app(o, "\"\n");
    }

    // ── REN-38 audit: the PER-STAGE parameter sections. The F-band emitter wrote the stage NAME and dropped
    // its PARAMETERS — a tess asset saved by an editor came back with default levels, a mesh asset with a
    // default meshlet budget, an any-hit with cutoff 0. The same field-loss class the frame blob carried.
    // ⛔ AFTER every root-level bare key (a bare key following a table belongs to THAT table — the scoping
    // scar) and ahead of [header], as named tables the parser reads order-independently.
    switch (desc.stage)
    {
    case StageKind::TessControl:
    case StageKind::TessEval:
        app(o, "\n[tess]\npatch_size = ");
        app_u32(o, desc.tess.patch_size);
        app(o, "\ninner      = ");
        app_f64(o, desc.tess.inner);
        app(o, "\nouter      = ");
        app_f64(o, desc.tess.outer);
        app(o, "\n");
        break;
    case StageKind::Mesh:
        app(o, "\n[mesh]\nmax_vertices   = ");
        app_u32(o, desc.mesh.max_vertices);
        app(o, "\nmax_primitives = ");
        app_u32(o, desc.mesh.max_primitives);
        app(o, "\nworkgroup      = ");
        app_u32(o, desc.mesh.workgroup);
        if (desc.mesh.fetch) { app(o, "\nfetch          = true"); }   // REN-38-F6+
        if (desc.mesh.payload) { app(o, "\npayload        = true"); } // REN-38-F6+: dispatched by a task
        app(o, "\n");
        break;
    case StageKind::Task:
        app(o, "\n[task]\nemit = ");
        app_u32(o, desc.task.emit);
        if (desc.task.emit_header >= 0) // REN-38-F6+: GPU-driven amplification source
        {
            app(o, "\nemit_header = ");
            app_u32(o, static_cast<crd::u32>(desc.task.emit_header));
        }
        app(o, "\n");
        break;
    case StageKind::Cull:
        app(o, "\n[cull]\nfrustum   = ");
        app(o, desc.cull.frustum ? "true" : "false");
        app(o, "\nworkgroup = ");
        app_u32(o, desc.cull.workgroup);
        app(o, "\nargs_off  = ");
        app_u32(o, desc.cull.args_off);
        app(o, "\n");
        break;
    case StageKind::RayGen:
    case StageKind::ClosestHit:
    case StageKind::Miss:
    case StageKind::AnyHit:
    // REN-38-F13 (caught by the switch-completeness gate): the two newest RT stages carry the [rt] keys that
    // are MOST theirs — sphere_radius belongs to Intersection, callable_scale/bias to Callable — and falling
    // out of this arm emitted them with NO [rt] section at all: a canonical form that silently loses the very
    // fields the stage exists for (the inert-copy-rot scar, emit side).
    case StageKind::Intersection:
    case StageKind::CallableStage:
        app(o, "\n[rt]\npayload_words = ");
        app_u32(o, desc.rt.payload_words);
        app(o, "\nas_set        = ");
        app_u32(o, desc.rt.as_set);
        app(o, "\nas_binding    = ");
        app_u32(o, desc.rt.as_binding);
        app(o, "\nout_binding   = ");
        app_u32(o, desc.rt.out_binding);
        app(o, "\nalpha_cutoff  = ");
        app_f64(o, desc.rt.alpha_cutoff);
        app(o, "\nsphere_radius = ");
        app_f64(o, desc.rt.sphere_radius);
        app(o, "\ncallable_scale = ");
        app_f64(o, desc.rt.callable_scale);
        app(o, "\ncallable_bias  = ");
        app_f64(o, desc.rt.callable_bias);
        app(o, "\nuse_callable   = ");
        app(o, desc.rt.use_callable ? "true" : "false");
        app(o, "\n");
        break;
    case StageKind::Vertex:
    case StageKind::VisBuffer:
        break;
    }
    // REN-38-F7: the expansion contract of a procedural vertex — a field-loss here would come back as a
    // 1-vertex-per-instance quad reading record word 0 of a record that starts at 32 (the blob-v3 disease).
    if (!desc.position_node.empty())
    {
        app(o, "\n[expand]\nverts_per_instance = ");
        app_u32(o, desc.expand.verts_per_instance);
        app(o, "\ninstance_words     = ");
        app_u32(o, desc.expand.instance_words);
        app(o, "\ninstance_off       = ");
        app_u32(o, desc.expand.instance_off);
        if (desc.expand.has_category)
        {
            app(o, "\ncategory_field     = ");
            app_u32(o, desc.expand.category_field);
            app(o, "\ncategory_mask_word = ");
            app_u32(o, desc.expand.category_mask_word);
        }
        app(o, "\n");
    }

    app(o, "\n[header]\n");
    const char* hk[13] = {"index_count", "index_off",   "vertex_off",  "instance_off",
                          "visible_off", "view_proj",   "light_vp",    "skin_off",
                          "palette_off", "joint_count", "morph_off",   "morph_weights",
                          "instance_count"};
    const crd::u32* hv = &desc.header.index_count;
    for (int i = 0; i < 13; ++i)
    {
        app(o, hk[i]);
        app(o, " = ");
        app_u32(o, hv[i]);
        app(o, "\n");
    }

    app(o, "\n[vertex]\nstride = ");
    app_u32(o, desc.vertex_stride);
    app(o, "\n");
    emit_attrs(o, desc.attrs, "attribute");

    app(o, "\n[instance]\nstride    = ");
    app_u32(o, desc.instance.stride);
    app(o, "\ntransform = ");
    app_u32(o, desc.instance.transform);
    if (desc.instance.has_normal_transform)
    {
        app(o, "\nnormal_transform = ");
        app_u32(o, desc.instance.normal_transform);
    }
    app(o, "\n");
    emit_attrs(o, desc.instance.attrs, "instance_attribute");

    if (desc.skin.scheme != SkinScheme::None)
    {
        app(o, "\n[skin]\nscheme         = \"");
        app(o, skin_scheme_name(desc.skin.scheme));
        app(o, "\"\ninfluences     = ");
        app_u32(o, desc.skin.influences);
        app(o, "\nstride         = ");
        app_u32(o, desc.skin.stride);
        app(o, "\njoint_words    = ");
        app_u32(o, desc.skin.joint_words);
        app(o, "\nweight_off     = ");
        app_u32(o, desc.skin.weight_off);
        app(o, "\npalette_stride = ");
        app_u32(o, desc.skin.palette_stride);
        app(o, "\n");
    }

    if (desc.morph.target_count > 0U)
    {
        app(o, "\n[morph]\ntargets  = ");
        app_u32(o, desc.morph.target_count);
        app(o, "\nstride   = ");
        app_u32(o, desc.morph.stride);
        app(o, "\napply_to = [");
        for (crd::usize i = 0; i < desc.morph.targets_apply_to.size(); ++i)
        {
            if (i > 0U) { app(o, ", "); }
            app_quoted(o, desc.morph.targets_apply_to[i]);
        }
        app(o, "]\n");
    }

    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        const VertNodeDesc& n = desc.nodes[i];
        app(o, "\n[[node]]\nname   = ");
        app_quoted(o, n.name);
        app(o, "\nop     = ");
        app_quoted(o, n.op);
        app(o, "\ninputs = [");
        for (crd::usize k = 0; k < n.inputs.size(); ++k)
        {
            if (k > 0U) { app(o, ", "); }
            const VertInput& in = n.inputs[k];
            if (in.kind == VertInputKind::Node) { app_quoted(o, in.name); }
            else if (in.kind == VertInputKind::Attribute)
            {
                app(o, "\"@");
                o.append(in.name.c_str());
                app(o, "\"");
            }
            // REN-38-F7: the procedural spellings survive the round trip verbatim
            else if (in.kind == VertInputKind::Corner) { app(o, "\"@corner\""); }
            else if (in.kind == VertInputKind::Instance) { app(o, "\"@instance\""); }
            else if (in.kind == VertInputKind::Category) { app(o, "\"@category\""); }
            else if (in.kind == VertInputKind::Field || in.kind == VertInputKind::FieldU
                     || in.kind == VertInputKind::FieldC || in.kind == VertInputKind::Hdr
                     || in.kind == VertInputKind::HdrU || in.kind == VertInputKind::HdrC)
            {
                const char* spelling = "hdrc:";
                if (in.kind == VertInputKind::Field) { spelling = "field:"; }
                else if (in.kind == VertInputKind::FieldU) { spelling = "fieldu:"; }
                else if (in.kind == VertInputKind::FieldC) { spelling = "fieldc:"; }
                else if (in.kind == VertInputKind::Hdr) { spelling = "hdr:"; }
                else if (in.kind == VertInputKind::HdrU) { spelling = "hdru:"; }
                app(o, "\"");
                app(o, spelling);
                app_u32(o, in.word);
                app(o, "\"");
            }
            else if (in.comps <= 1U) { app_f64(o, in.value[0]); }
            else
            {
                app(o, "[");
                for (crd::u32 c = 0; c < in.comps && c < 4U; ++c)
                {
                    if (c > 0U) { app(o, ", "); }
                    app_f64(o, in.value[c]);
                }
                app(o, "]");
            }
        }
        app(o, "]\n");
    }

    for (crd::usize i = 0; i < desc.varyings.size(); ++i)
    {
        const VaryingDesc& v = desc.varyings[i];
        app(o, "\n[[varying]]\nname     = ");
        app_quoted(o, v.name);
        app(o, "\nlocation = ");
        app_u32(o, v.location);
        app(o, "\ninterp   = \"");
        app(o, v.flat ? "flat" : "smooth");
        app(o, "\"\nsource   = [");
        bool any_node = false;
        for (crd::usize k = 0; k < v.source.size(); ++k)
        {
            if (k > 0U) { app(o, ", "); }
            const VaryingSource& s = v.source[k];
            switch (s.kind)
            {
            case VaryingSourceKind::ClipW: app(o, "\"clip.w\""); break;
            case VaryingSourceKind::World:
                app(o, "\"world:");
                o.append(s.name.c_str());
                app(o, "\"");
                break;
            case VaryingSourceKind::Instance:
                app(o, "\"instance:");
                o.append(s.name.c_str());
                app(o, "\"");
                break;
            case VaryingSourceKind::Node:
                app(o, "\"node:");
                o.append(s.name.c_str());
                app(o, "\"");
                any_node = true;
                break;
            case VaryingSourceKind::Attribute: app_quoted(o, s.name); break;
            }
        }
        app(o, "]\n");
        // ⛔ A node term's declared WIDTH has to survive the round trip too — dropping it turns a valid asset
        // into one that fails validation on reload, which is a save that corrupts its own file.
        if (any_node)
        {
            app(o, "node_comps = [");
            bool first = true;
            for (crd::usize k = 0; k < v.source.size(); ++k)
            {
                if (v.source[k].kind != VaryingSourceKind::Node) { continue; }
                if (!first) { app(o, ", "); }
                first = false;
                app_u32(o, v.source[k].comps);
            }
            app(o, "]\n");
        }
    }
    return o;
}

// ── ⭐ THE COOK ───────────────────────────────────────────────────────────────────────────────────────────
namespace
{
// ── ⭐ REN-38-F3: THE RAY-TRACING STAGES. ────────────────────────────────────
// These are IMPERATIVE entries (statement bodies), not the functional DAG the raster stages use — a raygen
// TRACES and STORES, which are effects. ⛔ All three stages share the declared `payload_words`; a mismatch does
// not fail to link, it reads the neighbouring payload slot.
[[nodiscard]] bool cook_rt(const VertexProgramDesc& desc, KGraph& g, crd::kir::KEntry& ve)
{
    Vx         c(g);
    const auto words = static_cast<int>(desc.rt.payload_words);
    if (desc.stage == StageKind::RayGen)
    {
        const int as  = g.accel_struct_decl(static_cast<int>(desc.rt.as_set), static_cast<int>(desc.rt.as_binding));
        const int out = g.buffer_decl(DType::F32, static_cast<int>(desc.rt.as_set),
                                      static_cast<int>(desc.rt.out_binding), true);
        const int pl  = g.ray_payload_decl(words);
        const int mk  = g.kernel_stmt_mark();
        const int idx = g.cast(g.swizzle(g.builtin(crd::kir::KBuiltin::LaunchId), 0), DType::U32);
        // One ray per launch index along +Z. ⛔ tmin is 0.001, not 0: a ray starting exactly ON the surface
        // self-intersects at t=0 and every pixel reports a hit against its own geometry.
        g.stmt_trace_ray_pipeline(as, pl, c.kf(0.0), c.kf(0.0), c.kf(0.0), c.kf(0.0), c.kf(0.0), c.kf(1.0),
                                  c.kf(0.001), c.kf(1.0e30));
        // ⭐ REN-38-F13: `use_callable` routes the traced payload THROUGH the SBT's callable — what lands in
        // the output is the callable's transform of it, so the gate can prove the fourth table dispatched.
        if (desc.rt.use_callable)
        {
            const int cd = g.callable_data_decl(words);
            g.stmt_payload_store(cd, 0, g.payload_load(pl, 0));
            g.stmt_execute_callable(c.ku(0U), cd);
            g.stmt_buffer_store(out, idx, g.payload_load(cd, 0));
        }
        else { g.stmt_buffer_store(out, idx, g.payload_load(pl, 0)); }
        ve.stage             = crd::kir::KStage::RayGen;
        ve.kernel_body_begin = mk;
        ve.kernel_body_count = g.stmt_count() - mk;
        return ve.kernel_body_count > 0;
    }
    // REN-38 audit: the ANY-HIT — called per traversal candidate; hits whose barycentric u+v falls below the
    // declared cutoff are IGNORED (`ignoreIntersectionEXT` / `IgnoreHit()`), so traversal continues past the
    // "transparent" region. The portable OMM fallback for alpha-tested RT geometry, as a declaration.
    if (desc.stage == StageKind::AnyHit)
    {
        (void)g.ray_payload_decl(words); // shares the pipeline's payload signature (unused by the ignore test)
        const int mk   = g.kernel_stmt_mark();
        const int bary = g.builtin(crd::kir::KBuiltin::HitBary);
        const int a    = g.binary(crd::kir::KOp::Add, g.vec_comp(bary, 0), g.vec_comp(bary, 1));
        g.stmt_ignore_hit_if(g.binary(crd::kir::KOp::CmpLt, a, c.kf(desc.rt.alpha_cutoff)));
        ve.stage             = crd::kir::KStage::AnyHit;
        ve.kernel_body_begin = mk;
        ve.kernel_body_count = g.stmt_count() - mk;
        return ve.kernel_body_count > 0;
    }
    // ── ⭐ REN-38-F13: INTERSECTION — the analytic SPHERE (object space, centred at the origin, declared
    // radius). The classic quadratic against the OBJECT ray; a hit reports the near root and the API itself
    // validates t against [tmin, tmax]. Procedural geometry is what this stage exists for: the AABB in the BLAS
    // is only a bound, the shape lives here, as authored math.
    if (desc.stage == StageKind::Intersection)
    {
        const int  mk = g.kernel_stmt_mark();
        const int  o3 = g.builtin(crd::kir::KBuiltin::ObjectRayOrigin);
        const int  d3 = g.builtin(crd::kir::KBuiltin::ObjectRayDirection);
        const auto cp = [&](int v, int i) { return g.vec_comp(v, i); };
        const int  ox = cp(o3, 0);
        const int  oy = cp(o3, 1);
        const int  oz = cp(o3, 2);
        const int  dx = cp(d3, 0);
        const int  dy = cp(d3, 1);
        const int  dz = cp(d3, 2);
        const int  qa = c.add(c.add(c.mul(dx, dx), c.mul(dy, dy)), c.mul(dz, dz));
        const int  qb = c.add(c.add(c.mul(ox, dx), c.mul(oy, dy)), c.mul(oz, dz));
        const int  qc = c.sub(c.add(c.add(c.mul(ox, ox), c.mul(oy, oy)), c.mul(oz, oz)),
                              c.kf(desc.rt.sphere_radius * desc.rt.sphere_radius));
        const int  disc = c.sub(c.mul(qb, qb), c.mul(qa, qc));
        const int  hit  = g.binary(KOp::CmpGe, disc, c.kf(0.0));
        const int  ifid = g.stmt_if_begin(hit);
        const int  root = g.unary(KOp::Sqrt, g.binary(KOp::Max, disc, c.kf(0.0)));
        // ⛔ BOTH roots. A ray starting INSIDE the sphere has its near root behind it; reporting only the near
        // one makes every inside-out ray miss. The API rejects whichever root falls outside [tmin, tmax], and
        // traversal keeps the closest accepted — so reporting both is correct from any origin.
        g.stmt_report_hit(c.dvd(c.sub(c.sub(c.kf(0.0), qb), root), qa), c.ku(0U));
        g.stmt_report_hit(c.dvd(c.add(c.sub(c.kf(0.0), qb), root), qa), c.ku(0U));
        g.stmt_if_end(ifid);
        ve.stage             = crd::kir::KStage::Intersection;
        ve.kernel_body_begin = mk;
        ve.kernel_body_count = g.stmt_count() - mk;
        return ve.kernel_body_count > 0;
    }
    // ── ⭐ REN-38-F13: CALLABLE — an SBT subroutine over its data block: cd.m0 = cd.m0 · scale + bias, a
    // transform a gate can verify against the declared constants.
    if (desc.stage == StageKind::CallableStage)
    {
        const int cd = g.callable_data_decl(words);
        const int mk = g.kernel_stmt_mark();
        g.stmt_payload_store(cd, 0,
                             c.add(c.mul(g.payload_load(cd, 0), c.kf(desc.rt.callable_scale)),
                                   c.kf(desc.rt.callable_bias)));
        ve.stage             = crd::kir::KStage::Callable;
        ve.kernel_body_begin = mk;
        ve.kernel_body_count = g.stmt_count() - mk;
        return ve.kernel_body_count > 0;
    }
    // MISS and CLOSEST-HIT both write the payload; what differs is the value, which is the whole point of a
    // hit/miss pair. ⛔ A miss shader that wrote nothing would leave the payload at whatever the last ray left.
    const int pl = g.ray_payload_decl(words);
    const int mk = g.kernel_stmt_mark();
    g.stmt_payload_store(pl, 0, c.kf(desc.stage == StageKind::Miss ? -1.0 : 1.0));
    ve.stage             = desc.stage == StageKind::Miss ? crd::kir::KStage::Miss : crd::kir::KStage::ClosestHit;
    ve.kernel_body_begin = mk;
    ve.kernel_body_count = g.stmt_count() - mk;
    return ve.kernel_body_count > 0;
}

// ── ⭐ REN-38-F4: GPU-DRIVEN CULLING. ───────────────────────────────────────
// A compute program that tests each instance and writes the surviving count into the indirect args — so the
// DRAW COUNT comes from the GPU and the CPU never reads it back. ⛔ The frustum test is declared rather than
// assumed: a cull pass that silently kept everything costs exactly what no culling costs, and looks identical.
[[nodiscard]] bool cook_cull(const VertexProgramDesc& desc, KGraph& g, crd::kir::KEntry& ve)
{
    Vx        c(g);
    const int mk  = g.kernel_stmt_mark();
    // ⛔ REN-38-F6: the global id is WorkgroupIndex · workgroup + LocalInvocationIndex — the kernel emitters
    // lower ONLY those two builtins (`GlobalInvocationId` has no arm in the compute value printer), so the F4
    // form could never emit GLSL at all. Same finding class as the storage seam below.
    const int lid = g.cast(g.builtin(crd::kir::KBuiltin::LocalInvocationIndex), DType::U32);
    const int wgi = g.cast(g.builtin(crd::kir::KBuiltin::WorkgroupIndex), DType::U32);
    const int gid = c.add(c.mul(wgi, c.ku(desc.cull.workgroup)), lid);
    // ⛔ REN-38-F6: the instance data reads through a KERNEL buffer declaration (`BufferLoad`, binding 0), NOT
    // the raster `StorageLoad` seam — the compute kernel emitters do not lower the raster implicit buffer, so
    // the F4 kernel could never create a device program (found the first time the live renderer tried).
    const int inbuf = g.buffer_decl(DType::U32, 0, 0, false);
    const auto blu  = [&](int idx) { return g.buffer_load(inbuf, idx); };
    const auto blf  = [&](int idx) { return g.int_bits_to_float(g.cast(blu(idx), DType::I32)); };
    const int base  = blu(c.ku(desc.header.instance_off));
    const int rec   = c.add(base, c.mul(gid, c.ku(desc.instance.stride)));
    // the instance world position is the transform matrix translation column
    const int wx = blf(c.add(rec, c.ku(desc.instance.transform + 12U)));
    const int wy = blf(c.add(rec, c.ku(desc.instance.transform + 13U)));
    const int wz = blf(c.add(rec, c.ku(desc.instance.transform + 14U)));
    int       vis = c.kf(1.0);
    if (desc.cull.frustum)
    {
        int clip[4];
        int vp[16];
        for (crd::u32 e = 0; e < 16U; ++e) { vp[e] = blf(c.ku(desc.header.view_proj + e)); }
        c.mul_mat4(static_cast<const int*>(vp), wx, wy, wz, c.kf(1.0), clip);
        const int w = g.binary(KOp::Max, clip[3], c.kf(1.0e-6));
        // inside = |x| <= w and |y| <= w and 0 <= z <= w — the standard clip-space containment test
        int in = g.binary(KOp::Step, g.unary(KOp::Abs, clip[0]), w);
        in     = c.mul(in, g.binary(KOp::Step, g.unary(KOp::Abs, clip[1]), w));
        in     = c.mul(in, g.binary(KOp::Step, c.kf(0.0), clip[2]));
        in     = c.mul(in, g.binary(KOp::Step, clip[2], w));
        vis    = in;
    }
    // ⛔ REN-38-F6: binding 1, NOT 3. A frame-graph compute pass binds its declared reads+writes in DECLARATION
    // ORDER from 0 (instances at 0 — the implicit storage seam — and the args buffer next), so an args buffer
    // declared at binding 3 could never bind through any authored pass: the F4 cook gate never dispatched, which
    // is how the wrong binding closed green.
    // ⛔ GUARD ON THE INSTANCE COUNT (a declared header word): threads past the instance section read garbage
    // transforms, and their verdicts polluted the indirect-args atomic — 16 marked of 8 visible on the VK gate.
    const int n_inst = blu(c.ku(desc.header.instance_count));
    const int in_rng = g.binary(KOp::CmpLt, gid, n_inst);
    const int visu   = g.select(in_rng, g.cast(vis, DType::U32), c.ku(0U));
    const int flags  = g.buffer_decl(DType::U32, 0, 1, true);
    g.stmt_buffer_store(flags, gid, visu);
    // ── ⭐ REN-38-F6+: REAL INDIRECT ARGS (binding 2) — the GPU-driven loop's other half, on the FRUSTUM
    // variant only (the passthrough variant is the marker/consumer kernel and keeps the two-binding set). The
    // kernel emits a dispatch command {survivors, 1, 1} that a `compute.indirect` pass consumes, so the
    // surviving count NEVER round-trips to the CPU. Thread 0 resets the accumulator, a BUFFER-scope barrier
    // orders it, and every thread atomically adds its verdict (0 or 1 — no branch needed). ⛔ The reset is
    // one-workgroup-scoped: the shipped scene graph dispatches groups_x = 1 (its workgroup covers the scene);
    // scaling past one workgroup puts the reset in its own prior pass, which the vocabulary already supports.
    if (desc.cull.frustum)
    {
        const int argsb = g.buffer_decl(DType::U32, 0, 2, true);
        const int is0   = g.binary(KOp::CmpEq, lid, c.ku(0U));
        const int if0   = g.stmt_if_begin(is0);
        g.stmt_buffer_store(argsb, c.ku(0U), c.ku(0U));
        g.stmt_buffer_store(argsb, c.ku(1U), c.ku(1U));
        g.stmt_buffer_store(argsb, c.ku(2U), c.ku(1U));
        g.stmt_if_end(if0);
        g.stmt_barrier(crd::kir::BarrierScope::Buffer);
        g.stmt_buffer_atomic_add(argsb, c.ku(0U), visu);
    }
    ve.stage             = crd::kir::KStage::Compute;
    ve.local_size[0]     = desc.cull.workgroup;
    ve.kernel_body_begin = mk;
    ve.kernel_body_count = g.stmt_count() - mk;
    return ve.kernel_body_count > 0;
}
} // namespace

namespace
{
// ── REN-38-F6/F7: the node graph for a PROCEDURAL stage (tess-eval / mesh / node-positioned vertex). ─────────
// The same registry dispatch the pull path runs, but non-literal, non-node inputs resolve through `input_of`
// (each stage names what it can actually honour — a domain stage its patch point, a procedural vertex its
// expansion indices and record fields; `input_of` returns -1 for anything else and the cook refuses it), and
// `custom_op` (name, inputs) may build a vertex-cook-local op the material registry deliberately does not have
// (`view_proj` — the registry has no header concept). It returns -2 for "not mine".
template <typename InFn, typename OpFn>
[[nodiscard]] bool build_stage_nodes(const VertexProgramDesc& desc, KGraph& g, Vx& c, const InFn& input_of,
                                     const OpFn& custom_op, crd::containers::Array<int>& built)
{
    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        const VertNodeDesc& n  = desc.nodes[i];
        crd::i32            oi = -1;
        for (crd::u32 k = 0; k < crd::matcook::material_op_count(); ++k)
        {
            if (str_eq(n.op, crd::matcook::material_op_name(k))) { oi = static_cast<crd::i32>(k); }
        }
        const bool is_custom = oi < 0;
        int in[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        for (crd::usize k = 0; k < n.inputs.size() && k < 8U; ++k)
        {
            const VertInput& vi = n.inputs[k];
            if (!is_custom
                && crd::matcook::material_op_arg_is_attr(static_cast<crd::u32>(oi), static_cast<crd::u32>(k)))
            {
                in[k] = static_cast<int>(vi.value[0]);
                continue;
            }
            if (vi.kind == VertInputKind::Node)
            {
                in[k] = -1;
                for (crd::usize s = 0; s < i; ++s)
                {
                    if (str_eq(desc.nodes[s].name, sv_of(vi.name))) { in[k] = built[s]; }
                }
                if (in[k] < 0) { return false; }
            }
            else if (vi.kind == VertInputKind::Literal)
            {
                int comps[4];
                for (crd::u32 e = 0; e < vi.comps && e < 4U; ++e) { comps[e] = c.kf(vi.value[e]); }
                in[k] = c.vecn(static_cast<const int*>(comps), vi.comps);
            }
            else
            {
                in[k] = input_of(vi);
                if (in[k] < 0) { return false; }
            }
        }
        int r = custom_op(n.op, static_cast<const int*>(in), static_cast<crd::u32>(n.inputs.size()));
        if (r == -2)
        {
            if (is_custom) { return false; } // neither a registry op nor a vertex-cook one
            r = crd::matcook::material_build_op(g, crd::containers::StringView(n.op.c_str(), n.op.size()),
                                                static_cast<const int*>(in),
                                                static_cast<crd::u32>(n.inputs.size()));
        }
        if (r < 0) { return false; }
        built.push_back(r);
    }
    return true;
}

// The refusing custom-op hook: stages whose position contract is fixed (tess-eval, mesh) name no local ops.
[[nodiscard]] inline int no_custom_op(const crd::containers::String&, const int*, crd::u32) { return -2; }

// The varyings a PROCEDURAL stage can emit: `node:` terms and `clip.w` only — there is no vertex record, so an
// attribute or instance source is a declaration the stage cannot honour.
[[nodiscard]] bool emit_procedural_varyings(const VertexProgramDesc& desc, KGraph& g, Vx& c, crd::kir::KEntry& ve,
                                            const crd::containers::Array<int>& built, int clip_w)
{
    ve.n_out = 0;
    for (crd::usize i = 0; i < desc.varyings.size(); ++i)
    {
        const VaryingDesc& v = desc.varyings[i];
        int                comps[4];
        crd::u32           w = 0U;
        for (crd::usize k = 0; k < v.source.size(); ++k)
        {
            const VaryingSource& s = v.source[k];
            if (s.kind == VaryingSourceKind::ClipW)
            {
                if (w < 4U) { comps[w++] = clip_w; }
                continue;
            }
            if (s.kind != VaryingSourceKind::Node) { return false; }
            int node = -1;
            for (crd::usize n = 0; n < desc.nodes.size(); ++n)
            {
                if (str_eq(desc.nodes[n].name, sv_of(s.name))) { node = built[n]; }
            }
            if (node < 0) { return false; }
            const auto got = static_cast<crd::u32>(g.node(node).comps());
            if (got != s.comps) { return false; }
            for (crd::u32 e = 0; e < got && w < 4U; ++e)
            {
                comps[w++] = got == 1U ? node : g.swizzle(node, static_cast<int>(e));
            }
        }
        if (w == 0U || ve.n_out >= crd::kir::kMaxStageOutputs) { return false; }
        ve.out[ve.n_out] = {c.vecn(static_cast<const int*>(comps), w), static_cast<int>(v.location),
                            v.flat ? crd::kir::Interp::Flat : crd::kir::Interp::Smooth};
        ++ve.n_out;
    }
    return true;
}

// The stage dispatch, unchecked — every path out of here runs the shape check in the public wrapper below.
bool cook_vertex_program_unchecked(const VertexProgramDesc& desc, KGraph& g, crd::kir::KEntry& ve)
{
    if (validate_vertex_program(desc, nullptr) != VertexCookError::Ok) { return false; }
    // REN-38-F3/F4: the stages that do NOT pull geometry take their own path — requiring a vertex record
    // of a raygen or a culling kernel would refuse a perfectly valid program.
    if (desc.stage == StageKind::RayGen || desc.stage == StageKind::ClosestHit
        || desc.stage == StageKind::Miss || desc.stage == StageKind::AnyHit
        || desc.stage == StageKind::Intersection || desc.stage == StageKind::CallableStage)
    {
        return cook_rt(desc, g, ve);
    }
    if (desc.stage == StageKind::Cull) { return cook_cull(desc, g, ve); }
    // ⛔⛔ A HULL STAGE DOES NOT PULL. It runs per CONTROL POINT of an already-assembled patch, so
    // `KBuiltin::VertexIndex` is not legal in it — `entry_valid` refuses the graph, correctly, because a hull
    // shader fetching by vertex index is reading something the stage does not have. Its whole job in the
    // B4-tess model is to SET THE TESS LEVELS and pass the patch through, so that is all it emits.
    if (desc.stage == StageKind::TessControl)
    {
        Vx hc(g);
        ve.stage           = crd::kir::KStage::TessControl;
        ve.position        = -1;
        ve.n_out           = 0;
        ve.tess_patch_size = desc.tess.patch_size;
        ve.tess_inner      = hc.kf(desc.tess.inner);
        ve.tess_outer      = hc.kf(desc.tess.outer);
        return true;
    }
    // ⛔⛔ REN-38-F6: A TASK STAGE EMITS NO GEOMETRY — `entry_valid` refuses a task entry carrying a position or
    // outputs, and the F2 cook sent it down the pull path below, which sets BOTH (and builds `VertexIndex` nodes
    // the stage-legality walk refuses graph-wide). Its whole job is amplification: the mesh-workgroup count.
    if (desc.stage == StageKind::Task)
    {
        Vx tk(g);
        ve.stage         = crd::kir::KStage::Task;
        ve.position      = -1;
        ve.n_out         = 0;
        ve.local_size[0] = desc.mesh.workgroup;
        // REN-38-F6+: a GPU-driven task reads its amplification count from a header word of the bound
        // storage buffer — the task emitters lower the sbuf read on both backends, so the CULLED count can
        // drive the meshlet dispatch.
        ve.task_emit = desc.task.emit_header >= 0 ? tk.hdru(static_cast<crd::u32>(desc.task.emit_header))
                                                  : tk.ku(desc.task.emit);
        return true;
    }
    // ── ⛔⛔ REN-38-F6 REWRITE: A DOMAIN STAGE DOES NOT PULL EITHER. The F1 cook sent TessEval down the pull
    // path below — but `KBuiltin::VertexIndex` is legal ONLY in a vertex stage and the tese emitter lowers no
    // storage loads, so the cooked entry could never create a program on ANY backend. F1 was closed on cook
    // gates that never ran `entry_valid`; the F6 join is what found it. The device-true contract (the proven
    // A1d/A7 path): position = the emitter's bilerp of the VS-emitted corners (`TessPatchPosition`), optionally
    // DISPLACED by the node graph — the "generated geometry + authored displacement" model. The graph's one
    // nameable attribute is `position` (the patch point); varyings are `node:`/`clip.w` terms only.
    if (desc.stage == StageKind::TessEval)
    {
        Vx        tc(g);
        const int patch = g.builtin(crd::kir::KBuiltin::TessPatchPosition); // vec4 — bilerped clip corners
        int       p3    = g.vec3(g.swizzle(patch, 0), g.swizzle(patch, 1), g.swizzle(patch, 2));
        crd::containers::Array<int> built(crd::memory::default_allocator());
        // REN-38-F6+: `hdr:N` / `hdru:N` read a word of the BOUND storage buffer (the tese emitters now lower
        // the sbuf seam) — the authored heightfield/ocean path. An author who reads a header word takes on the
        // matching obligation: the pass's draw item must carry a storage buffer (the #25 tess-storage seam).
        const auto input_of = [&](const VertInput& vi) {
            if (vi.kind == VertInputKind::Hdr) { return tc.hdrf(vi.word); }
            if (vi.kind == VertInputKind::HdrU) { return tc.hdru(vi.word); }
            if (vi.kind != VertInputKind::Attribute) { return -1; }
            const crd::i32 ai = find_attr(desc.attrs, sv_of(vi.name));
            return ai >= 0 && desc.attrs[static_cast<crd::usize>(ai)].kind == AttrKind::Position ? p3 : -1;
        };
        if (!build_stage_nodes(desc, g, tc, input_of, no_custom_op, built)) { return false; }
        if (!desc.displace.empty())
        {
            int disp = -1;
            for (crd::usize s = 0; s < desc.nodes.size(); ++s)
            {
                if (str_eq(desc.nodes[s].name, sv_of(desc.displace))) { disp = built[s]; }
            }
            if (disp < 0 || g.node(disp).comps() != 3) { return false; }
            p3 = disp;
        }
        ve.stage           = crd::kir::KStage::TessEval;
        ve.tess_patch_size = desc.tess.patch_size;
        ve.position = g.vec4(g.swizzle(p3, 0), g.swizzle(p3, 1), g.swizzle(p3, 2), g.swizzle(patch, 3));
        return emit_procedural_varyings(desc, g, tc, ve, built, g.swizzle(patch, 3));
    }
    // ── ⛔⛔ REN-38-F6 REWRITE: A MESH STAGE DOES NOT PULL. Same finding, same reason: the F2 cook pulled by
    // `VertexIndex` (vertex-stage-only) and the mesh emitter lowers no storage loads. The device-true form is
    // the PROCEDURAL MESHLET GRID (the proven A1c/A8 shape): each workgroup emits `max_primitives` triangles
    // tiling clip space, thread `tid` writes vertex tid (triangle tid/3, corner tid%3), and the DISPLACEMENT
    // graph moves the generated position — terrain/ocean's "generated grid + authored displacement". The grid
    // mechanic requires max_vertices == 3 * max_primitives, validated, because vertices past 3P would be
    // emitted unwritten and primitives past V/3 would index vertices that never existed.
    if (desc.stage == StageKind::Mesh)
    {
        Vx mc(g);
        // ── ⭐⭐ REN-38-F6+ MESHLET FETCH: `fetch = true` makes the mesh stage PULL REAL GEOMETRY. The F6
        // rewrite made this stage procedural because the mesh emitters lowered no storage loads; they now
        // lower the sbuf seam on both backends, so the pull contract reopens here: workgroup `wgi` covers
        // `max_vertices` consecutive slots of the SAME index/vertex/visible/instance layout the pulling VS
        // reads, thread `tid` fetches ONE indexed vertex, applies the instance matrix + view_proj, and the
        // primitive table stays the non-indexed (3t, 3t+1, 3t+2) triple — which is exactly why fetch keeps
        // the max_vertices == 3 * max_primitives invariant.
        if (desc.mesh.fetch)
        {
            const int ftid  = g.cast(g.builtin(crd::kir::KBuiltin::LocalInvocationIndex), DType::U32);
            const int fwgi  = g.cast(g.builtin(crd::kir::KBuiltin::WorkgroupIndex), DType::U32);
            const int gvid  = mc.add(mc.mul(fwgi, mc.ku(desc.mesh.max_vertices)), ftid);
            const int idxc  = mc.hdru(desc.header.index_count);
            const int ii    = mc.dvd(gvid, idxc);
            const int li    = mc.sub(gvid, mc.mul(ii, idxc));
            const int vidx  = mc.loadu(mc.add(mc.hdru(desc.header.index_off), li));
            const int vbase = mc.add(mc.hdru(desc.header.vertex_off), mc.mul(vidx, mc.ku(desc.vertex_stride)));
            const int slot  = mc.loadu(mc.add(mc.hdru(desc.header.visible_off), ii));
            const int ibase = mc.add(mc.hdru(desc.header.instance_off), mc.mul(slot, mc.ku(desc.instance.stride)));
            crd::containers::Array<AttrVals> vals(crd::memory::default_allocator());
            for (crd::usize i = 0; i < desc.attrs.size(); ++i)
            {
                AttrVals a;
                a.comps = desc.attrs[i].comps;
                for (crd::u32 k = 0; k < a.comps; ++k)
                {
                    a.obj[k] = mc.loadf(mc.add(vbase, mc.ku(desc.attrs[i].offset + k)));
                }
                vals.push_back(a);
            }
            crd::containers::Array<int> built(crd::memory::default_allocator());
            // `hdr:N` / `hdru:N` are legal here too — the buffer is already bound (that is what fetch means)
            const auto input_of = [&](const VertInput& vi) {
                if (vi.kind == VertInputKind::Hdr) { return mc.hdrf(vi.word); }
                if (vi.kind == VertInputKind::HdrU) { return mc.hdru(vi.word); }
                if (vi.kind != VertInputKind::Attribute) { return -1; }
                const crd::i32 ai = find_attr(desc.attrs, sv_of(vi.name));
                if (ai < 0) { return -1; }
                const AttrVals& av = vals[static_cast<crd::usize>(ai)];
                return mc.vecn(static_cast<const int*>(av.obj), av.comps);
            };
            if (!build_stage_nodes(desc, g, mc, input_of, no_custom_op, built)) { return false; }
            int posi = -1;
            for (crd::usize i = 0; i < desc.attrs.size(); ++i)
            {
                if (desc.attrs[i].kind == AttrKind::Position) { posi = static_cast<int>(i); break; }
            }
            if (posi < 0) { return false; } // validated (BadMesh), but the cook never trusts its caller
            AttrVals& pa = vals[static_cast<crd::usize>(posi)];
            if (!desc.displace.empty()) // the displacement REPLACES the object position, as everywhere else
            {
                int disp = -1;
                for (crd::usize si = 0; si < desc.nodes.size(); ++si)
                {
                    if (str_eq(desc.nodes[si].name, sv_of(desc.displace))) { disp = built[si]; }
                }
                if (disp < 0 || g.node(disp).comps() != 3) { return false; }
                for (crd::u32 e = 0; e < 3U; ++e) { pa.obj[e] = g.swizzle(disp, static_cast<int>(e)); }
            }
            int m[16];
            for (crd::u32 e = 0; e < 16U; ++e) { m[e] = mc.loadf(mc.add(ibase, mc.ku(desc.instance.transform + e))); }
            int wrld[4];
            mc.mul_mat4(static_cast<const int*>(m), pa.obj[0], pa.obj[1], pa.obj[2], mc.kf(1.0), wrld);
            int vp[16];
            for (crd::u32 e = 0; e < 16U; ++e) { vp[e] = mc.hdrf(desc.header.view_proj + e); }
            int clip[4];
            mc.mul_mat4(static_cast<const int*>(vp), wrld[0], wrld[1], wrld[2], wrld[3], clip);
            ve.stage           = crd::kir::KStage::Mesh;
            ve.mesh_vertices   = desc.mesh.max_vertices;
            ve.mesh_primitives = desc.mesh.max_primitives;
            ve.local_size[0]   = desc.mesh.workgroup;
            ve.mesh_payload_in = desc.mesh.payload; // REN-38-F6+: the declared task pairing
            const int fi0      = mc.mul(ftid, mc.ku(3U));
            ve.mesh_prim       = g.vec3(fi0, mc.add(fi0, mc.ku(1U)), mc.add(fi0, mc.ku(2U)));
            ve.position        = g.vec4(clip[0], clip[1], clip[2], clip[3]);
            return emit_procedural_varyings(desc, g, mc, ve, built, clip[3]);
        }
        const int tid  = g.cast(g.builtin(crd::kir::KBuiltin::LocalInvocationIndex), DType::U32);
        const int wg   = g.cast(g.builtin(crd::kir::KBuiltin::WorkgroupIndex), DType::F32);
        const int tri  = mc.dvd(tid, mc.ku(3U));
        const int corn = g.cast(mc.sub(tid, mc.mul(tri, mc.ku(3U))), DType::I32);
        const int trif = g.cast(tri, DType::F32);
        // one triangle column per (workgroup, triangle) slot: xc = -0.8 + slot·0.2 — the proven grid layout
        const int slot = mc.add(mc.mul(wg, mc.kf(static_cast<double>(desc.mesh.max_primitives))), trif);
        const int xc   = mc.add(mc.kf(-0.8), mc.mul(slot, mc.kf(0.2)));
        const int eq0  = g.binary(KOp::CmpEq, corn, g.constant(0.0, crd::kir::make_shape({1}), DType::I32));
        const int eq1  = g.binary(KOp::CmpEq, corn, g.constant(1.0, crd::kir::make_shape({1}), DType::I32));
        const int ox   = g.select(eq0, mc.kf(0.0), g.select(eq1, mc.kf(0.08), mc.kf(-0.08)));
        const int oy   = g.select(eq0, mc.kf(0.6), mc.kf(-0.6));
        int       p3   = g.vec3(mc.add(xc, ox), oy, mc.kf(0.0));
        crd::containers::Array<int> built(crd::memory::default_allocator());
        const auto input_of = [&](const VertInput& vi) {
            if (vi.kind != VertInputKind::Attribute) { return -1; }
            const crd::i32 ai = find_attr(desc.attrs, sv_of(vi.name));
            return ai >= 0 && desc.attrs[static_cast<crd::usize>(ai)].kind == AttrKind::Position ? p3 : -1;
        };
        if (!build_stage_nodes(desc, g, mc, input_of, no_custom_op, built)) { return false; }
        if (!desc.displace.empty())
        {
            int disp = -1;
            for (crd::usize s = 0; s < desc.nodes.size(); ++s)
            {
                if (str_eq(desc.nodes[s].name, sv_of(desc.displace))) { disp = built[s]; }
            }
            if (disp < 0 || g.node(disp).comps() != 3) { return false; }
            p3 = disp;
        }
        ve.stage           = crd::kir::KStage::Mesh;
        ve.mesh_vertices   = desc.mesh.max_vertices;
        ve.mesh_primitives = desc.mesh.max_primitives;
        ve.local_size[0]   = desc.mesh.workgroup;
        ve.mesh_payload_in = desc.mesh.payload; // REN-38-F6+: the declared task pairing
        const int lid      = g.cast(g.builtin(crd::kir::KBuiltin::LocalInvocationIndex), DType::U32);
        const int i0       = mc.mul(lid, mc.ku(3U));
        ve.mesh_prim       = g.vec3(i0, mc.add(i0, mc.ku(1U)), mc.add(i0, mc.ku(2U)));
        ve.position = g.vec4(g.swizzle(p3, 0), g.swizzle(p3, 1), g.swizzle(p3, 2), mc.kf(1.0));
        return emit_procedural_varyings(desc, g, mc, ve, built, mc.kf(1.0));
    }
    // ── ⭐⭐ REN-38-F7: the PROCEDURAL VERTEX stage — `position = "node:…"`. ─────────────────────────────────
    // No vertex record at all: the clip position IS a node value, and the graph's inputs are the EXPANSION
    // indices (instance = vid / N, corner = vid − instance·N), the per-instance record words and the header
    // words of the declared draw contract. This is the vocabulary that lets the debug-draw line quad, the
    // corner-table tessellation VS and the fullscreen pair be declarations instead of C++ builders.
    if (desc.stage == StageKind::Vertex && !desc.position_node.empty())
    {
        Vx        pc(g);
        const int vid    = g.cast(g.builtin(crd::kir::KBuiltin::VertexIndex), DType::U32);
        const int n_vpi  = pc.ku(desc.expand.verts_per_instance);
        const int inst   = pc.dvd(vid, n_vpi);
        const int corner = pc.sub(vid, pc.mul(inst, n_vpi));
        const int base   = desc.expand.instance_words > 0U
                               ? pc.add(pc.ku(desc.expand.instance_off),
                                        pc.mul(inst, pc.ku(desc.expand.instance_words)))
                               : -1;
        // packed RGBA8 → vec4 in [0,1], R in the low byte (Color::packed_rgba()'s layout)
        const auto unpack_rgba8 = [&](int packed) {
            const int m255 = pc.ku(0xFFU);
            const int inv  = pc.kf(1.0 / 255.0);
            const auto ch  = [&](crd::u32 shift) {
                const int bits =
                    g.binary(KOp::BitAnd, g.binary(KOp::Shr, packed, pc.ku(shift)), m255);
                return pc.mul(g.cast(bits, DType::F32), inv);
            };
            return g.vec4(ch(0U), ch(8U), ch(16U), ch(24U));
        };
        const auto input_of = [&](const VertInput& vi) {
            switch (vi.kind)
            {
            case VertInputKind::Corner:   return g.cast(corner, DType::F32);
            case VertInputKind::Instance: return g.cast(inst, DType::F32);
            case VertInputKind::Field:    return pc.loadf(pc.add(base, pc.ku(vi.word)));
            case VertInputKind::FieldU:   return pc.loadu(pc.add(base, pc.ku(vi.word)));
            case VertInputKind::FieldC:   return unpack_rgba8(pc.loadu(pc.add(base, pc.ku(vi.word))));
            case VertInputKind::Hdr:      return pc.hdrf(vi.word);
            case VertInputKind::HdrU:     return pc.hdru(vi.word);
            case VertInputKind::HdrC:     return unpack_rgba8(pc.hdru(vi.word));
            case VertInputKind::Category:
            {
                // visible ⇔ ((header[mask] >> ((flags >> 2) & 0xF)) & 1) != 0 — as 1.0 / 0.0, so the graph
                // composes it with `mix` rather than needing bool vocabulary
                const int flg = pc.loadu(pc.add(base, pc.ku(desc.expand.category_field)));
                const int cat = g.binary(KOp::BitAnd, g.binary(KOp::Shr, flg, pc.ku(2U)), pc.ku(0xFU));
                const int bit =
                    g.binary(KOp::BitAnd, g.binary(KOp::Shr, pc.hdru(desc.expand.category_mask_word), cat),
                             pc.ku(1U));
                return g.select(g.binary(KOp::CmpNe, bit, pc.ku(0U)), pc.kf(1.0), pc.kf(0.0));
            }
            case VertInputKind::Attribute: // no vertex record behind a procedural stage
            case VertInputKind::Literal:
            case VertInputKind::Node:
            default:                      return -1;
            }
        };
        const auto custom_op = [&](const crd::containers::String& op, const int* in, crd::u32 n_in) {
            if (!str_eq(op, std::string_view("view_proj"))) { return -2; }
            if (n_in != 1U || in[0] < 0 || g.node(in[0]).comps() != 3) { return -1; }
            int vp[16];
            for (crd::u32 e = 0; e < 16U; ++e) { vp[e] = pc.hdrf(desc.header.view_proj + e); }
            int clip[4];
            pc.mul_mat4(static_cast<const int*>(vp), g.swizzle(in[0], 0), g.swizzle(in[0], 1),
                        g.swizzle(in[0], 2), pc.kf(1.0), clip);
            return g.vec4(clip[0], clip[1], clip[2], clip[3]);
        };
        crd::containers::Array<int> built(crd::memory::default_allocator());
        if (!build_stage_nodes(desc, g, pc, input_of, custom_op, built)) { return false; }
        int pos = -1;
        for (crd::usize s = 0; s < desc.nodes.size(); ++s)
        {
            if (str_eq(desc.nodes[s].name, sv_of(desc.position_node))) { pos = built[s]; }
        }
        // ⛔ the clip position is a vec4 or it is nothing — a narrower node would rasterize garbage w
        if (pos < 0 || g.node(pos).comps() != 4) { return false; }
        ve.stage    = crd::kir::KStage::Vertex;
        ve.position = pos;
        return emit_procedural_varyings(desc, g, pc, ve, built, g.swizzle(pos, 3));
    }
    Vx c(g);
    if (desc.rebase_table != 0U)
    {
        // ⭐⭐ REN-38: WHICH REGION AM I — the draw table (absolute) holds each draw's region base; DrawIndex
        // picks the row (push constant + gl_DrawID on Vulkan, the root constant on DX12). Set AFTER the table
        // read (the table itself is absolute), BEFORE any other load, so the whole pull chain rebases.
        const int di = g.cast(g.builtin(crd::kir::KBuiltin::DrawIndex), DType::U32);
        c.base       = c.loadu(c.add(c.ku(desc.rebase_table), di));
    }

    // ── the pull address. One draw covers N instances of one mesh, so the vertex id carries both: the instance
    // is the quotient and the local index the remainder. (The GEO-1 idiom, scaled to instancing.)
    const int vid   = g.cast(g.builtin(crd::kir::KBuiltin::VertexIndex), DType::U32);
    const int idxc  = c.hdru(desc.header.index_count);
    const int ii    = c.dvd(vid, idxc);
    const int li    = c.sub(vid, c.mul(ii, idxc));
    const int vidx  = c.loadu(c.add(c.hdru(desc.header.index_off), li));
    const int vbase = c.add(c.hdru(desc.header.vertex_off), c.mul(vidx, c.ku(desc.vertex_stride)));
    const int slot  = c.loadu(c.add(c.hdru(desc.header.visible_off), ii));
    const int ibase = c.add(c.hdru(desc.header.instance_off), c.mul(slot, c.ku(desc.instance.stride)));

    crd::containers::Array<AttrVals> vals(crd::memory::default_allocator());
    for (crd::usize i = 0; i < desc.attrs.size(); ++i)
    {
        AttrVals a;
        a.comps = desc.attrs[i].comps;
        for (crd::u32 k = 0; k < a.comps; ++k)
        {
            a.obj[k] = c.loadf(c.add(vbase, c.ku(desc.attrs[i].offset + k)));
        }
        vals.push_back(a);
    }

    // ── 38-D2 MORPH TARGETS. Applied FIRST, to the raw attribute, because a blend shape is authored against the
    // rest pose — morphing after skinning would deform in the animated frame and the face would slide.
    // ⛔ Layout is VERTEX-MAJOR ((vidx·targets + t)·stride): all of one vertex's deltas are contiguous, so a
    // morphing vertex touches one cache line instead of `targets` of them, and no vertex COUNT is needed.
    if (desc.morph.target_count > 0U)
    {
        for (crd::u32 t = 0; t < desc.morph.target_count; ++t)
        {
            const int w = c.loadf(c.add(c.hdru(desc.header.morph_weights), c.ku(t)));
            const int mb =
                c.add(c.hdru(desc.header.morph_off),
                      c.mul(c.add(c.mul(vidx, c.ku(desc.morph.target_count)), c.ku(t)), c.ku(desc.morph.stride)));
            for (crd::usize a = 0; a < desc.morph.targets_apply_to.size(); ++a)
            {
                const crd::i32 ai = find_attr(desc.attrs, sv_of(desc.morph.targets_apply_to[a]));
                if (ai < 0) { continue; }
                AttrVals& av = vals[static_cast<crd::usize>(ai)];
                for (crd::u32 k = 0; k < av.comps; ++k)
                {
                    av.obj[k] = c.add(av.obj[k], c.mul(w, c.loadf(c.add(mb, c.ku(k)))));
                }
            }
        }
    }

    // ── 38-D2 SKINNING.
    if (desc.skin.scheme != SkinScheme::None)
    {
        const int sbase = c.add(c.hdru(desc.header.skin_off), c.mul(vidx, c.ku(desc.skin.stride)));
        int       joints[kMaxInfluences];
        const int mask = c.ku(0xFFFFU);
        for (crd::u32 k = 0; k < desc.skin.influences; ++k)
        {
            const int word = c.loadu(c.add(sbase, c.ku(k / 2U)));
            joints[k]      = (k % 2U) == 0U ? g.binary(KOp::BitAnd, word, mask) : g.binary(KOp::Shr, word, c.ku(16U));
        }
        int weights[kMaxInfluences];
        for (crd::u32 k = 0; k < desc.skin.influences; ++k)
        {
            weights[k] = c.loadf(c.add(sbase, c.ku(desc.skin.weight_off + k)));
        }
        const int pbase = c.add(c.hdru(desc.header.palette_off),
                                c.mul(slot, c.mul(c.hdru(desc.header.joint_count), c.ku(desc.skin.palette_stride))));

        if (desc.skin.scheme == SkinScheme::LinearBlend)
        {
            // LBS: Σ wₖ·(Mₖ·p). Blending the TRANSFORMED points is affine-equivalent to blending the matrices,
            // and it is one matrix load per influence instead of a 16-way accumulate.
            crd::containers::Array<AttrVals> acc(crd::memory::default_allocator());
            for (crd::usize i = 0; i < vals.size(); ++i)
            {
                AttrVals z;
                z.comps = vals[i].comps;
                for (crd::u32 k = 0; k < z.comps; ++k) { z.obj[k] = c.kf(0.0); }
                acc.push_back(z);
            }
            for (crd::u32 k = 0; k < desc.skin.influences; ++k)
            {
                const int mb = c.add(pbase, c.mul(joints[k], c.ku(desc.skin.palette_stride)));
                int       m[16];
                for (crd::u32 e = 0; e < 16U; ++e) { m[e] = c.loadf(c.add(mb, c.ku(e))); }
                for (crd::usize i = 0; i < vals.size(); ++i)
                {
                    const VertexAttrDesc& ad = desc.attrs[i];
                    if (ad.kind == AttrKind::Value) { continue; }
                    const AttrVals& src = vals[i];
                    int             out[4];
                    if (ad.kind == AttrKind::Position)
                    {
                        c.mul_mat4(static_cast<const int*>(m), src.obj[0], src.obj[1], src.obj[2], c.kf(1.0), out);
                    }
                    else
                    {
                        c.mul_mat3(static_cast<const int*>(m), src.obj[0], src.obj[1], src.obj[2], out);
                        out[3] = src.obj[3];
                    }
                    for (crd::u32 e = 0; e < src.comps && e < 3U; ++e)
                    {
                        acc[i].obj[e] = c.add(acc[i].obj[e], c.mul(out[e], weights[k]));
                    }
                }
            }
            for (crd::usize i = 0; i < vals.size(); ++i)
            {
                if (desc.attrs[i].kind == AttrKind::Value) { continue; }
                for (crd::u32 e = 0; e < vals[i].comps && e < 3U; ++e) { vals[i].obj[e] = acc[i].obj[e]; }
            }
        }
        else
        {
            // ⭐ DQS: blend the RIGID parts as unit dual quaternions. LBS interpolates matrices, so a joint twisted
            // 180° averages toward a DEGENERATE one and the limb collapses to the axis — the candy-wrapper
            // artifact. Blending rotations as quaternions cannot produce a shrinking transform.
            int q0[4];
            int qe[4];
            const int mb0 = c.add(pbase, c.mul(joints[0], c.ku(desc.skin.palette_stride)));
            for (crd::u32 e = 0; e < 4U; ++e)
            {
                q0[e] = c.mul(c.loadf(c.add(mb0, c.ku(e))), weights[0]);
                qe[e] = c.mul(c.loadf(c.add(mb0, c.ku(4U + e))), weights[0]);
            }
            const int ref0[4] = {c.loadf(c.add(mb0, c.ku(0U))), c.loadf(c.add(mb0, c.ku(1U))),
                                 c.loadf(c.add(mb0, c.ku(2U))), c.loadf(c.add(mb0, c.ku(3U)))};
            for (crd::u32 k = 1; k < desc.skin.influences; ++k)
            {
                const int mb = c.add(pbase, c.mul(joints[k], c.ku(desc.skin.palette_stride)));
                int       r[4];
                int       d[4];
                for (crd::u32 e = 0; e < 4U; ++e)
                {
                    r[e] = c.loadf(c.add(mb, c.ku(e)));
                    d[e] = c.loadf(c.add(mb, c.ku(4U + e)));
                }
                // ⛔ ANTIPODAL SIGN CORRECTION. q and -q are the SAME rotation, so a joint whose quaternion
                // happens to be stored negated relative to the first would blend toward the LONG way round — the
                // limb spins through the body between two frames that are visually adjacent.
                int dot = c.mul(ref0[0], r[0]);
                for (crd::u32 e = 1; e < 4U; ++e) { dot = c.add(dot, c.mul(ref0[e], r[e])); }
                const int sign = g.select(g.binary(KOp::CmpLt, dot, c.kf(0.0)), c.kf(-1.0), c.kf(1.0));
                const int w    = c.mul(weights[k], sign);
                for (crd::u32 e = 0; e < 4U; ++e)
                {
                    q0[e] = c.add(q0[e], c.mul(r[e], w));
                    qe[e] = c.add(qe[e], c.mul(d[e], w));
                }
            }
            // normalize by |q0| (both parts — the dual part scales with the real one)
            int len2 = c.mul(q0[0], q0[0]);
            for (crd::u32 e = 1; e < 4U; ++e) { len2 = c.add(len2, c.mul(q0[e], q0[e])); }
            const int inv = c.dvd(c.kf(1.0), g.unary(KOp::Sqrt, g.binary(KOp::Max, len2, c.kf(1e-12))));
            for (crd::u32 e = 0; e < 4U; ++e)
            {
                q0[e] = c.mul(q0[e], inv);
                qe[e] = c.mul(qe[e], inv);
            }
            const int qv[3] = {q0[0], q0[1], q0[2]};
            const int qw    = q0[3];
            const int ev[3] = {qe[0], qe[1], qe[2]};
            const int ew    = qe[3];
            // rotate(v) = v + 2·(qv × (qv × v + qw·v))
            const auto rotate = [&](const int v[3], int out[3]) {
                int t0[3];
                for (crd::u32 e = 0; e < 3U; ++e) { t0[e] = c.mul(qw, v[e]); }
                int cr[3];
                c.cross(qv, v, cr);
                for (crd::u32 e = 0; e < 3U; ++e) { cr[e] = c.add(cr[e], t0[e]); }
                int cr2[3];
                c.cross(qv, static_cast<const int*>(cr), cr2);
                for (crd::u32 e = 0; e < 3U; ++e) { out[e] = c.add(v[e], c.mul(c.kf(2.0), cr2[e])); }
            };
            // translation = 2·(qw·ev − ew·qv + qv × ev)
            int tcross[3];
            c.cross(qv, ev, tcross);
            int trans[3];
            for (crd::u32 e = 0; e < 3U; ++e)
            {
                trans[e] = c.mul(c.kf(2.0), c.add(c.sub(c.mul(qw, ev[e]), c.mul(ew, qv[e])), tcross[e]));
            }
            for (crd::usize i = 0; i < vals.size(); ++i)
            {
                const VertexAttrDesc& ad = desc.attrs[i];
                if (ad.kind == AttrKind::Value) { continue; }
                const int v[3] = {vals[i].obj[0], vals[i].obj[1], vals[i].obj[2]};
                int       r[3];
                rotate(static_cast<const int*>(v), r);
                for (crd::u32 e = 0; e < vals[i].comps && e < 3U; ++e)
                {
                    vals[i].obj[e] = ad.kind == AttrKind::Position ? c.add(r[e], trans[e]) : r[e];
                }
            }
        }
    }

    // ── 38-D3 DISPLACEMENT. In OBJECT space, after skinning, so a displacement follows an animated character
    // instead of being applied in a pose the mesh has already left.
    crd::containers::Array<int> built(crd::memory::default_allocator());
    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        const VertNodeDesc& n = desc.nodes[i];
        crd::i32            oi = -1;
        for (crd::u32 k = 0; k < crd::matcook::material_op_count(); ++k)
        {
            if (str_eq(n.op, crd::matcook::material_op_name(k))) { oi = static_cast<crd::i32>(k); }
        }
        if (oi < 0) { return false; }
        int in[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        for (crd::usize k = 0; k < n.inputs.size() && k < 8U; ++k)
        {
            const VertInput& vi = n.inputs[k];
            if (crd::matcook::material_op_arg_is_attr(static_cast<crd::u32>(oi), static_cast<crd::u32>(k)))
            {
                in[k] = static_cast<int>(vi.value[0]);
                continue;
            }
            if (vi.kind == VertInputKind::Attribute)
            {
                const crd::i32 ai = find_attr(desc.attrs, sv_of(vi.name));
                if (ai < 0) { return false; }
                const AttrVals& av = vals[static_cast<crd::usize>(ai)];
                in[k] = c.vecn(static_cast<const int*>(av.obj), av.comps);
            }
            else if (vi.kind == VertInputKind::Node)
            {
                in[k] = -1;
                for (crd::usize s = 0; s < i; ++s)
                {
                    if (str_eq(desc.nodes[s].name, sv_of(vi.name))) { in[k] = built[s]; }
                }
                if (in[k] < 0) { return false; }
            }
            else
            {
                int comps[4];
                for (crd::u32 e = 0; e < vi.comps && e < 4U; ++e) { comps[e] = c.kf(vi.value[e]); }
                in[k] = c.vecn(static_cast<const int*>(comps), vi.comps);
            }
        }
        const int r = crd::matcook::material_build_op(g, crd::containers::StringView(n.op.c_str(), n.op.size()),
                                                      static_cast<const int*>(in),
                                                      static_cast<crd::u32>(n.inputs.size()));
        if (r < 0) { return false; }
        built.push_back(r);
    }
    // The displacement node REPLACES the object position, so an author expresses "p + wave" as a node graph that
    // reads `@position` — not as an implicit addition the asset cannot see.
    if (!desc.displace.empty())
    {
        int disp = -1;
        for (crd::usize s = 0; s < desc.nodes.size(); ++s)
        {
            if (str_eq(desc.nodes[s].name, sv_of(desc.displace))) { disp = built[s]; }
        }
        if (disp < 0) { return false; }
        for (crd::usize i = 0; i < desc.attrs.size(); ++i)
        {
            if (desc.attrs[i].kind != AttrKind::Position) { continue; }
            for (crd::u32 e = 0; e < vals[i].comps && e < 3U; ++e) { vals[i].obj[e] = g.swizzle(disp, static_cast<int>(e)); }
            break;
        }
    }

    // ── the WORLD transform.
    int m[16];
    for (crd::u32 e = 0; e < 16U; ++e) { m[e] = c.loadf(c.add(ibase, c.ku(desc.instance.transform + e))); }
    int nm[16];
    if (desc.instance.has_normal_transform)
    {
        for (crd::u32 e = 0; e < 16U; ++e) { nm[e] = c.loadf(c.add(ibase, c.ku(desc.instance.normal_transform + e))); }
    }
    else
    {
        for (crd::u32 e = 0; e < 16U; ++e) { nm[e] = m[e]; }
    }
    for (crd::usize i = 0; i < desc.attrs.size(); ++i)
    {
        AttrVals& a = vals[i];
        if (desc.attrs[i].kind == AttrKind::Position)
        {
            int out[4];
            c.mul_mat4(static_cast<const int*>(m), a.obj[0], a.obj[1], a.obj[2], c.kf(1.0), out);
            for (crd::u32 e = 0; e < a.comps && e < 3U; ++e) { a.wld[e] = out[e]; }
            if (a.comps == 4U) { a.wld[3] = out[3]; }
        }
        else if (desc.attrs[i].kind == AttrKind::Direction)
        {
            int out[3];
            c.mul_mat3(static_cast<const int*>(nm), a.obj[0], a.obj[1], a.obj[2], out);
            for (crd::u32 e = 0; e < a.comps && e < 3U; ++e) { a.wld[e] = out[e]; }
            // the handedness sign of a 4-component tangent is not a coordinate — it passes through untouched
            if (a.comps == 4U) { a.wld[3] = a.obj[3]; }
        }
        else
        {
            for (crd::u32 e = 0; e < a.comps; ++e) { a.wld[e] = a.obj[e]; }
        }
    }

    // ── the CLIP transform.
    int       clip[4]  = {-1, -1, -1, -1};
    crd::i32  pos_attr = -1;
    for (crd::usize i = 0; i < desc.attrs.size(); ++i)
    {
        if (desc.attrs[i].kind == AttrKind::Position) { pos_attr = static_cast<crd::i32>(i); break; }
    }
    if (desc.transform == VertexTransform::None)
    {
        if (pos_attr < 0) { return false; }
        const AttrVals& p = vals[static_cast<crd::usize>(pos_attr)];
        clip[0]           = p.wld[0];
        clip[1]           = p.wld[1];
        clip[2]           = p.wld[2];
        clip[3]           = p.comps == 4U ? p.wld[3] : c.kf(1.0);
    }
    else
    {
        if (pos_attr < 0) { return false; }
        const crd::u32 base = desc.transform == VertexTransform::LightVp
                                  ? desc.header.light_vp + desc.cascade * 16U
                                  : desc.header.view_proj;
        int            vp[16];
        for (crd::u32 e = 0; e < 16U; ++e) { vp[e] = c.hdrf(base + e); }
        const AttrVals& p = vals[static_cast<crd::usize>(pos_attr)];
        c.mul_mat4(static_cast<const int*>(vp), p.wld[0], p.wld[1], p.wld[2], c.kf(1.0), clip);
    }

    // ── the VARYINGS.
    // ── ⭐⭐ REN-38-F1/F2/F5: THE SAME PULL PATH, DECORATED FOR ITS STAGE. ────────────────
    // Tessellation, mesh shading and the visibility buffer all read the SAME vertex record and emit the SAME
    // declared varyings — which is exactly why they belong to this declaration rather than to four parallel
    // ones that would drift from it. What differs is a handful of stage fields, set here.
    switch (desc.stage)
    {
    // F5 VISIBILITY BUFFER: the geometry pass writes an ID, not a colour — material evaluation moves to a
    // full-screen resolve, so overdraw costs a write instead of a shade. It emits the SAME declared varyings,
    // so the resolve pass is authored against the contract 38-D4 checks; the geometry side is a plain vertex
    // stage, which is why it shares this arm rather than needing its own.
    case StageKind::VisBuffer:
    case StageKind::Vertex:
    // Cull / RT stages / TessControl / TessEval / Mesh / Task never reach here — they took their own paths
    // above (⛔⛔ REN-38-F6: TessEval, Mesh and Task USED to fall through this pull tail, and the entries it
    // decorated for them could never create a program on any backend — `VertexIndex` is legal only in a vertex
    // stage, the tese/mesh emitters lower no storage loads, and a task entry may carry no position/outputs at
    // all) — but an enum switch that did not name them would silently stop covering a stage added later.
    case StageKind::Cull:
    case StageKind::RayGen:
    case StageKind::ClosestHit:
    case StageKind::Miss:
    case StageKind::AnyHit:
    case StageKind::Intersection:
    case StageKind::CallableStage:
    case StageKind::TessControl:
    case StageKind::TessEval:
    case StageKind::Mesh:
    case StageKind::Task:
        ve.stage = crd::kir::KStage::Vertex;
        break;
    }
    ve.position = g.vec4(clip[0], clip[1], clip[2], clip[3]);
    ve.n_out    = 0;
    for (crd::usize i = 0; i < desc.varyings.size(); ++i)
    {
        const VaryingDesc& v = desc.varyings[i];
        int                comps[4];
        crd::u32           w = 0U;
        for (crd::usize k = 0; k < v.source.size(); ++k)
        {
            const VaryingSource& s = v.source[k];
            if (s.kind == VaryingSourceKind::ClipW)
            {
                if (w < 4U) { comps[w++] = clip[3]; }
                continue;
            }
            if (s.kind == VaryingSourceKind::Node)
            {
                int node = -1;
                for (crd::usize n = 0; n < desc.nodes.size(); ++n)
                {
                    if (str_eq(desc.nodes[n].name, sv_of(s.name))) { node = built[n]; }
                }
                if (node < 0) { return false; }
                // ⛔ THE DECLARED WIDTH IS CROSS-CHECKED against what the graph actually built. A declaration
                // that disagreed would make 38-D4's contract check answer from the asset while the shader emitted
                // something else — the exact mismatch the contract exists to catch, one layer down.
                const auto got = static_cast<crd::u32>(g.node(node).comps());
                if (got != s.comps) { return false; }
                for (crd::u32 e = 0; e < got && w < 4U; ++e)
                {
                    comps[w++] = got == 1U ? node : g.swizzle(node, static_cast<int>(e));
                }
                continue;
            }
            if (s.kind == VaryingSourceKind::Instance)
            {
                const crd::i32 ai = find_attr(desc.instance.attrs, sv_of(s.name));
                if (ai < 0) { return false; }
                const VertexAttrDesc& ad = desc.instance.attrs[static_cast<crd::usize>(ai)];
                for (crd::u32 e = 0; e < ad.comps && w < 4U; ++e)
                {
                    comps[w++] = c.loadf(c.add(ibase, c.ku(ad.offset + e)));
                }
                continue;
            }
            const crd::i32 ai = find_attr(desc.attrs, sv_of(s.name));
            if (ai < 0) { return false; }
            const AttrVals& av = vals[static_cast<crd::usize>(ai)];
            for (crd::u32 e = 0; e < av.comps && w < 4U; ++e)
            {
                comps[w++] = s.kind == VaryingSourceKind::World ? av.wld[e] : av.obj[e];
            }
        }
        if (w == 0U || ve.n_out >= crd::kir::kMaxStageOutputs) { return false; }
        ve.out[ve.n_out] = {c.vecn(static_cast<const int*>(comps), w), static_cast<int>(v.location),
                            v.flat ? crd::kir::Interp::Flat : crd::kir::Interp::Smooth};
        ++ve.n_out;
    }
    // ⛔⛔ A TASK STAGE CARRIES NEITHER A POSITION NOR VARYINGS. It emits a workgroup COUNT; the pull path
    // above built a clip position and the declared interpolants because every other stage here wants them,
    // and `entry_valid` refuses a stage that writes a position it cannot write. Stripped once, here, rather
    // than by threading a flag through the whole builder.
    if (desc.stage == StageKind::Task)
    {
        ve.position = -1;
        ve.n_out    = 0;
    }
    return true;
}
} // namespace

bool cook_vertex_program(const VertexProgramDesc& desc, KGraph& g, crd::kir::KEntry& ve,
                         crd::kir::ShapeIssue* shape_issue)
{
    if (!cook_vertex_program_unchecked(desc, g, ve)) { return false; }
    // ⛔ THE SHAPE CHECK (REN-38 audit): a mis-built node — mismatched widths, an out-of-range component, the
    // wrong sample op for a comparison sampler — used to leave the cook with a VALID entry and fail in the
    // SHADER COMPILER, far from the asset, with nothing pointing at the cause. Every stage path ends here.
    return crd::kir::entry_shapes_valid(g, ve, desc.attrs.allocator(), shape_issue);
}

const char* vertex_cook_error_text(VertexCookError e) noexcept
{
    switch (e)
    {
    case VertexCookError::Ok:                return "ok";
    case VertexCookError::ParseFailed:       return "not valid TOML";
    case VertexCookError::BadSchema:         return "missing or unsupported `schema`";
    case VertexCookError::MissingName:       return "an attribute, node or varying has no name";
    case VertexCookError::DuplicateName:     return "two attributes, nodes or varyings share a name";
    case VertexCookError::AttrOutOfRecord:   return "an attribute reads past the declared stride";
    case VertexCookError::BadComponentCount: return "a component count outside 1..4";
    case VertexCookError::TooManyAttributes: return "more attributes than the cap";
    case VertexCookError::TooManyVaryings:   return "more varyings than CKIR carries";
    case VertexCookError::DuplicateLocation: return "two varyings at the same location";
    case VertexCookError::UnknownSource:     return "a source names an attribute, field or node that does not exist";
    case VertexCookError::EmptySource:       return "a varying with no source terms";
    case VertexCookError::VaryingTooWide:    return "a varying wider than 4 components";
    case VertexCookError::UnknownOp:         return "a node names an operation the registry does not have";
    case VertexCookError::WrongArity:        return "the right op with the wrong number of inputs";
    case VertexCookError::NodeCycle:         return "the displacement nodes form a CYCLE; the graph is a DAG";
    case VertexCookError::AttrNotConstant:   return "a compile-time argument was wired";
    case VertexCookError::AttrOutOfRange:    return "a compile-time argument is outside the range it accepts";
    case VertexCookError::BadSkin:           return "the skin record, influence count and palette stride disagree";
    case VertexCookError::BadMorph:          return "the morph stream is not fully declared";
    case VertexCookError::BadTransform:      return "a cascade index the header cannot hold";
    case VertexCookError::NoPosition:        return "no attribute of kind `position` — nothing to project";
    case VertexCookError::ContractMismatch:  return "the fragment side asked for a varying this VS does not emit";
    case VertexCookError::NodeWidthMismatch: return "a varying's declared node width is not what the graph built";
    case VertexCookError::BadStage:          return "a stage name the cooker does not have";
    case VertexCookError::BadTess:           return "a patch size or tess level that cannot tessellate";
    case VertexCookError::BadMesh:           return "a meshlet budget outside what a mesh pipeline promises";
    case VertexCookError::BadTask:           return "an amplification factor of zero launches nothing";
    case VertexCookError::BadRt:             return "a ray payload width outside 1..8";
    case VertexCookError::BadCull:           return "a culling workgroup outside 1..256";
    case VertexCookError::BadExpand:         return "an expansion contract a procedural input cannot resolve against";
    case VertexCookError::BadPositionNode:   return "a position node that does not exist, is not vec4, or is not a vertex stage";
    }
    return "unknown error";
}

} // namespace crd::vertcook
