// CEIR-30b-2a (§68/§103/§24 — the TWO-CLASS PLACEMENT→TRANSFER pass) — `plan_transfers` + `stage_class_from_partition`, the PURE
// device-free half of the Host+Gpu two-class run (30b-2b adds the engine runner + the CUDA gate). Placement is DATA: a
// `ProviderClass` per stage (hand-written by a fixture here; produced by 30b-3's sharding placement later) drives the host↔device
// transfers the plan's def-use edges imply — "placement is semantic, not glue" (§140), proven by INSPECTING the transfer plan.
// ⛔ MODEL (documented in the header): ExternalIn buffers are Host-BORN, and the Output-must-be-Host-visible readback IS a planned
// transfer — so ALL host↔device movement is planned (the runner hand-copies NOTHING). Proves:
//   (a) all-Host class vector → ZERO transfers; all-Gpu → exactly ONE DeviceToHost (the Output readback) + the ExternalIn uploads;
//   (b) the 5-stage sandwich [Gpu,Host,Host,Host,Gpu] → the KEY inter-class transfers by IDENTITY (xp D→H before 1, y H→D before 4,
//       z D→H at the end) + NO transfer inside the all-Host middle run (before_stage 2/3);
//   (c) the REVERSE sandwich [Host,Gpu,Gpu,Gpu,Host] → the transfers FLIP (xp H→D before 1, y D→H before 4) + NO final readback
//       (the Output ends on Host) — genericity, the placement drives it;
//   (d) an ALIASED buffer (a reshape) crossing → the transfer names the LANDLORD index, never the alias;
//   (e) `stage_class_from_partition` on the 29c-2 partition → all-Gpu (cuda_graphs is Gpu, fallback→Gpu) ⇒ plan_transfers has NO
//       inter-stage cross-class transfer (just the uploads + the one readback) — the adapter + "the 29c world was one class".

#include <crd/ceir/gpu/tensor_pipeline_exec.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/expand_ml.hpp>
#include <crd/ceir/gpu/partition_ml.hpp>
#include <crd/ceir/gpu/tensor_pipeline.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp>
#include <crd/ceir/semantics.hpp>
#include <crd/ceir/tensor.hpp>
#include <crd/ceir/type.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include "../gpu-shared/ckir_asset_resolve.hpp" // CEIR-30b-2b-2: the shared HostKernelResolveFn (relu.ckir for the mock VizDispatch)
#include "../gpu-shared/two_class_fixture.hpp"  // CEIR-30b-2b-2b: fill_two_class_sandwich — the ONE oracle body

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
// tensor_pipeline_exec.hpp transitively pulls crd::gpu (compute.hpp) alongside crd::ceir::gpu → a bare `gpu::` is ambiguous; the
// ceir-gpu names go through this alias (the test_host_exec.cpp convention). ProviderClass lives in crd::ceir (visible directly).
namespace ceg = crd::ceir::gpu;
using ceg::BufferRole;
using ceg::ExecuteError;
using ceg::MlProvider;
using ceg::TensorPipelineProfile;
using ceg::Transfer;
using ceg::TransferDir;
using ceg::TransferPlan;

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
void register_all(Context& ctx)
{
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)arith::register_arith_ops(ctx);
    (void)compute::register_compute_ops(ctx);
    (void)linalg::register_dialect(ctx);
    (void)tensor::register_dialect(ctx);
    (void)ml::register_dialect(ctx);
}
Value* decl(Context& ctx, OpId dcl, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(dcl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
Value* gemm(Context& ctx, Block* b, Value* a, Value* bb, Value* c, u32 m, u32 n)
{
    Operation* const g = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                            ctx.attr_bool(false), tf(ctx, sh2(ctx, m, n)));
    b->append(g);
    return g->result(0U);
}

// CEIR-30b-2b-2: the sandwich's ExternalIn SSA values a two-class EXECUTION gate seeds (the plan-only 30b-2a tests ignore this).
struct SandwichIO
{
    const Value* x  = nullptr;
    const Value* w0 = nullptr;
    const Value* w1 = nullptr;
    const Value* w2 = nullptr;
    const Value* w3 = nullptr;
};
// CEIR-30b-2b-2: one host-side input seed — the SSA value + its f32 data (the test_host_exec.cpp mold).
struct Seed
{
    const Value* value  = nullptr;
    const f32*   floats = nullptr;
    u32          count  = 0;
};

// build gemm(x,W0)→xp · ml.mlp(xp,W1,W2)relu→y · gemm(y,W3)→z, EXPAND (fuse=false ⇒ [Gemm,Gemm,VizDispatch,Gemm,Gemm]), plan. ⛔ the
// ml.mlp's result value is REPLACED by expand_ml_ops (RAUW+erase), so the KEY buffers are identified by STAGE POSITION (stage_out
// below), NOT by pre-expansion SSA values: xp=stage 0 out, y=stage 3 out, z=stage 4 out (the Output). `io` (optional) captures the
// ExternalIn values a 30b-2b-2 execution gate seeds; `share` toggles share_intermediate_storage (false ⇒ stage_out == landlord).
ceg::TensorPipelinePlan build_sandwich(Context& ctx, Module& m, memory::IAllocator* alloc, SandwichIO* io = nullptr, bool share = false)
{
    Block* const   b   = mkmain(ctx, m);
    const OpId     dcl = ctx.intern_op("resource", "declare");
    constexpr u32  rows = 4U; // uniform 4-wide (coopvec-legal in (e); a real relu MLP)
    constexpr u32  dim  = 4U;
    Value* const   x  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, rows, dim)));
    Value* const   w0 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dim, dim)));
    Value* const   c0 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, rows, dim)));
    Value* const   xp = gemm(ctx, b, x, w0, c0, rows, dim);
    Value* const   w1 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dim, dim)));
    Value* const   w2 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dim, dim)));
    Value*         mlpops[3] = {xp, w1, w2};
    Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mlpops, 3U), 1U, tf(ctx, sh2(ctx, rows, dim)), 0U);
    ctx.set_attr(mo, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mo);
    Value* const   w3 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dim, dim)));
    Value* const   c3 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, rows, dim)));
    (void)gemm(ctx, b, mo->result(0U), w3, c3, rows, dim);
    if (io != nullptr) { io->x = x; io->w0 = w0; io->w1 = w1; io->w2 = w2; io->w3 = w3; } // capture BEFORE expand (these survive it)

    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    ceg::PlanOptions opts;
    opts.fuse_gemm_relu             = false; // relu is a VizDispatch stage (not folded) — the [Gemm,Gemm,VizDispatch,Gemm,Gemm] shape
    opts.share_intermediate_storage = share; // false ⇒ each intermediate its OWN buffer (stage_out==landlord); true ⇒ the aliasing path
    ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, m, alloc, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 5U);
    return plan;
}

