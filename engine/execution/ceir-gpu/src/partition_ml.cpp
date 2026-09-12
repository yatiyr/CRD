#include <crd/ceir/gpu/partition_ml.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/gpu/sharding.hpp> // CEIR-30c: gather_meshes/resolve_mesh/mesh_extent — the placement loader's mesh-query helpers
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

// Assign one ml op to a provider, or -1 (CKIR fallback). CEIR-29c-3a `constraint` composes filter-then-prefer:
//   ELIGIBILITY = available + advertises this op + (when `has_class`) provider_class matches. ⛔ the class is a HARD FILTER
//   (a §102 requirement): an op no ELIGIBLE provider claims returns -1, NEVER a provider of another class.
//   `pinned` (CEIR-29a-3) is an authored PREFERENCE WITHIN the eligible set — tried first if IT is eligible, else first-eligible
//   (the "gelu on a relu kernel" scar: a pin never forces an unclaimable op; and a pin the class filter excluded is IGNORED —
//   the filter is outer). has_class=false + pinned<0 => first-available, the 24c invariant.
[[nodiscard]] crd::i32 assign(const Context& ctx, const Operation* op, containers::ConstSpan<MlProvider> providers,
                              const PartitionConstraint& c) noexcept
{
    const auto eligible = [&](const MlProvider& p) -> bool {
        return p.available && p.advertise != nullptr && (!c.has_class || p.provider_class == c.provider_class)
               && p.advertise(ctx, op);
    };
    if (c.pinned >= 0 && static_cast<crd::usize>(c.pinned) < providers.size()
        && eligible(providers[static_cast<crd::usize>(c.pinned)]))
    {
        return c.pinned;
    }
    for (crd::usize i = 0; i < providers.size(); ++i)
    {
        if (eligible(providers[i])) { return static_cast<crd::i32>(i); }
    }
    return -1; // CkirFallback
}

// CEIR-29a-3b/29c-3b: the FIRST op named `name` in pre-order, REGION-RECURSIVE — matching find_transform_misuse's walk (one
// helper for BOTH the assign_provider pin AND the constrain_provider_class filter loaders). ⛔ the loaders rely on that misuse
// guard for "at most one directive", and the guard is recursive; so the walk MUST be too, or a directive NESTED in a region
// passes the guard yet is silently missed here (→ a no-pin/no-class — the graceful-wrong-answer trap). Matches op NAME (I6).
[[nodiscard]] const Operation* first_op_named(const Context& ctx, const Region* r, StringView name) noexcept // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return nullptr; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == name) { return op; } // I6: op NAME, never op.kind
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const Operation* const found = first_op_named(ctx, op->region(i), name);
                if (found != nullptr) { return found; }
            }
        }
    }
    return nullptr;
}

// The pre-order walk — push one MlAssignment per ml op, GREEDY-GROWING maximal subgraph runs (CEIR-29a-2 §102).
// `next_subgraph` is a MONOTONIC id source threaded through the WHOLE module (never reset per block/provider — the
// monotone-watermark discipline: two runs of one provider in different blocks MUST get distinct ids). A run of a
// claims_subgraphs provider extends across CONSECUTIVE sibling ml ops it advertises; ANY non-ml op, an advertise
// refusal, a different provider, or the block end CLOSES it (no stepping over a non-ml op — that dependency is
// 29b's boundary-transfer question, not a claim). A per-op claim (claims_subgraphs=false) or a fallback is a -1 singleton.
void scan_region(const Context& ctx, const Region* r, containers::ConstSpan<MlProvider> providers, MlPartition& out, // NOLINT(misc-no-recursion)
                 crd::i32& next_subgraph, const PartitionConstraint& constraint)
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        crd::i32 run_provider = -1; // the provider growing an OPEN run in THIS block (-1 = none); resets at block start
        crd::i32 run_subgraph = -1; // its subgraph id
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (is_ml_op(ctx, op))
            {
                const crd::i32 p  = assign(ctx, op, providers, constraint);
                crd::i32       sg = -1;
                if (p >= 0 && providers[static_cast<usize>(p)].claims_subgraphs)
                {
                    if (run_provider == p) { sg = run_subgraph; } // EXTEND the open run (same provider, adjacent sibling ml op)
                    else { sg = next_subgraph++; run_provider = p; run_subgraph = sg; } // OPEN a new run
                }
                else { run_provider = -1; run_subgraph = -1; } // per-op claim OR fallback: a singleton that closes any run
                out.assignments.push_back(MlAssignment{op, p, sg});
            }
            else { run_provider = -1; run_subgraph = -1; } // a non-ml op between ml ops SPLITS the run
            for (u32 i = 0; i < op->num_regions(); ++i) { scan_region(ctx, op->region(i), providers, out, next_subgraph, constraint); }
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

