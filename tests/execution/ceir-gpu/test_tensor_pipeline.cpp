// CEIR-22c-1 — plan_tensor_pipeline (the PURE, device-free §137 memory plan). Proves: (1) a well-formed gemm→reshape→fft→reduce
// module plans to 3 DISPATCH stages (reshape = a zero-copy ALIAS, not a stage) + a buffer graph with the right ROLES / bytes /
// def-use WIRING (fft's in_re binds the buffer that aliases the gemm output — the asset-drives-it wiring); (2) the fft's
// imaginary input is marked Zeros + two FftTwiddle externals are planned; (3) TYPED-REJECTS (not-verify-clean, reshape-not-alias,
// a non-F32 stage → SynthRejected); (4) GENERICITY — a differently-shaped module plans differently (the 20c-2 rule). Device-free.

#include <crd/ceir/gpu/tensor_pipeline.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/gpu/expand_ml.hpp> // CEIR-26f-2b: expand_ml_ops — plan the ATTENTION module (the named aliasing consumer)
#include <crd/ceir/gpu/grad.hpp> // CEIR-25b-4b STEP 1: build_gradient — plan the DIFFERENTIATED module device-free
#include <crd/ceir/func.hpp>
#include <crd/ceir/passes/canonicalize.hpp> // CEIR-26b: canonicalize/fold — the tensor.reshape-of-reshape fold, composes with DCE
#include <crd/ceir/passes/cse.hpp>       // CEIR-26c: the CSE pass — collapse a duplicate gemm (D2->D1), the plan drops one Gemm stage
#include <crd/ceir/passes/dce.hpp>       // CEIR-26a: the DCE pass — pin-then-prune the dead backward branch (dx / W1ᵀ)
#include <crd/ceir/gen/arith_ops.hpp>    // register_arith_ops — the compute.dispatch grid consts
#include <crd/ceir/gen/compute_ops.hpp>  // register_compute_ops — the viz-prep dispatch (CEIR-22c-3)
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gen/transform_ops.hpp> // CEIR-27a: the transform dialect (transform.fuse / transform.share_storage)
#include <crd/ceir/transform.hpp>          // CEIR-27a: find_transform_misuse (the DuplicateDirective module-wide walk)
#include <crd/ceir/gen/tune_ops.hpp>       // CEIR-28a: the tune dialect (tune.entry config-cache rows)
#include <crd/ceir/tune.hpp>               // CEIR-28a: find_tune_misuse (DuplicateKey) + load_tune_entries
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp>           // CEIR-25c-1b-2: ml.mlp — the composite differentiated then ERASED before planning
#include <crd/ceir/parse.hpp>         // CEIR-22c-3c: parse the authored .ceir asset
#include <crd/ceir/print.hpp>         // CEIR-22c-3c: canonical text for the anti-drift + roundtrip checks
#include <crd/ceir/program_asset.hpp> // CEIR-22c-3c: collect_dependencies (the ckir_refs to the viz kernels)
#include <crd/ceir/quant.hpp>         // CEIR-23b-2a: quant.dequantize -> a Dequant stage
#include <crd/ceir/tensor.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
struct Kit
{
    OpId decl, cst, disp;
    explicit Kit(Context& ctx)
        : decl(ctx.intern_op("resource", "declare")), cst(ctx.intern_op("arith", "const")),
          disp(ctx.intern_op("compute", "dispatch"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)linalg::register_dialect(ctx);
        (void)tensor::register_dialect(ctx);
        (void)arith::register_arith_ops(ctx);     // CEIR-22c-3: the dispatch grid consts
        (void)compute::register_compute_ops(ctx); // CEIR-22c-3: the viz-prep compute.dispatch
        (void)quant::register_dialect(ctx);       // CEIR-23b-2a: quant.dequantize
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
// CEIR-27a: build a `ceir.transform` SCHEDULE module — the directive ops sit DIRECTLY in the module body block (a
// directive is compile metadata, not runtime work, so no func wrapper). `n_dir` directives: always transform.fuse{fuse};
// transform.share_storage{share} only when n_dir==2 (n_dir==1 is the PARTIAL-schedule case — an absent directive must
// keep the loader's base default). Returns the module.
Module* build_transform(Context& ctx, bool fuse, bool share, u32 n_dir = 2U)
{
    Module* const m   = ctx.create_module();
    Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    top->append(transform::build_fuse(ctx, ctx.attr_bool(fuse)));
    if (n_dir >= 2U) { top->append(transform::build_share_storage(ctx, ctx.attr_bool(share))); }
    return m;
}
// CEIR-28a: append one tune.entry cache row (KEY device/env/program_hash/shape -> schedule fuse/share).
Operation* add_tune_entry(Context& ctx, Block* b, const char* device, const char* env, u64 ph, const char* shape, bool fuse,
                          bool share)
{
    Operation* const e = tune::build_entry(ctx, ctx.attr_string(StringView(device)), ctx.attr_string(StringView(env)),
                                           ctx.attr_int(static_cast<crd::i64>(ph)), ctx.attr_string(StringView(shape)),
                                           ctx.attr_bool(fuse), ctx.attr_bool(share));
    b->append(e);
    return e;
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

// ── CEIR-22c-3 (design B) helpers: the viz-prep compute.dispatch of an authored .ckir kernel ──
Value* konst(Context& ctx, const Kit& k, Block* b, i64 v) // an index constant (a dispatch grid operand)
{
    Operation* const c = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
    ctx.set_attr(c, StringView("value"), ctx.attr_int(v));
    b->append(c);
    return c->result(0U);
}
// compute.dispatch(%gx,%gy,%gz, %bindings...) {kernel=@kernel, access}. `binds`/`nb` are the SLOT-order resource bindings.
Operation* mk_dispatch(Context& ctx, const Kit& k, Block* b, Value* gx, Value* gy, Value* gz, Value* const* binds, u32 nb,
                       const char* kernel, const char* access)
{
    containers::Array<Value*> ops(ctx.allocator());
    ops.push_back(gx);
    ops.push_back(gy);
    ops.push_back(gz);
    for (u32 i = 0; i < nb; ++i) { ops.push_back(binds[i]); }
    Operation* const op = ctx.create_operation(k.disp, ConstSpan<Value*>(ops.data(), ops.size()), 0U); // RESULTLESS
    ctx.set_attr(op, StringView("kernel"), ctx.attr_symbol(StringView(kernel)));
    ctx.set_attr(op, StringView("access"), ctx.attr_string(StringView(access)));
    b->append(op);
    return op;
}
// The DESIGN-B §137 pipeline: gemm[side,side] -> reshape[L] -> fft -> mag(CKIR dispatch) -> reduce(MAX, rank-0) ->
// normalize(CKIR dispatch, reads the rank-0 max scalar) -> the terminal [L] normalized spectrum. Two CKIR dispatches
// interleaved with the tensor ops (the charter-literal "reduction -> viz-prep" order; viz-prep TERMINAL).
void build_pipeline_b(Context& ctx, const Kit& k, Module& mod, u32 side)
{
    Block* const b   = mkmain(ctx, mod);
    const u32    l   = side * side;
    Value* const a   = decl(ctx, k, b, tf(ctx, sh2(ctx, side, side)));
    Value* const bb  = decl(ctx, k, b, tf(ctx, sh2(ctx, side, side)));
    Value* const c   = decl(ctx, k, b, tf(ctx, sh2(ctx, side, side)));
    Value* const d   = gemm(ctx, b, a, bb, c, sh2(ctx, side, side));
    Operation* const rs = tensor::build_reshape(ctx, d, tf(ctx, sh1(ctx, l)));
    b->append(rs);
    Value* const im0 = decl(ctx, k, b, tf(ctx, sh1(ctx, l)));
    Value*       ffops[2] = {rs->result(0U), im0};
    Operation* const ff = ctx.create_operation(ctx.intern_op("tensor", "fft"), ConstSpan<Value*>(ffops, 2U), 2U, tf(ctx, sh1(ctx, l)));
    ctx.set_attr(ff, StringView("direction"), ctx.attr_string(StringView("forward")));
    ctx.set_attr(ff, StringView("axis"), ctx.attr_int(0));
    b->append(ff);
    // mag = |fft| : compute.dispatch(@viz_magnitude, r,r,w) over (re, im, mag). One workgroup (grid 1,1,1; local_size = L).
    Value* const gx  = konst(ctx, k, b, 1);
    Value* const g1  = konst(ctx, k, b, 1);
    Value* const mag = decl(ctx, k, b, tf(ctx, sh1(ctx, l)));
    Value*       magbind[3] = {ff->result(0U), ff->result(1U), mag};
    (void)mk_dispatch(ctx, k, b, gx, g1, g1, magbind, 3U, "viz_magnitude", "r,r,w");
    // mx = max(mag) : a rank-0 reduction (the normalization factor).
    Operation* const rd = tensor::build_reduce(ctx, mag, ctx.attr_int(0), ctx.attr_string(StringView("max")),
                                               tf(ctx, shp(ctx, ConstSpan<TypeId>{})));
    b->append(rd);
    // norm = mag / mx : compute.dispatch(@viz_normalize, r,r,w) over (mag, mx[rank-0], norm) — the TERMINAL display spectrum.
    Value* const norm = decl(ctx, k, b, tf(ctx, sh1(ctx, l)));
    Value*       nbind[3] = {mag, rd->result(0U), norm};
    (void)mk_dispatch(ctx, k, b, gx, g1, g1, nbind, 3U, "viz_normalize", "r,r,w");
}

// CEIR-23c — the quantized-MLP CROWN: a 2-layer uniform-width (batch 4 × width 8) MLP, every weight Q8 (int8, per-tensor
// symmetric) dequantized-and-FUSED into its gemm (the QuantGemm collapse), relu between layers.
//   x[4,8] · dequant(W1_q8[8,8],s1,z1) → W1d · gemm(x,W1d,C1) → h1[4,8] · relu(h1)→h1r · dequant(W2_q8,s2,z2) → W2d ·
//   gemm(h1r,W2d,C2) → out[4,8].
// Every gemm is [4,8]·[8,8]→[4,8] so ALL fuse into the SAME baked quant_gemm_q8.ckir (uniform width — no general-dims work).
// relu rides compute.dispatch(@relu, r,w) — the VizDispatch path (no new StageKind). This IS the asset (module == the .ceir).
void build_quant_mlp(Context& ctx, const Kit& k, Module& mod)
{
    Block* const b  = mkmain(ctx, mod);
    const TypeId i8 = ctx.type_int(8U, true);
    const TypeId r0 = shp(ctx, ConstSpan<TypeId>{});
    // ── layer 1: h1 = relu(x · dequant(W1_q8)) ──
    Value* const     x   = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));           // activations x[4,8]
    Value* const     w1  = decl(ctx, k, b, ctx.type_tensor(i8, sh2(ctx, 8U, 8U))); // W1_q8[8,8] (int8)
    Value* const     s1  = decl(ctx, k, b, tf(ctx, r0));                         // scale1 (per-tensor, rank-0)
    Value* const     z1  = decl(ctx, k, b, ctx.type_tensor(i8, r0));             // zero_point1 (int8, rank-0; symmetric ⇒ 0)
    Operation* const dq1 = quant::build_dequantize(ctx, w1, s1, z1, ctx.attr_int(0),
                                                   ctx.attr_string(StringView("symmetric")), tf(ctx, sh2(ctx, 8U, 8U)));
    b->append(dq1);
    Value* const c1  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));   // accumulator C1 (β=0, unused)
    Value* const h1  = gemm(ctx, b, x, dq1->result(0U), c1, sh2(ctx, 4U, 8U));
    Value* const g1c = konst(ctx, k, b, 1);                          // dispatch grid (1,1,1); relu local_size = 32
    Value* const h1r = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));   // relu output h1r[4,8]
    Value*       rbind[2] = {h1, h1r};
    (void)mk_dispatch(ctx, k, b, g1c, g1c, g1c, rbind, 2U, "relu", "r,w");
    // ── layer 2: out = h1r · dequant(W2_q8) ──
    Value* const     w2  = decl(ctx, k, b, ctx.type_tensor(i8, sh2(ctx, 8U, 8U)));
    Value* const     s2  = decl(ctx, k, b, tf(ctx, r0));
    Value* const     z2  = decl(ctx, k, b, ctx.type_tensor(i8, r0));
    Operation* const dq2 = quant::build_dequantize(ctx, w2, s2, z2, ctx.attr_int(0),
                                                   ctx.attr_string(StringView("symmetric")), tf(ctx, sh2(ctx, 8U, 8U)));
    b->append(dq2);
    Value* const c2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
    (void)gemm(ctx, b, h1r, dq2->result(0U), c2, sh2(ctx, 4U, 8U)); // out[4,8] — the terminal
}
} // namespace

// CEIR-26d-2c: the PURE cook-time shape-specialization POLICY (bind_authored_local_size) — the device-free teeth of the RETIRED
// BakedKernelShapeUnsupported reject. Sentinel(0)+in-range binds (None); sentinel(0)+0 is LocalSizeUnbound (never silently 1);
// sentinel(0)+over-cap is LocalSizeExceedsLimit; the boundary (== cap) binds; a NON-sentinel authored size is left untouched.
// This is the [[feedback_deleting_reference_in_ab_parity_test_degrades_to_can_t_fail]] guard for the removed pre-check.
TEST_CASE("ceir 26d-2c: bind_authored_local_size cook-binds the sentinel + rejects unbound/oversize", "[ceir][tensor-pipeline]")
{
    crd::u32 ls = 0U; // sentinel + a valid numel ⇒ bound to the numel
    CHECK(gpu::bind_authored_local_size(ls, 64ULL, gpu::kMaxAuthoredLocalSize) == gpu::KernelShapeError::None);
    CHECK(ls == 64U);

    crd::u32 ls0 = 0U; // sentinel + numel 0 ⇒ LocalSizeUnbound, left untouched (never silently 1)
    CHECK(gpu::bind_authored_local_size(ls0, 0ULL, gpu::kMaxAuthoredLocalSize) == gpu::KernelShapeError::LocalSizeUnbound);
    CHECK(ls0 == 0U);

    crd::u32 lsx = 0U; // sentinel + numel > cap ⇒ LocalSizeExceedsLimit, left untouched
    CHECK(gpu::bind_authored_local_size(lsx, static_cast<crd::u64>(gpu::kMaxAuthoredLocalSize) + 1ULL, gpu::kMaxAuthoredLocalSize)
          == gpu::KernelShapeError::LocalSizeExceedsLimit);
    CHECK(lsx == 0U);

    crd::u32 lscap = 0U; // exactly at the cap ⇒ bound (inclusive boundary)
    CHECK(gpu::bind_authored_local_size(lscap, static_cast<crd::u64>(gpu::kMaxAuthoredLocalSize), gpu::kMaxAuthoredLocalSize)
          == gpu::KernelShapeError::None);
    CHECK(lscap == gpu::kMaxAuthoredLocalSize);

    crd::u32 lsauth = 128U; // a NON-sentinel authored size (fft n/2, viz, softmax/transpose) is LEFT UNTOUCHED regardless of numel
    CHECK(gpu::bind_authored_local_size(lsauth, 999999ULL, gpu::kMaxAuthoredLocalSize) == gpu::KernelShapeError::None);
    CHECK(lsauth == 128U);
}

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

// Read a whole file into a char Array (REQUIRE-fails on a bad path — the authored-asset presence gate).
containers::Array<char> slurp(const char* path, Context& ctx)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    containers::Array<char> s(ctx.allocator());
    s.resize(static_cast<usize>(sz), '\0');
    f.read(s.data(), sz);
    return s;
}