// the LANDLORD buffer a stage WRITES (its single trailing output — n_out==1 for every graph-tier/VizDispatch stage here). Robust
// to the ml.mlp result-value replacement: a stage's output buffer is a plan fact, not a pre-expansion SSA value.
u32 stage_out(const ceg::TensorPipelinePlan& plan, usize s)
{
    const ceg::PlanStage& st = plan.stages[s];
    const i32             a  = plan.buffers[static_cast<usize>(st.bind[st.nbind - 1U])].alias_of;
    return a >= 0 ? static_cast<u32>(a) : static_cast<u32>(st.bind[st.nbind - 1U]);
}
bool has_xfer(const TransferPlan& tp, u32 buffer, u32 before, TransferDir dir)
{
    for (usize i = 0; i < tp.transfers.size(); ++i)
    {
        const Transfer& t = tp.transfers[i];
        if (t.buffer == buffer && t.before_stage == before && t.direction == dir) { return true; }
    }
    return false;
}
u32 count_dir(const TransferPlan& tp, TransferDir dir)
{
    u32 n = 0;
    for (usize i = 0; i < tp.transfers.size(); ++i) { n += tp.transfers[i].direction == dir ? 1U : 0U; }
    return n;
}
u32 count_before(const TransferPlan& tp, u32 before)
{
    u32 n = 0;
    for (usize i = 0; i < tp.transfers.size(); ++i) { n += tp.transfers[i].before_stage == before ? 1U : 0U; }
    return n;
}
ConstSpan<ProviderClass> classes(const ProviderClass* p, usize n) { return ConstSpan<ProviderClass>(p, n); }

// CEIR-30b-2b-2: a DEVICE-FREE mock of the "device" provider class — a SECOND f32 table (populated ONLY by transfers, sentinel until
// then) run by ANOTHER Host executor, plus logs so the gate asserts execute_two_class drove EXACTLY the planned transfers + device
// stages (nothing hand-copied). The GpuStageFn/TransferFn are plain fn-ptrs (execute_two_class's contract), so their state rides here.
struct MockDevice
{
    Context*                            ctx = nullptr;
    memory::IAllocator*                 alloc = nullptr;
    f32*                                dev[40] = {}; // the "device" table, indexed by PLAN buffer (aliases → their landlord slot)
    usize                               nb    = 0;
    ceg::HostRunOptions                 host_opts;    // the relu resolver, so a device-side VizDispatch stage can run too
    containers::Array<Transfer>         xfer_log;     // every applied transfer, in order (asserted == plan_transfers by identity)
    containers::Array<const Operation*> gpu_ops;      // every device stage's op, in order (asserted == the device-class stages)
    u32                                 gpu_calls = 0;
    ExecuteError                        gpu_err   = ExecuteError::None; // error injection: a device stage that fails
    ExecuteError                        xfer_err  = ExecuteError::None; // error injection: a transfer that fails
    explicit MockDevice(memory::IAllocator* a) : alloc(a), xfer_log(a), gpu_ops(a) {}
};

// the device-class runner: log the stage, then run the 1-stage slice on the CPU over the SECOND table (a real 2nd executor — the
// mock proves the runner's slice/placement/threading device-free; a missing upload leaves the sentinel and the bit-exact fails).
ExecuteError mock_gpu_stage(const ceg::TensorPipelinePlan& slice, void* user)
{
    auto& md = *static_cast<MockDevice*>(user);
    ++md.gpu_calls;
    md.gpu_ops.push_back(slice.stages[0].op);
    if (md.gpu_err != ExecuteError::None) { return md.gpu_err; }
    return ceg::execute_tensor_pipeline_host(*md.ctx, slice, ConstSpan<f32*>(md.dev, md.nb), md.alloc, nullptr, md.host_opts);
}

