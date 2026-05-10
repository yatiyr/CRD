#pragma once

// crd-draw -- internal GPU layout types shared by renderer.cpp + overlay_pass.cpp.
//
// These match the line_aa.{vert,frag}.glsl shader contract bit-for-bit:
//   LineInstanceGpu     = per-instance vertex attributes (locations 0..4)
//   DrawPushConstants   = push_constant block in vertex shader
//
// `detail/` namespace marks them as engine-private; not part of the public
// crd-draw API. External consumers should not include this header.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/shader/shader_resource_loader.hpp>

#include <memory>

namespace crd::rhi
{
class Buffer;
class Device;
class Pipeline;
class PipelineLayout;
class ShaderModule;
}

namespace crd::draw::detail
{
struct LineInstanceGpu
{
    crd::math::Vec3f start;        // loc 0
    crd::math::Vec3f end;          // loc 1
    crd::u32         color_packed; // loc 2
    crd::u32         flags_raw;    // loc 3
    crd::f32         width;        // loc 4
};
static_assert(sizeof(LineInstanceGpu) == 36, "LineInstanceGpu must pack to 36 bytes");

// Per-instance triangle layout. Matches triangle_solid.vert.glsl inputs
// at locations 0..4. 44 bytes / instance. v0+v1+v2 = 36 bytes;
// color_packed + flags_raw = 8 bytes.
struct TriangleInstanceGpu
{
    crd::math::Vec3f v0;           // loc 0
    crd::math::Vec3f v1;           // loc 1
    crd::math::Vec3f v2;           // loc 2
    crd::u32         color_packed; // loc 3
    crd::u32         flags_raw;    // loc 4
};
static_assert(sizeof(TriangleInstanceGpu) == 44, "TriangleInstanceGpu must pack to 44 bytes");

struct DrawPushConstants
{
    crd::math::Mat4f view_proj;       // 64 -- used by all pipelines
    crd::math::Vec2f viewport_px;     //  8 -- used by line/triangle
    crd::u32         category_mask;   //  4 -- used by all
    crd::f32         time_s;          //  4 -- used by all
    // d2-grid-specific (next 64 bytes; ignored by line/triangle shaders).
    // The single shared push-constant layout = 128 bytes total = Vulkan minimum.
    crd::math::Vec3f camera_pos;      // 12 -- world-space camera xyz
    crd::f32         _pad_camera;     //  4 -- padding to vec4 align
    crd::u32         primary_color;   //  4 -- RGBA8 packed
    crd::u32         secondary_color; //  4 -- RGBA8 packed
    crd::f32         plane_y;         //  4 -- world Y of grid plane
    crd::f32         primary_cell;    //  4 -- primary grid cell size (m)
    crd::f32         secondary_cell;  //  4 -- secondary cell size    (m)
    crd::f32         fade_distance;   //  4 -- alpha = 0 at this dist
    crd::u32         axis_x_color;    //  4 -- packed RGBA8; line at z=0
    crd::u32         axis_z_color;    //  4 -- packed RGBA8; line at x=0
};
static_assert(sizeof(DrawPushConstants) == 128, "DrawPushConstants must pack to 128 bytes");

// Full RendererState definition — shared by renderer.cpp (init/shutdown)
// and overlay_pass.cpp (per-frame draw recording). Only the singleton
// accessor is exposed publicly (renderer_state()); construction is gated
// inside renderer.cpp's init() implementation.
struct RendererState
{
    crd::rhi::Device*                           device = nullptr;
    InitConfig                                  config{};

    crd::resources::ResourceHandle<crd::shader::ShaderResource> line_vert_handle;
    crd::resources::ResourceHandle<crd::shader::ShaderResource> line_frag_handle;
    crd::resources::ResourceHandle<crd::shader::ShaderResource> tri_vert_handle;
    crd::resources::ResourceHandle<crd::shader::ShaderResource> tri_frag_handle;
    crd::resources::ResourceHandle<crd::shader::ShaderResource> grid_vert_handle;
    crd::resources::ResourceHandle<crd::shader::ShaderResource> grid_frag_handle;

    std::unique_ptr<crd::rhi::ShaderModule>     line_vert_module;
    std::unique_ptr<crd::rhi::ShaderModule>     line_frag_module;
    std::unique_ptr<crd::rhi::ShaderModule>     tri_vert_module;
    std::unique_ptr<crd::rhi::ShaderModule>     tri_frag_module;
    std::unique_ptr<crd::rhi::ShaderModule>     grid_vert_module;
    std::unique_ptr<crd::rhi::ShaderModule>     grid_frag_module;
    std::unique_ptr<crd::rhi::PipelineLayout>   pipeline_layout;
    // d2-depth: 6-pipeline matrix indexed by [primitive][depth_variant].
    // primitive:     0 = line, 1 = triangle
    // depth_variant: 0 = Test (visible-when-closer; reverse-Z GREATER_OR_EQUAL)
    //                1 = Always (no depth test; current d0-d2 default)
    //                2 = GreaterDimmed (visible-when-occluded; reverse-Z LESS)
    // XRay primitives emit twice -- once as Test (full color, only visible
    // portion shows) and once as GreaterDimmed (alpha-multiplied color,
    // only occluded portion shows). Per ADR-0066 sec 19.1.
    static constexpr crd::u32 kDepthVariantCount = 3;
    std::unique_ptr<crd::rhi::Pipeline>         line_pipelines[kDepthVariantCount];
    std::unique_ptr<crd::rhi::Pipeline>         triangle_pipelines[kDepthVariantCount];
    // d2-grid: single pipeline, alpha-blended, depth-test reads scene depth
    // (so the grid is occluded by closer geometry but visible elsewhere).
    std::unique_ptr<crd::rhi::Pipeline>         grid_pipeline;

    crd::containers::Array<std::unique_ptr<crd::rhi::Buffer>> line_instance_buffers;
    crd::containers::Array<std::unique_ptr<crd::rhi::Buffer>> triangle_instance_buffers;

    bool initialised = false;
};

RendererState& renderer_state() noexcept;

} // namespace crd::draw::detail
