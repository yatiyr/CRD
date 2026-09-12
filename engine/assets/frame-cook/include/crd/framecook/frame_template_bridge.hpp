// RAF-8 (ADR-0106): the FrameGraphDesc -> render-graph FrameGraphTemplate LOAD BRIDGE.
//
// crd-render-graph is THE single live runtime (ADR-0106). A cooked frame asset deserializes into a `FrameGraphDesc`
// (the authoring/cooked TOPOLOGY); this bridge translates that topology into the render-graph's `FrameGraphTemplate`
// (resources + passes with a resolved executor id + a typed payload) so `crd::rendergraph::compile` + `execute_frame`
// run it — one submission, the canonical command model, the executors that RAF-8a proved on both backends.
//
// ⛔ ACYCLIC EDGE. frame-cook -> render-graph (+ render-pass + render-asset-core). render-graph does NOT depend back on
// frame-cook (it never sees a FrameGraphDesc / FramePassKind); this bridge is the ONE translation point, exactly as
// ADR-0106 mandates. The FramePassKind switch in frame_runtime.cpp stays as a migration adapter until RAF-12.
//
// ⛔ TOPOLOGY ONLY. The bridge maps the graph SHAPE (which resources, which passes, what each reads/writes) — the data
// `compile` needs to schedule, alias and barrier. It does NOT resolve the ECS draw lists or bind device resources;
// that is the host's job at record time (the `DrawListTable` + `ResourceTable` seams). A geometry pass therefore emits
// no `geometry` slot — its draws arrive through the resolved `DrawList`; a sampled read it declares (the shadow atlas)
// becomes an `input*` slot PURELY so the graph orders the write before the read.

#ifndef CRD_FRAMECOOK_FRAME_TEMPLATE_BRIDGE_HPP
#define CRD_FRAMECOOK_FRAME_TEMPLATE_BRIDGE_HPP

#include <crd/framecook/frame_asset.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderpass/executor_registry.hpp>
#include <crd/rendergraph/frame_graph.hpp>

namespace crd::framecook
{
// A resolver for `for_each` expansion — given the kind + arg an authored pass declares, return the INSTANCE COUNT (the
// number of cascades / views / faces). Mirrors `IFrameGraphHost::for_each_count`: the count is a runtime fact (how
// many shadow-casting lights this frame), so the bridge asks the host. Return 0 ⇒ the pass is a for_each the host
// cannot resolve — the bridge drops it and records an `UnresolvedForEach` diagnostic (never a silent skip).
using ForEachCountFn = crd::u32 (*)(FrameForEach kind, crd::u32 arg, void* user);

// Translate `desc` into `out`. `for_each_count` (nullable ⇒ treat every for_each as unresolved) expands multi-view
// passes into N ordinary passes. `schemas` is consulted for nothing structural today (the executor ids are built-in),
// but is threaded so a future app-executor pass validates against its registered schema here rather than at compile.
//
// Returns true on a full translation. Returns false if ANY pass kind is unmapped (`UnsupportedPassKind`) or a for_each
// is unresolved (`UnresolvedForEach`) or a pass exceeds the executor's slot capacity — every failure NAMED, never
// silent. `out` is populated up to the first hard failure (callers should treat a false return as unusable).
[[nodiscard]] bool build_frame_graph_template(const FrameGraphDesc& desc, ForEachCountFn for_each_count, void* user,
                                              const crd::renderpass::ExecutorRegistry& schemas,
                                              crd::rendergraph::FrameGraphTemplate& out,
                                              crd::renderasset::DiagnosticList& diags);

} // namespace crd::framecook

#endif // CRD_FRAMECOOK_FRAME_TEMPLATE_BRIDGE_HPP
