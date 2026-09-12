// CEIR-22a — the ceir.linalg dialect (sec-52/sec-59): the ONE Pure op `gemm` (the BLAS3 identity D = alpha*op(A)*op(B) + beta*C,
// op = optional transpose) + find_linalg_misuse. Device-free (crd-ceir). Proves: (1) a well-formed gemm (incl. a transposed A)
// verifies clean; (2) each contract is REJECTED with the exact LinalgMisuseKind — a/b/c/d Tensor-kinded, element consistency,
// A/B rank >= 2, the trans-applied CONTRACTION-dim, the leading BATCH dims, the result [batch..,M,N] identity, the accumulator
// C.shape == D.shape; (3) an Unknown (dynamic) contraction dim DEFERS (accepts); (4) an under-arity gemm FOLDS to the generated
// verify_gemm (the 21x min-arity guard). ⛔ gemm is DISTINCT from 21b tensor.matmul. ⛔ DECLARE-only: NO kernel_ref (sec-70).

#include <crd/ceir/linalg.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
struct LinalgKit
{
    OpId decl;
    explicit LinalgKit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)linalg::register_dialect(ctx);
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
Value* mkval(Context& ctx, const LinalgKit& k, Block* b, TypeId t)
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
Value* tf(Context& ctx, const LinalgKit& k, Block* b, TypeId shape) { return mkval(ctx, k, b, ctx.type_tensor(ctx.type_f32(), shape)); }
// build a gemm op: a[.,.] b[.,.] c[.,.] {alpha,beta,trans_a,trans_b} -> tensor<f32, dshape>
Operation* gemm(Context& ctx, const LinalgKit& k, Block* b, Value* a, Value* bb, Value* c, bool ta, bool tb, TypeId dshape)
{
    (void)k;
    Operation* const op = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(ta),
                                             ctx.attr_bool(tb), ctx.type_tensor(ctx.type_f32(), dshape));
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 22a: a well-formed linalg.gemm verifies (incl. a transposed A + an accumulator)", "[ceir][linalg]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const LinalgKit               k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // gemm(A[4,3], B[3,5], C[4,5]) no-trans -> D[4,5].
    Value* const a = tf(ctx, k, b, sh2(ctx, 4U, 3U));
    Value* const bb = tf(ctx, k, b, sh2(ctx, 3U, 5U));
    Value* const c = tf(ctx, k, b, sh2(ctx, 4U, 5U));
    gemm(ctx, k, b, a, bb, c, /*ta=*/false, /*tb=*/false, sh2(ctx, 4U, 5U));
    // gemm(A^T where A[3,4] -> op(A)[4,3], B[3,5], C[4,5]) trans_a -> D[4,5].
    Value* const at = tf(ctx, k, b, sh2(ctx, 3U, 4U));
    gemm(ctx, k, b, at, bb, c, /*ta=*/true, /*tb=*/false, sh2(ctx, 4U, 5U));
    // gemm(A[4,3], B^T where B[5,3] -> op(B)[3,5], C[4,5]) trans_b -> D[4,5] (proves trans_b IS applied: op(B) K'=3 == A's K=3;
    // untransposed B[5,3]'s second-to-last=5 would ContractionMismatch).
    Value* const bt = tf(ctx, k, b, sh2(ctx, 5U, 3U));
    gemm(ctx, k, b, a, bt, c, /*ta=*/false, /*tb=*/true, sh2(ctx, 4U, 5U));
    // a batched gemm: A[2,4,3], B[2,3,5], C[2,4,5] -> D[2,4,5].
    Value* const ab = tf(ctx, k, b, sh3(ctx, 2U, 4U, 3U));
    Value* const bbb = tf(ctx, k, b, sh3(ctx, 2U, 3U, 5U));
    Value* const cb = tf(ctx, k, b, sh3(ctx, 2U, 4U, 5U));
    gemm(ctx, k, b, ab, bbb, cb, /*ta=*/false, /*tb=*/false, sh3(ctx, 2U, 4U, 5U));

    CHECK(linalg::find_linalg_misuse(ctx, *m).kind == linalg::LinalgMisuseKind::None);
    CHECK(ctx.op_info(ctx.intern_op("linalg", "gemm")) != nullptr);
}

