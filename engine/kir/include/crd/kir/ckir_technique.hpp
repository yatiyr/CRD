#pragma once

// ckir_technique.hpp — REN-37.2 (D-007 row 140): THE LIGHTING TECHNIQUE AS A FIRST-CLASS, AUTHORED THING.
//
// ADR-0102 splits a shader in two: the MATERIAL is a surface response (`ckir_material.hpp`'s OpenPBR slab) and the
// RENDER PATH is a lighting technique. `ckir_cook.hpp` already owned the material half — `build_fs_for_pass` builds
// the surface once and routes it per pass. But the lighting half was `shade_forward`, a FIXED FUNCTION with a
// hardcoded key light and no shadow lookup. A technique could therefore not be authored, named, swapped, or
// verified, which is precisely why CSM became C++ (there was nothing to extend but the renderer).
//
// This header makes the lighting half the same kind of thing a material is: a NAMED graph function with a DECLARED
// contract.
//
//   technique "standard_forward"
//     consumes SurfaceData (the OpenPBR slab)
//     requires bindings: <name, type, FREQUENCY>            ← verified against the frame-graph pass (REN-37.3)
//     options  <name, range, default>                       ← the declared variant axis (REN-37.7)
//     provides shade(surface, view, light, bindings) -> vec3
//
// ⛔ TWO PROVENANCES, ONE CONTRACT — the point of the whole design:
//   • a REGISTERED body  (a `TechniqueBody` C++ builder — engine or plugin code), and
//   • an AUTHORED body   (a serialized CKIR graph, SPLICED in at cook time — NO engine code, no recompile).
// Both are described by the same `Technique` record and both are checked the same way, so the authored path is not
// a lesser second path. That is what makes the top rule ("only authored frame graphs") reachable for the SHADER
// half: a user can ship a brand-new toon technique as data.
//
// ⛔ WHAT A TECHNIQUE MAY SEE. `SurfaceData` + its own declared bindings. NOTHING ELSE. The moment a technique can
// reach the material's internals (or a material can sample a shadow map), the two axes stop being independent and
// the variant matrix becomes the product of two unbounded sets. The cooker is the only thing that sees both.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_cook.hpp>
#include <crd/kir/ckir_lighting.hpp>
#include <crd/kir/ckir_material.hpp>
#include <crd/kir/ckir_serialize.hpp>

#include <crd/containers/array.hpp>

