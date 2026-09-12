#pragma once

#include <crd/app/event.hpp>

namespace crd::app
{
class WindowCloseEvent final : public EventT<WindowCloseEvent, static_cast<crd::u32>(EventCategory::Application) |
                                                                   static_cast<crd::u32>(EventCategory::Window)>
{
public:
    static constexpr crd::containers::StringView kName = "WindowCloseEvent";
};

class WindowResizeEvent final : public EventT<WindowResizeEvent, static_cast<crd::u32>(EventCategory::Application) |
                                                                     static_cast<crd::u32>(EventCategory::Window)>
{
public:
    static constexpr crd::containers::StringView kName = "WindowResizeEvent";

    WindowResizeEvent(crd::i32 width, crd::i32 height) noexcept : m_width(width), m_height(height) {}

    [[nodiscard]] crd::i32 width() const noexcept { return m_width; }
    [[nodiscard]] crd::i32 height() const noexcept { return m_height; }

private:
    crd::i32 m_width = 0;
    crd::i32 m_height = 0;
};
} // namespace crd::app
