#include <crd/perf/gpu/gpu_resources_diag.hpp>

#include <crd/core/budget_pressure.hpp>
#include <crd/gpu/context.hpp>
#include <crd/gpu/device_pressure.hpp>
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/identity_registry.hpp>
#include <crd/gpu/object_identity.hpp>
#include <crd/gpu/queue_pressure.hpp>
#include <crd/gpu/validation.hpp>

namespace crd::perf::gpu
{
namespace
{
namespace cont = crd::containers;

using crd::perf::DiagFields;
using crd::perf::DiagStatus;

[[nodiscard]] cont::StringView backend_name(crd::gpu::GpuBackend b) noexcept
{
    switch (b) // no default: every backend is named
    {
    case crd::gpu::GpuBackend::Vulkan: return cont::StringView{"vulkan"};
    case crd::gpu::GpuBackend::Cuda: return cont::StringView{"cuda"};
    case crd::gpu::GpuBackend::Metal: return cont::StringView{"metal"};
    case crd::gpu::GpuBackend::Dx12: return cont::StringView{"dx12"};
    case crd::gpu::GpuBackend::WebGpu: return cont::StringView{"webgpu"};
    case crd::gpu::GpuBackend::Hip: return cont::StringView{"hip"};
    }
    return cont::StringView{"?"};
}

// "active", or the reason the mode is off ("not-requested", "layer-absent", ...).
[[nodiscard]] cont::StringView mode_state(const crd::gpu::ValidationActivation& a, crd::gpu::ValidationMode m) noexcept
{
    if (a.is_active(m))
    {
        return cont::StringView{"active"};
    }
    return cont::StringView{crd::gpu::to_string(a.unsupported_reason(m))};
}

void add_unavailable(DiagFields& item, crd::perf::DiagSnapshot& out, cont::StringView kind, cont::StringView reason)
{
    item.clear();
    item.str("kind", kind).str("status", "unavailable").str("reason", reason);
    (void)out.add_item(item);
}

// DIAG.10c: one segment of a device memory reading as `<prefix>_budget`, `<prefix>_usage` and `<prefix>_level`, or
// `<prefix>: "unavailable"` when it was not read.
void add_segment(DiagFields& item, const crd::BudgetReading& r, const char* budget_key, const char* usage_key,
                 const char* level_key, const char* missing_key)
{
    if (!r.read)
    {
        item.str(missing_key, "unavailable");
        return;
    }
    item.u64(budget_key, r.capacity)
        .u64(usage_key, r.in_use)
        .str(level_key, cont::StringView{crd::pressure_level_name(crd::pressure_level(r, r.in_use))});
}
} // namespace

bool GpuResourcesCommand::add_context(const crd::gpu::IGpuContext& context) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_context_count >= kGpuResourcesMaxContexts)
    {
        return false;
    }
    for (crd::u32 i = 0U; i < m_context_count; ++i)
    {
        if (m_contexts[i] == &context)
        {
            return false;
        }
    }
    m_contexts[m_context_count++] = &context;
    return true;
}

bool GpuResourcesCommand::remove_context(const crd::gpu::IGpuContext& context) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (crd::u32 i = 0U; i < m_context_count; ++i)
    {
        if (m_contexts[i] == &context)
        {
            // Keep the registration order of the rest: the answer lists contexts in that order.
            for (crd::u32 j = i + 1U; j < m_context_count; ++j)
            {
                m_contexts[j - 1U] = m_contexts[j];
            }
            m_contexts[--m_context_count] = nullptr;
            return true;
        }
    }
    return false;
}

bool GpuResourcesCommand::add_frame_graph(const crd::gpu::IFrameGraph& graph, cont::StringView label) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_graph_count >= kGpuResourcesMaxFrameGraphs)
    {
        return false;
    }
    for (crd::u32 i = 0U; i < m_graph_count; ++i)
    {
        if (m_graphs[i] == &graph)
        {
            return false;
        }
    }
    m_graphs[m_graph_count] = &graph;
    m_labels[m_graph_count] = label;
    ++m_graph_count;
    return true;
}

