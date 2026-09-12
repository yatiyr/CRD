#include <crd/app/app.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
class CustomEditorEvent final
    : public crd::app::EventT<CustomEditorEvent, static_cast<crd::u32>(crd::app::EventCategory::Application)>
{
public:
    static constexpr crd::containers::StringView kName = "CustomEditorEvent";

    explicit CustomEditorEvent(int value) noexcept : m_value(value) {}
    [[nodiscard]] int value() const noexcept { return m_value; }

private:
    int m_value = 0;
};
} // namespace

TEST_CASE("EventDispatcher dispatches by concrete type", "[app][event]")
{
    crd::app::KeyPressedEvent event(crd::platform::Key::A, {}, false);
    crd::app::Event& base = event;

    bool called = false;
    crd::app::EventDispatcher dispatcher(base);
    REQUIRE(dispatcher.dispatch<crd::app::KeyPressedEvent>(
        [&called](crd::app::KeyPressedEvent& e)
        {
            called = true;
            return e.key() == crd::platform::Key::A;
        }));
    REQUIRE(called);
    REQUIRE(base.handled);
}

TEST_CASE("EventBus supports app-defined typed events", "[app][event][bus]")
{
    crd::app::EventBus bus;

    int seen = 0;
    const auto subscription = bus.subscribe<CustomEditorEvent>([&seen](CustomEditorEvent& e) { seen += e.value(); });
    REQUIRE(subscription.is_valid());

    CustomEditorEvent event(7);
    bus.publish(event);
    REQUIRE(seen == 7);

    bus.unsubscribe(subscription);
    CustomEditorEvent event2(3);
    bus.publish(event2);
    REQUIRE(seen == 7);
}

TEST_CASE("Propagated events expose handled semantics through categories", "[app][event]")
{
    crd::app::MouseButtonPressedEvent event(crd::platform::MouseButton::Left, {});
    REQUIRE(event.is_in_category(crd::app::EventCategory::Input));
    REQUIRE(event.is_in_category(crd::app::EventCategory::Mouse));
    REQUIRE(event.is_in_category(crd::app::EventCategory::MouseButton));
    REQUIRE_FALSE(event.is_in_category(crd::app::EventCategory::Keyboard));
}
