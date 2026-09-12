#pragma once

// crd-ceir — the sparse dialect's find_sparse_misuse verifier (CEIR-23d, §53). ONE PURE op `sparse.spmv` (CSR sparse-matrix ×
// dense-vector, y = A·x), generated from sparse.ceirop.toml, over the CEIR-3d Tensor TYPE (members = [element, shape]). ⛔ NO
// new element TypeKind — a sparse matrix is the COMPOSITION of THREE dense rank-1 tensors (row_ptr, col_idx, values) through
// this op, plus the dense x. The generated verify_spmv owns STRUCTURAL conformance (4 operands, 1 result, no attrs); THIS owns
// the SHAPE-AWARE + ELEMENT-ROLE chain: operands/result Tensor-kinded, all five RANK-1, the CSR index arrays Int-kinded, the
// value side (values/x/y) Float-kinded AND EQUAL, and the CSR shape relations (col_idx.dim0 == values.dim0 = nnz;
// row_ptr.dim0 == y.dim0 + 1 = M+1). ⛔ row_ptr monotonicity / 0 <= col_idx < N / nnz == row_ptr[M] are runtime DATA
// properties, OUT of the TYPE verifier (never attempted here). ⛔ ELEMENT-AGNOSTIC: the u32-index/f32-value + CSR-loop
// restriction rides the 23e PROVIDER (typed SynthReject), not here (the 22a precedent). ⛔ I6 — find_sparse_misuse switches on
// op NAME + TypeKind, never op.kind. ⛔ Declare-only: typed NoSemantics + NO kernel_ref / NO lowering hook (§70 — the authored
// SpMV .ckir kernel is 23e). crd-ceir NEVER links gpu-context (I3/I4); sparse.spmv is NOT a tensor-pipeline plan op this band.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/sparse_ops.hpp> // register_sparse_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::sparse
{
// Register the `sparse` dialect: its generated ops (register_sparse_ops). Idempotent. ⛔ NO type-classes — Tensor is a 3d TypeKind.
Dialect* register_dialect(Context& ctx);

enum class SparseMisuseKind : u8
{
    None = 0,
    OperandNotTensor,     // an operand (row_ptr/col_idx/values/x) is not Tensor-kinded
    ResultNotTensor,      // the result (y) is not Tensor-kinded
    RankInvalid,          // row_ptr, col_idx, values, x, or y is not RANK-1
    IndexElementNotInt,   // row_ptr.element or col_idx.element is not Int-kinded (the CSR index arrays)
    ValueElementMismatch, // values/x/y not all Float-kinded AND equal element (one value type)
    NnzMismatch,          // col_idx.dim0 != values.dim0 (they name the same nnz nonzeros)
    RowPtrLengthMismatch, // row_ptr.dim0 != y.dim0 + 1 (M+1 offsets for M output rows)
};
[[nodiscard]] containers::StringView sparse_misuse_kind_name(SparseMisuseKind k) noexcept;

// The pointing result: the FIRST misuse (pre-order), the offending `op`, and the `value` it points at.
struct SparseMisuse
{
    const Value*     value = nullptr;
    const Operation* op    = nullptr;
    SparseMisuseKind kind  = SparseMisuseKind::None;
};
// The FIRST sparse misuse in module `m` (pre-order), or {None}. ⛔ const Context& — sparse reads types, interns nothing.
[[nodiscard]] SparseMisuse find_sparse_misuse(const Context& ctx, const Module& m);
} // namespace crd::ceir::sparse
