// dx12_work_graph_context.hpp — CEIR-20c-1: the D3D12 Work Graphs offline rig (the Dx12RayTracingContext mold). Builds a
// work-graph STATE OBJECT from a NODE DXIL library (the emit_work_graph_node_hlsl output, compiled lib_6_8) and
// DispatchGraph's the entry node: the GPU SELF-SCHEDULES the producer -> consumer chain via grid-launch records, so the
// consumer is sized by the device count with NO host submit boundary between stages (the 20c gold-standard vs 20b's
// host-read fallback). WINDOWS-ONLY; `valid()` is false unless the adapter reports D3D12_WORK_GRAPHS_TIER >= 1.0. The rig
// creates its own COMPUTE queue + one-shot command list + fence (mirroring Dx12RayTracingContext / Dx12ComputeContext);
// ceir-gpu names no backend — the CEIR-20c-1c executor drives this through caller hooks.
#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <memory>

namespace crd::gpu
{
class Dx12WorkGraphContext
{
public:
    Dx12WorkGraphContext();
    ~Dx12WorkGraphContext();
    Dx12WorkGraphContext(const Dx12WorkGraphContext&)            = delete;
    Dx12WorkGraphContext& operator=(const Dx12WorkGraphContext&) = delete;

    // OPTIONS21 WorkGraphsTier >= 1.0 AND the device/queue/list/fence all created. false ⇒ the caller SKIPS (like DXR/ray_query).
    [[nodiscard]] bool valid() const noexcept;

    // One UAV bound at root parameter `reg` (register u`reg`): `upload` (host->device before, or null = zero-init) +
    // `readback` (device->host after, or null) + `bytes`. The queue buffer (the produce header) + the output counter ride here.
    struct Binding
    {
        const void* upload   = nullptr;
        void*       readback = nullptr;
        crd::u64    bytes    = 0;
        crd::u32    reg      = 0;
    };

    // Build the work graph from the NODE DXIL LIBRARY (lib_6_8; entry = the [NodeIsProgramEntry] producer) named
    // `program`, create a global root signature of `bindings.size()` UAV root descriptors, bind the buffers, DispatchGraph
    // the entry with ONE empty entry record, submit + wait, and read back. Returns false on any device failure.
    [[nodiscard]] bool dispatch_graph(crd::containers::ConstSpan<crd::u8> node_dxil, const char* program_name,
                                      crd::containers::ConstSpan<Binding> bindings);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace crd::gpu
