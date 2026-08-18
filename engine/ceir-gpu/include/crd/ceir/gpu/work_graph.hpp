// work_graph.hpp — CEIR-20c-1c: derive a D3D12-Work-Graph TOPOLOGY from an authored ceir.work WorkBuildDesc (the SAME desc
// that build_work_ceir / execute_work_lowered consume for the 20b compute-indirect fallback). This is what makes Work
// Graphs a LOWERING of the authored program — one semantic ceir.work asset, N lowerings — rather than a hand-wired graph:
// the executor reads the desc's produce/consume stages + their shared queues and emits the node graph (produce = the entry
// that emits a grid-launch record; the consume that reads the same queue is its downstream). ceir-gpu names no backend —
// the caller resolves each node's `kernel` symbol to its .ckir + emits/compiles/dispatches (the WorkGraphHooks surface).
#pragma once

#include <crd/ceir/gpu/work_build.hpp> // WorkBuildDesc / WorkStageKind

#include <crd/core/types.hpp>

namespace crd::ceir::gpu
{
inline constexpr crd::u32 kNoWorkGraphNode = 0xFFFFFFFFU;

// One node of the derived graph: its role, the kernel symbol to cook into a node shader, the queue it produces/consumes
// (by source_param), and — for a producer — the downstream node it launches (via a grid-launch record).
struct WorkGraphNode
{
    WorkStageKind               role               = WorkStageKind::Produce;
    crd::containers::StringView kernel;                                      // the .ckir kernel symbol (the caller resolves it)
    crd::u64                    queue_source_param = 0U;                     // the shared queue's identity (produce writes / consume reads)
    crd::u32                    downstream         = kNoWorkGraphNode;       // producer: the node it feeds; else kNoWorkGraphNode
};

struct WorkGraphPlan
{
    WorkGraphNode nodes[8];
    crd::u32      num_nodes = 0U;
    crd::u32      entry     = kNoWorkGraphNode; // the program-entry producer node
};

// Derive the plan: one node per work stage; wire each PRODUCE to the CONSUME sharing its queue (produce→consume launch
// edge); the entry is the producer no other producer feeds. Returns false on a malformed / unsupported shape (0 stages,
// out-of-range queue, or != 1 entry — multi-entry / compact-fed graphs are ledgered for a later slice). Pure + device-free.
[[nodiscard]] inline bool build_work_graph_plan(const WorkBuildDesc& desc, WorkGraphPlan& out)
{
    out = WorkGraphPlan{};
    if (desc.num_stages == 0U || desc.num_stages > 8U) { return false; }
    out.num_nodes = desc.num_stages;
    for (crd::u32 i = 0; i < desc.num_stages; ++i)
    {
        const WorkStageDesc& st = desc.stages[i];
        if (st.queue >= desc.num_queues) { return false; }
        out.nodes[i].role               = st.kind;
        out.nodes[i].kernel             = st.kernel;
        out.nodes[i].queue_source_param = desc.queues[st.queue].source_param;
        out.nodes[i].downstream         = kNoWorkGraphNode;
    }
    // wire: each PRODUCE launches the CONSUME that reads its queue.
    for (crd::u32 p = 0; p < out.num_nodes; ++p)
    {
        if (out.nodes[p].role != WorkStageKind::Produce) { continue; }
        for (crd::u32 c = 0; c < out.num_nodes; ++c)
        {
            if (c != p && out.nodes[c].role == WorkStageKind::Consume
                && out.nodes[c].queue_source_param == out.nodes[p].queue_source_param)
            {
                out.nodes[p].downstream = c;
                break;
            }
        }
    }
    // entry: a producer no other producer feeds (the graph source). Exactly one for the supported single-producer shape.
    bool fed[8] = {};
    for (crd::u32 i = 0; i < out.num_nodes; ++i)
    {
        if (out.nodes[i].downstream != kNoWorkGraphNode) { fed[out.nodes[i].downstream] = true; }
    }
    crd::u32 entry   = kNoWorkGraphNode;
    crd::u32 n_entry = 0U;
    for (crd::u32 i = 0; i < out.num_nodes; ++i)
    {
        if (out.nodes[i].role == WorkStageKind::Produce && !fed[i])
        {
            entry = i;
            ++n_entry;
        }
    }
    if (n_entry != 1U) { return false; } // ledgered: multi-entry / compact-fed graphs are a later slice
    out.entry = entry;
    return true;
}
} // namespace crd::ceir::gpu
