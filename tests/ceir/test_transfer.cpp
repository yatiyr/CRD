// CEIR-13b §50 — the ceir.transfer dialect: copy / upload / readback / clear / mip_gen with STATIC per-kind 4a effects.
// Context::find_transfer_misuse enforces the movement contract (operand transferable, copy src!=dst, clear value fold +
// image placement, mip_gen is-image). The static effects are pinned two ways that DISCRIMINATE this slice from 13a's
// ambient dispatch: upload(%buf)->readback(%buf) is exactly RAW (the upload->first-read barrier, by construction), and a
// transfer does NOT extend an UNRELATED resource's 12c live range (precise operand, no whole-Memory ambient). ASCII names.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gen/transfer_ops.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;

namespace
{
struct Kit
{
    OpId cst, decl, view, disp, copy, upload, readback, clear, mipgen;
    explicit Kit(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), decl(ctx.intern_op("resource", "declare")),
          view(ctx.intern_op("resource", "view")), disp(ctx.intern_op("compute", "dispatch")),
          copy(ctx.intern_op("transfer", "copy")), upload(ctx.intern_op("transfer", "upload")),
          readback(ctx.intern_op("transfer", "readback")), clear(ctx.intern_op("transfer", "clear")),
          mipgen(ctx.intern_op("transfer", "mip_gen"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)compute::register_compute_ops(ctx);
        (void)transfer::register_transfer_ops(ctx);
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
Operation* decl_res(Context& ctx, const Kit& k, Block* b, TypeId ty)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, ty);
    b->append(d);
    return d;
}
Operation* unary(Context& ctx, Block* b, OpId kind, Value* v)
{
    Value* ops[1] = {v};
    Operation* const op = ctx.create_operation(kind, ConstSpan<Value*>(ops, 1U), 0U);
    b->append(op);
    return op;
}
Operation* binary(Context& ctx, Block* b, OpId kind, Value* a, Value* c)
{
    Value* ops[2] = {a, c};
    Operation* const op = ctx.create_operation(kind, ConstSpan<Value*>(ops, 2U), 0U);
    b->append(op);
    return op;
}
Operation* view_of(Context& ctx, const Kit& k, Block* b, Operation* res, TypeId buf, i64 off)
{
    Operation* const o = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
    ctx.set_attr(o, "value", ctx.attr_int(off));
    b->append(o);
    Operation* const s = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
    ctx.set_attr(s, "value", ctx.attr_int(16));
    b->append(s);
    Value*           vops[3] = {res->result(0U), o->result(0U), s->result(0U)};
    Operation* const v       = ctx.create_operation(k.view, ConstSpan<Value*>(vops, 3U), 1U,
                                                    ctx.type_view(buf, static_cast<u32>(ViewRange::Byte)));
    b->append(v);
    return v;
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

TEST_CASE("ceir 13b: well-formed transfers (incl. buffer-image copy and view subresource) verify and round-trip",
          "[ceir][transfer]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Module* const                m   = ctx.create_module();
    Block* const                 bm  = mkmain(ctx, *m);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    const TypeId                 img = ctx.type_image(ImageDim::Dim2D, ctx.type_f32());
    Operation* const             b1  = decl_res(ctx, k, bm, buf);
    Operation* const             b2  = decl_res(ctx, k, bm, buf);
    Operation* const             im  = decl_res(ctx, k, bm, img);
    (void)binary(ctx, bm, k.copy, b1->result(0U), b2->result(0U)); // buffer -> buffer
    (void)binary(ctx, bm, k.copy, im->result(0U), b1->result(0U)); // buffer -> IMAGE (kind combo from types)
    // a SUBRESOURCE copy between two distinct VIEWS of ONE root buffer -- LEGAL (distinct SSA, disjoint ranges).
    Operation* const v1 = view_of(ctx, k, bm, b2, buf, 0);
    Operation* const v2 = view_of(ctx, k, bm, b2, buf, 64);
    (void)binary(ctx, bm, k.copy, v1->result(0U), v2->result(0U));
    Operation* const up = unary(ctx, bm, k.upload, b1->result(0U));
    ctx.set_attr(up, "source", ctx.attr_symbol("host_verts"));
    Operation* const rb = unary(ctx, bm, k.readback, b2->result(0U));
    ctx.set_attr(rb, "dest", ctx.attr_symbol("host_out"));
    Operation* const cl = unary(ctx, bm, k.clear, b1->result(0U));
    ctx.set_attr(cl, "value", ctx.attr_int(7)); // a BUFFER clear with a fill word -- OK
    (void)unary(ctx, bm, k.clear, im->result(0U)); // an IMAGE clear with NO value -- OK
    (void)unary(ctx, bm, k.mipgen, im->result(0U)); // mip-gen an image -- OK
    bm->append(func::create_return(ctx, {}));

    REQUIRE(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
    REQUIRE(ctx.find_resource_misuse(*m).kind == ResourceMisuseKind::None);
    CHECK(ctx.find_transfer_misuse(*m).kind == TransferMisuseKind::None); // ⭐ all well-formed

    // BINARY round-trip: source/dest/value survive.
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    Context                     ctx2(&root);
    const Kit                   k2(ctx2);
    (void)k2;
    const ParseResult dr = deserialize(ctx2, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    CHECK(ctx2.find_transfer_misuse(*dr.module).kind == TransferMisuseKind::None);
    CHECK(ctx2.attr_value(find_op(ctx2, *dr.module, containers::StringView("transfer.upload"))->attr("source")).s
          == containers::StringView("host_verts"));
    CHECK(ctx2.attr_value(find_op(ctx2, *dr.module, containers::StringView("transfer.readback"))->attr("dest")).s
          == containers::StringView("host_out"));
    CHECK(ctx2.attr_value(find_op(ctx2, *dr.module, containers::StringView("transfer.clear"))->attr("value")).i == 7);

    // TEXT round-trip.
    const String      txt = print(ctx, *m, &root);
    Context           ctx3(&root);
    const Kit         k3(ctx3);
    (void)k3;
    const ParseResult pr = parse(ctx3, txt);
    REQUIRE(pr.ok);
    CHECK(ctx3.find_transfer_misuse(*pr.module).kind == TransferMisuseKind::None);
}

TEST_CASE("ceir 13b: find_transfer_misuse rejects every malformed transfer", "[ceir][transfer]")
{
    crd::memory::GrowableTlsfAllocator root;

    // OperandNotTransferable: copy dst is an i32 const.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const bad = ctx.create_operation(k.cst, {}, 1U, ctx.type_i32());
        ctx.set_attr(bad, "value", ctx.attr_int(0));
        bm->append(bad);
        Operation* const src = decl_res(ctx, k, bm, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
        (void)binary(ctx, bm, k.copy, bad->result(0U), src->result(0U));
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_transfer_misuse(*m).kind == TransferMisuseKind::OperandNotTransferable);
    }
    // CopySrcIsDst: copy(%buf, %buf) -- a whole-resource self-copy.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const buf = decl_res(ctx, k, bm, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
        (void)binary(ctx, bm, k.copy, buf->result(0U), buf->result(0U));
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_transfer_misuse(*m).kind == TransferMisuseKind::CopySrcIsDst);
    }
    // MipGenNotImage: mip_gen over a buffer.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const buf = decl_res(ctx, k, bm, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
        (void)unary(ctx, bm, k.mipgen, buf->result(0U));
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_transfer_misuse(*m).kind == TransferMisuseKind::MipGenNotImage);
    }
    // ClearValueInvalid: a clear whose `value` is a symbol, not an int (wrong-KIND fold; standalone walk).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const buf = decl_res(ctx, k, bm, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
        Operation* const cl  = unary(ctx, bm, k.clear, buf->result(0U));
        ctx.set_attr(cl, "value", ctx.attr_symbol("nope"));
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_transfer_misuse(*m).kind == TransferMisuseKind::ClearValueInvalid);
    }
    // ClearValueOnImage: a fill word on an IMAGE clear (typed image clears are 13d).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const im  = decl_res(ctx, k, bm, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        Operation* const cl  = unary(ctx, bm, k.clear, im->result(0U));
        ctx.set_attr(cl, "value", ctx.attr_int(1));
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.find_transfer_misuse(*m).kind == TransferMisuseKind::ClearValueOnImage);
    }
}

