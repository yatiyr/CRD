#pragma once

// crd-ceir — CEIR-26a Dead-Code Elimination: the FIRST optimization pass on the CEIR-8g PassManager framework
// (pass_manager.hpp). Removes an op iff it is a REGISTERED, Pure (side-effect-free) LEAF (no regions) whose results have
// NO uses — iterating to fixpoint so a chain collapses (a consumer's death frees its producer). Semantics-preserving;
// differential-tested (the 25c-1b-2a plan-stage shift 11→9 + the 25c-2 device output BIT-EXACT vs the RAW program on Vk+DX12+llvmpipe
// — for a semantics-preserving pass the reference is the unoptimized program's own device output, not a tolerance vs an oracle).
//
// ⛔ Deadness is `registered ∧ Pure ∧ no-result-uses`, NOT the naive "no result uses":
//   - `op_info(kind) == nullptr` (an UNREGISTERED kind) is MAXIMALLY EFFECTFUL (EMPTY≠UNKNOWN, context.hpp:648-651) — never
//     dead. - `OpTrait::Pure` is the side-effect-free trait (dialect.hpp: "safe to CSE/DCE"); a resultless `compute.dispatch`
//     (the write-through-declare pattern, 25c-1b-2a) is NOT Pure (its effect is the buffer write) ⇒ DCE KEEPS it. This is the
//     effect-narrowing scar in DCE form — a whole-class effect (the buffer write) must gate removal, not just the SSA edge.
// ⛔ LIVENESS CONTRACT (CEIR-26a, discovered at 26a-2): DCE reads liveness from SSA uses + effects ONLY. A value the caller
//    intends to READ BACK (e.g. an autodiff gradient the executor reads by-Value via the plan's `Output` marking) is NOT
//    SSA-live — plan-`Output`-by-traversal is not SSA-liveness. Such a value MUST be consumed in the IR (returned via
//    `func.return`, or stored) BEFORE any pass runs, or DCE (correctly) deletes it as dead. There is NO root side-channel —
//    the IR itself declares what is live (the Pass framework reads the program, not a caller argument).
// ⛔ Region-bearing ops are NOT DCE targets this slice (a region body may hold effects; structured-op DCE is name-forward).
// ⛔ COLLECT-then-erase (never erase mid-walk — the rewrite.hpp reserved-driver iterator-invalidation trap); `erase()` only
//    ever sees no-use ops here, so its live-user assert can never fire.

#include <crd/ceir/context.hpp>
#include <crd/ceir/diagnostic.hpp>
#include <crd/ceir/dialect.hpp> // OpTrait
#include <crd/ceir/ir.hpp>
#include <crd/ceir/pass_manager.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
// An op is DEAD iff it carries no regions (a leaf), its kind is REGISTERED (unregistered ⇒ maximally effectful ⇒ kept),
// it is Pure (side-effect-free — the write-through-declare `compute.dispatch` is NOT Pure ⇒ kept), and NO result has uses.
[[nodiscard]] inline bool dce_is_dead(const Context& ctx, const Operation& op) noexcept
{
    if (op.num_regions() != 0U) { return false; } // container / structured op — not a leaf DCE target (26b: structured DCE)
    // ⛔ UNREGISTERED ⇒ never dead (EMPTY≠UNKNOWN, context.hpp:648-651). `op_has_trait` ALREADY returns false for an unknown
    //    kind (has_trait: `op_info==nullptr ⇒ false`, dialect.cpp:162-165), so the Pure check below would keep it anyway — but
    //    state the maximally-effectful contract EXPLICITLY so a future trait-lookup change can never make an unknown op dead.
    if (ctx.op_info(op.kind()) == nullptr) { return false; }
    if (!ctx.op_has_trait(op, OpTrait::Pure)) { return false; } // effectful ⇒ keep (incl. resultless buffer-writing dispatch)
    for (u32 i = 0; i < op.num_results(); ++i)
    {
        if (op.result(i)->has_uses()) { return false; } // a live result ⇒ keep
    }
    return true; // registered, pure, leaf, every result unused ⇒ dead
}

// Append every currently-dead op in `region` (recursing into nested regions) to `dead`. ⛔ recursion order is immaterial this
// slice — no region-bearing op is ever a DCE target (num_regions!=0 ⇒ not dead); 26b's structured-op DCE will need post-order.
inline void dce_collect(const Context& ctx, Region* region, containers::Array<Operation*>& dead)
{
    for (Block* b = region->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            for (u32 i = 0; i < op->num_regions(); ++i) { dce_collect(ctx, op->region(i), dead); }
            if (dce_is_dead(ctx, *op)) { dead.push_back(op); }
        }
    }
}

// The DCE pass run fn (the `Pass::run` contract). Returns `changed`. Fixpoint: collect every currently-dead op, erase them,
// repeat — so a chain (`dx` gemm → BOTH its output `resource.declare` AND its `W1ᵀ` transpose) fully collapses (each dies only
// once `dx` is gone: the 26a-2 gate observes a 3-op drop from exactly this one dead gemm). `diag`
// unused (DCE emits no diagnostics). ⛔ arena never frees a node, so a worklist pointer stays valid across sibling erases.
[[nodiscard]] inline bool dce_run(Context& ctx, Module& m, DiagnosticEngine& /*diag*/)
{
    bool                          any = false;
    containers::Array<Operation*> dead(ctx.allocator());
    for (;;)
    {
        dead.clear();
        dce_collect(ctx, m.body(), dead);
        if (dead.size() == 0U) { break; }
        for (usize i = 0; i < dead.size(); ++i) { dead[i]->erase(); }
        any = true;
    }
    return any;
}

// The DCE `Pass` descriptor: name + run + preserves NOTHING (removing ops invalidates every analysis).
[[nodiscard]] inline Pass dce_pass() noexcept { return Pass{containers::StringView("dce"), &dce_run, {}}; }
} // namespace crd::ceir