// the cross-domain copy: log it, then memcpy between the host_ptr (== the runner's host_bufs[t.buffer]) and the device table.
ExecuteError mock_transfer(const Transfer& t, f32* host_ptr, u64 bytes, void* user)
{
    auto& md = *static_cast<MockDevice*>(user);
    md.xfer_log.push_back(t);
    if (md.xfer_err != ExecuteError::None) { return md.xfer_err; }
    const usize n  = static_cast<usize>(bytes / sizeof(f32));
    f32* const  dp = md.dev[static_cast<usize>(t.buffer)];
    if (t.direction == TransferDir::HostToDevice) { for (usize e = 0; e < n; ++e) { dp[e] = host_ptr[e]; } }
    else { for (usize e = 0; e < n; ++e) { host_ptr[e] = dp[e]; } }
    return ExecuteError::None;
}

// the fill_two_class_data_and_oracle seeds (byte-identical to test_ceir_pipeline_cuda.cpp) + the CPU oracle: xp=x@W0, h1=relu(xp@W1),
// yy=h1@W2, z=yy@W3, sequential-k = eval_cpu. dd=4. ⛔ 30b-2b-2b hoists this to gpu-shared for the CUDA three-way gate.
struct SandwichData
{
    f32 x[16];
    f32 w0[16];
    f32 w1[16];
    f32 w2[16];
    f32 w3[16];
    f32 oracle[16];
};
void fill_sandwich(SandwichData& d)
{
    crd::tests::fill_two_class_sandwich(4U, 4U, 4U, 4U, 4U, d.x, d.w0, d.w1, d.w2, d.w3, d.oracle); // the shared body at dd=4
}

// allocate a HOST table + the mock DEVICE table (one f32 region per landlord buffer, aliases share; both sentinel-filled), seed the
// host EXTERNALS by value (the device table stays sentinel until a transfer populates it), run execute_two_class, copy the Output
// (which the runner leaves Host-visible in the host table) to `out`. Returns the ExecuteError.
ExecuteError run_two_class_mock(MockDevice& md, const ceg::TensorPipelinePlan& plan, ConstSpan<ProviderClass> sc, const Seed* seeds,
                                usize n_seeds, f32* out, usize out_len, f32 sentinel, TensorPipelineProfile* profile = nullptr)
{
    const usize nb = plan.buffers.size();
    REQUIRE(nb <= 40U);
    u64 offs[40] = {};
    u64 total    = 0;
    for (usize i = 0; i < nb; ++i)
    {
        offs[i] = total;
        if (plan.buffers[i].alias_of < 0) { total += plan.buffers[i].bytes / sizeof(f32); }
    }
    containers::Array<f32> hstore(md.alloc);
    hstore.resize(static_cast<usize>(total), sentinel);
    containers::Array<f32> dstore(md.alloc);
    dstore.resize(static_cast<usize>(total), sentinel);
    f32* hptr[40] = {};
    for (usize i = 0; i < nb; ++i)
    {
        hptr[i]   = plan.buffers[i].alias_of >= 0 ? hptr[static_cast<usize>(plan.buffers[i].alias_of)] : hstore.data() + offs[i];
        md.dev[i] = plan.buffers[i].alias_of >= 0 ? md.dev[static_cast<usize>(plan.buffers[i].alias_of)] : dstore.data() + offs[i];
    }
    md.nb = nb;
    for (usize i = 0; i < nb; ++i)
    {
        if (plan.buffers[i].role != BufferRole::ExternalIn) { continue; }
        for (usize s = 0; s < n_seeds; ++s)
        {
            if (seeds[s].value == plan.buffers[i].value) { for (u32 e = 0; e < seeds[s].count; ++e) { hptr[i][e] = seeds[s].floats[e]; } }
        }
    }
    const ExecuteError ee = ceg::execute_two_class(*md.ctx, plan, sc, ConstSpan<f32*>(hptr, nb), &md, &mock_gpu_stage, &mock_transfer,
                                                   md.alloc, md.host_opts, profile);
    if (out != nullptr)
    {
        i32 oi = -1;
        for (usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == BufferRole::Output) { oi = static_cast<i32>(i); } }
        REQUIRE(oi >= 0);
        for (usize e = 0; e < out_len; ++e) { out[e] = hptr[static_cast<usize>(oi)][e]; } // the runner left the Output Host-visible
    }
    return ee;
}
} // namespace

// (a) all-Host → zero transfers; all-Gpu → exactly ONE readback (the Output) + the ExternalIn uploads.
TEST_CASE("ceir 30b-2a: plan_transfers all-Host is empty, all-Gpu is uploads + one readback", "[ceir][two-class]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    register_all(ctx);
    Module* const                 m    = ctx.create_module();
    const ceg::TensorPipelinePlan plan = build_sandwich(ctx, *m, &alloc);
    const u32                     z    = stage_out(plan, 4U); // the Output buffer (last stage's write)

    const ProviderClass all_host[5] = {ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host};
    const TransferPlan  th = ceg::plan_transfers(plan, classes(all_host, 5U), &alloc);
    CHECK(th.transfers.size() == 0U); // everything on Host — no §24 crossing

    const ProviderClass all_gpu[5] = {ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu};
    const TransferPlan  tg = ceg::plan_transfers(plan, classes(all_gpu, 5U), &alloc);
    CHECK(count_dir(tg, TransferDir::DeviceToHost) == 1U);                                   // exactly the Output readback
    CHECK(has_xfer(tg, z, static_cast<u32>(plan.stages.size()), TransferDir::DeviceToHost)); // z, at the end
    CHECK(count_dir(tg, TransferDir::HostToDevice) >= 3U);                                   // the ExternalIn uploads (x,W0,...)
    // every H2D is an UPLOAD before some stage (never after the last) — no inter-stage cross-class movement in an all-Gpu run.
    for (usize i = 0; i < tg.transfers.size(); ++i)
    {
        if (tg.transfers[i].direction == TransferDir::HostToDevice) { CHECK(tg.transfers[i].before_stage < plan.stages.size()); }
    }
}

