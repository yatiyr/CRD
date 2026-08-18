#pragma once

// crd-ceir-gpu — CEIR-22c: the TENSOR-PIPELINE EXECUTOR (§158 the run-half beside plan_tensor_pipeline's compile-half). RECORDS
// a TensorPipelinePlan into a caller-owned `crd::gpu::ComputeRecorder` (already begin()-ed): each stage's dispatch + an
// inter-stage ShaderWrite→ShaderRead barrier on each written output — so the whole GEMM→FFT→reduction→viz chain runs in ONE
// submit with the intermediates DEVICE-RESIDENT (§137 "no CPU round-trip"). ⛔ ceir-gpu names NO backend: the caller's RESOLVER
// does the backend half (re-synthesize the plan stage's op → emit GLSL/HLSL → compile → ComputePipeline, + the grid the
// emitter's local_size implies + the push blob), exactly like execute_lowered's KernelResolveFn. The caller owns
// begin()/submit_and_wait() + buffer upload/readback + the boundary transitions (the dispatch_kernel_1wg division of labor).

#include <crd/ceir/gpu/execute.hpp>         // ExecuteError (reused — UnresolvedKernel / UnmappedBinding / BindingArity)
#include <crd/ceir/gpu/tensor_pipeline.hpp> // TensorPipelinePlan / PlanStage
#include <crd/containers/span.hpp>
#include <crd/gpu/compute.hpp> // ComputeRecorder / ComputePipeline / ComputeBuffer / ComputeAccess (ADR-0100)

namespace crd::ceir::gpu
{
// A plan stage resolved to a recordable dispatch (the caller's backend half). ⛔ `pipeline` nullptr ⇒ ExecuteError::UnresolvedKernel.
struct ResolvedStage
{
    crd::gpu::ComputePipeline* pipeline  = nullptr;
    crd::u32                   gx        = 1; // workgroup grid (the emitter's local_size + the stage shape imply it — caller-derived)
    crd::u32                   gy        = 1;
    crd::u32                   gz        = 1;
    // the stage's push blob INLINE (gemm {m,k,n,batch} 16B; reduce {nout,redsize,pad,pad} 16B; fft push_size 0) — inline so it
    // survives to the executor's rec.dispatch (a `const void*` to the resolver's local would DANGLE). push_size ≤ 16.
    alignas(16) crd::u8 push[16] = {};
    crd::u32            push_size = 0;
};
// The caller's per-stage resolver: a PlanStage → a ResolvedStage. Called ONCE per stage at record time (the KernelResolveFn mold).
using StageResolveFn = ResolvedStage (*)(const PlanStage& stage, void* user);

// PURE, device-free structural validation (the always-runs half — the validate_lowered mirror). Every stage's bind[0..nbind)
// must index into a `[0, n_buffers)` buffer table + nbind ≤ 8. Returns the FIRST ExecuteError (UnmappedBinding / BindingArity)
// or None. Does NOT call the resolver (no device).
[[nodiscard]] ExecuteError validate_tensor_pipeline(const TensorPipelinePlan& plan, crd::u32 n_buffers);

// Validate, then RECORD the pipeline into `rec`: per stage — resolve → assemble the ordered `ComputeBuffer*` bindings
// (stage.bind[i] → `buffers[bind[i]]`, the 13a positional-slot order) → `rec.dispatch(...)`; then, after every NON-final stage,
// a `rec.barrier(out_buf, ShaderWrite, ShaderRead)` on each of the stage's `n_out` written outputs (the last n_out binds). ⛔
// `buffers` is indexed by PLAN buffer index (one ComputeBuffer* per PlanBuffer — the caller created GpuOnly intermediates +
// uploaded externals). ⛔ device-driving (records into the caller's already-begin()-ed recorder). First ExecuteError or None.
[[nodiscard]] ExecuteError execute_tensor_pipeline(const TensorPipelinePlan& plan, crd::gpu::ComputeRecorder& rec,
                                                   StageResolveFn resolve, void* user,
                                                   containers::ConstSpan<crd::gpu::ComputeBuffer*> buffers);
} // namespace crd::ceir::gpu
