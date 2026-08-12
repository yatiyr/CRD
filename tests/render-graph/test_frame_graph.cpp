// RAF-7 Gate 7 — the unified frame-graph runtime (device-free, via a command-capturing mock encoder).
//
// Gates (mission §7 · D-007 RAF-7): hand-built == authored (same recorded commands); multiple packets in one scope;
// declared use matches recorded (undeclared diagnosed); transient aliasing correct; persistent/history survives;
// resize recompiles only what's needed (schedule + slots unchanged); one submission (a single coherent stream).
//
// The live-backend wiring (real GPU transients, and thereby the 4 frame-graph-shaped GPU kinds) is RAF-7's device
// integration on top of this architecture — this suite proves the architecture with no device.
//
// ⛔ named allocator throughout; ASCII-only test names.

#include <crd/rendergraph/frame_graph.hpp>

#include <crd/ceir/gpu/render_fullscreen_build.hpp> // CEIR-16-3c-2: build a fullscreen plan for the replay executor
#include <crd/containers/string.hpp>
#include <crd/gpu/command_model.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/renderpass/executor_registry.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::u32;
using crd::u64;
using crd::containers::String;
using namespace crd::rendergraph;
using crd::gpu::DispatchDesc;
using crd::gpu::IRasterTarget;
using crd::gpu::RasterDrawPacket;
using crd::gpu::RenderingDesc;
using crd::gpu::TraceDesc;
using crd::gpu::TransferDesc;
using crd::renderpass::ExecutorRegistry;
using crd::renderpass::executor_type_id;
using crd::renderpass::pass_param_id;
using crd::renderpass::QueueKind;
using crd::renderpass::register_builtin_executors;
using crd::renderpass::ResourceRef;
using crd::renderpass::SlotAccess;
using crd::renderpass::SlotResourceKind;

namespace
{
// A command-capturing encoder: records the op sequence as a string (B=begin, D=draw, E=end, C=dispatch, T=transfer,
// R=trace) plus scope bookkeeping (max draws seen inside one begin/end).
class MockEncoder final : public crd::gpu::ICommandEncoder
{
public:
    explicit MockEncoder(crd::memory::IAllocator* alloc) noexcept : ops(alloc) {}
    void begin_rendering(const RenderingDesc&) override
    {
        ops.push_back('B');
        m_in_scope = true;
        m_draws_this_scope = 0;
        ++begin_count;
    }
    void draw(const RasterDrawPacket&) override
    {
        ops.push_back('D');
        ++draw_count;
        ++m_draws_this_scope;
        if (m_draws_this_scope > max_draws_in_scope)
        {
            max_draws_in_scope = m_draws_this_scope;
        }
    }
    void end_rendering() override
    {
        ops.push_back('E');
        m_in_scope = false;
        ++end_count;
    }
    void dispatch(const DispatchDesc&) override
    {
        ops.push_back('C');
        ++dispatch_count;
    }
    void transfer(const TransferDesc&) override
    {
        ops.push_back('T');
        ++transfer_count;
    }
    void trace_rays(const TraceDesc&) override
    {
        ops.push_back('R');
        ++trace_count;
    }

    String ops;
    int begin_count = 0;
    int draw_count = 0;
    int end_count = 0;
    int dispatch_count = 0;
    int transfer_count = 0;
    int trace_count = 0;
    int max_draws_in_scope = 0;

private:
    bool m_in_scope = false;
    int m_draws_this_scope = 0;
};

class FakeTarget final : public IRasterTarget
{
public:
    FakeTarget(u32 w, u32 h) noexcept : m_w(w), m_h(h) {}
    [[nodiscard]] u32 width() const noexcept override { return m_w; }
    [[nodiscard]] u32 height() const noexcept override { return m_h; }
    [[nodiscard]] u32 read_pixel(u32, u32) const noexcept override { return 0U; }

private:
    u32 m_w;
    u32 m_h;
};

// A minimal sampled texture for the device-free record gates: a fullscreen pass that DECLARES an input read resolves
// it to a real ITexture (a colour, so the executor takes the plain sampled path, not the shadow comparison sampler).
class FakeTexture final : public crd::gpu::ITexture
{
public:
    FakeTexture(u32 w, u32 h) noexcept : m_w(w), m_h(h) {}
    [[nodiscard]] u32 width() const noexcept override { return m_w; }
    [[nodiscard]] u32 height() const noexcept override { return m_h; }

private:
    u32 m_w;
    u32 m_h;
};

int g_sentinel = 0;
crd::gpu::IRasterProgram* fake_raster() noexcept { return reinterpret_cast<crd::gpu::IRasterProgram*>(&g_sentinel); }

// A fullscreen.raster pass writing `color_res`, optionally reading `input_res` (0 = none).
GraphPass fullscreen_pass(u64 name, u64 color_res, u64 input_res)
{
    GraphPass p;
    p.name_hash = name;
    p.payload.executor = executor_type_id("fullscreen.raster");
    p.payload.schema_version = 1U;
    p.payload.queue = QueueKind::Graphics;
    p.payload.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, color_res});
    if (input_res != 0U)
    {
        p.payload.resources.push_back(ResourceRef{pass_param_id("input0"), SlotResourceKind::Texture, SlotAccess::Read, input_res});
    }
    return p;
}

