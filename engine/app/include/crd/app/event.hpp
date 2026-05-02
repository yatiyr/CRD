#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::app
{
using EventTypeId = const void*;

enum class EventCategory : crd::u32
{
    None = 0,
    Application = 1U << 0U,
    Input = 1U << 1U,
    Keyboard = 1U << 2U,
    Mouse = 1U << 3U,
    MouseButton = 1U << 4U,
    Window = 1U << 5U,
};

[[nodiscard]] constexpr crd::u32 operator|(EventCategory a, EventCategory b) noexcept
{
    return static_cast<crd::u32>(a) | static_cast<crd::u32>(b);
}

class Event
{
public:
    virtual ~Event() = default;

    [[nodiscard]] virtual EventTypeId type_id() const noexcept = 0;
    [[nodiscard]] virtual containers::StringView name() const noexcept = 0;
    [[nodiscard]] virtual crd::u32 categories() const noexcept = 0;

    [[nodiscard]] bool is_in_category(EventCategory category) const noexcept
    {
        return (categories() & static_cast<crd::u32>(category)) != 0U;
    }

    bool handled = false;
};

template <typename Derived, crd::u32 CategoriesV> class EventT : public Event
{
public:
    [[nodiscard]] static EventTypeId static_type_id() noexcept
    {
        static const int token = 0;
        return &token;
    }

    [[nodiscard]] EventTypeId type_id() const noexcept final { return static_type_id(); }
    [[nodiscard]] containers::StringView name() const noexcept final { return Derived::kName; }
    [[nodiscard]] crd::u32 categories() const noexcept final { return CategoriesV; }
};
} // namespace crd::app
