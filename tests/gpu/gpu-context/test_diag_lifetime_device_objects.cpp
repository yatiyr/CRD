// DIAG.10c -- device objects in the lifetime ledger: two independent counts that must agree.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): the process-wide identity registry feeds the
// ledger's DeviceObject count, one per live Resource or Program identity from mint to retire; a Pass identity names a
// recorded pass, not a device object, and is not counted; a double or invalid retire releases nothing; a registry of
// one's own (built without the feed) records nothing. The registry's own live_count is the second count the ledger
// is held to. Device-free: the backends call the same mint/retire at their creation and destruction sites.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/gpu/identity_registry.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace g = crd::gpu;
using crd::LifetimeClass;
using crd::LifetimeVerdict;

crd::i64 registry_objects()
{
    const g::IdentityRegistry& reg = g::identity_registry();
    return static_cast<crd::i64>(reg.live_count(g::ObjectKind::Resource) + reg.live_count(g::ObjectKind::Program));
}

crd::i64 ledger_objects(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot()).live_change(LifetimeClass::DeviceObject);
}
} // namespace

TEST_CASE("diag10c: the ledger's device objects move with the identity registry's live count",
          "[gpu][lifetime][diag10c]")
{
    g::IdentityRegistry&        reg          = g::identity_registry();
    const crd::LifetimeSnapshot start        = crd::lifetime_ledger().snapshot();
    const crd::i64              registry_was = registry_objects();

    const g::ObjectIdentity resource = reg.mint(g::ObjectKind::Resource);
    const g::ObjectIdentity program  = reg.mint(g::ObjectKind::Program);
    const g::ObjectIdentity pass     = reg.mint(g::ObjectKind::Pass); // not a device object
    CHECK(ledger_objects(start) == 2);
    CHECK(registry_objects() - registry_was == 2);
    const crd::LifetimeReport held = crd::lifetime_report(start, crd::lifetime_ledger().snapshot());
    CHECK(held.verdict == LifetimeVerdict::Outstanding);
    CHECK(held.outstanding == crd::lifetime_class_bit(LifetimeClass::DeviceObject));

    CHECK(reg.retire(resource));
    CHECK_FALSE(reg.retire(resource)); // stale: nothing more is released
    CHECK_FALSE(reg.retire(g::ObjectIdentity{}));
    CHECK(ledger_objects(start) == 1);
    CHECK(registry_objects() - registry_was == 1);

    CHECK(reg.retire(pass));
    CHECK(ledger_objects(start) == 1);
    CHECK(reg.retire(program));
    CHECK(ledger_objects(start) == 0);
    CHECK(registry_objects() == registry_was);

    const crd::LifetimeReport r = crd::lifetime_report(start, crd::lifetime_ledger().snapshot());
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(r.underflowed == 0U);
}

TEST_CASE("diag10c: a registry of one's own does not feed the ledger", "[gpu][lifetime][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        g::IdentityRegistry     own;
        const g::ObjectIdentity a = own.mint(g::ObjectKind::Resource);
        (void)own.mint(g::ObjectKind::Program);
        CHECK(ledger_objects(start) == 0);
        CHECK(own.retire(a));
    }
    CHECK(crd::lifetime_report(start, crd::lifetime_ledger().snapshot()).verdict == LifetimeVerdict::Balanced);
}
