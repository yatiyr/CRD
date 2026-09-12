#include <crd/ceir/ml.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::ml
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
[[nodiscard]] usize rank_of(const Context& ctx, TypeId t) noexcept { return ctx.type_of(shape_of(ctx, t)).members.size(); }
[[nodiscard]] bool  is_float_elem(const Context& ctx, TypeId t) noexcept
{
    return ctx.type_of(elem_of(ctx, t)).kind == TypeKind::Float;
}
// The static extent of tensor `t`'s dim `axis`: true + `out` set iff the dim exists AND is Static (the both-static read half).
[[nodiscard]] bool dim_static(const Context& ctx, TypeId t, usize axis, crd::u32& out) noexcept
{
    const Type sh = ctx.type_of(shape_of(ctx, t));
    if (axis >= sh.members.size()) { return false; }
    const Type d = ctx.type_of(sh.members[axis]);
    if (static_cast<DimKind>(d.cols) != DimKind::Static) { return false; }
    out = d.count;
    return true;
}
// Do tensor `t1`'s dim `a1` and `t2`'s dim `a2` CONFLICT? True iff BOTH are static AND the counts differ (a dynamic dim never
// conflicts — the tri-state Unknown→accept doctrine).
[[nodiscard]] bool dims_conflict(const Context& ctx, TypeId t1, usize a1, TypeId t2, usize a2) noexcept
{
    crd::u32 c1 = 0;
    crd::u32 c2 = 0;
    return dim_static(ctx, t1, a1, c1) && dim_static(ctx, t2, a2, c2) && c1 != c2;
}

