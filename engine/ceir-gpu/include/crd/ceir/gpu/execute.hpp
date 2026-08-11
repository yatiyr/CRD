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
} // namespace crd::ceir::gpu
