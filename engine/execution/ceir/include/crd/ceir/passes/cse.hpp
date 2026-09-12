#pragma once

// crd-ceir — CEIR-26c Common-Subexpression Elimination: the SECOND value-rewriting optimization pass on the CEIR-8g
// PassManager framework (after 26a DCE + 26b canonicalize). Finds two structurally-identical eligible ops in the SAME block,
// RAUWs the later's results to the earlier's, and ERASES the later directly — SELF-CONTAINED, NO dce compose (unlike
// canonicalize's RAUW-and-leave): a duplicate's operands are pointer-shared with its surviving twin, so erasing the dup
// removes the last use of NOTHING (the twin still uses them) and orphans no op for DCE to reclaim. Semantics-preserving;
// differential-tested (26c-2 removes a duplicate DISPATCHED stage so the plan-stage count drops AND the device output stays
// BIT-EXACT vs the RAW program — the 26a witness at full teeth again, unlike 26b's alias-only reshape fold which changed no
// dispatch; proven bit-exact on real Vk (#4756) + DX12 (#4836)).
//
// ⛔ STANDALONE PASS, not a canonicalize RewritePattern (dce.hpp mold, NOT rewrite_driver.hpp): CSE needs a per-block `seen`
//    set — state a capture-free `match(const Context&, const Operation&)` fn-ptr cannot carry. It is the MLIR model (CSE is a
//    dedicated pass, not an op-local fold). Same collect-then-erase + fixpoint SHAPE as dce_run so the two stay one idiom — but
//    CSE erases the dup ITSELF (no dce compose to finish; see the self-contained note above), whereas canonicalize leaves a
//    dead input for DCE. ⛔ FIXPOINT NOTE: RAUW is immediate, so a straight-line def-before-use block collapses every cascade
//    in ONE productive round (a later dup's operand is rewired before it is scanned) — the `for(;;)` loop is dce_run-shape
//    parity + the convergence guarantee for the future cross-block extension; a SECOND productive round is only reachable if a
//    RAUW lands on an already-scanned op (a use textually before its def), which a straight-line corpus cannot construct today.
// ⛔ ELIGIBILITY = `leaf ∧ registered ∧ Pure ∧ has-results ∧ NOT-positively-nondeterministic` (dce_is_dead's first four, plus
//    §27):
//      - leaf (no regions): region CSE needs structural REGION equality + a dominance tree — name-forward this slice.
//      - registered (op_info!=nullptr): an unregistered kind is MAXIMALLY EFFECTFUL (EMPTY≠UNKNOWN, context.hpp) ⇒ never CSE.
//      - Pure (OpTrait::Pure): an effectful op is NOT interchangeable with a twin (the resultless write-through-declare
//        `compute.dispatch` writes a buffer — CSE'ing two would drop a write). This is the effect-narrowing scar in CSE form.
//      - has-results: nothing to RAUW otherwise (a resultless Pure op is DCE's job, not CSE's).
//      - §27 determinism: CSE replaces `op` with an EARLIER twin, ASSUMING twin-result == op-result. That rests on
//        device-bit-exact-by-construction (same kind + same operand Values + same device ⇒ same kernel ⇒ same bits; 26c-2
//        proves it on Vk+DX12). A POSITIVE nondeterminism claim (Nondeterministic / ExternalNondeterminism) BREAKS that
//        argument (unordered atomics / races-by-design / outside sources produce different bits per run) ⇒ disqualified.
//        `Unspecified` does NOT disqualify: absence-of-claim ≠ a nondeterminism claim (EMPTY≠UNKNOWN, the §27 doctrine —
//        Unspecified is conservative, not Nondeterministic), and an Unspecified Pure op RIDES device-bit-exact-by-construction.
//        No op-kind carries Nondeterministic TODAY, so the check disqualifies nothing live — but it lives in the eligibility
//        fn so it LANDS the day a kind does (the registered-default-empty rule in CSE form). A synthetic Nondeterministic op
//        gates it (test_cse.cpp) so the clause can never silently regress to a no-op.
// ⛔ STRUCTURAL EQUALITY (both ops eligible, in the same block, `b` earlier so it DOMINATES `a`): same kind, same operand
//    ARITY with each operand Value* POINTER-equal (SSA values are unique — two ops reading the same Value* read the same
//    thing; DEEP operand equality would be WRONG, it would fuse reads of two distinct-but-equal-typed inputs), same result
//    arity + each result TYPE equal, and same attrs. Attr compare is ORDER-INDEPENDENT: attr names are unique per op
//    (set_attr overwrites), so equal `num_attrs` + a per-name lookup of every `a` attr in `b` is total (no b->a loop needed —
//    an extra attr in `b` would trip the count; a missing one reads back an invalid AttrId != a's valid one).
// ⛔ NEVER add a RAUW'd duplicate to `seen`: a THIRD identical op must find the SURVIVOR, not the just-eliminated tombstone
//    (the monotone-tombstone-chain scar in CSE form). Three identical ops therefore collapse to the FIRST in ONE round.
// ⛔ SAME-BLOCK dominance scope only: an earlier op in the same graph-region block dominates a later one, so its result is
//    available where the later op is. Cross-block / cross-region CSE needs a real dominance tree — CSE does not build one this
//    slice (the DCE-leaf-scoping precedent). Each nested block gets its OWN `seen` scope.
// ⛔ COLLECT-then-erase + fixpoint (dce.hpp): erasing a dup can make two consumers identical, so re-run to fixpoint. RAUW is
//    immediate, so a straight-line cascade actually collapses in ONE productive round (a later dup's operand is rewired before
//    it is scanned); the loop is the DCE-shaped convergence guarantee for any op ordering and the future cross-block
//    extension. O(n²) per-block scan is fine at phase-1 — hash-keyed value-numbering when a module makes the scan measurable
//    (no consumer does today).

