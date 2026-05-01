#pragma once

#include <crd/core/types.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>

namespace crd::renderer
{

// GPU-layout per-frame uniform buffer object (set 0, binding 0).
// Matches the std140/std430 layout expected by shaders.
// All matrices are column-major (Vulkan/GLSL convention).
struct PerFrameUbo
{
    crd::math::Mat4f view;           // world-to-view
    crd::math::Mat4f proj;           // view-to-clip (Vulkan NDC: y-down, depth [0,1])
    crd::math::Mat4f view_proj;      // proj * view, precomputed
    crd::math::Mat4f inv_view_proj;  // inverse of view_proj (used for depth reconstruction)
    crd::math::Vec4f camera_pos_ws;  // world-space camera origin (w unused)
    crd::f32 viewport_width  = 0.0f;
    crd::f32 viewport_height = 0.0f;
    crd::f32 time_seconds    = 0.0f;
    crd::f32 _pad            = 0.0f;
};
static_assert(sizeof(PerFrameUbo) == 288,
              "PerFrameUbo must be exactly 288 bytes (4 Mat4f + 1 Vec4f + 4 f32)");

// Per-draw push constant block (Vertex stage, offset 0).
// 64 bytes — safely within Vulkan's guaranteed 128-byte push constant minimum.
struct PerDrawPush
{
    crd::math::Mat4f model; // local-to-world transform
};
static_assert(sizeof(PerDrawPush) == 64,  "PerDrawPush must be 64 bytes");
static_assert(sizeof(PerDrawPush) <= 128, "PerDrawPush exceeds Vulkan minimum push constant budget");

} // namespace crd::renderer
