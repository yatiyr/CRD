#pragma once

// crd-ceir — the layout dialect's find_layout_misuse verifier (CEIR-21c, §22). ONE Pure value-op (generated from
// layout.ceirop.toml): `layout.constrain(%tensor) {kind, …params} -> %tensor` — an OPTIONAL data-layout CONSTRAINT annotating a
// tensor (§22: "the compiler/provider chooses the physical layout where possible; allow explicit constraints where interop or
// algorithms require them"). ⛔ MECHANISM (advisor-locked): a CONSTRAINT OP, NOT a Tensor-type member — the tensor TYPE stays
// [element, shape] (3d), so the 21b tensor ops + their result-identity checks are UNTOUCHED; §23's tensor<S,E,L> is a Dxxx
// named-forward to the first type-identity consumer. ⛔ NO new type-classes; register_dialect just registers the generated op.
// The generated per-op verifier owns STRUCTURAL conformance (1 operand, 1 result, `kind` present+String); THIS owns the SEMANTIC
// chain: input/result Tensor-kinded, the passthrough type IDENTITY (result == input — the 21a AssertResultMismatch mold), the
// `kind` closed vocab, and the KIND-GATED params (a param under the wrong kind is a MISUSE, not ignored — the 12a nonsense-by-
// construction / 12b history_length-only-with-history rule) + their arity vs the tensor's RANK. ⛔ I6 — switches on op NAME, never
// op.kind. ⛔ Declare-only: typed NoSemantics + NO kernel_ref (§70). const Context& (no interning). sparse/packed-quantized kinds
// EXCLUDED (CEIR-23).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/layout_ops.hpp> // register_layout_ops (the generated op)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::layout
{
// Register the `layout` dialect: its generated op (register_layout_ops). Idempotent. ⛔ NO type-classes (Tensor is a 3d TypeKind).
Dialect* register_dialect(Context& ctx);

enum class LayoutMisuseKind : u8
{
    None = 0,
    OperandNotTensor,    // layout.constrain's operand is not Tensor-kinded
    ResultNotTensor,     // layout.constrain's result is not Tensor-kinded
    ResultTypeMismatch,  // the result type != the operand type (it is a PASSTHROUGH — same tensor, now constrained)
    KindInvalid,         // `kind` not in {row_major,col_major,strided,blocked,aos,soa,swizzle}
    ParamKindMismatch,   // a param (strides/block/swizzle) present under the WRONG kind (12a nonsense-by-construction)
    StridesArityMismatch,// `strides` element count != the tensor's rank
    BlockInvalid,        // `block` count != rank, or a block extent < 1
};
[[nodiscard]] containers::StringView layout_misuse_kind_name(LayoutMisuseKind k) noexcept;

struct LayoutMisuse
{
    const Value*     value = nullptr;
    const Operation* op    = nullptr;
    LayoutMisuseKind kind  = LayoutMisuseKind::None;
};
// The FIRST layout misuse in module `m` (pre-order), or {None}. const Context& (no interning — pure reads).
[[nodiscard]] LayoutMisuse find_layout_misuse(const Context& ctx, const Module& m);
} // namespace crd::ceir::layout
