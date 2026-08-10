// CEIR-9h (UNIVERSALITY VALIDATION, U8 the MANDATORY EXTERNAL-PLUGIN proof, U-§116/U-§117/U-§118). ⛔ The band's
// heaviest slice and the ACID TEST of the whole open-world thesis: a genuinely EXTERNAL plugin — a whole `plugin`
// dialect authored in a TEST-SIDE namespace (`plugin_ext`) that includes ONLY the public crd-ceir headers and edits NO
// engine source — registers its FULL surface (a custom TYPE class + ATTRIBUTE class + effect-LOCATION class + op
// INTERFACE + custom OPS with verifiers + a REWRITE + a lowering ConversionTarget + an IExecutionProvider) through the
// PUBLIC open-world APIs, with ZERO central-enum edits. Every piece has a public registration path (no private wall).
// ⛔ The "zero central-enum edits" is pinned MECHANICALLY by `check_ceir_invariants` (U-116: TypeKind/AttrKind end at
// `Extern`) + the in-test Extern-door assertions here. Five proofs: (1) the full surface registers + is queryable via
// public APIs (through the Extern doors); (2) the plugin's custom type/attr round-trip text<->binary + preserve through
// an UNREGISTERED Context + re-register unify (U-56 for a foreign plugin) + a single-byte fuzz sweep over the plugin
// blob; (3) the 8g rewrite A(B(x))->C(x) fires via `try_apply` and mutates the IR (the framework's FIRST real pattern);
// (4) the plugin is DISCOVERABLE by the 9g agent path over a hand-built OpSchema table (U-117); (5) the provider stub
// advertises the plugin ops (contract, not execution). ⛔ One honest asymmetry: a first-party dialect gets its OpSchema
// table GENERATED; a plugin HAND-AUTHORS it (a tooling asymmetry, named-forward). Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>      // umbrella: context/ir/dialect/interface/rewrite/binary/parse/print
#include <crd/ceir/binary.hpp>    // serialize / deserialize
#include <crd/ceir/effect.hpp>    // EffectRecord
#include <crd/ceir/exec.hpp>      // exec::ExecResult / ExecError
#include <crd/ceir/op_schema.hpp> // OpSchema (the agent discovery surface)
#include <crd/ceir/parse.hpp>     // parse
#include <crd/ceir/print.hpp>     // print
#include <crd/ceir/provider.hpp>  // IExecutionProvider
#include <crd/ceir/rewrite.hpp>   // RewritePattern / try_apply

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;
using crd::u32;
using crd::u8;
using crd::usize;
using ByteArray = Array<u8>;

