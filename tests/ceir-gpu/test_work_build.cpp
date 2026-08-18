// CEIR-20b step 2: build_work_ceir — the work-chain builder emits a find_work_misuse-CLEAN ceir.work program
// (queue_alloc + produce/consume/compact) for the wavefront mapping (trace=produce, compact=compact, shade=consume),
// and REJECTS malformed descs (a bad queue index, an empty kernel). The lowering to a LoweredCommand plan is CEIR-20b
// step 3 — this gates the EMISSION + VERIFY half (the build_fullscreen_ceir find_render_misuse gate, mirrored for
// ceir.work).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gpu/execute.hpp> // validate_lowered / validate_rt_lowered / ExecuteError / RtHooks (the typed-reject audit)
#include <crd/ceir/gpu/lower.hpp> // LoweredCommand / LoweredKind
#include <crd/ceir/gpu/work_build.hpp>
#include <crd/ceir/gpu/work_graph.hpp> // CEIR-20c-1c: build_work_graph_plan (the desc -> Work Graph topology)
#include <crd/ceir/work.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
// ⛔ crd::gpu (via execute.hpp → compute.hpp) AND crd::ceir::gpu are BOTH in scope here — a bare `cg::` is ambiguous.
// Alias it.
namespace cg = crd::ceir::gpu;
using cg::WorkAccess;
using cg::WorkBuildDesc;
using cg::WorkStageKind;

namespace
{
// The wavefront mapping (advisor design-lock): 2 device-resident queues (ray_queue COUNTER + hit_queue STRUCTURED);
// trace = produce (a const grid appending into ray_queue, binds `rays` r), compact = compact (ray_queue -> hit_queue,
// binds a predicate w), shade = consume (an INDIRECT dispatch over hit_queue's DEVICE count, binds `decisions` w). All
// 4 ops exercised.
void build_wavefront_desc(WorkBuildDesc& d)
{
    d.num_queues = 2U;
    d.queues[0].capacity = 64U;
    d.queues[0].record_stride = 24U;
    d.queues[0].source_param = 0x100U; // ray_queue
    d.queues[1].capacity = 64U;
    d.queues[1].record_stride = 4U;
    d.queues[1].source_param = 0x101U; // hit_queue

    d.num_stages = 3U;

    // trace = work.produce (grid 1x1x1 over ray_queue; binds rays r)
    d.stages[0].kind = WorkStageKind::Produce;
    d.stages[0].kernel = containers::StringView("wavefront_trace");
    d.stages[0].queue = 0U;
    d.stages[0].num_bindings = 1U;
    d.stages[0].bindings[0].source_param = 0x200U;
    d.stages[0].bindings[0].access = WorkAccess::Read;

    // compact = work.compact (ray_queue -> hit_queue; binds a predicate w)
    d.stages[1].kind = WorkStageKind::Compact;
    d.stages[1].kernel = containers::StringView("wavefront_compact");
    d.stages[1].src_queue = 0U;
    d.stages[1].queue = 1U;
    d.stages[1].num_bindings = 1U;
    d.stages[1].bindings[0].source_param = 0x201U;
    d.stages[1].bindings[0].access = WorkAccess::Write;

    // shade = work.consume (INDIRECT over hit_queue's device count; binds decisions w)
    d.stages[2].kind = WorkStageKind::Consume;
    d.stages[2].kernel = containers::StringView("wavefront_shade");
    d.stages[2].queue = 1U;
    d.stages[2].num_bindings = 1U;
    d.stages[2].bindings[0].source_param = 0x202U;
    d.stages[2].bindings[0].access = WorkAccess::Write;
}

// ── the host-only WorkHooks stub: records WHICH dispatch hook (direct = produce, indirect = consume/compact) ran per
// stage + the const grid a direct dispatch got (function pointers + a `user` capture, the KernelResolveFn precedent —
// no device). The indirect hook takes NO host grid: the DEVICE reads the queue count (advisor design-lock — sizing is
// device-driven, never host).
struct WorkStubCapture
{
    crd::u8 order[8] = {}; // 0 = direct (produce), 1 = indirect (consume/compact), in program order
    crd::u32 n = 0U;
    crd::u32 direct_gx[8] = {};           // the const gx each DIRECT dispatch received
    crd::u32 nhandles[8] = {};            // the descriptor count each stage got (queue + bindings)
    cg::WorkBufferHandle queue_h[8] = {}; // the queue handle each INDIRECT stage got (the device-count buffer)
    crd::u32 num_direct = 0U;
    crd::u32 num_indirect = 0U;
    crd::u32 num_barriers = 0U; // total barrier-hook calls (the replayed inter-stage hazards + the C5 pre-indirect one)
    crd::u32 num_ind_read = 0U; // barriers with to==IndirectRead — the executor-owned pre-indirect queue barrier
    crd::u8 kbytes = 1U;        // a sentinel non-empty kernel blob
};
containers::ConstSpan<crd::u8> stub_kernel_bytes(const Operation* /*op*/, void* user)
{
    auto* const c = static_cast<WorkStubCapture*>(user);
    return containers::ConstSpan<crd::u8>(&c->kbytes, 1U);
}
bool stub_dispatch(const Operation* /*op*/, containers::ConstSpan<crd::u8> /*kb*/,
                   containers::ConstSpan<cg::WorkBufferHandle> handles, crd::u32 gx, crd::u32 /*gy*/, crd::u32 /*gz*/,
                   void* user)
{
    auto* const c = static_cast<WorkStubCapture*>(user);
    if (c->n < 8U)
    {
        c->order[c->n] = 0U;
        c->direct_gx[c->n] = gx;
        c->nhandles[c->n] = static_cast<crd::u32>(handles.size()); // the queue + bindings are all descriptors
        c->n++;
    }
    c->num_direct++;
    return true;
}
bool stub_dispatch_indirect(const Operation* /*op*/, cg::WorkBufferHandle queue, containers::ConstSpan<crd::u8> /*kb*/,
                            containers::ConstSpan<cg::WorkBufferHandle> handles, void* user)
{
    auto* const c = static_cast<WorkStubCapture*>(user);
    if (c->n < 8U)
    {
        c->order[c->n] = 1U;
        c->nhandles[c->n] = static_cast<crd::u32>(handles.size());
        c->queue_h[c->n] = queue; // the device-count buffer handle (the first descriptor)
        c->n++;
    }
    c->num_indirect++;
    return true;
}
bool stub_barrier(cg::WorkBufferHandle /*buffer*/, crd::gpu::ComputeAccess /*from*/, crd::gpu::ComputeAccess to,
                  void* user)
{
    auto* const c = static_cast<WorkStubCapture*>(user);
    c->num_barriers++;
    if (to == crd::gpu::ComputeAccess::IndirectRead)
    {
        c->num_ind_read++;
    } // the executor-owned pre-indirect queue barrier
    return true;
}
} // namespace

