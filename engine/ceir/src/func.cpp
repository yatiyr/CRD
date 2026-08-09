#include <crd/ceir/func.hpp>

#include <crd/core/assert.hpp>

namespace crd::ceir::func
{
namespace
{
// The `sym_visibility` keyword MLIR uses; Public is the default and carries NO attribute (its absence == public).
[[nodiscard]] containers::StringView visibility_keyword(Visibility vis) noexcept
{
    if (vis == Visibility::Private) { return containers::StringView("private"); }
    if (vis == Visibility::Nested) { return containers::StringView("nested"); }
    return {}; // Public
}
} // namespace

Operation* create_func(Context& ctx, Module& module, containers::StringView name, Visibility vis, u32 num_params,
                       TypeId param_type)
{
    SymbolTable* const symbols = module.symbols();
    CRD_ASSERT_MSG(symbols != nullptr, "module has no symbol table");
    if (name.empty() || symbols->contains(name)) { return nullptr; } // no anonymous funcs; no silent redefinition
    Operation* const op    = ctx.create_operation(func_kind(ctx), {}, 0U, {}, 1U); // no results; one body region
    Block* const     entry = ctx.create_block(num_params, param_type);             // params = the entry block's args
    op->region(0)->append(entry);
    // The func's IDENTITY rides ON the op as attributes (MLIR's model: the SymbolTable is an INDEX built over
    // `sym_name`, not the source of truth). This makes the name/visibility part of the canonical text, so it prints
    // and round-trips through the generic attribute machinery — the parser rebuilds the table from these attrs.
    ctx.set_attr(op, "sym_name", ctx.attr_string(name));
    const containers::StringView vis_kw = visibility_keyword(vis);
    if (!vis_kw.empty()) { ctx.set_attr(op, "sym_visibility", ctx.attr_string(vis_kw)); }
    const bool ok = symbols->define(ctx.intern_symbol(name), op, vis);
    CRD_ASSERT_MSG(ok, "define after a passing contains() check must succeed");
    (void)ok;
    return op;
}

Block* func_body_block(Operation* func_op) noexcept
{
    if (func_op == nullptr || func_op->num_regions() == 0U) { return nullptr; }
    Region* const body = func_op->region(0);
    return body != nullptr ? body->first_block() : nullptr;
}

Operation* create_return(Context& ctx, containers::ConstSpan<Value*> values)
{
    return ctx.create_operation(return_kind(ctx), values, 0U, {}, 0U);
}

Operation* create_call(Context& ctx, containers::StringView callee, containers::ConstSpan<Value*> args,
                       u32 num_results, TypeId result_type)
{
    Operation* const op = ctx.create_operation(call_kind(ctx), args, num_results, result_type, 0U);
    ctx.set_attr(op, "callee", ctx.attr_symbol(callee)); // the callee is a SymbolRef ATTRIBUTE (CEIR-1c)
    return op;
}

Operation* resolve_call(const Context& ctx, const Operation* call, const SymbolTable& table)
{
    const AttrId id = call->attr("callee");
    if (!id.valid()) { return nullptr; }
    const AttrValue v = ctx.attr_value(id);
    if (v.kind != AttrKind::SymbolRef) { return nullptr; }
    const SymbolEntry* const e = table.lookup(v.s);
    return e != nullptr ? e->op : nullptr;
}

namespace
{
// A func.return is a block terminator — it must live inside a block (a trivial CEIR-1d verifier-hook demonstration).
bool verify_func_return(const Context& /*ctx*/, const Operation& op) noexcept { return op.parent_block() != nullptr; }

// ⭐ CEIR-5c: the `func.call` EffectsFn hook (§34). A call's effective effects are its CALLEE's — resolve through the
// query's SymbolTable, then union the callee body's families (nested regions + further calls handled by the shared
// `collect_region_effective_mask` recursion). The `visited` set guards RECURSION (a callee already on the collection
// stack contributes nothing new — a cyclic call graph terminates). ⛔ An UNRESOLVED callee ⇒ the full `ExternalCall`
// barrier (genuinely-unmodeled code); an unregistered op REACHED inside the callee degrades the same way (handled by
// `collect_effective_mask`). Returns true (handled) — except when no table is present, where it DECLINES so the static
// `ExternalCall` fallback stands (the conservative no-table baseline).
bool call_effects_fn(const Context& ctx, const Operation& call, const EffectQuery& q, u32& mask)
{
    if (q.symbols == nullptr) { return false; } // no resolver ⇒ decline ⇒ the registered static ExternalCall stands
    Operation* const callee = resolve_call(ctx, &call, *q.symbols);
    if (callee == nullptr)
    {
        mask |= effect_family_bit(EffectFamily::ExternalCall); // unresolved symbol ⇒ opaque barrier
        return true;
    }
    if (q.visited != nullptr)
    {
        if (q.visited->contains(callee)) { return true; }   // recursion: this callee is already a union member up-stack
        q.visited->insert(callee, static_cast<u8>(1));
    }
    Region* const body = callee->num_regions() > 0U ? callee->region(0) : nullptr;
    if (body != nullptr) { ctx.collect_region_effective_mask(*body, q, mask); }
    return true;
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    Dialect* const d = ctx.register_dialect("func");
    d->register_op("func", {.traits = flags_of(OpTrait::Symbol)});
    // ⛔ func.call transfers control to a callee whose effects are not modelled until CEIR-5 wires a callee-derived
    // `EffectsFn`. Declare a CONSERVATIVE `ExternalCall` barrier (§26 lists names without prose; ExternalCall = "control
    // leaves to code we don't model") so the CEIR-4d hazard analysis never mistakes a call for effect-free/reorderable —
    // the EMPTY≠UNKNOWN contract makes an empty span on a REGISTERED op mean "provably effect-free", which a call is not.
    static constexpr EffectRecord kCallEffects[] = {{EffectFamily::ExternalCall, EffectTarget::None, 0U, 0U}};
    // ⭐ CEIR-5c: the static ExternalCall is the CONSERVATIVE no-table baseline; the EffectsFn hook REFINES it to the
    // callee's precise families when a resolver (SymbolTable) is available (the A/B pair — see Context::ops_hazard).
    d->register_op("call",
                   {.effects = containers::ConstSpan<EffectRecord>(kCallEffects, 1U), .effects_fn = &call_effects_fn});
    d->register_op("return", {.traits = flags_of(OpTrait::Terminator), .verify = &verify_func_return});
    return d;
}

// ── §34 recursion policy (CEIR-5c) ──
containers::StringView recursion_policy_name(RecursionPolicy p) noexcept
{
    switch (p)
    {
    case RecursionPolicy::Unspecified: return containers::StringView("unspecified");
    case RecursionPolicy::None: return containers::StringView("none");
    case RecursionPolicy::Bounded: return containers::StringView("bounded");
    case RecursionPolicy::Unbounded: return containers::StringView("unbounded");
    }
    return containers::StringView("?");
}

containers::StringView recursion_violation_kind_name(RecursionViolationKind k) noexcept
{
    switch (k)
    {
    case RecursionViolationKind::None: return containers::StringView("none");
    case RecursionViolationKind::DeclaredNoneRecurses: return containers::StringView("declared-none-recurses");
    case RecursionViolationKind::BoundedMissingDepth: return containers::StringView("bounded-missing-depth");
    case RecursionViolationKind::InvalidPolicyAttr: return containers::StringView("invalid-policy-attr");
    }
    return containers::StringView("?");
}

void set_recursion_policy(Context& ctx, Operation* func_op, RecursionPolicy policy, u32 max_depth)
{
    ctx.set_attr(func_op, "recursion", ctx.attr_int(static_cast<i64>(policy)));
    // ⛔ ALWAYS overwrite the depth (never guard on max_depth>0): a re-declaration from Bounded(4) to Bounded(0) must
    // CLEAR the old bound, else BoundedMissingDepth can't fire (the declared-words-must-be-validated scar, at the setter).
    ctx.set_attr(func_op, "recursion_max_depth", ctx.attr_int(static_cast<i64>(max_depth)));
}

bool recursion_policy_of(const Context& ctx, const Operation& func_op, RecursionPolicy& out) noexcept
{
    const AttrId id = func_op.attr("recursion");
    if (!id.valid())
    {
        out = RecursionPolicy::Unspecified; // absent ⇒ no claim (the common case)
        return true;
    }
    const AttrValue v = ctx.attr_value(id);
    if (v.kind != AttrKind::Int || v.i < 0 || v.i > static_cast<i64>(RecursionPolicy::Unbounded)) { return false; }
    out = static_cast<RecursionPolicy>(v.i);
    return true;
}

u32 recursion_max_depth_of(const Context& ctx, const Operation& func_op) noexcept
{
    const AttrId id = func_op.attr("recursion_max_depth");
    if (!id.valid()) { return 0U; }
    const AttrValue v = ctx.attr_value(id);
    if (v.kind != AttrKind::Int || v.i < 0) { return 0U; }
    return static_cast<u32>(v.i);
}

namespace
{
// Append every RESOLVED callee (a defining func.func op) reached from region `r` — walks `r` incl. nested regions for
// `func.call` ops and resolves each against `table`. Unresolved calls are skipped (an unverifiable edge — single-module).
void collect_callees(const Context& ctx, Region* r, const SymbolTable& table, containers::Array<Operation*>& out)
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == containers::StringView("func.call"))
            {
                Operation* const callee = resolve_call(ctx, op, table);
                if (callee != nullptr) { out.push_back(callee); }
            }
            for (u32 i = 0; i < op->num_regions(); ++i) { collect_callees(ctx, op->region(i), table, out); }
        }
    }
}

