#pragma once

// crd-gpu-context-dx12 — the DIRECTX 12 implementation of the one compute dispatch surface `crd::gpu::IComputeContext`
// (ADR-0100). Raw D3D12 compute (a dedicated compute command queue, no rendering) + dxc (HLSL → DXIL) behind the SAME
// backend-agnostic interface geometry + CKIR consume. This is the SECOND backend — it proves the seam is genuinely
// backend-agnostic, not Vulkan-shaped. WINDOWS-ONLY (D3D12); the module self-skips elsewhere.
//
// One portability note surfaced by standing this up: D3D12 forbids a UAV on an UPLOAD/READBACK heap, so the Vulkan-style
// "shader writes a host-visible GpuToCpu buffer directly, then map()" pattern does NOT port. The PORTABLE readback
// contract is: shader writes a GpuOnly buffer → recorder.copy(GpuOnly → GpuToCpu) → submit → map(GpuToCpu). Consumers
// that want to run on both backends must use that copy form (the Vulkan backend supports it too).

#include <crd/gpu/compute.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>

#include <memory>

namespace crd::memory
{
class IAllocator;
}

namespace crd::gpu
{

class Dx12ComputeContext final : public IComputeContext
{
public:
    explicit Dx12ComputeContext(crd::memory::IAllocator* alloc);
    ~Dx12ComputeContext() override;
    Dx12ComputeContext(const Dx12ComputeContext&)            = delete;
    Dx12ComputeContext& operator=(const Dx12ComputeContext&) = delete;
    Dx12ComputeContext(Dx12ComputeContext&&)                 = delete;
    Dx12ComputeContext& operator=(Dx12ComputeContext&&)      = delete;

    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] bool supports_shader_int64() const noexcept override;
    [[nodiscard]] crd::u32 subgroup_size() const noexcept override;        // REN-38: OPTIONS1 WaveLaneCountMin
    [[nodiscard]] crd::u32 shared_memory_bytes() const noexcept override;  // REN-38: 32 KB TGSM spec limit
    [[nodiscard]] double   last_gpu_ms() const noexcept override;          // CGP-0: portable GPU timing (timestamp query heap)

    [[nodiscard]] std::unique_ptr<ComputeBuffer> create_buffer(crd::u64 bytes, crd::u32 usage, ComputeMemory memory) override;

    // Loads the cooked `<shader_dir>/<name>.cs.hlsl` and caches nothing here (the pipeline object is the cache handle):
    // compiles HLSL → DXIL (cs_6_0, entry `cs_main`), builds a root signature (n storage UAVs at u0.. + `push_size` bytes
    // of root constants at b0), and a compute PSO.
    [[nodiscard]] std::unique_ptr<ComputePipeline> create_pipeline(crd::containers::StringView shader_dir,
                                                                   crd::containers::StringView name, int n_bindings,
                                                                   crd::u32 push_size) override;

    // DX12-specific: a pipeline straight from HLSL source (mirrors the Vulkan backend's create_pipeline_from_spirv). For
    // CKIR's runtime-emitted kernels + tests — NOT on the backend-agnostic IComputeContext (HLSL is a DX12 concern).
    [[nodiscard]] std::unique_ptr<ComputePipeline> create_pipeline_from_hlsl(crd::containers::StringView hlsl,
                                                                             int n_bindings, crd::u32 push_size);

    // DX12-specific: a pipeline straight from PRE-COMPILED DXIL (mirrors create_pipeline_from_spirv). The zero-runtime-compile
    // load path for D-007 D2 cooked `.crdr` bundles — no dxc at runtime.
    [[nodiscard]] std::unique_ptr<ComputePipeline> create_pipeline_from_dxil(crd::containers::ConstSpan<crd::u8> dxil,
                                                                             int n_bindings, crd::u32 push_size);

    // D-007 D4: persist/seed the pipeline library (the PSO cache) across runs — the D3D12 analog of VkPipelineCache. Serialize
    // with pipeline_cache_data() to a file; on the next run, warm_pipeline_cache(blob) BEFORE creating pipelines.
    void               pipeline_cache_data(crd::containers::Array<crd::u8>& out) const;
    [[nodiscard]] bool warm_pipeline_cache(crd::containers::ConstSpan<crd::u8> blob);

    [[nodiscard]] ComputeRecorder& begin() override;
    void                           submit_and_wait() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::gpu
