// DIAG.10c -- hot-reload subscriptions in the lifetime ledger.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): each subscribe_reload() is one
// CallbackRegistration until unsubscribe_reload() with its token, or until the manager is destroyed with it still
// registered; an unknown token or a repeated unsubscribe releases nothing.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/resources/resource_manager.hpp>

#include <catch2/catch_test_macros.hpp>

#include <new>

namespace
{
using crd::LifetimeClass;
using crd::LifetimeVerdict;
using crd::resources::ResourceId;
using crd::resources::ResourceManager;

// Never destroyed: static-destruction-order safe, like the other resource tests' allocators.
alignas(crd::memory::GrowableTlsfAllocator) unsigned char s_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
crd::memory::GrowableTlsfAllocator& s_alloc = *::new (s_buf) crd::memory::GrowableTlsfAllocator();

void on_reload(ResourceId /*id*/, crd::u32 /*generation*/, void* /*user*/)
{
}

crd::i64 live_registrations(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot())
        .live_change(LifetimeClass::CallbackRegistration);
}
} // namespace

TEST_CASE("diag10c: each reload subscription is one callback registration until it ends",
          "[resources][lifetime][diag10c]")
{
    const ResourceId            a     = ResourceId::mint_random();
    const ResourceId            b     = ResourceId::mint_random();
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        ResourceManager rm(&s_alloc);
        const crd::u32  first = rm.subscribe_reload(a, &on_reload, nullptr);
        (void)rm.subscribe_reload(a, &on_reload, nullptr);
        (void)rm.subscribe_reload(b, &on_reload, nullptr);
        CHECK(live_registrations(start) == 3);

        rm.unsubscribe_reload(a, first);
        rm.unsubscribe_reload(a, first);         // already gone
        rm.unsubscribe_reload(b, first + 1000U); // never issued
        CHECK(live_registrations(start) == 2);
    } // the manager ends with two subscriptions still registered
    const crd::LifetimeReport r = crd::lifetime_report(start, crd::lifetime_ledger().snapshot());
    char                      line[160];
    (void)crd::format_lifetime_report(r, line, sizeof(line));
    INFO("window: " << line);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
}
