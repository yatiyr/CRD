// CEIR-21b — the tensor dialect (sec-51): the STRUCTURAL SIX PURE value-ops (elementwise/broadcast/reshape/transpose/reduce/
// matmul) over the CEIR-3d Tensor TYPE + the shape-aware find_tensor_misuse. Device-free (crd-ceir). Proves: (1) a well-formed
// six-op module verifies clean; (2) each element/shape misuse is REJECTED with the exact TensorMisuseKind — Tensor-kind, element
// consistency, the 3d predicates WRAPPED [Incompatible→pointing, Unknown→ACCEPT, Compatible→accept] + shapes_broadcast_result
// exactness, transpose PERMUTATION, reduce AXIS-bounds, matmul CONTRACTION-dim + result-shape IDENTITY; (3) Unknown (dynamic dims)
// ACCEPTS. ⛔ DECLARE-only: typed NoSemantics + NO kernel_ref (sec-70).

#include <crd/ceir/tensor.hpp>

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
struct TensorKit
{
    OpId decl;
    explicit TensorKit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)tensor::register_dialect(ctx);
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
Value* mkval(Context& ctx, const TensorKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return ctx.type_shape(ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId sh3(Context& ctx, u32 a, u32 c, u32 d)
{
    const TypeId dd[3] = {ctx.type_dim_static(a), ctx.type_dim_static(c), ctx.type_dim_static(d)};
    return ctx.type_shape(ConstSpan<TypeId>(dd, 3U));
}
// a tensor<f32, shape> value
Value* tf(Context& ctx, const TensorKit& k, Block* b, TypeId shape) { return mkval(ctx, k, b, ctx.type_tensor(ctx.type_f32(), shape)); }
} // namespace

TEST_CASE("ceir 21b: a well-formed tensor module verifies (elementwise/broadcast/reshape/transpose/reduce/matmul)", "[ceir][tensor]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const TensorKit               k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    const TypeId s43 = sh2(ctx, 4U, 3U);
    const TypeId s13 = sh2(ctx, 1U, 3U);
    const TypeId s34 = sh2(ctx, 3U, 4U);
    const TypeId s44 = sh2(ctx, 4U, 4U);
    const TypeId s12 = sh1(ctx, 12U);
    const TypeId s4  = sh1(ctx, 4U);

    Value* const t43 = tf(ctx, k, b, s43);
    Value* const t13 = tf(ctx, k, b, s13);
    Value* const t34 = tf(ctx, k, b, s34);

    const TypeId ef  = ctx.type_f32();
    // elementwise(t43, t13){add} -> tensor<f32, broadcast(4,3;1,3)=4,3>
    b->append(tensor::build_elementwise(ctx, t43, t13, ctx.attr_string(StringView("add")), ctx.type_tensor(ef, s43)));
    // broadcast(t13) -> tensor<f32, 4,3>  (1,3 broadcasts up to 4,3)
    b->append(tensor::build_broadcast(ctx, t13, ctx.type_tensor(ef, s43)));
    // reshape(t43) -> tensor<f32, 12>
    b->append(tensor::build_reshape(ctx, t43, ctx.type_tensor(ef, s12)));
    // transpose(t43){perm=1,0} -> tensor<f32, 3,4>
    b->append(tensor::build_transpose(ctx, t43, ctx.attr_string(StringView("1,0")), ctx.type_tensor(ef, s34)));
    // reduce(t43){axis=1,sum} -> tensor<f32, 4>
    b->append(tensor::build_reduce(ctx, t43, ctx.attr_int(1), ctx.attr_string(StringView("sum")), ctx.type_tensor(ef, s4)));
    // matmul(t43 [4,3], t34 [3,4]) -> tensor<f32, 4,4>
    b->append(tensor::build_matmul(ctx, t43, t34, ctx.type_tensor(ef, s44)));

    CHECK(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);

    for (const char* nm : {"elementwise", "broadcast", "reshape", "transpose", "reduce", "matmul"})
    {
        CHECK(ctx.op_info(ctx.intern_op("tensor", nm)) != nullptr);
    }
}

TEST_CASE("ceir 21b: the tensor chain REJECTS every element/shape misuse with the exact kind", "[ceir][tensor]")
{
    memory::GrowableTlsfAllocator root;
    using MK = tensor::TensorMisuseKind;
    const auto find = [](Context& c, Module& m) { return tensor::find_tensor_misuse(c, m).kind; };

    // OperandNotTensor: elementwise fed a non-tensor lhs.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* bad=mkval(ctx,k,b,ctx.type_i32()); Value* t=tf(ctx,k,b,sh2(ctx,1U,3U));
      b->append(tensor::build_elementwise(ctx,bad,t,ctx.attr_string(StringView("add")),ctx.type_tensor(ctx.type_f32(),sh2(ctx,1U,3U))));
      CHECK(find(ctx,*m)==MK::OperandNotTensor); }
    // ResultNotTensor: elementwise with a non-tensor result.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* t=tf(ctx,k,b,sh2(ctx,1U,3U));
      b->append(tensor::build_elementwise(ctx,t,t,ctx.attr_string(StringView("add")),ctx.type_i32()));
      CHECK(find(ctx,*m)==MK::ResultNotTensor); }
    // TensorElementMismatch: lhs f32, rhs i32.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* lf=tf(ctx,k,b,sh2(ctx,1U,3U)); Value* ri=mkval(ctx,k,b,ctx.type_tensor(ctx.type_i32(),sh2(ctx,1U,3U)));
      b->append(tensor::build_elementwise(ctx,lf,ri,ctx.attr_string(StringView("add")),ctx.type_tensor(ctx.type_f32(),sh2(ctx,1U,3U))));
      CHECK(find(ctx,*m)==MK::TensorElementMismatch); }
    // FnInvalid (elementwise): fn outside the vocab.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* t=tf(ctx,k,b,sh2(ctx,1U,3U));
      b->append(tensor::build_elementwise(ctx,t,t,ctx.attr_string(StringView("bogus")),ctx.type_tensor(ctx.type_f32(),sh2(ctx,1U,3U))));
      CHECK(find(ctx,*m)==MK::FnInvalid); }
    // ShapeBroadcastIncompatible: 4,3 vs 5,3.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* c=tf(ctx,k,b,sh2(ctx,5U,3U));
      b->append(tensor::build_elementwise(ctx,a,c,ctx.attr_string(StringView("add")),ctx.type_tensor(ctx.type_f32(),sh2(ctx,4U,3U))));
      const tensor::TensorMisuse mis=tensor::find_tensor_misuse(ctx,*m);
      CHECK(mis.kind==MK::ShapeBroadcastIncompatible); CHECK(mis.position>=0); }
    // BroadcastResultMismatch: elementwise result shape != the broadcast (declared 9,9).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* c=tf(ctx,k,b,sh2(ctx,1U,3U));
      b->append(tensor::build_elementwise(ctx,a,c,ctx.attr_string(StringView("add")),ctx.type_tensor(ctx.type_f32(),sh2(ctx,9U,9U))));
      CHECK(find(ctx,*m)==MK::BroadcastResultMismatch); }
    // ShapeReshapeIncompatible: 4,3 (12) -> 5 (5).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U));
      b->append(tensor::build_reshape(ctx,a,ctx.type_tensor(ctx.type_f32(),sh1(ctx,5U))));
      CHECK(find(ctx,*m)==MK::ShapeReshapeIncompatible); }
    // PermInvalid: transpose perm "0,0" (duplicate).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U));
      b->append(tensor::build_transpose(ctx,a,ctx.attr_string(StringView("0,0")),ctx.type_tensor(ctx.type_f32(),sh2(ctx,4U,3U))));
      CHECK(find(ctx,*m)==MK::PermInvalid); }
    // TransposeResultMismatch: perm "1,0" but result is 4,3 (should be 3,4).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U));
      b->append(tensor::build_transpose(ctx,a,ctx.attr_string(StringView("1,0")),ctx.type_tensor(ctx.type_f32(),sh2(ctx,4U,3U))));
      CHECK(find(ctx,*m)==MK::TransposeResultMismatch); }
    // AxisInvalid: reduce axis 5 on a rank-2 tensor.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U));
      b->append(tensor::build_reduce(ctx,a,ctx.attr_int(5),ctx.attr_string(StringView("sum")),ctx.type_tensor(ctx.type_f32(),sh1(ctx,4U))));
      CHECK(find(ctx,*m)==MK::AxisInvalid); }
    // FnInvalid (reduce): reduce fn outside its vocab.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U));
      b->append(tensor::build_reduce(ctx,a,ctx.attr_int(1),ctx.attr_string(StringView("bogus")),ctx.type_tensor(ctx.type_f32(),sh1(ctx,4U))));
      CHECK(find(ctx,*m)==MK::FnInvalid); }
    // ReduceResultMismatch: reduce axis 1 but result is 4,3 (should be 4).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U));
      b->append(tensor::build_reduce(ctx,a,ctx.attr_int(1),ctx.attr_string(StringView("sum")),ctx.type_tensor(ctx.type_f32(),sh2(ctx,4U,3U))));
      CHECK(find(ctx,*m)==MK::ReduceResultMismatch); }
    // MatmulRankInvalid: lhs rank 1.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh1(ctx,4U)); Value* c=tf(ctx,k,b,sh2(ctx,3U,4U));
      b->append(tensor::build_matmul(ctx,a,c,ctx.type_tensor(ctx.type_f32(),sh2(ctx,4U,4U))));
      CHECK(find(ctx,*m)==MK::MatmulRankInvalid); }
    // ContractionMismatch: lhs [4,3] K=3, rhs [5,4] K=5.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* c=tf(ctx,k,b,sh2(ctx,5U,4U));
      b->append(tensor::build_matmul(ctx,a,c,ctx.type_tensor(ctx.type_f32(),sh2(ctx,4U,4U))));
      CHECK(find(ctx,*m)==MK::ContractionMismatch); }
    // MatmulResultMismatch: [4,3]x[3,4] but result declared 9,9 (should be 4,4).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* c=tf(ctx,k,b,sh2(ctx,3U,4U));
      b->append(tensor::build_matmul(ctx,a,c,ctx.type_tensor(ctx.type_f32(),sh2(ctx,9U,9U))));
      CHECK(find(ctx,*m)==MK::MatmulResultMismatch); }
    // BroadcastResultMismatch (tensor.broadcast's OWN reject path): broadcast(4,3) -> 1,3 (a SHRINK -- input does not broadcast UP).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U));
      b->append(tensor::build_broadcast(ctx,a,ctx.type_tensor(ctx.type_f32(),sh2(ctx,1U,3U))));
      CHECK(find(ctx,*m)==MK::BroadcastResultMismatch); }
    // BatchMismatch: matmul(2,4,3 ; 3,3,4) -- K=3 matches but the batch dim 2 != 3 (matmul_shape copies LHS's batch, so this
    // would false-green without the explicit batch check).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh3(ctx,2U,4U,3U)); Value* c=tf(ctx,k,b,sh3(ctx,3U,3U,4U));
      b->append(tensor::build_matmul(ctx,a,c,ctx.type_tensor(ctx.type_f32(),sh3(ctx,2U,4U,4U))));
      CHECK(find(ctx,*m)==MK::BatchMismatch); }
    // STANDALONE-ROBUST: a malformed 1-operand elementwise (built via create_operation) must FOLD to the generated verify's
    // arity check (find_tensor_misuse skips it via the min-arity guard), NEVER trip an in-walk operand(1) assert.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* t=tf(ctx,k,b,sh2(ctx,1U,3U)); Value* ops=t;
      Operation* ew1=ctx.create_operation(ctx.intern_op("tensor","elementwise"),ConstSpan<Value*>(&ops,1U),1U,ctx.type_tensor(ctx.type_f32(),sh2(ctx,1U,3U)));
      b->append(ew1);
      CHECK(find(ctx,*m)==MK::None); } // folds (no assert); the generated verify_elementwise owns the 2-operand arity
}

