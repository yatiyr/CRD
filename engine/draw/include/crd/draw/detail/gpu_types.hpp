#pragma once

// crd-draw — internal GPU state shared by renderer.cpp (init/shutdown) + overlay_pass.cpp (per-submit packing/draws).
// RET-6 (ADR-0105): re-founded on gpu-context — programs are CKIR-compiled shader objects, the instance stream is ONE
// u32 storage buffer (layout constants live in draw_assets.hpp: 32-word header + 9-word lines + 11-word triangles).
//
// `detail/` marks this engine-private; external consumers should not include it.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/gpu/program.hpp>
#include <crd/gpu/raster_context.hpp>

#include <memory>

namespace crd::gpu
{
class VulkanGpuContext;
} // namespace crd::gpu

namespace crd::draw::detail
{

// Full RendererState definition — construction is gated inside renderer.cpp's init(); overlay_pass.cpp reads it
// through the singleton accessor.
struct RendererState
{
    crd::gpu::VulkanGpuContext* ctx    = nullptr;
    crd::gpu::IRasterContext*   raster = nullptr;
    InitConfig                  config{};

    std::unique_ptr<crd::gpu::IGpuProgram> line_vs;
    std::unique_ptr<crd::gpu::IGpuProgram> line_fs;
    std::unique_ptr<crd::gpu::IGpuProgram> tri_vs;
    std::unique_ptr<crd::gpu::IGpuProgram> tri_fs;
    std::unique_ptr<crd::gpu::IGpuProgram> grid_vs;
    std::unique_ptr<crd::gpu::IGpuProgram> grid_fs;

    std::unique_ptr<crd::gpu::IRasterProgram> line_prog;
    std::unique_ptr<crd::gpu::IRasterProgram> tri_prog;
    std::unique_ptr<crd::gpu::IRasterProgram> grid_prog;

    std::unique_ptr<crd::gpu::IStorageBuffer> storage; // 32-word header + the largest bin's instances

    crd::containers::Array<crd::u32> scratch{crd::memory::default_allocator()}; // per-submit packing scratch

    bool initialised = false;
};

RendererState& renderer_state() noexcept;

} // namespace crd::draw::detail
