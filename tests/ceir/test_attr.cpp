// CEIR-1c — the attribute + provenance gate: interned typed attribute VALUES (dedup), the per-op AttrDict (set /
// overwrite / lookup), the func.call SymbolRef attribute (which dissolves the CEIR-1b side-table), the source map
// (file registration + dedup), and SourceLoc provenance. Host-only, device-free.

#include <crd/ceir/ceir.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
namespace fn = crd::ceir::func;

TEST_CASE("ceir attr: identical values intern to one AttrId; kind + value both matter", "[ceir][attr]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    CHECK(ctx.attr_int(42) == ctx.attr_int(42));                     // dedup
    CHECK(ctx.attr_int(42) != ctx.attr_int(43));
    CHECK(ctx.attr_string("hello") == ctx.attr_string("hello"));     // dedup by CONTENT (distinct arena copies)
    CHECK(ctx.attr_string("hello") != ctx.attr_string("world"));
    CHECK(ctx.attr_bool(true) != ctx.attr_bool(false));
    CHECK(ctx.attr_int(1) != ctx.attr_bool(true));                   // same bits, different KIND
    CHECK(ctx.attr_symbol("f") != ctx.attr_string("f"));             // SymbolRef vs String
    CHECK(ctx.attr_float(1.5) == ctx.attr_float(1.5));               // exact by bit pattern
    CHECK(ctx.attr_float(1.5) != ctx.attr_float(2.5));
}

TEST_CASE("ceir attr: typed values round-trip through the intern table", "[ceir][attr]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    const AttrValue i = ctx.attr_value(ctx.attr_int(-7));
    CHECK(i.kind == AttrKind::Int);
    CHECK(i.i == -7);

    const AttrValue f = ctx.attr_value(ctx.attr_float(3.25));
    CHECK(f.kind == AttrKind::Float);
    CHECK(f.as_float() == 3.25);

    const AttrValue b = ctx.attr_value(ctx.attr_bool(true));
    CHECK(b.kind == AttrKind::Bool);
    CHECK(b.b);

    const AttrValue s = ctx.attr_value(ctx.attr_string("text"));
    CHECK(s.kind == AttrKind::String);
    CHECK(s.s == crd::containers::StringView{"text"});

    const TypeId    ity = ctx.type_i32();
    const AttrValue t   = ctx.attr_value(ctx.attr_type(ity));
    CHECK(t.kind == AttrKind::Type);
    CHECK(t.t == ity);

    const AttrValue none = ctx.attr_value(AttrId{}); // invalid id → Int(0) sentinel
    CHECK(none.kind == AttrKind::Int);
    CHECK(none.i == 0);
}

TEST_CASE("ceir attr: an op's attribute dict - set, lookup by name, overwrite-in-place", "[ceir][attr]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    Operation* op = ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U);
    REQUIRE(op != nullptr);
    CHECK(op->num_attrs() == 0U);
    CHECK(op->has_attr("x") == false);
    CHECK(op->attr("x").valid() == false);

    const AttrId seven = ctx.attr_int(7);
    ctx.set_attr(op, "x", seven);
    CHECK(op->num_attrs() == 1U);
    CHECK(op->has_attr("x"));
    CHECK(op->attr("x") == seven);
    CHECK(op->attr_name(0) == crd::containers::StringView{"x"});
    CHECK(op->attr_id_at(0) == seven);

    ctx.set_attr(op, "y", ctx.attr_bool(true));
    CHECK(op->num_attrs() == 2U);

    const AttrId nine = ctx.attr_int(9); // overwrite "x": no new slot, value updated
    ctx.set_attr(op, "x", nine);
    CHECK(op->num_attrs() == 2U);
    CHECK(op->attr("x") == nine);
    CHECK(op->attr("y").valid()); // untouched
}

TEST_CASE("ceir attr: func.call's callee is a SymbolRef attribute (dissolves the CEIR-1b side-table)",
          "[ceir][attr][func]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module*                      m = ctx.create_module();

    Operation* callee = fn::create_func(ctx, *m, "callee", Visibility::Public, 0U);
    Operation* call   = fn::create_call(ctx, "callee", crd::containers::ConstSpan<Value*>{}, 0U);
    REQUIRE(callee != nullptr);
    REQUIRE(call != nullptr);

    const AttrId cid = call->attr("callee"); // the callee rides a named SymbolRef attribute now
    REQUIRE(cid.valid());
    CHECK(ctx.attr_value(cid).kind == AttrKind::SymbolRef);
    CHECK(ctx.attr_value(cid).s == crd::containers::StringView{"callee"});
    CHECK(fn::resolve_call(ctx, call, *m->symbols()) == callee); // still resolves, now via the attribute
}

TEST_CASE("ceir source-map: file registration dedups; ids stable; SourceLoc provenance round-trips",
          "[ceir][provenance]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    const crd::u32 a = ctx.register_file("a.crd");
    const crd::u32 b = ctx.register_file("b.crd");
    CHECK(a == 1U);
    CHECK(b == 2U);
    CHECK(ctx.register_file("a.crd") == a); // dedup by path
    CHECK(ctx.file_path(a) == crd::containers::StringView{"a.crd"});
    CHECK(ctx.file_path(b) == crd::containers::StringView{"b.crd"});
    CHECK(ctx.file_path(0U).empty());  // 0 = unknown
    CHECK(ctx.file_path(99U).empty()); // out of range
    CHECK(ctx.register_file("") == 0U);

    // provenance on an op: SourceLoc carries a REAL file_id from the map
    Operation* op = ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U);
    op->set_loc(SourceLoc{a, 12U, 3U});
    CHECK(op->loc().file_id == a);
    CHECK(op->loc().line == 12U);
    CHECK(op->loc().col == 3U);
    CHECK(ctx.file_path(op->loc().file_id) == crd::containers::StringView{"a.crd"});
}
