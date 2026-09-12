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
#include <crd/kir/ckir_eval.hpp> // eval_cpu — the CPU oracle the 25b element-op synths compare against (device-free numeric gate)
#include <crd/kir/ckir_glsl.hpp> // CEIR-26e: emit_contract_glsl — the relu-epilogue store is a device-free source check
#include <crd/kir/ckir_hlsl.hpp> // CEIR-26e: emit_contract_hlsl — the DX12 mirror of the epilogue store
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
// CEIR-26e: does the emitted kernel source contain `needle`? (a device-free proxy for the relu-epilogue store — ASCII scan, no std).
bool src_has(const crd::containers::String& s, StringView needle)
{
    const StringView h = crd::containers::to_view(s);
    if (needle.size() > h.size()) { return false; }
    for (crd::usize i = 0; i + needle.size() <= h.size(); ++i)
    {
        bool m = true;
        for (crd::usize j = 0; j < needle.size(); ++j) { if (h[i + j] != needle[j]) { m = false; break; } }
        if (m) { return true; }
    }
    return false;
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

TEST_CASE("ceir 26e-2a: synth_gemm relu epilogue fuses max(contract 0) bit-exact and the contract emitters unwrap the Max root",
          "[ceir][ckir-synth]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    const TypeId                  ef = ctx.type_f32();
    // gemm(A[2,2], B[2,2]) plain → D[2,2]. A=[[1,2],[3,4]], B=[[1,0],[0,-1]] ⇒ A·B = [[1,-2],[3,-4]] (NEGATIVE outputs, so relu is
    // NOT identity — the advisor's "seed a negative row" gate). relu(A·B) = [[1,0],[3,0]].
    Value* const     a  = tf(ctx, k, b, sh2(ctx, 2U, 2U));
    Value* const     bb = tf(ctx, k, b, sh2(ctx, 2U, 2U));
    Value* const     cc = tf(ctx, k, b, sh2(ctx, 2U, 2U));
    Operation* const op = gemm(ctx, k, b, a, bb, cc, 1.0, 0.0, false, false, ctx.type_tensor(ef, sh2(ctx, 2U, 2U)));

    const f64  ad[4]     = {1, 2, 3, 4};  // A row-major
    const f64  bd[4]     = {1, 0, 0, -1}; // B row-major
    const f64* inputs[2] = {ad, bd};

    // (1) NUMERIC: eval_cpu(synth_gemm(Relu)) == relu(A·B) elementwise (the fused graph carries the Max node the emitters unwrap).
    {
        kir::KGraph           g2(&root);
        const gpu::GraphSynth s = gpu::synth_gemm(ctx, *op, g2, gpu::GemmEpilogue::Relu);
        REQUIRE(s.reject == gpu::SynthReject::None);
        REQUIRE(s.output >= 0);
        f64 out[4] = {};
        kir::eval_cpu(g2, inputs, &root, s.output, out);
        const f64 expect[4] = {1, 0, 3, 0}; // relu([1,-2,3,-4])
        for (int i = 0; i < 4; ++i) { CHECK(out[i] == expect[i]); }
    }
    // (2) REGRESSION: eval_cpu(synth_gemm(None)) == A·B (the default arg leaves every pre-26e caller intact — negatives survive).
    {
        kir::KGraph           g2(&root);
        const gpu::GraphSynth s = gpu::synth_gemm(ctx, *op, g2); // default None
        REQUIRE(s.reject == gpu::SynthReject::None);
        f64 out[4] = {};
        kir::eval_cpu(g2, inputs, &root, s.output, out);
        const f64 expect[4] = {1, -2, 3, -4};
        for (int i = 0; i < 4; ++i) { CHECK(out[i] == expect[i]); }
    }
    // (3) EMIT (both backends): the Relu graph emits `max(acc` at the store; the None graph does NOT (identity, not presence).
    {
        kir::KGraph           gr(&root);
        const gpu::GraphSynth sr = gpu::synth_gemm(ctx, *op, gr, gpu::GemmEpilogue::Relu);
        kir::KGraph           gn(&root);
        const gpu::GraphSynth sn = gpu::synth_gemm(ctx, *op, gn); // None
        kir::GlslKernel       kg_r(&root);
        kir::GlslKernel       kg_n(&root);
        REQUIRE(kir::emit_contract_glsl(gr, sr.output, kg_r));
        REQUIRE(kir::emit_contract_glsl(gn, sn.output, kg_n));
        CHECK(src_has(kg_r.source, StringView("max(acc")));
        CHECK(!src_has(kg_n.source, StringView("max(acc")));
        kir::GlslKernel kh_r(&root);
        kir::GlslKernel kh_n(&root);
        REQUIRE(kir::emit_contract_hlsl(gr, sr.output, kh_r));
        REQUIRE(kir::emit_contract_hlsl(gn, sn.output, kh_n));
        CHECK(src_has(kh_r.source, StringView("max(acc")));
        CHECK(!src_has(kh_n.source, StringView("max(acc")));
    }
    // (4) NEGATIVE (the cval guard): a Max(Contract, Const 5) root is NOT a relu — both emitters REJECT it (never a silent
    //     max-with-a-nonzero false-green). Hand-built (synth_gemm only ever emits cval 0).
    {
        kir::KGraph     g2(&root);
        const int       ia  = g2.input(kir::make_shape({2, 2}), kir::DType::F32);
        const int       ib  = g2.input(kir::make_shape({2, 2}), kir::DType::F32);
        const int       con = g2.contract(ia, ib);
        const int       z5  = g2.constant(5.0, kir::make_shape({2, 2}), kir::DType::F32);
        const int       mx  = g2.binary(kir::KOp::Max, con, z5);
        kir::GlslKernel kg(&root);
        kir::GlslKernel kh(&root);
        CHECK(!kir::emit_contract_glsl(g2, mx, kg));
        CHECK(!kir::emit_contract_hlsl(g2, mx, kh));
    }
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

TEST_CASE("ceir 25b-1: synth_transpose maps tensor.transpose to a CKIR permute + eval_cpu matches the transpose oracle", "[ceir][ckir-synth][autodiff]")
{
    using RJ = gpu::SynthReject;
    // ACCEPT + NUMERIC: transpose A[2,3] perm=[1,0] -> [3,2]; eval_cpu(g,{A}) == Aᵀ (pure data movement — exact f64).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* a=tf(ctx,k,b,sh2(ctx,2U,3U));
      Operation* op=tensor::build_transpose(ctx,a,ctx.attr_string(StringView("1,0")),ctx.type_tensor(ef,sh2(ctx,3U,2U)));
      b->append(op);
      kir::KGraph g(&root);
      const gpu::GraphSynth s=gpu::synth_transpose(ctx,*op,g);
      REQUIRE(s.reject==RJ::None); REQUIRE(s.output>=0);
      const f64 ad[6]={1,2,3, 4,5,6}; const f64* inputs[1]={ad}; f64 out[6]={};
      kir::eval_cpu(g,inputs,&root,s.output,out);
      const f64 expect[6]={1,4, 2,5, 3,6}; // Aᵀ row-major [3,2]
      for (int i=0;i<6;++i) { CHECK(out[i]==expect[i]); } }
    // OpNotSupported: a bare resource.declare.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=tf(ctx,k,b,sh2(ctx,2U,3U)); kir::KGraph g(&root);
      CHECK(gpu::synth_transpose(ctx,*v->defining_op(),g).reject==RJ::OpNotSupported); }
    // ElementNotF32: i32 transpose.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ei=ctx.type_i32();
      Value* a=mkval(ctx,k,b,ctx.type_tensor(ei,sh2(ctx,2U,3U)));
      Operation* op=tensor::build_transpose(ctx,a,ctx.attr_string(StringView("1,0")),ctx.type_tensor(ei,sh2(ctx,3U,2U)));
      b->append(op); kir::KGraph g(&root);
      CHECK(gpu::synth_transpose(ctx,*op,g).reject==RJ::ElementNotF32); }
    // TransposePermInvalid: perm "0,0" is not a permutation (duplicate) — the synth is standalone-robust (does not re-run the verifier).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* a=tf(ctx,k,b,sh2(ctx,2U,3U));
      Operation* op=tensor::build_transpose(ctx,a,ctx.attr_string(StringView("0,0")),ctx.type_tensor(ef,sh2(ctx,3U,2U)));
      b->append(op); kir::KGraph g(&root);
      CHECK(gpu::synth_transpose(ctx,*op,g).reject==RJ::TransposePermInvalid); }
}

