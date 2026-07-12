#pragma once

// ckir_material.hpp — the MATERIAL PROFILE layer on top of core CKIR (D-007 B5, ADR-0102 D3). A material is a CKIR
// fragment graph that OUTPUTS an OpenPBR surface-parameter struct and does NOT compute lighting — "material = surface
// response; render path = lighting technique" (ADR-0102). This header owns the canonical **OpenPBR 1.1 SURFACE SLAB** and
// its DEFERRED G-BUFFER packing (surface → N MRT attachments). The shared lighting library (GGX BRDF + light loop + shadows)
// that CONSUMES this layout is B8; the full Forward+/Deferred pipelines are the post-hesap rendering phase. The struct field
// order is APPEND-ONLY (it is the surface contract every material + the future lighting pass agree on — matching the shipped
// renderer `SurfaceData` stability rule: never remove/reorder; new fields default 0/identity).

#include <crd/kir/ckir.hpp>

namespace crd::kir::material
{

// B5-c: the SHADING MODEL — which lighting model the render path applies to this surface (a material-level TAG; the material
// stays lighting-agnostic — it just declares intent, B8 applies it). Fresh taxonomy (none existed): standard metallic-rough
// PBR + Unlit + the NPR/stylized family. APPEND-ONLY.
enum class ShadingModel : crd::u8
{
    Standard = 0, // OpenPBR metallic-roughness (the default lit model)
    Unlit,        // emit base_color directly (no lighting)
    Toon,         // quantised diffuse bands
    Cel,          // hard 2-tone cel
    Gooch,        // warm/cool technical shading
    Outline,      // silhouette/outline pass
    Hatching,     // cross-hatch strokes by luminance
};

// B5-c: the ALPHA MODE — the opaque/masked/translucent axis (the CKIR material-profile view; maps onto the renderer's frozen
// `AlphaMode` + blend state at B7/B8). `Masked` alpha-tests (discard where opacity < cutoff); `Additive` is a new value.
enum class AlphaMode : crd::u8
{
    Opaque = 0,  // opacity ignored, fully covered
    Masked,      // alpha test: discard where opacity < cutoff
    Translucent, // alpha blend (render-state at draw time)
    Additive,    // additive blend (render-state at draw time)
};

// B5: the full OpenPBR 1.1 SURFACE SLAB. Field ORDER is the struct layout — the first 7 (B5-a) are the compact deferred
// core; the rest (B5-b) are the OpenPBR layers (base extras · specular · transmission · subsurface · coat · fuzz/sheen ·
// thin-film · geometry). APPEND new fields at the END, never reorder.
enum SurfaceField : crd::u8
{
    // --- B5-a core (compact G-buffer) ---
    SfBaseColor = 0, // vec3 — linear base albedo
    SfMetallic,      // float
    SfRoughness,     // float — base/specular perceptual roughness
    SfNormal,        // vec3 — world-space shading normal
    SfEmissive,      // vec3 — emitted radiance (× emission_luminance)
    SfOcclusion,     // float — ambient occlusion
    SfOpacity,       // float — coverage (masked/translucent axis)
    // --- B5-b: base extras ---
    SfBaseWeight,       // float — base lobe weight
    SfDiffuseRoughness, // float — Oren-Nayar diffuse roughness
    // --- B5-b: specular lobe ---
    SfSpecularWeight,     // float
    SfSpecularColor,      // vec3
    SfSpecularIor,        // float — index of refraction (~1.5)
    SfSpecularAnisotropy, // float
    SfSpecularRotation,   // float
    // --- B5-b: transmission ---
    SfTransmissionWeight, // float
    SfTransmissionColor,  // vec3
    SfTransmissionDepth,  // float
    // --- B5-b: subsurface ---
    SfSubsurfaceWeight,     // float
    SfSubsurfaceColor,      // vec3
    SfSubsurfaceRadius,     // vec3 — per-channel mean free path
    SfSubsurfaceAnisotropy, // float
    // --- B5-b: coat ---
    SfCoatWeight,      // float
    SfCoatColor,       // vec3
    SfCoatRoughness,   // float
    SfCoatIor,         // float (~1.5)
    SfCoatAnisotropy,  // float
    SfCoatDarkening,   // float
    // --- B5-b: fuzz / sheen ---
    SfFuzzWeight,    // float
    SfFuzzColor,     // vec3
    SfFuzzRoughness, // float
    // --- B5-b: thin-film ---
    SfThinFilmWeight,    // float
    SfThinFilmThickness, // float (nm, normalised)
    SfThinFilmIor,       // float
    // --- B5-b: geometry ---
    SfThinWalled, // float (0/1)
    SfTangent,    // vec3 — world-space tangent (anisotropy frame)
    SfCoatNormal, // vec3 — coat shading normal
    // --- B5-b: emission ---
    SfEmissionLuminance, // float — emissive intensity multiplier
    // --- B5-c: shading-model + alpha-mode TAGS (float-encoded enum values) ---
    SfShadingModel, // float — ShadingModel (which lighting model B8 applies)
    SfAlphaMode,    // float — AlphaMode (opaque/masked/translucent/additive)
    SfCount
};

// B5: register the full OpenPBR surface slab in `g`. Returns the struct id.
[[nodiscard]] inline int define_surface(KGraph& g)
{
    KType f[SfCount];
    for (int i = 0; i < SfCount; ++i) { f[i] = KType::make_scalar(DType::F32); } // default: float
    f[SfBaseColor]        = KType::vec(DType::F32, 3);
    f[SfNormal]           = KType::vec(DType::F32, 3);
    f[SfEmissive]         = KType::vec(DType::F32, 3);
    f[SfSpecularColor]    = KType::vec(DType::F32, 3);
    f[SfTransmissionColor] = KType::vec(DType::F32, 3);
    f[SfSubsurfaceColor]  = KType::vec(DType::F32, 3);
    f[SfSubsurfaceRadius] = KType::vec(DType::F32, 3);
    f[SfCoatColor]        = KType::vec(DType::F32, 3);
    f[SfFuzzColor]        = KType::vec(DType::F32, 3);
    f[SfTangent]          = KType::vec(DType::F32, 3);
    f[SfCoatNormal]       = KType::vec(DType::F32, 3);
    return g.define_struct(f, SfCount);
}

// B5: fill `out[SfCount]` with the OpenPBR 1.1 DEFAULT value node for every field (a material overrides the ones it drives,
// then calls `build_surface`). Defaults follow the OpenPBR spec (unlit-off layers weight 0; ior 1.5; identity frames).
inline void surface_defaults(KGraph& g, int out[SfCount])
{
    const auto sh   = make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, DType::F32); };
    const auto v3   = [&](double x, double y, double z) { return g.vec3(g.constant(x, sh, DType::F32), g.constant(y, sh, DType::F32), g.constant(z, sh, DType::F32)); };
    for (int i = 0; i < SfCount; ++i) { out[i] = k(0.0); } // scalar 0 default
    out[SfBaseColor]         = v3(0.8, 0.8, 0.8);
    out[SfMetallic]          = k(0.0);
    out[SfRoughness]         = k(0.5);
    out[SfNormal]            = v3(0.0, 0.0, 1.0);
    out[SfEmissive]          = v3(0.0, 0.0, 0.0);
    out[SfOcclusion]         = k(1.0);
    out[SfOpacity]           = k(1.0);
    out[SfBaseWeight]        = k(1.0);
    out[SfDiffuseRoughness]  = k(0.0);
    out[SfSpecularWeight]    = k(1.0);
    out[SfSpecularColor]     = v3(1.0, 1.0, 1.0);
    out[SfSpecularIor]       = k(1.5);
    out[SfTransmissionColor] = v3(1.0, 1.0, 1.0);
    out[SfSubsurfaceColor]   = v3(0.8, 0.8, 0.8);
    out[SfSubsurfaceRadius]  = v3(1.0, 1.0, 1.0);
    out[SfCoatColor]         = v3(1.0, 1.0, 1.0);
    out[SfCoatIor]           = k(1.5);
    out[SfCoatDarkening]     = k(1.0);
    out[SfFuzzColor]         = v3(1.0, 1.0, 1.0);
    out[SfFuzzRoughness]     = k(0.5);
    out[SfThinFilmThickness] = k(0.5);
    out[SfThinFilmIor]       = k(1.5);
    out[SfTangent]           = v3(1.0, 0.0, 0.0);
    out[SfCoatNormal]        = v3(0.0, 0.0, 1.0);
    out[SfEmissionLuminance] = k(1.0);
}

