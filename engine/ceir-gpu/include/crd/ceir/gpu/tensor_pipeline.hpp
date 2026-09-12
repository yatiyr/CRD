#pragma once

// crd-ceir-gpu — CEIR-22c: the TENSOR-PIPELINE PLAN (§137: GEMM→FFT→reduction→viz-prep as ONE device-resident asset). The
// PURE, device-free HALF (the lower_region→execute_lowered §158 split): `plan_tensor_pipeline` walks a verify-clean module of
// high-level ops (linalg.gemm / tensor.fft / tensor.reduce / tensor.reshape / compute.dispatch [the viz kernel]) and derives —
// FROM THE SSA DEF-USE EDGES, not stage adjacency — the DEVICE-RESIDENT BUFFER WIRING: op A's result Value feeding op B's
// operand ⇒ the SAME buffer (so a re-wired asset PLANS DIFFERENTLY BY CONSTRUCTION — the 20c-2 asset-drives-it rule). The plan
// IS §137's "memory plan visible": buffers with a ROLE (external-in / intermediate / output / alias) + bytes + per-stage
// bindings, an INSPECTABLE struct. ⛔ NOT the 12d `plan_block_memory` (it keys off declared effects; these tensor ops are Pure
// with none — interval-coloring/aliasing is a ledgered slice). ⛔ SYNTHESIS-lite: the plan typed-rejects any op ckir_synth
// rejects (mirroring SynthReject) but does NOT own the KGraph — the executor (22c-2) RE-synthesizes (deterministic) + emits +
// compiles + records. RANK BRIDGE: a `tensor.reshape` that preserves the element count is a zero-copy plan ALIAS (gemm [M,N] →
// [M·N] → fft), never a stage. crd-ceir-gpu stays backend-free (no crd::gpu here — the plan is data; the executor names the RHI).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gpu/ckir_synth.hpp> // SynthReject — the per-stage synthesizability check the plan mirrors
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp> // CEIR-29c-1: the expand→source-ml lineage the partitioned planner reads
#include <crd/core/types.hpp>

