#include <crd/ceir/linalg.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::linalg
{
namespace
{
constexpr usize kMaxRank = 16U; // the shape-computer buffers; no real gemm exceeds it (mirrors the 21b tensor helpers)

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

// op(X)'s shape: X's shape, or — if `trans` — the innermost TWO axes swapped (X^T). {} if rank < 2 or > kMaxRank.
[[nodiscard]] TypeId op_shape(Context& ctx, TypeId shape, bool trans)
{
    if (!trans) { return shape; }
    const Type  st = ctx.type_of(shape);
    const usize r  = st.members.size();
    if (r < 2U || r > kMaxRank) { return {}; }
    TypeId dims[kMaxRank];
    for (usize i = 0; i < r; ++i) { dims[i] = st.members[i]; }
    const TypeId tmp = dims[r - 1U];
    dims[r - 1U]     = dims[r - 2U];
    dims[r - 2U]     = tmp;
    return ctx.type_shape(containers::ConstSpan<TypeId>(dims, r));
}
// The gemm result Shape [batch.., M, N] from op(A) [.., M, K] and op(B) [.., K, N] (both rank >= 2). Batch = op(A)'s leading.
[[nodiscard]] TypeId gemm_shape(Context& ctx, TypeId a_shape, TypeId b_shape)
{
    const Type  as = ctx.type_of(a_shape);
    const Type  bs = ctx.type_of(b_shape);
    const usize r  = as.members.size();
    if (r < 2U || bs.members.size() < 2U || r > kMaxRank) { return {}; }
    TypeId dims[kMaxRank];
    for (usize i = 0; i < r; ++i) { dims[i] = as.members[i]; } // batch.. + M (0..r-2) + K at r-1
    dims[r - 1U] = bs.members[bs.members.size() - 1U];         // replace K with N (op(B)'s last)
    return ctx.type_shape(containers::ConstSpan<TypeId>(dims, r));
}

// The pre-order walk — the FIRST linalg misuse, or {None}. Per-op by op NAME (never op.kind — I6); standalone-robust
// (every operand/result/attr access arity-guarded). ⛔ Context& NON-const: the shape-computers intern.
LinalgMisuse scan_linalg_region(Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == containers::StringView("linalg.gemm"))
            {
                // (1) operands (a,b,c) + result (d) Tensor-kinded (KIND — the generated verify_gemm owns arity).
                for (u32 i = 0; i < op->num_operands(); ++i)
                {
                    if (!is_tensor(ctx, op->operand(i))) { return {op->operand(i), op, LinalgMisuseKind::OperandNotTensor}; }
                }
                if (op->num_results() >= 1U && ctx.type_of(op->result(0U)->type()).kind != TypeKind::Tensor)
                {
                    return {op->result(0U), op, LinalgMisuseKind::ResultNotTensor};
                }
                // ⛔ min-arity guard BEFORE any operand(1/2) read (the 21x fold): a malformed under-arity gemm FOLDS to the
                //    generated verify_gemm's arity check (3 operands + 1 result), never trips an in-walk assert.
                if (op->num_operands() < 3U || op->num_results() == 0U) { continue; }
                const TypeId dt = op->result(0U)->type();
                const TypeId e0 = elem_of(ctx, op->operand(0U)->type());
                // (2) element consistency across a/b/c/d (members[0]).
                for (u32 i = 1; i < op->num_operands(); ++i)
                {
                    if (elem_of(ctx, op->operand(i)->type()) != e0) { return {op->operand(i), op, LinalgMisuseKind::ElementMismatch}; }
                }
                if (elem_of(ctx, dt) != e0) { return {op->result(0U), op, LinalgMisuseKind::ElementMismatch}; }
                // (3) shapes. A,B rank >= 2, then op(A)/op(B) apply the trans flags -> [.., M, K] / [.., K, N].
                const TypeId sa = shape_of(ctx, op->operand(0U)->type());
                const TypeId sb = shape_of(ctx, op->operand(1U)->type());
                if (shape_rank(ctx, sa) < 2U || shape_rank(ctx, sb) < 2U) { return {nullptr, op, LinalgMisuseKind::RankInvalid}; }
                const AttrValue ta      = ctx.attr_value(op->attr(containers::StringView("trans_a")));
                const AttrValue tb      = ctx.attr_value(op->attr(containers::StringView("trans_b")));
                const bool      trans_a = (ta.kind == AttrKind::Bool) && ta.b;
                const bool      trans_b = (tb.kind == AttrKind::Bool) && tb.b;
                const TypeId    oa      = op_shape(ctx, sa, trans_a);
                const TypeId    ob      = op_shape(ctx, sb, trans_b);
                if (!oa.valid() || !ob.valid()) { return {nullptr, op, LinalgMisuseKind::RankInvalid}; }
                const usize ra = shape_rank(ctx, oa);
                const usize rb = shape_rank(ctx, ob);
                // contraction: op(A)'s last axis (K) == op(B)'s second-to-last axis (K). Static-differ ⇒ misuse; dynamic/
                // symbolic-differ ⇒ Unknown (defer) — TypeId equality covers static-equal + same-symbolic (the 21b matmul rule).
                const TypeId kl = ctx.type_of(oa).members[ra - 1U];
                const TypeId kr = ctx.type_of(ob).members[rb - 2U];
                if (kl != kr && static_dim(ctx, kl) && static_dim(ctx, kr))
                {
                    return {op->operand(1U), op, LinalgMisuseKind::ContractionMismatch};
                }
                // BATCH dims (the leading rank-2 axes) AGREE right-aligned (static-differ ⇒ misuse — the 21b matmul precedent).
                const usize nbl = ra - 2U;
                const usize nbr = rb - 2U;
                const usize nb  = nbl < nbr ? nbl : nbr;
                for (usize i = 0; i < nb; ++i)
                {
                    const TypeId bl = ctx.type_of(oa).members[nbl - 1U - i];
                    const TypeId br = ctx.type_of(ob).members[nbr - 1U - i];
                    if (bl != br && static_dim(ctx, bl) && static_dim(ctx, br))
                    {
                        return {op->operand(1U), op, LinalgMisuseKind::BatchMismatch};
                    }
                }
                // result D.shape == [batch.., M, N] (the 21a/21b result-shape identity precedent).
                const TypeId sd  = shape_of(ctx, dt);
                const TypeId exp = gemm_shape(ctx, oa, ob);
                if (exp.valid() && sd != exp) { return {op->result(0U), op, LinalgMisuseKind::ResultShapeMismatch}; }
                // accumulator: C.shape == D.shape (beta·C is added; C the additive term, [.., M, N]).
                const TypeId sc = shape_of(ctx, op->operand(2U)->type());
                if (sc != sd) { return {op->operand(2U), op, LinalgMisuseKind::AccumulatorShapeMismatch}; }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const LinalgMisuse e = scan_linalg_region(ctx, op->region(i));
                if (e.kind != LinalgMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    return register_linalg_ops(ctx); // generated: the linalg dialect + its gemm op (idempotent). No type-classes (Tensor is 3d).
}

containers::StringView linalg_misuse_kind_name(LinalgMisuseKind k) noexcept
{
    switch (k)
    {
    case LinalgMisuseKind::None: return containers::StringView("none");
    case LinalgMisuseKind::OperandNotTensor: return containers::StringView("operand-not-tensor");
    case LinalgMisuseKind::ResultNotTensor: return containers::StringView("result-not-tensor");
    case LinalgMisuseKind::ElementMismatch: return containers::StringView("element-mismatch");
    case LinalgMisuseKind::RankInvalid: return containers::StringView("rank-invalid");
    case LinalgMisuseKind::ContractionMismatch: return containers::StringView("contraction-mismatch");
    case LinalgMisuseKind::ResultShapeMismatch: return containers::StringView("result-shape-mismatch");
    case LinalgMisuseKind::AccumulatorShapeMismatch: return containers::StringView("accumulator-shape-mismatch");
    case LinalgMisuseKind::BatchMismatch: return containers::StringView("batch-mismatch");
    }
    return containers::StringView("?");
}

LinalgMisuse find_linalg_misuse(Context& ctx, const Module& m) { return scan_linalg_region(ctx, m.body()); }
} // namespace crd::ceir::linalg
