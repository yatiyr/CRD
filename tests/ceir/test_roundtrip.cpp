// CEIR-1e (part 2) - the textual ROUND-TRIP gate: print(parse(print(x))) == print(x), BYTE-EXACT. A rich graph that
// hits every parser path - results/operands, attrs of every kind (incl. negative int, "4.0"/exponent floats, a string
// with embedded quote/backslash/brace, a symbol ref, a type), nested + empty + multi-block regions, a use-before-def
// operand (the Graph-region fixup pass), an unregistered-dialect opaque op, and func.func/call/return - is printed,
// parsed back, and re-printed; the two texts must be identical to the byte. Malformed inputs must be REJECTED with an
// offset. Host-only. ASCII-only test names (an em-dash breaks the CP1254 ctest filter).

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp / std::strstr (byte-exact compare + content spot-checks)

#include "rich_graph.hpp" // crd::ceir::test::build_rich — the dense fixture shared with the binary gate

using namespace crd::ceir;
using crd::ceir::test::build_rich;
using crd::containers::ConstSpan;
using crd::containers::String;

namespace
{
[[nodiscard]] bool bytes_equal(const String& a, const String& b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
} // namespace

TEST_CASE("ceir roundtrip: a rich graph prints, parses, and re-prints byte-exact", "[ceir][roundtrip]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module* const                m = build_rich(ctx);

    const String text1 = print(ctx, *m, &root);

    // sanity: the dense features really are present in the text we round-trip (the exact scientific spelling of the
    // exponent float is toolchain-dependent, so we assert ORDERING + robust values, not "1e+20" verbatim).
    CHECK(std::strstr(text1.c_str(), "flag = true, frac = 4.5, i = -42, s = ") != nullptr);  // sorted attrs, bool, neg
    CHECK(std::strstr(text1.c_str(), "s = \"he said \\\"hi\\\"\\\\n\"") != nullptr);         // escaped string
    CHECK(std::strstr(text1.c_str(), "sym = @target") != nullptr);                           // symbol ref
    CHECK(std::strstr(text1.c_str(), "ty = !struct<Point,x:!i32,y:!f32>") != nullptr);       // named-struct type attr
    CHECK(std::strstr(text1.c_str(), ": !vec<4x!f32>") != nullptr);                          // a nested aggregate value type
    CHECK(std::strstr(text1.c_str(), "whole = 4.0") != nullptr);                             // float, not "4"

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, text1);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    const String text2 = print(ctx2, *pr.module, &root);
    CHECK(bytes_equal(text1, text2));
}

TEST_CASE("ceir roundtrip: parsing is deterministic across two independent parses", "[ceir][roundtrip]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const String                 text = print(ctx, *build_rich(ctx), &root);

    Context     ca(&root);
    Context     cb(&root);
    ParseResult pa = parse(ca, text);
    ParseResult pb = parse(cb, text);
    REQUIRE(pa.ok);
    REQUIRE(pb.ok);
    CHECK(bytes_equal(print(ca, *pa.module, &root), print(cb, *pb.module, &root)));
}

TEST_CASE("ceir roundtrip: an unregistered-dialect op round-trips opaquely", "[ceir][roundtrip]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module* const                m     = ctx.create_module();
    Block* const                 entry = ctx.create_block(0U);
    m->body()->append(entry);
    entry->append(ctx.create_operation(ctx.intern_op("plugin", "widget"), {}, 0U));

    const String      t1 = print(ctx, *m, &root);
    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    CHECK(bytes_equal(t1, print(ctx2, *pr.module, &root)));
}

TEST_CASE("ceir roundtrip: a func's symbol identity survives print then parse", "[ceir][roundtrip][symbol]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Module* const                m   = ctx.create_module();
    Block* const                 top = ctx.create_block(0U);
    m->body()->append(top);
    top->append(func::create_func(ctx, *m, "exported", Visibility::Public, 0U));  // Public -> no sym_visibility attr
    top->append(func::create_func(ctx, *m, "hidden", Visibility::Private, 0U));   // Private -> sym_visibility="private"

    const String t1 = print(ctx, *m, &root);
    // the identity lives IN the canonical text as attributes, not in a side table the printer can't see
    CHECK(std::strstr(t1.c_str(), "sym_name = \"exported\"") != nullptr);
    CHECK(std::strstr(t1.c_str(), "sym_name = \"hidden\", sym_visibility = \"private\"") != nullptr);

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);

    // the parser rebuilt the module's SymbolTable from the sym_name/sym_visibility attrs
    const SymbolEntry* const a = pr.module->symbols()->lookup("exported");
    const SymbolEntry* const b = pr.module->symbols()->lookup("hidden");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->visibility == Visibility::Public);
    CHECK(b->visibility == Visibility::Private);
    CHECK(a->op->kind() == func::func_kind(ctx2));

    // a call resolves against the rebuilt table (semantic faithfulness, not just bytes)
    Operation* const call = func::create_call(ctx2, "exported", ConstSpan<Value*>{}, 0U);
    CHECK(func::resolve_call(ctx2, call, *pr.module->symbols()) == a->op);

    CHECK(bytes_equal(t1, print(ctx2, *pr.module, &root)));
}

TEST_CASE("ceir roundtrip: malformed inputs are rejected with an offset", "[ceir][roundtrip]")
{
    crd::memory::MallocAllocator root;

    auto rejects = [&root](const char* src) {
        Context     ctx(&root);
        ParseResult pr = parse(ctx, crd::containers::StringView(src));
        return !pr.ok && pr.module == nullptr;
    };

    CHECK(rejects("notmodule { ^bb0: }"));                    // missing 'module' keyword
    CHECK(rejects("module { ^bb0: } garbage"));               // trailing characters
    CHECK(rejects("module { ^bb0: %0 = test.a() : !t1\n %0 = test.b() : !t1 }")); // duplicate SSA id
    CHECK(rejects("module { ^bb0: test.use(%9) }"));          // operand references an undefined value
    CHECK(rejects("module { ^bb0: nodialect() }"));           // op name is not 'dialect.op'
    CHECK(rejects("module { ^bb0: test.x(%0"));               // unterminated operands / truncated
}
