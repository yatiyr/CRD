// CEIR-13a §42 — the ceir.compute dialect: compute.dispatch / compute.dispatch_indirect + the kernel binding contract.
// Context::find_dispatch_misuse enforces the access-string tokens/arity, binding-is-resource, grid-is-index, and
// args-is-buffer (binding KIND/SLOT are DERIVED from type + position, not stored). The conservative effect baseline
// (GPUCommand + ambient MemoryReadWrite) is pinned two ways: a dispatch HAZARDS the export of its own output (the WAR-scar
// IR edition), and its ambient touch extends a prior transient's 12c live range. Round-trip proves kernel/access survive.
// The precise per-binding barriers are CEIR-13d. ASCII test names.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/program_asset.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;

namespace
{
struct Kit
{
    OpId cst, decl, exp, disp, dispi;
    explicit Kit(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), decl(ctx.intern_op("resource", "declare")),
          exp(ctx.intern_op("resource", "export")), disp(ctx.intern_op("compute", "dispatch")),
          dispi(ctx.intern_op("compute", "dispatch_indirect"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)compute::register_compute_ops(ctx);
    }
};
Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m.body()->append(top);
    }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
Operation* konst(Context& ctx, const Kit& k, Block* b, i64 v, TypeId ty)
{
    Operation* const c = ctx.create_operation(k.cst, {}, 1U, ty);
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
Operation* decl_buf(Context& ctx, const Kit& k, Block* b)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
    ctx.set_attr(d, "lifetime", ctx.attr_string("transient"));
    ctx.set_attr(d, "size_class", ctx.attr_int(1));
    b->append(d);
    return d;
}
// build compute.dispatch(%gx,%gy,%gz, %bindings...) {kernel, access}. `grid` = 3 index values; `binds` = nb resources.
Operation* mk_dispatch(Context& ctx, const Kit& k, Block* b, Value* gx, Value* gy, Value* gz, Value* const* binds,
                       u32 nb, const char* kernel, const char* access)
{
    containers::Array<Value*> ops(ctx.allocator());
    ops.push_back(gx);
    ops.push_back(gy);
    ops.push_back(gz);
    for (u32 i = 0; i < nb; ++i) { ops.push_back(binds[i]); }
    Operation* const op = ctx.create_operation(k.disp, ConstSpan<Value*>(ops.data(), ops.size()), 0U);
    ctx.set_attr(op, "kernel", ctx.attr_symbol(containers::StringView(kernel)));
    ctx.set_attr(op, "access", ctx.attr_string(containers::StringView(access)));
    b->append(op);
    return op;
}
Operation* find_op(const Context& ctx, Module& m, containers::StringView qual)
{
    Block* const top = m.body()->first_block();
    for (Operation* fn = top->first_op(); fn != nullptr; fn = fn->next_in_block())
    {
        for (Operation* op = func::func_body_block(fn)->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == qual) { return op; }
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("ceir 13a: well-formed direct+indirect dispatches verify and survive round-trip", "[ceir][compute]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Module* const                m  = ctx.create_module();
    Block* const                 bm = mkmain(ctx, *m);
    const TypeId                 idx = ctx.type_index();
    // @main(){ %in=declare; %out=declare; %args=declare; %gx=64; %gy=1; %gz=1;
    //          dispatch(%gx,%gy,%gz, %in,%out){kernel=reduce, access="r,w"};
    //          dispatch_indirect(%args, %in,%out){kernel=scan, access="r,w"}; return }
    Operation* const in   = decl_buf(ctx, k, bm);
    Operation* const out  = decl_buf(ctx, k, bm);
    Operation* const args = decl_buf(ctx, k, bm);
    Operation* const gx   = konst(ctx, k, bm, 64, idx);
    Operation* const gy   = konst(ctx, k, bm, 1, idx);
    Operation* const gz   = konst(ctx, k, bm, 1, idx);
    Value*           bind[2] = {in->result(0U), out->result(0U)};
    (void)mk_dispatch(ctx, k, bm, gx->result(0U), gy->result(0U), gz->result(0U), bind, 2U, "reduce", "r,w");
    Value* iops[3] = {args->result(0U), in->result(0U), out->result(0U)};
    Operation* const di = ctx.create_operation(k.dispi, ConstSpan<Value*>(iops, 3U), 0U);
    ctx.set_attr(di, "kernel", ctx.attr_symbol("scan"));
    ctx.set_attr(di, "access", ctx.attr_string("r,w"));
    bm->append(di);
    bm->append(func::create_return(ctx, {}));

    REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
    CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::None); // ⭐ well-formed

    // BINARY round-trip + kernel/access value read-back on the twin.
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    Context                     ctx2(&root);
    const Kit                   k2(ctx2);
    (void)k2;
    const ParseResult dr = deserialize(ctx2, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    CHECK(ctx2.find_dispatch_misuse(*dr.module).kind == DispatchMisuseKind::None);
    Operation* const td = find_op(ctx2, *dr.module, containers::StringView("compute.dispatch"));
    REQUIRE(td != nullptr);
    CHECK(ctx2.attr_value(td->attr("kernel")).s == containers::StringView("reduce"));
    CHECK(ctx2.attr_value(td->attr("access")).s == containers::StringView("r,w"));

    // TEXT round-trip.
    const String      txt = print(ctx, *m, &root);
    Context           ctx3(&root);
    const Kit         k3(ctx3);
    (void)k3;
    const ParseResult pr = parse(ctx3, txt);
    REQUIRE(pr.ok);
    CHECK(ctx3.find_dispatch_misuse(*pr.module).kind == DispatchMisuseKind::None);
    Operation* const tt = find_op(ctx3, *pr.module, containers::StringView("compute.dispatch_indirect"));
    REQUIRE(tt != nullptr);
    CHECK(ctx3.attr_value(tt->attr("kernel")).s == containers::StringView("scan"));
    CHECK(ctx3.attr_value(tt->attr("access")).s == containers::StringView("r,w"));
}

TEST_CASE("ceir 13a: find_dispatch_misuse rejects every malformed dispatch", "[ceir][compute]")
{
    crd::memory::MallocAllocator root;

    // GridNotIndex: grid_x is a buffer, not an index.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const buf = decl_buf(ctx, k, bm);
        Operation* const gy  = konst(ctx, k, bm, 1, ctx.type_index());
        (void)mk_dispatch(ctx, k, bm, buf->result(0U), gy->result(0U), gy->result(0U), nullptr, 0U, "k", "");
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::GridNotIndex);
    }
    // KernelNotSymbol (CEIR-13c, identity fold): the `kernel` attr is an int, not a symbol -> KernelNotSymbol (checked
    // FIRST -- identity before contract). ⛔ call ONLY find_dispatch_misuse (a raw module may skip per-op verify).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const in  = decl_buf(ctx, k, bm);
        Operation* const g   = konst(ctx, k, bm, 1, ctx.type_index());
        Value*           bd[1] = {in->result(0U)};
        Operation* const op = mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 1U, "kk", "r");
        ctx.set_attr(op, "kernel", ctx.attr_int(9)); // overwrite the symbol with an int (unreadable identity)
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::KernelNotSymbol);
    }
    // AccessTokenInvalid: a token outside {r,w,rw}.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const in  = decl_buf(ctx, k, bm);
        Operation* const g   = konst(ctx, k, bm, 1, ctx.type_index());
        Value*           bd[1] = {in->result(0U)};
        (void)mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 1U, "k", "x");
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::AccessTokenInvalid);
    }
    // AccessTokenInvalid (wrong-KIND fold): `access` is an int, not a string -> folds into AccessTokenInvalid (12b
    // doctrine). ⛔ call ONLY find_dispatch_misuse (NOT verify): a deserialized raw module may skip per-op verify, so the
    // standalone walk must still reject -- a false-clean here would be a real path.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const in  = decl_buf(ctx, k, bm);
        Operation* const g   = konst(ctx, k, bm, 1, ctx.type_index());
        Value*           bd[1] = {in->result(0U)};
        Operation* const op = mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 1U, "k", "r");
        ctx.set_attr(op, "access", ctx.attr_int(5)); // overwrite the string with an int (wrong kind)
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::AccessTokenInvalid);
    }
    // AccessArityMismatch: 2 bindings but only 1 access token.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const in  = decl_buf(ctx, k, bm);
        Operation* const out = decl_buf(ctx, k, bm);
        Operation* const g   = konst(ctx, k, bm, 1, ctx.type_index());
        Value*           bd[2] = {in->result(0U), out->result(0U)};
        (void)mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 2U, "k", "r");
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::AccessArityMismatch);
    }
    // BindingNotResource: a binding operand is an i32 const.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const g   = konst(ctx, k, bm, 1, ctx.type_index());
        Operation* const bad = konst(ctx, k, bm, 7, ctx.type_i32()); // NOT a resource
        Value*           bd[1] = {bad->result(0U)};
        (void)mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 1U, "k", "r");
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::BindingNotResource);
    }
    // ArgsNotBuffer: indirect args operand 0 is an i32 const.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const bad = konst(ctx, k, bm, 7, ctx.type_i32());
        Operation* const in  = decl_buf(ctx, k, bm);
        Value*           iops[2] = {bad->result(0U), in->result(0U)};
        Operation* const di = ctx.create_operation(k.dispi, ConstSpan<Value*>(iops, 2U), 0U);
        ctx.set_attr(di, "kernel", ctx.attr_symbol("k"));
        ctx.set_attr(di, "access", ctx.attr_string("r"));
        bm->append(di);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::ArgsNotBuffer);
    }
}

