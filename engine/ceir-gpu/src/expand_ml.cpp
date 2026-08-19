#include <crd/ceir/gpu/expand_ml.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/gen/linalg_ops.hpp> // linalg::build_gemm
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

#include <crd/containers/span.hpp>

namespace crd::ceir::gpu
{
namespace
{
using containers::ConstSpan;
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
// The dim TypeId at `axis` of tensor `t` (reused verbatim so a dynamic dim survives), or {} if out of range.
[[nodiscard]] TypeId dim_of(const Context& ctx, TypeId t, usize axis) noexcept
{
    const Type sh = ctx.type_of(shape_of(ctx, t));
    return axis < sh.members.size() ? sh.members[axis] : TypeId{};
}
// The STATIC extent of tensor `t`'s dim `axis`, or 0 if absent / dynamic (0 ⇒ a baked-kernel-shape check conservatively rejects).
[[nodiscard]] crd::u32 dim_count(const Context& ctx, TypeId t, usize axis) noexcept
{
    const Type sh = ctx.type_of(shape_of(ctx, t));
    if (axis >= sh.members.size()) { return 0U; }
    const Type d = ctx.type_of(sh.members[axis]);
    return static_cast<DimKind>(d.cols) == DimKind::Static ? d.count : 0U;
}
// tensor<elem, [dim_a, dim_b]> — a rank-2 tensor from two dim TypeIds.
[[nodiscard]] TypeId tensor2(Context& ctx, TypeId elem, TypeId dim_a, TypeId dim_b)
{
    const TypeId dims[2] = {dim_a, dim_b};
    return ctx.type_tensor(elem, ctx.type_shape(ConstSpan<TypeId>(dims, 2U)));
}

// resource.declare : t (a fresh SSA buffer), inserted before `at`.
[[nodiscard]] Value* mk_decl(Context& ctx, Block* blk, Operation* at, TypeId t)
{
    Operation* const d = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, t);
    blk->insert_before(d, at);
    return d->result(0U);
}
// arith.const {value=1} : index — a dispatch grid operand (grid 1,1,1; the one-workgroup convention), inserted before `at`.
[[nodiscard]] Value* mk_const1(Context& ctx, Block* blk, Operation* at)
{
    Operation* const c = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_index());
    ctx.set_attr(c, StringView("value"), ctx.attr_int(1));
    blk->insert_before(c, at);
    return c->result(0U);
}
// linalg.gemm(a, b, c) {alpha=1, beta=0, no-transpose} : out_t — the PLAIN gemm (the synth/plan envelope). `c` is a fresh
// resource.declare (the β·C term; β=0 ⇒ ignored, but the op carries the operand). Inserted before `at`; returns the result value.
[[nodiscard]] Value* mk_gemm(Context& ctx, Block* blk, Operation* at, Value* a, Value* b, TypeId out_t)
{
    Value* const     c = mk_decl(ctx, blk, at, out_t);
    Operation* const g = linalg::build_gemm(ctx, a, b, c, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                            ctx.attr_bool(false), out_t);
    blk->insert_before(g, at);
    return g->result(0U);
}
// compute.dispatch(grid, grid, grid, binds...) {kernel=@kernel, access} — RESULTLESS, inserted before `at`. `binds`/`nb` are
// the SLOT-order bindings (inputs then outputs, the 13a rule); `access` is the matching per-binding r/w token string.
void mk_dispatch(Context& ctx, Block* blk, Operation* at, Value* grid, Value* const* binds, u32 nb, StringView kernel,
                 StringView access)
{
    Value* ops[3U + 8U] = {};
    ops[0] = grid;
    ops[1] = grid;
    ops[2] = grid;
    for (u32 i = 0; i < nb; ++i) { ops[3U + i] = binds[i]; }
    Operation* const op = ctx.create_operation(ctx.intern_op("compute", "dispatch"), ConstSpan<Value*>(ops, 3U + nb), 0U);
    ctx.set_attr(op, StringView("kernel"), ctx.attr_symbol(kernel));
    ctx.set_attr(op, StringView("access"), ctx.attr_string(access));
    blk->insert_before(op, at);
}

