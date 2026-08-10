# ADR-0126 — the CEIR-13z GPU execution seam (`execute_lowered` on IComputeContext)

**Status:** ACCEPTED 2026-08-10 · **Band:** CEIR-13z (the §129 execution proof) · **Module:** `crd-ceir-gpu` (bridge) ·
**Supersedes/strikes:** ADR-0125 §2.3 (the "validate_dispatch on a resolved DispatchDesc at execute" clause) + the
lower.hpp "binds to a gpu-context DispatchDesc at execute" comments — corrected in place here (§4).

## Context

CEIR-13d built `lower_region` → an inspectable `Array<LoweredCommand>` with UNRESOLVED kernel/binding handles (the CEIR
`kernel` symbol + resource `Value*`s ride each command's `op` back-pointer). 13z EXECUTES that list on a live device and
proves `add`·`reduce`·`scan`·FFT (as CEIR program assets) bit-exact vs the existing CKIR CPU oracles. This ADR pins the
execution seam.

## Two GPU surfaces, two roles (the load-bearing finding)

The engine has **two** compute-facing surfaces, and they use incompatible resource types. Naming which one 13z executes
through is the central decision:

| surface | types | role | who uses it |
|---|---|---|---|
| **ADR-0100 `IComputeContext`** | `ComputePipeline` (by-name), `ComputeBuffer`, `ComputeRecorder` | the **standalone compute-kernel execution** surface | the CKIR kernels + their CPU oracles + `tests/gpu-shared/ckir_kernel_dispatch.hpp`; every device CKIR compute test (Vulkan + DX12) |
| **RAF-2 `command_model.hpp` `DispatchDesc`/`ICommandEncoder`** | `IGpuProgram`, `IStorageBuffer` (render-asset) | the **raster/frame-graph declarative** command model | `render-graph/frame_graph.cpp::record_compute_dispatch` (a DispatchDesc into a frame's raster command buffer); both raster contexts' `create_command_encoder()` |

⛔ `DispatchDesc` is NOT dead code (a false "unwired" claim would be its own source≠scoreboard bug — verified: the render
graph records it). The two surfaces coexist by design.

**Decision: `execute_lowered` targets `IComputeContext`.** The §129 proof reuses the EXISTING CKIR kernels + oracles, which
live entirely on the ADR-0100 surface (`create_pipeline_from_{hlsl,glsl}` → `ComputePipeline`; `ComputeRecorder::dispatch`).
Routing 13z through the frame-graph `DispatchDesc` path would require re-cooking every proof kernel as an `IGpuProgram` +
render-asset `IStorageBuffer`s — a rewrite with no oracle on the other side. A future PARTITIONER (CEIR-21/26) may route
CEIR compute regions through EITHER surface; 13z picks the kernel-execution surface because that is where the proof lives.

## Decision — the seam

### 3.1 `execute_lowered` (crd-ceir-gpu, the bridge)

```
// The caller-supplied kernel resolver: LoweredCommand.op (the ceir.compute dispatch) → a compiled ComputePipeline.
// ⛔ Returns nullptr on failure (→ ExecuteError::UnresolvedKernel) — the 13d "null program rejected at execute" pin, now on
// the ComputePipeline type (NOT a command_model DispatchDesc). The TEST target links crd-kir + a backend to compile the
// CKIR kernel to a ComputePipeline and hands this in (the 13c cook KernelResolveFn precedent; NO crd-kir edge in the bridge).
using KernelResolveFn = crd::gpu::ComputePipeline* (*)(const Operation* dispatch, void* user);

// The binding table: a CEIR resource Value* (a dispatch's binding operand, NORMALIZED through ctx.resource_root) → the live
// ComputeBuffer the caller uploaded/created. Positional binding index = the dispatch operand order (compute.ops slot rule).
struct ResolvedBinding { const Value* resource; crd::gpu::ComputeBuffer* buffer; };

enum class ExecuteError : crd::u8 {
    None = 0,
    UnresolvedKernel,   // resolver returned nullptr
    ZeroDispatch,       // a resolved grid with a zero group (13d-1b parked ZeroDraw HERE — it comes due at execute)
    BindingArity,       // the dispatch's binding-operand count != the resolved bindings supplied
    UnmappedBinding,    // a binding operand's (root) Value has no ResolvedBinding entry
    UnsupportedCommand, // a Transfer command (dispatch-only at 13z-1; buffer-transfer execution is named-forward)
};
```

- **`validate_lowered(ctx, commands, resolver, bindings) → ExecuteError`** — PURE, device-free: the four structural checks
  above. This is the always-runs half (no GPU needed — the resolver may return a sentinel non-null pointer; validation never
  dereferences the pipeline). ⛔ Guards the all-skip false-green: a device suite that skips every case reads as PASS, so
  every 13z checkpoint ships these device-free assertions.
- **`execute_lowered(ctx, commands, IComputeContext&, resolver, bindings) → ExecuteError`** — validates, then for each
  `Dispatch`: resolve the pipeline, gather its `ComputeBuffer*` bindings (operand order, `resource_root`-normalized lookup),
  `rec.dispatch(pipe, buffers, push=nullptr, 0, gx, gy, gz)` (or `dispatch_indirect` when `dispatch_kind==Indirect`); for a
  `Barrier`, see §3.3; a `Transfer` → `UnsupportedCommand`. The caller owns `begin()`/`submit_and_wait()` and buffer
  upload/readback (the `ckir_kernel_dispatch.hpp` division — the CEIR program is the DISPATCH; I/O is the harness's).

### 3.2 Re-homed validation (what `validate_dispatch` used to cover)

The command_model `validate_dispatch(DispatchDesc)` does not run on this path (there is no desc). Its semantics move into
`ExecuteError`: NullProgram→`UnresolvedKernel`, ZeroDraw→`ZeroDispatch` (explicitly parked here by 13d-1b), plus the two the
lowering can't see (binding arity, unmapped Value) which need the resolved bindings. `find_dispatch_misuse` remains the IR
verifier (grid TYPES, access tokens) — unchanged; these are the EXECUTE-time structural checks it deliberately doesn't do.

### 3.3 The Barrier mapping — RESOLVED at 13z-3 (option a, per-resource; a 13d correctness completion)

⭐ **RESOLVED (CEIR-13z-3 part 1, advisor-reviewed): option (a), per-resource.** `LoweredCommand::Barrier` gains ONE
`const Value* resource` field (appended). `lower_region` now emits, before each dispatch, **one Barrier per conflicting root
resource** the dispatch touches (in `after`'s binding-operand order), each carrying `{hazard, before=the nearest earlier
writer, after, resource}`. ⛔ this is a 13d CORRECTNESS COMPLETION, not just an execution convenience: the part-2 "one barrier
per dispatch, nearest-strongest" DROPPED conflicts — a dispatch reading N buffers written by N prior passes recorded only
ONE, and the multi-dispatch FFT is exactly where the second conflict is real (a missed inter-pass barrier = a data race). The
whole-op `precise_hazard` was retired; the per-pair `conflict`/`pair_hazard`/`hazard_rank` primitives ARE the per-resource
core (the two-hazard-notions design still holds — this bridge scan stays narrowed; core `ops_hazard` stays conservative).
`test_lower.cpp` gains a discriminating multi-resource test (a dispatch reading 2 buffers from 2 writers → 2 RAW barriers).

- **(b)** — conservatively barrier every buffer `before` wrote — REJECTED (coarser + loses the §162 per-resource artifact).

The `HazardKind → (ComputeAccess from, ComputeAccess to)` map + the actual `rec.barrier` emission are `execute_lowered`'s
(13z-3 part 2, dispatch→dispatch only): RAW→`ShaderWrite→ShaderRead`, WAW→`ShaderWrite→ShaderWrite`, WAR→`ShaderRead→
ShaderWrite` (WAR pending a per-backend `ComputeRecorder::barrier` check). ⛔ the fft2d `TransferDst → ShaderRead` upload
barrier is the HARNESS's job BY CONSTRUCTION (a dispatch-only CEIR asset has no transfer; `execute_lowered` sees only
dispatch→dispatch hazards, so the producer-op-kind distinction never reaches it) — the reference `dispatch_fft2d` does the
upload copies + that barrier, exactly as `dispatch_ceir_multi` will.

## Scope pins (13z-1)

- **Dispatch-only.** A `Transfer` command → `UnsupportedCommand`. (command_model `TransferDesc` is image-oriented
  [`IRasterTarget`]; the 13b `ceir.transfer` ops are BUFFER movement — a separate mismatch, not solved here. Buffer-transfer
  execution is a named-forward; 13z uploads/reads via the harness's `IComputeContext`.)
- **Binding lookup normalizes through `ctx.resource_root`** (the CEIR-13d part-3 machinery) before the table lookup; byte-
  range sub-views are documented unsupported (identity-only), consistent with the hazard walk.
- **`crd::gpu::DispatchKind` stays on `LoweredCommand`** — a selector enum (Direct/Indirect), no churn; it does not imply the
  DispatchDesc execution surface.
- **crd-ceir CORE stays host-only/jobs-free/asset-free (I3/I4/I5).** `execute_lowered` + the resolver + the CPU-oracle
  reference all live in the crd-ceir-gpu BRIDGE or the test target; the core Interpreter stays `NoSemantics` for dispatch
  (the §150 forward — the reference-executor leg is a bridge-side CPU-oracle run at 13z-4, not core Interpreter semantics).

## Consequences

The lowering (13d) → execution (13z) split is complete: 13d made the inspectable list, 13z binds it to the ADR-0100 kernel
surface and runs it. `IExecutionProvider` wrapping (advertise/cost/plan) is a CEIR-21/26 named-forward — nothing consumes it
for GPU until the partitioner. Two GPU surfaces remain distinct by role; a future partitioner chooses per region.

Delivered across checkpoints: **13z-1** this seam + `add` on both backends; **13z-2** reduce/scan (text+builder); **13z-3**
FFT (the barrier mapping comes due); **13z-4** the reference-executor leg + hot-reload swap; **13z-z** the band gate + the
ADR-0108 cornerstone flip.