TEST_CASE("ceir 13a: a dispatch hazards the export of its output and extends a prior transient (conservative baseline)",
          "[ceir][compute]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 idx = ctx.type_index();

    // (1) the WAR-scar IR edition: dispatch WRITES %buf, then export(%buf) publishes it -> they MUST hazard (the ambient
    //     MemoryReadWrite makes GPUCommand-only insufficient -- GPUCommand is the Gpu class, export's is Memory).
    {
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const buf = decl_buf(ctx, k, bm);
        Operation* const g   = konst(ctx, k, bm, 1, idx);
        Value*           bd[1] = {buf->result(0U)};
        Operation* const disp = mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 1U, "k", "w");
        Value*           eo[1] = {buf->result(0U)};
        Operation* const ex   = ctx.create_operation(k.exp, ConstSpan<Value*>(eo, 1U), 0U);
        bm->append(ex);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.ops_hazard(*disp, *ex) != HazardKind::None); // ⭐ NOT freely reorderable
    }
    // (2) 12c integration: a dispatch's ambient memory touch extends a prior transient's live range to the dispatch pos.
    {
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const t   = decl_buf(ctx, k, bm);           // pos 0
        Operation* const gx  = konst(ctx, k, bm, 1, idx);      // pos 1
        Operation* const gy  = konst(ctx, k, bm, 1, idx);      // pos 2
        Operation* const gz  = konst(ctx, k, bm, 1, idx);      // pos 3
        (void)mk_dispatch(ctx, k, bm, gx->result(0U), gy->result(0U), gz->result(0U), nullptr, 0U, "k", ""); // pos 4
        bm->append(func::create_return(ctx, {}));
        containers::Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 1U);
        CHECK(lts[0].resource == t->result(0U));
        CHECK(lts[0].first == 0U);
        CHECK(lts[0].last == 4U); // ⭐ the dispatch's ambient MemoryReadWrite extended %t from 0 to 4
    }
}