// ml.mlp(x, W_1..W_n) {activation=relu} -> h = x; h_i = gemm(h_{i-1}, W_i); h = (i<n) ? relu(h_i) : h_i. FLOAT-only. Returns
// the final gemm result to RAUW the ml.mlp result with; or a typed error.
[[nodiscard]] MlExpandError expand_mlp(Context& ctx, Operation* op, Value*& out)
{
    if (op->num_operands() < 2U || op->num_results() == 0U || !is_tensor(ctx, op->operand(0U))
        || !is_tensor(ctx, op->result(0U)))
    {
        return MlExpandError::OperandNotTensor;
    }
    Value* const input = op->operand(0U);
    const u32    nw    = op->num_operands() - 1U; // number of weight matrices
    const TypeId elem  = elem_of(ctx, input->type());
    if (!is_float_elem(ctx, input->type())) { return MlExpandError::ElementNotFloat; }
    if (rank_of(ctx, input->type()) != 2U) { return MlExpandError::ShapeRankInvalid; }

    // ⛔ (baked-kernel pre-check, BEFORE emitting any ops) each weight rank-2, AND every relu'd intermediate (layers 1..nw-1) has
    //    M·hidden == 32 — relu.ckir bakes local_size=32 with NO bound guard, so a larger intermediate would leave an
    //    UNINITIALIZED tail (and a smaller one an OOB write) feeding the next gemm. A TYPED reject, never a silent miscompile
    //    (the UnsupportedQuantScheme precedent). Dimension-general relu = name-forward (the 24z ledger).
    for (u32 i = 1U; i <= nw; ++i)
    {
        if (!is_tensor(ctx, op->operand(i)) || rank_of(ctx, op->operand(i)->type()) != 2U) { return MlExpandError::ShapeRankInvalid; }
        if (i < nw && dim_count(ctx, input->type(), 0U) * dim_count(ctx, op->operand(i)->type(), 1U) != 32U)
        {
            return MlExpandError::BakedKernelShapeUnsupported;
        }
    }

    Block* const blk  = op->parent_block();
    Value* const grid = mk_const1(ctx, blk, op);
    Value*       prev = input;
    for (u32 i = 1; i <= nw; ++i)
    {
        Value* const wi = op->operand(i);
        if (!is_tensor(ctx, wi) || rank_of(ctx, wi->type()) != 2U) { return MlExpandError::ShapeRankInvalid; }
        // The layer output type: [rows(prev), cols(W_i)]; the FINAL layer uses the ml.mlp's declared result type (RAUW-exact).
        const TypeId h_t = (i < nw) ? tensor2(ctx, elem, dim_of(ctx, prev->type(), 0U), dim_of(ctx, wi->type(), 1U))
                                    : op->result(0U)->type();
        Value* const h   = mk_gemm(ctx, blk, op, prev, wi, h_t);
        if (i < nw)
        {
            Value* const relu_out = mk_decl(ctx, blk, op, h_t);
            Value* const binds[2] = {h, relu_out};
            mk_dispatch(ctx, blk, op, grid, binds, 2U, StringView("relu"), StringView("r,w"));
            prev = relu_out;
        }
        else { prev = h; }
    }
    out = prev;
    return MlExpandError::None;
}

