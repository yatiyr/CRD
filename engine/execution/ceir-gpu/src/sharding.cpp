#include <crd/ceir/gpu/sharding.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dist.hpp>   // dist::build_all_reduce (30a-3 materialization inserts it)
#include <crd/ceir/ir.hpp>
#include <crd/ceir/tensor.hpp> // tensor::build_reduce / build_elementwise (30b-3a lowering emits them)
#include <crd/ceir/type.hpp>

#include <crd/containers/array.hpp>

namespace crd::ceir::gpu
{
namespace
{
using containers::StringView;

// ⛔ HANG-GUARD (the monotone-watermark scar): the materialize fixpoint terminates because each insert makes one Partial
// Replicated (so #inserts < #Partial-producing ops), but a future rule that inserts a Partial-PRODUCING op would loop forever.
constexpr crd::u32 kMaxMaterializeInserts = 1U << 20;

[[nodiscard]] bool is_tensor_type(const Context& ctx, TypeId t) noexcept { return ctx.type_of(t).kind == TypeKind::Tensor; }

[[nodiscard]] StringView symbol_name(const Context& ctx, AttrId a) noexcept
{
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::SymbolRef ? v.s : StringView();
}
[[nodiscard]] crd::i32 int_attr(const Context& ctx, const Operation* op, const char* name) noexcept
{
    const AttrValue v = ctx.attr_value(op->attr(name));
    return v.kind == AttrKind::Int ? static_cast<crd::i32>(v.i) : -1;
}
[[nodiscard]] StringView string_attr(const Context& ctx, const Operation* op, const char* name) noexcept
{
    const AttrValue v = ctx.attr_value(op->attr(name));
    return v.kind == AttrKind::String ? v.s : StringView();
}

[[nodiscard]] Sharding replicated() noexcept { return Sharding{}; }
[[nodiscard]] Sharding conflict() noexcept { return Sharding{ShardingKind::Conflict, nullptr, -1, -1, {}}; }

// gather_meshes / resolve_mesh / mesh_extent are PUBLIC (declared in sharding.hpp) — hoisted out of this anon namespace at
// CEIR-30c so the placement loader (partition_ml) reuses them; their definitions sit just below the anon namespace.

// The sharding of `op`'s result(0), from the already-populated inputs (the walk guarantees result(0) is a Tensor).
[[nodiscard]] Sharding op_result_sharding(const Context& ctx, const Operation* op, const ShardingMap& sm,
                                          const containers::Array<const Operation*>& meshes)
{
    const StringView nm = ctx.op_name(op->kind());
    if (nm == StringView("dist.shard"))
    {
        if (op->num_operands() < 1U) { return conflict(); }
        if (sm.of(op->operand(0U)).kind != ShardingKind::Replicated) { return conflict(); } // reshard is ledgered
        const Operation* const mesh = resolve_mesh(ctx, meshes, symbol_name(ctx, op->attr("mesh")));
        if (mesh == nullptr) { return conflict(); }
        const crd::i32 maxis = int_attr(ctx, op, "mesh_axis");
        if (mesh_extent(ctx, mesh, maxis) == 1) { return replicated(); } // 1-device IDENTITY: a size-1 mesh axis ⇒ Replicated
        return Sharding{ShardingKind::Sharded, mesh, int_attr(ctx, op, "axis"), maxis, {}};
    }
    if (nm == StringView("dist.all_reduce"))
    {
        if (op->num_operands() < 1U) { return conflict(); }
        const Sharding         in   = sm.of(op->operand(0U));
        const Operation* const mesh = resolve_mesh(ctx, meshes, symbol_name(ctx, op->attr("mesh")));
        if (mesh == nullptr) { return conflict(); }
        // 1-device IDENTITY: an all_reduce over a single-device (1-D) mesh is a no-op — pass the input's sharding through, so an
        // authored shard→reduce→all_reduce on mesh "1" stays Replicated, NOT a Conflict (a rank>1 mesh needs a mesh_axis on the
        // collective — 30a-1 named it forward; checking axis 0 covers the 1-D case, the only one with a real consumer today).
        if (mesh_extent(ctx, mesh, 0) == 1) { return in; }
        const StringView fn = string_attr(ctx, op, "fn");
        // else: completes a matching Partial to Replicated; a Sharded input (an all_gather) / Replicated / fn-mismatch is a Conflict.
        if (in.kind == ShardingKind::Partial && in.mesh == mesh && in.fn == fn) { return replicated(); }
        return conflict();
    }
    if (nm == StringView("tensor.elementwise"))
    {
        if (op->num_operands() < 2U) { return conflict(); }
        return meet_sharding(sm.of(op->operand(0U)), sm.of(op->operand(1U)));
    }
    if (nm == StringView("tensor.reduce"))
    {
        if (op->num_operands() < 1U) { return conflict(); }
        const Sharding    in   = sm.of(op->operand(0U));
        const crd::i32    axis = int_attr(ctx, op, "axis");
        const StringView  fn   = string_attr(ctx, op, "fn");
        if (in.kind == ShardingKind::Replicated) { return replicated(); }
        if (in.kind == ShardingKind::Conflict) { return conflict(); }
        if (in.kind == ShardingKind::Sharded)
        {
            if (in.axis == axis) { return Sharding{ShardingKind::Partial, in.mesh, axis, in.mesh_axis, fn}; } // over the SPLIT axis
            const crd::i32 reindexed = in.axis > axis ? in.axis - 1 : in.axis; // the dropped axis shifts later axes down
            return Sharding{ShardingKind::Sharded, in.mesh, reindexed, in.mesh_axis, {}};
        }
        // in.kind == Partial: a further reduce keeps it Partial iff the fn matches (else two combine ops disagree).
        if (in.fn == fn) { return Sharding{ShardingKind::Partial, in.mesh, axis, in.mesh_axis, fn}; }
        return conflict();
    }
    if (nm == StringView("resource.declare") || nm == StringView("resource.import")) { return replicated(); } // a fresh seed
    return conflict(); // any OTHER tensor-producing op (matmul/gemm/transpose/reshape/broadcast/fft): unknown ⇒ Conflict (sec-70)
}

void walk(const Context& ctx, const Region* r, ShardingMap& sm, // NOLINT(misc-no-recursion)
          const containers::Array<const Operation*>& meshes)
{
    if (r == nullptr) { return; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->num_results() >= 1U && is_tensor_type(ctx, op->result(0U)->type()))
            {
                const Sharding s = op_result_sharding(ctx, op, sm, meshes);
                if (s.kind != ShardingKind::Replicated) { sm.map.insert(op->result(0U), s); } // Replicated = absent (the default)
            }
            for (u32 i = 0; i < op->num_regions(); ++i) { walk(ctx, op->region(i), sm, meshes); }
        }
    }
}

