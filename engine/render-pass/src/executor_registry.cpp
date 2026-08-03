#include <crd/renderpass/executor_registry.hpp>

#include <crd/containers/hash.hpp>

#include <utility> // std::swap

namespace crd::renderpass
{
ExecutorTypeId executor_type_id(StringView name) noexcept { return ExecutorTypeId{crd::containers::hash_string(name)}; }
u64 pass_param_id(StringView name) noexcept { return crd::containers::hash_string(name); }

usize ExecutorRegistry::lower_bound(ExecutorTypeId id) const noexcept
{
    usize lo = 0;
    usize hi = m_executors.size();
    while (lo < hi)
    {
        const usize mid = lo + (hi - lo) / 2;
        if (m_executors[mid].id < id)
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

const PassExecutorDesc* ExecutorRegistry::find(ExecutorTypeId id) const noexcept
{
    const usize idx = lower_bound(id);
    if (idx < m_executors.size() && m_executors[idx].id == id)
    {
        return &m_executors[idx];
    }
    return nullptr;
}

bool ExecutorRegistry::register_executor(const PassExecutorDesc& desc, DiagnosticList& diags)
{
    const usize idx = lower_bound(desc.id);
    if (idx < m_executors.size() && m_executors[idx].id == desc.id)
    {
        diags.error(DiagCode::DuplicateExecutor, "an executor with this id is already registered", desc.name);
        return false;
    }
    m_executors.push_back(desc);
    for (usize j = m_executors.size() - 1; j > idx; --j)
    {
        std::swap(m_executors[j], m_executors[j - 1]);
    }
    return true;
}

namespace
{
ParamSpec param(StringView name, ExecutorParamType type, bool required = true)
{
    return ParamSpec{pass_param_id(name), type, required};
}
ResourceSlotSpec slot(StringView name, SlotResourceKind kind, SlotAccess access, bool required = true)
{
    return ResourceSlotSpec{pass_param_id(name), kind, access, required};
}

PassExecutorDesc make_executor(StringView name, QueueKind queue)
{
    PassExecutorDesc d;
    d.id = executor_type_id(name);
    d.name = name;
    d.schema.version = 1U;
    d.schema.queue = queue;
    return d;
}
} // namespace

u32 register_builtin_executors(ExecutorRegistry& registry, DiagnosticList& diags)
{
    u32 count = 0;

    // scene.raster — draw scene geometry into colour (+ optional depth).
    {
        PassExecutorDesc d = make_executor("scene.raster", QueueKind::Graphics);
        d.schema.params.push_back(param("clear_color", ExecutorParamType::Vec4));
        d.schema.params.push_back(param("clear_depth", ExecutorParamType::F32));
        d.schema.params.push_back(param("depth_compare", ExecutorParamType::Enum));
        d.schema.params.push_back(param("load", ExecutorParamType::Bool, false));
        // REN-40-G1: LOAD depth (from a depth prepass) while CLEARING colour — the depth-prepass consumer shape.
        d.schema.params.push_back(param("load_depth", ExecutorParamType::Bool, false));
        // ⭐ RAF-8: OPTIONAL. A DEPTH-ONLY geometry pass (a shadow cascade, a depth prepass) writes only `depth` — no
        // colour — so `color` cannot be required; the executor renders depth-only when `color` is absent.
        d.schema.slots.push_back(slot("color", SlotResourceKind::ColorTarget, SlotAccess::Write, false));
        d.schema.slots.push_back(slot("depth", SlotResourceKind::DepthTarget, SlotAccess::ReadWrite, false));
        // RAF-8: OPTIONAL. A scene pass driven by a RESOLVED DRAW LIST supplies geometry PER-ITEM (each draw carries
        // its own storage buffer), so there is no single geometry slot; the slot is used only by the legacy
        // single-draw branch (an authored pass that names one geometry buffer and no draw list).
        d.schema.slots.push_back(slot("geometry", SlotResourceKind::StorageBuffer, SlotAccess::Read, false));
        // ⭐ RAF-8: MRT colour attachments (deferred G-buffer / visibility split) — a geometry pass may write N
        // colour targets, expressed as DATA (color1..3) rather than a distinct executor. color0 is `color` above.
        d.schema.slots.push_back(slot("color1", SlotResourceKind::ColorTarget, SlotAccess::Write, false));
        d.schema.slots.push_back(slot("color2", SlotResourceKind::ColorTarget, SlotAccess::Write, false));
        d.schema.slots.push_back(slot("color3", SlotResourceKind::ColorTarget, SlotAccess::Write, false));
        // ⭐ RAF-8: SAMPLED READS a geometry pass declares for SCHEDULING (the shadow atlas a shadowed-forward pass
        // reads — the barrier ordering). The record function binds the actual sampler off the resolved DrawList
        // (`pass_texture`); these slots exist so the graph ORDERS the atlas write before this read.
        d.schema.slots.push_back(slot("input0", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input1", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input2", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input3", SlotResourceKind::Texture, SlotAccess::Read, false));
        // ⭐ RAF-8: BUFFER reads a geometry pass declares for SCHEDULING — the GPU-cull command buffers (`instances`,
        // `cull_args`) an indirect forward pass reads, so the graph orders the cull WRITE before this draw READ. The
        // record function binds them off the resolved DrawList (`args`), not these slots; they exist for the barrier.
        d.schema.slots.push_back(slot("read_buffer0", SlotResourceKind::StorageBuffer, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("read_buffer1", SlotResourceKind::StorageBuffer, SlotAccess::Read, false));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // fullscreen.raster — a fullscreen pass into colour. RAF-8: full parity with the live RasterFullscreen —
    // 0 reads (procedural) · 1 read (textured / shadow-compare) · N reads (bindless, in input0..N-1 order) · an
    // optional constants buffer (the TAA-resolve shape) · VRS / conservative raster (draw ATTRIBUTES, not kinds).
    {
        PassExecutorDesc d = make_executor("fullscreen.raster", QueueKind::Graphics);
        d.schema.params.push_back(param("blend", ExecutorParamType::Enum, false));
        // ⭐ RAF-8: the COMPOSITE shape — LOAD the target (not clear) and BLEND over it (WBOIT's resolve). `load` +
        // `blend` turn an ordinary fullscreen bindless draw into draw_bindless_blend_load; absent ⇒ the clearing draw.
        d.schema.params.push_back(param("load", ExecutorParamType::Bool, false));
        d.schema.params.push_back(param("shading_rate", ExecutorParamType::Enum, false));   // VRS pipeline rate (0 = 1x1)
        d.schema.params.push_back(param("conservative", ExecutorParamType::Enum, false));   // 0 = off
        // ⭐ RAF-8 (REN-40-G3): read a depth input as RAW FLOAT (ordinary sampler), not through a comparison sampler —
        // the HZB builder and TAA reprojection need the stored depth value, not a pass/fail shadow test.
        d.schema.params.push_back(param("depth_as_float", ExecutorParamType::Bool, false));
        d.schema.slots.push_back(slot("color", SlotResourceKind::ColorTarget, SlotAccess::Write));
        d.schema.slots.push_back(slot("input0", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input1", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input2", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input3", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input4", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input5", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input6", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("input7", SlotResourceKind::Texture, SlotAccess::Read, false));
        d.schema.slots.push_back(slot("constants", SlotResourceKind::StorageBuffer, SlotAccess::Read, false));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // compute.dispatch — a compute kernel over a storage buffer.
    {
        PassExecutorDesc d = make_executor("compute.dispatch", QueueKind::Compute);
        d.schema.params.push_back(param("groups_x", ExecutorParamType::U32));
        d.schema.params.push_back(param("groups_y", ExecutorParamType::U32));
        d.schema.params.push_back(param("groups_z", ExecutorParamType::U32));
        // ⭐ RAF-8: the GPU-DRIVEN dispatch — the workgroup count comes from `args` (a buffer an EARLIER pass wrote as
        // {x,y,z}), not the CPU. When `args` is bound the dispatch is INDIRECT (dispatch_kernel_indirect); absent ⇒ the
        // grid comes from groups_*. `args_offset` is the BYTE offset of the {x,y,z} triple in that buffer.
        d.schema.params.push_back(param("args_offset", ExecutorParamType::U32, false));
        d.schema.slots.push_back(slot("storage", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite));
        // ⭐ RAF-8: a real kernel reads/writes SEVERAL buffers (cull reads geometry + writes visibility+args; skin
        // reads pose + writes vertices). storage1..3 declare the extra buffers so the graph orders them; the primary
        // is `storage` above. Read-vs-write access is per-ref, so one slot can be a read in one pass, a write in another.
        d.schema.slots.push_back(slot("storage1", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        d.schema.slots.push_back(slot("storage2", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        d.schema.slots.push_back(slot("storage3", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        // ⭐ RAF-8: the INDIRECT ARGS buffer (the {x,y,z} workgroup count a cull pass wrote). Read-only; its presence
        // selects the indirect dispatch. Declared distinctly from `storage*` so the graph transitions it to the
        // indirect-argument state the dispatch needs, not the generic UAV/SRV state a data buffer takes.
        d.schema.slots.push_back(slot("args", SlotResourceKind::StorageBuffer, SlotAccess::Read, false));
        // ⭐ RAF-8: a kernel that SAMPLES a texture (the HZB an occlusion cull reads). Bound at the fixed post-buffer
        // position via dispatch_kernel_sampled; the graph orders the HZB write before this read.
        d.schema.slots.push_back(slot("sampled", SlotResourceKind::Texture, SlotAccess::Read, false));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // transfer.clear — clear a target.
    {
        PassExecutorDesc d = make_executor("transfer.clear", QueueKind::Transfer);
        d.schema.params.push_back(param("clear_color", ExecutorParamType::Vec4));
        d.schema.slots.push_back(slot("target", SlotResourceKind::ColorTarget, SlotAccess::Write));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // transfer.copy — exact target-to-target copy.
    {
        PassExecutorDesc d = make_executor("transfer.copy", QueueKind::Transfer);
        d.schema.slots.push_back(slot("src", SlotResourceKind::ColorTarget, SlotAccess::Read));
        d.schema.slots.push_back(slot("dst", SlotResourceKind::ColorTarget, SlotAccess::Write));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // transfer.blit — rescaling target-to-target copy.
    {
        PassExecutorDesc d = make_executor("transfer.blit", QueueKind::Transfer);
        d.schema.params.push_back(param("filter", ExecutorParamType::Enum));
        d.schema.slots.push_back(slot("src", SlotResourceKind::ColorTarget, SlotAccess::Read));
        d.schema.slots.push_back(slot("dst", SlotResourceKind::ColorTarget, SlotAccess::Write));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // transfer.resolve — MSAA resolve.
    {
        PassExecutorDesc d = make_executor("transfer.resolve", QueueKind::Transfer);
        d.schema.slots.push_back(slot("src", SlotResourceKind::ColorTarget, SlotAccess::Read));
        d.schema.slots.push_back(slot("dst", SlotResourceKind::ColorTarget, SlotAccess::Write));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // raytrace.dispatch — an INLINE RAY-QUERY dispatch (VK_KHR_ray_query / DXR-1.1 inline RayQuery). The TLAS binds at
    // set 0/binding 0, the pass's storage buffers at 1..N, and a ray-query KERNEL runs into the frame's one submission
    // (dispatch_kernel_rt) — NOT a ray-tracing pipeline (no SBT). `accel` is required (a trace with none misses every
    // ray); `storage*` are the buffers the kernel reads/writes (the ray-hit output among them).
    {
        PassExecutorDesc d = make_executor("raytrace.dispatch", QueueKind::Compute);
        d.schema.params.push_back(param("groups_x", ExecutorParamType::U32));
        d.schema.params.push_back(param("groups_y", ExecutorParamType::U32));
        d.schema.params.push_back(param("groups_z", ExecutorParamType::U32, false));
        d.schema.slots.push_back(slot("accel", SlotResourceKind::AccelStructure, SlotAccess::Read));
        d.schema.slots.push_back(slot("storage", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite));
        d.schema.slots.push_back(slot("storage1", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        d.schema.slots.push_back(slot("storage2", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        d.schema.slots.push_back(slot("storage3", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // raytrace.pipeline — a ray-tracing PIPELINE trace (an SBT: raygen · miss · closest-hit + optional any-hit /
    // intersection / callable), recorded as trace_rays / _anyhit / _full into the frame's one submission. The ray-gen
    // GRID is groups_x × groups_y (the width × height the raygen shader indexes); the SBT stage programs come from the
    // host-resolved PassPrograms, so no program slot — only the accel read + the storage buffers the shaders touch.
    {
        PassExecutorDesc d = make_executor("raytrace.pipeline", QueueKind::Compute);
        d.schema.params.push_back(param("groups_x", ExecutorParamType::U32));
        d.schema.params.push_back(param("groups_y", ExecutorParamType::U32, false));
        d.schema.slots.push_back(slot("accel", SlotResourceKind::AccelStructure, SlotAccess::Read));
        d.schema.slots.push_back(slot("storage", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        d.schema.slots.push_back(slot("storage1", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        d.schema.slots.push_back(slot("storage2", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        d.schema.slots.push_back(slot("storage3", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite, false));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // mesh.raster — a MESH-SHADER amplification pass. Colour-only; the workgroup count is per-draw (the resolved draw
    // list) or, with no list, the declared `amplify_count` (a procedural mesh grid). A draw carrying a storage buffer
    // pulls its meshlets from it (GEO-1); the first draw clears, every later one loads.
    {
        PassExecutorDesc d = make_executor("mesh.raster", QueueKind::Graphics);
        d.schema.params.push_back(param("clear_color", ExecutorParamType::Vec4));
        d.schema.params.push_back(param("amplify_count", ExecutorParamType::U32, false));
        d.schema.slots.push_back(slot("color", SlotResourceKind::ColorTarget, SlotAccess::Write));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // tess.raster — a TESSELLATION amplification pass (the portable displacement path). Same shape as mesh.raster; the
    // count is a PATCH count. A draw carrying a storage buffer supplies its control points (GEO-1).
    {
        PassExecutorDesc d = make_executor("tess.raster", QueueKind::Graphics);
        d.schema.params.push_back(param("clear_color", ExecutorParamType::Vec4));
        d.schema.params.push_back(param("amplify_count", ExecutorParamType::U32, false));
        d.schema.slots.push_back(slot("color", SlotResourceKind::ColorTarget, SlotAccess::Write));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // mesh.indirect — the GPU-DRIVEN meshlet dispatch: the mesh-workgroup count comes from `args` (a buffer a compute
    // cull pass wrote), consumed by draw_mesh_indirect_buffer. Colour-only. `args_offset` is the BYTE offset.
    {
        PassExecutorDesc d = make_executor("mesh.indirect", QueueKind::Graphics);
        d.schema.params.push_back(param("clear_color", ExecutorParamType::Vec4));
        d.schema.params.push_back(param("args_offset", ExecutorParamType::U32, false));
        d.schema.slots.push_back(slot("color", SlotResourceKind::ColorTarget, SlotAccess::Write));
        d.schema.slots.push_back(slot("args", SlotResourceKind::StorageBuffer, SlotAccess::Read));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // visbuffer.raster — the HW-raster half of a Nanite split: draw a VS→FS program into an R32_UINT visibility target,
    // clearing the id to `clear_id`. Each draw (the resolved list) writes its primitive ids; the first clears, every
    // later one LOADS (draw_visbuffer_load) so the one image holds EVERY visible primitive's id.
    {
        PassExecutorDesc d = make_executor("visbuffer.raster", QueueKind::Graphics);
        d.schema.params.push_back(param("clear_id", ExecutorParamType::U32));
        d.schema.slots.push_back(slot("color", SlotResourceKind::ColorTarget, SlotAccess::Write));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // present — present a source image.
    {
        PassExecutorDesc d = make_executor("present", QueueKind::Graphics);
        d.schema.slots.push_back(slot("source", SlotResourceKind::ColorTarget, SlotAccess::Read));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    return count;
}

namespace
{
const ParamSpec* find_param_spec(const ExecutorSchema& schema, u64 name_hash) noexcept
{
    for (u32 i = 0; i < schema.params.size(); ++i)
    {
        if (schema.params[i].name_hash == name_hash)
        {
            return &schema.params[i];
        }
    }
    return nullptr;
}
const ResourceSlotSpec* find_slot_spec(const ExecutorSchema& schema, u64 name_hash) noexcept
{
    for (u32 i = 0; i < schema.slots.size(); ++i)
    {
        if (schema.slots[i].name_hash == name_hash)
        {
            return &schema.slots[i];
        }
    }
    return nullptr;
}
} // namespace

bool validate_payload(const ExecutorRegistry& registry, const PassPayload& payload, DiagnosticList& diags)
{
    const PassExecutorDesc* desc = registry.find(payload.executor);
    if (desc == nullptr)
    {
        diags.error(DiagCode::UnknownExecutor, "pass references an unregistered executor id");
        return false;
    }
    if (payload.schema_version != desc->schema.version)
    {
        diags.emit(Severity::Error, DiagCode::SchemaMismatch, "pass payload was cooked against a different executor schema",
                   desc->name);
        return false;
    }
    if (payload.queue != desc->schema.queue)
    {
        diags.error(DiagCode::QueueMismatch, "pass payload queue differs from the executor's declared queue", desc->name);
        return false;
    }

    // Every payload param must match a schema param by name + type (no unknown / mistyped params).
    for (u32 i = 0; i < payload.params.size(); ++i)
    {
        const ParamValue& pv = payload.params[i];
        const ParamSpec* spec = find_param_spec(desc->schema, pv.name_hash);
        if (spec == nullptr || spec->type != pv.value.type)
        {
            diags.error(DiagCode::InvalidParam, "pass payload has an unknown or type-mismatched parameter", desc->name);
            return false;
        }
    }
    // Every REQUIRED schema param must be present.
    for (u32 i = 0; i < desc->schema.params.size(); ++i)
    {
        const ParamSpec& spec = desc->schema.params[i];
        if (!spec.required)
        {
            continue;
        }
        bool found = false;
        for (u32 j = 0; j < payload.params.size(); ++j)
        {
            if (payload.params[j].name_hash == spec.name_hash)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            diags.error(DiagCode::InvalidParam, "pass payload is missing a required parameter", desc->name);
            return false;
        }
    }

    // Every payload resource must match a schema slot by name + kind + access.
    for (u32 i = 0; i < payload.resources.size(); ++i)
    {
        const ResourceRef& rr = payload.resources[i];
        const ResourceSlotSpec* spec = find_slot_spec(desc->schema, rr.slot_name_hash);
        if (spec == nullptr || spec->kind != rr.kind || spec->access != rr.access)
        {
            diags.error(DiagCode::InvalidSlot, "pass payload has an unknown or mismatched resource slot", desc->name);
            return false;
        }
        if (rr.resource_id == 0U)
        {
            diags.error(DiagCode::InvalidSlot, "pass payload resource slot is bound to nothing", desc->name);
            return false;
        }
    }
    // Every REQUIRED schema slot must be bound.
    for (u32 i = 0; i < desc->schema.slots.size(); ++i)
    {
        const ResourceSlotSpec& spec = desc->schema.slots[i];
        if (!spec.required)
        {
            continue;
        }
        bool bound = false;
        for (u32 j = 0; j < payload.resources.size(); ++j)
        {
            if (payload.resources[j].slot_name_hash == spec.name_hash && payload.resources[j].resource_id != 0U)
            {
                bound = true;
                break;
            }
        }
        if (!bound)
        {
            diags.error(DiagCode::InvalidSlot, "pass payload is missing a required resource slot", desc->name);
            return false;
        }
    }
    return true;
}
} // namespace crd::renderpass
