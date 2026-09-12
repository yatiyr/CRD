#pragma once

// crd-ceir — CEIR-26b canonicalize/fold: the FIRST pass driven by the reserved greedy rewrite driver (rewrite_driver.hpp).
// Applies a BOUND SET of algebraic simplifications to fixpoint, each RAUW-and-leave (the dead input is reclaimed by DCE, so
// canonicalize -> dce compose at the PassManager and each stays observable as its own job — rewrite_driver.hpp contract).
//
// ⛔ BOUND ARRAY, not a registry (unlike the VjpRegistry): autodiff needs a registry because it must hand a caller a TYPED
//    reject (MissingVjp) when an op-kind has no rule — the lookup IS the reject. Canonicalize has no such contract: an op with
//    no pattern is left alone, silently and correctly, so there is nothing to look up and no "is the set complete" question.
//    MLIR's op-provided-canonicalization indirection collapses to one array here because this layer has ONE Context and no
//    plugin boundary. (CEIR-27c/d SUPERSEDES the "no plugin boundary" clause: the §72 authored-rule path now EXISTS in
//    passes/rewrite_rules.hpp and reproduces this EXACT pattern set by DIFFERENTIAL [test_rewrite_driver.cpp "ceir 27c"/"27d"];
//    wiring it into canonicalize_run so kCanonPatterns can retire is the 27e design slice.) Patterns are grouped by dialect in
//    the array; a dialect is added here when it earns a fold.
// ⛔ passes/ DEPENDS on the tensor dialect (gen/tensor_ops.hpp) on purpose: the reshape fold IS tensor-specific, and a
//    per-dialect canonicalize header would fragment the pass across N files for zero benefit until a second dialect earns a fold.
// ⛔ match hooks are capture-free (`op.kind() == OpId{fnv1a_ct("dialect.op")}`) — a fn-ptr cannot capture an interned id, and
//    `match` takes a `const Context&` so it cannot intern (intern mutates). `fnv1a_ct("dialect.op") == intern_op(dialect,op)` is
//    a CONTRACT (id.hpp), not a coincidence — a gate MUST pin it (a hash change would silently stop the pattern matching:
//    greedy_rewrite returns false, DCE leaves the chain, the plan still succeeds — a silent no-op, the EMPTY≠UNKNOWN shape).
// ⛔ TWO REWRITE SHAPES compose here: (1) reshape-of-reshape BUILDS a new op (insert_before then RAUW — GROWS op count by 1 per
//    fire before DCE, per rewrite_driver.hpp's cap note); (2) identity_reshape RAUWs the result to its EXISTING operand (no
//    create, no insert — SHRINKS live ops directly). The fixpoint MEASURE is LIVE RESHAPES ON ANY x->consumer PATH (≤ N0 so the
//    rewrite_driver cap holds); total op count is NOT the measure (the fold grows it; DCE reclaims). The pair fully collapses a
//    net-identity chain T->U->T to x (fold -> reshape(x:T):T -> identity-elim -> x).

#include <crd/ceir/context.hpp>
#include <crd/ceir/diagnostic.hpp>
#include <crd/ceir/id.hpp> // fnv1a_ct
#include <crd/ceir/ir.hpp>
#include <crd/ceir/pass_manager.hpp>
#include <crd/ceir/passes/rewrite_driver.hpp>
#include <crd/ceir/rewrite.hpp>

