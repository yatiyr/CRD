#include <crd/log/log.hpp>
#include <crd/rhi/rhi.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_rhi, "SmokeRHI", crd::log::LogLevel::Trace)

namespace
{
class SmokeImage final : public crd::rhi::Image
{
public:
    explicit SmokeImage(crd::rhi::ImageDesc desc) : m_desc(desc) {}
    [[nodiscard]] const crd::rhi::ImageDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::ImageDesc m_desc{};
};

class SmokeBuffer final : public crd::rhi::Buffer
{
public:
    explicit SmokeBuffer(crd::rhi::BufferDesc desc) : m_desc(desc)
    {
        m_bytes.resize(static_cast<crd::usize>(desc.size_bytes));
    }
    [[nodiscard]] const crd::rhi::BufferDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] void* map() noexcept override { return m_bytes.data(); }
    void unmap() noexcept override {}

private:
    crd::rhi::BufferDesc m_desc{};
    crd::containers::Array<crd::u8> m_bytes{};
};

class SmokeShaderModule final : public crd::rhi::ShaderModule
{
public:
    explicit SmokeShaderModule(crd::rhi::ShaderModuleDesc desc) : m_stage(desc.stage), m_entry(desc.entry_point) {}
    [[nodiscard]] crd::rhi::ShaderStage stage() const noexcept override { return m_stage; }
    [[nodiscard]] crd::containers::StringView entry_point() const noexcept override { return m_entry; }

private:
    crd::rhi::ShaderStage m_stage = crd::rhi::ShaderStage::Vertex;
    crd::containers::String m_entry{};
};

class SmokePipeline final : public crd::rhi::Pipeline
{
public:
    explicit SmokePipeline(crd::rhi::GraphicsPipelineDesc desc) : m_desc(desc) {}
    [[nodiscard]] const crd::rhi::GraphicsPipelineDesc& desc() const noexcept override { return m_desc; }

private:
    crd::rhi::GraphicsPipelineDesc m_desc{};
};

class SmokeCommandBuffer final : public crd::rhi::CommandBuffer
{
public:
    void begin() override { CRD_LOG_INFO(g_log_smoke_rhi, "CommandBuffer::begin"); }
    void end() override { CRD_LOG_INFO(g_log_smoke_rhi, "CommandBuffer::end"); }
    void reset() override {}
    void begin_rendering(const crd::rhi::RenderingInfo& info) override
    {
        CRD_LOG_INFO(g_log_smoke_rhi, "begin_rendering {}x{}", info.extent.width, info.extent.height);
    }
    void end_rendering() override { CRD_LOG_INFO(g_log_smoke_rhi, "end_rendering"); }
    void bind_pipeline(crd::rhi::Pipeline& /*pipeline*/) override { CRD_LOG_INFO(g_log_smoke_rhi, "bind_pipeline"); }
    void bind_vertex_buffer(crd::rhi::Buffer& /*buffer*/, crd::u64 /*offset_bytes*/) override
    {
        CRD_LOG_INFO(g_log_smoke_rhi, "bind_vertex_buffer");
    }
    void draw(crd::u32 vertex_count, crd::u32 first_vertex) override
    {
        CRD_LOG_INFO(g_log_smoke_rhi, "draw vertices={} first={}", vertex_count, first_vertex);
    }
};

class SmokeSwapchain final : public crd::rhi::Swapchain
{
public:
    explicit SmokeSwapchain(crd::rhi::SwapchainDesc desc)
        : m_desc(desc), m_image(crd::rhi::ImageDesc{desc.extent, desc.color_format,
                                                    crd::rhi::enum_bits(crd::rhi::ImageUsage::ColorAttachment), 1, 1})
    {
    }
    [[nodiscard]] const crd::rhi::SwapchainDesc& desc() const noexcept override { return m_desc; }
    void acquire_next_image() override { CRD_LOG_INFO(g_log_smoke_rhi, "acquire_next_image"); }
    [[nodiscard]] crd::u32 current_image_index() const noexcept override { return 0; }
    [[nodiscard]] crd::rhi::Image& current_image() noexcept override { return m_image; }

private:
    crd::rhi::SwapchainDesc m_desc{};
    SmokeImage m_image;
};

