// CEIR-26b — the GREEDY REWRITE DRIVER gate (device-free). Proves the reserved fixpoint driver (rewrite_driver.hpp) built on
// the rewrite.hpp skeleton: (1) COLLECT-PER-ROUND reaches a FIXPOINT in ONE call — a chain collapses across rounds (a
// producer dies only once its consumer is gone, the re-collect seeing the freed state); (2) the `is_erased`-on-entry skip is
// load-bearing when a pattern erases an op LATER in the same round's array (a producer erasing its consumer); (3) a FOLD
// pattern that inserts a new op + RAUW and LEAVES the dead input composes with DCE (the canonicalize -> dce split); (4) a
// non-monotone (oscillating) pattern PAIR is caught by the cap -> a FATAL diagnostic, never a hang. Host-only. ASCII names.
//
// The synthetic `ct.*` dialect is interned for the erase/skip/oscillate tests (the driver checks no traits) and REGISTERED
// with Pure/effectful traits for the fold+DCE-compose test (DCE needs the traits to reclaim the folded-away ops); match hooks
// are capture-free via `OpId{fnv1a_ct("ct.op")}` (== intern_op, pinned below), the rewrite.hpp idiom.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/parse.hpp>                 // CEIR-27c: parse the authored rule asset
#include <crd/ceir/print.hpp>                 // CEIR-27c: canonical print for the anti-drift + the rule-vs-canonicalize differential
#include <crd/ceir/passes/canonicalize.hpp>   // CEIR-27c: canonicalize_run — the C++ pattern the authored rule reproduces
#include <crd/ceir/passes/dce.hpp>
#include <crd/ceir/passes/rewrite_driver.hpp>
#include <crd/ceir/passes/rewrite_rules.hpp>  // CEIR-27c: RewriteRule + find_rewrite_misuse + load_rewrite_rules + greedy_rewrite_rules
#include <crd/ceir/rewrite.hpp>
#include <crd/ceir/gen/resource_ops.hpp>      // CEIR-27c: resource.declare + resource.export (build the reshape payload + sink)
#include <crd/ceir/gen/rewrite_ops.hpp>       // CEIR-27c: register_rewrite_ops (parse the rule asset)
#include <crd/ceir/tensor.hpp>                // CEIR-27c: tensor::register_dialect + build_reshape
#include <crd/ceir/type.hpp>                  // CEIR-27c: the tensor-type builders

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::u32;