namespace crd::ceir::gpu
{
// Why a module could not be planned (TYPED — the plan NEVER silently drops or reorders a stage). ⛔ append at END.
enum class PlanReject : crd::u8
{
    None = 0,
    NotVerifyClean, // find_linalg_misuse / find_tensor_misuse / find_dispatch_misuse flagged the module (the verify-first contract)
    UnsupportedOp,  // an op is outside the pipeline vocab (linalg.gemm / tensor.fft / tensor.reduce / tensor.reshape / tensor.transpose /
                    // tensor.broadcast / tensor.elementwise / compute.dispatch / arith.const)
    SynthRejected,  // ckir_synth TYPED-REJECTED a stage (the offending PlanStage carries the SynthReject; e.g. non-F32, non-pow2 fft)
    ReshapeNotAlias, // a tensor.reshape whose total element count is NOT preserved (not a zero-copy alias — a real re-layout is name-forward)
    DanglingOperand, // a stage operand is neither a prior stage's result nor a module-declared external (resource.declare)
    NoOutput,        // the module has no terminal result (an empty / non-producing pipeline)
    DispatchOutputsNotTrailing, // a compute.dispatch whose `w` (write) bindings are not a CONTIGUOUS TRAILING run — the executor's
                                // "outputs = the last n_out binds" barrier contract needs writes last (author inputs then outputs)
    UnsupportedQuantScheme,     // a quant.dequantize that is NOT symmetric-per-tensor (asymmetric zp, or a rank-1 per-axis scale)
                                // — the plan-path Q8 kernels are symmetric-per-tensor only (read scale[0], drop zp). Name-forward
                                // (an asymmetric/per-axis plan path is a future slice); ⛔ a TYPED reject, never a silent miscompile.
    TuneCacheLockedMiss,        // CEIR-28c: plan_tensor_pipeline_cached under TunePolicy::Locked found NO cache row for this
                                // (device, env, program_hash, shape) — a LOCKED build refuses to silently fall back to the default
                                // schedule (cook-time certainty: ship NOTHING unmeasured; tune+commit a row first). ⛔ reject_op =
                                // nullptr — no op is at fault; the miss is a KEY property of the cache, not a module property.
    PartitionLineageMismatch,   // CEIR-29c-1: plan_tensor_pipeline_partitioned found a lineage index >= partition.assignments.size()
                                // — expand_ml_ops and partition_ml disagree on the ml-op pre-order (a walk-order drift, or a partition
                                // built on a DIFFERENT module than the one expanded). ⛔ a LOUD typed reject, never a silent -1 that
                                // would masquerade as "the fallback claimed more stages". reject_op = nullptr — a KEY-property
                                // mismatch between two inputs, not an op fault (the TuneCacheLockedMiss mold).
};
[[nodiscard]] containers::StringView plan_reject_name(PlanReject r) noexcept;

// A dispatch stage's kind — the per-op-kind binding/grid contract the executor honors. ⛔ reshape is NOT here (it is a buffer
// ALIAS, never a dispatched stage). ⛔ append at END.
enum class StageKind : crd::u8
{
    Gemm,        // linalg.gemm → a graph-tier Contract (emit_contract_glsl); binds A,B [,C] in + D out
    Fft,         // tensor.fft → a kernel-tier radix-2 plan; binds in_re,in_im,tw_re,tw_im + out_re,out_im (the 22b 6-buffer contract)
    Reduce,      // tensor.reduce → a graph-tier reduce (emit_reduce_glsl); binds in + out
    VizDispatch, // compute.dispatch of the AUTHORED viz .ckir (magnitude/normalize) — the §137 "mixed high-level tensor + CKIR" stage
    Dequant,     // quant.dequantize → the AUTHORED Q8 dequant .ckir (CEIR-23b); binds W_q8(int8, u32-packed device view), scale, zp + out
    QuantGemm,   // ⭐ the FUSED dequantize→gemm collapse (CEIR-23b-2b): a quant.dequantize whose result is SINGLE-USE by a gemm's
                 // weight (operand-1) is folded away (its output NEVER allocated); binds {A, W_q8(int8), scale, D} — the §54 fusion
                 // win. ⛔ n_out=1 (D trailing). Symmetric (zp≡0). op = the GEMM; the resolver RE-DERIVES W_q8/scale via
                 // fusable_dequant_into_gemm_weight (the ONE shared predicate — never a hand copy).
    Transpose,   // CEIR-25b-3 (autodiff backward vocab): tensor.transpose → a graph-tier CKIR Permute (emit_permute_glsl/hlsl);
                 // binds in + out. n_out=1. The resolver re-synthesizes via synth_transpose (baked static-shape index map).
    Broadcast,   // CEIR-25b-3: tensor.broadcast → a graph-tier CKIR Broadcast (emit_broadcast_nd_glsl/hlsl); binds in + out. n_out=1.
    Elementwise, // CEIR-25b-3: tensor.elementwise → a graph-tier CKIR binary (the fused-elementwise emitter); binds a, b + out.
                 // n_out=1. The one adjoint-accumulation stage (fn=add) of the reverse pass; resolver re-synthesizes via synth_elementwise.
    GemmRelu,    // ⭐ the FUSED gemm→relu epilogue collapse (CEIR-26e): a plain f32 gemm whose result is SINGLE-USE by a
                 // compute.dispatch(@relu) is folded into ONE stage — the intermediate z buffer is NEVER allocated + the relu
                 // dispatch is skipped. binds {A, B, out} (out = the relu's WRITE target h). ⛔ n_out=1 (out trailing). op = the
                 // GEMM; the resolver re-synthesizes via synth_gemm(op, GemmEpilogue::Relu) + emit_contract (the Max(Contract,0)
                 // unwrap). ⛔ NOT for a QuantGemm-weight gemm (fusable_gemm_into_relu excludes it — that triple-fuse is name-forward).
};

// A buffer's role in the device-resident plan — the §137 memory-plan visibility. ⛔ append at END.
enum class BufferRole : crd::u8
{
    ExternalIn,   // uploaded by the caller before the run (gemm A/B/C, the fft twiddle table, the zero-imaginary buffer)
    Intermediate, // a GpuOnly buffer produced by one stage + consumed by a later one — NEVER round-trips host (the §137 no-CPU-round-trip)
    Output,       // the terminal result, read back ONCE at the end
    Alias,        // a zero-copy view of another buffer (a reshape) — `alias_of` names the realized buffer; no allocation
};

// What an ExternalIn buffer must be seeded with (the executor uploads accordingly — never assume create_buffer zero-inits).
enum class FillKind : crd::u8
{
    CallerData, // the caller supplies the bytes (gemm A/B/C — the test/app's tensor data)
    Zeros,      // uploaded as zeros (the fft split-complex imaginary input of a real signal)
    FftTwiddle, // W_N^k = (cos(2πk/N), -sin(2πk/N)), k in [0,N/2) — the executor computes N/2 entries (the 22b fft contract)
};

// One buffer of the plan (the def-use "same buffer" key is `value`). ROLE + `bytes` + (for ExternalIn) `fill` are §137-visible.
struct PlanBuffer
{
    const Value* value    = nullptr;                  // the SSA Value this buffer realizes (nullptr for a synthesized side buffer, e.g. fft im/tw)
    BufferRole   role     = BufferRole::Intermediate;
    crd::u64     bytes    = 0;
    crd::i32     alias_of = -1;                        // the index of the realized buffer whose STORAGE this shares (-1 = its own). TWO
                                                       // senses: role==Alias ⇒ a reshape zero-copy VIEW (same value, bytes copied); role==
                                                       // Intermediate ⇒ a ⭐ 26f storage TENANT (a DIFFERENT value, disjoint lifetime, own
                                                       // bytes). The runner skips allocation when alias_of>=0 REGARDLESS of role, pointing
                                                       // bufs[i]=bufs[alias_of]. Always the LANDLORD (a realized buffer, alias_of<i), never a tenant.
    FillKind     fill     = FillKind::CallerData;      // ExternalIn only (meaningless otherwise)
};

// One dispatched stage. `bind[0..nbind)` are indices into the plan's buffer array in the stage's per-kind operand order
// (inputs then outputs — the 13a positional-slot rule the emitters + eval_cpu_kernel share). The grid + push blob are
// EXECUTE-time (the emitter's local_size/tiling is backend-side); the plan carries the op* + shapes the executor derives them from.
struct PlanStage
{
    const Operation* op           = nullptr;
    StageKind        kind         = StageKind::Gemm;
    crd::i32         bind[8]      = {-1, -1, -1, -1, -1, -1, -1, -1};
    crd::u32         nbind        = 0;
    crd::u32         n_out        = 0; // the LAST n_out binds are this stage's WRITTEN outputs (gemm 1 / fft 2 / reduce 1) — the
                                       // executor emits a ShaderWrite→ShaderRead barrier on each after the stage (before the next reads it)
    SynthReject      synth_reject = SynthReject::None; // None on a planned stage; set (+ PlanReject::SynthRejected) when a stage rejects
    // CEIR-29c-1 §102 — which PROVIDER owns this stage: the index into the partition's providers that claimed the ml op this
    // stage was expanded from, or -1 = the CKIR fallback (no provider). ⛔ SET ONLY by plan_tensor_pipeline_partitioned (via
    // the expand→ml-op lineage); the partition-BLIND plan_tensor_pipeline leaves it -1 (a launch-graph run = the contiguous
    // stages sharing one provider — 29c-2 captures exactly that range).
    crd::i32         provider     = -1;
};

// The plan: the ordered dispatch stages + the buffer graph. On a reject, `reject != None` + `reject_op` points at the offender
// (stages/buffers hold whatever was planned before the reject — inspectable). ⛔ the executor RE-synthesizes each stage's op
// (deterministic) to emit; the plan is a PURE description (no KGraph, no device).
struct TensorPipelinePlan
{
    PlanReject                    reject    = PlanReject::None;
    const Operation*              reject_op = nullptr;
    containers::Array<PlanBuffer> buffers;
    containers::Array<PlanStage>  stages;

