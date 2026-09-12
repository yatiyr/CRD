// CEIR-30b-1 (§68/§103 — the HOST provider class) — execute_tensor_pipeline_host, the CPU MIRROR of execute_tensor_pipeline. A
// GENERAL CPU tensor runner (synth_* → the f32-faithful kir::eval_cpu), NOT per-fixture glue — the falsifiable claim this gate
// makes. Device-FREE (no GPU): the whole point of the Host class is a real second §24 provider that needs no device. Proves:
//   (a) the canonical unfused ml.mlp (gemm→relu→gemm), planned FUSED to [GemmRelu, Gemm] (both graph-tier — eval_cpu runs the
//       Max(Contract,0) epilogue the CUDA emitter cannot unwrap), runs BIT-EXACT vs a CPU float MLP oracle whose arithmetic
//       mirrors eval_cpu (per-op multiply-then-add, sequential-k, relu=max). The oracle doctrine makes "==", not a tolerance, the
//       gate — the same reference every GPU backend proves against, now the executor itself;
//   (b) a standalone tensor.reduce(sum) runs BIT-EXACT vs a sequential f32 sum (the §140 collective's building block);
//   (c) an UNSUPPORTED stage kind (a kernel-tier tensor.fft) → ExecuteError::UnresolvedKernel and the pipeline ABORTS with NO
//       partial write downstream (the terminal buffer keeps its sentinel) — a TYPED reject, never a silent skip.

#include <crd/ceir/gpu/tensor_pipeline_exec.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/expand_ml.hpp> // expand_ml_ops (ml.mlp → the gemm/relu/gemm tensor vocab)
#include <crd/ceir/gpu/tensor_pipeline.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp>
#include <crd/ceir/tensor.hpp>
#include <crd/ceir/type.hpp>

#include <crd/math/cmath.hpp> // crd::math::max — the relu in the oracle

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include "../gpu-shared/ckir_asset_resolve.hpp" // CEIR-30b-2b-1: the shared HostKernelResolveFn (loads relu.ckir for VizDispatch)
#include "../gpu-shared/two_class_fixture.hpp"  // CEIR-30b-2b-2b: fill_two_class_sandwich — the ONE oracle body

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
// ⛔ tensor_pipeline_exec.hpp transitively pulls crd::gpu (compute.hpp) alongside crd::ceir::gpu, so a bare `gpu::` is AMBIGUOUS
// under the two using-namespaces — the ceir-gpu names go through this alias (the test_ceir_pipeline_cuda.cpp `ceg` convention).
namespace ceg = crd::ceir::gpu;
using ceg::BufferRole;        // these live in crd::ceir::gpu (NOT crd::ceir), so the using-namespaces do not bring them in
using ceg::ExecuteError;
using ceg::TensorPipelinePlan;
using ceg::TensorPipelineProfile;

