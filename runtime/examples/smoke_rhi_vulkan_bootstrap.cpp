#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/platform.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <cstring>
#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_rhi_vk, "SmokeRHIVulkan", crd::log::LogLevel::Trace)

namespace fs = crd::platform::fs;

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

    crd::rhi::DeviceDesc device_desc;
    auto device = instance->create_device(device_desc);
    auto swapchain = device->create_swapchain(
        {window.native_handle(), {1280, 720}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> vs_spv;
    crd::containers::Array<crd::u8> fs_spv;
    if (!fs::read_file_binary(shader_dir / "triangle.vert.spv", vs_spv) ||
        !fs::read_file_binary(shader_dir / "triangle.frag.spv", fs_spv))
    {
        CRD_LOG_ERROR(g_log_smoke_rhi_vk, "Failed to load compiled shader binaries from {}",
                      shader_dir.generic().data());
        return 3;
    }

    auto vs = device->create_shader_module(
        {crd::rhi::ShaderStage::Vertex, "main", crd::containers::make_span(vs_spv.data(), vs_spv.size())});
    auto fs_module = device->create_shader_module(
        {crd::rhi::ShaderStage::Fragment, "main", crd::containers::make_span(fs_spv.data(), fs_spv.size())});

    struct Vertex
    {
        float pos[2];
        float color[3];
    };

    const Vertex vertices[] = {
        {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}}, {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}}, {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}};

    auto vertex_buffer = device->create_buffer(
        {sizeof(vertices), crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    if (vs == nullptr || fs_module == nullptr || vertex_buffer == nullptr)
    {
        CRD_LOG_ERROR(g_log_smoke_rhi_vk, "Triangle resource bootstrap failed");
        return 3;
    }
    void* mapped = vertex_buffer->map();
    if (mapped == nullptr)
    {
        CRD_LOG_ERROR(g_log_smoke_rhi_vk, "Vertex buffer map failed");
        return 3;
    }
    std::memcpy(mapped, vertices, sizeof(vertices));
    vertex_buffer->unmap();

    const crd::rhi::VertexBindingDesc binding{0, sizeof(Vertex), crd::rhi::VertexInputRate::Vertex};
    const crd::rhi::VertexAttributeDesc attributes[] = {{0, 0, crd::rhi::Format::R32G32Sfloat, 0},
                                                        {1, 0, crd::rhi::Format::R32G32B32Sfloat, sizeof(float) * 2u}};
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
    for (crd::u32 i = 0; i < device_desc.frames_in_flight; ++i)
    {
        command_buffers.push_back(device->create_command_buffer());
    }

    if (swapchain == nullptr || pipeline == nullptr || command_buffers.size() != device_desc.frames_in_flight)
    {
        CRD_LOG_ERROR(g_log_smoke_rhi_vk, "Frame path bootstrap failed");
        return 3;
    }
    for (const auto& command_buffer : command_buffers)
    {
        if (command_buffer == nullptr)
        {
            CRD_LOG_ERROR(g_log_smoke_rhi_vk, "Command buffer allocation failed");
            return 3;
        }
    }

    CRD_LOG_INFO(g_log_smoke_rhi_vk, "Swapchain bootstrap OK: {}x{}", swapchain->desc().extent.width,
                 swapchain->desc().extent.height);

    crd::u32 frame_count = 0;
    while (!window.should_close())
    {
        window.poll_input();
        context.poll_events();

        if (swapchain->acquire_next_image())
        {
            auto& command_buffer = command_buffers[frame_count % command_buffers.size()];
            command_buffer->begin();
            command_buffer->begin_rendering({{swapchain->desc().extent.width, swapchain->desc().extent.height},
                                             {&swapchain->current_image(),
                                              crd::rhi::LoadOp::Clear,
                                              crd::rhi::StoreOp::Store,
                                              {0.07f, 0.08f, 0.12f, 1.0f}},
                                             nullptr});
            command_buffer->bind_pipeline(*pipeline);
            command_buffer->bind_vertex_buffer(*vertex_buffer, 0);
            command_buffer->draw(3, 0);
            command_buffer->end_rendering();
            command_buffer->end();
            if (device->graphics_queue().submit(*command_buffer, *swapchain))
            {
                device->graphics_queue().present(*swapchain);
            }
        }

        ++frame_count;
        if (frame_count >= 120)
        {
            CRD_LOG_INFO(g_log_smoke_rhi_vk, "Smoke complete after {} frames", frame_count);
            window.request_close();
        }

        if (window.input().state().was_key_pressed(crd::platform::Key::Escape))
        {
            window.request_close();
        }
    }

    device->wait_idle();

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