TEST_CASE("ceir 23c-b: the authored quant_mlp.ceir parse-loads, verifies clean, plans QuantGemm->relu->QuantGemm, matches the builder",
          "[ceir][tensor-pipeline][quant]")
{
    // ── the C++-built reference MLP + its canonical print (the anti-drift oracle; build_quant_mlp IS the asset's source) ──
    memory::GrowableTlsfAllocator rootb;
    Context                       ctxb(&rootb);
    const Kit                     kb(ctxb);
    Module* const                 mb = ctxb.create_module();
    build_quant_mlp(ctxb, kb, *mb);
    const containers::String t_built = print(ctxb, *mb, &rootb);

    // ── parse-load the COMMITTED authored asset (the printer/parser handles !i8 + quant.dequantize + rank-0 !shape<>) ──
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const containers::Array<char> src = slurp(CRD_REPO_DIR "/assets/ceir/quant_mlp.ceir", ctx);
    const ParseResult             pr  = parse(ctx, StringView(src.data(), src.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ⭐ ANTI-DRIFT: the committed asset's canonical print == the builder's (if build_quant_mlp changes, regenerate the file).
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ROUNDTRIP stability: print(parse(file)) re-parses to the identical text (the printer/parser fixed point for int8 + quant).
    Context           ctx2(&root);
    const Kit         k2(ctx2);
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

    // ── the parsed module is VERIFY-CLEAN across every walk the plan trusts (incl. find_quant_misuse — the quantize ops) ──
    CHECK(linalg::find_linalg_misuse(ctx, *pr.module).kind == linalg::LinalgMisuseKind::None);
    CHECK(tensor::find_tensor_misuse(ctx, *pr.module).kind == tensor::TensorMisuseKind::None);
    CHECK(quant::find_quant_misuse(ctx, *pr.module).kind == quant::QuantMisuseKind::None);
    CHECK(ctx.find_dispatch_misuse(*pr.module).kind == DispatchMisuseKind::None);
    CHECK(find_unregistered_op(ctx, *pr.module) == nullptr); // every op kind registered (no vacuous pass)

    // ── the PARSED module plans to the composed quantized MLP: QuantGemm(layer1) -> VizDispatch(relu) -> QuantGemm(layer2) ──
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *pr.module, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U); // both dequant->gemm pairs COLLAPSED; the relu dispatch survives
    CHECK(plan.stages[0].kind == gpu::StageKind::QuantGemm);
    CHECK(plan.stages[1].kind == gpu::StageKind::VizDispatch); // relu
    CHECK(plan.stages[2].kind == gpu::StageKind::QuantGemm);

    // ── the asset DECLARES its ONE CKIR kernel dependency (the relu dispatch; the QuantGemm kernel is a RESOLVER concern) ──
    const DependencyRecord dep = collect_dependencies(ctx, *pr.module, &root);
    REQUIRE(dep.ckir_refs.size() == 1U);
    CHECK(dep.ckir_refs[0].name == StringView("relu"));
    CHECK(!dep.ckir_refs[0].pinned);
}

// CEIR-27a — the `ceir.transform` schedule ASSET -> PlanOptions loader. Proves: an AUTHORED transform module sets the two
// program-global knobs (the PlanOptions C++ struct hoisted into an asset), the mapping survives print->parse (it is a TEXT
// asset, not just an in-memory build), and an ABSENT directive keeps the base default (a partial schedule is legal). This
// is the mechanism 27b's two-schedule bit-exact differential rides. Device-free.
TEST_CASE("ceir 27a: a ceir.transform schedule asset loads into PlanOptions (both knobs, text round-trip, partial)",
          "[ceir][tensor-pipeline][transform]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    (void)transform::register_transform_ops(ctx); // the Kit does not register it

    // ── (a) BOTH-OFF: base defaults (true,true); the schedule flips both to false ──
    Module* const off = build_transform(ctx, /*fuse=*/false, /*share=*/false);
    // IDENTITY (non-vacuous): the module holds EXACTLY the two directive ops of the right kinds — not an empty/dropped walk.
    {
        const OpId   fk = transform::fuse_kind(ctx);
        const OpId   sk = transform::share_storage_kind(ctx);
        Block* const b  = off->body()->first_block();
        REQUIRE(b != nullptr);
        u32 n = 0U;
        u32 n_fuse = 0U;
        u32 n_share = 0U;
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            ++n;
            if (op->kind() == fk) { ++n_fuse; }
            if (op->kind() == sk) { ++n_share; }
        }
        REQUIRE(n == 2U);
        REQUIRE(n_fuse == 1U);
        REQUIRE(n_share == 1U);
    }
    // WALK-THEN-LOAD: a well-formed schedule (each directive at most once) passes find_transform_misuse; the loader trusts
    // a walked module (the guard is the SEPARATE module-wide walk, not the loader — the find_*_misuse house pattern).
    CHECK(transform::find_transform_misuse(ctx, *off).kind == transform::TransformMisuseKind::None);
    const gpu::PlanOptions got_off = gpu::plan_options_from_transform(ctx, *off, gpu::PlanOptions{});
    CHECK(got_off.fuse_gemm_relu == false);
    CHECK(got_off.share_intermediate_storage == false);

    // ── (b) TEXT-ASSET round-trip: print -> parse in a FRESH context -> load -> SAME options (it is an authorable asset) ──
    const containers::String txt = print(ctx, *off, &root);
    Context                  ctx2(&root);
    const Kit                k2(ctx2);
    (void)transform::register_transform_ops(ctx2);
    const ParseResult pr = parse(ctx2, StringView(txt.c_str(), txt.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const containers::String txt2 = print(ctx2, *pr.module, &root); // roundtrip stable
    CHECK(StringView(txt2.c_str(), txt2.size()) == StringView(txt.c_str(), txt.size()));
    const gpu::PlanOptions got_parsed = gpu::plan_options_from_transform(ctx2, *pr.module, gpu::PlanOptions{});
    CHECK(got_parsed.fuse_gemm_relu == false);
    CHECK(got_parsed.share_intermediate_storage == false);

    // ── (c) BOTH-ON from a false base: the schedule flips both to true (the OTHER direction — not just "defaults survive") ──
    Module* const    on = build_transform(ctx, /*fuse=*/true, /*share=*/true);
    gpu::PlanOptions base_false;
    base_false.fuse_gemm_relu           = false;
    base_false.share_intermediate_storage = false;
    const gpu::PlanOptions got_on = gpu::plan_options_from_transform(ctx, *on, base_false);
    CHECK(got_on.fuse_gemm_relu == true);
    CHECK(got_on.share_intermediate_storage == true);

    // ── (d) PARTIAL schedule: ONLY transform.fuse{false} — the ABSENT share_storage keeps the base (true), fuse flips ──
    Module* const          partial     = build_transform(ctx, /*fuse=*/false, /*share=*/false, /*n_dir=*/1U);
    const gpu::PlanOptions  got_partial = gpu::plan_options_from_transform(ctx, *partial, gpu::PlanOptions{});
    CHECK(got_partial.fuse_gemm_relu == false);            // the present directive fired
    CHECK(got_partial.share_intermediate_storage == true); // the ABSENT directive kept the base default (partial schedule legal)

    // ── (e) DUPLICATE directive → find_transform_misuse REJECTS (a hand-authored asset could set a knob twice; the loader is
    //        last-write-wins SILENTLY, so the module-wide walk is the guard the walk-then-load discipline relies on) ──
    Module* const dup = ctx.create_module();
    Block*        dtop = dup->body()->first_block();
    if (dtop == nullptr) { dtop = ctx.create_block(0U); dup->body()->append(dtop); }
    dtop->append(transform::build_fuse(ctx, ctx.attr_bool(true)));
    dtop->append(transform::build_fuse(ctx, ctx.attr_bool(false))); // the DUPLICATE (second transform.fuse)
    const transform::TransformMisuse mis = transform::find_transform_misuse(ctx, *dup);
    CHECK(mis.kind == transform::TransformMisuseKind::DuplicateDirective);
    CHECK(mis.op != nullptr); // points at the offending (duplicate) op
}

// CEIR-27b (device-free half) — the committed `.ceir` SCHEDULE ASSETS parse-load, are CANONICAL (roundtrip-stable), match the
// build_transform oracle (anti-drift), pass the module-wide walk, and load to the expected PlanOptions. The "schedules are
// ASSETS" proof (§146: store the transform schedule as an asset); the DEVICE bit-exact §146 differential rides these files
// (ceir 27b in the Vulkan/DX12 pipeline TUs). ⛔ if build_transform changes, REGENERATE the committed files (the 22c-3c rule).
TEST_CASE("ceir 27b: the committed schedule_fuse / schedule_nofuse .ceir assets are canonical + load to PlanOptions",
          "[ceir][tensor-pipeline][transform]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    (void)transform::register_transform_ops(ctx); // the Kit does not register it

    struct SchedCase
    {
        const char* path;
        bool        fuse;
        bool        share;
    };
    const SchedCase cases[2] = {
        {CRD_REPO_DIR "/assets/ceir/schedule_fuse.ceir", true, true},
        {CRD_REPO_DIR "/assets/ceir/schedule_nofuse.ceir", false, false},
    };
    for (const SchedCase& c : cases)
    {
        // the C++ build_transform oracle + its canonical print (the anti-drift source; keep the builder, do NOT delete it).
        Module* const            built   = build_transform(ctx, c.fuse, c.share);
        const containers::String t_built = print(ctx, *built, &root);

        // parse-load the COMMITTED asset.
        const containers::Array<char> src = slurp(c.path, ctx);
        const ParseResult             pr  = parse(ctx, StringView(src.data(), src.size()));
        REQUIRE(pr.ok);
        REQUIRE(pr.module != nullptr);

        // ⭐ ANTI-DRIFT: the committed file's canonical print == the builder's (regenerate the file if build_transform changes).
        const containers::String t_file = print(ctx, *pr.module, &root);
        CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

        // ROUNDTRIP stability: print(parse(file)) re-parses to the identical canonical text (the committed file IS canonical).
        Context           ctx2(&root);
        const Kit         k2(ctx2);
        (void)transform::register_transform_ops(ctx2);
        const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
        REQUIRE(pr2.ok);
        const containers::String t_file2 = print(ctx2, *pr2.module, &root);
        CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

        // the schedule is well-formed (each program-global directive at most once — the walk-then-load discipline).
        CHECK(transform::find_transform_misuse(ctx, *pr.module).kind == transform::TransformMisuseKind::None);

        // loads to the expected knobs from an all-true base (so an unset knob would still read true — but both are set here).
        const gpu::PlanOptions got = gpu::plan_options_from_transform(ctx, *pr.module, gpu::PlanOptions{});
        CHECK(got.fuse_gemm_relu == c.fuse);
        CHECK(got.share_intermediate_storage == c.share);
    }
}

// CEIR-28a — the ceir.tune TARGET-SPECIFIC CONFIG CACHE: a `tune.entry` maps a device/env/program/shape KEY to a schedule
// (fuse/share = PlanOptions); plan_options_from_tune_cache REPLAYS the matching row (sec-80, the v17 select_schedule discipline
// made portable). (a) a 2-row cache walks clean + loads the fields (IDENTITY: exactly 2 tune.entry ops); (b) HIT — each row's KEY
// yields its schedule; (c) MISS — an unknown device returns hit=false + base UNCHANGED; (d) the FULL key is matched — env AND
// program_hash discriminate (a query differing only in env / only in hash MISSES, proving neither is ignored — IDENTITY not
// category); (e) DuplicateKey — two rows with the SAME key are rejected (the loader would be ambiguous). Device-free.
TEST_CASE("ceir 28a: a ceir.tune config cache loads + plan_options_from_tune_cache replays the matching row (full-key match)",
          "[ceir][tensor-pipeline][tune]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    (void)tune::register_tune_ops(ctx); // the Kit does not register it

    // ── (a) a 2-row cache (distinct device keys), walks clean, loads 2 entries with the expected fields ──
    Module* const cache = ctx.create_module();
    Block*        cb    = cache->body()->first_block();
    if (cb == nullptr) { cb = ctx.create_block(0U); cache->body()->append(cb); }
    add_tune_entry(ctx, cb, "vk:rtx4070ti", "win32", 101U, "mlp:4x8x8", /*fuse=*/true, /*share=*/false);
    add_tune_entry(ctx, cb, "dx12:rtx4070ti", "win32", 101U, "mlp:4x8x8", /*fuse=*/false, /*share=*/true);
    {
        const OpId ek      = tune::entry_kind(ctx); // IDENTITY (non-vacuous): exactly 2 tune.entry ops of the right kind
        u32        n       = 0U;
        u32        n_entry = 0U;
        for (const Operation* op = cb->first_op(); op != nullptr; op = op->next_in_block())
        {
            ++n;
            if (op->kind() == ek) { ++n_entry; }
        }
        CHECK(n == 2U);
        CHECK(n_entry == 2U);
    }
    CHECK(tune::find_tune_misuse(ctx, *cache).kind == tune::TuneMisuseKind::None);
    containers::Array<tune::TuneEntry> entries(&root);
    REQUIRE(tune::load_tune_entries(ctx, *cache, entries) == 2U);
    CHECK(entries[0].device == StringView("vk:rtx4070ti"));
    CHECK(entries[0].env == StringView("win32"));
    CHECK(entries[0].program_hash == 101U);
    CHECK(entries[0].shape == StringView("mlp:4x8x8"));
    CHECK(entries[0].fuse == true);
    CHECK(entries[0].share == false);
    CHECK(entries[1].device == StringView("dx12:rtx4070ti"));
    CHECK(entries[1].fuse == false);
    CHECK(entries[1].share == true);

    // ── (b) HIT: each row's full KEY replays its schedule ──
    const gpu::TuneCacheLookup h0 = gpu::plan_options_from_tune_cache(ctx, *cache, StringView("vk:rtx4070ti"),
                                                                      StringView("win32"), 101U, StringView("mlp:4x8x8"));
    CHECK(h0.hit);
    CHECK(h0.opts.fuse_gemm_relu == true);
    CHECK(h0.opts.share_intermediate_storage == false);
    const gpu::TuneCacheLookup h1 = gpu::plan_options_from_tune_cache(ctx, *cache, StringView("dx12:rtx4070ti"),
                                                                      StringView("win32"), 101U, StringView("mlp:4x8x8"));
    CHECK(h1.hit);
    CHECK(h1.opts.fuse_gemm_relu == false);
    CHECK(h1.opts.share_intermediate_storage == true);

    // ── (c) MISS: an unknown device returns hit=false + base UNCHANGED (the caller — 28c locked mode — decides fallback/reject) ──
    gpu::PlanOptions base;
    base.fuse_gemm_relu             = true;
    base.share_intermediate_storage = true;
    const gpu::TuneCacheLookup miss = gpu::plan_options_from_tune_cache(ctx, *cache, StringView("vk:llvmpipe"),
                                                                        StringView("win32"), 101U, StringView("mlp:4x8x8"), base);
    CHECK_FALSE(miss.hit);
    CHECK(miss.opts.fuse_gemm_relu == true); // base preserved
    CHECK(miss.opts.share_intermediate_storage == true);

    // ── (d) FULL-KEY discrimination (IDENTITY not category): env AND program_hash are matched, not ignored ──
    const gpu::TuneCacheLookup wrong_env = gpu::plan_options_from_tune_cache(ctx, *cache, StringView("vk:rtx4070ti"),
                                                                             StringView("linux"), 101U, StringView("mlp:4x8x8"));
    CHECK_FALSE(wrong_env.hit); // same device+hash+shape, DIFFERENT env ⇒ MISS (env is a real match dimension)
    const gpu::TuneCacheLookup wrong_hash = gpu::plan_options_from_tune_cache(ctx, *cache, StringView("vk:rtx4070ti"),
                                                                              StringView("win32"), 999U, StringView("mlp:4x8x8"));
    CHECK_FALSE(wrong_hash.hit); // different program_hash ⇒ MISS

    // ── (e) DuplicateKey: two rows with the SAME full key ⇒ find_tune_misuse rejects (the loader would be ambiguous) ──
    Module* const dup = ctx.create_module();
    Block*        db  = dup->body()->first_block();
    if (db == nullptr) { db = ctx.create_block(0U); dup->body()->append(db); }
    add_tune_entry(ctx, db, "vk:rtx4070ti", "win32", 7U, "mlp:2x2x2", true, true);
    add_tune_entry(ctx, db, "vk:rtx4070ti", "win32", 7U, "mlp:2x2x2", false, false); // same KEY, different schedule
    CHECK(tune::find_tune_misuse(ctx, *dup).kind == tune::TuneMisuseKind::DuplicateKey);

    // ── (f) MODULE-WIDE DuplicateKey (advisor): the SAME key in DIFFERENT top-level blocks is still a duplicate — find_tune_misuse
    //        is module-wide, matching load_tune_entries' scope. A per-block check (each block has ONE entry) would MISS this and
    //        silently load both ⇒ the loader would be ambiguous. This two-block module is the discriminating input. ──
    Module* const mb = ctx.create_module();
    Block*        b0 = mb->body()->first_block();
    if (b0 == nullptr) { b0 = ctx.create_block(0U); mb->body()->append(b0); }
    Block* const b1 = ctx.create_block(0U);
    mb->body()->append(b1);
    add_tune_entry(ctx, b0, "vk:rtx4070ti", "win32", 5U, "s", true, false);
    add_tune_entry(ctx, b1, "vk:rtx4070ti", "win32", 5U, "s", false, true); // same KEY, DIFFERENT block
    CHECK(tune::find_tune_misuse(ctx, *mb).kind == tune::TuneMisuseKind::DuplicateKey);
    // and load_tune_entries sees BOTH (module-wide) — the scope that makes the misuse check load-bearing.
    containers::Array<tune::TuneEntry> mb_entries(&root);
    CHECK(tune::load_tune_entries(ctx, *mb, mb_entries) == 2U);
}

// CEIR-28b-1 — program_hash: the program-identity KEY = fnv1a-64 of the module's CANONICAL PRINT. (a) two separately-built
// structurally-identical modules hash EQUAL (print-equal ⇒ hash-equal by construction); (b) a changed operand type ⇒ different
// hash; (c) ⭐ DISCRIMINATOR — the SAME ml.mlp module hashed BEFORE vs AFTER expand_ml_ops ⇒ DIFFERENT (the "hash the
// POST-expansion payload the planner consumes" claim GATED, not commented: an ml.mlp and its gemm/relu expansion are different
// programs). Device-free. The KEY producer 28b-2's measurer will call after expansion, before planning.
TEST_CASE("ceir 28b-1: program_hash is the fnv1a of the canonical print (post-expansion payload identity)",
          "[ceir][tensor-pipeline][tune]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    (void)ml::register_dialect(ctx); // ml.mlp — the Kit does not register it

    // build a 2-layer ml.mlp payload (x[8,4]·W1[4,4] → relu → ·W2[4,d_out]) into a fresh module (the mkmain+decl mold).
    const auto build_mlp = [&](u32 d_out) -> Module* {
        Module* const    m       = ctx.create_module();
        Block* const     b       = mkmain(ctx, *m);
        Value* const     xin     = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 4U)));
        Value* const     w1      = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value* const     w2      = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, d_out)));
        Value*           mops[3] = {xin, w1, w2};
        Operation* const mlp = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mops, 3U), 1U, tf(ctx, sh2(ctx, 8U, d_out)));
        ctx.set_attr(mlp, StringView("activation"), ctx.attr_string(StringView("relu")));
        b->append(mlp);
        return m;
    };

    // ── (a) print-equal ⇒ hash-equal: two SEPARATELY-built structurally-identical modules ──
    Module* const m1 = build_mlp(4U);
    Module* const m2 = build_mlp(4U);
    CHECK(tune::program_hash(ctx, *m1, &root) == tune::program_hash(ctx, *m2, &root));

    // ── (b) a changed operand type (d_out 4→8) ⇒ different hash (the print differs) ──
    Module* const m3 = build_mlp(8U);
    CHECK(tune::program_hash(ctx, *m3, &root) != tune::program_hash(ctx, *m1, &root));

    // ── (c) DISCRIMINATOR: the SAME module BEFORE vs AFTER expand_ml_ops ⇒ DIFFERENT (post-expansion is the planner's program) ──
    Module* const m4    = build_mlp(4U);
    const crd::u64 h_pre = tune::program_hash(ctx, *m4, &root);
    const gpu::MlExpandResult er = gpu::expand_ml_ops(ctx, *m4);
    REQUIRE(er.error == gpu::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    const crd::u64 h_post = tune::program_hash(ctx, *m4, &root);
    CHECK(h_pre != h_post); // ⛔ the measurer must hash AFTER expansion — the ml.mlp and its expansion are distinct programs
}