GraphResource color_res(u64 name, crd::rendergraph::ResourceLifetime life = ResourceLifetime::Transient)
{
    return GraphResource{name, SlotResourceKind::ColorTarget, life, /*size_class*/ 0xFULL};
}

PassPrograms fake_programs() noexcept
{
    PassPrograms pr;
    pr.raster = fake_raster();
    return pr;
}

// ⭐ CEIR-16-mesh-1 descriptor-parity gate (migration doc §6): the migration's WHOLE output is the RenderingDesc + the
// RasterDrawPacket (the encoder downstream is untouched, identically fed), so two record paths compare field-by-field
// WITHOUT a device — pixels would add nothing a device could break. This encoder captures those fields.
struct CapturedDraw
{
    u32                         rd_w = 0U, rd_h = 0U;
    bool                        has_color = false, has_depth = false;
    crd::gpu::LoadOp            load{};
    crd::gpu::StoreOp           store{};
    crd::gpu::BlendMode         blend{};
    float                       cr = -1.0F, cg = -1.0F, cb = -1.0F, ca = -1.0F;
    crd::gpu::RasterCommandKind cmd{};
    crd::gpu::GeometryKind      geo{};
    crd::gpu::IStorageBuffer*   args     = nullptr;
    u64                         args_off = 0U;
    crd::gpu::IRasterProgram*   prog     = nullptr;
    crd::usize                  nbind    = 0U;
    int                         draws    = 0;
};
class CaptureEncoder final : public crd::gpu::ICommandEncoder
{
public:
    CapturedDraw cap;
    void         begin_rendering(const RenderingDesc& rd) override
    {
        cap.rd_w      = rd.width;
        cap.rd_h      = rd.height;
        cap.has_color = rd.color.size() > 0U;
        cap.has_depth = rd.depth.enabled;
        if (cap.has_color)
        {
            cap.load  = rd.color[0].load;
            cap.store = rd.color[0].store;
            cap.blend = rd.color[0].blend;
            cap.cr    = rd.color[0].clear.r;
            cap.cg    = rd.color[0].clear.g;
            cap.cb    = rd.color[0].clear.b;
            cap.ca    = rd.color[0].clear.a;
        }
    }
    void draw(const RasterDrawPacket& p) override
    {
        ++cap.draws;
        cap.cmd      = p.command;
        cap.geo      = p.geometry.kind;
        cap.args     = p.geometry.args_buffer;
        cap.args_off = p.geometry.args_offset;
        cap.prog     = p.program;
        cap.nbind    = p.bindings.size();
    }
    void end_rendering() override {}
    void dispatch(const DispatchDesc&) override {}
    void transfer(const TransferDesc&) override {}
    void trace_rays(const TraceDesc&) override {}
};
crd::renderpass::TypedValue tv_u32(u32 v)
{
    crd::renderpass::TypedValue t;
    t.type = crd::renderpass::ExecutorParamType::U32;
    t.u    = v;
    return t;
}
crd::renderpass::TypedValue tv_vec4(crd::f32 r, crd::f32 g, crd::f32 b, crd::f32 a)
{
    crd::renderpass::TypedValue t;
    t.type  = crd::renderpass::ExecutorParamType::Vec4;
    t.v4[0] = r;
    t.v4[1] = g;
    t.v4[2] = b;
    t.v4[3] = a;
    return t;
}
// CEIR-16-mesh-2: captures EVERY draw's (command, amplify count, program, binding count) — the amplify pass emits N draws in
// ONE scope, so the last-only CaptureEncoder cannot check the per-item expansion parity.
struct AmpParityEncoder final : public crd::gpu::ICommandEncoder
{
    int                                                begins = 0, ends = 0;
    crd::containers::Array<crd::gpu::RasterCommandKind> cmds;
    crd::containers::Array<u32>                         counts;
    crd::containers::Array<crd::gpu::IRasterProgram*>   progs;
    crd::containers::Array<crd::usize>                  nbinds;
    explicit AmpParityEncoder(crd::memory::IAllocator* a) : cmds(a), counts(a), progs(a), nbinds(a) {}
    void begin_rendering(const RenderingDesc&) override { ++begins; }
    void draw(const RasterDrawPacket& p) override
    {
        cmds.push_back(p.command);
        counts.push_back(p.geometry.kind == crd::gpu::GeometryKind::Meshlet ? p.geometry.group_count_x
                                                                            : p.geometry.patch_count);
        progs.push_back(p.program);
        nbinds.push_back(p.bindings.size());
    }
    void end_rendering() override { ++ends; }
    void dispatch(const DispatchDesc&) override {}
    void transfer(const TransferDesc&) override {}
    void trace_rays(const TraceDesc&) override {}
};
} // namespace

