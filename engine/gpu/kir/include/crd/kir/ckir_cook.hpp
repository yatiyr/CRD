#pragma once

// ckir_cook.hpp — the material COOK SEAM (D-007 B8-k): lower an authored CKIR material into the per-pass (VS+FS) programs a
// renderer runs. A material is authored ONCE as a surface graph (a B5 OpenPBR surface built from the per-fragment varyings);
// the cook produces the RIGHT fragment program for each render pass from that one surface:
//   • Shadow / DepthPrepass — depth-only (no colour), + alpha-test discard for a Masked material.
//   • GBuffer (deferred)    — the surface packed into the B5 MRT G-buffer (+ alpha-test).
//   • Forward               — the surface SHADED (B8 Cook-Torrance) into one lit colour (+ alpha-test).
// Every variant is run through B7 `lower_entry` (const-fold → DCE → CSE), so the cooked program is the optimized graph — and
// ROUND-TRIP BIT-STABLE (the lowered variant evaluates identically to the un-lowered, per B7). `specialize_variant` bakes a
// `ShaderOption` selector to a compile-time value (B7 `specialize`) to mint a smaller static variant.
//
// SCOPE: this is the FS pass-differentiation + lowering + variant machinery — pure CKIR graph work, buildable + testable now.
// The material VS (a vertex-attribute position transform + B8-j skinning + varying emission) and the physical bindings
// (material params → set-2 uniform block, scene lights → set-1, bone palette → set-3, reflection/descriptor plumbing into the
// crd-shader infra) need vertex-buffer + descriptor binding and land with the renderer at B8-l; here a fixed key light + the
// fullscreen test VS drive the forward path so each cooked variant is renderable on both backends.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_lighting.hpp>
#include <crd/kir/ckir_lower.hpp>
#include <crd/kir/ckir_material.hpp>

namespace crd::kir::cook
{

// The render pass a material variant is cooked for. The FS differs per pass; the SURFACE is authored once.
enum class PassType : crd::u8
{
    Shadow       = 0, // depth-only render from the light (feeds the B8-f..h shadow maps)
    DepthPrepass = 1, // depth-only render from the camera (early-Z + velocity)
    GBuffer      = 2, // deferred: pack the surface into the B5 MRT G-buffer
    Forward      = 3, // forward: shade the surface into one lit colour
};

// The per-fragment inputs a surface builder consumes — the interpolated varyings a real VS would supply (here procedural,
// FragCoord-derived, in the test). Node ids in the FS graph; -1 = absent.
struct SurfaceInputs
{
    int uv           = -1; // vec2 texture coordinate
    int world_normal = -1; // vec3 interpolated normal
    int view_dir     = -1; // vec3 normalized view direction
    int world_pos    = -1; // vec3 world position
};

// The static ShaderOptions pinned at cook time — the variant key. (More options — vertex layout, shading model, lightmap —
// join here as the cook grows; the permutation matrix + dedup is D3.)
struct VariantOptions
{
    material::AlphaMode alpha_mode   = material::AlphaMode::Opaque;
    double              alpha_cutoff = 0.5;
};

// A cookable material: the authored surface graph as a builder callback (returns the B5 surface struct id from the
// per-fragment inputs). The callback IS the "material graph" until D1 serializes it; `user` carries the material's params.
struct MaterialTemplate
{
    int (*build_surface)(KGraph& g, int struct_id, const SurfaceInputs& in, void* user) = nullptr;
    void* user                                                                          = nullptr;
};

// shade_forward — the surface → lit-colour integrator. Extract (base, metallic, roughness, normal) from the B5 surface and
// evaluate the B8-a/-c Cook-Torrance BRDF for a directional key light, add emissive. (The scene light ARRAY binds at set-1 in
// B8-l; here a fixed key light keeps the cooked forward variant renderable + oracle-checkable.)
[[nodiscard]] inline int shade_forward(KGraph& g, int surface, const SurfaceInputs& in, int light_dir, int light_color)
{
    const int base = g.field_get(surface, material::SfBaseColor);
    const int met  = g.field_get(surface, material::SfMetallic);
    const int rgh  = g.field_get(surface, material::SfRoughness);
    const int nrm  = g.normalize(g.field_get(surface, material::SfNormal));
    const int emis = g.field_get(surface, material::SfEmissive);
    const int lit  = lighting::directional_light(g, base, met, rgh, nrm, in.view_dir, light_dir, light_color);
    return nodes::clamp01(g, nodes::detail::bin(g, KOp::Add, lit, emis));
}

// build_fs_for_pass — cook the FRAGMENT program for `pass` from the authored material. Builds the surface once, routes it to
// the pass's outputs, applies the alpha-test for a Masked material, and (unless `do_lower` is false) runs B7 `lower_entry`.
inline void build_fs_for_pass(const MaterialTemplate& t, PassType pass, const VariantOptions& opts, const SurfaceInputs& in,
                              KGraph& g, KEntry& e, int light_dir, int light_color, bool do_lower = true)
{
    const int struct_id = material::define_surface(g);
    const int surface   = t.build_surface(g, struct_id, in, t.user);
    e.stage              = KStage::Fragment;
    switch (pass)
    {
    case PassType::Shadow:
    case PassType::DepthPrepass:
        e.n_out = 0; // depth-only — the render target has no colour attachment
        break;
    case PassType::GBuffer:
        material::pack_gbuffer(g, e, surface); // sets stage + n_out = 4 + the MRT outputs
        break;
    case PassType::Forward:
    {
        const int lit = shade_forward(g, surface, in, light_dir, light_color);
        e.n_out       = 1;
        e.out[0]      = {g.vec4(g.swizzle(lit, 0), g.swizzle(lit, 1), g.swizzle(lit, 2), g.field_get(surface, material::SfOpacity)), 0};
        break;
    }
    }
    if (opts.alpha_mode == material::AlphaMode::Masked) { material::set_masked(g, e, surface, opts.alpha_cutoff); }
    if (do_lower) { lower::lower_entry(g, e); }
}

// specialize_variant — bake a `ShaderOption` selector node to a compile-time `value` (B7 `specialize`): the static branch it
// drives collapses to the chosen side + DCE reclaims the dead one, yielding a smaller variant bit-identical to the runtime
// uber-shader with that option. Gathers the entry's live roots (as `lower_entry` does) so the ids stay valid across the
// destructive rewrite. The variant KEY is (option, value); the full permutation matrix + dedup is D3.
inline void specialize_variant(KGraph& g, KEntry& e, int option, crd::f64 value)
{
    int* slots[kMaxStageOutputs + 6];
    int  n = 0;
    if (e.position >= 0) { slots[n++] = &e.position; }
    if (e.frag_depth >= 0) { slots[n++] = &e.frag_depth; }
    if (e.discard_cond >= 0) { slots[n++] = &e.discard_cond; }
    if (e.shading_rate >= 0) { slots[n++] = &e.shading_rate; }
    if (e.storage_write_index >= 0) { slots[n++] = &e.storage_write_index; }
    if (e.storage_write_value >= 0) { slots[n++] = &e.storage_write_value; }
    for (int k = 0; k < e.n_out; ++k) { slots[n++] = &e.out[k].node; }
    int roots[kMaxStageOutputs + 6];
    for (int i = 0; i < n; ++i) { roots[i] = *slots[i]; }
    lower::specialize(g, option, value, roots, n);
    for (int i = 0; i < n; ++i) { *slots[i] = roots[i]; }
}

} // namespace crd::kir::cook
