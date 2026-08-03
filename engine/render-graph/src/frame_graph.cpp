#include <crd/rendergraph/frame_graph.hpp>

#include <crd/renderpass/executor_registry.hpp>

#include <utility> // std::swap

namespace crd::rendergraph
{
using namespace crd::gpu;
using crd::renderpass::ExecutorParamType;
using crd::renderpass::pass_param_id;
using crd::renderpass::ResourceRef;
using crd::renderpass::TypedValue;

// ── lookups ──
const GraphResource* FrameGraphTemplate::find_resource(u64 name_hash) const noexcept
{
    for (u32 i = 0; i < m_resources.size(); ++i)
    {
        if (m_resources[i].name_hash == name_hash)
        {
            return &m_resources[i];
        }
    }
    return nullptr;
}

const CompiledResource* CompiledFrameGraph::find(u64 name_hash) const noexcept
{
    for (u32 i = 0; i < m_resources.size(); ++i)
    {
        if (m_resources[i].name_hash == name_hash)
        {
            return &m_resources[i];
        }
    }
    return nullptr;
}

const ResolvedResource* ResourceTable::find(u64 name_hash) const noexcept
{
    for (u32 i = 0; i < m_resources.size(); ++i)
    {
        if (m_resources[i].name_hash == name_hash)
        {
            return &m_resources[i];
        }
    }
    return nullptr;
}

// ── RecordContext ──
namespace
{
const ResourceRef* find_ref(const PassPayload& payload, u64 slot_name) noexcept
{
    for (u32 i = 0; i < payload.resources.size(); ++i)
    {
        if (payload.resources[i].slot_name_hash == slot_name)
        {
            return &payload.resources[i];
        }
    }
    return nullptr;
}
const TypedValue* find_param(const PassPayload& payload, u64 name) noexcept
{
    for (u32 i = 0; i < payload.params.size(); ++i)
    {
        if (payload.params[i].name_hash == name)
        {
            return &payload.params[i].value;
        }
    }
    return nullptr;
}
} // namespace

bool RecordContext::is_declared(u64 slot_name) const noexcept { return find_ref(*m_payload, slot_name) != nullptr; }

const ResolvedResource* resolve_declared(const RecordContext& /*unused*/, const PassPayload& payload,
                                         const ResourceTable& table, u64 slot_name)
{
    const ResourceRef* ref = find_ref(payload, slot_name);
    if (ref == nullptr)
    {
        return nullptr;
    }
    return table.find(ref->resource_id);
}

IRasterTarget* RecordContext::color_target(u64 slot_name) const
{
    if (!is_declared(slot_name))
    {
        m_diags->error(DiagCode::InvalidSlot, "record function touched an undeclared resource slot");
        m_ok = false;
        return nullptr;
    }
    const ResolvedResource* r = resolve_declared(*this, *m_payload, *m_table, slot_name);
    return (r != nullptr) ? r->target : nullptr;
}
IRasterTarget* RecordContext::depth_target(u64 slot_name) const { return color_target(slot_name); }
IStorageBuffer* RecordContext::storage(u64 slot_name) const
{
    if (!is_declared(slot_name))
    {
        m_diags->error(DiagCode::InvalidSlot, "record function touched an undeclared resource slot");
        m_ok = false;
        return nullptr;
    }
    const ResolvedResource* r = resolve_declared(*this, *m_payload, *m_table, slot_name);
    return (r != nullptr) ? r->buffer : nullptr;
}
IAccelerationStructure* RecordContext::accel(u64 slot_name) const
{
    if (!is_declared(slot_name))
    {
        m_diags->error(DiagCode::InvalidSlot, "record function touched an undeclared resource slot");
        m_ok = false;
        return nullptr;
    }
    const ResolvedResource* r = resolve_declared(*this, *m_payload, *m_table, slot_name);
    return (r != nullptr) ? r->accel : nullptr;
}

// ── GraphExecutorTable ──
usize GraphExecutorTable::lower_bound(ExecutorTypeId id) const noexcept
{
    usize lo = 0;
    usize hi = m_entries.size();
    while (lo < hi)
    {
        const usize mid = lo + (hi - lo) / 2;
        if (m_entries[mid].id < id)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}
PassRecordFn GraphExecutorTable::find(ExecutorTypeId id) const noexcept
{
    const usize idx = lower_bound(id);
    if (idx < m_entries.size() && m_entries[idx].id == id)
    {
        return m_entries[idx].fn;
    }
    return nullptr;
}
bool GraphExecutorTable::register_record(ExecutorTypeId id, PassRecordFn fn, DiagnosticList& diags)
{
    const usize idx = lower_bound(id);
    if (idx < m_entries.size() && m_entries[idx].id == id)
    {
        diags.error(DiagCode::DuplicateExecutor, "a record function for this executor is already registered");
        return false;
    }
    m_entries.push_back(Entry{id, fn});
    for (usize j = m_entries.size() - 1; j > idx; --j)
    {
        std::swap(m_entries[j], m_entries[j - 1]);
    }
    return true;
}

// ── built-in record functions: payload + resources → the canonical command model ──
namespace
{
ClearColor clear_from(const PassPayload& payload)
{
    const TypedValue* cc = find_param(payload, pass_param_id("clear_color"));
    if (cc != nullptr && cc->type == ExecutorParamType::Vec4)
    {
        return ClearColor{cc->v4[0], cc->v4[1], cc->v4[2], cc->v4[3]};
    }
    return ClearColor{};
}
u32 u32_param(const PassPayload& payload, StringView name, u32 fallback)
{
    const TypedValue* v = find_param(payload, pass_param_id(name));
    return (v != nullptr && v->type == ExecutorParamType::U32) ? v->u : fallback;
}

void record_scene_raster(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IRasterTarget* color = ctx.color_target(pass_param_id("color"));
    IStorageBuffer* geo = ctx.storage(pass_param_id("geometry"));
    IRasterTarget* depth = ctx.has(pass_param_id("depth")) ? ctx.depth_target(pass_param_id("depth")) : nullptr;
    if (!ctx.ok() || color == nullptr)
    {
        return;
    }
    RenderingDesc rd;
    rd.width = color->width();
    rd.height = color->height();
    rd.color.push_back(ColorAttachmentDesc{color, LoadOp::Clear, StoreOp::Store, clear_from(payload), BlendMode::Opaque});
    if (depth != nullptr)
    {
        const TypedValue* cd = find_param(payload, pass_param_id("clear_depth"));
        const f32 clear_depth = (cd != nullptr && cd->type == ExecutorParamType::F32) ? cd->f : 1.0F;
        rd.depth = DepthStencilAttachmentDesc{depth,      true, LoadOp::Clear, StoreOp::Store, clear_depth, true,
                                              DepthCompare::LessEqual};
    }
    RasterDrawPacket p;
    p.program = ctx.programs().raster;
    p.command = RasterCommandKind::Draw;
    p.geometry.kind = GeometryKind::StoragePull;
    p.geometry.vertex_or_index_count = 3U;
    if (geo != nullptr)
    {
        p.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, geo});
    }
    encoder.begin_rendering(rd);
    encoder.draw(p);
    encoder.end_rendering();
}

void record_fullscreen_raster(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IRasterTarget* color = ctx.color_target(pass_param_id("color"));
    IRasterTarget* input = ctx.has(pass_param_id("input")) ? ctx.color_target(pass_param_id("input")) : nullptr;
    (void)input;
    if (!ctx.ok() || color == nullptr)
    {
        return;
    }
    RenderingDesc rd;
    rd.width = color->width();
    rd.height = color->height();
    rd.color.push_back(ColorAttachmentDesc{color, LoadOp::Clear, StoreOp::Store, clear_from(payload), BlendMode::Opaque});
    RasterDrawPacket p;
    p.program = ctx.programs().raster;
    p.command = RasterCommandKind::Draw;
    p.geometry.kind = GeometryKind::None;
    p.geometry.vertex_or_index_count = 3U; // fullscreen triangle
    encoder.begin_rendering(rd);
    encoder.draw(p);
    encoder.end_rendering();
}

void record_compute_dispatch(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IStorageBuffer* storage = ctx.storage(pass_param_id("storage"));
    if (!ctx.ok())
    {
        return;
    }
    DispatchDesc d;
    d.kernel = ctx.programs().kernel;
    d.kind = DispatchKind::Direct;
    d.groups_x = u32_param(payload, "groups_x", 1U);
    d.groups_y = u32_param(payload, "groups_y", 1U);
    d.groups_z = u32_param(payload, "groups_z", 1U);
    if (storage != nullptr)
    {
        d.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, storage});
    }
    encoder.dispatch(d);
}

