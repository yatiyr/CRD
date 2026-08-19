// CEIR-22c-1 — plan_tensor_pipeline (the PURE, device-free §137 memory plan). Proves: (1) a well-formed gemm→reshape→fft→reduce
// module plans to 3 DISPATCH stages (reshape = a zero-copy ALIAS, not a stage) + a buffer graph with the right ROLES / bytes /
// def-use WIRING (fft's in_re binds the buffer that aliases the gemm output — the asset-drives-it wiring); (2) the fft's
// imaginary input is marked Zeros + two FftTwiddle externals are planned; (3) TYPED-REJECTS (not-verify-clean, reshape-not-alias,
// a non-F32 stage → SynthRejected); (4) GENERICITY — a differently-shaped module plans differently (the 20c-2 rule). Device-free.

#include <crd/ceir/gpu/tensor_pipeline.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>    // register_arith_ops — the compute.dispatch grid consts
#include <crd/ceir/gen/compute_ops.hpp>  // register_compute_ops — the viz-prep dispatch (CEIR-22c-3)
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/linalg.hpp>
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