namespace crd::kir::technique
{

// ── The BINDING CONTRACT vocabulary ──────────────────────────────────────────────────────────────────────────
// ADR-0102's set-frequency model, named. A technique declares WHAT it needs and AT WHAT RATE; the engine decides
// WHERE that lands (which set, which binding). Authors never pick descriptor slots — that hand-picking is exactly
// what made shadows and albedo fight over slot 1 in the pre-37 renderer.
enum class BindFrequency : crd::u8
{
    Frame    = 0, // camera, time, exposure — one per frame
    Pass     = 1, // the shadow atlas, cascade matrices, splits — one per pass
    Material = 2, // material parameters
    Object   = 3, // per-draw / per-skin
};

// The TYPE of a declared input. Deliberately a SMALL CLOSED SET: the cooker must be able to verify a binding by
// name AND type, and an open type system cannot be verified. Adding a type is a deliberate engine change with a
// gate, exactly like adding a `FramePassKind`.
enum class BindType : crd::u8
{
    Float = 0,
    Vec2,
    Vec3,
    Vec4,
    Mat4,
    FloatArray,
    Mat4Array,
    Texture2D,
    Texture2DArray,
    TextureCube,
    Texture2DShadow,
    Texture2DArrayShadow,
};

[[nodiscard]] inline const char* bind_type_text(BindType t) noexcept
{
    switch (t)
    {
    case BindType::Float: return "float";
    case BindType::Vec2: return "vec2";
    case BindType::Vec3: return "vec3";
    case BindType::Vec4: return "vec4";
    case BindType::Mat4: return "mat4";
    case BindType::FloatArray: return "float[]";
    case BindType::Mat4Array: return "mat4[]";
    case BindType::Texture2D: return "texture2D";
    case BindType::Texture2DArray: return "texture2DArray";
    case BindType::TextureCube: return "textureCube";
    case BindType::Texture2DShadow: return "texture2DShadow";
    case BindType::Texture2DArrayShadow: return "texture2DArrayShadow";
    }
    return "?";
}

[[nodiscard]] inline const char* bind_frequency_text(BindFrequency f) noexcept
{
    switch (f)
    {
    case BindFrequency::Frame: return "frame";
    case BindFrequency::Pass: return "pass";
    case BindFrequency::Material: return "material";
    case BindFrequency::Object: return "object";
    }
    return "?";
}

// Is this binding a texture-class resource (it needs a descriptor + possibly a sampler) rather than a value?
[[nodiscard]] inline bool bind_type_is_texture(BindType t) noexcept
{
    return t == BindType::Texture2D || t == BindType::Texture2DArray || t == BindType::TextureCube
           || t == BindType::Texture2DShadow || t == BindType::Texture2DArrayShadow;
}

// Does sampling it need a COMPARISON sampler (`sampler2DShadow` / `SamplerComparisonState`)?
[[nodiscard]] inline bool bind_type_is_shadow(BindType t) noexcept
{
    return t == BindType::Texture2DShadow || t == BindType::Texture2DArrayShadow;
}

// How many NODES one binding of this type resolves to — the single place that answer is written down, so the
// resolver, the cook-time verifier and the splice ABI cannot drift apart.
//   • a TEXTURE resolves to TWO nodes: the texture and its sampler. CKIR keeps them separable (Vulkan's model),
//     and a shadow binding in particular needs a COMPARISON sampler that the author must never have to name.
//   • an ARRAY resolves to `count` nodes (its elements).
//   • everything else resolves to one.
[[nodiscard]] inline crd::u32 bind_type_node_count(BindType t, crd::u32 count) noexcept
{
    if (t == BindType::Texture2D || t == BindType::Texture2DArray || t == BindType::TextureCube
        || t == BindType::Texture2DShadow || t == BindType::Texture2DArrayShadow)
    {
        return 2U;
    }
    if (t == BindType::FloatArray || t == BindType::Mat4Array) { return count == 0U ? 1U : count; }
    return 1U;
}

struct TechniqueBinding
{
    const char*   name  = nullptr;
    BindType      type  = BindType::Float;
    BindFrequency freq  = BindFrequency::Pass;
    crd::u32      count = 1U; // array length for FloatArray/Mat4Array; 1 otherwise
};

// A DECLARED variant axis. `min_value`/`max_value` bound it so the cook can enumerate the matrix without
// discovering combinations nobody asked for (REN-37.7's "declared, not discovered").
struct TechniqueOption
{
    const char* name          = nullptr;
    int         min_value     = 0;
    int         max_value     = 0;
    int         default_value = 0;
};

// ── The technique ABI ────────────────────────────────────────────────────────────────────────────────────────
// An AUTHORED technique body is a serialized CKIR graph whose `KOp::Input` leaves ARE its parameters, by index.
// The first `kTechFixedInputs` are the fixed contract (the unpacked OpenPBR surface + the view/light frame); the
// declared bindings follow, in declaration order, one input per resolved node.
//
// ⛔ APPEND-ONLY, exactly like the surface slab and the vtables: a renumbered slot silently feeds every authored
// technique the wrong value, and nothing would fail to compile.
inline constexpr int kTiBaseColor  = 0; // vec3
inline constexpr int kTiMetallic   = 1; // float
inline constexpr int kTiRoughness  = 2; // float
inline constexpr int kTiNormal     = 3; // vec3  (already normalized)
inline constexpr int kTiEmissive   = 4; // vec3
inline constexpr int kTiOpacity    = 5; // float
inline constexpr int kTiWorldPos   = 6; // vec3
inline constexpr int kTiViewDir    = 7; // vec3  (normalized, surface -> eye)
inline constexpr int kTiLightDir   = 8; // vec3  (the direction the light TRAVELS)
inline constexpr int kTiLightColor = 9; // vec3
inline constexpr int kTechFixedInputs = 10;

// Everything a technique body gets. The bindings are ALREADY RESOLVED to node ids by the caller's resolver — a
// technique never spells a set/binding number, which is what keeps it portable across renderers.
struct TechniqueContext
{
    // The fixed contract, unpacked from the surface by `unpack_surface` so a registered body and an authored blob
    // see exactly the same values.
    int fixed[kTechFixedInputs] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    // Resolved binding nodes, flattened in declaration order (an array binding contributes `count` entries).
    const int* bindings   = nullptr;
    int        n_bindings = 0;

    // Resolved option VALUES, in declaration order. A cooked (ship) variant passes literals here; the editor
    // übershader passes the node ids of uniform reads instead (REN-37.7's dual mode) via `option_nodes`.
    const int*      option_nodes  = nullptr; // may be null ⇒ use `option_values`
    const crd::f64* option_values = nullptr;
    int             n_options     = 0;

    [[nodiscard]] int binding(int i) const noexcept
    {
        return (bindings != nullptr && i >= 0 && i < n_bindings) ? bindings[i] : -1;
    }
    [[nodiscard]] crd::f64 option(int i, crd::f64 fallback) const noexcept
    {
        return (option_values != nullptr && i >= 0 && i < n_options) ? option_values[i] : fallback;
    }
};

// A registered technique body: build the shading subgraph and return the LIT RGB node (vec3). Return -1 on any
// internal failure — never a silently-wrong graph.
using TechniqueBody = int (*)(KGraph& g, const TechniqueContext& tc, void* user);

// The technique record. Exactly ONE of `body` / `blob` is the implementation; the rest is the declared contract
// both provenances share.
struct Technique
{
    const char*             name       = nullptr;
    const TechniqueBinding* bindings   = nullptr;
    int                     n_bindings = 0;
    const TechniqueOption*  options    = nullptr;
    int                     n_options  = 0;