// ── 30a-3 materialization ──
// a value ESCAPES if it has a use whose owner is NOT a dist.all_reduce / tensor.reduce / tensor.elementwise (those COMPOSE a
// partial — the reduce/elementwise defer it, an all_reduce completes it). Anything else (export, matmul, a dispatch, …) needs
// the partial COMPLETE.
[[nodiscard]] bool has_escaping_use(const Context& ctx, const Value* v) noexcept
{
    for (const Use* u = v->first_use(); u != nullptr; u = u->next)
    {
        const StringView nm = ctx.op_name(u->owner->kind());
        if (nm != StringView("dist.all_reduce") && nm != StringView("tensor.reduce") && nm != StringView("tensor.elementwise"))
        {
            return true;
        }
    }
    return false;
}

struct EscapeTarget
{
    Value*     value = nullptr;
    Operation* def   = nullptr;
    Block*     block = nullptr;
    Sharding   sh    = {};
};
// The FIRST Partial-with-an-escaping-use (pre-order), or false. A Conflict sets `had_conflict` (the pass refuses to materialize
// past it) — accumulated across the whole walk when no Partial is found (the terminating iteration).
bool find_escape(const Context& ctx, Region* r, const ShardingMap& sm, EscapeTarget& out, bool& had_conflict) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return false; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->num_results() >= 1U && is_tensor_type(ctx, op->result(0U)->type()))
            {
                const Sharding s = sm.of(op->result(0U));
                if (s.kind == ShardingKind::Conflict) { had_conflict = true; }
                else if (s.kind == ShardingKind::Partial && has_escaping_use(ctx, op->result(0U)))
                {
                    out = EscapeTarget{op->result(0U), op, b, s};
                    return true;
                }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                if (find_escape(ctx, op->region(i), sm, out, had_conflict)) { return true; }
            }
        }
    }
    return false;
}