TEST_CASE("raf7 hand-built and authored graphs record identical commands")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf7-eq");
    DiagnosticList d(&alloc);
    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);
    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);

    const u64 rc = pass_param_id("scene_color");
    const u64 ri = pass_param_id("scene_input");

    // CEIR-16-3d-3: fullscreen.raster records through the GENERIC CEIR replay (record_fullscreen_raster deleted), so the
    // pass needs its per-pass plan — a plain 1-input composite (it reads input0). The SAME plan feeds both graphs (both are
    // pass name_hash 1U); with the plan threaded, both record "BDE" and the hand-built==authored equality still holds.
    crd::ceir::Context                 cctx(&alloc);
    crd::ceir::gpu::FullscreenBuildDesc fbd;
    fbd.num_inputs             = 1U;
    fbd.inputs[0].source_param = pass_param_id("input0");
    crd::containers::Array<crd::ceir::gpu::LoweredCommand> fplan(&alloc);
    REQUIRE(crd::ceir::gpu::build_fullscreen_ceir(cctx, fbd, fplan));
    CeirPlanTable plans(&alloc);
    plans.bind(1U, CeirPassPlan{&cctx, fplan.data(), static_cast<u32>(fplan.size())});

    // Two constructions of the SAME graph, in different declaration orders (hand-built vs authored).
    FrameGraphTemplate a(&alloc);
    a.add_resource(color_res(ri, ResourceLifetime::Persistent));
    a.add_resource(color_res(rc));
    a.add_pass(fullscreen_pass(1U, rc, ri));

    FrameGraphTemplate b(&alloc);
    b.add_pass(fullscreen_pass(1U, rc, ri)); // pass first
    b.add_resource(color_res(rc));
    b.add_resource(color_res(ri, ResourceLifetime::Persistent));

    CompiledFrameGraph ca(&alloc);
    CompiledFrameGraph cb(&alloc);
    REQUIRE(compile(a, schemas, 128U, 128U, ca, d));
    REQUIRE(compile(b, schemas, 128U, 128U, cb, d));

    FakeTarget t(128U, 128U);
    FakeTexture in_tex(128U, 128U);
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{rc, SlotResourceKind::ColorTarget, &t, nullptr, nullptr});
    table.bind(ResolvedResource{ri, SlotResourceKind::Texture, nullptr, nullptr, nullptr, &in_tex});

    MockEncoder ea(&alloc);
    MockEncoder eb(&alloc);
    REQUIRE(execute(ca, a, records, table, fake_programs(), ea, d, nullptr, nullptr, &plans));
    REQUIRE(execute(cb, b, records, table, fake_programs(), eb, d, nullptr, nullptr, &plans));
    REQUIRE(ea.ops == eb.ops);        // identical recorded command stream
    REQUIRE(ea.ops == String("BDE")); // begin · draw · end
    REQUIRE_FALSE(d.has_errors());
}

TEST_CASE("raf7 multiple packets record in one scope")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf7-multi");
    DiagnosticList d(&alloc);
    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);

    // A custom executor that records TWO draws inside one begin/end scope.
    crd::renderpass::PassExecutorDesc desc;
    desc.id = executor_type_id("test.multidraw");
    desc.name = "test.multidraw";
    desc.schema.version = 1U;
    desc.schema.queue = QueueKind::Graphics;
    desc.schema.slots.push_back(
        crd::renderpass::ResourceSlotSpec{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, true});
    REQUIRE(schemas.register_executor(desc, d));

    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    const PassRecordFn multidraw = [](const PassPayload&, RecordContext& ctx, crd::gpu::ICommandEncoder& e)
    {
        IRasterTarget* color = ctx.color_target(pass_param_id("color"));
        if (!ctx.ok() || color == nullptr)
        {
            return;
        }
        RenderingDesc rd;
        rd.width = color->width();
        rd.height = color->height();
        rd.color.push_back(crd::gpu::ColorAttachmentDesc{color, crd::gpu::LoadOp::Clear, crd::gpu::StoreOp::Store, {},
                                                         crd::gpu::BlendMode::Opaque});
        RasterDrawPacket p;
        p.program = fake_raster();
        p.geometry.kind = crd::gpu::GeometryKind::None;
        p.geometry.vertex_or_index_count = 3U;
        e.begin_rendering(rd);
        e.draw(p);
        e.draw(p); // TWO packets in ONE scope
        e.end_rendering();
    };
    REQUIRE(records.register_record(executor_type_id("test.multidraw"), multidraw, d));

    const u64 rc = pass_param_id("c");
    GraphPass p;
    p.name_hash = 1U;
    p.payload.executor = executor_type_id("test.multidraw");
    p.payload.schema_version = 1U;
    p.payload.queue = QueueKind::Graphics;
    p.payload.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, rc});
    FrameGraphTemplate g(&alloc);
    g.add_resource(color_res(rc));
    g.add_pass(p);

    CompiledFrameGraph c(&alloc);
    REQUIRE(compile(g, schemas, 64U, 64U, c, d));
    FakeTarget t(64U, 64U);
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{rc, SlotResourceKind::ColorTarget, &t, nullptr, nullptr});
    MockEncoder e(&alloc);
    REQUIRE(execute(c, g, records, table, fake_programs(), e, d));
    REQUIRE(e.ops == String("BDDE"));   // two draws inside one begin/end
    REQUIRE(e.max_draws_in_scope == 2);
    REQUIRE(e.begin_count == e.end_count); // scopes balanced (one submission stream)
}