void record_transfer_clear(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IRasterTarget* target = ctx.color_target(pass_param_id("target"));
    if (!ctx.ok())
    {
        return;
    }
    TransferDesc t;
    t.kind = TransferKind::Clear;
    t.dst = target;
    t.clear = clear_from(payload);
    encoder.transfer(t);
}

void record_transfer_op(const PassPayload& /*payload*/, RecordContext& ctx, ICommandEncoder& encoder, TransferKind kind)
{
    IRasterTarget* src = ctx.color_target(pass_param_id("src"));
    IRasterTarget* dst = ctx.color_target(pass_param_id("dst"));
    if (!ctx.ok())
    {
        return;
    }
    TransferDesc t;
    t.kind = kind;
    t.src = src;
    t.dst = dst;
    encoder.transfer(t);
}
void record_transfer_copy(const PassPayload& p, RecordContext& ctx, ICommandEncoder& e)
{
    record_transfer_op(p, ctx, e, TransferKind::Copy);
}
void record_transfer_blit(const PassPayload& p, RecordContext& ctx, ICommandEncoder& e)
{
    record_transfer_op(p, ctx, e, TransferKind::Blit);
}
void record_transfer_resolve(const PassPayload& p, RecordContext& ctx, ICommandEncoder& e)
{
    record_transfer_op(p, ctx, e, TransferKind::Resolve);
}

