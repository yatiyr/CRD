// smoke_window — opens a 1280x720 Vulkan-ready GLFW window through
// crd-platform, pumps the event loop until the user closes it. Logs
// platform events through the engine's logger.

#include <crd/log/log.hpp>
#include <crd/platform/platform.hpp>

#include <memory>

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());
    crd::log::set_all_channels_level(crd::log::LogLevel::Trace);

    auto context = crd::platform::PlatformContext::create();
    if (!context.is_valid())
    {
        CRD_LOG_CRITICAL(crd::platform::g_log_platform, "Failed to initialise platform context");
        crd::log::shutdown();
        return 1;
    }

    crd::platform::WindowDesc desc;
    desc.size = {1280, 720};
    desc.title = crd::containers::String("Cerid - smoke_window");

    auto window = crd::platform::Window::create(context, desc);
    if (!window.is_valid())
    {
        CRD_LOG_CRITICAL(crd::platform::g_log_platform, "Failed to create window");
        crd::log::shutdown();
        return 2;
    }

    const auto fb = window.framebuffer_size();
    CRD_LOG_INFO(crd::platform::g_log_platform, "Initial framebuffer size: {}x{}", fb.width, fb.height);

    while (!window.should_close())
    {
        context.poll_events();
    }

    CRD_LOG_INFO(crd::platform::g_log_platform, "Window closed by user, exiting");
    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
