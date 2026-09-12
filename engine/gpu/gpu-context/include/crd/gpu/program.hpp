#pragma once

// crd-gpu-context — the SHADER PROGRAM seam (ADR-0103). This is the one place a GPU program is named, independent of any
// shading language or bytecode. The currency IN is the IR (`crd::kir::KGraph` + `KEntry`, at the graph-taking overload
// added with B3-c); the currency OUT is an opaque `IGpuProgram`. No module outside a backend ever holds shader SOURCE
// (GLSL/HLSL/WGSL/MSL/CUDA) or BYTECODE (SPIR-V/DXIL/PTX) — the invariants I1/I2 the `check_no_shader_language_leak`
// gate enforces. Each backend (`gpu-context-vulkan`, `-dx12`, …) owns its language + vendor compiler PRIVATELY.

#include <crd/core/types.hpp>

namespace crd::gpu
{

// The pipeline stage a program targets — the 14 SPIR-V execution models, COMPLETE from day one even though most have no
// emitter yet (a backend REFUSES LOUDLY for a stage it cannot lower; it never silently falls back to compute). This
// mirrors `crd::kir::KStage` one-for-one, in the same order; the graph-taking `create_program` overload (B3-c) is the
// single point that translates one vocabulary to the other, so `gpu-context` need not depend on `crd-kir` for the
// cooked-bytecode path. Ordered by pipeline position; append-only once a cooked resource serializes it.
enum class ShaderStage : crd::u8
{
    Compute = 0,
    Vertex,
    TessControl,
    TessEval,
    Geometry, // legacy: supported for ports, never the amplification path (mesh is)
    Fragment,
    Task,
    Mesh,
    RayGen,
    Intersection,
    AnyHit,
    ClosestHit,
    Miss,
    Callable,
};
constexpr int kShaderStageCount = 14;

// An opaque, compiled GPU program: the OUT currency of the shader seam. A consumer holds this handle and nothing else —
// never the emitted language, never the cooked bytecode. Backends return a concrete subtype (e.g. a VkShaderModule
// wrapper); the graph→program and cooked-blob→program factories that mint one land on `IGpuContext` (create_program).
class IGpuProgram
{
public:
    IGpuProgram()                              = default;
    virtual ~IGpuProgram()                     = default;
    IGpuProgram(const IGpuProgram&)            = delete;
    IGpuProgram& operator=(const IGpuProgram&) = delete;
    IGpuProgram(IGpuProgram&&)                 = delete;
    IGpuProgram& operator=(IGpuProgram&&)      = delete;

    [[nodiscard]] virtual bool        valid() const noexcept = 0; // false ⇒ compile/link failed (inspect diagnostics)
    [[nodiscard]] virtual ShaderStage stage() const noexcept = 0;
};

} // namespace crd::gpu