// CEIR-28c — the DETERMINISTIC LOCKED MODE (sec-80 "deterministic locked configuration mode"): plan_tensor_pipeline_cached picks the
// schedule the config cache holds for THIS target, keyed (device, env, program_hash(m), shape). Device-free (a plan-time policy — no
// GPU). The payload is a 2-layer ml.mlp EXPANDED, whose plan shape READS OUT the chosen schedule: fuse=true ⇒ GemmRelu+Gemm (2 stages),
// fuse=false ⇒ Gemm+VizDispatch(relu)+Gemm (3). Proven: (a) a HIT drives the plan from the ROW (fuse=false) OVER a CONTRADICTING base
// (fuse=true) — IDENTITY (VizDispatch present, no GemmRelu), not a bare stage count; this ALSO discriminates the KEY — a 2nd row at
// program_hash+1 with the OPPOSITE schedule is the trap the wrapper would hit if it hashed anything but `m`; (b) a MISS under Fallback
// falls back to `base` (GemmRelu present); (c) a MISS under Locked is a TYPED reject (TuneCacheLockedMiss, reject_op null, zero stages).
TEST_CASE("ceir 28c: plan_tensor_pipeline_cached selects the cached schedule (hit beats base, key is program_hash); locked miss is a typed reject",
          "[ceir][tensor-pipeline][tune]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    (void)ml::register_dialect(ctx);    // ml.mlp
    (void)tune::register_tune_ops(ctx); // tune.entry cache rows

    const auto has_kind = [](const gpu::TensorPipelinePlan& p, gpu::StageKind kind) {
        for (usize i = 0; i < p.stages.size(); ++i) { if (p.stages[i].kind == kind) { return true; } }
        return false;
    };

    // ── the payload: a 2-layer ml.mlp (x[8,4]·W1[4,4] → relu → ·W2[4,5]) EXPANDED to the planner's gemm/relu program ──
    Module* const m = ctx.create_module();
    Block* const  b = mkmain(ctx, *m);
    Value* const  x  = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 4U)));
    Value* const  w1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const  w2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 5U)));
    Value*        mops[3] = {x, w1, w2};
    Operation* const mlp = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mops, 3U), 1U, tf(ctx, sh2(ctx, 8U, 5U)));
    ctx.set_attr(mlp, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mlp);
    REQUIRE(gpu::expand_ml_ops(ctx, *m).error == gpu::MlExpandError::None);

    const crd::u64   h = tune::program_hash(ctx, *m, &root); // the key the wrapper computes INTERNALLY from `m`
    const char* const device = "vk:test-device";
    const char* const env    = "win32";
    const char* const shape  = "mlp:8x4x4x5";
    const gpu::PlanOptions base_fuse; // {fuse=true, share=true} — the DEFAULT, which CONTRADICTS the cached fuse=false row below

    // a 2-row cache: row1 = the REAL row (key h → fuse=false); row2 = a TRAP at h+1 with the OPPOSITE schedule (fuse=true). The wrapper
    // hits row1 ONLY if it hashes exactly `m`; a wrong hash lands on the trap (fuse=true, 2 stages) or misses — either would betray it.
    Module* const cache = ctx.create_module();
    Block*        cb    = cache->body()->first_block();
    if (cb == nullptr) { cb = ctx.create_block(0U); cache->body()->append(cb); }
    add_tune_entry(ctx, cb, device, env, h, shape, /*fuse=*/false, /*share=*/true);
    add_tune_entry(ctx, cb, device, env, h + 1U, shape, /*fuse=*/true, /*share=*/true); // the wrong-hash trap
    REQUIRE(tune::find_tune_misuse(ctx, *cache).kind == tune::TuneMisuseKind::None);

    // ── (a) HIT: the ROW's fuse=false drives the plan, beating base's fuse=true — AND proves the key is program_hash(m) ──
    {
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline_cached(
            ctx, *m, &root, *cache, StringView(device), StringView(env), StringView(shape), gpu::TunePolicy::Fallback, base_fuse);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        CHECK(plan.stages.size() == 3U);                        // Gemm + VizDispatch(relu) + Gemm — the fuse=false shape
        CHECK(has_kind(plan, gpu::StageKind::VizDispatch));     // the un-fused relu survived (the ROW won over the fuse=true base)
        CHECK_FALSE(has_kind(plan, gpu::StageKind::GemmRelu));  // NOT the default fuse plan, NOT the h+1 trap row (fuse=true)
    }

    // ── (b) MISS + Fallback: an unknown device key → plan with `base` (fuse=true), NOT measured ──
    {
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline_cached(
            ctx, *m, &root, *cache, StringView("vk:other-device"), StringView(env), StringView(shape), gpu::TunePolicy::Fallback, base_fuse);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        CHECK(plan.stages.size() == 2U);                        // GemmRelu + Gemm — the default base schedule
        CHECK(has_kind(plan, gpu::StageKind::GemmRelu));
    }

    // ── (c) MISS + Locked: an unknown device key → a TYPED reject (ship nothing unmeasured), no op at fault, no plan ──
    {
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline_cached(
            ctx, *m, &root, *cache, StringView("vk:other-device"), StringView(env), StringView(shape), gpu::TunePolicy::Locked, base_fuse);
        CHECK(plan.reject == gpu::PlanReject::TuneCacheLockedMiss);
        CHECK(plan.reject_op == nullptr);                       // ⛔ the miss is a KEY property, not a module property
        CHECK(plan.stages.size() == 0U);                        // a locked build refuses to fall back — nothing planned
    }
}

TEST_CASE("ceir 22c-3c: the authored tensor_pipeline.ceir parse-loads, verifies clean, plans 5 stages, and matches the builder",
          "[ceir][tensor-pipeline]")
{
    // ── the C++-built reference module + its canonical print (the anti-drift oracle) ──
    memory::GrowableTlsfAllocator rootb;
    Context                       ctxb(&rootb);
    const Kit                     kb(ctxb);
    Module* const                 mb = ctxb.create_module();
    build_pipeline_b(ctxb, kb, *mb, 8U);
    const containers::String t_built = print(ctxb, *mb, &rootb);

    // ── parse-load the COMMITTED authored asset (dialects registered via the Kit) ──
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx); // registers func/resource/linalg/tensor/arith/compute
    const containers::Array<char> src = slurp(CRD_REPO_DIR "/assets/ceir/tensor_pipeline.ceir", ctx);
    const ParseResult             pr  = parse(ctx, StringView(src.data(), src.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ⭐ ANTI-DRIFT: the committed asset's canonical print == the builder's (if build_pipeline_b changes, regenerate the file).
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ROUNDTRIP stability: print(parse(file)) re-parses to the identical text (the printer/parser fixed point).
    Context           ctx2(&root);
    const Kit         k2(ctx2);
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

    // ── the parsed module is VERIFY-CLEAN across every walk the plan trusts ──
    CHECK(linalg::find_linalg_misuse(ctx, *pr.module).kind == linalg::LinalgMisuseKind::None);
    CHECK(tensor::find_tensor_misuse(ctx, *pr.module).kind == tensor::TensorMisuseKind::None);
    CHECK(ctx.find_dispatch_misuse(*pr.module).kind == DispatchMisuseKind::None);
    CHECK(find_unregistered_op(ctx, *pr.module) == nullptr); // every op kind registered (no vacuous pass)

    // ── the PARSED module plans to the same 5-stage design-B pipeline ──
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *pr.module, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);
    REQUIRE(plan.stages.size() == 5U);
    CHECK(plan.stages[2].kind == gpu::StageKind::VizDispatch);
    CHECK(plan.stages[4].kind == gpu::StageKind::VizDispatch);

    // ── the asset DECLARES its two CKIR kernel dependencies (the §106 ckir_refs, sorted by name, unpinned) ──
    const DependencyRecord dep = collect_dependencies(ctx, *pr.module, &root);
    REQUIRE(dep.ckir_refs.size() == 2U);
    CHECK(dep.ckir_refs[0].name == StringView("viz_magnitude"));
    CHECK(dep.ckir_refs[1].name == StringView("viz_normalize"));
    CHECK(!dep.ckir_refs[0].pinned); // unpinned (no kernel_interface authored) — legal per the compute.dispatch TOML
    CHECK(!dep.ckir_refs[1].pinned);
}

TEST_CASE("ceir 22c-3a: design-B pipeline plans 5 stages with the two viz dispatches wired (mag Intermediate, norm Output)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    build_pipeline_b(ctx, k, *m, 8U); // 8x8 -> 64

    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);

    // 5 DISPATCH stages (reshape = alias): Gemm, Fft, VizDispatch(mag), Reduce(max), VizDispatch(normalize).
    REQUIRE(plan.stages.size() == 5U);
    CHECK(plan.stages[0].kind == gpu::StageKind::Gemm);
    CHECK(plan.stages[1].kind == gpu::StageKind::Fft);
    CHECK(plan.stages[2].kind == gpu::StageKind::VizDispatch);
    CHECK(plan.stages[3].kind == gpu::StageKind::Reduce);
    CHECK(plan.stages[4].kind == gpu::StageKind::VizDispatch);
    CHECK(plan.stages[2].nbind == 3U);
    CHECK(plan.stages[2].n_out == 1U);
    CHECK(plan.stages[4].nbind == 3U);
    CHECK(plan.stages[4].n_out == 1U);

    // ⭐ the mag dispatch reads the fft outputs (bind[4]=out_re, bind[5]=out_im) and writes the mag buffer (bind[2]).
    CHECK(plan.stages[2].bind[0] == plan.stages[1].bind[4]); // mag reads fft out_re
    CHECK(plan.stages[2].bind[1] == plan.stages[1].bind[5]); // mag reads fft out_im
    const i32 mag = plan.stages[2].bind[2];
    REQUIRE(mag >= 0);
    CHECK(plan.buffers[static_cast<usize>(mag)].role == gpu::BufferRole::Intermediate); // ⭐ a WRITE binding re-marked (not ExternalIn)
    CHECK(plan.buffers[static_cast<usize>(mag)].bytes == 256U);                          // [64] f32

    // reduce(max) reads mag; the normalize dispatch reads mag + the rank-0 max scalar, and writes the terminal norm.
    CHECK(plan.stages[3].bind[0] == mag);                          // reduce input = mag
    const i32 mx = plan.stages[3].bind[1];                          // the reduce output (rank-0)
    CHECK(plan.buffers[static_cast<usize>(mx)].bytes == 4U);        // ⭐ a rank-0 tensor binding = 1 f32
    CHECK(plan.stages[4].bind[0] == mag);                          // normalize reads mag
    CHECK(plan.stages[4].bind[1] == mx);                           // normalize reads the max scalar (a resource-kinded Tensor)
    const i32 norm = plan.stages[4].bind[2];
    CHECK(plan.buffers[static_cast<usize>(norm)].role == gpu::BufferRole::Output); // ⭐ terminal = the FINAL stage's write bind
    CHECK(plan.buffers[static_cast<usize>(norm)].bytes == 256U);                    // the [64] normalized spectrum

    // exactly ONE Output buffer, and it is the normalize dispatch's write binding (the resultless-dispatch Output rule).
    int n_out_bufs = 0;
    for (usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].role == gpu::BufferRole::Output) { ++n_out_bufs; CHECK(static_cast<i32>(i) == norm); }
    }
    CHECK(n_out_bufs == 1);
}

TEST_CASE("ceir 22c-3a: a compute.dispatch with a non-trailing write binding is DispatchOutputsNotTrailing",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);
    Value* const                  x = decl(ctx, k, b, tf(ctx, sh1(ctx, 4U)));
    Value* const                  y = decl(ctx, k, b, tf(ctx, sh1(ctx, 4U)));
    Value* const                  g = konst(ctx, k, b, 1);
    Value*                        binds[2] = {x, y};
    // access "w,r" — the WRITE binding is FIRST, not trailing. Dispatch-verify-clean (valid tokens, count == bindings), but the
    // plan's "outputs = the last n_out binds" contract forbids it.
    (void)mk_dispatch(ctx, k, b, g, g, g, binds, 2U, "bad", "w,r");
    REQUIRE(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::None); // verify-clean: the trailing rule is the PLAN's, not the walk's
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    CHECK(plan.reject == gpu::PlanReject::DispatchOutputsNotTrailing);
}

TEST_CASE("ceir 23b-2a: quant.dequantize plans to a Dequant stage (int8 W_q8 sizes to N bytes; symmetric 3-bind)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);
    const TypeId                  i8 = ctx.type_int(8U, true);
    // per-tensor: W_q8 int8 [64], scale f32 [] (RANK-0), zp int8 [] (RANK-0) -> dequantize -> out f32 [64].
    const TypeId r0     = shp(ctx, ConstSpan<TypeId>{});
    Value* const wq     = decl(ctx, k, b, ctx.type_tensor(i8, sh1(ctx, 64U)));
    Value* const sc     = decl(ctx, k, b, tf(ctx, r0));
    Value* const zp     = decl(ctx, k, b, ctx.type_tensor(i8, r0));
    Operation* const dq = quant::build_dequantize(ctx, wq, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                                  tf(ctx, sh1(ctx, 64U)));
    b->append(dq);

    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);
    REQUIRE(plan.stages.size() == 1U);
    CHECK(plan.stages[0].kind == gpu::StageKind::Dequant);
    CHECK(plan.stages[0].nbind == 3U); // symmetric: {W_q8, scale, out} — zp validated but NOT bound
    CHECK(plan.stages[0].n_out == 1U);
    // ⭐ the tensor_bytes ELEMENT-SIZE fix: W_q8 int8 [64] = 64 bytes (NOT 256) == the u32-packed device size.
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[0])].bytes == 64U);
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[1])].bytes == 4U);   // scale f32 rank-0 = 1 elem
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[2])].bytes == 256U); // out f32 [64]
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[2])].role == gpu::BufferRole::Output);
}

