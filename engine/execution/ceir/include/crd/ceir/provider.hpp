#pragma once

// crd-ceir — `IExecutionProvider` (§69; ADR-0109 §4.2): the ABSTRACT execution-provider seam. A provider advertises which
// ops it supplies reference behaviour for and EXECUTES a program; the graph partitioner assigns regions to providers
// (CEIR-21/26). ⛔ Concrete providers live in SEPARATE BRIDGE modules that depend on both crd-ceir AND a backend —
// `crd-ceir-host` (→ crd-jobs, CEIR-6b), `crd-ceir-gpu` (→ gpu-context, later). ⛔ NO backend type (VkDevice / a jobs
// Counter / an IGpuProgram) EVER appears in this header (I4: crd-ceir stays host-only + backend-free — the
// `crd-ceir-invariants` gate forbids a `crd/jobs/`-etc. include here). This header includes NO backend header.
//
// ⛔ VTABLE STABILITY (ADR-0109 §4.2 / the D135 discipline / the rhi-compute LTCG-SEGV scar): this is a PUBLIC abstract
// interface in a FOUNDATIONAL module dispatched across separate bridge targets — exactly where a mid-vtable insert
// silently mis-dispatches. APPEND new pure-virtuals AT THE END ONLY, never in the middle.

#include <crd/ceir/exec.hpp> // exec::ExecResult (+ transitively context.hpp: Context / Module / OpId)
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
class IExecutionProvider
{
public:
    IExecutionProvider()                                     = default;
    IExecutionProvider(const IExecutionProvider&)            = default;
    IExecutionProvider(IExecutionProvider&&)                 = default;
    IExecutionProvider& operator=(const IExecutionProvider&) = default;
    IExecutionProvider& operator=(IExecutionProvider&&)      = default;
    virtual ~IExecutionProvider()                            = default;

    // A short human name (diagnostics / partitioner logs).
    [[nodiscard]] virtual containers::StringView name() const noexcept = 0;

    // §69: does this provider supply reference behaviour for op-kind `k`? (The partitioner assigns regions by this.) 6b
    // ships the minimal ops-capability query; the full advertise surface — types/layouts/memory-domains/costs/determinism
    // + a compile→plan interface (§69/§70/§102/§103) — was a NAMED deferral to the partitioner band. CEIR-29a LANDED the
    // ADVERTISE half on the DEVICE-FREE partitioner descriptor `MlProvider` (gpu/partition_ml.hpp: provider_class +
    // memory_domain + determinism + claims_subgraphs).
    // ⛔ ~~the compile→plan pure-virtual APPEND on THIS seam is CEIR-29b (it lands with its implementor, the launch-graph
    // provider bridge)~~ **[STRUCK — CEIR-29b-2b reconcile: this seam is the SCALAR reference executor (`ExecResult` =
    // `Array<i64>`, the §118 Interpreter, arith/core/func); a TENSOR provider does NOT fit it. The "compile→plan" for a tensor
    // provider IS `plan_tensor_pipeline` (crd-ceir-gpu) — it already produces the reusable, provider-agnostic compiled form; a
    // LAUNCH-GRAPH provider (a native graph-API bridge) is a plan LAUNCH-MODE (`begin_capture`/`end_capture`, 29b-2a), NOT an
    // IExecutionProvider implementor. So `IExecutionProvider` gains NOTHING at 29b — no compile→plan pure-virtual here.]**
    [[nodiscard]] virtual bool advertises(const Context& ctx, OpId k) const = 0;

    // Execute `@entry(args)` of `m` on this provider's backend; returns the entry's `func.return` values or a typed error.
    [[nodiscard]] virtual exec::ExecResult execute(Context& ctx, const Module& m, containers::StringView entry,
                                                   containers::ConstSpan<crd::i64> args) = 0;

    // ⛔ APPEND new pure-virtuals BELOW THIS LINE ONLY (vtable stability — see the header banner).
};
} // namespace crd::ceir
