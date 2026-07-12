#include <crd/app/app.hpp>
#include <crd/config/config.hpp>
#include <crd/gpu/program.hpp>       // D-008: opaque IGpuProgram
#include <crd/gpu/vulkan_context.hpp> // D-008 C2-f: create_vulkan_gpu_context (rhi adopts one device)
#include <crd/imgui/imgui.hpp>
#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <cstring>
#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_imgui, "SmokeImGui", crd::log::LogLevel::Trace)

namespace fs = crd::platform::fs;

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    crd::app::ApplicationDesc app_desc;
    app_desc.window.title = crd::containers::String("Cerid - smoke_imgui_overlay");
    crd::app::Application app(app_desc);
    if (!app.is_valid())
    {
        return 1;
    }

    crd::config::Config config;
    const auto config_path = fs::executable_dir() / "configs" / "imgui_layer.toml";
    if (!config.load_from_file(config_path))
    {
        CRD_LOG_ERROR(g_log_smoke_imgui, "Failed to load imgui config from '{}'", config_path.generic().data());
        return 2;
    }

    // D-008 C2-f: rhi adopts a device from a windowed gpu-context (gpu_ctx outlives device).
    crd::gpu::GpuContextConfig gpu_cfg;
    gpu_cfg.headless = false;
    auto gpu_ctx   = crd::gpu::create_vulkan_gpu_context(gpu_cfg);
    auto device    = gpu_ctx ? crd::rhi::create_vulkan_device_adopting(*gpu_ctx) : nullptr;
    auto swapchain = device ? device->create_swapchain(
        {app.window().native_handle(), {1280, 720}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2})
                            : nullptr;
    if (gpu_ctx == nullptr || device == nullptr || swapchain == nullptr)
    {
        CRD_LOG_ERROR(g_log_smoke_imgui, "RHI bootstrap failed");
        return 3;
    }

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> vs_spv;
    crd::containers::Array<crd::u8> fs_spv;
    if (!fs::read_file_binary(shader_dir / "triangle.vert.spv", vs_spv) ||
        !fs::read_file_binary(shader_dir / "triangle.frag.spv", fs_spv))
    {
        CRD_LOG_ERROR(g_log_smoke_imgui, "Failed to load shaders");
        return 4;
    }

    auto vs = device->create_program(crd::rhi::ShaderStage::Vertex,
                                     crd::containers::make_span(vs_spv.data(), vs_spv.size()));
    auto fs_module = device->create_program(crd::rhi::ShaderStage::Fragment,
                                            crd::containers::make_span(fs_spv.data(), fs_spv.size()));

    struct Vertex
    {
        float pos[2];
        float color[3];
    };
    const Vertex vertices[] = {
        {{0.0F, -0.5F}, {1.0F, 0.0F, 0.0F}}, {{0.5F, 0.5F}, {0.0F, 1.0F, 0.0F}}, {{-0.5F, 0.5F}, {0.0F, 0.0F, 1.0F}}};
    auto vertex_buffer = device->create_buffer(
        {sizeof(vertices), crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    if (vs == nullptr || fs_module == nullptr || vertex_buffer == nullptr)
    {
        return 5;
    }
    void* mapped = vertex_buffer->map();
    if (mapped == nullptr)
    {
        return 6;
    }
    std::memcpy(mapped, vertices, sizeof(vertices));
    vertex_buffer->unmap();

    const crd::rhi::VertexBindingDesc binding{0, sizeof(Vertex), crd::rhi::VertexInputRate::Vertex};
    const crd::rhi::VertexAttributeDesc attributes[] = {{0, 0, crd::rhi::Format::R32G32Sfloat, 0},
                                                        {1, 0, crd::rhi::Format::R32G32B32Sfloat, sizeof(float) * 2U}};
    auto pipeline = device->create_graphics_pipeline({vs.get(),
                                                      fs_module.get(),
                                                      crd::rhi::PrimitiveTopology::TriangleList,
                                                      {swapchain->desc().extent.width, swapchain->desc().extent.height},
                                                      swapchain->desc().color_format,
                                                      crd::rhi::Format::Undefined,
                                                      crd::containers::make_span(&binding, 1),
                                                      crd::containers::make_span(attributes),
                                                      false,
                                                      false});

    crd::containers::Array<std::unique_ptr<crd::rhi::CommandBuffer>> command_buffers;
    for (crd::u32 i = 0; i < 2; ++i)
    {
        command_buffers.push_back(device->create_command_buffer());
    }

    auto imgui_layer = std::make_unique<crd::imgui::ImGuiLayer>(app, *device, *swapchain, config);
    auto* imgui = imgui_layer.get();
    app.push_overlay(std::move(imgui_layer));

    crd::u32 frame = 0;
    while (app.is_running())
    {
        if (!app.tick())
        {
            break;
        }

        if (!swapchain->acquire_next_image())
        {
            break;
        }

        auto& command_buffer = command_buffers[frame % command_buffers.size()];
        command_buffer->begin();
        command_buffer->transition_image(swapchain->current_image(),
                                         crd::rhi::ImageAccess::Undefined, crd::rhi::ImageAccess::ColorWrite);
        command_buffer->begin_rendering({{swapchain->desc().extent.width, swapchain->desc().extent.height},
                                         {&swapchain->current_image(),
                                          crd::rhi::LoadOp::Clear,
                                          crd::rhi::StoreOp::Store,
                                          {0.07F, 0.08F, 0.12F, 1.0F}},
                                         nullptr});
        command_buffer->bind_pipeline(*pipeline);
        command_buffer->bind_vertex_buffer(*vertex_buffer, 0);
        command_buffer->draw(3, 0);
        imgui->render(*command_buffer);
        command_buffer->end_rendering();
        command_buffer->transition_image(swapchain->current_image(),
                                         crd::rhi::ImageAccess::ColorWrite, crd::rhi::ImageAccess::Present);
        command_buffer->end();

        if (device->graphics_queue().submit(*command_buffer, *swapchain))
        {
            device->graphics_queue().present(*swapchain);
        }

        ++frame;
        if (frame >= 720)
        {
            app.close();
        }
    }

    device->wait_idle();
    app.detach_all_layers();
    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