namespace
{
TypeId shp(Context& ctx, ConstSpan<TypeId> dims) { return ctx.type_shape(dims); }
TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return shp(ctx, ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return shp(ctx, ConstSpan<TypeId>(d, 2U));
}
TypeId tf(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_f32(), shape); }

Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
Value* decl(Context& ctx, OpId dcl, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(dcl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}

// One host-side input seed: the SSA value + its f32 data (row-major, `count` elements).
struct Seed
{
    const Value* value  = nullptr;
    const f32*   floats = nullptr;
    u32          count  = 0;
};

// Run a plan on the HOST: allocate one f32 region per plan buffer (aliases share the landlord's slice — the alias_of<i order the
// plan guarantees), seed ExternalIn buffers by SSA value, execute, copy the Output-role buffer to `out`. `sentinel` pre-fills every
// backing slot so the (c) no-partial-write check can observe an untouched terminal. Returns the ExecuteError.
ExecuteError run_host(Context& ctx, memory::IAllocator* alloc, const TensorPipelinePlan& plan, const Seed* seeds, usize n_seeds,
                      f32* out, usize out_len, f32 sentinel = 0.0F, TensorPipelineProfile* profile = nullptr,
                      ceg::HostRunOptions opts = {})
{
    const usize nb = plan.buffers.size();
    REQUIRE(nb <= 40U);
    u64 offs[40]  = {};
    u64 total     = 0;
    for (usize i = 0; i < nb; ++i)
    {
        offs[i] = total;
        if (plan.buffers[i].alias_of < 0) { total += plan.buffers[i].bytes / sizeof(f32); }
    }
    containers::Array<f32> store(alloc);
    store.resize(static_cast<usize>(total), sentinel);
    f32* ptr[40] = {};
    for (usize i = 0; i < nb; ++i)
    {
        ptr[i] = plan.buffers[i].alias_of >= 0 ? ptr[static_cast<usize>(plan.buffers[i].alias_of)] : store.data() + offs[i];
    }
    for (usize i = 0; i < nb; ++i)
    {
        if (plan.buffers[i].role != BufferRole::ExternalIn) { continue; }
        for (usize s = 0; s < n_seeds; ++s)
        {
            if (seeds[s].value == plan.buffers[i].value)
            {
                for (u32 e = 0; e < seeds[s].count; ++e) { ptr[i][e] = seeds[s].floats[e]; }
            }
        }
    }
    const ExecuteError ee = ceg::execute_tensor_pipeline_host(ctx, plan, ConstSpan<f32*>(ptr, nb), alloc, profile, opts);
    // ALWAYS copy the Output buffer's CURRENT contents (whether success or error) so the caller can inspect it — on success it is
    // the computed result; on an abort it is whatever was (not) written, so (c) can prove the terminal keeps its sentinel.
    if (out != nullptr)
    {
        i32 oi = -1;
        for (usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == BufferRole::Output) { oi = static_cast<i32>(i); } }
        REQUIRE(oi >= 0);
        for (usize e = 0; e < out_len; ++e) { out[e] = ptr[static_cast<usize>(oi)][e]; }
    }
    return ee;
}

// a HostKernelResolveFn that rejects every symbol — the (c) "resolver returns false → UnresolvedKernel" negative.
bool reject_all_kernels(containers::StringView /*symbol*/, kir::KGraph& /*g*/, kir::KEntry& /*entry*/, void* /*user*/) { return false; }
} // namespace