TEST_CASE("ceir 16-3c-1: CeirPlanTable lookup + RecordContext carries the optional per-pass CEIR plan")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf7-ceir-plan");
    DiagnosticList             d(&alloc);

    // the plan table (the DrawListTable/PassProgramsTable counterpart): bind a plan by pass name hash, find it back; a
    // non-migrated pass has none.
    CeirPlanTable plans(&alloc);
    CeirPassPlan  plan;
    plan.count = 5U; // ctx/commands stay null — this device-free test exercises the TABLE + threading, not execution
    plans.bind(7U, plan);
    REQUIRE(plans.find(7U) != nullptr);
    CHECK(plans.find(7U)->count == 5U);
    CHECK(plans.find(999U) == nullptr); // a pass with no bound plan resolves to null (a non-migrated executor)

    // RecordContext threads the plan (optional; the pre-existing 5-arg construction stays valid, yielding a null plan).
    PassPayload   payload{};
    ResourceTable table(&alloc);
    PassPrograms  programs;
    RecordContext with_plan(payload, table, programs, d, nullptr, plans.find(7U));
    CHECK(with_plan.plan() == plans.find(7U));
    CHECK(with_plan.plan()->count == 5U);
    RecordContext without(payload, table, programs, d);
    CHECK(without.plan() == nullptr); // ⭐ a non-migrated pass threads no plan (the executor uses its C++ record path)
}

TEST_CASE("ceir 16-3c-1b: execute() threads a bound CeirPlanTable plan into the pass's RecordContext by name hash")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf7-ceir-thread");
    DiagnosticList             d(&alloc);
    ExecutorRegistry           schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);
    crd::renderpass::PassExecutorDesc desc;
    desc.id             = executor_type_id("test.planprobe");
    desc.name           = "test.planprobe";
    desc.schema.version = 1U;
    desc.schema.queue   = QueueKind::Graphics;
    desc.schema.slots.push_back(
        crd::renderpass::ResourceSlotSpec{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, true});
    REQUIRE(schemas.register_executor(desc, d));

    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    // The probe makes plan-arrival OBSERVABLE through execute()'s return: a pass that did NOT receive its plan (count 5)
    // touches an UNDECLARED slot (ctx.ok()→false → execute() returns false); one that received it records nothing, stays ok.
    const PassRecordFn probe = [](const PassPayload&, RecordContext& ctx, crd::gpu::ICommandEncoder&)
    {
        if (ctx.plan() == nullptr || ctx.plan()->count != 5U)
        {
            (void)ctx.color_target(pass_param_id("nonexistent")); // undeclared → ctx.ok() becomes false
        }
    };
    REQUIRE(records.register_record(executor_type_id("test.planprobe"), probe, d));

    const u64 rc = pass_param_id("c");
    GraphPass p;
    p.name_hash              = 42U;
    p.payload.executor       = executor_type_id("test.planprobe");
    p.payload.schema_version = 1U;
    p.payload.queue          = QueueKind::Graphics;
    p.payload.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, rc});
    FrameGraphTemplate g(&alloc);
    g.add_resource(color_res(rc));
    g.add_pass(p);
    CompiledFrameGraph c(&alloc);
    REQUIRE(compile(g, schemas, 64U, 64U, c, d));
    FakeTarget    t(64U, 64U);
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{rc, SlotResourceKind::ColorTarget, &t, nullptr, nullptr});

    // no plan table → the probe sees no plan → touches an undeclared slot → execute() fails.
    MockEncoder e_noplan(&alloc);
    REQUIRE_FALSE(execute(c, g, records, table, fake_programs(), e_noplan, d));

    // bind the plan for pass 42 (by name hash) → execute() threads it → the probe sees count 5 → stays ok → succeeds.
    DiagnosticList d2(&alloc); // fresh diags (the first execute logged the deliberate undeclared-access error)
    CeirPlanTable  plans(&alloc);
    CeirPassPlan   plan;
    plan.count = 5U;
    plans.bind(42U, plan);
    MockEncoder e_plan(&alloc);
    REQUIRE(execute(c, g, records, table, fake_programs(), e_plan, d2, nullptr, nullptr, &plans));
}

