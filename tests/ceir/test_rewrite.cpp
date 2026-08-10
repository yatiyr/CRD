// CEIR-8g (ADR-0117, U-§67/U-§74) — the rewrite/conversion SKELETON. try_apply matches + rewrites ONE op (caller-driven,
// per-op — the block-walking driver is reserved for CEIR-26); a ConversionTarget declares per-op-kind legality with an
// UNLISTED kind defaulting to ILLEGAL (EMPTY!=UNKNOWN). Host-only. ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/rewrite.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir; // NOLINT(google-build-using-namespace)

namespace
{
// An op-kind's OpId is the FNV of "dialect.op" — computable at compile time (fnv1a_ct), so a match hook needs no intern
// (a fn-ptr cannot capture). Pinned equal to intern_op in the test.
bool match_target(const Context&, const Operation& op) noexcept { return op.kind() == OpId{fnv1a_ct("test.target")}; }
void rewrite_mark(Context& ctx, Operation& op) { ctx.set_attr(&op, "rewritten", ctx.attr_bool(true)); }
bool legal_if_no_operands(const Context&, const Operation& op) noexcept { return op.num_operands() == 0U; }
} // namespace

TEST_CASE("ceir 8g: try_apply matches + rewrites one op, declines a non-matching op", "[ceir][rewrite]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    CHECK(ctx.intern_op("test", "target") == OpId{fnv1a_ct("test.target")}); // justifies the capture-free match hook

    const RewritePattern p{&match_target, &rewrite_mark};
    Operation* const     target = ctx.create_operation(ctx.intern_op("test", "target"), {}, 0U);
    Operation* const     other  = ctx.create_operation(ctx.intern_op("test", "other"), {}, 0U);

    CHECK(try_apply(p, ctx, *target)); // matches -> rewrites
    CHECK(target->has_attr("rewritten"));
    CHECK_FALSE(try_apply(p, ctx, *other)); // declines
    CHECK_FALSE(other->has_attr("rewritten"));

    // a match-only pattern (nullptr rewrite) applies without mutating; a nullptr match never applies.
    const RewritePattern probe{&match_target, nullptr};
    Operation* const     t2 = ctx.create_operation(ctx.intern_op("test", "target"), {}, 0U);
    CHECK(try_apply(probe, ctx, *t2));
    CHECK_FALSE(t2->has_attr("rewritten"));
    CHECK_FALSE(try_apply(RewritePattern{}, ctx, *t2)); // both hooks null -> never applies
}

TEST_CASE("ceir 8g: ConversionTarget legality; an UNLISTED kind is Illegal (EMPTY!=UNKNOWN)", "[ceir][rewrite]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    ConversionTarget             target(&root);
    const OpId legal   = ctx.intern_op("test", "legal");
    const OpId illegal = ctx.intern_op("test", "illegal");
    const OpId dyn     = ctx.intern_op("test", "dyn");
    target.set_legal(legal);
    target.set_illegal(illegal);
    target.set_dynamic(dyn, &legal_if_no_operands);

    CHECK(target.is_legal(ctx, *ctx.create_operation(legal, {}, 0U)));
    CHECK_FALSE(target.is_legal(ctx, *ctx.create_operation(illegal, {}, 0U)));
    CHECK(target.is_legal(ctx, *ctx.create_operation(dyn, {}, 0U))); // dynamic: 0 operands -> legal
    // ⛔ an UNLISTED kind is ILLEGAL by default (an unknown op must not read as legal).
    CHECK_FALSE(target.is_legal(ctx, *ctx.create_operation(ctx.intern_op("test", "unlisted"), {}, 0U)));

    // last-set-wins: re-declaring `legal` as illegal overwrites in place.
    target.set_illegal(legal);
    CHECK_FALSE(target.is_legal(ctx, *ctx.create_operation(legal, {}, 0U)));
}
