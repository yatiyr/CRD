#include <crd/ceir/tensor.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::tensor
{
namespace
{
constexpr usize kMaxRank = 16U; // the shape-computer buffers; no real tensor exceeds it (mirrors shapes_broadcast_result)

[[nodiscard]] bool is_tensor(const Context& ctx, const Value* v) noexcept
{
    return v != nullptr && ctx.type_of(v->type()).kind == TypeKind::Tensor;
}
// A Tensor's element type (members[0]) / shape type (members[1]); {} if malformed (guarded — a Tensor is always [element,shape]).
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

// A closed `fn` vocab (byte compare — the work `access`-token precedent).
[[nodiscard]] bool fn_in(containers::StringView s, bool reduce) noexcept
{
    if (reduce)
    {
        return s == containers::StringView("sum") || s == containers::StringView("prod")
               || s == containers::StringView("max") || s == containers::StringView("min")
               || s == containers::StringView("mean");
    }
    return s == containers::StringView("add") || s == containers::StringView("sub") || s == containers::StringView("mul")
           || s == containers::StringView("div") || s == containers::StringView("max") || s == containers::StringView("min")
           || s == containers::StringView("pow");
}

// Parse a comma-separated non-negative int list into `out` (≤ max entries); false on a malformed/empty token or overflow of max.
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
// Is `perm` (a parsed int list of `count` entries) a TRUE permutation of [0, rank)?
[[nodiscard]] bool is_permutation(const i64* perm, u32 count, usize rank) noexcept
{
    if (count != rank || rank > kMaxRank) { return false; }
    bool seen[kMaxRank] = {};
    for (u32 i = 0; i < count; ++i)
    {
        if (perm[i] < 0 || perm[i] >= static_cast<i64>(rank)) { return false; }
        if (seen[perm[i]]) { return false; } // duplicate
        seen[perm[i]] = true;
    }
    return true;
}
// Build the Shape whose members are `src`'s members reordered by `perm` (src rank == count, a validated permutation).
[[nodiscard]] TypeId permute_shape(Context& ctx, TypeId src, const i64* perm, u32 count)
{
    const Type st = ctx.type_of(src);
    if (st.members.size() != count || count > kMaxRank) { return {}; }
    TypeId dims[kMaxRank];
    for (u32 i = 0; i < count; ++i) { dims[i] = st.members[static_cast<usize>(perm[i])]; }
    return ctx.type_shape(containers::ConstSpan<TypeId>(dims, count));
}
// Build the Shape with member `axis` removed (rank-1 reduction).
[[nodiscard]] TypeId drop_axis_shape(Context& ctx, TypeId src, usize axis)
{
    const Type st = ctx.type_of(src);
    const usize r = st.members.size();
    if (axis >= r || r == 0U || r > kMaxRank) { return {}; }
    TypeId dims[kMaxRank];
    usize  n = 0U;
    for (usize i = 0; i < r; ++i)
    {
        if (i != axis) { dims[n++] = st.members[i]; }
    }
    return ctx.type_shape(containers::ConstSpan<TypeId>(dims, n));
}
// Build the matmul result Shape [batch.., M, N] from lhs [.., M, K] and rhs [.., K, N] (both rank >= 2). Batch = lhs's leading.
[[nodiscard]] TypeId matmul_shape(Context& ctx, TypeId lhs_shape, TypeId rhs_shape)
{
    const Type ls = ctx.type_of(lhs_shape);
    const Type rs = ctx.type_of(rhs_shape);
    const usize r = ls.members.size();
    if (r < 2U || rs.members.size() < 2U || r > kMaxRank) { return {}; }
    TypeId dims[kMaxRank];
    for (usize i = 0; i < r; ++i) { dims[i] = ls.members[i]; } // batch.. + M (0..r-2) + K at r-1
    dims[r - 1U] = rs.members[rs.members.size() - 1U];         // replace K with N (rhs's last)
    return ctx.type_shape(containers::ConstSpan<TypeId>(dims, r));
}

// The pre-order walk — the FIRST tensor misuse, or {None}. Per-op by op NAME (never op.kind — I6); standalone-robust (every
// operand/result/attr access arity-guarded). ⛔ Context& NON-const: the shape-computers + shapes_broadcast_result intern.
TensorMisuse scan_tensor_region(Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm    = ctx.op_name(op->kind());
            const bool                   is_ew  = nm == containers::StringView("tensor.elementwise");
            const bool                   is_bc  = nm == containers::StringView("tensor.broadcast");
            const bool                   is_rs  = nm == containers::StringView("tensor.reshape");
            const bool                   is_tr  = nm == containers::StringView("tensor.transpose");
            const bool                   is_rd  = nm == containers::StringView("tensor.reduce");
            const bool                   is_mm  = nm == containers::StringView("tensor.matmul");
            const bool                   is_fft = nm == containers::StringView("tensor.fft");
            if (is_ew || is_bc || is_rs || is_tr || is_rd || is_mm || is_fft)
            {
                // (1) operands + result Tensor-kinded (KIND — the generated verify_* owns arity).
                for (u32 i = 0; i < op->num_operands(); ++i)
                {
                    if (!is_tensor(ctx, op->operand(i))) { return {op->operand(i), op, TensorMisuseKind::OperandNotTensor, -1}; }
                }
                if (op->num_results() >= 1U && ctx.type_of(op->result(0U)->type()).kind != TypeKind::Tensor)
                {
                    return {op->result(0U), op, TensorMisuseKind::ResultNotTensor, -1};
                }
                // ⛔ min-arity guard BEFORE any operand(i) read (standalone-robust, the 12b fold: a malformed under-arity op —
                //    buildable via create_operation — must FOLD to the generated verify_*'s arity check, never trip an in-walk assert).
                const u32 min_ops = (is_ew || is_mm || is_fft) ? 2U : 1U;
                const u32 min_res = is_fft ? 2U : 1U; // fft is the first 2-result op (re_out, im_out)
                if (op->num_operands() < min_ops || op->num_results() < min_res) { continue; }
                const TypeId res = op->result(0U)->type();
                const TypeId e0  = elem_of(ctx, op->operand(0U)->type());
                // (2) element consistency: every operand + the result share the element type (members[0]).
                for (u32 i = 1; i < op->num_operands(); ++i)
                {
                    if (elem_of(ctx, op->operand(i)->type()) != e0)
                    {
                        return {op->operand(i), op, TensorMisuseKind::TensorElementMismatch, -1};
                    }
                }
                if (elem_of(ctx, res) != e0) { return {op->result(0U), op, TensorMisuseKind::TensorElementMismatch, -1}; }

                const TypeId s0  = shape_of(ctx, op->operand(0U)->type());
                const TypeId sr  = shape_of(ctx, res);

                // (3) per-op shape checks.
                if (is_ew)
                {
                    const TypeId       s1 = shape_of(ctx, op->operand(1U)->type());
                    const AttrValue    fn = ctx.attr_value(op->attr(containers::StringView("fn")));
                    if (fn.kind != AttrKind::String || !fn_in(fn.s, /*reduce=*/false))
                    {
                        return {nullptr, op, TensorMisuseKind::FnInvalid, -1};
                    }
                    const BroadcastResult br = ctx.shapes_broadcast(s0, s1);
                    if (br.compat == ShapeCompat::Incompatible)
                    {
                        return {op->operand(1U), op, TensorMisuseKind::ShapeBroadcastIncompatible, static_cast<i32>(br.position)};
                    }
                    if (br.compat == ShapeCompat::Compatible)
                    {
                        const TypeId exp = ctx.shapes_broadcast_result(s0, s1);
                        if (exp.valid() && sr != exp) { return {op->result(0U), op, TensorMisuseKind::BroadcastResultMismatch, -1}; }
                    }
                }
                else if (is_bc)
                {
                    // the input must broadcast UP to the result shape ⇒ broadcast(input, result) == result.
                    const BroadcastResult br = ctx.shapes_broadcast(s0, sr);
                    if (br.compat == ShapeCompat::Incompatible)
                    {
                        return {op->operand(0U), op, TensorMisuseKind::ShapeBroadcastIncompatible, static_cast<i32>(br.position)};
                    }
                    if (br.compat == ShapeCompat::Compatible)
                    {
                        const TypeId exp = ctx.shapes_broadcast_result(s0, sr);
                        if (exp.valid() && exp != sr) { return {op->result(0U), op, TensorMisuseKind::BroadcastResultMismatch, -1}; }
                    }
                }
                else if (is_rs)
                {
                    if (ctx.shapes_reshape(s0, sr) == ShapeCompat::Incompatible)
                    {
                        return {op->result(0U), op, TensorMisuseKind::ShapeReshapeIncompatible, -1};
                    }
                }
                else if (is_tr)
                {
                    const usize     rank = shape_rank(ctx, s0);
                    const AttrValue pv   = ctx.attr_value(op->attr(containers::StringView("perm")));
                    i64             perm[kMaxRank];
                    u32             pc = 0U;
                    if (pv.kind != AttrKind::String || !parse_int_list(pv.s, perm, kMaxRank, pc) || !is_permutation(perm, pc, rank))
                    {
                        return {nullptr, op, TensorMisuseKind::PermInvalid, -1};
                    }
                    const TypeId exp = permute_shape(ctx, s0, perm, pc);
                    if (exp.valid() && sr != exp) { return {op->result(0U), op, TensorMisuseKind::TransposeResultMismatch, -1}; }
                }
                else if (is_rd)
                {
                    const usize     rank = shape_rank(ctx, s0);
                    const AttrValue ax   = ctx.attr_value(op->attr(containers::StringView("axis")));
                    const i64       axis = (ax.kind == AttrKind::Int) ? ax.i : -1;
                    if (axis < 0 || axis >= static_cast<i64>(rank)) { return {nullptr, op, TensorMisuseKind::AxisInvalid, -1}; }
                    const AttrValue fn = ctx.attr_value(op->attr(containers::StringView("fn")));
                    if (fn.kind != AttrKind::String || !fn_in(fn.s, /*reduce=*/true))
                    {
                        return {nullptr, op, TensorMisuseKind::FnInvalid, -1};
                    }
                    const TypeId exp = drop_axis_shape(ctx, s0, static_cast<usize>(axis));
                    if (exp.valid() && sr != exp) { return {op->result(0U), op, TensorMisuseKind::ReduceResultMismatch, -1}; }
                }
                else if (is_mm)
                {
                    const TypeId s1 = shape_of(ctx, op->operand(1U)->type());
                    const usize  rl = shape_rank(ctx, s0);
                    const usize  rr = shape_rank(ctx, s1);
                    if (rl < 2U || rr < 2U) { return {nullptr, op, TensorMisuseKind::MatmulRankInvalid, -1}; }
                    // contraction: lhs's last axis (K) == rhs's second-to-last axis (K). Static-differ ⇒ mismatch; a dynamic/
                    // symbolic-differ pair is Unknown ⇒ ACCEPT (deferred). TypeId equality covers static-equal + same-symbolic.
                    const TypeId kl = ctx.type_of(s0).members[rl - 1U];
                    const TypeId kr = ctx.type_of(s1).members[rr - 2U];
                    if (kl != kr)
                    {
                        const Type dl = ctx.type_of(kl);
                        const Type dr = ctx.type_of(kr);
                        const bool both_static = static_cast<DimKind>(dl.cols) == DimKind::Static
                                                 && static_cast<DimKind>(dr.cols) == DimKind::Static;
                        if (both_static) { return {op->operand(1U), op, TensorMisuseKind::ContractionMismatch, -1}; }
                    }
                    // BATCH dims (the leading rank-2 axes) must AGREE right-aligned — matmul_shape copies LHS's batch, so an
                    // UNCHECKED rhs batch would false-green a mismatch (advisor). Static-differ ⇒ misuse; else defer (the K rule).
                    const usize nbl = rl - 2U;
                    const usize nbr = rr - 2U;
                    const usize nb  = nbl < nbr ? nbl : nbr;
                    for (usize i = 0; i < nb; ++i)
                    {
                        const TypeId bl = ctx.type_of(s0).members[nbl - 1U - i];
                        const TypeId br = ctx.type_of(s1).members[nbr - 1U - i];
                        if (bl != br
                            && static_cast<DimKind>(ctx.type_of(bl).cols) == DimKind::Static
                            && static_cast<DimKind>(ctx.type_of(br).cols) == DimKind::Static)
                        {
                            return {op->operand(1U), op, TensorMisuseKind::BatchMismatch, -1};
                        }
                    }
                    const TypeId exp = matmul_shape(ctx, s0, s1);
                    if (exp.valid() && sr != exp) { return {op->result(0U), op, TensorMisuseKind::MatmulResultMismatch, -1}; }
                }
                else if (is_fft)
                {
                    // The second result im_out (result(1)): Tensor-kinded + element == e0 (re_in/im_in/re_out handled by the
                    // generic kind+element pass above — the split-complex re/im share re_in's element). ⛔ DEFENSIVE: under the
                    // single-result-type model (create_operation + the printer emit ONE result type for all results) im_out's
                    // type is ALWAYS == re_out's, so these three checks are redundant-but-robust — they only fire for a future
                    // per-result-type construction path or a corrupted blob (no independent malformed test is constructible).
                    const Value* const im_out = op->result(1U);
                    if (ctx.type_of(im_out->type()).kind != TypeKind::Tensor)
                    {
                        return {im_out, op, TensorMisuseKind::ResultNotTensor, -1};
                    }
                    if (elem_of(ctx, im_out->type()) != e0) { return {im_out, op, TensorMisuseKind::TensorElementMismatch, -1}; }
                    // split-complex: re_in.shape == im_in.shape.
                    const TypeId s1 = shape_of(ctx, op->operand(1U)->type());
                    if (s0 != s1) { return {op->operand(1U), op, TensorMisuseKind::FftInputShapeMismatch, -1}; }
                    // a c2c FFT PRESERVES shape: re_out.shape == im_out.shape == re_in.shape.
                    if (sr != s0) { return {op->result(0U), op, TensorMisuseKind::FftResultShapeMismatch, -1}; }
                    if (shape_of(ctx, im_out->type()) != s0) { return {im_out, op, TensorMisuseKind::FftResultShapeMismatch, -1}; }
                    // axis in [0, rank).
                    const usize     rank = shape_rank(ctx, s0);
                    const AttrValue ax   = ctx.attr_value(op->attr(containers::StringView("axis")));
                    const i64       axis = (ax.kind == AttrKind::Int) ? ax.i : -1;
                    if (axis < 0 || axis >= static_cast<i64>(rank)) { return {nullptr, op, TensorMisuseKind::AxisInvalid, -1}; }
                    // direction closed vocab {forward, inverse}.
                    const AttrValue dir = ctx.attr_value(op->attr(containers::StringView("direction")));
                    if (dir.kind != AttrKind::String
                        || (dir.s != containers::StringView("forward") && dir.s != containers::StringView("inverse")))
                    {
                        return {nullptr, op, TensorMisuseKind::FftDirectionInvalid, -1};
                    }
                }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const TensorMisuse e = scan_tensor_region(ctx, op->region(i));
                if (e.kind != TensorMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    return register_tensor_ops(ctx); // generated: the tensor dialect + its six ops (idempotent). No type-classes (Tensor is 3d).
}

containers::StringView tensor_misuse_kind_name(TensorMisuseKind k) noexcept
{
    switch (k)
    {
    case TensorMisuseKind::None: return containers::StringView("none");
    case TensorMisuseKind::OperandNotTensor: return containers::StringView("operand-not-tensor");
    case TensorMisuseKind::ResultNotTensor: return containers::StringView("result-not-tensor");
    case TensorMisuseKind::TensorElementMismatch: return containers::StringView("tensor-element-mismatch");
    case TensorMisuseKind::FnInvalid: return containers::StringView("fn-invalid");
    case TensorMisuseKind::ShapeBroadcastIncompatible: return containers::StringView("shape-broadcast-incompatible");
    case TensorMisuseKind::BroadcastResultMismatch: return containers::StringView("broadcast-result-mismatch");
    case TensorMisuseKind::ShapeReshapeIncompatible: return containers::StringView("shape-reshape-incompatible");
    case TensorMisuseKind::PermInvalid: return containers::StringView("perm-invalid");
    case TensorMisuseKind::TransposeResultMismatch: return containers::StringView("transpose-result-mismatch");
    case TensorMisuseKind::AxisInvalid: return containers::StringView("axis-invalid");
    case TensorMisuseKind::ReduceResultMismatch: return containers::StringView("reduce-result-mismatch");
    case TensorMisuseKind::MatmulRankInvalid: return containers::StringView("matmul-rank-invalid");
    case TensorMisuseKind::ContractionMismatch: return containers::StringView("contraction-mismatch");
    case TensorMisuseKind::MatmulResultMismatch: return containers::StringView("matmul-result-mismatch");
    case TensorMisuseKind::BatchMismatch: return containers::StringView("batch-mismatch");
    case TensorMisuseKind::FftInputShapeMismatch: return containers::StringView("fft-input-shape-mismatch");
    case TensorMisuseKind::FftResultShapeMismatch: return containers::StringView("fft-result-shape-mismatch");
    case TensorMisuseKind::FftDirectionInvalid: return containers::StringView("fft-direction-invalid");
    }
    return containers::StringView("?");
}

TensorMisuse find_tensor_misuse(Context& ctx, const Module& m) { return scan_tensor_region(ctx, m.body()); }
} // namespace crd::ceir::tensor
