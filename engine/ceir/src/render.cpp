#include <crd/ceir/render.hpp>

#include <crd/ceir/dialect.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::render
{
namespace
{
// An attachment's verify hook (CEIR-14a): an `Extern` color/depth attachment type wraps EXACTLY ONE underlying member
// that is an IMAGE (the color/depth target), or a VIEW whose underlying is an Image (the §41 mip/layer/aspect
// subresource case), or a TypeParam (the 8a substitute-through-Extern precedent, so a generic attachment type
// `f<T>(a: render.color_attachment<T>)` verifies). 0/>1 members, or a non-image member (a buffer/scalar attachment),
// reject. ⛔ color vs depth share this hook — the ROLE is the CLASS (a distinct TypeClassId), not the member shape;
// the color/depth FORMAT specificity (the typed-clear-vs-format check) is find_render_misuse's, named-forward.
bool verify_attachment(const Context& ctx, const Type& t) noexcept
{
    if (t.members.size() != 1U) { return false; }
    const Type u = ctx.type_of(t.members[0]);
    if (u.kind == TypeKind::Image || u.kind == TypeKind::TypeParam) { return true; }
    if (u.kind == TypeKind::View && u.members.size() >= 1U) { return ctx.type_of(u.members[0]).kind == TypeKind::Image; }
    return false;
}

[[nodiscard]] TypeId attachment_type(Context& ctx, TypeClassId cls, TypeId image)
{
    Type         p;
    const TypeId m[1] = {image};
    p.members         = containers::ConstSpan<TypeId>(m, 1U);
    return ctx.type_extern(cls, p); // the 8a Extern factory (asserts the class's verify hook)
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    Dialect* const d = register_render_ops(ctx); // generated: the render dialect + its ops (idempotent)
    (void)d->register_type_class("color_attachment", TypeClassSpec{&verify_attachment, 1U}); // idempotent by class
    (void)d->register_type_class("depth_attachment", TypeClassSpec{&verify_attachment, 1U});
    return d;
}

TypeClassId color_attachment_class(Context& ctx) { return ctx.intern_type_class("render", "color_attachment"); }
TypeClassId depth_attachment_class(Context& ctx) { return ctx.intern_type_class("render", "depth_attachment"); }

TypeId type_color_attachment(Context& ctx, TypeId image) { return attachment_type(ctx, color_attachment_class(ctx), image); }
TypeId type_depth_attachment(Context& ctx, TypeId image) { return attachment_type(ctx, depth_attachment_class(ctx), image); }
} // namespace crd::ceir::render