// Does `cur` reach `target` through the resolved call graph? DFS with a `seen` func set (the union guard — a diamond
// visits a shared callee once; a cycle terminates). Self-recursion: `cur`'s own callee == target on the first hop.
bool call_reaches(const Context& ctx, Operation* cur, Operation* target, const SymbolTable& table,
                  containers::HashMap<const Operation*, u8>& seen)
{
    containers::Array<Operation*> callees(ctx.allocator());
    Region* const                body = cur->num_regions() > 0U ? cur->region(0) : nullptr;
    collect_callees(ctx, body, table, callees);
    for (u32 i = 0; i < static_cast<u32>(callees.size()); ++i)
    {
        Operation* const c = callees[i];
        if (c == target) { return true; }
        if (seen.contains(c)) { continue; }
        seen.insert(c, static_cast<u8>(1));
        if (call_reaches(ctx, c, target, table, seen)) { return true; }
    }
    return false;
}

// Collect the Symbol-defining ops (func.func) under region `r` in PRE-ORDER (the printer's deterministic walk).
void collect_symbol_ops(const Context& ctx, Region* r, containers::Array<Operation*>& out)
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.has_trait(op->kind(), OpTrait::Symbol)) { out.push_back(op); }
            for (u32 i = 0; i < op->num_regions(); ++i) { collect_symbol_ops(ctx, op->region(i), out); }
        }
    }
}
} // namespace

RecursionViolation find_recursion_violation(const Context& ctx, const Module& m, const SymbolTable& table)
{
    containers::Array<Operation*> funcs(ctx.allocator());
    collect_symbol_ops(ctx, m.body(), funcs);
    for (u32 i = 0; i < static_cast<u32>(funcs.size()); ++i)
    {
        Operation* const f = funcs[i];
        RecursionPolicy  p = RecursionPolicy::Unspecified;
        if (!recursion_policy_of(ctx, *f, p)) { return {f, RecursionViolationKind::InvalidPolicyAttr}; }
        if (p == RecursionPolicy::Bounded && recursion_max_depth_of(ctx, *f) < 1U)
        {
            return {f, RecursionViolationKind::BoundedMissingDepth}; // ⛔ declared words must be validated
        }
        if (p == RecursionPolicy::None)
        {
            containers::HashMap<const Operation*, u8> seen(ctx.allocator());
            if (call_reaches(ctx, f, f, table, seen)) { return {f, RecursionViolationKind::DeclaredNoneRecurses}; }
        }
    }
    return {};
}
} // namespace crd::ceir::func
