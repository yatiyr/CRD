#pragma once

#include <crd/app/event.hpp>
#include <crd/platform/input.hpp>

namespace crd::app
{
class KeyPressedEvent final : public EventT<KeyPressedEvent, static_cast<crd::u32>(EventCategory::Input) |
                                                                 static_cast<crd::u32>(EventCategory::Keyboard)>
{
public:
    static constexpr crd::containers::StringView kName = "KeyPressedEvent";

    KeyPressedEvent(crd::platform::Key key, crd::platform::KeyMods mods, bool repeated) noexcept
        : m_key(key), m_mods(mods), m_repeated(repeated)
    {
    }

    [[nodiscard]] crd::platform::Key key() const noexcept { return m_key; }
    [[nodiscard]] const crd::platform::KeyMods& mods() const noexcept { return m_mods; }
    [[nodiscard]] bool repeated() const noexcept { return m_repeated; }

private:
    crd::platform::Key m_key = crd::platform::Key::Unknown;
    crd::platform::KeyMods m_mods{};
    bool m_repeated = false;
};

class KeyReleasedEvent final : public EventT<KeyReleasedEvent, static_cast<crd::u32>(EventCategory::Input) |
                                                                   static_cast<crd::u32>(EventCategory::Keyboard)>
{
public:
    static constexpr crd::containers::StringView kName = "KeyReleasedEvent";

    KeyReleasedEvent(crd::platform::Key key, crd::platform::KeyMods mods) noexcept : m_key(key), m_mods(mods) {}

    [[nodiscard]] crd::platform::Key key() const noexcept { return m_key; }
    [[nodiscard]] const crd::platform::KeyMods& mods() const noexcept { return m_mods; }

private:
    crd::platform::Key m_key = crd::platform::Key::Unknown;
    crd::platform::KeyMods m_mods{};
};

class MouseButtonPressedEvent final
    : public EventT<MouseButtonPressedEvent, static_cast<crd::u32>(EventCategory::Input) |
                                                 static_cast<crd::u32>(EventCategory::Mouse) |
                                                 static_cast<crd::u32>(EventCategory::MouseButton)>
{
public:
    static constexpr crd::containers::StringView kName = "MouseButtonPressedEvent";

    MouseButtonPressedEvent(crd::platform::MouseButton button, crd::platform::KeyMods mods) noexcept
        : m_button(button), m_mods(mods)
    {
    }

    [[nodiscard]] crd::platform::MouseButton button() const noexcept { return m_button; }
    [[nodiscard]] const crd::platform::KeyMods& mods() const noexcept { return m_mods; }

private:
    crd::platform::MouseButton m_button = crd::platform::MouseButton::Left;
    crd::platform::KeyMods m_mods{};
};

class MouseButtonReleasedEvent final
    : public EventT<MouseButtonReleasedEvent, static_cast<crd::u32>(EventCategory::Input) |
                                                  static_cast<crd::u32>(EventCategory::Mouse) |
                                                  static_cast<crd::u32>(EventCategory::MouseButton)>
{
public:
    static constexpr crd::containers::StringView kName = "MouseButtonReleasedEvent";

    MouseButtonReleasedEvent(crd::platform::MouseButton button, crd::platform::KeyMods mods) noexcept
        : m_button(button), m_mods(mods)
    {
    }

    [[nodiscard]] crd::platform::MouseButton button() const noexcept { return m_button; }
    [[nodiscard]] const crd::platform::KeyMods& mods() const noexcept { return m_mods; }

private:
    crd::platform::MouseButton m_button = crd::platform::MouseButton::Left;
    crd::platform::KeyMods m_mods{};
};

class MouseMovedEvent final : public EventT<MouseMovedEvent, static_cast<crd::u32>(EventCategory::Input) |
                                                                 static_cast<crd::u32>(EventCategory::Mouse)>
{
public:
    static constexpr crd::containers::StringView kName = "MouseMovedEvent";

    MouseMovedEvent(crd::f32 x, crd::f32 y) noexcept : m_x(x), m_y(y) {}

    [[nodiscard]] crd::f32 x() const noexcept { return m_x; }
    [[nodiscard]] crd::f32 y() const noexcept { return m_y; }

private:
    crd::f32 m_x = 0.0f;
    crd::f32 m_y = 0.0f;
};

class MouseScrolledEvent final : public EventT<MouseScrolledEvent, static_cast<crd::u32>(EventCategory::Input) |
                                                                       static_cast<crd::u32>(EventCategory::Mouse)>
{
public:
    static constexpr crd::containers::StringView kName = "MouseScrolledEvent";

    MouseScrolledEvent(crd::f32 dx, crd::f32 dy) noexcept : m_dx(dx), m_dy(dy) {}

    [[nodiscard]] crd::f32 dx() const noexcept { return m_dx; }
    [[nodiscard]] crd::f32 dy() const noexcept { return m_dy; }

private:
    crd::f32 m_dx = 0.0f;
    crd::f32 m_dy = 0.0f;
};
} // namespace crd::app