// CEIR-29b-2b — the CUDA-Graphs LAUNCH-GRAPH provider's claim (see the header): the semantic class the CUDA emitters cover, WITHOUT
// coopvec's uniform-hidden / [1,1024] limits (the CKIR gemm is general-dims). op is ml.mlp; input + >=2 weights; activation == relu
// (the specialized-kernel scar — emit_contract_cuda has no fused-GemmRelu unwrap); input/weights/output Float + rank-2.
bool cuda_graphs_can_claim(const Context& ctx, const Operation* op)
{
    if (op == nullptr || ctx.op_name(op->kind()) != StringView("ml.mlp")) { return false; } // I6: op NAME, never op.kind
    if (op->num_operands() < 3U || op->num_results() == 0U) { return false; }                // input + >=2 weights (>=1 hidden layer)
    const AttrValue av = ctx.attr_value(op->attr(StringView("activation")));
    if (av.kind != AttrKind::String || av.s != StringView("relu")) { return false; } // relu only (no fused-GemmRelu / gelu on CUDA)
    const u32    nw    = op->num_operands() - 1U;
    const TypeId in_t  = op->operand(0U)->type();
    const TypeId out_t = op->result(0U)->type();
    if (!is_float_tensor(ctx, in_t) || rank_of(ctx, in_t) != 2U) { return false; }
    if (!is_float_tensor(ctx, out_t) || rank_of(ctx, out_t) != 2U) { return false; }
    for (u32 w = 1U; w <= nw; ++w)
    {
        const TypeId wt = op->operand(w)->type();
        if (!is_float_tensor(ctx, wt) || rank_of(ctx, wt) != 2U) { return false; }
    }
    return true;
}

MlPartition partition_ml(const Context& ctx, const Module& m, containers::ConstSpan<MlProvider> providers, memory::IAllocator* alloc,
                         PartitionConstraint constraint)
{
    MlPartition out(alloc);
    crd::i32    next_subgraph = 0; // CEIR-29a-2: the monotonic subgraph-id source for the whole-module walk
    scan_region(ctx, m.body(), providers, out, next_subgraph, constraint); // CEIR-29a-3 pin + 29c-3a class filter (default {} = neither)
    return out;
}

StringView provider_pin_kind_name(ProviderPinKind k) noexcept
{
    switch (k)
    {
    case ProviderPinKind::None: return StringView("none");
    case ProviderPinKind::Resolved: return StringView("resolved");
    case ProviderPinKind::UnknownProvider: return StringView("unknown-provider");
    case ProviderPinKind::DuplicateProviderName: return StringView("duplicate-provider-name");
    }
    return StringView("?");
}