namespace
{
// ── capture-free op-kind ids (compile-time FNV of "dialect.op"; == intern_op, pinned in each TEST_CASE) ──
constexpr OpId kProd{fnv1a_ct("ct.prod")};
constexpr OpId kCons{fnv1a_ct("ct.cons")};
constexpr OpId kSrc{fnv1a_ct("ct.src")};
constexpr OpId kReshape{fnv1a_ct("ct.reshape")};
constexpr OpId kSink{fnv1a_ct("ct.sink")};
constexpr OpId kA{fnv1a_ct("ct.a")};
constexpr OpId kB{fnv1a_ct("ct.b")};
constexpr OpId kPick{fnv1a_ct("ct.pick")};

// (1)/(2) DEAD-op removal: a ct.prod/ct.cons whose every result is unused. Monotone (erase strictly reduces op count).
bool match_dead(const Context&, const Operation& op) noexcept
{
    if (op.kind() != kProd && op.kind() != kCons) { return false; }
    if (op.num_results() == 0U) { return false; }
    for (u32 i = 0; i < op.num_results(); ++i)
    {
        if (op.result(i)->has_uses()) { return false; }
    }
    return true; // has a result, all unused -> dead
}
void rewrite_erase(Context&, Operation& op) { op.erase(); }

// A function-local static apply-counter (not a namespace-scope global — the test_pass_manager.cpp idiom, tidy-clean), so
// Test 2 can PROVE the is_erased skip is load-bearing: with the skip this eraser fires ONCE (on the dead producer); without
// it, it ALSO fires on the tombstoned consumer -> twice. Reset per test.
int& dead_erase_count()
{
    static int c = 0;
    return c;
}
void rewrite_erase_counted(Context&, Operation& op)
{
    ++dead_erase_count();
    op.erase();
}

// (2) A PRODUCER whose result's sole consumer is a ct.cons -> erase that consumer (its result is unused). This erases an op
// LATER in the round's array than the producer, so the driver's is_erased-on-entry skip is what keeps it from re-processing a
// tombstone. Monotone: once the consumer is gone the producer's result has no uses, so this pattern no longer matches it.
bool match_prod_with_cons(const Context&, const Operation& op) noexcept
{
    if (op.kind() != kProd || op.num_results() == 0U || !op.result(0)->has_uses()) { return false; }
    return op.result(0)->first_use()->owner->kind() == kCons;
}
void rewrite_kill_cons(Context&, Operation& op) { op.result(0)->first_use()->owner->erase(); }

// (3) reshape-of-reshape -> reshape(inner-input): a FOLD. Matches a ct.reshape whose result IS used and whose input is itself
// a ct.reshape; rewrites to a fresh ct.reshape over the INNER reshape's input, INSERTED before this op, then RAUW. It does
// NOT erase the now-dead reshapes (DCE reclaims them). `result HAS uses` in the match is the "don't re-fold a dead op" guard.
bool match_reshape_of_reshape(const Context&, const Operation& op) noexcept
{
    if (op.kind() != kReshape || op.num_results() == 0U || !op.result(0)->has_uses()) { return false; }
    const Value* const in = op.operand(0);
    const Operation* const def = (in != nullptr) ? in->defining_op() : nullptr;
    return def != nullptr && def->kind() == kReshape;
}
void rewrite_fold_reshape(Context& ctx, Operation& op)
{
    Operation* const inner       = op.operand(0)->defining_op(); // the input reshape
    Value* const     inner_input = inner->operand(0);            // the value BEFORE both reshapes
    Value* const     ops_in[1]   = {inner_input};
    Operation* const folded      = ctx.create_operation(kReshape, ConstSpan<Value*>(ops_in, 1U), 1U);
    op.parent_block()->insert_before(folded, &op); // ⛔ link BEFORE the RAUW — a floating op is invisible to re-collect + plan
    op.result(0)->replace_all_uses_with(folded->result(0));
}

// (4) an OSCILLATING pair: flip ct.pick's operand ct.a <-> ct.b forever. Non-monotone (no measure decreases) -> the cap fires.
Value* result_of_kind(Block* blk, OpId k) noexcept
{
    for (Operation* o = blk->first_op(); o != nullptr; o = o->next_in_block())
    {
        if (o->kind() == k && o->num_results() > 0U) { return o->result(0); }
    }
    return nullptr;
}
bool match_pick_on(const Operation& op, OpId operand_kind) noexcept
{
    if (op.kind() != kPick || op.num_operands() == 0U) { return false; }
    const Value* const in = op.operand(0);
    const Operation* const def = (in != nullptr) ? in->defining_op() : nullptr;
    return def != nullptr && def->kind() == operand_kind;
}
bool match_pick_on_a(const Context&, const Operation& op) noexcept { return match_pick_on(op, kA); }
bool match_pick_on_b(const Context&, const Operation& op) noexcept { return match_pick_on(op, kB); }
void rewrite_pick_to_b(Context&, Operation& op) { op.set_operand(0, result_of_kind(op.parent_block(), kB)); }
void rewrite_pick_to_a(Context&, Operation& op) { op.set_operand(0, result_of_kind(op.parent_block(), kA)); }

// CEIR-27c: build `%x = resource.declare : tensor<f32,[8,8]> ; %r = tensor.reshape(%x) : tensor<f32,shape_out> ;
// resource.export(%r)` into a fresh module (bare ops in the module block — no func). The export SINKS %r (gives it a use, so
// identity_reshape's has_uses guard passes). same_type=true ⇒ shape_out == [8,8] (a NO-OP reshape — the rule fires); false ⇒
// [64] (element-count-preserving but a DIFFERENT type — the result_type_eq_operand constraint is false, the rule must NOT fire).
Module* mk_reshape_mod(Context& ctx, bool same_type)
{
    const TypeId f32t     = ctx.type_f32();
    const TypeId din[2]   = {ctx.type_dim_static(8U), ctx.type_dim_static(8U)};
    const TypeId t_in     = ctx.type_tensor(f32t, ctx.type_shape(ConstSpan<TypeId>(din, 2U)));
    const TypeId dout[1]  = {ctx.type_dim_static(64U)};
    const TypeId t_out    = same_type ? t_in : ctx.type_tensor(f32t, ctx.type_shape(ConstSpan<TypeId>(dout, 1U)));
    Module* const    m   = ctx.create_module();
    Block* const     blk = ctx.create_block();
    m->body()->append(blk);
    Operation* const x = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, t_in);
    blk->append(x);
    Operation* const r = tensor::build_reshape(ctx, x->result(0), t_out);
    blk->append(r);
    Operation* const e = resource::build_export(ctx, r->result(0));
    blk->append(e);
    return m;
}

