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
} // namespace

Dialect* register_dialect(Context& ctx)
{
    Dialect* const d = ctx.register_dialect("func");
    d->register_op("func", flags_of(OpTrait::Symbol));
    d->register_op("call");
    d->register_op("return", flags_of(OpTrait::Terminator), &verify_func_return);
    return d;
}
} // namespace crd::ceir::func