ProviderPin provider_from_transform(const Context& ctx, const Module& transform_mod, containers::ConstSpan<MlProvider> providers)
{
    // (1) SPAN DEFECT FIRST — a name shared by two providers makes ANY pin ambiguous BY CONSTRUCTION (a broken span, not a
    // per-name issue): report it before resolving, whichever name the pin would target. O(n^2), n is a handful (the
    // subgraphs_of precedent). Empty names don't collide (an unnamed provider is never a pin target).
    for (crd::usize i = 0; i < providers.size(); ++i)
    {
        for (crd::usize j = i + 1U; j < providers.size(); ++j)
        {
            if (!providers[i].name.empty() && providers[i].name == providers[j].name)
            {
                return {-1, nullptr, ProviderPinKind::DuplicateProviderName};
            }
        }
    }
    // (2) THE DIRECTIVE — the (at-most-one, find_transform_misuse-guarded) transform.assign_provider, found REGION-RECURSIVELY
    // (the guard is recursive, so this must be too — a nested directive must not be silently missed). Matched by op NAME (I6).
    const Operation* const op = first_op_named(ctx, transform_mod.body(), StringView("transform.assign_provider"));
    if (op == nullptr) { return {}; } // no directive — {None, -1}: no pin (partition_ml runs first-available)
    // Read `provider` DEFENSIVELY (valid + String kind + non-empty); an empty/invalid/non-string name names NOTHING (the
    // attr-reader-checks-valid rule + is_memory_domain's !empty posture) ⇒ UnknownProvider, never a silent -1.
    const AttrId    a  = op->attr(StringView("provider"));
    const AttrValue av = ctx.attr_value(a);
    if (a.valid() && av.kind == AttrKind::String && !av.s.empty())
    {
        for (crd::usize i = 0; i < providers.size(); ++i)
        {
            if (providers[i].name == av.s) { return {static_cast<crd::i32>(i), op, ProviderPinKind::Resolved}; }
        }
    }
    return {-1, op, ProviderPinKind::UnknownProvider}; // matched no provider (bad attr, or the name is absent from the span)
}

StringView class_constraint_kind_name(ClassConstraintKind k) noexcept
{
    switch (k)
    {
    case ClassConstraintKind::None: return StringView("none");
    case ClassConstraintKind::Resolved: return StringView("resolved");
    case ClassConstraintKind::UnknownProviderClass: return StringView("unknown-provider-class");
    }
    return StringView("?");
}

ClassConstraint constraint_from_transform(const Context& ctx, const Module& transform_mod)
{
    // the (at-most-one, find_transform_misuse-guarded) transform.constrain_provider_class, found REGION-RECURSIVELY (I6 op name).
    const Operation* const op = first_op_named(ctx, transform_mod.body(), StringView("transform.constrain_provider_class"));
    if (op == nullptr) { return {}; } // no directive — {None}: no class filter (partition_ml runs unconstrained)
    // read `class` DEFENSIVELY (valid + String kind + non-empty) then decode via the shared semantics.hpp table; an
    // empty/invalid/non-string or unrecognized name names NO class ⇒ UnknownProviderClass, never a silent no-constraint.
    const AttrId    a  = op->attr(StringView("class"));
    const AttrValue av = ctx.attr_value(a);
    ProviderClass   pc = ProviderClass::Gpu;
    if (a.valid() && av.kind == AttrKind::String && !av.s.empty() && provider_class_from_name(av.s, pc))
    {
        return {pc, op, ClassConstraintKind::Resolved};
    }
    return {ProviderClass::Gpu, op, ClassConstraintKind::UnknownProviderClass};
}

StringView placement_kind_name(PlacementKind k) noexcept
{
    switch (k)
    {
    case PlacementKind::None: return StringView("none");
    case PlacementKind::Resolved: return StringView("resolved");
    case PlacementKind::UnknownMesh: return StringView("unknown-mesh");
    case PlacementKind::MultiAxisMesh: return StringView("multi-axis-mesh");
    case PlacementKind::UnknownProviderClass: return StringView("unknown-provider-class");
    case PlacementKind::RankCountMismatch: return StringView("rank-count-mismatch");
    }
    return StringView("?");
}

