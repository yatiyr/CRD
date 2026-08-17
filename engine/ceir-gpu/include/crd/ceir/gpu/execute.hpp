#pragma once

// crd-ceir-gpu — the CEIR-13z GPU EXECUTION seam (ADR-0126). Binds a 13d-lowered `Array<LoweredCommand>` to a live ADR-0100
// `IComputeContext` and runs it. ⛔ Two GPU surfaces, two roles (ADR-0126): the execute target is the ADR-0100 compute-KERNEL
// surface (`ComputePipeline`/`ComputeBuffer`/`ComputeRecorder`) — where the CKIR §129 proof kernels + their CPU oracles live
// — NOT the RAF frame-graph `command_model.hpp` `DispatchDesc` path (a separate consumer). The lowering (13d) made the list;
// this binds + runs it (compile ≠ run, §158). This is a BRIDGE header: it names gpu-context types; crd-ceir core never does.

#include <crd/ceir/gpu/lower.hpp> // LoweredCommand / LoweredKind
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/compute.hpp> // IComputeContext / ComputePipeline / ComputeBuffer / ComputeRecorder (ADR-0100)

namespace crd::ceir::gpu
{
// The caller-supplied kernel resolver: a lowered dispatch's source `op` (carrying the 13c `kernel` symbol) → a COMPILED
// `ComputePipeline`. ⛔ Returns nullptr on failure → `ExecuteError::UnresolvedKernel` (the 13d-1b "null program rejected at
// execute" pin, now on the `ComputePipeline` type). The TEST target links crd-kir + a backend to compile the CKIR kernel to
// a `ComputePipeline` and hands this in (the 13c cook `KernelResolveFn` precedent — the bridge NEVER links crd-kir).
using KernelResolveFn = crd::gpu::ComputePipeline* (*)(const Operation* dispatch, void* user);

// One resolved binding: a CEIR resource `Value*` (a dispatch binding operand, `resource_root`-NORMALIZED) → the live
// `ComputeBuffer` the caller uploaded/created. The caller registers every buffer once; `execute_lowered` looks each binding
// operand up here and builds the ordered `ComputeBuffer*` span `ComputeRecorder::dispatch` wants.
struct ResolvedBinding
{
    const Value*             resource = nullptr;
    crd::gpu::ComputeBuffer* buffer   = nullptr;
};

// The typed execute-time error — the `validate_dispatch` semantics re-homed off the (unused) DispatchDesc path (ADR-0126).
enum class ExecuteError : crd::u8
{
    None = 0,
    UnresolvedKernel,   // the resolver returned nullptr for a dispatch
    ZeroDispatch,       // a resolved grid with a zero group (13d-1b parked ZeroDraw at execute — it comes due here)
    BindingArity,       // the dispatch's binding-operand count exceeds kMaxBindings (the command-model structural cap)
    UnmappedBinding,    // a binding operand's (root) Value has no ResolvedBinding entry
    UnsupportedCommand, // a Transfer command, or a dynamic-grid dispatch (dispatch-only / const-grid at 13z-1 — named-forward)
    UnresolvedProgram,  // CEIR-14z-2: a render program/target resolver returned nullptr (a draw's @program or an attachment target)
    NoFrameGraph,       // CEIR-14z-4a: the raster context has no frame graph (create_frame_graph()/create_command_encoder() == nullptr) — a capability absence
    FrameBuildFailed,   // CEIR-14z-4a: the gpu-context frame graph failed to build (an unwritten transient / a cycle in the declared reads/writes)
    // CEIR-17b (scene resolve chain) — append at end.
    SceneChainMisuse,      // a scene.resolve_* chain failed find_scene_misuse (the verifier-first contract — refuse, never a garbage handle)
    UnresolvedSceneHandle, // a scene resolve callback is null (an unwired seam) or returned 0 (an unresolvable handle)
    // CEIR-19c (ceir.rt execution — the execute_rt_lowered surface) — append at end.
    AccelBuildFailed,    // a BuildSceneFn hook returned 0 (the acceleration-structure build failed)
    UnresolvedTlas,      // a ray_query's %tlas operand maps to no earlier AccelBuild %result (an unbuilt / misordered scene)
    TraceDispatchFailed, // a TraceDispatchFn hook returned false (the inline-rayQuery dispatch failed on the device)
    // CEIR-20b (ceir.work execution — the execute_work_lowered surface) — append at end.
    UnresolvedQueue,    // a DispatchIndirect's %queue resolve_queue hook is null / returned kQueueResolveFailed
    WorkDispatchFailed, // a WorkDispatchFn hook returned false (the work dispatch failed on the device)
};
[[nodiscard]] containers::StringView execute_error_name(ExecuteError e) noexcept;

// PURE, DEVICE-FREE structural validation (the always-runs half — guards the all-skip false-green). Walks `commands`; for
// each `Dispatch` checks: kernel resolves (non-null), grid non-zero, binding-operand count ≤ kMaxBindings, every binding
// operand maps. A `Transfer` (or a dynamic-grid dispatch) → `UnsupportedCommand`. A `Barrier` is INERT here (single-dispatch
// programs; the resource-on-barrier mapping is CEIR-13z-3/FFT). Returns the FIRST non-None error (or None). The resolver may
// return a sentinel non-null pointer — validation NEVER dereferences the pipeline, so no device is needed.
[[nodiscard]] ExecuteError validate_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                            KernelResolveFn resolver, void* user,
                                            containers::ConstSpan<ResolvedBinding> bindings);

