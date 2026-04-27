#pragma once

#include <crd/app/event.hpp>

namespace crd::app
{
class AppTickEvent final : public EventT<AppTickEvent, static_cast<crd::u32>(EventCategory::Application)>
{
public:
    static constexpr crd::containers::StringView kName = "AppTickEvent";
};

class AppUpdateEvent final : public EventT<AppUpdateEvent, static_cast<crd::u32>(EventCategory::Application)>
{
public:
    static constexpr crd::containers::StringView kName = "AppUpdateEvent";

    explicit AppUpdateEvent(crd::f64 delta_seconds) noexcept : m_delta_seconds(delta_seconds) {}
    [[nodiscard]] crd::f64 delta_seconds() const noexcept { return m_delta_seconds; }

private:
    crd::f64 m_delta_seconds = 0.0;
};

class AppRenderEvent final : public EventT<AppRenderEvent, static_cast<crd::u32>(EventCategory::Application)>
{
public:
    static constexpr crd::containers::StringView kName = "AppRenderEvent";
};
} // namespace crd::app