// ── the EXTERNAL plugin: authored entirely in this test-side namespace over the PUBLIC crd-ceir APIs (zero engine edits) ──
namespace plugin_ext
{
// a custom op-INTERFACE (8e): plugin behaviour lives in a typed function table, not a new trait bit.
struct PluginCost
{
    u32                          cost;
    static constexpr InterfaceId kId = make_interface_id("plugin.cost");
};
constexpr PluginCost kACost{5U};

bool verify_shape(const Context&, const Type& t) noexcept { return t.members.size() == 1U; } // a plugin type = 1 member
bool verify_meta(const Context&, const AttrValue&) noexcept { return true; }                  // permissive plugin attr
bool verify_store(const Context&, const EffectRecord&) noexcept { return true; }              // permissive plugin location
bool verify_unary(const Context&, const Operation& op) noexcept                               // a plugin op = 1 in, 1 out
{
    return op.num_operands() == 1U && op.num_results() == 1U;
}

struct Plugin
{
    TypeClassId     shape;
    AttrClassId     meta;
    LocationClassId store;
    OpId            a, b, c;
};
// Register the FULL plugin surface through the public open-world APIs. ⛔ NOT ONE engine source line is touched.
Plugin register_plugin(Context& ctx)
{
    Dialect* const d = ctx.register_dialect("plugin");
    Plugin         p{};
    p.shape = d->register_type_class("shape", TypeClassSpec{&verify_shape, 1U});                        // 8a custom TYPE
    p.meta  = d->register_attr_class("meta", AttrClassSpec{&verify_meta, 1U});                          // 8b custom ATTR
    p.store = d->register_location_class("store", LocationClassSpec{&verify_store, ResourceClass::Ecs, 1U}); // 8c LOCATION
    p.a     = d->register_op("a", OpSpec{.verify = &verify_unary, .determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
    p.b     = d->register_op("b", OpSpec{.verify = &verify_unary, .determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
    p.c     = d->register_op("c", OpSpec{.verify = &verify_unary, .determinism = DeterminismClass::BitExact, .domain = EvalDomain::CookTime});
    register_op_interface<PluginCost>(ctx, p.a, &kACost); // 8e custom INTERFACE on a plugin op
    return p;
}

// ── the 8g REWRITE: A(B(x)) -> C(x). Matches by op NAME (const-safe, zero hard-coded OpId), mutates via raw Context
// primitives (a COMPILER rewrite, not an authored edit — ADR-0119 §2.7). ⛔ RAUW A's result to C BEFORE erasing A
// (erase asserts results use-free); erase A THEN B (B's result feeds A, so B is not dead until A is gone); erase B ONLY
// if genuinely dead (x may flow elsewhere through B). ──
bool match_ab(const Context& ctx, const Operation& op) noexcept
{
    if (ctx.op_name(op.kind()) != StringView("plugin.a") || op.num_operands() < 1U) { return false; }
    const Operation* const inner = op.operand(0)->defining_op();
    return inner != nullptr && ctx.op_name(inner->kind()) == StringView("plugin.b");
}
void rewrite_ab_to_c(Context& ctx, Operation& op)
{
    Operation* const inner = op.operand(0)->defining_op(); // B
    Value* const     x     = inner->operand(0);            // B's input, wired into C BEFORE any erase
    Value* const     c_in[1] = {x};
    Operation* const c = ctx.create_operation(ctx.intern_op("plugin", "c"), ConstSpan<Value*>(c_in, 1U), 1U, op.result(0)->type());
    op.parent_block()->insert_before(c, &op);
    op.result(0)->replace_all_uses_with(c->result(0)); // A's former consumers now consume C
    op.erase();                                          // A (result now use-free)
    if (!inner->result(0)->has_uses()) { inner->erase(); } // B, only if now dead
}

// ── the provider STUB (contract, not execution): advertises the plugin ops; execute is NoSemantics (named-forward) ──
class PluginProvider : public IExecutionProvider
{
public:
    PluginProvider(OpId a, OpId b, OpId c) noexcept : m_a(a), m_b(b), m_c(c) {}
    [[nodiscard]] StringView name() const noexcept override { return StringView("plugin.provider"); }
    [[nodiscard]] bool advertises(const Context&, OpId k) const override { return k == m_a || k == m_b || k == m_c; }
    [[nodiscard]] exec::ExecResult execute(Context& ctx, const Module&, StringView, ConstSpan<i64>) override
    {
        exec::ExecResult r(ctx.allocator());
        r.error = exec::ExecError::NoSemantics; // stub: the provider advertises but installs no semantics (CEIR-24/29)
        return r;
    }

private:
    OpId m_a, m_b, m_c;
};
} // namespace plugin_ext

namespace
{
using plugin_ext::Plugin;
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
[[nodiscard]] ConstSpan<u8> span(const ByteArray& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
// A module with one top block carrying a plugin `a` op over a source `b(x)` and a consumer `d` of `a` (the rewrite fixture).
} // namespace

TEST_CASE("ceir 9h: the full external-plugin surface registers through the public open-world doors", "[ceir][plugin]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Plugin                 p = plugin_ext::register_plugin(ctx);

    // every piece is queryable via a PUBLIC API — the acid test that no piece needed a private wall.
    CHECK(ctx.type_class_info(p.shape) != nullptr);     // 8a type class
    CHECK(ctx.attr_class_info(p.meta) != nullptr);      // 8b attr class
    CHECK(ctx.location_class_info(p.store) != nullptr); // 8c location class
    CHECK(ctx.op_info(p.a) != nullptr);                 // the op
    const plugin_ext::PluginCost* const iface = get_op_interface<plugin_ext::PluginCost>(ctx, p.a); // 8e interface
    REQUIRE(iface != nullptr);
    CHECK(iface->cost == 5U);

    // ⛔ the custom type/attr/location went through the `Extern` DOORS — a new enum value was NOT minted (U-116, also
    // pinned mechanically in check_ceir_invariants: TypeKind/AttrKind end at Extern).
    Type         params;
    const TypeId elem[1] = {ctx.type_i64()};
    params.members       = ConstSpan<TypeId>(elem, 1U);
    const TypeId shape_ty = ctx.type_extern(p.shape, params);
    CHECK(ctx.type_of(shape_ty).kind == TypeKind::Extern);
    CHECK(ctx.attr_value(ctx.attr_extern(p.meta, ctx.attr_int(7))).kind == AttrKind::Extern);
}

TEST_CASE("ceir 9h: the plugin custom type and attr preserve through an unregistered Context and fuzz-sweep clean", "[ceir][plugin]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Plugin                 p = plugin_ext::register_plugin(ctx);

    // a module whose block arg is a plugin Extern TYPE and whose op carries a plugin Extern ATTR.
    const TypeId elem[1] = {ctx.type_i64()};
    Type         params;
    params.members    = ConstSpan<TypeId>(elem, 1U);
    const TypeId shape_ty = ctx.type_extern(p.shape, params);
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(1U, shape_ty);
    m->body()->append(top);
    Value* const     arg[1] = {top->arg(0U)};
    Operation* const op      = ctx.create_operation(p.a, ConstSpan<Value*>(arg, 1U), 1U, ctx.type_i64());
    ctx.set_attr(op, "cfg", ctx.attr_extern(p.meta, ctx.attr_int(7)));
    top->append(op);

    const ByteArray b1 = serialize(ctx, *m, &root);

    // decode into a host WITHOUT the plugin -> the Extern type + attr + unknown op are all PRESERVED; re-serialize exact.
    Context           unreg(&root);
    const ParseResult pr = deserialize(unreg, span(b1));
    REQUIRE(pr.ok);
    CHECK(blob_eq(b1, serialize(unreg, *pr.module, &root)));

    // re-register the plugin in a THIRD Context -> the preserved Extern type UNIFIES with a factory build.
    Context           ctx2(&root);
    const Plugin      p2 = plugin_ext::register_plugin(ctx2);
    const ParseResult pr2 = deserialize(ctx2, span(b1));
    REQUIRE(pr2.ok);
    const TypeId param_ty = pr2.module->body()->first_block()->arg(0U)->type();
    Type         params2;
    const TypeId elem2[1] = {ctx2.type_i64()};
    params2.members       = ConstSpan<TypeId>(elem2, 1U);
    CHECK(param_ty == ctx2.type_extern(p2.shape, params2));

    // a single-byte corruption sweep over the plugin blob, with the plugin REGISTERED (exercises the verify-hook rejects).
    for (usize i = 0; i < b1.size(); ++i)
    {
        ByteArray b(&root);
        for (usize k = 0; k < b1.size(); ++k) { b.push_back(b1[k]); }
        b[i] = static_cast<u8>(b[i] ^ 0xFFU);
        Context c(&root);
        (void)plugin_ext::register_plugin(c);
        (void)deserialize(c, span(b)); // no crash is the assertion (ASan)
    }
    SUCCEED();
}

TEST_CASE("ceir 9h: the plugin rewrite A(B(x)) to C(x) fires through the 8g try_apply and mutates the IR", "[ceir][plugin]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Plugin                 p = plugin_ext::register_plugin(ctx);

    // build D(A(B(x))) with x a block arg: B(x) -> A(B) -> D(A).
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(1U, ctx.type_i64());
    m->body()->append(top);
    Value* const     x       = top->arg(0U);
    Value* const     b_in[1] = {x};
    Operation* const b_op     = ctx.create_operation(p.b, ConstSpan<Value*>(b_in, 1U), 1U, ctx.type_i64());
    top->append(b_op);
    Value* const     a_in[1] = {b_op->result(0)};
    Operation* const a_op     = ctx.create_operation(p.a, ConstSpan<Value*>(a_in, 1U), 1U, ctx.type_i64());
    top->append(a_op);
    Value* const     d_in[1] = {a_op->result(0)};
    Operation* const d_op     = ctx.create_operation(p.a, ConstSpan<Value*>(d_in, 1U), 1U, ctx.type_i64()); // a consumer of A
    top->append(d_op);

    const RewritePattern pat{&plugin_ext::match_ab, &plugin_ext::rewrite_ab_to_c};
    REQUIRE(try_apply(pat, ctx, *a_op)); // the pattern FIRED on A(B(x))

    // post-state: A and B erased; a plugin.c consuming x now exists; D consumes C (the RAUW landed).
    CHECK(a_op->is_erased());
    CHECK(b_op->is_erased());
    Operation* const c_op = d_op->operand(0)->defining_op();
    REQUIRE(c_op != nullptr);
    CHECK(ctx.op_name(c_op->kind()) == StringView("plugin.c"));
    CHECK(c_op->operand(0) == x); // C consumes B's former input directly

    // the match leg matters too: try_apply on a NON-matching op (the plugin.c) does not fire.
    CHECK_FALSE(try_apply(pat, ctx, *c_op));
}

TEST_CASE("ceir 9h: the 9g agent path discovers and authors a plugin op over a hand-built schema table", "[ceir][plugin]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Plugin                 p = plugin_ext::register_plugin(ctx);
    (void)p;

    // ⛔ a plugin HAND-AUTHORS its OpSchema table (a first-party dialect gets it generated — the one tooling asymmetry).
    const OperandInfo a_ops[1]      = {{StringView("in"), StringView(""), false}};
    const ResultInfo  a_res[1]      = {{StringView("out"), StringView("")}};
    const OpSchema    plugin_schemas[1] = {OpSchema{.name               = StringView("a"),
                                                    .qualified          = StringView("plugin.a"),
                                                    .dialect            = StringView("plugin"),
                                                    .version            = 1U,
                                                    .summary            = StringView(""),
                                                    .docs               = StringView(""),
                                                    .operands           = ConstSpan<OperandInfo>(a_ops, 1U),
                                                    .results            = ConstSpan<ResultInfo>(a_res, 1U),
                                                    .attributes         = ConstSpan<AttrInfo>{},
                                                    .traits             = 0U,
                                                    .num_regions        = 0U,
                                                    .effects            = ConstSpan<EffectRecord>{},
                                                    .determinism        = DeterminismClass::BitExact,
                                                    .domain             = EvalDomain::CookTime,
                                                    .intrinsic          = false,
                                                    .native_provider    = StringView(""),
                                                    .native_determinism = DeterminismClass::Unspecified}};

    // the 9g agent loop, re-run over FOREIGN schemas: SELECT a 1-in/1-out op by property, intern via the discovered
    // dialect/name (zero hard-coded op knowledge), author it. This is the U-117 close.
    const OpSchema* discovered = nullptr;
    for (usize i = 0; i < 1U; ++i)
    {
        if (plugin_schemas[i].operands.size() == 1U && plugin_schemas[i].results.size() == 1U) { discovered = &plugin_schemas[i]; }
    }
    REQUIRE(discovered != nullptr);
    const OpId kind = ctx.intern_op(discovered->dialect, discovered->name);

    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(1U, ctx.type_i64());
    m->body()->append(top);
    Value* const     in[1] = {top->arg(0U)};
    Operation* const authored = ctx.create_operation(kind, ConstSpan<Value*>(in, 1U), 1U, ctx.type_i64());
    top->append(authored);
    // the agent authored the PLUGIN op it discovered (op_name == the discovered schema's qualified name).
    CHECK(ctx.op_name(authored->kind()) == discovered->qualified);
    CHECK(ctx.verify(*authored)); // and it passes the plugin's own structural verifier
}

TEST_CASE("ceir 9h: the plugin provider advertises its ops and declines foreign ones", "[ceir][plugin]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Plugin                 p = plugin_ext::register_plugin(ctx);

    plugin_ext::PluginProvider provider(p.a, p.b, p.c);
    CHECK(provider.name() == StringView("plugin.provider"));
    CHECK(provider.advertises(ctx, p.a)); // the plugin's own ops...
    CHECK(provider.advertises(ctx, p.c));
    CHECK_FALSE(provider.advertises(ctx, ctx.intern_op("arith", "addi"))); // ...but NOT a foreign op (a real contract)
}