// (b) the sandwich [Gpu,Host,Host,Host,Gpu] — the KEY inter-class transfers by identity + NO transfer inside the Host middle run.
TEST_CASE("ceir 30b-2a: plan_transfers on the Gpu-Host-Host-Host-Gpu sandwich crosses exactly at the class edges", "[ceir][two-class]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    register_all(ctx);
    Module* const                 m    = ctx.create_module();
    const ceg::TensorPipelinePlan plan = build_sandwich(ctx, *m, &alloc);
    const u32                     xp = stage_out(plan, 0U); // gemm(x,W0) → xp (mlp input)
    const u32                     y  = stage_out(plan, 3U); // the mlp's 2nd gemm → y (last-gemm input)
    const u32                     z  = stage_out(plan, 4U); // gemm(y,W3) → z (Output)

    const ProviderClass sc[5] = {ProviderClass::Gpu, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Gpu};
    const TransferPlan  tp = ceg::plan_transfers(plan, classes(sc, 5U), &alloc);

    CHECK(has_xfer(tp, xp, 1U, TransferDir::DeviceToHost)); // xp: written on Gpu@0, read on Host@1
    CHECK(has_xfer(tp, y, 4U, TransferDir::HostToDevice));  // y : written on Host@3, read on Gpu@4
    CHECK(has_xfer(tp, z, 5U, TransferDir::DeviceToHost));  // z : Output written on Gpu@4 → final readback
    CHECK(count_before(tp, 2U) == 0U); // stage 2 (relu, Host) reads a Host-written buffer — no crossing
    CHECK(count_before(tp, 3U) == 0U); // stage 3 (gemm, Host) reads Host-written + Host externals — no crossing
    CHECK(count_before(tp, 0U) >= 2U); // stage 0 (Gpu) uploads its Host externals (x, W0, ...)
}

// (c) the REVERSE sandwich [Host,Gpu,Gpu,Gpu,Host] — placement flips the transfers; the Output ends on Host (NO readback).
TEST_CASE("ceir 30b-2a: plan_transfers on the reverse sandwich flips the transfers and needs no final readback", "[ceir][two-class]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    register_all(ctx);
    Module* const                 m    = ctx.create_module();
    const ceg::TensorPipelinePlan plan = build_sandwich(ctx, *m, &alloc);
    const u32                     xp = stage_out(plan, 0U);
    const u32                     y  = stage_out(plan, 3U);
    const u32                     z  = stage_out(plan, 4U);

    const ProviderClass sc[5] = {ProviderClass::Host, ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Host};
    const TransferPlan  tp = ceg::plan_transfers(plan, classes(sc, 5U), &alloc);

    CHECK(has_xfer(tp, xp, 1U, TransferDir::HostToDevice)); // xp: written on Host@0, read on Gpu@1 (flipped)
    CHECK(has_xfer(tp, y, 4U, TransferDir::DeviceToHost));  // y : written on Gpu@3, read on Host@4 (flipped)
    // the Output z is written on Host@4 → already Host-visible → NO final readback (before_stage == num_stages).
    CHECK(!has_xfer(tp, z, 5U, TransferDir::DeviceToHost));
    CHECK(count_before(tp, 5U) == 0U); // no end-of-pipeline transfer at all — the terminal is Host
}

