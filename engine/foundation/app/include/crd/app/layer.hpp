#pragma once

#include <crd/app/event.hpp>
#include <crd/containers/string.hpp>

namespace crd::app
{
class Layer
{
public:
    explicit Layer(crd::containers::StringView name) : m_name(name) {}
    virtual ~Layer() = default;

    virtual void on_attach() {}
    virtual void on_detach() {}
    virtual void on_frame_begin() {}
    virtual void on_update(crd::f64 /*delta_seconds*/) {}
    virtual void on_render() {}
    virtual void on_event(Event& /*event*/) {}

    [[nodiscard]] crd::containers::StringView name() const noexcept { return m_name; }

private:
    crd::containers::String m_name{};
};
} // namespace crd::app