// B5: assemble a surface value from a full field array (a `struct_make`, SROA-lowered by the emitters).
[[nodiscard]] inline int build_surface_full(KGraph& g, int struct_id, const int fields[SfCount])
{
    return g.struct_make(struct_id, fields, SfCount);
}

// B5-a: convenience — a core (metallic-roughness) surface: OpenPBR defaults with the 7 compact fields overridden.
[[nodiscard]] inline int build_surface(KGraph& g, int struct_id, int base_color, int metallic, int roughness, int normal,
                                       int emissive, int occlusion, int opacity)
{
    int fields[SfCount];
    surface_defaults(g, fields);
    fields[SfBaseColor] = base_color;
    fields[SfMetallic]  = metallic;
    fields[SfRoughness] = roughness;
    fields[SfNormal]    = normal;
    fields[SfEmissive]  = emissive;
    fields[SfOcclusion] = occlusion;
    fields[SfOpacity]   = opacity;
    return build_surface_full(g, struct_id, fields);
}

// B5-a: the compact deferred G-BUFFER — 4 RGBA8 attachments (base_color+metallic · normal_enc+roughness · emissive+occlusion
// · opacity). The normal is encoded n*0.5+0.5 for the UNORM attachment. Sets `e.n_out = 4` + `e.out[0..3]`.
inline constexpr int kGBufferAttachments    = 4;
inline constexpr int kGBufferAttachmentsExt = 8; // B5-b: the extended G-buffer carries the OpenPBR layers

