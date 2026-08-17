// CEIR-8g (ADR-0117, U-§65/U-§73) — the AnalysisManager + PassManager frameworks. An analysis computes once + caches
// (never-REDUNDANT); a pass that reports `changed` invalidates every analysis it does NOT preserve (never-STALE); a
// pass that reports unchanged invalidates nothing (preserve-all); preserve_all/none. A compute COUNTER proves both
// directions. Host-only. ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/pass_manager.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/construct.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;

namespace
{
// A function-local static counter (not a namespace-scope global) — reset per test.
int& compute_counter()
{
    static int c = 0;
    return c;
}

// A trivial cached analysis: the op count of the module body. `compute` bumps the counter so a test can distinguish a
// fresh computation from a cache hit.
struct OpCountAnalysis
{
    static constexpr AnalysisId kId = make_analysis_id("test.op_count");
    crd::u32                    op_count = 0U;

    static const OpCountAnalysis* compute(Context&, const Module& m, crd::memory::GrowableLinearAllocator& arena)
    {
        ++compute_counter();
        auto* const r = crd::memory::construct<OpCountAnalysis>(arena);
        for (Block* b = m.body()->first_block(); b != nullptr; b = b->next_in_region())
        {
            for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block()) { ++r->op_count; }
        }
        return r;
    }
};

Module* empty_module(Context& ctx)
{
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    top->append(ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U));
    return m;
}
bool pass_changed(Context&, Module&, DiagnosticEngine&) { return true; }    // claims it mutated the module
bool pass_unchanged(Context&, Module&, DiagnosticEngine&) { return false; } // did nothing
bool pass_emit_fatal(Context&, Module&, DiagnosticEngine& d)
{
    d.emit(Severity::Fatal, make_diagnostic_code("test.fatal"), crd::containers::StringView("test.fatal"), SourceLoc{},
           crd::containers::StringView("stop the pipeline"));
    return true;
}
int& ran_after_fatal()
{
    static int c = 0;
    return c;
}
bool pass_marks_ran(Context&, Module&, DiagnosticEngine&)
{
    ++ran_after_fatal();
    return false;
}
// Append an op to the module's first block (a real mutation, so a recompute yields DIFFERENT data).
void append_op(Context& ctx, Module& m) { m.body()->first_block()->append(ctx.create_operation(ctx.intern_op("test", "op"), {}, 0U)); }
} // namespace

TEST_CASE("ceir 8g: an analysis computes once and caches (never-redundant)", "[ceir][analysis]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m = empty_module(ctx);
    compute_counter()             = 0;

    AnalysisManager am(&root);
    const OpCountAnalysis* const a1 = am.get<OpCountAnalysis>(ctx, *m);
    REQUIRE(a1 != nullptr);
    CHECK(a1->op_count == 1U);
    CHECK(compute_counter() == 1);
    const OpCountAnalysis* const a2 = am.get<OpCountAnalysis>(ctx, *m);
    CHECK(a2 == a1);                  // same cached pointer
    CHECK(compute_counter() == 1);    // NOT recomputed
    CHECK(am.is_cached(OpCountAnalysis::kId));
}

TEST_CASE("ceir 8g: invalidation forces recompute; a preserved analysis does not (never-stale + never-redundant)", "[ceir][analysis]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m = empty_module(ctx);
    compute_counter()             = 0;

    AnalysisManager am(&root);
    (void)am.get<OpCountAnalysis>(ctx, *m);
    CHECK(compute_counter() == 1);

    am.invalidate({}); // preserve NONE (a span of 0) -> evict OpCountAnalysis
    CHECK_FALSE(am.is_cached(OpCountAnalysis::kId));
    (void)am.get<OpCountAnalysis>(ctx, *m);
    CHECK(compute_counter() == 2); // ⛔ recomputed (never-stale)

    // preserve it: invalidate([kId]) keeps it -> no recompute
    const AnalysisId keep[1] = {OpCountAnalysis::kId};
    am.invalidate(ConstSpan<AnalysisId>(keep, 1U));
    CHECK(am.is_cached(OpCountAnalysis::kId));
    (void)am.get<OpCountAnalysis>(ctx, *m);
    CHECK(compute_counter() == 2); // ⛔ NOT recomputed (never-redundant)

    am.invalidate_all(); // preserve_none convenience
    CHECK_FALSE(am.is_cached(OpCountAnalysis::kId));
}

