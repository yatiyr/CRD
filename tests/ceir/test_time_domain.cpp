// CEIR-8f (ADR-0116, U-§17) — typed time domains. ⛔ A time domain IS an 8a TYPE-CLASS (no TimeDomainId). The `time`
// dialect registers six built-ins; a time type is an Extern type of the domain class over one underlying member. Because
// different type-classes with identical params are different TypeIds (the ADR-0111 landmine), time.wall<T> != time.sim<T>
// — the type distinction a future operand-type checker rejects mixing on (enforcement named-forward). A plugin domain
// (game.turn) works with ZERO central edits; the verify hook rejects a 0/2-member type; U-§56 round-trip. Host-only. ASCII.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/time.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::u8;
using crd::usize;
using ByteArray = crd::containers::Array<u8>;

namespace
{
bool verify_one_member(const Context&, const Type& t) noexcept { return t.members.size() == 1U; }
[[nodiscard]] ConstSpan<u8> span(const ByteArray& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
} // namespace

TEST_CASE("ceir 8f: time domains are distinct 8a type-classes (wall != sim); a plugin domain works", "[ceir][time]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    time::register_dialect(ctx);
    const TypeClassId wall = time::domain(ctx, "wall");
    const TypeClassId sim  = time::domain(ctx, "sim");
    const TypeId      f64  = ctx.type_f64();

    const TypeId t_wall = time::time_type(ctx, wall, f64);
    const TypeId t_sim  = time::time_type(ctx, sim, f64);
    CHECK(t_wall != t_sim);                              // ⛔ same underlying, different domain -> DIFFERENT TypeIds
    CHECK(t_wall == time::time_type(ctx, wall, f64));    // dedup (same domain + underlying)
    CHECK(time::time_type(ctx, sim, ctx.type_i32()) != t_sim); // different underlying -> different type

    // a PLUGIN clock domain (game.turn) — ZERO central edits, just a registered type-class under a plugin dialect.
    Dialect* const    g    = ctx.register_dialect("game");
    const TypeClassId turn = g->register_type_class("turn", TypeClassSpec{&verify_one_member, 1U});
    const TypeId      t_turn = time::time_type(ctx, turn, f64);
    CHECK(t_turn != t_wall); // a plugin domain is distinct from every built-in
}

TEST_CASE("ceir 8f: a time domain's verify hook rejects a 0- or 2-member type", "[ceir][time]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    time::register_dialect(ctx);
    const TypeClassId wall = time::domain(ctx, "wall");

    Type zero;
    zero.kind       = TypeKind::Extern;
    zero.type_class = wall; // 0 members
    CHECK_FALSE(ctx.verify_extern(zero));
    Type two;
    const TypeId m2[2] = {ctx.type_f64(), ctx.type_i32()};
    two.kind        = TypeKind::Extern;
    two.type_class  = wall;
    two.members     = ConstSpan<TypeId>(m2, 2U);
    CHECK_FALSE(ctx.verify_extern(two)); // exactly-one-member is the units-Time seed invariant
}

TEST_CASE("ceir 8f: a time-typed module round-trips text and binary byte-exact (U-56, inherited from 8a)", "[ceir][time]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    time::register_dialect(ctx);
    const TypeId  t_wall = time::time_type(ctx, time::domain(ctx, "wall"), ctx.type_f64());
    Module* const m      = ctx.create_module();
    Block* const  b      = ctx.create_block(1U, t_wall); // a block arg carries the time type (exercises the TYPE record)
    m->body()->append(b);

    const ByteArray blob1 = serialize(ctx, *m, &root);
    Context         ctx2(&root);
    time::register_dialect(ctx2); // the time classes must be registered in the decode ctx (registered-round-trip)
    const ParseResult pr = deserialize(ctx2, span(blob1));
    REQUIRE(pr.ok);
    CHECK(blob_eq(blob1, serialize(ctx2, *pr.module, &root)));
}
