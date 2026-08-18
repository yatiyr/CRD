// CEIR-22b — the CEIR→CKIR native-provider SYNTHESIS (ckir_synth.hpp), DEVICE-FREE. Proves synth_gemm maps a declare-only
// ceir.linalg.gemm op → a graph-tier CKIR Contract node WITHIN the bit-exact envelope (alpha==1, beta==0, no transpose, F32,
// rank-2, static dims), and TYPED-REJECTS everything outside it (never a silent wrong-result subset — the advisor's α=2-false-
// green guard). The on-DEVICE bit-exact run vs eval_cpu + an independent reference lives in tests/ceir-gpu-{vulkan,dx12}.

#include <crd/ceir/gpu/ckir_synth.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/tensor.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
struct Kit
{
    OpId decl;
    explicit Kit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)linalg::register_dialect(ctx);
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
Value* mkval(Context& ctx, const Kit& k, Block* b, TypeId t)
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
TypeId sh3(Context& ctx, u32 a, u32 c, u32 e)
{
    const TypeId d[3] = {ctx.type_dim_static(a), ctx.type_dim_static(c), ctx.type_dim_static(e)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 3U));
}
Value* tf(Context& ctx, const Kit& k, Block* b, TypeId shape) { return mkval(ctx, k, b, ctx.type_tensor(ctx.type_f32(), shape)); }
// build a gemm op with explicit epilogue attrs + an explicit element for the operands/result.
Operation* gemm(Context& ctx, const Kit& k, Block* b, Value* a, Value* bb, Value* c, double alpha, double beta, bool ta,
                bool tb, TypeId dtype)
{
    (void)k;
    Operation* const op = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(alpha), ctx.attr_float(beta), ctx.attr_bool(ta),
                                             ctx.attr_bool(tb), dtype);
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 22b: synth_gemm maps a plain gemm to a CKIR contract node (the bit-exact envelope)", "[ceir][ckir-synth]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);
    const TypeId                  ef = ctx.type_f32();
    // gemm(A[4,3], B[3,5], C[4,5]) alpha=1 beta=0 no-trans -> D[4,5]: the plain contract envelope.
    Value* const     a  = tf(ctx, k, b, sh2(ctx, 4U, 3U));
    Value* const     bb = tf(ctx, k, b, sh2(ctx, 3U, 5U));
    Value* const     c  = tf(ctx, k, b, sh2(ctx, 4U, 5U));
    Operation* const op = gemm(ctx, k, b, a, bb, c, 1.0, 0.0, false, false, ctx.type_tensor(ef, sh2(ctx, 4U, 5U)));

    kir::KGraph            g(&root);
    const gpu::GraphSynth  s = gpu::synth_gemm(ctx, *op, g);
    CHECK(s.reject == gpu::SynthReject::None);
    CHECK(s.output >= 0); // a real CKIR node id (input 0, input 1, contract → node 2); the DEVICE gate proves it computes A·B.
}

