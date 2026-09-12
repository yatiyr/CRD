// CEIR-21c — the layout dialect (sec-22): the ONE Pure op layout.constrain(%tensor){kind,params} -> %tensor (an OPTIONAL data-
// layout CONSTRAINT) + find_layout_misuse. Device-free (crd-ceir). Proves: (1) a well-formed constrain per KIND (with kind-gated
// params) verifies clean; (2) each misuse is REJECTED with the exact LayoutMisuseKind — Tensor-kind, the PASSTHROUGH type
// identity, the `kind` closed vocab, kind-gated params + arity vs the tensor rank. ⛔ MECHANISM: a CONSTRAINT OP, not a tensor-type
// member (the 21b tensor type is UNTOUCHED); sec-23 tensor<S,E,L> = a Dxxx named-forward. Declare-only, NO kernel_ref (sec-70).

#include <crd/ceir/layout.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp> // register_resource_ops (resource.declare — the typed-value seed)
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
struct LayoutKit
{
    OpId decl;
    explicit LayoutKit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)layout::register_dialect(ctx);
    }
};
Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
Value* mkval(Context& ctx, const LayoutKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId tt2(Context& ctx, u32 a, u32 c) { return ctx.type_tensor(ctx.type_f32(), sh2(ctx, a, c)); }
TypeId sh3(Context& ctx, u32 a, u32 c, u32 d)
{
    const TypeId d3[3] = {ctx.type_dim_static(a), ctx.type_dim_static(c), ctx.type_dim_static(d)};
    return ctx.type_shape(ConstSpan<TypeId>(d3, 3U));
}
TypeId tt3(Context& ctx, u32 a, u32 c, u32 d) { return ctx.type_tensor(ctx.type_f32(), sh3(ctx, a, c, d)); }
} // namespace

TEST_CASE("ceir 21c: a well-formed layout.constrain per kind verifies (row/col/strided/blocked/aos/soa/swizzle)", "[ceir][layout]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const LayoutKit               k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    const TypeId t43 = tt2(ctx, 4U, 3U); // rank-2 tensor
    Value* const v   = mkval(ctx, k, b, t43);

    for (const char* kind : {"row_major", "col_major", "aos", "soa"})
    {
        b->append(layout::build_constrain(ctx, v, ctx.attr_string(StringView(kind)), t43));
    }
    // strided with a rank-2 stride list
    Operation* const st = layout::build_constrain(ctx, v, ctx.attr_string(StringView("strided")), t43);
    ctx.set_attr(st, StringView("strides"), ctx.attr_string(StringView("1,4")));
    b->append(st);
    // blocked with a rank-2 block list (each >= 1)
    Operation* const bl = layout::build_constrain(ctx, v, ctx.attr_string(StringView("blocked")), t43);
    ctx.set_attr(bl, StringView("block"), ctx.attr_string(StringView("2,2")));
    b->append(bl);
    // swizzle with a pattern (value unparsed this slice)
    Operation* const sw = layout::build_constrain(ctx, v, ctx.attr_string(StringView("swizzle")), t43);
    ctx.set_attr(sw, StringView("swizzle"), ctx.attr_string(StringView("xyzw")));
    b->append(sw);
    // strided on a RANK-3 tensor -- pins arity-FOLLOWS-rank (3 strides accepted), not a fixed arity==2.
    Value* const     v3  = mkval(ctx, k, b, tt3(ctx, 2U, 4U, 3U));
    Operation* const st3 = layout::build_constrain(ctx, v3, ctx.attr_string(StringView("strided")), tt3(ctx, 2U, 4U, 3U));
    ctx.set_attr(st3, StringView("strides"), ctx.attr_string(StringView("1,4,12")));
    b->append(st3);

    CHECK(layout::find_layout_misuse(ctx, *m).kind == layout::LayoutMisuseKind::None);
    CHECK(ctx.op_info(ctx.intern_op("layout", "constrain")) != nullptr);
}

