#include <crd/ceir/shape.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::shape
{
namespace
{
// Is `v`'s type the given 3d TypeKind? (A null value ⇒ false.) The Shape/Dim/Index kinds this dialect operates over exist since
// CEIR-3d — no interning, so the whole walk stays const.
[[nodiscard]] bool is_kind(const Context& ctx, const Value* v, TypeKind k) noexcept
{
    if (v == nullptr) { return false; }
    return ctx.type_of(v->type()).kind == k;
}

// A shape.assert `relation` token — the CLOSED vocab {equal | broadcast | reshape} (byte compare, the find_dispatch_misuse
// access-token precedent — a value outside the enum the verifier NAMES is a misuse, not a silent pass).
[[nodiscard]] bool is_valid_relation(containers::StringView s) noexcept
{
    return s == containers::StringView("equal") || s == containers::StringView("broadcast")
           || s == containers::StringView("reshape");
}

// The pre-order walk — the FIRST shape misuse, or {None}. Per-op by op NAME (never an op.kind switch — I6), then recurse
// regions. STANDALONE-ROBUST (the 12b wrong-kind fold): every operand/result/attr access is arity-guarded, so a malformed op
// folds into the exact misuse kind rather than reading clean or tripping an assert.
ShapeMisuse scan_shape_region(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm = ctx.op_name(op->kind());
            // ⛔ shape.make: every operand Dim-typed, result Shape-typed.
            if (nm == containers::StringView("shape.make"))
            {
                for (u32 i = 0; i < op->num_operands(); ++i)
                {
                    if (!is_kind(ctx, op->operand(i), TypeKind::Dim))
                    {
                        return {op->operand(i), op, ShapeMisuseKind::MakeOperandNotDim, -1};
                    }
                }
                if (op->num_results() >= 1U && !is_kind(ctx, op->result(0U), TypeKind::Shape))
                {
                    return {op->result(0U), op, ShapeMisuseKind::ResultNotShape, -1};
                }
                // RESULT IDENTITY (the 12a underlying==operand precedent): the result Shape's members ARE the operand dims,
                // in order (rank == operand count AND each member == the matching operand's Dim type).
                if (op->num_results() >= 1U && is_kind(ctx, op->result(0U), TypeKind::Shape))
                {
                    const Type& rt = ctx.type_of(op->result(0U)->type());
                    bool        ok = (rt.members.size() == op->num_operands());
                    for (u32 i = 0; ok && i < op->num_operands(); ++i)
                    {
                        if (rt.members[i] != op->operand(i)->type()) { ok = false; }
                    }
                    if (!ok) { return {op->result(0U), op, ShapeMisuseKind::MakeResultShapeMismatch, -1}; }
                }
            }
            // ⛔ shape.rank: operand(0) Shape-typed, result Index-typed.
            else if (nm == containers::StringView("shape.rank"))
            {
                if (op->num_operands() >= 1U && !is_kind(ctx, op->operand(0U), TypeKind::Shape))
                {
                    return {op->operand(0U), op, ShapeMisuseKind::OperandNotShape, -1};
                }
                if (op->num_results() >= 1U && !is_kind(ctx, op->result(0U), TypeKind::Index))
                {
                    return {op->result(0U), op, ShapeMisuseKind::RankResultNotIndex, -1};
                }
            }
            // ⛔ shape.extent: operand(0) Shape-typed, result Dim-typed, `axis` >= 0 AND < the operand shape's rank (static).
            else if (nm == containers::StringView("shape.extent"))
            {
                if (op->num_operands() >= 1U && !is_kind(ctx, op->operand(0U), TypeKind::Shape))
                {
                    return {op->operand(0U), op, ShapeMisuseKind::OperandNotShape, -1};
                }
                if (op->num_results() >= 1U && !is_kind(ctx, op->result(0U), TypeKind::Dim))
                {
                    return {op->result(0U), op, ShapeMisuseKind::ExtentResultNotDim, -1};
                }
                const AttrValue ax   = ctx.attr_value(op->attr(containers::StringView("axis")));
                const i64       axis = (ax.kind == AttrKind::Int) ? ax.i : -1; // verify_extent ensures Int; robust here
                if (axis < 0) { return {nullptr, op, ShapeMisuseKind::ExtentAxisInvalid, -1}; }
                if (op->num_operands() >= 1U && is_kind(ctx, op->operand(0U), TypeKind::Shape))
                {
                    const Type& st   = ctx.type_of(op->operand(0U)->type());
                    const i64   rank = static_cast<i64>(st.members.size());
                    if (axis >= rank) { return {nullptr, op, ShapeMisuseKind::ExtentAxisInvalid, -1}; }
                    // RESULT IDENTITY: the extent is the operand shape's member Dim at `axis` (axis now in [0, rank)).
                    if (op->num_results() >= 1U && op->result(0U)->type() != st.members[static_cast<usize>(axis)])
                    {
                        return {op->result(0U), op, ShapeMisuseKind::ExtentResultMismatch, -1};
                    }
                }
            }
            // ⛔ shape.broadcast: both operands + result Shape-typed; shapes_broadcast != Incompatible (Unknown = ACCEPT).
            else if (nm == containers::StringView("shape.broadcast"))
            {
                if (op->num_operands() >= 1U && !is_kind(ctx, op->operand(0U), TypeKind::Shape))
                {
                    return {op->operand(0U), op, ShapeMisuseKind::OperandNotShape, -1};
                }
                if (op->num_operands() >= 2U && !is_kind(ctx, op->operand(1U), TypeKind::Shape))
                {
                    return {op->operand(1U), op, ShapeMisuseKind::OperandNotShape, -1};
                }
                if (op->num_results() >= 1U && !is_kind(ctx, op->result(0U), TypeKind::Shape))
                {
                    return {op->result(0U), op, ShapeMisuseKind::ResultNotShape, -1};
                }
                if (op->num_operands() >= 2U)
                {
                    const BroadcastResult br = ctx.shapes_broadcast(op->operand(0U)->type(), op->operand(1U)->type());
                    if (br.compat == ShapeCompat::Incompatible)
                    {
                        return {op->operand(1U), op, ShapeMisuseKind::ShapeBroadcastIncompatible, static_cast<i32>(br.position)};
                    }
                }
            }
            // ⛔ shape.reshape: both operands + result Shape-typed; shapes_reshape != Incompatible (Unknown = ACCEPT).
            else if (nm == containers::StringView("shape.reshape"))
            {
                if (op->num_operands() >= 1U && !is_kind(ctx, op->operand(0U), TypeKind::Shape))
                {
                    return {op->operand(0U), op, ShapeMisuseKind::OperandNotShape, -1};
                }
                if (op->num_operands() >= 2U && !is_kind(ctx, op->operand(1U), TypeKind::Shape))
                {
                    return {op->operand(1U), op, ShapeMisuseKind::OperandNotShape, -1};
                }
                if (op->num_results() >= 1U && !is_kind(ctx, op->result(0U), TypeKind::Shape))
                {
                    return {op->result(0U), op, ShapeMisuseKind::ResultNotShape, -1};
                }
                if (op->num_operands() >= 2U
                    && ctx.shapes_reshape(op->operand(0U)->type(), op->operand(1U)->type()) == ShapeCompat::Incompatible)
                {
                    return {op->operand(1U), op, ShapeMisuseKind::ShapeReshapeIncompatible, -1};
                }
                // RESULT IDENTITY: the result IS the validated `target` operand (reshape passes the target shape through).
                if (op->num_operands() >= 2U && op->num_results() >= 1U
                    && op->result(0U)->type() != op->operand(1U)->type())
                {
                    return {op->result(0U), op, ShapeMisuseKind::ReshapeResultMismatch, -1};
                }
            }
            // ⛔ shape.assert: both operands Shape-typed; `relation` in the closed vocab; result type == the lhs operand type.
            else if (nm == containers::StringView("shape.assert"))
            {
                if (op->num_operands() >= 1U && !is_kind(ctx, op->operand(0U), TypeKind::Shape))
                {
                    return {op->operand(0U), op, ShapeMisuseKind::OperandNotShape, -1};
                }
                if (op->num_operands() >= 2U && !is_kind(ctx, op->operand(1U), TypeKind::Shape))
                {
                    return {op->operand(1U), op, ShapeMisuseKind::OperandNotShape, -1};
                }
                const AttrValue rel = ctx.attr_value(op->attr(containers::StringView("relation")));
                if (rel.kind != AttrKind::String || !is_valid_relation(rel.s))
                {
                    return {nullptr, op, ShapeMisuseKind::AssertRelationInvalid, -1};
                }
                if (op->num_operands() >= 1U && op->num_results() >= 1U
                    && op->result(0U)->type() != op->operand(0U)->type())
                {
                    return {op->result(0U), op, ShapeMisuseKind::AssertResultMismatch, -1};
                }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const ShapeMisuse e = scan_shape_region(ctx, op->region(i));
                if (e.kind != ShapeMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    return register_shape_ops(ctx); // generated: the shape dialect + its six ops (idempotent). No type-classes (Dim/Shape are 3d).
}

containers::StringView shape_misuse_kind_name(ShapeMisuseKind k) noexcept
{
    switch (k)
    {
    case ShapeMisuseKind::None: return containers::StringView("none");
    case ShapeMisuseKind::OperandNotShape: return containers::StringView("operand-not-shape");
    case ShapeMisuseKind::MakeOperandNotDim: return containers::StringView("make-operand-not-dim");
    case ShapeMisuseKind::ResultNotShape: return containers::StringView("result-not-shape");
    case ShapeMisuseKind::RankResultNotIndex: return containers::StringView("rank-result-not-index");
    case ShapeMisuseKind::ExtentResultNotDim: return containers::StringView("extent-result-not-dim");
    case ShapeMisuseKind::ExtentAxisInvalid: return containers::StringView("extent-axis-invalid");
    case ShapeMisuseKind::ShapeBroadcastIncompatible: return containers::StringView("shape-broadcast-incompatible");
    case ShapeMisuseKind::ShapeReshapeIncompatible: return containers::StringView("shape-reshape-incompatible");
    case ShapeMisuseKind::AssertRelationInvalid: return containers::StringView("assert-relation-invalid");
    case ShapeMisuseKind::AssertResultMismatch: return containers::StringView("assert-result-mismatch");
    case ShapeMisuseKind::MakeResultShapeMismatch: return containers::StringView("make-result-shape-mismatch");
    case ShapeMisuseKind::ExtentResultMismatch: return containers::StringView("extent-result-mismatch");
    case ShapeMisuseKind::ReshapeResultMismatch: return containers::StringView("reshape-result-mismatch");
    }
    return containers::StringView("?");
}

ShapeMisuse find_shape_misuse(const Context& ctx, const Module& m) { return scan_shape_region(ctx, m.body()); }
} // namespace crd::ceir::shape