TEST_CASE("ceir 16-3c-2: record_ceir_render replays a built plan (plain 1-input) -> begin/draw/end, the imperative shape")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf7-ceir-replay");
    DiagnosticList             d(&alloc);

    // Build the fullscreen composite plan (a plain single-input read) via the CEIR builder — the DATA the executor replays.
    crd::ceir::Context                 cctx(&alloc);
    crd::ceir::gpu::FullscreenBuildDesc desc;
    desc.num_inputs             = 1U;
    desc.inputs[0].source_param = pass_param_id("input0"); // resolver maps the binding operand -> ctx.texture(pass_param_id("input0"))
    crd::containers::Array<crd::ceir::gpu::LoweredCommand> plan(&alloc);
    REQUIRE(crd::ceir::gpu::build_fullscreen_ceir(cctx, desc, plan));
    CeirPassPlan cp;
    cp.ctx      = &cctx;
    cp.commands = plan.data();
    cp.count    = static_cast<u32>(plan.size());

    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);
    crd::renderpass::PassExecutorDesc ed;
    ed.id             = executor_type_id("test.fsceir");
    ed.name           = "test.fsceir";
    ed.schema.version = 1U;
    ed.schema.queue   = QueueKind::Graphics;
    ed.schema.slots.push_back(
        crd::renderpass::ResourceSlotSpec{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, true});
    ed.schema.slots.push_back(
        crd::renderpass::ResourceSlotSpec{pass_param_id("input0"), SlotResourceKind::Texture, SlotAccess::Read, true});
    REQUIRE(schemas.register_executor(ed, d));

    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    REQUIRE(records.register_record(executor_type_id("test.fsceir"), record_ceir_render, d));

    const u64 rc = pass_param_id("out_color");
    const u64 ri = pass_param_id("in_tex");
    GraphPass p;
    p.name_hash              = 77U;
    p.payload.executor       = executor_type_id("test.fsceir");
    p.payload.schema_version = 1U;
    p.payload.queue          = QueueKind::Graphics;
    p.payload.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, rc});
    p.payload.resources.push_back(ResourceRef{pass_param_id("input0"), SlotResourceKind::Texture, SlotAccess::Read, ri});
    FrameGraphTemplate g(&alloc);
    g.add_resource(color_res(ri, ResourceLifetime::Persistent));
    g.add_resource(color_res(rc));
    g.add_pass(p);
    CompiledFrameGraph c(&alloc);
    REQUIRE(compile(g, schemas, 128U, 128U, c, d));

    FakeTarget    t(128U, 128U);
    FakeTexture   in_tex(128U, 128U);
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{rc, SlotResourceKind::ColorTarget, &t, nullptr, nullptr});
    table.bind(ResolvedResource{ri, SlotResourceKind::Texture, nullptr, nullptr, nullptr, &in_tex});

    CeirPlanTable plans(&alloc);
    plans.bind(77U, cp); // the plan for pass 77

    MockEncoder e(&alloc);
    REQUIRE(execute(c, g, records, table, fake_programs(), e, d, nullptr, nullptr, &plans));
    REQUIRE(e.ops == String("BDE"));   // ⭐ the CEIR replay records begin/draw/end — the same shape record_fullscreen_raster emits
    REQUIRE_FALSE(d.has_errors());
}

TEST_CASE("ceir 16-3c-2b: record_ceir_render replays a bindless (n>1) plan -> fs_texture_array GATHERS input0+input1")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf7-ceir-bindless");
    DiagnosticList             d(&alloc);

    // n>1 reads => the BINDLESS shape (ONE resource_table binding); the replay's fs_texture_array resolver must gather
    // input0..input7 (skip undeclared, abort on a declared-but-null) into the packet's texture array.
    crd::ceir::Context                 cctx(&alloc);
    crd::ceir::gpu::FullscreenBuildDesc desc;
    desc.num_inputs             = 2U;
    desc.inputs[0].source_param = pass_param_id("input0");
    desc.inputs[1].source_param = pass_param_id("input1");
    crd::containers::Array<crd::ceir::gpu::LoweredCommand> plan(&alloc);
    REQUIRE(crd::ceir::gpu::build_fullscreen_ceir(cctx, desc, plan));
    CeirPassPlan cp{&cctx, plan.data(), static_cast<u32>(plan.size())};

    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);
    crd::renderpass::PassExecutorDesc ed;
    ed.id             = executor_type_id("test.fsceir2");
    ed.name           = "test.fsceir2";
    ed.schema.version = 1U;
    ed.schema.queue   = QueueKind::Graphics;
    ed.schema.slots.push_back(
        crd::renderpass::ResourceSlotSpec{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, true});
    ed.schema.slots.push_back(
        crd::renderpass::ResourceSlotSpec{pass_param_id("input0"), SlotResourceKind::Texture, SlotAccess::Read, true});
    ed.schema.slots.push_back(
        crd::renderpass::ResourceSlotSpec{pass_param_id("input1"), SlotResourceKind::Texture, SlotAccess::Read, true});
    REQUIRE(schemas.register_executor(ed, d));

    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 14U);
    REQUIRE(records.register_record(executor_type_id("test.fsceir2"), record_ceir_render, d));

    const u64 rc  = pass_param_id("out2");
    const u64 ri0 = pass_param_id("in2_0");
    const u64 ri1 = pass_param_id("in2_1");
    GraphPass p;
    p.name_hash              = 88U;
    p.payload.executor       = executor_type_id("test.fsceir2");
    p.payload.schema_version = 1U;
    p.payload.queue          = QueueKind::Graphics;
    p.payload.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, rc});
    p.payload.resources.push_back(ResourceRef{pass_param_id("input0"), SlotResourceKind::Texture, SlotAccess::Read, ri0});
    p.payload.resources.push_back(ResourceRef{pass_param_id("input1"), SlotResourceKind::Texture, SlotAccess::Read, ri1});
    FrameGraphTemplate g(&alloc);
    g.add_resource(color_res(ri0, ResourceLifetime::Persistent));
    g.add_resource(color_res(ri1, ResourceLifetime::Persistent));
    g.add_resource(color_res(rc));
    g.add_pass(p);
    CompiledFrameGraph c(&alloc);
    REQUIRE(compile(g, schemas, 128U, 128U, c, d));

    FakeTarget    t(128U, 128U);
    FakeTexture   in0(128U, 128U);
    FakeTexture   in1(128U, 128U);
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{rc, SlotResourceKind::ColorTarget, &t, nullptr, nullptr});
    table.bind(ResolvedResource{ri0, SlotResourceKind::Texture, nullptr, nullptr, nullptr, &in0});
    table.bind(ResolvedResource{ri1, SlotResourceKind::Texture, nullptr, nullptr, nullptr, &in1});

    CeirPlanTable plans(&alloc);
    plans.bind(88U, cp);
    MockEncoder e(&alloc);
    REQUIRE(execute(c, g, records, table, fake_programs(), e, d, nullptr, nullptr, &plans));
    REQUIRE(e.ops == String("BDE")); // ⭐ the gather resolved input0+input1 into a BindlessTextureArray -> the draw materialized
    REQUIRE_FALSE(d.has_errors());
}

