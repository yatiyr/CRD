#include <crd/ceir/quant.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::quant
{
namespace
{
using containers::StringView;

[[nodiscard]] bool is_tensor(const Context& ctx, const Value* v) noexcept
{
    return v != nullptr && ctx.type_of(v->type()).kind == TypeKind::Tensor;
}
[[nodiscard]] TypeId elem_of(const Context& ctx, TypeId t) noexcept
{
    const Type tt = ctx.type_of(t);
    return tt.members.size() >= 1U ? tt.members[0] : TypeId{};
}
[[nodiscard]] TypeId shape_of(const Context& ctx, TypeId t) noexcept
{
    const Type tt = ctx.type_of(t);
    return tt.members.size() >= 2U ? tt.members[1] : TypeId{};
}
[[nodiscard]] usize shape_rank(const Context& ctx, TypeId shape) noexcept { return ctx.type_of(shape).members.size(); }
[[nodiscard]] bool   static_dim(const Context& ctx, TypeId dim) noexcept
{
    return static_cast<DimKind>(ctx.type_of(dim).cols) == DimKind::Static;
}

// The pre-order walk — the FIRST quant misuse, or {None}. Per-op by op NAME (never op.kind — I6); standalone-robust (every
// operand/result/attr access arity-guarded). ⛔ const Context& — reads types + attrs, interns nothing.
QuantMisuse scan_quant_region(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const StringView nm       = ctx.op_name(op->kind());
            const bool       quantize = nm == StringView("quant.quantize");
            const bool       dequant  = nm == StringView("quant.dequantize");
            if (quantize || dequant)
            {
                // (1) operands (input,scale,zero_point) + result (output) Tensor-kinded (KIND — generated verify owns arity).
                for (u32 i = 0; i < op->num_operands(); ++i)
                {
                    if (!is_tensor(ctx, op->operand(i))) { return {op->operand(i), op, QuantMisuseKind::OperandNotTensor}; }
                }
                if (op->num_results() >= 1U && ctx.type_of(op->result(0U)->type()).kind != TypeKind::Tensor)
                {
                    return {op->result(0U), op, QuantMisuseKind::ResultNotTensor};
                }
                // ⛔ min-arity guard BEFORE any operand(1/2)/result read (the 21x fold): an under-arity op FOLDS to the
                //    generated verify's arity check (3 operands + 1 result), never trips an in-walk assert.
                if (op->num_operands() < 3U || op->num_results() == 0U) { continue; }
                const TypeId it = op->operand(0U)->type(); // input
                const TypeId st = op->operand(1U)->type(); // scale
                const TypeId zt = op->operand(2U)->type(); // zero_point
                const TypeId ot = op->result(0U)->type();  // output

                // (2) output.shape == input.shape (quantize/dequantize preserve the value shape).
                const TypeId ish = shape_of(ctx, it);
                if (shape_of(ctx, ot) != ish) { return {op->result(0U), op, QuantMisuseKind::ShapeMismatch}; }
                // (3) scale.shape == zero_point.shape (they index the same quantization grid).
                const TypeId ssh = shape_of(ctx, st);
                if (shape_of(ctx, zt) != ssh) { return {op->operand(2U), op, QuantMisuseKind::ScaleZeroPointMismatch}; }
                // (4) scale is RANK-0 (per-tensor) or RANK-1 (per-axis).
                const usize srank = shape_rank(ctx, ssh);
                if (srank > 1U) { return {op->operand(1U), op, QuantMisuseKind::ScaleRankInvalid}; }
                // (5) a RANK-1 scale => `axis` in [0, input rank) AND scale.dim0 == input.dim[axis] (per-axis over `axis`).
                if (srank == 1U)
                {
                    const AttrValue av   = ctx.attr_value(op->attr(StringView("axis")));
                    const i64       axis = av.kind == AttrKind::Int ? av.i : -1;
                    const usize     irk  = shape_rank(ctx, ish);
                    if (axis < 0 || static_cast<usize>(axis) >= irk) { return {op->operand(1U), op, QuantMisuseKind::AxisScaleMismatch}; }
                    const TypeId sdim = ctx.type_of(ssh).members[0];
                    const TypeId idim = ctx.type_of(ish).members[static_cast<usize>(axis)];
                    if (sdim != idim && static_dim(ctx, sdim) && static_dim(ctx, idim))
                    {
                        return {op->operand(1U), op, QuantMisuseKind::AxisScaleMismatch};
                    }
                }
                // (6) VALUE(float)/STORAGE(int) element ROLES (swapped between the two ops): scale is the VALUE side, zero_point
                //     the STORAGE side. quantize: value=input, storage=output. dequantize: value=output, storage=input.
                const TypeId value_elem   = elem_of(ctx, quantize ? it : ot);
                const TypeId storage_elem = elem_of(ctx, quantize ? ot : it);
                if (elem_of(ctx, st) != value_elem) { return {op->operand(1U), op, QuantMisuseKind::ScaleElementMismatch}; }
                if (elem_of(ctx, zt) != storage_elem) { return {op->operand(2U), op, QuantMisuseKind::ZeroPointElementMismatch}; }
                // (7) `scheme` in {symmetric, asymmetric} (a wrong-KIND / absent scheme folds to SchemeInvalid — the 12b fold).
                const AttrValue sc = ctx.attr_value(op->attr(StringView("scheme")));
                const bool      ok = sc.kind == AttrKind::String
                                     && (sc.s == StringView("symmetric") || sc.s == StringView("asymmetric"));
                if (!ok) { return {nullptr, op, QuantMisuseKind::SchemeInvalid}; }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const QuantMisuse e = scan_quant_region(ctx, op->region(i));
                if (e.kind != QuantMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    return register_quant_ops(ctx); // generated: the quant dialect + quantize/dequantize (idempotent). No type-classes.
}

containers::StringView quant_misuse_kind_name(QuantMisuseKind k) noexcept
{
    switch (k)
    {
    case QuantMisuseKind::None: return StringView("none");
    case QuantMisuseKind::OperandNotTensor: return StringView("operand-not-tensor");
    case QuantMisuseKind::ResultNotTensor: return StringView("result-not-tensor");
    case QuantMisuseKind::ShapeMismatch: return StringView("shape-mismatch");
    case QuantMisuseKind::ScaleZeroPointMismatch: return StringView("scale-zero-point-mismatch");
    case QuantMisuseKind::ScaleRankInvalid: return StringView("scale-rank-invalid");
    case QuantMisuseKind::AxisScaleMismatch: return StringView("axis-scale-mismatch");
    case QuantMisuseKind::ScaleElementMismatch: return StringView("scale-element-mismatch");
    case QuantMisuseKind::ZeroPointElementMismatch: return StringView("zero-point-element-mismatch");
    case QuantMisuseKind::SchemeInvalid: return StringView("scheme-invalid");
    }
    return StringView("?");
}

QuantMisuse find_quant_misuse(const Context& ctx, const Module& m) { return scan_quant_region(ctx, m.body()); }
} // namespace crd::ceir::quant