TEST_CASE("ceir 25b-1: synth_broadcast maps tensor.broadcast to a CKIR broadcast + eval_cpu matches; rejects non-same-rank", "[ceir][ckir-synth][autodiff]")
{
    using RJ = gpu::SynthReject;
    // ACCEPT + NUMERIC: broadcast in[3,1] -> [3,4]; each row repeats in[r] across 4 cols (eval_cpu ckir_eval.hpp:213 same-rank).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* in=tf(ctx,k,b,sh2(ctx,3U,1U));
      Operation* op=tensor::build_broadcast(ctx,in,ctx.type_tensor(ef,sh2(ctx,3U,4U)));
      b->append(op);
      kir::KGraph g(&root);
      const gpu::GraphSynth s=gpu::synth_broadcast(ctx,*op,g);
      REQUIRE(s.reject==RJ::None); REQUIRE(s.output>=0);
      const f64 ind[3]={10,20,30}; const f64* inputs[1]={ind}; f64 out[12]={};
      kir::eval_cpu(g,inputs,&root,s.output,out);
      for (int r=0;r<3;++r) { for (int c=0;c<4;++c) { CHECK(out[r*4+c]==ind[r]); } } }
    // ACCEPT + NUMERIC (FIRST axis): broadcast in[1,4] -> [3,4]; each COLUMN repeats in[c] down 3 rows (the axis-0 reduce-grad
    // shape — a DIFFERENT branch of eval_cpu:217's index map than the last-axis case above; nothing else in-tree checks it).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* in=tf(ctx,k,b,sh2(ctx,1U,4U));
      Operation* op=tensor::build_broadcast(ctx,in,ctx.type_tensor(ef,sh2(ctx,3U,4U)));
      b->append(op);
      kir::KGraph g(&root);
      const gpu::GraphSynth s=gpu::synth_broadcast(ctx,*op,g);
      REQUIRE(s.reject==RJ::None); REQUIRE(s.output>=0);
      const f64 ind[4]={100,200,300,400}; const f64* inputs[1]={ind}; f64 out[12]={};
      kir::eval_cpu(g,inputs,&root,s.output,out);
      for (int r=0;r<3;++r) { for (int c=0;c<4;++c) { CHECK(out[r*4+c]==ind[c]); } } }
    // OpNotSupported: a bare resource.declare.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=tf(ctx,k,b,sh2(ctx,3U,1U)); kir::KGraph g(&root);
      CHECK(gpu::synth_broadcast(ctx,*v->defining_op(),g).reject==RJ::OpNotSupported); }
    // ElementNotF32: i32 broadcast.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ei=ctx.type_i32();
      Value* in=mkval(ctx,k,b,ctx.type_tensor(ei,sh2(ctx,3U,1U)));
      Operation* op=tensor::build_broadcast(ctx,in,ctx.type_tensor(ei,sh2(ctx,3U,4U)));
      b->append(op); kir::KGraph g(&root);
      CHECK(gpu::synth_broadcast(ctx,*op,g).reject==RJ::ElementNotF32); }
    // BroadcastShapeUnsupported: in[4] (rank-1) -> [3,4] (rank-2) — a numpy right-aligned broadcast the VERIFIER accepts, but the
    // CKIR g.broadcast is same-rank (name-forward; vjp_reduce reshapes to same rank FIRST).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Value* in=tf(ctx,k,b,sh1(ctx,4U));
      Operation* op=tensor::build_broadcast(ctx,in,ctx.type_tensor(ef,sh2(ctx,3U,4U)));
      b->append(op); kir::KGraph g(&root);
      CHECK(gpu::synth_broadcast(ctx,*op,g).reject==RJ::BroadcastShapeUnsupported); }
}

