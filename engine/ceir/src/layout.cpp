#include <crd/ceir/layout.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::layout
{
namespace
{
constexpr usize kMaxRank = 16U;

[[nodiscard]] bool is_tensor(const Context& ctx, const Value* v) noexcept
{
    return v != nullptr && ctx.type_of(v->type()).kind == TypeKind::Tensor;
}
// The rank of a tensor value = its shape's member count (members[1] is the Shape; sec-21 rank is always static). have_rank=false
// only for a malformed (non-2-member) Tensor, guarded by the caller.
[[nodiscard]] usize tensor_rank(const Context& ctx, TypeId tensor, bool& have_rank) noexcept
{
    const Type tt = ctx.type_of(tensor);
    if (tt.members.size() < 2U) { have_rank = false; return 0U; }
    have_rank = true;
    return ctx.type_of(tt.members[1]).members.size();
}
[[nodiscard]] bool kind_in_vocab(containers::StringView s) noexcept
{
    return s == containers::StringView("row_major") || s == containers::StringView("col_major")
           || s == containers::StringView("strided") || s == containers::StringView("blocked")
           || s == containers::StringView("aos") || s == containers::StringView("soa")
           || s == containers::StringView("swizzle");
}
// Parse a comma-separated non-negative int list into `out` (≤ max); false on a malformed/empty token or overflow of max.
// ⛔ A FILE-LOCAL COPY of tensor.cpp's parse_int_list — kept in sync by hand (the parse_access precedent across work/rt/context).
[[nodiscard]] bool parse_int_list(containers::StringView s, i64* out, u32 max, u32& count) noexcept
{
    count = 0U;
    if (s.size() == 0U) { return true; }
    usize start = 0U;
    for (usize i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ',')
        {
            i64  v   = 0;
            bool any = false;
            for (usize j = start; j < i; ++j)
            {
                if (s[j] < '0' || s[j] > '9') { return false; }
                v   = v * 10 + static_cast<i64>(s[j] - '0');
                any = true;
            }
            if (!any || count >= max) { return false; }
            out[count++] = v;
            start = i + 1U;
        }
    }
    return true;
}

LayoutMisuse scan_layout_region(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == containers::StringView("layout.constrain"))
            {
                // input + result Tensor-kinded.
                if (op->num_operands() >= 1U && !is_tensor(ctx, op->operand(0U)))
                {
                    return {op->operand(0U), op, LayoutMisuseKind::OperandNotTensor};
                }
                if (op->num_results() >= 1U && ctx.type_of(op->result(0U)->type()).kind != TypeKind::Tensor)
                {
                    return {op->result(0U), op, LayoutMisuseKind::ResultNotTensor};
                }
                // PASSTHROUGH identity: result type == input type (same tensor, now constrained).
                if (op->num_operands() >= 1U && op->num_results() >= 1U
                    && op->result(0U)->type() != op->operand(0U)->type())
                {
                    return {op->result(0U), op, LayoutMisuseKind::ResultTypeMismatch};
                }
                // `kind` closed vocab.
                const AttrValue kv = ctx.attr_value(op->attr(containers::StringView("kind")));
                if (kv.kind != AttrKind::String || !kind_in_vocab(kv.s))
                {
                    return {nullptr, op, LayoutMisuseKind::KindInvalid};
                }
                // ⛔ min-arity FOLD (the 21b standalone-robust rule): an arity-malformed constrain (0 operands/results — buildable
                //    via create_operation) FOLDS to the generated verify_constrain's arity check, never silently passing the param
                //    section below. After this, operand(0) is a well-formed Tensor (OperandNotTensor returned above otherwise), so
                //    its rank is ALWAYS available (a type_tensor Tensor is [element, shape]) — `have_rank` stays a defensive guard.
                if (op->num_operands() < 1U || op->num_results() < 1U) { continue; }
                // the tensor RANK (for the strides/block arity — always static, §21).
                bool        have_rank = false;
                const usize rank = (op->num_operands() >= 1U && is_tensor(ctx, op->operand(0U)))
                                       ? tensor_rank(ctx, op->operand(0U)->type(), have_rank)
                                       : 0U;
                // KIND-GATED params (present under the wrong kind ⇒ ParamKindMismatch, the 12a nonsense-by-construction).
                i64 buf[kMaxRank];
                u32 cnt = 0U;
                const AttrValue sv = ctx.attr_value(op->attr(containers::StringView("strides")));
                if (sv.kind == AttrKind::String)
                {
                    if (kv.s != containers::StringView("strided")) { return {nullptr, op, LayoutMisuseKind::ParamKindMismatch}; }
                    if (!parse_int_list(sv.s, buf, kMaxRank, cnt) || (have_rank && cnt != rank))
                    {
                        return {nullptr, op, LayoutMisuseKind::StridesArityMismatch};
                    }
                }
                const AttrValue bv = ctx.attr_value(op->attr(containers::StringView("block")));
                if (bv.kind == AttrKind::String)
                {
                    if (kv.s != containers::StringView("blocked")) { return {nullptr, op, LayoutMisuseKind::ParamKindMismatch}; }
                    if (!parse_int_list(bv.s, buf, kMaxRank, cnt) || (have_rank && cnt != rank))
                    {
                        return {nullptr, op, LayoutMisuseKind::BlockInvalid};
                    }
                    for (u32 i = 0; i < cnt; ++i)
                    {
                        if (buf[i] < 1) { return {nullptr, op, LayoutMisuseKind::BlockInvalid}; }
                    }
                }
                const AttrValue wv = ctx.attr_value(op->attr(containers::StringView("swizzle")));
                if (wv.kind == AttrKind::String && kv.s != containers::StringView("swizzle"))
                {
                    return {nullptr, op, LayoutMisuseKind::ParamKindMismatch};
                }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const LayoutMisuse e = scan_layout_region(ctx, op->region(i));
                if (e.kind != LayoutMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    return register_layout_ops(ctx); // generated: the layout dialect + its constrain op (idempotent). No type-classes.
}

containers::StringView layout_misuse_kind_name(LayoutMisuseKind k) noexcept
{
    switch (k)
    {
    case LayoutMisuseKind::None: return containers::StringView("none");
    case LayoutMisuseKind::OperandNotTensor: return containers::StringView("operand-not-tensor");
    case LayoutMisuseKind::ResultNotTensor: return containers::StringView("result-not-tensor");
    case LayoutMisuseKind::ResultTypeMismatch: return containers::StringView("result-type-mismatch");
    case LayoutMisuseKind::KindInvalid: return containers::StringView("kind-invalid");
    case LayoutMisuseKind::ParamKindMismatch: return containers::StringView("param-kind-mismatch");
    case LayoutMisuseKind::StridesArityMismatch: return containers::StringView("strides-arity-mismatch");
    case LayoutMisuseKind::BlockInvalid: return containers::StringView("block-invalid");
    }
    return containers::StringView("?");
}

LayoutMisuse find_layout_misuse(const Context& ctx, const Module& m) { return scan_layout_region(ctx, m.body()); }
} // namespace crd::ceir::layout