// Validate, then RECORD each command into `rec` (an already-`begin()`-ed recorder — the caller owns begin()/submit_and_wait()
// + buffer upload/readback, the `ckir_kernel_dispatch.hpp` division). Returns the first `ExecuteError` (or None on success).
// ⛔ device-driving. ⭐ 13z-3 part 2: a `Barrier` is REPLAYED as `rec.barrier(root_buffer, from, to)` per the HazardKind→
// ComputeAccess map (nullptr resource ⇒ all bound buffers); a `Transfer`/dynamic-grid → `UnsupportedCommand`.
[[nodiscard]] ExecuteError execute_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                           crd::gpu::ComputeRecorder& rec, KernelResolveFn resolver, void* user,
                                           containers::ConstSpan<ResolvedBinding> bindings);

// ── CEIR-19c: the ceir.rt EXECUTION seam (§134) ──────────────────────────────────────────────────────────────────────
// A SECOND executor beside execute_lowered, consuming the SAME 13d-lowered list (the render_materialize precedent: ONE
// lowering, surface-specific consumers). The RT surface is the STANDALONE vulkan/dx12 RayTracingContext (its own AS builds +
// an inline-rayQuery trace_dispatch), which the ADR-0100 IComputeContext CANNOT bind (no TLAS descriptor). ⛔ ceir-gpu links
// NO backend, so — exactly like KernelResolveFn — this executor takes CALLER HOOKS: the test/renderer supplies per-backend
// lambdas over VulkanRayTracingContext / Dx12RayTracingContext. ⭐ Barriers are INERT here (each trace_dispatch is
// submit+wait, so no cross-dispatch GPU hazard survives to replay). A portable IRayTracingContext unification (so the
// executor could name one interface instead of hooks) is a FILED follow-up — not this slice.

// An opaque, caller-owned acceleration-structure handle. ceir-gpu NEVER dereferences it — it only threads the handle a
// BuildSceneFn returns (keyed by the AccelBuild op's %result) to the RayQuery that reads that %tlas. 0 ⇒ build failed.
using RtSceneHandle = crd::u64;

// One resolved ray_query SSBO binding: the operand's (resource_root-normalized) Value* → a HOST span (upload/readback) — the
// trace_dispatch host-array model (ReSTIR's same-array in-out precedent, correct for the host-driven wavefront loop, §134
// determinism). The executor orders these by the ray_query's binding-operand order and assigns SSBO slots 1,2,… (the TLAS is
// implicit at descriptor 0, REN-38-A9's one-convention rule — the same kernel runs on the rig, the frame graph, and here).
struct RtHostBinding
{
    const Value* resource = nullptr;
    const void*  upload   = nullptr;
    void*        readback = nullptr;
    crd::u64     bytes    = 0;
};

