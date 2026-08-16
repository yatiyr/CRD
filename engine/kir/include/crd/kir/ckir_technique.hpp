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

    // RAF-11: drop every registered technique so the library can be re-populated from scratch on a hot reload
    // (the builtins + the authored + the app techniques are re-`define`d, so nothing is lost and none duplicate).
    void clear() { m_t.clear(); }

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
inline constexpr int kCsmBindCount      = 9;
inline constexpr int kCsmOptCascades    = 0;
inline constexpr int kCsmOptPcfTaps     = 1;
// ⭐⭐ REN-40-D: the SEAM option. In the outer `cascade_blend` fraction of a cascade's footprint the shadow is
// resolved from THIS cascade and the next coarser one and lerped, so the two turn into each other instead of
// meeting at a line. ⛔ A DECLARED option, so `blend = 0` cooks the byte-identical graph it always did — the
// parity arm is structural, not a threshold.
inline constexpr int kCsmOptBlend       = 2;
inline constexpr int kCsmOptSoft        = 3;
inline constexpr int kCsmOptLightAngle  = 4;
// ⭐⭐ REN-40-D: the two knobs the blocker search used to hardcode. A scaled filter with a FIXED tap count bands
// once its taps spread further apart than a texel, so the penumbra has to be bounded — but the bound is a
// quality/cost trade the CONTENT should make, not a magic number in the compiler. Same for how many taps the
// search itself spends: the estimate is once per fragment and the filter is per-tap, so they are separate costs
// and deserve separate dials.
inline constexpr int kCsmOptSoftMaxTexels  = 5;
inline constexpr int kCsmOptSoftSearchTaps = 6;
// ⭐⭐ SHADOW DISTANCE FADE: the outermost cascade fades shadow → fully lit over the last `fade_pct` of its
// footprint, matching UE5/Unity/Frostbite's universal approach. 0 = the historical hard cutoff (bit-identical).
inline constexpr int kCsmOptFadePct = 7;
// the plain-sampled atlas (texture + sampler) — bindings 7/8 in ABI order, after the 7 above
inline constexpr int kCsmBindAtlasDepthTex  = 7;
inline constexpr int kCsmBindAtlasDepthSamp = 8;
inline constexpr crd::u32 kCsmMaxCascades = 4U;
// ⭐⭐ REN-40-D (moments): the two FORMAT-DERIVED constants the EVSM/MSM tier shares between the CONVERT shader
// and the technique's RESOLVE — one home, or the atlas and its reader drift apart.
// c = 5.54 is the largest EVSM exponent whose SQUARE (the m2 channel) still fits fp16 (e^{2·5.54} = 64510 <
// 65504); 6e-5 is the published (Peters) fp16 quantisation floor for the 4-moment Hamburger reconstruction.
inline constexpr double kEvsmExponent  = 5.54;
inline constexpr double kMsmMomentBias = 6.0e-5;

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

    // ⭐⭐ REN-40-D: the cascade BLEND fraction, clamped. ⛔ 0 means the graph below is emitted exactly as it
    // was before this option existed — no second sample, no lerp, no extra node — which is what makes
    // "blend = 0 is bit-identical" a property of the COOK rather than a tolerance in a test.
    auto blend = static_cast<double>(tc.option(kCsmOptBlend, 0.0)) * 0.01; // the option is a PERCENT
    if (!(blend > 0.0)) { blend = 0.0; }
    if (blend > 0.9) { blend = 0.9; }
    // ⭐⭐ REN-40-D: PCSS. 0 keeps the fixed-radius filter and emits not one extra node.
    const int  soft_mode = static_cast<int>(tc.option(kCsmOptSoft, 0.0));
    const double angle_r = static_cast<double>(tc.option(kCsmOptLightAngle, 27.0)) * (0.01 * 3.14159265358979 / 180.0);
    // tan(x) ~= x + x^3/3 over the range an angular RADIUS can sensibly take, saturated past ~86 deg where the
    // series stops being a tangent at all and an unbounded penumbra is meaningless anyway.
    double tan_a = 0.0;
    if (angle_r > 0.0)
    {
        tan_a = angle_r < 1.5 ? (angle_r + (angle_r * angle_r * angle_r / 3.0)) : 1.5;
    }
    // ⛔ the penumbra CAP, authored. A fixed-tap filter bands once its taps spread past a texel, so a bound is
    // real engineering rather than timidity — but the content decides where the trade sits, and a technique that
    // wants unbounded softness reaches for a filterable representation (the moment atlas), not a bigger number.
    double max_texels = static_cast<double>(tc.option(kCsmOptSoftMaxTexels, 24.0));
    if (!(max_texels > 0.0)) { max_texels = 24.0; }
    if (max_texels > 256.0) { max_texels = 256.0; }
    auto n_search = static_cast<int>(tc.option(kCsmOptSoftSearchTaps, 8.0));
    if (n_search != 4 && n_search != 8 && n_search != 16) { n_search = 8; }
    auto fade_pct = static_cast<double>(tc.option(kCsmOptFadePct, 30.0)) * 0.01;
    if (!(fade_pct > 0.0)) { fade_pct = 0.0; }
    if (fade_pct > 0.5) { fade_pct = 0.5; }

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
    // from the cascade's own matrix so no new binding can drift out of agreement with it.
    //
    // ⛔⛔ SCAR 5 — A MATRIX ELEMENT IS NOT A SCALE (found 2026-07-30; it is why four shadow gates were red).
    // `light_vp = ortho · light_view`, so its first ROW is (1/radius)·right and its third ROW is
    // −(1/range)·back: the world→clip scales are the ROW NORMS, and any single element of a row carries a
    // DIRECTION COSINE with it. The first version of this code read `vp.c0.x` and `−vp.c2.z`, which are
    // (1/radius)·right.x and (1/range)·back.z — correct only when the light basis happens to be world-aligned.
    // Measured on the two shadow gates in this repo: a straight-down light gives right = (−1,0,0) and
    // back = (0,1,0); a light slanted in the XY plane gives right = (0,0,−1) and back.z = 0. So `vp.c0.x` came
    // out NEGATIVE in one and ZERO in the other — both clamped to the 1e-9 floor, making `texel_w` 1.95e6 world
    // units and the normal offset **2.2 MILLION** units, which puts every lookup outside every cascade so `any`
    // falls to 0 and the fallback declares the pixel LIT — and `−vp.c2.z` was 0 in BOTH, making the depth bias
    // identically zero. The grazing-angle acne this code was written to kill did disappear, because the SHADOW
    // disappeared with it.
    // ⭐ The engine already knew the right derivation: `csm.cpp::recover_camera` recovers the camera's
    // projection scales as ROW LENGTHS (`r0 = |(c0.x, c1.x, c2.x)|`) for exactly this reason. One derivation,
    // two places — a scale read off a composed matrix is ALWAYS a row norm, never an element.
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
    // ⭐⭐ REN-40-D: THE RECEIVER PLANE, per cascade — how much this surface's OWN depth changes per texel of
    // lateral offset, along the light's u and v axes (Isidoro, "Shadow Mapping: GPU-based Tips and Techniques").
    // ⛔⛔ WITHOUT IT A WIDE FILTER SHADOWS ITSELF, and it does so in a way that reads as a tuning problem rather
    // than a bug: every tap is compared against the depth at the FRAGMENT, so on any surface not square-on to the
    // light a tap `k` texels away sits `k · texel · tan(tilt)` deeper than the value it is compared against, and
    // beyond ~1 texel that exceeds the bias. The blocker search then finds the RECEIVER as its own blocker, and
    // `avg` tracks the search radius instead of the caster — so the measured penumbra scaled with the CAP rather
    // than with the caster's height, stayed almost flat across a 5x height change, and grew every time the cap
    // was raised. The shadows still looked like plausible soft shadows the whole time.
    int dz_du[kCsmMaxCascades];
    int dz_dv[kCsmMaxCascades];
    for (crd::u32 ci = 0; ci < n_casc; ++ci)
    {
        const int vp = tc.binding(kCsmBindLightVp0 + static_cast<int>(ci));
        if (vp < 0) { return -1; }
        // the matrix columns, as vectors (each is a mat·unit-vector the backend compiler folds to a column read)
        const int col_x = g.mat_mul_vec(vp, g.vec4(kf(1.0), kf(0.0), kf(0.0), kf(0.0)));
        const int col_y = g.mat_mul_vec(vp, g.vec4(kf(0.0), kf(1.0), kf(0.0), kf(0.0)));
        const int col_z = g.mat_mul_vec(vp, g.vec4(kf(0.0), kf(0.0), kf(1.0), kf(0.0)));
        // ROWS 0 and 2 of the world→clip matrix (see SCAR 5): |row0| = 1/radius, |row2| = 1/depth_range,
        // both independent of how the light happens to be oriented.
        const int row0 = g.vec3(g.swizzle(col_x, 0), g.swizzle(col_y, 0), g.swizzle(col_z, 0));
        const int row2 = g.vec3(g.swizzle(col_x, 2), g.swizzle(col_y, 2), g.swizzle(col_z, 2));
        const int inv_radius = mxf(g.vlength(row0), kf(1.0e-9));
        const int inv_range  = mxf(g.vlength(row2), kf(1.0e-9));
        const int texel_w    = dvd(kf(2.0), mul(msz, inv_radius)); // world units per shadow texel, THIS cascade
        bias_scale[ci]       = mul(texel_w, inv_range);
        // ── the receiver plane (see above). The light's own axes are the NORMALISED matrix rows. ──
        const int row1 = g.vec3(g.swizzle(col_x, 1), g.swizzle(col_y, 1), g.swizzle(col_z, 1));
        const int xh   = g.normalize(row0);
        const int yh   = g.normalize(row1);
        const int zh   = g.normalize(row2);
        const int nz   = g.dot(nrm, zh);
        // ⛔ sign-PRESERVING guard. The light's +z may run either way depending on how the cascade was fitted, so
        // clamping to a positive floor would flip the plane's tilt on half of all fits — the correction would then
        // ADD the error it exists to remove, which is worse than not correcting at all. 0.05 is ~87 deg of
        // grazing; past that the normal offset already dominates and an unbounded gradient is pure noise.
        const int snz    = g.select(g.binary(KOp::CmpGt, nz, kf(0.0)), kf(1.0), kf(-1.0));
        const int nz_saf = mul(snz, mxf(g.unary(KOp::Abs, nz), kf(0.05)));
        const int per_tx = mul(texel_w, inv_range); // world->NDC depth, per texel of lateral travel
        dz_du[ci]        = sub(kf(0.0), mul(dvd(g.dot(nrm, xh), nz_saf), per_tx));
        dz_dv[ci]        = sub(kf(0.0), mul(dvd(g.dot(nrm, yh), nz_saf), per_tx));
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
    int gdu = dz_du[0]; // the receiver plane rides the SELECTED cascade, exactly like the bias and the UV
    int gdv = dz_dv[0];
    int any = inside[0];
    // ⭐⭐ REN-40-D: the COARSER NEIGHBOUR, selected in the SAME walk. Blending needs the cascade this fragment
    // would fall into next, and the only place that is known cheaply is here — a parallel select chain costs a
    // handful of nodes and no extra branching, whereas recovering it afterwards would mean repeating the
    // containment logic and giving the two chances to disagree.
    const crd::u32 nx0  = n_casc > 1U ? 1U : 0U;
    int csf_n = kf(static_cast<double>(nx0));
    int su_n  = uvs[nx0][0];
    int sv_n  = uvs[nx0][1];
    int sz_n  = uvs[nx0][2];
    int bsc_n = bias_scale[nx0];
    int gdu_n = dz_du[nx0];
    int gdv_n = dz_dv[nx0];
    int in_n  = inside[nx0];
    for (crd::u32 ci = n_casc; ci-- > 1U;)
    {
        const int hit = gt(inside[ci], kf(0.5));
        csf = g.select(hit, kf(static_cast<double>(ci)), csf);
        su  = g.select(hit, uvs[ci][0], su);
        sv  = g.select(hit, uvs[ci][1], sv);
        sz  = g.select(hit, uvs[ci][2], sz);
        bsc = g.select(hit, bias_scale[ci], bsc); // ⛔ the bias rides the SELECTED cascade, like the UV
        gdu = g.select(hit, dz_du[ci], gdu);
        gdv = g.select(hit, dz_dv[ci], gdv);
        any = mxf(any, inside[ci]);
        if (blend > 0.0)
        {
            // the neighbour is ci+1, CLAMPED at the last cascade — the outermost one has nothing coarser to
            // blend into, and it is also the one whose edge is the end of the shadowed region, where the
            // containment fallback (not a blend) is the correct behaviour.
            const crd::u32 nx = (ci + 1U < n_casc) ? (ci + 1U) : ci;
            csf_n = g.select(hit, kf(static_cast<double>(nx)), csf_n);
            su_n  = g.select(hit, uvs[nx][0], su_n);
            sv_n  = g.select(hit, uvs[nx][1], sv_n);
            sz_n  = g.select(hit, uvs[nx][2], sz_n);
            bsc_n = g.select(hit, bias_scale[nx], bsc_n);
            gdu_n = g.select(hit, dz_du[nx], gdu_n);
            gdv_n = g.select(hit, dz_dv[nx], gdv_n);
            in_n  = g.select(hit, inside[nx], in_n);
        }
    }
    {
        const int hit0 = gt(inside[0], kf(0.5));
        csf = g.select(hit0, kf(0.0), csf);
        su  = g.select(hit0, uvs[0][0], su);
        sv  = g.select(hit0, uvs[0][1], sv);
        sz  = g.select(hit0, uvs[0][2], sz);
        bsc = g.select(hit0, bias_scale[0], bsc);
        gdu = g.select(hit0, dz_du[0], gdu);
        gdv = g.select(hit0, dz_dv[0], gdv);
        if (blend > 0.0)
        {
            csf_n = g.select(hit0, kf(static_cast<double>(nx0)), csf_n);
            su_n  = g.select(hit0, uvs[nx0][0], su_n);
            sv_n  = g.select(hit0, uvs[nx0][1], sv_n);
            sz_n  = g.select(hit0, uvs[nx0][2], sz_n);
            bsc_n = g.select(hit0, bias_scale[nx0], bsc_n);
            in_n  = g.select(hit0, inside[nx0], in_n);
        }
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
    // ── ⭐⭐ REN-40-D: soft modes 2 (EVSM) and 3 (MSM) — the FILTERABLE tier. ────────────────────────────────
    // The atlas bound at 4/5 is the prefiltered MOMENT atlas (a colour array through a LINEAR sampler — the
    // renderer's binding seam decides that from the same `soft_mode` option this body reads, so the two cannot
    // disagree). Visibility reconstructs from ONE bilinear read; there is no radius, no search and no tap loop,
    // which is the entire point of paying for the prefilter.
    int vis = -1;
    if (soft_mode >= 2)
    {
        const int mom = g.tex_sample(tex, samp, g.vec3(su, sv, csf));
        if (soft_mode == 3)
        {
            vis = lighting::msm_hamburger(g, mom, sz, kf(0.0), kf(kMsmMomentBias));
        }
        else
        {
            vis = lighting::evsm_shadow(g, mom, sz, kf(kEvsmExponent), kf(kEvsmExponent), kf(1.0e-4), kf(0.25));
        }
        // the cascade cross-fade, in moment space — the SAME footprint-driven factor the PCF arm uses, resolving
        // the neighbour's moments instead of re-filtering. Structure kept parallel to the PCF blend below so a
        // reader can diff the two arms line by line.
        if (blend > 0.0)
        {
            const int mom_n = g.tex_sample(tex, samp, g.vec3(su_n, sv_n, csf_n));
            int       vis_n = -1;
            if (soft_mode == 3)
            {
                vis_n = lighting::msm_hamburger(g, mom_n, sz_n, kf(0.0), kf(kMsmMomentBias));
            }
            else
            {
                vis_n = lighting::evsm_shadow(g, mom_n, sz_n, kf(kEvsmExponent), kf(kEvsmExponent), kf(1.0e-4),
                                              kf(0.25));
            }
            const int eu   = g.unary(KOp::Abs, sub(mul(su, kf(2.0)), kf(1.0)));
            const int ev   = g.unary(KOp::Abs, sub(mul(sv, kf(2.0)), kf(1.0)));
            const int edge = g.binary(KOp::Max, eu, ev);
            int       t01  = dvd(sub(edge, kf(1.0 - blend)), kf(blend));
            t01            = g.binary(KOp::Min, mxf(t01, kf(0.0)), kf(1.0));
            t01            = mul(t01, in_n);
            vis            = add(vis, mul(t01, sub(vis_n, vis)));
        }
    }
    // ── ⭐⭐ REN-40-D: PCSS — THE FILTER RADIUS BECOMES A MEASUREMENT. ────────────────────────────────────────
    // Fixed-radius PCF gives every shadow the same softness, so a box resting ON the floor has the same blurry
    // edge as one ten metres above it — the single cue the eye uses to read contact. PCSS recovers it: search the
    // map for what is actually BLOCKING this fragment, and set the filter radius from how far away that blocker
    // is.
    //
    // ⛔⛔ THE PENUMBRA IS DERIVED FROM AN ANGLE, NOT FROM A "LIGHT SIZE". A directional light has no size; it has
    // an angular diameter, and a blocker at world distance d casts a penumbra of `d · tan(theta)`. Expressed in
    // TEXELS of this cascade that is `(z_recv − z_blk) · tan(theta) / bsc`, because `bsc` is exactly
    // `texel_world / depth_range` — the same quantity that makes the depth bias scale-invariant. So the penumbra
    // is correct at every cascade scale for free, and there is no second unit to keep in agreement.
    // ⛔ The SEARCH radius is bounded by the same physics rather than by a magic constant: the widest penumbra
    // possible is the one from a blocker at the light's near plane, which is `z_recv · tan(theta) / bsc`.
    else
    {
        int radius = kf(1.0); // in texels; 1 = the historical fixed footprint
        if (soft_mode == 1 && tan_a > 0.0)
        {
            const int dtex  = tc.binding(kCsmBindAtlasDepthTex);
            const int dsamp = tc.binding(kCsmBindAtlasDepthSamp);
            if (dtex < 0 || dsamp < 0) { return -1; }
            const int inv_bsc = dvd(kf(1.0), mxf(bsc, kf(1.0e-9)));
            const int search  = g.binary(KOp::Min, mul(mul(sz, kf(tan_a)), inv_bsc), kf(max_texels));
            // ── the blocker search: average the depths that lie BETWEEN the light and this fragment ──
            // ⛔⛔ A DISC, NOT A RING. This started life as eight taps at EXACTLY ±search — a ring — and a ring never
            // samples the middle, so the commonest blocker of all (the one directly overhead) is the one it cannot
            // see. What it averages instead is whatever happens to sit at the search radius, which makes the estimate
            // depend on the search distance rather than on the blocker distance: the measured penumbra then barely
            // moved when the caster was lifted (14 px at h=4, 17 px at h=10, where the physics asks for 3× that
            // spread) and at wide angles it went NON-MONOTONE — wider at h=2 than at h=4. Every arm still looked like
            // a plausible soft shadow.
            // The distribution is a VOGEL (golden-angle) spiral: r_i = sqrt((i+½)/N), theta_i = i·GA. It is equal-area
            // by construction, so each tap carries the same weight of the disc and the mean is unbiased; it is
            // deterministic, so no per-fragment noise and nothing for a temporal filter to chase; and it is a
            // compile-time table, so the loop unrolls exactly like the PCF one above.
            static constexpr double kDisc16[16][2] = {
                {0.176777, 0.000000},  {-0.225772, 0.206826}, {0.034558, -0.393771}, {0.284571, 0.371173},
                {-0.522223, -0.092374}, {0.494695, -0.314685}, {-0.165466, 0.615525}, {-0.315561, -0.607594},
                {0.684642, 0.250030},  {-0.712256, 0.294009}, {0.343354, -0.733729}, {0.253730, 0.808932},
                {-0.764746, -0.443186}, {0.897134, -0.197232}, {-0.547507, 0.778772}, {-0.126487, -0.976090}};
            static constexpr double kDisc8[8][2] = {
                {0.250000, 0.000000},   {-0.319290, 0.292496}, {0.048872, -0.556877}, {0.402444, 0.524918},
                {-0.738535, -0.130636}, {0.699605, -0.445031}, {-0.234004, 0.870484}, {-0.446271, -0.859268}};
            static constexpr double kDisc4[4][2] = {
                {0.353553, 0.000000}, {-0.451544, 0.413652}, {0.069116, -0.787542}, {0.569142, 0.742346}};
            // ⛔ each table is normalised for ITS OWN count — a prefix of the 16-tap spiral only reaches
            // sqrt(N/16) of the radius, so slicing one table would quietly shrink the search at low tap counts.
            const double(*disc)[2] = kDisc8;
            if (n_search == 4) { disc = kDisc4; }
            else if (n_search == 16) { disc = kDisc16; }
            int sum   = kf(0.0);
            int count = kf(0.0);
            for (int si = 0; si < n_search; ++si)
            {
                const double o[2] = {disc[si][0], disc[si][1]};
                const int ou   = mul(kf(o[0]), search); // this tap's offset, IN TEXELS
                const int ov   = mul(kf(o[1]), search);
                const int su_s = add(su, mul(ou, tsz));
                const int sv_s = add(sv, mul(ov, tsz));
                const int d    = g.swizzle(g.tex_sample(dtex, dsamp, g.vec3(su_s, sv_s, csf)), 0);
                // ⛔ the reference is the RECEIVER PLANE extended to this tap, not the depth at the fragment: the
                // surface itself is `k · texel · tan(tilt)` deeper out here, and comparing against the fragment's own
                // depth counts that as a blocker (see the plane note above).
                const int pz   = add(sz, add(mul(gdu, ou), mul(gdv, ov)));
                const int is_b = stp(d, sub(pz, bias)); // 1 when this tap is genuinely IN FRONT of the surface
                // ⛔ accumulate the GAP (plane - blocker), not the raw depth: the penumbra is driven by how far the
                // blocker is from the surface, and on a tilted receiver the raw depths are spread by the tilt alone —
                // averaging them and subtracting `sz` once folds that spread straight into the penumbra estimate.
                sum            = add(sum, mul(is_b, sub(pz, d)));
                count          = add(count, is_b);
            }
            // ⛔ NO BLOCKER ⇒ FULLY LIT, and it must be expressed as a radius of 0 rather than skipped: a divide by a
            // zero count is a NaN that propagates through the filter and paints the fragment black.
            const int avg  = dvd(sum, mxf(count, kf(1.0)));       // the mean receiver-to-blocker depth GAP
            const int pen  = mul(mul(avg, kf(tan_a)), inv_bsc);  // texels
            const int lit  = stp(count, kf(0.5));                        // 1 when count < 0.5, i.e. none found
            // ⛔ bounded by the SEARCH as well as by the authored cap: filtering over a region wider than the one
            // that was searched asserts a penumbra from evidence that was never gathered, and the disagreement grows
            // with blocker distance — precisely where the estimate is load-bearing.
            const int cap  = g.binary(KOp::Min, kf(max_texels), search);
            radius         = g.select(g.binary(KOp::CmpGt, lit, kf(0.5)), kf(0.0),
                                      g.binary(KOp::Min, mxf(pen, kf(0.5)), cap));
        }
        int occ = kf(0.0);
        for (int t = 0; t < n_taps; ++t)
        {
            // ⛔ the same receiver plane as the search. At the historical radius of one texel this is a correction
            // of a fraction of the bias; at PCSS's radii it is the difference between a soft shadow and a surface
            // that shadows itself in bands, and the two paths MUST agree about where the surface is or the filter
            // darkens exactly the fragments the search decided were lit.
            const int ou = mul(kf(taps[t][0]), radius);
            const int ov = mul(kf(taps[t][1]), radius);
            const int tu = add(su, mul(ou, tsz));
            const int tv = add(sv, mul(ov, tsz));
            const int rf = sub(add(sz, add(mul(gdu, ou), mul(gdv, ov))), bias);
            occ          = add(occ, g.tex_sample_cmp(tex, samp, g.vec3(tu, tv, csf), rf));
        }
        vis = mul(occ, kf(1.0 / static_cast<double>(n_taps)));
        // ── ⭐⭐ REN-40-D: THE SEAM, CLOSED. ──────────────────────────────────────────────────────────────────────
        // Cascades are fitted as SPHERES and selected by CONTAINMENT, so a fragment leaves one and enters the next at
        // a hard line — and the two sides differ in texel size, in bias and in filter footprint, so the line is
        // VISIBLE as a step in shadow softness even when both sides are individually correct.
        // ⛔ The blend factor is driven by how far into the cascade's own footprint the sample sits (`edge` -> 1 at
        // the border), NOT by view distance: the split distances and the sphere fit are different quantities, and a
        // distance-driven fade would drift out of agreement with the containment test that actually chose the
        // cascade — which is scar 2 in a new costume.
        // ⛔ Gated on the NEIGHBOUR's containment: at the outermost cascade there is nothing coarser, and the correct
        // behaviour there is the unshadowed fallback, not a blend into a slice that does not contain the point.
        if (blend > 0.0)
        {
            const int bias_n = mul(bsc_n, add(kf(1.0), mul(kf(1.0), slope)));
            int       occ_n  = kf(0.0);
            for (int t = 0; t < n_taps; ++t)
            {
                const int ou = mul(kf(taps[t][0]), radius);
                const int ov = mul(kf(taps[t][1]), radius);
                const int tu = add(su_n, mul(ou, tsz));
                const int tv = add(sv_n, mul(ov, tsz));
                const int rf = sub(add(sz_n, add(mul(gdu_n, ou), mul(gdv_n, ov))), bias_n);
                occ_n        = add(occ_n, g.tex_sample_cmp(tex, samp, g.vec3(tu, tv, csf_n), rf));
            }
            const int vis_n = mul(occ_n, kf(1.0 / static_cast<double>(n_taps)));
            // how far into this cascade's footprint: 0 at the centre, 1 at the border
            const int eu   = g.unary(KOp::Abs, sub(mul(su, kf(2.0)), kf(1.0)));
            const int ev   = g.unary(KOp::Abs, sub(mul(sv, kf(2.0)), kf(1.0)));
            const int edge = g.binary(KOp::Max, eu, ev);
            int       t01  = dvd(sub(edge, kf(1.0 - blend)), kf(blend));
            t01            = g.binary(KOp::Min, mxf(t01, kf(0.0)), kf(1.0));
            t01            = mul(t01, in_n); // only where the neighbour actually contains the point
            vis            = add(vis, mul(t01, sub(vis_n, vis)));
        }
    }
    // scar 2 — the fallback keys off CONTAINMENT, exactly like the selection.
    vis = add(vis, mul(sub(kf(1.0), any), sub(kf(1.0), vis)));

    // ── SHADOW DISTANCE FADE: the outermost cascade smoothly dissolves to fully lit at its border. ────────────
    // Without this, shadows snap on/off at the cascade boundary — every major engine (UE5, Unity, Frostbite)
    // fades over the last 20-30% of the outermost cascade instead.
    // ⛔ The fade uses the SELECTED cascade's own UV to measure proximity to the border, and applies ONLY when
    // `csf == n_casc - 1` (the last cascade). For fragments in inner cascades `is_last` is 0 and the multiply
    // zeroes the correction — no extra nodes emitted when `fade_pct` is 0 (bit-identical).
    if (fade_pct > 0.0)
    {
        const int eu_f = g.unary(KOp::Abs, sub(mul(su, kf(2.0)), kf(1.0)));
        const int ev_f = g.unary(KOp::Abs, sub(mul(sv, kf(2.0)), kf(1.0)));
        const int edge_f = g.binary(KOp::Max, eu_f, ev_f);
        const int is_last = stp(kf(static_cast<double>(n_casc) - 0.5), csf);
        int fade_t = dvd(sub(edge_f, kf(1.0 - fade_pct)), kf(fade_pct));
        fade_t = g.binary(KOp::Min, mxf(fade_t, kf(0.0)), kf(1.0));
        // smoothstep: 3t^2 - 2t^3
        fade_t = mul(fade_t, mul(fade_t, sub(kf(3.0), mul(kf(2.0), fade_t))));
        fade_t = mul(fade_t, is_last);
        vis = add(vis, mul(fade_t, sub(kf(1.0), vis)));
    }

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
    // ⭐⭐ REN-40-D (PCSS): THE SAME ATLAS IMAGE THROUGH A PLAIN SAMPLER. ⛔⛔ IT HAS TO BE A SECOND BINDING, and
    // this repo has already paid for believing otherwise: a blocker search needs the STORED DEPTH, and a
    // COMPARISON sampler cannot return it — `texture(sampler2DArrayShadow, ...)` yields a comparison RESULT, and
    // the read-the-depth overload simply does not exist in GLSL. A cook that reached for it produced a graph that
    // built cleanly and a shader that could never compile. One image, two samplers, both declared.
    {"shadow_atlas_depth", BindType::Texture2DArray, BindFrequency::Pass, 1U},
};
inline constexpr TechniqueOption kForwardCsmOptions[] = {
    {"cascade_count", 1, 4, 4},
    {"pcf_taps", 1, 16, 4},
    // ⭐⭐ REN-40-D: the cascade BLEND, in PERCENT of a cascade's footprint (an option is an integer axis, and a
    // fraction of a fraction is what a percent is for). 0 = the hard select this technique has always done, and
    // it cooks the byte-identical graph — the parity arm is structural.
    {"cascade_blend_pct", 0, 50, 0},
    // ⭐⭐ REN-40-D: the SOFTNESS MODEL. 0 = fixed-radius PCF (what this technique has always done and what
    // `blend = 0` parity is measured against); 1 = PCSS — a blocker search sets the filter radius per fragment,
    // so a contact point stays sharp and the same shadow softens with distance from its caster.
    {"soft_mode", 0, 3, 0},
    // ⭐ THE LIGHT'S ANGULAR RADIUS, in hundredths of a degree. ⛔ An ANGLE, not a "light size" in some
    // unspecified unit: a directional light has no size, it has an angular diameter, and the penumbra of a
    // caster at distance d is d·tan(theta) in WORLD units — a physical quantity that stays correct at every
    // cascade scale. The sun is ~0.27 deg of angular RADIUS, which is the default.
    {"light_angle_x100", 1, 2000, 27},
    // ⭐⭐ REN-40-D: the penumbra CAP, in texels of the selected cascade. Both the blocker search and the filter
    // are bounded by it, so the two can never disagree about how far a blocker was looked for. It was a literal
    // 24 in two places, which is a quality/cost trade the CONTENT should be making.
    {"soft_max_texels", 1, 256, 24},
    // ⭐ how many taps the BLOCKER SEARCH spends. Separate from `pcf_taps` because they are separate costs: the
    // search runs once per fragment to produce one number, the filter runs per tap to produce the shadow.
    {"soft_search_taps", 4, 16, 8},
    // ⭐⭐ SHADOW DISTANCE FADE: the outermost cascade fades shadow → fully lit over the last `fade_pct` percent
    // of its footprint. 0 = the historical hard cutoff; 30 = the UE5/Unity/Frostbite norm.
    {"shadow_fade_pct", 0, 50, 30},
};

[[nodiscard]] inline Technique forward_csm() noexcept
{
    Technique t;
    t.name       = "forward_csm";
    t.body       = &body_forward_csm;
    t.bindings   = kForwardCsmBindings;
    // ⛔⛔ DERIVED, exactly like `n_options` below — and for the same reason, found the same way. This was a
    // literal `3`, so declaring a FOURTH binding did nothing: the resolver loops to `n_bindings`, so the new
    // binding was never resolved, the body's `n_bindings < kCsmBindCount` guard tripped, and every shadow
    // program silently failed to build — `set_shadows_enabled` just returned false. A count kept in agreement
    // with an array BY HAND is a fact with two homes, and this header had two of them.
    t.n_bindings = static_cast<int>(sizeof(kForwardCsmBindings) / sizeof(kForwardCsmBindings[0]));
    t.options    = kForwardCsmOptions;
    // ⛔⛔ DERIVED FROM THE ARRAY, never counted by hand. This was a literal `2`, so adding a third DECLARED
    // option silently did nothing: `TechniqueContext::option` bounds-checks against `n_options` and quietly
    // returned the DEFAULT for the new axis, so the cook produced the un-blended graph while the renderer,
    // the option table and the asset all agreed the feature was on. Nothing failed; the pixels simply did not
    // change. A count that has to be kept in agreement with an array by hand is a fact with two homes.
    t.n_options  = static_cast<int>(sizeof(kForwardCsmOptions) / sizeof(kForwardCsmOptions[0]));
    return t;
}

// ── ⭐⭐ REN-40-D: THE MOMENT-ATLAS SHADER FAMILY — depth → filterable moments → separable blur. ─────────────
// The `soft = "evsm" | "msm"` tier. PCF and PCSS filter a BINARY visibility function, so their softness is
// bounded by how many comparisons a fragment can afford; a MOMENT map stores enough of the depth DISTRIBUTION
// that visibility reconstructs from ONE filtered read — softness then costs a prefilter ONCE per atlas instead
// of N taps per fragment, and hardware bilinear on the atlas is LEGAL (moments are linear in the signal, a
// depth comparison is not — filtering before comparing is exactly the thing a comparison sampler cannot do).
//
// These are fullscreen FS bodies in the technique-library style: C++ KGraph builders behind `crd://shadow/*`
// names, SELECTED by the authored frame graph exactly as `forward_csm` itself is selected by name — the asset
// owns the topology (which passes exist, what they read and write), the library owns the mathematics.
// ⛔ The cascade LAYER is baked per instance program, the same rule the per-cascade shadow VS uses ("the
// cascade index is baked as a compile-time constant") — a fullscreen pass has no per-draw channel to carry it.
//
// EVSM (Lauritzen): the atlas holds (e^{c·d}, e^{2c·d}, −e^{−c·d}, e^{−2c·d}) — the first two moments of BOTH
// exponential warps; Chebyshev on each, take the min, bleed-reduce. ⛔ c = 5.54 is FORMAT-DERIVED, not taste:
// the largest exponent whose SQUARE (the m2 channel) still fits fp16 (e^{2·5.54·1} = 64510 < 65504). A bigger c
// is a hard NaN at d = 1, a smaller one leaks more light — RGBA16F fixes this number.
// MSM (Peters–Klein): the atlas holds the first four POWER moments (d, d², d³, d⁴); the Hamburger 4-moment
// reconstruction bounds visibility from them. Its fp16 quantisation floor is the published moment bias 6e-5.
// the depth→moment CONVERT: read the raw depth atlas layer, emit this soft mode's 4-channel moment vector.
// `soft_mode` is 2 (EVSM) or 3 (MSM) — the same declared option axis the resolve in `body_forward_csm` reads,
// so the convert and the resolve cannot disagree about what the atlas holds.
// CEIR-18p: the moment programs are LOAD-TIME SPECIALIZED — cascade layer, blur direction, and blur tap-spacing
// (1/map_size) are D12 SPEC-CONSTANT nodes (they round-trip through the `.ckir` form via axes+iidx, default-elided), and
// the host sets each per moment_prog[kind][index] slot via KGraph::set_spec_const after ckir_read + before create_program.
// ⛔ inv_size is a SPEC-CONST fed from the LIVE csm.map_size, NOT a baked constant (which would freeze the config — the
// §128 class) and NOT a TexSize derive: the atlas is a sampler2DArray whose GLSL textureSize is ivec3, but KOp::TexSize
// types a Tex2D as ivec2 — a rank mismatch that miscompiles on materialization. These ids name the three runtime knobs;
// ⛔ keep them in sync with the host patch sites in ensure_moment_program.
inline constexpr crd::u32 kMomentLayerSpec = 0U; // the cascade layer — the array texture's z coordinate
inline constexpr crd::u32 kMomentDirSpec   = 1U; // blur direction: 1.0 = horizontal (offset in x), 0.0 = vertical (y)
inline constexpr crd::u32 kMomentInvSpec   = 2U; // blur tap spacing = 1/map_size — host sets from the LIVE csm config

// CEIR-18p: the velocity FS's backend-dependent clip-Y sign is a SPEC-CONST so the ONE asset serves both backends
// (host patches it per backend after ckir_read, like the TAA sgn). Its own program ⇒ its own constant_id space.
inline constexpr crd::u32 kVelocityVsgnSpec = 0U; // -0.5 on a y-up backend (DX12), +0.5 on y-down (Vulkan)

[[nodiscard]] inline int body_moment_convert(KGraph& g, int soft_mode, crd::u32 layer)
{
    const auto sh1 = make_shape({1});
    const auto kf  = [&](double v) { return g.constant(v, sh1, DType::F32); };
    const int  uv  = g.stage_in(KType::vec(DType::F32, 2), 0, Interp::Smooth);
    // ⛔⛔ REN-40-D: this is a FULLSCREEN pass, so its single read rides the fullscreen sampler seam — texture at
    // (0,1) + PLAIN sampler at (0,2), EXACTLY as `body_hzb_build` and `body_moment_blur` declare. (The scene
    // forward's PCSS blocker search reads its plain sampler at (0,6), but that seam is bound by the SCENE
    // executor, not the fullscreen one — a convert copying it left (0,6) unbound and sampled 0 → every moment
    // shadow rendered black.) The COMPARISON sampler the executor would otherwise bind for a depth read (a
    // comparison sampler cannot return the stored value — the PCSS scar) is suppressed by the pass's
    // `depth_as_float = true`, which routes the read through draw_textured's plain sampler at (0,2).
    const int tex  = g.texture(0, 1, DType::F32, TexDim::Tex2D, /*arrayed=*/true, /*ms=*/false, /*shadow=*/false);
    const int samp = g.sampler(0, 2, /*shadow=*/false);
    const int lay  = g.spec_constant(kMomentLayerSpec, static_cast<double>(layer), DType::F32); // host patches per cascade
    const int d    = g.swizzle(g.tex_sample(tex, samp, g.vec3(g.swizzle(uv, 0), g.swizzle(uv, 1), lay)), 0);
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    if (soft_mode == 3) // MSM: the four power moments
    {
        const int d2 = mul(d, d);
        return g.vec4(d, d2, mul(d2, d), mul(d2, d2));
    }
    // EVSM: both warps and their squares
    const int wp = g.unary(KOp::Exp, mul(kf(kEvsmExponent), d));
    const int wn = g.binary(KOp::Sub, kf(0.0), g.unary(KOp::Exp, mul(kf(-kEvsmExponent), d)));
    return g.vec4(wp, mul(wp, wp), wn, mul(wn, wn));
}

// one axis of the separable Gaussian prefilter over the moment atlas. σ = 1.8 over 9 unit-spaced taps — wide
// enough that the reconstruction has a real distribution to work with, narrow enough that the kernel stays
// inside the guard band a cascade's fit margin provides. `inv_size` = 1/map_size, BAKED: a fullscreen pass has
// no header binding to read the size from, and the technique-variant rule ("declare the axis, specialize on
// it") is exactly what a baked constant is.
[[nodiscard]] inline int body_moment_blur(KGraph& g, bool horizontal, crd::u32 layer, double inv_size)
{
    const auto sh1 = make_shape({1});
    const auto kf  = [&](double v) { return g.constant(v, sh1, DType::F32); };
    const int  uv  = g.stage_in(KType::vec(DType::F32, 2), 0, Interp::Smooth);
    // an ordinary COLOUR array read: texture at (0,1), the pass's own declared sampler at (0,2) — the frame
    // asset says `filter = "linear", address = "clamp"`, and CLAMP is load-bearing (a wrapped tap at the atlas
    // edge would blend the opposite border into every cascade's rim).
    const int tex  = g.texture(0, 1, DType::F32, TexDim::Tex2D, /*arrayed=*/true, /*ms=*/false, /*shadow=*/false);
    const int samp = g.sampler(0, 2, /*shadow=*/false);
    static constexpr double kW[9] = {0.01897808, 0.05589817, 0.12092091, 0.19211605, 0.22417357,
                                     0.19211605, 0.12092091, 0.05589817, 0.01897808};
    // CEIR-18p: layer, direction (1=horizontal), and inv_size (1/map_size) are SPEC-CONSTS — the host patches each per
    // slot from the LIVE csm config (see kMoment*Spec). The direction spec gates which axis carries the tap offset:
    // su = uv.x + off*dir, sv = uv.y + off*(1-dir); dir=1 => horizontal, dir=0 => vertical.
    const int lay  = g.spec_constant(kMomentLayerSpec, static_cast<double>(layer), DType::F32);
    const int dir  = g.spec_constant(kMomentDirSpec, horizontal ? 1.0 : 0.0, DType::F32);
    const int inv  = g.spec_constant(kMomentInvSpec, inv_size, DType::F32);
    const int ndir = g.binary(KOp::Sub, kf(1.0), dir); // the complementary axis weight (1 - dir)
    int       acc  = -1;
    for (int i = 0; i < 9; ++i)
    {
        const int off = g.binary(KOp::Mul, kf(static_cast<double>(i - 4)), inv); // (i-4)/map_size
        const int su  = g.binary(KOp::Add, g.swizzle(uv, 0), g.binary(KOp::Mul, off, dir));
        const int sv  = g.binary(KOp::Add, g.swizzle(uv, 1), g.binary(KOp::Mul, off, ndir));
        const int m   = g.tex_sample(tex, samp, g.vec3(su, sv, lay));
        const int w   = nodes::detail::bin(g, KOp::Mul, m, g.splat(kf(kW[i]), 4));
        acc           = acc < 0 ? w : nodes::detail::bin(g, KOp::Add, acc, w);
    }
    return acc;
}

// REN-40-G3: HZB BUILD — half-res MIN reduction of the scene depth buffer. The fullscreen fragment shader
// gathers the 2×2 bilinear footprint (textureGather) and outputs the minimum. Reverse-Z: min = farthest
// surface, so a projected AABB whose closest point (max_z) < HZB value is behind the prepass and culled.
[[nodiscard]] inline int body_hzb_build(KGraph& g)
{
    const auto sh1  = make_shape({1});
    const int  uv   = g.stage_in(KType::vec(DType::F32, 2), 0, Interp::Smooth);
    const int  tex  = g.texture(0, 1, DType::F32, TexDim::Tex2D, false, false, false);
    const int  samp = g.sampler(0, 2, false);
    const int  comp = g.constant(0.0, sh1, DType::I32);
    const int  gath = g.tex_gather(tex, samp, uv, comp);
    const int  m01  = g.binary(KOp::Min, g.swizzle(gath, 0), g.swizzle(gath, 1));
    const int  m23  = g.binary(KOp::Min, g.swizzle(gath, 2), g.swizzle(gath, 3));
    return g.binary(KOp::Min, m01, m23);
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
