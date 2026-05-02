#include <crd/platform/input.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::platform;

TEST_CASE("InputState: default-initialised has nothing held", "[platform][input][state]")
{
    Input in;
    const InputState& s = in.state();
    REQUIRE_FALSE(s.is_key_down(Key::W));
    REQUIRE_FALSE(s.was_key_pressed(Key::W));
    REQUIRE_FALSE(s.was_key_released(Key::W));
    REQUIRE_FALSE(s.is_mouse_down(MouseButton::Left));
    REQUIRE(s.mouse_x() == 0.0F);
    REQUIRE(s.mouse_y() == 0.0F);
    REQUIRE(s.mouse_dx() == 0.0F);
    REQUIRE(s.mouse_dy() == 0.0F);
}

TEST_CASE("Input: key down sets pressed for one frame, then sticky-down", "[platform][input][state]")
{
    Input in;
    in.push_key_event(Key::W, InputEvent::Type::KeyDown, KeyMods{});

    REQUIRE(in.state().is_key_down(Key::W));
    REQUIRE(in.state().was_key_pressed(Key::W));
    REQUIRE_FALSE(in.state().was_key_released(Key::W));

    // Next frame: pressed/released cleared, down stays.
    in.on_poll_begin();
    REQUIRE(in.state().is_key_down(Key::W));
    REQUIRE_FALSE(in.state().was_key_pressed(Key::W));
    REQUIRE_FALSE(in.state().was_key_released(Key::W));
}

TEST_CASE("Input: key up clears down, sets released for one frame", "[platform][input][state]")
{
    Input in;
    in.push_key_event(Key::A, InputEvent::Type::KeyDown, KeyMods{});
    in.on_poll_begin();
    REQUIRE(in.state().is_key_down(Key::A));

    in.push_key_event(Key::A, InputEvent::Type::KeyUp, KeyMods{});
    REQUIRE_FALSE(in.state().is_key_down(Key::A));
    REQUIRE(in.state().was_key_released(Key::A));

    in.on_poll_begin();
    REQUIRE_FALSE(in.state().was_key_released(Key::A));
}

TEST_CASE("Input: repeated KeyDown does not re-trigger pressed-this-frame", "[platform][input][state]")
{
    Input in;
    in.push_key_event(Key::Space, InputEvent::Type::KeyDown, KeyMods{});
    in.on_poll_begin();
    REQUIRE(in.state().is_key_down(Key::Space));
    REQUIRE_FALSE(in.state().was_key_pressed(Key::Space));

    in.push_key_event(Key::Space, InputEvent::Type::KeyDown, KeyMods{});
    REQUIRE(in.state().is_key_down(Key::Space));
    // No transition: was already down -> pressed-this-frame stays false.
    REQUIRE_FALSE(in.state().was_key_pressed(Key::Space));
}

TEST_CASE("Input: Key::Unknown is silently dropped", "[platform][input][state]")
{
    Input in;
    in.push_key_event(Key::Unknown, InputEvent::Type::KeyDown, KeyMods{});
    REQUIRE_FALSE(in.state().is_key_down(Key::Unknown));
}

TEST_CASE("Input: mouse button transitions mirror keys", "[platform][input][state]")
{
    Input in;
    in.push_mouse_button_event(MouseButton::Left, InputEvent::Type::MouseDown, KeyMods{});
    REQUIRE(in.state().is_mouse_down(MouseButton::Left));
    REQUIRE(in.state().was_mouse_pressed(MouseButton::Left));

    in.on_poll_begin();
    REQUIRE(in.state().is_mouse_down(MouseButton::Left));
    REQUIRE_FALSE(in.state().was_mouse_pressed(MouseButton::Left));

    in.push_mouse_button_event(MouseButton::Left, InputEvent::Type::MouseUp, KeyMods{});
    REQUIRE_FALSE(in.state().is_mouse_down(MouseButton::Left));
    REQUIRE(in.state().was_mouse_released(MouseButton::Left));
}

TEST_CASE("Input: mouse first move seeds without spurious delta", "[platform][input][state]")
{
    Input in;
    in.push_mouse_move(100.0F, 200.0F);
    REQUIRE(in.state().mouse_x() == 100.0F);
    REQUIRE(in.state().mouse_y() == 200.0F);
    REQUIRE(in.state().mouse_dx() == 0.0F);
    REQUIRE(in.state().mouse_dy() == 0.0F);

    in.push_mouse_move(105.0F, 195.0F);
    REQUIRE(in.state().mouse_x() == 105.0F);
    REQUIRE(in.state().mouse_y() == 195.0F);
    REQUIRE(in.state().mouse_dx() == 5.0F);
    REQUIRE(in.state().mouse_dy() == -5.0F);
}

TEST_CASE("Input: mouse delta resets on poll_begin", "[platform][input][state]")
{
    Input in;
    in.push_mouse_move(0.0F, 0.0F);   // seed
    in.push_mouse_move(10.0F, 20.0F); // dx=10, dy=20
    REQUIRE(in.state().mouse_dx() == 10.0F);

    in.on_poll_begin();
    REQUIRE(in.state().mouse_dx() == 0.0F);
    REQUIRE(in.state().mouse_dy() == 0.0F);
    // Position is preserved across frames.
    REQUIRE(in.state().mouse_x() == 10.0F);
}

TEST_CASE("Input: scroll accumulates within a frame and resets on poll", "[platform][input][state]")
{
    Input in;
    in.push_scroll(0.0F, 1.0F);
    in.push_scroll(0.0F, 2.0F);
    REQUIRE(in.state().scroll_dy() == 3.0F);

    in.on_poll_begin();
    REQUIRE(in.state().scroll_dy() == 0.0F);
}

TEST_CASE("Input: event queue is opt-in and FIFO ordered", "[platform][input][queue]")
{
    Input in;
    REQUIRE_FALSE(in.event_queue_enabled());

    InputEvent dummy;
    REQUIRE_FALSE(in.try_pop_event(dummy));

    in.enable_event_queue(8);
    REQUIRE(in.event_queue_enabled());

    in.push_key_event(Key::A, InputEvent::Type::KeyDown, KeyMods{});
    in.push_key_event(Key::B, InputEvent::Type::KeyDown, KeyMods{});

    InputEvent e1{};
    REQUIRE(in.try_pop_event(e1));
    REQUIRE(e1.type == InputEvent::Type::KeyDown);
    REQUIRE(e1.payload.key.key == Key::A);

    InputEvent e2{};
    REQUIRE(in.try_pop_event(e2));
    REQUIRE(e2.payload.key.key == Key::B);

    InputEvent e3{};
    REQUIRE_FALSE(in.try_pop_event(e3));
}

TEST_CASE("Input: events without queue are silently dropped", "[platform][input][queue]")
{
    Input in;
    in.push_key_event(Key::A, InputEvent::Type::KeyDown, KeyMods{});
    // State still updates, queue not allocated, no crash.
    REQUIRE(in.state().is_key_down(Key::A));
    REQUIRE_FALSE(in.event_queue_enabled());
}

TEST_CASE("Input: mods are propagated to state", "[platform][input][state]")
{
    Input in;
    KeyMods m;
    m.shift = true;
    m.ctrl = true;
    in.push_key_event(Key::S, InputEvent::Type::KeyDown, m);
    REQUIRE(in.state().mods().shift);
    REQUIRE(in.state().mods().ctrl);
    REQUIRE_FALSE(in.state().mods().alt);
}