    TechniqueBody body = nullptr; // REGISTERED provenance (engine/plugin C++)
    void*         user = nullptr;

    const crd::u8* blob      = nullptr; // AUTHORED provenance (a serialized CKIR graph — no engine code)
    crd::u64       blob_size = 0U;

    [[nodiscard]] bool valid() const noexcept
    {
        return name != nullptr && ((body != nullptr) != (blob != nullptr && blob_size > 0U));
    }
    // Total resolved binding NODES this technique's ABI expects (arrays flattened).
    [[nodiscard]] int binding_node_count() const noexcept
    {
        int n = 0;
        for (int i = 0; i < n_bindings; ++i)
        {
            n += static_cast<int>(bind_type_node_count(bindings[i].type, bindings[i].count));
        }
        return n;
    }
};

namespace detail
{
[[nodiscard]] inline bool tech_name_eq(const char* a, const char* b) noexcept
{
    if (a == nullptr || b == nullptr) { return a == b; }
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b;
}
} // namespace detail

// A registry of techniques by name — the shader-half analogue of the built-in frame-graph pack: engine defaults
// register first and an app OVERRIDES purely by shadowing the name (`find` returns the LAST match).
class TechniqueLibrary
{
public:
    explicit TechniqueLibrary(crd::memory::IAllocator* a) : m_t(a) {}

    void define(const Technique& t) { m_t.push_back(t); }

    [[nodiscard]] const Technique* find(const char* name) const
    {
        for (crd::usize i = m_t.size(); i > 0U; --i)
        {
            if (detail::tech_name_eq(m_t[i - 1U].name, name)) { return &m_t[i - 1U]; }
        }
        return nullptr;
    }
    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_t.size()); }
    [[nodiscard]] const Technique& at(int i) const noexcept { return m_t[static_cast<crd::usize>(i)]; }

private:
    crd::containers::Array<Technique> m_t;
};

// ── SPLICING an authored graph ───────────────────────────────────────────────────────────────────────────────
// Inline `src` into `dst`, substituting each `KOp::Input` leaf of index k with `args[k]`, and return the `dst`
// node id that `src_result` maps to. This is the mechanism that lets a technique ship as DATA: the cook
// deserializes the blob into a scratch graph and splices it into the material's FS graph, after which lowering,
// specialization, content-hash dedup and emission all treat it as ordinary nodes.
//
// ⛔ Returns -1 (never a partial splice) when: an `Input` index is out of range; the graph carries KERNEL
// STATEMENTS (a fragment technique is a VALUE EXPRESSION by construction — control flow belongs in the compute
// path, and a silently-dropped statement would be a miscompile); or `src_result` is out of range.
[[nodiscard]] inline int splice_graph(KGraph& dst, const KGraph& src, const int* args, int n_args, int src_result)
{
    if (src_result < 0 || src_result >= src.size()) { return -1; }
    if (!src.serial_stmts().empty()) { return -1; }

    auto*                            al = dst.serial_nodes().allocator();
    crd::containers::Array<crd::i32> map(al);
    map.reserve(static_cast<crd::usize>(src.size()));
    for (int i = 0; i < src.size(); ++i) { map.push_back(-1); }

    // Struct ids are graph-local: a `TKind::Struct` type in `src` names src's registry, so every struct the
    // authored graph declared must be RE-DEFINED in dst and the ids remapped. Structs are processed in order, so
    // a nested struct's field type is already remapped when its parent is defined.
    crd::containers::Array<crd::i32> smap(al);
    {
        const crd::usize n_structs = src.serial_sbegin().size();
        for (crd::usize s = 0; s < n_structs; ++s)
        {
            const int             sid = static_cast<int>(s);
            const int             nf  = src.struct_field_count(sid);
            crd::containers::Array<KType> fields(al);
            fields.reserve(static_cast<crd::usize>(nf));
            for (int f = 0; f < nf; ++f)
            {
                KType ft = src.struct_field(sid, f);
                if (ft.kind == TKind::Struct)
                {
                    const crd::usize idx = static_cast<crd::usize>(ft.struct_id);
                    if (idx >= smap.size()) { return -1; } // forward reference — not producible by the builders
                    ft.struct_id = static_cast<crd::i16>(smap[idx]);
                }
                fields.push_back(ft);
            }
            smap.push_back(dst.define_struct(fields.data(), nf));
        }
    }

    crd::containers::Array<crd::i32> ext_buf(al);
    for (int i = 0; i < src.size(); ++i)
    {
        const KNode& n = src.node(i);
        if (n.op == KOp::Input)
        {
            if (n.iidx < 0 || n.iidx >= n_args || args[n.iidx] < 0) { return -1; }
            map[static_cast<crd::usize>(i)] = args[n.iidx];
            continue;
        }
        KNode c = n;
        if (c.type.kind == TKind::Struct)
        {
            const crd::usize idx = static_cast<crd::usize>(c.type.struct_id);
            if (idx >= smap.size()) { return -1; }
            c.type.struct_id = static_cast<crd::i16>(smap[idx]);
        }
        const auto remap = [&](crd::i32 o) -> crd::i32 {
            if (o < 0) { return -1; }
            if (o >= i) { return -2; } // a forward reference violates the push-order invariant
            return map[static_cast<crd::usize>(o)];
        };
        c.a = remap(n.a);
        c.b = remap(n.b);
        c.c = remap(n.c);
        c.d = remap(n.d);
        if (c.a == -2 || c.b == -2 || c.c == -2 || c.d == -2) { return -1; }

        int id = -1;
        if (n.n_ext > 0U)
        {
            ext_buf.clear();
            for (int k = 0; k < static_cast<int>(n.n_ext); ++k)
            {
                const crd::i32 o = remap(static_cast<crd::i32>(src.ext_operand(n, k)));
                if (o < 0) { return -1; }
                ext_buf.push_back(o);
            }
            id = dst.clone_with_ext(c, ext_buf.data());
        }
        else
        {
            id = dst.clone(c);
        }
        map[static_cast<crd::usize>(i)] = id;
    }
    return map[static_cast<crd::usize>(src_result)];
}

