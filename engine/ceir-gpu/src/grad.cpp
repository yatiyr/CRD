#include <crd/ceir/gpu/grad.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/gen/linalg_ops.hpp> // linalg::build_gemm
#include <crd/ceir/gen/tensor_ops.hpp> // tensor::build_transpose
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>

namespace crd::ceir::gpu
{
namespace
{
using containers::ConstSpan;
using containers::StringView;

// OpId.value for a compute.dispatch op — id.hpp pins OpId = FNV-1a of "dialect.op", so this compile-time constant lets
// the const `lookup` recognize a dispatch without a non-const intern_op / a per-op string compare.
constexpr crd::u64 kDispatchOp = fnv1a_ct("compute.dispatch");

[[nodiscard]] bool   is_tensor(const Context& ctx, const Value* v) noexcept
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
[[nodiscard]] usize rank_of(const Context& ctx, TypeId t) noexcept
{
    return ctx.type_of(shape_of(ctx, t)).members.size();
}
[[nodiscard]] bool   is_float_elem(const Context& ctx, TypeId t) noexcept
{
    return ctx.type_of(elem_of(ctx, t)).kind == TypeKind::Float;
}
[[nodiscard]] TypeId dim_of(const Context& ctx, TypeId t, usize axis) noexcept
{
    const Type sh = ctx.type_of(shape_of(ctx, t));
    return axis < sh.members.size() ? sh.members[axis] : TypeId{};
}
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
// tensor.transpose(src) {perm=[1,0]} : dst_t — the rank-2 transpose the plain-gemm operands need, inserted before `at`.
[[nodiscard]] Value* mk_transpose2d(Context& ctx, Block* blk, Operation* at, Value* src, TypeId dst_t)
{
    // ⛔ find_tensor_misuse parses `perm` as a comma-separated digit STRING (parse_int_list), NOT an int array.
    const AttrId     perm = ctx.attr_string(StringView("1,0")); // the rank-2 axis swap
    Operation* const t    = tensor::build_transpose(ctx, src, perm, dst_t);
    blk->insert_before(t, at);
    return t->result(0U);
}
// linalg.gemm(a, b, c) {alpha=1, beta=0, no-transpose} : out_t — the PLAIN gemm (the synth/plan envelope); `c` is a fresh
// declare (β=0 ⇒ ignored, but the op carries the operand). Inserted before `at`.
[[nodiscard]] Value* mk_gemm(Context& ctx, Block* blk, Operation* at, Value* a, Value* b, TypeId out_t)
{
    Value* const     c = mk_decl(ctx, blk, at, out_t);
    Operation* const g = linalg::build_gemm(ctx, a, b, c, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                            ctx.attr_bool(false), ctx.attr_bool(false), out_t);
    blk->insert_before(g, at);
    return g->result(0U);
}
// tensor.reshape(src) : dst_t — re-insert the reduced axis' size-1, inserted before `at`.
[[nodiscard]] Value* mk_reshape(Context& ctx, Block* blk, Operation* at, Value* src, TypeId dst_t)
{
    Operation* const op = tensor::build_reshape(ctx, src, dst_t);
    blk->insert_before(op, at);
    return op->result(0U);
}
// tensor.broadcast(src) : dst_t — expand along the (size-1) axes, inserted before `at`.
[[nodiscard]] Value* mk_broadcast(Context& ctx, Block* blk, Operation* at, Value* src, TypeId dst_t)
{
    Operation* const op = tensor::build_broadcast(ctx, src, dst_t);
    blk->insert_before(op, at);
    return op->result(0U);
}
// tensor.elementwise(a, b){fn=add} : dst_t — the reverse-mode adjoint SUM for a value with two consumers, before `at`.
[[nodiscard]] Value* mk_add(Context& ctx, Block* blk, Operation* at, Value* a, Value* b, TypeId dst_t)
{
    Operation* const op = tensor::build_elementwise(ctx, a, b, ctx.attr_string(StringView("add")), dst_t);
    blk->insert_before(op, at);
    return op->result(0U);
}
// arith.const {value=1} : index — the one-workgroup dispatch grid operand (relu/relu_vjp bake local_size=32), before `at`.
[[nodiscard]] Value* mk_const1(Context& ctx, Block* blk, Operation* at)
{
    Operation* const c = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_index());
    ctx.set_attr(c, StringView("value"), ctx.attr_int(1));
    blk->insert_before(c, at);
    return c->result(0U);
}
// compute.dispatch(grid,grid,grid, binds...) {kernel=@kernel, access} — RESULTLESS, before `at` (the expand_ml mk_dispatch mold).
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
// compute.dispatch(@relu_vjp, {x, gy, gx} r,r,w) : gx (a fresh declare of x's type) — the authored relu_vjp.ckir dispatch (25c-0).
// gx[i] = (x[i] > 0) ? gy[i] : 0, x the forward PRE-activation. Returns the operand-adjoint value the backward chain threads on.
[[nodiscard]] Value* emit_relu_vjp_dispatch(Context& ctx, Block* blk, Operation* at, Value* grid, Value* x, Value* gy)
{
    Value* const gx       = mk_decl(ctx, blk, at, x->type());
    Value* const binds[3] = {x, gy, gx};
    mk_dispatch(ctx, blk, at, grid, binds, 3U, StringView("relu_vjp"), StringView("r,r,w"));
    return gx;
}
} // namespace