// CEIR-20c-1c: the D3D12 Work Graph topology is DERIVED from the authored ceir.work desc (the same WorkBuildDesc the 20b
// compute-indirect fallback consumes) — one semantic program, two lowerings. produce = the entry that emits a grid-launch
// record; the consume reading the same queue is its downstream.
TEST_CASE("ceir 20c-1: build_work_graph_plan derives the produce->consume topology from a ceir.work desc",
          "[ceirgpu][work][ceir20c]")
{
    WorkBuildDesc d;
    d.num_queues              = 1U;
    d.queues[0].capacity      = 64U;
    d.queues[0].record_stride = 4U;
    d.queues[0].source_param  = 0xAAAAU;
    d.num_stages              = 2U;
    d.stages[0].kind          = WorkStageKind::Produce;
    d.stages[0].kernel        = containers::StringView("work_smoke_produce");
    d.stages[0].queue         = 0U;
    d.stages[1].kind          = WorkStageKind::Consume;
    d.stages[1].kernel        = containers::StringView("work_smoke_consume");
    d.stages[1].queue         = 0U;

    cg::WorkGraphPlan plan;
    REQUIRE(cg::build_work_graph_plan(d, plan));
    CHECK(plan.num_nodes == 2U);
    CHECK(plan.entry == 0U);                                                     // the producer is the program entry
    CHECK(plan.nodes[0].role == WorkStageKind::Produce);
    CHECK(plan.nodes[0].downstream == 1U);                                       // produce -> consume (shared queue)
    CHECK(plan.nodes[0].kernel == containers::StringView("work_smoke_produce"));
    CHECK(plan.nodes[1].role == WorkStageKind::Consume);
    CHECK(plan.nodes[1].downstream == cg::kNoWorkGraphNode);

    // two producers, no consumer ⇒ != 1 entry ⇒ reject (the multi-entry / compact-fed shape is ledgered).
    WorkBuildDesc     bad = d;
    bad.stages[1].kind    = WorkStageKind::Produce;
    cg::WorkGraphPlan p2;
    CHECK(!cg::build_work_graph_plan(bad, p2));
}

