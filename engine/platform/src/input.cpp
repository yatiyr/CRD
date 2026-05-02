#include <crd/platform/input.hpp>

namespace crd::platform
{
Input::Input() noexcept = default;

void Input::enable_event_queue(crd::usize capacity_pow2)
{
    m_queue.emplace(capacity_pow2);
}

bool Input::try_pop_event(InputEvent& out) noexcept
{
    if (!m_queue.has_value())
    {
        return false;
    }
    return m_queue->try_pop(out);
}

void Input::on_poll_begin() noexcept
{
    // Edge state lives for exactly one frame.
    m_state.m_key_pressed.fill(false);
    m_state.m_key_released.fill(false);
    m_state.m_mouse_pressed.fill(false);
    m_state.m_mouse_released.fill(false);
    m_state.m_mouse_dx = 0.0F;
    m_state.m_mouse_dy = 0.0F;
    m_state.m_scroll_dx = 0.0F;
    m_state.m_scroll_dy = 0.0F;
}

void Input::enqueue(const InputEvent& evt) noexcept
{
    if (m_queue.has_value())
    {
        // try_push returns false if full; we drop on overflow rather than
        // overwrite. The opt-in queue is sized by the caller.
        (void)m_queue->try_push(evt);
    }
}

void Input::push_key_event(Key k, InputEvent::Type type, KeyMods mods) noexcept
{
    const auto idx = static_cast<crd::usize>(k);
    if (idx == 0 || idx >= static_cast<crd::usize>(Key::Count))
    {
        return; // Unknown / out of range
    }

    m_state.m_mods = mods;

    if (type == InputEvent::Type::KeyDown)
    {
        if (!m_state.m_key_down[idx])
        {
            m_state.m_key_pressed[idx] = true;
        }
        m_state.m_key_down[idx] = true;
    }
    else if (type == InputEvent::Type::KeyUp)
    {
        if (m_state.m_key_down[idx])
        {
            m_state.m_key_released[idx] = true;
        }
        m_state.m_key_down[idx] = false;
    }
    // KeyRepeat: no state mutation, just an event.

    InputEvent evt;
    evt.type = type;
    evt.mods = mods;
    evt.payload.key.key = k;
    enqueue(evt);
}

void Input::push_mouse_button_event(MouseButton b, InputEvent::Type type, KeyMods mods) noexcept
{
    const auto idx = static_cast<crd::usize>(b);
    if (idx >= static_cast<crd::usize>(MouseButton::Count))
    {
        return;
    }

    m_state.m_mods = mods;

    if (type == InputEvent::Type::MouseDown)
    {
        if (!m_state.m_mouse_down[idx])
        {
            m_state.m_mouse_pressed[idx] = true;
        }
        m_state.m_mouse_down[idx] = true;
    }
    else if (type == InputEvent::Type::MouseUp)
    {
        if (m_state.m_mouse_down[idx])
        {
            m_state.m_mouse_released[idx] = true;
        }
        m_state.m_mouse_down[idx] = false;
    }

    InputEvent evt;
    evt.type = type;
    evt.mods = mods;
    evt.payload.mouse_button.button = b;
    enqueue(evt);
}

void Input::push_mouse_move(crd::f32 x, crd::f32 y) noexcept
{
    if (m_state.m_mouse_seeded)
    {
        m_state.m_mouse_dx += x - m_state.m_mouse_x;
        m_state.m_mouse_dy += y - m_state.m_mouse_y;
    }
    else
    {
        // First mouse position — don't synthesize a giant delta from
        // (0, 0) to wherever the cursor happened to enter the window.
        m_state.m_mouse_seeded = true;
    }
    m_state.m_mouse_x = x;
    m_state.m_mouse_y = y;

    InputEvent evt;
    evt.type = InputEvent::Type::MouseMove;
    evt.payload.mouse_move.x = x;
    evt.payload.mouse_move.y = y;
    enqueue(evt);
}

void Input::push_scroll(crd::f32 dx, crd::f32 dy) noexcept
{
    m_state.m_scroll_dx += dx;
    m_state.m_scroll_dy += dy;

    InputEvent evt;
    evt.type = InputEvent::Type::Scroll;
    evt.payload.scroll.dx = dx;
    evt.payload.scroll.dy = dy;
    enqueue(evt);
}

void Input::push_resize(crd::i32 width, crd::i32 height) noexcept
{
    InputEvent evt;
    evt.type = InputEvent::Type::Resize;
    evt.payload.resize.width = width;
    evt.payload.resize.height = height;
    enqueue(evt);
}
} // namespace crd::platform