namespace detail
{
[[nodiscard]] inline int enc_normal(KGraph& g, int n)
{
    const auto sh = make_shape({1});
    return g.binary(KOp::Mul, g.binary(KOp::Add, n, g.splat(g.constant(1.0, sh, DType::F32), 3)),
                    g.splat(g.constant(0.5, sh, DType::F32), 3));
}
} // namespace detail

inline void pack_gbuffer(KGraph& g, KEntry& e, int surface)
{
    const auto sh   = make_shape({1});
    const int  one  = g.constant(1.0, sh, DType::F32);
    const int  inv  = g.constant(1.0 / 255.0, sh, DType::F32); // encode the enum tags into UNORM8 (decode ×255)
    const int  sm   = g.binary(KOp::Mul, g.field_get(surface, SfShadingModel), inv);
    const int  am   = g.binary(KOp::Mul, g.field_get(surface, SfAlphaMode), inv);
    e.stage  = KStage::Fragment;
    e.n_out  = kGBufferAttachments;
    e.out[0] = {g.vec_concat(g.field_get(surface, SfBaseColor), g.field_get(surface, SfMetallic)), 0};
    e.out[1] = {g.vec_concat(detail::enc_normal(g, g.field_get(surface, SfNormal)), g.field_get(surface, SfRoughness)), 1};
    e.out[2] = {g.vec_concat(g.field_get(surface, SfEmissive), g.field_get(surface, SfOcclusion)), 2};
    e.out[3] = {g.vec4(g.field_get(surface, SfOpacity), sm, am, one), 3}; // (opacity, shading_model, alpha_mode, 1)
}

// B5-c: MASKED alpha test — discard the fragment where `surface.opacity < cutoff` (AlphaMode::Masked). Sets
// `e.discard_cond` (the B1-b mechanism) so a masked material culls sub-cutoff pixels. Call AFTER a pack_* helper.
inline void set_masked(KGraph& g, KEntry& e, int surface, double cutoff)
{
    const auto sh = make_shape({1});
    e.discard_cond = g.binary(KOp::CmpLt, g.field_get(surface, SfOpacity), g.constant(cutoff, sh, DType::F32));
}

// B5-b: the EXTENDED G-buffer — 8 RGBA8 attachments carrying EVERY OpenPBR layer for the observable (a forward material's
// full surface; the IORs/anisotropy stay in the struct for B8's BRDF):
//   0 (base_color, metallic) · 1 (normal_enc, roughness) · 2 (emissive, occlusion) · 3 (opacity, specular_w, coat_w, fuzz_w)
//   4 (coat_color, coat_roughness) · 5 (fuzz_color, fuzz_roughness) · 6 (transmission_color, transmission_w)
//   7 (thin_film_w, thin_film_thickness, subsurface_w, thin_walled)
inline void pack_gbuffer_ext(KGraph& g, KEntry& e, int surface)
{
    const auto s = [&](int f) { return g.field_get(surface, f); };
    e.stage  = KStage::Fragment;
    e.n_out  = kGBufferAttachmentsExt;
    e.out[0] = {g.vec_concat(s(SfBaseColor), s(SfMetallic)), 0};
    e.out[1] = {g.vec_concat(detail::enc_normal(g, s(SfNormal)), s(SfRoughness)), 1};
    e.out[2] = {g.vec_concat(s(SfEmissive), s(SfOcclusion)), 2};
    e.out[3] = {g.vec4(s(SfOpacity), s(SfSpecularWeight), s(SfCoatWeight), s(SfFuzzWeight)), 3};
    e.out[4] = {g.vec_concat(s(SfCoatColor), s(SfCoatRoughness)), 4};
    e.out[5] = {g.vec_concat(s(SfFuzzColor), s(SfFuzzRoughness)), 5};
    e.out[6] = {g.vec_concat(s(SfTransmissionColor), s(SfTransmissionWeight)), 6};
    e.out[7] = {g.vec4(s(SfThinFilmWeight), s(SfThinFilmThickness), s(SfSubsurfaceWeight), s(SfThinWalled)), 7};
}

} // namespace crd::kir::material