void record_raytrace_dispatch(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IStorageBuffer* output = ctx.storage(pass_param_id("output"));
    IAccelerationStructure* accel = ctx.accel(pass_param_id("accel"));
    (void)output;
    if (!ctx.ok() || accel == nullptr || ctx.programs().raygen == nullptr)
    {
        return;
    }
    TraceDesc t;
    t.raygen = ctx.programs().raygen;
    t.miss = ctx.programs().miss;
    t.closest_hit = ctx.programs().closest_hit;
    t.acceleration_structure = accel;
    t.width = u32_param(payload, "width", 1U);
    t.height = u32_param(payload, "height", 1U);
    encoder.trace_rays(t);
}

void record_present(const PassPayload& /*payload*/, RecordContext& ctx, ICommandEncoder& /*encoder*/)
{
    // Present is the SURFACE seam (IPresentSurface), not a command-encoder op — it declares its source (validated as
    // read here) but records no canonical command. The surface blits the source into the backbuffer at present time.
    (void)ctx.color_target(pass_param_id("source"));
}
} // namespace

u32 register_builtin_records(GraphExecutorTable& table, DiagnosticList& diags)
{
    using crd::renderpass::executor_type_id;
    u32 n = 0;
    n += table.register_record(executor_type_id("scene.raster"), record_scene_raster, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("fullscreen.raster"), record_fullscreen_raster, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("compute.dispatch"), record_compute_dispatch, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("transfer.clear"), record_transfer_clear, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("transfer.copy"), record_transfer_copy, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("transfer.blit"), record_transfer_blit, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("transfer.resolve"), record_transfer_resolve, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("raytrace.dispatch"), record_raytrace_dispatch, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("present"), record_present, diags) ? 1U : 0U;
    return n;
}

// ── compile ──
namespace
{
bool pass_touches(const GraphPass& p, u64 res_id, bool want_write) noexcept
{
    for (u32 i = 0; i < p.payload.resources.size(); ++i)
    {
        const ResourceRef& r = p.payload.resources[i];
        if (r.resource_id != res_id)
        {
            continue;
        }
        const bool writes = (r.access == SlotAccess::Write || r.access == SlotAccess::ReadWrite);
        const bool reads = (r.access == SlotAccess::Read || r.access == SlotAccess::ReadWrite);
        if (want_write ? writes : reads)
        {
            return true;
        }
    }
    return false;
}
} // namespace

