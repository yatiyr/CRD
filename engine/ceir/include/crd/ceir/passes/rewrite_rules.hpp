#pragma once

// crd-ceir — CEIR-27c/27d sec-72 the DECLARATIVE REWRITE-RULE model + driver: an AUTHORED `ceir.rewrite` module (gen/rewrite_ops.hpp,
// the rewrite.rule op) loads into RewriteRule data + greedy_rewrite_rules applies them to a payload to FIXPOINT -- so a plugin
// dialect ships a canonicalization WITHOUT editing canonicalize.hpp's kCanonPatterns (the sec-72 plugin boundary that array lacks).
// The TWO rules here express canonicalize.hpp's COMPLETE tensor.reshape pattern set: 27c identity_reshape (a no-op reshape -> its
// input, a RAUW-to-operand) + 27d reshape_of_reshape (reshape(reshape(x)) -> reshape(x), a BUILD-new-op). The driver mirrors
// greedy_rewrite (rewrite_driver.hpp): COLLECT-per-round, monotone fixpoint with a cap -> FATAL on a non-monotone set.
//
// ⛔ the RAUW re-match guard is UNIVERSAL (the DRIVER's, not a rule attr): BOTH actions RAUW result(result_idx) and LEAVE the dead op
//    for DCE (the canonicalize->dce compose), so a result with NO uses is already-rewritten; re-firing would flip round_changed
//    forever -> the cap -> FATAL. So match REQUIRES result(result_idx) to exist and HAVE uses, for EVERY action (canonicalize.hpp:51
//    /73 -- BOTH C++ matches carry it; it is universal, not action-specific). In the driver, no rule author can forget it.
// ⛔ an index (result/operand/inner_operand) exceeding the MATCHED op's arity is a RUNTIME no-match (arity is per-op, not static) --
//    NOT a static find_rewrite_misuse reject (that owns the closed vocabularies only). match returns false; the op is left alone.