#include <crd/ceir/gen/tensor_ops.hpp> // tensor::build_reshape — the tensor.reshape canonicalizations

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
namespace canon_detail
{
// tensor.reshape(tensor.reshape(x)) -> tensor.reshape(x): collapse a chained rank-bridge to ONE hop (both reshapes are
// element-count-preserving aliases, so the single fold is a valid alias too — tensor_bytes(T_out)==tensor_bytes(T_in) by
// transitivity). RAUW-and-leave: the two dead reshapes are reclaimed by DCE.
// ⛔ 26b-3: NO LONGER excludes T_out == T_in — a rank-preserving chain (T -> U -> T) folds to reshape(x:T):T (an identity) which
//    identity_reshape (below) then RAUWs to x, so the two patterns COMPOSE to fully collapse a net-identity chain to x.
[[nodiscard]] inline bool match_reshape_of_reshape(const Context& ctx, const Operation& op) noexcept
{
    (void)ctx;
    if (op.kind() != OpId{fnv1a_ct("tensor.reshape")}) { return false; }
    if (op.num_results() == 0U || !op.result(0)->has_uses() || op.num_operands() == 0U) { return false; }
    const Value* const     in    = op.operand(0);
    const Operation* const inner = (in != nullptr) ? in->defining_op() : nullptr;
    if (inner == nullptr || inner->kind() != OpId{fnv1a_ct("tensor.reshape")} || inner->num_operands() == 0U) { return false; }
    return inner->operand(0) != nullptr; // the value before both reshapes exists (identity result cleaned up by identity_reshape)
}
inline void rewrite_reshape_of_reshape(Context& ctx, Operation& op)
{
    Operation* const inner  = op.operand(0)->defining_op(); // the input reshape
    Value* const     x      = inner->operand(0);            // the value BEFORE both reshapes
    Operation* const folded = tensor::build_reshape(ctx, x, op.result(0)->type()); // one hop, result type = the outer's
    op.parent_block()->insert_before(folded, &op); // ⛔ LINK before the RAUW — a floating op is invisible to plan + re-collect
    op.result(0)->replace_all_uses_with(folded->result(0)); // leave the two dead reshapes for DCE
}

// tensor.reshape(x:T):T -> x: a NO-OP reshape (result type == operand type) IS its own operand. ⛔ RAUW-to-OPERAND — the SIBLING
// shape of the reshape-of-reshape fold's build+insert+RAUW: it folds to an EXISTING value, so NO create_operation / insert_before
// (correction #2 is vacuous here); leave the dead reshape for DCE. This is the first pattern that SHRINKS live ops directly.
[[nodiscard]] inline bool match_identity_reshape(const Context& ctx, const Operation& op) noexcept
{
    (void)ctx;
    if (op.kind() != OpId{fnv1a_ct("tensor.reshape")}) { return false; }
    if (op.num_results() == 0U || !op.result(0)->has_uses() || op.num_operands() == 0U) { return false; }
    const Value* const in = op.operand(0);
    return in != nullptr && op.result(0)->type() == in->type(); // a same-type (no-op) reshape
}
inline void rewrite_identity_reshape(Context& /*ctx*/, Operation& op)
{
    op.result(0)->replace_all_uses_with(op.operand(0)); // RAUW to the EXISTING operand — no create/insert; DCE reclaims the reshape
}
} // namespace canon_detail

// The canonicalization pattern SET (grouped by dialect). A `match` returning false on an unhandled op is the whole "not
// applicable" contract — no registry, no completeness question (see the header).
inline constexpr RewritePattern kCanonPatterns[] = {
    // ── tensor ──
    RewritePattern{&canon_detail::match_reshape_of_reshape, &canon_detail::rewrite_reshape_of_reshape},
    RewritePattern{&canon_detail::match_identity_reshape, &canon_detail::rewrite_identity_reshape},
};

// The canonicalize `Pass::run`: drive kCanonPatterns to fixpoint. Returns `changed`. RAUW-and-leave ⇒ compose with dce_pass()
// at the PassManager (canonicalize -> dce) to reclaim the folded-away inputs.
[[nodiscard]] inline bool canonicalize_run(Context& ctx, Module& m, DiagnosticEngine& diag)
{
    return greedy_rewrite(ctx, m,
                          containers::ConstSpan<RewritePattern>(kCanonPatterns,
                                                                sizeof(kCanonPatterns) / sizeof(kCanonPatterns[0])),
                          diag, containers::StringView("canonicalize"));
}

// The canonicalize `Pass` descriptor: name + run + preserves NOTHING (a fold rewires def-use ⇒ invalidates every analysis).
[[nodiscard]] inline Pass canonicalize_pass() noexcept
{
    return Pass{containers::StringView("canonicalize"), &canonicalize_run, {}};
}
} // namespace crd::ceir
