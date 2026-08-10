// CEIR-12a §36 — the ceir.resource dialect: the find_resource_misuse module-walk verifier (the type-system enforcement
// layer) + a text/binary ROUND-TRIP of a resource module (the View/ExternalResource TypeKinds flowing through op RESULTS
// -- where a serializer gap surfaces). Resource-ness rides the CEIR-3c resource TYPE; the verifier closes the
// construction hole that opgen's structural verify (arity/attr-kind) + doc-only TOML types leave open. ASCII names.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/print.hpp>

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
    OpId cst, decl, view, imp, exp;
    explicit Kit(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), decl(ctx.intern_op("resource", "declare")),
          view(ctx.intern_op("resource", "view")), imp(ctx.intern_op("resource", "import")),
          exp(ctx.intern_op("resource", "export"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
    }
};
Block* body(Context& ctx, Module& m)
{
    Block* b = m.body()->first_block();
    if (b == nullptr)
    {
        b = ctx.create_block(0U);
        m.body()->append(b);
    }
    return b;
}
Block* mkmain(Context& ctx, Module& m)
{
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    body(ctx, m)->append(f);
    return func::func_body_block(f);
}
Operation* konst(Context& ctx, const Kit& k, Block* b, i64 v, TypeId ty)
{
    Operation* const c = ctx.create_operation(k.cst, {}, 1U, ty);
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c;
}
// a raw View type with an arbitrary mask — bypasses type_view's factory-assert (to build the ViewMaskInvalid negative).
TypeId raw_view(Context& ctx, TypeId underlying, crd::u32 mask)
{
    Type t      = Type::scalar(TypeKind::View);
    t.count     = mask;
    TypeId mm[1] = {underlying};
    t.members   = ConstSpan<TypeId>(mm, 1U);
    return ctx.intern_type(t);
}
// CEIR-12b helpers: a lone @main{ %r = <decl|import> ...; return }; the caller decorates %r with intent attrs BEFORE
// calling find_resource_intent_misuse (the module + op outlive via the caller's Context). m_out receives the module.
Operation* lone_declare(Context& ctx, const Kit& k, Module*& m_out)
{
    m_out              = ctx.create_module();
    Block* const     bm = mkmain(ctx, *m_out);
    Operation* const d  = ctx.create_operation(k.decl, {}, 1U, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
    bm->append(d);
    bm->append(func::create_return(ctx, {}));
    return d;
}
Operation* lone_import(Context& ctx, const Kit& k, Module*& m_out)
{
    m_out               = ctx.create_module();
    Block* const     bm = mkmain(ctx, *m_out);
    Operation* const imp = ctx.create_operation(k.imp, {}, 1U, ctx.type_external_resource());
    bm->append(imp);
    bm->append(func::create_return(ctx, {}));
    return imp;
}
// the first resource.declare in @main's body (the deserialized twin keeps the same structure) — for VALUE read-back.
Operation* main_declare(const Context& ctx, Module& m)
{
    Block* const top = m.body()->first_block();
    if (top == nullptr) { return nullptr; }
    Operation* const fn = top->first_op(); // the @main func op
    if (fn == nullptr) { return nullptr; }
    for (Operation* op = func::func_body_block(fn)->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) == containers::StringView("resource.declare")) { return op; }
    }
    return nullptr;
}
} // namespace