// (d) an ALIASED buffer (a reshape) crossing the class boundary → the transfer names the LANDLORD, never the alias index.
TEST_CASE("ceir 30b-2a: plan_transfers names the landlord for an aliased buffer crossing", "[ceir][two-class]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    register_all(ctx);
    Module* const  m   = ctx.create_module();
    Block* const   b   = mkmain(ctx, *m);
    const OpId     dcl = ctx.intern_op("resource", "declare");
    constexpr u32  side = 4U;
    constexpr u32  l    = side * side;
    Value* const   a  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, side, side)));
    Value* const   bb = decl(ctx, dcl, b, tf(ctx, sh2(ctx, side, side)));
    Value* const   c  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, side, side)));
    Value* const   d  = gemm(ctx, b, a, bb, c, side, side);
    Operation* const rs = tensor::build_reshape(ctx, d, tf(ctx, sh1(ctx, l))); // a zero-copy ALIAS of the gemm output
    b->append(rs);
    Operation* const rd = tensor::build_reduce(ctx, rs->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")),
                                               tf(ctx, shp(ctx, ConstSpan<TypeId>{})));
    b->append(rd);
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 2U); // [Gemm, Reduce] — reshape is an alias, not a stage

    // gemm on Gpu, reduce on Host: the reshaped buffer (an Alias of the gemm output) crosses Gpu→Host before the reduce.
    const ProviderClass sc[2] = {ProviderClass::Gpu, ProviderClass::Host};
    const TransferPlan  tp = ceg::plan_transfers(plan, classes(sc, 2U), &alloc);

    // the reshape result is an Alias buffer; its landlord is the realized gemm-output buffer.
    u32 alias_idx = 0;
    for (usize i = 0; i < plan.buffers.size(); ++i) { if (plan.buffers[i].value == rs->result(0U)) { alias_idx = static_cast<u32>(i); } }
    REQUIRE(plan.buffers[alias_idx].alias_of >= 0);                              // the reshape IS an alias
    const u32 landlord = static_cast<u32>(plan.buffers[alias_idx].alias_of);
    CHECK(plan.buffers[landlord].alias_of < 0);                                  // the landlord is a realized buffer
    CHECK(has_xfer(tp, landlord, 1U, TransferDir::DeviceToHost));                // the transfer names the LANDLORD
    CHECK(!has_xfer(tp, alias_idx, 1U, TransferDir::DeviceToHost));              // NEVER the alias index
}

// (e) stage_class_from_partition on the 29c-2 partition → all-Gpu ⇒ plan_transfers has no inter-stage cross-class transfer.
TEST_CASE("ceir 30b-2a: stage_class_from_partition maps the 29c-2 partition to one class", "[ceir][two-class]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    register_all(ctx);
    Module* const  m   = ctx.create_module();
    Block* const   b   = mkmain(ctx, *m);
    const OpId     dcl = ctx.intern_op("resource", "declare");
    constexpr u32  rows = 4U;
    constexpr u32  dim  = 4U;
    Value* const   x  = decl(ctx, dcl, b, tf(ctx, sh2(ctx, rows, dim)));
    Value* const   w0 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dim, dim)));
    Value* const   c0 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, rows, dim)));
    Value* const   xp = gemm(ctx, b, x, w0, c0, rows, dim);
    Value* const   w1 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dim, dim)));
    Value* const   w2 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dim, dim)));
    Value*         mlpops[3] = {xp, w1, w2};
    Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mlpops, 3U), 1U, tf(ctx, sh2(ctx, rows, dim)), 0U);
    ctx.set_attr(mo, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mo);
    Value* const   w3 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, dim, dim)));
    Value* const   c3 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, rows, dim)));
    (void)gemm(ctx, b, mo->result(0U), w3, c3, rows, dim);

    // cuda_graphs (a Gpu-class launch-graph provider) claims the mlp; the flanking gemms are non-ml → fallback (-1).
    MlProvider prov;
    prov.name           = StringView("cuda_graphs");
    prov.available      = true;
    prov.advertise      = &ceg::cuda_graphs_can_claim;
    prov.provider_class = ProviderClass::Gpu;
    const MlProvider provs[1] = {prov};
    const ceg::MlPartition part = ceg::partition_ml(ctx, *m, ConstSpan<MlProvider>(provs, 1U), &alloc);
    REQUIRE(part.assignments.size() == 1U); // one ml op (the mlp); the gemms are not ml

    containers::HashMap<const Operation*, i32> lineage(&alloc);
    const ceg::MlExpandResult                  er = ceg::expand_ml_ops(ctx, *m, lineage);
    REQUIRE(er.error == ceg::MlExpandError::None);
    ceg::PlanOptions opts;
    opts.fuse_gemm_relu             = false;
    opts.share_intermediate_storage = false;
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline_partitioned(ctx, *m, &alloc, part, lineage, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 5U);

    const containers::Array<ProviderClass> sc = ceg::stage_class_from_partition(plan, ConstSpan<MlProvider>(provs, 1U), ProviderClass::Gpu, &alloc);
    REQUIRE(sc.size() == 5U);
    for (usize s = 0; s < sc.size(); ++s) { CHECK(sc[s] == ProviderClass::Gpu); } // cuda_graphs=Gpu, fallback→Gpu — ONE class

    const TransferPlan tp = ceg::plan_transfers(plan, ConstSpan<ProviderClass>(sc.data(), sc.size()), &alloc);
    CHECK(count_dir(tp, TransferDir::DeviceToHost) == 1U); // only the final Output readback — no inter-stage crossing (one class)
}

