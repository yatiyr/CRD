#pragma once

// crd-ceir-gpu — CEIR-22c: the TENSOR-PIPELINE EXECUTOR (§158 the run-half beside plan_tensor_pipeline's compile-half). RECORDS
// a TensorPipelinePlan into a caller-owned `crd::gpu::ComputeRecorder` (already begin()-ed): each stage's dispatch + an
// inter-stage ShaderWrite→ShaderRead barrier on each written output — so the whole GEMM→FFT→reduction→viz chain runs in ONE
// submit with the intermediates DEVICE-RESIDENT (§137 "no CPU round-trip"). ⛔ ceir-gpu names NO backend: the caller's RESOLVER
// does the backend half (re-synthesize the plan stage's op → emit GLSL/HLSL → compile → ComputePipeline, + the grid the
// emitter's local_size implies + the push blob), exactly like execute_lowered's KernelResolveFn. The caller owns
// begin()/submit_and_wait() + buffer upload/readback + the boundary transitions (the dispatch_kernel_1wg division of labor).

#include <crd/ceir/gpu/execute.hpp>         // ExecuteError (reused — UnresolvedKernel / UnmappedBinding / BindingArity)
#include <crd/ceir/gpu/partition_ml.hpp>    // CEIR-30b-2a: MlProvider — the stage_class_from_partition adapter (the 29c descriptor path)
#include <crd/ceir/gpu/tensor_pipeline.hpp> // TensorPipelinePlan / PlanStage
#include <crd/ceir/semantics.hpp>           // CEIR-30b-2a: ProviderClass (§69 Host/Gpu) — the two-class placement axis
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/gpu/compute.hpp> // ComputeRecorder / ComputePipeline / ComputeBuffer / ComputeAccess (ADR-0100)

namespace crd::kir
{
class KGraph;  // CEIR-30b-2b-1: the Host VizDispatch resolver hands back a kernel-tier graph
struct KEntry; //                + its entry (the eval_cpu_kernel inputs) — forward-declared (refs only in the typedef)
} // namespace crd::kir