// (a) the fused MLP runs bit-exact vs the CPU float oracle — the "general CPU executor, not glue" claim made falsifiable.
TEST_CASE("ceir 30b-1: execute_tensor_pipeline_host runs a fused MLP bit-exact vs the CPU oracle", "[ceir][host-exec]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)arith::register_arith_ops(ctx);
    (void)compute::register_compute_ops(ctx);
    (void)linalg::register_dialect(ctx);
    (void)ml::register_dialect(ctx);

    constexpr u32 mrows = 4U;
    constexpr u32 d0    = 6U;
    constexpr u32 d1    = 5U;
    constexpr u32 d2    = 3U;
    Module* const m   = ctx.create_module();
    Block* const  b   = mkmain(ctx, *m);
    const OpId    dcl = ctx.intern_op("resource", "declare");
    Value* const  x   = decl(ctx, dcl, b, tf(ctx, sh2(ctx, mrows, d0)));
    Value* const  w1  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d0, d1)));
    Value* const  w2  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d1, d2)));
    Value*        mlpops[3] = {x, w1, w2};
    Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mlpops, 3U), 1U,
                                               tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mo);

    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    ceg::PlanOptions opts;
    opts.fuse_gemm_relu = true; // gemm→relu folds to GemmRelu (Max(Contract,0)); eval_cpu runs the epilogue ⇒ [GemmRelu, Gemm]
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 2U);
    CHECK(plan.stages[0].kind == ceg::StageKind::GemmRelu);
    CHECK(plan.stages[1].kind == ceg::StageKind::Gemm);

    // fill inputs (x has negatives so relu bites) + the CPU float oracle (arithmetic mirrors eval_cpu: prod=a*b, acc=acc+prod,
    // sequential-k, relu=max) — the SAME recipe as the 29b-1 CUDA gate, so Host == CUDA == oracle all coincide bit-exact.
    f32 x_in[mrows * d0];
    f32 w1_in[d0 * d1];
    f32 w2_in[d1 * d2];
    f32 oracle[mrows * d2];
    for (u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<f32>(static_cast<int>(i) - 12); }
    for (u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<f32>(static_cast<int>(i % 7) - 3); }
    for (u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<f32>(static_cast<int>(i % 5) - 2); }
    for (u32 mm = 0; mm < mrows; ++mm)
    {
        f32 h1[d1];
        for (u32 nn = 0; nn < d1; ++nn)
        {
            f32 acc = 0.0F;
            for (u32 kk = 0; kk < d0; ++kk) { const f32 prod = x_in[mm * d0 + kk] * w1_in[kk * d1 + nn]; acc = acc + prod; }
            h1[nn] = crd::math::max(acc, 0.0F);
        }
        for (u32 j = 0; j < d2; ++j)
        {
            f32 acc = 0.0F;
            for (u32 nn = 0; nn < d1; ++nn) { const f32 prod = h1[nn] * w2_in[nn * d2 + j]; acc = acc + prod; }
            oracle[mm * d2 + j] = acc;
        }
    }

    const Seed seeds[3] = {{x, x_in, mrows * d0}, {w1, w1_in, d0 * d1}, {w2, w2_in, d1 * d2}};
    f32        got[mrows * d2];
    TensorPipelineProfile profile(&alloc); // §137 structural profile — one row per stage (bytes + kind; the CPU has no grid)
    const ExecuteError ee = run_host(ctx, &alloc, plan, seeds, 3U, got, mrows * d2, 0.0F, &profile);
    REQUIRE(ee == ExecuteError::None);
    for (u32 i = 0; i < mrows * d2; ++i) { CHECK(got[i] == oracle[i]); } // BIT-EXACT (the oracle doctrine, not a tolerance)

    // the profile mirrors the plan: two rows, GemmRelu then Gemm, with the trailing-output bytes each stage writes (h1[M,d1],
    // then y[M,d2]) — the row 30b-2's two-class orchestrator reads to attribute work per provider class.
    REQUIRE(profile.stages.size() == 2U);
    CHECK(profile.stages[0].kind == ceg::StageKind::GemmRelu);
    CHECK(profile.stages[1].kind == ceg::StageKind::Gemm);
    CHECK(profile.stages[0].bytes_out == static_cast<u64>(mrows) * d1 * sizeof(f32));
    CHECK(profile.stages[1].bytes_out == static_cast<u64>(mrows) * d2 * sizeof(f32));
    CHECK(profile.stages[0].workgroups == 0U); // CPU reference interpreter — no workgroup grid
}

// (b) a standalone reduce(sum) — the §140 collective's per-device building block — bit-exact vs a sequential f32 sum.
TEST_CASE("ceir 30b-1: execute_tensor_pipeline_host runs a reduce(sum) bit-exact vs a sequential f32 sum", "[ceir][host-exec]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)tensor::register_dialect(ctx);

    constexpr u32 n   = 17U; // odd, non-power-of-two — the accumulation order is what the bit-exact claim is about
    Module* const m   = ctx.create_module();
    Block* const  b   = mkmain(ctx, *m);
    const OpId    dcl = ctx.intern_op("resource", "declare");
    Value* const  in  = decl(ctx, dcl, b, tf(ctx, sh1(ctx, n)));
    Operation* const rd = tensor::build_reduce(ctx, in, ctx.attr_int(0), ctx.attr_string(StringView("sum")),
                                               tf(ctx, shp(ctx, ConstSpan<TypeId>{}))); // rank-0 sum
    b->append(rd);

    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 1U);
    REQUIRE(plan.stages[0].kind == ceg::StageKind::Reduce);

    f32 in_data[n];
    f32 oracle = 0.0F;
    for (u32 i = 0; i < n; ++i) { in_data[i] = 0.3F * static_cast<f32>(static_cast<int>(i) - 8); oracle = oracle + in_data[i]; }

    const Seed         seeds[1] = {{in, in_data, n}};
    f32                got      = -1.0F;
    const ExecuteError ee       = run_host(ctx, &alloc, plan, seeds, 1U, &got, 1U);
    REQUIRE(ee == ExecuteError::None);
    CHECK(got == oracle); // ascending-accumulation, round-per-step ⇒ bit-exact vs the sequential f32 sum
}

