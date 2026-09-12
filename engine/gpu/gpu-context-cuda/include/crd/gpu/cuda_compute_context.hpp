#pragma once

// crd-gpu-context-cuda — the CUDA implementation of crd::gpu::IComputeContext (ADR-0100), a THIRD compute backend behind
// the same backend-agnostic dispatch surface as Vulkan + DX12. User-directed 2026-08-07 (CUDA is a first-class GPU-compute
// backend). Raw CUDA driver API + NVRTC (CUDA C -> CUBIN, native SASS), no rendering. NVIDIA-ONLY: the CMake module
// self-skips without the CUDA toolkit, and valid() is false with no device, so non-NVIDIA builds are unaffected.
//
// DEVICE SHARING (no duplicate init): CUDA's per-device PRIMARY context is a refcounted singleton. This backend retains it
// via cuDevicePrimaryCtxRetain — the SAME context engine/gpu/kir-cuda (KirBackendCuda) retains — so the CKIR CUDA backend and
// this compute context share one device/context (the gpu-context<->kir device-sharing pattern, CUDA's flavour).
//
// HONEST SEMANTIC: within one CUDA stream kernel/copy ordering is IMPLICIT, so ComputeRecorder::barrier is a documented
// NO-OP on CUDA (a real difference from Vulkan/DX12's explicit barriers — recorded, not hidden).

#include <crd/gpu/compute.hpp>

#include <memory>

namespace crd::memory { class IAllocator; }

namespace crd::gpu
{

// ── CEIR-29b-2: a CAPTURED CUDA graph — a pipeline's dispatches recorded into ONE cudaGraph, INSTANTIATED ONCE, launched
// MANY (the runnable §70 native-graph provider's core; the instantiate-once/launch-many property is the win 29z benches).
// Opaque like ComputePipeline (this header stays cuda.h-free); RAII — its dtor destroys the CUgraphExec + CUgraph. A capture
// that was INVALIDATED (an unsafe call mid-capture) or failed to instantiate yields an INVALID handle: never launch it.
class CudaGraph
{
public:
    CudaGraph()                            = default;
    CudaGraph(const CudaGraph&)            = delete;
    CudaGraph& operator=(const CudaGraph&) = delete;
    CudaGraph(CudaGraph&&)                 = delete;
    CudaGraph& operator=(CudaGraph&&)      = delete;
    virtual ~CudaGraph()                   = default;

    // false ⇒ capture was invalidated / instantiate failed — the caller must NOT launch (the gate REQUIREs this true).
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    // The recorded graph-node count (== the captured dispatch count when uploads are kept outside the capture). 0 if invalid.
    [[nodiscard]] virtual crd::u32 node_count() const noexcept = 0;
};

// The concrete CUDA context. Consumers normally hold an IComputeContext&; this type adds the CUDA-specific ESCAPE HATCH
// (compile a kernel from CUDA C source — the mirror of Vulkan `from_spirv` / DX12 `from_hlsl`) and last_gpu_ms(). The
// create_pipeline(by-name) path resolves `<name>.cubin` in the shader dir (the cooked-kernel path, like Vulkan
// `<name>.comp.spv`); until a cooked CUDA corpus exists, source compilation via create_pipeline_from_cuda is the path.
class CudaComputeContext : public IComputeContext
{
public:
    // ── ESCAPE HATCH: NVRTC-compile `cuda_source` (CUDA C) to CUBIN, load it, and cache a pipeline whose kernel entry is
    // `entry` (extern "C"), with `n_bindings` buffer params followed by a `push_size`-byte by-value push param. `local_size`
    // is the 1-D CUDA BLOCK dim the pipeline launches with (blockDim.x). Returns nullptr on compile/load failure (NVRTC log
    // to stderr) or an invalid `local_size` (0 or > 1024).
    // ⛔ `local_size` is REQUIRED — no default (a defaulted 256 is how the NEXT shared-memory caller silently reproduces the
    // block-dim-mismatch bug). ⛔ Two kernel models: an ELEMENTWISE kernel (guarded `blockIdx.x*blockDim.x+threadIdx.x`) may
    // use any block ≥ its element stride; a SHARED-MEMORY kernel (shared arrays sized to local_size, `__syncthreads()`,
    // cross-thread reads within the workgroup — the CKIR is_kernel path: FFT/transpose/reduce/scan) REQUIRES
    // blockDim.x == local_size. Vulkan/DX12 bake this into the shader; CUDA specifies it at launch, so it rides the pipeline.
    // ⭐ `fmad` is the PER-KERNEL FP-contraction choice (NVRTC `--fmad=`): `true` = FMA-fused (the "Fast tier"); `false` =
    // bit-exact multiply-then-add (matches a per-op-rounding CPU oracle, the CUDA-fan-out convention). ⛔ REQUIRED — a
    // correctness-critical (oracle-matched) kernel MUST pass `false`; a perf kernel passes `true`. (Integer kernels are
    // exact either way.) Vulkan/DX12 never contract, so their kernels are always bit-exact; CUDA is the one backend that
    // chooses, so the choice is explicit per pipeline.
    [[nodiscard]] virtual std::unique_ptr<ComputePipeline>
    create_pipeline_from_cuda(crd::containers::StringView cuda_source, crd::containers::StringView entry, int n_bindings,
                              crd::u32 local_size, crd::u32 push_size, bool fmad) = 0;