StringView grad_error_name(GradError e) noexcept
{
    switch (e)
    {
    case GradError::None:             return StringView("None");
    case GradError::LossNotScalar:    return StringView("LossNotScalar");
    case GradError::WrtNotTensor:     return StringView("WrtNotTensor");
    case GradError::OperandNotTensor: return StringView("OperandNotTensor");
    case GradError::ElementNotFloat:  return StringView("ElementNotFloat");
    case GradError::ShapeRankInvalid: return StringView("ShapeRankInvalid");
    case GradError::MissingVjp:       return StringView("MissingVjp");
    case GradError::ReduceFnUnsupported: return StringView("ReduceFnUnsupported");
    case GradError::ArityUnsupported:    return StringView("ArityUnsupported");
    case GradError::MlpActivationUnsupported: return StringView("MlpActivationUnsupported");
    case GradError::MlpBakedShapeUnsupported: return StringView("MlpBakedShapeUnsupported");
    }
    return StringView("");
}

VjpRegistry::VjpRegistry(memory::IAllocator* alloc) : m_op_rules(alloc), m_kernel_rules(alloc) {}

void VjpRegistry::register_op(OpId op, VjpRuleFn fn) { m_op_rules.insert(op.value, fn); }
void VjpRegistry::register_kernel(StringView kernel, VjpRuleFn fn)
{
    m_kernel_rules.insert(containers::fnv1a_64(kernel.data(), kernel.size()), fn);
}

VjpRuleFn VjpRegistry::lookup(const Context& ctx, const Operation* op) const noexcept
{
    if (op == nullptr) { return nullptr; }
    const OpId kind = op->kind();
    if (kind.value == kDispatchOp)
    {
        const AttrId a = op->attr(StringView("kernel"));
        if (!a.valid()) { return nullptr; } // absent kernel ⇒ MissingVjp (never a fall-through to an op-kind rule)
        const AttrValue v = ctx.attr_value(a);
        if (v.kind != AttrKind::SymbolRef) { return nullptr; }
        const VjpRuleFn* const f = m_kernel_rules.find(containers::fnv1a_64(v.s.data(), v.s.size()));
        return f != nullptr ? *f : nullptr;
    }
    const VjpRuleFn* const f = m_op_rules.find(kind.value);
    return f != nullptr ? *f : nullptr;
}