// (c) an unsupported (kernel-tier) tensor.fft stage → UnresolvedKernel, the pipeline aborts, and the terminal keeps its sentinel
// (NO partial write past the failed stage). A gemm→reshape→fft→reduce plan: gemm runs, fft rejects, reduce (terminal) never runs.
TEST_CASE("ceir 30b-1: execute_tensor_pipeline_host rejects an unsupported stage with no partial write", "[ceir][host-exec]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)linalg::register_dialect(ctx);
    (void)tensor::register_dialect(ctx);

    constexpr u32 side = 4U;
    constexpr u32 l    = side * side;
    Module* const m    = ctx.create_module();
    Block* const  b    = mkmain(ctx, *m);
    const OpId    dcl  = ctx.intern_op("resource", "declare");
    Value* const  a    = decl(ctx, dcl, b, tf(ctx, sh2(ctx, side, side)));
    Value* const  bb   = decl(ctx, dcl, b, tf(ctx, sh2(ctx, side, side)));
    Value* const  c    = decl(ctx, dcl, b, tf(ctx, sh2(ctx, side, side)));
    Operation* const gm = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                             ctx.attr_bool(false), tf(ctx, sh2(ctx, side, side)));
    b->append(gm);
    Operation* const rs = tensor::build_reshape(ctx, gm->result(0U), tf(ctx, sh1(ctx, l)));
    b->append(rs);
    Value* const im0 = decl(ctx, dcl, b, tf(ctx, sh1(ctx, l)));
    Value*       ffops[2] = {rs->result(0U), im0};
    Operation* const ff = ctx.create_operation(ctx.intern_op("tensor", "fft"), ConstSpan<Value*>(ffops, 2U), 2U, tf(ctx, sh1(ctx, l)));
    ctx.set_attr(ff, StringView("direction"), ctx.attr_string(StringView("forward")));
    ctx.set_attr(ff, StringView("axis"), ctx.attr_int(0));
    b->append(ff);
    Operation* const rd = tensor::build_reduce(ctx, ff->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")),
                                               tf(ctx, shp(ctx, ConstSpan<TypeId>{})));
    b->append(rd);

    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U); // [Gemm, Fft, Reduce] — reshape is a zero-copy alias, not a stage
    CHECK(plan.stages[1].kind == ceg::StageKind::Fft);

    // seed the two gemm-input externals; leave the terminal (reduce Output) at the sentinel. The fft (stage 1) aborts, so the
    // reduce (stage 2) never writes — the terminal keeps -777 (the no-partial-write proof).
    f32 adata[side * side];
    f32 bdata[side * side];
    for (u32 i = 0; i < side * side; ++i) { adata[i] = 0.1F * static_cast<f32>(i + 1U); bdata[i] = -0.2F * static_cast<f32>(i); }
    const Seed         seeds[2] = {{a, adata, side * side}, {bb, bdata, side * side}};
    f32                terminal = 0.0F; // run_host copies the terminal (Output) buffer's CURRENT contents into this
    const ExecuteError ee       = run_host(ctx, &alloc, plan, seeds, 2U, &terminal, 1U, -777.0F);
    CHECK(ee == ExecuteError::UnresolvedKernel);
    CHECK(terminal == -777.0F); // the reduce (terminal) never ran — no partial write past the aborted fft stage
}