Placement placement_from_transform(const Context& ctx, const Module& transform_mod, const Module& payload, memory::IAllocator* alloc)
{
    Placement res(alloc);
    // the (at-most-one, find_transform_misuse-guarded) transform.place_mesh, found REGION-RECURSIVELY (I6 op name).
    const Operation* const op = first_op_named(ctx, transform_mod.body(), StringView("transform.place_mesh"));
    if (op == nullptr) { return res; } // no directive — {None}: no authored placement
    res.op = op;
    // resolve `mesh` (a Symbol) against the PAYLOAD's dist.mesh ops (so the rank-count validation has the extent).
    const AttrValue  mav       = ctx.attr_value(op->attr(StringView("mesh")));
    const StringView mesh_name = mav.kind == AttrKind::SymbolRef ? mav.s : StringView();
    containers::Array<const Operation*> meshes(alloc);
    gather_meshes(ctx, payload.body(), meshes);
    const Operation* const mesh = resolve_mesh(ctx, meshes, mesh_name);
    if (mesh == nullptr) { res.kind = PlacementKind::UnknownMesh; return res; }
    // ⛔ this slice places a 1-D mesh only. A 2-D mesh (shape "2,2") is verify-CLEAN (find_dist_misuse allows it),
    // so axis-0 validation would silently place 2 classes onto 4 ranks. Refuse a >1-axis mesh with a typed reject —
    // a `mesh_axis` attr (mirroring dist.shard) is the sec-71 named-forward. A valid axis-1 extent ⇒ the mesh has ≥2 axes.
    if (mesh_extent(ctx, mesh, 1) != 0) { res.kind = PlacementKind::MultiAxisMesh; return res; }
    const crd::i32 extent = mesh_extent(ctx, mesh, 0); // mesh_axis 0 — the 1-D mesh this slice (per-axis placement is name-forward)
    // parse `classes` (a comma-list of ProviderClass names) into rank_classes; an empty/unknown segment ⇒ UnknownProviderClass.
    const AttrValue  cav     = ctx.attr_value(op->attr(StringView("classes")));
    const StringView classes = cav.kind == AttrKind::String ? cav.s : StringView();
    usize            start   = 0;
    for (usize i = 0; i <= classes.size(); ++i)
    {
        if (i == classes.size() || classes[i] == ',')
        {
            const StringView seg(classes.data() + start, i - start);
            ProviderClass    pc = ProviderClass::Gpu;
            if (!provider_class_from_name(seg, pc)) { res.kind = PlacementKind::UnknownProviderClass; return res; }
            res.rank_classes.push_back(pc);
            start = i + 1U;
        }
    }
    // parse `fallback` (a single ProviderClass name).
    const AttrValue fav = ctx.attr_value(op->attr(StringView("fallback")));
    ProviderClass   fbc = ProviderClass::Gpu;
    if (fav.kind != AttrKind::String || !provider_class_from_name(fav.s, fbc))
    {
        res.kind = PlacementKind::UnknownProviderClass;
        return res;
    }
    res.fallback = fbc;
    // ⛔ the count MUST equal the mesh extent — a short/long list is a typed reject, never a silent rank-placed-nowhere.
    // extent<=0 (a malformed axis-0 shape) is unreachable past find_dist_misuse's MeshShapeInvalid guard in the intended
    // flow (30c-2 wires dist-verify-first); folded here into RankCountMismatch as the belt-and-braces refuse.
    if (extent <= 0 || res.rank_classes.size() != static_cast<usize>(extent))
    {
        res.kind = PlacementKind::RankCountMismatch;
        return res;
    }
    res.kind = PlacementKind::Resolved;
    return res;
}

PartitionConstraint compose_partition_constraint(const ClassConstraint& cc, const ProviderPin& pin) noexcept
{
    PartitionConstraint out;
    out.has_class      = cc.kind == ClassConstraintKind::Resolved; // Resolved IS has_class (no second boolean to drift)
    out.provider_class = cc.provider_class;                        // meaningful only when has_class
    out.pinned         = pin.index;                               // -1 (None/reject) = no pin; the filter is OUTER regardless
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