TEST_CASE("ceir 23b-2b: dequantize->gemm(weight) COLLAPSES to one QuantGemm stage (the dequant output never allocated)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  i8 = ctx.type_int(8U, true);
    const TypeId                  r0 = shp(ctx, ConstSpan<TypeId>{});
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    // A f32 [4,8] · W_q8 int8 [8,4] → dequantize → W_dq f32 [8,4] · C f32 [4,4] · D = gemm(A, W_dq, C).
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const     wq = decl(ctx, k, b, ctx.type_tensor(i8, sh2(ctx, 8U, 4U)));
    Value* const     sc = decl(ctx, k, b, tf(ctx, r0));
    Value* const     zp = decl(ctx, k, b, ctx.type_tensor(i8, r0));
    Operation* const dq = quant::build_dequantize(ctx, wq, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                                  tf(ctx, sh2(ctx, 8U, 4U)));
    b->append(dq);
    Value* const c = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    (void)gemm(ctx, b, a, dq->result(0U), c, sh2(ctx, 4U, 4U)); // D = gemm(A, W_dq, C)

    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);
    REQUIRE(plan.stages.size() == 1U); // ⭐ COLLAPSED: the dequantize is SKIPPED, the gemm → ONE QuantGemm stage
    CHECK(plan.stages[0].kind == gpu::StageKind::QuantGemm);
    CHECK(plan.stages[0].nbind == 4U);
    CHECK(plan.stages[0].n_out == 1U);
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[0])].bytes == 128U); // A f32 [4,8]
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[1])].bytes == 32U);  // ⭐ W_q8 int8 [8,4] = 32 B (alias-through to the dequant INPUT)
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[2])].bytes == 4U);   // scale
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[3])].bytes == 64U);  // D f32 [4,4]
    CHECK(plan.buffers[static_cast<usize>(plan.stages[0].bind[3])].role == gpu::BufferRole::Output);
    // ⭐ the dequantize's RESULT is NEVER allocated (the §54 fusion win): no plan buffer realizes it.
    bool dq_allocated = false;
    for (usize i = 0; i < plan.buffers.size(); ++i) { if (plan.buffers[i].value == dq->result(0U)) { dq_allocated = true; } }
    CHECK(!dq_allocated);
}

TEST_CASE("ceir 26e-2b a plain f32 gemm-relu epilogue FUSES to one GemmRelu stage (z gone + h Intermediate) QuantGemm not hijacked",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const auto                    buf_of = [](const gpu::TensorPipelinePlan& p, const Value* v) -> crd::i32 {
        for (usize i = 0; i < p.buffers.size(); ++i) { if (p.buffers[i].value == v) { return static_cast<crd::i32>(i); } }
        return -1;
    };
    const auto has_gemmrelu = [](const gpu::TensorPipelinePlan& p) {
        for (usize i = 0; i < p.stages.size(); ++i) { if (p.stages[i].kind == gpu::StageKind::GemmRelu) { return true; } }
        return false;
    };

    // ── (a) POSITIVE: a 2-layer f32 MLP x[2,3]·W1[3,4] → relu → ·W2[4,5]. The interior gemm→relu FUSES; the output gemm stays. ──
    {
        Module* const m  = ctx.create_module();
        Block* const  b  = mkmain(ctx, *m);
        Value* const  x  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 3U)));
        Value* const  w1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
        Value* const  c1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value* const  z1 = gemm(ctx, b, x, w1, c1, sh2(ctx, 2U, 4U)); // z1 = x·W1 — the fused-away intermediate
        Value* const  g  = konst(ctx, k, b, 1);
        Value* const  h1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value*        rb[2] = {z1, h1};
        (void)mk_dispatch(ctx, k, b, g, g, g, rb, 2U, "relu", "r,w");
        Value* const w2  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 5U)));
        Value* const c2  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 5U)));
        Value* const out = gemm(ctx, b, h1, w2, c2, sh2(ctx, 2U, 5U)); // terminal output layer (NOT fused — no relu)

        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        REQUIRE(plan.stages.size() == 2U); // ⭐ N-1: GemmRelu(z1→h1) + Gemm(out); the unfused form is 3 (Gemm+VizDispatch+Gemm)
        CHECK(plan.stages[0].kind == gpu::StageKind::GemmRelu);
        CHECK(plan.stages[1].kind == gpu::StageKind::Gemm);
        CHECK(gpu::fusable_gemm_into_relu(ctx, z1->defining_op())); // the shared predicate FIRED on the interior gemm
        CHECK(buf_of(plan, z1) == -1);                              // ⭐ z1 (the gemm result) is NEVER allocated (fused away)
        const crd::i32 bh = buf_of(plan, h1);
        REQUIRE(bh >= 0);
        CHECK(plan.buffers[static_cast<usize>(bh)].role == gpu::BufferRole::Intermediate); // ⭐ the ExternalIn→Intermediate re-mark landed
        CHECK(plan.stages[0].nbind == 3U);
        CHECK(plan.stages[0].n_out == 1U);
        CHECK(plan.stages[0].bind[0] == buf_of(plan, x));  // A
        CHECK(plan.stages[0].bind[1] == buf_of(plan, w1)); // B
        CHECK(plan.stages[0].bind[2] == bh);               // out = h1 (named-buffer, not just N-1 count)
        CHECK(plan.buffers[static_cast<usize>(buf_of(plan, out))].role == gpu::BufferRole::Output);
    }

    // ── (b) NEG multi-use: a gemm result read by TWO relu dispatches (num_uses==2) → NOT fusable → plain Gemm, no GemmRelu. ──
    {
        Module* const m  = ctx.create_module();
        Block* const  b  = mkmain(ctx, *m);
        Value* const  x  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 3U)));
        Value* const  w1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
        Value* const  c1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value* const  z1 = gemm(ctx, b, x, w1, c1, sh2(ctx, 2U, 4U));
        Value* const  g  = konst(ctx, k, b, 1);
        Value* const  h1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value*        rb1[2] = {z1, h1};
        (void)mk_dispatch(ctx, k, b, g, g, g, rb1, 2U, "relu", "r,w");
        Value* const h2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value*       rb2[2] = {z1, h2}; // z1's SECOND consumer
        (void)mk_dispatch(ctx, k, b, g, g, g, rb2, 2U, "relu", "r,w");

        CHECK(!gpu::fusable_gemm_into_relu(ctx, z1->defining_op())); // num_uses != 1
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        CHECK(!has_gemmrelu(plan));
        CHECK(plan.stages[0].kind == gpu::StageKind::Gemm); // the gemm plans plainly, both relus are VizDispatch
    }

    // ── (c) NEG non-plain: an α=2 gemm → relu → NOT fusable (gemm_is_plain false); the gemm plans plainly → synth REJECTS (as it
    //       always would — the fold declines, the reject is UNCHANGED from today; advisor's corrected (ii)). ──
    {
        Module* const    m  = ctx.create_module();
        Block* const     b  = mkmain(ctx, *m);
        Value* const     x  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 3U)));
        Value* const     w1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
        Value* const     c1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Operation* const g2 = linalg::build_gemm(ctx, x, w1, c1, ctx.attr_float(2.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                                 ctx.attr_bool(false), tf(ctx, sh2(ctx, 2U, 4U))); // α=2 — NON-plain
        b->append(g2);
        Value* const g  = konst(ctx, k, b, 1);
        Value* const h1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value*       rb[2] = {g2->result(0U), h1};
        (void)mk_dispatch(ctx, k, b, g, g, g, rb, 2U, "relu", "r,w");

        CHECK(!gpu::fusable_gemm_into_relu(ctx, g2)); // gemm_is_plain false
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        CHECK(plan.reject == gpu::PlanReject::SynthRejected); // synth_gemm rejects the α=2 gemm (never "runs unfused" — impossible)
        CHECK(!has_gemmrelu(plan));
    }

    // ── (d) NEG standalone relu: a relu dispatch whose read is a plain declare (NOT a gemm result) → VizDispatch, no GemmRelu
    //       (relu.ckir dispatches standalone — the 23c-a / 25c-1b path). ──
    {
        Module* const m   = ctx.create_module();
        Block* const  b   = mkmain(ctx, *m);
        Value* const  in  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U))); // a plain declare, not a gemm result
        Value* const  g   = konst(ctx, k, b, 1);
        Value* const  out = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value*        rb[2] = {in, out};
        (void)mk_dispatch(ctx, k, b, g, g, g, rb, 2U, "relu", "r,w");

        CHECK(!gpu::fusable_gemm_into_relu(ctx, in->defining_op())); // the producer is a resource.declare, not a gemm
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        CHECK(!has_gemmrelu(plan));
        CHECK(plan.stages[0].kind == gpu::StageKind::VizDispatch);
    }

    // ── (e) NEG quant-MLP-NOT-HIJACKED (advisor CRITICAL, a FULL PLAN not a predicate unit): the quant MLP dequant→gemm→relu is
    //       BOTH a QuantGemm target AND a GemmRelu producer; the !fusable_dequant exclusion keeps the fold OFF it. ──
    {
        Module* const m = ctx.create_module();
        build_quant_mlp(ctx, k, *m);
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        REQUIRE(plan.stages.size() == 3U); // QuantGemm → VizDispatch(relu) → QuantGemm (UNCHANGED — the fold did not hijack)
        CHECK(plan.stages[0].kind == gpu::StageKind::QuantGemm);
        CHECK(plan.stages[1].kind == gpu::StageKind::VizDispatch);
        CHECK(plan.stages[2].kind == gpu::StageKind::QuantGemm);
        CHECK(!has_gemmrelu(plan)); // ⭐ NO GemmRelu — the !fusable_dequant_into_gemm_weight exclusion holds
    }
}

TEST_CASE("ceir 23b-2b NEG wrong-slot: dequantize feeding gemm.operand-0 (ACTIVATION) does NOT fuse (weight-slot only)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  i8 = ctx.type_int(8U, true);
    const TypeId                  r0 = shp(ctx, ConstSpan<TypeId>{});
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    // W_dq (dequantized) is the ACTIVATION (operand-0) here, NOT the weight — must NOT fuse (only operand-1 is the weight slot).
    Value* const     wq = decl(ctx, k, b, ctx.type_tensor(i8, sh2(ctx, 4U, 8U))); // W_q8 int8 [4,8]
    Value* const     sc = decl(ctx, k, b, tf(ctx, r0));
    Value* const     zp = decl(ctx, k, b, ctx.type_tensor(i8, r0));
    Operation* const dq = quant::build_dequantize(ctx, wq, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                                  tf(ctx, sh2(ctx, 4U, 8U)));
    b->append(dq);
    Value* const bb = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 4U))); // B f32 [8,4] (the actual weight)
    Value* const c  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    (void)gemm(ctx, b, dq->result(0U), bb, c, sh2(ctx, 4U, 4U)); // D = gemm(W_dq, B, C) — W_dq is A

    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);
    REQUIRE(plan.stages.size() == 2U); // NOT collapsed: a real Dequant stage + a normal Gemm
    bool has_dequant = false;
    bool has_gemm    = false;
    bool has_qgemm   = false;
    for (usize i = 0; i < plan.stages.size(); ++i)
    {
        has_dequant = has_dequant || plan.stages[i].kind == gpu::StageKind::Dequant;
        has_gemm    = has_gemm || plan.stages[i].kind == gpu::StageKind::Gemm;
        has_qgemm   = has_qgemm || plan.stages[i].kind == gpu::StageKind::QuantGemm;
    }
    CHECK(has_dequant);
    CHECK(has_gemm);
    CHECK(!has_qgemm);
    // the dequant result IS allocated here (it is a real consumed intermediate).
    bool dq_allocated = false;
    for (usize i = 0; i < plan.buffers.size(); ++i) { if (plan.buffers[i].value == dq->result(0U)) { dq_allocated = true; } }
    CHECK(dq_allocated);
}

TEST_CASE("ceir 23b-2b NEG multi-use: dequantize feeding TWO gemms' weights does NOT fuse (single-use only)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  i8 = ctx.type_int(8U, true);
    const TypeId                  r0 = shp(ctx, ConstSpan<TypeId>{});
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    Value* const     wq = decl(ctx, k, b, ctx.type_tensor(i8, sh2(ctx, 8U, 4U))); // W_q8 int8 [8,4]
    Value* const     sc = decl(ctx, k, b, tf(ctx, r0));
    Value* const     zp = decl(ctx, k, b, ctx.type_tensor(i8, r0));
    Operation* const dq = quant::build_dequantize(ctx, wq, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                                  tf(ctx, sh2(ctx, 8U, 4U)));
    b->append(dq);
    // TWO gemms share the same dequantized weight (operand-1 in both) → num_uses(W_dq)==2 → never fusable.
    Value* const a1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const c1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const a2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const c2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    (void)gemm(ctx, b, a1, dq->result(0U), c1, sh2(ctx, 4U, 4U));
    (void)gemm(ctx, b, a2, dq->result(0U), c2, sh2(ctx, 4U, 4U));

    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U); // Dequant + Gemm + Gemm — no QuantGemm
    crd::u32 n_dequant = 0;
    crd::u32 n_gemm    = 0;
    crd::u32 n_qgemm   = 0;
    for (usize i = 0; i < plan.stages.size(); ++i)
    {
        n_dequant += plan.stages[i].kind == gpu::StageKind::Dequant ? 1U : 0U;
        n_gemm += plan.stages[i].kind == gpu::StageKind::Gemm ? 1U : 0U;
        n_qgemm += plan.stages[i].kind == gpu::StageKind::QuantGemm ? 1U : 0U;
    }
    CHECK(n_dequant == 1U);
    CHECK(n_gemm == 2U);
    CHECK(n_qgemm == 0U);
    CHECK(!gpu::fusable_dequant_into_gemm_weight(ctx, dq)); // the predicate itself rejects multi-use
}

TEST_CASE("ceir 23b-2b NEG asymmetric: an ASYMMETRIC dequantize never fuses AND the plan TYPED-REJECTS it (no silent-symmetric)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  i8 = ctx.type_int(8U, true);
    const TypeId                  r0 = shp(ctx, ConstSpan<TypeId>{});
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const     wq = decl(ctx, k, b, ctx.type_tensor(i8, sh2(ctx, 8U, 4U)));
    Value* const     sc = decl(ctx, k, b, tf(ctx, r0));
    Value* const     zp = decl(ctx, k, b, ctx.type_tensor(i8, r0)); // ⛔ dequantize storage-side zp is int8 (per §54)
    Operation* const dq = quant::build_dequantize(ctx, wq, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("asymmetric")),
                                                  tf(ctx, sh2(ctx, 8U, 4U)));
    b->append(dq);
    Value* const c = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    (void)gemm(ctx, b, a, dq->result(0U), c, sh2(ctx, 4U, 4U));

    CHECK(!gpu::fusable_dequant_into_gemm_weight(ctx, dq)); // asymmetric → NEVER fuses (drops the zp subtract)
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    CHECK(plan.reject == gpu::PlanReject::UnsupportedQuantScheme); // ⛔ typed reject, not a silent symmetric miscompile
    CHECK(plan.reject_op == dq);
}

TEST_CASE("ceir 23b-2b NEG per-axis: a PER-AXIS (rank-1 scale) dequantize never fuses AND the plan TYPED-REJECTS it",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  i8 = ctx.type_int(8U, true);
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const     wq = decl(ctx, k, b, ctx.type_tensor(i8, sh2(ctx, 8U, 4U)));
    Value* const     sc = decl(ctx, k, b, tf(ctx, sh1(ctx, 8U)));        // scale RANK-1 [8] (per-axis over axis 0)
    Value* const     zp = decl(ctx, k, b, ctx.type_tensor(i8, sh1(ctx, 8U))); // zp shape must match scale
    Operation* const dq = quant::build_dequantize(ctx, wq, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                                  tf(ctx, sh2(ctx, 8U, 4U)));
    b->append(dq);
    Value* const c = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    (void)gemm(ctx, b, a, dq->result(0U), c, sh2(ctx, 4U, 4U));

    CHECK(!gpu::fusable_dequant_into_gemm_weight(ctx, dq)); // per-axis scale → NEVER fuses (the kernel reads scale[0])
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    CHECK(plan.reject == gpu::PlanReject::UnsupportedQuantScheme);
    CHECK(plan.reject_op == dq);
}