// the other three graph-tier kinds the header claims — elementwise (30b-3's per-shard op AND the only other 2-input bind path),
// transpose, broadcast — each a single-stage plan run bit-exact vs a hand oracle. Closes the "claimed-but-untested" gap.
TEST_CASE("ceir 30b-1: execute_tensor_pipeline_host runs elementwise/transpose/broadcast bit-exact", "[ceir][host-exec]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)tensor::register_dialect(ctx);
    const OpId dcl = ctx.intern_op("resource", "declare");

    SECTION("elementwise add")
    {
        constexpr u32    n = 6U;
        Module* const    m = ctx.create_module();
        Block* const     b = mkmain(ctx, *m);
        Value* const     a = decl(ctx, dcl, b, tf(ctx, sh1(ctx, n)));
        Value* const     c = decl(ctx, dcl, b, tf(ctx, sh1(ctx, n)));
        Operation* const ew = tensor::build_elementwise(ctx, a, c, ctx.attr_string(StringView("add")), tf(ctx, sh1(ctx, n)));
        b->append(ew);
        const TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
        REQUIRE(plan.reject == ceg::PlanReject::None);
        REQUIRE(plan.stages.size() == 1U);
        REQUIRE(plan.stages[0].kind == ceg::StageKind::Elementwise);

        f32 ad[n];
        f32 cd[n];
        f32 oracle[n];
        for (u32 i = 0; i < n; ++i) { ad[i] = 0.5F * static_cast<f32>(i) - 1.0F; cd[i] = -0.25F * static_cast<f32>(i) + 2.0F; oracle[i] = ad[i] + cd[i]; }
        const Seed seeds[2] = {{a, ad, n}, {c, cd, n}};
        f32        got[n];
        REQUIRE(run_host(ctx, &alloc, plan, seeds, 2U, got, n) == ExecuteError::None);
        for (u32 i = 0; i < n; ++i) { CHECK(got[i] == oracle[i]); }
    }

    SECTION("transpose [2,3]->[3,2]")
    {
        constexpr u32    r = 2U;
        constexpr u32    c = 3U;
        Module* const    m = ctx.create_module();
        Block* const     b = mkmain(ctx, *m);
        Value* const     a = decl(ctx, dcl, b, tf(ctx, sh2(ctx, r, c)));
        Operation* const tp = tensor::build_transpose(ctx, a, ctx.attr_string(StringView("1,0")), tf(ctx, sh2(ctx, c, r)));
        b->append(tp);
        const TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
        REQUIRE(plan.reject == ceg::PlanReject::None);
        REQUIRE(plan.stages.size() == 1U);
        REQUIRE(plan.stages[0].kind == ceg::StageKind::Transpose);

        f32 ad[r * c];
        for (u32 i = 0; i < r * c; ++i) { ad[i] = 0.1F * static_cast<f32>(i + 1U); }
        f32 oracle[c * r]; // out[i,j] = a[j,i] — out[i*r+j] = a[j*c+i]
        for (u32 i = 0; i < c; ++i) { for (u32 j = 0; j < r; ++j) { oracle[i * r + j] = ad[j * c + i]; } }
        const Seed seeds[1] = {{a, ad, r * c}};
        f32        got[c * r];
        REQUIRE(run_host(ctx, &alloc, plan, seeds, 1U, got, c * r) == ExecuteError::None);
        for (u32 i = 0; i < c * r; ++i) { CHECK(got[i] == oracle[i]); }
    }

    SECTION("broadcast [1,3]->[2,3]")
    {
        constexpr u32    c    = 3U;
        constexpr u32    rout = 2U;
        Module* const    m = ctx.create_module();
        Block* const     b = mkmain(ctx, *m);
        Value* const     a = decl(ctx, dcl, b, tf(ctx, sh2(ctx, 1U, c)));
        Operation* const bc = tensor::build_broadcast(ctx, a, tf(ctx, sh2(ctx, rout, c)));
        b->append(bc);
        const TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
        REQUIRE(plan.reject == ceg::PlanReject::None);
        REQUIRE(plan.stages.size() == 1U);
        REQUIRE(plan.stages[0].kind == ceg::StageKind::Broadcast);

        f32 ad[c];
        for (u32 i = 0; i < c; ++i) { ad[i] = 0.3F * static_cast<f32>(i) - 0.7F; }
        f32 oracle[rout * c]; // row-repeat: out[r,col] = a[0,col]
        for (u32 rr = 0; rr < rout; ++rr) { for (u32 col = 0; col < c; ++col) { oracle[rr * c + col] = ad[col]; } }
        const Seed seeds[1] = {{a, ad, c}};
        f32        got[rout * c];
        REQUIRE(run_host(ctx, &alloc, plan, seeds, 1U, got, rout * c) == ExecuteError::None);
        for (u32 i = 0; i < rout * c; ++i) { CHECK(got[i] == oracle[i]); }
    }
}

