#include <crd/ceir/gpu/expand_ml.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/gen/linalg_ops.hpp> // linalg::build_gemm
#include <crd/ceir/gen/tensor_ops.hpp> // CEIR-26d-3: tensor::build_transpose (attention Kᵀ via the shape-generic synth path)
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
// CEIR-26d-3: tensor.transpose(src) {perm="1,0"} : dst_t — the rank-2 axis swap, the SHAPE-GENERIC synth path (StageKind::Transpose
// → synth_transpose/emit_permute, proven device-resident at 25b-4a). Replaces the baked transpose.ckir dispatch in attention so
// Kᵀ works at ANY (Sk,D) with ZERO baked-shape constraint (mirrors grad.cpp::mk_transpose2d). ⛔ `perm` is a comma-separated digit
// STRING (find_tensor_misuse parses it via parse_int_list), NOT an int array.
[[nodiscard]] Value* mk_transpose2d(Context& ctx, Block* blk, Operation* at, Value* src, TypeId dst_t)
{
    Operation* const t = tensor::build_transpose(ctx, src, ctx.attr_string(StringView("1,0")), dst_t);
    blk->insert_before(t, at);
    return t->result(0U);
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

    // ⛔ (pre-check, BEFORE emitting any ops) each weight rank-2. ⭐ CEIR-26d: the relu'd-intermediate `M·hidden == 32` reject is
    //    RETIRED — relu.ckir now ships the SHAPE SENTINEL (local_size=0) and the VizDispatch resolver cook-binds local_size to the
    //    intermediate's numel (bind_authored_local_size, tensor_pipeline.hpp) before emit, so an ml.mlp of ANY hidden width whose
    //    relu'd intermediate ≤ the device single-workgroup cap runs device-resident; an OVERSIZE numel is a resolver
    //    UnresolvedKernel (KernelShapeError::LocalSizeExceedsLimit), NOT a pre-emission reject (the width is not known to be
    //    device-illegal here — the cap is device-dependent, known only at resolve = cook). ⛔ CEIR-26d-3c: ml.attention is ALSO
    //    dimension-general now (transpose→synth, softmax→spec-const loop) — NO baked-shape reject remains in expand_ml (the
    //    BakedKernelShapeUnsupported enum value is DEAD-marked + KEPT at 26d-4d, append-only per the enum head — never returned).
    for (u32 i = 1U; i <= nw; ++i)
    {
        if (!is_tensor(ctx, op->operand(i)) || rank_of(ctx, op->operand(i)->type()) != 2U) { return MlExpandError::ShapeRankInvalid; }
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
    // ⛔ CEIR-26d-3c: the baked-kernel pre-check (was: reject any dims ≠ Sq=2,Sk=3,D=4) is RETIRED — 26d-3a made Kᵀ a shape-generic
    //    tensor.transpose (synth_transpose) and 26d-3b made softmax a spec-const loop kernel (local_size←Sq, Sk←spec-const), so the
    //    composite is dimension-general (any Sq/Sk/Dv; the two gemms are synth'd per-shape). Proven device-resident at generic dims
    //    (Sq=3,Sk=5,D=4) on both backends by the 26d-3c gates. The teeth moved to those POSITIVE gates.
    const TypeId elem = elem_of(ctx, q->type());
    Block* const blk  = op->parent_block();
    Value* const grid = mk_const1(ctx, blk, op);

    // Kt = transpose(K) : [D, Sk] — CEIR-26d-3: the SHAPE-GENERIC tensor.transpose (StageKind::Transpose → synth_transpose), NOT
    // the baked transpose.ckir dispatch — so Kᵀ carries no (Sk,D) baked constraint (the transpose leg of the 24z attention reject
    // retired; softmax's Sq/Sk baked-ness is 26d-3b).
    const TypeId kt_t = tensor2(ctx, elem, dim_of(ctx, q->type(), 1U), dim_of(ctx, k->type(), 0U)); // [D, Sk]
    Value* const kt   = mk_transpose2d(ctx, blk, op, k, kt_t);

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

namespace
{
// The shared driver: expand every ml op in pre-order; when `lineage != nullptr`, record each newly-created op → the 0-based
// pre-order index of its source ml op (the created ops of one expansion are exactly the ops inserted between `before` and
// `after` — every mk_* does insert_before(new, ml_op), then the ml op is erased). The index aligns with MlPartition::assignments.
[[nodiscard]] MlExpandResult expand_ml_ops_impl(Context& ctx, Module& m, containers::HashMap<const Operation*, crd::i32>* lineage)
{
    MlExpandResult res;
    crd::i32       ml_idx = 0;
    for (;;)
    {
        Operation* const op = find_first_ml(ctx, m.body());
        if (op == nullptr) { break; }
        Block* const     blk    = op->parent_block();
        Operation* const before = op->prev_in_block(); // stable across the expand (created ops land between before and after)
        Operation* const after  = op->next_in_block();
        const MlExpandError err  = expand_ml_op(ctx, op); // inserts created ops before op, RAUW its result, erase op
        if (err != MlExpandError::None)
        {
            res.error    = err;
            res.error_op = op;
            return res; // ⛔ stop on the first error — the un-expanded op would otherwise loop forever.
        }
        if (lineage != nullptr && blk != nullptr)
        {
            Operation* const start = (before != nullptr) ? before->next_in_block() : blk->first_op();
            for (Operation* c = start; c != nullptr && c != after; c = c->next_in_block()) { (void)lineage->insert(c, ml_idx); }
        }
        ++ml_idx;
        ++res.expanded;
    }
    return res;
}
} // namespace

MlExpandResult expand_ml_ops(Context& ctx, Module& m) { return expand_ml_ops_impl(ctx, m, nullptr); }

MlExpandResult expand_ml_ops(Context& ctx, Module& m, containers::HashMap<const Operation*, crd::i32>& lineage)
{
    lineage.clear(); // documented: cleared first, so a reused map never carries a stale (op → index) from a prior expand
    return expand_ml_ops_impl(ctx, m, &lineage);
}
} // namespace crd::ceir::gpu