TEST_CASE("ceir 12a: a declare+view+import+export module is well-formed and survives round-trip", "[ceir][resource]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Module* const                m  = ctx.create_module();
    Block* const                 bm = mkmain(ctx, *m);
    // @main(){ %buf = declare : buffer<f32>; %v = view(buf, 0, 16) : view(buffer<f32>, Byte); %ext = import :
    //          external_resource; export(buf); return }
    const TypeId     buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Operation* const d   = ctx.create_operation(k.decl, {}, 1U, buf);
    bm->append(d);
    Operation* const off = konst(ctx, k, bm, 0, ctx.type_index());
    Operation* const sz  = konst(ctx, k, bm, 16, ctx.type_index());
    Value* vops[3] = {d->result(0U), off->result(0U), sz->result(0U)};
    const TypeId     vty = ctx.type_view(buf, static_cast<crd::u32>(ViewRange::Byte));
    Operation* const v   = ctx.create_operation(k.view, ConstSpan<Value*>(vops, 3U), 1U, vty);
    bm->append(v);
    Operation* const imp = ctx.create_operation(k.imp, {}, 1U, ctx.type_external_resource());
    bm->append(imp);
    Value* eops[1] = {d->result(0U)};
    bm->append(ctx.create_operation(k.exp, ConstSpan<Value*>(eops, 1U), 0U));
    bm->append(func::create_return(ctx, {}));

    REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
    CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::None); // ⭐ the well-formed module verifies clean

    // ⭐ BINARY round-trip: the View + ExternalResource TypeKinds (in op results) must survive serialize->deserialize.
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    Context                     ctx2(&root);
    const Kit                   k2(ctx2);
    (void)k2;
    const ParseResult dr = deserialize(ctx2, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);
    CHECK(ctx2.find_resource_misuse(*dr.module).kind == ResourceMisuseKind::None); // the loaded twin still verifies

    // TEXT round-trip.
    const String      txt = print(ctx, *m, &root);
    Context           ctx3(&root);
    const Kit         k3(ctx3);
    (void)k3;
    const ParseResult pr = parse(ctx3, txt);
    REQUIRE(pr.ok);
    CHECK(ctx3.find_resource_misuse(*pr.module).kind == ResourceMisuseKind::None);
}

