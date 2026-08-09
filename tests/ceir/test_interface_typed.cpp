// CEIR-8e (ADR-0115) — the trait/interface split. Traits are the CLOSED core vocabulary (a factory guard rejects a
// minted bit); PLUGIN BEHAVIOR lives in the TYPED open-world op-interface surface. The matrix: an analysis dispatches
// through a typed interface it registered, knowing nothing about the op-kind (zero central edits); the compile-time
// InterfaceId (T::kId) equals the runtime intern of the same name (the FNV model); distinct names get distinct ids; a
// kind that did not register the interface (or an unregistered kind) yields nullptr; the reserved catalog names are
// distinct; interface_name reverse-looks-up; and kKnownTraitsMask covers exactly the 8 core traits. Host-only. ASCII.

#include <crd/ceir/ceir.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::u64;

namespace
{
// A CostInterface impl for a fictional `test.expensive` op — the analysis below dispatches through it without ever
// naming the op-kind (the whole point of §7 / the open-world interface surface).
u64 expensive_cost(const Context&, const Operation&) noexcept { return 4242U; }
u64 cheaper_cost(const Context&, const Operation&) noexcept { return 7U; }
const CostInterface kExpensiveCost{&expensive_cost};
const CostInterface kCheaperCost{&cheaper_cost};
} // namespace

TEST_CASE("ceir 8e: an analysis dispatches through a TYPED op-interface, knowing nothing about the op-kind", "[ceir][interface]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d    = ctx.register_dialect("test");
    const OpId                   kind = d->register_op("expensive", {});
    register_op_interface<CostInterface>(ctx, kind, &kExpensiveCost); // the dialect binds behavior — ZERO central edits

    Operation* const op = ctx.create_operation(kind, {}, 0U);
    // the analysis asks the op-KIND for the Cost interface and dispatches — no switch, no trait, no kind name.
    const CostInterface* const impl = get_op_interface<CostInterface>(ctx, op->kind());
    REQUIRE(impl != nullptr);
    CHECK(impl->cost(ctx, *op) == 4242U);

    // registering a SECOND impl for the same interface OVERWRITES in place (the pre-existing branch, now reachable
    // through the typed path) — the last binding wins, no duplicate slot.
    register_op_interface<CostInterface>(ctx, kind, &kCheaperCost);
    CHECK(get_op_interface<CostInterface>(ctx, kind)->cost(ctx, *op) == 7U);

    // a kind that did NOT register the interface -> nullptr (the analysis degrades, never crashes).
    const OpId plain = d->register_op("cheap", {});
    CHECK(get_op_interface<CostInterface>(ctx, plain) == nullptr);
    // an UNREGISTERED (unknown-dialect) kind -> nullptr too.
    CHECK(get_op_interface<CostInterface>(ctx, ctx.intern_op("plugin", "widget")) == nullptr);
}

TEST_CASE("ceir 8e: a typed interface's compile-time kId equals the runtime intern of its name (the FNV model)", "[ceir][interface]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    // T::kId is a COMPILE-TIME constant (no registry scan); intern_interface computes the SAME FNV at runtime.
    CHECK(CostInterface::kId == ctx.intern_interface(CostInterface::kName));
    CHECK(CostInterface::kId.valid());
    CHECK(make_interface_id("crd.iface.cost") == CostInterface::kId);
    // distinct names -> distinct ids (no collision across the surface).
    CHECK(make_interface_id("crd.iface.cost") != make_interface_id("crd.iface.shape"));
    // the reverse-lookup name table (diagnostics) resolves an interned id back to its name.
    CHECK(ctx.interface_name(CostInterface::kId) == crd::containers::StringView{"crd.iface.cost"});
}

TEST_CASE("ceir 8e: the reserved interface catalog names are all distinct", "[ceir][interface]")
{
    namespace ri = reserved_interfaces;
    const InterfaceId ids[7] = {make_interface_id(ri::kMemoryEffect), make_interface_id(ri::kShape),
                                make_interface_id(ri::kLowering),     make_interface_id(ri::kTimeline),
                                make_interface_id(ri::kIncremental),  make_interface_id(ri::kConstraint),
                                make_interface_id(ri::kSerialization)};
    for (crd::u32 i = 0; i < 7U; ++i)
    {
        CHECK(ids[i].valid());
        CHECK(ids[i] != CostInterface::kId); // the reserved names don't collide with the live proof interface
        for (crd::u32 j = i + 1U; j < 7U; ++j) { CHECK(ids[i] != ids[j]); }
    }
}

TEST_CASE("ceir 8e: the closed core-trait vocabulary is exactly 8 contiguous bits (kKnownTraitsMask)", "[ceir][interface]")
{
    // ⛔ traits are CLOSED (dialects SET core bits, never MINT). register_op REJECTS a stray bit (the factory leg,
    // proven at the assert like Pure=>zero — an abort can't be death-tested here); this pins the mask it checks against.
    CHECK(kKnownTraitsMask == 0xFFU);
    const OpTrait all[8] = {OpTrait::Terminator,  OpTrait::Symbol,        OpTrait::SymbolTable,   OpTrait::Pure,
                            OpTrait::IsolatedFromAbove, OpTrait::StateEdge, OpTrait::TokenProducer, OpTrait::TokenConsumer};
    crd::u32 combined = 0U;
    for (const OpTrait t : all)
    {
        CHECK((flags_of(t) & ~kKnownTraitsMask) == 0U); // every known trait is inside the mask
        combined |= flags_of(t);
    }
    CHECK(combined == kKnownTraitsMask); // ...and together they ARE the mask (no gap, no stray)

    // a registered op that SETS a known trait is fine (the allowed direction).
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d = ctx.register_dialect("t");
    const OpId                   k = d->register_op("s", {.traits = flags_of(OpTrait::StateEdge)});
    CHECK(ctx.has_trait(k, OpTrait::StateEdge));
}