TEST_CASE("ceir 21c: layout.constrain REJECTS every misuse with the exact kind", "[ceir][layout]")
{
    memory::GrowableTlsfAllocator root;
    using MK = layout::LayoutMisuseKind;
    const auto find = [](Context& c, Module& m) { return layout::find_layout_misuse(c, m).kind; };

    // OperandNotTensor: constrain fed a non-tensor operand.
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* bad=mkval(ctx,k,b,ctx.type_i32());
      b->append(layout::build_constrain(ctx,bad,ctx.attr_string(StringView("row_major")),ctx.type_i32()));
      CHECK(find(ctx,*m)==MK::OperandNotTensor); }
    // ResultNotTensor: tensor operand, non-tensor result.
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=mkval(ctx,k,b,tt2(ctx,4U,3U));
      b->append(layout::build_constrain(ctx,v,ctx.attr_string(StringView("row_major")),ctx.type_i32()));
      CHECK(find(ctx,*m)==MK::ResultNotTensor); }
    // ResultTypeMismatch: result tensor type != input's (a passthrough must preserve the type).
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=mkval(ctx,k,b,tt2(ctx,4U,3U));
      b->append(layout::build_constrain(ctx,v,ctx.attr_string(StringView("row_major")),tt2(ctx,3U,4U)));
      CHECK(find(ctx,*m)==MK::ResultTypeMismatch); }
    // KindInvalid: a kind outside the closed vocab.
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=mkval(ctx,k,b,tt2(ctx,4U,3U));
      b->append(layout::build_constrain(ctx,v,ctx.attr_string(StringView("bogus")),tt2(ctx,4U,3U)));
      CHECK(find(ctx,*m)==MK::KindInvalid); }
    // ParamKindMismatch: `strides` present under kind=row_major (not strided).
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=mkval(ctx,k,b,tt2(ctx,4U,3U));
      Operation* o=layout::build_constrain(ctx,v,ctx.attr_string(StringView("row_major")),tt2(ctx,4U,3U));
      ctx.set_attr(o,StringView("strides"),ctx.attr_string(StringView("1,4"))); b->append(o);
      CHECK(find(ctx,*m)==MK::ParamKindMismatch); }
    // StridesArityMismatch: strided with 1 stride on a rank-2 tensor.
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=mkval(ctx,k,b,tt2(ctx,4U,3U));
      Operation* o=layout::build_constrain(ctx,v,ctx.attr_string(StringView("strided")),tt2(ctx,4U,3U));
      ctx.set_attr(o,StringView("strides"),ctx.attr_string(StringView("1"))); b->append(o);
      CHECK(find(ctx,*m)==MK::StridesArityMismatch); }
    // BlockInvalid: blocked with a zero block extent.
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=mkval(ctx,k,b,tt2(ctx,4U,3U));
      Operation* o=layout::build_constrain(ctx,v,ctx.attr_string(StringView("blocked")),tt2(ctx,4U,3U));
      ctx.set_attr(o,StringView("block"),ctx.attr_string(StringView("2,0"))); b->append(o);
      CHECK(find(ctx,*m)==MK::BlockInvalid); }
    // StridesArityMismatch on a RANK-3 tensor: 2 strides where 3 are required (arity FOLLOWS the rank, not a fixed ==2).
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=mkval(ctx,k,b,tt3(ctx,2U,4U,3U));
      Operation* o=layout::build_constrain(ctx,v,ctx.attr_string(StringView("strided")),tt3(ctx,2U,4U,3U));
      ctx.set_attr(o,StringView("strides"),ctx.attr_string(StringView("1,4"))); b->append(o);
      CHECK(find(ctx,*m)==MK::StridesArityMismatch); }
    // STANDALONE-ROBUST: a 0-operand constrain (via create_operation) FOLDS to the generated verify's arity check — the param
    // section is SKIPPED (min-arity guard), so an arity-wrong `strides` does NOT false-fire StridesArityMismatch.
    { Context ctx(&root); const LayoutKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* o=ctx.create_operation(ctx.intern_op("layout","constrain"),{},1U,tt2(ctx,4U,3U));
      ctx.set_attr(o,StringView("kind"),ctx.attr_string(StringView("strided")));
      ctx.set_attr(o,StringView("strides"),ctx.attr_string(StringView("1"))); b->append(o);
      CHECK(find(ctx,*m)==MK::None); }
}