TEST_CASE("ceir 13b: static effects give a precise upload->readback RAW and no ambient lifetime bleed", "[ceir][transfer]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    const TypeId                 buf = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());

    // (1) the upload->first-read barrier scar, IR edition: upload WRITES %buf, readback READS it -> exactly RAW.
    {
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const b1  = decl_res(ctx, k, bm, buf);
        Operation* const up  = unary(ctx, bm, k.upload, b1->result(0U));
        Operation* const rb  = unary(ctx, bm, k.readback, b1->result(0U));
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.ops_hazard(*up, *rb) == HazardKind::Raw); // ⭐ write-then-read, precise (not the ambient != None)
    }
    // (2) the discriminator vs 13a: a transfer's PRECISE effect does NOT extend an UNRELATED transient's 12c live range.
    {
        Module* const    m  = ctx.create_module();
        Block* const     bm = mkmain(ctx, *m);
        Operation* const a  = ctx.create_operation(k.decl, {}, 1U, buf); // %a transient, declared pos 0, never used
        ctx.set_attr(a, "lifetime", ctx.attr_string("transient"));
        ctx.set_attr(a, "size_class", ctx.attr_int(1));
        bm->append(a);
        Operation* const b = decl_res(ctx, k, bm, buf);   // pos 1
        (void)unary(ctx, bm, k.upload, b->result(0U));    // pos 2: upload(%b) -- writes %b ONLY, not the whole Memory class
        bm->append(func::create_return(ctx, {}));
        containers::Array<ResourceLifetime> lts(&root);
        ctx.compute_block_lifetimes(*bm, lts);
        REQUIRE(lts.size() == 2U);
        const usize ai = lts[0].resource == a->result(0U) ? 0U : 1U;
        CHECK(lts[ai].first == 0U);
        CHECK(lts[ai].last == 0U); // ⭐ NOT extended by upload(%b) -- the precise-vs-ambient contrast with 13a's dispatch
    }
    // (3) cross-dialect seam: upload(%buf) then a dispatch BINDING %buf -> they hazard (13a's ambient composes with 13b).
    {
        Module* const    m   = ctx.create_module();
        Block* const     bm  = mkmain(ctx, *m);
        Operation* const b1  = decl_res(ctx, k, bm, buf);
        Operation* const up  = unary(ctx, bm, k.upload, b1->result(0U));
        Operation* const g   = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
        ctx.set_attr(g, "value", ctx.attr_int(1));
        bm->append(g);
        Value*           dops[4] = {g->result(0U), g->result(0U), g->result(0U), b1->result(0U)};
        Operation* const disp = ctx.create_operation(k.disp, ConstSpan<Value*>(dops, 4U), 0U);
        ctx.set_attr(disp, "kernel", ctx.attr_symbol("consume"));
        ctx.set_attr(disp, "access", ctx.attr_string("r"));
        bm->append(disp);
        bm->append(func::create_return(ctx, {}));
        CHECK(ctx.ops_hazard(*up, *disp) != HazardKind::None); // ⭐ upload write vs dispatch (ambient) read
    }
}