// ── 30b-3a lowering ──
// any tensor-result value in the module whose sharding is Conflict (the pass refuses to lower past one — checked BEFORE mutation).
[[nodiscard]] bool region_has_conflict(const Context& ctx, const Region* r, const ShardingMap& sm) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return false; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->num_results() >= 1U && is_tensor_type(ctx, op->result(0U)->type())
                && sm.of(op->result(0U)).kind == ShardingKind::Conflict)
            {
                return true;
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                if (region_has_conflict(ctx, op->region(i), sm)) { return true; }
            }
        }
    }
    return false;
}
// the FIRST op named `name` (pre-order), or null — a NON-const finder (the lowering mutates the op it returns).
Operation* first_op_named(const Context& ctx, Region* r, StringView name) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return nullptr; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == name) { return op; }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                Operation* const f = first_op_named(ctx, op->region(i), name);
                if (f != nullptr) { return f; }
            }
        }
    }
    return nullptr;
}
// the FIRST dist.shard / dist.all_reduce over a DEGENERATE (size-1) mesh axis (shard: its mesh_axis; all_reduce: axis 0, the only
// case propagate treats as identity) — the 1-device strip target. null when none remain.
Operation* first_degenerate_dist_op(const Context& ctx, Region* r, // NOLINT(misc-no-recursion)
                                    const containers::Array<const Operation*>& meshes)
{
    if (r == nullptr) { return nullptr; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const StringView nm = ctx.op_name(op->kind());
            if (nm == StringView("dist.shard") || nm == StringView("dist.all_reduce"))
            {
                const Operation* const mesh = resolve_mesh(ctx, meshes, symbol_name(ctx, op->attr("mesh")));
                const crd::i32         maxis = nm == StringView("dist.shard") ? int_attr(ctx, op, "mesh_axis") : 0;
                if (mesh != nullptr && mesh_extent(ctx, mesh, maxis) == 1) { return op; }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                Operation* const f = first_degenerate_dist_op(ctx, op->region(i), meshes);
                if (f != nullptr) { return f; }
            }
        }
    }
    return nullptr;
}
// erase every dist.mesh op (a mesh is referenced by NAME, never by SSA use, so it is dead once its shards/all_reduces are gone —
// stripping it is what makes the lowered module carry NO dist ops, incl. the 1-device byte-identical-with-the-plain-reduce identity).
void strip_dead_meshes(const Context& ctx, Module& m)
{
    for (Operation* me = first_op_named(ctx, m.body(), StringView("dist.mesh")); me != nullptr;
         me            = first_op_named(ctx, m.body(), StringView("dist.mesh")))
    {
        me->erase();
    }
}
// the tensor.elementwise op-name that COMBINES two per-rank partials for reduce `fn` (the fn -> op envelope); empty if `fn` has no
// combine op (unreachable past find_dist_misuse's {sum,prod,max,min}, but the lowering refuses rather than guess add).
[[nodiscard]] StringView combine_op_for(StringView fn) noexcept
{
    if (fn == StringView("sum")) { return StringView("add"); }
    if (fn == StringView("prod")) { return StringView("mul"); }
    if (fn == StringView("max")) { return StringView("max"); }
    if (fn == StringView("min")) { return StringView("min"); }
    return StringView();
}
// the per-rank SHARD tensor type: `full` with dim[axis] divided by E (a static split along a size-E mesh axis). Invalid TypeId if
// `full` is not a static-shaped Tensor, `axis` is out of range, or the sharded dim is not divisible by E.
[[nodiscard]] TypeId shard_tensor_type(Context& ctx, TypeId full, crd::i32 axis, crd::i32 E, memory::IAllocator* alloc)
{
    const Type tt = ctx.type_of(full);
    if (tt.kind != TypeKind::Tensor || tt.members.size() < 2U) { return TypeId{}; }
    const TypeId elem = tt.members[0];
    const Type   st   = ctx.type_of(tt.members[1]);
    const usize  rank = st.members.size();
    if (axis < 0 || static_cast<usize>(axis) >= rank) { return TypeId{}; }
    containers::Array<TypeId> dims(alloc);
    for (usize i = 0; i < rank; ++i)
    {
        if (static_cast<crd::i32>(i) == axis)
        {
            const Type d = ctx.type_of(st.members[i]);
            if (static_cast<DimKind>(d.cols) != DimKind::Static || E <= 0 || d.count % static_cast<u32>(E) != 0U)
            {
                return TypeId{};
            }
            dims.push_back(ctx.type_dim_static(d.count / static_cast<u32>(E)));
        }
        else { dims.push_back(st.members[i]); }
    }
    return ctx.type_tensor(elem, ctx.type_shape(containers::ConstSpan<TypeId>(dims.data(), dims.size())));
}
} // namespace

