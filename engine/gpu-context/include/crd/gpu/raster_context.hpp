#pragma once

// crd-gpu-context — the backend-agnostic RASTER dispatch surface (ADR-0103 / D-008 C1). The graphics analog of
// `IComputeContext`: it draws IR-authored programs into offscreen targets through DYNAMIC RENDERING (no VkRenderPass),
// on the SHADER-OBJECT model — programs + dynamic state, never a monolithic PSO (`VK_EXT_shader_object`), so the
// variant/permutation explosion ADR-0101 warns about never arises. Backends implement it; consumers depend on THIS,
// never on Vulkan. Created by a per-backend factory (`create_vulkan_raster_context`) from a graphics-capable context.
//
// C1-a (this slice) lands the interface + offscreen render targets + a dynamic-rendering CLEAR with pixel readback (the
// graphics-queue/dynamic-rendering/readback plumbing, green). C1-b appends the shader-object DRAW (bind VS+FS programs +
// dynamic state + `vkCmdDraw`); the interface is shaped for it (append-only, vtable-stable).

#include <crd/core/types.hpp>

#include <memory>

namespace crd::gpu
{

class IGpuProgram; // fwd — a raster program is assembled from VS + FS programs (the ADR-0103 currency)

// A clear / attachment colour, linear 0..1.
struct ClearColor
{
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

// An opaque offscreen colour render target (RGBA8). `read_pixel` is valid after an `IRasterContext` op that targeted it.
class IRasterTarget
{
public:
    IRasterTarget()                                = default;
    virtual ~IRasterTarget()                       = default;
    IRasterTarget(const IRasterTarget&)            = delete;
    IRasterTarget& operator=(const IRasterTarget&) = delete;
    IRasterTarget(IRasterTarget&&)                 = delete;
    IRasterTarget& operator=(IRasterTarget&&)      = delete;

    [[nodiscard]] virtual crd::u32 width() const noexcept  = 0;
    [[nodiscard]] virtual crd::u32 height() const noexcept = 0;
    // One texel of the last rendered result, packed little-endian RGBA8 (0xAABBGGRR — R in the low byte).
    [[nodiscard]] virtual crd::u32 read_pixel(crd::u32 x, crd::u32 y) const noexcept = 0;
};

// An opaque assembled raster program: the SHADER-OBJECT form (a linked VS + FS as `VkShaderEXT` + a pipeline layout),
// built once and drawn many times. This is what makes the seam free of monolithic PSOs (ADR-0101's permutation fear).
class IRasterProgram
{
public:
    IRasterProgram()                                 = default;
    virtual ~IRasterProgram()                        = default;
    IRasterProgram(const IRasterProgram&)            = delete;
    IRasterProgram& operator=(const IRasterProgram&) = delete;
    IRasterProgram(IRasterProgram&&)                 = delete;
    IRasterProgram& operator=(IRasterProgram&&)      = delete;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
};

// The raster dispatch surface. Kernel-source-agnostic like `IComputeContext` (ADR-0100): it renders compiled programs.
class IRasterContext
{
public:
    IRasterContext()                                 = default;
    virtual ~IRasterContext()                        = default;
    IRasterContext(const IRasterContext&)            = delete;
    IRasterContext& operator=(const IRasterContext&) = delete;
    IRasterContext(IRasterContext&&)                 = delete;
    IRasterContext& operator=(IRasterContext&&)      = delete;

    [[nodiscard]] virtual bool valid() const noexcept = 0; // false ⇒ context not graphics-capable (skip)

    [[nodiscard]] virtual std::unique_ptr<IRasterTarget> create_color_target(crd::u32 width, crd::u32 height) = 0;

    // Clear `target` to `color` via dynamic rendering and make the result host-readable (`read_pixel`). Synchronous.
    virtual void clear(IRasterTarget& target, ClearColor color) = 0;

    // --- C1-b: the shader-object DRAW path (appended — vtable-stable) ---------------------------------------------------

    // Assemble a raster program from a VERTEX and a FRAGMENT `IGpuProgram` (both created via `create_program`). Returns
    // nullptr if the backend can't (e.g. `VK_EXT_shader_object` absent, or a stage mismatch).
    [[nodiscard]] virtual std::unique_ptr<IRasterProgram>
    create_raster_program(IGpuProgram& vertex, IGpuProgram& fragment) = 0;

    // Clear `target` to `clear`, then draw `vertex_count` vertices with `program` (attributeless — the VS positions from
    // its vertex index), via dynamic rendering + shader objects. Result host-readable (`read_pixel`). Synchronous.
    virtual void draw(IRasterTarget& target, IRasterProgram& program, ClearColor clear, crd::u32 vertex_count) = 0;
};

} // namespace crd::gpu