// CEIR-27d: t_out selector for the [8,8]->[64]->t_out chain. RankBridge ([4,16], a genuine rank-bridge — only reshape_of_reshape
// applies, no identity follow-up); NetIdentity ([8,8]==t_in — the folded reshape is a NO-OP that identity RAUWs to %x, the two
// rules COMPOSE to fully collapse); OuterIdentity ([64]==t_mid, outer result-type == inner's but != input — the ORDER-DISCRIMINATING
// chain where BOTH rules match the OUTER reshape, so which fires first changes the module).
enum class ChainOut : crd::u8 // NOLINT(performance-enum-size)
{
    RankBridge,
    NetIdentity,
    OuterIdentity
};
// A TWO-reshape chain %x = resource.declare : [8,8] ; %r1 = tensor.reshape(%x) : [64] ; %r2 = tensor.reshape(%r1) : t_out ;
// resource.export(%r2). reshape_of_reshape folds %r2 -> tensor.reshape(%x) : t_out (ONE hop over the grandchild operand %x).
Module* mk_chain_mod(Context& ctx, ChainOut out)
{
    const TypeId f32t    = ctx.type_f32();
    const TypeId din[2]  = {ctx.type_dim_static(8U), ctx.type_dim_static(8U)};
    const TypeId t_in    = ctx.type_tensor(f32t, ctx.type_shape(ConstSpan<TypeId>(din, 2U)));
    const TypeId dmid[1] = {ctx.type_dim_static(64U)};
    const TypeId t_mid   = ctx.type_tensor(f32t, ctx.type_shape(ConstSpan<TypeId>(dmid, 1U)));
    const TypeId dout[2] = {ctx.type_dim_static(4U), ctx.type_dim_static(16U)};
    const TypeId t_rank  = ctx.type_tensor(f32t, ctx.type_shape(ConstSpan<TypeId>(dout, 2U)));
    TypeId       t_out   = t_rank; // ChainOut::RankBridge
    if (out == ChainOut::NetIdentity) { t_out = t_in; }
    else if (out == ChainOut::OuterIdentity) { t_out = t_mid; }
    Module* const m   = ctx.create_module();
    Block* const  blk = ctx.create_block();
    m->body()->append(blk);
    Operation* const x = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, t_in);
    blk->append(x);
    Operation* const r1 = tensor::build_reshape(ctx, x->result(0), t_mid);
    blk->append(r1);
    Operation* const r2 = tensor::build_reshape(ctx, r1->result(0), t_out);
    blk->append(r2);
    Operation* const e = resource::build_export(ctx, r2->result(0));
    blk->append(e);
    return m;
}
} // namespace

TEST_CASE("ceir 26b: greedy_rewrite collapses a dead chain to fixpoint in ONE call (collect-per-round)", "[ceir][rewrite]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    REQUIRE(ctx.intern_op("ct", "prod") == kProd); // pins the capture-free match ids to intern_op
    REQUIRE(ctx.intern_op("ct", "cons") == kCons);

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    // %p = ct.prod ; %c = ct.cons(%p) -- %c unused, %p used only by %c (the DCE dx->W1t dead-chain shape).
    Operation* const p = ctx.create_operation(kProd, {}, 1U);
    block->append(p);
    Value* const     pin[1] = {p->result(0)};
    Operation* const c      = ctx.create_operation(kCons, ConstSpan<Value*>(pin, 1U), 1U);
    block->append(c);
    REQUIRE(block->num_ops() == 2U);
    REQUIRE(p->result(0)->has_uses()); // ⛔ %p is NOT dead at t=0 -> its death (round 2, after %c) is the fixpoint proof

    const RewritePattern patterns[1] = {RewritePattern{&match_dead, &rewrite_erase}};
    DiagnosticEngine     diag(ctx, &root);
    const bool           changed = greedy_rewrite(ctx, *m, ConstSpan<RewritePattern>(patterns, 1U), diag);
    CHECK(changed);
    CHECK(block->num_ops() == 0U);
    CHECK(c->is_erased()); // round 1
    CHECK(p->is_erased()); // round 2 (only after the re-collect saw %p freed) -- identity, not just count
    CHECK_FALSE(diag.has_fatal());

    // idempotence: a second call over the emptied module changes nothing.
    CHECK_FALSE(greedy_rewrite(ctx, *m, ConstSpan<RewritePattern>(patterns, 1U), diag));
}

TEST_CASE("ceir 26b: the is_erased skip handles a pattern erasing a LATER op in the round", "[ceir][rewrite]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    REQUIRE(ctx.intern_op("ct", "prod") == kProd);
    REQUIRE(ctx.intern_op("ct", "cons") == kCons);

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    Operation* const p = ctx.create_operation(kProd, {}, 1U); // collected at index 0
    block->append(p);
    Value* const     pin[1] = {p->result(0)};
    Operation* const c      = ctx.create_operation(kCons, ConstSpan<Value*>(pin, 1U), 1U); // index 1 -- erased BY %p's rewrite
    block->append(c);

    // Pattern order: prod-erases-cons FIRST (erases %c while %c is still ahead in the array -> the skip guards it), then the
    // dead-op remover (COUNTED) cleans up the now-dead %p. Converges without touching a tombstone.
    const RewritePattern patterns[2] = {RewritePattern{&match_prod_with_cons, &rewrite_kill_cons},
                                        RewritePattern{&match_dead, &rewrite_erase_counted}};
    DiagnosticEngine     diag(ctx, &root);
    dead_erase_count() = 0;
    const bool changed = greedy_rewrite(ctx, *m, ConstSpan<RewritePattern>(patterns, 2U), diag);
    CHECK(changed);
    CHECK(c->is_erased()); // erased by %p's rewrite in round 1 (a later-in-array target -> skipped when reached)
    CHECK(p->is_erased()); // dead once %c is gone -> removed in round 2
    CHECK(block->num_ops() == 0U);
    CHECK_FALSE(diag.has_fatal());
    // ⛔ THE load-bearing gate (a can't-fail check otherwise): the dead-op remover fired EXACTLY once (on %p). Delete the
    // driver's is_erased-on-entry skip and this becomes 2 -- the remover would also match+erase the tombstoned %c in round 1
    // (erasing a tombstone is benign, so num_ops/is_erased alone can't tell the difference). This count is what fails.
    CHECK(dead_erase_count() == 1);
}

