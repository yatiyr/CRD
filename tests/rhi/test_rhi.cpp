#include <crd/rhi/rhi.hpp>

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace
{
class FakeImage final : public crd::rhi::Image
{
public:
    explicit FakeImage(crd::rhi::ImageDesc desc) : m_desc(std::move(desc)) {}
    [[nodiscard]] const crd::rhi::ImageDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::ImageDesc m_desc{};
};

class FakeBuffer final : public crd::rhi::Buffer
{
public:
    explicit FakeBuffer(crd::rhi::BufferDesc desc) : m_desc(desc)
    {
        m_storage.resize(static_cast<crd::usize>(desc.size_bytes));
    }
    [[nodiscard]] const crd::rhi::BufferDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] void* map() noexcept override { return m_storage.data(); }
    void unmap() noexcept override {}

private:
    crd::rhi::BufferDesc m_desc{};
    crd::containers::Array<crd::u8> m_storage{};
};

class FakeShaderModule final : public crd::rhi::ShaderModule
{
public:
    explicit FakeShaderModule(crd::rhi::ShaderModuleDesc desc) : m_stage(desc.stage), m_entry_point(desc.entry_point) {}
    [[nodiscard]] crd::rhi::ShaderStage stage() const noexcept override { return m_stage; }
    [[nodiscard]] crd::containers::StringView entry_point() const noexcept override { return m_entry_point; }

private:
    crd::rhi::ShaderStage m_stage = crd::rhi::ShaderStage::Vertex;
    crd::containers::String m_entry_point{};
};

class FakePipeline final : public crd::rhi::Pipeline
{
public:
    explicit FakePipeline(crd::rhi::GraphicsPipelineDesc desc) : m_desc(desc) {}
    [[nodiscard]] const crd::rhi::GraphicsPipelineDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::GraphicsPipelineDesc m_desc{};
};

class FakeCommandBuffer final : public crd::rhi::CommandBuffer
{
public:
    void begin() override { ++begin_count; }
    void end() override { ++end_count; }
    void reset() override { ++reset_count; }
    void begin_rendering(const crd::rhi::RenderingInfo& info) override
    {
        ++begin_rendering_count;
        last_extent = info.extent;
    }
    void end_rendering() override { ++end_rendering_count; }
    void bind_pipeline(crd::rhi::Pipeline& /*pipeline*/) override { ++bind_pipeline_count; }
    void bind_vertex_buffer(crd::rhi::Buffer& /*buffer*/, crd::u64 /*offset_bytes*/) override
    {
        ++bind_vertex_buffer_count;
    }
    void draw(crd::u32 vertex_count, crd::u32 first_vertex) override
    {
        ++draw_count;
        last_vertex_count = vertex_count;
        last_first_vertex = first_vertex;
    }

    int begin_count = 0;
    int end_count = 0;
    int reset_count = 0;
    int begin_rendering_count = 0;
    int end_rendering_count = 0;
    int bind_pipeline_count = 0;
    int bind_vertex_buffer_count = 0;
    int draw_count = 0;
    crd::rhi::Extent2D last_extent{};
    crd::u32 last_vertex_count = 0;
    crd::u32 last_first_vertex = 0;
};

class FakeSwapchain final : public crd::rhi::Swapchain
{
public:
    explicit FakeSwapchain(crd::rhi::SwapchainDesc desc)
        : m_desc(desc), m_image(crd::rhi::ImageDesc{desc.extent, desc.color_format,
                                                    crd::rhi::enum_bits(crd::rhi::ImageUsage::ColorAttachment), 1, 1})
    {
    }

    [[nodiscard]] const crd::rhi::SwapchainDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] bool acquire_next_image() override
    {
        ++acquire_count;
        return true;
    }
    [[nodiscard]] crd::u32 current_image_index() const noexcept override { return 0; }
    [[nodiscard]] crd::rhi::Image& current_image() noexcept override { return m_image; }

    int acquire_count = 0;

private:
    crd::rhi::SwapchainDesc m_desc{};
    FakeImage m_image;
};

class FakeQueue final : public crd::rhi::Queue
{
public:
    [[nodiscard]] bool submit(crd::rhi::CommandBuffer& /*command_buffer*/, crd::rhi::Swapchain& /*swapchain*/) override
    {
        ++submit_count;
        return true;
    }
    void present(crd::rhi::Swapchain& /*swapchain*/) override { ++present_count; }
    void wait_idle() override { ++wait_idle_count; }

    int submit_count = 0;
    int present_count = 0;
    int wait_idle_count = 0;
};

class FakeDevice final : public crd::rhi::Device
{
public:
    [[nodiscard]] crd::rhi::BackendApi api() const noexcept override { return crd::rhi::BackendApi::Vulkan; }