// ── Applying a technique ─────────────────────────────────────────────────────────────────────────────────────

// Unpack the OpenPBR surface into the fixed ABI slots. Both provenances go through this, so an authored blob and
// a registered body are fed BIT-IDENTICAL inputs — which is what makes the two paths interchangeable rather than
// merely similar.
inline void unpack_surface(KGraph& g, int surface, const cook::SurfaceInputs& in, int light_dir, int light_color,
                           TechniqueContext& tc)
{
    tc.fixed[kTiBaseColor] = g.field_get(surface, material::SfBaseColor);
    tc.fixed[kTiMetallic]  = g.field_get(surface, material::SfMetallic);
    tc.fixed[kTiRoughness] = g.field_get(surface, material::SfRoughness);
    tc.fixed[kTiNormal]    = g.normalize(g.field_get(surface, material::SfNormal));
    tc.fixed[kTiEmissive]  = g.field_get(surface, material::SfEmissive);
    tc.fixed[kTiOpacity]   = g.field_get(surface, material::SfOpacity);
    tc.fixed[kTiWorldPos]  = in.world_pos;
    tc.fixed[kTiViewDir]   = in.view_dir;
    tc.fixed[kTiLightDir]  = light_dir;
    tc.fixed[kTiLightColor] = light_color;
}

// Run `t` against an unpacked context. Dispatches to the registered body, or splices the authored blob using the
// ABI above. Returns the lit RGB node, or -1.
[[nodiscard]] inline int apply_technique(KGraph& g, const Technique& t, const TechniqueContext& tc)
{
    if (!t.valid()) { return -1; }
    if (t.body != nullptr) { return t.body(g, tc, t.user); }

    // AUTHORED provenance: deserialize + splice. The argument vector is the ABI: fixed slots, then the resolved
    // binding nodes in declaration order.
    auto*  al = g.serial_nodes().allocator();
    KGraph src(al);
    KEntry se;
    if (!deserialize_graph(crd::containers::ConstSpan<crd::u8>(t.blob, t.blob_size), src, se)) { return -1; }
    if (se.n_out < 1) { return -1; }

    crd::containers::Array<crd::i32> args(al);
    args.reserve(static_cast<crd::usize>(kTechFixedInputs) + static_cast<crd::usize>(tc.n_bindings));
    for (int i = 0; i < kTechFixedInputs; ++i) { args.push_back(tc.fixed[i]); }
    for (int i = 0; i < tc.n_bindings; ++i) { args.push_back(tc.bindings[i]); }
    return splice_graph(g, src, args.data(), static_cast<int>(args.size()), se.out[0].node);
}