// Build (a stage of) the acceleration structure named by `accel_op` (rt.blas_build / instance_populate / tlas_build). The
// hook reads the op's geometry/instances operands (+ host buffers via `user`) and returns an opaque handle; the TERMINAL
// tlas handle is what a ray_query binds. A FUSED build_scene backend returns a passthrough handle for blas/instance and the
// real scene at tlas_build. 0 ⇒ AccelBuildFailed.
using BuildSceneFn = RtSceneHandle (*)(const Operation* accel_op, void* user);

// Resolve a ray_query's kernel_ref (its `kernel` symbol) to the COMPILED inline-rayQuery shader BLOB (SPIR-V on Vulkan, DXIL
// on DX12) — NOT a ComputePipeline (the RT surface takes blobs; the trace_dispatch shaderc/dxc precedent). Empty ⇒
// UnresolvedKernel. The pure validator may return a SENTINEL non-empty span (never dereferenced without a device).
using KernelBytesFn = containers::ConstSpan<crd::u8> (*)(const Operation* ray_query, void* user);

// Dispatch the inline-ray-query kernel `kernel_bytes` against `tlas` (bound implicitly at descriptor 0), binding each entry
// of `binds` as an SSBO at slot 1+its-index (binding-operand order), `gx*gy*gz` workgroups. Returns false on failure. This
// IS the `dispatch_inline_ray_query` action the 19a dialect doc names as ray_query's lowering target.
using TraceDispatchFn = bool (*)(RtSceneHandle tlas, containers::ConstSpan<crd::u8> kernel_bytes,
                                 containers::ConstSpan<RtHostBinding> binds, crd::u32 gx, crd::u32 gy, crd::u32 gz,
                                 void* user);

// The caller hooks bundled (the per-backend RT surface). ⛔ ceir-gpu names no backend — the caller wires these over its
// VulkanRayTracingContext / Dx12RayTracingContext (the gate supplies per-backend lambdas, the KernelResolveFn precedent).
struct RtHooks
{
    BuildSceneFn    build_scene    = nullptr;
    KernelBytesFn   kernel_bytes   = nullptr;
    TraceDispatchFn trace_dispatch = nullptr;
    void*           user           = nullptr;
};

// PURE structural validation of the ceir.rt command list (the always-runs half, guarding the all-skip false-green — the
// validate_lowered mirror). For each RayQuery: grid non-zero (ZeroDispatch), a dynamic grid is named-forward
// (UnsupportedCommand — a const-grid witness at stage 1; host-readback drives groups later), kernel_bytes non-empty
// (UnresolvedKernel), the %tlas operand resolves to an earlier AccelBuild %result (UnresolvedTlas). A compute/raster/transfer
// kind here ⇒ UnsupportedCommand. Device-free (the hooks may return sentinels). Returns the FIRST non-None error (or None).
[[nodiscard]] ExecuteError validate_rt_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                               const RtHooks& hooks, containers::ConstSpan<RtHostBinding> bindings);

