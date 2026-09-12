#include <crd/ceir/time.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::time
{
namespace
{
// A time domain's verify hook (the crd-units Time seed): an `Extern` time type wraps EXACTLY ONE underlying member of a
// NUMERIC/QUANTITY kind — Int/Float/Quantity, or a TypeParam so a generic time type `f<T>(x: time.wall<T>)` verifies
// (the 8a substitute-through-Extern precedent). 0/>1 members, or a non-numeric member (`time.wall<!string>`), reject.
bool verify_time_domain(const Context& ctx, const Type& t) noexcept
{
    if (t.members.size() != 1U) { return false; }
    const TypeKind k = ctx.type_of(t.members[0]).kind;
    return k == TypeKind::Int || k == TypeKind::Float || k == TypeKind::Quantity || k == TypeKind::TypeParam;
}

// The six built-in U-§17 domains. ⛔ append-at-end is fine — an open-world set (a plugin adds `game.turn` freely).
constexpr const char* kDomains[] = {"wall", "sim", "frame", "audio_sample", "sequencer", "logical"};
} // namespace

Dialect* register_dialect(Context& ctx)
{
    Dialect* const d = ctx.register_dialect("time"); // idempotent
    for (const char* const name : kDomains)
    {
        (void)d->register_type_class(name, TypeClassSpec{&verify_time_domain, 1U}); // idempotent by class
    }
    return d;
}

TypeClassId domain(Context& ctx, containers::StringView name) { return ctx.intern_type_class("time", name); }

TypeId time_type(Context& ctx, TypeClassId cls, TypeId underlying)
{
    Type         p;
    const TypeId m[1] = {underlying};
    p.members         = containers::ConstSpan<TypeId>(m, 1U);
    return ctx.type_extern(cls, p); // the 8a Extern factory (asserts the class's verify hook)
}
} // namespace crd::ceir::time