TEST_CASE("ceir 16-mesh-1: record_ceir_render records the mesh.indirect descriptor end-to-end (DispatchMeshIndirect + args)")
{
    // record_mesh_indirect is DELETED — its descriptor parity vs this CEIR replay was PROVEN at deletion time (an A/B in this
    // test's git history, migration doc §6: no shipped asset uses mesh.indirect so a device A/B is vacuous, and both paths fed
    // the encoder the SAME RenderingDesc + RasterDrawPacket). This is the surviving end-to-end guard of the CEIR record path.
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "mesh-1-parity");
    DiagnosticList             d(&alloc);
    ExecutorRegistry           schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);

    const u64 r_col  = pass_param_id("mi_color");
    const u64 r_args = pass_param_id("mi_args");
    GraphPass p;
    p.name_hash              = 7U;
    p.payload.executor       = executor_type_id("mesh.indirect");
    p.payload.schema_version = 1U;
    p.payload.queue          = QueueKind::Graphics;
    p.payload.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, r_col});
    p.payload.resources.push_back(ResourceRef{pass_param_id("args"), SlotResourceKind::StorageBuffer, SlotAccess::Read, r_args});
    // ⭐ NON-default clear + NON-zero args_offset so a DROPPED field cannot hide behind a default (advisor).
    p.payload.params.push_back(crd::renderpass::ParamValue{pass_param_id("args_offset"), tv_u32(256U)});
    p.payload.params.push_back(crd::renderpass::ParamValue{pass_param_id("clear_color"), tv_vec4(0.1F, 0.2F, 0.3F, 1.0F)});

    FrameGraphTemplate g(&alloc);
    g.add_resource(GraphResource{r_col, SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    g.add_resource(GraphResource{r_args, SlotResourceKind::StorageBuffer, ResourceLifetime::Persistent, 1U});
    g.add_pass(p);
    CompiledFrameGraph c(&alloc);
    REQUIRE(compile(g, schemas, 320U, 240U, c, d));

    FakeTarget                      col(320U, 240U);
    crd::gpu::IStorageBuffer* const args_sentinel = reinterpret_cast<crd::gpu::IStorageBuffer*>(0xB0F0ULL);
    ResourceTable                   table(&alloc);
    table.bind(ResolvedResource{r_col, SlotResourceKind::ColorTarget, &col, nullptr, nullptr, nullptr});
    ResolvedResource args_rr{};
    args_rr.name_hash = r_args;
    args_rr.kind      = SlotResourceKind::StorageBuffer;
    args_rr.buffer    = args_sentinel;
    table.bind(args_rr);

    crd::ceir::Context                    cctx(&alloc);
    crd::ceir::gpu::MeshIndirectBuildDesc mbd;
    mbd.args_param  = pass_param_id("args");
    mbd.args_offset = 256U;
    mbd.clear       = crd::gpu::ClearColor{0.1F, 0.2F, 0.3F, 1.0F};
    crd::containers::Array<crd::ceir::gpu::LoweredCommand> cmds(&alloc);
    REQUIRE(crd::ceir::gpu::build_mesh_indirect_ceir(cctx, mbd, cmds));
    CeirPlanTable plans(&alloc);
    plans.bind(7U, CeirPassPlan{&cctx, cmds.data(), static_cast<u32>(cmds.size())});
    GraphExecutorTable records_b(&alloc);
    REQUIRE(records_b.register_record(executor_type_id("mesh.indirect"), record_ceir_render, d));
    CaptureEncoder eb;
    REQUIRE(execute(c, g, records_b, table, fake_programs(), eb, d, nullptr, nullptr, &plans));
    REQUIRE_FALSE(d.has_errors());

    // ── the recorded descriptor, field-by-field (the values the deleted record_mesh_indirect produced identically). ──
    CHECK(eb.cap.draws == 1);
    CHECK(eb.cap.rd_w == 320U); // extent_from_target resolved the 320x240 target, not the 1x1 placeholder
    CHECK(eb.cap.rd_h == 240U);
    CHECK(eb.cap.has_color);
    CHECK_FALSE(eb.cap.has_depth);
    CHECK(eb.cap.load == crd::gpu::LoadOp::Clear);
    CHECK(eb.cap.store == crd::gpu::StoreOp::Store);      // ⛔ the builder sets NO store attr — the materializer default matches legacy
    CHECK(eb.cap.blend == crd::gpu::BlendMode::Opaque);   // ⛔ ditto blend
    CHECK(eb.cap.cb > 0.25F);                             // ⭐ 0.3 clear carried (a mesh dispatch may not cover all pixels)
    CHECK(eb.cap.cmd == crd::gpu::RasterCommandKind::DispatchMeshIndirect);
    CHECK(eb.cap.geo == crd::gpu::GeometryKind::MeshletIndirect);
    CHECK(eb.cap.args == args_sentinel);                 // ⭐ the args buffer identity (the CEIR-16-mesh-1 materializer fix)
    CHECK(eb.cap.args_off == 256U);
    CHECK(eb.cap.prog == fake_raster());                 // the pass's raster program, resolved at record
    CHECK(eb.cap.nbind == 0U);                            // mesh.indirect binds NO descriptors
}

TEST_CASE("ceir 16-mesh-2: record_ceir_render EXPANDS mesh_dispatch_list over ctx.draws() into per-item amplify draws")
{
    // record_amplify_raster is DELETED — its per-item parity vs this CEIR replay was PROVEN at deletion time (an A/B in this
    // test's git history). This is the surviving end-to-end guard: record_ceir_render replays a build_amplify_ceir plan whose
    // mesh_dispatch_list the walk EXPANDS over ctx.draws() into N draws in ONE scope, with the skip/default per-item semantics.
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "mesh-2-parity");
    DiagnosticList             d(&alloc);
    ExecutorRegistry           schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);

    const u64 r_col = pass_param_id("amp_col");
    GraphPass p;
    p.name_hash              = 9U;
    p.payload.executor       = executor_type_id("mesh.raster"); // mesh.raster -> record_amplify_raster(mesh=true)
    p.payload.schema_version = 1U;
    p.payload.queue          = QueueKind::Graphics;
    p.payload.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, r_col});
    p.payload.params.push_back(crd::renderpass::ParamValue{pass_param_id("clear_color"), tv_vec4(0.1F, 0.2F, 0.3F, 1.0F)});
    FrameGraphTemplate g(&alloc);
    g.add_resource(GraphResource{r_col, SlotResourceKind::ColorTarget, ResourceLifetime::Persistent, 1U});
    g.add_pass(p);
    CompiledFrameGraph c(&alloc);
    REQUIRE(compile(g, schemas, 128U, 128U, c, d));

    FakeTarget    col(128U, 128U);
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{r_col, SlotResourceKind::ColorTarget, &col, nullptr, nullptr, nullptr});

    // the amplify DrawList: [normal (count 3, own program + storage), zero-count (SKIPPED), null-program (count 5 -> default)].
    crd::gpu::IStorageBuffer* const geo    = reinterpret_cast<crd::gpu::IStorageBuffer*>(0xB0F0ULL);
    int                            prog_a_s = 0;
    crd::gpu::IRasterProgram* const prog_a = reinterpret_cast<crd::gpu::IRasterProgram*>(&prog_a_s);
    RenderDrawItem items[3]{};
    items[0].program = prog_a;   items[0].vertex_count = 3U; items[0].storage = geo;
    items[1].program = prog_a;   items[1].vertex_count = 0U; // ⛔ zero count -> SKIPPED
    items[2].program = nullptr;  items[2].vertex_count = 5U; // null program -> the pass default
    DrawList dl{};
    dl.items = items;
    dl.count = 3U;
    DrawListTable draw_lists(&alloc); // execute() threads per-pass draw lists by name hash (RAF-8)
    draw_lists.bind(9U, dl);

    crd::ceir::Context              cctx(&alloc);
    crd::ceir::gpu::AmplifyBuildDesc abd;
    abd.patches        = false; // mesh.raster
    abd.fallback_count = 0U;
    crd::containers::Array<crd::ceir::gpu::LoweredCommand> cmds(&alloc);
    REQUIRE(crd::ceir::gpu::build_amplify_ceir(cctx, abd, cmds));
    CeirPlanTable plans(&alloc);
    plans.bind(9U, CeirPassPlan{&cctx, cmds.data(), static_cast<u32>(cmds.size())});
    GraphExecutorTable records_b(&alloc);
    REQUIRE(records_b.register_record(executor_type_id("mesh.raster"), record_ceir_render, d));
    AmpParityEncoder eb(&alloc);
    REQUIRE(execute(c, g, records_b, table, fake_programs(), eb, d, &draw_lists, nullptr, &plans));
    REQUIRE_FALSE(d.has_errors());

    // ── the expanded draws, per-item (the values the deleted record_amplify_raster produced identically). ──
    CHECK(eb.begins == 1); // ONE scope for N draws (single begin/end)
    CHECK(eb.ends == 1);
    REQUIRE(eb.cmds.size() == 2U); // the zero-count item (items[1]) produced no draw
    CHECK(eb.cmds[0] == crd::gpu::RasterCommandKind::DispatchMesh);
    CHECK(eb.counts[0] == 3U);
    CHECK(eb.progs[0] == prog_a);
    CHECK(eb.nbinds[0] == 1U); // the per-item storage bound
    CHECK(eb.cmds[1] == crd::gpu::RasterCommandKind::DispatchMesh);
    CHECK(eb.counts[1] == 5U);
    CHECK(eb.progs[1] == fake_raster()); // ⭐ null-program item resolved to the pass default
    CHECK(eb.nbinds[1] == 0U);
}