TEST_CASE("ceir 23b-2b NEG non-plain gemm: a beta!=0 (accumulating) gemm does NOT fuse (the fused kernel is alpha=1, beta=0)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  i8 = ctx.type_int(8U, true);
    const TypeId                  r0 = shp(ctx, ConstSpan<TypeId>{});
    Module* const                 m  = ctx.create_module();
    Block* const                  b  = mkmain(ctx, *m);
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const     wq = decl(ctx, k, b, ctx.type_tensor(i8, sh2(ctx, 8U, 4U)));
    Value* const     sc = decl(ctx, k, b, tf(ctx, r0));
    Value* const     zp = decl(ctx, k, b, ctx.type_tensor(i8, r0));
    Operation* const dq = quant::build_dequantize(ctx, wq, sc, zp, ctx.attr_int(0), ctx.attr_string(StringView("symmetric")),
                                                  tf(ctx, sh2(ctx, 8U, 4U))); // symmetric per-tensor — plannable UNFUSED
    b->append(dq);
    Value* const     c = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    // beta = 1.0 → D = A·W_dq + 1·C (accumulating) — the fused kernel applies no beta·C, so it MUST NOT fuse.
    Operation* const g = linalg::build_gemm(ctx, a, dq->result(0U), c, ctx.attr_float(1.0), ctx.attr_float(1.0),
                                            ctx.attr_bool(false), ctx.attr_bool(false), tf(ctx, sh2(ctx, 4U, 4U)));
    b->append(g);

    CHECK(!gpu::fusable_dequant_into_gemm_weight(ctx, dq)); // beta!=0 → the fusion gate rejects it (fused kernel is alpha=1,beta=0)
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    // the accumulating gemm is NOT collapsed into a QuantGemm — no fused stage appears in the plan (whatever the unfused Gemm
    // synth then does with beta!=0 is a separate matter; the fusion gate must simply not have fired).
    bool has_qgemm = false;
    for (usize i = 0; i < plan.stages.size(); ++i) { has_qgemm = has_qgemm || plan.stages[i].kind == gpu::StageKind::QuantGemm; }
    CHECK(!has_qgemm);
}

TEST_CASE("ceir 25b-3: plan routes tensor.transpose/broadcast/elementwise as stages + reshape as alias, def-use wired", "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);
    // in0[2,3] --transpose[1,0]--> t[3,2] ; in1[3,1] --broadcast--> bc[3,2] ; add(t,bc)-> e[3,2] ; reshape-> r[6] (alias) — the
    // reverse-pass vocab (transpose + broadcast + elementwise-add + a reshape rank-bridge) in one module.
    Value* const     in0 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 3U)));
    Operation* const tr  = tensor::build_transpose(ctx, in0, ctx.attr_string(StringView("1,0")), tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(tr);
    Value* const     in1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 1U)));
    Operation* const bc  = tensor::build_broadcast(ctx, in1, tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(bc);
    Operation* const ew = tensor::build_elementwise(ctx, tr->result(0U), bc->result(0U), ctx.attr_string(StringView("add")),
                                                    tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(ew);
    Operation* const rs = tensor::build_reshape(ctx, ew->result(0U), tf(ctx, sh1(ctx, 6U)));
    b->append(rs);

    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U); // transpose, broadcast, elementwise (reshape is an alias, NOT a stage)
    CHECK(plan.stages[0].kind == gpu::StageKind::Transpose);
    CHECK(plan.stages[0].nbind == 2U);
    CHECK(plan.stages[0].n_out == 1U);
    CHECK(plan.stages[1].kind == gpu::StageKind::Broadcast);
    CHECK(plan.stages[1].nbind == 2U);
    CHECK(plan.stages[1].n_out == 1U);
    CHECK(plan.stages[2].kind == gpu::StageKind::Elementwise);
    CHECK(plan.stages[2].nbind == 3U);
    CHECK(plan.stages[2].n_out == 1U);
    // def-use WIRING: elementwise reads transpose's output (bind0) and broadcast's output (bind1) — the same-buffer key is the Value.
    CHECK(plan.stages[2].bind[0] == plan.stages[0].bind[1]); // t
    CHECK(plan.stages[2].bind[1] == plan.stages[1].bind[1]); // bc
    // broadcast GROWS the buffer: out[3,2] (6 f32) == 2x in[3,1] (3 f32) — the one place a broadcast stage's byte math (which the
    // executor's barrier + profile consume) could be wrong.
    CHECK(plan.buffers[static_cast<usize>(plan.stages[1].bind[1])].bytes
          == 2U * plan.buffers[static_cast<usize>(plan.stages[1].bind[0])].bytes);
    // reshape = a zero-copy ALIAS of the elementwise output (its own buffer aliases e; no stage).
    bool found_alias = false;
    for (usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].role == gpu::BufferRole::Alias)
        {
            found_alias = true;
            CHECK(plan.buffers[i].alias_of == plan.stages[2].bind[2]); // aliases e (the elementwise output buffer)
        }
    }
    CHECK(found_alias);
}

TEST_CASE("ceir 25b-3: plan TYPED-REJECTS a broadcast-compat-but-not-same-shape elementwise (SynthRejected, the g.binary same-shape envelope)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);
    Value* const                  a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 2U)));
    Value* const                  bb = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 1U))); // broadcasts up to [3,2] -> VERIFY-CLEAN
    Operation* const              ew = tensor::build_elementwise(ctx, a, bb, ctx.attr_string(StringView("add")), tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(ew);
    // find_tensor_misuse accepts the broadcast; but synth_elementwise is SAME-SHAPE (g.binary, the bin-bcast OOB scar) -> a TYPED reject.
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    CHECK(plan.reject == gpu::PlanReject::SynthRejected);
    CHECK(plan.reject_op == ew);
}

// CEIR-25b-4b STEP 1 (device-FREE): the WHOLE backward pass of the 25a-2 corpus plans as ONE device-resident pipeline. This is the
// gate that catches a routing/emit-order gap BEFORE a device is touched: it confirms (a) the gemm VJP's EXPLICIT tensor.transpose
// materializes and feeds a backward gemm operand (def-use, not adjacency), (b) plan-finalize marks the FINAL stage's output = the
// accumulation grads[0] as the single readback target (a wrong emit order would move Output off grads[0]), (c) the reduce VJP's
// [N]->[N,1] reshape stays a zero-copy ALIAS of the caller-uploaded seed, not a stage. STEP 2 (a device gate) runs this same plan
// on Vk+DX12+llvmpipe vs the hesap nn_reverse analytic ref + a gradient_check FD witness.
TEST_CASE("ceir 25b-4b STEP 1: build_gradient(gemm(A,A)->reduce(sum)) plans as a device-resident backward pipeline (stages/roles/readback-target)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // the 25a-2 corpus: loss[3] = reduce(gemm(A[3,3], A[3,3]), axis=1, sum). A is SQUARE (BOTH gemm operands) so the reverse pass
    // ACCUMULATES the two operand adjoints via one tensor.elementwise{add} — the terminal backward op (the readback target).
    constexpr u32    nn   = 3U;
    Value* const     a    = decl(ctx, k, b, tf(ctx, sh2(ctx, nn, nn)));
    Value* const     carg = decl(ctx, k, b, tf(ctx, sh2(ctx, nn, nn))); // the gemm beta*C operand (beta=0)
    Value* const     cval = gemm(ctx, b, a, a, carg, sh2(ctx, nn, nn)); // C = A*A
    Operation* const rd =
        tensor::build_reduce(ctx, cval, ctx.attr_int(1), ctx.attr_string(StringView("sum")), tf(ctx, sh1(ctx, nn)));
    b->append(rd);
    Value* const loss = rd->result(0U);

    gpu::VjpRegistry reg(&root);
    gpu::register_builtin_vjps(reg, ctx);
    Value*                wrt[1]   = {a};
    Value*                grads[1] = {nullptr};
    const gpu::GradResult gr =
        gpu::build_gradient(ctx, *m, reg, loss, ConstSpan<Value*>(wrt, 1U), containers::Span<Value*>(grads, 1U), &root);
    REQUIRE(gr.error == gpu::GradError::None);
    REQUIRE(grads[0] != nullptr);
    REQUIRE(gr.seed != nullptr); // the dLoss seed handle rides out — an executor names THIS buffer to upload all-ones (STEP 2)

    // plan the DIFFERENTIATED module device-free.
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);

    // 8 dispatched stages (the reduce-VJP reshape seed[3]->[3,1] is a zero-copy ALIAS, not a stage):
    //   [0] fwd gemm C=A*A   [1] fwd reduce loss=sum(C,axis=1)   [2] broadcast dC=[3,1]->[3,3]
    //   [3] transpose Aᵀ (Bᵀ, for dA0)   [4] transpose Aᵀ (for dA1)
    //   [5] gemm dA0=gemm(dC,Aᵀ)   [6] gemm dA1=gemm(Aᵀ,dC)   [7] elementwise dA=add(dA0,dA1) == grads[0]
    REQUIRE(plan.stages.size() == 8U);
    CHECK(plan.stages[0].kind == gpu::StageKind::Gemm);
    CHECK(plan.stages[1].kind == gpu::StageKind::Reduce);
    CHECK(plan.stages[2].kind == gpu::StageKind::Broadcast);
    CHECK(plan.stages[3].kind == gpu::StageKind::Transpose);
    CHECK(plan.stages[4].kind == gpu::StageKind::Transpose);
    CHECK(plan.stages[5].kind == gpu::StageKind::Gemm);
    CHECK(plan.stages[6].kind == gpu::StageKind::Gemm);
    CHECK(plan.stages[7].kind == gpu::StageKind::Elementwise);

    // (a) the EXPLICIT transpose materializes and FEEDS a backward gemm operand (def-use, not adjacency). Gemm binds {A,B,D}
    //     (C dropped under beta==0), so dA0's B is bind[1], dA1's A is bind[0].
    CHECK(plan.stages[5].nbind == 3U);
    CHECK(plan.stages[5].n_out == 1U);
    CHECK(plan.stages[5].bind[1] == plan.stages[3].bind[1]); // dA0.B  == transpose[3].out (Bᵀ)
    CHECK(plan.stages[6].bind[0] == plan.stages[4].bind[1]); // dA1.A  == transpose[4].out (Aᵀ)

    // the accumulation elementwise reads BOTH backward gemm outputs (dA0 @ bind[0], dA1 @ bind[1]).
    CHECK(plan.stages[7].nbind == 3U);
    CHECK(plan.stages[7].n_out == 1U);
    CHECK(plan.stages[7].bind[0] == plan.stages[5].bind[2]); // dA0.out
    CHECK(plan.stages[7].bind[1] == plan.stages[6].bind[2]); // dA1.out

    // (b) READBACK TARGET: finalize marks the FINAL stage's trailing write Output; it must be grads[0] and the ONLY Output buffer.
    const crd::i32 out_bind = plan.stages[7].bind[2];
    REQUIRE(out_bind >= 0);
    CHECK(plan.buffers[static_cast<usize>(out_bind)].role == gpu::BufferRole::Output);
    CHECK(plan.buffers[static_cast<usize>(out_bind)].value == grads[0]);
    u32 n_output = 0;
    for (usize i = 0; i < plan.buffers.size(); ++i) { n_output += plan.buffers[i].role == gpu::BufferRole::Output ? 1U : 0U; }
    CHECK(n_output == 1U);

    // (c) the reduce-VJP reshape ([3]->[3,1], the 1D right-align dodge) is a zero-copy ALIAS of the caller-uploaded SEED, and the
    //     broadcast reads THAT alias — never a dispatched stage.
    const crd::i32 bcast_in = plan.stages[2].bind[0];
    REQUIRE(bcast_in >= 0);
    CHECK(plan.buffers[static_cast<usize>(bcast_in)].role == gpu::BufferRole::Alias);
    const crd::i32 seed_buf = plan.buffers[static_cast<usize>(bcast_in)].alias_of;
    REQUIRE(seed_buf >= 0);
    CHECK(plan.buffers[static_cast<usize>(seed_buf)].role == gpu::BufferRole::ExternalIn);
    CHECK(plan.buffers[static_cast<usize>(seed_buf)].value == gr.seed); // DISCRIMINATING: the alias points at the RETURNED seed handle
}