TEST_CASE("ceir 22b: synth_gemm TYPED-REJECTS everything outside the bit-exact envelope", "[ceir][ckir-synth]")
{
    using RJ = gpu::SynthReject;
    const auto synth = [](Context& c, memory::IAllocator& al, const Operation& op) {
        kir::KGraph g(&al);
        return gpu::synth_gemm(c, op, g).reject;
    };

    // OpNotSupported: not a linalg.gemm op (a bare resource.declare).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=tf(ctx,k,b,sh2(ctx,4U,3U));
      CHECK(synth(ctx,root,*v->defining_op())==RJ::OpNotSupported); }
    // ElementNotF32: i32 tensors (find_linalg_misuse-clean — element-agnostic — but the CKIR kernels are F32-only).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ei=ctx.type_i32();
      Value* a=mkval(ctx,k,b,ctx.type_tensor(ei,sh2(ctx,4U,3U))); Value* bb=mkval(ctx,k,b,ctx.type_tensor(ei,sh2(ctx,3U,5U)));
      Value* c=mkval(ctx,k,b,ctx.type_tensor(ei,sh2(ctx,4U,5U)));
      Operation* op=gemm(ctx,k,b,a,bb,c,1.0,0.0,false,false,ctx.type_tensor(ei,sh2(ctx,4U,5U)));
      CHECK(synth(ctx,root,*op)==RJ::ElementNotF32); }
    // GemmEpilogueUnsupported: alpha != 1.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,4U,5U));
      Operation* op=gemm(ctx,k,b,a,bb,c,2.0,0.0,false,false,ctx.type_tensor(ef,sh2(ctx,4U,5U)));
      CHECK(synth(ctx,root,*op)==RJ::GemmEpilogueUnsupported); }
    // GemmEpilogueUnsupported: beta != 0.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* a=tf(ctx,k,b,sh2(ctx,4U,3U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,4U,5U));
      Operation* op=gemm(ctx,k,b,a,bb,c,1.0,1.0,false,false,ctx.type_tensor(ef,sh2(ctx,4U,5U)));
      CHECK(synth(ctx,root,*op)==RJ::GemmEpilogueUnsupported); }
    // GemmEpilogueUnsupported: trans_a (the graph tier has no general 2D transpose).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* a=tf(ctx,k,b,sh2(ctx,3U,4U)); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sh2(ctx,4U,5U));
      Operation* op=gemm(ctx,k,b,a,bb,c,1.0,0.0,true,false,ctx.type_tensor(ef,sh2(ctx,4U,5U)));
      CHECK(synth(ctx,root,*op)==RJ::GemmEpilogueUnsupported); }
    // RankUnsupported: batched (rank-3) gemm.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* a=tf(ctx,k,b,sh3(ctx,2U,4U,3U)); Value* bb=tf(ctx,k,b,sh3(ctx,2U,3U,5U)); Value* c=tf(ctx,k,b,sh3(ctx,2U,4U,5U));
      Operation* op=gemm(ctx,k,b,a,bb,c,1.0,0.0,false,false,ctx.type_tensor(ef,sh3(ctx,2U,4U,5U)));
      CHECK(synth(ctx,root,*op)==RJ::RankUnsupported); }
    // ShapeNotStatic: a dynamic M dim (a device kernel needs a concrete extent).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      const TypeId dyn3[2]={ctx.type_dim_dynamic(), ctx.type_dim_static(3U)};
      const TypeId sdyn=ctx.type_shape(ConstSpan<TypeId>(dyn3,2U));
      const TypeId dyn5[2]={ctx.type_dim_dynamic(), ctx.type_dim_static(5U)};
      const TypeId sdyn5=ctx.type_shape(ConstSpan<TypeId>(dyn5,2U));
      Value* a=tf(ctx,k,b,sdyn); Value* bb=tf(ctx,k,b,sh2(ctx,3U,5U)); Value* c=tf(ctx,k,b,sdyn5);
      Operation* op=gemm(ctx,k,b,a,bb,c,1.0,0.0,false,false,ctx.type_tensor(ef,sdyn5));
      CHECK(synth(ctx,root,*op)==RJ::ShapeNotStatic); }
}

TEST_CASE("ceir 22b: synth_reduce maps sum/prod/max/min to a CKIR reduce + TYPED-REJECTS mean/non-F32/bad-axis", "[ceir][ckir-synth]")
{
    using RJ = gpu::SynthReject;
    // build a tensor.reduce op: reduce(input[6,8]){axis, fn} -> [6] (axis 1 dropped). Returns the op.
    const auto build_reduce = [](Context& ctx, const Kit& k, Block* b, TypeId inShape, TypeId elem, int axis, const char* fn,
                                 TypeId outShape) {
        Operation* const src = ctx.create_operation(k.decl, {}, 1U, ctx.type_tensor(elem, inShape));
        b->append(src);
        Operation* const op = tensor::build_reduce(ctx, src->result(0U), ctx.attr_int(axis), ctx.attr_string(StringView(fn)),
                                                   ctx.type_tensor(elem, outShape));
        b->append(op);
        return op;
    };
    const auto synth = [](Context& c, memory::IAllocator& al, const Operation& op) {
        kir::KGraph g(&al);
        return gpu::synth_reduce(c, op, g);
    };

    // ACCEPT: sum over axis 1 of [6,8] -> a CKIR reduce node.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_reduce(ctx,k,b,sh2(ctx,6U,8U),ctx.type_f32(),1,"sum",sh1(ctx,6U));
      const gpu::GraphSynth s=synth(ctx,root,*op);
      CHECK(s.reject==RJ::None); CHECK(s.output>=0); }
    // prod/max/min also map.
    for (const char* fn : {"prod", "max", "min"})
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_reduce(ctx,k,b,sh2(ctx,6U,8U),ctx.type_f32(),0,fn,sh1(ctx,8U));
      CHECK(synth(ctx,root,*op).reject==RJ::None); }
    // ReduceFnUnsupported: mean (needs a post-scale the graph tier lacks).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_reduce(ctx,k,b,sh2(ctx,6U,8U),ctx.type_f32(),1,"mean",sh1(ctx,6U));
      CHECK(synth(ctx,root,*op).reject==RJ::ReduceFnUnsupported); }
    // ElementNotF32: i32 reduce.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_reduce(ctx,k,b,sh2(ctx,6U,8U),ctx.type_i32(),1,"sum",sh1(ctx,6U));
      CHECK(synth(ctx,root,*op).reject==RJ::ElementNotF32); }
    // ReduceAxisInvalid: axis 5 on a rank-2 tensor.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_reduce(ctx,k,b,sh2(ctx,6U,8U),ctx.type_f32(),5,"sum",sh1(ctx,6U));
      CHECK(synth(ctx,root,*op).reject==RJ::ReduceAxisInvalid); }
}

