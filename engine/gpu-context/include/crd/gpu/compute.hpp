#pragma once

// crd-gpu-context — the backend-agnostic GPU COMPUTE DISPATCH interface (ADR-0100). `IComputeContext` is the one
// kernel-source-agnostic dispatch surface every GPU-compute technique (LBVH, radix, ray tracing, …) and CKIR itself sit
// on: buffers, cached pipelines requested BY NAME (the backend resolves the name to its own cooked kernel — Vulkan
// `<name>.comp.spv`, CUDA `<name>.ptx`, …), and a multi-pass copy/barrier/dispatch recorder. NO SPIR-V, NO Vulkan, NO
// file formats leak here — consumer code depends on this and never on a backend. Backends implement it
// (`VulkanComputeContext`, future CUDA/Metal/…). See ADR-0100 (one GPU compute manager) + ADR-0099 (the shared context).

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <memory>

namespace crd::gpu
{

// Buffer memory class.
enum class ComputeMemory : crd::u8
{
    GpuOnly,  // device-local VRAM (fast; not host-visible)
    CpuToGpu, // host-visible upload staging (mappable, host-coherent)
    GpuToCpu, // host-visible readback (mappable, host-cached for fast CPU reads)
};

// Buffer usage bits (combine with |).
// The names are deliberately lower_case: they are FLAG BITS a call site ORs together
// (`storage | transfer_dst`), and reading as prose at the use site is the point — the same choice
// `crd::kir::stage_mask` documents. Renaming them to the k-prefixed constant style would touch every
// dispatch call in the engine and tests to buy nothing, so the naming rule is suppressed HERE, once,
// with this justification rather than per line.
// NOLINTBEGIN(readability-identifier-naming)
namespace compute_usage
{
constexpr crd::u32 storage      = 1U;
constexpr crd::u32 transfer_src = 2U;
constexpr crd::u32 transfer_dst = 4U;
constexpr crd::u32 indirect     = 8U; // B4: an INDIRECT-dispatch args buffer (a compute pass writes the mesh-workgroup count a
                                      // later vkCmdDrawMeshTasksIndirectEXT / ExecuteIndirect consumes — GPU-driven, no CPU round-trip)
} // namespace compute_usage
// NOLINTEND(readability-identifier-naming)

// Access state for a buffer barrier (pass-to-pass hazard).
enum class ComputeAccess : crd::u8
{
    TransferSrc,
    TransferDst,
    ShaderRead,
    ShaderWrite,
    HostRead,     // for CPU readback of a shader-written host-visible buffer (barrier before map())
    IndirectRead, // C5: a compute-written args buffer read by vkCmdDispatchIndirect (ShaderWrite → IndirectRead). Appended at END.
};

// Opaque GPU buffer. `map`/`unmap` valid only for CpuToGpu/GpuToCpu.
class ComputeBuffer
{
public:
    ComputeBuffer()                                = default;
    virtual ~ComputeBuffer()                       = default;
    ComputeBuffer(const ComputeBuffer&)            = delete;
    ComputeBuffer& operator=(const ComputeBuffer&) = delete;
    ComputeBuffer(ComputeBuffer&&)                 = delete;
    ComputeBuffer& operator=(ComputeBuffer&&)      = delete;

    [[nodiscard]] virtual void* map() noexcept   = 0;
    virtual void                unmap() noexcept = 0;

    // B4: the backend-native buffer handle (Vulkan `VkBuffer`, DX12 `ID3D12Resource*`) as an opaque `void*`. The escape hatch
    // that lets an INDIRECT-args buffer written by a compute pass be consumed by the raster context's indirect mesh dispatch
    // (`IRasterContext::draw_mesh_indirect`) — a GPU-driven loop across the compute/graphics seam. Default nullptr.
    [[nodiscard]] virtual void* native_handle() const noexcept { return nullptr; }
};

// Opaque cached compute pipeline (a compiled kernel + N storage-buffer bindings + a push constant).
class ComputePipeline
{
public:
    ComputePipeline()                                  = default;
    virtual ~ComputePipeline()                         = default;
    ComputePipeline(const ComputePipeline&)            = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(ComputePipeline&&)                 = delete;
    ComputePipeline& operator=(ComputePipeline&&)      = delete;
};

// Records a multi-pass compute job into one command buffer. Get it from `IComputeContext::begin()`, record
// copies/barriers/dispatches (interleaved, in order), then `submit_and_wait()`. Descriptor sets are managed internally
// per dispatch and live until the next `begin()`.
class ComputeRecorder
{
public:
    ComputeRecorder()                                  = default;
    virtual ~ComputeRecorder()                         = default;
    ComputeRecorder(const ComputeRecorder&)            = delete;
    ComputeRecorder& operator=(const ComputeRecorder&) = delete;
    ComputeRecorder(ComputeRecorder&&)                 = delete;
    ComputeRecorder& operator=(ComputeRecorder&&)      = delete;

