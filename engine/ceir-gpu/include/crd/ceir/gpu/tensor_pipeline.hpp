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
#include <crd/core/types.hpp>

namespace crd::ceir::gpu
{
// Why a module could not be planned (TYPED — the plan NEVER silently drops or reorders a stage). ⛔ append at END.
enum class PlanReject : crd::u8
{
    None = 0,
    NotVerifyClean, // find_linalg_misuse / find_tensor_misuse / find_dispatch_misuse flagged the module (the verify-first contract)
    UnsupportedOp,  // an op is outside the pipeline vocab (linalg.gemm / tensor.fft / tensor.reduce / tensor.reshape / compute.dispatch / arith.const)
    SynthRejected,  // ckir_synth TYPED-REJECTED a stage (the offending PlanStage carries the SynthReject; e.g. non-F32, non-pow2 fft)
    ReshapeNotAlias, // a tensor.reshape whose total element count is NOT preserved (not a zero-copy alias — a real re-layout is name-forward)
    DanglingOperand, // a stage operand is neither a prior stage's result nor a module-declared external (resource.declare)
    NoOutput,        // the module has no terminal result (an empty / non-producing pipeline)
    DispatchOutputsNotTrailing, // a compute.dispatch whose `w` (write) bindings are not a CONTIGUOUS TRAILING run — the executor's
                                // "outputs = the last n_out binds" barrier contract needs writes last (author inputs then outputs)
    UnsupportedQuantScheme,     // a quant.dequantize that is NOT symmetric-per-tensor (asymmetric zp, or a rank-1 per-axis scale)
                                // — the plan-path Q8 kernels are symmetric-per-tensor only (read scale[0], drop zp). Name-forward
                                // (an asymmetric/per-axis plan path is a future slice); ⛔ a TYPED reject, never a silent miscompile.
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
    crd::i32     alias_of = -1;                        // Alias: the index of the realized buffer this views (reshape); else -1
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

[[nodiscard]] TensorPipelinePlan plan_tensor_pipeline(Context& ctx, const Module& m, memory::IAllocator* alloc);
} // namespace crd::ceir::gpu