// ── mesh-query helpers (public since CEIR-30c — the placement loader reuses them; they call the anon symbol_name above) ──
void gather_meshes(const Context& ctx, const Region* r, containers::Array<const Operation*>& out) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == StringView("dist.mesh")) { out.push_back(op); }
            for (u32 i = 0; i < op->num_regions(); ++i) { gather_meshes(ctx, op->region(i), out); }
        }
    }
}
const Operation* resolve_mesh(const Context& ctx, const containers::Array<const Operation*>& meshes, StringView name) noexcept
{
    if (name.size() == 0U) { return nullptr; }
    for (usize i = 0; i < meshes.size(); ++i)
    {
        if (symbol_name(ctx, meshes[i]->attr("name")) == name) { return meshes[i]; }
    }
    return nullptr;
}
// the extent of `mesh_op`'s `shape` along `mesh_axis` (the comma-int list — the dist.mesh `shape` mold); 0 if malformed or
// `mesh_axis` is out of range. A 1 means a DEGENERATE (single-device) mesh axis — the 1-device IDENTITY (a shard over it is Replicated).
crd::i32 mesh_extent(const Context& ctx, const Operation* mesh_op, crd::i32 mesh_axis) noexcept
{
    if (mesh_op == nullptr || mesh_axis < 0) { return 0; }
    const AttrValue shp = ctx.attr_value(mesh_op->attr("shape"));
    if (shp.kind != AttrKind::String) { return 0; }
    const StringView s     = shp.s;
    crd::i32         idx   = 0;
    usize            start = 0;
    for (usize i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ',')
        {
            if (idx == mesh_axis)
            {
                crd::i32 v  = 0;
                bool     ok = i > start;
                for (usize j = start; j < i && ok; ++j)
                {
                    const char c = s[j];
                    if (c < '0' || c > '9') { ok = false; }
                    else { v = v * 10 + (c - '0'); }
                }
                return ok ? v : 0;
            }
            ++idx;
            start = i + 1U;
        }
    }
    return 0; // mesh_axis past the last mesh dim
}

bool operator==(const Sharding& a, const Sharding& b) noexcept
{
    if (a.kind != b.kind) { return false; }
    if (a.kind == ShardingKind::Replicated || a.kind == ShardingKind::Conflict) { return true; } // other fields are defaults
    return a.mesh == b.mesh && a.axis == b.axis && a.mesh_axis == b.mesh_axis && a.fn == b.fn;
}

Sharding meet_sharding(const Sharding& a, const Sharding& b) noexcept
{
    if (a.kind == ShardingKind::Replicated) { return b; } // Replicated is bottom — the other operand's layout wins
    if (b.kind == ShardingKind::Replicated) { return a; }
    if (a == b) { return a; } // two equal Sharded / two equal Partial
    return conflict();        // different mesh/axis, Sharded vs Partial, or either already Conflict
}

containers::StringView sharding_kind_name(ShardingKind k) noexcept
{
    switch (k)
    {
    case ShardingKind::Replicated: return StringView("replicated");
    case ShardingKind::Sharded: return StringView("sharded");
    case ShardingKind::Partial: return StringView("partial");
    case ShardingKind::Conflict: return StringView("conflict");
    }
    return StringView("?");
}

