#pragma once

// crd-ceir-gpu — CEIR-25 ceir.autodiff: reverse-mode differentiation as a COMPILER TRANSFORM over the HIGH-LEVEL tensor
// vocab (§57). `build_gradient(ctx, m, reg, loss, wrt)` differentiates linalg.gemm / tensor.reduce(sum) / ml.mlp BEFORE
// expansion, emitting backward ops in the EXISTING OP vocab (tensor.transpose + linalg.gemm + tensor.elementwise/broadcast).
// ⛔ CORRECTED 2026-09-04 (25z, source=scoreboard): the original "ZERO new StageKinds / store is free" claim did NOT ship as
// stated — (1) tensor.transpose was NOT plannable pre-25b (only expand_ml's baked dispatch), so CEIR-25b-3 APPENDED planner
// StageKind::{Transpose,Broadcast,Elementwise} (no new OP-kinds; three new planner stages); (2) "store is free" holds for the
// tensor-vocab VJPs (the planner materializes forward SSA intermediates as buffers) but the ml.mlp COMPOSITE has no SSA interior
// and RECOMPUTES it (25c-3 = full gradient checkpointing). A VJP rule is looked up by OP-KIND. ⛔ SUPERSEDED 2026-09-04 (CEIR-25c-0, resultless-dispatch finding): the earlier clause "an authored
// kernel becomes differentiable by DECLARING its VJP-pair (relu.ckir <-> relu_vjp.ckir) via a compute.dispatch kernel symbol"
// does NOT hold — the MLP expansion emits relu as a RESULTLESS compute.dispatch, and build_gradient's reverse walk keys on
// `num_results` (a resultless op is never visited), so a kernel-symbol VJP rule has NO caller. Instead a COMPOSITE op (ml.mlp)
// differentiates via `register_op` (vjp_mlp): the rule reconstructs the interior activations and emits the authored
// relu_vjp.ckir as a compute.dispatch DIRECTLY. `register_kernel` is retained as API (correct machinery) but has no built-in
// caller; differentiating an already-EXPANDED module (resultless dispatches) is ledgered name-forward.
// An op on the backward reachability set with NO registered rule is a TYPED `MissingVjp` reject
// (unregistered op != zero gradient — the registered-default-empty scar in AD form), NEVER a silent zero.
// ⛔ DEVICE-FREE: a pure Context/Module rewrite. Oracles: hesap-autodiff nn_reverse (analytic) + gradient_check (FD).

#include <crd/ceir/context.hpp>
#include <crd/ceir/id.hpp>

