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

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

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
    crd::memory::GrowableTlsfAllocator root;
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

// CEIR-31b-3-c-i: a NAME with non-ident chars round-trips through print→parse via the QUOTED "..." form (MLIR's
// convention), while a plain-identifier name still prints BARE (byte-identical to before). This covers BOTH ceir-core
// asymmetries the first engine://-id'd, param-carrying frame exposed: (1) a SYMBOL ref (@"engine://ui/blur", an asset
// id — ':' and '/'), and (2) an op-ATTR NAME ("p:clear_color:0", the frame param-bag key encoding — ':'). Before this
// fix, to_ceir_frame printed both bare, and the parser's lexer (is_ident_char) stopped at the ':' — the ceir TEXT
// round-trip was broken for EVERY engine://-id'd frame (its name, every shader id, and every per-pass param). One fix,
// one helper (name_needs_quote), symmetric on the printer and the parser. Host-only, device-free.
TEST_CASE("ceir roundtrip: a non-ident symbol/attr-name round-trips via the quoted form; plain ones stay bare",
          "[ceir][roundtrip]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    Module* const                      m   = ctx.create_module();
    Block* const                       top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const op = ctx.create_operation(ctx.intern_op("test", "sym"), {}, 0U);
    ctx.set_attr(op, "bare", ctx.attr_symbol("bare_target"));     // a plain-ident symbol → prints BARE
    ctx.set_attr(op, "uri", ctx.attr_symbol("engine://ui/blur")); // ':' and '/' → symbol prints QUOTED
    ctx.set_attr(op, "flag", ctx.attr_bool(true));                // a plain-ident attr NAME → prints BARE
    ctx.set_attr(op, "p:clear_color:0", ctx.attr_int(1));         // ':' in the attr NAME → prints QUOTED
    top->append(op);

    const String t1 = print(ctx, *m, &root);
    CHECK(std::strstr(t1.c_str(), "bare = @bare_target") != nullptr);         // BARE symbol — unchanged form
    CHECK(std::strstr(t1.c_str(), "uri = @\"engine://ui/blur\"") != nullptr); // QUOTED symbol (non-ident asset id)
    CHECK(std::strstr(t1.c_str(), "flag = true") != nullptr);                 // BARE attr name — unchanged form
    CHECK(std::strstr(t1.c_str(), "\"p:clear_color:0\" = 1") != nullptr);     // QUOTED attr name (frame param key)

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const String t2 = print(ctx2, *pr.module, &root);
    CHECK(bytes_equal(t1, t2)); // print(parse(print)) fixpoint — both quoted forms re-parse to the identical name
}

TEST_CASE("ceir roundtrip: parsing is deterministic across two independent parses", "[ceir][roundtrip]")
{
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;

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
    CHECK(rejects("module { ^bb0: test.x() {\"\" = 1} }"));   // CEIR-31b-3-c-ii: an EMPTY quoted attr name — nameless attr
    CHECK(rejects("module { ^bb0: test.x() {s = @\"\"} }"));  // CEIR-31b-3-c-ii: an EMPTY quoted symbol ref — never printable
}