TEST_CASE("ceir 20b: build_work_ceir emits a find_work_misuse-clean wavefront program + lowers it",
          "[ceirgpu][work][ceir20b]")
{
    memory::GrowableTlsfAllocator alloc;
    Context ctx(&alloc);
    WorkBuildDesc d;
    build_wavefront_desc(d);
    containers::Array<cg::LoweredCommand> plan(&alloc);
    cg::WorkAssetResources res;

    Module* const m = cg::build_work_ceir(ctx, d, plan, res);
    REQUIRE(m != nullptr);                                                     // emitted + verified (verifier-first)
    CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::None); // re-verifies clean
    CHECK(res.count ==
          5U); // the caller's table: 2 queue Values + 3 binding declares (one per stage) exposed with source_params

    // the lowered plan: produce → Dispatch, compact + consume → DispatchIndirect, queue_alloc → nothing. Barriers may
    // be interleaved (conservative whole-Memory) — filter to the dispatch commands + assert their kinds in program
    // order.
    cg::LoweredKind dispatches[8] = {};
    crd::u32 nd = 0U;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(plan.size()); ++i)
    {
        if (plan[i].kind == cg::LoweredKind::Dispatch || plan[i].kind == cg::LoweredKind::DispatchIndirect)
        {
            dispatches[nd++] = plan[i].kind;
        }
    }
    REQUIRE(nd == 3U);                                 // trace + compact + shade (queue_allocs emit nothing)
    CHECK(dispatches[0] == cg::LoweredKind::Dispatch); // trace = produce (const grid)
    CHECK(dispatches[1] ==
          cg::LoweredKind::Dispatch); // compact = the SERIAL scan (grid 1,1,1 direct — NOT device-count)
    CHECK(dispatches[2] ==
          cg::LoweredKind::DispatchIndirect); // shade = consume (the one device-count INDIRECT dispatch)
}

TEST_CASE("ceir 20b: build_work_ceir rejects a malformed desc", "[ceirgpu][work][ceir20b]")
{
    memory::GrowableTlsfAllocator alloc;
    containers::Array<cg::LoweredCommand> plan(&alloc);
    cg::WorkAssetResources res;

    SECTION("a stage naming a queue index out of range")
    {
        Context ctx(&alloc);
        WorkBuildDesc d;
        build_wavefront_desc(d);
        d.stages[2].queue = 9U; // >= num_queues
        CHECK(cg::build_work_ceir(ctx, d, plan, res) == nullptr);
        CHECK(plan.size() == 0U); // out_plan cleared on rejection
    }
    SECTION("a compact stage naming a src queue out of range")
    {
        Context ctx(&alloc);
        WorkBuildDesc d;
        build_wavefront_desc(d);
        d.stages[1].src_queue = 9U; // >= num_queues
        CHECK(cg::build_work_ceir(ctx, d, plan, res) == nullptr);
    }
    SECTION("an empty kernel symbol")
    {
        Context ctx(&alloc);
        WorkBuildDesc d;
        build_wavefront_desc(d);
        d.stages[0].kernel = containers::StringView();
        CHECK(cg::build_work_ceir(ctx, d, plan, res) == nullptr);
    }
}

// CEIR-20b step 3: the widen-enum audit — a DispatchIndirect command targets execute_work_lowered (step 4); every OTHER
// executor REJECTS it TYPED (UnsupportedCommand). validate_lowered (the compute surface) + validate_rt_lowered (the RT
// surface) are host-only; the render surface's rejection is compile-enforced (the exhaustive render_materialize switch
// + -Werror=switch).
TEST_CASE("ceir 20b: a DispatchIndirect command is typed-rejected by the compute + RT executors",
          "[ceirgpu][work][ceir20b]")
{
    memory::GrowableTlsfAllocator alloc;
    Context ctx(&alloc);

    cg::LoweredCommand cmd;
    cmd.kind = cg::LoweredKind::DispatchIndirect;
    const containers::ConstSpan<cg::LoweredCommand> cmds(&cmd, 1U);

    CHECK(cg::validate_lowered(ctx, cmds, nullptr, nullptr, {}) == cg::ExecuteError::UnsupportedCommand);
    const cg::RtHooks hooks;
    CHECK(cg::validate_rt_lowered(ctx, cmds, hooks, {}) == cg::ExecuteError::UnsupportedCommand);
}

