#pragma once

// crd-draw — Renderer (RET-6, ADR-0105: re-founded on the ONE graphics layer; the rhi original — ADR-0066 §10 —
// retired with it).
//
// The GPU-side counterpart to RenderBuffer, on gpu-context: the AUTHORED draw-shader suite (draw_assets.hpp — line_aa /
// triangle_solid / infinite_grid) compiled through `VulkanGpuContext::create_program` + assembled into three
// `IRasterProgram`s, and ONE u32 storage buffer carrying the 32-word header + packed instances (the vertex-pulling
// contract — no vertex-input state, no pipeline objects, no cooked-GLSL pack, no push constants).
//
// Public API:
//   crd::draw::init(ctx, raster, config) — build the programs + the draw buffer. No ResourceManager, no shader pack.
//   crd::draw::shutdown()                — release GPU resources (before the raster context dies).
//   crd::draw::is_initialised()          — guard for `submit_overlay` (which no-ops when uninitialised).
//   is_overlay_enabled / set_overlay_enabled — the d4 master toggle (profile-gated by the APPLICATION, ADR-0066 §19.6).

#include <crd/core/types.hpp>

namespace crd::gpu
{
// ⛔ REN-39-D1: `IGpuContext`, not `VulkanGpuContext`. crd-draw only ever calls `create_program(KGraph, KEntry)`
// — the ADR-0103 currency, declared on the INTERFACE — so the Vulkan typing was a historical accident that made
// the whole debug-draw suite unreachable from any second backend. Naming the interface is what lets the same
// overlay run on DX12.
class IGpuContext;
class IRasterContext;
} // namespace crd::gpu

namespace crd::draw
{
struct InitConfig
{
    // Maximum instances per BIN per submit (a bin over the cap draws in batches — unbounded counts still render).
    // The draw buffer is sized up-front: 32 header words + max(lines·9, triangles·11) words.
    crd::u32 max_lines_per_frame     = 65536;
    crd::u32 max_triangles_per_frame = 32768;
};

[[nodiscard]] bool init(crd::gpu::IGpuContext& ctx, crd::gpu::IRasterContext& raster,
                        const InitConfig& config = {}) noexcept;

void shutdown() noexcept;

[[nodiscard]] bool is_initialised() noexcept;

// d4: master overlay enable — runtime toggle queried by `submit_overlay` and `DebugVizSystem::run` for zero-CPU
// early-outs. The application owns the profile policy (`set_overlay_enabled(profile.allow_debug_overlay)`);
// crd-draw deliberately does not pull in crd-profile. Not safe with concurrent reads — set at startup or between
// frames, the same contract as `set_theme()`.
[[nodiscard]] bool is_overlay_enabled() noexcept;
void               set_overlay_enabled(bool enabled) noexcept;

} // namespace crd::draw