TEST_CASE("ceir 26b: a reshape-of-reshape FOLD inserts+RAUWs, leaves the dead input for DCE (compose)", "[ceir][rewrite]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    // ⛔ REGISTER (not just intern) so DCE can reclaim the folded-away reshapes: src/reshape are Pure (DCE-removable when
    // unused); sink is NOT Pure (effectful -> DCE KEEPS it), so the folded reshape it consumes stays live.
    Dialect* const d = ctx.register_dialect("ct");
    d->register_op("src", OpSpec{.traits = flags_of(OpTrait::Pure)});
    d->register_op("reshape", OpSpec{.traits = flags_of(OpTrait::Pure)});
    d->register_op("sink", OpSpec{});
    REQUIRE(ctx.intern_op("ct", "src") == kSrc);
    REQUIRE(ctx.intern_op("ct", "reshape") == kReshape);
    REQUIRE(ctx.intern_op("ct", "sink") == kSink);

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    // %x = ct.src ; %r1 = ct.reshape(%x) ; %r2 = ct.reshape(%r1) ; %s = ct.sink(%r2) -- %s keeps %r2 live.
    Operation* const x = ctx.create_operation(kSrc, {}, 1U);
    block->append(x);
    Value* const     xin[1] = {x->result(0)};
    Operation* const r1     = ctx.create_operation(kReshape, ConstSpan<Value*>(xin, 1U), 1U);
    block->append(r1);
    Value* const     r1in[1] = {r1->result(0)};
    Operation* const r2      = ctx.create_operation(kReshape, ConstSpan<Value*>(r1in, 1U), 1U);
    block->append(r2);
    Value* const     r2in[1] = {r2->result(0)};
    Operation* const s       = ctx.create_operation(kSink, ConstSpan<Value*>(r2in, 1U), 1U);
    block->append(s);
    REQUIRE(block->num_ops() == 4U);

    const RewritePattern fold[1] = {RewritePattern{&match_reshape_of_reshape, &rewrite_fold_reshape}};
    DiagnosticEngine     diag(ctx, &root);
    CHECK(greedy_rewrite(ctx, *m, ConstSpan<RewritePattern>(fold, 1U), diag));
    CHECK_FALSE(diag.has_fatal());
    // idempotence on a CONVERGED real state (the folded reshape's input is not itself a reshape -> no re-fold): a second
    // call changes nothing, gating the "did nothing => invalidate nothing" contract non-trivially (Test 1's is on an empty module).
    CHECK_FALSE(greedy_rewrite(ctx, *m, ConstSpan<RewritePattern>(fold, 1U), diag));

    // the fold ran: %s now consumes a FRESH reshape whose input is %x (both reshapes collapsed to one hop) ...
    const Value* const     s_in   = s->operand(0);
    const Operation* const folded = s_in->defining_op();
    REQUIRE(folded != nullptr);
    CHECK(folded->kind() == kReshape);
    CHECK(folded->parent_block() == block);            // ⛔ correction #2: the new op was LINKED (not floating)
    CHECK(folded->operand(0)->defining_op() == x);     // one hop: reshape(%x), not reshape(reshape(%x))
    CHECK(folded != r2);
    // ... and the fold did NOT erase the now-dead originals -- that is DCE's job (canonicalize -> dce compose).
    CHECK_FALSE(r1->is_erased());
    CHECK_FALSE(r2->is_erased());
    CHECK_FALSE(r2->result(0)->has_uses()); // %r2 is dead (RAUW moved %s to the folded op)

    // compose with DCE: %r2 (dead) dies round 1, then %r1 (only used by %r2) dies round 2.
    CHECK(dce_run(ctx, *m, diag));
    CHECK(r2->is_erased());
    CHECK(r1->is_erased());
    CHECK_FALSE(folded->is_erased()); // the folded op is live (used by %s) -- kept
    CHECK_FALSE(x->is_erased());
    CHECK(block->num_ops() == 3U); // %x, folded reshape, %s
}

TEST_CASE("ceir 26b: a non-monotone (oscillating) pattern pair hits the cap -> FATAL, never hangs", "[ceir][rewrite]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    REQUIRE(ctx.intern_op("ct", "a") == kA);
    REQUIRE(ctx.intern_op("ct", "b") == kB);
    REQUIRE(ctx.intern_op("ct", "pick") == kPick);

    Module* const m     = ctx.create_module();
    Block* const  block = ctx.create_block();
    m->body()->append(block);
    Operation* const a = ctx.create_operation(kA, {}, 1U);
    block->append(a);
    Operation* const b = ctx.create_operation(kB, {}, 1U);
    block->append(b);
    Value* const     ain[1] = {a->result(0)};
    Operation* const pick   = ctx.create_operation(kPick, ConstSpan<Value*>(ain, 1U), 1U);
    block->append(pick);

    const RewritePattern oscillate[2] = {RewritePattern{&match_pick_on_a, &rewrite_pick_to_b},
                                         RewritePattern{&match_pick_on_b, &rewrite_pick_to_a}};
    DiagnosticEngine     diag(ctx, &root);
    // the driver must TERMINATE (the cap = seeded ops + 1) and REPORT rather than loop forever.
    const bool changed = greedy_rewrite(ctx, *m, ConstSpan<RewritePattern>(oscillate, 2U), diag, StringView("ct.oscillate"));
    CHECK(changed);
    CHECK(diag.has_fatal()); // ⛔ the non-monotone set is surfaced, never silently partial
}