// CEIR-20b step 4: execute_work_lowered is the GENERIC host driver — a Dispatch (produce) → the DIRECT hook with its
// const grid; a DispatchIndirect (consume/compact) → the INDIRECT hook (the DEVICE reads the queue count — no host
// grid). Host-only via a stub that records which hook ran per stage (the on-device smoke + the full wavefront
// decision-hash gate come with backend wiring).
TEST_CASE("ceir 20b: execute_work_lowered routes produce->direct and consume/compact->indirect (device-driven sizing)",
          "[ceirgpu][work][ceir20b]")
{
    memory::GrowableTlsfAllocator alloc;
    Context ctx(&alloc);
    WorkBuildDesc d;
    build_wavefront_desc(d);
    for (crd::u32 s = 0; s < d.num_stages; ++s)
    {
        d.stages[s].num_bindings = 0U;
    } // the driver only (no binding resolution)

    containers::Array<cg::LoweredCommand> plan(&alloc);
    cg::WorkAssetResources res;
    REQUIRE(cg::build_work_ceir(ctx, d, plan, res) != nullptr);
    REQUIRE(res.count == 2U); // the 2 queue Values (num_bindings=0 here — the queues are the only descriptors)

    // the caller's WorkBinding table: each exposed Value → a NAMED device-buffer handle (the source_param stands in as
    // the handle).
    containers::Array<cg::WorkResolvedBinding> table(&alloc);
    for (crd::u32 e = 0; e < res.count; ++e)
    {
        table.push_back(cg::WorkResolvedBinding{res.entries[e].value, res.entries[e].source_param});
    }

    WorkStubCapture cap;
    cg::WorkHooks hooks;
    hooks.kernel_bytes = &stub_kernel_bytes;
    hooks.dispatch = &stub_dispatch;
    hooks.dispatch_indirect = &stub_dispatch_indirect;
    hooks.barrier = &stub_barrier;
    hooks.user = &cap;

    const cg::ExecuteError err =
        cg::execute_work_lowered(ctx, containers::ConstSpan<cg::LoweredCommand>(plan.data(), plan.size()), hooks,
                                 containers::ConstSpan<cg::WorkResolvedBinding>(table.data(), table.size()));
    CHECK(err == cg::ExecuteError::None);
    REQUIRE(cap.n == 3U);            // trace + compact + shade (queue_allocs run no dispatch)
    CHECK(cap.order[0] == 0U);       // trace = produce → DIRECT (const grid)
    CHECK(cap.order[1] == 0U);       // compact = the SERIAL scan → DIRECT (grid 1,1,1)
    CHECK(cap.order[2] == 1U);       // shade = consume → INDIRECT (the one device-count dispatch)
    CHECK(cap.direct_gx[0] == 1U);   // produce's authored const grid (1,1,1)
    CHECK(cap.direct_gx[1] == 1U);   // compact's serial grid (1,1,1 default — no grid operands)
    CHECK(cap.nhandles[0] == 1U);    // produce descriptors: the queue it appends into (ray_queue) — no other bindings
    CHECK(cap.nhandles[1] == 2U);    // compact descriptors: src (ray_queue) + dst (hit_queue)
    CHECK(cap.nhandles[2] == 1U);    // consume descriptors: the queue (hit_queue)
    CHECK(cap.queue_h[2] == 0x101U); // ⭐ consume's DEVICE-COUNT buffer = the queue (hit_queue) handle
    CHECK(cap.num_direct == 2U);     // produce + compact
    CHECK(cap.num_indirect == 1U);   // consume only
    CHECK(cap.num_ind_read ==
          1U); // ⭐ the executor-owned ShaderWrite→IndirectRead barrier fires once — only for the consume
    CHECK(cap.num_barriers >=
          1U); // + the REPLAYED inter-stage whole-Memory hazards (device-resident: barriers are NOT inert)
}

// CEIR-20b step 4: validate_work_lowered is structure-only (the validate_rt_lowered mirror) — a foreign kind is
// UnsupportedCommand; a work plan whose kernel does not resolve is UnresolvedKernel.
TEST_CASE("ceir 20b: validate_work_lowered rejects a foreign kind + an unresolved kernel", "[ceirgpu][work][ceir20b]")
{
    memory::GrowableTlsfAllocator alloc;
    Context ctx(&alloc);

    SECTION("a foreign (Transfer) command")
    {
        cg::LoweredCommand foreign;
        foreign.kind = cg::LoweredKind::Transfer;
        const cg::WorkHooks empty;
        CHECK(cg::validate_work_lowered(ctx, containers::ConstSpan<cg::LoweredCommand>(&foreign, 1U), empty, {}) ==
              cg::ExecuteError::UnsupportedCommand);
    }
    SECTION("a work plan whose kernel_bytes hook is null → UnresolvedKernel")
    {
        WorkBuildDesc d;
        build_wavefront_desc(d);
        for (crd::u32 s = 0; s < d.num_stages; ++s)
        {
            d.stages[s].num_bindings = 0U;
        }
        containers::Array<cg::LoweredCommand> plan(&alloc);
        cg::WorkAssetResources res;
        REQUIRE(cg::build_work_ceir(ctx, d, plan, res) != nullptr);
        const cg::WorkHooks nokernel; // kernel_bytes null → empty span → UnresolvedKernel
        CHECK(cg::validate_work_lowered(ctx, containers::ConstSpan<cg::LoweredCommand>(plan.data(), plan.size()),
                                        nokernel, {}) == cg::ExecuteError::UnresolvedKernel);
    }
}
