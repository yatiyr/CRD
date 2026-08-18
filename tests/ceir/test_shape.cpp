// CEIR-21a — the shape dialect (sec-35): the 6 PURE value-ops (make/rank/extent/broadcast/reshape/assert) over the CEIR-3d
// shape TYPES (Dim/Shape) + the find_shape_misuse TYPE/PREDICATE chain. Device-free (crd-ceir, no gpu-context). Proves: (1) a
// well-formed 6-op shape module verifies clean; (2) each TYPE misuse is REJECTED with the exact ShapeMisuseKind (operand/result
// TypeKinds); (3) the 3d shape-relation predicates are WRAPPED with the band contract — Incompatible -> a POINTING misuse (the
// 3z right-aligned position), Unknown -> ACCEPT, Compatible -> accept; (4) extent axis bounds; (5) the assert relation vocab +
// pass-through-type. ⛔ DECLARE-only: the ops carry typed NoSemantics + NO kernel_ref (sec-70).

#include <crd/ceir/shape.hpp>

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
struct ShapeKit
{
    OpId decl;
    OpId make;
    explicit ShapeKit(Context& ctx) : decl(ctx.intern_op("resource", "declare")), make(ctx.intern_op("shape", "make"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)shape::register_dialect(ctx);
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
// A seed VALUE of type `t` (a resource.declare op with that result type — only the value's TYPE matters to find_shape_misuse).
Value* mkval(Context& ctx, const ShapeKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
// A rank-2 static shape !shape<!dim<a>,!dim<b>>.
TypeId shape2(Context& ctx, u32 a, u32 c)
{
    const TypeId dims[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(dims, 2U));
}
// A rank-1 static shape !shape<!dim<a>>.
TypeId shape1(Context& ctx, u32 a)
{
    const TypeId dims[1] = {ctx.type_dim_static(a)};
    return ctx.type_shape(ConstSpan<TypeId>(dims, 1U));
}
} // namespace

TEST_CASE("ceir 21a: a well-formed shape module verifies (make/rank/extent/broadcast/reshape/assert)", "[ceir][shape]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const ShapeKit                k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    const TypeId sh43 = shape2(ctx, 4U, 3U); // !shape<4,3> (12 elements)
    const TypeId sh13 = shape2(ctx, 1U, 3U); // !shape<1,3> (broadcasts with 4,3)
    const TypeId sh12 = shape1(ctx, 12U);    // !shape<12>  (reshape of 4x3)

    Value* const d4 = mkval(ctx, k, b, ctx.type_dim_static(4U));
    Value* const d3 = mkval(ctx, k, b, ctx.type_dim_static(3U));
    Value* const sa = mkval(ctx, k, b, sh43);
    Value* const sb = mkval(ctx, k, b, sh13);
    Value* const s12 = mkval(ctx, k, b, sh12);

    // make(%d4, %d3) -> !shape<4,3>  (variadic dims -> create_operation directly; build_make is the 1-dim convenience)
    Value* const     mkops[2] = {d4, d3};
    Operation* const mk       = ctx.create_operation(k.make, ConstSpan<Value*>(mkops, 2U), 1U, sh43);
    b->append(mk);
    // rank(!shape<4,3>) -> index
    b->append(shape::build_rank(ctx, sa, ctx.type_index()));
    // extent(!shape<4,3>) {axis=0} -> !dim<4>
    b->append(shape::build_extent(ctx, sa, ctx.attr_int(0), ctx.type_dim_static(4U)));
    // broadcast(!shape<4,3>, !shape<1,3>) -> !shape<4,3>  (1 broadcasts to 4)
    b->append(shape::build_broadcast(ctx, sa, sb, sh43));
    // reshape(!shape<4,3>, !shape<12>) -> !shape<12>  (12 == 12)
    b->append(shape::build_reshape(ctx, sa, s12, sh12));
    // assert(!shape<4,3>, !shape<1,3>) {relation=broadcast} -> !shape<4,3>  (result == lhs)
    b->append(shape::build_assert(ctx, sa, sb, ctx.attr_string(StringView("broadcast")), sh43));

    CHECK(shape::find_shape_misuse(ctx, *m).kind == shape::ShapeMisuseKind::None);

    // the six ops registered.
    for (const char* nm : {"make", "rank", "extent", "broadcast", "reshape", "assert"})
    {
        CHECK(ctx.op_info(ctx.intern_op("shape", nm)) != nullptr);
    }
}

TEST_CASE("ceir 21a: the shape type-chain REJECTS every TYPE/PREDICATE misuse with the exact kind", "[ceir][shape]")
{
    memory::GrowableTlsfAllocator root;
    using MK = shape::ShapeMisuseKind;

    // MakeOperandNotDim: shape.make fed a non-Dim operand (an i32).
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m   = ctx.create_module();
        Block* const   b   = mkmain(ctx, *m);
        Value* const   bad = mkval(ctx, k, b, ctx.type_i32());
        Value* const   ops = bad;
        Operation* const mk = ctx.create_operation(k.make, ConstSpan<Value*>(&ops, 1U), 1U, shape1(ctx, 4U));
        b->append(mk);
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::MakeOperandNotDim);
    }
    // ResultNotShape: shape.make with a non-Shape result type (an i32).
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   d4 = mkval(ctx, k, b, ctx.type_dim_static(4U));
        Value* const   ops = d4;
        Operation* const mk = ctx.create_operation(k.make, ConstSpan<Value*>(&ops, 1U), 1U, ctx.type_i32());
        b->append(mk);
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::ResultNotShape);
    }
    // OperandNotShape: shape.rank fed a non-Shape operand.
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m   = ctx.create_module();
        Block* const   b   = mkmain(ctx, *m);
        Value* const   bad = mkval(ctx, k, b, ctx.type_i32());
        b->append(shape::build_rank(ctx, bad, ctx.type_index()));
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::OperandNotShape);
    }
    // RankResultNotIndex: shape.rank with a non-Index result.
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   sa = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        b->append(shape::build_rank(ctx, sa, ctx.type_i32())); // result not Index
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::RankResultNotIndex);
    }
    // ExtentResultNotDim: shape.extent with a non-Dim result.
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   sa = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        b->append(shape::build_extent(ctx, sa, ctx.attr_int(0), ctx.type_i32())); // result not Dim
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::ExtentResultNotDim);
    }
    // ExtentAxisInvalid: axis >= rank (axis 5 on a rank-2 shape).
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   sa = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        b->append(shape::build_extent(ctx, sa, ctx.attr_int(5), ctx.type_dim_static(4U))); // axis 5 >= rank 2
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::ExtentAxisInvalid);
    }
    // ShapeBroadcastIncompatible: !shape<4,3> vs !shape<5,3> (4 vs 5, neither is 1) -> pointing position >= 0.
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   sa = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        Value* const   sc = mkval(ctx, k, b, shape2(ctx, 5U, 3U));
        b->append(shape::build_broadcast(ctx, sa, sc, shape2(ctx, 4U, 3U)));
        const shape::ShapeMisuse mis = shape::find_shape_misuse(ctx, *m);
        CHECK(mis.kind == MK::ShapeBroadcastIncompatible);
        CHECK(mis.position >= 0); // the right-aligned axis of the first bad pair (the 3z pointing diag)
    }
    // ShapeReshapeIncompatible: !shape<4,3> (12) vs !shape<5> (5) -> product mismatch.
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   sa = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        Value* const   s5 = mkval(ctx, k, b, shape1(ctx, 5U));
        b->append(shape::build_reshape(ctx, sa, s5, shape1(ctx, 5U)));
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::ShapeReshapeIncompatible);
    }
    // AssertRelationInvalid: a relation token outside {equal,broadcast,reshape}.
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   sa = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        Value* const   sb = mkval(ctx, k, b, shape2(ctx, 1U, 3U));
        b->append(shape::build_assert(ctx, sa, sb, ctx.attr_string(StringView("bogus")), shape2(ctx, 4U, 3U)));
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::AssertRelationInvalid);
    }
    // AssertResultMismatch: assert result type != its lhs operand type.
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   sa = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        Value* const   sb = mkval(ctx, k, b, shape2(ctx, 1U, 3U));
        // result = sb's type (!shape<1,3>), NOT lhs sa (!shape<4,3>)
        b->append(shape::build_assert(ctx, sa, sb, ctx.attr_string(StringView("equal")), shape2(ctx, 1U, 3U)));
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::AssertResultMismatch);
    }
    // MakeResultShapeMismatch: make(%d4,%d3) but the result shape's members are <9,9>, not <4,3> (result IDENTITY).
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   d4 = mkval(ctx, k, b, ctx.type_dim_static(4U));
        Value* const   d3 = mkval(ctx, k, b, ctx.type_dim_static(3U));
        Value* const   ops[2] = {d4, d3};
        Operation* const mk = ctx.create_operation(k.make, ConstSpan<Value*>(ops, 2U), 1U, shape2(ctx, 9U, 9U));
        b->append(mk);
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::MakeResultShapeMismatch);
    }
    // ExtentResultMismatch: extent(!shape<4,3>){axis=0} but the result is !dim<7>, not the member !dim<4>.
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m  = ctx.create_module();
        Block* const   b  = mkmain(ctx, *m);
        Value* const   sa = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        b->append(shape::build_extent(ctx, sa, ctx.attr_int(0), ctx.type_dim_static(7U))); // result dim<7> != member[0] dim<4>
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::ExtentResultMismatch);
    }
    // ReshapeResultMismatch: reshape(!shape<4,3>, target=!shape<12>) but the result is !shape<6,2> (!= the target).
    {
        Context ctx(&root);
        const ShapeKit k(ctx);
        Module* const  m   = ctx.create_module();
        Block* const   b   = mkmain(ctx, *m);
        Value* const   sa  = mkval(ctx, k, b, shape2(ctx, 4U, 3U));
        Value* const   s12 = mkval(ctx, k, b, shape1(ctx, 12U));
        b->append(shape::build_reshape(ctx, sa, s12, shape2(ctx, 6U, 2U))); // result != target (both 12 elts, but distinct shapes)
        CHECK(shape::find_shape_misuse(ctx, *m).kind == MK::ReshapeResultMismatch);
    }
}