    explicit TensorPipelinePlan(memory::IAllocator* alloc) : buffers(alloc), stages(alloc) {}
};

// Walk `m` (VERIFY-CLEAN first — find_linalg_misuse + find_tensor_misuse must be None, else PlanReject::NotVerifyClean), plan
// each supported op into a stage, and derive the device-resident buffer wiring from the SSA def-use edges + the fft
// 6-buffer/reshape-alias contracts. PURE + device-free (no synth KGraph kept, no GPU). ⛔ Context& (non-const — synthesizability
// checks may intern via the shape predicates). Returns the plan (its `reject` says None on success).
// ⭐ CEIR-23b-2b — the ONE shared FUSION predicate: is `dequant_op` (a quant.dequantize) fusable into a following gemm's WEIGHT
// operand? True iff its result has EXACTLY ONE use AND that use is a linalg.gemm's operand-1 (B/weight). ⛔ Called by the PLAN
// (the dequant-skip + the gemm QuantGemm-detect) AND the executor's RESOLVER (the W_q8/scale re-derive) — three hand copies of
// this predicate WOULD drift and reject a legal module (the advisor's shared-helper mandate). const-safe (reads the def-use graph).
[[nodiscard]] bool fusable_dequant_into_gemm_weight(const Context& ctx, const Operation* dequant_op) noexcept;

// ⭐ CEIR-26e — the ONE shared gemm→relu FUSION predicate: is `gemm_op` (a linalg.gemm) fusable into a following relu epilogue?
// TRUE iff (cheapest-first) it IS a linalg.gemm ∧ `gemm_is_plain` (α=1 β=0 no-transpose — a scaled/accumulating gemm has no fused
// form) ∧ ⛔⛔ its WEIGHT (operand-1) is NOT itself a fusable quant.dequantize (else the gemm is a QuantGemm target — the quant MLP
// `dequant→gemm→relu` is BOTH; fusing here would bind the never-allocated dequant f32 result → DanglingOperand; a triple-fuse is
// name-forward) ∧ its result has EXACTLY ONE use, a `compute.dispatch{kernel=="relu"}` whose bind[0] (operand-3, the READ) IS the
// gemm result with exactly the {grid×3, r, w} arity (5 operands). ⛔ Called by the PLAN at BOTH the gemm (skip) AND the relu
// dispatch (emit GemmRelu) — one predicate, never a hand copy (the fusable_dequant shared-helper mold). const-safe (reads def-use).
[[nodiscard]] bool fusable_gemm_into_relu(const Context& ctx, const Operation* gemm_op) noexcept;

// ⭐ CEIR-26e — the plan's SCHEDULE knobs (the cook-time fusion decisions). ⛔ ONE field for now — this is the 26f/27 "fusion IS a
// schedule decision, an authorable asset" SEED; do NOT pre-populate speculatively (the 26b-4 no-live-consumer rule). A plan-time
// flag (not a build flag) so the SAME module can be planned BOTH ways — the raw-vs-opt bit-exact differential (26e-3) needs it.
struct PlanOptions
{
    bool fuse_gemm_relu           = true; // fold a plain f32 gemm → single-use @relu dispatch into ONE StageKind::GemmRelu stage.
    bool share_intermediate_storage = true; // ⭐ 26f: disjoint-lifetime Intermediates SHARE physical storage (a tenant's alias_of =
                                            // the landlord; the runner skips its allocation). The free-list buffer-aliasing pass. Default ON.
};

[[nodiscard]] TensorPipelinePlan plan_tensor_pipeline(Context& ctx, const Module& m, memory::IAllocator* alloc, PlanOptions opts = {});

// ⭐ CEIR-27a — read an authored `ceir.transform` SCHEDULE module into PlanOptions (the seed above hoisted into an
// authored asset). Each directive op sets ONE program-global knob: transform.fuse -> fuse_gemm_relu,
// transform.share_storage -> share_intermediate_storage. An ABSENT directive keeps `base`'s value (a partial schedule is
// legal); an unknown op is ignored this slice (per-op targeting + a stricter schedule-verifier are the sec-71 named-forward).
// The transform module is a SEPARATE authored asset -- the payload program is planned with the RESULT, so two schedules
// optimize ONE semantic program (sec-71/sec-146). ⛔ a directive's `enable` is read defensively (valid + Bool kind) so an
// unverified module never mis-reads (the attr-reader-checks-valid rule); the op verifier still enforces it as required.
[[nodiscard]] PlanOptions plan_options_from_transform(Context& ctx, const Module& transform_mod, PlanOptions base = {});

// ⭐ CEIR-28a — read a TARGET-SPECIFIC CONFIG CACHE (`ceir.tune`) into PlanOptions: find the tune.entry ROW whose FULL KEY
// (device, env, program_hash, shape) matches the query and return its schedule (fuse, share) as PlanOptions. A HIT sets
// `hit=true` + `opts` from the row; a MISS returns `hit=false` + `base` UNCHANGED -- the caller (deterministic locked mode, 28c)
// chooses fallback-to-default vs a typed reject. This is the sec-80 "target-specific configuration cache" REPLAY path (v17's
// select_schedule, made portable): tune offline, replay at plan time, NEVER measure here. ⛔ the FULL four-attr key is matched
// (consistent with find_tune_misuse's DuplicateKey = all-four-equal, so the first match is unambiguous on a clean cache); env
// is a real match dimension (it bundles OS/driver -- sec-80's driver/compiler-version keying), NOT provenance-only. The cache
// module is a SEPARATE authored asset (assumes find_tune_misuse clean).
struct TuneCacheLookup
{
    bool        hit = false;
    PlanOptions opts{};
};
[[nodiscard]] TuneCacheLookup plan_options_from_tune_cache(Context& ctx, const Module& cache_mod, containers::StringView device,
                                                           containers::StringView env, u64 program_hash,
                                                           containers::StringView shape, PlanOptions base = {});

// ⭐ CEIR-28c — the DETERMINISTIC selection POLICY on a cache MISS (the sec-80 "deterministic locked configuration mode"). ⛔ append
// at END (a closed enum). ⛔ CEIR-28z-1 (advisor reversal of a 28c line): a THIRD "Require" policy is NAME-FORWARD, NOT built — the
// only Require semantic with a real consumer ("HIT but the planner IGNORED the row's opts → reject") needs a caller that must
// distinguish 'cache missing' from 'cache present but not honored', and none exists; the no-speculative rule wins over filling a
// reserved slot. The trigger: such a caller appears (then Require appends here + a `tune_row_applied` plan bit).
enum class TunePolicy : crd::u8
{
    Fallback = 0, // MISS → plan with `base` (the default schedule) and DO NOT measure. The permissive default (an untuned device runs correct-but-unoptimized).
    Locked,       // MISS → a TYPED reject (PlanReject::TuneCacheLockedMiss): a locked build ships NOTHING unmeasured (cook-time certainty).
};

// ⭐ CEIR-28c — plan `m` using the schedule the config cache selects for THIS target (the sec-80 policy wrapping the 28a loader). The
// key is (device, env, program_hash, shape); ⛔ program_hash is computed HERE from `m` (the POST-expansion payload the planner
// consumes) — the SAME producer the 28b measurer emits, so the plan's key can NEVER drift from the plan. HIT → plan with the row's
// {fuse, share}; MISS → `policy` decides (Fallback: plan with `base`, no measure; Locked: an EMPTY plan with reject
// TuneCacheLockedMiss). ⛔ NEVER measures (tune offline, replay at plan time — the v17 discipline, portable). PURE + device-free.
[[nodiscard]] TensorPipelinePlan plan_tensor_pipeline_cached(Context& ctx, const Module& m, memory::IAllocator* alloc,
                                                             const Module& cache_mod, containers::StringView device,
                                                             containers::StringView env, containers::StringView shape,
                                                             TunePolicy policy, PlanOptions base = {});

struct MlPartition; // CEIR-29c-1: the §69 partition (partition_ml.hpp) — a reference param, forward-declared to avoid coupling

// ⭐ CEIR-29c-1 §102 — plan `m` (the SAME plan plan_tensor_pipeline produces) and TAG each stage with the PROVIDER that claimed
// the ml op it was expanded from. `lineage` (from `expand_ml_ops(ctx, m, lineage)`) maps an expanded op → its source ml op's
// pre-order index, which indexes `partition.assignments`; a stage whose op is absent (a non-ml op, or the fallback) gets
// provider -1. ⛔ METADATA-ONLY: the stages/buffers/aliases are byte-identical to plan_tensor_pipeline — only PlanStage.provider
// is set. So a launch-graph provider's claimed run is the contiguous stage range sharing its index (29c-2 captures exactly it).
[[nodiscard]] TensorPipelinePlan plan_tensor_pipeline_partitioned(Context& ctx, const Module& m, memory::IAllocator* alloc,
                                                                  const MlPartition&                              partition,
                                                                  const containers::HashMap<const Operation*, crd::i32>& lineage,
                                                                  PlanOptions opts = {});

// ⭐ CEIR-26d — COOK-TIME KERNEL-SHAPE SPECIALIZATION (M2, the "specialize" pass in the §73 sense — a cook-time bind at the
// CEIR→CKIR boundary, NOT a CEIR IR rewrite: the shape is already in the operand types). The PURE numeric POLICY a VizDispatch
// resolver applies to an authored kernel's workgroup width AFTER ckir_read + BEFORE emit: if `local_size_x` is the SENTINEL 0
// ("bind at cook", asset-declared per kernel — relu.ckir/relu_vjp.ckir/softmax.ckir), set it to the resolver-supplied EXTENT
// `extent`, capped at `max_local_size` (the device workgroup limit, known at resolve = cook). ⛔ the EXTENT is per-kernel, chosen by
// the resolver's symbol switch (the 20b binding-table): elementwise kernels (relu/relu_vjp) supply the trailing-write operand's
// NUMEL (one lane per element); the row-wise softmax supplies Sq = dim0(probs) (one lane per ROW, the loop covers Sk). A NON-sentinel
// local_size is left as authored (fft's n/2, the viz kernels — untouched). ⛔ this is where an ml.mlp/attention of ANY shape ≤ cap
// becomes runnable (the 24z BakedKernelShapeUnsupported reject retired). Pure u32/u64 ⇒ device-free unit-testable.
enum class KernelShapeError : crd::u8
{
    None,                  // bound (or a non-sentinel authored size left as-is)
    LocalSizeUnbound,      // sentinel 0 but extent == 0 (no resolvable extent) — NEVER silently 1
    LocalSizeExceedsLimit, // extent > max_local_size — a single workgroup cannot cover it (multi-WG is name-forward)
};
// The portable single-workgroup width cap: 1024 is the guaranteed floor for BOTH Vulkan (maxComputeWorkGroupSize[0] ≥ 1024) and
// D3D12 (max threads/group = 1024), so it is the CORRECT max any conformant device honors — NOT a level-down. A true per-device
// query (res.compute) is a name-forward refinement.
inline constexpr crd::u32 kMaxAuthoredLocalSize = 1024U;
[[nodiscard]] inline KernelShapeError bind_authored_local_size(crd::u32& local_size_x, crd::u64 extent,
                                                               crd::u32 max_local_size) noexcept
{
    if (local_size_x != 0U) { return KernelShapeError::None; }        // authored fixed size — the asset drives it
    if (extent == 0U) { return KernelShapeError::LocalSizeUnbound; }
    if (extent > static_cast<crd::u64>(max_local_size)) { return KernelShapeError::LocalSizeExceedsLimit; }
    local_size_x = static_cast<crd::u32>(extent);
    return KernelShapeError::None;
}
} // namespace crd::ceir::gpu