TEST_CASE("ceir 25c-1b-2 / 26a-2: vjp_mlp backward plans device-resident - write-through-declare + two-output + DCE prune 11->9",
          "[ceir][tensor-pipeline][autodiff][dce]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    (void)ml::register_dialect(ctx); // ml.mlp — the Kit does not register it
    Module* const m = ctx.create_module();
    Block* const  b = mkmain(ctx, *m);

    // corpus (= the 25c-1 structural gate): out[8,4] = ml.mlp(x[8,4], W1[4,4], W2[4,4]){activation=relu}. loss = the mlp result (the
    // 25c-2 device gate seeds dOut=M non-uniform); x[8,4] so hidden = 8*4 == 32 (relu.ckir/relu_vjp.ckir's baked local_size).
    Value* const     xin     = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 4U)));
    Value* const     w1      = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     w2      = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value*           mops[3] = {xin, w1, w2};
    Operation* const mlp = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mops, 3U), 1U, tf(ctx, sh2(ctx, 8U, 4U)));
    ctx.set_attr(mlp, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mlp);

    gpu::VjpRegistry reg(&root);
    gpu::register_builtin_vjps(reg, ctx);
    Value*                wrt[2]   = {w1, w2};
    Value*                grads[2] = {nullptr, nullptr};
    const gpu::GradResult gr =
        gpu::build_gradient(ctx, *m, reg, mlp->result(0U), ConstSpan<Value*>(wrt, 2U), containers::Span<Value*>(grads, 2U), &root);
    REQUIRE(gr.error == gpu::GradError::None);
    REQUIRE(grads[0] != nullptr);
    REQUIRE(grads[1] != nullptr);

    // ⛔ NEGATIVE GATE (the reason the erase exists — a LOCKED header claim needs its own gate, the 24z baked-32 scar): the UNERASED
    //    module (forward ml.mlp composite present) is NOT plannable — plan_tensor_pipeline TYPED-REJECTS the composite as UnsupportedOp,
    //    pointing at the mlp op ITSELF. plan_tensor_pipeline is PURE (no module mutation), so this pre-plan is safe before the erase.
    const gpu::TensorPipelinePlan pre = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(pre.reject == gpu::PlanReject::UnsupportedOp);
    CHECK(pre.reject_op == mlp);

    // ⛔ FORWARD-LIVENESS CONTRACT (grad.hpp): vjp_mlp differentiates the COMPOSITE and RECOMPUTES the interior, so the backward never
    //    reads the ml.mlp result — loss is DEAD. A caller planning the backward ALONE erases the forward composite. Assert loss is dead
    //    BEFORE the erase (Operation::erase asserts on live result users — the guard fails LOUD instead of crashing).
    REQUIRE(!mlp->result(0U)->has_uses());
    mlp->erase();

    // ⛔ CEIR-26a DCE CONSUMER GATE: the vjp_mlp backward carries a DEAD branch — dx=gemm(dz1,W1ᵀ) + its W1ᵀ transpose feed
    //    NOTHING requested (wrt={W1,W2}, so the input-gradient dx is unwanted). But grads[0]=dW1 / grads[1]=dw2 are READBACK-BY-VALUE
    //    (the plan's Output marking), NOT SSA-consumed — a naive DCE would delete THEM too (Pure gemms, no uses) and cascade the
    //    whole backward. PIN them first: a func.return roots exactly the requested gradients (Terminator ⇒ NOT Pure ⇒ roots its
    //    operands; the planner SKIPS func.return — tensor_pipeline.cpp:211 — so the plan is unaffected). THEN DCE prunes precisely
    //    dx + W1ᵀ (dx first, then W1ᵀ once dx frees it — the fixpoint). See dce.hpp's LIVENESS CONTRACT + grad.hpp's caller note.
    Value* const     rets[2] = {grads[0], grads[1]};
    Operation* const ret     = func::create_return(ctx, ConstSpan<Value*>(rets, 2U));
    b->append(ret);
    REQUIRE(grads[0]->has_uses()); // the PIN is present (mirrors the DCE unit gate's t=0 has_uses fact) — its ABSENCE is the negative twin
    REQUIRE(grads[1]->has_uses());

    // a stage-kind histogram (Gemm / Transpose / VizDispatch) of a plan — computed pre- and post-DCE so the SHIFT is the identity proof.
    const auto histo = [&](const gpu::TensorPipelinePlan& p, u32& ng, u32& nt, u32& nd) {
        ng = 0U;
        nt = 0U;
        nd = 0U;
        for (usize i = 0; i < p.stages.size(); ++i)
        {
            const gpu::StageKind kk = p.stages[i].kind;
            ng += kk == gpu::StageKind::Gemm ? 1U : 0U;
            nt += kk == gpu::StageKind::Transpose ? 1U : 0U;
            nd += kk == gpu::StageKind::VizDispatch ? 1U : 0U;
        }
    };
    // PRE-DCE (the un-optimized backward — DCE has work to do): 11 stages. recompute z1=gemm + h1=@relu; backward 4 gemm
    // (dh1,dw2,dx,dW1) + 4 transpose (W2ᵀ,h1ᵀ,W1ᵀ,xᵀ) + 1 @relu_vjp. So gemm=5, transpose=4, VizDispatch=2.
    const gpu::TensorPipelinePlan pre_dce = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(pre_dce.reject == gpu::PlanReject::None);
    REQUIRE(pre_dce.stages.size() == 11U);
    u32 pg = 0U;
    u32 pt = 0U;
    u32 pd = 0U;
    histo(pre_dce, pg, pt, pd);
    CHECK(pg == 5U);
    CHECK(pt == 4U);
    CHECK(pd == 2U);
    // ⛔ b is the func BODY block (mkmain returns the func.func's body, one region deep) — count ops on b DIRECTLY, not a top-level
    //    module-body walk (that sees only the func.func container). erase() unlinks + tombstones (ir.hpp:370-371), so num_ops drops.
    const usize numops_before = b->num_ops();

    // run DCE. It prunes the dead branch — 2 ops to FIXPOINT: the dx gemm (dead, no user) frees, in round 2, the W1ᵀ transpose
    // (used only by the dx gemm — dying in round 2 is the chain-collapse/fixpoint proof; a single-round no-uses sweep would leave it).
    // The pinned gradients survive (their defining gemms are rooted by the func.return); the two resultless dispatches survive (NOT
    // Pure — the write-through effect). ⛔ 26c-2b: `resource.declare` is now NON-Pure (an Allocate effect — a declare NAMES a
    // distinct resource, so CSE must not merge two, [[feedback_cse_must_not_merge_external_source_ops_declare_is_alloc_not_pure]]),
    // so DCE no longer prunes the dx gemm's now-dead output declare (was the 3rd op) — it stays in the IR but is UNREACHABLE from
    // the outputs, so the planner ignores it (no stage, no buffer): the plan below is still 9 stages, device output unchanged.
    DiagnosticEngine diag(ctx, &root);
    const bool       dced = dce_run(ctx, *m, diag);
    CHECK(dced);
    CHECK_FALSE(diag.has_fatal());
    CHECK_FALSE(grads[0]->defining_op()->is_erased()); // the pin worked — dW1 (readback-by-Value) survived
    CHECK_FALSE(grads[1]->defining_op()->is_erased()); // dw2 survived
    const usize numops_after = b->num_ops();
    CAPTURE(numops_before, numops_after);
    CHECK(numops_before - numops_after == 2U); // dx gemm + the W1ᵀ transpose (2 plan stages; the dead output declare now stays, non-Pure)

    // POST-DCE: 9 stages (gemm 5→4 = dx gone; transpose 4→3 = W1ᵀ gone; VizDispatch 2→2 = @relu/@relu_vjp survive). The REST of this
    // gate (write-through-declare roles + two-output) now runs on the PRUNED plan — those invariants must survive DCE (dz1's only
    // surviving reader is dW1; dW1 stays terminal). plan_tensor_pipeline is PURE, so re-planning the DCE'd module is safe.
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == gpu::PlanReject::None);
    REQUIRE(plan.stages.size() == 9U);
    u32 n_gemm = 0U;
    u32 n_tr   = 0U;
    u32 n_disp = 0U;
    histo(plan, n_gemm, n_tr, n_disp);
    CHECK(n_gemm == 4U);
    CHECK(n_tr == 3U);
    CHECK(n_disp == 2U);

    // find the two dispatch stages by kernel symbol.
    crd::i32 relu_idx     = -1;
    crd::i32 relu_vjp_idx = -1;
    for (usize i = 0; i < plan.stages.size(); ++i)
    {
        if (plan.stages[i].kind != gpu::StageKind::VizDispatch) { continue; }
        const AttrValue kv = ctx.attr_value(plan.stages[i].op->attr(StringView("kernel")));
        if (kv.s == StringView("relu")) { relu_idx = static_cast<crd::i32>(i); }
        else if (kv.s == StringView("relu_vjp")) { relu_vjp_idx = static_cast<crd::i32>(i); }
    }
    REQUIRE(relu_idx >= 0);
    REQUIRE(relu_vjp_idx >= 0);

    // ⛔ WRITE-THROUGH-DECLARE (the two instances): each dispatch's w-bind (= bind[nbind-1], the trailing write) is a resource.declare
    //    the RESULTLESS dispatch WRITES (no SSA edge) — the plan must mark it Intermediate (NOT ExternalIn: the plan DOES see resultless
    //    dispatch writes, tensor_pipeline.cpp:455) and a LATER stage must READ it (submit order carries the non-SSA write; identity, not
    //    "n_out counted it"). A read bind is [0, nbind - n_out).
    const auto w_bind = [&](usize s) { return plan.stages[s].bind[plan.stages[s].nbind - 1U]; };
    // the MINIMUM stage index that READS `buf` (a read bind is [0, nbind - n_out)), or -1. The write-through invariant is UNIVERSAL —
    // EVERY reader after the writer — so checking the FIRST (min) reader > the writer proves it (a RAW reader before the dispatch fails).
    const auto first_reader = [&](crd::i32 buf) -> crd::i32 {
        for (usize s = 0; s < plan.stages.size(); ++s)
        {
            const gpu::PlanStage& st = plan.stages[s];
            for (u32 j = 0; j + st.n_out < st.nbind; ++j) { if (st.bind[j] == buf) { return static_cast<crd::i32>(s); } }
        }
        return -1;
    };
    const crd::i32 relu_out = w_bind(static_cast<usize>(relu_idx));      // h1 (the recomputed post-activation)
    const crd::i32 dz1_buf  = w_bind(static_cast<usize>(relu_vjp_idx)); // dz1 (the pre-activation adjoint)
    REQUIRE(relu_out >= 0);
    REQUIRE(dz1_buf >= 0);
    CHECK(plan.buffers[static_cast<usize>(relu_out)].role == gpu::BufferRole::Intermediate);
    CHECK(plan.buffers[static_cast<usize>(dz1_buf)].role == gpu::BufferRole::Intermediate);
    const crd::i32 relu_out_reader = first_reader(relu_out);
    const crd::i32 dz1_reader      = first_reader(dz1_buf);
    REQUIRE(relu_out_reader >= 0);
    REQUIRE(dz1_reader >= 0);
    CHECK(relu_out_reader > relu_idx);         // EVERY reader of h1 is after the recompute @relu (first reader is the min index)
    CHECK(dz1_reader > relu_vjp_idx);          // EVERY reader of dz1 is after the @relu_vjp
    CHECK(plan.stages[static_cast<usize>(relu_out_reader)].kind == gpu::StageKind::Transpose); // h1 → h1ᵀ transpose (feeds dw2)
    CHECK(plan.stages[static_cast<usize>(dz1_reader)].kind == gpu::StageKind::Gemm);           // dz1 → dW1 / dx gemms

    // ⛔ TWO-OUTPUT (the 25c-2 harness motivation, proven device-free HERE): grads[0] (dW1, the TERMINAL op) is the plan's SINGLE Output;
    //    grads[1] (dw2, emitted earlier + unread) is Intermediate = GpuOnly — so a by-Value readback of dw2 needs the 25c-2 run_quant_module
    //    extension: FORCE GpuToCpu for every NAMED out_val + accept a LIST of out_vals/dsts (the seed-handle/readback-by-Value contract).
    const auto buf_of = [&](const Value* v) -> crd::i32 {
        for (usize i = 0; i < plan.buffers.size(); ++i)
        {
            if (plan.buffers[i].value == v) { return static_cast<crd::i32>(i); }
        }
        return -1;
    };
    const crd::i32 g0 = buf_of(grads[0]);
    const crd::i32 g1 = buf_of(grads[1]);
    REQUIRE(g0 >= 0);
    REQUIRE(g1 >= 0);
    CHECK(plan.buffers[static_cast<usize>(g0)].role == gpu::BufferRole::Output);       // dW1 terminal = the readback target
    CHECK(plan.buffers[static_cast<usize>(g1)].role == gpu::BufferRole::Intermediate); // dw2 unread non-terminal = GpuOnly (needs (b))
    const gpu::PlanStage& last = plan.stages[plan.stages.size() - 1U];
    CHECK(last.bind[last.nbind - 1U] == g0); // grads[0] is the FINAL stage's trailing write
    u32 n_out_bufs = 0;
    for (usize i = 0; i < plan.buffers.size(); ++i) { n_out_bufs += plan.buffers[i].role == gpu::BufferRole::Output ? 1U : 0U; }
    CHECK(n_out_bufs == 1U);

    // the dLoss=dOut seed handle is an ExternalIn (the 25c-2 device gate uploads M=non-uniform into THIS buffer) — the STEP 1 (c) mirror.
    const crd::i32 seed_b = buf_of(gr.seed);
    REQUIRE(seed_b >= 0);
    CHECK(plan.buffers[static_cast<usize>(seed_b)].role == gpu::BufferRole::ExternalIn);
}

TEST_CASE("ceir 26a-2 NEGATIVE: an UNPINNED readback gradient IS deleted by DCE (the dce.hpp LIVENESS CONTRACT proof)",
          "[ceir][tensor-pipeline][autodiff][dce]")
{
    // ⛔ THE LOCKED-GUARD NEGATIVE GATE (feedback_locked_checklist_item_needs_a_gate...): the positive 26a-2 test proves pinned →
    //    survives, but that CANNOT distinguish "pinned and kept" from "kept anyway". The dce.hpp LIVENESS CONTRACT is a claim about
    //    the CATASTROPHE — an unpinned readback value (grads[] are readback-by-Value, NOT SSA-consumed) IS deleted. Prove it here.
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    (void)ml::register_dialect(ctx);
    Module* const m = ctx.create_module();
    Block* const  b = mkmain(ctx, *m);

    Value* const     xin     = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 4U)));
    Value* const     w1      = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     w2      = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value*           mops[3] = {xin, w1, w2};
    Operation* const mlp = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mops, 3U), 1U, tf(ctx, sh2(ctx, 8U, 4U)));
    ctx.set_attr(mlp, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mlp);

    gpu::VjpRegistry reg(&root);
    gpu::register_builtin_vjps(reg, ctx);
    Value*                wrt[2]   = {w1, w2};
    Value*                grads[2] = {nullptr, nullptr};
    const gpu::GradResult gr =
        gpu::build_gradient(ctx, *m, reg, mlp->result(0U), ConstSpan<Value*>(wrt, 2U), containers::Span<Value*>(grads, 2U), &root);
    REQUIRE(gr.error == gpu::GradError::None);
    REQUIRE(!mlp->result(0U)->has_uses());
    mlp->erase();

    // ⛔ NO func.return — grads[] are NOT SSA-consumed (t=0 fact the contract is about): the requested gradients dangle.
    REQUIRE_FALSE(grads[0]->has_uses());
    REQUIRE_FALSE(grads[1]->has_uses());

    DiagnosticEngine diag(ctx, &root);
    const bool       dced = dce_run(ctx, *m, diag);
    CHECK(dced);
    // THE CATASTROPHE: the requested gradients (dW1, dw2 — Pure gemms with no SSA uses) are DELETED. This is why a caller running any
    // pass MUST pin grads[] first (the positive 26a-2 test) — the contract is a lie the moment this stops firing.
    CHECK(grads[0]->defining_op()->is_erased());
    CHECK(grads[1]->defining_op()->is_erased());

    // SURVIVOR IDENTITY (not a category count): the two resultless compute.dispatch ops (@relu, @relu_vjp) are NOT Pure — DCE cannot
    // touch them even with every gradient they feed now dead. Both stay LINKED in b, identified by kernel symbol.
    bool have_relu     = false;
    bool have_relu_vjp = false;
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
    {
        const AttrId ka = op->attr(StringView("kernel"));
        if (!ka.valid()) { continue; }
        const AttrValue kv = ctx.attr_value(ka);
        if (kv.s == StringView("relu")) { have_relu = true; }
        else if (kv.s == StringView("relu_vjp")) { have_relu_vjp = true; }
    }
    CHECK(have_relu);
    CHECK(have_relu_vjp);
}

