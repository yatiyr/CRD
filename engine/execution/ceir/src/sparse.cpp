#include <crd/ceir/sparse.hpp>

#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::sparse
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
[[nodiscard]] bool   is_int_elem(const Context& ctx, TypeId t) noexcept { return ctx.type_of(elem_of(ctx, t)).kind == TypeKind::Int; }
[[nodiscard]] bool   is_float_elem(const Context& ctx, TypeId t) noexcept
{
    return ctx.type_of(elem_of(ctx, t)).kind == TypeKind::Float;
}
// The static extent of a shape's FIRST dim: true + `out` set iff dim0 exists AND is Static (the both-static guard's read half).
[[nodiscard]] bool dim0_static(const Context& ctx, TypeId shape, crd::u32& out) noexcept
{
    const Type sh = ctx.type_of(shape);
    if (sh.members.size() < 1U) { return false; }
    const Type d = ctx.type_of(sh.members[0]);
    if (static_cast<DimKind>(d.cols) != DimKind::Static) { return false; }
    out = d.count;
    return true;
}

// The pre-order walk — the FIRST sparse misuse, or {None}. Per-op by op NAME (never op.kind — I6); standalone-robust (every
// operand/result access arity-guarded). ⛔ const Context& — reads types, interns nothing.
SparseMisuse scan_sparse_region(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == StringView("sparse.spmv"))
            {
                // (1) operands (row_ptr,col_idx,values,x) + result (y) Tensor-kinded (KIND — the generated verify owns arity).
                for (u32 i = 0; i < op->num_operands(); ++i)
                {
                    if (!is_tensor(ctx, op->operand(i))) { return {op->operand(i), op, SparseMisuseKind::OperandNotTensor}; }
                }
                if (op->num_results() >= 1U && ctx.type_of(op->result(0U)->type()).kind != TypeKind::Tensor)
                {
                    return {op->result(0U), op, SparseMisuseKind::ResultNotTensor};
                }
                // ⛔ min-arity guard BEFORE any operand(1..3)/result read: an under-arity op FOLDS to the generated verify's
                //    arity check (4 operands + 1 result), never trips an in-walk out-of-range access.
                if (op->num_operands() < 4U || op->num_results() == 0U) { continue; }
                const TypeId rp = op->operand(0U)->type(); // row_ptr
                const TypeId ci = op->operand(1U)->type(); // col_idx
                const TypeId vl = op->operand(2U)->type(); // values
                const TypeId xt = op->operand(3U)->type(); // x
                const TypeId yt = op->result(0U)->type();  // y

                // (2) all five RANK-1 (row_ptr [M+1], col_idx/values [nnz], x [N], y [M]).
                if (shape_rank(ctx, shape_of(ctx, rp)) != 1U) { return {op->operand(0U), op, SparseMisuseKind::RankInvalid}; }
                if (shape_rank(ctx, shape_of(ctx, ci)) != 1U) { return {op->operand(1U), op, SparseMisuseKind::RankInvalid}; }
                if (shape_rank(ctx, shape_of(ctx, vl)) != 1U) { return {op->operand(2U), op, SparseMisuseKind::RankInvalid}; }
                if (shape_rank(ctx, shape_of(ctx, xt)) != 1U) { return {op->operand(3U), op, SparseMisuseKind::RankInvalid}; }
                if (shape_rank(ctx, shape_of(ctx, yt)) != 1U) { return {op->result(0U), op, SparseMisuseKind::RankInvalid}; }

                // (3) row_ptr + col_idx Int-kinded (the CSR index arrays).
                if (!is_int_elem(ctx, rp)) { return {op->operand(0U), op, SparseMisuseKind::IndexElementNotInt}; }
                if (!is_int_elem(ctx, ci)) { return {op->operand(1U), op, SparseMisuseKind::IndexElementNotInt}; }

                // (4) the VALUE side — values/x/y are Float-kinded AND EQUAL element (one value type; the scale-element precedent).
                if (!is_float_elem(ctx, vl)) { return {op->operand(2U), op, SparseMisuseKind::ValueElementMismatch}; }
                const TypeId ve = elem_of(ctx, vl);
                if (elem_of(ctx, xt) != ve) { return {op->operand(3U), op, SparseMisuseKind::ValueElementMismatch}; }
                if (elem_of(ctx, yt) != ve) { return {op->result(0U), op, SparseMisuseKind::ValueElementMismatch}; }

                // (5) col_idx.dim0 == values.dim0 (both name the same nnz nonzeros; both-static guard).
                crd::u32 nnz_ci = 0;
                crd::u32 nnz_vl = 0;
                if (dim0_static(ctx, shape_of(ctx, ci), nnz_ci) && dim0_static(ctx, shape_of(ctx, vl), nnz_vl) && nnz_ci != nnz_vl)
                {
                    return {op->operand(1U), op, SparseMisuseKind::NnzMismatch};
                }
                // (6) row_ptr.dim0 == y.dim0 + 1 (M+1 offsets for M output rows; .count ARITHMETIC + both-static guard).
                crd::u32 m_rp = 0;
                crd::u32 m_y  = 0;
                if (dim0_static(ctx, shape_of(ctx, rp), m_rp) && dim0_static(ctx, shape_of(ctx, yt), m_y) && m_rp != m_y + 1U)
                {
                    return {op->operand(0U), op, SparseMisuseKind::RowPtrLengthMismatch};
                }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const SparseMisuse e = scan_sparse_region(ctx, op->region(i));
                if (e.kind != SparseMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    return register_sparse_ops(ctx); // generated: the sparse dialect + spmv (idempotent). No type-classes.
}

containers::StringView sparse_misuse_kind_name(SparseMisuseKind k) noexcept
{
    switch (k)
    {
    case SparseMisuseKind::None: return StringView("none");
    case SparseMisuseKind::OperandNotTensor: return StringView("operand-not-tensor");
    case SparseMisuseKind::ResultNotTensor: return StringView("result-not-tensor");
    case SparseMisuseKind::RankInvalid: return StringView("rank-invalid");
    case SparseMisuseKind::IndexElementNotInt: return StringView("index-element-not-int");
    case SparseMisuseKind::ValueElementMismatch: return StringView("value-element-mismatch");
    case SparseMisuseKind::NnzMismatch: return StringView("nnz-mismatch");
    case SparseMisuseKind::RowPtrLengthMismatch: return StringView("row-ptr-length-mismatch");
    }
    return StringView("?");
}

SparseMisuse find_sparse_misuse(const Context& ctx, const Module& m) { return scan_sparse_region(ctx, m.body()); }
} // namespace crd::ceir::sparse
