#pragma once

// csm.hpp — REN-3.2-b: the CPU half of CASCADED SHADOW MAPS.
//
// The shader half already exists in CKIR (`csm_split_practical` / `csm_select_cascade` / `csm_texel_snap` /
// `csm_blend_factor`, B8). What was missing is the part that runs on the CPU once per frame: turning a camera
// frustum + a light direction into N stable `light_vp` matrices and their split distances.
//
// ── WHY STABILIZATION IS THE WHOLE PROBLEM ──────────────────────────────────────────────────────────────────
// A naive CSM fits each cascade's ortho box tightly to that slice of the view frustum. That is optimal for
// resolution and *visibly broken* in motion: as the camera rotates or moves, the fitted box changes size and
// orientation every frame, so every shadow texel lands on a different world position and the shadow edges
// CRAWL. Two fixes, both required, both here:
//
//   1. **Bounding SPHERE, not box.** A sphere around the cascade's frustum corners is invariant to camera
//      ROTATION — spin the camera in place and the sphere is identical, so the ortho extent never changes.
//      A box would breathe with every degree of yaw.
//   2. **Texel SNAPPING (Valient).** Quantize the ortho centre to whole shadow-texel increments in LIGHT space.
//      This is what makes translation safe: the projection can only ever move in exact texel steps, so a world
//      point keeps mapping to the same texel until it crosses a full one.
//
// Together they give the property REN-3.2's gate asserts: **a panning camera produces no shadow swim.** That
// matters doubly here because REN-3.6's TAA will AMPLIFY residual shimmer, not hide it — TAA on top of an
// unstabilized cascade looks worse than no TAA at all.
//
// Split placement is the Zhang PSSM 2006 practical scheme (a blend of logarithmic and uniform), matching
// `crd::kir::lighting::csm_split_practical` exactly so the CPU's split planes and the shader's cascade
// SELECTION agree — a mismatch there shows up as a hard seam at the cascade boundary.

#include <crd/core/types.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>

namespace crd::scenerender
{

// The most cascades a directional light may declare. Matches `crd::gpu::kFgMaxImageLayers`'s intent: a stated
// cap rather than a silent truncation, and 4 is the production norm.
inline constexpr crd::u32 kMaxCascades = 4;

struct CsmConfig
{
    crd::u32 cascade_count = 4;      // 1..kMaxCascades
    crd::u32 map_size      = 2048;   // texels per side of ONE cascade slice (the atlas is map_size^2 x count)
    float    lambda        = 0.85F;  // practical-split blend: 1 = pure logarithmic, 0 = pure uniform
    float    near_plane    = 0.1F;   // camera near used for splitting
    float    far_plane     = 200.0F; // the shadow DISTANCE - beyond this, no cascades (not the camera far)
    // How far to pull the light back beyond the cascade sphere. Casters BEHIND the visible slice still need to
    // be rendered, or a tall object just outside the cascade stops casting into it.
    float    caster_extrusion = 100.0F;
};

struct CsmCascades
{
    crd::math::Mat4f light_vp[kMaxCascades]{}; // world -> light clip, per cascade
    float            split_far[kMaxCascades]{}; // VIEW-space far distance of each cascade
    float            texel_world[kMaxCascades]{}; // world units per shadow texel (for bias scaling)
    crd::u32         count = 0;
};

// The practical (Zhang PSSM 2006) split: `lambda` blends the logarithmic and uniform schemes. Returns the
// VIEW-space far distance of cascade `i` of `count`. Kept as a free function so the gate can check it against
// the CKIR shader helper's formula directly.
[[nodiscard]] float csm_split_practical_cpu(float near_p, float far_p, float lambda, crd::u32 i, crd::u32 count);

// Build the stabilized cascades for a directional light.
// `view` and `proj` are the CAMERA's matrices; `light_dir` points FROM the scene TOWARD the light (it is
// normalized internally; a degenerate direction falls back to straight down rather than producing NaNs).
[[nodiscard]] CsmCascades compute_csm_cascades(const crd::math::Mat4f& view, const crd::math::Mat4f& proj,
                                               const crd::math::Vec3f& light_dir, const CsmConfig& cfg);

// The same fit from a COMBINED view_proj — what a renderer that was handed one matrix actually has. The camera's
// frustum half-extents and its inverse view are both recoverable from it, so this is exact, not an approximation:
// the projection's x/y scales survive the product's first two columns, and inverse(view_proj) composed with the
// projection gives back inverse(view). Provided so a caller never has to fabricate a view/proj split it does not
// have — a fabricated one silently mis-sizes every cascade.
[[nodiscard]] CsmCascades compute_csm_cascades_from_vp(const crd::math::Mat4f& view_proj,
                                                       const crd::math::Vec3f& light_dir, const CsmConfig& cfg);

// REN-37.3: the CAMERA'S WORLD POSITION, from the same exact reconstruction the cascade fit uses. The forward
// BRDF needs a real view vector — until this landed `view_dir` was the placeholder constant (0,1,0), which
// degenerates NoV and with it the Smith visibility term, `env_brdf_approx` and the energy compensation.
// ⛔ Derived from `view_proj` rather than taken as a parameter, so no caller can pass a camera position that
// disagrees with the matrix it also passes. Two sources for one truth is how the shadow camera ended up aimed at
// the sky.
[[nodiscard]] crd::math::Vec3f camera_position_from_vp(const crd::math::Mat4f& view_proj);

} // namespace crd::scenerender
