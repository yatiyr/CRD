#pragma once

// crd-ceir — CEIR-26b the GREEDY REWRITE DRIVER: the fixpoint orchestration the rewrite.hpp skeleton RESERVED for CEIR-26
// (rewrite.hpp:3-6 — "the greedy/worklist DRIVER (traversal order, fixpoint, iterator invalidation) IS the CEIR-26 work and
// is deliberately RESERVED"). Applies a pattern SET to every op in a module to FIXPOINT. COLLECT-PER-ROUND — the dce.hpp
// skeleton generalized from a single fixed rule to a pattern set: each round re-collects every LIVE op region-recursively,
// tries each pattern via the reserved `try_apply`, and loops until a round changes nothing. ⛔ RE-COLLECT (not a persistent
// worklist): the reserved `RewritePattern::rewrite` is `void(Context&, Operation&)` — it CANNOT return an op it created, so
// ONLY a fresh collect sees a newly-inserted (itself-foldable) op; the block link IS the visibility. ONE skeleton with
// dce_run (collect -> apply with is_erased skip -> fixpoint with a cap) so 26c-26f copy a single shape.
//
// ⛔ PATTERN-AUTHOR CONTRACT (a RewritePattern driven here):
//   - identify an op-KIND capture-free with `op.kind() == OpId{fnv1a_ct("dialect.op")}` (the rewrite.hpp match idiom — a
//     fn-ptr cannot capture an interned id) — `match` takes `const Context&`, so it CANNOT `intern_op`/`attr_int` (those
//     mutate the interner); read attrs via `op.has_attr`/`op.attr`/`ctx.attr_value` and names via `ctx.op_name` (all const).
//   - a `rewrite` that folds to a NEW op MUST link it (`op.parent_block()->insert_before(new_op, &op)`) BEFORE the RAUW — a
//     floating op (created, not inserted) is invisible to the planner (a silently dropped stage, the binding-drop shape) AND
//     to the next round's re-collect, so it is never re-examined.
//   - a `rewrite` RAUWs the replaced result but does NOT erase the now-dead input — DCE (dce.hpp) reclaims it, so the two
//     passes compose at the PassManager (canonicalize -> dce) and each stays observable as its own job. (A pattern MAY erase,
//     but then it owns the dead-op cleanup and the compose-with-DCE split is untested.)
//   - `match` MUST NOT re-match an already-folded op (require the result HAS uses / the canonical form absent), and every
//     pattern MUST be MONOTONE (each apply strictly reduces a measure — op count, chain depth) so the fixpoint terminates. A
//     non-monotone PAIR (A: x->y, B: y->x) is caught by the cap -> a FATAL diagnostic (never a silent partial result).
//
// ⛔ COLLECT-then-apply per round (never rewrite mid-collect — the reserved iterator-invalidation trap, rewrite.hpp:24-25).
//    `is_erased` on ENTRY skips an op a pattern erased earlier THIS round — a pattern can erase an op LATER in the array that
//    has not been visited yet (a producer erasing its consumer), so the check is load-bearing: do NOT remove it.
// ⛔ FIXPOINT CAP = INITIAL-seeded-op-count + 1, fixed from the FIRST collect — NOT recomputed per round: an insert+RAUW fold
//    GROWS the op count (it leaves the dead input for DCE), so a per-round cap would let an inserting oscillator outrun its
//    own bound. Op-count is therefore NOT the termination measure. The bound holds because a monotone pattern's measure (the
//    number of matchable sites / a reshape-chain depth) is itself <= the initial live-op count and strictly drops each round;
//    exceeding the cap => a non-monotone pattern set => a FATAL diag naming the pass (the caller/PassManager stops on has_fatal()).

#include <crd/ceir/context.hpp>
#include <crd/ceir/diagnostic.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/rewrite.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
// Append every op in `region` (recursing into nested regions FIRST) to `ops`. Mirrors dce_collect; kept separate so the
// driver never depends on dce.hpp. Order is immaterial to a fixpoint — every op is retried each round until nothing changes.
inline void greedy_collect(Region* region, containers::Array<Operation*>& ops)
{
    for (Block* b = region->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            for (u32 i = 0; i < op->num_regions(); ++i) { greedy_collect(op->region(i), ops); }
            ops.push_back(op);
        }
    }
}

// Apply `patterns` to every op in `m` to FIXPOINT (collect-per-round). Returns `changed`. On a non-terminating (non-monotone)
// pattern set it emits ONE Fatal diagnostic (naming `pass_name`) and stops — never hangs, never a silent partial result.
[[nodiscard]] inline bool greedy_rewrite(Context& ctx, Module& m, containers::ConstSpan<RewritePattern> patterns,
                                         DiagnosticEngine&      diag,
                                         containers::StringView pass_name = containers::StringView("canonicalize"))
{
    bool                          any = false;
    containers::Array<Operation*> ops(ctx.allocator());
    greedy_collect(m.body(), ops);
    const usize cap   = ops.size() + 1U; // monotone => rounds <= initial live ops; exceeding it => non-monotone => FATAL
    usize       round = 0;
    for (;;)
    {
        ops.clear();
        greedy_collect(m.body(), ops); // re-collect: sees ops a fold inserted this fixpoint + drops ops a pattern erased
        bool round_changed = false;
        for (usize i = 0; i < ops.size(); ++i)
        {
            Operation* const op = ops[i];
            if (op->is_erased()) { continue; } // ⛔ a pattern erased it earlier THIS round (a later-in-array target) — skip
            for (usize p = 0; p < patterns.size(); ++p)
            {
                if (try_apply(patterns[p], ctx, *op))
                {
                    round_changed = true;
                    any           = true;
                    break; // this op mutated (maybe erased) — stop trying patterns on it; the next round re-examines it
                }
            }
        }
        if (!round_changed) { break; }
        if (++round >= cap)
        {
            const containers::StringView notes[1] = {pass_name};
            diag.emit(Severity::Fatal, make_diagnostic_code("ceir.rewrite.nonmonotone"),
                      containers::StringView("ceir.rewrite.nonmonotone"), SourceLoc{},
                      containers::StringView(
                          "greedy_rewrite did not reach a fixpoint within the op-count bound — the pattern set is "
                          "non-monotone (it oscillates); a canonicalization must strictly reduce a measure"),
                      containers::ConstSpan<containers::StringView>(notes, 1U));
            break;
        }
    }
    return any;
}
} // namespace crd::ceir