// CEIR-30b-2b-1: the AUTHORED relu VizDispatch on Host via ckir_read→eval_cpu_kernel (the exit-(i) path — retires the 30b-1
// "authored-kernel CPU eval is name-forward" deferral). The SAME MLP as the (a) gate, but planned fuse=FALSE ⇒ [Gemm,
// VizDispatch(relu), Gemm] — the relu rides the kernel-tier path, bit-exact vs the same CPU oracle. + the resolver negatives.
TEST_CASE("ceir 30b-2b-1: execute_tensor_pipeline_host runs a fuse=false relu VizDispatch on Host bit-exact", "[ceir][host-exec]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)arith::register_arith_ops(ctx);
    (void)compute::register_compute_ops(ctx);
    (void)linalg::register_dialect(ctx);
    (void)ml::register_dialect(ctx);

    constexpr u32 mrows = 4U;
    constexpr u32 d0    = 6U;
    constexpr u32 d1    = 5U;
    constexpr u32 d2    = 3U;
    Module* const m   = ctx.create_module();
    Block* const  b   = mkmain(ctx, *m);
    const OpId    dcl = ctx.intern_op("resource", "declare");
    Value* const  x   = decl(ctx, dcl, b, tf(ctx, sh2(ctx, mrows, d0)));
    Value* const  w1  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d0, d1)));
    Value* const  w2  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d1, d2)));
    Value*        mlpops[3] = {x, w1, w2};
    Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mlpops, 3U), 1U,
                                               tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mo);

    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    ceg::PlanOptions opts;
    opts.fuse_gemm_relu = false; // relu is a VizDispatch stage (NOT folded) — the kernel-tier path this slice adds
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);
    CHECK(plan.stages[0].kind == ceg::StageKind::Gemm);
    CHECK(plan.stages[1].kind == ceg::StageKind::VizDispatch); // the relu
    CHECK(plan.stages[2].kind == ceg::StageKind::Gemm);

    f32 x_in[mrows * d0];
    f32 w1_in[d0 * d1];
    f32 w2_in[d1 * d2];
    f32 oracle[mrows * d2];
    for (u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<f32>(static_cast<int>(i) - 12); }
    for (u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<f32>(static_cast<int>(i % 7) - 3); }
    for (u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<f32>(static_cast<int>(i % 5) - 2); }
    for (u32 mm = 0; mm < mrows; ++mm)
    {
        f32 h1[d1];
        for (u32 nn = 0; nn < d1; ++nn)
        {
            f32 acc = 0.0F;
            for (u32 kk = 0; kk < d0; ++kk) { const f32 prod = x_in[mm * d0 + kk] * w1_in[kk * d1 + nn]; acc = acc + prod; }
            h1[nn] = crd::math::max(acc, 0.0F);
        }
        for (u32 j = 0; j < d2; ++j)
        {
            f32 acc = 0.0F;
            for (u32 nn = 0; nn < d1; ++nn) { const f32 prod = h1[nn] * w2_in[nn * d2 + j]; acc = acc + prod; }
            oracle[mm * d2 + j] = acc;
        }
    }
    const Seed seeds[3] = {{x, x_in, mrows * d0}, {w1, w1_in, d0 * d1}, {w2, w2_in, d1 * d2}};

    SECTION("bit-exact through the authored relu.ckir")
    {
        ceg::HostRunOptions ropts;
        ropts.kernel = &crd::tests::resolve_ckir_asset;
        ropts.user   = &alloc;
        f32 got[mrows * d2];
        const ExecuteError ee = run_host(ctx, &alloc, plan, seeds, 3U, got, mrows * d2, 0.0F, nullptr, ropts);
        REQUIRE(ee == ExecuteError::None);
        for (u32 i = 0; i < mrows * d2; ++i) { CHECK(got[i] == oracle[i]); } // BIT-EXACT (the oracle doctrine)
    }
    SECTION("no resolver -> UnresolvedKernel (the 30b-1 behavior preserved)")
    {
        f32 got[mrows * d2];
        CHECK(run_host(ctx, &alloc, plan, seeds, 3U, got, mrows * d2) == ExecuteError::UnresolvedKernel);
    }
    SECTION("resolver rejects the symbol -> UnresolvedKernel, terminal untouched")
    {
        ceg::HostRunOptions ropts;
        ropts.kernel = &reject_all_kernels;
        f32 term[mrows * d2];
        const ExecuteError ee = run_host(ctx, &alloc, plan, seeds, 3U, term, mrows * d2, -777.0F, nullptr, ropts);
        CHECK(ee == ExecuteError::UnresolvedKernel);
        for (u32 i = 0; i < mrows * d2; ++i) { CHECK(term[i] == -777.0F); } // the 2nd gemm (Output) never ran — no partial write
    }
}