// Validate, then EXECUTE each RT command via the hooks: AccelBuild → build_scene (the returned handle keyed by the op's
// %result); RayQuery → dispatch_inline_ray_query (kernel_bytes + the %tlas handle + the ordered host SSBO bindings + the
// resolved grid). A Barrier is INERT (submit+wait per trace_dispatch). ⛔ device-driving (THROUGH the caller hooks — ceir-gpu
// itself never touches a queue). Returns the first ExecuteError (or None on success).
[[nodiscard]] ExecuteError execute_rt_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                              const RtHooks& hooks, containers::ConstSpan<RtHostBinding> bindings);
// ── CEIR-20b: the ceir.work EXECUTION seam (§43) ─────────────────────────────────────────────────────────────────────
// A THIRD executor beside execute_lowered / execute_rt_lowered, consuming the SAME 13d-lowered list (the
// render_materialize precedent: ONE lowering, surface-specific consumers). It is the GENERIC host driver for a
// DEVICE-GENERATED-WORK program: walk the plan, run each stage, and SIZE a consume/compact from the queue's DEVICE
// count (read back to host — the 19c host while(count>0) loop, now data-driven from the authored ceir.work asset
// instead of a hand-written C++ loop). ⛔ ceir-gpu links NO backend → caller HOOKS (the execute_rt_lowered precedent):
// the gate/renderer supplies lambdas over its Vulkan/DX12 RT (or compute) context. Barriers are INERT (submit+wait per
// stage lives in the caller's dispatch hook — the readback of stage N coherent before stage N+1, the §134 determinism
// seam). NOTHING wavefront-specific lives here (the algorithm is the authored .frame.toml + .ckir kernels; this
// executor runs ANY ceir.work program).

// An opaque, caller-owned DEVICE-BUFFER handle (the RtSceneHandle precedent — ceir-gpu names no backend; it only
// threads the handle the caller mapped a %queue / binding resource to a NAMED device buffer). ⛔ DEVICE-RESIDENT, not
// host spans: the queue's count lives on-device (produce writes it, an indirect consume reads it), NEVER round-tripping
// host — the ceir.work distinction (host spans would be the 19c host-driven loop 20b retires; the "generic
// queue-buffer" 20c consumes). 0 ⇒ unresolved.
using WorkBufferHandle = crd::u64;

// One resolved resource: a CEIR resource `Value*` → the caller's NAMED device-buffer handle. The caller registers every
// queue AND binding ONCE (the gate reads a named buffer back via debug_scene_buffer after the run);
// execute_work_lowered looks each descriptor operand up here by `resource_root` (a work.queue Extern passes through
// resource_root unchanged, so queues + buffers key uniformly — the ResolvedBinding precedent, DEVICE-RESIDENT).
// Unmapped ⇒ UnmappedBinding.
struct WorkResolvedBinding
{
    const Value*     resource = nullptr;
    WorkBufferHandle buffer   = 0;
};

// Resolve a work op's kernel_ref (its `kernel` symbol) to the COMPILED shader BLOB (SPIR-V/DXIL — the KernelBytesFn
// precedent). Empty ⇒ UnresolvedKernel. The pure validator may return a SENTINEL non-empty span (never dereferenced
// without a device).
using WorkKernelBytesFn = containers::ConstSpan<crd::u8> (*)(const Operation* work_op, void* user);

// DIRECT dispatch (work.produce): run `kernel_bytes` over the const `gx*gy*gz` grid, binding `handles[i]` at descriptor
// i (the op's RESOURCE operands in order — the queue THEN the other bindings; the queue IS a descriptor the producer
// writes). ⛔ The caller's lambda binds a %tlas when `work_op` TRACES (the wavefront's produce=trace) — the `work_op`
// back-pointer carries that (the RtHooks single-hook precedent, TLAS folded into the caller). Returns false on failure.
using WorkDispatchFn = bool (*)(const Operation* work_op, containers::ConstSpan<crd::u8> kernel_bytes,
                                containers::ConstSpan<WorkBufferHandle> handles, crd::u32 gx, crd::u32 gy, crd::u32 gz,
                                void* user);

// INDIRECT dispatch (work.consume / work.compact): the launch grid is the `queue` handle's DEVICE count — the caller
// does an INDIRECT dispatch (vkCmdDispatchIndirect / ExecuteIndirect) reading the count from that buffer (its
// (count,1,1) header at offset 0 doubles as the indirect args — advisor design-lock), so SIZING never round-trips to
// host. `handles` are the op's resource descriptors in order (queue/src first). ⛔ ONLY dispatch sizing is
// device-driven; LOOP TERMINATION stays host-read (submit+wait per stage — the §134 line-2789 seam), a multi-bounce
// concern (declared-forward, not the single-bounce wavefront). The caller binds a %tlas when `work_op` traces
// (consume=shade). false ⇒ failure.
using WorkDispatchIndirectFn = bool (*)(const Operation* work_op, WorkBufferHandle queue,
                                        containers::ConstSpan<crd::u8> kernel_bytes,
                                        containers::ConstSpan<WorkBufferHandle> handles, void* user);