// CEIR-27c — the §72 DECLARATIVE REWRITE-RULE asset: an AUTHORED `ceir.rewrite` rule reproduces canonicalize.hpp's identity_reshape
// fold EXACTLY, driven WITHOUT being in kCanonPatterns — the plugin-boundary proof (§72: a plugin ships a rule without editing the
// central C++ switch). (a) the committed rule asset parse-loads canonically, walks clean, loads to the expected RewriteRule; (b)
// DIFFERENTIAL — greedy_rewrite_rules([the rule]) on module A == canonicalize_run on module B, print-equal (the semantics-preserving
// contract) + non-vacuous (the reshape RAUW'd/dead in both); (c) NEGATIVE — a type-CHANGING reshape leaves the rule inert; (d) no
// reshape ⇒ the driver returns false. ⛔ the has_uses re-match guard is exercised by (b)'s 2nd fixpoint round (the RAUW'd reshape
// must not re-fire). Device-free. Retirement of the C++ pattern is deferred past 27d (the differential is the proof).
TEST_CASE("ceir 27c: an AUTHORED ceir.rewrite rule reproduces canonicalize's identity_reshape fold (the plugin boundary)",
          "[ceir][rewrite][transform]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    (void)tensor::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)rewrite::register_rewrite_ops(ctx); // the rewrite dialect (parse the rule asset)

    // ── (a) the committed rule ASSET parse-loads, is CANONICAL, walks clean, and loads to the expected RewriteRule ──
    std::ifstream rf(CRD_REPO_DIR "/assets/ceir/rule_identity_reshape.ceir", std::ios::binary | std::ios::ate);
    REQUIRE(rf.good());
    const std::streamsize rsz = rf.tellg();
    rf.seekg(0);
    crd::containers::Array<char> rsrc(&root);
    rsrc.resize(static_cast<crd::usize>(rsz), '\0');
    rf.read(rsrc.data(), rsz);
    const StringView  file_text(rsrc.data(), static_cast<crd::usize>(rsz));
    const ParseResult rpr = parse(ctx, file_text);
    REQUIRE(rpr.ok);
    REQUIRE(rpr.module != nullptr);
    CHECK(rewrite::find_rewrite_misuse(ctx, *rpr.module).kind == rewrite::RewriteMisuseKind::None);
    const crd::containers::String rtext = print(ctx, *rpr.module, &root); // ANTI-DRIFT/canonical: print(parse(file)) == file
    CHECK(StringView(rtext.c_str(), rtext.size()) == file_text);
    crd::containers::Array<rewrite::RewriteRule> rules(&root);
    const crd::u32                               n = rewrite::load_rewrite_rules(ctx, *rpr.module, rules);
    REQUIRE(n == 1U);
    REQUIRE(rules.size() == 1U);
    CHECK(rules[0].root_kind == ctx.intern_op("tensor", "reshape"));
    CHECK(rules[0].constraint == rewrite::RewriteConstraint::ResultTypeEqOperand);
    CHECK(rules[0].action == rewrite::RewriteAction::ReplaceResultWithOperand);
    CHECK(rules[0].result_idx == 0U);
    CHECK(rules[0].operand_idx == 0U);

    DiagnosticEngine diag(ctx, &root);
    const ConstSpan<rewrite::RewriteRule> rule_span(rules.data(), rules.size());

    // ── (b) DIFFERENTIAL: the authored rule reproduces canonicalize_run's identity_reshape fold EXACTLY (print-equal) ──
    Module* const m_rule  = mk_reshape_mod(ctx, /*same_type=*/true);
    Module* const m_canon = mk_reshape_mod(ctx, /*same_type=*/true);
    const bool    ch_rule  = greedy_rewrite_rules(ctx, *m_rule, rule_span, diag);
    const bool    ch_canon = canonicalize_run(ctx, *m_canon, diag);
    CHECK(ch_rule);  // the authored rule fired
    CHECK(ch_canon); // the C++ pattern fired
    CHECK_FALSE(diag.has_fatal());
    const crd::containers::String p_rule  = print(ctx, *m_rule, &root);
    const crd::containers::String p_canon = print(ctx, *m_canon, &root);
    // ⭐ the authored rule's output == the C++ canonicalize's output — an asset rule reproduces the central pattern WITHOUT being
    //    in kCanonPatterns (the plugin-boundary proof, the semantics-preserving-differential contract).
    CHECK(StringView(p_rule.c_str(), p_rule.size()) == StringView(p_canon.c_str(), p_canon.size()));
    // NON-VACUOUS: the fold actually happened — the reshape's result was RAUW'd (now DEAD) in BOTH modules.
    const OpId reshape_k    = ctx.intern_op("tensor", "reshape");
    const auto reshape_dead = [reshape_k](const Module& mm) {
        for (const Block* bb = mm.body()->first_block(); bb != nullptr; bb = bb->next_in_region())
        {
            for (const Operation* o = bb->first_op(); o != nullptr; o = o->next_in_block())
            {
                if (o->kind() == reshape_k) { return o->num_results() > 0U && !o->result(0)->has_uses(); }
            }
        }
        return false;
    };
    CHECK(reshape_dead(*m_rule));
    CHECK(reshape_dead(*m_canon));
    // ⛔ the has_uses re-match GUARD is load-bearing, proven DISCRIMINATINGLY (advisor): a SECOND rule-driver call over the
    //    already-folded module returns FALSE (the RAUW'd reshape's result has no uses ⇒ no re-fire). WITHOUT the guard the
    //    constraint (result.type==operand.type) STILL holds ⇒ re-fire forever ⇒ the cap ⇒ FATAL. So this false-and-no-fatal
    //    IS the guard's proof — the idempotence check every greedy_rewrite test does (lines 146/220), not a category "nothing broke".
    CHECK_FALSE(greedy_rewrite_rules(ctx, *m_rule, rule_span, diag));
    CHECK_FALSE(diag.has_fatal());

    // ── (c) NEGATIVE: a type-CHANGING reshape ([8,8]→[64]) — the constraint is false, the rule does NOT fire, module UNCHANGED ──
    Module* const            m_neg      = mk_reshape_mod(ctx, /*same_type=*/false);
    const crd::containers::String neg_before = print(ctx, *m_neg, &root);
    const bool               ch_neg     = greedy_rewrite_rules(ctx, *m_neg, rule_span, diag);
    CHECK_FALSE(ch_neg); // result.type != operand.type ⇒ the rule is inert
    const crd::containers::String neg_after = print(ctx, *m_neg, &root);
    CHECK(StringView(neg_after.c_str(), neg_after.size()) == StringView(neg_before.c_str(), neg_before.size()));
    CHECK_FALSE(diag.has_fatal());

    // ── (d) a module with NO reshape ⇒ the driver returns false (nothing matches) ──
    Module* const    m_empty = ctx.create_module();
    Block* const     eb      = ctx.create_block();
    m_empty->body()->append(eb);
    Operation* const only = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U,
                                                 ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<TypeId>{})));
    eb->append(only);
    CHECK_FALSE(greedy_rewrite_rules(ctx, *m_empty, rule_span, diag));
    CHECK_FALSE(diag.has_fatal());
}

