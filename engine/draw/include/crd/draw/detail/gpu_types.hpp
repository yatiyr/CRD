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
class IGpuContext;
} // namespace crd::gpu

namespace crd::draw::detail
{

// Full RendererState definition — construction is gated inside renderer.cpp's init(); overlay_pass.cpp reads it
// through the singleton accessor.
struct RendererState
{
    crd::gpu::IGpuContext*      ctx    = nullptr;
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

    // ⛔ REN-39 (the overlay-corruption fix): TWO buffers, each uploaded ONCE per submission. The old single
    // buffer re-uploaded the same instance region per depth-variant bucket between draws — but uploads complete
    // BEFORE the frame's command buffer executes (the 38-G1 batch contract), so every bucket's draw read the
    // LAST bucket's bytes (dashed lines, vanishing solids). Now: `storage` = header + ALL triangle buckets,
    // `line_storage` = header + ALL line buckets, packed contiguously; each bucket draws its RANGE via
    // `draw_overlay_range`'s first-vertex offset. The grid reads only the header (either buffer serves).
    std::unique_ptr<crd::gpu::IStorageBuffer> storage;      // 32-word header + tri buckets, packed
    std::unique_ptr<crd::gpu::IStorageBuffer> line_storage; // 32-word header + line buckets, packed

    crd::containers::Array<crd::u32> scratch{crd::memory::default_allocator()}; // per-submit packing scratch

    bool initialised = false;
};

RendererState& renderer_state() noexcept;

} // namespace crd::draw::detail
