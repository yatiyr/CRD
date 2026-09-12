#pragma once

#include <crd/app/event_bus.hpp>
#include <crd/app/layer_stack.hpp>
#include <crd/core/crash.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/platform/platform.hpp>

#include <memory>

namespace crd::app
{
struct ApplicationDesc
{
    crd::platform::WindowDesc window{};
    crd::usize                platform_event_queue_capacity = 128;
    crd::jobs::Config         jobs_config{};
    const char*               crash_dir             = "./crashes";
    bool                      install_crash_handler = true;
};

class Application
{
public:
    explicit Application(const ApplicationDesc& desc = ApplicationDesc{});
    ~Application() noexcept;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] bool is_valid() const noexcept { return m_valid; }
    [[nodiscard]] bool is_running() const noexcept { return m_running; }

    void run();
    [[nodiscard]] bool tick();
    void close() noexcept;
    void detach_layer(Layer* layer);
    void detach_all_layers();
    void push_layer(std::unique_ptr<Layer> layer);
    void push_overlay(std::unique_ptr<Layer> overlay);

    [[nodiscard]] EventBus& event_bus() noexcept { return m_event_bus; }
    [[nodiscard]] const EventBus& event_bus() const noexcept { return m_event_bus; }
    [[nodiscard]] LayerStack& layers() noexcept { return m_layer_stack; }
    [[nodiscard]] const LayerStack& layers() const noexcept { return m_layer_stack; }
    [[nodiscard]] crd::platform::Window& window() noexcept
    {
        CRD_ASSERT(m_window != nullptr);
        return *m_window;
    }
    [[nodiscard]] const crd::platform::Window& window() const noexcept
    {
        CRD_ASSERT(m_window != nullptr);
        return *m_window;
    }
    [[nodiscard]] crd::platform::FrameClock& clock() noexcept { return m_clock; }
    [[nodiscard]] const crd::platform::FrameClock& clock() const noexcept { return m_clock; }

private:
    void dispatch_propagated(Event& event);
    void dispatch_platform_events();

    ApplicationDesc m_desc{};
    crd::platform::PlatformContext m_context{};
    std::unique_ptr<crd::platform::Window> m_window{};
    crd::platform::FrameClock m_clock{};
    EventBus m_event_bus{};
    LayerStack m_layer_stack{};
    crd::containers::Array<std::unique_ptr<Layer>> m_owned_layers{};
    bool m_valid = false;
    bool m_running = false;
};
} // namespace crd::app