// CEIR-26b-2 — the canonicalize/fold pass on the REAL tensor dialect: tensor.reshape(tensor.reshape(x)) -> tensor.reshape(x),
// composing with DCE, PLAN-STAGE-NEUTRAL (reshape is a zero-copy alias, not a stage), verifier-clean, idempotent. Device-free.
// ⛔ CONSTRUCTED corpus (advisor): no natural reshape-of-reshape exists in the autodiff (no vjp_reshape; vjp_reduce's mk_reshape
// consumes dout, not a forward reshape), so the redundant double-reshape is built by hand.
TEST_CASE("ceir 26b-2: canonicalize folds tensor.reshape-of-reshape + composes with DCE, plan-stage-neutral", "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    REQUIRE(ctx.intern_op("tensor", "reshape") == OpId{fnv1a_ct("tensor.reshape")}); // pins the capture-free match id (id.hpp contract)
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // gemm[4,4] -> reshape[16] -> reshape[2,8] -> reduce(axis0)->[8]. T_out=[2,8] != T_in=[4,4] so the fold is NOT an identity.
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     bb = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     c  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     d  = gemm(ctx, b, a, bb, c, sh2(ctx, 4U, 4U));
    Operation* const r1 = tensor::build_reshape(ctx, d, tf(ctx, sh1(ctx, 16U)));
    b->append(r1);
    Operation* const r2 = tensor::build_reshape(ctx, r1->result(0U), tf(ctx, sh2(ctx, 2U, 8U)));
    b->append(r2);
    Operation* const rd = tensor::build_reduce(ctx, r2->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")),
                                               tf(ctx, sh1(ctx, 8U)));
    b->append(rd);
    // ⛔ PIN the terminal reduce output (the 26a-2 liveness contract): it is readback-by-Value (plan-Output by traversal), NOT
    //    SSA-live, so without a func.return the following DCE would prune the whole reduce->reshape chain. func.return is skipped
    //    by the planner (tensor_pipeline.cpp:211) so the plan is unaffected. See dce.hpp's LIVENESS CONTRACT.
    Value* const rets[1] = {rd->result(0U)};
    b->append(func::create_return(ctx, ConstSpan<Value*>(rets, 1U)));
    REQUIRE(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
    const TypeId t_out = r2->result(0U)->type(); // ⛔ capture the outer reshape's result type — the folded op MUST carry EXACTLY it
                                                 //    (identity-not-category: find_tensor_misuse alone passes ANY 16-elt shape)

    // RAW plan: 2 stages (Gemm, Reduce; the two reshapes are aliases), 2 Alias buffers.
    const gpu::TensorPipelinePlan raw = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(raw.reject == gpu::PlanReject::None);
    const usize stages_before = raw.stages.size();
    int         alias_before  = 0;
    for (usize i = 0; i < raw.buffers.size(); ++i)
    {
        if (raw.buffers[i].role == gpu::BufferRole::Alias) { ++alias_before; }
    }
    REQUIRE(alias_before == 2); // r1, r2

    // canonicalize: the fold fires (r2's reshape-of-r1-of-d -> a fresh reshape straight over d).
    DiagnosticEngine diag(ctx, &root);
    CHECK(canonicalize_run(ctx, *m, diag));
    CHECK_FALSE(diag.has_fatal());
    const Value* const     rd_in  = rd->operand(0U);
    const Operation* const folded = rd_in->defining_op();
    REQUIRE(folded != nullptr);
    CHECK(folded->kind() == OpId{fnv1a_ct("tensor.reshape")});
    CHECK(folded != r2);
    CHECK(folded->parent_block() == b);            // ⛔ the new op was LINKED (not floating)
    CHECK(folded->operand(0U) == d);               // one hop: reshape(gemm output), not reshape(reshape(gemm output))
    CHECK(folded->result(0U)->type() == t_out);    // ⛔ IDENTITY-not-category: the fold preserved T_out (a wrong T_out passes find_tensor_misuse but breaks the downstream shape)
    CHECK(rd->operand(0U)->type() == t_out);       // ⛔ the reduce still reads a [2,8] alias
    CHECK_FALSE(r2->result(0U)->has_uses());        // r2 is dead (RAUW moved the reduce to the folded op)

    // compose with DCE: both dead reshapes reclaimed; the folded reshape (used by reduce) kept.
    CHECK(dce_run(ctx, *m, diag));
    CHECK(r1->is_erased());
    CHECK(r2->is_erased());
    CHECK_FALSE(folded->is_erased());
    // Fix 2 (advisor): the folded module still verifies (a fold that broke shapes_reshape would be a finding, not accepted).
    REQUIRE(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);

    // POST plan: STAGE COUNT UNCHANGED (reshape is a zero-copy alias, never a stage -> the 0-shift the row promised); one fewer Alias.
    const gpu::TensorPipelinePlan opt = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(opt.reject == gpu::PlanReject::None);
    CHECK(opt.stages.size() == stages_before); // 0 stage shift
    int alias_after = 0;
    for (usize i = 0; i < opt.buffers.size(); ++i)
    {
        if (opt.buffers[i].role == gpu::BufferRole::Alias) { ++alias_after; }
    }
    CHECK(alias_after == alias_before - 1); // 2 -> 1

    // idempotence on the converged module.
    CHECK_FALSE(canonicalize_run(ctx, *m, diag));
}

// ⛔ 26b-3 (SUPERSEDES the 26b-2a identity-EXCLUSION negative — struck in place): with identity_reshape present the exclusion is
// REMOVED, and the pair {reshape-of-reshape, identity_reshape} FULLY COLLAPSES a net-identity chain (T->U->T) to x — the fold
// emits reshape(x:T):T, then identity-elim RAUWs it to x. The reduce ends up reading the gemm output directly.
TEST_CASE("ceir 26b-3: canonicalize fully collapses a net-identity reshape chain (T->U->T) to x", "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    REQUIRE(ctx.intern_op("tensor", "reshape") == OpId{fnv1a_ct("tensor.reshape")}); // pins the capture-free match id (id.hpp contract)
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // gemm[4,4] -> reshape[16] -> reshape[4,4] -> reduce. NET-IDENTITY ([4,4]->[16]->[4,4]) -> collapses to reduce(gemm).
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     bb = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     c  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     d  = gemm(ctx, b, a, bb, c, sh2(ctx, 4U, 4U));
    Operation* const r1 = tensor::build_reshape(ctx, d, tf(ctx, sh1(ctx, 16U)));
    b->append(r1);
    Operation* const r2 = tensor::build_reshape(ctx, r1->result(0U), tf(ctx, sh2(ctx, 4U, 4U)));
    b->append(r2);
    Operation* const rd = tensor::build_reduce(ctx, r2->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")),
                                               tf(ctx, sh1(ctx, 4U)));
    b->append(rd);
    Value* const rets[1] = {rd->result(0U)};
    b->append(func::create_return(ctx, ConstSpan<Value*>(rets, 1U))); // pin the reduce (26a-2 liveness contract) for the later DCE
    REQUIRE(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);

    DiagnosticEngine diag(ctx, &root);
    CHECK(canonicalize_run(ctx, *m, diag)); // collapses over 2 internal rounds (fold -> reshape(x:T):T -> identity-elim -> x)
    CHECK_FALSE(diag.has_fatal());
    // ⛔ PRE-DCE FIXPOINT (advisor): a 2nd run changes nothing WITH the dead reshapes still linked -> catches a re-match bug (a
    //    pattern re-firing on a folded/dead op). Stronger than idempotence-after-DCE, which would hide a re-match on a tombstone.
    CHECK_FALSE(canonicalize_run(ctx, *m, diag));
    CHECK(rd->operand(0U) == d); // the reduce reads the GEMM output DIRECTLY (both reshapes fully collapsed away)
    // compose with DCE: r1, r2, and the intermediate identity reshape are all reclaimed.
    CHECK(dce_run(ctx, *m, diag));
    CHECK(r1->is_erased());
    CHECK(r2->is_erased());
    CHECK(rd->operand(0U) == d); // still reads the gemm after DCE
    REQUIRE(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
}

// ⛔ 26b-3: a DIRECTLY-AUTHORED identity reshape reshape(x:T):T -> x (RAUW-to-operand). Proves identity_reshape fires STANDALONE
// (not only as the reshape-of-reshape fold's cleanup) + that a same-type reshape IS authorable (find_tensor_misuse None).
TEST_CASE("ceir 26b-3: canonicalize eliminates a directly-authored identity reshape (RAUW-to-operand)", "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    REQUIRE(ctx.intern_op("tensor", "reshape") == OpId{fnv1a_ct("tensor.reshape")});
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // gemm[4,4] -> reshape([4,4]->[4,4]) [an identity, no rank change] -> reduce.
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     bb = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     c  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     d  = gemm(ctx, b, a, bb, c, sh2(ctx, 4U, 4U));
    Operation* const id = tensor::build_reshape(ctx, d, tf(ctx, sh2(ctx, 4U, 4U))); // T -> T identity
    b->append(id);
    Operation* const rd = tensor::build_reduce(ctx, id->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")),
                                               tf(ctx, sh1(ctx, 4U)));
    b->append(rd);
    Value* const rets[1] = {rd->result(0U)};
    b->append(func::create_return(ctx, ConstSpan<Value*>(rets, 1U)));
    REQUIRE(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None); // ⛔ an identity reshape IS authorable

    // PLAN leg (advisor): RAW has ONE Alias (the identity reshape aliasing the gemm output).
    const gpu::TensorPipelinePlan raw = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(raw.reject == gpu::PlanReject::None);
    int raw_alias = 0;
    for (usize i = 0; i < raw.buffers.size(); ++i) { if (raw.buffers[i].role == gpu::BufferRole::Alias) { ++raw_alias; } }
    REQUIRE(raw_alias == 1);

    DiagnosticEngine diag(ctx, &root);
    CHECK(canonicalize_run(ctx, *m, diag));       // identity-elim fires (the reshape-of-reshape fold does NOT — id's operand is the gemm)
    CHECK_FALSE(diag.has_fatal());
    CHECK(rd->operand(0U) == d);                  // reduce reads the gemm output directly (RAUW-to-operand)
    CHECK_FALSE(canonicalize_run(ctx, *m, diag)); // fixpoint
    CHECK(dce_run(ctx, *m, diag));
    CHECK(id->is_erased());
    CHECK(rd->operand(0U) == d);
    // ⛔ PLAN topology 26b-2 NEVER produced: the reduce reads the gemm output with NO Alias between them (Alias 1->0). The plan
    //    SEES the difference between RAUW-to-operand and RAUW-to-a-same-typed-alias; the IR check above cannot. (No DEVICE leg:
    //    identity-elim changes no dispatch and no buffer — a device run would compare a buffer to itself; 26e fusion is the first
    //    pattern whose device leg is load-bearing.)
    REQUIRE(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
    const gpu::TensorPipelinePlan opt = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(opt.reject == gpu::PlanReject::None);
    CHECK(opt.stages.size() == 2U); // Gemm + Reduce (no alias; the reshape is gone)
    int opt_alias = 0;
    for (usize i = 0; i < opt.buffers.size(); ++i) { if (opt.buffers[i].role == gpu::BufferRole::Alias) { ++opt_alias; } }
    CHECK(opt_alias == 0); // 1 -> 0
}

// ⛔ 26c-2a: the CSE PLAN leg (device-free) — CSE collapsing a DUPLICATE gemm REMOVES a DISPATCHED stage, a plan topology 26b's
// alias-only folds NEVER produced (there stage count was invariant). Two IDENTICAL gemm(A,B,C):[s,s] feed two DISTINCT reduces
// (sum axis-0 vs max axis-0 — different attr ⇒ NOT CSE-equal ⇒ both KEPT): RAW plans 2 Gemm + 2 Reduce = 4 stages; after cse_run
// the later gemm is erased (its consumer RAUW'd onto the survivor) ⇒ 1 Gemm + 2 Reduce = 3 stages. Proves the CSE-observable
// stage drop AND that the different-attr reduces are correctly kept (the structural-attr check at the plan boundary). The device
// bit-exact leg (a removed DISPATCH, the 26a witness at full teeth) is 26c-2b/2c (Vk/DX12).
TEST_CASE("ceir 26c-2a: CSE collapses a duplicate gemm and the plan drops one Gemm stage (4->3)", "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // %a,%b,%c = decl[s,s] ; %d1 = gemm(a,b,c) ; %d2 = gemm(a,b,c) [IDENTICAL] ; %r1 = reduce(d1,sum) ; %r2 = reduce(d2,max).
    const u32        s  = 4U;
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, s, s)));
    Value* const     bb = decl(ctx, k, b, tf(ctx, sh2(ctx, s, s)));
    Value* const     c  = decl(ctx, k, b, tf(ctx, sh2(ctx, s, s)));
    Operation* const g1 = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                             ctx.attr_bool(false), tf(ctx, sh2(ctx, s, s)));
    b->append(g1);
    Operation* const g2 = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                             ctx.attr_bool(false), tf(ctx, sh2(ctx, s, s))); // structurally identical to g1
    b->append(g2);
    Value* const     d1 = g1->result(0U);
    Value* const     d2 = g2->result(0U);
    Operation* const r1 = tensor::build_reduce(ctx, d1, ctx.attr_int(0), ctx.attr_string(StringView("sum")), tf(ctx, sh1(ctx, s)));
    b->append(r1);
    Operation* const r2 = tensor::build_reduce(ctx, d2, ctx.attr_int(0), ctx.attr_string(StringView("max")), tf(ctx, sh1(ctx, s)));
    b->append(r2);
    Value* const rets[2] = {r1->result(0U), r2->result(0U)}; // pin BOTH terminal reduces (26a-2 liveness contract)
    b->append(func::create_return(ctx, ConstSpan<Value*>(rets, 2U)));
    REQUIRE(linalg::find_linalg_misuse(ctx, *m).kind == linalg::LinalgMisuseKind::None);
    REQUIRE(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);

    // RAW plan: two independent gemm->reduce chains ⇒ 2 Gemm + 2 Reduce.
    const gpu::TensorPipelinePlan raw = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(raw.reject == gpu::PlanReject::None);
    int raw_gemm   = 0;
    int raw_reduce = 0;
    for (usize i = 0; i < raw.stages.size(); ++i)
    {
        raw_gemm += raw.stages[i].kind == gpu::StageKind::Gemm ? 1 : 0;
        raw_reduce += raw.stages[i].kind == gpu::StageKind::Reduce ? 1 : 0;
    }
    REQUIRE(raw.stages.size() == 4U);
    REQUIRE(raw_gemm == 2);
    REQUIRE(raw_reduce == 2);
    // ⛔ #2 (advisor, identity-not-category): RAW has TWO writers — the two Gemm stages write DIFFERENT buffers (bind[2]=output;
    //    beta=0 drops C so nbind=3). If they aliased, the planner had already deduped and CSE would be proving nothing.
    i32 raw_g_out_a = -1;
    i32 raw_g_out_b = -1;
    for (usize i = 0; i < raw.stages.size(); ++i)
    {
        if (raw.stages[i].kind != gpu::StageKind::Gemm) { continue; }
        if (raw_g_out_a < 0) { raw_g_out_a = raw.stages[i].bind[2]; }
        else { raw_g_out_b = raw.stages[i].bind[2]; }
    }
    REQUIRE(raw_g_out_a >= 0);
    REQUIRE(raw_g_out_b >= 0);
    CHECK(raw_g_out_a != raw_g_out_b); // two distinct Intermediate outputs — the planner did NOT dedupe; CSE has real work
    // ⛔ FINDING (source=scoreboard, empirical): the planner marks exactly ONE terminal as Output (the readback target); other
    //    terminals are Intermediate/GpuOnly (the 26a-2 g0=dW1 Output / g1=dw2 Intermediate precedent) — so n_out is a planner
    //    role choice, NOT "how many results". The role-agnostic "two distinct results" fact is that the two Reduce stages write
    //    DIFFERENT output buffers (bind[1]); the 26c-2b device leg reads the Output-role scalar directly + the other by-value.
    i32 raw_r_out_a = -1;
    i32 raw_r_out_b = -1;
    for (usize i = 0; i < raw.stages.size(); ++i)
    {
        if (raw.stages[i].kind != gpu::StageKind::Reduce) { continue; }
        if (raw_r_out_a < 0) { raw_r_out_a = raw.stages[i].bind[1]; }
        else { raw_r_out_b = raw.stages[i].bind[1]; }
    }
    REQUIRE(raw_r_out_a >= 0);
    REQUIRE(raw_r_out_b >= 0);
    CHECK(raw_r_out_a != raw_r_out_b); // r1, r2 write two distinct output buffers

    DiagnosticEngine diag(ctx, &root);
    CHECK(cse_run(ctx, *m, diag));    // d2 -> d1 (the identical gemms merge)
    CHECK(g2->is_erased());           // the later twin dies ...
    CHECK_FALSE(g1->is_erased());     // ... the earlier survives
    CHECK(r1->operand(0U) == d1);
    CHECK(r2->operand(0U) == d1);     // ⛔ r2 rewired off the erased %d2 onto the SURVIVING gemm output %d1
    CHECK_FALSE(cse_run(ctx, *m, diag)); // fixpoint — the sum/max reduces differ by attr, never merge

    // OPT plan: ONE gemm feeds BOTH reduces ⇒ 1 Gemm + 2 Reduce = 3 stages (the removed DISPATCH the device leg will prove bit-exact).
    REQUIRE(linalg::find_linalg_misuse(ctx, *m).kind == linalg::LinalgMisuseKind::None);
    const gpu::TensorPipelinePlan opt = gpu::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(opt.reject == gpu::PlanReject::None);
    int opt_gemm   = 0;
    int opt_reduce = 0;
    for (usize i = 0; i < opt.stages.size(); ++i)
    {
        opt_gemm += opt.stages[i].kind == gpu::StageKind::Gemm ? 1 : 0;
        opt_reduce += opt.stages[i].kind == gpu::StageKind::Reduce ? 1 : 0;
    }
    CHECK(opt.stages.size() == 3U); // 4 -> 3
    CHECK(opt_gemm == 1);           // the CSE-observable dispatch drop
    CHECK(opt_reduce == 2);         // both distinct reduces KEPT (the plan twin of the pass-level sum/max attr negative)
    // ⛔ #1 (advisor): the plan-level WIRING — the ONE surviving Gemm output feeds BOTH Reduce stages (the 22c-1
    //    fft_in_re-aliases-gemm precedent). Stage-count alone can't tell "3 stages correctly wired" from "3 stages, a Reduce
    //    reading a wrong/fresh buffer"; this is the topology the count can't see.
    i32 opt_gemm_out = -1;
    for (usize i = 0; i < opt.stages.size(); ++i)
    {
        if (opt.stages[i].kind == gpu::StageKind::Gemm) { opt_gemm_out = opt.stages[i].bind[2]; }
    }
    REQUIRE(opt_gemm_out >= 0);
    i32 opt_r_out_a = -1;
    i32 opt_r_out_b = -1;
    for (usize i = 0; i < opt.stages.size(); ++i)
    {
        if (opt.stages[i].kind != gpu::StageKind::Reduce) { continue; }
        CHECK(opt.stages[i].bind[0] == opt_gemm_out); // ⛔ both reduces read the ONE surviving gemm output %d1
        if (opt_r_out_a < 0) { opt_r_out_a = opt.stages[i].bind[1]; }
        else { opt_r_out_b = opt.stages[i].bind[1]; }
    }
    REQUIRE(opt_r_out_a >= 0);
    REQUIRE(opt_r_out_b >= 0);
    CHECK(opt_r_out_a != opt_r_out_b); // both reduces KEPT ⇒ still two DISTINCT result buffers (the reduces were NOT merged)
}