GradError vjp_gemm(Context& ctx, const VjpRegistry& /*reg*/, Block* blk, Operation* at, const Operation* fwd,
                   ConstSpan<Value*> result_adjoints, Value** operand_adjoints)
{
    if (fwd == nullptr || fwd->num_operands() < 3U || fwd->num_results() == 0U || result_adjoints.size() < 1U
        || result_adjoints[0] == nullptr)
    {
        return GradError::OperandNotTensor;
    }
    Value* const a  = fwd->operand(0U); // A [M, K]
    Value* const b  = fwd->operand(1U); // B [K, N]
    Value* const dc = result_adjoints[0]; // dC [M, N]
    if (!is_tensor(ctx, a) || !is_tensor(ctx, b) || !is_tensor(ctx, dc)) { return GradError::OperandNotTensor; }
    if (!is_float_elem(ctx, a->type())) { return GradError::ElementNotFloat; }
    if (rank_of(ctx, a->type()) != 2U || rank_of(ctx, b->type()) != 2U || rank_of(ctx, dc->type()) != 2U)
    {
        return GradError::ShapeRankInvalid;
    }
    const TypeId elem = elem_of(ctx, a->type());
    // Bᵀ : [N, K] (transpose of B[K, N]) ; Aᵀ : [K, M] (transpose of A[M, K]).
    const TypeId bt_t = tensor2(ctx, elem, dim_of(ctx, b->type(), 1U), dim_of(ctx, b->type(), 0U));
    const TypeId at_t = tensor2(ctx, elem, dim_of(ctx, a->type(), 1U), dim_of(ctx, a->type(), 0U));
    Value* const bt   = mk_transpose2d(ctx, blk, at, b, bt_t);
    Value* const atv  = mk_transpose2d(ctx, blk, at, a, at_t);
    // dA = gemm(dC[M,N], Bᵀ[N,K]) : [M,K] == A's type ; dB = gemm(Aᵀ[K,M], dC[M,N]) : [K,N] == B's type.
    operand_adjoints[0] = mk_gemm(ctx, blk, at, dc, bt, a->type());
    operand_adjoints[1] = mk_gemm(ctx, blk, at, atv, dc, b->type());
    // operand_adjoints[2] (the β·C term) stays the orchestrator's pre-zeroed null — C is not differentiated (β=0).
    return GradError::None;
}

GradError vjp_reduce(Context& ctx, const VjpRegistry& /*reg*/, Block* blk, Operation* at, const Operation* fwd,
                     ConstSpan<Value*> result_adjoints, Value** operand_adjoints)
{
    if (fwd == nullptr || fwd->num_operands() < 1U || fwd->num_results() == 0U || result_adjoints.size() < 1U
        || result_adjoints[0] == nullptr)
    {
        return GradError::OperandNotTensor;
    }
    Value* const input = fwd->operand(0U);
    Value* const dout  = result_adjoints[0];
    if (!is_tensor(ctx, input) || !is_tensor(ctx, dout)) { return GradError::OperandNotTensor; }
    if (!is_float_elem(ctx, input->type())) { return GradError::ElementNotFloat; }
    // sum's VJP is a broadcast (d/dx of a sum is 1); max/min/prod/mean are name-forward — a TYPED reject, not a wrong grad.
    const AttrValue fn = ctx.attr_value(fwd->attr(StringView("fn")));
    if (fn.kind != AttrKind::String || fn.s != StringView("sum")) { return GradError::ReduceFnUnsupported; }
    const AttrValue ax = ctx.attr_value(fwd->attr(StringView("axis")));
    if (ax.kind != AttrKind::Int) { return GradError::ShapeRankInvalid; }
    const TypeId in_t = input->type();
    const usize  rank = rank_of(ctx, in_t);
    if (rank == 0U || rank > 16U || ax.i < 0 || static_cast<usize>(ax.i) >= rank) { return GradError::ShapeRankInvalid; }
    // keepdim shape = input's, with the reduced axis re-inserted at size 1 (dodges the 1D right-align broadcast scar),
    // then broadcast back to the full input shape.
    TypeId dims[16];
    for (usize d = 0; d < rank; ++d)
    {
        dims[d] = (static_cast<i64>(d) == ax.i) ? ctx.type_dim_static(1U) : dim_of(ctx, in_t, d);
    }
    const TypeId keep_t   = ctx.type_tensor(elem_of(ctx, in_t), ctx.type_shape(ConstSpan<TypeId>(dims, rank)));
    Value* const reshaped = mk_reshape(ctx, blk, at, dout, keep_t);
    operand_adjoints[0]   = mk_broadcast(ctx, blk, at, reshaped, in_t);
    return GradError::None;
}