bool compile(const FrameGraphTemplate& tmpl, const ExecutorRegistry& schemas, u32 width, u32 height,
             CompiledFrameGraph& out, DiagnosticList& diags)
{
    const Array<GraphPass>& passes = tmpl.passes();
    const Array<GraphResource>& resources = tmpl.resources();
    const u32 np = static_cast<u32>(passes.size());

    // 1. Validate: executor + payload against the schema registry; every bound resource is a declared graph resource.
    for (u32 i = 0; i < np; ++i)
    {
        if (!crd::renderpass::validate_payload(schemas, passes[i].payload, diags))
        {
            return false;
        }
        for (u32 j = 0; j < passes[i].payload.resources.size(); ++j)
        {
            if (tmpl.find_resource(passes[i].payload.resources[j].resource_id) == nullptr)
            {
                diags.error(DiagCode::InvalidSlot, "pass binds a resource that is not a declared graph resource");
                return false;
            }
        }
    }

    // 2. Deterministic schedule: an edge from a producer (latest earlier writer of a read resource) to the consumer;
    //    Kahn's algorithm with min-index tie-breaking.
    Array<u32> indeg(out.m_schedule.allocator());
    Array<u8> adj(out.m_schedule.allocator()); // np x np adjacency matrix (dedup edges)
    indeg.resize(np, 0U);
    adj.resize(static_cast<usize>(np) * np, u8{0});
    for (u32 i = 0; i < np; ++i)
    {
        for (u32 rr = 0; rr < passes[i].payload.resources.size(); ++rr)
        {
            const ResourceRef& ref = passes[i].payload.resources[rr];
            const bool reads = (ref.access == SlotAccess::Read || ref.access == SlotAccess::ReadWrite);
            if (!reads)
            {
                continue;
            }
            for (u32 j = i; j-- > 0;) // latest earlier writer
            {
                if (pass_touches(passes[j], ref.resource_id, true))
                {
                    if (adj[static_cast<usize>(j) * np + i] == 0U)
                    {
                        adj[static_cast<usize>(j) * np + i] = 1U;
                        ++indeg[i];
                    }
                    break;
                }
            }
        }
    }

    out.m_schedule.clear();
    Array<u8> emitted(out.m_schedule.allocator());
    emitted.resize(np, u8{0});
    for (u32 produced = 0; produced < np; ++produced)
    {
        u32 pick = np;
        for (u32 i = 0; i < np; ++i)
        {
            if (emitted[i] == 0U && indeg[i] == 0U)
            {
                pick = i;
                break;
            }
        }
        if (pick == np)
        {
            diags.error(DiagCode::CyclicDependency, "frame graph pass dependencies contain a cycle");
            return false;
        }
        emitted[pick] = 1U;
        out.m_schedule.push_back(pick);
        for (u32 k = 0; k < np; ++k)
        {
            if (adj[static_cast<usize>(pick) * np + k] != 0U && indeg[k] > 0U)
            {
                --indeg[k];
            }
        }
    }

    // 3. Lifetimes (in schedule order) + 4. aliasing. Persistent/History get dedicated slots; transients alias when
    //    their lifetimes do not overlap and their (size_class, kind) match.
    out.m_resources.clear();
    struct PhysSlot
    {
        u64 size_class;
        SlotResourceKind kind;
        u32 last_use;
        bool dedicated;
    };
    Array<PhysSlot> slots(out.m_resources.allocator());
    for (u32 ri = 0; ri < resources.size(); ++ri)
    {
        const GraphResource& res = resources[ri];
        // lifetime over schedule positions
        u32 first_use = np;
        u32 last_use = 0;
        bool used = false;
        for (u32 pos = 0; pos < out.m_schedule.size(); ++pos)
        {
            const GraphPass& p = passes[out.m_schedule[pos]];
            if (pass_touches(p, res.name_hash, false) || pass_touches(p, res.name_hash, true))
            {
                if (!used)
                {
                    first_use = pos;
                    used = true;
                }
                last_use = pos;
            }
        }

        u32 physical = 0;
        const bool transient = (res.lifetime == ResourceLifetime::Transient);
        bool assigned = false;
        if (transient && used)
        {
            for (u32 s = 0; s < slots.size(); ++s)
            {
                if (!slots[s].dedicated && slots[s].kind == res.kind && slots[s].size_class == res.size_class &&
                    slots[s].last_use < first_use)
                {
                    physical = s;
                    slots[s].last_use = last_use;
                    assigned = true;
                    break;
                }
            }
        }
        if (!assigned)
        {
            physical = static_cast<u32>(slots.size());
            slots.push_back(PhysSlot{res.size_class, res.kind, used ? last_use : 0U, !transient});
        }
        out.m_resources.push_back(CompiledResource{res.name_hash, physical, res.lifetime, res.kind, res.size_class});
    }

    out.m_physical_slot_count = static_cast<u32>(slots.size());
    out.m_width = width;
    out.m_height = height;
    return true;
}

// ── execute ──
bool execute(const CompiledFrameGraph& compiled, const FrameGraphTemplate& tmpl, const GraphExecutorTable& records,
             const ResourceTable& table, const PassPrograms& programs, ICommandEncoder& encoder, DiagnosticList& diags)
{
    for (u32 pos = 0; pos < compiled.schedule().size(); ++pos)
    {
        const GraphPass& pass = tmpl.passes()[compiled.schedule()[pos]];
        const PassRecordFn fn = records.find(pass.payload.executor);
        if (fn == nullptr)
        {
            diags.error(DiagCode::UnknownExecutor, "no record function registered for this executor");
            return false;
        }
        RecordContext ctx(pass.payload, table, programs, diags);
        fn(pass.payload, ctx, encoder);
        if (!ctx.ok())
        {
            return false; // a record function touched an undeclared resource
        }
    }
    return true;
}
} // namespace crd::rendergraph