TEST_CASE("ceir 12a: find_resource_misuse rejects every malformed resource construction", "[ceir][resource]")
{
    crd::memory::MallocAllocator root;
    const TypeId                 dummy; // filled per case
    (void)dummy;

    // 1. view over a non-viewable operand (an i32) -> ViewOperandNotViewable
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        const TypeId     buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
        Operation* const bad = konst(ctx, k, bm, 7, ctx.type_i32()); // an i32, NOT a resource
        Operation* const off = konst(ctx, k, bm, 0, ctx.type_index());
        Operation* const sz  = konst(ctx, k, bm, 4, ctx.type_index());
        Value* vops[3] = {bad->result(0U), off->result(0U), sz->result(0U)};
        Operation* const v = ctx.create_operation(k.view, ConstSpan<Value*>(vops, 3U), 1U,
                                                  ctx.type_view(buf, static_cast<crd::u32>(ViewRange::Byte)));
        bm->append(v);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::ViewOperandNotViewable);
    }
    // 1b. view over a VIEW (view-of-view) -> ViewOperandNotViewable: only Buffer/Image are viewable (§36 views are flat).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        const TypeId     buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
        Operation* const d   = ctx.create_operation(k.decl, {}, 1U, buf);
        bm->append(d);
        Operation* const off = konst(ctx, k, bm, 0, ctx.type_index());
        Operation* const sz  = konst(ctx, k, bm, 4, ctx.type_index());
        Value* v1ops[3] = {d->result(0U), off->result(0U), sz->result(0U)};
        const TypeId     vty = ctx.type_view(buf, static_cast<crd::u32>(ViewRange::Byte));
        Operation* const v1  = ctx.create_operation(k.view, ConstSpan<Value*>(v1ops, 3U), 1U, vty); // a valid view %v
        bm->append(v1);
        Value* v2ops[3] = {v1->result(0U), off->result(0U), sz->result(0U)}; // operand(0) = %v (a View, not viewable)
        Operation* const v2 = ctx.create_operation(k.view, ConstSpan<Value*>(v2ops, 3U), 1U,
                                                   raw_view(ctx, vty, static_cast<crd::u32>(ViewRange::Byte)));
        bm->append(v2);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::ViewOperandNotViewable);
    }
    // 2. view result that is NOT a View type -> ViewResultNotView
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        const TypeId     buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
        Operation* const d   = ctx.create_operation(k.decl, {}, 1U, buf);
        bm->append(d);
        Value* vops[1] = {d->result(0U)};
        Operation* const v = ctx.create_operation(k.view, ConstSpan<Value*>(vops, 1U), 1U, buf); // result = buffer, not View
        bm->append(v);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::ViewResultNotView);
    }
    // 3. view whose result underlying != operand type -> ViewUnderlyingMismatch
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m    = ctx.create_module();
        Block* const     bm   = mkmain(ctx, *m);
        const TypeId     buf_f = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
        const TypeId     buf_i = ctx.type_buffer(BufferMode::Plain, ctx.type_i32());
        Operation* const d     = ctx.create_operation(k.decl, {}, 1U, buf_f);
        bm->append(d);
        Operation* const off = konst(ctx, k, bm, 0, ctx.type_index());
        Operation* const sz  = konst(ctx, k, bm, 4, ctx.type_index());
        Value* vops[3] = {d->result(0U), off->result(0U), sz->result(0U)};
        Operation* const v = ctx.create_operation(k.view, ConstSpan<Value*>(vops, 3U), 1U,
                                                  ctx.type_view(buf_i, static_cast<crd::u32>(ViewRange::Byte))); // underlying buf_i != buf_f
        bm->append(v);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::ViewUnderlyingMismatch);
    }
    // 4. view result whose mask is illegal for the underlying (Mip on a buffer) -> ViewMaskInvalid (raw View bypasses the factory-assert)
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        const TypeId     buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
        Operation* const d   = ctx.create_operation(k.decl, {}, 1U, buf);
        bm->append(d);
        Operation* const off = konst(ctx, k, bm, 0, ctx.type_index());
        Operation* const sz  = konst(ctx, k, bm, 4, ctx.type_index());
        Value* vops[3] = {d->result(0U), off->result(0U), sz->result(0U)};
        const TypeId     badv = raw_view(ctx, buf, static_cast<crd::u32>(ViewRange::Mip)); // Mip invalid for a buffer
        Operation* const v    = ctx.create_operation(k.view, ConstSpan<Value*>(vops, 3U), 1U, badv);
        bm->append(v);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::ViewMaskInvalid);
    }
    // 5. view whose operand count != 1 + 2*popcount(mask) -> ViewRangeArity (Byte needs 1 offset + 1 size = 3 operands)
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        const TypeId     buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
        Operation* const d   = ctx.create_operation(k.decl, {}, 1U, buf);
        bm->append(d);
        Value* vops[1] = {d->result(0U)}; // just the resource -- NO offset/size for the Byte range
        Operation* const v = ctx.create_operation(k.view, ConstSpan<Value*>(vops, 1U), 1U,
                                                  ctx.type_view(buf, static_cast<crd::u32>(ViewRange::Byte)));
        bm->append(v);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::ViewRangeArity);
    }
    // 6. export of a non-resource operand -> ExportOperandNotResource
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const bad = konst(ctx, k, bm, 3, ctx.type_i32());
        Value* eops[1] = {bad->result(0U)};
        bm->append(ctx.create_operation(k.exp, ConstSpan<Value*>(eops, 1U), 0U));
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::ExportOperandNotResource);
    }
    // 7. declare whose result is not a resource type (an i32) -> DeclImportResultNotResource
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m  = ctx.create_module();
        Block* const  bm = mkmain(ctx, *m);
        bm->append(ctx.create_operation(k.decl, {}, 1U, ctx.type_i32())); // declare : i32 -- not a resource
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::DeclImportResultNotResource);
    }
}