// CEIR-27d — the SECOND authored rule reshape_of_reshape: reshape(reshape(x)) -> reshape(x), the BUILD-a-new-op shape (vs 27c's
// RAUW-to-operand). It needs the closed vocabulary's new entries — the `operand_defined_by_root` constraint (a 2-hop path: the
// matched op's operand is produced by another op of the SAME root kind) + the `build_op_from_inner_operand` action (create a root
// op over the GRANDCHILD operand via the generic ctx.create_operation, insert-before, RAUW) + the OPTIONAL `inner_operand` attr
// (absent ⇒ 0). (a) the committed asset parse-loads canonically + loads the 6 fields; (b) reshape_of_reshape ALONE on a
// distinct-type chain == canonicalize_run (print-equal) + FIRE-COUNT identity (exactly 3 reshapes pre-DCE = 2 dead + 1 built,
// "fired once") + idempotence (the universal has_uses guard proof for build_op); (c) COMPOSE — BOTH rules in kCanonPatterns ORDER
// (reshape_of_reshape FIRST, identity SECOND) fully collapse a NET-IDENTITY chain to %x, print-equal to canonicalize (block order
// IS pattern priority — a real §72 semantic); (d) NEGATIVE — a reshape over a non-reshape input is inert. Device-free.
TEST_CASE("ceir 27d: an AUTHORED reshape_of_reshape rule BUILDS the folded op; the two rules COMPOSE == canonicalize",
          "[ceir][rewrite][transform]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                            ctx(&root);
    (void)tensor::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)rewrite::register_rewrite_ops(ctx);

    // read a committed asset's bytes into `buf` and return a view (bytes kept alive for the anti-drift/roundtrip compare).
    const auto read_text = [&](const char* path, crd::containers::Array<char>& buf) -> StringView {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        f.seekg(0);
        buf.resize(static_cast<crd::usize>(sz), '\0');
        f.read(buf.data(), sz);
        return StringView(buf.data(), static_cast<crd::usize>(sz));
    };

    // ── (a) the reshape_of_reshape ASSET parse-loads, is CANONICAL, walks clean, loads to the expected RewriteRule (6 fields) ──
    crd::containers::Array<char> ror_buf(&root);
    const StringView             ror_text = read_text(CRD_REPO_DIR "/assets/ceir/rule_reshape_of_reshape.ceir", ror_buf);
    const ParseResult            ror_pr   = parse(ctx, ror_text);
    REQUIRE(ror_pr.ok);
    REQUIRE(ror_pr.module != nullptr);
    CHECK(rewrite::find_rewrite_misuse(ctx, *ror_pr.module).kind == rewrite::RewriteMisuseKind::None);
    const crd::containers::String ror_print = print(ctx, *ror_pr.module, &root); // ANTI-DRIFT: print(parse(file)) == file
    CHECK(StringView(ror_print.c_str(), ror_print.size()) == ror_text);
    crd::containers::Array<rewrite::RewriteRule> ror_rules(&root);
    REQUIRE(rewrite::load_rewrite_rules(ctx, *ror_pr.module, ror_rules) == 1U);
    CHECK(ror_rules[0].root_kind == ctx.intern_op("tensor", "reshape"));
    CHECK(ror_rules[0].constraint == rewrite::RewriteConstraint::OperandDefinedByRoot);
    CHECK(ror_rules[0].action == rewrite::RewriteAction::BuildOpFromInnerOperand);
    CHECK(ror_rules[0].result_idx == 0U);
    CHECK(ror_rules[0].operand_idx == 0U);
    CHECK(ror_rules[0].inner_operand_idx == 0U); // absent in the asset ⇒ reads 0 (the optional-attr precedent)

    DiagnosticEngine diag(ctx, &root);

    // ── (b) DIFFERENTIAL: reshape_of_reshape ALONE on a DISTINCT-type chain [8,8]->[64]->[4,16] == canonicalize_run (print-equal) ──
    const ConstSpan<rewrite::RewriteRule> ror_span(ror_rules.data(), ror_rules.size());
    Module* const                         m_rule  = mk_chain_mod(ctx, ChainOut::RankBridge);
    Module* const                         m_canon = mk_chain_mod(ctx, ChainOut::RankBridge);
    CHECK(greedy_rewrite_rules(ctx, *m_rule, ror_span, diag));
    CHECK(canonicalize_run(ctx, *m_canon, diag));
    CHECK_FALSE(diag.has_fatal());
    const crd::containers::String p_rule  = print(ctx, *m_rule, &root);
    const crd::containers::String p_canon = print(ctx, *m_canon, &root);
    // ⭐ the authored BUILD-op rule's output == the C++ canonicalize's reshape_of_reshape output, WITHOUT being in kCanonPatterns.
    CHECK(StringView(p_rule.c_str(), p_rule.size()) == StringView(p_canon.c_str(), p_canon.size()));
    // ⭐ FIRE-COUNT IDENTITY (not "something folded"): build_op fired EXACTLY once ⇒ exactly 3 tensor.reshape ops pre-DCE (the 2
    //    dead originals + the 1 folded). A second fire would make 4; no fire, 2. This is the "fired once" discriminator.
    const OpId reshape_k      = ctx.intern_op("tensor", "reshape");
    const auto count_reshapes = [reshape_k](const Module& mm) {
        crd::u32 c = 0;
        for (const Block* bb = mm.body()->first_block(); bb != nullptr; bb = bb->next_in_region())
        {
            for (const Operation* o = bb->first_op(); o != nullptr; o = o->next_in_block())
            {
                if (o->kind() == reshape_k) { ++c; }
            }
        }
        return c;
    };
    CHECK(count_reshapes(*m_rule) == 3U);
    CHECK(count_reshapes(*m_canon) == 3U);
    // idempotence: a 2nd driver call over the folded module returns FALSE + no fatal — the UNIVERSAL has_uses guard proof for
    // build_op (WITHOUT it the dead %r2 STILL satisfies operand_defined_by_root ⇒ re-fires forever ⇒ the cap ⇒ FATAL).
    CHECK_FALSE(greedy_rewrite_rules(ctx, *m_rule, ror_span, diag));
    CHECK_FALSE(diag.has_fatal());

    // ── (c) COMPOSE: BOTH rules in kCanonPatterns ORDER (reshape_of_reshape [0], identity [1]) collapse a NET-IDENTITY chain
    //        [8,8]->[64]->[8,8] to %x, print-equal to canonicalize_run (which runs the same pair in the same order) ──
    crd::containers::Array<char> id_buf(&root);
    const StringView             id_text = read_text(CRD_REPO_DIR "/assets/ceir/rule_identity_reshape.ceir", id_buf);
    const ParseResult            id_pr   = parse(ctx, id_text);
    REQUIRE(id_pr.ok);
    REQUIRE(id_pr.module != nullptr);
    crd::containers::Array<rewrite::RewriteRule> both(&root);
    REQUIRE(rewrite::load_rewrite_rules(ctx, *ror_pr.module, both) == 1U); // both[0] = reshape_of_reshape (higher priority)
    REQUIRE(rewrite::load_rewrite_rules(ctx, *id_pr.module, both) == 1U);  // both[1] = identity_reshape
    REQUIRE(both.size() == 2U);
    const ConstSpan<rewrite::RewriteRule> both_span(both.data(), both.size());
    Module* const                         m_comp   = mk_chain_mod(ctx, ChainOut::NetIdentity);
    Module* const                         m_ccanon = mk_chain_mod(ctx, ChainOut::NetIdentity);
    CHECK(greedy_rewrite_rules(ctx, *m_comp, both_span, diag));
    CHECK(canonicalize_run(ctx, *m_ccanon, diag));
    CHECK_FALSE(diag.has_fatal());
    const crd::containers::String pc_rule  = print(ctx, *m_comp, &root);
    const crd::containers::String pc_canon = print(ctx, *m_ccanon, &root);
    CHECK(StringView(pc_rule.c_str(), pc_rule.size()) == StringView(pc_canon.c_str(), pc_canon.size()));
    // the collapse is real (not just print-matched): resource.export now consumes the DECLARE's result directly (both reshapes
    // RAUW'd away — the T->U->T net-identity fully folded to %x).
    const auto export_operand_is_declare = [&](const Module& mm) {
        const OpId       decl_k = ctx.intern_op("resource", "declare");
        const OpId       exp_k  = ctx.intern_op("resource", "export");
        const Operation* decl   = nullptr;
        for (const Block* bb = mm.body()->first_block(); bb != nullptr; bb = bb->next_in_region())
        {
            for (const Operation* o = bb->first_op(); o != nullptr; o = o->next_in_block())
            {
                if (o->kind() == decl_k) { decl = o; }
                if (o->kind() == exp_k && o->num_operands() > 0U) { return decl != nullptr && o->operand(0)->defining_op() == decl; }
            }
        }
        return false;
    };
    CHECK(export_operand_is_declare(*m_comp));
    CHECK(export_operand_is_declare(*m_ccanon));

    // ── (d) NEGATIVE: a single reshape over a NON-reshape (declare) input ⇒ operand_defined_by_root false ⇒ the rule is inert ──
    Module* const                 m_neg      = mk_reshape_mod(ctx, /*same_type=*/false); // %r = reshape(declare) : [64]
    const crd::containers::String neg_before = print(ctx, *m_neg, &root);
    CHECK_FALSE(greedy_rewrite_rules(ctx, *m_neg, ror_span, diag)); // the operand's defining op is a declare, not a reshape
    const crd::containers::String neg_after = print(ctx, *m_neg, &root);
    CHECK(StringView(neg_after.c_str(), neg_after.size()) == StringView(neg_before.c_str(), neg_before.size()));
    CHECK_FALSE(diag.has_fatal());

    // ── (e) ORDER-PRIORITY (block order IS pattern priority — a real §72 semantic, proven not asserted): on the OUTER-identity
    //        chain [8,8]->[64]->[64] BOTH rules match %r2 (r1 is a reshape; r2.result[64] == r1.result[64]). reshape_of_reshape
    //        FIRST (kCanonPatterns order) BUILDS the fold (3 reshapes) == canonicalize_run; identity FIRST RAUWs %r2->%r1 so NO
    //        fold is ever built (2 reshapes) != canonicalize. The distinct outcomes ARE the discriminator. ──
    Module* const m_ord  = mk_chain_mod(ctx, ChainOut::OuterIdentity);
    Module* const m_ocan = mk_chain_mod(ctx, ChainOut::OuterIdentity);
    CHECK(greedy_rewrite_rules(ctx, *m_ord, both_span, diag)); // both[0]=reshape_of_reshape (priority), both[1]=identity
    CHECK(canonicalize_run(ctx, *m_ocan, diag));
    CHECK_FALSE(diag.has_fatal());
    const crd::containers::String po_rule  = print(ctx, *m_ord, &root);
    const crd::containers::String po_canon = print(ctx, *m_ocan, &root);
    CHECK(StringView(po_rule.c_str(), po_rule.size()) == StringView(po_canon.c_str(), po_canon.size())); // r-o-r first == canon
    CHECK(count_reshapes(*m_ord) == 3U);                                                                 // the fold WAS built
    // reversed priority (identity FIRST) on the SAME chain: identity RAUWs the outer before reshape_of_reshape sees it ⇒ NO fold
    // (2 reshapes) and the module DIFFERS from canonicalize — the proof that module block order selects the pattern.
    const rewrite::RewriteRule            rev[2] = {both[1], both[0]}; // identity, then reshape_of_reshape
    const ConstSpan<rewrite::RewriteRule> rev_span(rev, 2U);
    Module* const                         m_rev = mk_chain_mod(ctx, ChainOut::OuterIdentity);
    CHECK(greedy_rewrite_rules(ctx, *m_rev, rev_span, diag));
    CHECK_FALSE(diag.has_fatal());
    CHECK(count_reshapes(*m_rev) == 2U); // identity-first ⇒ NO fold built
    const crd::containers::String pr_rev = print(ctx, *m_rev, &root);
    CHECK_FALSE(StringView(pr_rev.c_str(), pr_rev.size()) == StringView(po_canon.c_str(), po_canon.size())); // != canonicalize

    // ── (f) CROSS-PAIRING GUARD (crash-prevention): a rule whose action WALKS a path (build_op_from_inner_operand) paired with a
    //        constraint that does NOT validate it (result_type_eq_operand) — BOTH strings are in the closed vocab, so it parses,
    //        but rule_apply would deref operand.def.operand the constraint never checked. find_rewrite_misuse rejects it as
    //        ActionNeedsConstraint BEFORE the driver runs (the 27a DuplicateDirective shape: a readable .ceir that would crash). ──
    const StringView bad_text("module {\n  ^bb0:\n    rewrite.rule() {action = \"build_op_from_inner_operand\", constraint = "
                              "\"result_type_eq_operand\", operand = 0, result = 0, root = \"tensor.reshape\"}\n}\n");
    const ParseResult bad_pr = parse(ctx, bad_text);
    REQUIRE(bad_pr.ok);
    REQUIRE(bad_pr.module != nullptr);
    CHECK(rewrite::find_rewrite_misuse(ctx, *bad_pr.module).kind == rewrite::RewriteMisuseKind::ActionNeedsConstraint);
}
