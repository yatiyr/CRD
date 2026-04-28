#include <crd/log/log.hpp>
#include <crd/platform/platform.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_rhi_vk, "SmokeRHIVulkan", crd::log::LogLevel::Trace)

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    auto context = crd::platform::PlatformContext::create();
    if (!context.is_valid())
    {
        CRD_LOG_ERROR(g_log_smoke_rhi_vk, "PlatformContext init failed");
        return 1;
    }

    crd::platform::WindowDesc window_desc;
    window_desc.title = crd::containers::String("Cerid - smoke_rhi_vulkan_bootstrap");
    auto window = crd::platform::Window::create(context, window_desc);
    if (!window.is_valid())
    {
        CRD_LOG_ERROR(g_log_smoke_rhi_vk, "Window creation failed");
        return 2;
    }

    auto instance = crd::rhi::create_vulkan_instance({});
    crd::containers::Array<crd::rhi::AdapterInfo> adapters;
    instance->enumerate_adapters(adapters);
    for (const auto& adapter : adapters)
    {
        CRD_LOG_INFO(g_log_smoke_rhi_vk, "Adapter: {}", adapter.name.c_str());
    }

    auto device = instance->create_device({});
    auto swapchain = device->create_swapchain(
        {window.native_handle(), {1280, 720}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});
    CRD_LOG_INFO(g_log_smoke_rhi_vk, "Swapchain bootstrap OK: {}x{}", swapchain->desc().extent.width,
                 swapchain->desc().extent.height);

    while (!window.should_close())
    {
        window.poll_input();
        context.poll_events();
        if (window.input().state().was_key_pressed(crd::platform::Key::Escape))
        {
            window.request_close();
        }
    }

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