// ── The generalized cook: build the FS for `pass` with a NAMED technique ─────────────────────────────────────
// This is `cook::build_fs_for_pass` with the one fixed function removed. Shadow / DepthPrepass / GBuffer are
// UNCHANGED — a technique only exists to shade, and the free DCE collapse for the depth-only passes (every opaque
// material cooking to the same empty FS) is preserved exactly because the technique is never invoked there.
//
// Returns false when the technique failed to apply — the caller must NOT ship a half-built shader.
[[nodiscard]] inline bool build_fs_for_pass(const cook::MaterialTemplate& mt, const Technique& tech,
                                            cook::PassType pass, const cook::VariantOptions& opts,
                                            const cook::SurfaceInputs& in, KGraph& g, KEntry& e, int light_dir,
                                            int light_color, const int* binding_nodes, int n_binding_nodes,
                                            const crd::f64* option_values, int n_option_values, bool do_lower = true)
{
    const int struct_id = material::define_surface(g);
    const int surface   = mt.build_surface(g, struct_id, in, mt.user);
    // ⛔ The template's documented failure shape is a NEGATIVE node ("never a substitute surface") — and this
    // was the one consumer that never checked it: `unpack_surface`/`field_get` indexed the node table with -1,
    // an access violation. Found by the 38-G1 override proof (a deliberately broken user `.crdm` on disk must
    // fail the cook BY NAME, not crash the app).
    if (surface < 0) { return false; }
    e.stage             = KStage::Fragment;
    switch (pass)
    {
    case cook::PassType::Shadow:
    case cook::PassType::DepthPrepass:
        e.n_out = 0; // depth-only — the surface is never consumed and lowering reclaims ALL of it
        break;
    case cook::PassType::GBuffer:
        material::pack_gbuffer(g, e, surface);
        break;
    case cook::PassType::Forward:
    {
        TechniqueContext tc;
        unpack_surface(g, surface, in, light_dir, light_color, tc);
        tc.bindings      = binding_nodes;
        tc.n_bindings    = n_binding_nodes;
        tc.option_values = option_values;
        tc.n_options     = n_option_values;
        const int lit    = apply_technique(g, tech, tc);
        if (lit < 0) { return false; }
        e.n_out  = 1;
        e.out[0] = {g.vec4(g.swizzle(lit, 0), g.swizzle(lit, 1), g.swizzle(lit, 2),
                           g.field_get(surface, material::SfOpacity)),
                    0};
        break;
    }
    }
    if (opts.alpha_mode == material::AlphaMode::Masked) { material::set_masked(g, e, surface, opts.alpha_cutoff); }
    if (do_lower) { lower::lower_entry(g, e); }
    return true;
}

// ── `standard_forward` — the first technique, and the port of the old fixed function ─────────────────────────
// Byte-for-byte the shading `cook::shade_forward` performed, now reachable BY NAME and swappable by an asset.
// Declares NO bindings: a plain directional key light needs nothing the fixed ABI does not already carry, which
// is exactly why it is the right first technique — it proves the seam without also testing the binding contract.
[[nodiscard]] inline int body_standard_forward(KGraph& g, const TechniqueContext& tc, void* /*user*/)
{
    const int lit = lighting::directional_light(g, tc.fixed[kTiBaseColor], tc.fixed[kTiMetallic],
                                                tc.fixed[kTiRoughness], tc.fixed[kTiNormal], tc.fixed[kTiViewDir],
                                                tc.fixed[kTiLightDir], tc.fixed[kTiLightColor]);
    return nodes::clamp01(g, nodes::detail::bin(g, KOp::Add, lit, tc.fixed[kTiEmissive]));
}

[[nodiscard]] inline Technique standard_forward() noexcept
{
    Technique t;
    t.name = "standard_forward";
    t.body = &body_standard_forward;
    return t;
}

// ── `unlit` — the smallest possible technique, and the proof that the axis is real ──────────────────────────
// A material tagged Unlit still authors a full surface; the TECHNIQUE is what decides the surface is emitted
// rather than shaded. Under lowering, choosing this collapses every BRDF term away — the same free DCE the shadow
// pass gets, on the lighting axis.
[[nodiscard]] inline int body_unlit(KGraph& g, const TechniqueContext& tc, void* /*user*/)
{
    return nodes::clamp01(g, nodes::detail::bin(g, KOp::Add, tc.fixed[kTiBaseColor], tc.fixed[kTiEmissive]));
}

[[nodiscard]] inline Technique unlit() noexcept
{
    Technique t;
    t.name = "unlit";
    t.body = &body_unlit;
    return t;
}

// ── `forward_csm` — CASCADED SHADOW MAPS AS A TECHNIQUE (REN-37.4) ───────────────────────────────────────────
// This is the whole reason REN-37 exists. CSM is not merely a schedule of depth passes: it changes what EVERY
// material's fragment shader computes (select a cascade, project, PCF-filter, attenuate) and it needs
// pass-frequency inputs no material author should ever mention. Before this it was ~120 lines of hand-written
// C++ inside `scene_renderer.cpp`, which is the violation the top rule was restated in anger over.
//
// Now it is a NAMED technique with a DECLARED contract, selected by an asset. Its bindings, in ABI order:
//   [0] shadow_atlas TEXTURE   [1] shadow_atlas comparison SAMPLER   [2..5] csm_light_vp mat4 x4   [6] map_size
// and its options: [0] cascade_count (1..4)   [1] pcf_taps (1|4|8|16).
//
// ⛔ THE TWO SCARS THIS CODE ENCODES, both of which read as "the shadows look wrong" and neither of which points
// at its own cause:
//   1. cascades are fitted as SPHERES (that is what makes them rotation-invariant), so selecting one by DEPTH
//      SPLIT — a slab test — lets a pixel inside the depth range sit laterally OUTSIDE the sphere. Its UV leaves
//      [0,1], the comparison sampler reads the clamped edge, and the lookup comes back LIT: a bright RING around
//      each cascade. Selection is therefore by CONTAINMENT, which is the only rule consistent with the fit.
//   2. the out-of-range FALLBACK must agree with the SELECTION. "Outside the shadowed region" means NO cascade
//      contained the point (`any == 0`), not "beyond the last split". Keying the fallback off the split while
//      selecting by containment leaves a band that is shadowed by one rule and lit by the other.
inline constexpr int kCsmBindAtlasTex   = 0;
inline constexpr int kCsmBindAtlasSamp  = 1;
inline constexpr int kCsmBindLightVp0   = 2; // .. +3
inline constexpr int kCsmBindMapSize    = 6;
inline constexpr int kCsmBindCount      = 7;
inline constexpr int kCsmOptCascades    = 0;
inline constexpr int kCsmOptPcfTaps     = 1;
inline constexpr crd::u32 kCsmMaxCascades = 4U;