#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::gpu
{
// Why differentiation was REJECTED — the TYPED reject (the transform NEVER silently drops a gradient). ⛔ append at END.
enum class GradError : crd::u8
{
    None = 0,
    LossNotScalar,    // ⛔ DEAD 2026-09-04 (25z-1, source=scoreboard): NEVER returned (grep grad.cpp: only in grad_error_name).
                      //    The "loss must be rank-0 scalar" contract was NEVER enforced and is WRONG as shipped — loss may be ANY
                      //    rank (25b-4b loss=[3]; 25c loss=[8,4]), because the seed carries ∂L/∂loss for the caller's IMPLICIT
                      //    scalar objective L (below). Member KEPT (append-only enum; grad_error_name stays exhaustive), not live.
    WrtNotTensor,     // a requested-gradient (`wrt`) value is not a Tensor
    OperandNotTensor, // a differentiated op's operand/result is not Tensor-kinded (a non-verify-clean module)
    ElementNotFloat,  // a differentiated tensor's element is not Float (autodiff is float-only this band; name-forward)
    ShapeRankInvalid, // an operand is not the rank the VJP rule requires (gemm operands + result must be rank-2)
    MissingVjp,       // ⛔ an op on the backward reachability set has NO registered VJP rule — a TYPED reject, never a
                      //    silent zero-gradient (negative-gated in the SAME slice — the registered-default-empty scar)
    ReduceFnUnsupported, // reduce VJP: `fn` is not `sum` (max/min/prod/mean VJPs are name-forward — a TYPED reject, not a
                         //    silent wrong gradient); the registry keys on op-kind so vjp_reduce owns every tensor.reduce
    ArityUnsupported,    // an on-path op exceeds the reverse-walk's fixed operand/result caps (variadic VJPs = name-forward) —
                         //    raised in the PURE pre-pass (nothing emitted), never a silent adjoint truncation
    MlpActivationUnsupported, // ml.mlp VJP: `activation` is not `relu` (the authored relu_vjp.ckir pair is relu-only this band;
                              //    other activations are name-forward — a TYPED reject, never a silent wrong composite gradient)
    MlpBakedShapeUnsupported, // ⛔ DEAD 2026-09-04 (26d-4b, source=scoreboard): NEVER returned (grep grad.cpp: only in
                              //    grad_error_name). The M·hidden==32 guard was RETIRED — relu.ckir/relu_vjp.ckir now ship the shape
                              //    SENTINEL (local_size=0) and the resolver cook-binds local_size to the intermediate numel, so a vjp
                              //    at ANY interior width runs device-resident (proven h1=64 both backends, 26d-4b/4c). Member KEPT
                              //    (append-only enum; grad_error_name stays exhaustive), not live — exactly like LossNotScalar.
};
[[nodiscard]] containers::StringView grad_error_name(GradError e) noexcept;

class VjpRegistry; // forward — a VJP rule may recursively differentiate sub-ops through the registry

// A VJP rule: given a forward op + the adjoints of its RESULTS, emit backward ops (inserted before `at` in `blk`) that
// compute the adjoints of its OPERANDS, writing each into `operand_adjoints` (size == fwd->num_operands(); left nullptr
// for a non-differentiable operand — gemm's beta*C term, an Index grid, an integer). Returns a typed error or None.
// ⛔ A rule reads every forward value it needs through `fwd` (operand/result SSA) or `result_adjoints` — NEVER captured
// state (§57: backward ops REFERENCE forward SSA values; this contract is why a raw fn-ptr suffices). The orchestrator
// PRE-SIZES `operand_adjoints` to fwd->num_operands() and zero-inits it; a rule writes only slots [0, num_operands).
using VjpRuleFn = GradError (*)(Context& ctx, const VjpRegistry& reg, Block* blk, Operation* at, const Operation* fwd,
                                containers::ConstSpan<Value*> result_adjoints, Value** operand_adjoints);

// The VJP registry (§6/§7 open-world: dispatch on a REGISTERED rule, never switch(op.kind)). Two key spaces: plain op
// kinds (linalg.gemm, tensor.reduce, ml.mlp) and compute.dispatch kernel symbols. `lookup` resolves a compute.dispatch
// op by its `kernel` symbol and every other op by its kind; nullptr ⇒ the caller raises MissingVjp. ⛔ the kernel-symbol
// space is MACHINERY with NO built-in registrant this band (25c-0: a resultless dispatch is invisible to the result-keyed
// reverse walk, so register_kernel has no caller — see the SUPERSEDED clause above); retained for a future
// differentiate-already-expanded path.
class VjpRegistry
{
public:
    explicit VjpRegistry(memory::IAllocator* alloc);

    void register_op(OpId op, VjpRuleFn fn);                       // a kernel-agnostic op-kind rule
    void register_kernel(containers::StringView kernel, VjpRuleFn fn); // a compute.dispatch(kernel=@k) rule

    // ⛔ compute.dispatch resolves ONLY through the kernel map (by its `kernel` symbol); an absent/non-SymbolRef kernel
    //    attr ⇒ nullptr (→ MissingVjp) — NEVER a fall-through to an op-kind rule (a generic "dispatch VJP" cannot exist,
    //    so `register_op` is never called for compute.dispatch). Every other op resolves by kind. nullptr ⇒ MissingVjp.
    [[nodiscard]] VjpRuleFn lookup(const Context& ctx, const Operation* op) const noexcept;

private:
    containers::HashMap<crd::u64, VjpRuleFn> m_op_rules;     // keyed by OpId.value (FNV of "dialect.op")
    containers::HashMap<crd::u64, VjpRuleFn> m_kernel_rules; // keyed by FNV of the kernel symbol name
};

// Register the CEIR-25 built-in VJP rules into `reg` (25a-1: linalg.gemm; 25a-2: tensor.reduce(sum); 25c: ml.mlp) — all
// OP-KIND rules via register_op. ⛔ NO authored-kernel pair is register_kernel'd (25c-0: register_kernel has no built-in
// caller). Interns the op kinds it registers against `ctx`.
void register_builtin_vjps(VjpRegistry& reg, Context& ctx);

// The pointing result of build_gradient: the FIRST error + the offending op, or {None}. On None, the caller's `grads`
// span carries the emitted gradient Value per `wrt` (parallel).
struct GradResult
{
    GradError        error    = GradError::None;
    const Operation* error_op = nullptr;
    Value*           seed     = nullptr; // the dLoss ExternalIn (a resource.declare of loss's shape) the CALLER uploads ∂L/∂loss
                                         // into — the seed of the CALLER'S implicit scalar objective L over `loss` (loss is ANY rank,
                                         // NOT required scalar): ALL-ONES ⇔ L=sum(loss) (25a-2/25b-4b); a WEIGHTED seed M ⇔ L=⟨M,loss⟩
                                         // (25c). ("d loss/d loss=1" holds only at rank-0 — corrected 25z-1.) The "caller-provided
                                         // seed" promise WITH a handle: an executor names THIS buffer to seed the backward pass (the
                                         // plan realizes it ExternalIn; the reduce-VJP reshape aliases it). Non-null once emission begins.
};

// Differentiate module `m`: reverse-mode over the ops on the backward reachability set of `loss` wrt `wrt`, emitting
// backward ops in the existing vocab. `loss` may be ANY rank (NOT required scalar). The seed adjoint rides as a caller-provided
// ExternalIn (a resource.declare the CALLER uploads ∂L/∂loss into — the caller's implicit scalar objective L: ALL-ONES ⇔
// L=sum(loss), a WEIGHTED seed ⇔ L=⟨seed,loss⟩; "=1" only at rank-0, corrected 25z-1; the quant/softmax-scale precedent). `grads` (out; size ==
// `wrt.size()`) receives the gradient Value for each wrt (null ⇒ that wrt does not reach loss ⇒ zero gradient) — the span
// carries its own size so the contract cannot drift. A forward value with TWO consumers gets two partial adjoints, SUMMED
// via `tensor.elementwise{fn=add}` (the one place the existing-vocab claim could break — 25a-2's gate proves it).
// ⛔ `MissingVjp` / `ArityUnsupported` are PURE rejects — a dry pre-pass checks every on-path op BEFORE emitting anything,
// so a rejected module is byte-identical to the input. A RULE-level reject (e.g. `ReduceFnUnsupported`) may leave partial
// backward ops behind, so DISCARD the module on any non-None error. ⛔ Context& (builds ops); `scratch` backs the
// reverse-walk snapshot + the value->adjoint map (freed by the caller).
// ⛔ FORWARD-LIVENESS CONTRACT (25c-1b-2): a SUCCESSFUL differentiation only APPENDS the backward ops — it does NOT expand or
// remove the forward op(s) it differentiated. The `loss`-producing op is left intact (its result may still be wanted — a training
// loop reads BOTH loss and grads). A caller planning the BACKWARD ALONE must ERASE a now-dead forward COMPOSITE (e.g. `ml.mlp`,
// whose interior activations the rule RECOMPUTES so the backward never reads the composite's result) before `plan_tensor_pipeline`
// — the planner TYPED-REJECTS a non-plannable forward op (`UnsupportedOp`). Erase is safe iff the forward result has no remaining
// users (assert `!loss->has_uses()` first). Engine-side DCE of a dead differentiated op is ledgered CEIR-26 (a real DCE pass, not
// a special case in this transform — the forward op's liveness is the CALLER's decision).
// ⛔ grads[] are NOT SSA-consumed by this transform — they are readback-by-Value (the plan marks the terminal write `Output`).
//    A caller that runs ANY pass (DCE, CSE, ...) BEFORE planning MUST first pin grads[] in the IR (a `func.return`, or a store),
//    or DCE (correctly) prunes the gradients as dead Pure ops. The transform does not pin — it must not decide the gradients'
//    consumer (the same forward-liveness contract). See CEIR-26a (dce.hpp LIVENESS CONTRACT + the 26a-2 consumer gate).
[[nodiscard]] GradResult build_gradient(Context& ctx, Module& m, const VjpRegistry& reg, Value* loss,
                                        containers::ConstSpan<Value*> wrt, containers::Span<Value*> grads,
                                        memory::IAllocator* scratch);

// The linalg.gemm VJP rule (25a-1), exposed for the standalone gate. For C = A*B (A[M,K], B[K,N], C[M,N]) with output
// adjoint dC[M,N]: dA = gemm(dC, Bᵀ) [M,K], dB = gemm(Aᵀ, dC) [K,N] — bit-for-bit hesap nn_reverse::matmul_vjp. Emits
// two tensor.transpose (perm=[1,0]) + two plain linalg.gemm in the existing vocab. operand_adjoints[2] (beta*C) = null.
[[nodiscard]] GradError vjp_gemm(Context& ctx, const VjpRegistry& reg, Block* blk, Operation* at, const Operation* fwd,
                                 containers::ConstSpan<Value*> result_adjoints, Value** operand_adjoints);

// The tensor.reduce(sum) VJP rule (25a-2), exposed for the standalone gate. For output = reduce(input, axis, sum) with
// output adjoint dOut: dInput = broadcast(reshape(dOut, keepdim)) back to input's shape — sum's derivative is 1, so the
// gradient BROADCASTS along the reduced axis (reshape re-inserts the size-1 at `axis` to dodge the 1D right-align scar).
// ⛔ `fn` != sum ⇒ ReduceFnUnsupported (max/min/prod/mean VJPs are name-forward). Emits tensor.reshape + tensor.broadcast.
[[nodiscard]] GradError vjp_reduce(Context& ctx, const VjpRegistry& reg, Block* blk, Operation* at, const Operation* fwd,
                                   containers::ConstSpan<Value*> result_adjoints, Value** operand_adjoints);

// The ml.mlp VJP rule (25c-1), exposed for the standalone gate. ml.mlp(x, W_1..W_n){activation=relu} → h_n. Given the output
// adjoint dOut[M,N]: the rule RECONSTRUCTS the interior pre/post activations (z_i = gemm(h_{i-1}, W_i), h_i = relu(z_i) for
// i<n) — they are NOT SSA on the composite op, so the rule re-derives them (the checkpointing recompute; z_n = h_n is the live
// result, never re-derived). Backward (i=n..1): dW_i = gemm(h_{i-1}ᵀ, dz_i); dh_{i-1} = gemm(dz_i, W_iᵀ); dz_{i-1} =
// relu_vjp(z_{i-1}, dh_{i-1}) via a compute.dispatch(@relu_vjp) (i>1). operand_adjoints[0] = dx (the input adjoint, emitted
// LAST so grads[wrt=W_1] is the terminal op), [i] = dW_i. ⛔ `activation` != relu ⇒ MlpActivationUnsupported (TYPED reject).
[[nodiscard]] GradError vjp_mlp(Context& ctx, const VjpRegistry& reg, Block* blk, Operation* at, const Operation* fwd,
                                containers::ConstSpan<Value*> result_adjoints, Value** operand_adjoints);
} // namespace crd::ceir::gpu
