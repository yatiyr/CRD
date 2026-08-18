#pragma once

// crd-ceir — the tensor dialect's find_tensor_misuse verifier (CEIR-21b §51 + CEIR-22a §59). The STRUCTURAL SIX PURE value-ops
// (generated from tensor.ceirop.toml) over the CEIR-3d Tensor TYPE (members = [element, shape]): elementwise / broadcast /
// reshape / transpose / reduce / matmul — plus CEIR-22a's `fft` (a c2c FFT, complex = SPLIT re/im: 2 tensor operands → 2 results,
// mirroring the CKIR c2c split-buffer ABI; the §59 GEMM→FFT→reduction charter's FFT leg). ⛔ NO new type-classes (Tensor is a 3d TypeKind); register_dialect just registers the generated
// ops. The generated per-op verifier owns STRUCTURAL conformance (operand/result counts + required-attr PRESENCE + KIND — `fn`
// String, `axis` Int, `perm` String); THIS owns the SHAPE-AWARE TYPE chain: operands/result Tensor-kinded, ELEMENT consistency
// (members[0]), and the SHAPE relations — reusing the 3d predicates (shapes_broadcast / shapes_reshape) + the 21b producer
// shapes_broadcast_result + the NEW check classes (transpose PERMUTATION, reduce AXIS-BOUNDS, matmul CONTRACTION-DIM) with the
// band-locked contract: Incompatible → a POINTING misuse (the 3z position), Unknown → ACCEPT (a shape.assert discharges),
// Compatible → accept + RESULT-SHAPE IDENTITY (the 21a make/extent/reshape result-identity precedent). ⛔ I6 — find_tensor_misuse
// switches on op NAME + TypeKind, never op.kind. ⛔ Declare-only: typed NoSemantics + NO kernel_ref / NO lowering hook (§70).
// ⛔ Context& (NON-const, the find_work_misuse precedent) — shapes_broadcast_result INTERNS the expected result Shape.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/tensor_ops.hpp> // register_tensor_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::tensor
{
// Register the `tensor` dialect: its generated ops (register_tensor_ops). Idempotent. ⛔ NO type-classes — Tensor is a 3d
// TypeKind this dialect operates over.
Dialect* register_dialect(Context& ctx);

enum class TensorMisuseKind : u8
{
    None = 0,
    OperandNotTensor,           // an op's tensor operand is not Tensor-kinded
    ResultNotTensor,            // an op's result is not Tensor-kinded
    TensorElementMismatch,      // the operands'/result's element types (members[0]) do not all agree
    FnInvalid,                  // elementwise `fn` not in {add,sub,mul,div,max,min,pow} / reduce `fn` not in {sum,prod,max,min,mean}
    ShapeBroadcastIncompatible, // elementwise/broadcast: shapes_broadcast Incompatible (position = the 3z right-aligned axis)
    BroadcastResultMismatch,    // elementwise: result.shape != shapes_broadcast_result(lhs,rhs) / broadcast: input does not broadcast UP to result.shape
    ShapeReshapeIncompatible,   // reshape: shapes_reshape(input.shape, result.shape) == Incompatible
    PermInvalid,                // transpose: `perm` is not a TRUE permutation of [0, rank) (bad token / wrong length / dup / out-of-range)
    TransposeResultMismatch,    // transpose: result.shape != input.shape permuted by `perm`
    AxisInvalid,                // reduce: `axis` < 0 or >= input rank (or non-Int)
    ReduceResultMismatch,       // reduce: result.shape != input.shape with `axis` removed
    MatmulRankInvalid,          // matmul: an operand's shape rank < 2
    ContractionMismatch,        // matmul: lhs's last axis (K) != rhs's second-to-last axis (K)
    MatmulResultMismatch,       // matmul: result.shape != [batch.., M, N]
    BatchMismatch,              // matmul: a leading BATCH dim differs between lhs and rhs (right-aligned; static-differ ⇒ misuse)
    FftInputShapeMismatch,      // fft (CEIR-22a): re_in.shape != im_in.shape (the split-complex convention)
    FftResultShapeMismatch,     // fft: re_out/im_out shape != re_in.shape (a c2c FFT preserves shape)
    FftDirectionInvalid,        // fft: `direction` not in {forward, inverse}
};
[[nodiscard]] containers::StringView tensor_misuse_kind_name(TensorMisuseKind k) noexcept;

// The pointing result: the FIRST misuse (pre-order), the offending `op`, the `value` it points at (null for an attr misuse),
// and `position` = the right-aligned axis for ShapeBroadcastIncompatible (else -1) — the 3z pointing-diagnostic contract.
struct TensorMisuse
{
    const Value*     value    = nullptr;
    const Operation* op       = nullptr;
    TensorMisuseKind kind     = TensorMisuseKind::None;
    i32              position = -1;
};
// The FIRST tensor misuse in module `m` (pre-order), or {None}. Owns the SHAPE-AWARE element/shape chain (see the enum).
// ⛔ Context& (NON-const): shapes_broadcast_result interns the expected result Shape (the find_work_misuse non-const precedent).
[[nodiscard]] TensorMisuse find_tensor_misuse(Context& ctx, const Module& m);
} // namespace crd::ceir::tensor
