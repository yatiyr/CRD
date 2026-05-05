// crd_renderer — surface data contract (ADR-0048, Phase 2.7 v1c).
//
// Include this file in every material fragment shader:
//
//     #include <crd/renderer/surface_data.glsl.inc>
//
//     void crd_evaluate_surface(in VertexAttrs attrs, inout SurfaceData sd)
//     {
//         sd.base_color = vec3(1.0, 0.0, 0.0);
//         sd.roughness  = 0.5;
//     }
//
// The ForwardRenderPath fragment shader declares VertexAttrs as inputs,
// default-initialises SurfaceData, then calls crd_evaluate_surface to let
// the material fill it in. The render path shades using the result.
//
// Stability contract (ADR-0048 §3):
//   - Existing fields are NEVER removed or reordered.
//   - New fields are appended at the END of each struct, default = 0.
//   - The function signature is frozen.

#ifndef CRD_SURFACE_DATA_GLSL
#define CRD_SURFACE_DATA_GLSL

// ─── VertexAttrs ────────────────────────────────────────────────────────────
// Interpolated vertex data forwarded from the vertex stage to the fragment
// stage. Values are in world space unless noted.
struct VertexAttrs
{
    vec3 position_ws;   // world-space surface position
    vec3 normal_ws;     // normalised world-space geometric normal
    vec2 uv0;           // primary UV set (wraps per-material)
    vec4 tangent_ws;    // normalised world-space tangent; .w = bitangent sign (+1 or -1)
};

// ─── SurfaceData ────────────────────────────────────────────────────────────
// PBR surface parameters produced by crd_evaluate_surface() and consumed by
// the render path's shading model.
struct SurfaceData
{
    vec3  base_color;   // linear sRGB albedo (no gamma); default = vec3(1.0)
    vec3  normal_ws;    // shading normal (world space, normalised); may differ from VertexAttrs.normal_ws after normal mapping; default = VertexAttrs.normal_ws
    float metallic;     // 0 = dielectric, 1 = conductor; default = 0.0
    float roughness;    // 0 = mirror, 1 = fully diffuse; default = 1.0
    vec3  emissive;     // additive emissive radiance (linear sRGB); default = vec3(0.0)
    float occlusion;    // ambient occlusion: 0 = fully occluded, 1 = unoccluded; default = 1.0
};

// ─── Contract function ──────────────────────────────────────────────────────
// Each material fragment shader MUST provide one definition of this function.
// The render path calls it exactly once per fragment.
//
// On entry:  attrs   = interpolated geometry from the vertex stage.
//            sd      = zero-initialised (fields with non-zero semantic defaults
//                      must be set; this is intentional — don't leave them at 0).
// On return: sd      = material's shading parameters.
void crd_evaluate_surface(in VertexAttrs attrs, inout SurfaceData sd);

#endif // CRD_SURFACE_DATA_GLSL