[[nodiscard]] inline int body_forward_csm(KGraph& g, const TechniqueContext& tc, void* /*user*/)
{
    if (tc.n_bindings < kCsmBindCount) { return -1; }
    const auto sh  = make_shape({1});
    const auto kf  = [&](double v) { return g.constant(v, sh, DType::F32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto dvd = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto mxf = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto stp = [&](int e, int x) { return g.binary(KOp::Step, e, x); };
    const auto gt  = [&](int a, int b) { return g.binary(KOp::CmpGt, a, b); };

    const int tex  = tc.binding(kCsmBindAtlasTex);
    const int samp = tc.binding(kCsmBindAtlasSamp);
    const int msz  = tc.binding(kCsmBindMapSize);
    if (tex < 0 || samp < 0 || msz < 0) { return -1; }

    // The DECLARED options are compile-time here (ship mode). `specialize_variant` reaches the same graph from an
    // übershader in editor mode — same artifact, two lowering levels (§5).
    auto       n_casc = static_cast<crd::u32>(tc.option(kCsmOptCascades, 4.0));
    if (n_casc < 1U) { n_casc = 1U; }
    if (n_casc > kCsmMaxCascades) { n_casc = kCsmMaxCascades; }
    auto n_taps = static_cast<int>(tc.option(kCsmOptPcfTaps, 4.0));
    if (n_taps != 1 && n_taps != 4 && n_taps != 8 && n_taps != 16) { n_taps = 4; }

    const int wp = tc.fixed[kTiWorldPos]; // the projected position is built PER CASCADE (normal offset, below)

    // ── cascade selection by CONTAINMENT (scar 1) ──
    int uvs[kCsmMaxCascades][3];
    int inside[kCsmMaxCascades];
    // ⛔⛔ SCAR 3 — THE BIAS MUST BE SCALE-INVARIANT. Each cascade's ortho maps a DIFFERENT world depth span onto
    // the same [0,1], and `caster_extrusion` inflates that span further, so one constant in normalized depth is a
    // wildly different distance in each cascade (measured here: 0.2 world units in cascade 1, 2.9 in cascade 3 —
    // larger than the casters themselves). Shadows then detach from small objects entirely and survive only for
    // big ones, which reads as "the shadows are in completely wrong places" and points at neither the fit nor the
    // atlas. The bias is therefore expressed in SHADOW TEXELS and converted per cascade, both factors recovered
    // from the cascade's own matrix so no new binding can drift out of agreement with it:
    //   ortho_rh_zo puts 1/radius in c0.x and −1/(far−near) in c2.z, so
    //   texel_world = 2 / (map_size · c0.x)   and   1/depth_range = −c2.z.
    //
    // ⛔⛔ SCAR 4 — A DEPTH BIAS ALONE CANNOT SAVE A NEAR-EDGE-ON SURFACE. The depth a receiver gains across one
    // shadow texel is `texel_world · tan θ`, and tan θ diverges as the surface turns edge-on to the light — so
    // the depth bias a grazing face needs is UNBOUNDED, while any cap large enough to cover it peter-pans
    // everything else. Capping at 72° still leaves the surface 31% lit, which is exactly where the sandbox's
    // cube showed fine diagonal striping at the texel frequency.
    // The fix is NORMAL OFFSET: move the LOOKUP off the surface along its own normal, ~2 texels at grazing and
    // nothing head-on (∝ sin θ). It is bounded, it is perpendicular to the light rather than along it (so it
    // does not detach contact shadows the way depth bias does), and it is applied PER CASCADE in world units —
    // which is why it lives inside this loop, before the projection, using that cascade's own texel size.
    const int nrm     = tc.fixed[kTiNormal];
    const int ltrav   = tc.fixed[kTiLightDir]; // the direction the light TRAVELS
    const int ltoward = g.vec3(sub(kf(0.0), g.swizzle(ltrav, 0)), sub(kf(0.0), g.swizzle(ltrav, 1)),
                               sub(kf(0.0), g.swizzle(ltrav, 2)));
    const int ndl     = mxf(g.dot(nrm, g.normalize(ltoward)), kf(0.0));
    const int sin_t   = g.unary(KOp::Sqrt, mxf(sub(kf(1.0), mul(ndl, ndl)), kf(0.0)));

    int bias_scale[kCsmMaxCascades];
    for (crd::u32 ci = 0; ci < n_casc; ++ci)
    {
        const int vp = tc.binding(kCsmBindLightVp0 + static_cast<int>(ci));
        if (vp < 0) { return -1; }
        // the matrix columns, as vectors (constant-folds to a single header word each)
        const int col_x = g.mat_mul_vec(vp, g.vec4(kf(1.0), kf(0.0), kf(0.0), kf(0.0)));
        const int col_z = g.mat_mul_vec(vp, g.vec4(kf(0.0), kf(0.0), kf(1.0), kf(0.0)));
        const int inv_radius = mxf(g.swizzle(col_x, 0), kf(1.0e-9));
        const int inv_range  = sub(kf(0.0), g.swizzle(col_z, 2));
        const int texel_w    = dvd(kf(2.0), mul(msz, inv_radius)); // world units per shadow texel, THIS cascade
        bias_scale[ci]       = mul(texel_w, inv_range);
        // the normal-offset sample position, in WORLD units of this cascade's texel
        const int nofs = mul(texel_w, mul(kf(2.5), sin_t));
        const int wpo  = g.vec4(add(g.swizzle(wp, 0), mul(g.swizzle(nrm, 0), nofs)),
                                add(g.swizzle(wp, 1), mul(g.swizzle(nrm, 1), nofs)),
                                add(g.swizzle(wp, 2), mul(g.swizzle(nrm, 2), nofs)), kf(1.0));
        const int lp = g.mat_mul_vec(vp, wpo);
        const int iw = dvd(kf(1.0), mxf(g.swizzle(lp, 3), kf(1.0e-6)));
        uvs[ci][0]   = add(mul(mul(g.swizzle(lp, 0), iw), kf(0.5)), kf(0.5));
        uvs[ci][1]   = add(mul(mul(g.swizzle(lp, 1), iw), kf(0.5)), kf(0.5));
        uvs[ci][2]   = mul(g.swizzle(lp, 2), iw);
        // inside = uv within [margin, 1-margin] on both axes AND depth in [0,1]. The margin keeps the PCF taps
        // from straying off the map at the very edge, which would reintroduce the same fringe one texel wide.
        int in = stp(kf(0.02), uvs[ci][0]);
        in     = mul(in, stp(uvs[ci][0], kf(0.98)));
        in     = mul(in, stp(kf(0.02), uvs[ci][1]));
        in     = mul(in, stp(uvs[ci][1], kf(0.98)));
        in     = mul(in, stp(kf(0.0), uvs[ci][2]));
        in     = mul(in, stp(uvs[ci][2], kf(1.0)));
        inside[ci] = in;
    }
    // Walk from the LAST cascade back so the smallest (highest-resolution) containing cascade wins, then prefer
    // cascade 0 outright when it contains the point (it is the tightest of all).
    int csf = kf(0.0);
    int su  = uvs[0][0];
    int sv  = uvs[0][1];
    int sz  = uvs[0][2];
    int bsc = bias_scale[0];
    int any = inside[0];
    for (crd::u32 ci = n_casc; ci-- > 1U;)
    {
        const int hit = gt(inside[ci], kf(0.5));
        csf = g.select(hit, kf(static_cast<double>(ci)), csf);
        su  = g.select(hit, uvs[ci][0], su);
        sv  = g.select(hit, uvs[ci][1], sv);
        sz  = g.select(hit, uvs[ci][2], sz);
        bsc = g.select(hit, bias_scale[ci], bsc); // ⛔ the bias rides the SELECTED cascade, like the UV
        any = mxf(any, inside[ci]);
    }
    {
        const int hit0 = gt(inside[0], kf(0.5));
        csf = g.select(hit0, kf(0.0), csf);
        su  = g.select(hit0, uvs[0][0], su);
        sv  = g.select(hit0, uvs[0][1], sv);
        sz  = g.select(hit0, uvs[0][2], sz);
        bsc = g.select(hit0, bias_scale[0], bsc);
    }

    // ── the DEPTH bias, now a JUNIOR partner to the normal offset above. ──────────────────────────────────────
    // With the lookup already moved ~2.5 texels off the surface along its normal, this only has to cover depth
    // QUANTIZATION plus the residual slope across the ±0.5-texel PCF footprint — so it stays small and cannot
    // peter-pan. ⛔ It is still expressed in TEXELS (converted per cascade by `bsc`) and its slope term is a real
    // tan θ, not the `(1 − N·L)` proxy, which understates badly where it matters (at 60° it reads 0.5 against a
    // true slope of 1.73). The cap is what forced the normal offset to exist: tan θ is unbounded at grazing, and
    // no cap can be both large enough for an edge-on face and small enough not to detach everything else.
    const int ndl_safe = mxf(ndl, kf(0.1));
    const int slope    = g.binary(KOp::Min, dvd(sin_t, ndl_safe), kf(2.0));
    const int bias     = mul(bsc, add(kf(1.0), mul(kf(1.0), slope)));
    const int ref  = sub(sz, bias);

    // ── PCF. The tap count is a DECLARED option, so each choice cooks to its own variant with the loop fully
    // unrolled and no dynamic branch (the "declare the axis, specialize on it, dedup the result" rule).
    static constexpr double kTaps16[16][2] = {
        {-1.5, -1.5}, {-0.5, -1.5}, {0.5, -1.5}, {1.5, -1.5}, {-1.5, -0.5}, {-0.5, -0.5}, {0.5, -0.5}, {1.5, -0.5},
        {-1.5, 0.5},  {-0.5, 0.5},  {0.5, 0.5},  {1.5, 0.5},  {-1.5, 1.5},  {-0.5, 1.5},  {0.5, 1.5},  {1.5, 1.5}};
    static constexpr double kTaps8[8][2] = {{-1.0, -1.0}, {0.0, -1.0}, {1.0, -1.0}, {-1.0, 0.0},
                                            {1.0, 0.0},   {-1.0, 1.0}, {0.0, 1.0},  {1.0, 1.0}};
    static constexpr double kTaps4[4][2] = {{-0.5, -0.5}, {0.5, -0.5}, {-0.5, 0.5}, {0.5, 0.5}};
    static constexpr double kTaps1[1][2] = {{0.0, 0.0}};

    const double(*taps)[2] = kTaps4;
    if (n_taps == 1) { taps = kTaps1; }
    else if (n_taps == 8) { taps = kTaps8; }
    else if (n_taps == 16) { taps = kTaps16; }

    const int tsz = dvd(kf(1.0), mxf(msz, kf(1.0)));
    int occ = kf(0.0);
    for (int t = 0; t < n_taps; ++t)
    {
        const int tu = add(su, mul(kf(taps[t][0]), tsz));
        const int tv = add(sv, mul(kf(taps[t][1]), tsz));
        occ          = add(occ, g.tex_sample_cmp(tex, samp, g.vec3(tu, tv, csf), ref));
    }
    int vis = mul(occ, kf(1.0 / static_cast<double>(n_taps)));
    // scar 2 — the fallback keys off CONTAINMENT, exactly like the selection.
    vis = add(vis, mul(sub(kf(1.0), any), sub(kf(1.0), vis)));

    // Same surface, same BRDF as `standard_forward`, differing ONLY by the visibility term. ⛔ When the shadowed
    // path used a different (toy) shading model, ENABLING SHADOWS MADE PIXELS BRIGHTER — wrong, and invisible to
    // every gate except the one asserting `brighter == 0`.
    const int shaded = lighting::directional_light(g, tc.fixed[kTiBaseColor], tc.fixed[kTiMetallic],
                                                   tc.fixed[kTiRoughness], nrm, tc.fixed[kTiViewDir], ltrav,
                                                   tc.fixed[kTiLightColor]);
    const int lit    = g.vec3(mul(g.swizzle(shaded, 0), vis), mul(g.swizzle(shaded, 1), vis),
                              mul(g.swizzle(shaded, 2), vis));
    return nodes::clamp01(g, nodes::detail::bin(g, KOp::Add, lit, tc.fixed[kTiEmissive]));
}

// The declared contract. ⛔ Order is the ABI: the resolver fills binding nodes in exactly this order, and the
// `kCsmBind*` constants above are the single place that order is written down.
inline constexpr TechniqueBinding kForwardCsmBindings[] = {
    {"shadow_atlas", BindType::Texture2DArrayShadow, BindFrequency::Pass, 1U},
    {"csm_light_vp", BindType::Mat4Array, BindFrequency::Pass, kCsmMaxCascades},
    {"csm_map_size", BindType::Float, BindFrequency::Pass, 1U},
};
inline constexpr TechniqueOption kForwardCsmOptions[] = {
    {"cascade_count", 1, 4, 4},
    {"pcf_taps", 1, 16, 4},
};

[[nodiscard]] inline Technique forward_csm() noexcept
{
    Technique t;
    t.name       = "forward_csm";
    t.body       = &body_forward_csm;
    t.bindings   = kForwardCsmBindings;
    t.n_bindings = 3;
    t.options    = kForwardCsmOptions;
    t.n_options  = 2;
    return t;
}

// Register the engine's built-in techniques. An app calls this FIRST and then defines its own, so shadowing a
// built-in name is how you override it (the built-in-pack rule, applied to the shader half).
inline void register_builtin_techniques(TechniqueLibrary& lib)
{
    lib.define(standard_forward());
    lib.define(unlit());
    lib.define(forward_csm());
}

} // namespace crd::kir::technique