TEST_CASE("ceir 21a: Unknown shape relations ACCEPT (the tri-state deferral, discharged by shape.assert)", "[ceir][shape]")
{
    // ⭐ THE tri-state branch (3d "Unknown = a principled deferral to a CEIR-21 runtime check"): a broadcast/reshape over a
    // DYNAMIC/SYMBOLIC dim is neither Compatible nor Incompatible -> shapes_broadcast/shapes_reshape return Unknown ->
    // find_shape_misuse ACCEPTS (None). A shape.assert then DISCHARGES the deferral at runtime. This branch justifies
    // shape.assert's existence; a regression that rejected Unknown would fail HERE (and nowhere else).
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const ShapeKit                k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    const TypeId dyn3[2] = {ctx.type_dim_dynamic(), ctx.type_dim_static(3U)};
    const TypeId shd     = ctx.type_shape(ConstSpan<TypeId>(dyn3, 2U)); // !shape<dyn,3>
    const TypeId sh43    = shape2(ctx, 4U, 3U);
    const TypeId sh12    = shape1(ctx, 12U);

    Value* const sd  = mkval(ctx, k, b, shd);
    Value* const sa  = mkval(ctx, k, b, sh43);
    Value* const s12 = mkval(ctx, k, b, sh12);

    // broadcast(!shape<dyn,3>, !shape<4,3>) -> the dyn axis is UNKNOWN-compatible -> ACCEPT.
    b->append(shape::build_broadcast(ctx, sd, sa, sh43));
    // reshape(!shape<dyn,3>, !shape<12>) -> the dyn total is UNKNOWN vs 12 -> ACCEPT (deferred to runtime).
    b->append(shape::build_reshape(ctx, sd, s12, sh12));
    // assert(!shape<dyn,3>, !shape<4,3>) {relation=broadcast} -> the runtime DISCHARGE of the Unknown, result == lhs.
    b->append(shape::build_assert(ctx, sd, sa, ctx.attr_string(StringView("broadcast")), shd));

    CHECK(shape::find_shape_misuse(ctx, *m).kind == shape::ShapeMisuseKind::None);
}