// (f) the DEDUPE + already-resident branches: one ExternalIn feeding TWO stages. gemm(a,W)→p · gemm(a,V)→q · elementwise(p,q)→out,
// so `a` is read by stages 0 AND 1 (the only shape that exercises the (buffer, writer, direction) dedupe + the resident-on-class skip).
TEST_CASE("ceir 30b-2a: plan_transfers dedupes a shared external and skips a resident buffer", "[ceir][two-class]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    register_all(ctx);
    Module* const  m   = ctx.create_module();
    Block* const   b   = mkmain(ctx, *m);
    const OpId     dcl = ctx.intern_op("resource", "declare");
    constexpr u32  d   = 4U;
    Value* const   a   = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d, d)));
    Value* const   w   = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d, d)));
    Value* const   cc0 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d, d)));
    Value* const   p   = gemm(ctx, b, a, w, cc0, d, d);
    Value* const   v   = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d, d)));
    Value* const   cc1 = decl(ctx, dcl, b, tf(ctx, sh2(ctx, d, d)));
    Value* const   q   = gemm(ctx, b, a, v, cc1, d, d);
    Operation* const ew = tensor::build_elementwise(ctx, p, q, ctx.attr_string(StringView("add")), tf(ctx, sh2(ctx, d, d)));
    b->append(ew);
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U); // [Gemm, Gemm, Elementwise]

    u32 abuf = 0;
    for (usize i = 0; i < plan.buffers.size(); ++i) { if (plan.buffers[i].value == a) { abuf = static_cast<u32>(i); } }
    const u32 pbuf = stage_out(plan, 0U); // gemm(a,W) → p
    const u32 qbuf = stage_out(plan, 1U); // gemm(a,V) → q

    SECTION("all-Gpu: the shared external `a` uploads ONCE (dedupe on writer)")
    {
        const ProviderClass sc[3] = {ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu};
        const TransferPlan  tp = ceg::plan_transfers(plan, classes(sc, 3U), &alloc);
        CHECK(has_xfer(tp, abuf, 0U, TransferDir::HostToDevice));   // stage 0 uploads `a`
        CHECK(!has_xfer(tp, abuf, 1U, TransferDir::HostToDevice));  // stage 1 reuses it — the DEDUPE (same writer, no rewrite)
        u32 a_uploads = 0;
        for (usize i = 0; i < tp.transfers.size(); ++i) { if (tp.transfers[i].buffer == abuf) { ++a_uploads; } }
        CHECK(a_uploads == 1U);
    }
    SECTION("[Gpu,Host,Gpu]: a resident buffer is not re-transferred")
    {
        const ProviderClass sc[3] = {ProviderClass::Gpu, ProviderClass::Host, ProviderClass::Gpu};
        const TransferPlan  tp = ceg::plan_transfers(plan, classes(sc, 3U), &alloc);
        CHECK(has_xfer(tp, abuf, 0U, TransferDir::HostToDevice));   // stage 0 (Gpu) uploads Host-born `a`
        CHECK(!has_xfer(tp, abuf, 1U, TransferDir::HostToDevice));  // stage 1 (Host) reads Host-born `a` — already RESIDENT
        CHECK(!has_xfer(tp, abuf, 1U, TransferDir::DeviceToHost));
        CHECK(!has_xfer(tp, pbuf, 2U, TransferDir::HostToDevice));  // p written Gpu@0, read Gpu@2 — resident, no transfer
        CHECK(has_xfer(tp, qbuf, 2U, TransferDir::HostToDevice));   // q written Host@1, read Gpu@2 — crosses
    }
}

