// DIAG.10c -- event-bus subscriptions in the lifetime ledger.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): each subscription is one CallbackRegistration
// from subscribe() until unsubscribe(), a move-assignment over a bus that still holds subscriptions, or the bus's
// destruction; a move transfers them without counting them twice; an unknown or repeated unsubscribe releases
// nothing. A bus cannot be copied, so no subscription can be counted twice.

#include <crd/app/app.hpp>
#include <crd/core/lifetime_ledger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

namespace
{
using crd::LifetimeClass;
using crd::LifetimeVerdict;

class PingEvent final : public crd::app::EventT<PingEvent, static_cast<crd::u32>(crd::app::EventCategory::Application)>
{
public:
    static constexpr crd::containers::StringView kName = "PingEvent";
};

crd::i64 live_registrations(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot())
        .live_change(LifetimeClass::CallbackRegistration);
}

static_assert(!std::is_copy_constructible_v<crd::app::EventBus>);
static_assert(!std::is_copy_assignable_v<crd::app::EventBus>);
} // namespace

TEST_CASE("diag10c: each bus subscription is one callback registration until it ends", "[app][lifetime][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    int                         pings = 0;
    {
        crd::app::EventBus                 bus;
        const crd::app::EventSubscription first  = bus.subscribe<PingEvent>([&pings](PingEvent&) { ++pings; });
        const crd::app::EventSubscription second = bus.subscribe<PingEvent>([&pings](PingEvent&) { ++pings; });
        CHECK(live_registrations(start) == 2);

        bus.unsubscribe(first);
        bus.unsubscribe(first);                         // already gone
        bus.unsubscribe(crd::app::EventSubscription{}); // never registered
        CHECK(live_registrations(start) == 1);

        crd::app::EventBus moved(std::move(bus)); // the subscription moves with the bus
        CHECK(live_registrations(start) == 1);
        PingEvent e;
        moved.publish(e);
        CHECK(pings == 1);

        crd::app::EventBus other;
        (void)other.subscribe<PingEvent>([](PingEvent&) {});
        (void)other.subscribe<PingEvent>([](PingEvent&) {});
        CHECK(live_registrations(start) == 3);
        other = std::move(moved); // other's two end here; moved's one is now other's
        CHECK(live_registrations(start) == 1);
        other.unsubscribe(second);
        CHECK(live_registrations(start) == 0);
        (void)other.subscribe<PingEvent>([](PingEvent&) {}); // left for the destructor to end
    }
    const crd::LifetimeReport r = crd::lifetime_report(start, crd::lifetime_ledger().snapshot());
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(r.underflowed == 0U);
}