#include <crd/ceir/attr.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/diagnostic.hpp>
#include <crd/ceir/gen/rewrite_ops.hpp> // rewrite.rule op + rule_kind -- like canonicalize.hpp deliberately depends on tensor_ops.hpp
#include <crd/ceir/ir.hpp>
#include <crd/ceir/passes/rewrite_driver.hpp> // greedy_collect (shared with greedy_rewrite)

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir::rewrite
{
// The CLOSED match-predicate vocabulary (grows by one entry when a rule needs it). NOLINTNEXTLINE(performance-enum-size)
enum class RewriteConstraint : u8
{
    None = 0,             // absent/unrecognized (find_rewrite_misuse rejects an unknown string before the driver runs)
    ResultTypeEqOperand,  // result(result_idx).type == operand(operand_idx).type (the no-op reshape predicate)
    OperandDefinedByRoot, // operand(operand_idx) is produced by an op of kind==root with an inner_operand'th operand (reshape-of-reshape)
};
// The CLOSED rewrite-action vocabulary. NOLINTNEXTLINE(performance-enum-size)
enum class RewriteAction : u8
{
    None = 0,
    ReplaceResultWithOperand, // RAUW result(result_idx) -> operand(operand_idx); leave the dead op for DCE (27c identity_reshape)
    BuildOpFromInnerOperand,  // build a root-kind op over operand(operand_idx).def.operand(inner_operand), result type=result(result_idx).type; RAUW+leave (27d)
};
// One loaded rule (the data behind a rewrite.rule op). `root_kind` is the interned op-kind of the `root` op-name.
struct RewriteRule
{
    OpId              root_kind;
    RewriteConstraint constraint        = RewriteConstraint::None;
    RewriteAction     action            = RewriteAction::None;
    u32               result_idx        = 0;
    u32               operand_idx       = 0;
    u32               inner_operand_idx = 0; // the 2-hop inner index for BuildOpFromInnerOperand (absent in the asset -> 0)
};

// The MODULE-WIDE closed-vocabulary verifier (the find_resource/quant/transform_misuse house pattern). NOLINTNEXTLINE(performance-enum-size)
enum class RewriteMisuseKind : u8
{
    None = 0,
    UnknownConstraint,     // a rewrite.rule `constraint` string not in the closed vocabulary
    UnknownAction,         // a rewrite.rule `action` string not in the closed vocabulary
    ActionNeedsConstraint, // a path-WALKING action paired with a constraint that does NOT validate the path (crash-prevention)
};
struct RewriteMisuse
{
    const Operation*  op   = nullptr;
    RewriteMisuseKind kind = RewriteMisuseKind::None;
};
[[nodiscard]] inline containers::StringView rewrite_misuse_kind_name(RewriteMisuseKind k) noexcept
{
    switch (k)
    {
    case RewriteMisuseKind::None: return containers::StringView("none");
    case RewriteMisuseKind::UnknownConstraint: return containers::StringView("unknown-constraint");
    case RewriteMisuseKind::UnknownAction: return containers::StringView("unknown-action");
    case RewriteMisuseKind::ActionNeedsConstraint: return containers::StringView("action-needs-constraint");
    }
    return containers::StringView("?");
}

// Map a constraint/action string to its enum, or None if unrecognized -- the closed vocabulary lives in ONE place.
[[nodiscard]] inline RewriteConstraint constraint_from(containers::StringView s) noexcept
{
    if (s == containers::StringView("result_type_eq_operand")) { return RewriteConstraint::ResultTypeEqOperand; }
    if (s == containers::StringView("operand_defined_by_root")) { return RewriteConstraint::OperandDefinedByRoot; }
    return RewriteConstraint::None;
}
[[nodiscard]] inline RewriteAction action_from(containers::StringView s) noexcept
{
    if (s == containers::StringView("replace_result_with_operand")) { return RewriteAction::ReplaceResultWithOperand; }
    if (s == containers::StringView("build_op_from_inner_operand")) { return RewriteAction::BuildOpFromInnerOperand; }
    return RewriteAction::None;
}

namespace detail
{
// read a String-kind attr `name` off `op`, or empty if absent/wrong-kind (the attr-reader-checks-valid rule).
[[nodiscard]] inline containers::StringView rule_str(const Context& ctx, const Operation& op, containers::StringView name) noexcept
{
    const AttrId a = op.attr(name);
    if (!a.valid()) { return {}; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::String ? v.s : containers::StringView();
}
[[nodiscard]] inline RewriteMisuse scan_rewrite_region(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == containers::StringView("rewrite.rule"))
            {
                const RewriteConstraint con = constraint_from(rule_str(ctx, *op, containers::StringView("constraint")));
                const RewriteAction     act = action_from(rule_str(ctx, *op, containers::StringView("action")));
                if (con == RewriteConstraint::None) { return {op, RewriteMisuseKind::UnknownConstraint}; }
                if (act == RewriteAction::None) { return {op, RewriteMisuseKind::UnknownAction}; }
                // ⛔ a path-WALKING action may only pair with the constraint that VALIDATES the path: build_op_from_inner_operand
                //    derefs operand(operand_idx).def.operand(inner) -> REQUIRES operand_defined_by_root (which proves that inner op
                //    is a root op with that operand). Else a vocab-valid asset null-derefs in rule_apply (the 27a duplicate shape).
                if (act == RewriteAction::BuildOpFromInnerOperand && con != RewriteConstraint::OperandDefinedByRoot)
                {
                    return {op, RewriteMisuseKind::ActionNeedsConstraint};
                }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const RewriteMisuse e = scan_rewrite_region(ctx, op->region(i));
                if (e.kind != RewriteMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace detail

// The FIRST rewrite-rule misuse in module `m` (pre-order), or {None}. ⛔ const -- reads names + attrs, interns nothing.
[[nodiscard]] inline RewriteMisuse find_rewrite_misuse(const Context& ctx, const Module& m)
{
    return detail::scan_rewrite_region(ctx, m.body());
}

// Load every rewrite.rule in `rule_mod` into `out` (assumes find_rewrite_misuse clean). Interns each rule's `root` op-NAME to its
// kind (mutates ctx -- hence Context&, not const). Returns the count appended.
[[nodiscard]] inline u32 load_rewrite_rules(Context& ctx, const Module& rule_mod, containers::Array<RewriteRule>& out)
{
    const OpId rule_k = rule_kind(ctx);
    u32        n      = 0U;
    for (const Block* b = rule_mod.body()->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->kind() != rule_k) { continue; }
            const containers::StringView root_nm = detail::rule_str(ctx, *op, containers::StringView("root"));
            usize                        dot     = 0U; // split "dialect.op" at the '.' (op_name is dialect-qualified -- I6)
            while (dot < root_nm.size() && root_nm[dot] != '.') { ++dot; }
            const containers::StringView dia(root_nm.data(), dot);
            const containers::StringView opn(root_nm.data() + (dot < root_nm.size() ? dot + 1U : dot),
                                             dot < root_nm.size() ? root_nm.size() - dot - 1U : 0U);
            RewriteRule  rr;
            rr.root_kind   = ctx.intern_op(dia, opn);
            rr.constraint  = constraint_from(detail::rule_str(ctx, *op, containers::StringView("constraint")));
            rr.action      = action_from(detail::rule_str(ctx, *op, containers::StringView("action")));
            const AttrId ra = op->attr(containers::StringView("result"));
            const AttrId oa = op->attr(containers::StringView("operand"));
            const AttrId ia = op->attr(containers::StringView("inner_operand")); // OPTIONAL (required=false) -- absent reads 0
            rr.result_idx  = (ra.valid() && ctx.attr_value(ra).kind == AttrKind::Int) ? static_cast<u32>(ctx.attr_value(ra).i) : 0U;
            rr.operand_idx = (oa.valid() && ctx.attr_value(oa).kind == AttrKind::Int) ? static_cast<u32>(ctx.attr_value(oa).i) : 0U;
            rr.inner_operand_idx =
                (ia.valid() && ctx.attr_value(ia).kind == AttrKind::Int) ? static_cast<u32>(ctx.attr_value(ia).i) : 0U;
            out.push_back(rr);
            ++n;
        }
    }
    return n;
}
} // namespace crd::ceir::rewrite

namespace crd::ceir
{
namespace rr_detail
{
// does `rule` MATCH `op`? kind + the action's re-match guard + the constraint (all runtime, arity-guarded).
[[nodiscard]] inline bool rule_matches(const Context& /*ctx*/, const rewrite::RewriteRule& rule, const Operation& op) noexcept
{
    if (op.kind() != rule.root_kind) { return false; }
    // ⛔ UNIVERSAL RAUW-and-leave guard (driver's contract, not a rule attr): BOTH actions RAUW result(result_idx) and LEAVE the dead
    //    op for DCE, so an op whose result is already RAUW'd (no uses) must NOT re-match -- else round_changed never settles -> the cap
    //    -> FATAL. canonicalize.hpp:51/73 carry it in BOTH matches. This ALSO bounds result_idx for every constraint/action below.
    if (rule.result_idx >= op.num_results() || !op.result(rule.result_idx)->has_uses()) { return false; }
    if (rule.constraint == rewrite::RewriteConstraint::ResultTypeEqOperand)
    {
        if (rule.operand_idx >= op.num_operands()) { return false; } // this constraint owns the operand_idx bound
        return op.result(rule.result_idx)->type() == op.operand(rule.operand_idx)->type();
    }
    if (rule.constraint == rewrite::RewriteConstraint::OperandDefinedByRoot)
    {
        // operand(operand_idx) is produced by an op of kind==root with an inner_operand'th operand (reshape-of-reshape; canon.hpp:52-55).
        if (rule.operand_idx >= op.num_operands()) { return false; }
        const Value* const     in    = op.operand(rule.operand_idx);
        const Operation* const inner = (in != nullptr) ? in->defining_op() : nullptr;
        if (inner == nullptr || inner->kind() != rule.root_kind || rule.inner_operand_idx >= inner->num_operands()) { return false; }
        return inner->operand(rule.inner_operand_idx) != nullptr;
    }
    return false; // an unknown constraint never matches (find_rewrite_misuse rejects it statically anyway)
}
inline void rule_apply(Context& ctx, const rewrite::RewriteRule& rule, Operation& op)
{
    if (rule.action == rewrite::RewriteAction::ReplaceResultWithOperand)
    {
        op.result(rule.result_idx)->replace_all_uses_with(op.operand(rule.operand_idx)); // leave the dead op for DCE
    }
    else if (rule.action == rewrite::RewriteAction::BuildOpFromInnerOperand)
    {
        // build ONE root-kind op over the inner operand (reshape(reshape(x)) -> reshape(x)); insert BEFORE the RAUW (canon.hpp:62 --
        // a floating op is invisible to plan + re-collect), then RAUW result -> the new op's result; the two dead ops go to DCE.
        Value* const     x     = op.operand(rule.operand_idx)->defining_op()->operand(rule.inner_operand_idx);
        Value*           ins[1] = {x};
        Operation* const built = ctx.create_operation(rule.root_kind, containers::ConstSpan<Value*>(ins, 1U), 1U,
                                                       op.result(rule.result_idx)->type(), 0U);
        op.parent_block()->insert_before(built, &op);
        op.result(rule.result_idx)->replace_all_uses_with(built->result(0));
    }
}
} // namespace rr_detail

// Apply authored `rules` to every op in `m` to FIXPOINT (collect-per-round, the greedy_rewrite mold). Returns `changed`. On a
// non-monotone rule set emits ONE Fatal diagnostic (naming `pass_name`) and stops -- never hangs. The per-op work is rule
// INTERPRETATION (rr_detail) instead of fn-ptr patterns -- the sec-72 authored-rule path (canonicalize.hpp is the C++ path).
[[nodiscard]] inline bool greedy_rewrite_rules(Context& ctx, Module& m, containers::ConstSpan<rewrite::RewriteRule> rules,
                                               DiagnosticEngine&      diag,
                                               containers::StringView pass_name = containers::StringView("rewrite-rules"))
{
    bool                          any = false;
    containers::Array<Operation*> ops(ctx.allocator());
    greedy_collect(m.body(), ops);
    const usize cap   = ops.size() + 1U;
    usize       round = 0;
    for (;;)
    {
        ops.clear();
        greedy_collect(m.body(), ops);
        bool round_changed = false;
        for (usize i = 0; i < ops.size(); ++i)
        {
            Operation* const op = ops[i];
            if (op->is_erased()) { continue; }
            for (usize r = 0; r < rules.size(); ++r)
            {
                if (rr_detail::rule_matches(ctx, rules[r], *op))
                {
                    rr_detail::rule_apply(ctx, rules[r], *op);
                    round_changed = true;
                    any           = true;
                    break;
                }
            }
        }
        if (!round_changed) { break; }
        if (++round >= cap)
        {
            const containers::StringView notes[1] = {pass_name};
            diag.emit(Severity::Fatal, make_diagnostic_code("ceir.rewrite.nonmonotone"),
                      containers::StringView("ceir.rewrite.nonmonotone"), SourceLoc{},
                      containers::StringView("greedy_rewrite_rules did not reach a fixpoint within the op-count bound -- the rule "
                                             "set is non-monotone (it oscillates)"),
                      containers::ConstSpan<containers::StringView>(notes, 1U));
            break;
        }
    }
    return any;
}
} // namespace crd::ceir