    [[nodiscard]] std::unique_ptr<crd::rhi::Swapchain> create_swapchain(const crd::rhi::SwapchainDesc& desc) override
    {
        ++create_swapchain_count;
        return std::make_unique<FakeSwapchain>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Buffer> create_buffer(const crd::rhi::BufferDesc& desc) override
    {
        ++create_buffer_count;
        return std::make_unique<FakeBuffer>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Image> create_image(const crd::rhi::ImageDesc& desc) override
    {
        ++create_image_count;
        return std::make_unique<FakeImage>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::ShaderModule>
    create_shader_module(const crd::rhi::ShaderModuleDesc& desc) override
    {
        ++create_shader_module_count;
        return std::make_unique<FakeShaderModule>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Pipeline>
    create_graphics_pipeline(const crd::rhi::GraphicsPipelineDesc& desc) override
    {
        ++create_pipeline_count;
        return std::make_unique<FakePipeline>(desc);
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::CommandBuffer> create_command_buffer() override
    {
        ++create_command_buffer_count;
        return std::make_unique<FakeCommandBuffer>();
    }

    [[nodiscard]] crd::rhi::Queue& graphics_queue() noexcept override { return m_queue; }
    void wait_idle() override { ++wait_idle_count; }

    int create_swapchain_count = 0;
    int create_buffer_count = 0;
    int create_image_count = 0;
    int create_shader_module_count = 0;
    int create_pipeline_count = 0;
    int create_command_buffer_count = 0;
    int wait_idle_count = 0;
    FakeQueue m_queue{};
};

class FakeInstance final : public crd::rhi::Instance
{
public:
    [[nodiscard]] crd::rhi::BackendApi api() const noexcept override { return crd::rhi::BackendApi::Vulkan; }

    void enumerate_adapters(crd::containers::Array<crd::rhi::AdapterInfo>& out) const override
    {
        out.push_back(crd::rhi::AdapterInfo{crd::containers::String("Fake GPU"), crd::rhi::AdapterType::DiscreteGpu,
                                            8ull * 1024ull * 1024ull * 1024ull, true, true});
    }

    [[nodiscard]] std::unique_ptr<crd::rhi::Device> create_device(const crd::rhi::DeviceDesc& /*desc*/) override
    {
        return std::make_unique<FakeDevice>();
    }
};
} // namespace

TEST_CASE("RHI flags compose cleanly", "[rhi][types]")
{
    const crd::u32 usage = crd::rhi::BufferUsage::Vertex | crd::rhi::BufferUsage::TransferDst;
    REQUIRE(crd::rhi::has_flag(usage, crd::rhi::BufferUsage::Vertex));
    REQUIRE(crd::rhi::has_flag(usage, crd::rhi::BufferUsage::TransferDst));
    REQUIRE_FALSE(crd::rhi::has_flag(usage, crd::rhi::BufferUsage::Uniform));
}

TEST_CASE("RHI instance enumerates adapters and creates a device", "[rhi][instance]")
{
    FakeInstance instance;
    crd::containers::Array<crd::rhi::AdapterInfo> adapters;
    instance.enumerate_adapters(adapters);
    REQUIRE(adapters.size() == 1u);
    REQUIRE(adapters[0].name == "Fake GPU");

    auto device = instance.create_device({});
    REQUIRE(device != nullptr);
    REQUIRE(device->api() == crd::rhi::BackendApi::Vulkan);
}

TEST_CASE("RHI device can express the first-triangle resource flow", "[rhi][device]")
{
    FakeDevice device;

    crd::u8 shader_code[] = {0x03, 0x02, 0x23, 0x07};
    auto vs =
        device.create_shader_module({crd::rhi::ShaderStage::Vertex, "main", crd::containers::make_span(shader_code)});
    auto fs =
        device.create_shader_module({crd::rhi::ShaderStage::Fragment, "main", crd::containers::make_span(shader_code)});
    auto vb = device.create_buffer(
        {sizeof(float) * 18u, static_cast<crd::u32>(crd::rhi::BufferUsage::Vertex | crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::CpuToGpu});

    crd::rhi::VertexBindingDesc binding{0, sizeof(float) * 6u, crd::rhi::VertexInputRate::Vertex};
    crd::rhi::VertexAttributeDesc attrs[] = {{0, 0, crd::rhi::Format::R8G8B8A8Unorm, 0},
                                             {1, 0, crd::rhi::Format::R8G8B8A8Unorm, 16}};
    auto pipeline = device.create_graphics_pipeline({vs.get(),
                                                     fs.get(),
                                                     crd::rhi::PrimitiveTopology::TriangleList,
                                                     {1280, 720},
                                                     crd::rhi::Format::B8G8R8A8Unorm,
                                                     crd::rhi::Format::Undefined,
                                                     crd::containers::make_span(&binding, 1),
                                                     crd::containers::make_span(attrs),
                                                     false,
                                                     false});

    auto command_buffer = device.create_command_buffer();
    auto* cb = static_cast<FakeCommandBuffer*>(command_buffer.get());
    auto swapchain = device.create_swapchain(
        {nullptr, {1280, 720}, crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::PresentMode::Fifo, 2});
    REQUIRE(swapchain != nullptr);
    REQUIRE(swapchain->acquire_next_image());

    command_buffer->begin();
    command_buffer->begin_rendering(
        {{1280, 720}, {nullptr, crd::rhi::LoadOp::Clear, crd::rhi::StoreOp::Store, {0.1f, 0.2f, 0.3f, 1.0f}}, nullptr});
    command_buffer->bind_pipeline(*pipeline);
    command_buffer->bind_vertex_buffer(*vb, 0);
    command_buffer->draw(3, 0);
    command_buffer->end_rendering();
    command_buffer->end();

    REQUIRE(device.graphics_queue().submit(*command_buffer, *swapchain));
    device.graphics_queue().present(*swapchain);

    REQUIRE(device.create_shader_module_count == 2);
    REQUIRE(device.create_buffer_count == 1);
    REQUIRE(device.create_pipeline_count == 1);
    REQUIRE(device.create_command_buffer_count == 1);
    REQUIRE(cb->begin_count == 1);
    REQUIRE(cb->begin_rendering_count == 1);
    REQUIRE(cb->bind_pipeline_count == 1);
    REQUIRE(cb->bind_vertex_buffer_count == 1);
    REQUIRE(cb->draw_count == 1);
    REQUIRE(cb->last_vertex_count == 3u);
    REQUIRE(device.m_queue.submit_count == 1);
    REQUIRE(device.m_queue.present_count == 1);
}
