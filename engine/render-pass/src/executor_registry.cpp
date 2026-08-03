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
        d.schema.slots.push_back(slot("color", SlotResourceKind::ColorTarget, SlotAccess::Write));
        d.schema.slots.push_back(slot("depth", SlotResourceKind::DepthTarget, SlotAccess::ReadWrite, false));
        d.schema.slots.push_back(slot("geometry", SlotResourceKind::StorageBuffer, SlotAccess::Read));
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // fullscreen.raster — a fullscreen pass sampling one input into colour.
    {
        PassExecutorDesc d = make_executor("fullscreen.raster", QueueKind::Graphics);
        d.schema.params.push_back(param("blend", ExecutorParamType::Enum, false));
        d.schema.slots.push_back(slot("color", SlotResourceKind::ColorTarget, SlotAccess::Write));
        d.schema.slots.push_back(slot("input", SlotResourceKind::Texture, SlotAccess::Read, false)); // procedural passes need none
        count += registry.register_executor(d, diags) ? 1U : 0U;
    }
    // compute.dispatch — a compute kernel over a storage buffer.
    {
        PassExecutorDesc d = make_executor("compute.dispatch", QueueKind::Compute);
        d.schema.params.push_back(param("groups_x", ExecutorParamType::U32));
        d.schema.params.push_back(param("groups_y", ExecutorParamType::U32));
        d.schema.params.push_back(param("groups_z", ExecutorParamType::U32));
        d.schema.slots.push_back(slot("storage", SlotResourceKind::StorageBuffer, SlotAccess::ReadWrite));
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
    // raytrace.dispatch — a ray-tracing dispatch over an acceleration structure.
    {
        PassExecutorDesc d = make_executor("raytrace.dispatch", QueueKind::Compute);
        d.schema.params.push_back(param("width", ExecutorParamType::U32));
        d.schema.params.push_back(param("height", ExecutorParamType::U32));
        d.schema.slots.push_back(slot("output", SlotResourceKind::StorageBuffer, SlotAccess::Write));
        d.schema.slots.push_back(slot("accel", SlotResourceKind::AccelStructure, SlotAccess::Read));
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