namespace crd::ceir::gpu
{
// §137 "profiling" — the STRUCTURAL per-stage profile, derived at RECORD time from the plan binds + the ResolvedStage grid (no
// device timing here: the executor stays backend-free; the caller stamps the total submit WALL-time around submit_and_wait, and
// per-stage GPU timestamp queries are a ledgered name-forward). One row per DISPATCHED stage, in execution order.
struct TensorStageProfile
{
    StageKind kind       = StageKind::Gemm;
    crd::u32  gx         = 1; // the resolved workgroup grid (the asset/emitter-derived dispatch dims)
    crd::u32  gy         = 1;
    crd::u32  gz         = 1;
    crd::u64  workgroups = 0; // gx·gy·gz
    crd::u64  bytes_in   = 0; // Σ bytes of the stage's INPUT binds (the first nbind−n_out)
    crd::u64  bytes_out  = 0; // Σ bytes of the stage's OUTPUT binds (the trailing n_out)
};
// The inspectable §137 profile: the ordered per-stage rows. Total submit wall-time is CALLER-owned (kept out — the executor
// records, it never times), so a consumer pairs this with its own around-submit clock.
struct TensorPipelineProfile
{
    containers::Array<TensorStageProfile> stages;
    explicit TensorPipelineProfile(memory::IAllocator* alloc) : stages(alloc) {}
};

// A plan stage resolved to a recordable dispatch (the caller's backend half). ⛔ `pipeline` nullptr ⇒ ExecuteError::UnresolvedKernel.
struct ResolvedStage
{
    crd::gpu::ComputePipeline* pipeline  = nullptr;
    crd::u32                   gx        = 1; // workgroup grid (the emitter's local_size + the stage shape imply it — caller-derived)
    crd::u32                   gy        = 1;
    crd::u32                   gz        = 1;
    // the stage's push blob INLINE (gemm {m,k,n,batch} 16B; reduce {nout,redsize,pad,pad} 16B; fft push_size 0) — inline so it
    // survives to the executor's rec.dispatch (a `const void*` to the resolver's local would DANGLE). push_size ≤ 16.
    crd::u8  push[16] = {}; // ≤16B, naturally 4-aligned (u32 push structs); vkCmdPushConstants copies by-bytes
    crd::u32 push_size = 0;
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
// ⛔ `profile` (default nullptr) — when non-null, one TensorStageProfile row per stage is APPENDED (the §137 structural profile).
[[nodiscard]] ExecuteError execute_tensor_pipeline(const TensorPipelinePlan& plan, crd::gpu::ComputeRecorder& rec,
                                                   StageResolveFn resolve, void* user,
                                                   containers::ConstSpan<crd::gpu::ComputeBuffer*> buffers,
                                                   TensorPipelineProfile* profile = nullptr);

// CEIR-30b-1 (§68/§103 — the HOST provider class) — the CPU MIRROR of execute_tensor_pipeline: run the SAME TensorPipelinePlan on
// the CPU by re-synthesizing each stage's op (synth_gemm/reduce/transpose/broadcast/elementwise — the graph-tier CKIR) and
// interpreting it with the f32-faithful CPU reference `kir::eval_cpu` (round_dtype per elementary op ⇒ an F32 graph is BIT-EXACT
// vs a naive f32 GPU kernel — the same oracle every device backend proves against). This is the second real §24 provider class
// beside the GPU executor: a GENERAL CPU tensor runner (not per-fixture glue), so 30b-2's two-class orchestrator picks a runner by
// `PlanStage.provider`. ⛔ NO resolver + NO recorder: the CPU eval IS the backend (crd-kir is header-only + GPU-free, so ceir-gpu
// calls it directly — no backend is named). `buffers` is indexed by PLAN buffer index (one HOST f32 array per PlanBuffer — the
// caller allocated intermediates + filled externals; aliased buffers point at their landlord, the GPU-runner contract). Per stage:
// f32→f64 each INPUT bind (the first nbind−n_out, in the 13a positional-slot order = the synth's Input iidx order), eval, f64→f32
// the single trailing OUTPUT bind (LOSSLESS — eval_cpu already rounded every op to F32). ⛔ Fft (kernel-tier) + VizDispatch /
// Dequant / QuantGemm (authored .ckir) are NOT graph-tier ⇒ ExecuteError::UnresolvedKernel (a TYPED reject, NEVER a silent skip;
// a CPU eval_cpu_kernel + authored-kernel path is name-forward). `profile` (default nullptr): one row per stage — bytes_in/out +
// kind are meaningful; the CPU has no workgroup grid, so gx/gy/gz=1, workgroups=0. First ExecuteError or None.
// CEIR-30b-2b-1 — the Host resolver for a VizDispatch stage's AUTHORED kernel: given the dispatch's `kernel` symbol, hand back the
// kernel-tier CKIR graph + entry (the caller `ckir_read`s the .ckir asset — the ENGINE names no asset path, the StageResolveFn
// mold). Returns false ⇒ the stage is UnresolvedKernel (unknown symbol / bad read). ⛔ a plain fn-ptr (no captures) + a `void*`.
using HostKernelResolveFn = bool (*)(containers::StringView symbol, kir::KGraph& g, kir::KEntry& entry, void* user);

// CEIR-30b-2b-1 — the OPTIONAL Host-run knobs (default = none, so every 30b-1 caller is byte-identical). `kernel` (+ `user`)
// enables the VizDispatch kernel-tier path (ckir_read → eval_cpu_kernel); ABSENT ⇒ a VizDispatch stage stays UnresolvedKernel
// (the 30b-1 behavior + its (c) gate are preserved). ⛔ a future `subgroup_lanes` override (eval_cpu_kernel's REN-38 param) lands
// HERE when a Host kernel with subgroup ops arrives — name-forward, do NOT add it now.
struct HostRunOptions
{
    HostKernelResolveFn kernel = nullptr;
    void*               user   = nullptr;
};

[[nodiscard]] ExecuteError execute_tensor_pipeline_host(const Context& ctx, const TensorPipelinePlan& plan,
                                                        containers::ConstSpan<crd::f32*> buffers, memory::IAllocator* alloc,
                                                        TensorPipelineProfile* profile = nullptr, HostRunOptions opts = {});

// CEIR-30b-2a (§68/§103/§24 — the TWO-CLASS PLACEMENT→TRANSFER pass) — the direction a cross-domain copy moves a buffer across the
// §24 host↔device boundary. ⛔ append at END.
enum class TransferDir : crd::u8
{
    HostToDevice = 0, // upload — the buffer's authoritative contents live on Host, a Gpu stage needs them on the device
    DeviceToHost,     // readback — contents live on Gpu, a Host stage (or the final Output-must-be-host-visible rule) needs them
};
[[nodiscard]] containers::StringView transfer_dir_name(TransferDir d) noexcept;

// One planned cross-domain transfer: PLAN buffer `buffer` (the LANDLORD — aliases resolved to their realized slot) must be copied
// `direction` BEFORE stage `before_stage`. ⛔ `before_stage == plan.stages.size()` is the FINAL Output readback (after the last
// stage — the "must end Host-visible" rule). Pure DATA — the 30b-2b two-class runner APPLIES these; no device here.
struct Transfer
{
    crd::u32    buffer       = 0;
    crd::u32    before_stage = 0;
    TransferDir direction    = TransferDir::HostToDevice;
};
struct TransferPlan
{
    containers::Array<Transfer> transfers;
    explicit TransferPlan(memory::IAllocator* a) : transfers(a) {}
};

// CEIR-30b-2a — the PURE, device-free PLACEMENT→TRANSFER pass: given `stage_class` (one ProviderClass per plan stage — PLACEMENT
// AS DATA, hand-written by a fixture OR produced by 30b-3's sharding placement), derive the host↔device transfers the plan's
// def-use edges imply. Per stage, for each INPUT bind, resolve to the LANDLORD buffer + its last WRITER (a prior stage's trailing
// output, or an ExternalIn); if the writer's class ≠ the reading stage's class, a transfer crosses the §24 boundary BEFORE that
// stage. ⛔ MODEL (the delegated choice, documented): ExternalIn buffers are **Host-BORN** (the caller holds the f32 array), so a
// Gpu stage reading one plans an UPLOAD; and the Output-must-be-Host-visible readback IS a planned transfer (before_stage ==
// num_stages) — so the runner does ZERO hand-copying, ALL host↔device movement is planned (the "placement is semantic, nothing
// glue" §140 property). Deduped on (landlord, last-writer-stage, direction) — a buffer read twice on the same class with no
// intervening write crosses ONCE; a new write to a buffer invalidates its prior transfers. ⛔ `stage_class.size()` MUST ==
// plan.stages.size() (a mismatch → an empty plan, defensive). PURE (no device, no synth, no mutation).
[[nodiscard]] TransferPlan plan_transfers(const TensorPipelinePlan& plan, containers::ConstSpan<ProviderClass> stage_class,
                                          memory::IAllocator* alloc);

// CEIR-30b-2a — the ADAPTER: derive a per-stage ProviderClass vector from a PARTITIONED plan's `PlanStage.provider` tags (the 29c
// descriptor path). class = `providers[provider].provider_class`; a fallback stage (provider < 0, or an out-of-range index —
// bounds-guarded, defensive) → `fallback_class` (the CKIR fallback's execution class — Gpu in the 29c device world). So a caller
// holding a partition feeds `plan_transfers` WITHOUT hand-writing the vector (30b-3's sharding placement is the OTHER producer of
// the same vector — two producers, one consumer). PURE.
[[nodiscard]] containers::Array<ProviderClass> stage_class_from_partition(const TensorPipelinePlan& plan,
                                                                          containers::ConstSpan<MlProvider> providers,
                                                                          ProviderClass fallback_class, memory::IAllocator* alloc);

// CEIR-30c — the THIRD producer of the ONE per-stage ProviderClass vector (beside stage_class_from_partition + a fixture's hand-
// written vector): from an AUTHORED PLACEMENT `rank_classes` (one ProviderClass per mesh rank, from placement_from_transform).
// class = `rank_classes[stage.provider]`; a stage NO rank owns (provider < 0, or an out-of-range index — bounds-guarded) →
// `fallback_class` (the all-reduce COMBINE). ⛔ NO MlProvider indirection — a per-rank ProviderClass span in, avoiding the
// hand-composed-MlProvider drift (the compose_partition_constraint mold). ⛔ agrees with stage_class_from_partition on the same
// plan when its `providers[i].provider_class == rank_classes[i]` (the gate asserts this identity). PURE.
[[nodiscard]] containers::Array<ProviderClass> stage_class_from_placement(const TensorPipelinePlan& plan,
                                                                          containers::ConstSpan<ProviderClass> rank_classes,
                                                                          ProviderClass fallback_class, memory::IAllocator* alloc);

// CEIR-30b-2b-2 — a sub-plan over stages [lo, hi): the SAME buffer table (every PlanBuffer copied, indices unchanged, so
// validate_tensor_pipeline's n_buffers + every stage's bind[] stay valid), only the stage window narrows. The two-class runner
// slices each stage to a 1-stage sub-plan so a single-class runner records/evals exactly that stage against the shared buffers.
// PURE (a plan copy, no device). ⛔ signature EXACTLY mirrors the former test-local so the CUDA test's calls resolve here by ADL.
[[nodiscard]] TensorPipelinePlan slice_plan(const TensorPipelinePlan& plan, crd::usize lo, crd::usize hi,
                                            memory::IAllocator* alloc);

// CEIR-30b-2b-2 — the device-class caller's per-stage runner: record + dispatch + await the 1-stage `stage_slice` on the caller's
// device, over its OWN buffer table (indexed by PLAN buffer index — the slice keeps every buffer). Returns the stage's ExecuteError
// (None on success). ⛔ the ENGINE names no backend — the device-class caller (a CUDA/… test or app) supplies this + owns the table.
using GpuStageFn = ExecuteError (*)(const TensorPipelinePlan& stage_slice, void* gpu_user);

// CEIR-30b-2b-2 — the device-class caller's cross-domain copy: move `bytes` of PLAN buffer `t.buffer` (the LANDLORD) across the §24
// boundary — HostToDevice uploads `host_ptr` (== the runner's host_bufs[t.buffer]) to the caller's device buffer, DeviceToHost reads
// it back into `host_ptr`. Returns None on success. The runner owns the byte count (from the plan); the fn owns the device table.
using TransferFn = ExecuteError (*)(const Transfer& t, crd::f32* host_ptr, crd::u64 bytes, void* gpu_user);

// CEIR-30b-2b-2 (§68/§103/§140 — the TWO-CLASS RUNNER) — execute a mixed Host+device plan, placement AS DATA. `stage_class` (one
// ProviderClass per stage — a fixture's hand-written vector, or 30b-3's sharding placement) drives it: a Host stage runs on the CPU
// via execute_tensor_pipeline_host over `host_bufs` (one f32 array per PLAN buffer — externals filled by the caller, aliases point
// at their landlord); a device stage runs via `gpu` over the caller's OWN device buffer table (reached through `gpu_user`, never
// seen here). The host↔device crossings are DERIVED INTERNALLY (`plan_transfers(plan, stage_class)`) and applied via `xfer` at each
// stage boundary — the runner hand-copies NOTHING, and the Output ends Host-visible in `host_bufs` (the "placement is semantic, not
// glue" §140 property). ⛔ `stage_class.size()` MUST == plan.stages.size() (else BindingArity — the placement vector's arity ≠ the
// plan's; plan_transfers would silently yield an empty plan). `host_opts` threads the VizDispatch kernel resolver to the Host stages
// (a two-class relu sandwich is UnresolvedKernel without it). `profile` (default nullptr): one row per stage in stage order (Host +
// device alike, gx/gy/gz=1/workgroups=0 — the runner has no grid). First ExecuteError (validate / a Host stage / a `gpu` return / a
// `xfer` return) aborts — nothing after it runs.
[[nodiscard]] ExecuteError execute_two_class(const Context& ctx, const TensorPipelinePlan& plan,
                                             containers::ConstSpan<ProviderClass> stage_class,
                                             containers::ConstSpan<crd::f32*> host_bufs, void* gpu_user, GpuStageFn gpu,
                                             TransferFn xfer, memory::IAllocator* alloc, HostRunOptions host_opts = {},
                                             TensorPipelineProfile* profile = nullptr);
} // namespace crd::ceir::gpu
