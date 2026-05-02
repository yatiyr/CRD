# crd-app

Application-layer glue over the lower engine modules. `crd-app` owns the
main loop, layer stack, propagated event dispatch, and a small typed sync
event bus. It sits above `crd-platform` and below future editor / gameplay
code.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | `Event`, `EventDispatcher`, built-in app/window/input events | ✅ |
| v1b | `Layer`, `LayerStack` | ✅ |
| v1c | `Application`, platform event lifting, layer ownership | ✅ |
| v1d | typed sync `EventBus`, `smoke_app` | ✅ |

## Core decisions

- Cerid is **not** using a pure Hazel clone. The propagated side is
  Hazel-like: input/window/app events walk the `LayerStack` in reverse,
  and `handled` stops traversal. But Cerid also ships a separate
  `EventBus` for broadcast-style notifications.
- `handled` semantics belong **only** to propagated events.
  Event-bus traffic is broadcast and non-consuming by design.
- The shipped bus is **sync for now**. The public API is already typed,
  and the roadmap explicitly leaves room for an async version later if
  cross-thread producers become real.
- Event identity is **per-type token based**, not a fixed global enum.
  That means applications built on Cerid can define their own event
  classes without editing engine headers or registering central IDs.
- `Application` is **not** a singleton. Ownership is explicit and testable.
- `LayerStack` is application-composition machinery, not a rule that every
  subsystem in the engine must become a layer.

## What ships today

- `Event`
  - virtual base with `handled`, `type_id()`, `name()`, `categories()`
  - `EventT<Derived, Categories>` helper for writing new event types
- `EventDispatcher`
  - typed dispatch against a concrete event class
- Built-in events
  - app: `AppTickEvent`, `AppUpdateEvent`, `AppRenderEvent`
  - window: `WindowCloseEvent`, `WindowResizeEvent`
  - input: `KeyPressedEvent`, `KeyReleasedEvent`,
    `MouseButtonPressedEvent`, `MouseButtonReleasedEvent`,
    `MouseMovedEvent`, `MouseScrolledEvent`
- `Layer`
  - `on_attach`, `on_detach`, `on_update`, `on_render`, `on_event`
- `LayerStack`
  - normal layers inserted before overlays
  - update/render iterate forward
  - event propagation iterates reverse
- `Application`
  - owns `PlatformContext`, `Window`, `FrameClock`, `LayerStack`,
    `EventBus`, and owned layers
  - lifts `crd-platform::InputEvent` into typed app events
  - `push_layer(std::unique_ptr<Layer>)`
  - `push_overlay(std::unique_ptr<Layer>)`
  - `run()` and single-step `tick()`
- `EventBus`
  - `subscribe<T>(fn)`
  - `unsubscribe(token)`
  - `publish(event)`
  - custom app-defined event types are supported out of the box
- `smoke_app`
  - opens a real app loop and closes on ESC through the propagated event path

## How to use it

```cpp
class GameLayer final : public crd::app::Layer
{
public:
    explicit GameLayer(crd::app::Application& app) : Layer("GameLayer"), m_app(app) {}

    void on_event(crd::app::Event& event) override
    {
        crd::app::EventDispatcher dispatcher(event);
        (void)dispatcher.dispatch<crd::app::KeyPressedEvent>([this](crd::app::KeyPressedEvent& e) {
            if (e.key() == crd::platform::Key::Escape)
            {
                m_app.close();
                return true;
            }
            return false;
        });
    }

private:
    crd::app::Application& m_app;
};

crd::app::Application app;
app.push_layer(std::make_unique<GameLayer>(app));
app.run();
```

For broadcast-style notifications:

```cpp
class AssetReloadedEvent final : public crd::app::EventT<AssetReloadedEvent,
    static_cast<crd::u32>(crd::app::EventCategory::Application)>
{
public:
    static constexpr crd::containers::StringView kName = "AssetReloadedEvent";
};

auto sub = app.event_bus().subscribe<AssetReloadedEvent>([](AssetReloadedEvent&) {
    // refresh caches
});
```

## Long-term direction

- Event bus may gain an **async** path later, but only once there is a real
  cross-thread producer/consumer need.
- `crd-app` links `crd-jobs` **PUBLIC** (Phase 2.5 wiring, v1k). `Application::run()`
  calls `jobs::init(m_desc.jobs_config)` before the tick loop and `jobs::shutdown()` after.
  `ApplicationDesc` carries a `crd::jobs::Config jobs_config{}` field so callers can tune
  the thread count, fiber pool sizes, and frame arena capacity without touching the loop.
- Graphics, physics, and other engine systems plug in as layers. `crd-app` itself does not
  depend on `crd-rhi`, `crd-rhi-vulkan`, or `crd-renderer`.
