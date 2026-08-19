#include <crd/ceir/gpu/partition_ml.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::gpu
{
namespace
{
using containers::StringView;

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
[[nodiscard]] bool  is_float_tensor(const Context& ctx, TypeId t) noexcept
{
    return ctx.type_of(t).kind == TypeKind::Tensor && ctx.type_of(elem_of(ctx, t)).kind == TypeKind::Float;
}
// The static extent of tensor `t`'s dim `axis`: true + `out` set iff the dim exists AND is Static.
[[nodiscard]] bool dim_static(const Context& ctx, TypeId t, usize axis, crd::u32& out) noexcept
{
    const Type sh = ctx.type_of(shape_of(ctx, t));
    if (axis >= sh.members.size()) { return false; }
    const Type d = ctx.type_of(sh.members[axis]);
    if (static_cast<DimKind>(d.cols) != DimKind::Static) { return false; }
    out = d.count;
    return true;
}
[[nodiscard]] bool is_ml_op(const Context& ctx, const Operation* op) noexcept
{
    const StringView nm = ctx.op_name(op->kind());
    return nm == StringView("ml.mlp") || nm == StringView("ml.attention");
}

// Assign one ml op to the first AVAILABLE provider that advertises it, or -1 (CKIR fallback).
[[nodiscard]] crd::i32 assign(const Context& ctx, const Operation* op, containers::ConstSpan<MlProvider> providers) noexcept
{
    for (crd::usize i = 0; i < providers.size(); ++i)
    {
        const MlProvider& p = providers[i];
        if (p.available && p.advertise != nullptr && p.advertise(ctx, op)) { return static_cast<crd::i32>(i); }
    }
    return -1; // CkirFallback
}

// The pre-order walk — push one MlAssignment per ml op.
void scan_region(const Context& ctx, const Region* r, containers::ConstSpan<MlProvider> providers, MlPartition& out) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (is_ml_op(ctx, op)) { out.assignments.push_back(MlAssignment{op, assign(ctx, op, providers)}); }
            for (u32 i = 0; i < op->num_regions(); ++i) { scan_region(ctx, op->region(i), providers, out); }
        }
    }
}
} // namespace

bool coopvec_can_claim_mlp(const Context& ctx, const Operation* op)
{
    if (op == nullptr || ctx.op_name(op->kind()) != StringView("ml.mlp")) { return false; } // I6: op NAME, never op.kind
    if (op->num_operands() < 3U || op->num_results() == 0U) { return false; }                // input + >=2 weights (>=1 hidden layer)
    const u32 nw = op->num_operands() - 1U;

    // activation == relu (the coopvec kernel's ONLY hidden activation — a gelu claim is a silent wrong-function).
    const AttrValue av = ctx.attr_value(op->attr(StringView("activation")));
    if (av.kind != AttrKind::String || av.s != StringView("relu")) { return false; }

    // input + every weight + output: Tensor + Float + rank-2.
    const TypeId in_t  = op->operand(0U)->type();
    const TypeId out_t = op->result(0U)->type();
    if (!is_float_tensor(ctx, in_t) || rank_of(ctx, in_t) != 2U) { return false; }
    if (!is_float_tensor(ctx, out_t) || rank_of(ctx, out_t) != 2U) { return false; }
    for (u32 w = 1U; w <= nw; ++w)
    {
        const TypeId wt = op->operand(w)->type();
        if (!is_float_tensor(ctx, wt) || rank_of(ctx, wt) != 2U) { return false; }
    }

    // UNIFORM hidden width: W_1[in_dim, hidden]; every later layer's input width == hidden (and every INTERMEDIATE output == hidden);
    // W_n[hidden, out_dim]. All dims static + in [1, 1024] (CoopVecMlpConfig::valid).
    crd::u32 in_dim = 0;
    crd::u32 hidden = 0;
    if (!dim_static(ctx, op->operand(1U)->type(), 0U, in_dim) || !dim_static(ctx, op->operand(1U)->type(), 1U, hidden)) { return false; }
    crd::u32 out_dim = 0;
    for (u32 w = 2U; w <= nw; ++w)
    {
        crd::u32 d0 = 0;
        if (!dim_static(ctx, op->operand(w)->type(), 0U, d0) || d0 != hidden) { return false; } // layer input width must be `hidden`
        if (w < nw)
        {
            crd::u32 d1 = 0;
            if (!dim_static(ctx, op->operand(w)->type(), 1U, d1) || d1 != hidden) { return false; } // intermediate output stays `hidden`
        }
    }
    if (!dim_static(ctx, op->operand(nw)->type(), 1U, out_dim)) { return false; } // out_dim = W_n.dim1
    return in_dim >= 1U && in_dim <= 1024U && hidden >= 1U && hidden <= 1024U && out_dim >= 1U && out_dim <= 1024U;
}

MlPartition partition_ml(const Context& ctx, const Module& m, containers::ConstSpan<MlProvider> providers, memory::IAllocator* alloc)
{
    MlPartition out(alloc);
    scan_region(ctx, m.body(), providers, out);
    return out;
}

MlExpandResult apply_partition(Context& ctx, Module& m, const MlPartition& partition)
{
    (void)m;
    MlExpandResult res;
    for (crd::usize i = 0; i < partition.assignments.size(); ++i)
    {
        const MlAssignment& a = partition.assignments[i];
        if (a.provider >= 0) { continue; }                              // CLAIMED — the caller dispatches it natively (not expanded)
        const MlExpandError err = expand_ml_op(ctx, const_cast<Operation*>(a.op)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        if (err != MlExpandError::None)
        {
            res.error    = err;
            res.error_op = a.op;
            return res;
        }
        ++res.expanded;
    }
    return res;
}
} // namespace crd::ceir::gpu
