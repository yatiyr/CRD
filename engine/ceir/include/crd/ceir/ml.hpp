#pragma once

// crd-ceir — the ml dialect's find_ml_misuse verifier (CEIR-24, §55). TWO PURE composite ops: ml.mlp (a multi-layer
// perceptron, x + a VARIADIC tail of per-layer weights) + ml.attention (single-head scaled dot-product attention),
// generated from ml.ceirop.toml, over the CEIR-3d Tensor TYPE (members = [element, shape]). ⛔ NO new element TypeKind — an
// MLP/attention is the COMPOSITION of dense rank-2 tensors through the op (the composite op is the §69 provider PARTITION
// UNIT — a provider EXPANDS it into 22/23 vocab OR CLAIMS it whole). The generated verify_mlp/verify_attention own STRUCTURAL
// conformance (mlp: >=1 operand [input] + a variadic tail, 1 result, `activation` present+String; attention: 3 operands, 1
// result); THIS owns the SHAPE-AWARE chain: operands/result Tensor-kinded + Float + EQUAL element, all rank-2, the mlp WIDTH
// CHAIN (input.dim1==W_1.dim0, W_i.dim1==W_{i+1}.dim0, output.dim1==W_n.dim1, .count arithmetic), the closed-vocab
// `activation`, and the attention Q·Kᵀ·V shape relations. ⛔ ELEMENT-AGNOSTIC: the F32-only restriction rides the 24b provider
// (typed SynthReject; the 22a precedent). ⛔ MODE-FREE (§56): no inference/training mode — autodiff/training is a §57 compiler
// transform at CEIR-25. ⛔ I6 — find_ml_misuse switches on op NAME + TypeKind, never op.kind. Declare-only: Pure + typed
// NoSemantics + NO kernel_ref (§70 — the CKIR-expansion / coopvec-native provider mapping is 24b/24c). crd-ceir NEVER links
// gpu-context (I3/I4).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/ml_ops.hpp> // register_ml_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::ml
{
// Register the `ml` dialect: its generated ops (register_ml_ops). Idempotent. ⛔ NO type-classes — Tensor is a 3d TypeKind.
Dialect* register_dialect(Context& ctx);

enum class MlMisuseKind : u8
{
    None = 0,
    OperandNotTensor,      // an operand is not Tensor-kinded (either op)
    ResultNotTensor,       // the result is not Tensor-kinded (either op)
    MlpElementMismatch,    // ml.mlp: input/weights/output not all Float-kinded AND equal element
    MlpRankInvalid,        // ml.mlp: input, a weight, or output is not rank-2
    MlpArityInvalid,       // ml.mlp: fewer than 1 weight (needs input + >=1 weight)
    MlpWidthMismatch,      // ml.mlp: the width chain broke (input.dim1==W_1.dim0, W_i.dim1==W_{i+1}.dim0, output==[input.dim0, W_n.dim1])
    MlpActivationInvalid,  // ml.mlp: `activation` not in {relu}
    AttnElementMismatch,   // ml.attention: q/k/v/output not all Float-kinded AND equal element
    AttnRankInvalid,       // ml.attention: q, k, v, or output is not rank-2
    AttnHeadDimMismatch,   // ml.attention: query.dim1 != key.dim1 (the head dim D)
    AttnSeqMismatch,       // ml.attention: key.dim0 != value.dim0 (the key/value seq length Sk)
    AttnOutShapeMismatch,  // ml.attention: output != [query.dim0 (Sq), value.dim1 (Dv)]
};
[[nodiscard]] containers::StringView ml_misuse_kind_name(MlMisuseKind k) noexcept;

// The pointing result: the FIRST misuse (pre-order), the offending `op`, and the `value` it points at (null for an attr misuse).
struct MlMisuse
{
    const Value*     value = nullptr;
    const Operation* op    = nullptr;
    MlMisuseKind     kind  = MlMisuseKind::None;
};
// The FIRST ml misuse in module `m` (pre-order), or {None}. ⛔ const Context& — ml reads types + attrs, interns nothing.
[[nodiscard]] MlMisuse find_ml_misuse(const Context& ctx, const Module& m);
} // namespace crd::ceir::ml
