#include <crd/platform/filesystem.hpp>
#include <crd/platform/platform.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}
} // namespace

TEST_CASE("Vulkan instance enumerates at least one adapter", "[rhi][vulkan]")
{
    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);

    crd::containers::Array<crd::rhi::AdapterInfo> adapters;
    instance->enumerate_adapters(adapters);
    REQUIRE(adapters.size() >= 1u);
}

TEST_CASE("Vulkan device bootstrap creates a swapchain for an invisible window", "[rhi][vulkan]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan/window-backed test");
        return;
    }

    auto ctx = crd::platform::PlatformContext::create();
    REQUIRE(ctx.is_valid());

    crd::platform::WindowDesc window_desc;
    window_desc.visible = false;
    window_desc.title = crd::containers::String("crd-rhi-vulkan-tests");
    auto window = crd::platform::Window::create(ctx, window_desc);
    REQUIRE(window.is_valid());

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);

    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    auto swapchain = device->create_swapchain(
        {window.native_handle(), {1280, 720}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});
    REQUIRE(swapchain != nullptr);
    REQUIRE(swapchain->desc().extent.width > 0u);
    REQUIRE(swapchain->desc().extent.height > 0u);
}

TEST_CASE("Vulkan command buffer and frame loop can execute a triangle frame", "[rhi][vulkan]")
{
#if defined(NDEBUG)
    SUCCEED("Release build: triangle frame integration test is skipped due to driver-dependent optimization/runtime "
            "variance");
    return;
#endif

    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping Vulkan/window-backed test");
        return;
    }

    auto ctx = crd::platform::PlatformContext::create();
    REQUIRE(ctx.is_valid());

    crd::platform::WindowDesc window_desc;
    window_desc.visible = false;
    window_desc.title = crd::containers::String("crd-rhi-vulkan-frame-tests");
    auto window = crd::platform::Window::create(ctx, window_desc);
    REQUIRE(window.is_valid());

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    auto swapchain = device->create_swapchain(
        {window.native_handle(), {640, 360}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});
    REQUIRE(swapchain != nullptr);

    const auto shader_dir = fs::executable_dir() / "shaders";
    crd::containers::Array<crd::u8> vs_spv;
    crd::containers::Array<crd::u8> fs_spv;
    REQUIRE(fs::read_file_binary(shader_dir / "triangle.vert.spv", vs_spv));
    REQUIRE(fs::read_file_binary(shader_dir / "triangle.frag.spv", fs_spv));

    auto vs = device->create_shader_module(
        {crd::rhi::ShaderStage::Vertex, "main", crd::containers::make_span(vs_spv.data(), vs_spv.size())});
    auto fs_module = device->create_shader_module(
        {crd::rhi::ShaderStage::Fragment, "main", crd::containers::make_span(fs_spv.data(), fs_spv.size())});
    REQUIRE(vs != nullptr);
    REQUIRE(fs_module != nullptr);

    struct Vertex
    {
        float pos[2];
        float color[3];
    };

    const Vertex vertices[] = {
        {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}}, {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}}, {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}};

    auto vertex_buffer = device->create_buffer(
        {sizeof(vertices), crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex), crd::rhi::MemoryUsage::CpuToGpu});
    REQUIRE(vertex_buffer != nullptr);
    void* mapped = vertex_buffer->map();
    REQUIRE(mapped != nullptr);
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
    REQUIRE(pipeline != nullptr);

    auto command_buffer = device->create_command_buffer();
    REQUIRE(command_buffer != nullptr);

    REQUIRE(swapchain->acquire_next_image());
    command_buffer->begin();
    command_buffer->begin_rendering(
        {{swapchain->desc().extent.width, swapchain->desc().extent.height},
         {&swapchain->current_image(), crd::rhi::LoadOp::Clear, crd::rhi::StoreOp::Store, {0.0f, 0.1f, 0.2f, 1.0f}},
         nullptr});
    command_buffer->bind_pipeline(*pipeline);
    command_buffer->bind_vertex_buffer(*vertex_buffer, 0);
    command_buffer->draw(3, 0);
    command_buffer->end_rendering();
    command_buffer->end();

    REQUIRE(device->graphics_queue().submit(*command_buffer, *swapchain));
    device->wait_idle();
}
