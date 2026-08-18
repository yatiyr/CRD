#pragma once

// crd-ceir — the linalg dialect's find_linalg_misuse verifier (CEIR-22a, §52/§59). ONE PURE value-op `gemm` (generated from
// linalg.ceirop.toml) over the CEIR-3d Tensor TYPE (members = [element, shape]): the BLAS3 identity D = alpha·op(A)·op(B) + beta·C,
// op(X) = X or Xᵀ (the innermost two axes swapped) per the trans flags. ⛔ NO new type-classes (Tensor is a 3d TypeKind);
// register_dialect just registers the generated op. The generated verify_gemm owns STRUCTURAL conformance (3 operands, 1 result,
// alpha/beta present+Float, trans_a/trans_b present+Bool); THIS owns the SHAPE-AWARE TYPE chain: operands/result Tensor-kinded,
// ELEMENT consistency (members[0]), and — REUSING the 21b matmul shape machinery WITH the trans flags applied first — the
// CONTRACTION-DIM (op(A).K == op(B).K), the leading BATCH dims, the RESULT shape [batch.., M, N], and the ACCUMULATOR C.shape ==
// D.shape. ⛔ gemm is DISTINCT from 21b tensor.matmul (matmul == gemm alpha=1,beta=0,no-trans,C ignored) — NOT a duplicate.
// ⛔ I6 — find_linalg_misuse switches on op NAME + TypeKind, never op.kind. ⛔ Declare-only: typed NoSemantics + NO kernel_ref /
// NO lowering hook (§70 — the ckir/hesap/vendor PROVIDER mapping is 22b). ⛔ Context& (NON-const, the find_tensor_misuse
// precedent) — the shape-computer INTERNS the expected result Shape.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/linalg_ops.hpp> // register_linalg_ops (the generated op)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::linalg
{
// Register the `linalg` dialect: its generated op (register_linalg_ops). Idempotent. ⛔ NO type-classes — Tensor is a 3d
// TypeKind this dialect operates over.
Dialect* register_dialect(Context& ctx);

enum class LinalgMisuseKind : u8
{
    None = 0,
    OperandNotTensor,        // gemm: an operand (a/b/c) is not Tensor-kinded
    ResultNotTensor,         // gemm: the result (d) is not Tensor-kinded
    ElementMismatch,         // gemm: a/b/c/d element types (members[0]) do not all agree
    RankInvalid,             // gemm: op A or B has shape rank < 2
    ContractionMismatch,     // gemm: op(A)'s last axis (K) != op(B)'s second-to-last axis (K)
    ResultShapeMismatch,     // gemm: d.shape != [batch.., M, N]
    AccumulatorShapeMismatch, // gemm: c.shape (the beta·C accumulator) != d.shape
    BatchMismatch,           // gemm: a leading BATCH dim differs between op(A) and op(B) (right-aligned; static-differ ⇒ misuse)
};
[[nodiscard]] containers::StringView linalg_misuse_kind_name(LinalgMisuseKind k) noexcept;

// The pointing result: the FIRST misuse (pre-order), the offending `op`, and the `value` it points at (null for an attr misuse).
struct LinalgMisuse
{
    const Value*     value = nullptr;
    const Operation* op    = nullptr;
    LinalgMisuseKind kind  = LinalgMisuseKind::None;
};
// The FIRST linalg misuse in module `m` (pre-order), or {None}. ⛔ Context& (NON-const): the shape-computer interns the
// expected result Shape (the find_tensor_misuse precedent).
[[nodiscard]] LinalgMisuse find_linalg_misuse(Context& ctx, const Module& m);
} // namespace crd::ceir::linalg