TEST_CASE("raf7 an undeclared resource access is diagnosed")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf7-undeclared");
    DiagnosticList d(&alloc);
    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);

    crd::renderpass::PassExecutorDesc desc;
    desc.id = executor_type_id("test.buggy");
    desc.name = "test.buggy";
    desc.schema.version = 1U;
    desc.schema.queue = QueueKind::Graphics;
    desc.schema.slots.push_back(
        crd::renderpass::ResourceSlotSpec{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, true});
    REQUIRE(schemas.register_executor(desc, d));

    GraphExecutorTable records(&alloc);
    const PassRecordFn buggy = [](const PassPayload&, RecordContext& ctx, crd::gpu::ICommandEncoder&)
    {
        (void)ctx.color_target(pass_param_id("color"));      // declared — fine
        (void)ctx.storage(pass_param_id("secret_buffer"));   // NOT declared — a violation
    };
    REQUIRE(records.register_record(executor_type_id("test.buggy"), buggy, d));

    const u64 rc = pass_param_id("c");
    GraphPass p;
    p.name_hash = 1U;
    p.payload.executor = executor_type_id("test.buggy");
    p.payload.schema_version = 1U;
    p.payload.queue = QueueKind::Graphics;
    p.payload.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, rc});
    FrameGraphTemplate g(&alloc);
    g.add_resource(color_res(rc));
    g.add_pass(p);

    CompiledFrameGraph c(&alloc);
    REQUIRE(compile(g, schemas, 64U, 64U, c, d));
    FakeTarget t(64U, 64U);
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{rc, SlotResourceKind::ColorTarget, &t, nullptr, nullptr});
    MockEncoder e(&alloc);
    DiagnosticList exec_d(&alloc);
    REQUIRE_FALSE(execute(c, g, records, table, fake_programs(), e, exec_d)); // the violation fails execution
    REQUIRE(exec_d.contains(DiagCode::InvalidSlot));
}