// ml.attention(Q, K, V) -> Kt = transpose(K); scores = gemm(Q, Kt); probs = softmax(scores, scale=1/√D); out = gemm(probs, V).
[[nodiscard]] MlExpandError expand_attention(Context& ctx, Operation* op, Value*& out)
{
    if (op->num_operands() < 3U || op->num_results() == 0U || !is_tensor(ctx, op->operand(0U))
        || !is_tensor(ctx, op->operand(1U)) || !is_tensor(ctx, op->operand(2U)) || !is_tensor(ctx, op->result(0U)))
    {
        return MlExpandError::OperandNotTensor;
    }
    Value* const q = op->operand(0U); // Q [Sq, D]
    Value* const k = op->operand(1U); // K [Sk, D]
    Value* const v = op->operand(2U); // V [Sk, Dv]
    if (!is_float_elem(ctx, q->type())) { return MlExpandError::ElementNotFloat; }
    if (rank_of(ctx, q->type()) != 2U || rank_of(ctx, k->type()) != 2U || rank_of(ctx, v->type()) != 2U)
    {
        return MlExpandError::ShapeRankInvalid;
    }
    // ⛔ (baked-kernel pre-check) transpose.ckir bakes (Sk=3, D=4) + softmax.ckir bakes (Sq=2, Sk=3) — reject ANY other dims (a
    //    TYPED reject, never a silent wrong-shape kernel). Dv stays free (the two gemms are synth'd per-shape). Dimension-general
    //    transpose/softmax = name-forward (the 24z ledger).
    if (dim_count(ctx, q->type(), 0U) != 2U || dim_count(ctx, k->type(), 0U) != 3U || dim_count(ctx, q->type(), 1U) != 4U)
    {
        return MlExpandError::BakedKernelShapeUnsupported;
    }
    const TypeId elem = elem_of(ctx, q->type());
    Block* const blk  = op->parent_block();
    Value* const grid = mk_const1(ctx, blk, op);

    // Kt = transpose(K) : [D, Sk] (the head-dim × seq transpose the plain gemm(Q, Kt) needs).
    const TypeId kt_t  = tensor2(ctx, elem, dim_of(ctx, q->type(), 1U), dim_of(ctx, k->type(), 0U)); // [D, Sk]
    Value* const kt    = mk_decl(ctx, blk, op, kt_t);
    Value* const tb[2] = {k, kt};
    mk_dispatch(ctx, blk, op, grid, tb, 2U, StringView("transpose"), StringView("r,w"));

    // scores = gemm(Q, Kt) : [Sq, Sk].
    const TypeId scores_t = tensor2(ctx, elem, dim_of(ctx, q->type(), 0U), dim_of(ctx, k->type(), 0U)); // [Sq, Sk]
    Value* const scores   = mk_gemm(ctx, blk, op, q, kt, scores_t);

    // probs = softmax(scores, scale) : [Sq, Sk]. scale : [1] is a CALLER-UPLOADED buffer (1/√D; the quant_mlp dequant-scale mold).
    const TypeId d1        = ctx.type_dim_static(1U);
    const TypeId scale_t   = ctx.type_tensor(elem, ctx.type_shape(ConstSpan<TypeId>(&d1, 1U)));
    Value* const scale     = mk_decl(ctx, blk, op, scale_t);
    Value* const probs     = mk_decl(ctx, blk, op, scores_t);
    Value* const sb[3]     = {scores, scale, probs};
    mk_dispatch(ctx, blk, op, grid, sb, 3U, StringView("softmax"), StringView("r,r,w"));

    // out = gemm(probs, V) : [Sq, Dv] == the ml.attention result type (RAUW-exact).
    out = mk_gemm(ctx, blk, op, probs, v, op->result(0U)->type());
    return MlExpandError::None;
}

// The FIRST ml.mlp / ml.attention op in `r` (pre-order), or null.
[[nodiscard]] Operation* find_first_ml(const Context& ctx, Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return nullptr; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const StringView nm = ctx.op_name(op->kind());
            if (nm == StringView("ml.mlp") || nm == StringView("ml.attention")) { return op; }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                Operation* const f = find_first_ml(ctx, op->region(i));
                if (f != nullptr) { return f; }
            }
        }
    }
    return nullptr;
}
} // namespace

MlExpandError expand_ml_op(Context& ctx, Operation* op)
{
    if (op == nullptr) { return MlExpandError::OperandNotTensor; }
    const StringView nm  = ctx.op_name(op->kind());
    Value*           out = nullptr;
    MlExpandError    err = MlExpandError::None;
    if (nm == StringView("ml.mlp")) { err = expand_mlp(ctx, op, out); }
    else if (nm == StringView("ml.attention")) { err = expand_attention(ctx, op, out); }
    else { return MlExpandError::OperandNotTensor; } // not an ml op — a no-op miss (never silently succeed)
    if (err != MlExpandError::None) { return err; }
    op->result(0U)->replace_all_uses_with(out);
    op->erase();
    return MlExpandError::None;
}

MlExpandResult expand_ml_ops(Context& ctx, Module& m)
{
    MlExpandResult res;
    for (;;)
    {
        Operation* const op = find_first_ml(ctx, m.body());
        if (op == nullptr) { break; }
        const MlExpandError err = expand_ml_op(ctx, op);
        if (err != MlExpandError::None)
        {
            res.error    = err;
            res.error_op = op;
            return res; // ⛔ stop on the first error — the un-expanded op would otherwise loop forever.
        }
        ++res.expanded;
    }
    return res;
}
} // namespace crd::ceir::gpu