bool GpuResourcesCommand::remove_frame_graph(const crd::gpu::IFrameGraph& graph) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (crd::u32 i = 0U; i < m_graph_count; ++i)
    {
        if (m_graphs[i] == &graph)
        {
            for (crd::u32 j = i + 1U; j < m_graph_count; ++j)
            {
                m_graphs[j - 1U] = m_graphs[j];
                m_labels[j - 1U] = m_labels[j];
            }
            --m_graph_count;
            m_graphs[m_graph_count] = nullptr;
            m_labels[m_graph_count] = cont::StringView{};
            return true;
        }
    }
    return false;
}

crd::u32 GpuResourcesCommand::context_count() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_context_count;
}

crd::u32 GpuResourcesCommand::frame_graph_count() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_graph_count;
}

DiagStatus GpuResourcesCommand::run(void* self, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out)
{
    (void)call;
    auto* const cmd = static_cast<GpuResourcesCommand*>(self);
    cmd->m_runs.fetch_add(1U, std::memory_order_acq_rel);
    const std::lock_guard<std::mutex> lock(cmd->m_mutex);

    crd::gpu::IdentityRegistry& registry = crd::gpu::identity_registry();
    const crd::u64 live_res  = registry.live_count(crd::gpu::ObjectKind::Resource);
    const crd::u64 live_prog = registry.live_count(crd::gpu::ObjectKind::Program);
    const crd::u64 live_pass = registry.live_count(crd::gpu::ObjectKind::Pass);

    // DIAG.10c: each context's device memory, read once per run (the reading is a moment; the items and the summary
    // count agree because both come from this array).
    crd::gpu::DeviceMemoryReading memory[kGpuResourcesMaxContexts]{};
    crd::gpu::QueueDepthReading   queues[kGpuResourcesMaxContexts]{};
    crd::u64                      memory_read = 0U;
    crd::u64                      queue_read  = 0U;
    for (crd::u32 i = 0U; i < cmd->m_context_count; ++i)
    {
        memory[i] = cmd->m_contexts[i]->device_memory();
        if (memory[i].source != crd::gpu::DeviceMemorySource::Unavailable)
        {
            ++memory_read;
        }
        queues[i] = cmd->m_contexts[i]->queue_pressure(); // DIAG.10c: the completion seams' view of its queues
        if (queues[i].depth.read)
        {
            ++queue_read;
        }
    }

    out.summary.u64("live_resources", live_res)
        .u64("live_programs", live_prog)
        .u64("live_passes", live_pass)
        .u64("contexts", cmd->m_context_count)
        .u64("frame_graphs", cmd->m_graph_count)
        .u64("device_memory_read", memory_read)
        .u64("queue_depth_read", queue_read);

    DiagFields item(out.allocator());

    // The identity registry is one index space for the whole process, whichever contexts the host registered.
    const crd::gpu::ObjectKind kinds[]  = {crd::gpu::ObjectKind::Resource, crd::gpu::ObjectKind::Program,
                                           crd::gpu::ObjectKind::Pass};
    const crd::u64             counts[] = {live_res, live_prog, live_pass};
    for (crd::u32 k = 0U; k < 3U; ++k)
    {
        item.clear();
        item.str("kind", "identities")
            .str("object", cont::StringView{crd::gpu::to_string(kinds[k])})
            .str("scope", "process")
            .u64("live", counts[k]);
        (void)out.add_item(item);
    }

    if (cmd->m_context_count == 0U)
    {
        add_unavailable(item, out, "context", "the host registered no GPU context with this command");
    }
    for (crd::u32 i = 0U; i < cmd->m_context_count; ++i)
    {
        const crd::gpu::IGpuContext&          ctx        = *cmd->m_contexts[i];
        const crd::gpu::ValidationActivation  activation = ctx.validation_activation();
        const char* const                     adapter    = ctx.adapter_name();
        item.clear();
        item.str("kind", "context")
            .u64("index", i)
            .str("backend", backend_name(ctx.backend()))
            .str("adapter", adapter != nullptr ? cont::StringView{adapter} : cont::StringView{})
            .boolean("valid", ctx.valid())
            .str("core", mode_state(activation, crd::gpu::ValidationMode::Core))
            .str("synchronization", mode_state(activation, crd::gpu::ValidationMode::Synchronization))
            .str("gpu_assisted", mode_state(activation, crd::gpu::ValidationMode::GpuAssisted));
        // DIAG.10c: the queue depth (unbounded, so its level is reported, never judged against a bound).
        const crd::gpu::QueueDepthReading& q = queues[i];
        if (q.depth.read)
        {
            item.u64("queue_in_flight", q.counts.in_flight)
                .u64("queue_high_water", q.counts.high_water)
                .u64("queue_submitted", q.counts.submitted)
                .u64("queue_retired", q.counts.retired)
                .u64("queue_abandoned", q.counts.abandoned)
                .u64("queues", q.counts.queues);
        }
        else
        {
            item.str("queue_depth", "unavailable")
                .str("queue_reason", cont::StringView{q.reason != nullptr ? q.reason : "queue depth was not read"});
        }
        (void)out.add_item(item);

        const crd::gpu::DeviceMemoryReading& m = memory[i];
        item.clear();
        item.str("kind", "heap").u64("context", i);
        if (m.source == crd::gpu::DeviceMemorySource::Unavailable)
        {
            item.str("status", "unavailable")
                .str("reason", cont::StringView{m.reason != nullptr ? m.reason : "device memory was not read"});
        }
        else
        {
            item.str("status", "read").str("source", cont::StringView{crd::gpu::device_memory_source_name(m.source)});
            add_segment(item, m.local, "local_budget", "local_usage", "local_level", "local");
            add_segment(item, m.non_local, "non_local_budget", "non_local_usage", "non_local_level", "non_local");
            if (m.reason != nullptr && m.reason[0] != '\0')
            {
                item.str("reason", cont::StringView{m.reason});
            }
        }
        (void)out.add_item(item);
    }

    if (cmd->m_graph_count == 0U)
    {
        add_unavailable(item, out, "frame-graph", "the host registered no frame graph with this command");
    }
    for (crd::u32 i = 0U; i < cmd->m_graph_count; ++i)
    {
        const crd::gpu::IFrameGraph& graph = *cmd->m_graphs[i];
        item.clear();
        item.str("kind", "frame-graph")
            .u64("index", i)
            .str("label", cmd->m_labels[i])
            .u64("transient_bytes", graph.transient_memory_bytes())
            .u64("transient_logical_bytes", graph.transient_logical_bytes())
            .boolean("budget_exceeded", graph.last_build_exceeded_budget())
            .u64("barriers", graph.last_barrier_count())
            .u64("submits", graph.last_submit_count())
            .u64("passes", graph.pass_count())
            .u64("async_passes", graph.last_async_pass_count())
            .u64("presents", graph.last_present_count())
            .boolean("gpu_timing", graph.gpu_timing_available());
        // DIAG.10c: the transient budget as a reading (its bound is 0 when the graph has none).
        const crd::BudgetReading transient = graph.transient_pressure();
        if (transient.read)
        {
            item.u64("transient_budget", transient.bounded ? transient.capacity : 0U)
                .u64("transient_high_water", transient.high_water)
                .u64("budget_refusals", transient.refusals)
                .str("transient_level",
                     cont::StringView{crd::pressure_level_name(crd::pressure_level(transient, transient.in_use))});
        }
        else
        {
            item.str("transient_pressure", "unavailable");
        }
        (void)out.add_item(item);
    }
    return DiagStatus::Ok;
}

bool register_gpu_resources(crd::perf::DiagCommandService& service, GpuResourcesCommand& command)
{
    crd::perf::DiagCommandSpec spec;
    spec.name       = kGpuResourcesCommand;
    spec.owner      = cont::StringView{"gpu"};
    spec.summary    = cont::StringView{"live GPU identities, the host's GPU contexts and its frame graphs' footprints"};
    spec.authority  = crd::perf::DiagAuthority::Read;
    spec.takes_path = false;
    return service.register_command(spec, &GpuResourcesCommand::run, &command);
}
} // namespace crd::perf::gpu
