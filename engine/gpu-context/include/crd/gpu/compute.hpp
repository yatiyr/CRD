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
namespace compute_usage
{
constexpr crd::u32 storage      = 1U;
constexpr crd::u32 transfer_src = 2U;
constexpr crd::u32 transfer_dst = 4U;
} // namespace compute_usage

// Access state for a buffer barrier (pass-to-pass hazard).
enum class ComputeAccess : crd::u8
{
    TransferSrc,
    TransferDst,
    ShaderRead,
    ShaderWrite,
    HostRead, // for CPU readback of a shader-written host-visible buffer (barrier before map())
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
};

} // namespace crd::gpu