// CEIR-30b-2b-1: the 29c-2 SANDWICH gemm(x,W0)→mlp(relu)→gemm(.,W3) runs ALL-HOST (fuse=false) bit-exact vs the two-stage oracle.
// This is 30b-2b-2's all-HOST arm proven DEVICE-FREE now — so 30b-2b-2 debugs only the transfer + Gpu halves.
TEST_CASE("ceir 30b-2b-1: the two-class sandwich runs all-Host bit-exact", "[ceir][host-exec]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)arith::register_arith_ops(ctx);
    (void)compute::register_compute_ops(ctx);
    (void)linalg::register_dialect(ctx);
    (void)ml::register_dialect(ctx);

    constexpr u32 dd  = 4U; // uniform 4-wide (matches test_two_class's build_sandwich)
    Module* const m   = ctx.create_module();
    Block* const  b   = mkmain(ctx, *m);
    const OpId    dcl = ctx.intern_op("resource", "declare");
    const auto    gm  = [&](Value* aa, Value* bb, Value* cc) -> Value* {
        Operation* const g = linalg::build_gemm(ctx, aa, bb, cc, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                                ctx.attr_bool(false), tf(ctx, sh2(ctx, dd, dd)));
        b->append(g);
        return g->result(0U);
    };
    Value* const x   = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dd, dd)));
    Value* const w0  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dd, dd)));
    Value* const c0  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dd, dd)));
    Value* const xp  = gm(x, w0, c0);
    Value* const w1  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dd, dd)));
    Value* const w2  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dd, dd)));
    Value*       mlpops[3] = {xp, w1, w2};
    Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mlpops, 3U), 1U, tf(ctx, sh2(ctx, dd, dd)), 0U);
    ctx.set_attr(mo, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mo);
    Value* const w3 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dd, dd)));
    Value* const c3 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dd, dd)));
    (void)gm(mo->result(0U), w3, c3);

    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    ceg::PlanOptions popts;
    popts.fuse_gemm_relu             = false;
    popts.share_intermediate_storage = false;
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc, popts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 5U); // [Gemm, Gemm, VizDispatch(relu), Gemm, Gemm]

    // fill + the CPU oracle (xp=x@W0, h1=relu(xp@W1), yy=h1@W2, z=yy@W3, sequential-k = eval_cpu) via the ONE shared body
    // `crd::tests::fill_two_class_sandwich` (tests/gpu-shared/two_class_fixture.hpp) at dd=4 — byte-identical to the CUDA three-way
    // gate's data, so this all-Host arm is exactly what 30b-2b-2b re-seeds on the device.
    f32 x_in[dd * dd];
    f32 w0_in[dd * dd];
    f32 w1_in[dd * dd];
    f32 w2_in[dd * dd];
    f32 w3_in[dd * dd];
    f32 oracle[dd * dd];
    crd::tests::fill_two_class_sandwich(dd, dd, dd, dd, dd, x_in, w0_in, w1_in, w2_in, w3_in, oracle);

    ceg::HostRunOptions ropts;
    ropts.kernel = &crd::tests::resolve_ckir_asset;
    ropts.user   = &alloc;
    const Seed seeds[5] = {{x, x_in, dd * dd}, {w0, w0_in, dd * dd}, {w1, w1_in, dd * dd}, {w2, w2_in, dd * dd}, {w3, w3_in, dd * dd}};
    f32        got[dd * dd];
    const ExecuteError ee = run_host(ctx, &alloc, plan, seeds, 5U, got, dd * dd, 0.0F, nullptr, ropts);
    REQUIRE(ee == ExecuteError::None);
    for (u32 i = 0; i < dd * dd; ++i) { CHECK(got[i] == oracle[i]); } // BIT-EXACT all-Host (the 30b-2b-2 all-Host arm)
}