GradResult build_gradient(Context& ctx, Module& m, const VjpRegistry& reg, Value* loss, ConstSpan<Value*> wrt,
                          containers::Span<Value*> grads, memory::IAllocator* scratch)
{
    (void)m;
    GradResult res;
    if (loss == nullptr || loss->defining_op() == nullptr || !is_tensor(ctx, loss))
    {
        res.error = GradError::OperandNotTensor;
        return res;
    }
    Block* const blk = loss->defining_op()->parent_block();
    if (blk == nullptr) { res.error = GradError::OperandNotTensor; return res; }

    // snapshot the FORWARD ops BEFORE emitting any backward op (else the reverse walk re-processes its own output).
    containers::Array<Operation*> fwd(scratch);
    for (Operation* op = blk->first_op(); op != nullptr; op = op->next_in_block()) { fwd.push_back(op); }

    // ── PURE PRE-PASS (no emit): every op on loss's backward reachability set must have a rule + fit the walk's caps, so
    // MissingVjp / ArityUnsupported leave the module byte-identical (the expand_ml "reject before emit" precedent). The
    // reachability OVER-approximates (marks every operand reached, incl. a non-differentiated β·C leaf) — harmless here
    // (such operands are leaf declares); a precise wrt→loss path analysis is name-forward.
    {
        containers::HashMap<const Value*, u8> reached(scratch);
        (void)reached.insert(loss, 1U);
        for (usize idx = fwd.size(); idx-- > 0U;)
        {
            Operation* const op = fwd[idx];
            if (op->num_operands() == 0U) { continue; }
            bool on_path = false;
            for (u32 r = 0; r < op->num_results(); ++r)
            {
                if (reached.find(op->result(r)) != nullptr) { on_path = true; break; }
            }
            if (!on_path) { continue; }
            if (op->num_results() > 4U || op->num_operands() > 8U)
            {
                res.error = GradError::ArityUnsupported;
                res.error_op = op;
                return res;
            }
            if (reg.lookup(ctx, op) == nullptr)
            {
                res.error = GradError::MissingVjp;
                res.error_op = op;
                return res;
            }
            for (u32 i = 0; i < op->num_operands(); ++i) { (void)reached.insert(op->operand(i), 1U); }
        }
    }

    containers::HashMap<const Value*, Value*> adj(scratch); // forward value -> its accumulated adjoint

    // an end anchor so seed + backward ops land AFTER the forward ops; erased before returning.
    Operation* const anchor = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, loss->type());
    blk->append(anchor);

    // seed: dLoss rides a caller-uploaded ExternalIn (resource.declare) of loss's shape (the softmax/quant-scale precedent). The
    // handle rides out on GradResult so an executor can name the buffer to upload ALL-ONES into (the header's caller-seed promise).
    Value* const seed = mk_decl(ctx, blk, anchor, loss->type());
    res.seed          = seed;
    (void)adj.insert(loss, seed);

    for (usize idx = fwd.size(); idx-- > 0U;)
    {
        Operation* const op = fwd[idx];
        if (op->num_operands() == 0U) { continue; } // leaf (declare/const) — its adjoint is a gradient sink, not backprop'd
        const u32 nres = op->num_results() < 4U ? op->num_results() : 4U;
        Value*    radj[4]  = {};
        bool      on_path  = false;
        for (u32 r = 0; r < nres; ++r)
        {
            Value* const* const f = adj.find(op->result(r));
            radj[r]               = (f != nullptr) ? *f : nullptr;
            if (radj[r] != nullptr) { on_path = true; }
        }
        if (!on_path) { continue; } // op does not reach loss
        const VjpRuleFn rule = reg.lookup(ctx, op);
        if (rule == nullptr)
        {
            anchor->erase();
            res.error    = GradError::MissingVjp;
            res.error_op = op;
            return res;
        }
        Value*          oadj[8] = {};
        const u32       nop     = op->num_operands();
        const GradError e = rule(ctx, reg, blk, anchor, op, ConstSpan<Value*>(radj, nres), oadj);
        if (e != GradError::None)
        {
            anchor->erase();
            res.error    = e;
            res.error_op = op;
            return res;
        }
        for (u32 i = 0; i < nop && i < 8U; ++i)
        {
            if (oadj[i] == nullptr) { continue; }
            Value* const        v    = op->operand(i);
            Value* const* const prev = adj.find(v);
            if (prev != nullptr) // a value with two consumers: SUM the partial adjoints (tensor.elementwise{add})
            {
                Value* const acc = mk_add(ctx, blk, anchor, *prev, oadj[i], v->type());
                adj[v]           = acc;
            }
            else { (void)adj.insert(v, oadj[i]); }
        }
    }
    anchor->erase();

    for (usize i = 0; i < wrt.size() && i < grads.size(); ++i)
    {
        if (wrt[i] == nullptr || !is_tensor(ctx, wrt[i])) { res.error = GradError::WrtNotTensor; return res; }
        Value* const* const g = adj.find(wrt[i]);
        grads[i]              = (g != nullptr) ? *g : nullptr; // null ⇒ wrt not reached (zero gradient); the gate tests reached wrt
    }
    return res;
}