ShardingMap propagate_sharding(const Context& ctx, const Module& m, memory::IAllocator* alloc)
{
    ShardingMap                         sm(alloc);
    containers::Array<const Operation*> meshes(alloc);
    gather_meshes(ctx, m.body(), meshes); // ONCE
    walk(ctx, m.body(), sm, meshes);
    return sm;
}

MaterializeResult materialize_sharding(Context& ctx, Module& m, memory::IAllocator* alloc)
{
    MaterializeResult res;
    for (;;)
    {
        const ShardingMap sm = propagate_sharding(ctx, m, alloc); // ⛔ RE-RUN — each insert invalidates the map's pointer keys
        EscapeTarget      t;
        if (!find_escape(ctx, m.body(), sm, t, res.had_conflict)) { break; } // no escaping Partial left (or only Conflicts)
        if (res.inserted >= kMaxMaterializeInserts) { break; }              // bounded fixpoint (never a silent hang)
        // insert dist.all_reduce(V) {mesh = V's mesh, fn = V's Partial fn}, type-preserving, at V's DEF (dominating every use).
        const StringView mesh_name = symbol_name(ctx, t.sh.mesh->attr("name"));
        Operation* const ar =
            dist::build_all_reduce(ctx, t.value, ctx.attr_symbol(mesh_name), ctx.attr_string(t.sh.fn), t.value->type());
        Operation* const next = t.def->next_in_block();
        if (next != nullptr) { t.block->insert_before(ar, next); }
        else { t.block->append(ar); }
        t.value->replace_all_uses_with(ar->result(0U)); // repoints ALL uses (incl. ar's own operand → a transient self-cycle)
        ar->set_operand(0U, t.value);                   // ...restored: ar reads V, every prior consumer reads ar's result
        ++res.inserted;
    }
    return res;
}

