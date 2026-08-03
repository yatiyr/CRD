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
        p.payload.resources.push_back(ResourceRef{pass_param_id("input"), SlotResourceKind::Texture, SlotAccess::Read, input_res});
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
} // namespace

TEST_CASE("raf7 hand-built and authored graphs record identical commands")
{
    crd::memory::TlsfAllocator alloc(2U << 20U, nullptr, "raf7-eq");
    DiagnosticList d(&alloc);
    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 9U);
    GraphExecutorTable records(&alloc);
    REQUIRE(register_builtin_records(records, d) == 9U);

    const u64 rc = pass_param_id("scene_color");
    const u64 ri = pass_param_id("scene_input");

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
    ResourceTable table(&alloc);
    table.bind(ResolvedResource{rc, SlotResourceKind::ColorTarget, &t, nullptr, nullptr});
    table.bind(ResolvedResource{ri, SlotResourceKind::Texture, &t, nullptr, nullptr});

    MockEncoder ea(&alloc);
    MockEncoder eb(&alloc);
    REQUIRE(execute(ca, a, records, table, fake_programs(), ea, d));
    REQUIRE(execute(cb, b, records, table, fake_programs(), eb, d));
    REQUIRE(ea.ops == eb.ops);        // identical recorded command stream
    REQUIRE(ea.ops == String("BDE")); // begin · draw · end
    REQUIRE_FALSE(d.has_errors());
}

TEST_CASE("raf7 multiple packets record in one scope")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf7-multi");
    DiagnosticList d(&alloc);
    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 9U);

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
    REQUIRE(register_builtin_records(records, d) == 9U);
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

TEST_CASE("raf7 an undeclared resource access is diagnosed")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf7-undeclared");
    DiagnosticList d(&alloc);
    ExecutorRegistry schemas(&alloc);
    REQUIRE(register_builtin_executors(schemas, d) == 9U);

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
    REQUIRE(register_builtin_executors(schemas, d) == 9U);

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
    REQUIRE(register_builtin_executors(schemas, d) == 9U);

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
