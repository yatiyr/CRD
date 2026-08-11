#include <crd/ceir/frame.hpp>

#include <crd/ceir/dialect.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::frame
{
namespace
{
// The draw-list verify hook (CEIR-15a): a `draw_list` value is a MARKER Extern type with ZERO members — an ECS-query
// handle. The query (all/any/none/cull/sort/limit) lives in the `frame.draw_list` op's ATTRS, not the type; the host
// resolves the handle to pre-resolved DrawItems at execute (the IFrameGraphHost seam), so the IR carries no scene type.
bool verify_draw_list(const Context& /*ctx*/, const Type& t) noexcept { return t.members.size() == 0U; }
} // namespace

Dialect* register_dialect(Context& ctx)
{
    Dialect* const d = register_frame_ops(ctx); // generated: the frame dialect + its ops (idempotent)
    (void)d->register_type_class("draw_list", TypeClassSpec{&verify_draw_list, 0U}); // idempotent by class
    return d;
}

TypeClassId draw_list_class(Context& ctx) { return ctx.intern_type_class("frame", "draw_list"); }

TypeId type_draw_list(Context& ctx)
{
    const Type p; // zero members — a marker handle type (the query is in the op's attrs)
    return ctx.type_extern(draw_list_class(ctx), p); // the 8a Extern factory (asserts the class's verify hook)
}
} // namespace crd::ceir::frame