// The pre-order walk — the FIRST ml misuse, or {None}. Per-op by op NAME (never op.kind — I6); standalone-robust (every
// operand/result/attr access arity-guarded). ⛔ const Context& — reads types + attrs, interns nothing.
MlMisuse scan_ml_region(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const StringView nm = ctx.op_name(op->kind());

            if (nm == StringView("ml.mlp"))
            {
                for (u32 i = 0; i < op->num_operands(); ++i)
                {
                    if (!is_tensor(ctx, op->operand(i))) { return {op->operand(i), op, MlMisuseKind::OperandNotTensor}; }
                }
                if (op->num_results() >= 1U && ctx.type_of(op->result(0U)->type()).kind != TypeKind::Tensor)
                {
                    return {op->result(0U), op, MlMisuseKind::ResultNotTensor};
                }
                // ⛔ min-arity fold: an under-arity op FOLDS to the generated verify (>=1 operand + a result), never trips
                //    an in-walk out-of-range read. THEN the semantic >=1-weight (input + >=1 weight) is MlpArityInvalid.
                if (op->num_operands() < 1U || op->num_results() == 0U) { continue; }
                if (op->num_operands() < 2U) { return {nullptr, op, MlMisuseKind::MlpArityInvalid}; }
                const u32    nw    = op->num_operands() - 1U; // number of weight matrices (the variadic tail)
                const TypeId in_t  = op->operand(0U)->type();
                const TypeId out_t = op->result(0U)->type();

                // (element) input + every weight + output Float-kinded AND EQUAL.
                if (!is_float_elem(ctx, in_t)) { return {op->operand(0U), op, MlMisuseKind::MlpElementMismatch}; }
                const TypeId e = elem_of(ctx, in_t);
                for (u32 w = 1U; w <= nw; ++w)
                {
                    if (elem_of(ctx, op->operand(w)->type()) != e) { return {op->operand(w), op, MlMisuseKind::MlpElementMismatch}; }
                }
                if (elem_of(ctx, out_t) != e) { return {op->result(0U), op, MlMisuseKind::MlpElementMismatch}; }

                // (rank) input + every weight + output rank-2.
                if (rank_of(ctx, in_t) != 2U) { return {op->operand(0U), op, MlMisuseKind::MlpRankInvalid}; }
                for (u32 w = 1U; w <= nw; ++w)
                {
                    if (rank_of(ctx, op->operand(w)->type()) != 2U) { return {op->operand(w), op, MlMisuseKind::MlpRankInvalid}; }
                }
                if (rank_of(ctx, out_t) != 2U) { return {op->result(0U), op, MlMisuseKind::MlpRankInvalid}; }

                // (width chain) input.dim1==W_1.dim0; W_i.dim1==W_{i+1}.dim0; output.dim0==input.dim0; output.dim1==W_n.dim1.
                if (dims_conflict(ctx, in_t, 1U, op->operand(1U)->type(), 0U)) { return {op->operand(1U), op, MlMisuseKind::MlpWidthMismatch}; }
                for (u32 i = 1U; i < nw; ++i)
                {
                    if (dims_conflict(ctx, op->operand(i)->type(), 1U, op->operand(i + 1U)->type(), 0U))
                    {
                        return {op->operand(i + 1U), op, MlMisuseKind::MlpWidthMismatch};
                    }
                }
                if (dims_conflict(ctx, out_t, 0U, in_t, 0U)) { return {op->result(0U), op, MlMisuseKind::MlpWidthMismatch}; }
                if (dims_conflict(ctx, out_t, 1U, op->operand(nw)->type(), 1U)) { return {op->result(0U), op, MlMisuseKind::MlpWidthMismatch}; }

                // (activation) a CLOSED vocab: relu (only, this band).
                const AttrValue av = ctx.attr_value(op->attr(StringView("activation")));
                if (av.kind != AttrKind::String || av.s != StringView("relu")) { return {nullptr, op, MlMisuseKind::MlpActivationInvalid}; }
            }
            else if (nm == StringView("ml.attention"))
            {
                for (u32 i = 0; i < op->num_operands(); ++i)
                {
                    if (!is_tensor(ctx, op->operand(i))) { return {op->operand(i), op, MlMisuseKind::OperandNotTensor}; }
                }
                if (op->num_results() >= 1U && ctx.type_of(op->result(0U)->type()).kind != TypeKind::Tensor)
                {
                    return {op->result(0U), op, MlMisuseKind::ResultNotTensor};
                }
                if (op->num_operands() < 3U || op->num_results() == 0U) { continue; } // min-arity fold → generated verify
                const TypeId q   = op->operand(0U)->type(); // Q [Sq, D]
                const TypeId k   = op->operand(1U)->type(); // K [Sk, D]
                const TypeId v   = op->operand(2U)->type(); // V [Sk, Dv]
                const TypeId out = op->result(0U)->type();  // out [Sq, Dv]

                // (element) q/k/v/out Float-kinded AND EQUAL.
                if (!is_float_elem(ctx, q)) { return {op->operand(0U), op, MlMisuseKind::AttnElementMismatch}; }
                const TypeId e = elem_of(ctx, q);
                if (elem_of(ctx, k) != e) { return {op->operand(1U), op, MlMisuseKind::AttnElementMismatch}; }
                if (elem_of(ctx, v) != e) { return {op->operand(2U), op, MlMisuseKind::AttnElementMismatch}; }
                if (elem_of(ctx, out) != e) { return {op->result(0U), op, MlMisuseKind::AttnElementMismatch}; }

                // (rank) all rank-2.
                if (rank_of(ctx, q) != 2U) { return {op->operand(0U), op, MlMisuseKind::AttnRankInvalid}; }
                if (rank_of(ctx, k) != 2U) { return {op->operand(1U), op, MlMisuseKind::AttnRankInvalid}; }
                if (rank_of(ctx, v) != 2U) { return {op->operand(2U), op, MlMisuseKind::AttnRankInvalid}; }
                if (rank_of(ctx, out) != 2U) { return {op->result(0U), op, MlMisuseKind::AttnRankInvalid}; }

                // (shape) query.dim1==key.dim1 (D); key.dim0==value.dim0 (Sk); output==[query.dim0 (Sq), value.dim1 (Dv)].
                if (dims_conflict(ctx, q, 1U, k, 1U)) { return {op->operand(1U), op, MlMisuseKind::AttnHeadDimMismatch}; }
                if (dims_conflict(ctx, k, 0U, v, 0U)) { return {op->operand(2U), op, MlMisuseKind::AttnSeqMismatch}; }
                if (dims_conflict(ctx, out, 0U, q, 0U) || dims_conflict(ctx, out, 1U, v, 1U))
                {
                    return {op->result(0U), op, MlMisuseKind::AttnOutShapeMismatch};
                }
            }

            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const MlMisuse e = scan_ml_region(ctx, op->region(i));
                if (e.kind != MlMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    return register_ml_ops(ctx); // generated: the ml dialect + mlp/attention (idempotent). No type-classes.
}

containers::StringView ml_misuse_kind_name(MlMisuseKind k) noexcept
{
    switch (k)
    {
    case MlMisuseKind::None: return StringView("none");
    case MlMisuseKind::OperandNotTensor: return StringView("operand-not-tensor");
    case MlMisuseKind::ResultNotTensor: return StringView("result-not-tensor");
    case MlMisuseKind::MlpElementMismatch: return StringView("mlp-element-mismatch");
    case MlMisuseKind::MlpRankInvalid: return StringView("mlp-rank-invalid");
    case MlMisuseKind::MlpArityInvalid: return StringView("mlp-arity-invalid");
    case MlMisuseKind::MlpWidthMismatch: return StringView("mlp-width-mismatch");
    case MlMisuseKind::MlpActivationInvalid: return StringView("mlp-activation-invalid");
    case MlMisuseKind::AttnElementMismatch: return StringView("attn-element-mismatch");
    case MlMisuseKind::AttnRankInvalid: return StringView("attn-rank-invalid");
    case MlMisuseKind::AttnHeadDimMismatch: return StringView("attn-head-dim-mismatch");
    case MlMisuseKind::AttnSeqMismatch: return StringView("attn-seq-mismatch");
    case MlMisuseKind::AttnOutShapeMismatch: return StringView("attn-out-shape-mismatch");
    }
    return StringView("?");
}

MlMisuse find_ml_misuse(const Context& ctx, const Module& m) { return scan_ml_region(ctx, m.body()); }
} // namespace crd::ceir::ml