TEST_CASE("raf7 transient aliasing and persistent pinning")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf7-alias");
    DiagnosticList d(&alloc);
    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);

    // A 4-pass chain: src -> r1 -> r2 -> r3 -> out. r1 dies (last read at pass1) before r3 is born (first write at
    // pass2), so r1 and r3 may ALIAS. r1 and r2 overlap (both touched at pass1), so they may NOT.
    const u64 src = pass_param_id("src");
    const u64 r1 = pass_param_id("r1");
    const u64 r2 = pass_param_id("r2");
    const u64 r3 = pass_param_id("r3");
    const u64 out = pass_param_id("out");

    FrameGraphTemplate g(&alloc);
    g.add_resource(color_res(src, ResourceLifetime::Persistent)); // the persistent input
    g.add_resource(color_res(r1));
    g.add_resource(color_res(r2));
    g.add_resource(color_res(r3));
    g.add_resource(color_res(out, ResourceLifetime::Persistent)); // the persistent output
    g.add_pass(fullscreen_pass(1U, r1, src));
    g.add_pass(fullscreen_pass(2U, r2, r1));
    g.add_pass(fullscreen_pass(3U, r3, r2));
    g.add_pass(fullscreen_pass(4U, out, r3));

    CompiledFrameGraph c(&alloc);
    REQUIRE(compile(g, schemas, 100U, 100U, c, d));
    REQUIRE(c.schedule().size() == 4U);

    const CompiledResource* cr1 = c.find(r1);
    const CompiledResource* cr2 = c.find(r2);
    const CompiledResource* cr3 = c.find(r3);
    const CompiledResource* csrc = c.find(src);
    REQUIRE(cr1 != nullptr);
    REQUIRE(cr2 != nullptr);
    REQUIRE(cr3 != nullptr);
    REQUIRE(csrc != nullptr);
    REQUIRE(cr1->physical_slot == cr3->physical_slot); // non-overlapping transients alias
    REQUIRE(cr1->physical_slot != cr2->physical_slot); // overlapping transients do not
    // A persistent resource is never aliased onto a transient's slot.
    REQUIRE(csrc->physical_slot != cr1->physical_slot);
    REQUIRE(csrc->lifetime == ResourceLifetime::Persistent);
}

TEST_CASE("raf7 resize recompiles only the size, not the topology")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf7-resize");
    DiagnosticList d(&alloc);
    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 14U);

    const u64 r1 = pass_param_id("r1");
    const u64 out = pass_param_id("out");
    FrameGraphTemplate g(&alloc);
    g.add_resource(color_res(r1));
    g.add_resource(color_res(out, ResourceLifetime::Persistent));
    g.add_pass(fullscreen_pass(1U, r1, 0U));
    g.add_pass(fullscreen_pass(2U, out, r1));

    CompiledFrameGraph small(&alloc);
    CompiledFrameGraph large(&alloc);
    REQUIRE(compile(g, schemas, 100U, 100U, small, d));
    REQUIRE(compile(g, schemas, 400U, 300U, large, d));

    // The schedule + physical slot assignment are topology-derived: identical across the resize.
    REQUIRE(small.schedule().size() == large.schedule().size());
    for (u32 i = 0; i < small.schedule().size(); ++i)
    {
        REQUIRE(small.schedule()[i] == large.schedule()[i]);
    }
    REQUIRE(small.physical_slot_count() == large.physical_slot_count());
    REQUIRE(small.find(r1)->physical_slot == large.find(r1)->physical_slot);
    // Only the size differs.
    REQUIRE(small.width() == 100U);
    REQUIRE(large.width() == 400U);
    REQUIRE(large.height() == 300U);
}