LowerResult lower_sharded_reduction(Context& ctx, Module& m, memory::IAllocator* alloc,
                                    containers::HashMap<const Operation*, crd::i32>& rank_lineage)
{
    // (1) CONFLICT SCAN first — refuse before ANY mutation (a refuse leaves the module byte-identical; a partial rewrite corrupts).
    {
        const ShardingMap sm = propagate_sharding(ctx, m, alloc);
        if (region_has_conflict(ctx, m.body(), sm)) { return LowerResult{0U, true}; }
    }
    // (2) find the sec-140 shard chain (one per module this slice). No shard ⇒ nothing to lower.
    Operation* const ts = first_op_named(ctx, m.body(), StringView("dist.shard"));
    if (ts == nullptr) { return LowerResult{0U, false}; }
    if (ts->num_operands() < 1U) { return LowerResult{0U, true}; }
    containers::Array<const Operation*> meshes(alloc);
    gather_meshes(ctx, m.body(), meshes);
    const Operation* const mesh = resolve_mesh(ctx, meshes, symbol_name(ctx, ts->attr("mesh")));
    if (mesh == nullptr) { return LowerResult{0U, true}; }
    const crd::i32 axis  = int_attr(ctx, ts, "axis");
    const crd::i32 maxis = int_attr(ctx, ts, "mesh_axis");
    const crd::i32 extent = mesh_extent(ctx, mesh, maxis);
    if (extent <= 0) { return LowerResult{0U, true}; }

    // (3a) 1-DEVICE IDENTITY: a size-1 mesh axis is degenerate — strip every degenerate dist op (shard + any all_reduce), leaving
    // the unsharded reduce. ranks==1, rank_lineage empty (a 1-device mesh has no rank to place).
    if (extent == 1)
    {
        for (Operation* victim = first_degenerate_dist_op(ctx, m.body(), meshes); victim != nullptr;
             victim            = first_degenerate_dist_op(ctx, m.body(), meshes))
        {
            victim->result(0U)->replace_all_uses_with(victim->operand(0U)); // the annotation preserves the tensor — pass it through
            victim->erase();
        }
        strip_dead_meshes(ctx, m); // ⛔ NO dist ops survive — the lowered module is exactly the unsharded reduce (byte-identical)
        return LowerResult{1U, false};
    }

    // (3b) SPLIT (extent>=2): verify the clean chain via the analysis (pointers valid pre-mutation), then rewrite.
    const ShardingMap sm = propagate_sharding(ctx, m, alloc);
    if (sm.of(ts->result(0U)).kind != ShardingKind::Sharded || ts->result(0U)->num_uses() != 1U)
    {
        return LowerResult{0U, true};
    }
    Operation* const r = ts->result(0U)->first_use()->owner;
    if (ctx.op_name(r->kind()) != StringView("tensor.reduce") || sm.of(r->result(0U)).kind != ShardingKind::Partial
        || r->result(0U)->num_uses() != 1U)
    {
        return LowerResult{0U, true};
    }
    Operation* const ar = r->result(0U)->first_use()->owner;
    if (ctx.op_name(ar->kind()) != StringView("dist.all_reduce") || sm.of(ar->result(0U)).kind != ShardingKind::Replicated)
    {
        return LowerResult{0U, true};
    }
    const crd::i32   reduce_axis = int_attr(ctx, r, "axis"); // == the sharded axis (else propagate would not give Partial)
    const StringView fn          = string_attr(ctx, r, "fn");
    const StringView combine_nm  = combine_op_for(fn);
    if (combine_nm.size() == 0U) { return LowerResult{0U, true}; }
    Operation* const t = ts->operand(0U)->defining_op();
    if (t == nullptr) { return LowerResult{0U, true}; }
    const TypeId shard_ty = shard_tensor_type(ctx, ts->operand(0U)->type(), axis, extent, alloc);
    if (!shard_ty.valid()) { return LowerResult{0U, true}; }
    const TypeId reduce_ty = r->result(0U)->type(); // reducing the sharded axis eliminates it ⇒ same shape as the full reduce

    // Build the `extent` per-rank shard declares + `extent` tagged reduces + the (extent-1)-op combine tree, inserted before `t`
    // (top of the block — fresh seeds with no operands, dominating every downstream use). Every builder return is checked pre-erase.
    Block* const tb        = t->parent_block();
    const OpId   decl_id   = ctx.intern_op("resource", "declare");
    const AttrId axis_attr = ctx.attr_int(reduce_axis);
    const AttrId fn_attr   = ctx.attr_string(fn);
    const AttrId comb_attr = ctx.attr_string(combine_nm);
    containers::Array<Operation*> reduces(alloc);
    for (crd::i32 i = 0; i < extent; ++i)
    {
        Operation* const d = ctx.create_operation(decl_id, {}, 1U, shard_ty);
        tb->insert_before(d, t);
        Operation* const rd = tensor::build_reduce(ctx, d->result(0U), axis_attr, fn_attr, reduce_ty);
        tb->insert_before(rd, t);
        rank_lineage.insert(rd, i); // TAG the per-rank reduce (plan_tensor_pipeline_partitioned reads this → PlanStage.provider)
        reduces.push_back(rd);
    }
    Value* acc = reduces[0]->result(0U);
    for (crd::i32 i = 1; i < extent; ++i)
    {
        Operation* const ew = tensor::build_elementwise(ctx, acc, reduces[static_cast<usize>(i)]->result(0U), comb_attr, reduce_ty);
        tb->insert_before(ew, t);
        acc = ew->result(0U);
    }
    // RAUW the all_reduce's result → the combine, then erase the old chain CONSUMER-FIRST (each erase legal — the prior cleared
    // the last use): ar (result now unused) → r (only ar read it) → ts (only r read it) → t (only ts read it, if now dead).
    ar->result(0U)->replace_all_uses_with(acc);
    ar->erase();
    r->erase();
    ts->erase();
    if (!t->result(0U)->has_uses()) { t->erase(); }
    strip_dead_meshes(ctx, m); // ⛔ NO dist ops survive — plan_tensor_pipeline_partitioned consumes only tensor/resource ops
    return LowerResult{static_cast<crd::u32>(extent), false};
}
} // namespace crd::ceir::gpu