// CEIR-30b-2b-2a — the engine `execute_two_class` runner, proven DEVICE-FREE with a MOCK device (a 2nd f32 table + a 2nd Host
// executor). This is the MECHANISM proof (slice/placement/transfer/threading); the §140 REAL-domain proof — the same plan bit-exact
// across all-Host, all-Gpu(CUDA), and a split — is 30b-2b-2b's CUDA three-way gate. The falsifiable claim: the SAME sandwich plan,
// run under any placement vector, computes the oracle bit-exact BECAUSE each class is bit-exact and the planned transfers are
// lossless — "placement is semantic, not glue" (§140). The mock device is sentinel-until-transferred, so a missing/late upload
// leaves garbage and the bit-exact fails; the transfer + device-op LOGS assert the runner drove exactly the planned work.
TEST_CASE("ceir 30b-2b-2a: execute_two_class runs a mixed Host+device plan device-free (mock), placement-driven and bit-exact", "[ceir][two-class]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    register_all(ctx);
    Module* const m = ctx.create_module();
    SandwichIO    io;
    const ceg::TensorPipelinePlan plan = build_sandwich(ctx, *m, &alloc, &io); // [Gemm,Gemm,VizDispatch(relu),Gemm,Gemm]
    SandwichData d;
    fill_sandwich(d);
    const Seed seeds[5] = {{io.x, d.x, 16U}, {io.w0, d.w0, 16U}, {io.w1, d.w1, 16U}, {io.w2, d.w2, 16U}, {io.w3, d.w3, 16U}};

    SECTION("[Gpu,Host,Host,Host,Gpu]: bit-exact; drives exactly the planned transfers + the 2 device stages; full profile")
    {
        const ProviderClass sc[5] = {ProviderClass::Gpu, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Gpu};
        MockDevice          md(&alloc);
        md.ctx              = &ctx;
        md.host_opts.kernel = &crd::tests::resolve_ckir_asset; // the relu VizDispatch (stage 2, Host here) loads relu.ckir
        md.host_opts.user   = &alloc;
        TensorPipelineProfile profile(&alloc);
        f32                   got[16];
        const ExecuteError    ee = run_two_class_mock(md, plan, classes(sc, 5U), seeds, 5U, got, 16U, -777.0F, &profile);
        REQUIRE(ee == ExecuteError::None);
        for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == d.oracle[i]); } // BIT-EXACT vs the CPU oracle (the placement changed nothing)

        // the log == plan_transfers, by identity + in order (execute_two_class applies before_stage ascending, tp order within):
        const TransferPlan tp = ceg::plan_transfers(plan, classes(sc, 5U), &alloc);
        REQUIRE(md.xfer_log.size() == tp.transfers.size());
        for (usize i = 0; i < tp.transfers.size(); ++i)
        {
            CHECK(md.xfer_log[i].buffer == tp.transfers[i].buffer);
            CHECK(md.xfer_log[i].before_stage == tp.transfers[i].before_stage);
            CHECK(md.xfer_log[i].direction == tp.transfers[i].direction);
        }
        // exactly the two device stages (0 and 4), in order:
        REQUIRE(md.gpu_calls == 2U);
        REQUIRE(md.gpu_ops.size() == 2U);
        CHECK(md.gpu_ops[0] == plan.stages[0].op);
        CHECK(md.gpu_ops[1] == plan.stages[4].op);
        // one profile row per stage, in stage order (Host + device alike) — the FULL kind sequence, so a swapped stage is caught:
        REQUIRE(profile.stages.size() == 5U);
        const ceg::StageKind want[5] = {ceg::StageKind::Gemm, ceg::StageKind::Gemm, ceg::StageKind::VizDispatch,
                                        ceg::StageKind::Gemm, ceg::StageKind::Gemm};
        for (usize i = 0; i < 5U; ++i) { CHECK(profile.stages[i].kind == want[i]); }
    }
    SECTION("reverse [Host,Gpu,Gpu,Gpu,Host]: bit-exact; the Output ends on Host (no final readback logged)")
    {
        const ProviderClass sc[5] = {ProviderClass::Host, ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Host};
        MockDevice          md(&alloc);
        md.ctx              = &ctx;
        md.host_opts.kernel = &crd::tests::resolve_ckir_asset; // the relu is a device (mock) stage here → the mock runner needs it
        md.host_opts.user   = &alloc;
        f32                got[16];
        const ExecuteError ee = run_two_class_mock(md, plan, classes(sc, 5U), seeds, 5U, got, 16U, -777.0F);
        REQUIRE(ee == ExecuteError::None);
        for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == d.oracle[i]); }
        REQUIRE(md.gpu_calls == 3U); // stages 1,2,3 on the device
        // the runner drove the FLIPPED transfer set (xp H→D@1, y D→H@4, NO readback — the Output ends Host), by identity:
        const TransferPlan tp = ceg::plan_transfers(plan, classes(sc, 5U), &alloc);
        REQUIRE(md.xfer_log.size() == tp.transfers.size());
        for (usize i = 0; i < tp.transfers.size(); ++i)
        {
            CHECK(md.xfer_log[i].buffer == tp.transfers[i].buffer);
            CHECK(md.xfer_log[i].before_stage == tp.transfers[i].before_stage);
            CHECK(md.xfer_log[i].direction == tp.transfers[i].direction);
        }
        CHECK(count_before(tp, static_cast<u32>(plan.stages.size())) == 0U); // Output on Host ⇒ no final readback planned
    }
    SECTION("all-Host: bit-exact, ZERO transfers, ZERO device calls")
    {
        const ProviderClass sc[5] = {ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host};
        MockDevice          md(&alloc);
        md.ctx              = &ctx;
        md.host_opts.kernel = &crd::tests::resolve_ckir_asset;
        md.host_opts.user   = &alloc;
        f32                got[16];
        const ExecuteError ee = run_two_class_mock(md, plan, classes(sc, 5U), seeds, 5U, got, 16U, -777.0F);
        REQUIRE(ee == ExecuteError::None);
        for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == d.oracle[i]); }
        CHECK(md.gpu_calls == 0U);
        CHECK(md.xfer_log.size() == 0U);
    }
    SECTION("all-Gpu: bit-exact, all 5 stages on the device")
    {
        const ProviderClass sc[5] = {ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu, ProviderClass::Gpu};
        MockDevice          md(&alloc);
        md.ctx              = &ctx;
        md.host_opts.kernel = &crd::tests::resolve_ckir_asset;
        md.host_opts.user   = &alloc;
        f32                got[16];
        const ExecuteError ee = run_two_class_mock(md, plan, classes(sc, 5U), seeds, 5U, got, 16U, -777.0F);
        REQUIRE(ee == ExecuteError::None);
        for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == d.oracle[i]); }
        CHECK(md.gpu_calls == 5U);
    }
    SECTION("arity mismatch (4 classes for a 5-stage plan) → BindingArity; nothing ran")
    {
        const ProviderClass sc[4] = {ProviderClass::Gpu, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host};
        MockDevice          md(&alloc);
        md.ctx              = &ctx;
        md.host_opts.kernel = &crd::tests::resolve_ckir_asset;
        md.host_opts.user   = &alloc;
        f32                got[16];
        const ExecuteError ee = run_two_class_mock(md, plan, classes(sc, 4U), seeds, 5U, got, 16U, -777.0F);
        CHECK(ee == ExecuteError::BindingArity);
        CHECK(md.gpu_calls == 0U);
        CHECK(md.xfer_log.size() == 0U);
        for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == -777.0F); } // the Output was never touched
    }
    SECTION("host_opts.kernel=nullptr → the Host relu is UnresolvedKernel; the Gpu prefix (stage 0) ran, then abort")
    {
        const ProviderClass sc[5] = {ProviderClass::Gpu, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Gpu};
        MockDevice          md(&alloc);
        md.ctx = &ctx; // host_opts.kernel LEFT nullptr — the threading proof: the Host relu can't resolve without it
        f32                got[16];
        const ExecuteError ee = run_two_class_mock(md, plan, classes(sc, 5U), seeds, 5U, got, 16U, -777.0F);
        CHECK(ee == ExecuteError::UnresolvedKernel);
        CHECK(md.gpu_calls == 1U); // stage 0 (Gpu gemm — no kernel) ran; stage 2 (Host relu) needs the resolver → abort
        for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == -777.0F); }
    }
    SECTION("a device-stage error propagates; nothing after it runs")
    {
        const ProviderClass sc[5] = {ProviderClass::Gpu, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Gpu};
        MockDevice          md(&alloc);
        md.ctx              = &ctx;
        md.host_opts.kernel = &crd::tests::resolve_ckir_asset;
        md.host_opts.user   = &alloc;
        md.gpu_err          = ExecuteError::UnmappedBinding; // the device stage fails
        f32                got[16];
        const ExecuteError ee = run_two_class_mock(md, plan, classes(sc, 5U), seeds, 5U, got, 16U, -777.0F);
        CHECK(ee == ExecuteError::UnmappedBinding);
        CHECK(md.gpu_calls == 1U); // stage 0 was called, returned the injected error → the runner aborts
        for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == -777.0F); }
    }
    SECTION("a transfer error propagates before any stage runs")
    {
        const ProviderClass sc[5] = {ProviderClass::Gpu, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Gpu};
        MockDevice          md(&alloc);
        md.ctx              = &ctx;
        md.host_opts.kernel = &crd::tests::resolve_ckir_asset;
        md.host_opts.user   = &alloc;
        md.xfer_err         = ExecuteError::UnmappedBinding; // the first upload (before stage 0) fails
        f32                got[16];
        const ExecuteError ee = run_two_class_mock(md, plan, classes(sc, 5U), seeds, 5U, got, 16U, -777.0F);
        CHECK(ee == ExecuteError::UnmappedBinding);
        CHECK(md.gpu_calls == 0U); // no stage ran
        for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == -777.0F); }
    }
}

