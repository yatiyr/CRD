#pragma once

// crd-ceir-gpu — the §69 HETEROGENEOUS-EXECUTION PARTITIONER for ceir.ml (CEIR-24c). Given a module of ml ops + a set of
// PROVIDERS (each advertising which ops it can CLAIM whole), `partition_ml` produces an INSPECTABLE assignment of each ml op to a
// provider OR to the CKIR fallback. This is the §69 "advertise / assign" core — the minimal, DEVICE-FREE form (CEIR-29 owns the
// full native-graph partitioner across provider classes). A provider either CLAIMS an op whole (a native fused kernel — e.g. the
// coopvec per-invocation MLP) or it doesn't; a NOT-claimed op falls back to the CEIR-22/23 CKIR expansion (expand_ml_op).
//
// ⛔ DEVICE AVAILABILITY IS AN INPUT, NOT A QUERY: each MlProvider carries an `available` flag the CALLER sets from device caps
//    (e.g. cooperative_vector()). The partitioner stays PURE + device-free — the "can't-claim → fallback" negative is testable
//    with available=false (everything falls back). This mirrors the plan/execute split (a caps input, not a device call).
// ⛔ the advertise predicate is a SPECIALIZED-KERNEL SELECTION (the fusion/QuantGemm-scheme scar): it must check FULL semantic
//    attrs (op NAME per I6, the exact activation vocab, element kind, rank, layer-shape support), never just structure — a gelu
//    ml.mlp claimed by a relu-only native kernel is a silent wrong-function.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gpu/expand_ml.hpp> // MlExpandResult (apply_partition expands the fallback ops)
#include <crd/ceir/id.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>

namespace crd::ceir::gpu
{
// A provider's ADVERTISE predicate: can this provider CLAIM `op` whole (a native fused kernel)? PURE (const Context&, const op) —
// FULL semantic attrs, no device call, no captures (a plain function pointer). Returns false for ops it cannot claim.
using MlAdvertiseFn = bool (*)(const Context&, const Operation*);

// A §69 execution provider descriptor: a name, an `available` flag (the caller sets it from device caps — an INPUT), and the
// advertise predicate. An unavailable provider claims nothing (skipped in the assign loop, even if it could semantically claim).
struct MlProvider
{
    containers::StringView name;
    bool                   available = false;
    MlAdvertiseFn          advertise = nullptr;
};

// One op's assignment: `provider` = the index into the providers span that CLAIMS it, or -1 for the CKIR FALLBACK (no provider
// claimed it). `op` points at the ml op in the module.
struct MlAssignment
{
    const Operation* op       = nullptr;
    crd::i32         provider = -1; // -1 == CkirFallback
};

// The inspectable partition: one MlAssignment per ml op in the module (pre-order). `claimed(i)` counts the ops a given provider
// claimed; `fallback()` counts the CKIR-expansion ops.
struct MlPartition
{
    containers::Array<MlAssignment> assignments;
    explicit MlPartition(memory::IAllocator* a) : assignments(a) {}
    [[nodiscard]] crd::u32 fallback() const noexcept
    {
        crd::u32 n = 0;
        for (crd::usize i = 0; i < assignments.size(); ++i) { n += assignments[i].provider < 0 ? 1U : 0U; }
        return n;
    }
    [[nodiscard]] crd::u32 claimed_by(crd::i32 provider) const noexcept
    {
        crd::u32 n = 0;
        for (crd::usize i = 0; i < assignments.size(); ++i) { n += assignments[i].provider == provider ? 1U : 0U; }
        return n;
    }
};

// Assign every ml.mlp / ml.attention op in `m` (pre-order) to the FIRST available provider whose advertise predicate accepts it,
// or to the CKIR fallback (-1). ⛔ const Module — a pure inspection (no rewrite). ⛔ const Context — reads types + attrs.
[[nodiscard]] MlPartition partition_ml(const Context& ctx, const Module& m, containers::ConstSpan<MlProvider> providers,
                                       memory::IAllocator* alloc);

// Expand ONLY the CKIR-FALLBACK-assigned ops (provider < 0) into the 22/23 vocab (expand_ml_op each); the CLAIMED ops are left
// in place for the caller to dispatch natively (the §136 crown's coopvec path). Returns the expand count + first error.
[[nodiscard]] MlExpandResult apply_partition(Context& ctx, Module& m, const MlPartition& partition);

// The coopvec native provider's advertise predicate (the CLAIM check for the per-invocation cooperative-vector MLP): op is
// ml.mlp; activation == "relu"; input/weights/output all Float + rank-2; >=2 weights (coopvec needs >=1 HIDDEN layer); a UNIFORM
// hidden width (all intermediate widths equal — coopvec's single `hidden`); every dim (in/hidden/out) in [1, 1024]
// (CoopVecMlpConfig::valid). ⛔ false for ml.attention (no native attention kernel — the real can't-claim case) + for any
// non-relu / non-uniform-hidden / oversized ml.mlp.
[[nodiscard]] bool coopvec_can_claim_mlp(const Context& ctx, const Operation* op);
} // namespace crd::ceir::gpu
