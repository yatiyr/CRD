#include "sandbox_layer.hpp"

#include <crd/app/app.hpp>
#include <crd/config/config.hpp>
#include <crd/imgui/imgui.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <cstdio>
#include <cstring>
#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_sandbox, "Sandbox", crd::log::LogLevel::Trace)

namespace fs = crd::platform::fs;

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered so log lines survive a crash

    bool headless = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--headless") == 0)
        {
            headless = true;
        }
    }

    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    crd::app::ApplicationDesc app_desc;
    app_desc.window.title = crd::containers::String("Cerid Sandbox");
    app_desc.window.size  = {1280, 720};

    crd::app::Application app(app_desc);
    if (!app.is_valid())
    {
        CRD_LOG_ERROR(g_log_sandbox, "Application init failed");
        crd::log::shutdown();
        return 1;
    }

    auto instance = crd::rhi::create_vulkan_instance({.enable_validation = !headless});
    auto device   = instance->create_device({});
    auto swapchain = device->create_swapchain({
        .native_window_handle = app.window().native_handle(),
        .extent               = {1280, 720},
        .color_format         = crd::rhi::Format::B8G8R8A8Unorm,
        .present_mode         = crd::rhi::PresentMode::Fifo,
        .image_count          = 2,
    });

    if (device == nullptr || swapchain == nullptr)
    {
        CRD_LOG_ERROR(g_log_sandbox, "RHI bootstrap failed — Vulkan device or swapchain unavailable");
        crd::log::flush();
        crd::log::shutdown();
        return 1;
    }

    constexpr crd::u32 kFramesInFlight = 2;
    crd::containers::Array<std::unique_ptr<crd::rhi::CommandBuffer>> cmds;
    for (crd::u32 i = 0; i < kFramesInFlight; ++i)
    {
        cmds.push_back(device->create_command_buffer());
    }

    // Load ImGui config (use defaults if file is absent).
    crd::config::Config imgui_config;
    const auto config_path = fs::executable_dir() / "configs" / "imgui_layer.toml";
    if (!imgui_config.load_from_file(config_path))
    {
        CRD_LOG_WARN(g_log_sandbox, "imgui_layer.toml not found — using defaults");
    }

    // SandboxLayer now owns the ForwardRenderPath and shader compilation.
    auto* sandbox_layer = [&]() -> crd::sandbox::SandboxLayer*
    {
        auto layer = std::make_unique<crd::sandbox::SandboxLayer>(app, *device, *swapchain);
        auto* ptr  = layer.get();
        app.push_layer(std::move(layer));
        return ptr;
    }();

    auto* imgui = [&]() -> crd::imgui::ImGuiLayer*
    {
        auto layer = std::make_unique<crd::imgui::ImGuiLayer>(app, *instance, *device, *swapchain, imgui_config);
        auto* ptr  = layer.get();
        app.push_overlay(std::move(layer));
        return ptr;
    }();

    CRD_LOG_INFO(g_log_sandbox, "Sandbox started (headless={})", headless);

    crd::jobs::init(app_desc.jobs_config);

    crd::u32 frame = 0;
    while (app.is_running())
    {
        if (!app.tick())
        {
            break;
        }

        if (!swapchain->acquire_next_image())
        {
            continue; // swapchain out-of-date or minimized — on_event will resize before next acquire
        }

        auto& cmd = *cmds[frame % kFramesInFlight];
        cmd.begin();

        // 3D scene: ForwardRenderPath → blit to swapchain (leaves it in ColorWrite).
        sandbox_layer->render_scene(cmd, swapchain->current_image(), frame);

        // ImGui renders on top of the 3D output.
        cmd.begin_rendering({
            .extent           = {swapchain->desc().extent.width, swapchain->desc().extent.height},
            .color_attachment = {.image     = &swapchain->current_image(),
                                 .load_op   = crd::rhi::LoadOp::Load,
                                 .store_op  = crd::rhi::StoreOp::Store,
                                 .clear_color = {}},
        });
        imgui->render(cmd);
        cmd.end_rendering();

        cmd.transition_image(swapchain->current_image(),
                             crd::rhi::ImageAccess::ColorWrite, crd::rhi::ImageAccess::Present);
        cmd.end();

        if (device->graphics_queue().submit(cmd, *swapchain))
        {
            device->graphics_queue().present(*swapchain);
        }

        ++frame;

        if (headless && frame >= 1)
        {
            CRD_LOG_INFO(g_log_sandbox, "Headless: exiting after {} frame(s)", frame);
            app.close();
        }

        if (app.window().input().state().was_key_pressed(crd::platform::Key::Escape))
        {
            app.close();
        }
    }

    // Shutdown order matters:
    //   1. device->wait_idle() — drain in-flight GPU work before any GPU
    //      resource held by a layer is freed.
    //   2. detach_all_layers()  — runs each layer's destructor, which
    //      MUST `wait_ready()` every ResourceHandle it still holds
    //      (claims + reaps the per-load Counter so the CounterPool
    //      drops to zero acquired entries). After detach, the
    //      ResourceManager is destroyed with no in-flight loads.
    //   3. jobs::shutdown()     — final drain of the job system; the
    //      CounterPool's shutdown assert (m_acquired == 0) only holds
    //      because step 2 reaped all per-load Counters first.
    device->wait_idle();
    app.detach_all_layers();
    crd::jobs::shutdown();

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