TEST_CASE("ceir 13c: collect_dependencies extracts ckir_refs schema-driven (pin, unpinned, dedup, sorted, I6)",
          "[ceir][compute]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    // a hand-registered op in ANOTHER dialect carrying an attr literally named "kernel" -- but NO [op.kernel_ref] marker.
    Dialect* const               z = ctx.register_dialect("z");
    const OpId                   notd = z->register_op("notdispatch", {}); // no kernel_ref -> must NOT be extracted (I6)
    const TypeId                 idx  = ctx.type_index();
    const u64                    hash = 0x8000000000000001ULL; // ⛔ high bit SET -> pins the negative-int round-trip

    Module* const    m   = ctx.create_module();
    Block* const     bm  = mkmain(ctx, *m);
    Operation* const buf = decl_buf(ctx, k, bm);
    Operation* const g   = konst(ctx, k, bm, 1, idx);
    Value*           bd[1] = {buf->result(0U)};
    (void)mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 1U, "zebra", "r"); // unpinned, "zebra"
    Operation* const dp = mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 1U, "alpha", "r");
    ctx.set_attr(dp, "kernel_interface", ctx.attr_int(static_cast<i64>(hash))); // PINNED, "alpha"
    (void)mk_dispatch(ctx, k, bm, g->result(0U), g->result(0U), g->result(0U), bd, 1U, "alpha", "r"); // DUP "alpha" -> deduped
    Operation* const bad = ctx.create_operation(notd, {}, 0U);
    ctx.set_attr(bad, "kernel", ctx.attr_symbol("sneaky")); // I6: a "kernel" attr with no marker -> NOT a ckir_ref
    bm->append(bad);
    bm->append(func::create_return(ctx, {}));

    const DependencyRecord dep = collect_dependencies(ctx, *m, &root);
    REQUIRE(dep.ckir_refs.size() == 2U); // ⭐ alpha + zebra (alpha deduped); "sneaky" (no marker) EXCLUDED (I6)
    CHECK(dep.ckir_refs[0].name == containers::StringView("alpha")); // sorted by name: alpha < zebra
    CHECK(dep.ckir_refs[0].pinned);
    CHECK(dep.ckir_refs[0].interface_hash == hash);
    CHECK(dep.ckir_refs[1].name == containers::StringView("zebra"));
    CHECK_FALSE(dep.ckir_refs[1].pinned);

    // the high-bit interface-hash pin survives a BINARY round-trip (the negative-int i64-bit-pattern path).
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    Context                     ctx2(&root);
    const Kit                   k2(ctx2);
    (void)k2;
    (void)ctx2.register_dialect("z")->register_op("notdispatch", {});
    const ParseResult dr = deserialize(ctx2, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    const DependencyRecord dep2 = collect_dependencies(ctx2, *dr.module, &root);
    REQUIRE(dep2.ckir_refs.size() == 2U);
    CHECK(dep2.ckir_refs[0].pinned);
    CHECK(dep2.ckir_refs[0].interface_hash == hash); // ⭐ the high-bit u64 survived serialize->deserialize

    // ...and a TEXT round-trip (the printer/parser negative-int path).
    const String      txt = print(ctx, *m, &root);
    Context           ctx3(&root);
    const Kit         k3(ctx3);
    (void)k3;
    (void)ctx3.register_dialect("z")->register_op("notdispatch", {});
    const ParseResult pr = parse(ctx3, txt);
    REQUIRE(pr.ok);
    const DependencyRecord dep3 = collect_dependencies(ctx3, *pr.module, &root);
    REQUIRE(dep3.ckir_refs.size() == 2U);
    CHECK(dep3.ckir_refs[0].interface_hash == hash);
}
