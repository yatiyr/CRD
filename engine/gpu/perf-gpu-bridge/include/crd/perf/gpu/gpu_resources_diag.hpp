#pragma once

// crd-perf-gpu-bridge -- the GPU resource summary diagnostic command.
//
// `gpu.resources` (Read, no path) answers what the GPU side of this process holds right now:
//   - the process-wide identity registry's live resource, program and pass identities (one index space shared by every
//     backend, so the counts cover every context in the process, registered here or not);
//   - for each GPU context the host registered: its backend, adapter, whether it is valid, and each validation mode's
//     state (active, or the reason it is not);
//   - for each frame graph the host registered: its last build's transient footprint after and before aliasing, and
//     its last execute's barrier, submit, pass, async-pass and present counts, budget refusal and timing support.
// Evidence that cannot exist is an item with "status":"unavailable" and a reason: no backend queries device heap
// usage, so heap usage is always unavailable, and a host that registered no context or no frame graph is told so.
//
// The command lives in this bridge, not in crd-gpu-context: crd-gpu-context may not link crd-perf, and this module is
// the one place that names both (see its CMakeLists). It is registered into a crd-perf DiagCommandService, so a native
// caller, the CLI verb and the MCP tool get the same bounded, paginated answer under the same authority checks.
//
// Threads: the handler runs on the service caller's thread. Context reads (backend, adapter, validation activation)
// are immutable after creation and the identity registry takes its own lock, so both are safe from any thread. A frame
// graph's counters are not synchronized with its build/execute: a host registers a graph only when the service is
// called from the thread that drives it (or while nothing drives it). The registered lists take this object's lock,
// so a host may add and remove entries while another thread calls the service. Contract:
// docs/design/runtime-diagnostics.md.

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/diag_commands.hpp>

#include <atomic>
#include <mutex>

namespace crd::gpu
{
class IFrameGraph;
class IGpuContext;
} // namespace crd::gpu

namespace crd::perf::gpu
{
inline constexpr crd::containers::StringView kGpuResourcesCommand{"gpu.resources"};

inline constexpr crd::u32 kGpuResourcesMaxContexts    = 8U; // the GpuContextManager's own bound
inline constexpr crd::u32 kGpuResourcesMaxFrameGraphs = 8U;

// The host's configuration of the command. The host owns it, keeps it alive at least as long as every service it is
// registered with, and removes a context or frame graph before destroying it.
class GpuResourcesCommand
{
public:
    GpuResourcesCommand() = default;

    GpuResourcesCommand(const GpuResourcesCommand&)            = delete;
    GpuResourcesCommand& operator=(const GpuResourcesCommand&) = delete;
    GpuResourcesCommand(GpuResourcesCommand&&)                 = delete;
    GpuResourcesCommand& operator=(GpuResourcesCommand&&)      = delete;

    // Summarize `context`. Returns false when it is already registered or the list is full.
    [[nodiscard]] bool add_context(const crd::gpu::IGpuContext& context) noexcept;
    // Stop summarizing `context`. Returns false when it was not registered.
    bool remove_context(const crd::gpu::IGpuContext& context) noexcept;

    // Summarize `graph` under `label` (static storage; empty is allowed). Returns false when the graph is already
    // registered or the list is full.
    [[nodiscard]] bool add_frame_graph(const crd::gpu::IFrameGraph& graph, crd::containers::StringView label) noexcept;
    bool               remove_frame_graph(const crd::gpu::IFrameGraph& graph) noexcept;

    [[nodiscard]] crd::u32 context_count() const noexcept;
    [[nodiscard]] crd::u32 frame_graph_count() const noexcept;

    // Handler runs (requests that passed every service check).
    [[nodiscard]] crd::u64 runs() const noexcept { return m_runs.load(std::memory_order_acquire); }

private:
    friend bool register_gpu_resources(crd::perf::DiagCommandService& service, GpuResourcesCommand& command);

    static crd::perf::DiagStatus run(void* self, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out);

    mutable std::mutex           m_mutex;
    const crd::gpu::IGpuContext* m_contexts[kGpuResourcesMaxContexts]{};
    crd::u32                     m_context_count = 0U;
    const crd::gpu::IFrameGraph* m_graphs[kGpuResourcesMaxFrameGraphs]{};
    crd::containers::StringView  m_labels[kGpuResourcesMaxFrameGraphs]{};
    crd::u32                     m_graph_count = 0U;
    std::atomic<crd::u64>        m_runs{0U};
};

// Register `gpu.resources` (Read, no path) with `service`. Returns false when the service refuses the registration
// (a duplicate name or a full table).
[[nodiscard]] bool register_gpu_resources(crd::perf::DiagCommandService& service, GpuResourcesCommand& command);
} // namespace crd::perf::gpu