    // The GPU time (ms) of the most recent submit_and_wait(), measured with CUDA events. 0 if nothing was dispatched.
    // NOTE (integration): once crd::gpu::IComputeContext gains the last_gpu_ms() virtual (the CGP-0 increment), mark this
    // `override` — it is intentionally a plain method here because this worktree's IComputeContext predates that virtual.
    [[nodiscard]] double last_gpu_ms() const noexcept override = 0; // RAH/CGP-0: overrides IComputeContext::last_gpu_ms

    // ── CEIR-29b-2: CUDA-Graphs capture (NVIDIA-only — the runnable §70 native-graph provider; NO cross-backend mirror, per
    // the 29-0 census: portability lives in the device-free partitioner, the runnable proof is CUDA-only). begin_capture()
    // starts stream capture on the SAME stream begin()/dispatch use and returns the SAME recorder, so the SAME
    // execute_tensor_pipeline RECORDS its dispatches into a graph instead of launching them. ⛔ capture the DISPATCHES ONLY —
    // run the H->D uploads via begin()/submit_and_wait() FIRST, because a captured cuMemcpyAsync would re-run on every launch.
    [[nodiscard]] virtual ComputeRecorder& begin_capture() = 0;
    // End capture and INSTANTIATE the graph ONCE (the launch-many win). Returns an INVALID CudaGraph (valid()==false) if the
    // capture was invalidated/unjoined or instantiate failed — never a nullptr, so the caller always REQUIREs valid().
    [[nodiscard]] virtual std::unique_ptr<CudaGraph> end_capture() = 0;
    // Launch a captured graph on the stream and WAIT — bracketed by the same events, so last_gpu_ms() reports graph-launch
    // time via the SAME IComputeContext accessor the tune harness reads. Reusable (REPLAY): nothing re-instantiates. No-op on
    // an invalid graph.
    virtual void launch(const CudaGraph& graph) = 0;
    // ── CEIR-29z: ENQUEUE a captured graph on the recording stream WITHOUT waiting or its own event bracket — the launch()-vs-
    // enqueue split (the 29b-2a end_capture/launch split rationale, one level down). A TWO-CLASS pipeline records eager prefix
    // dispatches, enqueues the captured run, and records eager suffix dispatches into ONE begin()/submit_and_wait() bracket:
    // same-stream order carries the x'->graph->z dependency (CUDA barrier is a no-op) and there is ONE wait, so the CPU submit
    // path + last_gpu_ms() are measured on the SAME bracket structure as the N-dispatch fallback (the 29z bench's honesty). Use
    // BETWEEN begin() and submit_and_wait(); launch() is the standalone enqueue+bracket+wait form. No-op on an invalid graph.
    virtual void enqueue(const CudaGraph& graph) = 0;
};

// Create the CUDA compute context. Returns a context whose valid() is false if there is no CUDA driver/device (a clean,
// inspectable skip — no hidden fallback). `alloc` backs the context's own small host-side allocations. Returned as the
// concrete CudaComputeContext so callers reach create_pipeline_from_cuda + last_gpu_ms; it IS an IComputeContext.
[[nodiscard]] std::unique_ptr<CudaComputeContext> create_cuda_compute_context(crd::memory::IAllocator& alloc);

} // namespace crd::gpu