// CEIR-30b-2b-2a — the runner honours ALIASED (shared) intermediate storage: with share_intermediate_storage=true the sandwich's
// intermediates alias (the 29c-2 landlord path), and this is the ONLY place the landlord plumbing runs end-to-end THROUGH execution
// (host table + mock device table both aliased to their landlord). Still bit-exact — the aliasing is lifetime-safe by construction.
TEST_CASE("ceir 30b-2b-2a: the two-class runner honours aliased (shared) intermediate storage", "[ceir][two-class]")
{
    memory::GrowableTlsfAllocator alloc(1U << 20U);
    Context                       ctx(&alloc);
    register_all(ctx);
    Module* const m = ctx.create_module();
    SandwichIO    io;
    const ceg::TensorPipelinePlan plan = build_sandwich(ctx, *m, &alloc, &io, /*share=*/true);
    SandwichData d;
    fill_sandwich(d);
    const Seed seeds[5] = {{io.x, d.x, 16U}, {io.w0, d.w0, 16U}, {io.w1, d.w1, 16U}, {io.w2, d.w2, 16U}, {io.w3, d.w3, 16U}};

    u32 aliases = 0;
    for (usize i = 0; i < plan.buffers.size(); ++i) { if (plan.buffers[i].alias_of >= 0) { ++aliases; } }
    REQUIRE(aliases >= 1U); // share=true actually produced at least one alias (else this would not exercise the landlord path)

    const ProviderClass sc[5] = {ProviderClass::Gpu, ProviderClass::Host, ProviderClass::Host, ProviderClass::Host, ProviderClass::Gpu};
    MockDevice          md(&alloc);
    md.ctx              = &ctx;
    md.host_opts.kernel = &crd::tests::resolve_ckir_asset;
    md.host_opts.user   = &alloc;
    f32                got[16];
    const ExecuteError ee = run_two_class_mock(md, plan, classes(sc, 5U), seeds, 5U, got, 16U, -777.0F);
    REQUIRE(ee == ExecuteError::None);
    for (u32 i = 0; i < 16U; ++i) { CHECK(got[i] == d.oracle[i]); } // aliased storage, still bit-exact

    // runner-side contract: EVERY planned transfer names a LANDLORD, never an alias index (plan_transfers resolves aliases).
    for (usize i = 0; i < md.xfer_log.size(); ++i) { CHECK(plan.buffers[static_cast<usize>(md.xfer_log[i].buffer)].alias_of < 0); }
    // and at least one boundary-crossing buffer is the LANDLORD OF AN ALIAS — i.e. the landlord plumbing ran THROUGH a transfer,
    // not only inside the Host run (else this fixture doesn't exercise the claim and the assertion becomes a row note).
    u32 landlord_crossings = 0;
    for (usize i = 0; i < md.xfer_log.size(); ++i)
    {
        const u32 lb = md.xfer_log[i].buffer;
        for (usize j = 0; j < plan.buffers.size(); ++j)
        {
            if (plan.buffers[j].alias_of == static_cast<i32>(lb)) { ++landlord_crossings; break; }
        }
    }
    CHECK(landlord_crossings >= 1U);
}