// Record a memory barrier `from→to` on `buffer` into the caller's recorder (rec.barrier). ⛔ execute_work_lowered runs
// the chain in ONE submit (the single-bounce body — the 6-dispatch FFT precedent; per-stage submits return only for
// multi-bounce host-decision termination, ledgered), so the inter-stage hazards the plan carries (lower_region's
// conservative whole-Memory gather) must be REPLAYED here — the RtHostBinding host-span model made them inert, the
// DEVICE-RESIDENT model does not. Naming crd::gpu::ComputeAccess is fine (execute.hpp already names the abstract RHI —
// the rule bars BACKENDS, not crd::gpu). Nullable ⇒ inert (a future per-stage-submit caller stays valid). The one
// hazard the plan can't express — a produce-written queue read by vkCmdDispatchIndirect — the executor owns: it hooks
// (queue, ShaderWrite, IndirectRead) before each indirect dispatch (the C5 pattern ComputeAccess::IndirectRead was
// added for).
using WorkBarrierFn = bool (*)(WorkBufferHandle buffer, crd::gpu::ComputeAccess from, crd::gpu::ComputeAccess to,
                               void* user);

// The caller hooks bundled (the per-backend work surface — the RtHooks shape). ⛔ TWO dispatch hooks: a DIRECT
// (produce, const grid) and an INDIRECT (consume/compact, device-count) — the ceir.work distinction from
// compute.dispatch is device-driven sizing. ⛔ Hooks RECORD into an already-begin()-ed recorder — the caller owns
// begin()/submit_and_wait() + the boundary transitions (upload TransferDst→ShaderWrite on the queue since produce
// WRITES it / →ShaderRead elsewhere; ShaderWrite→TransferSrc before readback — the dispatch_kernel_1wg mold); the
// executor owns the INTER-STAGE barriers (via `barrier`).
struct WorkHooks
{
    WorkKernelBytesFn      kernel_bytes      = nullptr;
    WorkDispatchFn         dispatch          = nullptr; // DIRECT (produce)
    WorkDispatchIndirectFn dispatch_indirect = nullptr; // INDIRECT (consume/compact) — device reads the queue count
    WorkBarrierFn          barrier           = nullptr; // inter-stage memory barriers (nullable ⇒ inert)
    void*                  user              = nullptr;
};

// PURE structural validation of the ceir.work command list (the always-runs half — the validate_rt_lowered mirror). For
// each Dispatch/DispatchIndirect: it is a work op (work.produce/consume/compact — a non-work Dispatch is not this
// surface), kernel_bytes non-empty (UnresolvedKernel), a DispatchIndirect has its %queue operand (operand 0, →
// UnresolvedQueue). A Barrier is inert; a foreign kind (Transfer/render/RT) ⇒ UnsupportedCommand. Device-free (hooks
// may return sentinels). First error (or None).
[[nodiscard]] ExecuteError validate_work_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                                 const WorkHooks& hooks, containers::ConstSpan<WorkResolvedBinding> bindings);

// Validate, then DRIVE the work chain via the hooks: a `Dispatch` (produce) → the DIRECT dispatch hook (const grid); a
// `DispatchIndirect` (consume/compact) → the INDIRECT dispatch hook over the %queue (operand 0) — the device reads the
// count, the host never sizes it. Bindings (operands after the fixed queue prefix: produce 4, consume 1, compact 2)
// resolve to host spans in operand order. ⛔ device-driving THROUGH the hooks (ceir-gpu never touches a queue). First
// ExecuteError (or None).
[[nodiscard]] ExecuteError execute_work_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                                const WorkHooks& hooks, containers::ConstSpan<WorkResolvedBinding> bindings);
} // namespace crd::ceir::gpu
