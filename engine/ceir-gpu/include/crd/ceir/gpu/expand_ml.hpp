#pragma once

// crd-ceir-gpu — the ceir.ml EXPANSION provider (CEIR-24b, §55/§69). `expand_ml_ops` REWRITES every ml.mlp / ml.attention op
// in a module into the PROVEN CEIR-22/23 tensor vocabulary (linalg.gemm + compute.dispatch of the authored transpose/softmax/relu
// .ckir kernels) — the module the EXISTING plan_tensor_pipeline consumes (ZERO new StageKinds). This is the §69 provider's EXPAND
// path (a provider EXPANDS a composite op into primitives OR CLAIMS it whole; the CLAIM path + the partitioner are 24c). Shaped as
// a PER-OP rewrite so 24c can call it per-claim.
//
//   ml.mlp(x, W_1..W_n) {activation=relu} -> h = x; for i: h_i = gemm(h_{i-1}, W_i); h = (i<n) ? relu(h_i) : h_i   (FLOAT-only —
//     the dialect requires Float+equal weights; the quantized MLP path is proven hand-authored at 23c and is name-forward here).
//   ml.attention(Q, K, V) -> Kt = transpose(K); scores = gemm(Q, Kt); probs = softmax(scores, scale=1/√D); out = gemm(probs, V).
//     ⛔ gemm stays PLAIN (trans_b AND α≠1 are BOTH typed-rejected by the synth/plan envelope) -> K is transposed by the authored
//     @transpose kernel and the 1/√D scale rides as a CALLER-UPLOADED 1-element buffer read by the authored @softmax kernel (the
//     quant_mlp.ceir dequant-scale precedent; a resource.declare = ExternalIn the caller fills).
//
// ⛔ DEVICE-FREE: a pure Context/Module rewrite (build replacement ops, RAUW the ml op's result, erase the ml op) — NO gpu-context.
// The @transpose/@softmax/@relu kernel SYMBOLS resolve to the committed .ckir assets at RECORD time via the caller's StageResolveFn
// (execute_tensor_pipeline), exactly like the §137 viz kernels. Precondition: the module is find_ml_misuse-clean (verify-clean);
// after expansion find_ml_misuse == None (no ml ops remain) and the module plans through plan_tensor_pipeline.

#include <crd/ceir/context.hpp>
#include <crd/ceir/id.hpp>

namespace crd::ceir::gpu
{
enum class MlExpandError : crd::u8
{
    None = 0,
    OperandNotTensor, // an ml op operand / result is not Tensor-kinded (a non-verify-clean module)
    ElementNotFloat,  // an ml op's element is not Float (ml is float-only this band; quantized MLP = name-forward)
    ShapeRankInvalid, // an ml op operand / result is not rank-2 (the width/shape reads would be out of range)
    BakedKernelShapeUnsupported, // ⛔ a fixed-size authored kernel's BAKED dims don't match this op (relu.ckir local_size=32 ⇒
                                 //    every mlp relu'd intermediate must be M·hidden==32; transpose.ckir/softmax.ckir bake
                                 //    Sk==3 ∧ D==4 ∧ Sq==2). A TYPED reject — never a silent OOB / uninitialized-tail miscompile
                                 //    (the UnsupportedQuantScheme precedent). Dimension-general kernels = name-forward (24z ledger).
};

// The outcome: how many ml ops were expanded + the FIRST error (with the offending op), or {None}.
struct MlExpandResult
{
    crd::u32         expanded = 0;
    MlExpandError    error    = MlExpandError::None;
    const Operation* error_op = nullptr;
};

// Expand ONE ml.mlp / ml.attention op IN PLACE into the CEIR-22/23 tensor vocabulary (build replacement ops, RAUW the op's
// result, erase the op). `op` MUST be an ml.mlp or ml.attention op (else MlExpandError::OperandNotTensor, a no-op). This is the
// §69 apply_partition per-op entry — the partitioner expands only the ops it assigns to the CKIR fallback. ⛔ Context& (NOT const).
[[nodiscard]] MlExpandError expand_ml_op(Context& ctx, Operation* op);

// Expand EVERY ml.mlp / ml.attention op in module `m` into the CEIR-22/23 tensor vocabulary (in place). Idempotent on an
// ml-free module (expands 0). ⛔ Context& (NOT const) — builds ops + interns types/attrs; the rewrite RAUWs + erases each ml op.
[[nodiscard]] MlExpandResult expand_ml_ops(Context& ctx, Module& m);
} // namespace crd::ceir::gpu