TEST_CASE("ceir 25b-1: synth_elementwise maps the full binary vocab to a CKIR binary + eval_cpu matches; rejects mismatch/bad-fn", "[ceir][ckir-synth][autodiff]")
{
    using RJ = gpu::SynthReject;
    // build an elementwise op a[2,2] {fn} b[2,2] -> [2,2].
    const auto build_ew = [](Context& ctx, const Kit& k, Block* b, TypeId ea, TypeId sa, TypeId eb, TypeId sb, const char* fn,
                             TypeId er, TypeId sr) {
        Value* a=mkval(ctx,k,b,ctx.type_tensor(ea,sa)); Value* bb=mkval(ctx,k,b,ctx.type_tensor(eb,sb));
        Operation* op=tensor::build_elementwise(ctx,a,bb,ctx.attr_string(StringView(fn)),ctx.type_tensor(er,sr));
        b->append(op); return op;
    };
    // ACCEPT + NUMERIC: add / sub / mul over [2,2] == the exact-f64 oracle.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32(); const TypeId s22=sh2(ctx,2U,2U);
      const f64 ad[4]={1,2,3,4}; const f64 bd[4]={10,20,30,40}; const f64* inputs[2]={ad,bd};
      struct Case { const char* fn; f64 e[4]; };
      const Case cases[3]={{"add",{11,22,33,44}},{"sub",{-9,-18,-27,-36}},{"mul",{10,40,90,160}}};
      for (const Case& cs : cases) {
          Operation* op=build_ew(ctx,k,b,ef,s22,ef,s22,cs.fn,ef,s22);
          kir::KGraph g(&root);
          const gpu::GraphSynth s=gpu::synth_elementwise(ctx,*op,g);
          REQUIRE(s.reject==RJ::None); REQUIRE(s.output>=0);
          f64 out[4]={}; kir::eval_cpu(g,inputs,&root,s.output,out);
          for (int i=0;i<4;++i) { CHECK(out[i]==cs.e[i]); }
      } }
    // VOCAB: the FULL binary vocab {add,sub,mul,div,max,min,pow} all MAP (reject None) — the gold-standard completeness claim.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32(); const TypeId s22=sh2(ctx,2U,2U);
      for (const char* fn : {"add","sub","mul","div","max","min","pow"}) {
          Operation* op=build_ew(ctx,k,b,ef,s22,ef,s22,fn,ef,s22);
          kir::KGraph g(&root);
          CHECK(gpu::synth_elementwise(ctx,*op,g).reject==RJ::None);
      } }
    // OpNotSupported: a bare resource.declare.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* v=tf(ctx,k,b,sh2(ctx,2U,2U)); kir::KGraph g(&root);
      CHECK(gpu::synth_elementwise(ctx,*v->defining_op(),g).reject==RJ::OpNotSupported); }
    // ElementNotF32: i32 add.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ei=ctx.type_i32(); const TypeId s22=sh2(ctx,2U,2U);
      Operation* op=build_ew(ctx,k,b,ei,s22,ei,s22,"add",ei,s22); kir::KGraph g(&root);
      CHECK(gpu::synth_elementwise(ctx,*op,g).reject==RJ::ElementNotF32); }
    // ElementwiseFnUnsupported: a bogus fn (the synth is standalone-robust; fn_in would reject it in-dialect).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32(); const TypeId s22=sh2(ctx,2U,2U);
      Operation* op=build_ew(ctx,k,b,ef,s22,ef,s22,"atan2",ef,s22); kir::KGraph g(&root);
      CHECK(gpu::synth_elementwise(ctx,*op,g).reject==RJ::ElementwiseFnUnsupported); }
    // ⛔ ElementwiseShapeMismatch: a[2,2] + b[2,1] — broadcast-compatible per the VERIFIER, but g.binary is same-shape (the
    // bin-bcast OOB scar). NEVER an implicit broadcast → a TYPED reject.
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ef=ctx.type_f32();
      Operation* op=build_ew(ctx,k,b,ef,sh2(ctx,2U,2U),ef,sh2(ctx,2U,1U),"add",ef,sh2(ctx,2U,2U)); kir::KGraph g(&root);
      CHECK(gpu::synth_elementwise(ctx,*op,g).reject==RJ::ElementwiseShapeMismatch); }
}