TEST_CASE("ceir 8g: the PassManager drives invalidation; unchanged preserves all", "[ceir][pass]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m = empty_module(ctx);
    compute_counter()             = 0;
    AnalysisManager am(&root);
    (void)am.get<OpCountAnalysis>(ctx, *m); // counter = 1, cached

    DiagnosticEngine diag(ctx, &root);
    // a pass that reports UNCHANGED preserves everything regardless of its set -> no invalidation.
    PassManager pm_noop(&root);
    pm_noop.add_pass(Pass{crd::containers::StringView("noop"), &pass_unchanged, {}}); // preserved = {} but unchanged
    pm_noop.run(ctx, *m, am, diag);
    (void)am.get<OpCountAnalysis>(ctx, *m);
    CHECK(compute_counter() == 1); // unchanged -> preserve-all -> still cached

    // a pass that reports CHANGED with an empty preserved set invalidates OpCountAnalysis.
    PassManager pm_mut(&root);
    pm_mut.add_pass(Pass{crd::containers::StringView("mutate"), &pass_changed, {}});
    pm_mut.run(ctx, *m, am, diag);
    (void)am.get<OpCountAnalysis>(ctx, *m);
    CHECK(compute_counter() == 2); // changed + not preserved -> recomputed
}

TEST_CASE("ceir 8g: invalidation serves FRESH data, not just a fresh compute-count", "[ceir][analysis]")
{
    // ⛔ pins the PROPERTY U-§73 names (never STALE), not merely the compute counter: after a real mutation +
    // invalidation, the analysis result reflects the new IR.
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m = empty_module(ctx); // 1 op
    compute_counter()             = 0;
    AnalysisManager am(&root);
    CHECK(am.get<OpCountAnalysis>(ctx, *m)->op_count == 1U);

    append_op(ctx, *m); // the IR now has 2 ops
    CHECK(am.get<OpCountAnalysis>(ctx, *m)->op_count == 1U); // STALE cached result (still 1) — until invalidated
    am.invalidate({});
    CHECK(am.get<OpCountAnalysis>(ctx, *m)->op_count == 2U); // ⛔ FRESH data after invalidation
}

TEST_CASE("ceir 8g: a Fatal diagnostic short-circuits the pass pipeline", "[ceir][pass]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m = empty_module(ctx);
    AnalysisManager              am(&root);
    DiagnosticEngine             diag(ctx, &root);
    ran_after_fatal()            = 0;

    PassManager pm(&root);
    pm.add_pass(Pass{crd::containers::StringView("fatal"), &pass_emit_fatal, {}});
    pm.add_pass(Pass{crd::containers::StringView("after"), &pass_marks_ran, {}});
    pm.run(ctx, *m, am, diag);
    CHECK(diag.has_fatal());
    CHECK(ran_after_fatal() == 0); // ⛔ the second pass NEVER ran (Fatal stopped the pipeline)
}

TEST_CASE("ceir 8g: analysis ids are distinct FNVs sharing the one hash routine", "[ceir][analysis]")
{
    CHECK(OpCountAnalysis::kId == make_analysis_id("test.op_count"));
    CHECK(make_analysis_id("a") != make_analysis_id("b"));
    // ⛔ the ONE-shared-fnv1a_ct pin: an analysis id and an interface id of the SAME name are the SAME hash value.
    CHECK(make_analysis_id("crd.same").value == make_interface_id("crd.same").value);
}