TEST_CASE("ceir 22a: linalg.gemm REJECTS every contract misuse with the exact kind", "[ceir][linalg]")
{
    memory::GrowableTlsfAllocator root;
    using MK = linalg::LinalgMisuseKind;
    const auto find = [](Context& c, Module& m) { return linalg::find_linalg_misuse(c, m).kind; };

    // OperandNotTensor: A a non-tensor.
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=mkval(ctx,k,b,ctx.type_i32()); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,4U,5U));
      gemm(ctx,k,b,a,bb,c,false,false,sh2(ctx,4U,5U));
      CHECK(find(ctx,*m)==MK::OperandNotTensor); }
    // ResultNotTensor: result_type non-tensor.
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,4U,5U));
      b->append(linalg::build_gemm(ctx,a,bb,c,ctx.attr_float(1.0),ctx.attr_float(0.0),ctx.attr_bool(false),ctx.attr_bool(false),ctx.type_i32()));
      CHECK(find(ctx,*m)==MK::ResultNotTensor); }
    // ElementMismatch: C i32-element.
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=mkval(ctx,k,b,ctx.type_tensor(ctx.type_i32(),sh2(ctx,4U,5U)));
      gemm(ctx,k,b,a,bb,c,false,false,sh2(ctx,4U,5U));
      CHECK(find(ctx,*m)==MK::ElementMismatch); }
    // RankInvalid: A rank 1.
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh1(ctx,4U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,4U,5U));
      gemm(ctx,k,b,a,bb,c,false,false,sh2(ctx,4U,5U));
      CHECK(find(ctx,*m)==MK::RankInvalid); }
    // ContractionMismatch: A[4,3] K=3, B[5,5] second-to-last=5 != 3.
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,5U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,4U,5U));
      gemm(ctx,k,b,a,bb,c,false,false,sh2(ctx,4U,5U));
      CHECK(find(ctx,*m)==MK::ContractionMismatch); }
    // ContractionMismatch via trans_b: B[3,5] with trans_b -> op(B)[5,3], second-to-last=5 != A's K=3 (fails if trans_b
    // is silently read as false: untransposed op(B)[3,5] second-to-last=3 would MATCH and give None).
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,4U,5U));
      gemm(ctx,k,b,a,bb,c,false,true,sh2(ctx,4U,5U));
      CHECK(find(ctx,*m)==MK::ContractionMismatch); }
    // ResultShapeMismatch: A[4,3]xB[3,5] but D declared [9,9] (should be [4,5]).
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,9U,9U));
      gemm(ctx,k,b,a,bb,c,false,false,sh2(ctx,9U,9U)); // C matches the (wrong) D, so ResultShapeMismatch fires (not accumulator)
      CHECK(find(ctx,*m)==MK::ResultShapeMismatch); }
    // AccumulatorShapeMismatch: D correct [4,5] but C [9,9] (C != D).
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,9U,9U));
      gemm(ctx,k,b,a,bb,c,false,false,sh2(ctx,4U,5U));
      CHECK(find(ctx,*m)==MK::AccumulatorShapeMismatch); }
    // BatchMismatch: A[2,4,3], B[3,3,5] -- K=3 matches but batch 2 != 3.
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh3(ctx,2U,4U,3U)); Value* bb=tf(ctx,k,b,sh3(ctx,3U,3U,5U)); Value* c=tf(ctx,k,b,sh3(ctx,2U,4U,5U));
      gemm(ctx,k,b,a,bb,c,false,false,sh3(ctx,2U,4U,5U));
      CHECK(find(ctx,*m)==MK::BatchMismatch); }
    // STANDALONE-ROBUST: a 2-operand gemm (built via create_operation) FOLDS to the generated verify_gemm (the min-arity
    // guard skips it in the walk), NEVER trips an in-walk operand(2) assert.
    { Context ctx(&root); const LinalgKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* ops[2]={a,bb};
      Operation* bad=ctx.create_operation(ctx.intern_op("linalg","gemm"),ConstSpan<Value*>(ops,2U),1U,ctx.type_tensor(ctx.type_f32(),sh2(ctx,4U,5U)));
      b->append(bad);
      CHECK(find(ctx,*m)==MK::None); } // folds; the generated verify_gemm owns the 3-operand arity
}

TEST_CASE("ceir 22a: an Unknown (dynamic) contraction dim DEFERS (accepts)", "[ceir][linalg]")
{
    // A[4, dyn] (K dynamic) x B[3, 5]: the contraction K is Unknown (one side dynamic) -> defer (accept); the result [4,5]
    // is computed from op(A)'s M=4 + op(B)'s N=5 and matches D/C.
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const LinalgKit               k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);
    const TypeId dynk[2] = {ctx.type_dim_static(4U), ctx.type_dim_dynamic()};
    const TypeId sad     = ctx.type_shape(ConstSpan<TypeId>(dynk, 2U)); // <4, dyn>
    Value* const a = tf(ctx, k, b, sad);
    Value* const bb = tf(ctx, k, b, sh2(ctx, 3U, 5U));
    Value* const c = tf(ctx, k, b, sh2(ctx, 4U, 5U));
    gemm(ctx, k, b, a, bb, c, /*ta=*/false, /*tb=*/false, sh2(ctx, 4U, 5U));
    CHECK(linalg::find_linalg_misuse(ctx, *m).kind == linalg::LinalgMisuseKind::None);
}