TEST_CASE("ceir 12b: declare+export intent attrs are well-formed and survive round-trip", "[ceir][resource]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Module* const                m  = ctx.create_module();
    Block* const                 bm = mkmain(ctx, *m);
    // @main(){ %buf = declare {lifetime="history", history_length=2, memory_domain="device_local", residency="streamable",
    //          streaming_priority=5, budget_class="hipri"} : buffer<f32>; export(buf){direction="readwrite"}; return }
    const TypeId     buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Operation* const d   = ctx.create_operation(k.decl, {}, 1U, buf);
    ctx.set_attr(d, "lifetime", ctx.attr_string("history"));
    ctx.set_attr(d, "history_length", ctx.attr_int(2));
    ctx.set_attr(d, "memory_domain", ctx.attr_string("device_local"));
    ctx.set_attr(d, "residency", ctx.attr_string("streamable"));
    ctx.set_attr(d, "streaming_priority", ctx.attr_int(5));
    ctx.set_attr(d, "budget_class", ctx.attr_symbol("hipri"));
    bm->append(d);
    Value* eops[1] = {d->result(0U)};
    Operation* const e = ctx.create_operation(k.exp, ConstSpan<Value*>(eops, 1U), 0U);
    ctx.set_attr(e, "direction", ctx.attr_string("readwrite"));
    bm->append(e);
    bm->append(func::create_return(ctx, {}));

    REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
    CHECK(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::None);              // 12a TYPE contract still clean
    CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::None); // ⭐ the intent vocabulary verifies

    // ⭐ BINARY round-trip: the string/int/symbol intent attrs must survive -- and their VALUES, read back on the twin
    // (misuse-None alone is too weak: a serializer that dropped BOTH lifetime + history_length would also read clean).
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    Context                     ctx2(&root);
    const Kit                   k2(ctx2);
    (void)k2;
    const ParseResult dr = deserialize(ctx2, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);
    CHECK(ctx2.find_resource_intent_misuse(*dr.module).kind == ResourceIntentMisuseKind::None);
    Operation* const td = main_declare(ctx2, *dr.module);
    REQUIRE(td != nullptr);
    CHECK(ctx2.attr_value(td->attr("lifetime")).s == containers::StringView("history"));
    CHECK(ctx2.attr_value(td->attr("history_length")).i == 2);
    CHECK(ctx2.attr_value(td->attr("memory_domain")).s == containers::StringView("device_local"));
    CHECK(ctx2.attr_value(td->attr("residency")).s == containers::StringView("streamable"));
    CHECK(ctx2.attr_value(td->attr("streaming_priority")).i == 5);
    CHECK(ctx2.attr_value(td->attr("budget_class")).s == containers::StringView("hipri"));

    // TEXT round-trip: the same attr machinery via the printer/parser.
    const String      txt = print(ctx, *m, &root);
    Context           ctx3(&root);
    const Kit         k3(ctx3);
    (void)k3;
    const ParseResult pr = parse(ctx3, txt);
    REQUIRE(pr.ok);
    CHECK(ctx3.find_resource_intent_misuse(*pr.module).kind == ResourceIntentMisuseKind::None);
    // ⭐ the printer/parser is a DISTINCT code path from the binary serializer -- read the values back here too (the same
    // "misuse-None is too weak" argument applies): the interdependent pair + a symbol attr cover all three attr kinds.
    Operation* const tt = main_declare(ctx3, *pr.module);
    REQUIRE(tt != nullptr);
    CHECK(ctx3.attr_value(tt->attr("lifetime")).s == containers::StringView("history"));
    CHECK(ctx3.attr_value(tt->attr("history_length")).i == 2);
    CHECK(ctx3.attr_value(tt->attr("budget_class")).s == containers::StringView("hipri"));
}