TEST_CASE("ceir 22b: synth_fft maps a rank-1 power-of-2 c2c FFT to a CKIR plan + TYPED-REJECTS non-pow2/rank/direction/non-F32", "[ceir][ckir-synth]")
{
    using RJ = gpu::SynthReject;
    // build a tensor.fft op (re_in/im_in [shape], forward/inverse, axis) with element `elem`. Returns the op.
    const auto build_fft = [](Context& ctx, const Kit& k, Block* b, TypeId shape, TypeId elem, const char* dir, int axis) {
        Operation* const re = ctx.create_operation(k.decl, {}, 1U, ctx.type_tensor(elem, shape));
        Operation* const im = ctx.create_operation(k.decl, {}, 1U, ctx.type_tensor(elem, shape));
        b->append(re);
        b->append(im);
        Operation* const op = tensor::build_fft(ctx, re->result(0U), im->result(0U), ctx.attr_string(StringView(dir)),
                                                ctx.attr_int(axis), ctx.type_tensor(elem, shape));
        b->append(op);
        return op;
    };
    const auto synth = [](Context& c, memory::IAllocator& al, const Operation& op) {
        kir::KGraph g(&al);
        return gpu::synth_fft(c, op, g);
    };

    // ACCEPT: a rank-1 [8] forward c2c FFT → a CKIR plan with n==8.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_fft(ctx,k,b,sh1(ctx,8U),ctx.type_f32(),"forward",0);
      const gpu::FftSynth s=synth(ctx,root,*op);
      CHECK(s.reject==RJ::None); CHECK(s.n==8); CHECK(s.inverse==false);
      CHECK(s.plan.entry.local_size[0]==4U); } // ⛔ the RADIX-2 contract: n/2 threads (a batched-radix regression = the only other detector is a device hang)
    // ACCEPT: an inverse [16] FFT.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_fft(ctx,k,b,sh1(ctx,16U),ctx.type_f32(),"inverse",0);
      const gpu::FftSynth s=synth(ctx,root,*op);
      CHECK(s.reject==RJ::None); CHECK(s.n==16); CHECK(s.inverse==true);
      CHECK(s.plan.entry.local_size[0]==8U); } // n/2 for n=16
    // FftLengthNotPow2: n=6.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_fft(ctx,k,b,sh1(ctx,6U),ctx.type_f32(),"forward",0);
      CHECK(synth(ctx,root,*op).reject==RJ::FftLengthNotPow2); }
    // FftRankUnsupported: rank-2 [4,4] fft.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_fft(ctx,k,b,sh2(ctx,4U,4U),ctx.type_f32(),"forward",0);
      CHECK(synth(ctx,root,*op).reject==RJ::FftRankUnsupported); }
    // ElementNotF32: i32 fft.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_fft(ctx,k,b,sh1(ctx,8U),ctx.type_i32(),"forward",0);
      CHECK(synth(ctx,root,*op).reject==RJ::ElementNotF32); }
    // FftDirectionUnknown: a bogus direction.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Operation* op=build_fft(ctx,k,b,sh1(ctx,8U),ctx.type_f32(),"sideways",0);
      CHECK(synth(ctx,root,*op).reject==RJ::FftDirectionUnknown); }
}