GradError vjp_mlp(Context& ctx, const VjpRegistry& /*reg*/, Block* blk, Operation* at, const Operation* fwd,
                  ConstSpan<Value*> result_adjoints, Value** operand_adjoints)
{
    if (fwd == nullptr || fwd->num_operands() < 2U || fwd->num_results() == 0U || result_adjoints.size() < 1U
        || result_adjoints[0] == nullptr)
    {
        return GradError::OperandNotTensor;
    }
    // activation envelope: relu-only (the authored relu_vjp.ckir pair). The ml verifier already restricts to {relu}; the rule
    // re-checks so a non-verify-clean or future-activation module TYPED-rejects instead of silently emitting a wrong gradient.
    const AttrValue act = ctx.attr_value(fwd->attr(StringView("activation")));
    if (act.kind != AttrKind::String || act.s != StringView("relu")) { return GradError::MlpActivationUnsupported; }

    const u32    nw = fwd->num_operands() - 1U; // weight count (>=1, per the ml verifier)
    Value* const x  = fwd->operand(0U);
    if (!is_tensor(ctx, x) || !is_float_elem(ctx, x->type()) || rank_of(ctx, x->type()) != 2U)
    {
        return GradError::OperandNotTensor;
    }
    if (nw > 8U) { return GradError::ArityUnsupported; } // the reverse-walk's fixed depth cap (h/z arrays below)
    const TypeId elem = elem_of(ctx, x->type());
    Value* const dout = result_adjoints[0]; // dOut — the composite result's adjoint

    // ── RECONSTRUCT the interior forward (the checkpointing RECOMPUTE): z_i = gemm(h_{i-1}, W_i); h_i = relu(z_i) for i<nw.
    //    z_nw = h_nw is the LIVE composite result (never re-derived); h_0 = x. Only the INTERIOR z_1..z_{nw-1}, h_1..h_{nw-1}
    //    are re-derived — the values NOT reachable through `fwd` (operands x/W_i ARE live). ──
    // every weight must be rank-2 (the backward reads dim_of(W_i) for ALL i); the ml verifier guarantees it on a clean module.
    for (u32 i = 1U; i <= nw; ++i)
    {
        if (!is_tensor(ctx, fwd->operand(i)) || rank_of(ctx, fwd->operand(i)->type()) != 2U)
        {
            return GradError::ShapeRankInvalid;
        }
    }
    // ⛔ CEIR-26d-4b: the baked-kernel shape guard (was: reject any interior relu'd layer whose M·hidden_i ≠ 32) is RETIRED —
    //    relu.ckir (recompute) + relu_vjp.ckir (backward) now ship the SHAPE SENTINEL (local_size=0) and the VizDispatch resolver
    //    cook-binds local_size to the intermediate's numel (bind_authored_local_size) before emit, so a vjp of an MLP at ANY
    //    interior width ≤ the device single-workgroup cap runs device-resident (an oversize numel is a resolver UnresolvedKernel,
    //    the same LocalSizeExceedsLimit path as the forward). Proven at h1=64 on both backends by the 26d-4b gates.
    Value* const grid = mk_const1(ctx, blk, at);
    Value*       h[9] = {}; // h[i] = post-activation of layer i; h[0] = x. Only the INTERIOR h_1..h_{nw-1} are re-derived.
    Value*       z[9] = {}; // z[i] = pre-activation of layer i; only INTERIOR z_1..z_{nw-1} (z_nw = the LIVE result — never re-derived
                            //   and never used: layer nw has no relu, and dz_nw = dOut is given).
    h[0] = x;
    for (u32 i = 1U; i < nw; ++i) // INTERIOR layers only: each is relu'd, so its z_i (for relu_vjp) + h_i (for the next dW) is needed.
    {
        Value* const wi  = fwd->operand(i);
        const TypeId z_t = tensor2(ctx, elem, dim_of(ctx, h[i - 1U]->type(), 0U), dim_of(ctx, wi->type(), 1U));
        z[i] = mk_gemm(ctx, blk, at, h[i - 1U], wi, z_t);
        Value* const relu_out = mk_decl(ctx, blk, at, z_t);
        Value*       binds[2]  = {z[i], relu_out};
        mk_dispatch(ctx, blk, at, grid, binds, 2U, StringView("relu"), StringView("r,w"));
        h[i] = relu_out;
    }

    // ── BACKWARD (i = nw..1): dz_nw = dOut (last layer has NO relu). Per layer: dh_{i-1} = gemm(dz_i, W_iᵀ); then dz_{i-1} =
    //    relu_vjp(z_{i-1}, dh_{i-1}) (i>1) OR dx = dh_0 (i==1); dW_i = gemm(h_{i-1}ᵀ, dz_i) — EMITTED LAST per layer so dW_1 is
    //    the terminal op (grads[wrt=W_1] = the plan's single-Output readback target). ──
    Value* dz = dout;
    for (u32 k = 0; k < nw; ++k)
    {
        const u32    i  = nw - k; // nw, nw-1, ..., 1 (avoids the u32 reverse-loop underflow)
        Value* const wi = fwd->operand(i);
        // dh_{i-1} = gemm(dz_i [M, N_i], W_iᵀ [N_i, K_i]) : [M, K_i] == h_{i-1}'s type.
        const TypeId wt_t  = tensor2(ctx, elem, dim_of(ctx, wi->type(), 1U), dim_of(ctx, wi->type(), 0U));
        Value* const wit   = mk_transpose2d(ctx, blk, at, wi, wt_t); // W_iᵀ
        Value* const dprev = mk_gemm(ctx, blk, at, dz, wit, h[i - 1U]->type());
        Value*       next_dz = nullptr;
        if (i > 1U) { next_dz = emit_relu_vjp_dispatch(ctx, blk, at, grid, z[i - 1U], dprev); }
        else { operand_adjoints[0] = dprev; } // dx (input adjoint) — emitted BEFORE dW_1 so dW_1 lands last
        // dW_i = gemm(h_{i-1}ᵀ [K_i, M], dz_i [M, N_i]) : [K_i, N_i] == W_i's type. EMITTED LAST in the iteration.
        const TypeId ht_t = tensor2(ctx, elem, dim_of(ctx, h[i - 1U]->type(), 1U), dim_of(ctx, h[i - 1U]->type(), 0U));
        Value* const ht   = mk_transpose2d(ctx, blk, at, h[i - 1U], ht_t); // h_{i-1}ᵀ
        operand_adjoints[i] = mk_gemm(ctx, blk, at, ht, dz, wi->type());
        dz = next_dz;
    }
    return GradError::None;
}

void register_builtin_vjps(VjpRegistry& reg, Context& ctx)
{
    reg.register_op(ctx.intern_op("linalg", "gemm"), &vjp_gemm);
    reg.register_op(ctx.intern_op("tensor", "reduce"), &vjp_reduce);
    reg.register_op(ctx.intern_op("ml", "mlp"), &vjp_mlp); // 25c: the COMPOSITE MLP VJP (recompute interior + relu_vjp.ckir dispatch)
    // ⛔ 25c-0: relu is NOT register_kernel'd — a resultless compute.dispatch is never visited by build_gradient's result-keyed
    //    walk (the superseded-clause finding, grad.hpp); vjp_mlp emits the relu_vjp.ckir dispatch DIRECTLY. softmax = name-forward.
}
} // namespace crd::ceir::gpu
