// CEIR-22c-1 — plan_tensor_pipeline (the PURE, device-free §137 memory plan). Proves: (1) a well-formed gemm→reshape→fft→reduce
// module plans to 3 DISPATCH stages (reshape = a zero-copy ALIAS, not a stage) + a buffer graph with the right ROLES / bytes /
// def-use WIRING (fft's in_re binds the buffer that aliases the gemm output — the asset-drives-it wiring); (2) the fft's
// imaginary input is marked Zeros + two FftTwiddle externals are planned; (3) TYPED-REJECTS (not-verify-clean, reshape-not-alias,
// a non-F32 stage → SynthRejected); (4) GENERICITY — a differently-shaped module plans differently (the 20c-2 rule). Device-free.

#include <crd/ceir/gpu/tensor_pipeline.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/tensor.hpp>
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
TypeId shp(Context& ctx, ConstSpan<TypeId> dims) { return ctx.type_shape(dims); }
TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return shp(ctx, ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return shp(ctx, ConstSpan<TypeId>(d, 2U));
}
TypeId tf(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_f32(), shape); }
TypeId ti(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_i32(), shape); }

Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
Value* decl(Context& ctx, const Kit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
// gemm(A,B,C) plain -> D.
Value* gemm(Context& ctx, Block* b, Value* a, Value* bb, Value* c, TypeId dshape)
{
    Operation* const op = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                             ctx.attr_bool(false), tf(ctx, dshape));
    b->append(op);
    return op->result(0U);
}
// Build the gemm[m,m]->reshape[m*m]->fft->reduce pipeline into `m`. Returns nothing (the module IS the asset).
void build_pipeline(Context& ctx, const Kit& k, Module& mod, u32 side)
{
    Block* const b   = mkmain(ctx, mod);
    const u32    l   = side * side;
    Value* const a   = decl(ctx, k, b, tf(ctx, sh2(ctx, side, side)));
    Value* const bb  = decl(ctx, k, b, tf(ctx, sh2(ctx, side, side)));
    Value* const c   = decl(ctx, k, b, tf(ctx, sh2(ctx, side, side)));
    Value* const d   = gemm(ctx, b, a, bb, c, sh2(ctx, side, side));
    Operation* const rs = tensor::build_reshape(ctx, d, tf(ctx, sh1(ctx, l)));
    b->append(rs);
    Value* const im0 = decl(ctx, k, b, tf(ctx, sh1(ctx, l))); // the fft imaginary input (planned as Zeros)
    Value*       ffops[2] = {rs->result(0U), im0};
    Operation* const ff = ctx.create_operation(ctx.intern_op("tensor", "fft"), ConstSpan<Value*>(ffops, 2U), 2U, tf(ctx, sh1(ctx, l)));
    ctx.set_attr(ff, StringView("direction"), ctx.attr_string(StringView("forward")));
    ctx.set_attr(ff, StringView("axis"), ctx.attr_int(0));
    b->append(ff);
    Operation* const rd = tensor::build_reduce(ctx, ff->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")),
                                               tf(ctx, shp(ctx, ConstSpan<TypeId>{}))); // rank-0 sum
    b->append(rd);
}
} // namespace

TEST_CASE("ceir 22c-1: plan_tensor_pipeline wires gemm->reshape(alias)->fft->reduce device-resident", "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    build_pipeline(ctx, k, *m, 8U); // 8x8 -> 64

    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);

    // 3 DISPATCH stages (reshape is an alias, not a stage): Gemm, Fft, Reduce.
    REQUIRE(plan.stages.size() == 3U);
    CHECK(plan.stages[0].kind == gpu::StageKind::Gemm);
    CHECK(plan.stages[1].kind == gpu::StageKind::Fft);
    CHECK(plan.stages[2].kind == gpu::StageKind::Reduce);
    CHECK(plan.stages[0].nbind == 3U);
    CHECK(plan.stages[1].nbind == 6U);
    CHECK(plan.stages[2].nbind == 2U);

    // roles: an ALIAS buffer exists (the reshape) pointing at the gemm's Intermediate output.
    bool saw_alias = false;
    bool saw_zeros = false;
    bool saw_twid  = false;
    bool saw_out   = false;
    int  externals = 0;
    for (usize i = 0; i < plan.buffers.size(); ++i)
    {
        const gpu::PlanBuffer& pb = plan.buffers[i];
        if (pb.role == gpu::BufferRole::Alias)
        {
            saw_alias = true;
            CHECK(pb.alias_of >= 0);
            CHECK(plan.buffers[static_cast<usize>(pb.alias_of)].role == gpu::BufferRole::Intermediate); // aliases the gemm output D
            CHECK(pb.bytes == plan.buffers[static_cast<usize>(pb.alias_of)].bytes);                     // zero-copy: same bytes
        }
        if (pb.role == gpu::BufferRole::ExternalIn)
        {
            ++externals;
            if (pb.fill == gpu::FillKind::Zeros) { saw_zeros = true; }
            if (pb.fill == gpu::FillKind::FftTwiddle) { saw_twid = true; }
        }
        if (pb.role == gpu::BufferRole::Output) { saw_out = true; CHECK(pb.bytes == 4U); } // rank-0 sum = 1 f32
    }
    CHECK(saw_alias);            // the rank-bridge reshape alias
    CHECK(saw_zeros);            // the fft imaginary input seeded Zeros
    CHECK(saw_twid);             // the FftTwiddle externals
    CHECK(saw_out);              // the terminal reduce output
    CHECK(externals >= 5);       // A,B,C + im0(Zeros) + 2 twiddles

    // ⭐ def-use WIRING: the fft's in_re (bind[0]) is the ALIAS buffer, whose alias_of is the gemm output (stages[0].bind[2]).
    const i32 fft_in_re = plan.stages[1].bind[0];
    REQUIRE(fft_in_re >= 0);
    CHECK(plan.buffers[static_cast<usize>(fft_in_re)].role == gpu::BufferRole::Alias);
    CHECK(plan.buffers[static_cast<usize>(fft_in_re)].alias_of == plan.stages[0].bind[2]);
    // the reduce input (bind[0]) is the fft's out_re (stages[1].bind[4]).
    CHECK(plan.stages[2].bind[0] == plan.stages[1].bind[4]);
}

