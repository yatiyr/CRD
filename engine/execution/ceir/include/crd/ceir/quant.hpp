#pragma once

// crd-ceir — the quant dialect's find_quant_misuse verifier (CEIR-23a, §54). TWO PURE value-ops `quantize` (float →
// integer-storage) + `dequantize` (integer-storage → float), generated from quant.ceirop.toml, over the CEIR-3d Tensor TYPE
// (members = [element, shape]). ⛔ NO new element TypeKind (the split-complex precedent) — the STORAGE int rides the result
// (type_int(width,signed)); scale/zero_point are OPERANDS (rank-0 per-tensor / rank-1 per-axis, ONE form). The generated
// verify_quantize/verify_dequantize own STRUCTURAL conformance (3 operands, 1 result, axis present+Int, scheme present+String);
// THIS owns the SHAPE-AWARE + ELEMENT-ROLE chain: operands/result Tensor-kinded, output.shape == input.shape, scale.shape ==
// zero_point.shape, the scale rank-0|1 + per-axis (axis + scale.dim0 == input.dim[axis]) contract, and the VALUE(float)/
// STORAGE(int) element roles (swapped between quantize + dequantize), and the scheme CLOSED vocab. ⛔ ELEMENT-AGNOSTIC: the
// Q8/INT8/FP8-specific restriction rides the 23b PROVIDER (typed SynthReject), not here (the 22a precedent). ⛔ I6 —
// find_quant_misuse switches on op NAME + TypeKind, never op.kind. ⛔ Declare-only: typed NoSemantics + NO kernel_ref /
// NO lowering hook (§70 — the ckir/native-Q8 provider mapping is 23b). crd-ceir NEVER links gpu-context (I3/I4).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/quant_ops.hpp> // register_quant_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::quant
{
// Register the `quant` dialect: its generated ops (register_quant_ops). Idempotent. ⛔ NO type-classes — Tensor is a 3d TypeKind.
Dialect* register_dialect(Context& ctx);

enum class QuantMisuseKind : u8
{
    None = 0,
    OperandNotTensor,        // an operand (input/scale/zero_point) is not Tensor-kinded
    ResultNotTensor,         // the result (output) is not Tensor-kinded
    ShapeMismatch,           // output.shape != input.shape (quantize/dequantize preserve the value shape)
    ScaleZeroPointMismatch,  // scale.shape != zero_point.shape (they index the same quantization grid)
    ScaleRankInvalid,        // scale is neither RANK-0 (per-tensor) nor RANK-1 (per-axis)
    AxisScaleMismatch,       // a RANK-1 scale but `axis` out of [0,rank) OR scale.dim0 != input.dim[axis]
    ScaleElementMismatch,    // scale.element != the VALUE (float) side (== input for quantize / output for dequantize)
    ZeroPointElementMismatch, // zero_point.element != the STORAGE (int) side (== output for quantize / input for dequantize)
    SchemeInvalid,           // `scheme` not in {symmetric, asymmetric}
};
[[nodiscard]] containers::StringView quant_misuse_kind_name(QuantMisuseKind k) noexcept;

// The pointing result: the FIRST misuse (pre-order), the offending `op`, and the `value` it points at (null for an attr misuse).
struct QuantMisuse
{
    const Value*    value = nullptr;
    const Operation* op   = nullptr;
    QuantMisuseKind kind  = QuantMisuseKind::None;
};
// The FIRST quant misuse in module `m` (pre-order), or {None}. ⛔ const Context& — quant reads types + attrs, interns nothing.
[[nodiscard]] QuantMisuse find_quant_misuse(const Context& ctx, const Module& m);
} // namespace crd::ceir::quant