    virtual void copy(ComputeBuffer& src, ComputeBuffer& dst, crd::u64 src_off, crd::u64 dst_off, crd::u64 bytes) = 0;
    virtual void barrier(ComputeBuffer& buf, ComputeAccess from, ComputeAccess to)                                = 0;
    // Bind `pipeline` with `bindings[i]` at binding i, push `push_size` bytes, dispatch gx×gy×gz. `bindings.size()`
    // must equal the pipeline's binding count.
    virtual void dispatch(ComputePipeline& pipeline, crd::containers::ConstSpan<ComputeBuffer*> bindings,
                          const void* push, crd::u32 push_size, crd::u32 gx, crd::u32 gy, crd::u32 gz)             = 0;
    // C5: GPU-DRIVEN DISPATCH — the workgroup count comes from `args` (a compute-written buffer, usage `indirect`: three u32
    // {gx,gy,gz} at `args_offset`) instead of the CPU, so a preceding compute pass DECIDES the next pass's size with no CPU
    // round-trip (`vkCmdDispatchIndirect`). Appended at END (vtable stability); default no-op ⇒ backends opt in.
    virtual void dispatch_indirect(ComputePipeline& /*pipeline*/, crd::containers::ConstSpan<ComputeBuffer*> /*bindings*/,
                                   const void* /*push*/, crd::u32 /*push_size*/, ComputeBuffer& /*args*/, crd::u64 /*args_offset*/) {}
};

// The one GPU compute dispatch surface (ADR-0100). Kernel-source-agnostic: pipelines are requested BY NAME and the
// backend loads its own cooked kernel. Consumers depend on THIS, never on a concrete backend.
class IComputeContext
{
public:
    IComputeContext()                                  = default;
    virtual ~IComputeContext()                         = default;
    IComputeContext(const IComputeContext&)            = delete;
    IComputeContext& operator=(const IComputeContext&) = delete;
    IComputeContext(IComputeContext&&)                 = delete;
    IComputeContext& operator=(IComputeContext&&)      = delete;

    [[nodiscard]] virtual bool valid() const noexcept                = 0;
    [[nodiscard]] virtual bool supports_shader_int64() const noexcept = 0;

    [[nodiscard]] virtual std::unique_ptr<ComputeBuffer> create_buffer(crd::u64 bytes, crd::u32 usage, ComputeMemory memory) = 0;

    // Cache-and-return the pipeline for the cooked kernel named `name` in `shader_dir` (backend resolves the extension:
    // Vulkan `<name>.comp.spv`), with `n_bindings` storage-buffer bindings + a `push_size`-byte push constant.
    [[nodiscard]] virtual std::unique_ptr<ComputePipeline>
    create_pipeline(crd::containers::StringView shader_dir, crd::containers::StringView name, int n_bindings,
                    crd::u32 push_size) = 0;

    [[nodiscard]] virtual ComputeRecorder& begin() = 0;
    virtual void                           submit_and_wait() = 0;

    // ── REN-38 llvmpipe campaign (appended at END — vtable-stable). The DEVICE truths every warp-synchronous
    // kernel must be built against. Defaults are the VULKAN MINIMUM-SPEC floor, not a guess: a backend that has
    // not overridden them under-promises (a kernel shaped for 16 KB / any-width subgroups runs everywhere), it
    // never over-promises. Real backends override with the queried values (NV 32/48KB+, llvmpipe 8/32KB).
    [[nodiscard]] virtual crd::u32 subgroup_size() const noexcept { return 0U; }         // 0 = unknown width
    [[nodiscard]] virtual crd::u32 shared_memory_bytes() const noexcept { return 16384U; } // Vulkan min-spec
};

} // namespace crd::gpu
