#pragma once

// crd-draw -- Renderer (Phase 3.1 v1a-draw d0b, ADR-0066 sec 10).
//
// The GPU-side counterpart to RenderBuffer. Owns:
//   - Loaded SHDR resources (line_aa vert + frag)
//   - Vulkan ShaderModules
//   - PipelineLayout (with the 80-byte push-constant block declared by line_aa.vert.glsl)
//   - GraphicsPipeline (TriangleList; per-instance binding for line records;
//     blend-on with standard alpha; depth-test conditional per primitive's
//     DepthMode at record time)
//   - Per-frame instance buffer ring (one buffer per frame-in-flight; mapped
//     persistently for CPU-side writes; passed to bind_vertex_buffers in
//     the overlay pass)
//
// Public API:
//   crd::draw::init(rm, device, color_format, depth_format, frames_in_flight)
//     -- call once after ResourceManager is mounted; loads shaders + creates
//       pipeline. ResourceManager must have a ShaderResourceLoader registered
//       and the draw_shaders.crdr pack mounted (see CRD_DRAW_SHADERS_PACK_PATH
//       compile definition for the build-time path).
//   crd::draw::shutdown()
//     -- releases all GPU resources. Call before device destruction.
//   crd::draw::is_initialised()
//     -- query for guard checks in tests + `add_draw_overlay_pass` (which
//       early-outs to a no-op if the renderer isn't initialised).

#include <crd/core/types.hpp>
#include <crd/rhi/types.hpp>

namespace crd::resources { class ResourceManager; }
namespace crd::rhi       { class Device; }

namespace crd::draw
{
struct InitConfig
{
    // Color attachment format the overlay pass writes to. Must match the
    // color image the consuming IRenderPath outputs (typically
    // rhi::Format::B8G8R8A8Unorm for swapchain-bound paths).
    crd::rhi::Format color_format = crd::rhi::Format::B8G8R8A8Unorm;
    // Depth attachment format. Used only for Depth_Test mode primitives;
    // Always-on-top + XRay paths ignore. Pass Format::Undefined to disable
    // depth integration entirely (overlay always-on-top).
    crd::rhi::Format depth_format = crd::rhi::Format::D32Sfloat;
    // Number of frames the renderer needs to keep instance buffers alive
    // for. Match the swapchain's image_count or the application's
    // frames_in_flight value (typically 2).
    crd::u32         frames_in_flight = 2;
    // Maximum line instances per frame. Sized as a hard cap because the
    // instance buffer is created up-front. Default supports 64K lines/frame
    // = ~2.3 MB GPU memory per frame. Eylem v1c+ workloads need ~100K, so
    // raise this on real consumers.
    crd::u32         max_lines_per_frame     = 65536;
    // Maximum triangle instances per frame. Default supports 32K triangles
    // = ~1.4 MB GPU memory per frame. Solid sphere (icosphere subdivision 1)
    // = 80 triangles; capsule = ~600. Default holds ~50 solid spheres or
    // ~50 solid capsules per frame.
    crd::u32         max_triangles_per_frame = 32768;
};

[[nodiscard]] bool init(crd::resources::ResourceManager& rm,
                        crd::rhi::Device&                device,
                        const InitConfig&                config = {}) noexcept;

void shutdown() noexcept;

[[nodiscard]] bool is_initialised() noexcept;

// d4: master overlay enable -- runtime toggle queried by `add_draw_overlay_pass`
// and `DebugVizSystem::run` for zero-CPU early-outs (a single bool check per
// call site). Default = enabled.
//
// Profile gating contract per ADR-0066 §19.6: the application is responsible
// for flipping this based on its own profile state. Typical wiring:
//
//     // After resolving the active profile (dev / shipping / ...):
//     crd::draw::set_overlay_enabled(profile.allow_debug_overlay);
//
// `crd-draw` deliberately does not pull in `crd-profile` to query directly
// (would spread the dep edge); the application owns the policy.
//
// Thread-safety: not safe with concurrent reads. Set at startup or between
// frames, same contract as `set_theme()`.
[[nodiscard]] bool is_overlay_enabled() noexcept;
void               set_overlay_enabled(bool enabled) noexcept;

} // namespace crd::draw
