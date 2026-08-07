#pragma once

// crd-gpu-context-cuda — the CUDA implementation of crd::gpu::IComputeContext (ADR-0100), a THIRD compute backend behind
// the same backend-agnostic dispatch surface as Vulkan + DX12. User-directed 2026-08-07 (CUDA is a first-class GPU-compute
// backend). Raw CUDA driver API + NVRTC (CUDA C -> CUBIN, native SASS), no rendering. NVIDIA-ONLY: the CMake module
// self-skips without the CUDA toolkit, and valid() is false with no device, so non-NVIDIA builds are unaffected.
//
// DEVICE SHARING (no duplicate init): CUDA's per-device PRIMARY context is a refcounted singleton. This backend retains it
// via cuDevicePrimaryCtxRetain — the SAME context engine/kir-cuda (KirBackendCuda) retains — so the CKIR CUDA backend and
// this compute context share one device/context (the gpu-context<->kir device-sharing pattern, CUDA's flavour).
//
// HONEST SEMANTIC: within one CUDA stream kernel/copy ordering is IMPLICIT, so ComputeRecorder::barrier is a documented
// NO-OP on CUDA (a real difference from Vulkan/DX12's explicit barriers — recorded, not hidden).

#include <crd/gpu/compute.hpp>

#include <memory>

namespace crd::memory { class IAllocator; }

namespace crd::gpu
{

// The concrete CUDA context. Consumers normally hold an IComputeContext&; this type adds the CUDA-specific ESCAPE HATCH
// (compile a kernel from CUDA C source — the mirror of Vulkan `from_spirv` / DX12 `from_hlsl`) and last_gpu_ms(). The
// create_pipeline(by-name) path resolves `<name>.cubin` in the shader dir (the cooked-kernel path, like Vulkan
// `<name>.comp.spv`); until a cooked CUDA corpus exists, source compilation via create_pipeline_from_cuda is the path.
class CudaComputeContext : public IComputeContext
{
public:
    // ── ESCAPE HATCH: NVRTC-compile `cuda_source` (CUDA C) to CUBIN, load it, and cache a pipeline whose kernel entry is
    // `entry` (extern "C"), with `n_bindings` buffer params followed by a `push_size`-byte by-value push param. Returns
    // nullptr on compile/load failure (the NVRTC log is written to stderr).
    [[nodiscard]] virtual std::unique_ptr<ComputePipeline>
    create_pipeline_from_cuda(crd::containers::StringView cuda_source, crd::containers::StringView entry, int n_bindings,
                              crd::u32 push_size) = 0;

    // The GPU time (ms) of the most recent submit_and_wait(), measured with CUDA events. 0 if nothing was dispatched.
    // NOTE (integration): once crd::gpu::IComputeContext gains the last_gpu_ms() virtual (the CGP-0 increment), mark this
    // `override` — it is intentionally a plain method here because this worktree's IComputeContext predates that virtual.
    [[nodiscard]] double last_gpu_ms() const noexcept override = 0; // RAH/CGP-0: overrides IComputeContext::last_gpu_ms
};

// Create the CUDA compute context. Returns a context whose valid() is false if there is no CUDA driver/device (a clean,
// inspectable skip — no hidden fallback). `alloc` backs the context's own small host-side allocations. Returned as the
// concrete CudaComputeContext so callers reach create_pipeline_from_cuda + last_gpu_ms; it IS an IComputeContext.
[[nodiscard]] std::unique_ptr<CudaComputeContext> create_cuda_compute_context(crd::memory::IAllocator& alloc);

} // namespace crd::gpu