// CEIR-26f-2 — the SINGLE-PASS free-list buffer-aliasing (share_intermediate_storage). Device-free (the plan structure). Proves:
// (a) disjoint-lifetime Intermediates SHARE storage (a chain v0→v1→v2→out: v2's lifetime is disjoint from v0's, so v2 TENANTS v0 —
// alias_of = v0, v0's bytes GROW to max); (b) last_read = MAX over ALL consumers (a twice-read buffer is NOT freed early); (c)
// func.return operands are PINNED (a returned Intermediate is NEVER a tenant — the 25c-2 dw2 named-out trap).
TEST_CASE("ceir 26f-2: disjoint-lifetime Intermediates share storage (tenant+grow) last_read is max-over-consumers func.return pinned",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const auto                    buf_of = [](const gpu::TensorPipelinePlan& p, const Value* v) -> crd::i32 {
        for (usize i = 0; i < p.buffers.size(); ++i) { if (p.buffers[i].value == v) { return static_cast<crd::i32>(i); } }
        return -1;
    };
    const auto n_landlords = [](const gpu::TensorPipelinePlan& p) { // Intermediates that OWN their storage (alias_of < 0)
        crd::u32 n = 0;
        for (usize i = 0; i < p.buffers.size(); ++i)
        {
            if (p.buffers[i].role == gpu::BufferRole::Intermediate && p.buffers[i].alias_of < 0) { ++n; }
        }
        return n;
    };

    // ── (a) POSITIVE: v0[2,2](16B) → v1[4,4] → v2[4,4] → out[4,4] (a 4-dispatch chain). v0 dies after s1, v2 born s2 ⇒ v2 TENANTS
    //        v0; v2(64B) does NOT fit v0(16B) ⇒ v0 GROWS to 64B (the max). v1 is a landlord (born s1, v0 not yet free). ──
    {
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  g = konst(ctx, k, b, 1);
        Value* const  in = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value* const  v0 = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 2U))); // 16 B (small — forces the grow branch)
        Value*        d0[2] = {in, v0};
        (void)mk_dispatch(ctx, k, b, g, g, g, d0, 2U, "viz_magnitude", "r,w"); // s0: v0 produced
        Value* const v1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));  // 64 B
        Value*       d1[2] = {v0, v1};
        (void)mk_dispatch(ctx, k, b, g, g, g, d1, 2U, "viz_magnitude", "r,w"); // s1: v1 produced, v0 last-read
        Value* const v2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));  // 64 B
        Value*       d2[2] = {v1, v2};
        (void)mk_dispatch(ctx, k, b, g, g, g, d2, 2U, "viz_magnitude", "r,w"); // s2: v2 produced, v1 last-read
        Value* const out = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       d3[2] = {v2, out};
        (void)mk_dispatch(ctx, k, b, g, g, g, d3, 2U, "viz_magnitude", "r,w"); // s3: out produced (terminal), v2 last-read

        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root); // share_intermediate_storage default ON
        REQUIRE(plan.reject == gpu::PlanReject::None);
        const crd::i32 bv0 = buf_of(plan, v0);
        const crd::i32 bv1 = buf_of(plan, v1);
        const crd::i32 bv2 = buf_of(plan, v2);
        REQUIRE(bv0 >= 0);
        REQUIRE(bv1 >= 0);
        REQUIRE(bv2 >= 0);
        CHECK(plan.buffers[static_cast<usize>(bv2)].alias_of == bv0); // ⭐ v2 TENANTS v0 (disjoint lifetimes)
        CHECK(plan.buffers[static_cast<usize>(bv2)].alias_of < bv2);  // alias_of points at an EARLIER (realized) landlord
        CHECK(plan.buffers[static_cast<usize>(bv0)].alias_of < 0);    // v0 is the ROOT landlord
        CHECK(plan.buffers[static_cast<usize>(bv1)].alias_of < 0);    // v1 is a landlord (v0 not yet free at v1's birth)
        CHECK(plan.buffers[static_cast<usize>(bv0)].bytes == 64U);    // ⭐ v0 GREW 16→64 (max of landlord + tenant)
        CHECK(plan.buffers[static_cast<usize>(bv0)].role == gpu::BufferRole::Intermediate);
        CHECK(n_landlords(plan) == 2U); // v0, v1 — the 3 Intermediates need only 2 physical buffers
    }

    // ── (b) NEGATIVE last_read = MAX: z is read at s1 AND s3 (two consumers). A buffer `c` born at s2 (after z's FIRST read) must
    //        NOT tenant z (z is still live — read again at s3). If last_read used first_use(s1), z would free early ⇒ c tenants z ⇒
    //        CLOBBER. ──
    {
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  g = konst(ctx, k, b, 1);
        Value* const  in = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value* const  z  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*        dz[2] = {in, z};
        (void)mk_dispatch(ctx, k, b, g, g, g, dz, 2U, "viz_magnitude", "r,w"); // s0: z produced
        Value* const a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       da[2] = {z, a};
        (void)mk_dispatch(ctx, k, b, g, g, g, da, 2U, "viz_magnitude", "r,w"); // s1: a produced, z READ #1
        Value* const c  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       dc[2] = {a, c};
        (void)mk_dispatch(ctx, k, b, g, g, g, dc, 2U, "viz_magnitude", "r,w"); // s2: c produced (born AFTER z's read #1)
        Value* const out = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       do2[3] = {z, c, out};
        (void)mk_dispatch(ctx, k, b, g, g, g, do2, 3U, "viz_magnitude", "r,r,w"); // s3: out produced, z READ #2 + c read

        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        const crd::i32 bz = buf_of(plan, z);
        const crd::i32 bc = buf_of(plan, c);
        REQUIRE(bz >= 0);
        REQUIRE(bc >= 0);
        CHECK(plan.buffers[static_cast<usize>(bc)].alias_of != bz); // ⭐ c does NOT tenant z (z still live at s3 — last_read is MAX)
        CHECK(plan.buffers[static_cast<usize>(bz)].alias_of < 0);   // z is never a tenant (long-lived)
    }

    // ── (c) NEGATIVE func.return-PINNED: dw2 (an Intermediate born s2, with z free before it) is func.return'd (a readback target)
    //        ⇒ it must NOT tenant z. Absent pinning, dw2 WOULD tenant z (z free_at=1 < 2, fits). ──
    {
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  g = konst(ctx, k, b, 1);
        Value* const  in = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value* const  z  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*        dz[2] = {in, z};
        (void)mk_dispatch(ctx, k, b, g, g, g, dz, 2U, "viz_magnitude", "r,w"); // s0: z produced
        Value* const a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       da[2] = {z, a};
        (void)mk_dispatch(ctx, k, b, g, g, g, da, 2U, "viz_magnitude", "r,w"); // s1: a produced, z last-read (free after s1)
        Value* const dw2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       dd[2] = {a, dw2};
        (void)mk_dispatch(ctx, k, b, g, g, g, dd, 2U, "viz_magnitude", "r,w"); // s2: dw2 produced (z is free ⇒ would tenant z)
        Value* const out = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       dout[2] = {dw2, out};
        (void)mk_dispatch(ctx, k, b, g, g, g, dout, 2U, "viz_magnitude", "r,w"); // s3: out produced (terminal), dw2 last-read
        Value* const rets[2] = {dw2, out};
        b->append(func::create_return(ctx, ConstSpan<Value*>(rets, 2U))); // dw2 + out BOTH returned ⇒ dw2 PINNED (readback target)

        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        const crd::i32 bdw2 = buf_of(plan, dw2);
        REQUIRE(bdw2 >= 0);
        CHECK(plan.buffers[static_cast<usize>(bdw2)].alias_of < 0); // ⭐ func.return-PINNED ⇒ dw2 keeps its OWN buffer (not a tenant)
    }
}

// CEIR-26f-2b — the free-list's TWO remaining edges (advisor). Device-free. Proves:
// (d) the n_out==0 GUARD: a stage with no trailing write ("r,r") emits NO barrier (the per-stage ShaderWrite→ShaderRead barrier
//     rides the trailing outputs), so a buffer whose LAST reader is such a stage must NEVER lend its storage (a WAR that nothing
//     orders). The pass pins last_read=never_free ⇒ a later buffer does NOT tenant it. Latent (no corpus has an n_out==0 non-final
//     stage today) but the free-list would happily alias across one. (e) the NAMED consumer: a real ml.attention plans to exactly
//     ONE tenant, and it is the softmax write (probs) tenanting Kᵀ — the pass fires on the shipped op, not only a synthetic chain.
TEST_CASE("ceir 26f-2b: n_out==0 reader never lends (no-barrier WAR) and attention plans to exactly one tenant (probs tenants Kt)",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const auto                    buf_of = [](const gpu::TensorPipelinePlan& p, const Value* v) -> crd::i32 {
        for (usize i = 0; i < p.buffers.size(); ++i) { if (p.buffers[i].value == v) { return static_cast<crd::i32>(i); } }
        return -1;
    };

    // ── (d) NEGATIVE n_out==0 GUARD: s0 produces `held`; s1 is an all-read "r,r" dispatch (n_out==0) that LAST-reads `held`;
    //        s2 produces `later` (born after held's read, same 64 B ⇒ fits); s3 terminal. Absent the guard, held.free_at=1 <
    //        produce[later]=2 ⇒ later would TENANT held with NO barrier between held's read (s1) and later's write (s2) — a WAR
    //        clobber. The guard pins last_read[held]=never_free ⇒ held never lends ⇒ later keeps its OWN buffer. ──
    {
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  g = konst(ctx, k, b, 1);
        Value* const  in = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value* const  held = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*        d0[2] = {in, held};
        (void)mk_dispatch(ctx, k, b, g, g, g, d0, 2U, "viz_magnitude", "r,w"); // s0: held produced
        Value*        d1[2] = {held, in};
        (void)mk_dispatch(ctx, k, b, g, g, g, d1, 2U, "viz_magnitude", "r,r"); // s1: n_out==0 — held LAST-read, NO barrier emitted
        Value* const  later = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*        d2[2] = {in, later};
        (void)mk_dispatch(ctx, k, b, g, g, g, d2, 2U, "viz_magnitude", "r,w"); // s2: later produced (would tenant held absent guard)
        Value* const  out = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*        d3[2] = {later, out};
        (void)mk_dispatch(ctx, k, b, g, g, g, d3, 2U, "viz_magnitude", "r,w"); // s3: out produced (terminal)

        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == gpu::PlanReject::None);
        // ⭐ IDENTITY (advisor): prove the hole-shaped stage EXISTS in the plan (else `later` would have nothing to tenant for a
        //    trivial reason — the planner dropped s1 — and the gate would pass VACUOUSLY, testing category not identity).
        REQUIRE(plan.stages.size() == 4U);
        CHECK(plan.stages[1].n_out == 0U); // s1 is the all-read (n_out==0) stage the guard must key on
        const crd::i32 bheld  = buf_of(plan, held);
        const crd::i32 blater = buf_of(plan, later);
        REQUIRE(bheld >= 0);
        REQUIRE(blater >= 0);
        CHECK(plan.buffers[static_cast<usize>(blater)].alias_of < 0); // ⭐ later does NOT tenant held (its last reader emits no barrier)
        CHECK(plan.buffers[static_cast<usize>(bheld)].alias_of < 0);  // held stays a ROOT (never a tenant)
    }

    // ── (e) POSITIVE the NAMED consumer: expand a real ml.attention (Q[2,4]·K[3,4]·V[3,2] → out[2,2]) and plan it. The chain is
    //        transpose(K)→Kt[4,3] (s0), gemm1→scores[2,3] (s1), softmax→probs[2,3] (s2), gemm2→out[2,2] (s3). Kt frees after s1
    //        (48 B) and probs is born at s2 (24 B ⇒ fits) ⇒ probs TENANTS Kt — EXACTLY ONE tenant in the whole plan, and it is the
    //        softmax write. (scores can't tenant Kt: born s1, Kt not free until after s1. out never reads ⇒ neither tenant nor
    //        landlord. the gemm C operands + softmax scale are ExternalIn ⇒ not shareable.) ──
    {
        (void)ml::register_dialect(ctx); // ml.attention — the Kit does not register it
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  q  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value* const  ky = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
        Value* const  v  = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 2U)));
        b->append(ml::build_attention(ctx, q, ky, v, tf(ctx, sh2(ctx, 2U, 2U))));

        const gpu::MlExpandResult er = gpu::expand_ml_ops(ctx, *m);
        REQUIRE(er.error == gpu::MlExpandError::None);
        REQUIRE(er.expanded == 1U);

        // Kt = the tensor.transpose result; probs = the softmax compute.dispatch's TRAILING (write) operand {scores, scale, probs}.
        Value* kt    = nullptr;
        Value* probs = nullptr;
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const StringView nm = ctx.op_name(op->kind());
            if (nm == StringView("tensor.transpose")) { kt = op->result(0U); }
            else if (nm == StringView("compute.dispatch")) { probs = op->operand(op->num_operands() - 1U); }
        }
        REQUIRE(kt != nullptr);
        REQUIRE(probs != nullptr);

        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == gpu::PlanReject::None);

        crd::u32 n_tenant = 0U;
        for (usize i = 0; i < plan.buffers.size(); ++i) { if (plan.buffers[i].alias_of >= 0) { ++n_tenant; } }
        CHECK(n_tenant == 1U); // ⭐ EXACTLY one tenant in the whole attention plan

        const crd::i32 bkt = buf_of(plan, kt);
        const crd::i32 bpr = buf_of(plan, probs);
        REQUIRE(bkt >= 0);
        REQUIRE(bpr >= 0);
        CHECK(plan.buffers[static_cast<usize>(bpr)].alias_of == bkt); // ⭐ the one tenant is the softmax write (probs) tenanting Kt
        CHECK(plan.buffers[static_cast<usize>(bkt)].alias_of < 0);    // Kt is the ROOT landlord
    }
}

// CEIR-26f-3c — the func.return-pinning POSITIVE CONTROL (advisor). 26f-2(c) proved a func.return'd Intermediate dw2 does NOT tenant
// a freed landlord z (the NEGATIVE). Alone that is a CATEGORY gate — "dw2 keeps its own buffer" could hold for ANY reason (e.g. no
// landlord free). This adds the CONTROL: the SAME chain (z→a→dw2→out) built WITHOUT the func.return DOES tenant z (dw2.alias_of==bz).
// The two arms differ in NOTHING but the return ⇒ PINNING is the discriminator, not the lifetimes — an IDENTITY gate.
TEST_CASE("ceir 26f-3c: func.return-pinning is the DISCRIMINATOR -- the same chain tenants z WITHOUT the return, keeps its own WITH it",
          "[ceir][tensor-pipeline]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const auto                    buf_of = [](const gpu::TensorPipelinePlan& p, const Value* v) -> crd::i32 {
        for (usize i = 0; i < p.buffers.size(); ++i) { if (p.buffers[i].value == v) { return static_cast<crd::i32>(i); } }
        return -1;
    };

    // Build 26f-2(c)'s EXACT chain into a fresh module; append func.return{dw2,out} IFF `with_return`. dw2/z returned by out-param.
    const auto build_chain = [&](bool with_return, Value*& dw2_out, Value*& z_out) -> Module* {
        Module* const m  = ctx.create_module();
        Block* const  b  = mkmain(ctx, *m);
        Value* const  g  = konst(ctx, k, b, 1);
        Value* const  in = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value* const  z  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*        dz[2] = {in, z};
        (void)mk_dispatch(ctx, k, b, g, g, g, dz, 2U, "viz_magnitude", "r,w"); // s0: z produced
        Value* const a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       da[2] = {z, a};
        (void)mk_dispatch(ctx, k, b, g, g, g, da, 2U, "viz_magnitude", "r,w"); // s1: a produced, z last-read (free after s1)
        Value* const dw2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       dd[2] = {a, dw2};
        (void)mk_dispatch(ctx, k, b, g, g, g, dd, 2U, "viz_magnitude", "r,w"); // s2: dw2 produced (z free ⇒ WOULD tenant z)
        Value* const out = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
        Value*       dout[2] = {dw2, out};
        (void)mk_dispatch(ctx, k, b, g, g, g, dout, 2U, "viz_magnitude", "r,w"); // s3: out produced (terminal), dw2 last-read
        if (with_return)
        {
            Value* const rets[2] = {dw2, out};
            b->append(func::create_return(ctx, ConstSpan<Value*>(rets, 2U))); // dw2 + out returned ⇒ dw2 PINNED
        }
        dw2_out = dw2;
        z_out   = z;
        return m;
    };

    // ── WITH the func.return: dw2 PINNED ⇒ keeps its OWN buffer (26f-2(c)'s negative, the control's baseline). ──
    Value*        dw2_w = nullptr;
    Value*        z_w   = nullptr;
    Module* const mw    = build_chain(true, dw2_w, z_w);
    (void)z_w;
    const gpu::TensorPipelinePlan plan_w = gpu::plan_tensor_pipeline(ctx, *mw, &root);
    REQUIRE(plan_w.reject == gpu::PlanReject::None);
    const crd::i32 bdw2_w = buf_of(plan_w, dw2_w);
    REQUIRE(bdw2_w >= 0);
    CHECK(plan_w.buffers[static_cast<usize>(bdw2_w)].alias_of < 0); // ⭐ PINNED ⇒ dw2 keeps its OWN buffer

    // ── WITHOUT the func.return: dw2 is NOT pinned ⇒ it TENANTS z (z free_at=1 < produce[dw2]=2, fits). The ONLY change from the
    //    arm above is the missing return ⇒ PINNING is what refused the tenancy, NOT the lifetimes (the identity the control adds). ──
    Value*        dw2_n = nullptr;
    Value*        z_n   = nullptr;
    Module* const mn    = build_chain(false, dw2_n, z_n);
    const gpu::TensorPipelinePlan plan_n = gpu::plan_tensor_pipeline(ctx, *mn, &root);
    REQUIRE(plan_n.reject == gpu::PlanReject::None);
    const crd::i32 bdw2_n = buf_of(plan_n, dw2_n);
    const crd::i32 bz_n   = buf_of(plan_n, z_n);
    REQUIRE(bdw2_n >= 0);
    REQUIRE(bz_n >= 0);
    CHECK(plan_n.buffers[static_cast<usize>(bdw2_n)].alias_of == bz_n); // ⭐ NOT pinned ⇒ dw2 TENANTS z (the discriminating positive)
}