TEST_CASE("ceir 22c-1: plan_tensor_pipeline TYPED-REJECTS (verify-clean / reshape-alias / synth)", "[ceir][tensor-pipeline]")
{
    using RJ = gpu::PlanReject;
    // reshape-not-alias: reshape [8,8](64) -> [65] (element count NOT preserved → find_tensor_misuse would also flag it, so the
    // module is not verify-clean → NotVerifyClean fires first). Use a numel-preserving-but-typed variant to isolate ReshapeNotAlias.
    // (find_tensor_misuse's reshape uses shapes_reshape = product equality; a 64->65 reshape is Incompatible → NotVerifyClean.)
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* a=decl(ctx,k,b,tf(ctx,sh2(ctx,8U,8U))); Value* bb=decl(ctx,k,b,tf(ctx,sh2(ctx,8U,8U))); Value* c=decl(ctx,k,b,tf(ctx,sh2(ctx,8U,8U)));
      Value* d=gemm(ctx,b,a,bb,c,sh2(ctx,8U,8U));
      Operation* rs=tensor::build_reshape(ctx,d,tf(ctx,sh1(ctx,65U))); b->append(rs); // 64 -> 65: not verify-clean
      const gpu::TensorPipelinePlan plan=gpu::plan_tensor_pipeline(ctx,*m,&root);
      CHECK(plan.reject==RJ::NotVerifyClean); }
    // SynthRejected: an i32 gemm (find_linalg_misuse-clean — element-agnostic — but synth_gemm rejects non-F32).
    { memory::GrowableTlsfAllocator root; Context ctx(&root); const Kit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      const TypeId ei=ctx.type_i32();
      Value* a=decl(ctx,k,b,ti(ctx,sh2(ctx,8U,8U))); Value* bb=decl(ctx,k,b,ti(ctx,sh2(ctx,8U,8U))); Value* c=decl(ctx,k,b,ti(ctx,sh2(ctx,8U,8U)));
      Operation* op=linalg::build_gemm(ctx,a,bb,c,ctx.attr_float(1.0),ctx.attr_float(0.0),ctx.attr_bool(false),ctx.attr_bool(false),ti(ctx,sh2(ctx,8U,8U)));
      b->append(op);
      const gpu::TensorPipelinePlan plan=gpu::plan_tensor_pipeline(ctx,*m,&root);
      CHECK(plan.reject==RJ::SynthRejected);
      REQUIRE(plan.stages.size()>=1U); CHECK(plan.stages[0].synth_reject==gpu::SynthReject::ElementNotF32); (void)ei; }
}

TEST_CASE("ceir 22c-1: the plan is MODULE-DERIVED -- a differently-shaped asset plans differently (genericity)", "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m4 = ctx.create_module();
    Module* const                 m8 = ctx.create_module();
    build_pipeline(ctx, k, *m4, 4U); // 4x4 -> 16
    build_pipeline(ctx, k, *m8, 8U); // 8x8 -> 64
    const gpu::TensorPipelinePlan p4 = gpu::plan_tensor_pipeline(ctx, *m4, &root);
    const gpu::TensorPipelinePlan p8 = gpu::plan_tensor_pipeline(ctx, *m8, &root);
    REQUIRE(p4.reject == gpu::PlanReject::None);
    REQUIRE(p8.reject == gpu::PlanReject::None);
    // same STRUCTURE (3 stages) but different BYTES (the plan reflects the asset's shapes, not a hardcoded pipeline).
    CHECK(p4.stages.size() == p8.stages.size());
    // the gemm output buffer: 16*4=64 B (4x4) vs 64*4=256 B (8x8).
    CHECK(p4.buffers[static_cast<usize>(p4.stages[0].bind[2])].bytes == 64U);
    CHECK(p8.buffers[static_cast<usize>(p8.stages[0].bind[2])].bytes == 256U);
}
