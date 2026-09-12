#include <crd/ceir/detail/symbol_registration.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/symbol_table.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::detail
{
namespace
{
[[nodiscard]] bool sv_is(containers::StringView a, const char* b) noexcept
{
    return a == containers::StringView(b);
}
} // namespace

bool register_symbol(Context& ctx, Module& module, Operation* op) noexcept
{
    const AttrId name_id = op->attr("sym_name");
    if (!name_id.valid()) { return true; } // not a symbol-defining op
    const AttrValue nv = ctx.attr_value(name_id);
    if (nv.kind != AttrKind::String) { return true; }

    Visibility   vis    = Visibility::Public;
    const AttrId vis_id = op->attr("sym_visibility");
    if (vis_id.valid())
    {
        const AttrValue vv = ctx.attr_value(vis_id);
        if (vv.kind == AttrKind::String)
        {
            if (sv_is(vv.s, "private")) { vis = Visibility::Private; }
            else if (sv_is(vv.s, "nested")) { vis = Visibility::Nested; }
        }
    }
    SymbolTable* const symbols = module.symbols();
    if (symbols == nullptr) { return true; }
    return symbols->define(nv.s, op, vis); // false ⇒ duplicate name (a load error)
}
} // namespace crd::ceir::detail