TEST_CASE("ceir 21b: Unknown tensor shape relations ACCEPT (the tri-state deferral)", "[ceir][tensor]")
{
    // a DYNAMIC-dim tensor: elementwise/reshape over it are neither Compatible nor Incompatible -> Unknown -> ACCEPT.
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const TensorKit               k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);
    const TypeId ef = ctx.type_f32();
    const TypeId dyn3[2] = {ctx.type_dim_dynamic(), ctx.type_dim_static(3U)};
    const TypeId shd     = ctx.type_shape(ConstSpan<TypeId>(dyn3, 2U)); // shape<dyn,3>
    Value* const td = tf(ctx, k, b, shd);
    Value* const t43 = tf(ctx, k, b, sh2(ctx, 4U, 3U));
    // elementwise(dyn,3 ; 4,3){add} -> the dyn axis is Unknown-compatible -> ACCEPT (result shape unconstrained here: reuse 4,3).
    b->append(tensor::build_elementwise(ctx, td, t43, ctx.attr_string(StringView("add")), ctx.type_tensor(ef, sh2(ctx, 4U, 3U))));
    // reshape(dyn,3 -> 12): the dyn total is Unknown vs 12 -> ACCEPT.
    b->append(tensor::build_reshape(ctx, td, ctx.type_tensor(ef, sh1(ctx, 12U))));
    // broadcast(dyn,3 -> 4,3): the dyn axis is Unknown-compatible with 4 -> ACCEPT (tensor.broadcast's OWN Unknown path).
    b->append(tensor::build_broadcast(ctx, td, ctx.type_tensor(ef, sh2(ctx, 4U, 3U))));
    CHECK(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
}