TEST_CASE("ceir 12b: find_resource_intent_misuse rejects every malformed intent attr", "[ceir][resource]")
{
    crd::memory::MallocAllocator root;

    // positive A: lifetime=history with NO history_length (defaults to 1 -- the TAA prev-frame case) is well-formed.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m = nullptr;
        Operation* const d = lone_declare(ctx, k, m);
        ctx.set_attr(d, "lifetime", ctx.attr_string("history"));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::None);
    }
    // positive B: a declare with NO 12b attrs at all (absent = unspecified) is well-formed.
    {
        Context   ctx(&root);
        const Kit k(ctx);
        Module*   m = nullptr;
        (void)lone_declare(ctx, k, m);
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::None);
    }
    // 1. lifetime with a value outside the vocabulary -> LifetimeValueInvalid
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m = nullptr;
        Operation* const d = lone_declare(ctx, k, m);
        ctx.set_attr(d, "lifetime", ctx.attr_string("bogus"));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::LifetimeValueInvalid);
    }
    // 1b. lifetime of the WRONG KIND (an int, not a string) folds into the same kind -> LifetimeValueInvalid
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m = nullptr;
        Operation* const d = lone_declare(ctx, k, m);
        ctx.set_attr(d, "lifetime", ctx.attr_int(3));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::LifetimeValueInvalid);
    }
    // 2. history_length < 1 (with a valid lifetime=history) -> HistoryLengthInvalid
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m = nullptr;
        Operation* const d = lone_declare(ctx, k, m);
        ctx.set_attr(d, "lifetime", ctx.attr_string("history"));
        ctx.set_attr(d, "history_length", ctx.attr_int(0));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::HistoryLengthInvalid);
    }
    // 3. history_length present with lifetime ABSENT -> HistoryLengthWithoutHistory (the absent code path)
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m = nullptr;
        Operation* const d = lone_declare(ctx, k, m);
        ctx.set_attr(d, "history_length", ctx.attr_int(2));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::HistoryLengthWithoutHistory);
    }
    // 3b. history_length present with lifetime=transient (present but not history) -> HistoryLengthWithoutHistory
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m = nullptr;
        Operation* const d = lone_declare(ctx, k, m);
        ctx.set_attr(d, "lifetime", ctx.attr_string("transient"));
        ctx.set_attr(d, "history_length", ctx.attr_int(2));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::HistoryLengthWithoutHistory);
    }
    // 4. memory_domain outside the sec-24 vocabulary -> MemoryDomainValueInvalid
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m = nullptr;
        Operation* const d = lone_declare(ctx, k, m);
        ctx.set_attr(d, "memory_domain", ctx.attr_string("gpu_heap"));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::MemoryDomainValueInvalid);
    }
    // 5. residency outside the sec-25 vocabulary -> ResidencyValueInvalid
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m = nullptr;
        Operation* const d = lone_declare(ctx, k, m);
        ctx.set_attr(d, "residency", ctx.attr_string("cached"));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::ResidencyValueInvalid);
    }
    // 6. export direction outside {read, readwrite} -> DirectionValueInvalid
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        const TypeId     buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
        Operation* const d   = ctx.create_operation(k.decl, {}, 1U, buf);
        bm->append(d);
        Value* eops[1] = {d->result(0U)};
        Operation* const e = ctx.create_operation(k.exp, ConstSpan<Value*>(eops, 1U), 0U);
        ctx.set_attr(e, "direction", ctx.attr_string("write")); // not read|readwrite
        bm->append(e);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::DirectionValueInvalid);
    }
    // 7. a planning-intent attr on a resource.import (never planned) -> IntentAttrOnImport
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m   = nullptr;
        Operation* const imp = lone_import(ctx, k, m);
        ctx.set_attr(imp, "lifetime", ctx.attr_string("transient")); // even a VALID lifetime value is nonsense on an import
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::IntentAttrOnImport);
    }
    // 7b. the CEIR-12c size_class attr is ALSO declare-only planning intent -> on an import it is IntentAttrOnImport
    //     (pins that size_class stays in kIntentAttrNames; removing it there would silently regress this).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module*          m   = nullptr;
        Operation* const imp = lone_import(ctx, k, m);
        ctx.set_attr(imp, "size_class", ctx.attr_int(3));
        CHECK(ctx.find_resource_intent_misuse(*m).kind == ResourceIntentMisuseKind::IntentAttrOnImport);
    }
}