#include <crd/ceir/context.hpp>
#include <crd/ceir/diagnostic.hpp>
#include <crd/ceir/dialect.hpp> // OpTrait
#include <crd/ceir/ir.hpp>
#include <crd/ceir/pass_manager.hpp>
#include <crd/ceir/semantics.hpp> // DeterminismClass

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
// An op is a CSE CANDIDATE iff it is a registered, Pure LEAF with results whose §27 class is not a positive nondeterminism
// claim (see the header). A candidate may be eliminated in favour of an earlier structurally-equal candidate in its block.
[[nodiscard]] inline bool cse_is_candidate(const Context& ctx, const Operation& op) noexcept
{
    if (op.num_regions() != 0U) { return false; }                    // leaf only (region CSE needs structural region equality)
    if (op.num_results() == 0U) { return false; }                    // nothing to RAUW (a resultless Pure op is DCE's job)
    if (ctx.op_info(op.kind()) == nullptr) { return false; }         // unregistered ⇒ maximally effectful (EMPTY≠UNKNOWN)
    if (!ctx.op_has_trait(op, OpTrait::Pure)) { return false; }      // effectful ⇒ not interchangeable (buffer-writing dispatch)
    const DeterminismClass d = ctx.op_determinism(op.kind());        // §27: a POSITIVE nondeterminism claim breaks bit-exactness
    if (d == DeterminismClass::Nondeterministic || d == DeterminismClass::ExternalNondeterminism) { return false; }
    return true; // registered, pure, leaf, has results, not positively-nondeterministic ⇒ CSE candidate
}

// True iff `a` and `b` compute the SAME value: same kind, operand arity with each operand Value* pointer-equal, result arity
// with each result type equal, and identical attrs (order-independent — see the header). Callers pass two candidates in the
// same block with `b` earlier (dominating) than `a`.
[[nodiscard]] inline bool cse_structurally_equal(const Operation& a, const Operation& b) noexcept
{
    if (a.kind() != b.kind()) { return false; }
    if (a.num_operands() != b.num_operands()) { return false; }
    for (u32 i = 0; i < a.num_operands(); ++i)
    {
        if (a.operand(i) != b.operand(i)) { return false; } // Value* POINTER equality — SSA values are unique
    }
    if (a.num_results() != b.num_results()) { return false; }
    for (u32 i = 0; i < a.num_results(); ++i)
    {
        if (a.result(i)->type() != b.result(i)->type()) { return false; } // TypeId equality
    }
    if (a.num_attrs() != b.num_attrs()) { return false; }
    for (u32 i = 0; i < a.num_attrs(); ++i)
    {
        if (b.attr(a.attr_name(i)) != a.attr_id_at(i)) { return false; } // interned AttrId equality; absent-in-b ⇒ invalid ≠ valid
    }
    return true;
}

// CSE `region`: per block, keep a `seen` list of candidates; a later candidate structurally equal to an earlier `seen` op is a
// duplicate — RAUW its results to the survivor's and collect it into `dead`. Recurse into nested regions FIRST (each block
// scopes its own `seen` — cross-block CSE is name-forward). Collect-then-erase: `dead` is drained by the caller after the walk.
inline void cse_region(const Context& ctx, Region* region, containers::Array<Operation*>& dead)
{
    for (Block* b = region->first_block(); b != nullptr; b = b->next_in_region())
    {
        containers::Array<Operation*> seen(ctx.allocator()); // per-block dominance scope (an earlier op dominates a later one)
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            for (u32 i = 0; i < op->num_regions(); ++i) { cse_region(ctx, op->region(i), dead); } // nested blocks first
            if (!cse_is_candidate(ctx, *op)) { continue; }
            Operation* match = nullptr;
            for (usize s = 0; s < seen.size(); ++s)
            {
                if (cse_structurally_equal(*op, *seen[s])) { match = seen[s]; break; } // first (earliest) equal survivor
            }
            if (match != nullptr)
            {
                for (u32 i = 0; i < op->num_results(); ++i) { op->result(i)->replace_all_uses_with(match->result(i)); }
                dead.push_back(op); // ⛔ do NOT add to `seen`: a third dup must find `match`, not this tombstone
            }
            else
            {
                seen.push_back(op);
            }
        }
    }
}

// The CSE pass run fn (the `Pass::run` contract). Returns `changed`. Fixpoint: collect+RAUW every duplicate, erase them,
// repeat — so a cascade (two dups feeding two otherwise-identical consumers) fully collapses. `diag` unused (CSE emits none).
[[nodiscard]] inline bool cse_run(Context& ctx, Module& m, DiagnosticEngine& /*diag*/)
{
    bool                          any = false;
    containers::Array<Operation*> dead(ctx.allocator());
    for (;;)
    {
        dead.clear();
        cse_region(ctx, m.body(), dead);
        if (dead.size() == 0U) { break; }
        for (usize i = 0; i < dead.size(); ++i) { dead[i]->erase(); }
        any = true;
    }
    return any;
}

// The CSE `Pass` descriptor: name + run + preserves NOTHING (RAUW rewires def-use ⇒ invalidates every analysis).
[[nodiscard]] inline Pass cse_pass() noexcept { return Pass{containers::StringView("cse"), &cse_run, {}}; }
} // namespace crd::ceir