class SmokeQueue final : public crd::rhi::Queue
{
public:
    void submit(crd::rhi::CommandBuffer& /*command_buffer*/) override { CRD_LOG_INFO(g_log_smoke_rhi, "queue submit"); }
    void present(crd::rhi::Swapchain& /*swapchain*/) override { CRD_LOG_INFO(g_log_smoke_rhi, "queue present"); }
    void wait_idle() override { CRD_LOG_INFO(g_log_smoke_rhi, "queue wait_idle"); }
};

class SmokeDevice final : public crd::rhi::Device
{
public:
    [[nodiscard]] crd::rhi::BackendApi api() const noexcept override { return crd::rhi::BackendApi::Vulkan; }
    [[nodiscard]] std::unique_ptr<crd::rhi::Swapchain> create_swapchain(const crd::rhi::SwapchainDesc& desc) override
    {
        return std::make_unique<SmokeSwapchain>(desc);
    }
    [[nodiscard]] std::unique_ptr<crd::rhi::Buffer> create_buffer(const crd::rhi::BufferDesc& desc) override
    {
        return std::make_unique<SmokeBuffer>(desc);
    }
    [[nodiscard]] std::unique_ptr<crd::rhi::Image> create_image(const crd::rhi::ImageDesc& desc) override
    {
        return std::make_unique<SmokeImage>(desc);
    }
    [[nodiscard]] std::unique_ptr<crd::rhi::ShaderModule>
    create_shader_module(const crd::rhi::ShaderModuleDesc& desc) override
    {
        return std::make_unique<SmokeShaderModule>(desc);
    }
    [[nodiscard]] std::unique_ptr<crd::rhi::Pipeline>
    create_graphics_pipeline(const crd::rhi::GraphicsPipelineDesc& desc) override
    {
        return std::make_unique<SmokePipeline>(desc);
    }
    [[nodiscard]] std::unique_ptr<crd::rhi::CommandBuffer> create_command_buffer() override
    {
        return std::make_unique<SmokeCommandBuffer>();
    }
    [[nodiscard]] crd::rhi::Queue& graphics_queue() noexcept override { return m_queue; }
    void wait_idle() override { m_queue.wait_idle(); }

private:
    SmokeQueue m_queue{};
};
} // namespace

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    SmokeDevice device;
    crd::u8 shader_code[] = {0x03, 0x02, 0x23, 0x07};

    auto vs =
        device.create_shader_module({crd::rhi::ShaderStage::Vertex, "main", crd::containers::make_span(shader_code)});
    auto fs =
        device.create_shader_module({crd::rhi::ShaderStage::Fragment, "main", crd::containers::make_span(shader_code)});
    auto vb =
        device.create_buffer({sizeof(float) * 18u, crd::rhi::BufferUsage::Vertex | crd::rhi::BufferUsage::TransferDst,
                              crd::rhi::MemoryUsage::CpuToGpu});
    crd::rhi::VertexBindingDesc binding{0, sizeof(float) * 6u, crd::rhi::VertexInputRate::Vertex};
    crd::rhi::VertexAttributeDesc attrs[] = {{0, 0, crd::rhi::Format::R8G8B8A8Unorm, 0},
                                             {1, 0, crd::rhi::Format::R8G8B8A8Unorm, 16}};
    auto pipeline = device.create_graphics_pipeline({vs.get(), fs.get(), crd::rhi::PrimitiveTopology::TriangleList,
                                                     crd::rhi::Format::B8G8R8A8Unorm, crd::rhi::Format::Undefined,
                                                     crd::containers::make_span(&binding, 1),
                                                     crd::containers::make_span(attrs), false, false});
    auto cb = device.create_command_buffer();

    cb->begin();
    cb->begin_rendering(
        {{1280, 720}, {nullptr, crd::rhi::LoadOp::Clear, crd::rhi::StoreOp::Store, {0.1f, 0.1f, 0.2f, 1.0f}}, nullptr});
    cb->bind_pipeline(*pipeline);
    cb->bind_vertex_buffer(*vb, 0);
    cb->draw(3, 0);
    cb->end_rendering();
    cb->end();
    device.graphics_queue().submit(*cb);

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
